// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_feedback_outbox.h"

#include <objbase.h>

#include <algorithm>
#include <climits>
#include <functional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "base/values.h"
#include "client/phi_account_storage.h"
#include "client/phi_auth.h"
#include "client/phi_feedback_image.h"
#include "client/phi_feedback_zip.h"
#include "client/phi_http.h"
#include "client/phi_i18n.h"
#include "client/phi_version.h"

#pragma comment(lib, "version.lib")

namespace phi_client {

namespace {

constexpr wchar_t kManifestFilename[] = L"manifest.json";
constexpr char kSystemLogsFilename[] = "system_logs.txt";
constexpr char kPrimaryLogsZipFilename[] = "logs.zip";
// Chromium's own log in the user data dir (written under --enable-logging);
// the Windows stand-in for the Mac's PhiLogs directory.
constexpr wchar_t kChromiumLogFilename[] = L"chrome_debug.log";
// The Mac's phiExtensionIds: the extensions whose versions ride along.
constexpr const char* kPhiExtensionIds[] = {
    "pjlnhbfabokjejbhmgghmjiaknfhnima", "pjgdkljlcbjgedgeppodjijjphfcplno",
    "fenmfiepnpdlhplemgijlimpbebebljo", "ickhcgejficcoofnjnnobadfdnfbilnm"};
// The worker re-scans this often while some job is waiting on its backoff
// (the Mac's scheduleCurrentAccountProcessing(after: 60)).
constexpr DWORD kRetryScanMs = 60 * 1000;

// --- Small file helpers (raw Win32: the worker is a plain thread and the
// UI thread disallows base's blocking file API) -----------------------------

int64_t NowMs() {
  FILETIME ft = {};
  ::GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER v = {};
  v.LowPart = ft.dwLowDateTime;
  v.HighPart = ft.dwHighDateTime;
  return static_cast<int64_t>(v.QuadPart / 10000ULL) - 11644473600000LL;
}

std::wstring Join(const std::wstring& dir, const std::wstring& leaf) {
  if (dir.empty() || dir.back() == L'\\') {
    return dir + leaf;
  }
  return dir + L"\\" + leaf;
}

bool PathExists(const std::wstring& path) {
  return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(const std::wstring& path) {
  const DWORD attributes = ::GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

int64_t FileSize(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
    return 0;
  }
  return (static_cast<int64_t>(data.nFileSizeHigh) << 32) |
         data.nFileSizeLow;
}

bool CreateDirectories(const std::wstring& path) {
  if (IsDirectory(path)) {
    return true;
  }
  const size_t slash = path.find_last_of(L'\\');
  if (slash != std::wstring::npos && slash > 0) {
    const std::wstring parent = path.substr(0, slash);
    if (parent.size() > 2 && !CreateDirectories(parent)) {
      return false;
    }
  }
  return ::CreateDirectoryW(path.c_str(), nullptr) ||
         ::GetLastError() == ERROR_ALREADY_EXISTS;
}

bool ReadFileBytes(const std::wstring& path, std::string* out) {
  HANDLE f = ::CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return false;
  }
  out->clear();
  std::string chunk(64 * 1024, '\0');
  DWORD read = 0;
  while (::ReadFile(f, chunk.data(), static_cast<DWORD>(chunk.size()), &read,
                    nullptr) &&
         read > 0) {
    out->append(chunk.data(), read);
  }
  ::CloseHandle(f);
  return true;
}

// Atomic replace (the phi_tab_store / telemetry pattern).
bool WriteFileAtomic(const std::wstring& path, const std::string& contents) {
  const std::wstring temp = path + L".tmp";
  HANDLE f = ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  const BOOL ok = ::WriteFile(f, contents.data(),
                              static_cast<DWORD>(contents.size()), &written,
                              nullptr);
  ::CloseHandle(f);
  if (!ok || written != contents.size()) {
    ::DeleteFileW(temp.c_str());
    return false;
  }
  return ::MoveFileExW(temp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
         0;
}

// Every entry of `dir` (no recursion), name only.
std::vector<std::wstring> ListDirectory(const std::wstring& dir,
                                        bool directories) {
  std::vector<std::wstring> names;
  WIN32_FIND_DATAW data = {};
  HANDLE find = ::FindFirstFileW(Join(dir, L"*").c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) {
    return names;
  }
  do {
    const std::wstring name = data.cFileName;
    if (name == L"." || name == L"..") {
      continue;
    }
    const bool is_dir = data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    if (is_dir == directories) {
      names.push_back(name);
    }
  } while (::FindNextFileW(find, &data));
  ::FindClose(find);
  std::sort(names.begin(), names.end());
  return names;
}

void RemoveTree(const std::wstring& path) {
  if (!IsDirectory(path)) {
    ::DeleteFileW(path.c_str());
    return;
  }
  for (const std::wstring& name : ListDirectory(path, true)) {
    RemoveTree(Join(path, name));
  }
  for (const std::wstring& name : ListDirectory(path, false)) {
    ::DeleteFileW(Join(path, name).c_str());
  }
  ::RemoveDirectoryW(path.c_str());
}

std::wstring Widen(const std::string& utf8) {
  return base::UTF8ToWide(utf8);
}
std::string Narrow(const std::wstring& wide) {
  return base::WideToUTF8(wide);
}

// A relative path inside a job ("images/shot.png") to its absolute form.
std::wstring Resolve(const std::wstring& job_root,
                     const std::string& relative) {
  std::wstring rel = Widen(relative);
  std::replace(rel.begin(), rel.end(), L'/', L'\\');
  return Join(job_root, rel);
}

std::string Trim(const std::string& in) {
  const size_t begin = in.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return std::string();
  }
  const size_t end = in.find_last_not_of(" \t\r\n");
  return in.substr(begin, end - begin + 1);
}

std::wstring Lower(std::wstring s) {
  for (wchar_t& c : s) {
    c = static_cast<wchar_t>(::towlower(c));
  }
  return s;
}

std::wstring Extension(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  const size_t dot = path.find_last_of(L'.');
  if (dot == std::wstring::npos ||
      (slash != std::wstring::npos && dot < slash)) {
    return std::wstring();
  }
  return Lower(path.substr(dot));
}

std::wstring Leaf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// The Mac's sanitizedFilename, plus the characters Windows refuses.
std::wstring SanitizedFilename(const std::wstring& filename) {
  std::wstring cleaned;
  for (wchar_t c : filename) {
    cleaned += (c == L'/' || c == L':' || c == L'\\' || c == L'<' ||
                c == L'>' || c == L'"' || c == L'|' || c == L'?' ||
                c == L'*' || c < 32)
                   ? L'-'
                   : c;
  }
  const size_t begin = cleaned.find_first_not_of(L" \t\r\n");
  const size_t end = cleaned.find_last_not_of(L" \t\r\n.");
  if (begin == std::wstring::npos || end == std::wstring::npos) {
    return L"attachment";
  }
  return cleaned.substr(begin, end - begin + 1);
}

std::wstring UniqueFilename(const std::wstring& original,
                            const std::wstring& directory) {
  const std::wstring sanitized = SanitizedFilename(original);
  const std::wstring ext = Extension(sanitized);
  const std::wstring base = sanitized.substr(0, sanitized.size() - ext.size());
  std::wstring filename = sanitized;
  int index = 2;
  while (PathExists(Join(directory, filename))) {
    filename = base + L"-" + std::to_wstring(index) + ext;
    ++index;
  }
  return filename;
}

std::wstring ReplaceExtension(const std::wstring& filename,
                              const std::wstring& new_ext) {
  const std::wstring ext = Extension(filename);
  return filename.substr(0, filename.size() - ext.size()) + new_ext;
}

// --- The manifest --------------------------------------------------------

struct SourceAttachment {
  std::string relative_path;
  std::string filename;
  std::string mime_type;
  int64_t size = 0;
};

struct UploadAttachment {
  std::string id;
  std::string relative_path;
  std::string filename;
  std::string mime_type;
  int64_t size = 0;
  std::string attachment_type;  // screenshot | log | other
  bool required = false;
  std::string status;  // queued | uploaded | failed
  int retry_count = 0;
  std::string object_key;
};

struct Manifest {
  std::string id;
  int64_t created_at_ms = 0;
  std::string description;
  std::string contact_email;
  base::DictValue metadata;
  std::vector<SourceAttachment> source_images;
  std::vector<SourceAttachment> source_files;
  std::optional<SourceAttachment> chromium_system_logs;
  std::vector<UploadAttachment> prepared;
  int archive_strategy_version = 0;
  std::string status;  // queued | preparing | uploading | submitting |
                       // submitted | failedWaitingRetry
  int retry_count = 0;
  int64_t next_attempt_at_ms = 0;
  std::string last_error;
};

base::DictValue SourceToDict(const SourceAttachment& source) {
  base::DictValue d;
  d.Set("relativePath", source.relative_path);
  d.Set("filename", source.filename);
  d.Set("mimeType", source.mime_type);
  d.Set("size", static_cast<double>(source.size));
  return d;
}

std::optional<SourceAttachment> SourceFromDict(const base::DictValue* d) {
  if (!d) {
    return std::nullopt;
  }
  SourceAttachment source;
  const auto str = [&](const char* key) {
    const std::string* value = d->FindString(key);
    return value ? *value : std::string();
  };
  source.relative_path = str("relativePath");
  source.filename = str("filename");
  source.mime_type = str("mimeType");
  source.size = static_cast<int64_t>(d->FindDouble("size").value_or(0));
  if (source.relative_path.empty() || source.filename.empty()) {
    return std::nullopt;
  }
  return source;
}

base::DictValue UploadToDict(const UploadAttachment& a) {
  base::DictValue d;
  d.Set("id", a.id);
  d.Set("relativePath", a.relative_path);
  d.Set("filename", a.filename);
  d.Set("mimeType", a.mime_type);
  d.Set("size", static_cast<double>(a.size));
  d.Set("attachmentType", a.attachment_type);
  d.Set("required", a.required);
  d.Set("status", a.status);
  d.Set("retryCount", a.retry_count);
  if (!a.object_key.empty()) {
    d.Set("objectKey", a.object_key);
  }
  return d;
}

std::optional<UploadAttachment> UploadFromDict(const base::DictValue* d) {
  if (!d) {
    return std::nullopt;
  }
  UploadAttachment a;
  const auto str = [&](const char* key) {
    const std::string* value = d->FindString(key);
    return value ? *value : std::string();
  };
  a.id = str("id");
  a.relative_path = str("relativePath");
  a.filename = str("filename");
  a.mime_type = str("mimeType");
  a.size = static_cast<int64_t>(d->FindDouble("size").value_or(0));
  a.attachment_type = str("attachmentType");
  a.required = d->FindBool("required").value_or(false);
  a.status = str("status");
  a.retry_count = d->FindInt("retryCount").value_or(0);
  a.object_key = str("objectKey");
  if (a.relative_path.empty() || a.filename.empty()) {
    return std::nullopt;
  }
  return a;
}

std::wstring ManifestPath(const std::wstring& job_root) {
  return Join(job_root, kManifestFilename);
}

bool WriteManifest(const Manifest& m, const std::wstring& job_root) {
  base::DictValue root;
  root.Set("id", m.id);
  root.Set("createdAtMs", static_cast<double>(m.created_at_ms));
  root.Set("description", m.description);
  if (!m.contact_email.empty()) {
    root.Set("contactEmail", m.contact_email);
  }
  root.Set("metadata", m.metadata.Clone());
  base::ListValue images;
  for (const SourceAttachment& s : m.source_images) {
    images.Append(SourceToDict(s));
  }
  root.Set("sourceImages", std::move(images));
  base::ListValue files;
  for (const SourceAttachment& s : m.source_files) {
    files.Append(SourceToDict(s));
  }
  root.Set("sourceFiles", std::move(files));
  if (m.chromium_system_logs) {
    root.Set("chromiumSystemLogs", SourceToDict(*m.chromium_system_logs));
  }
  base::ListValue prepared;
  for (const UploadAttachment& a : m.prepared) {
    prepared.Append(UploadToDict(a));
  }
  root.Set("preparedAttachments", std::move(prepared));
  root.Set("archiveStrategyVersion", m.archive_strategy_version);
  root.Set("status", m.status);
  root.Set("retryCount", m.retry_count);
  if (m.next_attempt_at_ms) {
    root.Set("nextAttemptAtMs", static_cast<double>(m.next_attempt_at_ms));
  }
  if (!m.last_error.empty()) {
    root.Set("lastError", m.last_error);
  }
  std::optional<std::string> json = base::WriteJsonWithOptions(
      root, base::JSONWriter::OPTIONS_PRETTY_PRINT);
  return json && WriteFileAtomic(ManifestPath(job_root), *json);
}

std::optional<Manifest> ReadManifest(const std::wstring& job_root) {
  std::string text;
  if (!ReadFileBytes(ManifestPath(job_root), &text)) {
    return std::nullopt;
  }
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(text, base::JSON_PARSE_RFC);
  if (!root) {
    return std::nullopt;
  }
  Manifest m;
  const auto str = [&](const char* key) {
    const std::string* value = root->FindString(key);
    return value ? *value : std::string();
  };
  m.id = str("id");
  m.created_at_ms =
      static_cast<int64_t>(root->FindDouble("createdAtMs").value_or(0));
  m.description = str("description");
  m.contact_email = str("contactEmail");
  if (const base::DictValue* metadata = root->FindDict("metadata")) {
    m.metadata = metadata->Clone();
  }
  if (const base::ListValue* list = root->FindList("sourceImages")) {
    for (const base::Value& v : *list) {
      if (std::optional<SourceAttachment> s = SourceFromDict(v.GetIfDict())) {
        m.source_images.push_back(*s);
      }
    }
  }
  if (const base::ListValue* list = root->FindList("sourceFiles")) {
    for (const base::Value& v : *list) {
      if (std::optional<SourceAttachment> s = SourceFromDict(v.GetIfDict())) {
        m.source_files.push_back(*s);
      }
    }
  }
  m.chromium_system_logs = SourceFromDict(root->FindDict("chromiumSystemLogs"));
  if (const base::ListValue* list = root->FindList("preparedAttachments")) {
    for (const base::Value& v : *list) {
      if (std::optional<UploadAttachment> a = UploadFromDict(v.GetIfDict())) {
        m.prepared.push_back(*a);
      }
    }
  }
  m.archive_strategy_version =
      root->FindInt("archiveStrategyVersion").value_or(0);
  m.status = str("status");
  m.retry_count = root->FindInt("retryCount").value_or(0);
  m.next_attempt_at_ms =
      static_cast<int64_t>(root->FindDouble("nextAttemptAtMs").value_or(0));
  m.last_error = str("lastError");
  if (m.id.empty() || m.status.empty()) {
    return std::nullopt;
  }
  return m;
}

// --- Metadata (the Mac's makeMetadata) -----------------------------------

// The framework's version: chrome.dll's file version, the browser this
// client is hosting (the Mac's buildNumber slot).
std::string ChromiumVersion() {
  HMODULE module = ::GetModuleHandleW(L"chrome.dll");
  if (!module) {
    return std::string();
  }
  std::wstring path(MAX_PATH, L'\0');
  const DWORD length = ::GetModuleFileNameW(
      module, path.data(), static_cast<DWORD>(path.size()));
  if (!length || length >= path.size()) {
    return std::string();
  }
  path.resize(length);
  DWORD handle = 0;
  const DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &handle);
  if (!size) {
    return std::string();
  }
  std::string buffer(size, '\0');
  if (!::GetFileVersionInfoW(path.c_str(), 0, size, buffer.data())) {
    return std::string();
  }
  VS_FIXEDFILEINFO* info = nullptr;
  UINT info_size = 0;
  if (!::VerQueryValueW(buffer.data(), L"\\",
                        reinterpret_cast<void**>(&info), &info_size) ||
      !info) {
    return std::string();
  }
  return std::to_string(HIWORD(info->dwFileVersionMS)) + "." +
         std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
         std::to_string(HIWORD(info->dwFileVersionLS)) + "." +
         std::to_string(LOWORD(info->dwFileVersionLS));
}

std::string RegistryString(HKEY root, const wchar_t* key, const wchar_t* name) {
  std::wstring value(256, L'\0');
  DWORD size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
  DWORD type = 0;
  if (::RegGetValueW(root, key, name, RRF_RT_REG_SZ, &type, value.data(),
                     &size) != ERROR_SUCCESS ||
      size < sizeof(wchar_t)) {
    return std::string();
  }
  value.resize(size / sizeof(wchar_t) - 1);
  return Narrow(value);
}

// "Windows 10.0.22631 (23H2)" — the Mac's osVersionString slot.
std::string OsVersionString() {
  const wchar_t kKey[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
  std::string version =
      RegistryString(HKEY_LOCAL_MACHINE, kKey, L"CurrentMajorVersionNumber");
  // CurrentMajorVersionNumber is a DWORD; RtlGetVersion is the honest source.
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  RTL_OSVERSIONINFOW info = {};
  info.dwOSVersionInfoSize = sizeof(info);
  if (HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll")) {
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(
            ::GetProcAddress(ntdll, "RtlGetVersion"))) {
      fn(&info);
    }
  }
  version = "Windows " + std::to_string(info.dwMajorVersion) + "." +
            std::to_string(info.dwMinorVersion) + "." +
            std::to_string(info.dwBuildNumber);
  const std::string display =
      RegistryString(HKEY_LOCAL_MACHINE, kKey, L"DisplayVersion");
  if (!display.empty()) {
    version += " (" + display + ")";
  }
  return version;
}

// "LENOVO 21CB000BUS" — the Mac's modelIdentifier slot, from SMBIOS.
std::string ModelIdentifier() {
  const wchar_t kKey[] = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
  const std::string manufacturer =
      Trim(RegistryString(HKEY_LOCAL_MACHINE, kKey, L"SystemManufacturer"));
  const std::string product =
      Trim(RegistryString(HKEY_LOCAL_MACHINE, kKey, L"SystemProductName"));
  if (manufacturer.empty()) {
    return product;
  }
  return product.empty() ? manufacturer : manufacturer + " " + product;
}

base::DictValue MakeMetadata(const std::string& job_id,
                             const PhiFeedbackDraft& draft) {
  const std::string chromium = ChromiumVersion();
  base::DictValue browser;
  browser.Set("name", "Phi Browser");
  browser.Set("version", PHI_WIN_VERSION_FULL_STRING);
  browser.Set("channel", PhiFeedbackOutbox::BrowserChannelName());
  browser.Set("revision", chromium);

  base::DictValue page;
  if (!draft.page_url.empty()) {
    page.Set("url", draft.page_url);
  }
  if (!draft.page_title.empty()) {
    page.Set("title", draft.page_title);
  }

  base::DictValue context;
  context.Set("category", "issue-report");
  context.Set("locale", PhiActiveLocale());
  context.Set("trace_id", job_id);

  // The Mac's feedbackComponents: Phi's extensions, sorted by id (their
  // names double as ids there), capped at 100. Sentinel is Mac-only.
  std::vector<std::pair<std::string, std::string>> extensions(
      draft.extension_versions.begin(), draft.extension_versions.end());
  std::sort(extensions.begin(), extensions.end(),
            [](const auto& a, const auto& b) {
              return ::_stricmp(a.first.c_str(), b.first.c_str()) < 0;
            });
  base::ListValue components;
  for (const auto& [name, version] : extensions) {
    if (components.size() >= 100) {
      break;
    }
    base::DictValue component;
    component.Set("id", name);
    component.Set("name", name);
    component.Set("type", "extension");
    component.Set("version", version);
    components.Append(std::move(component));
  }

  base::DictValue extra;
  extra.Set("build_number", chromium);
  extra.Set("model_identifier", ModelIdentifier());
  extra.Set("os_version", OsVersionString());
  for (const auto& [key, value] : draft.sync_status) {
    extra.Set(key, value);
  }

  base::DictValue metadata;
  metadata.Set("browser", std::move(browser));
  metadata.Set("page", std::move(page));
  metadata.Set("client_context", std::move(context));
  metadata.Set("components", std::move(components));
  metadata.Set("extra", std::move(extra));
  return metadata;
}

// --- Attachment preparation (the Mac's prepareAttachments) ----------------

struct PreparedFile {
  std::wstring path;
  std::string filename;
  std::string mime_type;
  int64_t size = 0;
};

std::string RelativeTo(const std::wstring& path, const std::wstring& root) {
  std::wstring relative = path;
  if (relative.size() > root.size() &&
      Lower(relative.substr(0, root.size())) == Lower(root)) {
    relative = relative.substr(root.size());
    if (!relative.empty() && relative.front() == L'\\') {
      relative.erase(0, 1);
    }
  } else {
    relative = Leaf(path);
  }
  std::replace(relative.begin(), relative.end(), L'\\', L'/');
  return Narrow(relative);
}

UploadAttachment MakeUpload(const std::wstring& path,
                            const std::wstring& job_root,
                            const std::string& filename,
                            const std::string& mime_type,
                            int64_t size,
                            const char* type,
                            bool required) {
  UploadAttachment a;
  a.id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  a.relative_path = RelativeTo(path, job_root);
  a.filename = filename;
  a.mime_type = mime_type;
  a.size = size;
  a.attachment_type = type;
  a.required = required;
  a.status = "queued";
  return a;
}

PhiArchiveItem InlineItem(const std::string& text,
                          const std::string& archive_path) {
  PhiArchiveItem item;
  item.inline_data = text;
  item.length = text.size();
  item.archive_path = archive_path;
  return item;
}

PhiArchiveItem FileItem(const std::wstring& path,
                        const std::string& archive_path,
                        uint64_t offset,
                        uint64_t length) {
  PhiArchiveItem item;
  item.source_path = path;
  item.offset = offset;
  item.length = length;
  item.archive_path = archive_path;
  return item;
}

// Zips `items` into one archive, or into as many as the 20 MB limit takes
// (bisecting a bucket whose real zip came out too large) — the Mac's
// makeZipAttachments. Archives that cannot be brought under the limit are
// skipped with a log line.
std::vector<UploadAttachment> MakeZipAttachments(
    const std::vector<PhiArchiveItem>& items,
    const std::wstring& prepared_dir,
    const std::wstring& job_root,
    const std::string& single_filename,
    const std::string& numbered_prefix,
    const char* type,
    bool required,
    bool prefer_single,
    std::string* error) {
  std::vector<UploadAttachment> attachments;
  if (prefer_single && !single_filename.empty()) {
    const std::wstring destination = Join(prepared_dir, Widen(single_filename));
    if (!PhiWriteZip(items, destination, error)) {
      return attachments;
    }
    const int64_t size = FileSize(destination);
    if (size <= PhiFeedbackOutbox::kMaxAttachmentBytes) {
      attachments.push_back(MakeUpload(destination, job_root, single_filename,
                                       "application/zip", size, type,
                                       required));
      return attachments;
    }
    ::DeleteFileW(destination.c_str());
  }

  std::vector<std::vector<PhiArchiveItem>> pending =
      PhiBucketArchiveItems(items, PhiFeedbackOutbox::kZipPlanningBytes);
  bool numbered = single_filename.empty() || pending.size() > 1;
  int ordinal = 1;
  while (!pending.empty()) {
    std::vector<PhiArchiveItem> bucket = std::move(pending.front());
    pending.erase(pending.begin());
    const std::string filename =
        numbered ? numbered_prefix + "-" + std::to_string(ordinal) + ".zip"
                 : single_filename;
    const std::wstring destination = Join(prepared_dir, Widen(filename));
    if (!PhiWriteZip(bucket, destination, error)) {
      return attachments;
    }
    const int64_t size = FileSize(destination);
    if (size > PhiFeedbackOutbox::kMaxAttachmentBytes) {
      ::DeleteFileW(destination.c_str());
      if (bucket.size() > 1) {
        numbered = true;
        const size_t midpoint = std::max<size_t>(bucket.size() / 2, 1);
        std::vector<PhiArchiveItem> head(bucket.begin(),
                                         bucket.begin() + midpoint);
        std::vector<PhiArchiveItem> tail(bucket.begin() + midpoint,
                                         bucket.end());
        pending.insert(pending.begin(), std::move(tail));
        pending.insert(pending.begin(), std::move(head));
        continue;
      }
      LOG(WARNING) << "[Feedback] " << (required ? "required" : "optional")
                   << " zip skipped because it exceeds 20 MB: " << filename
                   << " size=" << size;
      continue;
    }
    attachments.push_back(MakeUpload(destination, job_root, filename,
                                     "application/zip", size, type,
                                     required));
    ++ordinal;
  }
  return attachments;
}

std::vector<UploadAttachment> TrimPrepared(
    std::vector<UploadAttachment> attachments,
    size_t limit,
    const std::wstring& job_root,
    const char* context) {
  if (attachments.size() <= limit) {
    return attachments;
  }
  for (size_t i = limit; i < attachments.size(); ++i) {
    ::DeleteFileW(Resolve(job_root, attachments[i].relative_path).c_str());
  }
  LOG(WARNING) << "[Feedback] " << context
               << " attachments trimmed to satisfy submit limit: kept="
               << limit << " total=" << attachments.size();
  attachments.resize(limit);
  return attachments;
}

// logs.zip: Chromium's system logs plus the tail of its debug log. The Mac
// zips its whole PhiLogs directory (latest file); chrome_debug.log grows
// without bound under --enable-logging, so only its last planning-budget
// bytes ride along, which keeps the archive under the limit and the
// recent history in. OPTIONAL: an archive that cannot be built (a locked
// log, a full disk) sends the report without it rather than failing it.
std::vector<UploadAttachment> PrepareLogZipAttachments(
    const std::wstring& job_root,
    const std::wstring& prepared_dir,
    const std::optional<SourceAttachment>& system_logs,
    const std::wstring& user_data_dir) {
  std::vector<PhiArchiveItem> items;
  if (system_logs) {
    const std::wstring path = Resolve(job_root, system_logs->relative_path);
    if (PathExists(path)) {
      items.push_back(FileItem(path, kSystemLogsFilename, 0,
                               static_cast<uint64_t>(FileSize(path))));
    } else {
      LOG(WARNING) << "[Feedback] Chromium system logs file was missing "
                   << "when preparing logs.zip";
    }
  }
  const std::wstring log = Join(user_data_dir, kChromiumLogFilename);
  const std::string archive_root = "PhiLogs";
  if (PathExists(log)) {
    const int64_t size = FileSize(log);
    const int64_t length =
        std::min<int64_t>(size, PhiFeedbackOutbox::kZipPlanningBytes);
    items.push_back(FileItem(log, archive_root + "/chrome_debug.log",
                             static_cast<uint64_t>(size - length),
                             static_cast<uint64_t>(length)));
  } else {
    items.push_back(InlineItem(
        archive_root + " directory was not found at feedback submission "
                       "time.\n",
        archive_root + "/missing.txt"));
  }

  std::vector<UploadAttachment> attachments;
  const std::wstring destination =
      Join(prepared_dir, Widen(kPrimaryLogsZipFilename));
  std::string zip_error;
  if (!PhiWriteZip(items, destination, &zip_error)) {
    ::DeleteFileW(destination.c_str());
    LOG(WARNING) << "[Feedback] log zip skipped because it could not be "
                 << "written: " << zip_error;
    return attachments;
  }
  const int64_t size = FileSize(destination);
  if (size > PhiFeedbackOutbox::kMaxAttachmentBytes) {
    ::DeleteFileW(destination.c_str());
    LOG(WARNING) << "[Feedback] log zip skipped because it exceeds 20 MB: "
                 << kPrimaryLogsZipFilename << " size=" << size;
    return attachments;
  }
  attachments.push_back(MakeUpload(destination, job_root,
                                   kPrimaryLogsZipFilename,
                                   "application/zip", size, "log", false));
  return attachments;
}

// An image over the limit is re-encoded as a JPEG that fits (pasted
// screenshots can be huge PNGs; picked files were capped at 10 MB).
bool NormalizeImage(const std::wstring& job_root,
                    const SourceAttachment& source,
                    const std::wstring& prepared_dir,
                    PreparedFile* out,
                    std::string* error) {
  const std::wstring path = Resolve(job_root, source.relative_path);
  if (source.size <= PhiFeedbackOutbox::kMaxAttachmentBytes) {
    *out = {path, source.filename, source.mime_type, source.size};
    return true;
  }
  const std::wstring filename = UniqueFilename(
      ReplaceExtension(Widen(source.filename), L".jpg"), prepared_dir);
  const std::wstring destination = Join(prepared_dir, filename);
  if (!PhiEncodeJpegUnderLimit(path, PhiFeedbackOutbox::kMaxAttachmentBytes,
                               destination)) {
    *error = "The image attachment could not be prepared: " +
             source.filename + ".";
    return false;
  }
  *out = {destination, Narrow(filename), "image/jpeg", FileSize(destination)};
  return true;
}

std::vector<UploadAttachment> PrepareImageAttachments(
    const std::wstring& job_root,
    const std::wstring& prepared_dir,
    const std::vector<SourceAttachment>& sources,
    int preferred_slots,
    int max_slots,
    std::string* error) {
  std::vector<UploadAttachment> attachments;
  if (max_slots <= 0) {
    if (!sources.empty()) {
      LOG(WARNING) << "[Feedback] image attachments skipped because no "
                   << "submit slots are available";
    }
    return attachments;
  }
  std::vector<PreparedFile> images;
  for (const SourceAttachment& source : sources) {
    PreparedFile prepared;
    if (!NormalizeImage(job_root, source, prepared_dir, &prepared, error)) {
      return attachments;
    }
    images.push_back(std::move(prepared));
  }
  const int effective_preferred =
      std::min(std::max(preferred_slots, 0), max_slots);
  const auto direct = [&](const PreparedFile& image) {
    return MakeUpload(image.path, job_root, image.filename, image.mime_type,
                      image.size, "screenshot", true);
  };
  if (static_cast<int>(images.size()) <= effective_preferred) {
    for (const PreparedFile& image : images) {
      attachments.push_back(direct(image));
    }
    return attachments;
  }
  // Too many for the slots: the first few ride as previewable images, the
  // rest zip together into what remains.
  const int direct_count =
      effective_preferred > 1
          ? std::min(effective_preferred - 1, static_cast<int>(images.size()))
          : 0;
  for (int i = 0; i < direct_count; ++i) {
    attachments.push_back(direct(images[static_cast<size_t>(i)]));
  }
  std::vector<PhiArchiveItem> items;
  for (size_t i = static_cast<size_t>(direct_count); i < images.size(); ++i) {
    items.push_back(FileItem(images[i].path, "images/" + images[i].filename,
                             0, static_cast<uint64_t>(images[i].size)));
  }
  std::vector<UploadAttachment> zips =
      MakeZipAttachments(items, prepared_dir, job_root, "images.zip",
                         "images", "screenshot", true, true, error);
  if (!error->empty()) {
    return attachments;
  }
  const size_t remaining = static_cast<size_t>(
      std::max(max_slots - static_cast<int>(attachments.size()), 0));
  zips = TrimPrepared(std::move(zips), remaining, job_root, "image zip");
  attachments.insert(attachments.end(), zips.begin(), zips.end());
  return attachments;
}

std::vector<UploadAttachment> PrepareUserFileAttachments(
    const std::wstring& job_root,
    const std::wstring& prepared_dir,
    const std::vector<SourceAttachment>& sources,
    int available_slots,
    std::string* error) {
  std::vector<UploadAttachment> attachments;
  if (available_slots <= 0) {
    if (!sources.empty()) {
      LOG(WARNING) << "[Feedback] optional file attachments skipped because "
                   << "no submit slots are available";
    }
    return attachments;
  }
  std::vector<PhiArchiveItem> items;
  for (const SourceAttachment& source : sources) {
    if (source.size > PhiFeedbackOutbox::kMaxAttachmentBytes) {
      LOG(WARNING) << "[Feedback] optional file skipped because it exceeds "
                   << "20 MB: " << source.filename;
      continue;
    }
    items.push_back(FileItem(Resolve(job_root, source.relative_path),
                             source.filename, 0,
                             static_cast<uint64_t>(source.size)));
  }
  if (items.empty()) {
    return attachments;
  }
  attachments = MakeZipAttachments(items, prepared_dir, job_root,
                                   "others.zip", "others", "other", false,
                                   true, error);
  if (!error->empty()) {
    return attachments;
  }
  return TrimPrepared(std::move(attachments),
                      static_cast<size_t>(available_slots), job_root,
                      "optional file zip");
}

// Returns the error, or empty; `out` receives the prepared list.
std::string PrepareAttachments(const std::wstring& job_root,
                               const Manifest& manifest,
                               const std::wstring& user_data_dir,
                               std::vector<UploadAttachment>* out) {
  const std::wstring prepared_dir = Join(job_root, L"prepared");
  if (!CreateDirectories(prepared_dir)) {
    return "The prepared directory could not be created.";
  }
  std::string error;
  std::vector<UploadAttachment> logs = PrepareLogZipAttachments(
      job_root, prepared_dir, manifest.chromium_system_logs, user_data_dir);
  const int slots_after_logs = std::max(
      PhiFeedbackOutbox::kMaxSubmitAttachments - static_cast<int>(logs.size()),
      0);
  const bool reserve_other_slot =
      !manifest.source_files.empty() && slots_after_logs > 1;
  const int preferred_image_slots =
      std::max(slots_after_logs - (reserve_other_slot ? 1 : 0), 0);
  std::vector<UploadAttachment> images = PrepareImageAttachments(
      job_root, prepared_dir, manifest.source_images, preferred_image_slots,
      slots_after_logs, &error);
  if (!error.empty()) {
    return error;
  }
  std::vector<UploadAttachment> files = PrepareUserFileAttachments(
      job_root, prepared_dir, manifest.source_files,
      PhiFeedbackOutbox::kMaxSubmitAttachments -
          static_cast<int>(logs.size()) - static_cast<int>(images.size()),
      &error);
  if (!error.empty()) {
    return error;
  }
  // Images (required), then the optional logs and files into what is left
  // (the logs reserved their slot above).
  std::vector<UploadAttachment> required = images;
  required.insert(required.end(), logs.begin(), logs.end());
  if (static_cast<int>(required.size()) >
      PhiFeedbackOutbox::kMaxSubmitAttachments) {
    return "Required feedback attachments exceed the five attachment submit "
           "limit.";
  }
  const size_t remaining = static_cast<size_t>(
      PhiFeedbackOutbox::kMaxSubmitAttachments) - required.size();
  if (files.size() > remaining) {
    LOG(WARNING) << "[Feedback] optional attachments trimmed to satisfy "
                 << "submit limit: kept=" << remaining
                 << " total=" << files.size();
    files.resize(remaining);
  }
  required.insert(required.end(), files.begin(), files.end());
  for (const UploadAttachment& a : required) {
    VLOG(1) << "[Feedback] prepared type=" << a.attachment_type
            << " required=" << a.required << " filename=" << a.filename
            << " size=" << a.size << " path=" << a.relative_path;
  }
  *out = std::move(required);
  return std::string();
}

// --- The account service over WinHTTP (the Mac's APIClient) ---------------

// The round trip itself lives in phi_http.cc, shared with the Auth0 flow
// and the account profile fetch.
using HttpResult = PhiHttpResult;

HttpResult HttpRequest(const wchar_t* method,
                       const std::string& url,
                       const std::wstring& headers,
                       const std::string& body) {
  return PhiHttpRequest(method, url, headers, body);
}

// An attachment size for the wire. The Mac sends an integer; base::Value's
// double would serialise as "4614.0", which the account service refuses as
// an invalid body (box-verified 2026-09-10). Every attachment is capped at
// 20 MB, so int always fits.
base::Value WireSize(int64_t size) {
  return base::Value(static_cast<int>(std::min<int64_t>(size, INT_MAX)));
}

// A call against the account service. `auth` supplies the bearer; a 401
// invalidates the cached token so the retry fetches a fresh one. Returns
// the error, or empty with `data` holding the envelope's `data` object.
std::string AccountPost(PhiAuth* auth,
                        const std::string& path,
                        const base::DictValue& body,
                        base::DictValue* data) {
  const std::string token = auth->AccessTokenBlocking();
  if (token.empty()) {
    return "No account is available for feedback submission.";
  }
  std::optional<std::string> json = base::WriteJson(body);
  if (!json) {
    return "The request could not be encoded.";
  }
  const std::wstring headers =
      L"Content-Type: application/json\r\nAccept: application/json\r\n"
      L"Authorization: Bearer " +
      Widen(token);
  const HttpResult result =
      HttpRequest(L"POST", PhiAuth::AccountBaseUrl() + path, headers, *json);
  if (!result.transport_ok) {
    return "The account service could not be reached.";
  }
  if (result.status == 401) {
    auth->InvalidateAccessToken();
  }
  if (result.status < 200 || result.status >= 300) {
    // The service's own words, for the log: a 400 says WHICH field it
    // refused, and nothing else does.
    LOG(ERROR) << "[Feedback] " << path << " -> HTTP " << result.status
               << ": " << result.body.substr(0, 600);
    return "HTTP error " + std::to_string(result.status) + ".";
  }
  std::optional<base::DictValue> envelope =
      base::JSONReader::ReadDict(result.body, base::JSON_PARSE_RFC);
  if (!envelope) {
    return "The account service returned an unreadable response.";
  }
  if (envelope->FindInt("code").value_or(-1) != 0) {
    const std::string* message = envelope->FindString("message");
    return message && !message->empty() ? *message
                                        : "The account service refused the "
                                          "request.";
  }
  if (base::DictValue* payload = envelope->FindDict("data")) {
    *data = std::move(*payload);
  }
  return std::string();
}

struct Presigned {
  std::string filename;
  std::string object_key;
  std::string upload_url;
  std::map<std::string, std::string> headers;
};

std::string PresignAttachments(PhiAuth* auth,
                               const std::vector<UploadAttachment>& batch,
                               std::vector<Presigned>* out) {
  if (batch.size() > static_cast<size_t>(
                         PhiFeedbackOutbox::kMaxSubmitAttachments)) {
    return "Feedback V2 presign supports at most five attachments per "
           "request";
  }
  base::ListValue list;
  for (const UploadAttachment& a : batch) {
    base::DictValue d;
    d.Set("filename", a.filename);
    d.Set("mime_type", a.mime_type);
    d.Set("size", WireSize(a.size));
    d.Set("attachment_type", a.attachment_type);
    list.Append(std::move(d));
  }
  base::DictValue body;
  body.Set("attachments", std::move(list));
  base::DictValue data;
  const std::string error =
      AccountPost(auth, "/api/auth/feedback/v2/attachments/presign", body,
                  &data);
  if (!error.empty()) {
    return error;
  }
  out->clear();
  if (const base::ListValue* rows = data.FindList("attachments")) {
    for (const base::Value& v : *rows) {
      const base::DictValue* row = v.GetIfDict();
      if (!row) {
        continue;
      }
      Presigned p;
      const auto str = [&](const char* key) {
        const std::string* value = row->FindString(key);
        return value ? *value : std::string();
      };
      p.filename = str("filename");
      p.object_key = str("object_key");
      p.upload_url = str("upload_url");
      if (const base::DictValue* headers = row->FindDict("headers")) {
        for (const auto [name, value] : *headers) {
          if (value.is_string()) {
            p.headers[name] = value.GetString();
          }
        }
      }
      if (!p.object_key.empty() && !p.upload_url.empty()) {
        out->push_back(std::move(p));
      }
    }
  }
  if (out->size() != batch.size()) {
    return "The presign response did not cover every attachment.";
  }
  return std::string();
}

std::string UploadAttachmentFile(const UploadAttachment& attachment,
                                 const Presigned& presigned,
                                 const std::wstring& job_root) {
  std::string bytes;
  if (!ReadFileBytes(Resolve(job_root, attachment.relative_path), &bytes)) {
    return "The prepared attachment could not be read: " +
           attachment.filename;
  }
  std::wstring headers;
  bool has_content_type = false;
  for (const auto& [name, value] : presigned.headers) {
    if (::_stricmp(name.c_str(), "Content-Type") == 0) {
      has_content_type = true;
    }
    headers += Widen(name) + L": " + Widen(value) + L"\r\n";
  }
  if (!has_content_type) {
    headers += L"Content-Type: " + Widen(attachment.mime_type) + L"\r\n";
  }
  const HttpResult result =
      HttpRequest(L"PUT", presigned.upload_url, headers, bytes);
  if (!result.transport_ok) {
    return "The upload could not be sent: " + attachment.filename;
  }
  if (result.status < 200 || result.status >= 300) {
    return "The upload was refused (HTTP " + std::to_string(result.status) +
           "): " + attachment.filename;
  }
  return std::string();
}

std::string SubmitFeedback(PhiAuth* auth,
                           const Manifest& manifest,
                           std::string* feedback_id) {
  base::ListValue attachments;
  for (const UploadAttachment& a : manifest.prepared) {
    if (a.status != "uploaded" || a.object_key.empty()) {
      continue;
    }
    base::DictValue d;
    d.Set("object_key", a.object_key);
    d.Set("filename", a.filename);
    d.Set("mime_type", a.mime_type);
    d.Set("size", WireSize(a.size));
    d.Set("attachment_type", a.attachment_type);
    attachments.Append(std::move(d));
  }
  if (attachments.size() > static_cast<size_t>(
                               PhiFeedbackOutbox::kMaxSubmitAttachments)) {
    return "Feedback V2 submit supports at most five attachments";
  }
  base::DictValue body;
  body.Set("description", manifest.description);
  if (!manifest.contact_email.empty()) {
    body.Set("contact_email", manifest.contact_email);
  }
  body.Set("metadata", manifest.metadata.Clone());
  body.Set("attachments", std::move(attachments));
  base::DictValue data;
  const std::string error =
      AccountPost(auth, "/api/auth/feedback/v2/submit", body, &data);
  if (!error.empty()) {
    return error;
  }
  *feedback_id = std::to_string(data.FindInt("feedback_id").value_or(0));
  return std::string();
}

// The Mac's retrying(): up to `attempts`, sleeping 2s, 4s… between.
std::string Retrying(int attempts, const std::function<std::string()>& op) {
  std::string last;
  for (int attempt = 1; attempt <= attempts; ++attempt) {
    last = op();
    if (last.empty()) {
      return last;
    }
    if (attempt < attempts) {
      ::Sleep(static_cast<DWORD>(attempt * 2) * 1000);
    }
  }
  return last;
}

// 1, 2, 4 … minutes, capped at an hour: a week of failures is ~170
// attempts, not a tight loop.
int64_t BackoffMs(int retry_count) {
  const int exponent = std::min(retry_count, 7);
  const int64_t seconds = std::min<int64_t>(3600, (1LL << exponent) * 30);
  return seconds * 1000;
}

}  // namespace

// static
std::string PhiFeedbackOutbox::OsVersion() {
  return OsVersionString();
}

// --- PhiFeedbackOutbox -------------------------------------------------------

PhiFeedbackOutbox& PhiFeedbackOutbox::Get() {
  // Process-lifetime (the telemetry pattern): the worker may be mid-upload
  // at teardown.
  static PhiFeedbackOutbox* instance = new PhiFeedbackOutbox();
  return *instance;
}

PhiFeedbackOutbox::PhiFeedbackOutbox() {
  ::InitializeCriticalSection(&lock_);
  wake_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

PhiFeedbackOutbox::~PhiFeedbackOutbox() = default;

void PhiFeedbackOutbox::Start(const std::wstring& user_data_dir,
                              PhiAuth* auth) {
  ::EnterCriticalSection(&lock_);
  const bool already = started_;
  if (!already) {
    started_ = true;
    user_data_dir_ = user_data_dir;
    // Every account's outbox sits in its own directory (the Mac's
    // users/<sub>/feedbackOutbox); root_ is the accounts' parent.
    root_ = Join(Join(user_data_dir, L"Phi"), L"users");
    auth_ = auth;
  }
  ::LeaveCriticalSection(&lock_);
  if (already || user_data_dir.empty()) {
    return;
  }
  thread_ = ::CreateThread(nullptr, 0, &PhiFeedbackOutbox::WorkerThread, this,
                           0, nullptr);
}

void PhiFeedbackOutbox::SetAccount(const std::string& user_id,
                                   const std::string& email) {
  ::EnterCriticalSection(&lock_);
  const bool changed = user_id != user_id_;
  user_id_ = user_id;
  email_ = email;
  ::LeaveCriticalSection(&lock_);
  if (changed) {
    Kick();
  }
}

void PhiFeedbackOutbox::SetNotify(HWND hwnd, UINT msg) {
  ::EnterCriticalSection(&lock_);
  notify_hwnd_ = hwnd;
  notify_msg_ = msg;
  ::LeaveCriticalSection(&lock_);
}

int PhiFeedbackOutbox::TakeAbandonedJobCount() {
  ::EnterCriticalSection(&lock_);
  const int count = abandoned_jobs_;
  abandoned_jobs_ = 0;
  ::LeaveCriticalSection(&lock_);
  return count;
}

void PhiFeedbackOutbox::NoteJobAbandoned() {
  ::EnterCriticalSection(&lock_);
  ++abandoned_jobs_;
  const HWND hwnd = notify_hwnd_;
  const UINT msg = notify_msg_;
  ::LeaveCriticalSection(&lock_);
  if (hwnd && msg) {
    ::PostMessageW(hwnd, msg, 0, 0);
  }
}

void PhiFeedbackOutbox::PurgeAccount(const std::string& user_id) {
  if (user_id.empty()) {
    return;
  }
  ::EnterCriticalSection(&lock_);
  pending_purges_.push_back(user_id);
  ::LeaveCriticalSection(&lock_);
  Kick();
}

void PhiFeedbackOutbox::RunPendingPurges() {
  std::vector<std::string> purges;
  std::string current;
  std::wstring root;
  ::EnterCriticalSection(&lock_);
  purges.swap(pending_purges_);
  current = user_id_;
  root = root_;
  ::LeaveCriticalSection(&lock_);
  if (root.empty()) {
    return;
  }
  for (const std::string& user_id : purges) {
    if (user_id == current) {
      continue;  // Signed back in before the purge ran: the jobs are live.
    }
    LOG(INFO) << "[Feedback] signed out; outbox purged";
    RemoveTree(AccountRoot(user_id));
  }
}

void PhiFeedbackOutbox::Kick() {
  if (wake_) {
    ::SetEvent(wake_);
  }
}

void PhiFeedbackOutbox::Stop() {
  HANDLE thread = nullptr;
  ::EnterCriticalSection(&lock_);
  stopping_ = true;
  thread = thread_;
  thread_ = nullptr;
  ::LeaveCriticalSection(&lock_);
  Kick();
  if (thread) {
    ::WaitForSingleObject(thread, 2000);
    ::CloseHandle(thread);
  }
}

std::string PhiFeedbackOutbox::CurrentUserId() {
  ::EnterCriticalSection(&lock_);
  std::string user_id = user_id_;
  ::LeaveCriticalSection(&lock_);
  return user_id;
}

std::wstring PhiFeedbackOutbox::AccountRoot(const std::string& user_id) const {
  // <UserData>\Phi\users\<account>\feedbackOutbox, the account directory
  // named as everywhere else (PhiAccountDirName: "auth0|5f…" -> "auth0_5f…").
  return Join(Join(root_, Widen(PhiAccountDirName(user_id))),
              L"feedbackOutbox");
}

bool PhiFeedbackOutbox::Enqueue(const PhiFeedbackDraft& draft,
                                std::string* error) {
  std::string user_id;
  std::string email;
  std::wstring root;
  ::EnterCriticalSection(&lock_);
  user_id = user_id_;
  email = email_;
  root = root_;
  ::LeaveCriticalSection(&lock_);
  if (user_id.empty() || root.empty()) {
    *error = "No account is available for feedback submission.";
    return false;
  }
  const std::string description = Trim(draft.description);
  if (description.empty()) {
    *error = "Feedback description is empty.";
    return false;
  }

  const std::string job_id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  const std::wstring job_root = Join(AccountRoot(user_id), Widen(job_id));
  const std::wstring images_dir = Join(job_root, L"images");
  const std::wstring files_dir = Join(job_root, L"files");
  const std::wstring logs_dir = Join(job_root, L"logs");
  if (!CreateDirectories(images_dir) || !CreateDirectories(files_dir) ||
      !CreateDirectories(logs_dir) ||
      !CreateDirectories(Join(job_root, L"prepared"))) {
    *error = "The feedback outbox directory could not be created.";
    return false;
  }

  Manifest manifest;
  manifest.id = job_id;
  manifest.created_at_ms = NowMs();
  manifest.description = description;
  manifest.contact_email = email;
  manifest.metadata = MakeMetadata(job_id, draft);
  manifest.archive_strategy_version = kArchiveStrategyVersion;
  manifest.status = "queued";

  const auto make_source = [&](const std::wstring& path,
                               const std::string& mime_type) {
    SourceAttachment source;
    source.relative_path = RelativeTo(path, job_root);
    source.filename = Narrow(Leaf(path));
    source.mime_type = mime_type;
    source.size = FileSize(path);
    return source;
  };
  for (const PhiFeedbackDraftAttachment& attachment : draft.attachments) {
    if (attachment.file_path.empty()) {
      const std::wstring filename =
          UniqueFilename(attachment.filename, images_dir);
      const std::wstring destination = Join(images_dir, filename);
      if (!WriteFileAtomic(destination, attachment.pasted_png)) {
        *error = "The pasted image could not be saved.";
        RemoveTree(job_root);
        return false;
      }
      manifest.source_images.push_back(make_source(destination, "image/png"));
      continue;
    }
    int64_t size = 0;
    std::string mime_type;
    bool is_image = false;
    if (!SelectedAttachmentInfo(attachment.file_path, &size, &mime_type,
                                &is_image, error)) {
      RemoveTree(job_root);
      return false;
    }
    is_image = is_image || attachment.is_image;
    const std::wstring& target_dir = is_image ? images_dir : files_dir;
    const std::wstring filename =
        UniqueFilename(Leaf(attachment.file_path), target_dir);
    const std::wstring destination = Join(target_dir, filename);
    if (!::CopyFileW(attachment.file_path.c_str(), destination.c_str(),
                     TRUE)) {
      *error = "The attachment could not be copied: " +
               Narrow(Leaf(attachment.file_path));
      RemoveTree(job_root);
      return false;
    }
    (is_image ? manifest.source_images : manifest.source_files)
        .push_back(make_source(destination, mime_type));
  }
  if (draft.chromium_system_logs) {
    const std::wstring destination =
        Join(logs_dir, Widen(kSystemLogsFilename));
    if (WriteFileAtomic(destination, *draft.chromium_system_logs)) {
      manifest.chromium_system_logs = make_source(destination, "text/plain");
    }
  }
  if (!WriteManifest(manifest, job_root)) {
    *error = "The feedback job could not be written.";
    RemoveTree(job_root);
    return false;
  }
  LOG(INFO) << "[Feedback] outbox job enqueued: " << job_id
            << " images=" << manifest.source_images.size()
            << " files=" << manifest.source_files.size()
            << " systemLogs=" << (manifest.chromium_system_logs ? 1 : 0);
  Kick();
  return true;
}

// static
DWORD WINAPI PhiFeedbackOutbox::WorkerThread(void* self) {
  static_cast<PhiFeedbackOutbox*>(self)->RunLoop();
  return 0;
}

void PhiFeedbackOutbox::RunLoop() {
  // WIC (image re-encoding) wants COM on this thread.
  const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  DWORD wait = INFINITE;
  for (;;) {
    ::WaitForSingleObject(wake_, wait);
    // Before the stop check: a sign-out right before quitting still
    // deletes that account's reports.
    RunPendingPurges();
    ::EnterCriticalSection(&lock_);
    const bool stopping = stopping_;
    ::LeaveCriticalSection(&lock_);
    if (stopping) {
      break;
    }
    wait = ProcessOutbox() ? kRetryScanMs : INFINITE;
  }
  if (SUCCEEDED(com)) {
    ::CoUninitialize();
  }
}

bool PhiFeedbackOutbox::ProcessOutbox() {
  const std::string user_id = CurrentUserId();
  if (user_id.empty()) {
    return false;
  }
  const std::wstring account_root = AccountRoot(user_id);
  if (!CreateDirectories(account_root)) {
    LOG(ERROR) << "[Feedback] outbox root could not be created";
    return true;
  }
  bool retryable = false;
  for (const std::wstring& name : ListDirectory(account_root, true)) {
    if (CurrentUserId() != user_id) {
      return retryable;
    }
    retryable = ProcessJob(Join(account_root, name), user_id) || retryable;
  }
  return retryable;
}

bool PhiFeedbackOutbox::ProcessJob(const std::wstring& job_root,
                                   const std::string& user_id) {
  std::optional<Manifest> read = ReadManifest(job_root);
  if (!read) {
    LOG(ERROR) << "[Feedback] job ignored because manifest is unreadable: "
               << Narrow(job_root);
    return false;
  }
  Manifest manifest = std::move(*read);
  if (manifest.status == "submitted") {
    return false;
  }
  if (manifest.next_attempt_at_ms > NowMs()) {
    return true;
  }
  if (ShouldDiscardFailedJob(manifest.retry_count, manifest.created_at_ms,
                             NowMs())) {
    LOG(ERROR) << "[Feedback] job discarded because retry limit was already "
               << "exceeded: job=" << manifest.id
               << " retryCount=" << manifest.retry_count;
    RemoveTree(job_root);
    NoteJobAbandoned();
    return false;
  }
  // Jobs prepared before logs.zip became optional still mark it required;
  // a log archive never holds a report back.
  for (UploadAttachment& a : manifest.prepared) {
    if (a.attachment_type == "log") {
      a.required = false;
    }
  }
  if (CurrentUserId() != user_id) {
    return false;
  }

  // The attempt, as one string of steps; the first failing step's message
  // becomes the job's lastError.
  const std::string error = [&]() -> std::string {
    if (manifest.archive_strategy_version != kArchiveStrategyVersion) {
      LOG(INFO) << "[Feedback] archive strategy changed; preparing job "
                << "again: " << manifest.id;
      RemoveTree(Join(job_root, L"prepared"));
      manifest.prepared.clear();
      manifest.archive_strategy_version = kArchiveStrategyVersion;
    }
    const bool prepared_exist = std::all_of(
        manifest.prepared.begin(), manifest.prepared.end(),
        [&](const UploadAttachment& a) {
          return PathExists(Resolve(job_root, a.relative_path));
        });
    if (!manifest.prepared.empty() && !prepared_exist) {
      LOG(WARNING) << "[Feedback] prepared files are incomplete; preparing "
                   << "job again: " << manifest.id;
      RemoveTree(Join(job_root, L"prepared"));
      manifest.prepared.clear();
    }
    if (manifest.prepared.empty()) {
      manifest.status = "preparing";
      manifest.last_error.clear();
      WriteManifest(manifest, job_root);
      std::vector<UploadAttachment> prepared;
      const std::string prepare_error =
          PrepareAttachments(job_root, manifest, user_data_dir_, &prepared);
      if (!prepare_error.empty()) {
        return prepare_error;
      }
      manifest.prepared = std::move(prepared);
      manifest.archive_strategy_version = kArchiveStrategyVersion;
      WriteManifest(manifest, job_root);
    }

    manifest.status = "uploading";
    manifest.last_error.clear();
    WriteManifest(manifest, job_root);

    // Required attachments first, in batches of five; a required failure
    // fails the attempt, an optional one is dropped from the report.
    for (const bool required_batch : {true, false}) {
      std::vector<size_t> indices;
      for (size_t i = 0; i < manifest.prepared.size(); ++i) {
        if (manifest.prepared[i].required == required_batch &&
            manifest.prepared[i].status != "uploaded") {
          indices.push_back(i);
        }
      }
      for (size_t start = 0; start < indices.size();
           start += static_cast<size_t>(kMaxSubmitAttachments)) {
        if (CurrentUserId() != user_id) {
          return "A required feedback attachment failed to upload: account "
                 "changed.";
        }
        std::vector<size_t> batch(
            indices.begin() + static_cast<ptrdiff_t>(start),
            indices.begin() +
                static_cast<ptrdiff_t>(std::min(
                    start + static_cast<size_t>(kMaxSubmitAttachments),
                    indices.size())));
        std::vector<UploadAttachment> requests;
        for (size_t index : batch) {
          requests.push_back(manifest.prepared[index]);
        }
        std::vector<Presigned> presigned;
        const std::string presign_error = Retrying(3, [&] {
          return PresignAttachments(auth_, requests, &presigned);
        });
        if (!presign_error.empty()) {
          if (required_batch) {
            return presign_error;
          }
          for (size_t index : batch) {
            manifest.prepared[index].retry_count += 1;
            manifest.prepared[index].status = "failed";
          }
          WriteManifest(manifest, job_root);
          LOG(WARNING) << "[Feedback] optional attachment batch skipped "
                       << "after presign retry: " << presign_error;
          continue;
        }
        for (size_t position = 0; position < batch.size(); ++position) {
          UploadAttachment& attachment = manifest.prepared[batch[position]];
          const Presigned& first = presigned[position];
          std::string upload_error = Retrying(2, [&] {
            return UploadAttachmentFile(attachment, first, job_root);
          });
          std::string object_key = first.object_key;
          if (!upload_error.empty()) {
            // Once more with a fresh presign (the first may have expired).
            std::vector<Presigned> fresh;
            upload_error = Retrying(3, [&] {
              return PresignAttachments(auth_, {attachment}, &fresh);
            });
            if (upload_error.empty()) {
              upload_error = Retrying(2, [&] {
                return UploadAttachmentFile(attachment, fresh.front(),
                                            job_root);
              });
              object_key =
                  fresh.empty() ? std::string() : fresh.front().object_key;
            }
          }
          if (!upload_error.empty()) {
            attachment.retry_count += 1;
            attachment.status = "failed";
            WriteManifest(manifest, job_root);
            if (attachment.required) {
              return "A required feedback attachment failed to upload: " +
                     attachment.filename + " (" + upload_error + ")";
            }
            LOG(WARNING) << "[Feedback] optional attachment skipped after "
                         << "upload retry: " << attachment.filename;
            continue;
          }
          attachment.status = "uploaded";
          attachment.object_key = object_key;
          attachment.retry_count = 0;
          WriteManifest(manifest, job_root);
        }
      }
    }
    for (const UploadAttachment& a : manifest.prepared) {
      if (a.required && a.status != "uploaded") {
        return "A required feedback attachment failed to upload: required "
               "attachments.";
      }
    }
    if (CurrentUserId() != user_id) {
      return "A required feedback attachment failed to upload: account "
             "changed.";
    }
    manifest.status = "submitting";
    WriteManifest(manifest, job_root);
    std::string feedback_id;
    const std::string submit_error = Retrying(3, [&] {
      return SubmitFeedback(auth_, manifest, &feedback_id);
    });
    if (!submit_error.empty()) {
      return submit_error;
    }
    LOG(INFO) << "[Feedback] submitted: job=" << manifest.id
              << " feedbackID=" << feedback_id;
    return std::string();
  }();

  if (error.empty()) {
    manifest.status = "submitted";
    manifest.next_attempt_at_ms = 0;
    manifest.last_error.clear();
    WriteManifest(manifest, job_root);
    RemoveTree(job_root);
    return false;
  }
  manifest.status = "failedWaitingRetry";
  manifest.retry_count += 1;
  manifest.last_error = error;
  if (ShouldDiscardFailedJob(manifest.retry_count, manifest.created_at_ms,
                             NowMs())) {
    LOG(ERROR) << "[Feedback] job discarded after retry limit: job="
               << manifest.id << " retryCount=" << manifest.retry_count
               << " error=" << error;
    RemoveTree(job_root);
    NoteJobAbandoned();
    return false;
  }
  manifest.next_attempt_at_ms = NowMs() + BackoffMs(manifest.retry_count);
  WriteManifest(manifest, job_root);
  LOG(ERROR) << "[Feedback] job failed and will retry: job=" << manifest.id
             << " error=" << error;
  return true;
}

std::string PhiFeedbackOutbox::StatusJson() {
  std::string user_id;
  std::wstring root;
  ::EnterCriticalSection(&lock_);
  user_id = user_id_;
  root = root_;
  ::LeaveCriticalSection(&lock_);
  base::DictValue out;
  out.Set("account", user_id);
  out.Set("root", Narrow(root));
  base::ListValue jobs;
  if (!user_id.empty() && !root.empty()) {
    const std::wstring account_root = AccountRoot(user_id);
    for (const std::wstring& name : ListDirectory(account_root, true)) {
      std::optional<Manifest> manifest =
          ReadManifest(Join(account_root, name));
      base::DictValue job;
      job.Set("id", Narrow(name));
      if (!manifest) {
        job.Set("status", "unreadable");
        jobs.Append(std::move(job));
        continue;
      }
      job.Set("status", manifest->status);
      job.Set("retryCount", manifest->retry_count);
      job.Set("lastError", manifest->last_error);
      job.Set("nextAttemptAtMs",
              static_cast<double>(manifest->next_attempt_at_ms));
      job.Set("sourceImages", static_cast<int>(manifest->source_images.size()));
      job.Set("sourceFiles", static_cast<int>(manifest->source_files.size()));
      job.Set("hasSystemLogs", manifest->chromium_system_logs.has_value());
      job.Set("prepared", static_cast<int>(manifest->prepared.size()));
      int uploaded = 0;
      base::ListValue prepared;
      for (const UploadAttachment& a : manifest->prepared) {
        uploaded += a.status == "uploaded" ? 1 : 0;
        base::DictValue row;
        row.Set("filename", a.filename);
        row.Set("type", a.attachment_type);
        row.Set("required", a.required);
        row.Set("status", a.status);
        row.Set("size", static_cast<double>(a.size));
        prepared.Append(std::move(row));
      }
      job.Set("uploaded", uploaded);
      job.Set("preparedAttachments", std::move(prepared));
      job.Set("description", manifest->description);
      job.Set("metadata", manifest->metadata.Clone());
      jobs.Append(std::move(job));
    }
  }
  out.Set("jobs", std::move(jobs));
  std::optional<std::string> json = base::WriteJson(out);
  return json ? *json : std::string("{}");
}

// static
bool PhiFeedbackOutbox::SelectedAttachmentInfo(const std::wstring& path,
                                               int64_t* size,
                                               std::string* mime_type,
                                               bool* is_image,
                                               std::string* error) {
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
      (data.dwFileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
        FILE_ATTRIBUTE_DEVICE))) {
    *error = "Only regular files can be attached.";
    return false;
  }
  *size = (static_cast<int64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
  if (*size > kMaxSelectedAttachmentBytes) {
    *error = Narrow(Leaf(path)) + " is larger than 10 MB.";
    return false;
  }
  *mime_type = MimeTypeForPath(path);
  *is_image = mime_type->rfind("image/", 0) == 0;
  return true;
}

// static
std::string PhiFeedbackOutbox::MimeTypeForPath(const std::wstring& path) {
  const std::wstring ext = Extension(path);
  if (ext.empty()) {
    return "application/octet-stream";
  }
  struct Known {
    const wchar_t* ext;
    const char* mime;
  };
  static constexpr Known kKnown[] = {
      {L".png", "image/png"},   {L".jpg", "image/jpeg"},
      {L".jpeg", "image/jpeg"}, {L".gif", "image/gif"},
      {L".bmp", "image/bmp"},   {L".webp", "image/webp"},
      {L".tif", "image/tiff"},  {L".tiff", "image/tiff"},
      {L".heic", "image/heic"}, {L".svg", "image/svg+xml"},
      {L".txt", "text/plain"},  {L".log", "text/plain"},
      {L".json", "application/json"}, {L".zip", "application/zip"},
      {L".pdf", "application/pdf"}};
  for (const Known& known : kKnown) {
    if (ext == known.ext) {
      return known.mime;
    }
  }
  const std::string registered =
      RegistryString(HKEY_CLASSES_ROOT, ext.c_str(), L"Content Type");
  return registered.empty() ? "application/octet-stream" : registered;
}

// static
bool PhiFeedbackOutbox::IsPhiExtensionId(const std::string& id) {
  for (const char* known : kPhiExtensionIds) {
    if (id == known) {
      return true;
    }
  }
  return false;
}

// static
std::string PhiFeedbackOutbox::BrowserChannelName() {
#if defined(DCHECK_ALWAYS_ON) || !defined(NDEBUG)
  return "debug";
#else
  return PHI_WIN_CHANNEL;
#endif
}

}  // namespace phi_client

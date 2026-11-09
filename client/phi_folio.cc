// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_folio.h"

#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <utility>

#include "base/base64.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "base/values.h"
#include "client/phi_features.h"
#include "client/phi_file_io.h"
#include "client/phi_folio_paths.h"
#include "client/phi_i18n.h"
#include "client/phi_log.h"
#include "client/phi_preferences.h"
#include "client/phi_prompt.h"
#include "client/phi_reader.h"

namespace phi_client {

namespace {

// The posted "results are waiting" nudge, and the one housekeeping timer.
constexpr UINT WM_PHI_FOLIO_DRAIN = WM_APP + 1;
constexpr UINT_PTR kTickTimer = 1;
constexpr UINT kTickMs = 1000;

// The Mac's budgets (SaveForLaterService.swift), in milliseconds/bytes.
// A save's reply acknowledges ACCEPTANCE, not a finished archive: a video
// save takes minutes, and waiting for completion here would time out
// mid-flight and write the item twice.
constexpr ULONGLONG kSaveAcceptMs = 12 * 1000;
// Longer than any save takes, short enough that a reused tab id resolves
// freshly.
constexpr ULONGLONG kPinnedFolderMs = 3600 * 1000;
// Repeated toggling of a like inside the window is one snapshot.
constexpr ULONGLONG kAutoSaveDedupMs = 600 * 1000;
// Idle timeout of one streamed write: only longer than the gap between two
// chunks, never a deadline for the whole capture.
constexpr ULONGLONG kBrokerIdleMs = 180 * 1000;
constexpr ULONGLONG kRenamedArchiveMs = 600 * 1000;
// base64 grows 4/3, so 512 KiB chunks stay inside the bridge's 1 MiB cap.
constexpr size_t kReadChunkBytes = 512 * 1024;
// The listing carries each Markdown file's frontmatter (and, budget
// permitting, the start of its body), bounded so hundreds of items still
// fit one bridge message.
constexpr size_t kListingHeadBytes = 4096;
constexpr size_t kListingHeadBudget = 600 * 1024;

constexpr char kPrefix[] = "saveForLater.";

std::string ToJson(const base::Value& value) {
  std::string json;
  base::JSONWriter::Write(value, &json);
  return json;
}

std::string ErrorJson(const char* code) {
  base::DictValue reply;
  reply.Set("error", code);
  return ToJson(base::Value(std::move(reply)));
}

constexpr char kOk[] = "{\"ok\":true}";

struct KeyedString {
  const char* key;
  const char* string_id;
};
// The outcome Mirage NAMES, and the catalog string that WORDS it.
constexpr KeyedString kToastTitles[] = {
    {"saved", "browser.folio.savedToast"},
    {"savedWithoutWebpage", "browser.folio.savedWithoutWebpageToast"},
    {"savedWithoutVideoArticle", "browser.folio.savedWithoutArticleToast"},
    {"savedPartial", "browser.folio.savedPartialToast"},
    {"alreadySaved", "browser.folio.alreadySavedToast"},
    {"highlightSaved", "browser.folio.highlightSavedToast"},
    {"failed", "browser.folio.failedToast"},
};
// A failure toast's second line: why, in words.
constexpr KeyedString kToastReasons[] = {
    {"folder_unreadable", "browser.folio.reasonFolderUnreadable"},
    {"markdown_write_failed", "browser.folio.reasonWriteFailed"},
    {"not_a_web_page", "browser.folio.reasonNotAWebPage"},
    {"page_gone", "browser.folio.reasonPageGone"},
    {"nothing_captured", "browser.folio.reasonNothingCaptured"},
    {"save_timed_out", "browser.folio.reasonSaveTimedOut"},
};

template <size_t N>
const char* LookUp(const KeyedString (&table)[N], const std::string& key) {
  for (const KeyedString& entry : table) {
    if (key == entry.key) {
      return entry.string_id;
    }
  }
  return nullptr;
}

std::optional<int> OptionalTabId(const base::DictValue& dict) {
  if (std::optional<int> id = dict.FindInt("tabId")) {
    return id;
  }
  // A JSON number the extension built from a double.
  if (std::optional<double> id = dict.FindDouble("tabId")) {
    return static_cast<int>(*id);
  }
  return std::nullopt;
}

std::string StringOr(const base::DictValue& dict, const char* key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

std::wstring Join(const std::wstring& folder, const std::string& name) {
  return folder + L"\\" + base::UTF8ToWide(name);
}

bool Exists(const std::wstring& path) {
  return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring WithExtension(const std::wstring& path, const wchar_t* ext) {
  const size_t dot = path.find_last_of(L'.');
  const size_t slash = path.find_last_of(L'\\');
  const std::wstring stem =
      dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)
          ? path
          : path.substr(0, dot);
  return stem + ext;
}

bool EndsWithW(const std::wstring& text, const wchar_t* suffix) {
  const size_t n = wcslen(suffix);
  return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

std::wstring LastComponent(const std::wstring& path) {
  std::wstring trimmed = path;
  while (!trimmed.empty() &&
         (trimmed.back() == L'\\' || trimmed.back() == L'/')) {
    trimmed.pop_back();
  }
  const size_t cut = trimmed.find_last_of(L"\\/");
  return cut == std::wstring::npos ? trimmed : trimmed.substr(cut + 1);
}

bool EnsureFolder(const std::wstring& folder) {
  const int result = ::SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
  return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS ||
         result == ERROR_FILE_EXISTS;
}

double UnixSeconds(const FILETIME& time) {
  ULARGE_INTEGER value;
  value.LowPart = time.dwLowDateTime;
  value.HighPart = time.dwHighDateTime;
  constexpr ULONGLONG kEpochDelta = 116444736000000000ULL;  // 1601 -> 1970.
  if (value.QuadPart < kEpochDelta) {
    return 0;
  }
  return static_cast<double>(value.QuadPart - kEpochDelta) / 1e7;
}

// The bytes of `path` at [0, max), or empty.
std::string ReadPrefix(const std::wstring& path, size_t max) {
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return std::string();
  }
  std::string data(max, '\0');
  DWORD read = 0;
  if (!::ReadFile(file, data.data(), static_cast<DWORD>(max), &read,
                  nullptr)) {
    read = 0;
  }
  ::CloseHandle(file);
  data.resize(read);
  return data;
}

// The folder listing, off the UI thread (the Mac's fs.list body).
std::string ListFolder(const std::wstring& folder) {
  struct Listed {
    std::wstring path;
    std::string name;
    double size = 0;
    double modified = 0;
    std::string prefix;
  };
  WIN32_FIND_DATAW data = {};
  HANDLE find = ::FindFirstFileExW((folder + L"\\*").c_str(),
                                   FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
  if (find == INVALID_HANDLE_VALUE) {
    // A folder that does not exist yet is an EMPTY library — the first save
    // creates it. One that exists and cannot be read is a failure, and must
    // say so: the caller reserves basenames from this listing, and an empty
    // answer would let it claim a taken name and overwrite that pair.
    const DWORD attributes = ::GetFileAttributesW(folder.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      return "{\"folder\":\"\",\"entries\":[]}";
    }
    LOG(WARNING) << "[Folio] list failed, err=" << ::GetLastError();
    return ErrorJson("list_failed");
  }
  std::vector<Listed> listed;
  do {
    const std::wstring name = data.cFileName;
    if (name.empty() || name[0] == L'.' ||
        (data.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN |
          FILE_ATTRIBUTE_REPARSE_POINT))) {
      continue;  // Part files, folders and links are not items.
    }
    if (!EndsWithW(name, L".md") && !EndsWithW(name, L".mhtml")) {
      continue;
    }
    Listed entry;
    entry.path = folder + L"\\" + name;
    entry.name = base::WideToUTF8(name);
    entry.size = static_cast<double>(
        (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) |
        data.nFileSizeLow);
    entry.modified = UnixSeconds(data.ftLastWriteTime);
    listed.push_back(std::move(entry));
  } while (::FindNextFileW(find, &data));
  ::FindClose(find);
  // Newest first, so a budget that runs out leaves the oldest items short
  // rather than an arbitrary few.
  std::sort(listed.begin(), listed.end(),
            [](const Listed& a, const Listed& b) {
              return a.modified > b.modified;
            });
  for (Listed& entry : listed) {
    if (EndsWithW(entry.path, L".md")) {
      entry.prefix = ReadPrefix(entry.path, kListingHeadBytes);
    }
  }
  // Two passes over one budget. Every item's frontmatter first: the
  // extension finds an item by its `source`, so an item listed without one
  // is invisible to "already saved" and gets written twice. Only what is
  // left extends the newest heads into the body.
  std::vector<std::optional<std::string>> heads(listed.size());
  size_t budget = kListingHeadBudget;
  for (size_t i = 0; i < listed.size(); ++i) {
    std::string frontmatter = PhiFolioFrontmatterPrefix(listed[i].prefix);
    if (frontmatter.empty() || frontmatter.size() > budget) {
      continue;
    }
    budget -= frontmatter.size();
    heads[i] = std::move(frontmatter);
  }
  for (size_t i = 0; i < listed.size(); ++i) {
    const std::string& prefix = listed[i].prefix;
    const size_t current = heads[i] ? heads[i]->size() : 0;
    if (prefix.size() <= current || prefix.size() - current > budget) {
      continue;
    }
    budget -= prefix.size() - current;
    heads[i] = prefix;
  }
  base::ListValue entries;
  for (size_t i = 0; i < listed.size(); ++i) {
    base::DictValue entry;
    entry.Set("name", listed[i].name);
    entry.Set("size", listed[i].size);
    entry.Set("modified", listed[i].modified);
    if (heads[i]) {
      // A fixed byte prefix can land mid-character: trimmed back to a
      // character boundary rather than handed over as U+FFFD.
      entry.Set("head", PhiFolioTrimToUtf8Boundary(*heads[i]));
    }
    entries.Append(std::move(entry));
  }
  base::DictValue reply;
  reply.Set("folder", base::WideToUTF8(folder));
  reply.Set("entries", std::move(entries));
  return ToJson(base::Value(std::move(reply)));
}

std::string ReadChunk(const std::wstring& path, int64_t offset, size_t length) {
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return ErrorJson("not_found");
  }
  std::string reply;
  LARGE_INTEGER size = {};
  LARGE_INTEGER seek = {};
  seek.QuadPart = offset;
  // EOF is a claim about the file, so it needs the file's size: a short
  // read is normal, an unstattable file is not.
  if (!::GetFileSizeEx(file, &size) ||
      !::SetFilePointerEx(file, seek, nullptr, FILE_BEGIN)) {
    ::CloseHandle(file);
    return ErrorJson("read_failed");
  }
  std::string chunk(length, '\0');
  DWORD read = 0;
  const bool ok = ::ReadFile(file, chunk.data(), static_cast<DWORD>(length),
                             &read, nullptr);
  ::CloseHandle(file);
  if (!ok) {
    return ErrorJson("read_failed");
  }
  chunk.resize(read);
  base::DictValue out;
  out.Set("data", base::Base64Encode(chunk));
  out.Set("eof", offset + static_cast<int64_t>(read) >= size.QuadPart);
  return ToJson(base::Value(std::move(out)));
}

// Moves a pair to the first free name at `desired`. The Markdown is the
// item: when its move fails nothing changed and the old name comes back,
// so the caller's next write lands on a file that exists.
std::string RenamePair(const std::wstring& folder,
                       const std::string& from,
                       const std::string& desired) {
  if (desired == from) {
    return from;
  }
  const std::string target = PhiFolioFirstFreeBasename(
      desired, [&](const std::string& name) {
        return Exists(Join(folder, name + ".md")) ||
               Exists(Join(folder, name + ".mhtml"));
      });
  if (!::MoveFileExW(Join(folder, from + ".md").c_str(),
                     Join(folder, target + ".md").c_str(), 0)) {
    return from;
  }
  const std::wstring old_webpage = Join(folder, from + ".mhtml");
  if (Exists(old_webpage)) {
    ::MoveFileExW(old_webpage.c_str(), Join(folder, target + ".mhtml").c_str(),
                  0);
  }
  return target;
}

// A copy that landed under its pair's old name moves to the new one, when
// the old name holds no Markdown and the new one has no copy yet.
void MoveLandedArchive(const std::wstring& from, const std::wstring& to) {
  if (!Exists(from) || Exists(to) || Exists(WithExtension(from, L".md"))) {
    return;
  }
  if (::MoveFileExW(from.c_str(), to.c_str(), 0)) {
    LOG(INFO) << "[Folio] webpage copy followed its renamed pair";
  } else {
    LOG(WARNING) << "[Folio] webpage copy left under its old name, err="
                 << ::GetLastError();
  }
}

// Undo of an automatic save: the pair to the Recycle Bin, unless the user
// has highlighted it since — deliberate work the file now carries alone.
void RecyclePair(const std::wstring& folder, const std::string& basename) {
  std::string content;
  if (PhiReadFileBytes(Join(folder, basename + ".md"), &content) &&
      PhiFolioHasUserHighlights(content)) {
    LOG(INFO) << "[Folio] undo skipped: the item has highlights";
    return;
  }
  for (const char* ext : {".md", ".mhtml"}) {
    const std::wstring path = Join(folder, basename + ext);
    if (!Exists(path)) {
      continue;
    }
    std::wstring from = path;
    from.push_back(L'\0');  // SHFileOperation takes a double-NUL list.
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT |
                FOF_NOERRORUI;
    ::SHFileOperationW(&op);
  }
}

}  // namespace

std::string PhiFolioDefaultFolder() {
  std::wstring base;
  PWSTR documents = nullptr;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr,
                                       &documents)) &&
      documents) {
    base = documents;
  }
  ::CoTaskMemFree(documents);
  if (base.empty()) {
    wchar_t profile[MAX_PATH] = {};
    const DWORD n =
        ::GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    base = (n > 0 && n < MAX_PATH ? std::wstring(profile) : std::wstring()) +
           L"\\Documents";
  }
  return base::WideToUTF8(base + L"\\PhiFolio");
}

PhiFolio::PhiFolio(Host* host) : host_(host) {
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = &PhiFolio::WndProc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = L"PhiFolioMessages";
  ::RegisterClassExW(&wc);
  msg_hwnd_ = ::CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
  if (msg_hwnd_) {
    ::SetWindowLongPtrW(msg_hwnd_, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(this));
  }
  worker_wake_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  worker_thread_ = ::CreateThread(
      nullptr, 0,
      [](LPVOID self) -> DWORD {
        static_cast<PhiFolio*>(self)->WorkerLoop();
        return 0;
      },
      this, 0, nullptr);
}

PhiFolio::~PhiFolio() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    worker_jobs_.clear();
  }
  if (worker_wake_) {
    ::SetEvent(worker_wake_);
  }
  if (worker_thread_) {
    ::WaitForSingleObject(worker_thread_, 5000);
    ::CloseHandle(worker_thread_);
  }
  if (worker_wake_) {
    ::CloseHandle(worker_wake_);
  }
  for (auto& [token, write] : broker_writes_) {
    ::CloseHandle(write.handle);
    ::DeleteFileW(write.part.c_str());
  }
  if (msg_hwnd_) {
    ::KillTimer(msg_hwnd_, kTickTimer);
    ::SetWindowLongPtrW(msg_hwnd_, GWLP_USERDATA, 0);
    ::DestroyWindow(msg_hwnd_);
  }
}

// static
bool PhiFolio::Enabled() {
  return kPhiFolioEnabled;
}

bool PhiFolio::AutoTriggerArmed() const {
  return Enabled() && kPhiFolioAutoTriggerEnabled &&
         host_->FolioPreferences().FolioAutoSaveOnSiteActions();
}

bool PhiFolio::CanSave(int tab_id) const {
  if (!Enabled() || in_flight_tabs_.count(tab_id)) {
    return false;
  }
  const std::optional<TabInfo> tab = host_->FolioFindTab(tab_id);
  return tab && PhiFolioIsSaveableUrl(tab->url);
}

void PhiFolio::Save(int tab_id) {
  if (!CanSave(tab_id)) {
    return;
  }
  const std::optional<TabInfo> tab = host_->FolioFindTab(tab_id);
  StartSave(tab_id, *tab, Trigger::kManual, std::string());
}

void PhiFolio::StartSave(int tab_id,
                         const TabInfo& tab,
                         Trigger trigger,
                         const std::string& action) {
  const std::string profile = host_->FolioProfileOfWindow(tab.window_id);
  in_flight_tabs_.insert(tab_id);
  PinFolder(tab_id, profile);
  const std::string save_id =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  // The page as it was at the trigger: with these the extension never has
  // to look at the tab, which may close or move on the moment Save is hit.
  base::DictValue payload;
  payload.Set("tabId", tab_id);
  payload.Set("requestId", save_id);
  payload.Set("trigger", trigger == Trigger::kManual ? std::string("manual")
                                                     : action);
  payload.Set("isAutomatic", trigger == Trigger::kSiteAction);
  payload.Set("highlightBlocks", base::ListValue());
  payload.Set("url", tab.url);
  payload.Set("pageTitle", tab.title);
  payload.Set("windowId", static_cast<double>(tab.window_id));
  pending_saves_[save_id] = {tab_id, tab.window_id, profile,
                             ::GetTickCount64() + kSaveAcceptMs};
  LOG(INFO) << "[Folio] save requested tab=" << tab_id << " trigger="
            << (trigger == Trigger::kManual ? "manual" : action);
  if (!host_->BroadcastToExtensions("saveForLater.save",
                                    ToJson(base::Value(std::move(payload))))) {
    // Nobody received it: say so now rather than after the budget.
    SettleSave(save_id, false);
    return;
  }
  UpdateTimer();
}

void PhiFolio::SettleSave(const std::string& save_id, bool accepted) {
  const auto it = pending_saves_.find(save_id);
  if (it == pending_saves_.end()) {
    return;
  }
  const PendingSave save = it->second;
  pending_saves_.erase(it);
  in_flight_tabs_.erase(save.tab_id);
  UpdateTimer();
  if (accepted) {
    LOG(INFO) << "[Folio] save accepted by the extension";
    return;
  }
  // There is deliberately no native save to fall back to: one pipeline, in
  // the half that ships with the extension updater.
  LOG(WARNING) << "[Folio] no extension took the save";
  ShowToast(save.window_id, PhiStr("browser.folio.failedToast"),
            LastComponent(FolderForProfile(save.profile_id)));
}

void PhiFolio::OpenLibrary() {
  if (Enabled()) {
    host_->FolioOpenTab(std::string("chrome-extension://") +
                        PhiReaderBridge::extension_id() + "/library.html");
  }
}

void PhiFolio::OpenAutoSaveSites() {
  if (Enabled()) {
    host_->FolioOpenTab(std::string("chrome-extension://") +
                        PhiReaderBridge::extension_id() + "/triggers.html");
  }
}

void PhiFolio::BroadcastArmedState() {
  const bool armed = AutoTriggerArmed();
  LOG(INFO) << "[Folio] broadcasting armed=" << armed
            << " enabled=" << Enabled();
  const std::vector<int64_t> windows = host_->FolioWindowPerProfile();
  if (windows.empty()) {
    host_->BroadcastToExtensions(
        "saveForLater.armedChanged",
        std::string("{\"armed\":") + (armed ? "true" : "false") +
            ",\"enabled\":" + (Enabled() ? "true" : "false") + "}");
    return;
  }
  for (const int64_t window : windows) {
    base::DictValue payload;
    payload.Set("armed", armed);
    payload.Set("enabled", Enabled());
    payload.Set("windowId", static_cast<double>(window));
    host_->BroadcastToExtensions("saveForLater.armedChanged",
                                 ToJson(base::Value(std::move(payload))));
  }
}

// --- Folders -----------------------------------------------------------------

std::wstring PhiFolio::FolderForProfile(const std::string& profile_id) const {
  return base::UTF8ToWide(
      host_->FolioPreferences().FolioFolderForProfile(profile_id));
}

void PhiFolio::PinFolder(int tab_id, const std::string& profile_id) {
  const ULONGLONG now = ::GetTickCount64();
  std::erase_if(pinned_folders_, [now](const auto& entry) {
    return now - entry.second.second >= kPinnedFolderMs;
  });
  pinned_folders_[tab_id] = {FolderForProfile(profile_id), now};
}

// The folder a request belongs to. A broker call names the tab it acts for,
// so the folder is that tab's: the save's pinned answer first (it survives
// the tab closing), then the tab's live window, and only then the active
// window for a request that named no tab.
std::wstring PhiFolio::FolderForTab(std::optional<int> tab_id) const {
  if (tab_id) {
    const auto pinned = pinned_folders_.find(*tab_id);
    if (pinned != pinned_folders_.end() &&
        ::GetTickCount64() - pinned->second.second < kPinnedFolderMs) {
      return pinned->second.first;
    }
    if (std::optional<TabInfo> tab = host_->FolioFindTab(*tab_id)) {
      return FolderForProfile(host_->FolioProfileOfWindow(tab->window_id));
    }
  }
  return FolderForProfile(
      host_->FolioProfileOfWindow(host_->FolioActiveWindow()));
}

int64_t PhiFolio::WindowForTab(std::optional<int> tab_id) const {
  if (tab_id) {
    if (std::optional<TabInfo> tab = host_->FolioFindTab(*tab_id)) {
      return tab->window_id;
    }
  }
  return host_->FolioActiveWindow();
}

// --- Messages ----------------------------------------------------------------

bool PhiFolio::HandleExtensionMessage(const std::string& request_id,
                                      const std::string& sender_id,
                                      const std::string& type,
                                      const std::string& payload_json) {
  if (type.rfind(kPrefix, 0) != 0) {
    return false;
  }
  const std::string verb = type.substr(sizeof(kPrefix) - 1);
  // Every extension shares this channel: only Mirage speaks for Folio, and
  // nothing is served while Folio is off. Still answered, so the sender
  // does not wait out its timeout.
  if (sender_id != PhiReaderBridge::extension_id() || !Enabled()) {
    LOG(WARNING) << "[Folio] " << type << " refused: sender=" << sender_id
                 << " enabled=" << Enabled();
    if (verb == "getArmed") {
      host_->RespondToExtension(request_id,
                                "{\"armed\":false,\"enabled\":false}");
    } else if (verb == "saveResult") {
      host_->RespondToExtension(request_id, "{\"accepted\":false}");
    } else if (verb == "videoArticles") {
      host_->RespondToExtension(request_id, "{\"available\":false}");
    } else {
      host_->RespondToExtension(request_id, ErrorJson("unavailable"));
    }
    return true;
  }
  if (verb == "getArmed") {
    OnGetArmed(request_id);
  } else if (verb == "trigger") {
    OnTrigger(request_id, payload_json);
  } else if (verb == "saveResult") {
    OnSaveResult(request_id, payload_json);
  } else if (verb == "toast") {
    OnToast(request_id, payload_json);
  } else if (verb == "fs.list") {
    OnList(request_id, payload_json);
  } else if (verb == "fs.read") {
    OnRead(request_id, payload_json);
  } else if (verb == "fs.writeText") {
    OnWriteText(request_id, payload_json);
  } else if (verb == "fs.writeBegin") {
    OnWriteBegin(request_id, payload_json);
  } else if (verb == "fs.writeChunk") {
    OnWriteChunk(request_id, payload_json);
  } else if (verb == "fs.writeEnd") {
    OnWriteEnd(request_id, payload_json);
  } else if (verb == "fs.rename") {
    OnRename(request_id, payload_json);
  } else if (verb == "delete") {
    OnDelete(request_id, payload_json);
  } else if (verb == "reveal") {
    OnReveal(request_id, payload_json);
  } else if (verb == "highlight") {
    OnLegacyHighlight(request_id);
  } else if (verb == "videoArticles") {
    // Video articles are Phi AI, which is not on Windows: Mirage keeps a
    // video as its link and webpage copy and does not offer the import.
    host_->RespondToExtension(request_id, "{\"available\":false}");
  } else if (verb == "videoGist") {
    host_->RespondToExtension(request_id, ErrorJson("unavailable"));
  } else {
    LOG(WARNING) << "[Folio] unknown message " << type;
    host_->RespondToExtension(request_id, ErrorJson("unknown_type"));
  }
  return true;
}

void PhiFolio::OnGetArmed(const std::string& request_id) {
  base::DictValue reply;
  reply.Set("armed", AutoTriggerArmed());
  // `enabled` is the master flag: Mirage keys its own surfaces (the
  // highlight context menu) off it.
  reply.Set("enabled", Enabled());
  host_->RespondToExtension(request_id, ToJson(base::Value(std::move(reply))));
}

// A site-action activation reported by Mirage. The app is the authority:
// the armed check runs again here so a stale extension cache can never
// write files past the gate, and a tab that has moved on is not saved.
void PhiFolio::OnTrigger(const std::string& request_id,
                         const std::string& payload_json) {
  host_->RespondToExtension(request_id, "{}");
  std::optional<base::DictValue> payload =
      base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
  const std::optional<int> tab_id =
      payload ? OptionalTabId(*payload) : std::nullopt;
  if (!payload || !tab_id) {
    LOG(INFO) << "[Folio] trigger dropped: bad payload";
    return;
  }
  if (!AutoTriggerArmed()) {
    LOG(INFO) << "[Folio] trigger dropped: disarmed";
    return;
  }
  const std::string url = StringOr(*payload, "url");
  const std::optional<TabInfo> tab = host_->FolioFindTab(*tab_id);
  if (!tab || tab->url != url) {
    LOG(INFO) << "[Folio] trigger dropped: the tab moved on";
    return;
  }
  const ULONGLONG now = ::GetTickCount64();
  std::erase_if(recent_auto_saves_, [now](const auto& entry) {
    return now - entry.second >= kAutoSaveDedupMs;
  });
  if (recent_auto_saves_.count(url) || !CanSave(*tab_id)) {
    return;
  }
  recent_auto_saves_[url] = now;
  StartSave(*tab_id, *tab, Trigger::kSiteAction, StringOr(*payload, "action"));
}

// The reply to a save request. An acknowledgement that arrives after the
// budget finds the user already told the save failed, so it is refused and
// Mirage stands down rather than writing an item the toast said was not
// saved.
void PhiFolio::OnSaveResult(const std::string& request_id,
                            const std::string& payload_json) {
  std::optional<base::DictValue> payload =
      base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
  const std::string save_id = payload ? StringOr(*payload, "requestId") : "";
  if (save_id.empty() || !pending_saves_.count(save_id)) {
    LOG(WARNING) << "[Folio] save acknowledged after its budget; refused";
    host_->RespondToExtension(request_id, "{\"accepted\":false}");
    return;
  }
  const bool ok = payload->FindBool("ok").value_or(false);
  if (!ok) {
    LOG(INFO) << "[Folio] extension save failed: "
              << StringOr(*payload, "error");
  }
  host_->RespondToExtension(request_id, "{\"accepted\":true}");
  SettleSave(save_id, ok);
}

void PhiFolio::OnToast(const std::string& request_id,
                       const std::string& payload_json) {
  host_->RespondToExtension(request_id, "{}");
  std::optional<base::DictValue> payload =
      base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
  if (!payload) {
    return;
  }
  // The outcome is NAMED by the extension and WORDED here: toasts are window
  // chrome, and the extension carries no translation for the browser's
  // locale.
  const char* title = LookUp(kToastTitles, StringOr(*payload, "titleKey"));
  if (!title) {
    LOG(INFO) << "[Folio] toast dropped: unknown outcome "
              << StringOr(*payload, "titleKey");
    return;
  }
  std::wstring message;
  if (const std::string* reason = payload->FindString("reason")) {
    // An unrecognised code says nothing rather than leaking an identifier.
    if (const char* worded = LookUp(kToastReasons, *reason)) {
      message = PhiStr(worded);
    }
  } else {
    message = base::UTF8ToWide(StringOr(*payload, "message"));
  }
  const std::optional<int> tab_id = OptionalTabId(*payload);
  const int64_t window = WindowForTab(tab_id);
  const std::string note_basename = StringOr(*payload, "noteBasename");
  const std::string note_block = StringOr(*payload, "noteBlock");
  if (!note_basename.empty() && payload->FindString("noteBlock")) {
    host_->FolioShowToast(
        window, PhiStr(title), message, 6000,
        PhiStr("browser.folio.addNoteAction"),
        [this, window, note_basename, note_block, tab_id] {
          PromptNote(window, note_basename, note_block, tab_id);
        });
    return;
  }
  const std::string undo = StringOr(*payload, "undoBasename");
  if (undo.empty() || !PhiFolioBrokerFileName(undo + ".md")) {
    ShowToast(window, PhiStr(title), message);
    return;
  }
  const std::wstring folder = FolderForTab(tab_id);
  host_->FolioShowToast(window, PhiStr(title), message, 6000,
                        PhiStr("browser.folio.undoAction"),
                        [this, folder, undo] {
                          RunOnWorker([folder, undo] {
                            RecyclePair(folder, undo);
                          });
                        });
}

void PhiFolio::OnLegacyHighlight(const std::string& request_id) {
  // The shape an OLD Mirage sends, from before the extension owned
  // highlights. A user action must never be a silent no-op.
  host_->RespondToExtension(request_id, "{}");
  LOG(WARNING) << "[Folio] legacy saveForLater.highlight from an old Mirage";
  ShowToast(host_->FolioActiveWindow(),
            PhiStr("browser.folio.staleExtensionToast"), std::wstring());
}

// --- Broker: reading ---------------------------------------------------------

void PhiFolio::OnList(const std::string& request_id,
                      const std::string& payload_json) {
  std::optional<base::DictValue> payload =
      base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
  const std::wstring folder =
      FolderForTab(payload ? OptionalTabId(*payload) : std::nullopt);
  RunOnWorker([this, folder, request_id] {
    std::string reply = ListFolder(folder);
    PostToUi([this, request_id, reply = std::move(reply)] {
      host_->RespondToExtension(request_id, reply);
    });
  });
}

void PhiFolio::OnRead(const std::string& request_id,
                      const std::string& payload_json) {
  std::optional<base::DictValue> payload =
      base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
  const std::string name = payload ? StringOr(*payload, "name") : "";
  if (!PhiFolioBrokerFileName(name)) {
    host_->RespondToExtension(request_id, ErrorJson("invalid"));
    return;
  }
  const std::wstring path = Join(FolderForTab(OptionalTabId(*payload)), name);
  const int64_t offset = std::max<int64_t>(
      0, static_cast<int64_t>(payload->FindDouble("offset").value_or(0)));
  const size_t length = std::clamp<size_t>(
      static_cast<size_t>(std::max(
          1.0, payload->FindDouble("length").value_or(kReadChunkBytes))),
      1, kReadChunkBytes);
  RunOnWorker([this, path, offset, length, request_id] {
    std::string reply = ReadChunk(path, offset, length);
    PostToUi([this, request_id, reply = std::move(reply)] {
      host_->RespondToExtension(request_id, reply);
    });
  });
}

// --- Broker: writing ---------------------------------------------------------

// A whole small file in one message — Markdown documents, far under the
// bridge's cap. Archives stream through writeBegin/Chunk/End instead.
void PhiFolio::OnWriteText(const std::string& request_id,
                           const std::string& payload_json) {
  std::optional<base::DictValue> payload =
      base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
  const std::string name = payload ? StringOr(*payload, "name") : "";
  const std::string* text = payload ? payload->FindString("text") : nullptr;
  if (!PhiFolioBrokerFileName(name) || !text) {
    host_->RespondToExtension(request_id, ErrorJson("invalid"));
    return;
  }
  const std::wstring folder = FolderForTab(OptionalTabId(*payload));
  RunOnWorker([this, folder, name, text = *text, request_id] {
    const bool ok =
        EnsureFolder(folder) && PhiWriteFileAtomic(Join(folder, name), text);
    if (!ok) {
      LOG(WARNING) << "[Folio] writeText failed, err=" << ::GetLastError();
    }
    PostToUi([this, request_id, ok] {
      host_->RespondToExtension(request_id,
                                ok ? kOk : ErrorJson("write_failed"));
    });
  });
}

void PhiFolio::OnWriteBegin(const std::string& request_id,
                            const std::string& payload_json) {
  std::optional<base::DictValue> payload =
      base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
  const std::string name = payload ? StringOr(*payload, "name") : "";
  if (!PhiFolioBrokerFileName(name)) {
    host_->RespondToExtension(request_id, ErrorJson("invalid"));
    return;
  }
  const std::wstring folder = FolderForTab(OptionalTabId(*payload));
  const std::string token = base::Uuid::GenerateRandomV4().AsLowercaseString();
  // A leading dot keeps the part file out of every listing.
  const std::wstring part = folder + L"\\.folio-part-" + base::UTF8ToWide(token);
  std::wstring target = Join(folder, name);
  // The pair may have been renamed while the page was still being
  // captured, before this write began.
  if (EndsWithW(target, L".mhtml")) {
    target = CurrentArchivePath(target);
  }
  RunOnWorker([this, folder, part, target, token, request_id] {
    HANDLE handle = INVALID_HANDLE_VALUE;
    if (EnsureFolder(folder)) {
      handle = ::CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    }
    PostToUi([this, handle, part, target, token, request_id] {
      if (handle == INVALID_HANDLE_VALUE) {
        LOG(WARNING) << "[Folio] broker begin failed";
        host_->RespondToExtension(request_id, ErrorJson("write_failed"));
        return;
      }
      BrokerWrite write;
      write.handle = handle;
      write.part = part;
      write.final_path = target;
      write.touched = ::GetTickCount64();
      broker_writes_[token] = std::move(write);
      UpdateTimer();
      base::DictValue reply;
      reply.Set("token", token);
      host_->RespondToExtension(request_id,
                                ToJson(base::Value(std::move(reply))));
    });
  });
}

void PhiFolio::OnWriteChunk(const std::string& request_id,
                            const std::string& payload_json) {
  std::optional<base::DictValue> payload =
      base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
  const std::string token = payload ? StringOr(*payload, "token") : "";
  std::string bytes;
  if (token.empty() || !payload->FindString("data") ||
      !base::Base64Decode(StringOr(*payload, "data"), &bytes)) {
    host_->RespondToExtension(request_id, ErrorJson("invalid"));
    return;
  }
  const auto it = broker_writes_.find(token);
  if (it == broker_writes_.end()) {
    host_->RespondToExtension(request_id, ErrorJson("unknown_token"));
    return;
  }
  // Touched before the write, so the sweep cannot close a handle the worker
  // is still writing through.
  it->second.touched = ::GetTickCount64();
  const HANDLE handle = it->second.handle;
  RunOnWorker([this, handle, bytes = std::move(bytes), token, request_id] {
    DWORD written = 0;
    const bool ok =
        ::WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()),
                    &written, nullptr) &&
        written == bytes.size();
    PostToUi([this, ok, token, request_id] {
      const auto write = broker_writes_.find(token);
      if (write == broker_writes_.end()) {
        host_->RespondToExtension(request_id, ErrorJson("unknown_token"));
        return;
      }
      if (ok) {
        write->second.touched = ::GetTickCount64();
        host_->RespondToExtension(request_id, kOk);
        return;
      }
      const BrokerWrite failed = write->second;
      broker_writes_.erase(write);
      UpdateTimer();
      RunOnWorker([failed] {
        ::CloseHandle(failed.handle);
        ::DeleteFileW(failed.part.c_str());
      });
      host_->RespondToExtension(request_id, ErrorJson("write_failed"));
    });
  });
}

void PhiFolio::OnWriteEnd(const std::string& request_id,
                          const std::string& payload_json) {
  std::optional<base::DictValue> payload =
      base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
  const std::string token = payload ? StringOr(*payload, "token") : "";
  const auto it = broker_writes_.find(token);
  if (it == broker_writes_.end()) {
    host_->RespondToExtension(
        request_id, ErrorJson(token.empty() ? "invalid" : "unknown_token"));
    return;
  }
  const BrokerWrite write = it->second;
  broker_writes_.erase(it);
  UpdateTimer();
  finishing_archives_.insert(write.final_path);
  RunOnWorker([this, write, request_id] {
    ::CloseHandle(write.handle);
    // Replace rather than remove-then-move: between the two a re-save would
    // have deleted the copy the user had and not yet written the new one.
    const bool ok = ::MoveFileExW(write.part.c_str(), write.final_path.c_str(),
                                  MOVEFILE_REPLACE_EXISTING |
                                      MOVEFILE_WRITE_THROUGH);
    if (ok) {
      // The part file was hidden; the landed copy is an ordinary item.
      ::SetFileAttributesW(write.final_path.c_str(), FILE_ATTRIBUTE_NORMAL);
    } else {
      LOG(WARNING) << "[Folio] broker end failed, err=" << ::GetLastError();
      ::DeleteFileW(write.part.c_str());
    }
    PostToUi([this, write, ok, request_id] {
      finishing_archives_.erase(write.final_path);
      if (ok && EndsWithW(write.final_path, L".mhtml")) {
        // The pair was renamed while this copy was landing.
        const std::wstring settled = CurrentArchivePath(write.final_path);
        if (settled != write.final_path) {
          RunOnWorker([from = write.final_path, settled] {
            MoveLandedArchive(from, settled);
          });
        }
      }
      host_->RespondToExtension(request_id,
                                ok ? kOk : ErrorJson("write_failed"));
    });
  });
}

// Moves a pair to a new basename (the video title promotion, a highlight's
// rename). Replies with the basename in effect afterwards — the old one when
// the move failed.
void PhiFolio::OnRename(const std::string& request_id,
                        const std::string& payload_json) {
  std::optional<base::DictValue> payload =
      base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
  const std::string from = payload ? StringOr(*payload, "from") : "";
  const std::string to = payload ? StringOr(*payload, "to") : "";
  if (!PhiFolioBrokerFileName(from + ".md") ||
      !PhiFolioBrokerFileName(to + ".md")) {
    host_->RespondToExtension(request_id, ErrorJson("invalid"));
    return;
  }
  const std::wstring folder = FolderForTab(OptionalTabId(*payload));
  RunOnWorker([this, folder, from, to, request_id] {
    std::string landed = RenamePair(folder, from, to);
    PostToUi([this, folder, from, landed = std::move(landed), request_id] {
      if (landed != from) {
        RecordArchiveRename(Join(folder, from + ".mhtml"),
                            Join(folder, landed + ".mhtml"));
      }
      base::DictValue reply;
      reply.Set("basename", landed);
      host_->RespondToExtension(request_id,
                                ToJson(base::Value(std::move(reply))));
    });
  });
}

std::wstring PhiFolio::CurrentArchivePath(const std::wstring& path) const {
  std::wstring current = path;
  const ULONGLONG now = ::GetTickCount64();
  for (int hop = 0; hop < 4; ++hop) {
    const auto renamed = renamed_archives_.find(current);
    // Markdown back at the old name is a later save that claimed the freed
    // name and keeps its own copy.
    if (renamed == renamed_archives_.end() ||
        now - renamed->second.at >= kRenamedArchiveMs ||
        Exists(WithExtension(current, L".md"))) {
      break;
    }
    current = renamed->second.to;
  }
  return current;
}

void PhiFolio::RecordArchiveRename(const std::wstring& from,
                                   const std::wstring& to) {
  const ULONGLONG now = ::GetTickCount64();
  std::erase_if(renamed_archives_, [now](const auto& entry) {
    return now - entry.second.at >= kRenamedArchiveMs;
  });
  renamed_archives_[from] = {to, now};
  // A copy still streaming lands under the new name.
  for (auto& [token, write] : broker_writes_) {
    if (write.final_path == from) {
      write.final_path = to;
    }
  }
  // One landing right now is moved by writeEnd once it has; one that landed
  // between the rename and now is moved here.
  if (!finishing_archives_.count(from)) {
    RunOnWorker([from, to] { MoveLandedArchive(from, to); });
  }
}

// --- Library actions ---------------------------------------------------------

// Delete means delete: the library page arms its button before acting, and
// that confirmation, not the Recycle Bin, stands between a mis-click and a
// lost item.
void PhiFolio::OnDelete(const std::string& request_id,
                        const std::string& payload_json) {
  std::optional<base::DictValue> payload =
      base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
  const std::string basename = payload ? StringOr(*payload, "basename") : "";
  if (!PhiFolioBrokerFileName(basename + ".md")) {
    host_->RespondToExtension(request_id, ErrorJson("invalid"));
    return;
  }
  const std::wstring folder = FolderForTab(OptionalTabId(*payload));
  RunOnWorker([this, folder, basename, request_id] {
    for (const char* ext : {".md", ".mhtml"}) {
      PhiRemoveFileIfPresent(Join(folder, basename + ext));
    }
    PostToUi([this, request_id] {
      host_->RespondToExtension(request_id, kOk);
    });
  });
}

void PhiFolio::OnReveal(const std::string& request_id,
                        const std::string& payload_json) {
  host_->RespondToExtension(request_id, "{}");
  std::optional<base::DictValue> payload =
      base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
  const std::string basename = payload ? StringOr(*payload, "basename") : "";
  if (!PhiFolioBrokerFileName(basename + ".md")) {
    return;
  }
  const std::wstring path =
      Join(FolderForTab(OptionalTabId(*payload)), basename + ".md");
  if (!Exists(path)) {
    return;
  }
  // The pair in Explorer, the Markdown selected (the Mac's Finder reveal).
  const std::wstring args = L"/select,\"" + path + L"\"";
  ::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr,
                  SW_SHOWNORMAL);
}

// --- Chrome ------------------------------------------------------------------

void PhiFolio::ShowToast(int64_t window_id,
                         const std::wstring& title,
                         const std::wstring& message) {
  host_->FolioShowToast(window_id, title, message, 3000, std::wstring(),
                        nullptr);
}

// The Add Note toast action. The prompt is native because the toast is;
// the write goes back to Mirage, so the extension stays the only writer of a
// saved item — through the same broker and the same tab, or the note would
// resolve its folder from whichever window is in front.
void PhiFolio::PromptNote(int64_t window_id,
                          const std::string& basename,
                          const std::string& block,
                          std::optional<int> tab_id) {
  std::wstring note;
  if (!PhiPromptText(host_->FolioShellForWindow(window_id),
                     PhiStr("browser.folio.addNoteTitle"),
                     PhiStr("browser.folio.addNoteMessage"), &note,
                     PhiStr("browser.folio.addNoteConfirmButton"),
                     PhiTextRules{true, 0})) {
    return;
  }
  base::DictValue payload;
  payload.Set("basename", basename);
  payload.Set("block", block);
  payload.Set("note", "**" + base::WideToUTF8(PhiStr("browser.folio.noteLabel")) +
                          ":** " + base::WideToUTF8(note));
  if (tab_id) {
    payload.Set("tabId", *tab_id);
  }
  payload.Set("windowId", static_cast<double>(window_id));
  const bool sent = host_->BroadcastToExtensions(
      "saveForLater.addNote", ToJson(base::Value(std::move(payload))));
  if (!sent) {
    LOG(WARNING) << "[Folio] note not delivered: no extension reachable";
  }
}

// --- Threads -----------------------------------------------------------------

void PhiFolio::RunOnWorker(std::function<void()> job) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;
    }
    worker_jobs_.push_back(std::move(job));
  }
  ::SetEvent(worker_wake_);
}

void PhiFolio::PostToUi(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;
    }
    ui_tasks_.push_back(std::move(task));
  }
  ::PostMessageW(msg_hwnd_, WM_PHI_FOLIO_DRAIN, 0, 0);
}

void PhiFolio::DrainUi() {
  while (true) {
    std::function<void()> task;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (ui_tasks_.empty()) {
        return;
      }
      task = std::move(ui_tasks_.front());
      ui_tasks_.pop_front();
    }
    task();
  }
}

void PhiFolio::WorkerLoop() {
  while (true) {
    std::function<void()> job;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) {
        return;
      }
      if (!worker_jobs_.empty()) {
        job = std::move(worker_jobs_.front());
        worker_jobs_.pop_front();
      }
    }
    if (job) {
      job();
    } else {
      ::WaitForSingleObject(worker_wake_, INFINITE);
    }
  }
}

void PhiFolio::UpdateTimer() {
  const bool want = !pending_saves_.empty() || !broker_writes_.empty();
  if (want == timer_running_ || !msg_hwnd_) {
    return;
  }
  timer_running_ = want;
  if (want) {
    ::SetTimer(msg_hwnd_, kTickTimer, kTickMs, nullptr);
  } else {
    ::KillTimer(msg_hwnd_, kTickTimer);
  }
}

void PhiFolio::OnTick() {
  const ULONGLONG now = ::GetTickCount64();
  std::vector<std::string> expired;
  for (const auto& [id, save] : pending_saves_) {
    if (now >= save.deadline) {
      expired.push_back(id);
    }
  }
  for (const std::string& id : expired) {
    SettleSave(id, false);  // Silence: no Mirage is listening.
  }
  // An upload the extension abandoned mid-stream, once it has actually gone
  // quiet for the whole idle window.
  for (auto it = broker_writes_.begin(); it != broker_writes_.end();) {
    if (now - it->second.touched < kBrokerIdleMs) {
      ++it;
      continue;
    }
    LOG(INFO) << "[Folio] broker write swept";
    RunOnWorker([write = it->second] {
      ::CloseHandle(write.handle);
      ::DeleteFileW(write.part.c_str());
    });
    it = broker_writes_.erase(it);
  }
  UpdateTimer();
}

// static
LRESULT CALLBACK PhiFolio::WndProc(HWND hwnd,
                                   UINT message,
                                   WPARAM wparam,
                                   LPARAM lparam) {
  auto* self =
      reinterpret_cast<PhiFolio*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self && message == WM_PHI_FOLIO_DRAIN) {
    self->DrainUi();
    return 0;
  }
  if (self && message == WM_TIMER && wparam == kTickTimer) {
    self->OnTick();
    return 0;
  }
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace phi_client

// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_update_cache.h"

#include <windows.h>

#include <shlobj.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include <openssl/curve25519.h>

#include "base/logging.h"

namespace phi_client {

namespace {

constexpr wchar_t kRecordName[] = L"update.txt";
// The record is a few short lines; anything longer is not ours.
constexpr DWORD kMaxRecordBytes = 4096;

std::string DecodeEntities(const std::string& in) {
  static constexpr std::pair<const char*, char> kEntities[] = {
      {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
      {"&quot;", '"'}, {"&apos;", '\''}};
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size();) {
    bool replaced = false;
    if (in[i] == '&') {
      for (const auto& [name, ch] : kEntities) {
        const size_t n = std::strlen(name);
        if (in.compare(i, n, name) == 0) {
          out.push_back(ch);
          i += n;
          replaced = true;
          break;
        }
      }
    }
    if (!replaced) {
      out.push_back(in[i++]);
    }
  }
  return out;
}

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string Trim(const std::string& s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && IsSpace(s[b])) {
    ++b;
  }
  while (e > b && IsSpace(s[e - 1])) {
    --e;
  }
  return s.substr(b, e - b);
}

// `name="value"` (or single quotes) inside one start tag.
std::string Attribute(const std::string& tag, const char* name) {
  const std::string key = std::string(name) + "=";
  for (size_t at = tag.find(key); at != std::string::npos;
       at = tag.find(key, at + 1)) {
    if (at == 0 || !IsSpace(tag[at - 1])) {
      continue;  // "os=" inside "sparkle:os=" — not this attribute.
    }
    const size_t open = at + key.size();
    if (open >= tag.size() || (tag[open] != '"' && tag[open] != '\'')) {
      continue;
    }
    const size_t close = tag.find(tag[open], open + 1);
    if (close == std::string::npos) {
      return std::string();
    }
    return DecodeEntities(tag.substr(open + 1, close - open - 1));
  }
  return std::string();
}

// The text of the first <name>…</name> inside `block`.
std::string ElementText(const std::string& block, const char* name) {
  const std::string open = std::string("<") + name + ">";
  const std::string close = std::string("</") + name + ">";
  const size_t b = block.find(open);
  if (b == std::string::npos) {
    return std::string();
  }
  const size_t e = block.find(close, b + open.size());
  if (e == std::string::npos) {
    return std::string();
  }
  return Trim(DecodeEntities(block.substr(b + open.size(),
                                          e - b - open.size())));
}

bool IsTagEnd(const std::string& s, size_t at) {
  return at < s.size() && (s[at] == '>' || s[at] == '/' || IsSpace(s[at]));
}

bool Base64Decode(const std::string& in, std::vector<uint8_t>* out) {
  out->clear();
  uint32_t acc = 0;
  int bits = 0;
  size_t pad = 0;
  for (const char c : in) {
    int v;
    if (c >= 'A' && c <= 'Z') {
      v = c - 'A';
    } else if (c >= 'a' && c <= 'z') {
      v = c - 'a' + 26;
    } else if (c >= '0' && c <= '9') {
      v = c - '0' + 52;
    } else if (c == '+') {
      v = 62;
    } else if (c == '/') {
      v = 63;
    } else if (c == '=') {
      ++pad;
      continue;
    } else if (IsSpace(c)) {
      continue;
    } else {
      return false;
    }
    if (pad) {
      return false;  // Data after padding.
    }
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(static_cast<uint8_t>((acc >> bits) & 0xff));
    }
  }
  return pad <= 2;
}

// Versions name the cached file: digits, letters, dots and dashes only.
bool IsSafeVersion(const std::string& version) {
  if (version.empty() || version.size() > 64) {
    return false;
  }
  for (const char c : version) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
          (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_')) {
      return false;
    }
  }
  return version.find("..") == std::string::npos;
}

std::wstring Widen(const std::string& ascii) {
  return std::wstring(ascii.begin(), ascii.end());
}

std::wstring InstallerPath(const std::wstring& dir,
                           const std::string& version) {
  return dir + L"\\PhiBrowserSetup-" + Widen(version) + L".exe";
}

bool FileLength(const std::wstring& path, uint64_t* length) {
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
      (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
    return false;
  }
  *length = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
            data.nFileSizeLow;
  return true;
}

}  // namespace

std::optional<PhiUpdateEnclosure> PhiParseAppcast(const std::string& xml,
                                                  const char* os_tag) {
  std::optional<PhiUpdateEnclosure> best;
  for (size_t item = xml.find("<item"); item != std::string::npos;
       item = xml.find("<item", item + 1)) {
    if (!IsTagEnd(xml, item + 5)) {
      continue;
    }
    const size_t item_end = xml.find("</item>", item);
    if (item_end == std::string::npos) {
      break;
    }
    const std::string block = xml.substr(item, item_end - item);
    const std::string item_version = ElementText(block, "sparkle:version");
    std::optional<PhiUpdateEnclosure> pick;
    int pick_score = 0;
    for (size_t at = block.find("<enclosure"); at != std::string::npos;
         at = block.find("<enclosure", at + 1)) {
      if (!IsTagEnd(block, at + 10)) {
        continue;
      }
      const size_t tag_end = block.find('>', at);
      if (tag_end == std::string::npos) {
        break;
      }
      const std::string tag = block.substr(at, tag_end - at);
      const std::string os = Attribute(tag, "sparkle:os");
      const int score = os == os_tag ? 3 : os == "windows" ? 2
                                         : os.empty()     ? 1
                                                          : 0;
      if (score <= pick_score) {
        continue;
      }
      PhiUpdateEnclosure e;
      e.url = Attribute(tag, "url");
      e.version = Attribute(tag, "sparkle:version");
      if (e.version.empty()) {
        e.version = item_version;
      }
      e.signature = Attribute(tag, "sparkle:edSignature");
      e.length = std::strtoull(Attribute(tag, "length").c_str(), nullptr, 10);
      if (e.url.empty() || e.version.empty()) {
        continue;
      }
      pick = std::move(e);
      pick_score = score;
    }
    if (pick && (!best ||
                 PhiCompareUpdateVersions(pick->version, best->version) > 0)) {
      best = std::move(pick);
    }
  }
  return best;
}

int PhiCompareUpdateVersions(const std::string& a, const std::string& b) {
  size_t i = 0;
  size_t j = 0;
  while (i < a.size() || j < b.size()) {
    const size_t ie = std::min(a.find('.', i), a.size());
    const size_t je = std::min(b.find('.', j), b.size());
    const std::string pa = i < a.size() ? a.substr(i, ie - i) : "0";
    const std::string pb = j < b.size() ? b.substr(j, je - j) : "0";
    const bool na = !pa.empty() &&
                    pa.find_first_not_of("0123456789") == std::string::npos;
    const bool nb = !pb.empty() &&
                    pb.find_first_not_of("0123456789") == std::string::npos;
    if (na && nb) {
      const unsigned long long va = std::strtoull(pa.c_str(), nullptr, 10);
      const unsigned long long vb = std::strtoull(pb.c_str(), nullptr, 10);
      if (va != vb) {
        return va < vb ? -1 : 1;
      }
    } else if (const int c = pa.compare(pb); c != 0) {
      return c < 0 ? -1 : 1;
    }
    i = ie + 1;
    j = je + 1;
  }
  return 0;
}

PhiUpdateCache::PhiUpdateCache(std::wstring dir) : dir_(std::move(dir)) {}

std::optional<PhiCachedUpdate> PhiUpdateCache::Read() const {
  if (dir_.empty()) {
    return std::nullopt;
  }
  const std::wstring record = dir_ + L"\\" + kRecordName;
  HANDLE h = ::CreateFileW(record.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }
  char buffer[kMaxRecordBytes] = {};
  DWORD read = 0;
  const BOOL ok = ::ReadFile(h, buffer, sizeof(buffer) - 1, &read, nullptr);
  ::CloseHandle(h);
  if (!ok) {
    return std::nullopt;
  }
  PhiCachedUpdate cached;
  const std::string text(buffer, read);
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) {
      end = text.size();
    }
    const std::string line = Trim(text.substr(start, end - start));
    start = end + 1;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    if (key == "version") {
      cached.version = value;
    } else if (key == "signature") {
      cached.signature = value;
    } else if (key == "length") {
      cached.length = std::strtoull(value.c_str(), nullptr, 10);
    }
  }
  if (!IsSafeVersion(cached.version) || cached.signature.empty() ||
      cached.length == 0) {
    return std::nullopt;
  }
  cached.file = InstallerPath(dir_, cached.version);
  uint64_t length = 0;
  if (!FileLength(cached.file, &length) || length != cached.length) {
    return std::nullopt;
  }
  return cached;
}

bool PhiUpdateCache::Store(const std::wstring& source,
                           const PhiUpdateEnclosure& enclosure) {
  if (dir_.empty() || !IsSafeVersion(enclosure.version) ||
      enclosure.signature.empty() || enclosure.length == 0 ||
      enclosure.signature.find_first_of("\r\n") != std::string::npos) {
    return false;
  }
  Clear();  // One installer at a time: a new one supersedes the old.
  const int made = ::SHCreateDirectoryExW(nullptr, dir_.c_str(), nullptr);
  if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS) {
    LOG(ERROR) << "[PhiUpdater] cannot create the update cache: " << made;
    return false;
  }
  const std::wstring file = InstallerPath(dir_, enclosure.version);
  const std::wstring part = file + L".part";
  uint64_t length = 0;
  if (!::CopyFileW(source.c_str(), part.c_str(), FALSE) ||
      !::MoveFileExW(part.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) ||
      !FileLength(file, &length) || length != enclosure.length) {
    LOG(ERROR) << "[PhiUpdater] cannot cache the installer: "
               << ::GetLastError();
    Clear();
    return false;
  }
  const std::string record = "version=" + enclosure.version +
                             "\nsignature=" + enclosure.signature +
                             "\nlength=" + std::to_string(enclosure.length) +
                             "\n";
  const std::wstring record_path = dir_ + L"\\" + kRecordName;
  HANDLE h = ::CreateFileW(record_path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  DWORD written = 0;
  const bool ok = h != INVALID_HANDLE_VALUE &&
                  ::WriteFile(h, record.data(),
                              static_cast<DWORD>(record.size()), &written,
                              nullptr) &&
                  written == record.size();
  if (h != INVALID_HANDLE_VALUE) {
    ::CloseHandle(h);
  }
  if (!ok) {
    Clear();
    return false;
  }
  return true;
}

void PhiUpdateCache::Clear() const {
  if (dir_.empty()) {
    return;
  }
  WIN32_FIND_DATAW found = {};
  HANDLE find = ::FindFirstFileW((dir_ + L"\\*").c_str(), &found);
  if (find == INVALID_HANDLE_VALUE) {
    return;  // No folder: nothing cached.
  }
  do {
    if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      ::DeleteFileW((dir_ + L"\\" + found.cFileName).c_str());
    }
  } while (::FindNextFileW(find, &found));
  ::FindClose(find);
  ::RemoveDirectoryW(dir_.c_str());
}

HANDLE PhiOpenVerifiedUpdate(const std::wstring& file,
                             uint64_t length,
                             const std::string& signature_b64,
                             const std::string& public_key_b64) {
  std::vector<uint8_t> signature;
  std::vector<uint8_t> key;
  if (!Base64Decode(signature_b64, &signature) || signature.size() != 64 ||
      !Base64Decode(public_key_b64, &key) || key.size() != 32) {
    LOG(ERROR) << "[PhiUpdater] cached installer: malformed signature or key";
    return INVALID_HANDLE_VALUE;
  }
  // Read sharing only: no writer, rename or delete until the caller closes
  // the handle after the installer started (CreateProcess's own open asks
  // only for read sharing).
  HANDLE h = ::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return INVALID_HANDLE_VALUE;
  }
  LARGE_INTEGER size = {};
  bool ok = ::GetFileSizeEx(h, &size) && size.QuadPart > 0 &&
            static_cast<uint64_t>(size.QuadPart) == length;
  if (ok) {
    HANDLE mapping =
        ::CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const void* view =
        mapping ? ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
    ok = view && ED25519_verify(static_cast<const uint8_t*>(view),
                                static_cast<size_t>(size.QuadPart),
                                signature.data(), key.data()) == 1;
    if (view) {
      ::UnmapViewOfFile(view);
    }
    if (mapping) {
      ::CloseHandle(mapping);
    }
  }
  if (!ok) {
    ::CloseHandle(h);
    return INVALID_HANDLE_VALUE;
  }
  return h;
}

}  // namespace phi_client

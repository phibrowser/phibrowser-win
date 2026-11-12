// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_local_store.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <climits>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <utility>

#include "client/phi_url_rule_normalize.h"
#include "third_party/sqlite/sqlite3.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace phi_client::store {

namespace {

// --- Environment ---------------------------------------------------------------

TimeMs WallNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// UUID().uuidString: a random (version 4) UUID, uppercase.
std::string RandomUuid() {
  static thread_local std::mt19937_64 rng([] {
    std::random_device device;
    return (static_cast<uint64_t>(device()) << 32) ^ device() ^
           static_cast<uint64_t>(WallNowMs());
  }());
  uint64_t hi = rng();
  uint64_t lo = rng();
  hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;  // Version 4.
  lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;  // RFC 4122.
  char text[37];
  std::snprintf(text, sizeof(text),
                "%08X-%04X-%04X-%04X-%012llX",
                static_cast<unsigned>(hi >> 32),
                static_cast<unsigned>((hi >> 16) & 0xFFFF),
                static_cast<unsigned>(hi & 0xFFFF),
                static_cast<unsigned>(lo >> 48),
                static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL));
  return text;
}

std::string LowercaseAscii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return s;
}

// --- Files (the manifest) ------------------------------------------------------

#if defined(_WIN32)
std::wstring Wide(const std::string& utf8) {
  if (utf8.empty()) {
    return std::wstring();
  }
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                      static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), n);
  return out;
}
#endif

#if defined(_WIN32)
bool ReadWholeFile(const std::string& path, std::string* out) {
  HANDLE file = ::CreateFileW(Wide(path).c_str(), GENERIC_READ,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  out->clear();
  std::string chunk(4096, '\0');
  DWORD read = 0;
  while (::ReadFile(file, chunk.data(), static_cast<DWORD>(chunk.size()),
                    &read, nullptr) &&
         read > 0) {
    out->append(chunk, 0, read);
  }
  ::CloseHandle(file);
  return true;
}

// Write-temp-then-replace, so a crash never leaves a truncated manifest.
bool WriteWholeFileAtomic(const std::string& path, const std::string& data) {
  const std::wstring temp = Wide(path + ".tmp");
  HANDLE file = ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD wrote = 0;
  const BOOL ok = ::WriteFile(file, data.data(),
                              static_cast<DWORD>(data.size()), &wrote,
                              nullptr);
  ::FlushFileBuffers(file);
  ::CloseHandle(file);
  if (!ok || wrote != data.size()) {
    ::DeleteFileW(temp.c_str());
    return false;
  }
  return ::MoveFileExW(temp.c_str(), Wide(path).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
#else
bool ReadWholeFile(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  out->assign(std::istreambuf_iterator<char>(in),
              std::istreambuf_iterator<char>());
  return true;
}

bool WriteWholeFileAtomic(const std::string& path, const std::string& data) {
  const std::string temp = path + ".tmp";
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(data.data(),
                             static_cast<std::streamsize>(data.size()))) {
      return false;
    }
  }
  return std::rename(temp.c_str(), path.c_str()) == 0;
}
#endif

// The manifest's activeStoreFormatVersion, or -1 when absent/unreadable. A
// tiny scan rather than a JSON dependency: the file is machine-written and
// this is the one number read from it.
int ManifestFormatVersion(const std::string& text) {
  const std::string key = "\"activeStoreFormatVersion\"";
  size_t at = text.find(key);
  if (at == std::string::npos) {
    return -1;
  }
  at = text.find(':', at + key.size());
  if (at == std::string::npos) {
    return -1;
  }
  ++at;
  while (at < text.size() && (text[at] == ' ' || text[at] == '\n' ||
                              text[at] == '\r' || text[at] == '\t')) {
    ++at;
  }
  int value = 0;
  bool any = false;
  while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
    value = value * 10 + (text[at] - '0');
    any = true;
    ++at;
  }
  return any ? value : -1;
}

// LocalStoreCompatibilityManifest, as the Mac writes it (sorted keys).
std::string ManifestJson(int version) {
  return "{\n  \"activeStoreFormatVersion\" : " + std::to_string(version) +
         ",\n  \"backups\" : [\n\n  ],\n  \"manifestVersion\" : 1\n}";
}

// --- SQLite ------------------------------------------------------------------

constexpr char kSchema[] = R"sql(
CREATE TABLE IF NOT EXISTS profile(
  profileId TEXT PRIMARY KEY,
  guid TEXT NOT NULL,
  displayName TEXT,
  bookmarkRootGuid TEXT,
  syncId TEXT,
  createdDate INTEGER);
CREATE TABLE IF NOT EXISTS tab(
  guid TEXT PRIMARY KEY,
  type INTEGER NOT NULL,
  title TEXT NOT NULL,
  idx INTEGER NOT NULL,
  url TEXT NOT NULL,
  favicon BLOB,
  faviconWidth INTEGER NOT NULL DEFAULT 0,
  faviconHeight INTEGER NOT NULL DEFAULT 0,
  createdDate INTEGER NOT NULL,
  updatedDate INTEGER NOT NULL,
  overrideTitle TEXT,
  isOpenned INTEGER NOT NULL DEFAULT 0,
  isCreatedByChromium INTEGER NOT NULL DEFAULT 0,
  needUpdateMetaData INTEGER NOT NULL DEFAULT 0,
  spaceId TEXT,
  profileId TEXT,
  source INTEGER NOT NULL DEFAULT 0,
  secondaryUrl TEXT,
  secondaryTitle TEXT,
  secondaryFavicon BLOB,
  secondaryFaviconWidth INTEGER NOT NULL DEFAULT 0,
  secondaryFaviconHeight INTEGER NOT NULL DEFAULT 0,
  splitPartnerGuid TEXT,
  splitExtraPanes TEXT,
  layout TEXT,
  lastSeen INTEGER,
  icon TEXT NOT NULL DEFAULT 'default',
  pinLineageId TEXT,
  isPinnedTabDormant INTEGER NOT NULL DEFAULT 0,
  syncId TEXT,
  contentUpdatedDate INTEGER,
  locationUpdatedDate INTEGER,
  parentGuid TEXT);
CREATE INDEX IF NOT EXISTS tab_parent ON tab(parentGuid);
CREATE TABLE IF NOT EXISTS space(
  spaceId TEXT PRIMARY KEY,
  profileId TEXT NOT NULL,
  name TEXT NOT NULL,
  colorHex TEXT NOT NULL,
  iconName TEXT NOT NULL,
  sortOrder INTEGER NOT NULL,
  createdDate INTEGER NOT NULL,
  updatedDate INTEGER NOT NULL,
  bookmarkRootGuid TEXT);
CREATE TABLE IF NOT EXISTS space_url_rule(
  id TEXT PRIMARY KEY,
  spaceId TEXT NOT NULL,
  host TEXT NOT NULL,
  pathPrefix TEXT,
  askBeforeRouting INTEGER NOT NULL DEFAULT 0,
  sortOrder INTEGER NOT NULL,
  createdDate INTEGER NOT NULL,
  syncId TEXT UNIQUE,
  contentUpdatedDate INTEGER,
  targetUpdatedDate INTEGER,
  deletedDate INTEGER,
  pendingLocalEdit INTEGER NOT NULL DEFAULT 0,
  mergePartnerSyncId TEXT);
CREATE TABLE IF NOT EXISTS browser_data_settings(
  id TEXT PRIMARY KEY,
  pinnedTabScopeRawValue TEXT NOT NULL);
)sql";

bool Exec(sqlite3* db, const char* sql) {
  return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

// A prepared statement with 1-based positional binds (in call order) and
// 0-based column reads.
class Stmt {
 public:
  Stmt(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
      stmt_ = nullptr;
    }
  }
  ~Stmt() { sqlite3_finalize(stmt_); }
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  bool ok() const { return stmt_ != nullptr; }

  Stmt& Text(const std::string& value) {
    sqlite3_bind_text(stmt_, ++bind_, value.data(),
                      static_cast<int>(value.size()), SQLITE_TRANSIENT);
    return *this;
  }
  Stmt& Text(const std::optional<std::string>& value) {
    if (!value) {
      sqlite3_bind_null(stmt_, ++bind_);
      return *this;
    }
    return Text(*value);
  }
  Stmt& Blob(const std::string& value) {
    if (value.empty()) {
      sqlite3_bind_null(stmt_, ++bind_);
    } else {
      sqlite3_bind_blob(stmt_, ++bind_, value.data(),
                        static_cast<int>(value.size()), SQLITE_TRANSIENT);
    }
    return *this;
  }
  Stmt& Int(int64_t value) {
    sqlite3_bind_int64(stmt_, ++bind_, value);
    return *this;
  }
  Stmt& Int(const std::optional<int64_t>& value) {
    if (!value) {
      sqlite3_bind_null(stmt_, ++bind_);
      return *this;
    }
    return Int(*value);
  }
  Stmt& Bool(bool value) { return Int(value ? 1 : 0); }

  // True on SQLITE_DONE (a write).
  bool Run() { return sqlite3_step(stmt_) == SQLITE_DONE; }
  // True while a row is available (a read).
  bool Next() { return sqlite3_step(stmt_) == SQLITE_ROW; }

  std::string ColText(int col) const {
    const unsigned char* text = sqlite3_column_text(stmt_, col);
    return text ? std::string(reinterpret_cast<const char*>(text),
                              static_cast<size_t>(
                                  sqlite3_column_bytes(stmt_, col)))
                : std::string();
  }
  std::optional<std::string> ColOptText(int col) const {
    if (sqlite3_column_type(stmt_, col) == SQLITE_NULL) {
      return std::nullopt;
    }
    return ColText(col);
  }
  std::string ColBlob(int col) const {
    const void* data = sqlite3_column_blob(stmt_, col);
    const int size = sqlite3_column_bytes(stmt_, col);
    return data && size > 0
               ? std::string(static_cast<const char*>(data),
                             static_cast<size_t>(size))
               : std::string();
  }
  int64_t ColInt(int col) const { return sqlite3_column_int64(stmt_, col); }
  std::optional<int64_t> ColOptInt(int col) const {
    if (sqlite3_column_type(stmt_, col) == SQLITE_NULL) {
      return std::nullopt;
    }
    return ColInt(col);
  }

 private:
  RAW_PTR_EXCLUSION sqlite3_stmt* stmt_ = nullptr;
  int bind_ = 0;
};

constexpr char kTabColumns[] =
    "guid, type, title, idx, url, favicon, faviconWidth, faviconHeight, "
    "createdDate, updatedDate, overrideTitle, isOpenned, isCreatedByChromium, "
    "needUpdateMetaData, spaceId, profileId, source, secondaryUrl, "
    "secondaryTitle, secondaryFavicon, secondaryFaviconWidth, "
    "secondaryFaviconHeight, splitPartnerGuid, splitExtraPanes, layout, "
    "lastSeen, icon, "
    "pinLineageId, isPinnedTabDormant, syncId, contentUpdatedDate, "
    "locationUpdatedDate, parentGuid";

Tab ReadTab(const Stmt& s) {
  Tab t;
  int c = 0;
  t.guid = s.ColText(c++);
  t.type = static_cast<RowType>(s.ColInt(c++));
  t.title = s.ColText(c++);
  t.index = static_cast<int>(s.ColInt(c++));
  t.url = s.ColText(c++);
  t.favicon = s.ColBlob(c++);
  t.favicon_width = static_cast<int>(s.ColInt(c++));
  t.favicon_height = static_cast<int>(s.ColInt(c++));
  t.created = s.ColInt(c++);
  t.updated = s.ColInt(c++);
  t.override_title = s.ColOptText(c++);
  t.is_opened = s.ColInt(c++) != 0;
  t.is_created_by_chromium = s.ColInt(c++) != 0;
  t.need_update_metadata = s.ColInt(c++) != 0;
  t.space_id = s.ColOptText(c++);
  t.profile_id = s.ColOptText(c++);
  t.source = static_cast<int>(s.ColInt(c++));
  t.secondary_url = s.ColOptText(c++);
  t.secondary_title = s.ColOptText(c++);
  t.secondary_favicon = s.ColBlob(c++);
  t.secondary_favicon_width = static_cast<int>(s.ColInt(c++));
  t.secondary_favicon_height = static_cast<int>(s.ColInt(c++));
  t.split_partner_guid = s.ColOptText(c++);
  t.split_extra_panes = s.ColOptText(c++);
  t.layout = s.ColOptText(c++);
  t.last_seen = s.ColOptInt(c++);
  t.icon = s.ColText(c++);
  t.pin_lineage_id = s.ColOptText(c++);
  t.pin_dormant = s.ColInt(c++) != 0;
  t.sync_id = s.ColOptText(c++);
  t.content_updated = s.ColOptInt(c++);
  t.location_updated = s.ColOptInt(c++);
  t.parent_guid = s.ColOptText(c++);
  return t;
}

bool WriteTab(sqlite3* db, const Tab& t) {
  const std::string sql = std::string("INSERT OR REPLACE INTO tab(") +
                          kTabColumns +
                          ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,"
                          "?,?,?,?,?,?,?,?,?,?,?,?,?)";
  Stmt s(db, sql.c_str());
  if (!s.ok()) {
    return false;
  }
  s.Text(t.guid)
      .Int(static_cast<int>(t.type))
      .Text(t.title)
      .Int(t.index)
      .Text(t.url)
      .Blob(t.favicon)
      .Int(t.favicon_width)
      .Int(t.favicon_height)
      .Int(t.created)
      .Int(t.updated)
      .Text(t.override_title)
      .Bool(t.is_opened)
      .Bool(t.is_created_by_chromium)
      .Bool(t.need_update_metadata)
      .Text(t.space_id)
      .Text(t.profile_id)
      .Int(t.source)
      .Text(t.secondary_url)
      .Text(t.secondary_title)
      .Blob(t.secondary_favicon)
      .Int(t.secondary_favicon_width)
      .Int(t.secondary_favicon_height)
      .Text(t.split_partner_guid)
      .Text(t.split_extra_panes)
      .Text(t.layout)
      .Int(t.last_seen)
      .Text(t.icon)
      .Text(t.pin_lineage_id)
      .Bool(t.pin_dormant)
      .Text(t.sync_id)
      .Int(t.content_updated)
      .Int(t.location_updated)
      .Text(t.parent_guid);
  return s.Run();
}

bool DeleteWhere(sqlite3* db, const char* sql, const std::string& key) {
  Stmt s(db, sql);
  return s.ok() && s.Text(key).Run();
}

// (index, guid): every ordered read, as on the Mac.
bool IndexGuidLess(const Tab* a, const Tab* b) {
  return a->index != b->index ? a->index < b->index : a->guid < b->guid;
}

int Clamp(int value, int lo, int hi) {
  return std::max(lo, std::min(value, hi));
}

}  // namespace

// --- Scope names ---------------------------------------------------------------

const char* PinScopeName(PinScope scope) {
  switch (scope) {
    case PinScope::kSpace:
      return "space";
    case PinScope::kApp:
      return "app";
    case PinScope::kProfile:
      break;
  }
  return "profile";
}

bool PinScopeFromName(std::string_view name, PinScope* out) {
  if (name == "space") {
    *out = PinScope::kSpace;
  } else if (name == "profile") {
    *out = PinScope::kProfile;
  } else if (name == "app") {
    *out = PinScope::kApp;
  } else {
    return false;
  }
  return true;
}

Env DefaultEnv() {
  return Env{&WallNowMs, &RandomUuid};
}

std::string NewUppercaseUuid() {
  return RandomUuid();
}

// --- Lifetime ------------------------------------------------------------------

LocalStore::LocalStore(Env env) : env_(std::move(env)) {}

LocalStore::~LocalStore() {
  Close();
}

LocalStore::OpenResult LocalStore::Open(const std::string& dir) {
  Close();
  const std::string manifest_path = dir + "/" + kCompatibilityManifest;
  std::string manifest;
  const bool has_manifest = ReadWholeFile(manifest_path, &manifest);
  if (has_manifest) {
    const int version = ManifestFormatVersion(manifest);
    if (version > kStoreFormatVersion) {
      return OpenResult::kRequiresNewerApp;
    }
  }
  const std::string path = dir + "/" + kStoreFilename;
  if (sqlite3_open_v2(path.c_str(), &db_,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                          SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    sqlite3_close(db_);
    db_ = nullptr;
    return OpenResult::kFailed;
  }
  sqlite3_busy_timeout(db_, 2000);
  if (!Exec(db_, "PRAGMA journal_mode=WAL;") || !Exec(db_, kSchema) ||
      !Exec(db_, "PRAGMA user_version=13;")) {
    Close();
    return OpenResult::kFailed;
  }
  // A store made before splits of three or four panes lacks the column;
  // one that has it refuses the statement, which is the answer wanted. The
  // format stays 13: the column is optional, and a build without it still
  // opens the store and reads such a cell as a pair.
  sqlite3_exec(db_, "ALTER TABLE tab ADD COLUMN splitExtraPanes TEXT;",
               nullptr, nullptr, nullptr);
  if (!has_manifest ||
      ManifestFormatVersion(manifest) != kStoreFormatVersion) {
    WriteWholeFileAtomic(manifest_path, ManifestJson(kStoreFormatVersion));
  }
  dir_ = dir;
  if (!LoadAll()) {
    Close();
    return OpenResult::kFailed;
  }
  return OpenResult::kOk;
}

void LocalStore::Close() {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
  dir_.clear();
  tabs_.clear();
  profiles_.clear();
  spaces_.clear();
  rules_.clear();
  pin_scope_ = PinScope::kProfile;
  settings_row_exists_ = false;
  dirty_ = Dirty();
}

bool LocalStore::LoadAll() {
  tabs_.clear();
  profiles_.clear();
  spaces_.clear();
  rules_.clear();
  pin_scope_ = PinScope::kProfile;
  settings_row_exists_ = false;
  {
    Stmt s(db_, (std::string("SELECT ") + kTabColumns + " FROM tab").c_str());
    if (!s.ok()) {
      return false;
    }
    while (s.Next()) {
      Tab t = ReadTab(s);
      tabs_[t.guid] = std::move(t);
    }
  }
  {
    Stmt s(db_,
           "SELECT profileId, guid, displayName, bookmarkRootGuid, syncId, "
           "createdDate FROM profile");
    if (!s.ok()) {
      return false;
    }
    while (s.Next()) {
      Profile p;
      p.profile_id = s.ColText(0);
      p.guid = s.ColText(1);
      p.display_name = s.ColOptText(2);
      p.bookmark_root = s.ColOptText(3);
      p.sync_id = s.ColOptText(4);
      p.created = s.ColOptInt(5);
      profiles_[p.profile_id] = std::move(p);
    }
  }
  {
    Stmt s(db_,
           "SELECT spaceId, profileId, name, colorHex, iconName, sortOrder, "
           "createdDate, updatedDate, bookmarkRootGuid FROM space");
    if (!s.ok()) {
      return false;
    }
    while (s.Next()) {
      Space sp;
      sp.space_id = s.ColText(0);
      sp.profile_id = s.ColText(1);
      sp.name = s.ColText(2);
      sp.color_hex = s.ColText(3);
      sp.icon_name = s.ColText(4);
      sp.sort_order = static_cast<int>(s.ColInt(5));
      sp.created = s.ColInt(6);
      sp.updated = s.ColInt(7);
      sp.bookmark_root = s.ColOptText(8);
      spaces_[sp.space_id] = std::move(sp);
    }
  }
  {
    Stmt s(db_,
           "SELECT id, spaceId, host, pathPrefix, askBeforeRouting, sortOrder, "
           "createdDate, syncId, contentUpdatedDate, targetUpdatedDate, "
           "deletedDate, pendingLocalEdit, mergePartnerSyncId "
           "FROM space_url_rule");
    if (!s.ok()) {
      return false;
    }
    while (s.Next()) {
      UrlRule r;
      r.id = s.ColText(0);
      r.space_id = s.ColText(1);
      r.host = s.ColText(2);
      r.path_prefix = s.ColOptText(3);
      r.ask = s.ColInt(4) != 0;
      r.sort_order = static_cast<int>(s.ColInt(5));
      r.created = s.ColInt(6);
      r.sync_id = s.ColOptText(7);
      r.content_updated = s.ColOptInt(8);
      r.target_updated = s.ColOptInt(9);
      r.deleted = s.ColOptInt(10);
      r.pending_local_edit = s.ColInt(11) != 0;
      r.merge_partner_sync_id = s.ColOptText(12);
      rules_[r.id] = std::move(r);
    }
  }
  {
    Stmt s(db_,
           "SELECT pinnedTabScopeRawValue FROM browser_data_settings "
           "WHERE id = ?");
    if (!s.ok()) {
      return false;
    }
    s.Text(std::string(kBrowserDataSettingsId));
    if (s.Next()) {
      settings_row_exists_ = true;
      PinScope scope = PinScope::kProfile;
      if (PinScopeFromName(s.ColText(0), &scope)) {
        pin_scope_ = scope;
      }
    }
  }
  return true;
}

bool LocalStore::Flush() {
  if (!db_) {
    dirty_ = Dirty();
    return false;
  }
  if (dirty_.empty()) {
    return true;
  }
  bool ok = Exec(db_, "BEGIN IMMEDIATE;");
  for (const std::string& guid : dirty_.tabs_deleted) {
    ok = ok && DeleteWhere(db_, "DELETE FROM tab WHERE guid = ?", guid);
  }
  for (const std::string& guid : dirty_.tabs) {
    const auto it = tabs_.find(guid);
    ok = ok && it != tabs_.end() && WriteTab(db_, it->second);
  }
  for (const std::string& id : dirty_.spaces_deleted) {
    ok = ok && DeleteWhere(db_, "DELETE FROM space WHERE spaceId = ?", id);
  }
  for (const std::string& id : dirty_.spaces) {
    const auto it = spaces_.find(id);
    if (!ok || it == spaces_.end()) {
      ok = false;
      break;
    }
    const Space& sp = it->second;
    Stmt s(db_,
           "INSERT OR REPLACE INTO space(spaceId, profileId, name, colorHex, "
           "iconName, sortOrder, createdDate, updatedDate, bookmarkRootGuid) "
           "VALUES(?,?,?,?,?,?,?,?,?)");
    ok = s.ok() && s.Text(sp.space_id)
                       .Text(sp.profile_id)
                       .Text(sp.name)
                       .Text(sp.color_hex)
                       .Text(sp.icon_name)
                       .Int(sp.sort_order)
                       .Int(sp.created)
                       .Int(sp.updated)
                       .Text(sp.bookmark_root)
                       .Run();
  }
  for (const std::string& id : dirty_.rules_deleted) {
    ok = ok && DeleteWhere(db_, "DELETE FROM space_url_rule WHERE id = ?", id);
  }
  for (const std::string& id : dirty_.rules) {
    const auto it = rules_.find(id);
    if (!ok || it == rules_.end()) {
      ok = false;
      break;
    }
    const UrlRule& r = it->second;
    Stmt s(db_,
           "INSERT OR REPLACE INTO space_url_rule(id, spaceId, host, "
           "pathPrefix, askBeforeRouting, sortOrder, createdDate, syncId, "
           "contentUpdatedDate, targetUpdatedDate, deletedDate, "
           "pendingLocalEdit, mergePartnerSyncId) "
           "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
    ok = s.ok() && s.Text(r.id)
                       .Text(r.space_id)
                       .Text(r.host)
                       .Text(r.path_prefix)
                       .Bool(r.ask)
                       .Int(r.sort_order)
                       .Int(r.created)
                       .Text(r.sync_id)
                       .Int(r.content_updated)
                       .Int(r.target_updated)
                       .Int(r.deleted)
                       .Bool(r.pending_local_edit)
                       .Text(r.merge_partner_sync_id)
                       .Run();
  }
  for (const std::string& id : dirty_.profiles) {
    const auto it = profiles_.find(id);
    if (!ok || it == profiles_.end()) {
      ok = false;
      break;
    }
    const Profile& p = it->second;
    Stmt s(db_,
           "INSERT OR REPLACE INTO profile(profileId, guid, displayName, "
           "bookmarkRootGuid, syncId, createdDate) VALUES(?,?,?,?,?,?)");
    ok = s.ok() && s.Text(p.profile_id)
                       .Text(p.guid)
                       .Text(p.display_name)
                       .Text(p.bookmark_root)
                       .Text(p.sync_id)
                       .Int(p.created)
                       .Run();
  }
  if (ok && dirty_.settings) {
    Stmt s(db_,
           "INSERT OR REPLACE INTO browser_data_settings(id, "
           "pinnedTabScopeRawValue) VALUES(?,?)");
    ok = s.ok() && s.Text(std::string(kBrowserDataSettingsId))
                       .Text(std::string(PinScopeName(pin_scope_)))
                       .Run();
    settings_row_exists_ = ok;
  }
  ok = ok && Exec(db_, "COMMIT;");
  const bool rows_changed = !dirty_.tabs.empty() || !dirty_.tabs_deleted.empty() ||
                            !dirty_.rules.empty() || !dirty_.rules_deleted.empty();
  dirty_ = Dirty();
  if (!ok) {
    Exec(db_, "ROLLBACK;");
    // Memory never leads disk: whatever this mutation did in memory is
    // thrown away with it.
    LoadAll();
    return false;
  }
  ++revision_;
  if (rows_changed && !quiet_flush_ && on_rows_changed_) {
    on_rows_changed_();
  }
  return true;
}

void LocalStore::Discard() {
  if (!dirty_.empty()) {
    dirty_ = Dirty();
    LoadAll();
  }
}

void LocalStore::PutTab(const Tab& tab) {
  tabs_[tab.guid] = tab;
  dirty_.tabs.insert(tab.guid);
  dirty_.tabs_deleted.erase(tab.guid);
}

void LocalStore::DeleteTab(const std::string& guid) {
  tabs_.erase(guid);
  dirty_.tabs.erase(guid);
  dirty_.tabs_deleted.insert(guid);
}

void LocalStore::PutSpace(const Space& space) {
  spaces_[space.space_id] = space;
  dirty_.spaces.insert(space.space_id);
  dirty_.spaces_deleted.erase(space.space_id);
}

void LocalStore::DeleteSpace(const std::string& space_id) {
  spaces_.erase(space_id);
  dirty_.spaces.erase(space_id);
  dirty_.spaces_deleted.insert(space_id);
}

void LocalStore::PutRule(const UrlRule& rule) {
  rules_[rule.id] = rule;
  dirty_.rules.insert(rule.id);
  dirty_.rules_deleted.erase(rule.id);
}

void LocalStore::DeleteRule(const std::string& id) {
  rules_.erase(id);
  dirty_.rules.erase(id);
  dirty_.rules_deleted.insert(id);
}

void LocalStore::PutProfile(const Profile& profile) {
  profiles_[profile.profile_id] = profile;
  dirty_.profiles.insert(profile.profile_id);
}

// profile(with:createIfNeeded: true).
Profile& LocalStore::EnsureProfile(const std::string& profile_id) {
  auto it = profiles_.find(profile_id);
  if (it == profiles_.end()) {
    Profile p;
    p.profile_id = profile_id;
    p.guid = env_.new_uuid();
    PutProfile(p);
    it = profiles_.find(profile_id);
  }
  return it->second;
}

// --- Reads ---------------------------------------------------------------------

const Tab* LocalStore::FindTab(const std::string& guid) const {
  const auto it = tabs_.find(guid);
  return it == tabs_.end() ? nullptr : &it->second;
}

const Profile* LocalStore::FindProfile(const std::string& profile_id) const {
  const auto it = profiles_.find(profile_id);
  return it == profiles_.end() ? nullptr : &it->second;
}

const Space* LocalStore::FindSpace(const std::string& space_id) const {
  const auto it = spaces_.find(space_id);
  return it == spaces_.end() ? nullptr : &it->second;
}

std::vector<Space> LocalStore::Spaces() const {
  std::vector<Space> out;
  out.reserve(spaces_.size());
  for (const auto& [id, space] : spaces_) {
    out.push_back(space);
  }
  // getAllSpaces' tiebreaks: sortOrder, then profileId, then createdDate
  // (and the id last, so equal rows cannot shuffle).
  std::sort(out.begin(), out.end(), [](const Space& a, const Space& b) {
    if (a.sort_order != b.sort_order) {
      return a.sort_order < b.sort_order;
    }
    if (a.profile_id != b.profile_id) {
      return a.profile_id < b.profile_id;
    }
    if (a.created != b.created) {
      return a.created < b.created;
    }
    return a.space_id < b.space_id;
  });
  return out;
}

std::vector<UrlRule> LocalStore::UrlRules(bool include_deleted) const {
  std::vector<UrlRule> out;
  for (const auto& [id, rule] : rules_) {
    if (include_deleted || !rule.deleted) {
      out.push_back(rule);
    }
  }
  std::sort(out.begin(), out.end(), [](const UrlRule& a, const UrlRule& b) {
    if (a.space_id != b.space_id) {
      return a.space_id < b.space_id;
    }
    if (a.sort_order != b.sort_order) {
      return a.sort_order < b.sort_order;
    }
    return a.id < b.id;
  });
  return out;
}

// --- Profiles ------------------------------------------------------------------

bool LocalStore::UpsertProfileDisplayNames(
    const std::map<std::string, std::string>& names) {
  if (!db_) {
    return false;
  }
  for (const auto& [profile_id, raw] : names) {
    size_t begin = raw.find_first_not_of(" \t\r\n");
    size_t end = raw.find_last_not_of(" \t\r\n");
    if (profile_id.empty() || begin == std::string::npos) {
      continue;
    }
    const std::string name = raw.substr(begin, end - begin + 1);
    Profile& profile = EnsureProfile(profile_id);
    if (profile.display_name != name) {
      profile.display_name = name;
      dirty_.profiles.insert(profile_id);
    }
  }
  return Flush();
}

// --- Pinned tabs -------------------------------------------------------------------

std::string LocalStore::Owner::SortKey() const {
  return profile_id.value_or("") + std::string(1, '\0') +
         space_id.value_or("");
}

// pinnedTabOwner(for:profileId:spaceId:).
LocalStore::Owner LocalStore::OwnerFor(PinScope scope,
                                       const std::string& profile_id,
                                       const std::string& space_id) {
  switch (scope) {
    case PinScope::kSpace:
      return Owner{profile_id, space_id};
    case PinScope::kProfile:
      return Owner{profile_id, std::nullopt};
    case PinScope::kApp:
      break;
  }
  return Owner{};
}

// pinnedTab(_:belongsTo:).
bool LocalStore::BelongsTo(const Tab& tab, PinScope scope) {
  switch (scope) {
    case PinScope::kSpace:
      return tab.space_id.has_value();
    case PinScope::kProfile:
      return tab.profile_id.has_value() && !tab.space_id.has_value();
    case PinScope::kApp:
      break;
  }
  return !tab.profile_id.has_value() && !tab.space_id.has_value();
}

// owner(of:at:).
LocalStore::Owner LocalStore::OwnerOf(const Tab& tab, PinScope scope) {
  switch (scope) {
    case PinScope::kSpace:
      return Owner{tab.profile_id, tab.space_id};
    case PinScope::kProfile:
      return Owner{tab.profile_id, std::nullopt};
    case PinScope::kApp:
      break;
  }
  return Owner{};
}

std::vector<Tab*> LocalStore::MutableActivePins(const Owner& owner) {
  std::vector<Tab*> out;
  for (auto& [guid, tab] : tabs_) {
    if (tab.type == RowType::kPinnedTab && tab.profile_id == owner.profile_id &&
        tab.space_id == owner.space_id) {
      out.push_back(&tab);
    }
  }
  std::sort(out.begin(), out.end(), IndexGuidLess);
  return out;
}

std::vector<const Tab*> LocalStore::ActivePins(
    const std::string& profile_id,
    const std::string& space_id) const {
  const Owner owner = OwnerFor(pin_scope_, profile_id, space_id);
  std::vector<const Tab*> out;
  for (const auto& [guid, tab] : tabs_) {
    if (tab.type == RowType::kPinnedTab && tab.profile_id == owner.profile_id &&
        tab.space_id == owner.space_id) {
      out.push_back(&tab);
    }
  }
  std::sort(out.begin(), out.end(), IndexGuidLess);
  return out;
}

// The index/updatedDate rewrite every pinned create and move ends with.
void LocalStore::RenumberPins(const std::vector<Tab*>& pins, TimeMs now) {
  for (size_t i = 0; i < pins.size(); ++i) {
    pins[i]->index = static_cast<int>(i);
    pins[i]->updated = now;
    dirty_.tabs.insert(pins[i]->guid);
    dirty_.tabs_deleted.erase(pins[i]->guid);
  }
}

// applyPinnedTabOwner.
void LocalStore::ApplyOwner(Tab& tab, const Owner& owner) {
  tab.profile_id = owner.profile_id;
  tab.space_id = owner.space_id;
  if (owner.profile_id) {
    EnsureProfile(*owner.profile_id);
  }
}

namespace {

// PinnedTabVariantSignature: content (title, url), the partner's lineage
// and content, and the layout.
struct VariantSignature {
  std::string title;
  std::string url;
  std::optional<std::string> partner_lineage;
  std::optional<std::pair<std::string, std::string>> partner_content;
  std::optional<std::string> layout;
  bool operator==(const VariantSignature&) const = default;
};

std::string LineageOf(const Tab& tab) {
  return tab.pin_lineage_id.value_or(tab.guid);
}

}  // namespace

const Tab* LocalStore::ResolveActivePin(const std::string& guid,
                                        const std::string& profile_id,
                                        const std::string& space_id) const {
  const std::vector<const Tab*> active = ActivePins(profile_id, space_id);
  for (const Tab* tab : active) {
    if (tab->guid == guid) {
      return tab;
    }
  }
  const Tab* source = FindTab(guid);
  if (!source || source->type != RowType::kPinnedTab ||
      BelongsTo(*source, pin_scope_)) {
    // A guid another ACTIVE owner holds is never redirected here.
    return nullptr;
  }
  const std::string lineage = LineageOf(*source);
  std::vector<const Tab*> matches;
  for (const Tab* tab : active) {
    if (LineageOf(*tab) == lineage) {
      matches.push_back(tab);
    }
  }
  if (matches.empty()) {
    return nullptr;
  }
  const auto signature = [this](const Tab& tab) {
    VariantSignature s{tab.title, tab.url, std::nullopt, std::nullopt,
                       tab.layout};
    if (tab.split_partner_guid) {
      if (const Tab* partner = FindTab(*tab.split_partner_guid)) {
        s.partner_lineage = LineageOf(*partner);
        s.partner_content = std::make_pair(partner->title, partner->url);
      }
    }
    return s;
  };
  const VariantSignature wanted = signature(*source);
  const Tab* exact = nullptr;
  int exact_count = 0;
  for (const Tab* tab : matches) {
    if (signature(*tab) == wanted) {
      exact = tab;
      ++exact_count;
    }
  }
  if (exact_count == 1) {
    return exact;
  }
  return matches.size() == 1 ? matches.front() : nullptr;
}

std::string LocalStore::CreatePin(const PinCreate& in, WriteError* error) {
  const auto fail = [&](WriteError e) {
    if (error) {
      *error = e;
    }
    return std::string();
  };
  if (!db_) {
    return fail(WriteError::kUnavailable);
  }
  if (!in.guid.empty() && FindTab(in.guid)) {
    return fail(WriteError::kRowAlreadyMapped);
  }
  if (in.url.empty()) {
    return fail(WriteError::kInvalidUrl);
  }
  const TimeMs now = env_.now();
  const Owner owner = OwnerFor(pin_scope_, in.profile_id, in.space_id);
  std::vector<Tab*> active = MutableActivePins(owner);
  Tab tab;
  tab.guid = in.guid.empty() ? env_.new_uuid() : in.guid;
  tab.type = RowType::kPinnedTab;
  tab.title = in.title;
  tab.url = in.url;
  tab.created = in.created.value_or(now);
  tab.updated = now;
  tab.pin_lineage_id = in.lineage_id.value_or(tab.guid);
  tab.source = in.source;
  // Nil means "never edited": a new pin must not look edited.
  tab.content_updated = in.content_updated;
  tab.favicon = in.favicon;
  tab.favicon_width = in.favicon_width;
  tab.favicon_height = in.favicon_height;
  ApplyOwner(tab, owner);
  const std::string guid = tab.guid;
  PutTab(tab);
  Tab* inserted = &tabs_[guid];
  const int count = static_cast<int>(active.size());
  const int at = Clamp(in.index.value_or(count), 0, count);
  active.insert(active.begin() + at, inserted);
  RenumberPins(active, now);
  if (!Flush()) {
    return fail(WriteError::kIo);
  }
  if (error) {
    *error = WriteError::kNone;
  }
  return guid;
}

bool LocalStore::MovePin(const std::string& guid,
                         const std::string& profile_id,
                         const std::string& space_id,
                         int new_index) {
  if (!db_) {
    return false;
  }
  const Tab* resolved = ResolveActivePin(guid, profile_id, space_id);
  if (!resolved) {
    return false;
  }
  const std::string resolved_guid = resolved->guid;
  const Owner owner = OwnerFor(pin_scope_, profile_id, space_id);
  std::vector<Tab*> active = MutableActivePins(owner);
  const auto it = std::find_if(active.begin(), active.end(), [&](Tab* t) {
    return t->guid == resolved_guid;
  });
  if (it == active.end()) {
    return false;
  }
  Tab* moving = *it;
  active.erase(it);
  const int count = static_cast<int>(active.size());
  active.insert(active.begin() + Clamp(new_index, 0, count), moving);
  ApplyOwner(*moving, owner);
  RenumberPins(active, env_.now());
  return Flush();
}

bool LocalStore::RemovePin(const std::string& guid,
                           const std::string& profile_id,
                           const std::string& space_id) {
  if (!db_) {
    return false;
  }
  const Tab* resolved = ResolveActivePin(guid, profile_id, space_id);
  if (!resolved) {
    return false;
  }
  const std::string resolved_guid = resolved->guid;
  const std::optional<std::string> partner = resolved->split_partner_guid;
  if (partner) {
    auto it = tabs_.find(*partner);
    if (it != tabs_.end() && it->second.split_partner_guid == resolved_guid) {
      it->second.split_partner_guid.reset();
      it->second.split_extra_panes.reset();  // No longer a split at all.
      it->second.layout.reset();
      it->second.updated = env_.now();
      PutTab(it->second);
    }
  }
  DeleteTab(resolved_guid);
  return Flush();
}

bool LocalStore::UpdatePin(const std::string& guid,
                           const std::optional<std::string>& url,
                           const std::optional<std::string>& title) {
  const Tab* found = FindTab(guid);
  if (!db_ || (!url && !title) || !found ||
      found->type != RowType::kPinnedTab || !BelongsTo(*found, pin_scope_)) {
    return false;
  }
  Tab tab = *found;
  bool content_changed = false;
  if (url) {
    if (url->empty()) {
      return false;
    }
    content_changed = content_changed || tab.url != *url;
    tab.url = *url;
    tab.need_update_metadata = true;
  }
  if (title) {
    content_changed = content_changed || tab.title != *title;
    tab.title = *title;
  }
  const TimeMs now = env_.now();
  if (content_changed) {
    tab.content_updated = now;
  }
  tab.updated = now;
  PutTab(tab);
  return Flush();
}

// The Mac's pinnedItems pairing over rows in (index, guid) order: a row is
// consumed with its partner — by its own link, or by a row linking back to
// it when its own link was never written.
std::vector<std::pair<Tab*, Tab*>> LocalStore::Units(
    const std::vector<Tab*>& pins) {
  std::vector<std::pair<Tab*, Tab*>> out;
  std::set<std::string> consumed;
  for (Tab* model : pins) {
    if (!consumed.insert(model->guid).second) {
      continue;
    }
    Tab* linked = nullptr;
    if (model->split_partner_guid) {
      for (Tab* t : pins) {
        if (t->guid == *model->split_partner_guid) {
          linked = t;
          break;
        }
      }
    }
    if (!linked) {
      for (Tab* t : pins) {
        if (t->split_partner_guid == model->guid) {
          linked = t;
          break;
        }
      }
    }
    if (linked && (linked == model || consumed.count(linked->guid))) {
      linked = nullptr;
    }
    if (linked) {
      consumed.insert(linked->guid);
    }
    out.emplace_back(model, linked);
  }
  return out;
}

std::vector<LocalStore::PinUnit> LocalStore::PinUnits(
    const std::string& profile_id,
    const std::string& space_id) const {
  std::vector<Tab*> pins = const_cast<LocalStore*>(this)->MutableActivePins(
      OwnerFor(pin_scope_, profile_id, space_id));
  std::vector<PinUnit> out;
  for (const auto& [primary, secondary] : Units(pins)) {
    out.push_back({primary, secondary});
  }
  return out;
}

bool LocalStore::RemovePinUnit(const std::string& guid) {
  const Tab* found = FindTab(guid);
  if (!db_ || !found || found->type != RowType::kPinnedTab) {
    return false;
  }
  std::vector<Tab*> pins =
      MutableActivePins(Owner{found->profile_id, found->space_id});
  std::set<std::string> doomed{guid};
  for (const auto& [primary, secondary] : Units(pins)) {
    if (primary->guid == guid || (secondary && secondary->guid == guid)) {
      doomed.insert(primary->guid);
      if (secondary) {
        doomed.insert(secondary->guid);
      }
    }
  }
  std::vector<Tab*> remaining;
  for (Tab* t : pins) {
    if (!doomed.count(t->guid)) {
      remaining.push_back(t);
    }
  }
  RenumberPins(remaining, env_.now());
  for (const std::string& g : doomed) {
    DeleteTab(g);
  }
  return Flush();
}

bool LocalStore::MovePinUnit(const std::string& guid, int unit_index) {
  const Tab* found = FindTab(guid);
  if (!db_ || !found || found->type != RowType::kPinnedTab) {
    return false;
  }
  std::vector<Tab*> pins =
      MutableActivePins(Owner{found->profile_id, found->space_id});
  std::vector<std::pair<Tab*, Tab*>> units = Units(pins);
  const auto it = std::find_if(units.begin(), units.end(), [&](const auto& u) {
    return u.first->guid == guid || (u.second && u.second->guid == guid);
  });
  if (it == units.end()) {
    return false;
  }
  const std::pair<Tab*, Tab*> moving = *it;
  units.erase(it);
  const int count = static_cast<int>(units.size());
  units.insert(units.begin() + Clamp(unit_index, 0, count), moving);
  std::vector<Tab*> ordered;
  for (const auto& [primary, secondary] : units) {
    ordered.push_back(primary);
    if (secondary) {
      ordered.push_back(secondary);
    }
  }
  RenumberPins(ordered, env_.now());
  return Flush();
}

bool LocalStore::SetPinSplitPair(const std::string& primary,
                                 const std::string& secondary,
                                 const std::string& layout) {
  auto a = tabs_.find(primary);
  auto b = tabs_.find(secondary);
  if (!db_ || primary == secondary || a == tabs_.end() || b == tabs_.end() ||
      a->second.type != RowType::kPinnedTab ||
      b->second.type != RowType::kPinnedTab) {
    return false;
  }
  const TimeMs now = env_.now();
  a->second.split_partner_guid = secondary;
  a->second.layout = layout;
  a->second.updated = now;
  b->second.split_partner_guid = primary;
  b->second.layout = layout;
  b->second.updated = now;
  PutTab(a->second);
  PutTab(b->second);
  return Flush();
}

bool LocalStore::ClearPinSplitPartner(const std::string& guid) {
  auto it = tabs_.find(guid);
  if (!db_ || it == tabs_.end() || it->second.type != RowType::kPinnedTab) {
    return false;
  }
  if (!it->second.split_partner_guid && !it->second.layout &&
      !it->second.split_extra_panes) {
    return true;  // Already equal: success, and nothing to write.
  }
  it->second.split_partner_guid.reset();
  it->second.split_extra_panes.reset();
  it->second.layout.reset();
  it->second.updated = env_.now();
  PutTab(it->second);
  return Flush();
}

bool LocalStore::SetSplitExtraPanes(const std::string& guid,
                                    const std::optional<std::string>& json) {
  auto it = tabs_.find(guid);
  if (!db_ || it == tabs_.end()) {
    return false;
  }
  const std::optional<std::string> value =
      json && !json->empty() ? json : std::nullopt;
  if (it->second.split_extra_panes == value) {
    return true;
  }
  it->second.split_extra_panes = value;
  it->second.updated = env_.now();
  PutTab(it->second);
  return Flush();
}

bool LocalStore::SetPinSplitLayout(const std::string& first,
                                   const std::string& second,
                                   const std::string& layout) {
  auto a = tabs_.find(first);
  auto b = tabs_.find(second);
  if (!db_ || a == tabs_.end() || b == tabs_.end() ||
      a->second.type != RowType::kPinnedTab ||
      b->second.type != RowType::kPinnedTab) {
    return false;
  }
  if (a->second.layout == layout && b->second.layout == layout) {
    return true;
  }
  const TimeMs now = env_.now();
  a->second.layout = layout;
  a->second.updated = now;
  b->second.layout = layout;
  b->second.updated = now;
  PutTab(a->second);
  PutTab(b->second);
  return Flush();
}

bool LocalStore::SwapPinSplitOrder(const std::string& guid) {
  const Tab* tab = FindTab(guid);
  if (!db_ || !tab || tab->type != RowType::kPinnedTab ||
      !tab->split_partner_guid) {
    return false;
  }
  const Tab* partner = FindTab(*tab->split_partner_guid);
  if (!partner || partner->profile_id != tab->profile_id ||
      partner->space_id != tab->space_id) {
    return false;
  }
  const std::string a = tab->guid;
  const std::string b = partner->guid;
  std::vector<Tab*> pins =
      MutableActivePins(Owner{tab->profile_id, tab->space_id});
  const auto ia = std::find_if(pins.begin(), pins.end(),
                               [&](Tab* t) { return t->guid == a; });
  const auto ib = std::find_if(pins.begin(), pins.end(),
                               [&](Tab* t) { return t->guid == b; });
  if (ia == pins.end() || ib == pins.end()) {
    return false;
  }
  std::iter_swap(ia, ib);
  RenumberPins(pins, env_.now());
  return Flush();
}

std::string LocalStore::TransferPin(const std::string& guid,
                                    const std::string& source_profile_id,
                                    const std::string& source_space_id,
                                    const std::string& target_profile_id,
                                    const std::string& target_space_id,
                                    int destination_index) {
  if (!db_) {
    return std::string();
  }
  const Owner source_owner =
      OwnerFor(pin_scope_, source_profile_id, source_space_id);
  const Owner target_owner =
      OwnerFor(pin_scope_, target_profile_id, target_space_id);
  std::vector<Tab*> source_tabs = MutableActivePins(source_owner);
  const auto source_it =
      std::find_if(source_tabs.begin(), source_tabs.end(),
                   [&](Tab* t) { return t->guid == guid; });
  if (source_it == source_tabs.end()) {
    return std::string();
  }
  Tab* source = *source_it;
  // pinnedTransferUnit: the row and its split partner, in (index, guid).
  std::vector<Tab*> unit{source};
  Tab* partner = nullptr;
  for (Tab* t : source_tabs) {
    if (t == source) {
      continue;
    }
    if ((source->split_partner_guid && t->guid == *source->split_partner_guid) ||
        (!source->split_partner_guid && t->split_partner_guid == source->guid)) {
      partner = t;
      break;
    }
  }
  if (partner) {
    unit.push_back(partner);
    std::sort(unit.begin(), unit.end(), IndexGuidLess);
    unit[0]->split_partner_guid = unit[1]->guid;
    unit[1]->split_partner_guid = unit[0]->guid;
  }
  std::set<std::string> unit_guids;
  for (Tab* t : unit) {
    unit_guids.insert(t->guid);
  }
  const TimeMs now = env_.now();

  if (source_owner == target_owner) {
    std::vector<Tab*> reordered;
    for (Tab* t : MutableActivePins(target_owner)) {
      if (!unit_guids.count(t->guid)) {
        reordered.push_back(t);
      }
    }
    const int count = static_cast<int>(reordered.size());
    reordered.insert(reordered.begin() + Clamp(destination_index, 0, count),
                     unit.begin(), unit.end());
    RenumberPins(reordered, now);
    return Flush() ? guid : std::string();
  }

  // Another owner: copy with fresh guids and the same lineage
  // (copyPinnedTab), then delete the source.
  std::map<std::string, std::string> mapping;
  std::vector<std::string> copied_guids;
  for (Tab* t : unit) {
    Tab copy;
    copy.guid = env_.new_uuid();
    copy.type = t->type;
    copy.title = t->title;
    copy.index = t->index;
    copy.url = t->url;
    copy.favicon = t->favicon;
    copy.favicon_width = t->favicon_width;
    copy.favicon_height = t->favicon_height;
    copy.created = t->created;
    copy.updated = t->updated;
    copy.override_title = t->override_title;
    copy.is_opened = t->is_opened;
    copy.is_created_by_chromium = t->is_created_by_chromium;
    copy.need_update_metadata = t->need_update_metadata;
    copy.source = t->source;
    copy.secondary_url = t->secondary_url;
    copy.secondary_title = t->secondary_title;
    copy.split_extra_panes = t->split_extra_panes;
    copy.layout = t->layout;
    copy.last_seen = t->last_seen;
    copy.icon = t->icon;
    copy.pin_lineage_id = LineageOf(*t);
    ApplyOwner(copy, target_owner);
    mapping[t->guid] = copy.guid;
    copied_guids.push_back(copy.guid);
    PutTab(copy);
  }
  if (copied_guids.size() == 2) {
    tabs_[copied_guids[0]].split_partner_guid = copied_guids[1];
    tabs_[copied_guids[1]].split_partner_guid = copied_guids[0];
  }
  std::vector<Tab*> target = MutableActivePins(target_owner);
  std::vector<Tab*> copies;
  for (const std::string& g : copied_guids) {
    copies.push_back(&tabs_[g]);
  }
  std::erase_if(target, [&](Tab* t) {
    return std::find(copies.begin(), copies.end(), t) != copies.end();
  });
  const int count = static_cast<int>(target.size());
  target.insert(target.begin() + Clamp(destination_index, 0, count),
                copies.begin(), copies.end());
  RenumberPins(target, now);
  std::vector<Tab*> remaining;
  for (Tab* t : source_tabs) {
    if (!unit_guids.count(t->guid)) {
      remaining.push_back(t);
    }
  }
  RenumberPins(remaining, now);
  for (const std::string& g : unit_guids) {
    DeleteTab(g);
  }
  const std::string moved = mapping[guid];
  return Flush() ? moved : std::string();
}

bool LocalStore::ChangePinScope(PinScope scope,
                                const std::string& preferred_profile_id,
                                const std::string& preferred_space_id) {
  if (!db_ || scope == pin_scope_) {
    return false;
  }
  MigratePins(pin_scope_, scope, preferred_profile_id, preferred_space_id);
  pin_scope_ = scope;
  ++pin_scope_generation_;
  dirty_.settings = true;
  return Flush();
}

// migratePinnedTabs: the target scope's collections are REBUILT from the
// active source rows with new guids; the source rows stay behind as
// inactive backups; copies of one pin merge back by lineage when their
// whole variant (content, partner, layout) agrees.
void LocalStore::MigratePins(PinScope from,
                             PinScope to,
                             const std::string& preferred_profile_id,
                             const std::string& preferred_space_id) {
  std::vector<Tab*> all_pinned;
  for (auto& [guid, tab] : tabs_) {
    if (tab.type == RowType::kPinnedTab) {
      all_pinned.push_back(&tab);
    }
  }
  std::sort(all_pinned.begin(), all_pinned.end(), IndexGuidLess);

  std::vector<Tab*> active_source;
  std::vector<Tab*> dormant_source;
  std::vector<std::string> target_guids;
  for (Tab* tab : all_pinned) {
    if (BelongsTo(*tab, from)) {
      active_source.push_back(tab);
    }
    // A Profile collection with no Space cannot be shown under Space scope
    // but is still live input when leaving it.
    if (from == PinScope::kSpace && tab->pin_dormant &&
        BelongsTo(*tab, PinScope::kProfile)) {
      dormant_source.push_back(tab);
    }
    if (BelongsTo(*tab, to)) {
      target_guids.push_back(tab->guid);
    }
  }
  std::vector<Tab*> source_rows = active_source;
  source_rows.insert(source_rows.end(), dormant_source.begin(),
                     dormant_source.end());

  // pinnedTabTargetOwners.
  std::set<Owner> target_owners;
  switch (to) {
    case PinScope::kApp:
      target_owners.insert(Owner{});
      break;
    case PinScope::kProfile: {
      std::set<std::string> ids;
      for (const auto& [id, profile] : profiles_) {
        ids.insert(id);
      }
      for (Tab* tab : source_rows) {
        if (tab->profile_id) {
          ids.insert(*tab->profile_id);
        }
      }
      for (const std::string& id : ids) {
        target_owners.insert(Owner{id, std::nullopt});
      }
      break;
    }
    case PinScope::kSpace:
      for (const auto& [id, space] : spaces_) {
        target_owners.insert(Owner{space.profile_id, space.space_id});
      }
      break;
  }

  // First row wins for a repeated guid, as on the Mac.
  std::map<std::string, std::string> lineage_by_guid;
  for (Tab* tab : source_rows) {
    lineage_by_guid.emplace(tab->guid, LineageOf(*tab));
  }

  const auto is_preferred = [&](const Owner& owner) {
    switch (from) {
      case PinScope::kSpace:
        return !preferred_space_id.empty() &&
               owner.space_id == preferred_space_id;
      case PinScope::kProfile:
        return !preferred_profile_id.empty() &&
               owner.profile_id == preferred_profile_id;
      case PinScope::kApp:
        break;
    }
    return true;
  };

  struct Candidate {
    Tab source;
    std::string lineage;
    VariantSignature signature;
    std::vector<std::string> source_guids;
    std::optional<TimeMs> last_seen;
    std::optional<TimeMs> content_updated;
  };

  const TimeMs now = env_.now();
  (void)now;
  // std::set iterates in SortKey order, the Mac's sorted(by: sortKey).
  for (const Owner& owner : target_owners) {
    // sourceCollections.
    std::map<Owner, std::vector<Tab*>> grouped;
    for (Tab* tab : source_rows) {
      bool relevant = true;
      if (from != PinScope::kApp && to != PinScope::kApp &&
          ((from == PinScope::kProfile && to == PinScope::kSpace) ||
           (from == PinScope::kSpace && to == PinScope::kProfile))) {
        relevant = tab->profile_id == owner.profile_id;
      }
      if (relevant) {
        grouped[OwnerOf(*tab, from)].push_back(tab);
      }
    }
    std::vector<Owner> ordered;
    for (const auto& [o, rows] : grouped) {
      ordered.push_back(o);
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [&](const Owner& a, const Owner& b) {
                       const bool pa = is_preferred(a);
                       const bool pb = is_preferred(b);
                       if (pa != pb) {
                         return pa;
                       }
                       return a.SortKey() < b.SortKey();
                     });
    std::vector<std::vector<Tab*>> collections;
    for (const Owner& o : ordered) {
      std::vector<Tab*> rows = grouped[o];
      std::sort(rows.begin(), rows.end(), IndexGuidLess);
      collections.push_back(std::move(rows));
    }

    // mergeCandidates.
    std::vector<Candidate> candidates;
    std::map<std::string, std::vector<size_t>> by_lineage;
    for (const auto& collection : collections) {
      for (Tab* src : collection) {
        const std::string lineage = LineageOf(*src);
        VariantSignature signature{src->title, src->url, std::nullopt,
                                   std::nullopt, src->layout};
        if (src->split_partner_guid) {
          const auto l = lineage_by_guid.find(*src->split_partner_guid);
          if (l != lineage_by_guid.end()) {
            signature.partner_lineage = l->second;
          }
          for (const auto& c : collections) {
            const auto p = std::find_if(c.begin(), c.end(), [&](Tab* t) {
              return t->guid == *src->split_partner_guid;
            });
            if (p != c.end()) {
              signature.partner_content =
                  std::make_pair((*p)->title, (*p)->url);
              break;
            }
          }
        }
        bool merged = false;
        for (size_t i : by_lineage[lineage]) {
          Candidate& candidate = candidates[i];
          if (!(candidate.signature == signature)) {
            continue;
          }
          candidate.source_guids.push_back(src->guid);
          if (src->last_seen &&
              (!candidate.last_seen || *src->last_seen > *candidate.last_seen)) {
            candidate.last_seen = src->last_seen;
          }
          if (src->content_updated &&
              (!candidate.content_updated ||
               *src->content_updated > *candidate.content_updated)) {
            candidate.content_updated = src->content_updated;
          }
          merged = true;
          break;
        }
        if (merged) {
          continue;
        }
        candidates.push_back(Candidate{*src, lineage, signature, {src->guid},
                                       src->last_seen, src->content_updated});
        by_lineage[lineage].push_back(candidates.size() - 1);
      }
    }

    // insertPinnedTabs.
    std::vector<std::string> new_guids;
    std::map<std::string, size_t> index_by_source;
    for (size_t i = 0; i < candidates.size(); ++i) {
      const Tab& src = candidates[i].source;
      Tab model;
      model.guid = env_.new_uuid();
      model.type = RowType::kPinnedTab;
      model.title = src.title;
      model.index = static_cast<int>(i);
      model.url = src.url;
      model.favicon = src.favicon;
      model.favicon_width = src.favicon_width;
      model.favicon_height = src.favicon_height;
      model.created = src.created;
      model.updated = src.updated;
      model.override_title = src.override_title;
      model.is_opened = false;
      model.is_created_by_chromium = src.is_created_by_chromium;
      model.need_update_metadata = src.need_update_metadata;
      model.source = src.source;
      model.secondary_url = src.secondary_url;
      model.secondary_title = src.secondary_title;
      model.split_extra_panes = src.split_extra_panes;
      model.secondary_favicon = src.secondary_favicon;
      model.secondary_favicon_width = src.secondary_favicon_width;
      model.secondary_favicon_height = src.secondary_favicon_height;
      model.layout = src.layout;
      model.last_seen = candidates[i].last_seen;
      model.icon = src.icon;
      model.content_updated = candidates[i].content_updated;
      model.pin_lineage_id = candidates[i].lineage;
      ApplyOwner(model, owner);
      new_guids.push_back(model.guid);
      for (const std::string& g : candidates[i].source_guids) {
        index_by_source[g] = i;
      }
      PutTab(model);
    }
    for (size_t i = 0; i < candidates.size(); ++i) {
      Tab& model = tabs_[new_guids[i]];
      const auto& partner = candidates[i].source.split_partner_guid;
      const auto found =
          partner ? index_by_source.find(*partner) : index_by_source.end();
      if (found == index_by_source.end()) {
        model.split_partner_guid.reset();
      } else {
        model.split_partner_guid = new_guids[found->second];
      }
    }
  }

  // The inactive target's old rows were backups, not input: they go, after
  // the insertion (dormant Profile rows can be both). The dormant rows that
  // outlive the deletion are resolved BEFORE it: the Mac keeps object
  // identity across context.delete, but here a deleted row's map node is
  // gone, and reading its guid afterwards was a use-after-free (the pinned
  // suite's Space -> Profile change crashed the client, 2026-10-16).
  const std::set<std::string> deleted_targets(target_guids.begin(),
                                              target_guids.end());
  std::vector<Tab*> dormant_survivors;
  for (Tab* tab : dormant_source) {
    if (!deleted_targets.count(tab->guid)) {
      dormant_survivors.push_back(tab);
    }
  }
  for (const std::string& g : target_guids) {
    DeleteTab(g);
  }

  if (from == PinScope::kProfile && to == PinScope::kSpace) {
    std::set<std::string> profiles_with_spaces;
    for (const auto& [id, space] : spaces_) {
      profiles_with_spaces.insert(space.profile_id);
    }
    for (Tab* tab : active_source) {
      const bool dormant =
          tab->profile_id && !profiles_with_spaces.count(*tab->profile_id);
      if (tab->pin_dormant != dormant) {
        tab->pin_dormant = dormant;
        PutTab(*tab);
      }
    }
  } else {
    for (Tab* tab : dormant_survivors) {
      if (tab->pin_dormant) {
        tab->pin_dormant = false;
        PutTab(*tab);
      }
    }
  }
}

// --- Bookmarks -------------------------------------------------------------------

std::string LocalStore::ExistingBookmarkRoot(
    const std::string& profile_id,
    const std::string& space_id) const {
  const Space* space = FindSpace(space_id);
  if (space && space->profile_id == profile_id && space->bookmark_root &&
      FindTab(*space->bookmark_root)) {
    return *space->bookmark_root;
  }
  if (space_id != kDefaultSpaceId) {
    return std::string();
  }
  const Profile* profile = FindProfile(profile_id);
  if (profile && profile->bookmark_root && FindTab(*profile->bookmark_root)) {
    return *profile->bookmark_root;
  }
  return std::string();
}

std::string LocalStore::BookmarkRoot(const std::string& profile_id,
                                     const std::string& space_id,
                                     bool create) {
  if (!db_ || profile_id.empty() || space_id.empty()) {
    return std::string();
  }
  if (!create && !FindProfile(profile_id)) {
    return std::string();
  }
  const auto space_it = spaces_.find(space_id);
  Space* space = space_it != spaces_.end() &&
                         space_it->second.profile_id == profile_id
                     ? &space_it->second
                     : nullptr;
  if (space && space->bookmark_root && FindTab(*space->bookmark_root)) {
    return *space->bookmark_root;
  }
  // Legacy compat: the default Space shares the Profile's root.
  if (space_id == kDefaultSpaceId) {
    const Profile* profile = FindProfile(profile_id);
    if (profile && profile->bookmark_root && FindTab(*profile->bookmark_root)) {
      const std::string root = *profile->bookmark_root;
      if (space) {
        space->bookmark_root = root;
        PutSpace(*space);
        Flush();
      }
      return root;
    }
  }
  // Heal-on-read: reclaim unlinked roots for this (profile, Space), oldest
  // first, and fold any duplicates into it.
  std::vector<Tab*> orphans;
  for (auto& [guid, tab] : tabs_) {
    if (tab.type == RowType::kBookmarkFolder && !tab.parent_guid &&
        tab.space_id == space_id && tab.profile_id == profile_id &&
        !tab.is_created_by_chromium) {
      orphans.push_back(&tab);
    }
  }
  std::sort(orphans.begin(), orphans.end(), [](const Tab* a, const Tab* b) {
    return a->created != b->created ? a->created < b->created
                                    : a->guid < b->guid;
  });
  if (!orphans.empty()) {
    const std::string primary = orphans.front()->guid;
    if (space) {
      space->bookmark_root = primary;
      PutSpace(*space);
    }
    for (size_t i = 1; i < orphans.size(); ++i) {
      const std::string duplicate = orphans[i]->guid;
      for (auto& [guid, tab] : tabs_) {
        if (tab.parent_guid == duplicate) {
          tab.parent_guid = primary;
          PutTab(tab);
        }
      }
      DeleteTab(duplicate);
    }
    Flush();
    return primary;
  }
  if (!create) {
    return std::string();
  }
  const std::string root = NewBookmarkRoot(profile_id, space_id);
  return Flush() ? root : std::string();
}

std::string LocalStore::NewBookmarkRoot(const std::string& profile_id,
                                        const std::string& space_id) {
  const TimeMs now = env_.now();
  Tab root;
  root.guid = env_.new_uuid();
  root.type = RowType::kBookmarkFolder;
  root.title = kBookmarkRootTitle;
  root.url = kFolderPlaceholderUrl;
  root.created = now;
  root.updated = now;
  root.profile_id = profile_id;
  root.space_id = space_id;
  PutTab(root);
  Profile& profile = EnsureProfile(profile_id);
  const auto space_it = spaces_.find(space_id);
  if (space_it != spaces_.end() && space_it->second.profile_id == profile_id) {
    space_it->second.bookmark_root = root.guid;
    PutSpace(space_it->second);
  }
  // Only the default Space's first root is mirrored onto the Profile.
  if (space_id == kDefaultSpaceId && !profile.bookmark_root) {
    profile.bookmark_root = root.guid;
    dirty_.profiles.insert(profile_id);
  }
  return root.guid;
}

bool LocalStore::IsBookmarkRoot(const std::string& guid) const {
  for (const auto& [id, profile] : profiles_) {
    if (profile.bookmark_root == guid) {
      return true;
    }
  }
  for (const auto& [id, space] : spaces_) {
    if (space.bookmark_root == guid) {
      return true;
    }
  }
  return false;
}

std::vector<const Tab*> LocalStore::Children(
    const std::string& parent_guid) const {
  std::vector<const Tab*> out;
  for (const auto& [guid, tab] : tabs_) {
    if (tab.parent_guid == parent_guid) {
      out.push_back(&tab);
    }
  }
  std::sort(out.begin(), out.end(), IndexGuidLess);
  return out;
}

std::vector<Tab*> LocalStore::MutableChildren(const std::string& parent_guid) {
  std::vector<Tab*> out;
  for (auto& [guid, tab] : tabs_) {
    if (tab.parent_guid == parent_guid) {
      out.push_back(&tab);
    }
  }
  std::sort(out.begin(), out.end(), IndexGuidLess);
  return out;
}

// normalizeIndexes: only rows whose index actually moves are touched.
void LocalStore::NormalizeIndexes(const std::vector<Tab*>& nodes, TimeMs now) {
  for (size_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i]->index != static_cast<int>(i)) {
      nodes[i]->index = static_cast<int>(i);
      nodes[i]->updated = now;
      dirty_.tabs.insert(nodes[i]->guid);
      dirty_.tabs_deleted.erase(nodes[i]->guid);
    }
  }
}

// insert(node:to:at:).
void LocalStore::InsertNode(Tab& node,
                            const std::string& parent_guid,
                            const std::optional<int>& index,
                            TimeMs now) {
  node.parent_guid = parent_guid;
  const std::string guid = node.guid;
  PutTab(node);
  std::vector<Tab*> siblings = MutableChildren(parent_guid);
  std::erase_if(siblings, [&](Tab* t) { return t->guid == guid; });
  const int count = static_cast<int>(siblings.size());
  const int at = Clamp(index.value_or(count), 0, count);
  siblings.insert(siblings.begin() + at, &tabs_[guid]);
  NormalizeIndexes(siblings, now);
}

std::string LocalStore::ResolveParent(
    const std::optional<std::string>& parent_guid,
    const std::string& profile_id,
    const std::string& space_id) {
  if (parent_guid && !parent_guid->empty()) {
    const Tab* node = FindTab(*parent_guid);
    if (node && node->type == RowType::kBookmarkFolder) {
      return node->guid;
    }
  }
  return BookmarkRoot(profile_id, space_id, /*create=*/true);
}

std::string LocalStore::CreateBookmark(const BookmarkCreate& in,
                                       WriteError* error) {
  const auto fail = [&](WriteError e) {
    if (error) {
      *error = e;
    }
    return std::string();
  };
  if (!db_) {
    return fail(WriteError::kUnavailable);
  }
  if (in.url.empty()) {
    return fail(WriteError::kInvalidUrl);
  }
  if (!in.guid.empty() && FindTab(in.guid)) {
    return fail(WriteError::kRowAlreadyMapped);
  }
  const std::string parent =
      ResolveParent(in.parent_guid, in.profile_id, in.space_id);
  if (parent.empty()) {
    return fail(WriteError::kRowNotFound);
  }
  const TimeMs now = env_.now();
  Tab node;
  node.guid = in.guid.empty() ? env_.new_uuid() : in.guid;
  node.type = RowType::kBookmark;
  // The UI default: an empty title shows the url.
  node.title = in.title.empty() ? in.url : in.title;
  node.url = in.url;
  node.created = now;
  node.updated = now;
  node.space_id = in.space_id;
  node.profile_id = in.profile_id;
  node.source = in.source;
  node.secondary_url = in.secondary_url;
  if (in.secondary_url && in.secondary_title && !in.secondary_title->empty()) {
    node.secondary_title = in.secondary_title;
  }
  if (in.secondary_url) {
    node.layout = in.layout;
    node.split_extra_panes = in.split_extra_panes;
    node.secondary_favicon = in.secondary_favicon;
    node.secondary_favicon_width = in.secondary_favicon_width;
    node.secondary_favicon_height = in.secondary_favicon_height;
  }
  node.favicon = in.favicon;
  node.favicon_width = in.favicon_width;
  node.favicon_height = in.favicon_height;
  EnsureProfile(in.profile_id);
  InsertNode(node, parent, in.index, now);
  if (!Flush()) {
    return fail(WriteError::kIo);
  }
  if (error) {
    *error = WriteError::kNone;
  }
  return node.guid;
}

std::string LocalStore::CreateFolder(
    const std::string& profile_id,
    const std::string& space_id,
    const std::optional<std::string>& parent_guid,
    const std::optional<int>& index,
    const std::string& title,
    const std::string& guid,
    int source) {
  if (!db_ || (!guid.empty() && FindTab(guid))) {
    return std::string();
  }
  const std::string parent = ResolveParent(parent_guid, profile_id, space_id);
  if (parent.empty()) {
    return std::string();
  }
  const TimeMs now = env_.now();
  Tab folder;
  folder.guid = guid.empty() ? env_.new_uuid() : guid;
  folder.type = RowType::kBookmarkFolder;
  folder.title = title;
  folder.url = kFolderPlaceholderUrl;
  folder.created = now;
  folder.updated = now;
  folder.space_id = space_id;
  folder.profile_id = profile_id;
  folder.source = source;
  EnsureProfile(profile_id);
  InsertNode(folder, parent, index, now);
  return Flush() ? folder.guid : std::string();
}

bool LocalStore::HasAncestor(const Tab& node,
                             const std::set<std::string>& guids) const {
  std::optional<std::string> parent = node.parent_guid;
  int guard = 0;
  while (parent && guard++ < 10000) {
    if (guids.count(*parent)) {
      return true;
    }
    const Tab* p = FindTab(*parent);
    parent = p ? p->parent_guid : std::nullopt;
  }
  return false;
}

bool LocalStore::MoveBookmark(const std::string& guid,
                              const std::string& profile_id,
                              const std::optional<std::string>& parent_guid,
                              int new_index) {
  if (!db_) {
    return false;
  }
  const Tab* node = FindTab(guid);
  if (!node || !node->is_bookmark_kind() || IsBookmarkRoot(guid)) {
    return false;
  }
  const std::string target_space = node->space_id.value_or(kDefaultSpaceId);
  const std::optional<std::string> original_parent = node->parent_guid;
  const std::string parent = ResolveParent(parent_guid, profile_id,
                                           target_space);
  if (parent.empty() || parent == guid) {
    return false;
  }
  // A folder never becomes its own ancestor.
  if (const Tab* p = FindTab(parent); p && HasAncestor(*p, {guid})) {
    return false;
  }
  const TimeMs now = env_.now();
  Tab& moving = tabs_[guid];
  moving.parent_guid = parent;
  PutTab(moving);
  if (original_parent && *original_parent != parent) {
    NormalizeIndexes(MutableChildren(*original_parent), now);
  }
  std::vector<Tab*> siblings = MutableChildren(parent);
  std::erase_if(siblings, [&](Tab* t) { return t->guid == guid; });
  const int count = static_cast<int>(siblings.size());
  siblings.insert(siblings.begin() + Clamp(new_index, 0, count), &tabs_[guid]);
  NormalizeIndexes(siblings, now);
  Tab& moved = tabs_[guid];
  // A reorder under the same parent is rank, not a move.
  if (original_parent != parent) {
    moved.location_updated = now;
  }
  moved.updated = now;
  PutTab(moved);
  return Flush();
}

void LocalStore::RetagSubtree(const std::string& guid,
                              const std::string& profile_id,
                              const std::string& space_id,
                              TimeMs now) {
  auto it = tabs_.find(guid);
  if (it == tabs_.end()) {
    return;
  }
  it->second.profile_id = profile_id;
  it->second.space_id = space_id;
  it->second.updated = now;
  PutTab(it->second);
  for (Tab* child : MutableChildren(guid)) {
    RetagSubtree(child->guid, profile_id, space_id, now);
  }
}

bool LocalStore::MoveBookmarksToSpace(
    const std::vector<std::string>& guids,
    const std::string& source_profile_id,
    const std::string& target_profile_id,
    const std::string& target_space_id,
    const std::optional<std::string>& target_parent,
    const std::optional<int>& destination_index) {
  if (!db_ || guids.empty()) {
    return false;
  }
  const std::string root =
      BookmarkRoot(target_profile_id, target_space_id, /*create=*/true);
  if (root.empty()) {
    return false;
  }
  std::string destination = root;
  if (target_parent && !target_parent->empty()) {
    const Tab* folder = FindTab(*target_parent);
    if (!folder || folder->type != RowType::kBookmarkFolder ||
        folder->profile_id != target_profile_id ||
        folder->space_id != target_space_id) {
      return false;
    }
    destination = folder->guid;
  }
  const std::set<std::string> requested(guids.begin(), guids.end());
  if (requested.count(destination) ||
      HasAncestor(*FindTab(destination), requested)) {
    return false;
  }
  std::vector<std::string> original_siblings;
  for (const Tab* t : Children(destination)) {
    original_siblings.push_back(t->guid);
  }
  EnsureProfile(target_profile_id);
  const TimeMs now = env_.now();
  std::set<std::string> source_parents;
  std::vector<std::string> moved;
  for (const std::string& guid : guids) {
    const Tab* node = FindTab(guid);
    if (!node || !node->is_bookmark_kind() ||
        node->profile_id != source_profile_id || IsBookmarkRoot(guid) ||
        HasAncestor(*node, requested)) {
      continue;
    }
    if (node->space_id == target_space_id &&
        node->profile_id == target_profile_id && !target_parent &&
        !destination_index) {
      continue;
    }
    if (node->parent_guid) {
      source_parents.insert(*node->parent_guid);
    }
    Tab& row = tabs_[guid];
    row.parent_guid = destination;
    // Only the moved row: a descendant's Space is diagnostic on the wire.
    row.location_updated = now;
    PutTab(row);
    RetagSubtree(guid, target_profile_id, target_space_id, now);
    moved.push_back(guid);
  }
  if (moved.empty()) {
    Discard();
    return false;
  }
  const std::set<std::string> moved_set(moved.begin(), moved.end());
  for (const std::string& parent : source_parents) {
    if (parent != destination) {
      NormalizeIndexes(MutableChildren(parent), now);
    }
  }
  std::vector<Tab*> target_siblings;
  for (Tab* t : MutableChildren(destination)) {
    if (!moved_set.count(t->guid)) {
      target_siblings.push_back(t);
    }
  }
  std::vector<Tab*> moved_nodes;
  for (const std::string& g : moved) {
    moved_nodes.push_back(&tabs_[g]);
  }
  if (destination_index) {
    const int bounded = Clamp(*destination_index, 0,
                              static_cast<int>(original_siblings.size()));
    int removed_before = 0;
    for (int i = 0; i < bounded; ++i) {
      if (moved_set.count(original_siblings[static_cast<size_t>(i)])) {
        ++removed_before;
      }
    }
    const int insertion = Clamp(bounded - removed_before, 0,
                                static_cast<int>(target_siblings.size()));
    target_siblings.insert(target_siblings.begin() + insertion,
                           moved_nodes.begin(), moved_nodes.end());
  } else {
    target_siblings.insert(target_siblings.end(), moved_nodes.begin(),
                           moved_nodes.end());
  }
  NormalizeIndexes(target_siblings, now);
  Tab& dest = tabs_[destination];
  dest.updated = now;
  PutTab(dest);
  return Flush();
}

bool LocalStore::UpdateBookmark(const std::string& guid,
                                const BookmarkUpdate& update) {
  const Tab* found = FindTab(guid);
  if (!db_ || !found || !found->is_bookmark_kind()) {
    return false;
  }
  Tab node = *found;
  bool content_changed = false;
  if (update.url) {
    if (update.url->empty()) {
      return false;
    }
    if (node.url != *update.url) {
      node.url = *update.url;
      content_changed = true;
    }
  }
  if (update.title) {
    const std::string title =
        update.title->empty() && !update.allows_empty_title ? node.url
                                                            : *update.title;
    if (node.title != title) {
      node.title = title;
      content_changed = true;
    }
  }
  bool secondary_cleared = false;
  if (update.secondary_url) {
    const std::optional<std::string> value =
        *update.secondary_url && !(*update.secondary_url)->empty()
            ? *update.secondary_url
            : std::nullopt;
    if (node.secondary_url != value) {
      node.secondary_url = value;
      content_changed = true;
    }
    secondary_cleared = !value.has_value();
  }
  if (secondary_cleared) {
    node.layout.reset();
    node.split_extra_panes.reset();
    node.secondary_favicon.clear();
    node.secondary_favicon_width = 0;
    node.secondary_favicon_height = 0;
    if (node.secondary_title) {
      node.secondary_title.reset();
      content_changed = true;
    }
  } else if (update.secondary_title) {
    const std::optional<std::string> value =
        *update.secondary_title && !(*update.secondary_title)->empty()
            ? *update.secondary_title
            : std::nullopt;
    if (node.secondary_title != value) {
      node.secondary_title = value;
      content_changed = true;
    }
  }
  if (update.icon && node.type == RowType::kBookmarkFolder &&
      node.icon != *update.icon) {
    node.icon = *update.icon;
    content_changed = true;
  }
  const TimeMs now = env_.now();
  if (content_changed) {
    node.content_updated = now;
  }
  node.updated = now;
  PutTab(node);
  return Flush();
}

bool LocalStore::SetBookmarkSplitLayout(const std::string& guid,
                                        const std::string& layout) {
  auto it = tabs_.find(guid);
  if (!db_ || it == tabs_.end() || it->second.type != RowType::kBookmark ||
      !it->second.secondary_url) {
    return false;
  }
  if (it->second.layout == layout) {
    return true;
  }
  it->second.layout = layout;
  it->second.updated = env_.now();
  PutTab(it->second);
  return Flush();
}

bool LocalStore::SwapBookmarkPanes(const std::string& guid) {
  auto it = tabs_.find(guid);
  if (!db_ || it == tabs_.end() || it->second.type != RowType::kBookmark ||
      !it->second.secondary_url) {
    return false;
  }
  Tab& t = it->second;
  std::string second_url = *t.secondary_url;
  std::string second_title = t.secondary_title.value_or(std::string());
  t.secondary_url = t.url;
  t.secondary_title =
      t.title.empty() ? std::nullopt : std::optional<std::string>(t.title);
  t.url = second_url;
  t.title = second_title.empty() ? second_url : second_title;
  std::swap(t.favicon, t.secondary_favicon);
  std::swap(t.favicon_width, t.secondary_favicon_width);
  std::swap(t.favicon_height, t.secondary_favicon_height);
  const TimeMs now = env_.now();
  t.content_updated = now;
  t.updated = now;
  PutTab(t);
  return Flush();
}

void LocalStore::CollectSubtree(const std::string& guid,
                                std::vector<std::string>* out) const {
  // Iterative and each row once: a corrupt parentGuid cycle (a self-parented
  // row) must neither loop nor overflow the stack. Pre-order, as before.
  std::set<std::string> seen;
  std::vector<std::string> pending = {guid};
  while (!pending.empty()) {
    std::string next = std::move(pending.back());
    pending.pop_back();
    if (!seen.insert(next).second) {
      continue;
    }
    out->push_back(next);
    const auto children = Children(next);
    for (auto child = children.rbegin(); child != children.rend(); ++child) {
      pending.push_back((*child)->guid);
    }
  }
}

bool LocalStore::DeleteBookmark(const std::string& guid) {
  const Tab* node = FindTab(guid);
  if (!db_ || !node || !node->is_bookmark_kind() || IsBookmarkRoot(guid)) {
    return false;
  }
  const std::optional<std::string> parent = node->parent_guid;
  std::vector<std::string> doomed;
  CollectSubtree(guid, &doomed);
  for (const std::string& g : doomed) {
    DeleteTab(g);
  }
  if (parent) {
    NormalizeIndexes(MutableChildren(*parent), env_.now());
  }
  return Flush();
}

// --- Sync landing ------------------------------------------------------------------

WriteError LocalStore::ApplyBookmarkSyncBatch(
    const std::vector<BookmarkSyncOp>& ops) {
  if (!db_) {
    return WriteError::kUnavailable;
  }
  if (ops.empty()) {
    return WriteError::kNone;
  }
  using Kind = BookmarkSyncOp::Kind;
  // The Space roots first (their own writes): nothing below may flush.
  std::map<std::string, std::string> root_of_space;
  auto profile_of_space = [&](const std::string& space_id,
                              const std::string& fallback) {
    const Space* space = FindSpace(space_id);
    return space ? space->profile_id : fallback;
  };
  for (const BookmarkSyncOp& op : ops) {
    std::string space_id;
    std::string profile_id;
    if (op.kind == Kind::kCreate) {
      space_id = op.space_id;
      profile_id = op.profile_id;
    } else if (op.kind == Kind::kMove) {
      space_id = op.space_id;
      const Tab* row = FindTab(op.guid);
      profile_id = profile_of_space(
          space_id, row ? row->profile_id.value_or(kDefaultProfileId)
                        : std::string(kDefaultProfileId));
    } else {
      continue;
    }
    if (!root_of_space.count(space_id)) {
      const std::string root = BookmarkRoot(profile_id, space_id, true);
      if (root.empty()) {
        return WriteError::kRowNotFound;
      }
      root_of_space[space_id] = root;
    }
  }
  const TimeMs now = env_.now();
  std::set<std::string> touched_parents;
  auto fail = [&](WriteError error) {
    Discard();
    return error;
  };
  auto bookmark_row = [&](const std::string& guid) -> Tab* {
    auto it = tabs_.find(guid);
    return it != tabs_.end() && it->second.is_bookmark_kind() &&
                   !IsBookmarkRoot(guid)
               ? &it->second
               : nullptr;
  };
  auto parent_for = [&](const std::optional<std::string>& parent_guid,
                        const std::string& space_id) -> std::string {
    if (parent_guid) {
      const Tab* parent = FindTab(*parent_guid);
      return parent && parent->type == RowType::kBookmarkFolder ? *parent_guid
                                                                : std::string();
    }
    const auto root = root_of_space.find(space_id);
    return root == root_of_space.end() ? std::string() : root->second;
  };
  for (const BookmarkSyncOp& op : ops) {
    switch (op.kind) {
      case Kind::kCreate: {
        if (FindTab(op.guid)) {
          return fail(WriteError::kRowAlreadyMapped);
        }
        const std::string parent = parent_for(op.parent_guid, op.space_id);
        if (parent.empty()) {
          return fail(WriteError::kRowNotFound);
        }
        Tab node;
        node.guid = op.guid;
        node.type = op.is_folder ? RowType::kBookmarkFolder : RowType::kBookmark;
        node.title = op.title;
        node.url = op.is_folder ? std::string(kFolderPlaceholderUrl) : op.url;
        node.created = op.created > 0 ? op.created : now;
        node.updated = now;
        node.space_id = op.space_id;
        node.profile_id = op.profile_id;
        node.source = op.source;
        node.secondary_url = op.secondary_url;
        node.secondary_title = op.secondary_title;
        node.sync_id = op.sync_id;
        node.content_updated = op.content_updated;
        node.index = op.index;
        node.parent_guid = parent;
        EnsureProfile(op.profile_id);
        PutTab(node);
        touched_parents.insert(parent);
        break;
      }
      case Kind::kClaim: {
        Tab* row = bookmark_row(op.guid);
        if (!row) {
          return fail(WriteError::kRowNotFound);
        }
        if (row->sync_id && row->sync_id != op.sync_id) {
          return fail(WriteError::kRowAlreadyMapped);
        }
        row->sync_id = op.sync_id;  // claiming is not editing
        PutTab(*row);
        break;
      }
      case Kind::kMove: {
        Tab* row = bookmark_row(op.guid);
        if (!row) {
          return fail(WriteError::kRowNotFound);
        }
        const std::string parent = parent_for(op.parent_guid, op.space_id);
        // Strict: a named parent must exist; never into itself or below.
        if (parent.empty() || parent == op.guid) {
          return fail(WriteError::kRowNotFound);
        }
        if (const Tab* p = FindTab(parent); p && HasAncestor(*p, {op.guid})) {
          return fail(WriteError::kRowNotFound);
        }
        if (row->parent_guid) {
          touched_parents.insert(*row->parent_guid);
        }
        const std::string target_profile = profile_of_space(
            op.space_id, row->profile_id.value_or(kDefaultProfileId));
        row->parent_guid = parent;
        row->index = op.index;
        row->updated = now;
        if (op.records_location_edit) {
          row->location_updated = now;
        }
        PutTab(*row);
        if (row->space_id != op.space_id || row->profile_id != target_profile) {
          RetagSubtree(op.guid, target_profile, op.space_id, now);
        }
        touched_parents.insert(parent);
        break;
      }
      case Kind::kUpdate: {
        Tab* row = bookmark_row(op.guid);
        if (!row) {
          return fail(WriteError::kRowNotFound);
        }
        bool changed = false;
        if (op.new_title && row->title != *op.new_title) {
          row->title = *op.new_title;
          changed = true;
        }
        if (op.new_url && !op.new_url->empty() && row->url != *op.new_url &&
            row->type == RowType::kBookmark) {
          row->url = *op.new_url;
          row->need_update_metadata = true;
          changed = true;
        }
        if (op.new_secondary_url && row->secondary_url != *op.new_secondary_url) {
          row->secondary_url = *op.new_secondary_url;
          changed = true;
        }
        if (op.new_secondary_title &&
            row->secondary_title != *op.new_secondary_title) {
          row->secondary_title = *op.new_secondary_title;
          changed = true;
        }
        if (changed) {
          row->content_updated = now;
          row->updated = now;
          PutTab(*row);
        }
        break;
      }
      case Kind::kDelete: {
        Tab* row = bookmark_row(op.guid);
        if (!row) {
          return fail(WriteError::kRowNotFound);
        }
        // Every child must be gone or lifted first (R-M3-3-17).
        if (row->type == RowType::kBookmarkFolder &&
            !MutableChildren(op.guid).empty()) {
          return fail(WriteError::kFolderNotEmpty);
        }
        if (row->parent_guid) {
          touched_parents.insert(*row->parent_guid);
        }
        DeleteTab(op.guid);
        break;
      }
    }
  }
  for (const std::string& parent : touched_parents) {
    if (FindTab(parent)) {
      NormalizeIndexes(MutableChildren(parent), now);
    }
  }
  return Flush() ? WriteError::kNone : WriteError::kIo;
}

WriteError LocalStore::ApplyPinSyncBatch(const std::vector<PinSyncOp>& ops) {
  if (!db_) {
    return WriteError::kUnavailable;
  }
  if (ops.empty()) {
    return WriteError::kNone;
  }
  using Kind = PinSyncOp::Kind;
  const TimeMs now = env_.now();
  std::set<Owner> touched;
  auto fail = [&](WriteError error) {
    Discard();
    return error;
  };
  auto pin_row = [&](const std::string& guid) -> Tab* {
    auto it = tabs_.find(guid);
    return it != tabs_.end() && it->second.type == RowType::kPinnedTab
               ? &it->second
               : nullptr;
  };
  for (const PinSyncOp& op : ops) {
    switch (op.kind) {
      case Kind::kCreate: {
        if (FindTab(op.guid)) {
          return fail(WriteError::kRowAlreadyMapped);
        }
        if (op.url.empty()) {
          return fail(WriteError::kInvalidUrl);
        }
        const Owner owner =
            OwnerFor(pin_scope_, op.profile_id.value_or(kDefaultProfileId),
                     op.space_id.value_or(kDefaultSpaceId));
        Tab tab;
        tab.guid = op.guid;
        tab.type = RowType::kPinnedTab;
        tab.title = op.title;
        tab.url = op.url;
        tab.created = op.created > 0 ? op.created : now;
        tab.updated = now;
        tab.pin_lineage_id = op.lineage_id.empty() ? op.guid : op.lineage_id;
        tab.source = op.source;
        tab.content_updated = op.content_updated;
        tab.index = op.index;
        ApplyOwner(tab, owner);
        PutTab(tab);
        touched.insert(owner);
        break;
      }
      case Kind::kRelineage: {
        Tab* row = pin_row(op.guid);
        if (!row) {
          return fail(WriteError::kRowNotFound);
        }
        row->pin_lineage_id = op.lineage_id;  // identity, not position
        row->updated = now;
        PutTab(*row);
        break;
      }
      case Kind::kMove: {
        Tab* row = pin_row(op.guid);
        if (!row) {
          return fail(WriteError::kRowNotFound);
        }
        if (!BelongsTo(*row, pin_scope_)) {
          return fail(WriteError::kNotInActiveScope);
        }
        if (row->index != op.index) {
          row->index = op.index;
          row->updated = now;
          PutTab(*row);
        }
        touched.insert(OwnerOf(*row, pin_scope_));
        break;
      }
      case Kind::kUpdate: {
        Tab* row = pin_row(op.guid);
        if (!row) {
          return fail(WriteError::kRowNotFound);
        }
        if (!BelongsTo(*row, pin_scope_)) {
          return fail(WriteError::kNotInActiveScope);
        }
        const bool url_given = op.new_url && !op.new_url->empty();
        if (!op.new_title && !url_given && !op.split_partner_lineage) {
          return fail(WriteError::kNoCandidateSurvived);
        }
        bool content_changed = false;
        if (op.new_title && row->title != *op.new_title) {
          row->title = *op.new_title;
          content_changed = true;
        }
        if (url_given && row->url != *op.new_url) {
          row->url = *op.new_url;
          row->need_update_metadata = true;
          content_changed = true;
        }
        if (content_changed) {
          row->content_updated = now;
        }
        row->updated = now;
        PutTab(*row);
        if (op.split_partner_lineage) {
          // The partner by lineage among this owner's pins.
          std::optional<std::string> partner_guid;
          if (*op.split_partner_lineage) {
            std::string wanted = **op.split_partner_lineage;
            std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            for (Tab* sibling : MutableActivePins(OwnerOf(*row, pin_scope_))) {
              std::string lineage = LineageOf(*sibling);
              std::transform(lineage.begin(), lineage.end(), lineage.begin(),
                             [](unsigned char c) { return std::tolower(c); });
              if (sibling->guid != op.guid && lineage == wanted) {
                partner_guid = sibling->guid;
                break;
              }
            }
          }
          // The old partner's back link goes only if it points here.
          if (row->split_partner_guid && row->split_partner_guid != partner_guid) {
            if (Tab* previous = pin_row(*row->split_partner_guid);
                previous && previous->split_partner_guid == op.guid) {
              previous->split_partner_guid.reset();
              previous->updated = now;
              PutTab(*previous);
            }
          }
          row->split_partner_guid = partner_guid;
          PutTab(*row);
          if (partner_guid) {
            if (Tab* partner = pin_row(*partner_guid)) {
              partner->split_partner_guid = op.guid;
              partner->updated = now;
              PutTab(*partner);
            }
          }
        }
        break;
      }
      case Kind::kDelete: {
        Tab* row = pin_row(op.guid);
        if (!row) {
          return fail(WriteError::kRowNotFound);
        }
        touched.insert(OwnerOf(*row, pin_scope_));
        if (row->split_partner_guid && *row->split_partner_guid != op.guid) {
          if (Tab* partner = pin_row(*row->split_partner_guid);
              partner && partner->split_partner_guid == op.guid) {
            partner->split_partner_guid.reset();
            partner->layout.reset();
            partner->updated = now;
            PutTab(*partner);
          }
        }
        DeleteTab(op.guid);
        break;
      }
    }
  }
  for (const Owner& owner : touched) {
    std::vector<Tab*> pins = MutableActivePins(owner);
    for (size_t i = 0; i < pins.size(); ++i) {
      if (pins[i]->index != static_cast<int>(i)) {
        pins[i]->index = static_cast<int>(i);
        pins[i]->updated = now;
        PutTab(*pins[i]);
      }
    }
  }
  return Flush() ? WriteError::kNone : WriteError::kIo;
}

bool LocalStore::ClearBookmarkSyncIds() {
  if (!db_) {
    return false;
  }
  for (auto& [guid, tab] : tabs_) {
    if (tab.is_bookmark_kind() && tab.sync_id) {
      tab.sync_id.reset();
      PutTab(tab);
    }
  }
  return Flush();
}

WriteError LocalStore::ReplaceUrlRulesForSync(const std::vector<UrlRule>& rows) {
  if (!db_) {
    return WriteError::kUnavailable;
  }
  std::set<std::string> kept;
  for (const UrlRule& rule : rows) {
    kept.insert(rule.id);
    const auto it = rules_.find(rule.id);
    if (it == rules_.end() || !(it->second == rule)) {
      PutRule(rule);
    }
  }
  std::vector<std::string> removed;
  for (const auto& [id, rule] : rules_) {
    if (!kept.count(id)) {
      removed.push_back(id);
    }
  }
  for (const std::string& id : removed) {
    DeleteRule(id);
  }
  return Flush() ? WriteError::kNone : WriteError::kIo;
}

// --- Any row -----------------------------------------------------------------------

bool LocalStore::SetFavicon(const std::string& guid,
                            bool pane_b,
                            const std::string& bgra,
                            int width,
                            int height) {
  auto it = tabs_.find(guid);
  if (!db_ || it == tabs_.end()) {
    return false;
  }
  Tab& t = it->second;
  std::string& bytes = pane_b ? t.secondary_favicon : t.favicon;
  int& w = pane_b ? t.secondary_favicon_width : t.favicon_width;
  int& h = pane_b ? t.secondary_favicon_height : t.favicon_height;
  if (bytes == bgra && w == width && h == height) {
    return true;
  }
  bytes = bgra;
  w = width;
  h = height;
  t.updated = env_.now();
  PutTab(t);
  quiet_flush_ = true;
  const bool ok = Flush();
  quiet_flush_ = false;
  return ok;
}

bool LocalStore::SetLastSeen(const std::string& guid, TimeMs seen_at) {
  auto it = tabs_.find(guid);
  if (!db_ || it == tabs_.end() ||
      (it->second.type != RowType::kPinnedTab &&
       it->second.type != RowType::kBookmark)) {
    return false;
  }
  it->second.last_seen = seen_at;
  it->second.updated = seen_at;
  PutTab(it->second);
  quiet_flush_ = true;
  const bool ok = Flush();
  quiet_flush_ = false;
  return ok;
}

// --- Spaces --------------------------------------------------------------------------

bool LocalStore::EnsureDefaultSpace(const std::string& profile_id) {
  if (!db_) {
    return false;
  }
  auto it = spaces_.find(kDefaultSpaceId);
  if (it == spaces_.end()) {
    if (!spaces_.empty()) {
      // The row was deleted and its role moved: never resurrect it.
      return true;
    }
    const TimeMs now = env_.now();
    Space space;
    space.space_id = kDefaultSpaceId;
    space.profile_id = profile_id;
    space.name = kDefaultSpaceName;
    space.color_hex = kDefaultSpaceColorHex;
    space.icon_name = kDefaultSpaceIconName;
    space.sort_order = 0;
    space.created = now;
    space.updated = now;
    PutSpace(space);
    it = spaces_.find(kDefaultSpaceId);
  }
  // Backlink an existing Profile root.
  if (!it->second.bookmark_root) {
    const Profile* profile = FindProfile(it->second.profile_id);
    if (profile && profile->bookmark_root) {
      it->second.bookmark_root = profile->bookmark_root;
      PutSpace(it->second);
    }
  }
  return Flush();
}

bool LocalStore::CreateSpace(const std::string& space_id,
                             const std::string& profile_id,
                             const std::string& name,
                             const std::string& color_hex,
                             const std::string& icon_name,
                             std::optional<TimeMs> created) {
  if (!db_ || space_id.empty() || profile_id.empty() || FindSpace(space_id)) {
    return false;
  }
  int next_order = 0;
  for (const auto& [id, space] : spaces_) {
    next_order = std::max(next_order, space.sort_order + 1);
  }
  const TimeMs now = env_.now();
  Space space;
  space.space_id = space_id;
  space.profile_id = profile_id;
  space.name = name;
  space.color_hex = color_hex;
  space.icon_name = icon_name;
  space.sort_order = next_order;
  space.created = created.value_or(now);
  space.updated = now;
  PutSpace(space);
  EnsureProfile(profile_id);
  if (!Flush()) {
    return false;
  }
  // The root is materialised at once, so the first bookmark write finds it.
  return !BookmarkRoot(profile_id, space_id, /*create=*/true).empty();
}

bool LocalStore::UpdateSpace(const std::string& space_id,
                             const std::optional<std::string>& name,
                             const std::optional<std::string>& color_hex,
                             const std::optional<std::string>& icon_name,
                             std::optional<TimeMs> created) {
  auto it = spaces_.find(space_id);
  if (!db_ || it == spaces_.end()) {
    return false;
  }
  if (created) {
    it->second.created = *created;
  }
  if (name) {
    it->second.name = *name;
  }
  if (color_hex) {
    it->second.color_hex = *color_hex;
  }
  if (icon_name) {
    it->second.icon_name = *icon_name;
  }
  it->second.updated = env_.now();
  PutSpace(it->second);
  return Flush();
}

bool LocalStore::ChangeSpaceProfile(const std::string& space_id,
                                    const std::string& profile_id) {
  auto it = spaces_.find(space_id);
  if (!db_ || space_id == kDefaultSpaceId || it == spaces_.end() ||
      profile_id.empty() || it->second.profile_id == profile_id) {
    return false;
  }
  EnsureProfile(profile_id);
  const bool includes_pins = pin_scope_ == PinScope::kSpace;
  for (auto& [guid, tab] : tabs_) {
    if (tab.space_id != space_id) {
      continue;
    }
    if (tab.is_bookmark_kind() ||
        (includes_pins && tab.type == RowType::kPinnedTab)) {
      tab.profile_id = profile_id;
      PutTab(tab);
    }
  }
  it->second.profile_id = profile_id;
  it->second.updated = env_.now();
  PutSpace(it->second);
  return Flush();
}

bool LocalStore::DeleteSpaceCascade(const std::string& space_id,
                                    bool user_intent) {
  if (!db_ || !FindSpace(space_id)) {
    return false;
  }
  const TimeMs now = env_.now();
  std::vector<std::string> doomed;
  for (const auto& [guid, tab] : tabs_) {
    if (tab.space_id == space_id) {
      doomed.push_back(guid);
    }
  }
  for (const std::string& guid : doomed) {
    DeleteTab(guid);
  }
  std::vector<std::string> purge;
  for (auto& [id, rule] : rules_) {
    if (rule.space_id != space_id) {
      continue;
    }
    if (user_intent) {
      // A soft delete, so sync can publish the tombstone. Never marks the
      // rule edited: deletion is not editing.
      if (!rule.deleted) {
        rule.deleted = now;
        PutRule(rule);
      }
    } else {
      purge.push_back(id);
    }
  }
  for (const std::string& id : purge) {
    DeleteRule(id);
  }
  // A Profile root pointer into the deleted rows would dangle.
  for (auto& [id, profile] : profiles_) {
    if (profile.bookmark_root && !FindTab(*profile.bookmark_root)) {
      profile.bookmark_root.reset();
      dirty_.profiles.insert(id);
    }
  }
  DeleteSpace(space_id);
  return Flush();
}

bool LocalStore::ReorderSpaces(const std::vector<std::string>& ordered) {
  if (!db_) {
    return false;
  }
  for (size_t i = 0; i < ordered.size(); ++i) {
    auto it = spaces_.find(ordered[i]);
    if (it != spaces_.end() && it->second.sort_order != static_cast<int>(i)) {
      it->second.sort_order = static_cast<int>(i);
      PutSpace(it->second);
    }
  }
  return Flush();
}

// --- URL rules -------------------------------------------------------------------------

WriteError LocalStore::ApplyUrlRuleEdits(
    const std::vector<UrlRuleDraft>& upserts,
    const std::set<std::string>& deleted_ids) {
  if (!db_) {
    return WriteError::kUnavailable;
  }
  // Step 1: one `now`, the whole table (soft-deleted rows included), two
  // indexes.
  const TimeMs now = env_.now();
  std::map<std::string, std::string> id_by_sync;
  for (const auto& [id, rule] : rules_) {
    if (rule.sync_id) {
      id_by_sync[*rule.sync_id] = id;
    }
  }
  std::set<std::string> touched_buckets;
  std::map<std::string, int> placements;
  std::set<std::string> inserted_ids;
  std::vector<std::string> upserted_ids;
  std::set<std::string> wrote_unit;

  const auto abort = [&](WriteError e) {
    Discard();
    return e;
  };

  for (const UrlRuleDraft& draft : upserts) {
    const std::string draft_id = draft.id.empty() ? env_.new_uuid() : draft.id;
    // Step 2: by id, then by syncId (which adopts the draft's id).
    std::string located;
    bool adopts_draft_id = false;
    if (rules_.count(draft_id)) {
      located = draft_id;
    } else if (draft.sync_id && id_by_sync.count(*draft.sync_id)) {
      located = id_by_sync[*draft.sync_id];
      adopts_draft_id = true;
    }
    // Step 2b: a soft-deleted match is left for its tombstone; the editor
    // gets a fresh row instead. Sync must never come through here.
    bool mints_fresh_id = false;
    if (!located.empty() && rules_[located].deleted) {
      if (draft.sync_id) {
        return abort(WriteError::kRowAlreadyMapped);
      }
      located.clear();
      mints_fresh_id = true;
    }

    if (!located.empty()) {
      UrlRule row = rules_[located];
      // Step 3: a row claims one identity, once.
      if (draft.sync_id) {
        if (row.sync_id && *row.sync_id != *draft.sync_id) {
          return abort(WriteError::kRowAlreadyMapped);
        }
        const auto owner = id_by_sync.find(*draft.sync_id);
        if (owner != id_by_sync.end() && owner->second != row.id) {
          return abort(WriteError::kRowAlreadyMapped);
        }
        if (!row.sync_id) {
          // Claiming is not editing: no stamp, no pending flag.
          row.sync_id = draft.sync_id;
          id_by_sync[*draft.sync_id] = row.id;
        }
      }
      if (adopts_draft_id) {
        DeleteRule(row.id);
        if (row.sync_id) {
          id_by_sync[*row.sync_id] = draft_id;
        }
        row.id = draft_id;
      }
      const std::string row_id = row.id;
      upserted_ids.push_back(row_id);
      // Step 4: the content group shares one stamp; equal content writes
      // nothing.
      if (draft.content) {
        const std::string host = NormalizedRuleHost(draft.content->host);
        const std::optional<std::string> path =
            NormalizedRulePathPrefix(draft.content->path_prefix);
        bool changed = false;
        if (row.host != host) {
          row.host = host;
          changed = true;
        }
        if (row.path_prefix != path) {
          row.path_prefix = path;
          changed = true;
        }
        if (row.ask != draft.content->ask) {
          row.ask = draft.content->ask;
          changed = true;
        }
        if (changed) {
          row.content_updated = now;
          wrote_unit.insert(row_id);
        }
      }
      // Step 5: the target.
      if (draft.space_id) {
        if (*draft.space_id != row.space_id) {
          touched_buckets.insert(row.space_id);
          row.space_id = *draft.space_id;
          row.target_updated = now;
          wrote_unit.insert(row_id);
        }
        touched_buckets.insert(*draft.space_id);
      } else {
        touched_buckets.insert(row.space_id);
      }
      if (draft.sort_order) {
        placements[row_id] = *draft.sort_order;
      }
      PutRule(row);
      continue;
    }

    // Step 6: insert. A new row cannot inherit omitted units.
    if (!draft.content || !draft.space_id) {
      return abort(WriteError::kNoCandidateSurvived);
    }
    const std::string sync_id =
        draft.sync_id ? *draft.sync_id : LowercaseAscii(env_.new_uuid());
    if (id_by_sync.count(sync_id)) {
      return abort(WriteError::kRowAlreadyMapped);
    }
    const std::string id =
        mints_fresh_id ? LowercaseAscii(env_.new_uuid()) : draft_id;
    UrlRule row;
    row.id = id;
    row.space_id = *draft.space_id;
    row.host = NormalizedRuleHost(draft.content->host);
    row.path_prefix = NormalizedRulePathPrefix(draft.content->path_prefix);
    row.ask = draft.content->ask;
    row.sort_order = INT_MAX;
    row.created = draft.created.value_or(now);
    row.sync_id = sync_id;
    // No content stamp for a new row: its projection falls back to
    // createdDate.
    row.content_updated = draft.content_updated;
    row.pending_local_edit = true;
    PutRule(row);
    id_by_sync[sync_id] = id;
    inserted_ids.insert(id);
    upserted_ids.push_back(id);
    if (draft.sort_order) {
      placements[id] = *draft.sort_order;
    }
    touched_buckets.insert(*draft.space_id);
  }

  // Step 7: soft deletes; absent rows are ignored.
  for (const std::string& id : deleted_ids) {
    auto it = rules_.find(id);
    if (it == rules_.end() || it->second.deleted) {
      continue;
    }
    it->second.deleted = now;
    touched_buckets.insert(it->second.space_id);
    PutRule(it->second);
  }

  // Step 8: densify each touched bucket's live rows.
  std::set<std::string> reordered_ids;
  for (const std::string& bucket : touched_buckets) {
    std::vector<UrlRule*> sequence;
    std::vector<UrlRule*> placed;
    for (auto& [id, rule] : rules_) {
      if (rule.space_id != bucket || rule.deleted) {
        continue;
      }
      (placements.count(id) ? placed : sequence).push_back(&rule);
    }
    std::sort(sequence.begin(), sequence.end(),
              [&](const UrlRule* a, const UrlRule* b) {
                const int l = inserted_ids.count(a->id) ? INT_MAX
                                                        : a->sort_order;
                const int r = inserted_ids.count(b->id) ? INT_MAX
                                                        : b->sort_order;
                return l != r ? l < r : a->id < b->id;
              });
    std::sort(placed.begin(), placed.end(),
              [&](const UrlRule* a, const UrlRule* b) {
                const int l = placements[a->id];
                const int r = placements[b->id];
                return l != r ? l < r : a->id < b->id;
              });
    for (UrlRule* rule : placed) {
      const int count = static_cast<int>(sequence.size());
      const int requested = placements[rule->id];
      sequence.insert(sequence.begin() + Clamp(requested, 0, count), rule);
    }
    for (size_t i = 0; i < sequence.size(); ++i) {
      if (sequence[i]->sort_order != static_cast<int>(i)) {
        sequence[i]->sort_order = static_cast<int>(i);
        reordered_ids.insert(sequence[i]->id);
        dirty_.rules.insert(sequence[i]->id);
        dirty_.rules_deleted.erase(sequence[i]->id);
      }
    }
  }

  // Step 9: pending only where a unit or the position really changed.
  for (const std::string& id : upserted_ids) {
    if (!wrote_unit.count(id) && !reordered_ids.count(id)) {
      continue;
    }
    auto it = rules_.find(id);
    if (it != rules_.end() && !it->second.pending_local_edit) {
      it->second.pending_local_edit = true;
      PutRule(it->second);
    }
  }
  return Flush() ? WriteError::kNone : WriteError::kIo;
}

}  // namespace phi_client::store

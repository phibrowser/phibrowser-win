// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_sync_owned_win.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <tuple>
#include <utility>

#include "base/json/json_writer.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_file_io.h"
#include "phibrowser-win/client/phi_key_crypto.h"
#include "phibrowser-win/client/phi_local_store.h"

namespace phi_client {

namespace {

// --- The table file ------------------------------------------------------------------

bool EnsureParent(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return true;
  }
  const std::wstring dir = path.substr(0, slash);
  return ::CreateDirectoryW(dir.c_str(), nullptr) ||
         ::GetLastError() == ERROR_ALREADY_EXISTS;
}

void SetBytes(base::DictValue& dict,
              std::string_view key,
              const std::optional<std::string>& bytes) {
  if (bytes) {
    dict.Set(key, keys::Base64Encode(keys::Bytes(bytes->begin(), bytes->end())));
  }
}

std::optional<std::string> FindBytes(const base::DictValue& dict,
                                     std::string_view key) {
  const std::string* text = dict.FindString(key);
  if (!text) {
    return std::nullopt;
  }
  const std::optional<keys::Bytes> bytes = keys::Base64Decode(*text);
  return bytes ? std::optional<std::string>(std::string(bytes->begin(), bytes->end()))
               : std::nullopt;
}

std::optional<std::string> FindText(const base::DictValue& dict,
                                    std::string_view key) {
  const std::string* text = dict.FindString(key);
  return text ? std::optional<std::string>(*text) : std::nullopt;
}

std::optional<int64_t> FindInt64(const base::DictValue& dict,
                                 std::string_view key) {
  const std::string* text = dict.FindString(key);
  int64_t value = 0;
  return text && base::StringToInt64(*text, &value)
             ? std::optional<int64_t>(value)
             : std::nullopt;
}

base::DictValue CursorToDict(const sync::OwnedCursor& cursor) {
  base::DictValue dict;
  dict.Set("entityId", cursor.entity_id);
  dict.Set("version", base::NumberToString(cursor.version));
  SetBytes(dict, "reconciled", cursor.reconciled);
  SetBytes(dict, "server", cursor.server);
  if (cursor.owner_uuid) {
    dict.Set("ownerUuid", *cursor.owner_uuid);
  }
  SetBytes(dict, "pendingApply", cursor.pending_apply);
  if (cursor.pending_owner_uuid) {
    dict.Set("pendingOwnerUuid", *cursor.pending_owner_uuid);
  }
  // Written only when set, like the Mac's optional: older files read false.
  if (cursor.parked_for_scope) {
    dict.Set("parkedForScope", true);
  }
  dict.Set("pendingTombstone", cursor.pending_tombstone);
  if (cursor.pending_partner_lineage) {
    dict.Set("pendingPartnerLineage", *cursor.pending_partner_lineage);
  }
  dict.Set("pendingDelete", cursor.pending_delete);
  dict.Set("deleteDecidedAtMs", base::NumberToString(cursor.delete_decided_at_ms));
  dict.Set("deleteRejectRounds", cursor.delete_reject_rounds);
  if (cursor.rekey_reject_rounds) {
    dict.Set("rekeyRejectRounds", *cursor.rekey_reject_rounds);
  }
  if (cursor.deleted_at_ms) {
    dict.Set("deletedAtMs", base::NumberToString(*cursor.deleted_at_ms));
  }
  return dict;
}

sync::OwnedCursor CursorFromDict(const base::DictValue& dict) {
  sync::OwnedCursor cursor;
  cursor.entity_id = FindText(dict, "entityId").value_or(std::string());
  cursor.version = FindInt64(dict, "version").value_or(0);
  cursor.reconciled = FindBytes(dict, "reconciled");
  cursor.server = FindBytes(dict, "server");
  cursor.owner_uuid = FindText(dict, "ownerUuid");
  cursor.pending_apply = FindBytes(dict, "pendingApply");
  cursor.pending_owner_uuid = FindText(dict, "pendingOwnerUuid");
  cursor.parked_for_scope = dict.FindBool("parkedForScope").value_or(false);
  cursor.pending_tombstone = dict.FindBool("pendingTombstone").value_or(false);
  cursor.pending_partner_lineage = FindText(dict, "pendingPartnerLineage");
  cursor.pending_delete = dict.FindBool("pendingDelete").value_or(false);
  cursor.delete_decided_at_ms = FindInt64(dict, "deleteDecidedAtMs").value_or(0);
  cursor.delete_reject_rounds = dict.FindInt("deleteRejectRounds").value_or(0);
  cursor.rekey_reject_rounds = dict.FindInt("rekeyRejectRounds");
  cursor.deleted_at_ms = FindInt64(dict, "deletedAtMs");
  return cursor;
}

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

// --- Store rows as sync sees them ---------------------------------------------------

sync::LocalBookmark BookmarkFrom(const store::Tab& tab,
                                 const std::optional<std::string>& parent) {
  sync::LocalBookmark row;
  row.sync_id = tab.sync_id;
  row.guid = tab.guid;
  row.space_id = tab.space_id.value_or(store::kDefaultSpaceId);
  row.profile_id = tab.profile_id.value_or(store::kDefaultProfileId);
  row.parent_guid = parent;
  row.index = tab.index;
  row.is_folder = tab.type == store::RowType::kBookmarkFolder;
  row.title = tab.title;
  row.url = tab.url;
  row.secondary_url = tab.secondary_url;
  row.secondary_title = tab.secondary_title;
  row.source = tab.source;
  row.created_ms = tab.created;
  row.content_updated_ms = tab.content_updated;
  row.location_updated_ms = tab.location_updated;
  return row;
}

bool PinBelongs(const store::Tab& tab, store::PinScope scope) {
  switch (scope) {
    case store::PinScope::kSpace:
      return tab.space_id.has_value();
    case store::PinScope::kProfile:
      return tab.profile_id.has_value() && !tab.space_id.has_value();
    case store::PinScope::kApp:
      break;
  }
  return !tab.profile_id.has_value() && !tab.space_id.has_value();
}

sync::LocalPin PinFrom(const store::Tab& tab,
                       const std::map<std::string, std::string>& lineage_by_guid) {
  sync::LocalPin row;
  row.lineage_id = tab.pin_lineage_id.value_or(tab.guid);
  row.guid = tab.guid;
  row.space_id = tab.space_id;
  row.profile_id = tab.profile_id;
  row.index = tab.index;
  row.title = tab.title;
  row.url = tab.url;
  if (tab.split_partner_guid) {
    const auto it = lineage_by_guid.find(*tab.split_partner_guid);
    if (it != lineage_by_guid.end()) {
      row.split_partner_lineage_id = it->second;
    }
  }
  row.source = tab.source;
  row.created_ms = tab.created;
  row.content_updated_ms = tab.content_updated;
  row.is_dormant = tab.pin_dormant;
  return row;
}

sync::LocalUrlRule RuleFrom(const store::UrlRule& rule) {
  sync::LocalUrlRule row;
  row.id = rule.id;
  row.sync_id = rule.sync_id;
  row.space_id = rule.space_id;
  row.host = rule.host;
  row.path_prefix = rule.path_prefix;
  row.ask = rule.ask;
  row.sort_order = rule.sort_order;
  row.created_ms = rule.created;
  row.content_updated_ms = rule.content_updated;
  row.target_updated_ms = rule.target_updated;
  row.deleted_ms = rule.deleted;
  row.pending_local_edit = rule.pending_local_edit;
  row.merge_partner_sync_id = rule.merge_partner_sync_id;
  return row;
}

store::UrlRule RuleTo(const sync::LocalUrlRule& row) {
  store::UrlRule rule;
  rule.id = row.id;
  rule.space_id = row.space_id;
  rule.host = row.host;
  rule.path_prefix = row.path_prefix;
  rule.ask = row.ask;
  rule.sort_order = row.sort_order;
  rule.created = row.created_ms;
  rule.sync_id = row.sync_id;
  rule.content_updated = row.content_updated_ms;
  rule.target_updated = row.target_updated_ms;
  rule.deleted = row.deleted_ms;
  rule.pending_local_edit = row.pending_local_edit;
  rule.merge_partner_sync_id = row.merge_partner_sync_id;
  return rule;
}

std::vector<sync::LocalUrlRule> RulesOf(const store::LocalStore& store) {
  std::vector<sync::LocalUrlRule> rows;
  for (const store::UrlRule& rule : store.UrlRules(/*include_deleted=*/true)) {
    rows.push_back(RuleFrom(rule));
  }
  std::sort(rows.begin(), rows.end(),
            [](const sync::LocalUrlRule& a, const sync::LocalUrlRule& b) {
              return std::tie(a.space_id, a.sort_order, a.id) <
                     std::tie(b.space_id, b.sort_order, b.id);
            });
  return rows;
}

}  // namespace

// --- PhiOwnedTableFile ---------------------------------------------------------------

PhiOwnedTableFile::PhiOwnedTableFile(std::wstring path) : path_(std::move(path)) {}

PhiOwnedTableFile::~PhiOwnedTableFile() = default;

// A missing, unreadable, older, newer or empty table reads as empty and
// reports a loss when the kind had records; it is never written back here.
// A locked table or a newer build's is not saved over either (the Space
// table's rule) until a Load reads one this build owns.
sync::OwnedTable PhiOwnedTableFile::Load(bool had_records, bool* reported_loss) {
  std::lock_guard<std::mutex> hold(lock_);
  *reported_loss = had_records;
  base::DictValue dict;
  sync::OwnedTable table;
  const PhiJsonFileStatus status = PhiReadJsonFile(path_, &dict);
  const int format = dict.FindInt("formatVersion").value_or(0);
  save_blocked_ = status == PhiJsonFileStatus::kUnreadable ||
                  (status == PhiJsonFileStatus::kOk &&
                   format > sync::OwnedTable::kFormatVersion);
  if (status != PhiJsonFileStatus::kOk ||
      format != sync::OwnedTable::kFormatVersion) {
    return sync::OwnedTable();
  }
  if (const base::DictValue* cursors = dict.FindDict("cursors")) {
    for (const auto [identity, value] : *cursors) {
      if (value.is_dict()) {
        table.cursors[identity] = CursorFromDict(value.GetDict());
      }
    }
  }
  if (table.cursors.empty()) {
    return sync::OwnedTable();
  }
  *reported_loss = false;
  return table;
}

bool PhiOwnedTableFile::Save(const sync::OwnedTable& table) {
  std::lock_guard<std::mutex> hold(lock_);
  if (save_blocked_) {
    return false;
  }
  base::DictValue dict;
  dict.Set("formatVersion", table.format_version);
  base::DictValue cursors;
  for (const auto& [identity, cursor] : table.cursors) {
    cursors.Set(identity, CursorToDict(cursor));
  }
  dict.Set("cursors", std::move(cursors));
  std::string json;
  return base::JSONWriter::WriteWithOptions(
             dict, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json) &&
         EnsureParent(path_) && PhiWriteFileAtomic(path_, json);
}

bool PhiOwnedTableFile::Remove() {
  std::lock_guard<std::mutex> hold(lock_);
  if (!PhiRemoveFileIfPresent(path_)) {
    return false;
  }
  save_blocked_ = false;
  return true;
}

// --- Bookmarks -------------------------------------------------------------------------

PhiSyncBookmarkAccess::PhiSyncBookmarkAccess(PhiRunOnUi run_on_ui,
                                             PhiOwnedSyncHost host)
    : run_on_ui_(std::move(run_on_ui)), host_(std::move(host)) {}

PhiSyncBookmarkAccess::~PhiSyncBookmarkAccess() = default;

// The canonical Space roots' trees; a root's children carry parent nullopt.
PhiSyncBookmarkAccess::Snapshot PhiSyncBookmarkAccess::Read(
    const store::LocalStore& store) {
  Snapshot snapshot;
  std::map<std::string, std::vector<const store::Tab*>> children;
  for (const auto& [guid, tab] : store.tabs()) {
    if (!tab.is_bookmark_kind()) {
      continue;
    }
    if (tab.sync_id) {
      snapshot.sync_ids.insert(*tab.sync_id);
    }
    if (tab.parent_guid) {
      children[*tab.parent_guid].push_back(&tab);
    }
  }
  struct Level {
    // Into the store's rows, read on this (UI) thread only.
    RAW_PTR_EXCLUSION const store::Tab* tab;
    std::optional<std::string> parent;
  };
  std::vector<Level> queue;
  std::set<std::string> roots;
  for (const store::Space& space : store.Spaces()) {
    const std::string root =
        store.ExistingBookmarkRoot(space.profile_id, space.space_id);
    if (root.empty() || !roots.insert(root).second) {
      continue;
    }
    for (const store::Tab* child : children[root]) {
      queue.push_back({child, std::nullopt});
    }
  }
  for (size_t head = 0; head < queue.size(); ++head) {
    const Level level = queue[head];
    snapshot.rows.push_back(BookmarkFrom(*level.tab, level.parent));
    snapshot.is_folder[level.tab->guid] =
        level.tab->type == store::RowType::kBookmarkFolder;
    if (level.tab->type == store::RowType::kBookmarkFolder) {
      for (const store::Tab* child : children[level.tab->guid]) {
        queue.push_back({child, level.tab->guid});
      }
    }
  }
  std::sort(snapshot.rows.begin(), snapshot.rows.end(),
            [](const sync::LocalBookmark& a, const sync::LocalBookmark& b) {
              return std::make_tuple(a.space_id, a.parent_guid.value_or(""),
                                     a.index, a.guid) <
                     std::make_tuple(b.space_id, b.parent_guid.value_or(""),
                                     b.index, b.guid);
            });
  snapshot.loaded = true;
  return snapshot;
}

bool PhiSyncBookmarkAccess::Rebuild() {
  cache_ = Snapshot();
  auto result = std::make_shared<Snapshot>();
  std::function<store::LocalStore*()> store = host_.store;
  if (!run_on_ui_([result, store] {
        if (store::LocalStore* s = store()) {
          *result = Read(*s);
        }
      })) {
    return false;
  }
  cache_ = *result;
  return cache_.loaded;
}

bool PhiSyncBookmarkAccess::AllBookmarks(std::vector<sync::LocalBookmark>* out) {
  if (!Rebuild()) {
    return false;
  }
  *out = cache_.rows;
  return true;
}

bool PhiSyncBookmarkAccess::AllSyncIds(std::set<std::string>* out) {
  if (!cache_.loaded) {
    return false;
  }
  *out = cache_.sync_ids;
  return true;
}

bool PhiSyncBookmarkAccess::CachedBookmarks(std::vector<sync::LocalBookmark>* out) {
  if (!cache_.loaded) {
    return false;
  }
  *out = cache_.rows;
  return true;
}

std::vector<sync::LocalBookmark> PhiSyncBookmarkAccess::Siblings(
    const std::optional<std::string>& parent_guid,
    const std::string& space_id) {
  std::vector<sync::LocalBookmark> out;
  for (const sync::LocalBookmark& row : cache_.rows) {
    if (row.space_id == space_id && row.parent_guid == parent_guid) {
      out.push_back(row);
    }
  }
  return out;
}

bool PhiSyncBookmarkAccess::IsKnownLocalBookmark(const std::string& guid) {
  return cache_.is_folder.count(guid) > 0;
}

std::optional<bool> PhiSyncBookmarkAccess::LocalIsFolder(const std::string& guid) {
  const auto it = cache_.is_folder.find(guid);
  return it == cache_.is_folder.end() ? std::nullopt
                                      : std::optional<bool>(it->second);
}

sync::BookmarkApplyResult PhiSyncBookmarkAccess::Apply(
    const std::vector<sync::BookmarkApplyOp>& ops) {
  using Kind = sync::BookmarkApplyOp::Kind;
  using StoreKind = store::LocalStore::BookmarkSyncOp::Kind;
  std::vector<store::LocalStore::BookmarkSyncOp> writes;
  for (const sync::BookmarkApplyOp& op : ops) {
    store::LocalStore::BookmarkSyncOp write;
    write.guid = op.guid;
    switch (op.kind) {
      case Kind::kClaim:
        write.kind = StoreKind::kClaim;
        write.sync_id = op.sync_id;
        break;
      case Kind::kCreate:
        write.kind = StoreKind::kCreate;
        write.guid = op.row.guid;
        write.sync_id = op.row.sync_id;
        write.parent_guid = op.row.parent_guid;
        write.space_id = op.row.space_id;
        write.index = op.row.index;
        write.profile_id = op.row.profile_id;
        write.is_folder = op.row.is_folder;
        write.title = op.row.title;
        write.url = op.row.url;
        write.secondary_url = op.row.secondary_url;
        write.secondary_title = op.row.secondary_title;
        write.source = op.row.source;
        write.created = op.row.created_ms;
        write.content_updated = op.row.content_updated_ms;
        break;
      case Kind::kMove:
        write.kind = StoreKind::kMove;
        write.parent_guid = op.parent_guid;
        write.space_id = op.space_id;
        write.index = op.index;
        write.records_location_edit = op.records_location_edit;
        break;
      case Kind::kUpdate:
        write.kind = StoreKind::kUpdate;
        write.new_title = op.fields.title;
        write.new_url = op.fields.url;
        write.new_secondary_url = op.fields.secondary_url;
        write.new_secondary_title = op.fields.secondary_title;
        break;
      case Kind::kDelete:
        write.kind = StoreKind::kDelete;
        break;
    }
    writes.push_back(std::move(write));
  }
  struct Result {
    bool ran = false;
    store::WriteError error = store::WriteError::kUnavailable;
    Snapshot snapshot;
  };
  auto result = std::make_shared<Result>();
  PhiOwnedSyncHost host = host_;
  if (!run_on_ui_([result, host, writes] {
        store::LocalStore* s = host.store();
        if (!s) {
          return;
        }
        result->ran = true;
        host.applying(true);
        result->error = s->ApplyBookmarkSyncBatch(writes);
        host.applying(false);
        if (result->error == store::WriteError::kNone) {
          host.landed();
        }
        result->snapshot = Read(*s);
      }) ||
      !result->ran) {
    cache_ = Snapshot();
    return sync::BookmarkApplyResult::kFailed;
  }
  cache_ = result->snapshot;
  switch (result->error) {
    case store::WriteError::kNone:
      return sync::BookmarkApplyResult::kOk;
    case store::WriteError::kFolderNotEmpty:
      return sync::BookmarkApplyResult::kFolderNotEmpty;
    case store::WriteError::kRowAlreadyMapped:
      return sync::BookmarkApplyResult::kRowAlreadyMapped;
    default:
      return sync::BookmarkApplyResult::kFailed;
  }
}

// --- Pins -------------------------------------------------------------------------------

PhiSyncPinAccess::PhiSyncPinAccess(PhiRunOnUi run_on_ui, PhiOwnedSyncHost host)
    : run_on_ui_(std::move(run_on_ui)), host_(std::move(host)) {}

PhiSyncPinAccess::~PhiSyncPinAccess() = default;

PhiSyncPinAccess::Snapshot PhiSyncPinAccess::Read(const store::LocalStore& store) {
  Snapshot snapshot;
  const store::PinScope scope = store.pin_scope();
  snapshot.scope = store::PinScopeName(scope);
  snapshot.generation = store.pin_scope_generation();
  std::map<std::string, std::string> lineage_by_guid;
  for (const auto& [guid, tab] : store.tabs()) {
    if (tab.type == store::RowType::kPinnedTab && !tab.pin_dormant) {
      lineage_by_guid[guid] = tab.pin_lineage_id.value_or(guid);
    }
  }
  for (const auto& [guid, tab] : store.tabs()) {
    if (tab.type != store::RowType::kPinnedTab || tab.pin_dormant) {
      continue;
    }
    sync::LocalPin row = PinFrom(tab, lineage_by_guid);
    if (PinBelongs(tab, scope)) {
      snapshot.active.push_back(row);
      snapshot.identity_pairs.insert(sync::PinKind::LineageKey(row.lineage_id) +
                                     ":" + sync::PinKind::LocalOwnerKey(row));
    }
    snapshot.all.push_back(std::move(row));
  }
  std::sort(snapshot.active.begin(), snapshot.active.end(),
            [](const sync::LocalPin& a, const sync::LocalPin& b) {
              const std::string left = a.profile_id.value_or("") + '\0' +
                                       a.space_id.value_or("");
              const std::string right = b.profile_id.value_or("") + '\0' +
                                        b.space_id.value_or("");
              return std::tie(left, a.index, a.guid) <
                     std::tie(right, b.index, b.guid);
            });
  snapshot.loaded = true;
  return snapshot;
}

bool PhiSyncPinAccess::Rebuild() {
  cache_ = Snapshot();
  auto result = std::make_shared<Snapshot>();
  std::function<store::LocalStore*()> store = host_.store;
  if (!run_on_ui_([result, store] {
        if (store::LocalStore* s = store()) {
          *result = Read(*s);
        }
      })) {
    return false;
  }
  cache_ = *result;
  return cache_.loaded;
}

std::string PhiSyncPinAccess::CurrentScope() {
  auto scope = std::make_shared<std::string>(cache_.scope);
  std::function<store::LocalStore*()> store = host_.store;
  run_on_ui_([scope, store] {
    if (store::LocalStore* s = store()) {
      *scope = store::PinScopeName(s->pin_scope());
    }
  });
  return *scope;
}

uint64_t PhiSyncPinAccess::ScopeGeneration() {
  auto generation = std::make_shared<uint64_t>(cache_.generation);
  std::function<store::LocalStore*()> store = host_.store;
  run_on_ui_([generation, store] {
    if (store::LocalStore* s = store()) {
      *generation = s->pin_scope_generation();
    }
  });
  return *generation;
}

// The account's scope is the synced PhiPinnedTabScope, which Windows lands
// by migrating the store at once: never a separate value to disagree with.
std::optional<std::string> PhiSyncPinAccess::AccountScope() {
  return std::nullopt;
}

bool PhiSyncPinAccess::AllPins(std::vector<sync::LocalPin>* out) {
  if (!Rebuild()) {
    return false;
  }
  *out = cache_.active;
  return true;
}

bool PhiSyncPinAccess::AllPinRows(std::vector<sync::LocalPin>* out) {
  if (!cache_.loaded) {
    return false;
  }
  *out = cache_.all;
  return true;
}

bool PhiSyncPinAccess::CachedPins(std::vector<sync::LocalPin>* out) {
  if (!cache_.loaded) {
    return false;
  }
  *out = cache_.active;
  return true;
}

bool PhiSyncPinAccess::IsKnownLocalPin(
    const std::string& lineage,
    const std::optional<std::string>& owner_key) {
  return owner_key &&
         cache_.identity_pairs.count(Lower(lineage) + ":" + *owner_key) > 0;
}

sync::PinApplyResult PhiSyncPinAccess::Apply(
    const std::vector<sync::PinApplyOp>& ops) {
  using Kind = sync::PinApplyOp::Kind;
  using StoreKind = store::LocalStore::PinSyncOp::Kind;
  std::vector<store::LocalStore::PinSyncOp> writes;
  for (const sync::PinApplyOp& op : ops) {
    store::LocalStore::PinSyncOp write;
    write.guid = op.guid;
    switch (op.kind) {
      case Kind::kCreate:
        write.kind = StoreKind::kCreate;
        write.guid = op.row.guid;
        write.profile_id = op.row.profile_id;
        write.space_id = op.row.space_id;
        write.lineage_id = op.row.lineage_id;
        write.title = op.row.title;
        write.url = op.row.url;
        write.source = op.row.source;
        write.created = op.row.created_ms;
        write.content_updated = op.row.content_updated_ms;
        write.index = op.row.index;
        break;
      case Kind::kRelineage:
        write.kind = StoreKind::kRelineage;
        write.lineage_id = op.new_lineage_id;
        break;
      case Kind::kMove:
        write.kind = StoreKind::kMove;
        write.index = op.index;
        break;
      case Kind::kUpdate:
        write.kind = StoreKind::kUpdate;
        write.new_title = op.fields.title;
        write.new_url = op.fields.url;
        write.split_partner_lineage = op.fields.split_partner_lineage_id;
        break;
      case Kind::kDelete:
        write.kind = StoreKind::kDelete;
        break;
    }
    writes.push_back(std::move(write));
  }
  struct Result {
    bool ran = false;
    bool scope_changed = false;
    store::WriteError error = store::WriteError::kUnavailable;
    Snapshot snapshot;
  };
  auto result = std::make_shared<Result>();
  PhiOwnedSyncHost host = host_;
  const uint64_t expected_generation = cache_.generation;
  const std::string expected_scope = cache_.scope;
  if (!run_on_ui_([result, host, writes, expected_generation, expected_scope] {
        store::LocalStore* s = host.store();
        if (!s) {
          return;
        }
        result->ran = true;
        // The batch fence (the Mac's applyPinSyncBatch expectedGeneration /
        // expectedScope): the plan was made against the sampled rows; a
        // migration since would land it on another scope's collections.
        // Nothing is written; the engine parks the batch and replans.
        if (s->pin_scope_generation() != expected_generation ||
            store::PinScopeName(s->pin_scope()) != expected_scope) {
          result->scope_changed = true;
          result->snapshot = Read(*s);
          return;
        }
        host.applying(true);
        result->error = s->ApplyPinSyncBatch(writes);
        host.applying(false);
        if (result->error == store::WriteError::kNone) {
          host.landed();
        }
        result->snapshot = Read(*s);
      }) ||
      !result->ran) {
    cache_ = Snapshot();
    return sync::PinApplyResult::kFailed;
  }
  cache_ = result->snapshot;
  if (result->scope_changed) {
    return sync::PinApplyResult::kFailed;  // parked: the next round replans
  }
  switch (result->error) {
    case store::WriteError::kNone:
      return sync::PinApplyResult::kOk;
    case store::WriteError::kRowAlreadyMapped:
    case store::WriteError::kRowNotFound:
    case store::WriteError::kNotInActiveScope:
    case store::WriteError::kNoCandidateSurvived:
      return sync::PinApplyResult::kRefused;
    default:
      return sync::PinApplyResult::kFailed;
  }
}

// --- URL rules -----------------------------------------------------------------------------

PhiSyncUrlRuleAccess::PhiSyncUrlRuleAccess(PhiRunOnUi run_on_ui,
                                           PhiOwnedSyncHost host)
    : run_on_ui_(std::move(run_on_ui)), host_(std::move(host)) {}

PhiSyncUrlRuleAccess::~PhiSyncUrlRuleAccess() = default;

bool PhiSyncUrlRuleAccess::AllRules(std::vector<sync::LocalUrlRule>* out) {
  loaded_ = false;
  auto rows = std::make_shared<std::optional<std::vector<sync::LocalUrlRule>>>();
  PhiOwnedSyncHost host = host_;
  if (!run_on_ui_([rows, host] {
        if (host.flush_rules) {
          host.flush_rules();
        }
        if (store::LocalStore* s = host.store()) {
          *rows = RulesOf(*s);
        }
      }) ||
      !*rows) {
    return false;
  }
  cache_ = **rows;
  loaded_ = true;
  *out = cache_;
  return true;
}

bool PhiSyncUrlRuleAccess::CachedRules(std::vector<sync::LocalUrlRule>* out) {
  if (!loaded_) {
    return false;
  }
  *out = cache_;
  return true;
}

bool PhiSyncUrlRuleAccess::Mutate(
    std::function<bool(std::vector<sync::LocalUrlRule>*)> mutate,
    bool notify) {
  struct Result {
    bool ran = false;
    bool wrote = false;
    std::vector<sync::LocalUrlRule> rows;
  };
  auto result = std::make_shared<Result>();
  PhiOwnedSyncHost host = host_;
  if (!run_on_ui_([result, host, mutate, notify] {
        store::LocalStore* s = host.store();
        if (!s) {
          return;
        }
        result->ran = true;
        if (host.flush_rules) {
          host.flush_rules();
        }
        std::vector<sync::LocalUrlRule> rows = RulesOf(*s);
        if (mutate(&rows)) {
          std::vector<store::UrlRule> table;
          for (const sync::LocalUrlRule& row : rows) {
            table.push_back(RuleTo(row));
          }
          host.applying(true);
          result->wrote = s->ReplaceUrlRulesForSync(table) == store::WriteError::kNone;
          host.applying(false);
          if (result->wrote && notify && host.rules_landed) {
            host.rules_landed();
          }
        }
        result->rows = RulesOf(*s);
      }) ||
      !result->ran) {
    loaded_ = false;
    return false;
  }
  cache_ = result->rows;
  loaded_ = true;
  return true;
}

sync::UrlRuleApplyError PhiSyncUrlRuleAccess::Apply(
    const std::vector<sync::UrlRuleSyncOp>& ops,
    const sync::UrlRuleMergeTail& merge_tail,
    bool* failed,
    sync::UrlRuleBatchOutcome* outcome) {
  auto error = std::make_shared<sync::UrlRuleApplyError>(sync::UrlRuleApplyError::kNone);
  auto batch_outcome = std::make_shared<sync::UrlRuleBatchOutcome>();
  const bool ran = Mutate(
      [ops, merge_tail, error, batch_outcome](std::vector<sync::LocalUrlRule>* rows) {
        const int64_t now = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        *error = sync::ApplyUrlRuleOps(rows, ops, merge_tail,
                                       [] { return store::NewRuleId(); }, now,
                                       batch_outcome.get());
        return *error == sync::UrlRuleApplyError::kNone;
      },
      /*notify=*/true);
  *failed = !ran;
  *outcome = *batch_outcome;
  return *error;
}

void PhiSyncUrlRuleAccess::HardDelete(const std::string& sync_id) {
  Mutate(
      [sync_id](std::vector<sync::LocalUrlRule>* rows) {
        const size_t before = rows->size();
        std::erase_if(*rows, [&](const sync::LocalUrlRule& row) {
          return row.sync_id == sync_id;
        });
        return rows->size() != before;
      },
      /*notify=*/false);
}

int PhiSyncUrlRuleAccess::PurgeSoftDeleted(int64_t cutoff_ms) {
  auto purged = std::make_shared<int>(0);
  Mutate(
      [cutoff_ms, purged](std::vector<sync::LocalUrlRule>* rows) {
        const size_t before = rows->size();
        std::erase_if(*rows, [&](const sync::LocalUrlRule& row) {
          return row.deleted_ms && *row.deleted_ms < cutoff_ms;
        });
        *purged = static_cast<int>(before - rows->size());
        return *purged > 0;
      },
      /*notify=*/false);
  return *purged;
}

void PhiSyncUrlRuleAccess::ClearPendingLocalEdits(
    const std::map<std::string, sync::RuleProjection>& entries,
    bool clears_partner) {
  Mutate(
      [entries, clears_partner](std::vector<sync::LocalUrlRule>* rows) {
        bool changed = false;
        for (const auto& [sync_id, confirmed] : entries) {
          changed = sync::ClearRulePendingLocalEdit(rows, sync_id, confirmed,
                                                    clears_partner) ||
                    changed;
        }
        return changed;
      },
      /*notify=*/false);
}

}  // namespace phi_client

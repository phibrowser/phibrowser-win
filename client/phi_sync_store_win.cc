// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_sync_store_win.h"

#include <windows.h>

#include <array>
#include <utility>
#include <vector>

#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "phibrowser-win/client/phi_file_io.h"
#include "phibrowser-win/client/phi_key_crypto.h"

namespace phi_client {

namespace {

bool EnsureParent(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return true;
  }
  const std::wstring dir = path.substr(0, slash);
  return ::CreateDirectoryW(dir.c_str(), nullptr) ||
         ::GetLastError() == ERROR_ALREADY_EXISTS;
}

bool Write(const std::wstring& path, const base::DictValue& values) {
  std::string json;
  return base::JSONWriter::WriteWithOptions(
             values, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json) &&
         EnsureParent(path) && PhiWriteFileAtomic(path, json);
}

std::string ToBase64(const std::string& bytes) {
  return keys::Base64Encode(keys::Bytes(bytes.begin(), bytes.end()));
}

std::optional<std::string> FromBase64(const std::string& text) {
  const std::optional<keys::Bytes> bytes = keys::Base64Decode(text);
  if (!bytes) {
    return std::nullopt;
  }
  return std::string(bytes->begin(), bytes->end());
}

}  // namespace

// --- marker.json ------------------------------------------------------------------------

PhiSyncMarkerFile::PhiSyncMarkerFile(std::wstring path)
    : path_(std::move(path)) {}

PhiSyncMarkerFile::~PhiSyncMarkerFile() = default;

sync::MarkerFile PhiSyncMarkerFile::Load() {
  std::lock_guard<std::mutex> hold(lock_);
  base::DictValue dict;
  sync::MarkerFile file;
  const PhiJsonFileStatus status = PhiReadJsonFile(path_, &dict);
  // A locked file is not a missing one: its flags (removal intent, the
  // reconfiguration latch) must not be saved away until a Load reads it.
  save_blocked_ = status == PhiJsonFileStatus::kUnreadable;
  if (status != PhiJsonFileStatus::kOk ||
      dict.FindInt("formatVersion").value_or(0) < 1) {
    return file;
  }
  if (const std::string* marker = dict.FindString("marker")) {
    std::optional<std::string> bytes = FromBase64(*marker);
    if (bytes && !bytes->empty()) {
      file.marker = std::move(bytes);
    }
  }
  if (const std::string* birthday = dict.FindString("storeBirthday")) {
    file.store_birthday = *birthday;
  }
  file.removal_pending = dict.FindBool("removalPending").value_or(false);
  file.requires_reconfiguration =
      dict.FindBool("requiresReconfiguration").value_or(false);
  if (const std::string* op = dict.FindString("resetOperationId");
      op && !op->empty()) {
    file.reset_operation_id = *op;
  }
  return file;
}

bool PhiSyncMarkerFile::Save(const sync::MarkerFile& file) {
  std::lock_guard<std::mutex> hold(lock_);
  if (save_blocked_) {
    return false;
  }
  base::DictValue dict;
  dict.Set("formatVersion", 1);
  if (file.marker) {
    dict.Set("marker", ToBase64(*file.marker));
  }
  dict.Set("storeBirthday", file.store_birthday);
  if (file.removal_pending) {
    dict.Set("removalPending", true);
  }
  if (file.requires_reconfiguration) {
    dict.Set("requiresReconfiguration", true);
  }
  if (file.reset_operation_id) {
    dict.Set("resetOperationId", *file.reset_operation_id);
  }
  return Write(path_, dict);
}

bool PhiSyncMarkerFile::Remove() {
  std::lock_guard<std::mutex> hold(lock_);
  if (!PhiRemoveFileIfPresent(path_)) {
    return false;
  }
  save_blocked_ = false;
  return true;
}

// --- engine state ---------------------------------------------------------------------

PhiSyncStateFile::PhiSyncStateFile(std::wstring path) : path_(std::move(path)) {
  // Locked or denied: empty in memory, never saved over this session.
  save_blocked_ =
      PhiReadJsonFile(path_, &values_) == PhiJsonFileStatus::kUnreadable;
}

PhiSyncStateFile::~PhiSyncStateFile() = default;

bool PhiSyncStateFile::ClearSyncState() {
  std::lock_guard<std::mutex> hold(lock_);
  if (save_blocked_) {
    return false;
  }
  base::DictValue cleared = values_.Clone();
  for (const std::string& key : sync::EngineStateKeys()) {
    cleared.Remove(key);
  }
  if (!Write(path_, cleared)) {
    return false;
  }
  values_ = std::move(cleared);
  return true;
}

std::optional<std::string> PhiSyncStateFile::Get(const std::string& key) const {
  std::lock_guard<std::mutex> hold(lock_);
  const std::string* value = values_.FindString(key);
  return value ? std::optional<std::string>(*value) : std::nullopt;
}

bool PhiSyncStateFile::Set(const std::string& key,
                           const std::optional<std::string>& value) {
  std::lock_guard<std::mutex> hold(lock_);
  const std::string* current = values_.FindString(key);
  if ((!value && !current) || (value && current && *current == *value)) {
    return true;
  }
  if (save_blocked_) {
    return false;
  }
  // Memory follows the disk: a write that did not land changes nothing.
  base::DictValue updated = values_.Clone();
  if (value) {
    updated.Set(key, *value);
  } else {
    updated.Remove(key);
  }
  if (!Write(path_, updated)) {
    return false;
  }
  values_ = std::move(updated);
  return true;
}

// --- spaces.json ------------------------------------------------------------------------

namespace {

// int64 values are strings: base::Value's integers are 32-bit.
void SetInt64(base::DictValue& dict, std::string_view key, int64_t value) {
  dict.Set(key, base::NumberToString(value));
}

std::optional<int64_t> FindInt64(const base::DictValue& dict,
                                 std::string_view key) {
  const std::string* text = dict.FindString(key);
  int64_t value = 0;
  if (!text || !base::StringToInt64(*text, &value)) {
    return std::nullopt;
  }
  return value;
}

void SetBytes(base::DictValue& dict,
              std::string_view key,
              const std::optional<std::string>& bytes) {
  if (bytes) {
    dict.Set(key, ToBase64(*bytes));
  }
}

std::optional<std::string> FindBytes(const base::DictValue& dict,
                                     std::string_view key) {
  const std::string* text = dict.FindString(key);
  return text ? FromBase64(*text) : std::nullopt;
}

std::optional<std::string> FindText(const base::DictValue& dict,
                                    std::string_view key) {
  const std::string* text = dict.FindString(key);
  return text ? std::optional<std::string>(*text) : std::nullopt;
}

base::DictValue CursorToDict(const sync::SpaceCursor& cursor) {
  base::DictValue dict;
  if (cursor.entity_id) {
    dict.Set("entityId", *cursor.entity_id);
  }
  SetInt64(dict, "version", cursor.version);
  SetBytes(dict, "reconciled", cursor.reconciled);
  SetBytes(dict, "server", cursor.server);
  SetBytes(dict, "pendingApply", cursor.pending_apply);
  SetBytes(dict, "pendingProjection", cursor.pending_projection);
  if (cursor.held_profile_uuid) {
    dict.Set("heldProfileUuid", *cursor.held_profile_uuid);
  }
  if (cursor.held_for_local_profile_id) {
    dict.Set("heldForLocalProfileId", *cursor.held_for_local_profile_id);
  }
  dict.Set("pendingDelete", cursor.pending_delete);
  dict.Set("deleteRejectRounds", cursor.delete_reject_rounds);
  dict.Set("pendingTombstone", cursor.pending_tombstone);
  if (cursor.deleted_at_ms) {
    SetInt64(dict, "deletedAtMs", *cursor.deleted_at_ms);
  }
  dict.Set("hidden", cursor.hidden);
  if (cursor.refused_at_ms) {
    SetInt64(dict, "refusedAtMs", *cursor.refused_at_ms);
  }
  if (cursor.purged_at_ms) {
    SetInt64(dict, "purgedAtMs", *cursor.purged_at_ms);
  }
  return dict;
}

sync::SpaceCursor CursorFromDict(const base::DictValue& dict) {
  sync::SpaceCursor cursor;
  cursor.entity_id = FindText(dict, "entityId");
  cursor.version = FindInt64(dict, "version").value_or(0);
  cursor.reconciled = FindBytes(dict, "reconciled");
  cursor.server = FindBytes(dict, "server");
  cursor.pending_apply = FindBytes(dict, "pendingApply");
  cursor.pending_projection = FindBytes(dict, "pendingProjection");
  cursor.held_profile_uuid = FindText(dict, "heldProfileUuid");
  cursor.held_for_local_profile_id = FindText(dict, "heldForLocalProfileId");
  cursor.pending_delete = dict.FindBool("pendingDelete").value_or(false);
  cursor.delete_reject_rounds = dict.FindInt("deleteRejectRounds").value_or(0);
  cursor.pending_tombstone = dict.FindBool("pendingTombstone").value_or(false);
  cursor.deleted_at_ms = FindInt64(dict, "deletedAtMs");
  cursor.hidden = dict.FindBool("hidden").value_or(false);
  cursor.refused_at_ms = FindInt64(dict, "refusedAtMs");
  cursor.purged_at_ms = FindInt64(dict, "purgedAtMs");
  return cursor;
}

// The table's flags, by their Mac names.
struct TableFlag {
  const char* name;
  bool sync::SpaceSyncTable::*field;
};
constexpr std::array<TableFlag, 12> kTableFlags = {{
    {"drainInProgress", &sync::SpaceSyncTable::drain_in_progress},
    {"hasDrainedFullReplay", &sync::SpaceSyncTable::has_drained_full_replay},
    {"hadRecords", &sync::SpaceSyncTable::had_records},
    {"spaceSectionEnabled", &sync::SpaceSyncTable::space_section_enabled},
    {"markerMovedWhileGateShut",
     &sync::SpaceSyncTable::marker_moved_while_gate_shut},
    {"didReplayForEmptyTable",
     &sync::SpaceSyncTable::did_replay_for_empty_table},
    {"bookmarksHadRecords", &sync::SpaceSyncTable::bookmarks_had_records},
    {"pinsHadRecords", &sync::SpaceSyncTable::pins_had_records},
    {"urlRulesHadRecords", &sync::SpaceSyncTable::url_rules_had_records},
    {"bookmarksReplayedForEmptyTable",
     &sync::SpaceSyncTable::bookmarks_replayed_for_empty_table},
    {"pinsReplayedForEmptyTable",
     &sync::SpaceSyncTable::pins_replayed_for_empty_table},
    {"urlRulesReplayedForEmptyTable",
     &sync::SpaceSyncTable::url_rules_replayed_for_empty_table},
}};

}  // namespace

PhiSpaceTableFile::PhiSpaceTableFile(std::wstring path)
    : path_(std::move(path)) {}

PhiSpaceTableFile::~PhiSpaceTableFile() = default;

sync::SpaceSyncTable PhiSpaceTableFile::Load() {
  std::lock_guard<std::mutex> hold(lock_);
  base::DictValue dict;
  sync::SpaceSyncTable table;
  const PhiJsonFileStatus status = PhiReadJsonFile(path_, &dict);
  // A locked table is not an empty one: it is never saved over until a
  // Load reads it again.
  save_blocked_ = status == PhiJsonFileStatus::kUnreadable;
  if (status != PhiJsonFileStatus::kOk) {
    return table;
  }
  // A newer build's table is not read (nor written over) by guesswork: it
  // reads as empty, and every Save is refused until a newer build runs.
  const int format = dict.FindInt("formatVersion").value_or(0);
  if (format > sync::SpaceSyncTable::kFormatVersion) {
    save_blocked_ = true;
  }
  if (format != sync::SpaceSyncTable::kFormatVersion) {
    return table;
  }
  if (const base::DictValue* cursors = dict.FindDict("cursors")) {
    for (const auto [uuid, value] : *cursors) {
      if (value.is_dict()) {
        table.cursors[uuid] = CursorFromDict(value.GetDict());
      }
    }
  }
  for (const TableFlag& flag : kTableFlags) {
    table.*(flag.field) = dict.FindBool(flag.name).value_or(false);
  }
  // An absent key is a table an older build wrote: it may have refused
  // entities this build accepts, so it reads as version 0 and replays once.
  table.inbound_acceptance_version =
      dict.FindInt("inboundAcceptanceVersion").value_or(0);
  table.last_drained_birthday = FindText(dict, "lastDrainedBirthday");
  if (const base::ListValue* kinds = dict.FindList("replayedKinds")) {
    for (const base::Value& kind : *kinds) {
      if (kind.is_string()) {
        table.replayed_kinds.insert(kind.GetString());
      }
    }
  }
  if (const base::DictValue* hashes = dict.FindDict("unreadableTagHashes")) {
    for (const auto [hash, value] : *hashes) {
      int64_t seen = 0;
      if (value.is_string() && base::StringToInt64(value.GetString(), &seen)) {
        table.unreadable_tag_hashes[hash] = seen;
      }
    }
  }
  return table;
}

bool PhiSpaceTableFile::Save(const sync::SpaceSyncTable& table) {
  std::function<void(const sync::SpaceSyncTable&)> on_saved;
  {
    std::lock_guard<std::mutex> hold(lock_);
    if (save_blocked_) {
      return false;
    }
    base::DictValue dict;
    dict.Set("formatVersion", sync::SpaceSyncTable::kFormatVersion);
    base::DictValue cursors;
    for (const auto& [uuid, cursor] : table.cursors) {
      cursors.Set(uuid, CursorToDict(cursor));
    }
    dict.Set("cursors", std::move(cursors));
    for (const TableFlag& flag : kTableFlags) {
      dict.Set(flag.name, table.*(flag.field));
    }
    dict.Set("inboundAcceptanceVersion", table.inbound_acceptance_version);
    if (table.last_drained_birthday) {
      dict.Set("lastDrainedBirthday", *table.last_drained_birthday);
    }
    base::ListValue kinds;
    for (const std::string& kind : table.replayed_kinds) {
      kinds.Append(kind);
    }
    dict.Set("replayedKinds", std::move(kinds));
    base::DictValue hashes;
    for (const auto& [hash, seen] : table.unreadable_tag_hashes) {
      hashes.Set(hash, base::NumberToString(seen));
    }
    dict.Set("unreadableTagHashes", std::move(hashes));
    if (!Write(path_, dict)) {
      return false;
    }
    on_saved = on_saved_;
  }
  if (on_saved) {
    on_saved(table);
  }
  return true;
}

bool PhiSpaceTableFile::Remove() {
  std::lock_guard<std::mutex> hold(lock_);
  if (!PhiRemoveFileIfPresent(path_)) {
    return false;
  }
  save_blocked_ = false;
  return true;
}

// --- sidecars ---------------------------------------------------------------------------

PhiSyncSidecarFile::PhiSyncSidecarFile(std::wstring path)
    : path_(std::move(path)) {
  // Locked or denied: empty in memory, never saved over this session.
  save_blocked_ =
      PhiReadJsonFile(path_, &values_) == PhiJsonFileStatus::kUnreadable;
}

PhiSyncSidecarFile::~PhiSyncSidecarFile() = default;

std::optional<int64_t> PhiSyncSidecarFile::Timestamp(
    const std::string& key) const {
  std::lock_guard<std::mutex> hold(lock_);
  const std::string* text = values_.FindString(key + ".phiSyncTs");
  int64_t stamp = 0;
  if (!text || !base::StringToInt64(*text, &stamp)) {
    return std::nullopt;
  }
  return stamp;
}

std::optional<std::string> PhiSyncSidecarFile::Value(
    const std::string& key) const {
  std::lock_guard<std::mutex> hold(lock_);
  const std::string* text = values_.FindString(key + ".phiSyncVal");
  return text ? FromBase64(*text) : std::nullopt;
}

bool PhiSyncSidecarFile::Set(const std::string& key,
                             int64_t stamp,
                             const std::string& value_signature) {
  std::lock_guard<std::mutex> hold(lock_);
  if (save_blocked_) {
    return false;
  }
  // Memory follows the disk: a stamp that did not persist is not kept, or
  // it would be lost at the next launch and re-stamped as a new edit.
  base::DictValue updated = values_.Clone();
  updated.Set(key + ".phiSyncTs", base::NumberToString(stamp));
  updated.Set(key + ".phiSyncVal", ToBase64(value_signature));
  if (!Write(path_, updated)) {
    return false;
  }
  values_ = std::move(updated);
  return true;
}

bool PhiSyncSidecarFile::ClearSidecars() {
  std::lock_guard<std::mutex> hold(lock_);
  if (save_blocked_) {
    return false;
  }
  std::vector<std::string> keys;
  for (const auto [key, value] : values_) {
    const auto ends_with = [&key](std::string_view suffix) {
      return key.size() >= suffix.size() &&
             std::string_view(key).substr(key.size() - suffix.size()) == suffix;
    };
    if (ends_with(".phiSyncTs") || ends_with(".phiSyncVal")) {
      keys.push_back(key);
    }
  }
  base::DictValue cleared = values_.Clone();
  cleared.Remove("PhiDefaultSpaceUuid.mirror");
  for (const std::string& key : keys) {
    cleared.Remove(key);
  }
  if (!Write(path_, cleared)) {
    return false;
  }
  values_ = std::move(cleared);
  return true;
}

std::optional<std::string> PhiSyncSidecarFile::Extra(
    const std::string& key) const {
  std::lock_guard<std::mutex> hold(lock_);
  const std::string* text = values_.FindString(key);
  return text ? std::optional<std::string>(*text) : std::nullopt;
}

bool PhiSyncSidecarFile::SetExtra(const std::string& key,
                                  const std::optional<std::string>& value) {
  std::lock_guard<std::mutex> hold(lock_);
  const std::string* current = values_.FindString(key);
  if ((!value && !current) || (value && current && *current == *value)) {
    return true;
  }
  if (save_blocked_) {
    return false;
  }
  base::DictValue updated = values_.Clone();
  if (value) {
    updated.Set(key, *value);
  } else {
    updated.Remove(key);
  }
  if (!Write(path_, updated)) {
    return false;
  }
  values_ = std::move(updated);
  return true;
}

}  // namespace phi_client

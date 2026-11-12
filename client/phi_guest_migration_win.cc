// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_guest_migration_win.h"

#include <windows.h>

#include <cctype>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>

#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "client/phi_account_storage.h"
#include "client/phi_file_io.h"
#include "client/phi_local_store.h"
#include "client/phi_util.h"

namespace phi_client {

namespace {

using store::GuestMappings;
using store::GuestPinTarget;
using store::GuestPresence;
using store::GuestRuleTarget;
using store::GuestSkippedRule;
using store::GuestSnapshot;
using store::GuestThemes;
using Outcome = PhiGuestMigrationResult::Outcome;

enum class Phase { kPrepared, kTargetImported, kSourceDirectoryStaged };

const char* PhaseName(Phase phase) {
  switch (phase) {
    case Phase::kPrepared:
      return "prepared";
    case Phase::kTargetImported:
      return "targetImported";
    case Phase::kSourceDirectoryStaged:
      break;
  }
  return "sourceDirectoryStaged";
}

std::optional<Phase> PhaseFromName(const std::string& name) {
  if (name == "prepared") {
    return Phase::kPrepared;
  }
  if (name == "targetImported") {
    return Phase::kTargetImported;
  }
  if (name == "sourceDirectoryStaged") {
    return Phase::kSourceDirectoryStaged;
  }
  return std::nullopt;
}

struct Journal {
  std::string operation_id;
  std::string source_user_id;
  std::string target_user_id;
  std::string snapshot_id;
  Phase phase = Phase::kPrepared;
  GuestMappings mappings;
};

// --- The mappings codec ------------------------------------------------------------

base::DictValue StringMap(const std::map<std::string, std::string>& map) {
  base::DictValue dict;
  for (const auto& [key, value] : map) {
    dict.Set(key, value);
  }
  return dict;
}

std::map<std::string, std::string> StringMapFrom(const base::DictValue* dict) {
  std::map<std::string, std::string> out;
  if (dict) {
    for (const auto [key, value] : *dict) {
      if (value.is_string()) {
        out[key] = value.GetString();
      }
    }
  }
  return out;
}

base::ListValue StringList(const std::vector<std::string>& values) {
  base::ListValue list;
  for (const std::string& value : values) {
    list.Append(value);
  }
  return list;
}

std::vector<std::string> StringListFrom(const base::ListValue* list) {
  std::vector<std::string> out;
  if (list) {
    for (const base::Value& value : *list) {
      if (value.is_string()) {
        out.push_back(value.GetString());
      }
    }
  }
  return out;
}

void SetOptional(base::DictValue& dict,
                 std::string_view key,
                 const std::optional<std::string>& value) {
  if (value) {
    dict.Set(key, *value);
  }
}

std::optional<std::string> FindOptional(const base::DictValue& dict,
                                        std::string_view key) {
  const std::string* value = dict.FindString(key);
  return value ? std::optional<std::string>(*value) : std::nullopt;
}

std::string FindText(const base::DictValue& dict, std::string_view key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

base::DictValue MappingsToDict(const GuestMappings& mappings) {
  base::DictValue dict;
  dict.Set("profileIDs", StringMap(mappings.profile_ids));
  dict.Set("spaceIDs", StringMap(mappings.space_ids));
  dict.Set("bookmarkGUIDs", StringMap(mappings.bookmark_guids));
  base::ListValue pins;
  for (const GuestPinTarget& pin : mappings.pin_targets) {
    base::DictValue item;
    item.Set("sourceGUID", pin.source_guid);
    item.Set("targetGUID", pin.target_guid);
    SetOptional(item, "targetProfileID", pin.target_profile_id);
    SetOptional(item, "targetSpaceID", pin.target_space_id);
    item.Set("targetIndex", pin.target_index);
    SetOptional(item, "targetSplitPartnerGUID", pin.target_split_partner_guid);
    item.Set("shouldInsert", pin.should_insert);
    item.Set("isDormant", pin.dormant);
    pins.Append(std::move(item));
  }
  dict.Set("pinnedTargets", std::move(pins));
  dict.Set("pinLineageIDs", StringMap(mappings.pin_lineage_ids));
  base::ListValue rules;
  for (const GuestRuleTarget& rule : mappings.rule_targets) {
    base::DictValue item;
    item.Set("sourceID", rule.source_id);
    item.Set("targetID", rule.target_id);
    item.Set("targetSpaceID", rule.target_space_id);
    item.Set("targetSortOrder", rule.target_sort_order);
    rules.Append(std::move(item));
  }
  dict.Set("urlRuleTargets", std::move(rules));
  base::ListValue skipped;
  for (const GuestSkippedRule& rule : mappings.skipped_rules) {
    base::DictValue item;
    item.Set("sourceID", rule.source_id);
    item.Set("targetSpaceID", rule.target_space_id);
    item.Set("host", rule.host);
    SetOptional(item, "pathPrefix", rule.path_prefix);
    skipped.Append(std::move(item));
  }
  dict.Set("skippedURLRules", std::move(skipped));
  SetOptional(dict, "defaultImportFolderGUID",
              mappings.default_import_folder_guid);
  dict.Set("targetPinnedTabScopeRawValue",
           store::PinScopeName(mappings.target_scope));
  dict.Set("targetSpaceProfileIDs",
           StringMap(mappings.target_space_profile_ids));
  dict.Set("insertedProfileIDs", StringList(mappings.inserted_profile_ids));
  dict.Set("insertedSpaceIDs", StringList(mappings.inserted_space_ids));
  return dict;
}

bool MappingsFromDict(const base::DictValue& dict, GuestMappings* out) {
  GuestMappings mappings;
  mappings.profile_ids = StringMapFrom(dict.FindDict("profileIDs"));
  mappings.space_ids = StringMapFrom(dict.FindDict("spaceIDs"));
  mappings.bookmark_guids = StringMapFrom(dict.FindDict("bookmarkGUIDs"));
  if (const base::ListValue* pins = dict.FindList("pinnedTargets")) {
    for (const base::Value& value : *pins) {
      if (!value.is_dict()) {
        return false;
      }
      const base::DictValue& item = value.GetDict();
      GuestPinTarget pin;
      pin.source_guid = FindText(item, "sourceGUID");
      pin.target_guid = FindText(item, "targetGUID");
      pin.target_profile_id = FindOptional(item, "targetProfileID");
      pin.target_space_id = FindOptional(item, "targetSpaceID");
      pin.target_index = item.FindInt("targetIndex").value_or(0);
      pin.target_split_partner_guid =
          FindOptional(item, "targetSplitPartnerGUID");
      pin.should_insert = item.FindBool("shouldInsert").value_or(false);
      pin.dormant = item.FindBool("isDormant").value_or(false);
      if (pin.source_guid.empty() || pin.target_guid.empty()) {
        return false;
      }
      mappings.pin_targets.push_back(std::move(pin));
    }
  }
  mappings.pin_lineage_ids = StringMapFrom(dict.FindDict("pinLineageIDs"));
  if (const base::ListValue* rules = dict.FindList("urlRuleTargets")) {
    for (const base::Value& value : *rules) {
      if (!value.is_dict()) {
        return false;
      }
      const base::DictValue& item = value.GetDict();
      GuestRuleTarget rule;
      rule.source_id = FindText(item, "sourceID");
      rule.target_id = FindText(item, "targetID");
      rule.target_space_id = FindText(item, "targetSpaceID");
      rule.target_sort_order = item.FindInt("targetSortOrder").value_or(0);
      if (rule.source_id.empty() || rule.target_id.empty()) {
        return false;
      }
      mappings.rule_targets.push_back(std::move(rule));
    }
  }
  if (const base::ListValue* skipped = dict.FindList("skippedURLRules")) {
    for (const base::Value& value : *skipped) {
      if (!value.is_dict()) {
        return false;
      }
      const base::DictValue& item = value.GetDict();
      mappings.skipped_rules.push_back(
          {FindText(item, "sourceID"), FindText(item, "targetSpaceID"),
           FindText(item, "host"), FindOptional(item, "pathPrefix")});
    }
  }
  mappings.default_import_folder_guid =
      FindOptional(dict, "defaultImportFolderGUID");
  store::PinScope scope = store::PinScope::kProfile;
  store::PinScopeFromName(FindText(dict, "targetPinnedTabScopeRawValue"),
                          &scope);
  mappings.target_scope = scope;
  mappings.target_space_profile_ids =
      StringMapFrom(dict.FindDict("targetSpaceProfileIDs"));
  mappings.inserted_profile_ids =
      StringListFrom(dict.FindList("insertedProfileIDs"));
  mappings.inserted_space_ids =
      StringListFrom(dict.FindList("insertedSpaceIDs"));
  *out = std::move(mappings);
  return true;
}

// --- Paths and files ----------------------------------------------------------------

std::wstring JournalPath(const std::wstring& user_data_dir) {
  return user_data_dir + L"\\Phi\\guestDataMigration\\pending.json";
}

std::wstring GuestDir(const std::wstring& user_data_dir) {
  return PhiAccountDirPath(user_data_dir, kGuestAccountId);
}

std::wstring TombstoneDir(const std::wstring& user_data_dir,
                          const std::string& operation_id) {
  return user_data_dir + L"\\Phi\\users\\." +
         base::UTF8ToWide(PhiAccountDirName(kGuestAccountId)) +
         L".guest-migration-" + base::UTF8ToWide(operation_id) +
         L".pending-delete";
}

bool DirectoryExists(const std::wstring& path) {
  const DWORD attributes = ::GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDirectoryChain(const std::wstring& dir) {
  std::error_code error;
  std::filesystem::create_directories(std::filesystem::path(dir), error);
  return !error || DirectoryExists(dir);
}

enum class JournalRead { kMissing, kOk, kCorrupt };

JournalRead LoadJournal(const std::wstring& user_data_dir, Journal* out) {
  base::DictValue dict;
  switch (PhiReadJsonFile(JournalPath(user_data_dir), &dict)) {
    case PhiJsonFileStatus::kMissing:
      return JournalRead::kMissing;
    case PhiJsonFileStatus::kCorrupt:
    case PhiJsonFileStatus::kUnreadable:  // locked: never "no journal"
      return JournalRead::kCorrupt;
    case PhiJsonFileStatus::kOk:
      break;
  }
  Journal journal;
  journal.operation_id = FindText(dict, "operationID");
  journal.source_user_id = FindText(dict, "sourceUserID");
  journal.target_user_id = FindText(dict, "targetUserID");
  journal.snapshot_id = FindText(dict, "snapshotID");
  const std::optional<Phase> phase = PhaseFromName(FindText(dict, "phase"));
  const base::DictValue* mappings = dict.FindDict("mappings");
  if (journal.operation_id.empty() || journal.target_user_id.empty() ||
      !phase || !mappings ||
      !MappingsFromDict(*mappings, &journal.mappings)) {
    return JournalRead::kCorrupt;
  }
  journal.phase = *phase;
  *out = std::move(journal);
  return JournalRead::kOk;
}

bool WriteJournal(const std::wstring& user_data_dir, const Journal& journal) {
  base::DictValue dict;
  dict.Set("operationID", journal.operation_id);
  dict.Set("sourceUserID", journal.source_user_id);
  dict.Set("targetUserID", journal.target_user_id);
  dict.Set("snapshotID", journal.snapshot_id);
  dict.Set("phase", PhaseName(journal.phase));
  dict.Set("mappings", MappingsToDict(journal.mappings));
  return EnsureDirectoryChain(user_data_dir + L"\\Phi\\guestDataMigration") &&
         PhiWriteJsonFile(JournalPath(user_data_dir), std::move(dict));
}

void RemoveJournal(const std::wstring& user_data_dir) {
  ::DeleteFileW(JournalPath(user_data_dir).c_str());
}

bool WriteReceipt(const std::wstring& target_dir,
                  const Journal& journal,
                  const GuestThemes& target_themes) {
  const std::wstring dir = target_dir + L"\\guestDataMigrations\\receipts";
  base::DictValue dict;
  dict.Set("operationID", journal.operation_id);
  dict.Set("sourceUserID", journal.source_user_id);
  dict.Set("targetUserID", journal.target_user_id);
  dict.Set("snapshotID", journal.snapshot_id);
  dict.Set("completedAt", static_cast<double>(PhiNowUnixMs()));
  dict.Set("mappings", MappingsToDict(journal.mappings));
  dict.Set("targetThemeIds", StringMap(target_themes.theme_ids));
  return EnsureDirectoryChain(dir) &&
         PhiWriteJsonFile(dir + L"\\" + base::UTF8ToWide(journal.operation_id) +
                              L".json",
                          std::move(dict));
}

// --- Themes ---------------------------------------------------------------------------

GuestThemes ReadThemes(const PhiAccountDefaults& defaults) {
  GuestThemes themes;
  themes.theme_ids = defaults.GetStringMap(kDefaultsSpaceThemeIds);
  themes.saturations = defaults.GetIntMap(kDefaultsWinSpaceSaturations);
  if (const base::DictValue* opacities =
          defaults.GetDict(kDefaultsSpaceOverlayOpacities)) {
    for (const auto [space, value] : *opacities) {
      if (!value.is_dict()) {
        continue;
      }
      for (const auto [mode, number] : value.GetDict()) {
        if (number.is_double() || number.is_int()) {
          themes.overlay_opacities[space][mode] = number.GetDouble();
        }
      }
    }
  }
  return themes;
}

void WriteThemes(PhiAccountDefaults& defaults, const GuestThemes& themes) {
  defaults.SetStringMap(kDefaultsSpaceThemeIds, themes.theme_ids);
  defaults.SetIntMap(kDefaultsWinSpaceSaturations, themes.saturations);
  base::DictValue opacities;
  for (const auto& [space, modes] : themes.overlay_opacities) {
    base::DictValue entry;
    for (const auto& [mode, value] : modes) {
      entry.Set(mode, value);
    }
    opacities.Set(space, std::move(entry));
  }
  defaults.SetDict(kDefaultsSpaceOverlayOpacities, std::move(opacities));
}

// Merges the Guest's Space themes into the target's defaults (target wins);
// idempotent, so a resumed migration runs it again.
GuestThemes MergeThemesInto(const std::wstring& target_dir,
                            const GuestThemes& guest,
                            const GuestMappings& mappings) {
  PhiAccountDefaults target;
  target.Load(target_dir);
  const GuestThemes merged =
      store::MergeGuestThemes(ReadThemes(target), guest, mappings.space_ids);
  WriteThemes(target, merged);
  std::set<std::string> mapped;
  for (const auto& [source, space] : mappings.space_ids) {
    mapped.insert(space);
  }
  return merged.Retaining(mapped);
}

std::string LowerUuid() {
  std::string id = store::NewUppercaseUuid();
  for (char& c : id) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return id;
}

bool OpenStore(store::LocalStore& db, const std::wstring& account_dir) {
  return db.Open(base::WideToUTF8(account_dir + L"\\localDB")) ==
         store::LocalStore::OpenResult::kOk;
}

bool GuestStoreExists(const std::wstring& user_data_dir) {
  const std::wstring file = GuestDir(user_data_dir) + L"\\localDB\\" +
                            base::UTF8ToWide(store::kStoreFilename);
  return ::GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES;
}

PhiGuestMigrationResult Fail(Outcome outcome, std::string error) {
  PhiGuestMigrationResult result;
  result.outcome = outcome;
  result.error = std::move(error);
  LOG(ERROR) << "[PhiClient] guest migration: " << result.error;
  return result;
}

// Re-reads the (intact) Guest store and verifies a landed import against it:
// a resumed "prepared" journal has no snapshot of its own.
bool VerifyAgainstGuest(const std::wstring& user_data_dir,
                        const store::LocalStore& target,
                        const Journal& journal,
                        GuestThemes* guest_themes,
                        std::string* error) {
  store::LocalStore guest;
  if (!GuestStoreExists(user_data_dir) ||
      !OpenStore(guest, GuestDir(user_data_dir))) {
    *error = "the Guest store is gone";
    return false;
  }
  PhiAccountDefaults defaults;
  defaults.Load(GuestDir(user_data_dir));
  *guest_themes = ReadThemes(defaults);
  GuestSnapshot snapshot;
  return store::MakeGuestSnapshot(guest, kGuestAccountId, *guest_themes,
                                  journal.snapshot_id, PhiNowUnixMs(),
                                  &snapshot, error) &&
         store::VerifyGuestImport(target, snapshot, journal.mappings, error);
}

PhiGuestMigrationResult Migrate(const std::wstring& user_data_dir,
                                const std::string& target_account,
                                const std::string& folder_title,
                                bool resume_only) {
  if (target_account.empty() || target_account == kGuestAccountId) {
    return Fail(Outcome::kNeedsRecovery, "the target cannot be Guest");
  }
  Journal journal;
  const JournalRead read = LoadJournal(user_data_dir, &journal);
  if (read == JournalRead::kCorrupt) {
    return Fail(Outcome::kNeedsRecovery, "the journal is unreadable");
  }
  if (read == JournalRead::kOk && journal.target_user_id != target_account) {
    return Fail(Outcome::kNeedsRecovery,
                "a pending migration targets another account");
  }
  if (read == JournalRead::kMissing && resume_only) {
    return PhiGuestMigrationResult();
  }
  if (read == JournalRead::kMissing && !GuestStoreExists(user_data_dir)) {
    return PhiGuestMigrationResult();  // Nothing to move.
  }
  const std::wstring target_dir =
      PhiEnsureAccountDir(user_data_dir, target_account);
  store::LocalStore target;
  if (target_dir.empty() || !OpenStore(target, target_dir)) {
    return Fail(read == JournalRead::kOk ? Outcome::kNeedsRecovery
                                         : Outcome::kFailedGuestIntact,
                "the target store could not be opened");
  }
  PhiGuestMigrationResult imported;
  imported.outcome = Outcome::kImported;

  if (read == JournalRead::kOk) {
    if (journal.phase != Phase::kPrepared) {
      imported.mappings = journal.mappings;
      return imported;
    }
    std::string detail;
    switch (store::GuestImportPresence(target, journal.mappings, &detail)) {
      case GuestPresence::kPartial:
        return Fail(Outcome::kNeedsRecovery, detail);
      case GuestPresence::kComplete: {
        GuestThemes guest_themes;
        if (!VerifyAgainstGuest(user_data_dir, target, journal, &guest_themes,
                                &detail)) {
          return Fail(Outcome::kNeedsRecovery, detail);
        }
        const GuestThemes target_themes =
            MergeThemesInto(target_dir, guest_themes, journal.mappings);
        WriteReceipt(target_dir, journal, target_themes);
        journal.phase = Phase::kTargetImported;
        if (!WriteJournal(user_data_dir, journal)) {
          return Fail(Outcome::kNeedsRecovery, "the journal write failed");
        }
        imported.mappings = journal.mappings;
        return imported;
      }
      case GuestPresence::kAbsent:
        // The import never landed: the Guest directory is intact.
        RemoveJournal(user_data_dir);
        if (resume_only) {
          PhiGuestMigrationResult dropped;
          dropped.outcome = Outcome::kFailedGuestIntact;
          return dropped;
        }
        break;
    }
    if (!GuestStoreExists(user_data_dir)) {
      return PhiGuestMigrationResult();
    }
  }

  // A fresh migration.
  const std::wstring guest_dir = GuestDir(user_data_dir);
  store::LocalStore guest;
  if (!OpenStore(guest, guest_dir)) {
    return Fail(Outcome::kFailedGuestIntact,
                "the Guest store could not be opened");
  }
  PhiAccountDefaults guest_defaults;
  guest_defaults.Load(guest_dir);
  const GuestThemes guest_themes = ReadThemes(guest_defaults);
  Journal fresh;
  fresh.operation_id = LowerUuid();
  fresh.source_user_id = kGuestAccountId;
  fresh.target_user_id = target_account;
  fresh.snapshot_id = LowerUuid();
  GuestSnapshot snapshot;
  std::string error;
  if (!store::MakeGuestSnapshot(guest, kGuestAccountId, guest_themes,
                                fresh.snapshot_id, PhiNowUnixMs(), &snapshot,
                                &error) ||
      !store::PlanGuestImport(snapshot, target, fresh.operation_id,
                              &fresh.mappings, &error)) {
    return Fail(Outcome::kFailedGuestIntact, error);
  }
  guest.Close();
  if (!WriteJournal(user_data_dir, fresh)) {
    return Fail(Outcome::kFailedGuestIntact, "the journal write failed");
  }
  if (!target.ImportGuestData(snapshot, fresh.mappings, folder_title,
                              &error)) {
    if (store::GuestImportPresence(target, fresh.mappings, nullptr) ==
        GuestPresence::kAbsent) {
      RemoveJournal(user_data_dir);
      return Fail(Outcome::kFailedGuestIntact, error);
    }
    return Fail(Outcome::kNeedsRecovery, error);
  }
  if (!store::VerifyGuestImport(target, snapshot, fresh.mappings, &error)) {
    return Fail(Outcome::kNeedsRecovery, error);
  }
  const GuestThemes target_themes =
      MergeThemesInto(target_dir, snapshot.themes, fresh.mappings);
  if (!WriteReceipt(target_dir, fresh, target_themes)) {
    LOG(ERROR) << "[PhiClient] guest migration: the receipt write failed";
  }
  fresh.phase = Phase::kTargetImported;
  if (!WriteJournal(user_data_dir, fresh)) {
    return Fail(Outcome::kNeedsRecovery, "the journal write failed");
  }
  LOG(INFO) << "[PhiClient] guest migration: imported "
            << fresh.mappings.inserted_space_ids.size() << " spaces, "
            << fresh.mappings.bookmark_guids.size() << " bookmarks, "
            << fresh.mappings.pin_targets.size() << " pins, "
            << fresh.mappings.rule_targets.size() << " rules ("
            << fresh.mappings.skipped_rules.size() << " already present)";
  imported.mappings = fresh.mappings;
  return imported;
}

}  // namespace

PhiGuestMigrationResult PhiMigrateGuestData(const std::wstring& user_data_dir,
                                            const std::string& target_account,
                                            const std::string& folder_title) {
  return Migrate(user_data_dir, target_account, folder_title,
                 /*resume_only=*/false);
}

bool PhiFinalizeGuestCleanup(const std::wstring& user_data_dir,
                             const std::string& target_account,
                             std::string* error) {
  Journal journal;
  if (LoadJournal(user_data_dir, &journal) != JournalRead::kOk ||
      journal.target_user_id != target_account ||
      journal.phase == Phase::kPrepared) {
    *error = "no finished import to clean up after";
    return false;
  }
  const std::wstring source = GuestDir(user_data_dir);
  const std::wstring tombstone =
      TombstoneDir(user_data_dir, journal.operation_id);
  const bool source_exists = DirectoryExists(source);
  if (journal.phase == Phase::kSourceDirectoryStaged && source_exists) {
    // A Guest directory made after staging holds NEW Guest data: only the
    // staged one goes (recoverStagedCleanupIfNeeded).
    std::error_code ignored;
    std::filesystem::remove_all(std::filesystem::path(tombstone), ignored);
    RemoveJournal(user_data_dir);
    return true;
  }
  if (source_exists) {
    if (DirectoryExists(tombstone)) {
      std::error_code ignored;
      std::filesystem::remove_all(std::filesystem::path(tombstone), ignored);
    }
    if (!::MoveFileExW(source.c_str(), tombstone.c_str(), 0)) {
      *error = "the Guest directory could not be staged (" +
               std::to_string(::GetLastError()) + ")";
      return false;
    }
  }
  if (journal.phase != Phase::kSourceDirectoryStaged) {
    journal.phase = Phase::kSourceDirectoryStaged;
    if (!WriteJournal(user_data_dir, journal)) {
      *error = "the journal write failed";
      return false;
    }
  }
  std::error_code removed;
  std::filesystem::remove_all(std::filesystem::path(tombstone), removed);
  if (removed || DirectoryExists(tombstone)) {
    *error = "the staged Guest directory could not be deleted";
    return false;
  }
  RemoveJournal(user_data_dir);
  LOG(INFO) << "[PhiClient] guest migration: Guest directory removed";
  return true;
}

void PhiRecoverGuestMigration(const std::wstring& user_data_dir,
                              const std::string& opening_account,
                              const std::string& folder_title) {
  Journal journal;
  if (LoadJournal(user_data_dir, &journal) != JournalRead::kOk) {
    return;
  }
  LOG(INFO) << "[PhiClient] guest migration: resuming a "
            << PhaseName(journal.phase) << " journal";
  const bool same_target = journal.target_user_id == opening_account;
  const PhiGuestMigrationResult result =
      Migrate(user_data_dir, journal.target_user_id, folder_title,
              /*resume_only=*/!same_target);
  if (result.outcome != Outcome::kImported) {
    return;
  }
  std::string error;
  if (!PhiFinalizeGuestCleanup(user_data_dir, journal.target_user_id,
                               &error)) {
    LOG(ERROR) << "[PhiClient] guest migration cleanup: " << error;
  }
}

}  // namespace phi_client

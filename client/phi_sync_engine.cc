// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_sync_engine.h"

#include <algorithm>
#include <initializer_list>
#include <utility>

#include "phibrowser-win/client/phi_sync_merge.h"

namespace phi_client::sync {

namespace {

// The Mac's state keys (PhiSyncEngine.statePrefix + …).
constexpr char kEntityIdKey[] = "phi.sync.entityId";
constexpr char kVersionKey[] = "phi.sync.version";
constexpr char kLastEntityKey[] = "phi.sync.lastEntity";
constexpr char kTombstoneRoundsKey[] = "phi.sync.tombstoneRounds";
constexpr char kHasAdoptedKey[] = "phi.sync.hasAdopted";
constexpr char kUnreadableKey[] = "phi.sync.unreadableSettings";
constexpr char kHlcMaxKey[] = "phi.sync.hlcMax";
constexpr char kWallClockOffsetKey[] = "phi.sync.wallClockOffsetMs";

constexpr int kMaxPullPages = 64;
constexpr int kMaxFollowUpRounds = 4;
constexpr int kTombstoneHealAfterRounds = 3;

constexpr char kReasonTombstone[] = "tombstone";
constexpr char kReasonForeign[] = "payload is not settings";
constexpr char kReasonUndecryptable[] = "ciphertext could not be opened";

std::optional<int64_t> ParseInt64(const std::optional<std::string>& text) {
  if (!text || text->empty()) {
    return std::nullopt;
  }
  int64_t value = 0;
  bool negative = false;
  size_t i = 0;
  if ((*text)[0] == '-') {
    negative = true;
    i = 1;
  }
  if (i == text->size()) {
    return std::nullopt;
  }
  for (; i < text->size(); ++i) {
    const char c = (*text)[i];
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    value = value * 10 + (c - '0');
  }
  return negative ? -value : value;
}

std::string Hex(const std::string& bytes) {
  return keys::HexLower(keys::Bytes(bytes.begin(), bytes.end()));
}

std::optional<std::string> Unhex(const std::string& hex) {
  if (hex.size() % 2) {
    return std::nullopt;
  }
  std::string out;
  for (size_t i = 0; i < hex.size(); i += 2) {
    int value = 0;
    for (size_t j = i; j < i + 2; ++j) {
      const char c = hex[j];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= c - '0';
      } else if (c >= 'a' && c <= 'f') {
        value |= c - 'a' + 10;
      } else {
        return std::nullopt;
      }
    }
    out.push_back(static_cast<char>(value));
  }
  return out;
}

std::optional<std::string> NormalizedMarker(
    const std::optional<std::string>& marker) {
  if (!marker || marker->empty()) {
    return std::nullopt;
  }
  return marker;
}

// PhiEntityCodec: AES-256-GCM combined, version byte first.
std::optional<std::string> Encrypt(const PhiEntity& entity,
                                   const keys::Bytes& key) {
  const std::string plain = entity.Serialize();
  const std::optional<keys::Bytes> sealed =
      keys::SealWithSymmetric(keys::Bytes(plain.begin(), plain.end()), key);
  if (!sealed) {
    return std::nullopt;
  }
  return std::string(sealed->begin(), sealed->end());
}

bool Decrypt(const std::string& ciphertext,
             const keys::Bytes& key,
             PhiEntity* out) {
  const std::optional<keys::Bytes> plain = keys::OpenWithSymmetric(
      keys::Bytes(ciphertext.begin(), ciphertext.end()), key);
  return plain && out->ParseFrom(std::string(plain->begin(), plain->end()));
}

}  // namespace

std::vector<std::string> EngineStateKeys() {
  return {kEntityIdKey,   kVersionKey,    kLastEntityKey,
          kTombstoneRoundsKey, kHasAdoptedKey, kUnreadableKey,
          kHlcMaxKey,     kWallClockOffsetKey};
}

std::string SettingsClientTagHash() {
  return ClientTagHash(kSettingsClientTag);
}

const char* SyncPhaseName(SyncPhase phase) {
  switch (phase) {
    case SyncPhase::kChecking:
      return "checking";
    case SyncPhase::kInitialSync:
      return "initialSync";
    case SyncPhase::kSyncing:
      return "syncing";
    case SyncPhase::kUpToDate:
      return "upToDate";
    case SyncPhase::kOffline:
      return "offline";
    case SyncPhase::kNeedsAttention:
      break;
  }
  return "needsAttention";
}

const char* RoundOutcomeName(RoundOutcome outcome) {
  switch (outcome) {
    case RoundOutcome::kOk:
      return "ok";
    case RoundOutcome::kGated:
      return "gated";
    case RoundOutcome::kPageBudgetExhausted:
      return "page_budget_exhausted";
    case RoundOutcome::kLocalReadFailed:
      return "local_read_failed";
    case RoundOutcome::kUnusableSettings:
      return "unusable_settings";
    case RoundOutcome::kCursorSaveFailed:
      return "cursor_save_failed";
    case RoundOutcome::kPullFailed:
      return "pull_failed";
    case RoundOutcome::kNotMyBirthday:
      return "not_my_birthday";
  }
  return "unknown";
}

bool MarkerFile::operator==(const MarkerFile& other) const {
  return marker == other.marker && store_birthday == other.store_birthday &&
         removal_pending == other.removal_pending &&
         requires_reconfiguration == other.requires_reconfiguration &&
         reset_operation_id == other.reset_operation_id;
}

// --- Settings ---------------------------------------------------------------------------

SettingEntity SnapshotSettings(const std::vector<SyncableSetting>& settings,
                               SettingsSidecars& sidecars,
                               int64_t now,
                               bool* sidecars_failed) {
  SettingEntity entity;
  for (const SyncableSetting& setting : settings) {
    std::optional<SettingValue> value = setting.read ? setting.read()
                                                     : std::nullopt;
    if (!value) {
      continue;
    }
    const std::string current = ValueSignature(*value);
    const std::optional<int64_t> stored = sidecars.Timestamp(setting.key);
    int64_t stamp = 0;
    if (stored && sidecars.Value(setting.key) == current) {
      stamp = *stored;
    } else {
      stamp = HybridClock::EditStamp(now, stored.value_or(0));
      if (!sidecars.Set(setting.key, stamp, current) && sidecars_failed) {
        *sidecars_failed = true;
      }
    }
    value->updated_at_ms = stamp;
    entity.values[setting.key] = std::move(*value);
  }
  return entity;
}

std::vector<std::string> ApplySettings(const SettingEntity& entity,
                                       const std::vector<SyncableSetting>& settings,
                                       SettingsSidecars& sidecars,
                                       bool* sidecars_failed) {
  std::vector<std::string> applied;
  for (const SyncableSetting& setting : settings) {
    const auto it = entity.values.find(setting.key);
    if (it == entity.values.end()) {
      continue;
    }
    if (setting.write) {
      setting.write(it->second);
    }
    // A refused write (the read-back differs) leaves the sidecars alone, so
    // the local value is re-pushed rather than marked as synced.
    const std::string expected = ValueSignature(it->second);
    const std::optional<SettingValue> stored =
        setting.read ? setting.read() : std::nullopt;
    if (!stored || ValueSignature(*stored) != expected) {
      continue;
    }
    if (!sidecars.Set(setting.key, it->second.updated_at_ms, expected) &&
        sidecars_failed) {
      *sidecars_failed = true;
    }
    applied.push_back(setting.key);
  }
  return applied;
}

// --- The engine -------------------------------------------------------------------------

PhiSyncEngine::PhiSyncEngine(EngineDeps deps) : deps_(std::move(deps)) {
  marker_ = deps_.marker_store->Load();
  if (marker_.requires_reconfiguration) {
    UpdateStatus(SyncPhase::kNeedsAttention);
  }
  hlc_ = HybridClock(ParseInt64(deps_.state->Get(kHlcMaxKey)).value_or(0));
  wall_clock_offset_ms_ =
      ParseInt64(deps_.state->Get(kWallClockOffsetKey)).value_or(0);
  if (deps_.space_store) {
    space_section_enabled_ = deps_.space_store->Load().space_section_enabled;
  }
  // A key registered after this device adopted the account (a newer
  // build's registry) is seeded at stamp 0, the Mac's reseed: the account's
  // value wins over this device's own, which was never an edit.
  if (has_adopted()) {
    for (const SyncableSetting& setting : deps_.settings) {
      if (deps_.sidecars->Timestamp(setting.key) || !setting.read) {
        continue;
      }
      if (const std::optional<SettingValue> value = setting.read()) {
        deps_.sidecars->Set(setting.key, 0, ValueSignature(*value));
      }
    }
  }
}

void PhiSyncEngine::SetSpaceSyncEnabled(bool enabled) {
  if (!deps_.space_store) {
    return;
  }
  Run(enabled ? Round::kSpaceGateOpen : Round::kSpaceGateShut);
}

void PhiSyncEngine::HandleLocalSpaceChange() {
  Run(Round::kLocalSpaceChange);
}

void PhiSyncEngine::RecordLocalSpaceDeletion(const std::string& local_space_id) {
  pending_deletion_space_id_ = local_space_id;
  Run(Round::kRecordDeletion);
}

void PhiSyncEngine::RetentionSweep() {
  Run(Round::kRetention);
}

void PhiSyncEngine::HandleLocalOwnedChange() {
  Run(Round::kLocalOwnedChange);
}

PhiSyncEngine::~PhiSyncEngine() = default;

void PhiSyncEngine::PullOnce() {
  Run(Round::kPull);
}

void PhiSyncEngine::PushLocalSettings() {
  Run(Round::kPush);
}

void PhiSyncEngine::HandleLocalSettingsChange() {
  Run(Round::kLocalChange);
}

bool PhiSyncEngine::has_adopted() const {
  return deps_.state->Get(kHasAdoptedKey).has_value();
}

void PhiSyncEngine::Run(Round round) {
  if (stopped_ || marker_.requires_reconfiguration) {
    return;
  }
  // Data rounds report status (the Mac's reportsStatus set).
  const bool reports_status =
      round == Round::kPull || round == Round::kPush ||
      round == Round::kLocalChange || round == Round::kLocalSpaceChange ||
      round == Round::kLocalOwnedChange;
  const uint64_t status_revision =
      reports_status ? UpdateStatus(status().last_success_ms
                                        ? SyncPhase::kSyncing
                                        : SyncPhase::kInitialSync)
                     : status().revision;
  round_offline_ = false;
  can_publish_this_round_ = false;
  cursor_save_failures_ = 0;
  round_outcome_ = RoundOutcome::kOk;
  round_pages_ = 0;
  round_marker_advanced_ = false;
  round_outbound_failed_ = false;
  space_counters_ = SpaceRoundCounters();
  space_list_unreachable_ = false;
  did_refresh_profiles_this_round_ = false;
  owned_round_started_.clear();
  owned_read_failed_.clear();
  owned_loss_observed_at_entry_.clear();
  owned_parked_retry_done_.clear();
  owned_tag_indices_.clear();
  owned_tables_.clear();
  owned_must_republish_.clear();
  owned_deferred_owners_.clear();
  owned_maps_this_round_.reset();
  owned_counters_.clear();
  switch (round) {
    case Round::kSpaceGateOpen:
    case Round::kSpaceGateShut:
      ApplySpaceGate(round == Round::kSpaceGateOpen);
      break;
    case Round::kLocalSpaceChange:
      // Record this device's Space edits at the time the user made them,
      // BEFORE the push's pull gate.
      StampLocalSpaceEdits();
      Push(/*retry_on_conflict=*/true);
      break;
    case Round::kRecordDeletion: {
      // No mapping: never published, nothing to tombstone.
      const std::optional<std::string> uuid =
          deps_.space_access
              ? deps_.space_access->SyncUuid(pending_deletion_space_id_)
              : std::nullopt;
      if (uuid) {
        const std::string id = *uuid;
        MutateSpaceTable(
            [&id](SpaceSyncTable& table) { table.RecordLocalDeletion(id); });
      }
      break;
    }
    case Round::kRetention:
      ApplyRetentionSweep();
      break;
    case Round::kPull:
      Pull(/*then_push=*/true);
      break;
    case Round::kPush:
    case Round::kLocalOwnedChange:
      Push(/*retry_on_conflict=*/true);
      break;
    case Round::kLocalChange:
      // R2.2: stamp the sidecars BEFORE the pull gate, so a preference
      // changed offline carries its own edit time. Only once this device
      // has settings history — before that, a snapshot would stamp every key
      // now and publish the device's defaults over the account.
      if (has_adopted()) {
        SnapshotLocalSettings();
      }
      Push(/*retry_on_conflict=*/true);
      break;
  }
  if (!stopped_ && !marker_.requires_reconfiguration) {
    for (const SyncableSetting& setting : deps_.settings) {
      if (setting.reconcile) {
        setting.reconcile();
      }
    }
  }
  RoundOutcome outcome = round_outcome_;
  if (outcome != RoundOutcome::kNotMyBirthday) {
    if (cursor_save_failures_ > 0) {
      outcome = space_list_unreachable_ ? RoundOutcome::kLocalReadFailed
                                        : RoundOutcome::kCursorSaveFailed;
    } else if (outcome == RoundOutcome::kOk && deps_.space_store &&
               !space_section_enabled_) {
      outcome = RoundOutcome::kGated;
    }
  }
  last_outcome_ = outcome;
  last_pages_ = round_pages_;
  last_marker_advanced_ = round_marker_advanced_;
  if (reports_status) {
    FinishStatusRound(status_revision);
  }
}

SyncStatus PhiSyncEngine::status() const {
  std::lock_guard<std::mutex> hold(status_lock_);
  return status_;
}

uint64_t PhiSyncEngine::UpdateStatus(SyncPhase phase,
                                     std::optional<uint64_t> completing) {
  std::lock_guard<std::mutex> hold(status_lock_);
  const SyncPhase final_phase =
      phase == SyncPhase::kUpToDate && completing &&
              *completing != status_.revision
          ? SyncPhase::kSyncing
          : phase;
  status_.phase = final_phase;
  if (final_phase == SyncPhase::kUpToDate) {
    status_.last_success_ms = deps_.now();
  }
  return ++status_.revision;
}

void PhiSyncEngine::NoteStatusError(const ProtocolStatus& status) {
  round_outbound_failed_ = true;
  if (status.error == ProtocolError::kTransport) {
    round_offline_ = true;
  }
}

void PhiSyncEngine::FinishStatusRound(uint64_t revision) {
  if (stopped_) {
    return;
  }
  if (marker_.requires_reconfiguration) {
    UpdateStatus(SyncPhase::kNeedsAttention);
    return;
  }
  SpaceSyncTable spaces = LoadSpaceTable();
  bool pending_inbound = unreadable_record().has_value() ||
                         !spaces.unreadable_tag_hashes.empty() ||
                         !owned_read_failed_.empty();
  bool pending_outbound = false;
  for (const auto& [id, cursor] : spaces.cursors) {
    pending_inbound = pending_inbound || cursor.pending_apply ||
                      cursor.held_profile_uuid || cursor.pending_tombstone;
    pending_outbound =
        pending_outbound || cursor.pending_projection || cursor.pending_delete;
  }
  for (OwnedKindAdapter* kind : deps_.owned_kinds) {
    bool loss = false;
    const OwnedTable table =
        kind->store().Load(HadRecordsFlag(spaces, kind->kind_id()), &loss);
    for (const auto& [identity, cursor] : table.cursors) {
      // A pin parked only because its owner shape belongs to another
      // pinned-tab scope is a dormant copy, not held work.
      pending_inbound = pending_inbound ||
                        (cursor.pending_apply && !cursor.parked_for_scope) ||
                        cursor.pending_tombstone ||
                        cursor.pending_partner_lineage;
      pending_outbound = pending_outbound || cursor.pending_delete;
    }
  }
  // `excludedUnmappedOwner` and `refused` recur every round by design and
  // are not pending work.
  for (const auto& [label, counters] : owned_counters_) {
    pending_outbound = pending_outbound || counters.pending_publish > 0;
  }
  const bool follow_up =
      round_outcome_ == RoundOutcome::kPageBudgetExhausted ||
      spaces.drain_in_progress;
  const bool succeeded = can_publish_this_round_ &&
                         round_outcome_ == RoundOutcome::kOk &&
                         !round_outbound_failed_ &&
                         cursor_save_failures_ == 0 && !pending_inbound &&
                         !pending_outbound && !follow_up;
  SyncPhase phase;
  if (round_offline_) {
    phase = SyncPhase::kOffline;
  } else if (round_outbound_failed_ || cursor_save_failures_ > 0 ||
             pending_inbound ||
             (round_outcome_ != RoundOutcome::kOk &&
              round_outcome_ != RoundOutcome::kPageBudgetExhausted)) {
    phase = SyncPhase::kNeedsAttention;
  } else if (succeeded) {
    phase = SyncPhase::kUpToDate;
  } else {
    phase = status().last_success_ms ? SyncPhase::kSyncing
                                     : SyncPhase::kInitialSync;
  }
  UpdateStatus(phase, revision);
}

bool PhiSyncEngine::Pull(bool then_push) {
  can_publish_this_round_ = false;
  if (stopped_) {
    return false;
  }
  keys::Bytes key;
  if (!deps_.domain_keys->DomainKey(&key)) {
    return false;
  }
  if (stopped_) {
    return false;
  }

  // Review A3: a settings entity refused under an older key or build is
  // re-read by ONE full replay, armed marker-first.
  const std::string key_fingerprint = keys::HexLower(keys::Sha256(key));
  if (const std::optional<UnreadableRecord> record = unreadable_record()) {
    const bool healable =
        record->reason == kReasonUndecryptable
            ? (record->key_fingerprint != key_fingerprint ||
               record->build != deps_.build_identity)
        : record->reason == kReasonForeign
            ? record->build != deps_.build_identity
            : false;
    if (healable) {
      if (!PersistStoredMarker(std::nullopt)) {
        round_outcome_ = RoundOutcome::kCursorSaveFailed;
        return false;
      }
      set_unreadable_record(std::nullopt);
    }
  }

  // The Space side obeys one rule: no copy of the table spans a network
  // call, and every flag is persisted the moment it is observed — before
  // the page's marker.
  const bool spaces_live = space_live();
  if (spaces_live && (!ArmSpaceReplayIfNeeded() || !ArmKindReplayIfNeeded())) {
    round_outcome_ = RoundOutcome::kCursorSaveFailed;
    return false;
  }
  const SpaceSyncTable table_at_entry = LoadSpaceTable();
  if (spaces_live && !stored_marker() && !table_at_entry.drain_in_progress) {
    // Persisted at once: page 1 makes the marker non-nil, and a round that
    // dies on page 2 must still leave the drain armed.
    MutateSpaceTable([](SpaceSyncTable& table) {
      table.drain_in_progress = true;
      table.has_drained_full_replay = false;
    });
  }
  // The account's profile list, refreshed BEFORE paging so this round's
  // bindings resolve against it (at most every 30 s).
  if (spaces_live && !did_refresh_profiles_this_round_) {
    const int64_t elapsed = deps_.now() - last_profile_refresh_at_ms_;
    if (last_profile_refresh_at_ms_ == 0 || elapsed >= 30'000) {
      did_refresh_profiles_this_round_ = true;
      const ProfileRefreshOutcome outcome =
          deps_.space_access->RefreshAccountProfiles();
      if (stopped_) {
        return false;
      }
      if (outcome != ProfileRefreshOutcome::kFailed &&
          outcome != ProfileRefreshOutcome::kSkipped) {
        last_profile_refresh_at_ms_ = deps_.now();
      }
      space_counters_.profile_refresh =
          outcome == ProfileRefreshOutcome::kFailed    ? "failed"
          : outcome == ProfileRefreshOutcome::kSkipped ? "skipped"
                                                       : "ok";
    }
  }
  // The owned kinds read their local rows once (routing needs their tags).
  if (spaces_live) {
    BeginOwnedRound();
  }
  const std::map<std::string, std::string> tag_index =
      spaces_live ? SpaceTagIndex(table_at_entry)
                  : std::map<std::string, std::string>();

  const std::string settings_tag_hash = SettingsClientTagHash();
  const bool started_from_scratch = !stored_marker().has_value();
  const std::optional<std::string> marker_at_entry = stored_marker();
  const bool records_gated_marker_moves = !spaces_live && deps_.space_store;
  bool marker_move_recorded = false;
  enum class View { kAbsent, kUsable, kUnusable };
  View view = View::kAbsent;
  SettingEntity usable;
  UnusableReason unusable_reason = UnusableReason::kTombstone;
  bool saw_settings_entity = false;
  bool may_settings_publish = true;
  bool reset_marker_to_nil = false;
  std::optional<std::string> marker = stored_marker();
  int pages = 0;
  bool more = true;

  // Guard 2: an empty Space table that once had records replays the type
  // once — nil marker FIRST, then the latch.
  if (spaces_live && table_at_entry.cursors.empty() &&
      table_at_entry.had_records && !table_at_entry.did_replay_for_empty_table) {
    if (!PersistStoredMarker(std::nullopt)) {
      round_outcome_ = RoundOutcome::kCursorSaveFailed;
      return false;
    }
    if (!MutateSpaceTable([](SpaceSyncTable& table) {
          table.did_replay_for_empty_table = true;
          table.has_drained_full_replay = false;
          table.drain_in_progress = true;
        })) {
      round_outcome_ = RoundOutcome::kCursorSaveFailed;
      return false;
    }
    marker.reset();
    reset_marker_to_nil = true;
  }
  if (spaces_live && !table_at_entry.cursors.empty() &&
      !table_at_entry.had_records) {
    MutateSpaceTable([](SpaceSyncTable& table) { table.had_records = true; });
  }

  while (more && pages < kMaxPullPages) {
    GetUpdatesResult response;
    const ProtocolStatus status = deps_.client->GetUpdates(
        marker.value_or(std::string()), marker_.store_birthday, &response);
    if (status.error == ProtocolError::kNotMyBirthday) {
      RequireReconfiguration();
      return false;
    }
    if (!status.ok()) {
      NoteStatusError(status);
      round_outcome_ = RoundOutcome::kPullFailed;
      // A drain interrupted mid-way is replayed, never resumed past a gap.
      if (spaces_live && LoadSpaceTable().drain_in_progress) {
        PersistStoredMarker(std::nullopt);
      }
      return false;
    }
    ObserveServerDate();
    if (stopped_) {
      return false;
    }
    set_stored_birthday(response.store_birthday);
    const std::string page_marker = response.new_marker;
    more = response.changes_remaining;
    ++pages;
    ++round_pages_;

    SpacePullBatch batch;
    std::map<std::string, OwnedPullBatch> owned_batches;
    bool page_carried_settings = false;
    for (const RemoteEntity& entity : response.entities) {
      if (entity.client_tag_hash != settings_tag_hash) {
        // Routed by tag before any decrypt: a tombstone has no ciphertext.
        if (spaces_live &&
            (tag_index.count(entity.client_tag_hash) ||
             !RouteOwnedEntity(entity, key, &owned_batches))) {
          RouteSpaceEntity(entity, key, tag_index, &batch);
        }
        continue;
      }
      if (!entity.entity_id.empty()) {
        set_stored_entity_id(entity.entity_id);
      }
      // max(version): replayed pages may re-serve an older one.
      set_stored_version(
          std::max(stored_version().value_or(0), entity.version));
      saw_settings_entity = true;
      page_carried_settings = true;
      if (entity.deleted) {
        // Another device's tombstone: nothing to apply, and publishing over
        // it would silently undelete the account's settings.
        view = View::kUnusable;
        unusable_reason = UnusableReason::kTombstone;
        continue;
      }
      PhiEntity decoded;
      if (!Decrypt(entity.ciphertext, key, &decoded)) {
        view = View::kUnusable;
        unusable_reason = UnusableReason::kUndecryptable;
        continue;
      }
      if (decoded.kind != PhiEntity::Kind::kSetting) {
        view = View::kUnusable;
        unusable_reason = UnusableReason::kForeignPayload;
        continue;
      }
      view = View::kUsable;
      usable = decoded.setting;
    }

    if (page_carried_settings && view == View::kUsable) {
      set_tombstone_rounds(0);
      set_unreadable_record(std::nullopt);
      Apply(usable, /*adopt=*/!has_adopted());
    } else if (page_carried_settings && view == View::kUnusable) {
      // Durable refusal: the baseline goes, so `PushSettings` reads "an
      // entity id with no baseline" and never commits over the bytes.
      set_unreadable_record(UnreadableRecord{
          unusable_reason == UnusableReason::kTombstone ? kReasonTombstone
          : unusable_reason == UnusableReason::kForeignPayload
              ? kReasonForeign
              : kReasonUndecryptable,
          key_fingerprint, deps_.build_identity});
      set_stored_last_entity(std::nullopt);
      may_settings_publish = false;
      NoteUnusable(unusable_reason);
    }

    if (spaces_live) {
      FlushSpaceObservations(batch);
      FlushOwnedObservations(owned_batches);
      SpaceSyncTable table = LoadSpaceTable();
      space_counters_.pulled +=
          static_cast<int>(batch.decoded.size() + batch.tombstones.size());
      ApplySpaces(batch, &table);
      ApplySpaceTombstones(batch, &table);
      WriteSpaceTable(table);

      // Owned kinds after Spaces: a row can land in a Space created here.
      owned_maps_this_round_.reset();
      const OwnedOwnerMaps maps = OwnedRoundMaps();
      for (OwnedKindAdapter* kind : deps_.owned_kinds) {
        RetryParkedOwnedClaims(*kind, maps);
      }
      for (OwnedKindAdapter* kind : deps_.owned_kinds) {
        const auto it = owned_batches.find(kind->label());
        ApplyOwnedKind(*kind, it == owned_batches.end() ? OwnedPullBatch()
                                                        : it->second,
                       maps);
      }
    }

    // The marker is the page's LAST write; any earlier failure stops it.
    if (cursor_save_failures_ > 0) {
      round_outcome_ = RoundOutcome::kCursorSaveFailed;
      break;
    }
    marker = page_marker;
    if (!reset_marker_to_nil) {
      if (records_gated_marker_moves && !marker_move_recorded &&
          NormalizedMarker(marker) != marker_at_entry) {
        // The gate is shut and the marker walked on: whatever it walked past
        // must be replayed when the gate opens. The flag goes BEFORE the
        // marker.
        if (!MutateSpaceTable([](SpaceSyncTable& table) {
              table.marker_moved_while_gate_shut = true;
            })) {
          round_outcome_ = RoundOutcome::kCursorSaveFailed;
          break;
        }
        marker_move_recorded = true;
      }
      if (!PersistStoredMarker(marker)) {
        round_outcome_ = RoundOutcome::kCursorSaveFailed;
        break;
      }
      if (stored_marker() != marker_at_entry) {
        round_marker_advanced_ = true;
      }
    }
  }
  const bool drained = !more;
  if (!drained && pages >= kMaxPullPages) {
    round_outcome_ = RoundOutcome::kPageBudgetExhausted;
  }
  if (spaces_live && drained && !reset_marker_to_nil &&
      round_outcome_ != RoundOutcome::kCursorSaveFailed) {
    const std::string birthday = marker_.store_birthday;
    std::set<std::string> kinds;
    for (OwnedKindAdapter* kind : deps_.owned_kinds) {
      kinds.insert(kind->label());
    }
    if (!MutateSpaceTable([&birthday, &kinds](SpaceSyncTable& table) {
          if (!table.drain_in_progress) {
            return;
          }
          table.drain_in_progress = false;
          table.has_drained_full_replay = true;
          table.last_drained_birthday = birthday;
          table.replayed_kinds = kinds;
        })) {
      round_outcome_ = RoundOutcome::kCursorSaveFailed;
    }
  }

  // Round-level absence: no page in the drain carried settings.
  if (!saw_settings_entity &&
      round_outcome_ != RoundOutcome::kCursorSaveFailed) {
    const std::optional<UnreadableRecord> record = unreadable_record();
    if (record && record->reason == kReasonTombstone) {
      NoteUnusable(UnusableReason::kTombstone);
    } else {
      set_tombstone_rounds(0);
    }
    if (drained && started_from_scratch && stored_entity_id()) {
      // A full replay carried no settings entity: the row this device
      // points at is gone; the next push creates.
      ClearEntityCursor();
    }
  }
  if (reset_marker_to_nil) {
    PersistStoredMarker(std::nullopt);
  }
  if (round_outcome_ == RoundOutcome::kOk && view == View::kUnusable) {
    round_outcome_ = RoundOutcome::kUnusableSettings;
  }

  can_publish_this_round_ =
      drained && !stopped_ && cursor_save_failures_ == 0;
  if (then_push && can_publish_this_round_) {
    if (may_settings_publish) {
      PushSettings(/*retry_on_conflict=*/false);
    }
    PushSpaces(/*retry_on_conflict=*/false, nullptr);
    PushOwnedItems(/*retry_on_conflict=*/true);
  }
  if (!drained && follow_up_rounds_used_ < kMaxFollowUpRounds) {
    ++follow_up_rounds_used_;
    if (deps_.request_follow_up) {
      deps_.request_follow_up();
    }
  } else if (drained) {
    follow_up_rounds_used_ = 0;
  }
  return can_publish_this_round_;
}

void PhiSyncEngine::NoteUnusable(UnusableReason reason) {
  if (reason != UnusableReason::kTombstone) {
    set_tombstone_rounds(0);
    return;
  }
  const int rounds = tombstone_rounds() + 1;
  set_tombstone_rounds(rounds);
  if (rounds < kTombstoneHealAfterRounds) {
    return;
  }
  // Gone for several pulls: forget the row so the next LOCAL change
  // re-creates it (never publishes by itself).
  ClearEntityCursor();
  set_unreadable_record(std::nullopt);
  set_tombstone_rounds(0);
}

void PhiSyncEngine::Apply(const SettingEntity& remote, bool adopt) {
  if (stopped_) {
    return;
  }
  // Observe before the snapshot below stamps anything.
  ObserveStamps(remote);
  SettingEntity merged;
  if (adopt) {
    // No settings history here: every local key would be stamped now and
    // beat the account's real edits. The first pull adopts wholesale.
    merged = remote;
  } else {
    std::optional<SettingEntity> local = SnapshotLocalSettings();
    if (!local) {
      ++cursor_save_failures_;  // the page replays
      return;
    }
    merged = MergeSettings(*local, remote);
  }
  if (stopped_) {
    return;
  }
  if (deps_.applying_remote) {
    deps_.applying_remote(true);
  }
  bool sidecars_failed = false;
  ApplySettings(merged, deps_.settings, *deps_.sidecars, &sidecars_failed);
  if (deps_.applying_remote) {
    deps_.applying_remote(false);
  }
  if (sidecars_failed) {
    // A landed value without its sidecar would read as a local edit later:
    // the page replays rather than move past it.
    ++cursor_save_failures_;
  }
  // What the server holds, not what this device now holds.
  set_stored_last_entity(remote);
  set_has_adopted(true);
}

void PhiSyncEngine::Push(bool retry_on_conflict) {
  if (!Pull(/*then_push=*/false)) {
    return;
  }
  PushSettings(retry_on_conflict);
  PushSpaces(retry_on_conflict, nullptr);
  PushOwnedItems(retry_on_conflict);
}

void PhiSyncEngine::PushSettings(bool retry_on_conflict) {
  if (stopped_ || !can_publish_this_round_) {
    return;
  }
  // The server holds bytes this device could not read: never overwrite
  // them. A refusal already on record is re-read by Pull itself once the key
  // or the build changes — rewinding here would replay the whole account
  // every round; only an unexplained missing baseline rewinds.
  if (stored_entity_id() && !stored_last_entity()) {
    if (!unreadable_record()) {
      PersistStoredMarker(std::nullopt);
    }
    return;
  }
  keys::Bytes key;
  if (!deps_.domain_keys->DomainKey(&key) || stopped_ ||
      !can_publish_this_round_) {
    return;
  }
  const std::optional<SettingEntity> last = stored_last_entity();
  const std::optional<SettingEntity> local = SnapshotLocalSettings();
  if (!local) {
    return;
  }
  // Merging against the last server entity keeps keys this build does not
  // know (a newer client's) instead of deleting them on every push.
  const SettingEntity outgoing =
      MergeSettings(*local, last.value_or(SettingEntity()));
  if (last && outgoing == *last) {
    return;
  }
  PhiEntity wrapper;
  wrapper.kind = PhiEntity::Kind::kSetting;
  wrapper.setting = outgoing;
  const std::optional<std::string> ciphertext = Encrypt(wrapper, key);
  if (!ciphertext) {
    return;
  }
  CommitEntry entry;
  entry.entity_id = stored_entity_id();
  entry.client_tag_hash = SettingsClientTagHash();
  entry.name = kSettingsClientTag;
  entry.ciphertext = ciphertext;
  entry.base_version = stored_version().value_or(0);
  std::vector<CommitOutcome> outcomes;
  const ProtocolStatus status =
      deps_.client->Commit({entry}, marker_.store_birthday, &outcomes);
  if (status.error == ProtocolError::kNotMyBirthday) {
    RequireReconfiguration();
    return;
  }
  if (!status.ok()) {
    NoteStatusError(status);
    return;
  }
  ObserveServerDate();
  if (stopped_ || outcomes.empty()) {
    return;
  }
  const CommitOutcome& outcome = outcomes.front();
  switch (outcome.kind) {
    case CommitOutcome::Kind::kApplied:
      if (!outcome.entity_id.empty()) {
        set_stored_entity_id(outcome.entity_id);
      }
      set_stored_version(outcome.version);
      set_stored_birthday(outcome.store_birthday);
      set_stored_last_entity(outgoing);
      set_tombstone_rounds(0);
      set_has_adopted(true);
      break;
    case CommitOutcome::Kind::kConflict:
      if (!retry_on_conflict) {
        round_outbound_failed_ = true;
        return;
      }
      if (!Pull(/*then_push=*/false)) {
        return;
      }
      PushSettings(/*retry_on_conflict=*/false);
      break;
    case CommitOutcome::Kind::kInvalidMessage:
      DropEntityCursorAfterInvalidMessage();
      break;
    case CommitOutcome::Kind::kRejected:
      round_outbound_failed_ = true;
      break;
  }
}

void PhiSyncEngine::DropEntityCursorAfterInvalidMessage() {
  // The server cannot find the row this commit names: drop the identity and
  // the marker so the next round rediscovers or re-creates it. Same account:
  // the birthday and `hasAdopted` stay.
  round_outbound_failed_ = true;
  ClearRemoteCursor();
}

void PhiSyncEngine::RequireReconfiguration() {
  // The store was reset under this device: pause, keep every cursor, and
  // wait for the user's "Set up sync again".
  UpdateStatus(SyncPhase::kNeedsAttention);
  can_publish_this_round_ = false;
  round_outcome_ = RoundOutcome::kNotMyBirthday;
  MarkerFile updated = marker_;
  updated.requires_reconfiguration = true;
  marker_ = updated;
  deps_.marker_store->Save(updated);
}

std::optional<SettingEntity> PhiSyncEngine::SnapshotLocalSettings() {
  if (stopped_) {
    return std::nullopt;
  }
  bool sidecars_failed = false;
  SettingEntity entity = SnapshotSettings(deps_.settings, *deps_.sidecars,
                                          HlcNow(), &sidecars_failed);
  if (sidecars_failed) {
    // A stamp that did not persist would be issued again later as a new
    // edit: nothing publishes from this snapshot.
    round_outbound_failed_ = true;
    return std::nullopt;
  }
  // The sidecars outlive an account switch while hlcMax does not: fold the
  // stamps back in so no stamp below a published one is ever issued.
  ObserveStamps(entity);
  return entity;
}

// --- The Space section -----------------------------------------------------------------

namespace {

std::string SpaceTagHash(const std::string& uuid) {
  return ClientTagHash(SpaceClientTag(uuid));
}

std::optional<SpaceEntity> ParseSpaceBytes(const std::optional<std::string>& bytes) {
  SpaceEntity entity;
  if (!bytes || !entity.ParseFrom(*bytes)) {
    return std::nullopt;
  }
  return entity;
}

}  // namespace

SpaceSyncTable PhiSyncEngine::LoadSpaceTable() {
  return deps_.space_store ? deps_.space_store->Load() : SpaceSyncTable();
}

bool PhiSyncEngine::WriteSpaceTable(const SpaceSyncTable& table) {
  // A retired engine and a settings-only engine: successful no-ops.
  if (stopped_ || !deps_.space_store) {
    return true;
  }
  if (!deps_.space_store->Save(table)) {
    ++cursor_save_failures_;
    return false;
  }
  return true;
}

bool PhiSyncEngine::MutateSpaceTable(
    const std::function<void(SpaceSyncTable&)>& mutate) {
  if (!deps_.space_store) {
    return true;
  }
  SpaceSyncTable table = LoadSpaceTable();
  mutate(table);
  return WriteSpaceTable(table);
}

void PhiSyncEngine::ApplySpaceGate(bool enabled) {
  if (enabled == space_section_enabled_) {
    return;
  }
  space_section_enabled_ = enabled;
  if (!MutateSpaceTable([enabled](SpaceSyncTable& table) {
        table.space_section_enabled = enabled;
      })) {
    round_outcome_ = RoundOutcome::kCursorSaveFailed;
    return;
  }
  if (enabled && !ArmSpaceReplayIfNeeded()) {
    round_outcome_ = RoundOutcome::kCursorSaveFailed;
  }
}

// The replay half of a gate opening: the shared marker walked past every
// Space entity while the gate was shut. Nil marker FIRST, then the latch.
bool PhiSyncEngine::ArmSpaceReplayIfNeeded() {
  const SpaceSyncTable table = LoadSpaceTable();
  // Third trigger (phibrowser-mac 48d4c4a6): a build that predates a relaxed
  // refusal predicate walked the shared marker past every entity it refused,
  // and refusals record nothing, so only a replay from a nil marker delivers
  // them again. The version is latched in the same write as the drain flags,
  // after the nil marker, so a failed write leaves it unlatched and the next
  // live pull replays again.
  const bool acceptance_stale =
      table.inbound_acceptance_version <
      SpaceSyncTable::kCurrentInboundAcceptanceVersion;
  if (!table.marker_moved_while_gate_shut && !acceptance_stale &&
      (table.has_drained_full_replay || table.drain_in_progress)) {
    return true;
  }
  if (!PersistStoredMarker(std::nullopt)) {
    return false;
  }
  return MutateSpaceTable([](SpaceSyncTable& t) {
    t.marker_moved_while_gate_shut = false;
    t.has_drained_full_replay = false;
    t.drain_in_progress = true;
    t.inbound_acceptance_version =
        SpaceSyncTable::kCurrentInboundAcceptanceVersion;
  });
}

bool PhiSyncEngine::ArmKindReplayIfNeeded() {
  const SpaceSyncTable table = LoadSpaceTable();
  if (table.drain_in_progress) {
    return true;  // the drain under way records every kind at its end
  }
  bool missing = false;
  for (OwnedKindAdapter* kind : deps_.owned_kinds) {
    missing = missing || !table.replayed_kinds.count(kind->label());
  }
  if (!missing) {
    return true;
  }
  if (!PersistStoredMarker(std::nullopt)) {
    return false;
  }
  return MutateSpaceTable([](SpaceSyncTable& t) {
    t.has_drained_full_replay = false;
    t.drain_in_progress = true;
  });
}

// client_tag_hash → Space uuid, from cursor keys, mappings and the default:
// a tombstone carries no payload to identify it by.
std::map<std::string, std::string> PhiSyncEngine::SpaceTagIndex(
    const SpaceSyncTable& table) {
  std::set<std::string> uuids;
  for (const auto& [uuid, cursor] : table.cursors) {
    uuids.insert(uuid);
  }
  uuids.insert(kDefaultSpaceUuid);
  if (deps_.space_access) {
    for (const auto& [space_id, uuid] : deps_.space_access->AllSpaceMappings()) {
      uuids.insert(uuid);
    }
  }
  std::map<std::string, std::string> index;
  for (const std::string& uuid : uuids) {
    index[SpaceTagHash(uuid)] = uuid;
  }
  return index;
}

void PhiSyncEngine::RouteSpaceEntity(
    const RemoteEntity& entity,
    const keys::Bytes& key,
    const std::map<std::string, std::string>& tag_index,
    SpacePullBatch* batch) {
  // A tombstone FIRST: its specifics are the server's default, so a decrypt
  // would call every delete "unreadable".
  if (entity.deleted) {
    const auto it = tag_index.find(entity.client_tag_hash);
    if (it != tag_index.end()) {
      batch->tombstones.push_back({it->second, entity.entity_id, entity.version});
    }
    return;
  }
  PhiEntity decoded;
  if (!Decrypt(entity.ciphertext, key, &decoded)) {
    batch->unreadable_hashes.push_back(entity.client_tag_hash);
    return;
  }
  // Another kind (bookmarks, pins, rules, a newer build's): not ours.
  if (decoded.kind != PhiEntity::Kind::kSpace) {
    return;
  }
  // The payload must hash back to the tag it arrived under.
  if (SpaceTagHash(decoded.space.space_uuid) != entity.client_tag_hash) {
    batch->unreadable_hashes.push_back(entity.client_tag_hash);
    return;
  }
  batch->decoded.push_back({decoded.space.space_uuid, decoded.space,
                            entity.entity_id, entity.version});
}

void PhiSyncEngine::FlushSpaceObservations(const SpacePullBatch& batch) {
  if (batch.unreadable_hashes.empty()) {
    return;
  }
  const int64_t seen_at = deps_.now();
  MutateSpaceTable([&](SpaceSyncTable& table) {
    for (const std::string& hash : batch.unreadable_hashes) {
      table.unreadable_tag_hashes[hash] = seen_at;
    }
  });
}

std::map<std::string, SpaceEntity> PhiSyncEngine::SnapshotLocal(
    const std::vector<LocalSpace>& spaces,
    const SpaceSyncTable& table,
    bool ensure_mapped) {
  SpaceLocalAccess& access = *deps_.space_access;
  std::map<std::string, std::optional<std::string>> uuid_by_space;
  std::map<std::string, std::optional<std::string>> uuid_by_profile;
  for (const LocalSpace& space : spaces) {
    uuid_by_space[space.space_id] = ensure_mapped
                                        ? access.EnsureMapped(space.space_id)
                                        : access.SyncUuid(space.space_id);
    if (!uuid_by_profile.count(space.profile_id)) {
      uuid_by_profile[space.profile_id] = access.GlobalUuid(space.profile_id);
    }
  }
  return SnapshotSpaces(
      spaces, table,
      [&](const std::string& profile_id) -> std::optional<std::string> {
        const auto it = uuid_by_profile.find(profile_id);
        return it == uuid_by_profile.end() ? std::nullopt : it->second;
      },
      [&](const std::string& space_id) -> std::optional<std::string> {
        const auto it = uuid_by_space.find(space_id);
        return it == uuid_by_space.end() ? std::nullopt : it->second;
      },
      HlcNow());
}

void PhiSyncEngine::ApplySpaces(const SpacePullBatch& batch,
                                SpaceSyncTable* table) {
  if (stopped_ || !deps_.space_access) {
    return;
  }
  SpaceLocalAccess& access = *deps_.space_access;
  const SpaceSyncTable table_at_entry = *table;

  // A held binding whose account profile now resolves re-lands from its
  // own baseline.
  std::map<std::string, SpaceEntity> reparked;
  for (const auto& [uuid, cursor] : table->cursors) {
    if (!cursor.held_profile_uuid || cursor.pending_apply) {
      continue;
    }
    std::optional<SpaceEntity> entity = ParseSpaceBytes(cursor.reconciled);
    if (entity && access.LocalProfileId(*cursor.held_profile_uuid)) {
      reparked[uuid] = std::move(*entity);
    }
  }
  struct Item {
    std::string uuid;
    SpaceEntity entity;
    std::string entity_id;
    int64_t version = 0;
    bool from_server = true;
  };
  std::vector<Item> pending;
  for (const auto& [uuid, cursor] : table->cursors) {  // sorted by uuid
    const auto re = reparked.find(uuid);
    if (re != reparked.end()) {
      pending.push_back({uuid, re->second, cursor.entity_id.value_or(""),
                         cursor.version, false});
      continue;
    }
    if (std::optional<SpaceEntity> entity = ParseSpaceBytes(cursor.pending_apply)) {
      pending.push_back({uuid, std::move(*entity), cursor.entity_id.value_or(""),
                         cursor.version, true});
    }
  }
  std::vector<Item> incoming;
  for (const SpacePullBatch::Decoded& d : batch.decoded) {
    incoming.push_back({d.uuid, d.entity, d.entity_id, d.version, true});
  }
  std::stable_sort(incoming.begin(), incoming.end(),
                   [](const Item& a, const Item& b) { return a.uuid < b.uuid; });
  std::vector<Item> all;
  for (Item& p : pending) {
    const bool superseded =
        std::any_of(incoming.begin(), incoming.end(),
                    [&](const Item& i) { return i.uuid == p.uuid; });
    if (!superseded) {
      all.push_back(std::move(p));
    }
  }
  for (Item& i : incoming) {
    all.push_back(std::move(i));
  }
  for (const Item& item : all) {
    ObserveStamps(item.entity);
  }
  // The Space list could not be read: nothing more lands or is dropped — an
  // unanswered "is it there?" never reads as "gone" (which dropped live
  // mappings and landed duplicates on every device). A stalled hop replays
  // this page next round; a store that cannot open parks the page's
  // arrivals instead, so the marker and every other kind move on.
  auto unreachable = [&] {
    *table = table_at_entry;
    if (!access.SpaceStoreUnavailable()) {
      NoteSpaceListUnreachable();
      return;
    }
    for (const Item& item : all) {
      if (!item.from_server) {
        continue;
      }
      SpaceCursor& cursor = table->cursors[item.uuid];
      cursor.pending_apply = item.entity.Serialize();
      if (!item.entity_id.empty()) {
        cursor.entity_id = item.entity_id;
      }
      cursor.version = std::max(cursor.version, item.version);
      ++space_counters_.parked;
    }
  };

  // This device's own local projection per Space with history: the merge's
  // LOCAL side (what the user did here since the baseline).
  std::map<std::string, SpaceEntity> projections;
  if (!all.empty()) {
    SpaceSyncTable projection_table = *table;
    std::set<std::string> with_history;
    for (auto& [uuid, cursor] : projection_table.cursors) {
      if (ParseSpaceBytes(cursor.reconciled)) {
        with_history.insert(uuid);
        cursor.pending_apply.reset();
      }
    }
    const std::optional<std::vector<LocalSpace>> current =
        access.CurrentSpaces();
    if (!current) {
      unreachable();
      return;
    }
    for (auto& [uuid, entity] :
         SnapshotLocal(*current, projection_table, false)) {
      if (with_history.count(uuid)) {
        projections[uuid] = std::move(entity);
      }
    }
  }

  bool landed_any = false;
  for (const Item& item : all) {
    if (stopped_) {
      return;
    }
    SpaceCursor cursor = table->cursors[item.uuid];
    const std::string tag = SpaceTagHash(item.uuid);
    auto park = [&] {
      if (item.from_server) {
        cursor.pending_apply = item.entity.Serialize();
        table->cursors[item.uuid] = cursor;
      }
      ++space_counters_.parked;
    };

    if (SpaceRefuses(item.entity)) {
      cursor.refused_at_ms = deps_.now();
      cursor.pending_apply.reset();
      table->cursors[item.uuid] = cursor;
      ++space_counters_.refused;
      continue;
    }
    if (cursor.deleted_at_ms) {
      cursor.pending_apply.reset();
      table->cursors[item.uuid] = cursor;
      continue;
    }
    if (cursor.pending_delete) {
      // This device deleted it: only harvest the identity for the tombstone.
      if (!item.entity_id.empty()) {
        cursor.entity_id = item.entity_id;
      }
      cursor.version = std::max(cursor.version, item.version);
      cursor.pending_apply.reset();
      table->cursors[item.uuid] = cursor;
      table->unreadable_tag_hashes.erase(tag);
      continue;
    }

    const bool is_default = item.uuid == kDefaultSpaceUuid;
    std::optional<std::string> local_space_id = access.LocalSpaceId(item.uuid);
    if (!is_default && local_space_id) {
      const LocalPresence known = access.IsKnownLocalSpace(*local_space_id);
      if (known == LocalPresence::kUnreachable) {
        unreachable();
        return;
      }
      if (known == LocalPresence::kAbsent) {
        // A dead mapping: the entity lands as a new Space.
        access.DropSpaceMapping(*local_space_id);
        local_space_id.reset();
      }
    }
    std::optional<std::string> profile_id;
    if (!is_default) {
      const std::string remote_uuid = item.entity.profile_uuid.get().str();
      profile_id = access.LocalProfileId(remote_uuid);
      if (profile_id) {
        const LocalPresence known = access.IsKnownLocalProfile(*profile_id);
        if (known == LocalPresence::kUnreachable) {
          unreachable();
          return;
        }
        if (known == LocalPresence::kAbsent) {
          access.DropProfileMapping(*profile_id);
          profile_id.reset();
        }
      }
      if (!profile_id) {
        if (!cursor.reconciled) {
          // §3.5 fallback B: a first landing with an unresolvable profile
          // parks until the profile exists here.
          cursor.pending_apply = item.entity.Serialize();
          if (!item.entity_id.empty()) {
            cursor.entity_id = item.entity_id;
          }
          cursor.version = std::max(cursor.version, item.version);
          table->cursors[item.uuid] = cursor;
          ++space_counters_.parked;
          continue;
        }
        // Fallback A: hold the remote binding, keep the local one.
        const std::optional<std::vector<LocalSpace>> current =
            access.CurrentSpaces();
        if (!current) {
          unreachable();
          return;
        }
        cursor.held_profile_uuid = remote_uuid;
        cursor.held_for_local_profile_id.reset();
        for (const LocalSpace& space : *current) {
          if (local_space_id && space.space_id == *local_space_id) {
            cursor.held_for_local_profile_id = space.profile_id;
          }
        }
      } else {
        cursor.held_profile_uuid.reset();
        cursor.held_for_local_profile_id.reset();
      }
    }

    // A name is not an identity. Until the user explicitly maps a local
    // Space, keep it separate instead of overwriting it on first landing.

    const std::optional<std::vector<LocalSpace>> current =
        access.CurrentSpaces();
    if (!current) {
      unreachable();
      return;
    }
    std::optional<LocalSpace> existing;
    for (const LocalSpace& space : *current) {
      if (local_space_id && space.space_id == *local_space_id) {
        existing = space;
      }
    }
    if (is_default && !existing) {
      const LocalPresence known =
          access.IsKnownLocalProfile(access.DefaultProfileId());
      if (known == LocalPresence::kUnreachable) {
        unreachable();
        return;
      }
      if (known == LocalPresence::kPresent) {
        profile_id = access.DefaultProfileId();
      }
    }
    SpaceEntity merged = item.entity;
    if (std::optional<SpaceEntity> baseline = ParseSpaceBytes(cursor.reconciled)) {
      const auto projection = projections.find(item.uuid);
      merged = MergeSpace(
          projection != projections.end() ? projection->second : *baseline,
          item.entity);
    }
    if (!is_default &&
        merged.profile_uuid.get().str() != item.entity.profile_uuid.get().str()) {
      profile_id = access.LocalProfileId(merged.profile_uuid.get().str());
      if (profile_id) {
        cursor.held_profile_uuid.reset();
        cursor.held_for_local_profile_id.reset();
      }
    }

    if (!local_space_id && !is_default) {
      const std::string new_id = deps_.new_space_id();
      if (!access.MapSpace(new_id, item.uuid)) {
        ++cursor_save_failures_;
        park();
        continue;
      }
      local_space_id = new_id;
    }

    const std::optional<std::string> landed =
        LandSpace(merged, existing, local_space_id, profile_id, access,
                  deps_.new_space_id, deps_.now());
    if (!landed) {
      park();
      continue;
    }
    if (stopped_) {
      return;
    }
    if (!is_default && profile_id && existing &&
        existing->profile_id != *profile_id) {
      const std::optional<std::vector<LocalSpace>> after =
          access.CurrentSpaces();
      if (!after) {
        unreachable();
        return;
      }
      bool rebound = false;
      for (const LocalSpace& space : *after) {
        if (space.space_id == *landed && space.profile_id == *profile_id) {
          rebound = true;
        }
      }
      if (!rebound) {
        park();
        continue;
      }
    }

    cursor.reconciled = merged.Serialize();
    cursor.pending_projection.reset();
    if (item.from_server) {
      cursor.server = item.entity.Serialize();
    }
    if (!item.entity_id.empty()) {
      cursor.entity_id = item.entity_id;
    }
    cursor.version = std::max(cursor.version, item.version);
    cursor.pending_apply.reset();
    table->cursors[item.uuid] = cursor;
    table->unreadable_tag_hashes.erase(tag);
    landed_any = true;
    ++space_counters_.applied;
  }

  if (landed_any) {
    // The account-wide order, once per page, after every Space landed.
    std::map<std::string, std::pair<std::string, std::string>> ranks;
    for (const auto& [uuid, cursor] : table->cursors) {
      if (cursor.hidden || cursor.deleted_at_ms) {
        continue;
      }
      const std::optional<SpaceEntity> entity = ParseSpaceBytes(cursor.reconciled);
      const std::optional<std::string> local = access.LocalSpaceId(uuid);
      if (!entity || !local) {
        continue;
      }
      const auto projection = projections.find(uuid);
      const SpaceEntity effective =
          projection != projections.end()
              ? MergeSpace(projection->second, *entity)
              : *entity;
      ranks[*local] = {effective.rank.get().str(), uuid};
    }
    const std::optional<std::vector<LocalSpace>> strip =
        access.AllSpacesForOrdering();
    if (!strip) {
      unreachable();
      return;
    }
    const std::vector<std::string> order = PlannedSpaceOrder(*strip, ranks);
    if (!access.ApplyOrder(order)) {
      // Replay this page next round rather than record half of it.
      *table = table_at_entry;
      ++cursor_save_failures_;
    }
  }
}

void PhiSyncEngine::ApplySpaceTombstones(const SpacePullBatch& batch,
                                         SpaceSyncTable* table) {
  if (stopped_ || !deps_.space_access) {
    return;
  }
  SpaceLocalAccess& access = *deps_.space_access;
  std::vector<SpacePullBatch::Tombstone> work = batch.tombstones;
  for (const auto& [uuid, cursor] : table->cursors) {
    if (cursor.pending_tombstone &&
        std::none_of(work.begin(), work.end(),
                     [&](const SpacePullBatch::Tombstone& t) {
                       return t.uuid == uuid;
                     })) {
      work.push_back({uuid, cursor.entity_id.value_or(""), cursor.version});
    }
  }
  for (const SpacePullBatch::Tombstone& item : work) {
    if (stopped_) {
      return;
    }
    SpaceCursor cursor = table->cursors[item.uuid];
    if (cursor.deleted_at_ms) {
      cursor.pending_tombstone = false;
      table->cursors[item.uuid] = cursor;
      continue;
    }
    if (const std::optional<std::string> local = access.LocalSpaceId(item.uuid)) {
      if (access.IsImporting(*local)) {
        cursor.pending_tombstone = true;
        table->cursors[item.uuid] = cursor;
        continue;
      }
      // Never hide the last live user Space for the default identity — nor
      // one the Space list cannot say is not the last.
      if (item.uuid == kDefaultSpaceUuid) {
        const std::optional<std::set<std::string>> live =
            LiveLocalUserSpaceIds(*table);
        if (!live || *live == std::set<std::string>{*local}) {
          cursor.pending_tombstone = true;
          table->cursors[item.uuid] = cursor;
          continue;
        }
      }
      if (!access.Hide(*local)) {
        cursor.pending_tombstone = true;
        table->cursors[item.uuid] = cursor;
        continue;
      }
    }
    cursor.hidden = true;
    cursor.deleted_at_ms = deps_.now();
    cursor.pending_tombstone = false;
    cursor.pending_delete = false;
    cursor.delete_reject_rounds = 0;
    cursor.pending_apply.reset();
    cursor.pending_projection.reset();
    cursor.held_profile_uuid.reset();
    cursor.held_for_local_profile_id.reset();
    if (!item.entity_id.empty()) {
      cursor.entity_id = item.entity_id;
    }
    cursor.version = std::max(cursor.version, item.version);
    table->cursors[item.uuid] = cursor;
    ++space_counters_.applied;
    table->unreadable_tag_hashes.erase(SpaceTagHash(item.uuid));
  }
}

std::optional<std::set<std::string>> PhiSyncEngine::LiveLocalUserSpaceIds(
    const SpaceSyncTable& table) {
  SpaceLocalAccess& access = *deps_.space_access;
  std::set<std::string> gone;
  for (const auto& [uuid, cursor] : table.cursors) {
    if (cursor.hidden || cursor.deleted_at_ms) {
      if (const std::optional<std::string> local = access.LocalSpaceId(uuid)) {
        gone.insert(*local);
      }
    }
  }
  const std::optional<std::vector<LocalSpace>> pairable =
      access.PairableSpaces();
  if (!pairable) {
    return std::nullopt;
  }
  std::set<std::string> live;
  for (const LocalSpace& space : *pairable) {
    if (!gone.count(space.space_id)) {
      live.insert(space.space_id);
    }
  }
  return live;
}

// Rides the cursor-save channel: the page's marker is not written, the drain
// does not complete and nothing publishes this round.
void PhiSyncEngine::NoteSpaceListUnreachable() {
  space_list_unreachable_ = true;
  ++cursor_save_failures_;
}

void PhiSyncEngine::StampLocalSpaceEdits() {
  if (stopped_ || !space_live()) {
    return;
  }
  SpaceSyncTable table = LoadSpaceTable();
  const bool any_baseline =
      std::any_of(table.cursors.begin(), table.cursors.end(),
                  [](const auto& entry) { return entry.second.reconciled.has_value(); });
  if (!any_baseline) {
    return;
  }
  const std::optional<std::vector<LocalSpace>> current =
      deps_.space_access->CurrentSpaces();
  if (!current) {
    return;  // the edits are stamped by the next local change
  }
  const std::map<std::string, SpaceEntity> projections =
      SnapshotLocal(*current, table, false);
  bool changed = false;
  for (const auto& [uuid, projection] : projections) {
    const std::optional<SpaceEntity> baseline =
        ParseSpaceBytes(table.cursors[uuid].reconciled);
    if (!baseline) {
      continue;
    }
    const std::optional<std::string> pending =
        MergeSpace(projection, *baseline) == *baseline
            ? std::nullopt
            : std::optional<std::string>(projection.Serialize());
    if (table.cursors[uuid].pending_projection != pending) {
      table.cursors[uuid].pending_projection = pending;
      changed = true;
    }
  }
  if (changed) {
    WriteSpaceTable(table);
  }
}

void PhiSyncEngine::PushSpaces(bool retry_on_conflict,
                               const std::set<std::string>* only_uuids) {
  if (stopped_ || !can_publish_this_round_ || !space_live()) {
    return;
  }
  SpaceSyncTable table = LoadSpaceTable();
  // Guard 1: nothing is published before a full replay drained — a fresh
  // device must never publish its factory Space over the account's.
  if (!table.has_drained_full_replay) {
    return;
  }
  SpaceLocalAccess& access = *deps_.space_access;
  const std::optional<std::vector<LocalSpace>> current = access.CurrentSpaces();
  if (!current) {
    if (access.SpaceStoreUnavailable()) {
      round_outbound_failed_ = true;  // nothing to publish from
    } else {
      NoteSpaceListUnreachable();
    }
    return;
  }
  const std::map<std::string, SpaceEntity> outgoing =
      SnapshotLocal(*current, table, /*ensure_mapped=*/true);

  struct Work {
    std::string uuid;
    CommitEntry entry;
    std::optional<SpaceEntity> outgoing;
  };
  std::vector<Work> work;
  std::set<std::string> uuids;
  for (const auto& [uuid, entity] : outgoing) {
    uuids.insert(uuid);
  }
  for (const auto& [uuid, cursor] : table.cursors) {
    if (cursor.pending_delete) {
      uuids.insert(uuid);
    }
  }
  for (const std::string& uuid : uuids) {
    if (only_uuids && !only_uuids->count(uuid)) {
      continue;
    }
    const std::string tag = SpaceTagHash(uuid);
    // Guard 3: never commit over a row this build could not read.
    if (table.unreadable_tag_hashes.count(tag)) {
      continue;
    }
    const auto cursor_it = table.cursors.find(uuid);
    const SpaceCursor* cursor =
        cursor_it == table.cursors.end() ? nullptr : &cursor_it->second;
    if (cursor && cursor->pending_delete) {
      if (!cursor->entity_id || cursor->version <= 0) {
        continue;
      }
      CommitEntry entry;
      entry.entity_id = cursor->entity_id;
      entry.client_tag_hash = tag;
      entry.name = kSpaceEntityName;
      entry.deleted = true;
      entry.base_version = cursor->version;
      work.push_back({uuid, entry, std::nullopt});
      continue;
    }
    const auto out = outgoing.find(uuid);
    if (out == outgoing.end()) {
      continue;
    }
    SpaceEntity to_send = out->second;
    if (cursor) {
      if (std::optional<SpaceEntity> server = ParseSpaceBytes(cursor->server)) {
        to_send = MergeSpace(out->second, *server);
        if (to_send == *server) {
          continue;  // nothing to publish
        }
      }
    }
    CommitEntry entry;
    entry.entity_id = cursor ? cursor->entity_id : std::nullopt;
    entry.client_tag_hash = tag;
    entry.name = kSpaceEntityName;
    entry.base_version = cursor ? cursor->version : 0;
    work.push_back({uuid, entry, to_send});
  }
  // A local deletion of a never-published Space needs no tombstone.
  for (auto& [uuid, cursor] : table.cursors) {
    if (!cursor.pending_delete || (cursor.entity_id && cursor.version > 0)) {
      continue;
    }
    cursor.pending_delete = false;
    cursor.reconciled.reset();
    cursor.server.reset();
    cursor.pending_projection.reset();
    cursor.deleted_at_ms = deps_.now();
  }
  if (work.empty()) {
    WriteSpaceTable(table);
    return;
  }

  keys::Bytes key;
  const bool have_key = deps_.domain_keys->DomainKey(&key);
  std::set<std::string> conflicted;
  std::set<std::string> tombstoned;
  for (size_t start = 0; start < work.size() && have_key; start += 25) {
    const size_t end = std::min(work.size(), start + 25);
    std::vector<CommitEntry> entries;
    bool sealed_all = true;
    for (size_t i = start; i < end; ++i) {
      CommitEntry entry = work[i].entry;
      if (work[i].outgoing) {
        PhiEntity wrapper;
        wrapper.kind = PhiEntity::Kind::kSpace;
        wrapper.space = *work[i].outgoing;
        entry.ciphertext = Encrypt(wrapper, key);
        if (!entry.ciphertext) {
          sealed_all = false;
          break;
        }
      }
      entries.push_back(std::move(entry));
    }
    if (!sealed_all) {
      round_outbound_failed_ = true;
      break;
    }
    if (stopped_ || !can_publish_this_round_) {
      break;
    }
    std::vector<CommitOutcome> outcomes;
    const ProtocolStatus status =
        deps_.client->Commit(entries, marker_.store_birthday, &outcomes);
    if (status.error == ProtocolError::kNotMyBirthday) {
      RequireReconfiguration();
      return;
    }
    if (!status.ok()) {
      NoteStatusError(status);
      break;
    }
    ObserveServerDate();
    if (stopped_) {
      return;
    }
    for (size_t i = 0; i < outcomes.size() && start + i < end; ++i) {
      const Work& item = work[start + i];
      const CommitOutcome& outcome = outcomes[i];
      SpaceCursor& cursor = table.cursors[item.uuid];
      const bool is_tombstone = item.entry.deleted;
      switch (outcome.kind) {
        case CommitOutcome::Kind::kApplied:
          if (!outcome.entity_id.empty()) {
            cursor.entity_id = outcome.entity_id;
          }
          cursor.version = outcome.version;
          set_stored_birthday(outcome.store_birthday);
          if (is_tombstone) {
            cursor.pending_delete = false;
            cursor.delete_reject_rounds = 0;
            cursor.reconciled.reset();
            cursor.server.reset();
            cursor.pending_projection.reset();
            cursor.deleted_at_ms = deps_.now();
            cursor.hidden = true;
            ++space_counters_.tombstones;
            tombstoned.insert(item.uuid);
          } else if (item.outgoing) {
            cursor.reconciled = item.outgoing->Serialize();
            cursor.server = cursor.reconciled;
            cursor.pending_projection.reset();
            cursor.delete_reject_rounds = 0;
            ++space_counters_.pushed;
          }
          break;
        case CommitOutcome::Kind::kConflict:
          conflicted.insert(item.uuid);
          ++space_counters_.conflicts;
          break;
        case CommitOutcome::Kind::kInvalidMessage:
          round_outbound_failed_ = true;
          if (!is_tombstone) {
            // The row it named is gone: the next push creates.
            cursor.entity_id.reset();
            cursor.version = 0;
            cursor.server.reset();
            break;
          }
          if (++cursor.delete_reject_rounds >= 3) {
            cursor.pending_delete = false;
            cursor.reconciled.reset();
            cursor.server.reset();
            cursor.pending_projection.reset();
            cursor.deleted_at_ms = deps_.now();
            cursor.hidden = true;
          }
          break;
        case CommitOutcome::Kind::kRejected:
          round_outbound_failed_ = true;
          break;
      }
    }
  }
  for (const std::string& uuid : tombstoned) {
    if (const std::optional<std::string> local = access.LocalSpaceId(uuid)) {
      access.DropSpaceMapping(*local);
    }
  }
  WriteSpaceTable(table);
  if (!conflicted.empty()) {
    if (!retry_on_conflict) {
      round_outbound_failed_ = true;
      return;
    }
    if (!Pull(/*then_push=*/false)) {
      return;
    }
    PushSpaces(/*retry_on_conflict=*/false, &conflicted);
  }
}

void PhiSyncEngine::ApplyRetentionSweep() {
  if (!deps_.space_access || !deps_.space_store) {
    return;
  }
  SpaceSyncTable table = LoadSpaceTable();
  const std::vector<std::string> expired = table.PurgeExpired(deps_.now());
  if (!expired.empty()) {
    WriteSpaceTable(table);
  }
  for (const std::string& uuid : expired) {
    if (stopped_) {
      return;
    }
    const std::optional<std::string> local =
        deps_.space_access->LocalSpaceId(uuid);
    // A failed purge keeps the mapping, so the row cannot be republished.
    if (local && deps_.space_access->Purge(*local)) {
      deps_.space_access->DropSpaceMapping(*local);
    }
  }
  DropExpiredOwnedTombstones();
  ApplyOwnedRetentionCascade();
  PurgeExpiredSoftDeletedOwnedRows();
}

void PhiSyncEngine::PurgeExpiredSoftDeletedOwnedRows() {
  const int64_t cutoff = deps_.now() - kSpaceRetentionMs;
  for (OwnedKindAdapter* kind : deps_.owned_kinds) {
    if (stopped_) {
      return;
    }
    kind->PurgeSoftDeletedRows(cutoff);
  }
}

void PhiSyncEngine::ObserveStamps(const SpaceEntity& entity) {
  const int64_t before = hlc_.max_seen();
  for (const OptionalValue* value :
       {&entity.name, &entity.icon_name, &entity.color_hex, &entity.rank,
        &entity.profile_uuid, &entity.theme_id, &entity.overlay_opacity_light,
        &entity.overlay_opacity_dark}) {
    hlc_.Observe(value->get().updated_at_ms);
  }
  if (hlc_.max_seen() != before) {
    PersistHlcMax();
  }
}

// --- The owned kinds ---------------------------------------------------------------

namespace {

// Ten batches of 25 per kind per round (§5.3).
constexpr int kMaxOwnedCommitsPerRound = 250;
constexpr size_t kMaxCommitEntriesPerBatch = 25;
constexpr int kRekeyRejectGiveUpRounds = 3;
constexpr int kTombstoneRejectGiveUpRounds = 3;

}  // namespace

OwnedSnapshotBytes::OwnedSnapshotBytes() = default;
OwnedSnapshotBytes::OwnedSnapshotBytes(const OwnedSnapshotBytes&) = default;
OwnedSnapshotBytes& OwnedSnapshotBytes::operator=(const OwnedSnapshotBytes&) =
    default;
OwnedSnapshotBytes::~OwnedSnapshotBytes() = default;
OwnedPlanInput::OwnedPlanInput() = default;
OwnedPlanInput::OwnedPlanInput(const OwnedPlanInput&) = default;
OwnedPlanInput::~OwnedPlanInput() = default;
OwnedPlanOutput::OwnedPlanOutput() = default;
OwnedPlanOutput::OwnedPlanOutput(const OwnedPlanOutput&) = default;
OwnedPlanOutput::OwnedPlanOutput(OwnedPlanOutput&&) = default;
OwnedPlanOutput& OwnedPlanOutput::operator=(OwnedPlanOutput&&) = default;
OwnedPlanOutput::~OwnedPlanOutput() = default;
OwnedLandingInput::OwnedLandingInput() = default;
OwnedLandingInput::OwnedLandingInput(const OwnedLandingInput&) = default;
OwnedLandingInput::~OwnedLandingInput() = default;
OwnedLandingOutcome::OwnedLandingOutcome() = default;
OwnedLandingOutcome::OwnedLandingOutcome(const OwnedLandingOutcome&) = default;
OwnedLandingOutcome::OwnedLandingOutcome(OwnedLandingOutcome&&) = default;
OwnedLandingOutcome& OwnedLandingOutcome::operator=(OwnedLandingOutcome&&) =
    default;
OwnedLandingOutcome::~OwnedLandingOutcome() = default;

bool OwnedKindAdapter::CommitFenceHolds() {
  return true;
}

bool OwnedKindAdapter::lands_empty_batch() const {
  return false;
}

bool OwnedKindAdapter::clears_split_partners() const {
  return false;
}

void OwnedKindAdapter::NotePublishApplied(const std::set<std::string>&) {}

void OwnedKindAdapter::ClearPendingLocalEdits(const std::set<std::string>&) {}

void OwnedKindAdapter::HardDeleteAfterTombstone(const std::set<std::string>&) {}

int OwnedKindAdapter::PurgeSoftDeletedRows(int64_t) {
  return 0;
}

std::map<std::string, std::string> OwnedKindAdapter::ClearedSplitPartners(
    const std::map<std::string, std::string>&,
    int64_t,
    const OwnedOwnerMaps&) {
  return {};
}

bool& PhiSyncEngine::HadRecordsFlag(SpaceSyncTable& table, OwnedKindId id) {
  switch (id) {
    case OwnedKindId::kBookmarks:
      return table.bookmarks_had_records;
    case OwnedKindId::kPins:
      return table.pins_had_records;
    case OwnedKindId::kUrlRules:
      return table.url_rules_had_records;
  }
  return table.url_rules_had_records;
}

bool& PhiSyncEngine::ReplayedForEmptyTableFlag(SpaceSyncTable& table,
                                                OwnedKindId id) {
  switch (id) {
    case OwnedKindId::kBookmarks:
      return table.bookmarks_replayed_for_empty_table;
    case OwnedKindId::kPins:
      return table.pins_replayed_for_empty_table;
    case OwnedKindId::kUrlRules:
      return table.url_rules_replayed_for_empty_table;
  }
  return table.url_rules_replayed_for_empty_table;
}

// The round's identity maps, computed once (and again after each page's
// Spaces landed).
const OwnedOwnerMaps& PhiSyncEngine::OwnedRoundMaps() {
  if (owned_maps_this_round_) {
    return *owned_maps_this_round_;
  }
  OwnedOwnerMaps maps;
  if (deps_.space_access) {
    SpaceLocalAccess& access = *deps_.space_access;
    const SpaceSyncTable table = LoadSpaceTable();
    for (const auto& [local_id, uuid] : access.AllSpaceMappings()) {
      maps.sync_uuid_by_space_id[local_id] = uuid;
      maps.local_space_id_by_sync_uuid[uuid] = local_id;
    }
    // The default Space's constant identity is no stored mapping.
    maps.sync_uuid_by_space_id[kDefaultSpaceUuid] = kDefaultSpaceUuid;
    maps.local_space_id_by_sync_uuid[kDefaultSpaceUuid] = kDefaultSpaceUuid;
    const std::optional<std::vector<LocalSpace>> current =
        access.CurrentSpaces();
    if (!current) {
      // Without the Space list every owner reads as gone: the kinds park
      // arrivals and publish nothing this round (BeginOwnedRound's path).
      for (OwnedKindAdapter* kind : deps_.owned_kinds) {
        if (owned_read_failed_.insert(kind->label()).second) {
          ++owned_counters_[kind->label()].local_read_failed;
        }
      }
    }
    for (const LocalSpace& space : current.value_or(std::vector<LocalSpace>())) {
      maps.local_profile_id_by_space_id[space.space_id] = space.profile_id;
      const auto uuid = maps.sync_uuid_by_space_id.find(space.space_id);
      if (uuid != maps.sync_uuid_by_space_id.end()) {
        const auto cursor = table.cursors.find(uuid->second);
        if (cursor == table.cursors.end() ||
            (!cursor->second.hidden && !cursor->second.purged_at_ms)) {
          maps.eligible_space_uuids.insert(uuid->second);
        }
      }
      if (!maps.global_uuid_by_profile_id.count(space.profile_id)) {
        if (std::optional<std::string> global =
                access.GlobalUuid(space.profile_id)) {
          maps.global_uuid_by_profile_id[space.profile_id] = *global;
          maps.local_profile_id_by_global_uuid[*global] = space.profile_id;
        }
      }
    }
  }
  owned_maps_this_round_ = std::move(maps);
  return *owned_maps_this_round_;
}

// Each kind once per round: one local read, one table load, the tag index.
// A failed read disables snapshot, diff, publication and landing — never
// "no rows", which would tombstone every published identity.
void PhiSyncEngine::BeginOwnedRound() {
  if (deps_.owned_kinds.empty() || !deps_.space_store) {
    return;
  }
  for (OwnedKindAdapter* kind : deps_.owned_kinds) {
    const std::string label = kind->label();
    if (!owned_round_started_.insert(label).second) {
      continue;
    }
    bool lost = false;
    bool reported_loss = false;
    const OwnedTable table = LoadOwnedTable(*kind, /*arms_replay_on_loss=*/false,
                                            false, &lost, &reported_loss);
    if (reported_loss) {
      owned_loss_observed_at_entry_.insert(label);
    }
    std::set<std::string> identities;
    if (kind->BeginRound()) {
      identities = kind->LocalIdentities();
    } else {
      owned_read_failed_.insert(label);
      ++owned_counters_[label].local_read_failed;
    }
    std::map<std::string, std::string>& index = owned_tag_indices_[label];
    for (const auto& [identity, cursor] : table.cursors) {
      identities.insert(identity);
    }
    for (const std::string& identity : identities) {
      if (!identity.empty()) {
        index[ClientTagHash(kind->ClientTag(identity))] = identity;
      }
    }
  }
}

// Loads a kind's table. Only publication arms the one-shot replay on a loss
// (marker nil FIRST, then the kind's latch and the drain flags).
OwnedTable PhiSyncEngine::LoadOwnedTable(OwnedKindAdapter& kind,
                                         bool arms_replay_on_loss,
                                         bool loss_observed_earlier,
                                         bool* lost,
                                         bool* reported_loss) {
  *lost = false;
  SpaceSyncTable space_table = LoadSpaceTable();
  bool store_loss = false;
  OwnedTable table = kind.store().Load(
      HadRecordsFlag(space_table, kind.kind_id()), &store_loss);
  *reported_loss = store_loss || loss_observed_earlier;
  owned_tables_[kind.label()] = table;
  if (!*reported_loss) {
    // The per-kind latch re-arms after a load that returns published cursors.
    const bool has_published = std::any_of(
        table.cursors.begin(), table.cursors.end(),
        [](const auto& entry) { return !entry.second.entity_id.empty(); });
    if (has_published &&
        ReplayedForEmptyTableFlag(space_table, kind.kind_id())) {
      const OwnedKindId id = kind.kind_id();
      MutateSpaceTable([this, id](SpaceSyncTable& t) {
        ReplayedForEmptyTableFlag(t, id) = false;
      });
    }
    return table;
  }
  if (!arms_replay_on_loss) {
    return table;
  }
  if (ReplayedForEmptyTableFlag(space_table, kind.kind_id())) {
    *lost = true;
    return table;
  }
  if (!PersistStoredMarker(std::nullopt)) {
    round_outcome_ = RoundOutcome::kCursorSaveFailed;
    return table;
  }
  const OwnedKindId id = kind.kind_id();
  if (!MutateSpaceTable([this, id](SpaceSyncTable& t) {
        ReplayedForEmptyTableFlag(t, id) = true;
        t.has_drained_full_replay = false;
        t.drain_in_progress = true;
      })) {
    round_outcome_ = RoundOutcome::kCursorSaveFailed;
    return table;
  }
  // The owned cursor table was lost: the type replays once.
  *lost = true;
  return table;
}

// Persists a kind's table; the first published cursor sets had-records.
bool PhiSyncEngine::WriteOwnedTable(OwnedKindAdapter& kind,
                                    const OwnedTable& table) {
  if (stopped_) {
    return true;
  }
  owned_tables_[kind.label()] = table;
  if (!kind.store().Save(table)) {
    ++cursor_save_failures_;
    return false;
  }
  const bool has_published = std::any_of(
      table.cursors.begin(), table.cursors.end(),
      [](const auto& entry) { return !entry.second.entity_id.empty(); });
  if (has_published) {
    const OwnedKindId id = kind.kind_id();
    MutateSpaceTable(
        [this, id](SpaceSyncTable& t) { HadRecordsFlag(t, id) = true; });
  }
  return true;
}

bool PhiSyncEngine::RouteOwnedEntity(
    const RemoteEntity& entity,
    const keys::Bytes& key,
    std::map<std::string, OwnedPullBatch>* batches) {
  if (deps_.owned_kinds.empty()) {
    return false;
  }
  // By tag first: a tombstone carries no ciphertext.
  OwnedKindAdapter* kind = nullptr;
  for (OwnedKindAdapter* candidate : deps_.owned_kinds) {
    const auto index = owned_tag_indices_.find(candidate->label());
    if (index != owned_tag_indices_.end() &&
        index->second.count(entity.client_tag_hash)) {
      kind = candidate;
      break;
    }
  }
  std::optional<PhiEntity> decoded;
  if (!kind) {
    // An unknown create can still be decoded and matched to a kind. A
    // tombstone, unreadable bytes or another kind go back to Space routing.
    if (entity.deleted) {
      return false;
    }
    PhiEntity envelope;
    if (!Decrypt(entity.ciphertext, key, &envelope)) {
      return false;
    }
    for (OwnedKindAdapter* candidate : deps_.owned_kinds) {
      if (candidate->Identity(envelope)) {
        kind = candidate;
        break;
      }
    }
    if (!kind) {
      return false;
    }
    decoded = std::move(envelope);
  }
  OwnedPullBatch& batch = (*batches)[kind->label()];
  if (entity.deleted) {
    const std::map<std::string, std::string>& index =
        owned_tag_indices_[kind->label()];
    const auto it = index.find(entity.client_tag_hash);
    if (it != index.end()) {
      batch.tombstones.push_back(
          {it->second, entity.entity_id, entity.version});
    }
    return true;
  }
  if (!decoded) {
    PhiEntity envelope;
    if (!Decrypt(entity.ciphertext, key, &envelope)) {
      batch.unreadable_hashes.push_back(entity.client_tag_hash);
      ++batch.unreadable;
      return true;
    }
    decoded = std::move(envelope);
  }
  const std::optional<std::string> identity = kind->Identity(*decoded);
  if (!identity) {
    return true;  // not this kind's payload: ignored
  }
  // The payload must hash back to the tag it arrived under (§2.5).
  if (ClientTagHash(kind->ClientTag(*identity)) != entity.client_tag_hash) {
    batch.unreadable_hashes.push_back(entity.client_tag_hash);
    ++batch.unreadable;
    return true;
  }
  batch.arrivals.push_back(
      {*identity, decoded->Serialize(), entity.entity_id, entity.version});
  // Learned at once: a later page of this drain may carry its tombstone.
  owned_tag_indices_[kind->label()][entity.client_tag_hash] = *identity;
  return true;
}

void PhiSyncEngine::FlushOwnedObservations(
    const std::map<std::string, OwnedPullBatch>& batches) {
  std::vector<std::string> hashes;
  for (const auto& [label, batch] : batches) {
    hashes.insert(hashes.end(), batch.unreadable_hashes.begin(),
                  batch.unreadable_hashes.end());
  }
  if (hashes.empty()) {
    return;
  }
  const int64_t seen_at = deps_.now();
  MutateSpaceTable([&](SpaceSyncTable& table) {
    for (const std::string& hash : hashes) {
      table.unreadable_tag_hashes[hash] = seen_at;
    }
  });
}

// Parked arrivals retry their claim once per round, in every round type.
void PhiSyncEngine::RetryParkedOwnedClaims(OwnedKindAdapter& kind,
                                           const OwnedOwnerMaps& maps) {
  const std::string label = kind.label();
  if (stopped_ || !owned_parked_retry_done_.insert(label).second ||
      owned_read_failed_.count(label)) {
    return;
  }
  OwnedTable table = owned_tables_[label];
  std::map<std::string, ParkedOwnedItem> parked;
  for (const auto& [identity, cursor] : table.cursors) {
    if (cursor.pending_apply) {
      parked[identity] = {*cursor.pending_apply, cursor.pending_owner_uuid};
    }
  }
  if (parked.empty()) {
    return;
  }
  const OwnedParkedClaimResult result =
      kind.RetryParkedClaims(parked, maps, wall_clock_offset_ms_);
  bool changed = false;
  for (const std::string& identity : result.persisted) {
    auto it = table.cursors.find(identity);
    // Only a write-back retry (its payload IS the landed baseline) clears.
    if (it == table.cursors.end() ||
        it->second.reconciled != it->second.pending_apply) {
      continue;
    }
    it->second.pending_apply.reset();
    it->second.pending_owner_uuid.reset();
    it->second.parked_for_scope = false;
    changed = true;
  }
  if (changed) {
    WriteOwnedTable(kind, table);
  }
}

void PhiSyncEngine::HarvestTriple(OwnedCursor* cursor,
                                  const std::string& entity_id,
                                  int64_t version) {
  if (!entity_id.empty()) {
    cursor->entity_id = entity_id;
    cursor->rekey_reject_rounds.reset();
  }
  cursor->version = std::max(cursor->version, version);
}

// Inbound landing: apply before the baselines move (§4.5).
void PhiSyncEngine::ApplyOwnedKind(OwnedKindAdapter& kind,
                                   const OwnedPullBatch& batch,
                                   const OwnedOwnerMaps& maps) {
  if (stopped_) {
    return;
  }
  const std::string label = kind.label();
  OwnedRoundCounters& counters = owned_counters_[label];
  counters.pulled +=
      static_cast<int>(batch.arrivals.size() + batch.tombstones.size());
  counters.unreadable += batch.unreadable;
  counters.tombstones += static_cast<int>(batch.tombstones.size());
  OwnedTable table = owned_tables_[label];

  if (owned_read_failed_.count(label)) {
    // Park what the marker already consumed in the cursor channels.
    for (const OwnedPullBatch::Tombstone& item : batch.tombstones) {
      OwnedCursor& cursor = table.cursors[item.identity];
      HarvestTriple(&cursor, item.entity_id, item.version);
      cursor.pending_tombstone = true;
    }
    for (const OwnedPullBatch::Arrival& item : batch.arrivals) {
      OwnedCursor& cursor = table.cursors[item.identity];
      const int64_t known = cursor.version;
      HarvestTriple(&cursor, item.entity_id, item.version);
      if (!cursor.deleted_at_ms || item.version > known) {
        cursor.pending_apply = item.payload;
        const std::vector<std::string> owners = kind.Owners(item.payload);
        cursor.pending_owner_uuid =
            owners.empty() ? std::nullopt : std::optional<std::string>(owners[0]);
        cursor.parked_for_scope = false;
      }
    }
    if (!batch.arrivals.empty() || !batch.tombstones.empty()) {
      WriteOwnedTable(kind, table);
    }
    return;
  }

  std::set<std::string> tombstoned;
  for (const OwnedPullBatch::Tombstone& item : batch.tombstones) {
    tombstoned.insert(item.identity);
  }
  for (const auto& [identity, cursor] : table.cursors) {
    if (cursor.pending_tombstone) {
      tombstoned.insert(identity);
    }
  }
  std::map<std::string, OwnedHarvest> tombstone_triples;
  for (const OwnedPullBatch::Tombstone& item : batch.tombstones) {
    OwnedHarvest& triple = tombstone_triples[item.identity];
    if (!item.entity_id.empty()) {
      triple.entity_id = item.entity_id;
    }
    triple.version = std::max(triple.version, item.version);
    auto cursor = table.cursors.find(item.identity);
    if (cursor != table.cursors.end()) {
      HarvestTriple(&cursor->second, item.entity_id, item.version);
    }
  }
  // §5.6 L2: a replay older than this device's tombstone is not a
  // resurrection.
  std::vector<OwnedPullBatch::Arrival> arrivals;
  std::set<std::string> resurrecting;
  int replayed_after_delete = 0;
  for (const OwnedPullBatch::Arrival& item : batch.arrivals) {
    auto cursor = table.cursors.find(item.identity);
    if (cursor == table.cursors.end() || !cursor->second.deleted_at_ms) {
      arrivals.push_back(item);
      continue;
    }
    const int64_t known = cursor->second.version;
    HarvestTriple(&cursor->second, item.entity_id, item.version);
    if (item.version <= known) {
      ++replayed_after_delete;
      continue;
    }
    resurrecting.insert(item.identity);
    arrivals.push_back(item);
  }
  counters.superseded_by_delete += replayed_after_delete;
  std::map<std::string, ParkedOwnedItem> parked;
  for (const auto& [identity, cursor] : table.cursors) {
    if (cursor.pending_apply) {
      parked[identity] = {*cursor.pending_apply, cursor.pending_owner_uuid};
    }
  }
  if (arrivals.empty() && tombstoned.empty() && parked.empty() &&
      !kind.lands_empty_batch()) {
    if (replayed_after_delete > 0) {
      WriteOwnedTable(kind, table);
    }
    return;
  }

  for (const OwnedPullBatch::Arrival& item : arrivals) {
    PhiEntity envelope;
    if (envelope.ParseFrom(item.payload)) {
      ObserveStamps(envelope);
    }
  }
  for (const auto& [identity, item] : parked) {
    PhiEntity envelope;
    if (envelope.ParseFrom(item.payload)) {
      ObserveStamps(envelope);
    }
  }
  OwnedPlanInput input;
  for (const OwnedPullBatch::Arrival& item : arrivals) {
    input.arrivals.push_back({item.payload, item.entity_id, item.version});
  }
  input.parked = parked;
  input.table = table;
  input.maps = maps;
  input.tombstoned = tombstoned;
  input.hlc_max = hlc_.max_seen();
  input.now = HlcNow();
  input.wall_offset_ms = wall_clock_offset_ms_;
  const OwnedPlanOutput output = kind.Plan(input);
  counters.adopted += output.adopted;
  counters.refused += output.plan.refused;
  counters.cycles_broken += output.plan.cycles_broken;
  if (output.plan.cycle_stamp_ms) {
    hlc_.Observe(output.plan.cycle_stamp_ms);
    PersistHlcMax();
  }
  counters.superseded_by_delete += output.plan.superseded_by_delete;
  owned_must_republish_[label] = output.must_republish;
  owned_deferred_owners_[label].insert(output.deferred_owners.begin(),
                                       output.deferred_owners.end());

  for (const auto& [identity, harvested] : output.plan.harvest) {
    auto cursor = table.cursors.find(identity);
    if (cursor != table.cursors.end()) {
      HarvestTriple(&cursor->second, harvested.entity_id, harvested.version);
    }
  }
  auto harvest_server_triple = [&](OwnedCursor* cursor,
                                   const std::string& identity) {
    auto it = output.plan.harvest.find(identity);
    if (it != output.plan.harvest.end()) {
      HarvestTriple(cursor, it->second.entity_id, it->second.version);
      return;
    }
    auto tomb = tombstone_triples.find(identity);
    if (tomb != tombstone_triples.end()) {
      HarvestTriple(cursor, tomb->second.entity_id, tomb->second.version);
    }
  };

  OwnedLandingInput landing;
  landing.steps = output.plan.steps;
  landing.table = table;
  landing.maps = maps;
  landing.claimed_local_ids = output.claimed_local_ids;
  landing.rebaselined = output.plan.rebaselined;
  landing.yielded_tombstones = output.plan.yielded_tombstones;
  landing.pre_landing_signatures = output.plan.pre_landing_signatures;
  landing.at_rest_identities = output.at_rest_identities;
  landing.converge_allowed = OwnedItemsPublishAllowed();
  const OwnedLandingOutcome outcome = kind.Land(landing);
  if (stopped_) {
    return;
  }
  counters.collapsed += outcome.collapsed;
  counters.transferred += outcome.transferred;
  counters.superseded_by_delete += outcome.superseded_by_delete;

  SpaceSyncTable space_table = LoadSpaceTable();
  bool space_table_changed = false;
  for (const std::string& identity : outcome.landed) {
    OwnedCursor& cursor = table.cursors[identity];
    harvest_server_triple(&cursor, identity);
    if (outcome.deleted.count(identity)) {
      cursor.reconciled.reset();
      cursor.server.reset();
      cursor.deleted_at_ms = deps_.now();
      cursor.pending_delete = false;
    } else {
      if (const auto it = outcome.reconciled.find(identity);
          it != outcome.reconciled.end()) {
        cursor.reconciled = it->second;
      }
      if (const auto it = output.server_bytes.find(identity);
          it != output.server_bytes.end()) {
        cursor.server = it->second;
      }
      if (resurrecting.count(identity) && cursor.deleted_at_ms) {
        cursor.deleted_at_ms.reset();
        ++counters.resurrected;
      }
    }
    cursor.pending_apply.reset();
    cursor.pending_owner_uuid.reset();
    cursor.parked_for_scope = false;
    cursor.pending_tombstone = false;
    if (const auto it = outcome.pending_partner_lineages.find(identity);
        it != outcome.pending_partner_lineages.end()) {
      cursor.pending_partner_lineage =
          it->second.empty() ? std::nullopt
                             : std::optional<std::string>(it->second);
    }
    if (space_table.unreadable_tag_hashes.erase(
            ClientTagHash(kind.ClientTag(identity)))) {
      space_table_changed = true;
    }
    ++counters.applied;
  }
  for (const auto& [identity, retired] : output.retired_identities) {
    if (outcome.landed.count(identity) && retired != identity) {
      table.RemoveCursor(retired);
    }
  }
  for (const auto& [identity, item] : output.plan.parked) {
    OwnedCursor& cursor = table.cursors[identity];
    harvest_server_triple(&cursor, identity);
    cursor.pending_apply = item.payload;
    cursor.pending_owner_uuid = item.pending_owner_uuid;
    cursor.parked_for_scope = false;
  }
  for (const std::string& identity : output.plan.parked_tombstones) {
    OwnedCursor& cursor = table.cursors[identity];
    harvest_server_triple(&cursor, identity);
    cursor.pending_tombstone = true;
  }
  // C4 direction (i): the row stays, both baselines go, the round-end
  // republish carries it over the tombstone's version.
  for (const std::string& identity : output.plan.yielded_tombstones) {
    OwnedCursor& cursor = table.cursors[identity];
    harvest_server_triple(&cursor, identity);
    cursor.reconciled.reset();
    cursor.server.reset();
    cursor.deleted_at_ms = deps_.now();
    cursor.pending_tombstone = false;
    cursor.pending_apply.reset();
    cursor.pending_owner_uuid.reset();
    cursor.parked_for_scope = false;
    cursor.pending_delete = false;
  }
  for (const std::string& identity : outcome.deferred_tombstones) {
    OwnedCursor& cursor = table.cursors[identity];
    harvest_server_triple(&cursor, identity);
    cursor.pending_tombstone = true;
  }
  for (const std::string& identity : outcome.parked) {
    OwnedCursor& cursor = table.cursors[identity];
    harvest_server_triple(&cursor, identity);
    for (const OwnedApplyStep& step : output.plan.steps) {
      if (step.identity == identity && step.payload) {
        cursor.pending_apply = step.payload;
        break;
      }
    }
    // Scope-mismatch parking is a dormant copy; any other parking reason is
    // held work.
    cursor.parked_for_scope = outcome.parked_for_scope.count(identity) > 0;
    if (tombstoned.count(identity)) {
      cursor.pending_tombstone = true;
    }
  }
  // Every identity this round attempted to land was decided under the
  // current scope. Unless this round parked it for another pinned-tab scope
  // again, it is no longer a dormant copy: a refused attempt keeps its
  // earlier pending_apply and must count as held.
  for (const OwnedApplyStep& step : output.plan.steps) {
    if (outcome.parked_for_scope.count(step.identity)) {
      continue;
    }
    auto cursor = table.cursors.find(step.identity);
    if (cursor != table.cursors.end()) {
      cursor->second.parked_for_scope = false;
    }
  }
  int dormant = 0;
  for (const auto& [identity, cursor] : table.cursors) {
    dormant += cursor.pending_apply && cursor.parked_for_scope;
  }
  counters.parked_for_scope = dormant;
  counters.refused += static_cast<int>(outcome.refused.size());
  for (const std::string& identity : output.plan.cancelled_deletes) {
    auto cursor = table.cursors.find(identity);
    if (cursor != table.cursors.end()) {
      cursor->second.pending_delete = false;
      cursor->second.delete_decided_at_ms = 0;
    }
  }
  for (const auto& [identity, waiting] : outcome.pending_partner_lineages) {
    if (outcome.landed.count(identity) || outcome.parked.count(identity) ||
        outcome.refused.count(identity)) {
      continue;
    }
    auto cursor = table.cursors.find(identity);
    if (cursor != table.cursors.end()) {
      cursor->second.pending_partner_lineage =
          waiting.empty() ? std::nullopt : std::optional<std::string>(waiting);
    }
  }
  for (const auto& [identity, bytes] : output.plan.rebaselined) {
    auto cursor = table.cursors.find(identity);
    if (cursor == table.cursors.end() || !cursor->second.reconciled ||
        cursor->second.pending_apply || cursor->second.pending_tombstone ||
        outcome.landed.count(identity) || outcome.parked.count(identity) ||
        outcome.refused.count(identity)) {
      continue;
    }
    cursor->second.reconciled = bytes;
    if (const auto it = output.server_bytes.find(identity);
        it != output.server_bytes.end()) {
      cursor->second.server = it->second;
    }
  }
  for (const std::string& identity : output.must_republish) {
    if (outcome.landed.count(identity)) {
      continue;
    }
    auto cursor = table.cursors.find(identity);
    const auto server = output.server_bytes.find(identity);
    if (cursor == table.cursors.end() || !cursor->second.reconciled ||
        cursor->second.pending_apply || cursor->second.pending_tombstone ||
        outcome.parked.count(identity) || outcome.refused.count(identity) ||
        server == output.server_bytes.end() ||
        cursor->second.server == server->second) {
      continue;
    }
    cursor->second.server = server->second;
  }
  if (space_table_changed) {
    WriteSpaceTable(space_table);
  }
  WriteOwnedTable(kind, table);
}

bool PhiSyncEngine::OwnedItemsPublishAllowed() {
  return space_live() && !deps_.owned_kinds.empty() &&
         LoadSpaceTable().has_drained_full_replay;
}

void PhiSyncEngine::PushOwnedItems(bool retry_on_conflict) {
  if (stopped_ || !can_publish_this_round_ || deps_.owned_kinds.empty() ||
      !space_live()) {
    return;
  }
  BeginOwnedRound();
  const OwnedOwnerMaps maps = OwnedRoundMaps();
  for (OwnedKindAdapter* kind : deps_.owned_kinds) {
    RetryParkedOwnedClaims(*kind, maps);
  }
  for (OwnedKindAdapter* kind : deps_.owned_kinds) {
    PublishOwnedKind(*kind, maps, retry_on_conflict, nullptr);
  }
}

void PhiSyncEngine::PublishOwnedKind(
    OwnedKindAdapter& kind,
    const OwnedOwnerMaps& maps,
    bool retry_on_conflict,
    const std::set<std::string>* only_identities) {
  const std::string label = kind.label();
  if (stopped_ || !can_publish_this_round_ || owned_read_failed_.count(label) ||
      !OwnedItemsPublishAllowed()) {
    return;
  }
  const int failures_before_load = cursor_save_failures_;
  bool lost = false;
  bool reported_loss = false;
  OwnedTable table = LoadOwnedTable(kind, /*arms_replay_on_loss=*/true,
                                    owned_loss_observed_at_entry_.count(label),
                                    &lost, &reported_loss);
  if (lost || cursor_save_failures_ != failures_before_load) {
    return;
  }
  OwnedRoundCounters& counters = owned_counters_[label];

  const int64_t round_hlc_max = hlc_.max_seen();
  const int64_t round_now = HlcNow();
  // §7.4: a baseline whose split link was cleared here reads as unlinked.
  OwnedTable doctored = table;
  if (kind.clears_split_partners()) {
    std::map<std::string, std::string> candidates;
    for (const auto& [identity, cursor] : table.cursors) {
      if (!cursor.pending_partner_lineage && cursor.reconciled) {
        candidates[identity] = *cursor.reconciled;
      }
    }
    for (const auto& [identity, cleared] :
         kind.ClearedSplitPartners(candidates, round_now, maps)) {
      doctored.cursors[identity].reconciled = cleared;
    }
  }
  const OwnedSnapshotBytes snapshot = kind.Snapshot(
      doctored, maps, round_now, round_hlc_max, wall_clock_offset_ms_);
  counters.excluded_unmapped_owner +=
      snapshot.skipped_unmapped_owner + snapshot.skipped_ineligible_owner;
  for (const auto& [identity, owner] : snapshot.owner_uuids) {
    auto cursor = table.cursors.find(identity);
    if (cursor != table.cursors.end() && cursor->second.owner_uuid != owner) {
      cursor->second.owner_uuid = owner;
    }
  }

  OwnedTombstoneResult diff;
  if (!kind.Tombstones(table, maps, HlcNow(), &diff)) {
    ++counters.local_read_failed;
    owned_read_failed_.insert(label);
    return;  // no cursor bytes are written
  }
  for (const auto& [identity, cursor] : diff.cursor_updates) {
    table.cursors[identity] = cursor;
  }
  // A local deletion of a never-published identity needs no tombstone.
  for (auto& [identity, cursor] : table.cursors) {
    if (!cursor.pending_delete ||
        (!cursor.entity_id.empty() && cursor.version != 0)) {
      continue;
    }
    cursor.pending_delete = false;
    cursor.reconciled.reset();
    cursor.server.reset();
    cursor.deleted_at_ms = deps_.now();
  }

  // Rule 3b: a yielded row (tombstone cursor, no baseline, live row) comes
  // back at the tombstone's version — unless its Space is hidden or purged.
  std::set<std::string> yield_withheld;
  if (kind.tombstone_yields_to_local_edits()) {
    const std::set<std::string> live = kind.LocalIdentities();
    const SpaceSyncTable space_table = LoadSpaceTable();
    const OwnerResolver resolve = maps.Resolver();
    for (auto& [identity, cursor] : table.cursors) {
      if (!cursor.deleted_at_ms || cursor.reconciled || !live.count(identity) ||
          snapshot.entities.count(identity)) {
        continue;
      }
      if (cursor.entity_id.empty() || cursor.version <= 0) {
        yield_withheld.insert(identity);
        continue;
      }
      const std::optional<std::string>& owner = cursor.owner_uuid;
      const auto space_cursor =
          owner ? space_table.cursors.find(*owner) : space_table.cursors.end();
      const bool owner_is_space = space_cursor != space_table.cursors.end() ||
                                  (owner && resolve.local_space_id(*owner));
      if (!owner_is_space) {
        continue;
      }
      if (space_cursor == space_table.cursors.end() ||
          (!space_cursor->second.hidden && !space_cursor->second.purged_at_ms)) {
        yield_withheld.insert(identity);
        continue;
      }
      // The Space is gone: the yield is revoked and the row deleted here.
      OwnedLandingInput revoke;
      revoke.steps.push_back({identity, OwnedStepKind::kDelete, std::nullopt,
                              std::nullopt, std::nullopt});
      revoke.table = table;
      revoke.maps = maps;
      const OwnedLandingOutcome revoked = kind.Land(revoke);
      yield_withheld.insert(identity);
      if (!revoked.landed.count(identity)) {
        continue;
      }
      cursor.reconciled.reset();
      cursor.server.reset();
      cursor.deleted_at_ms = deps_.now();
      cursor.pending_delete = false;
    }
  }

  int budget = kMaxOwnedCommitsPerRound;
  std::vector<std::string> delete_candidates;
  for (const auto& [identity, cursor] : table.cursors) {
    if (cursor.pending_delete && !cursor.deleted_at_ms &&
        !diff.deferred.count(identity)) {
      delete_candidates.push_back(identity);
    }
  }
  const std::vector<std::string> tombstone_slice =
      OwnedTombstoneSlice(kind, table, delete_candidates, &budget);
  std::set<std::string> unkeyed;
  std::set<std::string> pending;
  for (const auto& [identity, cursor] : table.cursors) {
    if (cursor.entity_id.empty() && cursor.reconciled && !cursor.deleted_at_ms &&
        !cursor.pending_apply && !cursor.pending_delete &&
        cursor.rekey_reject_rounds.value_or(0) < kRekeyRejectGiveUpRounds &&
        snapshot.entities.count(identity)) {
      unkeyed.insert(identity);
    }
    if (cursor.reconciled && cursor.server && *cursor.server != *cursor.reconciled &&
        !cursor.deleted_at_ms && !cursor.pending_apply &&
        !cursor.pending_delete && !cursor.pending_tombstone) {
      pending.insert(identity);
    }
  }
  std::set<std::string> republish = owned_must_republish_[label];
  republish.insert(unkeyed.begin(), unkeyed.end());
  republish.insert(pending.begin(), pending.end());
  const std::set<std::string>& deferred_owners = owned_deferred_owners_[label];
  std::vector<std::string> live_candidates;
  for (const auto& [identity, bytes] : snapshot.entities) {
    const auto cursor = table.cursors.find(identity);
    const OwnedCursor* c =
        cursor == table.cursors.end() ? nullptr : &cursor->second;
    if ((c && (c->pending_delete || c->pending_tombstone)) ||
        yield_withheld.count(identity)) {
      continue;
    }
    if (snapshot.minted.count(identity)) {
      const auto owner = snapshot.owner_uuids.find(identity);
      if (owner != snapshot.owner_uuids.end() &&
          deferred_owners.count(owner->second)) {
        continue;
      }
    }
    if (!c || !c->reconciled || bytes != *c->reconciled ||
        republish.count(identity)) {
      live_candidates.push_back(identity);
    }
  }
  // §8.4.5: a rule whose live projection equals what the account already
  // holds needs no pending-edit flag any more (the self-heal).
  if (!only_identities) {
    std::set<std::string> clear_candidates;
    for (const auto& [identity, bytes] : snapshot.entities) {
      const auto cursor = table.cursors.find(identity);
      if (cursor != table.cursors.end() && cursor->second.reconciled &&
          cursor->second.server &&
          *cursor->second.server == *cursor->second.reconciled &&
          bytes == *cursor->second.reconciled) {
        clear_candidates.insert(identity);
      }
    }
    kind.ClearPendingLocalEdits(clear_candidates);
  }
  const std::vector<std::string> live_slice =
      OwnedLiveSlice(kind, snapshot, live_candidates, &budget);
  if (!only_identities) {
    counters.pending_publish +=
        static_cast<int>(delete_candidates.size() - tombstone_slice.size()) +
        static_cast<int>(live_candidates.size() - live_slice.size());
  }

  const SpaceSyncTable space_table_now = LoadSpaceTable();
  struct Work {
    std::string identity;
    CommitEntry entry;
    std::optional<std::string> payload;
  };
  std::vector<Work> work;
  for (const std::string& identity : tombstone_slice) {
    if (only_identities && !only_identities->count(identity)) {
      continue;
    }
    const OwnedCursor& cursor = table.cursors.at(identity);
    if (cursor.entity_id.empty() || cursor.version <= 0) {
      continue;
    }
    const std::string hash = ClientTagHash(kind.ClientTag(identity));
    if (space_table_now.unreadable_tag_hashes.count(hash)) {
      continue;  // never commit over a row this build could not read
    }
    CommitEntry entry;
    entry.entity_id = cursor.entity_id;
    entry.client_tag_hash = hash;
    entry.name = kind.entity_name();
    entry.deleted = true;
    entry.base_version = cursor.version;
    work.push_back({identity, entry, std::nullopt});
  }
  for (const std::string& identity : live_slice) {
    if (only_identities && !only_identities->count(identity)) {
      continue;
    }
    const auto payload = snapshot.entities.find(identity);
    if (payload == snapshot.entities.end()) {
      continue;
    }
    const std::string hash = ClientTagHash(kind.ClientTag(identity));
    if (space_table_now.unreadable_tag_hashes.count(hash)) {
      continue;
    }
    const auto cursor = table.cursors.find(identity);
    CommitEntry entry;
    if (cursor != table.cursors.end() && !cursor->second.entity_id.empty()) {
      entry.entity_id = cursor->second.entity_id;
    }
    entry.client_tag_hash = hash;
    entry.name = kind.entity_name();
    entry.base_version =
        cursor == table.cursors.end() ? 0 : cursor->second.version;
    work.push_back({identity, entry, payload->second});
  }
  if (work.empty()) {
    WriteOwnedTable(kind, table);
    return;
  }

  keys::Bytes key;
  bool have_key = deps_.domain_keys->DomainKey(&key);
  std::set<std::string> conflicted;
  std::map<std::string, std::string> applied_minted;
  std::set<std::string> applied_live;
  std::set<std::string> applied_tombstones;
  for (size_t start = 0; start < work.size(); start += kMaxCommitEntriesPerBatch) {
    const size_t end = std::min(work.size(), start + kMaxCommitEntriesPerBatch);
    std::vector<CommitEntry> entries;
    bool sealed_all = have_key;
    for (size_t i = start; i < end && sealed_all; ++i) {
      CommitEntry entry = work[i].entry;
      if (work[i].payload) {
        PhiEntity envelope;
        if (!envelope.ParseFrom(*work[i].payload)) {
          sealed_all = false;
          break;
        }
        entry.ciphertext = Encrypt(envelope, key);
        if (!entry.ciphertext) {
          sealed_all = false;
          break;
        }
      }
      entries.push_back(std::move(entry));
    }
    if (!sealed_all) {
      round_outbound_failed_ = true;
      break;
    }
    if (stopped_ || !can_publish_this_round_) {
      break;
    }
    // The commit fence: a pin-scope migration since the sample means these
    // entries describe rows of a scope that is gone; dropped unsent, the
    // next round recomputes them from the cursors (delayed, not lost).
    if (!kind.CommitFenceHolds()) {
      break;
    }
    std::vector<CommitOutcome> outcomes;
    const ProtocolStatus status =
        deps_.client->Commit(entries, marker_.store_birthday, &outcomes);
    if (status.error == ProtocolError::kNotMyBirthday) {
      RequireReconfiguration();
      return;
    }
    if (!status.ok()) {
      NoteStatusError(status);
      break;
    }
    ObserveServerDate();
    if (stopped_) {
      return;
    }
    if (outcomes.size() != entries.size()) {
      round_outbound_failed_ = true;
    }
    for (size_t i = 0; i < outcomes.size() && start + i < end; ++i) {
      const Work& item = work[start + i];
      const CommitOutcome& outcome = outcomes[i];
      const bool existed = table.cursors.count(item.identity) > 0;
      OwnedCursor cursor =
          existed ? table.cursors[item.identity] : OwnedCursor();
      const bool is_tombstone = item.entry.deleted;
      switch (outcome.kind) {
        case CommitOutcome::Kind::kApplied:
          if (!outcome.entity_id.empty()) {
            cursor.entity_id = outcome.entity_id;
          }
          cursor.version = outcome.version;
          set_stored_birthday(outcome.store_birthday);
          if (is_tombstone) {
            cursor.pending_delete = false;
            cursor.delete_reject_rounds = 0;
            cursor.reconciled.reset();
            cursor.server.reset();
            cursor.deleted_at_ms = deps_.now();
            ++counters.tombstones;
            applied_tombstones.insert(item.identity);
          } else if (item.payload) {
            applied_live.insert(item.identity);
            cursor.reconciled = item.payload;
            cursor.server = item.payload;
            if (const auto owner = snapshot.owner_uuids.find(item.identity);
                owner != snapshot.owner_uuids.end()) {
              cursor.owner_uuid = owner->second;
            }
            if (cursor.deleted_at_ms) {
              cursor.deleted_at_ms.reset();  // rule 3b's resurrection
              ++counters.resurrected;
            }
            cursor.delete_reject_rounds = 0;
            cursor.rekey_reject_rounds.reset();
            ++counters.pushed;
            if (const auto minted = snapshot.minted.find(item.identity);
                minted != snapshot.minted.end()) {
              applied_minted[item.identity] = minted->second;
            }
          }
          table.cursors[item.identity] = cursor;
          break;
        case CommitOutcome::Kind::kConflict:
          // Never versioned but unidentified: that would send a create
          // with a nonzero base version.
          if (existed && (!outcome.entity_id.empty() || !cursor.entity_id.empty())) {
            HarvestTriple(&cursor, outcome.entity_id, outcome.version);
            table.cursors[item.identity] = cursor;
          }
          conflicted.insert(item.identity);
          break;
        case CommitOutcome::Kind::kInvalidMessage:
          round_outbound_failed_ = true;
          if (!existed) {
            break;
          }
          if (!is_tombstone) {
            cursor.entity_id.clear();
            cursor.version = 0;
            cursor.server.reset();
            if (unkeyed.count(item.identity)) {
              cursor.rekey_reject_rounds = cursor.rekey_reject_rounds.value_or(0) + 1;
            }
          } else if (++cursor.delete_reject_rounds >=
                     kTombstoneRejectGiveUpRounds) {
            cursor.pending_delete = false;
            cursor.reconciled.reset();
            cursor.server.reset();
            cursor.deleted_at_ms = deps_.now();
          }
          table.cursors[item.identity] = cursor;
          break;
        case CommitOutcome::Kind::kRejected:
          round_outbound_failed_ = true;
          break;
      }
    }
  }

  // Minted identities become local only once the account accepted them.
  if (!applied_minted.empty()) {
    const std::set<std::string> persisted = kind.ClaimIdentities(applied_minted);
    for (const auto& [identity, local_id] : applied_minted) {
      if (persisted.count(identity)) {
        continue;
      }
      OwnedCursor& cursor = table.cursors[identity];
      cursor.pending_apply = cursor.reconciled;
      cursor.parked_for_scope = false;
      if (!cursor.owner_uuid) {
        if (const auto owner = snapshot.owner_uuids.find(identity);
            owner != snapshot.owner_uuids.end()) {
          cursor.owner_uuid = owner->second;
        }
      }
    }
  }
  if (!applied_live.empty()) {
    kind.NotePublishApplied(applied_live);
  }
  const bool saved = WriteOwnedTable(kind, table);
  // A published rule tombstone lets the soft-deleted row go for good.
  if (saved && !applied_tombstones.empty()) {
    kind.HardDeleteAfterTombstone(applied_tombstones);
  }

  if (!conflicted.empty()) {
    if (!retry_on_conflict) {
      round_outbound_failed_ = true;
      return;
    }
    if (!Pull(/*then_push=*/false)) {
      return;
    }
    PublishOwnedKind(kind, maps, /*retry_on_conflict=*/false, &conflicted);
  }
}

// Parents before children; a child waits for its parent in the slice.
std::vector<std::string> PhiSyncEngine::OwnedLiveSlice(
    OwnedKindAdapter& kind,
    const OwnedSnapshotBytes& snapshot,
    const std::vector<std::string>& candidates,
    int* budget) {
  const std::set<std::string> candidate_set(candidates.begin(), candidates.end());
  std::map<std::string, std::string> parent_of;
  for (const std::string& identity : candidates) {
    const auto bytes = snapshot.entities.find(identity);
    if (bytes == snapshot.entities.end()) {
      continue;
    }
    for (const std::string& owner : kind.Owners(bytes->second)) {
      if (candidate_set.count(owner)) {
        parent_of[identity] = owner;
        break;
      }
    }
  }
  const std::map<std::string, int> depths = OwnedDepths(candidates, parent_of);
  std::vector<std::string> ordered = candidates;
  std::sort(ordered.begin(), ordered.end(),
            [&](const std::string& a, const std::string& b) {
              const int left = depths.at(a);
              const int right = depths.at(b);
              return left == right ? a < b : left < right;
            });
  std::vector<std::string> out;
  std::set<std::string> admitted;
  for (const std::string& identity : ordered) {
    if (static_cast<int>(out.size()) >= *budget) {
      break;
    }
    const auto parent = parent_of.find(identity);
    if (parent != parent_of.end() && !admitted.count(parent->second)) {
      continue;
    }
    out.push_back(identity);
    admitted.insert(identity);
  }
  *budget -= static_cast<int>(out.size());
  return out;
}

// Whole subtrees, children first; an ancestor waits while a descendant's
// own tombstone is still pending (unless the account kept rejecting it).
std::vector<std::string> PhiSyncEngine::OwnedTombstoneSlice(
    OwnedKindAdapter& kind,
    const OwnedTable& table,
    const std::vector<std::string>& candidates,
    int* budget) {
  if (candidates.empty()) {
    return {};
  }
  const std::set<std::string> candidate_set(candidates.begin(), candidates.end());
  std::map<std::string, std::string> parent_of;
  for (const std::string& identity : candidates) {
    const auto cursor = table.cursors.find(identity);
    if (cursor == table.cursors.end() || !cursor->second.reconciled) {
      continue;
    }
    for (const std::string& owner : kind.Owners(*cursor->second.reconciled)) {
      if (candidate_set.count(owner) || table.cursors.count(owner)) {
        parent_of[identity] = owner;
        break;
      }
    }
  }
  std::map<std::string, std::vector<std::string>> children_of;
  for (const auto& [child, parent] : parent_of) {
    children_of[parent].push_back(child);
  }
  std::set<std::string> blocked;
  for (const std::string& identity : candidates) {
    std::vector<std::string> stack = children_of[identity];
    while (!stack.empty()) {
      const std::string child = stack.back();
      stack.pop_back();
      for (const std::string& grandchild : children_of[child]) {
        stack.push_back(grandchild);
      }
      const auto cursor = table.cursors.find(child);
      if (cursor == table.cursors.end()) {
        continue;
      }
      if (cursor->second.delete_reject_rounds >= kTombstoneRejectGiveUpRounds) {
        continue;
      }
      blocked.insert(identity);
    }
  }
  std::map<std::string, std::string> root_of;
  for (const std::string& identity : candidates) {
    std::string cursor = identity;
    size_t hops = 0;
    for (auto it = parent_of.find(cursor);
         it != parent_of.end() && hops <= candidates.size();
         it = parent_of.find(cursor)) {
      cursor = it->second;
      ++hops;
    }
    root_of[identity] = cursor;
  }
  const std::map<std::string, int> depths = OwnedDepths(candidates, parent_of);
  std::map<std::string, std::vector<std::string>> groups;
  for (const std::string& identity : candidates) {
    groups[root_of[identity]].push_back(identity);
  }
  std::vector<std::string> out;
  for (auto& [root, members] : groups) {  // std::map: sorted roots
    std::vector<std::string> kept;
    for (const std::string& member : members) {
      if (!blocked.count(member)) {
        kept.push_back(member);
      }
    }
    std::sort(kept.begin(), kept.end(),
              [&](const std::string& a, const std::string& b) {
                const int left = depths.at(a);
                const int right = depths.at(b);
                return left == right ? a < b : left > right;
              });
    if (kept.empty()) {
      continue;
    }
    if (!out.empty() &&
        static_cast<int>(out.size() + kept.size()) > *budget) {
      continue;
    }
    out.insert(out.end(), kept.begin(), kept.end());
  }
  *budget = std::max(0, *budget - static_cast<int>(out.size()));
  return out;
}

void PhiSyncEngine::DropExpiredOwnedTombstones() {
  if (!space_live() || deps_.owned_kinds.empty()) {
    return;
  }
  BeginOwnedRound();
  const int64_t now_ms = deps_.now();
  for (OwnedKindAdapter* kind : deps_.owned_kinds) {
    if (stopped_) {
      return;
    }
    OwnedTable table = owned_tables_[kind->label()];
    const size_t before = table.cursors.size();
    table.DropExpiredTombstones(now_ms);
    if (table.cursors.size() != before) {
      WriteOwnedTable(*kind, table);
    }
  }
}

// A purged Space's items: the cursor goes unless a live row still claims it
// (then it follows the row's current owner).
void PhiSyncEngine::ApplyOwnedRetentionCascade() {
  if (!space_live() || deps_.owned_kinds.empty()) {
    return;
  }
  BeginOwnedRound();
  const SpaceSyncTable space_table = LoadSpaceTable();
  const OwnedOwnerMaps maps = OwnedRoundMaps();
  for (OwnedKindAdapter* kind : deps_.owned_kinds) {
    if (stopped_) {
      return;
    }
    if (owned_read_failed_.count(kind->label())) {
      continue;
    }
    OwnedTable table = owned_tables_[kind->label()];
    std::set<std::string> candidates;
    for (const auto& [identity, cursor] : table.cursors) {
      if (!cursor.owner_uuid) {
        continue;
      }
      const auto space = space_table.cursors.find(*cursor.owner_uuid);
      if (space != space_table.cursors.end() && space->second.purged_at_ms) {
        candidates.insert(identity);
      }
    }
    if (candidates.empty()) {
      continue;
    }
    OwnedLiveRows live;
    if (!kind->LiveOwners(candidates, maps, &live)) {
      continue;
    }
    bool changed = false;
    for (const std::string& identity : candidates) {
      OwnedCursor& cursor = table.cursors[identity];
      if (cursor.pending_apply) {
        continue;
      }
      if (!live.claimed.count(identity)) {
        table.cursors.erase(identity);
        changed = true;
        continue;
      }
      const auto owner = live.owners.find(identity);
      if (owner != live.owners.end() && cursor.owner_uuid != owner->second) {
        cursor.owner_uuid = owner->second;
        changed = true;
      }
    }
    if (changed) {
      WriteOwnedTable(*kind, table);
    }
  }
}

void PhiSyncEngine::ObserveStamps(const PhiEntity& envelope) {
  const int64_t before = hlc_.max_seen();
  auto observe = [this](const OptionalValue& value) {
    hlc_.Observe(value.get().updated_at_ms);
  };
  switch (envelope.kind) {
    case PhiEntity::Kind::kBookmark: {
      const BookmarkEntity& e = envelope.bookmark;
      for (const OptionalValue* v :
           {&e.space_uuid, &e.parent_uuid, &e.rank, &e.title, &e.url,
            &e.secondary_url, &e.secondary_title}) {
        observe(*v);
      }
      break;
    }
    case PhiEntity::Kind::kPinTab: {
      const PinTabEntity& e = envelope.pin_tab;
      for (const OptionalValue* v :
           {&e.rank, &e.title, &e.url, &e.split_partner_uuid}) {
        observe(*v);
      }
      break;
    }
    case PhiEntity::Kind::kUrlRule: {
      const UrlRuleEntity& e = envelope.url_rule;
      for (const OptionalValue* v : {&e.host, &e.path_prefix, &e.ask,
                                     &e.target_space_uuid, &e.rank}) {
        observe(*v);
      }
      break;
    }
    case PhiEntity::Kind::kSpace:
      ObserveStamps(envelope.space);
      return;
    default:
      break;
  }
  if (hlc_.max_seen() != before) {
    PersistHlcMax();
  }
}

// --- Clock -------------------------------------------------------------------------------

int64_t PhiSyncEngine::HlcNow() {
  const int64_t stamp = hlc_.Stamp(CorrectedNow());
  PersistHlcMax();
  return stamp;
}

int64_t PhiSyncEngine::CorrectedNow() const {
  return HybridClock::Corrected(deps_.now(), wall_clock_offset_ms_);
}

void PhiSyncEngine::ObserveServerDate() {
  const std::optional<int64_t> server_ms =
      deps_.client->last_server_date_ms();
  if (stopped_ || !server_ms) {
    return;
  }
  const int64_t correction =
      HybridClock::WallClockCorrection(*server_ms, deps_.now());
  if (correction == wall_clock_offset_ms_) {
    return;
  }
  wall_clock_offset_ms_ = correction;
  WriteState(kWallClockOffsetKey,
             correction == 0 ? std::nullopt
                             : std::optional<std::string>(
                                   std::to_string(correction)));
}

void PhiSyncEngine::ObserveStamps(const SettingEntity& entity) {
  const int64_t before = hlc_.max_seen();
  for (const auto& [key, value] : entity.values) {
    hlc_.Observe(value.updated_at_ms);
  }
  if (hlc_.max_seen() != before) {
    PersistHlcMax();
  }
}

void PhiSyncEngine::PersistHlcMax() {
  WriteState(kHlcMaxKey, std::to_string(hlc_.max_seen()));
}

// --- Persisted state ---------------------------------------------------------------------

bool PhiSyncEngine::PersistMarkerState(const MarkerFile& updated) {
  if (stopped_) {
    return true;  // a retired engine writes nothing; not a failure
  }
  if (updated == marker_) {
    return true;
  }
  const MarkerFile previous = marker_;
  marker_ = updated;
  if (!deps_.marker_store->Save(updated)) {
    marker_ = previous;
    ++cursor_save_failures_;
    return false;
  }
  return true;
}

bool PhiSyncEngine::PersistStoredMarker(
    const std::optional<std::string>& marker) {
  MarkerFile updated = marker_;
  updated.marker = NormalizedMarker(marker);
  return PersistMarkerState(updated);
}

void PhiSyncEngine::set_stored_birthday(const std::string& birthday) {
  MarkerFile updated = marker_;
  updated.store_birthday = birthday;
  PersistMarkerState(updated);
}

void PhiSyncEngine::WriteState(const std::string& key,
                               const std::optional<std::string>& value) {
  if (stopped_) {
    return;
  }
  if (!deps_.state->Set(key, value)) {
    ++cursor_save_failures_;  // the round stops before its marker
  }
}

std::optional<std::string> PhiSyncEngine::stored_entity_id() const {
  return deps_.state->Get(kEntityIdKey);
}

void PhiSyncEngine::set_stored_entity_id(const std::optional<std::string>& id) {
  WriteState(kEntityIdKey, id);
}

std::optional<int64_t> PhiSyncEngine::stored_version() const {
  return ParseInt64(deps_.state->Get(kVersionKey));
}

void PhiSyncEngine::set_stored_version(std::optional<int64_t> version) {
  WriteState(kVersionKey, version ? std::optional<std::string>(
                                        std::to_string(*version))
                                  : std::nullopt);
}

std::optional<SettingEntity> PhiSyncEngine::stored_last_entity() const {
  const std::optional<std::string> hex = deps_.state->Get(kLastEntityKey);
  const std::optional<std::string> bytes = hex ? Unhex(*hex) : std::nullopt;
  SettingEntity entity;
  if (!bytes || !entity.ParseFrom(*bytes)) {
    return std::nullopt;
  }
  return entity;
}

void PhiSyncEngine::set_stored_last_entity(
    const std::optional<SettingEntity>& entity) {
  WriteState(kLastEntityKey,
             entity ? std::optional<std::string>(Hex(entity->Serialize()))
                    : std::nullopt);
}

int PhiSyncEngine::tombstone_rounds() const {
  return static_cast<int>(
      ParseInt64(deps_.state->Get(kTombstoneRoundsKey)).value_or(0));
}

void PhiSyncEngine::set_tombstone_rounds(int rounds) {
  WriteState(kTombstoneRoundsKey,
             rounds > 0 ? std::optional<std::string>(std::to_string(rounds))
                        : std::nullopt);
}

void PhiSyncEngine::set_has_adopted(bool adopted) {
  WriteState(kHasAdoptedKey, adopted ? std::optional<std::string>("1")
                                     : std::nullopt);
}

std::optional<PhiSyncEngine::UnreadableRecord>
PhiSyncEngine::unreadable_record() const {
  const std::optional<std::string> text = deps_.state->Get(kUnreadableKey);
  if (!text) {
    return std::nullopt;
  }
  // "<reason>\n<fingerprint>\n<build>"
  const size_t a = text->find('\n');
  const size_t b = a == std::string::npos ? a : text->find('\n', a + 1);
  if (b == std::string::npos) {
    return std::nullopt;
  }
  return UnreadableRecord{text->substr(0, a), text->substr(a + 1, b - a - 1),
                          text->substr(b + 1)};
}

void PhiSyncEngine::set_unreadable_record(
    const std::optional<UnreadableRecord>& record) {
  WriteState(kUnreadableKey,
             record ? std::optional<std::string>(record->reason + "\n" +
                                                 record->key_fingerprint +
                                                 "\n" + record->build)
                    : std::nullopt);
}

void PhiSyncEngine::ClearEntityCursor() {
  set_stored_entity_id(std::nullopt);
  set_stored_version(std::nullopt);
  set_stored_last_entity(std::nullopt);
}

void PhiSyncEngine::ClearRemoteCursor() {
  ClearEntityCursor();
  PersistStoredMarker(std::nullopt);
  set_unreadable_record(std::nullopt);
}

}  // namespace phi_client::sync

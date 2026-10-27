// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_ENGINE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_ENGINE_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_key_crypto.h"
#include "phibrowser-win/client/phi_sync_clock.h"
#include "phibrowser-win/client/phi_sync_owned.h"
#include "phibrowser-win/client/phi_sync_proto.h"
#include "phibrowser-win/client/phi_sync_protocol.h"
#include "phibrowser-win/client/phi_sync_spaces.h"

// Phi native sync's engine (docs/user-data-parity.md S5) — phibrowser-mac
// PhiSyncEngine, ported section by section. Host-clean and SYNCHRONOUS: the
// Windows client runs every round on the account's key worker thread
// (PhiSyncKeyService), one at a time, which is the Mac's round queue.
//
// This file carries the settings kind (SyncableSettings + the engine's
// settings path: adopt vs merge, the unreadable-entity record, the tombstone
// heal, one conflict retry) and the round skeleton the Space and owned-item
// sections plug into.
namespace phi_client::sync {

// --- Settings (SyncableSettings) ----------------------------------------------------

// One synced preference: `key` is the Mac's UserDefaults key and the entity's
// map key. `read` resolves the CURRENT local value through the preference's
// own default (nullopt drops the key from the snapshot); `write` must
// silently refuse a value that does not fit the preference (a newer or
// corrupted peer) — apply notices and leaves the sidecars alone.
struct SyncableSetting {
  std::string key;
  std::function<std::optional<SettingValue>()> read;
  std::function<void(const SettingValue&)> write;
  // Retry local application of a register whose referenced rows may arrive
  // later in the round. Must not change the register or its timestamps.
  std::function<void()> reconcile = nullptr;
};

// The two sidecars per key (the Mac's `<key>.phiSyncTs` / `<key>.phiSyncVal`):
// the last local-change stamp and the last synced value's signature. Device-
// wide, not account-scoped (they outlive an account switch, as on the Mac).
class SettingsSidecars {
 public:
  virtual ~SettingsSidecars() = default;
  virtual std::optional<int64_t> Timestamp(const std::string& key) const = 0;
  virtual std::optional<std::string> Value(const std::string& key) const = 0;
  // False when it did not persist; the stored stamp and value stay as they
  // were then.
  virtual bool Set(const std::string& key, int64_t stamp,
                   const std::string& value_signature) = 0;
};

// SyncableSettings.snapshot: every registered key, a changed one stamped
// EditStamp(now, previous stamp) and its sidecars refreshed.
// `sidecars_failed` (optional) is set when a sidecar did not persist.
SettingEntity SnapshotSettings(const std::vector<SyncableSetting>& settings,
                               SettingsSidecars& sidecars,
                               int64_t now,
                               bool* sidecars_failed = nullptr);
// SyncableSettings.apply: registered keys only; the sidecars follow a value
// that actually landed. Returns the keys that landed.
std::vector<std::string> ApplySettings(const SettingEntity& entity,
                                       const std::vector<SyncableSetting>& settings,
                                       SettingsSidecars& sidecars,
                                       bool* sidecars_failed = nullptr);

// --- Persisted state --------------------------------------------------------------

// marker.json: the shared progress marker and store birthday (account dir).
struct MarkerFile {
  std::optional<std::string> marker;  // never empty: empty is stored as none
  std::string store_birthday;
  bool removal_pending = false;
  bool requires_reconfiguration = false;
  // The account reset's operation UUID (lowercase), journaled by the key
  // service before the reset request so a crash, timeout or lost response
  // retries the same operation. Set, it means an unfinished reconfiguration
  // (the service's NeedsReconfiguration). The engine never writes it on its
  // own: the service stops the engine before journaling and the engine does
  // not start again while it is set, so the mirror below cannot clobber it.
  std::optional<std::string> reset_operation_id;
  bool operator==(const MarkerFile& other) const;
};

class MarkerStore {
 public:
  virtual ~MarkerStore() = default;
  virtual MarkerFile Load() = 0;
  virtual bool Save(const MarkerFile& file) = 0;
};

// The engine's account-scoped cursor keys (the Mac's `phi.sync.*` state
// keys): entityId, version, lastEntity, tombstoneRounds, hasAdopted,
// unreadableSettings, hlcMax, wallClockOffsetMs. Values are strings; an
// absent key is nullopt.
class EngineStateStore {
 public:
  virtual ~EngineStateStore() = default;
  virtual std::optional<std::string> Get(const std::string& key) const = 0;
  // False when it did not persist; Get then still answers the old value.
  virtual bool Set(const std::string& key,
                   const std::optional<std::string>& value) = 0;
};

// The account's Phi domain key (DomainKeyManager).
class DomainKeyProvider {
 public:
  virtual ~DomainKeyProvider() = default;
  virtual bool DomainKey(keys::Bytes* out) = 0;
};

// --- Owned-item kinds (OwnedKindRegistration) ----------------------------------------

// Which of the Space table's per-kind flags a kind owns.
enum class OwnedKindId { kBookmarks, kPins, kUrlRules };

struct OwnedSnapshotBytes {
  std::map<std::string, std::string> entities;     // identity → envelope
  std::map<std::string, std::string> owner_uuids;  // identity → current owner
  // Identities minted in memory this round → local row id; persisted only
  // after the account accepted the create (§6.4).
  std::map<std::string, std::string> minted;
  int skipped_unmapped_owner = 0;
  int skipped_ineligible_owner = 0;
  bool scope_mismatch = false;

  OwnedSnapshotBytes();
  OwnedSnapshotBytes(const OwnedSnapshotBytes&);
  OwnedSnapshotBytes& operator=(const OwnedSnapshotBytes&);
  ~OwnedSnapshotBytes();
};

struct OwnedPlanInput {
  struct Arrival {
    std::string payload;
    std::string entity_id;
    int64_t version = 0;
  };
  std::vector<Arrival> arrivals;
  std::map<std::string, ParkedOwnedItem> parked;
  OwnedTable table;
  OwnedOwnerMaps maps;
  std::set<std::string> tombstoned;
  int64_t now = 0;
  int64_t hlc_max = 0;
  int64_t wall_offset_ms = 0;

  OwnedPlanInput();
  OwnedPlanInput(const OwnedPlanInput&);
  ~OwnedPlanInput();
};

struct OwnedPlanOutput {
  OwnedPlan plan;
  int adopted = 0;
  int unmatched_folders = 0;
  int unmergeable_pairs = 0;
  int normalized = 0;
  std::set<std::string> must_republish;
  std::set<std::string> deferred_owners;
  bool scope_mismatch = false;
  std::map<std::string, std::string> server_bytes;
  std::map<std::string, std::string> claimed_local_ids;
  std::map<std::string, std::string> retired_identities;
  std::set<std::string> at_rest_identities;  // rules
  int yield_no_partner = 0;                  // rules

  OwnedPlanOutput();
  OwnedPlanOutput(const OwnedPlanOutput&);
  OwnedPlanOutput(OwnedPlanOutput&&);
  OwnedPlanOutput& operator=(OwnedPlanOutput&&);
  ~OwnedPlanOutput();
};

struct OwnedLandingInput {
  std::vector<OwnedApplyStep> steps;
  OwnedTable table;
  OwnedOwnerMaps maps;
  std::map<std::string, std::string> claimed_local_ids;
  std::map<std::string, std::string> rebaselined;
  std::set<std::string> yielded_tombstones;
  // Rules' merge tail.
  std::map<std::string, RuleSignature> pre_landing_signatures;
  std::set<std::string> at_rest_identities;
  bool converge_allowed = false;

  OwnedLandingInput();
  OwnedLandingInput(const OwnedLandingInput&);
  ~OwnedLandingInput();
};

struct OwnedLandingOutcome {
  std::set<std::string> landed;
  std::set<std::string> parked;
  // Subset of parked: pins whose owner resolves locally but belongs to
  // another pinned-tab scope. Dormant copies, replanned every round but
  // excluded from the status held / pending counts.
  std::set<std::string> parked_for_scope;
  std::set<std::string> refused;
  std::map<std::string, std::string> reconciled;
  std::set<std::string> deleted;
  // identity → the partner lineage it waits for ("" = no longer waiting).
  std::map<std::string, std::string> pending_partner_lineages;
  std::set<std::string> deferred_tombstones;
  int relineaged = 0;
  int owner_moved = 0;
  int collapsed = 0;
  int transferred = 0;
  int superseded_by_delete = 0;

  OwnedLandingOutcome();
  OwnedLandingOutcome(const OwnedLandingOutcome&);
  OwnedLandingOutcome(OwnedLandingOutcome&&);
  OwnedLandingOutcome& operator=(OwnedLandingOutcome&&);
  ~OwnedLandingOutcome();
};

struct OwnedParkedClaimResult {
  std::set<std::string> paired;
  std::set<std::string> persisted;
};

struct OwnedLiveRows {
  std::set<std::string> claimed;
  std::map<std::string, std::string> owners;
};

// One owned kind as the engine drives it: its pure functions and its local
// access, bound together (the Mac's OwnedKindRegistration). Called on the
// engine's thread; the implementation marshals to the UI thread itself.
class OwnedKindAdapter {
 public:
  virtual ~OwnedKindAdapter() = default;

  virtual const char* label() const = 0;
  virtual const char* entity_name() const = 0;
  virtual OwnedKindId kind_id() const = 0;
  virtual OwnedTableStore& store() = 0;
  virtual bool tombstone_yields_to_local_edits() const = 0;
  virtual bool lands_empty_batch() const;

  // Pure.
  virtual std::optional<std::string> Identity(const PhiEntity& envelope) const = 0;
  virtual std::string ClientTag(const std::string& identity) const = 0;
  virtual std::vector<std::string> Owners(const std::string& payload) const = 0;
  // Pins' §7.4 doctoring of baselines whose split link was cleared here.
  virtual bool clears_split_partners() const;
  virtual std::map<std::string, std::string> ClearedSplitPartners(
      const std::map<std::string, std::string>& baselines,
      int64_t now,
      const OwnedOwnerMaps& maps);

  // Local access. False = the local rows could not be read.
  virtual bool BeginRound() = 0;
  virtual std::set<std::string> LocalIdentities() = 0;
  virtual OwnedSnapshotBytes Snapshot(const OwnedTable& table,
                                      const OwnedOwnerMaps& maps,
                                      int64_t now,
                                      int64_t hlc_max,
                                      int64_t wall_offset_ms) = 0;
  virtual bool Tombstones(const OwnedTable& table,
                          const OwnedOwnerMaps& maps,
                          int64_t now,
                          OwnedTombstoneResult* out) = 0;
  virtual OwnedParkedClaimResult RetryParkedClaims(
      const std::map<std::string, ParkedOwnedItem>& parked,
      const OwnedOwnerMaps& maps,
      int64_t wall_offset_ms) = 0;
  virtual OwnedPlanOutput Plan(const OwnedPlanInput& input) = 0;
  virtual OwnedLandingOutcome Land(const OwnedLandingInput& input) = 0;
  virtual std::set<std::string> ClaimIdentities(
      const std::map<std::string, std::string>& minted) = 0;
  virtual bool LiveOwners(const std::set<std::string>& candidates,
                          const OwnedOwnerMaps& maps,
                          OwnedLiveRows* out) = 0;

  // Re-probed right before each commit batch, after the last local read.
  // False drops the batch unsent (a later round retries); the kind's own
  // guard against publishing rows a concurrent local migration moved. Pins
  // only; true elsewhere.
  virtual bool CommitFenceHolds();
  // Rules only (§8.4.5 and the soft-delete lifecycle); no-ops elsewhere.
  virtual void NotePublishApplied(const std::set<std::string>& identities);
  virtual void ClearPendingLocalEdits(const std::set<std::string>& candidates);
  virtual void HardDeleteAfterTombstone(const std::set<std::string>& identities);
  virtual int PurgeSoftDeletedRows(int64_t cutoff_ms);
};

// --- The engine -----------------------------------------------------------------------

// SyncContextPhase / SyncContextSnapshot (the Mac's SyncStatusSnapshot):
// the "Phi data" line of the Sync pane's status card.
enum class SyncPhase {
  kChecking,
  kInitialSync,
  kSyncing,
  kUpToDate,
  kOffline,
  kNeedsAttention,
};
const char* SyncPhaseName(SyncPhase phase);
struct SyncStatus {
  SyncPhase phase = SyncPhase::kChecking;
  std::optional<int64_t> last_success_ms;
  uint64_t revision = 0;
};

enum class RoundOutcome {
  kOk,
  kGated,
  kPageBudgetExhausted,
  kLocalReadFailed,
  kUnusableSettings,
  kCursorSaveFailed,
  kPullFailed,
  kNotMyBirthday,
};
const char* RoundOutcomeName(RoundOutcome outcome);

struct EngineDeps {
  RAW_PTR_EXCLUSION SyncProtocolClient* client = nullptr;
  RAW_PTR_EXCLUSION DomainKeyProvider* domain_keys = nullptr;
  RAW_PTR_EXCLUSION MarkerStore* marker_store = nullptr;
  RAW_PTR_EXCLUSION EngineStateStore* state = nullptr;
  RAW_PTR_EXCLUSION SettingsSidecars* sidecars = nullptr;
  std::vector<SyncableSetting> settings;
  std::function<int64_t()> now;  // wall clock, epoch ms
  // What identifies this build for the unreadable-entity record.
  std::string build_identity;
  // The engine asks for another round after a page-budget cut.
  std::function<void()> request_follow_up;
  // Called around a settings apply, so the host can ignore its own echo.
  std::function<void(bool applying)> applying_remote;
  // The Space section (null: a settings-only engine, the Mac's M3-1).
  RAW_PTR_EXCLUSION SpaceLocalAccess* space_access = nullptr;
  RAW_PTR_EXCLUSION SpaceTableStore* space_store = nullptr;
  // A fresh local Space id (the Mac's UUID().uuidString).
  std::function<std::string()> new_space_id;
  // The owned kinds, in landing order (bookmarks, then pins); they need the
  // Space section.
  std::vector<OwnedKindAdapter*> owned_kinds;
};

// §11's per-round counters of one owned kind.
struct OwnedRoundCounters {
  int pulled = 0;
  int applied = 0;
  int tombstones = 0;
  int unreadable = 0;
  int refused = 0;
  int pushed = 0;
  int adopted = 0;
  int superseded_by_delete = 0;
  int resurrected = 0;
  int pending_publish = 0;
  int local_read_failed = 0;
  int cycles_broken = 0;
  int excluded_unmapped_owner = 0;
  int collapsed = 0;
  int transferred = 0;
  // Pins parked only because their owner shape belongs to another
  // pinned-tab scope (cursor parked_for_scope): dormant copies, excluded
  // from status, reported on their own log line.
  int parked_for_scope = 0;
};

// §11's per-round Space counters.
struct SpaceRoundCounters {
  int pulled = 0;
  int applied = 0;
  int refused = 0;
  int parked = 0;
  int pushed = 0;
  int conflicts = 0;
  int tombstones = 0;
  std::string profile_refresh = "skipped";
};

class PhiSyncEngine {
 public:
  explicit PhiSyncEngine(EngineDeps deps);
  ~PhiSyncEngine();

  PhiSyncEngine(const PhiSyncEngine&) = delete;
  PhiSyncEngine& operator=(const PhiSyncEngine&) = delete;

  // The rounds (each one runs to completion before the next).
  // GetUpdates → decrypt → merge → apply, then publish what the merge left
  // the server behind on.
  void PullOnce();
  // Pull first, then publish; one pull-and-retry on CONFLICT.
  void PushLocalSettings();
  // A local preference changed (debounced by the host).
  void HandleLocalSettingsChange();
  // The Space section's gate: account bound, ARK unlocked, pairing done.
  // Opening it replays the whole data type (the shared marker walked past
  // every Space while it was shut).
  void SetSpaceSyncEnabled(bool enabled);
  // A local Space edit (debounced by the host): stamp it at edit time, then
  // push.
  void HandleLocalSpaceChange();
  // The user deleted a local Space: tombstone it at the next push.
  void RecordLocalSpaceDeletion(const std::string& local_space_id);
  // Soft-deleted Spaces past retention are purged.
  void RetentionSweep();
  // A local bookmark / pin edit (debounced by the host): pull, then publish.
  void HandleLocalOwnedChange();

  // Sign-out / account switch / self-revoke: nothing more is written.
  // Any thread.
  void Stop() { stopped_ = true; }
  bool stopped() const { return stopped_; }
  bool requires_reconfiguration() const {
    return marker_.requires_reconfiguration;
  }

  // The status card's "Phi data" context (the Mac's statusSnapshot). Any
  // thread.
  SyncStatus status() const;

  // Diagnostics (the round log and the rig).
  RoundOutcome last_outcome() const { return last_outcome_; }
  int last_pages() const { return last_pages_; }
  bool last_marker_advanced() const { return last_marker_advanced_; }
  int64_t wall_clock_offset_ms() const { return wall_clock_offset_ms_; }
  bool has_adopted() const;
  bool space_section_enabled() const { return space_section_enabled_; }
  const SpaceRoundCounters& last_space_counters() const {
    return space_counters_;
  }
  const std::map<std::string, OwnedRoundCounters>& last_owned_counters() const {
    return owned_counters_;
  }

 private:
  enum class Round {
    kPull,
    kPush,
    kLocalChange,
    kSpaceGateOpen,
    kSpaceGateShut,
    kLocalSpaceChange,
    kRecordDeletion,
    kRetention,
    kLocalOwnedChange,
  };
  struct SpacePullBatch {
    struct Decoded {
      std::string uuid;
      SpaceEntity entity;
      std::string entity_id;
      int64_t version = 0;
    };
    struct Tombstone {
      std::string uuid;
      std::string entity_id;
      int64_t version = 0;
    };
    std::vector<Decoded> decoded;
    std::vector<Tombstone> tombstones;
    std::vector<std::string> unreadable_hashes;
  };
  enum class UnusableReason { kTombstone, kForeignPayload, kUndecryptable };
  struct UnreadableRecord {
    std::string reason;
    std::string key_fingerprint;  // hex
    std::string build;
  };

  void Run(Round round);
  bool Pull(bool then_push);
  void Push(bool retry_on_conflict);
  void PushSettings(bool retry_on_conflict);
  void Apply(const SettingEntity& remote, bool adopt);
  void NoteUnusable(UnusableReason reason);
  void DropEntityCursorAfterInvalidMessage();
  void RequireReconfiguration();
  std::optional<SettingEntity> SnapshotLocalSettings();

  // The Space section.
  bool space_live() const {
    return space_section_enabled_ && deps_.space_store && deps_.space_access;
  }
  void ApplySpaceGate(bool enabled);
  bool ArmSpaceReplayIfNeeded();
  // A registered owned kind the last full replay did not route re-arms one.
  bool ArmKindReplayIfNeeded();
  SpaceSyncTable LoadSpaceTable();
  bool WriteSpaceTable(const SpaceSyncTable& table);
  bool MutateSpaceTable(const std::function<void(SpaceSyncTable&)>& mutate);
  std::map<std::string, std::string> SpaceTagIndex(const SpaceSyncTable& table);
  void RouteSpaceEntity(const RemoteEntity& entity,
                        const keys::Bytes& key,
                        const std::map<std::string, std::string>& tag_index,
                        SpacePullBatch* batch);
  void FlushSpaceObservations(const SpacePullBatch& batch);
  void ApplySpaces(const SpacePullBatch& batch, SpaceSyncTable* table);
  void ApplySpaceTombstones(const SpacePullBatch& batch, SpaceSyncTable* table);
  // nullopt when the Space list could not be read.
  std::optional<std::set<std::string>> LiveLocalUserSpaceIds(
      const SpaceSyncTable& table);
  // The local side could not answer a Space read: the round lands, drops and
  // publishes nothing more, and the marker stays (the next round replays).
  void NoteSpaceListUnreachable();
  void PushSpaces(bool retry_on_conflict,
                  const std::set<std::string>* only_uuids);
  void StampLocalSpaceEdits();
  void ApplyRetentionSweep();
  std::map<std::string, SpaceEntity> SnapshotLocal(
      const std::vector<LocalSpace>& spaces,
      const SpaceSyncTable& table,
      bool ensure_mapped);
  void ObserveStamps(const SpaceEntity& entity);

  // The owned kinds.
  struct OwnedPullBatch {
    struct Arrival {
      std::string identity;
      std::string payload;
      std::string entity_id;
      int64_t version = 0;
    };
    struct Tombstone {
      std::string identity;
      std::string entity_id;
      int64_t version = 0;
    };
    std::vector<Arrival> arrivals;
    std::vector<Tombstone> tombstones;
    std::vector<std::string> unreadable_hashes;
    int unreadable = 0;
  };
  const OwnedOwnerMaps& OwnedRoundMaps();
  void BeginOwnedRound();
  OwnedTable LoadOwnedTable(OwnedKindAdapter& kind,
                            bool arms_replay_on_loss,
                            bool loss_observed_earlier,
                            bool* lost,
                            bool* reported_loss);
  bool WriteOwnedTable(OwnedKindAdapter& kind, const OwnedTable& table);
  bool& HadRecordsFlag(SpaceSyncTable& table, OwnedKindId id);
  bool& ReplayedForEmptyTableFlag(SpaceSyncTable& table, OwnedKindId id);
  // Routes one non-settings, non-Space entity to an owned kind; false when
  // no kind claims it (the Space router then sees it).
  bool RouteOwnedEntity(const RemoteEntity& entity,
                        const keys::Bytes& key,
                        std::map<std::string, OwnedPullBatch>* batches);
  void FlushOwnedObservations(
      const std::map<std::string, OwnedPullBatch>& batches);
  void RetryParkedOwnedClaims(OwnedKindAdapter& kind,
                              const OwnedOwnerMaps& maps);
  static void HarvestTriple(OwnedCursor* cursor,
                            const std::string& entity_id,
                            int64_t version);
  void ApplyOwnedKind(OwnedKindAdapter& kind,
                      const OwnedPullBatch& batch,
                      const OwnedOwnerMaps& maps);
  bool OwnedItemsPublishAllowed();
  void PushOwnedItems(bool retry_on_conflict);
  void PublishOwnedKind(OwnedKindAdapter& kind,
                        const OwnedOwnerMaps& maps,
                        bool retry_on_conflict,
                        const std::set<std::string>* only_identities);
  std::vector<std::string> OwnedLiveSlice(
      OwnedKindAdapter& kind,
      const OwnedSnapshotBytes& snapshot,
      const std::vector<std::string>& candidates,
      int* budget);
  std::vector<std::string> OwnedTombstoneSlice(
      OwnedKindAdapter& kind,
      const OwnedTable& table,
      const std::vector<std::string>& candidates,
      int* budget);
  void DropExpiredOwnedTombstones();
  void PurgeExpiredSoftDeletedOwnedRows();
  void ApplyOwnedRetentionCascade();
  void ObserveStamps(const PhiEntity& envelope);

  // Clock (C2 / AM-2).
  int64_t HlcNow();
  int64_t CorrectedNow() const;
  void ObserveServerDate();
  void ObserveStamps(const SettingEntity& entity);
  void PersistHlcMax();

  // Persisted state.
  bool PersistMarkerState(const MarkerFile& updated);
  bool PersistStoredMarker(const std::optional<std::string>& marker);
  std::optional<std::string> stored_marker() const { return marker_.marker; }
  void set_stored_birthday(const std::string& birthday);
  void WriteState(const std::string& key,
                  const std::optional<std::string>& value);
  std::optional<std::string> stored_entity_id() const;
  void set_stored_entity_id(const std::optional<std::string>& id);
  std::optional<int64_t> stored_version() const;
  void set_stored_version(std::optional<int64_t> version);
  std::optional<SettingEntity> stored_last_entity() const;
  void set_stored_last_entity(const std::optional<SettingEntity>& entity);
  int tombstone_rounds() const;
  void set_tombstone_rounds(int rounds);
  void set_has_adopted(bool adopted);
  std::optional<UnreadableRecord> unreadable_record() const;
  void set_unreadable_record(const std::optional<UnreadableRecord>& record);
  void ClearEntityCursor();
  void ClearRemoteCursor();

  // SyncStatusState.update: an upToDate that completes an older revision
  // than the current one reads as syncing (a newer edit arrived).
  uint64_t UpdateStatus(SyncPhase phase,
                        std::optional<uint64_t> completing = std::nullopt);
  // finishStatusRound: the phase a finished data round leaves.
  void FinishStatusRound(uint64_t revision);
  // noteStatusError: a failed request; a transport failure is "offline".
  void NoteStatusError(const ProtocolStatus& status);

  EngineDeps deps_;
  std::atomic<bool> stopped_{false};
  mutable std::mutex status_lock_;
  SyncStatus status_;
  bool round_offline_ = false;
  MarkerFile marker_;
  HybridClock hlc_;
  int64_t wall_clock_offset_ms_ = 0;

  // The Space section.
  bool space_section_enabled_ = false;
  int64_t last_profile_refresh_at_ms_ = 0;
  bool did_refresh_profiles_this_round_ = false;
  SpaceRoundCounters space_counters_;
  std::string pending_deletion_space_id_;
  bool space_list_unreachable_ = false;

  // The owned kinds' round state (reset by Run).
  std::set<std::string> owned_round_started_;
  std::set<std::string> owned_read_failed_;
  std::set<std::string> owned_loss_observed_at_entry_;
  std::set<std::string> owned_parked_retry_done_;
  std::map<std::string, std::map<std::string, std::string>> owned_tag_indices_;
  std::map<std::string, OwnedTable> owned_tables_;
  std::map<std::string, std::set<std::string>> owned_must_republish_;
  std::map<std::string, std::set<std::string>> owned_deferred_owners_;
  std::optional<OwnedOwnerMaps> owned_maps_this_round_;
  std::map<std::string, OwnedRoundCounters> owned_counters_;

  // Round state.
  bool can_publish_this_round_ = false;
  int cursor_save_failures_ = 0;
  RoundOutcome round_outcome_ = RoundOutcome::kOk;
  int round_pages_ = 0;
  bool round_marker_advanced_ = false;
  bool round_outbound_failed_ = false;
  int follow_up_rounds_used_ = 0;

  RoundOutcome last_outcome_ = RoundOutcome::kOk;
  int last_pages_ = 0;
  bool last_marker_advanced_ = false;
};

// The settings entity's client tag and its hash.
std::string SettingsClientTagHash();

// The engine's cursor keys, exposed for the account-scope wipe.
std::vector<std::string> EngineStateKeys();

}  // namespace phi_client::sync

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_ENGINE_H_

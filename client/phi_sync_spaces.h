// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_SPACES_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_SPACES_H_

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "phibrowser-win/client/phi_sync_proto.h"

// Phi native sync's Space kind (docs/user-data-parity.md S5) — phibrowser-mac
// PhiSpaceSyncState (the cursor table and the shared marker-derived flags),
// SyncableSpaces (snapshot, landing, order projection) and the
// PhiSpaceLocalAccess seam. Host-clean.
namespace phi_client::sync {

inline constexpr char kDefaultSpaceUuid[] = "default-space";
inline constexpr char kIncognitoSpaceUuid[] = "incognito-space";
inline constexpr char kSpaceEntityName[] = "phi-space";
// 30 days: a remote soft delete is purged locally after this.
inline constexpr int64_t kSpaceRetentionMs = 30LL * 24 * 60 * 60 * 1000;

// One Space identity's sync state (PhiSpaceCursor). Entity bytes are
// serialized PhiSpaceEntity.
struct SpaceCursor {
  std::optional<std::string> entity_id;
  int64_t version = 0;
  std::optional<std::string> reconciled;          // the landed baseline
  std::optional<std::string> server;              // what the server holds
  std::optional<std::string> pending_apply;       // parked arrival
  std::optional<std::string> pending_projection;  // this device's own edit
  std::optional<std::string> held_profile_uuid;
  std::optional<std::string> held_for_local_profile_id;
  bool pending_delete = false;
  int delete_reject_rounds = 0;
  bool pending_tombstone = false;
  std::optional<int64_t> deleted_at_ms;
  bool hidden = false;
  std::optional<int64_t> refused_at_ms;
  std::optional<int64_t> purged_at_ms;
  bool operator==(const SpaceCursor& other) const = default;
};

// PhiSpaceSyncTable: the Space cursors plus the SHARED marker-derived state
// (one copy for every kind on data type 2000).
struct SpaceSyncTable {
  static constexpr int kFormatVersion = 2;
  int format_version = kFormatVersion;
  // Inbound acceptance version (the Mac's PhiSpaceSyncTable.
  // currentInboundAcceptanceVersion). Bump it whenever an inbound refusal
  // predicate of any kind is relaxed, so every entity an older build refused
  // is pulled once more: a refusal records nothing and the shared data-type
  // 2000 marker walks past it, so without a replay it never lands after an
  // upgrade. History: 1 (2026-10) URL rule hosts of the "app:<bundle id>"
  // form (App rules).
  static constexpr int kCurrentInboundAcceptanceVersion = 1;
  // The acceptance version of the build that last replayed data type 2000
  // for this account. A table an older build wrote lacks the key and reads
  // as 0, which arms one marker-first replay (ArmSpaceReplayIfNeeded); a
  // fresh table starts current — its first live pull is already a full
  // replay from a nil marker.
  int inbound_acceptance_version = kCurrentInboundAcceptanceVersion;
  std::map<std::string, SpaceCursor> cursors;

  bool drain_in_progress = false;
  bool has_drained_full_replay = false;
  bool had_records = false;
  bool space_section_enabled = false;
  bool marker_moved_while_gate_shut = false;
  bool did_replay_for_empty_table = false;
  std::optional<std::string> last_drained_birthday;
  std::map<std::string, int64_t> unreadable_tag_hashes;

  bool bookmarks_had_records = false;
  bool pins_had_records = false;
  bool url_rules_had_records = false;
  bool bookmarks_replayed_for_empty_table = false;
  bool pins_replayed_for_empty_table = false;
  bool url_rules_replayed_for_empty_table = false;
  // Windows: the owned kinds the last completed full replay routed. A kind
  // registered later (a newer build) re-arms one replay, or the marker would
  // stay past every entity of it the account already holds.
  std::set<std::string> replayed_kinds;

  bool operator==(const SpaceSyncTable& other) const = default;

  // recordLocalDeletion: a published, live Space the user deleted.
  bool RecordLocalDeletion(const std::string& uuid);
  // Soft-deleted cursors past retention become bare tombstones.
  std::vector<std::string> PurgeExpired(int64_t now_ms);
};

class SpaceTableStore {
 public:
  virtual ~SpaceTableStore() = default;
  virtual SpaceSyncTable Load() = 0;
  virtual bool Save(const SpaceSyncTable& table) = 0;
};

// One local Space as sync sees it (PhiLocalSpace).
struct LocalSpace {
  std::string space_id;
  std::string profile_id;
  std::string name;
  std::string color_hex;
  std::string icon_name;
  int sort_order = 0;
  int64_t created_ms = 0;
  std::optional<std::string> theme_id;
  std::optional<double> opacity_light;
  std::optional<double> opacity_dark;
  bool operator==(const LocalSpace& other) const = default;
};

enum class ProfileRefreshOutcome { kUnchanged, kChanged, kSkipped, kFailed };

// A presence query's answer. kUnreachable: the local side could not be asked
// (a stalled or retired UI hop, no Space list open) — never "absent", which
// would drop a live mapping and land a duplicate.
enum class LocalPresence { kPresent, kAbsent, kUnreachable };

// PhiSpaceLocalAccess. Reads are queries; every write is one entity landing
// and returns false when it did not land. A Space list the local side could
// not read is nullopt, never empty. Identities: local Space ids and Chromium
// profile basenames on this side; sync uuids on the wire.
class SpaceLocalAccess {
 public:
  virtual ~SpaceLocalAccess() = default;

  // Publishable Spaces: user Spaces whose profile has an account uuid (the
  // default Space always) — no Incognito, no agent Spaces.
  virtual std::optional<std::vector<LocalSpace>> CurrentSpaces() = 0;
  // Every local Space in strip order (the order projection's input).
  virtual std::optional<std::vector<LocalSpace>> AllSpacesForOrdering() = 0;
  // The pairing's candidates: user Spaces regardless of profile mapping.
  virtual std::optional<std::vector<LocalSpace>> PairableSpaces() = 0;
  // After an unanswered Space read: true when the account's store cannot
  // open (failed, needs a newer app) — persistent, so Space arrivals park
  // and the marker moves on — rather than a stalled hop, which blocks the
  // round until the next one can ask.
  virtual bool SpaceStoreUnavailable() = 0;

  virtual std::optional<std::string> GlobalUuid(const std::string& profile_id) = 0;
  virtual std::optional<std::string> LocalProfileId(const std::string& uuid) = 0;
  virtual LocalPresence IsKnownLocalProfile(const std::string& profile_id) = 0;
  virtual void DropProfileMapping(const std::string& profile_id) = 0;
  // This device's default profile (the Mac's LocalStore.defaultProfileId).
  virtual std::string DefaultProfileId() = 0;

  virtual std::optional<std::string> SyncUuid(const std::string& space_id) = 0;
  virtual std::optional<std::string> LocalSpaceId(const std::string& uuid) = 0;
  // Mints (and persists) a sync uuid for an unmapped Space; nullopt when the
  // mapping could not be persisted.
  virtual std::optional<std::string> EnsureMapped(const std::string& space_id) = 0;
  virtual bool MapSpace(const std::string& space_id, const std::string& uuid) = 0;
  virtual void DropSpaceMapping(const std::string& space_id) = 0;
  virtual LocalPresence IsKnownLocalSpace(const std::string& space_id) = 0;
  virtual std::map<std::string, std::string> AllSpaceMappings() = 0;
  virtual bool IsImporting(const std::string& space_id) = 0;

  virtual ProfileRefreshOutcome RefreshAccountProfiles() = 0;

  virtual bool Create(const LocalSpace& space) = 0;
  virtual bool Update(const std::string& space_id,
                      const std::optional<std::string>& name,
                      const std::optional<std::string>& color_hex,
                      const std::optional<std::string>& icon_name,
                      std::optional<int64_t> created_ms) = 0;
  virtual bool Rebind(const std::string& space_id,
                      const std::string& profile_id) = 0;
  virtual bool ApplyThemeState(const std::string& space_id,
                               const std::optional<std::string>& theme_id,
                               std::optional<double> opacity_light,
                               std::optional<double> opacity_dark) = 0;
  virtual bool ApplyOrder(const std::vector<std::string>& ordered_space_ids) = 0;
  virtual bool Hide(const std::string& space_id) = 0;
  virtual bool Purge(const std::string& space_id) = 0;
};

// --- SyncableSpaces ------------------------------------------------------------------

// §6.2 S1-S4: publishable Spaces → entities, stamped against their
// baselines (and this device's own pending projection).
std::map<std::string, SpaceEntity> SnapshotSpaces(
    const std::vector<LocalSpace>& spaces,
    const SpaceSyncTable& table,
    const std::function<std::optional<std::string>(const std::string&)>& global_uuid,
    const std::function<std::optional<std::string>(const std::string&)>& sync_uuid,
    int64_t now);

// Lands one merged entity: creates, or applies theme state / rebind / row
// fields. Returns the local Space id, or nullopt when it did not land.
std::optional<std::string> LandSpace(const SpaceEntity& merged,
                                     const std::optional<LocalSpace>& existing,
                                     const std::optional<std::string>& local_space_id,
                                     const std::optional<std::string>& profile_id,
                                     SpaceLocalAccess& access,
                                     const std::function<std::string()>& new_id,
                                     int64_t now_ms);

// §7: the synced Spaces reordered by (rank, uuid) inside the slots they
// already occupy; unsynced Spaces keep theirs.
std::vector<std::string> PlannedSpaceOrder(
    const std::vector<LocalSpace>& local_order,
    const std::map<std::string, std::pair<std::string, std::string>>& synced_ranks);

int64_t OpacityMilliUnits(std::optional<double> value);
std::optional<double> OpacityFromMilli(const SettingValue& value);

}  // namespace phi_client::sync

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_SPACES_H_

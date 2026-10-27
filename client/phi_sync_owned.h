// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_OWNED_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_OWNED_H_

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "phibrowser-win/client/phi_sync_clock.h"
#include "phibrowser-win/client/phi_sync_proto.h"
#include "phibrowser-win/client/phi_sync_rank.h"

// Phi native sync's owned items (bookmarks, pinned tabs; docs/user-data-
// parity.md S5) — phibrowser-mac PhiOwnedItemState (the cursor table) and
// SyncableOwnedItems (snapshot, diff, plan, adoption): pure functions over
// a kind's traits, no store and no clock of their own. Host-clean.
//
// A kind `K` provides (the Mac's OwnedItemKind):
//   using Entity; using Local;
//   static constexpr bool kTombstoneYieldsToLocalEdits;
//   static std::string Identity(const Entity&);
//   static std::optional<std::string> LocalIdentity(const Local&,
//       const OwnerResolver&, const std::optional<std::string>& scope);
//   static PhiEntity Envelope(const Entity&);
//   static std::optional<Entity> FromEnvelope(const PhiEntity&);
//   static std::optional<Entity> Project(const Local&, const OwnerResolver&,
//       const std::optional<std::string>& scope,
//       const std::optional<std::string>& parent_identity);
//   static Entity Merge(const Entity& local, const Entity& remote);
//   static std::optional<OwnedRefusal> Refuses(const Entity&,
//       const std::optional<Entity>& baseline);
//   static std::vector<std::string> OwnerUuids(const Entity&);
//   static std::optional<std::string> EligibilityOwner(const Local&,
//       const OwnerResolver&, const std::optional<std::string>& scope);
//   static std::string LocalId(const Local&);
//   static std::optional<std::string> LocalParentId(const Local&);
//   static std::string Rank(const Entity&);
//   static int64_t LocationStamp(const Entity&);
//   static int64_t ContentStamp(const Entity&);
//   static Entity Stamp(const Entity& projected,
//       const std::optional<Entity>& baseline, const Local&,
//       const std::string& rank, int64_t now, int64_t hlc_max,
//       int64_t wall_offset_ms);
//   static std::string ContentSignature(const Entity&);
//   static std::optional<Entity> Reverted(const Entity&,
//       const std::optional<Entity>& baseline, int64_t stamp);
//   static std::optional<std::string> TargetOwnerUuid(const Entity&);
//   static std::optional<RuleProjection> TransferSource(const Entity&,
//       const OwnerResolver&);
namespace phi_client::sync {

// --- The cursor table (PhiOwnedItemState) -------------------------------------------

// One owned identity's sync state (PhiOwnedItemCursor). Entity bytes are
// serialized PhiEntity envelopes.
struct OwnedCursor {
  std::string entity_id;  // "" = never on the account: the next push creates
  int64_t version = 0;
  std::optional<std::string> reconciled;
  std::optional<std::string> server;
  // The row's current owner: its Space's sync uuid (bookmarks), its owner
  // key (pins). Kept when the row goes: the diff needs it.
  std::optional<std::string> owner_uuid;
  std::optional<std::string> pending_apply;
  std::optional<std::string> pending_owner_uuid;
  // True when pin landing parked pending_apply only because its owner
  // resolves locally but in another pinned-tab scope (a dormant copy of an
  // app, Profile or Space shape the current scope does not use). It is still
  // replanned every round and lands if the scope changes, but status counts
  // it neither as pending inbound nor as held. Set only by pin landing;
  // cleared whenever the cursor lands, is deleted, or is parked for any other
  // reason. Stored as "parkedForScope"; absent reads false, so older cursor
  // files decode unchanged.
  bool parked_for_scope = false;
  bool pending_tombstone = false;
  std::optional<std::string> pending_partner_lineage;
  bool pending_delete = false;
  int64_t delete_decided_at_ms = 0;  // hybrid-logical (A9)
  int delete_reject_rounds = 0;
  std::optional<int> rekey_reject_rounds;
  std::optional<int64_t> deleted_at_ms;  // wall clock (retention)
  bool operator==(const OwnedCursor& other) const = default;
};

// One kind's whole table (PhiOwnedItemTable), its own file.
struct OwnedTable {
  static constexpr int kFormatVersion = 1;
  int format_version = kFormatVersion;
  std::map<std::string, OwnedCursor> cursors;
  bool operator==(const OwnedTable& other) const = default;

  // Tombstone cursors finalized more than 30 days ago.
  void DropExpiredTombstones(int64_t now_ms);
  void RemoveCursor(const std::string& identity) { cursors.erase(identity); }
};

class OwnedTableStore {
 public:
  virtual ~OwnedTableStore() = default;
  // A missing, unreadable, older or EMPTY table reads as empty, and reports a
  // loss when the kind `had_records` (the engine then replays the type).
  virtual OwnedTable Load(bool had_records, bool* reported_loss) = 0;
  virtual bool Save(const OwnedTable& table) = 0;
  virtual bool Remove() = 0;
};

// --- Round context --------------------------------------------------------------------

inline constexpr char kAppOwnerKey[] = "app";

// The round's identity translation (OwnerResolver), from OwnedOwnerMaps.
struct OwnerResolver {
  std::function<std::optional<std::string>(const std::string&)> sync_uuid;
  std::function<std::optional<std::string>(const std::string&)> local_space_id;
  std::function<bool(const std::string&)> is_eligible_space;
  std::function<std::optional<std::string>(const std::string&)> global_uuid;
  std::function<std::optional<std::string>(const std::string&)>
      local_profile_id;
  std::function<std::optional<std::string>(const std::string&)>
      local_profile_id_for_space;
};

// The engine's per-round maps (OwnedOwnerMaps).
struct OwnedOwnerMaps {
  std::map<std::string, std::string> sync_uuid_by_space_id;
  std::map<std::string, std::string> local_space_id_by_sync_uuid;
  std::set<std::string> eligible_space_uuids;
  std::map<std::string, std::string> global_uuid_by_profile_id;
  std::map<std::string, std::string> local_profile_id_by_global_uuid;
  std::map<std::string, std::string> local_profile_id_by_space_id;

  OwnedOwnerMaps();
  OwnedOwnerMaps(const OwnedOwnerMaps&);
  OwnedOwnerMaps& operator=(const OwnedOwnerMaps&);
  ~OwnedOwnerMaps();

  // "app" (and incognito-space) map to themselves.
  OwnerResolver Resolver() const;
};

// A rule's transferable values (RuleProjection): what §8.4.4 moves from a
// deleted rule onto its merge partner. Stamps are wire milliseconds.
struct RuleProjection {
  std::string host;
  std::optional<std::string> path_prefix;
  bool ask = false;
  std::optional<int64_t> content_updated_ms;
  std::string target_owner_uuid;
  std::optional<std::string> target_space_id;
  std::optional<int64_t> target_updated_ms;
  std::optional<int> sort_order;
  bool operator==(const RuleProjection& other) const = default;
};

// A rule's duplicate key (RuleSignature): normalized host and path, and
// the account owner it targets.
struct RuleSignature {
  std::string host;
  std::optional<std::string> path_prefix;
  std::string owner;
  bool operator==(const RuleSignature& other) const = default;
  bool operator<(const RuleSignature& other) const;
};

template <typename Entity>
struct OwnedArrival {
  Entity entity;
  std::string entity_id;
  int64_t version = 0;
};

struct OwnedPlanContext {
  std::map<std::string, std::string> pairs;           // identity → local id
  std::map<std::string, std::string> adopted_merges;  // identity → envelope
  std::set<std::string> adopted_field_writes;
  std::set<std::string> tombstoned_identities;
  std::map<std::string, std::string> local_projections;  // identity → envelope
  std::set<std::string> deleted_subtree;
  std::set<std::string> live_local_parents;
  std::optional<std::string> local_scope;
  std::optional<std::string> account_scope;
  bool scope_moved_mid_round = false;
  std::set<std::string> pending_local_edits;
  std::set<std::string> unpublished;
  // URL rules only (§8.4.4): the quiescent merge partner W of X, and the
  // identities whose partner exists but is not at rest.
  std::map<std::string, std::string> merge_partners;
  std::set<std::string> partner_not_at_rest;
  // URL rules: each live rule's signature before this page lands.
  std::map<std::string, RuleSignature> local_signatures;

  bool scope_mismatch() const {
    return scope_moved_mid_round ||
           (local_scope && account_scope && *local_scope != *account_scope);
  }
};

enum class OwnedStepKind { kClaim, kCreate, kMove, kUpdate, kTransfer, kDelete };

struct OwnedApplyStep {
  std::string identity;
  OwnedStepKind kind = OwnedStepKind::kCreate;
  // "" = the Space root; nullopt = the payload's own parent.
  std::optional<std::string> new_parent_uuid;
  std::optional<std::string> new_rank;
  std::optional<std::string> payload;  // envelope bytes to reconcile
  // kMove of a kind with a mutable owner (rules): the new target.
  std::optional<std::string> new_owner_uuid = std::nullopt;
  // kTransfer: X's values onto partner `transfer_to`.
  std::optional<RuleProjection> transfer_source = std::nullopt;
  std::string transfer_to = std::string();
  bool operator==(const OwnedApplyStep& other) const = default;
};

struct ParkedOwnedItem {
  std::string payload;
  std::optional<std::string> pending_owner_uuid;
  bool operator==(const ParkedOwnedItem& other) const = default;
};

struct OwnedHarvest {
  std::string entity_id;
  int64_t version = 0;
};

struct OwnedPlan {
  std::vector<OwnedApplyStep> steps;  // phase-sorted
  std::map<std::string, ParkedOwnedItem> parked;
  int refused = 0;
  int lifted = 0;
  int cycles_broken = 0;
  int64_t cycle_stamp_ms = 0;
  int superseded_by_delete = 0;
  std::set<std::string> cancelled_deletes;
  std::map<std::string, OwnedHarvest> harvest;
  std::set<std::string> must_republish;
  std::set<std::string> parked_tombstones;
  std::set<std::string> yielded_tombstones;
  std::map<std::string, std::string> rebaselined;
  // URL rules: the signatures of identities this plan moves or updates,
  // taken before landing.
  std::map<std::string, RuleSignature> pre_landing_signatures;

  OwnedPlan();
  OwnedPlan(const OwnedPlan&);
  OwnedPlan(OwnedPlan&&);
  OwnedPlan& operator=(const OwnedPlan&);
  OwnedPlan& operator=(OwnedPlan&&);
  ~OwnedPlan();
};

enum class OwnedRefusal {
  kIllegalRank,
  kCycle,
  kIsFolderMismatch,
  kSelfReference,
  kInvalidUuid,
  kInvalidUrl,
  kEmptyHost,
  kDegenerateHost,
  kMalformedHost,
};

template <typename Entity>
struct OwnedSnapshotResult {
  std::map<std::string, Entity> entities;
  int skipped_unmapped_owner = 0;
  int skipped_ineligible_owner = 0;
};

struct OwnedTombstoneResult {
  std::vector<std::string> identities;  // children before parents
  std::map<std::string, OwnedCursor> cursor_updates;
  std::set<std::string> deferred;
};

// --- Helpers --------------------------------------------------------------------------

template <typename K>
std::optional<typename K::Entity> DecodeOwned(const std::string& bytes) {
  PhiEntity envelope;
  if (!envelope.ParseFrom(bytes)) {
    return std::nullopt;
  }
  return K::FromEnvelope(envelope);
}

template <typename K>
std::optional<typename K::Entity> DecodeOwned(
    const std::optional<std::string>& bytes) {
  return bytes ? DecodeOwned<K>(*bytes) : std::nullopt;
}

template <typename K>
std::string EncodeOwned(const typename K::Entity& entity) {
  return K::Envelope(entity).Serialize();
}

// Depth to the root through `parent_of`, bounded by its size (a cycle ends).
std::map<std::string, int> OwnedDepths(
    const std::vector<std::string>& identities,
    const std::map<std::string, std::string>& parent_of);

// Four phases: claim/create/move, update, delete.
int OwnedStepPhase(OwnedStepKind kind);

// --- Snapshot (§4.2) ------------------------------------------------------------------

// Outbound entities for every eligible local row, by identity. `locals` in
// sibling order (the ranks keep it).
template <typename K>
OwnedSnapshotResult<typename K::Entity> OwnedSnapshot(
    const std::vector<typename K::Local>& locals,
    const OwnedTable& table,
    const OwnerResolver& resolve,
    const std::optional<std::string>& scope,
    int64_t now,
    int64_t hlc_max,
    int64_t wall_offset_ms) {
  using Entity = typename K::Entity;
  OwnedSnapshotResult<Entity> result;

  // Pass 1: each row's own eligibility.
  std::map<std::string, size_t> index_by_local_id;
  std::vector<std::optional<std::string>> identity_of;
  std::vector<bool> self_eligible;
  for (size_t offset = 0; offset < locals.size(); ++offset) {
    const auto& local = locals[offset];
    index_by_local_id[K::LocalId(local)] = offset;
    const std::optional<std::string> identity =
        K::LocalIdentity(local, resolve, scope);
    identity_of.push_back(identity);
    if (!identity) {
      self_eligible.push_back(false);
      continue;
    }
    const std::optional<std::string> owner =
        K::EligibilityOwner(local, resolve, scope);
    if (!owner) {
      ++result.skipped_unmapped_owner;
      self_eligible.push_back(false);
      continue;
    }
    if (resolve.local_space_id(*owner) && !resolve.is_eligible_space(*owner)) {
      ++result.skipped_ineligible_owner;
      self_eligible.push_back(false);
      continue;
    }
    // Parked, pending-tombstone and pending-delete cursors never publish.
    const auto cursor = table.cursors.find(*identity);
    if (cursor != table.cursors.end() &&
        (cursor->second.pending_apply || cursor->second.pending_tombstone ||
         cursor->second.pending_delete)) {
      self_eligible.push_back(false);
      continue;
    }
    self_eligible.push_back(true);
  }

  // §4.2 rule 2: every ancestor must be eligible too.
  auto chain_eligible = [&](size_t offset) {
    if (!self_eligible[offset]) {
      return false;
    }
    size_t hops = 0;
    std::optional<std::string> parent = K::LocalParentId(locals[offset]);
    while (parent && hops <= locals.size()) {
      const auto it = index_by_local_id.find(*parent);
      if (it == index_by_local_id.end() || !self_eligible[it->second]) {
        return false;
      }
      parent = K::LocalParentId(locals[it->second]);
      ++hops;
    }
    return true;
  };

  std::map<std::string, std::string> identity_by_local_id;
  for (size_t offset = 0; offset < locals.size(); ++offset) {
    if (self_eligible[offset]) {
      identity_by_local_id[K::LocalId(locals[offset])] = *identity_of[offset];
    }
  }

  struct Candidate {
    std::string identity;
    size_t local;
    Entity entity;
    std::string group;
  };
  std::vector<Candidate> candidates;
  std::set<std::string> claimed;  // one candidate per identity
  for (size_t offset = 0; offset < locals.size(); ++offset) {
    if (!chain_eligible(offset) || !identity_of[offset]) {
      continue;
    }
    const std::string& identity = *identity_of[offset];
    if (!claimed.insert(identity).second) {
      continue;
    }
    std::optional<std::string> parent_identity;
    if (const std::optional<std::string> parent =
            K::LocalParentId(locals[offset])) {
      const auto it = identity_by_local_id.find(*parent);
      if (it != identity_by_local_id.end()) {
        parent_identity = it->second;
      }
    }
    std::optional<Entity> entity =
        K::Project(locals[offset], resolve, scope, parent_identity);
    if (!entity) {
      continue;
    }
    std::string group;
    for (const std::string& owner : K::OwnerUuids(*entity)) {
      if (!group.empty()) {
        group.push_back('\0');
      }
      group += owner;
    }
    candidates.push_back({identity, offset, std::move(*entity), group});
  }

  std::map<std::string, Entity> baselines;
  for (const Candidate& candidate : candidates) {
    const auto cursor = table.cursors.find(candidate.identity);
    if (cursor == table.cursors.end()) {
      continue;
    }
    if (std::optional<Entity> baseline = DecodeOwned<K>(cursor->second.reconciled)) {
      baselines.emplace(candidate.identity, std::move(*baseline));
    }
  }
  auto baseline_rank = [&](const std::string& identity) -> std::optional<std::string> {
    const auto it = baselines.find(identity);
    if (it == baselines.end()) {
      return std::nullopt;
    }
    const std::string rank = K::Rank(it->second);
    return IsLegalRank(rank) ? std::optional<std::string>(rank) : std::nullopt;
  };

  // Ranks compare within one owner group.
  std::vector<std::string> group_order;
  std::map<std::string, std::vector<size_t>> groups;
  for (size_t offset = 0; offset < candidates.size(); ++offset) {
    auto [it, inserted] = groups.try_emplace(candidates[offset].group);
    if (inserted) {
      group_order.push_back(candidates[offset].group);
    }
    it->second.push_back(offset);
  }
  std::map<std::string, std::string> assigned;
  for (const std::string& key : group_order) {
    std::vector<RankedItem> order;
    for (size_t member : groups[key]) {
      order.push_back({candidates[member].identity,
                       baseline_rank(candidates[member].identity)});
    }
    for (auto& [identity, rank] : AssignRanks(order)) {
      assigned[identity] = rank;
    }
  }

  for (const Candidate& candidate : candidates) {
    const auto it = assigned.find(candidate.identity);
    const std::string rank = it != assigned.end()
                                 ? it->second
                                 : baseline_rank(candidate.identity).value_or("V");
    const auto base = baselines.find(candidate.identity);
    result.entities.emplace(
        candidate.identity,
        K::Stamp(candidate.entity,
                 base == baselines.end() ? std::nullopt
                                         : std::optional<Entity>(base->second),
                 locals[candidate.local], rank, now, hlc_max, wall_offset_ms));
  }
  return result;
}

// --- Diff tombstones (§4.7) -----------------------------------------------------------

// The local-deletion source: a published identity no live row claims.
template <typename K>
OwnedTombstoneResult OwnedTombstones(
    const std::vector<typename K::Local>& locals,
    const OwnedTable& table,
    const OwnerResolver& resolve,
    const std::optional<std::string>& scope,
    int64_t now_ms,  // hybrid-logical: compared against wire stamps (A9)
    const std::set<std::string>& pending_claims = {},
    const std::set<std::string>& explicit_deletions = {},
    const std::set<std::string>& deferred_deletions = {}) {
  using Entity = typename K::Entity;
  std::set<std::string> live;
  for (const auto& local : locals) {
    if (std::optional<std::string> identity =
            K::LocalIdentity(local, resolve, scope)) {
      live.insert(*identity);
    }
  }
  OwnedTombstoneResult result;
  result.deferred = deferred_deletions;
  for (const auto& [identity, cursor] : table.cursors) {
    if (deferred_deletions.count(identity) || !cursor.reconciled ||
        cursor.deleted_at_ms || live.count(identity) ||
        pending_claims.count(identity)) {
      continue;
    }
    if (!explicit_deletions.count(identity)) {
      // Unknown or unmapped ownership is never permission to delete.
      if (!cursor.owner_uuid) {
        continue;
      }
      const std::string& owner = *cursor.owner_uuid;
      const bool mapped = resolve.local_space_id(owner).has_value() ||
                          resolve.local_profile_id(owner).has_value();
      if (!mapped) {
        continue;
      }
      if (resolve.local_space_id(owner) && !resolve.is_eligible_space(owner)) {
        continue;
      }
    }
    result.identities.push_back(identity);
    OwnedCursor updated = cursor;
    updated.pending_apply.reset();
    updated.pending_owner_uuid.reset();
    updated.parked_for_scope = false;
    updated.pending_delete = true;
    if (!cursor.pending_delete) {
      updated.delete_decided_at_ms = now_ms;
    }
    if (updated != cursor) {
      result.cursor_updates[identity] = updated;
    }
  }
  // Children before parents; baselines are the only parent source.
  std::map<std::string, std::string> parent_of;
  for (const std::string& identity : result.identities) {
    const std::optional<Entity> entity =
        DecodeOwned<K>(table.cursors.at(identity).reconciled);
    if (!entity) {
      continue;
    }
    for (const std::string& owner : K::OwnerUuids(*entity)) {
      if (table.cursors.count(owner)) {
        parent_of[identity] = owner;
        break;
      }
    }
  }
  const std::map<std::string, int> depths =
      OwnedDepths(result.identities, parent_of);
  std::sort(result.identities.begin(), result.identities.end(),
            [&](const std::string& a, const std::string& b) {
              const int left = depths.at(a);
              const int right = depths.at(b);
              return left == right ? a < b : left > right;
            });
  return result;
}

// --- The derived yield predicate (C4 direction i) ------------------------------------

template <typename K>
std::set<std::string> OwnedUnpublishedEdits(
    const std::map<std::string, std::string>& projections,
    const OwnedTable& table) {
  std::set<std::string> out;
  for (const auto& [identity, bytes] : projections) {
    const auto cursor = table.cursors.find(identity);
    if (cursor == table.cursors.end()) {
      continue;
    }
    const auto projected = DecodeOwned<K>(bytes);
    const auto baseline = DecodeOwned<K>(cursor->second.reconciled);
    if (!projected || !baseline) {
      continue;
    }
    if (K::ContentSignature(*projected) != K::ContentSignature(*baseline) ||
        K::OwnerUuids(*projected) != K::OwnerUuids(*baseline)) {
      out.insert(identity);
    }
  }
  return out;
}

std::set<std::string> OwnedUnpublishedMerges(
    const std::map<std::string, std::string>& projections,
    const OwnedTable& table);

// --- The projection domain (C5-a step 4a) ---------------------------------------------

template <typename K>
std::set<std::string> OwnedProjectionDomain(
    const std::vector<typename K::Entity>& arrivals,
    const std::map<std::string, ParkedOwnedItem>& parked,
    const std::set<std::string>& tombstoned,
    const std::function<std::optional<std::string>(const std::string&)>&
        local_parent) {
  std::set<std::string> domain = tombstoned;
  for (const auto& [identity, item] : parked) {
    domain.insert(identity);
  }
  std::vector<std::string> frontier;
  std::set<std::string> seeded;
  for (const auto& entity : arrivals) {
    const std::string identity = K::Identity(entity);
    if (identity.empty()) {
      continue;
    }
    domain.insert(identity);
    seeded.insert(identity);
    for (const std::string& owner : K::OwnerUuids(entity)) {
      frontier.push_back(owner);
    }
  }
  for (const auto& [identity, item] : parked) {
    if (seeded.count(identity)) {
      continue;
    }
    if (const auto entity = DecodeOwned<K>(item.payload)) {
      for (const std::string& owner : K::OwnerUuids(*entity)) {
        frontier.push_back(owner);
      }
    }
  }
  for (const std::string& identity : domain) {
    if (std::optional<std::string> parent = local_parent(identity)) {
      frontier.push_back(*parent);
    }
  }
  while (!frontier.empty()) {
    const std::string identity = frontier.back();
    frontier.pop_back();
    if (identity.empty() || !domain.insert(identity).second) {
      continue;
    }
    if (std::optional<std::string> parent = local_parent(identity)) {
      frontier.push_back(*parent);
    }
  }
  return domain;
}

// --- The inbound plan (§4.4) ----------------------------------------------------------

template <typename K>
OwnedPlan OwnedPlanFor(const std::vector<OwnedArrival<typename K::Entity>>& arrivals,
                       const std::map<std::string, ParkedOwnedItem>& parked,
                       const OwnedTable& table,
                       const OwnerResolver& resolve,
                       const OwnedPlanContext& context) {
  using Entity = typename K::Entity;
  OwnedPlan plan;

  // Harvest first, whatever happens to the entity (A6).
  for (const auto& item : arrivals) {
    const std::string identity = K::Identity(item.entity);
    if (identity.empty()) {
      continue;
    }
    OwnedHarvest& harvest = plan.harvest[identity];
    if (!item.entity_id.empty()) {
      harvest.entity_id = item.entity_id;
    }
    harvest.version = std::max(harvest.version, item.version);
  }

  // §7.3: a scope mismatch parks every arrival and tombstone.
  if (context.scope_mismatch()) {
    plan.parked = parked;
    for (const auto& item : arrivals) {
      const std::string identity = K::Identity(item.entity);
      if (identity.empty()) {
        continue;
      }
      const std::vector<std::string> owners = K::OwnerUuids(item.entity);
      plan.parked[identity] = ParkedOwnedItem{
          EncodeOwned<K>(item.entity),
          owners.empty() ? std::nullopt : std::optional<std::string>(owners[0])};
    }
    plan.parked_tombstones = context.tombstoned_identities;
    return plan;
  }

  // 1. The working set: parked items (uuid order) then this page's
  // arrivals, which replace a parked copy.
  struct Item {
    std::string identity;
    Entity entity;
  };
  std::vector<Item> working;
  std::map<std::string, size_t> slot_of;
  for (const auto& [identity, item] : parked) {  // std::map: sorted
    std::optional<Entity> entity = DecodeOwned<K>(item.payload);
    if (!entity) {
      plan.parked[identity] = item;  // kept verbatim
      continue;
    }
    slot_of[identity] = working.size();
    working.push_back({identity, std::move(*entity)});
  }
  for (const auto& item : arrivals) {
    const std::string identity = K::Identity(item.entity);
    if (identity.empty()) {
      ++plan.refused;
      continue;
    }
    const auto slot = slot_of.find(identity);
    if (slot != slot_of.end()) {
      working[slot->second] = {identity, item.entity};
    } else {
      slot_of[identity] = working.size();
      working.push_back({identity, item.entity});
    }
  }

  // 2. Refusals; a tombstone replaces its own live entity.
  auto baseline_of = [&](const std::string& identity) -> std::optional<Entity> {
    const auto cursor = table.cursors.find(identity);
    return cursor == table.cursors.end()
               ? std::nullopt
               : DecodeOwned<K>(cursor->second.reconciled);
  };
  std::vector<Item> survivors;
  for (const Item& item : working) {
    if (context.tombstoned_identities.count(item.identity)) {
      continue;
    }
    if (K::Refuses(item.entity, baseline_of(item.identity))) {
      ++plan.refused;
      continue;
    }
    survivors.push_back(item);
  }
  std::set<std::string> survivor_identities;
  for (const Item& item : survivors) {
    survivor_identities.insert(item.identity);
  }

  // 3. Ownership classes.
  enum class State { kItem, kLift, kExternal, kUnresolved };
  auto classify = [&](const std::string& uuid) {
    if (survivor_identities.count(uuid)) {
      return State::kItem;
    }
    const auto cursor = table.cursors.find(uuid);
    if (context.tombstoned_identities.count(uuid) ||
        (cursor != table.cursors.end() &&
         (cursor->second.deleted_at_ms || cursor->second.pending_delete))) {
      return State::kLift;
    }
    if (context.live_local_parents.count(uuid)) {
      return State::kExternal;
    }
    if (resolve.local_space_id(uuid)) {
      return resolve.is_eligible_space(uuid) ? State::kExternal
                                             : State::kUnresolved;
    }
    if (resolve.local_profile_id(uuid)) {
      return State::kExternal;
    }
    return State::kUnresolved;
  };

  // 4a. C5-a: break move cycles, oldest location stamp loses.
  std::map<std::string, Entity> entity_of;
  for (const Item& item : survivors) {
    entity_of.emplace(item.identity, item.entity);
  }
  auto projection_of = [&](const std::string& identity) -> std::optional<Entity> {
    const auto it = context.local_projections.find(identity);
    return it == context.local_projections.end() ? std::nullopt
                                                 : DecodeOwned<K>(it->second);
  };
  std::map<std::string, std::optional<Entity>> cycle_nodes;
  std::function<std::optional<Entity>(const std::string&)> cycle_node =
      [&](const std::string& identity) -> std::optional<Entity> {
    const auto cached = cycle_nodes.find(identity);
    if (cached != cycle_nodes.end()) {
      return cached->second;
    }
    const std::optional<Entity> projection = projection_of(identity);
    std::optional<Entity> node;
    const auto arrival = entity_of.find(identity);
    if (arrival != entity_of.end()) {
      node = projection ? K::Merge(*projection, arrival->second)
                        : arrival->second;
    } else if (classify(identity) == State::kExternal) {
      node = projection;
    }
    cycle_nodes[identity] = node;
    return node;
  };
  auto cycle_edge = [&](const std::string& identity) -> std::optional<std::string> {
    const std::optional<Entity> entity = cycle_node(identity);
    if (!entity) {
      return std::nullopt;
    }
    std::optional<std::string> best;
    for (const std::string& owner : K::OwnerUuids(*entity)) {
      if (owner != identity && cycle_node(owner) && (!best || owner < *best)) {
        best = owner;
      }
    }
    return best;
  };
  auto first_cycle = [&]() -> std::optional<std::vector<std::string>> {
    std::set<std::string> settled;
    std::vector<std::string> starts;
    for (const Item& item : survivors) {
      starts.push_back(item.identity);
    }
    std::sort(starts.begin(), starts.end());
    for (const std::string& start : starts) {
      if (settled.count(start)) {
        continue;
      }
      std::vector<std::string> walk;
      std::map<std::string, size_t> visited_at;
      std::optional<std::string> cursor = start;
      while (cursor && !visited_at.count(*cursor) && !settled.count(*cursor)) {
        visited_at[*cursor] = walk.size();
        walk.push_back(*cursor);
        cursor = cycle_edge(*cursor);
      }
      if (cursor && visited_at.count(*cursor)) {
        std::vector<std::string> cycle;
        for (size_t i = visited_at[*cursor]; i < walk.size(); ++i) {
          cycle.push_back(walk[i]);
        }
        return cycle;
      }
      settled.insert(walk.begin(), walk.end());
    }
    return std::nullopt;
  };
  std::set<std::string> reverted_by_cycle;
  while (const std::optional<std::vector<std::string>> cycle = first_cycle()) {
    int64_t newest = 0;
    for (const std::string& member : *cycle) {
      if (const auto node = cycle_node(member)) {
        newest = std::max(newest, K::LocationStamp(*node));
      }
    }
    const int64_t cycle_stamp = HybridClock::EditStamp(0, newest);
    auto stamp_of = [&](const std::string& member) {
      const auto node = cycle_node(member);
      return node ? K::LocationStamp(*node) : 0;
    };
    const std::string loser = *std::min_element(
        cycle->begin(), cycle->end(),
        [&](const std::string& a, const std::string& b) {
          const int64_t left = stamp_of(a);
          const int64_t right = stamp_of(b);
          return left == right ? a < b : left < right;
        });
    const std::optional<Entity> landing = cycle_node(loser);
    if (!landing) {
      break;
    }
    const auto arrival = entity_of.find(loser);
    const std::optional<Entity> restored =
        K::Reverted(arrival != entity_of.end() ? arrival->second : *landing,
                    baseline_of(loser), cycle_stamp);
    if (!restored || K::OwnerUuids(*restored) == K::OwnerUuids(*landing) ||
        !reverted_by_cycle.insert(loser).second) {
      break;
    }
    bool replaced = false;
    for (Item& item : survivors) {
      if (item.identity == loser) {
        item.entity = *restored;
        replaced = true;
      }
    }
    if (!replaced) {
      survivors.push_back({loser, *restored});
      survivor_identities.insert(loser);
    }
    entity_of.insert_or_assign(loser, *restored);
    cycle_nodes.clear();
    plan.cycle_stamp_ms = std::max(plan.cycle_stamp_ms, cycle_stamp);
    ++plan.cycles_broken;
  }

  // 4b. Kahn topological order; what cannot be ordered is refused.
  std::map<std::string, std::vector<std::string>> dependencies;
  for (const Item& item : survivors) {
    std::vector<std::string>& deps = dependencies[item.identity];
    for (const std::string& owner : K::OwnerUuids(item.entity)) {
      if (classify(owner) == State::kItem && owner != item.identity) {
        deps.push_back(owner);
      }
    }
  }
  std::vector<Item> ordered;
  std::set<std::string> placed;
  std::vector<Item> remaining = survivors;
  for (;;) {
    bool progressed = false;
    std::vector<Item> still;
    for (Item& item : remaining) {
      const std::vector<std::string>& deps = dependencies[item.identity];
      const bool ready = std::all_of(
          deps.begin(), deps.end(),
          [&](const std::string& dep) { return placed.count(dep) > 0; });
      if (ready) {
        placed.insert(item.identity);
        ordered.push_back(std::move(item));
        progressed = true;
      } else {
        still.push_back(std::move(item));
      }
    }
    remaining = std::move(still);
    if (remaining.empty() || !progressed) {
      break;
    }
  }
  plan.refused += static_cast<int>(remaining.size());

  // 5. Land in topological order.
  std::vector<OwnedApplyStep> steps;
  std::set<std::string> landed;
  for (const Item& item : ordered) {
    const std::string& identity = item.identity;
    std::optional<std::string> landing_parent;
    bool was_lifted = false;
    std::optional<std::string> blocked_by;
    for (const std::string& owner : K::OwnerUuids(item.entity)) {
      switch (classify(owner)) {
        case State::kItem:
          if (landed.count(owner)) {
            landing_parent = owner;
          } else {
            blocked_by = owner;
          }
          break;
        case State::kLift:
          was_lifted = true;
          landing_parent = std::string();
          break;
        case State::kExternal:
          break;
        case State::kUnresolved:
          blocked_by = owner;
          break;
      }
      if (blocked_by) {
        break;
      }
    }
    if (blocked_by) {
      plan.parked[identity] =
          ParkedOwnedItem{EncodeOwned<K>(item.entity), blocked_by};
      continue;
    }

    const auto cursor_it = table.cursors.find(identity);
    const OwnedCursor* cursor =
        cursor_it == table.cursors.end() ? nullptr : &cursor_it->second;
    const std::optional<Entity> baseline = baseline_of(identity);
    std::optional<Entity> adopted;
    if (const auto it = context.adopted_merges.find(identity);
        it != context.adopted_merges.end()) {
      adopted = DecodeOwned<K>(it->second);
    }
    const std::optional<Entity> local_projection = projection_of(identity);
    const Entity merged =
        adopted            ? *adopted
        : local_projection ? K::Merge(*local_projection, item.entity)
        : baseline         ? K::Merge(*baseline, item.entity)
                           : item.entity;
    // A local location that won lands the merged parent, not the remote one.
    if (!was_lifted && landing_parent &&
        K::OwnerUuids(merged) != K::OwnerUuids(item.entity)) {
      landing_parent.reset();
    }

    // §5.6 L1: a live arrival meets a pending local delete (A9).
    if (cursor && cursor->pending_delete) {
      // §8.4.4 β: a quiescent merge partner takes the arrival's edit; the
      // deletion stands.
      if (K::kTombstoneYieldsToLocalEdits) {
        const auto partner = context.merge_partners.find(identity);
        if (partner != context.merge_partners.end()) {
          if (std::optional<RuleProjection> source =
                  K::TransferSource(merged, resolve)) {
            OwnedApplyStep step;
            step.identity = identity;
            step.kind = OwnedStepKind::kTransfer;
            step.transfer_source = std::move(source);
            step.transfer_to = partner->second;
            steps.push_back(std::move(step));
            continue;
          }
        }
        if (context.partner_not_at_rest.count(identity)) {
          const std::vector<std::string> owners = K::OwnerUuids(item.entity);
          plan.parked[identity] = ParkedOwnedItem{
              EncodeOwned<K>(item.entity),
              owners.empty() ? std::nullopt : std::optional<std::string>(owners[0])};
          continue;
        }
      }
      const int64_t decided_at = cursor->delete_decided_at_ms;
      const bool newer_than_deletion =
          std::max(K::LocationStamp(merged), K::ContentStamp(merged)) >
          decided_at;
      const bool parent_is_live =
          !landing_parent || landing_parent->empty() ||
          landed.count(*landing_parent) ||
          context.live_local_parents.count(*landing_parent);
      const bool outside_deleted_subtree =
          !context.deleted_subtree.count(identity) &&
          !(landing_parent && context.deleted_subtree.count(*landing_parent));
      if (!newer_than_deletion || !parent_is_live || !outside_deleted_subtree) {
        ++plan.superseded_by_delete;
        continue;
      }
      plan.cancelled_deletes.insert(identity);
    }

    if (was_lifted) {
      ++plan.lifted;
    }
    landed.insert(identity);
    const std::string payload = EncodeOwned<K>(merged);
    const std::string rank = K::Rank(merged);
    if (!adopted && local_projection && payload != EncodeOwned<K>(item.entity)) {
      plan.must_republish.insert(identity);
    }
    if (reverted_by_cycle.count(identity)) {
      plan.must_republish.insert(identity);
    }

    if (context.pairs.count(identity)) {
      steps.push_back({identity, OwnedStepKind::kClaim, landing_parent, rank,
                       payload});
      if (context.adopted_field_writes.count(identity)) {
        steps.push_back({identity, OwnedStepKind::kUpdate, std::nullopt,
                         std::nullopt, payload});
      }
      continue;
    }
    if (!baseline) {
      steps.push_back({identity, OwnedStepKind::kCreate, landing_parent, rank,
                       payload});
      continue;
    }
    const bool moved = was_lifted || reverted_by_cycle.count(identity) ||
                       K::OwnerUuids(merged) != K::OwnerUuids(*baseline) ||
                       K::Rank(merged) != K::Rank(*baseline);
    auto capture_signature = [&] {
      const auto key = context.local_signatures.find(identity);
      if (key != context.local_signatures.end()) {
        plan.pre_landing_signatures.insert_or_assign(identity, key->second);
      }
    };
    if (moved) {
      OwnedApplyStep step{identity, OwnedStepKind::kMove, landing_parent, rank,
                          payload};
      step.new_owner_uuid = K::TargetOwnerUuid(merged);
      steps.push_back(std::move(step));
      capture_signature();
    }
    const bool content_changed =
        K::ContentSignature(merged) != K::ContentSignature(*baseline);
    if (content_changed) {
      steps.push_back({identity, OwnedStepKind::kUpdate, std::nullopt,
                       std::nullopt, payload});
      capture_signature();
    }
    if (!moved && !content_changed && cursor && payload != cursor->reconciled) {
      plan.rebaselined[identity] = payload;
    }
  }

  // 6. This page's tombstones, children first.
  std::map<std::string, std::string> delete_parent_of;
  for (const std::string& identity : context.tombstoned_identities) {
    std::optional<Entity> entity;
    if (const auto slot = slot_of.find(identity); slot != slot_of.end()) {
      entity = working[slot->second].entity;
    }
    if (!entity) {
      entity = baseline_of(identity);
    }
    if (!entity) {
      continue;
    }
    for (const std::string& owner : K::OwnerUuids(*entity)) {
      if (context.tombstoned_identities.count(owner) ||
          table.cursors.count(owner)) {
        delete_parent_of[identity] = owner;
        break;
      }
    }
  }
  std::vector<std::string> tombstoned(context.tombstoned_identities.begin(),
                                      context.tombstoned_identities.end());
  const std::map<std::string, int> depths =
      OwnedDepths(tombstoned, delete_parent_of);
  std::sort(tombstoned.begin(), tombstoned.end(),
            [&](const std::string& a, const std::string& b) {
              const int left = depths.at(a);
              const int right = depths.at(b);
              return left == right ? a < b : left > right;
            });
  for (const std::string& identity : tombstoned) {
    // α: an unpublished local edit beats the inbound delete (C4); with no
    // merge partner (bookmarks, pins) the row yields.
    if (K::kTombstoneYieldsToLocalEdits &&
        (context.pending_local_edits.count(identity) ||
         context.unpublished.count(identity))) {
      const auto partner = context.merge_partners.find(identity);
      if (partner != context.merge_partners.end()) {
        // A quiescent partner W takes X's frozen local projection; the
        // delete still follows in the fourth phase.
        std::optional<RuleProjection> source;
        if (const std::optional<Entity> projected = projection_of(identity)) {
          source = K::TransferSource(*projected, resolve);
        }
        if (!source) {
          plan.yielded_tombstones.insert(identity);
          continue;
        }
        OwnedApplyStep step;
        step.identity = identity;
        step.kind = OwnedStepKind::kTransfer;
        step.transfer_source = std::move(source);
        step.transfer_to = partner->second;
        steps.push_back(std::move(step));
      } else if (context.partner_not_at_rest.count(identity)) {
        plan.parked_tombstones.insert(identity);
        continue;
      } else {
        plan.yielded_tombstones.insert(identity);
        continue;
      }
    }
    steps.push_back({identity, OwnedStepKind::kDelete, std::nullopt,
                     std::nullopt, std::nullopt});
  }

  // 7. Stable phase sort.
  std::stable_sort(steps.begin(), steps.end(),
                   [](const OwnedApplyStep& a, const OwnedApplyStep& b) {
                     return OwnedStepPhase(a.kind) < OwnedStepPhase(b.kind);
                   });
  plan.steps = std::move(steps);
  return plan;
}

}  // namespace phi_client::sync

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_OWNED_H_

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_sync_pins.h"

#include <algorithm>
#include <utility>

#include "phibrowser-win/client/phi_sync_merge.h"

namespace phi_client::sync {

namespace {

constexpr char kDefaultProfileId[] = "Default";
constexpr char kPlaceholderUrl[] = "https://pin.phi/placeholder";

int PinOpPhase(const PinApplyOp& op) {
  switch (op.kind) {
    case PinApplyOp::Kind::kCreate:
    case PinApplyOp::Kind::kRelineage:
    case PinApplyOp::Kind::kMove:
      return 1;
    case PinApplyOp::Kind::kUpdate:
      return 2;
    case PinApplyOp::Kind::kDelete:
      return 3;
  }
  return 3;
}

PinApplyOp DeleteOp(const std::string& guid) {
  PinApplyOp op;
  op.kind = PinApplyOp::Kind::kDelete;
  op.guid = guid;
  return op;
}

PinApplyOp MoveOp(const std::string& guid, int index) {
  PinApplyOp op;
  op.kind = PinApplyOp::Kind::kMove;
  op.guid = guid;
  op.index = index;
  return op;
}

PinApplyOp UpdateOp(const std::string& guid, PinFieldPatch fields) {
  PinApplyOp op;
  op.kind = PinApplyOp::Kind::kUpdate;
  op.guid = guid;
  op.fields = std::move(fields);
  return op;
}

}  // namespace

std::vector<PinApplyOp> OrderPinOps(std::vector<PinApplyOp> ops) {
  std::stable_sort(ops.begin(), ops.end(),
                   [](const PinApplyOp& a, const PinApplyOp& b) {
                     return PinOpPhase(a) < PinOpPhase(b);
                   });
  return ops;
}

std::vector<PinApplyOp> NormalizePinVariants(const std::vector<LocalPin>& locals) {
  std::map<std::string, std::vector<LocalPin>> groups;
  for (const LocalPin& local : locals) {
    if (local.is_dormant) {
      continue;
    }
    groups[PinKind::LineageKey(local.lineage_id) + '\0' +
           PinKind::LocalOwnerKey(local)]
        .push_back(local);
  }
  std::vector<PinApplyOp> ops;
  for (auto& [key, members] : groups) {  // sorted: deterministic ops
    std::sort(members.begin(), members.end(),
              [](const LocalPin& a, const LocalPin& b) {
                return a.index == b.index ? a.guid < b.guid : a.index < b.index;
              });
    // 1. Exact duplicates collapse first (never counted as variants).
    std::vector<LocalPin> survivors;
    std::set<std::string> seen;
    for (const LocalPin& row : members) {
      const std::string signature =
          row.title + '\0' + row.url + '\0' +
          (row.split_partner_lineage_id
               ? "1" + PinKind::LineageKey(*row.split_partner_lineage_id)
               : std::string("0"));
      if (!seen.insert(signature).second) {
        ops.push_back(DeleteOp(row.guid));
        continue;
      }
      survivors.push_back(row);
    }
    // 2. Ordinal 0 keeps its lineage; the rest are reminted.
    for (size_t ordinal = 1; ordinal < survivors.size(); ++ordinal) {
      PinApplyOp op;
      op.kind = PinApplyOp::Kind::kRelineage;
      op.guid = survivors[ordinal].guid;
      op.new_lineage_id = PinKind::MintedLineage(
          PinKind::LineageKey(survivors[ordinal].lineage_id),
          static_cast<int>(ordinal));
      ops.push_back(std::move(op));
    }
  }
  return ops;
}

// --- The kind -----------------------------------------------------------------------------

PinOwnedKind::PinOwnedKind(PinLocalAccess& access,
                           OwnedTableStore& store,
                           std::function<std::string()> new_guid)
    : access_(access), store_(store), new_guid_(std::move(new_guid)) {}

PinOwnedKind::~PinOwnedKind() = default;

const char* PinOwnedKind::label() const {
  return "pins";
}

const char* PinOwnedKind::entity_name() const {
  return kPinEntityName;
}

OwnedKindId PinOwnedKind::kind_id() const {
  return OwnedKindId::kPins;
}

OwnedTableStore& PinOwnedKind::store() {
  return store_;
}

bool PinOwnedKind::tombstone_yields_to_local_edits() const {
  return PinKind::kTombstoneYieldsToLocalEdits;
}

std::optional<std::string> PinOwnedKind::Identity(const PhiEntity& envelope) const {
  const std::optional<PinTabEntity> entity = PinKind::FromEnvelope(envelope);
  if (!entity) {
    return std::nullopt;
  }
  const std::string identity = PinKind::Identity(*entity);
  return identity.empty() ? std::nullopt : std::optional<std::string>(identity);
}

std::string PinOwnedKind::ClientTag(const std::string& identity) const {
  return PinKind::ClientTag(identity);
}

std::vector<std::string> PinOwnedKind::Owners(const std::string& payload) const {
  const std::optional<PinTabEntity> entity = DecodeOwned<PinKind>(payload);
  return entity ? PinKind::OwnerUuids(*entity) : std::vector<std::string>();
}

bool PinOwnedKind::clears_split_partners() const {
  return true;
}

// §7.4: a baseline whose split link this device no longer carries reads as
// unlinked (stamped now), so the unlink publishes.
std::map<std::string, std::string> PinOwnedKind::ClearedSplitPartners(
    const std::map<std::string, std::string>& baselines,
    int64_t now,
    const OwnedOwnerMaps& maps) {
  const OwnerResolver resolve = maps.Resolver();
  std::map<std::string, std::string> out;
  for (const auto& [identity, bytes] : baselines) {
    std::optional<PinTabEntity> entity = DecodeOwned<PinKind>(bytes);
    if (!entity || entity->split_partner_uuid.get().str().empty() ||
        StillCarriesSplitLink(PinKind::Identity(*entity), resolve)) {
      continue;
    }
    SettingValue cleared;
    cleared.SetString(std::string());
    cleared.updated_at_ms = now;
    entity->split_partner_uuid.set(cleared);
    out[identity] = EncodeOwned<PinKind>(*entity);
  }
  return out;
}

void PinOwnedKind::Reload(const std::vector<LocalPin>& rows,
                          const std::string& local_scope,
                          const std::optional<std::string>& account_scope,
                          uint64_t scope_generation) {
  locals_ = rows;
  local_scope_ = local_scope;
  account_scope_ = account_scope;
  scope_generation_ = scope_generation;
  scope_moved_mid_round_ = false;
}

void PinOwnedKind::RescanScopes() {
  // All three, so a round trip X → Y → X since the sample still blocks.
  if (access_.CurrentScope() != local_scope_ ||
      access_.AccountScope() != account_scope_ ||
      access_.ScopeGeneration() != scope_generation_) {
    scope_moved_mid_round_ = true;
  }
}

bool PinOwnedKind::CommitFenceHolds() {
  RescanScopes();
  return !scope_blocked();
}

bool PinOwnedKind::scope_blocked() const {
  return scope_moved_mid_round_ ||
         (account_scope_ && *account_scope_ != local_scope_);
}

void PinOwnedKind::RefreshLandedRows(const std::vector<LocalPin>& rows) {
  locals_ = rows;
}

void PinOwnedKind::NoteDeletedRows(const std::set<std::string>& guids) {
  std::erase_if(locals_,
                [&](const LocalPin& row) { return guids.count(row.guid) > 0; });
}

void PinOwnedKind::NoteSplitPartnerWrites(
    const std::map<std::string, std::string>& linked,
    const std::set<std::string>& cleared) {
  for (LocalPin& row : locals_) {
    if (const auto it = linked.find(row.guid); it != linked.end()) {
      row.split_partner_lineage_id = it->second;
    } else if (cleared.count(row.guid)) {
      row.split_partner_lineage_id.reset();
    }
  }
}

bool PinOwnedKind::StillCarriesSplitLink(const std::string& identity,
                                         const OwnerResolver& resolve) const {
  for (const LocalPin& row : locals_) {
    if (row.split_partner_lineage_id &&
        PinKind::LocalIdentity(row, resolve, local_scope_) == identity) {
      return true;
    }
  }
  return false;
}

bool PinOwnedKind::BeginRound() {
  std::vector<LocalPin> rows;
  // The generation and scopes BEFORE the rows (the Mac's sample order): a
  // migration between the two reads shows as a changed probe, never as
  // rows of one scope under another's sample.
  const uint64_t generation = access_.ScopeGeneration();
  const std::string local_scope = access_.CurrentScope();
  const std::optional<std::string> account_scope = access_.AccountScope();
  if (!access_.AllPins(&rows)) {
    Reload({}, local_scope, account_scope, generation);
    return false;
  }
  Reload(rows, local_scope, account_scope, generation);
  return true;
}

// Pin tags seed from cursor keys only: a raw lineage cannot build an
// account tag without the round's owner maps.
std::set<std::string> PinOwnedKind::LocalIdentities() {
  return {};
}

OwnedSnapshotBytes PinOwnedKind::Snapshot(const OwnedTable& table,
                                          const OwnedOwnerMaps& maps,
                                          int64_t now,
                                          int64_t hlc_max,
                                          int64_t wall_offset_ms) {
  OwnedSnapshotBytes out;
  RescanScopes();
  if (scope_blocked()) {
    out.scope_mismatch = true;  // §7.3: nothing publishes this round
    return out;
  }
  const OwnerResolver resolve = maps.Resolver();
  const OwnedSnapshotResult<PinTabEntity> result = OwnedSnapshot<PinKind>(
      locals_, table, resolve, local_scope_, now, hlc_max, wall_offset_ms);
  out.skipped_unmapped_owner = result.skipped_unmapped_owner;
  out.skipped_ineligible_owner = result.skipped_ineligible_owner;
  for (const auto& [identity, entity] : result.entities) {
    out.entities[identity] = EncodeOwned<PinKind>(entity);
  }
  for (const LocalPin& row : locals_) {
    const std::optional<std::string> identity =
        PinKind::LocalIdentity(row, resolve, local_scope_);
    const std::optional<std::string> owner =
        PinKind::EligibilityOwner(row, resolve, local_scope_);
    if (identity && owner) {
      out.owner_uuids[*identity] = *owner;
    }
  }
  return out;
}

// The domain: this round's rows plus every other non-dormant row (each
// protecting only its own lineage/owner identity).
bool PinOwnedKind::Tombstones(const OwnedTable& table,
                              const OwnedOwnerMaps& maps,
                              int64_t now,
                              OwnedTombstoneResult* out) {
  RescanScopes();
  if (scope_blocked()) {
    *out = OwnedTombstoneResult();
    return true;
  }
  std::vector<LocalPin> full;
  if (!access_.AllPinRows(&full)) {
    return false;
  }
  std::vector<LocalPin> domain = locals_;
  std::set<std::string> claimed;
  for (const LocalPin& row : locals_) {
    claimed.insert(row.guid);
  }
  for (const LocalPin& row : full) {
    if (!claimed.count(row.guid)) {
      domain.push_back(row);
    }
  }
  *out = OwnedTombstones<PinKind>(domain, table, maps.Resolver(), local_scope_,
                                  now);
  return true;
}

OwnedParkedClaimResult PinOwnedKind::RetryParkedClaims(
    const std::map<std::string, ParkedOwnedItem>&,
    const OwnedOwnerMaps&,
    int64_t) {
  return {};  // pins never adopt (§6.7)
}

std::map<std::string, std::string> PinOwnedKind::LocalProjections(
    const std::set<std::string>& identities,
    const OwnedTable& table,
    const OwnerResolver& resolve,
    int64_t now,
    int64_t wall_offset_ms) const {
  std::map<std::string, std::string> out;
  if (identities.empty()) {
    return out;
  }
  std::map<std::string, const LocalPin*> row_of;
  for (const LocalPin& row : locals_) {
    const std::optional<std::string> identity =
        PinKind::LocalIdentity(row, resolve, local_scope_);
    if (identity && identities.count(*identity) && !row_of.count(*identity)) {
      row_of[*identity] = &row;
    }
  }
  for (const auto& [identity, row] : row_of) {
    const auto cursor = table.cursors.find(identity);
    if (cursor == table.cursors.end()) {
      continue;
    }
    const std::optional<PinTabEntity> baseline =
        DecodeOwned<PinKind>(cursor->second.reconciled);
    const std::optional<PinTabEntity> projected =
        PinKind::Project(*row, resolve, local_scope_, std::nullopt);
    if (!baseline || !projected) {
      continue;
    }
    out[identity] = EncodeOwned<PinKind>(
        PinKind::Stamp(*projected, baseline, *row, PinKind::Rank(*baseline),
                       now, 0, wall_offset_ms));
  }
  return out;
}

OwnedPlanOutput PinOwnedKind::Plan(const OwnedPlanInput& input) {
  OwnedPlanOutput out;
  RescanScopes();
  const OwnerResolver resolve = input.maps.Resolver();
  std::vector<OwnedArrival<PinTabEntity>> arrivals;
  std::set<std::string> domain = input.tombstoned;
  for (const auto& [identity, item] : input.parked) {
    domain.insert(identity);
  }
  for (const OwnedPlanInput::Arrival& item : input.arrivals) {
    std::optional<PinTabEntity> entity = DecodeOwned<PinKind>(item.payload);
    if (!entity) {
      continue;
    }
    const std::string identity = PinKind::Identity(*entity);
    out.server_bytes[identity] = item.payload;
    domain.insert(identity);
    arrivals.push_back({std::move(*entity), item.entity_id, item.version});
  }
  OwnedPlanContext context;
  context.tombstoned_identities = input.tombstoned;
  context.local_projections = LocalProjections(
      domain, input.table, resolve, input.now, input.wall_offset_ms);
  context.pending_local_edits =
      OwnedUnpublishedEdits<PinKind>(context.local_projections, input.table);
  context.unpublished =
      OwnedUnpublishedMerges(context.local_projections, input.table);
  std::set<std::string> live;
  for (const LocalPin& row : locals_) {
    if (std::optional<std::string> identity =
            PinKind::LocalIdentity(row, resolve, local_scope_)) {
      live.insert(*identity);
    }
  }
  for (const std::string& identity : input.tombstoned) {
    const auto cursor = input.table.cursors.find(identity);
    if (live.count(identity) && cursor != input.table.cursors.end() &&
        cursor->second.deleted_at_ms && !cursor->second.reconciled) {
      context.pending_local_edits.insert(identity);
    }
  }
  context.local_scope = local_scope_;
  context.account_scope = account_scope_;
  context.scope_moved_mid_round = scope_moved_mid_round_;
  out.plan = OwnedPlanFor<PinKind>(arrivals, input.parked, input.table, resolve,
                                   context);
  out.scope_mismatch = context.scope_mismatch();
  out.must_republish = out.plan.must_republish;
  return out;
}

OwnedLandingOutcome PinOwnedKind::Land(const OwnedLandingInput& input) {
  OwnedLandingOutcome outcome;
  const OwnerResolver resolve = input.maps.Resolver();
  RescanScopes();
  if (scope_blocked()) {
    for (const OwnedApplyStep& step : input.steps) {
      outcome.parked.insert(step.identity);
    }
    return outcome;
  }
  // A11's variant normalization rides the same transaction.
  std::vector<PinApplyOp> ops = NormalizePinVariants(locals_);
  int relineaged = 0;
  for (const PinApplyOp& op : ops) {
    if (op.kind == PinApplyOp::Kind::kRelineage) {
      ++relineaged;
    }
  }
  if (input.steps.empty() && ops.empty()) {
    return outcome;
  }

  // The first row per identity (the snapshot's survivor).
  std::map<std::string, LocalPin> row_of;
  for (const LocalPin& row : locals_) {
    if (std::optional<std::string> identity =
            PinKind::LocalIdentity(row, resolve, local_scope_)) {
      row_of.try_emplace(*identity, row);
    }
  }
  // An account owner key → the local owner, only in the matching scope.
  auto local_owner = [&](const std::string& owner_key)
      -> std::optional<std::pair<std::optional<std::string>,
                                 std::optional<std::string>>> {
    if (owner_key == kAppOwnerKey) {
      if (local_scope_ != "app") {
        return std::nullopt;
      }
      return std::make_pair(std::optional<std::string>(),
                            std::optional<std::string>());
    }
    if (std::optional<std::string> space_id = resolve.local_space_id(owner_key)) {
      if (local_scope_ != "space") {
        return std::nullopt;
      }
      std::optional<std::string> profile =
          resolve.local_profile_id_for_space(*space_id);
      if (!profile) {
        for (const LocalPin& row : locals_) {
          if (row.space_id == space_id && row.profile_id) {
            profile = row.profile_id;
            break;
          }
        }
      }
      return std::make_pair(space_id, std::optional<std::string>(
                                          profile.value_or(kDefaultProfileId)));
    }
    if (std::optional<std::string> profile_id = resolve.local_profile_id(owner_key)) {
      if (local_scope_ != "profile") {
        return std::nullopt;
      }
      return std::make_pair(std::optional<std::string>(), profile_id);
    }
    return std::nullopt;
  };
  // Whether the owner key resolves locally at all, ignoring the current
  // scope. True with a nullopt local_owner means the owner shape belongs to
  // another pinned-tab scope (a dormant copy).
  auto owner_resolves_in_any_scope = [&](const std::string& owner_key) {
    return owner_key == kAppOwnerKey ||
           resolve.local_space_id(owner_key).has_value() ||
           resolve.local_profile_id(owner_key).has_value();
  };

  struct Planned {
    OwnedApplyStep step;
    std::optional<PinTabEntity> entity;
    std::string identity;
    std::string owner_key;
  };
  std::vector<Planned> planned;
  for (const OwnedApplyStep& step : input.steps) {
    if (step.kind == OwnedStepKind::kClaim) {
      continue;  // pins never adopt
    }
    planned.push_back({step,
                       step.payload ? DecodeOwned<PinKind>(*step.payload)
                                    : std::nullopt,
                       step.identity,
                       PinKind::IdentityHalves(step.identity).second});
  }

  std::map<std::string, LocalPin> projected;
  std::set<std::string> original_guids;
  for (const LocalPin& row : locals_) {
    projected[row.guid] = row;
    original_guids.insert(row.guid);
  }
  std::map<std::string, std::string> guid_of;
  for (const auto& [identity, row] : row_of) {
    guid_of[identity] = row.guid;
  }
  std::map<std::string, std::string> rank_of;
  for (const auto& [identity, cursor] : input.table.cursors) {
    if (std::optional<PinTabEntity> entity =
            DecodeOwned<PinKind>(cursor.reconciled)) {
      rank_of[identity] = PinKind::Rank(*entity);
    }
  }
  std::set<std::string> created;
  std::set<std::string> deleted_identities;
  std::map<std::string, std::string> payload_of;
  std::set<std::string> touched_owners;
  std::map<std::string, std::string> owner_of_guid;
  for (const auto& [identity, row] : row_of) {
    owner_of_guid[row.guid] =
        PinKind::EligibilityOwner(row, resolve, local_scope_)
            .value_or(PinKind::IdentityHalves(identity).second);
  }

  for (const Planned& item : planned) {
    const std::string& identity = item.identity;
    if (item.step.payload) {
      payload_of[identity] = *item.step.payload;
    }
    if (item.step.new_rank) {
      rank_of[identity] = *item.step.new_rank;
    }
    if (item.step.kind != OwnedStepKind::kUpdate) {
      touched_owners.insert(item.owner_key);
    }
    if (item.step.kind == OwnedStepKind::kDelete) {
      deleted_identities.insert(identity);
      if (const auto guid = guid_of.find(identity); guid != guid_of.end()) {
        projected.erase(guid->second);
      }
      continue;
    }
    if (!item.entity || guid_of.count(identity)) {
      continue;
    }
    const auto owner = local_owner(item.owner_key);
    if (!owner) {
      outcome.parked.insert(identity);
      if (owner_resolves_in_any_scope(item.owner_key)) {
        outcome.parked_for_scope.insert(identity);
      }
      continue;
    }
    // One row per (lineage, local owner): a migrated row is updated next
    // round, never duplicated.
    const std::string lineage =
        PinKind::LineageKey(PinKind::IdentityHalves(identity).first);
    const std::string owner_key = owner->first     ? *owner->first
                                  : owner->second  ? *owner->second
                                                   : std::string(kAppOwnerKey);
    const bool owner_has_lineage = std::any_of(
        projected.begin(), projected.end(), [&](const auto& entry) {
          return PinKind::LineageKey(entry.second.lineage_id) == lineage &&
                 PinKind::LocalOwnerKey(entry.second) == owner_key;
        });
    if (owner_has_lineage) {
      outcome.parked.insert(identity);
      continue;
    }
    const std::string guid = new_guid_();
    guid_of[identity] = guid;
    created.insert(identity);
    owner_of_guid[guid] = item.owner_key;
    LocalPin row;
    row.lineage_id = PinKind::IdentityHalves(identity).first;
    row.guid = guid;
    row.space_id = owner->first;
    row.profile_id = owner->second;
    row.title = item.entity->title.get().str();
    row.url = item.entity->url.get().str().empty() ? kPlaceholderUrl
                                                   : item.entity->url.get().str();
    row.source = item.entity->source;
    row.created_ms = item.entity->created_at_ms;
    // The incoming content stamp (R-exec-5), not the landing time.
    row.content_updated_ms = std::max(item.entity->title.get().updated_at_ms,
                                      item.entity->url.get().updated_at_ms);
    projected[guid] = row;
  }

  // Ranks → a dense permutation per touched owner.
  std::map<std::string, std::string> rank_by_guid;
  for (const auto& [identity, guid] : guid_of) {
    if (const auto rank = rank_of.find(identity); rank != rank_of.end()) {
      rank_by_guid[guid] = rank->second;
    }
  }
  std::map<std::string, int> index_of;
  for (const std::string& owner : touched_owners) {
    std::vector<const LocalPin*> members;
    for (const auto& [guid, row] : projected) {
      const auto it = owner_of_guid.find(guid);
      if (it != owner_of_guid.end() && it->second == owner) {
        members.push_back(&row);
      }
    }
    std::sort(members.begin(), members.end(),
              [&](const LocalPin* a, const LocalPin* b) {
                const auto left = rank_by_guid.find(a->guid);
                const auto right = rank_by_guid.find(b->guid);
                const bool has_left = left != rank_by_guid.end();
                const bool has_right = right != rank_by_guid.end();
                if (has_left && has_right) {
                  // Ties break on the shared lineage, never a local guid.
                  return left->second == right->second
                             ? PinKind::LineageKey(a->lineage_id) <
                                   PinKind::LineageKey(b->lineage_id)
                             : left->second < right->second;
                }
                if (has_left != has_right) {
                  return has_left;  // unranked rows last
                }
                return a->index == b->index ? a->guid < b->guid
                                            : a->index < b->index;
              });
    for (size_t position = 0; position < members.size(); ++position) {
      index_of[members[position]->guid] = static_cast<int>(position);
    }
  }
  auto index_for = [&](const std::string& guid, int fallback) {
    const auto it = index_of.find(guid);
    return it == index_of.end() ? fallback : it->second;
  };

  // Phase 1: create and move.
  std::set<std::string> indexed;
  std::set<std::string> emitted_creates;
  for (const Planned& item : planned) {
    if (item.step.kind == OwnedStepKind::kDelete ||
        outcome.parked.count(item.identity)) {
      continue;
    }
    const auto guid = guid_of.find(item.identity);
    if (guid == guid_of.end()) {
      continue;
    }
    if (created.count(item.identity)) {
      if (!emitted_creates.insert(item.identity).second) {
        continue;  // one create per identity
      }
      LocalPin row = projected[guid->second];
      row.index = index_for(guid->second, 0);
      projected[guid->second] = row;
      PinApplyOp op;
      op.kind = PinApplyOp::Kind::kCreate;
      op.guid = row.guid;
      op.row = row;
      ops.push_back(std::move(op));
      indexed.insert(guid->second);
    } else if (item.step.kind == OwnedStepKind::kCreate ||
               item.step.kind == OwnedStepKind::kMove) {
      ops.push_back(MoveOp(guid->second, index_for(guid->second, 0)));
      indexed.insert(guid->second);
    }
  }
  for (const std::string& owner : touched_owners) {
    std::vector<const LocalPin*> movers;
    for (const auto& [guid, row] : projected) {
      const auto it = owner_of_guid.find(guid);
      if (it != owner_of_guid.end() && it->second == owner &&
          !indexed.count(guid) && original_guids.count(guid)) {
        movers.push_back(&row);
      }
    }
    std::sort(movers.begin(), movers.end(),
              [&](const LocalPin* a, const LocalPin* b) {
                const int left = index_for(a->guid, 0);
                const int right = index_for(b->guid, 0);
                return left == right ? a->guid < b->guid : left < right;
              });
    for (const LocalPin* row : movers) {
      ops.push_back(MoveOp(row->guid, index_for(row->guid, row->index)));
    }
  }

  // Phase 2: content and split partners (§7.4).
  std::vector<PinApplyOp> reverse_links;
  std::map<std::string, std::string> linked_partners;
  std::set<std::string> cleared_partners;
  for (const Planned& item : planned) {
    if (item.step.kind == OwnedStepKind::kDelete || !item.entity ||
        outcome.parked.count(item.identity)) {
      continue;
    }
    const auto guid_it = guid_of.find(item.identity);
    if (guid_it == guid_of.end()) {
      continue;
    }
    const std::string guid = guid_it->second;
    PinFieldPatch fields;
    if (!created.count(item.identity)) {
      fields.title = item.entity->title.get().str();
      const std::string url = item.entity->url.get().str();
      if (!url.empty()) {
        fields.url = url;
      }
    }
    const std::string partner =
        PinKind::LineageKey(item.entity->split_partner_uuid.get().str());
    if (partner.empty()) {
      bool linked_now = false;
      for (const LocalPin& row : locals_) {
        if (row.guid == guid && row.split_partner_lineage_id) {
          linked_now = true;
        }
      }
      if (linked_now) {
        fields.split_partner_lineage_id = std::optional<std::string>();
        cleared_partners.insert(guid);
      }
      outcome.pending_partner_lineages[item.identity] = std::string();
    } else {
      const LocalPin* partner_row = nullptr;
      for (const auto& [other_guid, row] : projected) {
        const auto owner = owner_of_guid.find(other_guid);
        if (other_guid != guid && owner != owner_of_guid.end() &&
            owner->second == item.owner_key &&
            PinKind::LineageKey(row.lineage_id) == partner) {
          partner_row = &row;
          break;
        }
      }
      fields.split_partner_lineage_id = std::optional<std::string>(partner);
      linked_partners[guid] = partner;
      outcome.pending_partner_lineages[item.identity] =
          partner_row ? std::string() : partner;
      const std::string this_lineage =
          PinKind::LineageKey(PinKind::IdentityHalves(item.identity).first);
      if (partner_row) {
        const std::string partner_identity =
            PinKind::LineageKey(partner_row->lineage_id) + ":" + item.owner_key;
        const auto cursor = input.table.cursors.find(partner_identity);
        if (cursor != input.table.cursors.end() &&
            cursor->second.pending_partner_lineage ==
                std::optional<std::string>(this_lineage) &&
            PinKind::LineageKey(
                partner_row->split_partner_lineage_id.value_or(std::string())) !=
                this_lineage) {
          PinFieldPatch reverse;
          reverse.split_partner_lineage_id =
              std::optional<std::string>(this_lineage);
          reverse_links.push_back(UpdateOp(partner_row->guid, reverse));
          linked_partners[partner_row->guid] = this_lineage;
          outcome.pending_partner_lineages[partner_identity] = std::string();
        }
      }
    }
    if (!fields.title && !fields.url && !fields.split_partner_lineage_id) {
      continue;
    }
    ops.push_back(UpdateOp(guid, std::move(fields)));
  }
  ops.insert(ops.end(), reverse_links.begin(), reverse_links.end());

  // Phase 3: deletes (a known identity with no row just finalizes).
  for (const Planned& item : planned) {
    if (item.step.kind != OwnedStepKind::kDelete) {
      continue;
    }
    const auto guid = guid_of.find(item.identity);
    if (guid == guid_of.end() || !original_guids.count(guid->second)) {
      outcome.landed.insert(item.identity);
      outcome.deleted.insert(item.identity);
      continue;
    }
    ops.push_back(DeleteOp(guid->second));
  }

  if (ops.empty()) {
    return outcome;
  }
  std::set<std::string> identities;
  for (const Planned& item : planned) {
    if (!outcome.parked.count(item.identity) &&
        !outcome.landed.count(item.identity)) {
      identities.insert(item.identity);
    }
  }
  const PinApplyResult result = access_.Apply(OrderPinOps(std::move(ops)));
  if (result != PinApplyResult::kOk) {
    (result == PinApplyResult::kRefused ? outcome.refused : outcome.parked)
        .insert(identities.begin(), identities.end());
    outcome.pending_partner_lineages.clear();
    return outcome;
  }
  outcome.relineaged = relineaged;
  NoteSplitPartnerWrites(linked_partners, cleared_partners);

  for (const std::string& identity : identities) {
    const auto [lineage, owner_key] = PinKind::IdentityHalves(identity);
    const auto owner = local_owner(owner_key);
    std::optional<std::string> local_owner_key;
    if (owner) {
      local_owner_key = owner->first    ? *owner->first
                        : owner->second ? *owner->second
                                        : std::string(kAppOwnerKey);
    }
    const bool known = access_.IsKnownLocalPin(lineage, local_owner_key);
    if (deleted_identities.count(identity)) {
      if (known) {
        outcome.parked.insert(identity);
      } else {
        outcome.landed.insert(identity);
        outcome.deleted.insert(identity);
      }
      continue;
    }
    if (!known) {
      outcome.parked.insert(identity);
      continue;
    }
    outcome.landed.insert(identity);
    if (const auto payload = payload_of.find(identity);
        payload != payload_of.end()) {
      outcome.reconciled[identity] = payload->second;
    }
  }
  for (const std::string& identity : outcome.landed) {
    outcome.parked.erase(identity);
    outcome.refused.erase(identity);
  }
  std::set<std::string> gone;
  for (const std::string& identity : outcome.deleted) {
    if (const auto guid = guid_of.find(identity); guid != guid_of.end()) {
      gone.insert(guid->second);
    }
  }
  NoteDeletedRows(gone);
  std::vector<LocalPin> refreshed;
  if (access_.CachedPins(&refreshed)) {
    RefreshLandedRows(refreshed);
  }
  return outcome;
}

std::set<std::string> PinOwnedKind::ClaimIdentities(
    const std::map<std::string, std::string>&) {
  return {};  // pins never mint
}

bool PinOwnedKind::LiveOwners(const std::set<std::string>& candidates,
                              const OwnedOwnerMaps& maps,
                              OwnedLiveRows* out) {
  std::vector<LocalPin> full;
  if (!access_.AllPinRows(&full)) {
    return false;
  }
  const OwnerResolver resolve = maps.Resolver();
  std::map<std::string, std::string> owner_by_identity;
  std::set<std::string> claimed_guids;
  for (const LocalPin& row : locals_) {
    claimed_guids.insert(row.guid);
    const std::optional<std::string> identity =
        PinKind::LocalIdentity(row, resolve, local_scope_);
    const std::optional<std::string> owner =
        PinKind::EligibilityOwner(row, resolve, local_scope_);
    if (identity && owner) {
      owner_by_identity[*identity] = *owner;
    }
  }
  std::set<std::string> out_of_scope;
  std::set<std::string> unresolved_lineages;
  for (const LocalPin& row : full) {
    if (claimed_guids.count(row.guid)) {
      continue;
    }
    const std::optional<std::string> identity =
        PinKind::LocalIdentity(row, resolve, local_scope_);
    if (!identity) {
      continue;
    }
    if (!PinKind::EligibilityOwner(row, resolve, local_scope_)) {
      unresolved_lineages.insert(PinKind::LineageKey(row.lineage_id));
      continue;
    }
    out_of_scope.insert(*identity);
  }
  for (const std::string& identity : candidates) {
    if (const auto owner = owner_by_identity.find(identity);
        owner != owner_by_identity.end()) {
      out->claimed.insert(identity);
      out->owners[identity] = owner->second;
      continue;
    }
    if (out_of_scope.count(identity) ||
        unresolved_lineages.count(PinKind::IdentityHalves(identity).first)) {
      out->claimed.insert(identity);
    }
  }
  return true;
}

}  // namespace phi_client::sync

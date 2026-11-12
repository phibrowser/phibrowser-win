// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_sync_bookmarks.h"

#include <algorithm>
#include <tuple>
#include <utility>

#include "client/phi_sync_merge.h"

namespace phi_client::sync {

namespace {

constexpr char kDefaultProfileId[] = "Default";
constexpr char kFolderPlaceholderUrl[] = "https://bookmark.phi/folder";

struct SiblingGroup {
  std::string space_id;
  std::optional<std::string> parent_guid;
  bool operator<(const SiblingGroup& other) const {
    return std::tie(space_id, parent_guid) <
           std::tie(other.space_id, other.parent_guid);
  }
  bool operator==(const SiblingGroup& other) const = default;
};

std::string TargetGuid(const BookmarkApplyOp& op) {
  return op.kind == BookmarkApplyOp::Kind::kCreate ? op.row.guid : op.guid;
}

int OpPhase(const BookmarkApplyOp& op) {
  switch (op.kind) {
    case BookmarkApplyOp::Kind::kClaim:
    case BookmarkApplyOp::Kind::kCreate:
    case BookmarkApplyOp::Kind::kMove:
      return 1;
    case BookmarkApplyOp::Kind::kUpdate:
      return 2;
    case BookmarkApplyOp::Kind::kDelete:
      return 3;
  }
  return 3;
}

BookmarkFieldPatch PatchFor(const BookmarkEntity& entity) {
  BookmarkFieldPatch patch;
  patch.title = entity.title.get().str();
  // An empty url is "unchanged": a bookmark never loses its URL.
  const std::string url = entity.url.get().str();
  if (!url.empty()) {
    patch.url = url;
  }
  const std::string secondary_url = entity.secondary_url.get().str();
  patch.secondary_url = secondary_url.empty()
                            ? std::optional<std::string>()
                            : std::optional<std::string>(secondary_url);
  const std::string secondary_title = entity.secondary_title.get().str();
  patch.secondary_title = secondary_title.empty()
                              ? std::optional<std::string>()
                              : std::optional<std::string>(secondary_title);
  return patch;
}

// pairWithinGroups: one-to-one within a level, local index order against
// remote rank order. Returns the unmatched arrivals.
int PairWithinGroups(
    const std::vector<BookmarkEntity>& remote,
    const std::vector<LocalBookmark>& local,
    const std::function<std::string(const BookmarkEntity&)>& remote_key,
    const std::function<std::string(const LocalBookmark&)>& local_key,
    const std::function<void(const BookmarkEntity&, const LocalBookmark&)>&
        pair) {
  std::map<std::string, std::vector<BookmarkEntity>> remote_groups;
  for (const BookmarkEntity& entity : remote) {
    remote_groups[remote_key(entity)].push_back(entity);
  }
  std::map<std::string, std::vector<LocalBookmark>> local_groups;
  for (const LocalBookmark& row : local) {
    local_groups[local_key(row)].push_back(row);
  }
  int left_over = 0;
  for (auto& [key, entities] : remote_groups) {
    std::sort(entities.begin(), entities.end(),
              [](const BookmarkEntity& a, const BookmarkEntity& b) {
                const std::string left = a.rank.get().str();
                const std::string right = b.rank.get().str();
                return left == right ? a.bookmark_uuid < b.bookmark_uuid
                                     : left < right;
              });
    std::vector<LocalBookmark> ordered = local_groups[key];
    std::sort(ordered.begin(), ordered.end(),
              [](const LocalBookmark& a, const LocalBookmark& b) {
                return a.index == b.index ? a.guid < b.guid : a.index < b.index;
              });
    for (size_t offset = 0; offset < entities.size(); ++offset) {
      if (offset < ordered.size()) {
        pair(entities[offset], ordered[offset]);
      } else {
        ++left_over;
      }
    }
  }
  return left_over;
}

}  // namespace

std::vector<BookmarkApplyOp> OrderBookmarkOps(
    std::vector<BookmarkApplyOp> ops,
    const std::map<std::string, std::string>& parent_of) {
  std::vector<std::string> targets;
  for (const BookmarkApplyOp& op : ops) {
    targets.push_back(TargetGuid(op));
  }
  const std::map<std::string, int> depths = OwnedDepths(targets, parent_of);
  std::vector<size_t> order(ops.size());
  for (size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    const int left_phase = OpPhase(ops[a]);
    const int right_phase = OpPhase(ops[b]);
    if (left_phase != right_phase) {
      return left_phase < right_phase;
    }
    const int left = depths.at(TargetGuid(ops[a]));
    const int right = depths.at(TargetGuid(ops[b]));
    if (left != right) {
      // Deletes children first; everything else parents first.
      return left_phase == 3 ? left > right : left < right;
    }
    return false;
  });
  std::vector<BookmarkApplyOp> out;
  for (size_t index : order) {
    out.push_back(std::move(ops[index]));
  }
  return out;
}

// --- Adoption (§6 rule i) -------------------------------------------------------------

OwnedAdoptionResult AdoptBookmarks(const std::vector<BookmarkEntity>& arrivals,
                                   const std::vector<LocalBookmark>& locals,
                                   const OwnerResolver& resolve,
                                   int64_t wall_offset_ms) {
  OwnedAdoptionResult result;
  std::map<std::string, const LocalBookmark*> local_by_guid;
  std::map<std::string, std::string> local_guid_by_identity;
  for (const LocalBookmark& local : locals) {
    local_by_guid[local.guid] = &local;
    if (local.sync_id) {
      local_guid_by_identity[*local.sync_id] = local.guid;
    }
  }
  std::map<std::string, std::vector<BookmarkEntity>> arrivals_by_parent;
  for (const BookmarkEntity& entity : arrivals) {
    arrivals_by_parent[entity.parent_uuid.get().str()].push_back(entity);
  }

  struct Level {
    std::string remote_parent;
    std::optional<std::string> local_parent_guid;
    std::string local_space_id;
  };
  std::vector<Level> queue;
  std::set<std::string> visited;
  auto enqueue = [&](const std::string& remote_parent,
                     const std::optional<std::string>& local_parent_guid,
                     const std::string& local_space_id) {
    const std::string key = remote_parent + '\0' +
                            local_parent_guid.value_or(std::string()) + '\0' +
                            local_space_id;
    if (visited.insert(key).second) {
      queue.push_back({remote_parent, local_parent_guid, local_space_id});
    }
  };

  // §6.2: the local side is projected with no baseline; location and rank
  // stay at 0 so a derived position never beats the arrival.
  auto record_merge = [&](const BookmarkEntity& entity,
                          const LocalBookmark& row) {
    const std::string parent_identity = entity.parent_uuid.get().str();
    std::optional<BookmarkEntity> projected = BookmarkKind::Project(
        row, resolve, std::nullopt,
        parent_identity.empty() ? std::nullopt
                                : std::optional<std::string>(parent_identity));
    if (!projected) {
      return false;
    }
    LocalBookmark at_rest = row;
    at_rest.location_updated_ms.reset();
    const BookmarkEntity stamped = BookmarkKind::Stamp(
        *projected, std::nullopt, at_rest, std::string(), 0, 0, wall_offset_ms);
    const BookmarkEntity merged = BookmarkKind::Merge(stamped, entity);
    result.merges[entity.bookmark_uuid] = EncodeOwned<BookmarkKind>(merged);
    if (!(merged == entity)) {
      result.must_republish.insert(entity.bookmark_uuid);
    }
    if (BookmarkKind::ContentSignature(merged) !=
        BookmarkKind::ContentSignature(stamped)) {
      result.field_writes.insert(entity.bookmark_uuid);
    }
    return true;
  };

  // Starting points: every mapped Space root, and every identified local
  // anchor (across rounds).
  for (const BookmarkEntity& entity : arrivals_by_parent[std::string()]) {
    if (std::optional<std::string> space_id =
            resolve.local_space_id(entity.space_uuid.get().str())) {
      enqueue(std::string(), std::nullopt, *space_id);
    }
  }
  for (const BookmarkEntity& entity : arrivals) {
    const std::string parent = entity.parent_uuid.get().str();
    if (parent.empty()) {
      continue;
    }
    const auto guid = local_guid_by_identity.find(parent);
    if (guid == local_guid_by_identity.end()) {
      continue;
    }
    enqueue(parent, guid->second, local_by_guid[guid->second]->space_id);
  }

  for (size_t head = 0; head < queue.size(); ++head) {
    const Level level = queue[head];
    std::vector<BookmarkEntity> candidates;
    for (const BookmarkEntity& entity : arrivals_by_parent[level.remote_parent]) {
      if (level.remote_parent.empty() &&
          resolve.local_space_id(entity.space_uuid.get().str()) !=
              std::optional<std::string>(level.local_space_id)) {
        continue;
      }
      const auto claimed = local_guid_by_identity.find(entity.bookmark_uuid);
      if (claimed != local_guid_by_identity.end()) {
        if (entity.is_folder) {
          enqueue(entity.bookmark_uuid, claimed->second,
                  local_by_guid[claimed->second]->space_id);
        }
        continue;
      }
      candidates.push_back(entity);
    }
    std::vector<LocalBookmark> local_folders;
    std::vector<LocalBookmark> local_items;
    for (const LocalBookmark& row : locals) {
      if (row.sync_id || row.space_id != level.local_space_id ||
          row.parent_guid != level.local_parent_guid) {
        continue;
      }
      (row.is_folder ? local_folders : local_items).push_back(row);
    }
    std::vector<BookmarkEntity> remote_folders;
    std::vector<BookmarkEntity> remote_items;
    for (const BookmarkEntity& entity : candidates) {
      (entity.is_folder ? remote_folders : remote_items).push_back(entity);
    }
    // Folders pair by title (they share one placeholder url), bookmarks by
    // url.
    result.unmatched_folders += PairWithinGroups(
        remote_folders, local_folders,
        [](const BookmarkEntity& e) { return e.title.get().str(); },
        [](const LocalBookmark& r) { return r.title; },
        [&](const BookmarkEntity& entity, const LocalBookmark& row) {
          if (!record_merge(entity, row)) {
            ++result.unmergeable_pairs;
            return;
          }
          result.pairs[entity.bookmark_uuid] = row.guid;
          enqueue(entity.bookmark_uuid, row.guid, row.space_id);
        });
    PairWithinGroups(
        remote_items, local_items,
        [](const BookmarkEntity& e) { return e.url.get().str(); },
        [](const LocalBookmark& r) { return r.url; },
        [&](const BookmarkEntity& entity, const LocalBookmark& row) {
          if (!record_merge(entity, row)) {
            ++result.unmergeable_pairs;
            return;
          }
          result.pairs[entity.bookmark_uuid] = row.guid;
        });
  }
  result.adopted = static_cast<int>(result.pairs.size());
  return result;
}

// --- The kind -----------------------------------------------------------------------------

BookmarkOwnedKind::BookmarkOwnedKind(BookmarkLocalAccess& access,
                                     OwnedTableStore& store,
                                     std::function<std::string()> new_identity,
                                     std::function<std::string()> new_guid)
    : access_(access),
      store_(store),
      new_identity_(std::move(new_identity)),
      new_guid_(std::move(new_guid)) {}

BookmarkOwnedKind::~BookmarkOwnedKind() = default;

const char* BookmarkOwnedKind::label() const {
  return "bookmarks";
}

const char* BookmarkOwnedKind::entity_name() const {
  return kBookmarkEntityName;
}

OwnedKindId BookmarkOwnedKind::kind_id() const {
  return OwnedKindId::kBookmarks;
}

OwnedTableStore& BookmarkOwnedKind::store() {
  return store_;
}

bool BookmarkOwnedKind::tombstone_yields_to_local_edits() const {
  return BookmarkKind::kTombstoneYieldsToLocalEdits;
}

std::optional<std::string> BookmarkOwnedKind::Identity(
    const PhiEntity& envelope) const {
  const std::optional<BookmarkEntity> entity =
      BookmarkKind::FromEnvelope(envelope);
  if (!entity || entity->bookmark_uuid.empty()) {
    return std::nullopt;
  }
  return entity->bookmark_uuid;
}

std::string BookmarkOwnedKind::ClientTag(const std::string& identity) const {
  return BookmarkKind::ClientTag(identity);
}

std::vector<std::string> BookmarkOwnedKind::Owners(
    const std::string& payload) const {
  const std::optional<BookmarkEntity> entity =
      DecodeOwned<BookmarkKind>(payload);
  return entity ? BookmarkKind::OwnerUuids(*entity) : std::vector<std::string>();
}

void BookmarkOwnedKind::Reload(const std::vector<LocalBookmark>& rows) {
  RefreshLandedRows(rows);
  pairs_.clear();
}

void BookmarkOwnedKind::RefreshLandedRows(const std::vector<LocalBookmark>& rows) {
  locals_ = rows;
  identity_to_guid_.clear();
  index_by_guid_.clear();
  for (size_t index = 0; index < locals_.size(); ++index) {
    index_by_guid_[locals_[index].guid] = index;
    if (locals_[index].sync_id) {
      identity_to_guid_[*locals_[index].sync_id] = locals_[index].guid;
    }
  }
}

void BookmarkOwnedKind::NotePersistedClaims(
    const std::map<std::string, std::string>& pairs) {
  for (const auto& [identity, guid] : pairs) {
    const auto it = index_by_guid_.find(guid);
    if (it == index_by_guid_.end()) {
      continue;
    }
    locals_[it->second].sync_id = identity;
    identity_to_guid_[identity] = guid;
  }
}

void BookmarkOwnedKind::NoteDeletedRows(const std::set<std::string>& guids) {
  if (guids.empty()) {
    return;
  }
  std::vector<LocalBookmark> kept;
  for (const LocalBookmark& row : locals_) {
    if (!guids.count(row.guid)) {
      kept.push_back(row);
    }
  }
  RefreshLandedRows(kept);
}

const LocalBookmark* BookmarkOwnedKind::Row(const std::string& guid) const {
  const auto it = index_by_guid_.find(guid);
  return it == index_by_guid_.end() ? nullptr : &locals_[it->second];
}

std::optional<std::string> BookmarkOwnedKind::GuidOf(
    const std::string& identity) const {
  const auto it = identity_to_guid_.find(identity);
  return it == identity_to_guid_.end() ? std::nullopt
                                       : std::optional<std::string>(it->second);
}

bool BookmarkOwnedKind::BeginRound() {
  std::vector<LocalBookmark> rows;
  if (!access_.AllBookmarks(&rows)) {
    Reload({});
    return false;
  }
  Reload(rows);
  return true;
}

// Every local sync id, orphans included, so their tombstones stay routable.
std::set<std::string> BookmarkOwnedKind::LocalIdentities() {
  std::set<std::string> ids;
  if (access_.AllSyncIds(&ids)) {
    return ids;
  }
  for (const LocalBookmark& row : locals_) {
    if (row.sync_id) {
      ids.insert(*row.sync_id);
    }
  }
  return ids;
}

OwnedSnapshotBytes BookmarkOwnedKind::Snapshot(const OwnedTable& table,
                                               const OwnedOwnerMaps& maps,
                                               int64_t now,
                                               int64_t hlc_max,
                                               int64_t wall_offset_ms) {
  OwnedSnapshotBytes out;
  const OwnerResolver resolve = maps.Resolver();
  // Provisional identities for unidentified rows, so a child's parent_uuid
  // names the same candidate; persisted only after the account accepted it.
  std::set<std::string> claimed_guids;
  for (const auto& [identity, guid] : pairs_) {
    claimed_guids.insert(guid);
  }
  std::vector<LocalBookmark> locals = locals_;
  for (LocalBookmark& row : locals) {
    if (!row.sync_id && !claimed_guids.count(row.guid)) {
      const std::string identity = new_identity_();
      row.sync_id = identity;
      out.minted[identity] = row.guid;
    }
  }
  const OwnedSnapshotResult<BookmarkEntity> result =
      OwnedSnapshot<BookmarkKind>(locals, table, resolve, std::nullopt, now,
                                  hlc_max, wall_offset_ms);
  out.skipped_unmapped_owner = result.skipped_unmapped_owner;
  out.skipped_ineligible_owner = result.skipped_ineligible_owner;
  for (const auto& [identity, entity] : result.entities) {
    out.entities[identity] = EncodeOwned<BookmarkKind>(entity);
  }
  for (const LocalBookmark& row : locals) {
    if (!row.sync_id) {
      continue;
    }
    if (std::optional<std::string> owner =
            BookmarkKind::EligibilityOwner(row, resolve, std::nullopt)) {
      out.owner_uuids[*row.sync_id] = *owner;
    }
  }
  // A minted candidate the snapshot excluded remints next round.
  for (auto it = out.minted.begin(); it != out.minted.end();) {
    if (out.entities.count(it->first)) {
      ++it;
    } else {
      it = out.minted.erase(it);
    }
  }
  return out;
}

// The deletion domain is every sync id (R-exec-4), not the snapshot.
bool BookmarkOwnedKind::Tombstones(const OwnedTable& table,
                                   const OwnedOwnerMaps& maps,
                                   int64_t now,
                                   OwnedTombstoneResult* out) {
  std::set<std::string> live;
  if (!access_.AllSyncIds(&live)) {
    return false;
  }
  std::vector<LocalBookmark> identities;
  for (const std::string& id : live) {
    LocalBookmark row;
    row.sync_id = id;
    row.guid = id;
    identities.push_back(row);
  }
  std::set<std::string> pending_claims;
  for (const auto& [identity, guid] : pairs_) {
    pending_claims.insert(identity);
  }
  *out = OwnedTombstones<BookmarkKind>(identities, table, maps.Resolver(),
                                       std::nullopt, now, pending_claims);
  return true;
}

OwnedParkedClaimResult BookmarkOwnedKind::RetryParkedClaims(
    const std::map<std::string, ParkedOwnedItem>& parked,
    const OwnedOwnerMaps& maps,
    int64_t wall_offset_ms) {
  OwnedParkedClaimResult out;
  std::vector<BookmarkEntity> entities;
  for (const auto& [identity, item] : parked) {
    if (std::optional<BookmarkEntity> entity =
            DecodeOwned<BookmarkKind>(item.payload)) {
      entities.push_back(std::move(*entity));
    }
  }
  if (entities.empty()) {
    return out;
  }
  const OwnedAdoptionResult adoption =
      AdoptBookmarks(entities, locals_, maps.Resolver(), wall_offset_ms);
  if (adoption.pairs.empty()) {
    return out;
  }
  for (const auto& [identity, guid] : adoption.pairs) {
    out.paired.insert(identity);
    pairs_[identity] = guid;
  }
  out.persisted = ClaimIdentities(adoption.pairs);
  std::map<std::string, std::string> persisted_pairs;
  for (const std::string& identity : out.persisted) {
    persisted_pairs[identity] = adoption.pairs.at(identity);
  }
  NotePersistedClaims(persisted_pairs);
  return out;
}

std::map<std::string, std::string> BookmarkOwnedKind::LocalProjections(
    const std::set<std::string>& identities,
    const OwnedTable& table,
    const OwnerResolver& resolve,
    int64_t now,
    int64_t wall_offset_ms) const {
  std::map<std::string, std::string> out;
  for (const std::string& identity : identities) {
    const std::optional<std::string> guid = GuidOf(identity);
    const LocalBookmark* row = guid ? Row(*guid) : nullptr;
    const auto cursor = table.cursors.find(identity);
    if (!row || cursor == table.cursors.end()) {
      continue;
    }
    const std::optional<BookmarkEntity> baseline =
        DecodeOwned<BookmarkKind>(cursor->second.reconciled);
    if (!baseline) {
      continue;
    }
    std::optional<std::string> parent_identity;
    if (row->parent_guid) {
      const LocalBookmark* parent = Row(*row->parent_guid);
      if (!parent || !parent->sync_id) {
        continue;
      }
      parent_identity = parent->sync_id;
    }
    const std::optional<BookmarkEntity> projected =
        BookmarkKind::Project(*row, resolve, std::nullopt, parent_identity);
    if (!projected) {
      continue;
    }
    // The baseline's rank: the account's order is the snapshot's job.
    out[identity] = EncodeOwned<BookmarkKind>(BookmarkKind::Stamp(
        *projected, baseline, *row, BookmarkKind::Rank(*baseline), now, 0,
        wall_offset_ms));
  }
  return out;
}

std::set<std::string> BookmarkOwnedKind::DeletedSubtree(
    const std::set<std::string>& tombstoned) const {
  std::map<std::string, std::vector<const LocalBookmark*>> children_of;
  for (const LocalBookmark& row : locals_) {
    if (row.parent_guid) {
      children_of[*row.parent_guid].push_back(&row);
    }
  }
  std::set<std::string> out;
  std::vector<std::string> stack;
  for (const std::string& identity : tombstoned) {
    if (std::optional<std::string> guid = GuidOf(identity)) {
      stack.push_back(*guid);
    }
  }
  size_t hops = 0;
  const size_t limit = locals_.size() * 2 + 1;
  while (!stack.empty() && hops < limit) {
    ++hops;
    const std::string guid = stack.back();
    stack.pop_back();
    for (const LocalBookmark* child : children_of[guid]) {
      if (child->sync_id) {
        out.insert(*child->sync_id);
      }
      stack.push_back(child->guid);
    }
  }
  return out;
}

OwnedPlanOutput BookmarkOwnedKind::Plan(const OwnedPlanInput& input) {
  OwnedPlanOutput out;
  const OwnerResolver resolve = input.maps.Resolver();
  std::vector<OwnedArrival<BookmarkEntity>> arrivals;
  for (const OwnedPlanInput::Arrival& item : input.arrivals) {
    std::optional<BookmarkEntity> entity =
        DecodeOwned<BookmarkKind>(item.payload);
    if (!entity) {
      continue;
    }
    out.server_bytes[entity->bookmark_uuid] = item.payload;
    const std::string space_uuid = entity->space_uuid.get().str();
    if (!space_uuid.empty()) {
      out.deferred_owners.insert(space_uuid);
    }
    arrivals.push_back({std::move(*entity), item.entity_id, item.version});
  }
  // Adoption is continuous and stateless: parked payloads join it.
  std::vector<BookmarkEntity> candidates;
  std::set<std::string> arrived;
  for (const auto& arrival : arrivals) {
    candidates.push_back(arrival.entity);
    arrived.insert(arrival.entity.bookmark_uuid);
  }
  for (const auto& [identity, item] : input.parked) {
    if (arrived.count(identity)) {
      continue;
    }
    if (std::optional<BookmarkEntity> entity =
            DecodeOwned<BookmarkKind>(item.payload)) {
      candidates.push_back(std::move(*entity));
    }
  }
  const OwnedAdoptionResult adoption =
      AdoptBookmarks(candidates, locals_, resolve, input.wall_offset_ms);

  OwnedPlanContext context;
  context.pairs = adoption.pairs;
  context.adopted_merges = adoption.merges;
  context.adopted_field_writes = adoption.field_writes;
  context.tombstoned_identities = input.tombstoned;
  std::vector<BookmarkEntity> arrival_entities;
  for (const auto& arrival : arrivals) {
    arrival_entities.push_back(arrival.entity);
  }
  const std::set<std::string> domain = OwnedProjectionDomain<BookmarkKind>(
      arrival_entities, input.parked, input.tombstoned,
      [this](const std::string& identity) -> std::optional<std::string> {
        const std::optional<std::string> guid = GuidOf(identity);
        const LocalBookmark* row = guid ? Row(*guid) : nullptr;
        if (!row || !row->parent_guid) {
          return std::nullopt;
        }
        const LocalBookmark* parent = Row(*row->parent_guid);
        return parent ? parent->sync_id : std::nullopt;
      });
  context.local_projections = LocalProjections(domain, input.table, resolve,
                                               input.now, input.wall_offset_ms);
  context.pending_local_edits = OwnedUnpublishedEdits<BookmarkKind>(
      context.local_projections, input.table);
  context.unpublished =
      OwnedUnpublishedMerges(context.local_projections, input.table);
  // A redelivered tombstone for an identity already yielded keeps yielding
  // while a live row claims it.
  for (const std::string& identity : input.tombstoned) {
    if (!identity_to_guid_.count(identity)) {
      continue;
    }
    const auto cursor = input.table.cursors.find(identity);
    if (cursor != input.table.cursors.end() && cursor->second.deleted_at_ms &&
        !cursor->second.reconciled) {
      context.pending_local_edits.insert(identity);
    }
  }
  for (const LocalBookmark& row : locals_) {
    if (row.is_folder && row.sync_id) {
      context.live_local_parents.insert(*row.sync_id);
    }
  }
  context.deleted_subtree = DeletedSubtree(input.tombstoned);
  out.plan = OwnedPlanFor<BookmarkKind>(arrivals, input.parked, input.table,
                                        resolve, context);
  for (const auto& [identity, guid] : adoption.pairs) {
    pairs_[identity] = guid;
  }
  out.adopted = adoption.adopted;
  out.unmatched_folders = adoption.unmatched_folders;
  out.unmergeable_pairs = adoption.unmergeable_pairs;
  out.must_republish = adoption.must_republish;
  out.must_republish.insert(out.plan.must_republish.begin(),
                            out.plan.must_republish.end());
  return out;
}

OwnedLandingOutcome BookmarkOwnedKind::Land(const OwnedLandingInput& input) {
  OwnedLandingOutcome outcome;
  if (input.steps.empty()) {
    return outcome;
  }
  const OwnerResolver resolve = input.maps.Resolver();
  struct Planned {
    OwnedApplyStep step;
    std::optional<BookmarkEntity> entity;
  };
  std::vector<Planned> planned;
  for (const OwnedApplyStep& step : input.steps) {
    planned.push_back({step, step.payload
                                 ? DecodeOwned<BookmarkKind>(*step.payload)
                                 : std::nullopt});
  }
  std::map<std::string, std::string> guid_of;
  for (const Planned& item : planned) {
    const auto pair = pairs_.find(item.step.identity);
    if (pair != pairs_.end()) {
      guid_of[item.step.identity] = pair->second;
    } else if (std::optional<std::string> guid = GuidOf(item.step.identity)) {
      guid_of[item.step.identity] = *guid;
    }
  }
  // The physical row type never changes (§4.6).
  std::vector<Planned> work;
  for (const Planned& item : planned) {
    const auto guid = guid_of.find(item.step.identity);
    if (item.entity && guid != guid_of.end()) {
      const std::optional<bool> is_folder = access_.LocalIsFolder(guid->second);
      if (is_folder && *is_folder != item.entity->is_folder) {
        outcome.refused.insert(item.step.identity);
        continue;
      }
    }
    work.push_back(item);
  }
  // Premint guids for creates (children resolve their parent), and for an
  // identity whose local deletion A9 just cancelled (its row is gone).
  for (const Planned& item : work) {
    if (guid_of.count(item.step.identity)) {
      continue;
    }
    const auto cursor = input.table.cursors.find(item.step.identity);
    if (item.step.kind == OwnedStepKind::kCreate ||
        (item.step.kind != OwnedStepKind::kDelete &&
         cursor != input.table.cursors.end() && cursor->second.pending_delete)) {
      guid_of[item.step.identity] = new_guid_();
    }
  }
  if (work.empty()) {
    return outcome;
  }
  std::set<std::string> deleted_identities;
  std::set<std::string> deleted_guids;
  for (const Planned& item : work) {
    if (item.step.kind == OwnedStepKind::kDelete) {
      deleted_identities.insert(item.step.identity);
      if (const auto guid = guid_of.find(item.step.identity);
          guid != guid_of.end()) {
        deleted_guids.insert(guid->second);
      }
    }
  }

  // An in-memory projection of the rows, moved as planned.
  std::map<std::string, LocalBookmark> projected;
  for (const LocalBookmark& row : locals_) {
    projected[row.guid] = row;
  }
  std::map<std::string, std::string> rank_of;
  for (const auto& [identity, cursor] : input.table.cursors) {
    if (std::optional<BookmarkEntity> entity =
            DecodeOwned<BookmarkKind>(cursor.reconciled)) {
      rank_of[identity] = BookmarkKind::Rank(*entity);
    }
  }
  struct Placed {
    Planned item;
    std::string guid;
    SiblingGroup group;
  };
  std::vector<Placed> placed;
  std::set<SiblingGroup> touched;
  std::map<std::string, std::string> parent_of;
  std::map<std::string, std::string> payload_of;

  for (const Planned& item : work) {
    const std::string& identity = item.step.identity;
    if (item.step.payload) {
      payload_of[identity] = *item.step.payload;
    }
    if (item.step.new_rank) {
      rank_of[identity] = *item.step.new_rank;
    }
    const auto resolved = guid_of.find(identity);
    // §5.6 T3: a known identity with no local row just finalizes.
    if (item.step.kind == OwnedStepKind::kDelete &&
        (resolved == guid_of.end() || !projected.count(resolved->second))) {
      outcome.landed.insert(identity);
      outcome.deleted.insert(identity);
      continue;
    }
    if (resolved == guid_of.end()) {
      continue;
    }
    const std::string guid = resolved->second;
    if (item.step.kind == OwnedStepKind::kUpdate ||
        item.step.kind == OwnedStepKind::kDelete) {
      const auto row = projected.find(guid);
      if (row != projected.end()) {
        placed.push_back(
            {item, guid, {row->second.space_id, row->second.parent_guid}});
        continue;
      }
      // An update whose row is gone rebuilds it below (A9 cancelled its
      // deletion).
      if (item.step.kind != OwnedStepKind::kUpdate || !item.entity) {
        continue;
      }
    }
    if (!item.entity) {
      continue;
    }
    const BookmarkEntity& entity = *item.entity;
    const std::string parent_identity =
        item.step.new_parent_uuid.value_or(entity.parent_uuid.get().str());
    SiblingGroup group;
    if (parent_identity.empty()) {
      const std::optional<std::string> space_id =
          resolve.local_space_id(entity.space_uuid.get().str());
      if (!space_id) {
        outcome.parked.insert(identity);
        continue;
      }
      group = {*space_id, std::nullopt};
    } else {
      std::optional<std::string> parent_guid;
      if (const auto it = guid_of.find(parent_identity); it != guid_of.end()) {
        parent_guid = it->second;
      } else {
        parent_guid = GuidOf(parent_identity);
      }
      const auto parent_row =
          parent_guid ? projected.find(*parent_guid) : projected.end();
      if (parent_row == projected.end()) {
        outcome.parked.insert(identity);
        continue;
      }
      group = {parent_row->second.space_id, *parent_guid};
      parent_of[guid] = *parent_guid;
    }
    auto row = projected.find(guid);
    if (row != projected.end()) {
      row->second.sync_id = identity;
      row->second.space_id = group.space_id;
      row->second.parent_guid = group.parent_guid;
    } else {
      LocalBookmark created;
      created.sync_id = identity;
      created.guid = guid;
      created.space_id = group.space_id;
      // The Space's own profile: a row under any other has no root to land in.
      std::optional<std::string> profile =
          resolve.local_profile_id_for_space(group.space_id);
      if (!profile) {
        for (const LocalBookmark& local : locals_) {
          if (local.space_id == group.space_id) {
            profile = local.profile_id;
            break;
          }
        }
      }
      created.profile_id = profile.value_or(kDefaultProfileId);
      created.parent_guid = group.parent_guid;
      created.is_folder = entity.is_folder;
      created.title = entity.title.get().str();
      created.url = entity.url.get().str().empty() ? kFolderPlaceholderUrl
                                                   : entity.url.get().str();
      if (!entity.secondary_url.get().str().empty()) {
        created.secondary_url = entity.secondary_url.get().str();
      }
      if (!entity.secondary_title.get().str().empty()) {
        created.secondary_title = entity.secondary_title.get().str();
      }
      created.source = entity.source;
      created.created_ms = entity.created_at_ms;
      projected[guid] = created;
    }
    touched.insert(group);
    placed.push_back({item, guid, group});
  }

  // C4's tree rule: a deleted folder's surviving children are lifted to
  // the Space root first (the store refuses a non-empty folder delete).
  std::map<std::string, std::vector<std::string>> children_of;
  for (const auto& [guid, row] : projected) {
    if (row.parent_guid) {
      children_of[*row.parent_guid].push_back(guid);
    }
  }
  std::set<std::string> lifted_yields;
  for (const std::string& guid : deleted_guids) {
    const auto folder = projected.find(guid);
    if (folder == projected.end() || !folder->second.is_folder) {
      continue;
    }
    std::vector<std::string> stack = children_of[guid];
    size_t hops = 0;
    while (!stack.empty() && hops <= projected.size()) {
      ++hops;
      const std::string child = stack.back();
      stack.pop_back();
      for (const std::string& grandchild : children_of[child]) {
        stack.push_back(grandchild);
      }
      auto row = projected.find(child);
      if (deleted_guids.count(child) || row == projected.end()) {
        continue;
      }
      row->second.parent_guid.reset();
      parent_of.erase(child);
      touched.insert({row->second.space_id, std::nullopt});
      if (row->second.sync_id &&
          input.yielded_tombstones.count(*row->second.sync_id)) {
        lifted_yields.insert(child);
      }
    }
  }
  for (const std::string& guid : deleted_guids) {
    projected.erase(guid);
  }

  // Wire ranks → full sibling permutations for every touched group.
  std::map<std::string, int> index_of;
  for (const SiblingGroup& group : touched) {
    std::vector<LocalBookmark> members;
    std::set<std::string> seen;
    for (const LocalBookmark& sibling :
         access_.Siblings(group.parent_guid, group.space_id)) {
      const auto row = projected.find(sibling.guid);
      if (row != projected.end() && row->second.space_id == group.space_id &&
          row->second.parent_guid == group.parent_guid &&
          seen.insert(sibling.guid).second) {
        members.push_back(row->second);
      }
    }
    for (const auto& [guid, row] : projected) {
      if (row.space_id == group.space_id &&
          row.parent_guid == group.parent_guid && seen.insert(guid).second) {
        members.push_back(row);
      }
    }
    for (const auto& [guid, index] :
         BookmarkKind::RankToIndex(members, rank_of)) {
      index_of[guid] = index;
    }
  }
  auto index_for = [&](const std::string& guid, int fallback) {
    const auto it = index_of.find(guid);
    return it == index_of.end() ? fallback : it->second;
  };

  std::map<std::string, std::vector<BookmarkApplyOp>> ops_by_space;
  std::map<std::string, std::set<std::string>> identities_by_space;
  std::set<std::string> indexed;
  auto emit = [&](BookmarkApplyOp op, const std::string& space_id) {
    ops_by_space[space_id].push_back(std::move(op));
  };
  auto move_op = [&](const std::string& guid, const SiblingGroup& group,
                     int index, bool records_location_edit) {
    BookmarkApplyOp op;
    op.kind = BookmarkApplyOp::Kind::kMove;
    op.guid = guid;
    op.parent_guid = group.parent_guid;
    op.space_id = group.space_id;
    op.index = index;
    op.records_location_edit = records_location_edit;
    return op;
  };
  auto create_op = [&](LocalBookmark row) {
    BookmarkApplyOp op;
    op.kind = BookmarkApplyOp::Kind::kCreate;
    op.guid = row.guid;
    op.row = std::move(row);
    return op;
  };
  auto update_op = [&](const std::string& guid, const BookmarkEntity& entity) {
    BookmarkApplyOp op;
    op.kind = BookmarkApplyOp::Kind::kUpdate;
    op.guid = guid;
    op.fields = PatchFor(entity);
    return op;
  };
  auto recreated_missing_row = [&](const std::string& guid,
                                   const SiblingGroup& group) {
    if (Row(guid)) {
      return false;
    }
    auto row = projected.find(guid);
    if (row == projected.end()) {
      return false;
    }
    LocalBookmark created = row->second;
    created.index = index_for(guid, 0);
    emit(create_op(std::move(created)), group.space_id);
    indexed.insert(guid);
    return true;
  };

  for (const Placed& entry : placed) {
    const std::string& identity = entry.item.step.identity;
    identities_by_space[entry.group.space_id].insert(identity);
    switch (entry.item.step.kind) {
      case OwnedStepKind::kClaim: {
        BookmarkApplyOp op;
        op.kind = BookmarkApplyOp::Kind::kClaim;
        op.guid = entry.guid;
        op.sync_id = identity;
        emit(std::move(op), entry.group.space_id);
        break;
      }
      case OwnedStepKind::kCreate:
        if (identity_to_guid_.count(identity) || pairs_.count(identity)) {
          // An existing identity: replay or adoption, not a new row.
          emit(move_op(entry.guid, entry.group, index_for(entry.guid, 0), false),
               entry.group.space_id);
          indexed.insert(entry.guid);
          if (entry.item.entity) {
            emit(update_op(entry.guid, *entry.item.entity), entry.group.space_id);
          }
        } else if (const auto row = projected.find(entry.guid);
                   row != projected.end()) {
          LocalBookmark created = row->second;
          created.index = index_for(entry.guid, 0);
          emit(create_op(std::move(created)), entry.group.space_id);
          indexed.insert(entry.guid);
        }
        break;
      case OwnedStepKind::kMove:
        if (recreated_missing_row(entry.guid, entry.group)) {
          break;
        }
        emit(move_op(entry.guid, entry.group, index_for(entry.guid, 0), false),
             entry.group.space_id);
        indexed.insert(entry.guid);
        break;
      case OwnedStepKind::kUpdate:
        if (recreated_missing_row(entry.guid, entry.group)) {
          break;
        }
        if (entry.item.entity) {
          emit(update_op(entry.guid, *entry.item.entity), entry.group.space_id);
        }
        break;
      case OwnedStepKind::kTransfer:
        break;  // rules only
      case OwnedStepKind::kDelete: {
        BookmarkApplyOp op;
        op.kind = BookmarkApplyOp::Kind::kDelete;
        op.guid = entry.guid;
        emit(std::move(op), entry.group.space_id);
        break;
      }
    }
  }
  // The rest of every touched group, in final-index order.
  for (const SiblingGroup& group : touched) {
    std::vector<const LocalBookmark*> movers;
    for (const auto& [guid, row] : projected) {
      if (row.space_id == group.space_id &&
          row.parent_guid == group.parent_guid && !indexed.count(guid) &&
          Row(guid)) {
        movers.push_back(&row);
      }
    }
    std::sort(movers.begin(), movers.end(),
              [&](const LocalBookmark* a, const LocalBookmark* b) {
                const int left = index_for(a->guid, 0);
                const int right = index_for(b->guid, 0);
                return left == right ? a->guid < b->guid : left < right;
              });
    for (const LocalBookmark* row : movers) {
      emit(move_op(row->guid, group, index_for(row->guid, row->index),
                   lifted_yields.count(row->guid) > 0),
           group.space_id);
    }
  }

  // One transaction per Space: an import lock in one does not roll back
  // another.
  bool did_apply = false;
  for (auto& [space_id, ops] : ops_by_space) {
    if (ops.empty()) {
      continue;
    }
    const std::set<std::string>& identities = identities_by_space[space_id];
    const BookmarkApplyResult result =
        access_.Apply(OrderBookmarkOps(std::move(ops), parent_of));
    if (result == BookmarkApplyResult::kFolderNotEmpty ||
        result == BookmarkApplyResult::kRowAlreadyMapped) {
      outcome.refused.insert(identities.begin(), identities.end());
      continue;
    }
    if (result != BookmarkApplyResult::kOk) {
      outcome.parked.insert(identities.begin(), identities.end());
      continue;
    }
    did_apply = true;
    // Checked against the rebuilt snapshot before any baseline moves.
    for (const std::string& identity : identities) {
      const auto guid = guid_of.find(identity);
      if (guid == guid_of.end()) {
        continue;
      }
      const bool known = access_.IsKnownLocalBookmark(guid->second);
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
  std::vector<LocalBookmark> refreshed;
  if (did_apply && access_.CachedBookmarks(&refreshed)) {
    RefreshLandedRows(refreshed);
  }
  return outcome;
}

std::set<std::string> BookmarkOwnedKind::ClaimIdentities(
    const std::map<std::string, std::string>& minted) {
  std::map<std::string, std::vector<BookmarkApplyOp>> by_space;
  std::map<std::string, std::set<std::string>> identities_by_space;
  for (const auto& [identity, guid] : minted) {
    const LocalBookmark* row = Row(guid);
    const std::string space_id = row ? row->space_id : std::string();
    BookmarkApplyOp op;
    op.kind = BookmarkApplyOp::Kind::kClaim;
    op.guid = guid;
    op.sync_id = identity;
    by_space[space_id].push_back(std::move(op));
    identities_by_space[space_id].insert(identity);
  }
  std::set<std::string> persisted;
  for (auto& [space_id, ops] : by_space) {
    if (access_.Apply(ops) == BookmarkApplyResult::kOk) {
      persisted.insert(identities_by_space[space_id].begin(),
                       identities_by_space[space_id].end());
    }
  }
  std::map<std::string, std::string> persisted_pairs;
  for (const std::string& identity : persisted) {
    persisted_pairs[identity] = minted.at(identity);
  }
  NotePersistedClaims(persisted_pairs);
  return persisted;
}

bool BookmarkOwnedKind::LiveOwners(const std::set<std::string>& candidates,
                                   const OwnedOwnerMaps& maps,
                                   OwnedLiveRows* out) {
  std::set<std::string> live;
  if (!access_.AllSyncIds(&live)) {
    return false;
  }
  for (const std::string& identity : candidates) {
    if (live.count(identity)) {
      out->claimed.insert(identity);
    }
  }
  const OwnerResolver resolve = maps.Resolver();
  for (const LocalBookmark& row : locals_) {
    if (!row.sync_id || !out->claimed.count(*row.sync_id)) {
      continue;
    }
    if (std::optional<std::string> owner =
            BookmarkKind::EligibilityOwner(row, resolve, std::nullopt)) {
      out->owners[*row.sync_id] = *owner;
    }
  }
  return true;
}

}  // namespace phi_client::sync

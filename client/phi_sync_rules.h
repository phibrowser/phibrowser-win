// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_RULES_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_RULES_H_

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_sync_engine.h"
#include "client/phi_sync_owned.h"

// The URL rule kind (docs/user-data-parity.md S5) — phibrowser-mac
// URLRuleKind (codec, stamps, merge, signatures, the merge tail: pointer
// pass + convergence), the rule OwnedKindRegistration (claims by signature,
// landing, pending-edit clearing) and the store's applyURLRuleSyncBatchBody,
// here a pure function over the rule rows. Host-clean.
namespace phi_client::sync {

inline constexpr char kUrlRuleEntityName[] = "phi-urlrule";
// The local rule targets that are actions (phi_url_router.h).
inline constexpr char kIncognitoRuleTargetId[] = "space.incognito";
inline constexpr char kKioskRuleTargetId[] = "__phi_kiosk__";

// One rule as sync sees it (PhiLocalURLRule), soft-deleted ones included.
struct LocalUrlRule {
  std::string id;
  std::optional<std::string> sync_id;
  std::string space_id;
  std::string host;
  std::optional<std::string> path_prefix;
  bool ask = false;
  int sort_order = 0;
  int64_t created_ms = 0;
  std::optional<int64_t> content_updated_ms;
  std::optional<int64_t> target_updated_ms;
  std::optional<int64_t> deleted_ms;
  bool pending_local_edit = false;
  std::optional<std::string> merge_partner_sync_id;
  bool operator==(const LocalUrlRule& other) const = default;
};

// The store's host/path normalization (LocalStore.normalizedRule).
std::pair<std::string, std::optional<std::string>> NormalizedRule(
    const std::string& host,
    const std::optional<std::string>& path_prefix);

struct UrlRuleKind {
  using Entity = UrlRuleEntity;
  using Local = LocalUrlRule;
  static constexpr bool kTombstoneYieldsToLocalEdits = true;

  static std::string ClientTag(const std::string& identity);
  static std::string Identity(const Entity& entity) { return entity.rule_uuid; }
  static std::optional<std::string> LocalIdentity(
      const Local& local,
      const OwnerResolver&,
      const std::optional<std::string>&) {
    return local.sync_id;
  }
  static PhiEntity Envelope(const Entity& entity);
  static std::optional<Entity> FromEnvelope(const PhiEntity& envelope);
  static std::string LocalId(const Local& local) { return local.id; }
  static std::optional<std::string> LocalParentId(const Local&) {
    return std::nullopt;
  }
  static std::optional<std::string> EligibilityOwner(
      const Local& local,
      const OwnerResolver& resolve,
      const std::optional<std::string>& scope);
  static std::vector<std::string> OwnerUuids(const Entity& entity) {
    return {entity.target_space_uuid.get().str()};
  }
  static std::optional<std::string> TargetOwnerUuid(const Entity& entity) {
    return entity.target_space_uuid.get().str();
  }
  static std::optional<RuleProjection> TransferSource(
      const Entity& entity,
      const OwnerResolver& resolve);
  static std::optional<Entity> Project(
      const Local& local,
      const OwnerResolver& resolve,
      const std::optional<std::string>& scope,
      const std::optional<std::string>& parent_identity);
  static std::string Rank(const Entity& entity) {
    return entity.rank.get().str();
  }
  static std::string ContentSignature(const Entity& entity);
  static int64_t LocationStamp(const Entity& entity) {
    return entity.target_space_uuid.get().updated_at_ms;
  }
  static int64_t ContentStamp(const Entity& entity) {
    return entity.host.get().updated_at_ms;
  }
  static std::optional<Entity> Reverted(const Entity&,
                                        const std::optional<Entity>&,
                                        int64_t) {
    return std::nullopt;
  }
  static Entity Stamp(const Entity& projected,
                      const std::optional<Entity>& baseline,
                      const Local& local,
                      const std::string& rank,
                      int64_t now,
                      int64_t hlc_max,
                      int64_t wall_offset_ms);
  static Entity Merge(const Entity& local, const Entity& remote);
  static std::optional<OwnedRefusal> Refuses(
      const Entity& entity,
      const std::optional<Entity>& baseline);

  // The local id a wire target names ("incognito-space" → the Incognito
  // action target); nullopt when unmapped.
  static std::optional<std::string> LocalSpaceIdOf(const std::string& target,
                                                   const OwnerResolver& resolve);
  static std::optional<RuleSignature> SignatureOf(const Local& row,
                                                  const OwnerResolver& resolve);
  static std::optional<RuleSignature> SignatureOf(const Entity& entity,
                                                  const OwnerResolver& resolve);
  static std::optional<RuleSignature> BaselineSignature(
      const std::string& identity,
      const OwnedTable& table,
      const OwnerResolver& resolve);
  static bool IsAtRest(const Local& row,
                       const OwnedCursor* cursor,
                       const OwnerResolver& resolve,
                       const std::set<std::string>& tombstones_this_page);
  // Wire ranks → dense sort orders among live siblings (unranked first).
  static std::map<std::string, int> RankToSortOrder(
      const std::vector<Local>& siblings,
      const std::map<std::string, std::string>& ranks);
  static RuleProjection ClearingProjection(const Local& row);
  static bool ClearingProjectionMatches(const RuleProjection& row,
                                        const RuleProjection& confirmed);
  static bool TransferSourceUnchanged(const Local* row,
                                      const RuleProjection& source);
};

// --- The rule table's sync writes (applyURLRuleSyncBatchBody) ---------------------

struct UrlRuleLandingValues {
  std::string sync_id;
  std::string space_id;
  std::string host;
  std::optional<std::string> path_prefix;
  bool ask = false;
  int sort_order = 0;
  int64_t created_ms = 0;
  int64_t content_updated_ms = 0;
  int64_t target_updated_ms = 0;
  bool operator==(const UrlRuleLandingValues& other) const = default;
};

struct UrlRuleEffectiveStamps {
  std::optional<int64_t> content;
  std::optional<int64_t> target;
  bool operator==(const UrlRuleEffectiveStamps& other) const = default;
};

struct UrlRuleSyncOp {
  enum class Kind {
    kCreate,
    kUpdate,
    kMove,
    kReorder,
    kDelete,
    kRekey,
    kSoftDelete,
    kSetMergePartner,
    kSetContentGroup,
    kTransfer,
  };
  Kind kind = Kind::kCreate;
  std::string sync_id;  // the addressed identity (kTransfer: the target W)
  std::optional<UrlRuleLandingValues> values;  // create/update/move/rekey
  std::string space_id;                        // kReorder
  int sort_order = 0;                          // kReorder
  std::string local_id;                        // kRekey
  std::optional<std::string> merge_partner;    // kSoftDelete / kSetMergePartner
  // kSetContentGroup.
  std::string host;
  std::optional<std::string> path_prefix;
  bool ask = false;
  int64_t content_updated_ms = 0;
  // kTransfer.
  std::string from_sync_id;
  RuleProjection source;
  UrlRuleEffectiveStamps target_stamps;
  bool operator==(const UrlRuleSyncOp& other) const = default;
};

struct UrlRuleMergeResult {
  std::vector<UrlRuleSyncOp> ops;
  int collapsed = 0;
  std::set<std::string> touched_buckets;
  bool changed_routing = false;
};
using UrlRuleMergeTail =
    std::function<UrlRuleMergeResult(const std::vector<LocalUrlRule>& rows)>;

struct UrlRuleBatchOutcome {
  int collapsed = 0;
  bool merge_changed_routing = false;
  std::set<std::string> deferred_tombstones;
  int transferred = 0;
  int transfer_superseded_by_delete = 0;
};

enum class UrlRuleApplyError { kNone, kRowAlreadyMapped, kRowNotFound };

// URLRuleApplyBatch.init: one op per identity (rekey > move > create >
// update > reorder; a move inside the current bucket degrades), then
// transfers, the merge tail's own ops, deletes last.
std::vector<UrlRuleSyncOp> OrderUrlRuleOps(
    const std::vector<UrlRuleSyncOp>& unordered,
    const std::map<std::string, std::string>& current_space_ids);

// The batch against the whole rule table, in place; `rows` is untouched on
// an error. `new_id` mints a local row id; `now_ms` stamps soft deletes.
UrlRuleApplyError ApplyUrlRuleOps(std::vector<LocalUrlRule>* rows,
                                  const std::vector<UrlRuleSyncOp>& ops,
                                  const UrlRuleMergeTail& merge_tail,
                                  const std::function<std::string()>& new_id,
                                  int64_t now_ms,
                                  UrlRuleBatchOutcome* outcome);

// The merge tail (URLRuleKind.mergePass): pointer pass + convergence.
UrlRuleMergeResult UrlRuleMergePass(
    const std::vector<LocalUrlRule>& rows,
    const std::set<std::string>& landed_this_page,
    const std::set<std::string>& published_identities,
    const std::map<std::string, RuleSignature>& pre_landing_signatures,
    const std::set<std::string>& at_rest,
    const std::map<std::string, UrlRuleLandingValues>& landed,
    const std::map<std::string, std::string>& rebaselined,
    const OwnedTable& table,
    bool converge_allowed,
    const OwnerResolver& resolve);

// The pending-edit clearing (the store's clearPendingLocalEdit bodies): the
// flag goes only while the row still equals what was published.
// `clears_partner` (the post-publish path) also drops the merge pointer.
// False when nothing changed.
bool ClearRulePendingLocalEdit(std::vector<LocalUrlRule>* rows,
                               const std::string& sync_id,
                               const RuleProjection& confirmed,
                               bool clears_partner);

// --- The kind as the engine drives it ----------------------------------------------

// PhiURLRuleLocalAccess.
class UrlRuleLocalAccess {
 public:
  virtual ~UrlRuleLocalAccess() = default;
  // Every rule, soft-deleted ones included, in (space, sortOrder, id)
  // order; rebuilds the snapshot. False when the store could not be read.
  virtual bool AllRules(std::vector<LocalUrlRule>* out) = 0;
  virtual bool CachedRules(std::vector<LocalUrlRule>* out) = 0;
  // The batch in one transaction (ApplyUrlRuleOps over the stored table);
  // the snapshot is rebuilt after it. kNone on success.
  virtual UrlRuleApplyError Apply(const std::vector<UrlRuleSyncOp>& ops,
                                  const UrlRuleMergeTail& merge_tail,
                                  bool* failed,
                                  UrlRuleBatchOutcome* outcome) = 0;
  virtual void HardDelete(const std::string& sync_id) = 0;
  // Soft-deleted rows older than `cutoff_ms` go for good; returns how many.
  virtual int PurgeSoftDeleted(int64_t cutoff_ms) = 0;
  virtual void ClearPendingLocalEdits(
      const std::map<std::string, RuleProjection>& entries,
      bool clears_partner) = 0;
};

class UrlRuleOwnedKind : public OwnedKindAdapter {
 public:
  UrlRuleOwnedKind(UrlRuleLocalAccess& access, OwnedTableStore& store);
  ~UrlRuleOwnedKind() override;

  const char* label() const override;
  const char* entity_name() const override;
  OwnedKindId kind_id() const override;
  OwnedTableStore& store() override;
  bool tombstone_yields_to_local_edits() const override;
  bool lands_empty_batch() const override;
  std::optional<std::string> Identity(const PhiEntity& envelope) const override;
  std::string ClientTag(const std::string& identity) const override;
  std::vector<std::string> Owners(const std::string& payload) const override;
  bool BeginRound() override;
  std::set<std::string> LocalIdentities() override;
  OwnedSnapshotBytes Snapshot(const OwnedTable& table,
                              const OwnedOwnerMaps& maps,
                              int64_t now,
                              int64_t hlc_max,
                              int64_t wall_offset_ms) override;
  bool Tombstones(const OwnedTable& table,
                  const OwnedOwnerMaps& maps,
                  int64_t now,
                  OwnedTombstoneResult* out) override;
  OwnedParkedClaimResult RetryParkedClaims(
      const std::map<std::string, ParkedOwnedItem>& parked,
      const OwnedOwnerMaps& maps,
      int64_t wall_offset_ms) override;
  OwnedPlanOutput Plan(const OwnedPlanInput& input) override;
  OwnedLandingOutcome Land(const OwnedLandingInput& input) override;
  std::set<std::string> ClaimIdentities(
      const std::map<std::string, std::string>& minted) override;
  bool LiveOwners(const std::set<std::string>& candidates,
                  const OwnedOwnerMaps& maps,
                  OwnedLiveRows* out) override;
  void NotePublishApplied(const std::set<std::string>& identities) override;
  void ClearPendingLocalEdits(const std::set<std::string>& candidates) override;
  void HardDeleteAfterTombstone(const std::set<std::string>& identities) override;
  int PurgeSoftDeletedRows(int64_t cutoff_ms) override;

 private:
  void Reload(const std::vector<LocalUrlRule>& rows);
  std::vector<LocalUrlRule> Live() const;

  RAW_PTR_EXCLUSION UrlRuleLocalAccess& access_;
  RAW_PTR_EXCLUSION OwnedTableStore& store_;
  std::vector<LocalUrlRule> rows_;  // soft-deleted included
  bool page_reload_failed_ = false;
  // This round's published projections, for the pending-edit clearing.
  std::map<std::string, RuleProjection> publish_baseline_;
  std::optional<OwnerResolver> publish_resolver_;
};

}  // namespace phi_client::sync

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_RULES_H_

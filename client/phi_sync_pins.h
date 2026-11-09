// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_PINS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_PINS_H_

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_sync_engine.h"
#include "client/phi_sync_owned_kinds.h"

// The pinned-tab kind as the engine drives it (docs/user-data-parity.md
// S5) — phibrowser-mac's pin OwnedKindRegistration (PinSyncRoundState,
// pinSnapshot / pinTombstones / pinPlan / landPins), PinKind's variant
// reminting and the PhiPinnedTabLocalAccess seam. Host-clean.
namespace phi_client::sync {

struct PinFieldPatch {
  std::optional<std::string> title;
  std::optional<std::string> url;
  // Outer = write it; inner nullopt = unlink.
  std::optional<std::optional<std::string>> split_partner_lineage_id;
  bool operator==(const PinFieldPatch& other) const = default;
};

struct PinApplyOp {
  enum class Kind { kCreate, kRelineage, kMove, kUpdate, kDelete };
  Kind kind = Kind::kCreate;
  std::string guid;
  LocalPin row;                // kCreate
  std::string new_lineage_id;  // kRelineage
  int index = 0;               // kMove
  PinFieldPatch fields;        // kUpdate
  bool operator==(const PinApplyOp& other) const = default;
};

// Phase order: create/relineage/move, update, delete (stable).
std::vector<PinApplyOp> OrderPinOps(std::vector<PinApplyOp> ops);

// §7.2 / A11: exact duplicates under one (lineage, owner) collapse; the
// other variants are reminted, the lowest-index row keeping its lineage.
std::vector<PinApplyOp> NormalizePinVariants(const std::vector<LocalPin>& locals);

enum class PinApplyResult {
  kOk,
  kRefused,  // the plan is wrong (row missing, out of scope, empty patch)
  kFailed,   // transient: park and retry
};

// PhiPinnedTabLocalAccess.
class PinLocalAccess {
 public:
  virtual ~PinLocalAccess() = default;
  // "space" / "profile" / "app".
  virtual std::string CurrentScope() = 0;
  // The store's pin-scope migration counter (LocalStore::pin_scope_generation),
  // sampled with the rows at BeginRound and re-probed before every commit
  // and landing; a change means the sampled rows are a stale scope's.
  virtual uint64_t ScopeGeneration() = 0;
  // The account's scope (the synced PhiPinnedTabScope mirror); nullopt
  // before it is known.
  virtual std::optional<std::string> AccountScope() = 0;
  // The active scope's pins, in (owner, index, guid) order; rebuilds the
  // snapshot. False when the store could not be read.
  virtual bool AllPins(std::vector<LocalPin>* out) = 0;
  // Every non-dormant pin row, every scope (the deletion domain).
  virtual bool AllPinRows(std::vector<LocalPin>* out) = 0;
  virtual bool CachedPins(std::vector<LocalPin>* out) = 0;
  // An active row with this lineage under this LOCAL owner key.
  virtual bool IsKnownLocalPin(const std::string& lineage,
                               const std::optional<std::string>& owner_key) = 0;
  // One transaction; the snapshot is rebuilt after it.
  virtual PinApplyResult Apply(const std::vector<PinApplyOp>& ops) = 0;
};

class PinOwnedKind : public OwnedKindAdapter {
 public:
  PinOwnedKind(PinLocalAccess& access,
               OwnedTableStore& store,
               std::function<std::string()> new_guid);
  ~PinOwnedKind() override;

  const char* label() const override;
  const char* entity_name() const override;
  OwnedKindId kind_id() const override;
  OwnedTableStore& store() override;
  bool tombstone_yields_to_local_edits() const override;
  std::optional<std::string> Identity(const PhiEntity& envelope) const override;
  std::string ClientTag(const std::string& identity) const override;
  std::vector<std::string> Owners(const std::string& payload) const override;
  bool clears_split_partners() const override;
  std::map<std::string, std::string> ClearedSplitPartners(
      const std::map<std::string, std::string>& baselines,
      int64_t now,
      const OwnedOwnerMaps& maps) override;
  bool BeginRound() override;
  // The commit fence (the Mac's B-3 stage 1): re-probes the scopes right
  // before a commit; false drops the commit unsent for a later round.
  bool CommitFenceHolds() override;
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

 private:
  // PinSyncRoundState.
  void Reload(const std::vector<LocalPin>& rows,
              const std::string& local_scope,
              const std::optional<std::string>& account_scope,
              uint64_t scope_generation);
  void RescanScopes();
  bool scope_blocked() const;
  void RefreshLandedRows(const std::vector<LocalPin>& rows);
  void NoteDeletedRows(const std::set<std::string>& guids);
  void NoteSplitPartnerWrites(const std::map<std::string, std::string>& linked,
                              const std::set<std::string>& cleared);
  bool StillCarriesSplitLink(const std::string& identity,
                             const OwnerResolver& resolve) const;
  std::map<std::string, std::string> LocalProjections(
      const std::set<std::string>& identities,
      const OwnedTable& table,
      const OwnerResolver& resolve,
      int64_t now,
      int64_t wall_offset_ms) const;

  RAW_PTR_EXCLUSION PinLocalAccess& access_;
  RAW_PTR_EXCLUSION OwnedTableStore& store_;
  std::function<std::string()> new_guid_;

  std::vector<LocalPin> locals_;
  std::string local_scope_ = "profile";
  std::optional<std::string> account_scope_;
  uint64_t scope_generation_ = 0;
  bool scope_moved_mid_round_ = false;
};

}  // namespace phi_client::sync

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_PINS_H_

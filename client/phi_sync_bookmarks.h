// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_BOOKMARKS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_BOOKMARKS_H_

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_sync_engine.h"
#include "client/phi_sync_owned_kinds.h"

// The bookmark kind as the engine drives it (docs/user-data-parity.md S5) —
// phibrowser-mac's bookmark OwnedKindRegistration (BookmarkSyncRoundState,
// bookmarkSnapshot / bookmarkPlan / landBookmarks) and SyncableOwnedItems'
// adoption, over the PhiBookmarkLocalAccess seam. Host-clean.
namespace phi_client::sync {

// A field patch: outer optional = write it; inner nullopt = clear it.
struct BookmarkFieldPatch {
  std::optional<std::string> title;
  std::optional<std::string> url;
  std::optional<std::optional<std::string>> secondary_url;
  std::optional<std::optional<std::string>> secondary_title;
  bool operator==(const BookmarkFieldPatch& other) const = default;
};

// One landing write (BookmarkApplyOp).
struct BookmarkApplyOp {
  enum class Kind { kClaim, kCreate, kMove, kUpdate, kDelete };
  Kind kind = Kind::kCreate;
  std::string guid;
  std::string sync_id;                      // kClaim
  LocalBookmark row;                        // kCreate
  std::optional<std::string> parent_guid;   // kMove: nullopt = the Space root
  std::string space_id;                     // kMove
  int index = 0;                            // kMove
  bool records_location_edit = false;       // kMove (C4's lift)
  BookmarkFieldPatch fields;                // kUpdate
  bool operator==(const BookmarkApplyOp& other) const = default;
};

// Phase order (claim/create/move, update, delete), parents first — deletes
// children first.
std::vector<BookmarkApplyOp> OrderBookmarkOps(
    std::vector<BookmarkApplyOp> ops,
    const std::map<std::string, std::string>& parent_of);

enum class BookmarkApplyResult {
  kOk,
  kFolderNotEmpty,   // refused: the plan is wrong
  kRowAlreadyMapped, // refused: the plan is wrong
  kFailed,           // transient (import lock, I/O): park and retry
};

// PhiBookmarkLocalAccess: the store's bookmark rows as sync reads them — a
// cached snapshot rebuilt by AllBookmarks() and after every Apply().
class BookmarkLocalAccess {
 public:
  virtual ~BookmarkLocalAccess() = default;
  // Every bookmark and folder under a Space root, in (space, parent, index,
  // guid) order; a Space root's children carry parent nullopt. False when
  // the store could not be read.
  virtual bool AllBookmarks(std::vector<LocalBookmark>* out) = 0;
  // Every sync id, orphans included. False before a successful snapshot.
  virtual bool AllSyncIds(std::set<std::string>* out) = 0;
  // The cached snapshot; false when the last read failed.
  virtual bool CachedBookmarks(std::vector<LocalBookmark>* out) = 0;
  virtual std::vector<LocalBookmark> Siblings(
      const std::optional<std::string>& parent_guid,
      const std::string& space_id) = 0;
  virtual bool IsKnownLocalBookmark(const std::string& guid) = 0;
  virtual std::optional<bool> LocalIsFolder(const std::string& guid) = 0;
  // One transaction; the snapshot is rebuilt after it.
  virtual BookmarkApplyResult Apply(const std::vector<BookmarkApplyOp>& ops) = 0;
};

// SyncableOwnedItems.adopt (rule i): an arrival matched to an unidentified
// local row claims it instead of creating a twin.
struct OwnedAdoptionResult {
  std::map<std::string, std::string> pairs;  // identity → local guid
  int adopted = 0;
  int unmatched_folders = 0;
  std::map<std::string, std::string> merges;  // identity → envelope
  std::set<std::string> must_republish;
  std::set<std::string> field_writes;
  int unmergeable_pairs = 0;
};
OwnedAdoptionResult AdoptBookmarks(const std::vector<BookmarkEntity>& arrivals,
                                   const std::vector<LocalBookmark>& locals,
                                   const OwnerResolver& resolve,
                                   int64_t wall_offset_ms);

class BookmarkOwnedKind : public OwnedKindAdapter {
 public:
  // `new_identity` mints a lowercase account uuid, `new_guid` a local row
  // guid (uppercase).
  BookmarkOwnedKind(BookmarkLocalAccess& access,
                    OwnedTableStore& store,
                    std::function<std::string()> new_identity,
                    std::function<std::string()> new_guid);
  ~BookmarkOwnedKind() override;

  const char* label() const override;
  const char* entity_name() const override;
  OwnedKindId kind_id() const override;
  OwnedTableStore& store() override;
  bool tombstone_yields_to_local_edits() const override;
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

 private:
  // BookmarkSyncRoundState.
  void Reload(const std::vector<LocalBookmark>& rows);
  void RefreshLandedRows(const std::vector<LocalBookmark>& rows);
  void NotePersistedClaims(const std::map<std::string, std::string>& pairs);
  void NoteDeletedRows(const std::set<std::string>& guids);
  const LocalBookmark* Row(const std::string& guid) const;
  std::optional<std::string> GuidOf(const std::string& identity) const;
  std::map<std::string, std::string> LocalProjections(
      const std::set<std::string>& identities,
      const OwnedTable& table,
      const OwnerResolver& resolve,
      int64_t now,
      int64_t wall_offset_ms) const;
  std::set<std::string> DeletedSubtree(
      const std::set<std::string>& tombstoned) const;

  RAW_PTR_EXCLUSION BookmarkLocalAccess& access_;
  RAW_PTR_EXCLUSION OwnedTableStore& store_;
  std::function<std::string()> new_identity_;
  std::function<std::string()> new_guid_;

  std::vector<LocalBookmark> locals_;
  std::map<std::string, std::string> identity_to_guid_;
  std::map<std::string, size_t> index_by_guid_;
  std::map<std::string, std::string> pairs_;  // this round's claims
};

}  // namespace phi_client::sync

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_BOOKMARKS_H_

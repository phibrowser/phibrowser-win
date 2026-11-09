// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_LOCAL_STORE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_LOCAL_STORE_H_

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_url_rule_normalize.h"

// The account's local browser data — the Windows twin of phibrowser-mac's
// SwiftData LocalStore (Sources/LocalStorage, schema V13), which the Mac's
// sync engine reads and lands into (docs/user-data-parity.md). Same five
// entities, same fields, same ids and the same WRITE RULES, because sync
// projects these rows onto the wire and compares its own baseline against
// them: which operation stamps contentUpdatedDate or locationUpdatedDate,
// which renumbers siblings, which keeps an inactive pin-scope backup, which
// soft-deletes a URL rule. Each operation below names the Mac function it
// ports.
//
// ONE SQLite file per account: <account>/localDB/LocalStore.sqlite, beside
// the Mac's LocalStoreCompatibility.json manifest (format 13). The rows are
// also held in memory — the UI reads them on every sidebar rebuild — and
// every mutation writes exactly the rows it touched in one transaction; a
// failed write rolls the memory back by reloading from disk, so memory never
// leads disk (the Mac's AccountUserDefaults rule, docs/sync.md "Marker
// persistence boundary").
//
// Deliberate differences from the Mac, none of them on the wire:
//  - Dates are Unix epoch milliseconds (the wire's unit), not Core Data
//    seconds.
//  - Favicons are the framework's raw BGRA with their size, and a split
//    bookmark keeps pane B's favicon too. Favicons are device-local on the
//    Mac as well (never synced), so only the encoding differs.
//  - A tab guid is unique (the Mac's schema allows duplicates and then heals
//    them; here a duplicate create is refused, as the Mac's sync path does).
//  - UI edits of a pinned tab's title/url stamp contentUpdatedDate. The Mac
//    stamps only on its agent/API path; stamping every real content change
//    only makes the stamp more precise (docs/user-data-parity.md D4).
//
// PURE: STL + sqlite3, no base/, no Win32 — host-tested
// (test/model/local_store_test.cc against the system's C compiler build of
// the vendored amalgamation).

struct sqlite3;

namespace phi_client::store {

using TimeMs = int64_t;  // Unix epoch milliseconds.

struct GuestSnapshot;
struct GuestMappings;

inline constexpr int kStoreFormatVersion = 13;
inline constexpr char kStoreFilename[] = "LocalStore.sqlite";
inline constexpr char kCompatibilityManifest[] = "LocalStoreCompatibility.json";

// Well-known ids (the Mac's LocalStore constants).
inline constexpr char kDefaultSpaceId[] = "default-space";
inline constexpr char kDefaultProfileId[] = "Default";
inline constexpr char kBrowserDataSettingsId[] = "browser-data-settings";
inline constexpr char kKioskRuleTargetId[] = "__phi_kiosk__";
inline constexpr char kFolderPlaceholderUrl[] = "https://bookmark.phi/folder";
inline constexpr char kDefaultSpaceName[] = "Default";
inline constexpr char kDefaultSpaceColorHex[] = "#3A6FF8";
inline constexpr char kDefaultSpaceIconName[] = "phi:phi-icon-view-grid-add";
inline constexpr char kBookmarkRootTitle[] = "Bookmarks";
inline constexpr char kDefaultFolderIcon[] = "default";

// TabDataType. 0 (a plain tab) is never created; it is kept for the schema.
enum class RowType : int {
  kTab = 0,
  kPinnedTab = 1,
  kBookmark = 2,
  kBookmarkFolder = 3,
};

// TabSource.
enum class Source : int { kPhi = 0, kChromium = 1, kSafari = 2, kArc = 3 };

// PinnedTabScope. Ordered by the Mac's hierarchyLevel.
enum class PinScope { kSpace = 0, kProfile = 1, kApp = 2 };
const char* PinScopeName(PinScope scope);
bool PinScopeFromName(std::string_view name, PinScope* out);

// TabDataModel (V13): pinned tabs, bookmarks and folders in one entity.
struct Tab {
  std::string guid;
  RowType type = RowType::kBookmark;
  std::string title;
  int index = 0;
  std::string url;
  std::string favicon;  // Raw BGRA; empty = none.
  int favicon_width = 0;
  int favicon_height = 0;
  TimeMs created = 0;
  TimeMs updated = 0;
  std::optional<std::string> override_title;
  bool is_opened = false;
  bool is_created_by_chromium = false;
  bool need_update_metadata = false;
  // Ownership. For pins it encodes the scope (see PinScope): Space =
  // both, Profile = profile only, App = neither.
  std::optional<std::string> space_id;
  std::optional<std::string> profile_id;
  int source = 0;
  std::optional<std::string> secondary_url;    // A split BOOKMARK's pane B.
  std::optional<std::string> secondary_title;
  std::string secondary_favicon;  // Windows-local, see the file comment.
  int secondary_favicon_width = 0;
  int secondary_favicon_height = 0;
  std::optional<std::string> split_partner_guid;  // A split PIN's other row.
  std::optional<std::string> layout;              // "vertical"/"horizontal".
  std::optional<TimeMs> last_seen;
  std::string icon = kDefaultFolderIcon;  // Folder icon id.
  std::optional<std::string> pin_lineage_id;
  bool pin_dormant = false;
  std::optional<std::string> sync_id;  // Bookmarks/folders: the wire uuid.
  std::optional<TimeMs> content_updated;
  std::optional<TimeMs> location_updated;
  std::optional<std::string> parent_guid;  // Bookmarks: the tree.

  bool is_bookmark_kind() const {
    return type == RowType::kBookmark || type == RowType::kBookmarkFolder;
  }
  bool operator==(const Tab&) const = default;
};

// ProfileModel. sync_id and created are declared by V12 and, as on the Mac,
// written by nothing yet (profile identity lives in account defaults).
struct Profile {
  std::string profile_id;  // Chromium profile directory basename.
  std::string guid;
  std::optional<std::string> display_name;
  std::optional<std::string> bookmark_root;
  std::optional<std::string> sync_id;
  std::optional<TimeMs> created;
  bool operator==(const Profile&) const = default;
};

// SpaceModel.
struct Space {
  std::string space_id;
  std::string profile_id;
  std::string name;
  std::string color_hex;
  std::string icon_name;
  int sort_order = 0;
  TimeMs created = 0;
  TimeMs updated = 0;
  std::optional<std::string> bookmark_root;
  bool operator==(const Space&) const = default;
};

// SpaceURLRule (V12 sync columns included).
struct UrlRule {
  std::string id;
  std::string space_id;  // A Space id or kKioskRuleTargetId.
  std::string host;
  std::optional<std::string> path_prefix;
  bool ask = false;
  int sort_order = 0;
  TimeMs created = 0;
  std::optional<std::string> sync_id;
  std::optional<TimeMs> content_updated;
  std::optional<TimeMs> target_updated;
  std::optional<TimeMs> deleted;
  bool pending_local_edit = false;
  std::optional<std::string> merge_partner_sync_id;
  bool operator==(const UrlRule&) const = default;
};

// LocalStore.URLRuleDraft: one editor Save's worth of rule changes. Every
// unit is optional and nil means OMITTED — no write, no stamp.
struct UrlRuleDraft {
  struct Content {
    std::string host;
    std::optional<std::string> path_prefix;
    bool ask = false;
  };
  std::string id;  // Empty mints a new uppercase UUID.
  std::optional<std::string> sync_id;
  std::optional<Content> content;
  std::optional<std::string> space_id;
  std::optional<int> sort_order;
  std::optional<TimeMs> created;
  std::optional<TimeMs> content_updated;
};

// LocalStoreWriteError.
enum class WriteError {
  kNone,
  kUnavailable,
  kRowNotFound,
  kRowIsRoot,
  kInvalidUrl,
  kRowAlreadyMapped,
  kNotInActiveScope,
  kNoCandidateSurvived,
  kIo,
  kFolderNotEmpty,
};

// The time and id sources, injectable so the host tests are deterministic.
struct Env {
  std::function<TimeMs()> now;
  // A new UUID in the Mac's UUID().uuidString form (uppercase 8-4-4-4-12).
  std::function<std::string()> new_uuid;
};
Env DefaultEnv();
// A new id in the Mac's UUID().uuidString form (uppercase).
std::string NewUppercaseUuid();

class LocalStore {
 public:
  enum class OpenResult {
    kOk,
    // The manifest names a newer store format than this build reads
    // (the Mac's requiresNewerApp): nothing is opened or written.
    kRequiresNewerApp,
    kFailed,
  };

  explicit LocalStore(Env env = DefaultEnv());
  ~LocalStore();
  LocalStore(const LocalStore&) = delete;
  LocalStore& operator=(const LocalStore&) = delete;

  // Opens (creating) <dir>/LocalStore.sqlite and its manifest. `dir` is
  // UTF-8 and must exist. Closes any store already open.
  OpenResult Open(const std::string& dir);
  void Close();
  bool is_open() const { return db_ != nullptr; }
  const std::string& dir() const { return dir_; }

  // --- Reads (memory; valid until the next mutation) ---------------------
  const std::map<std::string, Tab>& tabs() const { return tabs_; }
  const Tab* FindTab(const std::string& guid) const;
  const Profile* FindProfile(const std::string& profile_id) const;
  const std::map<std::string, Profile>& profiles() const { return profiles_; }
  const Space* FindSpace(const std::string& space_id) const;
  // getAllSpaces: (sortOrder, profileId, createdDate).
  std::vector<Space> Spaces() const;
  // getAllURLRules: (spaceId, sortOrder); soft-deleted rows only on request.
  std::vector<UrlRule> UrlRules(bool include_deleted = false) const;
  PinScope pin_scope() const { return pin_scope_; }
  // Bumped by every ChangePinScope (the Mac's LocalStore.pinScopeGeneration):
  // a pin sync round samples it with its rows and refuses to commit or land
  // over a migration that ran since — a name compare alone misses a round
  // trip X → Y → X.
  uint64_t pin_scope_generation() const { return pin_scope_generation_; }
  // An arbitrary change counter, bumped by every successful mutation.
  uint64_t revision() const { return revision_; }

  // --- Profiles ------------------------------------------------------------
  // upsertProfileDisplayNames.
  bool UpsertProfileDisplayNames(
      const std::map<std::string, std::string>& names);

  // --- Pinned tabs -----------------------------------------------------------
  // pinnedTabs(profileId:spaceId:scope:) under the current scope, in
  // (index, guid) order.
  std::vector<const Tab*> ActivePins(const std::string& profile_id,
                                     const std::string& space_id) const;
  // activePinnedTab(resolving:…): the active row for `guid`, following a
  // stale guid from an inactive scope's backup through its lineage.
  const Tab* ResolveActivePin(const std::string& guid,
                              const std::string& profile_id,
                              const std::string& space_id) const;

  struct PinCreate {
    std::string guid;  // Empty mints one.
    std::string url;
    std::string title;
    std::string profile_id;
    std::string space_id = kDefaultSpaceId;
    std::optional<int> index;  // Nil appends.
    std::optional<std::string> lineage_id;
    std::optional<TimeMs> created;
    int source = 0;
    std::optional<TimeMs> content_updated;
    std::string favicon;
    int favicon_width = 0;
    int favicon_height = 0;
  };
  // createPinnedTabBody. Returns the guid, or empty (with `error`).
  std::string CreatePin(const PinCreate& in, WriteError* error = nullptr);
  // moveOrCreatePinnedTabBody's move half, with a target index: the row
  // leaves its place and the whole collection is renumbered.
  bool MovePin(const std::string& guid,
               const std::string& profile_id,
               const std::string& space_id,
               int new_index);
  // removePinnedTab: deletes the one active row. A partner's link back to it
  // is cleared (the Mac leaves it dangling; see the file comment).
  bool RemovePin(const std::string& guid,
                 const std::string& profile_id,
                 const std::string& space_id);
  // removePinnedTabUnit: deletes the row AND its split partner (the cell
  // the sidebar shows), then renumbers what is left of the collection.
  bool RemovePinUnit(const std::string& guid);
  // Moves the cell `guid` belongs to (one row, or a split pair together) to
  // cell position `unit_index` in its collection, renumbering the rows.
  bool MovePinUnit(const std::string& guid, int unit_index);
  // The collection's cells in display order: each is one row, or a split
  // pair (primary first) — the Mac's LibrarySpaceContents.pinnedItems
  // pairing, which tolerates a half-written link.
  struct PinUnit {
    // Into the store's own rows; valid until the next mutation.
    RAW_PTR_EXCLUSION const Tab* primary = nullptr;
    RAW_PTR_EXCLUSION const Tab* secondary = nullptr;
  };
  std::vector<PinUnit> PinUnits(const std::string& profile_id,
                                const std::string& space_id) const;
  // updateActivePinnedTabBody: the exact row, which must be in the active
  // scope; stamps contentUpdatedDate only on a real change; a url write
  // also flags needUpdateMetaData.
  bool UpdatePin(const std::string& guid,
                 const std::optional<std::string>& url,
                 const std::optional<std::string>& title);
  // updatePinnedSplitPair / updateTabSplitPartnerBody(nil) /
  // updatePinnedSplitLayout.
  bool SetPinSplitPair(const std::string& primary,
                       const std::string& secondary,
                       const std::string& layout);
  bool ClearPinSplitPartner(const std::string& guid);
  bool SetPinSplitLayout(const std::string& first,
                         const std::string& second,
                         const std::string& layout);
  // Swaps the two rows' places in their collection (pane order IS index
  // order). False unless `guid` has a partner in the same collection.
  bool SwapPinSplitOrder(const std::string& guid);
  // transferPinnedTab: moves a pin (with its split partner) to another
  // Space's collection. Same owner under the scope = a reorder; another
  // owner = a copy with fresh guids and the same lineage, then the source is
  // deleted. Returns the moved primary's guid (new when copied), or empty.
  std::string TransferPin(const std::string& guid,
                          const std::string& source_profile_id,
                          const std::string& source_space_id,
                          const std::string& target_profile_id,
                          const std::string& target_space_id,
                          int destination_index);
  // changePinnedTabScope + migratePinnedTabs. The preferred owner decides
  // which collection's order wins when collections merge. False when the
  // scope is already `scope` or the write failed.
  bool ChangePinScope(PinScope scope,
                      const std::string& preferred_profile_id,
                      const std::string& preferred_space_id);

  // --- Bookmarks -------------------------------------------------------------
  // bookmarkRoot(profileId:spaceId:createIfNeeded:). The root is a real
  // folder row with no parent, never shown and never synced. Empty when it
  // does not exist and `create` is false.
  std::string BookmarkRoot(const std::string& profile_id,
                           const std::string& space_id,
                           bool create);
  // existingBookmarkRoot: no heal, no create.
  std::string ExistingBookmarkRoot(const std::string& profile_id,
                                   const std::string& space_id) const;
  bool IsBookmarkRoot(const std::string& guid) const;
  // children(of:): (index, guid).
  std::vector<const Tab*> Children(const std::string& parent_guid) const;

  struct BookmarkCreate {
    std::string profile_id;
    std::string space_id = kDefaultSpaceId;
    std::optional<std::string> parent_guid;  // Nil or unknown = the root.
    std::optional<int> index;                // Nil appends.
    std::string guid;                        // Empty mints one.
    std::string title;
    std::string url;
    std::optional<std::string> secondary_url;
    std::optional<std::string> secondary_title;
    std::optional<std::string> layout;
    std::string favicon;
    int favicon_width = 0;
    int favicon_height = 0;
    std::string secondary_favicon;
    int secondary_favicon_width = 0;
    int secondary_favicon_height = 0;
    int source = 0;
  };
  // createBookmarkBody / insertBookmarkNode: an empty title becomes the url.
  std::string CreateBookmark(const BookmarkCreate& in,
                             WriteError* error = nullptr);
  // createDirectoryBody / insertDirectoryNode.
  std::string CreateFolder(const std::string& profile_id,
                           const std::string& space_id,
                           const std::optional<std::string>& parent_guid,
                           const std::optional<int>& index,
                           const std::string& title,
                           const std::string& guid = std::string(),
                           int source = 0);
  // moveBookmarkBody (the UI path): records locationUpdatedDate only when
  // the parent changes. A folder never moves into itself or a descendant.
  bool MoveBookmark(const std::string& guid,
                    const std::string& profile_id,
                    const std::optional<std::string>& parent_guid,
                    int new_index);
  // moveBookmarksBody (the UI path): moves rows into another Space's tree,
  // retagging each subtree; only the moved rows record a location edit.
  bool MoveBookmarksToSpace(const std::vector<std::string>& guids,
                            const std::string& source_profile_id,
                            const std::string& target_profile_id,
                            const std::string& target_space_id,
                            const std::optional<std::string>& target_parent,
                            const std::optional<int>& destination_index);

  struct BookmarkUpdate {
    std::optional<std::string> title;
    std::optional<std::string> url;
    // Outer optional = change it; inner nullopt = clear it.
    std::optional<std::optional<std::string>> secondary_url;
    std::optional<std::optional<std::string>> secondary_title;
    std::optional<std::string> icon;  // Folders only.
    bool allows_empty_title = false;
  };
  // updateBookmarkBody: stamps contentUpdatedDate only on a real change.
  bool UpdateBookmark(const std::string& guid, const BookmarkUpdate& update);
  // updateBookmarkSplitLayout: layout is not synced, so no stamp.
  bool SetBookmarkSplitLayout(const std::string& guid,
                              const std::string& layout);
  // Swaps a split bookmark's panes (content edit: stamps).
  bool SwapBookmarkPanes(const std::string& guid);
  // deleteBookmarkBody: the subtree goes with a folder; siblings renumber.
  bool DeleteBookmark(const std::string& guid);

  // --- Any row ---------------------------------------------------------------
  // updateTabFavicon / updateLastSeen. Neither is sync-visible.
  bool SetFavicon(const std::string& guid,
                  bool pane_b,
                  const std::string& bgra,
                  int width,
                  int height);
  bool SetLastSeen(const std::string& guid, TimeMs seen_at);

  // --- Spaces ---------------------------------------------------------------
  // ensureDefaultSpace: creates `default-space` only in an EMPTY store (the
  // row may be a deleted Space whose role has moved).
  bool EnsureDefaultSpace(const std::string& profile_id);
  // createSpaceBody: appended to the global order; its bookmark root is
  // materialised at once.
  bool CreateSpace(const std::string& space_id,
                   const std::string& profile_id,
                   const std::string& name,
                   const std::string& color_hex,
                   const std::string& icon_name,
                   std::optional<TimeMs> created = std::nullopt);
  // updateSpaceBody: updatedDate moves on every call, as on the Mac.
  bool UpdateSpace(const std::string& space_id,
                   const std::optional<std::string>& name,
                   const std::optional<std::string>& color_hex,
                   const std::optional<std::string>& icon_name,
                   std::optional<TimeMs> created = std::nullopt);
  // changeSpaceProfileBody: refused for default-space; the Space's
  // bookmarks (and its pins under Space scope) move with it.
  bool ChangeSpaceProfile(const std::string& space_id,
                          const std::string& profile_id);
  // deleteSpaceCascadeBody: rows are hard-deleted; the Space's URL rules are
  // soft-deleted on a user delete (sync publishes their tombstones) and
  // hard-deleted on a retention purge.
  bool DeleteSpaceCascade(const std::string& space_id, bool user_intent);
  // reorderSpacesBody: sortOrder = position; no updatedDate.
  bool ReorderSpaces(const std::vector<std::string>& ordered_space_ids);

  // --- Sync landing (docs/user-data-parity.md S5) ----------------------------
  // One engine landing's bookmark writes (applyBookmarkSyncBatchBody), all
  // or nothing in ONE flush. Landing is not editing: no content or location
  // edit stamp, except a lift the engine asks to record.
  struct BookmarkSyncOp {
    enum class Kind { kClaim, kCreate, kMove, kUpdate, kDelete };
    Kind kind = Kind::kCreate;
    std::string guid;
    std::optional<std::string> sync_id;      // kClaim / kCreate
    // kCreate / kMove: nullopt parent = the Space root.
    std::optional<std::string> parent_guid;
    std::string space_id;
    int index = 0;
    // kCreate.
    std::string profile_id;
    bool is_folder = false;
    std::string title;
    std::string url;
    std::optional<std::string> secondary_url;
    std::optional<std::string> secondary_title;
    int source = 0;
    TimeMs created = 0;
    std::optional<TimeMs> content_updated;
    bool records_location_edit = false;  // kMove
    // kUpdate: outer = write, inner nullopt = clear.
    std::optional<std::string> new_title;
    std::optional<std::string> new_url;
    std::optional<std::optional<std::string>> new_secondary_url;
    std::optional<std::optional<std::string>> new_secondary_title;
  };
  WriteError ApplyBookmarkSyncBatch(const std::vector<BookmarkSyncOp>& ops);
  // The same for pins (applyPinSyncBatchBody), under the current scope.
  struct PinSyncOp {
    enum class Kind { kCreate, kRelineage, kMove, kUpdate, kDelete };
    Kind kind = Kind::kCreate;
    std::string guid;
    // kCreate (the owner is the scope's: Space → both ids, profile → the
    // profile, app → neither).
    std::optional<std::string> profile_id;
    std::optional<std::string> space_id;
    std::string lineage_id;  // kCreate / kRelineage
    std::string title;
    std::string url;
    int source = 0;
    TimeMs created = 0;
    std::optional<TimeMs> content_updated;
    int index = 0;  // kCreate / kMove
    // kUpdate.
    std::optional<std::string> new_title;
    std::optional<std::string> new_url;
    // Outer = write; inner nullopt = unlink; else the partner's lineage.
    std::optional<std::optional<std::string>> split_partner_lineage;
  };
  WriteError ApplyPinSyncBatch(const std::vector<PinSyncOp>& ops);
  // Self-removal: bookmarks and folders forget their account identity
  // (clearAllBookmarkSyncIds); pin lineages and rule ids are kept.
  bool ClearBookmarkSyncIds();
  // A URL rule sync batch (computed by phi_sync_rules over the whole table,
  // soft-deleted rows included): the table becomes `rows`, in one flush.
  WriteError ReplaceUrlRulesForSync(const std::vector<UrlRule>& rows);
  // Called after every successful write of rows sync reads (not favicons,
  // not last-seen): the host's local-change signal.
  void set_on_rows_changed(std::function<void()> callback) {
    on_rows_changed_ = std::move(callback);
  }

  // --- Guest migration (docs/user-data-parity.md S6) ------------------------
  // importGuestData: one planned Guest import (phi_guest_migration.h), all
  // or nothing in ONE flush. An import already present is success; a
  // partial one is refused. `folder_title` names the default-Space
  // bookmarks' "Imported from Guest" folder. Defined in
  // phi_guest_migration.cc.
  bool ImportGuestData(const GuestSnapshot& snapshot,
                       const GuestMappings& mappings,
                       const std::string& folder_title,
                       std::string* error);

  // --- URL rules --------------------------------------------------------------
  // applyURLRuleEditsBody: the one editor/agent write entry, all-or-nothing.
  WriteError ApplyUrlRuleEdits(const std::vector<UrlRuleDraft>& upserts,
                               const std::set<std::string>& deleted_ids);

 private:
  struct Dirty;

  bool LoadAll();
  // Writes what `dirty_` names in one transaction; on failure reloads the
  // memory from disk and returns false. Either way `dirty_` is emptied.
  bool Flush();
  void Discard();

  void PutTab(const Tab& tab);
  void DeleteTab(const std::string& guid);
  void PutSpace(const Space& space);
  void DeleteSpace(const std::string& space_id);
  void PutRule(const UrlRule& rule);
  void DeleteRule(const std::string& id);
  void PutProfile(const Profile& profile);
  Profile& EnsureProfile(const std::string& profile_id);

  // Pins.
  struct Owner {
    std::optional<std::string> profile_id;
    std::optional<std::string> space_id;
    bool operator==(const Owner&) const = default;
    bool operator<(const Owner& o) const { return SortKey() < o.SortKey(); }
    std::string SortKey() const;
  };
  static Owner OwnerFor(PinScope scope,
                        const std::string& profile_id,
                        const std::string& space_id);
  static bool BelongsTo(const Tab& tab, PinScope scope);
  static Owner OwnerOf(const Tab& tab, PinScope scope);
  std::vector<Tab*> MutableActivePins(const Owner& owner);
  static std::vector<std::pair<Tab*, Tab*>> Units(
      const std::vector<Tab*>& pins);
  void RenumberPins(const std::vector<Tab*>& pins, TimeMs now);
  void ApplyOwner(Tab& tab, const Owner& owner);
  void MigratePins(PinScope from,
                   PinScope to,
                   const std::string& preferred_profile_id,
                   const std::string& preferred_space_id);

  // Bookmarks.
  // A new root row for (profile, Space), linked, NOT flushed.
  std::string NewBookmarkRoot(const std::string& profile_id,
                              const std::string& space_id);
  std::string ResolveParent(const std::optional<std::string>& parent_guid,
                            const std::string& profile_id,
                            const std::string& space_id);
  std::vector<Tab*> MutableChildren(const std::string& parent_guid);
  void NormalizeIndexes(const std::vector<Tab*>& nodes, TimeMs now);
  void InsertNode(Tab& node,
                  const std::string& parent_guid,
                  const std::optional<int>& index,
                  TimeMs now);
  bool HasAncestor(const Tab& node, const std::set<std::string>& guids) const;
  void RetagSubtree(const std::string& guid,
                    const std::string& profile_id,
                    const std::string& space_id,
                    TimeMs now);
  void CollectSubtree(const std::string& guid,
                      std::vector<std::string>* out) const;

  Env env_;
  // sqlite3 is a C handle, not a PartitionAlloc object.
  RAW_PTR_EXCLUSION sqlite3* db_ = nullptr;
  std::string dir_;
  std::map<std::string, Tab> tabs_;
  std::map<std::string, Profile> profiles_;
  std::map<std::string, Space> spaces_;
  std::map<std::string, UrlRule> rules_;
  PinScope pin_scope_ = PinScope::kProfile;
  uint64_t pin_scope_generation_ = 0;
  bool settings_row_exists_ = false;
  uint64_t revision_ = 0;
  std::function<void()> on_rows_changed_;
  // Set around favicon / last-seen writes: they are not sync-visible.
  bool quiet_flush_ = false;

  struct Dirty {
    std::set<std::string> tabs, tabs_deleted;
    std::set<std::string> spaces, spaces_deleted;
    std::set<std::string> rules, rules_deleted;
    std::set<std::string> profiles;
    bool settings = false;
    bool empty() const {
      return tabs.empty() && tabs_deleted.empty() && spaces.empty() &&
             spaces_deleted.empty() && rules.empty() &&
             rules_deleted.empty() && profiles.empty() && !settings;
    }
  } dirty_;
};

}  // namespace phi_client::store

#endif  // PHIBROWSER_WIN_CLIENT_PHI_LOCAL_STORE_H_

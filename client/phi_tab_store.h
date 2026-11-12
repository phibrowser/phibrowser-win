// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_TAB_STORE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_TAB_STORE_H_

#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_local_store.h"

namespace phi_client {

// Pinned tabs and bookmarks as the sidebar and the verbs see them: CELLS.
// The rows themselves live in the account's local store
// (phi_local_store.h, the Mac's LocalStore schema), where a split PIN is two
// rows linked by splitPartnerGuid — as on the Mac and on the sync wire —
// and a split BOOKMARK is one row with a second pane. This class is the
// adapter between the two shapes: it composes a pinned pair into one cell
// (pane B riding in url_b/title_b/favicon_b) and turns a cell operation into
// the store's operations on the rows behind it. It also keeps the
// sidebar's spelling of a bookmark tree, where the Space's root folder is
// the empty parent ("") and never a row.
//
// Favicons cross this boundary base64-encoded (the framework's BGRA, as the
// verbs carry them) — or raw, for the sidebar, which paints the bytes and
// would only decode them again; the store keeps the raw bytes.

enum class PhiRowType { kPinnedTab = 1, kBookmark = 2, kBookmarkFolder = 3 };

// How a reader wants a row's favicons: base64 (the verbs' JSON), the raw
// BGRA bytes (the sidebar's paint rows, no encode/decode round trip), or
// not at all (the sidebar's tree, which reads only guid/kind/parent/index —
// on EVERY rebuild).
enum class PhiFaviconForm { kBase64, kRaw, kNone };

// WHERE A PINNED TAB LIVES — the Mac's PinnedTabScope (see store::PinScope).
using PhiPinScope = store::PinScope;
const char* PhiPinScopeName(PhiPinScope scope);
bool PhiPinScopeFromName(const std::string& name, PhiPinScope* out);

struct PhiRow {
  std::string guid;  // A pinned pair's cell is its FIRST row's guid.
  PhiRowType type = PhiRowType::kBookmark;
  std::string title;
  std::string url;
  // Pins: the cell's position in its collection. Bookmarks: the position
  // among the folder's children.
  int index = 0;
  std::string profile_id;
  std::string space_id;
  std::string parent_guid;  // Bookmarks only; "" = the Space's root.
  std::string favicon_base64;  // PhiFaviconForm::kBase64 only.
  // PhiFaviconForm::kRaw only: the premultiplied BGRA, exactly
  // width*height*4 bytes (a buffer of any other size is dropped).
  std::string favicon_raw;
  int favicon_width = 0;
  int favicon_height = 0;
  std::string lineage_id;
  // Split cell: url_b non-empty. For a pin, pane B is the partner row
  // (partner_guid); for a bookmark, the row's own second pane.
  std::string url_b;
  std::string title_b;
  std::string favicon_b_base64;
  std::string favicon_b_raw;
  int favicon_b_width = 0;
  int favicon_b_height = 0;
  std::string partner_guid;
  // A split cell of three or four panes: the panes after B, in order. Pins
  // and bookmarks alike keep them on the cell's own row, local to this
  // device (sync carries the first two panes).
  struct ExtraPane {
    std::string url;
    std::string title;
    std::string favicon_base64;  // PhiFaviconForm::kBase64 only.
    std::string favicon_raw;     // PhiFaviconForm::kRaw only.
    int favicon_width = 0;
    int favicon_height = 0;
  };
  std::vector<ExtraPane> more;
  std::string icon;    // Bookmark folders: the catalog symbol.
  std::string layout;  // Split cells: "vertical" (default) / "horizontal".
};

class PhiTabStore {
 public:
  explicit PhiTabStore(store::LocalStore& store);
  ~PhiTabStore();

  PhiTabStore(const PhiTabStore&) = delete;
  PhiTabStore& operator=(const PhiTabStore&) = delete;

  PhiPinScope pin_scope() const { return store_.pin_scope(); }
  // The Mac's changePinnedTabScope; `preferred_*` name the window the change
  // came from, whose collection's order wins where collections merge.
  bool SetPinScope(PhiPinScope scope,
                   const std::string& preferred_profile_id = std::string(),
                   const std::string& preferred_space_id = std::string());

  // Pinned cells visible to (profile, space) under the current scope.
  std::vector<PhiRow> PinnedFor(
      const std::string& profile_id,
      const std::string& space_id,
      PhiFaviconForm favicons = PhiFaviconForm::kBase64) const;
  std::string AddPinned(const std::string& profile_id,
                        const std::string& space_id,
                        const std::string& title,
                        const std::string& url,
                        const std::string& favicon_base64,
                        int favicon_width,
                        int favicon_height);

  // Bookmarks directly under `parent_guid` ("" = the Space's root).
  std::vector<PhiRow> BookmarksFor(
      const std::string& profile_id,
      const std::string& space_id,
      const std::string& parent_guid,
      PhiFaviconForm favicons = PhiFaviconForm::kBase64) const;
  // The whole owned tree, every depth, in no particular order.
  std::vector<PhiRow> BookmarkTree(
      const std::string& profile_id,
      const std::string& space_id,
      PhiFaviconForm favicons = PhiFaviconForm::kBase64) const;
  std::string AddBookmark(const std::string& profile_id,
                          const std::string& space_id,
                          const std::string& parent_guid,
                          const std::string& title,
                          const std::string& url,
                          bool folder,
                          const std::string& favicon_base64,
                          int favicon_width,
                          int favicon_height,
                          store::Source source = store::Source::kPhi);

  // Removes a cell (both rows of a pinned pair) or a bookmark with its
  // subtree.
  bool Remove(const std::string& guid);
  bool Contains(const std::string& guid) const;
  // Sets (or with an empty url_b clears) a cell's second pane. For a pin
  // that creates, updates or deletes the partner row.
  bool SetRowSecondary(const std::string& guid,
                       const std::string& url_b,
                       const std::string& title_b,
                       const std::string& favicon_b_base64 = std::string(),
                       int favicon_b_width = 0,
                       int favicon_b_height = 0);
  // Sets the panes after the second of a split cell (`favicon_base64`
  // filled, at most two), or with none clears them. False for a cell that
  // has no second pane.
  bool SetRowExtraPanes(const std::string& guid,
                        const std::vector<PhiRow::ExtraPane>& panes);
  bool RenameRow(const std::string& guid, const std::string& title);
  bool SetRowIcon(const std::string& guid, const std::string& icon);
  bool SetRowLayout(const std::string& guid, const std::string& layout);
  // Swaps a split cell's panes. Returns the cell's guid AFTERWARDS (a
  // pinned pair's cell is named by its first row, which a swap changes),
  // or empty when `guid` is no split cell.
  std::string SwapPanes(const std::string& guid);
  // A Space's deletion and re-binding (the Mac's deleteSpaceCascade /
  // changeSpaceProfile), which take the Space's rows with them.
  bool RemoveForSpace(const std::string& space_id);
  bool RehomeSpace(const std::string& space_id, const std::string& profile_id);
  // Moves a cell among its siblings (pins: cell positions).
  bool Reorder(const std::string& guid, int new_index);
  // The cell `guid` names (either row of a pinned pair).
  std::optional<PhiRow> Find(const std::string& guid) const;
  // Rewrites a cell's url (pane A). The stored favicon belonged to the old
  // site and is dropped.
  bool SetRowUrl(const std::string& guid, const std::string& url);
  // Reparents a bookmark under `new_parent` ("" = the root) at `new_index`
  // (negative appends).
  bool Move(const std::string& guid,
            const std::string& new_parent,
            int new_index);
  // Stores a favicon for a cell's pane, if the cell still shows `url`.
  bool SetFavicon(const std::string& guid,
                  bool pane_b,
                  const std::string& favicon_base64,
                  int width,
                  int height);

 private:
  PhiRow CellFromPin(const store::Tab& a,
                     const store::Tab* b,
                     int index,
                     PhiFaviconForm favicons) const;
  PhiRow CellFromBookmark(const store::Tab& row, PhiFaviconForm favicons) const;
  // The Space root → "" translation for a bookmark's parent.
  std::string ParentForUi(const store::Tab& row) const;
  std::optional<std::string> ParentForStore(const std::string& profile_id,
                                            const std::string& space_id,
                                            const std::string& parent) const;

  RAW_PTR_EXCLUSION store::LocalStore& store_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_TAB_STORE_H_

// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_MODEL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_MODEL_H_

#include <map>
#include <string>
#include <vector>

// The sidebar's model — docs/sidebar-architecture.md §4. Stored identity
// (the store), live content (the framework strip) and presentation (the
// controller) are separate axes; BuildSidebar() is the ONE place that
// decides what a live tab is: plain, pin-bound, bookmark-bound, split pane,
// surface, grouped. Pure functions over snapshots: no HWND, pixels, base/ or
// framework here, so this file compiles and is tested on the Linux host
// (test/model/run.sh).

namespace phi_client {

// ---- The tag ---------------------------------------------------------------
// A live tab bound to a stored cell carries the cell's guid as its framework
// tag (PhiTabData::custom_value, session-restored by Chromium):
//   <guid>     pane A, or the whole tab of a single (non-split) cell
//   <guid>#b   pane B of a split cell
//   <guid>#c   the third pane of a split cell of three or four
//   <guid>#d   the fourth
// Nothing else is ever written into the tag by this client — no pending
// markers, no placeholders. These functions are the only codec.
inline constexpr char kSidebarPaneBSuffix[] = "#b";
inline constexpr int kSidebarMaxPanes = 4;
std::string TagFor(const std::string& guid, bool pane_b);
// `pane` 0 to 3; anything else reads as pane A.
std::string TagForPane(const std::string& guid, int pane);
struct SidebarTag {
  std::string guid;
  int pane = 0;  // 0 = pane A … 3 = the fourth pane.
  bool pane_b = false;  // pane == 1.
};
// False for an empty tag. A tag with no suffix is pane A.
bool ParseTag(const std::string& tag, SidebarTag* out);

// ---- Keys ------------------------------------------------------------------
// One identity type for every row, hit target, drag payload, hover and
// selection. Never a vector index; never an Int/String namespace mix.
struct SidebarKey {
  // kSpace is the Spaces strip's pip — not a sidebar row, but it drags
  // through the same drag state, so it shares the key type.
  enum class Kind { kNone, kTab, kSplit, kGroup, kCell, kFolder, kSpace };
  Kind kind = Kind::kNone;
  std::string id;  // "7" (session id) | split token | group token | guid

  static SidebarKey Tab(int session_id);
  static SidebarKey Split(const std::string& split_id);
  static SidebarKey Group(const std::string& group_id);
  static SidebarKey Cell(const std::string& guid);
  static SidebarKey Folder(const std::string& guid);
  static SidebarKey Space(const std::string& space_id);

  bool empty() const { return kind == Kind::kNone; }
  // kTab only; 0 otherwise.
  int tab_id() const;
  std::string ToString() const;  // "tab:7", "split:…", "group:…", "cell:…"

  friend bool operator==(const SidebarKey& a, const SidebarKey& b) {
    return a.kind == b.kind && a.id == b.id;
  }
  friend bool operator!=(const SidebarKey& a, const SidebarKey& b) {
    return !(a == b);
  }
  friend bool operator<(const SidebarKey& a, const SidebarKey& b) {
    return a.kind != b.kind ? a.kind < b.kind : a.id < b.id;
  }
};

// ---- Inputs ----------------------------------------------------------------
enum class SidebarStoredKind { kPin, kBookmark, kFolder };
// One stored row. The bookmark set must be the WHOLE owned tree, including
// descendants of collapsed folders: expansion is presentation state and
// cannot decide whether a tab binds.
struct SidebarStoredItem {
  std::string guid;
  SidebarStoredKind kind = SidebarStoredKind::kBookmark;
  std::string parent_guid;  // Bookmarks/folders: the tree. Pins: empty.
  int index = 0;            // Order among siblings of the same kind+parent.
  bool split = false;       // Pins and bookmarks alike: the cell has a pane B.
  int extra = 0;            // Split cells: panes after B (0 to 2).
};

enum class SidebarSurface { kContent, kPeek, kReader };
struct SidebarLiveTab {
  int id = 0;      // Stable session identity. Never use a strip index as a key.
  int index = -1;  // Valid only within this snapshot.
  std::string tag;
  std::string split_id;
  std::string group_id;
  bool active = false;
  // A reading/peek surface that is (still) in this strip is never a row; an
  // active one hands the selection to its origin.
  SidebarSurface surface = SidebarSurface::kContent;
  int origin_id = 0;
};

struct SidebarLiveGroup {
  std::string id;
  bool collapsed = false;
};

struct SidebarPolicy {
  // Comfortable: pins stay hidden from the strip, bookmark-bound tabs return
  // to it as plain chips. The ONLY place layout mode touches membership.
  bool horizontal_layout = false;
  bool incognito = false;  // No stored cells at all.
};

struct SidebarInputs {
  std::vector<SidebarStoredItem> stored;   // Pins + the whole bookmark tree.
  std::vector<SidebarLiveTab> strip;
  std::vector<SidebarLiveGroup> groups;
  std::map<int, SidebarSurface> surfaces;  // origin tab id -> attached kind.
  SidebarPolicy policy;
};

// ---- Outputs ---------------------------------------------------------------
enum class SidebarBindingState { kClosed, kOpen, kSplitOpen, kPartial };
struct SidebarBinding {
  SidebarBindingState state = SidebarBindingState::kClosed;
  int tab_a = 0;  // Session ids; 0 = no live pane.
  int tab_b = 0;
  // A cell of three or four panes: the panes after B, one entry each (0 =
  // no live pane). Empty for a single cell and a pair.
  std::vector<int> more;
  std::string split_id;  // kSplitOpen: the live split every pane shares.
  bool active = false;   // A live pane is the strip's active tab.
  bool open() const { return !TabIds().empty(); }
  // The live tab of pane `pane` (0 = A), or 0.
  int TabOfPane(int pane) const {
    if (pane <= 0) return tab_a;
    if (pane == 1) return tab_b;
    const size_t i = static_cast<size_t>(pane - 2);
    return i < more.size() ? more[i] : 0;
  }
  std::vector<int> TabIds() const;
};

struct SidebarCell {
  SidebarKey key;  // kCell, or kFolder.
  std::string guid;
  SidebarStoredKind kind = SidebarStoredKind::kBookmark;
  std::string parent_guid;
  int depth = 0;       // Bookmarks: outline depth (root children = 0).
  bool split = false;  // The cell has a pane B (two grid units).
  int extra = 0;       // Panes after B.
  SidebarBinding binding;
  // Folders: a descendant cell is active. Groups use the content node.
  bool contains_active = false;
  SidebarSurface attached_surface = SidebarSurface::kContent;
};

enum class SidebarNodeKind { kGroup, kTab, kSplit };
struct SidebarContentNode {
  SidebarKey key;
  SidebarNodeKind kind = SidebarNodeKind::kTab;
  int tab_a = 0;  // kTab: the tab. kSplit: strip-order first pane.
  int tab_b = 0;  // kSplit: second pane.
  // kSplit of three or four panes: the panes after the second, in order.
  std::vector<int> more;
  // Every pane's tab, in order: one for kTab, two to four for kSplit.
  std::vector<int> Panes() const;
  std::string group_id;  // A container relationship, not a type of tab.
  bool collapsed = false;        // kGroup header.
  bool hidden = false;           // Member of a collapsed group.
  bool active = false;           // kTab/kSplit: holds the active tab.
  bool contains_active = false;  // kGroup: a member holds it.
  SidebarSurface attached_surface = SidebarSurface::kContent;
};

struct SidebarTree {
  std::vector<SidebarCell> pins;       // Store order.
  std::vector<SidebarCell> bookmarks;  // Depth-first, whole tree.
  // Strip order. A group header precedes its first member; a group's members
  // follow in strip order (Chromium keeps them contiguous; a header is never
  // emitted twice). A tab whose group is unknown renders flat.
  std::vector<SidebarContentNode> content;
  std::map<int, SidebarKey> owner_of;  // Live tab -> the cell hiding it.
  std::map<int, int> indices;          // Live tab -> strip index (this snapshot).
  SidebarKey active_key;               // The row that highlights (§4.8).
  int active_tab = 0;                  // After surface -> origin redirection.

  const SidebarCell* CellFor(const std::string& guid) const;
  const SidebarBinding* BindingFor(const std::string& guid) const;
  int IndexOf(int tab_id) const;
  bool Hidden(int tab_id) const { return owner_of.count(tab_id) > 0; }
};

// Pure reconciliation. Every live content tab appears exactly once: as a
// content node or in owner_of. Invalid, stale or conflicting relationships
// never hide otherwise reachable content: a duplicate tag binds the first
// strip tab and the others stay plain; a split cell binds as kSplitOpen only
// when both panes are live and share one live split; a pane sitting in an
// unrelated split is never hidden; a lone live pane outside any split is a
// recoverable kPartial. Splits are resolved by split id, never by
// adjacency. No repairs, no writes.
SidebarTree BuildSidebar(const SidebarInputs& in);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_MODEL_H_

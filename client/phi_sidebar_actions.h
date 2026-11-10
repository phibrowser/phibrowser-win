// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_ACTIONS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_ACTIONS_H_

#include <string>
#include <variant>

#include "phi_sidebar_model.h"

// The sidebar's ONE verb set — docs/sidebar-architecture.md §5. Every gesture,
// menu item and shortcut produces a SidebarCommand; SidebarHost::Apply is the
// only mutator, dispatching Move over the (what the key is) × (where it goes)
// table. The types and the place helpers here are pure (host-tested); the
// effects live in the coordinator, which owns the store and the framework.

namespace phi_client {

// Where an item goes. `before` is the anchor row in the destination; empty
// = the end of the section (or of `parent_guid` / `group_id`).
struct SidebarPlace {
  enum class Section { kTabs, kPinned, kBookmarks };
  Section section = Section::kTabs;
  std::string parent_guid;  // kBookmarks: the folder ("" = root).
  // kTabs: the destination group. Meaningful only when `group_explicit`;
  // otherwise Chromium assigns the group the destination sits in.
  std::string group_id;
  bool group_explicit = false;
  SidebarKey before;
};

struct SidebarMove {
  SidebarKey key;  // kTab, kSplit, kCell (pin or bookmark) or kFolder.
  SidebarPlace place;
};
// Activate the row's tab; `pane` names a pane of a split row/cell (0 = the
// first, up to 3 in a split row of four).
struct SidebarActivate {
  SidebarKey key;
  int pane = 0;
};
// Close the live tab(s): a tab/split row closes its panes; a cell closes its
// bound tabs and stays as a closed cell.
struct SidebarClose {
  SidebarKey key;
  int pane = -1;  // -1 = every pane; 0 to 3 = that pane only.
};
// Delete a stored cell (unpin / remove bookmark / delete folder); its live
// tabs stay as plain tabs.
struct SidebarRemoveCell {
  SidebarKey key;
};
// Pair two rows' tabs as a split with `a` primary; both are normalised to
// plain first (a split never lives in a stored cell by accident). When
// `a`'s tab is already a pane of a split with room (fewer than four panes),
// `b` joins that split right after it instead; a full split refuses.
struct SidebarFormSplit {
  SidebarKey a;
  SidebarKey b;
  std::string layout;  // "vertical" | "horizontal".
};
struct SidebarBreakSplit {
  SidebarKey key;  // kSplit row or a split cell.
};
struct SidebarSwapPanes {
  SidebarKey key;
};
// Take one pane out of a split row: a pair dissolves, a wider split keeps
// the rest and the tab lands right after it.
struct SidebarRemovePane {
  SidebarKey key;  // kSplit row.
  int pane = 0;
};
// Move one pane of a split row of three or four to another slot.
struct SidebarMovePane {
  SidebarKey key;  // kSplit row.
  int pane = 0;
  int slot = 0;
};
struct SidebarSetLayout {
  SidebarKey key;  // kSplit row (live) or a split cell (stored + live).
  std::string layout;
};
// Group membership by id: "" = a new group.
struct SidebarJoinGroup {
  SidebarKey key;
  std::string group_id;
};
struct SidebarLeaveGroup {
  SidebarKey key;
};
struct SidebarSetGroupCollapsed {
  SidebarKey key;  // kGroup.
  bool collapsed = false;
};
// An UNBOUND bookmark of a tab/split row at the root (the menu's Bookmark,
// the Mac's addTabToRootBookmarks — the drag path binds, the menu copies).
struct SidebarBookmarkCopy {
  SidebarKey key;
};
// Close the Peek or Reader surface attached to the row's tab (either pane)
// — the sidebar indicator's minus (docs/sidebar-architecture.md §4.6).
struct SidebarCloseSurface {
  SidebarKey key;
};
// A cell's tabs to another Space (bookmark cells only today).
struct SidebarMoveToSpace {
  SidebarKey key;
  std::string space_id;
  bool clone = false;
};

using SidebarCommand = std::variant<SidebarMove,
                                    SidebarActivate,
                                    SidebarClose,
                                    SidebarRemoveCell,
                                    SidebarFormSplit,
                                    SidebarBreakSplit,
                                    SidebarSwapPanes,
                                    SidebarRemovePane,
                                    SidebarMovePane,
                                    SidebarSetLayout,
                                    SidebarJoinGroup,
                                    SidebarLeaveGroup,
                                    SidebarSetGroupCollapsed,
                                    SidebarBookmarkCopy,
                                    SidebarCloseSurface,
                                    SidebarMoveToSpace>;

// ---- Place helpers (pure) --------------------------------------------------

// The stored sibling index `place.before` means inside its parent, counting
// the cells of the destination kind under `place.parent_guid` (pins: all
// pins) in tree order and skipping `moving_guid` (a row leaving its slot does
// not count as its own anchor). Empty anchor, or an anchor not in that
// parent, = the end (a large index; the store clamps).
int PlaceSiblingIndex(const SidebarTree& tree,
                      const SidebarPlace& place,
                      const std::string& moving_guid);

// The live tab MoveTabBefore anchors on: the anchor row's first pane, or 0
// for "the end". A cell anchor (kCell) means its pane A when open, else 0.
int PlaceAnchorTab(const SidebarTree& tree, const SidebarPlace& place);

// The tab a key stands for in the strip: a tab row's tab, a split row's
// first pane, a cell's pane A (0 when closed). `pane` selects a later pane
// (a cell has two at most).
int TabOfKey(const SidebarTree& tree, const SidebarKey& key, int pane = 0);

// True when moving `key` to `place` would land it where it already is.
bool MoveIsNoop(const SidebarTree& tree,
                const SidebarKey& key,
                const SidebarPlace& place);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_ACTIONS_H_

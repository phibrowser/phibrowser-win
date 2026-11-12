// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_DROP_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_DROP_H_

#include <string>
#include <vector>

#include "phi_sidebar_actions.h"
#include "phi_sidebar_layout.h"
#include "phi_sidebar_model.h"

// Where a drag lands — docs/sidebar-architecture.md §5. Pure functions over
// the placed rows of one section (key + rect + the little tree/group context
// a target needs) and the pointer; the controller feeds them its layout rows
// and gets back a SidebarPlace plus what to paint. Host-tested.

namespace phi_client {

struct DropRow {
  SidebarKey key;
  SidebarRect bounds;          // Empty = not on screen (hidden/clipped).
  // Bookmark outline context.
  int depth = 0;
  bool folder = false;
  bool expanded = false;
  std::string parent_guid;
  // Tab list context.
  std::string group_id;
  bool group_header = false;   // kGroup rows.
  // A member row: the group card's bottom edge (0 = none). A pointer past
  // the last member's midpoint but still above it drops INTO the group as
  // its last member; below it, after the group.
  int group_bottom = 0;
};

struct DropResolution {
  SidebarPlace place;
  // The visible slot the gap opens at, 0..n (n = end), counted over the
  // rows the slide animation moves: outline rows, grid cells, or visible
  // tab rows (headers never count). -1 = no gap: the drop lands INTO a
  // folder (highlight `folder_key` instead).
  int gap_slot = -1;
  SidebarKey folder_key;
  // The strip only (ResolveStripTabDrop): the content row the gap opens
  // BEFORE — a chip or a tab; rows.size() = past the last one. The slide
  // reads this rather than `gap_slot`, since a chip is a slot of its own on
  // the strip and none in the tab count.
  int anchor_row = -1;
};

// The bookmark outline (the Mac's SidebarBookmarkFolderDropResolver). Rows
// are the VISIBLE outline rows in order. A folder's top third: BEFORE it
// among its siblings (so the slot ahead of the first row is reachable).
// The rest of an expanded folder: its first child. The middle third of a
// collapsed folder: INTO it (highlight, no gap); its bottom third: after
// it. Otherwise before/after the row among its own siblings by halves; a
// pointer below every row lands at the root's end. `dragging` (may be empty) is the
// row leaving its slot, never its own anchor.
DropResolution ResolveBookmarkDrop(const std::vector<DropRow>& rows,
                                   int pointer_y,
                                   const SidebarKey& dragging);

// The pinned grid: cells in order; the pointer's row of cells, then left or
// right of a cell's horizontal midpoint. Below every row = the end.
DropResolution ResolvePinnedDrop(const std::vector<DropRow>& cells,
                                 int pointer_x,
                                 int pointer_y,
                                 const SidebarKey& dragging);

// The tab list (vertical) or strip (horizontal): rows include group headers;
// the slot is before the first row whose midpoint is past the pointer. A
// drop inside a group's run (after its header, before its last member's
// bottom half) joins that group; between groups it is ungrouped. Header rows
// themselves anchor on the group's first member. `horizontal` compares x.
DropResolution ResolveTabDrop(const std::vector<DropRow>& rows,
                              int pointer_x,
                              int pointer_y,
                              bool horizontal,
                              const SidebarKey& dragging);

// The comfortable strip (the Mac's TabStripDragController). The gap is
// decided by the dragged tab's EDGES, not by the pointer, so the moment a
// neighbour lets way does not depend on where inside the tab it was
// grabbed. `rows` are the strip's content rows in order — chips as headers,
// tabs — with their RESTING rects (empty when hidden or unplaced), and
// `dragged_row` the dragged tab's index among them; `proxy_left/right` is
// where the lifted tab is on screen right now. Every other placed row is a
// landmark: one left of the dragged tab lets way once the tab's LEFT edge
// reaches its centre, one right of it once the tab's RIGHT edge does, and
// the gap opens before the first landmark still ahead. (This is the Mac's
// calculateGapIndexEdgeBased in resting coordinates: its was-before /
// was-after hysteresis over the slid frames reduces to exactly this when
// the gap is the dragged tab's own width, as it is here.)
//
// Membership at the gap: between two members, or right after an expanded
// chip, joins the group; at a FOREIGN group's trailing edge the tab joins
// while it covers the last member by a third of that member's width
// (the Mac's trailingJoin); at its OWN group's trailing edge it stays a
// member until it has moved a third of its width past its home
// (trailingLeave); before a chip is before the group; right after a
// collapsed chip is after the group.
DropResolution ResolveStripTabDrop(const std::vector<DropRow>& rows,
                                   int dragged_row,
                                   int proxy_left,
                                   int proxy_right);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_DROP_H_

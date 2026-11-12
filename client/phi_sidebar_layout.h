// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_LAYOUT_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_LAYOUT_H_

#include <vector>

// Sidebar geometry as pure functions — docs/sidebar-architecture.md §6.
// Everything here is integer pixel math over already-scaled metrics: no
// HWND, no DPI lookups, no Win32 types, so it compiles and is tested on the
// Linux host (test/model/run.sh). The controller converts to RECT.

namespace phi_client {

struct SidebarRect {
  int left = 0, top = 0, right = 0, bottom = 0;
  bool empty() const { return right <= left || bottom <= top; }
  friend bool operator==(const SidebarRect& a, const SidebarRect& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right &&
           a.bottom == b.bottom;
  }
};

// ---- The pinned grid (the Mac's PinnedTabLayout) ---------------------------
// Cells are 45pt high with 8pt gaps and insets. The width breakpoint
// (decisionWidth = sidebar − 8: under 236 → 3, under 294 → 4, under 352 → 5,
// else 6) is only the column LIMIT — the column count is min(limit, units),
// shrunk while a cell would dip under the 53pt minimum, and cells STRETCH to
// fill the row: a lone pin spans the sidebar, two pins split it in half. A
// split cell spans two units (clamped to the column count when narrow); a
// cell that would overflow its row wraps whole.
struct PinnedGridMetrics {
  // Column solving is in DIPs, as the Mac's breakpoints are.
  int sidebar_dip = 0;
  int edge_inset_dip = 0;
  int gap_dip = 0;
  int min_cell_width_dip = 0;
  // Placement is in pixels.
  int origin_x = 0;   // favorites.left + scaled edge inset.
  int origin_y = 0;   // favorites.top.
  int sidebar_px = 0;
  int edge_inset_px = 0;
  int gap_px = 0;
  int cell_height_px = 0;
};
struct PinnedGridPlacement {
  int columns = 1;
  int rows = 0;  // 0 when there are no cells.
  std::vector<SidebarRect> cells;  // One per input unit entry, same order.
};
int PinnedGridColumnLimit(int sidebar_dip);
int PinnedGridColumns(const PinnedGridMetrics& m, int total_units);
PinnedGridPlacement LayoutPinnedGrid(const std::vector<int>& units,
                                     const PinnedGridMetrics& m);

// ---- A vertical list (bookmark outline, tab list with group headers) -------
struct ListItem {
  enum class Kind { kHeader, kRow };
  Kind kind = Kind::kRow;
  bool hidden = false;  // Takes no room (collapsed group member).
  // Extra room below this item, before the gap: the group card's inner
  // insets (TabGroupCellView.innerTableTop/BottomInset) ride the header and
  // the group's last visible member.
  int pad_below = 0;
};
struct ListMetrics {
  int left = 0, right = 0;
  int top = 0;         // First item's top before scrolling.
  int row_height = 0;
  int row_gap = 0;
  int header_height = 0;  // Header pitch reuses row_gap.
  int scroll = 0;         // Subtracted from every top.
  // Clipping viewport; items entirely outside come back as empty rects.
  // clip=false places without clipping (the horizontal strip has none).
  bool clip = false;
  int viewport_top = 0, viewport_bottom = 0;
};
struct ListPlacement {
  std::vector<SidebarRect> rects;  // One per item; empty when hidden/clipped.
  // The same rects before clipping (hidden ones still empty) — containers
  // that span rows need the placed edge even when it is off the viewport.
  std::vector<SidebarRect> unclipped;
  int content_height = 0;  // Unscrolled height of every placed item.
  int scroll_max = 0;      // max(0, content − viewport) when clipping.
  int bottom = 0;          // y after the last placed item (scrolled).
};
ListPlacement LayoutList(const std::vector<ListItem>& items,
                         const ListMetrics& m);

// ---- The horizontal strip's shared width ------------------------------------
// Chrome's rule: tabs take their ideal width while they fit and shrink toward
// the floor as more open; resting tabs give up width first, the active one
// keeps its wider slot until they are all at the floor.
struct StripWidths {
  int rest = 0;
  int active = 0;  // 0 when there is no active tab.
};
StripWidths SolveStripWidths(int resting_count,
                             bool has_active,
                             int room,
                             int ideal_rest,
                             int ideal_active,
                             int floor);

// ---- The horizontal strip's window (overflow) ------------------------------
// Past the tabs' floor width the strip's items (chips and tabs, in order) no
// longer fit, and the strip becomes a WINDOW onto them, stepped by whole
// items so none is ever cut in half: `first` items are scrolled out ahead of
// it, the ones past `end` hide, and the window gives `chevron_room` back for
// the overflow chevron. Each item takes `lead` + `width` and leaves `trail`
// after it; an item shows only if its right edge stays inside the limit.
struct StripSpan {
  int lead = 0;
  int width = 0;
  int trail = 0;
};
struct StripWindow {
  bool overflow = false;
  int first = 0;      // The first item shown.
  int end = 0;        // One past the last item shown.
  int first_max = 0;  // The latest `first` whose window still reaches the end.
  int limit = 0;      // Where the items stop (the chevron's room held back).
};
// `first` is the current scroll (clamped to the range); `reveal`, when not
// -1, is an item the window must show — the newly active tab — and moves
// `first` the least that does it.
StripWindow SolveStripWindow(const std::vector<StripSpan>& items,
                             int left,
                             int limit,
                             int chevron_room,
                             int first,
                             int reveal);

// ---- The horizontal strip's group chips (the Mac's TabGroupChipView) -------
// A group on the strip is a CHIP standing before its first member: the
// colour dot, the title, then — trailing — the collapse chevron, and before
// it either the member-count badge (expanded and user-titled), the 2x2
// favicon mosaic (collapsed) or nothing. The chip's width is measured up
// front so the tabs share only what the chips leave (TabStripLayoutInput.
// chipFullWidths), and its parts are placed by one rule the paint and the
// hit tests both read.
struct StripChipMetrics {
  int leading = 0;           // leadingPadding
  int dot = 0;               // dotSize
  int dot_to_label = 0;      // dotToLabelGap
  int label_slack = 0;       // labelSafetyMargin (text measures narrow)
  int label_right_pad = 0;   // labelRightPadding
  int count_to_label = 0;    // countToLabelGap; the mosaic takes the same
  int count_h_pad = 0;       // countHorizontalPadding
  int count_v_pad = 0;       // countVerticalPadding
  int mosaic = 0;            // TabGroupChipMosaicView.mosaicSize
  int collapse_gap = 0;      // collapseControlGap
  int collapse_control = 0;  // collapseControlSize
  int max_width = 0;         // maxFullWidth
};
// The chip's full width for a title `label_px` wide and a count badge whose
// digits are `count_px` wide. Collapsed reserves the mosaic whether or not
// the group is titled; expanded reserves the badge only for a user title
// (an auto-name already carries the count). Capped at `max_width`.
int SolveStripChipWidth(int label_px,
                        int count_px,
                        bool collapsed,
                        bool user_titled,
                        const StripChipMetrics& m);
struct StripChipPlacement {
  SidebarRect dot;
  SidebarRect label;
  SidebarRect count;     // Empty unless expanded and user-titled.
  SidebarRect mosaic;    // Empty unless collapsed.
  SidebarRect collapse;  // The chevron's frame.
};
// The parts inside a placed chip rect; `count_px` / `count_height_px` size
// the badge (digits plus padding).
StripChipPlacement PlaceStripChip(const SidebarRect& chip,
                                  int count_px,
                                  int count_height_px,
                                  bool collapsed,
                                  bool user_titled,
                                  const StripChipMetrics& m);
// The 2x2 mosaic's cells (top-left, top-right, bottom-left, bottom-right)
// inside `mosaic`: `cell` squares `gap` apart, `margin` in from the frame.
std::vector<SidebarRect> StripMosaicCells(const SidebarRect& mosaic,
                                          int cell,
                                          int gap,
                                          int margin);
// What each mosaic slot shows for a group of `member_count` tabs
// (TabGroupChipMosaicView.fillCells): the favicon of member i, an empty
// plate, or — at five and more — the "+N" overflow in the last slot.
enum class StripMosaicCell { kEmpty, kFavicon, kOverflow };
struct StripMosaicSlot {
  StripMosaicCell kind = StripMosaicCell::kEmpty;
  int favicon_index = -1;  // kFavicon: which member.
  int overflow = 0;        // kOverflow: how many more than the three shown.
};
std::vector<StripMosaicSlot> StripMosaicFill(int member_count);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_LAYOUT_H_

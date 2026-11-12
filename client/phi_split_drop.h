// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SPLIT_DROP_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SPLIT_DROP_H_

#include <windows.h>

#include <array>
#include <vector>

#include <string>

namespace phi_client {

class PhiShellWindow;

// Dropping a dragged sidebar row onto the PAGE to form (or re-pane) a split —
// the Windows twin of the Mac's SplitTabDropContainer.
//
// The Mac hangs this off an NSView that owns the web-content area and accepts
// the drag through NSDraggingDestination. Here the content area belongs to the
// reparented Chromium child, and the sidebar's drag is a raw mouse drag with
// the shell holding capture — so the shell resolves the zone itself (the
// controller's split-drop members) and this file owns the two things that are
// genuinely about the page area: the DROP GEOMETRY and the HINT CARDS drawn
// over it.
//
// The zones follow the Mac exactly:
//   create mode  (the focused tab is not in a split) — the four edge thirds
//                of the page form a new split; the dragged item lands in the
//                dropped pane, the focused tab takes the other. Left/right
//                make a side-by-side ("vertical") split, top/bottom a stacked
//                ("horizontal") one. The middle band is a no-drop area.
//   pane mode    (the focused tab IS a split) — the page maps onto the split's
//                live panes (two to four). While the split has room, the
//                leading and trailing quarter of each pane insert a new pane
//                there (kInsert); every other point swaps the dragged item
//                into the pane under the cursor (kPane). A split of four
//                only replaces.
// kNewWindow: the page's centre (Chrome's and Edge's rule — a tab dragged
// onto the page detaches) and the fifth snap tile (windows-native-ui.md
// §4.8).
enum class PhiSplitDropZone {
  kNone,
  kLeft,
  kRight,
  kTop,
  kBottom,
  kNewWindow,
  // Pane mode only; PhiSplitDropHints::target says which pane or slot.
  kPane,
  kInsert,
};

// The snap-tile flyout at the page's top centre (§4.8): five 56 × 40 dip
// tiles — Left, Right, Top, Bottom split, then New window — in a radius-8
// flyout, 8 apart, a divider before the fifth. In pane mode (`panes` 2 to
// 4) the first `panes` tiles are the split's panes and the rest of the
// first four are empty; `panes` 0 is create mode. `out[5]` is the flyout's
// own rect. Shell client coordinates, from `area` and `dpi`.
void PhiSnapTileRects(const RECT& area,
                      UINT dpi,
                      int panes,
                      std::array<RECT, 6>* out);

// True when the zone lands in Chromium's PRIMARY slot (leading / top).
bool PhiSplitZoneIsPrimary(PhiSplitDropZone zone);
// The divider axis a create-mode drop on `zone` produces: "vertical" for the
// side cards (side-by-side), "horizontal" for the top/bottom ones (stacked).
const char* PhiSplitZoneLayout(PhiSplitDropZone zone);

// CREATE mode hit test: the four edge thirds of `area`, left/right winning the
// overlapping corners. `retained` is the zone the drag is already on — it
// keeps its grip until the pointer leaves it by more than a small tolerance,
// so tremble in a corner cannot flip the choice back and forth.
// The zone under `pt` while nothing is split: a snap tile first, then the
// edge thirds (with the 12 px exit tolerance for `retained`), then the
// centre = New window. `dpi` places the tiles; 96 lays them out in dip.
PhiSplitDropZone PhiSplitCreateZoneAt(const RECT& area,
                                      POINT pt,
                                      PhiSplitDropZone retained,
                                      UINT dpi = 96);

// What the hint overlay should be showing right now.
struct PhiSplitDropHints {
  RECT area{};        // The page area, in shell client coordinates.
  bool replace = false;   // Pane mode.
  bool stacked = false;  // Pane mode: the split's divider axis.
  // Pane mode: every pane's share in slot order (two to four).
  std::vector<double> fractions = {0.5, 0.5};
  // Pane mode: where Chromium actually laid the panes out (the framework's
  // TabContentBoundsById, mapped into the shell), so the cards trace the
  // real panes rather than an estimate from the shares — the Mac's
  // replacePaneRects mirrors its own SplitPaneHostView the same way. `panes`
  // is false until the layout has run; the estimate stands in.
  bool panes = false;
  std::vector<RECT> pane_rects;
  // Pane mode: the split has fewer than four panes, so a drop may insert.
  bool can_add = false;
  PhiSplitDropZone active = PhiSplitDropZone::kNone;
  // kPane: the pane the drop replaces. kInsert: the slot the new pane takes
  // (0 = before the first pane, the pane count = after the last).
  int target = -1;
};

// PANE mode hit test: a pane tile or the New window tile first, then the
// panes themselves. Answers kPane (replace `*target`), kInsert (a new pane
// at slot `*target`, only while `hints.can_add`) or kNewWindow (its tile,
// or a divider gap of a full split). `*target` is -1 otherwise.
PhiSplitDropZone PhiSplitPaneZoneAt(const PhiSplitDropHints& hints,
                                    POINT pt,
                                    UINT dpi,
                                    int* target);

// The live panes of the split `hints` describes, in slot order: the real
// rects when it has them, else an estimate from the shares over
// `hints.area` with the framework's flush panes and 1 DIP dividers.
std::vector<RECT> PhiSplitPaneRects(const PhiSplitDropHints& hints);

// The strip a kInsert drop at `slot` lights up: the leading quarter of the
// first pane, the trailing quarter of the last, or the two quarters either
// side of the divider between two panes.
RECT PhiSplitInsertBand(const PhiSplitDropHints& hints, int slot);

// The hint cards themselves: one layered, click-through child window above the
// browser child, composed with GDI+ (per-pixel alpha, which is the only way to
// get the Mac card's translucency over live page content).
class PhiSplitDropOverlay {
 public:
  // Mounts or updates the cards over `controller`'s page area. Cheap to call
  // on every pointer move — a repaint only happens when something changed.
  static void Show(PhiShellWindow* controller,
                   const PhiSplitDropHints& hints);
  // Takes the cards down (the drag left the page, was dropped, or cancelled).
  static void Hide();
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SPLIT_DROP_H_

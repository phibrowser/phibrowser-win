// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_DRAG_IMAGE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_DRAG_IMAGE_H_

#include <windows.h>

#include <string>
#include <vector>

namespace phi_client {

struct PhiPalette;

// A premultiplied BGRA bitmap, top-down — the form every pixel source in the
// client already speaks (favicons, thumbnails, DIB sections).
struct PhiDragBitmap {
  std::vector<unsigned char> bgra;
  int width = 0;
  int height = 0;
  bool empty() const { return bgra.empty() || width <= 0 || height <= 0; }
};

// One pane of the page card: the page's snapshot when one exists, else the
// favicon + title placeholder the Mac draws (makeTabPlaceholderSnapshot).
struct PhiDragPane {
  PhiDragBitmap snapshot;
  PhiDragBitmap favicon;
  std::wstring title;
};

// The page card's model — the Mac's resolveOutsideWindowDragImage: a single
// page, or a split's two panes composed on one canvas around a divider.
struct PhiDragCard {
  bool split = false;
  bool stacked = false;  // A split's divider axis.
  double ratio = 0.5;    // The primary pane's share of a split.
  PhiDragPane a;
  PhiDragPane b;
  // What is on screen right now, when the dragged row IS the page being
  // shown (the Mac's live capture for the active tab). A split shows both
  // panes at once, so this already is the composite and wins over `a`/`b`.
  PhiDragBitmap whole;
};

// The drag image: the Windows twin of the Mac's NSDraggingSession image with
// TabDraggingSession's image switching layered on it.
//
// The shell's own DC cannot carry a dragged row past the sidebar — the
// reparented Chromium child covers the page and nothing paints beyond the
// window's edge — so the image lives in a topmost, click-through layered
// window of its own and follows the cursor across the whole desktop. Inside
// the sidebar it is the ROW's snapshot (createDraggingSnapshot: the pill,
// rounded); the moment the cursor leaves the sidebar it becomes the PAGE
// CARD (tabSnapshotSize 220x160, radius 12 — the page's snapshot aspect-
// filled, or favicon + title when none is cached), the way the Mac swaps its
// drag image on crossing the sidebar's boundary. A drop that changed nothing
// while still inside the sidebar slides the image home (AppKit's
// animatesToStartingPositionsOnCancelOrFail); everywhere else it simply goes.
//
// One drag runs at a time (it owns the mouse capture), so this is a single
// instance, like the split-drop cards.
class PhiDragImage {
 public:
  // The Mac's TabDraggingSession metrics, in DIPs.
  static constexpr int kCardWidth = 220;
  static constexpr int kCardHeight = 160;
  static constexpr int kCardRadius = 12;
  // A split's canvas: wide for side-by-side panes, tall for stacked ones.
  static constexpr int kSplitLongEdge = 280;
  static constexpr int kSplitShortEdge = 160;

  // Starts the image with the row's snapshot (`row`, straight out of the
  // shell's chrome, alpha ignored), rounded at `radius`, under the cursor
  // with `grip` — where inside the row the cursor grabbed it — so the copy
  // lifts off exactly in place. Screen coordinates throughout.
  static void Begin(HWND owner,
                    UINT dpi,
                    const PhiDragBitmap& row,
                    int radius,
                    POINT grip,
                    POINT cursor);
  // Back inside the sidebar: the row's snapshot again.
  static void ShowRow(POINT cursor);
  // The element as what it is about to BECOME over another band — a row
  // over the pinned grid shows as a cell, a cell over the outline or the
  // tab list as a row (`shape`, painted by the caller at the target's own
  // size, alpha ignored), rounded at `radius` and centred under the cursor,
  // where the Mac's temporary drag image sits (the drag frame's midpoint).
  // Composed on every call; the caller only calls on a mode or size change.
  static void ShowMorph(const PhiDragBitmap& shape, int radius, POINT cursor);
  // Outside it: the page card, composed once per drag and cached.
  static void ShowCard(const PhiDragCard& card,
                       const PhiPalette& palette,
                       POINT cursor);
  // Follows the cursor. Cheap: the composed surface is retained.
  static void MoveTo(POINT cursor);
  // Puts the image's top-left at `origin` (screen) instead of under the
  // cursor — the comfortable strip rides the dragged tab along its row,
  // clamped to the strip, the way the Mac's dragPresentationFrame does.
  static void PlaceAt(POINT origin);
  // Takes the window off the screen without ending the drag — for a screen
  // capture of what is under it, or a hit test that must not find it. The
  // next MoveTo / ShowRow / ShowCard brings it back.
  static void Conceal();
  // The drag ended and the image just goes.
  static void Hide();
  // The drag ended without effect inside the sidebar: the image glides back
  // to `home` (the row's screen rect at lift-off) and then goes.
  static void SlideBack(const RECT& home);
  // A shell going away takes its image with it.
  static void OwnerGone(HWND owner);
  static bool active();
};

// True when every sampled pixel of `bitmap` is the same colour — a capture of
// a page that has not painted yet (the Mac's isImageBlank), which the card
// must not show as "the page".
bool PhiDragBitmapIsUniform(const PhiDragBitmap& bitmap);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_DRAG_IMAGE_H_

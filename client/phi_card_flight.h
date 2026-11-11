// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_CARD_FLIGHT_H_
#define PHIBROWSER_WIN_CLIENT_PHI_CARD_FLIGHT_H_

#include <windows.h>

#include <functional>

// The flight a card arrives and leaves by (the Mac's Peek "appear flight",
// PeekPanelController.armAppearFlight / prepareToClose): it grows out of the
// point the user pressed, link-sized, into its place while it fades in, and
// shrinks back to that point when it goes. 200 ms, ease-out in and ease-in
// out, as there.
//
// A hosted page cannot be scaled, so the flight is a stand-in: a layered
// window above the owner that draws the card (its ground, stroke and drop
// shadow, or a snapshot of what was on screen) at each frame's size. The
// real card is shown when the flight lands.

namespace phi_client {

class PhiCardFlight {
 public:
  struct Look {
    int radius_px = 0;
    COLORREF fill = RGB(255, 255, 255);
    COLORREF stroke = RGB(0, 0, 0);
    int shadow_margin_px = 0;    // Room round the card for the shadow.
    int shadow_blur_px = 1;
    int shadow_offset_y_px = 0;
    double shadow_alpha = 0.0;   // 0 = no shadow.
    // FlyOut only: a dim over `scrim` (screen) that fades away with the
    // card, for a card that had one behind it. `scrim_radius_px` rounds the
    // dim's own corners. 0 alpha = none.
    RECT scrim = {};
    double scrim_alpha = 0.0;
    int scrim_radius_px = 0;
    // How long the flight takes, and whether its window is topmost (for a
    // card that is itself topmost, such as a menu).
    int duration_ms = 200;
    bool topmost = false;
  };

  // Where a flight starts from (or ends at): `origin` when the user pressed
  // somewhere, a card `start_width_px` wide there; else the card itself at
  // 97 % about its centre.
  struct Anchor {
    bool has_origin = false;
    POINT origin = {};           // Screen.
    int start_width_px = 0;
  };

  // Grows the card into `target` (screen). `landed` runs when it is there —
  // the moment to show the real card — and never after Cancel. Returns the
  // flight's id, or 0 when nothing was started (show the card at once).
  // `progress`, when given, hears each frame's eased 0..1 (what else fades
  // in with the card).
  static int FlyIn(HWND owner,
                   const RECT& target,
                   const Anchor& anchor,
                   const Look& look,
                   std::function<void()> landed,
                   std::function<void(double)> progress = nullptr);

  // FlyIn with the card's real picture: `snapshot` (a bitmap of `target`'s
  // size, taken over by the flight) grows instead of a plain card.
  static int FlyInSnapshot(HWND owner,
                           const RECT& target,
                           const Anchor& anchor,
                           const Look& look,
                           HBITMAP snapshot,
                           std::function<void()> landed);

  // Takes a snapshot of the screen at `from` NOW and shrinks it away. Hide
  // the real card right after calling. Runs to its end on its own;
  // `progress` hears each frame's eased 0..1 until then (what fades out with
  // the card). Returns the flight's id, or 0.
  static int FlyOut(HWND owner,
                    const RECT& from,
                    const Anchor& anchor,
                    const Look& look,
                    std::function<void(double)> progress = nullptr);

  // Ends flight `id` where it is; its `landed` is dropped. 0 is ignored.
  static void Cancel(int id);
  // Lets flight `id` finish but calls nothing of the caller's any more: for
  // a caller that goes away mid-flight.
  static void Detach(int id);
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CARD_FLIGHT_H_

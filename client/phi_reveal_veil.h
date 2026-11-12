// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_REVEAL_VEIL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_REVEAL_VEIL_H_

#include <windows.h>

#include "client/phi_window.h"

namespace phi_client {

// The Space switch's reveal swaps what sits in the content card: the
// entering browser child moves in, the leaving child and native pages come
// down. A browser child's pixels are a DirectComposition layer clipped to
// the part of the window nothing covers, and Chromium re-presents that clip
// only with its next commit — so the area a native (GDI) page uncovers is
// EMPTY for a frame: the transparent shell, black on an opaque backdrop.
// No sibling inside the shell can bridge that frame (a sibling over the
// child is exactly what clips it), so the veil is a separate top-level
// window: an owned, click-through, layered popup directly above the shell,
// holding the content card as the screen showed it the moment before the
// swap, lifted once the entering child has had time to present.
class PhiRevealVeil : public PhiWindow {
 public:
  PhiRevealVeil();
  ~PhiRevealVeil() override;

  // Covers `screen_rect` with what the screen shows there now, directly
  // above `owner`, for kHoldMs. A second cover while up re-reads the
  // screen (the veil included) and restarts the hold.
  void Cover(HWND owner, const RECT& screen_rect);
  void Lift();

  // A few desktop frames: the entering child's commit, with margin.
  static constexpr UINT kHoldMs = 48;

 protected:
  // The pixels come from UpdateLayeredWindow; this window never paints.
  void Paint(HDC, const RECT&) override {}
  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) override;
  void OnTimerFired(UINT_PTR id) override;

 private:
  HWND owner_ = nullptr;
  PhiWindow::Timer hold_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_REVEAL_VEIL_H_

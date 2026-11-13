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

  // Where the veil's picture is read from.
  enum class Source {
    // The desktop composition (CAPTUREBLT): what the user sees, browser
    // children's DirectComposition layers and layered children included.
    // ~80 ms for a 5 MP card at 300 % on a slow box.
    kScreen,
    // The owner's own GDI surface: ~20 ms, and the same pixels where
    // nothing but GDI shows — a native page covering the whole card with
    // no layered child over it. Anything else reads as what is under it.
    kOwnerSurface,
  };
  // Covers `screen_rect` with what the screen shows there now, directly
  // above `owner`, for kHoldMs. A second cover while up re-reads the
  // screen (the veil included) and restarts the hold.
  void Cover(HWND owner, const RECT& screen_rect,
             Source source = Source::kScreen);
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
  // The hold also runs on a thread-pool timer that POSTS the lift: a
  // WM_TIMER is only delivered once no paint is pending, and the Space
  // switch's theme ramp repaints the shell back to back for 600 ms, so the
  // veil (and the page under it) stayed up ~150 ms instead of 48
  // (GitHub phibrowser/phibrowser-win#13). hold_ stays as the fallback.
  void ArmLift();
  void DisarmLift();
  static void CALLBACK PostLift(PVOID hwnd, BOOLEAN);
  static UINT LiftMessage();

  HWND owner_ = nullptr;
  PhiWindow::Timer hold_;
  HANDLE lift_timer_ = nullptr;
  ULONGLONG covered_at_ = 0;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_REVEAL_VEIL_H_

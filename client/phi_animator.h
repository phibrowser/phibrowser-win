// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_ANIMATOR_H_
#define PHIBROWSER_WIN_CLIENT_PHI_ANIMATOR_H_

#include <windows.h>

#include <cstdint>
#include <functional>
#include <vector>

#include "client/phi_window.h"

namespace phi_client {

// ONE animation clock for the UI thread (docs/architecture-review.md §3.6,
// lane 3 §6.3). Seven hand-rolled easing loops exist today, each with its
// own SetTimer, its own tick period and its own whole-window
// InvalidateRect: the shell's add-space fade, drag slide and space-swap
// ramp, the tab preview's slide, the toast's fade, the drag image's
// slide-back and the agent sprite's glide.
//
// The animator holds a SINGLE coalescable 16 ms timer on its own
// message-only window while any animation is live, and nothing at all
// when none is. Each tick advances every running animation, calls its
// on_tick with the EASED position, and invalidates only the dirty RECT the
// callback reported — never the window.
//
// CONTRACT
//   * on_tick(t, dirty) runs with t already eased, 0 at the first tick and
//     exactly 1.0 on the last one, so a callback never has to special-case
//     the end. Fill *dirty in `target`'s CLIENT coordinates; leave it
//     empty to invalidate nothing (right for a tick that only moved the
//     window with SetWindowPos, which repaints itself).
//   * on_done runs after the final tick, once, and is removed from the
//     list BEFORE it is called — so it may legally destroy the window, the
//     animator's caller, or itself (the toast does exactly that).
//   * Every animation is bound to a `target` HWND. PhiWindow cancels its
//     own at WM_NCDESTROY, so no callback can outlive its window.
//   * Handles are never reused within a run; Cancel() on a finished handle
//     is a no-op, so a caller may hold a stale one.
class PhiAnimator {
 public:
  // `t` is the eased position, 0..1. Fill `dirty` with the region to
  // repaint in the target's client coordinates, or leave it zeroed.
  using TickFn = std::function<void(double t, RECT* dirty)>;
  using DoneFn = std::function<void()>;

  static PhiAnimator& ForThread();

  PhiAnimator(const PhiAnimator&) = delete;
  PhiAnimator& operator=(const PhiAnimator&) = delete;

  // Starts an animation over `duration_ms`. Returns a handle for Cancel(),
  // or 0 when it could not start (no target, no duration) — in which case
  // on_tick(1.0) and on_done are still run synchronously, so a caller
  // never has to duplicate the end state.
  int Animate(HWND target,
              int duration_ms,
              PhiEasing easing,
              TickFn on_tick,
              DoneFn on_done = DoneFn());

  // Drops the animation without running on_done.
  void Cancel(int handle);
  // Drops every animation on `target` without running their on_done.
  void CancelForWindow(HWND target);
  bool IsRunning(int handle) const;
  size_t live() const { return animations_.size(); }

  // The tick period. 16 ms, coalescable, so the OS may batch it with the
  // caret blink and other UI timers instead of waking the thread twice.
  static constexpr UINT kTickMs = 16;

 private:
  PhiAnimator();
  // Private and never called (defined out of line, as chromium-style
  // requires): ForThread() leaks the one instance so no exit-time
  // destructor runs and a late tick cannot reach a dead object.
  ~PhiAnimator();

  struct Animation {
    int handle = 0;
    HWND target = nullptr;
    int duration_ms = 0;
    int elapsed_ms = 0;
    PhiEasing easing = PhiEasing::kLinear;
    TickFn on_tick;
    DoneFn on_done;
  };

  static LRESULT CALLBACK ClockProc(HWND hwnd,
                                    UINT message,
                                    WPARAM wparam,
                                    LPARAM lparam);
  void EnsureWindow();
  void SyncTimer();
  void Tick();

  HWND hwnd_ = nullptr;
  bool timer_running_ = false;
  bool ticking_ = false;
  int next_handle_ = 1;
  std::vector<Animation> animations_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_ANIMATOR_H_

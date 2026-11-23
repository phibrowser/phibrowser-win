// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SPACE_SWIPE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SPACE_SWIPE_H_

#include <cstdint>

namespace phi_client {

// The two-finger sideways swipe that switches Space over the sidebar (and
// over comfortable's tab strip when the strip has nothing to scroll):
// the Mac's SpaceSwipeTracker, fed with wheel messages (PHI-2180).
//
// A Precision Touchpad reaches a plain window as WM_MOUSEWHEEL /
// WM_MOUSEHWHEEL with small deltas, and Windows keeps sending them for the
// inertia after the fingers lift. Those messages carry no gesture phases,
// so a gesture here is a run of wheel messages with no gap longer than
// kGestureGapMs. As on the Mac:
// - the run's first message latches its axis: a run that starts vertical
//   scrolls as before for its whole length, one that starts horizontal is
//   the swipe's, vertical drift included;
// - the swipe completes at 35 % of the band width, or as a flick (12 dip
//   or more at 650 dip/s or more);
// - rightward deltas (fingers moving left) step to the next Space.
// Unlike the Mac the band does not follow the fingers: the Windows band
// slide switches the Space when it starts, so the swipe commits the
// moment it qualifies, the slide plays, and the rest of the run (the
// inertia too) is swallowed. One run switches at most once.
class PhiSpaceSwipe {
 public:
  enum class Route {
    kPass,      // Not a swipe: the message's usual handling.
    kConsumed,  // The swipe's (or `other` took it): nothing else to do.
    kNext,      // The swipe completed toward the next Space.
    kPrevious,  // ... toward the previous Space.
  };

  // Messages further apart than this start a new gesture. A precision
  // touchpad sends one every 8-16 ms while the fingers or the inertia move.
  static constexpr int64_t kGestureGapMs = 150;
  // How far the page scrolls for one WHEEL_DELTA (Chromium's 100 dip per
  // notch), so the thresholds read in the Mac's points.
  static constexpr double kDipPerWheelUnit = 100.0 / 120.0;

  // The Mac's SpaceSwipeTracker.shouldComplete, minus the release-time
  // reversal check (a run has no release; its sum carries a reversal).
  static bool ShouldComplete(double distance_dip,
                             double velocity_dip_s,
                             double width_dip);

  // One wheel message over a surface that switches Spaces. `delta` is the
  // message's wheel delta (WM_MOUSEHWHEEL positive = rightward);
  // `width_dip` the width the 35 % rule measures. `other_takes`, when set,
  // is asked once, on a run's first horizontal message: true when another
  // handler took that message and wants the run (comfortable's strip
  // scrolling its tabs) — the rest of the run then passes.
  Route Feed(bool horizontal,
             int delta,
             int64_t now_ms,
             double width_dip,
             bool (*other_takes)(void*) = nullptr,
             void* other_context = nullptr);

  // Forget the run (the window lost the pointer, the layout changed).
  void Reset();

  // True while the current run is the swipe's (its messages are consumed).
  bool owns_run() const { return axis_ == Axis::kHorizontal; }

 private:
  enum class Axis { kNone, kHorizontal, kVertical, kOther };
  Axis axis_ = Axis::kNone;
  bool has_last_ = false;
  int64_t last_ms_ = 0;
  double distance_dip_ = 0;
  double velocity_dip_s_ = 0;
  bool done_ = false;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SPACE_SWIPE_H_

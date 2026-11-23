// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_space_swipe.h"

#include <algorithm>
#include <cmath>

namespace phi_client {

namespace {

// The Mac's SpaceSwipeTracker constants.
constexpr double kCompleteFraction = 0.35;
constexpr double kFlickMinDip = 12;
constexpr double kFlickMinVelocity = 650;   // dip per second
constexpr double kMinIntervalS = 1.0 / 240;

}  // namespace

// static
bool PhiSpaceSwipe::ShouldComplete(double distance_dip,
                                   double velocity_dip_s,
                                   double width_dip) {
  if (width_dip <= 0) {
    return false;
  }
  return std::abs(distance_dip) / width_dip >= kCompleteFraction ||
         (std::abs(distance_dip) >= kFlickMinDip &&
          distance_dip * velocity_dip_s > 0 &&
          std::abs(velocity_dip_s) >= kFlickMinVelocity);
}

void PhiSpaceSwipe::Reset() {
  axis_ = Axis::kNone;
  has_last_ = false;
  last_ms_ = 0;
  distance_dip_ = 0;
  velocity_dip_s_ = 0;
  done_ = false;
}

PhiSpaceSwipe::Route PhiSpaceSwipe::Feed(bool horizontal,
                                         int delta,
                                         int64_t now_ms,
                                         double width_dip,
                                         bool (*other_takes)(void*),
                                         void* other_context) {
  const bool fresh = !has_last_ || now_ms - last_ms_ > kGestureGapMs ||
                     now_ms < last_ms_;
  const int64_t previous_ms = last_ms_;
  if (fresh) {
    Reset();
  }
  has_last_ = true;
  last_ms_ = now_ms;
  if (delta == 0) {
    return axis_ == Axis::kHorizontal ? Route::kConsumed : Route::kPass;
  }
  if (axis_ == Axis::kNone) {
    // The run's first directional message decides, as the Mac's latch.
    if (!horizontal) {
      axis_ = Axis::kVertical;
    } else if (other_takes && other_takes(other_context)) {
      axis_ = Axis::kOther;
      return Route::kConsumed;  // `other_takes` handled this message.
    } else {
      axis_ = Axis::kHorizontal;
    }
  }
  if (axis_ != Axis::kHorizontal) {
    return Route::kPass;
  }
  if (!horizontal || done_) {
    return Route::kConsumed;  // Drift, or the run after its switch.
  }
  const double step_dip = delta * kDipPerWheelUnit;
  distance_dip_ += step_dip;
  if (!fresh) {
    const double interval_s = std::max(
        kMinIntervalS, static_cast<double>(now_ms - previous_ms) / 1000.0);
    velocity_dip_s_ = step_dip / interval_s;
  }
  if (!ShouldComplete(distance_dip_, velocity_dip_s_, width_dip)) {
    return Route::kConsumed;
  }
  done_ = true;
  return distance_dip_ > 0 ? Route::kNext : Route::kPrevious;
}

}  // namespace phi_client

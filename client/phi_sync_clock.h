// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_CLOCK_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_CLOCK_H_

#include <algorithm>
#include <cstdint>
#include <limits>

// The sync hybrid logical clock — phibrowser-mac
// Sources/Sync/Phi/PhiHybridClock.swift, ported line for line (docs/sync.md
// "Stamps and the hybrid logical clock"). Every last-writer-wins stamp on the
// wire comes from here, so Windows and the Mac must agree on every edge case,
// including the saturation at INT64_MAX that keeps a peer's legal maximum
// stamp from trapping. Pinned by the Mac-generated conformance vectors
// (test/sync-vectors, families clock_sequence and clock_functions).
//
// PURE: STL only, host-tested (test/model/sync_vectors_test.cc).

namespace phi_client::sync {

class HybridClock {
 public:
  // AM-2: below this measured skew the device's own clock is trusted as is.
  static constexpr int64_t kWallClockCorrectionThresholdMs = 5 * 60 * 1000;

  explicit HybridClock(int64_t max_seen = 0) : max_seen_(max_seen) {}

  // The largest stamp this device has issued or landed.
  int64_t max_seen() const { return max_seen_; }

  // stamp() = max(wall, maxSeen + 1); also advances maxSeen.
  int64_t Stamp(int64_t wall_ms) {
    const int64_t issued = std::max(wall_ms, Bumped(max_seen_));
    max_seen_ = std::max(max_seen_, issued);
    return issued;
  }

  // Folds a landed stamp into maxSeen. Never clamps (docs/sync.md "What is
  // observed, and the no-clamp rule").
  void Observe(int64_t stamp_ms) { max_seen_ = std::max(max_seen_, stamp_ms); }

  // An edit made at `edit_wall_ms` over a value stamped `overwritten_stamp_ms`
  // (AM-1): never at or below the value it replaces.
  static int64_t EditStamp(int64_t edit_wall_ms, int64_t overwritten_stamp_ms) {
    return std::max(edit_wall_ms, Bumped(overwritten_stamp_ms));
  }

  int64_t EditStamp(int64_t edit_wall_ms) const {
    return EditStamp(edit_wall_ms, max_seen_);
  }

  // AM-2: the correction to apply to this device's wall clock, from a server
  // Date header. 0 inside the threshold; the whole offset beyond it. An
  // overflowing difference saturates toward the server's sign.
  static int64_t WallClockCorrection(int64_t server_ms, int64_t local_ms) {
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
    if (local_ms < 0 ? server_ms > kMax + local_ms
                     : server_ms < kMin + local_ms) {
      return server_ms > 0 ? kMax : kMin;
    }
    const int64_t offset = server_ms - local_ms;
    if (offset > kWallClockCorrectionThresholdMs ||
        offset < -kWallClockCorrectionThresholdMs) {
      return offset;
    }
    return 0;
  }

  // A wall-clock instant with the correction applied, saturating.
  static int64_t Corrected(int64_t wall_ms, int64_t offset_ms) {
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
    if (offset_ms > 0 && wall_ms > kMax - offset_ms) {
      return kMax;
    }
    if (offset_ms < 0 && wall_ms < kMin - offset_ms) {
      return kMin;
    }
    return wall_ms + offset_ms;
  }

 private:
  // + 1 saturates: INT64_MAX is a legal stamp on the wire.
  static int64_t Bumped(int64_t stamp) {
    return stamp == std::numeric_limits<int64_t>::max() ? stamp : stamp + 1;
  }

  int64_t max_seen_;
};

}  // namespace phi_client::sync

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_CLOCK_H_

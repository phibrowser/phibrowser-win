// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_download_indicator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "base/compiler_specific.h"
#include "client/phi_dwrite.h"
#include "client/phi_mac_icons.h"
#include "client/phi_popup_chrome.h"

namespace phi_client {

namespace {

// The "download-button" Lottie (phibrowser-mac Resources/LottieFiles), in
// its own 48-unit frame, 30 fps, frames 113..143:
//   * the arrow's parent drops 28.75 units over frames 113..140,
//     cubic-bezier(0.4, 0, 0.19, 1), carrying a second arrow 28.75 units
//     above the first, so one arrow leaves through the tray while the next
//     arrives where it rests;
//   * both arrows show only inside a matte from y 9.06 to 38.94;
//   * the tray grows to 123 % over frames 119..127 and back by 140,
//     cubic-bezier(0.001, 0, 0.25, 1) both ways, about (24, 38.2).
constexpr double kFirstFrame = 113;
constexpr double kFrames = 30;
constexpr double kDrop = 28.75;
constexpr double kMatteTop = 9.06;
constexpr double kMatteBottom = 38.94;
constexpr double kTrayPivotX = 24.0;
constexpr double kTrayPivotY = 38.2;
// In kDownloadAlpha the arrow is rows 9..30 and the tray rows 31..38.
constexpr int kTrayFirstRow = 31;

// y of a CSS-style cubic-bezier(x1, y1, x2, y2) at x, by bisection (the
// curves here are monotonic in x).
double CubicBezier(double x1, double y1, double x2, double y2, double x) {
  x = std::clamp(x, 0.0, 1.0);
  const auto at = [](double p1, double p2, double s) {
    const double u = 1.0 - s;
    return 3 * u * u * s * p1 + 3 * u * s * s * p2 + s * s * s;
  };
  double lo = 0.0;
  double hi = 1.0;
  double s = x;
  for (int i = 0; i < 40; ++i) {
    s = (lo + hi) / 2;
    if (at(x1, x2, s) < x) {
      lo = s;
    } else {
      hi = s;
    }
  }
  return at(y1, y2, s);
}

// The 48 px mask, split into its arrow and tray, sampled bilinearly at a
// point in mask units (pixel centres at +0.5).
double Sample(const PhiMacIcon& icon, bool tray, double x, double y) {
  const double fx = x - 0.5;
  const double fy = y - 0.5;
  const int x0 = static_cast<int>(std::floor(fx));
  const int y0 = static_cast<int>(std::floor(fy));
  const double ax = fx - x0;
  const double ay = fy - y0;
  const auto px = [&](int cx, int cy) -> double {
    if (cx < 0 || cy < 0 || cx >= icon.width || cy >= icon.height) {
      return 0.0;
    }
    if ((cy >= kTrayFirstRow) != tray) {
      return 0.0;
    }
    // SAFETY: cx, cy are inside the width x height mask (checked above).
    return UNSAFE_BUFFERS(icon.alpha[cy * icon.width + cx]);
  };
  return (px(x0, y0) * (1 - ax) + px(x0 + 1, y0) * ax) * (1 - ay) +
         (px(x0, y0 + 1) * (1 - ax) + px(x0 + 1, y0 + 1) * ax) * ay;
}

// The cue's 48 px mask at position t (0..1).
std::vector<unsigned char> CueMask(double t) {
  const PhiMacIcon& icon = PhiDownloadIcon();
  const double frame = kFirstFrame + kFrames * std::clamp(t, 0.0, 1.0);
  const double drop =
      kDrop * CubicBezier(0.4, 0, 0.19, 1, (frame - 113) / (140 - 113));
  double scale = 1.0;
  if (frame >= 119 && frame < 127) {
    scale = 1.0 + 0.23 * CubicBezier(0.001, 0, 0.25, 1, (frame - 119) / 8);
  } else if (frame >= 127 && frame < 140) {
    scale = 1.23 - 0.23 * CubicBezier(0.001, 0, 0.25, 1, (frame - 127) / 13);
  }
  std::vector<unsigned char> out(
      static_cast<size_t>(icon.width) * icon.height, 0);
  for (int y = 0; y < icon.height; ++y) {
    const double cy = y + 0.5;
    const bool in_matte = cy >= kMatteTop && cy <= kMatteBottom;
    for (int x = 0; x < icon.width; ++x) {
      const double cx = x + 0.5;
      double a = 0.0;
      if (in_matte) {
        a = std::max(Sample(icon, false, cx, cy - drop),
                     Sample(icon, false, cx, cy - drop + kDrop));
      }
      a = std::max(a, Sample(icon, true,
                             kTrayPivotX + (cx - kTrayPivotX) / scale,
                             kTrayPivotY + (cy - kTrayPivotY) / scale));
      out[static_cast<size_t>(y) * icon.width + x] =
          static_cast<unsigned char>(std::clamp(a + 0.5, 0.0, 255.0));
    }
  }
  return out;
}

}  // namespace

double PhiDownloadsTotalProgress(const std::vector<PhiDownloadRow>& rows) {
  // The framework's rows carry no percentage, so a running download of
  // unknown size counts the Mac's way for one with no percent either: not
  // at all.
  double received = 0;
  double total = 0;
  for (const PhiDownloadRow& row : rows) {
    if (row.state != "in_progress" || row.total <= 0) {
      continue;
    }
    received += std::min(row.received, row.total);
    total += row.total;
  }
  return total > 0 ? std::clamp(received / total, 0.0, 1.0) : 0.0;
}

PhiDownloadIndicator::Change PhiDownloadIndicator::Update(
    int64_t source,
    const std::vector<PhiDownloadRow>& rows) {
  Change change;
  int max_id = -1;
  bool active = false;
  for (const PhiDownloadRow& row : rows) {
    max_id = std::max(max_id, row.id);
    active = active || row.state == "in_progress";
  }
  if (!seeded_ || source != source_) {
    // Whatever is already in the list was started before we looked.
    seeded_ = true;
    source_ = source;
    max_id_ = max_id;
  } else if (max_id > max_id_) {
    max_id_ = max_id;
    change.started = true;
    ++cues_;
  }
  const double progress = active ? PhiDownloadsTotalProgress(rows) : 0.0;
  if (active != active_) {
    active_ = active;
    change.active_changed = true;
  }
  // The bar is 14 dip: a step under half a percent moves no pixel.
  if (std::fabs(progress - progress_) >= 0.005 || change.active_changed) {
    progress_ = progress;
    change.repaint = true;
  }
  change.repaint = change.repaint || change.started;
  return change;
}

void PhiDownloadIndicator::Paint(HDC dc,
                                 const RECT& art,
                                 COLORREF ink,
                                 COLORREF accent,
                                 COLORREF ground,
                                 int num,
                                 int den) const {
  const int size = art.right - art.left;
  if (size <= 0) {
    return;
  }
  if (cue_t_ >= 0 && cue_t_ < 1) {
    const std::vector<unsigned char> mask = CueMask(cue_t_);
    const PhiMacIcon frame = {PhiDownloadIcon().width,
                              PhiDownloadIcon().height, mask.data()};
    const std::vector<unsigned char> bgra =
        PhiTintedIconBgra(frame, ink, size);
    PhiBlendBgra(dc, art, bgra.data(), size, size);
  } else {
    const std::vector<unsigned char> bgra =
        PhiTintedIconBgra(PhiDownloadIcon(), ink, size);
    PhiBlendBgra(dc, art, bgra.data(), size, size);
  }
  if (bar_opacity_ <= 0) {
    return;
  }
  // DownloadButtonView: a 14 x 2 bar, bottom-aligned in the 24 frame and
  // lifted 1, phiPrimary over phiPrimary at 20 %.
  const auto dip = [num, den](int v) {
    return std::max(1, (v * num + den / 2) / den);
  };
  const int width = dip(14);
  const int height = dip(2);
  const int left = (art.left + art.right - width) / 2;
  const int bottom = art.bottom - dip(1);
  const RECT track = {left, bottom - height, left + width, bottom};
  const int alpha = static_cast<int>(std::lround(255 * bar_opacity_));
  PhiFillCapsuleAA(dc, track,
                   PhiPopupBlend(accent, ground, alpha * 20 / 100));
  const int fill_w =
      static_cast<int>(std::lround(width * std::clamp(progress_, 0.0, 1.0)));
  if (fill_w > 0) {
    const RECT fill = {left, track.top, left + std::max(fill_w, height),
                       track.bottom};
    PhiFillCapsuleAA(dc, fill, PhiPopupBlend(accent, ground, alpha));
  }
}

std::string PhiDownloadIndicator::StateJson() const {
  char buffer[192];
  std::snprintf(buffer, sizeof(buffer),
                "{\"active\":%s,\"progress\":%.3f,\"cues\":%d,"
                "\"cueRunning\":%s,\"barOpacity\":%.2f}",
                active_ ? "true" : "false", progress_, cues_,
                cue_t_ >= 0 && cue_t_ < 1 ? "true" : "false", bar_opacity_);
  return buffer;
}

}  // namespace phi_client

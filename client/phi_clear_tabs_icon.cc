// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_clear_tabs_icon.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "client/phi_dwrite.h"
#include "client/phi_mac_icons.h"

namespace phi_client {

namespace {

constexpr double kFrame = 60.0;   // The Lottie's w and h.
constexpr double kFrames = 30.0;  // op: 30 frames at 60 fps.
constexpr double kStroke = 3.0;   // Every layer's stroke, round caps/joins.

struct Pt {
  double x;
  double y;
};

// A Lottie path: vertices with in / out tangents relative to them.
struct LottiePath {
  std::vector<Pt> v;
  std::vector<Pt> in;
  std::vector<Pt> out;
};

// clear-tabs.json's shapes, transcribed.
const LottiePath& LidHandle() {
  static const LottiePath path = {
      {{25.3125, 17.8125}, {25.3125, 15.375}, {27.1875, 13.5},
       {32.8125, 13.5}, {34.6875, 15.375}, {34.6875, 17.8125}},
      {{0, 0}, {0, 0}, {-1.0356, 0}, {0, 0}, {0, -1.0356}, {0, 0}},
      {{0, 0}, {0, -1.0356}, {0, 0}, {1.0356, 0}, {0, 0}, {0, 0}}};
  return path;
}
const LottiePath& LidBar() {
  static const LottiePath path = {
      {{15.0, 17.8125}, {45.0, 17.8125}}, {{0, 0}, {0, 0}}, {{0, 0}, {0, 0}}};
  return path;
}
const LottiePath& LineLeft() {
  static const LottiePath path = {
      {{26.25, 29.0625}, {26.25, 39.375}}, {{0, 0}, {0, 0}}, {{0, 0}, {0, 0}}};
  return path;
}
const LottiePath& LineRight() {
  static const LottiePath path = {
      {{33.75, 29.0625}, {33.75, 39.375}}, {{0, 0}, {0, 0}}, {{0, 0}, {0, 0}}};
  return path;
}
const LottiePath& Body() {
  static const LottiePath path = {
      {{18.75, 23.4375}, {20.25, 42.75}, {24.0, 45.9375}, {36.0, 45.9375},
       {39.75, 42.75}, {41.25, 23.4375}},
      {{0, 0}, {0, 0}, {-1.8699, 0.0211}, {0, 0}, {-0.2804, 1.8489}, {0, 0}},
      {{0, 0}, {0.2804, 1.8489}, {0, 0}, {1.8699, 0.0211}, {0, 0}, {0, 0}}};
  return path;
}

// y of cubic-bezier(x1, y1, x2, y2) at x, by bisection (monotonic in x).
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

// A three-keyframe property (0 -> peak at `mid` -> back by frame 30), each
// leg eased cubic-bezier(0.42, 0, 0.58, 1) as the Lottie's o / i handles say.
double Keyframed(double frame, double rest, double peak, double mid) {
  if (frame <= 0 || frame >= kFrames) {
    return rest;
  }
  if (frame <= mid) {
    return rest + (peak - rest) * CubicBezier(0.42, 0, 0.58, 1, frame / mid);
  }
  return peak + (rest - peak) * CubicBezier(0.42, 0, 0.58, 1,
                                            (frame - mid) / (kFrames - mid));
}

// The path as a polyline, each point run through `map`.
template <typename Map>
std::vector<Pt> Flatten(const LottiePath& path, Map map) {
  std::vector<Pt> points = {map(path.v[0])};
  for (size_t k = 0; k + 1 < path.v.size(); ++k) {
    const Pt p0 = path.v[k];
    const Pt p3 = path.v[k + 1];
    const Pt p1 = {p0.x + path.out[k].x, p0.y + path.out[k].y};
    const Pt p2 = {p3.x + path.in[k + 1].x, p3.y + path.in[k + 1].y};
    const bool straight = path.out[k].x == 0 && path.out[k].y == 0 &&
                          path.in[k + 1].x == 0 && path.in[k + 1].y == 0;
    const int steps = straight ? 1 : 12;
    for (int i = 1; i <= steps; ++i) {
      const double t = static_cast<double>(i) / steps;
      const double u = 1 - t;
      points.push_back(map(
          {u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x +
               t * t * t * p3.x,
           u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y +
               t * t * t * p3.y}));
    }
  }
  return points;
}

double SegmentDistance(Pt p, Pt a, Pt b) {
  const double dx = b.x - a.x;
  const double dy = b.y - a.y;
  const double len2 = dx * dx + dy * dy;
  double t = len2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0;
  t = std::clamp(t, 0.0, 1.0);
  const double ex = p.x - (a.x + t * dx);
  const double ey = p.y - (a.y + t * dy);
  return std::sqrt(ex * ex + ey * ey);
}

// Strokes `points` (pixel space) `half` px either side, round-capped and
// round-joined, into `mask` at `opacity`: each pixel takes its centre's
// distance to the polyline, with a one-pixel ramp for the edge.
void Stroke(std::vector<double>& mask,
            int size,
            const std::vector<Pt>& points,
            double half,
            double opacity) {
  double min_x = points[0].x, max_x = points[0].x;
  double min_y = points[0].y, max_y = points[0].y;
  for (const Pt& p : points) {
    min_x = std::min(min_x, p.x);
    max_x = std::max(max_x, p.x);
    min_y = std::min(min_y, p.y);
    max_y = std::max(max_y, p.y);
  }
  const int x0 = std::max(0, static_cast<int>(std::floor(min_x - half - 1)));
  const int x1 = std::min(size - 1, static_cast<int>(std::ceil(max_x + half + 1)));
  const int y0 = std::max(0, static_cast<int>(std::floor(min_y - half - 1)));
  const int y1 = std::min(size - 1, static_cast<int>(std::ceil(max_y + half + 1)));
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      const Pt c = {x + 0.5, y + 0.5};
      double d = SegmentDistance(c, points[0], points[0]);
      for (size_t i = 0; i + 1 < points.size(); ++i) {
        d = std::min(d, SegmentDistance(c, points[i], points[i + 1]));
      }
      const double cover = std::clamp(half + 0.5 - d, 0.0, 1.0) * opacity;
      double& cell = mask[static_cast<size_t>(y) * size + x];
      cell = std::max(cell, cover);
    }
  }
}

}  // namespace

std::vector<unsigned char> PhiClearTabsIconMask(double t, int size) {
  std::vector<unsigned char> out;
  if (size <= 0) {
    return out;
  }
  const double frame = kFrames * std::clamp(t, 0.0, 1.0);
  const double px = size / kFrame;
  // Lid: rotation 0 -> -18 -> 0 (peak at frame 10.5) about its anchor at
  // the bar's left end, which itself lifts 17.8125 -> 15.9375 -> back.
  const double angle =
      Keyframed(frame, 0, -18, 10.5) * std::numbers::pi / 180.0;
  const Pt lid_anchor = {15.0, 17.8125};
  const Pt lid_at = {15.0, Keyframed(frame, 17.8125, 15.9375, 10.5)};
  const double cs = std::cos(angle);
  const double sn = std::sin(angle);
  const auto lid = [&](Pt p) {
    const double dx = p.x - lid_anchor.x;
    const double dy = p.y - lid_anchor.y;
    return Pt{(lid_at.x + cs * dx - sn * dy) * px,
              (lid_at.y + sn * dx + cs * dy) * px};
  };
  // Body (and the Lines, its children): vertical scale 100 -> 94 -> 100 %
  // (peak at frame 13.5) about the base, (30, 45.9375).
  const double squash = Keyframed(frame, 1.0, 0.94, 13.5);
  const auto body = [&](Pt p) {
    return Pt{p.x * px, (45.9375 + (p.y - 45.9375) * squash) * px};
  };
  // Lines: opacity 100 -> 30 -> 100 % (peak at frame 12).
  const double lines = Keyframed(frame, 1.0, 0.3, 12.0);

  std::vector<double> mask(static_cast<size_t>(size) * size, 0.0);
  const double half = kStroke / 2 * px;
  Stroke(mask, size, Flatten(LidHandle(), lid), half, 1.0);
  Stroke(mask, size, Flatten(LidBar(), lid), half, 1.0);
  Stroke(mask, size, Flatten(Body(), body), half, 1.0);
  Stroke(mask, size, Flatten(LineLeft(), body), half, lines);
  Stroke(mask, size, Flatten(LineRight(), body), half, lines);
  out.resize(mask.size());
  for (size_t i = 0; i < mask.size(); ++i) {
    out[i] = static_cast<unsigned char>(std::lround(mask[i] * 255.0));
  }
  return out;
}

void PhiPaintClearTabsIcon(HDC dc, const RECT& art, COLORREF ink, double t) {
  const int size = art.right - art.left;
  if (size <= 0) {
    return;
  }
  const std::vector<unsigned char> mask = PhiClearTabsIconMask(t, size);
  const PhiMacIcon icon = {size, size, mask.data()};
  const std::vector<unsigned char> bgra = PhiTintedIconBgra(icon, ink, size);
  PhiBlendBgra(dc, art, bgra.data(), size, size);
}

}  // namespace phi_client

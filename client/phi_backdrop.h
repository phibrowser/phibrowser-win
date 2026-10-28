// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_BACKDROP_H_
#define PHIBROWSER_WIN_CLIENT_PHI_BACKDROP_H_

#include <algorithm>
#include <cstdint>
#include <vector>

#include "phibrowser-win/client/phi_theme.h"

namespace phi_client {

// Recover coverage from GDI's opaque RGB painting over a known ground.
// Removing the ground contribution keeps antialiased glyph edges from
// acquiring opaque halos on glass. An 80% tint remains over the OS backdrop.
// Inputs are packed RGB (0xRRGGBB); output is premultiplied ARGB for a DIB.
//
// The theme ground's opacity over the OS backdrop (sidebar, address row and
// window ground alike): the Mac's fixed overlay alpha over its vibrancy
// material (ThemeColorAdjustment.defaultOverlayAlpha, 0.8), so the backdrop
// shows through by a fifth.
inline constexpr int kPhiBackdropTintAlpha = 204;

inline int PhiBackdropCoverage(uint32_t pixel, uint32_t ground) {
  int coverage = 0;
  for (int shift : {0, 8, 16}) {
    const int c = (pixel >> shift) & 255;
    const int g = (ground >> shift) & 255;
    if (c < g) {
      coverage = std::max(coverage, ((g - c) * 255 + g - 1) / g);
    } else if (c > g) {
      coverage = std::max(coverage,
                          ((c - g) * 255 + 254 - g) / (255 - g));
    }
  }
  return coverage;
}

// The tint is `pixel` read as one colour at kPhiBackdropTintAlpha over
// `ground`, as the Mac lays its overlay colour over the material; only a
// pixel too far from `ground` for that alpha takes more.
inline uint32_t PhiBackdropPixel(uint32_t pixel, uint32_t ground) {
  const int alpha =
      std::max(kPhiBackdropTintAlpha, PhiBackdropCoverage(pixel, ground));
  uint32_t result = static_cast<uint32_t>(alpha) << 24;
  for (int shift : {0, 8, 16}) {
    const int c = (pixel >> shift) & 255;
    const int g = (ground >> shift) & 255;
    const int premultiplied = c - (g * (255 - alpha) + 127) / 255;
    result |= static_cast<uint32_t>(std::clamp(premultiplied, 0, alpha))
              << shift;
  }
  return result;
}

// Skia's SkColor takes straight ARGB, unlike the shell's premultiplied DIB.
// Use the same theme tint for compositor-owned surfaces such as split gaps.
inline uint32_t PhiBackdropColor(uint32_t ground, bool dark) {
  const uint32_t pixel = PhiBackdropPixel(ground, dark ? 0 : 0xFFFFFF);
  const uint32_t alpha = pixel >> 24;
  uint32_t result = alpha << 24;
  for (int shift : {0, 8, 16}) {
    result |= ((((pixel >> shift) & 255) * 255 + alpha / 2) / alpha) << shift;
  }
  return result;
}

// Replace a GDI image's opaque ground with a premultiplied backdrop, while
// preserving its controls' coverage. Derive the backdrop by removing the
// neutral white/black backing from the theme ground with PhiBackdropPixel:
// fading the already-tinted ground itself would attenuate the theme twice.
inline uint32_t PhiBackdropPixelOver(uint32_t pixel,
                                     uint32_t ground,
                                     uint32_t backdrop,
                                     int minimum_coverage = 0) {
  // Known geometry can require opaque coverage even when a card happens
  // to have the same RGB as the ground beneath it.
  const int coverage = std::max(PhiBackdropCoverage(pixel, ground),
                                std::clamp(minimum_coverage, 0, 255));
  const int remaining = 255 - coverage;
  const int alpha = coverage + ((backdrop >> 24) * remaining + 127) / 255;
  uint32_t result = static_cast<uint32_t>(alpha) << 24;
  for (int shift : {0, 8, 16}) {
    const int c = (pixel >> shift) & 255;
    const int g = (ground >> shift) & 255;
    const int b = (backdrop >> shift) & 255;
    const int channel = c - (g * remaining + 127) / 255 +
                        (b * remaining + 127) / 255;
    result |= static_cast<uint32_t>(std::clamp(channel, 0, alpha)) << shift;
  }
  return result;
}

// The packed RGB (0xRRGGBB) of a COLORREF / PhiColor (0x00BBGGRR).
inline uint32_t PhiPackedRgb(PhiColor color) {
  return (static_cast<uint32_t>(PhiRed(color)) << 16) |
         (static_cast<uint32_t>(PhiGreen(color)) << 8) |
         static_cast<uint32_t>(PhiBlue(color));
}

// The backdrop each row of a surface sits on, for PhiBackdropPixelOver:
// `palette`'s sidebar ground at that row's place in a column `full_height`
// tall whose top is `first_row` rows above the surface. `glass` gives the
// premultiplied tint over the OS backdrop (PhiBackdropPixel); otherwise the
// ground itself, opaque. Painting stays on the flat ground_top; regrading
// each pixel against it moves the ground onto the palette's gradient while
// every wash, label and card keeps its coverage.
inline std::vector<uint32_t> PhiBackdropRowGrounds(const PhiPalette& palette,
                                                   int first_row,
                                                   int rows,
                                                   int full_height,
                                                   bool glass) {
  std::vector<uint32_t> grounds(static_cast<size_t>(std::max(rows, 0)));
  const double span = full_height > 1 ? full_height - 1.0 : 1.0;
  for (int y = 0; y < rows; ++y) {
    const uint32_t rgb = PhiPackedRgb(
        PhiPaletteGroundAt(palette, (first_row + y) / span));
    grounds[static_cast<size_t>(y)] =
        glass ? PhiBackdropPixel(rgb, palette.dark ? 0 : 0xFFFFFF)
              : 0xFF000000u | rgb;
  }
  return grounds;
}

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_BACKDROP_H_

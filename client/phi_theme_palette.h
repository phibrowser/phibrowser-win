// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_THEME_PALETTE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_THEME_PALETTE_H_

#include <array>
#include <string>

#include "phibrowser-win/client/phi_util.h"

// The Mac's ThemePalette (Utilities/Theme/Theme.swift, 3d045cf6 /
// 4c9d919e / 23c728d9): ONE per-Space record — an anchor hue, a shared
// saturation and brightness and a harmony — that resolves to BOTH
// appearances through fixed HSB maps. Only the anchor hue is stored; the
// other one or two colours derive from the harmony (analogous +45°,
// complementary +180°, split complementary +150° / +210°).
//
// The record is LOCAL: it lives in the account defaults beside the theme
// pins (spaceThemePalettes, the Mac's key) and is NOT on the Space sync
// payload — a palette Space syncs its theme pin as before and a peer
// renders that. HOST-CLEAN: STL + phi_util.h, so the geometry and the
// colour maps can be host-tested without windows.h.
namespace phi_client {

// HSB components, each 0...1 — what NSColor.getHue(saturation:brightness:)
// reads and what NSColor(hue:saturation:brightness:) takes.
struct PhiHsb {
  double hue = 0;
  double saturation = 0;
  double brightness = 0;
};

PhiHsb PhiHsbOf(PhiColor color);
PhiColor PhiColorFromHsb(const PhiHsb& hsb);

struct PhiThemePalette {
  enum class Harmony { kSingle, kAnalogous, kComplementary, kSplitComplementary };

  // A point of the unit field (0...1 both ways, y down).
  struct Point {
    double x = 0.5;
    double y = 0.5;
  };

  // The stored record's schema version (ThemePalette.version).
  static constexpr int kVersion = 3;

  double hue = 0;         // 0...1, wrapped.
  double saturation = 0;  // 0...1: the handle's radius on the field.
  double brightness = 1;  // 0...1; the removed slider's right endpoint.
  Harmony harmony = Harmony::kSingle;

  bool operator==(const PhiThemePalette&) const = default;

  // Normalises as the Mac's initialiser does: hue wrapped, the rest clamped.
  static PhiThemePalette Make(double hue,
                              double saturation,
                              double brightness,
                              Harmony harmony);
  // The Mac's init(color:appearance:): the record whose resolved colour for
  // `dark` is `color` — the inverse of the HSB maps, preferring the outer
  // dark radius when it can represent the input brightness.
  static PhiThemePalette FromColor(PhiColor color, bool dark);

  int color_count() const;
  static int ColorCountOf(Harmony harmony);
  // A legacy record's harmony from its colour count (1 single, 2
  // complementary, 3 split complementary).
  static Harmony HarmonyForCount(int count);
  // The wire spelling ("single", "analogous", "complementary",
  // "splitComplementary"); false for anything else.
  static const char* HarmonyName(Harmony harmony);
  static bool HarmonyFromName(const std::string& name, Harmony* out);

  // The hue of colour `index` (0 is the anchor), wrapped.
  double HueAt(int index) const;
  // The resolved HSB of colour `index` for one appearance — the Mac's
  // resolvedColor: light S = sat × 0.52, B = 0.65 + 0.35 × brightness;
  // dark S from the two-branch radial curve (90 % at 40 % of the radius,
  // 45 % at the edge), B = 0.12 + brightness × (0.24 + 0.54 × sat).
  PhiHsb ResolvedHsb(int index, bool dark) const;
  PhiColor ResolvedColor(int index, bool dark) const {
    return PhiColorFromHsb(ResolvedHsb(index, dark));
  }
  // The accent (the Mac's themeColor) the palette's anchor hue carries:
  // S .65 / B .60 in light, S .50 / B .85 in dark.
  PhiColor Accent(bool dark) const;

  // Samples the field at `point` with the handles' own coordinate mapping
  // and colour resolution (no centre snap) — what the field bitmap shows.
  PhiHsb ColorAt(Point point, bool dark) const;

  // Where handle `index` sits on the unit field: hue → angle through the
  // Figma spectrum stops, radius = saturation × 0.42.
  Point HandlePosition(int index) const;
  // The record after handle `index` is dragged to `point`: the anchor
  // follows so the harmony holds across the hue wrap. `snap_to_center`
  // (pointer edits only) sends every linked handle to neutral within
  // 0.025 of the centre, keeping the hue.
  PhiThemePalette MovingHandle(int index,
                               Point point,
                               bool snap_to_center) const;
  PhiThemePalette SettingHarmony(Harmony value) const;
  PhiThemePalette SettingBrightness(double value) const;
  PhiThemePalette SettingSaturation(double value) const;
  PhiThemePalette RotatingHue(double delta) const;

  // The field's radius as a fraction of its side (handles never leave it).
  static constexpr double kFieldRadius = 0.42;
  // Within this radius a pointer edit snaps to neutral (≈ 6 pt of 244).
  static constexpr double kCenterSnapRadius = 0.025;
};

// The Mac sidebar's palette gradient (ColoredVisualEffectView.gradientColors)
// sampled at `position` (0 top ... 1 bottom): one continuous blend in Oklab,
// which avoids RGB's brightness dips. Two colours run linearly; three are a
// quadratic Bezier on the squared position, so the first colour holds the
// upper half and the other two overlap below it. One colour is flat.
PhiColor PhiOklabGradient(const std::array<PhiColor, 3>& colors,
                          int count,
                          double position);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_THEME_PALETTE_H_

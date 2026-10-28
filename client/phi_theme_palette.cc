// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_theme_palette.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace phi_client {

namespace {

constexpr double kPi = 3.14159265358979323846;

// The dark map's radial curve (ThemePalette.darkSaturation).
constexpr double kDarkInnerRadius = 0.4;
constexpr double kDarkInnerSaturation = 0.9;
constexpr double kDarkOuterSaturation = 0.45;

// The supplied Figma field has nonuniform angular colour stops. Hue maps
// through those same stops so the handles track the displayed spectrum.
constexpr std::array<double, 8> kSpectrumHues = {
    0, 1.0 / 12, 1.0 / 6, 1.0 / 3, 0.5, 2.0 / 3, 5.0 / 6, 1};
constexpr std::array<double, 8> kSpectrumAngles = {
    0, 0.149, 0.332, 0.486, 0.620, 0.784, 0.880, 1};

double WrappedHue(double value) {
  if (!std::isfinite(value)) {
    return 0;
  }
  return value - std::floor(value);
}

double Unit(double value) {
  return std::isfinite(value) ? std::min(std::max(value, 0.0), 1.0) : 0;
}

// Piecewise-linear map of `value` from the `source` stops onto `target`.
double Interpolate(double value,
                   const std::array<double, 8>& source,
                   const std::array<double, 8>& target) {
  size_t index = source.size() - 1;
  for (size_t i = 1; i < source.size(); ++i) {
    if (value <= source[i]) {
      index = i;
      break;
    }
  }
  const double fraction =
      (value - source[index - 1]) / (source[index] - source[index - 1]);
  return target[index - 1] + fraction * (target[index] - target[index - 1]);
}

double HueOffset(PhiThemePalette::Harmony harmony, int index) {
  switch (harmony) {
    case PhiThemePalette::Harmony::kSingle:
      return 0;
    case PhiThemePalette::Harmony::kAnalogous:
      return index * 45.0 / 360.0;
    case PhiThemePalette::Harmony::kComplementary:
      return index / 2.0;
    case PhiThemePalette::Harmony::kSplitComplementary:
      switch (index) {
        case 1:
          return 150.0 / 360.0;
        case 2:
          return 210.0 / 360.0;
        default:
          return 0;
      }
  }
  return 0;
}

}  // namespace

PhiHsb PhiHsbOf(PhiColor color) {
  const double r = PhiRed(color) / 255.0;
  const double g = PhiGreen(color) / 255.0;
  const double b = PhiBlue(color) / 255.0;
  const double max = std::max({r, g, b});
  const double min = std::min({r, g, b});
  const double delta = max - min;
  PhiHsb out;
  out.brightness = max;
  out.saturation = max > 0 ? delta / max : 0;
  if (delta > 0) {
    double hue = 0;
    if (max == r) {
      hue = (g - b) / delta;
    } else if (max == g) {
      hue = 2 + (b - r) / delta;
    } else {
      hue = 4 + (r - g) / delta;
    }
    out.hue = WrappedHue(hue / 6);
  }
  return out;
}

PhiColor PhiColorFromHsb(const PhiHsb& hsb) {
  const double h = WrappedHue(hsb.hue) * 6;
  const double s = Unit(hsb.saturation);
  const double v = Unit(hsb.brightness);
  const double c = v * s;
  const double x = c * (1 - std::fabs(std::fmod(h, 2.0) - 1));
  const double m = v - c;
  double r = 0, g = 0, b = 0;
  if (h < 1) {
    r = c;
    g = x;
  } else if (h < 2) {
    r = x;
    g = c;
  } else if (h < 3) {
    g = c;
    b = x;
  } else if (h < 4) {
    g = x;
    b = c;
  } else if (h < 5) {
    r = x;
    b = c;
  } else {
    r = c;
    b = x;
  }
  const auto channel = [m](double value) {
    return static_cast<int>(std::lround(Unit(value + m) * 255));
  };
  return PhiRgb(channel(r), channel(g), channel(b));
}

PhiThemePalette PhiThemePalette::Make(double hue,
                                      double saturation,
                                      double brightness,
                                      Harmony harmony) {
  PhiThemePalette palette;
  palette.hue = WrappedHue(hue);
  palette.saturation = Unit(saturation);
  palette.brightness = Unit(brightness);
  palette.harmony = harmony;
  return palette;
}

PhiThemePalette PhiThemePalette::FromColor(PhiColor color, bool dark) {
  const PhiHsb hsb = PhiHsbOf(color);
  const double lower = dark ? 0.12 : 0.65;
  double saturation = 0;
  if (dark) {
    const double color_saturation =
        std::min(hsb.saturation, kDarkInnerSaturation);
    // Invert the squared smoothstep used by the inner field.
    const double inner_mix = std::sqrt(color_saturation / kDarkInnerSaturation);
    const double inner_radius =
        kDarkInnerRadius * (0.5 - std::sin(std::asin(1 - 2 * inner_mix) / 3));
    const double outer_radius = Unit(
        kDarkInnerRadius +
        (1 - kDarkInnerRadius) *
            std::sqrt((kDarkInnerSaturation - color_saturation) /
                      (kDarkInnerSaturation - kDarkOuterSaturation)));
    // Two radii can share a saturation. Prefer the outer one when it can
    // represent the input brightness; otherwise use the neutral-centre
    // branch.
    saturation = color_saturation >= kDarkOuterSaturation - 0.000001 &&
                         hsb.brightness <= 0.36 + 0.54 * outer_radius + 0.000001
                     ? outer_radius
                     : inner_radius;
  } else {
    saturation = Unit(hsb.saturation / 0.52);
  }
  const double span = dark ? 0.24 + 0.54 * saturation : 0.35;
  return Make(hsb.hue, saturation, (hsb.brightness - lower) / span,
              Harmony::kSingle);
}

int PhiThemePalette::ColorCountOf(Harmony harmony) {
  switch (harmony) {
    case Harmony::kSingle:
      return 1;
    case Harmony::kAnalogous:
    case Harmony::kComplementary:
      return 2;
    case Harmony::kSplitComplementary:
      return 3;
  }
  return 1;
}

int PhiThemePalette::color_count() const {
  return ColorCountOf(harmony);
}

PhiThemePalette::Harmony PhiThemePalette::HarmonyForCount(int count) {
  switch (std::min(std::max(count, 1), 3)) {
    case 1:
      return Harmony::kSingle;
    case 2:
      return Harmony::kComplementary;
    default:
      return Harmony::kSplitComplementary;
  }
}

const char* PhiThemePalette::HarmonyName(Harmony harmony) {
  switch (harmony) {
    case Harmony::kSingle:
      return "single";
    case Harmony::kAnalogous:
      return "analogous";
    case Harmony::kComplementary:
      return "complementary";
    case Harmony::kSplitComplementary:
      return "splitComplementary";
  }
  return "single";
}

bool PhiThemePalette::HarmonyFromName(const std::string& name, Harmony* out) {
  for (Harmony harmony : {Harmony::kSingle, Harmony::kAnalogous,
                          Harmony::kComplementary,
                          Harmony::kSplitComplementary}) {
    if (name == HarmonyName(harmony)) {
      *out = harmony;
      return true;
    }
  }
  return false;
}

double PhiThemePalette::HueAt(int index) const {
  return WrappedHue(hue + HueOffset(harmony, index));
}

PhiHsb PhiThemePalette::ResolvedHsb(int index, bool dark) const {
  PhiHsb out;
  out.hue = HueAt(index);
  if (dark) {
    double dark_saturation = 0;
    if (saturation <= kDarkInnerRadius) {
      const double progress = saturation / kDarkInnerRadius;
      const double smooth = progress * progress * (3 - 2 * progress);
      dark_saturation = kDarkInnerSaturation * smooth * smooth;
    } else {
      const double progress =
          (saturation - kDarkInnerRadius) / (1 - kDarkInnerRadius);
      dark_saturation = kDarkInnerSaturation -
                        (kDarkInnerSaturation - kDarkOuterSaturation) *
                            progress * progress;
    }
    out.saturation = dark_saturation;
    out.brightness = 0.12 + brightness * (0.24 + saturation * 0.54);
  } else {
    out.saturation = saturation * 0.52;
    out.brightness = 0.65 + brightness * 0.35;
  }
  return out;
}

PhiColor PhiThemePalette::Accent(bool dark) const {
  return PhiColorFromHsb({hue, dark ? 0.50 : 0.65, dark ? 0.85 : 0.60});
}

PhiHsb PhiThemePalette::ColorAt(Point point, bool dark) const {
  const PhiThemePalette sample =
      MovingHandle(0, point, /*snap_to_center=*/false);
  return sample.ResolvedHsb(0, dark);
}

PhiThemePalette::Point PhiThemePalette::HandlePosition(int index) const {
  const double angle =
      Interpolate(HueAt(index), kSpectrumHues, kSpectrumAngles) * 2 * kPi -
      kPi / 4;
  const double radius = saturation * kFieldRadius;
  return {0.5 + std::cos(angle) * radius, 0.5 + std::sin(angle) * radius};
}

PhiThemePalette PhiThemePalette::MovingHandle(int index,
                                              Point point,
                                              bool snap_to_center) const {
  const double x = point.x - 0.5;
  const double y = point.y - 0.5;
  const double radius = std::hypot(x, y);
  // Interaction snaps within about 6 points in the 244-point field. Colour
  // sampling keeps the continuous mapping by leaving this disabled.
  if (snap_to_center && radius <= kCenterSnapRadius) {
    return Make(hue, 0, brightness, harmony);
  }
  // Retain the hue at the white centre, where the angle is undefined.
  const double angle = WrappedHue((std::atan2(y, x) + kPi / 4) / (2 * kPi));
  const double moved_hue =
      radius < 0.0001 ? HueAt(index)
                      : Interpolate(angle, kSpectrumAngles, kSpectrumHues);
  return Make(moved_hue - HueOffset(harmony, index), radius / kFieldRadius,
              brightness, harmony);
}

PhiThemePalette PhiThemePalette::SettingHarmony(Harmony value) const {
  return Make(hue, saturation, brightness, value);
}

PhiThemePalette PhiThemePalette::SettingBrightness(double value) const {
  return Make(hue, saturation, value, harmony);
}

PhiThemePalette PhiThemePalette::SettingSaturation(double value) const {
  return Make(hue, value, brightness, harmony);
}

PhiThemePalette PhiThemePalette::RotatingHue(double delta) const {
  return Make(hue + delta, saturation, brightness, harmony);
}

namespace {

struct Oklab {
  double l = 0;
  double a = 0;
  double b = 0;
};

Oklab OklabOf(PhiColor color) {
  const auto linear = [](int value) {
    const double c = value / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
  };
  const double r = linear(PhiRed(color));
  const double g = linear(PhiGreen(color));
  const double b = linear(PhiBlue(color));
  const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g +
                             0.0514459929 * b);
  const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g +
                             0.1073969566 * b);
  const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g +
                             0.6299787005 * b);
  return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
          1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
          0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

PhiColor ColorOfOklab(const Oklab& value) {
  const double l_root = value.l + 0.3963377774 * value.a + 0.2158037573 * value.b;
  const double m_root = value.l - 0.1055613458 * value.a - 0.0638541728 * value.b;
  const double s_root = value.l - 0.0894841775 * value.a - 1.2914855480 * value.b;
  const double l = l_root * l_root * l_root;
  const double m = m_root * m_root * m_root;
  const double s = s_root * s_root * s_root;
  const auto encoded = [](double c) {
    c = Unit(c);
    c = c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055;
    return static_cast<int>(c * 255 + 0.5);
  };
  return PhiRgb(
      encoded(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s),
      encoded(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s),
      encoded(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s));
}

}  // namespace

PhiColor PhiOklabGradient(const std::array<PhiColor, 3>& colors,
                          int count,
                          double position) {
  count = std::min(count, 3);
  if (count <= 1) {
    return colors[0];
  }
  position = Unit(position);
  if (position <= 0) {
    return colors[0];
  }
  if (position >= 1) {
    return colors[static_cast<size_t>(count - 1)];
  }
  const Oklab first = OklabOf(colors[0]);
  const Oklab second = OklabOf(colors[1]);
  Oklab value;
  if (count >= 3) {
    const Oklab third = OklabOf(colors[2]);
    const double t = position * position;
    const double remaining = 1 - t;
    const double w0 = remaining * remaining;
    const double w1 = 2 * remaining * t;
    const double w2 = t * t;
    value = {first.l * w0 + second.l * w1 + third.l * w2,
             first.a * w0 + second.a * w1 + third.a * w2,
             first.b * w0 + second.b * w1 + third.b * w2};
  } else {
    value = {first.l + (second.l - first.l) * position,
             first.a + (second.a - first.a) * position,
             first.b + (second.b - first.b) * position};
  }
  return ColorOfOklab(value);
}

}  // namespace phi_client

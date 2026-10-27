// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_theme.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "base/no_destructor.h"
#include "phibrowser-win/client/phi_util.h"

#if defined(_WIN32)
#include "base/win/registry.h"
#endif

namespace phi_client {

namespace {

// The Mac writes colours as 0xRRGGBB; Win32 wants COLORREF (0x00BBGGRR).
constexpr PhiColor Hex(int rgb) {
  return PhiRgb((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// The one blend now lives in phi_util.h (PhiBlend); this file's private copy
// was the original of the eight the review found.

// How much of the overlay colour reaches the window. The Mac composites the
// overlay at 0.8 and then scales it by the theme's saturation slider
// (OverlaySaturationScale, 0.1...0.9), so the slider — not the 0.8 — is what
// the user actually feels. Mapped here onto HALF that span (Jixiang,
// 2026-09-10: the top of the slider was too heavy; the ceiling is half of
// what it was, and the default sits at 10, a light wash): slider 0 is
// barely tinted, 100 is a moderate tint.
constexpr int kTintMinAlpha = 20;
constexpr int kTintMaxAlpha = 80;
// The gradient's foot is a little stronger than its head.
constexpr int kTintGradientSpread = 36;
// A linked palette's anchor colour reaches the window at the Mac's fixed
// overlay alpha (ThemeColorAdjustment.defaultOverlayAlpha, 0.8): the
// palette's own saturation is the user's radius on the field, so no slider
// scales it further.
constexpr int kPaletteOverlayAlpha = 204;

bool ResolveDark(PhiAppearance appearance) {
  return appearance == PhiAppearance::kDark ||
         (appearance == PhiAppearance::kSystem && PhiSystemPrefersDark());
}

}  // namespace

const char* PhiAppearanceName(PhiAppearance appearance) {
  switch (appearance) {
    case PhiAppearance::kLight:
      return "light";
    case PhiAppearance::kDark:
      return "dark";
    case PhiAppearance::kSystem:
      break;
  }
  return "system";
}

bool PhiAppearanceFromName(const std::string& name, PhiAppearance* out) {
  if (name == "system") {
    *out = PhiAppearance::kSystem;
  } else if (name == "light") {
    *out = PhiAppearance::kLight;
  } else if (name == "dark") {
    *out = PhiAppearance::kDark;
  } else {
    return false;
  }
  return true;
}

const std::vector<PhiTheme>& PhiBuiltInThemes() {
  // Values transcribed from ThemedColors.swift's makeDesignTheme calls.
  static const base::NoDestructor<std::vector<PhiTheme>> themes({
      {"pure", "Pure",
       {Hex(0xFFFFFF), Hex(0xFFFFFF), Hex(0x3AA4D5)},
       {Hex(0x383838), Hex(0x383838), Hex(0x1E7099)}},
      {"mint", "Mint",
       {Hex(0x8AE25A), Hex(0xFFFFFF), Hex(0x73BD4B)},
       {Hex(0x1B380B), Hex(0x1B380B), Hex(0x48802A)}},
      {"mist", "Mist",
       {Hex(0x66CCFF), Hex(0xFFFFFF), Hex(0x57AED9)},
       {Hex(0x0B2938), Hex(0x0B2938), Hex(0x1E7099)}},
      {"aqua", "Aqua",
       {Hex(0x5BDEE3), Hex(0xFFFFFF), Hex(0x4BB9BD)},
       {Hex(0x0B3738), Hex(0x0B3738), Hex(0x2A7D80)}},
      {"iris", "Iris",
       {Hex(0x7566FF), Hex(0xFFFFFF), Hex(0x6357D9)},
       {Hex(0x100B38), Hex(0x100B38), Hex(0x3D339C)}},
      {"petal", "Petal",
       {Hex(0xD966FF), Hex(0xFFFFFF), Hex(0xB857D9)},
       {Hex(0x2D0B38), Hex(0x2D0B38), Hex(0x82339C)}},
      {"coral", "Coral",
       {Hex(0xFF6666), Hex(0xFFFFFF), Hex(0xD95757)},
       {Hex(0x380B0B), Hex(0x380B0B), Hex(0x9C3E3E)}},
      {"amber", "Amber",
       {Hex(0xFFD966), Hex(0xFFFFFF), Hex(0xD9B857)},
       {Hex(0x382D0B), Hex(0x382D0B), Hex(0x9C8133)}},
  });
  return *themes;
}

const PhiTheme& PhiThemeById(const std::string& id) {
  for (const PhiTheme& theme : PhiBuiltInThemes()) {
    if (id == theme.id) {
      return theme;
    }
  }
  return PhiBuiltInThemes().front();  // Pure is the Mac's default.
}

bool PhiSystemPrefersDark() {
#if !defined(_WIN32)
  // The host tests resolve palettes without a registry; "system" reads
  // light there, which is what an unconfigured Windows reports too.
  return false;
#else
  // The same key the Windows shell uses for app appearance; 0 means dark.
  base::win::RegKey key(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      KEY_READ);
  DWORD light = 1;
  if (key.Valid() && key.ReadValueDW(L"AppsUseLightTheme", &light) ==
                         ERROR_SUCCESS) {
    return light == 0;
  }
  return false;
#endif
}

bool PhiHighContrastActive() {
#if !defined(_WIN32)
  return false;
#else
  HIGHCONTRASTW contrast = {};
  contrast.cbSize = sizeof(contrast);
  return ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast),
                                 &contrast, 0) &&
         (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
#endif
}

PhiPalette PhiPaletteFromSystemColors(const PhiSystemColors& colors) {
  PhiPalette palette;
  // The window's lightness only sets the ink-set FLAG (DWM's dark caption,
  // the NTP mark); every colour below is the system's.
  palette.dark = PhiContrastRatio(colors.window, PhiRgb(0xFF, 0xFF, 0xFF)) >
                 PhiContrastRatio(colors.window, PhiRgb(0, 0, 0));
  // Grounds: one flat window colour — no tint, no gradient.
  palette.ground_top = colors.window;
  palette.ground_bottom = colors.window;
  palette.window_bg = colors.window;
  palette.ground_fixed = colors.window;
  palette.card = colors.window;
  palette.flyout = colors.window;
  palette.dialog_footer = colors.window;
  palette.overlay_scrim = colors.window;
  // Text. Secondary text keeps the full text colour — a contrast theme has
  // no "dimmer but legible" step; gray text is for what is disabled or a
  // placeholder, which is the tertiary role.
  palette.ink = colors.window_text;
  palette.label = colors.window_text;
  palette.label_dim = colors.window_text;
  palette.label_tertiary = colors.gray_text;
  // The washes sit under label-coloured text: the button face, which the
  // theme pairs with a text colour of the same contrast. (The highlight
  // needs its own text colour, and no wash's caller switches to it.)
  palette.selected = colors.button_face;
  palette.hover = colors.button_face;
  palette.hover_strong = colors.button_face;
  palette.pressed = colors.button_face;
  palette.bar_hover = colors.button_face;
  palette.bar_hover_strong = colors.button_face;
  palette.control_fill = colors.button_face;
  palette.accent_soft = colors.button_face;
  // Edges in the text colour: a contrast theme draws its boundaries.
  palette.border = colors.window_text;
  palette.stroke_strong = colors.window_text;
  palette.separator = colors.gray_text;
  // The accent is the selection; an accent fill carries on_accent text.
  palette.accent = colors.highlight;
  palette.on_accent = colors.highlight_text;
  palette.accent_hover = colors.hot_light;
  // No red or amber of our own: a status colour that fails the user's
  // contrast is worse than the theme's own.
  palette.destructive = colors.highlight;
  palette.destructive_hover = colors.hot_light;
  palette.warning = colors.window_text;
  palette.error = colors.window_text;
  return palette;
}

PhiPalette PhiHighContrastPalette() {
  PhiSystemColors colors;
#if defined(_WIN32)
  colors.window = ::GetSysColor(COLOR_WINDOW);
  colors.window_text = ::GetSysColor(COLOR_WINDOWTEXT);
  colors.highlight = ::GetSysColor(COLOR_HIGHLIGHT);
  colors.highlight_text = ::GetSysColor(COLOR_HIGHLIGHTTEXT);
  colors.hot_light = ::GetSysColor(COLOR_HOTLIGHT);
  colors.gray_text = ::GetSysColor(COLOR_GRAYTEXT);
  colors.button_face = ::GetSysColor(COLOR_BTNFACE);
#endif
  return PhiPaletteFromSystemColors(colors);
}

namespace {

// The one ladder every resolved palette climbs: `colors`' overlay over its
// background at the two alphas is the ground, its accent the accent, and
// every wash, label and hairline derives from those. Shared by the theme
// resolver (the slider picks the alphas) and the linked-palette resolver
// (the Mac's fixed overlay alpha).
PhiPalette ResolveFromColors(const PhiThemeColors& colors,
                             bool dark,
                             int top_alpha,
                             int bottom_alpha) {
  PhiPalette palette;
  palette.dark = dark;
  palette.ground_top = PhiBlend(colors.overlay, colors.background, top_alpha);
  palette.ground_bottom =
      PhiBlend(colors.overlay, colors.background, bottom_alpha);
  palette.accent = colors.accent;
  palette.window_bg = colors.background;
  // The ink the Mac's percentage overlays are made of.
  const PhiColor ink = dark ? PhiRgb(0xFF, 0xFF, 0xFF) : PhiRgb(0, 0, 0);
  palette.ink = ink;
  // The Windows-native ladder (docs/windows-native-ui.md §2.3), as /255:
  // hover ink 4.5 % ≈ 11 (dark: white 6 % ≈ 15), pressed 9 % ≈ 23 (dark
  // 10 % ≈ 26), stroke 9 % ≈ 23 (dark white 8 % ≈ 20), separator 7 % ≈ 18,
  // label 87 % ≈ 222 (dark 90 % ≈ 230), dim 62 % ≈ 158, tertiary 42 % ≈ 107.
  palette.hover = PhiBlend(ink, palette.ground_top, dark ? 15 : 11);
  palette.pressed = PhiBlend(ink, palette.ground_top, dark ? 26 : 23);
  palette.hover_strong = palette.pressed;
  // The top bar's copies: same ink, its own ground.
  palette.bar_hover = PhiBlend(ink, palette.window_bg, dark ? 15 : 11);
  palette.bar_hover_strong = PhiBlend(ink, palette.window_bg, dark ? 26 : 23);
  palette.label_tertiary = PhiBlend(ink, palette.ground_top, 107);
  palette.border = PhiBlend(ink, palette.ground_top, dark ? 20 : 23);
  palette.stroke_strong = PhiBlend(ink, palette.ground_top, dark ? 41 : 23);
  palette.separator = PhiBlend(ink, palette.ground_top, 18);
  palette.control_fill = PhiBlend(ink, palette.window_bg, dark ? 15 : 11);
  if (dark) {
    // The selected row lifts off the ground in WHITE; the card / toolbar /
    // active tab is the design's #2B2B2B; the flyout a step lighter.
    const PhiColor white = PhiRgb(0xFF, 0xFF, 0xFF);
    palette.selected = PhiBlend(white, palette.ground_top, 77);
    palette.card = PhiRgb(0x2B, 0x2B, 0x2B);
    palette.label = PhiBlend(white, palette.ground_top, 230);
    palette.label_dim = PhiBlend(white, palette.ground_top, 158);
    palette.flyout = PhiRgb(0x2C, 0x2C, 0x2C);
    palette.dialog_footer = PhiRgb(0x20, 0x20, 0x20);
  } else {
    palette.selected = PhiRgb(0xFF, 0xFF, 0xFF);
    palette.card = PhiRgb(0xFF, 0xFF, 0xFF);
    palette.label = PhiBlend(ink, palette.ground_top, 222);
    palette.label_dim = PhiBlend(ink, palette.ground_top, 158);
    palette.flyout = PhiRgb(0xF9, 0xF9, 0xF9);
    palette.dialog_footer = PhiRgb(0xF3, 0xF3, 0xF3);
  }
  palette.accent_soft = PhiBlend(colors.accent, palette.card, 36);  // 14 %.

  // The roles the individual files were inventing. Values transcribed from
  // where they already exist: phi_popup_chrome.h:26-44 (the fixed ground and
  // the button ladder), phi_settings_window.cc:2441-2446 and :2491 (the
  // parallel palette and the close-button red), phi_shell_window.cc (the
  // second copy of that red).
  // The Windows Settings ground (#F3F3F3 / #202020): the Mac's #EAEAEA /
  // #252525 is retired with the Mac look (windows-native-ui.md §2.3).
  palette.ground_fixed =
      dark ? PhiRgb(0x20, 0x20, 0x20) : PhiRgb(0xF3, 0xF3, 0xF3);
  // Legibility over the accent, not a fixed white: the light Amber accent
  // (#D9B857) carries black text, everything darker carries white. The
  // threshold is Rec. 601 luma, which is what the Mac's
  // NSColor.usingColorSpace check comes out to for these eight accents.
  const int luma = (PhiRed(palette.accent) * 299 +
                    PhiGreen(palette.accent) * 587 +
                    PhiBlue(palette.accent) * 114) / 1000;
  palette.on_accent = luma > 150 ? PhiRgb(0x1A, 0x1A, 0x1A)
                                 : PhiRgb(0xFF, 0xFF, 0xFF);
  // Hover deepens the accent by pushing it a step toward the ink of the
  // OPPOSITE appearance, the way PhiAlertButton's .primary does.
  palette.accent_hover =
      PhiBlend(dark ? PhiRgb(0xFF, 0xFF, 0xFF) : PhiRgb(0, 0, 0),
               palette.accent, 26);
  // Windows' danger red (the caption close button's, and Fluent's critical
  // text): #C42B1C light, #E0463A dark — systemRed is the Mac's.
  palette.destructive = dark ? PhiRgb(0xE0, 0x46, 0x3A) : PhiRgb(0xC4, 0x2B, 0x1C);
  palette.destructive_hover =
      PhiBlend(dark ? PhiRgb(0xFF, 0xFF, 0xFF) : PhiRgb(0, 0, 0),
               palette.destructive, 26);
  palette.warning = dark ? PhiRgb(0xD9, 0xA5, 0x27) : PhiRgb(0xB0, 0x80, 0x00);
  palette.error = dark ? PhiRgb(0xE8, 0x5D, 0x5D) : PhiRgb(0xC0, 0x1E, 0x1E);
  // The dim behind a modal / the group overview: 40% black over the ground
  // in light, 55% in dark (a dark ground needs more to read as "behind").
  palette.overlay_scrim =
      PhiBlend(PhiRgb(0, 0, 0), palette.ground_top, dark ? 140 : 102);
  return palette;
}

}  // namespace

PhiPalette PhiResolvePalette(const PhiTheme& theme,
                             PhiAppearance appearance,
                             int saturation) {
  // A contrast theme overrides the app's theme, its appearance and the
  // Space tint alike (see the header).
  if (PhiHighContrastActive()) {
    return PhiHighContrastPalette();
  }
  saturation = PhiClamp(saturation, 0, 100);
  const int top_alpha =
      kTintMinAlpha + (kTintMaxAlpha - kTintMinAlpha) * saturation / 100;
  const int bottom_alpha = top_alpha + kTintGradientSpread > 255
                               ? 255
                               : top_alpha + kTintGradientSpread;
  const bool dark = ResolveDark(appearance);
  return ResolveFromColors(dark ? theme.dark : theme.light, dark, top_alpha,
                           bottom_alpha);
}

PhiPalette PhiResolvePalette(const PhiTheme& base,
                             PhiAppearance appearance,
                             const PhiThemePalette& palette) {
  if (PhiHighContrastActive()) {
    return PhiHighContrastPalette();
  }
  const bool dark = ResolveDark(appearance);
  // ThemePalette.applying(to:): the base keeps its window background; the
  // overlay and the accent are the palette's. The ground is flat — the
  // Mac's gradient has no home on the Windows sidebar (see the header).
  PhiThemeColors colors = dark ? base.dark : base.light;
  colors.overlay = palette.ResolvedColor(0, dark);
  colors.accent = palette.Accent(dark);
  return ResolveFromColors(colors, dark, kPaletteOverlayAlpha,
                           kPaletteOverlayAlpha);
}

namespace {

// WCAG 2 relative luminance of an sRGB colour.
double RelativeLuminance(PhiColor color) {
  const auto channel = [](int value) {
    const double c = value / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * channel(PhiRed(color)) + 0.7152 * channel(PhiGreen(color)) +
         0.0722 * channel(PhiBlue(color));
}

}  // namespace

double PhiContrastRatio(PhiColor a, PhiColor b) {
  const double la = RelativeLuminance(a);
  const double lb = RelativeLuminance(b);
  return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

PhiPalette PhiPageBarPalette(const PhiPalette& base, PhiColor page) {
  if (PhiHighContrastActive()) {
    return base;  // A page's colour never replaces the user's contrast.
  }
  const PhiColor white = PhiRgb(0xFF, 0xFF, 0xFF);
  const PhiColor black = PhiRgb(0, 0, 0);
  // WebContentHeader.updatePageColor: dark ink-set when white reads better.
  const bool dark = PhiContrastRatio(page, white) > PhiContrastRatio(page, black);
  const PhiColor ink = dark ? white : black;
  PhiPalette palette = base;
  palette.dark = dark;
  palette.ink = ink;
  palette.ground_top = page;
  palette.ground_bottom = page;
  palette.window_bg = page;
  palette.card = page;
  // PhiResolvePalette's ladder, over the page instead of the tint.
  palette.hover = PhiBlend(ink, page, dark ? 15 : 11);
  palette.pressed = PhiBlend(ink, page, dark ? 26 : 23);
  palette.hover_strong = palette.pressed;
  palette.bar_hover = palette.hover;
  palette.bar_hover_strong = palette.pressed;
  palette.control_fill = palette.hover;
  palette.label = PhiBlend(ink, page, dark ? 230 : 222);
  palette.label_dim = PhiBlend(ink, page, 158);
  palette.label_tertiary = PhiBlend(ink, page, 107);
  palette.border = PhiBlend(ink, page, dark ? 20 : 23);
  palette.stroke_strong = PhiBlend(ink, page, dark ? 41 : 23);
  palette.separator = PhiBlend(ink, page, 18);
  palette.selected = dark ? PhiBlend(white, page, 77) : white;
  palette.accent_soft = PhiBlend(base.accent, page, 36);
  return palette;
}

bool PhiParseColorHex(const std::string& hex, PhiColor* out) {
  const size_t start = !hex.empty() && hex[0] == '#' ? 1 : 0;
  if (hex.size() - start != 6) {
    return false;
  }
  std::array<int, 3> channels = {};
  for (size_t i = 0; i < channels.size(); ++i) {
    for (size_t nibble = 0; nibble < 2; ++nibble) {
      const char c = hex[start + i * 2 + nibble];
      int value = 0;
      if (c >= '0' && c <= '9') {
        value = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        value = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        value = c - 'A' + 10;
      } else {
        return false;
      }
      channels[i] = channels[i] * 16 + value;
    }
  }
  *out = PhiRgb(channels[0], channels[1], channels[2]);
  return true;
}

namespace {

// NSColor.getHue(_:saturation:brightness:alpha:) on an sRGB colour, each
// 0...1 — the Mac's snapping reads exactly these.
struct Hsb {
  double hue = 0;
  double saturation = 0;
  double brightness = 0;
};

Hsb HsbOf(PhiColor color) {
  const double r = PhiRed(color) / 255.0;
  const double g = PhiGreen(color) / 255.0;
  const double b = PhiBlue(color) / 255.0;
  const double max = std::max({r, g, b});
  const double min = std::min({r, g, b});
  const double delta = max - min;
  Hsb out;
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
    hue /= 6;
    out.hue = hue < 0 ? hue + 1 : hue;
  }
  return out;
}

// Below these a colour names no hue worth matching (the Mac's
// neutralSaturation / neutralBrightness).
constexpr double kNeutralSaturation = 0.15;
constexpr double kNeutralBrightness = 0.12;

}  // namespace

std::string PhiNearestThemeId(PhiColor color) {
  const PhiTheme& fallback = PhiBuiltInThemes().front();
  const Hsb source = HsbOf(color);
  if (source.saturation < kNeutralSaturation ||
      source.brightness < kNeutralBrightness) {
    return fallback.id;
  }
  // Hue wraps, so the distance is the shorter way round. Pure's overlay is
  // white — no hue — so it is only ever the fallback, never the nearest.
  const PhiTheme* nearest = nullptr;
  double best = 2;
  for (const PhiTheme& theme : PhiBuiltInThemes()) {
    if (&theme == &fallback) {
      continue;
    }
    const double delta = std::abs(source.hue - HsbOf(theme.light.overlay).hue);
    const double distance = std::min(delta, 1 - delta);
    if (distance < best) {
      best = distance;
      nearest = &theme;
    }
  }
  return nearest ? nearest->id : fallback.id;
}

std::string PhiThemeOverlayHex(const std::string& theme_id) {
  const PhiColor overlay = PhiThemeById(theme_id).light.overlay;
  char hex[8];
  std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", PhiRed(overlay),
                PhiGreen(overlay), PhiBlue(overlay));
  return hex;
}

std::string PhiThemeIdForName(const std::string& raw) {
  auto same = [](const std::string& a, const char* b) {
    const std::string other(b);
    return a.size() == other.size() &&
           std::equal(a.begin(), a.end(), other.begin(), [](char x, char y) {
             return std::tolower(static_cast<unsigned char>(x)) ==
                    std::tolower(static_cast<unsigned char>(y));
           });
  };
  for (const PhiTheme& theme : PhiBuiltInThemes()) {
    if (same(raw, theme.id) || same(raw, theme.name)) {
      return theme.id;
    }
  }
  return std::string();
}

}  // namespace phi_client

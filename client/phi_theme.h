// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_THEME_H_
#define PHIBROWSER_WIN_CLIENT_PHI_THEME_H_

#if defined(_WIN32)
#include <windows.h>
#endif

#include <array>
#include <string>
#include <vector>

#include "phibrowser-win/client/phi_theme_palette.h"
#include "phibrowser-win/client/phi_util.h"

namespace phi_client {

// The Mac client's theme model, ported (Utilities/Theme/Theme.swift and
// ThemedColors.swift): eight built-in themes, each carrying a light and a dark
// palette, plus a separate three-state appearance choice. Ids and names match
// the Mac raw values so a stored preference means the same thing on both.
enum class PhiAppearance { kSystem, kLight, kDark };

const char* PhiAppearanceName(PhiAppearance appearance);
bool PhiAppearanceFromName(const std::string& name, PhiAppearance* out);

// One theme's colours for one appearance. `windowOverlayBackground` over
// `windowBackground` is what tints the window; `themeColor` is the accent the
// Mac uses for pills and controls.
struct PhiThemeColors {
  PhiColor overlay = PhiRgb(0xFF, 0xFF, 0xFF);
  PhiColor background = PhiRgb(0xFF, 0xFF, 0xFF);
  PhiColor accent = PhiRgb(0x3A, 0xA4, 0xD5);
};

struct PhiTheme {
  const char* id;
  const char* name;
  PhiThemeColors light;
  PhiThemeColors dark;
};

// In the Mac's order: Pure, Mint, Mist, Aqua, Iris, Petal, Coral, Amber.
const std::vector<PhiTheme>& PhiBuiltInThemes();
// The theme with `id`, or Pure (the default) when unknown.
const PhiTheme& PhiThemeById(const std::string& id);

// What the chrome actually paints with, once a theme and an appearance have
// been resolved. Everything the client draws reads from here, so switching
// theme or appearance is a single recompute.
//
// GDI paints opaque, so the Mac's alpha overlays (black/white at 4%, 8%, …)
// are pre-blended over the ground here. The percentages are the Mac's asset
// values (Assets.xcassets ColorSet / DefaultColors.swift).
struct PhiPalette {
  PhiColor ground_top;
  PhiColor ground_bottom;
  PhiColor card;            // The settings window's section cards.
  PhiColor selected;        // sidebar-tab-selectedColor: white / white 30%.
  PhiColor hover;           // sidebar-tab-hoveredColor: ink 4%.
  PhiColor hover_strong;    // …-emphasized: ink 8%. Also resting field fill.
  // The same two overlays composited over `window_bg` instead of the tinted
  // ground — what the Mac gets for free by keeping them translucent. The top
  // bar stands on windowBackground, so anything washed THERE (the balanced /
  // comfortable address field) must blend over that, not over the Space tint:
  // blending over the tint paints a themed pill on an untinted bar.
  PhiColor bar_hover;
  PhiColor bar_hover_strong;
  PhiColor label;           // textPrimary: black 85% / white 80%.
  PhiColor label_dim;       // textSecondary.
  PhiColor label_tertiary;  // textTertiary: ink 30% — "New Tab", placeholders.
  PhiColor border;          // ink 8%.
  PhiColor separator;       // ink 6%.
  PhiColor window_bg;       // windowBackground — the top bar's own ground.
  PhiColor accent;

  // The roles individual files invented for themselves, brought into the one
  // token set (lane 7 §2.2 counted 143 hard-coded RGB() outside this file).
  // Each is the value those files were already using, so adopting a token is
  // not a redesign — it is a deletion.

  // The ink the percentage overlays above are made of: black in light,
  // white in dark. Nine files derive their own washes from a literal
  // RGB(0,0,0) / RGB(0xFF,0xFF,0xFF); this is that constant, once.
  PhiColor ink;
  // The FIXED window ground — #EAEAEA / #252525, the Mac's
  // DefaultColors.windowBackground. Secondary windows (Settings, Feedback,
  // PhiAlert, the transcript) stand on this and never on the per-Space tint,
  // which is why phi_settings_window.cc:2441 defines a parallel palette
  // today and phi_popup_chrome.h:26 a second struct.
  PhiColor ground_fixed;
  // What is legible ON `accent`: the label of an accent-filled button, the
  // glyph in an accent pill. Files pick a literal white today, which is
  // wrong for the light Amber accent.
  PhiColor on_accent;
  // The accent under the cursor — PhiAlertButton's .primary hover.
  PhiColor accent_hover;
  // systemRed and its hover: the close-button wash (two files carry
  // RGB(0xC4,0x2B,0x1C)), a Delete action, a failed download.
  PhiColor destructive;
  PhiColor destructive_hover;
  // A counter near its limit, and a hard failure — the feedback form's two.
  PhiColor warning;
  PhiColor error;
  // The dim laid over the content behind a modal, the group overview and
  // the tab switcher. Opaque here for the same reason everything else is:
  // GDI does not composite. Blend it yourself when the surface is layered.
  PhiColor overlay_scrim;

  // The Windows-native design's roles (docs/windows-native-ui.md §2.3).
  // Flyouts (menus, the Space switcher, the hover card): #F9F9F9 / #2C2C2C.
  PhiColor flyout;
  // A dialog's footer band under its body: #F3F3F3 / #202020.
  PhiColor dialog_footer;
  // The stroke a control needs to read on a card: ink 9 % / white 16 %.
  PhiColor stroke_strong;
  // A toggled-on button's fill: the accent at 14 % over the ground.
  PhiColor accent_soft;
  // A control's resting fill on a card (the omnibox at rest, a combo):
  // ink 4.5 % / white 6 %.
  PhiColor control_fill;
  // The pressed wash: ink 9 % / white 10 %.
  PhiColor pressed;

  // The sidebar ground's stops, top to bottom: a two- or three-colour
  // Space palette's colours, each at the fixed overlay alpha over the
  // window background (stop 0 is ground_top). The sidebar is still PAINTED
  // on the flat ground_top and every wash above is pre-blended over it; the
  // shell regrades its pixels row by row onto the gradient after painting
  // (PhiPaletteGroundAt), so the washes keep their coverage over the
  // secondary colours. One stop is a flat ground; unused stops repeat the
  // last, so a ramp can lerp them element-wise.
  std::array<PhiColor, 3> ground_stops = {};
  int ground_stop_count = 1;

  bool dark = false;
};

// The sidebar ground `position` (0 top ... 1 bottom) of the way down:
// ground_top for a flat palette, the Mac's Oklab palette gradient otherwise.
PhiColor PhiPaletteGroundAt(const PhiPalette& palette, double position);

// The design-unit table: the metrics that recur ACROSS surfaces, in Mac
// design points at 96 dpi. Scale exactly one way, with PhiDip(dpi, design)
// from phi_util.h — never MulDiv by hand, never a second `Scaled()` member.
//
// Surface-specific metrics stay with their surface (sidebar_metrics in
// phi_sidebar_view.h is the model: named, commented against the Mac view
// they came from, and in ONE place). What belongs here is what two or more
// unrelated files would otherwise both invent.
struct PhiMetrics {
  // Corner radii. The client draws every one of these through phi_dwrite's
  // antialiased path — never GDI's RoundRect.
  int corner_chip = 6;    // Hover washes behind an icon button.
  int corner = 8;         // THE pill radius: rows, buttons, fields.
  int corner_card = 12;   // Section cards, popover bodies.
  int corner_window = 14; // A layered window's own corners.

  // Rhythm.
  int pad_tight = 4;
  int pad = 8;
  int pad_card = 12;
  int pad_window = 16;

  // Rows and controls.
  int row_height = 32;     // A sidebar row's pill, a menu row.
  int row_gap = 4;
  int control_height = 28; // A field, a segmented control.
  int hairline = 1;        // A separator, a border. Scale it too: a 1px
                           // hairline at 200% is a half-weight line.

  // PhiAlert's actions (phi_prompt.cc's 130x36 buttons).
  int button_width = 130;
  int button_height = 36;

  // Icons.
  int icon = 16;
  int icon_large = 20;

  // Type sizes, design points — the argument PhiPopupFont takes.
  int text_small = 12;
  int text_body = 13;
  int text_title = 18;
};
inline constexpr PhiMetrics kPhiMetrics = {};

// Resolves `theme` for `appearance`; kSystem asks Windows which it is.
// `saturation` is the Mac's slider position, 0..100: how much of the theme's
// overlay colour reaches the window (OverlaySaturationScale).
PhiPalette PhiResolvePalette(const PhiTheme& theme,
                             PhiAppearance appearance,
                             int saturation = 10);
// Resolves a Space's linked palette (the Mac's ThemePalette.applying(to:)):
// the record's anchor colour for the appearance, composited at the Mac's
// fixed 0.8 overlay alpha over `base`'s window background, is the ground;
// the anchor hue's accent (S .65 / B .60 light, S .50 / B .85 dark) is the
// accent; every wash and label re-derives over them. `base` is the Space's
// theme pin (its background shows through the overlay); the palette beats
// its overlay colour and the saturation slider alike.
//
// A two- or three-colour palette paints the sidebar as the Mac's vertical
// Oklab gradient: ground_stops carries the colours, and the shell regrades
// the sidebar's pixels onto it after painting (see PhiPalette). Every other
// surface — the top bar, washes, accent — takes the PRIMARY colour.
PhiPalette PhiResolvePalette(const PhiTheme& base,
                             PhiAppearance appearance,
                             const PhiThemePalette& palette);
// The chrome around a page that reported its own colour (the Mac's
// WebContentHeaderPageColorPresentation): `base` with `page` as the ground of
// every surface the palette paints on, and the ink the page's colour calls for
// -- light when white contrasts with it more than black does, the Mac's WCAG
// rule -- with every wash, label and hairline re-derived over it. The accent,
// the flyouts and the dialog roles stay `base`'s: they are not on the page.
PhiPalette PhiPageBarPalette(const PhiPalette& base, PhiColor page);
// WCAG 2 contrast ratio between two opaque colours, 1..21.
double PhiContrastRatio(PhiColor a, PhiColor b);
// True when Windows is currently set to a dark app appearance.
bool PhiSystemPrefersDark();

// Windows contrast themes (Settings > Accessibility > Contrast themes). While
// one is on, every palette this file resolves is the SYSTEM's — the user
// chose those colours for legibility, and a tinted theme over them defeats
// the point — so PhiResolvePalette answers with PhiHighContrastPalette()
// and PhiPageBarPalette leaves the page colour out. False on the host.
bool PhiHighContrastActive();

// The system colours a contrast theme sets, as RGB.
struct PhiSystemColors {
  PhiColor window = PhiRgb(0xFF, 0xFA, 0xEF);          // COLOR_WINDOW
  PhiColor window_text = PhiRgb(0x3D, 0x3D, 0x3D);     // COLOR_WINDOWTEXT
  PhiColor highlight = PhiRgb(0x90, 0x39, 0x09);       // COLOR_HIGHLIGHT
  PhiColor highlight_text = PhiRgb(0xFF, 0xF5, 0xE3);  // COLOR_HIGHLIGHTTEXT
  PhiColor hot_light = PhiRgb(0x1C, 0x5E, 0x75);       // COLOR_HOTLIGHT
  PhiColor gray_text = PhiRgb(0x67, 0x67, 0x67);       // COLOR_GRAYTEXT
  PhiColor button_face = PhiRgb(0xFF, 0xFA, 0xEF);     // COLOR_BTNFACE
};

// The palette roles mapped onto `colors`: grounds and cards are the window,
// ink is the window text, washes are the button face, borders are drawn in
// the text colour so they stay visible, the accent is the selection
// highlight (with its own text on top). Pure, so the host can test it.
PhiPalette PhiPaletteFromSystemColors(const PhiSystemColors& colors);

// The live contrast theme's palette, from GetSysColor. The host answers
// with PhiSystemColors' defaults (the "Desert" light scheme).
PhiPalette PhiHighContrastPalette();

// Parses the wire's "#RRGGBB" (case-insensitive; the leading '#' optional)
// into a COLORREF. False on anything else — the caller keeps its default.
bool PhiParseColorHex(const std::string& hex, PhiColor* out);

// The built-in theme a free colour snaps to (the Mac's
// BrowserMigrationSpaceTheme): the nearest light overlay by hue, the way an
// imported or agent-given Space colour becomes one of the eight themes. A
// colour too close to neutral or too dark to carry a hue takes the default
// (Pure), which is never a hue target itself.
std::string PhiNearestThemeId(PhiColor color);
// "#RRGGBB" of `theme_id`'s light overlay — the colour a Space pinned to that
// theme stores as its colorHex (an unknown id reads as Pure's).
std::string PhiThemeOverlayHex(const std::string& theme_id);
// The built-in theme `raw` names by id or by display name, any case; empty
// when it names none.
std::string PhiThemeIdForName(const std::string& raw);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_THEME_H_

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_popup_chrome.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_fluent.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_secondary_window.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace phi_client {

namespace {

// The band and the Win11 caption-button metric, in design units. The
// geometry itself lives in phi_secondary_window.h, where the host tests
// can reach it; these are the defaults it is fed.
constexpr int kCaptionHeight = kPhiCaptionHeightDip;
constexpr int kCaptionButtonWidth = kPhiCaptionButtonWidthDip;
constexpr int kMarkSize = 16;
constexpr int kMarkInset = 12;
// PhiAlertButton: radius 8, 13pt label; disabled at 0.55, pressed at 0.78.
constexpr int kButtonRadius = 4;  // Fluent controls (§2.1).
constexpr int kWellRadius = 8;
// Segoe MDL2 Assets — the OS caption buttons' own glyphs.
constexpr wchar_t kGlyphMinimize[] = L"\uE921";
constexpr wchar_t kGlyphMaximize[] = L"\uE922";
constexpr wchar_t kGlyphRestore[] = L"\uE923";
constexpr wchar_t kGlyphClose[] = L"\uE8BB";
constexpr COLORREF kCloseHot = RGB(0xC4, 0x2B, 0x1C);

// Newer DWM attributes than the SDK headers we build against may declare.
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr DWORD kDwmBorderColor = 34;
constexpr DWORD kDwmCornerRound = 2;

const PhiPalette* g_live_palette = nullptr;

// The owner-draw button's role and hover state, kept on a subclass so the
// parent's WM_DRAWITEM can read them back.
struct ButtonState {
  PhiPopupButtonRole role = PhiPopupButtonRole::kSecondary;
  bool hot = false;
};

constexpr UINT_PTR kButtonSubclassId = 0x5049;  // 'PI'

LRESULT CALLBACK ButtonProc(HWND hwnd,
                            UINT msg,
                            WPARAM wparam,
                            LPARAM lparam,
                            UINT_PTR id,
                            DWORD_PTR data) {
  auto* state = reinterpret_cast<ButtonState*>(data);
  switch (msg) {
    case WM_NCDESTROY:
      ::RemoveWindowSubclass(hwnd, &ButtonProc, id);
      delete state;
      break;
    case WM_MOUSEMOVE:
      if (state && !state->hot) {
        state->hot = true;
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&track);
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      break;
    case WM_MOUSELEAVE:
      if (state && state->hot) {
        state->hot = false;
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      break;
    case WM_ERASEBKGND:
      return 1;  // The owner-draw paints every pixel.
    case WM_SETCURSOR:
      if (state && state->role == PhiPopupButtonRole::kLink) {
        ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    default:
      break;
  }
  return ::DefSubclassProc(hwnd, msg, wparam, lparam);
}

void SetFace(LOGFONTW* lf, const wchar_t* face) {
  const std::wstring name(face);
  std::fill(std::begin(lf->lfFaceName), std::end(lf->lfFaceName), L'\0');
  std::copy_n(name.begin(),
              std::min(name.size(), std::size(lf->lfFaceName) - 1),
              std::begin(lf->lfFaceName));
}

// The Win32 spelling of the pure caption parts.
int HitCodeFor(PhiCaptionPart part) {
  switch (part) {
    case PhiCaptionPart::kMinimize:
      return HTMINBUTTON;
    case PhiCaptionPart::kMaximize:
      return HTMAXBUTTON;
    case PhiCaptionPart::kClose:
      return HTCLOSE;
    case PhiCaptionPart::kCaption:
      return HTCAPTION;
    case PhiCaptionPart::kOutside:
      break;
  }
  return HTCLIENT;
}

// This window's band, for the pure geometry in phi_secondary_window.h.
// A free function so phi_popup_chrome.h need not include that header
// (which includes this one back).
PhiCaptionMetrics BandMetrics(HWND hwnd,
                              int height_px,
                              int button_px,
                              unsigned buttons) {
  RECT client = {};
  if (hwnd) {
    ::GetClientRect(hwnd, &client);
  }
  PhiCaptionMetrics metrics;
  metrics.width = static_cast<int>(client.right);
  metrics.height = height_px;
  metrics.button_width = button_px;
  metrics.close = (buttons & kPhiCaptionClose) != 0;
  metrics.maximize = (buttons & kPhiCaptionMaximize) != 0;
  metrics.minimize = (buttons & kPhiCaptionMinimize) != 0;
  return metrics;
}

PhiCaptionPart PartFor(int hit_code) {
  switch (hit_code) {
    case HTMINBUTTON:
      return PhiCaptionPart::kMinimize;
    case HTMAXBUTTON:
      return PhiCaptionPart::kMaximize;
    case HTCLOSE:
      return PhiCaptionPart::kClose;
    case HTCAPTION:
      return PhiCaptionPart::kCaption;
    default:
      break;
  }
  return PhiCaptionPart::kOutside;
}

int Border(UINT dpi, bool vertical) {
  return ::GetSystemMetricsForDpi(vertical ? SM_CYSIZEFRAME : SM_CXSIZEFRAME,
                                  dpi) +
         ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

}  // namespace

PhiPopupColors PhiPopupColorsFor(bool dark, COLORREF accent) {
  // The Windows-native ladder (docs/windows-native-ui.md §2.3): the
  // Windows Settings ground, white / #2B2B2B cards, the ink percentages,
  // Fluent's standard button (#FBFBFB, 1 px stroke) and the danger red.
  PhiPopupColors c;
  c.dark = dark;
  c.bg = dark ? RGB(0x20, 0x20, 0x20) : RGB(0xF3, 0xF3, 0xF3);
  c.ink = dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0);
  c.label = PhiPopupBlend(c.ink, c.bg, dark ? 230 : 222);       // 90 / 87 %
  c.label_strong = c.ink;
  c.secondary = PhiPopupBlend(c.ink, c.bg, 158);               // 62 %
  c.tertiary = PhiPopupBlend(c.ink, c.bg, 107);                // 42 %
  c.card = dark ? RGB(0x2B, 0x2B, 0x2B) : RGB(0xFF, 0xFF, 0xFF);
  c.card_border = PhiPopupBlend(c.ink, c.bg, dark ? 20 : 23);  // 8 / 9 %
  c.separator = PhiPopupBlend(c.ink, c.bg, 18);                // 7 %
  c.well = PhiPopupBlend(c.ink, c.bg, dark ? 15 : 11);         // 6 / 4.5 %
  c.hover = PhiPopupBlend(c.ink, c.bg, dark ? 15 : 11);
  c.accent = accent;
  c.accent_hover = dark ? PhiPopupBlend(RGB(0xFF, 0xFF, 0xFF), accent, 26)
                        : PhiPopupBlend(RGB(0, 0, 0), accent, 26);
  c.button = dark ? RGB(0x33, 0x33, 0x33) : RGB(0xFB, 0xFB, 0xFB);
  c.button_hover = dark ? RGB(0x3A, 0x3A, 0x3A) : RGB(0xF6, 0xF6, 0xF6);
  c.destructive = dark ? RGB(0xE0, 0x46, 0x3A) : RGB(0xC4, 0x2B, 0x1C);
  c.destructive_hover = PhiPopupBlend(c.bg, c.destructive, 46);
  c.warning = dark ? RGB(0xFF, 0xD6, 0x0A) : RGB(0xB0, 0x80, 0x00);
  c.error = dark ? RGB(0xFF, 0x6B, 0x6B) : RGB(0xC0, 0x1E, 0x1E);
  return c;
}

PhiPopupColors PhiPopupColorsNow() {
  if (g_live_palette) {
    return PhiPopupColorsFor(g_live_palette->dark, g_live_palette->accent);
  }
  const bool dark = PhiSystemPrefersDark();
  return PhiPopupColorsFor(dark, dark ? RGB(0x1E, 0x70, 0x99)
                                      : RGB(0x3A, 0xA4, 0xD5));
}

void PhiSetPopupPalette(const PhiPalette* live) {
  g_live_palette = live;
}

COLORREF PhiHsbColor(double hue, double saturation, double brightness) {
  const double h = std::fmod(std::max(0.0, hue), 1.0) * 6.0;
  const int i = static_cast<int>(std::floor(h));
  const double f = h - i;
  const double s = saturation, b = brightness;
  const double p = b * (1 - s), q = b * (1 - s * f), t = b * (1 - s * (1 - f));
  double r = 0, g = 0, bl = 0;
  switch (i % 6) {
    case 0: r = b; g = t; bl = p; break;
    case 1: r = q; g = b; bl = p; break;
    case 2: r = p; g = b; bl = t; break;
    case 3: r = p; g = q; bl = b; break;
    case 4: r = t; g = p; bl = b; break;
    default: r = b; g = p; bl = q; break;
  }
  return RGB(static_cast<int>(std::lround(r * 255)),
             static_cast<int>(std::lround(g * 255)),
             static_cast<int>(std::lround(bl * 255)));
}

double PhiHueOf(COLORREF color) {
  const double r = GetRValue(color) / 255.0, g = GetGValue(color) / 255.0,
               b = GetBValue(color) / 255.0;
  const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
  const double d = mx - mn;
  if (d <= 0.0) {
    return 0.0;
  }
  double h = mx == r ? std::fmod((g - b) / d, 6.0)
             : mx == g ? (b - r) / d + 2.0
                       : (r - g) / d + 4.0;
  h /= 6.0;
  return h < 0 ? h + 1.0 : h;
}

COLORREF PhiPopupBlend(COLORREF over, COLORREF base, int alpha) {
  auto mix = [&](int a, int b) { return (a * alpha + b * (255 - alpha)) / 255; };
  return RGB(mix(GetRValue(over), GetRValue(base)),
             mix(GetGValue(over), GetGValue(base)),
             mix(GetBValue(over), GetBValue(base)));
}

void PhiPopupFillRounded(HDC dc, const RECT& r, COLORREF fill, int radius) {
  PhiFillRoundRectAA(dc, r, radius, fill);
}

void PhiPopupStrokeRounded(HDC dc,
                           const RECT& r,
                           COLORREF fill,
                           COLORREF border,
                           int radius,
                           int width) {
  PhiFillRoundRectAA(dc, r, radius, fill);
  PhiStrokeRoundRectAA(dc, r, radius, border, width);
}

HFONT PhiPopupFont(UINT dpi, int size, int weight) {
  // The §2.2 ramp: Segoe UI Variable (Segoe UI on Windows 10).
  return PhiFluentFont(dpi, size, weight);
}

HFONT PhiPopupGlyphFont(UINT dpi) {
  LOGFONTW lf = {};
  SetFace(&lf, PhiIconFaceName());
  lf.lfHeight = -::MulDiv(10, static_cast<int>(dpi), 96);
  lf.lfWeight = FW_NORMAL;
  lf.lfCharSet = DEFAULT_CHARSET;
  lf.lfQuality = CLEARTYPE_QUALITY;
  return ::CreateFontIndirectW(&lf);
}

void PhiPopupStyleButton(HWND button, PhiPopupButtonRole role) {
  if (!button) {
    return;
  }
  auto* state = new ButtonState;
  state->role = role;
  if (!::SetWindowSubclass(button, &ButtonProc, kButtonSubclassId,
                           reinterpret_cast<DWORD_PTR>(state))) {
    delete state;
    return;
  }
  // BS_OWNERDRAW is a button TYPE — it replaces push / default-push.
  LONG_PTR style = ::GetWindowLongPtrW(button, GWL_STYLE);
  style = (style & ~static_cast<LONG_PTR>(BS_TYPEMASK)) | BS_OWNERDRAW;
  ::SetWindowLongPtrW(button, GWL_STYLE, style);
}

void PhiPopupPaintButton(HDC dc,
                         const RECT& bounds,
                         const std::wstring& text,
                         PhiPopupButtonRole role,
                         bool hot,
                         bool pressed,
                         bool enabled,
                         const PhiPopupColors& colors,
                         HFONT font,
                         UINT dpi) {
  const int radius = ::MulDiv(kButtonRadius, static_cast<int>(dpi), 96);
  COLORREF fill = colors.button;
  COLORREF ink = colors.label;
  switch (role) {
    case PhiPopupButtonRole::kPrimary:
      fill = hot ? colors.accent_hover : colors.accent;
      ink = RGB(0xFF, 0xFF, 0xFF);
      break;
    case PhiPopupButtonRole::kSecondary:
      fill = hot ? colors.button_hover : colors.button;
      break;
    case PhiPopupButtonRole::kDestructive:
      fill = hot ? colors.destructive_hover : colors.destructive;
      ink = RGB(0xFF, 0xFF, 0xFF);
      break;
    case PhiPopupButtonRole::kLink:
      fill = colors.bg;
      ink = colors.accent;
      break;
  }
  if (!enabled) {
    // The Mac dims the whole button to 0.55.
    fill = PhiPopupBlend(fill, colors.bg, 140);
    ink = PhiPopupBlend(ink, colors.bg, 140);
  } else if (pressed) {
    fill = PhiPopupBlend(fill, colors.bg, 199);  // 0.78
  }
  HBRUSH ground = ::CreateSolidBrush(colors.bg);
  ::FillRect(dc, &bounds, ground);
  ::DeleteObject(ground);
  if (role == PhiPopupButtonRole::kSecondary) {
    // Fluent's standard button: the fill with a 1 px stroke.
    PhiPopupStrokeRounded(dc, bounds, fill, colors.card_border, radius);
  } else if (role != PhiPopupButtonRole::kLink) {
    PhiPopupFillRounded(dc, bounds, fill, radius);
  }
  ::SetBkMode(dc, TRANSPARENT);
  ::SetTextColor(dc, ink);
  HGDIOBJ previous = font ? ::SelectObject(dc, font) : nullptr;
  RECT text_rc = bounds;
  PhiDrawTextAA(dc, text.c_str(), -1, &text_rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  if (role == PhiPopupButtonRole::kLink && hot && enabled) {
    // The Mac's inline link: underlined while hovered.
    RECT measure = bounds;
    PhiDrawTextAA(dc, text.c_str(), -1, &measure,
                DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    const int w = measure.right - measure.left;
    const int x = (bounds.left + bounds.right - w) / 2;
    const int y = (bounds.top + bounds.bottom + (measure.bottom - measure.top)) /
                      2 -
                  1;
    RECT line = {x, y, x + w, y + 1};
    HBRUSH under = ::CreateSolidBrush(ink);
    ::FillRect(dc, &line, under);
    ::DeleteObject(under);
  }
  if (previous) {
    ::SelectObject(dc, previous);
  }
}

bool PhiPopupDrawButton(const DRAWITEMSTRUCT& item,
                        const PhiPopupColors& colors,
                        HFONT font) {
  if (item.CtlType != ODT_BUTTON) {
    return false;
  }
  DWORD_PTR data = 0;
  ButtonState* state =
      ::GetWindowSubclass(item.hwndItem, &ButtonProc, kButtonSubclassId,
                          &data)
          ? reinterpret_cast<ButtonState*>(data)
          : nullptr;
  const PhiPopupButtonRole role =
      state ? state->role : PhiPopupButtonRole::kSecondary;
  wchar_t text[256] = {};
  ::GetWindowTextW(item.hwndItem, text, 255);
  HFONT face = font ? font
                    : reinterpret_cast<HFONT>(
                          ::SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
  PhiPopupPaintButton(item.hDC, item.rcItem, text, role, state && state->hot,
                      (item.itemState & ODS_SELECTED) != 0,
                      (item.itemState & ODS_DISABLED) == 0, colors, face,
                      ::GetDpiForWindow(item.hwndItem));
  return true;
}

void PhiPopupPaintWell(HDC dc,
                       const RECT& bounds,
                       const PhiPopupColors& c,
                       UINT dpi) {
  const int radius = ::MulDiv(kWellRadius, static_cast<int>(dpi), 96);
  PhiPopupStrokeRounded(dc, bounds, c.card, c.card_border, radius, 1);
}

void PhiPopupThemeControl(HWND control, bool dark) {
  if (!control) {
    return;
  }
  ::SetWindowTheme(control, dark ? L"DarkMode_Explorer" : L"Explorer",
                   nullptr);
}

PhiPopupChrome::PhiPopupChrome() = default;

PhiPopupChrome::~PhiPopupChrome() {
  for (HFONT font : {title_font_, glyph_font_}) {
    if (font) {
      ::DeleteObject(font);
    }
  }
  for (HBRUSH brush : {ground_brush_, hot_brush_}) {
    if (brush) {
      ::DeleteObject(brush);
    }
  }
}

void PhiPopupChrome::SetCaptionHeight(int design_units) {
  caption_height_ = design_units > 0 ? design_units : kCaptionHeight;
}

void PhiPopupChrome::SetDimWhenInactive(bool dim) {
  dim_when_inactive_ = dim;
}

void PhiPopupChrome::SetClientProbe(ClientProbe probe, void* context) {
  client_probe_ = probe;
  client_probe_context_ = context;
}

HBRUSH PhiPopupChrome::GroundBrush(COLORREF color) {
  if (!ground_brush_ || ground_color_ != color) {
    if (ground_brush_) {
      ::DeleteObject(ground_brush_);
    }
    ground_brush_ = ::CreateSolidBrush(color);
    ground_color_ = color;
  }
  return ground_brush_;
}

HBRUSH PhiPopupChrome::HotBrush(COLORREF color) {
  if (!hot_brush_ || hot_color_ != color) {
    if (hot_brush_) {
      ::DeleteObject(hot_brush_);
    }
    hot_brush_ = ::CreateSolidBrush(color);
    hot_color_ = color;
  }
  return hot_brush_;
}

void PhiPopupChrome::Attach(HWND hwnd,
                            unsigned buttons,
                            const PhiPopupColors& colors) {
  hwnd_ = hwnd;
  buttons_ = buttons;
  if (const UINT dpi = ::GetDpiForWindow(hwnd)) {
    dpi_ = dpi;
  }
  EnsureFonts();
  ApplyColors(colors);
  if (HICON big = PhiAppIcon(32)) {
    ::SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
  }
  if (HICON small_icon = PhiAppIcon(16)) {
    ::SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
                   reinterpret_cast<LPARAM>(small_icon));
  }
  // Re-run WM_NCCALCSIZE so the caption is ours from the first paint.
  ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
}

void PhiPopupChrome::ApplyColors(const PhiPopupColors& colors) {
  if (!hwnd_) {
    return;
  }
  const BOOL dark = colors.dark ? TRUE : FALSE;
  ::DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &dark,
                          sizeof(dark));
  const DWORD corner = kDwmCornerRound;
  ::DwmSetWindowAttribute(hwnd_, kDwmWindowCornerPreference, &corner,
                          sizeof(corner));
  // The 1px DWM edge takes the ground's own separator, not the OS accent.
  const COLORREF edge = PhiPopupBlend(colors.ink, colors.bg, 46);
  ::DwmSetWindowAttribute(hwnd_, kDwmBorderColor, &edge, sizeof(edge));
}

int PhiPopupChrome::Scaled(int design) const {
  return ::MulDiv(design, static_cast<int>(dpi_), 96);
}

int PhiPopupChrome::CaptionHeight() const {
  return Scaled(caption_height_);
}

SIZE PhiPopupChrome::OuterSize(int content_w, int content_h) const {
  SIZE size = {Scaled(content_w) + 2 * Border(dpi_, false),
               Scaled(content_h + caption_height_) + Border(dpi_, true)};
  return size;
}

RECT PhiPopupChrome::ContentRect() const {
  RECT client = {};
  if (hwnd_) {
    ::GetClientRect(hwnd_, &client);
  }
  client.top = std::min<LONG>(client.top + CaptionHeight(), client.bottom);
  return client;
}

void PhiPopupChrome::EnsureFonts() {
  if (font_dpi_ == dpi_ && title_font_ && glyph_font_) {
    return;
  }
  HFONT title = PhiPopupFont(dpi_, 13, FW_SEMIBOLD);
  HFONT glyph = PhiPopupGlyphFont(dpi_);
  if (!title || !glyph) {
    for (HFONT font : {title, glyph}) {
      if (font) {
        ::DeleteObject(font);
      }
    }
    return;
  }
  for (HFONT font : {title_font_, glyph_font_}) {
    if (font) {
      ::DeleteObject(font);
    }
  }
  title_font_ = title;
  glyph_font_ = glyph;
  font_dpi_ = dpi_;
}

RECT PhiPopupChrome::ButtonRect(int hit_code) const {
  return PhiToRect(
      PhiCaptionButtonBounds(BandMetrics(hwnd_, CaptionHeight(),
                                         Scaled(kCaptionButtonWidth), buttons_),
                             PartFor(hit_code)));
}

int PhiPopupChrome::HitTestCaption(POINT pt) const {
  const PhiCaptionPart part = PhiCaptionHitTest(
      BandMetrics(hwnd_, CaptionHeight(), Scaled(kCaptionButtonWidth),
                  buttons_),
      static_cast<int>(pt.x), static_cast<int>(pt.y));
  if (part == PhiCaptionPart::kCaption && client_probe_ &&
      client_probe_(client_probe_context_, pt)) {
    // The owner's own band control takes the click; without this the
    // band would drag the window out from under the pointer.
    return HTCLIENT;
  }
  return HitCodeFor(part);
}

void PhiPopupChrome::SetHot(int hit_code) {
  if (hot_ == hit_code) {
    return;
  }
  hot_ = hit_code;
  InvalidateBand();
}

void PhiPopupChrome::InvalidateBand() {
  if (!hwnd_) {
    return;
  }
  RECT client = {};
  ::GetClientRect(hwnd_, &client);
  client.bottom = std::min<LONG>(client.bottom, CaptionHeight());
  ::InvalidateRect(hwnd_, &client, FALSE);
}

bool PhiPopupChrome::HandleMessage(UINT msg,
                                   WPARAM wparam,
                                   LPARAM lparam,
                                   LRESULT* result) {
  if (!hwnd_) {
    return false;
  }
  const bool resizable =
      (::GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_THICKFRAME) != 0;
  switch (msg) {
    case WM_NCCALCSIZE: {
      // Claim the OS caption for the client (the band is drawn there);
      // only the thin side/bottom borders stay non-client. BOTH wparam
      // forms matter — the first calc at creation is the FALSE one, and
      // skipping it leaves the OS caption alive.
      RECT* rect = wparam == TRUE
                       ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0]
                       : reinterpret_cast<RECT*>(lparam);
      const UINT dpi = ::GetDpiForWindow(hwnd_);
      const int border_x = Border(dpi, false);
      const int border_y = Border(dpi, true);
      rect->left += border_x;
      rect->right -= border_x;
      rect->bottom -= border_y;
      if (::IsZoomed(hwnd_)) {
        rect->top += border_y;  // Maximized frames overhang the monitor.
      }
      *result = 0;
      return true;
    }
    case WM_NCHITTEST: {
      const LRESULT hit = ::DefWindowProcW(hwnd_, msg, wparam, lparam);
      if (hit != HTCLIENT) {
        *result = hit;  // The real borders.
        return true;
      }
      POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ::ScreenToClient(hwnd_, &pt);
      if (resizable && !::IsZoomed(hwnd_) &&
          pt.y < Border(::GetDpiForWindow(hwnd_), true)) {
        *result = HTTOP;  // The band's first pixels still resize.
        return true;
      }
      *result = HitTestCaption(pt);
      return true;
    }
    case WM_NCMOUSEMOVE: {
      const int code = static_cast<int>(wparam);
      SetHot(code == HTMINBUTTON || code == HTMAXBUTTON || code == HTCLOSE
                 ? code
                 : 0);
      TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE | TME_NONCLIENT,
                               hwnd_, 0};
      ::TrackMouseEvent(&track);
      *result = 0;
      return true;
    }
    case WM_NCMOUSELEAVE:
      SetHot(0);
      pressed_ = 0;
      *result = 0;
      return true;
    case WM_MOUSEMOVE:
      SetHot(0);  // Client moves mean we left the window controls.
      return false;
    case WM_NCLBUTTONDOWN:
      // Swallow presses on the drawn window controls; the action fires on
      // release over the same button, like the OS ones.
      if (wparam == HTMINBUTTON || wparam == HTMAXBUTTON ||
          wparam == HTCLOSE) {
        pressed_ = static_cast<int>(wparam);
        *result = 0;
        return true;
      }
      break;
    case WM_NCLBUTTONUP:
      if (pressed_) {
        const int pressed = pressed_;
        pressed_ = 0;
        if (static_cast<int>(wparam) == pressed) {
          if (pressed == HTMINBUTTON) {
            ::ShowWindow(hwnd_, SW_MINIMIZE);
          } else if (pressed == HTMAXBUTTON) {
            ::ShowWindow(hwnd_, ::IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
          } else {
            ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
          }
        }
        *result = 0;
        return true;
      }
      break;
    case WM_NCLBUTTONDBLCLK:
      if (wparam == HTCAPTION && !(buttons_ & kPhiCaptionMaximize)) {
        *result = 0;  // Nothing to zoom to.
        return true;
      }
      break;
    case WM_ACTIVATE:
    case WM_SIZE:
    case WM_SETTEXT:
      InvalidateBand();  // Title dims when inactive; buttons move on resize.
      return false;
    case WM_DPICHANGED:
      dpi_ = HIWORD(wparam);
      EnsureFonts();
      return false;
    default:
      break;
  }
  return false;
}

void PhiPopupChrome::PaintCaption(HDC dc,
                                  const PhiPopupColors& colors,
                                  bool show_mark,
                                  const wchar_t* title) {
  if (!hwnd_) {
    return;
  }
  EnsureFonts();
  RECT client = {};
  ::GetClientRect(hwnd_, &client);
  const int h = CaptionHeight();
  RECT band = {0, 0, client.right, h};
  ::FillRect(dc, &band, GroundBrush(colors.bg));
  ::SetBkMode(dc, TRANSPARENT);

  const bool active = !dim_when_inactive_ || ::GetActiveWindow() == hwnd_;
  if (show_mark) {
    const std::vector<unsigned char>& logo = PhiLogoBgra256();
    if (!logo.empty()) {
      const int mark = Scaled(kMarkSize);
      RECT mark_rc = {Scaled(kMarkInset), (h - mark) / 2,
                      Scaled(kMarkInset) + mark, (h + mark) / 2};
      PhiDrawBgra(dc, mark_rc, logo.data(), 256, 256);
    }
  }

  wchar_t text[256] = {};
  if (!title) {
    ::GetWindowTextW(hwnd_, text, 255);
    title = text;
  }
  if (*title) {
    // At the leading edge after the mark (Windows), never under the
    // button cluster.
    const int leading = show_mark ? Scaled(kMarkInset * 2 + kMarkSize)
                                  : Scaled(12);
    const int reserve = PhiCaptionTitleInset(
        BandMetrics(hwnd_, h, Scaled(kCaptionButtonWidth), buttons_), leading);
    RECT title_rc = {leading, 0, client.right - reserve, h};
    HGDIOBJ previous = title_font_ ? ::SelectObject(dc, title_font_) : nullptr;
    ::SetTextColor(dc, active ? colors.label : colors.tertiary);
    PhiDrawTextAA(dc, title, -1, &title_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                    DT_END_ELLIPSIS);
    if (previous) {
      ::SelectObject(dc, previous);
    }
  }

  PaintCaptionButtons(dc, colors);
}

void PhiPopupChrome::PaintCaptionButtons(HDC dc, const PhiPopupColors& colors) {
  if (!hwnd_) {
    return;
  }
  EnsureFonts();
  ::SetBkMode(dc, TRANSPARENT);
  // The hot fill first (close turns the Win11 red), then the OS buttons'
  // own glyphs.
  const bool active = !dim_when_inactive_ || ::GetActiveWindow() == hwnd_;
  HGDIOBJ previous = glyph_font_ ? ::SelectObject(dc, glyph_font_) : nullptr;
  const bool zoomed = ::IsZoomed(hwnd_);
  for (const auto [code, glyph] :
       {std::pair{HTMINBUTTON, kGlyphMinimize},
        std::pair{HTMAXBUTTON, zoomed ? kGlyphRestore : kGlyphMaximize},
        std::pair{HTCLOSE, kGlyphClose}}) {
    const RECT rc = ButtonRect(code);
    if (rc.right <= rc.left) {
      continue;
    }
    COLORREF ink = active ? colors.label : colors.tertiary;
    if (hot_ == code) {
      const bool close = code == HTCLOSE;
      ::FillRect(dc, &rc, HotBrush(close ? kCloseHot : colors.hover));
      ink = close ? RGB(0xFF, 0xFF, 0xFF) : colors.label;
    }
    ::SetTextColor(dc, ink);
    RECT box = rc;
    PhiDrawTextAA(dc, glyph, -1, &box,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }
  if (previous) {
    ::SelectObject(dc, previous);
  }
}

}  // namespace phi_client

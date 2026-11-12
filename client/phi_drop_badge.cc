// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_drop_badge.h"

#include <objidl.h>

#include <gdiplus.h>

#include <algorithm>
#include <cmath>

#include "client/phi_fluent.h"
#include "client/phi_gdiplus.h"
#include "client/phi_panel.h"

namespace phi_client {

namespace {

constexpr wchar_t kClassName[] = L"PhiDropBadge";
constexpr int kHeightDip = 22;
constexpr int kPadDip = 8;
constexpr int kOffsetX = 16;
constexpr int kOffsetY = 20;
constexpr int kTextDip = 11;

HWND g_badge = nullptr;
bool g_plus = false;

// Leaked on purpose: no exit-time destructor.
std::wstring& LastText() {
  static auto* const text = new std::wstring;
  return *text;
}

LRESULT CALLBACK BadgeProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_NCHITTEST) {
    return HTTRANSPARENT;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool EnsureWindow(HWND owner) {
  if (g_badge && ::IsWindow(g_badge)) {
    return true;
  }
  WNDCLASSW cls = {};
  cls.lpfnWndProc = &BadgeProc;
  cls.hInstance = ::GetModuleHandleW(nullptr);
  cls.lpszClassName = kClassName;
  ::RegisterClassW(&cls);  // Idempotent: a second call fails harmlessly.
  g_badge = ::CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT |
          WS_EX_NOACTIVATE | WS_EX_TOPMOST,
      kClassName, L"", WS_POPUP, 0, 0, 1, 1, owner, nullptr,
      ::GetModuleHandleW(nullptr), nullptr);
  return g_badge != nullptr;
}

}  // namespace

// static
void PhiDropBadge::Show(HWND owner,
                        UINT dpi,
                        POINT cursor_screen,
                        const std::wstring& text,
                        bool plus,
                        const PhiPalette& palette) {
  if (text.empty()) {
    Hide();
    return;
  }
  if (!owner || !EnsureWindow(owner)) {
    return;
  }
  const auto scaled = [dpi](int dip) {
    return ::MulDiv(dip, static_cast<int>(dpi), 96);
  };
  const POINT origin = {cursor_screen.x + scaled(kOffsetX),
                        cursor_screen.y + scaled(kOffsetY)};
  const bool same = text == LastText() && plus == g_plus;
  LastText() = text;
  g_plus = plus;
  if (same) {
    // Only the position moved: UpdateLayeredWindow with the position alone.
    ::SetWindowPos(g_badge, HWND_TOPMOST, origin.x, origin.y, 0, 0,
                   SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return;
  }
  if (!PhiEnsureGdiPlus()) {
    return;
  }
  // Measure, then compose the pill into a layer (GDI+ over the layer's
  // premultiplied bits, the split-drop overlay's way).
  const float text_px = static_cast<float>(scaled(kTextDip));
  Gdiplus::Font font(L"Segoe UI", text_px, Gdiplus::FontStyleBold,
                     Gdiplus::UnitPixel);
  Gdiplus::RectF measured;
  {
    Gdiplus::Bitmap probe(1, 1, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(&probe);
    g.MeasureString(text.c_str(), -1, &font, Gdiplus::PointF(0, 0), &measured);
  }
  const int plus_w = plus ? scaled(14) : 0;
  const int width = scaled(kPadDip) * 2 + plus_w +
                    static_cast<int>(std::ceil(measured.Width)) + 2;
  const int height = scaled(kHeightDip);
  PhiLayerSurface layer(width, height);
  if (!layer.valid()) {
    return;
  }
  {
    Gdiplus::Bitmap bitmap(width, height, width * 4, PixelFormat32bppPARGB,
                           static_cast<BYTE*>(layer.bits()));
    Gdiplus::Graphics graphics(&bitmap);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
    const auto color = [](COLORREF c, BYTE alpha) {
      return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
    };
    const float r = height / 2.0f;
    const float w = static_cast<float>(width) - 1.0f;
    const float h = static_cast<float>(height) - 1.0f;
    Gdiplus::GraphicsPath pill;
    pill.AddArc(0.5f, 0.5f, r * 2, r * 2, 90, 180);
    pill.AddArc(w - r * 2, 0.5f, r * 2, r * 2, 270, 180);
    pill.CloseFigure();
    Gdiplus::SolidBrush fill(color(palette.flyout, 255));
    graphics.FillPath(&fill, &pill);
    Gdiplus::Pen stroke(color(palette.stroke_strong, 255), 1.0f);
    graphics.DrawPath(&stroke, &pill);
    (void)h;
    float x = static_cast<float>(scaled(kPadDip));
    Gdiplus::SolidBrush ink(color(palette.label, 255));
    if (plus) {
      Gdiplus::Font plus_font(L"Segoe UI", text_px + 2, Gdiplus::FontStyleBold,
                              Gdiplus::UnitPixel);
      Gdiplus::RectF plus_box(x, 0, static_cast<float>(plus_w),
                              static_cast<float>(height));
      Gdiplus::StringFormat centred;
      centred.SetAlignment(Gdiplus::StringAlignmentCenter);
      centred.SetLineAlignment(Gdiplus::StringAlignmentCenter);
      graphics.DrawString(L"+", -1, &plus_font, plus_box, &centred, &ink);
      x += plus_w;
    }
    Gdiplus::RectF box(x, 0, static_cast<float>(width) - x,
                       static_cast<float>(height));
    Gdiplus::StringFormat format;
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    graphics.DrawString(text.c_str(), -1, &font, box, &format, &ink);
  }
  layer.Commit(g_badge, &origin);
  ::SetWindowPos(g_badge, HWND_TOPMOST, origin.x, origin.y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// static
void PhiDropBadge::Hide() {
  LastText().clear();
  if (g_badge && ::IsWindow(g_badge)) {
    ::ShowWindow(g_badge, SW_HIDE);
  }
}

}  // namespace phi_client

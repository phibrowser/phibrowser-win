// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_caption_band.h"

#include <shellapi.h>
#include <windowsx.h>

#include "client/phi_dwrite.h"
#include "client/phi_fluent.h"

namespace phi_client {

namespace {

void DrawGlyph(HDC dc, const RECT& r, const wchar_t* glyph, COLORREF ink) {
  if (r.right <= r.left) {
    return;
  }
  ::SetTextColor(dc, ink);
  PhiDrawTextAA(dc, glyph, -1, const_cast<RECT*>(&r),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// An auto-hide taskbar on `edge` of `monitor` (ABE_*). It only slides out
// when the pointer reaches the screen edge over a window that does not
// cover the monitor exactly; a maximized window that does reads as
// fullscreen to the shell.
bool HasAutoHideAppBar(UINT edge, const RECT& monitor) {
  APPBARDATA bar = {sizeof(bar)};
  bar.uEdge = edge;
  bar.rc = monitor;
  return ::SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &bar) != 0;
}

}  // namespace

PhiCaptionBand::PhiCaptionBand(PhiCaptionShell* shell) : shell_(shell) {}

PhiCaptionBand::~PhiCaptionBand() = default;

void PhiCaptionBand::Install(HWND shell_hwnd) {
  shell_hwnd_ = shell_hwnd;
}

void PhiCaptionBand::InvalidateBand() {
  if (!shell_hwnd_) {
    return;
  }
  const RECT band = shell_->CaptionRects().title_bar;
  ::InvalidateRect(shell_hwnd_, &band, FALSE);
}

void PhiCaptionBand::SetCaptionHot(int code) {
  if (hot_caption_ == code) {
    return;
  }
  hot_caption_ = code;
  InvalidateBand();
}

// --- Hit-testing -----------------------------------------------------------

int PhiCaptionBand::HitTest(POINT client_pt) {
  const PhiCaptionRects rects = shell_->CaptionRects();
  if (!::PtInRect(&rects.title_bar, client_pt)) {
    return 0;
  }
  if (::PtInRect(&rects.caption_close, client_pt)) {
    return HTCLOSE;
  }
  if (::PtInRect(&rects.caption_max, client_pt)) {
    return HTMAXBUTTON;
  }
  if (::PtInRect(&rects.caption_min, client_pt)) {
    return HTMINBUTTON;
  }
  // The band's own controls take their own clicks — without this the band
  // would drag the window out from under the pointer instead.
  if (::PtInRect(&rects.back, client_pt) ||
      ::PtInRect(&rects.forward, client_pt) ||
      ::PtInRect(&rects.reload, client_pt) ||
      ::PtInRect(&rects.search_tabs, client_pt) ||
      ::PtInRect(&rects.update, client_pt) ||
      ::PtInRect(&rects.app_menu, client_pt) ||
      ::PtInRect(&rects.sidebar_toggle, client_pt) ||
      ::PtInRect(&rects.url_field, client_pt) ||
      ::PtInRect(&rects.agent_chip, client_pt) ||
      ::PtInRect(&rects.field_ext_strip, client_pt) ||
      ::PtInRect(&rects.extensions, client_pt)) {
    return HTCLIENT;
  }
  // Comfortable's band IS the tab strip, the vertical layout's row carries
  // the Space pips, and a sidebar cell's §5.8 tile may overhang into the
  // band: everything the shell draws there takes its own clicks; the bare
  // run still drags the window, which is how Chrome's strip behaves. The
  // strip is the shell's, so the shell answers for it.
  if (shell_->CaptionStripTakesPoint(client_pt)) {
    return HTCLIENT;
  }
  return HTCAPTION;  // Everything else in the band drags the window.
}

// --- The custom frame ------------------------------------------------------

bool PhiCaptionBand::OnNcCalcSize(HWND hwnd,
                                  WPARAM wparam,
                                  LPARAM lparam,
                                  LRESULT* result) {
  // Claim the caption for the client area: only the resize borders stay
  // non-client. The top edge keeps NO border — the band goes flush to it
  // (WM_NCHITTEST serves HTTOP from the band's first pixels). BOTH wparam
  // forms must be handled: the FIRST calc at creation comes with wparam
  // FALSE (a bare RECT), and skipping it leaves the OS caption alive — its
  // buttons then sit doubled above the band's.
  *result = 0;
  if (shell_->CaptionContentFullscreen()) {
    return true;  // Fullscreen: every pixel is client, no borders.
  }
  RECT* rect = wparam == TRUE
                   ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0]
                   : reinterpret_cast<RECT*>(lparam);
  const UINT dpi = ::GetDpiForWindow(hwnd);
  const int border_x = ::GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
                       ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
  const int border_y = ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
                       ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
  rect->left += border_x;
  rect->right -= border_x;
  rect->bottom -= border_y;
  if (::IsZoomed(hwnd)) {
    rect->top += border_y;  // Maximized frames overhang the monitor.
    // The client now covers the monitor exactly, which hides an auto-hide
    // taskbar for good: leave a sliver on its edge for the pointer to
    // reach it through, as Chrome and the Terminal do.
    APPBARDATA state = {sizeof(state)};
    MONITORINFO monitor = {sizeof(monitor)};
    if ((::SHAppBarMessage(ABM_GETSTATE, &state) & ABS_AUTOHIDE) &&
        ::GetMonitorInfoW(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                          &monitor)) {
      constexpr int kAutoHideSliver = 2;  // Pixels, Chrome's figure.
      const RECT& bounds = monitor.rcMonitor;
      if (HasAutoHideAppBar(ABE_TOP, bounds)) {
        rect->top += kAutoHideSliver;
      }
      if (HasAutoHideAppBar(ABE_BOTTOM, bounds)) {
        rect->bottom -= kAutoHideSliver;
      }
      if (HasAutoHideAppBar(ABE_LEFT, bounds)) {
        rect->left += kAutoHideSliver;
      }
      if (HasAutoHideAppBar(ABE_RIGHT, bounds)) {
        rect->right -= kAutoHideSliver;
      }
    }
  }
  return true;
}

namespace {

// The caption buttons that hit-test as client area (OnNcHitTest).
bool IsClientButton(int code) {
  return code == HTMINBUTTON || code == HTCLOSE;
}

}  // namespace

bool PhiCaptionBand::OnNcHitTest(HWND hwnd, LPARAM lparam, LRESULT* result) {
  const LRESULT hit = ::DefWindowProcW(hwnd, WM_NCHITTEST, 0, lparam);
  if (hit != HTCLIENT) {
    *result = hit;  // The real resize borders.
    return true;
  }
  POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
  ::ScreenToClient(hwnd, &pt);
  const UINT dpi = ::GetDpiForWindow(hwnd);
  const int border_y = ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
                       ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
  if (!::IsZoomed(hwnd) && pt.y < border_y) {
    *result = HTTOP;  // The band's first pixels still resize.
    return true;
  }
  const int band_hit = HitTest(pt);
  // Minimize and close answer HTCLIENT: for HTMINBUTTON / HTCLOSE the
  // system shows its own tooltip beside ours. Maximize keeps its code, the
  // snap layouts flyout needs it.
  *result = band_hit && !IsClientButton(band_hit) ? band_hit : HTCLIENT;
  return true;
}

namespace {

void FireCaptionButton(HWND hwnd, int pressed) {
  if (pressed == HTMINBUTTON) {
    ::ShowWindow(hwnd, SW_MINIMIZE);
  } else if (pressed == HTMAXBUTTON) {
    ::ShowWindow(hwnd, ::IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
  } else if (pressed == HTCLOSE) {
    ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
  }
}

}  // namespace

bool PhiCaptionBand::OnNcLButtonDown(WPARAM hit) {
  // Swallow presses on our drawn caption buttons; the action fires on
  // release over the same button, like the OS ones.
  if (hit != HTMINBUTTON && hit != HTMAXBUTTON && hit != HTCLOSE) {
    pressed_caption_ = 0;  // Any other press disarms a stale one.
    return false;
  }
  pressed_caption_ = static_cast<int>(hit);
  if (shell_hwnd_) {
    ::SetCapture(shell_hwnd_);  // The release comes back as WM_LBUTTONUP.
  }
  return true;
}

bool PhiCaptionBand::OnLButtonDown(POINT client_pt) {
  const int hit = HitTest(client_pt);
  if (!IsClientButton(hit)) {
    return false;
  }
  pressed_caption_ = hit;
  if (shell_hwnd_) {
    ::SetCapture(shell_hwnd_);
  }
  return true;
}

bool PhiCaptionBand::OnNcLButtonUp(HWND hwnd, WPARAM hit) {
  // Without the capture (it could not be taken) the release still arrives
  // non-client when it lands on the band.
  const int pressed = pressed_caption_;
  pressed_caption_ = 0;
  if (pressed && static_cast<int>(hit) == pressed) {
    FireCaptionButton(hwnd, pressed);
    return true;
  }
  return false;
}

bool PhiCaptionBand::OnLButtonUp(POINT client_pt) {
  const int pressed = pressed_caption_;
  if (!pressed) {
    return false;
  }
  pressed_caption_ = 0;
  if (shell_hwnd_ && ::GetCapture() == shell_hwnd_) {
    ::ReleaseCapture();
  }
  // Released over the button it was pressed on: the action. Anywhere else
  // (off the button, off the window) the press is simply over.
  if (shell_hwnd_ && HitTest(client_pt) == pressed) {
    FireCaptionButton(shell_hwnd_, pressed);
  }
  return true;
}

void PhiCaptionBand::OnCaptureLost() {
  pressed_caption_ = 0;
}

void PhiCaptionBand::OnNcMouseMove(WPARAM hit, POINT client_pt) {
  SetCaptionHot(hit == HTMINBUTTON || hit == HTMAXBUTTON || hit == HTCLOSE
                    ? static_cast<int>(hit)
                    : 0);
  const RECT band = shell_->CaptionRects().title_bar;
  if (::PtInRect(&band, client_pt)) {
    // A non-client point is drag space: nothing the band's controls washed
    // is under the pointer.
    shell_->CaptionClearShellHover();
  }
}

void PhiCaptionBand::OnNcMouseLeave() {
  // Minimize and close are client area: a move onto one may already have
  // lit it.
  if (!IsClientButton(hot_caption_)) {
    SetCaptionHot(0);
  }
}

void PhiCaptionBand::OnMouseMove(POINT client_pt) {
  // Minimize and close hover here (they are client area). Under a press's
  // capture only the pressed button stays hot, while the pointer is on it.
  const int hit = HitTest(client_pt);
  if (pressed_caption_) {
    SetCaptionHot(hit == pressed_caption_ ? pressed_caption_ : 0);
  } else {
    SetCaptionHot(IsClientButton(hit) ? hit : 0);
  }
}

void PhiCaptionBand::OnMouseLeave() {
  // Maximize is non-client: a move onto it may already have lit it.
  if (hot_caption_ != HTMAXBUTTON) {
    SetCaptionHot(0);
  }
}

// --- Paint -----------------------------------------------------------------

void PhiCaptionBand::Paint(HDC dc, bool with_buttons) {
  const PhiCaptionRects rects = shell_->CaptionRects();
  if (rects.title_bar.right <= rects.title_bar.left) {
    return;
  }
  const PhiPalette palette = shell_->CaptionPalette();
  // The seam between the app's band controls and the window's. It draws in
  // the TERTIARY ink, not the separator: a hairline that short, standing
  // alone in a gap rather than running the window's width, disappears at
  // 7%.
  if (rects.band_divider.right > rects.band_divider.left) {
    HBRUSH divider_brush = ::CreateSolidBrush(palette.label_tertiary);
    ::FillRect(dc, &rects.band_divider, divider_brush);
    ::DeleteObject(divider_brush);
  }
  if (with_buttons) {
    PaintButtons(dc);
  }
}

void PhiCaptionBand::PaintButtons(HDC dc) {
  const PhiCaptionRects rects = shell_->CaptionRects();
  if (rects.title_bar.right <= rects.title_bar.left) {
    return;
  }
  const PhiPalette palette = shell_->CaptionPalette();
  const COLORREF kLabel = palette.label;
  // Hot fills first (close turns the Windows red), then the glyphs.
  if (hot_caption_ == HTMINBUTTON || hot_caption_ == HTMAXBUTTON) {
    const RECT& hot_rc =
        hot_caption_ == HTMINBUTTON ? rects.caption_min : rects.caption_max;
    HBRUSH hover_brush = ::CreateSolidBrush(palette.hover);
    ::FillRect(dc, &hot_rc, hover_brush);
    ::DeleteObject(hover_brush);
  } else if (hot_caption_ == HTCLOSE) {
    HBRUSH close_brush = ::CreateSolidBrush(RGB(0xC4, 0x2B, 0x1C));
    ::FillRect(dc, &rects.caption_close, close_brush);
    ::DeleteObject(close_brush);
  }
  HFONT glyph_font = shell_->CaptionGlyphFont();
  HGDIOBJ previous_caption =
      glyph_font ? ::SelectObject(dc, glyph_font) : nullptr;
  const bool zoomed = shell_hwnd_ && ::IsZoomed(shell_hwnd_);
  // Comfortable's band carries the tab strip, and everything on it sits on
  // the STRIP's line rather than the band's — the caption glyphs follow, so
  // the trio reads level with search-tabs next to it. Only the glyphs move:
  // the buttons keep the full band height, which is the Win11 hit target
  // (and the hover wash) whatever the glyph inside is doing.
  const int glyph_drop =
      rects.mode == PhiLayoutMode::kComfortable
          ? (rects.tabs_top + rects.title_bar.bottom) / 2 -
                (rects.title_bar.top + rects.title_bar.bottom) / 2
          : 0;
  const auto glyph_rc = [&](const RECT& rc) {
    return RECT{rc.left, rc.top + glyph_drop, rc.right,
                rc.bottom + glyph_drop};
  };
  DrawGlyph(dc, glyph_rc(rects.caption_min), fluent::kMinimize, kLabel);
  DrawGlyph(dc, glyph_rc(rects.caption_max),
            zoomed ? fluent::kRestore : fluent::kMaximize, kLabel);
  DrawGlyph(dc, glyph_rc(rects.caption_close), fluent::kClose,
            hot_caption_ == HTCLOSE ? RGB(0xFF, 0xFF, 0xFF) : kLabel);
  if (previous_caption) {
    ::SelectObject(dc, previous_caption);
  }
}

}  // namespace phi_client

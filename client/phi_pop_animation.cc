// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_pop_animation.h"

#include <dwmapi.h>

#include <algorithm>
#include <map>

#include "client/phi_card_flight.h"
#include "client/phi_menu_paint.h"
#include "client/phi_modal_scope.h"
#include "client/phi_popup_chrome.h"

#pragma comment(lib, "dwmapi.lib")

namespace phi_client {

namespace {

constexpr UINT kFrameMs = 8;
constexpr int kOpenMs = 220;
constexpr int kCloseMs = 170;
// If the window's picture cannot be taken, it fades in instead.
constexpr ULONGLONG kFadeMs = 160;
// A window that has not painted within this long is shown as it is.
constexpr ULONGLONG kPaintWaitMs = 250;
constexpr wchar_t kMenuWindowClass[] = L"#32768";

struct PopIn {
  POINT source = {};
  ULONGLONG visible_since = 0;  // 0 until the window is first seen visible.
  int ticks_visible = 0;
  int flight = 0;               // The flight carrying its picture, if any.
  ULONGLONG fade_start = 0;     // The fallback fade, once it has begun.
};

// UI thread only, like everything that shows a window here.
std::map<HWND, PopIn>& Arriving() {
  static std::map<HWND, PopIn> arriving;
  return arriving;
}
// Where each popped-in window came from, for its way back.
std::map<HWND, POINT>& Sources() {
  static std::map<HWND, POINT> sources;
  return sources;
}

UINT_PTR g_timer = 0;
HHOOK g_menu_hook = nullptr;
int g_menu_depth = 0;
// The fit and keyboard hooks of the outermost PhiTrackPopupMenu (PHI-2095),
// and the monitor its menus open on.
HHOOK g_menu_fit_hook = nullptr;
HHOOK g_menu_key_hook = nullptr;
HMONITOR g_menu_monitor = nullptr;

int DipToPx(HWND hwnd, int dip) {
  const int dpi = static_cast<int>(::GetDpiForWindow(hwnd));
  return ::MulDiv(dip, dpi ? dpi : 96, 96);
}

bool IsTopmost(HWND hwnd) {
  return (::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

// The corner the window itself lands with on Windows 11: a menu's small
// one (4) or a popover's (8). Windows 10 refuses the read and gets 8.
int CornerRadiusDip(HWND hwnd) {
  DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DEFAULT;
  if (SUCCEEDED(::DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                                        &corner, sizeof(corner))) &&
      corner == DWMWCP_ROUNDSMALL) {
    return 4;
  }
  return 8;
}

// The picture's frame: the rounded corners Windows 11 gives a popup, and a
// soft shadow so the landing does not gain one from nowhere.
PhiCardFlight::Look LookFor(HWND hwnd, int duration_ms) {
  PhiCardFlight::Look look;
  look.radius_px = DipToPx(hwnd, CornerRadiusDip(hwnd));
  look.shadow_margin_px = DipToPx(hwnd, 20);
  look.shadow_blur_px = DipToPx(hwnd, 16);
  look.shadow_offset_y_px = DipToPx(hwnd, 4);
  look.shadow_alpha = 0.22;
  look.duration_ms = duration_ms;
  // Above its own window either way; a menu is topmost itself.
  look.topmost = true;
  return look;
}

PhiCardFlight::Anchor AnchorFor(HWND hwnd, const RECT& rect, POINT source) {
  PhiCardFlight::Anchor anchor;
  anchor.has_origin = true;
  anchor.origin = source;
  // Button-sized at the source: a sixth of the window, at least 28.
  anchor.start_width_px = std::max<int>(
      DipToPx(hwnd, 28), static_cast<int>((rect.right - rect.left) / 6));
  return anchor;
}

// The window's own pixels, whatever its opacity. Null when it has none yet.
HBITMAP Picture(HWND hwnd, const RECT& rect) {
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) {
    return nullptr;
  }
  HDC screen = ::GetDC(nullptr);
  HDC memory = ::CreateCompatibleDC(screen);
  HBITMAP bitmap = ::CreateCompatibleBitmap(screen, width, height);
  ::ReleaseDC(nullptr, screen);
  if (!memory || !bitmap) {
    if (bitmap) ::DeleteObject(bitmap);
    if (memory) ::DeleteDC(memory);
    return nullptr;
  }
  HGDIOBJ previous = ::SelectObject(memory, bitmap);
  // PW_RENDERFULLCONTENT: what the compositor holds for the window, which
  // is there even while the layer's alpha is 0.
  bool drawn = ::PrintWindow(hwnd, memory, 0x00000002) != FALSE;
  if (drawn) {
    // An unpainted window comes back as one flat colour (usually black).
    const COLORREF first = ::GetPixel(memory, width / 2, height / 2);
    bool flat = true;
    for (int i = 1; i <= 6 && flat; ++i) {
      for (int j = 1; j <= 6 && flat; ++j) {
        flat = ::GetPixel(memory, width * i / 7, height * j / 7) == first;
      }
    }
    // An unpainted themed menu can be grey or tinted, not just black.
    // Wait for actual content instead of animating a blank rectangle and
    // replacing it with the menu only when the flight lands.
    drawn = !flat;
  }
  ::SelectObject(memory, previous);
  ::DeleteDC(memory);
  if (!drawn) {
    ::DeleteObject(bitmap);
    return nullptr;
  }
  return bitmap;
}

void Reveal(HWND hwnd) {
  if (::IsWindow(hwnd)) {
    ::SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
  }
}

void CALLBACK OnFrame(HWND, UINT, UINT_PTR, DWORD) {
  std::map<HWND, PopIn>& arriving = Arriving();
  const ULONGLONG now = ::GetTickCount64();
  for (auto it = arriving.begin(); it != arriving.end();) {
    const HWND hwnd = it->first;
    PopIn& pop = it->second;
    if (!::IsWindow(hwnd)) {
      PhiCardFlight::Cancel(pop.flight);
      Sources().erase(hwnd);
      it = arriving.erase(it);
      continue;
    }
    if (pop.flight != 0 || !::IsWindowVisible(hwnd)) {
      ++it;  // In flight (its landing removes it), or not shown yet.
      continue;
    }
    if (pop.fade_start != 0) {
      const double t = std::min(
          1.0, static_cast<double>(now - pop.fade_start) /
                   static_cast<double>(kFadeMs));
      const double eased = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
      ::SetLayeredWindowAttributes(
          hwnd, 0, static_cast<BYTE>(eased * 255.0 + 0.5), LWA_ALPHA);
      it = t >= 1.0 ? arriving.erase(it) : std::next(it);
      continue;
    }
    if (pop.visible_since == 0) {
      pop.visible_since = now;
    }
    // One tick's grace for the first paint, then the picture.
    if (++pop.ticks_visible < 2) {
      ++it;
      continue;
    }
    RECT rect = {};
    ::GetWindowRect(hwnd, &rect);
    HBITMAP picture = Picture(hwnd, rect);
    if (!picture) {
      if (now - pop.visible_since < kPaintWaitMs) {
        ++it;  // Not painted yet: look again next tick.
        continue;
      }
      pop.fade_start = now;  // No picture to be had: fade in instead.
      ++it;
      continue;
    }
    pop.flight = PhiCardFlight::FlyInSnapshot(
        ::GetWindow(hwnd, GW_OWNER), rect, AnchorFor(hwnd, rect, pop.source),
        LookFor(hwnd, kOpenMs), picture, [hwnd] {
          Reveal(hwnd);
          Arriving().erase(hwnd);
        });
    if (pop.flight == 0) {
      Reveal(hwnd);
      it = arriving.erase(it);
      continue;
    }
    ++it;
  }
  if (arriving.empty() && g_timer) {
    ::KillTimer(nullptr, g_timer);
    g_timer = 0;
  }
}

// The menu window this PhiTrackPopupMenu call has popped in, if any. One
// open, one pop-in: only the first menu window shown in the call (its root)
// flies in. Submenus appear the way Windows shows them, so a pass of the
// pointer down the ⋯ menu no longer starts a zoom in and a zoom out for
// every submenu row it rests on (with Windows' animation effects off each
// one had its own; a keyboard-opened one always did), and a menu window
// that is hidden and shown again inside the same open is not flown twice
// (PHI-2037, feedback 372: "fired twice or more"). Leaving a hover-opened
// submenu to Windows also keeps its own fade from dropping a layer we set
// (GitHub phibrowser/phibrowser-win#9 regression, 9e52ee8).
HWND g_menu_popped = nullptr;

// Windows 11 rounds a menu window only when it draws the rows itself: one
// with owner-drawn rows (every menu PhiMenuApplyTheme dresses) is left at
// DWMWCP_DEFAULT and comes up square, with the old flat frame and a square
// drop shadow (PHI-2138, feedback 386: the tab area's menu). Ask for the
// same 8 dip rounding as PhiPopover, including submenus. Using ROUNDSMALL
// here made menus look square beside the Extensions/Downloads popovers.
// It holds for a layered window too, so the pop-in keeps it.
// Windows 10 refuses the attribute and
// the menu stays as it was.
void RoundMenuCorners(HWND menu_window) {
  const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
  ::DwmSetWindowAttribute(menu_window, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                          sizeof(corner));
}

// A menu window about to be placed stays inside its monitor's WORK area:
// Windows keeps a popup menu on the monitor but lets it cover the taskbar
// (PHI-2095). Only moves it, and only when it fits.
void KeepMenuInWorkArea(HWND hwnd, WINDOWPOS* pos) {
  if ((pos->flags & SWP_NOMOVE) && (pos->flags & SWP_NOSIZE) &&
      !(pos->flags & SWP_SHOWWINDOW)) {
    return;
  }
  RECT now = {};
  ::GetWindowRect(hwnd, &now);
  const int x = (pos->flags & SWP_NOMOVE) ? now.left : pos->x;
  const int y = (pos->flags & SWP_NOMOVE) ? now.top : pos->y;
  const int cx = (pos->flags & SWP_NOSIZE) ? now.right - now.left : pos->cx;
  const int cy = (pos->flags & SWP_NOSIZE) ? now.bottom - now.top : pos->cy;
  if (cx <= 0 || cy <= 0) {
    return;
  }
  const RECT rect = {x, y, x + cx, y + cy};
  MONITORINFO info = {sizeof(info)};
  if (!::GetMonitorInfoW(::MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST),
                         &info)) {
    return;
  }
  const RECT& work = info.rcWork;
  int nx = x;
  int ny = y;
  if (cy <= work.bottom - work.top) {
    ny = std::max(static_cast<int>(work.top),
                  std::min(y, static_cast<int>(work.bottom) - cy));
  }
  if (cx <= work.right - work.left) {
    nx = std::max(static_cast<int>(work.left),
                  std::min(x, static_cast<int>(work.right) - cx));
  }
  if (nx != x || ny != y) {
    pos->x = nx;
    pos->y = ny;
    pos->flags &= ~static_cast<UINT>(SWP_NOMOVE);
  }
}

bool IsMenuWindow(HWND hwnd) {
  wchar_t cls[16] = {};
  return ::GetClassNameW(hwnd, cls, 16) > 0 &&
         ::lstrcmpW(cls, kMenuWindowClass) == 0;
}

// After the owner has filled a menu (WM_INITMENUPOPUP returned) and before
// Windows sizes it: shorten its rows when it would not fit the screen.
LRESULT CALLBACK MenuFitHook(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION) {
    const CWPRETSTRUCT* message =
        reinterpret_cast<const CWPRETSTRUCT*>(lparam);
    if (message && message->message == WM_INITMENUPOPUP) {
      const HMENU menu = reinterpret_cast<HMENU>(message->wParam);
      const UINT dpi = message->hwnd ? ::GetDpiForWindow(message->hwnd) : 0;
      HMONITOR monitor = g_menu_monitor;
      if (!monitor && message->hwnd) {
        monitor = ::MonitorFromWindow(message->hwnd, MONITOR_DEFAULTTONEAREST);
      }
      PhiMenuFitToWorkArea(menu, monitor, dpi);
    }
  }
  return ::CallNextHookEx(nullptr, code, wparam, lparam);
}

// Home and End inside the menu loop; Windows' menus ignore both.
LRESULT CALLBACK MenuKeyHook(int code, WPARAM wparam, LPARAM lparam) {
  if (code == MSGF_MENU) {
    const MSG* msg = reinterpret_cast<const MSG*>(lparam);
    if (msg && msg->message == WM_KEYDOWN &&
        (msg->wParam == VK_HOME || msg->wParam == VK_END) &&
        PhiMenuSelectEdge(msg->wParam == VK_END)) {
      return TRUE;
    }
  }
  return ::CallNextHookEx(nullptr, code, wparam, lparam);
}

// Sees every message SENT on this thread while a menu is up: a menu window
// about to be shown is rounded and armed before its first frame, and one
// about to be hidden leaves its picture behind; every menu window is kept
// inside the work area.
LRESULT CALLBACK MenuHook(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION) {
    const CWPSTRUCT* message = reinterpret_cast<const CWPSTRUCT*>(lparam);
    if (message && message->message == WM_WINDOWPOSCHANGING &&
        message->lParam && IsMenuWindow(message->hwnd)) {
      // The same WINDOWPOS the menu window's procedure gets next.
      KeepMenuInWorkArea(message->hwnd,
                         reinterpret_cast<WINDOWPOS*>(message->lParam));
    }
    if (message && message->message == WM_WINDOWPOSCHANGING) {
      const WINDOWPOS* pos = reinterpret_cast<const WINDOWPOS*>(message->lParam);
      wchar_t cls[16] = {};
      if (pos && (pos->flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW)) &&
          ::GetClassNameW(message->hwnd, cls, 16) > 0 &&
          ::lstrcmpW(cls, kMenuWindowClass) == 0) {
        const bool visible = ::IsWindowVisible(message->hwnd) != FALSE;
        if ((pos->flags & SWP_SHOWWINDOW) && !visible) {
          // Every menu window, submenus too, before its first frame.
          RoundMenuCorners(message->hwnd);
          if (!g_menu_popped) {
            g_menu_popped = message->hwnd;
            PhiPopIn(message->hwnd);
          } else if (message->hwnd == g_menu_popped &&
                     !Arriving().count(message->hwnd)) {
            // Shown again in the same open: as it is, with no second
            // flight (a pop-out that cut its first one short left it at
            // alpha 0).
            Reveal(message->hwnd);
          }
        } else if ((pos->flags & SWP_HIDEWINDOW) && visible) {
          PhiPopOut(message->hwnd);
        }
      }
    }
  }
  return ::CallNextHookEx(nullptr, code, wparam, lparam);
}

}  // namespace

bool PhiPopAnimationsEnabled() {
  // Always: these are part of how Phi looks, not an effect Windows'
  // "Animation effects" switch governs (decided 2026-11-11).
  return true;
}

void PhiPopIn(HWND hwnd, const POINT* source) {
  if (!hwnd || !PhiPopAnimationsEnabled()) {
    return;
  }
  if (Arriving().count(hwnd)) {
    return;  // Already on its way in: one pop-in per showing, never a restart.
  }
  const LONG_PTR ex_style = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  if (!(ex_style & WS_EX_LAYERED)) {
    ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex_style | WS_EX_LAYERED);
  }
  if (!::SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA)) {
    return;  // A per-pixel layered window: it draws its own way in.
  }
  PopIn pop;
  if (source) {
    pop.source = *source;
  } else {
    ::GetCursorPos(&pop.source);
  }
  Arriving()[hwnd] = pop;
  std::map<HWND, POINT>& sources = Sources();
  if (sources.size() > 64) {
    // Windows that went without a pop-out: forget the dead ones.
    for (auto it = sources.begin(); it != sources.end();) {
      it = ::IsWindow(it->first) ? std::next(it) : sources.erase(it);
    }
  }
  sources[hwnd] = pop.source;
  if (!g_timer) {
    g_timer = ::SetTimer(nullptr, 0, kFrameMs, &OnFrame);
    if (!g_timer) {
      // No timer, no animation: never leave the window invisible.
      Reveal(hwnd);
      Arriving().erase(hwnd);
    }
  }
}

void PhiPopOut(HWND hwnd) {
  const auto source = Sources().find(hwnd);
  if (source == Sources().end()) {
    return;
  }
  const POINT origin = source->second;
  Sources().erase(source);
  const auto arriving = Arriving().find(hwnd);
  if (arriving != Arriving().end()) {
    // Closed before it had landed: nothing on screen to fly back.
    PhiCardFlight::Cancel(arriving->second.flight);
    Arriving().erase(arriving);
    return;
  }
  if (!::IsWindow(hwnd) || !::IsWindowVisible(hwnd) || ::IsIconic(hwnd)) {
    return;
  }
  RECT rect = {};
  ::GetWindowRect(hwnd, &rect);
  PhiCardFlight::FlyOut(::GetWindow(hwnd, GW_OWNER), rect,
                        AnchorFor(hwnd, rect, origin),
                        LookFor(hwnd, kCloseMs));
}

BOOL PhiTrackPopupMenu(HMENU menu,
                       UINT flags,
                       int x,
                       int y,
                       int reserved,
                       HWND owner,
                       const RECT* rect) {
  // The menu's loop is a nested one: pipe verbs wait, Chromium's tasks run
  // (PHIBROWSER-WIN-7). A caller's own scope around this one just nests.
  PhiModalScope modal(owner);
  // A plain menu (Settings' dropdowns, the import and URL-rules pickers)
  // is drawn by Windows in uxtheme's app mode, which only a themed menu's
  // show used to set: in Dark it opened light until some Phi-drawn menu
  // had been shown. Follow the app appearance here; a menu
  // PhiMenuApplyTheme has dressed keeps the palette it was given (an
  // incognito window's menus are dark in a light app).
  if (!PhiMenuIsThemed(menu)) {
    PhiMenuApplyFrameMode(PhiPopupColorsNow().dark);
  }
  const bool animate = PhiPopAnimationsEnabled();
  // A menu opened from inside another's loop gets its own pop-in.
  const HWND outer_popped = g_menu_popped;
  g_menu_popped = nullptr;
  // The hook rounds the menu's windows and pops them in when animating
  // (PhiPopIn and PhiPopOut do nothing otherwise); the menu fits the work
  // area of the monitor it opens on (PHI-2095).
  const HMONITOR outer_monitor = g_menu_monitor;
  g_menu_monitor = ::MonitorFromPoint(POINT{x, y}, MONITOR_DEFAULTTONEAREST);
  if (g_menu_depth++ == 0) {
    const DWORD thread = ::GetCurrentThreadId();
    g_menu_hook =
        ::SetWindowsHookExW(WH_CALLWNDPROC, &MenuHook, nullptr, thread);
    g_menu_fit_hook = ::SetWindowsHookExW(WH_CALLWNDPROCRET, &MenuFitHook,
                                          nullptr, thread);
    g_menu_key_hook =
        ::SetWindowsHookExW(WH_MSGFILTER, &MenuKeyHook, nullptr, thread);
  }
  // With our pop-in the system's fade or slide would run first and delay
  // the menu; without it the flags stay the caller's.
  const BOOL result = ::TrackPopupMenu(
      menu, animate ? (flags | TPM_NOANIMATION) : flags, x, y, reserved, owner,
      rect);
  if (--g_menu_depth == 0) {
    for (HHOOK* hook : {&g_menu_hook, &g_menu_fit_hook, &g_menu_key_hook}) {
      if (*hook) {
        ::UnhookWindowsHookEx(*hook);
        *hook = nullptr;
      }
    }
  }
  g_menu_monitor = outer_monitor;
  g_menu_popped = outer_popped;
  return result;
}

}  // namespace phi_client

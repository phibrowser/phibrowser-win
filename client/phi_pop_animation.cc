// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_pop_animation.h"

#include <algorithm>
#include <map>

#include "client/phi_card_flight.h"

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

int DipToPx(HWND hwnd, int dip) {
  const int dpi = static_cast<int>(::GetDpiForWindow(hwnd));
  return ::MulDiv(dip, dpi ? dpi : 96, 96);
}

bool IsTopmost(HWND hwnd) {
  return (::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

// The picture's frame: the rounded corners Windows 11 gives a popup, and a
// soft shadow so the landing does not gain one from nowhere.
PhiCardFlight::Look LookFor(HWND hwnd, int duration_ms) {
  PhiCardFlight::Look look;
  look.radius_px = DipToPx(hwnd, 8);
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
    drawn = !(flat && first == RGB(0, 0, 0));
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

// Sees every message SENT on this thread while a menu is up: a menu window
// about to be shown is armed before its first frame, and one about to be
// hidden leaves its picture behind.
LRESULT CALLBACK MenuHook(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION) {
    const CWPSTRUCT* message = reinterpret_cast<const CWPSTRUCT*>(lparam);
    if (message && message->message == WM_WINDOWPOSCHANGING) {
      const WINDOWPOS* pos = reinterpret_cast<const WINDOWPOS*>(message->lParam);
      wchar_t cls[16] = {};
      if (pos && (pos->flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW)) &&
          ::GetClassNameW(message->hwnd, cls, 16) > 0 &&
          ::lstrcmpW(cls, kMenuWindowClass) == 0) {
        const bool visible = ::IsWindowVisible(message->hwnd) != FALSE;
        if ((pos->flags & SWP_SHOWWINDOW) && !visible) {
          PhiPopIn(message->hwnd);
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
  const bool animate = PhiPopAnimationsEnabled();
  if (animate && g_menu_depth++ == 0) {
    g_menu_hook = ::SetWindowsHookExW(WH_CALLWNDPROC, &MenuHook, nullptr,
                                      ::GetCurrentThreadId());
  }
  // With our pop-in the system's fade or slide would run first and delay
  // the menu; without it the flags stay the caller's.
  const BOOL result = ::TrackPopupMenu(
      menu, animate ? (flags | TPM_NOANIMATION) : flags, x, y, reserved, owner,
      rect);
  if (animate && --g_menu_depth == 0 && g_menu_hook) {
    ::UnhookWindowsHookEx(g_menu_hook);
    g_menu_hook = nullptr;
  }
  return result;
}

}  // namespace phi_client

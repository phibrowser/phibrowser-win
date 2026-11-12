// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_popover.h"

#include "client/phi_pop_animation.h"

#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

namespace phi_client {

namespace {

// The reopen guard's two words of shared state, in one place instead of a
// pair of statics per popover class (lane 3 §5, "Popovers carry hidden
// static coupling").
HWND g_last_closed_owner = nullptr;
ULONGLONG g_last_closed_tick = 0;

}  // namespace

PhiPopover::PhiPopover() = default;

PhiPopover::~PhiPopover() = default;

namespace {
constexpr wchar_t kPopoverProp[] = L"PhiPopover";
}  // namespace

// static
void PhiPopover::RecordClose(HWND owner) {
  g_last_closed_owner = owner;
  g_last_closed_tick = ::GetTickCount64();
}

// static
bool PhiPopover::ShouldSuppressReopen(HWND owner, int within_ms) {
  return owner && g_last_closed_owner == owner &&
         ::GetTickCount64() - g_last_closed_tick <
             static_cast<ULONGLONG>(within_ms);
}

RECT PhiPopover::PlacementRect(const Placement& placement, SIZE size) const {
  HMONITOR monitor = nullptr;
  if (placement.monitor_from_owner && owner_ && ::IsWindow(owner_)) {
    monitor = ::MonitorFromWindow(owner_, MONITOR_DEFAULTTONEAREST);
  } else {
    monitor = ::MonitorFromRect(&placement.anchor, MONITOR_DEFAULTTONEAREST);
  }
  MONITORINFO info = {sizeof(info)};
  RECT work = {0, 0, ::GetSystemMetrics(SM_CXSCREEN),
               ::GetSystemMetrics(SM_CYSCREEN)};
  if (::GetMonitorInfoW(monitor, &info)) {
    work = info.rcWork;
  }

  PhiPlacementSpec spec;
  spec.side = placement.side;
  spec.align = placement.align;
  spec.gap = Scaled(placement.gap_dip);
  spec.margin = placement.margin_px;
  spec.flip = placement.flip;
  spec.flip_only_if_fits = placement.flip_only_if_fits;
  spec.clamp = placement.clamp;
  const PhiRectI out =
      PhiPlacePopover(PhiFromRect(placement.anchor), size.cx, size.cy,
                      PhiFromRect(work), spec);
  return PhiToRect(out);
}

bool PhiPopover::CreatePopover(const ClassSpec& spec,
                               HWND owner,
                               SIZE size,
                               const Placement& placement,
                               const Options& options) {
  if (!owner || !::IsWindow(owner)) {
    return false;
  }
  owner_ = owner;
  options_ = options;

  DWORD ex_style = WS_EX_TOOLWINDOW;
  if (!options_.activate) {
    ex_style |= WS_EX_NOACTIVATE;
  }
  if (options_.topmost) {
    ex_style |= WS_EX_TOPMOST;
  }
  if (options_.layered) {
    ex_style |= WS_EX_LAYERED;
  }

  // Create at the origin first: placement needs Scaled(), which needs the
  // dpi Create() resolves off the owner. The window is not visible yet.
  RECT bounds = {0, 0, size.cx, size.cy};
  if (!Create(spec, ex_style, WS_POPUP, bounds, owner)) {
    return false;
  }
  const RECT target = PlacementRect(placement, size);
  ::SetWindowPos(hwnd(), nullptr, target.left, target.top,
                 target.right - target.left, target.bottom - target.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
  // Where it will grow from: the middle of what it hangs off. A popover
  // placed at an explicit point has no such thing and takes the pointer.
  has_pop_source_ = placement.side != PhiPopoverSide::kExplicit &&
                    placement.anchor.right > placement.anchor.left &&
                    placement.anchor.bottom > placement.anchor.top;
  pop_source_ = {(placement.anchor.left + placement.anchor.right) / 2,
                 (placement.anchor.top + placement.anchor.bottom) / 2};
  if (options_.dwm_round) {
    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    // Windows 10 has no rounding and refuses the attribute: the popover is
    // then square and keeps drawing its own rim.
    dwm_rounded_ = SUCCEEDED(::DwmSetWindowAttribute(
        hwnd(), DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner)));
  }
  // Hit names are the tooltips (phi_window.h): the icon-only actions on a
  // popover are otherwise unlabelled to a sighted user.
  EnableTooltips(true);
  // The nested-popover rule above recognises an owner by this mark.
  ::SetPropW(hwnd(), kPopoverProp, reinterpret_cast<HANDLE>(1));
  return true;
}

// static
bool PhiPopover::IsPopoverWindow(HWND hwnd) {
  return hwnd && ::GetPropW(hwnd, kPopoverProp) != nullptr;
}

void PhiPopover::Show() {
  if (!alive()) {
    return;
  }
  shown_tick_ = ::GetTickCount64();
  // A popover the user opened grows out of what opened it (its anchor) and
  // shrinks back there when it goes (phi_pop_animation.h): one that takes
  // the focus, is not already up, and draws itself (a per-pixel layered
  // surface has its own way in). Hover cards, toasts and HUDs keep their
  // own slides and fades.
  if (options_.activate && !options_.layered && !::IsWindowVisible(hwnd())) {
    PhiPopIn(hwnd(), has_pop_source_ ? &pop_source_ : nullptr);
  }
  ::ShowWindow(hwnd(), options_.activate ? SW_SHOW : SW_SHOWNOACTIVATE);
  if (options_.activate && options_.foreground) {
    ::SetForegroundWindow(hwnd());
  }
}

void PhiPopover::Reposition(const Placement& placement, SIZE size) {
  if (!alive()) {
    return;
  }
  const RECT target = PlacementRect(placement, size);
  ::SetWindowPos(hwnd(), nullptr, target.left, target.top,
                 target.right - target.left, target.bottom - target.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void PhiPopover::MoveTo(const RECT& screen_rect) {
  if (!alive()) {
    return;
  }
  ::SetWindowPos(hwnd(), nullptr, screen_rect.left, screen_rect.top,
                 screen_rect.right - screen_rect.left,
                 screen_rect.bottom - screen_rect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

bool PhiPopover::InOpeningGrace() const {
  return shown_tick_ != 0 &&
         ::GetTickCount64() - shown_tick_ <
             static_cast<ULONGLONG>(options_.opening_grace_ms);
}

void PhiPopover::Dismiss() {
  // A popover that closes while it holds the foreground (Escape, a pick)
  // hands it back to its owner first. Left to DestroyWindow, Windows picks
  // the next window in z-order — another app or Phi's console — and the
  // owner stays inactive: its acrylic sidebar falls back to an opaque fill
  // until clicked. Hiding moves activation before WM_DESTROY, so this runs
  // ahead of Destroy. A dismissal by deactivation (a click elsewhere) has
  // already lost the foreground and leaves it where the user put it.
  if (alive() && ::GetForegroundWindow() == hwnd()) {
    const HWND owner = ::GetWindow(hwnd(), GW_OWNER);
    if (owner && ::IsWindowVisible(owner) && ::IsWindowEnabled(owner)) {
      ::SetForegroundWindow(owner);
    }
  }
  if (alive()) {
    PhiPopOut(hwnd());  // Its picture shrinks back to where it came from.
  }
  Destroy();
}

void PhiPopover::OnDismiss() {
  Dismiss();
}

void PhiPopover::OnEscape() {
  if (options_.dismiss_on_escape) {
    OnDismiss();
  }
}

LRESULT PhiPopover::OnMessage(UINT message,
                              WPARAM wparam,
                              LPARAM lparam,
                              bool* handled) {
  switch (message) {
    case WM_NCHITTEST:
      if (options_.click_through) {
        *handled = true;
        return HTTRANSPARENT;
      }
      break;
    case WM_MOUSEACTIVATE:
      if (!options_.activate) {
        *handled = true;
        return MA_NOACTIVATE;
      }
      break;
    case WM_ACTIVATE:
      if (LOWORD(wparam) == WA_INACTIVE) {
        if (!options_.dismiss_on_inactive || InOpeningGrace()) {
          break;
        }
        // NESTED POPOVERS (the create-Space flyout's colour editor): a
        // popover this one owns taking the activation is not the user
        // leaving — the parent stays up under its child, as an AppKit
        // popover does. When the CHILD is then dismissed for a window
        // that is neither its owner nor a sibling under that owner, the
        // owner popover has lost the user too but never hears it (it was
        // already inactive): tell it, so no parent lingers inactive.
        const HWND next = reinterpret_cast<HWND>(lparam);
        if (next && ::GetWindow(next, GW_OWNER) == hwnd()) {
          break;
        }
        *handled = true;
        const HWND owner = owner_;
        OnDismiss();
        if (owner && ::IsWindow(owner) && IsPopoverWindow(owner) &&
            next != owner && (!next || ::GetWindow(next, GW_OWNER) != owner)) {
          ::SendMessageW(owner, WM_ACTIVATE, WA_INACTIVE,
                         reinterpret_cast<LPARAM>(next));
        }
        return 0;
      }
      break;
    default:
      break;
  }
  return PhiWindow::OnMessage(message, wparam, lparam, handled);
}

}  // namespace phi_client

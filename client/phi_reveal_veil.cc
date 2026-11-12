// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_reveal_veil.h"

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "client/phi_panel.h"

namespace phi_client {

namespace {

constexpr wchar_t kVeilClass[] = L"PhiRevealVeil";

}  // namespace

PhiRevealVeil::PhiRevealVeil() = default;

PhiRevealVeil::~PhiRevealVeil() {
  Destroy();
}

void PhiRevealVeil::Cover(HWND owner, const RECT& screen_rect) {
  const int width = screen_rect.right - screen_rect.left;
  const int height = screen_rect.bottom - screen_rect.top;
  if (!owner || !::IsWindowVisible(owner) || ::IsIconic(owner) ||
      width <= 0 || height <= 0) {
    return;
  }
  if (alive() && owner != owner_) {
    Destroy();  // A veil rides one shell.
  }
  if (!alive()) {
    ClassSpec spec;
    spec.name = kVeilClass;
    spec.cursor = nullptr;
    const RECT nothing = {0, 0, 0, 0};
    // Owned, so it stays above its shell and goes with it; transparent to
    // hit tests and never activated, so input reaches the shell under it.
    if (!Create(spec,
                WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                    WS_EX_NOACTIVATE,
                WS_POPUP, nothing, owner)) {
      return;
    }
    owner_ = owner;
    hold_ = PhiWindow::Timer(this);
  }
  PhiLayerSurface layer(width, height);
  if (!layer.valid()) {
    return;
  }
  // What the user sees now: the desktop composition, the browser child's
  // DirectComposition layer and the layered corner caps included.
  HDC screen = ::GetDC(nullptr);
  const bool read = screen &&
                    ::BitBlt(layer.dc(), 0, 0, width, height, screen,
                             screen_rect.left, screen_rect.top,
                             SRCCOPY | CAPTUREBLT) != FALSE;
  if (screen) {
    ::ReleaseDC(nullptr, screen);
  }
  if (!read) {
    return;
  }
  ::GdiFlush();
  // BitBlt leaves the alpha byte zero; the layer is opaque.
  // SAFETY: the layer was just created with exactly width * height * 4
  // bytes (top-down, no stride padding at 32bpp).
  base::span<unsigned char> bits = UNSAFE_BUFFERS(base::span<unsigned char>(
      static_cast<unsigned char*>(layer.bits()), layer.byte_size()));
  for (size_t i = 3; i < bits.size(); i += 4) {
    bits[i] = 0xFF;
  }
  const POINT origin = {screen_rect.left, screen_rect.top};
  if (!layer.Commit(hwnd(), &origin)) {
    return;
  }
  // Directly above the shell: the shell's own popups (a hover card, a
  // flyout) stay above the veil.
  const HWND above = ::GetWindow(owner, GW_HWNDPREV);
  ::SetWindowPos(hwnd(), above && above != hwnd() ? above : HWND_TOP, 0, 0,
                 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                           SWP_SHOWWINDOW);
  hold_.Stop();
  hold_.Start(kHoldMs);
}

void PhiRevealVeil::Lift() {
  hold_.Stop();
  if (alive()) {
    ::ShowWindow(hwnd(), SW_HIDE);
  }
}

LRESULT PhiRevealVeil::OnMessage(UINT message,
                                 WPARAM wparam,
                                 LPARAM lparam,
                                 bool* handled) {
  if (message == WM_NCHITTEST) {
    *handled = true;
    return HTTRANSPARENT;
  }
  if (message == WM_MOUSEACTIVATE) {
    *handled = true;
    return MA_NOACTIVATE;
  }
  return PhiWindow::OnMessage(message, wparam, lparam, handled);
}

void PhiRevealVeil::OnTimerFired(UINT_PTR id) {
  if (id == hold_.id()) {
    Lift();
  }
}

}  // namespace phi_client

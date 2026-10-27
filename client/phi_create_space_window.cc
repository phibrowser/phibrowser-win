// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_create_space_window.h"

#include <algorithm>

#include "base/logging.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_shell_window.h"

namespace phi_client {

namespace {

constexpr wchar_t kClassName[] = L"PhiCreateSpaceWindow";
// The Mac window: the form's 280pt column inside 20pt of padding.
constexpr int kWidthDip = 320;
// Provisional until the form has measured itself (FitToForm).
constexpr int kProvisionalHeightDip = 600;

PhiCreateSpaceWindow* g_window = nullptr;

}  // namespace

// static
void PhiCreateSpaceWindow::Open(PhiShellWindow* controller,
                                const std::string& initial_profile_id,
                                const std::string& source_theme_id,
                                int saturation,
                                HWND anchor) {
  if (!controller) {
    return;
  }
  if (g_window && g_window->alive() &&
      (g_window->controller_ != controller || g_window->closing_)) {
    // A form is up for another window's slot: the new request takes over,
    // as the Mac's shared window opens for the active window. (A form that
    // already committed or cancelled is on its way out; a fresh one opens.)
    g_window->Destroy();  // Self-owned: clears g_window and frees it.
  }
  if (!g_window || !g_window->alive()) {
    auto* window = new PhiCreateSpaceWindow(controller);
    if (!window->OpenWindow(initial_profile_id, source_theme_id, saturation,
                            anchor)) {
      delete window;
      return;
    }
    // Self-owned from here: the object (its form and the form's fonts) goes
    // with its HWND; OnNcDestroy clears g_window first.
    window->DeleteWhenDestroyed();
    g_window = window;
  }
  // A minimized window comes back; SW_SHOW alone leaves it on the taskbar.
  ::ShowWindow(g_window->hwnd(),
               ::IsIconic(g_window->hwnd()) ? SW_RESTORE : SW_SHOW);
  ::SetForegroundWindow(g_window->hwnd());
  if (g_window->form_) {
    g_window->form_->FocusField();
  }
}

// static
void PhiCreateSpaceWindow::CloseIfBoundTo(PhiShellWindow* controller) {
  if (g_window && g_window->alive() && g_window->controller_ == controller) {
    g_window->Destroy();
  }
}

// static
void PhiCreateSpaceWindow::CloseForSignOut() {
  if (g_window && g_window->alive()) {
    g_window->CloseFromForm();  // Hidden now, destroyed on a posted turn.
  }
}

// static
void PhiCreateSpaceWindow::NoteProfileCreated(PhiShellWindow* controller,
                                              const std::string& profile_id) {
  if (g_window && g_window->alive() && g_window->controller_ == controller &&
      g_window->form_) {
    g_window->form_->OnProfileCreated(profile_id);
  }
}

PhiCreateSpaceWindow::PhiCreateSpaceWindow(PhiShellWindow* controller)
    : controller_(controller) {}

PhiCreateSpaceWindow::~PhiCreateSpaceWindow() {
  form_.reset();
  if (bg_brush_) {
    ::DeleteObject(bg_brush_);
  }
}

bool PhiCreateSpaceWindow::OpenWindow(const std::string& initial_profile_id,
                                      const std::string& source_theme_id,
                                      int saturation,
                                      HWND anchor) {
  ClassSpec spec;
  spec.name = kClassName;
  Options options;
  // Close only, no title on the band and no leading mark: the Mac's
  // transparent title bar with its zoom and minimize buttons hidden.
  options.caption_buttons = kPhiCaptionClose;
  options.show_mark = false;
  // The form handles Escape and Return itself (PhiCreateSpacePanel::OnKey,
  // and its field's FieldProc); nothing for the dialog manager.
  options.dialog_keys = kPhiDialogNone;
  const RECT bounds = {CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT + 100,
                       CW_USEDEFAULT + 100};
  // Ownerless like Settings and Feedback: the Mac's panel window stands on
  // its own, and the shell it serves may go behind it.
  if (!CreateSecondary(spec, 0,
                       WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                       bounds, nullptr, PhiStr("sidebar.createSpace.title"),
                       options)) {
    LOG(ERROR) << "[CreateSpace] CreateWindowEx failed err="
               << ::GetLastError();
    return false;
  }
  OnColorsChanged();
  const SIZE outer = chrome().OuterSize(kWidthDip, kProvisionalHeightDip);
  ::SetWindowPos(hwnd(), nullptr, 0, 0, outer.cx, outer.cy,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  form_ = std::make_unique<PhiCreateSpacePanel>(
      controller_, hwnd(), static_cast<PhiCreateSpaceFormHost*>(this));
  form_->Show(ContentRect(), initial_profile_id, source_theme_id, saturation);
  FitToForm();
  CenterOn(anchor && ::IsWindow(anchor) ? anchor : controller_->shell_hwnd());
  return true;
}

void PhiCreateSpaceWindow::FitToForm() {
  if (!alive() || !form_) {
    return;
  }
  // The form measured itself at the provisional width; the window takes
  // that height and the form then fills the client area exactly.
  const int content_px = form_->ContentHeight();
  if (content_px > 0) {
    const int content_dip =
        static_cast<int>((content_px * 96 + dpi() - 1) / dpi());
    const SIZE outer = chrome().OuterSize(kWidthDip, content_dip);
    ::SetWindowPos(hwnd(), nullptr, 0, 0, outer.cx, outer.cy,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  form_->Reposition(ContentRect());
}

void PhiCreateSpaceWindow::Paint(HDC dc, const RECT&) {
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  ::FillRect(dc, &client, bg_brush_);
  PaintBand(dc, L"");  // The band carries the close button alone.
}

LRESULT PhiCreateSpaceWindow::OnWindowMessage(UINT msg,
                                              WPARAM wparam,
                                              LPARAM lparam,
                                              bool* handled) {
  switch (msg) {
    case WM_SIZE:
      if (alive() && form_) {
        form_->Reposition(ContentRect());
      }
      break;
    case WM_CLOSE:
      Destroy();
      *handled = true;
      return 0;
    default:
      break;
  }
  return PhiSecondaryWindow::OnWindowMessage(msg, wparam, lparam, handled);
}

void PhiCreateSpaceWindow::OnEscape() {
  Destroy();
}

void PhiCreateSpaceWindow::OnColorsChanged() {
  if (bg_brush_) {
    ::DeleteObject(bg_brush_);
  }
  bg_brush_ = ::CreateSolidBrush(colors().bg);
  if (form_) {
    form_->Refresh();
  }
}

void PhiCreateSpaceWindow::OnDpiChanged(UINT old_dpi) {
  PhiSecondaryWindow::OnDpiChanged(old_dpi);
  FitToForm();
}

void PhiCreateSpaceWindow::OnNcDestroy() {
  if (g_window == this) {
    g_window = nullptr;
  }
  PhiSecondaryWindow::OnNcDestroy();
}

void PhiCreateSpaceWindow::FormCommit(const PhiNewSpaceSpec& spec) {
  // The Mac's onClose after manager.createSpace: the window goes, the new
  // Space presents in the slot the request came from.
  if (controller_ && controller_->spaces_host()) {
    controller_->spaces_host()->RequestCreateSpaceWithOptions(
        controller_->slot().value, spec, "standalone_window");
  }
  CloseFromForm();
}

void PhiCreateSpaceWindow::FormCancel() {
  CloseFromForm();
}

void PhiCreateSpaceWindow::CloseFromForm() {
  // The form calls in from its own click / key / field handlers, still on
  // its stack, and destroying the window now would free the form (the
  // window is self-owned) under it. Hide at once; the posted close tears it
  // down from the window's own turn.
  if (closing_ || !alive()) {
    return;
  }
  closing_ = true;
  ::ShowWindow(hwnd(), SW_HIDE);
  ::PostMessageW(hwnd(), WM_CLOSE, 0, 0);
}

COLORREF PhiCreateSpaceWindow::FormGround() const {
  return colors().bg;
}

}  // namespace phi_client

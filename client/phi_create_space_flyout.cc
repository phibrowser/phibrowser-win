// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_create_space_flyout.h"

#include <memory>
#include <utility>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_create_space_panel.h"
#include "client/phi_popover.h"
#include "client/phi_shell_window.h"
#include "client/phi_theme.h"

namespace phi_client {

namespace {

constexpr int kWidthDip = 300;          // §5.5: a 300 dip flyout.
constexpr int kProvisionalHeightDip = 600;

class Flyout : public PhiPopover, private PhiCreateSpaceFormHost {
 public:
  explicit Flyout(PhiShellWindow* controller) : controller_(controller) {}
  ~Flyout() override { form_.reset(); }

  PhiShellWindow* controller() const { return controller_; }
  PhiCreateSpacePanel* form() const { return form_.get(); }

  bool Open(const RECT& anchor_screen,
            const std::string& initial_profile_id,
            const std::string& source_theme_id,
            int saturation) {
    HWND owner = controller_ ? controller_->shell_hwnd() : nullptr;
    if (!owner) {
      return false;
    }
    ClassSpec spec;
    spec.name = L"PhiCreateSpaceFlyout";
    spec.style = CS_DBLCLKS;
    Options options;  // Activating, dismiss on inactive or Escape.
    placement_.anchor = anchor_screen;
    placement_.side = PhiPopoverSide::kBelow;
    placement_.align = PhiPopoverAlign::kLeading;
    placement_.gap_dip = 4;
    placement_.margin_px = 8;
    const SIZE probe = {Scaled(kWidthDip), Scaled(kProvisionalHeightDip)};
    if (!CreatePopover(spec, owner, probe, placement_, options)) {
      return false;
    }
    form_ = std::make_unique<PhiCreateSpacePanel>(
        controller_, hwnd(), static_cast<PhiCreateSpaceFormHost*>(this));
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    form_->Show(client, initial_profile_id, source_theme_id, saturation);
    FitToForm();
    Show();
    form_->FocusField();
    return true;
  }

 protected:
  PhiPalette QueryPalette() const override {
    return controller_ ? controller_->CurrentPalette()
                       : PhiResolvePalette(PhiThemeById("pure"),
                                           PhiAppearance::kSystem);
  }

  void Paint(HDC dc, const RECT&) override {
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    HBRUSH ground = ::CreateSolidBrush(palette().flyout);
    ::FillRect(dc, &client, ground);
    ::DeleteObject(ground);
  }

  LRESULT OnMessage(UINT message, WPARAM wparam, LPARAM lparam,
                    bool* handled) override {
    if (message == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE) {
      // The form's "New profile…" prompt is owned by this window: focus
      // moving to it is not the user leaving the flyout.
      const HWND other = reinterpret_cast<HWND>(lparam);
      if (other && ::GetWindow(other, GW_OWNER) == hwnd()) {
        *handled = true;
        return 0;
      }
    }
    if (message == WM_SIZE && alive() && form_) {
      RECT client = {};
      ::GetClientRect(hwnd(), &client);
      form_->Reposition(client);
    }
    return PhiPopover::OnMessage(message, wparam, lparam, handled);
  }

  void OnDpiChanged(UINT) override { FitToForm(); }

  void OnThemeChanged() override {
    PhiPopover::OnThemeChanged();
    if (form_) {
      form_->Refresh();
    }
  }

  void OnNcDestroy() override;

  // PhiCreateSpaceFormHost.
  void FormCommit(const PhiNewSpaceSpec& spec) override {
    // The Mac's onClose after manager.createSpace: the flyout goes, the
    // new Space presents in the slot the request came from.
    if (controller_ && controller_->spaces_host()) {
      controller_->spaces_host()->RequestCreateSpaceWithOptions(
          controller_->slot().value, spec, "sidebar");
    }
    Dismiss();
  }
  void FormCancel() override { Dismiss(); }
  COLORREF FormGround() const override { return palette().flyout; }

 private:
  void FitToForm() {
    if (!alive() || !form_) {
      return;
    }
    // The form measured itself at the provisional height; the flyout
    // takes that height and the form then fills the client area exactly.
    const int content_px = form_->ContentHeight();
    if (content_px > 0) {
      Reposition(placement_, {Scaled(kWidthDip), content_px});
    }
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    form_->Reposition(client);
  }

  RAW_PTR_EXCLUSION PhiShellWindow* const controller_;
  std::unique_ptr<PhiCreateSpacePanel> form_;
  Placement placement_;
};

Flyout* g_flyout = nullptr;

void Flyout::OnNcDestroy() {
  if (g_flyout == this) {
    g_flyout = nullptr;
  }
  PhiPopover::OnNcDestroy();
}

}  // namespace

void PhiOpenCreateSpaceFlyout(PhiShellWindow* controller,
                              const RECT& anchor_screen,
                              const std::string& initial_profile_id,
                              const std::string& source_theme_id,
                              int saturation) {
  if (!controller) {
    return;
  }
  if (g_flyout && g_flyout->alive()) {
    if (g_flyout->controller() == controller) {
      ::SetForegroundWindow(g_flyout->hwnd());
      if (g_flyout->form()) {
        g_flyout->form()->FocusField();
      }
      return;
    }
    g_flyout->Destroy();
  }
  Flyout* flyout = new Flyout(controller);
  flyout->DeleteWhenDestroyed();
  if (!flyout->Open(anchor_screen, initial_profile_id, source_theme_id,
                    saturation)) {
    delete flyout;
    return;
  }
  g_flyout = flyout;
}

void PhiCloseCreateSpaceFlyoutIfBoundTo(PhiShellWindow* controller) {
  if (g_flyout && g_flyout->alive() && g_flyout->controller() == controller) {
    g_flyout->Destroy();
  }
}

void PhiNoteCreateSpaceFlyoutProfileCreated(PhiShellWindow* controller,
                                            const std::string& profile_id) {
  if (g_flyout && g_flyout->alive() && g_flyout->controller() == controller &&
      g_flyout->form()) {
    g_flyout->form()->OnProfileCreated(profile_id);
  }
}

}  // namespace phi_client

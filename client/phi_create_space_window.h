// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_CREATE_SPACE_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_CREATE_SPACE_WINDOW_H_

#include <windows.h>

#include <memory>
#include <string>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_create_space_panel.h"
#include "phibrowser-win/client/phi_secondary_window.h"

namespace phi_client {

class PhiShellWindow;

// The Mac's CreateSpacePanel.present(...): the "Create a Space" form in a
// chrome-light window of its own, for wherever a docked sidebar cannot host
// it inline — the comfortable layout (the compact pip's "New Space…"), a
// collapsed sidebar with no floating panel up, a window without a Spaces
// strip. Titled and closable only (the Mac hides its title, minimize and
// zoom), the form on the popup ground rather than a Space tint, no live
// theme preview; one window app-wide, a second request brings it forward
// (the Mac reuses its open window); Create, Cancel, Escape and the close
// button all take it down, and the created Space presents in the slot the
// request came from.
class PhiCreateSpaceWindow : public PhiSecondaryWindow,
                             private PhiCreateSpaceFormHost {
 public:
  // Opens (or fronts) the window for `controller`'s slot. The form opens on
  // `initial_profile_id`'s profile and picks a random theme other than
  // `source_theme_id` (the active Space's), with the slider at
  // `saturation` — CreateSpacePanel.requestCreation's seeding. The window
  // centres on `anchor` (the shell when null): Settings' Spaces pane
  // centres it on itself, so it comes up in front of Settings rather than
  // over a browser window behind it.
  static void Open(PhiShellWindow* controller,
                   const std::string& initial_profile_id,
                   const std::string& source_theme_id,
                   int saturation,
                   HWND anchor = nullptr);
  // The shell that owns the request is going away: the form goes with it.
  static void CloseIfBoundTo(PhiShellWindow* controller);
  // Sign-out: the form goes, whichever shell it belongs to.
  static void CloseForSignOut();
  // A profile the form asked for ("New Profile…") has appeared in
  // `controller`'s slot: the form's picker selects it.
  static void NoteProfileCreated(PhiShellWindow* controller,
                                 const std::string& profile_id);

  ~PhiCreateSpaceWindow() override;

 private:
  explicit PhiCreateSpaceWindow(PhiShellWindow* controller);
  bool OpenWindow(const std::string& initial_profile_id,
                  const std::string& source_theme_id,
                  int saturation,
                  HWND anchor);
  // The form fills the client area below the band; the window is as tall
  // as the form wants (the Mac's setContentSize(fittingSize)).
  void FitToForm();
  // Commit / Cancel from inside the form: hide now, destroy on a posted
  // turn (see the .cc).
  void CloseFromForm();

  // PhiSecondaryWindow.
  void Paint(HDC dc, const RECT& dirty) override;
  LRESULT OnWindowMessage(UINT msg,
                          WPARAM wparam,
                          LPARAM lparam,
                          bool* handled) override;
  void OnColorsChanged() override;
  void OnDpiChanged(UINT old_dpi) override;
  void OnEscape() override;
  void OnNcDestroy() override;

  // PhiCreateSpaceFormHost.
  void FormCommit(const PhiNewSpaceSpec& spec) override;
  void FormCancel() override;
  COLORREF FormGround() const override;

  RAW_PTR_EXCLUSION PhiShellWindow* const controller_;
  std::unique_ptr<PhiCreateSpacePanel> form_;
  HBRUSH bg_brush_ = nullptr;
  // Committed or cancelled: hidden, a WM_CLOSE posted.
  bool closing_ = false;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CREATE_SPACE_WINDOW_H_

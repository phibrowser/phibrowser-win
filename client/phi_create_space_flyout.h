// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_CREATE_SPACE_FLYOUT_H_
#define PHIBROWSER_WIN_CLIENT_PHI_CREATE_SPACE_FLYOUT_H_

#include <windows.h>

#include <string>

namespace phi_client {

class PhiShellWindow;

// The create-Space form as a flyout (docs/windows-native-ui.md §5.5, the
// "Flyout" host): a 300 dip PhiPopover dropping from the Space chip, the
// strip's "+" or the sidebar toggle, on the `flyout` ground, hosting the
// same PhiCreateSpacePanel the docked sidebar and the standalone window
// mount. Used by the horizontal layout and a hidden sidebar — the cases
// that opened PhiCreateSpaceWindow before. Settings' "+" keeps the window.
//
// One flyout at a time; opening it for another shell closes the first.
void PhiOpenCreateSpaceFlyout(PhiShellWindow* controller,
                              const RECT& anchor_screen,
                              const std::string& initial_profile_id,
                              const std::string& source_theme_id,
                              int saturation);
// The shell is going: its flyout goes first.
void PhiCloseCreateSpaceFlyoutIfBoundTo(PhiShellWindow* controller);
// A profile the form asked for exists now (the form's "New profile…").
void PhiNoteCreateSpaceFlyoutProfileCreated(PhiShellWindow* controller,
                                            const std::string& profile_id);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CREATE_SPACE_FLYOUT_H_

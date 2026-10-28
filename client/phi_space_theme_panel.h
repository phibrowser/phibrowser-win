// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SPACE_THEME_PANEL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SPACE_THEME_PANEL_H_

#include <windows.h>

#include <functional>
#include <optional>
#include <string>

#include "phibrowser-win/client/phi_theme_palette.h"

namespace phi_client {

class PhiPopover;
class PhiShellWindow;

// The Mac's SpaceThemeEditorView (ThemeOpacitySlider.swift, 3d045cf6 /
// 4c9d919e / 23c728d9), shown from a Space menu's "Set Theme" item: a
// 260x296 popover holding a 244-pt colour field — hue is the angle through
// the Figma spectrum stops, saturation the radius, with the linked
// handles (38 / 28 / 14 pt) showing the palette's exact resolved colours —
// over a toolbar of Light / Dark buttons (the app-wide appearance) and
// the four harmony buttons (single, analogous, complementary, split
// complementary). No brightness slider, no confirm button: a drag
// PREVIEWS the Space's windows per pointer move and persists ONCE on
// release; a click, a harmony button and the arrow keys persist at once.
//
// The panel is a PhiPopover (docs/architecture-review.md §3.6): it does
// NOT run a message loop. `on_closed` runs once, after the window is gone
// (Escape, or focus leaving the panel).
void PhiOpenSpaceThemePanel(PhiShellWindow* controller,
                            const RECT& anchor_screen,
                            const std::string& space_id,
                            std::function<void()> on_closed);

// The same Space editor from Settings ▸ Spaces' "Color ›" row (the Mac's
// ThemeColorSettingsRow popover, arrowEdge .trailing): owned by `owner`
// and placed beside `anchor_screen`; `controller` is any browser window,
// whose SpacesHost reads and writes the record. False when it could not
// open.
bool PhiOpenSpaceThemePanelBeside(PhiShellWindow* controller,
                                  HWND owner,
                                  const RECT& anchor_screen,
                                  const std::string& space_id);

// TRANSITIONAL synchronous wrapper, exactly as for the folder icon picker:
// it drives the asynchronous panel through a short local message loop so
// the existing call site (in phi_shell_window.cc, a file phase 5 does
// not own) keeps compiling. Phase 6 moves that call site onto
// PhiOpenSpaceThemePanel and deletes this.
void PhiShowSpaceThemePanel(PhiShellWindow* controller,
                            const RECT& anchor_screen,
                            const std::string& space_id);

// The same editor over a DRAFT — the create-Space form's "Color ›" row
// (the Mac's SpaceThemeEditorView(palette:) popover): nothing is written
// to any Space; every edit, pointer moves included, lands in `on_change`
// and the form commits the draft after creation.
struct PhiThemeDraftEditor {
  // The theme whose window background shows through the preview — the
  // form's randomly chosen base theme.
  std::string base_theme_id;
  // The draft so far; nullopt seeds the editor from `base_theme_id`'s
  // overlay colour (the Mac's ThemePalette(color:appearance:)).
  std::optional<PhiThemePalette> palette;
  std::function<void(const PhiThemePalette&)> on_change;
  std::function<void()> on_closed;
};

// Opens the draft editor beside `anchor_screen` (to its right, the Mac's
// arrowEdge: .trailing), owned by `owner`. Returns the popover, valid until
// `on_closed` has run, so the form can Dismiss() it when it goes away
// first; null when it could not open (then `on_closed` already ran).
PhiPopover* PhiOpenSpaceThemeDraftPanel(PhiShellWindow* controller,
                                        HWND owner,
                                        const RECT& anchor_screen,
                                        PhiThemeDraftEditor editor);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SPACE_THEME_PANEL_H_

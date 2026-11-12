// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_DROP_BADGE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_DROP_BADGE_H_

#include <windows.h>

#include <string>

#include "client/phi_theme.h"

namespace phi_client {

// Windows' drop-description pill by the cursor (docs/windows-native-ui.md
// §4.8 "Badge"): 22 dip, `flyout` ground, 1 dip stroke, 11 dip text — "Move",
// "Pin", "Split left", "New window", "Move to <Space>"… It is a layered
// top-level tool window the drag session moves with the ghost; a leading
// "+" glyph says the source is kept.
class PhiDropBadge {
 public:
  // Shows (or updates) the badge for `owner`'s drag, its top-left 16 × 20
  // dip past `cursor_screen`. An empty `text` hides it.
  static void Show(HWND owner,
                   UINT dpi,
                   POINT cursor_screen,
                   const std::wstring& text,
                   bool plus,
                   const PhiPalette& palette);
  static void Hide();
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_DROP_BADGE_H_

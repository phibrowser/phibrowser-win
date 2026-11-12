// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_EXTENSIONS_POPOVER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_EXTENSIONS_POPOVER_H_

#include <windows.h>

#include <cstdint>

#include "client/phi_ids.h"
#include "client/phi_theme.h"

namespace phi_framework {
class PhiFramework;
}

namespace phi_client {

// The Mac extensions popover (ExtensionList.swift), opened from the puzzle
// button: an "Extensions" title over a 4-column grid of icon tiles, each
// wearing its badge and a pin disc on its corner, then a "Setting" title
// and the "Manage Extensions" row (the extensions page in a new tab).
// Clicking a tile runs its action (a popup-bearing one anchors Chromium's
// ExtensionPopup to the tile; a disabled one falls back to its context
// menu), a right-click opens that menu. One popover at a time; deactivation
// dismisses it; the toggle races are handled the downloads popover's way.
class PhiExtensionsPopover {
 public:
  static void Toggle(HWND owner,
                     const RECT& anchor_screen,
                     const PhiPalette& palette,
                     phi_framework::PhiFramework* framework,
                     BrowserId browser);
  // Re-reads the list (the delegate's OnExtensionsChanged — the async
  // refresh completed or the set changed). No-op when closed.
  static void RefreshOpen();
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_EXTENSIONS_POPOVER_H_

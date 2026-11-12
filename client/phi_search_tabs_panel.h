// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SEARCH_TABS_PANEL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SEARCH_TABS_PANEL_H_

namespace phi_client {

class PhiShellWindow;

// The Mac SearchTabs palette (Ctrl+Shift+A): a centered floating panel
// searching across open tabs (every window of the profile), pinned tabs,
// bookmarks, and recently-closed entries. Singleton; deactivation or Escape
// dismisses; Enter/click activates the selection.
class PhiSearchTabsPanel {
 public:
  // Opens the palette for `controller`'s shell (or closes it if open).
  static void Toggle(PhiShellWindow* controller);
  // Closes the panel if it is `controller`'s (its shell is being destroyed;
  // called from ~PhiShellWindow). No-op otherwise.
  static void CloseIfBoundTo(const PhiShellWindow* controller);
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SEARCH_TABS_PANEL_H_

// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_TAB_SWITCHER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_TAB_SWITCHER_H_

namespace phi_client {

class PhiShellWindow;

// The Mac Ctrl+Tab MRU switcher HUD: a centered card strip in most-recently-
// used order. Ctrl+Tab opens it selecting the previous tab; further presses
// advance (Shift reverses); RELEASING Ctrl commits the selection.
class PhiTabSwitcher {
 public:
  // True while a gesture is live (a HUD hidden by a pending cancel is not).
  static bool IsActive();
  // The keyboard hook's key-down path, while IsActive(): Esc cancels the
  // gesture. True when the key was consumed (swallow it).
  static bool HandleKeyDown(unsigned int virtual_key);
  // Opens the HUD (when inactive) or advances the selection by `direction`.
  static void Cycle(PhiShellWindow* controller, int direction);
  // Activates the selection and closes.
  static void Commit();
  // Closes without switching. Also runs by itself when the app or the
  // owning window loses activation, so a HUD never outlives its gesture.
  static void Cancel();
  // Cancel() only if the HUD is cycling `controller`'s tabs (its shell is
  // being destroyed; called from ~PhiShellWindow).
  static void CancelIfBoundTo(const PhiShellWindow* controller);
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_TAB_SWITCHER_H_

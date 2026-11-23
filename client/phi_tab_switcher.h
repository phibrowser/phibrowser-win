// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_TAB_SWITCHER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_TAB_SWITCHER_H_

namespace phi_client {

class PhiShellWindow;

// The Mac's tab switcher (TabSwitchManager): its Show Tab Switcher rows,
// Ctrl+Tab and Ctrl+Shift+Tab by default, rebindable. A gesture walks the
// RECENT tabs: the focused one first, then the ones this window has shown
// (or seen opened in the background), newest first, at most five. Forward
// starts on the previous tab, reverse on the oldest of them; further
// presses step on. Releasing a hold key (the chord's Ctrl or Alt) commits.
// The card strip shows only once the hold keys stay down a quarter second,
// or on a second press: a quick tap switches without it.
class PhiTabSwitcher {
 public:
  // The hold keys a gesture's chord carried, as Cycle's `hold_mask`.
  static constexpr unsigned int kHoldCtrl = 1;
  static constexpr unsigned int kHoldAlt = 2;
  static unsigned int HoldMask(bool ctrl, bool alt) {
    return (ctrl ? kHoldCtrl : 0) | (alt ? kHoldAlt : 0);
  }

  // True while a gesture is live (a HUD hidden by a pending cancel is not).
  static bool IsActive();
  // The keyboard hook's key-down path, while IsActive(): Esc cancels the
  // gesture. True when the key was consumed (swallow it).
  static bool HandleKeyDown(unsigned int virtual_key);
  // The hook's key-up path, while IsActive(): true when releasing
  // `virtual_key` lets go of one of the live gesture's hold keys.
  static bool ReleasesHold(unsigned int virtual_key);
  // Starts a gesture (when inactive) or steps the selection by
  // `direction`. With no hold key (`hold_mask` 0: a menu command, or a
  // chord without Ctrl and Alt) a new gesture commits its first step at
  // once, as the Mac's does.
  static void Cycle(PhiShellWindow* controller,
                    int direction,
                    unsigned int hold_mask);
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

// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_THEME_BUS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_THEME_BUS_H_

#include <windows.h>

#include <functional>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_theme.h"

namespace phi_client {

class PhiWindow;

// The client's OS-appearance listener and palette fan-out
// (docs/architecture-review.md §3.6; lane 3 ranked finding 7: "No OS
// theme-change path" — no WM_SETTINGCHANGE / ImmersiveColorSet handler
// exists anywhere in the tree today, so a live Windows dark/light switch
// only reaches the chrome when something else happens to call ApplyTheme,
// and an open Feedback window keeps its stale
// PhiPopupColors forever).
//
// The bus owns its OWN message-only window. It deliberately does not
// borrow the coordinator's (PhiCoordinatorMsg): that window carries the
// control-pipe verb marshal and a WM_TIMER catch-all branch, and the
// substrate must not depend on the coordinator being alive.
//
// WHAT IT LISTENS FOR
//   WM_SETTINGCHANGE with lParam == L"ImmersiveColorSet"  (app light/dark)
//   WM_SETTINGCHANGE with wParam == SPI_SETHIGHCONTRAST   (contrast theme)
//   WM_THEMECHANGED                                       (visual style)
//   WM_DWMCOLORIZATIONCOLORCHANGED                        (accent colour)
//   WM_SYSCOLORCHANGE                                     (system colours)
// Any of them re-reads PhiSystemPrefersDark() and PhiHighContrastActive()
// and, when either answer moved, broadcasts. Under a contrast theme the
// palette is the system colours themselves (phi_theme.h), so a
// WM_SYSCOLORCHANGE then broadcasts unconditionally.
//
// WHAT A BROADCAST DOES, in order:
//   1. every observer callback runs (see AddObserver below);
//   2. every registered PhiWindow gets NotifyThemeChanged(), which
//      re-reads its QueryPalette() and repaints.
// Observers run FIRST on purpose: the object that owns the app-wide
// palette (today PhiCoordinator::ApplyTheme, which recomputes and pushes
// to the controllers and Settings) must recompute before the windows read
// it back.
//
// HOW THE REST OF THE CLIENT SHOULD JOIN (phase 6 work, not done here):
//   * PhiCoordinator registers ONE observer in its constructor that calls
//     ApplyTheme(); that single line makes a live OS toggle repaint the
//     shells, the sidebar and Settings, and it replaces the registry probe
//     hidden inside PhiResolvePalette (phi_theme.cc:106-131) as the only
//     time the client notices the OS moved.
//   * The four secondary windows (prompt, feedback, settings, kiosk)
//     snapshot PhiPopupColorsNow() at construction today
//     (phi_prompt.cc:83, phi_feedback_window.cc:302) and never refresh.
//     Once they derive from PhiSecondaryWindow : PhiWindow they are
//     registered automatically and only need to override QueryPalette();
//     until then each can call AddObserver() in its constructor and
//     RemoveObserver() in its destructor, re-taking PhiPopupColorsNow()
//     and invalidating. PhiSetPopupPalette / PhiPopupColors keep working
//     unchanged either way — the bus does not own them.
class PhiThemeBus {
 public:
  // The UI thread's bus. Creates the message-only window on first use.
  static PhiThemeBus& Get();

  PhiThemeBus(const PhiThemeBus&) = delete;
  PhiThemeBus& operator=(const PhiThemeBus&) = delete;

  // PhiWindow::Create / WM_NCDESTROY call these; nothing else should.
  void Register(PhiWindow* window);
  void Unregister(PhiWindow* window);

  // A non-PhiWindow subscriber. Returns a token for RemoveObserver().
  // Observers run before the window fan-out.
  int AddObserver(std::function<void()> on_theme_changed);
  void RemoveObserver(int token);

  // Runs the observers and then every registered window's
  // NotifyThemeChanged(). Safe to call re-entrantly-ish: the lists are
  // copied before they are walked, and a window that unregisters during
  // the walk is skipped.
  void Broadcast();

  // True when Windows is currently on a dark app appearance, as of the
  // last OS notification (or the first read). Cheaper than
  // PhiSystemPrefersDark(), which opens the registry.
  bool system_prefers_dark() const { return system_dark_; }
  // True while a Windows contrast theme is on, as of the last notification.
  bool high_contrast() const { return high_contrast_; }

  // Test/diagnostic seam: pretend the OS just changed. Re-reads the
  // registry and broadcasts when the answer moved.
  void PollSystemAppearance();

 private:
  PhiThemeBus();
  // Private and never called (defined out of line, as chromium-style
  // requires): Get() leaks the one instance on purpose, so the bus
  // outlives every window and no exit-time destructor runs.
  ~PhiThemeBus();

  static LRESULT CALLBACK BusProc(HWND hwnd,
                                  UINT message,
                                  WPARAM wparam,
                                  LPARAM lparam);
  void EnsureWindow();

  struct Observer {
    int token = 0;
    std::function<void()> callback;
  };

  HWND hwnd_ = nullptr;
  bool system_dark_ = false;
  bool high_contrast_ = false;
  int next_token_ = 1;
  std::vector<Observer> observers_;
  // Back-pointers to windows that unregister themselves at WM_NCDESTROY.
  RAW_PTR_EXCLUSION std::vector<PhiWindow*> windows_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_THEME_BUS_H_

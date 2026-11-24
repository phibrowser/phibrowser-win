// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_UPDATER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_UPDATER_H_

#include <windows.h>

namespace phi_client {

// The Windows release notes: the phibrowser/phibrowser-win GitHub releases,
// newest first. Settings ▸ About's "What's new" row and Help ▸ What's new
// open it in a tab (PHI-2024, PHI-2217).
inline constexpr char kPhiReleaseNotesUrl[] =
    "https://github.com/phibrowser/phibrowser-win/releases/";

// The Windows update check — WinSparkle over the SAME appcast pipeline the
// Mac's Sparkle uses (ota.phibrowser.com/<channel>/appcast). WinSparkle.dll
// is vendored (third_party/winsparkle) and loaded DYNAMICALLY: a dev deploy
// without the DLL simply has no updater, it never fails to launch.
//
// The flow mirrors the Mac: background checks find an update and WinSparkle
// walks the user through download; the downloaded installer runs when the
// app quits and updates the per-user install in place (keeping a rollback
// snapshot — installer/phi-installer.nsi). WinSparkle launches the installer
// FIRST and then asks the app to quit — through the shutdown-request
// callback, and its default for that callback is nothing at all — so Init
// takes where to post the request: the installer's running-app wait
// (phi-installer.nsi) covers the seconds Chromium takes to unwind.
//
// The installer WinSparkle downloads is kept (PHI-2199, phi_update_cache.h):
// WinSparkle hands the verified file to us to run, we copy it into a per-user
// cache under the feed's version and EdDSA signature and start the copy.
// When that start fails (a cancelled UAC prompt), the next Update click or
// "Check for Update..." installs the cached copy instead of downloading the
// package again, and a launch with one cached shows the Update reminder at
// once. Every start re-verifies the file; the cache goes once this build is
// that version, the feed offers another one, or the user skips it.
class PhiUpdater {
 public:
  // Loads the DLL, configures the appcast + app identity + the feed's
  // EdDSA public key, and starts the background check schedule. Safe to
  // call once at startup, after the first window exists. No-op (with a log
  // line) when the DLL is absent — and when no signing key is built in: an
  // update that cannot be verified is never offered, whatever the build.
  // A --dev-hooks build honours the PHI_APPCAST_URL / PHI_APPCAST_ED25519
  // environment overrides for dev/test rings. `quit_msg` is posted to
  // `quit_hwnd` (from WinSparkle's own thread) once the installer is
  // running and the app must go: the handler closes every window through
  // the normal paths, the way the user's own quit does. `state_msg` is
  // posted to the same window whenever UpdateAvailable changes.
  static void Init(HWND quit_hwnd, UINT quit_msg, UINT state_msg);

  // "Check for Update..." and the Update reminder — the interactive check
  // with WinSparkle's UI, or, with an installer cached that the feed still
  // offers, that installer (UAC is the prompt). False when the updater is
  // unavailable (DLL missing, no key).
  static bool CheckForUpdateWithUi();

  // True when the DLL loads and a signing key is built in (drives the menu
  // item's enabled state).
  static bool Available();

  // A check found an update the user has neither installed nor skipped:
  // the windows show the Update reminder (the Mac's upgrade button). Any
  // thread.
  static bool UpdateAvailable();

  // win_sparkle_cleanup on shutdown.
  static void Shutdown();
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_UPDATER_H_

// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_updater.h"

#include <windows.h>

#include <atomic>
#include <string>

#include "base/logging.h"
#include "client/phi_features.h"
#include "client/phi_version.h"

namespace phi_client {

namespace {

// The win-nightly update feed beside the Mac's mac-nightly. A --dev-hooks
// build may override it with the PHI_APPCAST_URL environment variable; a
// release build cannot.
constexpr char kDefaultAppcastUrl[] =
    "https://ota.phibrowser.com/win-nightly/appcast/PhiBrowserWinUpdate.xml";

// The appcast's EdDSA (Ed25519) public key, base64 — the counterpart of the
// key installer/make-appcast.sh signs with. The matching private key is kept
// outside the client repository in phibrowser-win-build. A signed payload is
// required whatever the transport.
constexpr char kEdDsaPublicKey[] =
    "D4xwuYEnKtPEo0h+VBKzG9vioEpcu4BHY8FxALF3vfk=";

// The WinSparkle C surface we use, resolved by hand: the DLL is optional.
using SetAppcastUrlFn = void(__cdecl*)(const char*);
using SetAppDetailsFn = void(__cdecl*)(const wchar_t*,
                                       const wchar_t*,
                                       const wchar_t*);
using SetEdDsaKeyFn = int(__cdecl*)(const char*);
using SetAutomaticCheckFn = void(__cdecl*)(int);
using VoidFn = void(__cdecl*)();
using CanShutdownFn = int(__cdecl*)();
using SetCanShutdownFn = void(__cdecl*)(CanShutdownFn);
using SetShutdownRequestFn = void(__cdecl*)(VoidFn);
using SetCallbackFn = void(__cdecl*)(VoidFn);

struct Sparkle {
  HMODULE dll = nullptr;
  SetAppcastUrlFn set_appcast_url = nullptr;
  SetAppDetailsFn set_app_details = nullptr;
  SetEdDsaKeyFn set_eddsa_public_key = nullptr;
  SetAutomaticCheckFn set_automatic_check = nullptr;
  SetCanShutdownFn set_can_shutdown = nullptr;
  SetShutdownRequestFn set_shutdown_request = nullptr;
  VoidFn init = nullptr;
  VoidFn check_update_with_ui = nullptr;
  VoidFn cleanup = nullptr;
  // The update-state callbacks (WinSparkle 0.5 / 0.8): optional — without
  // them the updater still works, there is just no in-window reminder.
  SetCallbackFn set_did_find_update = nullptr;
  SetCallbackFn set_did_not_find_update = nullptr;
  SetCallbackFn set_update_skipped = nullptr;
};

Sparkle g_sparkle;
bool g_initialized = false;
// Where the shutdown request goes (Init's arguments). Read from WinSparkle's
// thread, so set before init and never changed.
HWND g_quit_hwnd = nullptr;
UINT g_quit_msg = 0;
UINT g_state_msg = 0;
// An update is on offer (the Mac's sparkleDidDownloadUpdate .. skip): set
// from WinSparkle's thread, read by the windows' layout.
std::atomic<bool> g_update_available{false};

// WinSparkle's "may I close you?" — always yes: Chromium has no unsaved
// documents, and the close path itself saves the session.
int __cdecl CanShutdown() {
  return 1;
}

// WinSparkle has launched the installer (from its own thread): hand the
// quit to the UI thread. WITHOUT this callback WinSparkle does nothing and
// the installer meets a running Phi (its default RequestShutdown is empty).
void __cdecl RequestShutdown() {
  LOG(INFO) << "[PhiUpdater] installer launched; requesting quit";
  ::PostMessageW(g_quit_hwnd, g_quit_msg, 0, 0);
}

// A check found an update, found none, or the user skipped the one it
// found (all from WinSparkle's thread). "Remind me later" and closing the
// dialog leave the reminder up — that is what it is for.
void SetUpdateAvailable(bool available) {
  if (g_update_available.exchange(available) != available) {
    LOG(INFO) << "[PhiUpdater] update available=" << available;
    ::PostMessageW(g_quit_hwnd, g_state_msg, 0, 0);
  }
}
void __cdecl DidFindUpdate() {
  SetUpdateAvailable(true);
}
void __cdecl DidNotFindUpdate() {
  SetUpdateAvailable(false);
}

// WinSparkle.dll sits beside phi_client.exe (the sync and the installer both
// place it there). An absolute path keeps the search-order honest — no
// PATH/current-directory lookup for a DLL we execute.
std::wstring DllPath() {
  wchar_t exe[MAX_PATH] = {};
  const DWORD length = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return std::wstring();
  }
  std::wstring path(exe);
  const size_t slash = path.find_last_of(L'\\');
  if (slash == std::wstring::npos) {
    return std::wstring();
  }
  path.resize(slash + 1);
  path += L"WinSparkle.dll";
  return path;
}

bool Load() {
  if (g_sparkle.dll) {
    return true;
  }
  const std::wstring path = DllPath();
  if (path.empty()) {
    return false;
  }
  HMODULE dll = ::LoadLibraryExW(path.c_str(), nullptr,
                                 LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!dll) {
    LOG(INFO) << "[PhiUpdater] WinSparkle.dll not present; updater off";
    return false;
  }
  auto proc = [dll](const char* name) {
    return ::GetProcAddress(dll, name);
  };
  Sparkle sparkle;
  sparkle.dll = dll;
  sparkle.set_appcast_url = reinterpret_cast<SetAppcastUrlFn>(
      proc("win_sparkle_set_appcast_url"));
  sparkle.set_app_details = reinterpret_cast<SetAppDetailsFn>(
      proc("win_sparkle_set_app_details"));
  sparkle.set_eddsa_public_key = reinterpret_cast<SetEdDsaKeyFn>(
      proc("win_sparkle_set_eddsa_public_key"));
  sparkle.set_automatic_check = reinterpret_cast<SetAutomaticCheckFn>(
      proc("win_sparkle_set_automatic_check_for_updates"));
  sparkle.set_can_shutdown = reinterpret_cast<SetCanShutdownFn>(
      proc("win_sparkle_set_can_shutdown_callback"));
  sparkle.set_shutdown_request = reinterpret_cast<SetShutdownRequestFn>(
      proc("win_sparkle_set_shutdown_request_callback"));
  sparkle.init = reinterpret_cast<VoidFn>(proc("win_sparkle_init"));
  sparkle.check_update_with_ui = reinterpret_cast<VoidFn>(
      proc("win_sparkle_check_update_with_ui"));
  sparkle.cleanup = reinterpret_cast<VoidFn>(proc("win_sparkle_cleanup"));
  sparkle.set_did_find_update = reinterpret_cast<SetCallbackFn>(
      proc("win_sparkle_set_did_find_update_callback"));
  sparkle.set_did_not_find_update = reinterpret_cast<SetCallbackFn>(
      proc("win_sparkle_set_did_not_find_update_callback"));
  sparkle.set_update_skipped = reinterpret_cast<SetCallbackFn>(
      proc("win_sparkle_set_update_skipped_callback"));
  if (!sparkle.set_appcast_url || !sparkle.set_app_details ||
      !sparkle.set_eddsa_public_key || !sparkle.set_can_shutdown ||
      !sparkle.set_shutdown_request || !sparkle.init ||
      !sparkle.check_update_with_ui || !sparkle.cleanup) {
    LOG(ERROR) << "[PhiUpdater] WinSparkle.dll is missing exports; ignored";
    ::FreeLibrary(dll);
    return false;
  }
  g_sparkle = sparkle;
  return true;
}

// The PHI_APPCAST_URL / PHI_APPCAST_ED25519 overrides exist ONLY in a
// --dev-hooks build. Together they replace the update feed and the key that
// signs it, so in a release exe any process that can set an environment
// variable for us could hand us its own "update"; there the built-in values
// are the only values (docs/architecture-review/lane7-cross-cutting.md §6).
std::string AppcastUrl() {
  if constexpr (kPhiDevHooks) {
    char value[1024] = {};
    const DWORD length =
        ::GetEnvironmentVariableA("PHI_APPCAST_URL", value, sizeof(value));
    if (length > 0 && length < sizeof(value)) {
      LOG(INFO) << "[PhiUpdater] appcast override: " << value;
      return std::string(value, length);
    }
  }
  return kDefaultAppcastUrl;
}

// The built-in key, or in a --dev-hooks build the PHI_APPCAST_ED25519
// override (signature testing against a dev feed). Empty means no updater.
std::string EdDsaPublicKey() {
  if constexpr (kPhiDevHooks) {
    char value[128] = {};
    const DWORD length =
        ::GetEnvironmentVariableA("PHI_APPCAST_ED25519", value, sizeof(value));
    if (length > 0 && length < sizeof(value)) {
      LOG(INFO) << "[PhiUpdater] appcast signing key override in effect";
      return std::string(value, length);
    }
  }
  return kEdDsaPublicKey;
}

}  // namespace

// static
void PhiUpdater::Init(HWND quit_hwnd, UINT quit_msg, UINT state_msg) {
  if (g_initialized) {
    return;
  }
  g_quit_hwnd = quit_hwnd;
  g_quit_msg = quit_msg;
  g_state_msg = state_msg;
  // No key, no updater — in every build. A feed that cannot be verified is
  // never fetched, let alone installed; the menu item goes dark
  // (Available) rather than offering an unsigned download.
  const std::string eddsa_key = EdDsaPublicKey();
  if (eddsa_key.empty()) {
    LOG(ERROR) << "[PhiUpdater] no appcast signing key built in; updater off";
    return;
  }
  if (!Load()) {
    return;
  }
  // The key goes in BEFORE the feed is named and before init, so there is
  // no window in which WinSparkle could check keyless; a rejected key keeps
  // it uninitialised for the same reason.
  if (!g_sparkle.set_eddsa_public_key(eddsa_key.c_str())) {
    LOG(ERROR) << "[PhiUpdater] EdDSA public key rejected; updater off";
    return;
  }
  const std::string url = AppcastUrl();
  g_sparkle.set_appcast_url(url.c_str());
  // Not a copyright notice: WinSparkle keeps its settings under
  // HKCU\Software\<company>\<app>, so renaming it would orphan existing
  // installs' update settings (the uninstaller deletes this key).
  g_sparkle.set_app_details(L"Fyde Innovations", L"Phi Browser",
                            PHI_WIN_VERSION_FULL_WIDE);
  if (g_sparkle.set_automatic_check) {
    g_sparkle.set_automatic_check(1);  // Default daily background checks.
  }
  // The quit hand-off, before init: an update found by the first check
  // must already know how to close us.
  g_sparkle.set_can_shutdown(&CanShutdown);
  g_sparkle.set_shutdown_request(&RequestShutdown);
  // The reminder's state, before init for the same reason: the first
  // background check may find an update.
  if (g_sparkle.set_did_find_update && g_sparkle.set_did_not_find_update &&
      g_sparkle.set_update_skipped) {
    g_sparkle.set_did_find_update(&DidFindUpdate);
    g_sparkle.set_did_not_find_update(&DidNotFindUpdate);
    g_sparkle.set_update_skipped(&DidNotFindUpdate);
  }
  g_sparkle.init();
  g_initialized = true;
  LOG(INFO) << "[PhiUpdater] init v" << PHI_WIN_VERSION_FULL_STRING
            << " appcast=" << url;
}

// static
bool PhiUpdater::CheckForUpdateWithUi() {
  if (!g_initialized) {
    return false;  // Never initialised: no DLL, no key.
  }
  g_sparkle.check_update_with_ui();
  return true;
}

// static
bool PhiUpdater::UpdateAvailable() {
  return g_update_available.load();
}

// static
bool PhiUpdater::Available() {
  return !EdDsaPublicKey().empty() && (g_sparkle.dll != nullptr || Load());
}

// static
void PhiUpdater::Shutdown() {
  if (g_initialized && g_sparkle.cleanup) {
    g_sparkle.cleanup();
    g_initialized = false;
  }
}

}  // namespace phi_client

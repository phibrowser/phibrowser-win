// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_updater.h"

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <optional>
#include <string>

#include "base/logging.h"
#include "client/phi_features.h"
#include "client/phi_http.h"
#include "client/phi_update_cache.h"
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
using RunInstallerFn = int(__cdecl*)(const wchar_t*);
using SetRunInstallerFn = void(__cdecl*)(RunInstallerFn);
// winsparkle.h's WINSPARKLE_RETURN_ERROR: "Failed to launch the installer."
constexpr int kSparkleReturnError = -1;

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
  // WinSparkle 0.8: the verified download is handed to us to run, which
  // is where it is kept for the next attempt (PHI-2199). Optional too:
  // without it every attempt downloads again, as before.
  SetRunInstallerFn set_user_run_installer = nullptr;
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
// An install from the cache is in flight (its worker thread is up).
std::atomic<bool> g_cached_install_busy{false};
// The updater's own message-only window, for the worker threads' hand-back
// to the UI thread (WinSparkle's own window wants the UI thread).
HWND g_updater_hwnd = nullptr;
constexpr UINT WM_PHI_UPDATER_SPARKLE_UI = WM_APP + 1;

// The architecture tag WinSparkle matches enclosures against.
#if defined(_M_ARM64)
constexpr char kOsTag[] = "windows-arm64";
#else
constexpr char kOsTag[] = "windows-x64";
#endif

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
void __cdecl DidNotFindUpdate();
void __cdecl UpdateSkipped();

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
  sparkle.set_user_run_installer = reinterpret_cast<SetRunInstallerFn>(
      proc("win_sparkle_set_user_run_installer_callback"));
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

// %LOCALAPPDATA%\Phi\Update Cache — per user, beside the default User Data
// folder; or in a --dev-hooks build PHI_UPDATE_CACHE_DIR, so a test build
// never shares the installed copy's cache.
std::wstring CacheDir() {
  if constexpr (kPhiDevHooks) {
    wchar_t value[MAX_PATH] = {};
    const DWORD length =
        ::GetEnvironmentVariableW(L"PHI_UPDATE_CACHE_DIR", value, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
      return std::wstring(value, length);
    }
  }
  PWSTR local = nullptr;
  std::wstring dir;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                       &local))) {
    dir = std::wstring(local) + L"\\Phi\\Update Cache";
  }
  ::CoTaskMemFree(local);
  return dir;
}

PhiUpdateCache Cache() {
  return PhiUpdateCache(CacheDir());
}

// The enclosure the feed offers this build now. BLOCKING (a worker or
// WinSparkle's thread). Empty when the feed cannot be read.
std::optional<PhiUpdateEnclosure> FetchOffer() {
  constexpr size_t kMaxFeedBytes = 1024 * 1024;
  const PhiHttpResult result =
      PhiHttpRequest(L"GET", AppcastUrl(), L"", "", kMaxFeedBytes);
  if (!result.transport_ok || result.status != 200) {
    LOG(INFO) << "[PhiUpdater] feed not read (status " << result.status
              << ")";
    return std::nullopt;
  }
  return PhiParseAppcast(result.body, kOsTag);
}

// Starts the installer the way WinSparkle does (ShellExecuteEx, so the
// installer's manifest brings up UAC), while `verified` keeps the checked
// bytes locked. False on any failure, a cancelled UAC prompt included.
bool LaunchInstaller(const std::wstring& file, HANDLE verified, HWND owner) {
  SHELLEXECUTEINFOW sei = {};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
  sei.hwnd = owner;
  sei.lpFile = file.c_str();
  sei.nShow = SW_SHOWDEFAULT;
  const bool ok = ::ShellExecuteExW(&sei) != FALSE;
  const DWORD error = ok ? 0 : ::GetLastError();
  ::CloseHandle(verified);
  if (ok) {
    LOG(INFO) << "[PhiUpdater] cached installer started";
  } else {
    LOG(INFO) << "[PhiUpdater] cached installer not started (error " << error
              << (error == ERROR_CANCELLED ? ", UAC cancelled" : "")
              << "); it stays cached";
  }
  return ok;
}

// The cached installer, verified and still worth installing (newer than
// this build), locked open; INVALID_HANDLE_VALUE (and the cache cleared)
// otherwise.
HANDLE OpenUsableCache(const PhiUpdateCache& cache,
                       const PhiCachedUpdate& cached) {
  if (PhiCompareUpdateVersions(cached.version, PHI_WIN_VERSION_FULL_STRING) <=
      0) {
    LOG(INFO) << "[PhiUpdater] cached installer " << cached.version
              << " is installed or older; removed";
    cache.Clear();
    return INVALID_HANDLE_VALUE;
  }
  HANDLE verified = PhiOpenVerifiedUpdate(cached.file, cached.length,
                                          cached.signature, EdDsaPublicKey());
  if (verified == INVALID_HANDLE_VALUE) {
    LOG(ERROR) << "[PhiUpdater] cached installer " << cached.version
               << " failed verification; removed";
    cache.Clear();
  }
  return verified;
}

// WinSparkle downloaded and verified the installer and wants it run (its
// UI thread). Keep a copy under the feed's version and signature, start
// the COPY (re-verified), and say we ran it: 1 lets WinSparkle close its
// window and ask us to quit, kSparkleReturnError puts up its "Failed to
// launch" box and the copy stays for the next attempt. 0 hands the file
// back to WinSparkle unchanged — the feed could not be read or does not
// describe this file, so there is nothing to cache it under.
int __cdecl RunInstaller(const wchar_t* path) {
  const std::wstring source = path ? path : L"";
  if (source.size() < 4 ||
      _wcsicmp(source.c_str() + source.size() - 4, L".exe") != 0) {
    return 0;
  }
  const std::optional<PhiUpdateEnclosure> offer = FetchOffer();
  if (!offer || offer->signature.empty()) {
    LOG(INFO) << "[PhiUpdater] no signed offer to cache the download under";
    return 0;
  }
  PhiUpdateCache cache = Cache();
  if (!cache.Store(source, *offer)) {
    return 0;
  }
  const std::optional<PhiCachedUpdate> cached = cache.Read();
  if (!cached) {
    cache.Clear();
    return 0;
  }
  HANDLE verified = PhiOpenVerifiedUpdate(cached->file, cached->length,
                                          cached->signature, EdDsaPublicKey());
  if (verified == INVALID_HANDLE_VALUE) {
    // The feed changed between WinSparkle's read and ours.
    LOG(INFO) << "[PhiUpdater] download does not match the feed now; "
                 "not cached";
    cache.Clear();
    return 0;
  }
  LOG(INFO) << "[PhiUpdater] installer " << cached->version << " cached";
  return LaunchInstaller(cached->file, verified, ::GetForegroundWindow())
             ? 1
             : kSparkleReturnError;
}

void PostSparkleUi() {
  ::PostMessageW(g_updater_hwnd, WM_PHI_UPDATER_SPARKLE_UI, 0, 0);
}

// The Update pill or "Check for Update..." with an installer cached (a
// worker thread: the feed read and the hash of a large file are slow).
// The feed decides first: when it now offers another version or other
// bytes, the cache is stale and WinSparkle takes over. Offline, the cached
// installer still installs — it is verified and newer than this build.
DWORD WINAPI InstallCachedThread(void* param) {
  const HWND owner = static_cast<HWND>(param);
  PhiUpdateCache cache = Cache();
  std::optional<PhiCachedUpdate> cached = cache.Read();
  HANDLE verified = INVALID_HANDLE_VALUE;
  if (cached) {
    if (const std::optional<PhiUpdateEnclosure> offer = FetchOffer()) {
      if (offer->version != cached->version ||
          offer->signature != cached->signature) {
        LOG(INFO) << "[PhiUpdater] cached installer " << cached->version
                  << " superseded by " << offer->version << "; removed";
        cache.Clear();
        cached.reset();
      }
    }
    if (cached) {
      verified = OpenUsableCache(cache, *cached);
    }
  }
  if (verified == INVALID_HANDLE_VALUE) {
    PostSparkleUi();  // Nothing usable cached: WinSparkle's own flow.
  } else {
    LOG(INFO) << "[PhiUpdater] installing cached " << cached->version
              << " (no download)";
    if (LaunchInstaller(cached->file, verified, owner)) {
      RequestShutdown();  // What WinSparkle does after its own launch.
    }
  }
  g_cached_install_busy = false;
  return 0;
}

// At start-up (a worker thread): drop an installer this build already is
// (the update went in) or one that no longer verifies; a good one brings
// the Update reminder up at once, before the next feed check.
DWORD WINAPI CheckCacheThread(void*) {
  PhiUpdateCache cache = Cache();
  if (const std::optional<PhiCachedUpdate> cached = cache.Read()) {
    HANDLE verified = OpenUsableCache(cache, *cached);
    if (verified != INVALID_HANDLE_VALUE) {
      ::CloseHandle(verified);
      LOG(INFO) << "[PhiUpdater] installer " << cached->version
                << " is cached and verified";
      SetUpdateAvailable(true);
    }
  } else {
    cache.Clear();  // A half-written or foreign folder.
  }
  return 0;
}

void __cdecl DidNotFindUpdate() {
  // The feed no longer offers anything newer: a cached installer is moot.
  Cache().Clear();
  SetUpdateAvailable(false);
}

void __cdecl UpdateSkipped() {
  Cache().Clear();  // The user does not want this version.
  SetUpdateAvailable(false);
}

LRESULT CALLBACK UpdaterWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_PHI_UPDATER_SPARKLE_UI) {
    if (g_initialized) {
      g_sparkle.check_update_with_ui();
    }
    return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void CreateUpdaterWindow() {
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &UpdaterWndProc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = L"PhiUpdaterMessages";
  ::RegisterClassExW(&wc);
  g_updater_hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                     HWND_MESSAGE, nullptr, wc.hInstance,
                                     nullptr);
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
  // WinSparkle keeps its state under HKCU\Software\<company>\<app>; the
  // installer drops the key of the old company name.
  g_sparkle.set_app_details(L"Phinomenon Inc.", L"Phi Browser",
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
    g_sparkle.set_update_skipped(&UpdateSkipped);
  }
  // Keep the verified download for the next attempt (PHI-2199).
  if (g_sparkle.set_user_run_installer) {
    g_sparkle.set_user_run_installer(&RunInstaller);
  }
  CreateUpdaterWindow();
  g_sparkle.init();
  g_initialized = true;
  if (g_sparkle.set_user_run_installer) {
    if (HANDLE thread =
            ::CreateThread(nullptr, 0, &CheckCacheThread, nullptr, 0, nullptr)) {
      ::CloseHandle(thread);
    }
  }
  LOG(INFO) << "[PhiUpdater] init v" << PHI_WIN_VERSION_FULL_STRING
            << " appcast=" << url;
}

// static
bool PhiUpdater::CheckForUpdateWithUi() {
  if (!g_initialized) {
    return false;  // Never initialised: no DLL, no key.
  }
  // An installer from an earlier attempt: install it rather than download
  // it again (PHI-2199). The worker checks the feed and the file first.
  if (g_sparkle.set_user_run_installer && Cache().Read()) {
    if (g_cached_install_busy.exchange(true)) {
      return true;  // The first click is still at it.
    }
    if (HANDLE thread = ::CreateThread(nullptr, 0, &InstallCachedThread,
                                       ::GetForegroundWindow(), 0, nullptr)) {
      ::CloseHandle(thread);
      return true;
    }
    g_cached_install_busy = false;
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

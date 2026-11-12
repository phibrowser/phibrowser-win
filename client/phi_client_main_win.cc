// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.
//
// phi_client.dll's entry points. The prebuilt Phi.exe host owns process
// startup and loads this DLL in the browser process only, so client code
// stays out of the sandboxed children. The host forwards the framework's
// PhiClient* lookups to the exports below.

#include <windows.h>

#include <cstdint>
#include <string>

#include "base/command_line.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "content/public/common/content_switches.h"
#include "client/phi_auth.h"
#include "client/phi_client_logging.h"
#include "client/phi_coordinator.h"
#include "client/phi_startup_profile.h"

namespace {

// The user data dir the host resolved, held from PhiClientInitialize.
std::wstring& UserDataDir() {
  static base::NoDestructor<std::wstring> dir;
  return *dir;
}

}  // namespace

// The contract this DLL was built against; the host checks it before calling
// anything else here.
extern "C" __declspec(dllexport) uint32_t PhiClientAbiVersion() {
#if defined(PHI_DEV_HOOKS)
  // suite-hostsplit's stand-in for a client built against another framework.
  // One shot: the marker beside the exe is deleted first.
  std::wstring marker(32768, L'\0');
  marker.resize(::GetModuleFileNameW(nullptr, marker.data(),
                                     static_cast<DWORD>(marker.size())));
  marker.resize(marker.find_last_of(L'\\') + 1);
  marker += L"phi-dev-abi-mismatch";
  if (::DeleteFileW(marker.c_str())) {
    return phi_framework::kPhiFrameworkAbiVersion + 1;
  }
#endif
  return phi_framework::kPhiFrameworkAbiVersion;
}

// Called once by the host, before ChromeMain, with the user data dir it
// resolved (--user-data-dir, the installer's Software\\Phi\\UserDataDir, the
// default). A null or empty dir means the host could not resolve one.
extern "C" __declspec(dllexport) void PhiClientInitialize(
    const wchar_t* user_data_dir) {
  UserDataDir() = user_data_dir ? user_data_dir : L"";
  base::CommandLine::Init(0, nullptr);
  phi_client::InitEarlyClientLogging();

  // The file log the feedback zip picks up, warnings and up unless the launch
  // names its own logging: AttachClientLoggingToBrowserLog reads these from
  // this module's command line. chrome.dll applies the same default
  // (ChromeMainDelegate::BasicStartupComplete).
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (!command_line->HasSwitch(switches::kEnableLogging)) {
    command_line->AppendSwitch(switches::kEnableLogging);
    if (!command_line->HasSwitch(switches::kLoggingLevel)) {
      command_line->AppendSwitchASCII(switches::kLoggingLevel, "1");
    }
  }
}

#if defined(PHI_DEV_HOOKS)
namespace {

// suite-hostsplit's fault inside this module, on the browser's UI thread. One
// shot: the marker is deleted first, so the next launch starts normally.
void CrashIfDevMarkerPresent() {
  if (::DeleteFileW((UserDataDir() + L"\\phi-dev-crash-client").c_str())) {
    *static_cast<volatile int*>(nullptr) = 1;
  }
}

}  // namespace
#endif  // defined(PHI_DEV_HOOKS)

// Called once by the framework to obtain the client delegate. Leaked
// deliberately: it must outlive ChromeMain, which never returns in normal
// operation.
extern "C" __declspec(dllexport)
phi_framework::PhiFrameworkDelegate* PhiClientCreateDelegate() {
#if defined(PHI_DEV_HOOKS)
  CrashIfDevMarkerPresent();
#endif
  // The coordinator is the composition root; the delegate the framework
  // talks to is the member it owns (docs/architecture-review.md §3.4), so
  // the leak keeps both alive.
  auto* coordinator = new phi_client::PhiCoordinator();
  return &coordinator->framework_dispatcher();
}

// Chromium sync's server (--sync-url), resolved by the framework at startup
// (ChromeMainDelegate::BasicStartupComplete) — the Mac's ChromiumLauncher
// passes the same switch. It follows the Auth0 tenant, so a token and the
// server it is sent to always belong together.
extern "C" __declspec(dllexport) const char* PhiClientSyncServerUrl() {
  static const base::NoDestructor<std::string> url(
      phi_client::PhiAuth::SyncServerUrl());
  return url->c_str();
}

// The profile the browser process starts in, asked by the host and the
// framework at startup: the default Space's profile as the client last
// recorded it (phi_startup_profile.h), "" for Chromium to use "Default".
// Resolved from the same user data dir Chromium will use (--user-data-dir,
// the installer's Software\Phi\UserDataDir, the default).
extern "C" __declspec(dllexport) const char* PhiClientStartupProfile() {
  static const base::NoDestructor<std::string> profile([] {
    return UserDataDir().empty()
               ? std::string()
               : phi_client::PhiReadStartupProfile(UserDataDir());
  }());
  return profile->c_str();
}

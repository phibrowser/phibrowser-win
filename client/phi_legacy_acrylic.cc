// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_legacy_acrylic.h"

#include <cstdint>

#include "base/command_line.h"

namespace phi_client {

namespace {

// user32's SetWindowCompositionAttribute, undocumented but stable since
// Windows 10 1803; every acrylic-on-Windows-10 app goes through it.
enum : DWORD { kWcaAccentPolicy = 19 };
enum : int {
  kAccentDisabled = 0,
  kAccentEnableAcrylicBlurBehind = 4,
};
// GradientColor is AABBGGRR and only applies with this flag.
constexpr int kAccentFlagGradientColor = 2;

struct AccentPolicy {
  int accent_state;
  int accent_flags;
  DWORD gradient_color;
  int animation_id;
};

struct CompositionAttributeData {
  DWORD attribute;
  void* data;
  SIZE_T size;
};

using SetWindowCompositionAttributeFn =
    BOOL(WINAPI*)(HWND, CompositionAttributeData*);

SetWindowCompositionAttributeFn CompositionSetter() {
  static const SetWindowCompositionAttributeFn setter = [] {
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    return user32 ? reinterpret_cast<SetWindowCompositionAttributeFn>(
                        ::GetProcAddress(user32,
                                         "SetWindowCompositionAttribute"))
                  : nullptr;
  }();
  return setter;
}

DWORD OsBuild() {
  static const DWORD build = [] {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW info = {sizeof(info)};
    if (HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll")) {
      if (auto fn = reinterpret_cast<RtlGetVersionFn>(
              ::GetProcAddress(ntdll, "RtlGetVersion"))) {
        if (fn(&info) == 0) {
          return info.dwBuildNumber;
        }
      }
    }
    return DWORD{0};
  }();
  return build;
}

// The neutral the blur settles to under the theme tint, at a quarter: the
// shell's own tint (Sidebar transparency) does the rest, so at 0x40 the
// accent path reads close to Windows 11's desktop acrylic (compared side
// by side on gmk, 2026-11-13).
constexpr DWORD kDarkNeutral = 0x40202020;   // AABBGGRR
constexpr DWORD kLightNeutral = 0x40F3F3F3;  // AABBGGRR

}  // namespace

bool PhiLegacyAcrylicSupported() {
  return OsBuild() >= 17134 && CompositionSetter() != nullptr;
}

bool PhiSetLegacyAcrylic(HWND hwnd, bool enabled, bool dark) {
  const SetWindowCompositionAttributeFn setter = CompositionSetter();
  if (!hwnd || !setter || (enabled && !PhiLegacyAcrylicSupported())) {
    return false;
  }
  AccentPolicy policy = {};
  if (enabled) {
    policy.accent_state = kAccentEnableAcrylicBlurBehind;
    policy.accent_flags = kAccentFlagGradientColor;
    policy.gradient_color = dark ? kDarkNeutral : kLightNeutral;
  } else {
    policy.accent_state = kAccentDisabled;
  }
  CompositionAttributeData data = {kWcaAccentPolicy, &policy, sizeof(policy)};
  return setter(hwnd, &data) != FALSE;
}

bool PhiForceLegacyAcrylic() {
#if defined(PHI_DEV_HOOKS)
  static const bool forced =
      base::CommandLine::ForCurrentProcess()->HasSwitch("phi-legacy-acrylic");
  return forced;
#else
  return false;
#endif
}

}  // namespace phi_client

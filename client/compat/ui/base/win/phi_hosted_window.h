// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_BASE_WIN_PHI_HOSTED_WINDOW_H_
#define UI_BASE_WIN_PHI_HOSTED_WINDOW_H_

#include <windows.h>

namespace ui::win {

// The native client owns these properties on each adopted Browser HWND.
// Unlike WS_VISIBLE, presentation excludes outgoing/parked animation children.
// Properties cross the exe/DLL boundary without sharing C++ allocations.
inline constexpr wchar_t kPhiHostedShell[] = L"Phi.HostedShell";
inline constexpr wchar_t kPhiHostedPresented[] = L"Phi.HostedPresented";
inline constexpr wchar_t kPhiShellFullscreen[] = L"Phi.ShellFullscreen";
// Borrowed WINDOWPLACEMENT owned by the shell host, valid while fullscreen.
inline constexpr wchar_t kPhiShellRestoredPlacement[] =
    L"Phi.RestoredPlacement";

inline HWND GetPhiHostedShell(HWND browser) {
  return browser ? reinterpret_cast<HWND>(::GetPropW(browser, kPhiHostedShell))
                 : nullptr;
}

inline bool IsPhiHostedWindow(HWND browser) {
  return GetPhiHostedShell(browser) != nullptr;
}

inline bool IsPhiHostedWindowPresented(HWND browser) {
  const HWND shell = GetPhiHostedShell(browser);
  return !shell ||
         (::IsWindow(shell) && ::GetPropW(browser, kPhiHostedPresented) &&
          ::IsWindowVisible(shell) && !::IsIconic(shell));
}

inline bool IsPhiHostedWindowActive(HWND browser) {
  const HWND shell = GetPhiHostedShell(browser);
  return !shell ||
         (IsPhiHostedWindowPresented(browser) &&
          ::GetAncestor(::GetForegroundWindow(), GA_ROOTOWNER) == shell);
}

inline void SetPhiHostedWindow(HWND browser, HWND shell, bool presented) {
  if (shell) {
    ::SetPropW(browser, kPhiHostedShell, reinterpret_cast<HANDLE>(shell));
  } else {
    ::RemovePropW(browser, kPhiHostedShell);
  }
  if (shell && presented) {
    ::SetPropW(browser, kPhiHostedPresented, reinterpret_cast<HANDLE>(1));
  } else {
    ::RemovePropW(browser, kPhiHostedPresented);
  }
}

}  // namespace ui::win

#endif  // UI_BASE_WIN_PHI_HOSTED_WINDOW_H_

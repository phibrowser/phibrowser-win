// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_keyboard_accelerators.h"

#include <array>

namespace phi_client {
PhiKeyboardCommandQueue::PhiKeyboardCommandQueue() = default;
PhiKeyboardCommandQueue::~PhiKeyboardCommandQueue() = default;
}  // namespace phi_client

#if defined(_WIN32)

#include "phibrowser-win/client/phi_app_menu.h"
#include "phibrowser-win/client/phi_preferences.h"
#include "phibrowser-win/client/phi_tab_switcher.h"
#include "phibrowser-win/client/phi_shell_window.h"

namespace phi_client {

namespace {
// The one accelerator service, reachable from the static hook. The client
// creates exactly one for the process lifetime.
PhiKeyboardAccelerators* g_accelerators = nullptr;

// The FIXED shortcuts, reserved the way Chrome reserves its browser
// shortcuts (they fire even while a page has focus): Quit (the Mac locks ⌘Q
// the same way), F5, the app menu's keys and the Ctrl+1..9 Space switch.
// Edit keys (Ctrl+C/V/X/Z/A...) stay with the web content; everything a
// user can rebind — Open File, Print, Find, zoom, View Source, DevTools,
// Back / Forward / Home, Settings and the rest — lives in the preferences'
// rebindable table, not here.
constexpr PhiKeyboardAccelerators::Accelerator kAccelerators[] = {
    {'Q', true, true, false, kCmdQuit},
    {VK_F5, false, false, false, kCmdReload},
    // F11 deliberately unbound — see the menu bar's IDC_FULLSCREEN note.
    // The numeric keypad's zoom twins; Ctrl++ / Ctrl+- themselves are the
    // rebindable zoomIn / zoomOut rows.
    {VK_ADD, true, false, false, kCmdZoomIn},
    {VK_SUBTRACT, true, false, false, kCmdZoomOut},
    // The app menu (docs/windows-native-ui.md §3.3): F10, Alt+F, Alt+E
    // (Chrome's and Edge's mnemonics). The solo Alt tap posts the same
    // command from OnKey.
    {VK_F10, false, false, false, kCmdShowAppMenu},
    {'F', false, false, true, kCmdShowAppMenu},
    {'E', false, false, true, kCmdShowAppMenu},
    // Ctrl+Tab / Ctrl+Shift+Tab are handled in OnKey — the MRU switcher.
    // Ctrl+1..9 switches Spaces (the Mac's ⌃1..⌃9; the Spaces menu and
    // the pips' hover cards advertise these).
    {'1', true, false, false, kCmdSpace1},
    {'2', true, false, false, kCmdSpace2},
    {'3', true, false, false, kCmdSpace3},
    {'4', true, false, false, kCmdSpace4},
    {'5', true, false, false, kCmdSpace5},
    {'6', true, false, false, kCmdSpace6},
    {'7', true, false, false, kCmdSpace7},
    {'8', true, false, false, kCmdSpace8},
    {'9', true, false, false, kCmdSpace9},
};

// AltGr reaches the app as LEFT Ctrl + RIGHT Alt, so Polish AltGr+N (ń) or
// US-International AltGr+R (®) reads as the Ctrl+Alt+N / Ctrl+Alt+R chords.
// True when Right Alt is down and the focused thread's layout types
// something for `vk` at that shift state: the key is text, not a chord.
// A layout with no AltGr plane (US) answers 0, so Ctrl+Alt chords still
// work there, and Left Ctrl + Left Alt never asks.
bool IsAltGrCharacter(UINT vk, LPARAM lparam, bool shift) {
  if ((::GetKeyState(VK_RMENU) & 0x8000) == 0) {
    return false;
  }
  const HWND focus = ::GetFocus();
  const HKL layout = ::GetKeyboardLayout(
      focus ? ::GetWindowThreadProcessId(focus, nullptr) : 0);
  std::array<BYTE, 256> keys = {};
  keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
  keys[VK_MENU] = keys[VK_RMENU] = 0x80;
  if (shift) {
    keys[VK_SHIFT] = 0x80;
  }
  std::array<wchar_t, 4> out = {};
  // Flag bit 2 (Windows 10 1607+): translate without touching the
  // keyboard state, so a pending dead key the user typed is not consumed.
  const int produced = ::ToUnicodeEx(
      vk, static_cast<UINT>((lparam >> 16) & 0xFF), keys.data(), out.data(),
      static_cast<int>(out.size()), 0x4, layout);
  // -1 is an AltGr dead key (still text); a control character is not.
  return produced < 0 || (produced > 0 && out[0] >= 0x20);
}
}  // namespace

PhiKeyboardAccelerators::PhiKeyboardAccelerators(
    const PhiPreferences& preferences,
    Host& host)
    : preferences_(preferences), host_(host) {
  g_accelerators = this;
}

PhiKeyboardAccelerators::~PhiKeyboardAccelerators() {
  if (hook_) {
    ::UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
  }
  g_accelerators = nullptr;
}

bool PhiKeyboardAccelerators::Install() {
  if (hook_) {
    return true;
  }
  hook_ = ::SetWindowsHookExW(WH_KEYBOARD, &PhiKeyboardAccelerators::HookProc,
                              nullptr, ::GetCurrentThreadId());
  return hook_ != nullptr;
}

// static
UINT PhiKeyboardAccelerators::MatchFixedAccelerator(UINT vk,
                                                    bool ctrl,
                                                    bool shift,
                                                    bool alt) {
  for (const Accelerator& accel : kAccelerators) {
    if (accel.vk == vk && accel.ctrl == ctrl && accel.shift == shift &&
        accel.alt == alt) {
      return accel.command;
    }
  }
  return 0;
}

// static
bool PhiKeyboardAccelerators::AltSoloAfterKeyDown(bool current,
                                                  UINT vk,
                                                  bool repeat) {
  if (vk == VK_MENU) {
    // A repeat (bit 30) is still a solo hold.
    return repeat ? current : true;
  }
  return false;
}

// static
LRESULT CALLBACK PhiKeyboardAccelerators::HookProc(int code,
                                                   WPARAM wparam,
                                                   LPARAM lparam) {
  PhiKeyboardAccelerators* self = g_accelerators;
  if (code != HC_ACTION || !self) {
    return ::CallNextHookEx(nullptr, code, wparam, lparam);
  }
  if (self->OnKey(wparam, lparam)) {
    return 1;
  }
  return ::CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT PhiKeyboardAccelerators::OnKey(WPARAM wparam, LPARAM lparam) {
  if (lparam & 0x80000000) {  // Key-up.
    // Releasing Ctrl commits an in-flight MRU switch (the Mac's ⌃⇥ HUD).
    if (wparam == VK_CONTROL && PhiTabSwitcher::IsActive()) {
      if (HWND shell = host_.FocusedShellHwnd()) {
        ::PostMessageW(shell, kMsgPhiSwitcherCommit, 0, 0);
      } else {
        PhiTabSwitcher::Cancel();
      }
    }
    // Alt pressed and released with nothing in between opens the app menu
    // — the keyboard's way to the ⋯ flyout, as Alt opens Chrome's. Any
    // chord (Alt+Tab, Alt+Left, Alt+F4, a mnemonic) cleared alt_solo_ on
    // its key-down, so only a bare tap gets here. The key is NOT
    // swallowed: Chromium still sees the release it expects.
    if (wparam == VK_MENU && alt_solo_) {
      alt_solo_ = false;
      if (HWND shell = host_.FocusedShellHwnd()) {
        PhiAppMenu::PostAccelerator(shell, kCmdShowAppMenu);
      }
    }
    return 0;
  }
  // Track whether Alt is being held alone, for the reveal gesture above.
  alt_solo_ = AltSoloAfterKeyDown(alt_solo_, static_cast<UINT>(wparam),
                                  (lparam & 0x40000000) != 0);
  const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
  const bool alt = (lparam & 0x20000000) != 0;  // The context code bit.
  if (ctrl) {
    // Alt under a held Ctrl — Ctrl+Alt, or AltGr, which arrives as
    // LCtrl+RAlt — is never the bare tap that opens the menu.
    alt_solo_ = false;
  }
  if (ctrl && alt &&
      IsAltGrCharacter(static_cast<UINT>(wparam), lparam, shift)) {
    return 0;  // AltGr typing a character: the page / field gets it.
  }
  // Esc under an open Ctrl+Tab HUD cancels it, and the page never sees it.
  if (PhiTabSwitcher::HandleKeyDown(static_cast<UINT>(wparam))) {
    return 1;
  }
  // Esc dismisses the Peek card floating over the focused shell (the Mac's
  // window-level Escape), swallowed so the page under it does not also see
  // it. With no card open the key goes to the page as usual.
  if (wparam == VK_ESCAPE && !ctrl && !shift && !alt) {
    if (HWND shell = host_.FocusedShellHwnd()) {
      if (host_.DismissPeeksForShell(shell)) {
        return 1;
      }
    }
  }
  if (wparam == VK_F6 && !ctrl && !alt) {
    // F6 / Shift+F6 cycle the focus: address field -> sidebar -> page.
    if (HWND shell = host_.FocusedShellHwnd()) {
      ::PostMessageW(shell, kMsgPhiCycleFocus,
                     shift ? kPhiFocusPrevious : kPhiFocusNext, 0);
      return 1;
    }
  }
  if (wparam == VK_TAB && ctrl && !alt) {
    // Ctrl+Tab drives the MRU switcher HUD, not a plain next-tab hop.
    if (HWND shell = host_.FocusedShellHwnd()) {
      ::PostMessageW(shell, shift ? kMsgPhiSwitcherPrev : kMsgPhiSwitcherNext,
                     0, 0);
      return 1;
    }
  }
  // The REBINDABLE shortcuts first (their effective chords carry the user's
  // overrides), so a custom chord shadows any fixed one it collides with.
  // The menu captures the originating presentation before posting, and
  // dispatches valid commands with HIWORD 1 (accelerator provenance).
  if (const UINT command = preferences_.MatchRebindableChord(
          static_cast<UINT>(wparam), ctrl, shift, alt)) {
    if (HWND shell = host_.FocusedShellHwnd()) {
      PhiAppMenu::PostAccelerator(shell, command,
                                  host_.FocusedFindBrowser(shell));
      return 1;
    }
  }
  if (const UINT command =
          MatchFixedAccelerator(static_cast<UINT>(wparam), ctrl, shift, alt)) {
    if (HWND shell = host_.FocusedShellHwnd()) {
      PhiAppMenu::PostAccelerator(shell, command,
                                  host_.FocusedFindBrowser(shell));
      return 1;  // Swallow: Chromium must not double-handle it.
    }
  }
  // A default chord the user moved a shortcut away from is DEAD (the Mac
  // replaces Chromium's key equivalent, so ⌘T opens nothing once New Tab is
  // ⌘⌥T): swallowed here, or the hosted browser's own accelerator for the
  // default would still fire it.
  if (preferences_.IsDisplacedDefaultChord(static_cast<UINT>(wparam), ctrl,
                                           shift, alt) &&
      host_.FocusedShellHwnd()) {
    return 1;
  }
  return 0;
}

}  // namespace phi_client

#endif  // defined(_WIN32)

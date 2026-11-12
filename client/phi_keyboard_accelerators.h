// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_KEYBOARD_ACCELERATORS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_KEYBOARD_ACCELERATORS_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "client/phi_ids.h"

namespace phi_client {

// Pure queue policy, shared with the host regression tests. Taking a command
// consumes it even if its presentation has gone away; it is never replayed.
class PhiKeyboardCommandQueue {
 public:
  PhiKeyboardCommandQueue();
  ~PhiKeyboardCommandQueue();
  PhiKeyboardCommandQueue(const PhiKeyboardCommandQueue&) = delete;
  PhiKeyboardCommandQueue& operator=(const PhiKeyboardCommandQueue&) = delete;

  struct Target {
    std::string space;
    BrowserId browser;
    TabId tab;
    bool operator==(const Target&) const = default;
  };
  struct Command {
    unsigned int id;
    Target target;
    BrowserId find_browser;
    uintptr_t focus;
  };
  uintptr_t Enqueue(Command command) {
    const uintptr_t token = ++next_token_;
    commands_.emplace(token, std::move(command));
    return token;
  }
  std::optional<Command> Take(uintptr_t token, const Target& current) {
    const auto it = commands_.find(token);
    if (it == commands_.end()) {
      return std::nullopt;
    }
    Command command = std::move(it->second);
    commands_.erase(it);
    if (command.target != current) {
      return std::nullopt;
    }
    return command;
  }
  void Erase(uintptr_t token) { commands_.erase(token); }
  void Clear() { commands_.clear(); }

 private:
  // Shared across instances: a destroyed/recreated shell cannot consume an
  // old message even if Windows reuses its HWND. Accessed on the UI thread.
  inline static uintptr_t next_token_ = 0;
  std::map<uintptr_t, Command> commands_;
};

}  // namespace phi_client

#if defined(_WIN32)
#include <windows.h>

#include "base/memory/raw_ptr_exclusion.h"

namespace phi_client {

class PhiPreferences;

// The client's menu-bar accelerators (docs/architecture-review.md §3.4,
// lane 1 §1.2 cluster 17). Keys land in the focused window — the browser
// child while browsing — so a plain WM_KEYDOWN handler on a shell never
// sees them; a thread-scoped WH_KEYBOARD hook on the UI thread does,
// before dispatch.
//
// It owns the hook, the FIXED accelerator table, the Alt-solo state and
// nothing else: the rebindable chords come from PhiPreferences (which owns
// the user's overrides) and every match is posted to the focused shell
// through the Host. No business state, no framework, no windows of its own.
class PhiKeyboardAccelerators {
 public:
  // What the hook needs from the app: which shell has focus, and the one
  // key it decides for something else (Esc over a Peek card).
  class Host {
   public:
    virtual ~Host() = default;
    // The shell window owning the focus HWND, or null (focus is elsewhere).
    virtual HWND FocusedShellHwnd() const = 0;
    // A focused Peek's browser, or invalid for the ordinary Space target.
    virtual BrowserId FocusedFindBrowser(HWND shell) const = 0;
    // True when a web page in `shell` holds the keyboard focus: a key-down
    // goes to it, and comes back through OnUnhandledPageKey when the page
    // leaves it alone.
    virtual bool PageTakesKeys(HWND shell) const = 0;
    // Esc with no modifiers over `shell`: dismiss the Peek card(s) floating
    // over it. True when one was dismissed — then the key is swallowed so
    // the page under the card does not also see it.
    virtual bool DismissPeeksForShell(HWND shell) = 0;
  };

  PhiKeyboardAccelerators(const PhiPreferences& preferences, Host& host);
  ~PhiKeyboardAccelerators();

  PhiKeyboardAccelerators(const PhiKeyboardAccelerators&) = delete;
  PhiKeyboardAccelerators& operator=(const PhiKeyboardAccelerators&) = delete;

  // Installs the WH_KEYBOARD hook on the CALLING thread (the UI thread).
  // False when Windows refused; the client then simply has no accelerators.
  // Idempotent: a second call is a no-op.
  bool Install();

  // One entry of the FIXED table — the shortcuts reserved the way Chrome
  // reserves its browser shortcuts (they fire even while a page has focus).
  struct Accelerator {
    UINT vk;
    bool ctrl;
    bool shift;
    bool alt;
    UINT command;
  };
  // The fixed table's command for a chord, or 0 when it binds none. Pure.
  static UINT MatchFixedAccelerator(UINT vk, bool ctrl, bool shift, bool alt);
  // True when `command` fires before a focused page sees its chord, the way
  // Chrome reserves new tab, close tab and the like. Every other shortcut
  // goes to the page first (Google Sheets takes Ctrl+F for its own find)
  // and runs only when the page leaves the key alone. Pure.
  static bool IsReservedCommand(UINT command);
  // True for a chord the hosted browser itself takes before the page
  // (Chrome's reserved accelerators): a shortcut the user moved onto one
  // cannot go to the page first, or Chromium's command would run. Pure.
  static bool IsChromiumReservedChord(UINT vk,
                                      bool ctrl,
                                      bool shift,
                                      bool alt);
  // A key-down the focused page left unhandled (PhiFrameworkDelegate::
  // OnUnhandledPageKey): runs the shortcut the hook let through. True when
  // the chord was one of ours, so Chromium's own accelerator stays out.
  bool OnUnhandledPageKey(UINT vk, bool ctrl, bool shift, bool alt);
  // What a key-down does to the Alt-solo state: VK_MENU arms it (a key
  // REPEAT is still a solo hold and leaves it as it was); any other
  // key-down ends it. Pure.
  static bool AltSoloAfterKeyDown(bool current, UINT vk, bool repeat);
  // A mouse button went down while Alt was held: that is a chord too
  // (Alt-click opens a link in split view, Ctrl+Alt-click in a Kiosk), so
  // the release must not summon the menu titles -- Windows' own Alt-revealed
  // bar stands down the same way. Called from the coordinator's mouse hook.
  void OnMouseButtonDown() { alt_solo_ = false; }
  // The app gained or lost the foreground (WM_ACTIVATEAPP). An Alt held
  // across it was Alt+Tab, whose release must not open the ⋯ menu — the
  // Tab never reached this hook, so the hold still looks solo.
  void OnAppActivationChanged() { alt_solo_ = false; }

 private:
  static LRESULT CALLBACK HookProc(int code, WPARAM wparam, LPARAM lparam);
  // The hook body proper. Returns 1 to swallow the key, 0 to pass it on.
  LRESULT OnKey(WPARAM wparam, LPARAM lparam);

  // The user's chord overrides (PhiPreferences::MatchRebindableChord) — the
  // service reads them live, so a rebind takes effect on the next key.
  RAW_PTR_EXCLUSION const PhiPreferences& preferences_;
  RAW_PTR_EXCLUSION Host& host_;
  HHOOK hook_ = nullptr;
  // Alt is down with nothing pressed since — a bare tap summons the caption
  // band's hidden menu titles on release. Cleared by any other key-down (so
  // Alt+Tab and the Alt+arrow history chords never trip it) and by a mouse
  // button (an Alt-click).
  bool alt_solo_ = false;
};

}  // namespace phi_client
#endif  // defined(_WIN32)

#endif  // PHIBROWSER_WIN_CLIENT_PHI_KEYBOARD_ACCELERATORS_H_

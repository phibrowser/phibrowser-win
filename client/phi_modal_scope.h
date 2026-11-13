// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_MODAL_SCOPE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_MODAL_SCOPE_H_

#include <windows.h>

namespace phi_framework {
class PhiFramework;
}  // namespace phi_framework

namespace phi_client {

// UI-thread nesting counter for every nested message loop the client runs:
// TrackPopupMenu, PhiPrompt*, MessageBox, the folder-icon picker and the
// Space theme panel. While it is non-zero, the coordinator DEFERS control
// verbs arriving from the pipe (PhiCoordinator::MsgWndProc, WM_PHI_CONTROL)
// instead of executing them inside the loop, and a shell's RequestClose is
// deferred to the next turn — so a verb can never destroy the controller
// whose context-menu frame is on the stack, and a posted row rebuild can
// never invalidate a reference a menu handler is holding
// (docs/architecture-review.md §4 items 10, 11).
//
// Usage: `PhiModalScope modal;` on the stack around the nested loop.
//
// Each scope also records the top-level window its loop belongs to (the
// root owner of the window active when it opened, or the one passed in),
// so a window asks about ITS loops only: closing one window does not wait
// on a menu or dialog up in another. A scope whose window cannot be told
// (nothing active) counts for every window.
//
// The counter is defined in phi_modal_scope.cc, not as a function-local
// static in an inline function: the client is one exe, but the kit's
// -Wunique-object-duplication rejects that shape on principle.
int& PhiModalDepthRef();
int PhiModalDepth();
// The open scopes that belong to `window`'s root owner (plus the unowned).
int PhiModalDepthFor(HWND window);

// Every scope also lets Chromium run its UI-thread tasks inside the loop it
// wraps (PhiFramework::BeginNativeNestedLoop): without that the pump never
// wakes inside a native loop, so navigations, IPC replies and timers wait for
// the loop to end and HangWatcher reports the UI thread hung (Sentry
// PHIBROWSER-WIN-7). Framework callbacks can therefore arrive while a scope
// is open; what they may not do to the window that owns it is covered where
// they land (PhiCoordinator::RemoveSlot parks a controller whose loop is on
// the stack). A scope opened before the framework is ready only counts.
class PhiModalScope {
 public:
  PhiModalScope();
  explicit PhiModalScope(HWND owner);
  ~PhiModalScope();
  PhiModalScope(const PhiModalScope&) = delete;
  PhiModalScope& operator=(const PhiModalScope&) = delete;

  // The framework the scopes bracket their loops with; set once it is ready
  // (PhiCoordinator::OnFrameworkReady). UI thread only.
  static void SetFramework(phi_framework::PhiFramework* framework);

 private:
  // The framework this scope called Begin on, so End pairs with it.
  phi_framework::PhiFramework* began_ = nullptr;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_MODAL_SCOPE_H_

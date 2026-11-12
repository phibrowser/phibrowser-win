// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_MODAL_SCOPE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_MODAL_SCOPE_H_

#include <windows.h>

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

class PhiModalScope {
 public:
  PhiModalScope();
  explicit PhiModalScope(HWND owner);
  ~PhiModalScope();
  PhiModalScope(const PhiModalScope&) = delete;
  PhiModalScope& operator=(const PhiModalScope&) = delete;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_MODAL_SCOPE_H_

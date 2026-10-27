// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_POPOVER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_POPOVER_H_

#include <windows.h>

#include "phibrowser-win/client/phi_window.h"

namespace phi_client {

// The floating-surface family (docs/architecture-review.md §3.6, lane 3
// §6.2). A popover is an owned WS_POPUP | WS_EX_TOOLWINDOW window placed
// off an anchor, dismissed when it loses activation or takes Escape. It
// collapses eleven hand-rolled classes; each survivor keeps only its
// model, its Paint() and its hit handling.
//
// WHAT THE FAMILY OWNS
//   * Placement. ONE anchor-plus-monitor-clamp implementation
//     (PhiPlacePopover in phi_window.h); five copies existed before.
//     Reposition() re-runs it, which is how a popover that grows with its
//     content (extensions) or slides between rows (tab preview) moves.
//   * Dismissal. WM_ACTIVATE / WA_INACTIVE closes the window once the
//     OPENING GRACE has elapsed. The grace exists because a menu teardown
//     or the browser child's async focus deactivates a popover the instant
//     it opens; three files hand-rolled the same 500 ms window.
//   * Escape, through PhiWindow::OnKey's default.
//   * The reopen guard. Clicking the button that opened a popover must
//     CLOSE it, not close-and-reopen: the click first deactivates the
//     popover (destroying it) and then reaches the button. RecordClose()
//     and SuppressReopen() are the shared form of the `last_owner` /
//     `closed_tick` statics the two framework popovers carry today.
//   * Corners: DWM rounding, or a shaped layer for the surfaces that need
//     per-pixel alpha. NEVER SetWindowRgn — see
//     ~/.claude/projects/-home-jx-Phi/memory/phi-win-antialiasing.md.
//
// WHAT A SUBCLASS OWNS: its model, Paint(), hits_ and the click handler.
// Nothing else.
//
// ASYNCHRONOUS BY CONSTRUCTION. A popover never runs a nested message
// loop. The two surfaces that did (the folder icon picker and the Space
// theme panel, lane 3 §4.4) now take a completion callback; their
// synchronous wrappers are the only remaining loops and exist purely so
// their call sites — in files this phase does not own — keep compiling.
class PhiPopover : public PhiWindow {
 public:
  struct Options {
    // False adds WS_EX_NOACTIVATE and shows with SW_SHOWNOACTIVATE: a HUD
    // or a hover card that must never take focus from the page.
    bool activate = true;
    bool topmost = false;
    bool layered = false;
    // DWM rounds the corners. Off for a layered surface that carries its
    // own antialiased shape.
    bool dwm_round = true;
    bool dismiss_on_inactive = true;
    bool dismiss_on_escape = true;
    // Milliseconds after ShowWindow during which WA_INACTIVE is ignored.
    int opening_grace_ms = 500;
    // WM_NCHITTEST answers HTTRANSPARENT: clicks fall through to whatever
    // is underneath (toast, tab preview, tab-switcher HUD).
    bool click_through = false;
    // Take the foreground on show. False for a non-activating popover.
    bool foreground = true;
  };

  // Where the popover goes. `anchor` is in SCREEN coordinates; for
  // PhiPopoverSide::kExplicit its top-left IS the position.
  struct Placement {
    RECT anchor = {};
    PhiPopoverSide side = PhiPopoverSide::kBelow;
    PhiPopoverAlign align = PhiPopoverAlign::kLeading;
    // DESIGN units; the popover scales them by its own dpi.
    int gap_dip = 0;
    // DEVICE pixels (an edge margin is a physical inset, not a design one).
    int margin_px = 0;
    bool flip = true;
    bool flip_only_if_fits = false;
    bool clamp = true;
    // Which monitor's work area clamps: the one under the anchor rect
    // (default) or the one the owner window is on. Both spellings exist in
    // the tree today and they differ when the anchor is off-owner.
    bool monitor_from_owner = false;
  };

  PhiPopover();
  ~PhiPopover() override;

  // Registers the class, places the popover and shows it. `size` is in
  // device pixels. Returns false when the window could not be created.
  bool CreatePopover(const ClassSpec& spec,
                     HWND owner,
                     SIZE size,
                     const Placement& placement,
                     const Options& options);

  // Shows the popover and starts the opening grace. Split from
  // CreatePopover so a subclass can lay itself out (and take its first
  // measurement pass) before the window appears.
  void Show();

  // Re-runs placement for a new size and/or anchor without recreating.
  void Reposition(const Placement& placement, SIZE size);
  // Moves to an already-computed screen rect (the animator's slide path).
  void MoveTo(const RECT& screen_rect);

  // Where a `size` popover WOULD land — for a subclass that has to know
  // before it commits (the tab preview's slide target).
  RECT PlacementRect(const Placement& placement, SIZE size) const;

  HWND owner() const { return owner_; }
  // True when DWM rounds this popover's corners (Windows 11) — and draws
  // its border with them, so a square GDI rim inside would cut across the
  // curve. False on Windows 10, where the popover is square.
  bool dwm_rounded() const { return dwm_rounded_; }
  bool InOpeningGrace() const;
  // Closes the popover (DestroyWindow). Safe when already gone.
  void Dismiss();

  // The reopen guard, shared by every popover: a toggle call site asks
  // ShouldSuppressReopen(owner) before constructing, and the popover files
  // RecordClose() on its way out.
  static void RecordClose(HWND owner);
  static bool ShouldSuppressReopen(HWND owner, int within_ms = 300);
  // True for a window this family created (a popover owning a popover is
  // the nested case WM_ACTIVATE handles).
  static bool IsPopoverWindow(HWND hwnd);

 protected:
  // The popover is being dismissed by activation loss or Escape. The
  // window still exists; the default calls Dismiss().
  virtual void OnDismiss();

  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) override;
  void OnEscape() override;

  const Options& options() const { return options_; }

 private:
  HWND owner_ = nullptr;
  Options options_;
  bool dwm_rounded_ = false;
  ULONGLONG shown_tick_ = 0;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_POPOVER_H_

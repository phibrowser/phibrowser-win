// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_NATIVE_PAGES_HOST_H_
#define PHIBROWSER_WIN_CLIENT_PHI_NATIVE_PAGES_HOST_H_

#include <windows.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"

namespace phi_client {

class PhiBookmarkManager;
class PhiNewTabPage;
class PhiShellWindow;

// One pane of the split on screen, as the new-tab route reads it: the
// tab, its URL, and where Chromium laid the pane out, in shell client
// coordinates (empty until Chromium's layout has run).
struct NativePane {
  int tab_id = 0;
  std::string url;
  RECT rect = {};
};

// THE two client-drawn pages that mount OVER the content card by URL
// (docs/architecture-review.md §3.5, lane 1 cluster M): the bookmark
// manager and the native new-tab page. The Mac mounts both the same way —
// the tab stays an ordinary browser tab and navigating it away restores
// the Chromium view — so the whole component is one predicate per page
// plus the mount/unmount it drives.
//
// A SPLIT has a new-tab page PER PANE (the Mac's per-pane
// WebContentViewController, each with its own NewTabViewController): a
// pane on a new-tab URL gets a page over its own rect, a pane on a web
// page shows Chromium's content. Outside a split the one page covers the
// card, as before.
//
// INVARIANT: a page is mounted or unmounted ONLY from Sync(), and Sync()
// runs only on the posted kMsgPhiSyncNativePages turn (or on the two
// explicit resumption points — the Space swap landing, and ResetNtp). The
// rebuild that NOTICES the route change calls SyncIfRouteChanged(), which
// posts; it never mounts inline, because the rebuild can run under
// WM_PAINT and a mount creates windows and moves focus.
class PhiNativePagesShell {
 public:
  virtual HWND NativePagesShellWindow() const = 0;
  // The controller both panels are constructed against: they read the
  // shell's layout, palette and omnibox session through it. Kept as the
  // owner pointer rather than a fourth interface because the panels are
  // the next things to move onto ShellServices, not this component.
  virtual PhiShellWindow* NativePagesOwner() = 0;
  // Off-the-record: nothing persists, so there is no bookmark manager.
  virtual bool NativePagesIncognito() const = 0;
  // A hidden agent window keeps Chromium's web NTP (the agent drives it
  // over CDP and no one looks at a native one there).
  virtual bool NativePagesIsAgent() const = 0;
  virtual bool NativePagesContentFullscreen() const = 0;
  // The active browser has no tabs at all: the NTP's omnibox is the way
  // back in.
  virtual bool NativePagesPlaceholding() const = 0;
  // Placeholder mode under "New tab behavior = Omnibox": the floating
  // omnibox is the way in, so the page under it is a blank ground with no
  // mark and no box of its own (the Mac's placeholder shell).
  virtual bool NativePagesBlankPlaceholder() const = 0;
  // A Space switch is mid-slide: the page arrives (or leaves) with the
  // content, when the slide's completion re-runs Sync().
  virtual bool NativePagesSwapPending() const = 0;
  // The ACTIVE tab's URL, which is what selects the route.
  virtual const std::string& NativePagesActiveUrl() const = 0;
  // The ACTIVE tab's id (0 when there is none) — in a split, the pane
  // whose page owns the keyboard and the commit.
  virtual int NativePagesActiveTabId() const = 0;
  // The panes of the split the ACTIVE tab is in, primary (left/top) first;
  // empty when the active tab is not in a split.
  virtual std::vector<NativePane> NativePagesSplitPanes() const = 0;
  virtual void NativePagesRaiseCaps() = 0;
  // Never leave the keyboard on a hidden field.
  virtual void NativePagesFocusContent() = 0;
  virtual HWND NativePagesContentWindow() const = 0;
  // True while the commit that blanked the NTP's controls is still the
  // ACTIVE tab's business: the controls come back once another tab is
  // active, or that load has ended.
  virtual bool NativePagesCommitPending() const = 0;

 protected:
  // Non-virtual and protected: the shell OWNS the host, never the other way
  // round, so nothing ever deletes through this pointer.
  ~PhiNativePagesShell() = default;
};

class PhiNativePagesHost {
 public:
  explicit PhiNativePagesHost(PhiNativePagesShell* shell);
  ~PhiNativePagesHost();

  PhiNativePagesHost(const PhiNativePagesHost&) = delete;
  PhiNativePagesHost& operator=(const PhiNativePagesHost&) = delete;

  // THE mount point: brings both pages into line with the current route.
  // Runs on the posted turn.
  void Sync();
  // The new-tab page alone: the Space-switch slide defers its mount past
  // the reveal, exactly as the content itself is deferred.
  void SyncNtp();
  // The reveal's first half: the pages the entering content wants go up,
  // painted, BEFORE its browser child shows — the framework's own new-tab
  // page under them never reaches the screen. Pages no longer wanted stay
  // up over the leaving child until the SyncNtp after the reveal.
  void MountNtpForReveal();
  // A rebuild noticed the route may have moved. Refreshes a mounted NTP in
  // place (a palette ramp repaints it with the chrome, a pane that moved
  // is followed) and posts kMsgPhiSyncNativePages when the mount state
  // actually has to change.
  void SyncIfRouteChanged();
  // The content card moved or resized, or Chromium moved a pane.
  void Reposition();
  // A palette change (the create-Space form's live preview, a theme ramp).
  void Refresh();
  // A Space URL rule re-homed a navigation that began on the new tab: the
  // page returns to its clean state (the Mac's resetToInitialState).
  void ResetNtp();
  // Both pages go before the shell window they sit over.
  void Shutdown();

  // Whether either page is currently mounted over the content card.
  bool bookmark_page_mounted() const;
  // Any new-tab page is up (the card's, or a pane's).
  bool ntp_mounted() const;
  // Back above the browser child — every mounted page.
  void RaiseNtp();
  // The omnibox EDIT the session is (or should be) in: the field that has
  // the focus when a page's field does, else the ACTIVE pane's page's
  // field. Null when no page is up.
  HWND ntp_field() const;
  // That page's search box in screen coordinates, for the omnibox
  // session's suggestion dropdown. False when no page is up.
  bool NtpBoxScreenRect(RECT* out) const;
  // The active pane's page is up and its field is where the keyboard
  // belongs (the shell forwards activation and focus to it rather than to
  // the browser child).
  bool ntp_wants_focus() const;
  void FocusNtpField();
  // omniBoxDidClear: the active pane's page blanks its controls so nothing
  // of the new-tab page lingers over the arriving one; the rebuild that
  // sees the new URL unmounts it. No-op unless that page is mounted; true
  // when it blanked.
  bool HideNtpControls();

 private:
  // Where a slot's page should be: slot 0 is the card (no split) or the
  // primary pane, slot 1 the secondary pane.
  struct Placement {
    bool wanted = false;
    int tab_id = 0;
    bool pane = false;  // false: the whole content card.
    RECT rect = {};
  };
  static constexpr int kSlots = 2;

  // The two route predicates. Pure functions of the shell's state.
  bool WantsBookmarkPage() const;
  bool WantsNtp() const;
  // The new-tab route per slot, from the shell's state.
  std::array<Placement, kSlots> ComputePlacements() const;
  void SyncBookmarkPage();
  void SyncNtpSlot(int slot, const Placement& want);
  // The page whose tab is the active one (or the card's page). Null when
  // none is mounted.
  PhiNewTabPage* ActivePage() const;
  // The page whose window or field has the keyboard focus, if any.
  PhiNewTabPage* FocusedPage() const;
  bool PageMounted(int slot) const;
  // Focuses the active page's omnibox unless the user is busy in another
  // control.
  void FocusNtpIfIdle();

  // Owns this host and outlives it.
  RAW_PTR_EXCLUSION PhiNativePagesShell* const shell_;
  std::unique_ptr<PhiBookmarkManager> bookmark_manager_;
  std::array<std::unique_ptr<PhiNewTabPage>, kSlots> pages_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_NATIVE_PAGES_HOST_H_

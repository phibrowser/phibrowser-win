// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_native_pages_host.h"

#include <algorithm>
#include <array>

#include "phibrowser-win/client/phi_bookmark_manager.h"
#include "phibrowser-win/client/phi_new_tab_page.h"
#include "phibrowser-win/client/phi_shell_window.h"

namespace phi_client {

PhiNativePagesHost::PhiNativePagesHost(PhiNativePagesShell* shell)
    : shell_(shell) {}

PhiNativePagesHost::~PhiNativePagesHost() = default;

void PhiNativePagesHost::Shutdown() {
  bookmark_manager_.reset();
  for (auto& page : pages_) {
    page.reset();
  }
}

bool PhiNativePagesHost::bookmark_page_mounted() const {
  return bookmark_manager_ && bookmark_manager_->visible();
}

bool PhiNativePagesHost::PageMounted(int slot) const {
  return pages_[slot] && pages_[slot]->visible();
}

bool PhiNativePagesHost::ntp_mounted() const {
  for (int slot = 0; slot < kSlots; ++slot) {
    if (PageMounted(slot)) {
      return true;
    }
  }
  return false;
}

PhiNewTabPage* PhiNativePagesHost::ActivePage() const {
  const int active = shell_->NativePagesActiveTabId();
  for (int slot = 0; slot < kSlots; ++slot) {
    if (PageMounted(slot) &&
        (pages_[slot]->tab_id() == 0 || pages_[slot]->tab_id() == active)) {
      return pages_[slot].get();
    }
  }
  return nullptr;
}

PhiNewTabPage* PhiNativePagesHost::FocusedPage() const {
  const HWND focus = ::GetFocus();
  if (!focus) {
    return nullptr;
  }
  for (int slot = 0; slot < kSlots; ++slot) {
    if (PageMounted(slot) &&
        (focus == pages_[slot]->hwnd() || focus == pages_[slot]->field())) {
      return pages_[slot].get();
    }
  }
  return nullptr;
}

void PhiNativePagesHost::RaiseNtp() {
  for (int slot = 0; slot < kSlots; ++slot) {
    if (PageMounted(slot)) {
      ::SetWindowPos(pages_[slot]->hwnd(), HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
  }
}

HWND PhiNativePagesHost::ntp_field() const {
  PhiNewTabPage* page = FocusedPage();
  if (!page) {
    page = ActivePage();
  }
  return page ? page->field() : nullptr;
}

bool PhiNativePagesHost::NtpBoxScreenRect(RECT* out) const {
  PhiNewTabPage* page = FocusedPage();
  if (!page) {
    page = ActivePage();
  }
  if (!page || !out) {
    return false;
  }
  *out = page->BoxScreenRect();
  return true;
}

bool PhiNativePagesHost::ntp_wants_focus() const {
  PhiNewTabPage* page = ActivePage();
  return page && page->WantsFocus();
}

void PhiNativePagesHost::FocusNtpField() {
  if (PhiNewTabPage* page = ActivePage()) {
    page->FocusField();
  }
}

bool PhiNativePagesHost::HideNtpControls() {
  PhiNewTabPage* page = ActivePage();
  if (!page) {
    return false;
  }
  page->HideControls();
  return true;
}

// The bookmark route: the Mac's URL-driven mount, refused off the record
// where there is nothing stored to manage.
bool PhiNativePagesHost::WantsBookmarkPage() const {
  return !shell_->NativePagesIncognito() &&
         PhiBookmarkManager::IsManagerUrl(shell_->NativePagesActiveUrl());
}

// The new-tab route outside a split: the ACTIVE tab sits on a new-tab URL
// — or the browser has no tabs at all (placeholder mode), where the page's
// omnibox is the way back in.
bool PhiNativePagesHost::WantsNtp() const {
  if (shell_->NativePagesIsAgent() || shell_->NativePagesContentFullscreen()) {
    return false;
  }
  if (shell_->NativePagesPlaceholding()) {
    return true;
  }
  return PhiNewTabPage::IsNewTabUrl(shell_->NativePagesActiveUrl());
}

std::array<PhiNativePagesHost::Placement, PhiNativePagesHost::kSlots>
PhiNativePagesHost::ComputePlacements() const {
  std::array<Placement, kSlots> out;
  if (shell_->NativePagesIsAgent() || shell_->NativePagesContentFullscreen()) {
    return out;
  }
  const std::vector<NativePane> panes = shell_->NativePagesSplitPanes();
  if (panes.empty()) {
    // The one page over the card, keyed by the active tab's URL.
    out[0].wanted = WantsNtp();
    return out;
  }
  // A page per new-tab pane, over that pane; a web pane keeps Chromium's
  // content. Both panes may be new tabs — that is the "split of two new
  // tabs" the sidebar can form, and it shows as two pages side by side.
  const int count = std::min<int>(kSlots, static_cast<int>(panes.size()));
  for (int slot = 0; slot < count; ++slot) {
    if (!PhiNewTabPage::IsNewTabUrl(panes[slot].url)) {
      continue;
    }
    out[slot].wanted = true;
    out[slot].tab_id = panes[slot].tab_id;
    out[slot].pane = true;
    out[slot].rect = panes[slot].rect;
  }
  return out;
}

void PhiNativePagesHost::SyncBookmarkPage() {
  if (!WantsBookmarkPage()) {
    if (bookmark_manager_) {
      bookmark_manager_->Hide();
    }
    return;
  }
  if (!bookmark_manager_) {
    bookmark_manager_ =
        std::make_unique<PhiBookmarkManager>(shell_->NativePagesOwner());
  }
  if (bookmark_manager_->visible()) {
    bookmark_manager_->Reposition();
    bookmark_manager_->Refresh();
  } else {
    bookmark_manager_->Show();
  }
}

void PhiNativePagesHost::SyncNtpSlot(int slot, const Placement& want) {
  std::unique_ptr<PhiNewTabPage>& page = pages_[slot];
  if (!want.wanted) {
    if (PageMounted(slot)) {
      const HWND focus = ::GetFocus();
      const bool had_focus =
          focus && (focus == page->hwnd() || focus == page->field());
      page->Hide();
      if (had_focus) {
        // Never leave the keyboard on a hidden field.
        shell_->NativePagesFocusContent();
      }
    }
    return;
  }
  if (!page) {
    page = std::make_unique<PhiNewTabPage>(shell_->NativePagesOwner());
  }
  page->SetPlacement(want.tab_id, want.pane ? &want.rect : nullptr);
  const bool active = want.tab_id == 0 ||
                      want.tab_id == shell_->NativePagesActiveTabId();
  // An empty window under the Omnibox behaviour: the page is only the
  // blank ground under the floating omnibox (NativePagesBlankPlaceholder) —
  // controls hidden, never focused.
  const bool blank = !want.pane && shell_->NativePagesPlaceholding() &&
                     shell_->NativePagesBlankPlaceholder();
  if (page->visible() && blank) {
    if (page->controls_hidden()) {
      page->Refresh();  // A palette ramp repaints it with the chrome.
    } else {
      page->HideControls();
    }
    return;
  }
  if (page->visible()) {
    // Controls blanked by a commit come back once that navigation is no
    // longer the active tab's business: another tab is active, or the load
    // ended and this tab is still a new tab (a new-tab URL typed in, a
    // navigation that never left).
    if (page->controls_hidden() &&
        !(active && shell_->NativePagesCommitPending())) {
      page->Reset();
      if (active) {
        FocusNtpIfIdle();
      }
    } else {
      page->Refresh();  // A palette ramp repaints it with the chrome.
    }
    return;
  }
  page->Show();
  if (blank) {
    page->HideControls();
  }
  // Painted now, not on a later WM_PAINT: the shell sits on a translucent
  // backdrop, so a page shown but not yet painted lets the desktop through
  // the pane for a frame (switching to a new-tab tab blinked).
  ::UpdateWindow(page->hwnd());
  if (const HWND shell = shell_->NativePagesShellWindow()) {
    // The corner caps go back on top of the page, as they ride the browser.
    shell_->NativePagesRaiseCaps();
    // viewDidAppear focuses the omnibox — in the window the user is in, and
    // only the ACTIVE pane's: a background window mounting its page must
    // not pull the focus over, and the other pane's page must not pull it
    // off the active pane.
    if (active && !blank && ::GetForegroundWindow() == shell) {
      FocusNtpIfIdle();
    }
  }
}

void PhiNativePagesHost::SyncNtp() {
  if (shell_->NativePagesSwapPending()) {
    // A Space switch is mid-slide: the page arrives (or leaves) with the
    // content, when the slide's completion re-runs this — the Mac reveals
    // the target Space's content, native page included, on completion.
    return;
  }
  const std::array<Placement, kSlots> want = ComputePlacements();
  for (int slot = 0; slot < kSlots; ++slot) {
    SyncNtpSlot(slot, want[slot]);
  }
}

void PhiNativePagesHost::MountNtpForReveal() {
  const std::array<Placement, kSlots> want = ComputePlacements();
  for (int slot = 0; slot < kSlots; ++slot) {
    if (!want[slot].wanted) {
      continue;
    }
    SyncNtpSlot(slot, want[slot]);
    if (PageMounted(slot)) {
      // Painted now: a GDI paint made in this turn lands before the window
      // shows that follow it, an invalidation left to WM_PAINT does not.
      ::UpdateWindow(pages_[slot]->hwnd());
    }
  }
}

void PhiNativePagesHost::Sync() {
  SyncBookmarkPage();
  SyncNtp();
}

void PhiNativePagesHost::SyncIfRouteChanged() {
  const HWND shell = shell_->NativePagesShellWindow();
  if (!shell) {
    return;
  }
  const bool mounted = bookmark_page_mounted();
  bool post = mounted != WantsBookmarkPage();
  const std::array<Placement, kSlots> want = ComputePlacements();
  const int active = shell_->NativePagesActiveTabId();
  for (int slot = 0; slot < kSlots; ++slot) {
    const bool up = PageMounted(slot);
    if (up != want[slot].wanted) {
      post = true;
      continue;
    }
    if (!up) {
      continue;
    }
    PhiNewTabPage* page = pages_[slot].get();
    if (page->tab_id() != want[slot].tab_id) {
      post = true;  // The pane changed hands (a swap): remount for it.
      continue;
    }
    // A page blanked by a commit whose navigation is over (or belongs to
    // another tab now) needs the sync too, to bring its controls back.
    const bool this_active = page->tab_id() == 0 || page->tab_id() == active;
    if (page->controls_hidden() &&
        !(this_active && shell_->NativePagesCommitPending())) {
      post = true;
    }
    // In place: the pane Chromium laid out, and the palette.
    page->SetPlacement(want[slot].tab_id,
                       want[slot].pane ? &want[slot].rect : nullptr);
    page->Refresh();  // Palette ramps repaint it with the chrome.
  }
  if (post) {
    ::PostMessageW(shell, kMsgPhiSyncNativePages, 0, 0);
  }
}

void PhiNativePagesHost::Reposition() {
  if (bookmark_page_mounted()) {
    bookmark_manager_->Reposition();
  }
  if (!ntp_mounted()) {
    return;
  }
  // Chromium may have moved the panes (a divider drag) with no route
  // change: re-read where each mounted page's pane is now.
  const std::array<Placement, kSlots> want = ComputePlacements();
  for (int slot = 0; slot < kSlots; ++slot) {
    if (!PageMounted(slot)) {
      continue;
    }
    PhiNewTabPage* page = pages_[slot].get();
    if (want[slot].wanted && want[slot].tab_id == page->tab_id()) {
      page->SetPlacement(want[slot].tab_id,
                         want[slot].pane ? &want[slot].rect : nullptr);
    }
    page->Reposition();
  }
}

void PhiNativePagesHost::Refresh() {
  for (int slot = 0; slot < kSlots; ++slot) {
    if (PageMounted(slot)) {
      pages_[slot]->Refresh();
    }
  }
}

void PhiNativePagesHost::FocusNtpIfIdle() {
  const HWND shell = shell_->NativePagesShellWindow();
  PhiNewTabPage* page = ActivePage();
  if (!page || !shell || ::GetForegroundWindow() != shell) {
    return;
  }
  const HWND focus = ::GetFocus();
  if (focus && focus != shell &&
      focus != shell_->NativePagesContentWindow() &&
      focus != page->hwnd() && focus != page->field()) {
    return;  // The user is in another control (the address field, a panel).
  }
  page->FocusField();
}

void PhiNativePagesHost::ResetNtp() {
  if (PhiNewTabPage* page = ActivePage()) {
    page->Reset();
    FocusNtpIfIdle();
    return;
  }
  SyncNtp();
}

}  // namespace phi_client

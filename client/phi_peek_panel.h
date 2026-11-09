// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_PEEK_PANEL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_PEEK_PANEL_H_

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_ids.h"
#include "client/phi_panel.h"
#include "client/phi_surface_registry.h"
#include "client/phi_theme.h"

namespace phi_framework {
class PhiFramework;
}

namespace phi_client {

class PhiCoordinator;
class PhiShellWindow;
class PhiWindowModel;

// The peek's two helper surfaces, both PhiPanels of their own (defined in
// phi_peek_panel.cc): the drop shadow under the card, and the chip layer
// over the hosted page.
class PhiPeekShadow;

// The Mac's floating Peek panel (UserInterface/Peek/PeekPanelController): a
// link opened from a bound tab previews in a card floating over the page
// pane — the sidebar stays usable — instead of taking a tab. Closing (X, Esc,
// a click on the page around it) or expanding ends the peek.
//
// Geometry follows the Mac's spec: the card insets from the content card by
// 4% of its width (at least 16 DIP) horizontally and 14 DIP vertically, with
// a 36 dip header on the flyout ground above the page (§5.7). The card itself
// carries no chrome — the page fills it edge to edge (Arc's peek layout).
//
// WHAT THE CARD HOSTS DIVERGES FROM THE MAC, deliberately: there a peek is a
// live tab in the opener's own strip whose view is re-parented into the
// panel, which works because the Mac client hosts WebContents views directly.
// This client hosts a whole BrowserView per window and a non-active tab is
// not independently rendered, so the peek is its own browser (see the fork's
// phi_peek_window.h) whose window is parented into this panel. The panel is a
// WS_CHILD of the opener's shell — the bookmark manager's trick — so it
// clips into the content card and travels with the window.
class PhiPeekPanel : public PhiSurfacePanel, public PhiPanel {
 public:
  // `model` is the coordinator's window model: the peek's own url is read
  // from its browser's snapshot there (docs/architecture-review.md §3.3).
  PhiPeekPanel(int64_t window_id,
               HWND browser_hwnd,
               int64_t opener_window_id,
               int64_t opener_tab_id,
               PhiShellWindow* opener,
               phi_framework::PhiFramework* framework,
               const PhiWindowModel* model,
               PhiCoordinator* coordinator);
  ~PhiPeekPanel() override;

  PhiPeekPanel(const PhiPeekPanel&) = delete;
  PhiPeekPanel& operator=(const PhiPeekPanel&) = delete;

  // PhiSurfacePanel. The "opener" is the surface's origin.
  SidebarSurface kind() const override;
  int64_t window_id() const override;
  int64_t origin_window_id() const override;
  int64_t origin_tab_id() const override;
  int64_t opener_window_id() const { return opener_window_id_.value; }
  int64_t opener_tab_id() const { return opener_tab_id_; }
  // The card's own window (the peek's browser is reparented inside it) —
  // what the click-outside test compares against.
  HWND hwnd() const override;
  // The card, anything in it, or the chip layer beside it.
  bool ContainsWindow(HWND window) const override;
  // Only a press on the PAGE PANE around the peek dismisses it (the Mac's
  // monitor: "only clicks on the page pane around the panel close the
  // peek"). The sidebar, the caption band, a resize grab on the frame: the
  // peek stays — a tab switch hides it with its opener instead.
  bool ClickDismisses(POINT screen_point) const override;
  // Shows the shadow, the card and the chips, in that z-order. Raise() is
  // the same for a peek that is already up and nothing for one that is down.
  void Show();

  // The opener window moved or resized: re-fit the card to its content rect.
  void Reposition() override;

  // A peek belongs to the tab that opened it and covers that tab's whole
  // split: it shows while the opener or its split partner is active (the
  // surface coordinator decides).
  void SetSurfaceShown(bool shown) override;

  // The framework says this peek's page moved: re-read the url for the title
  // strip and repaint.
  void RefreshState() override;

  // Shadow first, then the card over it — both above the opener's page.
  void Raise() override;
  void NoteEndReason(const char* reason) override;
  // The metric's two readings: the reason a closer announced (null when
  // none did) and when the card came up (GetTickCount64).
  const char* end_reason() const { return end_reason_; }
  uint64_t opened_tick() const { return opened_tick_; }

 protected:
  // PhiPanel: the peek keeps its layout, its ground fill and its commands.
  void Paint(HDC dc, const RECT& dirty) override;
  void OnSize(int width, int height) override;
  PhiPalette QueryPalette() const override;
  void OnThemeChanged() override;

 private:
  // §5.7: a 36 dip header on the flyout ground (the peeked page's favicon,
  // its host and path, then Open as tab · Open in split view · Close),
  // the page below it. The header is the panel's own client area, so the
  // hosted page never covers it and the buttons are ordinary hits.
  struct Layout {
    RECT header;
    RECT page;
    RECT favicon;
    RECT text;
    RECT expand;   // Promote the preview into a tab of the opener window.
    RECT split;    // ...and put it beside the tab that opened it.
    RECT close;
  };
  Layout ComputeLayout() const;

  void ClickControl(int index);
  void PositionContent();
  void Dismiss();
  // The header's buttons into the panel's hit list (hover, click, UIA).
  void LayoutHits();
  void PaintHeader(HDC dc, const Layout& layout);
  void OnHover(int hit, int previous) override;
  void OnClick(int hit, POINT client_pt) override;
  int64_t surface_tab_id() const override;
  // Rounds the card with a corner fringe (PhiPanel::SetCornerFringe): the
  // hosted browser is a child of this window, so only this window's region
  // can clip it, and the fringe antialiases the curve that region cuts.
  void ApplyCardShape();
  // Re-renders and repositions the drop shadow under the card.
  void UpdateShadow();

  BrowserId window_id_;  // The peek's own browser.
  HWND browser_hwnd_ = nullptr;
  // The card's drop shadow: a LAYERED sibling beneath the panel. A plain
  // child window is opaque and could only shade its own background, so the
  // blur would never reach the opener's page.
  std::unique_ptr<PhiPeekShadow> shadow_;
  // The close / new-tab / split chips, a shaped-layer child of this window
  // stacked above the hosted browser.
  // The page card's right margin beside the card, in pixels, as Reposition
  // last measured it: where the chips stand.
  BrowserId opener_window_id_;  // The browser the peek was opened FROM.
  int64_t opener_tab_id_ = 0;
  RAW_PTR_EXCLUSION PhiShellWindow* opener_ = nullptr;
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  RAW_PTR_EXCLUSION const PhiWindowModel* model_ = nullptr;
  RAW_PTR_EXCLUSION PhiCoordinator* coordinator_ = nullptr;

  std::wstring url_;
  // The peek browser's one tab and its favicon, for the header.
  int64_t tab_id_ = 0;
  std::vector<unsigned char> favicon_;
  int favicon_w_ = 0;
  int favicon_h_ = 0;
  int hot_ = -1;
  const char* end_reason_ = nullptr;
  uint64_t opened_tick_ = 0;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_PEEK_PANEL_H_

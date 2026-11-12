// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_READER_PANEL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_READER_PANEL_H_

#include <windows.h>

#include <cstdint>
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

// The Mac's Reader View overlay (UserInterface/Reader/ReaderPanelController):
// the reading surface covers the WHOLE page pane — the sidebar stays usable —
// while the origin tab keeps its live page underneath, so leaving the reader
// simply reveals it again. No chrome of its own: the reader page carries its
// own HUD (style controls, read-aloud, close).
//
// WHAT THE PANEL HOSTS DIVERGES FROM THE MAC the same way PhiPeekPanel's does,
// and for the same reason. There the surface is a live tab in the origin's own
// strip, kept off the client's tab list, whose view is re-parented into a
// child window — that client hosts WebContents views directly. This one hosts
// a whole BrowserView per window and a non-active tab is not independently
// rendered, so the surface is MOVED into a browser of its own
// (PhiFramework::MoveTabToReaderPanel) whose window is parented in here. The
// panel is a WS_CHILD of the origin's shell, so it clips into the content card
// and travels with the window.
//
// One reader per origin tab, any number of origins: a panel shows while its
// origin tab is the active one — or the active tab's split partner, for a
// reader over a split pane, which covers that pane alone (phibrowser-mac
// 89dd8a48: each pane of a split can show its own reader, both up at once).
class PhiReaderPanel : public PhiSurfacePanel, public PhiPanel {
 public:
  // `model` is the coordinator's window model: the surface browser's strip
  // is read from its snapshot there (docs/architecture-review.md §3.3).
  PhiReaderPanel(int64_t window_id,
                 HWND browser_hwnd,
                 int64_t origin_window_id,
                 int64_t origin_tab_id,
                 PhiShellWindow* origin,
                 phi_framework::PhiFramework* framework,
                 const PhiWindowModel* model,
                 PhiCoordinator* coordinator);
  ~PhiReaderPanel() override;

  PhiReaderPanel(const PhiReaderPanel&) = delete;
  PhiReaderPanel& operator=(const PhiReaderPanel&) = delete;

  // PhiSurfacePanel.
  SidebarSurface kind() const override;
  int64_t window_id() const override;
  int64_t origin_window_id() const override;
  int64_t origin_tab_id() const override;
  HWND hwnd() const override;

  // The surface tab that was moved in, once the move has happened (the panel
  // is constructed re-entrantly from Browser::Create, before it has one).
  // What tells a link click apart from the tab going away: an emptied panel
  // browser is handed a placeholder new-tab page, which is a DIFFERENT tab,
  // not a navigation of this one.
  void BindSurfaceTab(int64_t surface_tab_id) override;

  // Puts the panel above the origin's browser child, without disturbing the
  // corner caps that have to sit above IT (see PhiShellWindow::
  // RaiseCornerCaps) — raising this window alone squares the card's corners.
  void Raise() override;
  // The origin window moved or resized, or Chromium laid the split panes
  // out again: re-fit to the content rect, or to the origin's pane while it
  // is a pane of the split on screen.
  void Reposition() override;
  // A reader belongs to the tab it was opened for (the surface coordinator
  // decides when that tab's page is on screen).
  void SetSurfaceShown(bool shown) override;
  // The hosted page moved. A link inside the reader is a plain link, so a
  // click navigates the surface away from reader.html — the panel then hands
  // the URL back to the origin window as a normal tab rather than showing an
  // arbitrary site inside the reading surface (the Mac's
  // expandReaderOverlayIntoTab; like Peek, this client re-opens the URL
  // instead of moving the live tab across).
  void RefreshState() override;

 protected:
  // PhiPanel: the reader keeps its model, its ground fill and its card
  // shape; registration, the `this` binding, the back buffer, the dpi and
  // the teardown come from the base (phi_window.h).
  void Paint(HDC dc, const RECT& dirty) override;
  void OnSize(int width, int height) override;
  PhiPalette QueryPalette() const override;
  void OnThemeChanged() override;

 private:
  // Over the whole card the panel stays square: it covers the content card
  // exactly, so the shell's antialiased corner caps, raised above it, round
  // it — no window region (a region cuts on the pixel grid). Over a split
  // pane it rounds itself inside the pane's sheet, as a pane's new-tab page
  // does (PhiNewTabPage::ApplyPaneCorners): no cap sits at a pane's corners.
  void ApplyCardShape();
  void PositionContent();
  int64_t surface_tab_id() const override;

  BrowserId window_id_;  // The surface's own browser.
  HWND browser_hwnd_ = nullptr;
  BrowserId origin_window_id_;  // The browser whose tab the reader covers.
  int64_t origin_tab_id_ = 0;
  RAW_PTR_EXCLUSION PhiShellWindow* origin_ = nullptr;
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  RAW_PTR_EXCLUSION const PhiWindowModel* model_ = nullptr;
  RAW_PTR_EXCLUSION PhiCoordinator* coordinator_ = nullptr;
  // The last URL seen on the hosted page, so the navigate-away test fires
  // once rather than on every state refresh.
  std::wstring url_;
  int64_t surface_tab_id_ = 0;
  bool expanded_ = false;
  // Covering the origin's split pane rather than the whole content card.
  bool in_pane_ = false;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_READER_PANEL_H_

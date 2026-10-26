// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_reader_panel.h"

#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_coordinator.h"
#include "phibrowser-win/client/phi_reader.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_shell_window.h"
#include "phibrowser-win/client/phi_window_model.h"

namespace phi_client {

namespace {

constexpr wchar_t kReaderPanelClassName[] = L"PhiReaderPanel";

}  // namespace

PhiReaderPanel::PhiReaderPanel(int64_t window_id,
                               HWND browser_hwnd,
                               int64_t origin_window_id,
                               int64_t origin_tab_id,
                               PhiShellWindow* origin,
                               phi_framework::PhiFramework* framework,
                               const PhiWindowModel* model,
                               PhiCoordinator* coordinator)
    : window_id_(BrowserId{window_id}),
      browser_hwnd_(browser_hwnd),
      origin_window_id_(BrowserId{origin_window_id}),
      origin_tab_id_(origin_tab_id),
      origin_(origin),
      framework_(framework),
      model_(model),
      coordinator_(coordinator) {
  HWND shell = origin_ ? origin_->shell_hwnd() : nullptr;
  if (!shell) {
    LOG(ERROR) << "[PhiClient] reader panel has no origin shell";
    return;
  }

  // A child of the origin's shell, above the hosted browser — the peek
  // panel's trick, so the reader clips into the content card and travels
  // with the window.
  ClassSpec spec;
  spec.name = kReaderPanelClassName;
  spec.style = CS_HREDRAW | CS_VREDRAW;
  Options options;
  options.style = WS_CLIPCHILDREN;
  const RECT nothing = {0, 0, 0, 0};
  if (!CreatePanel(spec, shell, nothing, options)) {
    LOG(ERROR) << "[PhiClient] reader panel CreateWindowEx failed err="
               << ::GetLastError();
    return;
  }

  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    LONG_PTR style = ::GetWindowLongPtrW(browser_hwnd_, GWL_STYLE);
    style &= ~(WS_POPUP | WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_THICKFRAME |
               WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_MAXIMIZE |
               WS_MINIMIZE);  // A maximized child covers the header.
    style |= WS_CHILD | WS_CLIPSIBLINGS;
    ::SetWindowLongPtrW(browser_hwnd_, GWL_STYLE, style);
    LONG_PTR ex = ::GetWindowLongPtrW(browser_hwnd_, GWL_EXSTYLE);
    ex &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME |
            WS_EX_STATICEDGE | WS_EX_APPWINDOW);
    ::SetWindowLongPtrW(browser_hwnd_, GWL_EXSTYLE, ex);
    ::SetParent(browser_hwnd_, hwnd());
    ::SetWindowPos(browser_hwnd_, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                       SWP_FRAMECHANGED);
  }

  Reposition();
  RefreshState();
  // Above the shell's hosted browser child, or the page would occlude it.
  ShowPanel();
  if (origin_) {
    origin_->RaiseCornerCaps();
  }
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    ::SetFocus(browser_hwnd_);
  }
}

SidebarSurface PhiReaderPanel::kind() const {
  return SidebarSurface::kReader;
}
int64_t PhiReaderPanel::window_id() const {
  return window_id_.value;
}
int64_t PhiReaderPanel::origin_window_id() const {
  return origin_window_id_.value;
}
int64_t PhiReaderPanel::origin_tab_id() const {
  return origin_tab_id_;
}
HWND PhiReaderPanel::hwnd() const {
  // PhiSurfacePanel's spelling of the base's HWND; the two names meet here.
  return PhiWindow::hwnd();
}

PhiPalette PhiReaderPanel::QueryPalette() const {
  // The ORIGIN SLOT's palette, not its browser's: the two numbers only
  // coincide until that slot switches Space.
  if (coordinator_ && origin_) {
    return coordinator_->PaletteForSlot(origin_->slot().value);
  }
  return PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kLight);
}
void PhiReaderPanel::BindSurfaceTab(int64_t surface_tab_id) {
  surface_tab_id_ = surface_tab_id;
}

PhiReaderPanel::~PhiReaderPanel() {
  // Out from under the panel before it goes, hidden first — never a
  // visible page at the desktop's top-left while Chromium closes it.
  PhiReleaseHostedChild(browser_hwnd_);
  // The base clears GWLP_USERDATA at WM_NCDESTROY, so nothing delivered
  // during the teardown can reach a half-destroyed object.
  Destroy();
}

void PhiReaderPanel::Reposition() {
  if (!alive() || !origin_) {
    return;
  }
  SyncDpiFromParent();
  // The page below the address row, never the row itself: since the
  // Windows-native layout (docs/windows-native-ui.md §4) the row carries the
  // caption buttons and the tool cluster, which stay in reach while reading
  // — the reader is a surface over the page, not over the window's chrome.
  // A pane of the split on screen: the reader covers that pane only, so the
  // other pane — and its own reader — stay in view beside it. Chromium lays
  // the panes out; before it has, the rect is empty and the framework's
  // OnContentBoundsChanged re-fits this panel once it has.
  RECT pane = {};
  if (origin_->SplitPaneRectForTab(static_cast<int>(origin_tab_id_), &pane)) {
    if (::IsRectEmpty(&pane)) {
      return;
    }
    // Inside the pane's edge, like a pane's new-tab page: the framework
    // strokes the focused pane's 2 dip accent ring within the pane's own
    // bounds, and the ring is what shows which pane is focused.
    ::InflateRect(&pane, -Scaled(2), -Scaled(2));
    in_pane_ = true;
    SetPanelBounds(pane);
    PositionContent();
    ApplyCardShape();
    Invalidate();
    return;
  }
  const RECT content = origin_->ContentRect();
  const int width = content.right - content.left;
  const int height = content.bottom - content.top;
  if (width <= 0 || height <= 0) {
    return;
  }
  in_pane_ = false;
  SetPanelBounds(content);
  PositionContent();
  ApplyCardShape();
  Invalidate();
  // The caps were placed for the card this panel now covers; re-assert them
  // over it so the reading surface wears the same rounded corners.
  if (origin_) {
    origin_->RaiseCornerCaps();
  }
}

void PhiReaderPanel::ApplyCardShape() {
  if (!alive()) {
    return;
  }
  if (in_pane_) {
    // Square, but where the pane meets the content card's rounded corner:
    // the card's radius less the ring there, concentric with it. The fringe
    // carries the arc's partial pixels, over the framework's pane chrome no
    // cap could match.
    RECT pane = {};
    const unsigned corners =
        origin_ && origin_->SplitPaneRectForTab(
                       static_cast<int>(origin_tab_id_), &pane)
            ? origin_->SplitPaneCardCorners(pane)
            : 0;
    SetCornerFringe(
        origin_ ? origin_->ContentCornerRadius() - Scaled(2) : 0, corners,
        QueryPalette().card);
    return;
  }
  // Square: the panel covers the content card exactly, and the shell's
  // antialiased corner caps round the card from ABOVE it — the reader comes
  // up before them on every caps raise (kMsgRaiseCaps), and Reposition and
  // the show paths ask for one. The region this used to cut stair-stepped
  // the same curve under the caps, and was all that showed whenever they
  // were not on top.
  ClearCornerFringe();
}

void PhiReaderPanel::OnThemeChanged() {
  PhiPanel::OnThemeChanged();
  ApplyCardShape();  // The pane fringe keeps the colour it was made with.
}

void PhiReaderPanel::Raise() {
  if (!IsPanelVisible()) {
    return;
  }
  RaisePanel();
}

void PhiReaderPanel::SetSurfaceShown(bool shown) {
  if (!alive()) {
    return;
  }
  if (shown) {
    ShowPanel();
    if (origin_ && !in_pane_) {
      origin_->RaiseCornerCaps();
    }
  } else {
    HidePanel();
  }
}

void PhiReaderPanel::RefreshState() {
  if (!framework_ || expanded_) {
    return;
  }
  // Read the STRIP, not the nav state: a panel whose surface tab is being
  // removed (reader.close, or the reading page's own X) has an empty strip
  // and reports a placeholder page, which is not a navigation and must not
  // be mistaken for one — the browser is about to close on its own. The
  // strip is the model's snapshot of this browser, pulled typed when the
  // model has not been pushed it yet (the panel is built inside its
  // Browser::Create).
  const phi_framework::WindowSnapshot* snapshot =
      model_ ? model_->Get(window_id_) : nullptr;
  phi_framework::WindowSnapshot pulled;
  if (!snapshot && PhiWindowModel::Pull(framework_, window_id_, &pulled)) {
    snapshot = &pulled;
  }
  if (!snapshot || snapshot->tabs.size() != 1) {
    return;
  }
  const phi_framework::TabSnapshot& row = snapshot->tabs.front();
  const bool is_surface = row.surface == phi_framework::TabSurface::kReader;
  if (!surface_tab_id_) {
    // The framework panelled the surface itself (the insertion divert), so
    // nobody bound it here: adopt the reader page standing for our origin.
    if (is_surface && row.origin.value == origin_tab_id_) {
      surface_tab_id_ = row.id.value;
    }
    return;
  }
  if (row.id.value != surface_tab_id_) {
    // Not the tab that was moved in: the surface has gone and this browser
    // is on its way out with a placeholder page. Nothing to hand back.
    return;
  }
  if (row.url.empty()) {
    return;
  }
  url_ = base::UTF8ToWide(row.url);
  if (is_surface || row.url == "about:blank") {
    return;
  }
  // A link clicked inside the reader. The reading surface is not a place to
  // browse from, so the destination becomes a normal tab in the origin
  // window and the panel goes away. Guarded so the teardown runs once.
  expanded_ = true;
  LOG(INFO) << "[PhiClient] reader panel " << window_id_.value
            << " navigated away — expanding into a tab";
  if (coordinator_) {
    coordinator_->ExpandReaderPanelIntoTab(window_id_.value, row.url);
  }
}

void PhiReaderPanel::PositionContent() {
  if (!browser_hwnd_ || !::IsWindow(browser_hwnd_)) {
    return;
  }
  // The reading page fills the panel: no header (Jixiang, 2026-09-20) —
  // the address row above it stays live, and the reader closes from its
  // own HUD, the omnibox button, Ctrl+Alt+R or the row's indicator tile.
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  ::SetWindowPos(browser_hwnd_, nullptr, 0, 0, client.right, client.bottom,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

int64_t PhiReaderPanel::surface_tab_id() const {
  return surface_tab_id_;
}

void PhiReaderPanel::Paint(HDC dc, const RECT&) {
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  const PhiPalette palette = QueryPalette();
  HBRUSH ground = ::CreateSolidBrush(palette.ground_top);
  ::FillRect(dc, &client, ground);
  ::DeleteObject(ground);
}

void PhiReaderPanel::OnSize(int, int) {
  PositionContent();
}

}  // namespace phi_client

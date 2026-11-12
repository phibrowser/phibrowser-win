// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_NEW_TAB_PAGE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_NEW_TAB_PAGE_H_

#include <windows.h>

#include <string>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_panel.h"
#include "client/phi_theme.h"

namespace phi_client {

class PhiShellWindow;

// The Mac's native new-tab page (UserInterface/WebContent/NativeNTP/
// NewTabViewController.swift): the Phi mark, the "Incognito" caption in an
// off-the-record window, and an omnibox centred in the content card — drawn
// by the client instead of Chromium's web NTP.
//
// URL-DRIVEN like the bookmark manager: the page is a child window mounted
// over the content card while the active tab sits on a new-tab URL, and
// unmounted the moment it navigates away. The tab under it is an ordinary
// browser tab (chrome://newtab keeps loading behind the page, so CDP and the
// sidebar see the same tab they always did); only what the user SEES is the
// native page. The Mac shows this page for incognito windows and its AI web
// NTP elsewhere; this client has no AI NTP, so every new tab gets it.
//
// The omnibox is a real EDIT owned by this page but driven by the window
// controller's ONE omnibox session (query, suggestion dropdown, commit) —
// the controller subclasses the field exactly like its own address field.
//
// IN A SPLIT the page covers ONE PANE, not the card: the host mounts a page
// per new-tab pane (the Mac's per-pane WebContentViewController each hosting
// its own NewTabViewController) and places it with SetPlacement. A click on
// a pane's page activates that pane's tab, as a click on its web content
// would — the omnibox session commits into the active tab, so the pane the
// user is typing in must be the active one.
class PhiNewTabPage : public PhiPanel {
 public:
  explicit PhiNewTabPage(PhiShellWindow* controller);
  ~PhiNewTabPage() override;

  PhiNewTabPage(const PhiNewTabPage&) = delete;
  PhiNewTabPage& operator=(const PhiNewTabPage&) = delete;

  // True for the new-tab URLs (the Mac String.isNTP, plus the WebUI spelling
  // Chromium itself redirects to).
  static bool IsNewTabUrl(const std::string& url);

  // Mounts the page over the content card in its clean state — controls
  // visible, omnibox empty. Focus is the controller's call.
  void Show();
  void Hide();
  bool visible() const;
  HWND field() const { return field_; }
  // True while the page is up WITH its omnibox showing — the field is then
  // where keyboard focus belongs (the Mac focuses it in viewDidAppear).
  bool WantsFocus() const;
  // True after HideControls, until Show or Reset.
  bool controls_hidden() const { return controls_hidden_; }

  // Where the page sits: over `pane` (shell client coordinates) for the tab
  // `tab_id`, or over the whole content card when `pane` is null (`tab_id`
  // then 0). Moves a mounted page at once.
  void SetPlacement(int tab_id, const RECT* pane);
  int tab_id() const { return tab_id_; }
  // Follows the content card / its pane (and the monitor's DPI) on relayout.
  void Reposition();
  // Repaints — a palette change.
  void Refresh();
  // The Mac's omniBoxDidClear: a navigation was committed from the omnibox,
  // so the controls blank out until the rebuild unmounts the page. Nothing
  // of the old page flashes over the arriving one.
  void HideControls();
  // The Mac's resetToInitialState: a Space URL rule re-homed the navigation
  // and this tab is a new tab again — controls back, omnibox cleared.
  void Reset();
  void FocusField();
  // The omnibox box in SCREEN coordinates — the suggestion dropdown anchors
  // flush under it, as wide as it is.
  RECT BoxScreenRect() const;

 protected:
  // PhiPanel: model, paint and hit handling only — the registration, the
  // `this` binding, the cached back buffer, the fonts and the dpi all come
  // from the base (phi_window.h).
  void Paint(HDC dc, const RECT& dirty) override;
  void OnSize(int width, int height) override;
  void OnDpiChanged(UINT old_dpi) override;
  void OnMouseDown(int hit, POINT client_pt) override;
  PhiPalette QueryPalette() const override;
  // The base repaints the page; the EDIT child and a pane's corner fringe
  // carry colours of their own and are refreshed here.
  void OnThemeChanged() override;
  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) override;

 private:
  struct Geometry {
    RECT icon{};
    RECT label{};  // Empty outside incognito.
    RECT box{};
  };

  void EnsureWindow();
  void EnsureField();
  // The three faces, out of the base's per-dpi font set. const because
  // ComputeGeometry() measures with them and BoxScreenRect() is const.
  HFONT LabelFont() const;
  HFONT FieldFont() const;
  HFONT GlyphFont() const;
  void PushFieldFont();
  // A pane-hosted page's rounded corners (see Reposition), in the page's
  // current ground.
  void ApplyPaneCorners();
  Geometry ComputeGeometry() const;
  void LayoutField();

  RAW_PTR_EXCLUSION PhiShellWindow* const controller_;
  HWND field_ = nullptr;
  // The EDIT's ground (WM_CTLCOLOREDIT must return a REAL brush).
  HBRUSH field_brush_ = nullptr;
  COLORREF field_brush_color_ = CLR_INVALID;
  bool controls_hidden_ = false;
  // The pane this page covers: 0 / no rect = the whole content card.
  int tab_id_ = 0;
  bool has_pane_ = false;
  RECT pane_ = {};
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_NEW_TAB_PAGE_H_

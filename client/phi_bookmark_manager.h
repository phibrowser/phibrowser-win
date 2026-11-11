// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_BOOKMARK_MANAGER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_BOOKMARK_MANAGER_H_

#include <windows.h>

#include <set>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_panel.h"
#include "client/phi_theme.h"

namespace phi_client {

class PhiShellWindow;

// The Mac's native bookmark manager (UserInterface/WebContent/BookmarkManager,
// docs/plans/2026-08-12-native-bookmark-manager.md): the Space's bookmark tree
// as a two-column outline, replacing Chromium's own bookmark surface.
//
// URL-DRIVEN, exactly as the Mac specifies: the page is not a window mode but
// a view mounted over the content card while the active tab sits on
// chrome://bookmarks. The tab stays a real browser tab — closing, moving and
// splitting it keep normal semantics — and navigating away unmounts the page.
// Structurally this is the Group Overview's child-window trick, held for the
// life of the tab instead of a keypress.
//
// Columns follow the spec: Website carries the disclosure, the folder symbol
// or the favicon(s) and the title; Address carries a folder's child count, a
// bookmark's URL, or "primary | secondary" for a split.
class PhiBookmarkManager : public PhiPanel {
 public:
  explicit PhiBookmarkManager(PhiShellWindow* controller);
  ~PhiBookmarkManager() override;

  PhiBookmarkManager(const PhiBookmarkManager&) = delete;
  PhiBookmarkManager& operator=(const PhiBookmarkManager&) = delete;

  // True when `url` is the bookmark-manager route. Both the Chromium spelling
  // and the Phi one resolve here, as URLProcessor normalizes them on Mac.
  static bool IsManagerUrl(const std::string& url);
  // What the menu item navigates to.
  static const char* ManagerUrl();

  // Shows the page over the content card and refreshes it from the store.
  void Show();
  // Takes it down, leaving the Chromium view to paint again.
  void Hide();
  bool visible() const;

  // Follows the content card when the shell relayouts.
  void Reposition();
  // Re-reads the tree (a store mutation happened elsewhere).
  void Refresh();

 protected:
  // PhiPanel: the outline model, Paint and the hit handling. Registration,
  // the `this` binding, the cached back buffer (this file held one of the
  // ten copies of the block), the fonts, the dpi and hover all come from
  // the base.
  void Paint(HDC dc, const RECT& dirty) override;
  void OnSize(int width, int height) override;
  void OnDpiChanged(UINT old_dpi) override;
  void OnMouseDown(int hit, POINT client_pt) override;
  void OnWheel(int delta, POINT client_pt) override;
  bool OnKey(WPARAM virtual_key) override;
  PhiPalette QueryPalette() const override;
  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) override;

 private:
  // One visible outline row — the flattened projection the Mac builds in
  // BookmarkManagerProjection, including its display signature inputs.
  struct Row {
    std::string guid;
    std::string parent_guid;
    std::wstring title;
    std::string url;
    std::wstring title_b;  // Split secondary.
    std::string url_b;
    std::string icon;  // Folder catalog symbol.
    bool folder = false;
    int depth = 0;
    bool expanded = false;
    int child_count = 0;
    std::vector<unsigned char> favicon;
    int favicon_width = 0;
    int favicon_height = 0;
    std::vector<unsigned char> favicon_b;
    int favicon_b_width = 0;
    int favicon_b_height = 0;
    RECT bounds{};      // Row rect in client coords, scroll applied.
    RECT disclosure{};  // The folder's twisty.
  };

  void EnsureWindow();
  void EnsureSearchField();
  void LayoutChrome();
  // THE rect list (phi_window.h contract rule 6): every row, then the New
  // Folder button on top of them, so a row scrolled under the header
  // cannot take the button's pointer.
  void RebuildHits();
  bool HeaderButtonHot(int key) const;

  // Rebuilds `rows_` from the store: the outline in normal mode, or the flat
  // depth-first match list while a search is active.
  void Rebuild();
  void AppendSubtree(const std::string& parent_guid, int depth);
  void AppendMatches(const std::string& parent_guid);
  // Positions `rows_` at the (clamped) scroll offset and rebuilds the hits;
  // no store read, so a wheel notch stays cheap.
  void LayoutRows();
  // The client area below the header — the only part rows paint or hit in.
  RECT BodyRect() const;
  bool RowVisible(const Row& row) const;

  void PaintRow(HDC dc, const Row& row, const PhiPalette& palette);
  int RowAt(POINT pt) const;  // -1 when the point is on no row.
  int ContentHeight() const;
  void ClampScroll();

  void ClickAt(POINT pt, bool ctrl, bool shift);
  void OnDoubleClick(POINT pt);
  void OnContextMenu(POINT pt);
  void OpenRow(const Row& row);
  void DeleteSelection();
  void NewFolder(const std::string& parent_guid);
  void EditRow(const Row& row);
  // The Mac's BookmarkManagerAnalyticsSession: one session per mount
  // (Show..Hide) — bookmark_manager_opened at the start, and
  // bookmark_manager_edited at the end only when an edit committed in
  // between. Nothing about the bookmarks themselves.
  void BeginSession();
  void EndSession();
  void MarkEdited() { session_edited_ = true; }
  // Ordered selected guids, in visible order, as the spec requires actions to
  // derive them at invocation time.
  std::vector<std::string> SelectedGuids() const;

  RAW_PTR_EXCLUSION PhiShellWindow* const controller_;
  HWND search_ = nullptr;
  // Cached background for the search EDIT: WM_CTLCOLOREDIT must hand back a
  // REAL brush — a null one leaves the field unpainted (it renders black).
  HBRUSH field_brush_ = nullptr;
  COLORREF field_brush_color_ = 0;

  std::vector<Row> rows_;
  std::set<std::string> expanded_;
  std::set<std::string> selected_;
  std::string anchor_guid_;  // Shift-range origin.
  std::wstring query_;
  int scroll_ = 0;
  RECT new_folder_button_{};
  RECT import_button_{};
  bool session_open_ = false;
  bool session_edited_ = false;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_BOOKMARK_MANAGER_H_

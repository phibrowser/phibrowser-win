// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_KIOSK_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_KIOSK_WINDOW_H_

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_secondary_window.h"
#include "phibrowser-win/client/phi_theme.h"

namespace phi_framework {
class PhiFramework;
struct WindowSnapshot;
}

namespace phi_client {

class PhiCoordinator;
class PhiWindowModel;

// The Mac's Kiosk browser window (UserInterface/Kiosk): an ephemeral,
// single-tab window for a link opened outside the user's Spaces — an external
// link, a URL rule routed to Kiosk, or the "Open Link in Kiosk" context-menu
// item. Deliberately minimal: no sidebar, no tab list, no Spaces, just a
// compact toolbar over the page.
//
// The FRAMEWORK owns the browser (a popup-backed Browser created with
// is_kiosk, see phi_kiosk_window.cc in the fork); this class only owns the
// native window that hosts it. It is built the same way as the main shell —
// Chromium's HWND is restyled into a WS_CHILD and clipped into the content
// rect — but from a much smaller controller, because none of the shell's
// sidebar/tab machinery applies to a window that can only ever hold one tab.
//
// The chrome is ONE band, the Mac's KioskBrowserToolbar: the profile chip,
// the page's domain and the extensions/copy actions ride inside a single
// address capsule, with "Open in <Space>" trailing it. The Mac spends its
// leading 78 points clearing the traffic lights; here that room is at the
// TRAILING edge instead, because the window controls are Windows' own —
// minimize / maximize / close, so the band is title bar and toolbar at
// once. That frame is NOT hand-rolled here any more: this is a
// PhiSecondaryWindow whose PhiPopupChrome is told the band is 52dip
// instead of 32 and that the toolbar's own controls claim their pixels
// (CaptionClaimsClient). Lane 3 §2 counted four copies of the
// WM_NCCALCSIZE / WM_NCHITTEST / hot-button code; this was the fourth.
//
// Not ported from the Mac (deliberately, and not needed for the window to
// work): the cursor-zoom open animation, the global shortcut, frame autosave,
// the omnibox behind the address capsule (this client has none outside the
// shell, so the capsule shows the domain and takes no clicks), and switching
// the window's profile from the chip (the Mac rebuilds the Kiosk in the
// chosen profile; no framework call opens a Kiosk, so the chip names the
// profile without offering the menu). See windows-framework-status.md.
class PhiKioskWindow : public PhiSecondaryWindow {
 public:
  // `model` is the coordinator's window model: the toolbar's url, title
  // and back state are read from this browser's snapshot there
  // (docs/architecture-review.md §3.3).
  PhiKioskWindow(int64_t window_id,
                 HWND browser_hwnd,
                 phi_framework::PhiFramework* framework,
                 const PhiWindowModel* model,
                 PhiCoordinator* coordinator,
                 bool incognito);
  ~PhiKioskWindow() override;

  PhiKioskWindow(const PhiKioskWindow&) = delete;
  PhiKioskWindow& operator=(const PhiKioskWindow&) = delete;

  int64_t window_id() const { return window_id_.value; }

  // The framework says this window's tab or navigation state moved: re-read
  // the title/URL/can-go-back and repaint the toolbar.
  void RefreshState();

 private:
  // PhiSecondaryWindow.
  void Paint(HDC dc, const RECT& dirty) override;
  LRESULT OnWindowMessage(UINT msg,
                          WPARAM wparam,
                          LPARAM lparam,
                          bool* handled) override;
  void OnSize(int width, int height) override;
  void OnDpiChanged(UINT old_dpi) override;
  void OnTimerFired(UINT_PTR id) override;
  // The window's colours: the band stands on the app palette's ground,
  // and its window controls take that palette's own hover and label —
  // the Kiosk never dimmed them when the window went inactive.
  PhiPopupColors QueryColors() const override;
  // The band's own toolbar controls take their clicks; the capsule's
  // bare run deliberately does not, so it drags (the Mac's
  // isMovableByWindowBackground).
  bool CaptionClaimsClient(POINT client_pt) const override;
  void OnFramePointer() override;

  // Toolbar geometry, all in physical pixels for the current DPI.
  struct Layout {
    RECT band;  // Title bar and toolbar in one row.
    RECT caption_min;
    RECT caption_max;
    RECT caption_close;
    RECT back;
    RECT address;  // The capsule holding the three below.
    RECT profile;  // The profile chip inside it.
    RECT url;
    RECT extensions;
    RECT copy;
    RECT space_menu;     // "Open in <Space>", the whole capsule.
    RECT space_primary;  // Its opening half.
    RECT space_chevron;  // Its Space-list half.
    RECT content;
  };
  Layout ComputeLayout();

  void EnsureFonts();
  int TextWidth(HFONT font, const std::wstring& text);
  // Re-measures the band's variable-width labels. Measuring in
  // ComputeLayout instead would put four text extents behind every
  // WM_NCHITTEST, and the window sends one per mouse move.
  void MeasureLabels();
  // The Mac's URLProcessor.displayName: the www.-stripped host, and nothing
  // at all for about:blank, the new-tab page or an extension page.
  std::wstring DisplayUrl() const;
  const PhiPalette& Palette() const;

  void OnMouseMove(POINT pt);
  void OnToolbarClick(POINT pt);
  void PositionContent();
  void RefreshProfile();
  void RefreshSpaces();
  void FocusFirstPage();
  void CopyUrl();
  // Hands the page to `space_id`; this ephemeral window goes once the page
  // has opened there — the Mac's Kiosk space menu, and what the old expand
  // button did.
  void OpenInSpace(const std::string& space_id);
  void ShowSpaceList(const RECT& anchor);

  BrowserId window_id_;  // The Kiosk's browser; a Kiosk has no slot.
  HWND browser_hwnd_ = nullptr;
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  RAW_PTR_EXCLUSION const PhiWindowModel* model_ = nullptr;
  RAW_PTR_EXCLUSION PhiCoordinator* coordinator_ = nullptr;
  // Opened from an off-the-record page: its Space list holds Incognito
  // Spaces only (one to create, when none exists) — the page must never be
  // re-opened in a regular profile.
  const bool incognito_ = false;
  // This browser's snapshot: the model's copy, or pulled typed for a
  // browser the model has not been pushed yet (the window is built inside
  // its Browser::Create). `pulled` is the pull's storage.
  const phi_framework::WindowSnapshot* CurrentSnapshot(
      phi_framework::WindowSnapshot* pulled) const;

  std::wstring title_;
  std::wstring url_;
  bool can_go_back_ = false;

  // The profile this window's browser belongs to, for the chip.
  std::wstring profile_name_;

  // The Space list behind the trailing menu, and the one it offers by
  // default (the Mac's KioskSpaceMenuTargetResolver).
  struct SpaceRow {
    std::string id;
    std::wstring name;
    std::wstring icon;  // Resolved display glyph: catalog stand-in or emoji.
  };
  std::vector<SpaceRow> spaces_;
  std::string target_space_id_;
  const SpaceRow* TargetSpace() const;

  // The copy button wears a checkmark for a second after a copy; the id
  // comes from the window's own pool, not the literal 1 the file used to
  // pick (lane 3 §4.1).
  bool copy_flash_ = false;
  Timer copy_flash_timer_;

  bool first_page_focused_ = false;

  // When "Open in" last handed the page over (GetTickCount64 ms), 0 if never.
  uint64_t moved_at_tick_ = 0;

  // Which toolbar control the cursor is over, for the hover wash. Indexes
  // the same order Layout declares them; -1 for none. The WINDOW controls
  // are not here: PhiPopupChrome owns their hot and pressed state.
  int hot_ = -1;

  // Label widths, from MeasureLabels.
  int profile_text_w_ = 0;
  int space_prefix_w_ = 0;
  int space_name_w_ = 0;

  // Rebuilt whenever the DPI moves; owned here.
  HFONT ui_font_ = nullptr;
  HFONT ui_strong_font_ = nullptr;
  HFONT glyph_font_ = nullptr;
  HFONT caption_font_ = nullptr;
  UINT font_dpi_ = 0;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_KIOSK_WINDOW_H_

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_TAB_PREVIEW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_TAB_PREVIEW_H_

#include <windows.h>

#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_ids.h"
#include "client/phi_popover.h"

namespace phi_client {

class PhiShellWindow;
struct PhiPalette;

// The Mac's TabPreviewController (UserInterface/Common/TabPreview): the hover
// card that every tab surface shows once the cursor SETTLES on a row — the
// page's cached snapshot over its title and URL.
//
// It is a floating popup, not shell-DC chrome like the Space pips' card: the
// card overhangs the reparented Chromium child window, which would paint over
// anything the shell drew. Corners and shadow come from DWM, the way the
// downloads and extensions popovers get theirs.
//
// Timing is the Mac's CustomTooltipConfiguration for the .tabPreview handoff
// group: a cold hover waits 500ms, but while a card is already up the next row
// takes it over after 150ms and the panel SLIDES to the new anchor over 180ms
// (ease-in-out) instead of blinking out and back.
class PhiTabPreview : public PhiPopover {
 public:
  // Where the card hangs off its row. The Mac's CustomTooltipPlacement cases
  // that the preview call sites actually use, in Y-down terms.
  enum class Placement {
    // Sidebar tab and bookmark rows (.rightTopAttached): flush to the row's
    // right edge, card top aligned with the row's top.
    kRightTopAttached,
    // Pinned grid cells and the comfortable strip (.belowAttached): centred
    // under the cell, flush to its bottom.
    kBelowAttached,
  };

  // One previewable row, resolved by the controller from whatever it is
  // hovering. `key` is the Mac's logical target id: re-entering the SAME key
  // is a no-op, so a repaint or a tab-list rebuild never restarts the delay.
  struct Target {
    std::string key;
    // The tab's stable session id, for the snapshot. Rows with no live tab
    // are ineligible and never reach here.
    TabId tab_id;
    std::wstring title;
    std::wstring url;
    // The active tab: the Mac shows title and URL only — its snapshot is
    // whatever the user is already looking at.
    bool foreground = false;
    // chrome://bookmarks and phi://bookmarks opt out of snapshots
    // (TabPreviewURLPolicy.allowsSnapshot).
    bool allow_snapshot = true;
    // A split row previews BOTH panes in one card (the Mac's
    // SplitTabPreviewController). Pane B is set only for a split.
    TabId tab_id_b;
    std::wstring title_b;
    std::wstring url_b;
    bool foreground_b = false;
    // The split's own layout: side-by-side panes (the wire's "vertical") or
    // one above the other ("horizontal").
    bool stacked = false;
    RECT anchor{};  // Screen coords.
    Placement placement = Placement::kRightTopAttached;
  };

  explicit PhiTabPreview(PhiShellWindow* controller);
  ~PhiTabPreview() override;

  PhiTabPreview(const PhiTabPreview&) = delete;
  PhiTabPreview& operator=(const PhiTabPreview&) = delete;

  // The cursor rests on `target`. Arms the show delay, or hands the visible
  // card over to it.
  void PointerEntered(const Target& target);
  // The cursor left every previewable row.
  void PointerExited();
  // A click or a drag on the hovered row: down now, and no card for THAT row
  // until the pointer moves off it (the Mac's cancelForInteraction). Moving
  // to a different row re-arms normally.
  void Cancel();

  // DEAD SEAM, kept only so the shell's WM_TIMER branch
  // (phi_shell_window.cc) keeps compiling: the card used to arm its
  // three timers on the SHELL's HWND — a window it does not own — and the
  // shell dispatched them here first (lane 3 §4.1, ranked finding 9). It
  // owns its own window and its own timer ids now, so this always returns
  // false. Phase 6 deletes the call site and this method with it.
  bool OnTimer(UINT_PTR id);

  bool visible() const { return visible_; }

 protected:
  void Paint(HDC dc, const RECT& dirty) override;
  void OnTimerFired(UINT_PTR id) override;
  void OnDpiChanged(UINT old_dpi) override;
  PhiPalette QueryPalette() const override;

 private:
  // CustomTooltipController's timings. `kHandoffDelayMs` is a LEAVE grace,
  // not a show delay: while a card is up (or inside that grace, or inside
  // the warm window just after it), the next row takes the card over with
  // NO wait at all — only the frame animates. Only a genuinely cold hover
  // pays kShowDelayMs.
  static constexpr int kShowDelayMs = 500;
  static constexpr int kHandoffDelayMs = 150;
  static constexpr int kWarmGraceMs = 300;
  static constexpr int kSlideDurationMs = 180;

  void EnsureWindow();
  // Pulls the snapshot for `pending_` and lays the card out; false when the
  // target stopped being previewable while the delay ran.
  bool Resolve();
  // Resolve `pending_` and put it on screen — sliding the panel over if a
  // card is already up.
  void Present();
  // `preserve_warmth` keeps the group warm for kWarmGraceMs, so a hover that
  // lands right after skips the cold delay (the Mac's hideActive).
  void Hide(bool preserve_warmth);
  bool IsWarm() const;
  void ArmDismiss();
  void CancelDismiss();
  bool IsSplit() const { return shown_.tab_id_b.valid(); }
  // A title (capped at two lines) plus its URL line, at `width`.
  int TextBlockHeight(const std::wstring& title,
                      const std::wstring& url,
                      int width);
  // One pane of the card: its snapshot band (or a placeholder) over the
  // title and URL. `image_height` of 0 draws text only.
  void DrawPane(HDC dc,
                const RECT& bounds,
                int image_height,
                const std::vector<unsigned char>& shot,
                int shot_width,
                int shot_height,
                const std::wstring& title,
                const std::wstring& url,
                const PhiPalette& palette);
  // The card's size for the resolved content, and where it lands off the
  // anchor — the Mac's CustomTooltipGeometry, flipping to the other side
  // and clamping into the work area with the same 4pt margin. The flip and
  // the clamp are PhiPlacePopover's now (phi_window.h); this only names
  // the side.
  SIZE CardSize();
  PhiPopover::Placement CardPlacement() const;
  void PaintCard(HDC dc);
  HFONT TitleFont();
  HFONT UrlFont();

  RAW_PTR_EXCLUSION PhiShellWindow* const controller_;

  // The card on screen, and the one the armed timer will promote to.
  Target shown_;
  Target pending_;
  bool visible_ = false;
  bool armed_ = false;
  bool dismiss_armed_ = false;
  // GetTickCount64 deadline for the warm window; 0 = cold.
  ULONGLONG warm_until_ = 0;
  // Set by Cancel(): the row that was just clicked or dragged. It gets no
  // card again until the pointer genuinely moves off it, so a click never
  // makes the card blink straight back up.
  std::string suppressed_key_;

  // Resolved content for `pending_`, moved to the card on promote.
  std::vector<unsigned char> snapshot_;
  int snapshot_width_ = 0;
  int snapshot_height_ = 0;
  std::vector<unsigned char> snapshot_b_;
  int snapshot_b_width_ = 0;
  int snapshot_b_height_ = 0;
  // The Mac's .compact mode: a foreground pane (or a snapshot the policy
  // refuses) drops the whole card to text only.
  bool compact_ = false;

  // The card's own timers, out of PhiWindow's per-window pool.
  Timer show_timer_;
  Timer dismiss_timer_;
  // Handoff slide (the Mac's animator().setFrame over handoffFrameAnimation-
  // Duration), on the shared PhiAnimator's one 16 ms clock.
  int slide_ = 0;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_TAB_PREVIEW_H_

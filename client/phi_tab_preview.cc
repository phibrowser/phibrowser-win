// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_tab_preview.h"

#include <dwmapi.h>

#include <algorithm>
#include <string>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_animator.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_shell_window.h"

namespace phi_client {

namespace {

// TabPreviewView.Metrics, in DIP. The snapshot keeps the Mac's 16:10 crop.
constexpr int kCardWidth = 280;
constexpr int kImageHeight = kCardWidth * 10 / 16;
constexpr int kPadX = 12;
constexpr int kPadY = 10;
constexpr int kTextGap = 4;   // Title-to-URL spacing.
constexpr int kTitleSize = 12;  // windows-native-ui.md §5.6: 12/600 over 11.
constexpr int kUrlSize = 11;
// CustomTooltipGeometry: the gap an attached placement does NOT take, and
// the margin the card keeps off the screen edge.
constexpr int kScreenMargin = 4;

const wchar_t kClassName[] = L"PhiTabPreview";

// SwiftUI's .truncationMode(.middle). GDI has DT_PATH_ELLIPSIS, but that is
// BACKSLASH-path logic — handed a URL it collapses the line to nothing — and
// DT_END_ELLIPSIS would drop the tail the Mac deliberately keeps.
std::wstring MiddleElide(HDC dc, const std::wstring& text, int width) {
  if (text.empty() || width <= 0) {
    return std::wstring();
  }
  SIZE extent = {};
  if (::GetTextExtentPoint32W(dc, text.c_str(),
                              static_cast<int>(text.size()), &extent) &&
      extent.cx <= width) {
    return text;
  }
  const std::wstring kEllipsis = L"…";
  std::wstring best = kEllipsis;
  int lo = 0;
  int hi = static_cast<int>(text.size());
  while (lo <= hi) {
    const int keep = (lo + hi) / 2;
    const size_t head = static_cast<size_t>((keep + 1) / 2);
    const size_t tail = static_cast<size_t>(keep / 2);
    std::wstring candidate =
        text.substr(0, head) + kEllipsis + text.substr(text.size() - tail);
    if (::GetTextExtentPoint32W(dc, candidate.c_str(),
                                static_cast<int>(candidate.size()),
                                &extent) &&
        extent.cx <= width) {
      best = std::move(candidate);
      lo = keep + 1;
    } else {
      hi = keep - 1;
    }
  }
  return best;
}

}  // namespace

PhiTabPreview::PhiTabPreview(PhiShellWindow* controller)
    : controller_(controller) {}

PhiTabPreview::~PhiTabPreview() {
  Hide(/*preserve_warmth=*/false);
  Destroy();
}

HFONT PhiTabPreview::TitleFont() {
  return fonts().Get(kTitleSize, FW_SEMIBOLD);
}

HFONT PhiTabPreview::UrlFont() {
  return fonts().Get(kUrlSize, FW_NORMAL);
}

PhiPalette PhiTabPreview::QueryPalette() const {
  return controller_ ? controller_->CurrentPalette()
                     : PhiResolvePalette(PhiThemeById("pure"),
                                         PhiAppearance::kSystem);
}

void PhiTabPreview::EnsureWindow() {
  if (alive()) {
    return;
  }
  HWND owner = controller_ ? controller_->shell_hwnd() : nullptr;
  if (!owner) {
    return;
  }
  ClassSpec spec;
  spec.name = kClassName;
  spec.cursor = nullptr;

  Options options;
  // Owned by the shell, so Windows keeps it above the shell AND above the
  // Chromium child inside it — without WS_EX_TOPMOST, which would leave
  // the card floating over other applications.
  options.activate = false;
  options.foreground = false;
  options.dismiss_on_inactive = false;
  options.dismiss_on_escape = false;
  // Never take the pointer: the card lives entirely off the hover it is
  // showing for.
  options.click_through = true;

  PhiPopover::Placement placement;
  placement.side = PhiPopoverSide::kExplicit;
  placement.clamp = false;
  SIZE probe = {1, 1};
  // Created hidden and zero-sized; Present() sizes, places and shows it.
  CreatePopover(spec, owner, probe, placement, options);
}

void PhiTabPreview::OnDpiChanged(UINT) {
  // Re-measure the card that is UP (shown_), not the one a delay is armed
  // for: the fonts the base just rebuilt change every text block's height.
  if (!visible_) {
    return;
  }
  MoveTo(PlacementRect(CardPlacement(), CardSize()));
  Invalidate();
}

bool PhiTabPreview::IsWarm() const {
  return warm_until_ != 0 && ::GetTickCount64() < warm_until_;
}

void PhiTabPreview::ArmDismiss() {
  if (!alive() || dismiss_armed_) {
    return;
  }
  if (!dismiss_timer_.valid()) {
    dismiss_timer_ = NewTimer();
  }
  dismiss_armed_ = true;
  dismiss_timer_.Start(kHandoffDelayMs);
}

void PhiTabPreview::CancelDismiss() {
  if (!dismiss_armed_) {
    return;
  }
  dismiss_armed_ = false;
  dismiss_timer_.Stop();
}

void PhiTabPreview::PointerEntered(const Target& target) {
  if (!controller_ || !target.tab_id.valid()) {
    PointerExited();
    return;
  }
  EnsureWindow();  // The card owns the HWND its timers live on.
  if (!alive()) {
    return;
  }
  // Re-entering the row the card already belongs to must not restart
  // anything — the controller re-resolves hover on every mouse move.
  if (visible_ && shown_.key == target.key) {
    shown_.anchor = target.anchor;
    CancelDismiss();
    return;
  }
  if (armed_ && pending_.key == target.key) {
    pending_.anchor = target.anchor;
    return;
  }
  // Suppression is per-row: leaving the clicked row re-arms everything.
  if (!suppressed_key_.empty()) {
    if (suppressed_key_ == target.key) {
      return;
    }
    suppressed_key_.clear();
  }

  // Coming from another row cancels that row's pending dismissal: this is a
  // handoff, not a leave.
  CancelDismiss();
  pending_ = target;

  if (visible_ || IsWarm()) {
    // The Mac's hasActiveRequestInGroup / isWarm path: straight to screen,
    // with only the frame animating.
    armed_ = false;
    show_timer_.Stop();
    Present();
    return;
  }

  armed_ = true;
  if (!show_timer_.valid()) {
    show_timer_ = NewTimer();
  }
  show_timer_.Start(kShowDelayMs);
}

void PhiTabPreview::PointerExited() {
  suppressed_key_.clear();
  armed_ = false;
  pending_ = Target();
  show_timer_.Stop();
  // A visible card does NOT go down on the spot: the gap between two rows is
  // a leave, and dropping the card there would make every row-to-row move
  // pay the cold delay again. Give the next row the Mac's handoff grace.
  if (visible_) {
    ArmDismiss();
    return;
  }
  Hide(/*preserve_warmth=*/false);
}

void PhiTabPreview::Cancel() {
  // Whichever row the interaction landed on is the one to hold down.
  std::string key = visible_ ? shown_.key : pending_.key;
  armed_ = false;
  pending_ = Target();
  show_timer_.Stop();
  CancelDismiss();
  Hide(/*preserve_warmth=*/false);
  suppressed_key_ = std::move(key);
}

bool PhiTabPreview::OnTimer(UINT_PTR) {
  return false;  // See the header: the shell no longer owns our timers.
}

void PhiTabPreview::OnTimerFired(UINT_PTR id) {
  if (id == show_timer_.id()) {
    show_timer_.Stop();
    if (armed_) {
      armed_ = false;
      Present();
    }
    return;
  }
  if (id == dismiss_timer_.id()) {
    CancelDismiss();
    Hide(/*preserve_warmth=*/true);
  }
}

bool PhiTabPreview::Resolve() {
  snapshot_.clear();
  snapshot_width_ = 0;
  snapshot_height_ = 0;
  snapshot_b_.clear();
  snapshot_b_width_ = 0;
  snapshot_b_height_ = 0;
  compact_ = false;
  if (!controller_ || !pending_.tab_id.valid()) {
    return false;
  }
  const bool split = pending_.tab_id_b.valid();
  // The Mac's previewMode: a pane the user is already looking at, or a page
  // the snapshot policy refuses, drops the card to text only.
  compact_ = pending_.foreground || !pending_.allow_snapshot ||
             (split && pending_.foreground_b);
  if (compact_) {
    return true;
  }
  phi_framework::PhiFramework* framework = controller_->framework();
  if (!framework) {
    compact_ = true;
    return true;
  }
  const BrowserId browser = controller_->active_browser_id();
  if (!browser.valid()) {
    compact_ = true;  // No browser attached: no page to snapshot.
    return true;
  }
  const auto pull = [&](TabId tab_id, std::vector<unsigned char>* out,
                        int* out_width, int* out_height) {
    int width = 0;
    int height = 0;
    const unsigned char* pixels =
        framework->TabThumbnailBgra(browser.value, tab_id.value, &width,
                                    &height);
    if (!pixels || width <= 0 || height <= 0) {
      return;
    }
    // The framework's buffer is scratch — valid only until its next call, so
    // pane A is copied out before pane B is asked for.
    const size_t bytes =
        static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    // SAFETY: TabThumbnailBgra reports the dimensions of the buffer it just
    // returned, the same contract TabFaviconBgra carries.
    const auto copy =
        UNSAFE_BUFFERS(base::span<const unsigned char>(pixels, bytes));
    out->assign(copy.begin(), copy.end());
    *out_width = width;
    *out_height = height;
  };
  pull(pending_.tab_id, &snapshot_, &snapshot_width_, &snapshot_height_);
  if (split) {
    pull(pending_.tab_id_b, &snapshot_b_, &snapshot_b_width_,
         &snapshot_b_height_);
  }
  return true;
}

int PhiTabPreview::TextBlockHeight(const std::wstring& title,
                                   const std::wstring& url,
                                   int width) {
  HDC screen = ::GetDC(nullptr);
  if (!screen) {
    return 0;
  }
  HGDIOBJ prev = ::SelectObject(screen, TitleFont());
  RECT measure = {0, 0, std::max(1, width), 0};
  PhiDrawTextAA(screen, title.c_str(), -1, &measure,
                DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  TEXTMETRICW metrics = {};
  ::GetTextMetricsW(screen, &metrics);
  const int line = metrics.tmHeight;
  int height = std::min<int>(measure.bottom - measure.top, 2 * line);
  height = std::max(height, line);
  if (!url.empty()) {
    ::SelectObject(screen, UrlFont());
    ::GetTextMetricsW(screen, &metrics);
    height += Scaled(kTextGap) + metrics.tmHeight;
  }
  ::SelectObject(screen, prev);
  ::ReleaseDC(nullptr, screen);
  return height;
}

SIZE PhiTabPreview::CardSize() {
  const int width = Scaled(kCardWidth);
  const int pad_x = Scaled(kPadX);
  const int pad_y = Scaled(kPadY);
  SIZE size = {width, 0};

  if (!IsSplit()) {
    // A single tab shows its band only when there IS a cached snapshot —
    // the Mac's `if let image`, no placeholder.
    const int band =
        (!compact_ && !snapshot_.empty()) ? Scaled(kImageHeight) : 0;
    size.cy = band + TextBlockHeight(shown_.title, shown_.url,
                                     width - 2 * pad_x) +
              2 * pad_y;
    return size;
  }

  if (!shown_.stacked) {
    // Side by side (SplitLayout.vertical): one full-height band across two
    // half-width panes, sized to the taller text block.
    const int pane_w = (width - 1) / 2;
    const int band = compact_ ? 0 : Scaled(kImageHeight);
    const int text = std::max(
        TextBlockHeight(shown_.title, shown_.url, pane_w - 2 * pad_x),
        TextBlockHeight(shown_.title_b, shown_.url_b, pane_w - 2 * pad_x));
    size.cy = band + text + 2 * pad_y;
    return size;
  }

  // Stacked (SplitLayout.horizontal): two full-width panes, each on a
  // half-height band, with a hairline between them.
  const int band = compact_ ? 0 : Scaled(kImageHeight) / 2;
  const int avail = width - 2 * pad_x;
  size.cy = band + TextBlockHeight(shown_.title, shown_.url, avail) +
            2 * pad_y + 1 + band +
            TextBlockHeight(shown_.title_b, shown_.url_b, avail) + 2 * pad_y;
  return size;
}

PhiPopover::Placement PhiTabPreview::CardPlacement() const {
  PhiPopover::Placement placement;
  placement.anchor = shown_.anchor;
  placement.gap_dip = 0;
  placement.margin_px = kScreenMargin;
  placement.flip = true;
  // CustomTooltipGeometry flips only when the OTHER side actually fits,
  // then clamps — which is what PhiPlacePopover does with this flag.
  placement.flip_only_if_fits = true;
  placement.clamp = true;
  switch (shown_.placement) {
    case Placement::kRightTopAttached:
      // Flush to the row's right edge, card top aligned with the row's top.
      placement.side = PhiPopoverSide::kRightTop;
      break;
    case Placement::kBelowAttached:
      // Centred under the cell, flush to its bottom.
      placement.side = PhiPopoverSide::kBelow;
      placement.align = PhiPopoverAlign::kCenter;
      break;
  }
  return placement;
}

void PhiTabPreview::Present() {
  if (!controller_ || !pending_.tab_id.valid()) {
    return;
  }
  if (!Resolve()) {
    Hide(/*preserve_warmth=*/false);
    return;
  }
  EnsureWindow();
  if (!alive()) {
    return;
  }
  const bool handoff = visible_;
  shown_ = pending_;
  const SIZE size = CardSize();
  const RECT target = PlacementRect(CardPlacement(), size);

  if (handoff) {
    // Slide the panel to its new row instead of blinking it out and back
    // (the Mac's handoffFrameAnimationDuration), on the shared animator.
    RECT from = {};
    ::GetWindowRect(hwnd(), &from);
    PhiAnimator& animator = PhiAnimator::ForThread();
    animator.Cancel(slide_);
    Invalidate();
    slide_ = animator.Animate(
        hwnd(), kSlideDurationMs, PhiEasing::kEaseInOut,
        [this, from, target](double t, RECT* dirty) {
          const int x = PhiLerpInt(from.left, target.left, t);
          const int y = PhiLerpInt(from.top, target.top, t);
          const int w = PhiLerpInt(from.right - from.left,
                                   target.right - target.left, t);
          const int h = PhiLerpInt(from.bottom - from.top,
                                   target.bottom - target.top, t);
          ::SetWindowPos(hwnd(), nullptr, x, y, w, h,
                         SWP_NOACTIVATE | SWP_NOZORDER);
          // The card resizes every frame; repaint all of it.
          *dirty = {0, 0, w, h};
        },
        [this] { slide_ = 0; });
    return;
  }

  MoveTo(target);
  Invalidate();
  Show();
  visible_ = true;
  warm_until_ = 0;
}

void PhiTabPreview::Hide(bool preserve_warmth) {
  PhiAnimator::ForThread().Cancel(slide_);
  slide_ = 0;
  const bool was_visible = visible_;
  if (alive()) {
    ::ShowWindow(hwnd(), SW_HIDE);
  }
  visible_ = false;
  shown_ = Target();
  snapshot_.clear();
  snapshot_width_ = 0;
  snapshot_height_ = 0;
  snapshot_b_.clear();
  snapshot_b_width_ = 0;
  snapshot_b_height_ = 0;
  compact_ = false;
  // Warmth only means anything after a card was actually up: a hover landing
  // inside the grace skips the cold delay, as on Mac.
  warm_until_ = (preserve_warmth && was_visible)
                    ? ::GetTickCount64() + kWarmGraceMs
                    : 0;
}

void PhiTabPreview::DrawPane(HDC dc,
                             const RECT& bounds,
                             int image_height,
                             const std::vector<unsigned char>& shot,
                             int shot_width,
                             int shot_height,
                             const std::wstring& title,
                             const std::wstring& url,
                             const PhiPalette& palette) {
  int y = bounds.top;
  if (image_height > 0) {
    RECT band = {bounds.left, y, bounds.right, y + image_height};
    if (!shot.empty()) {
      // Aspect-fill the band: crop the snapshot's overhang rather than
      // squashing it, the way SwiftUI's .aspectRatio(.fill) + .clipped() does.
      const double target = static_cast<double>(band.right - band.left) /
                            std::max(1, static_cast<int>(band.bottom - band.top));
      const double source =
          static_cast<double>(shot_width) / std::max(1, shot_height);
      RECT fill = band;
      if (source > target) {
        const int wide =
            static_cast<int>((band.bottom - band.top) * source + 0.5);
        const int overhang = (wide - (band.right - band.left)) / 2;
        fill.left -= overhang;
        fill.right += overhang;
      } else if (source < target) {
        const int tall =
            static_cast<int>((band.right - band.left) / source + 0.5);
        const int overhang = (tall - (band.bottom - band.top)) / 2;
        fill.top -= overhang;
        fill.bottom += overhang;
      }
      HRGN clip = ::CreateRectRgnIndirect(&band);
      ::SelectClipRgn(dc, clip);
      PhiDrawBgra(dc, fill, shot.data(), shot_width, shot_height);
      ::SelectClipRgn(dc, nullptr);
      ::DeleteObject(clip);
    } else {
      // A pane whose snapshot has not been recorded yet still keeps its
      // frame, so a split card never goes lopsided.
      HBRUSH ground = ::CreateSolidBrush(palette.hover);
      ::FillRect(dc, &band, ground);
      ::DeleteObject(ground);
    }
    y = band.bottom;
  }

  RECT text = {bounds.left + Scaled(kPadX), y + Scaled(kPadY),
               bounds.right - Scaled(kPadX), bounds.bottom - Scaled(kPadY)};
  if (text.right <= text.left || text.bottom <= text.top) {
    return;
  }
  HGDIOBJ prev = ::SelectObject(dc, TitleFont());
  ::SetTextColor(dc, palette.label);
  TEXTMETRICW metrics = {};
  ::GetTextMetricsW(dc, &metrics);
  // Reserve what the title ACTUALLY takes, capped at two lines — reserving
  // the cap unconditionally would push the URL past the bottom of a card
  // that CardSize() measured for a one-line title, and it would never draw.
  RECT measured = {0, 0, text.right - text.left, 0};
  PhiDrawTextAA(dc, title.c_str(), -1, &measured,
                DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  const LONG title_height =
      std::max<LONG>(metrics.tmHeight,
                     std::min<LONG>(measured.bottom - measured.top,
                                    2 * metrics.tmHeight));
  RECT title_rc = text;
  title_rc.bottom = std::min<LONG>(text.bottom, text.top + title_height);
  PhiDrawTextAA(dc, title.c_str(), -1, &title_rc,
                DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
  if (!url.empty()) {
    ::SelectObject(dc, UrlFont());
    ::SetTextColor(dc, palette.label_dim);
    RECT url_rc = text;
    url_rc.top = title_rc.bottom + Scaled(kTextGap);
    if (url_rc.top < url_rc.bottom) {
      const std::wstring line =
          MiddleElide(dc, url, url_rc.right - url_rc.left);
      ::DrawTextW(dc, line.c_str(), -1, &url_rc,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    }
  }
  ::SelectObject(dc, prev);
}

void PhiTabPreview::Paint(HDC dc, const RECT&) {
  PaintCard(dc);
}

void PhiTabPreview::PaintCard(HDC dc) {
  if (!controller_ || !alive()) {
    return;
  }
  RECT rc = {};
  ::GetClientRect(hwnd(), &rc);
  const PhiPalette& palette = PhiWindow::palette();

  // The flyout ground (§5.6); DWM rounds the corners, so the fill is a
  // plain rect.
  HBRUSH ground = ::CreateSolidBrush(palette.flyout);
  ::FillRect(dc, &rc, ground);
  ::DeleteObject(ground);
  ::SetBkMode(dc, TRANSPARENT);

  if (!IsSplit()) {
    const int band =
        (!compact_ && !snapshot_.empty()) ? Scaled(kImageHeight) : 0;
    DrawPane(dc, rc, band, snapshot_, snapshot_width_, snapshot_height_,
             shown_.title, shown_.url, palette);
  } else if (!shown_.stacked) {
    const int band = compact_ ? 0 : Scaled(kImageHeight);
    const LONG split_x = rc.left + (rc.right - rc.left - 1) / 2;
    RECT left = {rc.left, rc.top, split_x, rc.bottom};
    RECT right = {split_x + 1, rc.top, rc.right, rc.bottom};
    DrawPane(dc, left, band, snapshot_, snapshot_width_, snapshot_height_,
             shown_.title, shown_.url, palette);
    DrawPane(dc, right, band, snapshot_b_, snapshot_b_width_,
             snapshot_b_height_, shown_.title_b, shown_.url_b, palette);
    RECT divider = {split_x, rc.top, split_x + 1, rc.bottom};
    HBRUSH line = ::CreateSolidBrush(palette.separator);
    ::FillRect(dc, &divider, line);
    ::DeleteObject(line);
  } else {
    const int band = compact_ ? 0 : Scaled(kImageHeight) / 2;
    const int top_h =
        band + TextBlockHeight(shown_.title, shown_.url,
                               rc.right - rc.left - 2 * Scaled(kPadX)) +
        2 * Scaled(kPadY);
    RECT top = {rc.left, rc.top, rc.right, rc.top + top_h};
    RECT bottom = {rc.left, top.bottom + 1, rc.right, rc.bottom};
    DrawPane(dc, top, band, snapshot_, snapshot_width_, snapshot_height_,
             shown_.title, shown_.url, palette);
    DrawPane(dc, bottom, band, snapshot_b_, snapshot_b_width_,
             snapshot_b_height_, shown_.title_b, shown_.url_b, palette);
    RECT divider = {rc.left, top.bottom, rc.right, top.bottom + 1};
    HBRUSH line = ::CreateSolidBrush(palette.separator);
    ::FillRect(dc, &divider, line);
    ::DeleteObject(line);
  }

  // A hairline rim, as the Mac tooltip chrome carries — Windows 10 only:
  // on Windows 11 DWM rounds the card and draws its border along the curve,
  // and a square rim inside it would cut across both corners.
  if (dwm_rounded()) {
    return;
  }
  HPEN pen = ::CreatePen(PS_SOLID, 1, palette.border);
  HGDIOBJ old_pen = ::SelectObject(dc, pen);
  HGDIOBJ old_brush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
  ::Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
  ::SelectObject(dc, old_brush);
  ::SelectObject(dc, old_pen);
  ::DeleteObject(pen);
}

}  // namespace phi_client

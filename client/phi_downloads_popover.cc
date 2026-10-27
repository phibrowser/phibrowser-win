// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_downloads_popover.h"

#include <commctrl.h>
#include <commoncontrols.h>  // IImageList, for SHGetImageList.
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "base/compiler_specific.h"
#include "base/json/json_reader.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_downloads_model.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_library.h"
#include "phibrowser-win/client/phi_popover.h"
#include "phibrowser-win/client/phi_shell_window.h"
#include "phibrowser-win/client/phi_util.h"

namespace phi_client {

namespace {

// DownloadsListView's metrics, in DIP.
constexpr int kPanelWidth = 340;
constexpr int kPanelHeight = 317;
constexpr int kRowHeight = 56;
constexpr int kListPad = 12;   // Horizontal list padding.
constexpr int kIconSize = 30;
constexpr int kIconGap = 12;
constexpr int kActionButton = 24;
constexpr int kBottomBar = 33;

// hits_ keys: row i is i * kKeyStride, its action `a` is + 1 + a. The
// footer button has its own key. The base resolves hover and clicks over
// the list; nothing here hit-tests by hand any more.
constexpr int kKeyStride = 8;
constexpr int kAllDownloadsKey = -1;

void FillRect(HDC dc, const RECT& r, COLORREF color) {
  HBRUSH brush = ::CreateSolidBrush(color);
  ::FillRect(dc, &r, brush);
  ::DeleteObject(brush);
}

void FillRounded(HDC dc, const RECT& r, COLORREF color, int radius) {
  PhiFillRoundRectAA(dc, r, radius, color);
}

// The row shape, the JSON parse, the per-state actions, the icon cache and
// the formatters live in phi_downloads_model.h, shared with the Library's
// All Downloads list.

// The popover keeps its download rows, its Paint() and its per-row
// actions. The window, the placement, the back buffer, hover, the poll
// timer, dismissal and lifetime belong to PhiPopover (lane 3 §6.2).
class Popover : public PhiPopover {
 public:
  static Popover* instance;

  Popover(phi_framework::PhiFramework* framework, BrowserId browser)
      : framework_(framework), browser_id_(browser.value) {}

  ~Popover() override = default;  // icons_ owns its handles.

  bool Open(HWND owner, const RECT& anchor, const PhiPalette& colors) {
    ClassSpec spec;
    spec.name = L"PhiDownloadsPopover";
    // A double-click on a finished row opens the file (DownloadItemRow).
    spec.style = CS_DBLCLKS;

    Options options;  // Activating, DWM-rounded, 500 ms opening grace.

    placement_.anchor = anchor;
    placement_.side = PhiPopoverSide::kAbove;
    placement_.gap_dip = 8;
    placement_.flip = false;  // The bottom-bar button always drops upward.
    placement_.monitor_from_owner = true;

    SIZE probe = {1, 1};
    if (!CreatePopover(spec, owner, probe, placement_, options)) {
      return false;
    }
    instance = this;  // Before Show(): activation churn can destroy us.
    SetPalette(colors);
    SIZE size = {Scaled(kPanelWidth), Scaled(kPanelHeight)};
    Reposition(placement_, size);
    Refresh();
    Show();
    poll_ = NewTimer();
    poll_.Start(kPhiDownloadsPollMs);
    return true;
  }

 protected:
  void Paint(HDC dc, const RECT&) override {
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    PaintPanel(dc, client);
  }

  void OnTimerFired(UINT_PTR id) override {
    if (id == poll_.id()) {
      Refresh();
    }
  }

  void OnHover(int hit, int) override {
    hover_row_ = -1;
    hover_action_ = -1;
    hover_all_ = false;
    if (hit < 0) {
      return;
    }
    const int key = hits_[static_cast<size_t>(hit)].key;
    if (key == kAllDownloadsKey) {
      hover_all_ = true;
      return;
    }
    hover_row_ = key / kKeyStride;
    const int sub = key % kKeyStride;
    if (sub > 0) {
      hover_action_ = sub - 1;
    }
  }

  void OnWheel(int delta, POINT) override {
    scroll_ = std::max(
        0, std::min(max_scroll_, scroll_ - delta * Scaled(kRowHeight) /
                                               WHEEL_DELTA));
    BuildHits();
    Invalidate();
  }

  void OnClick(int hit, POINT) override {
    OnHover(hit, -1);
    RunHoveredAction();
  }

  void OnDpiChanged(UINT) override {
    // Placement again, not a resize in place: the panel hangs ABOVE its
    // button, so growing it with SWP_NOMOVE ran it down over the button.
    Reposition(placement_, {Scaled(kPanelWidth), Scaled(kPanelHeight)});
    icons_.Clear();  // The icons were picked for the old size.
    BuildHits();
  }

  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) override {
    if (message == WM_LBUTTONDBLCLK) {
      // The second press of a double-click. On a finished row's body it
      // opens the file; anywhere else it is an ordinary press, and the
      // WM_LBUTTONUP that follows still clicks an action button.
      const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const int hit = HitTest(pt);
      if (hit >= 0 && framework_) {
        const int key = hits_[static_cast<size_t>(hit)].key;
        const size_t row = static_cast<size_t>(key / kKeyStride);
        if (key != kAllDownloadsKey && key % kKeyStride == 0 &&
            row < rows_.size() && PhiDownloadCanOpen(rows_[row])) {
          framework_->DownloadAction(browser_id_,
                                     static_cast<uint32_t>(rows_[row].id),
                                     "open");
        }
      }
      *handled = true;
      return 0;
    }
    return PhiPopover::OnMessage(message, wparam, lparam, handled);
  }

  void OnNcDestroy() override {
    poll_.Stop();
    PhiPopover::RecordClose(owner());
    if (instance == this) {
      instance = nullptr;
    }
  }

 private:
  HFONT NameFont() { return fonts().Get(13, FW_NORMAL); }
  HFONT NameStrikeFont() {
    PhiFontDesc desc;
    desc.size = 13;
    desc.strikeout = true;
    return fonts().Get(desc);
  }
  HFONT SmallFont() { return fonts().Get(11, FW_NORMAL); }
  HFONT SmallBoldFont() { return fonts().Get(11, FW_BOLD); }
  HFONT GlyphFont() {
    PhiFontDesc desc;
    desc.size = 12;
    desc.face = PhiIconFaceName();
    return fonts().Get(desc);
  }
  HFONT EmptyFont() {
    PhiFontDesc desc;
    desc.size = 32;
    desc.face = PhiIconFaceName();
    return fonts().Get(desc);
  }

  // THE layout pass: every visible row, its action buttons and the footer
  // link, into the base's hit list. Re-run whenever the rows or the scroll
  // offset move — never from Paint().
  void BuildHits() {
    ClearHits();
    const RECT list = ListRect();
    for (size_t i = 0; i < rows_.size(); ++i) {
      RECT row = RowRect(static_cast<int>(i));
      RECT visible = row;
      visible.top = std::max(row.top, list.top);
      visible.bottom = std::min(row.bottom, list.bottom);
      if (visible.bottom <= visible.top) {
        continue;
      }
      AddHit(visible, static_cast<int>(i) * kKeyStride, rows_[i].name,
             PhiHitRole::kListItem);
      const std::vector<PhiDownloadAction> actions =
          PhiDownloadActionsFor(rows_[i]);
      for (size_t a = 0; a < actions.size(); ++a) {
        RECT button = ActionRect(row, static_cast<int>(a),
                                 static_cast<int>(actions.size()));
        button.top = std::max(button.top, list.top);
        button.bottom = std::min(button.bottom, list.bottom);
        if (button.bottom <= button.top) {
          continue;
        }
        AddHit(button,
               static_cast<int>(i) * kKeyStride + 1 + static_cast<int>(a),
               PhiStr(actions[a].name_key), PhiHitRole::kButton, false,
               actions[a].enabled);
      }
    }
    AddHit(AllDownloadsRect(), kAllDownloadsKey,
           PhiStr("downloads.list.openAllDownloadsButton"),
           PhiHitRole::kButton);
  }

  void Refresh() {
    const char* json =
        framework_ ? framework_->ListDownloadsJson(browser_id_) : "[]";
    if (json == last_json_) {
      return;
    }
    last_json_ = json;
    rows_ = PhiParseDownloadsJson(last_json_);
    const int list_h = Scaled(kPanelHeight - kBottomBar);
    max_scroll_ = std::max(
        0, static_cast<int>(rows_.size()) * Scaled(kRowHeight) - list_h);
    scroll_ = std::min(scroll_, max_scroll_);
    BuildHits();
    Invalidate();
  }

  RECT ListRect() const {
    RECT rc;
    ::GetClientRect(hwnd(), &rc);
    rc.bottom -= Scaled(kBottomBar);
    return rc;
  }

  RECT RowRect(int index) const {
    const RECT list = ListRect();
    const int top = list.top + index * Scaled(kRowHeight) - scroll_;
    return {list.left + Scaled(kListPad), top,
            list.right - Scaled(kListPad), top + Scaled(kRowHeight)};
  }

  RECT ActionRect(const RECT& row, int ordinal, int count) const {
    const int btn = Scaled(kActionButton);
    const int gap = Scaled(4);
    const int mid = (row.top + row.bottom) / 2;
    const int right = row.right - (count - 1 - ordinal) * (btn + gap);
    return {right - btn, mid - btn / 2, right, mid + btn / 2};
  }

  RECT AllDownloadsRect() {
    RECT rc;
    ::GetClientRect(hwnd(), &rc);
    RECT measure = {0, 0, 0, 0};
    HDC dc = ::GetDC(hwnd());
    HGDIOBJ old = ::SelectObject(dc, SmallFont());
    PhiDrawTextAA(dc, PhiStr("downloads.list.openAllDownloadsButton"), -1,
                  &measure, DT_CALCRECT | DT_SINGLELINE);
    ::SelectObject(dc, old);
    ::ReleaseDC(hwnd(), dc);
    const int w = measure.right + Scaled(8 + 4 + 10 + 8);
    const int h = measure.bottom + Scaled(8);
    const int mid = rc.bottom - Scaled(kBottomBar) / 2;
    return {rc.right - Scaled(12) - w, mid - h / 2, rc.right - Scaled(12),
            mid + h / 2};
  }

  void PaintPanel(HDC dc, const RECT& client) {
    const PhiPalette& p = palette();
    FillRect(dc, client, p.window_bg);
    ::SetBkMode(dc, TRANSPARENT);
    const COLORREF dim = PhiBlend(p.label, p.window_bg, 128);

    if (rows_.empty()) {
      RECT list = ListRect();
      HGDIOBJ old = ::SelectObject(dc, EmptyFont());
      ::SetTextColor(dc, p.label_dim);
      RECT glyph = list;
      glyph.bottom = (list.top + list.bottom) / 2 + Scaled(6);
      PhiDrawTextAA(dc, L"", -1, &glyph,
                  DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
      ::SelectObject(dc, SmallFont());
      RECT text = list;
      text.top = glyph.bottom + Scaled(8);
      PhiDrawTextAA(dc, PhiStr("downloads.list.emptyMessage"), -1, &text,
                  DT_CENTER | DT_TOP | DT_SINGLELINE);
      ::SelectObject(dc, old);
    }

    const RECT list = ListRect();
    for (size_t i = 0; i < rows_.size(); ++i) {
      const RECT row = RowRect(static_cast<int>(i));
      if (row.bottom <= list.top || row.top >= list.bottom) {
        continue;
      }
      const PhiDownloadRow& item = rows_[i];
      const bool failed = PhiDownloadFailed(item);

      // File-type icon, from the shell like NSWorkspace's.
      const int icon = Scaled(kIconSize);
      const int mid = (row.top + row.bottom) / 2;
      const RECT icon_rc = {row.left, mid - icon / 2, row.left + icon,
                            mid - icon / 2 + icon};
      icons_.Draw(dc, icon_rc, item.path, icon);

      const std::vector<PhiDownloadAction> actions =
          PhiDownloadActionsFor(item);
      const int actions_w = static_cast<int>(actions.size()) *
                                Scaled(kActionButton) +
                            (static_cast<int>(actions.size()) - 1) * Scaled(4);
      RECT info = {row.left + icon + Scaled(kIconGap), row.top,
                   row.right - actions_w - Scaled(8), row.bottom};

      // Name line (13pt, struck through for failed/cancelled/deleted), with
      // the Mac's "· Canceled/Failed/Deleted" bold suffix.
      HGDIOBJ old = ::SelectObject(
          dc, failed ? NameStrikeFont() : NameFont());
      ::SetTextColor(dc, p.label);
      RECT name_rc = {info.left, mid - Scaled(18), info.right,
                      mid - Scaled(1)};
      const char* status_key = PhiDownloadStatusKey(item);
      const wchar_t* status = status_key ? PhiStr(status_key) : nullptr;
      RECT name_measure = name_rc;
      PhiDrawTextAA(dc, item.name.c_str(), -1, &name_measure,
                  DT_CALCRECT | DT_SINGLELINE);
      RECT name_draw = name_rc;
      if (status) {
        RECT tag_measure = {0, 0, 0, 0};
        ::SelectObject(dc, SmallBoldFont());
        std::wstring tag = std::wstring(L"· ") + status;
        PhiDrawTextAA(dc, tag.c_str(), -1, &tag_measure,
                    DT_CALCRECT | DT_SINGLELINE);
        name_draw.right =
            std::min(static_cast<int>(name_rc.right - tag_measure.right) -
                         Scaled(4),
                     static_cast<int>(name_rc.left + name_measure.right));
        ::SelectObject(dc, failed ? NameStrikeFont() : NameFont());
        PhiDrawTextAA(dc, item.name.c_str(), -1, &name_draw,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
        RECT tag_rc = {std::min(name_draw.right,
                                name_draw.left + name_measure.right) +
                           Scaled(4),
                       name_rc.top, name_rc.right, name_rc.bottom};
        ::SelectObject(dc, SmallBoldFont());
        ::SetTextColor(dc, dim);
        PhiDrawTextAA(dc, tag.c_str(), -1, &tag_rc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      } else {
        PhiDrawTextAA(dc, item.name.c_str(), -1, &name_draw,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
      }

      // Second line: progress (bar, or figures while hovered) for live
      // downloads, relative date or "From host" otherwise.
      RECT sub = {info.left, mid + Scaled(2), info.right, mid + Scaled(17)};
      ::SelectObject(dc, SmallFont());
      ::SetTextColor(dc, dim);
      if (item.state == "in_progress") {
        if (hover_row_ == static_cast<int>(i)) {
          std::wstring text =
              PhiStrF("downloads.item.transfer.progressLabel",
                      {PhiFormatDownloadBytes(item.received),
                       PhiFormatDownloadBytes(item.total)});
          if (item.paused) {
            text += L" ";
            text += PhiStr("downloads.item.status.pausedIndicator");
          } else if (item.speed > 0) {
            text += L" ・ " + PhiFormatDownloadBytes(item.speed) + L"/s";
          }
          PhiDrawTextAA(dc, text.c_str(), -1, &sub,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
          const int bar_mid = (sub.top + sub.bottom) / 2;
          RECT track = {sub.left, bar_mid - Scaled(1), sub.right,
                        bar_mid + Scaled(2)};
          FillRounded(dc, track, PhiBlend(p.accent, p.window_bg, 51),
                      Scaled(3));
          if (item.total > 0) {
            // Clamped: a server that under-reports its length sends more
            // bytes than the total, and the fill ran out of the track.
            const double done =
                std::clamp(item.received / item.total, 0.0, 1.0);
            RECT fill = track;
            fill.right = fill.left + static_cast<int>(
                (track.right - track.left) * done);
            if (fill.right > fill.left) {
              FillRounded(dc, fill, p.accent, Scaled(3));
            }
          }
        }
      } else {
        const std::wstring text =
            item.state == "complete" && item.end_time > 0
                ? PhiFormatDownloadRelative(item.end_time)
                : PhiStrF("downloads.item.sourceHostLabel", {item.host});
        PhiDrawTextAA(dc, text.c_str(), -1, &sub,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      }

      // Action buttons.
      ::SelectObject(dc, GlyphFont());
      for (size_t a = 0; a < actions.size(); ++a) {
        const RECT button = ActionRect(row, static_cast<int>(a),
                                       static_cast<int>(actions.size()));
        if (hover_row_ == static_cast<int>(i) &&
            hover_action_ == static_cast<int>(a) && actions[a].enabled) {
          FillRounded(dc, button, p.hover_strong, Scaled(6));
        }
        ::SetTextColor(dc, actions[a].enabled ? p.label : p.label_tertiary);
        wchar_t glyph[2] = {actions[a].glyph, 0};
        PhiDrawTextAA(dc, glyph, 1, const_cast<RECT*>(&button),
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      }
      ::SelectObject(dc, old);

      // Separator (not after the last row).
      if (i + 1 < rows_.size()) {
        RECT line = {row.left, row.bottom - 1, row.right, row.bottom};
        FillRect(dc, line, PhiBlend(p.label, p.window_bg, 26));
      }
    }

    // Bottom bar: divider + trailing "All Downloads ›".
    RECT bar = client;
    bar.top = client.bottom - Scaled(kBottomBar);
    FillRect(dc, bar, p.window_bg);
    RECT divider = {bar.left, bar.top, bar.right, bar.top + 1};
    FillRect(dc, divider, PhiBlend(p.label, p.window_bg, 26));
    const RECT button = AllDownloadsRect();
    if (hover_all_) {
      FillRounded(dc, button, PhiBlend(p.accent, p.window_bg, 15),
                  Scaled(6));
    }
    HGDIOBJ old = ::SelectObject(dc, SmallFont());
    ::SetTextColor(dc, p.accent);
    RECT text = {button.left + Scaled(8), button.top,
                 button.right - Scaled(18), button.bottom};
    PhiDrawTextAA(dc, PhiStr("downloads.list.openAllDownloadsButton"), -1,
                  &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ::SelectObject(dc, GlyphFont());
    RECT chevron = {text.right, button.top, button.right, button.bottom};
    PhiDrawTextAA(dc, L"", 1, &chevron,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    ::SelectObject(dc, old);

    // Hairline border over everything (the panel edge) — Windows 10 only:
    // on 11 DWM rounds the panel and draws its border along the curve, and
    // a square rim inside it would cut across all four corners.
    if (dwm_rounded()) {
      return;
    }
    HPEN pen = ::CreatePen(PS_SOLID, 1, p.border);
    HGDIOBJ old_pen = ::SelectObject(dc, pen);
    HGDIOBJ old_brush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(dc, client.left, client.top, client.right, client.bottom);
    ::SelectObject(dc, old_brush);
    ::SelectObject(dc, old_pen);
    ::DeleteObject(pen);
  }

  void RunHoveredAction() {
    if (hover_all_) {
      // The Mac's "All Downloads" opens the Library on its Downloads
      // section over the owning shell. Opening it refocuses and destroys
      // this panel: resolve the shell first, dismiss, then open.
      PhiShellWindow* shell = PhiShellWindow::FromHwnd(owner());
      Dismiss();
      if (shell) {
        PhiLibrary::Open(shell, PhiLibrarySection::kDownloads);
      }
      return;
    }
    if (hover_row_ < 0 || hover_action_ < 0 ||
        hover_row_ >= static_cast<int>(rows_.size())) {
      return;
    }
    const PhiDownloadRow& row = rows_[static_cast<size_t>(hover_row_)];
    const std::vector<PhiDownloadAction> actions = PhiDownloadActionsFor(row);
    if (hover_action_ >= static_cast<int>(actions.size()) ||
        !actions[static_cast<size_t>(hover_action_)].enabled) {
      return;
    }
    const PhiDownloadAction& action =
        actions[static_cast<size_t>(hover_action_)];
    if (std::string_view(action.verb) == "copy") {
      PhiCopyTextToClipboard(hwnd(), row.url);
      return;
    }
    if (framework_) {
      framework_->DownloadAction(browser_id_, static_cast<uint32_t>(row.id),
                                 action.verb);
      last_json_.clear();  // Force the next poll to repaint.
      Refresh();
    }
  }

  // Not rewritten to raw_ptr: the popover lives strictly inside the
  // framework's lifetime (owned popup of the shell window).
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_;
  // The browser the popover was opened for (BrowserId::value; the
  // framework calls below are still int64_t until phase 2).
  int64_t browser_id_;
  Placement placement_;
  Timer poll_;
  std::vector<PhiDownloadRow> rows_;
  std::string last_json_;
  PhiDownloadIconCache icons_;
  int scroll_ = 0;
  int max_scroll_ = 0;
  int hover_row_ = -1;
  int hover_action_ = -1;
  bool hover_all_ = false;
};

Popover* Popover::instance = nullptr;

}  // namespace

void PhiDownloadsPopover::Toggle(HWND owner,
                                 const RECT& anchor_screen,
                                 const PhiPalette& palette,
                                 phi_framework::PhiFramework* framework,
                                 BrowserId browser) {
  if (Popover::instance) {
    Popover::instance->Dismiss();
    return;
  }
  // The click that dismissed the popover (deactivating it) must not
  // immediately reopen it — the flyout toggle race, now the family's.
  if (PhiPopover::ShouldSuppressReopen(owner)) {
    return;
  }
  Popover* popover = new Popover(framework, browser);
  popover->DeleteWhenDestroyed();
  if (!popover->Open(owner, anchor_screen, palette)) {
    delete popover;
  }
}

}  // namespace phi_client

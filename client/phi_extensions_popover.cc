// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_extensions_popover.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "client/phi_dwrite.h"
#include "client/phi_extension_action.h"
#include "client/phi_i18n.h"
#include "client/phi_popover.h"

#pragma comment(lib, "dwmapi.lib")

namespace phi_client {

namespace {

// The Mac ExtensionList's geometry (ExtensionList.swift), in design units:
// a 240 pt panel, 12 pt side padding, an "Extensions" title, a 4-column
// grid of 50x32 tiles on a 6 pt gutter (four rows visible, the rest
// scroll), then a "Settings" title and the "Manage Extensions" row.
constexpr int kPanelWidth = 240;
constexpr int kSidePadding = 12;
constexpr int kTopPadding = 12;
constexpr int kBottomPadding = 12;
constexpr int kSectionSpacing = 8;   // The VStack's spacing.
constexpr int kTitleHeight = 20;     // A 15 pt medium title's line.
constexpr int kColumns = 4;
constexpr int kTileWidth = 50;
constexpr int kTileHeight = 32;
constexpr int kTileSpacing = 6;
constexpr int kTileRadius = 8;
constexpr int kMaxVisibleRows = 4;
constexpr int kTileIcon = 18;
constexpr int kPinDot = 16;          // The black pin disc on a tile corner.
constexpr int kPinOffset = 4;        // Its overhang past the tile's corner.
constexpr int kEmptyHeight = 100;    // "No extensions found".
constexpr int kSettingSpacing = 4;   // Title → row inside the section.
constexpr int kManageRowHeight = 22;
constexpr int kManageRowInset = 8;   // The row's horizontal padding.
constexpr int kManageTextInset = 4;  // Text/chevron inside the row.
constexpr int kManageRadius = 6;
// The scroll cue beside a grid that scrolls: a 3 pt capsule in the side
// padding, 3 pt in from the panel's edge.
constexpr int kScrollThumbWidth = 3;
constexpr int kScrollThumbInset = 3;
constexpr int kScrollThumbMin = 16;
constexpr UINT kPollIntervalMs = 700;
// hits_ keys: tile i is i * kKeyStride, its pin toggle is + 1; the Manage
// Extensions row is kManageKey. The base resolves hover and clicks over
// the list.
constexpr int kKeyStride = 2;
constexpr int kManageKey = 1 << 20;

// `t` of `over` on `under`, for the Mac's translucent overlays (a 60 %
// secondary chevron, a 60 % pin disc) on a flat GDI ground.
COLORREF Mix(COLORREF over, COLORREF under, float t) {
  const auto channel = [t](int o, int u) {
    return static_cast<int>(o * t + u * (1.0f - t) + 0.5f);
  };
  return RGB(channel(GetRValue(over), GetRValue(under)),
             channel(GetGValue(over), GetGValue(under)),
             channel(GetBValue(over), GetBValue(under)));
}

void FillRect(HDC dc, const RECT& r, COLORREF color) {
  HBRUSH brush = ::CreateSolidBrush(color);
  ::FillRect(dc, &r, brush);
  ::DeleteObject(brush);
}

void FillRounded(HDC dc, const RECT& r, COLORREF color, int radius) {
  PhiFillRoundRectAA(dc, r, radius, color);
}

// Box-averages a premultiplied BGRA buffer down to `out_px` square — the
// manifest icons arrive at 48 or 128 and the tile's icon is 18.
std::vector<unsigned char> ScaleBgra(const unsigned char* bgra,
                                     int width,
                                     int height,
                                     int out_px) {
  std::vector<unsigned char> out(
      static_cast<size_t>(out_px) * static_cast<size_t>(out_px) * 4, 0);
  if (!bgra || width <= 0 || height <= 0 || out_px <= 0) {
    return out;
  }
  const auto source = UNSAFE_BUFFERS(base::span<const unsigned char>(
      bgra, static_cast<size_t>(width) * static_cast<size_t>(height) * 4));
  for (int y = 0; y < out_px; ++y) {
    const int sy0 = y * height / out_px;
    const int sy1 = std::max(sy0 + 1, (y + 1) * height / out_px);
    for (int x = 0; x < out_px; ++x) {
      const int sx0 = x * width / out_px;
      const int sx1 = std::max(sx0 + 1, (x + 1) * width / out_px);
      std::array<unsigned int, 4> sum = {0, 0, 0, 0};
      for (int sy = sy0; sy < sy1; ++sy) {
        for (int sx = sx0; sx < sx1; ++sx) {
          const size_t at =
              (static_cast<size_t>(sy) * static_cast<size_t>(width) +
               static_cast<size_t>(sx)) *
              4;
          for (int c = 0; c < 4; ++c) {
            sum[c] += source[at + static_cast<size_t>(c)];
          }
        }
      }
      const unsigned int n =
          static_cast<unsigned int>((sy1 - sy0) * (sx1 - sx0));
      const size_t to =
          (static_cast<size_t>(y) * static_cast<size_t>(out_px) +
           static_cast<size_t>(x)) *
          4;
      for (int c = 0; c < 4; ++c) {
        out[to + static_cast<size_t>(c)] =
            static_cast<unsigned char>(sum[c] / n);
      }
    }
  }
  return out;
}

struct ExtensionTile {
  std::string id;
  std::wstring name;
  bool pinned = false;
  // The action's state on this window's active tab: badge and grayscale on
  // the tile's icon, enabled for the click (the Mac's ExtensionGridItem).
  PhiExtensionActionState state;
  RECT bounds{};   // The tile; empty while scrolled out of view.
  RECT pin_box{};  // The pin disc straddling its bottom-right corner.
};

// The popover keeps its tiles, its Paint() and its pin/action handling. The
// window, the placement, the back buffer, hover, the poll timer, dismissal
// and lifetime belong to PhiPopover (lane 3 §6.2).
class Popover : public PhiPopover {
 public:
  static Popover* instance;

  Popover(phi_framework::PhiFramework* framework, BrowserId browser)
      : framework_(framework), browser_id_(browser.value) {}

  ~Popover() override = default;

  bool Open(HWND owner, const RECT& anchor, const PhiPalette& colors) {
    ClassSpec spec;
    spec.name = L"PhiExtensionsPopover";

    Options options;  // Activating, DWM-rounded, 500 ms opening grace.

    // Below the anchor when it sits in the owner's top half (the address
    // capsule's in-field button — the Mac popover drops downward there);
    // above it for the bottom bar. Clamped to the work area either way.
    RECT owner_rect = {};
    ::GetWindowRect(owner, &owner_rect);
    const bool anchored_high =
        anchor.top < (owner_rect.top + owner_rect.bottom) / 2;
    placement_.anchor = anchor;
    placement_.side = anchored_high ? PhiPopoverSide::kBelow
                                    : PhiPopoverSide::kAbove;
    placement_.gap_dip = 8;
    placement_.monitor_from_owner = true;

    SIZE probe = {1, 1};
    if (!CreatePopover(spec, owner, probe, placement_, options)) {
      return false;
    }
    instance = this;  // Before Show(): activation churn can destroy us.
    SetPalette(colors);
    Refresh();
    Reposition(placement_, {Scaled(kPanelWidth), PanelHeight()});
    BuildHits();  // Refresh laid out against the pre-placement size.
    Show();
    // The list loads asynchronously on first use; poll cheaply while open
    // so the panel fills even if the delegate event races the open.
    poll_ = NewTimer();
    poll_.Start(kPollIntervalMs);
    return true;
  }

  void Refresh() {
    const char* json =
        framework_ ? framework_->ListExtensionsJson(browser_id_) : "[]";
    if (json == last_json_ && !tiles_.empty()) {
      return;
    }
    last_json_ = json;
    tiles_.clear();
    std::optional<base::Value> parsed =
        base::JSONReader::Read(last_json_, base::JSON_PARSE_RFC);
    if (parsed && parsed->is_list()) {
      for (const base::Value& value : parsed->GetList()) {
        const base::DictValue* dict = value.GetIfDict();
        if (!dict) {
          continue;
        }
        ExtensionTile tile;
        if (const std::string* id = dict->FindString("id")) {
          tile.id = *id;
        }
        if (const std::string* name = dict->FindString("name")) {
          tile.name = base::UTF8ToWide(*name);
        }
        tile.pinned = dict->FindBool("pinned").value_or(false);
        tile.state = PhiReadExtensionActionState(*dict);
        tiles_.push_back(std::move(tile));
      }
    }
    scroll_row_ = std::min(scroll_row_, MaxScrollRow());
    if (alive()) {
      // The tile count changes the panel height: re-run placement so the
      // panel keeps hanging off its anchor (below the capsule's button, or
      // above the bottom bar) instead of growing past it.
      Reposition(placement_, {Scaled(kPanelWidth), PanelHeight()});
      BuildHits();
      Invalidate();
    }
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
    hover_tile_ = -1;
    hover_pin_ = false;
    hover_manage_ = false;
    if (hit < 0) {
      return;
    }
    const int key = hits_[static_cast<size_t>(hit)].key;
    if (key == kManageKey) {
      hover_manage_ = true;
      return;
    }
    hover_tile_ = key / kKeyStride;
    hover_pin_ = (key % kKeyStride) != 0;
  }

  void OnClick(int hit, POINT) override {
    if (context_menu_request_) {
      return;
    }
    OnHover(hit, -1);
    RunHoveredAction();
  }

  void OnWheel(int delta, POINT) override {
    if (context_menu_request_) {
      return;
    }
    // The Mac scrolls the grid past four rows; a notch is a row here.
    const int next = std::clamp(scroll_row_ - delta / WHEEL_DELTA, 0,
                                MaxScrollRow());
    if (next == scroll_row_) {
      return;
    }
    scroll_row_ = next;
    BuildHits();
    Invalidate();
  }

  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) override {
    if (message == WM_ACTIVATEAPP && !wparam && context_menu_request_) {
      *handled = true;
      Dismiss();
      return 0;
    }
    if (message == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE &&
        context_menu_request_) {
      // Views owns capture/focus while its asynchronous menu is up. The
      // completion callback resumes normal dismissal. Destruction still
      // cancels the request, and another application's activation dismisses.
      const HWND next = reinterpret_cast<HWND>(lparam);
      if (next && ::GetAncestor(next, GA_ROOTOWNER) ==
                      ::GetAncestor(owner(), GA_ROOTOWNER)) {
        *handled = true;
        return 0;
      }
    }
    if (message == WM_RBUTTONUP) {
      // A tile's secondary click: the action's context menu at the pointer
      // (the Mac ExtensionGridItem's onSecondaryTap), above this panel.
      const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const int hit = HitTest(pt);
      if (hit >= 0 && framework_) {
        const int key = hits_[static_cast<size_t>(hit)].key;
        const size_t tile = static_cast<size_t>(key / kKeyStride);
        if (key != kManageKey && tile < tiles_.size()) {
          POINT screen = pt;
          ::ClientToScreen(hwnd(), &screen);
          const std::string id = tiles_[tile].id;
          ShowContextMenu(id, screen);
        }
      }
      *handled = true;
      return 0;
    }
    return PhiPopover::OnMessage(message, wparam, lparam, handled);
  }

  void OnDpiChanged(UINT) override {
    Reposition(placement_, {Scaled(kPanelWidth), PanelHeight()});
    BuildHits();
  }

  void OnNcDestroy() override {
    poll_.Stop();
    const uint64_t request = std::exchange(context_menu_request_, 0);
    if (request && framework_) {
      framework_->CancelExtensionContextMenu(browser_id_, request);
    }
    PhiPopover::RecordClose(owner());
    if (instance == this) {
      instance = nullptr;
    }
  }

 private:
  void ShowContextMenu(const std::string& extension_id, POINT screen) {
    if (context_menu_request_) {
      return;
    }
    // Unique across panel instances: a completion for a destroyed panel must
    // never refresh or activate its replacement, even if its HWND was reused.
    static uint64_t next_request = 0;
    context_menu_request_ = ++next_request;
    framework_->ShowExtensionContextMenu(
        browser_id_, extension_id.c_str(), screen.x, screen.y, hwnd(),
        context_menu_request_, &ContextMenuClosed);
    // Showing can dispatch native messages and destroy us. No member access.
  }

  static void ContextMenuClosed(uint64_t request) {
    Popover* panel = instance;
    if (!panel || panel->context_menu_request_ != request) {
      return;
    }
    panel->context_menu_request_ = 0;
    const HWND foreground = ::GetForegroundWindow();
    // A click outside, a newly opened dialog/page, or another application
    // ends the panel too. Normal menu dismissal leaves its native owner
    // active, so pin/unpin and Escape retain the panel without stealing focus.
    if (foreground != panel->hwnd()) {
      panel->Dismiss();
      return;
    }
    panel->Refresh();
    // Do not reclaim focus: menu commands may have opened a dialog or page.
  }

  // The Mac's 15 pt .medium section titles. Named face: asking "Segoe UI"
  // for a weight past regular rounds up to Bold; Semibold is the nearest
  // face Windows ships.
  HFONT TitleFont() {
    PhiFontDesc desc;
    desc.size = 15;
    desc.weight = FW_SEMIBOLD;
    desc.face = L"Segoe UI Semibold";
    return fonts().Get(desc);
  }
  HFONT RowFont() { return fonts().Get(13, FW_NORMAL); }
  HFONT BadgeFont() {
    PhiFontDesc desc;
    desc.size = 8;
    desc.weight = FW_SEMIBOLD;
    desc.face = L"Segoe UI Semibold";
    return fonts().Get(desc);
  }
  HFONT GlyphFont(int size) {
    PhiFontDesc desc;
    desc.size = size;
    desc.face = PhiIconFaceName();
    return fonts().Get(desc);
  }

  int TotalRows() const {
    return (static_cast<int>(tiles_.size()) + kColumns - 1) / kColumns;
  }
  int VisibleRows() const {
    return std::min(kMaxVisibleRows, TotalRows());
  }
  int MaxScrollRow() const {
    return std::max(0, TotalRows() - kMaxVisibleRows);
  }
  // The grid band's height: the Mac's gridHeight, or the empty label's.
  int GridHeight() const {
    if (tiles_.empty()) {
      return Scaled(kEmptyHeight);
    }
    const int rows = VisibleRows();
    return rows * Scaled(kTileHeight) + (rows - 1) * Scaled(kTileSpacing);
  }
  int GridTop() const {
    return Scaled(kTopPadding) + Scaled(kTitleHeight) +
           Scaled(kSectionSpacing);
  }
  int SettingTop() const {
    return GridTop() + GridHeight() + Scaled(kSectionSpacing);
  }
  RECT ManageRow() const {
    const int top =
        SettingTop() + Scaled(kTitleHeight) + Scaled(kSettingSpacing);
    return {Scaled(kManageRowInset), top,
            Scaled(kPanelWidth) - Scaled(kManageRowInset),
            top + Scaled(kManageRowHeight)};
  }
  int PanelHeight() const {
    return ManageRow().bottom + Scaled(kBottomPadding);
  }

  // THE layout pass: one hit per visible tile plus one for its pin disc
  // (added second, so it takes the pointer), then the Manage row.
  void BuildHits() {
    ClearHits();
    const int stride_x = Scaled(kTileWidth) + Scaled(kTileSpacing);
    const int stride_y = Scaled(kTileHeight) + Scaled(kTileSpacing);
    const int first = scroll_row_ * kColumns;
    const int last = std::min(static_cast<int>(tiles_.size()),
                              first + VisibleRows() * kColumns);
    for (int i = 0; i < static_cast<int>(tiles_.size()); ++i) {
      ExtensionTile& tile = tiles_[static_cast<size_t>(i)];
      if (i < first || i >= last) {
        tile.bounds = RECT{};
        tile.pin_box = RECT{};
        continue;
      }
      const int slot = i - first;
      const int left = Scaled(kSidePadding) + (slot % kColumns) * stride_x;
      const int top = GridTop() + (slot / kColumns) * stride_y;
      tile.bounds = {left, top, left + Scaled(kTileWidth),
                     top + Scaled(kTileHeight)};
      // The 16 pt disc's bottom-right sits 4 pt past the tile's (the Mac's
      // .bottomTrailing + .offset(4, 4)).
      const int dot = Scaled(kPinDot);
      const int overhang = Scaled(kPinOffset);
      tile.pin_box = {tile.bounds.right + overhang - dot,
                      tile.bounds.bottom + overhang - dot,
                      tile.bounds.right + overhang,
                      tile.bounds.bottom + overhang};
      AddHit(tile.bounds, i * kKeyStride, tile.name, PhiHitRole::kButton);
      AddHit(tile.pin_box, i * kKeyStride + 1,
             PhiStr(tile.pinned ? "extensions.popover.unpinButtonTooltip"
                                : "extensions.popover.pinButtonTooltip"),
             PhiHitRole::kToggle, tile.pinned);
    }
    AddHit(ManageRow(), kManageKey,
           PhiStr("extensions.list.manageExtensionsButton"),
           PhiHitRole::kButton);
  }

  void PaintPanel(HDC dc, const RECT& client) {
    const PhiPalette& p = palette();
    const COLORREF bg = p.ground_bottom;
    const COLORREF label = p.label;
    const COLORREF dim = p.label_dim;
    FillRect(dc, client, bg);
    ::SetBkMode(dc, TRANSPARENT);

    // "Extensions".
    RECT title = {Scaled(kSidePadding), Scaled(kTopPadding),
                  client.right - Scaled(kSidePadding),
                  Scaled(kTopPadding) + Scaled(kTitleHeight)};
    ::SelectObject(dc, TitleFont());
    ::SetTextColor(dc, label);
    PhiDrawTextAA(dc,
                  PhiStr("extensions.popover.browserExtensionsSectionTitle"),
                  -1, &title,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (tiles_.empty()) {
      RECT empty = {0, GridTop(), client.right, GridTop() + GridHeight()};
      ::SelectObject(dc, RowFont());
      ::SetTextColor(dc, dim);
      PhiDrawTextAA(dc, PhiStr("extensions.popover.emptyMessage"), -1, &empty,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
      for (size_t i = 0; i < tiles_.size(); ++i) {
        if (tiles_[i].bounds.right > tiles_[i].bounds.left) {
          PaintTile(dc, tiles_[i], static_cast<int>(i) == hover_tile_);
        }
      }
      PaintScrollCue(dc, client, Mix(dim, bg, 0.5f));
    }

    // "Settings" and the Manage Extensions row (ManageExtensionsButton).
    RECT setting = {Scaled(kSidePadding), SettingTop(),
                    client.right - Scaled(kSidePadding),
                    SettingTop() + Scaled(kTitleHeight)};
    ::SelectObject(dc, TitleFont());
    ::SetTextColor(dc, label);
    PhiDrawTextAA(dc, PhiStr("extensions.popover.settingsHeading"), -1,
                  &setting,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    const RECT row = ManageRow();
    if (hover_manage_) {
      FillRounded(dc, row, p.hover, Scaled(kManageRadius));
    }
    RECT text = {row.left + Scaled(kManageTextInset), row.top,
                 row.right - Scaled(kManageTextInset) - Scaled(16),
                 row.bottom};
    ::SelectObject(dc, RowFont());
    ::SetTextColor(dc, dim);
    PhiDrawTextAA(dc, PhiStr("extensions.list.manageExtensionsButton"), -1,
                  &text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    // chevron.right, 12 pt medium, secondary at 60 %.
    RECT chevron = {row.right - Scaled(kManageTextInset) - Scaled(16),
                    row.top, row.right - Scaled(kManageTextInset),
                    row.bottom};
    ::SelectObject(dc, GlyphFont(12));
    ::SetTextColor(dc, Mix(dim, hover_manage_ ? p.hover : bg, 0.6f));
    PhiDrawTextAA(dc, L"", -1, &chevron,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

  // Past sixteen extensions the grid scrolls, and nothing said so: a thumb
  // in the side padding, as long as the share of rows in view and as far
  // down as the scroll, tells the reader there is more under the wheel.
  void PaintScrollCue(HDC dc, const RECT& client, COLORREF ink) {
    const int max_row = MaxScrollRow();
    if (max_row <= 0) {
      return;
    }
    const int track_top = GridTop();
    const int track_h = GridHeight();
    const int thumb_h =
        std::max(Scaled(kScrollThumbMin),
                 track_h * VisibleRows() / std::max(1, TotalRows()));
    const int top =
        track_top + (track_h - thumb_h) * scroll_row_ / max_row;
    const int right = client.right - Scaled(kScrollThumbInset);
    const RECT thumb = {right - Scaled(kScrollThumbWidth), top, right,
                        top + thumb_h};
    PhiFillCapsuleAA(dc, thumb, ink);
  }

  // One 50x32 tile (ExtensionGridItem): the radius-8 wash (emphasized on
  // hover), the 18 pt icon with its badge, and the pin disc on the corner —
  // solid when pinned, 60 % while hovered, absent otherwise.
  void PaintTile(HDC dc, const ExtensionTile& tile, bool hot) {
    const PhiPalette& p = palette();
    const COLORREF fill = hot ? p.hover_strong : p.hover;
    FillRounded(dc, tile.bounds, fill, Scaled(kTileRadius));

    const int icon_px = Scaled(kTileIcon);
    const int mid_x = (tile.bounds.left + tile.bounds.right) / 2;
    const int mid_y = (tile.bounds.top + tile.bounds.bottom) / 2;
    RECT icon = {mid_x - icon_px / 2, mid_y - icon_px / 2,
                 mid_x - icon_px / 2 + icon_px, mid_y - icon_px / 2 + icon_px};
    int iw = 0;
    int ih = 0;
    const unsigned char* bgra =
        framework_ ? framework_->ExtensionIconBgra(tile.id.c_str(), &iw, &ih)
                   : nullptr;
    if (bgra && iw > 0 && ih > 0) {
      std::vector<unsigned char> scaled = ScaleBgra(bgra, iw, ih, icon_px);
      if (tile.state.grayscale) {
        scaled = PhiGrayedActionIcon(scaled);  // The grayed action.
      }
      PhiBlendBgra(dc, icon, scaled.data(), icon_px, icon_px);
    } else {
      // Icon not decoded yet: the puzzle stands in (the Mac's
      // puzzlepiece.extension, secondary, half-faded when grayed).
      ::SelectObject(dc, GlyphFont(14));
      ::SetTextColor(dc, tile.state.grayscale
                             ? Mix(p.label_dim, fill, 0.5f)
                             : p.label_dim);
      PhiDrawTextAA(dc, L"", -1, &icon,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    PhiDrawExtensionBadge(dc, icon, tile.state, BadgeFont(), dpi());

    if (!tile.pinned && !hot) {
      return;
    }
    const COLORREF ink = RGB(0, 0, 0);
    const COLORREF disc = tile.pinned ? ink : Mix(ink, fill, 0.6f);
    FillRounded(dc, tile.pin_box, disc, Scaled(kPinDot) / 2);
    ::SelectObject(dc, GlyphFont(9));
    ::SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
    RECT glyph = tile.pin_box;
    PhiDrawTextAA(dc, L"", -1, &glyph,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

  void RunHoveredAction() {
    if (!framework_) {
      return;
    }
    if (hover_manage_) {
      // Opening a tab can activate another window and destroy this panel.
      // Capture the target and dismiss before handing control to Chromium.
      auto* framework = framework_;
      const phi_framework::WindowId browser{browser_id_};
      Dismiss();
      framework->OpenTab(browser, "chrome://extensions");
      return;
    }
    if (hover_tile_ < 0 || hover_tile_ >= static_cast<int>(tiles_.size())) {
      return;
    }
    const ExtensionTile tile = tiles_[static_cast<size_t>(hover_tile_)];
    if (hover_pin_) {
      framework_->SetExtensionPinned(browser_id_, tile.id.c_str(),
                                     !tile.pinned);
      // The change notification refreshes the list; flip optimistically so
      // the toggle reads instant.
      tiles_[static_cast<size_t>(hover_tile_)].pinned = !tile.pinned;
      BuildHits();
      Invalidate();
      return;
    }
    // Run the action anchored to the TILE (screen coords) and leave — the
    // extension's own popup takes the stage. A DISABLED action doesn't run;
    // fall back to its context menu like Chrome (ExecuteUserAction), the
    // Mac ExtensionList's triggerExtension.
    RECT anchor = tile.bounds;
    ::MapWindowPoints(hwnd(), nullptr, reinterpret_cast<POINT*>(&anchor), 2);
    if (!tile.state.enabled) {
      ShowContextMenu(tile.id, {anchor.left, anchor.bottom});
      return;
    }
    // Dismiss before handing off: showing the action can destroy this panel.
    auto* framework = framework_;
    const int64_t browser_id = browser_id_;
    Dismiss();
    framework->TriggerExtensionAction(browser_id, tile.id.c_str(), anchor.left,
                                      anchor.top, anchor.right - anchor.left,
                                      anchor.bottom - anchor.top);
  }

  // Not rewritten to raw_ptr: the popover lives strictly inside the
  // framework's lifetime (owned popup of the shell window).
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_;
  // The browser the popover was opened for (BrowserId::value; the
  // framework calls below are still int64_t until phase 2).
  int64_t browser_id_;
  Placement placement_;
  Timer poll_;
  std::vector<ExtensionTile> tiles_;
  std::string last_json_;
  int scroll_row_ = 0;  // The first visible grid row.
  int hover_tile_ = -1;
  bool hover_pin_ = false;
  bool hover_manage_ = false;
  uint64_t context_menu_request_ = 0;
};

Popover* Popover::instance = nullptr;

}  // namespace

void PhiExtensionsPopover::Toggle(HWND owner,
                                  const RECT& anchor_screen,
                                  const PhiPalette& palette,
                                  phi_framework::PhiFramework* framework,
                                  BrowserId browser) {
  if (Popover::instance) {
    Popover::instance->Dismiss();
    return;
  }
  if (PhiPopover::ShouldSuppressReopen(owner)) {
    return;
  }
  Popover* popover = new Popover(framework, browser);
  popover->DeleteWhenDestroyed();
  if (!popover->Open(owner, anchor_screen, palette)) {
    delete popover;
  }
}

void PhiExtensionsPopover::RefreshOpen() {
  if (Popover::instance) {
    Popover::instance->Refresh();
  }
}

}  // namespace phi_client

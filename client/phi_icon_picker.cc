// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_icon_picker.h"

#include <imm.h>
#include <windowsx.h>

#pragma comment(lib, "imm32.lib")

#include <algorithm>
#include <cwctype>
#include <utility>
#include <vector>

#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "client/phi_dwrite.h"
#include "client/phi_emoji.h"
#include "client/phi_fluent.h"
#include "client/phi_icons.h"
#include "client/phi_modal_scope.h"
#include "client/phi_popover.h"

namespace phi_client {

namespace {

// Solid catalog grid: 20 dip artwork, 10 dip gaps, 16 dip padding.
constexpr int kWidth = 262;
constexpr int kPad = 16;
constexpr int kSegmentHeight = 28;
constexpr int kSegmentWidth = 72;
constexpr int kSearchHeight = 32;
constexpr int kColumns = 8;
constexpr int kCell = 20;
constexpr int kCellRadius = 6;
constexpr int kRowGap = 10;
constexpr int kHeaderHeight = 22;
constexpr int kVisibleRows = 11;
constexpr int kCategoryCell = 24;
constexpr int kCategoryStripHeight = 36;
constexpr int kGap = 8;

// Hit keys below the cell range.
constexpr int kHitIconTab = -1;
constexpr int kHitEmojiTab = -2;
constexpr int kHitCategoryFirst = -100;  // -100 - group index.

// One grid cell: what is drawn, what is stored, what is searched.
using Cell = PhiIconChoice;

struct Section {
  std::wstring title;
  std::vector<Cell> cells;
};

std::wstring Lower(std::wstring text) {
  return PhiIconSearchFold(text);
}

class Picker : public PhiPopover {
 public:
  Picker(PhiPalette palette, std::string current, PhiSpaceIconChosen on_chosen)
      : palette_(std::move(palette)),
        current_(std::move(current)),
        on_chosen_(std::move(on_chosen)) {
    emoji_tab_ = current_.rfind("emoji:", 0) == 0;
  }
  ~Picker() override {
    for (HFONT font : {label_font_, header_font_, icon_font_}) {
      if (font) {
        ::DeleteObject(font);
      }
    }
  }

  bool Open(HWND owner, const RECT& anchor_screen) {
    ClassSpec spec;
    spec.name = L"PhiSpaceIconPicker";
    Options options;  // Activating, dismiss on inactive or Escape.
    Placement placement;
    placement.anchor = anchor_screen;
    placement.side = PhiPopoverSide::kBelow;
    placement.align = PhiPopoverAlign::kLeading;
    placement.gap_dip = 4;
    placement.margin_px = 8;
    SIZE probe = {1, 1};
    if (!CreatePopover(spec, owner, probe, placement, options)) {
      return false;
    }
    // The search box takes the catalogue's English names: no IME, or the
    // first letter opens a composition instead of filtering.
    ::ImmAssociateContext(hwnd(), nullptr);
    EnsureFonts();
    BuildSections();
    Layout();
    Reposition(placement, {Scaled(kWidth), Height()});
    Show();
    return true;
  }

  // Open() failed before a window existed, so OnNcDestroy will never give
  // the answer: give it now, or a caller waiting on it waits forever.
  void AnswerUnopened() {
    if (on_chosen_) {
      PhiSpaceIconChosen callback = std::move(on_chosen_);
      on_chosen_ = PhiSpaceIconChosen();
      callback(std::string());
    }
  }

 protected:
  PhiPalette QueryPalette() const override { return palette_; }

  void OnDpiChanged(UINT) override {
    EnsureFonts();
    Layout();
    ::SetWindowPos(hwnd(), nullptr, 0, 0, Scaled(kWidth), Height(),
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }

  LRESULT OnMessage(UINT message, WPARAM wparam, LPARAM lparam,
                    bool* handled) override {
    if (message == WM_CHAR) {
      const wchar_t c = static_cast<wchar_t>(wparam);
      if (c == VK_BACK) {
        if (!query_.empty()) {
          query_.pop_back();
        }
      } else if (c >= L' ') {
        query_.push_back(c);
      } else {
        *handled = true;
        return 0;  // Enter and Escape are OnKey's.
      }
      scroll_ = 0;
      focus_ = -1;
      Layout();
      *handled = true;
      return 0;
    }
    return PhiPopover::OnMessage(message, wparam, lparam, handled);
  }

  bool OnKey(WPARAM virtual_key) override {
    switch (virtual_key) {
      case VK_RETURN:
        if (focus_ >= 0 && focus_ < static_cast<int>(flat_.size())) {
          chosen_ = flat_[static_cast<size_t>(focus_)].storage;
          Dismiss();
        }
        return true;
      case VK_LEFT:
        MoveFocus(-1);
        return true;
      case VK_RIGHT:
        MoveFocus(1);
        return true;
      case VK_UP:
        MoveFocus(-kColumns);
        return true;
      case VK_DOWN:
        MoveFocus(kColumns);
        return true;
      case VK_TAB:
        emoji_tab_ = !emoji_tab_;
        scroll_ = 0;
        focus_ = -1;
        Layout();
        return true;
      default:
        return PhiPopover::OnKey(virtual_key);
    }
  }

  void OnWheel(int delta, POINT client_pt) override {
    if (client_pt.y < grid_.top) {
      return;
    }
    const int step = Scaled(kCell + kRowGap) * 3;
    scroll_ = std::max(0, std::min(scroll_ - delta * step / WHEEL_DELTA,
                                   max_scroll_));
    Layout();
  }

  void OnClick(int hit, POINT) override {
    if (hit < 0) {
      return;
    }
    const int key = hits_[static_cast<size_t>(hit)].key;
    if (key == kHitIconTab || key == kHitEmojiTab) {
      emoji_tab_ = key == kHitEmojiTab;
      scroll_ = 0;
      focus_ = -1;
      Layout();
      return;
    }
    if (key <= kHitCategoryFirst) {
      // The category strip: scroll the group's header to the top.
      const size_t group = static_cast<size_t>(kHitCategoryFirst - key);
      if (group < section_tops_.size()) {
        scroll_ = std::min(max_scroll_, section_tops_[group]);
        Layout();
      }
      return;
    }
    if (key >= 0 && key < static_cast<int>(flat_.size())) {
      chosen_ = flat_[static_cast<size_t>(key)].storage;
      Dismiss();
    }
  }

  void OnNcDestroy() override {
    if (on_chosen_) {
      PhiSpaceIconChosen callback = std::move(on_chosen_);
      on_chosen_ = PhiSpaceIconChosen();
      callback(chosen_);
    }
  }

  void Paint(HDC dc, const RECT&) override {
    RECT rc = {};
    ::GetClientRect(hwnd(), &rc);
    const PhiPalette& p = palette_;
    HBRUSH ground = ::CreateSolidBrush(p.flyout);
    ::FillRect(dc, &rc, ground);
    ::DeleteObject(ground);
    ::SetBkMode(dc, TRANSPARENT);
    const int hot = hovered();

    // The segmented control.
    for (int i = 0; i < 2; ++i) {
      const bool on = (i == 1) == emoji_tab_;
      const RECT seg = SegmentRect(i);
      const bool is_hot = hot >= 0 && hits_[static_cast<size_t>(hot)].key ==
                                          (i == 0 ? kHitIconTab : kHitEmojiTab);
      if (on) {
        PhiFillRoundRectAA(dc, seg, Scaled(4), p.accent_soft);
      } else if (is_hot) {
        PhiFillRoundRectAA(dc, seg, Scaled(4), p.hover);
      }
      ::SelectObject(dc, label_font_);
      ::SetTextColor(dc, on ? p.accent : p.label);
      RECT text = seg;
      ::DrawTextW(dc, i == 0 ? L"Icon" : L"Emoji", -1, &text,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    // The search box: control fill, magnifier, the query or placeholder.
    {
      PhiFillRoundRectAA(dc, search_, Scaled(4), p.control_fill);
      PhiStrokeRoundRectAA(dc, search_, Scaled(4), p.border, 1);
      RECT glass = {search_.left + Scaled(10), search_.top,
                    search_.left + Scaled(26), search_.bottom};
      PhiDrawIconGlyph(dc, glass, fluent::kSearch, Scaled(14), p.label_dim);
      ::SelectObject(dc, label_font_);
      RECT text = {glass.right + Scaled(6), search_.top,
                   search_.right - Scaled(10), search_.bottom};
      if (query_.empty()) {
        ::SetTextColor(dc, p.label_tertiary);
        ::DrawTextW(dc, L"Search", -1, &text,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      } else {
        ::SetTextColor(dc, p.label);
        ::DrawTextW(dc, query_.c_str(), -1, &text,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      }
      if (!query_.empty()) {
        RECT edge = {search_.left + Scaled(6), search_.bottom - Scaled(2),
                     search_.right - Scaled(6), search_.bottom};
        PhiFillRoundRectAA(dc, edge, 1, p.accent);
      }
    }
    // The emoji category strip: each group's first emoji, the current one
    // underlined in the accent.
    if (emoji_tab_ && query_.empty()) {
      for (size_t g = 0; g < category_rects_.size(); ++g) {
        const RECT cell = category_rects_[g];
        const bool current = g == current_group_;
        const bool is_hot =
            hot >= 0 && hits_[static_cast<size_t>(hot)].key ==
                            kHitCategoryFirst - static_cast<int>(g);
        if (is_hot) {
          PhiFillRoundRectAA(dc, cell, Scaled(kCellRadius), p.hover);
        }
        RECT art = cell;
        ::InflateRect(&art, -Scaled(5), -Scaled(5));
        if (!PhiDrawColorIcon(dc, art, category_glyphs_[g], Scaled(16),
                              p.label)) {
          ::SelectObject(dc, label_font_);
          ::SetTextColor(dc, p.label);
          ::DrawTextW(dc, category_glyphs_[g].c_str(), -1, &art,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (current) {
          RECT line = {cell.left + Scaled(6), cell.bottom - Scaled(2),
                       cell.right - Scaled(6), cell.bottom};
          PhiFillRoundRectAA(dc, line, 1, p.accent);
        }
      }
    }
    // The grid, clipped to its viewport.
    ::IntersectClipRect(dc, grid_.left, grid_.top, grid_.right, grid_.bottom);
    for (const Header& header : headers_) {
      ::SelectObject(dc, header_font_);
      ::SetTextColor(dc, p.label_dim);
      RECT text = header.rect;
      ::DrawTextW(dc, header.text.c_str(), -1, &text,
                  DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
    }
    for (size_t i = 0; i < hits_.size(); ++i) {
      const PhiHit& hit = hits_[i];
      if (hit.key < 0) {
        continue;
      }
      const RECT cell = PhiToRect(hit.bounds);
      const Cell& item = flat_[static_cast<size_t>(hit.key)];
      const bool selected = item.storage == current_;
      const bool focused = hit.key == focus_;
      const bool is_hot = static_cast<int>(i) == hot;
      if (selected) {
        PhiFillRoundRectAA(dc, cell, Scaled(kCellRadius), p.accent_soft);
        PhiStrokeRoundRectAA(dc, cell, Scaled(kCellRadius), p.accent, 1);
      } else if (is_hot || focused) {
        PhiFillRoundRectAA(dc, cell, Scaled(kCellRadius), p.hover);
        if (focused) {
          PhiStrokeRoundRectAA(dc, cell, Scaled(kCellRadius), p.accent, 1);
        }
      }
      RECT art = cell;
      const COLORREF ink = selected ? p.accent : p.label;
      if (PhiDrawCatalogArtwork(dc, art, item.text, Scaled(20), ink)) {
        continue;
      }
      if (!PhiIsPrivateGlyph(item.text) &&
          PhiDrawColorIcon(dc, art, item.text, Scaled(16), ink)) {
        continue;
      }
      ::SelectObject(dc, PhiIsPrivateGlyph(item.text) ? icon_font_
                                                       : label_font_);
      ::SetTextColor(dc, ink);
      RECT cell_rc = cell;
      ::DrawTextW(dc, item.text.c_str(), -1, &cell_rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    ::SelectClipRgn(dc, nullptr);
  }

 private:
  struct Header {
    std::wstring text;
    RECT rect;
  };

  void EnsureFonts() {
    for (HFONT font : {label_font_, header_font_, icon_font_}) {
      if (font) {
        ::DeleteObject(font);
      }
    }
    label_font_ = PhiFluentFont(dpi(), 13);
    header_font_ = PhiFluentFont(dpi(), 11, FW_SEMIBOLD);
    icon_font_ = PhiIconFont(dpi(), 14);
  }

  // Preserve the supplied catalog order: pictograms, digits, letters, marks.
  // Emoji retain their named categories.
  void BuildSections() {
    icon_sections_.clear();
    emoji_sections_.clear();
    for (const PhiIconChoiceGroup& group : PhiIconChoiceGroups(false)) {
      icon_sections_.push_back({group.title, group.items});
    }
    category_glyphs_.clear();
    for (const PhiIconChoiceGroup& group : PhiIconChoiceGroups(true)) {
      category_glyphs_.push_back(
          group.items.empty() ? std::wstring() : group.items.front().text);
      emoji_sections_.push_back({group.title, group.items});
    }
  }

  RECT SegmentRect(int index) const {
    const int left = Scaled(kPad) + index * (Scaled(kSegmentWidth) + Scaled(4));
    return {left, Scaled(kPad), left + Scaled(kSegmentWidth),
            Scaled(kPad) + Scaled(kSegmentHeight)};
  }

  int Height() const { return height_; }

  // THE layout pass: the chrome above the grid, then the visible cells
  // into the hit list (hover, click and the UIA grid all read it).
  void Layout() {
    ClearHits();
    headers_.clear();
    flat_.clear();
    section_tops_.clear();
    category_rects_.clear();
    const int width = Scaled(kWidth);
    int y = Scaled(kPad);
    AddHit(SegmentRect(0), kHitIconTab, L"Icon", PhiHitRole::kTab, !emoji_tab_);
    AddHit(SegmentRect(1), kHitEmojiTab, L"Emoji", PhiHitRole::kTab, emoji_tab_);
    y += Scaled(kSegmentHeight) + Scaled(kGap);
    search_ = {Scaled(kPad), y, width - Scaled(kPad), y + Scaled(kSearchHeight)};
    y += Scaled(kSearchHeight) + Scaled(kGap);
    const std::vector<Section>& sections =
        emoji_tab_ ? emoji_sections_ : icon_sections_;
    if (emoji_tab_ && query_.empty()) {
      // The category strip: one cell per group, spread across the width.
      const int count = static_cast<int>(sections.size());
      const int cell = Scaled(kCategoryCell);
      const int span = width - 2 * Scaled(kPad);
      const int pitch = count > 1 ? (span - cell) / (count - 1) : 0;
      for (int g = 0; g < count; ++g) {
        RECT rc = {Scaled(kPad) + g * pitch, y, Scaled(kPad) + g * pitch + cell,
                   y + cell};
        category_rects_.push_back(rc);
        AddHit(rc, kHitCategoryFirst - g, sections[static_cast<size_t>(g)].title,
               PhiHitRole::kTab);
      }
      y += Scaled(kCategoryStripHeight);
    }
    // The grid: its viewport is kVisibleRows of cells; content scrolls
    // under it by `scroll_` px.
    const int pitch = Scaled(kCell + kRowGap);
    const int span = width - 2 * Scaled(kPad);
    const int col_pitch = (span - Scaled(kCell)) / (kColumns - 1);
    grid_ = {Scaled(kPad), y, width - Scaled(kPad),
             y + kVisibleRows * pitch - Scaled(kRowGap)};
    const std::wstring needle = Lower(query_);
    int content_y = 0;  // Unscrolled content offset from grid_.top.
    current_group_ = 0;
    size_t group_index = 0;
    for (const Section& section : sections) {
      std::vector<const Cell*> shown;
      for (const Cell& cell : section.cells) {
        if (needle.empty() || Lower(cell.name).find(needle) != std::wstring::npos) {
          shown.push_back(&cell);
        }
      }
      section_tops_.push_back(content_y);
      if (content_y <= scroll_) {
        current_group_ = group_index;
      }
      ++group_index;
      if (shown.empty()) {
        continue;
      }
      if (!section.title.empty() && (needle.empty() || !emoji_tab_)) {
        RECT header = {grid_.left, grid_.top + content_y - scroll_, grid_.right,
                       grid_.top + content_y - scroll_ + Scaled(kHeaderHeight)};
        if (header.bottom > grid_.top && header.top < grid_.bottom) {
          headers_.push_back({section.title, header});
        }
        content_y += Scaled(kHeaderHeight) + Scaled(4);
      }
      for (size_t i = 0; i < shown.size(); ++i) {
        const int col = static_cast<int>(i % kColumns);
        const int row = static_cast<int>(i / kColumns);
        const int top = grid_.top + content_y + row * pitch - scroll_;
        RECT cell = {grid_.left + col * col_pitch, top,
                     grid_.left + col * col_pitch + Scaled(kCell),
                     top + Scaled(kCell)};
        flat_.push_back(*shown[i]);
        if (cell.bottom > grid_.top && cell.top < grid_.bottom) {
          AddHit(cell, static_cast<int>(flat_.size()) - 1, shown[i]->name,
                 PhiHitRole::kListItem, shown[i]->storage == current_);
        }
      }
      const int rows = static_cast<int>((shown.size() + kColumns - 1) / kColumns);
      content_y += rows * pitch + Scaled(kGap);
    }
    const int viewport = grid_.bottom - grid_.top;
    max_scroll_ = std::max(0, content_y - Scaled(kGap) - viewport);
    scroll_ = std::max(0, std::min(scroll_, max_scroll_));
    height_ = grid_.bottom + Scaled(kPad);
    Invalidate();
  }

  void MoveFocus(int delta) {
    if (flat_.empty()) {
      return;
    }
    int next = focus_ < 0 ? 0 : focus_ + delta;
    next = std::max(0, std::min(next, static_cast<int>(flat_.size()) - 1));
    focus_ = next;
    // Keep the focused cell in view: find its row offset by rebuilding
    // (cheap — the grid is a few hundred rects) and scrolling to it.
    Layout();
    for (const PhiHit& hit : hits_) {
      if (hit.key == focus_) {
        return;  // Visible.
      }
    }
    // Off-screen: page in the direction of travel.
    const int pitch = Scaled(kCell + kRowGap);
    scroll_ = std::max(0, std::min(scroll_ + (delta > 0 ? pitch : -pitch) *
                                                 std::max(1, std::abs(delta) / kColumns),
                                   max_scroll_));
    Layout();
  }

  PhiPalette palette_;
  std::string current_;
  std::string chosen_;
  PhiSpaceIconChosen on_chosen_;
  bool emoji_tab_ = false;
  std::wstring query_;
  int scroll_ = 0;
  int max_scroll_ = 0;
  int focus_ = -1;
  int height_ = 0;
  RECT search_{};
  RECT grid_{};
  size_t current_group_ = 0;
  std::vector<Section> icon_sections_;
  std::vector<Section> emoji_sections_;
  std::vector<std::wstring> category_glyphs_;
  std::vector<RECT> category_rects_;
  std::vector<int> section_tops_;
  std::vector<Header> headers_;
  std::vector<Cell> flat_;  // The cells the current tab + query show.
  HFONT label_font_ = nullptr;
  HFONT header_font_ = nullptr;
  HFONT icon_font_ = nullptr;
};

}  // namespace

std::wstring PhiIconSearchFold(const std::wstring& text) {
  std::wstring folded = text;
  for (wchar_t& c : folded) {
    c = static_cast<wchar_t>(::towlower(c));
  }
  return folded;
}

const std::vector<PhiIconChoiceGroup>& PhiIconChoiceGroups(bool emoji) {
  // Built on first use and kept: the emoji catalogue is ~1900 entries, and
  // an embedded grid rebuilds on every change of the page around it.
  static const base::NoDestructor<std::vector<PhiIconChoiceGroup>> icons([] {
    PhiIconChoiceGroup group;
    for (const PhiCatalogIcon& icon : PhiIconCatalogAll()) {
      PhiIconChoice choice;
      choice.text = std::wstring(1, icon.glyph);
      choice.storage = std::string(kPhiIconPrefix) + icon.id;
      choice.name = base::UTF8ToWide(icon.name);
      choice.folded = PhiIconSearchFold(choice.name);
      group.items.push_back(std::move(choice));
    }
    return std::vector<PhiIconChoiceGroup>{std::move(group)};
  }());
  static const base::NoDestructor<std::vector<PhiIconChoiceGroup>> emojis([] {
    std::vector<PhiIconChoiceGroup> groups;
    for (const PhiEmojiGroup& source : PhiEmojiGroups()) {
      PhiIconChoiceGroup group;
      group.title = base::UTF8ToWide(source.name);
      for (const PhiEmoji& emoji : source.items) {
        PhiIconChoice choice;
        choice.text = base::UTF8ToWide(emoji.text);
        choice.storage = std::string(kPhiEmojiPrefix) + emoji.id;
        choice.name = base::UTF8ToWide(emoji.name);
        choice.folded = PhiIconSearchFold(choice.name);
        group.items.push_back(std::move(choice));
      }
      groups.push_back(std::move(group));
    }
    return groups;
  }());
  return emoji ? *emojis : *icons;
}

void PhiOpenSpaceIconPicker(HWND owner,
                            const PhiPalette& palette,
                            const RECT& anchor_screen,
                            const std::string& current_storage,
                            PhiSpaceIconChosen on_chosen) {
  if (!owner || !::IsWindow(owner)) {
    if (on_chosen) {
      on_chosen(std::string());
    }
    return;
  }
  Picker* picker = new Picker(palette, current_storage, std::move(on_chosen));
  picker->DeleteWhenDestroyed();
  if (!picker->Open(owner, anchor_screen)) {
    picker->AnswerUnopened();
    delete picker;  // Never shown: OnNcDestroy will not run.
  }
}

std::string PhiPickSpaceIcon(HWND owner,
                             const PhiPalette& palette,
                             const RECT& anchor_screen,
                             const std::string& current_storage) {
  std::string chosen;
  bool done = false;
  PhiOpenSpaceIconPicker(owner, palette, anchor_screen, current_storage,
                         [&chosen, &done](const std::string& storage) {
                           chosen = storage;
                           done = true;
                         });
  if (done) {
    return chosen;  // The picker never opened.
  }
  PhiModalScope modal;
  MSG msg;
  while (!done) {
    const BOOL got = ::GetMessageW(&msg, nullptr, 0, 0);
    if (got <= 0) {
      if (got == 0) {
        // WM_QUIT is the OUTER loop's: put it back, or the app never exits.
        ::PostQuitMessage(static_cast<int>(msg.wParam));
      }
      break;
    }
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }
  return chosen;
}

}  // namespace phi_client

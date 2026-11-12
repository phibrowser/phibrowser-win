// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_group_overview.h"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "chrome/browser/phinomenon/phi_framework/phi_window_snapshot.h"
#include "client/phi_dwrite.h"
#include "client/phi_ids.h"
#include "client/phi_panel.h"
#include "client/phi_theme.h"
#include "client/phi_shell_window.h"
#include "client/phi_window_model.h"

namespace phi_client {

namespace {

constexpr int kCardWidth = 240;
constexpr int kCardHeight = 190;
constexpr int kCardGap = 20;
constexpr int kGridPad = 32;
constexpr int kFooterHeight = 30;
constexpr int kCloseBox = 18;

struct Card {
  bool add_card = false;  // The trailing "+" card.
  TabId tab_id;
  std::wstring title;
  std::vector<unsigned char> thumbnail;
  int thumb_w = 0;
  int thumb_h = 0;
  std::vector<unsigned char> favicon;
  int fav_w = 0;
  int fav_h = 0;
  RECT bounds{};
  RECT close_box{};
};

// SELF-OWNED, through the base: PhiGroupOverview::Close() destroys the
// window and DeleteWhenDestroyed() frees the object at WM_NCDESTROY. The
// old shape — Close() deleting the object, whose destructor destroyed the
// window, whose WM_KILLFOCUS called Close() again — needed the singleton
// nulled first as a documented landmine (lane 3 §5). It is now the base's
// ordinary teardown: the object outlives every teardown message and
// nothing can reach it after the HWND is gone.
class Overview : public PhiPanel {
 public:
  Overview(PhiShellWindow* controller, const std::string& group_id)
      : controller_(controller), group_id_(group_id) {
    ClassSpec spec;
    spec.name = L"PhiGroupOverview";
    // No class cursor, as before: the overview does not change the pointer.
    spec.cursor = nullptr;
    Options options;
    // Keeps the card's corner caps visible (PhiPanel adds WS_CLIPSIBLINGS).
    const RECT nothing = {0, 0, 0, 0};
    if (!CreatePanel(spec, controller_->shell_hwnd(), nothing, options)) {
      return;
    }
    DeleteWhenDestroyed();
    BuildCards();
    Reposition();
    ShowPanel();
    ::SetFocus(hwnd());
  }

  // No Destroy() here: this object is SELF-OWNED through the base, so it
  // is only ever freed after its window's WM_NCDESTROY (see Close()).
  // Destroying the window from the destructor is exactly the re-entrancy
  // this migration removes.
  ~Overview() override = default;

  static Overview* instance;

  PhiShellWindow* controller() const { return controller_; }
  const std::string& group_id() const { return group_id_; }

  // The window model moved. Re-reads the group's members when they (or
  // their titles) changed — tabs closed, added or renamed elsewhere — and
  // leaves when the group is gone. False when the overview should close.
  bool Refresh() {
    const std::string signature = MemberSignature();
    if (signature.empty()) {
      return false;  // No members: the group no longer exists.
    }
    if (signature == signature_) {
      return true;
    }
    BuildCards();
    LayoutCards();
    Invalidate();
    return true;
  }

 protected:
  void Paint(HDC dc, const RECT& dirty) override;

  void OnMouseDown(int, POINT pt) override {
    ::SetFocus(hwnd());  // Keep Escape working after a click.
    Click(pt);
  }

  void OnEscape() override { PhiGroupOverview::Close(); }

  void OnWheel(int delta, POINT) override { ScrollBy(-delta / 2); }

  bool OnKey(WPARAM key) override {
    RECT rc = {};
    ::GetClientRect(hwnd(), &rc);
    const int line = Scaled(kCardHeight) + Scaled(kCardGap);
    const int page = std::max(line, static_cast<int>(rc.bottom - rc.top) - line);
    switch (key) {
      case VK_UP:
        ScrollBy(-line);
        return true;
      case VK_DOWN:
        ScrollBy(line);
        return true;
      case VK_PRIOR:
        ScrollBy(-page);
        return true;
      case VK_NEXT:
      case VK_SPACE:
        ScrollBy(page);
        return true;
      case VK_HOME:
        ScrollBy(-ContentHeight());
        return true;
      case VK_END:
        ScrollBy(ContentHeight());
        return true;
      default:
        return PhiPanel::OnKey(key);
    }
  }

  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) override {
    if (message == WM_KILLFOCUS) {
      // Focus moved elsewhere (page click, another window): leave the
      // overview — matching the Mac's dismissal.
      *handled = true;
      PhiGroupOverview::Close();
      return 0;
    }
    return PhiPanel::OnMessage(message, wparam, lparam, handled);
  }

  void OnNcDestroy() override {
    if (instance == this) {
      instance = nullptr;
    }
    PhiPanel::OnNcDestroy();
  }

 private:
  HFONT font() { return fonts().Get(12, FW_NORMAL); }

  PhiPalette QueryPalette() const override {
    return controller_ ? controller_->CurrentPalette() : PhiPalette();
  }

  // The group's tab ids and titles, in strip order; "" when it has none.
  std::string MemberSignature() const {
    const PhiWindowModel* model = controller_->model();
    const BrowserId browser = controller_->active_browser_id();
    const phi_framework::WindowSnapshot* snapshot =
        model && browser.valid() ? model->Get(browser) : nullptr;
    std::string out;
    if (snapshot) {
      for (const phi_framework::TabSnapshot& tab : snapshot->tabs) {
        if (tab.group_id == group_id_) {
          out += base::NumberToString(tab.id.value) + ":" + tab.title + "\n";
        }
      }
    }
    return out;
  }

  // The grid's full height, both pads included.
  int ContentHeight() const {
    if (cards_.empty()) {
      return 0;
    }
    const int rows = (static_cast<int>(cards_.size()) + per_row_ - 1) / per_row_;
    return Scaled(kGridPad) * 2 + rows * Scaled(kCardHeight) +
           (rows - 1) * Scaled(kCardGap);
  }

  void ScrollBy(int dy) {
    const int before = scroll_;
    scroll_ += dy;
    LayoutCards();  // Clamps.
    if (scroll_ != before) {
      Invalidate();
    }
  }

  void Reposition() {
    // The overlay covers the CONTENT card (the page area).
    SetPanelBounds(controller_->ContentRect());
    LayoutCards();
  }

  // The group's members from the window model — the same snapshot the
  // sidebar's rows come from, never a parse of the framework's JSON.
  void BuildCards() {
    cards_.clear();
    signature_ = MemberSignature();
    phi_framework::PhiFramework* framework = controller_->framework();
    const PhiWindowModel* model = controller_->model();
    const BrowserId browser = controller_->active_browser_id();
    const phi_framework::WindowSnapshot* snapshot =
        framework && model && browser.valid() ? model->Get(browser) : nullptr;
    if (snapshot) {
      for (const phi_framework::TabSnapshot& tab : snapshot->tabs) {
        if (tab.group_id != group_id_) {
          continue;
        }
        Card card;
        card.tab_id = tab.id;
        card.title = base::UTF8ToWide(tab.title);
        int w = 0, h = 0;
        if (const unsigned char* pixels = framework->TabThumbnailBgra(
                browser.value, tab.id.value, &w, &h)) {
          const auto copy = UNSAFE_BUFFERS(base::span<const unsigned char>(
              pixels, static_cast<size_t>(w) * h * 4));
          card.thumbnail.assign(copy.begin(), copy.end());
          card.thumb_w = w;
          card.thumb_h = h;
        }
        if (const unsigned char* pixels = framework->TabFaviconBgraById(
                browser.value, tab.id.value, &w, &h)) {
          const auto copy = UNSAFE_BUFFERS(base::span<const unsigned char>(
              pixels, static_cast<size_t>(w) * h * 4));
          card.favicon.assign(copy.begin(), copy.end());
          card.fav_w = w;
          card.fav_h = h;
        }
        cards_.push_back(std::move(card));
      }
    }
    Card add;
    add.add_card = true;
    cards_.push_back(std::move(add));
  }

  void LayoutCards() {
    RECT rc = {};
    ::GetClientRect(hwnd(), &rc);
    const int usable = rc.right - rc.left - Scaled(kGridPad) * 2;
    const int per_row = std::max(
        1, (usable + Scaled(kCardGap)) /
               (Scaled(kCardWidth) + Scaled(kCardGap)));
    per_row_ = per_row;
    // Clamped against the current card count, so a card leaving at the
    // bottom of a scrolled grid never leaves a gap.
    const int limit =
        std::max(0, ContentHeight() - static_cast<int>(rc.bottom - rc.top));
    scroll_ = std::clamp(scroll_, 0, limit);
    int x = rc.left + Scaled(kGridPad);
    int y = rc.top + Scaled(kGridPad) - scroll_;
    int in_row = 0;
    for (Card& card : cards_) {
      card.bounds = {x, y, x + Scaled(kCardWidth), y + Scaled(kCardHeight)};
      card.close_box = {card.bounds.right - Scaled(kCloseBox) - Scaled(6),
                        card.bounds.top + Scaled(6),
                        card.bounds.right - Scaled(6),
                        card.bounds.top + Scaled(6) + Scaled(kCloseBox)};
      if (++in_row >= per_row) {
        in_row = 0;
        x = rc.left + Scaled(kGridPad);
        y += Scaled(kCardHeight) + Scaled(kCardGap);
      } else {
        x += Scaled(kCardWidth) + Scaled(kCardGap);
      }
    }
  }

  void PaintCards(HDC dc) {
    RECT rc = {};
    ::GetClientRect(hwnd(), &rc);
    const PhiPalette palette = QueryPalette();
    HBRUSH bg = ::CreateSolidBrush(palette.window_bg);
    ::FillRect(dc, &rc, bg);
    ::DeleteObject(bg);
    ::SetBkMode(dc, TRANSPARENT);
    for (const Card& card : cards_) {
      if (card.add_card) {
        PhiFillRoundRectAA(dc, card.bounds, Scaled(10), palette.hover);
        RECT plus = card.bounds;
        HGDIOBJ prev_font = ::SelectObject(dc, font());
        ::SetTextColor(dc, palette.label_dim);
        ::DrawTextW(dc, L"+", -1, &plus,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (prev_font) {
          ::SelectObject(dc, prev_font);
        }
        continue;
      }
      RECT shot = card.bounds;
      shot.bottom -= Scaled(kFooterHeight);
      if (!card.thumbnail.empty()) {
        PhiDrawBgra(dc, shot, card.thumbnail.data(), card.thumb_w,
                    card.thumb_h);
      } else {
        HBRUSH ground = ::CreateSolidBrush(palette.card);
        ::FillRect(dc, &shot, ground);
        ::DeleteObject(ground);
      }
      HPEN pen = ::CreatePen(PS_SOLID, 1, palette.border);
      HGDIOBJ prev_pen = ::SelectObject(dc, pen);
      HGDIOBJ prev_brush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
      ::Rectangle(dc, card.bounds.left, card.bounds.top, card.bounds.right,
                  card.bounds.bottom);
      ::SelectObject(dc, prev_brush);
      ::SelectObject(dc, prev_pen);
      ::DeleteObject(pen);
      // Close box.
      HGDIOBJ prev_font = ::SelectObject(dc, font());
      ::SetTextColor(dc, palette.label_dim);
      RECT close_rc = card.close_box;
      ::DrawTextW(dc, L"✕", -1, &close_rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      RECT footer = card.bounds;
      footer.top = shot.bottom + Scaled(4);
      RECT icon_rc = {footer.left + Scaled(8), footer.top,
                      footer.left + Scaled(24), footer.bottom - Scaled(6)};
      if (!card.favicon.empty()) {
        PhiBlendBgra(dc, icon_rc, card.favicon.data(), card.fav_w,
                     card.fav_h);
      }
      RECT title_rc = {icon_rc.right + Scaled(6), footer.top,
                       footer.right - Scaled(8), footer.bottom};
      ::SetTextColor(dc, palette.label);
      ::DrawTextW(dc, card.title.c_str(), -1, &title_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
      if (prev_font) {
        ::SelectObject(dc, prev_font);
      }
    }
  }

  void Click(POINT pt) {
    phi_framework::PhiFramework* framework = controller_->framework();
    const BrowserId browser = controller_->active_browser_id();
    if (!browser.valid()) {
      return;  // No browser attached: nothing to activate or close.
    }
    for (size_t i = 0; i < cards_.size(); ++i) {
      const Card& card = cards_[i];
      if (!::PtInRect(&card.bounds, pt)) {
        continue;
      }
      if (card.add_card) {
        if (framework) {
          // OpenTab answers with the stable id the group call wants.
          const TabId fresh = framework->OpenTab(browser, "");
          if (fresh.valid()) {
            const std::string ids =
                "[" + base::NumberToString(fresh.value) + "]";
            framework->AddTabsToGroup(browser.value, group_id_.c_str(),
                                      ids.c_str());
          }
        }
        PhiGroupOverview::Close();
        return;
      }
      if (::PtInRect(&card.close_box, pt)) {
        if (framework && card.tab_id.valid()) {
          framework->CloseTabById(browser.value, card.tab_id.value);
        }
        // The card leaves in place: the model hears of the close on its
        // push, but the overview is the user's view of it right now.
        cards_.erase(cards_.begin() + static_cast<std::ptrdiff_t>(i));
        LayoutCards();
        Invalidate();
        return;
      }
      if (framework && card.tab_id.valid()) {
        framework->ActivateTabById(browser.value, card.tab_id.value);
      }
      PhiGroupOverview::Close();
      return;
    }
  }

  RAW_PTR_EXCLUSION PhiShellWindow* const controller_;
  const std::string group_id_;
  std::vector<Card> cards_;
  std::string signature_;  // MemberSignature() the cards were built from.
  int per_row_ = 1;
  int scroll_ = 0;
};

Overview* Overview::instance = nullptr;

void Overview::Paint(HDC dc, const RECT&) {
  PaintCards(dc);
}

}  // namespace

// static
bool PhiGroupOverview::IsOpen() {
  return Overview::instance != nullptr;
}

// static
std::string PhiGroupOverview::OpenGroupId() {
  return Overview::instance ? Overview::instance->group_id() : std::string();
}

// static
void PhiGroupOverview::Open(PhiShellWindow* controller,
                            const std::string& group_id) {
  Close();
  if (controller) {
    Overview::instance = new Overview(controller, group_id);
    // The address row drops the page colour under the overview.
    controller->ScheduleRefresh();
  }
}

// static
void PhiGroupOverview::RefreshIfBoundTo(const PhiShellWindow* controller) {
  if (Overview::instance && Overview::instance->controller() == controller &&
      !Overview::instance->Refresh()) {
    Close();
  }
}

// static
void PhiGroupOverview::CloseIfBoundTo(const PhiShellWindow* controller) {
  if (Overview::instance && Overview::instance->controller() == controller) {
    Close();
  }
}

// static
void PhiGroupOverview::Close() {
  if (Overview* gone = Overview::instance) {
    // The singleton is released BEFORE the teardown starts, so the
    // WM_KILLFOCUS that DestroyWindow delivers finds no overview to close
    // and simply returns. The object itself is freed by the base at
    // WM_NCDESTROY (DeleteWhenDestroyed), not from inside a message.
    Overview::instance = nullptr;
    if (PhiShellWindow* controller = gone->controller()) {
      controller->ScheduleRefresh();  // The page colour comes back.
    }
    if (gone->alive()) {
      gone->Destroy();  // DeleteWhenDestroyed frees it at WM_NCDESTROY.
    } else {
      delete gone;  // It never got a window to be destroyed with.
    }
  }
}

}  // namespace phi_client

// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_tab_switcher.h"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <string>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "chrome/browser/phinomenon/phi_framework/phi_window_snapshot.h"
#include "client/phi_agent_spaces.h"
#include "client/phi_bookmark_manager.h"
#include "client/phi_dwrite.h"
#include "client/phi_ids.h"
#include "client/phi_popover.h"
#include "client/phi_theme.h"
#include "client/phi_shell_window.h"
#include "client/phi_window_model.h"

namespace phi_client {

namespace {

// Card metrics (DIP) — the Mac TabSwitchMetrics' shape.
constexpr int kCardWidth = 192;
constexpr int kCardHeight = 150;
constexpr int kCardGap = 12;
constexpr int kPanelPad = 16;
constexpr int kFooterHeight = 28;
// The Mac's TabSwitchMetrics.maxRecentTabs: a gesture walks at most the
// five most recent tabs.
constexpr size_t kMaxCards = 5;
// The HUD's watchdog: focus leaving the owner or Esc cancels it; a hold-key
// release the keyboard hook never saw commits it.
constexpr UINT kPollMs = 50;
// The Mac's TabSwitchMetrics.longPressThreshold: the cards show only when
// the hold keys are still down this long after the first press (or at a
// second press). A quicker tap switches with nothing on screen.
constexpr UINT kRevealMs = 250;

// Whether every hold key of `mask` is down now.
bool HoldKeysDown(unsigned int mask) {
  if ((mask & PhiTabSwitcher::kHoldCtrl) &&
      !(::GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
    return false;
  }
  if ((mask & PhiTabSwitcher::kHoldAlt) &&
      !(::GetAsyncKeyState(VK_MENU) & 0x8000)) {
    return false;
  }
  return true;
}
// Inset from the work-area edges the card strip keeps (DIP).
constexpr int kScreenMargin = 16;

// A close requested from inside the HUD's own window procedure runs from
// the message loop instead (a thread timer), so the window is never
// destroyed under its own dispatch.
UINT_PTR g_deferred_close = 0;
void CALLBACK DeferredClose(HWND, UINT, UINT_PTR id, DWORD);

struct Card {
  TabId tab_id;
  // Where the tab lives: another Space of the window when the switcher
  // includes all Spaces.
  BrowserId browser;
  std::string space_id;
  std::wstring title;
  std::vector<unsigned char> thumbnail;
  int thumb_w = 0;
  int thumb_h = 0;
  std::vector<unsigned char> favicon;
  int fav_w = 0;
  int fav_h = 0;
};

// The HUD keeps its MRU model and its Paint(); the window itself — class
// registration, `this` binding, the back buffer, the dpi and the fonts —
// belongs to PhiPopover now (lane 3 §6.2).
class Hud : public PhiPopover {
 public:
  Hud(PhiShellWindow* controller, int direction, unsigned int hold_mask)
      : controller_(controller), hold_mask_(hold_mask) {
    ClassSpec spec;
    spec.name = L"PhiTabSwitch";
    spec.cursor = nullptr;

    Options options;
    options.activate = false;
    options.foreground = false;
    options.topmost = true;
    options.dwm_round = false;  // PhiFillRoundRectAA draws the card.
    options.dismiss_on_inactive = false;  // Releasing Ctrl commits it.
    options.dismiss_on_escape = false;
    options.click_through = true;

    Placement placement;
    placement.side = PhiPopoverSide::kExplicit;
    // Kept on the owner's monitor: a window straddling an edge still gets
    // the whole strip on screen.
    placement.clamp = true;
    placement.monitor_from_owner = true;
    SIZE probe = {1, 1};
    if (!CreatePopover(spec, controller_->shell_hwnd(), probe, placement,
                       options)) {
      return;
    }
    // As many cards as the work area holds, never more than kMaxCards.
    MONITORINFO monitor = {sizeof(monitor)};
    if (::GetMonitorInfoW(::MonitorFromWindow(controller_->shell_hwnd(),
                                              MONITOR_DEFAULTTONEAREST),
                          &monitor)) {
      const int room = monitor.rcWork.right - monitor.rcWork.left -
                       Scaled(kScreenMargin) * 2 - Scaled(kPanelPad) * 2 +
                       Scaled(kCardGap);
      const int fit = room / (Scaled(kCardWidth) + Scaled(kCardGap));
      max_cards_ = static_cast<size_t>(
          std::clamp(fit, 1, static_cast<int>(kMaxCards)));
    }
    BuildCards(direction);
    const int count = static_cast<int>(std::min(cards_.size(), max_cards_));
    SIZE size = {};
    size.cx = Scaled(kPanelPad) * 2 + count * Scaled(kCardWidth) +
              std::max(0, count - 1) * Scaled(kCardGap);
    size.cy = Scaled(kPanelPad) * 2 + Scaled(kCardHeight);
    RECT owner_rect = {};
    ::GetWindowRect(controller_->shell_hwnd(), &owner_rect);
    const LONG x = (owner_rect.left + owner_rect.right) / 2 - size.cx / 2;
    const LONG y = (owner_rect.top + owner_rect.bottom) / 2 - size.cy / 2;
    placement.anchor = {x, y, x, y};
    Reposition(placement, size);
    if (!hold_mask_) {
      return;  // One step, committed by the caller; never shown.
    }
    poll_ = NewTimer();
    poll_.Start(kPollMs);
    reveal_ = NewTimer();
    reveal_.Start(kRevealMs);
  }

  ~Hud() override { Destroy(); }

  // Set once a close is pending: the HUD is hidden and takes no more
  // input; DeferredClose commits (when `commit_pending`) and deletes it.
  bool cancelled() const { return cancelled_; }
  bool commit_pending() const { return commit_pending_; }
  unsigned int hold_mask() const { return hold_mask_; }
  // The cards are on screen (see kRevealMs).
  bool revealed() const { return revealed_; }

  // Hides now and finishes from the message loop (see DeferredClose).
  void RequestClose(bool commit) {
    if (cancelled_) {
      return;
    }
    cancelled_ = true;
    commit_pending_ = commit;
    poll_.Stop();
    reveal_.Stop();
    if (alive()) {
      ::ShowWindow(hwnd(), SW_HIDE);
    }
    if (!g_deferred_close) {
      g_deferred_close = ::SetTimer(nullptr, 0, 0, &DeferredClose);
    }
  }

  void Advance(int direction) {
    if (cards_.empty()) {
      return;
    }
    const int count = static_cast<int>(cards_.size());
    selected_ = ((selected_ + direction) % count + count) % count;
    Invalidate();
    // A second press shows the cards at once (the Mac's advanceSession).
    Reveal();
  }

  void Reveal() {
    reveal_.Stop();
    if (revealed_ || cancelled_ || !alive()) {
      return;
    }
    revealed_ = true;
    Show();
  }

  void Commit() {
    if (selected_ >= 0 && selected_ < static_cast<int>(cards_.size())) {
      const Card card = cards_[static_cast<size_t>(selected_)];
      phi_framework::PhiFramework* framework = controller_->framework();
      const BrowserId browser =
          card.browser.valid() ? card.browser : controller_->active_browser_id();
      if (framework && browser.valid() && card.tab_id.valid()) {
        // A tab of another Space: the tab first, so the Space arrives
        // showing it, then the Space.
        framework->ActivateTabById(browser.value, card.tab_id.value);
        if (!card.space_id.empty() &&
            card.space_id != controller_->active_space()) {
          controller_->SwitchToSpace(card.space_id);
        }
        // The framework's push refreshes the sidebar; nothing to do here.
      }
    }
  }

  static Hud* instance;

  PhiShellWindow* controller() const { return controller_; }

 protected:
  // The HUD follows the shell's live palette, so a theme switch (or an OS
  // dark/light toggle through PhiThemeBus) repaints it.
  PhiPalette QueryPalette() const override {
    return controller_->CurrentPalette();
  }

  void Paint(HDC dc, const RECT&) override { PaintCards(dc); }

  // The app losing activation (Alt+Tab, a click into another program, the
  // Start menu) cancels: the Ctrl release that would commit goes to
  // another app, and a topmost HUD must not outlive the gesture.
  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) override {
    if (message == WM_ACTIVATEAPP && !wparam) {
      RequestClose(/*commit=*/false);
    }
    return PhiPopover::OnMessage(message, wparam, lparam, handled);
  }

  void OnTimerFired(UINT_PTR id) override {
    if (cancelled_) {
      return;
    }
    if (id == reveal_.id()) {
      // The quarter second is up: still held, the cards show; let go
      // (before the hook saw the release), the step commits unseen.
      reveal_.Stop();
      if (HoldKeysDown(hold_mask_)) {
        Reveal();
      } else {
        RequestClose(/*commit=*/true);
      }
      return;
    }
    if (id != poll_.id()) {
      return;
    }
    // Another of our own windows taking activation (a second Phi window,
    // a dialog) is the same as the app losing it.
    HWND foreground = ::GetForegroundWindow();
    const bool owner_active =
        foreground && ::GetAncestor(foreground, GA_ROOTOWNER) ==
                          ::GetAncestor(owner(), GA_ROOTOWNER);
    // Esc held: the hook did not route it (PhiTabSwitcher::HandleKeyDown
    // is the swallowing path).
    const bool escape = (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    if (!owner_active || escape) {
      RequestClose(/*commit=*/false);
      return;
    }
    // A hold key released for two ticks with no commit arriving (a
    // WM_TIMER only runs once the input queue is drained): the hook missed
    // the key-up — a tap released before the gesture began. Releasing
    // commits.
    hold_up_ticks_ = HoldKeysDown(hold_mask_) ? 0 : hold_up_ticks_ + 1;
    if (hold_up_ticks_ >= 2) {
      RequestClose(/*commit=*/true);
    }
  }

 private:
  // The Mac's recentTabIDs: the focused tab first, then every tab this
  // window has shown — or seen opened in the background, which comes
  // straight after the focused one — newest first; a tab never seen is no
  // card. At most max_cards_. The Spaces' own tabs from the window model,
  // never a parse of the framework's JSON. `direction` picks the first
  // selection: forward the previous tab, reverse the oldest card.
  void BuildCards(int direction) {
    phi_framework::PhiFramework* framework = controller_->framework();
    const PhiWindowModel* model = controller_->model();
    const BrowserId browser = controller_->active_browser_id();
    if (!framework || !model || !browser.valid()) {
      return;  // No browser attached: an empty switcher.
    }
    struct Entry {
      TabId id;
      std::wstring title;
      BrowserId browser;
      std::string space_id;
    };
    std::vector<Entry> entries;
    const phi_framework::WindowSnapshot* current = model->Get(browser);
    if (current) {
      for (const phi_framework::TabSnapshot& tab : current->tabs) {
        entries.push_back(
            {tab.id,
             base::UTF8ToWide(PhiTabDisplayTitle(
                 tab.title, tab.url, model->PendingUrl(browser, tab.id))),
             browser, controller_->active_space()});
      }
    }
    if (current && controller_->TabSwitcherIncludesAllSpaces()) {
      // Every Space of this window with the same profile and the same
      // Incognito state, never an agent Space, so no title or thumbnail
      // crosses those lines (the Mac's `c8c96bca`).
      for (const std::string& space_id : controller_->space_order()) {
        const BrowserId other = controller_->BrowserIdForSpace(space_id);
        if (!other.valid() || other == browser || PhiIsAgentSpaceId(space_id)) {
          continue;
        }
        const phi_framework::WindowSnapshot* snapshot = model->Get(other);
        if (!snapshot || snapshot->profile_id != current->profile_id ||
            snapshot->incognito != current->incognito) {
          continue;
        }
        for (const phi_framework::TabSnapshot& tab : snapshot->tabs) {
          if (tab.surface == phi_framework::TabSurface::kContent) {
            entries.push_back(
                {tab.id,
                 base::UTF8ToWide(PhiTabDisplayTitle(
                     tab.title, tab.url, model->PendingUrl(other, tab.id))),
                 other, space_id});
          }
        }
      }
    }
    // The seen stamps outlive a Space switch, so each Space keeps its own
    // recent order, as each Mac Space keeps its TabSwitchManager.
    const TabId focused = controller_->active_tab_id();
    std::erase_if(entries, [&](const Entry& entry) {
      return entry.id != focused && controller_->TabSeenStamp(entry.id) == 0;
    });
    std::stable_sort(entries.begin(), entries.end(),
                     [&](const Entry& a, const Entry& b) {
                       if ((a.id == focused) != (b.id == focused)) {
                         return a.id == focused;
                       }
                       return controller_->TabSeenStamp(a.id) >
                              controller_->TabSeenStamp(b.id);
                     });
    for (const Entry& entry : entries) {
      if (cards_.size() >= max_cards_) {
        break;
      }
      Card card;
      card.tab_id = entry.id;
      card.title = entry.title;
      card.browser = entry.browser;
      card.space_id = entry.space_id;
      int w = 0, h = 0;
      if (const unsigned char* pixels = framework->TabThumbnailBgra(
              entry.browser.value, entry.id.value, &w, &h)) {
        const auto copy = UNSAFE_BUFFERS(base::span<const unsigned char>(
            pixels, static_cast<size_t>(w) * h * 4));
        card.thumbnail.assign(copy.begin(), copy.end());
        card.thumb_w = w;
        card.thumb_h = h;
      }
      if (const unsigned char* pixels = framework->TabFaviconBgraById(
              entry.browser.value, entry.id.value, &w, &h)) {
        const auto copy = UNSAFE_BUFFERS(base::span<const unsigned char>(
            pixels, static_cast<size_t>(w) * h * 4));
        card.favicon.assign(copy.begin(), copy.end());
        card.fav_w = w;
        card.fav_h = h;
      }
      cards_.push_back(std::move(card));
    }
    // The Mac's startSession: one step from the focused tab, either way
    // round (forward the previous tab, reverse the oldest card); with no
    // focused card, the newest forward and the oldest in reverse.
    const int count = static_cast<int>(cards_.size());
    if (count <= 1) {
      selected_ = 0;
      return;
    }
    int at = -1;
    for (int i = 0; i < count; ++i) {
      if (cards_[static_cast<size_t>(i)].tab_id == focused) {
        at = i;
        break;
      }
    }
    if (at < 0) {
      selected_ = direction < 0 ? count - 1 : 0;
    } else {
      selected_ = ((at + (direction < 0 ? -1 : 1)) % count + count) % count;
    }
  }

  void PaintCards(HDC dc) {
    RECT rc = {};
    ::GetClientRect(hwnd(), &rc);
    const PhiPalette& palette = PhiWindow::palette();
    PhiFillRoundRectAA(dc, rc, Scaled(16), palette.card);
    PhiStrokeRoundRectAA(dc, rc, Scaled(16), palette.border, 1);
    ::SetBkMode(dc, TRANSPARENT);
    int x = rc.left + Scaled(kPanelPad);
    for (size_t i = 0; i < cards_.size() && i < max_cards_; ++i) {
      const Card& card = cards_[i];
      RECT card_rc = {x, rc.top + Scaled(kPanelPad),
                      x + Scaled(kCardWidth),
                      rc.top + Scaled(kPanelPad) + Scaled(kCardHeight)};
      if (static_cast<int>(i) == selected_) {
        RECT halo = card_rc;
        ::InflateRect(&halo, Scaled(4), Scaled(4));
        PhiFillRoundRectAA(dc, halo, Scaled(10), palette.hover_strong);
        PhiStrokeRoundRectAA(dc, halo, Scaled(10), palette.accent,
                             Scaled(2));
      }
      // Snapshot area (16:10-ish) with the footer strip below it.
      RECT shot = card_rc;
      shot.bottom -= Scaled(kFooterHeight);
      if (!card.thumbnail.empty()) {
        PhiDrawBgra(dc, shot, card.thumbnail.data(), card.thumb_w,
                    card.thumb_h);
      } else {
        HBRUSH ground = ::CreateSolidBrush(palette.hover);
        ::FillRect(dc, &shot, ground);
        ::DeleteObject(ground);
      }
      RECT footer = card_rc;
      footer.top = shot.bottom + Scaled(4);
      RECT icon_rc = {footer.left + Scaled(6), footer.top,
                      footer.left + Scaled(22), footer.bottom - Scaled(4)};
      if (!card.favicon.empty()) {
        PhiBlendBgra(dc, icon_rc, card.favicon.data(), card.fav_w,
                     card.fav_h);
      }
      RECT title_rc = {icon_rc.right + Scaled(6), footer.top,
                       footer.right - Scaled(6), footer.bottom};
      HFONT font = fonts().Get(12, FW_NORMAL);
      HGDIOBJ prev_font = font ? ::SelectObject(dc, font) : nullptr;
      ::SetTextColor(dc, palette.label);
      ::DrawTextW(dc, card.title.c_str(), -1, &title_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
      if (prev_font) {
        ::SelectObject(dc, prev_font);
      }
      x += Scaled(kCardWidth) + Scaled(kCardGap);
    }
  }

  RAW_PTR_EXCLUSION PhiShellWindow* const controller_;
  std::vector<Card> cards_;
  int selected_ = 0;
  size_t max_cards_ = kMaxCards;
  const unsigned int hold_mask_;
  Timer poll_;
  Timer reveal_;
  int hold_up_ticks_ = 0;
  bool revealed_ = false;
  bool cancelled_ = false;
  bool commit_pending_ = false;
};

Hud* Hud::instance = nullptr;

void CALLBACK DeferredClose(HWND, UINT, UINT_PTR id, DWORD) {
  ::KillTimer(nullptr, id);
  g_deferred_close = 0;
  if (Hud::instance && Hud::instance->commit_pending()) {
    Hud::instance->Commit();
  }
  PhiTabSwitcher::Cancel();
}

}  // namespace

// static
bool PhiTabSwitcher::IsActive() {
  return Hud::instance != nullptr && !Hud::instance->cancelled();
}

// static
bool PhiTabSwitcher::HandleKeyDown(unsigned int virtual_key) {
  if (virtual_key != VK_ESCAPE || !IsActive()) {
    return false;
  }
  Cancel();
  return true;
}

// static
bool PhiTabSwitcher::ReleasesHold(unsigned int virtual_key) {
  if (!IsActive()) {
    return false;
  }
  const unsigned int mask = Hud::instance->hold_mask();
  switch (virtual_key) {
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
      return (mask & kHoldCtrl) != 0;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
      return (mask & kHoldAlt) != 0;
    default:
      return false;
  }
}

// static
void PhiTabSwitcher::Cycle(PhiShellWindow* controller,
                           int direction,
                           unsigned int hold_mask) {
  if (Hud::instance && Hud::instance->cancelled()) {
    Cancel();  // A pending close: this press starts a fresh gesture.
  }
  if (!Hud::instance) {
    if (!controller) {
      return;
    }
    Hud::instance = new Hud(controller, direction, hold_mask);
    if (!hold_mask) {
      Commit();  // Nothing to hold: the first step is the whole gesture.
    }
    return;  // Opening already selected the first step.
  }
  Hud::instance->Advance(direction);
}

// static
void PhiTabSwitcher::Commit() {
  if (!Hud::instance) {
    return;
  }
  if (!Hud::instance->cancelled()) {
    Hud::instance->Commit();
  }
  delete Hud::instance;
  Hud::instance = nullptr;
}

// static
void PhiTabSwitcher::Cancel() {
  if (g_deferred_close) {
    ::KillTimer(nullptr, g_deferred_close);
    g_deferred_close = 0;
  }
  delete Hud::instance;
  Hud::instance = nullptr;
}

// static
void PhiTabSwitcher::CancelIfBoundTo(const PhiShellWindow* controller) {
  if (Hud::instance && Hud::instance->controller() == controller) {
    Cancel();
  }
}

}  // namespace phi_client

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_notification_cards.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "client/phi_dwrite.h"
#include "client/phi_i18n.h"
#include "client/phi_local_store.h"
#include "client/phi_popover.h"

namespace phi_client {

namespace {

// The shared queue's depth (NotificationCardManager.maxQueueSize).
constexpr size_t kMaxCards = 5;

// NotificationMessageCardView's metrics, in DIP: the sidebar card is 275
// wide, 10/8 padded, a 12 pt semibold title over up to three 12 pt lines,
// 20 pt icon buttons, and an accent button row under it.
constexpr int kCardWidth = 275;
constexpr int kPadX = 10;
constexpr int kPadY = 8;
constexpr int kIconSize = 20;
constexpr int kIconGap = 2;
constexpr int kTitleGap = 4;
constexpr int kRowGap = 8;
constexpr int kButtonHeight = 24;
constexpr int kPageWidth = 24;
constexpr int kButtonRadius = 6;
constexpr int kIconRadius = 4;
constexpr int kTitleSize = 12;  // Pixels at 96 dpi (PhiFontDesc).
constexpr int kMessageSize = 12;
constexpr int kButtonSize = 11;
constexpr int kGlyphSize = 10;
constexpr int kMessageMaxLines = 3;
// How far in from the owner's client corner the card sits.
constexpr int kInset = 16;

// MDL2 glyphs: ChromeClose, ChromeMinimize, ChevronLeft, ChevronRight.
constexpr wchar_t kGlyphClose[] = L"\xE8BB";
constexpr wchar_t kGlyphHide[] = L"\xE921";
constexpr wchar_t kGlyphPrevious[] = L"\xE76B";
constexpr wchar_t kGlyphNext[] = L"\xE76C";

enum HitKey {
  kHitClose = 1,
  kHitHide,
  kHitPrimary,
  kHitPrevious,
  kHitNext,
};

PhiNotificationCards* g_cards = nullptr;

}  // namespace

// The one card on screen. Owned by the manager and only ever hidden, never
// destroyed, from its own click: a click that decides the last card would
// otherwise free the window under its own message.
class PhiNotificationCardView : public PhiPopover {
 public:
  explicit PhiNotificationCardView(PhiNotificationCards* cards)
      : cards_(cards) {}
  ~PhiNotificationCardView() override = default;

  bool Open(HWND owner, const PhiPalette& palette) {
    ClassSpec spec;
    spec.name = L"PhiNotificationCard";
    Options options;
    options.activate = false;
    options.foreground = false;
    options.dismiss_on_inactive = false;
    options.dismiss_on_escape = false;
    Placement placement;
    placement.side = PhiPopoverSide::kExplicit;
    placement.clamp = false;
    if (!CreatePopover(spec, owner, SIZE{1, 1}, placement, options)) {
      return false;
    }
    SetPalette(palette);
    EnableTooltips(true);
    return true;
  }

  // Shows `card` (index `index` of `count`) in the owner's bottom-right
  // corner.
  void Render(const PhiNotificationCards::Card& card,
              size_t index,
              size_t count) {
    title_ = card.title;
    message_ = card.message;
    button_ = card.button_title;
    index_ = index;
    count_ = count;
    Layout();
    Place();
    Invalidate();
    if (!::IsWindowVisible(hwnd())) {
      Show();
    }
  }

  void Hide() {
    if (alive()) {
      ::ShowWindow(hwnd(), SW_HIDE);
    }
  }

  // Re-anchors to the owner after it moved or resized.
  void Place() {
    if (!alive() || !owner()) {
      return;
    }
    RECT client = {};
    ::GetClientRect(owner(), &client);
    POINT corner = {client.right, client.bottom};
    ::ClientToScreen(owner(), &corner);
    const LONG x = corner.x - Scaled(kInset) - size_.cx;
    const LONG y = corner.y - Scaled(kInset) - size_.cy;
    Placement placement;
    placement.side = PhiPopoverSide::kExplicit;
    placement.clamp = false;
    placement.anchor = {x, y, x, y};
    Reposition(placement, size_);
  }

 protected:
  void Paint(HDC dc, const RECT&) override {
    const PhiPalette& colors = palette();
    RECT rc = {};
    ::GetClientRect(hwnd(), &rc);
    HBRUSH ground = ::CreateSolidBrush(colors.card);
    ::FillRect(dc, &rc, ground);
    ::DeleteObject(ground);
    ::SetBkMode(dc, TRANSPARENT);

    // Hover washes under the icon and paging buttons.
    for (int key : {kHitClose, kHitHide, kHitPrevious, kHitNext}) {
      const PhiHit* hit = HitByKey(key);
      if (hit && hovered() >= 0 && hits()[hovered()].key == key) {
        PhiFillRoundRectAA(dc, PhiToRect(hit->bounds), Scaled(kIconRadius),
                           colors.hover_strong);
      }
    }

    ::SelectObject(dc, fonts().Get(kTitleSize, FW_SEMIBOLD));
    ::SetTextColor(dc, colors.label);
    RECT title = title_rect_;
    ::DrawTextW(dc, title_.c_str(), -1, &title,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);

    ::SelectObject(dc, fonts().Get(kMessageSize, FW_NORMAL));
    ::SetTextColor(dc, colors.label_dim);
    RECT message = message_rect_;
    PhiDrawTextAA(dc, message_.c_str(), -1, &message,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS |
                      DT_EDITCONTROL | DT_NOPREFIX);

    const PhiFontDesc glyph_font = {kGlyphSize, FW_NORMAL,
                                    L"Segoe MDL2 Assets"};
    ::SelectObject(dc, fonts().Get(glyph_font));
    ::SetTextColor(dc, colors.label_dim);
    DrawGlyph(dc, kHitClose, kGlyphClose);
    DrawGlyph(dc, kHitHide, kGlyphHide);
    DrawGlyph(dc, kHitPrevious, kGlyphPrevious);
    DrawGlyph(dc, kHitNext, kGlyphNext);

    if (const PhiHit* primary = HitByKey(kHitPrimary)) {
      const bool hot =
          hovered() >= 0 && hits()[hovered()].key == kHitPrimary;
      RECT button = PhiToRect(primary->bounds);
      PhiFillRoundRectAA(dc, button, Scaled(kButtonRadius),
                         hot ? colors.accent_hover : colors.accent);
      ::SelectObject(dc, fonts().Get(kButtonSize, FW_SEMIBOLD));
      ::SetTextColor(dc, colors.on_accent);
      ::DrawTextW(dc, button_.c_str(), -1, &button,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
    }
  }

  void OnHover(int, int) override { Invalidate(); }

  void OnClick(int hit, POINT) override {
    if (hit < 0 || hit >= static_cast<int>(hits().size())) {
      return;
    }
    // Every action below may hide this window; none destroys it.
    switch (hits()[hit].key) {
      case kHitPrimary:
        cards_->Decide(index_, "accept");
        break;
      case kHitClose:
        cards_->Decide(index_, "reject");
        break;
      case kHitHide:
        cards_->HideStack();
        break;
      case kHitPrevious:
        cards_->Page(-1);
        break;
      case kHitNext:
        cards_->Page(1);
        break;
      default:
        break;
    }
  }

  void OnDpiChanged(UINT) override {
    Layout();
    Place();
  }

  // The app palette as it is now, so a theme or appearance change while a
  // card is up repaints it in the new colours.
  PhiPalette QueryPalette() const override {
    return cards_->palette_ ? cards_->palette_() : PhiPopover::QueryPalette();
  }

 private:
  void DrawGlyph(HDC dc, int key, const wchar_t* glyph) {
    if (const PhiHit* hit = HitByKey(key)) {
      RECT r = PhiToRect(hit->bounds);
      PhiDrawTextAA(dc, glyph, 1, &r,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
  }

  // Measures the card at the window's dpi and rebuilds the hit list.
  void Layout() {
    const int width = Scaled(kCardWidth);
    const int pad_x = Scaled(kPadX);
    const int pad_y = Scaled(kPadY);
    const int icon = Scaled(kIconSize);
    int y = pad_y;

    ClearHits();
    // Header: the title, then hide and close at the trailing edge.
    RECT close = {width - pad_x - icon, y, width - pad_x, y + icon};
    RECT hide = {close.left - Scaled(kIconGap) - icon, y,
                 close.left - Scaled(kIconGap), y + icon};
    title_rect_ = {pad_x, y, hide.left - Scaled(kIconGap), y + icon};
    y += icon + Scaled(kTitleGap);

    // The message, wrapped, at most three lines.
    HDC probe = ::GetDC(hwnd());
    HGDIOBJ previous =
        ::SelectObject(probe, fonts().Get(kMessageSize, FW_NORMAL));
    TEXTMETRICW metrics = {};
    ::GetTextMetricsW(probe, &metrics);
    RECT measure = {pad_x, y, width - pad_x, y};
    PhiDrawTextAA(probe, message_.c_str(), -1, &measure,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL |
                      DT_NOPREFIX | DT_CALCRECT);
    ::SelectObject(probe, previous);
    ::ReleaseDC(hwnd(), probe);
    const int line = std::max<int>(1, metrics.tmHeight);
    const int message_height = std::min<int>(
        measure.bottom - measure.top, line * kMessageMaxLines);
    message_rect_ = {pad_x, y, width - pad_x, y + message_height};
    y += message_height + Scaled(kRowGap);

    // The button row: ‹ [button] › — paging only with more than one card.
    const int button_h = Scaled(kButtonHeight);
    RECT button = {pad_x, y, width - pad_x, y + button_h};
    const bool paged = count_ > 1;
    RECT prev = {}, next = {};
    if (paged) {
      prev = {pad_x, y, pad_x + Scaled(kPageWidth), y + button_h};
      next = {width - pad_x - Scaled(kPageWidth), y, width - pad_x,
              y + button_h};
      button.left = prev.right + Scaled(kIconGap) * 2;
      button.right = next.left - Scaled(kIconGap) * 2;
    }
    y += button_h + pad_y;

    AddHit(button, kHitPrimary, button_, PhiHitRole::kButton);
    if (paged) {
      AddHit(prev, kHitPrevious, PhiStr("notifications.card.previous"),
             PhiHitRole::kButton);
      AddHit(next, kHitNext, PhiStr("notifications.card.next"),
             PhiHitRole::kButton);
    }
    AddHit(hide, kHitHide, PhiStr("notifications.card.hide"),
           PhiHitRole::kButton);
    AddHit(close, kHitClose, PhiStr("notifications.card.close"),
           PhiHitRole::kButton);
    size_ = {width, y};
  }

  // Back-pointer to the manager that owns this view and outlives it.
  RAW_PTR_EXCLUSION PhiNotificationCards* cards_;
  std::wstring title_;
  std::wstring message_;
  std::wstring button_;
  size_t index_ = 0;
  size_t count_ = 0;
  RECT title_rect_ = {};
  RECT message_rect_ = {};
  SIZE size_ = {1, 1};
};

PhiNotificationCards::PhiNotificationCards(DecisionSink sink,
                                           OwnerProvider owner,
                                           PaletteProvider palette,
                                           ActionSink action)
    : sink_(std::move(sink)),
      owner_(std::move(owner)),
      palette_(std::move(palette)),
      action_(std::move(action)) {
  g_cards = this;
}

PhiNotificationCards::~PhiNotificationCards() {
  if (g_cards == this) {
    g_cards = nullptr;
  }
  if (expiry_timer_) {
    ::KillTimer(nullptr, expiry_timer_);
  }
  if (view_) {
    view_->Destroy();
  }
}

std::string PhiNotificationCards::Show(const Request& request,
                                       const std::string& capability_id) {
  Card card;
  card.id = "agent-notification:" + store::NewUppercaseUuid();
  card.title = base::UTF8ToWide(request.title);
  card.message = base::UTF8ToWide(request.message);
  card.button_title = base::UTF8ToWide(
      request.button_title.empty() ? std::string("Run")
                                   : request.button_title);
  card.expires_at_ms =
      ::GetTickCount64() +
      static_cast<uint64_t>(request.expires_in_seconds) * 1000;
  card.capability_id = capability_id;
  const std::string id = card.id;
  // A full queue evicts its oldest card, which answers timeout.
  while (cards_.size() >= kMaxCards) {
    Answer(cards_.size() - 1, "timeout");
  }
  cards_.insert(cards_.begin(), std::move(card));
  current_ = 0;
  hidden_ = false;  // A new card shows a stack the user hid.
  LOG(INFO) << "[PhiClient] notification card " << id << " queued ("
            << cards_.size() << " waiting)";
  ArmExpiry();
  Present(/*allow_recreate=*/true);
  return id;
}

void PhiNotificationCards::Refresh() {
  Present(/*allow_recreate=*/true);
}

// static
void PhiNotificationCards::NoteOwnerMoved(HWND owner) {
  if (g_cards && g_cards->view_ && g_cards->view_->alive() &&
      g_cards->view_->owner() == owner) {
    g_cards->view_->Place();
  }
}

void PhiNotificationCards::Decide(size_t index, const char* decision) {
  if (index < cards_.size()) {
    // The view's two buttons: the Mac's onRun / onDelete.
    NoteAction(std::string_view(decision) == "accept" ? "run" : "delete");
    Answer(index, decision);
    ArmExpiry();
    Present(/*allow_recreate=*/false);
  }
}

void PhiNotificationCards::HideStack() {
  NoteAction("dismiss");  // The Mac's onDismiss.
  hidden_ = true;
  Present(/*allow_recreate=*/false);
}

void PhiNotificationCards::NoteAction(const char* action) {
  if (action_) {
    action_(action);
  }
}

void PhiNotificationCards::Page(int delta) {
  if (cards_.size() < 2) {
    return;
  }
  const int count = static_cast<int>(cards_.size());
  current_ = static_cast<size_t>(
      (static_cast<int>(current_) + delta + count) % count);
  Present(/*allow_recreate=*/false);
}

void PhiNotificationCards::Answer(size_t index, const char* decision) {
  Card card = std::move(cards_[index]);
  cards_.erase(cards_.begin() + static_cast<std::ptrdiff_t>(index));
  if (current_ >= cards_.size() && current_ > 0) {
    current_ = cards_.size() - 1;
  }
  if (cards_.empty()) {
    hidden_ = false;
  }
  LOG(INFO) << "[PhiClient] notification card " << card.id << ": "
            << decision;
  base::DictValue payload;
  payload.Set("notificationId", card.id);
  payload.Set("decision", decision);
  std::string json;
  base::JSONWriter::Write(payload, &json);
  if (sink_) {
    sink_(card.capability_id, json);
  }
}

void PhiNotificationCards::Present(bool allow_recreate) {
  if (cards_.empty() || hidden_) {
    if (view_) {
      view_->Hide();
    }
    shown_id_.clear();
    return;
  }
  const HWND owner = owner_ ? owner_() : nullptr;
  if (allow_recreate && view_ &&
      (!view_->alive() || (owner && view_->owner() != owner))) {
    // The user is in another window now (or ours went away): the stack
    // moves there. Never on the view's own click path.
    view_->Destroy();
    view_.reset();
  }
  if (!view_) {
    if (!owner) {
      shown_id_.clear();
      return;  // No user window: the cards wait for one.
    }
    auto view = std::make_unique<PhiNotificationCardView>(this);
    if (!view->Open(owner, palette_ ? palette_() : PhiPalette())) {
      shown_id_.clear();
      return;
    }
    view_ = std::move(view);
  }
  if (!view_->alive()) {
    shown_id_.clear();
    return;
  }
  const Card& card = cards_[current_];
  view_->Render(card, current_, cards_.size());
  if (card.id != shown_id_) {
    shown_id_ = card.id;
    NoteAction("show");
  }
}

void PhiNotificationCards::ArmExpiry() {
  if (expiry_timer_) {
    ::KillTimer(nullptr, expiry_timer_);
    expiry_timer_ = 0;
  }
  if (cards_.empty()) {
    return;
  }
  uint64_t soonest = cards_.front().expires_at_ms;
  for (const Card& card : cards_) {
    soonest = std::min(soonest, card.expires_at_ms);
  }
  const uint64_t now = ::GetTickCount64();
  const UINT delay = soonest > now
                         ? static_cast<UINT>(std::min<uint64_t>(
                               soonest - now, USER_TIMER_MAXIMUM))
                         : USER_TIMER_MINIMUM;
  expiry_timer_ = ::SetTimer(nullptr, 0, delay, &ExpiryTimerProc);
}

void PhiNotificationCards::ExpireDue() {
  const uint64_t now = ::GetTickCount64();
  bool changed = false;
  for (size_t i = cards_.size(); i-- > 0;) {
    if (cards_[i].expires_at_ms <= now) {
      Answer(i, "timeout");
      changed = true;
    }
  }
  ArmExpiry();
  if (changed) {
    Present(/*allow_recreate=*/false);
  }
}

// static
void CALLBACK PhiNotificationCards::ExpiryTimerProc(HWND,
                                                    UINT,
                                                    UINT_PTR id,
                                                    DWORD) {
  if (g_cards && g_cards->expiry_timer_ == id) {
    ::KillTimer(nullptr, id);
    g_cards->expiry_timer_ = 0;
    g_cards->ExpireDue();
  }
}

}  // namespace phi_client

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_toast.h"

#include <oleauto.h>
#include <uiautomation.h>

#include <algorithm>
#include <functional>
#include <string>
#include <utility>

#include "phibrowser-win/client/phi_animator.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_popover.h"

namespace phi_client {

namespace {

// The fade the toast used to hand-roll with a 30 ms SetTimer and a
// 32-per-step alpha ramp; the same total durations, now on the shared
// animator's one coalescable 16 ms clock (lane 3 §6.3).
constexpr int kFadeInMs = 240;
constexpr int kFadeOutMs = 210;

// OverlayToastCenter's metrics, in DIP.
constexpr int kCornerRadius = 12;
constexpr int kTextInset = 16;
constexpr int kSideInset = 48;
constexpr int kHeightOneLine = 40;
constexpr int kHeightTwoLine = 56;
constexpr int kTopOffset = 56;
constexpr int kTitleSize = 13;
constexpr int kMessageSize = 12;
// A long title or message wraps inside this width (and inside the owner,
// less a margin each side) instead of stretching the toast off the window;
// past a few lines it ends in an ellipsis.
constexpr int kMaxWidth = 440;
constexpr int kOwnerMargin = 24;
constexpr int kTitleMaxLines = 2;
constexpr int kMessageMaxLines = 3;
constexpr int kTextInsetY = 10;
constexpr int kLineGap = 2;
// The action button (OverlayToastAction): a pill at the trailing edge.
constexpr int kActionHeight = 26;
constexpr int kActionPadX = 12;
constexpr int kActionGap = 12;
constexpr int kActionRadius = 6;
constexpr int kActionKey = 1;
constexpr UINT kWrapFlags =
    DT_CENTER | DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS | DT_NOPREFIX;


// The Mac queues toasts; replacement is still the simpler v1, so exactly
// one lives at a time.
class Toast;
Toast* g_toast = nullptr;

class Toast : public PhiPopover {
 public:
  Toast(std::wstring title,
        std::wstring message,
        const PhiPalette& palette,
        std::wstring action_label,
        std::function<void()> action)
      : title_(std::move(title)),
        message_(std::move(message)),
        pending_palette_(palette),
        action_label_(std::move(action_label)),
        action_(std::move(action)) {}

  ~Toast() override = default;

  bool Open(HWND owner, int duration_ms) {
    ClassSpec spec;
    spec.name = L"PhiToast";
    spec.cursor = nullptr;

    Options options;
    options.activate = false;
    options.foreground = false;
    options.topmost = true;
    options.layered = true;
    options.dwm_round = false;  // PhiFillRoundRectAA draws the corners.
    options.dismiss_on_inactive = false;
    options.dismiss_on_escape = false;
    // Clicks belong to whatever is under it — unless there is a button.
    options.click_through = !has_action();

    // Created off-screen at a provisional size: the text has to be
    // measured with fonts() at the window's own dpi, which only exists
    // once the window does. Nothing is visible until Show().
    Placement placement;
    placement.side = PhiPopoverSide::kExplicit;
    placement.clamp = false;
    SIZE probe = {1, 1};
    if (!CreatePopover(spec, owner, probe, placement, options)) {
      return false;
    }
    g_toast = this;  // Before Show(), so a teardown cannot leave us stale.
    SetPalette(pending_palette_);
    ::SetLayeredWindowAttributes(hwnd(), 0, 0, LWA_ALPHA);

    const SIZE size = Measure();
    RECT owner_rect = {};
    ::GetWindowRect(owner, &owner_rect);
    const LONG x = (owner_rect.left + owner_rect.right) / 2 - size.cx / 2;
    const LONG y = owner_rect.top + Scaled(kTopOffset);
    placement.anchor = {x, y, x, y};
    Reposition(placement, size);
    LayoutAction();
    Show();
    // Raised on the owner, the window the reader is in: the toast itself
    // is click-through and never focused.
    PhiAnnounceToScreenReaders(
        owner, message_.empty() ? title_ : title_ + L"\n" + message_);

    PhiAnimator::ForThread().Animate(
        hwnd(), kFadeInMs, PhiEasing::kLinear,
        [this](double t, RECT*) { SetAlpha(static_cast<BYTE>(t * 255.0)); });
    dismiss_ = NewTimer();
    dismiss_.Start(static_cast<UINT>(std::max(500, duration_ms)));
    return true;
  }

 protected:
  void Paint(HDC dc, const RECT&) override {
    RECT rc = {};
    ::GetClientRect(hwnd(), &rc);
    const PhiPalette& colors = palette();
    PhiFillRoundRectAA(dc, rc, Scaled(kCornerRadius), colors.card);
    PhiStrokeRoundRectAA(dc, rc, Scaled(kCornerRadius), colors.border, 1);
    ::SetBkMode(dc, TRANSPARENT);
    // The title (and the message under it) as measured, wrapped, the block
    // centred vertically.
    RECT text = rc;
    text.left += Scaled(kTextInset);
    text.right -= Scaled(kTextInset);
    if (has_action()) {
      text.right = action_rect_.left - Scaled(kActionGap);
      const bool hot = hovered() >= 0;
      PhiFillRoundRectAA(dc, action_rect_, Scaled(kActionRadius),
                         hot ? colors.hover_strong : colors.hover);
      ::SelectObject(dc, TitleFont());
      ::SetTextColor(dc, colors.accent);
      RECT label = action_rect_;
      ::DrawTextW(dc, action_label_.c_str(), -1, &label,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    const int block =
        title_h_ + (message_.empty() ? 0 : Scaled(kLineGap) + message_h_);
    RECT top = text;
    top.top = (rc.top + rc.bottom - block) / 2;
    top.bottom = top.top + title_h_;
    ::SelectObject(dc, TitleFont());
    ::SetTextColor(dc, colors.label);
    PhiDrawTextAA(dc, title_.c_str(), -1, &top, kWrapFlags);
    if (message_.empty()) {
      return;
    }
    RECT bottom = text;
    bottom.top = top.bottom + Scaled(kLineGap);
    bottom.bottom = bottom.top + message_h_;
    ::SelectObject(dc, MessageFont());
    ::SetTextColor(dc, colors.label_dim);
    PhiDrawTextAA(dc, message_.c_str(), -1, &bottom, kWrapFlags);
  }

  void OnHover(int, int) override { ::InvalidateRect(hwnd(), nullptr, FALSE); }

  void OnClick(int hit, POINT) override {
    if (hit < 0 || !action_ || fading_out_) {
      return;
    }
    std::function<void()> action = std::move(action_);
    action_ = nullptr;
    Destroy();  // First: the action may open a modal prompt.
    action();
  }

  void OnTimerFired(UINT_PTR id) override {
    if (id != dismiss_.id() || fading_out_) {
      return;
    }
    fading_out_ = true;
    dismiss_.Stop();
    PhiAnimator::ForThread().Animate(
        hwnd(), kFadeOutMs, PhiEasing::kLinear,
        [this](double t, RECT*) {
          SetAlpha(static_cast<BYTE>((1.0 - t) * 255.0));
        },
        [this] { Destroy(); });
  }

  void OnDpiChanged(UINT) override {
    const SIZE size = Measure();
    ::SetWindowPos(hwnd(), nullptr, 0, 0, size.cx, size.cy,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    LayoutAction();
  }

  void OnNcDestroy() override {
    if (g_toast == this) {
      g_toast = nullptr;
    }
  }

 private:
  bool has_action() const { return !action_label_.empty() && action_; }

  // The button's rect, measured from its label, and its one hit.
  int ActionWidth() {
    if (!has_action()) {
      return 0;
    }
    HDC probe = ::GetDC(nullptr);
    SIZE extent = {};
    if (probe) {
      HGDIOBJ prev = ::SelectObject(probe, TitleFont());
      ::GetTextExtentPoint32W(probe, action_label_.c_str(),
                              static_cast<int>(action_label_.size()), &extent);
      ::SelectObject(probe, prev);
      ::ReleaseDC(nullptr, probe);
    }
    return static_cast<int>(extent.cx) + 2 * Scaled(kActionPadX);
  }

  void LayoutAction() {
    ClearHits();
    if (!has_action()) {
      return;
    }
    RECT rc = {};
    ::GetClientRect(hwnd(), &rc);
    const int width = action_width_;
    const int height = Scaled(kActionHeight);
    action_rect_ = {rc.right - Scaled(kTextInset) - width,
                    (rc.top + rc.bottom - height) / 2,
                    rc.right - Scaled(kTextInset),
                    (rc.top + rc.bottom + height) / 2};
    AddHit(action_rect_, kActionKey, action_label_, PhiHitRole::kButton);
  }

  HFONT TitleFont() { return fonts().Get(kTitleSize, FW_SEMIBOLD); }
  HFONT MessageFont() { return fonts().Get(kMessageSize, FW_NORMAL); }

  void SetAlpha(BYTE alpha) {
    if (alive()) {
      ::SetLayeredWindowAttributes(hwnd(), 0, alpha, LWA_ALPHA);
    }
  }

  // The widest the text may run: kMaxWidth, and never past the owner.
  int MaxTextWidth() const {
    int width = Scaled(kMaxWidth);
    RECT owner_rect = {};
    if (owner() && ::GetWindowRect(owner(), &owner_rect)) {
      width = std::min(width, static_cast<int>(owner_rect.right -
                                               owner_rect.left) -
                                  2 * Scaled(kOwnerMargin));
    }
    const int action = has_action() ? action_width_ + Scaled(kActionGap) : 0;
    return std::max(width - Scaled(kSideInset) - action, Scaled(80));
  }

  // One block's wrapped extent at `max_width`, at most `max_lines` tall.
  static SIZE MeasureBlock(HDC dc,
                           const std::wstring& text,
                           int max_width,
                           int max_lines) {
    TEXTMETRICW metrics = {};
    ::GetTextMetricsW(dc, &metrics);
    RECT bounds = {0, 0, max_width, 0};
    PhiDrawTextAA(dc, text.c_str(), -1, &bounds,
                  DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
    SIZE size = {};
    size.cx = std::min<LONG>(bounds.right - bounds.left, max_width);
    size.cy = std::min<LONG>(bounds.bottom - bounds.top,
                             metrics.tmHeight * max_lines);
    return size;
  }

  // Size to the text, the Mac's topCenter slot; long text wraps.
  SIZE Measure() {
    action_width_ = ActionWidth();
    const int max_width = MaxTextWidth();
    HDC probe = ::GetDC(nullptr);
    SIZE title_size = {};
    SIZE message_size = {};
    if (probe) {
      HGDIOBJ prev = ::SelectObject(probe, TitleFont());
      title_size = MeasureBlock(probe, title_, max_width, kTitleMaxLines);
      if (!message_.empty()) {
        ::SelectObject(probe, MessageFont());
        message_size =
            MeasureBlock(probe, message_, max_width, kMessageMaxLines);
      }
      ::SelectObject(probe, prev);
      ::ReleaseDC(nullptr, probe);
    }
    title_h_ = static_cast<int>(title_size.cy);
    message_h_ = static_cast<int>(message_size.cy);
    SIZE size = {};
    size.cx = std::max<LONG>(std::max(title_size.cx, message_size.cx), 1) +
              Scaled(kSideInset) +
              (has_action() ? action_width_ + Scaled(kActionGap) : 0);
    // One- and two-line toasts keep their fixed heights; wrapped text
    // grows the toast.
    const int block =
        title_h_ + (message_.empty() ? 0 : Scaled(kLineGap) + message_h_);
    size.cy = std::max(
        Scaled(message_.empty() ? kHeightOneLine : kHeightTwoLine),
        block + 2 * Scaled(kTextInsetY));
    return size;
  }

  std::wstring title_;
  std::wstring message_;
  // Measured block heights (Measure), laid out by Paint.
  int title_h_ = 0;
  int message_h_ = 0;
  PhiPalette pending_palette_;
  std::wstring action_label_;
  std::function<void()> action_;
  RECT action_rect_ = {};
  int action_width_ = 0;  // Measured with the label font (Measure).
  Timer dismiss_;
  bool fading_out_ = false;
};

}  // namespace

// Screen readers learn of a toast or a page through a UIA notification
// (UiaRaiseNotificationEvent, Windows 10 1709+). uiautomationcore.dll is
// resolved by hand, as phi_uia.cc does: the framework's link recipe carries
// no uiautomationcore.lib, and an older system just stays silent.
void PhiAnnounceToScreenReaders(HWND hwnd, const std::wstring& text) {
  using HostFromHwnd = decltype(&::UiaHostProviderFromHwnd);
  using ClientsListening = decltype(&::UiaClientsAreListening);
  using RaiseNotification = decltype(&::UiaRaiseNotificationEvent);
  struct Api {
    HostFromHwnd host_from_hwnd = nullptr;
    ClientsListening clients_listening = nullptr;
    RaiseNotification raise = nullptr;
  };
  static const Api api = [] {
    Api loaded;
    HMODULE core = ::LoadLibraryExW(L"uiautomationcore.dll", nullptr,
                                    LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!core) {
      return loaded;
    }
    loaded.host_from_hwnd = reinterpret_cast<HostFromHwnd>(
        ::GetProcAddress(core, "UiaHostProviderFromHwnd"));
    loaded.clients_listening = reinterpret_cast<ClientsListening>(
        ::GetProcAddress(core, "UiaClientsAreListening"));
    loaded.raise = reinterpret_cast<RaiseNotification>(
        ::GetProcAddress(core, "UiaRaiseNotificationEvent"));
    return loaded;
  }();
  if (!api.host_from_hwnd || !api.clients_listening || !api.raise ||
      !hwnd || text.empty() || !api.clients_listening()) {
    return;
  }
  IRawElementProviderSimple* provider = nullptr;
  if (FAILED(api.host_from_hwnd(hwnd, &provider)) || !provider) {
    return;
  }
  BSTR display =
      ::SysAllocStringLen(text.c_str(), static_cast<UINT>(text.size()));
  BSTR activity = ::SysAllocString(L"PhiToast");
  if (display && activity) {
    // A newer toast replaces the one before it, so the reader may drop a
    // superseded one.
    api.raise(provider, NotificationKind_Other,
              NotificationProcessing_ImportantMostRecent, display, activity);
  }
  ::SysFreeString(display);
  ::SysFreeString(activity);
  provider->Release();
}

void PhiShowToast(HWND owner,
                  const PhiPalette& palette,
                  const std::wstring& title,
                  const std::wstring& message,
                  int duration_ms) {
  PhiShowToastWithAction(owner, palette, title, message, duration_ms,
                         std::wstring(), nullptr);
}

void PhiShowToastWithAction(HWND owner,
                            const PhiPalette& palette,
                            const std::wstring& title,
                            const std::wstring& message,
                            int duration_ms,
                            const std::wstring& action_label,
                            std::function<void()> action) {
  if (!owner || !::IsWindow(owner)) {
    return;
  }
  if (g_toast) {
    g_toast->Destroy();  // Replacement, not queueing (v1).
    g_toast = nullptr;
  }
  Toast* toast =
      new Toast(title, message, palette, action_label, std::move(action));
  toast->DeleteWhenDestroyed();
  if (!toast->Open(owner, duration_ms)) {
    delete toast;
  }
}

}  // namespace phi_client

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_window.h"

#include <algorithm>

namespace phi_client {

// ---------------------------------------------------------------------------
// Pure — compiled on the Linux host too (test/model/ui_substrate_test.cc).

PhiRectI PhiRectUnion(const PhiRectI& a, const PhiRectI& b) {
  if (a.empty()) {
    return b;
  }
  if (b.empty()) {
    return a;
  }
  PhiRectI out;
  out.left = std::min(a.left, b.left);
  out.top = std::min(a.top, b.top);
  out.right = std::max(a.right, b.right);
  out.bottom = std::max(a.bottom, b.bottom);
  return out;
}

int PhiHitAt(const std::vector<PhiHit>& hits, int x, int y) {
  // Last match wins: hits go in in paint order, so the rect drawn on top
  // takes the pointer.
  for (size_t i = hits.size(); i > 0; --i) {
    const PhiHit& hit = hits[i - 1];
    if (hit.enabled && hit.bounds.Contains(x, y)) {
      return static_cast<int>(i - 1);
    }
  }
  return -1;
}

PhiRectI PhiPlacePopover(const PhiRectI& anchor,
                         int width,
                         int height,
                         const PhiRectI& work,
                         const PhiPlacementSpec& spec) {
  int x = anchor.left;
  int y = anchor.top;

  switch (spec.side) {
    case PhiPopoverSide::kExplicit:
      break;

    case PhiPopoverSide::kRightTop: {
      x = anchor.right + spec.gap;
      y = anchor.top;
      const int flipped = anchor.left - width - spec.gap;
      if (spec.flip && x + width > work.right - spec.margin &&
          (!spec.flip_only_if_fits || flipped >= work.left + spec.margin)) {
        x = flipped;
      }
      break;
    }

    case PhiPopoverSide::kBelow:
    case PhiPopoverSide::kAbove: {
      switch (spec.align) {
        case PhiPopoverAlign::kLeading:
          x = anchor.left;
          break;
        case PhiPopoverAlign::kCenter:
          x = (anchor.left + anchor.right) / 2 - width / 2;
          break;
        case PhiPopoverAlign::kCenterIfWider:
          x = anchor.width() > width
                  ? anchor.left + (anchor.width() - width) / 2
                  : anchor.left;
          break;
      }
      const bool below = spec.side == PhiPopoverSide::kBelow;
      const int under = anchor.bottom + spec.gap;
      const int over = anchor.top - height - spec.gap;
      y = below ? under : over;
      if (spec.flip) {
        if (below && y + height > work.bottom - spec.margin) {
          if (!spec.flip_only_if_fits || over >= work.top + spec.margin) {
            y = over;
          }
        } else if (!below && y < work.top + spec.margin) {
          if (!spec.flip_only_if_fits ||
              under + height <= work.bottom - spec.margin) {
            y = under;
          }
        }
      }
      break;
    }
  }

  if (spec.clamp) {
    const int min_x = work.left + spec.margin;
    const int min_y = work.top + spec.margin;
    const int max_x = std::max(min_x, work.right - spec.margin - width);
    const int max_y = std::max(min_y, work.bottom - spec.margin - height);
    x = std::min(std::max(x, min_x), max_x);
    y = std::min(std::max(y, min_y), max_y);
  }

  PhiRectI out;
  out.left = x;
  out.top = y;
  out.right = x + width;
  out.bottom = y + height;
  return out;
}

PhiTimerPool::PhiTimerPool() = default;

PhiTimerPool::~PhiTimerPool() = default;

unsigned PhiTimerPool::Alloc() {
  for (size_t i = 0; i < used_.size(); ++i) {
    if (!used_[i]) {
      used_[i] = true;
      return kPhiTimerIdBase + static_cast<unsigned>(i);
    }
  }
  used_.push_back(true);
  return kPhiTimerIdBase + static_cast<unsigned>(used_.size() - 1);
}

void PhiTimerPool::Release(unsigned id) {
  if (id < kPhiTimerIdBase) {
    return;
  }
  const size_t index = id - kPhiTimerIdBase;
  if (index < used_.size()) {
    used_[index] = false;
  }
}

bool PhiTimerPool::InUse(unsigned id) const {
  if (id < kPhiTimerIdBase) {
    return false;
  }
  const size_t index = id - kPhiTimerIdBase;
  return index < used_.size() && used_[index];
}

size_t PhiTimerPool::live() const {
  size_t count = 0;
  for (size_t i = 0; i < used_.size(); ++i) {
    if (used_[i]) {
      ++count;
    }
  }
  return count;
}

double PhiEase(PhiEasing easing, double t) {
  t = std::clamp(t, 0.0, 1.0);
  switch (easing) {
    case PhiEasing::kLinear:
      return t;
    case PhiEasing::kEaseIn:
      return t * t;
    case PhiEasing::kEaseOut:
      return 1.0 - (1.0 - t) * (1.0 - t);
    case PhiEasing::kEaseInOut:
      return t < 0.5 ? 2.0 * t * t : 1.0 - 2.0 * (1.0 - t) * (1.0 - t);
  }
  return t;
}

int PhiLerpInt(int from, int to, double t) {
  return from + static_cast<int>((to - from) * t + (to >= from ? 0.5 : -0.5));
}

}  // namespace phi_client

#if defined(_WIN32)

#include <dwmapi.h>
#include <windowsx.h>

#include <map>
#include <utility>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/no_destructor.h"
#include "phibrowser-win/client/phi_animator.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_theme_bus.h"
#include "phibrowser-win/client/phi_uia.h"

namespace phi_client {

namespace {

// THE class table (lane 3 §1: 33 registration blocks in four idioms, none
// of which checks ERROR_CLASS_ALREADY_EXISTS). Keyed by class name, so the
// name is spelled once — in the surface's ClassSpec — and two objects of
// one class register once between them.
std::map<std::wstring, bool>& ClassRegistry() {
  static base::NoDestructor<std::map<std::wstring, bool>> registry;
  return *registry;
}

void EnsureClassRegistered(const PhiWindow::ClassSpec& spec, WNDPROC proc) {
  std::map<std::wstring, bool>& registry = ClassRegistry();
  if (registry.find(spec.name) != registry.end()) {
    return;
  }
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = spec.style;
  wc.lpfnWndProc = proc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.hCursor = spec.cursor ? ::LoadCursorW(nullptr, spec.cursor) : nullptr;
  wc.hIcon = spec.icon;
  wc.hIconSm = spec.icon_small;
  wc.lpszClassName = spec.name;
  // No hbrBackground: every PhiWindow answers WM_ERASEBKGND itself, and a
  // class brush plus an erase-eating proc is the contradiction Settings
  // carries today (lane 3 §3.3).
  if (::RegisterClassExW(&wc) == 0 &&
      ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return;  // Leave it unregistered; CreateWindowExW will fail visibly.
  }
  registry[spec.name] = true;
}

// ---------------------------------------------------------------------------
// THE tooltip (lane 3 §2 "Tooltips": a grep for TOOLTIPS_CLASS / TTM_ over
// the whole tree comes back empty, so the nav buttons and every icon-only
// control have no text alternative at all; §7 item 15).
//
// It is driven by hits_, the list every surface already keeps: a hit's
// `name` is its accessible name AND its tip, so a surface cannot label one
// and forget the other, and no surface needs a TOOLTIPS_CLASS, a TTM_
// message or a second rect list. One window per UI thread, re-created when
// the owning surface changes (the owner is what fixes the dpi and the
// palette). Opt-in per surface — PhiWindow::EnableTooltips — which the
// PhiPopover and PhiPanel bases opt every surface of theirs into.
constexpr int kTipPaddingX = 8;      // Design units.
constexpr int kTipPaddingY = 5;
constexpr int kTipMaxWidth = 320;
constexpr int kTipCursorDx = 14;
constexpr int kTipCursorDy = 22;
constexpr int kTipScreenMargin = 4;
constexpr int kTipFontSize = 12;

class PhiTipWindow final : public PhiWindow {
 public:
  void ShowFor(PhiWindow* owner, const std::wstring& text);
  // The same for an owner that is not a PhiWindow (PhiShowTooltipFor):
  // `key` identifies it, `owner_hwnd` fixes the dpi.
  void ShowFor(const void* key,
               HWND owner_hwnd,
               const PhiPalette& palette,
               const std::wstring& text);
  void Hide();
  // The owner is going away: drop the window with it (Windows destroys an
  // owned popup anyway) so the next ShowFor() re-creates at the new dpi.
  void Forget();
  const void* owner_window() const { return owner_window_; }

 protected:
  void Paint(HDC dc, const RECT& dirty) override;

 private:
  SIZE Measure();

  // Compared, never dereferenced after Forget().
  RAW_PTR_EXCLUSION const void* owner_window_ = nullptr;
  std::wstring text_;
  bool dwm_rounded_ = false;
};

SIZE PhiTipWindow::Measure() {
  SIZE size = {0, 0};
  HDC screen = ::GetDC(nullptr);
  if (!screen) {
    return size;
  }
  HGDIOBJ old_font =
      ::SelectObject(screen, fonts().Get(kTipFontSize, FW_NORMAL));
  RECT text = {0, 0, Scaled(kTipMaxWidth), 0};
  PhiDrawTextAA(screen, text_.c_str(), -1, &text,
                DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  ::SelectObject(screen, old_font);
  ::ReleaseDC(nullptr, screen);
  size.cx = text.right - text.left + Scaled(kTipPaddingX) * 2;
  size.cy = text.bottom - text.top + Scaled(kTipPaddingY) * 2;
  return size;
}

void PhiTipWindow::ShowFor(PhiWindow* owner, const std::wstring& text) {
  if (!owner) {
    Hide();
    return;
  }
  ShowFor(owner, owner->hwnd(), owner->palette(), text);
}

void PhiTipWindow::ShowFor(const void* key,
                           HWND owner_hwnd,
                           const PhiPalette& owner_palette,
                           const std::wstring& text) {
  if (!key || !owner_hwnd || text.empty()) {
    Hide();
    return;
  }
  // The tip is an owned popup of `owner_hwnd`, and showing an owned window
  // pulls its owner above any unrelated top-level that was in front of it.
  // A hover tip armed while the pointer rested on a background window (the
  // shell's chrome behind the Send feedback form or Settings) would yank
  // that window over the one the user is looking at, which read as the
  // front window closing by itself. A background window shows no tip.
  if (::GetForegroundWindow() != owner_hwnd) {
    Hide();
    return;
  }
  if (alive() && owner_window_ != key) {
    Destroy();  // A different owner means a different dpi and palette.
  }
  owner_window_ = key;
  text_ = text;
  if (!alive()) {
    ClassSpec spec;
    spec.name = L"PhiTooltip";
    const RECT origin = {0, 0, 0, 0};
    if (!Create(spec,
                WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST |
                    WS_EX_TRANSPARENT,
                WS_POPUP, origin, owner_hwnd)) {
      owner_window_ = nullptr;
      return;
    }
    // DWM rounds the corners; nothing here draws a curve, so the AA rule
    // (memory: phi-win-antialiasing) is not in play.
    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUNDSMALL;
    dwm_rounded_ = SUCCEEDED(::DwmSetWindowAttribute(
        hwnd(), DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner)));
  }
  SetPalette(owner_palette);
  const SIZE size = Measure();
  POINT cursor = {0, 0};
  ::GetCursorPos(&cursor);
  MONITORINFO monitor = {};
  monitor.cbSize = sizeof(monitor);
  ::GetMonitorInfoW(::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST),
                    &monitor);
  PhiRectI at;
  at.left = static_cast<int>(cursor.x) + Scaled(kTipCursorDx);
  at.top = static_cast<int>(cursor.y) + Scaled(kTipCursorDy);
  at.right = at.left;
  at.bottom = at.top;
  PhiPlacementSpec placement;
  placement.side = PhiPopoverSide::kExplicit;
  placement.margin = Scaled(kTipScreenMargin);
  const PhiRectI target =
      PhiPlacePopover(at, size.cx, size.cy, PhiFromRect(monitor.rcWork),
                      placement);
  ::SetWindowPos(hwnd(), HWND_TOPMOST, target.left, target.top,
                 target.width(), target.height(), SWP_NOACTIVATE);
  ::ShowWindow(hwnd(), SW_SHOWNOACTIVATE);
  Invalidate();
}

void PhiTipWindow::Hide() {
  if (alive()) {
    ::ShowWindow(hwnd(), SW_HIDE);
  }
}

void PhiTipWindow::Forget() {
  owner_window_ = nullptr;
  text_.clear();
  if (alive()) {
    Destroy();
  }
}

void PhiTipWindow::Paint(HDC dc, const RECT&) {
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  const PhiPalette& colors = palette();
  if (HBRUSH fill = ::CreateSolidBrush(colors.window_bg)) {
    ::FillRect(dc, &client, fill);
    ::DeleteObject(fill);
  }
  // Windows 10 only: on 11 DWM draws the border along its rounded corners,
  // and a square frame inside would cut across them.
  HBRUSH edge = dwm_rounded_ ? nullptr : ::CreateSolidBrush(colors.border);
  if (edge) {
    ::FrameRect(dc, &client, edge);
    ::DeleteObject(edge);
  }
  RECT text = client;
  ::InflateRect(&text, -Scaled(kTipPaddingX), -Scaled(kTipPaddingY));
  HGDIOBJ old_font = ::SelectObject(dc, fonts().Get(kTipFontSize, FW_NORMAL));
  const int old_mode = ::SetBkMode(dc, TRANSPARENT);
  const COLORREF old_color = ::SetTextColor(dc, colors.label);
  PhiDrawTextAA(dc, text_.c_str(), -1, &text,
                DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
  ::SetTextColor(dc, old_color);
  ::SetBkMode(dc, old_mode);
  ::SelectObject(dc, old_font);
}

// Deliberately leaked, like the class registry above: there is one per UI
// thread, it outlives every surface, and an exit-time destructor is a style
// violation (-Wexit-time-destructors). Null until a surface with tooltips
// enabled actually dwells on a named hit.
PhiTipWindow*& TipSlot() {
  static PhiTipWindow* tip = nullptr;
  return tip;
}

PhiTipWindow* TipIfCreated() {
  return TipSlot();
}

PhiTipWindow& Tip() {
  PhiTipWindow*& slot = TipSlot();
  if (!slot) {
    slot = new PhiTipWindow();
  }
  return *slot;
}

}  // namespace

void PhiShowTooltipFor(HWND owner,
                       const std::wstring& text,
                       const PhiPalette& palette) {
  if (!owner || text.empty()) {
    PhiHideTooltipFor(owner);
    return;
  }
  Tip().ShowFor(owner, owner, palette, text);
}

void PhiHideTooltipFor(HWND owner) {
  PhiTipWindow* tip = TipIfCreated();
  if (tip && owner && tip->owner_window() == owner) {
    tip->Hide();
  }
}

// ---------------------------------------------------------------------------
// PhiFontSet.

PhiFontSet::PhiFontSet(UINT dpi) : dpi_(dpi ? dpi : 96) {}

PhiFontSet::~PhiFontSet() {
  Rebuild(dpi_);
}

HFONT PhiFontSet::Get(const PhiFontDesc& desc) {
  for (Entry& entry : fonts_) {
    if (entry.desc.size == desc.size && entry.desc.weight == desc.weight &&
        entry.desc.strikeout == desc.strikeout &&
        entry.desc.italic == desc.italic &&
        ::lstrcmpW(entry.desc.face, desc.face) == 0) {
      return entry.font;
    }
  }
  const int height = -::MulDiv(desc.size, static_cast<int>(dpi_), 96);
  HFONT font = ::CreateFontW(
      height, 0, 0, 0, desc.weight, desc.italic ? TRUE : FALSE, FALSE,
      desc.strikeout ? TRUE : FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, desc.face);
  fonts_.push_back({desc, font});
  return font;
}

HFONT PhiFontSet::Get(int size, int weight) {
  PhiFontDesc desc;
  desc.size = size;
  desc.weight = weight;
  return Get(desc);
}

void PhiFontSet::Rebuild(UINT dpi) {
  for (Entry& entry : fonts_) {
    if (entry.font) {
      ::DeleteObject(entry.font);
      entry.font = nullptr;
    }
  }
  fonts_.clear();
  dpi_ = dpi ? dpi : 96;
}

// ---------------------------------------------------------------------------
// PhiWindow::Capture.

PhiWindow::Capture::Capture(PhiWindow* owner) : owner_(owner) {
  if (!owner_ || !owner_->hwnd()) {
    owner_ = nullptr;
    return;
  }
  ::SetCapture(owner_->hwnd());
  owner_->CaptureTaken();
}

PhiWindow::Capture::~Capture() {
  Release();
}

PhiWindow::Capture::Capture(Capture&& other) noexcept : owner_(other.owner_) {
  other.owner_ = nullptr;
}

PhiWindow::Capture& PhiWindow::Capture::operator=(Capture&& other) noexcept {
  if (this != &other) {
    Release();
    owner_ = other.owner_;
    other.owner_ = nullptr;
  }
  return *this;
}

void PhiWindow::Capture::Release() {
  if (!owner_) {
    return;
  }
  PhiWindow* owner = owner_;
  owner_ = nullptr;
  // Cleared BEFORE ReleaseCapture: that call delivers WM_CAPTURECHANGED
  // synchronously, and a DELIBERATE release must not report itself as a
  // loss through OnCaptureLost().
  owner->capture_held_ = false;
  if (owner->hwnd() && ::GetCapture() == owner->hwnd()) {
    ::ReleaseCapture();
  }
}

// ---------------------------------------------------------------------------
// PhiWindow::Timer.

PhiWindow::Timer::Timer(PhiWindow* owner) : owner_(owner) {
  if (owner_) {
    id_ = owner_->AllocTimer();
  }
}

PhiWindow::Timer::~Timer() {
  if (owner_ && id_) {
    Stop();
    owner_->ReleaseTimer(id_);
  }
}

PhiWindow::Timer::Timer(Timer&& other) noexcept
    : owner_(other.owner_), id_(other.id_), running_(other.running_) {
  other.owner_ = nullptr;
  other.id_ = 0;
  other.running_ = false;
}

PhiWindow::Timer& PhiWindow::Timer::operator=(Timer&& other) noexcept {
  if (this != &other) {
    if (owner_ && id_) {
      Stop();
      owner_->ReleaseTimer(id_);
    }
    owner_ = other.owner_;
    id_ = other.id_;
    running_ = other.running_;
    other.owner_ = nullptr;
    other.id_ = 0;
    other.running_ = false;
  }
  return *this;
}

void PhiWindow::Timer::Start(UINT interval_ms, bool coalescable) {
  if (!owner_ || !id_) {
    return;
  }
  owner_->StartTimer(id_, interval_ms, coalescable);
  running_ = true;
}

void PhiWindow::Timer::Stop() {
  if (!owner_ || !id_ || !running_) {
    return;
  }
  owner_->StopTimer(id_);
  running_ = false;
}

// ---------------------------------------------------------------------------
// PhiWindow.

PhiWindow::PhiWindow() : fonts_(96) {
  palette_ = PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kSystem);
}

PhiWindow::~PhiWindow() {
  if (theme_registered_) {
    PhiThemeBus::Get().Unregister(this);
    theme_registered_ = false;
  }
  // Both no-ops on the normal path (WM_NCDESTROY already did them), and
  // the safety net for a surface deleted without its HWND ever dying: the
  // provider table and the tooltip are keyed by PhiWindow*, and a stale
  // key would be handed to the NEXT window allocated at this address.
  PhiUiaWindowDestroyed(this);
  PhiTipWindow* tip = TipIfCreated();
  if (tip && tip->owner_window() == this) {
    tip->Forget();
  }
  DropBackBuffer();
}

HWND PhiWindow::Create(const ClassSpec& spec,
                       DWORD ex_style,
                       DWORD style,
                       const RECT& bounds,
                       HWND owner_or_parent,
                       const wchar_t* window_name) {
  if (!spec.name) {
    return nullptr;
  }
  EnsureClassRegistered(spec, &PhiWindow::Thunk);
  // The one place dpi_ is written outside WM_DPICHANGED.
  dpi_ = owner_or_parent ? ::GetDpiForWindow(owner_or_parent) : 96;
  if (!dpi_) {
    dpi_ = 96;
  }
  fonts_.Rebuild(dpi_);
  palette_ = QueryPalette();
  hwnd_ = ::CreateWindowExW(
      ex_style, spec.name, window_name ? window_name : L"", style, bounds.left,
      bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
      owner_or_parent, nullptr, ::GetModuleHandleW(nullptr), this);
  if (!hwnd_) {
    return nullptr;
  }
  if (!owner_or_parent) {
    // A top-level window with no owner takes its own monitor's dpi. An
    // owned popup does NOT: it is created at the origin and only moved
    // into place afterwards, so asking the window would answer for the
    // PRIMARY monitor and scale a popover on a second display wrong.
    const UINT actual = ::GetDpiForWindow(hwnd_);
    if (actual && actual != dpi_) {
      dpi_ = actual;
      fonts_.Rebuild(dpi_);
    }
  }
  PhiThemeBus::Get().Register(this);
  theme_registered_ = true;
  OnCreated();
  return hwnd_;
}

int PhiWindow::Scaled(int design) const {
  return ::MulDiv(design, static_cast<int>(dpi_), 96);
}

RECT PhiWindow::ScaledRect(const RECT& design) const {
  RECT out = {Scaled(static_cast<int>(design.left)),
              Scaled(static_cast<int>(design.top)),
              Scaled(static_cast<int>(design.right)),
              Scaled(static_cast<int>(design.bottom))};
  return out;
}

void PhiWindow::SetPalette(const PhiPalette& palette) {
  palette_ = palette;
  Invalidate();
}

PhiPalette PhiWindow::QueryPalette() const {
  return palette_;
}

void PhiWindow::OnThemeChanged() {
  const PhiPalette next = QueryPalette();
  palette_ = next;
  Invalidate();
}

void PhiWindow::NotifyThemeChanged() {
  OnThemeChanged();
}

int PhiWindow::HitTest(POINT client_pt) const {
  return PhiHitAt(hits_, static_cast<int>(client_pt.x),
                  static_cast<int>(client_pt.y));
}

const PhiHit* PhiWindow::HitByKey(int key) const {
  for (const PhiHit& hit : hits_) {
    if (hit.key == key) {
      return &hit;
    }
  }
  return nullptr;
}

void PhiWindow::ClearHits() {
  hits_.clear();
  hover_ = -1;
  ++hits_version_;
}

void PhiWindow::AddHit(const RECT& bounds,
                       int key,
                       std::wstring name,
                       PhiHitRole role,
                       bool selected,
                       bool enabled) {
  PhiHit hit;
  hit.bounds = PhiFromRect(bounds);
  hit.key = key;
  hit.name = std::move(name);
  hit.role = role;
  hit.selected = selected;
  hit.enabled = enabled;
  hits_.push_back(std::move(hit));
}

UINT_PTR PhiWindow::AllocTimer() {
  return static_cast<UINT_PTR>(timers_.Alloc());
}

void PhiWindow::ReleaseTimer(UINT_PTR id) {
  StopTimer(id);
  timers_.Release(static_cast<unsigned>(id));
}

void PhiWindow::StartTimer(UINT_PTR id, UINT interval_ms, bool coalescable) {
  if (!hwnd_ || !id) {
    return;
  }
  if (coalescable) {
    ::SetCoalescableTimer(hwnd_, id, interval_ms, nullptr, TIMERV_DEFAULT_COALESCING);
  } else {
    ::SetTimer(hwnd_, id, interval_ms, nullptr);
  }
}

void PhiWindow::StopTimer(UINT_PTR id) {
  if (hwnd_ && id) {
    ::KillTimer(hwnd_, id);
  }
}

void PhiWindow::Invalidate() {
  if (hwnd_) {
    ::InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

void PhiWindow::Invalidate(const RECT& rect) {
  if (hwnd_ && rect.right > rect.left && rect.bottom > rect.top) {
    ::InvalidateRect(hwnd_, &rect, FALSE);
  }
}

void PhiWindow::InvalidateHit(int index) {
  if (index < 0 || index >= static_cast<int>(hits_.size())) {
    return;
  }
  Invalidate(PhiToRect(hits_[static_cast<size_t>(index)].bounds));
}

void PhiWindow::Destroy() {
  if (hwnd_) {
    ::DestroyWindow(hwnd_);
  }
}

LRESULT PhiWindow::OnMessage(UINT, WPARAM, LPARAM, bool*) {
  return 0;
}

bool PhiWindow::OnKey(WPARAM virtual_key) {
  if (virtual_key == VK_ESCAPE) {
    OnEscape();
    return true;
  }
  if (virtual_key == VK_RETURN) {
    OnEnter();
    return true;
  }
  return false;
}

UINT PhiWindow::DlgCode() const {
  return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;
}

LRESULT PhiWindow::OnGetObject(WPARAM wparam, LPARAM lparam, bool* handled) {
  // THE accessibility seam (phi_uia.h). Every PhiWindow answers
  // UiaRootObjectId with a fragment root over hits() — one child per hit,
  // named, typed and invokable — and leaves every other object id to
  // DefWindowProcW so the raw child controls keep their MSAA answers.
  return PhiUiaOnGetObject(this, wparam, lparam, handled);
}

HWND PhiWindow::hwnd() const {
  return hwnd_;
}

const std::vector<PhiHit>& PhiWindow::hits() const {
  return hits_;
}

uint32_t PhiWindow::hits_version() const {
  return hits_version_;
}

bool PhiWindow::InvokeHit(int index) {
  int x = 0;
  int y = 0;
  if (!hwnd_ || !PhiUiaInvokePoint(hits_, index, &x, &y)) {
    return false;
  }
  // LOCALS ONLY from here down. The click can destroy the window and, for
  // a DeleteWhenDestroyed() surface, this object with it — the search-tabs
  // panel commits and closes on the button DOWN — so nothing below may
  // touch a member.
  const HWND hwnd = hwnd_;
  const LPARAM point =
      MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y));
  ::SendMessageW(hwnd, WM_LBUTTONDOWN, 0, point);
  if (::IsWindow(hwnd)) {
    ::SendMessageW(hwnd, WM_LBUTTONUP, 0, point);
  }
  return true;
}

void PhiWindow::EnableTooltips(bool enabled) {
  tooltips_enabled_ = enabled;
  if (!enabled) {
    HideTooltip();
  }
}

void PhiWindow::ScheduleTooltip() {
  if (!tooltips_enabled_ || !hwnd_) {
    return;
  }
  HideTooltip();
  if (hover_ < 0 || hover_ >= static_cast<int>(hits_.size()) ||
      hits_[static_cast<size_t>(hover_)].name.empty()) {
    return;
  }
  if (!tooltip_timer_) {
    tooltip_timer_ = AllocTimer();
  }
  // The system's own initial tooltip delay (what TTDT_INITIAL defaults to),
  // so a Phi tip appears exactly as fast as every other app's.
  StartTimer(tooltip_timer_, ::GetDoubleClickTime());
}

void PhiWindow::HideTooltip() {
  if (tooltip_timer_) {
    StopTimer(tooltip_timer_);
  }
  PhiTipWindow* tip = TipIfCreated();
  if (tip && tip->owner_window() == this) {
    tip->Hide();
  }
}

bool PhiWindow::HandleTooltipTimer(UINT_PTR id) {
  if (!tooltip_timer_ || id != tooltip_timer_) {
    return false;
  }
  StopTimer(tooltip_timer_);
  if (tooltips_enabled_ && hover_ >= 0 &&
      hover_ < static_cast<int>(hits_.size())) {
    Tip().ShowFor(this, hits_[static_cast<size_t>(hover_)].name);
  }
  return true;
}

void PhiWindow::CaptureTaken() {
  capture_held_ = true;
}

void PhiWindow::CaptureLost() {
  if (!capture_held_) {
    return;
  }
  capture_held_ = false;
  OnCaptureLost();
}

void PhiWindow::UpdateHover(POINT client_pt) {
  if (!tracking_leave_) {
    TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd_, 0};
    ::TrackMouseEvent(&track);
    tracking_leave_ = true;
  }
  const int hit = HitTest(client_pt);
  if (hit == hover_) {
    return;
  }
  const int previous = hover_;
  hover_ = hit;
  // Only the two rects that changed — never the whole window (lane 3 §3.3).
  PhiRectI dirty;
  if (previous >= 0 && previous < static_cast<int>(hits_.size())) {
    dirty = hits_[static_cast<size_t>(previous)].bounds;
  }
  if (hit >= 0 && hit < static_cast<int>(hits_.size())) {
    dirty = PhiRectUnion(dirty, hits_[static_cast<size_t>(hit)].bounds);
  }
  if (!dirty.empty()) {
    Invalidate(PhiToRect(dirty));
  }
  // Before OnHover: an override may destroy the window.
  ScheduleTooltip();
  OnHover(hit, previous);
}

void PhiWindow::ClearHover() {
  tracking_leave_ = false;
  HideTooltip();
  if (hover_ < 0) {
    return;
  }
  const int previous = hover_;
  hover_ = -1;
  if (previous < static_cast<int>(hits_.size())) {
    Invalidate(PhiToRect(hits_[static_cast<size_t>(previous)].bounds));
  }
  OnHover(-1, previous);
}

bool PhiWindow::EnsureBackBuffer(int width, int height) {
  if (width <= 0 || height <= 0) {
    return false;
  }
  if (buffer_dc_ && buffer_width_ >= width && buffer_height_ >= height) {
    return true;
  }
  DropBackBuffer();
  HDC screen = ::GetDC(hwnd_);
  if (!screen) {
    return false;
  }
  buffer_dc_ = ::CreateCompatibleDC(screen);
  if (buffer_dc_) {
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    buffer_bitmap_ = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS,
                                       &buffer_bits_, nullptr, 0);
  }
  ::ReleaseDC(hwnd_, screen);
  if (!buffer_dc_ || !buffer_bitmap_) {
    DropBackBuffer();
    return false;
  }
  buffer_previous_ = ::SelectObject(buffer_dc_, buffer_bitmap_);
  buffer_width_ = width;
  buffer_height_ = height;
  return true;
}

void PhiWindow::DropBackBuffer() {
  if (buffer_dc_) {
    if (buffer_previous_) {
      ::SelectObject(buffer_dc_, buffer_previous_);
    }
    ::DeleteDC(buffer_dc_);
  }
  if (buffer_bitmap_) {
    ::DeleteObject(buffer_bitmap_);
  }
  buffer_dc_ = nullptr;
  buffer_bitmap_ = nullptr;
  buffer_bits_ = nullptr;
  buffer_previous_ = nullptr;
  buffer_width_ = 0;
  buffer_height_ = 0;
}

bool PhiWindow::PreservePaintAlpha() const {
  return false;
}

void PhiWindow::PaintNow() {
  PAINTSTRUCT ps = {};
  HDC dc = ::BeginPaint(hwnd_, &ps);
  if (!dc) {
    return;
  }
  RECT client = {};
  ::GetClientRect(hwnd_, &client);
  RECT dirty = ps.rcPaint;
  if (dirty.right <= dirty.left || dirty.bottom <= dirty.top) {
    dirty = client;
  }
  if (EnsureBackBuffer(client.right, client.bottom)) {
    Paint(buffer_dc_, dirty);
    // Ordinary native surfaces are opaque; GDI leaves their alpha zero.
    // DWM now composites them over glass, so supply explicit alpha before
    // presenting. Shaped/layered panels use their own alpha paint path.
    ::GdiFlush();
    // SAFETY: the top-down 32bpp DIB owns width * height pixels; clamp the
    // dirty rectangle to that allocation before taking its row spans.
    auto pixels = UNSAFE_BUFFERS(base::span<uint32_t>(
        static_cast<uint32_t*>(buffer_bits_),
        static_cast<size_t>(buffer_width_) * buffer_height_));
    const int left = std::clamp<int>(dirty.left, 0, buffer_width_);
    const int right = std::clamp<int>(dirty.right, left, buffer_width_);
    for (int y = std::clamp<int>(dirty.top, 0, buffer_height_);
         !PreservePaintAlpha() &&
         y < std::clamp<int>(dirty.bottom, 0, buffer_height_); ++y) {
      for (auto& pixel : pixels.subspan(
               static_cast<size_t>(y) * buffer_width_ + left,
               static_cast<size_t>(right - left))) {
        pixel |= 0xFF000000u;
      }
    }
    ::BitBlt(dc, dirty.left, dirty.top, dirty.right - dirty.left,
             dirty.bottom - dirty.top, buffer_dc_, dirty.left, dirty.top,
             SRCCOPY);
  } else {
    Paint(dc, dirty);
  }
  ::EndPaint(hwnd_, &ps);
}

LRESULT PhiWindow::Dispatch(UINT message, WPARAM wparam, LPARAM lparam) {
  bool handled = false;
  const LRESULT result = OnMessage(message, wparam, lparam, &handled);
  if (handled) {
    return result;
  }

  switch (message) {
    case WM_ERASEBKGND:
      return 1;  // Paint() covers the client rect.

    case WM_PAINT:
      PaintNow();
      // A repaint is what follows a layout pass, so this is where a
      // rebuilt hits_ becomes visible to a screen reader. No-op unless
      // this window has handed out a provider AND hits_version() moved.
      PhiUiaHitsChanged(this);
      return 0;

    case WM_PRINTCLIENT: {
      // The SAME path as WM_PAINT (lane 3 §3.1 finding: the shell rebuilds
      // its model here today).
      RECT client = {};
      ::GetClientRect(hwnd_, &client);
      Paint(reinterpret_cast<HDC>(wparam), client);
      return 0;
    }

    case WM_MOUSEMOVE: {
      POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      UpdateHover(pt);
      return 0;
    }

    case WM_MOUSELEAVE:
      ClearHover();
      return 0;

    case WM_LBUTTONDOWN: {
      POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      HideTooltip();
      OnMouseDown(HitTest(pt), pt);
      return 0;
    }

    case WM_LBUTTONUP: {
      POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      OnClick(HitTest(pt), pt);
      return 0;
    }

    case WM_MOUSEWHEEL: {
      POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ::ScreenToClient(hwnd_, &pt);
      OnWheel(GET_WHEEL_DELTA_WPARAM(wparam), pt);
      return 0;
    }

    case WM_KEYDOWN:
      if (OnKey(wparam)) {
        return 0;
      }
      break;

    case WM_GETDLGCODE:
      return static_cast<LRESULT>(DlgCode());

    case WM_TIMER:
      // The base's own tooltip id never reaches a subclass: OnTimerFired
      // overrides do not chain to the base.
      if (HandleTooltipTimer(static_cast<UINT_PTR>(wparam))) {
        return 0;
      }
      OnTimerFired(static_cast<UINT_PTR>(wparam));
      return 0;

    case WM_CAPTURECHANGED:
      // lParam names the window TAKING the capture; our own SetCapture is
      // not a loss.
      if (reinterpret_cast<HWND>(lparam) != hwnd_) {
        CaptureLost();
      }
      return 0;

    case WM_CANCELMODE:
      CaptureLost();
      break;  // DefWindowProcW releases the capture itself.

    case WM_SIZE:
      OnSize(LOWORD(lparam), HIWORD(lparam));
      return 0;

    case WM_DPICHANGED: {
      // THE only place dpi_ moves after Create() (contract rule 4).
      const UINT old_dpi = dpi_;
      const UINT next = HIWORD(wparam);
      dpi_ = next ? next : old_dpi;
      if (const RECT* suggested = reinterpret_cast<const RECT*>(lparam)) {
        ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                       suggested->right - suggested->left,
                       suggested->bottom - suggested->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
      }
      fonts_.Rebuild(dpi_);
      DropBackBuffer();
      OnDpiChanged(old_dpi);
      Invalidate();
      return 0;
    }

    case WM_GETOBJECT: {
      bool a11y_handled = false;
      const LRESULT a11y = OnGetObject(wparam, lparam, &a11y_handled);
      if (a11y_handled) {
        return a11y;
      }
      break;
    }

    default:
      break;
  }
  return ::DefWindowProcW(hwnd_, message, wparam, lparam);
}

// static
LRESULT CALLBACK PhiWindow::Thunk(HWND hwnd,
                                  UINT message,
                                  WPARAM wparam,
                                  LPARAM lparam) {
  auto* self =
      reinterpret_cast<PhiWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<PhiWindow*>(create->lpCreateParams);
    if (self) {
      self->hwnd_ = hwnd;
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
  }
  if (!self) {
    // Before WM_NCCREATE and after WM_NCDESTROY — never a member call.
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
  }
  if (message == WM_NCDESTROY) {
    self->OnNcDestroy();
    PhiAnimator::ForThread().CancelForWindow(hwnd);
    if (self->theme_registered_) {
      PhiThemeBus::Get().Unregister(self);
      self->theme_registered_ = false;
    }
    // A UIA client is another process holding COM references that can
    // outlive this HWND by any amount: DETACH the provider (it then
    // answers UIA_E_ELEMENTNOTAVAILABLE) rather than freeing it, and let
    // UiaDisconnectProvider drop the core's own cache.
    PhiUiaWindowDestroyed(self);
    PhiTipWindow* tip = TipIfCreated();
    if (tip && tip->owner_window() == self) {
      tip->Forget();
    }
    self->DropBackBuffer();
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    self->hwnd_ = nullptr;
    const LRESULT result = ::DefWindowProcW(hwnd, message, wparam, lparam);
    if (self->delete_on_destroy_) {
      delete self;  // Nothing can reach it now: USERDATA is zeroed.
    }
    return result;
  }
  return self->Dispatch(message, wparam, lparam);
}

}  // namespace phi_client

#endif  // defined(_WIN32)

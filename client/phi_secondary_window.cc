// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_secondary_window.h"

#include <algorithm>

namespace phi_client {

// ---------------------------------------------------------------------------
// Pure — compiled on the Linux host too (test/model/ui_substrate_test.cc).

namespace {

// The buttons in TRAILING-TO-LEADING order, which is the order they are
// laid out in: close outermost, then maximize, then minimize.
struct CaptionSlot {
  PhiCaptionPart part;
  bool PhiCaptionMetrics::* present;
};

constexpr CaptionSlot kCaptionSlots[] = {
    {PhiCaptionPart::kClose, &PhiCaptionMetrics::close},
    {PhiCaptionPart::kMaximize, &PhiCaptionMetrics::maximize},
    {PhiCaptionPart::kMinimize, &PhiCaptionMetrics::minimize},
};

}  // namespace

PhiRectI PhiCaptionButtonBounds(const PhiCaptionMetrics& metrics,
                                PhiCaptionPart part) {
  PhiRectI empty;
  if (metrics.width <= 0 || metrics.height <= 0 || metrics.button_width <= 0) {
    return empty;
  }
  int right = metrics.width;
  for (const CaptionSlot& slot : kCaptionSlots) {
    if (!(metrics.*slot.present)) {
      continue;
    }
    if (slot.part == part) {
      PhiRectI out;
      out.left = right - metrics.button_width;
      out.top = 0;
      out.right = right;
      out.bottom = metrics.height;
      return out;
    }
    right -= metrics.button_width;
  }
  return empty;
}

PhiCaptionPart PhiCaptionHitTest(const PhiCaptionMetrics& metrics,
                                 int x,
                                 int y) {
  if (y < 0 || y >= metrics.height) {
    return PhiCaptionPart::kOutside;
  }
  for (const CaptionSlot& slot : kCaptionSlots) {
    const PhiRectI bounds = PhiCaptionButtonBounds(metrics, slot.part);
    if (!bounds.empty() && bounds.Contains(x, y)) {
      return slot.part;
    }
  }
  return PhiCaptionPart::kCaption;
}

int PhiCaptionTitleInset(const PhiCaptionMetrics& metrics, int leading) {
  int trailing = 0;
  for (const CaptionSlot& slot : kCaptionSlots) {
    if (metrics.*slot.present) {
      trailing += metrics.button_width;
    }
  }
  return std::max(leading, trailing);
}

PhiRectI PhiFitRectInto(const PhiRectI& frame, const PhiRectI& work) {
  const int w = std::max(0, std::min(frame.width(), work.width()));
  const int h = std::max(0, std::min(frame.height(), work.height()));
  // w/h never exceed the work area, so the clamp bounds are ordered.
  const int x = std::clamp(frame.left, work.left, work.right - w);
  const int y = std::clamp(frame.top, work.top, work.bottom - h);
  PhiRectI out;
  out.left = x;
  out.top = y;
  out.right = x + w;
  out.bottom = y + h;
  return out;
}

PhiRectI PhiPlaceCentered(int width,
                          int height,
                          const PhiRectI& anchor,
                          const PhiRectI& work) {
  PhiRectI frame;
  frame.left = anchor.left + (anchor.width() - width) / 2;
  frame.top = anchor.top + (anchor.height() - height) / 3;
  frame.right = frame.left + width;
  frame.bottom = frame.top + height;
  return PhiFitRectInto(frame, work);
}

bool PhiDialogWantsKey(unsigned keys,
                       PhiDialogMessageKind kind,
                       unsigned virtual_key) {
  if (keys == kPhiDialogNone || kind == PhiDialogMessageKind::kOther) {
    return false;
  }
  switch (virtual_key) {
    case kPhiVkTab:
      return (keys & kPhiDialogTab) != 0;
    case kPhiVkEscape:
      return (keys & kPhiDialogEscape) != 0;
    case kPhiVkReturn:
      return (keys & kPhiDialogEnter) != 0;
    case kPhiVkLeft:
    case kPhiVkUp:
    case kPhiVkRight:
    case kPhiVkDown:
      // Arrows never arrive as WM_CHAR; asking about one is a no.
      return kind != PhiDialogMessageKind::kChar &&
             (keys & kPhiDialogArrows) != 0;
    default:
      return false;
  }
}

int PhiDialogOwner(const std::vector<uintptr_t>& roots,
                   const std::vector<uintptr_t>& ancestors) {
  for (uintptr_t candidate : ancestors) {
    if (!candidate) {
      continue;
    }
    for (size_t i = 0; i < roots.size(); ++i) {
      if (roots[i] == candidate) {
        return static_cast<int>(i);
      }
    }
  }
  return -1;
}

}  // namespace phi_client

#if defined(_WIN32)

namespace phi_client {

namespace {

// The thread's active dialogs. A raw leaked vector rather than a
// thread_local object: -Wexit-time-destructors rules out a destructor on
// a static or thread_local, and the list must outlive every window on
// the thread anyway.
struct DialogEntry {
  HWND hwnd = nullptr;
  unsigned keys = 0;
};

std::vector<DialogEntry>& Dialogs() {
  thread_local std::vector<DialogEntry>* list = new std::vector<DialogEntry>();
  return *list;
}

// One WH_GETMESSAGE hook per UI thread, installed with the first
// registration and removed with the last (a plain pointer, so no
// exit-time destructor).
thread_local HHOOK g_dialog_hook = nullptr;

PhiDialogMessageKind KindOf(UINT message) {
  switch (message) {
    case WM_KEYDOWN:
      return PhiDialogMessageKind::kKeyDown;
    case WM_SYSKEYDOWN:
      return PhiDialogMessageKind::kSysKeyDown;
    case WM_CHAR:
    case WM_SYSCHAR:
      return PhiDialogMessageKind::kChar;
    default:
      return PhiDialogMessageKind::kOther;
  }
}

// WM_CHAR carries the CHARACTER, not the virtual key; for the three keys
// the dialog manager consumes they are the same number, which is exactly
// why the list above is limited to those.
unsigned VirtualKeyOf(const MSG& msg) {
  return static_cast<unsigned>(msg.wParam);
}

LRESULT CALLBACK DialogGetMsgProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION && wparam == PM_REMOVE) {
    auto* msg = reinterpret_cast<MSG*>(lparam);
    if (PhiFilterDialogMessage(msg)) {
      // IsDialogMessageW already dispatched what it wanted; blank the
      // message so the main loop's TranslateMessage / DispatchMessage
      // cannot deliver it a second time.
      msg->message = WM_NULL;
      msg->wParam = 0;
      msg->lParam = 0;
    }
  }
  return ::CallNextHookEx(nullptr, code, wparam, lparam);
}

void EnsureDialogHook() {
  if (g_dialog_hook || Dialogs().empty()) {
    return;
  }
  g_dialog_hook = ::SetWindowsHookExW(WH_GETMESSAGE, &DialogGetMsgProc,
                                      nullptr, ::GetCurrentThreadId());
}

void DropDialogHookIfIdle() {
  if (g_dialog_hook && Dialogs().empty()) {
    ::UnhookWindowsHookEx(g_dialog_hook);
    g_dialog_hook = nullptr;
  }
}

}  // namespace

void PhiRegisterDialogWindow(HWND hwnd, unsigned keys) {
  if (!hwnd || keys == kPhiDialogNone) {
    return;
  }
  std::vector<DialogEntry>& list = Dialogs();
  for (DialogEntry& entry : list) {
    if (entry.hwnd == hwnd) {
      entry.keys = keys;
      return;
    }
  }
  list.push_back({hwnd, keys});
  EnsureDialogHook();
}

void PhiUnregisterDialogWindow(HWND hwnd) {
  std::vector<DialogEntry>& list = Dialogs();
  for (size_t i = 0; i < list.size(); ++i) {
    if (list[i].hwnd == hwnd) {
      list.erase(list.begin() + static_cast<ptrdiff_t>(i));
      break;
    }
  }
  DropDialogHookIfIdle();
}

size_t PhiActiveDialogCount() {
  return Dialogs().size();
}

bool PhiFilterDialogMessage(MSG* msg) {
  if (!msg || !msg->hwnd) {
    return false;
  }
  const PhiDialogMessageKind kind = KindOf(msg->message);
  if (kind == PhiDialogMessageKind::kOther) {
    return false;
  }
  std::vector<DialogEntry>& list = Dialogs();
  if (list.empty()) {
    return false;
  }
  std::vector<uintptr_t> roots;
  roots.reserve(list.size());
  for (const DialogEntry& entry : list) {
    roots.push_back(reinterpret_cast<uintptr_t>(entry.hwnd));
  }
  // The target and its parents, innermost first. Depth is bounded: the
  // deepest control set in the client is a child of a top-level window.
  std::vector<uintptr_t> ancestors;
  for (HWND walk = msg->hwnd; walk; walk = ::GetParent(walk)) {
    ancestors.push_back(reinterpret_cast<uintptr_t>(walk));
    if (ancestors.size() > 8) {
      break;
    }
  }
  const int index = PhiDialogOwner(roots, ancestors);
  if (index < 0) {
    return false;
  }
  const DialogEntry& owner = list[static_cast<size_t>(index)];
  if (!PhiDialogWantsKey(owner.keys, kind, VirtualKeyOf(*msg))) {
    return false;
  }
  return ::IsDialogMessageW(owner.hwnd, msg) != FALSE;
}

// ---------------------------------------------------------------------------
// PhiSecondaryWindow.

namespace {

PhiRectI ToRectI(const RECT& r) {
  PhiRectI out;
  out.left = r.left;
  out.top = r.top;
  out.right = r.right;
  out.bottom = r.bottom;
  return out;
}

RECT MonitorWorkArea(HMONITOR monitor) {
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  if (monitor && ::GetMonitorInfoW(monitor, &info)) {
    return info.rcWork;
  }
  RECT work = {};
  ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  return work;
}

// The C-style probe PhiPopupChrome asks before answering HTCAPTION, so
// the frame needs no knowledge of the window that owns it (and no
// friend).
bool ClientProbeThunk(void* context, POINT client_pt) {
  auto* self = static_cast<PhiSecondaryWindow*>(context);
  return self->CaptionClaimsClientForFrame(client_pt);
}

}  // namespace

PhiSecondaryWindow::PhiSecondaryWindow() {
  colors_ = PhiPopupColorsNow();
}

PhiSecondaryWindow::~PhiSecondaryWindow() {
  if (dialog_registered_ && hwnd()) {
    PhiUnregisterDialogWindow(hwnd());
    dialog_registered_ = false;
  }
}

HWND PhiSecondaryWindow::CreateSecondary(const ClassSpec& spec,
                                         DWORD ex_style,
                                         DWORD style,
                                         const RECT& bounds,
                                         HWND owner,
                                         const wchar_t* title,
                                         const Options& options) {
  options_ = options;
  colors_ = QueryColors();
  if (!Create(spec, ex_style, style, bounds, owner, title)) {
    return nullptr;
  }
  chrome_.SetCaptionHeight(options_.caption_height_dip);
  chrome_.SetDimWhenInactive(options_.dim_when_inactive);
  chrome_.SetClientProbe(&ClientProbeThunk, this);
  chrome_.Attach(hwnd(), options_.caption_buttons, colors_);
  if (options_.dialog_keys != kPhiDialogNone) {
    PhiRegisterDialogWindow(hwnd(), options_.dialog_keys);
    dialog_registered_ = true;
  }
  return hwnd();
}

PhiPopupColors PhiSecondaryWindow::QueryColors() const {
  return PhiPopupColorsNow();
}

bool PhiSecondaryWindow::CaptionClaimsClient(POINT) const {
  return false;
}

bool PhiSecondaryWindow::CaptionClaimsClientForFrame(POINT client_pt) const {
  return CaptionClaimsClient(client_pt);
}

int PhiSecondaryWindow::CaptionHeight() const {
  return chrome_.CaptionHeight();
}

RECT PhiSecondaryWindow::ContentRect() const {
  return chrome_.ContentRect();
}

void PhiSecondaryWindow::CenterOn(HWND around) {
  if (!alive()) {
    return;
  }
  // The monitor the window opens on: the anchor's (a minimized anchor
  // answers for where it was restored), else the owner's, else the one
  // under the pointer — never simply the primary.
  HWND reference = around && ::IsWindow(around)
                       ? around
                       : ::GetWindow(hwnd(), GW_OWNER);
  HMONITOR monitor = nullptr;
  if (reference) {
    monitor = ::MonitorFromWindow(reference, MONITOR_DEFAULTTONEAREST);
  } else {
    POINT cursor = {};
    ::GetCursorPos(&cursor);
    monitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
  }
  const RECT work = MonitorWorkArea(monitor);
  RECT anchor = {};
  if (!around || ::IsIconic(around) || !::GetWindowRect(around, &anchor)) {
    anchor = work;
  }
  RECT frame = {};
  ::GetWindowRect(hwnd(), &frame);
  const int w = frame.right - frame.left;
  const int h = frame.bottom - frame.top;
  const PhiRectI placed =
      PhiPlaceCentered(w, h, ToRectI(anchor), ToRectI(work));
  const bool same_size = placed.width() == w && placed.height() == h;
  ::SetWindowPos(hwnd(), nullptr, placed.left, placed.top, placed.width(),
                 placed.height(),
                 SWP_NOZORDER | SWP_NOACTIVATE | (same_size ? SWP_NOSIZE : 0));
}

RECT PhiSecondaryWindow::WorkArea() const {
  return MonitorWorkArea(
      alive() ? ::MonitorFromWindow(hwnd(), MONITOR_DEFAULTTONEAREST)
              : nullptr);
}

void PhiSecondaryWindow::FitToWorkArea() {
  if (!alive() || ::IsZoomed(hwnd()) || ::IsIconic(hwnd())) {
    return;
  }
  RECT frame = {};
  ::GetWindowRect(hwnd(), &frame);
  const PhiRectI now = ToRectI(frame);
  const PhiRectI fitted = PhiFitRectInto(now, ToRectI(WorkArea()));
  if (fitted == now) {
    return;
  }
  const bool same_size =
      fitted.width() == now.width() && fitted.height() == now.height();
  ::SetWindowPos(hwnd(), nullptr, fitted.left, fitted.top, fitted.width(),
                 fitted.height(),
                 SWP_NOZORDER | SWP_NOACTIVATE | (same_size ? SWP_NOSIZE : 0));
}

void PhiSecondaryWindow::PaintBand(HDC dc, const wchar_t* title) {
  chrome_.PaintCaption(dc, colors_, options_.show_mark, title);
}

void PhiSecondaryWindow::PaintCaptionButtons(HDC dc) {
  chrome_.PaintCaptionButtons(dc, colors_);
}

void PhiSecondaryWindow::RefreshColors() {
  const PhiPopupColors next = QueryColors();
  colors_ = next;
  chrome_.ApplyColors(colors_);
  OnColorsChanged();
  Invalidate();
}

void PhiSecondaryWindow::OnThemeChanged() {
  PhiWindow::OnThemeChanged();  // Palette + repaint.
  RefreshColors();              // The popup ladder the band and cards use.
}

void PhiSecondaryWindow::OnDpiChanged(UINT) {
  // PhiWindow has already moved the window and rebuilt fonts(); the
  // chrome saw WM_DPICHANGED on its way through OnMessage and rebuilt
  // its own two.
}

void PhiSecondaryWindow::OnNcDestroy() {
  if (dialog_registered_) {
    PhiUnregisterDialogWindow(hwnd());
    dialog_registered_ = false;
  }
}

LRESULT PhiSecondaryWindow::OnMessage(UINT message,
                                      WPARAM wparam,
                                      LPARAM lparam,
                                      bool* handled) {
  LRESULT result = 0;
  if (chrome_.HandleMessage(message, wparam, lparam, &result)) {
    // The frame took it. Two of those also mean the pointer left the
    // client area, which is how the Kiosk's toolbar drops its hover.
    if (message == WM_NCMOUSEMOVE || message == WM_NCMOUSELEAVE) {
      OnFramePointer();
    }
    *handled = true;
    return result;
  }
  return OnWindowMessage(message, wparam, lparam, handled);
}

LRESULT PhiSecondaryWindow::OnWindowMessage(UINT message,
                                            WPARAM wparam,
                                            LPARAM lparam,
                                            bool* handled) {
  return PhiWindow::OnMessage(message, wparam, lparam, handled);
}

}  // namespace phi_client

#endif  // defined(_WIN32)

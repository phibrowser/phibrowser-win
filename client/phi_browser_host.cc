// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_browser_host.h"

#include <dwmapi.h>

#include <algorithm>

#include "base/logging.h"
#include "base/no_destructor.h"
#include "client/phi_dwrite.h"
#include "ui/base/win/phi_hosted_window.h"

namespace phi_client {

namespace {
// Original window procs of adopted browser children, by hwnd (a slot hosts
// one per Space). UI-thread only.
std::map<HWND, WNDPROC>& BrowserBaseProcs() {
  static base::NoDestructor<std::map<HWND, WNDPROC>> procs;
  return *procs;
}

LRESULT CALLBACK BrowserSubclassProc(HWND hwnd, UINT msg, WPARAM wparam,
                                     LPARAM lparam) {
  WNDPROC base = nullptr;
  auto it = BrowserBaseProcs().find(hwnd);
  if (it != BrowserBaseProcs().end()) {
    base = it->second;
  }
  if (msg == WM_WINDOWPOSCHANGED) {
    const auto* pos = reinterpret_cast<const WINDOWPOS*>(lparam);
    if (pos && !(pos->flags & SWP_NOZORDER)) {
      // Async: never touch sibling z-order from inside its own reorder.
      ::PostMessageW(::GetParent(hwnd), PhiBrowserHost::kMsgRaiseCaps, 0, 0);
    }
  }
  if (msg == WM_NCDESTROY) {
    // Windows recycles HWND values: a stale map entry would hand a future
    // window this browser's old WNDPROC. Unhook on final destruction.
    const LRESULT result =
        base ? ::CallWindowProcW(base, hwnd, msg, wparam, lparam)
             : ::DefWindowProcW(hwnd, msg, wparam, lparam);
    BrowserBaseProcs().erase(hwnd);
    return result;
  }
  return base ? ::CallWindowProcW(base, hwnd, msg, wparam, lparam)
              : ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

// A corner cap: a tiny shell child ABOVE the browser child, shaped (window
// region) to the part of a radius-square OUTSIDE the rounded corner's arc.
// The browser renders via DirectComposition, which ignores the child's own
// window region — but hwnd AIRSPACE is honoured: a sibling above occludes.
// The cap paints the ground tint (plus the card's 1px arc), so the page
// reads as a rounded card exactly like the Mac's.
LRESULT CALLBACK CornerCapProc(HWND hwnd, UINT msg, WPARAM wparam,
                               LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_NCDESTROY) {
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);  // See ShellWndProc.
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_PAINT) {
    auto* self = reinterpret_cast<PhiBrowserHost*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    PAINTSTRUCT ps = {};
    HDC dc = ::BeginPaint(hwnd, &ps);
    if (self) {
      self->PaintCornerCap(hwnd, dc);
    }
    ::EndPaint(hwnd, &ps);
    return 0;
  }
  if (msg == WM_NCHITTEST) {
    return HTTRANSPARENT;  // The tint under it takes the click.
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

ATOM EnsureCornerCapClass() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &CornerCapProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PhiCornerCap";
    return ::RegisterClassExW(&wc);
  }();
  return atom;
}

// The status bubble rides the content card's bottom-left like the corner
// caps do: a shell-child sibling raised over the reparented browser (the
// only way client pixels appear over the page).
LRESULT CALLBACK StatusBubbleProc(HWND hwnd, UINT msg, WPARAM wparam,
                                  LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_NCDESTROY) {
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);  // See ShellWndProc.
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_PAINT) {
    auto* self = reinterpret_cast<PhiBrowserHost*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    PAINTSTRUCT ps = {};
    HDC dc = ::BeginPaint(hwnd, &ps);
    if (self) {
      self->PaintStatusBubble(hwnd, dc);
    }
    ::EndPaint(hwnd, &ps);
    return 0;
  }
  if (msg == WM_NCHITTEST) {
    return HTTRANSPARENT;  // Hovers keep belonging to the page under it.
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

ATOM EnsureStatusBubbleClass() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &StatusBubbleProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PhiStatusBubble";
    return ::RegisterClassExW(&wc);
  }();
  return atom;
}

// The ground tint at `y` of a `height`-tall window: the band gradient's
// colour there, which is what a corner cap shows outside the card's arc.
COLORREF GroundTintAt(const PhiPalette& palette, int y, int height) {
  height = height > 0 ? height : 1;
  auto channel = [&](int top, int bottom) {
    return top + (bottom - top) * y / height;
  };
  return RGB(
      channel(GetRValue(palette.ground_top), GetRValue(palette.ground_bottom)),
      channel(GetGValue(palette.ground_top), GetGValue(palette.ground_bottom)),
      channel(GetBValue(palette.ground_top),
              GetBValue(palette.ground_bottom)));
}

}  // namespace


// --- Lifetime ------------------------------------------------------------

PhiBrowserHost::PhiBrowserHost(PhiBrowserHostShell* shell) : shell_(shell) {}

PhiBrowserHost::~PhiBrowserHost() {
  ::RemovePropW(shell_hwnd(), ui::win::kPhiShellRestoredPlacement);
  // The overlays are shell children and would die with it; taking them down
  // here keeps their procs from finding a half-destroyed host.
  if (status_bubble_ && ::IsWindow(status_bubble_)) {
    ::DestroyWindow(status_bubble_);
  }
  status_bubble_ = nullptr;
  for (HWND& cap : corner_caps_) {
    if (cap && ::IsWindow(cap)) {
      ::DestroyWindow(cap);
    }
    cap = nullptr;
  }
}

HWND PhiBrowserHost::shell_hwnd() const {
  return shell_->BrowserHostShellWindow();
}

int PhiBrowserHost::Scaled(int design_units) const {
  return shell_->BrowserHostScaled(design_units);
}

void PhiBrowserHost::AdoptFirst(const std::string& space_id,
                                BrowserId browser,
                                HWND browser_hwnd) {
  content_hwnd_ = browser_hwnd;
  AdoptContentChild();
  spaces_.Attach(space_id, browser);
  hwnds_[browser] = browser_hwnd;
}

HWND PhiBrowserHost::ReleaseContentChildKeepRecord() {
  HWND child = content_hwnd_;
  if (child) {
    ui::win::SetPhiHostedWindow(child, shell_hwnd(), false);
  }
  content_hwnd_ = nullptr;
  return child;
}

HWND PhiBrowserHost::DetachSpaceBrowser(const std::string& space_id) {
  if (!spaces_.Has(space_id)) {
    return nullptr;
  }
  const BrowserId browser = spaces_.ForSpace(space_id);
  auto it = hwnds_.find(browser);
  HWND child = it != hwnds_.end() ? it->second : nullptr;
  if (child && ::IsWindow(child)) {
    ui::win::SetPhiHostedWindow(child, shell_hwnd(), false);
    ::ShowWindow(child, SW_HIDE);
  }
  if (content_hwnd_ == child) {
    content_hwnd_ = nullptr;
  }
  hwnds_.erase(browser);
  spaces_.Forget(browser);
  return child;
}

void PhiBrowserHost::ReadoptContentChild(HWND browser_hwnd) {
  if (!browser_hwnd || !::IsWindow(browser_hwnd)) {
    return;
  }
  content_hwnd_ = browser_hwnd;
  AdoptContentChild();
  shell_->BrowserHostRelayout();  // Shows + positions it in this shell.
}

void PhiBrowserHost::ReleaseContentChild() {
  if (content_hwnd_ && ::IsWindow(content_hwnd_)) {
    ui::win::SetPhiHostedWindow(content_hwnd_, nullptr, false);
    ::SetParent(content_hwnd_, nullptr);
  }
  content_hwnd_ = nullptr;
}

bool PhiBrowserHost::ActiveBrowserPlaceholding() const {
  const BrowserId active = spaces_.active();
  return active.valid() && placeholder_.count(active) > 0;
}

void PhiBrowserHost::RaiseOverlays() {
  // The browser child reordered itself (Chromium raises its window on
  // activation): put the overlays back above it. Never from inside that
  // reorder — this runs on the posted kMsgRaiseCaps turn.
  if (status_bubble_ && ::IsWindow(status_bubble_) &&
      ::IsWindowVisible(status_bubble_)) {
    ::SetWindowPos(status_bubble_, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  for (HWND cap : corner_caps_) {
    if (cap && ::IsWindow(cap)) {
      ::SetWindowPos(cap, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
  }
}

void PhiBrowserHost::PositionContentChild(const PhiContentLayout& layout) {
  if (content_hwnd_ && ::IsWindow(content_hwnd_)) {
    const int w = layout.content.right - layout.content.left;
    const int h = layout.content.bottom - layout.content.top;
    // A child awaiting the slide-end reveal is PARKED outside the client
    // area — the parent clips children, so it stays invisible even if
    // Chromium shows it on its own (fresh browsers do, post-creation).
    const int x = layout.parked == content_hwnd_ ? layout.client.right
                                                 : layout.content.left;
    // A placeholder-mode browser stays HIDDEN: its strip is empty and the
    // dead tab's final frame would linger in the content card.
    const bool placeholding = ActiveBrowserPlaceholding();
    ::SetWindowPos(content_hwnd_, nullptr, x, layout.content.top,
                   w > 0 ? w : 0, h > 0 ? h : 0,
                   SWP_NOZORDER | SWP_NOACTIVATE |
                       (placeholding ? SWP_HIDEWINDOW : SWP_SHOWWINDOW));
  }
}

void PhiBrowserHost::HidePreviousContent(HWND previous) {
  if (!previous || previous == content_hwnd_ || !::IsWindow(previous) ||
      !::IsWindowVisible(previous)) {
    return;
  }
  // SetWindowPos and GDI painting finish before DirectComposition presents
  // their result. Hiding the outgoing child immediately can uncover the
  // transparent shell for one desktop frame. Keep it underneath until the
  // incoming child (or the full-content slide frame) has been presented.
  ::DwmFlush();
  ::ShowWindow(previous, SW_HIDE);
}

void PhiBrowserHost::PositionCornerCaps(const PhiContentLayout& layout) {
  if (layout.corner_radius <= 0) {
    for (HWND& cap : corner_caps_) {
      if (cap) {
        ::DestroyWindow(cap);
        cap = nullptr;
      }
    }
    return;
  }
  // The rounded card corners. DComp ignores a region ON the browser child
  // (verified by pixel probe: corners stayed page-white), so the corners are
  // capped by sibling windows above it instead.
  EnsureCornerCapClass();
  const int radius = layout.corner_radius;
  const bool top_caps = layout.top_caps;
  const std::array<POINT, 4> positions = {{
      {layout.content.left, layout.content.top},
      {layout.content.right - radius, layout.content.top},
      {layout.content.left, layout.content.bottom - radius},
      {layout.content.right - radius, layout.content.bottom - radius},
  }};
  for (int i = 0; i < 4; ++i) {
    if (!corner_caps_[i]) {
      // Layered (per-pixel alpha, a child since Windows 8): the arc is a
      // coverage ramp, not a region's pixel-grid cut.
      corner_caps_[i] = ::CreateWindowExW(
          WS_EX_LAYERED, L"PhiCornerCap", L"", WS_CHILD, 0, 0, radius,
          radius, shell_hwnd(), nullptr, ::GetModuleHandleW(nullptr), this);
    }
    if (!corner_caps_[i]) {
      continue;
    }
    const bool shown = ((i == 2) || (i == 3 && layout.right_caps) ||
                        (i < 2 && top_caps)) &&
                       !content_fullscreen_;
    ::SetWindowPos(corner_caps_[i], HWND_TOP, positions[i].x, positions[i].y,
                   radius, radius,
                   SWP_NOACTIVATE | (shown ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    // Shape: the radius-square minus the quarter circle — only the bite
    // outside the card's curve shows. The circle is centred on the cap's
    // inner corner with radius `radius` exactly; the cap strokes the ring
    // one pixel outside it, and the shell's frame uses the same circle, so
    // the hairline runs unbroken around the arc. The layer carries the
    // shape as alpha; only if it cannot be committed does the cap fall
    // back to a window region (the same shape, on the pixel grid).
    const bool layered =
        (::GetWindowLongPtrW(corner_caps_[i], GWL_EXSTYLE) & WS_EX_LAYERED) !=
        0;
    if (layered && CommitCornerCap(i)) {
      continue;
    }
    if (layered) {
      ::SetWindowLongPtrW(
          corner_caps_[i], GWL_EXSTYLE,
          ::GetWindowLongPtrW(corner_caps_[i], GWL_EXSTYLE) & ~WS_EX_LAYERED);
    }
    HRGN square = ::CreateRectRgn(0, 0, radius + 1, radius + 1);
    const int cx = (i == 0 || i == 2) ? radius : 0;
    const int cy = i < 2 ? radius : 0;
    HRGN circle = ::CreateEllipticRgn(cx - radius, cy - radius, cx + radius,
                                      cy + radius);
    ::CombineRgn(square, square, circle, RGN_DIFF);
    ::DeleteObject(circle);
    ::SetWindowRgn(corner_caps_[i], square, TRUE);  // Window owns it now.
    ::InvalidateRect(corner_caps_[i], nullptr, TRUE);
  }
}


void PhiBrowserHost::OnContentFullscreenChanged(BrowserId browser,
                                                bool fullscreen) {
  if (browser != spaces_.active()) {
    return;
  }
  content_fullscreen_ = fullscreen;
  shell_->BrowserHostRelayout();
}

void PhiBrowserHost::SetShellFullscreen(BrowserId browser,
                                        bool fullscreen,
                                        const RECT& target) {
  const HWND shell = shell_hwnd();
  if (!shell || (fullscreen && (browser != spaces_.active() ||
                                shell_->BrowserHostIsAgent()))) {
    return;
  }
  if (fullscreen) {
    // The standard borderless-fullscreen recipe: remember placement and
    // style, strip the frame, cover the window's monitor.
    if (!shell_fullscreen_) {
      pre_fullscreen_placement_.length = sizeof(WINDOWPLACEMENT);
      ::GetWindowPlacement(shell, &pre_fullscreen_placement_);
      pre_fullscreen_style_ = ::GetWindowLongPtrW(shell, GWL_STYLE);
    }
    shell_fullscreen_ = true;
    ::SetPropW(shell, ui::win::kPhiShellRestoredPlacement,
               reinterpret_cast<HANDLE>(&pre_fullscreen_placement_));
    ::SetPropW(shell, ui::win::kPhiShellFullscreen,
               reinterpret_cast<HANDLE>(1));
    MONITORINFO monitor = {};
    monitor.cbSize = sizeof(monitor);
    ::GetMonitorInfoW(
        ::MonitorFromWindow(shell, MONITOR_DEFAULTTONEAREST), &monitor);
    if (target.right > target.left && target.bottom > target.top) {
      monitor.rcMonitor = target;
    }
    ::SetWindowLongPtrW(
        shell, GWL_STYLE,
        pre_fullscreen_style_ &
            ~static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME));
    ::SetWindowPos(shell, HWND_TOP, monitor.rcMonitor.left,
                   monitor.rcMonitor.top,
                   monitor.rcMonitor.right - monitor.rcMonitor.left,
                   monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                   SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
  } else {
    // A late event from a hidden Space cannot change the visible shell.
    if (!shell_fullscreen_ || browser != spaces_.active()) {
      return;
    }
    shell_fullscreen_ = false;
    ::RemovePropW(shell, ui::win::kPhiShellRestoredPlacement);
    ::RemovePropW(shell, ui::win::kPhiShellFullscreen);
    ::SetWindowLongPtrW(shell, GWL_STYLE, pre_fullscreen_style_);
    ::SetWindowPlacement(shell, &pre_fullscreen_placement_);
    ::SetWindowPos(shell, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
  }
  shell_->BrowserHostRelayout();
  ::InvalidateRect(shell, nullptr, TRUE);
}

void PhiBrowserHost::OnPlaceholderModeChanged(BrowserId browser,
                                              bool entered) {
  if (entered) {
    placeholder_.insert(browser);
  } else {
    placeholder_.erase(browser);
  }
  if (browser == spaces_.active()) {
    if (content_hwnd_ && ::IsWindow(content_hwnd_)) {
      ::ShowWindow(content_hwnd_, entered ? SW_HIDE : SW_SHOW);
    }
    if (entered) {
      // The address bar stays the way in (the Mac keeps it usable).
      shell_->BrowserHostFocusAddressField();
    }
    // Placeholder mode is a layout input (the bookmark bar's "on new tab",
    // the native new-tab page): the rebuild turn re-reads it.
    shell_->BrowserHostScheduleRefresh();
  }
}

void PhiBrowserHost::AdoptContentChild() {
  // Strip the top-level chrome/styles and make Chromium's window a clipped
  // child of our shell. SetParent on a WS_CHILD window is a true child reparent
  // (hwnd_message_handler.cc SetParentOrOwner), so it clips into our content
  // rect and stops being an independent taskbar/Alt-Tab entry.
  LONG_PTR style = ::GetWindowLongPtrW(content_hwnd_, GWL_STYLE);
  style &= ~(WS_POPUP | WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_THICKFRAME |
             WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
  style |= WS_CHILD | WS_CLIPSIBLINGS;
  ::SetWindowLongPtrW(content_hwnd_, GWL_STYLE, style);
  // Drop the top-level extended frame edges too; without this + FRAMECHANGED,
  // Windows keeps the old non-client metrics and paints a dead band at the
  // top of the content region.
  LONG_PTR ex = ::GetWindowLongPtrW(content_hwnd_, GWL_EXSTYLE);
  ex &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME |
          WS_EX_STATICEDGE | WS_EX_APPWINDOW);
  ::SetWindowLongPtrW(content_hwnd_, GWL_EXSTYLE, ex);
  ::SetParent(content_hwnd_, shell_hwnd());
  ui::win::SetPhiHostedWindow(content_hwnd_, shell_hwnd(), true);
  // Watch its z-order: Chromium raises this window on activation, which
  // would bury the corner caps riding above it.
  if (BrowserBaseProcs().find(content_hwnd_) == BrowserBaseProcs().end()) {
    WNDPROC previous = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
        content_hwnd_, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&BrowserSubclassProc)));
    BrowserBaseProcs()[content_hwnd_] = previous;
  }
  ::SetWindowPos(content_hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
}

bool PhiBrowserHost::CommitCornerCap(int index) {
  if (index < 0 || index > 3 || !corner_caps_[index] ||
      !::IsWindow(corner_caps_[index])) {
    return false;
  }
  RECT rc = {};
  ::GetClientRect(corner_caps_[index], &rc);
  const int radius = rc.right;
  if (radius <= 0 || rc.bottom != radius) {
    return false;
  }
  const RECT content = shell_->BrowserHostContentRect();
  const PhiPalette palette = shell_->BrowserHostPalette();
  const int y = index < 2 ? content.top : content.bottom - radius;
  // The card's 1px hairline rides the ring just outside the arc — the same
  // circle the frame's PhiStrokeRoundRectAA of kContentRadius + 1 follows,
  // so the two meet at the cap's edge.
  const int ground_height = shell_->BrowserHostClientRect().bottom;
  return PhiCommitCornerCap(corner_caps_[index], index, radius,
                            GroundTintAt(palette, y, ground_height),
                            palette.border);
}

void PhiBrowserHost::RefreshCornerCaps() {
  for (int i = 0; i < 4; ++i) {
    HWND cap = corner_caps_[i];
    if (!cap || !::IsWindow(cap)) {
      continue;
    }
    const bool layered =
        (::GetWindowLongPtrW(cap, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
    if (!layered || !CommitCornerCap(i)) {
      ::InvalidateRect(cap, nullptr, TRUE);
    }
  }
}

// The GDI path, for a cap whose layer could not be committed and so keeps
// the region cut: the tint over the bite, and the hairline around it.
void PhiBrowserHost::PaintCornerCap(HWND cap, HDC dc) {
  int corner = -1;
  for (int i = 0; i < 4; ++i) {
    if (corner_caps_[i] == cap) {
      corner = i;
    }
  }
  if (corner < 0) {
    return;
  }
  RECT rc = {};
  ::GetClientRect(cap, &rc);
  const RECT content = shell_->BrowserHostContentRect();
  const PhiPalette palette = shell_->BrowserHostPalette();
  // The ground tint at this cap's height, matching the band gradient.
  const int y = corner < 2 ? content.top : content.bottom - rc.bottom;
  const int ground_height = shell_->BrowserHostClientRect().bottom;
  HBRUSH tint = ::CreateSolidBrush(GroundTintAt(palette, y, ground_height));
  ::FillRect(dc, &rc, tint);
  ::DeleteObject(tint);
  // The card's 1px border, continued around the arc: the ring just OUTSIDE
  // the region's circle (same centre, radius + 1), which is also the circle
  // the shell's frame stroke follows, so the two meet at the cap's edge.
  // A ring on the region's own circle would fall inside the bite and be
  // clipped away — the corners then showed a bare, unstroked arc.
  const int r = rc.right;
  const int cx = (corner == 0 || corner == 2) ? r : 0;
  const int cy = corner < 2 ? r : 0;
  PhiStrokeEllipseAA(dc, {cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1},
                     palette.border, 1);
}

void PhiBrowserHost::OnTargetURLChanged(BrowserId browser,
                                        const std::wstring& url) {
  if (shell_->BrowserHostIsAgent()) {
    return;  // Off-screen agent surfaces show no chrome.
  }
  if (browser != spaces_.active()) {
    return;  // A hidden Space's page; its bubble would lie about this one.
  }
  SetStatusUrl(url);
}

void PhiBrowserHost::SetStatusUrl(const std::wstring& url) {
  if (url == status_url_) {
    return;
  }
  status_url_ = url;
  if (url.empty()) {
    if (status_bubble_) {
      ::ShowWindow(status_bubble_, SW_HIDE);
    }
    return;
  }
  const HWND shell = shell_hwnd();
  if (!shell) {
    return;
  }
  EnsureStatusBubbleClass();
  if (!status_bubble_) {
    // Layered: its rounded corner is alpha, not a region's stair-step.
    status_bubble_ = ::CreateWindowExW(
        WS_EX_LAYERED, L"PhiStatusBubble", L"", WS_CHILD, 0, 0, 0, 0,
        shell, nullptr, ::GetModuleHandleW(nullptr), this);
  }
  if (!status_bubble_) {
    return;
  }
  const RECT content = shell_->BrowserHostContentRect();
  const int height = Scaled(24);
  SIZE text = {};
  if (HDC dc = ::GetDC(shell)) {
    HFONT status_font = shell_->BrowserHostStatusFont();
    HGDIOBJ old_font =
        status_font ? ::SelectObject(dc, status_font) : nullptr;
    ::GetTextExtentPoint32W(dc, status_url_.c_str(),
                            static_cast<int>(status_url_.size()), &text);
    if (old_font) {
      ::SelectObject(dc, old_font);
    }
    ::ReleaseDC(shell, dc);
  }
  // At most half the content width (the Mac StatusURLView's cap).
  const int max_width = std::max<int>(
      Scaled(60), (content.right - content.left) / 2);
  const int width = std::min<int>(text.cx + Scaled(24), max_width);
  ::SetWindowPos(status_bubble_, HWND_TOP, content.left,
                 content.bottom - height, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
  // Only the top-right corner rounds. The bubble's surface is committed
  // with that corner as alpha coverage; should the layer fail, the same
  // shape goes on as a window region (oversized so the other three corners
  // fall outside the window) and the bubble paints through WM_PAINT.
  const int r = Scaled(8);
  const bool layered =
      (::GetWindowLongPtrW(status_bubble_, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
  if (layered &&
      PhiCommitShapedLayer(
          status_bubble_, width, height,
          [&](HDC dc) { PaintStatusBubble(status_bubble_, dc); },
          [&](double px, double py) {
            return PhiRoundRectCoverage(px, py, width, height, r,
                                        /*corners=*/0x2);
          })) {
    return;
  }
  if (layered) {
    ::SetWindowLongPtrW(
        status_bubble_, GWL_EXSTYLE,
        ::GetWindowLongPtrW(status_bubble_, GWL_EXSTYLE) & ~WS_EX_LAYERED);
  }
  HRGN shape = ::CreateRoundRectRgn(-2 * r, 0, width + 1, height + 2 * r + 1,
                                    2 * r, 2 * r);
  ::SetWindowRgn(status_bubble_, shape, TRUE);  // Window owns it now.
  ::InvalidateRect(status_bubble_, nullptr, TRUE);
}

void PhiBrowserHost::PaintStatusBubble(HWND bubble, HDC dc) {
  RECT rc = {};
  ::GetClientRect(bubble, &rc);
  const PhiPalette palette = shell_->BrowserHostPalette();
  HBRUSH fill = ::CreateSolidBrush(palette.card);
  ::FillRect(dc, &rc, fill);
  ::DeleteObject(fill);
  // The card border along the exposed top and right edges: a rounded rect
  // oversized past the left and bottom, so only its top-right arc shows.
  const int r = Scaled(8);
  PhiStrokeRoundRectAA(dc, {-2 * r, 0, rc.right, rc.bottom + 2 * r}, r,
                       palette.border, 1);
  RECT text = rc;
  text.left += Scaled(10);
  text.right -= Scaled(12);
  ::SetBkMode(dc, TRANSPARENT);
  ::SetTextColor(dc, palette.label);
  HFONT status_font = shell_->BrowserHostStatusFont();
  HGDIOBJ old_font =
      status_font ? ::SelectObject(dc, status_font) : nullptr;
  // Middle truncation, the way a URL keeps its host and tail readable.
  PhiDrawTextAA(dc, status_url_.c_str(), -1, &text,
              DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
  if (old_font) {
    ::SelectObject(dc, old_font);
  }
}

void PhiBrowserHost::RequestRaiseCaps() {
  if (const HWND shell = shell_hwnd()) {
    ::PostMessageW(shell, kMsgRaiseCaps, 0, 0);
  }
}

void PhiBrowserHost::AttachSpaceBrowser(const std::string& space_id,
                                        BrowserId browser,
                                        HWND browser_hwnd,
                                        bool agent) {
  if (!browser_hwnd || !::IsWindow(browser_hwnd)) {
    return;
  }
  // Reparent it into this shell exactly like the primary browser, then show it
  // in place of the current Space's.
  HWND previous = content_hwnd_;
  if (previous && previous != browser_hwnd) {
    if (auto* framework = shell_->BrowserHostFramework()) {
      framework->ExitContentFullscreen(spaces_.active().value);
      ui::win::SetPhiHostedWindow(previous, shell_hwnd(), false);
      framework->FocusBrowserWindow(spaces_.active().value, false);
      framework->CloseExtensionPopup(spaces_.active().value);
    }
    content_fullscreen_ = false;
    ui::win::SetPhiHostedWindow(previous, shell_hwnd(), false);
  }
  shell_->BrowserHostBeginSpaceSwap(space_id, browser);
  content_hwnd_ = browser_hwnd;
  AdoptContentChild();
  if (shell_->BrowserHostSwapRunning()) {
    shell_->BrowserHostNoteSwapChildren(
        content_hwnd_,
        previous && previous != browser_hwnd ? previous : nullptr);
    // Parked off the client rect until the band lands (LayoutContents keeps
    // it there; hiding would lose to Chromium's own post-creation Show).
    RECT rc = {};
    ::GetClientRect(shell_hwnd(), &rc);
    ::SetWindowPos(content_hwnd_, nullptr, rc.right, 0, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  spaces_.Attach(space_id, browser);
  if (!agent) {
    shell_->BrowserHostNoteSpaceWindow(space_id, browser);
  }
  hwnds_[browser] = browser_hwnd;
  shell_->BrowserHostEndSpacePreview();
  if (phi_framework::PhiFramework* framework =
          agent ? nullptr : shell_->BrowserHostFramework()) {
    framework->SetWindowSpace(browser.value, space_id.c_str());
  }
  shell_->BrowserHostRelayout();
  if (!shell_->BrowserHostSwapRunning() && previous &&
      previous != browser_hwnd && ::IsWindow(previous)) {
    // Outgoing goes away only after the incoming one is shown in place, so
    // the content rect is never uncovered.
    HidePreviousContent(previous);
  }
  shell_->BrowserHostRefreshTabs();
}

void PhiBrowserHost::FocusActiveBrowser() {
  // Deferred switches and background callbacks must never steal native focus.
  if (::GetForegroundWindow() != shell_hwnd()) {
    return;
  }
  if (shell_->BrowserHostFocusNativePage()) {
    // The native new-tab page owns the keyboard while it is up: its omnibox
    // IS the page (the Mac focuses it in viewDidAppear).
    return;
  }
  // Showing a different browser child is not enough: as a WS_CHILD it never
  // gets WM_ACTIVATE, so without this the keyboard still goes to the browser
  // that was visible before the Space switch (input appears dead until the
  // user clicks). Give the child Win32 focus AND drive Chromium's own
  // activation across the boundary.
  if (content_hwnd_ && ::IsWindow(content_hwnd_)) {
    ::SetFocus(content_hwnd_);
  }
  const BrowserId active = spaces_.active();
  phi_framework::PhiFramework* framework = shell_->BrowserHostFramework();
  if (framework && active.valid()) {
    framework->FocusBrowserWindow(active.value);
  }
}

bool PhiBrowserHost::ShowSpace(const std::string& space_id) {
  if (!spaces_.Has(space_id)) {
    return false;
  }
  auto hwnd_it = hwnds_.find(spaces_.ForSpace(space_id));
  if (hwnd_it == hwnds_.end() || !::IsWindow(hwnd_it->second)) {
    return false;
  }
  HWND previous = (content_hwnd_ && content_hwnd_ != hwnd_it->second &&
                   ::IsWindow(content_hwnd_))
                      ? content_hwnd_
                      : nullptr;
  if (previous) {
    if (auto* framework = shell_->BrowserHostFramework()) {
      framework->ExitContentFullscreen(spaces_.active().value);
      ui::win::SetPhiHostedWindow(previous, shell_hwnd(), false);
      framework->FocusBrowserWindow(spaces_.active().value, false);
      framework->CloseExtensionPopup(spaces_.active().value);
    }
    content_fullscreen_ = false;
    ui::win::SetPhiHostedWindow(previous, shell_hwnd(), false);
  }
  SetStatusUrl(std::wstring());  // The leaving page's link target is stale.
  shell_->BrowserHostAbandonTyping();  // So is a half-typed query.
  shell_->BrowserHostBeginSpaceSwap(space_id, spaces_.ForSpace(space_id));
  content_hwnd_ = hwnd_it->second;
  ui::win::SetPhiHostedWindow(content_hwnd_, shell_hwnd(), true);
  spaces_.Show(space_id);
  shell_->BrowserHostEndSpacePreview();
  if (shell_->BrowserHostSwapRunning()) {
    // Deferred: the entering page is positioned hidden and revealed when
    // the band slide completes; the leaving page stays up until then.
    shell_->BrowserHostNoteSwapChildren(content_hwnd_, previous);
    shell_->BrowserHostRelayout();
  } else {
    // Shows + positions the incoming child...
    shell_->BrowserHostRelayout();
    // ...and only then drop the outgoing one (never uncover the rect).
    if (previous) {
      HidePreviousContent(previous);
    }
    FocusActiveBrowser();
  }
  shell_->BrowserHostRefreshTabs();
  shell_->BrowserHostRetintChrome();
  shell_->BrowserHostNoteActiveSpace(space_id);
  return true;
}

void PhiBrowserHost::CloseSpaceBrowser(const std::string& space_id) {
  if (!spaces_.Has(space_id)) {
    return;
  }
  const BrowserId browser = spaces_.ForSpace(space_id);
  if (shell_->BrowserHostDeferSpaceClose(space_id)) {
    // Closing the slot's last browser destroys the shell; never from under
    // a nested loop of our own (the RequestClose invariant).
    return;
  }
  if (spaces_.active_space() == space_id) {
    for (const std::string& other : spaces_.order()) {
      if (other != space_id && spaces_.Has(other)) {
        ShowSpace(other);
        break;
      }
    }
  }
  if (phi_framework::PhiFramework* framework =
          shell_->BrowserHostFramework()) {
    // Chromium's close path; OnBrowserWindowClosing then reaches
    // ForgetBrowser for the bookkeeping. Closing the slot's last browser
    // tears the whole slot down — intended for a window that only ever
    // showed the deleted Space.
    framework->CloseBrowserWindow(browser.value);
  }
}

bool PhiBrowserHost::ForgetBrowser(BrowserId browser) {
  hwnds_.erase(browser);
  if (spaces_.Forget(browser)) {
    content_hwnd_ = nullptr;
    // Fall back to any remaining Space so the slot stays usable.
    if (!spaces_.order().empty()) {
      ShowSpace(spaces_.order().front());
    }
  }
  return !spaces_.empty();
}

}  // namespace phi_client

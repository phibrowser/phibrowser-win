// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_agent_handoff_prompt.h"

#include <windows.h>

#include <algorithm>
#include <string>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "client/phi_agent_transcript.h"
#include "client/phi_dwrite.h"
#include "client/phi_i18n.h"
#include "client/phi_popup_chrome.h"
#include "client/phi_secondary_window.h"
#include "client/phi_shell_window.h"

namespace phi_client {

namespace {

// The Mac panel: 320pt wide, content-driven height, 20pt gutters.
constexpr int kWidth = 320;
constexpr int kPad = 20;
constexpr int kButtonHeight = 28;
constexpr int kButtonGap = 8;
constexpr int kSwitchWidth = 168;
constexpr int kLaterWidth = 72;

class Prompt : public PhiSecondaryWindow {
 public:
  static Prompt* instance;

  Prompt(const std::string& task_id,
         const std::string& message,
         int64_t slot,
         AgentHost* host)
      : task_id_(task_id),
        // The agent's words, cleaned and capped the way the transcript
        // shows them: this window is TOPMOST and sized to its text.
        message_(base::UTF8ToWide(
            message.empty()
                ? base::WideToUTF8(PhiStr("agent.handoffPrompt.message"))
                : PhiAgentTranscript::SanitizeText(
                      message, /*keep_newlines=*/true,
                      PhiAgentTranscript::kMaxDetailChars))),
        slot_(slot),
        host_(host) {}

  ~Prompt() override {
    if (bg_brush_) {
      ::DeleteObject(bg_brush_);
    }
  }

  const std::string& task_id() const { return task_id_; }

  bool Open(HWND anchor) {
    ClassSpec spec;
    spec.name = L"PhiAgentHandoffPrompt";
    Options options;
    options.caption_buttons = kPhiCaptionClose;
    options.dialog_keys = kPhiDialogTab | kPhiDialogEscape | kPhiDialogEnter;
    // Created at a provisional size; the body is measured once the window
    // has its DPI, then the frame is fitted and centred.
    const RECT bounds = {0, 0, 320, 160};
    if (!CreateSecondary(spec, WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                         WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                         bounds, anchor, PhiStr("agent.handoffPrompt.title"),
                         options)) {
      return false;
    }
    OnColorsChanged();
    CreateChildren();
    Fit();
    CenterOn(anchor);
    // Shown without stealing focus: the user may be mid-keystroke in
    // another app; the flash (the Mac's dock bounce) says something wants
    // them.
    ::ShowWindow(hwnd(), SW_SHOWNOACTIVATE);
    if (anchor) {
      FLASHWINFO flash = {sizeof(flash), anchor,
                          FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
      ::FlashWindowEx(&flash);
    }
    return true;
  }

 protected:
  void OnColorsChanged() override {
    if (bg_brush_) {
      ::DeleteObject(bg_brush_);
    }
    bg_brush_ = ::CreateSolidBrush(colors().bg);
    Invalidate();
  }

  void OnDpiChanged(UINT old_dpi) override {
    PhiSecondaryWindow::OnDpiChanged(old_dpi);
    for (HWND child : {switch_, later_}) {
      ::SendMessageW(child, WM_SETFONT,
                     reinterpret_cast<WPARAM>(fonts().Get(13, FW_NORMAL)),
                     TRUE);
    }
    Fit();
  }

  void Paint(HDC dc, const RECT&) override {
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    ::FillRect(dc, &client, bg_brush_);
    PaintBand(dc);
    RECT body = BodyRect();
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, colors().label);
    ::SelectObject(dc, fonts().Get(13, FW_NORMAL));
    PhiDrawTextAA(dc, message_.c_str(), -1, &body,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
  }

  LRESULT OnWindowMessage(UINT message,
                          WPARAM wparam,
                          LPARAM lparam,
                          bool* handled) override {
    switch (message) {
      case DM_GETDEFID:
        *handled = true;
        return MAKELRESULT(IDOK, DC_HASDEFID);
      case WM_COMMAND:
        if (LOWORD(wparam) == IDOK) {
          Switch();
          *handled = true;
          return 0;
        }
        if (LOWORD(wparam) == IDCANCEL) {
          Destroy();
          *handled = true;
          return 0;
        }
        break;
      case WM_DRAWITEM:
        if (PhiPopupDrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam),
                               colors(), nullptr)) {
          *handled = true;
          return TRUE;
        }
        break;
      case WM_CLOSE:
        Destroy();
        *handled = true;
        return 0;
      default:
        break;
    }
    return PhiSecondaryWindow::OnWindowMessage(message, wparam, lparam,
                                               handled);
  }

  void OnNcDestroy() override {
    if (instance == this) {
      instance = nullptr;
    }
    PhiSecondaryWindow::OnNcDestroy();
  }

 private:
  void CreateChildren() {
    HFONT font = fonts().Get(13, FW_NORMAL);
    switch_ = ::CreateWindowExW(
        0, L"BUTTON", PhiStr("agent.handoffPrompt.switchToAgentSpaceButton"),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 10, 10,
        hwnd(), reinterpret_cast<HMENU>(IDOK), ::GetModuleHandleW(nullptr),
        nullptr);
    later_ = ::CreateWindowExW(
        0, L"BUTTON", PhiStr("agent.handoffPrompt.laterButton"),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10,
        hwnd(), reinterpret_cast<HMENU>(IDCANCEL), ::GetModuleHandleW(nullptr),
        nullptr);
    for (HWND child : {switch_, later_}) {
      ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    PhiPopupStyleButton(switch_, PhiPopupButtonRole::kPrimary);
    PhiPopupStyleButton(later_, PhiPopupButtonRole::kSecondary);
  }

  // The message block: the band, a gutter, the wrapped text.
  RECT BodyRect() const {
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    const int pad = Scaled(kPad);
    return {pad, CaptionHeight() + Scaled(12), client.right - pad,
            client.bottom - pad - Scaled(kButtonHeight) - Scaled(12)};
  }

  // Sizes the frame to the wrapped message, then places the buttons in
  // the bottom-right (the primary rightmost, as the Mac's alert row).
  void Fit() {
    const int width = Scaled(kWidth);
    const int pad = Scaled(kPad);
    HDC dc = ::GetDC(hwnd());
    HGDIOBJ old = ::SelectObject(dc, fonts().Get(13, FW_NORMAL));
    RECT measure = {0, 0, width - 2 * pad, 0};
    PhiDrawTextAA(dc, message_.c_str(), -1, &measure,
                  DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    ::SelectObject(dc, old);
    ::ReleaseDC(hwnd(), dc);
    const int text_height = std::max<int>(measure.bottom, Scaled(18));
    const int client_height = CaptionHeight() + Scaled(12) + text_height +
                              Scaled(12) + Scaled(kButtonHeight) + pad;
    RECT frame = {0, 0, width, client_height};
    ::AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(::GetWindowLongPtrW(
                                           hwnd(), GWL_STYLE)),
                               FALSE,
                               static_cast<DWORD>(::GetWindowLongPtrW(
                                   hwnd(), GWL_EXSTYLE)),
                               dpi());
    // Never taller than the screen it is on: the buttons stay reachable
    // and the (clipped) text cannot cover the work area.
    MONITORINFO monitor = {sizeof(monitor)};
    if (::GetMonitorInfoW(
            ::MonitorFromWindow(hwnd(), MONITOR_DEFAULTTONEAREST), &monitor)) {
      const LONG work_height = monitor.rcWork.bottom - monitor.rcWork.top;
      if (frame.bottom - frame.top > work_height) {
        frame.bottom = frame.top + work_height;
      }
    }
    ::SetWindowPos(hwnd(), nullptr, 0, 0, frame.right - frame.left,
                   frame.bottom - frame.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    const int top = client.bottom - pad - Scaled(kButtonHeight);
    const int switch_w = Scaled(kSwitchWidth);
    const int later_w = Scaled(kLaterWidth);
    ::MoveWindow(switch_, client.right - pad - switch_w, top, switch_w,
                 Scaled(kButtonHeight), TRUE);
    ::MoveWindow(later_,
                 client.right - pad - switch_w - Scaled(kButtonGap) - later_w,
                 top, later_w, Scaled(kButtonHeight), TRUE);
    Invalidate();
  }

  void Switch() {
    AgentHost* host = host_;
    const std::string task_id = task_id_;
    const int64_t slot = slot_;
    Destroy();  // Before the watch: the swap must not fight the prompt.
    if (host) {
      host->RequestWatchAgentSpace(task_id, slot);
    }
  }

  std::string task_id_;
  std::wstring message_;
  int64_t slot_ = 0;
  RAW_PTR_EXCLUSION AgentHost* host_ = nullptr;
  HWND switch_ = nullptr;
  HWND later_ = nullptr;
  HBRUSH bg_brush_ = nullptr;
};

Prompt* Prompt::instance = nullptr;

}  // namespace

// static
void PhiAgentHandoffPrompt::Show(PhiShellWindow* anchor,
                                 const std::string& task_id,
                                 const std::string& message,
                                 AgentHost* host) {
  if (Prompt::instance) {
    Prompt::instance->Destroy();  // A newer request replaces the old one.
    Prompt::instance = nullptr;
  }
  HWND anchor_hwnd = anchor ? anchor->shell_hwnd() : nullptr;
  auto* prompt = new Prompt(task_id, message,
                            anchor ? anchor->slot().value : 0, host);
  prompt->DeleteWhenDestroyed();
  Prompt::instance = prompt;
  if (!prompt->Open(anchor_hwnd)) {
    Prompt::instance = nullptr;
    delete prompt;
  }
}

// static
void PhiAgentHandoffPrompt::DismissFor(const std::string& task_id) {
  if (Prompt::instance && Prompt::instance->task_id() == task_id) {
    Prompt::instance->Destroy();
    Prompt::instance = nullptr;
  }
}

}  // namespace phi_client

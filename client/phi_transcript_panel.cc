// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_transcript_panel.h"

#include <commctrl.h>

#include <algorithm>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "phibrowser-win/client/phi_popup_chrome.h"
#include "phibrowser-win/client/phi_secondary_window.h"
#include "phibrowser-win/client/phi_text_wrap.h"

namespace phi_client {

namespace {

// The Mac floating panel's shape: 460×560, no smaller than 320×300.
constexpr int kPanelWidth = 460;
constexpr int kPanelHeight = 560;
constexpr int kPanelMinWidth = 320;
constexpr int kPanelMinHeight = 300;
constexpr int kInputRowHeight = 44;
constexpr int kSendWidth = 72;
constexpr int kPad = 8;

constexpr UINT_PTR kFeedId = 1;
constexpr UINT_PTR kInputId = 2;
constexpr UINT_PTR kSendId = 3;

// The Mac AgentTranscriptPanel's own console palette — deliberately not
// the theme roles: a near-white / near-black ground, a slightly deeper
// chrome band, and text at 85% / 92%.
struct ConsolePalette {
  COLORREF background;
  COLORREF chrome;
  COLORREF text;
  COLORREF faint;
};

ConsolePalette ConsoleFor(bool dark) {
  if (dark) {
    return {RGB(19, 20, 26), RGB(31, 32, 40), RGB(235, 235, 235),
            RGB(89, 89, 89)};
  }
  return {RGB(246, 246, 249), RGB(233, 235, 238), RGB(38, 38, 38),
          RGB(173, 173, 173)};
}

class Panel : public PhiSecondaryWindow {
 public:
  static Panel* instance;

  Panel(const PhiPalette& palette,
        const std::string& task_id,
        PhiTranscriptHost* host)
      : task_id_(task_id),
        host_(host),
        open_dark_(palette.dark),
        open_accent_(palette.accent) {}

  ~Panel() override {
    if (ui_font_) {
      ::DeleteObject(ui_font_);
    }
    DropBrushes();
  }

  bool Open(HWND owner) {
    ClassSpec spec;
    spec.name = L"PhiTranscriptPanel";

    // Park beside the owner's top-right corner, clamped to the work area.
    // The dpi the base takes from the owner is the one this arithmetic
    // needs, so the window is created at a provisional origin and moved
    // by the same numbers the old code used.
    const UINT dpi = ::GetDpiForWindow(owner);
    const auto scaled = [dpi](int design) {
      return ::MulDiv(design, static_cast<int>(dpi ? dpi : 96), 96);
    };
    RECT owner_rect = {};
    ::GetWindowRect(owner, &owner_rect);
    const int w = scaled(kPanelWidth);
    const int h = scaled(kPanelHeight);
    int x = owner_rect.right + scaled(8);
    int y = owner_rect.top;
    HMONITOR monitor = ::MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {sizeof(info)};
    if (::GetMonitorInfoW(monitor, &info)) {
      if (x + w > info.rcWork.right) {
        x = std::max(static_cast<int>(info.rcWork.left),
                     static_cast<int>(owner_rect.right) - w);
        y = owner_rect.top + scaled(40);
      }
      y = std::max(static_cast<int>(info.rcWork.top),
                   std::min(y, static_cast<int>(info.rcWork.bottom) - h));
    }
    const std::wstring title =
        L"Agent Transcript — " +
        base::UTF8ToWide(host_->TranscriptAgentName(task_id_));

    Options options;
    // The Mac window is closable, miniaturizable and resizable: the full
    // Windows cluster.
    options.caption_buttons =
        kPhiCaptionClose | kPhiCaptionMinimize | kPhiCaptionMaximize;
    options.dialog_keys = kPhiDialogTab;

    const RECT bounds = {x, y, x + w, y + h};
    if (!CreateSecondary(spec, WS_EX_TOOLWINDOW,
                         WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
                             WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
                         bounds, owner, title.c_str(), options)) {
      return false;
    }
    OnColorsChanged();  // The console palette and the cached brushes.
    EnsureFonts();
    CreateChildren();
    Refresh();
    ::ShowWindow(hwnd(), SW_SHOWNOACTIVATE);
    return true;
  }

  const std::string& task_id() const { return task_id_; }

  void Refresh() {
    if (!feed_ || !host_) {
      return;
    }
    std::vector<std::string> lines;
    for (const PhiTranscriptEntry& entry : host_->TranscriptEntries(task_id_)) {
      lines.push_back(PhiRenderTranscriptEntry(entry));
    }
    const std::string activity = host_->TranscriptActivity(task_id_);
    if (!activity.empty()) {
      lines.push_back("· " + activity);
    }
    if (lines.size() == last_line_count_) {
      return;  // Nothing new; leave the user's scroll/selection alone.
    }
    last_line_count_ = lines.size();
    std::wstring text;
    for (const std::string& line : lines) {
      std::string one = line;
      size_t pos = 0;
      while ((pos = one.find('\n', pos)) != std::string::npos) {
        one.replace(pos, 1, "\r\n");
        pos += 2;
      }
      text += base::UTF8ToWide(one);
      text += L"\r\n";
    }
    ::SetWindowTextW(feed_, text.c_str());
    // Follow the tail, the console way.
    const int length = ::GetWindowTextLengthW(feed_);
    ::SendMessageW(feed_, EM_SETSEL, length, length);
    ::SendMessageW(feed_, EM_SCROLLCARET, 0, 0);
  }

  void Send() {
    if (!input_ || !host_) {
      return;
    }
    const int length = ::GetWindowTextLengthW(input_);
    if (length <= 0) {
      return;
    }
    std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, L'\0');
    ::GetWindowTextW(input_, buffer.data(), length + 1);
    const std::string text = base::WideToUTF8(buffer.data());
    if (host_->PushTranscriptMessage(task_id_, text)) {
      ::SetWindowTextW(input_, L"");
      Refresh();
    }
  }

 protected:
  // The panel opened on the shell's palette; once the OS or the app
  // theme actually moves, the app-wide answer is the only live one it
  // has (lane 3 ranked finding 7).
  PhiPopupColors QueryColors() const override {
    return followed_theme_ ? PhiPopupColorsNow()
                           : PhiPopupColorsFor(open_dark_, open_accent_);
  }

  void OnThemeChanged() override {
    followed_theme_ = true;
    PhiSecondaryWindow::OnThemeChanged();
  }

  void OnColorsChanged() override {
    console_ = ConsoleFor(colors().dark);
    DropBrushes();
    bg_brush_ = ::CreateSolidBrush(colors().bg);
    feed_brush_ = ::CreateSolidBrush(console_.background);
    card_brush_ = ::CreateSolidBrush(colors().card);
    rule_brush_ = ::CreateSolidBrush(console_.faint);
    PhiPopupThemeControl(feed_, colors().dark);
  }

  void OnDpiChanged(UINT old_dpi) override {
    PhiSecondaryWindow::OnDpiChanged(old_dpi);
    // fonts() was rebuilt by the base; the one font this panel owns
    // outright has to follow (lane 3 ranked finding 10 — the transcript
    // and the feedback form kept their pre-move fonts).
    EnsureFonts();
    ::SendMessageW(feed_, WM_SETFONT, reinterpret_cast<WPARAM>(FeedFont()),
                   TRUE);
    for (HWND child : {input_, send_}) {
      ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_),
                     TRUE);
    }
    Layout();
  }

  void OnSize(int, int) override { Layout(); }

  void Paint(HDC dc, const RECT&) override {
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    ::FillRect(dc, &client, bg_brush_);
    PaintBand(dc);
    // The feed's ground reaches the window edges; the divider (the Mac's
    // faint rule) closes it above the prompt row.
    const int row = Scaled(kInputRowHeight);
    RECT feed = {0, CaptionHeight(), client.right, client.bottom - row - 1};
    ::FillRect(dc, &feed, feed_brush_);
    RECT rule = {0, client.bottom - row - 1, client.right, client.bottom - row};
    ::FillRect(dc, &rule, rule_brush_);
    PhiPopupPaintWell(dc, InputWell(), colors(), dpi());
  }

  LRESULT OnWindowMessage(UINT message,
                          WPARAM wparam,
                          LPARAM lparam,
                          bool* handled) override {
    switch (message) {
      case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lparam);
        mmi->ptMinTrackSize.x = Scaled(kPanelMinWidth);
        mmi->ptMinTrackSize.y = Scaled(kPanelMinHeight);
        *handled = true;
        return 0;
      }
      case WM_COMMAND:
        if (LOWORD(wparam) == kSendId) {
          Send();
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
      case WM_CTLCOLORSTATIC: {  // The read-only feed EDIT arrives here.
        HDC dc = reinterpret_cast<HDC>(wparam);
        ::SetTextColor(dc, console_.text);
        ::SetBkColor(dc, console_.background);
        *handled = true;
        return reinterpret_cast<LRESULT>(feed_brush_);
      }
      case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        ::SetTextColor(dc, colors().label);
        ::SetBkColor(dc, colors().card);
        *handled = true;
        return reinterpret_cast<LRESULT>(card_brush_);
      }
      case WM_CLOSE:
        Destroy();  // DeleteWhenDestroyed() frees us after WM_NCDESTROY.
        *handled = true;
        return 0;
      default:
        break;
    }
    return PhiSecondaryWindow::OnWindowMessage(message, wparam, lparam, handled);
  }

  void OnNcDestroy() override {
    if (instance == this) {
      instance = nullptr;
    }
    PhiSecondaryWindow::OnNcDestroy();
  }

 private:
  // The feed reads best in a console face (the Mac's monospaced 11.5).
  HFONT FeedFont() {
    PhiFontDesc desc;
    desc.size = 12;
    desc.face = L"Cascadia Mono";
    return fonts().Get(desc);
  }

  void EnsureFonts() {
    if (ui_font_) {
      ::DeleteObject(ui_font_);
    }
    ui_font_ = PhiPopupFont(dpi(), 13);
  }

  void DropBrushes() {
    for (HBRUSH* slot :
         {&bg_brush_, &feed_brush_, &card_brush_, &rule_brush_}) {
      if (*slot) {
        ::DeleteObject(*slot);
        *slot = nullptr;
      }
    }
  }

  void CreateChildren() {
    feed_ = ::CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL,
        0, 0, 10, 10, hwnd(), reinterpret_cast<HMENU>(kFeedId),
        ::GetModuleHandleW(nullptr), nullptr);
    PhiUseTextWrapRules(feed_);
    input_ = ::CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT |
                             ES_AUTOHSCROLL,
        0, 0, 10, 10, hwnd(), reinterpret_cast<HMENU>(kInputId),
        ::GetModuleHandleW(nullptr), nullptr);
    send_ = ::CreateWindowExW(
        0, L"BUTTON", L"Send",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10,
        hwnd(), reinterpret_cast<HMENU>(kSendId), ::GetModuleHandleW(nullptr),
        nullptr);
    ::SendMessageW(feed_, WM_SETFONT, reinterpret_cast<WPARAM>(FeedFont()),
                   TRUE);
    for (HWND child : {input_, send_}) {
      ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_),
                     TRUE);
    }
    ::SendMessageW(input_, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(L"Message the agent…"));
    PhiPopupStyleButton(send_, PhiPopupButtonRole::kPrimary);
    PhiPopupThemeControl(feed_, colors().dark);
    // Enter sends; the subclass forwards everything else to the EDIT.
    ::SetWindowLongPtrW(input_, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(this));
    input_base_proc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
        input_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&InputProc)));
    Layout();
  }

  // The prompt row's well (the input's drawn border) and the divider over
  // it, in client pixels.
  RECT InputWell() const {
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    const int pad = Scaled(kPad);
    const int row = Scaled(kInputRowHeight);
    const int send_w = Scaled(kSendWidth);
    const int top = client.bottom - row + Scaled(8);
    return {pad, top, client.right - 2 * pad - send_w,
            client.bottom - Scaled(8)};
  }

  void Layout() {
    if (!feed_) {
      return;
    }
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    const int band = CaptionHeight();
    const int pad = Scaled(kPad);
    const int row = Scaled(kInputRowHeight);
    const int send_w = Scaled(kSendWidth);
    // The feed fills from the band to the divider above the prompt row.
    ::MoveWindow(feed_, 0, band, client.right,
                 std::max<int>(0, client.bottom - row - band - 1), TRUE);
    const RECT well = InputWell();
    const int line = Scaled(20);
    ::MoveWindow(input_, well.left + Scaled(10),
                 (well.top + well.bottom - line) / 2,
                 std::max<int>(0, well.right - well.left - Scaled(20)), line,
                 TRUE);
    ::MoveWindow(send_, client.right - pad - send_w, well.top, send_w,
                 well.bottom - well.top, TRUE);
    Invalidate();
  }

  static LRESULT CALLBACK InputProc(HWND hwnd, UINT message, WPARAM w,
                                    LPARAM l) {
    auto* self = reinterpret_cast<Panel*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
      if (message == WM_KEYDOWN && w == VK_RETURN) {
        self->Send();
        return 0;
      }
      if (message == WM_CHAR && w == VK_RETURN) {
        return 0;  // No beep for the handled key.
      }
      return ::CallWindowProcW(self->input_base_proc_, hwnd, message, w, l);
    }
    return ::DefWindowProcW(hwnd, message, w, l);
  }

  ConsolePalette console_ = ConsoleFor(false);
  std::string task_id_;
  RAW_PTR_EXCLUSION PhiTranscriptHost* host_ = nullptr;
  HWND feed_ = nullptr;
  HWND input_ = nullptr;
  HWND send_ = nullptr;
  WNDPROC input_base_proc_ = nullptr;
  HFONT ui_font_ = nullptr;
  HBRUSH bg_brush_ = nullptr;
  HBRUSH feed_brush_ = nullptr;
  HBRUSH card_brush_ = nullptr;
  HBRUSH rule_brush_ = nullptr;
  size_t last_line_count_ = static_cast<size_t>(-1);
  bool open_dark_ = false;
  COLORREF open_accent_ = 0;
  bool followed_theme_ = false;
};

Panel* Panel::instance = nullptr;

}  // namespace

// static
void PhiTranscriptPanel::Toggle(HWND owner,
                                const PhiPalette& palette,
                                const std::string& task_id,
                                PhiTranscriptHost* host) {
  if (Panel::instance) {
    const bool same = Panel::instance->task_id() == task_id;
    Panel::instance->Destroy();  // Clears the singleton at WM_NCDESTROY.
    Panel::instance = nullptr;
    if (same) {
      return;  // The Mac toggle: reopening the shown task closes it.
    }
  }
  if (!owner || !host) {
    return;
  }
  auto* panel = new Panel(palette, task_id, host);
  panel->DeleteWhenDestroyed();
  Panel::instance = panel;
  if (!panel->Open(owner)) {
    Panel::instance = nullptr;
    delete panel;
  }
}

// static
void PhiTranscriptPanel::RefreshOpen() {
  if (Panel::instance) {
    Panel::instance->Refresh();
  }
}

// static
bool PhiTranscriptPanel::IsOpen() {
  return Panel::instance != nullptr;
}

// static
void PhiTranscriptPanel::CloseFor(const std::string& task_id) {
  if (Panel::instance && Panel::instance->task_id() == task_id) {
    Panel::instance->Destroy();
    Panel::instance = nullptr;
  }
}

}  // namespace phi_client

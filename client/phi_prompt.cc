// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_prompt.h"

#include <windowsx.h>

#include <algorithm>
#include <cwctype>
#include <functional>
#include <utility>

#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_modal_scope.h"
#include "phibrowser-win/client/phi_popup_chrome.h"
#include "phibrowser-win/client/phi_secondary_window.h"

namespace phi_client {

namespace {

constexpr wchar_t kPromptClass[] = L"PhiPrompt";

// PhiAlertLayout, in design units: a 465-wide surface with 24 of padding
// around 417 of content; the mark, 24 to the title, 16 to the copy, 36 to
// the actions row; 130x36 actions 8 apart.
// The ContentDialog (docs/windows-native-ui.md §6): 448 wide, body padding
// 24, title 20/600, 12 to the message, 16 to an accessory; a footer band
// with padding 24 and equal-width 32 dip buttons 8 apart.
constexpr int kWidth = 448;
constexpr int kPadding = 24;
constexpr int kContentWidth = kWidth - 2 * kPadding;  // 400
constexpr int kTitleHeight = 28;  // 20 dip, one line (a longer title wraps).
constexpr int kCopySpacing = 12;
constexpr int kAccessorySpacing = 16;
constexpr int kActionHeight = 32;
constexpr int kActionGap = 8;
constexpr int kFooterHeight = kPadding + kActionHeight + kPadding;
// The accessory wells: the Mac's 240x24 field grown to the alert's width.
constexpr int kFieldHeight = 32;
constexpr int kFieldInsetX = 10;
constexpr int kListRowHeight = 32;
constexpr int kListMaxRows = 8;
constexpr int kListInset = 4;
constexpr int kRecorderHeight = 32;
constexpr int kCheckboxHeight = 20;

constexpr UINT_PTR kControlId = 100;
constexpr UINT_PTR kActionIdFirst = 1000;

enum class Accessory { kNone, kField, kList, kRecorder, kCheckbox };

struct Action {
  std::wstring label;
  PhiPopupButtonRole role = PhiPopupButtonRole::kSecondary;
};

struct Spec {
  std::wstring title;
  std::wstring message;
  std::vector<Action> actions;
  int default_index = -1;
  int cancel_index = -1;
  // The action a list double-click fires. Normally the default action; the
  // security prompts make Cancel the default (Return denies) while a
  // double-click on a row still means "that row, confirmed".
  int list_activate_index = -1;
  Accessory accessory = Accessory::kNone;
  std::wstring field_text;      // kField's initial text.
  std::wstring placeholder;     // kRecorder's resting line.
  std::wstring checkbox_label;  // kCheckbox's text.
  bool checked = false;         // kCheckbox's initial state.
  std::vector<std::wstring> options;  // kList's rows.
  int selected = -1;                  // kList's preselected row.
  // kList drawn by the alert instead of the LISTBOX: a divider above a
  // section's first row and a trailing "Current" status. Parallel to
  // `options` when non-empty.
  std::vector<PhiChoiceRow> rows;
  // The security prompts: the confirming action (action 0, or a row
  // double-click) and the list selection it confirms count only when
  // genuine hardware input made them — see Alert::hardware_target_.
  bool hardware_confirm = false;
};

// Whether the input message just retrieved came from a real keyboard or
// mouse (or the system, e.g. accessibility input) rather than SendInput
// or a posted / sent message.
bool IsHardwareInput(const MSG& msg) {
  const bool input =
      (msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST) ||
      (msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST);
  INPUT_MESSAGE_SOURCE source = {};
  return input && ::GetCurrentInputMessageSource(&source) &&
         (source.originId == IMO_HARDWARE || source.originId == IMO_SYSTEM);
}

// One alert window: built from a Spec, run modally, read back.
//
// THE ONE SECONDARY WINDOW THAT MAY BLOCK. Every other surface in the
// client is asynchronous; this one keeps a nested GetMessage loop because
// PhiAlert / PhiConfirm / PhiPromptText / PhiPromptChoose /
// PhiPromptShortcut are called from a dozen places that need the answer on
// the same stack (see the note at the top of phi_prompt.h). It therefore
// registers NO dialog keys with the thread's active-dialog list
// (phi_secondary_window.h): its own loop calls IsDialogMessageW, and the
// chord recorder's `filter` must see raw key messages BEFORE the dialog
// manager does.
class Alert : public PhiSecondaryWindow {
 public:
  Alert(HWND owner, Spec spec) : owner_(owner), spec_(std::move(spec)) {}

  ~Alert() override {
    // A stack object, so the HWND has to go before the members do; the
    // base clears GWLP_USERDATA at WM_NCDESTROY, so nothing can reach us
    // afterwards.
    Destroy();
    for (HFONT font : {title_font_, body_font_, button_font_}) {
      if (font) {
        ::DeleteObject(font);
      }
    }
    for (HBRUSH brush : {card_brush_, ground_brush_}) {
      if (brush) {
        ::DeleteObject(brush);
      }
    }
  }

  // Created hidden at a placeholder size; Build measures the copy and
  // sizes the window to fit, then Run centres it on the owner.
  bool Open() {
    hardware_selection_ = spec_.selected;
    ClassSpec spec;
    spec.name = kPromptClass;
    Options options;
    // No caption band, no close button, no Phi mark (§6): the dialog is
    // its body and its footer; Esc cancels, Enter takes the default.
    options.caption_buttons = 0;
    options.caption_height_dip = 0;
    options.show_mark = false;
    options.dialog_keys = kPhiDialogNone;  // See the note above.
    const RECT bounds = {0, 0, 100, 100};
    if (!CreateSecondary(spec, WS_EX_DLGMODALFRAME,
                         WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                         bounds, owner_, spec_.title.c_str(), options)) {
      return false;
    }
    title_font_ = PhiPopupFont(dpi(), 20, FW_SEMIBOLD);
    body_font_ = PhiPopupFont(dpi(), 14);
    button_font_ = PhiPopupFont(dpi(), 14);
    OnColorsChanged();  // The two cached brushes.
    Build();
    return true;
  }

  HWND control() const { return control_; }
  bool done() const { return done_; }
  int chosen() const { return chosen_; }
  void Finish(int chosen) {
    chosen_ = chosen;
    done_ = true;
    // Wake the modal loop: a WM_COMMAND delivered via SendMessage from
    // outside the thread is processed INSIDE GetMessageW without making it
    // return, so `done` alone would leave the loop parked until unrelated
    // input.
    ::PostMessageW(hwnd(), WM_NULL, 0, 0);
  }

  // Shows the alert modally over its owner and pumps messages until an
  // action, Escape or the close button ends it. `filter` sees every message
  // first and returns true to swallow it (the chord recorder).
  int Run(const std::function<bool(const MSG&)>& filter = nullptr) {
    if (!alive()) {
      return -1;
    }
    CenterOn(owner_);
    HWND scrim = nullptr;
    if (owner_) {
      ::EnableWindow(owner_, FALSE);  // Modal against its owner, as NSAlert.
      scrim = CreateScrim(owner_);    // WinUI's smoke layer over the owner.
    }
    ::ShowWindow(hwnd(), SW_SHOW);
    ::SetForegroundWindow(hwnd());
    HWND focus = control_;
    if (!focus && spec_.default_index >= 0 &&
        static_cast<size_t>(spec_.default_index) < buttons_.size()) {
      focus = buttons_[static_cast<size_t>(spec_.default_index)];
    }
    if (!focus && !buttons_.empty()) {
      focus = buttons_.back();
    }
    ::SetFocus(focus ? focus : hwnd());
    if (control_ && spec_.accessory == Accessory::kField) {
      ::SendMessageW(control_, EM_SETSEL, 0, -1);
    }

    {
      // Every PhiPrompt*/PhiAlert*/PhiNotice/PhiConfirm caller runs its
      // nested loop here, so this ONE scope is what keeps a pipe verb from
      // destroying the controller (or rebuilding the rows) whose handler is
      // waiting on the answer.
      PhiModalScope modal;
      MSG msg;
      while (!done_ && ::GetMessageW(&msg, nullptr, 0, 0)) {
        if (filter && filter(msg)) {
          continue;
        }
        // Asked right after GetMessageW, while this message is the
        // thread's current input; the controls' notifications arrive
        // synchronously inside the dispatch below.
        hardware_target_ = spec_.hardware_confirm && IsHardwareInput(msg)
                               ? msg.hwnd
                               : nullptr;
        if (!::IsDialogMessageW(hwnd(), &msg)) {
          ::TranslateMessage(&msg);
          ::DispatchMessageW(&msg);
        }
        hardware_target_ = nullptr;
      }
    }
    if (scrim) {
      ::DestroyWindow(scrim);
    }
    if (owner_) {
      ::EnableWindow(owner_, TRUE);
      ::SetForegroundWindow(owner_);
    }
    return chosen_;
  }

  // The 30 % ink layer over the owner's client area for the dialog's
  // lifetime (docs/windows-native-ui.md §6): a layered popup owned by the
  // owner, activation-free, under the dialog.
  static HWND CreateScrim(HWND owner) {
    static bool registered = false;
    if (!registered) {
      registered = true;
      WNDCLASSW cls = {};
      cls.lpfnWndProc = ::DefWindowProcW;
      cls.hInstance = ::GetModuleHandleW(nullptr);
      cls.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
      cls.lpszClassName = L"PhiDialogScrim";
      ::RegisterClassW(&cls);
    }
    RECT client = {};
    if (!::GetClientRect(owner, &client) || ::IsIconic(owner)) {
      return nullptr;
    }
    ::MapWindowPoints(owner, nullptr, reinterpret_cast<POINT*>(&client), 2);
    HWND scrim = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"PhiDialogScrim", L"", WS_POPUP, client.left, client.top,
        client.right - client.left, client.bottom - client.top, owner, nullptr,
        ::GetModuleHandleW(nullptr), nullptr);
    if (!scrim) {
      return nullptr;
    }
    ::SetLayeredWindowAttributes(scrim, 0, 77, LWA_ALPHA);  // 30 %.
    ::ShowWindow(scrim, SW_SHOWNOACTIVATE);
    return scrim;
  }

  bool ControlChecked() const {
    return control_ &&
           ::SendMessageW(control_, BM_GETCHECK, 0, 0) == BST_CHECKED;
  }

  std::wstring ControlText() const {
    if (!control_) {
      return std::wstring();
    }
    const int length = ::GetWindowTextLengthW(control_);
    if (length <= 0) {
      return std::wstring();
    }
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    ::GetWindowTextW(control_, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
  }

  int ListSelection() const {
    if (!control_) {
      return -1;
    }
    if (spec_.hardware_confirm) {
      return hardware_selection_;  // ConfirmIsGenuine checked they agree.
    }
    const LRESULT sel = ::SendMessageW(control_, LB_GETCURSEL, 0, 0);
    return sel == LB_ERR ? -1 : static_cast<int>(sel);
  }

 private:
  HWND Child(const wchar_t* cls,
             const wchar_t* text,
             DWORD style,
             const RECT& rc,
             UINT_PTR id,
             HFONT font) {
    HWND control = ::CreateWindowExW(
        0, cls, text, WS_CHILD | WS_VISIBLE | style, rc.left, rc.top,
        rc.right - rc.left, rc.bottom - rc.top, hwnd(),
        reinterpret_cast<HMENU>(id), ::GetModuleHandleW(nullptr), nullptr);
    if (control && font) {
      ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font),
                     TRUE);
    }
    return control;
  }

  // Measures the copy, sizes the window and places the children.
  void Build() {
    const int x0 = Scaled(kPadding);
    const int content_w = Scaled(kContentWidth);
    int y = CaptionHeight() + Scaled(kPadding);

    // One line as a rule; a title longer than the content width wraps (the
    // ContentDialog's does) instead of losing its end to an ellipsis.
    int title_h = Scaled(kTitleHeight);
    {
      HDC dc = ::GetDC(hwnd());
      HGDIOBJ previous = title_font_ ? ::SelectObject(dc, title_font_) : nullptr;
      RECT measure = {0, 0, content_w, 0};
      ::DrawTextW(dc, spec_.title.c_str(), -1, &measure,
                  DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
      title_h = std::max(title_h, static_cast<int>(measure.bottom));
      if (previous) {
        ::SelectObject(dc, previous);
      }
      ::ReleaseDC(hwnd(), dc);
    }
    title_ = {x0, y, x0 + content_w, y + title_h};
    y += title_h;

    // The copy wraps at the content width; its height is the text's.
    int message_h = 0;
    if (!spec_.message.empty()) {
      HDC dc = ::GetDC(hwnd());
      HGDIOBJ previous = body_font_ ? ::SelectObject(dc, body_font_) : nullptr;
      RECT measure = {0, 0, content_w, 0};
      ::DrawTextW(dc, spec_.message.c_str(), -1, &measure,
                  DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
      message_h = measure.bottom;
      if (previous) {
        ::SelectObject(dc, previous);
      }
      ::ReleaseDC(hwnd(), dc);
      y += Scaled(kCopySpacing);
      message_ = {x0, y, x0 + content_w, y + message_h};
      y += message_h;
    }

    int well_h = 0;
    switch (spec_.accessory) {
      case Accessory::kNone:
        break;
      case Accessory::kField:
        well_h = Scaled(kFieldHeight);
        break;
      case Accessory::kList: {
        const int rows = std::clamp(static_cast<int>(spec_.options.size()),
                                    1, kListMaxRows);
        well_h = Scaled(rows * kListRowHeight + 2 * kListInset);
        break;
      }
      case Accessory::kRecorder:
        well_h = Scaled(kRecorderHeight);
        break;
      case Accessory::kCheckbox:
        well_h = Scaled(kCheckboxHeight);
        break;
    }
    if (well_h) {
      y += Scaled(kAccessorySpacing);
      well_ = {x0, y, x0 + content_w, y + well_h};
      y += well_h;
    }

    // The footer band: a separator, then the buttons on the footer ground.
    y += Scaled(kPadding);
    footer_top_ = y;
    actions_top_ = y + Scaled(kPadding);
    y += Scaled(kFooterHeight);

    // Size the window: the content plus the band and the thin borders.
    const int content_h = ::MulDiv(y - CaptionHeight(), 96,
                                   static_cast<int>(dpi()));
    const SIZE outer = chrome().OuterSize(kWidth, content_h);
    ::SetWindowPos(hwnd(), nullptr, 0, 0, outer.cx, outer.cy,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    BuildAccessory();
    BuildActions();
  }

  void BuildAccessory() {
    switch (spec_.accessory) {
      case Accessory::kNone:
        return;
      case Accessory::kField: {
        // The EDIT sits inset in the drawn well, borderless — the well's
        // stroke is the field's border.
        const int line = Scaled(20);
        RECT rc = {well_.left + Scaled(kFieldInsetX),
                   (well_.top + well_.bottom - line) / 2,
                   well_.right - Scaled(kFieldInsetX),
                   (well_.top + well_.bottom + line) / 2};
        control_ = Child(L"EDIT", spec_.field_text.c_str(),
                         WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT, rc,
                         kControlId, body_font_);
        break;
      }
      case Accessory::kList: {
        RECT rc = {well_.left + Scaled(kListInset), well_.top + Scaled(kListInset),
                   well_.right - Scaled(kListInset),
                   well_.bottom - Scaled(kListInset)};
        // Owner-drawn when the rows carry sections / status: the LISTBOX
        // keeps the strings (LBS_HASSTRINGS) so keyboard search and the
        // selection work unchanged; only the painting moves here.
        const DWORD drawn = spec_.rows.empty()
                                ? 0
                                : (LBS_OWNERDRAWFIXED | LBS_HASSTRINGS);
        control_ = Child(L"LISTBOX", L"",
                         WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                             LBS_NOINTEGRALHEIGHT | drawn,
                         rc, kControlId, button_font_);
        if (!control_) {
          return;
        }
        ::SendMessageW(control_, LB_SETITEMHEIGHT, 0, Scaled(kListRowHeight));
        for (const std::wstring& option : spec_.options) {
          ::SendMessageW(control_, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(option.c_str()));
        }
        if (spec_.selected >= 0 &&
            spec_.selected < static_cast<int>(spec_.options.size())) {
          ::SendMessageW(control_, LB_SETCURSEL,
                         static_cast<WPARAM>(spec_.selected), 0);
        }
        break;
      }
      case Accessory::kRecorder: {
        RECT rc = {well_.left + Scaled(2), well_.top + Scaled(2),
                   well_.right - Scaled(2), well_.bottom - Scaled(2)};
        control_ = Child(L"STATIC", spec_.placeholder.c_str(),
                         SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX, rc,
                         kControlId, body_font_);
        break;
      }
      case Accessory::kCheckbox:
        // The ContentDialog's CheckBox (the Mac's suppression button): a
        // plain row on the body, no well.
        control_ = Child(L"BUTTON", spec_.checkbox_label.c_str(),
                         WS_TABSTOP | BS_AUTOCHECKBOX, well_, kControlId,
                         body_font_);
        if (control_ && spec_.checked) {
          ::SendMessageW(control_, BM_SETCHECK, BST_CHECKED, 0);
        }
        break;
    }
    PhiPopupThemeControl(control_, colors().dark);
  }

  void BuildActions() {
    // Left to right in declaration order — the affirmative first, Cancel
    // last (Windows' order, D2) — each an equal share of the row.
    const size_t n = spec_.actions.size();
    if (n == 0) {
      return;
    }
    const int h = Scaled(kActionHeight);
    const int gap = Scaled(kActionGap);
    const int left = Scaled(kPadding);
    const int content_w = Scaled(kContentWidth);
    const int w = (content_w - gap * static_cast<int>(n - 1)) / static_cast<int>(n);
    buttons_.assign(n, nullptr);
    int x = left;
    for (size_t i = 0; i < n; ++i) {
      const int right = i + 1 == n ? left + content_w : x + w;
      RECT rc = {x, actions_top_, right, actions_top_ + h};
      buttons_[i] = Child(L"BUTTON", spec_.actions[i].label.c_str(),
                          WS_TABSTOP | BS_PUSHBUTTON, rc, kActionIdFirst + i,
                          button_font_);
      PhiPopupStyleButton(buttons_[i], spec_.actions[i].role);
      x += w + gap;
    }
  }

  void Paint(HDC dc, const RECT&) override {
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    // The body on `card`, the footer band on the dialog-footer ground with
    // a 1 px separator between them (§6).
    RECT body = {client.left, client.top, client.right, footer_top_};
    ::FillRect(dc, &body, card_brush_);
    RECT footer = {client.left, footer_top_, client.right, client.bottom};
    ::FillRect(dc, &footer, ground_brush_);
    RECT rule = {client.left, footer_top_, client.right, footer_top_ + 1};
    HBRUSH separator = ::CreateSolidBrush(colors().separator);
    ::FillRect(dc, &rule, separator);
    ::DeleteObject(separator);
    ::SetBkMode(dc, TRANSPARENT);
    HGDIOBJ previous = title_font_ ? ::SelectObject(dc, title_font_) : nullptr;
    ::SetTextColor(dc, colors().label_strong);
    RECT title = title_;
    const bool one_line = title.bottom - title.top <= Scaled(kTitleHeight);
    ::DrawTextW(dc, spec_.title.c_str(), -1, &title,
                DT_LEFT | DT_NOPREFIX |
                    (one_line ? DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS
                              : DT_WORDBREAK));
    if (!spec_.message.empty()) {
      if (body_font_) {
        ::SelectObject(dc, body_font_);
      }
      ::SetTextColor(dc, colors().label);
      RECT message = message_;
      ::DrawTextW(dc, spec_.message.c_str(), -1, &message,
                  DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }
    if (spec_.accessory != Accessory::kNone &&
        spec_.accessory != Accessory::kCheckbox) {
      PhiPopupPaintWell(dc, well_, colors(), dpi());
    }
    if (previous) {
      ::SelectObject(dc, previous);
    }
  }

  // The alert never lets the base's Escape / Return defaults fire: its
  // modal loop hands both to IsDialogMessageW, which turns them into
  // IDCANCEL / the default action below.
  UINT DlgCode() const override { return 0; }

  LRESULT OnWindowMessage(UINT msg,
                          WPARAM wparam,
                          LPARAM lparam,
                          bool* handled) override {
    switch (msg) {
      case DM_GETDEFID:
        // IsDialogMessage's Return: the default action, or none.
        *handled = true;
        if (spec_.default_index >= 0) {
          return MAKELRESULT(kActionIdFirst + spec_.default_index,
                             DC_HASDEFID);
        }
        return 0;
      case WM_COMMAND: {
        const UINT id = LOWORD(wparam);
        const UINT code = HIWORD(wparam);
        if (id >= kActionIdFirst &&
            id < kActionIdFirst + spec_.actions.size() &&
            (code == BN_CLICKED || code == 0)) {
          const int index = static_cast<int>(id - kActionIdFirst);
          // A security prompt's confirming button answers only a real
          // click or key press on it — never BM_CLICK, UI Automation's
          // Invoke or SendInput from another program. Cancel always works.
          if (index == 0 && spec_.hardware_confirm &&
              !ConfirmIsGenuine(buttons_[0])) {
            *handled = true;
            return 0;
          }
          Finish(index);
          *handled = true;
          return 0;
        }
        if (id == kControlId && code == LBN_SELCHANGE &&
            spec_.hardware_confirm && hardware_target_ == control_) {
          hardware_selection_ = static_cast<int>(
              ::SendMessageW(control_, LB_GETCURSEL, 0, 0));
        }
        if (id == IDOK) {
          // Return with no default action registered: nothing happens,
          // the Mac's `.noButton` mode.
          if (spec_.default_index >= 0) {
            Finish(spec_.default_index);
          }
          *handled = true;
          return 0;
        }
        if (id == IDCANCEL) {
          Finish(spec_.cancel_index);
          *handled = true;
          return 0;
        }
        if (id == kControlId && code == LBN_DBLCLK) {
          const int activate = spec_.list_activate_index >= 0
                                   ? spec_.list_activate_index
                                   : spec_.default_index;
          if (activate == 0 && spec_.hardware_confirm &&
              !ConfirmIsGenuine(control_)) {
            *handled = true;
            return 0;
          }
          if (activate >= 0) {
            Finish(activate);  // A double-click confirms the row.
          }
          *handled = true;
          return 0;
        }
        break;
      }
      case WM_DRAWITEM: {
        const DRAWITEMSTRUCT& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (item.CtlType == ODT_LISTBOX && item.CtlID == kControlId) {
          DrawRow(item);
          *handled = true;
          return TRUE;
        }
        if (PhiPopupDrawButton(item, colors(), button_font_)) {
          *handled = true;
          return TRUE;
        }
        break;
      }
      case WM_CTLCOLOREDIT:
      case WM_CTLCOLORLISTBOX:
      case WM_CTLCOLORSTATIC: {
        // The accessory paints on the well's card colour.
        HDC dc = reinterpret_cast<HDC>(wparam);
        ::SetTextColor(dc, colors().label);
        ::SetBkColor(dc, colors().card);
        *handled = true;
        return reinterpret_cast<LRESULT>(card_brush_);
      }
      case WM_CLOSE:
        Finish(spec_.cancel_index);
        *handled = true;
        return 0;
      default:
        break;
    }
    return PhiSecondaryWindow::OnWindowMessage(msg, wparam, lparam, handled);
  }

  // hardware_confirm: the confirming notification is being raised by
  // hardware input aimed at `source` (the button, or the list for a
  // double-click), and the list still shows the rows and the selection the
  // user made by hand. A tampered list (LB_SETCURSEL, rows swapped) is put
  // back to the user's choice and the confirmation ignored.
  bool ConfirmIsGenuine(HWND source) {
    if (!source || hardware_target_ != source) {
      return false;
    }
    if (spec_.accessory != Accessory::kList || !control_) {
      return true;
    }
    bool intact = ::SendMessageW(control_, LB_GETCOUNT, 0, 0) ==
                  static_cast<LRESULT>(spec_.options.size());
    for (size_t i = 0; intact && i < spec_.options.size(); ++i) {
      const LRESULT length =
          ::SendMessageW(control_, LB_GETTEXTLEN, static_cast<WPARAM>(i), 0);
      if (length == LB_ERR ||
          static_cast<size_t>(length) != spec_.options[i].size()) {
        intact = false;
        break;
      }
      std::wstring text(static_cast<size_t>(length) + 1, L'\0');
      ::SendMessageW(control_, LB_GETTEXT, static_cast<WPARAM>(i),
                     reinterpret_cast<LPARAM>(text.data()));
      text.resize(static_cast<size_t>(length));
      intact = text == spec_.options[i];
    }
    if (intact && ::SendMessageW(control_, LB_GETCURSEL, 0, 0) ==
                      static_cast<LRESULT>(hardware_selection_)) {
      return true;
    }
    if (intact) {
      ::SendMessageW(control_, LB_SETCURSEL,
                     static_cast<WPARAM>(hardware_selection_), 0);
    }
    return false;
  }

  // One owner-drawn chooser row: the selection as the Mac's accent wash,
  // a hairline above a section's first row, the label, and the "Current"
  // status in the secondary ink at the trailing edge.
  void DrawRow(const DRAWITEMSTRUCT& item) {
    if (item.itemID == static_cast<UINT>(-1) ||
        item.itemID >= spec_.rows.size()) {
      ::FillRect(item.hDC, &item.rcItem, card_brush_);
      return;
    }
    const PhiChoiceRow& row = spec_.rows[item.itemID];
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    RECT rc = item.rcItem;
    ::FillRect(item.hDC, &rc, card_brush_);
    if (selected) {
      RECT wash = rc;
      wash.left += Scaled(2);
      wash.right -= Scaled(2);
      PhiFillRoundRectAA(item.hDC, wash, Scaled(4),
                         PhiPopupBlend(colors().accent, colors().card, 46));
    }
    if (row.starts_section && item.itemID > 0) {
      RECT line = {rc.left + Scaled(6), rc.top, rc.right - Scaled(6),
                   rc.top + 1};
      HBRUSH separator = ::CreateSolidBrush(colors().separator);
      ::FillRect(item.hDC, &line, separator);
      ::DeleteObject(separator);
    }
    ::SetBkMode(item.hDC, TRANSPARENT);
    HGDIOBJ previous =
        button_font_ ? ::SelectObject(item.hDC, button_font_) : nullptr;
    RECT status = rc;
    status.right -= Scaled(8);
    int status_w = 0;
    if (row.current) {
      const wchar_t* current = PhiStr("sidebar.spaceChooser.currentStatus");
      RECT measure = {0, 0, 0, 0};
      ::DrawTextW(item.hDC, current, -1, &measure,
                  DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
      status_w = measure.right;
      ::SetTextColor(item.hDC, colors().secondary);
      ::DrawTextW(item.hDC, current, -1, &status,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    RECT label = rc;
    label.left += Scaled(8);
    label.right -= Scaled(8) + status_w + (status_w ? Scaled(12) : 0);
    ::SetTextColor(item.hDC, colors().label);
    ::DrawTextW(item.hDC, row.label.c_str(), -1, &label,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                    DT_END_ELLIPSIS);
    if (previous) {
      ::SelectObject(item.hDC, previous);
    }
    if ((item.itemState & ODS_FOCUS) && selected) {
      ::DrawFocusRect(item.hDC, &rc);
    }
  }

  void OnColorsChanged() override {
    for (HBRUSH* slot : {&card_brush_, &ground_brush_}) {
      if (*slot) {
        ::DeleteObject(*slot);
        *slot = nullptr;
      }
    }
    card_brush_ = ::CreateSolidBrush(colors().card);
    ground_brush_ = ::CreateSolidBrush(colors().bg);
  }

  HWND owner_ = nullptr;
  Spec spec_;
  HFONT title_font_ = nullptr;
  HFONT body_font_ = nullptr;
  HFONT button_font_ = nullptr;
  // The alert's ground was a CreateSolidBrush / DeleteObject pair on
  // every paint (lane 3 ranked finding 12).
  HBRUSH card_brush_ = nullptr;
  HBRUSH ground_brush_ = nullptr;
  HWND control_ = nullptr;
  std::vector<HWND> buttons_;
  RECT title_ = {};
  RECT message_ = {};
  RECT well_ = {};
  int footer_top_ = 0;
  int actions_top_ = 0;
  bool done_ = false;
  int chosen_ = -1;
  // hardware_confirm: the window the input message being dispatched was
  // aimed at when it came from real hardware (null otherwise, and between
  // messages), and the list row the user last picked by hand.
  HWND hardware_target_ = nullptr;
  int hardware_selection_ = -1;
};

PhiPopupButtonRole RoleFor(PhiAlertRole role) {
  switch (role) {
    case PhiAlertRole::kPrimary:
      return PhiPopupButtonRole::kPrimary;
    case PhiAlertRole::kDestructive:
      return PhiPopupButtonRole::kDestructive;
    case PhiAlertRole::kSecondary:
      break;
  }
  return PhiPopupButtonRole::kSecondary;
}

}  // namespace

int PhiAlert(HWND owner,
             const wchar_t* title,
             const wchar_t* message,
             const std::vector<PhiAlertAction>& actions,
             int default_index,
             int cancel_index) {
  Spec spec;
  spec.title = title ? title : L"";
  spec.message = message ? message : L"";
  for (const PhiAlertAction& action : actions) {
    spec.actions.push_back({action.label, RoleFor(action.role)});
  }
  spec.default_index =
      default_index < static_cast<int>(actions.size()) ? default_index : -1;
  spec.cancel_index =
      cancel_index < static_cast<int>(actions.size()) ? cancel_index : -1;
  Alert alert(owner, std::move(spec));
  return alert.Open() ? alert.Run() : -1;
}

int PhiAlertWithCheckbox(HWND owner,
                         const wchar_t* title,
                         const wchar_t* message,
                         const std::vector<PhiAlertAction>& actions,
                         const wchar_t* checkbox_label,
                         bool* checked,
                         int default_index,
                         int cancel_index) {
  Spec spec;
  spec.title = title ? title : L"";
  spec.message = message ? message : L"";
  for (const PhiAlertAction& action : actions) {
    spec.actions.push_back({action.label, RoleFor(action.role)});
  }
  spec.default_index =
      default_index < static_cast<int>(actions.size()) ? default_index : -1;
  spec.cancel_index =
      cancel_index < static_cast<int>(actions.size()) ? cancel_index : -1;
  spec.accessory = Accessory::kCheckbox;
  spec.checkbox_label = checkbox_label ? checkbox_label : L"";
  spec.checked = checked && *checked;
  Alert alert(owner, std::move(spec));
  if (!alert.Open()) {
    return -1;
  }
  const int chosen = alert.Run();
  if (checked) {
    *checked = alert.ControlChecked();
  }
  return chosen;
}

void PhiNotice(HWND owner, const wchar_t* title, const wchar_t* message) {
  PhiAlert(owner, title, message,
           {{PhiStr("prompt.ok"), PhiAlertRole::kPrimary}}, 0, 0);
}

size_t PhiCodePointCount(const std::wstring& text) {
  size_t count = 0;
  for (const wchar_t c : text) {
    // A low surrogate is the second half of a pair already counted.
    if (c < 0xDC00 || c > 0xDFFF) {
      ++count;
    }
  }
  return count;
}

std::wstring PhiTrimWhitespace(const std::wstring& text) {
  size_t first = 0;
  size_t last = text.size();
  while (first < last && std::iswspace(text[first])) {
    ++first;
  }
  while (last > first && std::iswspace(text[last - 1])) {
    --last;
  }
  return text.substr(first, last - first);
}

bool PhiPromptText(HWND owner,
                   const wchar_t* title,
                   const wchar_t* message,
                   std::wstring* value,
                   const wchar_t* confirm_label) {
  return PhiPromptText(owner, title, message, value, confirm_label,
                       PhiTextRules());
}

bool PhiPromptText(HWND owner,
                   const wchar_t* title,
                   const wchar_t* message,
                   std::wstring* value,
                   const wchar_t* confirm_label,
                   const PhiTextRules& rules) {
  std::wstring text = value ? *value : std::wstring();
  // A name that breaks a rule asks AGAIN, the typed text kept, rather than
  // returning a value the caller would only refuse after the fact.
  for (;;) {
    Spec spec;
    spec.title = title ? title : L"";
    spec.message = message ? message : L"";
    spec.accessory = Accessory::kField;
    spec.field_text = text;
    spec.actions = {{confirm_label ? confirm_label : PhiStr("prompt.ok"),
                     PhiPopupButtonRole::kPrimary},
                    {PhiStr("prompt.cancel"), PhiPopupButtonRole::kSecondary}};
    spec.default_index = 0;
    spec.cancel_index = 1;
    Alert alert(owner, std::move(spec));
    if (!alert.Open()) {
      return false;
    }
    const bool accepted = alert.Run() == 0;
    std::wstring entered = alert.ControlText();
    if (rules.trim) {
      entered = PhiTrimWhitespace(entered);
    }
    if (!accepted || entered.empty()) {
      return false;
    }
    if (rules.max_code_points > 0 &&
        PhiCodePointCount(entered) > rules.max_code_points) {
      PhiNotice(
          owner,
          PhiStr("settings.account.renameDialog.nameTooLongError.title"),
          PhiStrF("settings.account.renameDialog.nameTooLongError.message",
                  {PhiFormatNumber(
                      static_cast<long long>(rules.max_code_points))})
              .c_str());
      text = entered;
      continue;
    }
    if (value) {
      *value = entered;
    }
    return true;
  }
}

bool PhiConfirm(HWND owner,
                const wchar_t* title,
                const wchar_t* message,
                const wchar_t* confirm_label,
                bool destructive) {
  // The confirming button answers Return only when it is not destructive —
  // an irreversible action wants a deliberate click, the Mac's noButton.
  // The affirmative leads (Windows' order, D2); a destructive one is never
  // the default — Return is inert, Esc cancels (the Mac's noButton rule).
  return PhiAlert(owner, title, message,
                  {{confirm_label ? confirm_label : PhiStr("prompt.ok"),
                    destructive ? PhiAlertRole::kDestructive
                                : PhiAlertRole::kPrimary},
                   {PhiStr("prompt.cancel"), PhiAlertRole::kSecondary}},
                  destructive ? -1 : 0, 1) == 0;
}

std::wstring PhiFormatChord(const PhiShortcutChord& chord) {
  if (!chord.vk) {
    return L"\u2014";
  }
  std::wstring text;
  if (chord.ctrl) {
    text += L"Ctrl+";
  }
  if (chord.alt) {
    text += L"Alt+";
  }
  if (chord.shift) {
    text += L"Shift+";
  }
  const UINT vk = chord.vk;
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
    text += static_cast<wchar_t>(vk);
    return text;
  }
  if (vk >= VK_F1 && vk <= VK_F24) {
    text += L"F" + std::to_wstring(vk - VK_F1 + 1);
    return text;
  }
  switch (vk) {
    case VK_LEFT:   return text + L"Left";
    case VK_RIGHT:  return text + L"Right";
    case VK_UP:     return text + L"Up";
    case VK_DOWN:   return text + L"Down";
    case VK_HOME:   return text + L"Home";
    case VK_END:    return text + L"End";
    case VK_PRIOR:  return text + L"PageUp";
    case VK_NEXT:   return text + L"PageDown";
    case VK_SPACE:  return text + L"Space";
    case VK_RETURN: return text + L"Enter";
    case VK_BACK:   return text + L"Backspace";
    case VK_DELETE: return text + L"Delete";
    case VK_INSERT: return text + L"Insert";
    case VK_TAB:    return text + L"Tab";
    // The zoom keys as the ⋯ menu spells them ("Ctrl++"), not the US
    // layout's unshifted "=".
    case VK_OEM_PLUS:  return text + L"+";
    case VK_OEM_MINUS: return text + L"-";
  }
  // Punctuation and everything else — the layout's character for the key.
  const UINT ch = ::MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0xFFFF;
  if (ch) {
    text += static_cast<wchar_t>(ch);
  } else {
    text += L"Key" + std::to_wstring(vk);
  }
  return text;
}

bool PhiPromptShortcut(HWND owner,
                       const wchar_t* title,
                       const wchar_t* message,
                       PhiShortcutChord* out) {
  const std::wstring resting = PhiStr("prompt.shortcut.placeholder");
  Spec spec;
  spec.title = title ? title : L"";
  spec.message = message ? message : L"";
  spec.accessory = Accessory::kRecorder;
  spec.placeholder = resting;
  spec.actions = {{PhiStr("prompt.cancel"), PhiPopupButtonRole::kSecondary}};
  spec.cancel_index = 0;
  Alert alert(owner, std::move(spec));
  if (!alert.Open()) {
    return false;
  }
  HWND preview = alert.control();

  const auto is_modifier = [](UINT vk) {
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_LWIN || vk == VK_RWIN;
  };
  const auto held_prefix = [&] {
    std::wstring text;
    if (::GetKeyState(VK_CONTROL) & 0x8000) {
      text += L"Ctrl+";
    }
    if (::GetKeyState(VK_MENU) & 0x8000) {
      text += L"Alt+";
    }
    if (::GetKeyState(VK_SHIFT) & 0x8000) {
      text += L"Shift+";
    }
    return text.empty() ? resting : text + L"\u2026";
  };
  // Chords a shortcut may not take, with the reason the preview shows.
  // Ctrl+C / V / X / Z / Y / A (Shift or not) are every text field's
  // editing keys; a shortcut on one would swallow them on every page. And
  // Ctrl+Alt on a character key IS AltGr on many keyboard layouts (German
  // @, Polish ą, …): bound, it would stop people typing that character.
  const auto refusal = [](const PhiShortcutChord& chord) -> std::wstring {
    const UINT vk = chord.vk;
    if (chord.ctrl && !chord.alt &&
        (vk == 'C' || vk == 'V' || vk == 'X' || vk == 'Z' || vk == 'Y' ||
         vk == 'A')) {
      return PhiStrF("prompt.shortcut.reservedForEditing",
                     {PhiFormatChord(chord)});
    }
    const bool types_character =
        (::MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0xFFFF) != 0 &&
        vk != VK_SPACE && vk != VK_RETURN && vk != VK_TAB && vk != VK_BACK;
    if (chord.ctrl && chord.alt && types_character) {
      return PhiStrF("prompt.shortcut.reservedForAltGr",
                     {PhiFormatChord(chord)});
    }
    return std::wstring();
  };

  PhiShortcutChord captured;
  bool accepted = false;
  // The recorder itself: every key in the modal loop is a candidate.
  // WM_SYSKEYDOWN carries the Alt chords; consuming it also keeps
  // DefWindowProc out of the menu loop.
  const auto filter = [&](const MSG& msg) {
    if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
      const UINT vk = static_cast<UINT>(msg.wParam);
      const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
      const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
      const bool alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
      if (is_modifier(vk)) {
        ::SetWindowTextW(preview, held_prefix().c_str());
        return true;
      }
      if (vk == VK_ESCAPE && !ctrl && !alt) {
        alert.Finish(0);
        return true;
      }
      if ((vk == VK_DELETE || vk == VK_BACK) && !ctrl && !alt) {
        // Bare Delete (or Backspace) REMOVES the shortcut — the Mac
        // recorder's Delete; the caller stores the unbound chord (vk 0).
        captured = {};
        accepted = true;
        alert.Finish(-1);
        return true;
      }
      if (ctrl || alt || (vk >= VK_F1 && vk <= VK_F24)) {
        const PhiShortcutChord chord = {vk, ctrl, shift, alt};
        const std::wstring refused = refusal(chord);
        if (!refused.empty()) {
          // Not taken; the recorder keeps listening for another chord.
          ::SetWindowTextW(preview, refused.c_str());
          return true;
        }
        captured = chord;
        accepted = true;
        alert.Finish(-1);
        return true;
      }
      // A bare key: browsers require a modifier so pages keep their typing.
      ::SetWindowTextW(preview, PhiStr("prompt.shortcut.needsModifier"));
      return true;
    }
    if (msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP) {
      if (is_modifier(static_cast<UINT>(msg.wParam))) {
        ::SetWindowTextW(preview, held_prefix().c_str());
      }
      return true;
    }
    return false;
  };
  alert.Run(filter);
  if (!accepted) {
    return false;
  }
  if (out) {
    *out = captured;
  }
  return true;
}

int PhiPromptChoose(HWND owner,
                    const wchar_t* title,
                    const wchar_t* message,
                    const std::vector<std::wstring>& options,
                    int default_index,
                    const wchar_t* confirm_label,
                    const wchar_t* cancel_label,
                    bool cancel_is_default,
                    bool require_hardware_input) {
  Spec spec;
  spec.title = title ? title : L"";
  spec.message = message ? message : L"";
  spec.accessory = Accessory::kList;
  spec.options = options;
  spec.hardware_confirm = require_hardware_input;
  spec.selected = default_index;
  spec.actions = {{confirm_label ? confirm_label : PhiStr("prompt.open"),
                   PhiPopupButtonRole::kPrimary},
                  {cancel_label ? cancel_label : PhiStr("prompt.keepHere"),
                   PhiPopupButtonRole::kSecondary}};
  spec.default_index = cancel_is_default ? 1 : 0;
  spec.cancel_index = 1;
  spec.list_activate_index = 0;
  Alert alert(owner, std::move(spec));
  if (!alert.Open()) {
    return -1;
  }
  const bool accepted = alert.Run() == 0;
  return accepted ? alert.ListSelection() : -1;
}

int PhiPromptChooseRows(HWND owner,
                        const wchar_t* title,
                        const wchar_t* message,
                        const std::vector<PhiChoiceRow>& rows,
                        int default_index,
                        const wchar_t* confirm_label,
                        const wchar_t* cancel_label,
                        bool cancel_is_default) {
  Spec spec;
  spec.title = title ? title : L"";
  spec.message = message ? message : L"";
  spec.accessory = Accessory::kList;
  spec.rows = rows;
  for (const PhiChoiceRow& row : rows) {
    spec.options.push_back(row.label);
  }
  spec.selected = default_index;
  spec.actions = {{confirm_label ? confirm_label : PhiStr("prompt.open"),
                   PhiPopupButtonRole::kPrimary},
                  {cancel_label ? cancel_label : PhiStr("prompt.keepHere"),
                   PhiPopupButtonRole::kSecondary}};
  spec.default_index = cancel_is_default ? 1 : 0;
  spec.cancel_index = 1;
  spec.list_activate_index = 0;
  Alert alert(owner, std::move(spec));
  if (!alert.Open()) {
    return -1;
  }
  const bool accepted = alert.Run() == 0;
  return accepted ? alert.ListSelection() : -1;
}

}  // namespace phi_client

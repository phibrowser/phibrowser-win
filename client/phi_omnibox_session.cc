// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_omnibox_session.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <imm.h>
#include <windowsx.h>

#include <algorithm>
#include <iterator>
#include <optional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_panel.h"
#include "phibrowser-win/client/phi_shell_menus.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "imm32.lib")

namespace phi_client {

namespace {

// Omnibox suggestion dropdown metrics (DIP).
constexpr int kRowHeight = 32;
constexpr int kMaxRows = 8;
constexpr int kPopupPad = 12;   // The list's total vertical padding.
constexpr int kPopupInset = 6;  // Half of it, above the first row.

// The EDIT class's own window procedure, captured the first time a field is
// subclassed. PROCESS-WIDE on purpose: every address field in every shell
// window is an EDIT, so they all chain to the same base proc, and the value
// is a constant of the class rather than state of any one window. It is a
// plain pointer with no destructor, so it costs nothing at exit.
WNDPROC g_edit_base_proc = nullptr;

// Reads the whole text of an EDIT.
std::wstring TextOf(HWND edit) {
  const int len = ::GetWindowTextLengthW(edit);
  std::vector<wchar_t> buffer(static_cast<size_t>(len) + 1, L'\0');
  if (len > 0) {
    ::GetWindowTextW(edit, buffer.data(), len + 1);
  }
  return std::wstring(buffer.data());
}

// The clipboard's Unicode text, or empty.
std::wstring ClipboardText(HWND owner) {
  std::wstring text;
  if (::OpenClipboard(owner)) {
    if (HANDLE data = ::GetClipboardData(CF_UNICODETEXT)) {
      if (const wchar_t* chars =
              static_cast<const wchar_t*>(::GlobalLock(data))) {
        text = chars;
        ::GlobalUnlock(data);
      }
    }
    ::CloseClipboard();
  }
  return text;
}

RECT ToRect(const SidebarRect& r) {
  return RECT{r.left, r.top, r.right, r.bottom};
}

LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
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
  auto* self = reinterpret_cast<PhiOmniboxSession*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (self) {
        self->PaintPopup(hwnd, dc);
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_MOUSEMOVE:
      if (self) {
        // The hover wash leaves with the pointer (WM_MOUSELEAVE below).
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&track);
        self->PopupHover(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      }
      return 0;
    case WM_MOUSELEAVE:
      if (self) {
        self->PopupHover(-1, -1);
      }
      return 0;
    case WM_LBUTTONDOWN:
      if (self) {
        self->PopupClick(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      }
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

ATOM EnsurePopupClass() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &PopupProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PhiOmniboxPopup";
    return ::RegisterClassExW(&wc);
  }();
  return atom;
}

}  // namespace

// --- Lifetime ------------------------------------------------------------

PhiOmniboxSession::PhiOmniboxSession(PhiOmniboxShell* shell) : shell_(shell) {}

PhiOmniboxSession::~PhiOmniboxSession() {
  // The EDIT is a child of the shell window and dies with it; the popup is
  // a top-level window of ours, so it goes here.
  if (popup_ && ::IsWindow(popup_)) {
    ::DestroyWindow(popup_);
  }
  popup_ = nullptr;
  if (brush_) {
    ::DeleteObject(brush_);
  }
}

int PhiOmniboxSession::Scaled(int design_units) const {
  return shell_->OmniboxScaled(design_units);
}

// --- The address field ---------------------------------------------------

void PhiOmniboxSession::EnsureField() {
  const HWND shell_hwnd = shell_->OmniboxShellWindow();
  if (field_ || !shell_hwnd) {
    return;
  }
  field_ = ::CreateWindowExW(
      0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL, 0, 0,
      10, 10, shell_hwnd, nullptr, ::GetModuleHandleW(nullptr), nullptr);
  if (!field_) {
    LOG(ERROR) << "[PhiClient] address field CreateWindowEx failed err="
               << ::GetLastError();
    return;
  }
  if (HFONT font = shell_->OmniboxTextFont()) {
    ::SendMessageW(field_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  }
  // The Mac's placeholder, shown by the EDIT itself while empty (and by the
  // capture path, which renders without children).
  ::SendMessageW(field_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(
                     PhiStr("addressBar.configuration.defaultPlaceholder")));
  AttachField(field_);
  Position();
}

void PhiOmniboxSession::AttachField(HWND edit) {
  if (!edit || !::IsWindow(edit)) {
    return;
  }
  ::SetWindowLongPtrW(edit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  WNDPROC previous = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
      edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&FieldProc)));
  if (!g_edit_base_proc) {
    g_edit_base_proc = previous;  // The EDIT class proc — one for all fields.
  }
}

HWND PhiOmniboxSession::FocusedField() const {
  const HWND focus = ::GetFocus();
  if (!focus) {
    return nullptr;
  }
  if (field_ && focus == field_) {
    return field_;
  }
  const HWND native = shell_->OmniboxNativePageField();
  if (native && focus == native) {
    return native;
  }
  return nullptr;
}

bool PhiOmniboxSession::FieldHasFocus() const {
  return field_ && ::GetFocus() == field_;
}

void PhiOmniboxSession::SetFont(HFONT font) {
  if (field_ && font) {
    ::SendMessageW(field_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  }
}

void PhiOmniboxSession::Position() {
  if (!field_) {
    return;
  }
  // Sits INSIDE the painted field box, vertically centred.
  const RECT target = ToRect(PhiOmniboxTextRect(
      shell_->OmniboxCapsule(), /*text_height=*/Scaled(20),
      // SideAddressBar: text starts 12 in from the pill's leading edge, 8
      // clear before the in-field trailing stack (or the bare trailing edge
      // without one).
      /*leading_inset=*/Scaled(12), /*icon_gap=*/Scaled(8),
      /*trailing_gap=*/Scaled(8), KeywordLeadingAdvance(),
      KeywordTrailingAdvance()));
  // Idempotent, because the paint path calls this too: the in-field
  // accessory set changes with the page — the reader button comes and goes —
  // and the EDIT is a real child window, so a stale width paints straight
  // over whatever just appeared beside it. Moving only on a real change
  // keeps that call from feeding itself more paints.
  RECT placed = target;
  const HWND parent = shell_->OmniboxShellWindow();
  RECT current = {};
  if (::GetWindowRect(field_, &current) &&
      ::MapWindowPoints(HWND_DESKTOP, parent,
                        reinterpret_cast<POINT*>(&current), 2) &&
      ::EqualRect(&current, &placed)) {
    return;
  }
  ::MoveWindow(field_, placed.left, placed.top, placed.right - placed.left,
               placed.bottom - placed.top, TRUE);
}

void PhiOmniboxSession::SetFieldVisible(bool visible) {
  if (field_) {
    ::ShowWindow(field_, visible ? SW_SHOW : SW_HIDE);
  }
}


void PhiOmniboxSession::Sync(bool force) {
  if (!field_) {
    return;
  }
  // Never overwrite a half-typed URL: nav state refreshes on every paint and
  // on every framework notification.
  if (!force && ::GetFocus() == field_) {
    return;
  }
  // Chromium's own pages read phi:// (the Mac's prefill goes through
  // phiBrandEnsuredUrlString); the fork navigates phi:// back.
  const std::wstring shown = PhiOmniboxBrandUrl(nav_url_);
  if (shown == TextOf(field_)) {
    return;  // Unchanged; setting it again would fight the caret.
  }
  programmatic_ = true;  // Not the user typing — no autocomplete.
  ::SetWindowTextW(field_, shown.c_str());
  programmatic_ = false;
}

void PhiOmniboxSession::CommitField() {
  Commit(field_);
}

void PhiOmniboxSession::ShowFieldMenu(HWND edit, POINT screen) {
  DWORD sel_start = 0;
  DWORD sel_end = 0;
  ::SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_start),
                 reinterpret_cast<LPARAM>(&sel_end));
  const bool has_selection = sel_end > sel_start;
  const bool has_text = ::GetWindowTextLengthW(edit) > 0;
  const bool clipboard_text =
      ::IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE;
  const bool can_undo = ::SendMessageW(edit, EM_CANUNDO, 0, 0) != 0;
  PhiMenuSpec spec = PhiBuildOmniboxFieldMenu(has_selection, has_text,
                                              clipboard_text, can_undo);
  menu_palette_ = shell_->OmniboxPalette();
  PhiPopupMenuRequest request;
  request.owner = shell_->OmniboxShellWindow();
  request.x = screen.x;
  request.y = screen.y;
  request.align = TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON;
  request.palette = &menu_palette_;
  const PhiMenuAction action = PhiRunPopupMenu(spec, request);
  if (!::IsWindow(edit)) {
    return;
  }
  if (action.is("edit.undo")) {
    ::SendMessageW(edit, WM_UNDO, 0, 0);
  } else if (action.is("edit.cut")) {
    ::SendMessageW(edit, WM_CUT, 0, 0);
  } else if (action.is("edit.copy")) {
    ::SendMessageW(edit, WM_COPY, 0, 0);
  } else if (action.is("edit.paste")) {
    ::SendMessageW(edit, WM_PASTE, 0, 0);
  } else if (action.is("edit.delete")) {
    ::SendMessageW(edit, WM_CLEAR, 0, 0);
  } else if (action.is("edit.selectAll")) {
    ::SendMessageW(edit, EM_SETSEL, 0, -1);
  } else if (action.is("edit.pasteAndGo")) {
    // Chrome's and Edge's row: the clipboard replaces the field and goes,
    // without a round trip through the keyboard — sanitized as Chrome's
    // paste is (line breaks folded, javascript: schemes stripped).
    const std::wstring text =
        PhiOmniboxTrimTyped(PhiOmniboxSanitizePaste(ClipboardText(edit)));
    if (text.empty()) {
      return;
    }
    ::SetWindowTextW(edit, text.c_str());
    ClosePopup(/*stop_query=*/false);
    Commit(edit);
  }
}

void PhiOmniboxSession::PasteInto(HWND edit) {
  // Every paste into the field (WM_PASTE, Ctrl+V, Shift+Insert, the menu)
  // lands sanitized: a pasted "javascript:..." never reaches Enter intact.
  const std::wstring text = PhiOmniboxSanitizePaste(ClipboardText(edit));
  if (!text.empty()) {
    ::SendMessageW(edit, EM_REPLACESEL, TRUE,
                   reinterpret_cast<LPARAM>(text.c_str()));
  }
}

void PhiOmniboxSession::Commit(HWND edit) {
  phi_framework::PhiFramework* framework = shell_->OmniboxFramework();
  if (!edit || !::IsWindow(edit) || !framework) {
    return;
  }
  const std::wstring typed = PhiOmniboxTrimTyped(TextOf(edit));
  if (typed.empty()) {
    return;
  }
  shell_->OmniboxNativePageCommitting();
  // The framework resolves what was typed the way the omnibox would — a bare
  // host becomes a URL, free text becomes a search.
  const BrowserId window = shell_->OmniboxBrowser();
  if (!window.valid()) {
    return;  // No browser attached: nowhere for the text to go.
  }
  BrowserId browser = window;
  if (new_tab_pending_ || shell_->OmniboxBrowserPlaceholding()) {
    // Omnibox new-tab mode — and a placeholder-mode browser, whose strip is
    // EMPTY (Navigate alone would no-op): the tab is born on commit, in the
    // WINDOW (never inside a peek). The fresh tab is active, so Navigate
    // applies the full typed-text resolution to it (NewTab itself loads
    // URLs literally).
    new_tab_pending_ = false;
    framework->NewTab(window.value, "");
  } else {
    // The page the field describes: a presented Peek navigates in place and
    // stays up (PHI-1745); the opener behind the card is never the target.
    browser = CurrentTabTarget();
    if (!browser.valid()) {
      return;  // Aimed at a peek that closed meanwhile: dropped.
    }
  }
  if (browser == window) {
    // The typed URL replaces the ORIGIN tab's page, which the reader (or a
    // peek) is covering — leave it, or what arrives loads unseen behind it
    // (the Mac's closeReaderOverlayForAddressBarNavigation).
    shell_->OmniboxLeaveReader();
  }
  framework->Navigate(browser.value, base::WideToUTF8(typed).c_str());
  shell_->OmniboxFocusContent();  // Keys belong to the page again.
}

bool PhiOmniboxSession::FocusFieldIfVisible() {
  if (!field_ || !::IsWindowVisible(field_)) {
    return false;
  }
  ::SetFocus(field_);
  ::SendMessageW(field_, EM_SETSEL, 0, -1);
  return true;
}

void PhiOmniboxSession::StartNewTab() {
  // "New tab behavior = Omnibox": Ctrl+T / the New Tab row focus the
  // address field instead of opening a page; the tab is born on commit
  // (the Mac's toggleOmniBox new-tab mode). The page mode opens the NTP.
  if (shell_->OmniboxNewTabFocusesField() && field_ &&
      ::IsWindowVisible(field_)) {
    new_tab_pending_ = true;
    ::SetFocus(field_);
    ::SendMessageW(field_, EM_SETSEL, 0, -1);
    return;
  }
  const BrowserId browser = shell_->OmniboxBrowser();
  phi_framework::PhiFramework* framework = shell_->OmniboxFramework();
  if (framework && browser.valid()) {
    framework->NewTab(browser.value, "");
  }
}

void PhiOmniboxSession::Repaint() {
  if (field_ && ::IsWindow(field_)) {
    // Erase so WM_CTLCOLOREDIT hands the EDIT the current palette's brush.
    ::InvalidateRect(field_, nullptr, TRUE);
  }
}

HBRUSH PhiOmniboxSession::OnCtlColorEdit(HDC edit_dc, HWND edit) {
  if (!field_ || edit != field_) {
    return nullptr;
  }
  // The shell's CurrentPalette, not the host's: incognito overrides to
  // fixed-dark.
  const PhiPalette palette = shell_->OmniboxFieldPalette();
  // In the sidebar the field rests on the 4% wash (8% while hovered,
  // tracking the capsule's brighten); in a top bar it sits bare on the
  // bar's own ground, washing only under the cursor.
  // Comfortable's omnibox is a filled pill like the sidebar's, not a
  // bare top-bar field: without this the EDIT paints the toolbar's
  // white straight over the pill and leaves only its rim showing.
  // Balanced and comfortable wash over the bar's own windowBackground,
  // so the EDIT carries no Space tint there; only performance's
  // sidebar pill blends over the tinted ground (see kFieldBg).
  // The EDIT paints on the field's own fill (windows-native-ui.md §4, the
  // shell's PaintChrome): the control fill at rest, a step darker under
  // the pointer, the card body while focused — in every layout.
  const bool focused = ::GetFocus() == field_;
  const COLORREF bg = focused        ? palette.card
                      : field_hover_ ? PhiBlend(palette.ink, palette.card, 18)
                                     : palette.control_fill;
  ::SetTextColor(edit_dc, palette.label);
  ::SetBkColor(edit_dc, bg);
  if (!brush_ || brush_color_ != bg) {
    if (brush_) {
      ::DeleteObject(brush_);
    }
    brush_ = ::CreateSolidBrush(bg);
    brush_color_ = bg;
  }
  return brush_;
}

bool PhiOmniboxSession::OnShellCommand(WPARAM wparam, LPARAM lparam) {
  if (!field_ || lparam != reinterpret_cast<LPARAM>(field_) ||
      HIWORD(wparam) != EN_CHANGE) {
    return false;
  }
  OnFieldChanged(field_);  // The user typed: autocomplete runs.
  return true;
}

void PhiOmniboxSession::OnFieldChanged(HWND edit) {
  // EN_CHANGE while the user types: run/refresh the autocomplete query.
  // Programmatic sets (Sync, arrowing onto a row) are suppressed.
  phi_framework::PhiFramework* framework = shell_->OmniboxFramework();
  if (programmatic_ || !edit || !framework || ::GetFocus() != edit) {
    return;
  }
  const std::wstring typed = TextOf(edit);
  const BrowserId browser = shell_->OmniboxBrowser();
  // The user's own text now (an inline completion typed over or deleted
  // is gone with it), and the arrows' pick no longer answers it: Enter
  // waits for the rows this text brings, or commits the text.
  user_text_ = typed;
  inline_shown_.clear();
  arrowed_ = false;
  const bool prevent_inline = last_edit_deleted_;
  last_edit_deleted_ = false;
  if (keyword_mode() && edit == field_) {
    // Behind the chip the text is the engine's query: no suggestions for it
    // (the Mac's handleInputChanged clears them) — and the new-tab gesture
    // stays armed for the commit, so this is not ClosePopup(true).
    HidePopup();
    if (browser.valid()) {
      framework->StopOmniboxQuery(browser.value);
    }
    return;
  }
  if (typed.empty()) {
    ClosePopup(/*stop_query=*/true);
    return;
  }
  if (!browser.valid()) {
    return;
  }
  framework->StartOmniboxQuery(browser.value, base::WideToUTF8(typed).c_str(),
                               prevent_inline);
  // A hint that no longer prefixes what is typed goes now, not when the
  // next row set lands.
  UpdateKeywordHint();
}

// --- Navigation state ----------------------------------------------------

void PhiOmniboxSession::RebuildNavState(const phi_framework::NavState* nav,
                                        BrowserId page_browser) {
  // A Peek presented over the page, or closed under it (its chip, Esc, page
  // JS window.close(), teardown), moves the field to another browser. Only
  // those two transitions count — a Space switch moves it too, and keeps
  // its edit as before. The opener stays strip-active through a peek's
  // close, so no focus change dismisses the edit: do it here (the Mac's
  // omniBoxDidClear sink), or Enter lands on the opener behind the gone card.
  const BrowserId window = shell_->OmniboxBrowser();
  const bool peek = page_browser.valid() && page_browser != window;
  // Same window only: a Space switch changes the window AND the page, and
  // must keep a half-typed edit exactly as it did before peeks could seed
  // the field.
  const bool retargeted = window == nav_window_ && (peek || nav_peek_) &&
                          page_browser != nav_browser_;
  nav_window_ = window;
  nav_browser_ = page_browser;
  nav_peek_ = peek;
  nav_url_.clear();
  can_go_back_ = can_go_forward_ = nav_loading_ = false;
  if (nav) {
    // The NTP shows the PLACEHOLDER, not its chrome:// url (the Mac's
    // updateDisplayedURL isNTP blank).
    if (!PhiOmniboxHidesUrl(nav->url)) {
      nav_url_ = base::UTF8ToWide(nav->url);
    }
    can_go_back_ = nav->can_go_back;
    can_go_forward_ = nav->can_go_forward;
    nav_loading_ = nav->loading;
  }
  if (favicon_missing_ && popup_ && ::IsWindowVisible(popup_)) {
    // A history row painted its glyph while the favicon was being fetched;
    // the fetch reports as a window change, which lands here.
    ::InvalidateRect(popup_, nullptr, FALSE);
  }
  if (retargeted && field_ && ::GetFocus() == field_) {
    LOG(INFO) << "[PhiClient] omnibox: page target moved to browser "
              << page_browser.value << " under the field — dismissing";
    Sync(/*force=*/true);
    shell_->OmniboxFocusContent();
    return;
  }
  Sync(/*force=*/false);
}

BrowserId PhiOmniboxSession::CurrentTabTarget() {
  const BrowserId page = shell_->OmniboxPageBrowser();
  if (nav_peek_ && page != nav_browser_) {
    // The peek the field was seeded from is gone (or another took its place)
    // and Enter landed before the retarget dismissed the edit: the page this
    // submission was aimed at no longer exists, so drop it rather than
    // navigate the opener behind the vanished card.
    LOG(INFO) << "[PhiClient] omnibox: presented peek " << nav_browser_.value
              << " is gone — dropping the submission";
    Sync(/*force=*/true);
    shell_->OmniboxFocusContent();
    return BrowserId();
  }
  return page;
}

std::wstring PhiOmniboxSession::DisplayedUrl() const {
  // What the user actually sees, mid-edit.
  return field_ ? TextOf(field_) : PhiOmniboxBrandUrl(nav_url_);
}

void PhiOmniboxSession::SetFieldHover(bool hovered) {
  const HWND shell_hwnd = shell_->OmniboxShellWindow();
  if (!hovered) {
    // A leave from one side (EDIT vs shell rim) can land after the enter on
    // the other; trust the cursor, not the message order.
    POINT pt = {};
    if (::GetCursorPos(&pt) && shell_hwnd &&
        ::ScreenToClient(shell_hwnd, &pt)) {
      const RECT field = ToRect(shell_->OmniboxCapsule().field);
      hovered = ::PtInRect(&field, pt) != FALSE;
    }
  }
  if (field_hover_ == hovered) {
    return;
  }
  field_hover_ = hovered;
  if (shell_hwnd) {
    // The capsule's rim: the field plus the wash's antialiased edge.
    RECT field = ToRect(shell_->OmniboxCapsule().field);
    ::InflateRect(&field, Scaled(2), Scaled(2));
    ::InvalidateRect(shell_hwnd, &field, FALSE);
  }
  if (field_) {
    // The EDIT repaints through WM_CTLCOLOREDIT to pick up the new wash.
    ::InvalidateRect(field_, nullptr, TRUE);
  }
}

void PhiOmniboxSession::CopyLinkToClipboard() {
  const HWND shell_hwnd = shell_->OmniboxShellWindow();
  if (nav_url_.empty() || !shell_hwnd || !::OpenClipboard(shell_hwnd)) {
    LOG(WARNING) << "copy link: nothing copied (url empty " << nav_url_.empty()
                 << ", clipboard error " << ::GetLastError() << ")";
    return;
  }
  // The Mac's phiBrandEnsuredUrlString: Chromium's own pages travel as
  // phi:// links.
  std::wstring url = nav_url_;
  static constexpr wchar_t kChromeScheme[] = L"chrome://";
  if (url.rfind(kChromeScheme, 0) == 0) {
    url = L"phi://" + url.substr(std::size(kChromeScheme) - 1);
  }
  ::EmptyClipboard();
  const size_t bytes = (url.size() + 1) * sizeof(wchar_t);
  if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    if (wchar_t* dest = static_cast<wchar_t*>(::GlobalLock(mem))) {
      std::copy_n(url.c_str(), url.size() + 1, dest);
      ::GlobalUnlock(mem);
      if (!::SetClipboardData(CF_UNICODETEXT, mem)) {
        ::GlobalFree(mem);
      }
    } else {
      ::GlobalFree(mem);
    }
  }
  ::CloseClipboard();
  copy_link_flash_ = true;
  ::SetTimer(shell_hwnd, kCopyLinkTimer, 1000, nullptr);
  ::InvalidateRect(shell_hwnd, nullptr, FALSE);
}

void PhiOmniboxSession::RevertCopyLinkFlash() {
  const HWND shell_hwnd = shell_->OmniboxShellWindow();
  if (shell_hwnd) {
    ::KillTimer(shell_hwnd, kCopyLinkTimer);
  }
  copy_link_flash_ = false;
  if (shell_hwnd) {
    ::InvalidateRect(shell_hwnd, nullptr, FALSE);
  }
}

// --- Suggestion dropdown -------------------------------------------------

void PhiOmniboxSession::OnResults(BrowserId browser_id,
                                  const std::string& json) {
  if (browser_id != shell_->OmniboxBrowser() || !FocusedField() ||
      keyword_mode()) {
    // A stale query, the user is no longer typing — or the chip went up
    // while the query's last update was still on its way.
    return;
  }
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  std::wstring input;
  if (root) {
    if (const std::string* text = root->FindString("input")) {
      input = base::UTF8ToWide(*text);
    }
  }
  if (PhiOmniboxTrimTyped(input) != PhiOmniboxTrimTyped(user_text_)) {
    // The last update of a query the user has typed past (the Mac's
    // shouldAcceptResponse): its rows would arm a stale default.
    return;
  }
  // The arrows' pick survives this query's streamed updates
  // (PhiOmniboxArmedRow), by its engine when it named one.
  const int previous = selected_;
  const std::wstring previous_keyword =
      previous >= 0 && previous < static_cast<int>(rows_.size())
          ? rows_[static_cast<size_t>(previous)].keyword
          : std::wstring();
  rows_.clear();
  results_input_ = input;
  hovered_ = -1;
  hovered_delete_ = false;
  if (root) {
    if (const base::ListValue* matches = root->FindList("matches")) {
      for (const base::Value& v : *matches) {
        const base::DictValue* m = v.GetIfDict();
        if (!m || rows_.size() >= static_cast<size_t>(kMaxRows)) {
          continue;
        }
        Row row;
        row.line = m->FindInt("line").value_or(static_cast<int>(rows_.size()));
        if (const std::string* s = m->FindString("type")) {
          row.type = *s;
        }
        if (const std::string* s = m->FindString("contents")) {
          row.contents = PhiOmniboxBrandUrl(base::UTF8ToWide(*s));
        }
        if (const std::string* s = m->FindString("description")) {
          row.description = base::UTF8ToWide(*s);
        }
        if (const std::string* s = m->FindString("fillIntoEdit")) {
          row.fill = PhiOmniboxBrandUrl(base::UTF8ToWide(*s));
        }
        if (const std::string* s = m->FindString("url")) {
          row.url = base::UTF8ToWide(*s);
        }
        if (const std::string* s = m->FindString("keyword")) {
          row.keyword = base::UTF8ToWide(*s);
        }
        if (const std::string* s = m->FindString("engine")) {
          row.engine = base::UTF8ToWide(*s);
        }
        if (const std::string* s = m->FindString("inline")) {
          row.inline_completion = base::UTF8ToWide(*s);
        }
        row.has_tab_match = m->FindBool("hasTabMatch").value_or(false);
        row.is_default = m->FindBool("isDefault").value_or(false);
        row.deletable = m->FindBool("deletable").value_or(false);
        row.swap = m->FindBool("swap").value_or(false);
        if (!row.engine.empty() && row.url.empty()) {
          // The engine-selection hint reads "Search Microsoft Bing —
          // bing.com" (the Mac's suggestionTitle row), not the bare
          // keyword the provider fills in. It is never the default and
          // never forgotten (the Mac's selectionEngine rules).
          row.contents = PhiStrF("addressBar.keywordSearch.suggestionTitle",
                                 {row.engine});
          row.description = row.keyword;
          row.is_default = false;
          row.deletable = false;
          row.swap = false;
        }
        if (PhiOmniboxKeepsRow(row)) {
          rows_.push_back(std::move(row));
        }
      }
    }
  }
  selected_ = PhiOmniboxArmedRow(rows_, previous, previous_keyword, arrowed_);
  arrowed_ = arrowed_ && selected_ >= 0;
  if (rows_.empty()) {
    ClosePopup(/*stop_query=*/false);
  } else {
    ShowPopup();
    ApplyInline(FocusedField());
  }
  UpdateKeywordHint();
}

void PhiOmniboxSession::SetFieldText(HWND edit,
                                     const std::wstring& text,
                                     size_t start,
                                     size_t end) {
  programmatic_ = true;  // Not the user typing — no new query.
  ::SetWindowTextW(edit, text.c_str());
  ::SendMessageW(edit, EM_SETSEL, start, end);
  ::SendMessageW(edit, EM_SCROLLCARET, 0, 0);
  programmatic_ = false;
}

void PhiOmniboxSession::ApplyInline(HWND edit) {
  if (!edit || !::IsWindow(edit) || arrowed_ ||
      (keyword_mode() && edit == field_)) {
    return;  // An arrowed row's fill or the chip's query is showing.
  }
  if (HIMC imc = ::ImmGetContext(edit)) {
    // Mid-composition the IME owns the text (the Mac skips marked text).
    const LONG composing =
        ::ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
    ::ImmReleaseContext(edit, imc);
    if (composing > 0) {
      return;
    }
  }
  const std::wstring text = TextOf(edit);
  DWORD sel_start = 0;
  DWORD sel_end = 0;
  ::SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_start),
                 reinterpret_cast<LPARAM>(&sel_end));
  if (!PhiOmniboxCanInline(text, sel_start, sel_end, user_text_,
                           inline_shown_)) {
    return;  // The user took the text over (caret moved, selection).
  }
  const std::wstring completion = PhiOmniboxInlineFor(rows_, user_text_);
  const std::wstring target = user_text_ + completion;
  inline_shown_ = completion;
  if (target != text) {
    // The completion selected, so the next keystroke replaces it; a set
    // without one puts the typed text back.
    SetFieldText(edit, target, user_text_.size(), target.size());
  }
}

void PhiOmniboxSession::RestoreUserText(HWND edit) {
  inline_shown_.clear();
  if (edit && ::IsWindow(edit) && TextOf(edit) != user_text_) {
    SetFieldText(edit, user_text_, user_text_.size(), user_text_.size());
  }
}

void PhiOmniboxSession::ShowPopup() {
  const HWND shell_hwnd = shell_->OmniboxShellWindow();
  const HWND focused = FocusedField();
  if (!shell_hwnd || !focused) {
    return;
  }
  EnsurePopupClass();
  if (!popup_) {
    popup_ = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               L"PhiOmniboxPopup", L"", WS_POPUP, 0, 0, 0, 0,
                               shell_hwnd, nullptr,
                               ::GetModuleHandleW(nullptr), this);
    if (popup_) {
      // The flyout look PhiPopover gives its windows: DWM rounds the corners
      // and casts the shadow, so nothing here draws a curve or a square
      // rim inside the rounding.
      const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
      ::DwmSetWindowAttribute(popup_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                              sizeof(corner));
    }
  }
  if (!popup_) {
    return;
  }
  // DWM's hairline follows the palette (a Space or theme switch moves it).
  const COLORREF border = shell_->OmniboxPalette().border;
  ::DwmSetWindowAttribute(popup_, DWMWA_BORDER_COLOR, &border, sizeof(border));
  const int height = PhiOmniboxPopupHeight(static_cast<int>(rows_.size()),
                                           Scaled(kRowHeight),
                                           Scaled(kPopupPad));
  RECT anchor = {};
  int left = 0;
  int width = 0;
  if (focused != field_ && shell_->OmniboxNativePageBox(&anchor)) {
    // Flush under the new-tab page's box and exactly as wide — the Mac's
    // suggestion list grows out of the box itself.
    left = anchor.left;
    width = anchor.right - anchor.left;
  } else {
    ::GetWindowRect(focused, &anchor);
    left = anchor.left - Scaled(8);
    width = std::max<int>(anchor.right - anchor.left + Scaled(16), Scaled(320));
  }
  ::SetWindowPos(popup_, HWND_TOP, left, anchor.bottom + Scaled(6), width,
                 height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  ::InvalidateRect(popup_, nullptr, TRUE);
}

void PhiOmniboxSession::HidePopup() {
  rows_.clear();
  results_input_.clear();
  selected_ = -1;
  hovered_ = -1;
  hovered_delete_ = false;
  arrowed_ = false;
  inline_shown_.clear();
  if (popup_ && ::IsWindow(popup_)) {
    ::ShowWindow(popup_, SW_HIDE);
  }
  UpdateKeywordHint();  // No rows, no hint.
}

void PhiOmniboxSession::ClosePopup(bool stop_query) {
  HidePopup();
  if (stop_query) {
    new_tab_pending_ = false;  // The gesture was abandoned.
  }
  const BrowserId browser = shell_->OmniboxBrowser();
  phi_framework::PhiFramework* framework = shell_->OmniboxFramework();
  if (stop_query && framework && browser.valid()) {
    framework->StopOmniboxQuery(browser.value);
  }
}

void PhiOmniboxSession::MoveSelection(int delta) {
  if (rows_.empty()) {
    return;
  }
  selected_ =
      PhiOmniboxNextSelection(selected_, delta, static_cast<int>(rows_.size()));
  arrowed_ = selected_ >= 0;
  const HWND focused = FocusedField();
  if (selected_ >= 0 && focused) {
    const Row& row = rows_[static_cast<size_t>(selected_)];
    if (!row.fill.empty()) {
      // The row's fillIntoEdit lands in the field, caret at the end — the
      // omnibox arrow behavior (the Mac's temporary text).
      inline_shown_.clear();
      SetFieldText(focused, row.fill, row.fill.size(), row.fill.size());
    }
  } else if (focused) {
    // Back up to the field: what the user typed, without a completion.
    RestoreUserText(focused);
  }
  if (popup_) {
    ::InvalidateRect(popup_, nullptr, TRUE);
  }
  UpdateKeywordHint();
}

void PhiOmniboxSession::CommitSelection() {
  CommitRow(selected_, ::GetKeyState(VK_CONTROL) < 0);
}

void PhiOmniboxSession::CommitRow(int index, bool ctrl) {
  phi_framework::PhiFramework* framework = shell_->OmniboxFramework();
  if (index < 0 || index >= static_cast<int>(rows_.size()) || !framework) {
    return;
  }
  const Row& row = rows_[static_cast<size_t>(index)];
  if (!row.engine.empty() && row.url.empty()) {
    // The "Search X" row has nowhere to go: Enter and a click open the
    // engine's site (the Mac's b0a536db); Tab alone raises the chip.
    OpenEngineSite(row);
    return;
  }
  const int line = row.line;
  // The framework opens it with Chromium's own semantics (fixup, shortcuts
  // learning) in the tab PhiOmniboxOpenFor names: the row's open tab only
  // while Ctrl is held (the Mac's Hold-to-switch), a FRESH tab in omnibox
  // new-tab mode and in a placeholder browser (its strip is empty), else
  // the current tab.
  const bool new_tab =
      new_tab_pending_ || shell_->OmniboxBrowserPlaceholding();
  new_tab_pending_ = false;
  const PhiOmniboxOpen open = PhiOmniboxOpenFor(row, ctrl, new_tab);
  const BrowserId browser = shell_->OmniboxBrowser();
  if (!browser.valid()) {
    return;
  }
  if (open == PhiOmniboxOpen::kCurrentTab) {
    const BrowserId page = CurrentTabTarget();
    if (!page.valid()) {
      ClosePopup(/*stop_query=*/true);
      return;  // Aimed at a peek that closed meanwhile: dropped.
    }
    if (page != browser) {
      // Chromium opens a CURRENT_TAB match in the browser the query ran in
      // — the opener behind the card. The presented peek's page takes the
      // row's destination directly instead (the Mac's rule); as any native
      // navigation, the match skips Chromium's shortcut learning.
      const std::wstring& url = row.url.empty() ? row.fill : row.url;
      framework->Navigate(page.value, base::WideToUTF8(url).c_str());
      ClosePopup(/*stop_query=*/true);
      shell_->OmniboxFocusContent();
      return;
    }
    // The match replaces the page the reader covers (as Commit).
    shell_->OmniboxLeaveReader();
  }
  using Disposition = phi_framework::PhiFramework::OmniboxDisposition;
  framework->OpenOmniboxMatch(
      browser.value, line,
      open == PhiOmniboxOpen::kSwitchToTab ? Disposition::kSwitchToTab
      : open == PhiOmniboxOpen::kNewTab    ? Disposition::kNewForegroundTab
                                           : Disposition::kCurrentTab);
  ClosePopup(/*stop_query=*/false);
  shell_->OmniboxFocusContent();
}

void PhiOmniboxSession::DeleteRow(int index) {
  phi_framework::PhiFramework* framework = shell_->OmniboxFramework();
  const BrowserId browser = shell_->OmniboxBrowser();
  if (index < 0 || index >= static_cast<int>(rows_.size()) ||
      !rows_[static_cast<size_t>(index)].deletable || !framework ||
      !browser.valid()) {
    return;
  }
  // No local removal: Chromium drops the match and the running query
  // reports the shorter set, with the arrows' pick kept by place (the
  // Mac's deleteSuggestion).
  framework->DeleteOmniboxMatch(browser.value,
                                rows_[static_cast<size_t>(index)].line);
}

RECT PhiOmniboxSession::RowDeleteRect(int row) const {
  RECT rc = {};
  if (!popup_ || row < 0 || row >= static_cast<int>(rows_.size()) ||
      !rows_[static_cast<size_t>(row)].deletable ||
      (row != hovered_ && row != selected_)) {
    return RECT{};
  }
  ::GetClientRect(popup_, &rc);
  const int top = rc.top + Scaled(kPopupInset) + row * Scaled(kRowHeight);
  const int size = Scaled(24);
  const int right = rc.right - Scaled(10);
  const int y = top + (Scaled(kRowHeight) - size) / 2;
  return RECT{right - size, y, right, y + size};
}

int PhiOmniboxSession::RowAtY(int y) const {
  return PhiOmniboxRowAtY(y, static_cast<int>(rows_.size()), Scaled(kRowHeight),
                          Scaled(kPopupInset));
}

void PhiOmniboxSession::PopupHover(int x, int y) {
  // The pointer only LIGHTS a row; it never arms it. Enter commits what the
  // field shows unless the arrows picked a row, so a cursor resting over the
  // list cannot redirect a typed URL.
  const int row = y < 0 ? -1 : RowAtY(y);
  const bool changed = row != hovered_;
  hovered_ = row;
  const RECT remove = RowDeleteRect(row);
  const POINT pt = {x, y};
  const bool over_delete = row >= 0 && ::PtInRect(&remove, pt) != FALSE;
  if ((changed || over_delete != hovered_delete_) && popup_) {
    hovered_delete_ = over_delete;
    ::InvalidateRect(popup_, nullptr, TRUE);
  }
}

void PhiOmniboxSession::PopupClick(int x, int y) {
  // A click on a row's × forgets it; anywhere else opens the row under it,
  // whatever the arrows had armed — in its open tab with Ctrl held.
  const int row = RowAtY(y);
  const RECT remove = RowDeleteRect(row);
  if (row >= 0 && ::PtInRect(&remove, POINT{x, y})) {
    DeleteRow(row);
    return;
  }
  CommitRow(row, ::GetKeyState(VK_CONTROL) < 0);
}

void PhiOmniboxSession::PaintPopup(HWND popup, HDC dc) {
  RECT rc = {};
  ::GetClientRect(popup, &rc);
  const PhiPalette palette = shell_->OmniboxPalette();
  HBRUSH bg = ::CreateSolidBrush(palette.card);
  ::FillRect(dc, &rc, bg);
  ::DeleteObject(bg);
  // No rim here: DWM draws the rounded border and shadow (ShowPopup).
  ::SetBkMode(dc, TRANSPARENT);
  HFONT text_font = shell_->OmniboxTextFont();
  HFONT icon_font = shell_->OmniboxIconFont();
  HFONT hint_font = shell_->OmniboxHintFont();
  phi_framework::PhiFramework* framework = shell_->OmniboxFramework();
  const BrowserId browser = shell_->OmniboxBrowser();
  favicon_missing_ = false;
  for (size_t i = 0; i < rows_.size(); ++i) {
    const Row& row = rows_[i];
    const int index = static_cast<int>(i);
    RECT r = {rc.left + Scaled(6),
              rc.top + Scaled(kPopupInset) + index * Scaled(kRowHeight),
              rc.right - Scaled(6), 0};
    r.bottom = r.top + Scaled(kRowHeight);
    const COLORREF row_bg = index == selected_  ? palette.hover_strong
                            : index == hovered_ ? palette.hover
                                                : palette.card;
    if (row_bg != palette.card) {
      PhiFillRoundRectAA(dc, r, Scaled(6), row_bg);
    }
    RECT glyph_rc = {r.left + Scaled(4), r.top, r.left + Scaled(28), r.bottom};
    // A history row shows its page's favicon (the Mac's faviconPageURL);
    // the fetch is async, so a miss paints the glyph and repaints on
    // arrival (RebuildNavState).
    bool drew_favicon = false;
    const bool history = row.type.find("history") != std::string::npos;
    if (history && framework && browser.valid() && !row.url.empty()) {
      int width = 0;
      int height = 0;
      const unsigned char* pixels = framework->FaviconForUrlBgra(
          browser.value, base::WideToUTF8(row.url).c_str(), &width, &height);
      if (pixels && width > 0 && height > 0) {
        const int size = Scaled(16);
        const int x = (glyph_rc.left + glyph_rc.right - size) / 2;
        const int y = (glyph_rc.top + glyph_rc.bottom - size) / 2;
        drew_favicon =
            PhiDrawBgra(dc, RECT{x, y, x + size, y + size}, pixels, width,
                        height);
      } else {
        favicon_missing_ = true;
      }
    }
    if (!drew_favicon) {
      // Type glyph: search rows a magnifier, bookmarks a star, history a
      // clock, everything else a globe.
      const wchar_t glyph =
          row.type.find("search") != std::string::npos     ? L'\uE721'
          : row.type.find("bookmark") != std::string::npos ? L'\uE734'
          : history                                        ? L'\uE81C'
                                                           : L'\uE774';
      HGDIOBJ glyph_font = icon_font ? ::SelectObject(dc, icon_font) : nullptr;
      ::SetTextColor(dc, palette.label_dim);
      const wchar_t glyph_text[2] = {glyph, L'\0'};
      PhiDrawTextAA(dc, glyph_text, 1, &glyph_rc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      if (glyph_font) {
        ::SelectObject(dc, glyph_font);
      }
    }
    RECT text_rc = {glyph_rc.right + Scaled(6), r.top, r.right - Scaled(8),
                    r.bottom};
    // The trailing stack, right to left: the × of a lit deletable row (the
    // Mac's delete button), then "Hold Ctrl to switch →" on a row that has
    // an open tab (the Mac's SwitchToTabView).
    const RECT remove = RowDeleteRect(index);
    if (remove.right > remove.left) {
      if (hovered_delete_ && index == hovered_) {
        PhiFillCapsuleAA(dc, remove, PhiBlend(palette.ink, row_bg, 24));
      }
      HGDIOBJ previous = icon_font ? ::SelectObject(dc, icon_font) : nullptr;
      ::SetTextColor(dc, palette.label_dim);
      RECT glyph = remove;
      PhiDrawTextAA(dc, L"\uE711", 1, &glyph,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      if (previous) {
        ::SelectObject(dc, previous);
      }
      text_rc.right = remove.left - Scaled(6);
    }
    if (row.has_tab_match) {
      const wchar_t* hint =
          PhiStr("addressBar.suggestions.switchToOpenTabHintCtrl");
      HGDIOBJ previous = hint_font ? ::SelectObject(dc, hint_font) : nullptr;
      RECT measure = text_rc;
      PhiDrawTextAA(dc, hint, -1, &measure,
                    DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
      // At most half the row: the row's own text keeps the rest.
      const int width =
          std::min<int>(measure.right - measure.left,
                        (text_rc.right - text_rc.left) / 2);
      RECT hint_rc = {text_rc.right - width, r.top, text_rc.right, r.bottom};
      ::SetTextColor(dc, PhiBlend(palette.label_dim, row_bg, 190));
      PhiDrawTextAA(dc, hint, -1, &hint_rc,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
      if (previous) {
        ::SelectObject(dc, previous);
      }
      text_rc.right = hint_rc.left - Scaled(8);
    }
    // Chromium's swap_contents_and_description: some rows (a navigation
    // suggestion's title) read description first.
    const std::wstring& title = row.swap ? row.description : row.contents;
    const std::wstring& subtitle = row.swap ? row.contents : row.description;
    HGDIOBJ prev_font = text_font ? ::SelectObject(dc, text_font) : nullptr;
    ::SetTextColor(dc, palette.label);
    RECT measure = text_rc;
    PhiDrawTextAA(dc, title.c_str(), -1, &measure,
                  DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    RECT contents_rc = text_rc;
    contents_rc.right = std::min(
        text_rc.right,
        static_cast<LONG>(text_rc.left + (measure.right - measure.left)));
    PhiDrawTextAA(dc, title.c_str(), -1, &contents_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
    if (!subtitle.empty() && contents_rc.right + Scaled(16) < text_rc.right) {
      RECT desc_rc = {contents_rc.right + Scaled(8), r.top, text_rc.right,
                      r.bottom};
      ::SetTextColor(dc, palette.label_dim);
      const std::wstring desc = L"— " + subtitle;
      PhiDrawTextAA(dc, desc.c_str(), -1, &desc_rc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
    }
    if (prev_font) {
      ::SelectObject(dc, prev_font);
    }
  }
}

// --- Keyword search ------------------------------------------------------

int PhiOmniboxSession::KeywordLeadingAdvance() const {
  return chip_width_px_ > 0 ? chip_width_px_ + Scaled(8) : 0;
}

int PhiOmniboxSession::KeywordTrailingAdvance() const {
  return hint_width_px_ > 0 ? hint_width_px_ + Scaled(8) : 0;
}

bool PhiOmniboxSession::ResultsCurrent(const std::wstring& typed) const {
  return !results_input_.empty() &&
         PhiOmniboxTrimTyped(results_input_) == PhiOmniboxTrimTyped(typed);
}

bool PhiOmniboxSession::SpaceShortcutEnabled() const {
  phi_framework::PhiFramework* framework = shell_->OmniboxFramework();
  const BrowserId browser = shell_->OmniboxBrowser();
  return framework && browser.valid() &&
         framework->KeywordSpaceShortcutEnabled(browser.value);
}

int PhiOmniboxSession::MeasureText(HFONT font,
                                   const std::wstring& text) const {
  const HWND shell_hwnd = shell_->OmniboxShellWindow();
  if (!shell_hwnd || text.empty()) {
    return 0;
  }
  HDC dc = ::GetDC(shell_hwnd);
  if (!dc) {
    return 0;
  }
  HGDIOBJ previous = font ? ::SelectObject(dc, font) : nullptr;
  SIZE extent = {};
  ::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()),
                          &extent);
  if (previous) {
    ::SelectObject(dc, previous);
  }
  ::ReleaseDC(shell_hwnd, dc);
  return extent.cx;
}

void PhiOmniboxSession::InvalidateCapsule() {
  const HWND shell_hwnd = shell_->OmniboxShellWindow();
  if (shell_hwnd) {
    RECT field = ToRect(shell_->OmniboxCapsule().field);
    ::InflateRect(&field, Scaled(2), Scaled(2));
    ::InvalidateRect(shell_hwnd, &field, FALSE);
  }
  if (field_ && ::IsWindow(field_)) {
    ::InvalidateRect(field_, nullptr, TRUE);
  }
}

bool PhiOmniboxSession::AcceptKeyword(int row) {
  // The chip lives in the shell's capsule: only its own field enters
  // keyword search (the new-tab page's box has nowhere to draw it).
  if (keyword_mode() || !field_ || FocusedField() != field_ || row < 0 ||
      row >= static_cast<int>(rows_.size())) {
    return false;
  }
  const Row& picked = rows_[static_cast<size_t>(row)];
  if (picked.engine.empty() || picked.keyword.empty()) {
    return false;
  }
  // A row that already carried a query ("bing.com maps") keeps it as the
  // text; the selection hint starts the query empty.
  const std::wstring query = picked.url.empty() ? std::wstring()
                                                : picked.contents;
  keyword_keyword_ = picked.keyword;
  keyword_engine_ = picked.engine;
  // The Mac badge: 10 of padding either side of the 13/600 label, at most
  // 160 wide.
  chip_width_px_ =
      Scaled(20) + std::min(MeasureText(shell_->OmniboxChipFont(),
                                        keyword_engine_),
                            Scaled(140));
  hint_text_.clear();
  hint_width_px_ = 0;
  HidePopup();
  phi_framework::PhiFramework* framework = shell_->OmniboxFramework();
  const BrowserId browser = shell_->OmniboxBrowser();
  if (framework && browser.valid()) {
    framework->StopOmniboxQuery(browser.value);
  }
  programmatic_ = true;
  ::SetWindowTextW(field_, query.c_str());
  ::SendMessageW(field_, EM_SETSEL, query.size(), query.size());
  programmatic_ = false;
  // Beside the chip the empty field asks for the query, not for a URL.
  ::SendMessageW(field_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(
                     PhiStr("addressBar.keywordSearch.queryPlaceholder")));
  Position();
  InvalidateCapsule();
  return true;
}

void PhiOmniboxSession::ExitKeyword(const std::wstring& text, bool requery) {
  if (!keyword_mode()) {
    return;
  }
  // Backspace passes keyword_keyword_ itself: copy before it is cleared.
  const std::wstring restore = text;
  keyword_keyword_.clear();
  keyword_engine_.clear();
  chip_width_px_ = 0;
  if (field_ && ::IsWindow(field_)) {
    ::SendMessageW(field_, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(PhiStr(
                       "addressBar.configuration.defaultPlaceholder")));
    // Unsuppressed, the EDIT's EN_CHANGE runs autocomplete on `text` —
    // the keyword's hint comes back with it.
    programmatic_ = !requery;
    ::SetWindowTextW(field_, restore.c_str());
    ::SendMessageW(field_, EM_SETSEL, restore.size(), restore.size());
    programmatic_ = false;
  }
  Position();
  InvalidateCapsule();
}

void PhiOmniboxSession::CommitKeyword(HWND edit) {
  phi_framework::PhiFramework* framework = shell_->OmniboxFramework();
  const BrowserId browser = shell_->OmniboxBrowser();
  if (!edit || !::IsWindow(edit) || !framework || !browser.valid()) {
    return;
  }
  const std::wstring query = PhiOmniboxTrimTyped(TextOf(edit));
  if (query.empty()) {
    return;  // Nothing to search yet (the Mac's guard).
  }
  const std::string url = framework->KeywordSearchUrl(
      browser.value, base::WideToUTF8(keyword_keyword_).c_str(),
      base::WideToUTF8(query).c_str());
  if (url.empty()) {
    // The engine went away or cannot take a query any more — the Mac beeps.
    ::MessageBeep(MB_OK);
    return;
  }
  NavigateCommitted(framework, browser, url);
}

void PhiOmniboxSession::OpenEngineSite(const PhiOmniboxRow& row) {
  phi_framework::PhiFramework* framework = shell_->OmniboxFramework();
  const BrowserId browser = shell_->OmniboxBrowser();
  if (!framework || !browser.valid() || row.keyword.empty()) {
    return;
  }
  // The framework needs search terms to build any URL; the keyword itself
  // serves, since only the scheme and host survive.
  const std::string keyword = base::WideToUTF8(row.keyword);
  const std::string site = PhiOmniboxEngineSite(
      framework->KeywordSearchUrl(browser.value, keyword.c_str(),
                                  keyword.c_str()));
  if (site.empty()) {
    ::MessageBeep(MB_OK);  // As the Mac: no site to open.
    return;
  }
  ClosePopup(/*stop_query=*/false);
  NavigateCommitted(framework, browser, site);
}

void PhiOmniboxSession::NavigateCommitted(
    phi_framework::PhiFramework* framework,
    BrowserId browser,
    const std::string& url) {
  shell_->OmniboxNativePageCommitting();
  // As Commit: `browser` is the WINDOW's, where a new tab is born; otherwise
  // the page the field describes — a presented peek navigates in place.
  BrowserId target = browser;
  if (new_tab_pending_ || shell_->OmniboxBrowserPlaceholding()) {
    new_tab_pending_ = false;  // As Commit: the tab is born on commit.
    framework->NewTab(browser.value, "");
  } else {
    target = CurrentTabTarget();
    if (!target.valid()) {
      // Aimed at a peek that closed meanwhile: dropped, the chip with it.
      ExitKeyword(std::wstring(), /*requery=*/false);
      Sync(/*force=*/true);
      return;
    }
  }
  if (target == browser) {
    shell_->OmniboxLeaveReader();
  }
  framework->Navigate(target.value, url.c_str());
  // The chip (if up) comes down with the commit; the URL fills the field
  // once the navigation reports, as after any commit.
  ExitKeyword(std::wstring(), /*requery=*/false);
  Sync(/*force=*/true);
  shell_->OmniboxFocusContent();
}

void PhiOmniboxSession::UpdateKeywordHint() {
  std::wstring text;
  int width = 0;
  // The hint (and the chip) belong to the shell's own field; the new-tab
  // page's box has no capsule to draw them in.
  if (!keyword_mode() && field_ && FocusedField() == field_ &&
      !rows_.empty()) {
    // What the user typed — not an inline completion or an arrowed row's
    // fill (the Mac reads state.inputText, never the field editor).
    const std::wstring& typed = user_text_;
    const bool current = ResultsCurrent(typed);
    const int hint = PhiOmniboxKeywordHintRow(rows_, selected_, typed, current);
    if (hint >= 0) {
      const bool space =
          PhiOmniboxSpaceKeywordRow(rows_, arrowed_ ? selected_ : -1, typed,
                                    current, SpaceShortcutEnabled()) >= 0;
      text = PhiStrF(space ? "addressBar.keywordSearch.tabOrSpaceHint"
                           : "addressBar.keywordSearch.tabHint",
                     {rows_[static_cast<size_t>(hint)].engine});
      // Never more than a third of the capsule: the text keeps its room.
      const SidebarRect field = shell_->OmniboxCapsule().field;
      width = std::min(MeasureText(shell_->OmniboxHintFont(), text),
                       (field.right - field.left) / 3);
    }
  }
  if (text == hint_text_ && width == hint_width_px_) {
    return;
  }
  hint_text_ = std::move(text);
  hint_width_px_ = width;
  Position();
  InvalidateCapsule();
}

void PhiOmniboxSession::PaintKeyword(HDC dc) {
  if (!field_ || !dc || (!keyword_mode() && hint_text_.empty())) {
    return;
  }
  const PhiOmniboxCapsule capsule = shell_->OmniboxCapsule();
  if (capsule.field.right <= capsule.field.left) {
    return;
  }
  // Where the text would start and stop WITHOUT the two: the chip sits at
  // the start, the hint ends at the stop, and the EDIT runs between.
  const SidebarRect text = PhiOmniboxTextRect(
      capsule, Scaled(20), Scaled(12), Scaled(8), Scaled(8));
  const PhiPalette palette = shell_->OmniboxFieldPalette();
  const int previous_mode = ::SetBkMode(dc, TRANSPARENT);
  if (keyword_mode()) {
    // The Mac badge: a 24-tall accent capsule carrying the engine's name in
    // the 13/600 face, legible ink on the accent.
    const int half = Scaled(12);
    const int mid = (capsule.field.top + capsule.field.bottom) / 2;
    const RECT chip = {text.left, mid - half, text.left + chip_width_px_,
                       mid + half};
    // A seeded engine wears its brand (the Mac's brandColor), with white
    // ink; every other engine the accent.
    const int brand = PhiOmniboxKeywordBrand(keyword_keyword_);
    PhiFillCapsuleAA(dc, chip,
                     brand >= 0 ? RGB((brand >> 16) & 0xFF,
                                      (brand >> 8) & 0xFF, brand & 0xFF)
                                : palette.accent);
    RECT label = {chip.left + Scaled(10), chip.top, chip.right - Scaled(10),
                  chip.bottom};
    HFONT font = shell_->OmniboxChipFont();
    HGDIOBJ previous = font ? ::SelectObject(dc, font) : nullptr;
    ::SetTextColor(dc, brand >= 0 ? RGB(0xFF, 0xFF, 0xFF) : palette.on_accent);
    PhiDrawTextAA(dc, keyword_engine_.c_str(), -1, &label,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
    if (previous) {
      ::SelectObject(dc, previous);
    }
  }
  if (!hint_text_.empty()) {
    RECT hint = {text.right - hint_width_px_, capsule.field.top, text.right,
                 capsule.field.bottom};
    HFONT font = shell_->OmniboxHintFont();
    HGDIOBJ previous = font ? ::SelectObject(dc, font) : nullptr;
    ::SetTextColor(dc, palette.label_dim);
    PhiDrawTextAA(dc, hint_text_.c_str(), -1, &hint,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
    if (previous) {
      ::SelectObject(dc, previous);
    }
  }
  ::SetBkMode(dc, previous_mode);
}

bool PhiOmniboxSession::PaintField(HWND field) {
  RECT rc = {};
  ::GetClientRect(field, &rc);
  if (rc.right <= 0 || rc.bottom <= 0) {
    return false;
  }
  if (!field_surface_ || field_surface_->width() != rc.right ||
      field_surface_->height() != rc.bottom) {
    field_surface_ = std::make_unique<PhiLayerSurface>(rc.right, rc.bottom);
  }
  if (!field_surface_->valid()) {
    return false;
  }
  PAINTSTRUCT ps = {};
  HDC dc = ::BeginPaint(field, &ps);
  const HDC buffer = field_surface_->dc();
  const HBRUSH background = reinterpret_cast<HBRUSH>(::SendMessageW(
      ::GetParent(field), WM_CTLCOLOREDIT, reinterpret_cast<WPARAM>(buffer),
      reinterpret_cast<LPARAM>(field)));
  ::FillRect(buffer, &rc,
              background ? background : ::GetSysColorBrush(COLOR_WINDOW));
  // Let the native EDIT draw selection, scrolling, cue text and IME using
  // its parent's palette. Supply opaque alpha: zero-alpha GDI glyphs get
  // washed out when the shell's DWM frame extends beneath the address row.
  ::CallWindowProcW(g_edit_base_proc, field, WM_PRINTCLIENT,
                    reinterpret_cast<WPARAM>(buffer),
                    PRF_CLIENT | PRF_ERASEBKGND);
  ::GdiFlush();
  // SAFETY: PhiLayerSurface owns a tightly packed, top-down 32-bit DIB.
  auto pixels = UNSAFE_BUFFERS(base::span<uint32_t>(
      static_cast<uint32_t*>(field_surface_->bits()),
      field_surface_->byte_size() / 4));
  for (auto& pixel : pixels) {
    pixel |= 0xFF000000u;
  }
  ::BitBlt(dc, 0, 0, rc.right, rc.bottom, buffer, 0, 0, SRCCOPY);
  ::EndPaint(field, &ps);
  return true;
}

// static
LRESULT CALLBACK PhiOmniboxSession::FieldProc(HWND hwnd,
                                              UINT msg,
                                              WPARAM wparam,
                                              LPARAM lparam) {
  if (msg == WM_NCDESTROY) {
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);  // See ShellWndProc.
    return ::CallWindowProcW(g_edit_base_proc, hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiOmniboxSession*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self && self->shell_->OmniboxShellDestroying()) {
    // The field dies with the shell; its WM_KILLFOCUS would still reach the
    // framework for a window that is going.
    return ::CallWindowProcW(g_edit_base_proc, hwnd, msg, wparam, lparam);
  }
  switch (msg) {
    case WM_PAINT:
      if (self && self->PaintField(hwnd)) {
        return 0;
      }
      break;
    case WM_ERASEBKGND:
      if (self) {
        return 1;  // PaintField supplies the complete opaque background.
      }
      break;
    case WM_KEYDOWN:
      if (wparam == VK_RETURN) {
        if (self) {
          if (self->keyword_mode() && hwnd == self->field_) {
            self->CommitKeyword(hwnd);
          } else if (self->selected_ >= 0 &&
                     (self->arrowed_ ||
                      self->ResultsCurrent(self->user_text_))) {
            // The arrows' pick, or the default row of rows that answer
            // what is typed (the Mac's selectSuggestion) — Chromium opens
            // it, inline completion included. Rows of a text the user has
            // typed past are stale: the text itself commits instead.
            self->CommitSelection();
          } else {
            // stop_query=false: the omnibox-new-tab pending flag must
            // survive into the commit (the query dies with focus anyway).
            self->ClosePopup(/*stop_query=*/false);
            self->Commit(hwnd);
          }
        }
        return 0;
      }
      if (wparam == VK_ESCAPE) {
        if (self) {
          if (self->keyword_mode() && hwnd == self->field_) {
            // The chip comes down and the query with it; the next Escape
            // reverts the field as usual.
            self->ExitKeyword(std::wstring(), /*requery=*/true);
            return 0;
          }
          if (self->popup_ && ::IsWindowVisible(self->popup_)) {
            // First Escape drops an inline completion or an arrowed row's
            // text and closes the list; the second reverts the field.
            self->RestoreUserText(hwnd);
            self->ClosePopup(/*stop_query=*/true);
            return 0;
          }
          if (hwnd != self->field_) {
            // The new-tab page's omnibox has no URL to revert to: Escape
            // clears what was typed and keeps the caret.
            ::SetWindowTextW(hwnd, L"");
            return 0;
          }
          self->Sync(/*force=*/true);
          self->shell_->OmniboxFocusContent();
        }
        return 0;
      }
      if ((wparam == VK_DOWN || wparam == VK_UP) && self &&
          !self->rows_.empty()) {
        self->MoveSelection(wparam == VK_DOWN ? 1 : -1);
        return 0;
      }
      if (wparam == VK_TAB && self && hwnd == self->field_ &&
          ::GetKeyState(VK_CONTROL) >= 0) {
        // Tab picks the hinted engine (the Mac's
        // omniBoxTextFieldDidReceiveTabEvent); behind the chip it is inert.
        // Either way the EDIT never sees it — it has no tab stop to go to.
        if (!self->keyword_mode()) {
          const std::wstring typed = self->user_text_;
          self->AcceptKeyword(PhiOmniboxKeywordHintRow(
              self->rows_, self->selected_, typed,
              self->ResultsCurrent(typed)));
        }
        return 0;
      }
      if (wparam == VK_DELETE && self) {
        if (::GetKeyState(VK_SHIFT) < 0 && ::GetKeyState(VK_CONTROL) >= 0 &&
            self->selected_ >= 0 &&
            self->selected_ < static_cast<int>(self->rows_.size()) &&
            self->rows_[static_cast<size_t>(self->selected_)].deletable) {
          // Shift+Delete on an armed row Chromium can forget removes it
          // (Chrome's own chord), instead of the EDIT's cut.
          self->DeleteRow(self->selected_);
          return 0;
        }
        self->last_edit_deleted_ = true;
      }
      if (wparam == VK_INSERT && self && ::GetKeyState(VK_SHIFT) < 0 &&
          ::GetKeyState(VK_CONTROL) >= 0) {
        self->PasteInto(hwnd);  // Shift+Insert: the EDIT's own paste.
        return 0;
      }
      break;
    case WM_CHAR:
      // These keys are handled above; letting them reach the EDIT would
      // only produce the "unhandled character" beep.
      // (Ctrl+Enter arrives as a line feed.)
      if (wparam == VK_RETURN || wparam == VK_ESCAPE || wparam == VK_TAB ||
          wparam == 0x0A) {
        return 0;
      }
      if (self) {
        // Backspace (and the 0x7F of Ctrl+Backspace) removes text: the
        // query it starts asks for no inline completion, or what was just
        // deleted would come straight back. Anything else typed may
        // complete again.
        self->last_edit_deleted_ =
            wparam == VK_BACK || wparam == 0x7F || wparam == 0x18;  // ^X
      }
      if (wparam == VK_BACK && self && self->keyword_mode() &&
          hwnd == self->field_ && ::GetWindowTextLengthW(hwnd) == 0) {
        // Backspace on an empty query takes the chip off and puts the
        // keyword back (the Mac's exitKeywordSearchIfEmpty). At the CHAR
        // stage, not the key-down: TranslateMessage has already queued
        // this character by the time the key-down is dispatched, and the
        // EDIT deletes on it — handled there, it would eat the restored
        // keyword's last letter.
        self->ExitKeyword(self->keyword_keyword_, /*requery=*/true);
        return 0;
      }
      if (wparam == L' ' && self && hwnd == self->field_ &&
          !self->keyword_mode() && ::GetKeyState(VK_CONTROL) >= 0 &&
          ::GetKeyState(VK_MENU) >= 0) {
        // Space at the end of the text, nothing selected: picks the armed
        // engine row (the Mac's handleKeywordSearchSpaceKeyDown). Anywhere
        // else it is a space.
        DWORD sel_start = 0;
        DWORD sel_end = 0;
        ::SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_start),
                       reinterpret_cast<LPARAM>(&sel_end));
        const std::wstring typed = TextOf(hwnd);
        if (sel_start == sel_end && sel_end == typed.size() &&
            // Only a row the ARROWS armed: the default row is armed on its
            // own now, and "bing.com foo" + Space must stay a space.
            self->AcceptKeyword(PhiOmniboxSpaceKeywordRow(
                self->rows_, self->arrowed_ ? self->selected_ : -1, typed,
                self->ResultsCurrent(typed), self->SpaceShortcutEnabled()))) {
          return 0;
        }
      }
      if (wparam == 0x16 && self) {
        self->PasteInto(hwnd);  // Ctrl+V pastes inside the EDIT, not by
        return 0;               // WM_PASTE: sanitize it here too.
      }
      break;
    case WM_PASTE:
      if (self) {
        self->last_edit_deleted_ = false;
        self->PasteInto(hwnd);
        return 0;
      }
      break;
    case WM_CUT:
    case WM_CLEAR:
      if (self) {
        self->last_edit_deleted_ = true;
      }
      break;
    case WM_KILLFOCUS:
      // The user left the field: the dropdown has nothing to complete, and
      // a chip left standing would search the next thing typed.
      if (self) {
        self->ClosePopup(/*stop_query=*/true);
        if (self->keyword_mode() && hwnd == self->field_) {
          self->ExitKeyword(std::wstring(), /*requery=*/false);
          self->Sync(/*force=*/true);
        }
      }
      break;
    case WM_MOUSEMOVE:
      // The capsule's hover brighten while the cursor rides the EDIT (the
      // child swallows the shell's mouse moves over the text area). The
      // new-tab page's field has no capsule.
      if (self && hwnd == self->field_ && !self->field_hover_) {
        self->SetFieldHover(true);
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&track);
      }
      break;
    case WM_MOUSELEAVE:
      if (self && hwnd == self->field_) {
        self->SetFieldHover(false);
      }
      break;
    case WM_SETFOCUS: {
      const LRESULT result =
          ::CallWindowProcW(g_edit_base_proc, hwnd, msg, wparam, lparam);
      ::SendMessageW(hwnd, EM_SETSEL, 0, -1);  // Typing replaces the whole URL.
      ::InvalidateRect(hwnd, nullptr, FALSE);
      return result;
    }
    case WM_CONTEXTMENU:
      if (self) {
        self->shell_->OmniboxFieldPressed();
        // The EDIT's stock menu is replaced by the Fluent flyout (§3.5).
        // Shift+F10 / the menu key arrive with (-1, -1): anchor those on
        // the caret's field.
        POINT screen = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (screen.x == -1 && screen.y == -1) {
          RECT rc = {};
          ::GetWindowRect(hwnd, &rc);
          screen = {rc.left + (rc.right - rc.left) / 3, rc.bottom};
        }
        self->ShowFieldMenu(hwnd, screen);
        return 0;
      }
      break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
      // A press on the field is a press on the shell's chrome that the
      // shell's own click handler never sees (the EDIT is a child window):
      // the extension popup closes on it as on any other chrome press.
      if (self) {
        self->shell_->OmniboxFieldPressed();
      }
      break;
  }
  // EDIT also draws directly while editing/selecting. Repaint that output
  // through the opaque buffer after the native operation has completed.
  const bool repaint = self &&
      (msg == WM_CHAR || msg == WM_KEYDOWN || msg == WM_SETTEXT ||
       msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_LBUTTONDBLCLK ||
       (msg == WM_MOUSEMOVE && (wparam & MK_LBUTTON)) ||
       msg == WM_KILLFOCUS || msg == EM_SETSEL || msg == EM_REPLACESEL ||
       msg == EM_UNDO || msg == WM_PASTE || msg == WM_CUT || msg == WM_CLEAR ||
       msg == WM_UNDO || msg == WM_IME_COMPOSITION);
  const LRESULT result =
      ::CallWindowProcW(g_edit_base_proc, hwnd, msg, wparam, lparam);
  if (repaint && ::IsWindow(hwnd)) {
    ::InvalidateRect(hwnd, nullptr, FALSE);
  }
  return result;
}

}  // namespace phi_client

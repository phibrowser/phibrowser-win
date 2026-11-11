// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_kiosk_window.h"
#include "client/phi_pop_animation.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "base/logging.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "chrome/browser/phinomenon/phi_framework/phi_window_snapshot.h"
#include "client/phi_client_analytics.h"
#include "client/phi_coordinator.h"
#include "client/phi_dwrite.h"
#include "client/phi_fluent.h"
#include "client/phi_extensions_popover.h"
#include "client/phi_glyph_bitmap.h"
#include "client/phi_i18n.h"
#include "client/phi_icons.h"
#include "client/phi_menu_paint.h"
#include "client/phi_new_tab_page.h"
#include "client/phi_theme.h"
#include "client/phi_window_model.h"

namespace phi_client {

namespace {

constexpr wchar_t kKioskClassName[] = L"PhiKioskShell";

// Toolbar controls, in the order they are drawn and hit-tested. The window
// controls are NOT here: they arrive as non-client messages and their hot
// and pressed state belongs to PhiPopupChrome.
enum HotControl {
  kHotBack = 0,
  kHotExtensions,
  kHotCopy,
  kHotSpaceOpen,
  kHotSpaceList,
};

// Design metrics (DIP), from the Mac's KioskBrowserToolbar.
constexpr int kBandHeight = 44;  // The address row (windows-native-ui.md §4.7).
constexpr int kCaptionButtonWidth = 46;  // The Win11 caption button metric.
// The Mac clears the traffic lights with 78 leading points; the window
// controls are on the trailing edge here, so the band opens on the plain
// gutter the Mac gives its trailing control.
constexpr int kEdge = 12;
constexpr int kNavButton = 32;  // The one toolbar button (§2.2a).
constexpr int kCapsuleHeight = 32;  // addressBarHeight.
constexpr int kCapsuleInset = 5;    // The chip's and the actions' own inset.
constexpr int kChipHeight = 26;
constexpr int kChipMaxWidth = 138;
constexpr int kChipPad = 8;
constexpr int kAccessory = 24;  // The extensions / copy slots.
constexpr int kAccessoryGap = 2;
constexpr int kSpaceMenuHeight = 32;
constexpr int kSpaceMenuMaxWidth = 220;  // spaceMenuMaximumWidth.
constexpr int kSpaceChevron = 32;        // menuButtonWidth.
constexpr int kSpaceIcon = 12;           // iconSize.
constexpr int kSpacePad = 6;
constexpr int kGap = 8;
// An off-the-record Kiosk's Space row when no Incognito Space exists yet:
// choosing it creates one. Not a Space id (those never contain spaces).
constexpr char kNewIncognitoSpaceRow[] = "new incognito space";
// The trailing cluster's breath before the window controls, as the shell's
// band gives its own (kBandTrailingGap).
constexpr int kCaptionGap = 12;
// A cold Space's browser takes a moment and this window stays up meanwhile, so
// a second "Open in" within this long is ignored: it would open the page twice.
// A time rather than a flag: a request that fails must not leave the button dead.
constexpr uint64_t kMoveRetryMs = 5000;
// Below this the capsule is too narrow to hold anything but the domain.
constexpr int kCapsuleContentsMinimum = 132;

// Opening size, when the framework gives us no better hint. The Mac restores
// an autosaved frame here; that is not ported yet, so every Kiosk opens at
// this size, centered on the work area.
constexpr int kInitialWidth = 980;
constexpr int kInitialHeight = 680;
// How far right of the Kiosk already open a new one lands (the Mac's
// cascadeOffset).
constexpr int kCascadeOffset = 28;

// Segoe MDL2 glyphs, written as escapes: the literal private-use characters
// the first Peek panel used landed in the file as EMPTY string literals.
constexpr wchar_t kGlyphBack[] = L"\uE72B";
constexpr wchar_t kGlyphProfile[] = L"\uE77B";  // Contact.
constexpr wchar_t kGlyphExtensions[] = L"\uEA86";  // Puzzle.
constexpr wchar_t kGlyphCopy[] = L"\uE71B";  // Link.
constexpr wchar_t kGlyphCopied[] = L"\uE73E";  // Checkmark.
constexpr wchar_t kGlyphChevron[] = L"\uE70D";
// The window controls' glyphs are the frame's (phi_popup_chrome.cc), not
// this file's, since the caption consolidation.

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) {
    return std::wstring();
  }
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                         static_cast<int>(utf8.size()), nullptr,
                                         0);
  std::wstring out(static_cast<size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                        static_cast<int>(utf8.size()), out.data(), size);
  return out;
}

std::string Narrow(const std::wstring& wide) {
  if (wide.empty()) {
    return std::string();
  }
  const int size =
      ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr,
                            nullptr);
  std::string out(static_cast<size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                        static_cast<int>(wide.size()), out.data(), size,
                        nullptr, nullptr);
  return out;
}

// A rounded fill through the anti-aliased path, GDI's stair-stepping
// RoundRect standing in where Direct2D is unavailable.
void FillRounded(HDC dc, const RECT& rc, COLORREF fill, int radius) {
  PhiFillRoundRectAA(dc, rc, radius, fill);
}

void FillCapsule(HDC dc, const RECT& rc, COLORREF fill) {
  FillRounded(dc, rc, fill, (rc.bottom - rc.top) / 2);
}

void StrokeCapsule(HDC dc, const RECT& rc, COLORREF ink, int width) {
  PhiStrokeCapsuleAA(dc, rc, ink, width);
}

// A Space's icon the way the sidebar draws it: the catalog's own artwork,
// colour emoji through DirectWrite, the font glyph as the last resort.
void DrawSpaceIcon(HDC dc,
                   const RECT& rc,
                   const std::wstring& icon,
                   int size,
                   COLORREF ink,
                   HFONT glyph_font,
                   HFONT text_font) {
  if (icon.empty()) {
    return;
  }
  if (PhiDrawCatalogIcon(dc, rc, icon, size, ink)) {
    return;
  }
  const bool private_glyph = PhiIsPrivateGlyph(icon);
  if (!private_glyph && PhiDrawColorIcon(dc, rc, icon, size, ink)) {
    return;
  }
  HFONT font = private_glyph ? glyph_font : text_font;
  HGDIOBJ previous = font ? ::SelectObject(dc, font) : nullptr;
  ::SetTextColor(dc, ink);
  RECT box = rc;
  PhiDrawTextAA(dc, icon.c_str(), -1, &box,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  if (previous) {
    ::SelectObject(dc, previous);
  }
}

}  // namespace

PhiKioskWindow::PhiKioskWindow(int64_t window_id,
                               HWND browser_hwnd,
                               phi_framework::PhiFramework* framework,
                               const PhiWindowModel* model,
                               PhiCoordinator* coordinator,
                               bool incognito)
    : window_id_(BrowserId{window_id}),
      browser_hwnd_(browser_hwnd),
      framework_(framework),
      model_(model),
      coordinator_(coordinator),
      incognito_(incognito) {
  // On the display the user acted on (the Mac's `a910dca8`), not always the
  // primary one: the pointer's when it is over a Phi window (a link click,
  // a menu), else the display of the Phi window that has the keyboard (a
  // shortcut), else the pointer's.
  RECT work = {0, 0, kInitialWidth, kInitialHeight};
  int dpi = static_cast<int>(::GetDpiForSystem());
  HMONITOR display = nullptr;
  {
    POINT cursor = {};
    ::GetCursorPos(&cursor);
    const auto ours = [](HWND hwnd) {
      DWORD process = 0;
      return hwnd && ::GetWindowThreadProcessId(hwnd, &process) &&
             process == ::GetCurrentProcessId();
    };
    const HWND under = ::WindowFromPoint(cursor);
    const HWND foreground = ::GetForegroundWindow();
    const HWND acted = ours(under) ? ::GetAncestor(under, GA_ROOT)
                       : ours(foreground) ? foreground
                                          : nullptr;
    MONITORINFO monitor = {sizeof(monitor)};
    display =
        acted && !ours(under)
            ? ::MonitorFromWindow(acted, MONITOR_DEFAULTTONEAREST)
            : ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    if (::GetMonitorInfoW(display, &monitor)) {
      work = monitor.rcWork;
    } else {
      ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    }
    if (acted) {
      // That window's scale stands in for its display's.
      if (const UINT window_dpi = ::GetDpiForWindow(acted)) {
        dpi = static_cast<int>(window_dpi);
      }
    }
  }
  const int width = ::MulDiv(kInitialWidth, dpi, 96);
  const int height = ::MulDiv(kInitialHeight, dpi, 96);
  int x = work.left + ((work.right - work.left) - width) / 2;
  int y = work.top + ((work.bottom - work.top) - height) / 2;
  // A Kiosk already on that display: the new one opens shifted right of it
  // rather than exactly on top (the Mac's `ebdbb259`), and back at the left
  // edge once the shift would pass the right one. The frontmost Kiosk is the
  // one the user opened or used last; EnumWindows walks front to back.
  struct Frontmost {
    HMONITOR display;
    HWND kiosk;
  } frontmost = {display, nullptr};
  ::EnumWindows(
      [](HWND hwnd, LPARAM param) -> BOOL {
        auto* out = reinterpret_cast<Frontmost*>(param);
        wchar_t name[64] = {};
        DWORD process = 0;
        if (!::IsWindowVisible(hwnd) || ::IsIconic(hwnd) || ::IsZoomed(hwnd) ||
            !::GetWindowThreadProcessId(hwnd, &process) ||
            process != ::GetCurrentProcessId() ||
            !::GetClassNameW(hwnd, name, 64) ||
            ::wcscmp(name, kKioskClassName) != 0 ||
            ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) != out->display) {
          return TRUE;
        }
        out->kiosk = hwnd;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&frontmost));
  RECT previous = {};
  if (frontmost.kiosk && ::GetWindowRect(frontmost.kiosk, &previous)) {
    x = previous.left + ::MulDiv(kCascadeOffset, dpi, 96);
    y = previous.top;
    if (x + width > work.right) {
      x = work.left;
    }
  }

  ClassSpec spec;
  spec.name = kKioskClassName;
  spec.style = CS_HREDRAW | CS_VREDRAW;
  spec.icon = PhiAppIcon(32);
  spec.icon_small = PhiAppIcon(16);

  Options options;
  // The band IS the title bar: one 52dip row carrying the toolbar and the
  // full window-control cluster. PhiPopupChrome's WM_NCCALCSIZE retires
  // the OS caption exactly as this file used to do by hand.
  options.caption_buttons =
      kPhiCaptionClose | kPhiCaptionMinimize | kPhiCaptionMaximize;
  options.caption_height_dip = kBandHeight;
  options.show_mark = false;         // The toolbar draws the whole band.
  options.dim_when_inactive = false; // The Kiosk never dimmed its glyphs.
  options.dialog_keys = kPhiDialogNone;  // No native controls at all.

  const RECT bounds = {x, y, x + width, y + height};
  if (!CreateSecondary(spec, 0, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, bounds,
                       nullptr, L"Phi", options)) {
    LOG(ERROR) << "[PhiClient] kiosk CreateWindowEx failed err="
               << ::GetLastError();
    return;
  }
  copy_flash_timer_ = NewTimer();

  // Same adoption the shell performs: strip Chromium's top-level chrome and
  // clip its HWND into our content rect, so it stops being its own taskbar
  // and Alt-Tab entry.
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    LONG_PTR style = ::GetWindowLongPtrW(browser_hwnd_, GWL_STYLE);
    style &= ~(WS_POPUP | WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_THICKFRAME |
               WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    style |= WS_CHILD | WS_CLIPSIBLINGS;
    ::SetWindowLongPtrW(browser_hwnd_, GWL_STYLE, style);
    LONG_PTR ex = ::GetWindowLongPtrW(browser_hwnd_, GWL_EXSTYLE);
    ex &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME |
            WS_EX_STATICEDGE | WS_EX_APPWINDOW);
    ::SetWindowLongPtrW(browser_hwnd_, GWL_EXSTYLE, ex);
    ::SetParent(browser_hwnd_, hwnd());
    ::SetWindowPos(browser_hwnd_, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                       SWP_FRAMECHANGED);
  }

  RefreshProfile();
  RefreshState();
  PositionContent();
  ::ShowWindow(hwnd(), SW_SHOW);
  ::SetForegroundWindow(hwnd());
}

PhiKioskWindow::~PhiKioskWindow() {
  // The browser HWND belongs to Chromium; hand it back to the desktop before
  // this window dies so its teardown is not reparented into a dead HWND.
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    ::SetParent(browser_hwnd_, nullptr);
  }
  Destroy();  // The base clears GWLP_USERDATA at WM_NCDESTROY.
  for (HFONT font : {ui_font_, ui_strong_font_, glyph_font_, caption_font_}) {
    if (font) {
      ::DeleteObject(font);
    }
  }
}

const phi_framework::WindowSnapshot* PhiKioskWindow::CurrentSnapshot(
    phi_framework::WindowSnapshot* pulled) const {
  if (const phi_framework::WindowSnapshot* known =
          model_ ? model_->Get(window_id_) : nullptr) {
    return known;
  }
  return PhiWindowModel::Pull(framework_, window_id_, pulled) ? pulled
                                                              : nullptr;
}

void PhiKioskWindow::FocusFirstPage() {
  // The Mac's focusPageIfUnclaimed: keys like Space did nothing until a click.
  // The page arrives while the browser is still hidden, so nothing gives it the
  // keyboard; Win32 focus on the browser's HWND does not reach the page. Once,
  // with the window in front and the page there.
  if (first_page_focused_ || url_.empty() || title_.empty() || !alive() ||
      ::GetForegroundWindow() != hwnd()) {
    return;
  }
  first_page_focused_ = true;
  framework_->FocusActivePage(window_id_.value);
}

void PhiKioskWindow::RefreshState() {
  if (!framework_) {
    return;
  }
  phi_framework::WindowSnapshot pulled;
  if (const phi_framework::WindowSnapshot* snapshot =
          CurrentSnapshot(&pulled)) {
    url_ = Widen(snapshot->nav.url);
    title_ = Widen(snapshot->nav.title);
    can_go_back_ = snapshot->nav.can_go_back;
  }
  FocusFirstPage();
  RefreshSpaces();
  MeasureLabels();
  if (alive()) {
    // The page title names the window in the taskbar, as on the Mac.
    ::SetWindowTextW(hwnd(), title_.empty() ? L"Phi" : title_.c_str());
    Invalidate();
  }
}

void PhiKioskWindow::RefreshProfile() {
  if (!framework_ || !coordinator_) {
    return;
  }
  // The snapshot names the profile by its on-disk id; the display name is
  // the owner's "id\tname" summary for it — the one profile list every
  // surface reads.
  phi_framework::WindowSnapshot pulled;
  const phi_framework::WindowSnapshot* snapshot = CurrentSnapshot(&pulled);
  if (!snapshot || snapshot->profile_id.empty()) {
    return;
  }
  for (const std::string& summary : coordinator_->ProfileSummaries()) {
    const size_t tab = summary.find('\t');
    if (summary.substr(0, tab) != snapshot->profile_id) {
      continue;
    }
    profile_name_ = Widen(tab == std::string::npos ? snapshot->profile_id
                                                   : summary.substr(tab + 1));
    MeasureLabels();
    break;
  }
}

void PhiKioskWindow::RefreshSpaces() {
  spaces_.clear();
  target_space_id_.clear();
  if (!coordinator_) {
    return;
  }
  for (const PhiSpaceView& space : coordinator_->Spaces()) {
    if (incognito_ && !model::IsIncognitoSpaceId(space.id)) {
      continue;  // Off the record: Incognito Spaces only.
    }
    SpaceRow row;
    row.id = space.id;
    row.name = Widen(!space.name.empty() ? space.name : "Space " + space.id);
    row.icon = Widen(space.icon_text);
    spaces_.push_back(std::move(row));
  }
  if (!incognito_) {
    target_space_id_ = coordinator_->KioskTargetSpaceId();
    return;
  }
  if (spaces_.empty()) {
    // None yet: the one row creates it (OpenInSpace).
    SpaceRow row;
    row.id = kNewIncognitoSpaceRow;
    row.name = PhiStr("app.fileMenu.createNewIncognitoSpace");
    spaces_.push_back(std::move(row));
  }
  target_space_id_ = spaces_.front().id;
}

const PhiKioskWindow::SpaceRow* PhiKioskWindow::TargetSpace() const {
  for (const SpaceRow& row : spaces_) {
    if (row.id == target_space_id_) {
      return &row;
    }
  }
  return spaces_.empty() ? nullptr : &spaces_.front();
}

const PhiPalette& PhiKioskWindow::Palette() const {
  // A Kiosk belongs to no Space, so PaletteForSlot falls through to the
  // app-wide palette — which is what this window should follow.
  static const PhiPalette kFallback =
      PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kLight);
  return coordinator_ ? coordinator_->PaletteForSlot(window_id_.value)
                      : kFallback;
}

void PhiKioskWindow::EnsureFonts() {
  if (font_dpi_ == dpi() && ui_font_) {
    return;
  }
  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  if (!::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                    &metrics, 0, dpi())) {
    return;  // Keep whatever we had rather than falling back to no font.
  }
  // The face tracks the system (Segoe UI); the sizes are the Mac's — 13pt
  // for the domain and the chip labels.
  LOGFONTW ui = metrics.lfMessageFont;
  ui.lfHeight = -Scaled(13);
  HFONT ui_built = ::CreateFontIndirectW(&ui);
  ui.lfWeight = FW_SEMIBOLD;
  HFONT strong_built = ::CreateFontIndirectW(&ui);

  LOGFONTW glyph = metrics.lfMessageFont;
  // The unsafe-libc-call gate rules out wcscpy_s; copy the face name directly.
  const std::wstring glyph_face = PhiIconFaceName();
  std::fill(std::begin(glyph.lfFaceName), std::end(glyph.lfFaceName), L'\0');
  std::copy_n(glyph_face.begin(),
              std::min(glyph_face.size(), std::size(glyph.lfFaceName) - 1),
              std::begin(glyph.lfFaceName));
  glyph.lfHeight = -Scaled(12);
  HFONT glyph_built = ::CreateFontIndirectW(&glyph);
  // The window controls wear the OS buttons' own glyph size.
  glyph.lfHeight = -Scaled(10);
  HFONT caption_built = ::CreateFontIndirectW(&glyph);

  if (!ui_built || !strong_built || !glyph_built || !caption_built) {
    for (HFONT font : {ui_built, strong_built, glyph_built, caption_built}) {
      if (font) {
        ::DeleteObject(font);
      }
    }
    return;
  }
  for (HFONT font : {ui_font_, ui_strong_font_, glyph_font_, caption_font_}) {
    if (font) {
      ::DeleteObject(font);
    }
  }
  ui_font_ = ui_built;
  ui_strong_font_ = strong_built;
  glyph_font_ = glyph_built;
  caption_font_ = caption_built;
  font_dpi_ = dpi();
}

int PhiKioskWindow::TextWidth(HFONT font, const std::wstring& text) {
  if (!alive() || text.empty()) {
    return 0;
  }
  HDC dc = ::GetDC(hwnd());
  if (!dc) {
    return 0;
  }
  HGDIOBJ previous = font ? ::SelectObject(dc, font) : nullptr;
  SIZE size = {};
  ::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()),
                          &size);
  if (previous) {
    ::SelectObject(dc, previous);
  }
  ::ReleaseDC(hwnd(), dc);
  return size.cx;
}

void PhiKioskWindow::MeasureLabels() {
  EnsureFonts();
  if (!ui_font_ || !ui_strong_font_) {
    return;
  }
  profile_text_w_ = TextWidth(ui_strong_font_, profile_name_);
  space_prefix_w_ = TextWidth(ui_font_, PhiStr("kiosk.toolbar.openInSpace"));
  const SpaceRow* target = TargetSpace();
  space_name_w_ = target ? TextWidth(ui_strong_font_, target->name) : 0;
}

std::wstring PhiKioskWindow::DisplayUrl() const {
  if (url_.empty() || url_ == L"about:blank" ||
      url_.rfind(L"chrome-extension:", 0) == 0 ||
      PhiNewTabPage::IsNewTabUrl(Narrow(url_))) {
    return std::wstring();
  }
  const size_t scheme = url_.find(L"://");
  if (scheme == std::wstring::npos) {
    return url_;  // No host to show: the Mac shows the string as it stands.
  }
  std::wstring host = url_.substr(scheme + 3);
  const size_t path = host.find_first_of(L"/?#");
  if (path != std::wstring::npos) {
    host = host.substr(0, path);
  }
  if (host.rfind(L"www.", 0) == 0) {
    host = host.substr(4);
  }
  return host.empty() ? url_ : host;
}

PhiKioskWindow::Layout PhiKioskWindow::ComputeLayout() {
  Layout out = {};
  RECT client = {};
  if (alive()) {
    ::GetClientRect(hwnd(), &client);
  }
  if (font_dpi_ != dpi()) {
    MeasureLabels();  // Builds this DPI's fonts, then re-measures on them.
  }

  const int bar = Scaled(kBandHeight);
  const int mid = bar / 2;
  const int btn = Scaled(kNavButton);
  const int cap = Scaled(kCaptionButtonWidth);

  out.band = {0, 0, client.right, bar};
  out.content = {0, bar, client.right, client.bottom};

  // The window controls close the band, minimize / maximize / close in the
  // system's own order — the ONE implementation, shared with the frame
  // that hit-tests and paints them (phi_secondary_window.h).
  PhiCaptionMetrics band = {};
  band.width = static_cast<int>(client.right);
  band.height = bar;
  band.button_width = cap;
  band.close = true;
  band.maximize = true;
  band.minimize = true;
  out.caption_close =
      PhiToRect(PhiCaptionButtonBounds(band, PhiCaptionPart::kClose));
  out.caption_max =
      PhiToRect(PhiCaptionButtonBounds(band, PhiCaptionPart::kMaximize));
  out.caption_min =
      PhiToRect(PhiCaptionButtonBounds(band, PhiCaptionPart::kMinimize));

  // "Open in <Space>" leads them: prefix, the Space's icon, its name, then
  // the chevron half that lists the others.
  int trailing = out.caption_min.left - Scaled(kCaptionGap);
  if (TargetSpace()) {
    const int menu_h = Scaled(kSpaceMenuHeight);
    int width = Scaled(kSpacePad) + space_prefix_w_ + Scaled(kSpacePad) +
                Scaled(kSpaceIcon) + Scaled(kSpacePad) + space_name_w_ +
                Scaled(kSpacePad) + Scaled(kSpaceChevron);
    width = (std::min)(width, Scaled(kSpaceMenuMaxWidth));
    out.space_menu = {trailing - width, mid - menu_h / 2, trailing,
                      mid + menu_h / 2};
    out.space_chevron = {out.space_menu.right - Scaled(kSpaceChevron),
                         out.space_menu.top, out.space_menu.right,
                         out.space_menu.bottom};
    out.space_primary = {out.space_menu.left, out.space_menu.top,
                         out.space_chevron.left, out.space_menu.bottom};
    trailing = out.space_menu.left - Scaled(kGap);
  }

  int x = Scaled(kEdge);
  if (can_go_back_) {
    out.back = {x, mid - btn / 2, x + btn, mid + btn / 2};
    x = out.back.right + Scaled(kGap);
  }
  const int capsule_h = Scaled(kCapsuleHeight);
  out.address = {x, mid - capsule_h / 2, (std::max)(x, trailing),
                 mid + capsule_h / 2};
  out.url = {out.address.left + Scaled(kChipPad), out.address.top,
             out.address.right - Scaled(kChipPad), out.address.bottom};

  if (out.address.right - out.address.left < Scaled(kCapsuleContentsMinimum)) {
    return out;  // Too narrow for anything but the domain.
  }
  const int inset = Scaled(kCapsuleInset);
  const int slot = Scaled(kAccessory);
  out.copy = {out.address.right - inset - slot, mid - slot / 2,
              out.address.right - inset, mid + slot / 2};
  out.extensions = {out.copy.left - Scaled(kAccessoryGap) - slot, out.copy.top,
                    out.copy.left - Scaled(kAccessoryGap), out.copy.bottom};

  const int chip_h = Scaled(kChipHeight);
  const int chip_left = out.address.left + inset;
  int chip_w = Scaled(kChipPad) + Scaled(kSpaceIcon) + Scaled(kSpacePad) +
               profile_text_w_ + Scaled(kChipPad);
  chip_w = (std::min)(chip_w, Scaled(kChipMaxWidth));
  // Never past the actions: the domain keeps a readable run between them.
  chip_w = (std::min)(chip_w, static_cast<int>(out.extensions.left) -
                                  Scaled(kSpacePad) - chip_left - Scaled(48));
  if (!profile_name_.empty() && chip_w > 0) {
    out.profile = {chip_left, mid - chip_h / 2, chip_left + chip_w,
                   mid + chip_h / 2};
    out.url.left = out.profile.right + Scaled(kSpacePad);
  }
  out.url.right = out.extensions.left - Scaled(kSpacePad);
  return out;
}

void PhiKioskWindow::PositionContent() {
  if (!browser_hwnd_ || !::IsWindow(browser_hwnd_)) {
    return;
  }
  const Layout layout = ComputeLayout();
  ::SetWindowPos(browser_hwnd_, nullptr, layout.content.left,
                 layout.content.top, layout.content.right - layout.content.left,
                 layout.content.bottom - layout.content.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void PhiKioskWindow::Paint(HDC mem, const RECT&) {
  RECT client = {};
  ::GetClientRect(hwnd(), &client);

  const PhiPalette palette = Palette();

  // The back buffer is the base's cached one: the toolbar sits directly
  // against the accelerated web surface, and painting it straight to the
  // window tears while resizing (lane 3 §3.4 retired ten copies of the
  // per-paint compatible bitmap this used to be).
  const Layout layout = ComputeLayout();
  // The band, and the ground under it: the browser child covers the
  // content region, but a resize can outrun it for a frame and the base's
  // cached back buffer must not show through there.
  HBRUSH bar_brush = ::CreateSolidBrush(palette.window_bg);
  ::FillRect(mem, &client, bar_brush);
  ::DeleteObject(bar_brush);

  // Hairline under the band, separating chrome from page.
  RECT rule = {0, layout.band.bottom - 1, client.right, layout.band.bottom};
  HBRUSH rule_brush = ::CreateSolidBrush(palette.separator);
  ::FillRect(mem, &rule, rule_brush);
  ::DeleteObject(rule_brush);

  ::SetBkMode(mem, TRANSPARENT);

  const auto draw_glyph = [&](const RECT& rc, const wchar_t* glyph,
                              COLORREF ink) {
    ::SetTextColor(mem, ink);
    RECT box = rc;
    PhiDrawTextAA(mem, glyph, -1, &box,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  };

  // The window controls: the frame owns their hot state, their fill and
  // their glyphs (lane 3 §2 — this was the fourth copy of that code).
  PaintCaptionButtons(mem);

  // The address capsule: the Mac's bordered field, holding the profile chip,
  // the domain and the two actions.
  // The read-only address keeps NO fill and draws dim (§4): a box always
  // means "you can type", dim text "you cannot".

  if (layout.profile.right > layout.profile.left) {
    FillCapsule(mem, layout.profile, palette.hover_strong);
    RECT icon = {layout.profile.left + Scaled(kChipPad), layout.profile.top,
                 layout.profile.left + Scaled(kChipPad) + Scaled(kSpaceIcon),
                 layout.profile.bottom};
    HGDIOBJ previous_chip_font =
        glyph_font_ ? ::SelectObject(mem, glyph_font_) : nullptr;
    draw_glyph(icon, kGlyphProfile, palette.label_dim);
    if (previous_chip_font) {
      ::SelectObject(mem, previous_chip_font);
    }
    HGDIOBJ previous_name_font =
        ui_strong_font_ ? ::SelectObject(mem, ui_strong_font_) : nullptr;
    ::SetTextColor(mem, palette.label);
    RECT name = {icon.right + Scaled(kSpacePad), layout.profile.top,
                 layout.profile.right - Scaled(kChipPad) / 2,
                 layout.profile.bottom};
    PhiDrawTextAA(mem, profile_name_.c_str(), -1, &name,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                    DT_END_ELLIPSIS);
    if (previous_name_font) {
      ::SelectObject(mem, previous_name_font);
    }
  }

  // The domain, centred in the run the chip and the actions leave — a Kiosk
  // has no omnibox, so it shows what it is displaying and nothing more.
  HGDIOBJ previous_url_font =
      ui_font_ ? ::SelectObject(mem, ui_font_) : nullptr;
  ::SetTextColor(mem, palette.label_dim);
  RECT url_rc = layout.url;
  const std::wstring shown = DisplayUrl();
  PhiDrawTextAA(mem, shown.c_str(), -1, &url_rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                  DT_END_ELLIPSIS);
  if (previous_url_font) {
    ::SelectObject(mem, previous_url_font);
  }

  HGDIOBJ previous_glyph_font =
      glyph_font_ ? ::SelectObject(mem, glyph_font_) : nullptr;
  const auto draw_control = [&](int index, const RECT& rc, const wchar_t* glyph,
                                bool enabled, COLORREF wash) {
    (void)wash;
    // The one button (§2.2a): 32 square, radius 4, a 16 dip glyph.
    PhiPaintIconButton(mem, rc, glyph,
                       !enabled        ? PhiButtonState::kDisabled
                       : index == hot_ ? PhiButtonState::kHover
                                       : PhiButtonState::kRest,
                       palette, palette.label, Scaled(fluent::kIconToolbar),
                       Scaled(fluent::kButtonRadius));
  };
  draw_control(kHotBack, layout.back, kGlyphBack, can_go_back_, palette.hover);
  draw_control(kHotExtensions, layout.extensions, kGlyphExtensions, true,
               palette.hover_strong);
  draw_control(kHotCopy, layout.copy,
               copy_flash_ ? kGlyphCopied : kGlyphCopy, true,
               palette.hover_strong);
  if (previous_glyph_font) {
    ::SelectObject(mem, previous_glyph_font);
  }

  // "Open in <Space>": one capsule, two halves — the opening action and the
  // Space list — with the hover wash clipped to the half under the pointer.
  if (layout.space_menu.right > layout.space_menu.left) {
    FillCapsule(mem, layout.space_menu, palette.hover);
    for (const auto& [index, segment] :
         {std::pair<int, RECT>{kHotSpaceOpen, layout.space_primary},
          std::pair<int, RECT>{kHotSpaceList, layout.space_chevron}}) {
      if (hot_ != index) {
        continue;
      }
      const int save = ::SaveDC(mem);
      ::IntersectClipRect(mem, segment.left, segment.top, segment.right,
                          segment.bottom);
      FillCapsule(mem, layout.space_menu, palette.hover_strong);
      ::RestoreDC(mem, save);
    }
    StrokeCapsule(mem, layout.space_menu, palette.border, 1);

    const SpaceRow* target = TargetSpace();
    const std::wstring prefix = PhiStr("kiosk.toolbar.openInSpace");
    int x = layout.space_primary.left + Scaled(kSpacePad);
    HGDIOBJ previous_prefix_font =
        ui_font_ ? ::SelectObject(mem, ui_font_) : nullptr;
    ::SetTextColor(mem, palette.label_dim);
    RECT prefix_rc = {x, layout.space_menu.top,
                      layout.space_primary.right - Scaled(kSpacePad),
                      layout.space_menu.bottom};
    PhiDrawTextAA(mem, prefix.c_str(), -1, &prefix_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    x += space_prefix_w_ + Scaled(kSpacePad);
    if (previous_prefix_font) {
      ::SelectObject(mem, previous_prefix_font);
    }
    if (target && x + Scaled(kSpaceIcon) < layout.space_primary.right) {
      RECT icon = {x, layout.space_menu.top, x + Scaled(kSpaceIcon),
                   layout.space_menu.bottom};
      DrawSpaceIcon(mem, icon, target->icon, Scaled(kSpaceIcon), palette.label,
                    glyph_font_, ui_font_);
      x = icon.right + Scaled(kSpacePad);
      HGDIOBJ previous_space_font =
          ui_strong_font_ ? ::SelectObject(mem, ui_strong_font_) : nullptr;
      ::SetTextColor(mem, palette.label);
      RECT name = {x, layout.space_menu.top,
                   layout.space_primary.right - Scaled(kSpacePad),
                   layout.space_menu.bottom};
      PhiDrawTextAA(mem, target->name.c_str(), -1, &name,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                      DT_END_ELLIPSIS);
      if (previous_space_font) {
        ::SelectObject(mem, previous_space_font);
      }
    }
    // The divider between the halves, the Mac's 18pt rule.
    const int rule_h = Scaled(18);
    RECT divider = {layout.space_chevron.left,
                    (layout.space_menu.top + layout.space_menu.bottom - rule_h) / 2,
                    layout.space_chevron.left + 1,
                    (layout.space_menu.top + layout.space_menu.bottom + rule_h) / 2};
    HBRUSH divider_brush = ::CreateSolidBrush(palette.border);
    ::FillRect(mem, &divider, divider_brush);
    ::DeleteObject(divider_brush);

    HGDIOBJ previous_chevron_font =
        caption_font_ ? ::SelectObject(mem, caption_font_) : nullptr;
    draw_glyph(layout.space_chevron, kGlyphChevron, palette.label_dim);
    if (previous_chevron_font) {
      ::SelectObject(mem, previous_chevron_font);
    }
  }

}

void PhiKioskWindow::OnMouseMove(POINT pt) {
  const Layout layout = ComputeLayout();
  int hot = -1;
  if (can_go_back_ && ::PtInRect(&layout.back, pt)) {
    hot = kHotBack;
  } else if (::PtInRect(&layout.extensions, pt)) {
    hot = kHotExtensions;
  } else if (::PtInRect(&layout.copy, pt)) {
    hot = kHotCopy;
  } else if (::PtInRect(&layout.space_primary, pt)) {
    hot = kHotSpaceOpen;
  } else if (::PtInRect(&layout.space_chevron, pt)) {
    hot = kHotSpaceList;
  }
  if (hot != hot_) {
    hot_ = hot;
    Invalidate(layout.band);
  }
  if (hot >= 0) {
    TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd(), 0};
    ::TrackMouseEvent(&track);
  }
}

void PhiKioskWindow::CopyUrl() {
  if (url_.empty() || !::OpenClipboard(hwnd())) {
    return;
  }
  ::EmptyClipboard();
  const size_t bytes = (url_.size() + 1) * sizeof(wchar_t);
  if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    if (wchar_t* dest = static_cast<wchar_t*>(::GlobalLock(mem))) {
      std::copy_n(url_.c_str(), url_.size() + 1, dest);
      ::GlobalUnlock(mem);
      if (!::SetClipboardData(CF_UNICODETEXT, mem)) {
        ::GlobalFree(mem);
      }
    } else {
      ::GlobalFree(mem);
    }
  }
  ::CloseClipboard();
  // The glyph swaps to a checkmark for a second (the Mac's copyURLButton).
  copy_flash_ = true;
  copy_flash_timer_.Start(1000);
  Invalidate();
}

void PhiKioskWindow::OpenInSpace(const std::string& requested) {
  if (!framework_ || !coordinator_ || requested.empty()) {
    return;
  }
  const uint64_t now = ::GetTickCount64();
  if (moved_at_tick_ && now - moved_at_tick_ < kMoveRetryMs) {
    return;
  }
  std::string space_id = requested;
  if (incognito_) {
    // Off the record: an Incognito Space only, made now if the row said so.
    if (space_id == kNewIncognitoSpaceRow) {
      space_id = coordinator_->KioskIncognitoSpaceId();
    }
    if (!model::IsIncognitoSpaceId(space_id)) {
      return;
    }
  }
  const std::string url = Narrow(url_);
  if (url.empty()) {
    return;
  }
  // The Mac's kiosk_opened_in_space: the page starts its transfer. The
  // count only, never which Space.
  coordinator_->CaptureAnalytics(
      kAnalyticsModuleKiosk, kEventKioskOpenedInSpace,
      PhiSpaceSwitchedProperties(
          static_cast<int>(coordinator_->spaces_model().spaces.size())));
  moved_at_tick_ = now;
  // Closing tears this object down through OnBrowserWindowClosing, and a
  // Space that is already open closes it right here: touch nothing after it.
  // A cold Space closes it later, because with no user window left this Kiosk
  // may be the last Browser, and Chromium would exit with it.
  coordinator_->MoveKioskPageToSpace(window_id_, space_id, url);
}

void PhiKioskWindow::ShowSpaceList(const RECT& anchor) {
  if (spaces_.empty()) {
    return;
  }
  HMENU menu = ::CreatePopupMenu();
  if (!menu) {
    return;
  }
  // Each row wears its Space's icon as a bitmap — the strip's artwork or
  // colour emoji, which menu text can render neither of.
  std::vector<HBITMAP> bitmaps;
  for (size_t i = 0; i < spaces_.size(); ++i) {
    ::AppendMenuW(menu,
                  MF_STRING | (spaces_[i].id == target_space_id_
                                   ? MF_CHECKED | MF_DEFAULT
                                   : 0),
                  static_cast<UINT>(i) + 1,
                  PhiEscapeMenuText(spaces_[i].name).c_str());
    if (HBITMAP bitmap = PhiSpaceGlyphBitmap(spaces_[i].icon, Scaled(16),
                                             ::GetSysColor(COLOR_MENUTEXT))) {
      bitmaps.push_back(bitmap);
      PhiSetMenuItemBitmap(menu, static_cast<UINT>(i) + 1, bitmap);
    }
  }
  POINT screen = {anchor.left, anchor.bottom};
  ::ClientToScreen(hwnd(), &screen);
  const UINT chosen = static_cast<UINT>(PhiTrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, screen.x, screen.y, 0,
      hwnd(), nullptr));
  ::DestroyMenu(menu);
  for (HBITMAP bitmap : bitmaps) {
    ::DeleteObject(bitmap);
  }
  if (chosen >= 1 && chosen <= spaces_.size()) {
    OpenInSpace(spaces_[chosen - 1].id);  // May tear this object down.
  }
}

void PhiKioskWindow::OnToolbarClick(POINT pt) {
  if (!framework_) {
    return;
  }
  const Layout layout = ComputeLayout();
  if (can_go_back_ && ::PtInRect(&layout.back, pt)) {
    framework_->GoBack(window_id_.value);
  } else if (::PtInRect(&layout.extensions, pt)) {
    RECT anchor = layout.extensions;
    ::MapWindowPoints(hwnd(), nullptr, reinterpret_cast<POINT*>(&anchor), 2);
    PhiExtensionsPopover::Toggle(hwnd(), anchor, Palette(), framework_,
                                 window_id_);
  } else if (::PtInRect(&layout.copy, pt)) {
    CopyUrl();
  } else if (::PtInRect(&layout.space_primary, pt)) {
    OpenInSpace(target_space_id_);  // May tear this object down.
  } else if (::PtInRect(&layout.space_chevron, pt)) {
    ShowSpaceList(layout.space_menu);  // May tear this object down.
  }
}

bool PhiKioskWindow::CaptionClaimsClient(POINT client_pt) const {
  // The band's own controls take their own clicks; without this the band
  // would drag the window out from under the pointer instead. The
  // capsule's bare run is deliberately NOT one of them — it takes no
  // clicks, so it drags, which is the Mac's isMovableByWindowBackground.
  const Layout layout = const_cast<PhiKioskWindow*>(this)->ComputeLayout();
  return (can_go_back_ && ::PtInRect(&layout.back, client_pt)) ||
         ::PtInRect(&layout.extensions, client_pt) ||
         ::PtInRect(&layout.copy, client_pt) ||
         ::PtInRect(&layout.space_menu, client_pt);
}

void PhiKioskWindow::OnFramePointer() {
  // Most of the band hit-tests as HTCAPTION, so a pointer crossing it
  // arrives as a NON-client move and not as WM_MOUSEMOVE: nothing the
  // band's own controls were washing is under it any more.
  if (hot_ != -1) {
    hot_ = -1;
    Invalidate();
  }
}

PhiPopupColors PhiKioskWindow::QueryColors() const {
  const PhiPalette& palette = Palette();
  PhiPopupColors colors = PhiPopupColorsFor(palette.dark, palette.accent);
  // The band is the toolbar, so the frame's window controls must stand on
  // the toolbar's own ground and wear its own ink — and never dim, which
  // Options::dim_when_inactive already says.
  colors.bg = palette.window_bg;
  colors.hover = palette.hover;
  colors.label = palette.label;
  colors.tertiary = palette.label;
  return colors;
}

void PhiKioskWindow::OnSize(int, int) {
  PositionContent();
  Invalidate();
}

void PhiKioskWindow::OnDpiChanged(UINT old_dpi) {
  PhiSecondaryWindow::OnDpiChanged(old_dpi);
  MeasureLabels();  // Rebuilds this DPI's fonts, then re-measures on them.
  PositionContent();
  Invalidate();
}

void PhiKioskWindow::OnTimerFired(UINT_PTR id) {
  if (id != copy_flash_timer_.id()) {
    return;
  }
  copy_flash_timer_.Stop();
  copy_flash_ = false;
  Invalidate();
}

LRESULT PhiKioskWindow::OnWindowMessage(UINT msg,
                                        WPARAM wparam,
                                        LPARAM lparam,
                                        bool* handled) {
  switch (msg) {
    case WM_MOUSEMOVE: {
      POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      OnMouseMove(pt);
      *handled = true;
      return 0;
    }
    case WM_MOUSELEAVE:
      if (hot_ != -1) {
        hot_ = -1;
        Invalidate();
      }
      *handled = true;
      return 0;
    case WM_LBUTTONUP: {
      POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      OnToolbarClick(pt);
      *handled = true;
      return 0;
    }
    case WM_ACTIVATE:
      if (LOWORD(wparam) != WA_INACTIVE) {
        // The main window's active Space may have moved while this window
        // was away, and the menu must name where the page will really go.
        RefreshSpaces();
        MeasureLabels();
        Invalidate();
        FocusFirstPage();
      }
      break;  // Activation is still the frame's and the system's to finish.
    case WM_SETFOCUS:
      // Keystrokes belong to the page, never to this bare chrome.
      if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
        ::SetFocus(browser_hwnd_);
        // The browser's own frame is a child and sees no activation, so the
        // page has to be told, as the shell tells its own.
        if (framework_) {
          framework_->FocusBrowserWindow(window_id_.value);
        }
      }
      *handled = true;
      return 0;
    case WM_CLOSE:
      // Route through the framework so the Browser tears down properly; the
      // coordinator drops this window when OnBrowserWindowClosing arrives.
      if (framework_) {
        framework_->CloseBrowserWindow(window_id_.value);
        *handled = true;
        return 0;
      }
      break;
    default:
      break;
  }
  return PhiSecondaryWindow::OnWindowMessage(msg, wparam, lparam, handled);
}

}  // namespace phi_client

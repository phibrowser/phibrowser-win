// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_import_window.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <utility>

#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_fluent.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_menu_paint.h"
#include "phibrowser-win/client/phi_popup_chrome.h"

namespace phi_client {

namespace {

constexpr wchar_t kClassName[] = L"PhiImportWindow";

// The Mac's .normal window is 500 x 760; Windows' dialog padding and
// ContentDialog footer take what the Mac's 56pt margins did.
constexpr int kWidthDip = 520;
constexpr int kHeightDip = 720;
constexpr int kPadDip = 32;
constexpr int kHeaderHeightDip = 52;
constexpr int kToggleRowDip = 40;
constexpr int kRowGapDip = 8;
constexpr int kIconDip = 24;
constexpr int kButtonHeightDip = 32;
constexpr int kKeyStride = 1000;

PhiImportWindow* g_window = nullptr;

std::wstring W(const std::string& s) {
  return base::UTF8ToWide(s);
}

const char* SourceTitleKey(PhiImportSource source) {
  switch (source) {
    case PhiImportSource::kChrome:
      return "oobe.importBrowserData.source.chromeOption";
    case PhiImportSource::kEdge:
      return "oobe.importBrowserData.source.edgeOption";
    case PhiImportSource::kBrave:
      return "oobe.importBrowserData.source.braveOption";
    case PhiImportSource::kArc:
      return "oobe.importBrowserData.source.arcOption";
    case PhiImportSource::kFile:
      return "oobe.importBrowserData.source.fileOption";
  }
  return "";
}

// The browser's own icon, from the executable its App Paths entry names
// (Arc for Windows, an MSIX package, has none: its row takes the globe).
HICON BrowserIcon(PhiImportSource source, int px) {
  const wchar_t* exe = nullptr;
  switch (source) {
    case PhiImportSource::kChrome:
      exe = L"chrome.exe";
      break;
    case PhiImportSource::kEdge:
      exe = L"msedge.exe";
      break;
    case PhiImportSource::kBrave:
      exe = L"brave.exe";
      break;
    default:
      return nullptr;
  }
  const std::wstring key =
      std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") +
      exe;
  wchar_t path[MAX_PATH] = {};
  for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    DWORD size = sizeof(path);
    if (::RegGetValueW(root, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr,
                       path, &size) == ERROR_SUCCESS &&
        path[0]) {
      HICON icon = nullptr;
      if (SUCCEEDED(::SHDefExtractIconW(path, 0, 0, &icon, nullptr,
                                        static_cast<UINT>(px))) &&
          icon) {
        return icon;
      }
    }
  }
  return nullptr;
}

std::wstring FileName(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool Readable(const std::wstring& path) {
  const DWORD attributes = ::GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

// --- Lifetime -----------------------------------------------------------------------

// static
void PhiImportWindow::Open(Host* host,
                           const PhiImportTarget& target,
                           HWND anchor) {
  if (!host) {
    return;
  }
  if (!g_window || !g_window->alive()) {
    g_window = new PhiImportWindow(host, target);
    if (!g_window->OpenWindow(anchor)) {
      delete g_window;
      g_window = nullptr;
      return;
    }
  } else if (!host->BrowserImportRunning()) {
    // Re-invoked from another Space: the next import lands there (the
    // Mac's rebindTarget); a running one keeps its destination.
    g_window->Retarget(target);
  }
  ::ShowWindow(g_window->hwnd(),
               ::IsIconic(g_window->hwnd()) ? SW_RESTORE : SW_SHOW);
  ::SetForegroundWindow(g_window->hwnd());
}

// static
PhiImportWindow* PhiImportWindow::Current() {
  return g_window && g_window->alive() ? g_window : nullptr;
}

PhiImportWindow::PhiImportWindow(Host* host, const PhiImportTarget& target)
    : host_(host), target_(target) {}

PhiImportWindow::~PhiImportWindow() {
  for (Row& row : rows_) {
    if (row.icon) {
      ::DestroyIcon(row.icon);
    }
  }
  if (bg_brush_) {
    ::DeleteObject(bg_brush_);
  }
}

bool PhiImportWindow::OpenWindow(HWND anchor) {
  ClassSpec spec;
  spec.name = kClassName;
  Options options;
  options.caption_buttons = kPhiCaptionClose | kPhiCaptionMinimize;
  options.dialog_keys = kPhiDialogNone;  // Escape handled here.
  const RECT bounds = {CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT + 100,
                       CW_USEDEFAULT + 100};
  if (!CreateSecondary(spec, 0,
                       WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                           WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                       bounds, nullptr,
                       PhiStr("appMenu.importFromBrowser"),
                       options)) {
    LOG(ERROR) << "[Import] CreateWindowEx failed err=" << ::GetLastError();
    return false;
  }
  DeleteWhenDestroyed();
  OnColorsChanged();
  const SIZE outer = chrome().OuterSize(kWidthDip, kHeightDip);
  ::SetWindowPos(hwnd(), nullptr, 0, 0, outer.cx, outer.cy,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  CenterOn(anchor && ::IsWindow(anchor) ? anchor : nullptr);
  LoadRows();
  Layout();
  return true;
}

void PhiImportWindow::OnNcDestroy() {
  if (g_window == this) {
    g_window = nullptr;
  }
  PhiSecondaryWindow::OnNcDestroy();
}

void PhiImportWindow::OnColorsChanged() {
  if (bg_brush_) {
    ::DeleteObject(bg_brush_);
  }
  bg_brush_ = ::CreateSolidBrush(colors().bg);
  if (alive()) {
    Invalidate();
  }
}

void PhiImportWindow::OnDpiChanged(UINT old_dpi) {
  PhiSecondaryWindow::OnDpiChanged(old_dpi);
  for (Row& row : rows_) {
    if (row.icon) {
      ::DestroyIcon(row.icon);
    }
    row.icon = BrowserIcon(row.source, Scaled(kIconDip));
  }
  Layout();
}

LRESULT PhiImportWindow::OnWindowMessage(UINT message,
                                         WPARAM wparam,
                                         LPARAM lparam,
                                         bool* handled) {
  if (message == WM_SIZE && alive()) {
    Layout();
  }
  return PhiSecondaryWindow::OnWindowMessage(message, wparam, lparam, handled);
}

void PhiImportWindow::OnEscape() {
  // A running import carries on without the window (the Mac's import
  // outlives its window too); the coordinator reports a failure itself.
  Destroy();
}

// --- Model --------------------------------------------------------------------------

void PhiImportWindow::LoadRows() {
  for (Row& row : rows_) {
    if (row.icon) {
      ::DestroyIcon(row.icon);
    }
  }
  rows_.clear();
  expanded_ = -1;
  const std::string untitled =
      base::WideToUTF8(PhiStr("oobe.importBrowserData.arc.untitledSpaceName"));
  for (PhiImportSource source : kPhiImportSources) {
    // A browser's row shows when its data folder exists (the Mac's
    // hasChromeData); the file row always does, last.
    if (source != PhiImportSource::kFile &&
        PhiImportUserDataDir(source).empty()) {
      continue;
    }
    Row row;
    row.source = source;
    row.title = PhiStr(SourceTitleKey(source));
    row.icon = BrowserIcon(source, Scaled(kIconDip));
    row.offered = PhiImportOfferedTypes(source);
    if (source == PhiImportSource::kArc) {
      row.spaces = PhiLoadArcSpaces(untitled);
      // A sidebar with no Spaces leaves nothing to import.
      row.enabled = !row.spaces.empty();
      // The picker names each Space's profile.
      row.profiles = PhiLoadChromiumProfiles(source);
    } else if (source != PhiImportSource::kFile) {
      row.profiles = PhiLoadChromiumProfiles(source);
    }
    rows_.push_back(std::move(row));
  }
}

void PhiImportWindow::Retarget(const PhiImportTarget& target) {
  target_ = target;
  if (phase_ == Phase::kFailed) {
    // A reopened window starts over (the Mac's resetForReuse).
    phase_ = Phase::kSelecting;
    status_.clear();
    file_path_.clear();
    LoadRows();
  }
  Layout();
  Invalidate();
}

bool PhiImportWindow::Configured(const Row& row) const {
  if (row.source == PhiImportSource::kFile) {
    return !file_path_.empty();
  }
  return row.enabled && !row.types.empty();
}

bool PhiImportWindow::AnyConfigured() const {
  return std::any_of(rows_.begin(), rows_.end(),
                     [this](const Row& row) { return Configured(row); });
}

std::wstring PhiImportWindow::PickerText(const Row& row) const {
  if (row.source == PhiImportSource::kArc) {
    // More than one Space: the picker shows the chosen one.
    if (row.spaces.size() > 1 &&
        row.selected < static_cast<int>(row.spaces.size())) {
      return W(row.spaces[static_cast<size_t>(row.selected)].title);
    }
    return std::wstring();
  }
  // The dropdown shows only for more than one profile (the Mac's
  // fillChromiumProfileSelector).
  if (row.profiles.size() > 1 &&
      row.selected < static_cast<int>(row.profiles.size())) {
    return W(row.profiles[static_cast<size_t>(row.selected)].name);
  }
  return std::wstring();
}

int PhiImportWindow::RowOf(PhiImportSource source) const {
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (rows_[i].source == source) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void PhiImportWindow::ToggleExpansion(int row) {
  if (phase_ != Phase::kSelecting || row < 0 ||
      row >= static_cast<int>(rows_.size()) ||
      !rows_[static_cast<size_t>(row)].enabled) {
    return;
  }
  // Single-open accordion.
  expanded_ = expanded_ == row ? -1 : row;
  Layout();
  Invalidate();
}

void PhiImportWindow::ToggleType(int row, PhiImportDataType type) {
  if (phase_ != Phase::kSelecting) {
    return;
  }
  Row& r = rows_[static_cast<size_t>(row)];
  if (!r.types.erase(type)) {
    r.types.insert(type);
  }
  Layout();
  Invalidate();
}

void PhiImportWindow::Pick(int row) {
  if (phase_ != Phase::kSelecting) {
    return;
  }
  const Row& r = rows_[static_cast<size_t>(row)];
  HMENU menu = ::CreatePopupMenu();
  auto profile_name = [&r](const std::string& directory) -> std::string {
    for (const PhiChromiumProfile& p : r.profiles) {
      if (p.directory == directory) {
        return p.name;
      }
    }
    return directory;
  };
  const size_t count =
      r.source == PhiImportSource::kArc ? r.spaces.size() : r.profiles.size();
  for (size_t i = 0; i < count; ++i) {
    // "Name (account)" for a profile, "Space (profile)" for an Arc Space.
    std::wstring label;
    if (r.source == PhiImportSource::kArc) {
      const PhiArcSpace& space = r.spaces[i];
      label = W(space.title);
      if (space.profile_directory) {
        label += L" (" + W(profile_name(*space.profile_directory)) + L")";
      }
    } else {
      label = W(r.profiles[i].name);
      if (!r.profiles[i].email.empty()) {
        label += L" (" + W(r.profiles[i].email) + L")";
      }
    }
    ::AppendMenuW(menu,
                  MF_STRING | (static_cast<int>(i) == r.selected ? MF_CHECKED
                                                                 : 0),
                  i + 1, PhiEscapeMenuText(label).c_str());
  }
  POINT pt = {r.picker.left, r.picker.bottom};
  ::ClientToScreen(hwnd(), &pt);
  const UINT picked = static_cast<UINT>(::TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN, pt.x, pt.y, 0,
      hwnd(), nullptr));
  ::DestroyMenu(menu);
  if (picked > 0) {
    SelectPick(row, static_cast<int>(picked) - 1);
  }
}

void PhiImportWindow::SelectPick(int row, int index) {
  Row& r = rows_[static_cast<size_t>(row)];
  if (index == r.selected) {
    return;
  }
  r.selected = index;
  // The old selection belonged to the previous profile / Space (the Mac's
  // resetToggles); the row stays expanded.
  r.types.clear();
  Layout();
  Invalidate();
}

void PhiImportWindow::ChooseFile() {
  if (phase_ != Phase::kSelecting) {
    return;
  }
  using Microsoft::WRL::ComPtr;
  ComPtr<IFileOpenDialog> dialog;
  if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
    return;
  }
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
  // What Chromium's file importer sniffs: a bookmarks HTML file, a Safari
  // History JSON, a Safari export archive (the Mac's .html/.json/.zip).
  const COMDLG_FILTERSPEC types[] = {
      {L"Bookmarks, History or Archive", L"*.html;*.htm;*.json;*.zip"},
      {L"*.*", L"*.*"},
  };
  dialog->SetFileTypes(static_cast<UINT>(std::size(types)), types);
  dialog->SetTitle(PhiStr("oobe.importBrowserData.importButton"));
  if (FAILED(dialog->Show(hwnd()))) {
    return;  // Cancelled.
  }
  ComPtr<IShellItem> item;
  wchar_t* path = nullptr;
  if (SUCCEEDED(dialog->GetResult(&item)) &&
      SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
    SetFile(path);
    ::CoTaskMemFree(path);
  }
}

void PhiImportWindow::SetFile(const std::wstring& path) {
  file_path_ = path;
  Layout();
  Invalidate();
}

void PhiImportWindow::StartImport() {
  if (phase_ != Phase::kSelecting || !AnyConfigured()) {
    return;
  }
  PhiImportRequest request;
  request.target = target_;
  for (const Row& row : rows_) {
    if (!Configured(row)) {
      continue;
    }
    request.sources.push_back(row.source);
    if (row.source == PhiImportSource::kFile) {
      request.file_path = file_path_;
      continue;
    }
    request.types[row.source] =
        std::vector<PhiImportDataType>(row.types.begin(), row.types.end());
    if (row.source == PhiImportSource::kArc) {
      if (row.selected < static_cast<int>(row.spaces.size())) {
        request.arc_space = row.spaces[static_cast<size_t>(row.selected)];
      }
    } else if (row.selected < static_cast<int>(row.profiles.size())) {
      request.profiles[row.source] =
          row.profiles[static_cast<size_t>(row.selected)].directory;
    }
  }
  // Collapse first: the card shrinks back under the status line.
  expanded_ = -1;
  const PhiImportWindow* const self = this;
  if (!host_->StartBrowserImport(request)) {
    return;  // One is already running; this press is ignored.
  }
  if (g_window != self) {
    return;  // A run that finished at once closed (and deleted) the window.
  }
  if (phase_ == Phase::kSelecting) {
    phase_ = Phase::kImporting;
  }
  Layout();
  Invalidate();
}

void PhiImportWindow::SetStatus(const std::wstring& status) {
  status_ = status;
  Layout();
  Invalidate();
}

void PhiImportWindow::Finish(const std::vector<PhiImportSource>& failed) {
  if (failed.empty()) {
    // Every source landed: the window has done its job (the Mac closes it
    // on completion).
    Destroy();
    return;
  }
  std::wstring names;
  for (PhiImportSource source : failed) {
    names += (names.empty() ? L"" : L", ") +
             std::wstring(PhiImportSourceName(source));
  }
  status_ = PhiStrF("oobe.importBrowserData.progress.completedWithErrors",
                    {names});
  phase_ = Phase::kFailed;
  Layout();
  Invalidate();
}

// --- Layout -------------------------------------------------------------------------

HFONT PhiImportWindow::TitleFont() {
  return fonts().Get(24, FW_SEMIBOLD);
}
HFONT PhiImportWindow::BodyFont() {
  return fonts().Get(14, FW_NORMAL);
}
HFONT PhiImportWindow::SmallFont() {
  return fonts().Get(12, FW_NORMAL);
}
HFONT PhiImportWindow::GlyphFont(int size) {
  return fonts().Get(PhiFontDesc{size, FW_NORMAL, PhiIconFaceName()});
}

int PhiImportWindow::MeasureHeight(HFONT font,
                                   const std::wstring& text,
                                   int width) {
  if (text.empty()) {
    return 0;
  }
  HDC dc = ::GetDC(hwnd());
  HGDIOBJ old = ::SelectObject(dc, font);
  RECT r = {0, 0, width, 0};
  ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r,
              DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
  ::SelectObject(dc, old);
  ::ReleaseDC(hwnd(), dc);
  return r.bottom - r.top;
}

void PhiImportWindow::AddButtons(std::vector<Button> buttons, int bottom) {
  // Trailing-aligned, primary last (the Windows ContentDialog order).
  const RECT content = ContentRect();
  int right = content.right - Scaled(kPadDip);
  HDC dc = ::GetDC(hwnd());
  HGDIOBJ old = ::SelectObject(dc, BodyFont());
  for (auto it = buttons.rbegin(); it != buttons.rend(); ++it) {
    SIZE size = {};
    ::GetTextExtentPoint32W(dc, it->text.c_str(),
                            static_cast<int>(it->text.size()), &size);
    const int width = std::max<int>(Scaled(100), size.cx + Scaled(32));
    it->rect = {right - width, bottom - Scaled(kButtonHeightDip), right,
                bottom};
    right -= width + Scaled(8);
  }
  ::SelectObject(dc, old);
  ::ReleaseDC(hwnd(), dc);
  for (Button& button : buttons) {
    buttons_.push_back(std::move(button));
  }
}

void PhiImportWindow::Layout() {
  if (!alive()) {
    return;
  }
  const RECT content = ContentRect();
  const int left = content.left + Scaled(kPadDip);
  const int right = content.right - Scaled(kPadDip);
  const int width = right - left;
  int y = content.top + Scaled(24);

  const std::wstring title = PhiStr("oobe.importBrowserData.initialTitle");
  title_rect_ = {left, y, right, y + MeasureHeight(TitleFont(), title, width)};
  y = title_rect_.bottom + Scaled(4);
  target_rect_ = {left, y, right,
                  y + MeasureHeight(SmallFont(), target_.label, width)};
  y = target_rect_.bottom + Scaled(20);

  const int pad = Scaled(16);
  for (size_t i = 0; i < rows_.size(); ++i) {
    Row& row = rows_[i];
    row.toggles.clear();
    row.hint = row.file_name = row.choose = row.picker = RECT{};
    const int top = y;
    row.header = {left, top, right, top + Scaled(kHeaderHeightDip)};
    // The picker pill sits left of the chevron.
    const std::wstring pick = PickerText(row);
    if (!pick.empty()) {
      HDC dc = ::GetDC(hwnd());
      HGDIOBJ old = ::SelectObject(dc, SmallFont());
      SIZE size = {};
      ::GetTextExtentPoint32W(dc, pick.c_str(), static_cast<int>(pick.size()),
                              &size);
      ::SelectObject(dc, old);
      ::ReleaseDC(hwnd(), dc);
      const int pill_w =
          std::min<int>(Scaled(180), size.cx + Scaled(10 + 24));
      const int pill_right = right - Scaled(44);
      const int mid = (row.header.top + row.header.bottom) / 2;
      row.picker = {pill_right - pill_w, mid - Scaled(14), pill_right,
                    mid + Scaled(14)};
    }
    y = row.header.bottom;
    if (static_cast<int>(i) == expanded_) {
      if (row.source == PhiImportSource::kFile) {
        const int mid = y + Scaled(kToggleRowDip) / 2;
        row.choose = {right - pad - Scaled(120), mid - Scaled(14), right - pad,
                      mid + Scaled(14)};
        row.file_name = {left + pad, y, row.choose.left - Scaled(12),
                         y + Scaled(kToggleRowDip)};
        y += Scaled(kToggleRowDip) + Scaled(8);
      } else {
        for (PhiImportDataType type : row.offered) {
          row.toggles.push_back(
              {type, RECT{left, y, right, y + Scaled(kToggleRowDip)}});
          y += Scaled(kToggleRowDip);
        }
        // Windows keeps a running Chromium's history and cookie databases
        // locked (the Mac's Safari reminder, for every browser here).
        const std::wstring hint = PhiStrF(
            "oobe.importBrowserData.windows.closeReminder",
            {PhiImportSourceName(row.source)});
        const int h = MeasureHeight(SmallFont(), hint, width - 2 * pad);
        row.hint = {left + pad, y + Scaled(4), right - pad,
                    y + Scaled(4) + h};
        y = row.hint.bottom + Scaled(12);
      }
    }
    row.card = {left, top, right, y};
    y += Scaled(kRowGapDip);
  }

  buttons_.clear();
  const int bottom = content.bottom - Scaled(24);
  auto button = [](Control control, const wchar_t* text,
                   PhiPopupButtonRole role, bool enabled) {
    Button b;
    b.control = control;
    b.text = text;
    b.role = static_cast<int>(role);
    b.enabled = enabled;
    return b;
  };
  if (phase_ == Phase::kFailed) {
    AddButtons({button(Control::kDone, PhiStr("oobe.importBrowserData.doneButton"),
                       PhiPopupButtonRole::kPrimary, true)},
               bottom);
  } else {
    const bool selecting = phase_ == Phase::kSelecting;
    AddButtons(
        {button(Control::kCancel, PhiStr("prompt.cancel"),
                PhiPopupButtonRole::kSecondary, true),
         button(Control::kImport, PhiStr("app.import"),
                PhiPopupButtonRole::kPrimary, selecting && AnyConfigured())},
        bottom);
  }
  const int status_bottom =
      bottom - Scaled(kButtonHeightDip) - Scaled(16);
  const int status_h = MeasureHeight(BodyFont(), status_, width);
  status_rect_ = {left, status_bottom - status_h, right, status_bottom};

  ClearHits();
  for (const Button& b : buttons_) {
    AddHit(b.rect, static_cast<int>(b.control) * kKeyStride, b.text,
           PhiHitRole::kButton, false, b.enabled);
  }
  const bool selecting = phase_ == Phase::kSelecting;
  for (size_t i = 0; i < rows_.size(); ++i) {
    const Row& row = rows_[i];
    const int r = static_cast<int>(i);
    AddHit(row.header, static_cast<int>(Control::kHeader) * kKeyStride + r,
           row.title, PhiHitRole::kButton, static_cast<int>(i) == expanded_,
           selecting && row.enabled);
    if (row.picker.right > row.picker.left) {
      AddHit(row.picker, static_cast<int>(Control::kPicker) * kKeyStride + r,
             row.title + L": " + PickerText(row), PhiHitRole::kButton, false,
             selecting);
    }
    for (const auto& [type, rect] : row.toggles) {
      AddHit(rect,
             static_cast<int>(Control::kToggle) * kKeyStride + r * 10 +
                 static_cast<int>(type),
             PhiStr(PhiImportDataTypeLabelKey(type)), PhiHitRole::kToggle,
             row.types.count(type) > 0, selecting);
    }
    if (row.choose.right > row.choose.left) {
      AddHit(row.choose, static_cast<int>(Control::kChooseFile) * kKeyStride,
             PhiStr("oobe.importBrowserData.importButton"),
             PhiHitRole::kButton, false, selecting);
    }
  }
}

// --- Paint ---------------------------------------------------------------------------

void PhiImportWindow::Paint(HDC dc, const RECT&) {
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  ::FillRect(dc, &client, bg_brush_);
  ::SetBkMode(dc, TRANSPARENT);
  const PhiPopupColors& c = colors();
  auto text = [&](HFONT font, COLORREF ink, const std::wstring& s, RECT r,
                  UINT format) {
    if (s.empty()) {
      return;
    }
    HGDIOBJ old = ::SelectObject(dc, font);
    ::SetTextColor(dc, ink);
    PhiDrawTextAA(dc, s.c_str(), static_cast<int>(s.size()), &r,
                  format | DT_NOPREFIX);
    ::SelectObject(dc, old);
  };
  const int hovered_key =
      hovered() >= 0 && static_cast<size_t>(hovered()) < hits().size() &&
              hits()[static_cast<size_t>(hovered())].enabled
          ? hits()[static_cast<size_t>(hovered())].key
          : -1;

  text(TitleFont(), c.label_strong, PhiStr("oobe.importBrowserData.initialTitle"),
       title_rect_, DT_WORDBREAK);
  text(SmallFont(), c.secondary, target_.label, target_rect_, DT_WORDBREAK);

  const int pad = Scaled(16);
  for (size_t i = 0; i < rows_.size(); ++i) {
    const Row& row = rows_[i];
    const int r = static_cast<int>(i);
    // A configured row carries the accent wash (the Mac's configured
    // background); a disabled one is dimmed.
    const COLORREF card =
        Configured(row) ? PhiPopupBlend(c.accent, c.card, 28) : c.card;
    PhiPopupStrokeRounded(dc, row.card, card, c.card_border, Scaled(8));
    const bool header_hot =
        hovered_key == static_cast<int>(Control::kHeader) * kKeyStride + r;
    if (header_hot) {
      RECT wash = row.header;
      ::InflateRect(&wash, -Scaled(1), -Scaled(1));
      PhiFillRoundRectAA(dc, wash, Scaled(7), PhiPopupBlend(c.ink, card, 10));
    }
    const COLORREF ink = row.enabled ? c.label : c.tertiary;
    const int mid = (row.header.top + row.header.bottom) / 2;
    const int icon = Scaled(kIconDip);
    RECT icon_rect = {row.header.left + pad, mid - icon / 2,
                      row.header.left + pad + icon, mid + icon / 2};
    if (row.icon) {
      ::DrawIconEx(dc, icon_rect.left, icon_rect.top, row.icon, icon, icon, 0,
                   nullptr, DI_NORMAL);
    } else {
      text(GlyphFont(20), ink,
           row.source == PhiImportSource::kFile ? fluent::kDocument
                                                : fluent::kGlobe,
           icon_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    RECT title = {icon_rect.right + Scaled(14), row.header.top,
                  (row.picker.right > row.picker.left ? row.picker.left
                                                      : row.header.right) -
                      Scaled(12),
                  row.header.bottom};
    text(fonts().Get(15, FW_NORMAL), ink, row.title, title,
         DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    if (row.picker.right > row.picker.left) {
      const bool hot =
          hovered_key == static_cast<int>(Control::kPicker) * kKeyStride + r;
      PhiPopupStrokeRounded(dc, row.picker,
                            hot ? c.button_hover : c.button, c.card_border,
                            Scaled(6));
      RECT value = row.picker;
      value.left += Scaled(10);
      value.right -= Scaled(24);
      text(SmallFont(), c.label, PickerText(row), value,
           DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
      RECT chevron = row.picker;
      chevron.left = chevron.right - Scaled(22);
      text(GlyphFont(8), c.secondary, fluent::kChevronDown, chevron,
           DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
    RECT chevron = {row.header.right - Scaled(40), row.header.top,
                    row.header.right - pad, row.header.bottom};
    text(GlyphFont(12), row.enabled ? c.secondary : c.tertiary,
         r == expanded_ ? fluent::kChevronDown : fluent::kChevronRight,
         chevron, DT_SINGLELINE | DT_VCENTER | DT_CENTER);

    // The body.
    for (const auto& [type, rect] : row.toggles) {
      RECT label = rect;
      label.left += pad;
      label.right -= pad + Scaled(52);
      text(BodyFont(), c.label, PhiStr(PhiImportDataTypeLabelKey(type)),
           label, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
      // The design's ToggleSwitch: a 40 x 20 capsule (Settings' look).
      const bool on = row.types.count(type) > 0;
      const int sw_mid = (rect.top + rect.bottom) / 2;
      RECT sw = {rect.right - pad - Scaled(40), sw_mid - Scaled(10),
                 rect.right - pad, sw_mid + Scaled(10)};
      const bool hot = hovered_key == static_cast<int>(Control::kToggle) *
                                              kKeyStride +
                                          r * 10 + static_cast<int>(type);
      if (hot) {
        RECT halo = sw;
        ::InflateRect(&halo, Scaled(3), Scaled(3));
        PhiFillRoundRectAA(dc, halo, (halo.bottom - halo.top) / 2,
                           PhiPopupBlend(c.ink, card, 14));
      }
      if (on) {
        PhiFillRoundRectAA(dc, sw, Scaled(10), c.accent);
      } else {
        PhiPopupStrokeRounded(dc, sw, card, PhiPopupBlend(c.ink, card, 100),
                              Scaled(10));
      }
      const int inset = Scaled(4);
      const int knob = (sw.bottom - sw.top) - 2 * inset;
      const int knob_left = on ? sw.right - knob - inset : sw.left + inset;
      RECT dot = {knob_left, sw.top + inset, knob_left + knob,
                  sw.top + inset + knob};
      PhiFillEllipseAA(dc, dot, on ? RGB(0xFF, 0xFF, 0xFF) : c.secondary);
    }
    if (row.hint.bottom > row.hint.top) {
      text(SmallFont(), c.secondary,
           PhiStrF("oobe.importBrowserData.windows.closeReminder",
                   {PhiImportSourceName(row.source)}),
           row.hint, DT_WORDBREAK);
    }
    if (row.choose.right > row.choose.left) {
      text(BodyFont(), file_path_.empty() ? c.secondary : c.label,
           file_path_.empty()
               ? std::wstring(PhiStr(
                     "oobe.importBrowserData.filePicker.emptySelectionPlaceholder"))
               : FileName(file_path_),
           row.file_name, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
      const bool hot =
          hovered_key == static_cast<int>(Control::kChooseFile) * kKeyStride;
      PhiPopupPaintButton(dc, row.choose,
                          PhiStr("oobe.importBrowserData.importButton"),
                          PhiPopupButtonRole::kSecondary, hot, false,
                          phase_ == Phase::kSelecting, c, SmallFont(), dpi());
    }
  }

  text(BodyFont(), phase_ == Phase::kFailed ? c.error : c.secondary, status_,
       status_rect_, DT_WORDBREAK);
  for (const Button& b : buttons_) {
    const bool hot = hovered_key == static_cast<int>(b.control) * kKeyStride;
    PhiPopupPaintButton(dc, b.rect, b.text,
                        static_cast<PhiPopupButtonRole>(b.role), hot, false,
                        b.enabled, c, BodyFont(), dpi());
  }
  PaintBand(dc);
}

void PhiImportWindow::OnHover(int, int) {
  Invalidate();
}

void PhiImportWindow::OnClick(int hit, POINT) {
  if (hit < 0 || static_cast<size_t>(hit) >= hits().size() ||
      !hits()[static_cast<size_t>(hit)].enabled) {
    return;
  }
  const int key = hits()[static_cast<size_t>(hit)].key;
  const Control control = static_cast<Control>(key / kKeyStride);
  const int rest = key % kKeyStride;
  switch (control) {
    case Control::kHeader:
      ToggleExpansion(rest);
      break;
    case Control::kPicker:
      Pick(rest);
      break;
    case Control::kToggle:
      ToggleType(rest / 10, static_cast<PhiImportDataType>(rest % 10));
      break;
    case Control::kChooseFile:
      ChooseFile();
      break;
    case Control::kImport:
      StartImport();
      break;
    case Control::kCancel:
    case Control::kDone:
      Destroy();
      break;
    case Control::kNone:
      break;
  }
}

// --- Harness -------------------------------------------------------------------------

std::string PhiImportWindow::StateJson() const {
  base::DictValue state;
  state.Set("phase", phase_ == Phase::kSelecting   ? "selecting"
                     : phase_ == Phase::kImporting ? "importing"
                                                   : "failed");
  state.Set("status", base::WideToUTF8(status_));
  state.Set("target", base::WideToUTF8(target_.label));
  state.Set("targetSpaceId", target_.space_id);
  state.Set("file", base::WideToUTF8(file_path_));
  base::ListValue rows;
  for (size_t i = 0; i < rows_.size(); ++i) {
    const Row& row = rows_[i];
    base::DictValue r;
    r.Set("source", PhiImportSourceKey(row.source));
    r.Set("enabled", row.enabled);
    r.Set("expanded", static_cast<int>(i) == expanded_);
    r.Set("configured", Configured(row));
    r.Set("selected", row.selected);
    base::ListValue types;
    for (PhiImportDataType type : row.types) {
      types.Append(PhiImportDataTypeAnalytics(type));
    }
    r.Set("types", std::move(types));
    base::ListValue choices;
    if (row.source == PhiImportSource::kArc) {
      for (const PhiArcSpace& space : row.spaces) {
        base::DictValue s;
        s.Set("title", space.title);
        s.Set("profile", space.profile_directory.value_or(std::string()));
        s.Set("pins", static_cast<int>(space.root.children.size()));
        choices.Append(std::move(s));
      }
    } else {
      for (const PhiChromiumProfile& p : row.profiles) {
        base::DictValue s;
        s.Set("directory", p.directory);
        s.Set("name", p.name);
        s.Set("email", p.email);
        choices.Append(std::move(s));
      }
    }
    r.Set("choices", std::move(choices));
    rows.Append(std::move(r));
  }
  state.Set("rows", std::move(rows));
  std::string json;
  base::JSONWriter::Write(state, &json);
  return json;
}

std::string PhiImportWindow::Drive(const std::string& action,
                                   const std::string& value) {
  auto parse = [this](const std::string& v, std::string* rest) -> int {
    const size_t colon = v.find(':');
    const std::optional<PhiImportSource> source =
        PhiImportSourceFromKey(v.substr(0, colon));
    if (rest) {
      *rest = colon == std::string::npos ? std::string() : v.substr(colon + 1);
    }
    return source ? RowOf(*source) : -1;
  };
  if (action == "cancel") {
    Destroy();
    return std::string();
  }
  if (phase_ != Phase::kSelecting) {
    return "wrong_phase";
  }
  if (action == "expand") {
    const int row = parse(value, nullptr);
    if (row < 0 || !rows_[static_cast<size_t>(row)].enabled) {
      return "invalid_value";
    }
    ToggleExpansion(row);
    return std::string();
  }
  if (action == "toggle") {
    std::string rest;
    const int row = parse(value, &rest);
    if (row < 0) {
      return "invalid_value";
    }
    for (PhiImportDataType type : rows_[static_cast<size_t>(row)].offered) {
      if (rest == PhiImportDataTypeAnalytics(type)) {
        ToggleType(row, type);
        return std::string();
      }
    }
    return "invalid_value";
  }
  if (action == "profile") {
    std::string rest;
    const int row = parse(value, &rest);
    int index = -1;
    if (row < 0 || !base::StringToInt(rest, &index)) {
      return "invalid_value";
    }
    const Row& r = rows_[static_cast<size_t>(row)];
    const size_t count =
        r.source == PhiImportSource::kArc ? r.spaces.size() : r.profiles.size();
    if (index < 0 || static_cast<size_t>(index) >= count) {
      return "invalid_value";
    }
    SelectPick(row, index);
    return std::string();
  }
  if (action == "file") {
    const std::wstring path = base::UTF8ToWide(value);
    if (!Readable(path)) {
      return "invalid_value";
    }
    SetFile(path);
    return std::string();
  }
  if (action == "import") {
    if (!AnyConfigured()) {
      return "nothing_selected";
    }
    StartImport();
    return std::string();
  }
  return "unknown_action";
}

}  // namespace phi_client

// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_bookmark_manager.h"
#include "client/phi_pop_animation.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "client/phi_client_analytics.h"
#include "client/phi_dwrite.h"
#include "client/phi_folder_icons.h"
#include "client/phi_i18n.h"
#include "client/phi_mac_icons.h"
#include "client/phi_prompt.h"
#include "client/phi_theme.h"
#include "client/phi_shell_window.h"

namespace phi_client {

namespace {

// Page metrics (DIP), following the Mac page's proportions.
constexpr int kPagePad = 24;
constexpr int kHeaderHeight = 64;
constexpr int kSearchWidth = 260;
constexpr int kSearchHeight = 28;
constexpr int kButtonWidth = 104;
constexpr int kButtonHeight = 28;
constexpr int kRowHeight = 32;
constexpr int kIndent = 18;
constexpr int kTwisty = 14;
constexpr int kIconSize = 16;
constexpr int kColumnGap = 24;
// The Address column takes the trailing 45% of the body, as the Mac's
// stretching Address column settles to at a typical page width.
constexpr int kAddressPercent = 45;
constexpr int kRowRadius = 6;

const wchar_t kClassName[] = L"PhiBookmarkManager";
constexpr int kSearchId = 4101;
// The New Folder button rides in hits_ with the rows; its key is out of
// the rows' index space.
constexpr int kNewFolderKey = -1;
// The Import button beside it (GitHub issue 6): the app menu's "Import from
// another browser…" window, reachable from the page a user looks on first.
constexpr int kImportKey = -2;
// The width kept clear for the "Bookmarks" title before the header buttons.
constexpr int kTitleReserve = 130;

// The four faces, in DESIGN units — the base's PhiFontSet scales them.
constexpr int kTitleSize = 20;
constexpr int kRowSize = 13;
constexpr int kDimSize = 12;
constexpr int kButtonSize = 12;

// Case-insensitive, whitespace-trimmed containment — the Mac matches
// localized/case/diacritic-insensitively; this keeps the case and whitespace
// halves and leaves diacritic folding to a later pass.
std::wstring Fold(const std::wstring& text) {
  std::wstring out = text;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
  return out;
}

std::wstring Trim(const std::wstring& text) {
  size_t begin = text.find_first_not_of(L" \t\r\n");
  if (begin == std::wstring::npos) {
    return std::wstring();
  }
  size_t end = text.find_last_not_of(L" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

// Puts `text` on the clipboard (the context menu's Copy Link).
void CopyToClipboard(HWND owner, const std::wstring& text) {
  if (!::OpenClipboard(owner)) {
    return;
  }
  ::EmptyClipboard();
  const size_t count = text.size() + 1;
  if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, count * sizeof(wchar_t))) {
    if (void* dest = ::GlobalLock(mem)) {
      // SAFETY: the allocation above is exactly `count` wchar_t.
      auto span = UNSAFE_BUFFERS(
          base::span<wchar_t>(static_cast<wchar_t*>(dest), count));
      std::copy(text.begin(), text.end(), span.begin());
      span[text.size()] = L'\0';
      ::GlobalUnlock(mem);
      if (::SetClipboardData(CF_UNICODETEXT, mem)) {
        mem = nullptr;  // The system owns it now.
      }
    }
    if (mem) {
      ::GlobalFree(mem);
    }
  }
  ::CloseClipboard();
}

}  // namespace

PhiBookmarkManager::PhiBookmarkManager(PhiShellWindow* controller)
    : controller_(controller) {}

PhiBookmarkManager::~PhiBookmarkManager() {
  EndSession();  // A window closing over the page ends its session too.
  Destroy();  // Takes the search field with it.
  if (field_brush_) {
    ::DeleteObject(field_brush_);
  }
}

// static
const char* PhiBookmarkManager::ManagerUrl() {
  return "chrome://bookmarks/";
}

// static
bool PhiBookmarkManager::IsManagerUrl(const std::string& url) {
  return url.rfind("chrome://bookmarks", 0) == 0 ||
         url.rfind("phi://bookmarks", 0) == 0;
}

bool PhiBookmarkManager::visible() const {
  return IsPanelVisible();
}

PhiPalette PhiBookmarkManager::QueryPalette() const {
  return controller_ ? controller_->CurrentPalette() : PhiPalette();
}

bool PhiBookmarkManager::HeaderButtonHot(int key) const {
  const int hit = hovered();
  return hit >= 0 && hits()[static_cast<size_t>(hit)].key == key;
}

void PhiBookmarkManager::EnsureWindow() {
  if (alive()) {
    return;
  }
  HWND owner = controller_ ? controller_->shell_hwnd() : nullptr;
  if (!owner) {
    return;
  }
  ClassSpec spec;
  spec.name = kClassName;
  spec.style = CS_DBLCLKS;  // The outline opens rows on double-click.
  Options options;
  // WS_CLIPCHILDREN for the search EDIT; WS_CLIPSIBLINGS is PhiPanel's and
  // keeps the content card's corner caps visible.
  options.style = WS_CLIPCHILDREN;
  const RECT nothing = {0, 0, 0, 0};
  if (!CreatePanel(spec, owner, nothing, options)) {
    return;
  }
  EnsureSearchField();
}

void PhiBookmarkManager::EnsureSearchField() {
  if (search_ || !alive()) {
    return;
  }
  search_ = ::CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 0, 0, hwnd(),
      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kSearchId)),
      ::GetModuleHandleW(nullptr), nullptr);
  if (search_) {
    ::SendMessageW(search_, WM_SETFONT,
                   reinterpret_cast<WPARAM>(fonts().Get(kRowSize, FW_NORMAL)),
                   TRUE);
  }
  if (search_) {
    ::SendMessageW(search_, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(L"Search bookmarks"));
  }
}

void PhiBookmarkManager::LayoutChrome() {
  if (!alive()) {
    return;
  }
  RECT rc = {};
  ::GetClientRect(hwnd(), &rc);
  const int pad = Scaled(kPagePad);
  const int header_mid = rc.top + Scaled(kHeaderHeight) / 2;
  const int search_w = Scaled(kSearchWidth);
  const int search_h = Scaled(kSearchHeight);
  const int button_w = Scaled(kButtonWidth);
  const int button_h = Scaled(kButtonHeight);
  if (search_) {
    ::SetWindowPos(search_, nullptr, rc.right - pad - search_w,
                   header_mid - search_h / 2, search_w, search_h,
                   SWP_NOZORDER | SWP_NOACTIVATE);
  }
  new_folder_button_ = {rc.right - pad - search_w - Scaled(12) - button_w,
                        header_mid - button_h / 2,
                        rc.right - pad - search_w - Scaled(12),
                        header_mid + button_h / 2};
  import_button_ = new_folder_button_;
  ::OffsetRect(&import_button_, -(button_w + Scaled(8)), 0);
  // A narrow page has no room for it beside the title: the button goes,
  // and the app menu's Bookmarks ▸ Import bookmarks… stays.
  if (import_button_.left < rc.left + pad + Scaled(kTitleReserve)) {
    ::SetRectEmpty(&import_button_);
  }
}

void PhiBookmarkManager::Show() {
  EnsureWindow();
  if (!alive()) {
    return;
  }
  Reposition();
  Refresh();
  ShowPanel();
  BeginSession();
}

void PhiBookmarkManager::Hide() {
  EndSession();
  HidePanel();
}

void PhiBookmarkManager::BeginSession() {
  if (session_open_) {
    return;
  }
  session_open_ = true;
  session_edited_ = false;
  if (controller_ && controller_->window_host()) {
    controller_->window_host()->CaptureAnalytics(
        kAnalyticsModuleBookmarks, kEventBookmarkManagerOpened,
        base::DictValue());
  }
}

void PhiBookmarkManager::EndSession() {
  if (!session_open_) {
    return;
  }
  session_open_ = false;
  if (session_edited_ && controller_ && controller_->window_host()) {
    controller_->window_host()->CaptureAnalytics(
        kAnalyticsModuleBookmarks, kEventBookmarkManagerEdited,
        base::DictValue());
  }
  session_edited_ = false;
}

void PhiBookmarkManager::Reposition() {
  if (!alive() || !controller_) {
    return;
  }
  SyncDpiFromParent();
  SetPagePanelBounds(controller_->ContentRect(), Scaled(8));
  RaisePanel();
  LayoutChrome();
  Rebuild();
}

void PhiBookmarkManager::Refresh() {
  if (!alive()) {
    return;
  }
  Rebuild();
  // Re-reads QueryPalette() and repaints: a palette ramp moves the page
  // with the chrome.
  NotifyThemeChanged();
}

void PhiBookmarkManager::OnDpiChanged(UINT) {
  if (search_) {
    ::SendMessageW(search_, WM_SETFONT,
                   reinterpret_cast<WPARAM>(fonts().Get(kRowSize, FW_NORMAL)),
                   TRUE);
  }
  LayoutChrome();
  Rebuild();
}

void PhiBookmarkManager::OnSize(int, int) {
  LayoutChrome();
  Rebuild();
  Invalidate();
}

void PhiBookmarkManager::AppendSubtree(const std::string& parent_guid,
                                       int depth) {
  if (!controller_ || !controller_->sidebar_host()) {
    return;
  }
  for (PhiStoredRowView& view :
       controller_->sidebar_host()->BookmarkRows(controller_->slot().value,
                                         parent_guid)) {
    Row row;
    row.guid = view.guid;
    row.parent_guid = parent_guid;
    row.title = base::UTF8ToWide(!view.title.empty() ? view.title : view.url);
    row.url = view.url;
    row.title_b = base::UTF8ToWide(view.title_b);
    row.url_b = view.url_b;
    row.favicon_b = std::move(view.favicon_b_bgra);
    row.favicon_b_width = view.favicon_b_width;
    row.favicon_b_height = view.favicon_b_height;
    row.icon = view.icon;
    row.folder = view.folder;
    row.depth = depth;
    row.favicon = std::move(view.favicon_bgra);
    row.favicon_width = view.favicon_width;
    row.favicon_height = view.favicon_height;
    if (row.folder) {
      row.child_count = static_cast<int>(
          controller_->sidebar_host()
              ->BookmarkRows(controller_->slot().value, row.guid)
              .size());
      row.expanded = expanded_.count(row.guid) > 0;
    }
    const bool expanded = row.folder && row.expanded;
    const std::string guid = row.guid;
    rows_.push_back(std::move(row));
    if (expanded) {
      AppendSubtree(guid, depth + 1);
    }
  }
}

void PhiBookmarkManager::AppendMatches(const std::string& parent_guid) {
  if (!controller_ || !controller_->sidebar_host()) {
    return;
  }
  const std::wstring needle = Fold(Trim(query_));
  for (PhiStoredRowView& view :
       controller_->sidebar_host()->BookmarkRows(controller_->slot().value,
                                         parent_guid)) {
    const std::wstring title = base::UTF8ToWide(view.title);
    const std::wstring url = base::UTF8ToWide(view.url);
    const std::wstring title_b = base::UTF8ToWide(view.title_b);
    const std::wstring url_b = base::UTF8ToWide(view.url_b);
    const bool hit = Fold(title).find(needle) != std::wstring::npos ||
                     Fold(url).find(needle) != std::wstring::npos ||
                     Fold(title_b).find(needle) != std::wstring::npos ||
                     Fold(url_b).find(needle) != std::wstring::npos;
    const std::string guid = view.guid;
    const bool folder = view.folder;
    if (hit) {
      Row row;
      row.guid = view.guid;
      row.parent_guid = parent_guid;
      row.title =
          base::UTF8ToWide(!view.title.empty() ? view.title : view.url);
      row.url = view.url;
      row.title_b = title_b;
      row.url_b = view.url_b;
      row.favicon_b = std::move(view.favicon_b_bgra);
      row.favicon_b_width = view.favicon_b_width;
      row.favicon_b_height = view.favicon_b_height;
      row.icon = view.icon;
      row.folder = folder;
      // Results are FLAT root rows and folders are not expandable here, as
      // the spec requires — a flat index is not a sibling index.
      row.depth = 0;
      row.favicon = std::move(view.favicon_bgra);
      row.favicon_width = view.favicon_width;
      row.favicon_height = view.favicon_height;
      if (folder) {
        row.child_count = static_cast<int>(
            controller_->sidebar_host()
                ->BookmarkRows(controller_->slot().value, guid)
                .size());
      }
      rows_.push_back(std::move(row));
    }
    if (folder) {
      AppendMatches(guid);  // Depth-first source order.
    }
  }
}

void PhiBookmarkManager::Rebuild() {
  rows_.clear();
  if (!alive()) {
    ClearHits();
    return;
  }
  if (Trim(query_).empty()) {
    AppendSubtree(std::string(), 0);
  } else {
    AppendMatches(std::string());
  }
  LayoutRows();
}

void PhiBookmarkManager::LayoutRows() {
  // Clamp FIRST, against the new row count, so a delete at the bottom of a
  // scrolled list lays out flush instead of leaving a blank gap.
  ClampScroll();
  RECT rc = {};
  ::GetClientRect(hwnd(), &rc);
  const int pad = Scaled(kPagePad);
  int y = rc.top + Scaled(kHeaderHeight) - scroll_;
  for (Row& row : rows_) {
    row.bounds = {rc.left + pad, y, rc.right - pad, y + Scaled(kRowHeight)};
    const int text_left =
        row.bounds.left + row.depth * Scaled(kIndent) + Scaled(4);
    row.disclosure = {text_left, y + (Scaled(kRowHeight) - Scaled(kTwisty)) / 2,
                      text_left + Scaled(kTwisty),
                      y + (Scaled(kRowHeight) + Scaled(kTwisty)) / 2};
    y += Scaled(kRowHeight);
  }
  RebuildHits();
}

RECT PhiBookmarkManager::BodyRect() const {
  RECT rc = {};
  ::GetClientRect(hwnd(), &rc);
  rc.top += Scaled(kHeaderHeight);
  return rc;
}

bool PhiBookmarkManager::RowVisible(const Row& row) const {
  const RECT body = BodyRect();
  RECT shown = {};
  return ::IntersectRect(&shown, &row.bounds, &body) != FALSE;
}

void PhiBookmarkManager::RebuildHits() {
  ClearHits();
  // Rows are clipped to the body: a row scrolled under the header (or past
  // the bottom) is not pointable, as it is not painted.
  const RECT body = BodyRect();
  for (size_t i = 0; i < rows_.size(); ++i) {
    const Row& row = rows_[i];
    RECT shown = {};
    if (!::IntersectRect(&shown, &row.bounds, &body)) {
      continue;
    }
    AddHit(shown, static_cast<int>(i), row.title,
           row.folder ? PhiHitRole::kGroup : PhiHitRole::kListItem,
           selected_.count(row.guid) > 0);
  }
  // LAST, so it wins the pointer over a row scrolled under the header —
  // the two independent PtInRect tests this replaces could both answer yes.
  AddHit(new_folder_button_, kNewFolderKey, L"New Folder",
         PhiHitRole::kButton);
  if (!::IsRectEmpty(&import_button_)) {
    AddHit(import_button_, kImportKey,
           std::wstring(PhiStr("appMenu.importFromBrowser")),
           PhiHitRole::kButton);
  }
}

int PhiBookmarkManager::ContentHeight() const {
  return static_cast<int>(rows_.size()) * Scaled(kRowHeight);
}

void PhiBookmarkManager::ClampScroll() {
  if (!alive()) {
    return;
  }
  RECT rc = {};
  ::GetClientRect(hwnd(), &rc);
  const int visible = rc.bottom - rc.top - Scaled(kHeaderHeight);
  const int limit = std::max(0, ContentHeight() - visible);
  scroll_ = std::max(0, std::min(limit, scroll_));
}

int PhiBookmarkManager::RowAt(POINT pt) const {
  // Only the body is a row surface; the header covers whatever scrolled
  // under it.
  const RECT body = BodyRect();
  if (!::PtInRect(&body, pt)) {
    return -1;
  }
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (::PtInRect(&rows_[i].bounds, pt)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

std::vector<std::string> PhiBookmarkManager::SelectedGuids() const {
  std::vector<std::string> out;
  for (const Row& row : rows_) {
    if (selected_.count(row.guid) > 0) {
      out.push_back(row.guid);
    }
  }
  return out;
}

void PhiBookmarkManager::PaintRow(HDC dc,
                                  const Row& row,
                                  const PhiPalette& palette) {
  const bool is_selected = selected_.count(row.guid) > 0;
  if (is_selected) {
    const int r = Scaled(kRowRadius);
    PhiFillRoundRectAA(dc, row.bounds, r, palette.hover_strong);
    PhiStrokeRoundRectAA(dc, row.bounds, r, palette.accent, 1);
  }

  const int mid = (row.bounds.top + row.bounds.bottom) / 2;
  int x = row.bounds.left + row.depth * Scaled(kIndent) + Scaled(4);

  // Disclosure twisty — folders only, and never while a search flattens the
  // tree (the spec makes search results non-expandable).
  if (row.folder && Trim(query_).empty()) {
    RECT twisty = row.disclosure;
    ::SetTextColor(dc, palette.label_dim);
    HGDIOBJ prev = ::SelectObject(dc, fonts().Get(kRowSize, FW_NORMAL));
    ::DrawTextW(dc, row.expanded ? L"▾" : L"▸", -1, &twisty,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SelectObject(dc, prev);
  }
  x += Scaled(kTwisty) + Scaled(6);

  const int icon_px = Scaled(kIconSize);
  RECT icon = {x, mid - icon_px / 2, x + icon_px, mid + icon_px / 2};
  if (row.folder) {
    const std::string resolved = PhiResolveFolderIcon(row.icon);
    const PhiMacIcon* mask = resolved == PhiFolderIconDefaultId()
                                 ? PhiPlainFolderMask(row.expanded)
                                 : PhiFolderIconMask(resolved);
    if (mask) {
      const std::vector<unsigned char> bgra =
          PhiTintedIconBgra(*mask, palette.label_dim, icon_px);
      if (!bgra.empty()) {
        PhiBlendBgra(dc, icon, bgra.data(), icon_px, icon_px);
      }
    }
  } else if (!row.favicon.empty()) {
    PhiDrawBgra(dc, icon, row.favicon.data(), row.favicon_width,
                row.favicon_height);
  }
  x = icon.right + Scaled(8);
  // A split bookmark shows BOTH favicons, as its Website cell does on Mac.
  if (!row.url_b.empty() && !row.favicon_b.empty()) {
    RECT icon_b = {x, mid - icon_px / 2, x + icon_px, mid + icon_px / 2};
    PhiDrawBgra(dc, icon_b, row.favicon_b.data(), row.favicon_b_width,
                row.favicon_b_height);
    x = icon_b.right + Scaled(8);
  }

  const int body_width = row.bounds.right - row.bounds.left;
  const int address_left =
      row.bounds.right - body_width * kAddressPercent / 100;
  RECT title_rc = {x, row.bounds.top, address_left - Scaled(kColumnGap),
                   row.bounds.bottom};
  HGDIOBJ prev_font = ::SelectObject(dc, fonts().Get(kRowSize, FW_NORMAL));
  ::SetTextColor(dc, palette.label);
  ::DrawTextW(dc, row.title.c_str(), -1, &title_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);

  // Address column: a folder's child count, a bookmark's URL, or the split's
  // "primary | secondary".
  std::wstring address;
  if (row.folder) {
    address = base::UTF8ToWide(base::NumberToString(row.child_count)) +
              (row.child_count == 1 ? L" item" : L" items");
  } else if (!row.url_b.empty()) {
    address = base::UTF8ToWide(row.url) + L"  |  " + base::UTF8ToWide(row.url_b);
  } else {
    address = base::UTF8ToWide(row.url);
  }
  RECT address_rc = {address_left, row.bounds.top, row.bounds.right,
                     row.bounds.bottom};
  ::SelectObject(dc, fonts().Get(kDimSize, FW_NORMAL));
  ::SetTextColor(dc, palette.label_dim);
  ::DrawTextW(dc, address.c_str(), -1, &address_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
  ::SelectObject(dc, prev_font);
}

void PhiBookmarkManager::Paint(HDC dc, const RECT&) {
  if (!controller_) {
    return;
  }
  RECT rc = {};
  ::GetClientRect(hwnd(), &rc);
  // Live, as before: the slot's palette ramps between frames. The base's
  // cached palette() is what the theme bus repaints against.
  const PhiPalette palette = QueryPalette();
  HBRUSH ground = ::CreateSolidBrush(palette.window_bg);
  ::FillRect(dc, &rc, ground);
  ::DeleteObject(ground);
  ::SetBkMode(dc, TRANSPARENT);

  const int pad = Scaled(kPagePad);
  RECT title_rc = {rc.left + pad, rc.top, rc.left + pad + Scaled(300),
                   rc.top + Scaled(kHeaderHeight)};
  HGDIOBJ prev_font = ::SelectObject(dc, fonts().Get(kTitleSize, FW_SEMIBOLD));
  ::SetTextColor(dc, palette.label);
  ::DrawTextW(dc, L"Bookmarks", -1, &title_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  // The "Import…" and "New Folder" buttons.
  ::SelectObject(dc, fonts().Get(kButtonSize, FW_NORMAL));
  ::SetTextColor(dc, palette.label);
  const std::wstring import_label(PhiStr("bookmarkManager.import"));
  const struct {
    RECT bounds;
    int key;
    const wchar_t* label;
  } buttons[] = {{import_button_, kImportKey, import_label.c_str()},
                 {new_folder_button_, kNewFolderKey, L"New Folder"}};
  for (const auto& button : buttons) {
    if (::IsRectEmpty(&button.bounds)) {
      continue;
    }
    PhiFillRoundRectAA(
        dc, button.bounds, Scaled(6),
        HeaderButtonHot(button.key) ? palette.hover_strong : palette.hover);
    PhiStrokeRoundRectAA(dc, button.bounds, Scaled(6), palette.border, 1);
    RECT button_text = button.bounds;
    ::DrawTextW(dc, button.label, -1, &button_text,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                    DT_END_ELLIPSIS);
  }

  // A hairline under the header, then the rows.
  RECT rule = {rc.left + pad, rc.top + Scaled(kHeaderHeight) - 1,
               rc.right - pad, rc.top + Scaled(kHeaderHeight)};
  HBRUSH line = ::CreateSolidBrush(palette.separator);
  ::FillRect(dc, &rule, line);
  ::DeleteObject(line);
  ::SelectObject(dc, prev_font);

  if (rows_.empty()) {
    // The Mac's centred empty state; no synthetic node is added to the tree.
    RECT empty = {rc.left, rc.top + Scaled(kHeaderHeight), rc.right,
                  rc.bottom};
    HGDIOBJ prev = ::SelectObject(dc, fonts().Get(kRowSize, FW_NORMAL));
    ::SetTextColor(dc, palette.label_tertiary);
    ::DrawTextW(dc,
                Trim(query_).empty() ? L"No bookmarks yet"
                                     : L"No bookmarks match your search",
                -1, &empty,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SelectObject(dc, prev);
    return;
  }

  // Clip the list so rows never paint over the header.
  const RECT body = {rc.left, rc.top + Scaled(kHeaderHeight), rc.right,
                     rc.bottom};
  HRGN clip = ::CreateRectRgnIndirect(&body);
  ::SelectClipRgn(dc, clip);
  for (const Row& row : rows_) {
    if (row.bounds.bottom < body.top || row.bounds.top > body.bottom) {
      continue;
    }
    PaintRow(dc, row, palette);
  }
  ::SelectClipRgn(dc, nullptr);
  ::DeleteObject(clip);
}

void PhiBookmarkManager::ClickAt(POINT pt, bool ctrl, bool shift) {
  if (::PtInRect(&new_folder_button_, pt)) {
    NewFolder(std::string());
    return;
  }
  if (::PtInRect(&import_button_, pt)) {
    if (controller_ && controller_->window_host()) {
      controller_->window_host()->RequestBrowserImport();
    }
    return;
  }
  const int index = RowAt(pt);
  if (index < 0) {
    selected_.clear();
    Invalidate();
    return;
  }
  const Row& row = rows_[static_cast<size_t>(index)];
  if (row.folder && Trim(query_).empty() && ::PtInRect(&row.disclosure, pt)) {
    if (expanded_.count(row.guid) > 0) {
      expanded_.erase(row.guid);
    } else {
      expanded_.insert(row.guid);
    }
    Refresh();
    return;
  }

  if (ctrl) {
    if (selected_.count(row.guid) > 0) {
      selected_.erase(row.guid);
    } else {
      selected_.insert(row.guid);
      anchor_guid_ = row.guid;
    }
  } else if (shift && !anchor_guid_.empty()) {
    int anchor = -1;
    for (size_t i = 0; i < rows_.size(); ++i) {
      if (rows_[i].guid == anchor_guid_) {
        anchor = static_cast<int>(i);
        break;
      }
    }
    if (anchor >= 0) {
      selected_.clear();
      const int from = std::min(anchor, index);
      const int to = std::max(anchor, index);
      for (int i = from; i <= to; ++i) {
        selected_.insert(rows_[static_cast<size_t>(i)].guid);
      }
    }
  } else {
    selected_.clear();
    selected_.insert(row.guid);
    anchor_guid_ = row.guid;
  }
  RebuildHits();  // hits_ carries the selection for assistive tech.
  Invalidate();
}

void PhiBookmarkManager::OpenRow(const Row& row) {
  if (row.folder || row.url.empty() || !controller_ ||
      !controller_->sidebar_host()) {
    return;
  }
  controller_->sidebar_host()->RequestOpen(controller_->slot().value, row.url);
  if (!row.url_b.empty()) {
    controller_->sidebar_host()->RequestOpen(controller_->slot().value,
                                             row.url_b);
  }
}

void PhiBookmarkManager::OnDoubleClick(POINT pt) {
  const int index = RowAt(pt);
  if (index < 0) {
    return;
  }
  const Row row = rows_[static_cast<size_t>(index)];
  if (row.folder) {
    if (Trim(query_).empty()) {
      if (expanded_.count(row.guid) > 0) {
        expanded_.erase(row.guid);
      } else {
        expanded_.insert(row.guid);
      }
      Refresh();
    }
    return;
  }
  OpenRow(row);
}

void PhiBookmarkManager::NewFolder(const std::string& parent_guid) {
  if (!controller_ || !controller_->sidebar_host()) {
    return;
  }
  std::wstring name = PhiStr("common.bookmarkFolder.defaultName");
  if (!PhiPromptText(controller_->shell_hwnd(),
                     PhiStr("app.bookmarkFolderDialog.createTitle"),
                     PhiStr("bookmarkManager.folderName"), &name,
                     /*confirm_label=*/nullptr, kPhiNameRules)) {
    return;
  }
  if (!parent_guid.empty()) {
    expanded_.insert(parent_guid);
  }
  controller_->sidebar_host()->AddBookmarkFolder(controller_->slot().value,
                                                 parent_guid,
                                                 base::WideToUTF8(name));
  MarkEdited();
  Refresh();
}

void PhiBookmarkManager::EditRow(const Row& row) {
  if (!controller_ || !controller_->sidebar_host()) {
    return;
  }
  std::wstring name = row.title;
  if (PhiPromptText(controller_->shell_hwnd(),
                    PhiStr(row.folder ? "bookmarkManager.renameFolder"
                                      : "bookmarkManager.renameBookmark"),
                    PhiStr(row.folder ? "bookmarkManager.enterANewNameFor2"
                                      : "bookmarkManager.enterANewNameFor"),
                    &name, /*confirm_label=*/nullptr, kPhiNameRules)) {
    controller_->sidebar_host()->RenameStoredRow(row.guid,
                                                 base::WideToUTF8(name));
    MarkEdited();
  }
  Refresh();
}

void PhiBookmarkManager::DeleteSelection() {
  if (!controller_ || !controller_->sidebar_host()) {
    return;
  }
  const std::vector<std::string> guids = SelectedGuids();
  if (guids.empty()) {
    return;
  }
  // The spec: a multi-item or folder deletion confirms; a single leaf goes
  // straight through, as the sidebar already behaves.
  // A selection scrolled out of sight confirms too: Delete never removes
  // something the user cannot see without asking.
  bool needs_confirm = guids.size() > 1;
  bool folder = false;
  for (const Row& row : rows_) {
    if (selected_.count(row.guid) > 0) {
      folder = folder || row.folder;
      needs_confirm = needs_confirm || row.folder || !RowVisible(row);
    }
  }
  if (needs_confirm) {
    const std::wstring message =
        guids.size() > 1
            ? L"Delete the " +
                  base::UTF8ToWide(base::NumberToString(guids.size())) +
                  L" selected items?"
        : folder ? L"Delete this folder and everything inside it?"
                 : PhiStr("bookmarkManager.deleteThisBookmark");
    if (!PhiConfirm(controller_->shell_hwnd(), L"Delete Bookmarks",
                    message.c_str(), L"Delete")) {
      return;
    }
  }
  for (const std::string& guid : guids) {
    // A folder takes its subtree.
    controller_->sidebar_host()->RemoveStoredRow(guid);
  }
  MarkEdited();
  selected_.clear();
  Refresh();
}

void PhiBookmarkManager::OnContextMenu(POINT pt) {
  const int index = RowAt(pt);
  if (index < 0) {
    return;
  }
  const Row row = rows_[static_cast<size_t>(index)];
  // Right-clicking an unselected row replaces the selection; right-clicking
  // inside the batch keeps it (the spec's rule).
  if (selected_.count(row.guid) == 0) {
    selected_.clear();
    selected_.insert(row.guid);
    anchor_guid_ = row.guid;
    Invalidate();
  }
  const bool batch = selected_.size() > 1;
  const bool split = !row.url_b.empty();

  enum : UINT { kOpen = 1, kCopy, kCopyLeft, kCopyRight, kEdit, kNewNested,
                kDelete };
  HMENU menu = ::CreatePopupMenu();
  if (!menu) {
    return;
  }
  if (!batch) {
    if (!row.folder) {
      ::AppendMenuW(menu, MF_STRING, kOpen, L"Open");
      if (split) {
        ::AppendMenuW(menu, MF_STRING, kCopyLeft, L"Copy Left URL");
        ::AppendMenuW(menu, MF_STRING, kCopyRight, L"Copy Right URL");
      } else {
        ::AppendMenuW(menu, MF_STRING, kCopy, L"Copy Link");
      }
    }
    ::AppendMenuW(menu, MF_STRING, kEdit, L"Edit...");
    if (row.folder) {
      ::AppendMenuW(menu, MF_STRING, kNewNested, L"New Nested Folder...");
    }
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  }
  ::AppendMenuW(menu, MF_STRING, kDelete, L"Delete");

  POINT screen = pt;
  ::ClientToScreen(hwnd(), &screen);
  const UINT chosen = static_cast<UINT>(PhiTrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
      screen.x, screen.y, 0, hwnd(), nullptr));
  ::DestroyMenu(menu);

  switch (chosen) {
    case kOpen:
      OpenRow(row);
      break;
    case kCopy:
    case kCopyLeft:
      CopyToClipboard(hwnd(), base::UTF8ToWide(row.url));
      break;
    case kCopyRight:
      CopyToClipboard(hwnd(), base::UTF8ToWide(row.url_b));
      break;
    case kEdit:
      EditRow(row);
      break;
    case kNewNested:
      NewFolder(row.guid);
      break;
    case kDelete:
      DeleteSelection();
      break;
    default:
      break;
  }
}

bool PhiBookmarkManager::OnKey(WPARAM key) {
  if (key == VK_DELETE) {
    DeleteSelection();
    return true;
  }
  return PhiPanel::OnKey(key);
}

void PhiBookmarkManager::OnMouseDown(int, POINT pt) {
  ::SetFocus(hwnd());
  ClickAt(pt, (::GetKeyState(VK_CONTROL) & 0x8000) != 0,
          (::GetKeyState(VK_SHIFT) & 0x8000) != 0);
}

void PhiBookmarkManager::OnWheel(int delta, POINT) {
  // Scrolling moves the existing rows; the tree (and its favicons) is only
  // re-read when the store or the outline changes.
  const int before = scroll_;
  scroll_ -= delta / 2;
  ClampScroll();
  if (scroll_ == before) {
    return;
  }
  LayoutRows();
  Invalidate();
}

LRESULT PhiBookmarkManager::OnMessage(UINT message,
                                      WPARAM wparam,
                                      LPARAM lparam,
                                      bool* handled) {
  switch (message) {
    case WM_LBUTTONDBLCLK:
      *handled = true;
      OnDoubleClick({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
      return 0;
    case WM_RBUTTONUP:
      *handled = true;
      OnContextMenu({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
      return 0;
    case WM_COMMAND:
      if (HIWORD(wparam) == EN_CHANGE &&
          LOWORD(wparam) == static_cast<WORD>(kSearchId)) {
        const int length = ::GetWindowTextLengthW(search_);
        std::wstring text;
        if (length > 0) {
          text.resize(static_cast<size_t>(length) + 1, L'\0');
          const int copied =
              ::GetWindowTextW(search_, text.data(), length + 1);
          text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
        }
        query_ = text;
        scroll_ = 0;
        *handled = true;
        Refresh();
        return 0;
      }
      break;
    case WM_CTLCOLOREDIT:
      if (controller_) {
        const PhiPalette colors = QueryPalette();
        ::SetTextColor(reinterpret_cast<HDC>(wparam), colors.label);
        ::SetBkColor(reinterpret_cast<HDC>(wparam), colors.hover_strong);
        if (!field_brush_ || field_brush_color_ != colors.hover_strong) {
          if (field_brush_) {
            ::DeleteObject(field_brush_);
          }
          field_brush_ = ::CreateSolidBrush(colors.hover_strong);
          field_brush_color_ = colors.hover_strong;
        }
        *handled = true;
        return reinterpret_cast<LRESULT>(field_brush_);
      }
      break;
    default:
      break;
  }
  return PhiPanel::OnMessage(message, wparam, lparam, handled);
}

}  // namespace phi_client

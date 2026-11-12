// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_search_tabs_panel.h"

#include "client/phi_dwrite.h"

#include <windows.h>

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include "base/json/json_reader.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "client/phi_popover.h"
#include "client/phi_theme.h"
#include "client/phi_shell_window.h"

namespace phi_client {

namespace {

// Design metrics (DIP), the Mac panel's centered 680pt form.
constexpr int kPanelWidth = 680;
constexpr int kPanelHeight = 520;
constexpr int kFieldHeight = 40;
constexpr int kRowHeight = 36;
constexpr int kHeaderHeight = 26;
constexpr int kPad = 10;
// Section caps: a browse-y handful unfiltered, more once the user types.
constexpr size_t kUnfilteredCap = 6;
constexpr size_t kFilteredCap = 20;

// One visible line of the palette: either a section header or an openable
// result.
struct Row {
  enum class Kind { kHeader, kOpenTab, kPinned, kBookmark, kRecentlyClosed };
  Kind kind = Kind::kHeader;
  std::wstring title;
  std::wstring url;     // Dim, after the title.
  std::wstring detail;  // Right-aligned recency text.
  // Which of these means something depends on `kind`.
  int64_t tab_id = 0;
  int64_t window_id = 0;
  int64_t session_id = 0;
  std::string guid;
  std::string url_utf8;
};

std::wstring Lower(const std::wstring& text) {
  std::wstring out = text;
  ::CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
  return out;
}

// Every space-separated token must appear in the row's title or url.
bool Matches(const std::wstring& haystack_lower, const std::wstring& query) {
  size_t start = 0;
  while (start < query.size()) {
    size_t end = query.find(L' ', start);
    if (end == std::wstring::npos) {
      end = query.size();
    }
    if (end > start) {
      const std::wstring token = Lower(query.substr(start, end - start));
      if (haystack_lower.find(token) == std::wstring::npos) {
        return false;
      }
    }
    start = end + 1;
  }
  return true;
}

// The palette keeps its sources, its row model, its Paint() and its EDIT
// child. The window, the placement, the back buffer, hover, dismissal and
// lifetime belong to PhiPopover (lane 3 §6.2).
class Panel : public PhiPopover {
 public:
  explicit Panel(PhiShellWindow* controller) : controller_(controller) {}

  ~Panel() override {
    if (field_brush_) {
      ::DeleteObject(field_brush_);
    }
  }

  bool Open() {
    HWND owner = controller_ ? controller_->shell_hwnd() : nullptr;
    if (!owner) {
      return false;
    }
    ClassSpec spec;
    spec.name = L"PhiSearchTabsPanel";
    spec.cursor = nullptr;

    Options options;  // Activating, dismiss on inactive, 500 ms grace.
    options.dwm_round = false;  // The panel draws its own square rim.

    // Centred over the shell's upper fifth, then clamped into the work
    // area of the owner's monitor: a shell dragged half off screen (or
    // smaller than the palette) still gets the whole palette on screen.
    RECT owner_rect = {};
    ::GetWindowRect(owner, &owner_rect);
    Placement placement;
    placement.side = PhiPopoverSide::kExplicit;
    placement.clamp = true;
    placement.monitor_from_owner = true;

    SIZE probe = {1, 1};
    if (!CreatePopover(spec, owner, probe, placement, options)) {
      return false;
    }
    instance = this;  // Before Show(): activation churn can destroy us.
    SIZE size = {Scaled(kPanelWidth), Scaled(kPanelHeight)};
    // Never larger than the work area either (the list scrolls).
    MONITORINFO monitor = {sizeof(monitor)};
    if (::GetMonitorInfoW(::MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST),
                          &monitor)) {
      size.cx = std::min<LONG>(size.cx,
                               monitor.rcWork.right - monitor.rcWork.left);
      size.cy = std::min<LONG>(size.cy,
                               monitor.rcWork.bottom - monitor.rcWork.top);
    }
    const LONG x = (owner_rect.left + owner_rect.right) / 2 - size.cx / 2;
    const LONG y =
        owner_rect.top + (owner_rect.bottom - owner_rect.top) / 5;
    placement.anchor = {x, y, x, y};
    Reposition(placement, size);

    FetchSources();
    BuildRows(std::wstring());
    // The search field: a real EDIT so IME/caret/clipboard just work.
    field_ = ::CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT, Scaled(kPad) * 2,
        Scaled(kPad) + (Scaled(kFieldHeight) - Scaled(20)) / 2,
        size.cx - Scaled(kPad) * 4, Scaled(20), hwnd(), nullptr,
        ::GetModuleHandleW(nullptr), nullptr);
    ::SendMessageW(field_, WM_SETFONT,
                   reinterpret_cast<WPARAM>(FieldFont()), TRUE);
    ::SetWindowLongPtrW(field_, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(this));
    // The base WNDPROC is kept PER PANEL, not in a process global the way
    // the two EDIT subclass chains in the tree do it (lane 3 §13): one
    // more subclass on any EDIT would break a shared chain.
    field_base_proc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
        field_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&FieldProc)));
    ::SendMessageW(field_, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(L"Search tabs, bookmarks..."));
    Show();
    ::SetFocus(field_);
    return true;
  }

  PhiShellWindow* controller() const { return controller_; }

  static Panel* instance;

 private:
  HFONT FieldFont() { return fonts().Get(15, FW_NORMAL); }
  HFONT RowFont() { return fonts().Get(13, FW_NORMAL); }
  HFONT DimFont() { return fonts().Get(13, FW_NORMAL); }
  HFONT HeaderFont() { return fonts().Get(11, FW_SEMIBOLD); }

  // Reads every source ONCE at open; typing only re-filters.
  void FetchSources() {
    open_tabs_.clear();
    pinned_.clear();
    bookmarks_.clear();
    recently_closed_.clear();
    SidebarHost* host = controller_->sidebar_host();
    phi_framework::PhiFramework* framework = controller_->framework();
    const int64_t browser = controller_->active_browser_id().value;
    if (framework && browser) {
      std::optional<base::DictValue> data = base::JSONReader::ReadDict(
          framework->SearchTabsDataJson(browser), base::JSON_PARSE_RFC);
      if (data) {
        if (const base::ListValue* tabs = data->FindList("openTabs")) {
          for (const base::Value& v : *tabs) {
            const base::DictValue* d = v.GetIfDict();
            if (!d) {
              continue;
            }
            Row row;
            row.kind = Row::Kind::kOpenTab;
            if (const std::string* s = d->FindString("title")) {
              row.title = base::UTF8ToWide(*s);
            }
            if (const std::string* s = d->FindString("url")) {
              row.url = base::UTF8ToWide(*s);
            }
            if (const std::string* s = d->FindString("lastActiveText")) {
              row.detail = base::UTF8ToWide(*s);
            }
            row.tab_id =
                static_cast<int64_t>(d->FindDouble("tabId").value_or(0));
            row.window_id =
                static_cast<int64_t>(d->FindDouble("windowId").value_or(0));
            open_tabs_.push_back(std::move(row));
          }
        }
        if (const base::ListValue* closed =
                data->FindList("recentlyClosed")) {
          for (const base::Value& v : *closed) {
            const base::DictValue* d = v.GetIfDict();
            if (!d) {
              continue;
            }
            Row row;
            row.kind = Row::Kind::kRecentlyClosed;
            if (const std::string* s = d->FindString("title")) {
              row.title = base::UTF8ToWide(*s);
            }
            if (const std::string* s = d->FindString("url")) {
              row.url = base::UTF8ToWide(*s);
            }
            if (const std::string* s = d->FindString("lastActiveText")) {
              row.detail = base::UTF8ToWide(*s);
            }
            row.session_id =
                static_cast<int64_t>(d->FindDouble("sessionId").value_or(0));
            recently_closed_.push_back(std::move(row));
          }
        }
      }
    }
    if (host) {
      for (const PhiStoredRowView& pin :
           host->PinnedRows(controller_->slot().value)) {
        Row row;
        row.kind = Row::Kind::kPinned;
        row.title =
            base::UTF8ToWide(!pin.title.empty() ? pin.title : pin.url);
        row.url = base::UTF8ToWide(pin.url);
        row.guid = pin.guid;
        row.url_utf8 = pin.url;
        pinned_.push_back(std::move(row));
      }
      // Bookmarks flatten depth-first, folders skipped (the palette opens
      // pages, not folders — the Mac drills folders via a submenu we skip).
      const auto descend = [&](const auto& self,
                               const std::string& parent) -> void {
        for (const PhiStoredRowView& view :
             host->BookmarkRows(controller_->slot().value, parent)) {
          if (view.folder) {
            self(self, view.guid);
            continue;
          }
          if (view.url.empty()) {
            continue;
          }
          Row row;
          row.kind = Row::Kind::kBookmark;
          row.title =
              base::UTF8ToWide(!view.title.empty() ? view.title : view.url);
          row.url = base::UTF8ToWide(view.url);
          row.guid = view.guid;
          row.url_utf8 = view.url;
          bookmarks_.push_back(std::move(row));
        }
      };
      descend(descend, std::string());
    }
  }

  void AppendSection(const wchar_t* header, const std::vector<Row>& source,
                     const std::wstring& query) {
    const size_t cap = query.empty() ? kUnfilteredCap : kFilteredCap;
    size_t added = 0;
    for (const Row& row : source) {
      if (added >= cap) {
        break;
      }
      if (!query.empty() && !Matches(Lower(row.title + L" " + row.url),
                                     query)) {
        continue;
      }
      if (added == 0) {
        Row head;
        head.kind = Row::Kind::kHeader;
        head.title = header;
        rows_.push_back(std::move(head));
      }
      rows_.push_back(row);
      ++added;
    }
  }

  void BuildRows(const std::wstring& query) {
    rows_.clear();
    selected_ = -1;
    AppendSection(L"Open Tabs", open_tabs_, query);
    AppendSection(L"Pinned Tabs", pinned_, query);
    AppendSection(L"Bookmarks", bookmarks_, query);
    AppendSection(L"Recently Closed", recently_closed_, query);
    // Arm the first openable row, the Mac's default selection.
    for (size_t i = 0; i < rows_.size(); ++i) {
      if (rows_[i].kind != Row::Kind::kHeader) {
        selected_ = static_cast<int>(i);
        break;
      }
    }
    scroll_ = 0;
    BuildHits();
  }

  // THE layout pass: one hit per OPENABLE row (headers are painted, never
  // hit). Rebuilt whenever the rows or the scroll offset move.
  void BuildHits() {
    ClearHits();
    if (!alive()) {
      return;
    }
    const RECT list = ListRect();
    int y = list.top - scroll_;
    for (size_t i = 0; i < rows_.size(); ++i) {
      const int height = rows_[i].kind == Row::Kind::kHeader
                             ? Scaled(kHeaderHeight)
                             : Scaled(kRowHeight);
      if (rows_[i].kind != Row::Kind::kHeader) {
        RECT bounds = {list.left, std::max<LONG>(y, list.top), list.right,
                       std::min<LONG>(y + height, list.bottom)};
        if (bounds.bottom > bounds.top) {
          AddHit(bounds, static_cast<int>(i), rows_[i].title,
                 PhiHitRole::kListItem,
                 static_cast<int>(i) == selected_);
        }
      }
      y += height;
    }
  }

  void Requery() {
    const int len = ::GetWindowTextLengthW(field_);
    std::vector<wchar_t> buffer(static_cast<size_t>(len) + 1, L'\0');
    if (len > 0) {
      ::GetWindowTextW(field_, buffer.data(), len + 1);
    }
    BuildRows(buffer.data());
    Invalidate();
  }

  RECT ListRect() const {
    RECT rc;
    ::GetClientRect(hwnd(), &rc);
    rc.top += Scaled(kPad) + Scaled(kFieldHeight);
    rc.left += Scaled(kPad);
    rc.right -= Scaled(kPad);
    rc.bottom -= Scaled(kPad);
    return rc;
  }

  // The furthest the list scrolls: its last row flush with the bottom.
  int MaxScroll() const {
    const RECT list = ListRect();
    return std::max(0, RowTop(rows_.size()) -
                           static_cast<int>(list.bottom - list.top));
  }

  int RowTop(size_t index) const {
    int y = 0;
    for (size_t i = 0; i < index; ++i) {
      y += rows_[i].kind == Row::Kind::kHeader ? Scaled(kHeaderHeight)
                                               : Scaled(kRowHeight);
    }
    return y;
  }

  int RowAtY(int client_y) const {
    const RECT list = ListRect();
    int y = list.top - scroll_;
    for (size_t i = 0; i < rows_.size(); ++i) {
      const int h = rows_[i].kind == Row::Kind::kHeader
                        ? Scaled(kHeaderHeight)
                        : Scaled(kRowHeight);
      if (client_y >= y && client_y < y + h) {
        return rows_[i].kind == Row::Kind::kHeader ? -1
                                                   : static_cast<int>(i);
      }
      y += h;
    }
    return -1;
  }

  void MoveSelection(int direction) {
    if (rows_.empty()) {
      return;
    }
    int i = selected_;
    const int count = static_cast<int>(rows_.size());
    for (int step = 0; step < count; ++step) {
      i += direction;
      if (i < 0) {
        i = count - 1;
      } else if (i >= count) {
        i = 0;
      }
      if (rows_[static_cast<size_t>(i)].kind != Row::Kind::kHeader) {
        selected_ = i;
        EnsureVisible(static_cast<size_t>(i));
        BuildHits();
        Invalidate();
        return;
      }
    }
  }

  void EnsureVisible(size_t index) {
    const RECT list = ListRect();
    const int view = list.bottom - list.top;
    const int top = RowTop(index);
    const int bottom = top + Scaled(kRowHeight);
    if (top - scroll_ < 0) {
      scroll_ = top;
    } else if (bottom - scroll_ > view) {
      scroll_ = bottom - view;
    }
  }

  void Commit() {
    if (selected_ < 0 ||
        selected_ >= static_cast<int>(rows_.size())) {
      return;
    }
    const Row row = rows_[static_cast<size_t>(selected_)];
    PhiShellWindow* controller = controller_;
    // First — the action may refocus other windows. Everything below runs
    // on locals: `this` is gone once the window is.
    Destroy();
    SidebarHost* sidebar_host = controller->sidebar_host();
    WindowHost* window_host = controller->window_host();
    phi_framework::PhiFramework* framework = controller->framework();
    const int64_t browser = controller->active_browser_id().value;
    switch (row.kind) {
      case Row::Kind::kOpenTab:
        if (framework && framework->ActivateSearchTab(row.tab_id) &&
            window_host) {
          // The tab may live in another window (or a hidden Space) — the
          // client surfaces the right shell.
          window_host->FocusWindowForBrowser(row.window_id);
        }
        break;
      case Row::Kind::kPinned:
        controller->OpenOrFocusPinned(row.guid, row.url_utf8);
        break;
      case Row::Kind::kBookmark:
        if (sidebar_host) {
          sidebar_host->RequestOpen(controller->slot().value, row.url_utf8);
        }
        break;
      case Row::Kind::kRecentlyClosed:
        if (framework && browser) {
          framework->OpenRecentlyClosedEntry(browser, row.session_id);
        }
        break;
      case Row::Kind::kHeader:
        break;
    }
  }

  void PaintPanel(HDC dc) {
    RECT rc = {};
    ::GetClientRect(hwnd(), &rc);
    const PhiPalette& palette = PhiWindow::palette();
    HBRUSH bg = ::CreateSolidBrush(palette.card);
    ::FillRect(dc, &rc, bg);
    ::DeleteObject(bg);
    HPEN pen = ::CreatePen(PS_SOLID, 1, palette.border);
    HGDIOBJ old_pen = ::SelectObject(dc, pen);
    HGDIOBJ old_brush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
    // A hairline under the search field.
    ::MoveToEx(dc, rc.left,
               rc.top + Scaled(kPad) + Scaled(kFieldHeight) - Scaled(4),
               nullptr);
    ::LineTo(dc, rc.right,
             rc.top + Scaled(kPad) + Scaled(kFieldHeight) - Scaled(4));
    ::SelectObject(dc, old_brush);
    ::SelectObject(dc, old_pen);
    ::DeleteObject(pen);

    ::SetBkMode(dc, TRANSPARENT);
    const RECT list = ListRect();
    ::IntersectClipRect(dc, list.left, list.top, list.right, list.bottom);
    int y = list.top - scroll_;
    for (size_t i = 0; i < rows_.size(); ++i) {
      const Row& row = rows_[i];
      if (row.kind == Row::Kind::kHeader) {
        RECT r = {list.left + Scaled(6), y, list.right - Scaled(6),
                  y + Scaled(kHeaderHeight)};
        ::SelectObject(dc, HeaderFont());
        ::SetTextColor(dc, palette.label_dim);
        ::DrawTextW(dc, row.title.c_str(), -1, &r,
                    DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
        y += Scaled(kHeaderHeight);
        continue;
      }
      RECT r = {list.left, y, list.right, y + Scaled(kRowHeight)};
      if (static_cast<int>(i) == selected_) {
        PhiFillRoundRectAA(dc, r, Scaled(6), palette.hover_strong);
      }
      // Recency right-aligned; title then dim url fill the rest.
      RECT detail_rc = {r.right - Scaled(150), r.top, r.right - Scaled(10),
                        r.bottom};
      if (!row.detail.empty()) {
        ::SelectObject(dc, DimFont());
        ::SetTextColor(dc, palette.label_tertiary);
        ::DrawTextW(dc, row.detail.c_str(), -1, &detail_rc,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      }
      RECT text_rc = {r.left + Scaled(10), r.top,
                      detail_rc.left - Scaled(8), r.bottom};
      ::SelectObject(dc, RowFont());
      ::SetTextColor(dc, palette.label);
      RECT measure = text_rc;
      ::DrawTextW(dc, row.title.c_str(), -1, &measure,
                  DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
      RECT title_rc = text_rc;
      title_rc.right = std::min(
          text_rc.right,
          static_cast<LONG>(text_rc.left + (measure.right - measure.left)));
      ::DrawTextW(dc, row.title.c_str(), -1, &title_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
      if (!row.url.empty() && title_rc.right + Scaled(16) < text_rc.right) {
        RECT url_rc = {title_rc.right + Scaled(8), r.top, text_rc.right,
                       r.bottom};
        ::SetTextColor(dc, palette.label_dim);
        const std::wstring dim = L"— " + row.url;
        ::DrawTextW(dc, dim.c_str(), -1, &url_rc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
      }
      y += Scaled(kRowHeight);
    }
    ::SelectClipRgn(dc, nullptr);
  }

  static LRESULT CALLBACK FieldProc(HWND hwnd, UINT msg, WPARAM wparam,
                                    LPARAM lparam) {
    Panel* self = reinterpret_cast<Panel*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
      case WM_KEYDOWN:
        if (!self) {
          break;
        }
        if (wparam == VK_DOWN || wparam == VK_UP) {
          self->MoveSelection(wparam == VK_DOWN ? 1 : -1);
          return 0;
        }
        if (wparam == VK_RETURN) {
          self->Commit();  // Destroys the panel.
          return 0;
        }
        if (wparam == VK_ESCAPE) {
          self->Destroy();
          return 0;
        }
        break;
      case WM_CHAR:
        if (wparam == VK_RETURN || wparam == VK_ESCAPE) {
          return 0;  // Handled above; the EDIT would only beep.
        }
        break;
    }
    if (!self || !self->field_base_proc_) {
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    return ::CallWindowProcW(self->field_base_proc_, hwnd, msg, wparam,
                             lparam);
  }

  // The EDIT is a real child control, so WM_COMMAND / WM_CTLCOLOREDIT
  // still come through the parent proc; everything else the panel used to
  // hand-roll (paint framing, hover, dismissal, Escape) is the base's.
  LRESULT OnMessage(UINT message, WPARAM w, LPARAM l,
                    bool* handled) override {
    switch (message) {
      case WM_COMMAND:
        if (reinterpret_cast<HWND>(l) == field_ && HIWORD(w) == EN_CHANGE) {
          *handled = true;
          Requery();
          return 0;
        }
        break;
      case WM_CTLCOLOREDIT: {
        *handled = true;
        const PhiPalette& colors = palette();
        HDC edit_dc = reinterpret_cast<HDC>(w);
        ::SetTextColor(edit_dc, colors.label);
        ::SetBkColor(edit_dc, colors.card);
        // The brush is cached BY COLOUR: the old code created it once and
        // never compared it again, so a theme switch left the field
        // painted in the previous palette (lane 3 §3.5, the one wrong copy
        // of the four "cached brush by colour" idioms).
        if (!field_brush_ || field_brush_color_ != colors.card) {
          if (field_brush_) {
            ::DeleteObject(field_brush_);
          }
          field_brush_ = ::CreateSolidBrush(colors.card);
          field_brush_color_ = colors.card;
        }
        return reinterpret_cast<LRESULT>(field_brush_);
      }
      default:
        break;
    }
    return PhiPopover::OnMessage(message, w, l, handled);
  }

  PhiPalette QueryPalette() const override {
    return controller_->CurrentPalette();
  }

  void Paint(HDC dc, const RECT&) override { PaintPanel(dc); }

  void OnHover(int hit, int) override {
    if (hit < 0) {
      return;  // A header or the gutter keeps the current selection.
    }
    const int row = hits_[static_cast<size_t>(hit)].key;
    if (row != selected_) {
      selected_ = row;
      Invalidate();
    }
  }

  void OnMouseDown(int hit, POINT) override {
    if (hit < 0) {
      return;
    }
    selected_ = hits_[static_cast<size_t>(hit)].key;
    Commit();  // Destroys the panel.
  }

  void OnWheel(int delta, POINT) override {
    scroll_ = std::clamp(scroll_ - delta / 2, 0, MaxScroll());
    BuildHits();
    Invalidate();
  }

  void OnDpiChanged(UINT) override {
    SIZE size = {Scaled(kPanelWidth), Scaled(kPanelHeight)};
    ::SetWindowPos(hwnd(), nullptr, 0, 0, size.cx, size.cy,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (field_) {
      // fonts() dropped every HFONT; the EDIT is holding a dead handle.
      ::SendMessageW(field_, WM_SETFONT,
                     reinterpret_cast<WPARAM>(FieldFont()), TRUE);
      ::SetWindowPos(field_, nullptr, Scaled(kPad) * 2,
                     Scaled(kPad) + (Scaled(kFieldHeight) - Scaled(20)) / 2,
                     size.cx - Scaled(kPad) * 4, Scaled(20),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    BuildHits();
  }

  void OnNcDestroy() override {
    if (instance == this) {
      instance = nullptr;
    }
  }

  // The controller owns the shell that owns this popup.
  RAW_PTR_EXCLUSION PhiShellWindow* const controller_;
  HWND field_ = nullptr;
  WNDPROC field_base_proc_ = nullptr;
  HBRUSH field_brush_ = nullptr;
  COLORREF field_brush_color_ = 0;
  std::vector<Row> open_tabs_;
  std::vector<Row> pinned_;
  std::vector<Row> bookmarks_;
  std::vector<Row> recently_closed_;
  std::vector<Row> rows_;
  int selected_ = -1;
  int scroll_ = 0;
};

Panel* Panel::instance = nullptr;

}  // namespace

// static
void PhiSearchTabsPanel::Toggle(PhiShellWindow* controller) {
  if (Panel::instance) {
    Panel::instance->Dismiss();
    return;
  }
  if (!controller) {
    return;
  }
  Panel* panel = new Panel(controller);
  panel->DeleteWhenDestroyed();
  if (!panel->Open()) {
    delete panel;
  }
}

// static
void PhiSearchTabsPanel::CloseIfBoundTo(const PhiShellWindow* controller) {
  if (Panel::instance && Panel::instance->controller() == controller) {
    // WM_NCDESTROY clears the singleton and deletes the panel.
    Panel::instance->Dismiss();
  }
}

}  // namespace phi_client

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_url_rules_window.h"

#include <commctrl.h>  // EM_SETCUEBANNER
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <set>
#include <utility>

#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_menu_paint.h"
#include "phibrowser-win/client/phi_panel.h"
#include "phibrowser-win/client/phi_popup_chrome.h"
#include "phibrowser-win/client/phi_profile_service.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_settings_window.h"
#include "phibrowser-win/client/phi_space_manager.h"

namespace phi_client {

namespace {

constexpr wchar_t kClassName[] = L"PhiUrlRulesWindow";
constexpr wchar_t kListClassName[] = L"PhiUrlRulesList";

// The Mac window's ideal size (700x460 of content below the band). Design
// units throughout; Scaled() at use.
constexpr int kWidth = 700;
constexpr int kHeight = 460;
// Header: the Mac's 16 horizontal / 12 vertical padding around the title
// and the wrapped subtitle.
constexpr int kHeaderPadX = 16;
constexpr int kHeaderPadY = 12;
constexpr int kTitleHeight = 18;
constexpr int kTitleGap = 4;
// Footer: Add Rule leading, Cancel / Save trailing.
constexpr int kFooterHeight = 54;
constexpr int kFooterPadX = 16;
constexpr int kButtonHeight = 30;
constexpr int kAddWidth = 110;
constexpr int kActionWidth = 96;
constexpr int kButtonGap = 8;
// The list: the Mac's inset table — rows 4 apart on the window ground,
// each row a card two lines tall.
constexpr int kListPad = 12;
constexpr int kRowHeight = 84;
constexpr int kRowGap = 6;
constexpr int kRowPad = 12;
constexpr int kLineHeight = 28;
constexpr int kLine1Y = 10;
constexpr int kLine2Y = 46;
constexpr int kTypeWidth = 150;   // The Mac's matchTypePopup width.
constexpr int kTargetWidth = 220;
constexpr int kRemoveWidth = 24;  // The Mac's 22 plus its hit slack.
constexpr int kControlGap = 10;
constexpr int kRadius = 8;
constexpr int kPillRadius = 6;
constexpr int kScrollbarWidth = 6;
constexpr int kWheelStep = 48;
constexpr int kDragThreshold = 4;

// Hit keys: (row + 1) * 4 + part.
enum RowPart { kPartType = 1, kPartTarget = 2, kPartRemove = 3 };

enum : UINT_PTR {
  kIdAdd = 101,
  kIdCancel,
  kIdSave,
  // The rows' EDIT fields: kFieldIdBase + Row::id.
  kFieldIdBase = 1000,
};

PhiUrlRulesWindow* g_window = nullptr;
// WM_CLOSE's wParam (unused by the system) marking a close that skips the
// discard prompt: CloseForSignOut.
constexpr WPARAM kCloseWithoutPrompt = 1;

std::wstring Widen(const std::string& utf8) {
  return base::UTF8ToWide(utf8);
}
std::string Narrow(const std::wstring& wide) {
  return base::WideToUTF8(wide);
}

std::wstring WindowText(HWND hwnd) {
  const int length = ::GetWindowTextLengthW(hwnd);
  std::wstring text(static_cast<size_t>(length) + 1, L'\0');
  const int copied = ::GetWindowTextW(hwnd, text.data(), length + 1);
  text.resize(static_cast<size_t>(std::max(copied, 0)));
  return text;
}

}  // namespace

// ---------------------------------------------------------------------------
// The rule list: a child panel that paints the rows, owns their EDIT
// fields (so the panel's client rect clips them while scrolling), takes the
// pill and remove clicks, and drags rows to reorder.

class PhiUrlRulesWindow::RuleList : public PhiPanel {
 public:
  explicit RuleList(PhiUrlRulesWindow* window) : window_(window) {}
  ~RuleList() override = default;

  bool Mount(const RECT& bounds) {
    ClassSpec spec;
    spec.name = kListClassName;
    spec.style = CS_DBLCLKS;
    Options options;
    // WS_EX_CONTROLPARENT: the dialog manager's Tab walk descends into the
    // panel to reach the fields.
    options.style = WS_CLIPCHILDREN;
    options.ex_style = WS_EX_CONTROLPARENT;
    options.in_card_stack = false;  // The window's only child panel.
    if (!CreatePanel(spec, window_->hwnd(), bounds, options)) {
      return false;
    }
    RebuildBrushes();
    ShowPanel();
    return true;
  }

  // The window's rows moved (loaded, added, removed, reordered): create the
  // fields the rows lack, drop the ones whose row is gone, place them all
  // and rebuild the hit list. Never called from Paint().
  void Relayout() {
    if (!alive()) {
      return;
    }
    std::vector<Row>& rows = window_->rows();
    // Fields follow rows by id.
    std::set<int> live;
    for (const Row& row : rows) {
      live.insert(row.id);
      if (!FieldFor(row.id)) {
        CreateField(row);
      }
    }
    for (size_t i = 0; i < fields_.size();) {
      if (live.count(fields_[i].first) == 0) {
        ::DestroyWindow(fields_[i].second);
        fields_.erase(fields_.begin() + static_cast<ptrdiff_t>(i));
      } else {
        ++i;
      }
    }
    // The scroll range.
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    const int viewport = static_cast<int>(client.bottom - client.top);
    content_height_ = Scaled(kListPad) +
                      static_cast<int>(rows.size()) *
                          Scaled(kRowHeight + kRowGap) -
                      (rows.empty() ? 0 : Scaled(kRowGap)) +
                      Scaled(kListPad);
    scroll_ = std::clamp(scroll_, 0, std::max(0, content_height_ - viewport));
    // Place the fields and rebuild the hits.
    ClearHits();
    for (size_t i = 0; i < rows.size(); ++i) {
      const Geometry g = GeometryFor(static_cast<int>(i));
      if (HWND field = FieldFor(rows[i].id)) {
        ::SetWindowPos(field, nullptr, g.edit.left, g.edit.top,
                       g.edit.right - g.edit.left, g.edit.bottom - g.edit.top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        ::SendMessageW(field, EM_SETCUEBANNER, TRUE,
                       reinterpret_cast<LPARAM>(
                           window_->MatchValuePlaceholder(rows[i].type)));
      }
      const int base = (static_cast<int>(i) + 1) * 4;
      AddHit(g.type, base + kPartType, window_->MatchTypeLabel(rows[i].type),
             PhiHitRole::kButton);
      AddHit(g.target, base + kPartTarget, window_->TargetLabel(rows[i]),
             PhiHitRole::kButton);
      AddHit(g.remove, base + kPartRemove,
             PhiStr("sidebar.urlRulesEditor.removeRuleButton.tooltip"),
             PhiHitRole::kButton);
    }
    Invalidate();
  }

  void ScrollToBottom() {
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    scroll_ = std::max(
        0, content_height_ - static_cast<int>(client.bottom - client.top));
    Relayout();
  }

  // Scrolls just far enough that row `index` is fully in view.
  void ScrollToRow(int index) {
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    const int viewport = static_cast<int>(client.bottom - client.top);
    const int top = index * Scaled(kRowHeight + kRowGap);
    const int bottom = top + Scaled(kRowHeight) + 2 * Scaled(kListPad);
    if (top < scroll_) {
      scroll_ = top;
    } else if (bottom > scroll_ + viewport) {
      scroll_ = bottom - viewport;
    }
    Relayout();  // Clamps to the content.
  }

  void FocusField(int row_id) {
    if (HWND field = FieldFor(row_id)) {
      ::SetFocus(field);
    }
  }

  // Reads every field back into its row — the values the EN_CHANGE
  // notifications already carry, re-read once at Save so a change the
  // notification missed (an IME commit) cannot be lost.
  void CommitFields() {
    for (Row& row : window_->rows()) {
      if (HWND field = FieldFor(row.id)) {
        row.value = Narrow(WindowText(field));
      }
    }
  }

  void RebuildBrushes() {
    for (HBRUSH* slot : {&bg_brush_, &field_brush_}) {
      if (*slot) {
        ::DeleteObject(*slot);
        *slot = nullptr;
      }
    }
    const PhiPopupColors& c = window_->popup_colors();
    bg_brush_ = ::CreateSolidBrush(c.bg);
    field_brush_ = ::CreateSolidBrush(FieldColor());
    for (const auto& [id, field] : fields_) {
      PhiPopupThemeControl(field, c.dark);
    }
    Invalidate();
  }

  void ApplyFonts() {
    for (const auto& [id, field] : fields_) {
      ::SendMessageW(field, WM_SETFONT,
                     reinterpret_cast<WPARAM>(fonts().Get(13, FW_NORMAL)),
                     TRUE);
    }
  }

 private:
  struct Geometry {
    RECT row;
    RECT type;
    RECT field;   // The drawn well.
    RECT edit;    // The EDIT inset in it.
    RECT remove;
    RECT open_in;
    RECT target;
  };

  Geometry GeometryFor(int index) const {
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    Geometry g;
    const int pad = Scaled(kListPad);
    const int top = pad + index * Scaled(kRowHeight + kRowGap) - scroll_;
    g.row = {pad, top, client.right - pad - Scaled(kScrollbarWidth),
             top + Scaled(kRowHeight)};
    const int inner_pad = Scaled(kRowPad);
    const int line1 = g.row.top + Scaled(kLine1Y);
    const int line2 = g.row.top + Scaled(kLine2Y);
    const int line_h = Scaled(kLineHeight);
    g.type = {g.row.left + inner_pad, line1, g.row.left + inner_pad +
              Scaled(kTypeWidth), line1 + line_h};
    g.remove = {g.row.right - inner_pad - Scaled(kRemoveWidth), line1,
                g.row.right - inner_pad, line1 + line_h};
    g.field = {g.type.right + Scaled(kControlGap), line1,
               g.remove.left - Scaled(kControlGap), line1 + line_h};
    g.edit = {g.field.left + Scaled(8), g.field.top + Scaled(5),
              g.field.right - Scaled(8), g.field.bottom - Scaled(5)};
    g.target = {g.field.right - Scaled(kTargetWidth), line2, g.field.right,
                line2 + line_h};
    g.open_in = {g.type.left, line2, g.target.left - Scaled(8),
                 line2 + line_h};
    return g;
  }

  HWND FieldFor(int row_id) const {
    for (const auto& [id, field] : fields_) {
      if (id == row_id) {
        return field;
      }
    }
    return nullptr;
  }

  void CreateField(const Row& row) {
    HWND field = ::CreateWindowExW(
        0, L"EDIT", Widen(row.value).c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL, 0, 0,
        10, 10, hwnd(),
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kFieldIdBase + row.id)),
        ::GetModuleHandleW(nullptr), nullptr);
    if (!field) {
      return;
    }
    ::SendMessageW(field, WM_SETFONT,
                   reinterpret_cast<WPARAM>(fonts().Get(13, FW_NORMAL)),
                   TRUE);
    PhiPopupThemeControl(field, window_->popup_colors().dark);
    ::SetWindowSubclass(field, &PhiUrlRulesWindow::ControlProc,
                        static_cast<UINT_PTR>(kFieldIdBase + row.id),
                        reinterpret_cast<DWORD_PTR>(window_));
    fields_.emplace_back(row.id, field);
  }

  COLORREF FieldColor() const {
    const PhiPopupColors& c = window_->popup_colors();
    // The field well: the ground colour on the card — reads as a bezel
    // against the row without a second stroke fighting the pills.
    return PhiPopupBlend(c.ink, c.card, c.dark ? 18 : 8);
  }

  // The row under `y`, or -1.
  int RowAt(int y) const {
    const std::vector<Row>& rows = window_->rows();
    for (size_t i = 0; i < rows.size(); ++i) {
      const Geometry g = GeometryFor(static_cast<int>(i));
      if (y >= g.row.top && y < g.row.bottom) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  // The slot a dragged row belongs in when the cursor is at `y`: the row
  // whose band the cursor is in, by centre line.
  int DropIndexFor(int y) const {
    const int n = static_cast<int>(window_->rows().size());
    if (n == 0) {
      return 0;
    }
    const int pitch = Scaled(kRowHeight + kRowGap);
    const int index = (y + scroll_ - Scaled(kListPad)) / pitch;
    return std::clamp(index, 0, n - 1);
  }

  // PhiWindow.
  void Paint(HDC dc, const RECT&) override {
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    ::FillRect(dc, &client, bg_brush_);
    const PhiPopupColors& c = window_->popup_colors();
    ::SetBkMode(dc, TRANSPARENT);
    HFONT body = fonts().Get(13, FW_NORMAL);
    HFONT small = fonts().Get(11, FW_NORMAL);
    HFONT glyphs = fonts().Get(PhiFontDesc{12, FW_NORMAL,
                                           PhiIconFaceName()});
    HGDIOBJ previous = ::SelectObject(dc, body);

    const std::vector<Row>& rows = window_->rows();
    if (rows.empty()) {
      // The Mac's centred empty overlay: "No rules yet." over its hint.
      RECT title = client;
      title.bottom = (client.top + client.bottom) / 2 - Scaled(4);
      ::SetTextColor(dc, c.secondary);
      ::DrawTextW(dc, PhiStr("sidebar.urlRulesEditor.emptyTitle"), -1, &title,
                  DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
      ::SelectObject(dc, small);
      RECT hint = client;
      hint.top = title.bottom + Scaled(8);
      hint.left += Scaled(24);
      hint.right -= Scaled(24);
      ::DrawTextW(dc, PhiStr("sidebar.urlRulesEditor.emptyHint"), -1, &hint,
                  DT_CENTER | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
      ::SelectObject(dc, previous);
      return;
    }

    const int hot_hit = hovered();
    const int hot_key = hot_hit >= 0 && hot_hit < static_cast<int>(hits().size())
                            ? hits()[static_cast<size_t>(hot_hit)].key
                            : 0;
    for (size_t i = 0; i < rows.size(); ++i) {
      const Row& row = rows[i];
      const Geometry g = GeometryFor(static_cast<int>(i));
      if (g.row.bottom < client.top || g.row.top > client.bottom) {
        continue;
      }
      const bool lifted = dragging_ && static_cast<int>(i) == press_row_;
      // The card, lifted with a wash while it is being dragged.
      PhiFillRoundRectAA(dc, g.row, Scaled(kRadius),
                         lifted ? PhiPopupBlend(c.accent, c.card, 30) : c.card);
      // A row the last Save refused: outlined in the destructive red until
      // it is edited.
      PhiStrokeRoundRectAA(
          dc, g.row, Scaled(kRadius),
          lifted        ? c.accent
          : row.invalid ? c.destructive
                        : c.card_border,
          row.invalid && !lifted ? Scaled(2) : 1);
      const int base = (static_cast<int>(i) + 1) * 4;
      // Line 1: the match-type pill, the field well, the remove glyph.
      PaintPill(dc, g.type, window_->MatchTypeLabel(row.type),
                hot_key == base + kPartType, c, body);
      PhiFillRoundRectAA(dc, g.field, Scaled(kPillRadius), FieldColor());
      PhiStrokeRoundRectAA(dc, g.field, Scaled(kPillRadius), c.card_border, 1);
      ::SelectObject(dc, glyphs);
      ::SetTextColor(dc, hot_key == base + kPartRemove ? c.destructive
                                                       : c.secondary);
      RECT remove = g.remove;
      PhiDrawTextAA(dc, L"\uE74D", -1, &remove,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);  // MDL2 Delete.
      // Line 2: "Open in" and the destination pill.
      ::SelectObject(dc, body);
      ::SetTextColor(dc, c.secondary);
      RECT open_in = g.open_in;
      ::DrawTextW(dc, PhiStr("sidebar.urlRulesEditor.targetSpaceLabel"), -1,
                  &open_in,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                      DT_END_ELLIPSIS);
      PaintPill(dc, g.target, window_->TargetLabel(row).c_str(),
                hot_key == base + kPartTarget, c, body);
    }
    // A thin thumb when the rows outgrow the viewport.
    const int viewport = static_cast<int>(client.bottom - client.top);
    if (content_height_ > viewport && viewport > 0) {
      const int track = viewport - 2 * Scaled(4);
      const int thumb_h = std::max(Scaled(24), track * viewport /
                                                   content_height_);
      const int thumb_y =
          Scaled(4) + (track - thumb_h) * scroll_ /
                          std::max(1, content_height_ - viewport);
      RECT thumb = {client.right - Scaled(kScrollbarWidth) - Scaled(2),
                    thumb_y, client.right - Scaled(2), thumb_y + thumb_h};
      PhiFillRoundRectAA(dc, thumb, Scaled(2), PhiPopupBlend(c.ink, c.bg, 60));
    }
    ::SelectObject(dc, previous);
  }

  // The settings pane's kPopup pill: a wash, the label, the "↕" cue.
  void PaintPill(HDC dc,
                 const RECT& bounds,
                 const wchar_t* text,
                 bool hot,
                 const PhiPopupColors& c,
                 HFONT font) {
    PhiFillRoundRectAA(dc, bounds, Scaled(kPillRadius),
                       PhiPopupBlend(c.ink, c.card, hot ? 30 : 15));
    ::SelectObject(dc, font);
    ::SetTextColor(dc, c.label);
    RECT label = bounds;
    label.left += Scaled(10);
    label.right -= Scaled(18);
    ::DrawTextW(dc, text, -1, &label,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                    DT_END_ELLIPSIS);
    RECT arrows = bounds;
    arrows.left = bounds.right - Scaled(16);
    ::SetTextColor(dc, c.secondary);
    ::DrawTextW(dc, L"\u2195", -1, &arrows,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

  void OnSize(int, int) override { Relayout(); }

  void OnMouseDown(int hit, POINT pt) override {
    if (hit >= 0) {
      return;  // A pill or the remove glyph: answered on the click.
    }
    // A press on a row's own surface arms a reorder drag (the Mac's
    // drag-from-the-row's-empty-area).
    press_row_ = RowAt(pt.y);
    if (press_row_ < 0) {
      return;
    }
    press_pt_ = pt;
    dragging_ = false;
    capture_ = BeginCapture();
  }

  void OnClick(int hit, POINT) override {
    const bool was_dragging = dragging_;
    capture_.Release();
    dragging_ = false;
    press_row_ = -1;
    if (was_dragging || hit < 0 ||
        hit >= static_cast<int>(hits().size())) {
      return;
    }
    const int key = hits()[static_cast<size_t>(hit)].key;
    const int row = key / 4 - 1;
    if (row < 0 || row >= static_cast<int>(window_->rows().size())) {
      return;
    }
    const Geometry g = GeometryFor(row);
    switch (key % 4) {
      case kPartType:
        window_->PickMatchType(row, g.type);
        break;
      case kPartTarget:
        window_->PickTarget(row, g.target);
        break;
      case kPartRemove:
        window_->RemoveRow(row);
        break;
      default:
        break;
    }
  }

  void OnCaptureLost() override {
    capture_.Release();
    if (dragging_) {
      dragging_ = false;
      Invalidate();
    }
    press_row_ = -1;
  }

  void OnWheel(int delta, POINT) override {
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    const int limit = std::max(
        0, content_height_ - static_cast<int>(client.bottom - client.top));
    const int next =
        std::clamp(scroll_ - delta * Scaled(kWheelStep) / WHEEL_DELTA, 0, limit);
    if (next == scroll_) {
      return;
    }
    scroll_ = next;
    Relayout();
  }

  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) override {
    switch (message) {
      case WM_MOUSEMOVE:
        if (capture_.held() && press_row_ >= 0) {
          const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
          if (!dragging_ &&
              std::abs(pt.y - press_pt_.y) >= Scaled(kDragThreshold)) {
            dragging_ = true;
            ::SetFocus(hwnd());  // Leave the field: the drag owns the mouse.
          }
          if (dragging_) {
            const int to = DropIndexFor(pt.y);
            if (to != press_row_) {
              std::vector<Row>& rows = window_->rows();
              Row moved = std::move(rows[static_cast<size_t>(press_row_)]);
              rows.erase(rows.begin() + press_row_);
              rows.insert(rows.begin() + to, std::move(moved));
              press_row_ = to;
              Relayout();
            }
          }
        }
        break;
      case WM_COMMAND: {
        const UINT_PTR id = LOWORD(wparam);
        if (HIWORD(wparam) == EN_CHANGE && id >= kFieldIdBase) {
          HWND field = reinterpret_cast<HWND>(lparam);
          window_->SetRowValue(static_cast<int>(id - kFieldIdBase),
                               Narrow(WindowText(field)));
          *handled = true;
          return 0;
        }
        break;
      }
      case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        const PhiPopupColors& c = window_->popup_colors();
        ::SetTextColor(dc, c.label);
        ::SetBkColor(dc, FieldColor());
        *handled = true;
        return reinterpret_cast<LRESULT>(field_brush_);
      }
      default:
        break;
    }
    return PhiPanel::OnMessage(message, wparam, lparam, handled);
  }

  void OnNcDestroy() override {
    for (HBRUSH* slot : {&bg_brush_, &field_brush_}) {
      if (*slot) {
        ::DeleteObject(*slot);
        *slot = nullptr;
      }
    }
    fields_.clear();  // Destroyed with the panel.
    PhiPanel::OnNcDestroy();
  }

  RAW_PTR_EXCLUSION PhiUrlRulesWindow* const window_;
  std::vector<std::pair<int, HWND>> fields_;  // Row id -> its EDIT.
  HBRUSH bg_brush_ = nullptr;
  HBRUSH field_brush_ = nullptr;
  int scroll_ = 0;
  int content_height_ = 0;
  // The reorder drag.
  Capture capture_;
  int press_row_ = -1;
  POINT press_pt_ = {};
  bool dragging_ = false;
};

// ---------------------------------------------------------------------------
// The window.

// static
void PhiUrlRulesWindow::Open(PhiSettingsHost* host, HWND anchor) {
  if (!g_window || !g_window->alive()) {
    auto* window = new PhiUrlRulesWindow(host);
    if (!window->OpenWindow(anchor)) {
      delete window;
      return;
    }
    // Self-owned from here: the object goes with its HWND (OnNcDestroy
    // clears g_window first).
    window->DeleteWhenDestroyed();
    g_window = window;
  }
  // A minimized window comes back; SW_SHOW alone leaves it on the taskbar.
  ::ShowWindow(g_window->hwnd(),
               ::IsIconic(g_window->hwnd()) ? SW_RESTORE : SW_SHOW);
  ::SetForegroundWindow(g_window->hwnd());
}

// static
HWND PhiUrlRulesWindow::Handle() {
  return g_window && g_window->alive() ? g_window->hwnd() : nullptr;
}

// static
void PhiUrlRulesWindow::CloseForSignOut() {
  if (const HWND hwnd = Handle()) {
    ::PostMessageW(hwnd, WM_CLOSE, kCloseWithoutPrompt, 0);
  }
}

PhiUrlRulesWindow::PhiUrlRulesWindow(PhiSettingsHost* host) : host_(host) {}

PhiUrlRulesWindow::~PhiUrlRulesWindow() {
  list_.reset();
  for (HFONT font : {font_, headline_font_, small_font_}) {
    if (font) {
      ::DeleteObject(font);
    }
  }
  for (HBRUSH brush : {bg_brush_, divider_brush_}) {
    if (brush) {
      ::DeleteObject(brush);
    }
  }
}

bool PhiUrlRulesWindow::OpenWindow(HWND anchor) {
  ClassSpec spec;
  spec.name = kClassName;
  Options options;
  options.caption_buttons = kPhiCaptionClose | kPhiCaptionMinimize;
  // Tab walks the fields and buttons from the MAIN loop; Escape and Return
  // mean something specific here and stay in ControlProc.
  options.dialog_keys = kPhiDialogTab;
  const RECT bounds = {CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT + 100,
                       CW_USEDEFAULT + 100};
  if (!CreateSecondary(spec, 0,
                       WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                           WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                       bounds, nullptr, PhiStr("app.urlRulesEditor.windowTitle"),
                       options)) {
    LOG(ERROR) << "[UrlRules] CreateWindowEx failed err=" << ::GetLastError();
    return false;
  }
  OnColorsChanged();
  const SIZE outer = chrome().OuterSize(kWidth, kHeight);
  ::SetWindowPos(hwnd(), nullptr, 0, 0, outer.cx, outer.cy,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  CenterOn(anchor);
  Build();
  Load();
  return true;
}

HWND PhiUrlRulesWindow::Child(const wchar_t* cls,
                              const wchar_t* text,
                              DWORD style,
                              UINT_PTR id) {
  HWND control = ::CreateWindowExW(
      0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd(),
      reinterpret_cast<HMENU>(id), ::GetModuleHandleW(nullptr), nullptr);
  if (control) {
    ::SetWindowSubclass(control, &PhiUrlRulesWindow::ControlProc, id,
                        reinterpret_cast<DWORD_PTR>(this));
  }
  return control;
}

void PhiUrlRulesWindow::EnsureFonts() {
  for (HFONT* slot : {&font_, &headline_font_, &small_font_}) {
    if (*slot) {
      ::DeleteObject(*slot);
      *slot = nullptr;
    }
  }
  // The Mac's ladder: .headline for the title, .callout (12pt) for the
  // subtitle, 13pt controls.
  font_ = PhiPopupFont(dpi(), 13);
  headline_font_ = PhiPopupFont(dpi(), 13, FW_SEMIBOLD);
  small_font_ = PhiPopupFont(dpi(), 12);
}

void PhiUrlRulesWindow::Build() {
  EnsureFonts();
  add_ = Child(L"BUTTON", PhiStr("sidebar.urlRulesEditor.addRuleButton"),
               WS_TABSTOP | BS_PUSHBUTTON, kIdAdd);
  cancel_ = Child(L"BUTTON", PhiStr("sidebar.urlRulesEditor.cancelButton"),
                  WS_TABSTOP | BS_PUSHBUTTON, kIdCancel);
  save_ = Child(L"BUTTON", PhiStr("sidebar.urlRulesEditor.saveButton"),
                WS_TABSTOP | BS_DEFPUSHBUTTON, kIdSave);
  for (HWND button : {add_, cancel_, save_}) {
    ::SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  }
  PhiPopupStyleButton(add_, PhiPopupButtonRole::kSecondary);
  PhiPopupStyleButton(cancel_, PhiPopupButtonRole::kSecondary);
  PhiPopupStyleButton(save_, PhiPopupButtonRole::kPrimary);
  list_ = std::make_unique<RuleList>(this);
  Layout();
  if (!list_->Mount(list_bounds_)) {
    LOG(ERROR) << "[UrlRules] the rule list could not be created";
    list_.reset();
  }
}

void PhiUrlRulesWindow::Layout() {
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  const int band = CaptionHeight();
  // The header: the title line and the wrapped subtitle.
  int subtitle_h = Scaled(kTitleHeight);
  {
    HDC dc = ::GetDC(hwnd());
    HGDIOBJ previous = small_font_ ? ::SelectObject(dc, small_font_) : nullptr;
    RECT measure = {0, 0, client.right - 2 * Scaled(kHeaderPadX), 0};
    ::DrawTextW(dc, PhiStr("sidebar.urlRulesEditor.subtitle"), -1, &measure,
                DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    subtitle_h = std::max(subtitle_h, static_cast<int>(measure.bottom));
    if (previous) {
      ::SelectObject(dc, previous);
    }
    ::ReleaseDC(hwnd(), dc);
  }
  header_ = {client.left, band, client.right,
             band + Scaled(kHeaderPadY) + Scaled(kTitleHeight) +
                 Scaled(kTitleGap) + subtitle_h + Scaled(kHeaderPadY)};
  footer_ = {client.left, client.bottom - Scaled(kFooterHeight), client.right,
             client.bottom};
  list_bounds_ = {client.left, header_.bottom + 1, client.right,
                  footer_.top - 1};
  if (list_ && list_->alive()) {
    list_->SetPanelBounds(list_bounds_);
  }
  // The footer's buttons.
  const int button_y = footer_.top + (Scaled(kFooterHeight) -
                                      Scaled(kButtonHeight)) / 2;
  const auto place = [&](HWND control, int x, int w) {
    if (control) {
      ::SetWindowPos(control, nullptr, x, button_y, w, Scaled(kButtonHeight),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
  };
  place(add_, Scaled(kFooterPadX), Scaled(kAddWidth));
  place(save_, client.right - Scaled(kFooterPadX) - Scaled(kActionWidth),
        Scaled(kActionWidth));
  place(cancel_,
        client.right - Scaled(kFooterPadX) - 2 * Scaled(kActionWidth) -
            Scaled(kButtonGap),
        Scaled(kActionWidth));
  Invalidate();
}

void PhiUrlRulesWindow::Paint(HDC dc, const RECT&) {
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  ::FillRect(dc, &client, bg_brush_);
  PaintBand(dc);
  ::SetBkMode(dc, TRANSPARENT);
  HGDIOBJ previous =
      headline_font_ ? ::SelectObject(dc, headline_font_) : nullptr;
  ::SetTextColor(dc, colors().label_strong);
  RECT title = {header_.left + Scaled(kHeaderPadX),
                header_.top + Scaled(kHeaderPadY),
                header_.right - Scaled(kHeaderPadX),
                header_.top + Scaled(kHeaderPadY) + Scaled(kTitleHeight)};
  ::DrawTextW(dc, PhiStr("sidebar.urlRulesEditor.title"), -1, &title,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  if (small_font_) {
    ::SelectObject(dc, small_font_);
  }
  ::SetTextColor(dc, colors().secondary);
  RECT subtitle = {title.left, title.bottom + Scaled(kTitleGap), title.right,
                   header_.bottom - Scaled(kHeaderPadY)};
  ::DrawTextW(dc, PhiStr("sidebar.urlRulesEditor.subtitle"), -1, &subtitle,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
  if (previous) {
    ::SelectObject(dc, previous);
  }
  // The two dividers the Mac draws around the list.
  RECT top = {client.left, header_.bottom, client.right, header_.bottom + 1};
  RECT bottom = {client.left, footer_.top - 1, client.right, footer_.top};
  ::FillRect(dc, &top, divider_brush_);
  ::FillRect(dc, &bottom, divider_brush_);
  // The note a refused Save leaves, between Add Rule and Cancel.
  if (HasInvalidRows()) {
    if (small_font_) {
      previous = ::SelectObject(dc, small_font_);
    }
    ::SetTextColor(dc, colors().destructive);
    RECT note = {Scaled(kFooterPadX) + Scaled(kAddWidth) + Scaled(kButtonGap),
                 footer_.top,
                 client.right - Scaled(kFooterPadX) - 2 * Scaled(kActionWidth) -
                     2 * Scaled(kButtonGap),
                 footer_.bottom};
    const wchar_t* text = PhiStr("sidebar.urlRulesEditor.invalidRows");
    // Wrapped, then centred on the footer by hand (DT_VCENTER is
    // single-line only).
    RECT measure = note;
    ::DrawTextW(dc, text, -1, &measure,
                DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    const int height = std::min(static_cast<int>(measure.bottom - measure.top),
                                static_cast<int>(note.bottom - note.top));
    note.top = (footer_.top + footer_.bottom - height) / 2;
    note.bottom = note.top + height;
    ::DrawTextW(dc, text, -1, &note,
                DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS |
                    DT_EDITCONTROL);
    if (small_font_) {
      ::SelectObject(dc, previous);
    }
  }
}

void PhiUrlRulesWindow::Load() {
  rows_.clear();
  for (const model::UrlRule& rule : host_->UrlRules().rules()) {
    Row row;
    row.id = next_row_id_++;
    row.type = UrlRuleMatchTypeOf(rule.host, rule.path_prefix);
    row.value = UrlRuleDisplayValue(rule.host, rule.path_prefix);
    row.target_id = rule.space_id;
    row.ask = rule.ask;
    rows_.push_back(std::move(row));
  }
  initial_fingerprint_ = Fingerprint();
  if (list_) {
    list_->Relayout();
  }
}

std::vector<PhiUrlRulesWindow::Destination> PhiUrlRulesWindow::Destinations()
    const {
  std::vector<Destination> out;
  for (const PhiSpaceSummary& space : host_->SpaceList().Summaries()) {
    // "Space name — Profile" so each entry shows which profile it routes
    // into (the Mac's spaceMenuTitle).
    std::wstring label = Widen(space.name);
    const std::string profile =
        host_->ProfileList().ProfileDisplayName(space.profile_id);
    if (!profile.empty()) {
      label += L" \u2014 " + Widen(profile);
    }
    out.push_back({space.id, std::move(label)});
  }
  out.push_back({kPhiIncognitoRuleTargetId,
                 PhiStr("spaces.builtIn.incognito.routingTargetName")});
  out.push_back({kPhiKioskRuleTargetId,
                 PhiStr("sidebar.urlRulesEditor.target.kioskOption")});
  return out;
}

std::wstring PhiUrlRulesWindow::TargetLabel(const Row& row) const {
  if (row.ask) {
    return PhiStr("sidebar.urlRulesEditor.target.askEveryTimeOption");
  }
  for (const Destination& destination : Destinations()) {
    if (destination.id == row.target_id) {
      return destination.label;
    }
  }
  // An auto-route rule whose Space was deleted: say so (never a false "Ask
  // every time") so the user re-targets it; Save refuses it until then.
  return PhiStr("sidebar.urlRulesEditor.target.unavailablePlaceholder");
}

const wchar_t* PhiUrlRulesWindow::MatchTypeLabel(UrlRuleMatchType type) const {
  switch (type) {
    case UrlRuleMatchType::kDomainSuffix:
      return PhiStr("sidebar.urlRulesEditor.matchType.domainSuffix");
    case UrlRuleMatchType::kDomain:
      return PhiStr("sidebar.urlRulesEditor.matchType.domain");
    case UrlRuleMatchType::kDomainContains:
      return PhiStr("sidebar.urlRulesEditor.matchType.domainContains");
    case UrlRuleMatchType::kUrl:
      return PhiStr("sidebar.urlRulesEditor.matchType.url");
    case UrlRuleMatchType::kSourceApp:
      return PhiStr("sidebar.urlRulesEditor.matchType.app");
  }
  return L"";
}

const wchar_t* PhiUrlRulesWindow::MatchValuePlaceholder(
    UrlRuleMatchType type) const {
  switch (type) {
    case UrlRuleMatchType::kDomainSuffix:
      return PhiStr("sidebar.urlRulesEditor.matchValue.domainSuffixPlaceholder");
    case UrlRuleMatchType::kDomain:
      return PhiStr("sidebar.urlRulesEditor.matchValue.domainPlaceholder");
    case UrlRuleMatchType::kDomainContains:
      return PhiStr(
          "sidebar.urlRulesEditor.matchValue.domainContainsPlaceholder");
    case UrlRuleMatchType::kUrl:
      return PhiStr("sidebar.urlRulesEditor.matchValue.urlPlaceholder");
    case UrlRuleMatchType::kSourceApp:
      return PhiStr("sidebar.urlRulesEditor.matchValue.appPlaceholder");
  }
  return L"";
}

void PhiUrlRulesWindow::AddBlankRow() {
  // The Mac's addBlankRow: a fresh row targeting the first destination,
  // scrolled into view with the cursor in its field.
  const std::vector<Destination> destinations = Destinations();
  if (destinations.empty()) {
    return;
  }
  Row row;
  row.id = next_row_id_++;
  row.target_id = destinations.front().id;
  rows_.push_back(std::move(row));
  const int id = rows_.back().id;
  if (list_) {
    list_->Relayout();
    list_->ScrollToBottom();
    list_->FocusField(id);
  }
}

void PhiUrlRulesWindow::RemoveRow(int row_index) {
  if (row_index < 0 || row_index >= static_cast<int>(rows_.size())) {
    return;
  }
  rows_.erase(rows_.begin() + row_index);
  if (list_) {
    list_->Relayout();
  }
  Invalidate();  // The footer note may go with the row.
}

void PhiUrlRulesWindow::SetRowValue(int row_id, std::string value) {
  for (Row& row : rows_) {
    if (row.id == row_id) {
      if (row.value != value) {
        ClearInvalid(&row);
      }
      row.value = std::move(value);
      return;
    }
  }
}

bool PhiUrlRulesWindow::HasInvalidRows() const {
  return std::any_of(rows_.begin(), rows_.end(),
                     [](const Row& row) { return row.invalid; });
}

void PhiUrlRulesWindow::ClearInvalid(Row* row) {
  if (!row->invalid) {
    return;
  }
  row->invalid = false;
  if (list_ && list_->alive()) {
    list_->Invalidate();
  }
  Invalidate();  // The footer note may go.
}

void PhiUrlRulesWindow::PickMatchType(int row_index, const RECT& anchor) {
  if (row_index < 0 || row_index >= static_cast<int>(rows_.size()) || !list_) {
    return;
  }
  const UrlRuleMatchType current = rows_[static_cast<size_t>(row_index)].type;
  HMENU menu = ::CreatePopupMenu();
  if (!menu) {
    return;
  }
  static constexpr std::array<UrlRuleMatchType, 4> kTypes = {
      UrlRuleMatchType::kDomainSuffix, UrlRuleMatchType::kDomain,
      UrlRuleMatchType::kDomainContains, UrlRuleMatchType::kUrl};
  for (size_t i = 0; i < kTypes.size(); ++i) {
    ::AppendMenuW(menu, MF_STRING | (kTypes[i] == current ? MF_CHECKED : 0),
                  static_cast<UINT>(i) + 1, MatchTypeLabel(kTypes[i]));
  }
  POINT at = {anchor.left, anchor.bottom};
  ::ClientToScreen(list_->hwnd(), &at);
  const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, at.x, at.y, 0,
      hwnd(), nullptr));
  ::DestroyMenu(menu);
  if (chosen < 1 || chosen > kTypes.size() ||
      row_index >= static_cast<int>(rows_.size())) {
    return;
  }
  // The type is the source of truth; the typed value stays as it is and
  // encodes under the new type at Save (the Mac's matchTypeChanged).
  rows_[static_cast<size_t>(row_index)].type = kTypes[chosen - 1];
  ClearInvalid(&rows_[static_cast<size_t>(row_index)]);
  list_->Relayout();
}

void PhiUrlRulesWindow::PickTarget(int row_index, const RECT& anchor) {
  if (row_index < 0 || row_index >= static_cast<int>(rows_.size()) || !list_) {
    return;
  }
  const Row snapshot = rows_[static_cast<size_t>(row_index)];
  const std::vector<Destination> destinations = Destinations();
  HMENU menu = ::CreatePopupMenu();
  if (!menu) {
    return;
  }
  bool known = snapshot.ask;
  for (const Destination& destination : destinations) {
    known = known || destination.id == snapshot.target_id;
  }
  constexpr UINT kUnavailable = 9000;
  constexpr UINT kAsk = 9001;
  if (!known) {
    ::AppendMenuW(menu, MF_STRING | MF_GRAYED | MF_CHECKED, kUnavailable,
                  PhiStr("sidebar.urlRulesEditor.target.unavailablePlaceholder"));
  }
  for (size_t i = 0; i < destinations.size(); ++i) {
    if (UrlRuleTargetIsKiosk(destinations[i].id)) {
      ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);  // Kiosk in its own section.
    }
    const bool checked = !snapshot.ask && destinations[i].id == snapshot.target_id;
    // Space and profile names are the user's: an '&' is a character.
    ::AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0),
                  static_cast<UINT>(i) + 1,
                  PhiEscapeMenuText(destinations[i].label).c_str());
  }
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING | (snapshot.ask ? MF_CHECKED : 0), kAsk,
                PhiStr("sidebar.urlRulesEditor.target.askEveryTimeOption"));
  POINT at = {anchor.left, anchor.bottom};
  ::ClientToScreen(list_->hwnd(), &at);
  const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, at.x, at.y, 0,
      hwnd(), nullptr));
  ::DestroyMenu(menu);
  if (row_index >= static_cast<int>(rows_.size())) {
    return;
  }
  Row& row = rows_[static_cast<size_t>(row_index)];
  if (chosen >= 1 && chosen <= destinations.size()) {
    row.target_id = destinations[chosen - 1].id;
    row.ask = false;
  } else if (chosen == kAsk) {
    // "Ask every time" rides the row's target as the prompt's suggested
    // default (the Mac's askSpaceTag).
    row.ask = true;
  } else {
    return;
  }
  ClearInvalid(&row);
  list_->Relayout();
}

std::string PhiUrlRulesWindow::Fingerprint() const {
  std::string out;
  for (const Row& row : rows_) {
    out += std::to_string(row.id) + "|" + (row.ask ? "1" : "0") + "|" +
           row.target_id + "|" + std::to_string(static_cast<int>(row.type)) +
           "|" + row.value + "\n";
  }
  return out;
}

void PhiUrlRulesWindow::Save() {
  if (list_) {
    list_->CommitFields();
  }
  if (Fingerprint() != initial_fingerprint_) {
    // The Mac's save(): resolve each row's destination, then encode. An
    // auto-route rule must target a live destination; an "ask every time"
    // rule only uses its target as the prompt's default, so a deleted
    // Space falls back to the first promptable destination (never Kiosk,
    // which the chooser lists as a section of its own) rather than dropping
    // the rule. A row that has no usable pattern or no live destination is
    // NOT dropped: it is outlined and the window stays open so the user
    // fixes or removes it.
    std::set<std::string> valid;
    std::vector<std::string> promptable;
    for (const Destination& destination : Destinations()) {
      valid.insert(destination.id);
      if (!UrlRuleTargetIsKiosk(destination.id)) {
        promptable.push_back(destination.id);
      }
    }
    std::vector<model::UrlRule> rules;
    int first_invalid = -1;
    for (size_t i = 0; i < rows_.size(); ++i) {
      Row& row = rows_[i];
      row.invalid = false;
      std::string target;
      if (row.ask && valid.count(row.target_id) &&
          !UrlRuleTargetIsKiosk(row.target_id)) {
        target = row.target_id;
      } else if (row.ask && !promptable.empty()) {
        target = promptable.front();
      } else if (!row.ask && valid.count(row.target_id)) {
        target = row.target_id;
      }
      model::UrlRule rule;
      const std::string value = NormalizeUrlRuleHost(row.value);
      if (!value.empty()) {
        UrlRuleEncode(row.type, value, &rule.host, &rule.path_prefix);
      }
      if (rule.host.empty() || target.empty()) {
        row.invalid = true;
        if (first_invalid < 0) {
          first_invalid = static_cast<int>(i);
        }
        continue;
      }
      rule.space_id = target;
      rule.ask = row.ask;
      rules.push_back(std::move(rule));
    }
    if (first_invalid >= 0) {
      if (list_) {
        list_->ScrollToRow(first_invalid);
        list_->FocusField(rows_[static_cast<size_t>(first_invalid)].id);
      }
      Invalidate();  // The footer note.
      return;
    }
    host_->UrlRules().ReplaceRules(rules);
  }
  Destroy();
}

bool PhiUrlRulesWindow::ConfirmDiscard() {
  if (list_) {
    list_->CommitFields();
  }
  if (Fingerprint() == initial_fingerprint_) {
    return true;
  }
  // Discard first (the affirmative of "Discard changes?"), Keep Editing
  // last; Escape and the alert's close button keep editing. The alert runs
  // a modal loop: should this window be torn down meanwhile (the object is
  // self-owned), nothing of it is touched afterwards.
  const HWND self = hwnd();
  const int choice = PhiAlert(
      hwnd(), PhiStr("sidebar.urlRulesEditor.discardAlert.title"),
      PhiStr("sidebar.urlRulesEditor.discardAlert.message"),
      {{PhiStr("sidebar.urlRulesEditor.discardAlert.discardButton"),
        PhiAlertRole::kDestructive},
       {PhiStr("sidebar.urlRulesEditor.discardAlert.keepEditingButton"),
        PhiAlertRole::kSecondary}},
      /*default_index=*/1, /*cancel_index=*/1);
  if (!::IsWindow(self) || g_window != this) {
    return false;
  }
  return choice == 0;
}

// static
LRESULT CALLBACK PhiUrlRulesWindow::ControlProc(HWND hwnd,
                                                UINT msg,
                                                WPARAM wparam,
                                                LPARAM lparam,
                                                UINT_PTR id,
                                                DWORD_PTR data) {
  auto* self = reinterpret_cast<PhiUrlRulesWindow*>(data);
  if (msg == WM_NCDESTROY) {
    ::RemoveWindowSubclass(hwnd, &PhiUrlRulesWindow::ControlProc, id);
    return ::DefSubclassProc(hwnd, msg, wparam, lparam);
  }
  if (!self || !self->alive()) {
    return ::DefSubclassProc(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_KEYDOWN) {
    if (wparam == VK_ESCAPE) {
      // Cancel: the Mac's Escape on the sheet.
      ::PostMessageW(self->hwnd(), WM_CLOSE, 0, 0);
      return 0;
    }
    if (wparam == VK_RETURN) {
      wchar_t cls[16] = {};
      ::GetClassNameW(hwnd, cls, 16);
      if (::_wcsicmp(cls, L"Button") == 0) {
        ::SendMessageW(hwnd, BM_CLICK, 0, 0);
      }
      return 0;  // A field's Return does nothing (and does not beep).
    }
  }
  if (msg == WM_CHAR && (wparam == VK_ESCAPE || wparam == VK_RETURN)) {
    return 0;
  }
  return ::DefSubclassProc(hwnd, msg, wparam, lparam);
}

LRESULT PhiUrlRulesWindow::OnWindowMessage(UINT msg,
                                           WPARAM wparam,
                                           LPARAM lparam,
                                           bool* handled) {
  switch (msg) {
    case WM_DRAWITEM:
      if (PhiPopupDrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam),
                             colors(), font_)) {
        *handled = true;
        return TRUE;
      }
      break;
    case WM_COMMAND: {
      const UINT_PTR id = LOWORD(wparam);
      if (HIWORD(wparam) != BN_CLICKED) {
        break;
      }
      switch (id) {
        case kIdAdd:
          AddBlankRow();
          *handled = true;
          return 0;
        case kIdCancel:
          if (ConfirmDiscard()) {
            Destroy();
          }
          *handled = true;
          return 0;
        case kIdSave:
          Save();
          *handled = true;
          return 0;
        default:
          break;
      }
      break;
    }
    case WM_SIZE:
      if (alive() && list_) {
        Layout();
      }
      break;
    case WM_CLOSE:
      if (wparam == kCloseWithoutPrompt || ConfirmDiscard()) {
        Destroy();
      }
      *handled = true;
      return 0;
    default:
      break;
  }
  return PhiSecondaryWindow::OnWindowMessage(msg, wparam, lparam, handled);
}

void PhiUrlRulesWindow::OnEscape() {
  if (ConfirmDiscard()) {
    Destroy();
  }
}

void PhiUrlRulesWindow::OnColorsChanged() {
  for (HBRUSH* slot : {&bg_brush_, &divider_brush_}) {
    if (*slot) {
      ::DeleteObject(*slot);
      *slot = nullptr;
    }
  }
  bg_brush_ = ::CreateSolidBrush(colors().bg);
  divider_brush_ = ::CreateSolidBrush(colors().separator);
  if (list_ && list_->alive()) {
    list_->RebuildBrushes();
  }
}

void PhiUrlRulesWindow::OnDpiChanged(UINT old_dpi) {
  PhiSecondaryWindow::OnDpiChanged(old_dpi);
  EnsureFonts();
  for (HWND button : {add_, cancel_, save_}) {
    if (button) {
      ::SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font_),
                     TRUE);
    }
  }
  Layout();
  if (list_ && list_->alive()) {
    list_->SyncDpiFromParent();
    list_->ApplyFonts();
    list_->Relayout();
  }
}

void PhiUrlRulesWindow::OnNcDestroy() {
  if (g_window == this) {
    g_window = nullptr;
  }
  PhiSecondaryWindow::OnNcDestroy();
}

}  // namespace phi_client

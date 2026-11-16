// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_content_blocking_window.h"

#include <commctrl.h>  // EM_SETCUEBANNER
#include <shlwapi.h>   // StrFormatByteSizeEx
#include <windowsx.h>

#include <algorithm>
#include <cwctype>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "client/phi_dwrite.h"
#include "client/phi_i18n.h"
#include "client/phi_panel.h"
#include "client/phi_popup_chrome.h"
#include "client/phi_prompt.h"
#include "client/phi_settings_window.h"

namespace phi_client {

namespace {

using Category = PhiContentBlockingCategory;

constexpr wchar_t kClassName[] = L"PhiContentBlockingWindow";
constexpr wchar_t kListClassName[] = L"PhiContentBlockingList";

// Design units (dip). The Mac sheet is 480 x 540; a Windows list chooser
// dialog may run to 548 wide (docs/windows-native-ui.md §6).
constexpr int kWidth = 520;
constexpr int kHeight = 580;
constexpr int kPad = 24;
constexpr int kTitleHeight = 28;  // 20 / 600.
constexpr int kTitleGap = 8;
constexpr int kFooterHeight = 72;  // A 32 button in 20 of padding.
constexpr int kButtonHeight = 32;
constexpr int kButtonGap = 8;
constexpr int kDoneWidth = 120;
constexpr int kUpdateWidth = 168;
constexpr int kSegmentWidth = 96;
constexpr int kLabelHeight = 20;
constexpr int kFieldHeight = 32;
constexpr int kRulesHeight = 170;
constexpr int kFieldGap = 16;
// The list.
constexpr int kRowHeight = 56;
constexpr int kAddRowHeight = 52;
constexpr int kHeadingHeight = 22;
constexpr int kSectionGap = 16;
constexpr int kCardRadius = 6;
constexpr int kCheckSize = 18;
constexpr int kGlyphButton = 32;
constexpr int kProgressWidth = 72;
constexpr int kScrollbarWidth = 4;
constexpr int kWheelStep = 48;

enum : UINT_PTR {
  kIdUpdate = 101,
  kIdDone,
  kIdKindUrl,
  kIdKindRules,
  kIdName,
  kIdUrl,
  kIdRules,
  kIdAdd,
  kIdCancel,
};

PhiContentBlockingWindow* g_window = nullptr;
// Bumped per window, so an add's late answer knows its window went.
int g_window_serial = 0;

std::wstring Widen(const std::string& utf8) {
  return base::UTF8ToWide(utf8);
}

std::wstring WindowText(HWND hwnd) {
  const int length = ::GetWindowTextLengthW(hwnd);
  std::wstring text(static_cast<size_t>(length) + 1, L'\0');
  const int copied = ::GetWindowTextW(hwnd, text.data(), length + 1);
  text.resize(static_cast<size_t>(std::max(copied, 0)));
  return text;
}

std::wstring Trimmed(const std::wstring& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::iswspace(text[begin])) {
    ++begin;
  }
  while (end > begin && std::iswspace(text[end - 1])) {
    --end;
  }
  return text.substr(begin, end - begin);
}

// The Mac's ContentBlockingListStrings: catalog titles and descriptions by
// list id; an unknown id (a newer catalog) reads as itself.
struct ListStrings {
  const char* id;
  const char* title;
  const char* desc;
};
constexpr ListStrings kListStrings[] = {
    {"easylist", "settings.privacy.contentBlocking.list.easylist.title",
     "settings.privacy.contentBlocking.list.easylist.desc"},
    {"easyprivacy", "settings.privacy.contentBlocking.list.easyprivacy.title",
     "settings.privacy.contentBlocking.list.easyprivacy.desc"},
    {"easylist-cookie",
     "settings.privacy.contentBlocking.list.easylistCookie.title",
     "settings.privacy.contentBlocking.list.easylistCookie.desc"},
    {"adguard-german",
     "settings.privacy.contentBlocking.list.adguardGerman.title",
     "settings.privacy.contentBlocking.list.adguardGerman.desc"},
    {"adguard-french",
     "settings.privacy.contentBlocking.list.adguardFrench.title",
     "settings.privacy.contentBlocking.list.adguardFrench.desc"},
    {"adguard-dutch", "settings.privacy.contentBlocking.list.adguardDutch.title",
     "settings.privacy.contentBlocking.list.adguardDutch.desc"},
    {"adguard-spanish",
     "settings.privacy.contentBlocking.list.adguardSpanish.title",
     "settings.privacy.contentBlocking.list.adguardSpanish.desc"},
    {"adguard-chinese",
     "settings.privacy.contentBlocking.list.adguardChinese.title",
     "settings.privacy.contentBlocking.list.adguardChinese.desc"},
    {"adguard-japanese",
     "settings.privacy.contentBlocking.list.adguardJapanese.title",
     "settings.privacy.contentBlocking.list.adguardJapanese.desc"},
    {"phi-specific", "settings.privacy.contentBlocking.list.phiSpecific.title",
     "settings.privacy.contentBlocking.list.phiSpecific.desc"},
};

std::wstring Bytes(int64_t bytes) {
  wchar_t text[64] = {};
  if (FAILED(::StrFormatByteSizeEx(static_cast<ULONGLONG>(std::max<int64_t>(
                                       bytes, 0)),
                                   SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT,
                                   text, 64))) {
    return std::to_wstring(bytes);
  }
  return text;
}

}  // namespace

std::wstring PhiContentBlockingListTitle(const PhiContentBlockingList& list) {
  if (list.custom) {
    return Widen(list.name);
  }
  for (const ListStrings& strings : kListStrings) {
    if (list.id == strings.id) {
      return PhiStr(strings.title);
    }
  }
  return Widen(list.id);
}

std::wstring PhiContentBlockingListDescription(
    const PhiContentBlockingList& list) {
  if (list.custom) {
    return std::wstring();
  }
  for (const ListStrings& strings : kListStrings) {
    if (list.id == strings.id) {
      return PhiStr(strings.desc);
    }
  }
  return std::wstring();
}

std::wstring PhiContentBlockingSummary(Category category,
                                       const PhiContentBlockingState& state) {
  // The Mac's ContentBlockingRuleSets.summary: one list reads as its name,
  // more become a count; custom lists apply to every switch.
  std::vector<const PhiContentBlockingList*> checked;
  for (const PhiContentBlockingList* list :
       PhiContentBlockingListsFor(category, state)) {
    if (list->checked) {
      checked.push_back(list);
    }
  }
  std::vector<const PhiContentBlockingList*> custom;
  for (const PhiContentBlockingList* list :
       PhiContentBlockingCustomLists(state)) {
    if (list->checked) {
      custom.push_back(list);
    }
  }
  if (checked.empty() && custom.empty()) {
    return PhiStr("settings.privacy.contentBlocking.summary.noneSelected");
  }
  std::wstring names;
  if (checked.size() == 1) {
    names = PhiContentBlockingListTitle(*checked.front());
  } else if (checked.size() > 1) {
    names = PhiStrF("settings.privacy.contentBlocking.summary.count",
                    {std::to_wstring(checked.size())});
  }
  if (!custom.empty()) {
    const std::wstring part =
        custom.size() == 1
            ? std::wstring(
                  PhiStr("settings.privacy.contentBlocking.summary.customOne"))
            : PhiStrF("settings.privacy.contentBlocking.summary.custom",
                      {std::to_wstring(custom.size())});
    names = names.empty() ? part : names + L" + " + part;
  }
  std::vector<const PhiContentBlockingList*> all = checked;
  all.insert(all.end(), custom.begin(), custom.end());
  if (std::any_of(all.begin(), all.end(),
                  [](const PhiContentBlockingList* list) {
                    return list->downloading;
                  })) {
    return names + L" · " +
           PhiStr("settings.privacy.contentBlocking.list.downloading");
  }
  const size_t missing = static_cast<size_t>(std::count_if(
      all.begin(), all.end(),
      [](const PhiContentBlockingList* list) { return !list->available; }));
  if (missing > 0) {
    return names + L" · " +
           PhiStrF("settings.privacy.contentBlocking.summary.notDownloaded",
                   {std::to_wstring(missing)});
  }
  return names;
}

namespace {

// "Downloading… 1.2 MB of 3.4 MB", with what the server announced.
std::wstring DownloadingLine(const PhiContentBlockingList& list) {
  if (list.total_bytes > 0) {
    return PhiStrF("settings.privacy.contentBlocking.list.downloadingOf",
                   {Bytes(list.downloaded_bytes), Bytes(list.total_bytes)});
  }
  if (list.downloaded_bytes > 0) {
    return PhiStrF("settings.privacy.contentBlocking.list.downloadingBytes",
                   {Bytes(list.downloaded_bytes)});
  }
  return PhiStr("settings.privacy.contentBlocking.list.downloading");
}

// The line under a list that is downloading or not on disk; empty once it
// is (the Mac's availabilityLine).
std::wstring AvailabilityLine(const PhiContentBlockingList& list) {
  if (list.downloading) {
    return DownloadingLine(list);
  }
  if (list.available) {
    return std::wstring();
  }
  if (list.last_error.empty()) {
    return PhiStr("settings.privacy.contentBlocking.listInfo.notDownloaded");
  }
  return PhiStrF("settings.privacy.contentBlocking.list.downloadFailed",
                 {Widen(list.last_error)});
}

// The details' state line (the Mac's ContentBlockingListInfoPopover).
std::wstring UpdateLine(const PhiContentBlockingList& list) {
  if (list.downloading) {
    return PhiStr("settings.privacy.contentBlocking.list.downloading");
  }
  if (!list.available) {
    return AvailabilityLine(list);
  }
  if (list.fetched_at_ms <= 0) {
    return PhiStr("settings.privacy.contentBlocking.listInfo.builtIn");
  }
  const int64_t ms = static_cast<int64_t>(list.fetched_at_ms);
  return PhiStrF("settings.privacy.contentBlocking.listInfo.updated",
                 {PhiFormatDate(ms, PhiDateStyle::kShortDate) + L" " +
                  PhiFormatDate(ms, PhiDateStyle::kShortTime)});
}

}  // namespace

// ---------------------------------------------------------------------------
// The list: one painted panel for both cards, the custom-list heading and
// "Add Filter…". Every control is a hit (so UI Automation and the mouse see
// the same thing) and a keyboard stop: the panel is ONE Tab stop, the arrows
// walk its controls, Space or Return activates the focused one.

class PhiContentBlockingWindow::ListPanel : public PhiPanel {
 public:
  explicit ListPanel(PhiContentBlockingWindow* window) : window_(window) {}
  ~ListPanel() override = default;

  bool Mount(const RECT& bounds) {
    ClassSpec spec;
    spec.name = kListClassName;
    Options options;
    options.style = WS_TABSTOP;
    options.in_card_stack = false;
    if (!CreatePanel(spec, window_->hwnd(), bounds, options)) {
      return false;
    }
    EnableTooltips(true);
    ShowPanel();
    return true;
  }

  // The state moved: lay the rows out again and rebuild the hits.
  void Relayout() {
    if (!alive()) {
      return;
    }
    const PartKey focused = FocusedKey();
    parts_.clear();
    rows_.clear();
    cards_.clear();
    heading_ = {};
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    const int right = client.right - Scaled(kScrollbarWidth) - Scaled(4);
    int y = -scroll_;
    const PhiContentBlockingState* state = window_->state();
    if (state) {
      const auto add_card = [&](const std::vector<const PhiContentBlockingList*>&
                                    lists,
                                bool with_add) {
        RECT card = {0, y, right, y};
        for (size_t i = 0; i < lists.size(); ++i) {
          AddRow(*lists[i], y, right, i > 0);
          y += Scaled(kRowHeight);
        }
        if (with_add) {
          Part add;
          add.kind = PartKind::kAdd;
          add.name =
              PhiStr("settings.privacy.contentBlocking.advanced.addFilter");
          add.bounds = {right - Scaled(16) - Scaled(132),
                        y + (Scaled(kAddRowHeight) - Scaled(kButtonHeight)) / 2,
                        right - Scaled(16),
                        y + (Scaled(kAddRowHeight) + Scaled(kButtonHeight)) / 2};
          add.divider_above = !lists.empty();
          add.row_top = y;
          parts_.push_back(add);
          y += Scaled(kAddRowHeight);
        }
        card.bottom = y;
        cards_.push_back(card);
      };
      add_card(PhiContentBlockingListsFor(window_->category(), *state),
               /*with_add=*/false);
      y += Scaled(kSectionGap);
      heading_ = {Scaled(2), y, right, y + Scaled(kHeadingHeight)};
      y += Scaled(kHeadingHeight) + Scaled(6);
      add_card(PhiContentBlockingCustomLists(*state), /*with_add=*/true);
    }
    content_height_ = y + scroll_;
    const int viewport = static_cast<int>(client.bottom);
    const int limit = std::max(0, content_height_ - viewport);
    if (scroll_ > limit) {
      scroll_ = limit;
      Relayout();
      return;
    }
    // Keep the focus on the same control across the rebuild.
    focus_ = -1;
    for (size_t i = 0; i < parts_.size(); ++i) {
      if (parts_[i].kind == focused.kind && parts_[i].list_id == focused.id) {
        focus_ = static_cast<int>(i);
      }
    }
    if (focus_ < 0 && !parts_.empty() && focused.kind != PartKind::kNone) {
      focus_ = 0;
    }
    ClearHits();
    for (size_t i = 0; i < parts_.size(); ++i) {
      const Part& part = parts_[i];
      AddHit(part.bounds, static_cast<int>(i), part.name,
             part.kind == PartKind::kCheck ? PhiHitRole::kToggle
                                           : PhiHitRole::kButton,
             part.kind == PartKind::kCheck && part.on, part.enabled);
    }
    Invalidate();
  }

  void RebuildBrushes() { Invalidate(); }

 private:
  enum class PartKind { kNone, kCheck, kControl, kInfo, kAdd };
  enum class Control { kNone, kDownload, kDownloading, kRemove };
  struct Part {
    PartKind kind = PartKind::kNone;
    RECT bounds = {};
    std::string list_id;
    std::wstring name;
    bool on = false;
    bool enabled = true;
    Control control = Control::kNone;
    bool divider_above = false;
    int row_top = 0;
  };
  struct Row {
    RECT bounds = {};
    std::wstring title;
    std::wstring line;
    bool divider_above = false;
    bool downloading = false;
    int64_t received = 0;
    int64_t total = -1;
  };
  struct PartKey {
    PartKind kind = PartKind::kNone;
    std::string id;
  };

  PartKey FocusedKey() const {
    if (focus_ < 0 || focus_ >= static_cast<int>(parts_.size())) {
      return {};
    }
    return {parts_[static_cast<size_t>(focus_)].kind,
            parts_[static_cast<size_t>(focus_)].list_id};
  }

  void AddRow(const PhiContentBlockingList& list,
              int top,
              int right,
              bool divider_above) {
    Row row;
    row.bounds = {0, top, right, top + Scaled(kRowHeight)};
    row.title = PhiContentBlockingListTitle(list);
    row.line = AvailabilityLine(list);
    row.divider_above = divider_above;
    row.downloading = list.downloading;
    row.received = list.downloaded_bytes;
    row.total = list.total_bytes;
    rows_.push_back(row);
    const int mid = top + Scaled(kRowHeight) / 2;
    Part check;
    check.kind = PartKind::kCheck;
    check.list_id = list.id;
    check.on = list.checked;
    check.name = row.title;
    check.bounds = {Scaled(14), mid - Scaled(14), Scaled(14) + Scaled(28),
                    mid + Scaled(14)};
    parts_.push_back(check);
    // Info, trailing; the one state control before it.
    RECT info = {right - Scaled(8) - Scaled(kGlyphButton),
                 mid - Scaled(kGlyphButton) / 2, right - Scaled(8),
                 mid + Scaled(kGlyphButton) / 2};
    Part control;
    control.list_id = list.id;
    control.control = list.downloading ? Control::kDownloading
                      : list.custom    ? Control::kRemove
                      : list.available ? Control::kNone
                                       : Control::kDownload;
    if (control.control == Control::kDownloading) {
      // Not a control: the progress bar, painted with the row.
      control.control = Control::kNone;
    }
    if (control.control != Control::kNone) {
      control.kind = PartKind::kControl;
      control.bounds = {info.left - Scaled(4) - Scaled(kGlyphButton), info.top,
                        info.left - Scaled(4), info.bottom};
      control.name = PhiStrF(
          "settings.a11y.labelValue",
          {PhiStr(control.control == Control::kRemove
                      ? "settings.privacy.contentBlocking.list.remove"
                      : "settings.privacy.contentBlocking.list.download"),
           row.title});
      parts_.push_back(control);
    }
    Part about;
    about.kind = PartKind::kInfo;
    about.list_id = list.id;
    about.bounds = info;
    about.name = PhiStrF(
        "settings.a11y.labelValue",
        {PhiStr("settings.privacy.contentBlocking.list.info"), row.title});
    parts_.push_back(about);
  }

  void Activate(int index) {
    if (index < 0 || index >= static_cast<int>(parts_.size())) {
      return;
    }
    const Part part = parts_[static_cast<size_t>(index)];
    switch (part.kind) {
      case PartKind::kCheck:
        window_->ToggleList(part.list_id, !part.on);
        break;
      case PartKind::kControl:
        if (part.control == Control::kRemove) {
          window_->RemoveCustomList(part.list_id);
        } else if (part.control == Control::kDownload) {
          window_->DownloadList(part.list_id);
        }
        break;
      case PartKind::kInfo:
        window_->ShowListInfo(part.list_id);
        break;
      case PartKind::kAdd:
        window_->ShowAddFilter();
        break;
      case PartKind::kNone:
        break;
    }
  }

  void MoveFocus(int delta) {
    if (parts_.empty()) {
      return;
    }
    const int n = static_cast<int>(parts_.size());
    focus_ = focus_ < 0 ? (delta > 0 ? 0 : n - 1)
                        : std::clamp(focus_ + delta, 0, n - 1);
    ScrollIntoView(parts_[static_cast<size_t>(focus_)].bounds);
    Invalidate();
  }

  void ScrollIntoView(const RECT& bounds) {
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    const int pad = Scaled(8);
    int next = scroll_;
    if (bounds.top - pad < 0) {
      next = scroll_ + bounds.top - pad;
    } else if (bounds.bottom + pad > client.bottom) {
      next = scroll_ + bounds.bottom + pad - client.bottom;
    }
    next = std::max(0, next);
    if (next != scroll_) {
      scroll_ = next;
      Relayout();
    }
  }

  // PhiWindow.
  void Paint(HDC dc, const RECT&) override {
    const PhiPopupColors& c = window_->popup_colors();
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    HBRUSH ground = ::CreateSolidBrush(c.bg);
    ::FillRect(dc, &client, ground);
    ::DeleteObject(ground);
    ::SetBkMode(dc, TRANSPARENT);
    HFONT body = fonts().Get(14, FW_NORMAL);
    HFONT small_font = fonts().Get(12, FW_NORMAL);
    HFONT strong = fonts().Get(14, FW_SEMIBOLD);
    HFONT glyphs = fonts().Get(PhiFontDesc{14, FW_NORMAL, PhiIconFaceName()});
    HFONT check_glyph =
        fonts().Get(PhiFontDesc{12, FW_SEMIBOLD, PhiIconFaceName()});
    HGDIOBJ previous = ::SelectObject(dc, body);
    for (const RECT& card : cards_) {
      if (card.bottom > card.top) {
        PhiFillRoundRectAA(dc, card, Scaled(kCardRadius), c.card);
        PhiStrokeRoundRectAA(dc, card, Scaled(kCardRadius), c.card_border, 1);
      }
    }
    if (heading_.bottom > heading_.top) {
      ::SelectObject(dc, strong);
      ::SetTextColor(dc, c.secondary);
      RECT heading = heading_;
      PhiDrawTextAA(dc,
                    PhiStr(
                        "settings.privacy.contentBlocking.ruleSets.customSection"),
                    -1, &heading,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                        DT_END_ELLIPSIS);
    }
    const int hot = hovered();
    // The ring only once the keyboard is in use (WinUI's focus visual).
    const bool keyboard = keyboard_cues_ && ::GetFocus() == hwnd();
    // Rows: title over the availability line, the progress bar while a
    // download runs.
    for (const Row& row : rows_) {
      if (row.divider_above) {
        RECT divider = {row.bounds.left + 1, row.bounds.top,
                        row.bounds.right - 1, row.bounds.top + 1};
        HBRUSH sep = ::CreateSolidBrush(c.separator);
        ::FillRect(dc, &divider, sep);
        ::DeleteObject(sep);
      }
      const int text_left = Scaled(52);
      const int text_right =
          row.bounds.right - Scaled(8 + 4 + 2 * kGlyphButton + 8) -
          (row.downloading ? Scaled(kProgressWidth) : 0);
      const int mid = (row.bounds.top + row.bounds.bottom) / 2;
      ::SelectObject(dc, body);
      ::SetTextColor(dc, c.label);
      RECT title = row.line.empty()
                       ? RECT{text_left, row.bounds.top, text_right,
                              row.bounds.bottom}
                       : RECT{text_left, mid - Scaled(20), text_right, mid};
      PhiDrawTextAA(dc, row.title.c_str(), -1, &title,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                        DT_END_ELLIPSIS);
      if (!row.line.empty()) {
        ::SelectObject(dc, small_font);
        ::SetTextColor(dc, c.tertiary);
        RECT line = {text_left, mid + Scaled(1), text_right, mid + Scaled(19)};
        PhiDrawTextAA(dc, row.line.c_str(), -1, &line,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                          DT_END_ELLIPSIS);
      }
      if (row.downloading) {
        // The Mac's linear ProgressView when the size is known; otherwise
        // a third of the track, still, beside "Downloading…".
        const int bar_right =
            row.bounds.right - Scaled(8 + kGlyphButton + 4 + 8);
        RECT track = {bar_right - Scaled(kProgressWidth), mid - Scaled(2),
                      bar_right, mid + Scaled(2)};
        PhiFillCapsuleAA(dc, track, PhiPopupBlend(c.ink, c.card, 30));
        RECT fill = track;
        const int width = track.right - track.left;
        fill.right =
            track.left +
            (row.total > 0
                 ? static_cast<int>(std::min<int64_t>(
                       width, width * row.received / std::max<int64_t>(
                                                         1, row.total)))
                 : width / 3);
        if (fill.right - fill.left >= Scaled(4)) {
          PhiFillCapsuleAA(dc, fill, c.accent);
        }
      }
    }
    for (size_t i = 0; i < parts_.size(); ++i) {
      const Part& part = parts_[i];
      const bool is_hot = hot == static_cast<int>(i);
      const bool is_focus = keyboard && focus_ == static_cast<int>(i);
      RECT r = part.bounds;
      switch (part.kind) {
        case PartKind::kCheck: {
          // The WinUI CheckBox: 18 square, radius 4; accent with a white
          // check when on, a stroked box when off.
          const int cx = (r.left + r.right) / 2;
          const int cy = (r.top + r.bottom) / 2;
          RECT box = {cx - Scaled(kCheckSize) / 2, cy - Scaled(kCheckSize) / 2,
                      cx + Scaled(kCheckSize) / 2, cy + Scaled(kCheckSize) / 2};
          if (part.on) {
            PhiFillRoundRectAA(dc, box, Scaled(4),
                               is_hot ? c.accent_hover : c.accent);
            ::SelectObject(dc, check_glyph);
            ::SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
            PhiDrawTextAA(dc, L"", -1, &box,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
          } else {
            PhiFillRoundRectAA(dc, box, Scaled(4),
                               is_hot ? PhiPopupBlend(c.ink, c.card, 14)
                                      : c.card);
            PhiStrokeRoundRectAA(dc, box, Scaled(4),
                                 PhiPopupBlend(c.ink, c.card, 140), 1);
          }
          break;
        }
        case PartKind::kControl:
        case PartKind::kInfo: {
          if (is_hot) {
            PhiFillRoundRectAA(dc, r, Scaled(4),
                               PhiPopupBlend(c.ink, c.card, c.dark ? 16 : 12));
          }
          const wchar_t* glyph =
              part.kind == PartKind::kInfo        ? L""  // Info
              : part.control == Control::kRemove ? L""  // Delete
                                                  : L"";  // Download
          ::SelectObject(dc, glyphs);
          ::SetTextColor(dc, is_hot && part.control == Control::kRemove
                                 ? c.destructive
                                 : c.secondary);
          PhiDrawTextAA(dc, glyph, -1, &r,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
          break;
        }
        case PartKind::kAdd: {
          if (part.divider_above) {
            RECT divider = {1, part.row_top, r.right + Scaled(16) - 1,
                            part.row_top + 1};
            HBRUSH sep = ::CreateSolidBrush(c.separator);
            ::FillRect(dc, &divider, sep);
            ::DeleteObject(sep);
          }
          PhiPopupPaintButton(
              dc, r, PhiStr("settings.privacy.contentBlocking.advanced.addFilter"),
              PhiPopupButtonRole::kSecondary, is_hot, false, true, c, body,
              dpi());
          break;
        }
        case PartKind::kNone:
          break;
      }
      if (is_focus) {
        RECT ring = r;
        ::InflateRect(&ring, Scaled(2), Scaled(2));
        PhiStrokeRoundRectAA(dc, ring, Scaled(6), c.label, Scaled(2));
      }
    }
    // A thin thumb when the lists outgrow the viewport.
    const int viewport = static_cast<int>(client.bottom);
    if (content_height_ > viewport && viewport > 0) {
      const int track = viewport - 2 * Scaled(2);
      const int thumb_h =
          std::max(Scaled(24), track * viewport / content_height_);
      const int thumb_y =
          Scaled(2) + (track - thumb_h) * scroll_ /
                          std::max(1, content_height_ - viewport);
      RECT thumb = {client.right - Scaled(kScrollbarWidth), thumb_y,
                    client.right, thumb_y + thumb_h};
      PhiFillRoundRectAA(dc, thumb, Scaled(2), PhiPopupBlend(c.ink, c.bg, 70));
    }
    ::SelectObject(dc, previous);
  }

  void OnSize(int, int) override { Relayout(); }

  void OnHover(int, int) override { Invalidate(); }

  void OnClick(int hit, POINT) override {
    if (hit < 0) {
      return;
    }
    focus_ = hit;
    keyboard_cues_ = false;
    Activate(hit);
  }

  void OnWheel(int delta, POINT) override {
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    const int limit =
        std::max(0, content_height_ - static_cast<int>(client.bottom));
    const int next = std::clamp(scroll_ - delta * Scaled(kWheelStep) / WHEEL_DELTA,
                                0, limit);
    if (next != scroll_) {
      scroll_ = next;
      Relayout();
    }
  }

  bool OnKey(WPARAM key) override {
    keyboard_cues_ = true;
    switch (key) {
      case VK_UP:
      case VK_LEFT:
        MoveFocus(-1);
        return true;
      case VK_DOWN:
      case VK_RIGHT:
        MoveFocus(1);
        return true;
      case VK_HOME:
        focus_ = -1;
        MoveFocus(1);
        return true;
      case VK_END:
        focus_ = -1;
        MoveFocus(-1);
        return true;
      case VK_SPACE:
      case VK_RETURN:
        Activate(focus_);
        return true;
      case VK_ESCAPE:
        window_->CloseFromList();
        return true;
      default:
        return false;
    }
  }

  // Tab and Shift+Tab stay the dialog manager's: the panel is one stop.
  UINT DlgCode() const override { return DLGC_WANTARROWS | DLGC_WANTCHARS; }

  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) override {
    switch (message) {
      case WM_SETFOCUS:
        if (focus_ < 0 && !parts_.empty()) {
          focus_ = 0;
        }
        // Tabbed in: show where the focus landed.
        if (::GetKeyState(VK_TAB) < 0) {
          keyboard_cues_ = true;
        }
        Invalidate();
        break;
      case WM_KILLFOCUS:
        Invalidate();
        break;
      case WM_CHAR:
        if (wparam == L' ' || wparam == L'\r' || wparam == 0x1B) {
          *handled = true;  // Answered as WM_KEYDOWN; no beep.
          return 0;
        }
        break;
      default:
        break;
    }
    return PhiPanel::OnMessage(message, wparam, lparam, handled);
  }

  RAW_PTR_EXCLUSION PhiContentBlockingWindow* const window_;
  std::vector<Part> parts_;
  std::vector<Row> rows_;
  std::vector<RECT> cards_;
  RECT heading_ = {};
  int scroll_ = 0;
  int content_height_ = 0;
  int focus_ = -1;
  bool keyboard_cues_ = false;
};

// ---------------------------------------------------------------------------
// The window.

// static
void PhiContentBlockingWindow::Open(PhiSettingsHost* host,
                                    const std::string& profile_id,
                                    Category category,
                                    bool enable_on_close,
                                    HWND anchor) {
  if (!host || profile_id.empty()) {
    return;
  }
  if (g_window && g_window->alive() && !g_window->closing_) {
    g_window->Retarget(profile_id, category, enable_on_close);
  } else {
    if (g_window && g_window->alive()) {
      g_window->quiet_close_ = true;
      g_window->Destroy();  // A closing one: let a fresh one take over.
    }
    auto* window =
        new PhiContentBlockingWindow(host, profile_id, category, enable_on_close);
    if (!window->OpenWindow(anchor)) {
      delete window;
      return;
    }
    window->DeleteWhenDestroyed();
    g_window = window;
  }
  ::ShowWindow(g_window->hwnd(),
               ::IsIconic(g_window->hwnd()) ? SW_RESTORE : SW_SHOW);
  ::SetForegroundWindow(g_window->hwnd());
  if (g_window->list_ && g_window->list_->alive()) {
    ::SetFocus(g_window->list_->hwnd());
  }
}

// static
HWND PhiContentBlockingWindow::Handle() {
  return g_window && g_window->alive() && !g_window->closing_
             ? g_window->hwnd()
             : nullptr;
}

// static
void PhiContentBlockingWindow::CloseQuietly() {
  if (g_window && g_window->alive()) {
    g_window->quiet_close_ = true;
    g_window->Destroy();
  }
}

PhiContentBlockingWindow::PhiContentBlockingWindow(
    PhiSettingsHost* host,
    const std::string& profile_id,
    Category category,
    bool enable_on_close)
    : host_(host),
      blocking_(&host->ContentBlocking()),
      profile_id_(profile_id),
      category_(category),
      enable_on_close_(enable_on_close) {
  ++g_window_serial;
}

PhiContentBlockingWindow::~PhiContentBlockingWindow() {
  list_.reset();
  for (HBRUSH brush : {bg_brush_, field_brush_}) {
    if (brush) {
      ::DeleteObject(brush);
    }
  }
}

const PhiContentBlockingState* PhiContentBlockingWindow::state() const {
  return blocking_->State(profile_id_);
}

bool PhiContentBlockingWindow::OpenWindow(HWND anchor) {
  ClassSpec spec;
  spec.name = kClassName;
  Options options;
  options.caption_buttons = kPhiCaptionClose;
  options.show_mark = false;  // A dialog: no Phi mark (§2.2b).
  options.dialog_keys = kPhiDialogTab;
  const RECT bounds = {CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT + 100,
                       CW_USEDEFAULT + 100};
  // Owned by Settings, so it stays in front of it and goes with it.
  if (!CreateSecondary(spec, 0,
                       WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                       bounds, anchor && ::IsWindow(anchor) ? anchor : nullptr,
                       Title(), options)) {
    LOG(ERROR) << "[ContentBlocking] CreateWindowEx failed err="
               << ::GetLastError();
    return false;
  }
  OnColorsChanged();
  const SIZE outer = chrome().OuterSize(kWidth, kHeight);
  ::SetWindowPos(hwnd(), nullptr, 0, 0, outer.cx, outer.cy,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  CenterOn(anchor);
  blocking_->AddObserver(this);
  Build();
  ShowPage(Page::kChooser);
  blocking_->Refresh(profile_id_);
  return true;
}

void PhiContentBlockingWindow::Retarget(const std::string& profile_id,
                                        Category category,
                                        bool enable_on_close) {
  profile_id_ = profile_id;
  category_ = category;
  enable_on_close_ = enable_on_close;
  waiting_for_downloads_ = false;
  ::SetWindowTextW(hwnd(), Title());
  ShowPage(Page::kChooser);
  blocking_->Refresh(profile_id_);
}

const wchar_t* PhiContentBlockingWindow::Title() const {
  switch (category_) {
    case Category::kAds:
      return PhiStr("settings.privacy.contentBlocking.ruleSets.title.ads");
    case Category::kTrackers:
      return PhiStr("settings.privacy.contentBlocking.ruleSets.title.trackers");
    case Category::kCookieBanners:
      return PhiStr("settings.privacy.contentBlocking.ruleSets.title.cookies");
  }
  return L"";
}

HWND PhiContentBlockingWindow::Child(const wchar_t* cls,
                                     const wchar_t* text,
                                     DWORD style,
                                     UINT_PTR id) {
  HWND control = ::CreateWindowExW(
      0, cls, text, WS_CHILD | style, 0, 0, 10, 10, hwnd(),
      reinterpret_cast<HMENU>(id), ::GetModuleHandleW(nullptr), nullptr);
  if (control) {
    ::SetWindowSubclass(control, &PhiContentBlockingWindow::ControlProc, id,
                        reinterpret_cast<DWORD_PTR>(this));
  }
  return control;
}

void PhiContentBlockingWindow::Build() {
  update_ = Child(L"BUTTON",
                  PhiStr("settings.privacy.contentBlocking.ruleSets.update"),
                  WS_TABSTOP | BS_PUSHBUTTON, kIdUpdate);
  done_ = Child(L"BUTTON", PhiStr("settings.privacy.contentBlocking.ruleSets.done"),
                WS_TABSTOP | BS_DEFPUSHBUTTON, kIdDone);
  PhiPopupStyleButton(update_, PhiPopupButtonRole::kSecondary);
  PhiPopupStyleButton(done_, PhiPopupButtonRole::kPrimary);
  // The URL | Custom segment: two owner-drawn buttons this window paints,
  // the chosen one in the accent.
  kind_url_ = Child(
      L"BUTTON", PhiStr("settings.privacy.contentBlocking.customFilter.kind.url"),
      WS_TABSTOP | BS_OWNERDRAW, kIdKindUrl);
  kind_rules_button_ = Child(
      L"BUTTON",
      PhiStr("settings.privacy.contentBlocking.customFilter.kind.rules"),
      WS_TABSTOP | BS_OWNERDRAW, kIdKindRules);
  name_ = Child(L"EDIT", L"", WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL, kIdName);
  url_ = Child(L"EDIT", L"", WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL, kIdUrl);
  rules_ = Child(L"EDIT", L"",
                 WS_TABSTOP | WS_VSCROLL | ES_LEFT | ES_MULTILINE |
                     ES_AUTOVSCROLL | ES_WANTRETURN,
                 kIdRules);
  add_ = Child(L"BUTTON",
               PhiStr("settings.privacy.contentBlocking.customFilter.add"),
               WS_TABSTOP | BS_DEFPUSHBUTTON, kIdAdd);
  cancel_ = Child(L"BUTTON",
                  PhiStr("settings.privacy.contentBlocking.customFilter.cancel"),
                  WS_TABSTOP | BS_PUSHBUTTON, kIdCancel);
  PhiPopupStyleButton(add_, PhiPopupButtonRole::kPrimary);
  PhiPopupStyleButton(cancel_, PhiPopupButtonRole::kSecondary);
  ::SendMessageW(
      name_, EM_SETCUEBANNER, TRUE,
      reinterpret_cast<LPARAM>(PhiStr(
          "settings.privacy.contentBlocking.customFilter.namePlaceholder")));
  // The example URL is not a translatable string (the Mac's verbatim).
  ::SendMessageW(url_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"https://example.com/filters.txt"));
  for (HWND field : {name_, url_, rules_}) {
    PhiPopupThemeControl(field, colors().dark);
  }
  ApplyFonts();
  list_ = std::make_unique<ListPanel>(this);
  Layout();
  if (!list_->Mount(list_bounds_)) {
    LOG(ERROR) << "[ContentBlocking] the list could not be created";
    list_.reset();
  }
  SyncControls();
}

void PhiContentBlockingWindow::ApplyFonts() {
  HFONT body = fonts().Get(14, FW_NORMAL);
  for (HWND control :
       {update_, done_, kind_url_, kind_rules_button_, name_, url_, add_,
        cancel_}) {
    if (control) {
      ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(body), TRUE);
    }
  }
  if (rules_) {
    // Filter rules read in a fixed-pitch face, as the Mac's monospaced
    // editor does.
    ::SendMessageW(rules_, WM_SETFONT,
                   reinterpret_cast<WPARAM>(
                       fonts().Get(PhiFontDesc{13, FW_NORMAL, L"Consolas"})),
                   TRUE);
  }
}

void PhiContentBlockingWindow::Layout() {
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  const int band = CaptionHeight();
  const int left = Scaled(kPad);
  const int right = client.right - Scaled(kPad);
  header_ = {left, band + Scaled(12), right,
             band + Scaled(12) + Scaled(kTitleHeight)};
  // The intro, wrapped.
  int intro_h = 0;
  {
    HDC dc = ::GetDC(hwnd());
    HGDIOBJ previous = ::SelectObject(dc, fonts().Get(14, FW_NORMAL));
    RECT measure = {0, 0, right - left, 0};
    PhiDrawTextAA(dc, PhiStr("settings.privacy.contentBlocking.ruleSets.intro"),
                  -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    intro_h = static_cast<int>(measure.bottom);
    ::SelectObject(dc, previous);
    ::ReleaseDC(hwnd(), dc);
  }
  intro_ = {left, header_.bottom + Scaled(kTitleGap), right,
            header_.bottom + Scaled(kTitleGap) + intro_h};
  footer_ = {client.left, client.bottom - Scaled(kFooterHeight), client.right,
             client.bottom};
  list_bounds_ = {left, intro_.bottom + Scaled(16), right + Scaled(8),
                  footer_.top - Scaled(8)};
  if (list_ && list_->alive()) {
    list_->SetPanelBounds(list_bounds_);
  }
  const int button_y =
      footer_.top + (Scaled(kFooterHeight) - Scaled(kButtonHeight)) / 2;
  const auto place = [&](HWND control, int x, int y, int w, int h) {
    if (control) {
      ::SetWindowPos(control, nullptr, x, y, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
  };
  // Chooser: Update Downloaded leading, Done trailing (the Mac's HStack).
  place(update_, left, button_y, Scaled(kUpdateWidth), Scaled(kButtonHeight));
  place(done_, right - Scaled(kDoneWidth), button_y, Scaled(kDoneWidth),
        Scaled(kButtonHeight));
  // Custom filter: the segment, Name, then the URL or the rules.
  int y = header_.bottom + Scaled(16);
  place(kind_url_, left, y, Scaled(kSegmentWidth), Scaled(kButtonHeight));
  place(kind_rules_button_, left + Scaled(kSegmentWidth), y,
        Scaled(kSegmentWidth), Scaled(kButtonHeight));
  y += Scaled(kButtonHeight) + Scaled(kFieldGap);
  name_label_ = {left, y, right, y + Scaled(kLabelHeight)};
  y = name_label_.bottom + Scaled(6);
  place(name_, left + Scaled(10), y + Scaled(6), right - left - Scaled(20),
        Scaled(kFieldHeight) - Scaled(12));
  y += Scaled(kFieldHeight) + Scaled(kFieldGap);
  source_label_ = {left, y, right, y + Scaled(kLabelHeight)};
  y = source_label_.bottom + Scaled(6);
  place(url_, left + Scaled(10), y + Scaled(6), right - left - Scaled(20),
        Scaled(kFieldHeight) - Scaled(12));
  place(rules_, left + Scaled(8), y + Scaled(6), right - left - Scaled(16),
        Scaled(kRulesHeight) - Scaled(12));
  const int source_h = kind_rules_ ? kRulesHeight : kFieldHeight;
  y += Scaled(source_h) + Scaled(12);
  error_ = {left, y, right, footer_.top - Scaled(8)};
  // Add then Cancel, trailing (affirmative first, §6).
  place(cancel_, right - Scaled(kDoneWidth), button_y, Scaled(kDoneWidth),
        Scaled(kButtonHeight));
  place(add_, right - 2 * Scaled(kDoneWidth) - Scaled(kButtonGap), button_y,
        Scaled(kDoneWidth), Scaled(kButtonHeight));
  Invalidate();
}

void PhiContentBlockingWindow::SyncControls() {
  if (!alive()) {
    return;
  }
  const PhiContentBlockingState* s = state();
  const bool downloading = s && PhiContentBlockingAnyDownloading(category_, *s);
  const bool chooser = page_ == Page::kChooser;
  if (done_) {
    ::SetWindowTextW(
        done_, PhiStr(downloading
                          ? "settings.privacy.contentBlocking.ruleSets.downloading"
                          : "settings.privacy.contentBlocking.ruleSets.done"));
    ::EnableWindow(done_, !downloading);
    ::ShowWindow(done_, chooser ? SW_SHOW : SW_HIDE);
  }
  if (update_) {
    const bool any_downloaded =
        s && !PhiContentBlockingDownloadedIds(category_, *s).empty();
    ::ShowWindow(update_, chooser && any_downloaded ? SW_SHOW : SW_HIDE);
    ::EnableWindow(update_, !downloading);
  }
  if (list_ && list_->alive()) {
    ::ShowWindow(list_->hwnd(), chooser ? SW_SHOW : SW_HIDE);
    list_->Relayout();
  }
  for (HWND control : {kind_url_, kind_rules_button_, name_, add_, cancel_}) {
    if (control) {
      ::ShowWindow(control, chooser ? SW_HIDE : SW_SHOW);
    }
  }
  if (url_) {
    ::ShowWindow(url_, !chooser && !kind_rules_ ? SW_SHOW : SW_HIDE);
  }
  if (rules_) {
    ::ShowWindow(rules_, !chooser && kind_rules_ ? SW_SHOW : SW_HIDE);
  }
  if (add_) {
    ::EnableWindow(add_, !submitting_);
  }
  if (kind_url_) {
    ::InvalidateRect(kind_url_, nullptr, FALSE);
  }
  if (kind_rules_button_) {
    ::InvalidateRect(kind_rules_button_, nullptr, FALSE);
  }
  Invalidate();
}

void PhiContentBlockingWindow::ShowPage(Page page) {
  page_ = page;
  if (page == Page::kAddFilter) {
    kind_rules_ = false;
    attempted_ = false;
    submitting_ = false;
    submit_error_.clear();
    for (HWND field : {name_, url_, rules_}) {
      if (field) {
        ::SetWindowTextW(field, L"");
      }
    }
    ::SetWindowTextW(
        hwnd(), PhiStr("settings.privacy.contentBlocking.customFilter.title"));
  } else {
    ::SetWindowTextW(hwnd(), Title());
  }
  Layout();
  SyncControls();
  if (page == Page::kAddFilter && name_) {
    ::SetFocus(name_);
  } else if (list_ && list_->alive()) {
    ::SetFocus(list_->hwnd());
  }
}

void PhiContentBlockingWindow::SetKind(bool rules) {
  kind_rules_ = rules;
  submit_error_.clear();
  Layout();
  SyncControls();
  ::SetFocus(rules ? rules_ : url_);
}

void PhiContentBlockingWindow::OnContentBlockingChanged(
    const std::string& profile_id) {
  if (!alive() || closing_ || profile_id != profile_id_) {
    return;
  }
  SyncControls();
  const PhiContentBlockingState* s = state();
  // Close once the downloads Done started have all landed (the Mac's
  // onChange); a failure leaves the window up with its error line.
  if (waiting_for_downloads_ && s &&
      !PhiContentBlockingAnyDownloading(category_, *s)) {
    waiting_for_downloads_ = false;
    if (PhiContentBlockingMissingIds(category_, *s).empty()) {
      CloseSoon();
    }
  }
}

void PhiContentBlockingWindow::ToggleList(const std::string& list_id,
                                          bool checked) {
  blocking_->SetList(profile_id_, list_id, checked);
}

void PhiContentBlockingWindow::DownloadList(const std::string& list_id) {
  blocking_->DownloadLists(profile_id_, {list_id});
}

void PhiContentBlockingWindow::RemoveCustomList(const std::string& list_id) {
  blocking_->RemoveCustomList(profile_id_, list_id);
}

void PhiContentBlockingWindow::ShowListInfo(const std::string& list_id) {
  const PhiContentBlockingState* s = state();
  if (!s) {
    return;
  }
  const PhiContentBlockingList* found = nullptr;
  for (const PhiContentBlockingList& list : s->lists) {
    if (list.id == list_id) {
      found = &list;
    }
  }
  if (!found) {
    return;
  }
  // Copies: the alert's loop lets the state change under it.
  const PhiContentBlockingList list = *found;
  const std::wstring title = PhiContentBlockingListTitle(list);
  std::wstring message = PhiContentBlockingListDescription(list);
  const auto add_line = [&message](const std::wstring& line) {
    if (!line.empty()) {
      message += (message.empty() ? L"" : L"\n\n") + line;
    }
  };
  add_line(Widen(list.source_url));
  add_line(UpdateLine(list));
  if (!list.license.empty()) {
    add_line(PhiStrF("settings.privacy.contentBlocking.listInfo.license",
                     {Widen(list.license)}));
  }
  std::vector<PhiAlertAction> actions;
  int close_index = 0;
  if (!list.homepage.empty()) {
    actions.push_back({PhiStr("settings.privacy.contentBlocking.listInfo.source"),
                       PhiAlertRole::kSecondary});
    close_index = 1;
  }
  actions.push_back({PhiStr("settings.privacy.contentBlocking.listInfo.close"),
                     PhiAlertRole::kPrimary});
  const int pressed =
      PhiAlert(hwnd(), title.c_str(), message.c_str(), actions, close_index,
               close_index);
  if (!list.homepage.empty() && pressed == 0) {
    host_->OpenBrowserPageInProfile(profile_id_, list.homepage);
  }
}

void PhiContentBlockingWindow::ShowAddFilter() {
  ShowPage(Page::kAddFilter);
}

void PhiContentBlockingWindow::CloseFromList() {
  CloseSoon();
}

void PhiContentBlockingWindow::Done() {
  const PhiContentBlockingState* s = state();
  if (!s) {
    CloseSoon();
    return;
  }
  const std::vector<std::string> missing =
      PhiContentBlockingMissingIds(category_, *s);
  if (missing.empty()) {
    CloseSoon();
    return;
  }
  waiting_for_downloads_ = true;
  blocking_->DownloadLists(profile_id_, missing);
}

void PhiContentBlockingWindow::UpdateDownloaded() {
  const PhiContentBlockingState* s = state();
  if (!s) {
    return;
  }
  const std::vector<std::string> ids =
      PhiContentBlockingDownloadedIds(category_, *s);
  if (!ids.empty()) {
    blocking_->DownloadLists(profile_id_, ids);
  }
}

void PhiContentBlockingWindow::SubmitFilter() {
  if (submitting_) {
    return;
  }
  attempted_ = true;
  submit_error_.clear();
  const std::wstring name = Trimmed(WindowText(name_));
  const std::wstring url = Trimmed(WindowText(url_));
  const std::wstring rules = WindowText(rules_);
  // The Mac's ContentBlockingCustomFilterInput.validationError; Chromium
  // validates again.
  if (name.empty()) {
    submit_error_ =
        PhiStr("settings.privacy.contentBlocking.customFilter.error.name");
  } else if (!kind_rules_) {
    std::wstring lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    const size_t scheme_end = lower.find(L"://");
    const std::wstring scheme =
        scheme_end == std::wstring::npos ? L"" : lower.substr(0, scheme_end);
    const std::wstring rest = scheme_end == std::wstring::npos
                                  ? L""
                                  : lower.substr(scheme_end + 3);
    const size_t host_end = rest.find_first_of(L"/?#:");
    const std::wstring host =
        host_end == std::wstring::npos ? rest : rest.substr(0, host_end);
    if ((scheme != L"http" && scheme != L"https") || host.empty() ||
        url.find(L' ') != std::wstring::npos) {
      submit_error_ =
          PhiStr("settings.privacy.contentBlocking.customFilter.error.url");
    }
  } else if (Trimmed(rules).empty()) {
    submit_error_ =
        PhiStr("settings.privacy.contentBlocking.customFilter.error.rules");
  }
  if (!submit_error_.empty()) {
    Invalidate();
    return;
  }
  submitting_ = true;
  SyncControls();
  const int serial = g_window_serial;
  PhiContentBlockingWindow* self = this;
  std::wstring rules_text = rules;
  // Windows line ends to the plain ones the rule parser expects.
  rules_text.erase(std::remove(rules_text.begin(), rules_text.end(), L'\r'),
                   rules_text.end());
  blocking_->AddCustomList(
      profile_id_, base::WideToUTF8(name),
      kind_rules_ ? std::string() : base::WideToUTF8(url),
      kind_rules_ ? base::WideToUTF8(rules_text) : std::string(),
      [self, serial](bool ok, const std::string& result) {
        if (g_window != self || g_window_serial != serial || !self->alive()) {
          return;  // The window went while the add was out.
        }
        self->submitting_ = false;
        if (ok) {
          self->ShowPage(Page::kChooser);
        } else {
          self->submit_error_ = Widen(result);
          self->SyncControls();
        }
      });
}

void PhiContentBlockingWindow::CloseSoon() {
  if (closing_ || !alive()) {
    return;
  }
  closing_ = true;
  ::ShowWindow(hwnd(), SW_HIDE);
  ::PostMessageW(hwnd(), WM_CLOSE, 0, 0);
}

LRESULT CALLBACK PhiContentBlockingWindow::ControlProc(HWND hwnd,
                                                       UINT msg,
                                                       WPARAM wparam,
                                                       LPARAM lparam,
                                                       UINT_PTR id,
                                                       DWORD_PTR data) {
  auto* self = reinterpret_cast<PhiContentBlockingWindow*>(data);
  if (msg == WM_NCDESTROY) {
    ::RemoveWindowSubclass(hwnd, &PhiContentBlockingWindow::ControlProc, id);
    return ::DefSubclassProc(hwnd, msg, wparam, lparam);
  }
  if (!self || !self->alive()) {
    return ::DefSubclassProc(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_KEYDOWN) {
    if (wparam == VK_ESCAPE) {
      self->OnEscape();
      return 0;
    }
    if (wparam == VK_RETURN && id != kIdRules) {
      wchar_t cls[16] = {};
      ::GetClassNameW(hwnd, cls, 16);
      if (::_wcsicmp(cls, L"Button") == 0) {
        ::SendMessageW(hwnd, BM_CLICK, 0, 0);
      } else if (self->page_ == Page::kAddFilter) {
        self->SubmitFilter();  // Return in a field adds, as the Mac's sheet.
      }
      return 0;
    }
  }
  if (msg == WM_CHAR && (wparam == VK_ESCAPE ||
                         (wparam == VK_RETURN && id != kIdRules))) {
    return 0;
  }
  return ::DefSubclassProc(hwnd, msg, wparam, lparam);
}

void PhiContentBlockingWindow::Paint(HDC dc, const RECT&) {
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  ::FillRect(dc, &client, bg_brush_);
  PaintBand(dc, L"");  // The close button alone; the title is the page's.
  ::SetBkMode(dc, TRANSPARENT);
  HGDIOBJ previous = ::SelectObject(dc, fonts().Get(20, FW_SEMIBOLD));
  ::SetTextColor(dc, colors().label_strong);
  RECT title = header_;
  const wchar_t* title_text =
      page_ == Page::kChooser
          ? Title()
          : PhiStr("settings.privacy.contentBlocking.customFilter.title");
  PhiDrawTextAA(dc, title_text, -1, &title,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                    DT_END_ELLIPSIS);
  ::SelectObject(dc, fonts().Get(14, FW_NORMAL));
  if (page_ == Page::kChooser) {
    ::SetTextColor(dc, colors().secondary);
    RECT intro = intro_;
    PhiDrawTextAA(dc, PhiStr("settings.privacy.contentBlocking.ruleSets.intro"),
                  -1, &intro, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
  } else {
    ::SetTextColor(dc, colors().label);
    RECT name_label = name_label_;
    PhiDrawTextAA(dc,
                  PhiStr("settings.privacy.contentBlocking.customFilter.name"),
                  -1, &name_label,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT source_label = source_label_;
    PhiDrawTextAA(
        dc,
        PhiStr(kind_rules_
                   ? "settings.privacy.contentBlocking.customFilter.rules"
                   : "settings.privacy.contentBlocking.customFilter.url"),
        -1, &source_label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    // The fields' wells (the Fluent TextBox look the prompt draws too).
    for (HWND field : {name_, kind_rules_ ? rules_ : url_}) {
      RECT r = {};
      ::GetWindowRect(field, &r);
      ::MapWindowPoints(nullptr, hwnd(), reinterpret_cast<POINT*>(&r), 2);
      ::InflateRect(&r, Scaled(field == rules_ ? 8 : 10), Scaled(6));
      const bool focused = ::GetFocus() == field;
      PhiFillRoundRectAA(dc, r, Scaled(4), FieldColor());
      PhiStrokeRoundRectAA(dc, r, Scaled(4), colors().card_border, 1);
      if (focused) {
        RECT edge = {r.left + Scaled(2), r.bottom - Scaled(2),
                     r.right - Scaled(2), r.bottom};
        PhiFillRoundRectAA(dc, edge, Scaled(1), colors().accent);
      }
    }
    const std::wstring& message = submit_error_;
    if (!message.empty()) {
      ::SelectObject(dc, fonts().Get(13, FW_NORMAL));
      ::SetTextColor(dc, colors().error);
      RECT error = error_;
      PhiDrawTextAA(dc, message.c_str(), -1, &error,
                    DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    }
  }
  // The footer's separator.
  RECT separator = {footer_.left, footer_.top, footer_.right, footer_.top + 1};
  HBRUSH sep = ::CreateSolidBrush(colors().separator);
  ::FillRect(dc, &separator, sep);
  ::DeleteObject(sep);
  ::SelectObject(dc, previous);
}

COLORREF PhiContentBlockingWindow::FieldColor() const {
  return colors().card;
}

LRESULT PhiContentBlockingWindow::OnWindowMessage(UINT msg,
                                                  WPARAM wparam,
                                                  LPARAM lparam,
                                                  bool* handled) {
  switch (msg) {
    case WM_DRAWITEM: {
      auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
      if (item->CtlID == kIdKindUrl || item->CtlID == kIdKindRules) {
        const bool chosen = (item->CtlID == kIdKindRules) == kind_rules_;
        PhiPopupPaintButton(item->hDC, item->rcItem, WindowText(item->hwndItem),
                            chosen ? PhiPopupButtonRole::kPrimary
                                   : PhiPopupButtonRole::kSecondary,
                            false, (item->itemState & ODS_SELECTED) != 0,
                            (item->itemState & ODS_DISABLED) == 0, colors(),
                            fonts().Get(14, FW_NORMAL), dpi());
        if (item->itemState & ODS_FOCUS) {
          RECT ring = item->rcItem;
          ::InflateRect(&ring, -Scaled(2), -Scaled(2));
          PhiStrokeRoundRectAA(item->hDC, ring, Scaled(4), colors().label, 1);
        }
        *handled = true;
        return TRUE;
      }
      if (PhiPopupDrawButton(*item, colors(), fonts().Get(14, FW_NORMAL))) {
        *handled = true;
        return TRUE;
      }
      break;
    }
    case WM_CTLCOLOREDIT: {
      HDC dc = reinterpret_cast<HDC>(wparam);
      ::SetTextColor(dc, colors().label);
      ::SetBkColor(dc, FieldColor());
      *handled = true;
      return reinterpret_cast<LRESULT>(field_brush_);
    }
    case WM_COMMAND: {
      const UINT_PTR id = LOWORD(wparam);
      const UINT code = HIWORD(wparam);
      if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
        Invalidate();  // The focused field's accent edge.
        break;
      }
      if (code != BN_CLICKED) {
        break;
      }
      *handled = true;
      switch (id) {
        case kIdDone:
          Done();
          return 0;
        case kIdUpdate:
          UpdateDownloaded();
          return 0;
        case kIdKindUrl:
          SetKind(false);
          return 0;
        case kIdKindRules:
          SetKind(true);
          return 0;
        case kIdAdd:
          SubmitFilter();
          return 0;
        case kIdCancel:
          ShowPage(Page::kChooser);
          return 0;
        default:
          *handled = false;
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
      Destroy();
      *handled = true;
      return 0;
    default:
      break;
  }
  return PhiSecondaryWindow::OnWindowMessage(msg, wparam, lparam, handled);
}

void PhiContentBlockingWindow::OnEscape() {
  if (page_ == Page::kAddFilter) {
    ShowPage(Page::kChooser);  // The inner sheet's Cancel.
  } else {
    CloseSoon();
  }
}

void PhiContentBlockingWindow::OnColorsChanged() {
  for (HBRUSH* slot : {&bg_brush_, &field_brush_}) {
    if (*slot) {
      ::DeleteObject(*slot);
      *slot = nullptr;
    }
  }
  bg_brush_ = ::CreateSolidBrush(colors().bg);
  field_brush_ = ::CreateSolidBrush(FieldColor());
  for (HWND field : {name_, url_, rules_}) {
    if (field) {
      PhiPopupThemeControl(field, colors().dark);
    }
  }
  if (list_ && list_->alive()) {
    list_->RebuildBrushes();
  }
  Invalidate();
}

void PhiContentBlockingWindow::OnDpiChanged(UINT old_dpi) {
  PhiSecondaryWindow::OnDpiChanged(old_dpi);
  ApplyFonts();
  Layout();
  if (list_ && list_->alive()) {
    list_->SyncDpiFromParent();
    list_->Relayout();
  }
}

void PhiContentBlockingWindow::OnNcDestroy() {
  blocking_->RemoveObserver(this);
  // The Mac's chooserClosed: a switch the user tried to turn on follows
  // what was picked here once it has something usable.
  if (!quiet_close_ && enable_on_close_) {
    const PhiContentBlockingState* s = state();
    if (s && !s->Switch(category_) &&
        !PhiContentBlockingNeedsDownload(category_, *s)) {
      blocking_->SetCategory(profile_id_, category_, true);
    }
  }
  if (g_window == this) {
    g_window = nullptr;
  }
  PhiSecondaryWindow::OnNcDestroy();
}

}  // namespace phi_client

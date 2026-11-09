// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_menu_paint.h"

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

#include "client/phi_dwrite.h"
#include "client/phi_fluent.h"
#include "client/phi_icons.h"

namespace phi_client {

namespace {

// The flyout's metrics (dip). Fluent MenuFlyoutItem: 4 dip side margin on
// the highlight, radius 4; 12 dip text padding; a 16 dip icon column; the
// chord 28 dip after the widest label.
constexpr int kSideMargin = 4;
constexpr int kPad = 12;
constexpr int kIconColumn = 16;
constexpr int kIconGap = 12;
constexpr int kShortcutGap = 28;
constexpr int kSeparatorHeight = 9;
constexpr int kSeparatorInset = 12;
constexpr int kArrowRoom = 20;  // The system's submenu arrow at the right.
constexpr int kHighlightRadius = 4;
constexpr int kLabelSize = 14;
constexpr int kShortcutSize = 12;
constexpr int kHintSize = 11;

int Scale(int dip, UINT dpi) {
  return ::MulDiv(dip, static_cast<int>(dpi), 96);
}

// The fonts, once per DPI (menus open and close constantly; a font per
// draw would churn GDI handles).
struct MenuFonts {
  HFONT label = nullptr;
  HFONT label_strong = nullptr;
  HFONT shortcut = nullptr;
  HFONT hint = nullptr;
};

const MenuFonts& FontsFor(UINT dpi) {
  // Leaked on purpose: process-lifetime GDI handles, no exit-time destructor.
  static auto* const fonts = new std::map<UINT, MenuFonts>;
  auto it = fonts->find(dpi);
  if (it == fonts->end()) {
    MenuFonts built;
    built.label = PhiFluentFont(dpi, kLabelSize);
    built.label_strong = PhiFluentFont(dpi, kLabelSize, FW_SEMIBOLD);
    built.shortcut = PhiFluentFont(dpi, kShortcutSize);
    built.hint = PhiFluentFont(dpi, kHintSize);
    it = fonts->emplace(dpi, built).first;
  }
  return it->second;
}

const PhiMenuRow* RowFromData(ULONG_PTR data) {
  if (!data) {
    return nullptr;
  }
  const PhiMenuRow* row = reinterpret_cast<const PhiMenuRow*>(data);
  return row->magic == PhiMenuRow::kMagic ? row : nullptr;
}

// The label as shown: a mnemonic '&' dropped, "&&" read as one literal '&'
// (DrawText's own prefix rule).
std::wstring StripMnemonic(const std::wstring& label) {
  std::wstring out;
  for (size_t i = 0; i < label.size(); ++i) {
    if (label[i] == L'&' && i + 1 < label.size()) {
      ++i;  // The next character is shown as is — a '&' too.
    }
    out.push_back(label[i]);
  }
  return out;
}

// The mnemonic character of `label` (the one after a single '&'), or 0.
wchar_t MnemonicOf(const std::wstring& label) {
  for (size_t i = 0; i + 1 < label.size(); ++i) {
    if (label[i] != L'&') {
      continue;
    }
    if (label[i + 1] == L'&') {
      ++i;  // "&&": a literal '&', not a mnemonic.
      continue;
    }
    return label[i + 1];
  }
  return 0;
}

int TextWidth(HDC dc, HFONT font, const std::wstring& text) {
  if (text.empty()) {
    return 0;
  }
  HGDIOBJ previous = ::SelectObject(dc, font);
  SIZE extent = {};
  ::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()),
                          &extent);
  ::SelectObject(dc, previous);
  return extent.cx;
}

// The undocumented uxtheme pair every dark-mode Win32 app uses
// (SetPreferredAppMode ordinal 135, FlushMenuThemes 136; Windows 10 1903+):
// the popup's FRAME and its submenu arrows follow the app mode, which the
// documented API cannot reach. A missing export leaves light frames — the
// rows are still ours.
void ApplyMenuFrameMode(bool dark) {
  static bool last_dark = false;
  static bool applied = false;
  if (applied && last_dark == dark) {
    return;
  }
  applied = true;
  last_dark = dark;
  HMODULE uxtheme = ::GetModuleHandleW(L"uxtheme.dll");
  if (!uxtheme) {
    uxtheme = ::LoadLibraryW(L"uxtheme.dll");
  }
  if (!uxtheme) {
    return;
  }
  using SetPreferredAppModeFn = int(WINAPI*)(int);
  using FlushMenuThemesFn = void(WINAPI*)();
  auto set_mode = reinterpret_cast<SetPreferredAppModeFn>(
      ::GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
  auto flush = reinterpret_cast<FlushMenuThemesFn>(
      ::GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
  if (set_mode) {
    set_mode(dark ? 2 /*ForceDark*/ : 3 /*ForceLight*/);
  }
  if (flush) {
    flush();
  }
}

}  // namespace

// --- The store -------------------------------------------------------------

PhiMenuRowStore::PhiMenuRowStore() = default;
PhiMenuRowStore::~PhiMenuRowStore() = default;

PhiMenuRow* PhiMenuRowStore::Adopt(std::unique_ptr<PhiMenuRow> row) {
  rows_.push_back(std::move(row));
  return rows_.back().get();
}

void PhiMenuRowStore::Release(const PhiMenuRow* row) {
  if (!row) {
    return;
  }
  rows_.erase(std::remove_if(rows_.begin(), rows_.end(),
                             [row](const std::unique_ptr<PhiMenuRow>& r) {
                               return r.get() == row;
                             }),
              rows_.end());
}

void PhiMenuRowStore::ReleaseTree(HMENU menu) {
  const int count = ::GetMenuItemCount(menu);
  for (int i = 0; i < count; ++i) {
    MENUITEMINFOW info = {sizeof(info)};
    info.fMask = MIIM_DATA | MIIM_SUBMENU;
    if (!::GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info)) {
      continue;
    }
    if (info.hSubMenu) {
      ReleaseTree(info.hSubMenu);
    }
    Release(RowFromData(info.dwItemData));
  }
}

namespace {

std::unique_ptr<PhiMenuRow> MakeRow(const std::wstring& text,
                                    const wchar_t* icon) {
  auto row = std::make_unique<PhiMenuRow>();
  PhiMenuSplitText(text, &row->label, &row->shortcut);
  if (icon) {
    row->icon = icon;
  }
  return row;
}

void SetItem(HMENU menu,
             UINT item,
             bool by_position,
             UINT id,
             HMENU sub,
             bool enabled,
             bool separator,
             PhiMenuRow* row,
             bool insert) {
  MENUITEMINFOW info = {sizeof(info)};
  info.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_STATE;
  info.fType = MFT_OWNERDRAW | (separator ? MFT_SEPARATOR : 0u);
  info.dwItemData = reinterpret_cast<ULONG_PTR>(row);
  info.fState = enabled ? MFS_ENABLED : MFS_DISABLED;
  if (sub) {
    info.fMask |= MIIM_SUBMENU;
    info.hSubMenu = sub;
  } else if (!separator) {
    info.fMask |= MIIM_ID;
    info.wID = id;
  }
  if (insert) {
    ::InsertMenuItemW(menu, item, by_position ? TRUE : FALSE, &info);
  } else {
    ::SetMenuItemInfoW(menu, item, by_position ? TRUE : FALSE, &info);
  }
}

}  // namespace

PhiMenuRow* PhiMenuRowStore::AppendCommand(HMENU menu,
                                           UINT id,
                                           const std::wstring& text,
                                           const wchar_t* icon,
                                           bool enabled) {
  PhiMenuRow* row = Adopt(MakeRow(text, icon));
  SetItem(menu, ::GetMenuItemCount(menu), true, id, nullptr, enabled, false,
          row, true);
  return row;
}

PhiMenuRow* PhiMenuRowStore::AppendSubmenu(HMENU menu,
                                           HMENU sub,
                                           const std::wstring& text,
                                           const wchar_t* icon,
                                           bool enabled) {
  PhiMenuRow* row = Adopt(MakeRow(text, icon));
  row->submenu = true;
  SetItem(menu, ::GetMenuItemCount(menu), true, 0, sub, enabled, false, row,
          true);
  return row;
}

void PhiMenuRowStore::AppendSeparator(HMENU menu) {
  auto row = std::make_unique<PhiMenuRow>();
  row->separator = true;
  PhiMenuRow* adopted = Adopt(std::move(row));
  SetItem(menu, ::GetMenuItemCount(menu), true, 0, nullptr, true, true,
          adopted, true);
}

PhiMenuRow* PhiMenuRowStore::InsertCommandBefore(HMENU menu,
                                                 UINT before,
                                                 UINT id,
                                                 const std::wstring& text,
                                                 const wchar_t* icon,
                                                 bool enabled) {
  PhiMenuRow* row = Adopt(MakeRow(text, icon));
  SetItem(menu, before, false, id, nullptr, enabled, false, row, true);
  return row;
}

void PhiMenuRowStore::InsertSeparatorBefore(HMENU menu, UINT before) {
  auto row = std::make_unique<PhiMenuRow>();
  row->separator = true;
  PhiMenuRow* adopted = Adopt(std::move(row));
  SetItem(menu, before, false, 0, nullptr, true, true, adopted, true);
}

void PhiMenuRowStore::ClearMenu(HMENU menu) {
  ReleaseTree(menu);
  while (::GetMenuItemCount(menu) > 0) {
    ::DeleteMenu(menu, 0, MF_BYPOSITION);
  }
}

void PhiMenuRowStore::DeleteCommand(HMENU menu, UINT id) {
  MENUITEMINFOW info = {sizeof(info)};
  info.fMask = MIIM_DATA;
  if (::GetMenuItemInfoW(menu, id, FALSE, &info)) {
    Release(RowFromData(info.dwItemData));
  }
  ::DeleteMenu(menu, id, MF_BYCOMMAND);
}

void PhiMenuRowStore::Clear() {
  rows_.clear();
}

const PhiMenuRow* PhiMenuRowStore::Find(HMENU menu, UINT id) const {
  return PhiMenuRowOf(menu, id);
}

// --- Readers ---------------------------------------------------------------

void PhiMenuSplitText(const std::wstring& text,
                      std::wstring* label,
                      std::wstring* shortcut) {
  const size_t tab = text.find(L'\t');
  *label = text.substr(0, tab);
  *shortcut = tab == std::wstring::npos ? std::wstring() : text.substr(tab + 1);
}

std::wstring PhiEscapeMenuText(const std::wstring& text) {
  std::wstring out;
  out.reserve(text.size());
  for (wchar_t c : text) {
    if (c == L'&') {
      out += L"&&";
    } else if (c == L'\t') {
      out.push_back(L' ');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

const PhiMenuRow* PhiMenuRowAt(HMENU menu, int position) {
  MENUITEMINFOW info = {sizeof(info)};
  info.fMask = MIIM_DATA | MIIM_FTYPE;
  if (!::GetMenuItemInfoW(menu, static_cast<UINT>(position), TRUE, &info) ||
      !(info.fType & MFT_OWNERDRAW)) {
    return nullptr;
  }
  return RowFromData(info.dwItemData);
}

const PhiMenuRow* PhiMenuRowOf(HMENU menu, UINT id) {
  MENUITEMINFOW info = {sizeof(info)};
  info.fMask = MIIM_DATA | MIIM_FTYPE;
  if (!::GetMenuItemInfoW(menu, id, FALSE, &info) ||
      !(info.fType & MFT_OWNERDRAW)) {
    return nullptr;
  }
  return RowFromData(info.dwItemData);
}

std::wstring PhiMenuLabelAt(HMENU menu, int position) {
  if (const PhiMenuRow* row = PhiMenuRowAt(menu, position)) {
    return StripMnemonic(row->label);
  }
  MENUITEMINFOW info = {sizeof(info)};
  info.fMask = MIIM_STRING;
  if (!::GetMenuItemInfoW(menu, static_cast<UINT>(position), TRUE, &info) ||
      info.cch == 0) {
    return std::wstring();
  }
  std::wstring buffer(info.cch + 1, L'\0');
  info.cch = static_cast<UINT>(buffer.size());
  info.dwTypeData = buffer.data();
  ::GetMenuItemInfoW(menu, static_cast<UINT>(position), TRUE, &info);
  buffer.resize(info.cch);
  std::wstring label, shortcut;
  PhiMenuSplitText(buffer, &label, &shortcut);
  return StripMnemonic(label);
}

// --- Theme -----------------------------------------------------------------

void PhiMenuApplyTheme(HMENU root, const PhiPalette& palette) {
  // One brush per colour, kept: a MENUINFO brush must outlive the menu.
  static auto* const brushes = new std::map<COLORREF, HBRUSH>;
  auto it = brushes->find(palette.flyout);
  if (it == brushes->end()) {
    it = brushes->emplace(palette.flyout, ::CreateSolidBrush(palette.flyout))
             .first;
  }
  MENUINFO info = {sizeof(info)};
  info.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
  info.hbrBack = it->second;
  ::SetMenuInfo(root, &info);
  ApplyMenuFrameMode(palette.dark);
}

// --- Measure / draw --------------------------------------------------------

bool PhiMenuOnMeasureItem(HWND owner, MEASUREITEMSTRUCT* measure) {
  if (!measure || measure->CtlType != ODT_MENU) {
    return false;
  }
  const PhiMenuRow* row = RowFromData(measure->itemData);
  if (!row) {
    return false;
  }
  const UINT dpi = owner ? ::GetDpiForWindow(owner) : 96;
  if (row->separator) {
    measure->itemHeight = static_cast<UINT>(Scale(kSeparatorHeight, dpi));
    measure->itemWidth = 0;
    return true;
  }
  HDC dc = ::GetDC(owner);
  const MenuFonts& fonts = FontsFor(dpi);
  int width = Scale(kPad + kIconColumn + kIconGap, dpi) +
              TextWidth(dc, fonts.label_strong, StripMnemonic(row->label));
  if (!row->hint.empty()) {
    width = std::max(width, Scale(kPad + kIconColumn + kIconGap, dpi) +
                                TextWidth(dc, fonts.hint, row->hint));
  }
  if (!row->shortcut.empty()) {
    width += Scale(kShortcutGap, dpi) +
             TextWidth(dc, fonts.shortcut, row->shortcut);
  }
  width += Scale(row->submenu ? kArrowRoom : kPad, dpi);
  ::ReleaseDC(owner, dc);
  width = std::max(width, Scale(row->min_width_dip, dpi));
  // The system adds the check-mark column to what we report, and lays the
  // row out at that total: hand it the width WITHOUT that column so the
  // popup is the width we measured (Chromium's NativeMenuWin does the same).
  width -= ::GetSystemMetricsForDpi(SM_CXMENUCHECK, dpi);
  measure->itemWidth = static_cast<UINT>(std::max(width, 0));
  measure->itemHeight = static_cast<UINT>(Scale(row->height_dip, dpi));
  return true;
}

bool PhiMenuOnDrawItem(HWND owner,
                       const DRAWITEMSTRUCT& draw,
                       const PhiPalette& palette) {
  if (draw.CtlType != ODT_MENU) {
    return false;
  }
  const PhiMenuRow* row = RowFromData(draw.itemData);
  if (!row) {
    return false;
  }
  const UINT dpi = owner ? ::GetDpiForWindow(owner) : 96;
  HDC dc = draw.hDC;
  const RECT& r = draw.rcItem;
  const PhiColor ink = palette.ink;
  // The ground: the flyout's, always repainted (the highlight has to go
  // away when the selection moves).
  HBRUSH ground = ::CreateSolidBrush(palette.flyout);
  ::FillRect(dc, &r, ground);
  ::DeleteObject(ground);
  if (row->separator) {
    RECT line = {r.left + Scale(kSeparatorInset, dpi),
                 (r.top + r.bottom) / 2, r.right - Scale(kSeparatorInset, dpi),
                 (r.top + r.bottom) / 2 + 1};
    HBRUSH sep = ::CreateSolidBrush(PhiBlend(ink, palette.flyout, 18));
    ::FillRect(dc, &line, sep);
    ::DeleteObject(sep);
    return true;
  }
  const bool disabled = (draw.itemState & ODS_DISABLED) != 0 ||
                        (draw.itemState & ODS_GRAYED) != 0;
  const bool selected = (draw.itemState & ODS_SELECTED) != 0;
  const bool checked = (draw.itemState & ODS_CHECKED) != 0;
  const bool hide_prefix = (draw.itemState & ODS_NOACCEL) != 0;
  if (selected && !disabled && !row->header) {
    RECT wash = {r.left + Scale(kSideMargin, dpi), r.top + Scale(2, dpi),
                 r.right - Scale(kSideMargin, dpi), r.bottom - Scale(2, dpi)};
    PhiFillRoundRectAA(dc, wash, Scale(kHighlightRadius, dpi),
                       PhiBlend(ink, palette.flyout, palette.dark ? 15 : 11));
  }
  // The inks: label 87 % (dark 90 %), disabled 36 %, header tertiary,
  // danger the destructive red, chord tertiary.
  COLORREF label_ink = PhiBlend(ink, palette.flyout, palette.dark ? 230 : 222);
  if (row->header) {
    label_ink = PhiBlend(ink, palette.flyout, 107);
  } else if (disabled) {
    label_ink = PhiBlend(ink, palette.flyout, 92);
  } else if (row->danger) {
    label_ink = palette.destructive;
  }
  const COLORREF shortcut_ink = PhiBlend(ink, palette.flyout, disabled ? 92 : 107);
  const MenuFonts& fonts = FontsFor(dpi);
  ::SetBkMode(dc, TRANSPARENT);

  // The icon column: a check (accent) when checked with no picture, else
  // the row's glyph or picture.
  const int icon_px = Scale(kIconColumn, dpi);
  RECT icon = {r.left + Scale(kPad, dpi), (r.top + r.bottom - icon_px) / 2,
               r.left + Scale(kPad, dpi) + icon_px,
               (r.top + r.bottom + icon_px) / 2};
  if (!row->picture.empty()) {
    const int picture_px = Scale(14, dpi);
    if (PhiDrawCatalogIcon(dc, icon, row->picture, picture_px, label_ink)) {
      // The Space icon catalogue's artwork.
    } else if (PhiIsPrivateGlyph(row->picture)) {
      // A bare icon-face glyph (a builder's stand-in): the icon face, never
      // the emoji face, which has no such code point.
      PhiDrawIconGlyph(dc, icon, row->picture, icon_px, label_ink);
    } else {
      PhiDrawColorIcon(dc, icon, row->picture, picture_px, label_ink);
    }
  } else if (checked) {
    if (row->radio) {
      RECT dot = icon;
      ::InflateRect(&dot, -Scale(4, dpi), -Scale(4, dpi));
      PhiFillEllipseAA(dc, dot, disabled ? label_ink : palette.accent);
    } else {
      PhiDrawIconGlyph(dc, icon, fluent::kCheckMark, Scale(12, dpi),
                       disabled ? label_ink : palette.accent);
    }
  } else if (row->mask) {
    const std::vector<unsigned char> bgra =
        PhiTintedIconBgra(*row->mask, label_ink, icon_px);
    PhiBlendBgra(dc, icon, bgra.data(), icon_px, icon_px);
  } else if (!row->icon.empty()) {
    PhiDrawIconGlyph(dc, icon, row->icon, icon_px, label_ink);
  }

  // The label, then the chord. A checked Space row (a picture, no check
  // mark to show it) reads semibold instead.
  RECT text = r;
  text.left = icon.right + Scale(kIconGap, dpi);
  text.right -= Scale(row->submenu ? kArrowRoom : kPad, dpi);
  HGDIOBJ previous = ::SelectObject(
      dc, checked && !row->picture.empty() ? fonts.label_strong : fonts.label);
  ::SetTextColor(dc, label_ink);
  RECT label_rc = text;
  if (!row->shortcut.empty()) {
    label_rc.right -= TextWidth(dc, fonts.shortcut, row->shortcut) +
                      Scale(kShortcutGap, dpi);
  }
  if (row->hint.empty()) {
    ::DrawTextW(dc, row->label.c_str(), -1, &label_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    (hide_prefix ? DT_HIDEPREFIX : 0));
  } else {
    // Two lines (§5.3): the label above the middle, the hint in the
    // tertiary ink below it.
    const int mid = (r.top + r.bottom) / 2;
    RECT top = label_rc;
    top.bottom = mid + Scale(1, dpi);
    ::DrawTextW(dc, row->label.c_str(), -1, &top,
                DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS |
                    (hide_prefix ? DT_HIDEPREFIX : 0));
    ::SelectObject(dc, fonts.hint);
    ::SetTextColor(dc, shortcut_ink);
    RECT bottom = label_rc;
    bottom.top = mid + Scale(1, dpi);
    ::DrawTextW(dc, row->hint.c_str(), -1, &bottom,
                DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
  }
  if (!row->shortcut.empty()) {
    ::SelectObject(dc, fonts.shortcut);
    ::SetTextColor(dc, shortcut_ink);
    ::DrawTextW(dc, row->shortcut.c_str(), -1, &text,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }
  ::SelectObject(dc, previous);
  return true;
}

LRESULT PhiMenuOnMenuChar(HMENU menu, wchar_t ch) {
  if (!menu) {
    return 0;
  }
  const wchar_t want = static_cast<wchar_t>(::towupper(ch));
  const int count = ::GetMenuItemCount(menu);
  for (int i = 0; i < count; ++i) {
    const PhiMenuRow* row = PhiMenuRowAt(menu, i);
    if (!row || row->separator) {
      continue;
    }
    const wchar_t mnemonic = MnemonicOf(row->label);
    if (mnemonic && static_cast<wchar_t>(::towupper(mnemonic)) == want) {
      return MAKELRESULT(i, MNC_EXECUTE);
    }
  }
  return 0;
}

}  // namespace phi_client

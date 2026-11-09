// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_MENU_PAINT_H_
#define PHIBROWSER_WIN_CLIENT_PHI_MENU_PAINT_H_

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_mac_icons.h"
#include "client/phi_theme.h"

// The Fluent flyout look for Win32 popup menus (docs/windows-native-ui.md
// §3.1, §3.5; impl log D6): every row of the app menu and of every context
// menu is an MFT_OWNERDRAW item whose dwItemData points at a PhiMenuRow,
// and the owner window forwards WM_MEASUREITEM / WM_DRAWITEM / WM_MENUCHAR
// here. The menu stays an HMENU — keyboard navigation, mnemonics (through
// WM_MENUCHAR, since owner-drawn items carry no string the system can
// read), submenus, TrackPopupMenu's modal loop and the remote menu.list /
// menu.invoke walk are all the system's; only the pixels are ours: 34 dip
// rows, a 16 dip icon column, the label at 14, the chord right-aligned at
// 12 in the tertiary ink, danger rows in the destructive red, the accent
// check, the flyout ground.

namespace phi_client {

// One row's picture and text. Owned by a PhiMenuRowStore for as long as
// the HMENU item that points at it exists.
struct PhiMenuRow {
  // Guards the cast from dwItemData: other owner-drawn things (buttons)
  // arrive at the same message with their own data.
  unsigned magic = kMagic;
  static constexpr unsigned kMagic = 0x504D4E55;  // "PMNU"
  // Mnemonic '&' allowed ("&&" is a literal '&'); no "\t" chord inside.
  // A USER-SUPPLIED name goes through PhiEscapeMenuText first.
  std::wstring label;
  std::wstring shortcut;  // "Ctrl+T", drawn right-aligned. Empty = none.
  // A second, dim line under the label (the switcher's "profile · N tabs",
  // windows-native-ui.md §5.3). Rows with one are 40 dip.
  std::wstring hint;
  // A glyph of the icon face (one code point) for the icon column.
  std::wstring icon;
  // A Space picture instead: catalog artwork or an emoji, drawn through the
  // icon pipeline. Wins over `icon`.
  std::wstring picture;
  // A template mask (phi_mac_icons.h) instead of `icon`, tinted with the
  // label's ink at the icon column's size: artwork the icon face lacks.
  // The icons are function statics, so a plain pointer never dangles.
  RAW_PTR_EXCLUSION const PhiMacIcon* mask = nullptr;
  bool separator = false;
  bool submenu = false;
  bool danger = false;   // Destructive row: red label and icon.
  bool header = false;   // A section label: tertiary, no wash.
  bool radio = false;    // A checked state draws a dot, not a check mark.
  int height_dip = 34;   // The row's height (separators use their own).
  // The menu's minimum width when this row is in it (the app menu's root
  // asks for 320). Only the widest row's value matters.
  int min_width_dip = 0;
};

// Owns the rows a menu tree points at. A menu that is cleared and rebuilt
// (the Spaces submenu on every open) releases the rows of the items it
// deletes through ClearMenu, so the store never grows past the live tree.
class PhiMenuRowStore {
 public:
  PhiMenuRowStore();
  ~PhiMenuRowStore();
  PhiMenuRowStore(const PhiMenuRowStore&) = delete;
  PhiMenuRowStore& operator=(const PhiMenuRowStore&) = delete;

  // Appends an owner-drawn command row to `menu`. `text` may carry a
  // "\tChord" suffix (the string catalogue's menu strings do), which
  // becomes the row's shortcut.
  PhiMenuRow* AppendCommand(HMENU menu,
                            UINT id,
                            const std::wstring& text,
                            const wchar_t* icon = nullptr,
                            bool enabled = true);
  PhiMenuRow* AppendSubmenu(HMENU menu,
                            HMENU sub,
                            const std::wstring& text,
                            const wchar_t* icon = nullptr,
                            bool enabled = true);
  void AppendSeparator(HMENU menu);
  // Inserts before the item with command id `before` (MF_BYCOMMAND).
  PhiMenuRow* InsertCommandBefore(HMENU menu,
                                  UINT before,
                                  UINT id,
                                  const std::wstring& text,
                                  const wchar_t* icon = nullptr,
                                  bool enabled = true);
  void InsertSeparatorBefore(HMENU menu, UINT before);
  // Deletes every item of `menu` (submenus go with them) and releases
  // their rows.
  void ClearMenu(HMENU menu);
  // Deletes one item by command id and releases its row.
  void DeleteCommand(HMENU menu, UINT id);
  // Rows whose HMENU has been destroyed wholesale (DestroyMenu on the
  // root): drop everything.
  void Clear();

  const PhiMenuRow* Find(HMENU menu, UINT id) const;

 private:
  PhiMenuRow* Adopt(std::unique_ptr<PhiMenuRow> row);
  void Release(const PhiMenuRow* row);
  void ReleaseTree(HMENU menu);

  std::vector<std::unique_ptr<PhiMenuRow>> rows_;
};

// Splits a catalogue menu string into label and chord at its "\t".
void PhiMenuSplitText(const std::wstring& text,
                      std::wstring* label,
                      std::wstring* shortcut);
// A user-supplied name (a bookmark title, a Space or profile name, an
// extension name) as menu text that shows LITERALLY: every '&' doubled so
// "AT&T" is not "AT" with an underlined T that the T key opens, and a tab
// turned into a space so it is not read as a chord. For owner-drawn rows
// (AppendCommand / AppendSubmenu) and plain AppendMenuW items alike; append
// a "\tChord" AFTER escaping.
std::wstring PhiEscapeMenuText(const std::wstring& text);
// The row an item carries, or null when the item is not one of ours.
const PhiMenuRow* PhiMenuRowAt(HMENU menu, int position);
const PhiMenuRow* PhiMenuRowOf(HMENU menu, UINT id);
// An item's label, mnemonic marker stripped: the owner-drawn row's, or the
// system string for a plain item (chord suffix stripped too).
std::wstring PhiMenuLabelAt(HMENU menu, int position);

// The flyout ground behind the rows, applied to `root` and every submenu.
// Called once per show (the palette can change between shows).
void PhiMenuApplyTheme(HMENU root, const PhiPalette& palette);

// The owner's message handlers. Each returns true when the message was one
// of our menu rows (the owner then returns TRUE from the message).
bool PhiMenuOnMeasureItem(HWND owner, MEASUREITEMSTRUCT* measure);
bool PhiMenuOnDrawItem(HWND owner,
                       const DRAWITEMSTRUCT& draw,
                       const PhiPalette& palette);
// WM_MENUCHAR: the mnemonic `ch` against `menu`'s rows. The LRESULT to
// return (MNC_EXECUTE with the item's position), or 0 when no row matches.
LRESULT PhiMenuOnMenuChar(HMENU menu, wchar_t ch);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_MENU_PAINT_H_

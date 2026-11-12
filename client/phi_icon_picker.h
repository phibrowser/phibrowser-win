// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_ICON_PICKER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_ICON_PICKER_H_

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "client/phi_theme.h"

namespace phi_client {

// The Space icon picker (docs/windows-native-ui.md §5.9): one 300 dip
// flyout for the pip's "Change icon…", the app menu's and Settings ›
// Spaces — the Icon | Emoji segmented control, a search box, then a
// 7-column grid of 28 dip cells in sections ("Letters & numbers" and
// "Symbols" on the icon tab; the emoji groups behind a category strip on
// the other). Arrows move, Enter picks, Esc closes; the wheel scrolls the
// grid in place.
//
// A PhiPopover: no message loop. `on_chosen` runs exactly once, after the
// window is gone, with the chosen STORAGE value ("phi:<id>" /
// "emoji:<id>") or an empty string when the picker was dismissed.
using PhiSpaceIconChosen = std::function<void(const std::string& storage)>;

void PhiOpenSpaceIconPicker(HWND owner,
                            const PhiPalette& palette,
                            const RECT& anchor_screen,
                            const std::string& current_storage,
                            PhiSpaceIconChosen on_chosen);

// The synchronous form (the folder picker's pattern): pumps messages under
// a PhiModalScope until the picker is gone and returns the choice, or an
// empty string. The picker owns itself; nothing on this frame is reached
// from inside the loop.
std::string PhiPickSpaceIcon(HWND owner,
                             const PhiPalette& palette,
                             const RECT& anchor_screen,
                             const std::string& current_storage);

// The picker's catalogue, for a surface that embeds the grid instead of
// opening the flyout (Settings › Spaces' editor pane). The flyout reads
// the same lists, so both offer the same icons in the same order.
struct PhiIconChoice {
  std::wstring text;     // What is drawn (a catalog stand-in or an emoji).
  std::string storage;   // "phi:<id>" / "emoji:<id>".
  std::wstring name;     // The catalogue name: searched, and announced.
  std::wstring folded;   // `name` folded by PhiIconSearchFold.
};
struct PhiIconChoiceGroup {
  std::wstring title;  // Empty for the icon tab's one group.
  std::vector<PhiIconChoice> items;
};
// The Icon tab (`emoji` false: one untitled group, the Mac catalog's
// order) or the Emoji tab (one group per catalogue group). Built once.
const std::vector<PhiIconChoiceGroup>& PhiIconChoiceGroups(bool emoji);
// The search's case folding: a query matches a choice whose `folded`
// name contains PhiIconSearchFold(query).
std::wstring PhiIconSearchFold(const std::wstring& text);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_ICON_PICKER_H_

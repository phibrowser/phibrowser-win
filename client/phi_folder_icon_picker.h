// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_FOLDER_ICON_PICKER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_FOLDER_ICON_PICKER_H_

#include <windows.h>

#include <functional>
#include <string>

namespace phi_client {

class PhiShellWindow;

// The Mac's BookmarkFolderIconPicker, shown from a bookmark folder's
// "Change Icon..." menu item: the 24 catalog symbols in a 6-column grid,
// the current one marked, a click choosing.
//
// The picker is a PhiPopover (docs/architecture-review.md §3.6): it does
// NOT run a message loop. `on_chosen` runs exactly once, after the window
// is gone, with the chosen id or an empty string when the picker was
// dismissed (Escape, or focus leaving it).
using PhiFolderIconChosen = std::function<void(const std::string& icon_id)>;

void PhiOpenFolderIconPicker(PhiShellWindow* controller,
                             const RECT& anchor_screen,
                             const std::string& selected,
                             PhiFolderIconChosen on_chosen);

// TRANSITIONAL synchronous wrapper. It drives the asynchronous picker
// above through a short local message loop so the existing call site (in
// phi_shell_window.cc, a file phase 5 does not own) keeps compiling.
// Phase 6 moves that call site onto PhiOpenFolderIconPicker and deletes
// this — it is the last nested GetMessage loop the picker has, and the
// only reason one survives here at all.
std::string PhiPickFolderIcon(PhiShellWindow* controller,
                              const RECT& anchor_screen,
                              const std::string& selected);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_FOLDER_ICON_PICKER_H_

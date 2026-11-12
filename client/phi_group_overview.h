// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_GROUP_OVERVIEW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_GROUP_OVERVIEW_H_

#include <string>

namespace phi_client {

class PhiShellWindow;

// The Mac Group Overview: a full-content grid of one tab group's tabs
// (snapshot cards with favicon/title footers), opened by double-clicking
// the group's header. Click selects a tab, the card's ✕ closes it, the
// trailing "+" card appends a tab to the group, the wheel and the arrow /
// Page / Home / End keys scroll the grid, Escape (or focus loss) leaves the
// overview.
class PhiGroupOverview {
 public:
  static bool IsOpen();
  // The group the open overview shows, empty when none is open — the strip
  // chip of that group wears its selected ring.
  static std::string OpenGroupId();
  static void Open(PhiShellWindow* controller,
                   const std::string& group_id);
  static void Close();
  // The window model of `controller` changed: the open overview re-reads
  // its group's cards when the members moved, and closes once the group is
  // gone. Cheap when nothing changed; called from the shell's row rebuild.
  static void RefreshIfBoundTo(const PhiShellWindow* controller);
  // Close() only if the open overview belongs to `controller` (its shell is
  // being destroyed; called from ~PhiShellWindow).
  static void CloseIfBoundTo(const PhiShellWindow* controller);
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_GROUP_OVERVIEW_H_

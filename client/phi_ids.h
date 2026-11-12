// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_IDS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_IDS_H_

#include <cstddef>
#include <cstdint>
#include <functional>

#include "chrome/browser/phinomenon/phi_framework/phi_ids.h"

namespace phi_client {

// The framework's ids, under the names the client reasons in.
using BrowserId = phi_framework::WindowId;  // one Chromium Browser
using TabId = phi_framework::TabId;
using RequestToken = phi_framework::RequestToken;

// A shell SLOT: one PhiShellWindow, one top-level HWND, N Space
// browsers reparented into it over time. This is the client's own axis —
// the framework has no idea of it (docs/architecture-review.md §3.1
// principle 1).
//
// The numeric value is the id of the browser the slot was opened with, so
// the control-socket dialect keeps its `windowId` numbers and the skill's
// CDP correlation (Browser.getWindowForTarget == the agent window's slot)
// keeps working. What the type takes away is the OTHER direction: a SlotId
// cannot be passed where the framework wants a browser, which is exactly
// the `active_browser_ ? active_browser_ : window_id_` bug family — once
// the opening browser has closed, that number names nothing.
struct SlotId {
  int64_t value = 0;
  constexpr SlotId() = default;
  constexpr explicit SlotId(int64_t v) : value(v) {}
  constexpr bool valid() const { return value > 0; }
  // The slot a browser OPENED; the registry, not this, says which slot a
  // browser is attached to now.
  static constexpr SlotId OpenedBy(BrowserId browser) {
    return SlotId(browser.value);
  }
  friend constexpr bool operator==(SlotId a, SlotId b) {
    return a.value == b.value;
  }
  friend constexpr bool operator!=(SlotId a, SlotId b) {
    return a.value != b.value;
  }
  friend constexpr bool operator<(SlotId a, SlotId b) {
    return a.value < b.value;
  }
};

}  // namespace phi_client

namespace std {
template <>
struct hash<phi_client::SlotId> {
  size_t operator()(phi_client::SlotId id) const noexcept {
    return std::hash<int64_t>()(id.value);
  }
};
}  // namespace std

#endif  // PHIBROWSER_WIN_CLIENT_PHI_IDS_H_

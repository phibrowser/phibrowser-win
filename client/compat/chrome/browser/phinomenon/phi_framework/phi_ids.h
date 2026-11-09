// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PHINOMENON_PHI_FRAMEWORK_PHI_IDS_H_
#define CHROME_BROWSER_PHINOMENON_PHI_FRAMEWORK_PHI_IDS_H_

#include <cstddef>
#include <cstdint>
#include <functional>

// Strong identifier types shared by the framework and the Windows client
// (phibrowser-win docs/architecture-review.md §3.3). One type per address
// space, so a window id can no longer be handed to a method that wanted a
// tab id, and — on the client — a shell SLOT can no longer be mistaken for
// the browser it currently shows. Kept free of Chromium includes so the
// client's host-compiled tests can use it.
//
// Construction is explicit on purpose: `WindowId{n}` at the boundary where
// an int comes in (JSON, the legacy int64_t signatures), `.value` at the
// boundary where one goes out. Phase 1 introduces the types beside the
// legacy signatures; phase 2 migrates the signatures themselves.

namespace phi_framework {

// Browser::session_id().id() — the value CDP's Browser.getWindowForTarget
// reports and every legacy `int64_t window_id` parameter carries.
struct WindowId {
  int64_t value = 0;
  constexpr WindowId() = default;
  constexpr explicit WindowId(int64_t v) : value(v) {}
  constexpr bool valid() const { return value > 0; }
  friend constexpr bool operator==(WindowId a, WindowId b) {
    return a.value == b.value;
  }
  friend constexpr bool operator!=(WindowId a, WindowId b) {
    return a.value != b.value;
  }
  friend constexpr bool operator<(WindowId a, WindowId b) {
    return a.value < b.value;
  }
};

// The SessionID of a tab's WebContents — "id" in ListTabsJson, the value
// the *ById methods take. Chromium's SessionID is 32-bit; the legacy
// signatures spell it as both int and int64_t, this is the one width.
struct TabId {
  int32_t value = 0;
  constexpr TabId() = default;
  constexpr explicit TabId(int32_t v) : value(v) {}
  constexpr bool valid() const { return value > 0; }
  friend constexpr bool operator==(TabId a, TabId b) {
    return a.value == b.value;
  }
  friend constexpr bool operator!=(TabId a, TabId b) {
    return a.value != b.value;
  }
  friend constexpr bool operator<(TabId a, TabId b) {
    return a.value < b.value;
  }
};

// The token RequestBrowser hands back and OnBrowserWindowCreated echoes,
// so a client matches the window it asked for by identity rather than by
// "the next one that arrives". 0 = not request-driven.
struct RequestToken {
  uint64_t value = 0;
  constexpr RequestToken() = default;
  constexpr explicit RequestToken(uint64_t v) : value(v) {}
  constexpr bool valid() const { return value != 0; }
  friend constexpr bool operator==(RequestToken a, RequestToken b) {
    return a.value == b.value;
  }
  friend constexpr bool operator!=(RequestToken a, RequestToken b) {
    return a.value != b.value;
  }
  friend constexpr bool operator<(RequestToken a, RequestToken b) {
    return a.value < b.value;
  }
};

}  // namespace phi_framework

namespace std {
template <>
struct hash<phi_framework::WindowId> {
  size_t operator()(phi_framework::WindowId id) const noexcept {
    return std::hash<int64_t>()(id.value);
  }
};
template <>
struct hash<phi_framework::TabId> {
  size_t operator()(phi_framework::TabId id) const noexcept {
    return std::hash<int32_t>()(id.value);
  }
};
template <>
struct hash<phi_framework::RequestToken> {
  size_t operator()(phi_framework::RequestToken t) const noexcept {
    return std::hash<uint64_t>()(t.value);
  }
};
}  // namespace std

#endif  // CHROME_BROWSER_PHINOMENON_PHI_FRAMEWORK_PHI_IDS_H_

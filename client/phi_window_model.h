// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_WINDOW_MODEL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_WINDOW_MODEL_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "chrome/browser/phinomenon/phi_framework/phi_window_snapshot.h"
#include "client/phi_ids.h"

namespace phi_client {

// The client's ONE copy of framework window state (docs/architecture-
// review.md §3.3 "client side"): the last WindowSnapshot per browser plus
// the indexes every consumer used to rebuild by re-parsing ListTabsJson —
// tab by id, tab by tag, the active tab, which browser holds a tab. Fed by
// PhiFrameworkDelegate::OnWindowChanged (push) and PhiFramework::Snapshot
// (pull, for a browser adopted before its first push). Owned by the
// coordinator; every controller and popover reads it through a const
// pointer and never parses JSON for state again.
//
// Pure: STL + the API structs, no Win32, no base/ — host-tested by
// test/model/window_model_test.cc.
//
// STALENESS: rule 1 of the framework contract means a snapshot reflects
// the last committed strip, never a mid-mutation one. A consumer that
// mutated the strip and needs the result NOW pulls (Refresh) instead of
// waiting for the push.
class PhiWindowModel {
 public:
  using Snapshot = phi_framework::WindowSnapshot;
  using Tab = phi_framework::TabSnapshot;
  using Split = phi_framework::SplitSnapshot;
  using Group = phi_framework::GroupSnapshot;

  PhiWindowModel();
  ~PhiWindowModel();
  PhiWindowModel(const PhiWindowModel&) = delete;
  PhiWindowModel& operator=(const PhiWindowModel&) = delete;

  // Stores a copy; rebuilds the browser's indexes. Returns the reason to
  // repaint with (the caller's, passed through) — kept simple on purpose.
  void Update(const Snapshot& snapshot);
  // Pull: asks the framework for the browser's snapshot and stores it.
  // False when the framework does not know the browser (it was closed).
  bool Refresh(phi_framework::PhiFramework* framework, BrowserId browser);
  // The one pull everybody else goes through: copies the framework's scratch
  // snapshot (api rule 5) into client-owned storage (rule 6) before any
  // other framework call can overwrite it. False for an unknown browser.
  static bool Pull(phi_framework::PhiFramework* framework,
                   BrowserId browser,
                   Snapshot* out);
  void Remove(BrowserId browser);
  bool Has(BrowserId browser) const;

  // --- Reads. Pointers are valid until the next Update/Refresh/Remove for
  // that browser; copy what must outlive a callback.
  const Snapshot* Get(BrowserId browser) const;
  uint64_t Version(BrowserId browser) const;  // 0 = unknown browser.
  const Tab* FindTab(BrowserId browser, TabId tab) const;
  const Tab* FindTabByTag(BrowserId browser, std::string_view tag) const;
  const Tab* FindTabAt(BrowserId browser, int index) const;
  const Tab* ActiveTab(BrowserId browser) const;
  const Split* FindSplit(BrowserId browser, std::string_view split_id) const;
  const Split* SplitOfTab(BrowserId browser, TabId tab) const;
  const Group* FindGroup(BrowserId browser, std::string_view group_id) const;
  // The browser whose strip holds `tab`, across every known browser.
  std::optional<BrowserId> BrowserOfTab(TabId tab) const;
  // Every tab carrying `tag` across every known browser (a stored row's
  // panes can sit in two windows during a move).
  std::vector<std::pair<BrowserId, TabId>> TabsWithTag(
      std::string_view tag) const;
  // Every browser the model knows, in id order.
  std::vector<BrowserId> Browsers() const;

 private:
  struct Entry {
    Snapshot snapshot;
    std::map<TabId, size_t> tab_index;  // id -> position in snapshot.tabs
    std::map<std::string, size_t> tag_index;  // tag -> position (first)
    int active = -1;  // position of the active tab, -1 = none
  };
  const Entry* Find(BrowserId browser) const;
  static void Reindex(Entry* entry);

  std::map<BrowserId, Entry> entries_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_WINDOW_MODEL_H_

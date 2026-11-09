// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef UNSAFE_BUFFERS_BUILD
// The boundary's arrays are plain pointer-and-count pairs by design.
#pragma allow_unsafe_buffers
#endif

#ifndef CHROME_BROWSER_PHINOMENON_PHI_FRAMEWORK_PHI_WINDOW_SNAPSHOT_H_
#define CHROME_BROWSER_PHINOMENON_PHI_FRAMEWORK_PHI_WINDOW_SNAPSHOT_H_

#include <string>
#include <vector>

#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"

// The owned form of the window snapshot. Each side of the boundary builds or
// keeps these with its own standard library; only the *View structs in
// phi_framework_api.h ever cross. See that header for what each field means.
namespace phi_framework {

struct TabSnapshot {
  TabId id;
  int index = -1;
  std::string title;
  std::string url;
  std::string tag;
  std::string split_id;
  std::string group_id;
  bool active = false;
  bool loading = false;
  bool unloaded = false;
  TabSurface surface = TabSurface::kContent;
  TabId origin;
  bool has_page_color = false;
  uint32_t page_color_argb = 0;
};

struct SplitSnapshot {
  std::string id;
  TabId primary;
  TabId secondary;
  SplitLayout layout = SplitLayout::kVertical;
  double ratio = 0.5;
};

struct GroupSnapshot {
  std::string id;
  std::string title;
  TabGroupColor color = TabGroupColor::kGrey;
  bool collapsed = false;
};

struct NavState {
  std::string url;
  std::string title;
  bool can_go_back = false;
  bool can_go_forward = false;
  bool loading = false;
};

struct WindowSnapshot {
  WindowId window;
  uint64_t version = 0;
  std::string profile_id;
  bool incognito = false;
  NavState nav;
  std::vector<TabSnapshot> tabs;
  std::vector<SplitSnapshot> splits;
  std::vector<GroupSnapshot> groups;
};

// A copy of `view` that owns everything it holds.
inline WindowSnapshot ToOwnedSnapshot(const WindowSnapshotView& view) {
  WindowSnapshot owned;
  owned.window = view.window;
  owned.version = view.version;
  owned.profile_id = view.profile_id;
  owned.incognito = view.incognito;
  owned.nav = {view.nav.url, view.nav.title, view.nav.can_go_back,
               view.nav.can_go_forward, view.nav.loading};
  owned.tabs.reserve(view.tab_count);
  for (size_t i = 0; i < view.tab_count; ++i) {
    const TabSnapshotView& tab = view.tabs[i];
    owned.tabs.push_back(
        {tab.id, tab.index, tab.title, tab.url, tab.tag, tab.split_id,
         tab.group_id, tab.active, tab.loading, tab.unloaded, tab.surface,
         tab.origin, tab.has_page_color, tab.page_color_argb});
  }
  owned.splits.reserve(view.split_count);
  for (size_t i = 0; i < view.split_count; ++i) {
    const SplitSnapshotView& split = view.splits[i];
    owned.splits.push_back(
        {split.id, split.primary, split.secondary, split.layout, split.ratio});
  }
  owned.groups.reserve(view.group_count);
  for (size_t i = 0; i < view.group_count; ++i) {
    const GroupSnapshotView& group = view.groups[i];
    owned.groups.push_back(
        {group.id, group.title, group.color, group.collapsed});
  }
  return owned;
}

}  // namespace phi_framework

#endif  // CHROME_BROWSER_PHINOMENON_PHI_FRAMEWORK_PHI_WINDOW_SNAPSHOT_H_

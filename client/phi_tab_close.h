// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_TAB_CLOSE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_TAB_CLOSE_H_

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "chrome/browser/phinomenon/phi_framework/phi_window_snapshot.h"

namespace phi_client {

using phi_framework::GroupSnapshot;
using phi_framework::SplitSnapshot;
using phi_framework::TabId;

// Every pane of `split` in slot order: two to four. A snapshot from before
// the pane list existed carries only the pair.
inline std::vector<TabId> PhiSplitPanes(const SplitSnapshot& split) {
  if (split.tabs.size() >= 2) {
    return split.tabs;
  }
  return {split.primary, split.secondary};
}

// Each pane's share, matching PhiSplitPanes.
inline std::vector<double> PhiSplitFractions(const SplitSnapshot& split) {
  if (split.fractions.size() >= 2 &&
      split.fractions.size() == PhiSplitPanes(split).size()) {
    return split.fractions;
  }
  return {split.ratio, 1.0 - split.ratio};
}
using phi_framework::TabSnapshot;
using phi_framework::TabSurface;
using phi_framework::WindowSnapshot;

// Which tabs a batch close may take (the Mac's BrowserState rules, its
// States/TabOrder/README.md "Native range closing"). Each answer is computed
// from the strip as it is now, in strip order, collapsed group members
// included, so a caller recomputes it at invocation: an anchor that went away
// under a menu closes nothing.

enum class PhiTabCloseDirection { kBefore, kAfter };

// The ordinary tabs: neither bound to a pin or bookmark (the tag) nor a reader
// surface. A split is one unit, so a pane that is not ordinary protects its
// partner too.
inline std::set<int> PhiOrdinaryTabIds(const std::vector<TabSnapshot>& tabs) {
  const auto plain = [](const TabSnapshot& tab) {
    return tab.tag.empty() && tab.surface == TabSurface::kContent;
  };
  std::set<std::string> protected_splits;
  for (const TabSnapshot& tab : tabs) {
    if (!tab.split_id.empty() && !plain(tab)) {
      protected_splits.insert(tab.split_id);
    }
  }
  std::set<int> ordinary;
  for (const TabSnapshot& tab : tabs) {
    if (plain(tab) && !protected_splits.count(tab.split_id)) {
      ordinary.insert(tab.id.value);
    }
  }
  return ordinary;
}

// `ids` with the partner of every split among them.
inline std::set<int> PhiTabCloseUnits(const std::vector<TabSnapshot>& tabs,
                                      const std::set<int>& ids) {
  std::set<std::string> splits;
  for (const TabSnapshot& tab : tabs) {
    if (!tab.split_id.empty() && ids.count(tab.id.value)) {
      splits.insert(tab.split_id);
    }
  }
  std::set<int> units = ids;
  for (const TabSnapshot& tab : tabs) {
    if (splits.count(tab.split_id)) {
      units.insert(tab.id.value);
    }
  }
  return units;
}

// Close Other Tabs: every ordinary tab outside `keeping` and its splits.
inline std::vector<TabId> PhiOtherCloseTargets(
    const std::vector<TabSnapshot>& tabs,
    const std::set<int>& keeping) {
  std::vector<TabId> targets;
  if (keeping.empty()) {
    return targets;
  }
  const std::set<int> ordinary = PhiOrdinaryTabIds(tabs);
  const std::set<int> keep = PhiTabCloseUnits(tabs, keeping);
  for (const TabSnapshot& tab : tabs) {
    if (ordinary.count(tab.id.value) && !keep.count(tab.id.value)) {
      targets.push_back(tab.id);
    }
  }
  return targets;
}

// Close Tabs to the Left / Right (Above / Below): the ordinary tabs before or
// after `anchors`, which name a row's panes or a whole group. An anchor that
// is not ordinary (gone, or bound) answers nothing.
inline std::vector<TabId> PhiDirectionalCloseTargets(
    const std::vector<TabSnapshot>& tabs,
    const std::set<int>& anchors,
    PhiTabCloseDirection direction) {
  std::vector<TabId> targets;
  const std::set<int> ordinary = PhiOrdinaryTabIds(tabs);
  if (anchors.empty()) {
    return targets;
  }
  for (const int id : anchors) {
    if (!ordinary.count(id)) {
      return targets;
    }
  }
  const std::set<int> anchor = PhiTabCloseUnits(tabs, anchors);
  size_t first = tabs.size();
  size_t last = 0;
  for (size_t i = 0; i < tabs.size(); ++i) {
    if (anchor.count(tabs[i].id.value)) {
      first = std::min(first, i);
      last = i;
    }
  }
  if (first == tabs.size()) {
    return targets;
  }
  std::set<int> range;
  for (size_t i = 0; i < tabs.size(); ++i) {
    if (direction == PhiTabCloseDirection::kBefore ? i < first : i > last) {
      range.insert(tabs[i].id.value);
    }
  }
  const std::set<int> closing = PhiTabCloseUnits(tabs, range);
  for (const TabSnapshot& tab : tabs) {
    if (ordinary.count(tab.id.value) && closing.count(tab.id.value) &&
        !anchor.count(tab.id.value)) {
      targets.push_back(tab.id);
    }
  }
  return targets;
}

// Clear All Tabs: the ordinary tabs that are not on screen. The foreground tab
// and its split stay. No active tab answers nothing: a transient focus gap must
// not close the whole Space. A Peek is its own browser, never in this strip.
inline std::vector<TabId> PhiClearAllCandidates(
    const std::vector<TabSnapshot>& tabs) {
  std::vector<TabId> candidates;
  std::set<int> foreground;
  for (const TabSnapshot& tab : tabs) {
    if (tab.active) {
      foreground.insert(tab.id.value);
    }
  }
  if (foreground.empty()) {
    return candidates;
  }
  return PhiOtherCloseTargets(tabs, foreground);
}

// Whether Clear All Tabs has anything to offer: more than one ordinary tab,
// a split counting as one (the Mac's canClearAllTabs, `01dafe08`). A lone
// split has nothing behind it to clear.
inline bool PhiCanClearAllTabs(const std::vector<TabSnapshot>& tabs) {
  const std::set<int> ordinary = PhiOrdinaryTabIds(tabs);
  std::set<std::string> splits;
  int units = 0;
  for (const TabSnapshot& tab : tabs) {
    if (!ordinary.count(tab.id.value)) {
      continue;
    }
    if (tab.split_id.empty() || splits.insert(tab.split_id).second) {
      ++units;
    }
  }
  return units > 1;
}

// One cleanup's record (the Mac's ClearedTabsBatch). Only removals Chromium
// confirms come back: a page's beforeunload can refuse, and that tab is still
// open. Navigation history is not saved.
struct PhiClearedTabsBatch {
  struct Page {
    int id = 0;
    std::string url;
    std::string title;
    std::string group_id;
  };
  uint64_t serial = 0;
  std::vector<Page> pages;
  std::vector<int> order;  // The whole strip when it was cleared.
  std::vector<GroupSnapshot> groups;  // Those a cleared page belonged to.
  std::vector<SplitSnapshot> splits;  // Those with every pane cleared.
  std::set<int> pending;              // Asked to close, not gone yet.
  std::set<int> closed;               // Confirmed gone.
  // On the Undo history: after the first confirmed removal, or at once for
  // a Redo's cleanup (the Mac registers a Redo's inverse while UndoManager
  // is still redoing).
  bool registered = false;
};

// One browser's (one Space's) cleanup history, the Mac's per-BrowserState
// clearTabsUndoManager: Undo takes the newest registered cleanup back, Redo
// runs a fresh cleanup of whatever is behind the foreground then. A new
// cleanup that registers drops the Redo steps, as a new UndoManager action
// does. While a restore's snapshot has not come back, neither another
// cleanup nor an Undo or Redo may overlap it (isRestoringClearedTabs).
struct PhiClearTabsHistory {
  // The Mac's UndoManager keeps every level; this bounds a long session.
  static constexpr size_t kMaxSteps = 50;
  std::vector<PhiClearedTabsBatch> batches;  // Oldest first.
  int redo = 0;
  bool restoring = false;
  uint64_t restoring_deadline = 0;  // GetTickCount64; a lost snapshot's cap.

  bool CanUndo() const {
    return std::any_of(batches.begin(), batches.end(),
                       [](const auto& b) { return b.registered; });
  }
  bool CanRedo() const { return redo > 0; }
  int UndoDepth() const {
    return static_cast<int>(std::count_if(
        batches.begin(), batches.end(),
        [](const auto& b) { return b.registered; }));
  }
  // The newest registered cleanup, or nullptr.
  PhiClearedTabsBatch* Top() {
    for (auto it = batches.rbegin(); it != batches.rend(); ++it) {
      if (it->registered) {
        return &*it;
      }
    }
    return nullptr;
  }
  // Starts a cleanup. An earlier one with no confirmed removal yet never
  // reached the history, and this one supersedes it.
  void Begin(PhiClearedTabsBatch batch) {
    std::erase_if(batches, [](const auto& b) { return !b.registered; });
    batches.push_back(std::move(batch));
    if (batches.size() > kMaxSteps) {
      batches.erase(batches.begin());
    }
  }
  // Moves confirmed removals out of `pending`, given the ids still live.
  // Returns the serial of a cleanup whose FIRST removal this was (its toast
  // goes up then), or 0.
  uint64_t NoteRemovals(const std::set<int>& live) {
    uint64_t first = 0;
    for (PhiClearedTabsBatch& batch : batches) {
      if (batch.pending.empty()) {
        continue;
      }
      const bool none_yet = batch.closed.empty();
      for (auto id = batch.pending.begin(); id != batch.pending.end();) {
        if (live.count(*id)) {
          ++id;
        } else {
          batch.closed.insert(*id);
          id = batch.pending.erase(id);
        }
      }
      if (none_yet && !batch.closed.empty()) {
        if (!batch.registered) {
          batch.registered = true;
          redo = 0;
        }
        first = batch.serial;
      }
    }
    return first;
  }
  // Takes the newest registered cleanup off the history for its restore and
  // makes room for one Redo.
  std::optional<PhiClearedTabsBatch> PopForUndo() {
    for (auto it = batches.rbegin(); it != batches.rend(); ++it) {
      if (it->registered) {
        PhiClearedTabsBatch batch = std::move(*it);
        batches.erase(std::next(it).base());
        ++redo;
        return batch;
      }
    }
    return std::nullopt;
  }
};

inline PhiClearedTabsBatch PhiCaptureClearedTabs(
    const WindowSnapshot& snapshot,
    const std::vector<TabId>& candidates) {
  PhiClearedTabsBatch batch;
  for (const TabId id : candidates) {
    batch.pending.insert(id.value);
  }
  std::set<std::string> group_ids;
  for (const TabSnapshot& tab : snapshot.tabs) {
    batch.order.push_back(tab.id.value);
    if (batch.pending.count(tab.id.value)) {
      batch.pages.push_back({tab.id.value, tab.url, tab.title, tab.group_id});
      group_ids.insert(tab.group_id);
    }
  }
  for (const GroupSnapshot& group : snapshot.groups) {
    if (group_ids.count(group.id)) {
      batch.groups.push_back(group);
    }
  }
  for (const SplitSnapshot& split : snapshot.splits) {
    const std::vector<TabId> panes = PhiSplitPanes(split);
    if (std::all_of(panes.begin(), panes.end(), [&](const TabId pane) {
          return batch.pending.count(pane.value) != 0;
        })) {
      batch.splits.push_back(split);
    }
  }
  return batch;
}

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_TAB_CLOSE_H_

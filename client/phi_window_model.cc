// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_window_model.h"

#include <utility>

namespace phi_client {

PhiWindowModel::PhiWindowModel() = default;
PhiWindowModel::~PhiWindowModel() = default;

void PhiWindowModel::Update(const Snapshot& snapshot) {
  if (!snapshot.window.valid()) {
    return;
  }
  Entry& entry = entries_[snapshot.window];
  entry.snapshot = snapshot;
  Reindex(&entry);
}

// static
bool PhiWindowModel::Pull(phi_framework::PhiFramework* framework,
                          BrowserId browser,
                          Snapshot* out) {
  if (!framework || !out) {
    return false;
  }
  const phi_framework::WindowSnapshotView* view = framework->Snapshot(browser);
  if (!view) {
    return false;
  }
  *out = phi_framework::ToOwnedSnapshot(*view);
  return true;
}

bool PhiWindowModel::Refresh(phi_framework::PhiFramework* framework,
                             BrowserId browser) {
  if (!framework || !browser.valid()) {
    return false;
  }
  Snapshot snapshot;
  if (!Pull(framework, browser, &snapshot)) {
    // The framework no longer knows the browser: whatever we held for it
    // describes a strip that is gone, and a reader that gets `false` here
    // must not find a stale one through Get afterwards.
    entries_.erase(browser);
    return false;
  }
  // The framework fills `window` itself; a pull that came back for another
  // id (it cannot, but the map key must agree with the payload) is stored
  // under the id we asked for.
  snapshot.window = browser;
  Update(snapshot);
  return true;
}

void PhiWindowModel::Remove(BrowserId browser) {
  entries_.erase(browser);
}

bool PhiWindowModel::Has(BrowserId browser) const {
  return entries_.count(browser) > 0;
}

const PhiWindowModel::Snapshot* PhiWindowModel::Get(BrowserId browser) const {
  const Entry* entry = Find(browser);
  return entry ? &entry->snapshot : nullptr;
}

uint64_t PhiWindowModel::Version(BrowserId browser) const {
  const Entry* entry = Find(browser);
  return entry ? entry->snapshot.version : 0;
}

const PhiWindowModel::Tab* PhiWindowModel::FindTab(BrowserId browser,
                                                   TabId tab) const {
  const Entry* entry = Find(browser);
  if (!entry || !tab.valid()) {
    return nullptr;
  }
  const auto it = entry->tab_index.find(tab);
  return it == entry->tab_index.end() ? nullptr
                                      : &entry->snapshot.tabs[it->second];
}

const PhiWindowModel::Tab* PhiWindowModel::FindTabByTag(
    BrowserId browser,
    std::string_view tag) const {
  const Entry* entry = Find(browser);
  if (!entry || tag.empty()) {
    return nullptr;
  }
  const auto it = entry->tag_index.find(std::string(tag));
  return it == entry->tag_index.end() ? nullptr
                                      : &entry->snapshot.tabs[it->second];
}

const PhiWindowModel::Tab* PhiWindowModel::FindTabAt(BrowserId browser,
                                                     int index) const {
  const Entry* entry = Find(browser);
  if (!entry || index < 0) {
    return nullptr;
  }
  // By the tab's own `index` field, not its vector position: the two agree
  // for a well-formed snapshot, but the field is what the wire's callers
  // mean by "index".
  for (const Tab& tab : entry->snapshot.tabs) {
    if (tab.index == index) {
      return &tab;
    }
  }
  return nullptr;
}

const PhiWindowModel::Tab* PhiWindowModel::ActiveTab(BrowserId browser) const {
  const Entry* entry = Find(browser);
  if (!entry || entry->active < 0) {
    return nullptr;
  }
  return &entry->snapshot.tabs[static_cast<size_t>(entry->active)];
}

const PhiWindowModel::Split* PhiWindowModel::FindSplit(
    BrowserId browser,
    std::string_view split_id) const {
  const Entry* entry = Find(browser);
  if (!entry || split_id.empty()) {
    return nullptr;
  }
  for (const Split& split : entry->snapshot.splits) {
    if (split.id == split_id) {
      return &split;
    }
  }
  return nullptr;
}

const PhiWindowModel::Split* PhiWindowModel::SplitOfTab(BrowserId browser,
                                                        TabId tab) const {
  const Tab* found = FindTab(browser, tab);
  if (!found || found->split_id.empty()) {
    return nullptr;
  }
  return FindSplit(browser, found->split_id);
}

const PhiWindowModel::Group* PhiWindowModel::FindGroup(
    BrowserId browser,
    std::string_view group_id) const {
  const Entry* entry = Find(browser);
  if (!entry || group_id.empty()) {
    return nullptr;
  }
  for (const Group& group : entry->snapshot.groups) {
    if (group.id == group_id) {
      return &group;
    }
  }
  return nullptr;
}

std::optional<BrowserId> PhiWindowModel::BrowserOfTab(TabId tab) const {
  if (!tab.valid()) {
    return std::nullopt;
  }
  for (const auto& [browser, entry] : entries_) {
    if (entry.tab_index.count(tab)) {
      return browser;
    }
  }
  return std::nullopt;
}

std::vector<std::pair<BrowserId, TabId>> PhiWindowModel::TabsWithTag(
    std::string_view tag) const {
  std::vector<std::pair<BrowserId, TabId>> out;
  if (tag.empty()) {
    return out;
  }
  // Every tab, not the tag index: the index keeps the FIRST holder per
  // browser, and this is the call that finds the duplicates a restore can
  // bring back.
  for (const auto& [browser, entry] : entries_) {
    for (const Tab& t : entry.snapshot.tabs) {
      if (t.tag == tag) {
        out.emplace_back(browser, t.id);
      }
    }
  }
  return out;
}

std::vector<BrowserId> PhiWindowModel::Browsers() const {
  std::vector<BrowserId> out;
  out.reserve(entries_.size());
  for (const auto& [browser, entry] : entries_) {
    out.push_back(browser);
  }
  return out;
}

const PhiWindowModel::Entry* PhiWindowModel::Find(BrowserId browser) const {
  const auto it = entries_.find(browser);
  return it == entries_.end() ? nullptr : &it->second;
}

// static
void PhiWindowModel::Reindex(Entry* entry) {
  entry->tab_index.clear();
  entry->tag_index.clear();
  entry->active = -1;
  const std::vector<Tab>& tabs = entry->snapshot.tabs;
  for (size_t i = 0; i < tabs.size(); ++i) {
    const Tab& tab = tabs[i];
    if (tab.id.valid()) {
      entry->tab_index.emplace(tab.id, i);  // First wins on a duplicate id.
    }
    if (!tab.tag.empty()) {
      entry->tag_index.emplace(tab.tag, i);
    }
    if (tab.active && entry->active < 0) {
      entry->active = static_cast<int>(i);
    }
  }
}

}  // namespace phi_client

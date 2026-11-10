// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phi_sidebar_model.h"

#include <algorithm>
#include <set>

namespace phi_client {

// ---- Tag -------------------------------------------------------------------

std::string TagFor(const std::string& guid, bool pane_b) {
  if (guid.empty()) return {};
  return pane_b ? guid + kSidebarPaneBSuffix : guid;
}

std::string TagForPane(const std::string& guid, int pane) {
  if (guid.empty()) return {};
  if (pane <= 0 || pane >= kSidebarMaxPanes) return guid;
  return guid + '#' + static_cast<char>('a' + pane);
}

bool ParseTag(const std::string& tag, SidebarTag* out) {
  if (tag.empty()) return false;
  out->guid = tag;
  out->pane = 0;
  // "#b", "#c" or "#d" at the very end.
  if (tag.size() >= 2 && tag[tag.size() - 2] == '#') {
    const int pane = tag.back() - 'a';
    if (pane >= 1 && pane < kSidebarMaxPanes) {
      out->guid = tag.substr(0, tag.size() - 2);
      out->pane = pane;
    }
  }
  out->pane_b = out->pane == 1;
  return !out->guid.empty();
}

// ---- Keys ------------------------------------------------------------------

SidebarKey SidebarKey::Tab(int session_id) {
  return {Kind::kTab, std::to_string(session_id)};
}
SidebarKey SidebarKey::Split(const std::string& split_id) {
  return {Kind::kSplit, split_id};
}
SidebarKey SidebarKey::Group(const std::string& group_id) {
  return {Kind::kGroup, group_id};
}
SidebarKey SidebarKey::Cell(const std::string& guid) {
  return {Kind::kCell, guid};
}
SidebarKey SidebarKey::Folder(const std::string& guid) {
  return {Kind::kFolder, guid};
}
SidebarKey SidebarKey::Space(const std::string& space_id) {
  return {Kind::kSpace, space_id};
}

int SidebarKey::tab_id() const {
  if (kind != Kind::kTab) return 0;
  return std::stoi(id);
}

std::string SidebarKey::ToString() const {
  switch (kind) {
    case Kind::kNone: return "none";
    case Kind::kTab: return "tab:" + id;
    case Kind::kSplit: return "split:" + id;
    case Kind::kGroup: return "group:" + id;
    case Kind::kCell: return "cell:" + id;
    case Kind::kFolder: return "folder:" + id;
    case Kind::kSpace: return "space:" + id;
  }
  return "none";
}

// ---- Outputs ---------------------------------------------------------------

std::vector<int> SidebarBinding::TabIds() const {
  std::vector<int> ids;
  if (tab_a) ids.push_back(tab_a);
  if (tab_b && tab_b != tab_a) ids.push_back(tab_b);
  for (int id : more) {
    if (id && std::find(ids.begin(), ids.end(), id) == ids.end()) {
      ids.push_back(id);
    }
  }
  return ids;
}

std::vector<int> SidebarContentNode::Panes() const {
  std::vector<int> ids;
  if (tab_a) ids.push_back(tab_a);
  if (tab_b) ids.push_back(tab_b);
  ids.insert(ids.end(), more.begin(), more.end());
  return ids;
}

const SidebarCell* SidebarTree::CellFor(const std::string& guid) const {
  for (const SidebarCell& cell : pins) {
    if (cell.guid == guid) return &cell;
  }
  for (const SidebarCell& cell : bookmarks) {
    if (cell.guid == guid) return &cell;
  }
  return nullptr;
}

const SidebarBinding* SidebarTree::BindingFor(const std::string& guid) const {
  const SidebarCell* cell = CellFor(guid);
  return cell ? &cell->binding : nullptr;
}

int SidebarTree::IndexOf(int tab_id) const {
  const auto it = indices.find(tab_id);
  return it == indices.end() ? -1 : it->second;
}

// ---- BuildSidebar ----------------------------------------------------------

namespace {

SidebarSurface SurfaceFor(const std::map<int, SidebarSurface>& surfaces,
                          const std::vector<int>& panes) {
  for (int id : panes) {
    if (id == 0) continue;
    const auto it = surfaces.find(id);
    if (it != surfaces.end() && it->second != SidebarSurface::kContent) {
      return it->second;
    }
  }
  return SidebarSurface::kContent;
}

}  // namespace

SidebarTree BuildSidebar(const SidebarInputs& in) {
  SidebarTree out;

  // 1. The strip: content tabs in order, one per id. Surfaces are never
  //    rows; an active one hands the selection to its origin.
  std::vector<const SidebarLiveTab*> tabs;
  for (const SidebarLiveTab& tab : in.strip) {
    if (tab.id > 0 && tab.index >= 0) tabs.push_back(&tab);
  }
  std::stable_sort(tabs.begin(), tabs.end(), [](const auto* a, const auto* b) {
    return a->index < b->index;
  });
  {
    std::set<int> seen;
    std::erase_if(tabs, [&](const auto* tab) { return !seen.insert(tab->id).second; });
  }
  int surface_origin = 0;
  std::map<int, const SidebarLiveTab*> by_id;
  std::map<std::string, const SidebarLiveTab*> by_tag;  // First in strip order.
  std::map<std::string, std::vector<const SidebarLiveTab*>> splits;
  for (const auto* tab : tabs) {
    if (tab->surface != SidebarSurface::kContent) {
      if (tab->active) surface_origin = tab->origin_id;
      continue;
    }
    by_id.emplace(tab->id, tab);
    out.indices.emplace(tab->id, tab->index);
    if (!tab->tag.empty()) by_tag.emplace(tab->tag, tab);
    if (!tab->split_id.empty()) splits[tab->split_id].push_back(tab);
    if (tab->active) out.active_tab = tab->id;
  }
  if (surface_origin && by_id.count(surface_origin)) {
    out.active_tab = surface_origin;
  }
  // Live split partners, by split identity — never by adjacency. A split
  // straddling a group boundary is not merged. `mates_of` holds every pane
  // of a tab's split (two to four, strip order, itself included);
  // `partner_of` only the other pane of a pair, which is what a stored
  // split cell binds to.
  std::map<int, const SidebarLiveTab*> partner_of;
  std::map<int, const std::vector<const SidebarLiveTab*>*> mates_of;
  for (const auto& [id, panes] : splits) {
    if (panes.size() < 2 || panes.size() > 4 ||
        !std::all_of(panes.begin(), panes.end(), [&](const auto* pane) {
          return pane->group_id == panes[0]->group_id;
        })) {
      continue;
    }
    for (const auto* pane : panes) mates_of.emplace(pane->id, &panes);
    if (panes.size() == 2) {
      partner_of.emplace(panes[0]->id, panes[1]);
      partner_of.emplace(panes[1]->id, panes[0]);
    }
  }
  const auto tagged = [&](const std::string& tag) -> const SidebarLiveTab* {
    const auto it = by_tag.find(tag);
    return it == by_tag.end() ? nullptr : it->second;
  };
  const auto partner = [&](const SidebarLiveTab* tab) -> const SidebarLiveTab* {
    if (!tab) return nullptr;
    const auto it = partner_of.find(tab->id);
    return it == partner_of.end() ? nullptr : it->second;
  };

  // 2. Stored cells and their bindings. A live pane has at most one owner.
  std::map<std::string, const SidebarStoredItem*> records;
  if (!in.policy.incognito) {
    for (const SidebarStoredItem& item : in.stored) {
      if (!item.guid.empty()) records.emplace(item.guid, &item);
    }
  }
  std::map<std::string, SidebarBinding> bindings;
  std::set<int> claimed;
  const auto claim = [&](const std::string& guid, SidebarBinding binding) {
    for (int id : binding.TabIds()) {
      if (claimed.count(id)) return;  // Never give a pane two owners.
    }
    for (int id : binding.TabIds()) {
      claimed.insert(id);
      binding.active |= id == out.active_tab;
    }
    bindings[guid] = std::move(binding);
  };
  for (const auto& [guid, item] : records) {
    if (item->kind == SidebarStoredKind::kFolder) continue;
    const SidebarLiveTab* a = tagged(TagFor(guid, false));
    if (item->split && item->extra > 0) {
      // Three or four panes: open as a split only when every pane is live
      // and they form one split of exactly that many panes. Otherwise each
      // pane that is not in some other split stays recoverable, as below.
      std::vector<const SidebarLiveTab*> panes = {a, tagged(TagFor(guid, true))};
      for (int pane = 2; pane < 2 + item->extra && pane < kSidebarMaxPanes;
           ++pane) {
        panes.push_back(tagged(TagForPane(guid, pane)));
      }
      const bool all_live =
          std::none_of(panes.begin(), panes.end(),
                       [](const SidebarLiveTab* pane) { return !pane; });
      const auto mates = all_live ? mates_of.find(a->id) : mates_of.end();
      const bool together =
          mates != mates_of.end() && mates->second->size() == panes.size() &&
          std::all_of(panes.begin(), panes.end(),
                      [&](const SidebarLiveTab* pane) {
                        return pane->split_id == a->split_id;
                      });
      SidebarBinding binding;
      const auto id_of = [&](const SidebarLiveTab* pane) {
        return pane && (together || pane->split_id.empty()) ? pane->id : 0;
      };
      binding.tab_a = id_of(panes[0]);
      binding.tab_b = id_of(panes[1]);
      for (size_t i = 2; i < panes.size(); ++i) {
        binding.more.push_back(id_of(panes[i]));
      }
      if (together) {
        binding.state = SidebarBindingState::kSplitOpen;
        binding.split_id = a->split_id;
      } else if (binding.open()) {
        binding.state = SidebarBindingState::kPartial;
      }
      claim(guid, binding);
    } else if (item->split) {
      const SidebarLiveTab* b = tagged(TagFor(guid, true));
      SidebarBinding binding;
      if (a && b && partner(a) == b) {
        binding = {SidebarBindingState::kSplitOpen, a->id, b->id, {}, a->split_id};
      } else {
        // A closed/missing pane is recoverable. A pane in an unrelated
        // split is NOT: that whole split stays in the normal section.
        binding.tab_a = a && a->split_id.empty() ? a->id : 0;
        binding.tab_b = b && b->split_id.empty() ? b->id : 0;
        if (binding.open()) binding.state = SidebarBindingState::kPartial;
      }
      claim(guid, binding);
    } else if (a && a->split_id.empty()) {
      claim(guid, {SidebarBindingState::kOpen, a->id, 0, {}, {}});
    }
  }
  // Which claims hide their tab from the strip (policy).
  for (const auto& [guid, binding] : bindings) {
    const bool hides = !in.policy.horizontal_layout ||
                       records.at(guid)->kind == SidebarStoredKind::kPin;
    if (!hides) continue;
    const SidebarKey key = SidebarKey::Cell(guid);
    for (int id : binding.TabIds()) out.owner_of.emplace(id, key);
  }

  // 3. Cells: pins in store order; bookmarks depth-first by sibling index.
  const auto make_cell = [&](const SidebarStoredItem& item, int depth) {
    SidebarCell cell;
    cell.guid = item.guid;
    cell.kind = item.kind;
    cell.parent_guid = item.parent_guid;
    cell.depth = depth;
    cell.split = item.split;
    cell.extra = item.split ? item.extra : 0;
    cell.key = item.kind == SidebarStoredKind::kFolder
                   ? SidebarKey::Folder(item.guid)
                   : SidebarKey::Cell(item.guid);
    const auto b = bindings.find(item.guid);
    if (b != bindings.end()) {
      cell.binding = b->second;
      cell.attached_surface = SurfaceFor(in.surfaces, cell.binding.TabIds());
    }
    return cell;
  };
  std::vector<const SidebarStoredItem*> pins, leaves;
  for (const auto& [guid, item] : records) {
    (item->kind == SidebarStoredKind::kPin ? pins : leaves).push_back(item);
  }
  const auto by_index = [](const SidebarStoredItem* x,
                           const SidebarStoredItem* y) {
    return x->index != y->index ? x->index < y->index : x->guid < y->guid;
  };
  std::sort(pins.begin(), pins.end(), by_index);
  for (const auto* item : pins) out.pins.push_back(make_cell(*item, 0));
  std::map<std::string, std::vector<const SidebarStoredItem*>> children;
  for (const auto* item : leaves) children[item->parent_guid].push_back(item);
  for (auto& [parent, kids] : children) {
    std::sort(kids.begin(), kids.end(), by_index);
  }
  std::set<std::string> visited;
  const auto descend = [&](auto&& self, const std::string& parent,
                           int depth) -> void {
    const auto it = children.find(parent);
    if (it == children.end()) return;
    for (const auto* item : it->second) {
      if (!visited.insert(item->guid).second) continue;  // Cycle guard.
      out.bookmarks.push_back(make_cell(*item, depth));
      if (item->kind == SidebarStoredKind::kFolder) {
        self(self, item->guid, depth + 1);
      }
    }
  };
  descend(descend, std::string(), 0);
  // Orphans (parent missing) still exist: surface them at the root rather
  // than lose them.
  for (const auto* item : leaves) {
    if (!visited.count(item->guid)) {
      visited.insert(item->guid);
      out.bookmarks.push_back(make_cell(*item, 0));
      if (item->kind == SidebarStoredKind::kFolder) {
        descend(descend, item->guid, 1);
      }
    }
  }
  // A collapsed folder still owns its descendants: carry active state up.
  for (SidebarCell& cell : out.bookmarks) {
    if (!cell.binding.active) continue;
    std::set<std::string> seen;
    std::string parent = cell.parent_guid;
    while (!parent.empty() && seen.insert(parent).second) {
      const auto rec = records.find(parent);
      if (rec == records.end()) break;
      for (SidebarCell& folder : out.bookmarks) {
        if (folder.guid == parent) folder.contains_active = true;
      }
      parent = rec->second->parent_guid;
    }
  }

  // 4. Content: strip order, a header before a group's first member.
  std::map<std::string, const SidebarLiveGroup*> groups;
  for (const SidebarLiveGroup& group : in.groups) {
    if (!group.id.empty()) groups.emplace(group.id, &group);
  }
  std::set<std::string> headers_emitted;
  std::set<int> emitted;
  for (const auto* tab : tabs) {
    if (!by_id.count(tab->id) || out.owner_of.count(tab->id) ||
        emitted.count(tab->id)) {
      continue;
    }
    std::string group_id = tab->group_id;
    const SidebarLiveGroup* group = nullptr;
    if (!group_id.empty()) {
      const auto g = groups.find(group_id);
      if (g == groups.end()) {
        group_id.clear();  // Joined before created: render flat for now.
      } else {
        group = g->second;
        if (headers_emitted.insert(group_id).second) {
          SidebarContentNode header;
          header.key = SidebarKey::Group(group_id);
          header.kind = SidebarNodeKind::kGroup;
          header.group_id = group_id;
          header.collapsed = group->collapsed;
          out.content.push_back(std::move(header));
        }
      }
    }
    // The whole split merges into one row, or none of it does.
    const auto mates = mates_of.find(tab->id);
    const bool merged =
        mates != mates_of.end() &&
        std::all_of(mates->second->begin(), mates->second->end(),
                    [&](const auto* pane) {
                      return pane == tab || (!out.owner_of.count(pane->id) &&
                                             !emitted.count(pane->id));
                    });
    SidebarContentNode node;
    node.group_id = group_id;
    node.hidden = group && group->collapsed;
    if (merged) {
      const auto& panes = *mates->second;
      node.key = SidebarKey::Split(tab->split_id);
      node.kind = SidebarNodeKind::kSplit;
      node.tab_a = panes[0]->id;
      node.tab_b = panes[1]->id;
      for (size_t i = 2; i < panes.size(); ++i) node.more.push_back(panes[i]->id);
      for (const auto* pane : panes) emitted.insert(pane->id);
    } else {
      node.key = SidebarKey::Tab(tab->id);
      node.kind = SidebarNodeKind::kTab;
      node.tab_a = tab->id;
    }
    emitted.insert(tab->id);
    const std::vector<int> node_panes = node.Panes();
    node.active = out.active_tab != 0 &&
                  std::find(node_panes.begin(), node_panes.end(),
                            out.active_tab) != node_panes.end();
    node.attached_surface = SurfaceFor(in.surfaces, node_panes);
    if (node.active && !group_id.empty()) {
      for (SidebarContentNode& n : out.content) {
        if (n.kind == SidebarNodeKind::kGroup && n.group_id == group_id) {
          n.contains_active = true;
        }
      }
    }
    out.content.push_back(std::move(node));
  }

  // 5. The row that highlights.
  if (out.active_tab) {
    const auto owner = out.owner_of.find(out.active_tab);
    if (owner != out.owner_of.end()) {
      out.active_key = owner->second;
    } else {
      for (const SidebarContentNode& node : out.content) {
        if (node.active) {
          out.active_key = node.key;
          break;
        }
      }
    }
  }
  return out;
}

}  // namespace phi_client

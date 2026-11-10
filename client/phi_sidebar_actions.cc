// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phi_sidebar_actions.h"

#include <limits>
#include <vector>

namespace phi_client {

namespace {

// The cells a destination parent holds, in tree order.
std::vector<const SidebarCell*> SiblingsOf(const SidebarTree& tree,
                                           const SidebarPlace& place) {
  std::vector<const SidebarCell*> cells;
  if (place.section == SidebarPlace::Section::kPinned) {
    for (const SidebarCell& cell : tree.pins) cells.push_back(&cell);
  } else if (place.section == SidebarPlace::Section::kBookmarks) {
    for (const SidebarCell& cell : tree.bookmarks) {
      if (cell.parent_guid == place.parent_guid) cells.push_back(&cell);
    }
  }
  return cells;
}

const SidebarContentNode* NodeFor(const SidebarTree& tree,
                                  const SidebarKey& key) {
  for (const SidebarContentNode& node : tree.content) {
    if (node.key == key) return &node;
  }
  return nullptr;
}

}  // namespace

int PlaceSiblingIndex(const SidebarTree& tree,
                      const SidebarPlace& place,
                      const std::string& moving_guid) {
  const std::vector<const SidebarCell*> siblings = SiblingsOf(tree, place);
  if (place.before.empty()) return std::numeric_limits<int>::max();
  int index = 0;
  for (const SidebarCell* cell : siblings) {
    if (cell->key == place.before) return index;
    if (cell->guid != moving_guid) ++index;
  }
  return std::numeric_limits<int>::max();
}

int TabOfKey(const SidebarTree& tree, const SidebarKey& key, int pane) {
  switch (key.kind) {
    case SidebarKey::Kind::kTab:
      return pane == 0 ? key.tab_id() : 0;
    case SidebarKey::Kind::kSplit: {
      const SidebarContentNode* node = NodeFor(tree, key);
      if (!node) return 0;
      const std::vector<int> panes = node->Panes();
      return pane >= 0 && pane < static_cast<int>(panes.size())
                 ? panes[static_cast<size_t>(pane)]
                 : 0;
    }
    case SidebarKey::Kind::kCell: {
      const SidebarCell* cell = tree.CellFor(key.id);
      if (!cell) return 0;
      return cell->binding.TabOfPane(pane);
    }
    case SidebarKey::Kind::kGroup:
    case SidebarKey::Kind::kFolder:
    case SidebarKey::Kind::kSpace:
    case SidebarKey::Kind::kNone:
      return 0;
  }
  return 0;
}

int PlaceAnchorTab(const SidebarTree& tree, const SidebarPlace& place) {
  if (place.section != SidebarPlace::Section::kTabs || place.before.empty()) {
    return 0;
  }
  return TabOfKey(tree, place.before, 0);
}

bool MoveIsNoop(const SidebarTree& tree,
                const SidebarKey& key,
                const SidebarPlace& place) {
  if (place.before == key) return true;  // Anchored on itself.
  if (place.section == SidebarPlace::Section::kTabs) {
    if (key.kind != SidebarKey::Kind::kTab && key.kind != SidebarKey::Kind::kSplit) {
      return false;  // A cell entering the list is always a change.
    }
    // The visible content rows in order; a move is a no-op when the anchor
    // is the row right after the key (or both are last), the group agrees.
    const SidebarContentNode* prev = nullptr;
    bool key_seen = false;
    for (const SidebarContentNode& node : tree.content) {
      if (node.kind == SidebarNodeKind::kGroup) continue;
      if (key_seen) {
        if (place.group_explicit && place.group_id != node.group_id) return false;
        return node.key == place.before;
      }
      if (node.key == key) {
        key_seen = true;
        if (place.group_explicit && place.group_id != node.group_id) return false;
        continue;
      }
      prev = &node;
    }
    (void)prev;
    return key_seen && place.before.empty();  // Already last, moving to end.
  }
  // Stored sections: the key must already be a cell of that parent, and the
  // anchor must be the cell right after it (or both at the end).
  if (key.kind != SidebarKey::Kind::kCell && key.kind != SidebarKey::Kind::kFolder) {
    return false;
  }
  const SidebarCell* cell = tree.CellFor(key.id);
  if (!cell) return false;
  const bool in_section =
      place.section == SidebarPlace::Section::kPinned
          ? cell->kind == SidebarStoredKind::kPin
          : cell->kind != SidebarStoredKind::kPin &&
                cell->parent_guid == place.parent_guid;
  if (!in_section) return false;
  const std::vector<const SidebarCell*> siblings = SiblingsOf(tree, place);
  for (size_t i = 0; i < siblings.size(); ++i) {
    if (siblings[i]->guid != cell->guid) continue;
    if (i + 1 < siblings.size()) return siblings[i + 1]->key == place.before;
    return place.before.empty();
  }
  return false;
}

}  // namespace phi_client

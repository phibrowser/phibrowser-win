// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_shell_menus.h"

#include "phibrowser-win/client/phi_features.h"

#include <utility>

#include "phibrowser-win/client/phi_i18n.h"

#if defined(_WIN32)
#include "phibrowser-win/client/phi_fluent.h"
#include "phibrowser-win/client/phi_menu_paint.h"
#include "phibrowser-win/client/phi_modal_scope.h"
#endif

namespace phi_client {

// --- The item helpers ------------------------------------------------------

PhiMenuItem PhiMenuItem::Separator() {
  PhiMenuItem item;
  item.separator = true;
  return item;
}

PhiMenuItem PhiMenuItem::Command(std::string key, std::wstring label) {
  PhiMenuItem item;
  item.key = std::move(key);
  item.label = std::move(label);
  return item;
}

PhiMenuItem PhiMenuItem::Submenu(std::wstring label,
                                 std::vector<PhiMenuItem> items) {
  PhiMenuItem item;
  item.label = std::move(label);
  item.submenu = true;
  item.items = std::move(items);
  return item;
}

namespace {

const PhiMenuItem* FindIn(const std::vector<PhiMenuItem>& items,
                          std::string_view key) {
  for (const PhiMenuItem& item : items) {
    if (!item.key.empty() && item.key == key) {
      return &item;
    }
    if (const PhiMenuItem* found = FindIn(item.items, key)) {
      return found;
    }
  }
  return nullptr;
}

void KeysIn(const std::vector<PhiMenuItem>& items,
            std::vector<std::string>* out) {
  for (const PhiMenuItem& item : items) {
    out->push_back(item.separator ? "-" : item.key);
    KeysIn(item.items, out);
  }
}

// A dynamic row, one per choice, all sharing a key.
std::vector<PhiMenuItem> ChoiceRows(const std::vector<PhiMenuChoice>& choices,
                                    const char* key) {
  std::vector<PhiMenuItem> rows;
  rows.reserve(choices.size());
  for (const PhiMenuChoice& choice : choices) {
    PhiMenuItem item = PhiMenuItem::Command(key, choice.label);
    item.target.id = choice.id;
    item.glyph = choice.glyph;
    item.hint = choice.hint;
    item.shortcut = choice.shortcut;
    rows.push_back(std::move(item));
  }
  return rows;
}

// An item that persists to the Space store: greyed off the record in an
// incognito shell, exactly as the old `persist_flags` did.
PhiMenuItem PersistCommand(const char* key,
                           std::wstring label,
                           bool incognito) {
  PhiMenuItem item = PhiMenuItem::Command(key, std::move(label));
  item.enabled = !incognito;
  return item;
}

}  // namespace

const PhiMenuItem* PhiMenuSpec::Find(std::string_view key) const {
  return FindIn(items, key);
}

std::vector<std::string> PhiMenuSpec::Keys() const {
  std::vector<std::string> keys;
  KeysIn(items, &keys);
  return keys;
}

// --- Group header ----------------------------------------------------------

PhiMenuSpec PhiBuildGroupHeaderMenu(
    const std::vector<PhiMenuColorOption>& colors) {
  std::vector<PhiMenuItem> swatches;
  swatches.reserve(colors.size());
  for (const PhiMenuColorOption& color : colors) {
    PhiMenuItem item = PhiMenuItem::Command(
        "group.color",
        PhiStr((std::string("tabs.tabGroup.colorName.") + color.name).c_str()));
    item.target.id = color.name;
    item.checked = color.checked;
    swatches.push_back(std::move(item));
  }
  PhiMenuSpec spec;
  // The Mac's "New tab in group" leads (windows-native-ui.md §3.5).
  spec.items.push_back(
      PhiMenuItem::Command("group.newTab", PhiStr("contextMenu.newTabInGroup")));
  spec.items.push_back(
      PhiMenuItem::Command("group.rename", PhiStr("contextMenu.renameGroup")));
  spec.items.push_back(
      PhiMenuItem::Submenu(PhiStr("contextMenu.changeColor"), std::move(swatches)));
  spec.items.push_back(PhiMenuItem::Separator());
  spec.items.push_back(PhiMenuItem::Command("group.ungroup", PhiStr("contextMenu.ungroup")));
  spec.items.push_back(PhiMenuItem::Command("group.close", PhiStr("contextMenu.closeGroup")));
  return spec;
}

// --- Tab row ---------------------------------------------------------------

namespace {

// The Mac's group block: an ungrouped row may start a group or join one that
// exists; a grouped row may move to another, to a new one, or leave. A
// merged split row takes the same block — the pair moves as one.
void AppendGroupBlock(const PhiTabMenuInputs& in,
                      std::vector<PhiMenuItem>& items) {
  if (!in.grouped) {
    items.push_back(PhiMenuItem::Command("tab.newGroup", PhiStr("contextMenu.newTabGroup")));
    if (!in.groups.empty()) {
      items.push_back(PhiMenuItem::Submenu(
          PhiStr("contextMenu.addToGroup"), ChoiceRows(in.groups, "tab.joinGroup")));
    }
  } else {
    if (!in.groups.empty()) {
      items.push_back(PhiMenuItem::Submenu(
          PhiStr("contextMenu.moveToGroup"), ChoiceRows(in.groups, "tab.joinGroup")));
    }
    items.push_back(
        PhiMenuItem::Command("tab.moveNewGroup", PhiStr("contextMenu.moveToNewGroup")));
    items.push_back(
        PhiMenuItem::Command("tab.leaveGroup", PhiStr("contextMenu.removeFromGroup")));
  }
}

}  // namespace

PhiMenuSpec PhiBuildTabRowMenu(const PhiTabMenuInputs& in) {
  PhiMenuSpec spec;
  std::vector<PhiMenuItem>& items = spec.items;
  // A tab an agent is holding leads with the takeover: it is the action the
  // badge is asking about.
  if (in.agent_driven) {
    items.push_back(PhiMenuItem::Command(
        in.agent_reclaimed ? "tab.handBack" : "tab.takeControl",
        in.agent_reclaimed ? PhiStr("contextMenu.handBackToAgent") : PhiStr("contextMenu.takeControl")));
    items.push_back(PhiMenuItem::Separator());
  }
  if (!in.merged) {
    items.push_back(PersistCommand("tab.pin", PhiStr("contextMenu.pin"), in.incognito));
    items.push_back(PhiMenuItem::Command("tab.duplicate", PhiStr("contextMenu.duplicate")));
    items.push_back(PhiMenuItem::Command("tab.copyUrl", PhiStr("contextMenu.copyLink")));
    items.push_back(PhiMenuItem::Separator());
    if (!in.split_bound) {
      // "Split with Current" pairs this row with the FOCUSED tab; on the
      // focused row itself — or when the focused tab is already split — it
      // becomes "Open as Split", a fresh pane (the Mac's two labels).
      items.push_back(PhiMenuItem::Command(
          in.current_can_pair ? "tab.splitCurrent" : "tab.openAsSplit",
          in.current_can_pair ? PhiStr("contextMenu.splitWithCurrent") : PhiStr("contextMenu.openAsSplit")));
      items.push_back(PhiMenuItem::Separator());
    }
    items.push_back(
        PersistCommand("tab.bookmark", PhiStr("contextMenu.addToBookmarks"), in.incognito));
    // "Add to Folder": this Space's bookmark folders in outline order,
    // nesting indented, then a fresh folder — the Mac's addToFolder submenu.
    // Nothing persists from incognito.
    if (in.bookmark_folders) {
      std::vector<PhiMenuItem> folder_items =
          ChoiceRows(in.folders, "tab.addToFolder");
      if (!folder_items.empty()) {
        folder_items.push_back(PhiMenuItem::Separator());
      }
      folder_items.push_back(
          PhiMenuItem::Command("tab.newFolder", PhiStr("contextMenu.newFolder")));
      items.push_back(
          PhiMenuItem::Submenu(PhiStr("contextMenu.addToFolder"), std::move(folder_items)));
    }
    items.push_back(PhiMenuItem::Separator());
    AppendGroupBlock(in, items);
    items.push_back(PhiMenuItem::Separator());
    // Move/Clone to Space — the Mac's cross-Space submenus (the tab's own
    // Space skipped; nothing persists from incognito).
    if (!in.spaces.empty()) {
      items.push_back(PhiMenuItem::Submenu(
          PhiStr("contextMenu.moveToSpace"), ChoiceRows(in.spaces, "tab.moveToSpace")));
      items.push_back(PhiMenuItem::Submenu(
          PhiStr("contextMenu.cloneToSpace"), ChoiceRows(in.spaces, "tab.cloneToSpace")));
      items.push_back(PhiMenuItem::Separator());
    }
  } else {
    // The merged split row: every action operates on the pair.
    items.push_back(PersistCommand("tab.pin", PhiStr("contextMenu.pinSplit"), in.incognito));
    items.push_back(PhiMenuItem::Command("tab.duplicate", PhiStr("contextMenu.duplicateSplit")));
    items.push_back(PhiMenuItem::Command("tab.copyLeft", PhiStr("contextMenu.copyLeftURL")));
    items.push_back(PhiMenuItem::Command("tab.copyRight", PhiStr("contextMenu.copyRightURL")));
    items.push_back(PhiMenuItem::Separator());
    // The Mac's "Reverse Panes": swap the two sides of this split.
    items.push_back(
        PhiMenuItem::Command("tab.reversePanes", PhiStr("contextMenu.reversePanes")));
    // The Mac's convert item, labelled by where it takes you: a side-by-side
    // pair offers the stacked arrangement and vice versa.
    items.push_back(PhiMenuItem::Command("tab.convertSplit",
                                         in.split_stacked
                                             ? PhiStr("contextMenu.convertToHorizontalSplit")
                                             : PhiStr("contextMenu.convertToVerticalSplit")));
    items.push_back(
        PhiMenuItem::Command("tab.removeSplit", PhiStr("contextMenu.removeFromSplit")));
    items.push_back(PhiMenuItem::Separator());
    items.push_back(PersistCommand("tab.bookmark", PhiStr("contextMenu.addSplitToBookmarks"),
                                   in.incognito));
    items.push_back(PhiMenuItem::Separator());
    // The pair groups as one: both panes join (or leave) together, as the
    // Mac's per-tab group block does for a split member.
    AppendGroupBlock(in, items);
    items.push_back(PhiMenuItem::Separator());
  }
  // Chrome's "Move tab to new window" (§3.5): the row (a split's pair
  // together) detaches into a fresh window of the same profile.
  items.push_back(
      PhiMenuItem::Command("tab.moveToNewWindow", PhiStr("contextMenu.moveToNewWindow")));
  items.push_back(PhiMenuItem::Separator());
  items.push_back(PhiMenuItem::Command("tab.close", PhiStr("contextMenu.close")));
  items.push_back(PhiMenuItem::Command("tab.closeOthers", PhiStr("contextMenu.closeOtherTabs")));
  return spec;
}

// --- Pinned cell -----------------------------------------------------------

PhiMenuSpec PhiBuildPinnedCellMenu(const PhiPinnedMenuInputs& in) {
  PhiMenuSpec spec;
  std::vector<PhiMenuItem>& items = spec.items;
  items.push_back(PhiMenuItem::Command("pin.open", PhiStr("contextMenu.open")));
  items.push_back(PhiMenuItem::Command(
      "pin.duplicate", in.split_cell ? PhiStr("contextMenu.duplicateSplit") : PhiStr("contextMenu.duplicate")));
  if (in.split_cell) {
    items.push_back(PhiMenuItem::Command("pin.copyLeft", PhiStr("contextMenu.copyLeftURL")));
    items.push_back(PhiMenuItem::Command("pin.copyRight", PhiStr("contextMenu.copyRightURL")));
  } else {
    items.push_back(PhiMenuItem::Command("pin.copyUrl", PhiStr("contextMenu.copyLink")));
  }
  items.push_back(PhiMenuItem::Command("pin.rename", PhiStr("contextMenu.rename")));
  // The Mac's Edit…: the pin's address (a split pin keeps its panes).
  if (!in.split_cell) {
    items.push_back(PhiMenuItem::Command("pin.editUrl", PhiStr("contextMenu.editURL")));
  }
  if (in.split_cell) {
    items.push_back(PhiMenuItem::Separator());
    items.push_back(PhiMenuItem::Command("pin.convertSplit",
                                         in.horizontal
                                             ? PhiStr("contextMenu.convertToHorizontalSplit")
                                             : PhiStr("contextMenu.convertToVerticalSplit")));
  }
  items.push_back(PhiMenuItem::Separator());
  items.push_back(PhiMenuItem::Command(
      "pin.unpin", in.split_cell ? PhiStr("contextMenu.unpinSplit") : PhiStr("contextMenu.unpin")));
  items.push_back(PhiMenuItem::Separator());
  PhiMenuItem close = PhiMenuItem::Command("pin.close", PhiStr("contextMenu.close"));
  close.enabled = in.open;
  items.push_back(std::move(close));
  return spec;
}

// --- Bookmark row ----------------------------------------------------------

PhiMenuSpec PhiBuildBookmarkBarAreaMenu(bool incognito) {
  PhiMenuSpec spec;
  spec.items.push_back(
      PersistCommand("bar.newRootFolder", PhiStr("contextMenu.newFolder"), incognito));
  return spec;
}

PhiMenuSpec PhiBuildBookmarkRowMenu(const PhiBookmarkMenuInputs& in) {
  PhiMenuSpec spec;
  std::vector<PhiMenuItem>& items = spec.items;
  if (!in.folder) {
    items.push_back(PhiMenuItem::Command("bm.open", PhiStr("contextMenu.openInNewTab")));
    items.push_back(PhiMenuItem::Command("bm.openSplit", PhiStr("contextMenu.openAsSplit")));
    if (in.split_row) {
      items.push_back(PhiMenuItem::Command("bm.copyLeft", PhiStr("contextMenu.copyLeftURL")));
      items.push_back(PhiMenuItem::Command("bm.copyRight", PhiStr("contextMenu.copyRightURL")));
    } else {
      items.push_back(PhiMenuItem::Command("bm.copyUrl", PhiStr("contextMenu.copyLink")));
    }
  }
  items.push_back(PhiMenuItem::Command("bm.rename", PhiStr("contextMenu.rename")));
  if (!in.folder && !in.split_row) {
    // The Mac's Edit…
    items.push_back(PhiMenuItem::Command("bm.editUrl", PhiStr("contextMenu.editURL")));
  }
  if (in.split_row) {
    items.push_back(PhiMenuItem::Command("bm.convertSplit",
                                         in.horizontal
                                             ? PhiStr("contextMenu.convertToHorizontalSplit")
                                             : PhiStr("contextMenu.convertToVerticalSplit")));
  }
  if (in.folder) {
    // The Mac's folder menu: Rename, then the icon picker.
    items.push_back(PhiMenuItem::Command("bm.changeIcon", PhiStr("contextMenu.changeIcon")));
    items.push_back(
        PhiMenuItem::Command("bm.newFolder", PhiStr("contextMenu.newNestedFolder")));
  }
  // Move/Clone to Space — the Mac's appendSpaceTransferMenuItems, leaf rows
  // only (a folder's subtree stays where it is).
  if (!in.folder && !in.spaces.empty()) {
    items.push_back(PhiMenuItem::Separator());
    items.push_back(PhiMenuItem::Submenu(
        PhiStr("contextMenu.moveToSpace"), ChoiceRows(in.spaces, "bm.moveToSpace")));
    items.push_back(PhiMenuItem::Submenu(
        PhiStr("contextMenu.cloneToSpace"), ChoiceRows(in.spaces, "bm.cloneToSpace")));
  }
  items.push_back(PhiMenuItem::Separator());
  items.push_back(PhiMenuItem::Command(
      "bm.remove", in.folder ? PhiStr("contextMenu.deleteFolder") : PhiStr("contextMenu.delete")));
  return spec;
}

// --- The tab area ----------------------------------------------------------

PhiMenuSpec PhiBuildTabAreaMenu(const PhiTabAreaMenuInputs& in) {
  PhiMenuSpec spec;
  std::vector<PhiMenuItem>& items = spec.items;
  if (in.space_only) {
    // The pip's menu (§5.4): the Space block without its leading separator
    // and without the "New Space" row (the "+" pip and the switcher do that).
    PhiTabAreaMenuInputs rest = in;
    rest.space_only = false;
    PhiMenuSpec full = PhiBuildTabAreaMenu(rest);
    bool past_layout = false;
    for (PhiMenuItem& item : full.items) {
      if (item.key == "area.renameSpace") {
        past_layout = true;
      }
      if (past_layout) {
        items.push_back(std::move(item));
      }
    }
    return spec;
  }
  items.push_back(PhiMenuItem::Command("area.newTab", PhiStr("contextMenu.newTab")));
  items.push_back(PhiMenuItem::Command("area.reopen", PhiStr("contextMenu.reopenClosedTab")));
  items.push_back(PhiMenuItem::Separator());
  items.push_back(
      PersistCommand("area.newRootFolder", PhiStr("contextMenu.newFolder"), in.incognito));
  PhiMenuItem bookmark_all =
      PersistCommand("area.bookmarkAll", PhiStr("contextMenu.bookmarkAllTabs"), in.incognito);
  // Two different disabled states: incognito greys the row off the record,
  // an empty tab list only makes it inert.
  bookmark_all.enabled = !in.incognito && !in.tabs_empty;
  bookmark_all.grey_when_disabled = in.incognito;
  items.push_back(std::move(bookmark_all));
  items.push_back(PhiMenuItem::Separator());
  const struct {
    const char* id;
    PhiLayoutMode mode;
    const wchar_t* label;
  } kLayouts[] = {
      // Performance is retired on Windows (impl log D9): two layouts.
      {"comfortable", PhiLayoutMode::kComfortable, PhiStr("contextMenu.horizontalTabs")},
      {"balanced", PhiLayoutMode::kBalanced, PhiStr("contextMenu.verticalTabs")},
  };
  std::vector<PhiMenuItem> layouts;
  for (const auto& entry : kLayouts) {
    PhiMenuItem item = PhiMenuItem::Command("area.layout", entry.label);
    item.target.id = entry.id;
    item.checked = in.mode == entry.mode;
    layouts.push_back(std::move(item));
  }
  items.push_back(PhiMenuItem::Submenu(PhiStr("contextMenu.layout"), std::move(layouts)));
  if (in.incognito && !in.incognito_space) {
    // Incognito shells have no Spaces: the active-Space block never appears.
    return spec;
  }
  // The active Space's own theme and profile, in the Mac's order
  // (appendActiveSpaceMenuItems): rename, icon, theme, profile, delete.
  //
  // Change Profile — the Mac's radio list. Re-binding respawns the Space's
  // window, which the default Space's cannot be, so its rows read the
  // binding out but stay disabled.
  std::vector<PhiMenuItem> profiles;
  for (const PhiMenuChoice& profile : in.profiles) {
    PhiMenuItem item = PhiMenuItem::Command("area.profile", profile.label);
    item.target.id = profile.id;
    item.target.text = profile.label;
    item.checked = profile.id == in.active_profile;
    item.enabled = in.can_reprofile;
    profiles.push_back(std::move(item));
  }
  if (profiles.empty()) {
    PhiMenuItem none;
    none.label = PhiStr("menuBar.noProfiles");
    none.enabled = false;
    profiles.push_back(std::move(none));
  }
  items.push_back(PhiMenuItem::Separator());
  items.push_back(PhiMenuItem::Command("area.newSpace", PhiStr("contextMenu.newSpace")));
  // An Incognito Space keeps the generated name and the fixed-dark theme.
  PhiMenuItem rename =
      PhiMenuItem::Command("area.renameSpace", PhiStr("contextMenu.renameSpace"));
  rename.enabled = !in.incognito_space;
  items.push_back(std::move(rename));
  items.push_back(PhiMenuItem::Command("area.changeIcon", PhiStr("contextMenu.changeIcon")));
  // Set Theme opens the Mac's editor panel (swatch row + saturation slider)
  // rather than a list of theme names.
  PhiMenuItem theme = PhiMenuItem::Command("area.setTheme", PhiStr("sidebar.setTheme"));
  theme.enabled = !in.incognito_space;
  items.push_back(std::move(theme));
  items.push_back(PhiMenuItem::Submenu(
      PhiStr("app.activeSpaceContextMenu.profileSubmenuTitle"),
      std::move(profiles)));
  PhiMenuItem del =
      PhiMenuItem::Command("area.deleteSpace", PhiStr("contextMenu.deleteSpace"));
  del.enabled = in.can_delete_space;
  items.push_back(std::move(del));
  if (in.incognito_space) {
    // The Incognito Space's own end (asked first by the shell).
    items.push_back(PhiMenuItem::Separator());
    items.push_back(PhiMenuItem::Command(
        "area.closeIncognitoSpace",
        PhiStr("app.spacesMenu.closeActiveIncognitoSpace")));
  }
  return spec;
}

// --- The Spaces strip ------------------------------------------------------

PhiMenuSpec PhiBuildAgentChipMenu() {
  PhiMenuSpec spec;
  if (kPhiTranscriptEnabled) {
    spec.items.push_back(
        PhiMenuItem::Command("agent.transcript", PhiStr("contextMenu.showTranscript")));
    spec.items.push_back(PhiMenuItem::Separator());
  }
  spec.items.push_back(PhiMenuItem::Command("agent.finish", PhiStr("contextMenu.finishTask")));
  return spec;
}

PhiMenuSpec PhiBuildSpaceSwitcherMenu(const std::vector<PhiMenuChoice>& spaces,
                                      const std::vector<PhiMenuChoice>& agents,
                                      const std::string& watched_agent,
                                      const std::string& active_space,
                                      bool with_new_space,
                                      bool with_new_profile) {
  PhiMenuSpec spec;
  spec.items = ChoiceRows(spaces, "space.switch");
  for (PhiMenuItem& item : spec.items) {
    if (item.target.id == active_space) {
      // Bold (the default item) marks the active Space: a row with a bitmap
      // shows no check mark.
      item.checked = true;
      item.default_item = true;
    }
  }
  if (!agents.empty()) {
    // The agent Spaces after the user's, past a rule, as the strip groups
    // them — the compact pip's only road to watch mode.
    spec.items.push_back(PhiMenuItem::Separator());
    std::vector<PhiMenuItem> rows = ChoiceRows(agents, "agent.watch");
    for (PhiMenuItem& item : rows) {
      if (item.target.id == watched_agent) {
        item.checked = true;
        item.default_item = true;
      }
      spec.items.push_back(std::move(item));
    }
  }
  if (!with_new_space) {
    return spec;
  }
  // "New Space…" after a rule, wearing the plus (the Mac's spaceSwitcherMenu
  // newSpaceItem): it opens the create form — inline in a docked sidebar,
  // else in the standalone window.
  spec.items.push_back(PhiMenuItem::Separator());
  PhiMenuItem new_space = PhiMenuItem::Command("space.new", PhiStr("contextMenu.newSpace"));
  new_space.glyph = L"\uE710";  // MDL2 Add.
  spec.items.push_back(std::move(new_space));
  if (!with_new_profile) {
    return spec;
  }
  // The Mac's create panel offers a fresh Profile for the new Space; here it
  // is the second row (profile creation is asynchronous — the Space follows
  // once the profile exists).
  spec.items.push_back(PhiMenuItem::Command(
      "space.newWithProfile", PhiStr("contextMenu.newSpaceWithNewProfile")));
  return spec;
}

// --- The sidebar's profile button ------------------------------------------

PhiMenuSpec PhiBuildProfileMenu(const PhiProfileMenuInputs& in) {
  // The Mac's populateProfileMenu, row for row. The chords arrive resolved
  // (a rebindable command shows the user's binding, as the ⋯ menu does)
  // rather than from the decor table, which only knows the defaults.
  PhiMenuSpec spec;
  std::vector<PhiMenuItem>& items = spec.items;
  const auto row = [&](const char* key, const char* label_key,
                       const std::wstring& chord) {
    PhiMenuItem item = PhiMenuItem::Command(key, PhiStr(label_key));
    item.shortcut = chord;
    items.push_back(std::move(item));
  };
  row("profile.downloads", "profile.menu.downloads", in.downloads_chord);
  row("profile.bookmarks", "profile.menu.bookmarks", in.bookmarks_chord);
  row("profile.extensions", "profile.menu.extensions", in.extensions_chord);
  items.push_back(PhiMenuItem::Separator());
  row("profile.newTab", "profile.menu.newTab", in.new_tab_chord);
  row("profile.newIncognitoSpace", "profile.menu.newIncognitoSpace",
      in.new_incognito_space_chord);
  row("profile.newIncognitoWindow", "profile.menu.newIncognitoWindow",
      in.new_incognito_window_chord);
  items.push_back(PhiMenuItem::Separator());
  row("profile.settings", "profile.menu.settings", in.settings_chord);
  if (!in.signed_in) {
    // Guest Mode / signed out: the Mac's trailing Sign in (its
    // supportsAuthentication && !isAuthenticated).
    row("profile.signIn", "profile.menu.signIn", std::wstring());
  }
  return spec;
}

// --- The omnibox field -----------------------------------------------------

PhiMenuSpec PhiBuildOmniboxFieldMenu(bool has_selection,
                                     bool has_text,
                                     bool clipboard_text,
                                     bool can_undo) {
  PhiMenuSpec spec;
  std::vector<PhiMenuItem>& items = spec.items;
  if (clipboard_text) {
    items.push_back(PhiMenuItem::Command("edit.pasteAndGo",
                                         PhiStr("contextMenu.edit.pasteAndGo")));
    items.push_back(PhiMenuItem::Separator());
  }
  PhiMenuItem undo = PhiMenuItem::Command("edit.undo", PhiStr("contextMenu.edit.undo"));
  undo.enabled = can_undo;
  items.push_back(std::move(undo));
  items.push_back(PhiMenuItem::Separator());
  PhiMenuItem cut = PhiMenuItem::Command("edit.cut", PhiStr("contextMenu.edit.cut"));
  cut.enabled = has_selection;
  items.push_back(std::move(cut));
  PhiMenuItem copy = PhiMenuItem::Command("edit.copy", PhiStr("contextMenu.edit.copy"));
  copy.enabled = has_selection;
  items.push_back(std::move(copy));
  PhiMenuItem paste = PhiMenuItem::Command("edit.paste", PhiStr("contextMenu.edit.paste"));
  paste.enabled = clipboard_text;
  items.push_back(std::move(paste));
  PhiMenuItem erase = PhiMenuItem::Command("edit.delete", PhiStr("contextMenu.edit.delete"));
  erase.enabled = has_selection;
  items.push_back(std::move(erase));
  items.push_back(PhiMenuItem::Separator());
  PhiMenuItem all = PhiMenuItem::Command("edit.selectAll", PhiStr("contextMenu.edit.selectAll"));
  all.enabled = has_text;
  items.push_back(std::move(all));
  return spec;
}

// --- The address bar's site menu ------------------------------------------

PhiMenuSpec PhiBuildSiteMenu(const PhiSiteMenuInputs& in) {
  PhiMenuSpec spec;
  std::vector<PhiMenuItem>& items = spec.items;
  std::vector<PhiMenuItem> settings;
  settings.push_back(PhiMenuItem::Command(
      "site.clearCache", PhiStr("browser.addressBarMenu.clearCacheAction")));
  settings.push_back(PhiMenuItem::Command(
      "site.clearCookies", PhiStr("browser.addressBarMenu.clearCookiesAction")));
  settings.push_back(PhiMenuItem::Separator());
  settings.push_back(PhiMenuItem::Command(
      "site.moreSettings",
      PhiStr("browser.addressBarMenu.openBrowserSettingsAction")));
  PhiMenuItem settings_item = PhiMenuItem::Submenu(
      PhiStr("browser.addressBarMenu.siteSettingsAction"), std::move(settings));
  settings_item.glyph = L"\uE713";  // MDL2 Settings, the Mac's gearshape.
  items.push_back(std::move(settings_item));
  if (in.content_blocking_row) {
    // The page picks the rule set up on its next load (the shell reloads).
    PhiMenuItem blocking = PhiMenuItem::Command(
        "site.contentBlocking",
        PhiStr("browser.addressBarMenu.contentBlocking.toggle"));
    blocking.checked = in.content_blocking_on;
    items.push_back(std::move(blocking));
  }
  items.push_back(PhiMenuItem::Separator());
  PhiMenuItem full_url = PhiMenuItem::Command(
      "site.showFullUrl", PhiStr("browser.addressBarMenu.showFullURLToggle"));
  full_url.checked = in.always_show_url_path;
  items.push_back(std::move(full_url));
  if (!in.security.empty()) {
    // The Mac's status row: a lock when the connection is secure, the red
    // error mark (a danger row) for anything else; a click opens the
    // certificate when the page has one.
    items.push_back(PhiMenuItem::Separator());
    const bool secure = in.security == "secure";
    const char* label =
        secure                         ? "browser.addressBarMenu.security.secureStatus"
        : in.security == "partial"     ? "browser.addressBarMenu.security.partiallySecureStatus"
        : in.security == "insecure"    ? "browser.addressBarMenu.security.notSecureStatus"
                                       : "browser.addressBarMenu.security.unknownStatus";
    items.push_back(PhiMenuItem::Command(
        secure ? "site.security" : "site.securityWarning", PhiStr(label)));
  }
  return spec;
}

// --- The bookmark bar ------------------------------------------------------

namespace {

void AppendBookmarkNodes(const std::vector<PhiMenuBookmarkNode>& rows,
                         size_t skip,
                         int depth,
                         size_t* leaves,
                         std::vector<PhiMenuItem>* into) {
  size_t at = 0;
  for (const PhiMenuBookmarkNode& row : rows) {
    if (at++ < skip || *leaves >= 200) {
      continue;
    }
    if (row.folder) {
      if (depth >= 4) {
        continue;
      }
      std::vector<PhiMenuItem> children;
      AppendBookmarkNodes(row.children, 0, depth + 1, leaves, &children);
      into->push_back(PhiMenuItem::Submenu(row.title, std::move(children)));
    } else if (!row.url.empty()) {
      ++*leaves;
      PhiMenuItem item = PhiMenuItem::Command("bar.open", row.title);
      item.target.id = row.guid;
      item.target.url = row.url;
      into->push_back(std::move(item));
    }
  }
}

}  // namespace

PhiMenuSpec PhiBuildBookmarkBarMenu(
    const std::vector<PhiMenuBookmarkNode>& roots,
    size_t skip_first) {
  PhiMenuSpec spec;
  size_t leaves = 0;
  AppendBookmarkNodes(roots, skip_first, 0, &leaves, &spec.items);
  return spec;
}

// --- Showing one -----------------------------------------------------------

#if defined(_WIN32)

namespace {

// The Fluent decorations by KEY (docs/windows-native-ui.md §3.5): the
// chord in the right column, the destructive rows in red, an icon on the
// primary rows. Kept out of the pure builders so the host test pins the
// order alone; the painter reads them from the row.
struct RowDecor {
  const char* key;
  const wchar_t* shortcut;
  bool danger;
  const wchar_t* icon;
};
constexpr RowDecor kRowDecor[] = {
    {"area.newTab", L"Ctrl+T", false, fluent::kAdd},
    {"area.reopen", L"Ctrl+Shift+T", false, fluent::kClock},
    {"area.newRootFolder", nullptr, false, fluent::kFolder},
    {"area.bookmarkAll", L"Ctrl+Shift+D", false, fluent::kFavoriteStar},
    {"area.newSpace", nullptr, false, fluent::kAdd},
    {"area.renameSpace", nullptr, false, fluent::kRename},
    {"area.changeIcon", nullptr, false, fluent::kEmoji},
    {"area.setTheme", nullptr, false, fluent::kColor},
    {"area.deleteSpace", nullptr, true, fluent::kDelete},
    {"space.new", nullptr, false, fluent::kAdd},
    {"tab.pin", nullptr, false, fluent::kPin},
    {"tab.duplicate", nullptr, false, fluent::kCopy},
    {"tab.copyUrl", L"Ctrl+Shift+C", false, fluent::kLink},
    {"site.security", nullptr, false, fluent::kLock},
    {"site.securityWarning", nullptr, true, fluent::kStatusError},
    {"tab.moveToNewWindow", nullptr, false, fluent::kOpenInNewWindow},
    {"edit.undo", L"Ctrl+Z", false, fluent::kUndo},
    {"edit.cut", L"Ctrl+X", false, fluent::kCut},
    {"edit.copy", L"Ctrl+C", false, fluent::kCopy},
    {"edit.paste", L"Ctrl+V", false, fluent::kPaste},
    {"edit.pasteAndGo", nullptr, false, fluent::kGo},
    {"edit.delete", nullptr, false, nullptr},
    {"edit.selectAll", L"Ctrl+A", false, nullptr},
    {"tab.bookmark", L"Ctrl+D", false, fluent::kFavoriteStar},
    {"tab.close", L"Ctrl+W", false, fluent::kCancel},
    {"tab.closeOthers", nullptr, false, nullptr},
    {"tab.removeSplit", nullptr, false, nullptr},
    {"pin.open", nullptr, false, fluent::kOpenInNewWindow},
    {"pin.copyUrl", nullptr, false, fluent::kLink},
    {"pin.rename", nullptr, false, fluent::kRename},
    {"pin.unpin", nullptr, false, fluent::kUnpin},
    {"pin.close", L"Ctrl+W", false, fluent::kCancel},
    {"bm.open", nullptr, false, fluent::kOpenInNewWindow},
    {"bm.copyUrl", nullptr, false, fluent::kLink},
    {"bm.rename", nullptr, false, fluent::kRename},
    {"bm.changeIcon", nullptr, false, fluent::kEmoji},
    {"bm.newFolder", nullptr, false, fluent::kFolder},
    {"bm.remove", nullptr, true, fluent::kDelete},
    {"bar.newRootFolder", nullptr, false, fluent::kFolder},
    {"bar.open", nullptr, false, fluent::kGlobe},
    {"group.rename", nullptr, false, fluent::kRename},
    {"group.ungroup", nullptr, false, nullptr},
    {"group.close", nullptr, true, fluent::kCancel},
    {"agent.finish", nullptr, true, nullptr},
    {"feedback.open", L"Enter", false, fluent::kOpenFile},
    {"feedback.reveal", nullptr, false, fluent::kOpenFolder},
    {"feedback.remove", L"Del", true, fluent::kDelete},
    // The profile button's rows wear the ⋯ menu's icons for the same
    // commands; their chords come resolved on the row (PhiBuildProfileMenu).
    {"profile.downloads", nullptr, false, fluent::kDownload},
    {"profile.bookmarks", nullptr, false, fluent::kFavoriteStar},
    {"profile.extensions", nullptr, false, fluent::kPuzzle},
    {"profile.newTab", nullptr, false, fluent::kAdd},
    {"profile.newIncognitoSpace", nullptr, false, nullptr},
    {"profile.newIncognitoWindow", nullptr, false, fluent::kIncognito},
    {"profile.settings", nullptr, false, fluent::kSettings},
    {"profile.signIn", nullptr, false, fluent::kContact},
};

const RowDecor* DecorFor(const std::string& key) {
  for (const RowDecor& decor : kRowDecor) {
    if (key == decor.key) {
      return &decor;
    }
  }
  return nullptr;
}

// Flattens the spec into an HMENU of owner-drawn rows, numbering the
// actionable rows from 1 so TPM_RETURNCMD's answer indexes `actions`
// directly. Submenus are owned by their parent, so the caller destroys
// exactly one handle; the rows live in `store` for the loop.
bool AppendSpec(HMENU menu,
                const std::vector<PhiMenuItem>& items,
                std::vector<const PhiMenuItem*>* actions,
                PhiMenuRowStore* store) {
  for (const PhiMenuItem& item : items) {
    if (item.separator) {
      store->AppendSeparator(menu);
      continue;
    }
    if (item.submenu) {
      HMENU sub = ::CreatePopupMenu();
      if (!sub) {
        return false;
      }
      if (!AppendSpec(sub, item.items, actions, store)) {
        ::DestroyMenu(sub);
        return false;
      }
      PhiMenuRow* row = store->AppendSubmenu(menu, sub, item.label);
      row->height_dip = 32;
      row->picture = item.glyph;
      continue;
    }
    // A label-only row (the "no profiles" placeholder) carries no key and
    // takes id 0, which TrackPopupMenu can never return.
    UINT id = 0;
    if (!item.key.empty()) {
      actions->push_back(&item);
      id = static_cast<UINT>(actions->size());
    }
    PhiMenuRow* row = store->AppendCommand(menu, id, item.label, nullptr,
                                           item.enabled);
    row->height_dip = 32;
    row->picture = item.glyph;
    row->hint = item.hint;
    row->shortcut = item.shortcut;
    if (!item.hint.empty()) {
      // A two-line row (the switcher, §5.3): 40 dip in a 300 dip flyout.
      row->height_dip = 40;
      row->min_width_dip = 300;
    }
    if (const RowDecor* decor = DecorFor(item.key)) {
      if (decor->shortcut && row->shortcut.empty()) {
        row->shortcut = decor->shortcut;
      }
      row->danger = decor->danger;
      if (decor->icon) {
        row->icon = decor->icon;
      }
    }
    UINT state = item.enabled ? MFS_ENABLED : MFS_DISABLED;
    if (!item.enabled && !item.grey_when_disabled) {
      // Inert but not greyed: the design retires that state — a disabled
      // row reads disabled (docs/windows-native-ui.md §3.5).
      state = MFS_DISABLED;
    }
    if (item.checked) {
      state |= MFS_CHECKED;
    }
    if (item.default_item) {
      state |= MFS_DEFAULT;
    }
    MENUITEMINFOW info = {sizeof(info)};
    info.fMask = MIIM_STATE;
    info.fState = state;
    ::SetMenuItemInfoW(menu, ::GetMenuItemCount(menu) - 1, TRUE, &info);
  }
  return true;
}

}  // namespace

PhiMenuAction PhiRunPopupMenu(const PhiMenuSpec& spec,
                              const PhiPopupMenuRequest& request) {
  PhiMenuAction action;
  if (!request.owner) {
    return action;
  }
  HMENU menu = ::CreatePopupMenu();
  if (!menu) {
    return action;
  }
  // Pointers into `spec`, which outlives the loop: the SPEC is a value the
  // caller built from a snapshot, so nothing here reaches into a rebuildable
  // cache (docs/architecture-review.md §3.1 principle 4).
  std::vector<const PhiMenuItem*> actions;
  PhiMenuRowStore rows;
  const bool built = AppendSpec(menu, spec.items, &actions, &rows);
  UINT chosen = 0;
  if (built) {
    if (request.palette) {
      PhiMenuApplyTheme(menu, *request.palette);
    }
    // The nested loop: a PhiModalScope, so the coordinator parks pipe verbs
    // and the shell parks its own close until the menu is down.
    PhiModalScope modal;
    chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, request.align | TPM_RETURNCMD, request.x, request.y, 0,
        request.owner, nullptr));
  }
  ::DestroyMenu(menu);  // Owns every submenu it was given.
  if (chosen >= 1 && chosen <= actions.size()) {
    const PhiMenuItem* item = actions[chosen - 1];
    action.key = item->key;
    action.target = item->target;
  }
  return action;
}

#endif  // defined(_WIN32)

}  // namespace phi_client

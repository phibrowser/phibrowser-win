// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SHELL_MENUS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SHELL_MENUS_H_

#if defined(_WIN32)
#include <windows.h>
#endif

#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_shell_layout.h"
#include "phibrowser-win/client/phi_theme.h"

// Every context menu the shell window drops, as DATA
// (docs/architecture-review.md §3.1 principle 4, §3.5, lane 1 §3.2).
//
// The rule the old code broke: `::TrackPopupMenu` runs a nested message
// loop, so a `kMsgPhiRebuildRows` posted before or during it is dispatched
// INSIDE it — `tabs_`, `pinned_`, `bookmarks_`, `group_headers_` and
// `chips_` are cleared and refilled, and any reference into them dangles
// from that moment on. Phase 0 papered over it by copying each row's fields
// into locals just before the loop; the shape here is that no reference
// exists at all:
//
//   1. the shell takes a SNAPSHOT of the row (plain values),
//   2. a PURE builder turns (snapshot, tree, prefs, policy) into a
//      `PhiMenuSpec` — no HWND, no host round-trip, host-testable,
//   3. `PhiRunPopupMenu` shows it under a `PhiModalScope` and answers with a
//      `PhiMenuAction`: a stable KEY plus the target the key names, both by
//      value,
//   4. the shell applies the action by key against state it re-reads AFTER
//      the loop (the active browser, the store, the tree).
//
// Nothing that survives the loop points into a cache, so a rebuild under a
// menu is a non-event. The keys are also what the model tests assert on:
// menu contents and order are pinned host-side instead of only by the box's
// suite-menu.

namespace phi_client {

// One dynamic row's identity, copied out of the model when the spec is built
// and handed back untouched with the action. Never a pointer, never an index
// into a rebuildable vector.
struct PhiMenuTarget {
  // The stable id the key names: a Space id, a store guid, a tab-group id, a
  // profile id, a colour name, a layout-mode wire name.
  std::string id;
  // The address a bookmark-bar row carries — the row may be gone by the time
  // the action is applied, and the click still opens what the user saw.
  std::string url;
  // The display name a confirmation prompt has to repeat (profile rows).
  std::wstring text;

  bool operator==(const PhiMenuTarget&) const = default;
};

// One item. A separator carries nothing else; an item with `items` is a
// submenu. Builders simply do not emit a submenu they would leave empty —
// except a bookmark FOLDER, which shows as an empty dropdown exactly as the
// bar's own menu always has.
struct PhiMenuItem {
  std::string key;  // Stable across a rebuild; empty on separators/labels.
  PhiMenuTarget target;
  std::wstring label;
  bool enabled = true;
  // A disabled item that is NOT greyed (MF_DISABLED without MF_GRAYED): the
  // tab area's "Bookmark All Tabs" with no tabs reads as a live row that
  // does nothing, which is what the Mac does.
  bool grey_when_disabled = true;
  bool checked = false;
  // MF_DEFAULT — bold. A row with a bitmap shows no check mark, so the
  // Space switchers mark the active Space this way as well.
  bool default_item = false;
  bool separator = false;
  // A submenu — true even when `items` is empty (a bookmark folder with no
  // children still drops an empty menu, as the bar's own always has).
  bool submenu = false;
  // Rasterized into the row's bitmap by the runner (Space icons: menu text
  // cannot render the strip's artwork or a colour emoji).
  std::wstring glyph;
  // A dim second line (40 dip rows) and a per-row chord; the decor table
  // supplies chords by key for everything else.
  std::wstring hint;
  std::wstring shortcut;
  std::vector<PhiMenuItem> items;

  static PhiMenuItem Separator();
  static PhiMenuItem Command(std::string key, std::wstring label);
  static PhiMenuItem Submenu(std::wstring label, std::vector<PhiMenuItem> items);
};

struct PhiMenuSpec {
  std::vector<PhiMenuItem> items;

  bool empty() const { return items.empty(); }
  // The first item with `key`, anywhere in the tree, or null.
  const PhiMenuItem* Find(std::string_view key) const;
  // Depth-first keys, one entry per item, "-" for a separator and "" for a
  // label-only row. Submenu children follow their parent. What the model
  // test asserts order with.
  std::vector<std::string> Keys() const;
};

// What the user chose. `key` empty means the menu was dismissed.
struct PhiMenuAction {
  std::string key;
  PhiMenuTarget target;

  bool valid() const { return !key.empty(); }
  bool is(std::string_view k) const { return key == k; }
};

// --- Builder inputs --------------------------------------------------------
// Display text arrives WIDE: the pure half does no UTF-8 conversion (that
// needs base/, which the host build does not link) and no store lookups. The
// shell converts and orders; these builders decide shape.

// A dynamic row: an id plus what it reads as.
struct PhiMenuChoice {
  std::string id;
  std::wstring label;
  std::wstring glyph;  // Space rows only.
  // The switcher's second line and keycap (§5.3): "profile · N tabs" and
  // "Ctrl+n"; empty on every other list.
  std::wstring hint;
  std::wstring shortcut;
};

// The omnibox field's menu (§3.5): "Paste and go" leads (only with text
// on the clipboard), then the EDIT's own verbs with their states.
PhiMenuSpec PhiBuildOmniboxFieldMenu(bool has_selection,
                                     bool has_text,
                                     bool clipboard_text,
                                     bool can_undo);

// A tab group's colour swatch row.
struct PhiMenuColorOption {
  std::string name;  // The palette name; keys the localized label.
  bool checked = false;
};

// --- The sidebar's row menus ----------------------------------------------

// The group header's menu: rename, the colour list, ungroup, close.
PhiMenuSpec PhiBuildGroupHeaderMenu(
    const std::vector<PhiMenuColorOption>& colors);

// The tab row's menu, in the Mac TabModel+Sidebar order: pin, duplicate,
// copy, the split block, bookmarks, groups, Spaces, then close.
struct PhiTabMenuInputs {
  bool merged = false;      // A split row: every action takes the pair.
  bool incognito = false;   // Nothing persists: the store rows grey off.
  bool agent_driven = false;
  bool agent_reclaimed = false;
  // The row already belongs to a split (unmerged rows only): no
  // "Split with Current" for it.
  bool split_bound = false;
  // The FOCUSED tab can still take a partner — "Split with Current" rather
  // than "Open as Split".
  bool current_can_pair = false;
  bool split_stacked = false;  // Merged rows: the divider's current axis.
  bool grouped = false;        // The row is in a tab group.
  bool bookmark_folders = false;  // Emit the "Add to Folder" submenu.
  std::vector<PhiMenuChoice> folders;  // Labels already indented.
  std::vector<PhiMenuChoice> groups;   // Labels already carry the count.
  std::vector<PhiMenuChoice> spaces;   // The tab's own Space excluded.
};
PhiMenuSpec PhiBuildTabRowMenu(const PhiTabMenuInputs& in);

// A pinned cell's menu: open, duplicate, copy, rename, unpin, close.
struct PhiPinnedMenuInputs {
  bool split_cell = false;
  bool horizontal = false;  // The cell remembers a stacked arrangement.
  bool open = false;        // A live tab is bound: "Close" is live.
};
PhiMenuSpec PhiBuildPinnedCellMenu(const PhiPinnedMenuInputs& in);

// A bookmark row's menu (a leaf or a folder).
struct PhiBookmarkMenuInputs {
  bool folder = false;
  bool split_row = false;
  bool horizontal = false;
  std::vector<PhiMenuChoice> spaces;  // Leaf rows only.
};
PhiMenuSpec PhiBuildBookmarkRowMenu(const PhiBookmarkMenuInputs& in);

// The bookmark BAR's bare run (the Mac BookmarkBar's own menu(for:)): root
// folder creation only; a pill gets the bookmark row's menu above.
PhiMenuSpec PhiBuildBookmarkBarAreaMenu(bool incognito);

// The tab AREA's menu (the Mac's TabAreaContextMenuHelper): new tab, reopen,
// the bookmark block, the layout switch, then the active Space's controls.
struct PhiTabAreaMenuInputs {
  // Nothing persists here: an incognito window or an Incognito Space.
  bool incognito = false;
  // The active Space is an Incognito Space (docs/incognito-space-port.md):
  // the Space block stays — rename, theme and profile read but are off —
  // and "Close Incognito Space" trails it (the Mac's
  // appendActiveSpaceMenuItems). An incognito WINDOW has no Space block.
  bool incognito_space = false;
  bool tabs_empty = false;
  PhiLayoutMode mode = PhiLayoutMode::kPerformance;
  bool can_delete_space = false;
  // The default Space's window cannot be re-profiled (a fresh shell's first
  // browser lands there): its rows read the binding out but stay disabled.
  bool can_reprofile = false;
  std::string active_profile;
  std::vector<PhiMenuChoice> profiles;
  // The Space block alone (docs/windows-native-ui.md §5.4): a right-click
  // on a Space pip or the Space chip — rename, icon, theme, profile,
  // delete — without the tab-area rows.
  bool space_only = false;
};
PhiMenuSpec PhiBuildTabAreaMenu(const PhiTabAreaMenuInputs& in);

// --- The Spaces strip's menus ---------------------------------------------

// An agent pip's menu (the Mac agentContextMenu).
PhiMenuSpec PhiBuildAgentChipMenu();

// The full Space switcher: the compact pip's (with_new_space false) and the
// overflow "…" button's (true, which adds the two creation rows).
// `agents` are the live agent Spaces (the Mac draws them in the switcher
// with the driver's icon), each row an "agent.watch" of its taskId;
// `watched_agent` is ticked.
PhiMenuSpec PhiBuildSpaceSwitcherMenu(const std::vector<PhiMenuChoice>& spaces,
                                      const std::vector<PhiMenuChoice>& agents,
                                      const std::string& watched_agent,
                                      const std::string& active_space,
                                      bool with_new_space,
                                      bool with_new_profile);

// --- The sidebar's profile button -----------------------------------------

// The avatar button's menu (the Mac's populateProfileMenu): Download List,
// Manage Bookmarks, Manage Extensions, ─, New Tab, New Incognito Space, New
// Incognito Window, ─, Settings, and Sign in while no account is signed in.
// The chords are the user's EFFECTIVE ones (the ⋯ menu's
// PhiAppMenu::Label rule), so the shell resolves them and passes them in;
// empty leaves the row without one.
struct PhiProfileMenuInputs {
  std::wstring downloads_chord;
  std::wstring bookmarks_chord;
  std::wstring extensions_chord;
  std::wstring new_tab_chord;
  std::wstring new_incognito_space_chord;
  std::wstring new_incognito_window_chord;
  std::wstring settings_chord;
  bool signed_in = false;  // True hides the Sign in row.
};
PhiMenuSpec PhiBuildProfileMenu(const PhiProfileMenuInputs& in);

// --- The bookmark bar's dropdown ------------------------------------------

// A snapshot of the store's bookmark outline, taken before the menu opens.
struct PhiMenuBookmarkNode {
  std::string guid;
  std::string url;
  std::wstring title;  // The row's title, or its address when untitled.
  bool folder = false;
  std::vector<PhiMenuBookmarkNode> children;
};
// A bar folder's children (`skip_first` 0) or the root's overflow
// (`skip_first` = the pills already on the bar). Folders nest four deep and
// the whole menu stops at 200 leaves, as the bar's own dropdown always has.
PhiMenuSpec PhiBuildBookmarkBarMenu(
    const std::vector<PhiMenuBookmarkNode>& roots,
    size_t skip_first);

// --- The address bar's site menu ------------------------------------------

// What the Mac's WebContentAddressBarMenu shows for the active tab
// (framework SiteMenuJson plus the client pref).
struct PhiSiteMenuInputs {
  // "Content blocking on this site": the row exists, and its check.
  bool content_blocking_row = false;
  bool content_blocking_on = false;
  bool always_show_url_path = false;
  // The security row: "secure" | "partial" | "insecure" | "unknown", or
  // empty for none (Chromium's and Phi's own pages).
  std::string security;
};
// Site Settings ▸ (Clear Cache · Clear Cookie · ─ · More Settings),
// Content blocking on this site, ─, Always Show URL Path, then ─ and the
// security row. The Mac's leading Share ▸ is not ported yet.
PhiMenuSpec PhiBuildSiteMenu(const PhiSiteMenuInputs& in);

// --- Showing one ----------------------------------------------------------

#if defined(_WIN32)
struct PhiPopupMenuRequest {
  HWND owner = nullptr;
  int x = 0;  // Screen coordinates, as TrackPopupMenu wants them.
  int y = 0;
  UINT align = TPM_LEFTALIGN | TPM_TOPALIGN;
  // > 0: every item carrying a glyph gets it rasterized at this pixel size
  // and set as the row's bitmap (Space icons).
  int glyph_px = 0;
  // The flyout ground and inks (docs/windows-native-ui.md §3.5): every row
  // is owner-drawn to the Fluent look through phi_menu_paint, and the
  // owner's WM_DRAWITEM paints it. Null keeps the system look.
  RAW_PTR_EXCLUSION const PhiPalette* palette = nullptr;
};

// Builds the HMENU tree, tracks it under a PhiModalScope (so the coordinator
// parks pipe verbs and the shell parks its own close), destroys it, and
// answers with the chosen item's key and target — BY VALUE, so the caller
// holds nothing that a rebuild under the loop could have invalidated.
PhiMenuAction PhiRunPopupMenu(const PhiMenuSpec& spec,
                              const PhiPopupMenuRequest& request);
#endif  // defined(_WIN32)

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SHELL_MENUS_H_

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_APP_MENU_H_
#define PHIBROWSER_WIN_CLIENT_PHI_APP_MENU_H_

#include <windows.h>

#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_keyboard_accelerators.h"
#include "phibrowser-win/client/phi_menu_paint.h"
#include "phibrowser-win/client/phi_theme.h"

namespace phi_framework {
class PhiFramework;
}  // namespace phi_framework

namespace phi_client {

class ChromePrefsHost;
struct PhiAgentPipView;
class PhiWindowModel;
class SidebarHost;
class SpacesHost;
class WindowHost;

// Menu command ids (WM_COMMAND). In the header so the keyboard-accelerator
// hook can post the same commands the menus fire.
enum : UINT {
  // Phi
  kCmdAbout = 1001,
  kCmdPreferences,
  kCmdCheckUpdate,
  kCmdDeleteBrowsingData,
  kCmdImportBrowser,
  kCmdQuit,
  // File
  kCmdNewTab,
  kCmdNewWindow,
  kCmdNewIncognitoWindow,
  kCmdNewIncognitoSpace,
  kCmdReopenClosedTab,
  kCmdOpenFile,
  kCmdOpenLocation,
  kCmdCloseWindow,
  kCmdCloseTab,
  kCmdSavePageAs,
  kCmdPrint,
  // Edit
  kCmdUndo,
  kCmdRedo,
  kCmdCut,
  kCmdCopy,
  kCmdCopyUrl,
  kCmdPaste,
  kCmdPasteMatchStyle,
  kCmdDelete,
  kCmdSelectAll,
  kCmdSearchWeb,
  kCmdFind,
  kCmdFindNext,
  kCmdFindPrevious,
  // View
  kCmdStop,
  kCmdReload,
  kCmdForceReload,
  kCmdFullScreen,
  kCmdActualSize,
  kCmdZoomIn,
  kCmdZoomOut,
  kCmdViewSource,
  kCmdDevTools,
  kCmdInspect,
  kCmdJsConsole,
  kCmdLayoutHeader,
  kCmdLayoutBalanced,
  kCmdLayoutPerformance,
  kCmdLayoutComfortable,
  kCmdAlwaysShowBookmarkBar,
  kCmdBookmarkBarOnNewTab,
  kCmdToggleSidebar,
  kCmdToggleChatbar,
  kCmdNewConversation,
  // Spaces
  kCmdNewSpace,
  kCmdRenameSpace,
  kCmdChangeIcon,
  // Opens the Set Theme panel on the active Space (the Mac's Edit Theme,
  // which presents SpaceThemeEditorView rather than a list of themes).
  kCmdEditSpaceTheme,
  kCmdDeleteSpace,
  kCmdNextSpace,
  kCmdPreviousSpace,
  // Switch to the Nth Space — the Ctrl+1..9 the Spaces menu advertises,
  // delivered by the coordinator's keyboard hook (the Mac's ⌃1..⌃9).
  kCmdSpace1,
  kCmdSpace2,
  kCmdSpace3,
  kCmdSpace4,
  kCmdSpace5,
  kCmdSpace6,
  kCmdSpace7,
  kCmdSpace8,
  kCmdSpace9,
  kCmdUrlRules,
  kCmdNewProfile,
  // History
  kCmdHome,
  kCmdBack,
  kCmdForward,
  kCmdRecentlyClosedHeader,
  kCmdShowFullHistory,
  // Bookmarks
  kCmdBookmarkThisTab,
  kCmdBookmarkAllTabs,
  kCmdExportBookmarks,
  kCmdBookmarkManager,
  // Tab
  kCmdNewTabToRight,
  kCmdNextTab,
  kCmdPreviousTab,
  kCmdDuplicateTab,
  kCmdMuteSite,
  kCmdSearchTabs,
  // Window
  kCmdMinimize,
  kCmdZoomWindow,
  kCmdNameWindow,
  kCmdDownloads,
  kCmdExtensions,
  kCmdTaskManager,
  // Help
  kCmdReportIssue,
  kCmdWhatsNew,
  kCmdDocumentation,
  kCmdExportUserData,
  kCmdImportUserData,
  // APPEND ONLY below this line: the box test rigs post these ids as raw
  // WM_COMMAND numbers, so renumbering an existing entry silently rewires
  // their clicks.
  kCmdToggleReader,  // View — Reader View for the active tab.
  kCmdAgentAutoview,  // View — Agent Autoview (follows the working agent).
  // The app menu itself (docs/windows-native-ui.md §3.3): F10, Alt+F,
  // Alt+E and the solo Alt tap post it; the shell drops the menu under
  // its ⋯ button.
  kCmdShowAppMenu,
  // Extensions ▸ Manage extensions (chrome://extensions in a new tab).
  kCmdManageExtensions,
  // Spaces ▸ Close Incognito Space — the Mac Spaces menu's row, present
  // while an Incognito Space is the active one.
  kCmdCloseIncognitoSpace,
  // Folio (docs/folio-port.md): save the active page as Markdown + MHTML,
  // and open the library (Mirage's library.html). The Mac's File menu rows.
  kCmdSaveToFolio,
  kCmdOpenFolio,
  // Layout ▸ Show sidebar — the same toggle as kCmdToggleSidebar, listed
  // under its own name so the remote surface can address the row.
  kCmdShowBookmarksBar = kCmdAlwaysShowBookmarkBar,
};

// One finished sidebar row (Next / Previous tab walk them). A SNAPSHOT of what the
// last rebuild placed: a menu opening is not a reason to rebuild
// (docs/architecture-review.md §3.1 principle 3; phase 2 made rebuilds
// posted).
struct PhiAppMenuTab {
  TabId id;
  std::wstring title;
  bool active = false;
};

// The navigation state the View, History and Bookmarks menus enable from.
struct PhiAppMenuNavState {
  std::wstring url;
  bool can_go_back = false;
  bool can_go_forward = false;
  bool loading = false;
};

// WHAT THE MENU ASKS OF ITS SHELL (docs/architecture-review.md §3.5, lane 1
// §2.1 item 3): nav state, the finished rows, the active Space, the address
// field, and command dispatch. No `friend`, and nothing here reaches back
// into a cache the menu could outlive. (PhiMenuBarShell, renamed unchanged
// when the menu bar became the app menu — windows-native-ui.md §10 P1.)
class PhiAppMenuShell {
 public:
  // --- Identity and services ----------------------------------------------
  virtual HWND MenuShellWindow() const = 0;
  virtual SlotId MenuSlot() const = 0;
  virtual bool MenuIncognito() const = 0;
  // The bar genuinely spans bookmarks, Spaces, profiles and prefs — but not
  // the agent's: it takes those FOUR narrow services, never the set
  // (docs/architecture-review.md §3.5, lane 1 §4.3). Any of them may be
  // null, and the same guards that used to test one host now test the one
  // the item about to run needs.
  virtual SidebarHost* MenuSidebarHost() const = 0;
  virtual SpacesHost* MenuSpacesHost() const = 0;
  virtual ChromePrefsHost* MenuPrefsHost() const = 0;
  virtual WindowHost* MenuWindowHost() const = 0;
  virtual phi_framework::PhiFramework* MenuFramework() const = 0;
  virtual const PhiWindowModel* MenuModel() const = 0;
  // The browser the slot SHOWS — invalid when none is attached, and NEVER
  // the slot's own number.
  virtual BrowserId MenuActiveBrowser() const = 0;
  // The TREE's active tab, not the active ROW: a tab bound to a pinned cell
  // or a bookmark is no row in the list, and Ctrl+W must still close it.
  virtual TabId MenuActiveTab() const = 0;
  // The page PAGE commands act on — Reload, Stop, Zoom, Print, Save Page,
  // View Source, DevTools, Find, Copy URL, Bookmark, Save to Folio: the
  // browser of the Peek presented over the active tab, else
  // MenuActiveBrowser() (the Mac's pageCommandTarget; PhiShellWindow::
  // PageCommandBrowser). Tab and window commands stay on MenuActiveBrowser.
  virtual BrowserId MenuPageCommandBrowser() const = 0;
  // That page's tab — the presented Peek's own, else MenuActiveTab().
  virtual TabId MenuPageCommandTab() const = 0;
  virtual std::string MenuActiveSpace() const = 0;
  virtual PhiPalette MenuPalette() const = 0;
  virtual int MenuScaled(int design_units) const = 0;

  // --- The omnibox --------------------------------------------------------
  // The address field's EDIT (null before the shell has one): Open Location
  // and the Edit menu's text commands operate on it.
  virtual HWND MenuAddressField() const = 0;
  // Focus the active page before Chromium opens its owned Find popup.
  virtual void MenuFocusPageForFind() = 0;
  // Open Location: focuses the field, selected whole — raising the floating
  // sidebar first when the collapsed performance shell has no field of its
  // own (the panel's is the one to type in).
  virtual void MenuFocusAddressField() = 0;
  // Re-reads navigation state from the window model. NOT a row rebuild —
  // the rows a menu shows are always the finished ones.
  virtual void MenuRefreshNavState() = 0;
  virtual PhiAppMenuNavState MenuNavState() const = 0;

  // --- The finished rows --------------------------------------------------
  virtual std::vector<PhiAppMenuTab> MenuTabRows() const = 0;

  // --- Reader View --------------------------------------------------------
  virtual bool MenuReaderActive() const = 0;
  virtual bool MenuReaderOfferable() const = 0;
  // Back/Forward over a tab the reader covers means "leave the reader".
  virtual bool MenuLeaveReaderForNavigation() = 0;

  // --- Commands the bar hands back ----------------------------------------
  // True while an agent holds this (agent) window: the tab, navigation
  // and bookmark commands are refused (see PhiShellWindow::AgentHoldsInput).
  virtual bool MenuAgentHoldsInput() const = 0;
  // The agent Spaces this window's switchers offer, after the user Spaces
  // — the Mac's slot.presentedSpaces, which the Spaces menu rows, Ctrl+N
  // and Next/Previous Space index into (empty in agent and incognito
  // windows). See PhiAgentPipView (phi_shell_window.h).
  virtual std::vector<PhiAgentPipView> MenuAgentSpaces() const = 0;
  // A Spaces menu row / Ctrl+N pick: PhiShellWindow::SwitchToSpace.
  virtual void MenuSwitchSpace(const std::string& space_id) = 0;
  virtual void MenuNewTab() = 0;
  virtual void MenuToggleSidebar() = 0;
  // `from_accelerator`: the chord rather than the View menu row (the
  // reader's usage metric names the two differently).
  virtual void MenuToggleReader(bool from_accelerator) = 0;
  virtual void MenuToggleDownloads() = 0;
  virtual void MenuToggleSearchTabs() = 0;
  virtual void MenuOpenFeedback() = 0;
  // Ctrl+W on an Incognito Space's last tab ends the Space instead (asked
  // first). True when it took the close — whatever the answer was.
  virtual bool MenuCloseTabEndsIncognitoSpace() = 0;
  // Ends the active Incognito Space (asked first).
  virtual void MenuCloseIncognitoSpace() = 0;
  virtual void MenuCreateSpace() = 0;
  // The Mac's Edit Theme: the panel drops under the caption, centred on the
  // window (the menu bar has no trigger point to hang it off).
  virtual void MenuEditSpaceTheme() = 0;
  // "Change icon…": the §5.9 picker flyout under the ⋯ button.
  virtual void MenuChangeSpaceIcon() = 0;
  // Extensions ▸ <name>: runs the action as a click on its toolbar button
  // would, anchored to the app-menu button.
  virtual void MenuTriggerExtension(const std::string& extension_id) = 0;
  // Whether the sidebar is currently shown (Layout ▸ Show sidebar's check).
  virtual bool MenuSidebarShown() const = 0;

 protected:
  // The shell owns the menu, never the reverse.
  ~PhiAppMenuShell() = default;
};

// The "Settings and more" menu (docs/windows-native-ui.md §3): Chrome's and
// Edge's ⋯ flyout, which replaced the Mac-style menu bar on Windows. ONE
// popup HMENU tree — New tab / New window / New incognito window; Spaces ▸,
// History ▸, Bookmarks ▸, Downloads, Extensions ▸; Zoom ▸, Print, Find,
// Reader view, Layout ▸, More tools ▸; Settings, Help ▸, Exit — every row
// owner-drawn to the Fluent look through phi_menu_paint. The dynamic
// submenus (Spaces, History, Bookmarks, Extensions) and every enable state
// are rebuilt as their menu drops down (WM_INITMENUPOPUP), the Win32
// equivalent of the Mac's menuWillOpen. Items whose feature has no Windows
// surface yet stay listed and disabled.
class PhiAppMenu {
 public:
  explicit PhiAppMenu(PhiAppMenuShell* shell);
  ~PhiAppMenu();

  PhiAppMenu(const PhiAppMenu&) = delete;
  PhiAppMenu& operator=(const PhiAppMenu&) = delete;

  // Builds the tree. Commands route to `shell` as WM_COMMAND (no
  // TPM_RETURNCMD), so the click path and the accelerator path are one.
  void Install(HWND shell);

  // Queued shortcuts carry the originating presentation, never just an id.
  // Stale commands are discarded; no replay into a newly attached Space.
  static bool PostAccelerator(HWND shell,
                              UINT command,
                              BrowserId find_browser = BrowserId{});

  // Drops the menu with its top-right corner at SCREEN (x, y) — under the
  // ⋯ button's trailing edge — or top-left when `right_align` is false.
  // Runs the modal loop; returns when it is down.
  void Show(int screen_x, int screen_y, bool right_align);
  // The menu is up right now (the button paints pressed meanwhile).
  bool open() const { return open_; }

  // WM_INITMENUPOPUP: refresh `menu` if it is one of ours.
  void OnMenuOpening(HMENU menu);
  // WM_COMMAND with lparam == 0. True when `id` was a menu command.
  // `from_accelerator` is WM_COMMAND's HIWORD 1: the chord, not the row.
  bool OnCommand(UINT id, bool from_accelerator = false);

  // The Space block's Rename… and Delete… for `space_id`. The menu's own
  // rows pass the active Space; a pip's right-click passes the pip's, which
  // a switch to a cold Space has not made active yet (release review).
  void RenameSpace(const std::string& space_id);
  void DeleteSpace(const std::string& space_id);

  // The whole tree as JSON (refreshing dynamic menus first), for the
  // control socket: [{"title","enabled","checked","id"?,"items"?},…] —
  // the root's rows, submenus nested under "items".
  std::string ListJson();
  // Activates the item reached by `path` (row titles from the root, e.g.
  // ["More tools","Task manager"]; mnemonics and chords stripped). False
  // when no such item exists or it is disabled.
  bool InvokePath(const std::vector<std::string>& path);

 private:
  static LRESULT CALLBACK AcceleratorProc(HWND hwnd,
                                          UINT message,
                                          WPARAM wparam,
                                          LPARAM lparam,
                                          UINT_PTR subclass_id,
                                          DWORD_PTR data);
  void DispatchAccelerator(UINT_PTR token);
  PhiKeyboardCommandQueue pending_accelerators_;
  void BuildStatic();
  void RefreshRoot();
  void RefreshSpaces();
  void RefreshHistory();
  void RefreshBookmarks();
  void RefreshExtensions();
  void RefreshLayout();
  void RefreshTools();
  void RefreshHelp();
  // Re-reads the live enable state of every Chromium-routed item (the
  // kIdcRoutes table) from the framework — run on every menu open.
  void RefreshChromiumCommands();
  // The Netscape bookmark HTML the Mac exporter writes: a Favorites folder
  // of the pinned rows first, then the Space's bookmark tree.
  std::string BuildBookmarksHtml() const;
  // Rebuilds the bookmark rows of `menu` from the store's rows under
  // `parent_guid` — folders recurse into submenus, as on Mac.
  void AppendBookmarkRows(HMENU menu, const std::string& parent_guid,
                          int depth);
  // Every Space from the owner's full list, then this window's agent
  // Spaces. The title is the bare name; the glyph rides beside it.
  struct SpaceEntry {
    std::string id;
    std::wstring title;
    std::wstring glyph;
    bool agent = false;
  };
  std::vector<SpaceEntry> SpaceList() const;
  // A command row's text: the catalogue string for `key`, its "\tChord"
  // suffix replaced by the command's EFFECTIVE chord when `id` is a
  // rebindable shortcut (ChromePrefsHost::RebindableChordLabel) — the Mac
  // rebuilds its menu after every change; dropped while it is unbound.
  std::wstring Label(UINT id, const char* key) const;
  void SetEnabled(HMENU menu, UINT id, bool enabled);
  void SetChecked(HMENU menu, UINT id, bool checked);

  RAW_PTR_EXCLUSION PhiAppMenuShell* const services_;  // Owns this.
  PhiMenuRowStore rows_;
  HWND shell_ = nullptr;  // Owner for TrackPopupMenu (commands route here).
  HMENU root_ = nullptr;
  HMENU spaces_ = nullptr;
  HMENU profiles_ = nullptr;
  HMENU delete_profiles_ = nullptr;
  HMENU history_ = nullptr;
  HMENU bookmarks_ = nullptr;
  HMENU extensions_ = nullptr;
  HMENU zoom_ = nullptr;
  HMENU layout_ = nullptr;
  HMENU tools_ = nullptr;
  HMENU help_ = nullptr;
  bool open_ = false;

  // What the dynamic ids currently mean, rebuilt with their menus.
  std::vector<std::string> space_ids_;     // kSpaceFirst + i
  std::vector<std::string> profile_ids_;   // kProfileFirst / kDeleteProfileFirst + i
  std::vector<std::wstring> profile_names_;  // Their display names, in step.
  std::vector<std::string> bookmark_urls_; // kBookmarkFirst + i
  std::vector<std::string> extension_ids_;  // kExtensionFirst + i
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_APP_MENU_H_

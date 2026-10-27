// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_features.h"
#include "phibrowser-win/client/phi_app_menu.h"

#include "phibrowser-win/client/phi_app_model.h"

#include "phibrowser-win/client/phi_bookmark_manager.h"

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <optional>
#include <utility>

#pragma comment(lib, "comdlg32.lib")

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/app/chrome_command_ids.h"
#include "phibrowser-win/client/phi_fluent.h"
#include "phibrowser-win/client/phi_folio.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_mac_icons.h"
#include "phibrowser-win/client/phi_modal_scope.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_updater.h"
#include "phibrowser-win/client/phi_version.h"
#include "phibrowser-win/client/phi_toast.h"
#include "phibrowser-win/client/phi_theme.h"
// The four narrow host interfaces and PhiWindowModel still live in the
// shell's header. The menu reaches nothing else in it — no `friend`, no
// private member: everything else comes through PhiAppMenuShell.
#include "phibrowser-win/client/phi_shell_window.h"

namespace phi_client {

namespace {

constexpr UINT_PTR kAcceleratorSubclass = 0x50484941;
UINT AcceleratorMessage() {
  static const UINT message =
      ::RegisterWindowMessageW(L"Phi.AppMenu.AttributedAccelerator");
  return message;
}

// Dynamic ranges. Nothing overlaps the static kCmd* ids.
constexpr UINT kSpaceFirst = 2000;       // + index into space_ids_
constexpr UINT kSpaceLast = 2099;
constexpr UINT kProfileFirst = 2200;     // Change profile rows
constexpr UINT kProfileLast = 2299;
constexpr UINT kDeleteProfileFirst = 2300;  // Delete profile rows
constexpr UINT kDeleteProfileLast = 2399;
constexpr UINT kExtensionFirst = 2500;   // + index into extension_ids_
constexpr UINT kExtensionLast = 2699;
constexpr UINT kBookmarkFirst = 3000;    // + index into bookmark_urls_
constexpr UINT kBookmarkLast = 3499;

// The flyout's width (docs/windows-native-ui.md §3.1).
constexpr int kRootWidth = 320;

// Menu items that route straight through the framework's generic Chromium
// command wire — features Chromium already implements; the hollowed chrome
// only lacked the wire. Enablement mirrors the live command state on every
// menu open, so these behave exactly like Chromium's own menu items.
// `page` marks a PAGE command — one that acts on the page in front of the
// user, the presented Peek's when one is up (MenuPageCommandBrowser, the
// Mac's pageCommandTarget); the rest are tab and window commands and stay on
// the strip's browser (MenuActiveBrowser).
struct IdcRoute {
  UINT cmd;
  int idc;
  bool page = false;
};
constexpr IdcRoute kIdcRoutes[] = {
    {kCmdDeleteBrowsingData, IDC_CLEAR_BROWSING_DATA},
    {kCmdNewIncognitoWindow, IDC_NEW_INCOGNITO_WINDOW},
    {kCmdReopenClosedTab, IDC_RESTORE_TAB},
    {kCmdOpenFile, IDC_OPEN_FILE},
    {kCmdSavePageAs, IDC_SAVE_PAGE, true},
    {kCmdPrint, IDC_PRINT, true},
    {kCmdFind, IDC_FIND, true},
    // IDC_FULLSCREEN is NOT routed: BrowserView fullscreens the reparented
    // CHILD inside the shell (chrome vanishes, nothing truly fullscreens)
    // and the exit leaves the child mispositioned. Fullscreen needs the
    // SHELL to drive it (monitor-sized borderless shell + child relayout).
    {kCmdActualSize, IDC_ZOOM_NORMAL, true},
    {kCmdZoomIn, IDC_ZOOM_PLUS, true},
    {kCmdZoomOut, IDC_ZOOM_MINUS, true},
    {kCmdViewSource, IDC_VIEW_SOURCE, true},
    {kCmdDevTools, IDC_DEV_TOOLS, true},
    {kCmdInspect, IDC_DEV_TOOLS_INSPECT, true},
    {kCmdJsConsole, IDC_DEV_TOOLS_CONSOLE, true},
    {kCmdHome, IDC_HOME},
    {kCmdShowFullHistory, IDC_SHOW_HISTORY},
    {kCmdDuplicateTab, IDC_DUPLICATE_TAB},
    {kCmdMuteSite, IDC_WINDOW_MUTE_SITE},
    {kCmdTaskManager, IDC_TASK_MANAGER},
};

const IdcRoute* RouteForCommand(UINT id) {
  for (const IdcRoute& route : kIdcRoutes) {
    if (route.cmd == id) {
      return &route;
    }
  }
  return nullptr;
}

}  // namespace

PhiAppMenu::PhiAppMenu(PhiAppMenuShell* shell) : services_(shell) {}

PhiAppMenu::~PhiAppMenu() {
  if (shell_) {
    ::RemoveWindowSubclass(shell_, &AcceleratorProc, kAcceleratorSubclass);
  }
  // Never attached, so the tree is ours to destroy — it takes every
  // submenu with it; the rows go with the store.
  if (root_) {
    ::DestroyMenu(root_);
  }
}

void PhiAppMenu::SetEnabled(HMENU menu, UINT id, bool enabled) {
  ::EnableMenuItem(menu, id,
                   MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_DISABLED |
                                                              MF_GRAYED));
}

void PhiAppMenu::SetChecked(HMENU menu, UINT id, bool checked) {
  ::CheckMenuItem(menu, id, MF_BYCOMMAND | (checked ? MF_CHECKED
                                                     : MF_UNCHECKED));
}

void PhiAppMenu::Install(HWND shell) {
  if (root_) {
    return;
  }
  shell_ = shell;
  ::SetWindowSubclass(shell_, &AcceleratorProc, kAcceleratorSubclass,
                      reinterpret_cast<DWORD_PTR>(this));
  root_ = ::CreatePopupMenu();
  if (!root_) {
    return;
  }
  spaces_ = ::CreatePopupMenu();
  profiles_ = ::CreatePopupMenu();
  delete_profiles_ = ::CreatePopupMenu();
  history_ = ::CreatePopupMenu();
  bookmarks_ = ::CreatePopupMenu();
  extensions_ = ::CreatePopupMenu();
  zoom_ = ::CreatePopupMenu();
  layout_ = ::CreatePopupMenu();
  tools_ = ::CreatePopupMenu();
  help_ = ::CreatePopupMenu();
  BuildStatic();
}

void PhiAppMenu::Show(int screen_x, int screen_y, bool right_align) {
  if (!root_ || !shell_) {
    return;
  }
  PhiMenuApplyTheme(root_, services_->MenuPalette());
  // WM_INITMENUPOPUP fires for the root and every submenu exactly as it
  // would for an attached bar, so the dynamic rows refresh on the way
  // down. Commands arrive as WM_COMMAND on the shell. The scope makes the
  // coordinator defer pipe verbs for as long as the menu loop runs.
  PhiModalScope modal;
  open_ = true;
  ::TrackPopupMenu(root_,
                   (right_align ? TPM_RIGHTALIGN : TPM_LEFTALIGN) |
                       TPM_TOPALIGN | TPM_VERTICAL,
                   screen_x, screen_y, 0, shell_, nullptr);
  open_ = false;
}

std::wstring PhiAppMenu::Label(UINT id, const char* key) const {
  std::wstring text = PhiStr(key);
  ChromePrefsHost* prefs = services_->MenuPrefsHost();
  const std::optional<std::wstring> chord =
      prefs ? prefs->RebindableChordLabel(id) : std::nullopt;
  if (!chord) {
    return text;  // Not rebindable: the catalogue's own chord stands.
  }
  const size_t tab = text.find(L'\t');
  if (tab != std::wstring::npos) {
    text.erase(tab);
  }
  if (!chord->empty()) {
    text += L'\t' + *chord;
  }
  return text;
}

void PhiAppMenu::BuildStatic() {
  // The root, top to bottom (docs/windows-native-ui.md §3.1). Chords ride
  // the catalogue strings ("\tCtrl+T") and the painter right-aligns them;
  // Label() swaps in the user's chord for a rebindable command.
  auto row = [&](HMENU menu, UINT id, const char* key, const wchar_t* icon,
                 bool enabled = true) {
    PhiMenuRow* r =
        rows_.AppendCommand(menu, id, Label(id, key), icon, enabled);
    if (menu == root_) {
      r->min_width_dip = kRootWidth;
    }
    return r;
  };
  auto sub = [&](HMENU menu, HMENU child, const char* key,
                 const wchar_t* icon) {
    PhiMenuRow* r = rows_.AppendSubmenu(menu, child, PhiStr(key), icon);
    if (menu == root_) {
      r->min_width_dip = kRootWidth;
    }
    return r;
  };
  row(root_, kCmdNewTab, "appMenu.newTab", fluent::kAdd);
  row(root_, kCmdNewWindow, "appMenu.newWindow", fluent::kNewWindow);
  row(root_, kCmdNewIncognitoWindow, "appMenu.newIncognitoWindow",
      fluent::kIncognito, false);
  // The Mac's File ▸ New Incognito Space (docs/incognito-space-port.md):
  // joins this window's Spaces, unlike the incognito window above it.
  row(root_, kCmdNewIncognitoSpace, "appMenu.newIncognitoSpace", nullptr);
  rows_.AppendSeparator(root_);
  sub(root_, spaces_, "appMenu.spaces", fluent::kTabs);
  sub(root_, history_, "appMenu.history", fluent::kHistory);
  sub(root_, bookmarks_, "appMenu.bookmarks", fluent::kFavoriteStar);
  row(root_, kCmdDownloads, "appMenu.downloads", fluent::kDownload);
  sub(root_, extensions_, "appMenu.extensions", fluent::kPuzzle);
  rows_.AppendSeparator(root_);
  sub(root_, zoom_, "appMenu.zoom", fluent::kZoomIn);
  row(root_, kCmdPrint, "appMenu.print", fluent::kPrint, false);
  row(root_, kCmdFind, "appMenu.findOnPage", fluent::kFind, false);
  row(root_, kCmdToggleReader, "appMenu.readerView", fluent::kReadingMode);
  // Folio (docs/folio-port.md): the Mac's File ▸ Save to Folio / Open Folio.
  if (PhiFolio::Enabled()) {
    row(root_, kCmdSaveToFolio, "appMenu.saveToFolio", fluent::kSaveLocal);
    row(root_, kCmdOpenFolio, "appMenu.openFolio", fluent::kLibrary);
  }
  sub(root_, layout_, "appMenu.layout", fluent::kView);
  sub(root_, tools_, "appMenu.moreTools", fluent::kDeveloperTools);
  rows_.AppendSeparator(root_);
  row(root_, kCmdPreferences, "appMenu.settings", fluent::kSettings);
  sub(root_, help_, "appMenu.help", fluent::kHelp);
  row(root_, kCmdQuit, "appMenu.exit", fluent::kPowerButton);

  // Zoom ▸ (impl log D7: a submenu, not Chrome's inline row).
  row(zoom_, kCmdZoomIn, "appMenu.zoomIn", fluent::kZoomIn, false);
  row(zoom_, kCmdZoomOut, "appMenu.zoomOut", fluent::kZoomOut, false);
  row(zoom_, kCmdActualSize, "appMenu.actualSize", nullptr, false);
  rows_.AppendSeparator(zoom_);
  row(zoom_, kCmdFullScreen, "appMenu.fullScreen", fluent::kFullScreen, false);

  // History ▸: Back / Forward / Home, the recently-closed rows, full history.
  row(history_, kCmdBack, "appMenu.back", fluent::kBack);
  row(history_, kCmdForward, "appMenu.forward", fluent::kForward);
  row(history_, kCmdHome, "appMenu.home", fluent::kHome, false);
  rows_.AppendSeparator(history_);
  PhiMenuRow* recent = row(history_, kCmdRecentlyClosedHeader,
                           "appMenu.recentlyClosed", nullptr, false);
  recent->header = true;
  row(history_, kCmdReopenClosedTab, "appMenu.reopenClosedTab", fluent::kClock,
      false);
  rows_.AppendSeparator(history_);
  row(history_, kCmdShowFullHistory, "appMenu.showFullHistory",
      fluent::kHistory, false);

  // Layout ▸: the two layouts (radio), the sidebar, the bookmarks bar.
  row(layout_, kCmdLayoutComfortable, "appMenu.horizontalTabs", nullptr)
      ->radio = true;
  row(layout_, kCmdLayoutBalanced, "appMenu.verticalTabs", nullptr)->radio =
      true;
  rows_.AppendSeparator(layout_);
  row(layout_, kCmdToggleSidebar, "appMenu.showSidebar", fluent::kClosePane);
  row(layout_, kCmdAlwaysShowBookmarkBar, "appMenu.showBookmarksBar",
      nullptr);

  // More tools ▸.
  row(tools_, kCmdOpenFile, "appMenu.openFile", fluent::kOpenFile, false);
  row(tools_, kCmdSavePageAs, "appMenu.savePageAs", fluent::kSave, false);
  row(tools_, kCmdTaskManager, "appMenu.taskManager", fluent::kProcessing,
      false);
  rows_.AppendSeparator(tools_);
  row(tools_, kCmdViewSource, "appMenu.viewSource", fluent::kCode, false);
  row(tools_, kCmdDevTools, "appMenu.developerTools", fluent::kDeveloperTools,
      false);
  row(tools_, kCmdJsConsole, "appMenu.javascriptConsole", nullptr, false);
  row(tools_, kCmdInspect, "appMenu.inspectElements", nullptr, false);
  rows_.AppendSeparator(tools_);
  row(tools_, kCmdDeleteBrowsingData, "appMenu.deleteBrowsingData",
      fluent::kEraseTool, false);
  row(tools_, kCmdImportBrowser, "appMenu.importFromBrowser", fluent::kImport);
  row(tools_, kCmdExportUserData, "appMenu.exportUserData", fluent::kExport,
      false);
  row(tools_, kCmdImportUserData, "appMenu.importUserData", nullptr, false);
  rows_.AppendSeparator(tools_);
  row(tools_, kCmdAgentAutoview, "appMenu.agentAutoview", fluent::kRobot);

  // Help ▸.
  row(help_, kCmdAbout, "appMenu.about", fluent::kInfo);
  // Live only when WinSparkle.dll is deployed beside the client.
  row(help_, kCmdCheckUpdate, "appMenu.checkForUpdate", fluent::kSync,
      PhiUpdater::Available());
  row(help_, kCmdWhatsNew, "appMenu.whatsNew", fluent::kLightbulb, false);
  row(help_, kCmdDocumentation, "appMenu.documentation", fluent::kDocument,
      false);
  rows_.AppendSeparator(help_);
  // The sidebar Feedback pill's bug (Figma solar:bug-line-duotone), not
  // the icon face's Feedback glyph.
  row(help_, kCmdReportIssue, "appMenu.reportIssue", nullptr)->mask =
      &PhiFeedbackIcon();

  // Spaces / Bookmarks / Extensions hold dynamic rows; filled on open.
  RefreshSpaces();
  RefreshBookmarks();
  RefreshExtensions();
}

void PhiAppMenu::OnMenuOpening(HMENU menu) {
  if (menu == root_) {
    RefreshRoot();
  } else if (menu == spaces_) {
    RefreshSpaces();
  } else if (menu == history_) {
    RefreshHistory();
  } else if (menu == bookmarks_) {
    RefreshBookmarks();
  } else if (menu == extensions_) {
    RefreshExtensions();
  } else if (menu == layout_) {
    RefreshLayout();
  } else if (menu == tools_) {
    RefreshTools();
  } else if (menu == help_) {
    RefreshHelp();
  } else if (menu != zoom_) {
    return;  // A nested submenu; its parent's open already refreshed it.
  }
  RefreshChromiumCommands();
}

void PhiAppMenu::RefreshChromiumCommands() {
  const BrowserId window = services_->MenuActiveBrowser();
  const BrowserId page = services_->MenuPageCommandBrowser();
  for (const IdcRoute& route : kIdcRoutes) {
    // MF_BYCOMMAND searches submenus, so one call from the root reaches
    // items inside Zoom / More tools. No browser attached: everything off.
    // A page command's state is the page in front of the user's (the
    // presented peek's, when one is up) — the one it will run on.
    const BrowserId target = route.page ? page : window;
    const bool on = services_->MenuFramework() && target.valid() &&
                    services_->MenuFramework()->IsChromiumCommandEnabled(
                        target.value, route.idc);
    ::EnableMenuItem(
        root_, route.cmd,
        MF_BYCOMMAND | (on ? MF_ENABLED : MF_DISABLED | MF_GRAYED));
  }
}

void PhiAppMenu::RefreshRoot() {
  services_->MenuRefreshNavState();
  SetEnabled(root_, kCmdPreferences, services_->MenuWindowHost() != nullptr);
  // Reader View is offered where it has something to show, and ticked
  // while its surface is up.
  const bool reader_active = services_->MenuReaderActive();
  SetEnabled(root_, kCmdToggleReader,
             reader_active || services_->MenuReaderOfferable());
  SetChecked(root_, kCmdToggleReader, reader_active);
  // Save to Folio: an http(s) page with no save of it in flight — the page
  // in front of the user (the presented peek's, when one is up).
  SidebarHost* sidebar = services_->MenuSidebarHost();
  const TabId tab = services_->MenuPageCommandTab();
  SetEnabled(root_, kCmdSaveToFolio,
             sidebar && tab.valid() && sidebar->CanSaveToFolio(tab.value));
}

void PhiAppMenu::RefreshLayout() {
  PhiLayoutMode mode = PhiLayoutMode::kBalanced;
  if (services_->MenuPrefsHost()) {
    mode = services_->MenuPrefsHost()->LayoutMode();
  }
  const bool horizontal = mode == PhiLayoutMode::kComfortable;
  SetChecked(layout_, kCmdLayoutComfortable, horizontal);
  SetChecked(layout_, kCmdLayoutBalanced, !horizontal);
  // The sidebar exists in the vertical layout only; the bookmarks bar in
  // the horizontal one (the vertical keeps bookmarks in the sidebar).
  SetEnabled(layout_, kCmdToggleSidebar, !horizontal);
  SetChecked(layout_, kCmdToggleSidebar,
             !horizontal && services_->MenuSidebarShown());
  SetEnabled(layout_, kCmdAlwaysShowBookmarkBar, horizontal);
  SetChecked(layout_, kCmdAlwaysShowBookmarkBar,
             horizontal && services_->MenuPrefsHost() &&
                 services_->MenuPrefsHost()->AlwaysShowBookmarkBar());
}

void PhiAppMenu::RefreshTools() {
  SetEnabled(tools_, kCmdImportBrowser,
             services_->MenuFramework() != nullptr &&
                 !services_->MenuIncognito());
  if (ChromePrefsHost* prefs = services_->MenuPrefsHost()) {
    SetEnabled(tools_, kCmdAgentAutoview, prefs->AllowAgentControl());
    SetChecked(tools_, kCmdAgentAutoview, prefs->AgentAutoView());
  }
}

void PhiAppMenu::RefreshHelp() {
  SetEnabled(help_, kCmdCheckUpdate, PhiUpdater::Available());
}

std::vector<PhiAppMenu::SpaceEntry> PhiAppMenu::SpaceList() const {
  std::vector<SpaceEntry> spaces;
  if (!services_->MenuSpacesHost()) {
    return spaces;
  }
  for (const PhiSpaceView& space : services_->MenuSpacesHost()->Spaces()) {
    spaces.push_back(
        {space.id,
         !space.name.empty()
             ? base::UTF8ToWide(space.name)
             : PhiStrF("sidebar.createSpace.defaultName",
                       {base::UTF8ToWide(space.id)}),
         base::UTF8ToWide(space.icon_text), false});
  }
  // Then this window's agent Spaces, in strip order — the Mac's
  // presentedSpaces: the rows list them, and Ctrl+N and Next/Previous
  // Space reach them, past the user Spaces.
  for (const PhiAgentPipView& pip : services_->MenuAgentSpaces()) {
    spaces.push_back({pip.space_id, PhiAgentPipLabel(pip), pip.icon, true});
  }
  return spaces;
}

void PhiAppMenu::RefreshSpaces() {
  rows_.ClearMenu(spaces_);
  rows_.ClearMenu(profiles_);
  rows_.ClearMenu(delete_profiles_);
  space_ids_.clear();
  profile_ids_.clear();
  profile_names_.clear();
  SpacesHost* spaces_host = services_->MenuSpacesHost();
  const std::string active = services_->MenuActiveSpace();

  // An Incognito Space keeps its generated name, its fixed-dark theme and
  // its off-the-record session (docs/incognito-space-port.md).
  const bool incognito_space = model::IsIncognitoSpaceId(active);
  rows_.AppendCommand(spaces_, kCmdNewSpace, PhiStr("appMenu.newSpace"),
                      fluent::kAdd, spaces_host != nullptr);
  rows_.AppendSeparator(spaces_);
  rows_.AppendCommand(spaces_, kCmdRenameSpace, PhiStr("appMenu.renameSpace"),
                      fluent::kRename,
                      spaces_host != nullptr && !incognito_space);
  rows_.AppendCommand(spaces_, kCmdChangeIcon, PhiStr("appMenu.changeIcon"),
                      fluent::kEmoji, spaces_host != nullptr);
  // Edit theme — the Mac's item: it presents the Space theme editor (swatch
  // row + saturation slider) on the ACTIVE Space.
  rows_.AppendCommand(spaces_, kCmdEditSpaceTheme, PhiStr("appMenu.editTheme"),
                      fluent::kColor,
                      spaces_host != nullptr && !active.empty() &&
                          !incognito_space);

  // Change profile ▸ — the Mac's radio list: picking a row re-binds the
  // ACTIVE Space, respawning its window in the new profile. The default
  // Space's binding is fixed, so its rows stay disabled and only read the
  // binding out. Delete profile rows are guarded the Mac's way: Default
  // and any profile still bound to a Space refuse.
  std::string active_profile;
  std::vector<std::string> bound_profiles;
  if (spaces_host) {
    for (const PhiSpaceView& space : spaces_host->Spaces()) {
      bound_profiles.push_back(space.profile_id);
      if (space.id == active) {
        active_profile = space.profile_id;
      }
    }
  }
  const bool can_reprofile = spaces_host && !active.empty() &&
                             active != spaces_host->DefaultSpaceId() &&
                             !incognito_space;
  int profile_count = 0;
  if (spaces_host) {
    for (const std::string& summary : spaces_host->ProfileSummaries()) {
      const size_t tab = summary.find('\t');
      const std::string id = summary.substr(0, tab);
      const std::string name =
          tab == std::string::npos ? std::string() : summary.substr(tab + 1);
      if (id.empty() ||
          profile_count >= static_cast<int>(kProfileLast - kProfileFirst)) {
        continue;
      }
      // `title` stays raw for the confirm dialogs; the ROWS escape it (a
      // user-chosen name with "&" would otherwise grow a mnemonic).
      const std::wstring title = base::UTF8ToWide(!name.empty() ? name : id);
      PhiMenuRow* r = rows_.AppendCommand(
          profiles_, kProfileFirst + static_cast<UINT>(profile_count),
          PhiEscapeMenuText(title), nullptr, can_reprofile);
      r->radio = true;
      if (id == active_profile) {
        ::CheckMenuItem(profiles_, kProfileFirst + static_cast<UINT>(profile_count),
                        MF_BYCOMMAND | MF_CHECKED);
      }
      profile_names_.push_back(title);
      const bool in_use =
          std::find(bound_profiles.begin(), bound_profiles.end(), id) !=
          bound_profiles.end();
      const bool deletable = id != "Default" && !in_use;
      // The Mac marks in-use rows "<name> — in use by a Space".
      const std::wstring delete_title =
          in_use ? PhiStrF("menuBar.profileInUse", {PhiEscapeMenuText(title)})
                 : PhiEscapeMenuText(title);
      PhiMenuRow* d = rows_.AppendCommand(
          delete_profiles_,
          kDeleteProfileFirst + static_cast<UINT>(profile_count),
          delete_title, nullptr, deletable);
      d->danger = deletable;
      profile_ids_.push_back(id);
      ++profile_count;
    }
  }
  if (profile_count == 0) {
    rows_.AppendCommand(profiles_, 0, PhiStr("menuBar.noProfiles"), nullptr,
                        false);
    rows_.AppendCommand(delete_profiles_, 0,
                        PhiStr("app.spacesMenu.deleteProfile.emptyPlaceholder"),
                        nullptr, false);
  }
  rows_.AppendSubmenu(spaces_, profiles_, PhiStr("appMenu.changeProfile"),
                      fluent::kContact);
  // Any Space may go except the last one (the Mac's last-Space rule); the
  // default Space hands its role on when deleted.
  PhiMenuRow* del = rows_.AppendCommand(
      spaces_, kCmdDeleteSpace, PhiStr("appMenu.deleteSpace"), fluent::kDelete,
      spaces_host && spaces_host->CanDeleteSpace(active));
  del->danger = true;
  if (incognito_space) {
    // The Mac's closeIncognitoSpaceFromMenu row: Delete is off for a
    // runtime Space, so this is the menu's way to end it.
    rows_.AppendSeparator(spaces_);
    rows_.AppendCommand(spaces_, kCmdCloseIncognitoSpace,
                        PhiStr("app.spacesMenu.closeActiveIncognitoSpace"),
                        nullptr, spaces_host != nullptr);
  }
  rows_.AppendSeparator(spaces_);
  const std::vector<SpaceEntry> spaces = SpaceList();
  const bool several = spaces.size() > 1;
  rows_.AppendCommand(spaces_, kCmdNextSpace,
                      Label(kCmdNextSpace, "appMenu.nextSpace"), nullptr,
                      several);
  rows_.AppendCommand(spaces_, kCmdPreviousSpace,
                      Label(kCmdPreviousSpace, "appMenu.previousSpace"),
                      nullptr, several);
  rows_.AppendSeparator(spaces_);
  bool agent_divider = false;
  for (size_t i = 0; i < spaces.size() && kSpaceFirst + i <= kSpaceLast;
       ++i) {
    if (spaces[i].agent && !agent_divider) {
      rows_.AppendSeparator(spaces_);  // Agent Spaces after a divider.
      agent_divider = true;
    }
    // Escaped BEFORE the accelerator column joins it: the tab and the
    // shortcut are menu syntax, the name is the user's text.
    std::wstring title = PhiEscapeMenuText(spaces[i].title);
    if (i < 9) {
      title += L"\tCtrl+" + std::to_wstring(i + 1);
    }
    const UINT id = kSpaceFirst + static_cast<UINT>(i);
    PhiMenuRow* r = rows_.AppendCommand(spaces_, id, title);
    // The Space's icon as the row's picture: catalog artwork or colour
    // emoji, drawn by the painter.
    r->picture = spaces[i].glyph;
    if (spaces[i].id == active) {
      ::CheckMenuItem(spaces_, id, MF_BYCOMMAND | MF_CHECKED);
    }
    space_ids_.push_back(spaces[i].id);
  }
  rows_.AppendSeparator(spaces_);
  rows_.AppendCommand(spaces_, kCmdUrlRules, PhiStr("appMenu.urlRules"),
                      fluent::kLink);
  rows_.AppendSeparator(spaces_);
  rows_.AppendCommand(spaces_, kCmdNewProfile, PhiStr("appMenu.newProfile"),
                      fluent::kPeople, services_->MenuFramework() != nullptr);
  rows_.AppendSubmenu(spaces_, delete_profiles_,
                      PhiStr("appMenu.deleteProfile"), nullptr);
}

void PhiAppMenu::RefreshHistory() {
  services_->MenuRefreshNavState();
  const PhiAppMenuNavState nav = services_->MenuNavState();
  SetEnabled(history_, kCmdBack, nav.can_go_back);
  SetEnabled(history_, kCmdForward, nav.can_go_forward);
}

void PhiAppMenu::AppendBookmarkRows(HMENU menu, const std::string& parent_guid,
                                    int depth) {
  if (!services_->MenuSidebarHost() || depth > 5) {
    return;
  }
  for (const PhiStoredRowView& row :
       services_->MenuSidebarHost()->BookmarkRows(services_->MenuSlot().value,
                                                  parent_guid)) {
    const std::wstring shown = PhiEscapeMenuText(
        base::UTF8ToWide(!row.title.empty() ? row.title : row.url));
    if (row.folder) {
      HMENU sub = ::CreatePopupMenu();
      AppendBookmarkRows(sub, row.guid, depth + 1);
      if (::GetMenuItemCount(sub) == 0) {
        rows_.AppendCommand(sub, 0,
                            PhiStr("app.bookmarksMenu.emptyFolderPlaceholder"),
                            nullptr, false);
      }
      rows_.AppendSubmenu(menu, sub, shown, fluent::kFolder);
    } else if (!row.url.empty() &&
               bookmark_urls_.size() <= kBookmarkLast - kBookmarkFirst) {
      rows_.AppendCommand(
          menu, kBookmarkFirst + static_cast<UINT>(bookmark_urls_.size()),
          shown, fluent::kGlobe);
      bookmark_urls_.push_back(row.url);
    }
  }
}

void PhiAppMenu::RefreshBookmarks() {
  rows_.ClearMenu(bookmarks_);
  bookmark_urls_.clear();
  services_->MenuRefreshNavState();
  // The rows as the last rebuild placed them; a menu opening never asks for
  // a fresh one (docs/architecture-review.md §3.1 principle 3).
  const bool has_rows = !services_->MenuTabRows().empty();
  const bool store = services_->MenuSidebarHost() != nullptr;
  rows_.AppendCommand(bookmarks_, kCmdBookmarkThisTab,
                      Label(kCmdBookmarkThisTab, "appMenu.bookmarkThisTab"),
                      fluent::kFavoriteStar,
                      store && !services_->MenuNavState().url.empty() &&
                          !services_->MenuIncognito());
  rows_.AppendCommand(bookmarks_, kCmdBookmarkAllTabs,
                      Label(kCmdBookmarkAllTabs, "appMenu.bookmarkAllTabs"),
                      nullptr,
                      store && has_rows && !services_->MenuIncognito());
  rows_.AppendSeparator(bookmarks_);
  PhiLayoutMode mode = PhiLayoutMode::kBalanced;
  if (services_->MenuPrefsHost()) {
    mode = services_->MenuPrefsHost()->LayoutMode();
  }
  if (mode == PhiLayoutMode::kComfortable) {
    // The bookmarks bar is a horizontal-layout feature: the vertical layout
    // keeps bookmarks in the sidebar.
    rows_.AppendCommand(
        bookmarks_, kCmdAlwaysShowBookmarkBar,
        Label(kCmdAlwaysShowBookmarkBar, "appMenu.showBookmarksBar"), nullptr);
    SetChecked(bookmarks_, kCmdAlwaysShowBookmarkBar,
               services_->MenuPrefsHost() &&
                   services_->MenuPrefsHost()->AlwaysShowBookmarkBar());
  }
  // The Mac's entry for the native manager; Chromium's own shortcut for
  // IDC_SHOW_BOOKMARK_MANAGER on Windows is Ctrl+Shift+O.
  rows_.AppendCommand(bookmarks_, kCmdBookmarkManager,
                      PhiStr("appMenu.bookmarkManager"), nullptr,
                      services_->MenuFramework() != nullptr &&
                          !services_->MenuIncognito());
  rows_.AppendCommand(bookmarks_, kCmdExportBookmarks,
                      PhiStr("appMenu.exportBookmarks"), fluent::kExport,
                      store);
  const int fixed = ::GetMenuItemCount(bookmarks_);
  AppendBookmarkRows(bookmarks_, std::string(), 0);
  if (::GetMenuItemCount(bookmarks_) > fixed) {
    // A separator between the commands and the tree — inserted after the
    // fact so an empty list gets no dangling rule. The first tree row is
    // at `fixed`: insert by position through a temporary id-less path.
    MENUITEMINFOW info = {sizeof(info)};
    info.fMask = MIIM_ID;
    if (::GetMenuItemInfoW(bookmarks_, static_cast<UINT>(fixed), TRUE, &info) &&
        info.wID) {
      rows_.InsertSeparatorBefore(bookmarks_, info.wID);
    } else {
      // A folder leads the tree (no id): fall back to a plain separator.
      ::InsertMenuW(bookmarks_, static_cast<UINT>(fixed),
                    MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    }
  }
}

void PhiAppMenu::RefreshExtensions() {
  rows_.ClearMenu(extensions_);
  extension_ids_.clear();
  rows_.AppendCommand(extensions_, kCmdManageExtensions,
                      PhiStr("appMenu.manageExtensions"), fluent::kSettings,
                      services_->MenuFramework() != nullptr &&
                          services_->MenuActiveBrowser().valid());
  const BrowserId target = services_->MenuActiveBrowser();
  if (!services_->MenuFramework() || !target.valid()) {
    return;
  }
  // The same rows the puzzle popover lists (ListExtensionsJson); a row runs
  // the action as a click on its icon would.
  const char* json = services_->MenuFramework()->ListExtensionsJson(target.value);
  std::optional<base::Value> parsed =
      base::JSONReader::Read(json ? json : "[]", base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list() || parsed->GetList().empty()) {
    return;
  }
  rows_.AppendSeparator(extensions_);
  for (const base::Value& entry : parsed->GetList()) {
    const base::DictValue* dict = entry.GetIfDict();
    if (!dict ||
        extension_ids_.size() > kExtensionLast - kExtensionFirst) {
      continue;
    }
    const std::string* id = dict->FindString("id");
    const std::string* name = dict->FindString("name");
    if (!id || id->empty()) {
      continue;
    }
    const bool visible = dict->FindBool("visible").value_or(true);
    const bool enabled = dict->FindBool("enabled").value_or(true);
    if (!visible) {
      continue;
    }
    rows_.AppendCommand(
        extensions_, kExtensionFirst + static_cast<UINT>(extension_ids_.size()),
        PhiEscapeMenuText(
            base::UTF8ToWide(name && !name->empty() ? *name : *id)),
        fluent::kPuzzle, enabled);
    extension_ids_.push_back(*id);
  }
}

bool PhiAppMenu::PostAccelerator(HWND shell,
                                 UINT command,
                                 BrowserId find_browser) {
  DWORD_PTR data = 0;
  if (!::GetWindowSubclass(shell, &AcceleratorProc, kAcceleratorSubclass,
                           &data) ||
      !AcceleratorMessage()) {
    return false;
  }
  auto* menu = reinterpret_cast<PhiAppMenu*>(data);
  auto* c = menu->services_;
  const UINT_PTR token = menu->pending_accelerators_.Enqueue(
      {command,
       {c->MenuActiveSpace(), c->MenuActiveBrowser(), c->MenuActiveTab()},
       find_browser,
       reinterpret_cast<uintptr_t>(::GetFocus())});
  if (!::PostMessageW(shell, AcceleratorMessage(), token, 0)) {
    menu->pending_accelerators_.Erase(token);
    return false;
  }
  return true;
}

LRESULT CALLBACK PhiAppMenu::AcceleratorProc(HWND hwnd,
                                             UINT message,
                                             WPARAM wparam,
                                             LPARAM lparam,
                                             UINT_PTR subclass_id,
                                             DWORD_PTR data) {
  auto* menu = reinterpret_cast<PhiAppMenu*>(data);
  if (message == AcceleratorMessage()) {
    menu->DispatchAccelerator(wparam);
    return 0;
  }
  if (message == WM_NCDESTROY) {
    menu->pending_accelerators_.Clear();
    menu->shell_ = nullptr;
    ::RemoveWindowSubclass(hwnd, &AcceleratorProc, subclass_id);
  }
  return ::DefSubclassProc(hwnd, message, wparam, lparam);
}

void PhiAppMenu::DispatchAccelerator(UINT_PTR token) {
  auto* c = services_;
  const auto pending = pending_accelerators_.Take(
      token,
      {c->MenuActiveSpace(), c->MenuActiveBrowser(), c->MenuActiveTab()});
  if (!pending) {
    return;
  }
  if (pending->id == kCmdFind &&
      ::GetFocus() != reinterpret_cast<HWND>(pending->focus)) {
    return;
  }
  if (pending->id == kCmdFind && pending->find_browser.valid()) {
    if (c->MenuModel() && c->MenuModel()->Has(pending->find_browser) &&
        c->MenuFramework()) {
      auto* framework = c->MenuFramework();
      // Peek is a hosted child: Win32 SetFocus alone does not activate its
      // Aura FocusManager. Find must open inside the surface's Views tree.
      framework->FocusBrowserWindow(pending->find_browser.value);
      framework->ExecuteChromiumCommand(pending->find_browser.value, IDC_FIND);
    }
    return;
  }
  // Synchronous only after validation; preserves the shell's app-menu route
  // and accelerator provenance without a second unattributed queue hop.
  ::SendMessageW(shell_, WM_COMMAND, MAKEWPARAM(pending->id, 1), 0);
}

void PhiAppMenu::RenameSpace(const std::string& space_id) {
  PhiAppMenuShell* c = services_;
  if (!c->MenuSpacesHost() || space_id.empty()) {
    return;
  }
  // Prefill with the Space's current name (from the owner's list; the
  // chip label is the same string).
  std::wstring name;
  for (const PhiSpaceView& space : c->MenuSpacesHost()->Spaces()) {
    if (space.id == space_id) {
      name = base::UTF8ToWide(space.name);
      break;
    }
  }
  // Trimmed, a blank name refused, the shared name length enforced.
  if (PhiPromptText(c->MenuShellWindow(), PhiStr("app.renameSpaceDialog.title"),
                    PhiStr("app.renameSpaceDialog.message"), &name,
                    /*confirm_label=*/nullptr, kPhiNameRules)) {
    c->MenuSpacesHost()->RenameSpace(space_id, base::WideToUTF8(name));
  }
}

void PhiAppMenu::DeleteSpace(const std::string& space_id) {
  PhiAppMenuShell* c = services_;
  if (!c->MenuSpacesHost() || space_id.empty()) {
    return;
  }
  // The confirm names the Space: a pip's menu may be deleting one that is
  // not the Space on screen.
  std::wstring name;
  for (const PhiSpaceView& space : c->MenuSpacesHost()->Spaces()) {
    if (space.id == space_id) {
      name = base::UTF8ToWide(space.name);
      break;
    }
  }
  const std::wstring title =
      name.empty() ? std::wstring(PhiStr("dialog.deleteSpace.title"))
                   : PhiStrF("dialog.deleteNamedSpace.title", {name});
  if (PhiConfirm(c->MenuShellWindow(), title.c_str(),
                 PhiStr("menuBar.deleteSpaceConfirmBody"),
                 PhiStr("settings.spaces.deleteConfirmation.deleteButton"))) {
    c->MenuSpacesHost()->DeleteSpace(space_id);
  }
}

bool PhiAppMenu::OnCommand(UINT id, bool from_accelerator) {
  PhiAppMenuShell* c = services_;
  // The browser the slot SHOWS — invalid when none is attached, and NEVER
  // the slot's own number. Every framework branch below tests it first.
  // Nothing here refreshes by hand after a framework mutation: the model's
  // push schedules the rebuild (docs/architecture-review.md §3.1
  // principle 3).
  const BrowserId target = c->MenuActiveBrowser();

  // Under agent ownership the window is the agent's page: the Mac refuses
  // New Tab, closing, reopening and duplicating tabs, Back/Forward/Home and
  // the bookmark commands; only Space navigation stays live.
  if (c->MenuAgentHoldsInput()) {
    switch (id) {
      case kCmdNewTab:
      case kCmdNewTabToRight:
      case kCmdCloseTab:
      case kCmdReopenClosedTab:
      case kCmdDuplicateTab:
      case kCmdOpenFile:
      case kCmdOpenLocation:
      case kCmdBack:
      case kCmdForward:
      case kCmdHome:
      case kCmdBookmarkThisTab:
      case kCmdBookmarkAllTabs:
        return true;
      default:
        break;
    }
  }

  // Dynamic ranges first.
  if (id >= kSpaceFirst && id <= kSpaceLast) {
    const size_t i = id - kSpaceFirst;
    if (i < space_ids_.size()) {
      c->MenuSwitchSpace(space_ids_[i]);
    }
    return true;
  }
  if (id >= kCmdSpace1 && id <= kCmdSpace9) {
    // Ctrl+N from the keyboard hook: resolved against the LIVE Space list
    // (space_ids_ only refreshes when the Spaces menu opens).
    const std::vector<SpaceEntry> spaces = SpaceList();
    const size_t i = id - kCmdSpace1;
    if (i < spaces.size()) {
      c->MenuSwitchSpace(spaces[i].id);
    }
    return true;
  }
  if (id >= kExtensionFirst && id <= kExtensionLast) {
    const size_t i = id - kExtensionFirst;
    if (i < extension_ids_.size()) {
      c->MenuTriggerExtension(extension_ids_[i]);
    }
    return true;
  }
  if (id >= kBookmarkFirst && id <= kBookmarkLast) {
    const size_t i = id - kBookmarkFirst;
    if (c->MenuSidebarHost() && i < bookmark_urls_.size()) {
      c->MenuSidebarHost()->RequestOpen(c->MenuSlot().value, bookmark_urls_[i]);
    }
    return true;
  }
  if (id >= kProfileFirst && id <= kProfileLast) {
    // Re-binds the active Space. Confirmed like the Mac's: the Space's
    // window is replaced and its tabs reload in the new profile, where the
    // old profile's logins do not exist.
    const size_t i = id - kProfileFirst;
    if (c->MenuSpacesHost() && i < profile_ids_.size() &&
        PhiConfirm(c->MenuShellWindow(),
                   PhiStrF("app.changeSpaceProfileConfirmation.title",
                           {i < profile_names_.size() ? profile_names_[i]
                                                      : std::wstring()})
                       .c_str(),
                   PhiStr("app.changeSpaceProfileConfirmation."
                          "profileScopedMessage"),
                   nullptr, /*destructive=*/false)) {
      c->MenuSpacesHost()->SetSpaceProfile(c->MenuActiveSpace(),
                                           profile_ids_[i]);
    }
    return true;
  }
  if (id >= kDeleteProfileFirst && id <= kDeleteProfileLast) {
    const size_t i = id - kDeleteProfileFirst;
    if (c->MenuSpacesHost() && i < profile_ids_.size() &&
        PhiConfirm(c->MenuShellWindow(),
                   PhiStr("dialog.deleteProfile.title"),
                   PhiStr("menuBar.deleteProfileConfirmBody"),
                   PhiStr("settings.profiles.deleteConfirmation.deleteButton"))) {
      c->MenuSpacesHost()->DeleteProfileById(profile_ids_[i]);
    }
    return true;
  }

  if (const IdcRoute* route = RouteForCommand(id)) {
    // A page command (Zoom, Print, Save Page, View Source, DevTools, Find)
    // acts on the page in front of the user — the presented peek's, when
    // one is up (the Mac's pageCommandTarget); the rest on the strip's.
    const BrowserId page = route->page ? c->MenuPageCommandBrowser() : target;
    if (c->MenuFramework() && page.valid()) {
      if (id == kCmdFind) {
        c->MenuFocusPageForFind();
      }
      c->MenuFramework()->ExecuteChromiumCommand(page.value, route->idc);
    }
    return true;
  }

  switch (id) {
    case kCmdAbout:
      PhiNotice(c->MenuShellWindow(), PhiStr("menuBar.aboutPhi"),
                PhiStrF("menuBar.aboutBody", {PHI_WIN_VERSION_FULL_WIDE}).c_str());
      return true;
    case kCmdReportIssue:
      c->MenuOpenFeedback();
      return true;
    case kCmdCheckUpdate:
      if (!PhiUpdater::CheckForUpdateWithUi()) {
        PhiNotice(c->MenuShellWindow(), PhiStr("menuBar.checkForUpdate"),
                  PhiStr("menuBar.updaterUnavailableBody"));
      }
      return true;
    case kCmdPreferences:
      if (c->MenuWindowHost()) {
        c->MenuWindowHost()->RequestSettings();
      }
      return true;
    case kCmdUrlRules:
      // Spaces ▸ URL Rules…: the universal editor (the Mac's Spaces menu
      // item over AppController.openURLRulesEditor).
      if (c->MenuWindowHost()) {
        c->MenuWindowHost()->RequestUrlRulesEditor(c->MenuShellWindow());
      }
      return true;
    case kCmdQuit:
    case kCmdCloseWindow:
      // Through the normal close path; posted so the menu unwinds first.
      ::PostMessageW(c->MenuShellWindow(), WM_CLOSE, 0, 0);
      return true;
    case kCmdNewTab:
      c->MenuNewTab();  // Honors the "New tab behavior" setting.
      return true;
    case kCmdNewWindow:
      if (c->MenuWindowHost()) {
        // The profile the slot is showing.
        c->MenuWindowHost()->RequestNewWindow(c->MenuSlot().value);
      }
      return true;
    case kCmdNewIncognitoSpace:
      if (c->MenuSpacesHost()) {
        c->MenuSpacesHost()->RequestNewIncognitoSpace(c->MenuSlot().value);
      }
      return true;
    case kCmdOpenLocation:
      c->MenuFocusAddressField();
      return true;
    case kCmdCloseTab:
      if (c->MenuCloseTabEndsIncognitoSpace()) {
        return true;
      }
      // The TREE's active tab, not the active ROW: a tab bound to a pinned
      // cell or a bookmark is no row in the list, and Ctrl+W must still
      // close it (lane 1 D2).
      if (c->MenuFramework() && target.valid() && c->MenuActiveTab().valid()) {
        c->MenuFramework()->CloseTabById(target.value, c->MenuActiveTab().value);
      } else if (target.valid() && c->MenuModel()) {
        // Only a known, empty browser is a placeholder. A cold Space whose
        // browser has not attached yet must not accidentally close its shell.
        const auto* snapshot = c->MenuModel()->Get(target);
        if (snapshot && snapshot->tabs.empty()) {
          ::PostMessageW(c->MenuShellWindow(), WM_CLOSE, 0, 0);
        }
      }
      return true;
    case kCmdUndo:
    case kCmdCut:
    case kCmdCopy:
    case kCmdPaste:
    case kCmdDelete:
    case kCmdSelectAll: {
      HWND field = c->MenuAddressField();
      if (!field || ::GetFocus() != field) {
        return true;
      }
      switch (id) {
        case kCmdUndo:
          ::SendMessageW(field, WM_UNDO, 0, 0);
          break;
        case kCmdCut:
          ::SendMessageW(field, WM_CUT, 0, 0);
          break;
        case kCmdCopy:
          ::SendMessageW(field, WM_COPY, 0, 0);
          break;
        case kCmdPaste:
          ::SendMessageW(field, WM_PASTE, 0, 0);
          break;
        case kCmdDelete:
          ::SendMessageW(field, WM_CLEAR, 0, 0);
          break;
        case kCmdSelectAll:
          ::SendMessageW(field, EM_SETSEL, 0, -1);
          break;
      }
      return true;
    }
    case kCmdCopyUrl: {
      // The nav state's url is the page in front of the user — the presented
      // peek's, when one is up (PhiShellWindow::RebuildNavState).
      const std::wstring url = c->MenuNavState().url;
      if (url.empty() || !::OpenClipboard(c->MenuShellWindow())) {
        return true;
      }
      ::EmptyClipboard();
      const size_t bytes = (url.size() + 1) * sizeof(wchar_t);
      if (HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (wchar_t* dest = static_cast<wchar_t*>(::GlobalLock(handle))) {
          std::copy_n(url.c_str(), url.size() + 1, dest);
          ::GlobalUnlock(handle);
          if (!::SetClipboardData(CF_UNICODETEXT, handle)) {
            ::GlobalFree(handle);
          }
        } else {
          ::GlobalFree(handle);
        }
      }
      ::CloseClipboard();
      // The Mac's copy confirmation toast (OverlayToastCenter).
      PhiShowToast(c->MenuShellWindow(), c->MenuPalette(), PhiStr("app.copyTabURLsToast.single"));
      return true;
    }
    case kCmdStop:
      // Stop and Reload are page commands: the presented peek's page when
      // one is up (MenuPageCommandBrowser), else the active tab's.
      if (const BrowserId page = c->MenuPageCommandBrowser();
          c->MenuFramework() && page.valid()) {
        c->MenuFramework()->StopLoading(page.value);
      }
      return true;
    case kCmdAlwaysShowBookmarkBar:
      // Comfortable only: the item is absent from the vertical layouts'
      // View menu, and its Ctrl+Shift+B does nothing there either.
      if (c->MenuPrefsHost() &&
          c->MenuPrefsHost()->LayoutMode() == PhiLayoutMode::kComfortable) {
        c->MenuPrefsHost()->SetAlwaysShowBookmarkBar(
            !c->MenuPrefsHost()->AlwaysShowBookmarkBar());
      }
      return true;
    case kCmdBookmarkBarOnNewTab:
      if (c->MenuPrefsHost() &&
          c->MenuPrefsHost()->LayoutMode() == PhiLayoutMode::kComfortable) {
        c->MenuPrefsHost()->SetBookmarkBarOnNewTab(
            !c->MenuPrefsHost()->BookmarkBarOnNewTab());
      }
      return true;
    case kCmdToggleSidebar:
      c->MenuToggleSidebar();
      return true;
    case kCmdToggleReader:
      c->MenuToggleReader(from_accelerator);
      return true;
    case kCmdReload:
    case kCmdForceReload:
      if (const BrowserId page = c->MenuPageCommandBrowser();
          c->MenuFramework() && page.valid()) {
        c->MenuFramework()->Reload(page.value, id == kCmdForceReload);
      }
      return true;
    case kCmdAgentAutoview:
      if (c->MenuPrefsHost()) {
        c->MenuPrefsHost()->RequestAgentAutoView(
            !c->MenuPrefsHost()->AgentAutoView());
      }
      return true;
    case kCmdLayoutBalanced:
    case kCmdLayoutPerformance:  // Retired (impl log D9): lands on balanced.
    case kCmdLayoutComfortable:
      if (c->MenuPrefsHost()) {
        c->MenuPrefsHost()->RequestLayoutMode(
            id == kCmdLayoutComfortable ? PhiLayoutMode::kComfortable
                                        : PhiLayoutMode::kBalanced);
      }
      return true;
    case kCmdManageExtensions:
      if (c->MenuFramework() && target.valid()) {
        c->MenuFramework()->OpenTab(target, "chrome://extensions");
      }
      return true;
    case kCmdSaveToFolio:
      // Re-checked there: the chord arrives without the menu's enable pass.
      // The page in front of the user — the presented peek's, when one is up.
      if (SidebarHost* sidebar = c->MenuSidebarHost();
          sidebar && c->MenuPageCommandTab().valid()) {
        sidebar->RequestSaveToFolio(c->MenuPageCommandTab().value);
      }
      return true;
    case kCmdOpenFolio:
      if (SidebarHost* sidebar = c->MenuSidebarHost()) {
        sidebar->RequestOpenFolio();
      }
      return true;
    case kCmdNewSpace:
      c->MenuCreateSpace();  // The sidebar form, or at once without one.
      return true;
    case kCmdRenameSpace:
      RenameSpace(c->MenuActiveSpace());
      return true;
    case kCmdChangeIcon:
      c->MenuChangeSpaceIcon();
      return true;
    case kCmdEditSpaceTheme:
      // The Mac's .windowTop placement: the menu bar has no trigger point to
      // hang the panel off, so the shell drops it under the caption, centred
      // on the window.
      c->MenuEditSpaceTheme();
      return true;
    case kCmdDeleteSpace:
      DeleteSpace(c->MenuActiveSpace());
      return true;
    case kCmdCloseIncognitoSpace:
      c->MenuCloseIncognitoSpace();
      return true;
    case kCmdNewProfile: {
      std::wstring name;
      if (c->MenuFramework() &&
          PhiPromptText(c->MenuShellWindow(),
                        PhiStr("settings.profiles.createDialog.title"),
                        PhiStr("menuBar.newProfilePromptBody"), &name,
                        /*confirm_label=*/nullptr, kPhiNameRules)) {
        c->MenuFramework()->CreateProfile(base::WideToUTF8(name).c_str());
      }
      return true;
    }
    case kCmdNextSpace:
    case kCmdPreviousSpace: {
      const std::vector<SpaceEntry> spaces = SpaceList();
      if (spaces.size() < 2) {
        return true;
      }
      size_t at = 0;
      for (size_t i = 0; i < spaces.size(); ++i) {
        if (spaces[i].id == c->MenuActiveSpace()) {
          at = i;
          break;
        }
      }
      const size_t next = id == kCmdNextSpace
                              ? (at + 1) % spaces.size()
                              : (at + spaces.size() - 1) % spaces.size();
      c->MenuSwitchSpace(spaces[next].id);
      return true;
    }
    case kCmdBack:
      // Back over a tab the reader is covering means "leave the reader" —
      // the same rule the toolbar's back button follows.
      if (c->MenuLeaveReaderForNavigation()) {
        return true;
      }
      if (c->MenuFramework() && target.valid()) {
        c->MenuFramework()->GoBack(target.value);
      }
      return true;
    case kCmdForward:
      if (c->MenuLeaveReaderForNavigation()) {
        return true;
      }
      if (c->MenuFramework() && target.valid()) {
        c->MenuFramework()->GoForward(target.value);
      }
      return true;
    case kCmdBookmarkThisTab:
      // Ctrl+D reaches here even though the row is greyed off the record:
      // an incognito page never becomes a regular (synced) bookmark.
      // The page in front of the user: the presented peek's page when one
      // is up, else the active tab's (the Mac's pageCommandTarget).
      if (c->MenuSidebarHost() && !c->MenuIncognito()) {
        c->MenuSidebarHost()->RequestBookmarkActiveTab(
            c->MenuSlot().value, c->MenuPageCommandBrowser().value);
      }
      return true;
    case kCmdBookmarkAllTabs: {
      if (!c->MenuSidebarHost() || c->MenuIncognito()) {
        return true;
      }
      std::wstring name = PhiStr("menuBar.savedTabs");
      if (!PhiPromptText(
              c->MenuShellWindow(),
              PhiStr("common.tabAreaContextMenu.bookmarkAllTabsAction"),
              PhiStr("menuBar.bookmarkAllTabsPromptBody"), &name,
              /*confirm_label=*/nullptr, kPhiNameRules) ||
          name.empty()) {
        return true;
      }
      const std::string folder = c->MenuSidebarHost()->AddBookmarkFolder(
          c->MenuSlot().value, std::string(), base::WideToUTF8(name));
      if (folder.empty() || !c->MenuModel() || !target.valid()) {
        return true;
      }
      int added = 0;
      if (const phi_framework::WindowSnapshot* snapshot =
              c->MenuModel()->Get(target)) {
        for (const phi_framework::TabSnapshot& tab : snapshot->tabs) {
          if (!tab.url.empty() &&
              !c->MenuSidebarHost()
                   ->AddBookmarkRow(c->MenuSlot().value, folder, tab.title,
                                    tab.url)
                   .empty()) {
            ++added;
          }
        }
      }
      PhiShowToast(
          c->MenuShellWindow(), c->MenuPalette(),
          PhiStrF("menuBar.tabsBookmarkedToast", {std::to_wstring(added)}));
      return true;
    }
    case kCmdBookmarkManager:
      // URL-driven, as on Mac: the manager IS a tab on the bookmark route,
      // and the client mounts its native page over that tab's content.
      if (c->MenuFramework() && target.valid()) {
        c->MenuFramework()->NewTab(target.value, PhiBookmarkManager::ManagerUrl());
      }
      return true;
    case kCmdExportBookmarks: {
      if (!c->MenuSidebarHost()) {
        return true;
      }
      wchar_t path[MAX_PATH] = L"bookmarks.html";
      // "<label>\0*.html\0\0": the label localized, the pattern not.
      std::wstring filter = PhiStr("menuBar.exportBookmarks.htmlFileFilter");
      filter.push_back(L'\0');
      filter += L"*.html";
      filter.push_back(L'\0');
      filter.push_back(L'\0');
      OPENFILENAMEW ofn = {sizeof(ofn)};
      ofn.hwndOwner = c->MenuShellWindow();
      ofn.lpstrFilter = filter.c_str();
      ofn.lpstrFile = path;
      ofn.nMaxFile = MAX_PATH;
      ofn.Flags = OFN_OVERWRITEPROMPT;
      ofn.lpstrDefExt = L"html";
      if (!::GetSaveFileNameW(&ofn)) {
        return true;
      }
      const std::string html = BuildBookmarksHtml();
      HANDLE f = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
      if (f != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        ::WriteFile(f, html.data(), static_cast<DWORD>(html.size()), &wrote,
                    nullptr);
        ::CloseHandle(f);
        PhiShowToast(c->MenuShellWindow(), c->MenuPalette(),
                     PhiStr("menuBar.bookmarksExported"));
      }
      return true;
    }
    case kCmdNextTab:
    case kCmdPreviousTab: {
      // As the last rebuild placed them.
      const std::vector<PhiAppMenuTab> tabs = c->MenuTabRows();
      if (tabs.size() < 2 || !c->MenuFramework() || !target.valid()) {
        return true;
      }
      size_t at = 0;
      for (size_t i = 0; i < tabs.size(); ++i) {
        if (tabs[i].active) {
          at = i;
          break;
        }
      }
      const size_t next = id == kCmdNextTab
                              ? (at + 1) % tabs.size()
                              : (at + tabs.size() - 1) % tabs.size();
      c->MenuFramework()->ActivateTabById(target.value, tabs[next].id.value);
      return true;
    }
    case kCmdImportBrowser:
      if (c->MenuWindowHost()) {
        c->MenuWindowHost()->RequestBrowserImport();
      }
      return true;
    case kCmdNewTabToRight: {
      // Composed from the primitives: IDC_NEW_TAB_TO_RIGHT stays disabled in
      // the hollowed chrome (it keys off FEATURE_TABSTRIP).
      if (!c->MenuFramework() || !target.valid()) {
        return true;
      }
      // By stable ids: the new tab's own id from OpenTab, the anchor the
      // tree's active tab, and an anchored move that survives whatever the
      // insertion did to the indices.
      const TabId anchor = c->MenuActiveTab();
      const TabId created = c->MenuFramework()->OpenTab(target, "");
      if (created.valid() && anchor.valid()) {
        c->MenuFramework()->MoveTabAfter(target.value, created.value, anchor.value,
                                    nullptr);
      }
      return true;
    }
    case kCmdSearchTabs:
      c->MenuToggleSearchTabs();
      return true;
    case kCmdDownloads:
      c->MenuToggleDownloads();
      return true;
    case kCmdMinimize:
      ::ShowWindow(c->MenuShellWindow(), SW_MINIMIZE);
      return true;
    case kCmdZoomWindow:
      ::ShowWindow(c->MenuShellWindow(),
                   ::IsZoomed(c->MenuShellWindow()) ? SW_RESTORE : SW_MAXIMIZE);
      return true;
    default:
      break;
  }
  // Disabled placeholders land here when invoked programmatically.
  return id >= kCmdAbout && id <= kCmdOpenFolio;
}

std::string PhiAppMenu::BuildBookmarksHtml() const {
  // The Mac BookmarkHTMLExporter's shape: standard Netscape file, pinned
  // rows first as one "Favorites" folder, then the bookmark tree.
  const auto escaped = [](const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
      switch (c) {
        case '&':
          out += "&amp;";
          break;
        case '<':
          out += "&lt;";
          break;
        case '>':
          out += "&gt;";
          break;
        case '"':
          out += "&quot;";
          break;
        default:
          out.push_back(c);
      }
    }
    return out;
  };
  std::string html =
      "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
      "<!-- This is an automatically generated file. -->\n"
      "<META HTTP-EQUIV=\"Content-Type\" "
      "CONTENT=\"text/html; charset=UTF-8\">\n"
      "<TITLE>Bookmarks</TITLE>\n"
      "<H1>Bookmarks</H1>\n"
      "<DL><p>\n";
  if (services_->MenuSidebarHost()) {
    const std::vector<PhiStoredRowView> pins =
        services_->MenuSidebarHost()->PinnedRows(services_->MenuSlot().value);
    if (!pins.empty()) {
      html += "    <DT><H3>Favorites</H3>\n    <DL><p>\n";
      for (const PhiStoredRowView& pin : pins) {
        if (pin.url.empty()) {
          continue;
        }
        html += "        <DT><A HREF=\"" + escaped(pin.url) + "\">" +
                escaped(!pin.title.empty() ? pin.title : pin.url) +
                "</A>\n";
      }
      html += "    </DL><p>\n";
    }
    const auto descend = [&](const auto& self, const std::string& parent,
                             const std::string& indent) -> void {
      for (const PhiStoredRowView& row :
           services_->MenuSidebarHost()->BookmarkRows(
               services_->MenuSlot().value, parent)) {
        if (row.folder) {
          html += indent + "<DT><H3>" + escaped(row.title) + "</H3>\n" +
                  indent + "<DL><p>\n";
          self(self, row.guid, indent + "    ");
          html += indent + "</DL><p>\n";
        } else if (!row.url.empty()) {
          html += indent + "<DT><A HREF=\"" + escaped(row.url) + "\">" +
                  escaped(!row.title.empty() ? row.title : row.url) +
                  "</A>\n";
        }
      }
    };
    descend(descend, std::string(), "    ");
  }
  html += "</DL><p>\n";
  return html;
}

std::string PhiAppMenu::ListJson() {
  RefreshRoot();
  RefreshSpaces();
  RefreshHistory();
  RefreshBookmarks();
  RefreshExtensions();
  RefreshLayout();
  RefreshTools();
  RefreshHelp();
  RefreshChromiumCommands();

  // Serialize what the user would see, straight from the HMENUs.
  struct Walker {
    static base::ListValue Items(HMENU menu, int depth) {
      base::ListValue items;
      const int count = ::GetMenuItemCount(menu);
      for (int i = 0; i < count && depth < 8; ++i) {
        MENUITEMINFOW info = {sizeof(info)};
        info.fMask = MIIM_ID | MIIM_STATE | MIIM_SUBMENU | MIIM_FTYPE;
        if (!::GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info)) {
          continue;
        }
        base::DictValue item;
        if (info.fType & MFT_SEPARATOR) {
          item.Set("separator", true);
        } else {
          item.Set("title", base::WideToUTF8(PhiMenuLabelAt(menu, i)));
          item.Set("enabled", (info.fState & MFS_DISABLED) == 0);
          item.Set("checked", (info.fState & MFS_CHECKED) != 0);
          if (const PhiMenuRow* row = PhiMenuRowAt(menu, i);
              row && !row->shortcut.empty()) {
            item.Set("shortcut", base::WideToUTF8(row->shortcut));
          }
          if (info.hSubMenu) {
            item.Set("items", Items(info.hSubMenu, depth + 1));
          } else {
            item.Set("id", static_cast<int>(info.wID));
          }
        }
        items.Append(std::move(item));
      }
      return items;
    }
  };
  std::string json;
  base::JSONWriter::Write(Walker::Items(root_, 0), &json);
  return json;
}

bool PhiAppMenu::InvokePath(const std::vector<std::string>& path) {
  if (!root_ || path.empty()) {
    return false;
  }
  HMENU menu = root_;
  OnMenuOpening(menu);  // What a click would have refreshed.
  for (size_t step = 0; step < path.size(); ++step) {
    const std::wstring want = base::UTF8ToWide(path[step]);
    const int count = ::GetMenuItemCount(menu);
    bool found = false;
    for (int i = 0; i < count; ++i) {
      if (PhiMenuLabelAt(menu, i) != want) {
        continue;
      }
      MENUITEMINFOW info = {sizeof(info)};
      info.fMask = MIIM_ID | MIIM_STATE | MIIM_SUBMENU;
      if (!::GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info)) {
        return false;
      }
      if (info.hSubMenu) {
        if (step + 1 >= path.size()) {
          return false;  // A submenu itself cannot be activated.
        }
        menu = info.hSubMenu;
        OnMenuOpening(menu);
        found = true;
        break;
      }
      if (step + 1 != path.size() || (info.fState & MFS_DISABLED)) {
        return false;
      }
      return OnCommand(info.wID);
    }
    if (!found) {
      return false;
    }
  }
  return false;
}

}  // namespace phi_client

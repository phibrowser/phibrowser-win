// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_preferences.h"

#include <algorithm>
#include <map>
#include <optional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "phibrowser-win/client/phi_file_io.h"
#include "phibrowser-win/client/phi_folio.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_app_menu.h"

namespace phi_client {

namespace {

// The customisable shortcuts (the Mac Shortcuts pane's editable rows), in the
// pane's display order. `name` is the stable prefs key (what
// CustomShortcuts.json stores, so it never changes); `group` the pane's
// ASCII group id (phi_settings_types.h kPhiShortcutGroups); `label_key` the
// Mac's title key, resolved when the rows are built so translations join by
// key; the chord the default the ⋯ menu advertises. Everything NOT in this
// table (Quit, F5, the app menu's F10 / Alt+F / Alt+E, F6, Ctrl+Tab's MRU
// switcher, Ctrl+1..9 Spaces) stays fixed — only ⌘Q does on Mac.
struct RebindableShortcut {
  const char* name;
  const char* group;
  const char* label_key;
  UINT command;
  PhiShortcutChord def;
};
constexpr RebindableShortcut kRebindableShortcuts[] = {
    {"openSettings", "App", "settings.shortcuts.command.openSettings",
     kCmdPreferences, {VK_OEM_COMMA, true, false, false}},
    {"newTab", "File", "settings.shortcuts.command.newTab", kCmdNewTab,
     {'T', true, false, false}},
    {"newWindow", "File", "settings.shortcuts.command.newWindow",
     kCmdNewWindow, {'N', true, false, false}},
    {"newIncognitoWindow", "File",
     "settings.shortcuts.command.newIncognitoWindow", kCmdNewIncognitoWindow,
     {'N', true, true, false}},
    // The Mac's ⌃⇧N has no free twin here (Ctrl+Shift+N is the incognito
    // window); Ctrl+Alt follows the reader's Ctrl+Alt+R.
    {"newIncognitoSpace", "File", "app.fileMenu.createNewIncognitoSpace",
     kCmdNewIncognitoSpace, {'N', true, false, true}},
    {"reopenClosedTab", "File", "settings.shortcuts.command.reopenClosedTab",
     kCmdReopenClosedTab, {'T', true, true, false}},
    {"openFile", "File", "settings.shortcuts.command.openFile", kCmdOpenFile,
     {'O', true, false, false}},
    // The Mac's ⌘⌥S, as Reader's ⌘⌥R became Ctrl+Alt+R.
    {"saveToFolio", "File", "settings.shortcuts.command.saveToFolio",
     kCmdSaveToFolio, {'S', true, false, true}},
    {"openLocation", "File", "settings.shortcuts.command.focusAddressBar",
     kCmdOpenLocation, {'L', true, false, false}},
    {"closeTab", "File", "settings.shortcuts.command.closeTab", kCmdCloseTab,
     {'W', true, false, false}},
    {"closeWindow", "File", "settings.shortcuts.command.closeWindow",
     kCmdCloseWindow, {'W', true, true, false}},
    {"print", "File", "settings.shortcuts.command.printPage", kCmdPrint,
     {'P', true, false, false}},
    {"copyUrl", "Edit", "settings.shortcuts.command.copyURL", kCmdCopyUrl,
     {'C', true, true, false}},
    {"find", "Edit", "settings.shortcuts.command.findOnPage", kCmdFind,
     {'F', true, false, false}},
    {"reload", "View", "settings.shortcuts.command.reloadPage", kCmdReload,
     {'R', true, false, false}},
    {"forceReload", "View", "settings.shortcuts.command.forceReload",
     kCmdForceReload, {'R', true, true, false}},
    // Ctrl+0 / Ctrl++ / Ctrl+-; the numeric keypad's zoom twins stay fixed
    // in the hook's table.
    {"actualSize", "View", "settings.shortcuts.command.resetZoom",
     kCmdActualSize, {'0', true, false, false}},
    {"zoomIn", "View", "settings.shortcuts.command.zoomIn", kCmdZoomIn,
     {VK_OEM_PLUS, true, false, false}},
    {"zoomOut", "View", "settings.shortcuts.command.zoomOut", kCmdZoomOut,
     {VK_OEM_MINUS, true, false, false}},
    {"viewSource", "View", "settings.shortcuts.command.viewPageSource",
     kCmdViewSource, {'U', true, false, false}},
    {"devTools", "View", "settings.shortcuts.command.openDeveloperTools",
     kCmdDevTools, {'I', true, true, false}},
    {"jsConsole", "View", "settings.shortcuts.command.openDeveloperConsole",
     kCmdJsConsole, {'J', true, true, false}},
    {"toggleSidebar", "View", "settings.shortcuts.command.toggleSidebar",
     kCmdToggleSidebar, {'S', true, false, false}},
    // Ctrl+Alt+R, the Mac's Cmd-Opt-R: Safari's reader chord (Cmd-Shift-R)
    // is Chromium's hard reload on both platforms.
    {"toggleReader", "View", "settings.shortcuts.command.toggleReaderView",
     kCmdToggleReader, {'R', true, false, true}},
    {"toggleBookmarkBar", "View",
     "settings.shortcuts.command.toggleBookmarkBar", kCmdAlwaysShowBookmarkBar,
     {'B', true, true, false}},
    // The history chords Chrome reserves: Alt+Home / Left / Right.
    {"home", "History", "settings.shortcuts.command.goHome", kCmdHome,
     {VK_HOME, false, false, true}},
    {"back", "History", "settings.shortcuts.command.goBack", kCmdBack,
     {VK_LEFT, false, false, true}},
    {"forward", "History", "settings.shortcuts.command.goForward",
     kCmdForward, {VK_RIGHT, false, false, true}},
    {"showFullHistory", "History", "settings.shortcuts.command.showHistory",
     kCmdShowFullHistory, {'H', true, false, false}},
    {"bookmarkThisTab", "Bookmarks", "settings.shortcuts.command.bookmarkTab",
     kCmdBookmarkThisTab, {'D', true, false, false}},
    {"bookmarkAllTabs", "Bookmarks",
     "settings.shortcuts.command.bookmarkAllTabs", kCmdBookmarkAllTabs,
     {'D', true, true, false}},
    {"downloads", "Window", "settings.shortcuts.command.showDownloads",
     kCmdDownloads, {'J', true, false, false}},
    {"searchTabs", "Window", "settings.shortcuts.command.searchTabs",
     kCmdSearchTabs, {'A', true, true, false}},
    {"nextSpace", "Spaces", "settings.shortcuts.command.selectNextSpace",
     kCmdNextSpace, {VK_RIGHT, true, false, true}},
    {"previousSpace", "Spaces",
     "settings.shortcuts.command.selectPreviousSpace", kCmdPreviousSpace,
     {VK_LEFT, true, false, true}},
};

}  // namespace

PhiPreferences::PhiPreferences(DirtyCallback dirty)
    : palette_(PhiResolvePalette(PhiThemeById(settings_.theme_id),
                                 static_cast<PhiAppearance>(
                                     settings_.appearance),
                                 settings_.saturation)),
      dirty_(std::move(dirty)) {}

PhiPreferences::~PhiPreferences() = default;

void PhiPreferences::AddObserver(Observer* observer) {
  observers_.push_back(observer);
}

void PhiPreferences::RemoveObserver(Observer* observer) {
  std::erase(observers_, observer);
}

void PhiPreferences::Load(const std::wstring& user_data_dir) {
  user_data_dir_ = user_data_dir;
  store_.Init(user_data_dir);
  // The load leaves the model usable: decoded, imported from the
  // pre-phase-3 files on a first run over an old profile, or defaults over
  // a quarantined file (the truth-owner rules are in phi_app_model.h).
  store_.Load(&settings_);
  // The language first and before any chrome is built, so the first window
  // comes up in the chosen language rather than flashing English; an empty
  // preference follows the system.
  PhiSetLanguage(settings_.language);
  // The palette depends on the theme AND the saturation, so it is resolved
  // once both are known.
  ResolvePalette();
}

void PhiPreferences::Save() {
  store_.Save(settings_);
}

void PhiPreferences::BindAccount(PhiAccountDefaults* defaults) {
  account_ = defaults;
  const model::PhiSettings fresh;
  // The sidebar width: the account's, clamped, or the default.
  int width = fresh.sidebar_width;
  if (account_) {
    width = static_cast<int>(
        account_->GetDouble(kDefaultsSidebarWidth, fresh.sidebar_width));
  }
  width = std::max(kSidebarWidthMin, std::min(width, kSidebarWidthMax));
  // The shortcut overrides: <account>\CustomShortcuts.json — the Mac's file
  // and place. The chords are Windows virtual keys keyed by the rebind
  // name (a Mac key code means nothing here; shortcuts are not synced).
  std::map<std::string, model::ShortcutChord> overrides;
  shortcuts_save_blocked_ = false;
  if (account_ && !account_->dir().empty()) {
    std::string bytes;
    const PhiReadStatus read =
        PhiReadFile(account_->dir() + L"\\CustomShortcuts.json", &bytes);
    if (read == PhiReadStatus::kError) {
      // Locked: the defaults stand in, and the user's file is never
      // replaced this session.
      shortcuts_save_blocked_ = true;
      LOG(ERROR) << "[PhiClient] shortcuts could not be read; saves off";
    }
    if (read == PhiReadStatus::kOk) {
      std::optional<base::Value> list =
          base::JSONReader::Read(bytes, base::JSON_PARSE_RFC);
      if (list && list->is_list()) {
        for (const base::Value& entry : list->GetList()) {
          const base::DictValue* d = entry.GetIfDict();
          const std::string* name = d ? d->FindString("name") : nullptr;
          if (!name) {
            continue;
          }
          overrides[*name] = {
              static_cast<unsigned>(d->FindInt("vk").value_or(0)),
              d->FindBool("ctrl").value_or(false),
              d->FindBool("shift").value_or(false),
              d->FindBool("alt").value_or(false)};
        }
      }
    }
  }
  if (width != settings_.sidebar_width) {
    settings_.sidebar_width = width;
    Notify(PreferenceChange::kSidebarWidth);
  }
  if (overrides != settings_.shortcut_overrides) {
    settings_.shortcut_overrides = std::move(overrides);
    Notify(PreferenceChange::kShortcuts);
  }
}

void PhiPreferences::SaveShortcuts() const {
  if (!account_ || account_->dir().empty() || shortcuts_save_blocked_) {
    return;
  }
  base::ListValue list;
  for (const auto& [name, chord] : settings_.shortcut_overrides) {
    base::DictValue entry;
    entry.Set("name", name);
    entry.Set("vk", static_cast<int>(chord.vk));
    entry.Set("ctrl", chord.ctrl);
    entry.Set("shift", chord.shift);
    entry.Set("alt", chord.alt);
    list.Append(std::move(entry));
  }
  std::string json;
  base::JSONWriter::Write(list, &json);
  if (!PhiWriteFileAtomic(account_->dir() + L"\\CustomShortcuts.json",
                          json)) {
    LOG(ERROR) << "[PhiClient] shortcuts: write failed";
  }
}

// --- ChromePrefsHost --------------------------------------------------------

PhiLayoutMode PhiPreferences::LayoutMode() const {
  return settings_.layout_mode;
}

bool PhiPreferences::SuppressCloseIncognitoSpaceWarning() const {
  return settings_.suppress_close_incognito_space_warning;
}

void PhiPreferences::SetSuppressCloseIncognitoSpaceWarning(bool suppress) {
  if (settings_.suppress_close_incognito_space_warning == suppress) {
    return;
  }
  settings_.suppress_close_incognito_space_warning = suppress;
  MarkDirty();
  Notify(PreferenceChange::kSyncedMisc);
}

void PhiPreferences::SetAutoPictureInPictureMode(const std::string& mode) {
  if ((mode != "off" && mode != "normal" && mode != "parked") ||
      settings_.auto_picture_in_picture_mode == mode) {
    return;
  }
  settings_.auto_picture_in_picture_mode = mode;
  MarkDirty();
  Notify(PreferenceChange::kSyncedMisc);
}

void PhiPreferences::SetSelectionTintEnabled(bool enabled) {
  if (settings_.selection_tint_enabled == enabled) {
    return;
  }
  settings_.selection_tint_enabled = enabled;
  MarkDirty();
  Notify(PreferenceChange::kSyncedMisc);
}

void PhiPreferences::RequestLayoutMode(PhiLayoutMode mode) {
  if (settings_.layout_mode == mode) {
    return;
  }
  settings_.layout_mode = mode;
  MarkDirty();
  Notify(PreferenceChange::kLayoutMode);
}

bool PhiPreferences::ShowTabPreviews() const {
  return settings_.show_tab_previews;
}

bool PhiPreferences::ShowOpenTabIndicators() const {
  return settings_.show_open_tab_indicators;
}

bool PhiPreferences::DimUnloadedTabIcons() const {
  return settings_.dim_unloaded_tab_icons;
}

bool PhiPreferences::NewTabOpensOmnibox() const {
  return settings_.new_tab_behavior == model::NewTabBehavior::kOmnibox;
}

int PhiPreferences::SidebarWidth() const {
  return settings_.sidebar_width;
}

void PhiPreferences::RequestSidebarWidth(int dip) {
  const int clamped =
      std::max(kSidebarWidthMin, std::min(dip, kSidebarWidthMax));
  if (clamped == settings_.sidebar_width) {
    return;
  }
  settings_.sidebar_width = clamped;
  if (account_) {
    account_->SetDouble(kDefaultsSidebarWidth, clamped);
  }
  Notify(PreferenceChange::kSidebarWidth);
}

bool PhiPreferences::SettingsWindowSize(int* width, int* height) const {
  if (settings_.settings_window_width <= 0 ||
      settings_.settings_window_height <= 0) {
    return false;
  }
  *width = settings_.settings_window_width;
  *height = settings_.settings_window_height;
  return true;
}

void PhiPreferences::RequestSettingsWindowSize(int width, int height) {
  if (width <= 0 || height <= 0 ||
      (width == settings_.settings_window_width &&
       height == settings_.settings_window_height)) {
    return;
  }
  settings_.settings_window_width = width;
  settings_.settings_window_height = height;
  MarkDirty();  // Nobody else reads it live: no Notify.
}

const PhiPalette& PhiPreferences::Palette() const {
  return palette_;
}

bool PhiPreferences::AlwaysShowBookmarkBar() const {
  return settings_.always_show_bookmark_bar;
}

void PhiPreferences::SetAlwaysShowBookmarkBar(bool enabled) {
  if (enabled == settings_.always_show_bookmark_bar) {
    return;
  }
  settings_.always_show_bookmark_bar = enabled;
  MarkDirty();
  Notify(PreferenceChange::kBookmarkBar);
}

bool PhiPreferences::BookmarkBarOnNewTab() const {
  return settings_.bookmark_bar_on_new_tab;
}

void PhiPreferences::SetBookmarkBarOnNewTab(bool enabled) {
  if (enabled == settings_.bookmark_bar_on_new_tab) {
    return;
  }
  settings_.bookmark_bar_on_new_tab = enabled;
  MarkDirty();
  Notify(PreferenceChange::kBookmarkBar);
}

std::string PhiPreferences::ThemeId() const {
  return settings_.theme_id;
}

PhiAppearance PhiPreferences::Appearance() const {
  // Same enumerators in the same order (phi_app_model.h): the model's twin
  // exists only to keep that header off windows.h.
  return static_cast<PhiAppearance>(settings_.appearance);
}

int PhiPreferences::Saturation() const {
  return settings_.saturation;
}

// --- Theme ------------------------------------------------------------------

void PhiPreferences::SetThemeId(const std::string& id) {
  const std::string normalised = PhiThemeById(id).id;
  if (normalised == settings_.theme_id) {
    return;
  }
  settings_.theme_id = normalised;
  ResolvePalette();
  MarkDirty();
  Notify(PreferenceChange::kTheme);
}

void PhiPreferences::SetAppearance(PhiAppearance appearance) {
  if (appearance == Appearance()) {
    return;
  }
  settings_.appearance = static_cast<model::Appearance>(appearance);
  ResolvePalette();
  MarkDirty();
  Notify(PreferenceChange::kTheme);
}

void PhiPreferences::SetSaturation(int value) {
  value = value < 0 ? 0 : (value > 100 ? 100 : value);
  if (value == settings_.saturation) {
    return;
  }
  settings_.saturation = value;
  ResolvePalette();
  MarkDirty();
  Notify(PreferenceChange::kSaturation);
}

void PhiPreferences::ResolvePalette() {
  palette_ = PhiResolvePalette(PhiThemeById(settings_.theme_id), Appearance(),
                               settings_.saturation);
}

// --- General ----------------------------------------------------------------

void PhiPreferences::SetNewTabBehavior(model::NewTabBehavior behavior) {
  if (behavior == settings_.new_tab_behavior) {
    return;
  }
  settings_.new_tab_behavior = behavior;
  MarkDirty();
  Notify(PreferenceChange::kNewTab);
}

bool PhiPreferences::AlwaysShowFullUrl() const {
  return settings_.always_show_full_url;
}

void PhiPreferences::SetAlwaysShowFullUrl(bool enabled) {
  if (enabled == settings_.always_show_full_url) {
    return;
  }
  settings_.always_show_full_url = enabled;
  MarkDirty();
  Notify(PreferenceChange::kFullUrl);
}

void PhiPreferences::SetShowTabPreviews(bool enabled) {
  if (enabled == settings_.show_tab_previews) {
    return;
  }
  settings_.show_tab_previews = enabled;
  MarkDirty();
  Notify(PreferenceChange::kPreviews);
}

void PhiPreferences::SetShowOpenTabIndicators(bool enabled) {
  if (enabled == settings_.show_open_tab_indicators) {
    return;
  }
  settings_.show_open_tab_indicators = enabled;
  MarkDirty();
  Notify(PreferenceChange::kOpenIndicators);
}

void PhiPreferences::SetDimUnloadedTabIcons(bool enabled) {
  if (enabled == settings_.dim_unloaded_tab_icons) {
    return;
  }
  settings_.dim_unloaded_tab_icons = enabled;
  MarkDirty();
  Notify(PreferenceChange::kDimUnloaded);
}

void PhiPreferences::SetPeekViewEnabled(bool enabled) {
  if (enabled == settings_.peek_view_enabled) {
    return;
  }
  settings_.peek_view_enabled = enabled;
  MarkDirty();
  Notify(PreferenceChange::kPeek);
}

void PhiPreferences::SetAutoPeekViewEnabled(bool enabled) {
  if (enabled == settings_.auto_peek_view_enabled) {
    return;
  }
  settings_.auto_peek_view_enabled = enabled;
  MarkDirty();
  Notify(PreferenceChange::kPeek);
}

void PhiPreferences::SetOpenKioskOnCtrlAltClick(bool enabled) {
  if (enabled == settings_.open_kiosk_on_ctrl_alt_click) {
    return;
  }
  settings_.open_kiosk_on_ctrl_alt_click = enabled;
  MarkDirty();
  Notify(PreferenceChange::kKiosk);
}

std::string PhiPreferences::FolioGlobalFolder() const {
  return settings_.folio_folder.empty() ? PhiFolioDefaultFolder()
                                        : settings_.folio_folder;
}

void PhiPreferences::SetFolioGlobalFolder(const std::string& path) {
  const std::string stored =
      path == PhiFolioDefaultFolder() ? std::string() : path;
  if (stored == settings_.folio_folder) {
    return;
  }
  settings_.folio_folder = stored;
  MarkDirty();
  Notify(PreferenceChange::kFolio);
}

std::string PhiPreferences::FolioFolderOverride(
    const std::string& profile_id) const {
  const auto it = settings_.folio_profile_folders.find(profile_id);
  return it == settings_.folio_profile_folders.end() ? std::string()
                                                     : it->second;
}

void PhiPreferences::SetFolioFolderOverride(const std::string& profile_id,
                                            const std::string& path) {
  if (profile_id.empty() || FolioFolderOverride(profile_id) == path) {
    return;
  }
  if (path.empty()) {
    settings_.folio_profile_folders.erase(profile_id);
  } else {
    settings_.folio_profile_folders[profile_id] = path;
  }
  MarkDirty();
  Notify(PreferenceChange::kFolio);
}

std::string PhiPreferences::FolioFolderForProfile(
    const std::string& profile_id) const {
  const std::string override_path = FolioFolderOverride(profile_id);
  return override_path.empty() ? FolioGlobalFolder() : override_path;
}

void PhiPreferences::SetFolioAutoSaveOnSiteActions(bool enabled) {
  if (enabled == settings_.folio_auto_save_on_site_actions) {
    return;
  }
  settings_.folio_auto_save_on_site_actions = enabled;
  MarkDirty();
  Notify(PreferenceChange::kFolio);
}

void PhiPreferences::SetRestoreLastSession(bool enabled) {
  if (enabled == settings_.restore_last_session) {
    return;
  }
  settings_.restore_last_session = enabled;
  MarkDirty();
  Notify(PreferenceChange::kRestore);
}

void PhiPreferences::SetDeveloperMode(bool enabled) {
  if (settings_.developer_mode == enabled) {
    return;
  }
  settings_.developer_mode = enabled;
  MarkDirty();
  Notify(PreferenceChange::kDeveloper);
}

void PhiPreferences::SetGuestModeEnabled(bool enabled) {
  if (settings_.guest_mode_enabled == enabled) {
    return;
  }
  settings_.guest_mode_enabled = enabled;
  MarkDirty();
  Notify(PreferenceChange::kAccess);
}

// The Mac's per-account loginPhase, in THAT account's defaults — through
// the bound instance when it is the open account (a second writer would be
// overwritten by the bound one's next save), else read or written directly.
int PhiPreferences::OnboardingPhase(const std::string& sub) const {
  if (sub.empty() || user_data_dir_.empty()) {
    return 0;
  }
  const std::wstring dir = PhiAccountDirPath(user_data_dir_, sub);
  if (account_ && account_->dir() == dir) {
    return account_->GetInt(kDefaultsLoginPhase, 0);
  }
  PhiAccountDefaults defaults;
  defaults.Load(dir);
  return defaults.GetInt(kDefaultsLoginPhase, 0);
}

void PhiPreferences::SetOnboardingPhase(const std::string& sub, int phase) {
  if (sub.empty() || OnboardingPhase(sub) == phase) {
    return;
  }
  const std::wstring dir = PhiAccountDirPath(user_data_dir_, sub);
  if (account_ && account_->dir() == dir) {
    account_->SetInt(kDefaultsLoginPhase, phase);
  } else {
    PhiAccountDefaults defaults;
    defaults.Load(PhiEnsureAccountDir(user_data_dir_, sub));
    defaults.SetInt(kDefaultsLoginPhase, phase);
  }
  // The Mac's app-level phiLoginPhase follows the account's.
  settings_.login_phase = phase;
  MarkDirty();
  Notify(PreferenceChange::kAccess);
}

void PhiPreferences::SetAllowAgentControl(bool enabled) {
  // The Settings window, or the connection prompt's Allow — never a verb
  // (see agentSpace.settings.set), so an agent cannot admit itself.
  if (settings_.allow_agent_control == enabled) {
    return;
  }
  settings_.allow_agent_control = enabled;
  LOG(INFO) << "[PhiAgent] agent control "
            << (enabled ? "enabled" : "disabled");
  MarkDirty();
  Notify(PreferenceChange::kAgentControl);
}

bool PhiPreferences::AllowAgentControl() const {
  return settings_.allow_agent_control;
}

bool PhiPreferences::AgentAutoView() const {
  return settings_.agent_auto_view;
}

void PhiPreferences::RequestAgentAutoView(bool enabled) {
  SetAgentAutoView(enabled);
}

void PhiPreferences::SetAgentAutoView(bool enabled) {
  if (settings_.agent_auto_view == enabled) {
    return;
  }
  settings_.agent_auto_view = enabled;
  MarkDirty();
  Notify(PreferenceChange::kAgentAutoView);
}

void PhiPreferences::SetAllowAgentUserSpaces(bool enabled) {
  // Only the Settings window reaches here — never a verb (see
  // agentSpace.settings.set), so a connected agent cannot widen its own
  // reach.
  if (settings_.allow_agent_user_spaces == enabled) {
    return;
  }
  settings_.allow_agent_user_spaces = enabled;
  LOG(INFO) << "[PhiAgent] agent access to user Spaces "
            << (enabled ? "enabled" : "disabled");
  MarkDirty();
  Notify(PreferenceChange::kAgentUserSpaces);
}

bool PhiPreferences::AgentProfileAllowed(
    const std::string& profile_id) const {
  const auto& blocked = settings_.disallowed_agent_profile_ids;
  return std::find(blocked.begin(), blocked.end(), profile_id) ==
         blocked.end();
}

void PhiPreferences::SetAgentProfileAllowed(const std::string& profile_id,
                                            bool allowed) {
  // Settings-window only, like the two gates above: an agent that could
  // clear its own refusal would make the switch decorative.
  if (profile_id.empty() || AgentProfileAllowed(profile_id) == allowed) {
    return;
  }
  std::vector<std::string>& blocked = settings_.disallowed_agent_profile_ids;
  if (allowed) {
    std::erase(blocked, profile_id);
  } else {
    blocked.push_back(profile_id);
  }
  LOG(INFO) << "[PhiAgent] agent Spaces in profile " << profile_id << " "
            << (allowed ? "allowed" : "refused");
  MarkDirty();
  Notify(PreferenceChange::kAgentUserSpaces);
}

void PhiPreferences::SetAnalyticsEnabled(bool enabled) {
  if (enabled == settings_.analytics_enabled) {
    return;
  }
  settings_.analytics_enabled = enabled;
  MarkDirty();
  Notify(PreferenceChange::kAnalytics);
}

// --- Language ---------------------------------------------------------------

std::string PhiPreferences::LanguagePreference() const {
  return PhiLanguagePreference();
}

void PhiPreferences::SetLanguage(const std::string& tag) {
  const bool active_changed = PhiSetLanguage(tag);
  // The catalog's normalised preference is what the file carries.
  settings_.language = PhiLanguagePreference();
  MarkDirty();
  Notify(PreferenceChange::kLanguage);
  // Every piece of chrome caches the strings it was built with, so the whole
  // UI is rebuilt — only when the ACTIVE locale moved: picking Japanese on a
  // Japanese machine changes the preference without changing a single
  // string.
  if (active_changed) {
    Notify(PreferenceChange::kLocaleStrings);
  }
}

// --- Shortcuts --------------------------------------------------------------

std::vector<PhiShortcutRow> PhiPreferences::ShortcutRows() const {
  // The group order and the fixed, informational rows are the PANE's shape
  // and live in phi_settings_types.h; this service supplies only what it
  // owns — each group's rebindable rows, with their EFFECTIVE chords — plus
  // the catalog lookup that header cannot make itself.
  const auto label = [](const char* key) {
    return base::WideToUTF8(PhiStr(key));
  };
  return PhiBuildShortcutRows(
      [&](std::string_view group, std::vector<PhiShortcutRow>* out) {
        for (const RebindableShortcut& def : kRebindableShortcuts) {
          if (group != def.group) {
            continue;
          }
          out->push_back(
              {std::string(group), label(def.label_key),
               base::WideToUTF8(PhiFormatChord(EffectiveChord(def.name))),
               def.name,
               settings_.shortcut_overrides.count(def.name) != 0});
        }
      },
      label);
}

void PhiPreferences::RebindShortcut(const std::string& name,
                                    UINT vk,
                                    bool ctrl,
                                    bool shift,
                                    bool alt) {
  const RebindableShortcut* target = nullptr;
  for (const RebindableShortcut& def : kRebindableShortcuts) {
    if (name == def.name) {
      target = &def;
      break;
    }
  }
  if (!target) {
    return;
  }
  // vk 0 is the UNBOUND chord (the recorder's Delete): a stored override
  // with no key, so the row stays inert across restarts; no modifier rides
  // it.
  const PhiShortcutChord chord =
      vk ? PhiShortcutChord{vk, ctrl, shift, alt} : PhiShortcutChord{};
  // Steal the chord from whichever rebindable currently answers to it — the
  // stolen shortcut goes UNBOUND (vk 0) until given a new chord or restored.
  for (const RebindableShortcut& def : kRebindableShortcuts) {
    if (chord.vk && name != def.name && EffectiveChord(def.name) == chord) {
      settings_.shortcut_overrides[def.name] = {};
    }
  }
  // Rebinding back to the default is a restore, not an override.
  if (chord == target->def) {
    settings_.shortcut_overrides.erase(name);
  } else {
    settings_.shortcut_overrides[name] = {chord.vk, chord.ctrl, chord.shift,
                                          chord.alt};
  }
  SaveShortcuts();
  Notify(PreferenceChange::kShortcuts);
}

void PhiPreferences::RestoreShortcut(const std::string& name) {
  if (settings_.shortcut_overrides.erase(name) == 0) {
    return;
  }
  SaveShortcuts();
  Notify(PreferenceChange::kShortcuts);
}

void PhiPreferences::RestoreAllShortcuts() {
  if (settings_.shortcut_overrides.empty()) {
    return;
  }
  settings_.shortcut_overrides.clear();
  SaveShortcuts();
  Notify(PreferenceChange::kShortcuts);
}

bool PhiPreferences::AnyShortcutCustomized() const {
  return !settings_.shortcut_overrides.empty();
}

PhiShortcutChord PhiPreferences::EffectiveChord(const std::string& name) const {
  auto it = settings_.shortcut_overrides.find(name);
  if (it != settings_.shortcut_overrides.end()) {
    const model::ShortcutChord& chord = it->second;
    return {chord.vk, chord.ctrl, chord.shift, chord.alt};
  }
  for (const RebindableShortcut& def : kRebindableShortcuts) {
    if (name == def.name) {
      return def.def;
    }
  }
  return {};
}

UINT PhiPreferences::MatchRebindableChord(UINT vk,
                                          bool ctrl,
                                          bool shift,
                                          bool alt) const {
  const PhiShortcutChord pressed{vk, ctrl, shift, alt};
  for (const RebindableShortcut& def : kRebindableShortcuts) {
    const PhiShortcutChord chord = EffectiveChord(def.name);
    if (chord.vk && chord == pressed) {
      return def.command;
    }
  }
  return 0;
}

bool PhiPreferences::IsDisplacedDefaultChord(UINT vk,
                                             bool ctrl,
                                             bool shift,
                                             bool alt) const {
  const PhiShortcutChord pressed{vk, ctrl, shift, alt};
  for (const RebindableShortcut& def : kRebindableShortcuts) {
    if (def.def == pressed && EffectiveChord(def.name) != pressed) {
      return true;
    }
    // Chromium binds Ctrl+Shift+= beside Ctrl+= for zoom in (the shifted
    // key is the "+" the label names); the twin goes dead with its row.
    if (def.def.vk == VK_OEM_PLUS && !def.def.shift) {
      PhiShortcutChord twin = def.def;
      twin.shift = true;
      if (pressed == twin && EffectiveChord(def.name) != def.def) {
        return true;
      }
    }
  }
  return false;
}

std::optional<std::wstring> PhiPreferences::RebindableChordLabel(
    UINT command) const {
  for (const RebindableShortcut& def : kRebindableShortcuts) {
    if (def.command == command) {
      const PhiShortcutChord chord = EffectiveChord(def.name);
      return chord.vk ? PhiFormatChord(chord) : std::wstring();
    }
  }
  return std::nullopt;
}

// --- Mirrors ----------------------------------------------------------------

void PhiPreferences::SetReaderStyle(const model::ReaderStyle& style) {
  settings_.reader = style;
  MarkDirty();
}

void PhiPreferences::SetPinScopeMirror(PhiPinScope scope) {
  if (settings_.pin_scope == scope) {
    return;
  }
  settings_.pin_scope = scope;
  MarkDirty();
  Notify(PreferenceChange::kPinScope);
}

void PhiPreferences::MarkDirty() {
  if (dirty_) {
    dirty_();
  }
}

void PhiPreferences::Notify(PreferenceChange what) {
  // Over a copy: an observer may unsubscribe (or open a window that
  // subscribes) from inside the callback.
  const std::vector<Observer*> observers = observers_;
  for (Observer* observer : observers) {
    observer->OnPreferencesChanged(what);
  }
}

}  // namespace phi_client

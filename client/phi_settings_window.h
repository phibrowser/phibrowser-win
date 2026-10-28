// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SETTINGS_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SETTINGS_WINDOW_H_

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_popup_chrome.h"
#include "phibrowser-win/client/phi_holographic_card.h"
#include "phibrowser-win/client/phi_secondary_window.h"
#include "phibrowser-win/client/phi_settings_types.h"
#include "phibrowser-win/client/phi_sync_keys.h"
#include "phibrowser-win/client/phi_tab_store.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_shell_window.h"

namespace phi_client {

class PhiAccountAndFeedback;
class PhiPreferences;
class PhiProfileService;
class PhiSpaceManager;
class PhiUrlRouter;
class PhiSyncKeyService;

// Is Phi the browser Windows launches for an http: URL? A registry read
// (AssocQueryString through the user's choice, compared to this exe's
// path) — the settings pane's Default Browser row, and the launch-time
// settings snapshot the analytics report.
bool PhiThisExeIsDefaultBrowser();

// What the New Tab shortcut opens — the Mac's NewTabShortcutBehavior. The
// windows-free model header carries the enum (phi_app_model.h); this is the
// UI side's spelling of the SAME type, not a second one to cast between.
using PhiNewTabBehavior = model::NewTabBehavior;

// What changed behind the host, for a window showing it. The view
// subscribes (PhiSettingsHost::AddSettingsObserver) and repaints on a
// notification; the model side no longer reaches it through the static
// PhiSettingsWindow::Refresh() (lane 1 D9).
enum class PhiSettingsChange {
  kPreferences,  // Any PhiPreferences domain (theme, prefs, language...).
  kSpaces,       // The Space list, a Space's theme, icon or profile.
  kProfiles,     // A profile was created, renamed, deleted or imported.
  kAccount,      // The Auth0 session settled or was signed out.
  kStoredRows,   // Pinned tabs / bookmarks changed.
  kConsent,      // The credential audit trail grew.
};
class PhiSettingsObserver {
 public:
  virtual void OnSettingsChanged(PhiSettingsChange what) = 0;

 protected:
  virtual ~PhiSettingsObserver() = default;
};

// What the settings window asks the app to DO, plus the doors to the
// services that own the values it shows.
//
// It used to be 75 virtuals, most of them one-line forwards from the
// coordinator to a service, several of them handing rows across as
// TAB-SEPARATED STRINGS that both sides split apart again by position
// (docs/architecture-review.md §3.5; lane 1 §1.3 and §5 D7). Phase 6 splits
// it in two:
//
//   READS go to the service that owns the domain. PhiPreferences,
//   PhiSpaceManager, PhiProfileService and PhiAccountAndFeedback are
//   handed to the window CONST — it can see the
//   values, it cannot write behind the coordinator's back — and every row
//   they answer is a struct (phi_settings_types.h).
//
//   ACTIONS stay here, because each one needs something the service cannot
//   reach on its own: a framework push, a window, another store, a Chromium
//   profile. The exception is PhiUrlRouter, handed over MUTABLE: it is the
//   sole owner of the rules half of PhiSpaces.json and persists and
//   re-pushes the routing table itself, so the coordinator added nothing
//   between its editor (PhiUrlRulesWindow) and it.
class PhiSettingsHost {
 public:
  virtual ~PhiSettingsHost() = default;

  // The window's subscription to changes behind the host (above). The
  // window adds itself when it opens and removes itself when it goes.
  virtual void AddSettingsObserver(PhiSettingsObserver* observer) = 0;
  virtual void RemoveSettingsObserver(PhiSettingsObserver* observer) = 0;

  // --- The services the window reads directly -------------------------------
  // All outlive the window (the coordinator owns them for the app's life).
  virtual const PhiPreferences& Preferences() const = 0;
  virtual const PhiSpaceManager& SpaceList() const = 0;
  virtual const PhiProfileService& ProfileList() const = 0;
  // Mutable too: the Account pane drives sign-in, rename and the avatar
  // editor on the service itself.
  virtual PhiAccountAndFeedback& Account() = 0;
  // Mutable: see the class comment — the URL Rules editor (PhiUrlRulesWindow,
  // opened through the host below) IS this service's editor.
  virtual PhiUrlRouter& UrlRules() = 0;
  // Navigation ▸ "Manage URL Rules…": the universal editor window, over
  // `anchor` (the settings window).
  virtual void RequestUrlRulesEditor(HWND anchor) = 0;
  // --- Sync (docs/user-data-parity.md S3) -------------------------------------
  // The signed-in account's key layer; null signed out or in Guest Mode.
  virtual PhiSyncKeyService* SyncKeys() = 0;
  // "Set up sync on this device" / "Continue setup": the setup window, over
  // `anchor`.
  virtual void RequestSyncSetup(HWND anchor) = 0;

  // --- Theme ----------------------------------------------------------------
  virtual void SetThemeId(const std::string& id) = 0;
  virtual void SetAppearance(PhiAppearance appearance) = 0;
  virtual void SetSaturation(int value) = 0;  // 0..100, the Mac's range.

  // The client's one analytics capture path (phi_client_analytics.h):
  // the harness ring, then the collector's consent gate and allow-list.
  virtual void CaptureAnalytics(const char* module,
                                const char* event_name,
                                base::DictValue properties) = 0;

  // --- Account (the Auth0 session; stage 1 of bucket D's auth work) ---------
  virtual void RequestSignIn() = 0;
  virtual void RequestSignOut() = 0;
  // Registers Phi with Windows and opens the system Default apps page. The
  // choice itself is the user's to make there — Windows 10 and later allow
  // no other way.
  virtual void RequestDefaultBrowser() = 0;

  // --- Appearance -----------------------------------------------------------
  virtual void RequestLayoutMode(PhiLayoutMode mode) = 0;

  // --- Browsing -------------------------------------------------------------
  virtual void SetNewTabBehavior(PhiNewTabBehavior behavior) = 0;
  virtual void SetAutoPictureInPictureMode(const std::string& mode) = 0;
  virtual void SetAlwaysShowFullUrl(bool enabled) = 0;
  // GeneralSettings.showTabPreviews — the hover cards on tab, pin and
  // bookmark rows. On by default, as on Mac.
  virtual void SetShowTabPreviews(bool enabled) = 0;
  // GeneralSettings.showOpenTabIndicators — the dot on inactive open pins
  // and bookmarks. Off by default, as on Mac.
  virtual void SetShowOpenTabIndicators(bool enabled) = 0;
  // GeneralSettings.dimUnloadedTabIcons — a reclaimed tab's favicon and
  // open dot at 30%. On by default, as on Mac.
  virtual void SetDimUnloadedTabIcons(bool enabled) = 0;
  // The UI language. An EMPTY tag means "follow the operating system", which
  // is the default and the only value that tracks a later OS change.
  virtual void SetLanguagePreference(const std::string& tag) = 0;
  // "Restore last session": app-wide choice, pushed into every loaded
  // profile's Chromium startup pref (SessionStartupPref LAST).
  virtual void SetRestoreLastSession(bool enabled) = 0;
  // "Share anonymous usage analytics": the telemetry consent. Off by
  // default; only this pane can turn it on (never the control channel).
  virtual void SetAnalyticsEnabled(bool enabled) = 0;

  // --- Spaces / Profiles ----------------------------------------------------
  // The pin scope's truth is the TAB STORE, not the preferences (the
  // settings file only mirrors it), and setting it re-owns every stored
  // row — so it stays an action here rather than a service read.
  virtual PhiPinScope PinScope() const = 0;
  virtual void SetPinScope(PhiPinScope scope) = 0;
  // A Space's own theme, icon and saturation override (the Mac's per-Space
  // settings; saturation -1 follows the app-wide slider).
  virtual void SetSpaceTheme(const std::string& space_id,
                             const std::string& theme_id) = 0;
  virtual void SetSpaceIcon(const std::string& space_id,
                            const std::string& icon) = 0;
  virtual void SetSpaceSaturation(const std::string& space_id, int value) = 0;
  // The Spaces editor's "Color ›" row: the Space theme editor (the palette
  // field) as a popover owned by `owner`, beside `anchor_screen`. False
  // when there is no browser window to drive it.
  virtual bool OpenSpaceThemeEditor(const std::string& space_id,
                                    HWND owner,
                                    const RECT& anchor_screen) = 0;
  // Re-binds a Space to another profile (the expander's Profile pick,
  // windows-native-ui.md §7.1). Refused for the default Space.
  virtual bool SetSpaceProfile(const std::string& space_id,
                               const std::string& profile_id) = 0;
  // The window's client size on every resize, remembered across opens.
  virtual void SetSettingsWindowSize(int width, int height) = 0;
  // Space / profile management, with the Mac's guards (see SpacesHost).
  virtual bool RenameSpace(const std::string& space_id,
                           const std::string& name) = 0;
  virtual bool DeleteSpace(const std::string& space_id) = 0;
  // Moves a Space to `new_index` in the Space list — the Spaces list's
  // grip. The SAME signature as SpacesHost's chip drag, so the
  // coordinator's one override serves both.
  virtual void ReorderSpace(const std::string& space_id, int new_index) = 0;
  virtual bool DeleteProfileById(const std::string& profile_id) = 0;
  virtual bool RenameProfileById(const std::string& profile_id,
                                 const std::string& name) = 0;
  virtual void CreateProfileNamed(const std::string& name) = 0;
  // Per-profile browsing settings (the Mac profile pane's two controls).
  // The engine LIST is read off PhiProfileService; setting one goes through
  // the framework's keyword service, so it comes back here.
  virtual bool SetSearchEngine(const std::string& profile_id,
                               const std::string& keyword) = 0;
  virtual bool SetDownloadDirectory(const std::string& profile_id,
                                    const std::string& dir) = 0;
  // Creates a Space (Default profile) — the fallback behind the list's "+"
  // when no browser window can host the form's outcome.
  virtual void CreateSpaceNamed(const std::string& name) = 0;
  // The list's "+": the standalone create-Space window (the Mac's
  // SpacesSettingsView.newSpace always presents the floating panel — a
  // sidebar overlay would open in a browser window behind Settings), for
  // the focused browser window's slot, centred on `anchor`. False when
  // there is no browser window to create the Space in.
  virtual bool OpenCreateSpaceWindow(const std::string& initial_profile_id,
                                     HWND anchor) = 0;
  // Spaces ▸ "Manage Spaces in Library": the Library overlay on its Spaces
  // section over the focused browser window (phi_library.h). False when
  // there is no browser window to open it over; the Settings window hides
  // itself on true (the Mac's openLibrary then closeSettings).
  virtual bool OpenLibrarySpaces() = 0;

  // --- Navigation -----------------------------------------------------------
  // Peek View, the Mac's peekViewEnabled / autoPeekViewEnabled pair. The
  // master gate governs the "Open Link in Peek View" context-menu item and,
  // under it, the automatic diversion; the sub-gate governs that diversion
  // alone — a cross-site link leaving a bookmark- or pinned-bound tab
  // previews in a Peek instead of taking a new tab. Both are pushed into the
  // framework, which is where both decisions are actually made.
  virtual void SetPeekViewEnabled(bool enabled) = 0;
  virtual void SetAutoPeekViewEnabled(bool enabled) = 0;
  // Kiosk, the Mac's openKioskOnCommandOptionClick: a link clicked with
  // Ctrl+Alt held opens in a Kiosk window. Pushed into the framework too —
  // Browser::OpenURLFromTab is where the click is remapped.
  virtual void SetOpenKioskOnCtrlAltClick(bool enabled) = 0;

  // --- Folio (docs/folio-port.md) -------------------------------------------
  // The global folder, a profile's override (empty path clears it), and the
  // site-action opt-in, which re-arms every profile's Mirage. Reads go
  // through Preferences().
  virtual void SetFolioGlobalFolder(const std::string& path) = 0;
  virtual void SetFolioFolderOverride(const std::string& profile_id,
                                      const std::string& path) = 0;
  virtual void SetFolioAutoSaveOnSiteActions(bool enabled) = 0;
  // Choose sites…: the extension's per-site auto-save switches.
  virtual void OpenFolioAutoSaveSites() = 0;

  // --- Shortcuts (the Mac Shortcuts pane's customising) ---------------------
  // Rebinding takes the chord's raw parts; a chord already held by another
  // rebindable shortcut is STOLEN (that one goes unbound), as the Mac pane
  // does. The keyboard hook reads the table live, so this is a push, not a
  // plain preference write.
  virtual void RebindShortcut(const std::string& name,
                              UINT vk,
                              bool ctrl,
                              bool shift,
                              bool alt) = 0;
  // A customised row's Reset: erases that one override.
  virtual void RestoreShortcut(const std::string& name) = 0;
  virtual void RestoreAllShortcuts() = 0;

  // --- Developer ------------------------------------------------------------
  // Developer mode gates the Developer pane, as on Mac (default off).
  virtual void SetDeveloperMode(bool enabled) = 0;
  // Developer ▸ "Allow agents to control Phi": the master switch. On, any
  // local program holding the launch token is admitted with no prompt and
  // no identity check (there is none to make on Windows); off, the first
  // connection asks the user to turn it on. Never agent-settable.
  virtual void SetAllowAgentControl(bool enabled) = 0;
  // Developer ▸ "Allow agents to operate your Spaces": lets a connected
  // agent read and change the user's OWN Spaces, tabs, bookmarks, URL rules
  // and profiles. Default off; agent Spaces are never affected, and the
  // agent cannot flip it over the control channel.
  virtual void SetAllowAgentUserSpaces(bool enabled) = 0;
  // Developer ▸ "Profiles agents can use": which profiles an agent may
  // bind a NEW agent Space to. Turning one off stops the next
  // agentSpace.create against it; Spaces already open are untouched.
  virtual void SetAgentProfileAllowed(const std::string& profile_id,
                                      bool allowed) = 0;
  // "Additional browser settings" — opens chrome://settings in a tab.
  virtual void OpenBrowserSettings() = 0;
  // Opens any browser page in a tab — the Mac profile pane's data links
  // (chrome://settings/privacy and friends).
  virtual void OpenBrowserPage(const std::string& url) = 0;
  // The same, in a browser of `profile_id` — a window showing one of its
  // Spaces, else a new window of one. Never another profile's: nothing
  // opens when the profile has no Space to open in.
  virtual void OpenBrowserPageInProfile(const std::string& profile_id,
                                        const std::string& url) = 0;
};

// The settings window, shaped like the Mac's Preferences: a TOOLBAR of panes
// across the top (SettingsWindowController is created with style:
// .toolbarItems — not a sidebar), and below it sections, each a small title
// over a rounded CARD holding its rows (GeneralSectionView wrapping a
// GeneralContainerView; label left, control trailing). A real
// top-level window, NOT an overlay inside a shell: the content region belongs
// to Chromium's DirectComposition surface, which composites above anything GDI
// draws there (windows-framework-design.md — the falsified overlay approach).
class PhiSettingsWindow : public PhiSecondaryWindow,
                          public PhiSettingsObserver {
 public:
  static void Open(PhiSettingsHost* host);
  // Repaints the open window. The coordinator no longer calls this (it
  // notifies the observer instead); the verb ladder still does for the
  // profile settings it writes through the framework.
  static void Refresh();
  // The open window's HWND, or null. Reported over the control socket: a caller
  // in another session cannot enumerate our windows to find it.
  static HWND Handle();
  static std::string CurrentSection();
  static bool ShowSection(const std::string& name);
  // Lands on Spaces with `space_id` selected — the detail column IS the icon
  // picker, so this is the "Change Icon…" destination. Needs an open window
  // (callers Open() first).
  static void ShowSpacePicker(const std::string& space_id);

  ~PhiSettingsWindow() override;

  // PhiSettingsObserver: whatever changed behind the host, the pane
  // rebuilds NOW and repaints — never from inside WM_PAINT (lane 3 §3.1
  // and ranked finding 1; docs/sidebar-architecture.md §2.7).
  void OnSettingsChanged(PhiSettingsChange what) override;

 private:
  explicit PhiSettingsWindow(PhiSettingsHost* host);
  bool OpenWindow();

  // PhiSecondaryWindow. Paint() only paints: the item list it draws was
  // built by the last ScheduleRebuild().
  void Paint(HDC dc, const RECT& dirty) override;
  LRESULT OnWindowMessage(UINT msg,
                          WPARAM wparam,
                          LPARAM lparam,
                          bool* handled) override;
  void OnDpiChanged(UINT old_dpi) override;
  void OnTimerFired(UINT_PTR id) override;
  void OnCaptureLost() override;
  void OnNcDestroy() override;

  // One drawn thing. Rows are built per section and hit-tested from the same
  // list, so what is drawn and what is clickable cannot drift apart.
  enum class ItemKind {
    kToolbarItem,    // A page, in the navigation rail down the left.
    kPageTitle,      // The page's 28/600 title.
    kCard,           // The well a section's rows sit in.
    kSectionTitle,   // Small secondary label ABOVE a card.
    kSubtitle,       // 11pt tertiary line under a section title.
    kRowLabel,
    kHint,
    kLink,           // 12pt accent text ("Edit in … settings…").
    kDivider,        // The 1px separator between card rows.
    kSwatch,         // A theme dot: 22pt circle, 2pt ring when selected.
    kImageCard,      // 122x72 picture card (layout/appearance/new-tab).
    kToggle,
    kSliderTrack,
    kPopup,          // Trailing pill that opens a popup menu on click.
    kListRow,        // Master-list row (Spaces / Profiles).
    kToolButton,     // 34x34 glyph button in a list's bottom strip.
    kBadge,          // Small capsule ("Default", "Not linked", …).
    kButton,
    kIcon,           // A lone MDL2 glyph (row gutters, drag handles).
    kKeycap,         // Shortcuts' bordered key pill (min width 80, r6).
    kSearchField,    // Shortcuts' in-card filter field.
    kIconChip,       // The Mac's 24pt SettingsIconChip leading a row.
    kAvatar,         // The account's picture as a circle; `text` = fallback glyph.
    kEditableLabel,  // A 13pt semibold label trailed by a pencil; click edits.
    kProfileCard,    // The membership card (PhiHolographicCard).
    kExpander,       // An expander card's header row (§7.1 / §7.2).
    kAppMark,        // The Phi mark at `bounds` size (About).
    kAccountBlock,   // The rail's account block: 44 avatar, name, email.
    kLayoutCard,     // The Layout picker's half: 110 preview + radio row.
    // Settings › Spaces (the two-pane card, design node 9056:18105).
    kSpaceCard,      // The card: `value` = divider x, `total` = bar line y.
    kSpaceRow,       // A Space list row's body: icon, name (to `value` px).
    kDragHandle,     // A row's three-line reorder grip.
    kRowPopup,       // A row's profile pill; `on` = its row is selected.
    kBarButton,      // The list's + / − / edit glyph buttons.
    kHairline,       // A 1 px 10 % rule over `backdrop` (or the card).
    kGroupBox,       // A 3 % grey well; `value`/`visible`/`total` = grid scroll.
    kPaneLabel,      // 13 px label inside the card; `value` 1 = centred note.
    kSegment,        // Icon | Emoji: the pill (no action), then its segments.
    kIconSearch,     // The embedded picker's search pill (a text input).
    kPickerCell,     // One glyph of the embedded picker; `value` = its index.
    kUrlRow,         // The page's "URL Rules" row under the card.
    kThemeColor,     // A "Color ›" row: `colors` as a 20 dot, a chevron.
  };
  struct Item {
    ItemKind kind = ItemKind::kRowLabel;
    RECT bounds{};
    std::wstring text;
    std::wstring detail;  // Second line / trailing text, kind-dependent.
    std::wstring icon;    // A list row's leading Space icon (glyph text).
    bool selected = false;
    bool on = false;
    bool enabled = true;  // Drawn dim and inert when false.
    // Multi-line text, measured at build time and drawn DT_WORDBREAK. The
    // Mac wraps every label and description
    // (`fixedSize(horizontal: false, vertical: true)`); a single line with
    // DT_END_ELLIPSIS chopped the longer ones mid-sentence.
    bool wrap = false;
    // A kButton whose label reads in the destructive red (Delete Space).
    bool danger = false;
    COLORREF swatch = 0;
    COLORREF ring = 0;   // A swatch's selection-ring color (its own accent).
    // kThemeColor: the theme's colours, top to bottom (one is flat).
    std::vector<COLORREF> colors;
    int value = 0;       // Slider position, or an option index.
    std::string action;  // What a click does; empty = not clickable.
    // The action's ONE target id (a theme, a Space, a profile, a rule, a
    // shortcut name). `space` carries the Space a Space-SCOPED action (a
    // theme dot, an icon cell) applies to, so no item packs two ids into
    // one tab-separated payload the click handler has to split again.
    std::string payload;
    std::string space;
    // The profile a profile-SCOPED action (a "Your data" page) runs in.
    std::string profile;
    // The accessible name when the drawn text is not one (the Spaces
    // card's glyph buttons, a row's profile pill); a slider's LABEL.
    std::wstring a11y;
    // The ground an item sits on when it is not the card (the Spaces
    // editor's grey wells), for the paint's blends.
    COLORREF backdrop = CLR_INVALID;
    // A scrolled grid's extent, in rows (kGroupBox).
    int total = 0;
    int visible = 0;
    // False keeps a focusable item out of the Tab walk (an unselected
    // Space row's grip and profile pill: the list is ONE stop).
    bool tab_stop = true;
  };

  void Rebuild();
  // THE way the pane changes: rebuild the item list from the host, then
  // ask for a repaint. Everything that used to InvalidateRect(TRUE) and
  // let WM_PAINT rebuild calls this instead.
  void ScheduleRebuild();
  void BuildAccount(int* y);
  void BuildGeneral(int* y);
  void BuildAppearance(int* y);
  void BuildAbout(int* y);
  void BuildAdvanced(int* y);
  void BuildSpaces(int* y);
  // The Mac's Navigations pane: Kiosk, Peek and URL Rules.
  void BuildNavigation(int* y);
  // The "URL Rules" section the Spaces and Navigation panes share.
  void AddUrlRulesSection(int* y);
  // The URL Rules editor, a drill-in of the Navigation pane (the Mac's
  // universal editor window).
  void BuildProfiles(int* y);
  void BuildAi(int* y);
  void BuildShortcuts(int* y);
  void BuildDeveloper(int* y);
  void BuildSync(int* y);
  // The paired pane's status card (the Mac's statusSection).
  void BuildSyncStatus(int* y);
  // Settings ▸ Sync's device list, fetched through the key layer while the
  // pane is up (the Mac's 3 s poll).
  void RefreshSyncDevices(bool force);
  // The Mac's ProfileDetailSettingsView: the Search Engine / Download
  // Location card, then "Your Data and Settings" links. Shared by the
  // Profiles detail column and the General pane's single-profile case.
  // `show_folio_folder` (the Mac's showsFolioFolder) adds the profile's
  // Folio Folder row to the card; General passes false, since its own
  // Folio section above already shows the folder.
  void BuildProfileDetail(int* y, int left, int right,
                          const std::string& profile_id,
                          bool show_folio_folder);
  // One expander card's header row (windows-native-ui.md §7.1 / §7.2):
  // a tile (a Space icon, or a profile avatar when `avatar`, a filled
  // circle with `icon` as its initial), the 14 name, the 12 hint, an
  // optional "Default" badge and the chevron. Clicking runs `action`.
  void AddExpander(int* y, const std::wstring& name,
                   const std::wstring& hint, const std::wstring& icon,
                   bool avatar, COLORREF tint, bool expanded, bool badge,
                   const std::string& action, const std::string& payload);
  // An expander body's row: the label in the row text's column, the
  // control kDetailLabelWidth further in; returns the control's rect.
  RECT AddDetailRow(int* y, const wchar_t* label, int control_height,
                    int control_width);
  // Settings › Spaces' two-pane card: the Space list (reorder grip, icon,
  // name, profile pill; + / − / edit under it) beside the selected Space's
  // editor (colour, saturation, the embedded icon / emoji picker).
  void BuildSpaceCard(int* y,
                      const std::vector<PhiSpaceSummary>& spaces,
                      const std::vector<PhiProfileSummary>& profiles);
  void BuildSpaceEditor(const RECT& pane, const PhiSpaceSummary& space);
  // The design's "URL Rules" row closing the Spaces page.
  void AddSpacesUrlRow(int* y);
  // Selects a Space in the list; a new selection resets the picker (tab,
  // search, grid scroll). `reveal` scrolls the list to it.
  void SelectSpace(const std::string& space_id, bool reveal);
  // Keyboard: the list's Up / Down (selects), the grip's Up / Down
  // (moves the Space), the picker grid's arrows (focus, scrolling).
  void MoveSpaceSelection(int delta);
  void ReorderByKey(int delta);
  void MoveIconFocus(int delta);
  // The grip's drag: the list index under client `y`, and the drop.
  int ReorderIndexAt(int y);
  void EndReorder(bool commit);
  // The profile expander's body (name, search engine, download location,
  // used by, data pages, delete), inside the open card.
  void AddProfileBody(int* y, const PhiProfileSummary& profile,
                      const std::vector<PhiSpaceSummary>& spaces);
  // The rail's "Find a setting" filter: true when `title` or `hint`
  // contains the typed text (case-folded), or when nothing is typed.
  bool SearchMatches(const wchar_t* title, const wchar_t* hint) const;
  // The results page the filter builds: every page's matching rows under
  // that page's heading.
  void BuildSearchResults(int* y);
  // A section = a small title, then a card holding its rows. BeginSection
  // remembers where the card starts; EndSection sizes it around the rows that
  // were added, the way the Mac's GeneralSectionView wraps a
  // GeneralContainerView.
  void BeginSection(int* y, const wchar_t* title);
  void EndSection(int* y);
  void AddHint(int* y, const wchar_t* text);
  // A section title with the Mac's 11pt description line under it, for a
  // card BeginSection then opens title-less.
  void AddSectionHeading(int* y, const wchar_t* title,
                         const wchar_t* description);
  // The 1px separator between two rows of the open card.
  void AddDividerRow(int* y);
  // An 11pt tertiary line tucked under the row just added.
  void AddSubHint(int* y, const wchar_t* text);
  // Label + trailing switch, one call — most Mac rows are exactly this.
  // `hint` is the row's own 11pt description, part of the SAME row.
  void AddToggleRow(int* y, const wchar_t* title, bool on, bool enabled,
                    const std::string& action,
                    const wchar_t* hint = nullptr,
                    int text_indent = 0,
                    wchar_t chip_glyph = 0,
                    COLORREF chip_color = 0,
                    int row_padding = -1);
  // A row inside the open card; returns the rect its control occupies,
  // RIGHT-ALIGNED as on Mac (label left, Spacer, accessory trailing).
  //
  // `hint`, when given, is the Mac's 11pt description line 4pt under the
  // title and INSIDE the row (GeneralSettingView stacks them in one
  // VStack): the trailing control centres on the pair, and the divider a
  // caller adds next lands after the description rather than between a
  // title and the sentence explaining it.
  // `text_indent` moves the text block right of the card's padding — the
  // Mac's `.padding(.leading, 36)` on the rows under a chip-led header, so
  // they line up with that header's text rather than with its icon.
  // `chip_glyph` / `chip_color` draw the Mac's SettingsIconChip in the 24pt
  // gutter that indent leaves, top-aligned with the title.
  // `row_padding` is the Mac's `.padding(.vertical, …)`: the card default
  // for a header row, 6 for the tight rows grouped under one. -1 takes the
  // default (the metric itself lives with the other metrics, in the .cc).
  RECT AddRow(int* y, const wchar_t* title, int control_height,
              int control_width, const wchar_t* hint = nullptr,
              int text_indent = 0, wchar_t chip_glyph = 0,
              COLORREF chip_color = 0, int row_padding = -1);
  // Height of `text` wrapped into `width` with `font` — DT_CALCRECT
  // through the same DrawTextW the paint pass ends in, so what is measured
  // and what is drawn cannot disagree.
  int WrappedHeight(HFONT font, const wchar_t* text, int width) const;
  // A ComboBox pill's width (dip) for its current `text`: kPillWidth at
  // least, grown to fit a longer (translated) value up to kPillMaxWidth,
  // past which the paint ellipsizes.
  int PillWidthFor(const std::wstring& text) const;
  // AddRow + its trailing ComboBox pill showing `value`, sized to fit it;
  // a click runs `action`.
  void AddPopupRow(int* y, const wchar_t* title, const std::wstring& value,
                   const char* action, const wchar_t* hint, int text_indent,
                   wchar_t chip_glyph);
  void AddChips(const RECT& area,
                const std::vector<std::wstring>& labels,
                int selected_index,
                const std::string& action);

  // Hover, press and the slider drag. Every control here was inert under
  // the pointer (no hover wash, no hand cursor) and FIRED on the press —
  // and again on every mouse-move while the button stayed down, so a drag
  // across the pane flipped every switch it crossed. The base's hit list
  // (phi_window.h: "hits_ is THE rect list") owns all of it now, which is
  // also what makes the window answer UI Automation.
  void OnMouseDown(int hit, POINT client_pt) override;
  void OnClick(int hit, POINT client_pt) override;
  void OnWheel(int delta, POINT client_pt) override;
  void OnEscape() override;
  // UIA's Invoke. A row scrolled out of the page is published with an
  // empty rect (PublishHits), which no synthesized click can land on, so
  // the row is scrolled into view first and THEN clicked through the base.
  bool InvokeHit(int index) override;
  // items_ index for a hit index, or -1. The hit's `key` IS that index.
  int ItemOfHit(int hit) const;
  // The hit index publishing items_[item], or -1.
  int HitOfItem(int item) const;

  // --- Keyboard focus (docs/2026-10-10-release-review.md: Settings had no
  // keyboard model at all) -------------------------------------------------
  // One drawn item takes the focus at a time. Tab / Shift+Tab walk the TAB
  // STOPS in build order — which is visual order: the rail, then the page
  // top to bottom — and a GROUP (the rail's pages, one row of theme dots,
  // one picture-card picker) is a single stop whose members the arrows walk,
  // as a Windows radio group is. Space / Enter act exactly as a click does
  // (Apply), sliders step on Left / Right, and the focused item is scrolled
  // into view. The ring is drawn only after keyboard use; a press hides it.
  //
  // Identity across a rebuild: items_ is rebuilt on every change, so the
  // focus is remembered by WHAT it is (kind, action, ids, and its ordinal
  // among equals) and found again after each Rebuild().
  struct FocusKey {
    ItemKind kind = ItemKind::kRowLabel;
    std::string action;
    std::string payload;
    std::string space;
    int value = 0;
    int ordinal = 0;  // Among earlier items with the same key.
  };
  bool Focusable(const Item& item) const;
  // A text input: the rail's "Find a setting" or the Shortcuts filter.
  // Typing goes into it, so Space types rather than activates.
  bool IsTextInput(const Item& item) const;
  // Items in the rail (never scrolled; their ring is clipped to the rail).
  bool InRail(const Item& item) const;
  // Whether two focusable items are members of one arrow-key group.
  bool SameFocusGroup(const Item& a, const Item& b) const;
  // The focusable items in order, and the group (tab stop) each one is in.
  void FocusOrder(std::vector<int>* order, std::vector<int>* group) const;
  FocusKey FocusKeyOf(int item) const;
  bool MatchesFocusKey(const Item& item, const FocusKey& key) const;
  // Re-finds the focused item after a rebuild (or the nearest focusable
  // one when it went away) and tells UIA which hit it is.
  void ResolveFocus();
  // Moves the focus to items_[item], scrolls it into view and announces it.
  // `keyboard` shows the ring.
  void SetFocusItem(int item, bool keyboard);
  void TabFocus(bool backward);
  // Within the focused item's group; no wrap.
  void ArrowFocus(int delta);
  // True (and the page rebuilt) when the scroll had to move.
  bool ScrollItemIntoView(int item);
  void ScrollTo(int scroll);
  int MaxScroll() const;
  // Space / Enter on the focused item: what a click on it does.
  void ActivateFocus();
  // A slider's keyboard step, in the slider's own 0..100 units.
  void StepSlider(const Item& item, int delta);
  // WM_KEYDOWN / WM_SYSKEYDOWN; true when the key was used.
  bool HandleKey(WPARAM virtual_key, LPARAM lparam, bool alt);
  // The rail field ("Find a setting") or the Shortcuts filter, or -1.
  int SearchFieldItem(bool rail) const;
  // Publishes every actionable item into hits_ — the one rect list hover,
  // clicks, tooltips and UIA all read.
  void PublishHits();
  void Apply(const Item& item, int x);
  void EnsureFont();
  // A lone MDL2 glyph at a size the ladder does not carry (the Account
  // avatar). Cached by size and dropped with the rest on a DPI move; it
  // used to be a CreateFontIndirectW / DeleteObject pair per item PER
  // PAINT (lane 3 ranked finding 12).
  HFONT IconFont(int size);
  // The window title follows the selected pane, as the Mac library does.
  void SyncTitle();

  RAW_PTR_EXCLUSION PhiSettingsHost* host_;
  // The services the panes read straight out of (see PhiSettingsHost).
  // Taken once at construction: the coordinator owns them for the app's
  // life, so they outlive every window.
  RAW_PTR_EXCLUSION const PhiPreferences* prefs_;
  RAW_PTR_EXCLUSION const PhiSpaceManager* spaces_;
  RAW_PTR_EXCLUSION const PhiProfileService* profiles_;
  RAW_PTR_EXCLUSION PhiAccountAndFeedback* account_;
  HFONT font_ = nullptr;
  HFONT title_font_ = nullptr;
  HFONT small_font_ = nullptr;  // Section titles and toolbar labels.
  HFONT hint_font_ = nullptr;   // 11pt hints and captions.
  HFONT emoji_font_ = nullptr;  // The icon-picker grid's emoji.
  HFONT mdl2_font_ = nullptr;   // The grid's Phi-icon MDL2 stand-ins.
  HFONT body_font_ = nullptr;   // 13 px: the Spaces card's text.
  HFONT page_title_font_ = nullptr;  // The page title, 28 bold.
  // The caption strip is PhiPopupChrome's now, through
  // PhiSecondaryWindow: the OS caption is retired so the title can sit
  // CENTERED with the Phi mark leading, and the close button's hover,
  // press and red-when-hot live in the frame. Lane 3 §2 counted four
  // copies of that code; this was the third.
  std::vector<std::pair<int, HFONT>> icon_fonts_;
  UINT font_dpi_ = 0;
  std::string section_ = "general";
  std::vector<Item> items_;
  // Index of the card being filled, so EndSection can size it.
  size_t open_card_ = 0;
  // Pane content can outgrow the fixed window; the wheel scrolls it. Reset on
  // pane switch, clamped to the built content's height.
  int scroll_ = 0;
  int content_height_ = 0;
  // The navigation rail's width and the page column it leaves (client
  // px), from the client rect on every rebuild (windows-native-ui.md §7).
  int rail_w_ = 0;
  int content_left_ = 0;
  int content_right_ = 0;
  // The scrollbar's thumb as last painted, and its drag.
  RECT thumb_rect_{};
  bool scroll_drag_ = false;
  int scroll_drag_grab_ = 0;  // Pointer y − thumb top at the press.
  // Master-list selections (the Mac remembers per pane).
  std::string selected_space_;
  std::string selected_profile_;
  // Settings › Spaces: the list's first shown row and whether the next
  // build scrolls the selection into it; the editor's picker tab (-1
  // follows the Space's icon, 0 Icon, 1 Emoji), search text and grid
  // scroll (first row; -1 = reveal the current icon). The rects are where
  // the wheel scrolls the list and the grid rather than the page.
  int list_first_ = 0;
  bool list_reveal_ = true;
  int list_visible_ = 0;
  int list_pitch_ = 0;
  RECT list_rect_{};
  int icon_tab_ = -1;
  std::wstring icon_search_;
  int grid_first_row_ = -1;
  int grid_rows_ = 0;
  int grid_visible_ = 0;
  int grid_count_ = 0;
  RECT grid_rect_{};
  int wheel_accum_ = 0;
  // The grip's drag: the Space being moved, the list order as it is
  // dragged (the Mac's orderedIds) and where it started.
  bool reorder_drag_ = false;
  std::string reorder_id_;
  std::vector<std::string> reorder_order_;
  int reorder_from_ = -1;
  // The Shortcuts pane's filter text ("Find shortcut keys"), fed by WM_CHAR
  // while that pane is up.
  std::wstring shortcut_search_;
  // The rail's "Find a setting" text (every other page), and the same
  // case-folded, which AddRow tests titles and hints against while a
  // search is up; `filter_hits_` counts the rows a page kept.
  std::wstring rail_search_;
  std::wstring search_filter_;
  int filter_hits_ = 0;
  // Whether the last AddRow was dropped by the filter (its divider and
  // sub-hint follow it out).
  bool last_row_skipped_ = false;
  // Where the open section began (item index and y), so EndSection can
  // drop a card the filter emptied; the heading AddSectionHeading put
  // above it goes with it.
  size_t section_start_ = 0;
  int section_start_y_ = 0;
  bool heading_pending_ = false;
  size_t heading_start_ = 0;
  int heading_start_y_ = 0;
  // Whether this exe is the system's http handler — a registry walk
  // (AssocQueryStringW). Resolved on demand, CACHED for the life of the
  // answer, and dropped only when the window is activated, which is when
  // the user comes back from the Windows Settings page that the "Set as
  // default" button opens. The Mac's twin re-checks in viewWillAppear for
  // the same reason. Before phase 5 this probe sat behind a Rebuild()
  // that ran inside WM_PAINT (lane 3 §3.1).
  std::optional<bool> is_default_browser_;
  // The item the press landed on, and the capture a slider drag holds so
  // the knob keeps following the pointer outside the window.
  int pressed_ = -1;
  bool dragging_ = false;
  // The slider the press landed on, replayed by every move of the drag —
  // re-hit-testing each move is what let a drag act on its neighbours.
  Item drag_item_;
  Capture drag_;
  // The keyboard focus (items_ index, or -1) and what it is, so a rebuild
  // can find it again; whether its ring shows (keyboard use since the last
  // press); whether the window is active (no ring on an inactive window);
  // and the WM_CHAR a Space that activated something leaves behind.
  int focus_ = -1;
  FocusKey focus_key_;
  bool focus_visible_ = false;
  bool window_active_ = false;
  bool swallow_space_char_ = false;
  // Rows added to the card BeginSection opened, so a lone hint knows
  // whether it tucks under a row or is the card's own first line.
  int card_rows_ = 0;
  // Screen-compatible DC kept for DT_CALCRECT measuring; owned.
  mutable HDC measure_dc_ = nullptr;
  // A profile whose data is still loading re-arms one short poll per
  // rebuild; the id comes from the window's own pool, not the hand-picked
  // 0x517 (lane 3 §4.1).
  Timer profile_poll_;
  // Settings ▸ You's membership card (the Mac's ProfileCardViewController):
  // the account's persisted style under the current theme's palette.
  PhiHolographicCard profile_card_;
  Timer card_timer_;
  // Settings ▸ Sync: the last device list, whether one is being fetched, and
  // the pane's action outcome lines. Replies find the window through
  // `sync_alive_` (the key layer answers asynchronously).
  Timer sync_poll_;
  std::shared_ptr<int> sync_alive_ = std::make_shared<int>(0);
  bool sync_loaded_ = false;
  bool sync_refreshing_ = false;
  uint64_t sync_refreshed_tick_ = 0;
  std::string sync_list_error_;
  std::vector<keys::AccountDevice> sync_devices_;
  std::vector<keys::PendingApproval> sync_pending_;
  std::wstring sync_action_error_;
  std::wstring sync_remove_note_;
  // The server refused a removal with 409 last_device: the Removal row shows
  // the account reset even when the device list has not caught up.
  bool sync_last_device_ = false;
  // A removal or account reset is in flight: the row's button is disabled.
  bool sync_removing_ = false;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SETTINGS_WINDOW_H_

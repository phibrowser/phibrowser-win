// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SHELL_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SHELL_WINDOW_H_

#include <windows.h>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_drag_image.h"
#include "phibrowser-win/client/phi_extension_action.h"
#include "phibrowser-win/client/phi_feedback_window.h"
#include "phibrowser-win/client/phi_browser_host.h"
#include "phibrowser-win/client/phi_caption_band.h"
#include "phibrowser-win/client/phi_content_slide.h"
#include "phibrowser-win/client/phi_app_menu.h"
#include "phibrowser-win/client/phi_native_pages_host.h"
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_omnibox_session.h"
#include "phibrowser-win/client/phi_sidebar_actions.h"
#include "phibrowser-win/client/phi_sidebar_view.h"
#include "phibrowser-win/client/phi_shell_menus.h"
#include "phibrowser-win/client/phi_shell_layout.h"
#include "phibrowser-win/client/phi_spaces_strip.h"
#include "phibrowser-win/client/phi_sidebar_model.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_model.h"

namespace phi_client {

// PhiLayoutMode and its wire names live in phi_shell_layout.h (the pure
// layout needs them on the host); every includer of this header sees them.

// A stored row (pinned tab or bookmark) as the chrome consumes it — the
// favicon already decoded to raw premultiplied BGRA.
struct PhiStoredRowView {
  std::string guid;
  std::string title;  // UTF-8.
  std::string url;
  bool folder = false;
  std::vector<unsigned char> favicon_bgra;
  int favicon_width = 0;
  int favicon_height = 0;
  // Split cell (pin or bookmark): ONE row, the second pane riding here.
  std::string url_b;
  std::string title_b;
  std::vector<unsigned char> favicon_b_bgra;
  int favicon_b_width = 0;
  int favicon_b_height = 0;
  // Folders only: the catalog symbol id (the Mac's folderIconName).
  std::string icon;
  // Split rows only: "vertical" (side-by-side) or "horizontal" (stacked).
  std::string layout;
};

// A Space as the chips row and menus consume it.
struct PhiSpaceView {
  std::string id;
  std::string name;       // UTF-8.
  std::string profile_id;
  std::string icon;       // Storage form ("emoji:<id>" / "phi:<id>").
  std::string icon_text;  // Resolved display glyph, UTF-8 (may be PUA).
  // A runtime Incognito Space (docs/incognito-space-port.md).
  bool incognito = false;
};

// What the create-Space form commits — CreateSpacePanel.create's arguments.
struct PhiNewSpaceSpec {
  std::string name;        // Already defaulted ("Space N") by the form.
  std::string profile_id;
  std::string icon;        // Storage form ("phi:<id>" / "emoji:<id>").
  std::string theme_id;    // A built-in theme, pinned to the new Space.
  int saturation = -1;     // Legacy slider value; -1 = follow the app.
  // The form's draft palette (its "Color ›" editor), committed onto the new
  // Space after creation — the Mac's selectedPalette.
  std::optional<PhiThemePalette> palette;
  std::string color_hex;   // The theme overlay, "#RRGGBB" (the Mac's colorHex).
  // The Space the form opened from (the Mac's afterSpaceId): the new Space
  // lands right after it in the order; empty appends.
  std::string after_space_id;
};

// A live agent Space as a pip in the switcher — the Mac's robot Space pip.
// The user clicks it to watch the (normally hidden) agent Space live.
struct PhiAgentPipView {
  std::string task_id;   // The skill's taskId; also the pip's identity.
  std::string space_id;  // "agentspace-N": what a slot hosts it under.
  std::string caption;   // The agent's current status line (may be empty).
  std::string agent_name;  // "Claude Code", "Codex", … or empty.
  std::string profile_id;  // The profile it runs in (the hover card's).
  // The pip's icon: the driving agent's brand mark (phi_agent_icons.h)
  // when the driver named itself as a known agent, else the robot — the
  // Mac's pipIcon override, render-time only.
  std::wstring icon;
  int number = 0;        // The Mac's task ordinal ("R1").
  // What the corner badge shows (the Mac's agentBadge, in precedence):
  // error (failed, or an error the user has not seen yet), completed,
  // the user holding control, idle, else running.
  enum class Badge { kRunning, kIdle, kUserHolds, kCompleted, kError };
  Badge badge = Badge::kRunning;
  bool watched = false;  // Its window is currently surfaced on-screen.
};

// The pip's name wherever a switcher lists it — the strip, the Spaces
// menus, its hover card: the Mac's "R<number>", and the driving agent.
inline std::wstring PhiAgentPipLabel(const PhiAgentPipView& pip) {
  return L"R" + std::to_wstring(pip.number) + L" · " +
         (pip.agent_name.empty() ? L"Agent"
                                 : base::UTF8ToWide(pip.agent_name));
}

// What a slot needs from its owner (the coordinator), as five narrow
// interfaces (docs/architecture-review.md §3.5): the full Space list to draw,
// and requests that only the owner can service (switching to a Space that
// has no browser yet must create one in that Space's profile). The shell
// takes the five as a ShellServices at construction and hands each
// component only the ones it calls — there is no union to reach a service
// through any more. ChromePrefsHost is PhiPreferences already; the
// coordinator implements the other four by forwarding to the service that
// owns each.
// Every `int64_t slot` parameter carries SlotId::value (the controller's
// slot()) and becomes SlotId in phase 2; a `browser` parameter carries
// BrowserId::value.
class PhiRevealVeil;
class PhiTabPreview;
class PhiBookmarkManager;
class PhiNewTabPage;
class PhiCreateSpacePanel;

// Stored items, rows, surfaces, Reader View and THE sidebar mutator.
class SidebarHost {
 public:
  virtual ~SidebarHost() = default;
  // Reader View: upstream says this tab has an article. The shell shows its
  // reader button only for such tabs.
  virtual void NoteTabDistillable(int64_t window_id,
                                  int tab_id,
                                  bool distillable) = 0;
  // Reader View proper is the Mirage extension's — it probes every page,
  // extracts in-page and renders on its own surface, and the client relays
  // intent. True when the extension's probe (or a site rule) says this tab's
  // CURRENT document has an article; the shell ORs it with upstream's
  // distillability verdict, which is the Mac's rule for offering the button.
  virtual bool IsReaderOfferable(int tab_id, const std::string& url) const = 0;
  // True while the extension's reading surface stands for this tab — either
  // the tab IS that surface, or the surface is open on its behalf.
  virtual bool IsReaderActive(int tab_id, const std::string& url) const = 0;
  // Enters Reader View for the tab, or leaves it when the tab already shows
  // the reading surface. `url` is what tells those two apart. `entry_point`
  // is which control asked, for the usage metric (PhiReaderBridge's
  // vocabulary: "address_bar", "view_menu", "shortcut").
  virtual void RequestToggleReaderView(int tab_id,
                                       const std::string& url,
                                       const char* entry_point) = 0;
  // Folio (docs/folio-port.md): whether Save to Folio is offered for the
  // tab, the save itself (Mirage runs it), and Open Folio.
  virtual bool CanSaveToFolio(int tab_id) const = 0;
  virtual void RequestSaveToFolio(int tab_id) = 0;
  virtual void RequestOpenFolio() = 0;
  // The page underneath the tab's attached surfaces (Reader View, Peek) is
  // about to change or move — an address-bar navigation or Back/Forward on
  // the origin — so every surface attached to it closes first. A surface
  // cannot be left over a page that changed beneath it.
  virtual void RequestCloseSurfaces(int tab_id) = 0;
  // The same for a peek that covers `tab_id` without being its own: one
  // bound to its split partner in `browser`, which covers the whole split
  // (the Mac's visiblePeek). The partner's reader, over the other pane
  // only, stays. False when there is no such peek.
  virtual bool RequestClosePartnerPeek(int64_t browser, int tab_id) = 0;
  // §5.8: the favicon the row tile shows for the surface attached to
  // `tab_id` (the peeked page's; the origin's own for a reader). False
  // when nothing is attached or the page has none yet.
  virtual bool SurfaceFaviconForOrigin(int tab_id,
                                       std::vector<unsigned char>* bgra,
                                       int* width,
                                       int* height) = 0;
  // The content card moved or resized: re-fit whatever is hosted over it.
  virtual void NoteContentLayoutChanged(int64_t slot) = 0;
  // Raise this slot's surface panels above the browser child, so the corner
  // caps can then be raised above THEM (see RaiseCornerCaps).
  virtual void RaiseReaderPanels(int64_t slot) = 0;
  // The slot's pinned tabs, in sibling order.
  // A stored row (pin or bookmark) was opened or focused from the sidebar —
  // the Mac's updateLastSeen (device-local; not synced).
  virtual void NoteStoredRowSeen(const std::string& guid) = 0;
  virtual std::vector<PhiStoredRowView> PinnedRows(int64_t slot) const = 0;
  // Complete scoped identity inventory, independent of folder expansion,
  // layout, and favicon loading. The sidebar model owns live reconciliation.
  virtual std::vector<SidebarStoredItem> SidebarStoredItems(int64_t slot) const = 0;
  // THE mutator for sidebar state (docs/sidebar-architecture.md §5): every
  // gesture, menu item and shortcut is a SidebarCommand applied against the
  // slot's current tree. False when the command was refused or was a no-op.
  virtual bool Apply(int64_t slot,
                     const SidebarCommand& command,
                     const SidebarTree& tree) = 0;
  // Peek/Reader surfaces attached to tabs of `browser`: origin tab id ->
  // kind. The model puts the indicator on whichever row owns the origin.
  virtual std::map<int, SidebarSurface> AttachedSurfaces(
      int64_t browser) const = 0;
  // The browser of the Peek on screen over `browser`'s active tab — its own,
  // or its split partner's, which covers the whole split (the Mac's
  // visiblePeek) — or an invalid id when none is presented. What page
  // commands and the address field aim at (PhiShellWindow::
  // PageCommandBrowser).
  virtual BrowserId PresentedPeekBrowser(int64_t browser) const = 0;
  // Bookmark rows under `parent_guid` (empty = the root), so the menu bar can
  // descend into folders the way the Mac's Bookmarks menu does.
  virtual std::vector<PhiStoredRowView> BookmarkRows(
      int64_t slot,
      const std::string& parent_guid) const = 0;
  // Opens `url` in the slot (a pinned tab or bookmark was clicked).
  virtual void RequestOpen(int64_t slot, const std::string& url) = 0;
  // Appends one bookmark row under `parent_guid` ("" = root) in `slot`'s
  // Space — Bookmark All Tabs' per-tab fill. Returns the new row's guid.
  virtual std::string AddBookmarkRow(int64_t slot,
                                     const std::string& parent_guid,
                                     const std::string& title,
                                     const std::string& url) = 0;
  // Bookmarks the page `browser` shows — the slot's own browser, or the
  // Peek presented over it (the Mac's pageCommandTarget) — into the slot's
  // Space: the menu bar's Bookmark This Tab / Ctrl+D. `browser` 0 falls
  // back to the slot's shown browser.
  virtual void RequestBookmarkActiveTab(int64_t slot, int64_t browser) = 0;
  // Creates a bookmark folder under `parent_guid` (empty = the root) in the
  // slot's Space — the sidebar's New Folder entries. Returns the new guid
  // (empty on refusal).
  virtual std::string AddBookmarkFolder(int64_t slot,
                                        const std::string& parent_guid,
                                        const std::string& title) = 0;
  // Removes a stored row (a pinned tab or bookmark) by its guid.
  virtual void RemoveStoredRow(const std::string& guid) = 0;
  // Retitles a stored row — the context menus' Rename….
  // Sets a bookmark folder's catalog symbol (the Mac's "Change Icon...").
  virtual bool SetStoredRowIcon(const std::string& guid,
                                const std::string& icon) = 0;
  // Retargets a bookmark (the manager's Address cell edit).
  virtual bool SetStoredRowUrl(const std::string& guid,
                               const std::string& url) = 0;
  // Remembers a split row's divider axis so it reopens stacked or
  // side-by-side (the Mac's persistent split layout).
  virtual bool SetStoredRowLayout(const std::string& guid,
                                  const std::string& layout) = 0;
  virtual bool RenameStoredRow(const std::string& guid,
                               const std::string& name) = 0;
  // --- Space-scoped rows, for the Library's cards (phi_library.cc) --------
  // The sidebar's PinnedRows / BookmarkRows read the SLOT's current Space;
  // the Library shows every Space at once, so these name the Space (the
  // coordinator maps it to its profile). The same favicon upgrade applies.
  virtual std::vector<PhiStoredRowView> PinnedRowsForSpace(
      const std::string& space_id) const = 0;
  virtual std::vector<PhiStoredRowView> BookmarkRowsForSpace(
      const std::string& space_id,
      const std::string& parent_guid) const = 0;
  // Appends a pinned tab / a bookmark (or, `folder`, a folder under
  // `parent_guid`, "" = the root) to `space_id`'s rows — the Library's
  // New Pinned Tab… / New Bookmark… / New Folder…. Returns the new guid.
  virtual std::string AddPinnedToSpace(const std::string& space_id,
                                       const std::string& title,
                                       const std::string& url) = 0;
  virtual std::string AddBookmarkToSpace(const std::string& space_id,
                                         const std::string& parent_guid,
                                         const std::string& title,
                                         const std::string& url,
                                         bool folder) = 0;
  // Re-homes a bookmark under another Space's root (the Mac's
  // moveBookmarks(toSpace:)); `clone` copies instead. Folders and pins
  // are refused.
  virtual bool MoveStoredRowToSpace(const std::string& guid,
                                    const std::string& space_id,
                                    bool clone) = 0;
  // Moves a pinned tab (a split pair travels whole) to the end of another
  // Space's pins — the Mac's transferPinnedTab over LocalStore::TransferPin.
  virtual bool TransferPinToSpace(const std::string& guid,
                                  const std::string& space_id) = 0;
};

// The Space list, switching, creation, per-Space theme and the default role.
class SpacesHost {
 public:
  virtual ~SpacesHost() = default;
  // Every Space, in display order.
  virtual std::vector<PhiSpaceView> Spaces() const = 0;
  virtual void RequestSwitchSpace(int64_t slot, const std::string& space_id) = 0;
  // Creates a Space in this slot's current profile (`new_profile` = false) or
  // in a freshly created profile (true), then switches to it.
  virtual void RequestCreateSpace(int64_t slot, bool new_profile) = 0;
  // Incognito Spaces (docs/incognito-space-port.md): a new one joins this
  // slot's strip and the slot switches to it (or, from a slot with no
  // Spaces — an incognito window — the first user window's); closing one
  // moves every slot showing it to its last regular Space, closes its
  // browsers and drops it. No confirmation here: the shell asks first.
  virtual void RequestNewIncognitoSpace(int64_t slot) = 0;
  virtual void RequestCloseIncognitoSpace(const std::string& space_id) = 0;
  // A Space's browser is live in this slot. The coordinator remembers the
  // (Space, window id) pair so a later run can name that window when it asks
  // the framework to park or rebuild it (lazy Space restore).
  virtual void NoteSpaceWindow(const std::string& space_id,
                               int64_t browser_window_id) = 0;
  // The user is now looking at `space_id` in this slot — every switch, not
  // only the ones the coordinator drove: a chip click on a Space whose
  // browser is already attached never leaves the controller. The
  // coordinator persists it as the Space a cold start should reopen
  // (docs/architecture-review.md §4 item 21). Agent shells never call it.
  virtual void NoteSlotActiveSpace(const std::string& space_id) = 0;
  // Moves a Space to `new_index` in the Space list (chip drag).
  virtual void ReorderSpace(const std::string& space_id, int new_index) = 0;
  // The theme THIS SLOT paints with: a Space may pin its own (the Mac's
  // per-Space theme override), otherwise the app-wide one.
  virtual const PhiPalette& PaletteForSlot(int64_t slot) const = 0;
  // The same for a Space by id — the split-view chrome is pushed per hosted
  // browser, each on its own Space's palette, not the slot's current one.
  virtual const PhiPalette& PaletteForSpace(
      const std::string& space_id) const = 0;
  // A Space's pinned theme id (empty = follows the app-wide one), and setting
  // it — the sidebar's Space menu writes through here.
  virtual std::string SpaceThemeId(const std::string& space_id) const = 0;
  virtual void SetSpaceTheme(const std::string& space_id,
                             const std::string& theme_id) = 0;
  // A Space's own overlay saturation, 0..100 (-1 = follow the app-wide
  // slider), and setting it — the Set Theme panel's second row. The setter
  // is the Settings host's spelling, so one coordinator override serves
  // both.
  virtual int SpaceSaturation(const std::string& space_id) const = 0;
  virtual void SetSpaceSaturation(const std::string& space_id, int value) = 0;
  // A Space's linked palette (the Mac's spaceThemePalettes record; nullopt
  // when it paints with its pin) and the theme editor's two writes: Set
  // persists once (a release, a toolbar click), Preview repaints the
  // Space's windows per pointer move without writing.
  virtual std::optional<PhiThemePalette> SpacePalette(
      const std::string& space_id) const = 0;
  virtual void SetSpacePalette(const std::string& space_id,
                               const PhiThemePalette& palette) = 0;
  virtual void PreviewSpacePalette(const std::string& space_id,
                                   const PhiThemePalette& palette) = 0;
  // Space management (the menu bar and the chip context menu). Only the
  // last Space is undeletable (the default hands its role on); deletion
  // takes the Space's bookmarks and space-scoped pins with it, as on Mac.
  virtual bool RenameSpace(const std::string& space_id,
                           const std::string& name) = 0;
  virtual bool DeleteSpace(const std::string& space_id) = 0;
  // The Space holding the default role — what a fresh shell binds its first
  // browser to (see the coordinator's DefaultSpaceId).
  virtual std::string DefaultSpaceId() const = 0;
  // Whether the Space menu may offer Delete (never for the last Space).
  virtual bool CanDeleteSpace(const std::string& space_id) const = 0;
  // Re-binds a Space to another profile — the Space menu's "Change Profile",
  // the Mac's SpaceManager.changeProfile. A window's profile is baked in at
  // spawn, so the Space's browser is closed and respawned with its tabs
  // reopened there; the default Space keeps its binding. False when nothing
  // changed.
  virtual bool SetSpaceProfile(const std::string& space_id,
                               const std::string& profile_id) = 0;
  // Profile management. Deleting Default, or a profile still bound to a
  // Space, is refused (re-bind or delete those Spaces first).
  virtual bool DeleteProfileById(const std::string& profile_id) = 0;
  // Opens Settings > Spaces with this Space selected (the remote surface's
  // "Change Icon…" destination; the shell's own path is the §5.9 flyout
  // through SetSpaceIcon).
  virtual void RequestSpaceIconPicker(const std::string& space_id) = 0;
  // The Space's icon, as the picker stores it ("phi:<id>" / "emoji:<id>").
  virtual void SetSpaceIcon(const std::string& space_id,
                            const std::string& icon) = 0;
  // The create-Space form (PhiCreateSpacePanel). ProfileSummaries is the
  // Settings host's spelling, so one coordinator override serves both.
  virtual std::vector<std::string> ProfileSummaries() const = 0;  // "id\tname"
  // Creates a profile for the form; the slot's controller hears
  // OnSpaceFormProfileCreated once it exists (creation is asynchronous).
  virtual void RequestCreateProfileForSpaceForm(int64_t slot,
                                                const std::string& name) = 0;
  // Creates the Space the form described, pins its theme, and switches
  // `slot` to it. Returns the new Space's id, or empty on failure.
  // `surface` is where the form was hosted, for the analytics (the Mac's
  // CreateSpacePanel.Style.analyticsSurface): "sidebar" for the strip's
  // and the pip flyout's form, "standalone_window" for the create window.
  virtual std::string RequestCreateSpaceWithOptions(
      int64_t slot,
      const PhiNewSpaceSpec& spec,
      const char* surface) = 0;
  // The same creation WITHOUT the switch: the new Space lands after
  // `spec.after_space_id` and stays in the background — the Library's
  // inline create card (the Mac's CreateSpacePanel(style: .library)).
  // `surface` is "library".
  virtual std::string RequestCreateSpaceInBackground(
      int64_t slot,
      const PhiNewSpaceSpec& spec,
      const char* surface) = 0;
};

// The client-wide chrome preferences: layout, palette, previews, the
// bookmark-bar flags, the sidebar width. PhiPreferences implements it.
class ChromePrefsHost {
 public:
  virtual ~ChromePrefsHost() = default;
  // The layout every slot renders in — one setting for the whole client, as on
  // Mac, so a new window never disagrees with the one beside it.
  virtual PhiLayoutMode LayoutMode() const = 0;
  virtual void RequestLayoutMode(PhiLayoutMode mode) = 0;
  // The Mac's GeneralSettings.showTabPreviews: whether a settled hover on a
  // tab, pin or bookmark row raises its page-snapshot card.
  virtual bool ShowTabPreviews() const = 0;
  // The Mac's GeneralSettings.showOpenTabIndicators: a dot on an inactive
  // pinned tab or bookmark whose tab is open. Off by default.
  virtual bool ShowOpenTabIndicators() const = 0;
  // The Mac's GeneralSettings.dimUnloadedTabIcons: a reclaimed tab's favicon
  // and open dot at 30%. On by default; the dimmed title stays either way.
  virtual bool DimUnloadedTabIcons() const = 0;
  // Appearance ▸ "Sidebar transparency", 0-100 %: how much of the Desktop
  // Acrylic backdrop shows through the theme tint; 0 paints it opaque.
  virtual int SidebarTransparency() const = 0;
  // Developer ▸ "Allow agents to control Phi": gates the View menu's agent
  // items, as the Mac's cdpAgentAccessEnabled does.
  virtual bool AllowAgentControl() const = 0;
  // View ▸ Agent Autoview (the Mac's autoViewEnabled).
  virtual bool AgentAutoView() const = 0;
  virtual void RequestAgentAutoView(bool enabled) = 0;
  // "New tab behavior = Omnibox": a new tab focuses the address field
  // instead of opening a page (the tab is born on commit).
  virtual bool NewTabOpensOmnibox() const = 0;
  // The sidebar width in DIP — one app-wide value like the layout mode,
  // clamped to the Mac split range (193–500).
  virtual int SidebarWidth() const = 0;
  virtual void RequestSidebarWidth(int dip) = 0;
  // The app-wide resolved theme.
  virtual const PhiPalette& Palette() const = 0;
  // The bookmark bar's two visibility switches (the Mac View menu's
  // "Always Show Bookmark Bar" / "Show Bookmark Bar on New Tab").
  virtual bool AlwaysShowBookmarkBar() const = 0;
  virtual void SetAlwaysShowBookmarkBar(bool enabled) = 0;
  virtual bool BookmarkBarOnNewTab() const = 0;
  virtual void SetBookmarkBarOnNewTab(bool enabled) = 0;
  // The address bar site menu's "Always Show URL Path" (Settings' "Always
  // show the full URL"; synced with the Mac's alwaysShowURLPath).
  virtual bool AlwaysShowFullUrl() const = 0;
  virtual void SetAlwaysShowFullUrl(bool enabled) = 0;
  // Closing an Incognito Space asks first unless the user ticked "Do not
  // ask again" (the Mac's suppressCloseIncognitoSpaceWarning, synced).
  virtual bool SuppressCloseIncognitoSpaceWarning() const = 0;
  virtual void SetSuppressCloseIncognitoSpaceWarning(bool suppress) = 0;
  // The create-Space form's theme defaults. The three are the Settings
  // host's spellings, so one coordinator override serves both.
  virtual std::string ThemeId() const = 0;  // The app-wide theme.
  virtual PhiAppearance Appearance() const = 0;
  // The theme editor's Light / Dark buttons write the APP-WIDE appearance
  // (the Mac's ThemeManager.setUserAppearanceChoice), as Settings does.
  virtual void RequestAppearance(PhiAppearance appearance) = 0;
  virtual int Saturation() const = 0;  // 0..100.
  // The ⋯ menu's chord column for a menu command: the EFFECTIVE chord of a
  // rebindable one (PhiPreferences' table, with the user's overrides),
  // formatted as the catalogue spells chords; an empty string when the user
  // unbound it (no chord shown); nullopt when the command is not rebindable
  // at all, so the catalogue string's own "\tChord" stands.
  virtual std::optional<std::wstring> RebindableChordLabel(
      UINT command) const = 0;
  // The command a rebindable shortcut binds to this chord, or 0 (what the
  // keyboard hook asks; the floating omnibox asks it for New / Close Tab).
  virtual UINT MatchRebindableChord(UINT vk,
                                    bool ctrl,
                                    bool shift,
                                    bool alt) const = 0;
};

// Agent Spaces as pips, watching, and the operating mask over the user's
// own tabs.
class AgentHost {
 public:
  virtual ~AgentHost() = default;
  // Live agent Spaces to show as pips in `slot`'s switchers (empty when
  // none): every live agent Space minus those another user slot has on
  // screen — an agent Space is offered only by the window hosting it
  // (PhiAgentSpaceManager::Presents); 0 lists them all, for an agent
  // window's own strip. The user watches one by clicking its pip: the
  // agent's hidden window takes the frame of the window the pip was
  // clicked in (`watcher_slot`) and that window is concealed until the
  // watch ends — the Mac's slot.activate, ONE window on screen, not the
  // agent's beside the user's. Clicking the watched pip again ends the
  // watch; a pip for a Space another window hosts surfaces that window.
  virtual std::vector<PhiAgentPipView> AgentSpaces(int64_t slot) const = 0;
  virtual void RequestWatchAgentSpace(const std::string& task_id,
                                      int64_t watcher_slot) = 0;
  // A user-Space pip clicked while the agent Space is up: the watch ends
  // and the window shows `space_id` (empty: the Space it came from) — in
  // the slot hosting the agent Space this IS the normal Space swap back.
  virtual void RequestLeaveWatchedAgentSpace(const std::string& space_id) = 0;
  // The slot hosting an agent Space for a watch has let its browser go
  // (the swap back landed, or the window is closing): `child` goes back
  // to its own hidden agent shell.
  virtual void OnAgentBrowserReturned(const std::string& task_id,
                                      HWND child) = 0;
  // Watch-mode control pill (C3): ownership transfers ONLY through these —
  // the fog barrier is what makes that true. Finish ends the task.
  virtual void RequestAgentTakeControl(const std::string& task_id) = 0;
  virtual void RequestAgentHandBack(const std::string& task_id) = 0;
  virtual void RequestAgentFinish(const std::string& task_id) = 0;
  // Opens the task's transcript console (the agent pip context menu's
  // "Show Transcript", the Mac showTranscriptAction).
  virtual void RequestShowTranscript(const std::string& task_id) = 0;
  // --- Agent operating mask (the user's own Spaces) -----------------------
  // True while an agent holds `tab_id`; the row wears the mask badge.
  virtual bool IsTabAgentDriven(int tab_id) const = 0;
  // True once the user took `tab_id` back — the badge reads as "you have
  // control" and the browser refuses that tab's drive commands.
  virtual bool IsTabReclaimed(int tab_id) const = 0;
  // "Take control" / "Hand back" on a driven tab.
  virtual void RequestReclaimTab(int tab_id) = 0;
  virtual void RequestReleaseTab(int tab_id) = 0;
};

// Windows as a set: new windows, cross-window drops, tear-off placement,
// the feedback form and the settings window.
class WindowHost {
 public:
  virtual ~WindowHost() = default;
  // File > New Window: a user window in the profile the slot is showing.
  // Goes through the coordinator so the request is token-matched like every
  // other creation (contract rule 3) instead of the legacy NewWindow.
  virtual void RequestNewWindow(int64_t slot) = 0;
  // Surfaces the shell hosting `browser_window_id` (foregrounding it and
  // switching it to that browser's Space when it is hidden) — the search
  // palette's cross-window jump.
  virtual void FocusWindowForBrowser(int64_t browser_window_id) = 0;
  // The process gained or lost the foreground (WM_ACTIVATEAPP, every shell
  // reports it) — the accelerators' Alt state, no analytics (the Mac stopped
  // counting Cmd-Tab, e748d9f4). NoteUserSpaceSwitched is an observation
  // the analytics count (phi_client_analytics.h).
  virtual void NoteAppActivation(bool active) = 0;
  virtual void NoteUserSpaceSwitched() = 0;
  // "Import from another browser…": opens (or fronts) the import window
  // targeting the focused window's Space (phi_import_window.h).
  virtual void RequestBrowserImport() = 0;
  // The client's one analytics capture path (phi_client_analytics.h):
  // the harness ring, then the collector's consent gate and allow-list.
  virtual void CaptureAnalytics(const char* module,
                                const char* event_name,
                                base::DictValue properties) = 0;
  // Opens the settings window (theme, appearance, layout).
  virtual void RequestSettings() = 0;
  // --- The account (the sidebar's profile button) --------------------------
  // The signed-in account's name / email (either may be null); false signed
  // out — the Mac's isAuthenticated, which gates the menu's Sign in row.
  virtual bool AccountInfo(std::string* name, std::string* email) const = 0;
  // Guest Mode (the Mac's isGuest): the button shows no avatar there even
  // while a session is being signed in over it.
  virtual bool AccountIsGuest() const = 0;
  // The account picture as a `px` circle (premultiplied BGRA for
  // PhiBlendBgra), empty until one is fetched. The buffer is the account
  // service's cache for that size — use it before the next call.
  virtual const std::vector<uint8_t>& AccountAvatarCircle(int px) = 0;
  // Starts the hosted login (the profile menu's Sign in).
  virtual void RequestSignIn() = 0;
  // Opens the universal URL Rules editor (Spaces ▸ URL Rules…), centred
  // over `anchor`.
  virtual void RequestUrlRulesEditor(HWND anchor) = 0;
  // Opens the feedback form for `slot`, about `context` (its focused
  // tab) — Help ▸ Report an Issue, the sidebar's Feedback capsule, and
  // Chromium's own feedback command all land here.
  virtual void RequestFeedback(int64_t slot,
                               const PhiFeedbackContext& context) = 0;
  // Cross-window drag (the Mac's window-to-window tab drop): the slot whose
  // shell is under `screen` (topmost wins; 0 = none but `exclude_slot`), and
  // the move itself — the dragged tab (plus its split partner) joins the
  // target window's strip. False leaves the drop to the tear-off path.
  virtual int64_t SlotAtScreenPoint(POINT screen,
                                    int64_t exclude_slot) const = 0;
  virtual bool RequestDropTabOnWindow(int64_t source_slot,
                                      int tab_a,
                                      int tab_b,
                                      int64_t target_slot) = 0;
  // Tear-off placement (the Mac's TabDraggingSession
  // resolvedTearOffWindowFrame): the screen point a tab was dropped at
  // outside every window, noted by the dragging controller just before it
  // asks the framework for the new window, and taken by that window's
  // constructor so it opens AROUND the drop instead of at the saved
  // placement. Stale after a few seconds, in case no window ever came.
  virtual void NoteTearOffDrop(POINT screen) = 0;
  virtual bool TakeTearOffDrop(POINT* screen) = 0;
};

// The five, as the shell receives them at construction. Non-owning; every
// pointer outlives the shell.
struct ShellServices {
  RAW_PTR_EXCLUSION SidebarHost* sidebar = nullptr;
  RAW_PTR_EXCLUSION SpacesHost* spaces = nullptr;
  RAW_PTR_EXCLUSION ChromePrefsHost* prefs = nullptr;
  RAW_PTR_EXCLUSION AgentHost* agent = nullptr;
  RAW_PTR_EXCLUSION WindowHost* window = nullptr;
};

// The Mac sidebar's default and drag range (MainSplitViewController).
inline constexpr int kSidebarWidthDefault = 240;
inline constexpr int kSidebarWidthMin = 193;
inline constexpr int kSidebarWidthMax = 500;

class PhiAppMenu;

// Owns the client-drawn shell window for one browser (the Windows twin of the
// Mac MainBrowserWindowController), using the Option-B model
// (windows-framework-design.md §2.4): the CLIENT owns a top-level window and
// its own painting, and Chromium's browser window is reparented in as a
// WS_CHILD filling the content rect. The client's sidebar is drawn into the
// client's own window (no DirectComposition conflict, unlike the falsified
// overlay approach), while web content renders via DComp inside the reparented
// child's own HWND — two non-overlapping regions, both visible.
// Shell-window app messages posted by the coordinator's keyboard hook —
// the Ctrl+Tab MRU switcher gestures (open/advance on the shell's thread).
enum : UINT {
  kMsgPhiSwitcherNext = WM_APP + 20,
  kMsgPhiSwitcherPrev,
  kMsgPhiSwitcherCommit,
  // Posted when a rebuild notices the active tab crossing into or out of a
  // NATIVE-PAGE route (the bookmark manager, the new-tab page), so the
  // mount happens on its own turn rather than inside the rebuild.
  kMsgPhiSyncNativePages,
  // The coalesced row rebuild (ScheduleRefresh): WM_PAINT and input never
  // rebuild, so the rows rebuild from the window model on this posted turn,
  // the bands whose rows changed are invalidated, and the paint recomposes.
  kMsgPhiRebuildRows,
  // The comfortable Space slide was begun and the switch under it has run:
  // fetch the entering page and release the slide (StartSlideCapture).
  kMsgPhiSlideCapture,
  // A close that arrived while a nested message loop of this window was on
  // the stack (PhiModalDepthFor > 0): the handler retries on this posted
  // turn, and keeps retrying by timer until the loop has unwound. See
  // RequestClose.
  kMsgPhiDeferredClose,
  // F6 / Shift+F6 from the keyboard hook, and the sidebar's Esc: moves the
  // focus round address field -> sidebar -> page. WPARAM is the direction
  // (kPhiFocusNext / kPhiFocusPrevious) or kPhiFocusPage.
  kMsgPhiCycleFocus,
};
enum : WPARAM { kPhiFocusNext = 0, kPhiFocusPrevious = 1, kPhiFocusPage = 2 };

// The shell window (docs/architecture-review.md §3.5): a window procedure,
// the layout cache, the DPI and fonts, focus routing, and dispatch to the
// components that own the rest. It keeps the name PhiShellWindow while
// the components are extracted one at a time — the rename to PhiShellWindow
// would touch every other file and lands with the last of them.
//
// Each component's needs reach it through a narrow interface implemented
// PRIVATELY here, so none of them widen the shell's public surface.
class PhiShellWindow : public PhiHitSource,
                            private PhiOmniboxShell,
                            private PhiBrowserHostShell,
                            private PhiCaptionShell,
                            private PhiAppMenuShell,
                            private PhiNativePagesShell,
                            private PhiSidebarShell,
                            private PhiSpacesStripShell,
                            private PhiContentSlideShell {
 public:
  // `agent` hosts the window as a HIDDEN agent Space: the shell is created
  // off-screen and off the taskbar (WS_EX_TOOLWINDOW), shown without
  // activation so its content still composites and renders for CDP screen
  // capture, but the user never sees it. Everything else is identical, so a
  // future "watch" gesture can move it on-screen.
  // `incognito` hosts an off-the-record browser: fixed-dark chrome, no
  // Spaces strip / pinned grid / bookmarks (nothing persists), and the
  // window stays OUT of the Space registry so URL routing never lands here.
  // `slot` is this shell's identity; `browser` is the first browser it
  // adopts (the one it was opened with, which backs the default Space).
  // Today the two carry the same number — the types are what keep the slot
  // from ever being handed to the framework as a browser (phi_ids.h).
  // `model` is the coordinator's window model (docs/architecture-review.md
  // §3.3): every row, nav state and split this shell shows is read from
  // it, never parsed from the framework's JSON. Owned by the coordinator,
  // which outlives every controller.
  // `first_space` names the Space `browser` backs when it is not the
  // default one — an Incognito Space opened with no user window up, which
  // gets a shell of its own (the Mac mints a slot for it).
  PhiShellWindow(SlotId slot,
                      BrowserId browser,
                      HWND browser_hwnd,
                      phi_framework::PhiFramework* framework,
                      const ShellServices& services,
                      const PhiWindowModel* model,
                      bool agent = false,
                      bool incognito = false,
                      bool start_hidden = false,
                      const std::string& first_space = std::string());
  // A shell created behind the onboarding gate (start_hidden) comes on
  // screen here once access is granted; a no-op for a visible shell.
  void ShowHeldShell();
  bool PresentBrowser(BrowserId browser, bool activate);
  // The shell's first show places it directly below `above` (null: just
  // not on top) without activating it — a window opened for a background
  // spaces.openTab must not move the user's focus. Chromium's own Show()
  // asks to activate; this answers it once, inactive.
  void PresentBehindOnFirstShow(HWND above);
  void SetShellFullscreen(BrowserId browser,
                          bool fullscreen,
                          const RECT& target);
  // The reverse (a sign-out re-arms the gate): off screen, browser alive.
  void HideShell();
  bool shell_hidden() const { return shell_hidden_; }
  // Virtual because the shell now implements its components' interfaces:
  // the class has virtual functions, and PhiWindowRegistry deletes it
  // through a PhiShellWindow* (its test stand-ins subclass it).
  ~PhiShellWindow() override;

  PhiShellWindow(const PhiShellWindow&) = delete;
  PhiShellWindow& operator=(const PhiShellWindow&) = delete;

  // The slot's identity — what every host interface's `slot` takes.
  // Never a browser: see active_browser_id() for the one the slot shows.
  SlotId slot() const { return slot_; }
  // Upstream's distillability verdict for one of this window's tabs.
  void NoteTabDistillable(TabId tab_id, bool distillable);
  // True when `tab_id` is in the strip this window is currently drawing.
  bool ShowsTab(TabId tab_id) const;
  // Which corners of the content card are rounded right now. The reader panel
  // covers that card and has to wear the same shape: the TOP corners belong
  // to the bar band in the layouts that have one (only performance rounds
  // them), and content fullscreen rounds nothing — the same rule the corner
  // caps are shown by.
  void CardRoundedCorners(bool* top, bool* bottom) const;
  // Puts the rounded-card corner caps back on top. They are sibling windows
  // ABOVE the page (DirectComposition ignores a region on the browser child),
  // so anything else raised over the content card — the reader panel — buries
  // them and squares the card's corners until this runs.
  void RaiseCornerCaps();
  // Reader View for the ACTIVE tab: enter, or leave when the tab already
  // shows the extension's reading surface. The reader button, the View menu
  // item, its shortcut and the page context menu all land here.
  // `entry_point` names the control for the reader's usage metric.
  void ToggleReaderView(const char* entry_point);
  // Back/Forward or an address-bar commit on a tab the reader is covering:
  // leaves the reader and returns true, so the caller skips the navigation
  // (Back) or lets it land on the page underneath (the address bar). The
  // same for the peek on screen — the tab's own, or its split partner's,
  // which covers the whole split (the Mac's closePeekForBackForward-
  // Navigation). False when no such surface is up and the command is an
  // ordinary one.
  bool LeaveReaderViewForNavigation();
  // What the tab a Peek was opened from is bound to, for the peek's usage
  // metric — the Mac's peekOpenerKind: "pinned", "bookmark" or "normal"
  // (a tab in the strip, or one this window's tree does not know).
  const char* PeekOpenerKind(int tab_id) const;
  // Whether the active tab is in Reader View right now (the item's tick, and
  // what makes the toggle mean "leave").
  bool ActiveTabShowsReader() const;
  // Whether the View menu's Reader View item is reachable. DELIBERATELY not
  // the button's offerability test: the menu and its shortcut offer the
  // reader on any real page, so a page the probe misjudges still opens
  // through them (the Mac's rule). Only the native new-tab page is refused —
  // it has no web contents to extract from.
  bool ActiveTabCanUseReader() const;
  // The URL gate both of the above sit behind — the Mac's
  // ReaderExtractionService.canOfferReader.
  static bool CanOfferReaderForUrl(const std::string& url);
  bool is_agent() const { return agent_; }
  // An incognito WINDOW: no Spaces at all (window-level).
  bool is_incognito() const { return incognito_; }
  // The whole window is closing (RequestClose ran): its browsers go one by
  // one, and the Spaces it falls back to meanwhile are no user choice.
  bool closing() const { return closing_; }
  // Ends the Incognito Space `space_id` — after the Mac's confirmation
  // unless "Do not ask again" was ticked. Cancel leaves everything open.
  // By value: callers pass active_space(), which the alert's loop and the
  // close's own switch-away both rewrite.
  void CloseIncognitoSpaceAsking(std::string space_id);
  // The last tab of the Incognito Space on screen is closing (Ctrl+W, its
  // row's close box): the Space ends instead, asked first. True when that
  // took the close, whatever the answer; false for any other tab.
  bool CloseTabEndsIncognitoSpace();
  // Nothing persists from what the window shows: an incognito window, or
  // an Incognito Space on screen (content-level — the look, the omnibox
  // glyph, the new-tab page, the greyed bookmark and pin rows).
  bool ShowsIncognito() const;
  // For satellite surfaces (search palette, popovers) that act through the
  // same seams the controller does — each takes the ONE interface its own
  // job needs, never the set (docs/architecture-review.md §3.5).
  SidebarHost* sidebar_host() const { return services_.sidebar; }
  SpacesHost* spaces_host() const { return services_.spaces; }
  ChromePrefsHost* prefs_host() const { return services_.prefs; }
  AgentHost* agent_host() const { return services_.agent; }
  WindowHost* window_host() const { return services_.window; }
  phi_framework::PhiFramework* framework() const { return framework_; }
  // The window model the rows are built from — what a satellite reads
  // instead of parsing the framework's tab JSON.
  const PhiWindowModel* model() const { return model_; }
  // The feedback form about this window's focused tab (the Mac's
  // showFeedbackWindow).
  void OpenFeedback();
  PhiPalette CurrentPalette() const;  // Ramped while a switch is in flight.
  // The page colour the address row takes (the Mac's WebContentHeader page
  // colour): the active tab's reported colour, or nullopt where the Mac keeps
  // the native chrome -- no page (the new tab / bookmark page, placeholder),
  // a split, the group overview. BarPalette is the palette painted with it.
  std::optional<PhiColor> ActivePageColor() const;
  PhiPalette BarPalette() const;
  // Tab ids most-recently-used first — the Ctrl+Tab switcher's order.
  const std::vector<TabId>& TabMruOrder() const;
  // The content card's client-coordinate rect (the group overview covers it).
  RECT ContentRect();
  // Whether `tab_id` is a pane of the split on screen; `*pane` is then its
  // client-coordinate rect, empty until Chromium has laid the panes out
  // (OnContentBoundsChanged follows). What a reader over one pane covers.
  bool SplitPaneRectForTab(int tab_id, RECT* pane) const;
  // The create-Space form — the Mac's CreateSpacePanel hosted in the
  // sidebar (SidebarViewController.showCreateSpaceOverlay). Every "New
  // Space" entry point (the strip's "+", the switcher menus, the menu bar)
  // routes through ShowCreateSpaceForm; where a docked vertical sidebar
  // cannot host the form — comfortable, a collapsed sidebar — it opens in
  // the standalone window instead (phi_create_space_window.h, the Mac's
  // CreateSpacePanel.present).
  void ShowCreateSpaceForm();
  void CommitCreateSpaceForm(const PhiNewSpaceSpec& spec);
  void CancelCreateSpaceForm();
  // The form's live theme preview: the chrome paints with this palette
  // until the created Space presents, or the form is cancelled.
  void PreviewCreateSpaceTheme(const std::string& theme_id, int saturation);
  // The same for the form's draft palette over its base theme.
  void PreviewCreateSpacePalette(const std::string& theme_id,
                                 const PhiThemePalette& palette);
  // The form's icon, mirrored on the strip's dashed placeholder pip.
  void SetCreateSpaceIcon(const std::string& storage_value);
  void OnSpaceFormProfileCreated(const std::string& profile_id);
  bool creating_space() const { return spaces_->creating(); }
  // Whether the address EDIT — a real child window — must stay hidden:
  // fullscreen content, a collapsed performance sidebar, or the create-Space
  // form covering the sidebar's address row.
  bool OmniboxFieldHidden() const;
  // Focuses the address field, raising the floating panel first when the
  // collapsed performance shell has no field of its own.
  void FocusAddressField();
  // Surfaces (visible=true) or parks off-screen (visible=false) an agent
  // window so the user can watch it live. No-op on a non-agent window.
  // `frame` (screen px) is the watching window's frame, which the agent
  // window takes over exactly, and `owner` that window: the agent window
  // becomes its OWNED window for the watch — kept above it by the system,
  // minimized with it, no taskbar button of its own — so the user window
  // never has to be hidden and nothing blinks when the watch ends. Null
  // frame centres it on the work area, unowned.
  void SetAgentVisible(bool visible,
                       const RECT* frame = nullptr,
                       HWND owner = nullptr);
  // The USER window's side of a watch. It stays shown and painted under
  // the agent window (hiding it made the desktop flash through when the
  // watch ended, then a cold repaint); `concealed=false` only takes the
  // focus back. No-op on an agent window.
  void SetConcealedForWatch(bool concealed);
  // Brings the shell window to the front (restored if minimised): an agent
  // Space asked for from another window is surfaced where it already is.
  void BringToFront();
  // Whether the agent holds this (agent) window: the owner tells it on
  // every ownership change. While it does, the window refuses what the
  // Mac's refuses under agent ownership — New Tab, closing tabs, the tab
  // menus, Back/Forward — so a stray keystroke never moves the agent's
  // page; the pill's Take control is the one way in.
  void SetAgentOwned(bool agent_owned);
  // The agent holds the page on screen: this is an agent window, or a user
  // window whose ACTIVE Space is the agent Space it hosts for a watch.
  bool AgentHoldsInput() const;

  // --- An agent Space hosted in a USER slot for a watch ---------------------
  // The Mac's slot.activate for an agent Space: its browser is attached to
  // THIS slot under `space_id` and shown with the slot's own Space swap
  // (band slide, pill slide, content reveal), exactly as any Space. The
  // agent shell keeps the browser's record and gets the child back when
  // the watch ends (LeaveHostedAgentSpace: the swap back to a user Space,
  // then, once it lands, OnAgentBrowserReturned on the agent host).
  // Called while another agent Space is hosted, it is the switch between
  // the two — the Mac's slot.activate from one agent Space to the next:
  // ONE swap, the hosted page leaving as the new one enters, and the
  // leaving browser handed back when the slide lands. The user Space the
  // first watch left stays the one the watch returns to.
  void HostAgentSpace(const std::string& space_id,
                      BrowserId browser,
                      HWND browser_hwnd,
                      const std::string& task_id);
  bool HostsAgentSpace() const { return !hosted_agent_task_.empty(); }
  const std::string& hosted_agent_task() const { return hosted_agent_task_; }
  // True while `task_id`'s browser sits in this frame: hosted, or on its
  // way home as the leaving page of a running swap (the watch ended, or
  // the next agent Space took over) — until that swap lands, this slot
  // alone presents the Space (PhiAgentSpaceManager::HostingSlotFor).
  bool HoldsAgentTask(const std::string& task_id) const {
    return !task_id.empty() && (hosted_agent_task_ == task_id ||
                                leaving_agent_task_ == task_id);
  }
  // `space_id` empty: the user Space the slot showed before.
  void LeaveHostedAgentSpace(const std::string& space_id);
  // The agent shell's two sides of the hand-over.
  HWND ReleaseAgentBrowserForHosting();
  void ReadoptAgentBrowser(HWND browser_hwnd);

  // Click on a pin or a sidebar bookmark row (cell click, context-menu
  // Open, or the pinned.open verb): focus the bound live tab if it still
  // exists, otherwise open the row's url as a new bound tab — the Mac's
  // BrowserState.openOrFocusPinnedTab / bookmarkGuidBacking model.
  void OpenOrFocusPinned(const std::string& guid, const std::string& url);
  // The Library's open (phi_library.cc): switches this window to
  // `space_id`, then opens or focuses the row there — at once when the
  // Space's browser is attached, else once the switch lands
  // (BrowserHostNoteActiveSpace), the Mac's slot.activate(onSwapSettled:).
  void OpenStoredRowInSpace(const std::string& space_id,
                            const std::string& guid,
                            const std::string& url);
  // The §5.9 icon picker flyout for `space_id`, hung off `anchor_screen`;
  // a pick applies through the spaces host at once. Pumps its own loop.
  void ShowSpaceIconPicker(const std::string& space_id,
                           const RECT& anchor_screen);
  // The ACTIVE tab, as the sidebar's rebuild resolved it (surfaces
  // redirected to the page they cover), and its URL. The omnibox, the
  // reader affordance, the native-page route, the feedback context and the
  // Library's close-on-tab-change all read the shell's spelling of the
  // sidebar's answer.
  TabId active_tab_id() const;
  const std::string& active_tab_url() const;
  // The shell bound to a PhiClientShell HWND (GWLP_USERDATA, as
  // ShellWndProc reads it), or null for any other window — the way a
  // surface that was handed only the owner HWND finds its controller.
  static PhiShellWindow* FromHwnd(HWND hwnd);

  // The pointered link target changed in `browser_id`'s page (empty url =
  // pointer left the link). Shows/hides the status bubble when that browser
  // is the one this shell displays.
  void OnTargetURLChanged(BrowserId browser_id, const std::wstring& url);

  // Autocomplete results for `browser_id`'s running omnibox query — feeds
  // the address field's suggestion dropdown (the Mac OmniBox panel).
  void OnOmniboxResults(BrowserId browser_id, const std::string& json);

  // `browser_id`'s last tab closed but the browser SURVIVES in placeholder
  // mode (entered) or a new tab ended that mode. The content child hides
  // while placeholding — the dead tab's final frame would linger otherwise
  // — and a field commit births the first tab.
  void OnPlaceholderModeChanged(BrowserId browser_id, bool entered);
  // A tab's content (video/element) entered or left fullscreen: the hosted
  // child resizes itself to the MONITOR, so the shell must go borderless
  // fullscreen for the duration or the render is clipped to the window.
  void OnContentFullscreenChanged(BrowserId browser_id, bool fullscreen);
  // Chromium moved a pane of this window's split (the framework's
  // OnContentBoundsChanged): the native pages over the panes follow.
  void OnContentBoundsChanged(BrowserId browser_id);
  // The entering page the comfortable Space slide asked the framework for
  // (StartSlideCapture): true when `request_id` is this window's.
  bool OnTabSurfaceCaptured(int request_id,
                            const unsigned char* bgra,
                            int width,
                            int height);

  // THE refresh entry point (docs/architecture-review.md §3.1 principle 3):
  // marks the rows dirty and posts ONE coalesced rebuild (kMsgPhiRebuildRows)
  // for this turn's burst of changes — the model's push, a pref, a Space
  // switch. The rebuild runs on that posted turn, invalidates the bands whose
  // rows changed, and the paint recomposes. Nothing rebuilds inline.
  void ScheduleRefresh();
  // A verb that READS rows (the agent dialect's window.layout, a capture)
  // right after a change: runs the pending rebuild now, on the caller's
  // turn, so it sees the rows the next paint will. A no-op when nothing is
  // pending.
  void FlushPendingRebuild();
  // Rebuilds the Space chips row from the owner's full Space list.
  void RebuildChips();
  // Re-derives the geometry and repositions the browser child + corner caps
  // (public so the owner can push app-wide layout changes, e.g. the sidebar
  // width, into every window).
  void LayoutContents();
  // PhiUpdater::UpdateAvailable changed: the Update reminder appears or
  // goes, and what it pushes along moves with it.
  void NoteUpdateAvailabilityChanged();
  // The account moved (sign-in, sign-out, a profile or avatar fetch —
  // PhiCoordinator::NoteAccountChanged): the bottom bar's profile button
  // reads it at paint time, so the bar recomposes.
  void InvalidateBottomBar();

  // --- Spaces (client-owned, as on Mac) ------------------------------------
  // A slot shows ONE Space at a time. Each Space has its own Browser; the
  // controller swaps which browser child is visible in the shell window.

  // Adopts an additional browser as `space_id`'s content for this slot and
  // shows it (hiding the previous). Called when the framework reports a
  // browser created for this slot.
  void AttachSpaceBrowser(const std::string& space_id,
                          BrowserId browser,
                          HWND browser_hwnd);
  // Shows the Space's browser, hiding the current one. False if unknown.
  bool ShowSpace(const std::string& space_id);
  bool HasSpace(const std::string& space_id) const;
  // A switcher's pick by Space id — a pip, a Spaces menu row, Ctrl+N,
  // Next/Previous Space — over everything this window presents (the
  // Mac's slot.presentedSpaces): a user Space shows (attached already, or
  // requested cold; from an agent Space this is the end of the watch), an
  // agent Space is watched, exactly as its pip's click. The active Space
  // is a no-op.
  void SwitchToSpace(const std::string& space_id);
  // Recreates the menu bar from the CURRENT language. The bar caches every
  // title and item string as wide text inside its HMENUs, so a language
  // change cannot be repainted — the menus have to be built again, and the
  // caption band's measured title rects re-measured with them.
  // Recreates the app menu from the CURRENT language: the HMENU rows cache
  // every string as wide text, so a language change cannot be repainted.
  void RebuildAppMenu();
  // The Space whose browser is `browser_id` in this slot, or empty — the
  // reverse of BrowserIdForSpace, for a skill payload that names a WINDOW
  // and wants the Space it shows.
  std::string SpaceForBrowser(BrowserId browser_id) const {
    return browsers_->spaces().SpaceOf(browser_id);
  }
  const std::string& active_space() const {
    return browsers_->spaces().active_space();
  }
  // The browser this slot is SHOWING (for tab/nav commands): the active one,
  // else the first Space browser still attached, else invalid — a slot
  // between its last browser going and its own teardown, or one whose
  // adoption failed. NEVER the slot number: that names the browser the slot
  // was OPENED with, which may be long closed (docs/architecture-review.md
  // §3.5). Callers skip the framework call when this is invalid.
  BrowserId active_browser_id() const { return browsers_->spaces().active(); }
  // Legacy spelling of active_browser_id() for the coordinator's int64_t
  // paths; 0 when no browser is attached. Goes with the phase 2 signatures.
  int64_t active_browser() const { return active_browser_id().value; }
  // The page in front of the user, for PAGE commands and the address field
  // (the Mac's BrowserState.pageCommandTarget, a19624bb / c11849cf): the
  // browser of the Peek presented over the active tab — its own, or its
  // split partner's — else active_browser_id(). A peek's page is never the
  // strip-active tab (its opener stays active beneath the panel), so a
  // command resolved through active_browser_id() would act on the page
  // hidden behind the card. Window-scoped commands (new tab, close tab,
  // Back/Forward, history) keep reading active_browser_id().
  BrowserId PageCommandBrowser() const;
  // That page's tab: the presented Peek's own (invalid while its browser
  // is not in the model yet), else active_tab_id().
  TabId PageCommandTab() const;
  // The browser this slot holds for `space_id`, or invalid — feeds the page
  // context menu's Space list (its current-Space exclusion).
  BrowserId BrowserIdForSpace(const std::string& space_id) const {
    return browsers_->spaces().ForSpace(space_id);
  }
  // Spaces this slot has a browser for, in creation order.
  const std::vector<std::string>& space_order() const {
    return browsers_->spaces().order();
  }
  // Drops a browser that went away; returns true if the slot still has one.
  bool ForgetBrowser(BrowserId browser);
  // Closes the browser this slot holds for `space_id` (a Space being
  // deleted), switching to another open Space first so the window never
  // shows a dead one. No-op when the Space is not open here. If it was the
  // slot's ONLY browser, the slot closes with it.
  void CloseSpaceBrowser(const std::string& space_id);
  void RetireSpaceBrowser(const std::string& space_id);
  // Retires the Space's browser AND stops holding the Space, so a switch to
  // it asks for a new browser while this one stays on screen until that
  // replacement attaches (PhiSpaceBrowsers::RetireAndUnmap).
  void RetireAndUnmapSpaceBrowser(const std::string& space_id);
  bool NeedsSpaceReplacement() const;
  // Starts closing the whole slot (its Space browsers, one per Space). MAY
  // DESTROY `this` before returning — the last browser going away tears the
  // slot down — so touch nothing on this object afterwards.
  //
  // INVARIANT: the controller is never destroyed while one of its own nested
  // message loops (a context menu, a prompt, the icon picker — anything
  // under a PhiModalScope) is on the stack. Both close entry points check
  // PhiModalDepthFor(this window) and, while it is non-zero, park the
  // request (a loop up in ANOTHER window does not hold this one) and post
  // kMsgPhiDeferredClose to the shell instead; the handler re-arms itself
  // (kDeferredCloseTimer) until the loop has unwound and only then runs the
  // close for real. Callers therefore may not assume the close happened
  // synchronously — only that it will.
  void RequestClose();
  // Gives the currently shown browser Win32 focus and Chromium activation.
  void FocusActiveBrowser();
  // The same for the page in front of the user (PageCommandBrowser): a
  // presented Peek is a hosted child whose Aura FocusManager only Chromium's
  // own activation reaches, so its browser is focused through the framework;
  // with no peek this is FocusActiveBrowser.
  void FocusPageBrowser();
  // --- Native new-tab page (phi_new_tab_page.cc) ---------------------------
  // The page's omnibox joins this window's ONE omnibox session: `edit` gets
  // the address field's subclass (Enter / Escape / arrows, the suggestion
  // dropdown) and reports its EN_CHANGE through OnOmniboxFieldChanged.
  void AttachOmniboxField(HWND edit);
  void OnOmniboxFieldChanged(HWND field);
  // A Space URL rule re-homed a navigation that began on this window's new
  // tab (the framework's OnRefreshNewTab): the native page returns to its
  // clean state, as the Mac's resetToInitialState does.
  void ResetNativeNtp();
  // A pane's native new-tab page was clicked (or its field took focus):
  // that pane's tab becomes the active one, as a click on the pane's web
  // content would make it — the omnibox session commits into the active
  // tab, so it must be the pane the user is typing in.
  void ActivatePaneTab(int tab_id);

  // The shell window's app menu (⋯; null before the shell exists). The
  // control socket reads and clicks it through here.
  PhiAppMenu* app_menu() { return app_menu_.get(); }
  // Drops the app menu under its ⋯ button (the button's click, F10, Alt+F,
  // Alt+E and the solo Alt tap all land here).
  void ShowAppMenu();

  // Re-lays the chrome for `mode` and repaints. Cheap: all geometry is derived
  // from the client rect on demand, so nothing is cached across the switch.
  void ApplyLayoutMode(PhiLayoutMode mode);
  // Repaints for a palette the host just changed.
  void ApplyPalette();
  // Child surfaces in the sidebar must use the same alpha contract.
  bool sidebar_backdrop_enabled() const { return sidebar_backdrop_; }
  // The content region (client coords, physical pixels) the browser child
  // occupies.
  RECT content_rect();
  // The DPI the chrome is currently laid out for (96 = 100%).
  UINT dpi() const { return dpi_; }
  // Chrome geometry, published so tools and tests can locate what they are
  // asserting about instead of mirroring constants that then drift.
  RECT url_field_rect();
  // The reader button's slot inside the address capsule, empty when the
  // reader is not being offered for the active tab. Published for the same
  // reason as the field: a test asserting the button appeared should read the
  // layout, not count pixels.
  RECT reader_button_rect();
  // The caption band's sidebar toggle (empty in comfortable), and whether
  // the sidebar is collapsed behind it — for the layout verb.
  RECT sidebar_toggle_rect();
  // The app menu button (⋯), for the layout verb.
  RECT app_menu_rect();
  // The sidebar bottom bar's profile (avatar) button, for the layout verb.
  RECT profile_rect();
  // The Update reminder pill (empty without an update), for the layout verb.
  RECT update_rect();
  // The in-field Copy link and site-menu slots, for the layout verb.
  RECT copy_link_rect();
  RECT site_menu_rect();
  // The active tab is one pane of a split.
  bool ActiveTabIsSplit() const;
  // Which corners of the split pane `pane` (client coordinates) meet the
  // content card's rounded outer corners -- PhiRoundRectCoverage's bits
  // (1 TL, 2 TR, 4 BL, 8 BR) -- the framework's rule
  // (phi_split_view_style.h: the lower-left against the sidebar, the
  // lower-right while Windows rounds the window, none in fullscreen); and
  // the card's radius there. A surface over a pane rounds the same corners.
  unsigned SplitPaneCardCorners(const RECT& pane);
  int ContentCornerRadius() const;
  bool sidebar_collapsed() const { return sidebar_collapsed_; }
  // The last user Space this slot showed (never an agent or Incognito
  // Space): where it retreats when its Incognito Space closes. Empty for a
  // shell opened on an Incognito Space that never left it.
  const std::string& last_regular_space() const { return last_regular_space_; }
  // The floating panel is slid in over the page right now (§4.4).
  bool sidebar_floating() const { return floating_sidebar_ && floating_expanded_; }
  // Docks (`collapsed` false) or hides the sidebar; hidden, the panel
  // stays tucked at the window's left edge. ToggleSidebar flips it.
  void SetSidebarCollapsed(bool collapsed);
  // Esc: tucks the slid-in floating panel. True when it was up.
  bool TuckFloatingSidebar();
  // Esc during a sidebar drag (§4.8): cancels it; false when none.
  bool CancelSidebarDrag();
  // The split view's hover chip (docs/windows-native-ui.md §4.6): while
  // the active tab is one pane of a split, a chip at the UNFOCUSED pane's
  // top-right shows its favicon, host and a ⋯ pane menu; it fades in when
  // the pointer enters that pane and leaves 600 ms after it goes. A popup
  // owned by the shell, driven by kSplitChipTimer.
  void SplitChipTick();
  // Runs the chip's poll exactly while the active tab is split.
  void SyncSplitChipTimer();
  // The two signals behind that offer, separately — a test needs to know
  // WHICH one lit the button (upstream's distiller, or the reader
  // extension's own in-page probe and site rules).
  bool ActiveTabExtensionOfferable() const;
  bool ActiveTabDistillable() const;
  int tabs_top();
  int tab_row_height() const;
  // The placed sidebar rows (keys + client rects) for the agent dialect's
  // window.layout, so a box probe can aim a real drag without pixels.
  base::ListValue LayoutRowsJson() const;
  base::DictValue DragStateJson() const;
  // The gap between rows (the Mac's 2pt intercell spacing); row pitch is
  // tab_row_height() + tab_row_gap().
  int tab_row_gap() const;
  int sidebar_width() const;
  // The caption band's height — the row holding the menu titles and the
  // caption buttons. Everything else starts below it.
  int title_bar_height() const;
  // The tinted margin around the content card.
  int content_margin() const;
  // Client origin minus window origin. PrintWindow(PW_CLIENTONLY) renders from
  // the WINDOW origin, so a capture is offset by exactly this — a caller
  // comparing against client coordinates has to add it.
  POINT frame_offset() const;
  // Layout coordinates to SCREEN, for anything hung off a row, chip or
  // button (menus, previews, popovers, pickers). While the floating panel's
  // events run (floating_layout_), the layout is the EXPANDED one and the
  // panel shows it from floating_offset_y_ down at its own origin, so the
  // mapping goes through the panel — the strip's hover card's convention.
  POINT LayoutToScreen(POINT pt) const;
  RECT LayoutToScreen(const RECT& rect) const;

  // Shell window geometry (client-owned; no framework round-trip).
  bool GetBounds(int* x, int* y, int* w, int* h) const;
  bool SetBounds(int x, int y, int w, int h);
  HWND shell_hwnd() const { return shell_hwnd_; }

  // PhiHitSource — THE rect list assistive technology reads (phi_window.h).
  // The sidebar publishes its rows into it on every rebuild (a stable key
  // per row, the row's title as the name, and a role of list item, button
  // or group), and the shell answers WM_GETOBJECT from it. The shell is
  // hand-registered rather than a PhiWindow, which is exactly why the
  // provider takes this interface instead of a window.
  HWND hwnd() const override;
  const std::vector<PhiHit>& hits() const override;
  uint32_t hits_version() const override;
  int HitTest(POINT client_pt) const override;
  bool InvokeHit(int index) override;

  // Window procedure for the client shell window.
  static LRESULT CALLBACK ShellWndProc(HWND hwnd,
                                       UINT msg,
                                       WPARAM wparam,
                                       LPARAM lparam);

 private:
  // Every constant in this class is a DESIGN UNIT — a pixel at 100% scaling.
  // Win32 places windows and paints in PHYSICAL pixels, and the client is
  // per-monitor DPI aware (it inherits Chrome's manifest), so nothing reaches
  // the screen without going through here first. Without it the whole chrome
  // renders 20% small at 125% and half-size at 200%, while the system font —
  // which IS scaled — outgrows the boxes drawn for it.
  int Scaled(int design_units) const;
  RECT Scaled(const RECT& design_units) const;
  // Rebuilds `ui_font_` for `dpi_`. Idempotent for an unchanged DPI.
  void EnsureFont();

  // Every rect the chrome needs, for one layout mode — ShellLayout
  // (phi_shell_layout.h) converted to RECTs. ComputeLayout() answers from a
  // cache keyed by ShellLayoutInputs, so the eight reads per pointer move
  // cost a struct compare; the geometry itself is the pure, host-tested
  // ComputeShellLayout.
  struct Layout {
    PhiLayoutMode mode = PhiLayoutMode::kPerformance;
    RECT client{};
    // The caption band: drag space, then the trailing control cluster and
    // the three caption buttons. The OS caption is gone (WM_NCCALCSIZE) —
    // this row IS the title bar.
    RECT title_bar{};
    RECT caption_min{}, caption_max{}, caption_close{};
    RECT sidebar{};  // Empty in Comfortable.
    RECT top_bar{};  // The band above the content; empty in Performance.
    RECT content{};  // Where the reparented browser child goes.
    // The bookmark bar band, carved off the content's top when visible
    // (the Mac's 32pt BookmarkBar under the header).
    RECT bookmark_bar{};
    RECT app_icon{};
    // Search trails the sidebar header (the caption strip in comfortable).
    // Navigation stays on the address row's leading edge.
    RECT search_tabs{};
    // The Update reminder pill (the Mac's upgrade button) just before
    // search; empty unless an update is on offer.
    RECT update{};
    // The hairline closing the band's trailing cluster, in the gap between
    // search-tabs and the caption buttons. Every layout has one.
    RECT band_divider{};
    RECT back{}, forward{}, reload{};
    RECT spaces_strip{};   // The band the Space chips live in.
    RECT url_field{};
    RECT favorites{};      // Pinned-tab band (or its reserved drop zone).
    RECT bookmarks{};      // The outline band: rows (or zone) + separator.
    RECT stored_band{};    // The two bands' capped, scrolling viewport.
    // The 16pt separator row between the bookmark outline and "+ New Tab"
    // (the Mac's SeparatorItem); empty without bookmarks.
    RECT bookmark_separator{};
    RECT new_tab_row{};    // The sidebar's "+ New Tab" row.
    RECT bottom_bar{};
    // Bottom-bar buttons, the Mac's order: the profile (avatar) button,
    // downloads and memory leading, the Feedback and Chat capsules
    // trailing. Settings has NO bottom-bar button — it is a row of the
    // profile menu and of the app menu, as on Mac.
    RECT sidebar_profile{};    // The Mac's ProfileButton, left-most.
    RECT sidebar_downloads{};  // The bottom bar's own Downloads button.
    RECT memory{}, feedback{}, chat{};
    RECT extensions{};     // The puzzle button (in-field / bottom bar / header).
    // The app menu button (⋯): comfortable's toolbar trailing button, the
    // vertical layout's band cluster. Every layout has one — it is the
    // only way to the menu (docs/windows-native-ui.md §3).
    RECT app_menu{};
    // The globe leading comfortable's omnibox pill, where Chrome shows the
    // site chip. Empty in the other layouts, whose fields carry no icon.
    RECT page_icon{};
    // The floating panel's pin (empty on the shell's own layout) and the
    // sidebar's section labels.
    RECT pin{};
    RECT pinned_label{}, bookmarks_label{}, tabs_label{};
    RECT agent_chip{};  // §5.10: the address-row agent chip; empty for none.
    // Reader View's entry, leading the copy-link button. Empty unless the
    // ACTIVE tab is distillable — the Mac shows the reader button only where
    // Reader View has something to offer.
    RECT reader{};
    // The Mac's hover-only pair trailing Reader: Copy link and the site
    // menu (WebContentAddressBarMenu). Laid out always, painted and
    // live only while the field is hovered (or the menu / flash is up);
    // Reader shows on the same rule, or while Reader View is on.
    RECT copy_link{};
    RECT site_menu{};
    // The vertical sidebar header's toggle (phi_shell_layout.h).
    RECT sidebar_toggle{};
    // The pinned-extension icon strip leading the copy-link button inside
    // the capsule. Empty when nothing is pinned OR the icons would squeeze
    // the text below the Mac's 84pt minimum (they hide, as on Mac).
    RECT field_ext_strip{};
    RECT add_space{};      // The strip's trailing "+" chip.
    // The pip's edge in points, the trailing slot's with it: the vertical
    // layouts fill the strip band with it, comfortable keeps the Mac's
    // compact caption pip.
    int chip_size = 0;
    int chips_left = 0, chips_top = 0;
    int chips_limit = 0;  // Right edge chips may not cross.
    int tabs_left = 0, tabs_top = 0;
    int tabs_limit = 0;   // Bottom (vertical) or right (horizontal) edge.
    bool horizontal_tabs = false;
    bool spaces_switch = true;  // The Spaces strip / compact pip exists.
  };
  Layout ComputeLayout(bool include_floating = true);
  // Physical shell/page bounds never use the floating panel's row geometry.
  Layout ComputeWindowLayout();
  // The cache behind ComputeLayout: the inputs the last layout was derived
  // from and that layout. Re-derived whenever the inputs differ — a resize,
  // a DPI move, a mode/pref change, a count from the rebuild, the floating
  // panel's override — so no explicit invalidation can be forgotten.
  ShellLayoutInputs CurrentLayoutInputs(bool include_floating = true) const;
  static Layout ToLayout(const ShellLayout& shell);
  ShellLayoutInputs layout_inputs_;
  Layout layout_cache_;
  bool layout_cache_valid_ = false;
  // The two inputs that cost host round-trips (BookmarkBarVisible reads
  // two prefs; ActiveTabReaderOfferable asks the reader bridge), captured
  // by RefreshLayoutInputs at the points they can change — the rebuild,
  // LayoutContents, a distillability report — rather than per layout read.
  bool layout_bookmark_bar_ = false;
  bool layout_reader_ = false;
  void RefreshLayoutInputs();

  // `include_child_controls` draws what child windows would draw themselves —
  // set only on the WM_PRINTCLIENT (PrintWindow) path, which renders the
  // parent alone. In WM_PAINT the URL field's EDIT child paints that region.
  void PaintChrome(HDC dc, const Layout& layout, const PhiPalette& palette,
                   bool include_child_controls);
  // Handles a click in the chrome (tab rows, close boxes, buttons, toolbar).
  void OnChromeClick(int x, int y);
  // Middle-click in the chrome: closes the tab row under the pointer (the
  // Mac tab list's otherMouseDown). Pins/bookmarks deliberately ignore it.
  void OnChromeMiddleClick(int x, int y);
  // Right-click in the chrome. On a Space chip this raises the Mac's Space
  // menu (activate / rename / change icon / set theme / delete); elsewhere it
  // does nothing, as on Mac.
  void OnChromeContextMenu(int x, int y);

  // --- Context menus (phi_shell_menus.h) ----------------------------------
  // A menu's nested loop dispatches whatever is queued, kMsgPhiRebuildRows
  // included, so tabs_ / pinned_ / bookmarks_ / group_headers_ / chips_ can
  // be cleared and refilled while one is up. Nothing below takes a reference
  // into them: the hit-test copies the row into a MenuRow, a pure builder
  // turns that into a spec, and the action comes back by key
  // (docs/architecture-review.md §3.1 principle 4, lane 1 §3.2).
  struct MenuRow {
    SidebarKey key;
    TabId id;
    TabId b_id;
    std::string guid;      // Store rows (pins, bookmarks).
    std::string split_id;  // Tab rows: the split this row already belongs to.
    std::string group_id;
    std::wstring title;
    std::string url;    // UTF-8, as the framework and the store want it.
    std::string url_b;  // Pane B of a split row / cell.
    std::string layout;  // A split's remembered divider axis.
    std::string icon;    // A bookmark folder's catalog symbol.
    bool merged = false;
    bool folder = false;
    bool open = false;  // A pinned cell with a live bound tab.
    RECT bounds{};      // Where a popover the action opens hangs off.
  };
  // Screen placement for a popup, resolved from a CLIENT point.
  PhiPopupMenuRequest PopupAt(POINT client_pt, UINT align) const;
  // The dynamic rows the builders take: every Space but this slot's (for the
  // transfer submenus), the full list (for the switchers), this Space's
  // bookmark folders in outline order, and the tab groups other than one.
  // `with_incognito`: list the live Incognito Spaces too (a tab may move
  // into one; a bookmark may not).
  std::vector<PhiMenuChoice> SpaceMenuChoices(bool with_incognito) const;
  std::vector<PhiMenuChoice> SpaceSwitcherChoices() const;
  std::vector<PhiMenuChoice> BookmarkFolderChoices() const;
  std::vector<PhiMenuChoice> GroupMenuChoices(const std::string& exclude) const;
  // The store's bookmark outline as a value, for the bar's dropdown.
  std::vector<PhiMenuBookmarkNode> BookmarkTreeSnapshot(
      const std::string& parent_guid,
      int depth) const;
  // The browser a menu was opened for, re-checked after its loop: still
  // one of this slot's, else invalid. The actions act on THAT browser, never
  // on whichever is active once the loop has unwound.
  BrowserId StillHeld(BrowserId opening_browser) const;
  // One menu each, from a snapshot. The by-value parameters are the point:
  // the copy happens at the call, before the loop.
  void ShowGroupHeaderMenu(POINT pt,
                           std::string group_id,
                           std::wstring title,
                           COLORREF color);
  void ShowTabRowMenu(POINT pt, const MenuRow& row);
  void ShowPinnedCellMenu(POINT pt, const MenuRow& row);
  void ShowBookmarkRowMenu(POINT pt, const MenuRow& row);
  // `space_only`: the Space block alone — a right-click on a Space pip or
  // the Space chip (docs/windows-native-ui.md §5.4). `space_id`: the Space
  // the block acts on (empty = the active Space) — a pip's, by value, so
  // the menu acts on it whether or not the switch to it has landed.
  void ShowTabAreaMenu(POINT pt,
                       bool space_only = false,
                       std::string space_id = std::string());
  // A right-click inside the bookmark bar's band: a pill takes the bookmark
  // row's menu, the bare run the bar's own. False when `pt` is off the bar.
  bool ShowBookmarkBarContextMenu(POINT pt);
  void ShowAgentChipMenu(POINT pt, std::string task);
  // The compact pip's switcher (with_new_space false) and the overflow "…"
  // button's (true, which adds the two creation rows).
  void ShowSpaceSwitcherMenu(POINT pt, bool overflow);
  // The bottom bar's profile (avatar) button: the Mac's populateProfileMenu,
  // dropped upward off the button (docs/windows-native-ui.md §4.2).
  void ShowProfileMenu();

  // Collapses/restores the sidebar (View > Toggle Sidebar, Ctrl+S). The
  // content card widens to the left margin; the margin becomes the hover
  // trigger for the floating sidebar. No-op in the comfortable layout.
  void ToggleSidebar();
  // The New Tab entry points (Ctrl+T, the sidebar row, the menu): honors
  // the "New tab behavior" setting — page mode opens the NTP, omnibox mode
  // focuses the address field and the tab is born on commit.
  void StartNewTab();
  // Opens/closes the downloads popover, anchored to the sidebar's download
  // button — the Window menu's and Ctrl+J's path to the button's toggle.
  void ToggleDownloadsPopover();
  // Re-reads navigation state (url/title/canGoBack/…) for the toolbar.
  void RebuildNavState();

  // The shell's UI font at `ui_font_dpi_`, used both for the address field and
  // for every label PaintChrome draws, so text and boxes scale together.
  HFONT ui_font_ = nullptr;
  UINT ui_font_dpi_ = 0;
  // Space icons are emoji, which the UI font has no glyphs for; Segoe UI Emoji
  // does. GDI renders it MONOCHROME — colour emoji need DirectWrite, which is
  // a much larger change than the icons are worth right now.
  HFONT emoji_font_ = nullptr;
  UINT emoji_font_dpi_ = 0;
  // Segoe MDL2 Assets for the caption glyphs (minimize / maximize / restore /
  // close) — the same glyphs the OS buttons use.
  HFONT caption_font_ = nullptr;
  UINT caption_font_dpi_ = 0;
  // MDL2 at the emoji size, for Space-pip catalog glyphs.
  HFONT chip_icon_font_ = nullptr;
  // The same two faces at the vertical layouts' larger pip size. Built with
  // the caption font, so `caption_font_dpi_` covers them.
  HFONT pip_icon_font_ = nullptr;
  HFONT pip_emoji_font_ = nullptr;
  // The UI face at the strip's text size, for the big pip's initial-letter
  // stand-in and the trailing slot's "+" / "…".
  HFONT pip_text_font_ = nullptr;
  // 11pt labels — the bottom bar's Feedback/Chat capsules (the Mac's 11pt).
  HFONT small_font_ = nullptr;
  // The sidebar's section labels: 12 dip Segoe UI Variable, `dim`.
  HFONT label_font_ = nullptr;
  // The row face at semibold — bookmark folder titles (the Mac's .medium)
  // and tab-group headers (.semibold). Built with ui_font_.
  HFONT ui_semibold_font_ = nullptr;
  // 8pt semibold — the pinned extension icons' badge pill (the Mac's
  // ExtensionBadge). Built with ui_font_.
  HFONT badge_font_ = nullptr;
  UINT dpi_ = 96;
  // A move/size this controller asked for (the framework's SetBounds, the
  // agent watch taking its watcher's frame) is in flight: a WM_DPICHANGED
  // it triggers keeps the requested rect instead of Windows' rescaled one.
  bool exact_bounds_pending_ = false;
  // Where the keyboard was when the window lost activation, for the
  // activation that brings it back (Alt+Tab returns to the address field,
  // or the sidebar ring, rather than always to the page).
  enum class KeyboardHome { kPage, kField, kSidebar };
  KeyboardHome keyboard_home_ = KeyboardHome::kPage;


  // Model freshness: rebuilds happen on the posted turn ScheduleRefresh
  // asks for, never per paint, never from input. A capture verb that needs
  // current rows calls FlushPendingRebuild first.
  bool rows_dirty_ = true;
  bool rebuild_posted_ = false;  // A kMsgPhiRebuildRows is in the queue.
  // Cached paint resources, recreated on resize / palette change instead of
  // per WM_PAINT: the full-window backbuffer bitmap and the ground-gradient
  // band brushes.
  HBITMAP back_buffer_ = nullptr;
  RAW_PTR_EXCLUSION void* back_buffer_bits_ = nullptr;
  SIZE back_buffer_size_{};
  bool sidebar_backdrop_ = false;
  bool backdrop_split_ = false;
  // Where PaintChrome last drew the account picture and its ring (empty: the
  // glyph or no button). The backdrop regrade keeps this disc opaque: a
  // photo is not ink over the ground, and reading it as one tints and washes
  // it out.
  RECT avatar_disc_ = {};
  void UpdateSidebarBackdrop();
  void PrepareBackdropPixels(const PhiPalette& palette);
  // The slot palette's window ground — what uncovered backbuffer pixels show.
  COLORREF BackgroundFill() const;


  // Sidebar edge resize (the Mac's split drag): a 4-DIP grab band in the
  // sidebar's trailing gutter; live while dragging, persisted on release.
  int sidebar_dip_ = kSidebarWidthDefault;
  bool sidebar_resizing_ = false;
  HBITMAP sidebar_resize_snapshot_ = nullptr;
  RECT sidebar_resize_source_{};
  std::vector<HWND> sidebar_resize_hidden_;
  // The browser child on screen when the drag began: LayoutContents shows
  // the CURRENT one on release, so this one is never re-shown blindly.
  HWND sidebar_resize_content_ = nullptr;
  void BeginSidebarResize();
  void ResizeSidebar(int width_dip);
  void EndSidebarResize();
  void PaintSidebarResizePreview(HDC dc);
  bool InSidebarResizeBand(POINT client_pt);


  // Hover feedback (the Mac leans on hover: row washes, close affordances).
  // Which chrome element the cursor rests on, as element-kind + position in
  // its vector; repaints fire only when it changes.
  // kNav: the toolbar's back/forward/reload/search buttons — hover_index_
  // is 0..3 in that order.
  // kAddSpace: the Spaces strip's trailing slot — the "+" or, on overflow,
  // the "…" switcher standing in for it.
  using HoverKind = PhiChromeHover;
  HoverKind hover_kind_ = HoverKind::kNone;
  int hover_index_ = -1;
  // §5.8: the pointer is on the hovered row's surface tile.
  bool surface_tile_hot_ = false;
  void OnChromeMouseMove(POINT client_pt);
  void ClearHover();
  // Tooltips for the chrome's own icon buttons (the caption trio, the nav
  // and address-row buttons, the sidebar's header and bottom bar, and the
  // button hits the sidebar publishes — close boxes, the strip chevron),
  // on THE tooltip (PhiShowTooltipFor). The name under a layout point, or
  // empty; the caption buttons answer by their non-client hit code.
  std::wstring ChromeTipAt(POINT client_pt);
  std::wstring CaptionTip(WPARAM hit) const;
  // The pointer is over `text` (empty: over nothing named): a change hides
  // the tip and restarts the system's tooltip delay. Dismiss hides it for a
  // press until the pointer moves on to something else.
  void UpdateChromeTip(const std::wstring& text);
  void DismissChromeTip();
  // UpdateChromeTip for whatever is under the pointer now (a leave
  // notification: client and non-client leaves cross in either order).
  void RetipAtCursor();
  std::wstring chrome_tip_;
  std::wstring shell_title_;  // What SetWindowText last received.
  // The client rect a hovered element's wash covers, empty for none — so a
  // hover change invalidates the old and new element, not the whole shell
  // (lane 3 §7 item 3).
  RECT HoverRect(HoverKind kind, int index);
  void SetHover(HoverKind kind, int index);
  // The chrome buttons a kNav hover indexes, in PaintChrome's order.
  std::array<RECT, 23> NavHoverTable(const Layout& layout) const;
  // The 12/600 face the Space chip, the agent chip and the Feedback pill
  // share; built on demand and rebuilt when the DPI moves.
  HFONT EnsureChipNameFont();
  // §5.10: the agent chip's data from the slot's agent pips (the active
  // Space's agent = the full chip; a running agent elsewhere = compact).
  void ComputeAgentChip();
  void PaintAgentChip(HDC dc, const RECT& rect, const PhiPalette& palette,
                      bool hot);

  // The hovered row's page-snapshot card (the Mac's TabPreviewController).
  // Unlike the Space card above it floats in its own popup, so it can hang
  // over the Chromium child window.
  std::unique_ptr<PhiTabPreview> tab_preview_;
  // --- The client-drawn pages, as a component (phi_native_pages_host.h) ----
  // The bookmark manager and the native new-tab page, mounted over the
  // content card by the ACTIVE tab's URL (docs/architecture-review.md §3.5,
  // lane 1 cluster M). Nothing else creates, shows or hides either one.
  std::unique_ptr<PhiNativePagesHost> native_pages_;
  void UpdateTabPreview();  // Re-aims the card at the hovered row.

  // Whether to offer the reader control for the active tab: upstream's
  // distiller verdict OR the reader extension's own probe — either alone is
  // incomplete, since the distiller misses pages a site rule rescues and the
  // probe misses tabs whose content script never ran — and always while the
  // reader is already up, which is the way back out of it.
  bool ActiveTabReaderOfferable() const;
  // True when the split holding `tab_id` stacks its panes (the wire's
  // "horizontal"); false for side-by-side and for a tab in no split.
  bool SplitIsStacked(TabId tab_id) const;
  // The framework's CURRENT state for `browser`, pulled typed — for the few
  // gesture paths that just mutated the strip and act on the result inside
  // the same turn, before the coordinator's model has heard the push. Never
  // for rows or paint: those read model_.
  bool PullSnapshot(BrowserId browser,
                    phi_framework::WindowSnapshot* out) const;
  // The bookmark bar (the Mac's 32pt HorizontalBar/BookmarkBar): root-level
  // rows as favicon+title pills, the rest behind a trailing "›" menu.
  // Items are laid out by the paint pass and hit-tested from the same list.
  struct BarItem {
    RECT bounds{};
    int bookmark = -1;    // Index into bookmarks_ (a depth-0 row).
    bool overflow = false;
  };
  std::vector<BarItem> bar_items_;
  bool BookmarkBarVisible();
  void PaintBookmarkBar(HDC dc, const Layout& layout,
                        const PhiPalette& palette);
  // A dropdown of bookmark rows under `anchor`: a bar folder's children
  // (parent_guid set) or the root's overflow (skip_first shown pills).
  void ShowBookmarkBarMenu(RECT anchor,
                           std::string parent_guid,
                           size_t skip_first);
  // The tab whose native new-tab page blanked on an omnibox commit; the
  // controls return once another tab is active or that load has ended.
  TabId ntp_commit_tab_id_;
  // Counted before the layout is derived, because the bands they occupy push
  // everything below them down.
  int pinned_row_count_ = 0;
  int bookmark_row_count_ = 0;
  // A drag in flight that could land in an empty stored band: the layout
  // reserves that band's drop zone (SidebarSetDropZones).
  bool pinned_drop_zone_ = false;
  bool bookmark_drop_zone_ = false;
  // The Spaces-switch state the chrome was last laid out for: a Space
  // count crossing the single-Space line moves every band, so the rebuild
  // that notices it re-lays the children and repaints the whole shell.
  bool laid_out_spaces_switch_ = true;

  // The page changes AFTER the band slide completes (the Mac reveals the
  // target on completion): the entering child stays hidden and the leaving
  // one visible until then.
  HWND swap_reveal_ = nullptr;   // Shown when the slide lands.
  HWND swap_conceal_ = nullptr;  // Hidden when the slide lands.
  void RevealSwappedContent();
  // Chrome + drag indicator, using the same palette as alpha recovery.
  void ComposeFrame(HDC mem, const PhiPalette& palette);

  // --- The comfortable layout's Space switch (phi_content_slide.h) --------
  // No sidebar band to push in there: the whole client area slides — the
  // leaving Space's composite out, the entering Space's in — with the
  // caption buttons standing still (the Mac's performHorizontalWindowSlide).
  // The vertical layouts keep the strip's band slide; BrowserHostBeginSpace-
  // Swap starts one or the other, and both park the entering browser child
  // until they land (RevealSwappedContent) — the comfortable slide carries
  // the entering page as pixels the compositor hands over
  // (StartTabSurfaceCapture) instead of moving the live child.
  std::unique_ptr<PhiContentSlide> content_slide_;
  // The band slide's reveal, bridged (phi_reveal_veil.h). Made on first use.
  std::unique_ptr<PhiRevealVeil> reveal_veil_;
  void BeginContentSlide(const std::string& space_id, BrowserId entering);
  // The client area as it is on screen right now: the chrome from our own
  // paint (caption buttons left out, as the Mac fades its traffic lights
  // before the capture) and the content rect read off the screen — the
  // browser child paints through DirectComposition, which no PrintWindow
  // reads. Null when the window is not on screen to be read.
  HBITMAP CaptureLeavingComposite();
  // The posted turn after the switch: asks the framework for the entering
  // page (retried on kSlideHoldTimer while the just-shown view has no frame
  // yet), and releases the slide when it arrives or the hold runs out.
  void StartSlideCapture();
  void ReleaseContentSlide();
  void CancelContentSlide();
  int slide_capture_request_ = 0;
  int slide_capture_tries_ = 0;
  BrowserId slide_entering_browser_;
  TabId slide_entering_tab_;
  static constexpr UINT_PTR kSlideHoldTimer = 0x51D;
  static constexpr UINT kSlideHoldTickMs = 16;
  static constexpr int kSlideHoldTries = 5;  // ~80 ms, then it goes blank.
  // PaintChrome leaves the caption buttons out while set: the slide paints
  // them itself, last and unslid, and the leaving composite carries none.
  bool caption_buttons_suppressed_ = false;
  // A tick's frame: the two composites blitted at their slid positions,
  // the caption buttons over them, straight to the window.
  void ComposeSlideFrame();
  // The client-sized back buffer WM_PAINT composes into, (re)made for `dc`.
  HDC AcquireBackBuffer(HDC dc, HGDIOBJ* old_bitmap);
  // The half of LayoutContents that places what sits against the content
  // rect — the browser child, the watch overlay, the corner caps.
  void PlaceContentChildren(const Layout& layout);
  // PhiContentSlideShell:
  HWND ContentSlideWindow() const override;
  void ContentSlideMoved() override;
  void ContentSlideLanded() override;

  // Tabs upstream's distiller says carry an article. Positive-only reports,
  // so entries are added and cleared when the tab navigates away.
  std::set<TabId> distillable_tabs_;
  // Pinned-extension icons inside the address capsule (SideAddressBar's
  // right-stack lead): the pinned set with resolved 16px icons and each
  // action's state on the active tab (badge, enabled, grayscale — a hidden
  // page action is left out), refreshed with the tab list and on
  // OnExtensionsChanged, which the framework also raises for a state change.
  struct FieldExtension {
    std::string id;
    std::vector<unsigned char> icon;  // Premultiplied BGRA; may be empty.
    int icon_w = 0;
    int icon_h = 0;
    PhiExtensionActionState state;
  };
  std::vector<FieldExtension> field_extensions_;
  // The count ComputeLayout carves capsule slots for (it cannot call the
  // framework itself).
  int field_ext_count_ = 0;

 public:
  // Re-reads the pinned set + icons (delegate's OnExtensionsChanged, and
  // every tab-list rebuild).
  void RefreshFieldExtensions();
  // The extensions (puzzle) button in SCREEN pixels — where Chromium anchors
  // the extension bubbles it would hang off its own toolbar button (the
  // post-install notice, the uninstall confirmation). False when the layout
  // carries no such button.
  bool ExtensionsButtonScreenRect(RECT* rect);

 private:
  // Slot `i` of the in-field icon strip, or an empty rect without one.
  RECT FieldExtRect(const Layout& layout, size_t i) const;
  // The action's context menu at in-field icon `index`'s bottom-left
  // corner (`slot` in shell client coordinates) — the Mac's secondaryAction,
  // and what a click on a disabled action falls back to.
  void ShowFieldExtensionMenu(size_t index, const RECT& slot);
  // The in-field site menu (the Mac's WebContentAddressBarMenu) under
  // `slot`, for the active tab; applies the chosen row after the loop.
  void ShowSiteMenu(const RECT& slot);
  // The in-field Copy link / site-menu pair shows: the field is hovered,
  // the menu is open, or the copy flash is still up.
  bool FieldToolsShown() const;
  // Dismisses the active browser's extension action popup when native shell
  // activity bypasses Chromium's widget activation.
  void CloseExtensionPopup();

  // The slot's identity. Not a browser: the browsers this slot holds live
  // in space_browsers_ / browser_hwnds_, and "the original browser" is
  // simply whichever of them is still attached.
  const SlotId slot_;
  // --- The content area, as a component (phi_browser_host.h) ---------------
  // Chromium's HWND adopted as a child, the browser this slot holds for each
  // Space, placeholder and content-fullscreen mode, and the overlays that
  // ride above the page (docs/architecture-review.md §3.5, lane 1 clusters E
  // and G). It owns BrowserId -> HWND; SetParent and subclassing happen
  // there and nowhere else.
  std::unique_ptr<PhiBrowserHost> browsers_;
  HWND shell_hwnd_ = nullptr;  // Our client-owned top-level.
  // Set at the top of the destructor: the shell's DestroyWindow still
  // delivers WM_ACTIVATE / WM_SIZE / WM_CAPTURECHANGED to ShellWndProc on
  // the way down, and those handlers must not lay out, refresh or call the
  // host for an object whose destructor body is executing.
  bool destroying_ = false;
  // Close requests parked by RequestClose / CloseSpaceBrowser while a nested
  // loop was up (see the RequestClose invariant); drained by
  // RunDeferredClose on the posted kMsgPhiDeferredClose turn.
  bool deferred_slot_close_ = false;
  // RequestClose began tearing the window down (see closing()).
  bool closing_ = false;
  std::vector<std::string> deferred_space_closes_;
  void PostDeferredClose();
  void RunDeferredClose();
  // The caption band (phi_caption_band.h): the row that replaced the OS
  // caption. It owns the menu-bar host, the measured title rects, the
  // reveal state machine, the caption buttons and the WM_NC* frame; the
  // shell forwards it messages and asks it to paint its rect.
  std::unique_ptr<PhiCaptionBand> caption_band_;
  // The palette a context menu was opened with; PopupAt points the request
  // at it and it must outlive the menu loop.
  mutable PhiPalette popup_palette_;
  // The app menu (⋯), the shell's own control since the menu bar went.
  std::unique_ptr<PhiAppMenu> app_menu_;
  // --- The address field, as a component (phi_omnibox_session.h) -----------
  // The single owner of the EDIT, its subclass, the suggestion dropdown, the
  // results and the navigation state (docs/architecture-review.md §3.5,
  // lane 1 cluster F). The shell forwards the field's messages here and
  // reads back what it paints; nothing else touches any of it.
  //
  // NOT reset in the destructor body: the EDIT dies with the shell window a
  // few lines further down and its last messages must still find a live
  // session (they take the OmniboxShellDestroying path).
  std::unique_ptr<PhiOmniboxSession> omnibox_;
  // --- PhiCaptionShell: what the caption band asks of its shell ------------
  HWND CaptionShellWindow() const override;
  int CaptionScaled(int design_units) const override;
  UINT CaptionDpi() const override;
  HFONT CaptionGlyphFont() const override;
  PhiPalette CaptionPalette() const override;
  PhiCaptionRects CaptionRects() override;
  bool CaptionStripTakesPoint(POINT client_pt) override;
  void CaptionClearShellHover() override;
  bool CaptionContentFullscreen() const override;

  // --- PhiAppMenuShell: what the app menu asks of its shell ----------------
  // This interface RETIRED `friend class PhiMenuBar` and the six const
  // reference views the shell used to publish so the bar's by-name reaches
  // kept compiling (lane 1 §2.1 item 3); the app menu inherited it.
  HWND MenuShellWindow() const override;
  SlotId MenuSlot() const override;
  bool MenuIncognito() const override;
  SidebarHost* MenuSidebarHost() const override;
  SpacesHost* MenuSpacesHost() const override;
  ChromePrefsHost* MenuPrefsHost() const override;
  WindowHost* MenuWindowHost() const override;
  phi_framework::PhiFramework* MenuFramework() const override;
  const PhiWindowModel* MenuModel() const override;
  BrowserId MenuActiveBrowser() const override;
  TabId MenuActiveTab() const override;
  BrowserId MenuPageCommandBrowser() const override;
  TabId MenuPageCommandTab() const override;
  std::string MenuActiveSpace() const override;
  PhiPalette MenuPalette() const override;
  int MenuScaled(int design_units) const override;
  HWND MenuAddressField() const override;
  void MenuFocusPageForFind() override;
  void MenuFocusAddressField() override;
  void MenuRefreshNavState() override;
  PhiAppMenuNavState MenuNavState() const override;
  std::vector<PhiAppMenuTab> MenuTabRows() const override;
  bool MenuReaderActive() const override;
  bool MenuReaderOfferable() const override;
  bool MenuLeaveReaderForNavigation() override;
  std::vector<PhiAgentPipView> MenuAgentSpaces() const override;
  void MenuSwitchSpace(const std::string& space_id) override;
  void MenuNewTab() override;
  void MenuToggleSidebar() override;
  void MenuToggleReader(bool from_accelerator) override;
  bool MenuAgentHoldsInput() const override;
  void MenuToggleDownloads() override;
  void MenuToggleSearchTabs() override;
  void MenuToggleLibrary() override;
  void MenuOpenFeedback() override;
  bool MenuCloseTabEndsIncognitoSpace() override;
  void MenuCloseIncognitoSpace() override;
  void MenuCreateSpace() override;
  void MenuEditSpaceTheme() override;
  void MenuChangeSpaceIcon() override;
  void MenuTriggerExtension(const std::string& extension_id) override;
  bool MenuSidebarShown() const override;

  // --- PhiBrowserHostShell: what the content host asks of its shell --------
  HWND BrowserHostShellWindow() const override;
  int BrowserHostScaled(int design_units) const override;
  PhiPalette BrowserHostPalette() const override;
  RECT BrowserHostContentRect() override;
  RECT BrowserHostClientRect() override;
  HFONT BrowserHostStatusFont() const override;
  phi_framework::PhiFramework* BrowserHostFramework() const override;
  bool BrowserHostIsAgent() const override;
  void BrowserHostRelayout() override;
  void BrowserHostRefreshTabs() override;
  void BrowserHostScheduleRefresh() override;
  void BrowserHostBeginSpaceSwap(const std::string& space_id,
                                 BrowserId entering) override;
  bool BrowserHostSwapRunning() const override;
  void BrowserHostNoteSwapChildren(HWND reveal, HWND conceal) override;
  void BrowserHostEndSpacePreview() override;
  void BrowserHostNoteSpaceWindow(const std::string& space_id,
                                  BrowserId browser) override;
  void BrowserHostNoteActiveSpace(const std::string& space_id) override;
  void BrowserHostAbandonTyping() override;
  void BrowserHostRetintChrome() override;
  void BrowserHostFocusAddressField() override;
  bool BrowserHostFocusNativePage() override;
  bool BrowserHostDeferSpaceClose(const std::string& space_id) override;

  // --- PhiOmniboxShell: what the session asks of its shell ------------------
  HWND OmniboxShellWindow() const override;
  bool OmniboxShellDestroying() const override;
  int OmniboxScaled(int design_units) const override;
  HFONT OmniboxTextFont() const override;
  HFONT OmniboxIconFont() const override;
  HFONT OmniboxChipFont() const override;
  HFONT OmniboxHintFont() const override;
  PhiPalette OmniboxPalette() const override;
  PhiPalette OmniboxFieldPalette() const override;
  PhiLayoutMode OmniboxLayoutMode() const override;
  PhiOmniboxCapsule OmniboxCapsule() override;
  phi_framework::PhiFramework* OmniboxFramework() const override;
  BrowserId OmniboxBrowser() const override;
  BrowserId OmniboxPageBrowser() const override;
  bool OmniboxBrowserPlaceholding() const override;
  bool OmniboxNewTabFocusesField() const override;
  void OmniboxFocusContent() override;
  void OmniboxFieldPressed() override;
  void OmniboxLeaveReader() override;
  HWND OmniboxNativePageField() const override;
  bool OmniboxNativePageBox(RECT* out) const override;
  bool OmniboxContentScreenRect(RECT* out) override;
  bool OmniboxClosesFloating(UINT vk) const override;
  void OmniboxNativePageCommitting() override;

  // --- PhiNativePagesShell: what the native pages ask of their shell -------
  HWND NativePagesShellWindow() const override;
  PhiShellWindow* NativePagesOwner() override;
  bool NativePagesIncognito() const override;
  bool NativePagesIsAgent() const override;
  bool NativePagesContentFullscreen() const override;
  bool NativePagesPlaceholding() const override;
  bool NativePagesBlankPlaceholder() const override;
  bool NativePagesSwapPending() const override;
  const std::string& NativePagesActiveUrl() const override;
  int NativePagesActiveTabId() const override;
  std::vector<NativePane> NativePagesSplitPanes() const override;
  void NativePagesRaiseCaps() override;
  void NativePagesFocusContent() override;
  HWND NativePagesContentWindow() const override;
  bool NativePagesCommitPending() const override;

  // --- PhiSidebarShell: what the sidebar asks of its shell -----------------
  HWND SidebarShellWindow() const override;
  PhiShellWindow* SidebarOwner() override;
  int SidebarScaled(int design_units) const override;
  UINT SidebarDpi() const override;
  int SidebarWidthDip() const override;
  PhiSidebarRects SidebarRects() override;
  void SidebarSetRowCounts(int pinned_rows, int bookmark_rows) override;
  void SidebarSetDropZones(bool pinned, bool bookmark) override;
  void SidebarRefreshLayoutInputs() override;
  PhiPalette SidebarPalette() const override;
  BrowserId SidebarActiveBrowser() const override;
  std::string SidebarActiveSpace() const override;
  bool SidebarPullSnapshot(
      BrowserId browser,
      phi_framework::WindowSnapshot* out) const override;
  bool SidebarSplitIsStacked(TabId tab_id) const override;
  bool SidebarSplitPaneRects(RECT* primary, RECT* secondary) const override;
  HFONT SidebarGlyphFont() const override;
  HFONT SidebarSemiboldFont() const override;
  HFONT SidebarTextFont() const override;
  PhiChromeHover SidebarHoverKind() const override;
  int SidebarHoverIndex() const override;
  bool SidebarSurfaceTileHovered() const override;
  bool SidebarSurfaceFavicon(TabId origin_tab_id,
                             std::vector<unsigned char>* bgra,
                             int* width,
                             int* height) const override;
  PhiSpacesStrip* SidebarSpacesStrip() override;
  void SidebarScheduleRefresh() override;
  void SidebarSyncNativePages() override;
  PhiDragBitmap SidebarCaptureChrome(const RECT& client_rect) override;
  HWND SidebarContentWindow() const override;
  void SidebarStartNewTab() override;
  void SidebarPublishHits(std::vector<PhiHit> hits) override;
  // The sidebar's slice of a per-turn Layout.
  PhiSidebarRects BandRects(const Layout& layout) const;
  // `client_rect` of the shell's chrome as it paints right now, opaque —
  // what the drag image lifts the dragged element's pixels out of.
  PhiDragBitmap CaptureChromeRect(const RECT& client_rect);
  // THE rect list the accessibility provider reads (phi_window.h): the
  // sidebar publishes its rows here on every rebuild. Not yet answered on
  // WM_GETOBJECT — the shell window is still hand-registered rather than a
  // PhiWindow, so this list is filled and ready but not yet reachable by a
  // screen reader (see the report).
  std::vector<PhiHit> hits_;
  uint32_t hits_version_ = 0;

  // --- PhiSpacesStripShell: what the Spaces strip asks of its shell --------
  HWND SpacesShellWindow() const override;
  PhiShellWindow* SpacesOwner() override;
  SlotId SpacesSlot() const override;
  bool SpacesIncognito() const override;
  bool SpacesIsAgent() const override;
  int SpacesScaled(int design_units) const override;
  PhiSpacesStripRects SpacesRects() override;
  const std::string& SpacesActiveSpace() const override;
  bool SpacesHasSpace(const std::string& space_id) const override;
  PhiPalette SpacesPalette() const override;
  bool SpacesDragActive() const override;
  int SpacesHoveredChip() const override;
  int SpacesTabCount(const std::string& space_id) const override;
  bool SpacesAddSlotHovered() const override;
  RECT SpacesAddSlotDirtyRect() override;
  void SpacesForgetHover() override;
  void SpacesRehoverAtCursor() override;
  void SpacesScheduleRefresh() override;
  void SpacesApplyPalette() override;
  void SpacesRefreshTinted() override;
  void SpacesRepaintOmnibox() override;
  void SpacesFocusContent() override;
  void SpacesSetCoveredContentSuppressed(bool suppressed) override;
  void SpacesRevealSwappedContent() override;
  void SpacesSwapBandModel() override;
  void SpacesDropLeavingBand() override;
  HFONT SpacesUiFont() const override;
  HFONT SpacesSmallFont() const override;
  HFONT SpacesEmojiFont() const override;
  HFONT SpacesChipIconFont() const override;
  HFONT SpacesPipIconFont() const override;
  HFONT SpacesPipEmojiFont() const override;
  HFONT SpacesPipTextFont() const override;
  bool SpacesSidebarCanHostForm(const RECT& form) const override;
  bool SpacesCreateFlyoutAnchor(RECT* screen) const override;
  HWND SpacesFloatingPanel(int* offset_y) const override;
  // The strip's slice of a per-turn Layout.
  PhiSpacesStripRects StripRects(const Layout& layout) const;

  // Owned by the framework (a singleton in chrome.dll); non-owning, and across
  // a module boundary, so not a raw_ptr.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* const framework_;
  // The five narrow services the shell was constructed with (§3.5). The
  // coordinator implements all five and owns this controller, so every
  // pointer outlives it; each component below is handed only the ones it
  // calls.
  const ShellServices services_;
  // The coordinator's window model (it owns the instance and outlives every
  // controller): the one source for rows, nav state, splits and groups.
  RAW_PTR_EXCLUSION const PhiWindowModel* const model_;
  // The page colour the chrome follows and whether the reader hides it from
  // the strip's active tab -- settled on the rebuild turn (RebuildTabs), like
  // the nav state, so a colour that arrives alone still repaints.
  std::optional<PhiColor> page_color_;
  bool page_color_reader_ = false;
  // The site menu is tracking (its button stays lit and shown).
  bool site_menu_open_ = false;
  std::string last_regular_space_;  // See last_regular_space().
  // A Library open waiting for its Space's browser to arrive
  // (OpenStoredRowInSpace): opened by BrowserHostNoteActiveSpace once the
  // switch lands, dropped when another Space lands first.
  struct PendingStoredOpen {
    std::string space_id;
    std::string guid;
    std::string url;
  };
  std::optional<PendingStoredOpen> pending_stored_open_;
  // Runs the pending open when `landed_space` is its Space. A fresh browser
  // lands through AttachSpaceBrowser (which never notes the active Space),
  // an attached one through BrowserHostNoteActiveSpace; only the latter,
  // being the user's visible landing, drops a pending open for another
  // Space (`drop_on_mismatch`) — a background attach must not.
  void ConsumePendingStoredOpen(const std::string& landed_space,
                                bool drop_on_mismatch);
  // The dark flag the DWM backdrop was last given (UpdateSidebarBackdrop).
  bool backdrop_dark_ = false;
  // Whether the slot's settled palette (its active Space's) is dark.
  bool SettledDark() const;
  // Rebuilds the paint caches (tabs_/pinned_/bookmarks_/group_headers_,
  // the nav state, the chips) from model_ and the host's stores WITHOUT
  // invalidating. Runs on the posted rebuild turn; a few structural view
  // changes (a Space switch, a layout mode change, the floating panel
  // opening) run it directly through RefreshTabs so the next frame is not a
  // frame of the old geometry.
  void RebuildTabs();
  // Titles the shell after the presented Space's active tab, or after the
  // Space while the placeholder page is up (the Mac's 9387a460 / 9068b449).
  // Idempotent; every rows rebuild and placeholder change calls it.
  void RetitleShell();
  // RebuildTabs + an invalidation of the whole shell. For the structural
  // view changes above only — a framework mutation never needs it, the
  // model's push schedules the rebuild.
  void RefreshTabs();
  // What the rebuild turn compares to invalidate only the bands whose rows
  // changed: a cheap signature of each band's rows.
  struct BandSignature {
    std::vector<std::pair<std::string, RECT>> rows;  // key + bounds.
    std::vector<std::pair<std::string, RECT>> chips;
    std::wstring nav_url;
    bool can_go_back = false;
    bool can_go_forward = false;
    bool nav_loading = false;
    // The page colour the chrome follows (-1 = none), and whether the reader
    // hides it from the strip's active tab.
    int64_t page_color = -1;
    bool reader_shown = false;
    ShellLayoutInputs layout;
  };
  BandSignature CurrentBandSignature() const;
  // ActivePageColor's eligibility, evaluated on the rebuild turn.
  std::optional<PhiColor> ComputeActivePageColor() const;
  // Invalidates what changed between two signatures (everything when the
  // layout itself moved).
  void InvalidateChangedBands(const BandSignature& before);
  // --- The Spaces strip, as a component (phi_spaces_strip.h) --------------
  // The pips, their viewport and overflow, the hover card, the trailing "+"
  // and its fade, the create-Space form host and the Space-switch animation
  // (docs/architecture-review.md §3.5, lane 1 clusters K and O plus the chip
  // half of H). The shell keeps only the RETAINED sidebar rows the band
  // slide paints (leaving_band_ below) — they are the sidebar's, not the
  // strip's — and the parked browser child the slide reveals.
  std::unique_ptr<PhiSpacesStrip> spaces_;
  // --- The sidebar, as a component (phi_sidebar_view.h) --------------------
  // The row pipeline, the band's paint, its hit tests, its scrolling, the
  // expanded-bookmark set, the MRU order and the drag session
  // (docs/architecture-review.md §3.5, lane 1 clusters H, I and J plus the
  // sidebar half of Q). The shell keeps the geometry the rows are placed
  // in, the hover cursor they share with the rest of the chrome, and the
  // resize band that sets the width.
  std::unique_ptr<PhiSidebarView> sidebar_;
  PhiLayoutMode layout_mode_ = PhiLayoutMode::kPerformance;

  // The split view's chrome, pushed per hosted browser (the Mac's
  // SplitPaneHostView look, drawn by the framework's MultiContentsView):
  // the Space's window ground, border and accent, and the top inset the
  // layout mode implies. Re-pushed from LayoutContents / ApplyPalette, and
  // only when a value moved — the framework re-lays out on every push.
  void PushSplitViewStyle();
  std::map<int64_t, phi_framework::SplitViewStyle> split_style_pushed_;

  // The floating sidebar (docs/windows-native-ui.md §4.4): with the
  // sidebar hidden, an invisible left-edge hover zone reveals a popup
  // showing the REAL sidebar over the page. Leaving slides it completely
  // out and hides its native window; the toggle docks it. PaintChrome
  // runs against the expanded
  // layout (floating_layout_ override, at the panel's own height) and
  // the panel forwards its mouse events through the normal handlers.
  bool sidebar_collapsed_ = false;
  // Hosted as a hidden agent Space (off-screen, off-taskbar). See the ctor.
  bool agent_ = false;
  // Off-the-record shell: fixed-dark, no Spaces/pins/bookmarks, unrouted.
  bool incognito_ = false;
  // Behind the onboarding gate: created but not shown (see ShowHeldShell).
  bool shell_hidden_ = false;
  // PresentBehindOnFirstShow: the next PresentBrowser shows the shell
  // inactive, below `present_behind_`.
  bool present_behind_pending_ = false;
  HWND present_behind_ = nullptr;
  // For an agent window: whether it is currently surfaced (being watched).
  bool agent_visible_ = false;
  bool agent_owned_ = false;
  // The agent Space this USER slot hosts for a watch (see HostAgentSpace).
  std::string hosted_agent_task_;
  std::string hosted_agent_space_;
  std::string hosted_return_space_;  // The user Space to swap back to.
  // The agent Space on its way out: the leaving page of the running swap
  // (a user Space or the next agent Space entering). It paints until the
  // slide lands; FinishUnhost then detaches its browser and hands it back
  // (or at once, when no slide runs).
  std::string leaving_agent_task_;
  std::string leaving_agent_space_;
  void FinishUnhost();
  bool floating_layout_ = false;
  // A sidebar drag armed from the floating panel: until it ends the panel is
  // the drag's window (capture, timers, paint) and floating_layout_ stays
  // set between its events, as a menu opened from the panel holds it.
  bool floating_drag_ = false;
  int floating_forward_depth_ = 0;
  HWND floating_sidebar_ = nullptr;
  // The horizontal layout's Space chip width (dip), measured from the
  // active Space's name (§5.2); refreshed with the layout inputs.
  int space_chip_dip_ = 0;
  // §5.10 cue 3: the address-row agent chip, recomputed with the layout
  // inputs. `agent_chip_task_` names the task it stands for; compact =
  // the mark and dot only (a user Space while an agent works elsewhere).
  int agent_chip_dip_ = 0;
  // The Update reminder's measured width in dip (0 = no update on offer).
  int update_dip_ = 0;
  bool agent_chip_compact_ = false;
  std::string agent_chip_task_;
  std::string agent_chip_space_;
  std::wstring agent_chip_name_;
  std::wstring agent_chip_status_;
  std::wstring agent_chip_mark_;   // The brand mark's stand-in glyph.
  COLORREF agent_chip_dot_ = 0;
  HFONT chip_name_font_ = nullptr;
  UINT chip_name_font_dpi_ = 0;
  // Slid in (true) or tucked (false); the slide between the two runs on
  // the tick timer from `floating_slide_start_`.
  bool floating_expanded_ = false;
  ULONGLONG floating_slide_start_ = 0;
  int floating_slide_from_ = 0;
  int floating_slide_to_ = 0;
  // The panel's client height while it is up: the expanded layout the
  // panel paints is computed for THIS height, so its bands fit the panel.
  int floating_panel_h_ = 0;
  // The shell's paints, counted, and the count the panel last repainted
  // at: the tick repaints the panel only when the chrome it mirrors did
  // (a rebuild, a hover card, a theme ramp) or while it slides — an idle
  // panel stays idle. Its own pointer events repaint it directly.
  uint32_t chrome_paints_ = 0;
  uint32_t floating_painted_at_ = 0;
  HWND split_chip_ = nullptr;
  int split_chip_tab_ = 0;  // The pane's tab while shown; 0 = hidden.
  ULONGLONG split_chip_leave_tick_ = 0;
  void ShowSplitChip(int tab_id, const RECT& pane);
  void HideSplitChip();
  void PaintSplitChip(HDC dc);
  void OnSplitChipClick(POINT chip_pt);
  const TabRow* SplitRowForTab(int tab_id) const;
  static LRESULT CALLBACK SplitChipProc(HWND hwnd,
                                        UINT msg,
                                        WPARAM wparam,
                                        LPARAM lparam);
  int floating_offset_y_ = 0;
  ULONGLONG floating_shown_tick_ = 0;
  ULONGLONG floating_outside_tick_ = 0;
  // The pointer entered the invisible edge zone (the 300 ms dwell).
  ULONGLONG floating_hover_tick_ = 0;
  // The band (chips, pinned grid, outline, tab rows) belongs to this
  // shell's own paint and pointer: false while the sidebar is collapsed and
  // the turn is not the floating panel's.
  bool BandOnShell() const;
  // Creates the tucked panel for a hidden sidebar (no-op when it exists,
  // the sidebar is docked, the layout is horizontal or the page is
  // fullscreen) and destroys it for the other states.
  void EnsureFloatingPanel();
  void DestroyFloatingPanel();
  // The panel's screen rect, tucked or slid in.
  RECT FloatingPanelRect(bool expanded) const;
  // Re-seats the panel after the shell moved or resized (hidden while the
  // shell is minimised).
  void PositionFloatingPanel();
  // Clips the panel to the shell's client area: a popup is not clipped by
  // its owner, so the tucked panel's off-window part (and the slide's)
  // would otherwise paint over the desktop beside the window. The rounded
  // region stays at rest too; native shadows are disabled to obey the clip.
  void ClipFloatingPanel();
  // Starts the 150 ms slide to the expanded or tucked position.
  void SlideFloatingPanel(bool expanded);
  // Slides the panel in (the left-edge hover, a drag crossing the edge).
  void ShowFloatingSidebar();
  // Slides it back to its tucked position.
  void HideFloatingSidebar(bool animated);
  void FloatingSidebarTick();
  void PaintFloatingSidebar(HDC dc);
  static LRESULT CALLBACK FloatingSidebarProc(HWND hwnd,
                                              UINT msg,
                                              WPARAM wparam,
                                              LPARAM lparam);
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SHELL_WINDOW_H_

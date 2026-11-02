# Mac parity: what the Windows client has, lacks and leaves out

This is the weekly feature-parity ledger between the macOS client
(`phibrowser-mac`) and this Windows client. It lists every user-facing Mac
feature once, says whether Windows has it, and for every gap records
either the decision to leave it out or that it still needs to be built.
It is a list of surfaces, not of code: how a feature is implemented is in
the owning doc each row points to.

**Checked against phibrowser-mac `8c0b8596` (2026-11-02) and
phibrowser-win `098dcb3`, on 2026-11-02.**

## Legend

| Mark | Meaning |
|---|---|
| ✅ | Ported. Behaviour matches the Mac closely enough that a user moving between the two is not surprised. |
| ◐ | Partial. The surface exists on Windows but a named piece of the Mac behaviour is missing. The missing piece is the row's "Gap". |
| 🔧 | To implement. Nothing or almost nothing on Windows, and no decision to skip it. |
| ⛔ | Deliberately left out. The "Gap / decision" column names the decision and where it is recorded. A ⛔ row is revisited only when its reason changes. |
| ❓ | Needs a decision. Absent on Windows, nothing recorded either way. The weekly pass should turn each of these into 🔧 or ⛔. |
| — | Not applicable on Windows (a macOS platform notion with no Windows meaning). Listed so it is not re-asked. |

Two standing decisions cover many rows and are not repeated in full:

* **AI suite OFF** (`client/phi_features.h` `kPhiAiEnabled = false`,
  decided 2026-09-17, recorded in `windows-native-ui.md` §3.2 and §8).
  Everything the Mac puts on screen for Phi AI (Chat, Sidecar, browser
  memory, connectors, Sentinel, Phi Link, Private AI, AI new-tab page,
  Organize tabs with AI, site memory) is one switch away; the code paths
  were kept, not deleted. Rows say "⛔ AI suite".
* **Agent automation ON.** Agent Spaces, the skill relay, shadow windows,
  the control pipe and the credential broker are automation, not the AI
  suite, and are live on Windows.

## 1. Window, layouts and menus

| Feature | Mac | Windows | Status | Gap / decision |
|---|---|---|---|---|
| Layout modes | Balanced / Performance / Comfortable | Vertical tabs / Horizontal tabs | ◐ | Performance removed on purpose (`windows-native-ui.md` §4.3); a synced Performance peer reads as Vertical and is not written back. Wire names kept. |
| Menu bar | AppKit menu bar | ⋯ app menu (F10, Alt+F/E, solo Alt) | ⛔ | Windows-native redesign, `windows-native-ui.md` §3 and §8. Every Mac menu item has a mapped home (§3.2) except the rows marked below. |
| Full screen | ⌃⌘F | F11 row shown, never enabled | 🔧 | `IDC_FULLSCREEN` is not routed; the shell has to drive it (`phi_app_menu.cc`). |
| Window title follows the active tab (Space name while the placeholder / NTP is up) | Mac `9387a460`, `9068b449` | `PhiShellWindow::RetitleShell` on every rows rebuild and placeholder change | ✅ | Ported 2026-10-19, box-verified (page → tab title, Ctrl+T → Space name, close → back). A tab showing the native new-tab page counts as the Mac's placeholder. D29's agent-title cue ("Claude — Research") is still open. |
| Window ▸ Name window | yes | dropped | ⛔ | Never worked on Windows; `windows-native-ui.md` §8. |
| Edit ▸ Search the web | ⌥⌘F | dropped | ⛔ | Same, §8. |
| Hide / Hide Others | ⌘H / ⌥⌘H | — | — | macOS app model. |
| Share… (system share sheet) | File ▸ Share, site menu Share ▸, ⌃⌥S | none | 🔧 | `phi_shell_menus.h` notes the site menu's Share ▸ is "not ported yet". Windows has `IDataTransferManager`. |
| Capture Full Page / Capture Area… + screenshot annotation editor (Mac Oct 2026 series `fec4739a`…`c8c6ac28`; rebindable ⇧⌘1 / ⇧⌘2 `7e731204`) | File menu, `docs/page-screenshots.md` | none | 🔧 | PNG (not JPEG since `f7bc5b67`) via CDP `Page.captureScreenshot` with `captureBeyondViewport`, 16,384 px / 64 Mpx clip, 60 s budget, copied to the clipboard + Save toast; Capture Area… drags an overlay with wheel / edge autoscroll; then the browser-attached annotation editor (select, freehand, text, rectangle, ellipse, arrow, blur, crop; PNG / JPG / PDF export; undo). Two `kRebindableShortcuts` rows (Ctrl+Shift+1 / 2) come with it. |
| Copy URL / Copy URLs (⇧⌘C) | with toast | Ctrl+Shift+C, Copy link flash | ✅ | Only the address field's button brands chrome:// as phi://. `kCmdCopyUrl`, `PhiKioskWindow::CopyUrl` and `tab.copyUrl` copy the raw URL, where the Mac's copy paths brand it. |
| New Tab to the Right / New Tab Below | title follows layout | handler only, no row | ◐ | Corrected 2026-11-02: `kCmdNewTabToRight` has a handler and an agent refusal (`phi_app_menu.cc`), but no menu row invokes it and nothing reads `menuBar.newTabToTheRight`, although `windows-native-ui.md` §3.2 lists it on the tab context menu. No "below" wording in the vertical layout. |
| Tab ▸ Select tab 1–8 / last tab | ⌘1–⌘9 | none | ⛔ | Ctrl+1–9 switches Spaces on Windows (`windows-native-ui.md` §3.3); Chrome's tab-index chord was given up for it. |
| Zoom readout | live "100 %" | fixed row | ◐ | `windows-native-ui-impl.md` "What is left" §3.1. |
| What's new / Documentation | open pages | disabled placeholders | 🔧 | Two URLs; the rows exist. |
| Improve Translations for Phi Browser | Help row | none | 🔧 | A Help row opening Weblate; the README already points there. |
| Extension Info / Export Logs (⌥-only Help rows) | hidden rows | none | 🔧 | Decided 2026-10-19: implement as two Help rows (Export logs… zips the Phi + Chromium logs; Extension info lists versions). Send feedback keeps its own bundling. |
| *DEBUG* menu | debug builds | dev-hooks verbs | — | Developer tooling, different shape by design. |
| Check for update | Sparkle | WinSparkle + Update pill | ✅ | Update window is WinSparkle's own, not the Mac's custom sheet. |
| Uninstall Phi | Help row, removes Phi + Sentinel data | NSIS uninstaller with delete-data checkbox | ✅ | Different entry point (Windows Apps & features), same outcome. |
| Time Machine backups (policy snapshots + restore) | Help ▸ Time Machine Backups, `docs/time-machine.md` | none | 🔧 | Decided 2026-10-19: implement. A version-keyed snapshot of the user-data folder taken by policy before an update, with a restore entry point (Help ▸ Backups) that also reinstalls the matching build. Needs a Windows twin of `TimeMachineRollbackPolicy.json`. |
| Export / Import User Data (zip) | Help ▸ Manage User Data | disabled placeholders | 🔧 | `user-data-parity.md` S6 lists it as missing. |
| Import from another browser | Chrome / Safari / Arc / Dia / file | Chrome / Edge / Brave / Arc for Windows / file | ✅ | `browser-import-port.md` (2026-10-23). Safari and Dia are Mac-only; Edge and Brave are the Windows additions. Cookies are offered only for a source without an app-bound key (current Chrome and Edge seal cookies and passwords as `v20`, which no other app can read). Stays open naming the failed sources instead of closing. |
| Migrate from Another Browser (Arc / Dia / Zen Spaces wizard) | wizard rebuilds profiles + Spaces | none | 🔧 | Decided 2026-10-19: implement for the sources that exist on Windows (Zen; Arc only if a Windows install is found). Dia is Mac-only. Re-adds `browser_migration_finished` to `analytics.md`. |
| AppleScript suite | `Phi.sdef`, ~16 commands | — | — | Platform scripting. The control pipe (`tools/phi-browser-skill`) is the Windows automation surface and is a superset. |
| Deeplink `phi://native/openpage?page=settings…` | opens a Settings section | `phi://newtab` only | 🔧 | Small: a protocol handler registration plus the Settings route. |
| Kiosk global shortcut (⌥⌘N in any app) | Navigation setting | none | ⛔ | `phi_kiosk_window.h`: "not ported from the Mac (deliberately)". |
| Open external links in Kiosk | Navigation setting | row not drawn | 🔧 | Revisited 2026-10-27 (the ⛔ reason changed): Phi IS registered as the http / https handler (installer `RegisterBrowser`, Settings ▸ You "Set as default"). A link from another app reaches the running browser through Chromium's ProcessSingleton and stock `StartupBrowserCreator`, so Domain / URL rules already steer it, but the CLIENT never learns the open was external. Needs a framework hook on the forwarded command line (`IS_WIN_PHI_CLIENT`) before the Kiosk preference (and App rules below) can apply. `63c68d90` (restore the session before a windowless rule open) belongs to this path. |
| App URL rules: route external links by the SENDING app (Mac `48d4c4a6`, 2026-10-23) | `app:<bundle id>` rule kind, running-app picker | rules accepted, stored, synced and shown as "App" (retarget / ask / delete; not creatable); never matched | ◐ | Storage / sync / editor half ported 2026-10-27 (`UrlRuleKind::Refuses`, `SpaceSyncTable::inbound_acceptance_version` one-time replay, `PushRoutingTableNow` skips them). Matching stays Mac-only by decision: Windows cannot recover the launching app (only the command line crosses ProcessSingleton; the parent process is often a broker). |
| Return to the sending app when an external-link Kiosk closes (Mac `423c5a4e`, 2026-10-28) | sender captured at the open, re-activated on close unless focus left the Kiosk or the page moved to a Space | none | 🔧 | Waits on the external-link hook two rows up. Windows may be able to capture the foreground window before `PhiKioskWindow` takes it, which the Mac cannot (macOS has already raised Phi); untested. Whether z-order already returns activation on close is also unchecked and could shrink this. |

## 2. Sidebar, tabs, Spaces

| Feature | Mac | Windows | Status | Gap / decision |
|---|---|---|---|---|
| Docked sidebar: Space pips, pinned grid, bookmarks, tab tree, bottom bar | yes | yes, 200–480 dip | ✅ | `sidebar-architecture.md`. |
| Floating sidebar (edge hover) | yes | yes | ✅ | No fade on slide-in (impl doc §4.4). |
| Sidebar bottom bar: profile, chat, memory, downloads, feedback | all five | profile avatar + Downloads + Feedback pill | ✅ | Chat and memory are ⛔ AI suite. The profile button returned 2026-10-27 on request (left-most 24 dip avatar / person glyph, the Mac's menu: Download List → Library ▸ Downloads, Manage Bookmarks, Manage Extensions, New Tab, New Incognito Space / Window, Settings, Sign in when guest), reversing the 2026-09 decision in `windows-native-ui.md` §4.2. |
| Horizontal strip: Space chip, group chips, overflow, bookmarks bar | yes | yes | ✅ | Group headers in the horizontal layout: none on either side's strip beyond chips. |
| Tab groups (9 colours, collapse, overview) | yes | yes | ✅ | |
| Tab hover preview card | yes | yes | ✅ | |
| Open-tab indicators / unloaded (dimmed) indicators | yes | yes | ✅ | |
| Tab audio / muted indicator on rows and strip cells | yes (Mac `4e6de510` keeps it while muted) | none | 🔧 | No audio glyph on Windows tab rows; needs the framework's per-tab audible / muted state. |
| Ctrl/⌃+Tab MRU switcher | yes | yes | ✅ | |
| Tab Search (⇧⌘A / Ctrl+Shift+A) | open, pinned, bookmarks, recently closed | same | ✅ | |
| Multi-selection of tabs / bookmarks with batch menu | `TabMultiSelectionMenu` | strings imported, no selection model | 🔧 | `common.tabMultiSelectionContextMenu.*` keys exist in `strings.json`; nothing draws them. When it lands: the fork's batch `TabsProxy::CloseTabs` / `DuplicateTabs` (`4d9bceef9d9`, Mac `5787c25a`) need exporting through `phi_framework_api.h`, and the range anchor resets on an active-tab change (`17c0d54c`). |
| Pinned tabs: scope App / Profile / Space, split pins, rename, edit URL | yes | yes | ✅ | |
| Edit pinned tab icon | Edit… dialog has icon | Rename… / Edit URL… only | ◐ | No icon override on a pin. |
| Bookmark favicon actions: Separate From Bookmark / Back to Saved URL | row favicon menu | none | 🔧 | The "bookmark drifted from its saved URL" affordance. |
| Bookmark folder icon picker | yes | yes (24 symbols) | ✅ | |
| Close an open bookmark from its row (hover minus, Mac `7ec6e1b7`, 2026-10-27) | minus on hover while open | none | 🔧 | Tab rows have `close_box`; `ShortcutRow` has no close control, and `OnStoredRowClick` and `PhiBuildBookmarkRowMenu` have no close path, although `sidebar-architecture.md` §5.3 promises one. `SidebarClose`, which `pin.close` already uses, does the work. |
| Drag & drop: tabs, pins, bookmarks, groups, cross-window, tear-off | full | ghost / gap / ring / badge / snap tiles / New window | ◐ | Deferred mechanics D31–D34: edge auto-scroll, Ctrl/Shift modifiers, whole-group drag, bookmarks bar as source/target, cross-window slot, OLE file drops, pip lift, hidden-sidebar reveal during drag. |
| Drop a file onto the sidebar | yes | none | ◐ | Part of D31 (OLE drops). |
| Spaces: create (name, icon, profile), rename, icon/emoji picker, theme, change profile, delete, reorder | yes | yes | ✅ | Picker lacks "Recently used" and skin tones (D24); create form keeps old control styling (D25). |
| Space switch slide animation, Ctrl+n, hover card | yes | yes | ✅ | Hover card restyled neutral (no Space tint) with the Mac 2026-10-27 (`5588d798`); Windows keeps its agent status line and follows the app appearance (the Mac forces light). A new Space lands right after the source Space on both sides. |
| Incognito Space | ⌃⇧N | Ctrl+Alt+N | ✅ | `incognito-space-port.md`; not ported: account-menu entry, scripting command, pre-warmed tree, new-pip chip flight. |
| URL rules (Domain / contains / suffix / URL → Space / Incognito / Kiosk / Ask) | yes | yes | ✅ | |
| Pinned tab scope follows the synced setting (per-account follower, 2026-10-14 Mac `05160468`) | yes | Windows syncs `PhiPinnedTabScope` | ✅ | Generation fence + commit / landing fences ported 2026-10-19 (§9); 2026-10-27 `parked_for_scope` keeps dormant other-scope copies out of "Needs attention" (`4d6a7bf2`). No "not applied yet" status line: Windows migrates the store and writes the key in one call. |
| Library (overlay / window: Space cards, pins, bookmarks, Downloads, Folio) | View ▸ Library; rail Folio / Spaces / Downloads | ⋯ ▸ Library, Ctrl+Shift+L, Settings ▸ Spaces ▸ Manage Spaces in Library, downloads popover ▸ All Downloads, profile menu ▸ Download List; rail Spaces / Downloads | ◐ | Built 2026-10-27 (`windows-native-ui-impl.md` Round L): scrim + 36-dip card over sidebar and content, Space cards (theme ground, icon picker, inline rename, theme panel, pins grid + bookmark outline, double-click opens, handle drag reorders, "…" menu, row context menus with Move to Space ▸), inline create card after the current Space (`space_created surface: library`), Downloads list (search, grouped, hover actions), standalone window, `library_section_viewed`. Gaps: no Folio section (Folio is being redesigned — deliberately omitted 2026-10-27); no drag of pins / bookmarks between cards (Move to Space ▸ stands in); no profile filter in Downloads (the framework lists per window, 20 rows); no spring flight / shadow blur; rail and content grounds still follow the Space, where the Mac's went neutral (`294d4d02`). |
| Space theme palette editor: colour field, 1–3 linked handles, harmony (single / analogous / complementary / split), Light / Dark buttons (Mac `3d045cf6`, `4c9d919e`, `23c728d9`, 2026-10-21/23) | replaces the swatch row + saturation slider in the Space editor and the create form; 2–3 colours paint the sidebar as a vertical Oklab gradient | same editor (`phi_space_theme_panel.cc`: 260×296 popover, 244 field, 38/28/14 handles, harmony + Light/Dark toolbar), the create form's "Color ›" row on a draft, local `spaceThemePalettes` record (`model::Space::palette`, `PhiThemePalette`) | ◐ | Ported 2026-10-27 (`windows-native-ui-impl.md` Round J), box-verified on gmk: Space editor light/dark, draft editor from the create form, a split-complementary palette on the chrome, preset choice drops the record, pinned-preset Spaces unchanged. Record is LOCAL on both sides (not on the Space sync payload), so a Mac palette Space still renders here as its pinned preset. 2026-10-28: 2–3 colours now paint the sidebar as the Mac's vertical Oklab gradient down the window's full height (`PhiPalette::ground_stops`, `PhiOklabGradient`): the sidebar is still painted on the flat primary ground and the shell regrades each pixel onto the row's gradient colour by its coverage over that ground (`PhiBackdropRowGrounds`, acrylic and opaque frames, the floating sidebar, the create form), so washes, labels and the active row keep their look over the secondary colours; the top bar and accent stay primary. Settings ▸ Spaces now has the Mac's single "Color ›" row (gradient dot + chevron) opening the editor beside it, in place of the eight dots and the saturation slider; since `d414679` Appearance has the same row, with a link to Spaces when there are several. Minor: handles are not named for UIA (one field hit, no "Color n" tooltip); the guest→account migration does not carry palettes; drag cost not measured against the Mac's `a6e3a27a` (cached field, frame-coalesced preview). |
| Group Overview page | yes | yes | ✅ | |

## 3. Address bar and page

| Feature | Mac | Windows | Status | Gap / decision |
|---|---|---|---|---|
| Omnibox with suggestions, keyword chip ("Tab to search") | yes | yes | ✅ | Under the Omnibox new-tab behaviour an EMPTY window (Ctrl+N, last tab closed) shows the placeholder with the field focused instead of a new-tab tab, as on the Mac (`936d090e`, ported 2026-10-27, fork + client). Since 2026-10-28 the Mac's centred floating omnibox exists too (`phi_floating_omnibox.{h,cc}`, a `PhiPopover` whose field joins `PhiOmniboxSession`): under the Omnibox new-tab behaviour Ctrl+T, the New Tab rows and an empty window open it over the page, suggestions drop flush under it, the keyword chip works in it, a commit opens a new tab (or the empty window's first); Escape, a click outside, or the user's New Tab / Close Tab chord close it. The empty window's placeholder under it is blank (the native new-tab page mounts with its controls hidden, `NativePagesBlankPlaceholder`), and switching to a Space whose window is already empty opens the card too (the Mac's `emptyLiveSpaceOpensOmniBox`). A window that is not in front when it empties focuses the address field instead and does not open the card later (the Mac owes it until the window is presented). The Mac went blank for new windows and empty Spaces on 2026-11-02 (`0c4f1552`) but still shows its game after a last-tab close; Windows is blank in all three. |
| "Hold ⌘ to switch" to an already-open tab | yes | "Hold Ctrl to switch →" | ✅ | Ported 2026-10-28 with the rest of the Mac `OmniBoxViewModel` / `OmniBoxTextField` logic (fork `OpenOmniboxMatch` takes an explicit disposition, `DeleteOmniboxMatch`, `StartOmniboxQuery(prevent_inline)`, `swap` in the results json; client `phi_omnibox_session.{h,cc}`): a row with an open tab opens in the current tab unless Ctrl is held (Enter or click), where it used to switch silently; inline autocompletion of the default row (selected, typed over, dropped on Backspace / Delete); the default row armed when the rows answer the typed text, an arrowed pick kept across streamed updates, stale result sets dropped; Shift+Delete and a hover × forget deletable rows; history rows show their favicon; Chromium's swapped rows, pedals and empty rows filtered; chrome:// reads phi:// in the field and the rows; the keyword chip wears the seeded engines' brand colours. Not ported: a bookmark / pinned row's tab opening address-bar submissions in a NEW tab (`3d3719b1`). |
| Site menu: security row, Content blocking on this site, Always show URL path, Site settings, Clear cache / cookie, Extensions ▸, Share ▸ | yes | all but Share ▸ and Extensions ▸ | ◐ | Share: see §1. Extensions live in the field's puzzle button instead (design). |
| Site menu: Collect Site Memories / Delete Site Memories | yes | none | ⛔ | AI suite (site memory is browser memory). |
| Copy link in the field, Reader button (hover) | yes | yes | ✅ | Copies the presented Peek's URL (the Mac's `d94d3329`), through `RebuildNavState` and `nav_url_`. |
| Reader View (typography prefs) | yes | yes + read-aloud HUD | ✅ | Windows has read-aloud; Mac does not. `-phi-auto-reader` automation entry point not ported (analytics doc). |
| Peek (Shift-click, auto-peek from pins/bookmarks) | chips: Close, Open as Tab, Open as Split, Reader View | chips: Close, Open as Tab, Open as Split | ◐ | No Reader chip on the Peek card; Peek is omitted from session restore (`analytics.md`). Page commands and the address field aim at the presented Peek since 2026-10-27 (`a19624bb`, `c11849cf`; Back / Forward and the Reader toggle stay on the opener). The Shift-click SPA-router guard + opener video pause landed 2026-10-27 (`80f92213`: fork `phi_peek_link_guard.{h,cc}` runs the Mac's capture-phase click guard in a Phi isolated world of every document, gated on `peekViewEnabled`; `OpenUrlAsPeek` pauses the opener's videos in every frame). Still missing: appear / close flights and the opener dim. The Mac turned its guard and the video pause OFF on 2026-10-27 (`3a6cbf2e`, for the CDP session it retained per tab; still off at `8c0b8596`). Windows keeps its version, which holds no DevTools session, so Windows does more than the Mac here until the Mac re-enables it. |
| Split view (two panes, drop hints, swap, convert, pin/bookmark a split) | yes | yes | ✅ | `split-view-chrome.md`. |
| Split with New Tab (Tab menu, rebindable shortcut, omnibox pane picker, Hold to add an open tab; Mac `6985cc34`, 2026-10-28) | yes | row-menu "Open as Split" only | 🔧 | A ⋯ row, a `kRebindableShortcuts` row and a split-pane mode in `PhiOmniboxSession`. The Mac's Ctrl+Shift+= is Chromium's zoom-in twin on Windows, so it needs another chord or none. The Mac previews a blank pane before a real split exists, which has no twin under a hosted HWND, so that needs a decision. |
| Modifier clicks: Shift → Peek, Alt/⌥ → split, Ctrl+Alt/⌘⌥ → Kiosk | yes | yes | ✅ | |
| Kiosk window ("Open in" Space, profile chip) | yes, profile chip menu, cursor-zoom open animation, frame autosave | yes, no profile switch from the chip, default size | ◐ | `phi_kiosk_window.h` lists the four pieces as deliberately not ported; the profile-chip switch has no framework call. Also open, by code reading and not reproduced: the page does not take keyboard focus on open (`79b11061`). "Open in" reopens the page by URL rather than moving the live tab, and the Kiosk closes only once the page has opened (fixed 2026-11-02: with no user window it used to lose the page and exit Chromium, `74cb2bce`). With no user window the last session is restored first when that setting is on, as on the Mac. |
| Native new-tab page | Phi mark + omnibox, AI suggestions | Phi mark + omnibox | ✅ | AI suggestions ⛔ AI suite. |
| One default surface for the address row, active strip tab, page backing and split gaps (Mac `5e434655`, 2026-10-28) | `extensionSurfaceBackground`: white, or in dark the overlay colour at 20 % | `window_bg` (the theme's own colour in dark) beside `card` #2B2B2B on the new-tab page | ◐ | Read from code, no screenshot: a dark-mode seam between the address row and the new-tab page is expected. The fix is one dark `window_bg` in `ResolveFromColors`. The value is a design call, because `windows-native-ui.md` §2.3 argues for #2B2B2B over the Mac's near-black. |
| Bookmark manager (native, per Space) | yes | yes | ✅ | |
| Bookmarks bar (always / on new tab) | yes | yes | ✅ | |
| Image preview window (zoom, export original — Mac `1705cb23`, 2026-10-15) | `UserInterface/ImagePreview` | none | 🔧 | Whole surface absent. New on the Mac this cycle. |
| Picture-in-picture: auto Off / Normal / Park at edge | yes | yes | ✅ | `picture-in-picture.md`. |
| Widevine DRM (Netflix, Spotify, Disney+ …) | fork only: component-updated CDM, `Phi Helper (CDM)` signed without library validation, VMP host files (`Phi Framework` + castLabs EVS `.sig`) | component-updated CDM; VMP host files `Phi.exe` + `framework\chrome.dll`, `.sig` beside each from `make-installer.sh --vmp` (EVS, after Authenticode) | ◐ | Ported 2026-10-30 (fork `cdm_host_file_path.cc` + `media_options.gni`, ship args `enable_widevine`), nuc-verified by `box-tests/suite-widevine.mjs` 4/4: CDM 4.10.3112.0 fetched by the component updater, license request, Shaka Widevine playback, host verification ran. No CDM helper needed (a macOS library-validation matter). Gap: no `.sig` ships yet — it needs a castLabs EVS account with Widevine 3PL access covering the Windows build; until then the host is unverified and services that demand VMP refuse a license. |
| Sidebar Mini Player / native media controls | play/pause, ±10 s, tracks, volume, PiP, source switching; setting Off / Auto / compact / expanded | none | 🔧 | `docs/native-media-controls.md`. The Mac shipped nine Mini Player fixes 2026-10-13/14, so it is an active surface. Needs the framework's MediaSession bridge on Windows. |
| Shorten links to selected text (Copy Link to Highlight) | Advanced setting | row not drawn | ⛔ | The copy callback reaches only the Mac bridge (`phi_settings_window.cc`, `analytics.md`). |
| Apply theme to text selection on web pages | General setting | synced, nothing reads it | ⛔ | No web-selection tint on Windows (`phi_settings_window.cc`). |
| Extension popups / side panel / dialogs | native hosts | Chromium popovers | ✅ | `chromium-popovers.md`. |
| Extensions popover (pin/unpin), extension action semantics | yes | yes | ✅ | |
| Downloads: living list, pause/resume/retry, keep/discard, All Downloads | yes | yes | ✅ | |
| Toasts (URL copied, saved to Folio + Undo) | yes | yes | ✅ | |

## 4. Settings

| Pane / row | Mac | Windows | Status | Gap / decision |
|---|---|---|---|---|
| Settings window shape | fixed toolbar of panes | Windows Settings-style rail with search | ⛔ | Redesign, `windows-native-ui.md` §7. Rows below are compared, not the chrome. |
| You / Account: profile card, randomize, change name, avatar, sign in/out, default browser, share card | yes | all but Randomize | ◐ | The Randomize button under the card is new on the Mac (`92816ab6`, 2026-10-27). |
| Account: Invitation code + View details | yes | none | 🔧 | `login-port.md`: backend work; blocked until the Windows ring has the invitation API. |
| Account: Export account data (email code flow) | yes | none | 🔧 | |
| Account: Delete account (email code, sign out, local wipe) | yes | none | 🔧 | `user-data-parity.md` S6 also lists the missing account-deletion fence. When built, bind the device-key removal to the subject captured before the request (Mac `a6cf9a15`); `<account_dir>\sync\device_key.dat` goes with an account-dir wipe. |
| General: language, new-tab behaviour, restore last session, analytics consent, Folio folder + auto-save + Choose sites | yes | yes | ✅ | |
| Appearance: layout, colour appearance, Space colour (palette editor), tab toggles | yes (split across General/Advanced) | yes | ✅ | The saturation slider is gone on both sides (Mac `62986748`, Windows `d414679`). With several Spaces Windows shows a hint and a link to Spaces where the Mac greys the row. |
| Spaces: list, expander editor, URL rules, pinned tab scope, shortcut | yes | yes | ✅ | No tab count in the expander hint (impl doc "What is left" §7). |
| Profiles: list, search engine, download location, Folio folder, data links | yes | yes | ✅ | |
| Profiles: Content blocking (Block ads / trackers / cookie banners, rule-set chooser, custom filters) | per profile, `docs/content-blocking.md` | per-site toggle only | 🔧 | The framework owns the engine, so the Windows half is the switches, the chooser sheet and the download/update actions. `analytics.md` already notes the missing snapshot keys. |
| Navigation: Kiosk toggles, Peek toggles, URL rules | three Kiosk toggles | one Kiosk toggle | ◐ | The two missing Kiosk toggles are ⛔ (see §1). |
| Phi & AI pane | yes | built, not mounted | ⛔ | AI suite. |
| Phi Link (Telegram relay) | yes | none | ⛔ | AI suite (Sentinel relay). |
| Sync: status, set up, devices approve/deny, remove device, reset sync from the last device | yes | yes | ✅ | `user-data-parity.md`. Reset (`055df916`: `PUT /keys/v1/reset-operations/<uuid>`, journaled in `marker.json`, resumed by "Set up sync again") ported 2026-10-27 as the Settings Removal row; the setup window's remove / reset offer is not built (Windows has no remove control there). |
| Shortcuts: grouped table, search, record, reset (all + per row), unassign, conflicts, Go to Space rows, effective chords in menus | yes (shortcut engine `896b36d0`, Oct 2026) | yes | ✅ | 2026-10-27: localized titles, Reset All behind a confirmation, Delete-to-unassign + per-row Reset, Chromium page commands rebindable, ⋯ menu shows effective chords, displaced default chords swallowed. Deliberately not ported: informational conflicts / filters / system-hotkey warnings (Windows keeps confirm-and-steal), `excludedKeys`, Kiosk global hotkey (⛔). Kiosk & Peek group rows absent; the numpad zoom twins stay fixed and unlisted. |
| Advanced: restore session, full URL, previews, indicators, shorten links, Mini Player, auto PiP, developer mode, Chromium settings | yes | all but Shorten links (⛔) and Mini Player (🔧) | ◐ | Rows regrouped into Appearance / General on Windows. |
| Developer: Allow agents (CDP), per-agent Allowed / Blocked lists, "Allow all agents" | yes | one switch | ⛔ | No code-signature peer identity on the Windows pipe (`phi_settings_window.cc`, `phi_control_server.cc`). Revisit if the pipe gains a caller identity. |
| Developer: Allow agents to operate Spaces, operated-page rendering, profiles agents can use, install skill | yes | yes (no "exact rendering" toggle) | ◐ | The operated-page mask toggle has no Windows row. |
| Developer: Agent Password Manager (Bitwarden sign in / unlock / timeout), Agent Credential Approvals (standing grants, revoke) | yes, `docs/bitwarden-password-manager.md` | helper process + `credentials.*` verbs + Allow once / 10 min prompt; no Settings rows | ◐ | `PhiBitwardenHelper.exe` and the broker are in; the sign-in sheet, session settings, approvals list and "Always" grants are not. `analytics.md` records `approval_type: always` and `kind: run` as not ported. |
| About page | About window | About page + notice | ✅ | |

## 5. Onboarding, account, data

| Feature | Mac | Windows | Status | Gap / decision |
|---|---|---|---|---|
| Onboarding: welcome, name, sign in / guest, consent, layout, next steps | yes | yes, the earlier design | ◐ | `login-port.md`. The Mac's 2026-10-26 Welcome redesign is the membership card row below. Missing since the Mac's 2026-10-28 refresh (`28c0618b`): Back on every later page, "Sign in instead" on the guest page, the legal wording (Terms of Use first), arrow keys on the layout page, the email-derived name prefill (`1cf61180`), Omnibox as the guest default (`5196706b`). Undecided: the footer pills and the guest feature list, whose copy names AI features that are off here. |
| Onboarding keyboard and screen reader (Mac `8d998ebc`, 2026-10-30) | VoiceOver reads page titles, steps and a card summary, and hears page changes | controls are UIA hits; titles, guest bullets and next steps are paint only; no page-change announcement | ◐ | By code reading, no Narrator run. The consent box cannot be ticked from the keyboard (`OnKey` has no Space and drawn hits take no Tab focus), and Enter stays disabled until it is ticked, so the two consent pages cannot be finished keyboard-only. The consent row's UIA name is the raw "I agree to the {0} and {1}" pattern. The name EDIT has no accessible name and the Settings card is invisible to UIA. |
| Onboarding: browser import step | Chrome / Safari / Arc / Dia / file | "Import browser data" link under Next steps | ◐ | A dedicated step with source picker is missing; see §1 import row. |
| Onboarding: password manager choice (iCloud Passwords) | yes | skipped | — | iCloud Passwords is Mac-only (`phi_onboarding_window.cc`). A Windows equivalent (Windows Hello / Credential Manager) would be a new decision. |
| Guest mode + guest → account migration | yes | built, not live-tested | ◐ | `user-data-parity.md` S6. |
| Hosted login (Auth0), reauthentication, sign out | yes | yes, off-the-record sign-in window | ✅ | |
| Sync keys: recovery code, device approval, profile / Space matching, sync method (Merge or Use your account's data, Mac `40312627`) | yes | profiles page only | ◐ | Corrected 2026-10-27: the Windows pairing wizard has no Spaces step and no method / replace step (`user-data-parity.md` S5 "Not yet"). No wire change — a Windows peer sees a Mac join as before. Also absent: the recovery-code confirmation page and its durable pending flag (Mac `32799547`, 2026-09-24, not logged here before), so a Windows first device can finish setup without proving the code was saved, and that page's Back link (`a8a8f23e`). |
| Crash feedback ("Phi quit unexpectedly") on next launch | `docs/crash-feedback.md` | Crashpad → Sentry, no prompt | 🔧 | Needs the last-run status from Crashpad and the existing Send feedback form with the crash id. |
| Send feedback: files, pasted images, diagnostics, outbox | 10 × 20 MiB, Sentinel logs | 5 × 10 MB, week-long outbox | ✅ | Sentinel logs are ⛔ AI suite. |
| Account deletion local data removal | yes | none | 🔧 | With the Delete account row above. |
| Membership card: holographic foil card with member ID (SHA-256(sub)[0:8]), "Date Joined", random style + PNG export, on the onboarding Welcome page and Settings ▸ You (Mac `6b300dc0`, `cda12eaf`, 2026-10-20/26) | `HolographicCardView` | `PhiHolographicCard` (`client/phi_holographic_card.cc`): the same layer stack composed in software — palette base + iridescence + radial, one of 6 procedural foil textures, seeded grain, logo + sweep, one of 6 seal ornaments, sheen, edge; pointer light + tilt, entrance spin, flat PNG export at 2× on the accent canvas | ◐ | Ported 2026-10-27 (`windows-native-ui-impl.md` Round K); the Mac has moved since, see the end of this row. Foil / grain / seal masks are rasterised once per style and size with GDI+ on a worker and cached as PNG under `<user data>\PhiCardArtwork\v1`; the light-driven layers recompose per frame. Member id from `auth().user().sub`, join month from the profile's `created_at`, style in the account defaults under the Mac's `profileCardStyle` key (palette overridden by the current theme in Settings, which the Mac has dropped). The conch frame sets (12.5 MB) are gone. Not ported: the Mac's `.random()` seeding details (both are uniform), IvyPresto Headline Light now ships for the name. Mac 2026-10-27/29, not here yet: the card no longer follows the theme in Settings (`40ede7a7`); no member ID printed (`1cf61180`); no radial highlight, a logo sweep that fades in with tilt and no shadow in Settings (`ba180400`); a randomized style saved at once (`8fb61ef5`). |

## 6. Agents and automation

| Feature | Mac | Windows | Status | Gap / decision |
|---|---|---|---|---|
| Agent Spaces: overlay, fog, ring, pill (Take control / Hand back / Finish), "The agent needs you", Autoview | yes | yes | ✅ | Agent cues D26–D30 deferred: brand accent per agent, pulsing ring, window title, ⋯ on the capsule, omnibox dim/eye. |
| Agent viewport follows the agent Space's own page area (Mac `1c39d48c`, 2026-10-29) | `agentSpace.panelSize` answers for the aimed Space, re-measured before each action | same; `VerbAgentPanelSize` answers in CSS pixels for the shell hosting the watched Space, else the focused user window, else the parked agent shell | ✅ | Ported 2026-11-02, box-verified on nuc at 300 % and 150 %: an agent tab matches a real tab's size hidden and watched, and follows sidebar and window changes at the next action. Before, the skill's un-aimed call was refused (`scope_not_allowed`), so agent pages laid out at the parked shell's fixed frame (1027×684 at 300 %, 1386×849 at 150 %) whatever the user's window was. The skill names the task, not the Mac's `windowId`, which resolves to the user's shell while watched. |
| Agent Transcript console | View menu, pip "Show Transcript" | built, `kPhiTranscriptEnabled = false` | ⛔ | Decided off 2026-09-09 (`phi_features.h`). |
| Agent approval (Allow Once / Always / Deny per agent) | yes | one admission switch | ⛔ | See Developer row in §4. |
| Companion apps (Folio.app): signed admission past the agent switch, an allowlisted channel, `pageActions.*` rows in File and the page context menu, built-in Folio stepping aside (Mac `6a54718b`, `dc18d2c5`, fork `03d1b24aa3b`, 2026-10-30) | yes, `docs/companion-apps.md` | none. The fork half is compiled but dormant: nothing fills `PhiPageActionMenuRegistry`, so no row shows and the `PhiBridgeWrapper::InvokePageAction` stub is unreachable | ⛔ | Decided 2026-11-02: not needed for now. Reopen when a Windows companion app exists. Admission would need its own decision first: the pipe has no peer identity (decided 2026-09-08), and the Mac's check rests on kernel facts Windows lacks (live code signature, responsible process), so an Authenticode check of the image on disk proves less. |
| Shadow windows, cursor / effects, captureWindow, reader verbs, user messages | yes | yes | ✅ | |
| User-Space verbs: spaces, profiles, bookmarks, pins, groups, split, URL rules, downloads | yes | yes, plus window / menu / layout / theme / settings / feedback / account / onboarding / sync families | ✅ | Windows is a superset. |
| `agentSpace.app.info` (which Phi build is answering) | `docs/agent-app-info.md` | `capabilities` + `ping`, no `app.info` | 🔧 | One verb; the skill should see the same reply shape on both platforms. |
| Notification cards (`notification.show`, Run, paging) | yes, with Mute / Pop up menu | yes | ◐ | The Mute / Pop up presentation menu is deliberately absent (`analytics.md`). No agent avatar on Windows cards (the Mac loads `/api/v1/files/…` avatars through the agent transport, `d830a327`). Working cards, revoke and conversation cards (`05ea7e55`) come from the Sidecar's extension envelope, so they are ⛔ AI suite; `notification.show` stays action-only on both sides. |
| Credentials: `credentials.status/get/getTotp/autofill` | Bitwarden | Bitwarden helper | ✅ | UI gaps in §4. |
| Organize tabs with AI (⌃⇧G) | yes | none | ⛔ | AI suite. |
| Sidecar Travel Back, Live Mode setting (Mac `a6f31c0a`, 2026-10-15) | yes | none | ⛔ | AI suite. |
| Service broker for bundled extensions | yes | none | ⛔ | AI suite (Sentinel). |
| Sentinel helper, watchdog, Phi Chat hotkey | yes | none | ⛔ | AI suite. |
| Folio: Save to Folio, Open Folio, auto-save on site actions, Add Note | yes | yes | ✅ | `folio-port.md`. |
| Folio library | Mirage `library.html` — the Mac DELETED its native library 2026-10-19 (`45a10144`; `docs/folio-native-library.md` now says the extension page is the only Folio UI) | Mirage `library.html` (`PhiFolio::OpenLibrary`) | ✅ | The 2026-10-19 "implement the native library" decision is void: both sides open `library.html`. The Library surface's Folio section (§2) hosts the same page. |

## 7. Summary (2026-11-02)

| Status | Rows |
|---|---|
| ✅ ported | 52 |
| ◐ partial | 26 |
| 🔧 to implement | 26 |
| ⛔ deliberately left | 19 |
| ❓ needs a decision | 0 |
| — not applicable | 4 |

**The 🔧 list, roughly by user impact:** Mini Player / media controls ·
content blocking switches · multi-selection · Bitwarden Settings rows and standing grants ·
Time Machine-style snapshots + restore · crash feedback prompt · image
preview · Arc/Zen migration wizard · Share · Capture Full Page ·
Export/Import user data · account export + deletion · invitation code
(backend) · Split with New Tab · full screen · client-side
external-link hook (Kiosk preference, App rules, return to the sending
app) · `agentSpace.app.info` · tab audio indicator ·
bookmark Separate/Back-to-saved · close an open bookmark from its row ·
What's new / Documentation / Improve
translations rows · Export logs / Extension info rows · settings deeplink.

**Decided 2026-10-19:** the five rows that were ❓ in the first pass
(Library, Time Machine snapshots, native Folio library, Arc/Zen wizard,
Export Logs / Extension Info) are all to be implemented. The native Folio
library row closed itself 2026-10-27: the Mac deleted its native library
and both sides open Mirage's `library.html`.

**Decided 2026-10-27, both new Mac design work this cycle, both L:**
the Space theme palette editor (§2) and the holographic membership card
(§5). Each was a visual redesign of a surface Windows already had; both
were implemented for parity the same day (§2 row now ◐, Round J; §5 row
now ✅, Round K). No ❓ rows remain. Also on request 2026-10-27: the
sidebar profile button (§2, ✅, reversing the 2026-09 decision) and the
Library surface (§2, ◐ — Spaces + Downloads, no Folio while Folio is being
redesigned).

**Decided 2026-11-02:** companion apps (§6) are not needed on Windows for
now. The row is ⛔ until a Windows companion app exists. This pass also
corrected the summary: the previous counts were one ✅ short and one 🔧 over,
because "Hold to switch" went ✅ on 2026-10-28 without the table moving.

## 8. The weekly check

Run from the repo root with a Mac checkout beside it (or `PHI_MAC_SRC`):

```
tools/mac-parity-check.sh
```

```
tools/mac-parity-check.sh --table
```

The first form reads the "Checked against phibrowser-mac `<hash>`" line
above and prints every Mac commit since, grouped `feat` → `fix/improve` →
`docs` → other, each with the user-facing areas it touched (UserInterface
directory, States, Preferences, docs, strings) and the new
`Localizable.xcstrings` keys. The second prints the same commits as an
empty markdown table for §9.

**Every Mac commit gets a verdict**, written into §9 the same week. The
subject is not enough: read the diff, find the Windows twin of the code it
touches, and decide:

| Verdict | Means | The row says |
|---|---|---|
| **port** | Windows shares the surface or behaviour and diverges after this commit | what to change and in which Windows file, and how urgent |
| **skip** | Mac-only plumbing, a macOS platform notion, or a ⛔ feature | the one-line reason |
| **watch** | the behaviour belongs to a 🔧 or ◐ row that Windows has not built yet | the row it waits on; it is picked up when that row lands |
| **done** | Windows already behaves this way (or ported it earlier) | where |

Then:

1. **Every `feat`** gets a feature row in §1–§6, or changes an existing
   row's status, in addition to its verdict.
2. **A `fix` matters** only when the Windows port shares the behaviour
   (Mini Player fixes do not, until the Mini Player exists; omnibox and
   sync fixes usually do). A shared fix that Windows lacks is a **port**.
3. **New strings** that do not belong to an existing row usually mean a
   new surface; find the commit and add the row. New keys for a ported
   surface mean regenerating `client/resources/strings.json`
   (`tools/gen-strings-catalog.py`, `docs/i18n.md`).
4. **Turn ❓ into 🔧 or ⛔** when a decision is made; a ⛔ names the date
   and where the decision lives (a doc section, a code comment).
5. Update §7's counts, bump the "Checked against" hashes and the date,
   and keep `analytics.md`'s "Not ported" table in step when an event's
   feature changes status.

The **port** verdicts are the week's recommendation list; they are
proposals until someone picks them up. The Windows side is dated too:
when a 🔧 or ◐ row lands, change its mark in the same change that lands
it, pointing at the owning doc.

## 9. Triage log

One table per pass, newest first. Commits are in Mac history order.

### 2026-11-02 — phibrowser-mac `f7d850a3` → `8c0b8596` (66 commits; release/2.15.0 merged into main and dev)

**Port recommendations this pass (21 commits in 12 ports; the second and the
page-loss half of the first have been done). The other Windows findings are
from reading code at `098dcb3` and were not built or run on a box, so their bug
claims still need reproducing. In order of user impact:**

1. `74cb2bce` + `ea04bb00` + `2028f868` **windowless opens**. Closing the last
   shell leaves a Kiosk or agent window alive with the process up. "Open in" a
   Space from that Kiosk then lost the page: `RouteUrlToSpace` returned with no
   slot and `PhiKioskWindow::OpenInSpace` closed the Kiosk anyway, which exited
   Chromium when it was the last Browser. **Page loss reproduced and fixed
   2026-11-02**: a request with no slot gets a shell of its own, a parked Space
   rebuilds its window, and the Kiosk closes only once the page has opened
   (`PendingCreation::closing_kiosks`). Verified on nuc: no user window, a double
   click, a live Space, a cold Space, a parked Space. **Restore gate done
   2026-11-02**: `RunAfterWindowlessRestore` restores the last session first
   when that setting is on and holds the open until it settles or its rescue
   shell lands (15 s backstop), for the Kiosk move and an `activate` agent open.
   `windowOpened` is true only for a fresh window. Routed opens now rebuild a
   parked Space's window instead of orphaning its tabs (found on the box).
   Verified on nuc: Kiosk move and agent open with the setting on, Kiosk move
   with it off.
2. `1c39d48c` **agent viewport**. **Done 2026-11-02, box-verified on nuc at
   300 % and 150 %.** Code reading first said the viewport came out 1.5× too
   large; the box showed the skill's un-aimed `agentSpace.panelSize` was
   refused, so it never saw the physical-pixel answer and stayed at the parked
   shell's fixed frame, following no sidebar or window change.
3. `79b11061` **Kiosk page focus on open**. Keys like Space may do nothing until
   a click, on every Kiosk open. S; check on a box first (Ctrl+Alt-click a link,
   press Space).
4. `8d998ebc` **onboarding keyboard and screen reader**. The consent pages
   cannot be finished keyboard-only, and titles and steps are never read. M.
5. `40ede7a7` + `92816ab6` + `ba180400` + `1cf61180` + `8fb61ef5` **membership
   card follow-ups**: style independent of the theme in Settings, Randomize in
   Settings, lighting, no printed ID, email-derived names, randomize saved at
   once. S each, best done together.
6. `5196706b` **guests default to the Omnibox new-tab behaviour**. One line in
   `OnboardingContinueAsGuest`. S.
7. `28c0618b` + `0d790e04` + `c87fde81` **onboarding refresh**. Back, "Sign in
   instead", legal wording and the catalog regen are M. The pills and guest
   feature list are a design decision, and the list's AI copy would be false
   with the AI suite off.
8. `5e434655` + `294d4d02` **neutral default surface**. S once the dark value is
   chosen: the Mac's near-black or the design's #2B2B2B.
9. `6985cc34` **Split with New Tab**. M; New Tab Page mode alone is S. Needs a
   chord and a decision on the blank-pane preview.
10. `7ec6e1b7` **close an open bookmark from its row**. Windows has no close
    control there at all, so this is more than the Mac's icon swap. S.
11. `8d76a688` **even out the pinned band's gaps** (Windows 16 dip, Mac 8 pt). S,
    but `windows-native-ui.md` may intend the 16, so it needs a design call and
    may become a skip.
12. `a6e3a27a` **palette editor responsiveness**. The same hazards exist (a
    synchronous field render, `ApplyPalette` per mouse move). S, low urgency;
    measure on a box first, the field is half the Mac's size at 100 %.

| Mac commit | Subject | Verdict | Windows action / reason |
|---|---|---|---|
| `3a6cbf2e` | fix: temporarily disable Peek CDP link guard and video pausing | skip | Keep Windows' guard. The Mac's reason is a retained CDP session per tab; fork `PhiPeekLinkGuard` holds none and only runs a script in an isolated world on each committed document (residual cost per frame unmeasured). The Mac has not re-enabled it at `8c0b8596`, so Windows does more than the Mac here (§3 Peek row). |
| `05ea7e55` | feat: add working notification cards, card revoke, and opening card conversations in the AI Chat panel | skip | ⛔ AI suite: all three arrive through the Sidecar's `chrome.phinomenonPrivate` envelope, and Mac `notification.show` stays action-only. `phi_notification_cards` serves `notification.show` only. |
| `92816ab6` | feat: add profile card randomization in account settings | port | S. `phi_settings_window.cc` `BuildAccount`: a dice under the card and a `randomizecard` action (`PhiHolographicCardStyle::Random`, `SetProfileCardStyle`, `PhiNotice` on failure). The dice paths move out of `phi_onboarding_window.cc`. Skip its theme switch, which `40ede7a7` removes. New keys `settings.account.card.*`. |
| `62986748` | feat: replace settings theme presets with palette editor entry | done | `BuildAppearance` and `BuildSpaceEditor` have the single `spacecolor` row since `d414679`. Minor difference on the §4 Appearance row. |
| `ba180400` | improve: refine profile card layout and lighting | port | S. `phi_holographic_card.cc`: drop the radial block in `ComposeFace`, scale the logo sweep alpha by the light's distance so the logo is dark at rest and in the export, add a no-shadow option for Settings. The layout half is already there. |
| `a6cf9a15`, `4b8b4a56` | Remove the deleted account's sync device key during account deletion (PHI-1798) | watch | Delete account 🔧 (§4) and local data removal 🔧 (§5). Same patch twice. Keep the binding to the subject captured before the request. |
| `9070b049` | Revert "feat: open Phi Chat's Private AI settings from the AI settings row" | skip | ⛔ AI suite. |
| `ec8a2f1e`, `ad2f32de` | fix: drop the timed agent-mode re-assert on Space switch now that Chromium holds the agent tab visible | done | Same patch twice. The client has no timed re-assert; the hidden shell stays shown off-screen, and the Chromium-side hold (fork `3f57767fbc8`) is compiled for both clients. |
| `a5094c62` | Merge pull request #251 (revert Private AI) | skip | Clean merge of `9070b049`. |
| `0ed3b4af` | fix: drop stale AI Chat collapse mirrors that looped on panel reopen | skip | ⛔ AI Chat. |
| `12d27def` | fix: pin the agent control pill's badge divider to 1pt | skip | NSBox / stack-view slack. The Windows divider is a painted rule and the pill's width is not content-driven (`phi_agent_overlay.cc`). |
| `7ec6e1b7` | fix: use minus icon to close open sidebar bookmarks | port | S. Add `close_box` to `ShortcutRow`, placed in the bookmark loop of `phi_sidebar_view.cc`, shown when open and hovered; hit in `OnStoredRowClick` before `ArmDrag`, calling `SidebarClose`, refused under `agent_holds_input_`; a UIA button. `phi_fluent.h` has no minus glyph yet. New §2 row. |
| `5787c25a` | feat: batch tab updates for smoother closing and duplication | watch | Multi-selection 🔧 (§2). Fork `4d9bceef9d9` compiles for both clients but is not exported through `phi_framework_api.h`. `tab.closeOthers` keeps its loop, because `ScheduleRefresh` coalesces where the Mac rebuilt per tab. |
| `8d3893d2` | fix: blend sticky sidebar New Tab row into themed backgrounds | done | `layout.new_tab_row` is fixed and unfilled over the regrade, and rows clip to `tabs_top`, so nothing sits under it. Not checked on screen. |
| `6f6a8b8e` | fix: use neutral search panel backgrounds and align cell colors | done | `palette.card` is neutral in `PhiSearchTabsPanel` and `PaintPopup`, and both lists select with `hover_strong`. The Mac's solid accent selection with white text is not on Windows; no doc records that as a decision, so a change there would be a new request. |
| `40ede7a7` | fix: decouple profile card style from browser theme | port | S. `phi_settings_window.cc`: delete the `PaletteForTheme` override in the `kProfileCard` paint, and compute the `downloadcard` canvas from the style's palette (needs an accessor on the palette table in `phi_holographic_card.cc`). Onboarding stays theme-linked, as on the Mac. |
| `fdb65dbc` | fix: reduce bookmark sidebar scrolling overhead | skip | NSOutlineView row reuse and Combine replays; a Windows wheel notch only offsets and schedules a rebuild. Not profiled. |
| `294d4d02` | fix: use neutral content backgrounds and loading backdrop | port | S, with `5e434655`. `phi_library.cc`: `ContentGround` drops the accent blend and `OverlayFor` base; `CardGround` stops using `ground_top`. Open: Mac `windowBackground` reads #EAEAEA / #252525 in `DefaultColors.swift` but #FFFFFF / #212121 in the Theme README, so the rail's target value needs checking. The loading-backdrop half is superseded by `5e434655`. |
| `b1334046` | fix: move sidebar extension shelf closer to the address bar | skip | Superseded by `9f5b887f` and `8d76a688`. Windows has no sidebar shelf; pinned extensions sit in the address field and tool cluster. |
| `5e434655` | fix: unify address bar and web content background colors | port | S plus a design call. `phi_theme.cc` `ResolveFromColors`: one dark `window_bg`, assigned before `bar_hover` and `control_fill` derive from it. Split corners stay the Space tint by Windows design; light mode does not change. New §3 row. |
| `a6e3a27a` | improve: keep Space theme editor responsive on slower Macs | port | S, low urgency, measure first. `phi_space_theme_panel.cc`: `FieldBitmap` renders synchronously from paint with a per-panel cache, and each drag runs `PreviewSpacePalette`. A process-wide cache plus a frame timer flushed before `Commit`. The router half has no twin. |
| `a19a2fe6` | chore: remove unused CustomSlider | skip | Dead AppKit slider; no twin in `client/`. |
| `17c0d54c` | fix: reset tab range selection anchor when focus changes | watch | Multi-selection 🔧 (§2): no selection model yet. Clear the anchor synchronously on an active-tab change when it lands. |
| `79b11061` | fix: focus Kiosk page opened from an external app (#256) | port | S, reproduce first. `PhiKioskWindow::OnWindowMessage` `WM_SETFOCUS` also calls `framework_->FocusBrowserWindow` (the shell does, `phi_shell_window.cc`; the Kiosk only `SetFocus`es a `WS_CHILD`), plus a one-shot in `RefreshState` on the first URL while foreground. Applies to every Kiosk open, not only external ones. |
| `423c5a4e` | feat: return to the source app when an external-link Kiosk closes (#253) | watch | New §1 row 🔧, waits on the external-link hook. |
| `6985cc34` | feat: add Split with New Tab to the Tab menu and shortcuts (#226) | port | M; New Tab Page mode alone is S. `kCmdSplitWithNewTab` beside `kCmdNewTabToRight`, reusing `OpenTab` and `SplitTabsNormalized`, on the agent refusal list, disabled when already split, in the placeholder, group overview or Kiosk. `kRebindableShortcuts` row in group "Tab". A split-pane mode in `PhiOmniboxSession` with a Ctrl hint. New §3 row. |
| `63c68d90` | fix: restore the session before a windowless Kiosk-setting rule link opens in a Space | watch | Open external links in Kiosk 🔧 (§1): the Mac's Swift-side external path, which Windows lacks. A forwarded link on Windows already restores first through the fork's `PhiExplicitRestoreIfNecessary`. |
| `74cb2bce` | fix: restore the session before a windowless Kiosk page moves to a Space | done | Ported 2026-11-02, box-verified on nuc: `RouteUrlToSpace` no longer stops at an invalid slot, `RequestSpaceBrowser` hosts a shell of its own or rebuilds a parked window, `FinishRequestedSpaceBrowser` closes the Kiosk once its page has opened, and `PhiCoordinator::RunAfterWindowlessRestore` restores the last session first (`PhiSessionRestore::BeginWindowlessRestore`). The earlier skips of `4dd5a8c3` and friends hold for silent launch, but process-alive with no user window IS reachable on Windows. |
| `2028f868` | fix: restore the session before a windowless agent open lands in a Space | done | Ported 2026-11-02, box-verified on nuc (old build answered `windowOpened:true` for a rebuilt window; new answers false with the last session back first). `OpenUserSpaceWindow` with `activate` goes through the gate and `activate:false` stays ungated; `PendingCreation::materialized_ghost` carries the answer. `references/management.md`, the `spaces.openTab` text and `verbs.md` updated. |
| `ea04bb00` | fix: hold windowless opens until the restore's fallback window lands | done | Ported 2026-11-02 with `74cb2bce`: the hold ends only after any rescue shell lands (`awaiting_restore_fallback_`), with the watchdog as backstop. The rescue-shell branch was not forced on the box: every restore there brought a window back. |
| `28c0618b` | improve: refresh onboarding design and navigation | port | M for behaviour: Back on Welcome / Layout / Next Steps with Escape as Back (`OnEscape` handles only SigningIn and GuestPrivacy today), "Sign in instead", legal strings (`CheckRowRuns` already takes a reordered {1} {0}), arrows on the layout page. Design decision: the two 200×40 footer pills and the guest feature list. Skip the Login retry hint (hosted page) and the password-manager changes. |
| `0d790e04` | i18n: guest onboarding feature list, legal link wording, Split with New Tab, account card and agent status in 8 locales | port | Catalog regen (`tools/gen-strings-catalog.py`), with the ports that use the keys. Across this whole range, 12 changed Mac values hit keys Windows has, among them `oobe.*.legalAgreement`, `termsLink`, `privacyLink`, `beginButton`. |
| `b61f1fb4` | fix: keep a Kiosk page focused when it moves into a fresh window | done | No seed-on-top race: `FinishRequestedSpaceBrowser` opens the URL inside the creation announcement and `OpenEmptyWindow` seeds only an empty strip. Recheck focus once `74cb2bce` lands. |
| `a8a8f23e`, `e27e68ab` | Let the recovery code confirmation page go back to the displayed code (#260) | watch | Sync keys ◐ (§5). Same patch twice. Windows has no confirmation page to go back from: `kSaved` clears the code and calls `LoadPairing`. That gap dates from Mac `32799547`, now on the row. |
| `94648b6f` | fix: stabilize omnibox layout before focusing input | skip | Auto Layout / NSPanel. `PhiFloatingOmnibox::Create` computes its rect, sizes the EDIT, then shows and focuses, in order. |
| `565e6d37`, `20539f59` | fix: host native Chat menus and preserve canvas transparency | skip | ⛔ AI suite (`sidecar.nativeMenu`, Chat canvas). Same patch twice. |
| `4586eb53` | fix: hide the address bar separator while previewing a split | skip | No split preview on Windows, and the row stays under a real split by design (`split-view-chrome.md`). Reconsider if `6985cc34` ports a blank-pane preview. |
| `87543479` | fix: apply the onboarding nickname to the signed-in account | done | No staged credentials on Windows; `AccountInfo` prefers the profile's name, and `PhiAuth` refresh rewrites the user from the new id_token. |
| `8fb61ef5` | fix: keep the randomized onboarding card style across navigation | port | S. `PhiOnboardingWindow::RandomizeCard`: `SetProfileCardStyle` first, `PhiNotice` on failure and no card change. Matters even without Back: close and resume reloads the old style while the randomized theme is already saved. |
| `10a47dc9` | feat: sign in from the onboarding login page with the Enter key | done | `PhiOnboardingWindow::OnEnter` `kLogin`, guarded by `!AccountSigningIn()`. Not tested on a box. |
| `59fa1cfb` | fix: keep peek and reader pages from washing out lighter in dark mode | skip | AppKit Plus Lighter vibrancy. The Windows panels parent a whole browser HWND with no vibrancy. |
| `1c39d48c` | fix: size the agent viewport from the agent Space's own page area | done | Ported 2026-11-02, box-verified on nuc at 300 % and 150 %, hidden and watched (§6 row). `VerbAgentPanelSize` (`phi_verbs_lifecycle.cc`) answers from `ctx.scope` in CSS pixels, rounded up as Chromium rounds a view; `helpers.mjs` and the skill docs took the Mac's patch, naming the task and dropping the AI Chat panel. The skill selftest passes as far as the old skill's: both stop at `Storage.setCookies`. Its drag step needs a page area about 674 px tall (the fixture's target gets centred), which nuc at 300 % cannot give; it passes at 150 %. |
| `9f5b887f` | fix: align sidebar extension shelf spacing with the design | skip | No sidebar shelf; the address-in-sidebar layout (Performance) is removed on Windows. |
| `c87fde81` | i18n: drop the spaces around the link placeholders in the Chinese legal agreement sentences | port | Comes with the catalog regen above. |
| `5196706b` | feat: default to omnibox after guest onboarding | port | S. `PhiCoordinator::OnboardingContinueAsGuest`: set the Omnibox new-tab behaviour beside `RequestLayoutMode`. The floating omnibox it opens landed 2026-10-28. |
| `918ea103` | feat: report email protection state in preferences snapshot | skip | AI-suite setting, like `641e4b2d`; Windows has no such preference. |
| `38d59a35` | fix: keep the sidebar collapse animation smooth | skip | NSVisualEffectView `maskImage`. `SetSidebarCollapsed` is instant and the shell paints the backdrop. |
| `f93aa171` | fix: show the theme gradient in the floating sidebar | done | `PaintFloatingSidebar` regrades through `PhiBackdropRowGrounds` when a palette has two or more stops. The macOS 26 glass half has no twin. |
| `1cf61180` | fix: hide card identifier, handle email-derived names, and remove retry hint hover | port | S. Drop the `member_id_` draw in `phi_holographic_card.cc` `EnsureStatics` (keep `MemberId`). In `ShowPage(kSetName)` prefill the email's local part, and in `OnAccountChanged` continue to Welcome if updating that prefill fails. Retry hint hover already absent. Not checked: whether Auth0 sends an email as `name` on Windows. |
| `d94d3329` | fix: copy the peeked URL from the address bar's Copy URL buttons | done | `CopyLinkToClipboard` copies `nav_url_`, which `RebuildNavState` fills from `PageCommandBrowser()`. The chrome:// branding gap in the other copy paths is on the §1 row. |
| `24c2345f` | fix: don't rebuild the AI Chat panel while the AI switch is off | skip | ⛔ AI Chat. |
| `b3351da8`, `d967f0cf` | merges (release/2.15.0 into main, main into dev) | skip | No conflict resolutions in the merge diffs; their contents are triaged by commit here. |
| `6a54718b` | feat: admit signed companion apps over CDP, with page actions and the built-in Folio stepping aside (#269) | skip | Decided 2026-11-02: companion apps are not needed on Windows for now. New §6 row ⛔. |
| `8d76a688` | fix: even out sidebar spacing to match the design | port | S, needs a design call first. `ComputeShellLayout`: drop the gap after a non-empty pinned band and trim its top inset, keeping 2 dip for the pin tile's overhang. `windows-native-ui.md` also specifies 8 gap + 8 inset, so the 16 dip may be intended, in which case this is a skip. Header offsets and the separator have no twin. |
| `96eaa1d3` | fix: keep the comfortable tab outline on the page card after a height-only resize | skip | Windows comfortable has no outline path: the active tab is a filled top and the page is flush. |
| `dc18d2c5` | feat: show companion apps' page actions in the web-content right-click menu, with the selection and link sent to the app | skip | Same decision. Its fork half `03d1b24aa3b` compiles on Windows but is dormant: `PhiPageActionMenuRegistry` stays empty, so the stub in `e1b1cca9c00` is unreachable. |
| `8d998ebc` | fix: make onboarding pages readable with VoiceOver | port | M. `phi_onboarding_window.cc` `Layout`: UIA text for page titles, bullets and steps; name the card and the EDIT; fix the consent row's raw pattern; announce page changes (`AnnounceToScreenReaders` is file-local in `phi_toast.cc`). Keyboard focus on drawn hits and Space on the consent box are the most serious part. New §5 row. |
| `0c4f1552` | fix: show an empty page instead of the game in new windows and empty Spaces under the Omnibox new tab behavior | done | The stub drops the fork's dino contents and `NativePagesBlankPlaceholder` blanks the native page. Windows is also blank after a last-tab close, where the Mac keeps its game (§3 omnibox row). |
| `cbac32e7` | chore: remove the unused Chromium bookmark notification handlers | skip | Dead Mac handlers. Fork `60f97510768` is safe for Windows: its removed pushes were `IS_MAC_PHI`, and the client pulls through `AllBookmarksJson`, which the commit leaves alone. Fork not built for Windows in this pass. |
| `af179a2c` | fix: activate the opener instead of leaving the page blank when Chromium picks a Peek or Reader tab after a close | done | Surfaces are never strip tabs here: Peek and Reader live in their own browsers (`DivertReaderSurfaceAfterInsert`, `phi_reader_panel.h`), so Chromium cannot pick one. Not box-tested. |
| `8c0b8596` | fix: keep the Peek panel at its size when focus returns to its opener after another tab closes | done | `PhiPeekPanel::Reposition` sizes from `opener_->ContentRect()`, pure shell geometry with no mount involved. Not box-tested. |

### 2026-10-27 — phibrowser-mac `68638148` → `f7d850a3` (97 commits; release/2.14.0 + 2.14.1 merged into dev)

**Port recommendations this pass (16 commits in 14 ports; the S/M ones landed
2026-10-27, see the rows; two L design changes are recommended, not built).
Box-verified 2026-10-27 on nuc (pipe) and gmk (screenshot rigs): omnibox
empty-window behaviour (new window / last-tab close / incognito keeps a tab /
New Tab page unchanged), Shortcuts pane (titles, new rows, Reset All dialog,
Delete-to-unbind, per-row Reset, ⋯ menu chord), neutral hover card (light +
dark), new Space after the source, floating sidebar over content fullscreen,
Peek address field (show / Ctrl+L navigate in place / Esc restores). Not
box-tested: the Sync Removal / Reset row (neither box is paired), the General
inline profile card (gmk has two profiles), sign-out closes (no sign-out on a
test account), `parked_for_scope` and the App-rule replay (host tests only).**

1. `a19624bb` + `c11849cf` **page commands and the address field aim at the
   presented Peek** (Reload, Stop, zoom, print, save, view source, DevTools,
   Copy URL, Bookmark, Folio; Ctrl+L shows and navigates the Peek's URL
   without closing it). Windows aimed everything but Find at the opener.
2. `936d090e` **Omnibox new-tab behaviour in empty windows**: no auto-created
   tab on last-tab close or a new window; placeholder mode + focused address
   field (the Windows twin of the Mac's floating omnibox). Fork + client.
3. `055df916` **Reset sync when the last device removes itself**
   (`PUT /keys/v1/reset-operations/<uuid>`, `marker.json`
   `resetOperationId`, Removal row flips to Reset; 409 `last_device` only by
   body code). Windows dead-ended on the last device.
4. `4d6a7bf2` **pins parked for another pinned-tab scope stay out of
   "Needs attention"** (`parkedForScope` cursor flag). A Windows device in
   Space scope showed Needs attention forever once it pulled app- or
   Profile-shaped dormant copies.
5. `48d4c4a6` **App URL rules** (`app:<bundle id>` hosts): accept, store,
   sync and show them so Windows peers stop refusing the Mac's rules each
   round (`inboundAcceptanceVersion` replay). Matching stays Mac-only —
   Windows cannot learn which app launched a URL (see the §1 row).
6. `896b36d0` family (`c89c05cb`, `60dfd31a`, `ce059932`) **Shortcuts pane**:
   localized row/group titles, "Reset All to Defaults" behind a confirmation,
   Delete-to-unassign + per-row Reset, Chromium page commands rebindable,
   effective chords in the ⋯ menu, displaced default chords no longer fire,
   Alt+Shift+N split-tab chord gone (menu action only, as on the Mac).
7. `3a4469a3` **sign-out closes every auxiliary window** (Kiosk, Feedback,
   URL rules, Sync setup, Create Space, Import) before the Login page.
8. `acd6b265` (+`189babda`, `86a43c0e`) **floating sidebar over content
   fullscreen**; Windows destroyed the panel whenever a page was fullscreen.
9. `5588d798` **a new Space lands right after the source Space** (strip
   placeholder too) and the **Space hover card loses its tint**.
10. `d509f24d` **General no longer shows the per-profile Folio Folder row
    twice**.
11. `3b450442` catalog: the Removal section re-keyed to the Mac's
    `sync.removal.*` so its translations survive the regen.
12. `6b300dc0` + `cda12eaf` (+`752d7539`, `fbc5b9de`, `230725c6`)
    **holographic membership card** (procedural foil card with member ID =
    SHA-256(sub)[0:8], random style, export) in onboarding Welcome and
    Settings ▸ You — **recommend, L**: replaces the 12.5 MB conch frame
    sets; a design decision for the Windows-native direction (new §5 row).
    LANDED 2026-10-27 (§5 row ✅, Round K: `PhiHolographicCard`).
13. `3d045cf6` + `4c9d919e` + the harmony half of `23c728d9` **Space theme
    palette editor** (colour field, 1–3 linked handles, harmony, light/dark)
    — **recommend, L**: local-only record, nothing on the wire; a Mac palette
    Space renders on Windows as its pinned preset (new §2 row). LANDED
    2026-10-27 (§2 row ◐: primary colour only on the chrome, Round J).
14. `80f92213` **Shift-click SPA-router guard + opener video pause** —
    **recommend, M, fork**: a capture-phase click guard so page listeners
    cannot navigate the opener behind a Peek; needs a Blink or per-tab CDP
    seam under `IS_WIN_PHI_CLIENT` (Peek row gap).

| Mac commit | Subject | Verdict | Windows action / reason |
|---|---|---|---|
| `039e52d3` | perf: observe traffic light frame changes per object | skip | macOS window buttons. |
| `a19624bb` | fix: aim page commands at the presented peek instead of the opener | done | Ported 2026-10-27: `PhiShellWindow::PageCommandBrowser` (the visible Peek over the active tab, else the tab) drives Reload / Stop / zoom / print / save / view source / DevTools / Find / Copy URL / Bookmark / Folio in `PhiAppMenu::ExecuteCommand`. |
| `ebc7063e` | improve: default the Folio auto-save flag on unless PostHog disables it | done | Windows has no remote flags; `kPhiFolioAutoTriggerEnabled = true` (`phi_features.h`) already shows the toggle. |
| `d509f24d` | improve: hide the per-profile Folio Folder row in General | done | Ported 2026-10-27: `BuildProfileDetail(show_folio_folder=false)` from General; the Profiles pane keeps its row. |
| `addeb8b5` | fix: square the Peek control chips and centre their glyphs | done | AppKit alignment-rect artefact; Windows chips are drawn 28-dip rects with DWrite-centred glyphs (`PhiPeekPanel::PaintHeader`). |
| `45a10144` | feat: host Mirage's Folio library page inside the Library's Folio pane in place of the native library | watch | The Mac DELETED its native library; `library.html` is the only Folio UI on both sides, so the former "native Folio library 🔧" row is closed (§6) and the Library 🔧 row (§2) now hosts `library.html` in its Folio section. |
| `5d5f3c6f` | fix: prevent delayed token notifications to Sentinel | skip | ⛔ AI suite. |
| `10e72672` | docs: sync pageColor bridge contract with held page color | done | The documented contract is the fork's `PhiPageColorTabHelper`, compiled for both clients. |
| `f4cb47d2`, `4ec8a392`, `e5bc31ef`, `5a192b93` | fix: page card inset / corners / resize through AI Chat and side panel slides | skip | ⛔ AI Chat + Mac content-surface hosting; the Windows extension side panel is Chromium's own view. |
| `2010d662` | fix: open the content address bar on the activation click | done | The Windows field is a real EDIT; `WM_MOUSEACTIVATE` lets the activating click reach it. |
| `fec4739a`, `2ac14793`, `7b0c35ec`, `0f0aa5c3`, `e4b5b572`, `c11e968c`, `715c8b67`, `f7bc5b67`, `916b0303`, `39681178`, `6e399950`, `c8c6ac28` | feat/fix: screenshot annotation editor series (crop, floating tools, scrolling selection, blur, Bezier strokes, PNG, PDF leaks, export, toolbar order) | watch | Capture Full Page 🔧 (§1). The row now records what to match: PNG (not JPEG) since `f7bc5b67`, Capture Area…, and the attached annotation editor. |
| `63238441`, `100a2a2f` | fix/test: preserve mini player visibility across navigation | watch | Mini Player 🔧. |
| `29cb09ed`, `ffcee6bf` | fix: theme tab colour / flashes while the extension side panel is open | skip | Windows has no page-card separation; `ComputeActivePageColor` ignores the side panel. |
| `896b36d0` | Feat/shortcut engine (#203) | done (Windows deltas) | Ported 2026-10-27 the parts a Windows user sees: Delete-to-unassign + per-row Reset, Chromium page commands (Open File, Print, Find, View Source, DevTools, Console, zoom, Back/Forward/Home, Settings) rebindable, effective chords in the ⋯ menu, displaced default chords swallowed by the hook. Not needed: owner/scope inventory, NSMenu scan, informational conflicts (Windows keeps confirm-and-steal), filters, system-hotkey warnings, `excludedKeys`, Kiosk global hotkey (⛔). |
| `93ca9d0e`, `a34b8627` | fix: Swift 6.3 filtering workarounds | skip | Compiler workarounds. |
| `0a0f483e`, `d9b2d481`, `69035603` | i18n: shortcuts / screenshot / reset strings | done | Imported by key on the 2026-10-27 catalog regen; screenshot-editor keys wait on the Capture row. |
| `3a4469a3` | fix: close auxiliary windows before showing login after logout | done | Ported 2026-10-27 in `PhiCoordinator::RequireLogin`. |
| `4d6a7bf2` | fix: keep pins parked for another pinned-tab scope out of held and Needs attention (#206) | done | Ported 2026-10-27: `OwnedCursor::parked_for_scope`, set in `PinOwnedKind::Land`, excluded from `FinishStatusRound`'s pending-inbound count. |
| `c89c05cb` | fix: localize missing shortcut command titles | done | Ported 2026-10-27: every row, fixed row and group heading of the Shortcuts pane resolves a Mac `settings.shortcuts.*` key. |
| `7e731204` | feat: add customizable capture shortcuts and clarify key labels | watch / done | Capture rows: Capture Full Page 🔧 (add two `kRebindableShortcuts` rows, Ctrl+Shift+1/2, when it lands). Key labels: Windows chords are VK-based (`PhiFormatChord` prints Ctrl+Shift+1), so the shifted-symbol ambiguity cannot exist. |
| `ce059932` | fix: prevent false close conflicts and disable split tab shortcuts | done | Close half n/a (Windows compares its own rows only). Split half ported 2026-10-27: the fork's Alt+Shift+N → `IDC_NEW_SPLIT_TAB` row is excluded under `IS_WIN_PHI_CLIENT`; split stays a menu / Alt-click action. |
| `4f947521` | fix: prevent crash when opening the profile menu | skip | AppKit `NSMenuItem.copy()` of a hosting view; Windows menus are HMENU rows. |
| `39681178`… | (screenshot editor, see above) | watch | |
| `60dfd31a` | improve: use reset wording in shortcut settings | done | Ported 2026-10-27: "Reset All to Defaults" behind a destructive `PhiConfirm`; per-row "Reset". |
| `80f92213` | fix: refine PeekView transitions and link activation | done | Functional half ported 2026-10-27, box-verified on gmk (PhiModClick, `probes/spa-guard.mjs` + `pk-pages/spa*.html`): fork `chrome/browser/phinomenon/phi_peek_link_guard.{h,cc}` installs the Mac's capture-phase `click` guard (verbatim script) in a Phi isolated world of every committed document — `TabsProxy` attaches the helper per tab, `DidFinishNavigation` runs it, no DevTools session — and removes it from live documents when `peekViewEnabled` is pushed off; `phi::OpenUrlAsPeek` pauses the opener's playing `<video>`s in every frame. Verified: a Shift-click on a `preventDefault` + `pushState` link opens the Peek with the opener's URL unchanged and its video paused (also a video inside an iframe); a plain click still runs the router; with Peek off the Shift-click reaches the router and opens nothing. Visual half (appear/close flights, opener dim, chip shadow) stays a Peek ◐ gap. |
| `6f047987` | fix: display keypad Enter symbol in shortcut settings | done | Windows stores the VK only; both Enter keys are one chord labelled "Enter". |
| `7fe71d5c` | fix: support non-Latin shortcuts for screenshot history and feedback image paste | done / watch | Feedback paste keys on the WM_KEYDOWN virtual key (`PhiFeedbackWindow::ControlProc`), layout-independent. Screenshot half: Capture 🔧. |
| `4bfb1639` | fix: use cached thumbnails for group overview previews | done | `PhiGroupOverview::BuildCards` only reads `TabThumbnailBgra` (the cached thumbnail); no live capture to remove. |
| `074f1900`, `c7d82b6d` | sidebar Chat button width / hide Feedback to keep Chat titles | skip | ⛔ Chat; the Windows bottom bar (Downloads + Feedback) cannot overflow at the 193-dip minimum. |
| `d830a327` | fix: load notification avatars through the agent transport | skip | Windows cards draw no agent avatar (noted on the §6 row). Rule for when one is added: `/api/v1/files/…` refs go through the agent transport, internet URLs never carry the Phi token. |
| `a96f0673`, `f20dbecd`, `53045c78`, `615e2335`, `84c793c5`, `129229f9`, `708ec8ed`, `568c062d` | Sentinel service broker, Private AI settings, `openPhiChatAtPath`, Phi Chat links | skip | ⛔ AI suite. |
| `e7e62b93`, `d2d989a0`, `5e1fce4f` | merges (release/2.14.0, #212, #215) | skip | Merges; their contents are triaged by commit here. |
| `3d045cf6`, `4c9d919e`, `23c728d9` | feat: linked theme palettes + colour picker; dark palette curve / drag preview; unified content surfaces + palette refinements | done | Ported 2026-10-27 (Round J): per-Space palette record `{hue, saturation, brightness, colorCount, harmony}` stored LOCALLY under the Mac's `spaceThemePalettes` key with the Mac's coding (version 3, legacy decode rules), not on the Space proto; `PhiResolvePalette(base, appearance, palette)` with the Mac's HSB maps and 0.8 overlay alpha, palette over saturation / hex, a preset choice clears it, the Settings slider writes the palette's radius; the editor replaces the swatch row + slider in `phi_space_theme_panel.cc` and the create form's colour block is the Mac's "Color ›" row on a draft committed after creation. §2 row ◐ (sidebar gradient + Settings "Color ›" row followed 2026-10-28). `23c728d9`'s Chat canvas / `getWindowTheme` / header-above-NTP parts are ⛔ Chat or Mac content-surface. |
| `5588d798` | feat: refine space switcher and window controls | done | Ported 2026-10-27: a new Space is placed right after the source Space (`ReorderSpace` after create; strip placeholder after the source pip, viewport anchored on the source); hover card restyled neutral (ink washes, no Space tint; Windows keeps its agent status line). Traffic lights skip; Mini Player card material watch; `RetitleShell` same-title guard already present. |
| `40312627` | feat(sync): sync method step, Merge or Use your account's data (PHI-1716) | watch | Layered on the pairing wizard's Spaces step, which Windows lacks (`user-data-parity.md` S5). No wire change: no endpoint, envelope field or flag — a Windows peer sees the Mac join exactly as before. §5 sync row corrected to ◐. |
| `48d4c4a6` | Route external links by sending app with App URL rules (#220) | done (storage / sync / editor half) | Ported 2026-10-27: `UrlRuleKind::Refuses` accepts `app:<id>` hosts, `SpaceSyncTable::inbound_acceptance_version` (=1, absent → 0) arms a one-time Space replay so already-refused rules arrive, the URL-rules editor shows the kind as "App" (retarget / ask / delete, not creatable), `PushRoutingTableNow` skips them. Matching is Mac-only (new §1 row). |
| `6b300dc0`, `752d7539`, `c6889f07`, `fbc5b9de`, `230725c6`, `cda12eaf` | feat: native holographic cards in onboarding and profile; welcome page redesign + member ID from `sub` | port (recommend) | `HolographicCardView` (CALayer foil/grain/seal stack, 8 palettes × 6 textures × 6 ornaments, pointer tilt, member ID = SHA-256(sub)[0:8], "Date Joined", PNG export; random style on the Welcome page via a dice button, persisted per account) replaces the frame-sequence card on both the Welcome page and Settings ▸ You. Windows draws the conch JPEG frames (`phi_profile_card.cc`, 12.5 MB of frames) and shows no member ID. New §5 row ❓. |
| `c11849cf` | Show and navigate the presented Peek's URL from the address-bar omnibox (PHI-1745) | done | Ported 2026-10-27: `OmniboxBrowser()` follows `PageCommandBrowser()`; `RebuildNavState` describes the Peek; a commit on a Peek navigates it without closing it; the field re-syncs when the Peek closes under it. |
| `4dd5a8c3`, `876eb8fe`, `16e50fc5` | fix/refactor: external links open at once with no window / after a windowless restore; shared readiness check | skip | Mac cold-open queue. Windows has no client-side external-open path: ProcessSingleton + stock `StartupBrowserCreator` open the URL (see the §1 row). |
| `acd6b265`, `189babda`, `86a43c0e` | fix: floating sidebar takes clicks in content fullscreen (PHI-1794); standalone windows too; keyboard back to the fullscreen page on hide | done | Ported 2026-10-27: `RelayoutShell` keeps the floating panel in content fullscreen, `FloatingPanelRect` drops the address-row offset, hide posts focus back to the page. `189babda` n/a (one `PhiShellWindow` path); `86a43c0e`'s no-steal rule already held (`MA_NOACTIVATE`). |
| `c4dcc11c` | Scope bookmark publisher fetches to their Profile and Space | skip | SwiftData predicate optimisation; Windows filters an in-memory map. |
| `24bf4b69` | Avoid full bookmark reloads for visit-only updates | done | `LocalStore::SetLastSeen` is a quiet flush that never fires `on_rows_changed_`. |
| `ee681cfe`, `40a29a66`, `2bba41f0` | sidebar scrolling stalls with many tabs | skip | `NSScrollView` / `NSTableView` reuse; the Windows list is one coalesced `RebuildTabs` per scroll notch, viewport-clipped. |
| `ee384556` | fix: third-party password manager icons at full opacity in OOBE | skip | Windows skips the password-manager step (`phi_onboarding_window.h`). |
| `641e4b2d` | feat: add email protection setting exposed to extensions | skip | AI-suite plumbing: a row inside the AI settings pane feeding the bundled extensions' `getNativeSettings`. |
| `d7ff1f4e` | fix: migration wizard crash when a Zen session lists a workspace twice | watch | Migration wizard 🔧 (§1): dedupe workspace uuids first-wins when it is built. |
| `e72a204a` | fix: keep items removed from diffable outlines alive briefly | skip | AppKit `NSOutlineView` hazard; Windows rows are value rows, UIA refs already ref-counted. |
| `ae367c72` | fix: stop keying the slot snapshot by user data directory | done | `PhiRestore.json` lives in the account dir inside the user-data dir and moves with it. |
| `01395154` | feat: restore by the cold-start rule on the first wake after a silent launch | skip | Silent launch = Phi Chat shim / Sentinel `--no-startup-window` (⛔ AI suite); Windows never launches windowless. |
| `3b450442` | i18n: sync setup method / replace, removal and reset, theme palette modes, app URL rules, membership card, email protection (8 locales) | done | 0 new keys; 66 keys gained locales. Windows Removal section re-keyed to `sync.removal.*` (its old keys borrowed translations from Mac keys this cycle deleted); `sidebar.urlRulesEditor.matchType.app` added; the rest have no Windows surface yet. |
| `936d090e` | feat: open the omnibox instead of auto-creating a tab in empty windows under the Omnibox new tab behavior | done | Ported 2026-10-27 (fork + client): pushed `SetNewTabOpensOmnibox`; `Browser::TabStripEmpty` skips the refill tab and shows the placeholder at once under `IS_WIN_PHI_CLIENT`; new user Space windows are seeded tab-less and enter placeholder mode (`EnterPlaceholderModeIfEmpty`), which focuses the address field — the Windows twin of the floating omnibox. |
| `1e12c9d9`, `ba064919`, `d074f9c5`, `f7d850a3` | fix: floating omnibox click-through / hosting / event replay / panel size | skip / done | Mac NSPanel overlay plumbing. The Windows field is an inline EDIT: a press outside it lands on the real control and `WM_KILLFOCUS` closes the dropdown. |

### 2026-10-19 — phibrowser-mac `208eede5` → `68638148` (65 commits; the Folio port's baseline to the first ledger)

**Port recommendations this pass (4; all four landed 2026-10-19 as commits e062e80, 82fe397, 6e24f6a and c520431 — 1 and 2 box-verified, 3 and 4 build-verified only, see the rows):**

1. `b0a536db` **Enter on an engine row with an empty query opens the
   engine's site** (scheme + host, path `/`). Windows `CommitKeyword`
   in `client/phi_omnibox_session.cc` still returns on an empty query (the
   Mac's old guard). Small.
2. `9387a460` + `9068b449` **shell window title** follows the presented
   Space's active tab, and shows the Space name while the placeholder
   page is up. Windows never retitles the shell (new 🔧 row in §1).
   Small, user-visible in the taskbar and Alt+Tab.
3. `05160468` **pin scope: key is the desired scope, row is the actual;
   a pin sync round blocked on a scope migration publishes nothing**
   (generation fence, retry, "not applied yet" status line). Windows
   `phi_sync_settings_win.cc` applies the synced key straight through
   `set_pin_scope` and `phi_sync_owned_win.cc` samples the scope once per
   snapshot with no fence. Medium: a Windows peer can still publish pins
   under a stale scope while a migration runs. Read `docs/sync.md` B-3
   stage 1 before porting; the 2026-10-16 `MigratePins` use-after-free fix
   touched the same path.
4. `7a967bd3` / `789584cb`, `1e93e6a1` / `8638de8a`, `e13e6dca` / `486c20a8`
   **sync pairing: the Default profile pairs only with the account's
   default profile** (fixed row, no picker; keeps the default flag on
   rename; `PATCH /keys/v1/profiles/<uuid>` with an expected envelope,
   409 on mismatch). Windows `phi_sync_setup_window.cc` has no default
   rule and `phi_sync_key_service.cc` no replace-envelope call. Medium:
   a Windows device can pair Default to a non-default remote profile,
   which the Mac now refuses. Each pair is one change on `dev` and `main`.

| Mac commit | Subject | Verdict | Windows action / reason |
|---|---|---|---|
| `9d0121c7` | fix: prevent alert sounds when holding Escape to dismiss omnibox | skip | Mac `NSBeep` on key-repeat; Windows Escape exits the keyword chip / closes the popup without a beep (`phi_omnibox_session.cc` `VK_ESCAPE`). |
| `b0a536db` | fix: open search engine sites when submitting empty-query hints | done | Ported 2026-10-19: Enter or a click on the "Search X" row opens the engine's site (`PhiOmniboxSession::OpenEngineSite`, `PhiOmniboxEngineSite`); Tab still raises the chip. Box-verified (Bing row → `https://cn.bing.com/`). |
| `4e6de510` | fix: preserve media controls and tab indicators when muted | watch | Mini Player (§3 🔧) and the new tab audio indicator row (§2 🔧). |
| `c3ffbcb9` | fix: keep Mini Player volume controls open and unmute on click | watch | Mini Player 🔧. |
| `83722665` | fix: preserve Mini Player visibility and theme across Space switches | watch | Mini Player 🔧. |
| `55d1519a` | fix: reduce Mini Player flashes during Space auto-PiP transitions | watch | Mini Player 🔧; the auto-PiP half is already ✅. |
| `9ee39fa4` | fix: show webpage volume in the Mini Player slider | watch | Mini Player 🔧. |
| `d00df7b2` | fix: keep muted Mini Player cards available after switching sources | watch | Mini Player 🔧. |
| `f6e13a27` | fix: prevent Mini Player slider drags from switching sources | watch | Mini Player 🔧. |
| `5e9a65da` | feat: track notification card usage and popup settings | done | Ported 2026-10-16 in phibrowser-win `ed557b2` (`notification_card_action`); popup / mute values are ⛔ by design (`analytics.md`). |
| `05160468` | feat: pin scope row follows the scope setting through a per-account follower, generation fence and blocked-round silence | done | Ported 2026-10-19 as the fence half: `LocalStore::pin_scope_generation` (bumped by `ChangePinScope`), sampled before the rows in `PinOwnedKind::BeginRound`, re-probed by `RescanScopes` (so X → Y → X blocks), a commit fence (`OwnedKindAdapter::CommitFenceHolds`, pins only) right before each commit batch, and a landing fence in `PhiSyncPinAccess::Apply` that writes nothing and parks when the generation or scope moved. The follower / "not applied yet" status line is not needed: Windows migrates the store and writes the key in one UI call. Build-verified; the box is in guest mode, so not live-tested. |
| `7a967bd3` / `789584cb` | Sync: pair the Default profile only with the account default profile | done | Ported 2026-10-19: envelope `"default"` flag (`PhiProfilePayloadJson`), `RegisterLocalProfile` / `AdoptRemoteProfile` take `is_default` (a Default refuses a known non-default with `kDefaultMismatch`), `RemoteProfile::MayPair`, the mapping pass adopts an account default for a free Default first and logs demoted defaults once per uuid, best-effort `BackfillDefaultFlag` over `PATCH /keys/v1/profiles/<uuid>` (`ReplaceProfileKeyEnvelope`, 409 → retry ×3). Chromium's `Default` profile is the device Default. Build-verified only (no signed-in box). |
| `1e93e6a1` / `8638de8a` | Sync: keep the default flag when the pairing wizard renames a remote Profile | done | With the above: `RemoteProfile.is_default` rides through the sheet's candidates. |
| `e13e6dca` / `486c20a8` | Sync: fix the pairing sheet's Default row to the account default instead of offering a picker | done | Ported 2026-10-19 in `phi_sync_setup_window.cc`: `FixedDefaultRemote` (the Default's row shows the first readable account default, no picker), `LocalChoice` reads back under the rule, `MayPair` filters the local pickers and "Assign to", no "Create on this PC" for the authoritative default, `kDefaultMismatch` shows `sync.setup.error.defaultProfileMismatch`. No Default badge on rows (Windows-only omission). Build-verified only. |
| `fa58b0b4` / `11b92af2` | i18n: pinned tab scope, synced deletion and profile matching strings | done | Catalog regenerated 2026-10-19 with the one new Windows key (`sync.setup.error.defaultProfileMismatch`, PC wording, English only until the translation repo has it); the Mac's pin-scope status strings are not needed here (see `05160468`). |
| `1705cb23` | feat: export original images from image preview | watch | Image preview window 🔧 (§3); the export button comes with it. |
| `54f3d0f5` | i18n: image preview export strings | watch | With the image preview. |
| `a6f31c0a` | feat: add Sidecar Live Mode setting for nightly and debug builds | skip | ⛔ AI suite. |
| `7ace2350`, `664e8d2b` | Merge release/2.13.0, merge main into dev | skip | Merges. |
| `93720dd0`, `6d5686e6`, `875626f7`, `ed879465`, `b4285408`, `a1023cea`, `04557f4f`, `dfea7048` | feat: content surface series (draw tab pages through the Space's content surface, bridge declarations, DevTools radii, mount without first paint, close snapshot removed, backdrop, clip rects) | skip | Mac AppKit/CALayer hosting of Chromium web views. Windows hosts the browser HWND directly (`phi_browser_host.cc`); no twin. |
| `94f28d79`, `220b6f0c`, `7045e8f8`, `0be5f32c`, `a195356d`, `a4e03f09`, `0b968c6b`, `43876b43` | fix: web view layout / content surface fixes (final size before join, cold mount, dangling windows, content fullscreen, superview change, side panel slide hold) | skip | Same Mac hosting layer. |
| `e153c5a1` | fix: leave shell fullscreen when the Space that entered content fullscreen leaves it | watch | Full screen 🔧 (§1); the Space-owner rule applies once F11 / content fullscreen is driven by the shell. |
| `9387a460` | fix: title the hosted shell after the presented Space's active tab | done | Ported 2026-10-19 (`RetitleShell`), box-verified. |
| `9068b449` | fix: show the Space name on the shell while the placeholder page is up | done | Ported 2026-10-19 with the above; the native new-tab page is the placeholder here. |
| `bda50b5f` | refactor: detach Peek and Reader hosts on tab close in every window | done | `PhiSurfaceCoordinator::CloseSurfacesForOrigin` already sweeps every window's surfaces on tab close. |
| `d0e8ed57` | perf: write the guest choice and relayout only when the value changes | done | `PhiPreferences::SetGuestModeEnabled` early-returns on an unchanged value. |
| `f186888b`, `1ed78b95` | perf/test: Lottie animations on the main thread | skip | No Lottie runtime on Windows (`phi_folder_icons.h`). |
| `f65329d5`, `b13f4c90`, `1f011d9b` | perf: tab recording icon / badge rendering | watch | Windows tab rows have no recording / audio indicator yet (new §2 row). |
| `4fca9dd1` | perf: reuse the cached traffic light buttons | skip | macOS window controls. |
| `5f36ef3c`, `6f690a1a`, `10988ee6`, `5ca61a6a` | refactor/test: dangling window registry, page container layout mode, test harness | skip | Mac-only structure and tests. |
| `04d129d4`, `33ab7827`, `1497db7b`, `2c86e3bd` | perf/chore: content surface protocol notes and compatibility with an older framework | skip | Mac bridge header documentation. |
| `b6c070fe`, `b692922a`, `6da438eb`, `7afc07ff`, `c478ba8d`, `68638148` | docs: content surface, compositor, fullscreen owner, slide hold | skip | Mac hosting docs; `7afc07ff` is relevant to the full-screen 🔧 row when it is built. |

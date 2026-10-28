# Mac parity: what the Windows client has, lacks and leaves out

This is the weekly feature-parity ledger between the macOS client
(`phibrowser-mac`) and this Windows client. It lists every user-facing Mac
feature once, says whether Windows has it, and for every gap records
either the decision to leave it out or that it still needs to be built.
It is a list of surfaces, not of code: how a feature is implemented is in
the owning doc each row points to.

**Checked against phibrowser-mac `f7d850a3` (2026-10-27) and
phibrowser-win `6804f71` plus the 2026-10-27 parity working tree, on
2026-10-27.**

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
| Copy URL / Copy URLs (⇧⌘C) | with toast | Ctrl+Shift+C, Copy link flash | ✅ | |
| New Tab to the Right / New Tab Below | title follows layout | New tab to the right only | ◐ | No "below" wording in the vertical layout. |
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
| Open external links in Kiosk | Navigation setting | row not drawn | 🔧 | Revisited 2026-10-27 (the ⛔ reason changed): Phi IS registered as the http / https handler (installer `RegisterBrowser`, Settings ▸ You "Set as default"). A link from another app reaches the running browser through Chromium's ProcessSingleton and stock `StartupBrowserCreator`, so Domain / URL rules already steer it, but the CLIENT never learns the open was external. Needs a framework hook on the forwarded command line (`IS_WIN_PHI_CLIENT`) before the Kiosk preference (and App rules below) can apply. |
| App URL rules: route external links by the SENDING app (Mac `48d4c4a6`, 2026-10-23) | `app:<bundle id>` rule kind, running-app picker | rules accepted, stored, synced and shown as "App" (retarget / ask / delete; not creatable); never matched | ◐ | Storage / sync / editor half ported 2026-10-27 (`UrlRuleKind::Refuses`, `SpaceSyncTable::inbound_acceptance_version` one-time replay, `PushRoutingTableNow` skips them). Matching stays Mac-only by decision: Windows cannot recover the launching app (only the command line crosses ProcessSingleton; the parent process is often a broker). |

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
| Multi-selection of tabs / bookmarks with batch menu | `TabMultiSelectionMenu` | strings imported, no selection model | 🔧 | `common.tabMultiSelectionContextMenu.*` keys exist in `strings.json`; nothing draws them. |
| Pinned tabs: scope App / Profile / Space, split pins, rename, edit URL | yes | yes | ✅ | |
| Edit pinned tab icon | Edit… dialog has icon | Rename… / Edit URL… only | ◐ | No icon override on a pin. |
| Bookmark favicon actions: Separate From Bookmark / Back to Saved URL | row favicon menu | none | 🔧 | The "bookmark drifted from its saved URL" affordance. |
| Bookmark folder icon picker | yes | yes (24 symbols) | ✅ | |
| Drag & drop: tabs, pins, bookmarks, groups, cross-window, tear-off | full | ghost / gap / ring / badge / snap tiles / New window | ◐ | Deferred mechanics D31–D34: edge auto-scroll, Ctrl/Shift modifiers, whole-group drag, bookmarks bar as source/target, cross-window slot, OLE file drops, pip lift, hidden-sidebar reveal during drag. |
| Drop a file onto the sidebar | yes | none | ◐ | Part of D31 (OLE drops). |
| Spaces: create (name, icon, profile), rename, icon/emoji picker, theme, change profile, delete, reorder | yes | yes | ✅ | Picker lacks "Recently used" and skin tones (D24); create form keeps old control styling (D25). |
| Space switch slide animation, Ctrl+n, hover card | yes | yes | ✅ | Hover card restyled neutral (no Space tint) with the Mac 2026-10-27 (`5588d798`); Windows keeps its agent status line and follows the app appearance (the Mac forces light). A new Space lands right after the source Space on both sides. |
| Incognito Space | ⌃⇧N | Ctrl+Alt+N | ✅ | `incognito-space-port.md`; not ported: account-menu entry, scripting command, pre-warmed tree, new-pip chip flight. |
| URL rules (Domain / contains / suffix / URL → Space / Incognito / Kiosk / Ask) | yes | yes | ✅ | |
| Pinned tab scope follows the synced setting (per-account follower, 2026-10-14 Mac `05160468`) | yes | Windows syncs `PhiPinnedTabScope` | ✅ | Generation fence + commit / landing fences ported 2026-10-19 (§9); 2026-10-27 `parked_for_scope` keeps dormant other-scope copies out of "Needs attention" (`4d6a7bf2`). No "not applied yet" status line: Windows migrates the store and writes the key in one call. |
| Library (overlay / window: Space cards, pins, bookmarks, Downloads, Folio) | View ▸ Library; rail Folio / Spaces / Downloads | ⋯ ▸ Library, Ctrl+Shift+L, Settings ▸ Spaces ▸ Manage Spaces in Library, downloads popover ▸ All Downloads, profile menu ▸ Download List; rail Spaces / Downloads | ◐ | Built 2026-10-27 (`windows-native-ui-impl.md` Round L): scrim + 36-dip card over sidebar and content, Space cards (theme ground, icon picker, inline rename, theme panel, pins grid + bookmark outline, double-click opens, handle drag reorders, "…" menu, row context menus with Move to Space ▸), inline create card after the current Space (`space_created surface: library`), Downloads list (search, grouped, hover actions), standalone window, `library_section_viewed`. Gaps: no Folio section (Folio is being redesigned — deliberately omitted 2026-10-27); no drag of pins / bookmarks between cards (Move to Space ▸ stands in); no profile filter in Downloads (the framework lists per window, 20 rows); no spring flight / shadow blur. |
| Space theme palette editor: colour field, 1–3 linked handles, harmony (single / analogous / complementary / split), Light / Dark buttons (Mac `3d045cf6`, `4c9d919e`, `23c728d9`, 2026-10-21/23) | replaces the swatch row + saturation slider in the Space editor and the create form; 2–3 colours paint the sidebar as a vertical Oklab gradient | same editor (`phi_space_theme_panel.cc`: 260×296 popover, 244 field, 38/28/14 handles, harmony + Light/Dark toolbar), the create form's "Color ›" row on a draft, local `spaceThemePalettes` record (`model::Space::palette`, `PhiThemePalette`) | ◐ | Ported 2026-10-27 (`windows-native-ui-impl.md` Round J), box-verified on gmk: Space editor light/dark, draft editor from the create form, a split-complementary palette on the chrome, preset choice drops the record, pinned-preset Spaces unchanged. Record is LOCAL on both sides (not on the Space sync payload), so a Mac palette Space still renders here as its pinned preset. 2026-10-28: 2–3 colours now paint the sidebar as the Mac's vertical Oklab gradient down the window's full height (`PhiPalette::ground_stops`, `PhiOklabGradient`): the sidebar is still painted on the flat primary ground and the shell regrades each pixel onto the row's gradient colour by its coverage over that ground (`PhiBackdropRowGrounds`, acrylic and opaque frames, the floating sidebar, the create form), so washes, labels and the active row keep their look over the secondary colours; the top bar and accent stay primary. Settings ▸ Spaces now has the Mac's single "Color ›" row (gradient dot + chevron) opening the editor beside it, in place of the eight dots and the saturation slider (General's Space color row is unchanged). Minor: handles are not named for UIA (one field hit, no "Color n" tooltip); the guest→account migration does not carry palettes. |
| Group Overview page | yes | yes | ✅ | |

## 3. Address bar and page

| Feature | Mac | Windows | Status | Gap / decision |
|---|---|---|---|---|
| Omnibox with suggestions, keyword chip ("Tab to search") | yes | yes | ✅ | Under the Omnibox new-tab behaviour an EMPTY window (Ctrl+N, last tab closed) shows the placeholder with the field focused instead of a new-tab tab, as on the Mac (`936d090e`, ported 2026-10-27, fork + client). Windows has no floating omnibox: the inline field over the placeholder is its twin. |
| "Hold ⌘ to switch" to an already-open tab | yes | none | 🔧 | Chromium's switch-to-tab suggestion is not surfaced in the Windows popup. |
| Site menu: security row, Content blocking on this site, Always show URL path, Site settings, Clear cache / cookie, Extensions ▸, Share ▸ | yes | all but Share ▸ and Extensions ▸ | ◐ | Share: see §1. Extensions live in the field's puzzle button instead (design). |
| Site menu: Collect Site Memories / Delete Site Memories | yes | none | ⛔ | AI suite (site memory is browser memory). |
| Copy link in the field, Reader button (hover) | yes | yes | ✅ | |
| Reader View (typography prefs) | yes | yes + read-aloud HUD | ✅ | Windows has read-aloud; Mac does not. `-phi-auto-reader` automation entry point not ported (analytics doc). |
| Peek (Shift-click, auto-peek from pins/bookmarks) | chips: Close, Open as Tab, Open as Split, Reader View | chips: Close, Open as Tab, Open as Split | ◐ | No Reader chip on the Peek card; Peek is omitted from session restore (`analytics.md`). Page commands and the address field aim at the presented Peek since 2026-10-27 (`a19624bb`, `c11849cf`; Back / Forward and the Reader toggle stay on the opener). The Shift-click SPA-router guard + opener video pause landed 2026-10-27 (`80f92213`: fork `phi_peek_link_guard.{h,cc}` runs the Mac's capture-phase click guard in a Phi isolated world of every document, gated on `peekViewEnabled`; `OpenUrlAsPeek` pauses the opener's videos in every frame). Still missing: appear / close flights and the opener dim. |
| Split view (two panes, drop hints, swap, convert, pin/bookmark a split) | yes | yes | ✅ | `split-view-chrome.md`. |
| Modifier clicks: Shift → Peek, Alt/⌥ → split, Ctrl+Alt/⌘⌥ → Kiosk | yes | yes | ✅ | |
| Kiosk window ("Open in" Space, profile chip) | yes, profile chip menu, cursor-zoom open animation, frame autosave | yes, no profile switch from the chip, default size | ◐ | `phi_kiosk_window.h` lists the four pieces as deliberately not ported; the profile-chip switch has no framework call. |
| Native new-tab page | Phi mark + omnibox, AI suggestions | Phi mark + omnibox | ✅ | AI suggestions ⛔ AI suite. |
| Bookmark manager (native, per Space) | yes | yes | ✅ | |
| Bookmarks bar (always / on new tab) | yes | yes | ✅ | |
| Image preview window (zoom, export original — Mac `1705cb23`, 2026-10-15) | `UserInterface/ImagePreview` | none | 🔧 | Whole surface absent. New on the Mac this cycle. |
| Picture-in-picture: auto Off / Normal / Park at edge | yes | yes | ✅ | `picture-in-picture.md`. |
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
| You / Account: profile card, change name, avatar, sign in/out, default browser, share card | yes | yes | ✅ | |
| Account: Invitation code + View details | yes | none | 🔧 | `login-port.md`: backend work; blocked until the Windows ring has the invitation API. |
| Account: Export account data (email code flow) | yes | none | 🔧 | |
| Account: Delete account (email code, sign out, local wipe) | yes | none | 🔧 | `user-data-parity.md` S6 also lists the missing account-deletion fence. |
| General: language, new-tab behaviour, restore last session, analytics consent, Folio folder + auto-save + Choose sites | yes | yes | ✅ | |
| Appearance: layout, colour appearance, Space colour + saturation, tab toggles | yes (split across General/Advanced) | yes | ✅ | |
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
| Onboarding: welcome, name, sign in / guest, consent, layout, next steps | yes | yes | ✅ | `login-port.md`. The Mac's 2026-10-26 Welcome redesign is the membership card row below. |
| Onboarding: browser import step | Chrome / Safari / Arc / Dia / file | "Import browser data" link under Next steps | ◐ | A dedicated step with source picker is missing; see §1 import row. |
| Onboarding: password manager choice (iCloud Passwords) | yes | skipped | — | iCloud Passwords is Mac-only (`phi_onboarding_window.cc`). A Windows equivalent (Windows Hello / Credential Manager) would be a new decision. |
| Guest mode + guest → account migration | yes | built, not live-tested | ◐ | `user-data-parity.md` S6. |
| Hosted login (Auth0), reauthentication, sign out | yes | yes, off-the-record sign-in window | ✅ | |
| Sync keys: recovery code, device approval, profile / Space matching, sync method (Merge or Use your account's data, Mac `40312627`) | yes | profiles page only | ◐ | Corrected 2026-10-27: the Windows pairing wizard has no Spaces step and no method / replace step (`user-data-parity.md` S5 "Not yet"). No wire change — a Windows peer sees a Mac join as before. |
| Crash feedback ("Phi quit unexpectedly") on next launch | `docs/crash-feedback.md` | Crashpad → Sentry, no prompt | 🔧 | Needs the last-run status from Crashpad and the existing Send feedback form with the crash id. |
| Send feedback: files, pasted images, diagnostics, outbox | 10 × 20 MiB, Sentinel logs | 5 × 10 MB, week-long outbox | ✅ | Sentinel logs are ⛔ AI suite. |
| Account deletion local data removal | yes | none | 🔧 | With the Delete account row above. |
| Membership card: holographic foil card with member ID (SHA-256(sub)[0:8]), "Date Joined", random style + PNG export, on the onboarding Welcome page and Settings ▸ You (Mac `6b300dc0`, `cda12eaf`, 2026-10-20/26) | `HolographicCardView` | `PhiHolographicCard` (`client/phi_holographic_card.cc`): the same layer stack composed in software — palette base + iridescence + radial, one of 6 procedural foil textures, seeded grain, logo + sweep, one of 6 seal ornaments, sheen, edge; pointer light + tilt, entrance spin, flat PNG export at 2× on the accent canvas | ✅ | Ported 2026-10-27 (`windows-native-ui-impl.md` Round K). Foil / grain / seal masks are rasterised once per style and size with GDI+ on a worker and cached as PNG under `<user data>\PhiCardArtwork\v1`; the light-driven layers recompose per frame. Member id from `auth().user().sub`, join month from the profile's `created_at`, style in the account defaults under the Mac's `profileCardStyle` key (palette overridden by the current theme in Settings, as on the Mac). The conch frame sets (12.5 MB) are gone. Not ported: the Mac's `.random()` seeding details (both are uniform), IvyPresto Headline Light now ships for the name. |

## 6. Agents and automation

| Feature | Mac | Windows | Status | Gap / decision |
|---|---|---|---|---|
| Agent Spaces: overlay, fog, ring, pill (Take control / Hand back / Finish), "The agent needs you", Autoview | yes | yes | ✅ | Agent cues D26–D30 deferred: brand accent per agent, pulsing ring, window title, ⋯ on the capsule, omnibox dim/eye. |
| Agent Transcript console | View menu, pip "Show Transcript" | built, `kPhiTranscriptEnabled = false` | ⛔ | Decided off 2026-09-09 (`phi_features.h`). |
| Agent approval (Allow Once / Always / Deny per agent) | yes | one admission switch | ⛔ | See Developer row in §4. |
| Shadow windows, cursor / effects, captureWindow, reader verbs, user messages | yes | yes | ✅ | |
| User-Space verbs: spaces, profiles, bookmarks, pins, groups, split, URL rules, downloads | yes | yes, plus window / menu / layout / theme / settings / feedback / account / onboarding / sync families | ✅ | Windows is a superset. |
| `agentSpace.app.info` (which Phi build is answering) | `docs/agent-app-info.md` | `capabilities` + `ping`, no `app.info` | 🔧 | One verb; the skill should see the same reply shape on both platforms. |
| Notification cards (`notification.show`, Run, paging) | yes, with Mute / Pop up menu | yes | ◐ | The Mute / Pop up presentation menu is deliberately absent (`analytics.md`). No agent avatar on Windows cards (the Mac loads `/api/v1/files/…` avatars through the agent transport, `d830a327`). |
| Credentials: `credentials.status/get/getTotp/autofill` | Bitwarden | Bitwarden helper | ✅ | UI gaps in §4. |
| Organize tabs with AI (⌃⇧G) | yes | none | ⛔ | AI suite. |
| Sidecar Travel Back, Live Mode setting (Mac `a6f31c0a`, 2026-10-15) | yes | none | ⛔ | AI suite. |
| Service broker for bundled extensions | yes | none | ⛔ | AI suite (Sentinel). |
| Sentinel helper, watchdog, Phi Chat hotkey | yes | none | ⛔ | AI suite. |
| Folio: Save to Folio, Open Folio, auto-save on site actions, Add Note | yes | yes | ✅ | `folio-port.md`. |
| Folio library | Mirage `library.html` — the Mac DELETED its native library 2026-10-19 (`45a10144`; `docs/folio-native-library.md` now says the extension page is the only Folio UI) | Mirage `library.html` (`PhiFolio::OpenLibrary`) | ✅ | The 2026-10-19 "implement the native library" decision is void: both sides open `library.html`. The Library surface's Folio section (§2) hosts the same page. |

## 7. Summary (2026-10-27)

| Status | Rows |
|---|---|
| ✅ ported | 53 |
| ◐ partial | 20 |
| 🔧 to implement | 24 |
| ⛔ deliberately left | 18 |
| ❓ needs a decision | 0 |
| — not applicable | 4 |

**The 🔧 list, roughly by user impact:** Mini Player / media controls ·
content blocking switches · multi-selection · Bitwarden Settings rows and standing grants ·
Time Machine-style snapshots + restore · crash feedback prompt · image
preview · Arc/Zen migration wizard · Share · Capture Full Page ·
Export/Import user data · account export + deletion · invitation code
(backend) · "Hold to switch" omnibox row · full screen · client-side
external-link hook (Kiosk preference, App rules) ·
`agentSpace.app.info` · tab audio indicator ·
bookmark Separate/Back-to-saved · What's new / Documentation / Improve
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

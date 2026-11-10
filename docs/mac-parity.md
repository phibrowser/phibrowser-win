# Mac parity: what the Windows client has, lacks and leaves out

This is the weekly feature-parity ledger between the macOS client
(`phibrowser-mac`) and this Windows client. It lists every user-facing Mac
feature once, says whether Windows has it, and for every gap records
either the decision to leave it out or that it still needs to be built.
It is a list of surfaces, not of code: how a feature is implemented is in
the owning doc each row points to.

**Checked against phibrowser-mac `28df0d0d` (2026-11-10) and
phibrowser-win `f967816`, on 2026-11-10.**

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
| Double-click an ordinary tab to close it (Mac `9897f718`, 2026-10-30) | Advanced toggle, off by default; an unmodified double-click on a sidebar row, group member, split pane or strip cell closes that tab; pinned tabs and bookmarks keep their own double-click | Advanced toggle, off by default. An unmodified double-click on a sidebar row, group member, split pane (that pane only) or strip cell closes the tab, on the release with a 5 dip guard against a drag, docked and in the floating panel; refused while an agent holds the Space | ✅ | Done 2026-11-04, see §9 port 4. |
| Close the tabs above / below / left / right of a tab or group (Mac `b47255d6`, 2026-10-31) | tab and group context menus, titles follow the layout; Advanced toggle, on by default; a split counts as one unit and a whole group protects its range; pins and bookmark-bound tabs stay | Tab and group menus, titled by layout, greyed when empty; Advanced toggle, on by default; a split is one unit, a group protects its range, bound tabs stay. Close Other Tabs follows the same rules | ◐ | Done 2026-11-04, see §9 port 3. Not ported: `IDC_WINDOW_CLOSE_TABS_TO_RIGHT` routing, and tabs close one by one, so a whole group as a target closes with no group prompt. |
| Clear All Tabs with Undo (Mac `06bee451`, `af8702c3`, `ea18f661`, 2026-11-02) | File menu, rebindable ⇧⌘K, sidebar separator button; closes ordinary background tabs and keeps the foreground tab, its split, a visible Peek, pins and bookmark-bound tabs; Edit ▸ Undo and Redo per window bring the pages back unloaded with their order, groups and splits; a toast names the shortcut (`docs/architecture/clear-tabs.md`) | ⋯ menu row, rebindable Ctrl+Shift+K. Keeps the foreground tab, its split, pins and bound tabs. A toast's Undo button brings the pages back unloaded in one batch with their order, groups and splits; only removals Chromium confirmed return | ◐ | Done 2026-11-04, see §9 port 5. Left out by decision: the sidebar button, Edit ▸ Undo and Redo. The toast lasts 10 s and holds only the latest cleanup. Fork `RestoreTabs` export in `phi_framework_api.h`. |
| Pinned tabs: scope App / Profile / Space, split pins, rename, edit URL | yes | yes | ✅ | |
| Edit pinned tab icon | Edit… dialog has icon | Rename… / Edit URL… only | ◐ | No icon override on a pin. |
| Bookmark favicon actions: Separate From Bookmark / Back to Saved URL | row favicon menu | none | 🔧 | The "bookmark drifted from its saved URL" affordance. |
| Bookmark folder icon picker | yes | yes (24 symbols) | ✅ | |
| Close an open bookmark from its row (hover minus, Mac `7ec6e1b7`, 2026-10-27) | minus on hover while open | minus on hover while open | ✅ | Ported 2026-11-02, box-verified on nuc: `ShortcutRow::close_box`, a `fluent::kRemove` minus and a UIA button; a click closes the tab through `SidebarClose` and the bookmark stays. `kRemove` is new in `phi_fluent.h`. |
| Space bookmark root: a collapsible row named after the Space above its bookmarks (Mac `705af790`, `b457988a`, `2a0a49c2`, `0372e721`, 2026-11-02/03) | presentation only, no persisted parent changes; collapsed state kept per Space across relaunch, local; hidden with no bookmarks and in Incognito; a focused bookmark floats under a collapsed root; drops insert at the top level, and a collapsed root highlights as a target | A row named after the Space above its bookmarks, children one level in, collapsed state per Space in local preferences, hidden with no bookmarks, in Incognito, in an agent Space and in Comfortable. A collapsed root highlights as a drop target and takes the item at the top level; an expanded one opens the gap under it | ◐ | Done 2026-11-04, see §9 port 6. Differs by decision: no floating row; a collapsed root holding the active bookmark wears the hover wash and full-strength text. Folder expansion is still per shell and not persisted. |
| Drag & drop: tabs, pins, bookmarks, groups, cross-window, tear-off | full | ghost / gap / ring / badge / snap tiles / New window | ◐ | Deferred mechanics D31–D34: edge auto-scroll, Ctrl/Shift modifiers, whole-group drag, bookmarks bar as source/target, cross-window slot, OLE file drops, pip lift, hidden-sidebar reveal during drag. |
| Drop a file onto the sidebar | yes | none | ◐ | Part of D31 (OLE drops). |
| Spaces: create (name, icon, profile), rename, icon/emoji picker, theme, change profile, delete, reorder | yes | yes | ✅ | Picker lacks "Recently used" and skin tones (D24); create form keeps old control styling (D25). |
| Space switch slide animation, Ctrl+n, hover card | yes | yes | ✅ | Hover card restyled neutral (no Space tint) with the Mac 2026-10-27 (`5588d798`); Windows keeps its agent status line and follows the app appearance (the Mac forces light). A new Space lands right after the source Space on both sides. |
| Incognito Space | ⌃⇧N | Ctrl+Alt+N | ✅ | `incognito-space-port.md`; not ported: account-menu entry, scripting command, pre-warmed tree, new-pip chip flight. |
| URL rules (Domain / contains / suffix / URL → Space / Incognito / Kiosk / Ask) | yes | yes | ✅ | |
| Pinned tab scope follows the synced setting (per-account follower, 2026-10-14 Mac `05160468`) | yes | Windows syncs `PhiPinnedTabScope` | ✅ | Generation fence + commit / landing fences ported 2026-10-19 (§9); 2026-10-27 `parked_for_scope` keeps dormant other-scope copies out of "Needs attention" (`4d6a7bf2`). No "not applied yet" status line: Windows migrates the store and writes the key in one call. |
| Library (overlay / window: Space cards, pins, bookmarks, Downloads, Folio) | View ▸ Library; rail Folio / Spaces / Downloads | ⋯ ▸ Library, Ctrl+Shift+L, Settings ▸ Spaces ▸ Manage Spaces in Library, downloads popover ▸ All Downloads, profile menu ▸ Download List; rail Spaces / Downloads | ◐ | Built 2026-10-27 (`windows-native-ui-impl.md` Round L): scrim + 36-dip card over sidebar and content, Space cards (theme ground, icon picker, inline rename, theme panel, pins grid + bookmark outline, double-click opens, handle drag reorders, "…" menu, row context menus with Move to Space ▸), inline create card after the current Space (`space_created surface: library`), Downloads list (search, grouped, hover actions), standalone window, `library_section_viewed`. Gaps: no Folio section (Folio is being redesigned — deliberately omitted 2026-10-27); no drag of pins / bookmarks between cards (Move to Space ▸ stands in); no profile filter in Downloads (the framework lists per window, 20 rows); no spring flight / shadow blur. Rail and content grounds went neutral with the Mac's (`294d4d02`, 2026-11-02). |
| Space theme palette editor: colour field, 1–3 linked handles, harmony (single / analogous / complementary / split), Light / Dark buttons (Mac `3d045cf6`, `4c9d919e`, `23c728d9`, 2026-10-21/23) | replaces the swatch row + saturation slider in the Space editor and the create form; 2–3 colours paint the sidebar as a vertical Oklab gradient | same editor (`phi_space_theme_panel.cc`: 260×296 popover, 244 field, 38/28/14 handles, harmony + Light/Dark toolbar), the create form's "Color ›" row on a draft, local `spaceThemePalettes` record (`model::Space::palette`, `PhiThemePalette`) | ◐ | Ported 2026-10-27 (`windows-native-ui-impl.md` Round J), box-verified on gmk: Space editor light/dark, draft editor from the create form, a split-complementary palette on the chrome, preset choice drops the record, pinned-preset Spaces unchanged. Record is LOCAL on both sides (not on the Space sync payload), so a Mac palette Space still renders here as its pinned preset. 2026-10-28: 2–3 colours now paint the sidebar as the Mac's vertical Oklab gradient down the window's full height (`PhiPalette::ground_stops`, `PhiOklabGradient`): the sidebar is still painted on the flat primary ground and the shell regrades each pixel onto the row's gradient colour by its coverage over that ground (`PhiBackdropRowGrounds`, acrylic and opaque frames, the floating sidebar, the create form), so washes, labels and the active row keep their look over the secondary colours; the top bar and accent stay primary. Settings ▸ Spaces now has the Mac's single "Color ›" row (gradient dot + chevron) opening the editor beside it, in place of the eight dots and the saturation slider; since `d414679` Appearance has the same row, with a link to Spaces when there are several. Minor: handles are not named for UIA (one field hit, no "Color n" tooltip); the guest→account migration does not carry palettes; drag cost not measured against the Mac's `a6e3a27a` (cached field, frame-coalesced preview). 2026-11-02 (`a6e3a27a`): measured on nuc at 150 %, a field renders in 31 ms at 366 px and a drag preview costs 0.5 ms (median 446 µs, p95 829 µs, max 985 µs), so only the field became a process-wide cache and drags stay unthrottled. 2026-11-10: preset strip added (Mac `26eeac82`, `3fd3ad5c`): three pages of recommended palettes, saved colours (`savedThemePalettes`, local) with Save, Reset and a delete badge; checked on nuc. The slide between pages is not ported. |
| Group Overview page | yes | yes | ✅ | |
| Tab Switcher includes all Spaces (Mac `cde2a6a1`, `c8c96bca`, 2026-11-04/05) | Settings ▸ Spaces toggle, off by default; Ctrl+Tab lists recent tabs from every Space of the window and switches Space when needed; only Spaces with the same profile and Incognito state, never agent Spaces | the same toggle in Settings ▸ Spaces (`tabSwitcherIncludesAllSpaces`); the shell stamps each tab when it takes its Space's focus and again when the window leaves that Space, and the switcher merges by stamp | ✅ | Ported 2026-11-10, checked on nuc: off stays in the Space, on reaches the other Space's last tab and switches Space. Stamps start at launch, so tabs not seen since then follow in strip order. |
| Replace Saved URL with Current, on pins and bookmarks (Mac `0d8342c4`, 2026-11-09) | "Edit Pinned Tab ▸" and "Edit Bookmark ▸" submenus hold Edit URL and the new row; enabled when the item is open, not a split, not Incognito and its page has left the saved URL | the same two submenus and rule; it saves only, the page stays | ✅ | Ported 2026-11-10, checked on nuc on a pinned cell. The bookmark row shares the code and was not exercised. |

## 3. Address bar and page

| Feature | Mac | Windows | Status | Gap / decision |
|---|---|---|---|---|
| Omnibox with suggestions, keyword chip ("Tab to search") | yes | yes | ✅ | Under the Omnibox new-tab behaviour an EMPTY window (Ctrl+N, last tab closed) shows the placeholder with the field focused instead of a new-tab tab, as on the Mac (`936d090e`, ported 2026-10-27, fork + client). Since 2026-10-28 the Mac's centred floating omnibox exists too (`phi_floating_omnibox.{h,cc}`, a `PhiPopover` whose field joins `PhiOmniboxSession`): under the Omnibox new-tab behaviour Ctrl+T, the New Tab rows and an empty window open it over the page, suggestions drop flush under it, the keyword chip works in it, a commit opens a new tab (or the empty window's first); Escape, a click outside, or the user's New Tab / Close Tab chord close it. The empty window's placeholder under it is blank (the native new-tab page mounts with its controls hidden, `NativePagesBlankPlaceholder`), and switching to a Space whose window is already empty opens the card too (the Mac's `emptyLiveSpaceOpensOmniBox`). A window that is not in front when it empties focuses the address field instead and does not open the card later (the Mac owes it until the window is presented). The Mac went blank for new windows and empty Spaces on 2026-11-02 (`0c4f1552`) but still shows its game after a last-tab close; Windows is blank in all three. |
| "Hold ⌘ to switch" to an already-open tab | yes | "Hold Ctrl to switch →" | ✅ | Ported 2026-10-28 with the rest of the Mac `OmniBoxViewModel` / `OmniBoxTextField` logic (fork `OpenOmniboxMatch` takes an explicit disposition, `DeleteOmniboxMatch`, `StartOmniboxQuery(prevent_inline)`, `swap` in the results json; client `phi_omnibox_session.{h,cc}`): a row with an open tab opens in the current tab unless Ctrl is held (Enter or click), where it used to switch silently; inline autocompletion of the default row (selected, typed over, dropped on Backspace / Delete); the default row armed when the rows answer the typed text, an arrowed pick kept across streamed updates, stale result sets dropped; Shift+Delete and a hover × forget deletable rows; history rows show their favicon; Chromium's swapped rows, pedals and empty rows filtered; chrome:// reads phi:// in the field and the rows; the keyword chip wears the seeded engines' brand colours. Not ported: a bookmark / pinned row's tab opening address-bar submissions in a NEW tab (`3d3719b1`). |
| Site menu: security row, Content blocking on this site, Always show URL path, Site settings, Clear cache / cookie, Extensions ▸, Share ▸ | yes | all but Share ▸ and Extensions ▸ | ◐ | Share: see §1. Extensions live in the field's puzzle button instead (design). Always show URL path drives the field since 2026-11-04, default off as on the Mac: off shows the host alone, on adds the port, path, query and fragment, and neither shows the scheme, `www.` or trailing slashes. Editing and copying use the whole URL. Unlike the Mac's on state, `data:`, `blob:`, `view-source:` and `file:///` URLs stay whole. |
| Site menu: Collect Site Memories / Delete Site Memories | yes | none | ⛔ | AI suite (site memory is browser memory). |
| Copy link in the field, Reader button (hover) | yes | yes | ✅ | Copies the presented Peek's URL (the Mac's `d94d3329`), through `RebuildNavState` and `nav_url_`. |
| Reader View (typography prefs) | yes | yes + read-aloud HUD | ✅ | Windows has read-aloud; Mac does not. `-phi-auto-reader` automation entry point not ported (analytics doc). |
| Peek (Shift-click, auto-peek from pins/bookmarks) | chips: Close, Open as Tab, Open as Split, Reader View | chips: Close, Open as Tab, Open as Split | ◐ | No Reader chip on the Peek card; Peek is omitted from session restore (`analytics.md`). Page commands and the address field aim at the presented Peek since 2026-10-27 (`a19624bb`, `c11849cf`; Back / Forward and the Reader toggle stay on the opener). The Shift-click SPA-router guard + opener video pause landed 2026-10-27 (`80f92213`: fork `phi_peek_link_guard.{h,cc}` runs the Mac's capture-phase click guard in a Phi isolated world of every document, gated on `peekViewEnabled`; `OpenUrlAsPeek` pauses the opener's videos in every frame). Still missing: appear / close flights and the opener dim. The Mac turned its guard and the video pause OFF on 2026-10-27 (`3a6cbf2e`, for the CDP session it retained per tab; still off at `8c0b8596`). Windows keeps its version, which holds no DevTools session, so Windows does more than the Mac here until the Mac re-enables it. |
| Split view (two panes, drop hints, swap, convert, pin/bookmark a split) | yes | yes | ✅ | `split-view-chrome.md`. |
| Splits of three or four panes (Mac `b3a379e3`, 2026-11-05) | up to four panes with a share each; add, remove and move a pane; drop hints for add left / right / top / bottom / between and replace; pinned and bookmarked splits of three or four (LocalStore V14 columns `splitPaneGuids`, `splitExtraPanes`, local only) | up to four panes with a share each, laid out by the fork; one merged sidebar row and strip cell; Split with Current, Alt-click and Split with New Tab add a pane; page drops insert or replace; row menu and hover chip move, remove and close panes; `splitView.*` verbs take `tabIds` and `fractions` | ◐ | Built 2026-11-10 (`split-view-chrome.md`, "Three and four panes"), checked end to end on nuc. Pinned, bookmarked and duplicated splits of three or four work too (one optional `splitExtraPanes` column, store format still 13; a pinned split keeps its later panes on its first row, unlike the Mac's `splitPaneGuids`). Missing: the hover preview and drag card beyond two panes, and per-pane rows in a stored cell's own menu. No pane move handle by decision: the hover chip and the row menu move panes. |
| Edge-to-edge web content (Mac `35f4d29c`, `0eceaa08`, 2026-11-05) | General ▸ Appearance toggle: no outer page margins in every layout, square corners in full screen, a soft page shadow on the sidebar | the page has no outer margin in any layout (`kContentMargin` 0, `phi_shell_layout.h`) | ⛔ | Decided 2026-11-10: nothing to switch. The Windows page card already runs to the window's edges (`windows-native-ui.md`), so the Mac's option would have one state. |
| Split with New Tab (Tab menu, rebindable shortcut, omnibox pane picker, Hold to add an open tab; Mac `6985cc34`, 2026-10-28) | yes | ⋯ row, Ctrl+Shift+\\ (rebindable, group Tab), New Tab Page mode, Omnibox mode with Hold to add an open tab | ✅ | Ported 2026-11-02. The Mac's Ctrl+Shift+= is zoom-in on Windows, so the chord is Ctrl+Shift+\\. No blank-pane preview, by decision: it has no twin under a hosted HWND. Box-verified on nuc: New Tab Page mode splits, a second chord does nothing, an Omnibox card commit splits. ⋯ row: splits in New Tab Page mode, disabled and refused on a split tab, and from the real popup opens the Omnibox card, whose commit splits. Hold: on a row with an open tab, Ctrl+Enter pairs that tab with no third tab, plain Enter opens a fresh pane. The "Hold Ctrl" hint on the row was not looked at. |
| Modifier clicks: Shift → Peek, Alt/⌥ → split, Ctrl+Alt/⌘⌥ → Kiosk | yes | yes | ✅ | |
| Kiosk window ("Open in" Space, profile chip) | yes, profile chip menu, cursor-zoom open animation, frame autosave | yes, no profile switch from the chip, default size | ◐ | `phi_kiosk_window.h` lists the four pieces as deliberately not ported; the profile-chip switch has no framework call. The page takes keyboard focus on open (`79b11061`, 2026-11-02, see §9). "Open in" reopens the page by URL rather than moving the live tab, and the Kiosk closes only once the page has opened (fixed 2026-11-02: with no user window it used to lose the page and exit Chromium, `74cb2bce`). With no user window the last session is restored first when that setting is on, as on the Mac. |
| Native new-tab page | Phi mark + omnibox, AI suggestions | Phi mark + omnibox | ✅ | AI suggestions ⛔ AI suite. |
| One default surface for the address row, active strip tab, page backing and split gaps (Mac `5e434655`, 2026-10-28) | `extensionSurfaceBackground`: white, or in dark the overlay colour at 20 % | one dark `window_bg`, #2B2B2B, for the address row, strip tab and page backing; Library grounds use the same surfaces | ✅ | Ported 2026-11-02 at #2B2B2B, the value `windows-native-ui.md` §2.3 argues for, not the Mac's near-black. Box-verified on nuc: Mist dark read #0B2938 beside #2B2B2B before, #2B2B2B everywhere after. Library grounds are neutral too, see `294d4d02` in §9. |
| Bookmark manager (native, per Space) | yes | yes | ✅ | |
| Bookmarks bar (always / on new tab) | yes | yes | ✅ | |
| Image preview window (zoom, export original — Mac `1705cb23`, 2026-10-15) | `UserInterface/ImagePreview` | none | 🔧 | Whole surface absent. New on the Mac this cycle. |
| Picture-in-picture: auto Off / Normal / Park at edge | yes | yes | ✅ | `picture-in-picture.md`. |
| Widevine DRM (Netflix, Spotify, Disney+ …) | fork only: component-updated CDM, `Phi Helper (CDM)` signed without library validation, VMP host files (`Phi Framework` + castLabs EVS `.sig`) | component-updated CDM; VMP host files `Phi.exe` + `framework\chrome.dll`, `.sig` beside each from `make-installer.sh --vmp` (EVS, after Authenticode; flag 1 on the exe, flag 0 on the DLL) | ◐ | Ported 2026-10-30 (fork `cdm_host_file_path.cc` + `media_options.gni`, ship args `enable_widevine`). No CDM helper needed (a macOS library-validation matter). Official releases VMP-sign (build repo `vmp-sign.sh`). Official 0.4.0.13 x64, installed on nuc on 2026-11-05, passes `box-tests/suite-widevine.mjs` 5/5: CDM 4.10.3112.0 fetched by the component updater, license request, Shaka Widevine playback, host verification ran, and Google's UAT license server answers `PLATFORM_SOFTWARE_VERIFIED`. Spotify plays on official 0.4.0.14 x64. On arm64, 0.4.0.14 and earlier crashed every web page (Aw, Snap, STATUS_BREAKPOINT) on GitHub's hosted Cobalt 100 VM: the Linux cross-build's snapshot toolchain built V8's builtins with pointer authentication that chrome.dll's V8 couldn't read. Fixed in 0.4.0.15 (build repo `args/PhiFxWinShipArm64.gn`), which passes the page check and the Widevine suite 6/6 on a Cobalt 100 VM (build repo `arm-smoke.yml`, CI.md). Real Arm hardware not checked. Test releases stay unsigned, so services that demand VMP refuse them a license. |
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
| You / Account: profile card, randomize, change name, avatar, sign in/out, default browser, share card | yes | all; Randomize is a text button above Download image | ✅ | Randomize ported 2026-11-02 (`92816ab6`), box-verified: the card changes and its style saves, the theme stays. The Mac's is a dice; Windows uses its native text button. |
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
| Advanced: restore session, full URL, previews, indicators, shorten links, Mini Player, auto PiP, developer mode, Chromium settings | yes | all but Shorten links (⛔) and Mini Player (🔧) | ◐ | Rows regrouped into Appearance / General on Windows. The full URL row (under Appearance) drives the address field since 2026-11-04; see §3. The Mac's new tab-actions card (double-click to close, positional closing) belongs to the §2 rows of those names. |
| Developer: Allow agents (CDP), per-agent Allowed / Blocked lists, "Allow all agents" | yes | one switch | ⛔ | No code-signature peer identity on the Windows pipe (`phi_settings_window.cc`, `phi_control_server.cc`). Revisit if the pipe gains a caller identity. |
| Developer: Allow agents to operate Spaces, operated-page rendering, profiles agents can use, install skill | yes | yes (no "exact rendering" toggle) | ◐ | The operated-page mask toggle has no Windows row. Ported 2026-11-04 (Mac `c3149697`, §9): the switch now also gates shadow windows, and turning it off closes them. Still different from the Mac on purpose: it is off by default, credential verbs come with admission, and the description says so. |
| Developer: Agent Password Manager (Bitwarden sign in / unlock / timeout), Agent Credential Approvals (standing grants, revoke) | yes, `docs/bitwarden-password-manager.md` | helper process + `credentials.*` verbs + Allow once / 10 min prompt; no Settings rows | ◐ | `PhiBitwardenHelper.exe` and the broker are in; the sign-in sheet, session settings, approvals list and "Always" grants are not. `analytics.md` records `approval_type: always` and `kind: run` as not ported. |
| About page | About window | About page + notice | ✅ | |

## 5. Onboarding, account, data

| Feature | Mac | Windows | Status | Gap / decision |
|---|---|---|---|---|
| Onboarding: welcome, name, sign in / guest, consent, layout, next steps | yes | yes, with Windows-native footer buttons | ✅ | `login-port.md`. The Mac's 2026-10-28 refresh (`28c0618b`) ported 2026-11-02: Back on every later page with Escape as Back, "Sign in instead", the legal wording, Up and Down on the layout list, the email-derived name prefill (`1cf61180`), Omnibox as the guest default (`5196706b`). Decided: footer buttons stay Windows-native rather than the Mac's pills, and the guest list leaves out AI features that are off on Windows. Next Steps still says "Onboard your AI assistant", which the Mac keeps too. Box-verified on nuc, signed in and as a guest. |
| Onboarding keyboard and screen reader (Mac `8d998ebc`, 2026-10-30) | VoiceOver reads page titles, steps and a card summary, and hears page changes | Tab moves through drawn controls with a focus ring, Space and Enter activate them, page titles, bullets and steps are UIA text, the card and name field are named, page changes are announced | ✅ | Ported 2026-11-02, box-verified on nuc through UIA and real keys on every page: the consent pages finish keyboard-only and the consent row's name is right. No Narrator run. |
| Onboarding: browser import step | Chrome / Safari / Arc / Dia / file | "Import browser data" link under Next steps | ◐ | A dedicated step with source picker is missing; see §1 import row. |
| Onboarding: password manager choice (iCloud Passwords) | yes | skipped | — | iCloud Passwords is Mac-only (`phi_onboarding_window.cc`). A Windows equivalent (Windows Hello / Credential Manager) would be a new decision. |
| Guest mode + guest → account migration | yes | built, not live-tested | ◐ | `user-data-parity.md` S6. |
| Hosted login (Auth0), reauthentication, sign out | yes | yes, off-the-record sign-in window | ✅ | |
| Sync keys: recovery code, device approval, profile / Space matching, sync method (Merge or Use your account's data, Mac `40312627`) | yes | profiles page only | ◐ | Corrected 2026-10-27: the Windows pairing wizard has no Spaces step and no method / replace step (`user-data-parity.md` S5 "Not yet"). No wire change — a Windows peer sees a Mac join as before. The recovery-code confirmation page, its durable pending flag (Mac `32799547`) and its Back (`a8a8f23e`) ported 2026-11-02, see §9. The warning about losing the code and the "Forgot your recovery code?" Help link (`a7ac1c07`) ported 2026-11-04, see §9; the Help article itself names only the Mac. |
| Crash feedback ("Phi quit unexpectedly") on next launch | `docs/crash-feedback.md` | Crashpad → Sentry, no prompt | 🔧 | Needs the last-run status from Crashpad and the existing Send feedback form with the crash id. |
| Send feedback: files, pasted images, diagnostics, outbox | 10 × 20 MiB, Sentinel logs | 5 × 10 MB, week-long outbox | ✅ | Sentinel logs are ⛔ AI suite. |
| Sync problem telemetry (Mac `9921ac9a`, 2026-11-03): `sync_attention_entered` / `_snapshot` / `_cleared`, held counts by reason, sync counts in the launch snapshot, sync keys in feedback | PostHog, Sentry and feedback, enum values and counts only (Mac `docs/sync.md`, "Sync status contract") | `user_defaults_snapshot` carries `sync_enabled` and the three counts from a local mirror; feedback carries `sync_phase`. No `sync_attention_*` events and no attention keys in feedback | ◐ | Partly done 2026-11-04, see §9 port 7. The reporter was not built: this engine has no held counts by reason, no problem categories and none of the helper conditions. No Sentry half. |
| Account deletion local data removal | yes | none | 🔧 | With the Delete account row above. |
| Sign out discards the session (Mac `8655ec6b`, 2026-11-09) | Sign out marks a discard before closing windows; the next launch or the next sign-in or Guest entry empties Session restore and Undo Close for every profile, so the next account does not get the previous account's windows | Sign out marks the same discard (`MarkSignOutSessionDiscard`). Windows keeps its windows hidden behind the login gate rather than closing them, so at the next sign-in or Guest entry it closes every tab they hold, then consumes the mark (`PhiCoordinator::DiscardSignedOutSession`) | ◐ | Built 2026-11-10, not run: the test profile has no signed-in session. Before this, the next account got the previous account's open tabs back in the same run. Framework ABI 3. |
| Membership card: holographic foil card with member ID (SHA-256(sub)[0:8]), "Date Joined", random style + PNG export, on the onboarding Welcome page and Settings ▸ You (Mac `6b300dc0`, `cda12eaf`, 2026-10-20/26) | `HolographicCardView` | `PhiHolographicCard` (`client/phi_holographic_card.cc`): the same layer stack composed in software — palette base + iridescence + radial, one of 6 procedural foil textures, seeded grain, logo + sweep, one of 6 seal ornaments, sheen, edge; pointer light + tilt, entrance spin, flat PNG export at 2× on the accent canvas | ◐ | Ported 2026-10-27 (`windows-native-ui-impl.md` Round K); the Mac has moved since, see the end of this row. Foil / grain / seal masks are rasterised once per style and size with GDI+ on a worker and cached as PNG under `<user data>\PhiCardArtwork\v1`; the light-driven layers recompose per frame. Member id from `auth().user().sub`, join month from the profile's `created_at`, style in the account defaults under the Mac's `profileCardStyle` key (the Settings card keeps its own palette since 2026-11-02, `40ede7a7`). The conch frame sets (12.5 MB) are gone. Not ported: the Mac's `.random()` seeding details (both are uniform), IvyPresto Headline Light now ships for the name. Mac 2026-10-27/29 follow-ups ported 2026-11-02 (`92816ab6`, `40ede7a7`, `ba180400`, `1cf61180`, `8fb61ef5`): no radial highlight, a dark logo at rest, no printed member ID, no shadow in Settings, a style saved as soon as it is randomized. 2026-11-10: look refreshed to Mac `0b1ee030` (foil on both faces, hairline edge, "Member Since" in Share Tech Mono, a random style always has an ornament); built, not looked at on a box. |

## 6. Agents and automation

| Feature | Mac | Windows | Status | Gap / decision |
|---|---|---|---|---|
| Agent Spaces: overlay, fog, ring, pill (Take control / Hand back / Finish), "The agent needs you", Autoview | yes | yes | ✅ | Agent cues D26–D30 deferred: brand accent per agent, pulsing ring, window title, ⋯ on the capsule, omnibox dim/eye. |
| Agent viewport follows the agent Space's own page area (Mac `1c39d48c`, 2026-10-29) | `agentSpace.panelSize` answers for the aimed Space, re-measured before each action | same; `VerbAgentPanelSize` answers in CSS pixels for the shell hosting the watched Space, else the focused user window, else the parked agent shell | ✅ | Ported 2026-11-02, box-verified on nuc at 300 % and 150 %: an agent tab matches a real tab's size hidden and watched, and follows sidebar and window changes at the next action. Before, the skill's un-aimed call was refused (`scope_not_allowed`), so agent pages laid out at the parked shell's fixed frame (1027×684 at 300 %, 1386×849 at 150 %) whatever the user's window was. The skill names the task, not the Mac's `windowId`, which resolves to the user's shell while watched. Hand-off fixed the same day, see `1c39d48c` in §9: an exact-size override made Chromium restore the hidden frame's size on clear (fork `web_contents_impl.cc`), and the pill's Take control left the page pinned until the agent exited (skill `ownershipChanged`). Also verified: several windows of different sizes, a visible bookmark bar, the Horizontal layout, no user window. |
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

## 7. Summary (2026-11-10)

| Status | Rows |
|---|---|
| ✅ ported | 61 |
| ◐ partial | 28 |
| 🔧 to implement | 24 |
| ⛔ deliberately left | 20 |
| ❓ needs a decision | 0 |
| — not applicable | 4 |

**The 🔧 list, roughly by user impact:** Mini Player / media controls ·
content blocking switches · multi-selection ·
Bitwarden Settings rows and standing grants ·
Time Machine-style snapshots + restore · crash feedback prompt · image
preview · Arc/Zen migration wizard · Share · Capture Full Page ·
Export/Import user data · account export + deletion · invitation code
(backend) · full screen · client-side
external-link hook (Kiosk preference, App rules, return to the sending
app) · `agentSpace.app.info` · tab audio indicator ·
bookmark Separate/Back-to-saved ·
What's new / Documentation / Improve
translations rows · Export logs / Extension info rows · settings deeplink ·
sync problem telemetry.

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
above and prints every Mac commit since, up to `origin/dev` (`PHI_MAC_REF`
overrides it; `git fetch origin` in the Mac checkout first, which may sit behind), grouped `feat` → `fix/improve` →
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

### 2026-11-10 — phibrowser-mac `df894346` → `28df0d0d` (64 commits, 3 merges)

Read through the diffs and the Windows code; nothing was built or run, and no port is
started. Eleven commits are release-branch copies of others in the range and share a row.
Five new 🔧 rows: splits of three or four panes and edge-to-edge content (§3), Tab Switcher
across Spaces and Replace Saved URL (§2), sign-out session discard (§5).

**Port recommendations, in order of user impact.** S under half a day, M about a day, L
several days with box time.

1. `8655ec6b` **sign-out session discard.** M. Export the fork's mark and consume and call
   them on sign out, sign in and Guest entry.
2. `b3a379e3` **splits of up to four panes.** L. The largest Mac change of the cycle.
   Done 2026-11-10 (§3 row), stored splits included.
3. `cde2a6a1` + `c8c96bca` **Tab Switcher across Spaces.** M.
4. `0d8342c4` **Replace Saved URL with Current.** S.
5. `35f4d29c` + `0eceaa08` **edge-to-edge web content.** M.
6. `26eeac82` + `e9a3fe13` + `3fd3ad5c` **theme editor presets and saved colours.** M.
7. `0b1ee030` **membership card look.** M. Open since 2026-11-04.
8. `1ecc82b3` + `073b616c` **omnibox word selection and sideways scrolling.** S each.
9. `074b9760` **page colour crossfade.** S.
10. `422c08c0` **sidebar gradient through a Space switch.** S, check first.
11. Small fixes: `01dafe08`, `1e890904`, `fd7357d1`, `a910dca8`, `d2497fae`, `0ad6c4ae`.

**Decisions wanted:** the Time Machine row (the Mac hid the feature, `81602df9`), the
Kiosk launch omnibox (`ebdbb259`), and the About conch (`094e85c8`).

**Results, 2026-11-10.** Ports 1 to 9 and the small fixes are in the tree; where a line
says "nuc" it was run there by real input or over the control pipe.

* 1 sign-out discard: built, not run (no signed-in test session).
* 2 splits of up to four panes: done, see the §3 row.
* 3 Tab Switcher across Spaces, 4 Replace Saved URL: done, nuc.
* 5 edge-to-edge: nothing to port, the Windows page has no outer margin (§3, ⛔).
* 6 theme editor presets: three pages of recommended palettes, saved colours with
  Save, Reset and a delete badge, paging that stops at each end. nuc, by screenshots.
  The Mac's slide between pages is not ported.
* 7 membership card: both faces carry the foil, a dark hairline edge, the name pressed
  in with a lower highlight, "Member Since" with the date in Share Tech Mono (the font
  and its OFL licence ship in `resources\fonts`), and a random style always has an
  ornament. Built, not looked at: the card needs a signed-in onboarding.
* 8 omnibox: a double-click selects one part of an address (a number keeps its dots).
  nuc. Sideways wheel scrolling is not ported: a single-line EDIT scrolls only by
  moving its caret. The wheel no longer falls through the field to the page behind.
* 9 page colour crossfade: a quarter of a second on a same-tab change. nuc.
* 10 sidebar gradient through a Space switch: already there (`LerpPalette` blends the
  gradient stops; a flat side repeats its colour).
* Small fixes: `01dafe08` nuc; `1e890904` and `fd7357d1` by reading only.
* `a910dca8`: the Kiosk opened centred on the primary display whatever display the user
  was on. It now opens on the display of the Phi window under the pointer, else the one
  with the keyboard. Built, not run: nuc has one display.
* `d2497fae` and `0ad6c4ae` were not checked.

| Mac commit | Subject | Verdict | Windows action / reason |
|---|---|---|---|
| `e24c450a` | feat: open Phi Chat's Private AI settings from the AI settings row | skip | ⛔ AI suite. |
| `422c08c0` | fix: crossfade the sidebar's navigation gradient through a Space switch instead of dropping it | done | `LerpPalette` (`phi_spaces_strip.cc`) already blends the gradient stops through a Space switch. |
| `860d24e7` | fix: show the arrow cursor over Next steps consent text and a hand over its links | done | Copy of `da5da806`, port 9 of 2026-11-04. |
| `84bfeaf3` | fix: let Tab reach OOBE footer buttons and follow the Keyboard navigation setting | done | Copy of `597d76b5`, triaged 2026-11-03. |
| `d2497fae` | fix: draw the omnibox and tab search above the floating sidebar they open from | port | S, check first. Open the floating omnibox and Tab Search from the floating sidebar and see which window is on top. |
| `a910dca8` | fix: open Kiosk on the display the user acted on with multiple displays | port | S, check first. `PhiKioskWindow` opens at a default size; confirm which monitor it picks when the browser is on another one. |
| `074b9760` | improve: crossfade the header, active tab and bookmark bar when the page color changes | port | S. The address row, strip tab and bookmark bar snap today. 0.25 s on a same-tab change only; a tab switch still snaps. |
| `c27df341` | fix: focus a Kiosk page opened from an in-app link | done | A workaround for an AppKit first responder. Windows focuses the Kiosk page on open (`79b11061`, §3). |
| `02dd39fa` | fix: zoom the window on titlebar double-click on macOS 27.2 | skip | macOS titlebar. |
| `1ecc82b3`, `da25f095` | Fix URL word selection in the omnibox | port | S. Double-click and Ctrl+arrow in the address EDIT use the system word breaks; the Mac now follows Chromium's URL boundaries. A word-break procedure on the field, as `phi_text_wrap.cc` does for wrapping. |
| `073b616c` | Fix horizontal scrolling in the omnibox | port | Copy of `345e60b0`, open since 2026-11-04. Neither omnibox file handles `WM_MOUSEHWHEEL`. |
| `0b1ee030` | fix: refine shared membership card appearance | port | Copy of `df894346`, open since 2026-11-04. |
| `26eeac82`, `22625592` | Color picker presets | port | M. `phi_space_theme_panel.cc`: pages of curated palettes, saved colours with save, remove and reset, previous and next buttons. Also carries the membership card change above. |
| `e9a3fe13`, `29fa0c35` | improve: stop rebuilding preset swatch gradients on every drag frame | watch | With the presets. |
| `3fd3ad5c`, `eab1a3c3` | feat: stop preset paging from wrapping and slide between pages | watch | With the presets. |
| `70bdaeec`, `2feb550b` | fix: recover guest entry after keychain owner mismatch | skip | macOS keychain. |
| `35f4d29c`, `5967630b` | feat: add an edge-to-edge web content option | skip | The Windows page has no outer margin; §3 row, ⛔. |
| `0eceaa08`, `c533a6d3` | fix: return the page edge shadow only after the Peek restore animation | skip | With edge-to-edge. |
| `cdd17bc4` | i18n: theme editor palette controls, edge-to-edge content setting and the member since card label | watch | Regenerate the catalog when those ports land. |
| `fd7357d1`, `277dd871` | fix: make screenshotBrowser fail when the page capture times out | port | S. The Windows skill's `helpers.mjs` has the same swallow-everything `catch`; take the change and the SKILL.md line. |
| `e114075d`, `3ac3c2c9` | fix: keep a launch with its own data directory from taking over the running browser's agent socket | skip | The Windows pipe name carries the process id and a random suffix, so two instances never share one. |
| `01dafe08` | fix: disable Clear All Tabs when the only normal tabs form one split | port | S. `CanClearAllTabs` counts raw ordinary tabs, so a lone split leaves the row enabled and `ClearAllTabs` then does nothing. |
| `0ad6c4ae` | fix: show the drop indicator when dragging a folder between top-level folders | port | S, check first. Windows has the Space root row since 2026-11-04; drag a folder beside a collapsed sibling and look for the gap. |
| `5142015f`, `4ffbbe39`, `441030bb` | xcstrings order and stale strings | skip | Catalog housekeeping. A removed key the Windows catalog still uses shows up at the next regeneration. |
| `f6a9a59f`, `aabf38a0` | Auth0 credentials to the data protection keychain | skip | macOS keychain. |
| `aff124e6`, `8295a1e8` | docs: bridge header comments | skip | Comments. |
| `62e549a0` | feat: add the setting for PiP on other apps' fullscreen desktops | skip | macOS Spaces and full screen desktops. |
| `f77afd55` | feat: grey out Phi menus while only a PiP is on the desktop | skip | macOS menu bar. |
| `d12d4bbd`, `7bfc1239`, `4c7f766e`, `e123af5e`, `3c405222` | custom Telegram bots | skip | Phi Link, ⛔ AI suite. |
| `cde2a6a1` | feat: add a setting to let the Tab Switcher include all Spaces | port | M. New §2 row. |
| `c8c96bca` | fix: keep all-Spaces Tab Switcher recency across Space switches, limit it to matching Spaces, and support empty Spaces | port | With `cde2a6a1`. |
| `094e85c8` | Add interactive conch logo to About Phi | watch | Needs a decision: Windows dropped the conch frame sets (12.5 MB) on 2026-11-02. |
| `22fbb87d` | fix: stop the system beep when typing with focus outside web content | skip | AppKit responder chain. |
| `81602df9` | feat: hide Time Machine backups and purge existing snapshots on launch | watch | The Mac turned the feature off. The §1 Time Machine row is 🔧 by the 2026-10-19 decision and needs a new one. |
| `8655ec6b`, `0246e720` | feat: mark the session discard on sign-out, consume it on sign-in | port | M. New §5 row. |
| `b3a379e3` | feat: support splits of up to four tabs, including pinned tabs and bookmarks | port | L. New §3 row. |
| `1e890904` | fix: install the Grok Bot skill link into the common agents folder | port | S. `phi_skill_installer.cc` links Grok Bot to `.grok\skills`; it should be `.agents\skills`. Update the comment and the skill's `install.md`. |
| `776f80d3` | fix: vendor a Bitwarden helper that doesn't embed the builder's home directory | skip | The Mac's vendored binary. Not checked: whether `PhiBitwardenHelper.exe` embeds a build path. |
| `0d8342c4` | feat: replace saved pin and bookmark URLs with current URL | port | S. New §2 row. |
| `ebdbb259` | Kiosk: ask for the URL in a Spotlight-style omnibox, and polish window placement and focus | watch | The launch omnibox serves the global Kiosk shortcut, which is ⛔ on Windows (§1), and the Mac's New Kiosk menu item. Windows has no New Kiosk row. Needs a decision. |
| `3ffc5d1b` | fix: keep the sidebar toggle button visible after pinning or unpinning the sidebar | skip | Performance layout header and traffic lights. |
| `8d5b0c8c` | fix: show the Update button beside the sidebar button in Performance layout | skip | Performance layout, removed on Windows. |
| `28df0d0d` | i18n: four-pane split hints and pane menu items, Tab Switcher scope setting, saved URL replace, About conch labels | watch | Regenerate the catalog when those ports land. |

### 2026-11-04 — phibrowser-mac `597d76b5` → `df894346` (3 commits)

No new feature in this range, so no new end-to-end test is due until one of these is
ported. Each port gets its test first (the rule from 2026-11-04). Read through the
diffs only; nothing was built or run. Ports not started.

**End-to-end suites added 2026-11-04 for that day's work:** `tabclose`, `cleartabs`,
`spaceroot`, `synctelemetry`, `toast`, `floatingomnibox`, `urlpath`. Each failed on a client
with the feature broken or missing. Full sweep on nuc at 300 %, `main` `7bb1bf6`, fork
`2d0815acd18`: 243 passed, 8 failed, 39 suites, all 8 the known nuc pixel and bounds
failures (`layout`, `settings`, `tabui` x3, `theme`, `toolbar`, `window`). `suite-feedback` and
two-box sync not run, and the new suites have not run on gmk.

| Mac commit | Subject | Verdict | Windows action / reason |
|---|---|---|---|
| `0776b43f` | fix: admit the browser runner a companion app spawns as an agent it vouches for instead of refusing it as Phi's own orphaned runtime | skip | `AgentPeerIdentity` and companion passes. Windows has neither: the pipe gate is the switch alone, and companion apps are ⛔ (§6, decided 2026-11-02). |
| `345e60b0` | Fix horizontal scrolling in the omnibox | port | S. The Mac scrolls the field with a sideways wheel and keeps wheel events from reaching what is behind the card. Whether the Windows EDIT and floating card do either was not checked. |
| `df894346` | fix: refine shared membership card appearance | port | M. `phi_holographic_card.cc` is the twin: back face drawn like the front, black edge, taller name, "Member Since" with the date in Share Tech Mono (a new font and licence to ship), and no random style without an ornament. |

### 2026-11-03 — phibrowser-mac `8c0b8596` → `597d76b5` (33 commits, no merges)

**Port recommendations this pass (16 commits in 10 ports), in order of user
impact. Ports 1, 2, 8, 9 and 10 done 2026-11-04; ports 3 to 6 done and port 7 in
part later that day; see the two Results blocks below.** Effort is an estimate: S under half a day, M about a
day, L several days with box time.

1. `c3149697` **what the agent Spaces switch allows.** S, one decision. The
   Mac's description now tells users the switch also covers driving their
   windows over CDP, invisible shadow windows that can use a profile's logins,
   and credential requests. The Windows gate is narrower. `PhiCapability` puts
   shadow windows under `kAgentSpace`, admission always grants the credential
   capabilities, and the switch adds only `kUserSpaceRead` and `kUserSpaceWrite`.
   It is off by default on purpose (`phi_app_model.h`; `user-data-parity.md`
   still says true). So with the switch off, an admitted agent can open a hidden
   window in any profile "Profiles agents can use" allows, while the Windows
   skill's `references/lifecycle.md` says every shadow call fails
   `user_space_operations_disabled`. From the code; not run. **Decide:** gate
   `agentSpace.shadow.*` behind the switch (recommended: it is what the skill
   promises, and the window is the part a user cannot see), or keep the gate and
   correct the skill. Credentials need their own answer, because gating them
   with the default off would stop fills for everyone who has not opted in.
   Either way rewrite `settings.developer.agentBrowsingDataAccess.descriptionWindows`
   in every locale. The Mac's text does not fit as it stands ("On by default").
2. `a7ac1c07` **recovery code warning and Help link.** S. In
   `PhiSyncSetupWindow`, add the loss warning to `Page::kShowingCode` and the
   "Forgot your recovery code?" link to `Page::kEnterCode` (not the confirmation
   page). The Mac's warning says "no Mac", so it needs a `…Windows` key. The
   Safari fallback has no twin: nothing in that window is modal, so a default
   browser or a Phi tab can show the article. Whether the Help article reads
   correctly for Windows was not checked.
3. `b47255d6` **positional tab closing.** M. New rows after `tab.closeOthers` in
   `PhiBuildTabRowMenu` and after `group.close` in `PhiBuildGroupHeaderMenu`,
   with actions beside `tab.closeOthers` in `phi_shell_window.cc` (the same
   collect-ids-then-`CloseTabById` loop). Left / right in Horizontal, above /
   below in Vertical. Take the Mac's rules: strip order including collapsed
   members, a split as one unit, a whole group as the anchor, bound tabs excluded.
   Toggle in `BuildAdvanced`, on by default; the Mac does not sync it.
4. `9897f718` **double-click to close.** S. Toggle in `BuildAdvanced`, off by
   default; the Mac does not sync it. In the shell's `WM_LBUTTONDBLCLK`, after
   group headers, an unmodified second press on an ordinary row, pane or strip
   cell closes that tab, refused while an agent owns the Space. The message
   arrives on the press where the Mac acts on release with a 5 pt drag guard, so
   the port has to decide against a drag starting from that press. Shares its
   Advanced card with 3; land them together.
5. `06bee451` + `af8702c3` + `ea18f661` **Clear All Tabs with Undo.** L, two
   decisions. A new `kCmdClearAllTabs`, a ⋯ row and a `kRebindableShortcuts` row
   at Ctrl+Shift+K (free in that table and in Chromium's accelerator table).
   Port `ea18f661`'s batch restore, not the one-tab-at-a-time version it replaced:
   export `TabsProxy::RestoreTabs` through `phi_framework_api.h`, and record only
   removals Chromium confirms, since a page's beforeunload can refuse. **Decide
   how Undo is reached:** `kCmdUndo` serves only the address field, and Ctrl+Z
   with the page focused goes to the page; the Mac's route depends on knowing
   whether a text input has the keyboard, which Windows would need from the
   framework. Recommended: an Undo button on the toast
   (`PhiShowToastWithAction`, as Folio does), with Ctrl+Z as a later addition.
   **Decide on the sidebar button:** Windows has no separator row (the Tabs label
   parts the outline from the list) and no Lottie. Recommended: menu and shortcut
   first, no sidebar button.
6. `705af790` + `b457988a` + `2a0a49c2` + `0372e721` + `05a47d22` **Space bookmark
   root.** M without the floating row, L with it. A header row in the bookmark
   outline with paint, hit, keyboard ring and UIA, `bookmark_row_count` in the
   layout, drops in `phi_sidebar_drop`, a local per-Space collapsed list, primary
   text at 55 % (`05a47d22`). No reconciling between sidebars: one `sidebar_`
   paints both. `b457988a` costs nothing, since a second press already reaches
   `OnChromeClick`. **Decide what a collapsed root shows for a focused bookmark:**
   the Mac floats it under the root; Windows folders carry `contains_active` up
   instead. Recommended: reuse `contains_active`. This adds a row to every user's
   sidebar on update, so it is worth a look at the design first.
7. `9921ac9a` **sync problem telemetry.** M for the launch snapshot counts and the
   feedback keys (`PhiUserDefaultsSnapshotProperties`, `extra` in
   `phi_feedback_outbox.cc`). L for the reporter, which needs park reasons on
   `ParkedOwnedItem` and the Mac's debounce, grace and dedupe rules. The Sentry half
   has no client path and is left out unless asked. `analytics.md` "Not ported" was
   not updated in this pass.
8. `8ec42e42` **wider sidebar resize band.** S. `InSidebarResizeBand` is 4 dip,
   all of it on the sidebar side because the page's HWND starts at the edge. Widen
   it to `kEdgeInset` (8 dip), where the rows stop. The AI Chat half is ⛔.
9. `da5da806` **hand cursor over the consent links.** S. `PhiOnboardingWindow` has
   no `WM_SETCURSOR`, so text is already the arrow but `kPrivacyLink` and
   `kTermsLink` are too. `phi_settings_window.cc` has the pattern.
10. `52d90d69` **catalog regen.** S, with any of the above. Of the 23 keys it
    changed, two are used on Windows today and English-only in `strings.json`
    (`profile.card.accessibilitySummary` and `…WithoutDate`, the card's screen
    reader name), so the regen helps on its own. The rest arrive with their ports
    or have no Windows use.

**Results 2026-11-04, on nuc at 300 %, on top of `44bc928`.** Client only, on fork
`3808182ec8b`.

- **Port 1.** `agentSpace.shadow.create`, `.openTab`, `.ping` and `.list` now need
  the switch (`kVerbRequiredOnTask`: a task-scoped verb otherwise costs only
  `kAgentSpace`). `.close` never does, so an agent can always clean up. Both
  directions ran on the plain client, with the agent Space, Space and profile
  names, and `credentials.status` as controls. **Differs from the Mac on purpose:**
  turning the switch off closes every open shadow window at once, where the Mac
  lets them finish, and turning off "Allow agents to control Phi" does the same.
  A window that lands after the switch went off is refused on arrival. That race
  was not hit in 36 tries on nuc, so the branch was run with a temporary forcing
  line, since removed. Also fixed: `shadow.list {}`, which is what
  `listShadowWindows()` sends, answered `scope_not_allowed` whatever the switch
  said, so that helper had never worked on Windows.
- **Port 2.** The warning reads "no other device"; the link is on `Page::kEnterCode`
  only. Run against staging with a throwaway account, as two profiles: first device
  in German, entry page in English and German. The link opens
  `/help/sync/#forgot-your-recovery-code` and `/help/de/sync/#…`, moves down under a
  wrong-code error, and leaves the setup window usable. `pt-BR` gets the English
  article, because the Help Center has no `pt-BR` page. The article's steps exist on
  Windows but it names only the Mac ("Keep at least one Mac set up for sync").
- **Port 8.** The band is `kEdgeInset`, starting where the rows stop; Comfortable has
  none. Widening it made the edge jump to the pointer by up to 8 dip when a drag
  started, so the drag now keeps the grab offset.
- **Port 9.** A hand over both links on the guest consent page and Next steps, the
  arrow elsewhere.
- **Port 10.** The membership card's two reader keys gained 8 locales; nothing else
  in the catalog moved.
- **Regression:** `suite-agent` 10/10 and `suite-contract` 5/5 on the dev-hooks build.

**Not tested in that run.** nuc has no mouse, so cursors were read from Phi's input
queue, never seen on screen. A credential fill or reveal with the switch off (only
`credentials.status` was run), an incognito shadow window closing, production, and
the full suite sweep. The new translations of the description and the warning are
not from the translation repository. On screen: the description in English, German
and Simplified Chinese, the warning in German.

**Results 2026-11-04, second round, on nuc at 300 %, on top of `dee7316`.** Fork
`3d820951226` plus the `RestoreTabs` export. Real mouse and keyboard input. One combined
client was built and tested, not each commit alone.

- **Ports 3 and 4** (`eefa624`). Closing from a tab, a merged split, a group header, a
  collapsed group and a group member, in Balanced and Comfortable; bound tabs stayed; the
  rows hide with the switch, and both switches apply at once and survive a restart.
  Double-click closes a row, a strip cell and one split pane, in the docked sidebar and the
  floating panel; a single click, Ctrl or Shift, a double-press that drags, a pin, a
  bookmark and an agent-held Space do not close. **Differs from the Mac:** a whole group as
  a target closes with no group prompt, because tabs close one by one.
- **Port 5** (`4a3948e`, fork `2d0815acd18`). Ctrl+Shift+K and the ⋯ row; Undo restored order,
  a split, a collapsed coloured group, and a page into its still-open group, all unloaded with
  focus unmoved. A `beforeunload` page: Cancel keeps it and Undo leaves it alone; Leave closes
  it and Undo brings it back. Refused under an agent; nothing happens with only a foreground
  split; Reader View over the foreground tab is unaffected. **Decisions to reverse if wanted:**
  Undo is a toast button (10 s, latest cleanup only, no Redo), no sidebar button, the row sits
  at the end of the ⋯ menu's first block. The toast can sit over a "Leave site?" dialog's title.
- **Port 6** (`c28180d`). Collapse by click, keyboard and in the floating panel; persisted
  across relaunch; a second Space keeps its own state; hidden with no bookmarks, in Incognito
  and in Comfortable, whose bookmark bar still shows every bookmark with the root collapsed;
  the name follows a rename. Drops on a collapsed and an expanded root, and bookmark moves
  between rows and into a folder, landed where expected. **Found:** a collapsed folder computes
  `contains_active` (`phi_sidebar_model.cc`) and paints nothing, so the ranked list above was
  wrong to say it shows it; the root got its own treatment and folders were left as they are.
- **Port 7, in part** (`f60d87c`). Against a throwaway staging account and a loopback analytics
  collector: `sync_enabled` false, then true with unknown counts left out, then
  `sync_paired_device_count` 1, `sync_account_profile_count` 1 and
  `sync_default_flagged_profile_count` 1 on the next launch, and cleared by an account reset and
  by sign-out. Feedback carried `sync_phase` `upToDate` and `notStarted`. **Not built:** the
  `sync_attention_*` reporter. It needs park reasons through 29 parking sites and 17 cursor
  writes, problem categories across 49 round-outcome sites, and four helper conditions that do
  not exist here, then a way to provoke each on a real engine. It deserves its own pass.

**Not tested in that round.** Rebinding the new shortcut; `IDC` routing does not exist; a split
or group restored into a window on another Space; the root with enough bookmarks to scroll the
stored band; a drag that grows the pinned grid under the root; production sync or analytics;
translations on screen except English (every new string comes from the Mac's catalog by key,
apart from `appMenu.clearAllTabs` and `tabs.clearAll.confirmation.windows`, which are the Mac's
text recased or cut to its first sentence, not from the translation repository).

**Sweep 2026-11-04, on gmk rather than nuc, fork `2d0815acd18`, client `f60d87c` plus the fix
below:** 182 passed, 6 failed, 32 suites, `suite-feedback` and two-box sync not run. Three are the
pixel failures already known (settings saturation, theme repaint, a Space's pinned theme).
`suite-menu`'s expected list was stale; it now has the new rows and passes 7/7. `menu: New tab
opens a tab` failed once and passed on rerun. `nav: free text` gives a live Google page 10 s to
finish loading: the navigation commits in about 115 ms on `3145519` and `f60d87c` alike, then the
page takes 2.8 to 22.7 s, so it failed on the slow runs. gmk's 96 DPI passes the nuc-only pixel
and bounds failures. Commits were not swept one by one.

**Found by that sweep:** a window destroyed with the floating omnibox open crashed `Phi.exe`, because
`~PhiShellWindow` reset `sidebar_` before the card's teardown asked it for the active tab. `3145519`
crashes the same way, so the ports did not cause it, and only a profile with "new tab opens the
omnibox" reaches it, which is why nuc never did. Fixed by closing the card first. Reproduced
through the control pipe only, not by closing a window by hand.

**How this was checked.** Every diff was read, and each Windows twin in `client/`
and the fork at `3808182ec8b`. Nothing was built or run on a box for the triage, so
every Windows claim in the ranked list above is from the code. `phi/phi-r156` has
moved to `3d820951226` since; that range was not read.

**Coverage.** The table has all 33 commits, matching `--table`. The script's grouped
form prints 31: its groups match known subject prefixes, so `a7ac1c07` (no prefix)
and `52d90d69` (`i18n:`) appear in no group. `release/2.16.0` holds four commits not
on `dev` by hash: three are copies of `c7972bc1`, `da5da806` and `597d76b5` by patch
id, and `e24c450a` is the Private AI row `9070b049` reverted on `dev` (⛔ AI suite).

| Mac commit | Subject | Verdict | Windows action / reason |
|---|---|---|---|
| `9897f718` | feat: add optional double-click tab closing | done | Port 4, 2026-11-04. §2 row updated. |
| `b47255d6` | feat: add native positional tab closing | done | Port 3, 2026-11-04. §2 row updated. |
| `5397c931` | fix: show the peek's URL in the performance sidebar address bar | skip | That address bar belongs to the Performance layout, removed on Windows (§1). The Windows field already follows a presented Peek (§3 Peek row). |
| `c3149697` | fix: tell users everything the agent Spaces permission allows and correct the skill's Settings paths | done | Port 1, 2026-11-04. The skill-path half has no twin: the Windows `install.md` is its own text and already names Settings ▸ Developer ▸ Agent control. |
| `8c1bc1fe` | fix: leave shell fullscreen when a page exits during the enter | skip | The Mac holds the exit for AppKit's asynchronous transition. `PhiBrowserHost::SetShellFullscreen` is borderless and synchronous, called straight from `OnHostedWindowFullscreen`, so there is no settle to wait for. |
| `93a8f43c` | docs: add the popup-over-fullscreen-shell row to the hosted-shell regression matrix (#277) | skip | Docs for macOS fullscreen Spaces and auxiliary windows. The Windows shell is a borderless window with no Space to switch away from. Whether a popup stays above the shell when the shell is clicked was not checked. |
| `e2941d32` | fix: keep the web view in the window for content fullscreen (PHI-1862) | skip | AppKit's pointer tracking and first responder. `PhiBrowserHost::OnContentFullscreenChanged` sets a flag and relayouts; nothing is reparented. |
| `344d30c8` | fix: keep sidebar folder toggles in step with fast clicks | skip | NSOutlineView's `doubleAction` and the Lottie icon. A second press already goes to `OnChromeClick`, which toggles the folder, and Windows folder icons do not animate. |
| `c4d5ce52` | fix: keep bookmark folder expansion in sync between floating and main sidebars | skip | The Mac has two outlines. Windows has one `sidebar_` and one `expanded_bookmarks_`, painted on both surfaces. |
| `1aff6a83` | fix: keep a collapsed folder closed when its selected bookmark floats | skip | Windows never expands a folder for focus and has no floating proxy: a collapsed folder shows `contains_active` (`phi_sidebar_model.cc`). |
| `44d93b39` | fix: keep a nested folder collapsed after its parent is re-expanded | skip | NSOutlineView's remembered expansion. `PhiVisibleOutlineRows` derives the rows from one set per guid, so a nested folder keeps its own state. |
| `06bee451` | feat: add undoable background tab cleanup | done | Port 5, 2026-11-04. §2 row updated. The organize half of the separator button is ⛔ AI suite. |
| `705af790` | feat: group Space bookmarks under a collapsible root folder in the sidebar | done | Port 6, 2026-11-04. §2 row updated. |
| `8ec42e42` | fix: widen the drag area of the hidden sidebar and AI chat dividers | done | Port 8, 2026-11-04. |
| `b457988a` | fix: keep the Space root toggling on fast repeated clicks | done | Part of port 6: a second press reaches the same toggle. |
| `2a0a49c2` | fix: remember the Space bookmark root's collapsed state across relaunch | done | Part of port 6, in local preferences. |
| `af8702c3` | feat: show an undo hint after clearing tabs | done | Part of port 5: the hint is the toast's Undo button. |
| `ea18f661` | fix: keep tab order steady when undoing Clear All Tabs | done | Part of port 5, with the fork's `RestoreTabs` export. |
| `88a34caa` | fix: leave the keyboard with a page that already has it | skip | NSWindow first responder. Windows has no refocus retry on URL ready and `OnContentFullscreenChanged` does not refocus; `FocusBrowserWindow` only drives aura activation. |
| `55745b35` | test: cover the focus retry for a page that has no URL yet | skip | Mac tests for the `88a34caa` path. |
| `9921ac9a` | feat: sync needs-attention telemetry and launch snapshot sync counts (#279) | partial | Port 7, 2026-11-04: launch snapshot counts and `sync_phase`. The reporter was not built, see Results. §5 row updated. |
| `971066ad` | fix: keep the focused pane in the window when a split collapses | skip | AppKit view removal. Windows panes live in the fork's `MultiContentsView` inside one hosted HWND (`split-view-chrome.md`), and the client moves no view when a split ends. Whether the surviving pane keeps the keyboard was not checked. |
| `a7ac1c07` | Warn about losing the recovery code and link Help from the recovery code entry page (#278) | done | Port 2, 2026-11-04. §5 Sync keys row updated. |
| `52d90d69` | i18n: positional tab closing, tab cleanup, VoiceOver descriptions and the agent browsing data setting in 8 locales | done | Port 10, 2026-11-04. |
| `05a47d22` | fix: keep sidebar labels and icons consistent across space themes | done | Part of port 6: the root's colour. No clear button, so nothing else uses it. |
| `848024c8` | chore: log stale sidebar theme bindings in debug and nightly builds | skip | Debug logging for AppKit `ThemedHostingView` bindings; no twin. |
| `0372e721` | feat: highlight the Space root on drag and drop items at its top | done | Part of port 6: `ResolveOutlineDrop`. |
| `830fecf2` | docs: extend the popup-over-fullscreen-shell regression row | skip | Same docs row as `93a8f43c`. |
| `cdb13bf2` | docs: note that a popup placed on another display stays off the fullscreen shell | skip | Same docs row. |
| `9c925fc3` | docs: note that a floating popup only enters fullscreen through Chromium | skip | Same docs row; AppKit's `toggleFullScreen:` on an auxiliary window. |
| `c7972bc1` | fix: crossfade the sidebar's navigation gradient through a Space switch instead of dropping it | skip | `LerpPalette` (`phi_spaces_strip.cc`) already ramps every `ground_stops` entry, a flat side as a flat gradient. One small difference: a two-colour side pads as a, b, b where the Mac uses a, middle, b, so a switch to a three-colour palette may start with a small step. Not seen on a box; not worth a port alone. |
| `da5da806` | fix: show the arrow cursor over Next steps consent text and a hand over its links | done | Port 9, 2026-11-04, the hand only. |
| `597d76b5` | fix: let Tab reach OOBE footer buttons and follow the Keyboard navigation setting | skip | `MoveKeyboardFocus` already walks every enabled control including Back and Next and skips `kCard`; Space and Enter activate; Up and Down move the layout choice. Keyboard navigation is a macOS setting, and the password manager page is skipped on Windows. |

### 2026-11-02 — phibrowser-mac `f7d850a3` → `8c0b8596` (66 commits; release/2.15.0 merged into main and dev)

**Port recommendations this pass (21 commits in 12 ports), all resolved on
2026-11-02: eleven done, one a skip. In order of user impact:**

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
3. `79b11061` **Kiosk page focus on open**. Done, reproduced first (table).
4. `8d998ebc` **onboarding keyboard and screen reader**. Done (table).
5. `40ede7a7` + `92816ab6` + `ba180400` + `1cf61180` + `8fb61ef5` **membership
   card follow-ups**. Done (table).
6. `5196706b` **guests default to the Omnibox new-tab behaviour**. Done (table).
7. `28c0618b` + `0d790e04` + `c87fde81` **onboarding refresh**. Done, without the
   pills (table).
8. `5e434655` + `294d4d02` **neutral default surface**. Done at #2B2B2B (table).
9. `6985cc34` **Split with New Tab**. Done at Ctrl+Shift+\\, no preview (table).
10. `7ec6e1b7` **close an open bookmark from its row**. Done (table).
11. `8d76a688` **even out the pinned band's gaps**. Skip: the design doc states
    the spacing (table).
12. `a6e3a27a` **palette editor responsiveness**. Done from measurements (table).

**The recovery-code confirmation page (Mac `32799547`, `a8a8f23e`).** Done
2026-11-02. A first device must type the shown code back before sync can pair:
`EnrollmentRecord::recovery_confirmation_required` (the Mac's
`recoveryConfirmationRequired` in `sync.pairingEnrollment`) is set before
bootstrap, survives `BeginEnrollment`, blocks `IsPaired` and
`CompleteEnrollment`, and is cleared only by a verified code, by a bootstrap that
never showed one, or by removal. Back shows the same code while the window stays
open; a reopened window offers re-entry alone. Verified on nuc against a stub
`/keys/v1` on loopback (`PHI_SYNC_BASE_URL`, dev-hooks only), because the real
account already has keys: bootstrap, wrong code refused, Back shows the same code,
close and reopen, restart, right code pairs, bootstrap answering 500 and 409 clear
the flag. The stub's 200 for a repeat `POST /keys/v1/devices` was an assumption; the
real server answers 204. Verified 2026-11-02 on staging with a throwaway account,
as two profiles on nuc: the first device confirms its code with no rotation, the
second joins with the code, bookmarks cross both ways, and both unlock after a
restart. Not verified: two separate machines, a Mac on the same account, production.

**Leads, reproduced first on nuc.** A link forwarded while only a Kiosk is alive
landed inside the Kiosk with restore off: real, fixed in the fork (a Kiosk or agent
Space is no target for `GetExistingBrowserForOpenBehavior`, and `FindTabbedBrowser`
skips a Kiosk), so a new shell gets the link. `IsAgentDriven()` stayed true through
Take control: real, a real click for fullscreen got "not granted" in a handed-off
Space; fixed with `PhiFramework::SetAgentMode` following ownership.

**Test gaps closed on nuc.** Kiosk move: the target Space deleted while the menu is
open leaves the Kiosk and its page alone and Open in still works; a request that
never answers (a throwaway fault, 20 s deadline) keeps the page in the Kiosk and a
second pick lands it, with a live shell and with no user window. Behind the
onboarding gate a forwarded link opens no Kiosk, so the gate branch has no box path.
Agent viewport: focused window of two different sizes, visible bookmark bar in
Horizontal, no user window, hand-off, take-over, Take control and Hand back by real
clicks. Direct import with both browsers running: Edge 135 URLs of 135, Chrome 123
of 123 (its account bookmarks). History arrived too, 1202 URLs in the test profile
from 154 and 1516 at the sources; not every source row is imported, and the exact
rule was not checked.

| Mac commit | Subject | Verdict | Windows action / reason |
|---|---|---|---|
| `3a6cbf2e` | fix: temporarily disable Peek CDP link guard and video pausing | skip | Keep Windows' guard. The Mac's reason is a retained CDP session per tab; fork `PhiPeekLinkGuard` holds none and only runs a script in an isolated world on each committed document (residual cost per frame unmeasured). The Mac has not re-enabled it at `8c0b8596`, so Windows does more than the Mac here (§3 Peek row). |
| `05ea7e55` | feat: add working notification cards, card revoke, and opening card conversations in the AI Chat panel | skip | ⛔ AI suite: all three arrive through the Sidecar's `chrome.phinomenonPrivate` envelope, and Mac `notification.show` stays action-only. `phi_notification_cards` serves `notification.show` only. |
| `92816ab6` | feat: add profile card randomization in account settings | done | Ported 2026-11-02, box-verified on nuc: a text button "Randomize" above Download image (`randomizecard` through `SetProfileCardStyle`). Text rather than the Mac's dice, a Windows-native call; easy to overrule. New keys `settings.account.card.*`. |
| `62986748` | feat: replace settings theme presets with palette editor entry | done | `BuildAppearance` and `BuildSpaceEditor` have the single `spacecolor` row since `d414679`. Minor difference on the §4 Appearance row. |
| `ba180400` | improve: refine profile card layout and lighting | done | Ported 2026-11-02: no radial block in `ComposeFace`, the logo sweep scaled by the light's distance, `SetShowsShadow(false)` in Settings. Looked at on nuc in onboarding and Settings. |
| `a6cf9a15`, `4b8b4a56` | Remove the deleted account's sync device key during account deletion (PHI-1798) | watch | Delete account 🔧 (§4) and local data removal 🔧 (§5). Same patch twice. Keep the binding to the subject captured before the request. |
| `9070b049` | Revert "feat: open Phi Chat's Private AI settings from the AI settings row" | skip | ⛔ AI suite. |
| `ec8a2f1e`, `ad2f32de` | fix: drop the timed agent-mode re-assert on Space switch now that Chromium holds the agent tab visible | done | Same patch twice. The client has no timed re-assert; the hidden shell stays shown off-screen, and the Chromium-side hold (fork `3f57767fbc8`) is compiled for both clients. |
| `a5094c62` | Merge pull request #251 (revert Private AI) | skip | Clean merge of `9070b049`. |
| `0ed3b4af` | fix: drop stale AI Chat collapse mirrors that looped on panel reopen | skip | ⛔ AI Chat. |
| `12d27def` | fix: pin the agent control pill's badge divider to 1pt | skip | NSBox / stack-view slack. The Windows divider is a painted rule and the pill's width is not content-driven (`phi_agent_overlay.cc`). |
| `7ec6e1b7` | fix: use minus icon to close open sidebar bookmarks | done | Ported 2026-11-02, box-verified on nuc: the §2 row. No old-build run, because the old code had no close path at all. |
| `5787c25a` | feat: batch tab updates for smoother closing and duplication | watch | Multi-selection 🔧 (§2). Fork `4d9bceef9d9` compiles for both clients but is not exported through `phi_framework_api.h`. `tab.closeOthers` keeps its loop, because `ScheduleRefresh` coalesces where the Mac rebuilt per tab. |
| `8d3893d2` | fix: blend sticky sidebar New Tab row into themed backgrounds | done | `layout.new_tab_row` is fixed and unfilled over the regrade, and rows clip to `tabs_top`, so nothing sits under it. Not checked on screen. |
| `6f6a8b8e` | fix: use neutral search panel backgrounds and align cell colors | done | `palette.card` is neutral in `PhiSearchTabsPanel` and `PaintPopup`, and both lists select with `hover_strong`. The Mac's solid accent selection with white text is not on Windows; no doc records that as a decision, so a change there would be a new request. |
| `40ede7a7` | fix: decouple profile card style from browser theme | done | Ported 2026-11-02, box-verified on nuc: the card's palette changed through Randomize while the theme stayed; the export canvas comes from the style (`PhiHolographicCardStyle::ExportCanvas`). Onboarding stays theme-linked, as on the Mac. |
| `fdb65dbc` | fix: reduce bookmark sidebar scrolling overhead | skip | NSOutlineView row reuse and Combine replays; a Windows wheel notch only offsets and schedules a rebuild. Not profiled. |
| `294d4d02` | fix: use neutral content backgrounds and loading backdrop | done | Ported 2026-11-02 by code: `phi_library.cc` `CardGround` is `ground_fixed`, `ContentGround` is `card`. Box-verified on nuc against a tinted sidebar: rail 32,32,32 and content 43,43,43 in dark, 243,243,243 and white in light, in Downloads; Spaces looks the same. The loading-backdrop half is superseded by `5e434655`. |
| `b1334046` | fix: move sidebar extension shelf closer to the address bar | skip | Superseded by `9f5b887f` and `8d76a688`. Windows has no sidebar shelf; pinned extensions sit in the address field and tool cluster. |
| `5e434655` | fix: unify address bar and web content background colors | done | Ported 2026-11-02, box-verified on nuc at #2B2B2B: the §3 row. Light mode does not change; split corners stay the Space tint by Windows design. |
| `a6e3a27a` | improve: keep Space theme editor responsive on slower Macs | done | Ported 2026-11-02 after measuring on nuc at 150 % (field 31 ms at 366 px; about 4× the pixels at 300 %, an estimate; previews 76 samples, avg 511 µs, max 985 µs). The field is cached per process so a reopen costs nothing. No frame timer: a preview is far under a frame. The router half has no twin. |
| `a19a2fe6` | chore: remove unused CustomSlider | skip | Dead AppKit slider; no twin in `client/`. |
| `17c0d54c` | fix: reset tab range selection anchor when focus changes | watch | Multi-selection 🔧 (§2): no selection model yet. Clear the anchor synchronously on an active-tab change when it lands. |
| `79b11061` | fix: focus Kiosk page opened from an external app (#256) | done | Reproduced and ported 2026-11-02 on nuc: `hf=0 keys=0` on the old build, `hf=1 keys=2` on the final one, opened by a real click. `WM_SETFOCUS` also calls `FocusBrowserWindow`, and a one-shot `FocusFirstPage` runs once the first page is up and the window in front, through the new fork call `FocusActivePage` (`IDC_FOCUS_WEB_CONTENTS_PANE` is disabled in a Kiosk). |
| `423c5a4e` | feat: return to the source app when an external-link Kiosk closes (#253) | watch | New §1 row 🔧, waits on the external-link hook. |
| `6985cc34` | feat: add Split with New Tab to the Tab menu and shortcuts (#226) | done | Ported 2026-11-02: the §3 row. Ctrl+Shift+\\ and no preview, by decision. `kPairSplitTimer` waits up to 3 s for the new tab, because a push does not refresh the tab list. |
| `63c68d90` | fix: restore the session before a windowless Kiosk-setting rule link opens in a Space | watch | Open external links in Kiosk 🔧 (§1): the Mac's Swift-side external path, which Windows lacks. A forwarded link on Windows already restores first through the fork's `PhiExplicitRestoreIfNecessary`. |
| `74cb2bce` | fix: restore the session before a windowless Kiosk page moves to a Space | done | Ported 2026-11-02, box-verified on nuc: `RouteUrlToSpace` no longer stops at an invalid slot, `RequestSpaceBrowser` hosts a shell of its own or rebuilds a parked window, `FinishRequestedSpaceBrowser` closes the Kiosk once its page has opened, and `PhiCoordinator::RunAfterWindowlessRestore` restores the last session first (`PhiSessionRestore::BeginWindowlessRestore`). The earlier skips of `4dd5a8c3` and friends hold for silent launch, but process-alive with no user window IS reachable on Windows. |
| `2028f868` | fix: restore the session before a windowless agent open lands in a Space | done | Ported 2026-11-02, box-verified on nuc (old build answered `windowOpened:true` for a rebuilt window; new answers false with the last session back first). `OpenUserSpaceWindow` with `activate` goes through the gate and `activate:false` stays ungated; `PendingCreation::materialized_ghost` carries the answer. `references/management.md`, the `spaces.openTab` text and `verbs.md` updated. |
| `ea04bb00` | fix: hold windowless opens until the restore's fallback window lands | done | Ported 2026-11-02 with `74cb2bce`: the hold ends only after any rescue shell lands (`awaiting_restore_fallback_`), with the watchdog as backstop. The rescue-shell branch was not forced on the box: every restore there brought a window back. |
| `28c0618b` | improve: refresh onboarding design and navigation | done | Ported 2026-11-02, box-verified on nuc: the §5 row. Skipped as decided: the two footer pills, the Login retry hint (hosted page) and the password-manager changes. |
| `0d790e04` | i18n: guest onboarding feature list, legal link wording, Split with New Tab, account card and agent status in 8 locales | done | Regenerated 2026-11-02 from `origin/dev`'s catalog (`--mac-catalog`), because the Mac checkout sits 59 commits behind. |
| `b61f1fb4` | fix: keep a Kiosk page focused when it moves into a fresh window | done | No seed-on-top race: `FinishRequestedSpaceBrowser` opens the URL inside the creation announcement and `OpenEmptyWindow` seeds only an empty strip. Recheck focus once `74cb2bce` lands. |
| `a8a8f23e`, `e27e68ab` | Let the recovery code confirmation page go back to the displayed code (#260) | done | Ported 2026-11-02 with Mac `32799547`, which this needs. See the recovery note above this table. |
| `94648b6f` | fix: stabilize omnibox layout before focusing input | skip | Auto Layout / NSPanel. `PhiFloatingOmnibox::Create` computes its rect, sizes the EDIT, then shows and focuses, in order. |
| `565e6d37`, `20539f59` | fix: host native Chat menus and preserve canvas transparency | skip | ⛔ AI suite (`sidecar.nativeMenu`, Chat canvas). Same patch twice. |
| `4586eb53` | fix: hide the address bar separator while previewing a split | skip | No split preview on Windows, and the row stays under a real split by design (`split-view-chrome.md`). Reconsider if `6985cc34` ports a blank-pane preview. |
| `87543479` | fix: apply the onboarding nickname to the signed-in account | done | No staged credentials on Windows; `AccountInfo` prefers the profile's name, and `PhiAuth` refresh rewrites the user from the new id_token. |
| `8fb61ef5` | fix: keep the randomized onboarding card style across navigation | done | Ported 2026-11-02, box-verified on nuc: `RandomizeCard` saves first; the style held through Back and forward. |
| `10a47dc9` | feat: sign in from the onboarding login page with the Enter key | done | `PhiOnboardingWindow::OnEnter` `kLogin`, guarded by `!AccountSigningIn()`. Not tested on a box. |
| `59fa1cfb` | fix: keep peek and reader pages from washing out lighter in dark mode | skip | AppKit Plus Lighter vibrancy. The Windows panels parent a whole browser HWND with no vibrancy. |
| `1c39d48c` | fix: size the agent viewport from the agent Space's own page area | done | Ported 2026-11-02, box-verified on nuc at 300 % and 150 %, hidden and watched (§6 row). `VerbAgentPanelSize` (`phi_verbs_lifecycle.cc`) answers from `ctx.scope` in CSS pixels, rounded up as Chromium rounds a view; `helpers.mjs` and the skill docs took the Mac's patch, naming the task and dropping the AI Chat panel. The skill selftest passes as far as the old skill's: both stop at `Storage.setCookies`. Its drag step needs a page area about 674 px tall (the fixture's target gets centred), which nuc at 300 % cannot give; it passes at 150 %. **Hand-off, found 2026-11-02 on nuc while closing the test gaps:** watched and handed off, the page laid out at the hidden frame's 1386×849 inside a 1191×750 area, right edge cut off, until the next resize. `ClearDeviceEmulationSize` restores the pre-emulation size whenever the current size equals the emulated one, and this port made them equal. The old skill ended correct and hosting without a skill sizes correctly (1192×750), so `c50918f` exposed it. Fixed in the fork: restore to the container's real size (`IS_WIN_PHI_CLIENT`, Mac unchanged; check whether the Mac hits the same case). The pill's Take control also left the page pinned while the agent stayed alive; fixed in the skill's `ownershipChanged`, a patch the Mac's skill lacks too. |
| `9f5b887f` | fix: align sidebar extension shelf spacing with the design | skip | No sidebar shelf; the address-in-sidebar layout (Performance) is removed on Windows. |
| `c87fde81` | i18n: drop the spaces around the link placeholders in the Chinese legal agreement sentences | done | Came with the catalog regen. |
| `5196706b` | feat: default to omnibox after guest onboarding | done | Ported 2026-11-02, box-verified on nuc: `openNewTabPageOnCmdT:false` after the guest flow. |
| `918ea103` | feat: report email protection state in preferences snapshot | skip | AI-suite setting, like `641e4b2d`; Windows has no such preference. |
| `38d59a35` | fix: keep the sidebar collapse animation smooth | skip | NSVisualEffectView `maskImage`. `SetSidebarCollapsed` is instant and the shell paints the backdrop. |
| `f93aa171` | fix: show the theme gradient in the floating sidebar | done | `PaintFloatingSidebar` regrades through `PhiBackdropRowGrounds` when a palette has two or more stops. The macOS 26 glass half has no twin. |
| `1cf61180` | fix: hide card identifier, handle email-derived names, and remove retry hint hover | done | Ported 2026-11-02, box-verified on nuc: the member ID is no longer drawn, and an account named by its email offers the local part. If saving that prefill fails the flow carries on with the card showing it, and a later successful rename takes over (a bug in the first version, caught on the box). The test account's name does arrive as its email on Windows. |
| `d94d3329` | fix: copy the peeked URL from the address bar's Copy URL buttons | done | `CopyLinkToClipboard` copies `nav_url_`, which `RebuildNavState` fills from `PageCommandBrowser()`. The chrome:// branding gap in the other copy paths is on the §1 row. |
| `24c2345f` | fix: don't rebuild the AI Chat panel while the AI switch is off | skip | ⛔ AI Chat. |
| `b3351da8`, `d967f0cf` | merges (release/2.15.0 into main, main into dev) | skip | No conflict resolutions in the merge diffs; their contents are triaged by commit here. |
| `6a54718b` | feat: admit signed companion apps over CDP, with page actions and the built-in Folio stepping aside (#269) | skip | Decided 2026-11-02: companion apps are not needed on Windows for now. New §6 row ⛔. |
| `8d76a688` | fix: even out sidebar spacing to match the design | skip | `windows-native-ui.md:517` ("an 8 dip gap parts them") and `:621` ("44 dip cells, 8 gap, 8 inset") state the spacing, so the 16 dip is intended. Header offsets and the separator have no twin. |
| `96eaa1d3` | fix: keep the comfortable tab outline on the page card after a height-only resize | skip | Windows comfortable has no outline path: the active tab is a filled top and the page is flush. |
| `dc18d2c5` | feat: show companion apps' page actions in the web-content right-click menu, with the selection and link sent to the app | skip | Same decision. Its fork half `03d1b24aa3b` compiles on Windows but is dormant: `PhiPageActionMenuRegistry` stays empty, so the stub in `e1b1cca9c00` is unreachable. |
| `8d998ebc` | fix: make onboarding pages readable with VoiceOver | done | Ported 2026-11-02, box-verified on nuc: the §5 row. `PhiAnnounceToScreenReaders` is public now; the name EDIT is named through `IAccPropServices`. |
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

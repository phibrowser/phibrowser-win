# Mac parity: what the Windows client has, lacks and leaves out

This is the weekly feature-parity ledger between the macOS client
(`phibrowser-mac`) and this Windows client. It lists every user-facing Mac
feature once, says whether Windows has it, and for every gap records
either the decision to leave it out or that it still needs to be built.
It is a list of surfaces, not of code: how a feature is implemented is in
the owning doc each row points to.

**Checked against phibrowser-mac `93b1d0aa` (2026-11-24) and
phibrowser-win dev `5ec533e`, on 2026-11-24.**

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
| Layout modes | Balanced / Performance / Comfortable | Balanced / Performance / Comfortable | ✅ | Performance restored in 0.7.0; since PHI-2238 its window buttons are in a hover drawer at the window's top-right, so nothing covers the page at rest. Settings ▸ Appearance, the ⋯ Layout submenu and the tab area's Layout submenu list the three in the Mac's order, and the picker selects the mode in use (PHI-2105). |
| Toggle Balanced Layout shortcut (Mac `0fecf9c8`, 2026-11-16) | ⌃⇧L: Balanced, or Performance from Balanced | none | ⛔ | Decided 2026-11-17 (this pass): Windows has no Performance layout (row above), so the toggle has no second state, and Ctrl+Shift+L already opens the Library. A Vertical / Horizontal toggle would be a new Windows shortcut, not this one. |
| Menu bar | AppKit menu bar | ⋯ app menu (F10, Alt+F/E, solo Alt) | ⛔ | Windows-native redesign, `windows-native-ui.md` §3 and §8. Every Mac menu item has a mapped home (§3.2) except the rows marked below. |
| View ▸ Appearance: System / Light / Dark (Mac `6952fddf`, 2026-11-10) | submenu above the layout section | ⋯ ▸ Layout ▸ Appearance, three radio rows above the layouts | ✅ | Ported 2026-11-11, run on nuc through the menu: each row sets the app-wide appearance and the tick follows. |
| Full screen | ⌃⌘F | F11 row shown, never enabled | 🔧 | `IDC_FULLSCREEN` is not routed; the shell has to drive it (`phi_app_menu.cc`). |
| Window title follows the active tab (Space name while the placeholder / NTP is up) | Mac `9387a460`, `9068b449` | `PhiShellWindow::RetitleShell` on every rows rebuild and placeholder change | ✅ | Ported 2026-10-19, box-verified (page → tab title, Ctrl+T → Space name, close → back). A tab showing the native new-tab page counts as the Mac's placeholder. D29's agent-title cue ("Claude — Research") is still open. |
| Window ▸ Name window | yes | dropped | ⛔ | Never worked on Windows; `windows-native-ui.md` §8. |
| Edit ▸ Search the web | ⌥⌘F | dropped | ⛔ | Same, §8. |
| Hide / Hide Others | ⌘H / ⌥⌘H | — | — | macOS app model. |
| Share… (system share sheet) | File ▸ Share, site menu Share ▸, ⌃⌥S | none | 🔧 | `phi_shell_menus.h` notes the site menu's Share ▸ is "not ported yet". Windows has `IDataTransferManager`. |
| Capture Full Page / Capture Area… + screenshot annotation editor (Mac Oct 2026 series `fec4739a`…`c8c6ac28`; rebindable ⇧⌘1 / ⇧⌘2 `7e731204`) | File menu, `docs/page-screenshots.md` | ⋯ menu rows above Print, rebindable Ctrl+Shift+1 / Ctrl+Shift+2 (PHI-2251, `docs/page-screenshots.md`) | ◐ | Ported client-only (no fork or ABI change): PNG via CDP `Page.captureScreenshot` over the in-process DevTools transport with `captureBeyondViewport`, the 16,384 px / 64 Mpx clip and 60 s budget, PNG + bitmap on the clipboard (a newer copy is kept), Save toast; Capture Area… overlay with wheel and 32 DIP edge autoscroll; the editor over the content card with select, arrow, freehand, text, rectangle, ellipse, blur, crop, undo / redo (100 steps), context menu, PNG / JPG / PDF export. Gaps: a style strip instead of the object-anchored property popover and colour picker; no annotation Cut / Copy / Paste, mirrored resize, marquee subtract or Space-drag pan; closing the window drops an unsaved editor (Mac: standalone window); PDF is always flattened (Mac keeps vectors unless blur needs flattening); export and decode run on the UI thread. |
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
| Time Machine backups (policy snapshots + restore) | hidden since Mac `81602df9` (2026-11-09): the Help entry is gone and launch purges the snapshots; the code is kept for re-enabling | none | ⛔ | Decided 2026-11-11: not built while the Mac keeps it off. It replaces the 2026-10-19 "implement". Windows never took a snapshot, so there is nothing to purge. Reopen if the Mac turns it back on. |
| Export / Import User Data (zip) | Help ▸ Manage User Data | disabled placeholders | 🔧 | `user-data-parity.md` S6 lists it as missing. With it: on the first launch after an import, retire the Chromium windows no imported Space maps to instead of parking them as ghosts (Mac `da036e8a`). |
| Import from another browser | Chrome / Safari / Arc / Dia / file | Chrome / Edge / Brave / Arc for Windows / file | ✅ | `browser-import-port.md` (2026-10-23). Safari and Dia are Mac-only; Edge and Brave are the Windows additions. Cookies are offered only for a source without an app-bound key (current Chrome and Edge seal cookies and passwords as `v20`, which no other app can read). Stays open naming the failed sources instead of closing. |
| Migrate from Another Browser (Arc / Dia / Zen Spaces wizard) | wizard rebuilds profiles + Spaces | none | 🔧 | Decided 2026-10-19: implement for the sources that exist on Windows (Zen; Arc only if a Windows install is found). Dia is Mac-only. Re-adds `browser_migration_finished` to `analytics.md`. |
| AppleScript suite | `Phi.sdef`, ~16 commands | — | — | Platform scripting. The control pipe (`tools/phi-browser-skill`) is the Windows automation surface and is a superset. |
| Deeplink `phi://native/openpage?page=settings…` | opens a Settings section | `phi://newtab` only | 🔧 | Small: a protocol handler registration plus the Settings route. |
| Kiosk global shortcut (⌥⌘N in any app) | Navigation setting | none | ⛔ | `phi_kiosk_window.h`: "not ported from the Mac (deliberately)". |
| Open external links in Kiosk | Navigation setting | row not drawn | 🔧 | Revisited 2026-10-27 (the ⛔ reason changed): Phi IS registered as the http / https handler (installer `RegisterBrowser`, Settings ▸ You "Set as default"). A link from another app reaches the running browser through Chromium's ProcessSingleton and stock `StartupBrowserCreator`, so Domain / URL rules already steer it, but the CLIENT never learns the open was external. Needs a framework hook on the forwarded command line (`IS_PHI_ABI_CLIENT`) before the Kiosk preference (and App rules below) can apply. `63c68d90` (restore the session before a windowless rule open) belongs to this path. So does `02b41cee` (2026-11-20, PHI-2134): an external link a rule sends to a Space opens in a window already showing it. Windows probably switches the last active window instead, because `RouteUrlToSpace` keeps a link in its source window and an external open has the last active window as its source (not checked on a box). |
| App URL rules: route external links by the SENDING app (Mac `48d4c4a6`, 2026-10-23) | `app:<bundle id>` rule kind, running-app picker | rules accepted, stored, synced and shown as "App" (retarget / ask / delete; not creatable); never matched | ◐ | Storage / sync / editor half ported 2026-10-27 (`UrlRuleKind::Refuses`, `SpaceSyncTable::inbound_acceptance_version` one-time replay, `PushRoutingTableNow` skips them). Matching stays Mac-only by decision: Windows cannot recover the launching app (only the command line crosses ProcessSingleton; the parent process is often a broker). |
| Return to the sending app when an external-link Kiosk closes (Mac `423c5a4e`, 2026-10-28) | sender captured at the open, re-activated on close unless focus left the Kiosk or the page moved to a Space | none | 🔧 | Waits on the external-link hook two rows up. Windows may be able to capture the foreground window before `PhiKioskWindow` takes it, which the Mac cannot (macOS has already raised Phi); untested. Whether z-order already returns activation on close is also unchecked and could shrink this. |

## 2. Sidebar, tabs, Spaces

| Feature | Mac | Windows | Status | Gap / decision |
|---|---|---|---|---|
| Docked sidebar: Space pips, pinned grid, bookmarks, tab tree, bottom bar | yes | yes, 200–480 dip | ✅ | `sidebar-architecture.md`. |
| Floating sidebar (edge hover) | yes; since 2026-11-16 resizable by its edge (`bb4082e1`) and sprung into place as it slides in (`20876a28`) | yes | ◐ | No fade on slide-in (impl doc §4.4). Proposed 2026-11-24 (§9): unpinning with the shortcut while the pointer is on the docked column leaves the panel out in place (Mac `0416448a`); the slide honours the system Animation effects switch (Mac Reduce Motion, `dacfd786`, `01be27ec`); a pointer that ran from the panel past the window's sidebar edge keeps it out (`29179ff9`). Since 2026-11-17 a flyout shown from the panel (icon pickers, the create-Space form, the downloads popover) holds it out until the flyout closes (Mac `57f08e2a`, §9). Not built, waiting on a decision (§9 2026-11-17): the edge resize (the panel is the docked width, `Scaled(sidebar_dip_)`, with no resize band) and the spring slide-in (Windows eases 150 ms out both ways). |
| Sidebar on the right (Mac `574c20c4`, 2026-11-12) | Settings ▸ General ▸ Appearance Left / Right (`sidebarPosition`, local, default left; width kept per side, `lastKnownRightSidebarWidth`); tab-area menu "Move Sidebar to Right / Left" in vertical layouts; the docked and floating sidebars, trigger edge, previews and omnibox popup mirror; the traffic lights move to a 32 pt band that fades in when the pointer reaches the top edge; Comfortable is always left | left only | 🔧 | Triaged 2026-11-14. Windows needs no hover band: its caption buttons already sit at the top right. Proposed: with the sidebar on the right the caption buttons move to the trailing end of the sidebar header (the window corner), and back to the address row while the sidebar is collapsed; the floating panel docks at the right edge below the address row. Touches `phi_shell_layout`, the shell's resize, caption and floating-panel code, menus, Settings and prefs. L, client only. Since 2026-11-19 (`5c1aa0a1`) the Mac offers the option only in nightly and debug builds, so stable Mac users cannot move the sidebar either. |
| Sidebar bottom bar: profile, chat, memory, downloads, feedback | all five | profile avatar + Downloads + Feedback pill | ✅ | Chat and memory are ⛔ AI suite. The profile button returned 2026-10-27 on request (left-most 24 dip avatar / person glyph, the Mac's menu: Download List → Library ▸ Downloads, Manage Bookmarks, Manage Extensions, New Tab, New Incognito Space / Window, Settings, Sign in when guest), reversing the 2026-09 decision in `windows-native-ui.md` §4.2. 2026-11-11 (Mac `ce1f357f`): the sidebar area menu ends in a "Bottom Buttons" submenu that shows or hides Profile, Downloads and Feedback (`sidebarBottomBarShows*`, this device only, all on by default); a hidden button leaves no slot. Run on nuc. No Memory entry: the chip is ⛔ AI suite. 2026-11-13 (GitHub #13): the Downloads button now does what the Mac's `DownloadButtonView` does: the `download-button` Lottie's arrow drop through the tray once per new download (drawn natively from the button's own mask, `phi_download_indicator.cc`) and a 14 × 2 dip accent progress bar under the icon while a download runs (bytes over bytes, fading in and out over 200 ms). The framework has no download event, so the shell polls the list every 500 ms, every 150 ms for 1.5 s after a page change. Gap: Comfortable, a hidden sidebar or a hidden Downloads button have no button to show it on (the Mac's header shows a Downloads button in its traditional layout; Windows has none in the address row by the 2026-09-20 decision). |
| Horizontal strip: Space chip, group chips, overflow, bookmarks bar | yes | yes | ✅ | Group headers in the horizontal layout: none on either side's strip beyond chips. |
| Tab groups (9 colours, collapse, overview) | yes | yes | ✅ | |
| Group tabs opened from links (Mac `7f937b6b`, `4964a1e3`, `33199022`, `2f4cf3b6`, 2026-11-11) | Advanced toggle `groupTabsOpenedFromLinks`, off by default, not synced: a tab the user opens from a link (Ctrl / middle click, Open Link in New Tab) of an ungrouped tab forms a group of the opener (its whole split) and the new tab, named after the results page's search terms, else the opener's title or site; from a pinned tab or Bookmark Tab the group holds the new tabs only and later links join it; never in agent, Kiosk or shadow windows; `auto_tab_groups_enabled` in `user_defaults_snapshot` | Advanced ▸ "Group tabs opened from links", off by default, this device only: a Ctrl-click, middle-click or Open Link in New Tab from an ungrouped tab groups the opener (its whole split) with the new tab, named after the opener's search terms, else its title, else its site; a later link of a grouped opener joins by Chromium's rule; a pinned tab or Bookmark Tab groups its links without itself and later links join that group (memory only); a page's own new tab (plain click on target=_blank) is never grouped; never in agent Spaces (also watched), Kiosk, shadow or Peek browsers; `auto_tab_groups_enabled` in `user_defaults_snapshot` | ✅ | Done 2026-11-15, see §9 port 8. Fork `OnUserOpenedLinkTab` (the Mac's `ScopedUserOpenedNewTab` now also on Windows, reported from `OnTabCreated`, posted after the insert's snapshot), ABI 8 with port 9. |
| New tabs at the top, New Tab row at the bottom (Mac `5590a1d1`, 2026-11-13) | General ▸ Browsing toggle `newTabsAtTop`, off by default, SYNCED: in sidebar layouts ordinary new tabs go before the first normal tab (before a whole split, after pins and bookmark tabs); Comfortable always appends; the sidebar New Tab row follows the setting, so by default it now sits under the tabs, sticky at that edge | General ▸ Browsing "Open new tabs at top", off by default, synced (`newTabsAtTop`): off, ordinary new tabs (Ctrl+T, the New Tab row, an omnibox commit into a new tab) append and the New Tab row follows the last tab, sticking at the list's bottom edge with its hairline once the tabs overflow; on, the row leads the list (hairline while rows scroll under it) and those tabs open before the first tab of the list: after pins and bookmark tabs, before a whole split or group; Comfortable always appends | ✅ | Done 2026-11-15, see §9 port 9. Fork `PlaceNewTabBefore` (TabsProxy's initial placement over `PhiMoveNewTabTo`, so opener children survive), ABI 8 with port 8. Not ported: the Mac's drop index for a tab dropped on the New Tab row itself (Windows drop zones come from the tab rows). Since 2026-11-19 (Mac `234efbac`) the Mac picks the position with two illustrated cards, At the top / At the bottom, in place of the toggle; Windows still has the toggle (port proposed §9). |
| Tab hover preview card | yes | yes | ✅ | |
| Open-tab indicators / unloaded (dimmed) indicators | yes | yes | ✅ | |
| Tab audio / muted indicator on rows and strip cells | yes (Mac `4e6de510` keeps it while muted) | the Mac speaker button on playing or muted rows, pinned cells and bookmark rows; Mute tab / Unmute tab in their menus | ✅ | Corrected 2026-11-24: ported 2026-11-15 (`7880a8c`, GitHub #14) on framework ABI 7 (`TabSnapshot::audible` / `audio_muted`), the row was not updated then. |
| Tab capture indicators and "Share this tab instead" (Mac `00c25130`…`75d5f1c7`, merged 2026-11-23) | a red dot on the favicon while a tab uses the microphone or camera, badges while a tab presents a screen share or is being shared, an outline around a shared page and its sidebar row; while a tab share runs, hovering another tab shows a button that moves the share to it without restarting it (fork tab sharing proxy `ee1384d0b39`) | none: no capture state in the framework snapshot | 🔧 | Triaged 2026-11-24, waiting on a decision. L: fork (export the capture flags and the tab sharing proxy through `phi_framework_api.h`, ABI) + client (badges, outline, hover button in rows and strip cells). |
| "New Tab Created" toast for a background tab while the sidebar is hidden (Mac `1f585bac`, `919ba4cb`, 2026-11-17) | a link opened in a background tab with the sidebar collapsed (vertical layouts, not Kiosk) shows a clickable toast at the top trailing corner; a click switches to the tab | none | 🔧 | Triaged 2026-11-24, port proposed (§9). S–M, client: from `OnUserOpenedLinkTab`, plus a whole-toast click in `phi_toast`. |
| Ctrl/⌃+Tab MRU switcher | yes | yes | ✅ | |
| Tab Search (⇧⌘A / Ctrl+Shift+A) | open, pinned, bookmarks, recently closed | same | ✅ | |
| Multi-selection of tabs / bookmarks with batch menu | `TabMultiSelectionMenu` | strings imported, no selection model | 🔧 | `common.tabMultiSelectionContextMenu.*` keys exist in `strings.json`; nothing draws them. When it lands: the fork's batch `TabsProxy::CloseTabs` / `DuplicateTabs` (`4d9bceef9d9`, Mac `5787c25a`) need exporting through `phi_framework_api.h`, and the range anchor resets on an active-tab change (`17c0d54c`). |
| Double-click an ordinary tab to close it (Mac `9897f718`, 2026-10-30) | Advanced toggle, off by default; an unmodified double-click on a sidebar row, group member, split pane or strip cell closes that tab; pinned tabs and bookmarks keep their own double-click | Advanced toggle, off by default. An unmodified double-click on a sidebar row, group member, split pane (that pane only) or strip cell closes the tab, on the release with a 5 dip guard against a drag, docked and in the floating panel; refused while an agent holds the Space | ✅ | Done 2026-11-04, see §9 port 4. |
| Close the tabs above / below / left / right of a tab or group (Mac `b47255d6`, 2026-10-31) | tab and group context menus, titles follow the layout; Advanced toggle, on by default; a split counts as one unit and a whole group protects its range; pins and bookmark-bound tabs stay | Tab and group menus, titled by layout, greyed when empty; Advanced toggle, on by default; a split is one unit, a group protects its range, bound tabs stay. Close Other Tabs follows the same rules | ◐ | Done 2026-11-04, see §9 port 3. Not ported: `IDC_WINDOW_CLOSE_TABS_TO_RIGHT` routing, and tabs close one by one, so a whole group as a target closes with no group prompt. |
| Clear All Tabs with Undo (Mac `06bee451`, `af8702c3`, `ea18f661`, 2026-11-02) | File menu, rebindable ⇧⌘K, sidebar separator button; closes ordinary background tabs and keeps the foreground tab, its split, a visible Peek, pins and bookmark-bound tabs; Edit ▸ Undo and Redo per window bring the pages back unloaded with their order, groups and splits; a toast names the shortcut (`docs/architecture/clear-tabs.md`) | ⋯ menu row, rebindable Ctrl+Shift+K, and the separator row's Clear button: a 28 dip row with a 1 dip hairline heads the tab list, and while the pointer is over the sidebar and there is something to clear a 24 dip bin sits at its trailing end (the hairline stops 36 dip short); a click plays the Mac's `clear-tabs` Lottie, drawn natively (`phi_clear_tabs_icon.cc`, 0.5 s), then clears. Keeps the foreground tab, its split, pins and bound tabs. Each Space keeps its cleanups' history (`PhiClearTabsHistory`): ⋯ ▸ Undo clear all tabs / Redo clear all tabs, Ctrl+Z / Ctrl+Shift+Z, and the toast ("Cleared tabs. Use Ctrl+Z to undo.") still carries an Undo button for the newest one. Undo brings the pages back unloaded in one batch with their order, groups and splits; only removals Chromium confirmed return; Redo is a fresh cleanup of what is behind the foreground then | ✅ | Done 2026-11-04, see §9 port 5; the sidebar button and the lasting Undo / Redo added 2026-11-19 on request (suite `suite-cleartabshistory`). One difference: the Mac leaves ⌘Z to a page whose focused element takes text input, which Windows cannot see without a framework call, so Ctrl+Z is page-first: it undoes a cleanup when the page leaves the key alone (a non-editable page, or a field with nothing left to undo) and never inside a native text field (the address field, an inline rename). Fork `RestoreTabs` export in `phi_framework_api.h`. |
| Pinned tabs: scope App / Profile / Space, split pins, rename, edit URL | yes | yes | ✅ | |
| Keep pinned tabs loaded (Mac `375fed24`, `734319a9`, 2026-11-10) | Advanced toggle, on by default, this Mac only; an open pinned tab is skipped by Memory Saver and memory-pressure unloading | Advanced toggle, on by default, this device only; the sidebar pushes each pinned cell's live tabs through the framework call `SetTabKeepLoaded` (ABI 4) | ✅ | Ported 2026-11-11. nuc, read from `chrome://discards`: the pinned tab carries "Tab is a Phi pinned tab" with the switch on, loses it with the switch off, and a plain tab never has it. A real unload under memory pressure was not provoked. Pinned tabs of a Space that is not on screen get the state the next time that Space's sidebar rebuilds. |
| Edit pinned tab icon (Mac `91740002`, 2026-11-13: pinned cell menu ▸ Change Icon…, not on split cells, symbol or emoji with Reset; stored in the V14 `icon` column, `"default"` = none; not synced) | Edit… dialog has icon; Change Icon… | Change Icon… (Space icon picker, icons and emoji) and Reset Icon in the single pinned cell's menu; stored in the pin's `icon` column, not synced | ✅ | Ported 2026-11-15. Reset Icon is a menu row: the Windows picker has no slot for the Mac's trash button. `pinned.list` reports `icon`. suite-pinicon nuc 5/5. |
| Bookmark favicon actions: Separate From Bookmark / Back to Saved URL; a slash before the title when the open tab left the saved page (Mac `e917b732`, 2026-11-10) | row favicon menu; slanted separator between favicon and title | the slash only | ◐ | The slash was ported 2026-11-11 (single bookmark rows, the Mac's URL comparison: `www.` and a lone root slash do not count), seen on nuc. The favicon actions themselves are still missing. |
| Bookmark folder icon picker | yes | yes (24 symbols) | ✅ | |
| Close an open bookmark from its row (hover minus, Mac `7ec6e1b7`, 2026-10-27) | minus on hover while open | minus on hover while open | ✅ | Ported 2026-11-02, box-verified on nuc: `ShortcutRow::close_box`, a `fluent::kRemove` minus and a UIA button; a click closes the tab through `SidebarClose` and the bookmark stays. `kRemove` is new in `phi_fluent.h`. |
| Space bookmark root: a collapsible row named after the Space above its bookmarks (Mac `705af790`, `b457988a`, `2a0a49c2`, `0372e721`, 2026-11-02/03) | presentation only, no persisted parent changes; collapsed state kept per Space across relaunch, local; hidden with no bookmarks and in Incognito; a focused bookmark floats under a collapsed root; drops insert at the top level, and a collapsed root highlights as a target | A row named after the Space above its bookmarks, children one level in, collapsed state per Space in local preferences, hidden with no bookmarks, in Incognito, in an agent Space and in Comfortable. A collapsed root highlights as a drop target and takes the item at the top level; an expanded one opens the gap under it | ◐ | Done 2026-11-04, see §9 port 6. Differs by decision: no floating row; a collapsed root holding the active bookmark wears the hover wash and full-strength text. Folder expansion is still per shell and not persisted. Missing since 2026-11-19 (Mac `c813ad28`, port proposed §9): a hover "…" button on the root row opening a trimmed tab-area menu (New Tab, New Folder, the active Space's rows without Edit Theme). |
| Drag & drop: tabs, pins, bookmarks, groups, cross-window, tear-off | full | ghost / gap / ring / badge / snap tiles / New window | ◐ | Deferred mechanics D31–D34: edge auto-scroll, Ctrl/Shift modifiers, whole-group drag, bookmarks bar as source/target, cross-window slot, OLE file drops, pip lift, hidden-sidebar reveal during drag. When whole-group drag lands, its gap stays on the tab side of the New Tab row (Mac `032a3b4e`). |
| Drop a file onto the sidebar | yes | none | ◐ | Part of D31 (OLE drops). |
| Drag a tab onto another tab's row to make a split, or onto a split's row to add a pane (Mac `aa3bba6c`, `59e65c80`, `33ac26c5`, 2026-11-16) | a middle band of the row takes the drop (left or right half picks the side; an 8 pt edge band stays a reorder gap), the row previews the half-width pane; up to four panes, same group, not a pinned split | splits form only by dropping onto the page; every tab-list drop is a reorder gap | 🔧 | Triaged 2026-11-17, waiting on a decision. M, client: an "on row" band in `ResolveTabDrop`, the half-row placeholder, commits through `FormSplitOrdered` / `AddTabToSplitById`. |
| Empty pinned band's drop zone opens at the header (Mac `8eca5e62`, 2026-11-16) | with nothing pinned the zone stays shut at the lift and opens once the drag is carried over the sidebar header | the same | ✅ | Ported 2026-11-17, see §9. Before, the 100 dip zone opened at the lift and pushed every tab row down under the pointer. The empty outline's one-row zone still opens at the lift, as before (the Mac commit leaves it alone). |
| Hover preview of a collapsed bookmark folder or Space root (Mac `ca07af8f`, 2026-11-16) | a 0.4 s rest opens a 280 pt panel beside the row: a search field and every page inside (subfolders flattened, split panes one per row) with "last seen" times; keyboard, gap path, closes on leave | none | 🔧 | Triaged 2026-11-17, waiting on a decision. L, client (a new `PhiPopover`; the stored `last_seen` exists). With it: the floating panel stays out while a preview anchored in it is up (Mac `bde0d73c`). |
| Spaces: create (name, icon, profile), rename, icon/emoji picker, theme, change profile, delete, reorder | yes | yes | ◐ | Picker lacks "Recently used" and skin tones (D24); create form keeps old control styling (D25). Since 2026-11-23 (Mac `739fd939`) the Mac's icon set is Heroicons v2 mini: the old icons redrawn under their old ids and 70 new ones. Windows has the 88 old ids only (`phi_icons.cc`), so a Space, pin or folder synced from a Mac with a new icon shows none here. Port proposed (§9), M, client. |
| Mouse back / forward side buttons over the sidebar switch Space (Mac `711f8a61`, `c7f41199`, 2026-11-16) | back = previous Space, forward = next, docked or floating sidebar, no wrap (a bounce at the ends); over the page they still navigate | the same: XBUTTON1 / XBUTTON2 over the docked sidebar or the floating panel step through the window's Spaces (agent Spaces after the user ones), no wrap, the band bounces at the ends (22 dip, 0.3 s, PHI-2163); vertical layouts only, not in incognito windows, not under the create form; over the page Chromium navigates as before | ✅ | Ported 2026-11-17, see §9; the end bounce 2026-11-23 (PHI-2163, `suite-spaceswipe`). Before, the client ignored the side buttons over the sidebar. The Mac's swipe-shaped button events (Logi Options+, SensibleSideButtons) are a macOS event type with no Windows twin. |
| Trackpad swipe switches Space (Mac `SpaceSwipeTracker`, `23643d84`) | a two-finger sideways swipe over the sidebar (docked or floating), or over the traditional tab bar where the strip has nothing to scroll, switches to the next (fingers left) / previous Space; the axis latches per gesture; completes at 35 % of the band or a flick (12 pt at 650 pt/s); the band follows the fingers and can be pulled back; no wrap, the band bounces at the ends; the media card keeps its own swipe | the same areas (comfortable's strip row for the tab bar, the Mini Player excluded), fed by WM_MOUSEHWHEEL (a Precision Touchpad's pan reaches a plain window that way, inertia included); a gesture is a run of wheel messages less than 150 ms apart; same latch and thresholds; the slide does NOT follow the fingers — it plays once the swipe qualifies, and the rest of the run is swallowed (one switch per gesture); a swipe during a running slide is ignored; a tilt wheel's sideways notch counts as a swipe | ◐ | 2026-11-23 PHI-2180 (`phi_space_swipe.{h,cc}`, `suite-spaceswipe`, synthetic HWHEEL only: no box has a precision touchpad). Gap: the finger-following, cancellable slide (the Windows band slide switches the Space when it starts). |
| Sidebar header: Top Buttons submenu, Space icons in an empty header (Mac `b7bee5f1`, `823dba25`, 2026-11-16) | the tab-area menu's "Top Buttons" shows or hides the header's Search Tabs, Toggle Sidebar and (Performance) Back / Forward / Reload; with every button hidden, the Space icons move up into the header row beside the traffic lights, centred on the active Space | none: the header always shows the app mark, Search Tabs and the sidebar toggle; the Spaces strip has its own row | 🔧 | Triaged 2026-11-17, waiting on a decision. Windows has only two of the header buttons (Back / Forward / Reload live in the address row) and its header leads with the app mark, so "empty" means the mark and the pips share the row. M, client. |
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
| "Hold ⌘ to switch" to an already-open tab | yes | "Hold Ctrl to switch →" | ✅ | Ported 2026-10-28 with the rest of the Mac `OmniBoxViewModel` / `OmniBoxTextField` logic (fork `OpenOmniboxMatch` takes an explicit disposition, `DeleteOmniboxMatch`, `StartOmniboxQuery(prevent_inline)`, `swap` in the results json; client `phi_omnibox_session.{h,cc}`): a row with an open tab opens in the current tab unless Ctrl is held (Enter or click), where it used to switch silently; inline autocompletion of the default row (selected, typed over, dropped on Backspace / Delete); the default row armed when the rows answer the typed text, an arrowed pick kept across streamed updates, stale result sets dropped; Shift+Delete and a hover × forget deletable rows; history rows show their favicon; Chromium's swapped rows, pedals and empty rows filtered; chrome:// reads phi:// in the field and the rows; the keyword chip wears the seeded engines' brand colours. Not ported: a bookmark / pinned row's tab opening address-bar submissions in a NEW tab (`3d3719b1`). Watched since 2026-11-14: the Mac's unified omnibox (`92ba12c5`, `df77cf4a`, `13f47f02`: Phi pins, bookmarks, split items and commands ranked as rows, Enter opens a split item as a new copy, ⌘ switches) runs only in its Canary build; on Windows the fork's `IsUnifiedOmniboxEnabled` stub returns false and the framework API has no session calls. The Mac took it out of Canary on 2026-11-19 (`8820a54e`); see the unified omnibox row below. |
| Unified omnibox: Phi pins, bookmarks, split items and commands ranked as rows (Mac `92ba12c5`, `df77cf4a`, `13f47f02`; every build since `8820a54e`, 2026-11-19; commands fill vacant rows, `a35cb0f7`) | native pin / bookmark / command providers beside Chromium's; Enter opens a split item as a new copy, ⌘ switches to an open one | Chromium's rows only (the fork stub returns false) | 🔧 | Triaged 2026-11-24, waiting on a decision. L: fork (enable the providers on Windows, export the session calls through `phi_framework_api.h`, ABI) + client (`phi_omnibox_session`, row drawing for pins, bookmarks, splits and commands). |
| Site menu: security row, Content blocking on this site, Always show URL path, Site settings, Clear cache / cookie, Extensions ▸, Share ▸ | yes | all but Share ▸ and Extensions ▸ | ◐ | Share: see §1. Extensions live in the field's puzzle button instead (design). Always show URL path drives the field since 2026-11-04, default off as on the Mac: off shows the host alone, on adds the port, path, query and fragment, and neither shows the scheme, `www.` or trailing slashes. Editing and copying use the whole URL. Unlike the Mac's on state, `data:`, `blob:`, `view-source:` and `file:///` URLs stay whole. |
| Site menu: Collect Site Memories / Delete Site Memories | yes | none | ⛔ | AI suite (site memory is browser memory). |
| Copy link in the field, Reader button (hover) | yes | yes | ✅ | Copies the presented Peek's URL (the Mac's `d94d3329`), through `RebuildNavState` and `nav_url_`. |
| Reader View (typography prefs) | yes | yes + read-aloud HUD | ✅ | Windows has read-aloud; Mac does not. `-phi-auto-reader` automation entry point not ported (analytics doc). |
| Peek (Shift-click, auto-peek from pins/bookmarks) | chips: Close, Open as Tab, Open as Split, Reader View | chips: Close, Open as Tab, Open as Split | ◐ | No Reader chip on the Peek card; Peek is omitted from session restore (`analytics.md`). Page commands and the address field aim at the presented Peek since 2026-10-27 (`a19624bb`, `c11849cf`; Back / Forward and the Reader toggle stay on the opener). The Shift-click SPA-router guard + opener video pause landed 2026-10-27 (`80f92213`: fork `phi_peek_link_guard.{h,cc}` runs the Mac's capture-phase click guard in a Phi isolated world of every document, gated on `peekViewEnabled`; `OpenUrlAsPeek` pauses the opener's videos in every frame). Still missing: appear / close flights and the opener dim. The Mac turned its guard and the video pause OFF on 2026-10-27 (`3a6cbf2e`, for the CDP session it retained per tab; still off at `8c0b8596`). Windows keeps its version, which holds no DevTools session, so Windows does more than the Mac here until the Mac re-enables it. 2026-11-17 (Mac `2fec54f4`): Settings ▸ Navigation "Also peek links to the same site" (off by default; fork `ShouldAutoPeekLinkChild` reads `AutoPeekSameSiteLinks`, framework ABI 10) and "Peek panel size" Large / Medium / Small (80 % and 68 % of the page, never under 480 dip while it fits; Large keeps the 40 / 24 dip insets).  |
| Split view (two panes, drop hints, swap, convert, pin/bookmark a split) | yes | yes | ✅ | `split-view-chrome.md`. |
| Splits of three or four panes (Mac `b3a379e3`, 2026-11-05; synced since `461fd42d`, 2026-11-17) | up to four panes with a share each; add, remove and move a pane; drop hints for add left / right / top / bottom / between and replace; pinned and bookmarked splits of three or four (LocalStore V14 columns `splitPaneGuids`, `splitExtraPanes`, local only) | up to four panes with a share each, laid out by the fork; one merged sidebar row and strip cell; Split with Current, Alt-click and Split with New Tab add a pane; page drops insert or replace; row menu and hover chip move, remove and close panes; `splitView.*` verbs take `tabIds` and `fractions` | ◐ | Built 2026-11-10 (`split-view-chrome.md`, "Three and four panes"), checked end to end on nuc. Pinned, bookmarked and duplicated splits of three or four work too (one optional `splitExtraPanes` column, store format still 13; a pinned split keeps its later panes on its first row, unlike the Mac's `splitPaneGuids`). Missing: the hover preview and drag card beyond two panes, and per-pane rows in a stored cell's own menu. 2026-11-11, on request: the pane move grip is there (drag along the split to reorder, pull out to remove, drag onto the tab list to make it a tab and keep dragging), never on a pinned or bookmark-bound split; run on nuc. The panes do not slide under the drag as on the Mac. 2026-11-11 (Mac `153044b1`, `80bd9bdb`): the row menu's move rows act on the focused pane and say so, and Remove from Split dissolves the whole split (row menu and hover chip); run on nuc. Later on 2026-11-11, on request (`8422f31d`, `6e786436`, `b36d2c7d`), run on nuc: a pinned split and a split bookmark have an editor with a name and an address per pane (Edit URL in the cell's menu; an open pane follows a changed address); dropping a tab onto a pane of a bookmark-bound split is saved into the cell; closing one pane with Ctrl+W leaves the cell unchanged and moves the panes that stay to the tab list (decided 2026-11-11, where the Mac drops the pane from the cell); the hover chip's move rows move the focused pane like the row menu's; and move, swap and Remove from Split are refused for a split a pinned cell or a bookmark stands for. A pinned split takes no drop, as on the Mac. Not done: the Bookmarks page and the Library still edit only a split bookmark's first address. 2026-11-17 (Mac `461fd42d`): the Mac now syncs a bookmark's extra panes (`split_extra_panes`), a pinned split of three or four (`split_pane_uuids`, one pin per pane) and every split's orientation (`split_layout`); Windows syncs pairs only and keeps the rest local. Waiting on a decision (§9 2026-11-17). |
| Edge-to-edge web content (Mac `35f4d29c`, `0eceaa08`, 2026-11-05) | General ▸ Appearance toggle: no outer page margins in every layout, square corners in full screen, a soft page shadow on the sidebar | Appearance ▸ Page margin (off by default): on, an 8 dip margin with the window backdrop around a rounded page card, dropped while maximized or full screen; off, the page runs to the window's edges | ◐ | Revisited 2026-11-24: the 2026-11-10 ⛔ ("nothing to switch") no longer holds since Page margin landed (`29beb69`, GitHub #13). The switch is the same choice the other way round. Gaps: the defaults differ (the Mac shows its margins until Edge-to-edge is turned on; Windows starts flush), and there is no soft page shadow on the sidebar. The full-screen page is square on both sides (Mac `99282330`). |
| Split with New Tab (Tab menu, rebindable shortcut, omnibox pane picker, Hold to add an open tab; Mac `6985cc34`, 2026-10-28) | yes | ⋯ row, Ctrl+Shift+\\ (rebindable, group Tab), New Tab Page mode, Omnibox mode with Hold to add an open tab | ✅ | Ported 2026-11-02. The Mac's Ctrl+Shift+= is zoom-in on Windows, so the chord is Ctrl+Shift+\\. No blank-pane preview, by decision: it has no twin under a hosted HWND. Box-verified on nuc: New Tab Page mode splits, a second chord does nothing, an Omnibox card commit splits. ⋯ row: splits in New Tab Page mode, disabled and refused on a split tab, and from the real popup opens the Omnibox card, whose commit splits. Hold: on a row with an open tab, Ctrl+Enter pairs that tab with no third tab, plain Enter opens a fresh pane. The "Hold Ctrl" hint on the row was not looked at. |
| Modifier clicks: Shift → Peek, Alt/⌥ → split, Ctrl+Alt/⌘⌥ → Kiosk | yes | yes | ✅ | |
| Kiosk window ("Open in" Space, profile chip) | yes, profile chip menu, cursor-zoom open animation, frame autosave | yes, no profile switch from the chip, default size | ◐ | `phi_kiosk_window.h` lists the four pieces as deliberately not ported; the profile-chip switch has no framework call. The page takes keyboard focus on open (`79b11061`, 2026-11-02, see §9). "Open in" reopens the page by URL rather than moving the live tab, and the Kiosk closes only once the page has opened (fixed 2026-11-02: with no user window it used to lose the page and exit Chromium, `74cb2bce`). With no user window the last session is restored first when that setting is on, as on the Mac. |
| Native new-tab page | Phi mark + omnibox, AI suggestions | Phi mark + omnibox | ✅ | AI suggestions ⛔ AI suite. |
| One default surface for the address row, active strip tab, page backing and split gaps (Mac `5e434655`, 2026-10-28) | `extensionSurfaceBackground`: white, or in dark the overlay colour at 20 % | one dark `window_bg`, #2B2B2B, for the address row, strip tab and page backing; Library grounds use the same surfaces | ✅ | Ported 2026-11-02 at #2B2B2B, the value `windows-native-ui.md` §2.3 argues for, not the Mac's near-black. Box-verified on nuc: Mist dark read #0B2938 beside #2B2B2B before, #2B2B2B everywhere after. Library grounds are neutral too, see `294d4d02` in §9. |
| Bookmark manager (native, per Space) | yes | yes | ✅ | |
| Bookmarks bar (always / on new tab) | yes | yes | ✅ | |
| Image preview window (zoom, export original — Mac `1705cb23`, 2026-10-15) | `UserInterface/ImagePreview` | none | 🔧 | Whole surface absent. New on the Mac this cycle. |
| Picture-in-picture: auto Off / Normal / Park at edge | yes | yes | ✅ | `picture-in-picture.md`. Pages without an `enterpictureinpicture` handler (YouTube) pop out too: browser-initiated auto-PiP is on in r158 (`suite-pipchapters`, PHI-2252). |
| PiP window: YouTube chapter segments and the playing chapter's title over the progress bar (Mac 2.3.0) | fork `phi_pip_page_state` + `video_overlay_window_views`; Mirage's YouTube script reports the chapters (`phinomenonPrivate.setVideoChapters`) | the same fork code and Mirage, nothing in the client | ✅ | Triaged 2026-11-27 (PHI-2252, `pip-extras.md`): in official 0.8.0.25, which has Mirage 2.19.0; fork builds with Mirage 2.17.0 (before `6c0a5dc93051`) have the window code but no script. nuc: `suite-pipchapters` passes on 0.8.0.25, fails on fork `e837b182`. Mirage changes go to phibrowser/phi-ai. |
| PiP window: Skip Ad button while YouTube offers a skip (Mac 2.19.0) | fork `skip_ad_label_button` + `PhiPipPageState` (one trusted click on www.youtube.com per press, ADR 0022); Mirage handles the Media Session `skipad` action | the same, nothing in the client | ✅ | As the chapters row. nuc: a real press skips the page's ad with a trusted click, from the visible tab and from a background tab after auto-PiP. |
| Widevine DRM (Netflix, Spotify, Disney+ …) | fork only: component-updated CDM, `Phi Helper (CDM)` signed without library validation, VMP host files (`Phi Framework` + castLabs EVS `.sig`) | component-updated CDM; VMP host files `Phi.exe` + `framework\chrome.dll`, `.sig` beside each from `make-installer.sh --vmp` (EVS, after Authenticode; flag 1 on the exe, flag 0 on the DLL) | ◐ | Ported 2026-10-30 (fork `cdm_host_file_path.cc` + `media_options.gni`, ship args `enable_widevine`). No CDM helper needed (a macOS library-validation matter). Official releases VMP-sign (build repo `vmp-sign.sh`). Official 0.4.0.13 x64, installed on nuc on 2026-11-05, passes `box-tests/suite-widevine.mjs` 5/5: CDM 4.10.3112.0 fetched by the component updater, license request, Shaka Widevine playback, host verification ran, and Google's UAT license server answers `PLATFORM_SOFTWARE_VERIFIED`. Spotify plays on official 0.4.0.14 x64. On arm64, 0.4.0.14 and earlier crashed every web page (Aw, Snap, STATUS_BREAKPOINT) on GitHub's hosted Cobalt 100 VM: the Linux cross-build's snapshot toolchain built V8's builtins with pointer authentication that chrome.dll's V8 couldn't read. Fixed in 0.4.0.15 (build repo `args/PhiFxWinShipArm64.gn`), which passes the page check and the Widevine suite 6/6 on a Cobalt 100 VM (build repo `arm-smoke.yml`, CI.md). Real Arm hardware not checked. Test releases stay unsigned, so services that demand VMP refuse them a license. |
| Sidebar Mini Player / native media controls | play/pause, seek, tracks, volume and mute, PiP, show tab, dismiss, source switching; setting Off / Auto / compact / expanded | the same card on the sidebar's bottom bar (docked and floating), same selection rules and heights; Settings ▸ Advanced ▸ Mini Player | ◐ | Ported 2026-11-13 (GitHub #12): fork `phi_framework/phi_media_controls` + ABI 6, client `phi_mini_player`; `docs/native-media-controls.md`. gmk: `suite-miniplayer` 11/11 (fails 10/11 on a build without it), scenario `mini-player` PASS. Gaps: analytics (`mini_player_action`, launch properties), keyboard/screen-reader access, trackpad swipe (a 40 dip mouse drag cycles instead). The Space-exit auto-PiP wait (`55d1519a`) was ported 2026-11-27 (PHI-2252, `pip-extras.md`): nuc `suite-pipspaceexit` fails on dev 27dd678 (the card showed 54 to 371 ms after the switch) and passes with it. Ships only with a new framework release (ABI 6). |
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
| Rename in place: Spaces, profiles, the account name (Mac `5e8905b4`, 2026-11-16) | the row's name (double-click, Return or the rename button) or the account name (pencil or double-click) becomes a field with the name selected; Enter saves, Escape, a click outside or losing focus cancels; an empty or over-long account name, or a duplicate profile name, keeps the field open with the reason in red | the same on Settings ▸ Spaces (double-click a row, Return on the selected row, the bar's rename button), a profile's Name field and the account name | ✅ | Ported 2026-11-17, see §9. Before, each opened a modal rename prompt. A scroll or a switch to another pane while editing cancels. The Mac's create-profile prompt with an inline duplicate check is not ported. |
| Profiles: list, search engine, download location, Folio folder, data links | yes | yes | ✅ | 2026-11-17 (Mac `b0b3cd4c`): a profile no Space uses greys its data links with the Mac's note, since they open in a window of that profile only. The other half of that commit (a Chromium window on such a profile is closed, not shown as another profile's Space) did not reproduce on Windows: the window is parked as a ghost (§9). |
| Profiles: Content blocking (Block ads / trackers / cookie banners, rule-set chooser, custom filters) | per profile, `docs/content-blocking.md` | Settings ▸ Profiles (and General with one profile): the three switches with their rule set button and summary, the status small print; the rule set chooser (`PhiContentBlockingWindow`: checkboxes, download with progress and errors, Update Downloaded, list details, custom lists) and its Custom filter form (URL or pasted rules); the site menu's per-site row | ✅ | 2026-11-16, framework ABI 9 (`PhiFramework` content blocking calls over the Mac bridge's `bridge_api`, `OnContentBlockingReply` / `OnContentBlockingChanged`), facade `client/phi_content_blocking.*`, launch snapshot keys `block_*_enabled`. Defaults stay off. The site row still shows only while a switch is on (the Mac shows it always; with nothing on it would claim blocking that is not happening). The list details are a dialog, not a popover. Needs the framework release with ABI 9 and a `framework.lock` bump before main can ship it. |
| Navigation: Kiosk toggles, Peek toggles, URL rules | three Kiosk toggles; Peek: enable, automatic, same-site links, panel size (`2fec54f4`) | one Kiosk toggle; all four Peek rows | ◐ | The two missing Kiosk toggles are ⛔ (see §1). Peek's "Also peek links to the same site" (framework ABI 10) and "Peek panel size" Large / Medium / Small (this device only) ported 2026-11-17, see §9. |
| Phi & AI pane | yes | built, not mounted | ⛔ | AI suite. |
| Phi Link (Telegram relay) | yes | none | ⛔ | AI suite (Sentinel relay). |
| Sync: status, set up, devices approve/deny, remove device, reset sync from the last device | yes | yes | ✅ | `user-data-parity.md`. Reset (`055df916`: `PUT /keys/v1/reset-operations/<uuid>`, journaled in `marker.json`, resumed by "Set up sync again") ported 2026-10-27 as the Settings Removal row; the setup window's remove / reset offer is not built (Windows has no remove control there). |
| Shortcuts: grouped table, search, record, reset (all + per row), unassign, conflicts, Go to Space rows, effective chords in menus | yes (shortcut engine `896b36d0`, Oct 2026) | yes | ✅ | 2026-10-27: localized titles, Reset All behind a confirmation, Delete-to-unassign + per-row Reset, Chromium page commands rebindable, ⋯ menu shows effective chords, displaced default chords swallowed. Deliberately not ported: informational conflicts / filters / system-hotkey warnings (Windows keeps confirm-and-steal), `excludedKeys`, Kiosk global hotkey (⛔). Kiosk & Peek group rows absent; the numpad zoom twins stay fixed and unlisted. |
| Advanced: restore session, full URL, previews, indicators, shorten links, Mini Player, auto PiP, developer mode, Chromium settings | yes | all but Shorten links (⛔) | ◐ | Rows regrouped into Appearance / General on Windows. The full URL row (under Appearance) drives the address field since 2026-11-04; see §3. The Mac's new tab-actions card (double-click to close, positional closing) belongs to the §2 rows of those names. |
| Developer: Allow agents (CDP), per-agent Allowed / Blocked lists, "Allow all agents" | yes | one switch | ⛔ | No code-signature peer identity on the Windows pipe (`phi_settings_window.cc`, `phi_control_server.cc`). Revisit if the pipe gains a caller identity. |
| Developer: Allow agents to operate Spaces, operated-page rendering, profiles agents can use, install skill | yes | yes (no "exact rendering" toggle) | ◐ | The operated-page mask toggle has no Windows row. Ported 2026-11-04 (Mac `c3149697`, §9): the switch now also gates shadow windows, and turning it off closes them. Still different from the Mac on purpose: it is off by default, credential verbs come with admission, and the description says so. |
| Developer: Agent Password Manager (Bitwarden sign in / unlock / timeout), Agent Credential Approvals (standing grants, revoke) | yes, `docs/bitwarden-password-manager.md` | helper process + `credentials.*` verbs + Allow once / 10 min prompt; no Settings rows | ◐ | `PhiBitwardenHelper.exe` and the broker are in; the sign-in sheet, session settings, approvals list and "Always" grants are not. `analytics.md` records `approval_type: always` and `kind: run` as not ported. |
| About page | About window, with a conch to drag since Mac `094e85c8` (2026-11-06) | About page + notice | ✅ | The conch is left out (decided 2026-11-11): it plays 8.5 MB of frame images, and Windows dropped its conch frames on 2026-11-02 for the membership card. |

## 5. Onboarding, account, data

| Feature | Mac | Windows | Status | Gap / decision |
|---|---|---|---|---|
| Onboarding: welcome, name, sign in / guest, consent, layout, next steps | yes | yes, with Windows-native footer buttons | ✅ | `login-port.md`. The Mac's 2026-10-28 refresh (`28c0618b`) ported 2026-11-02: Back on every later page with Escape as Back, "Sign in instead", the legal wording, Up and Down on the layout list, the email-derived name prefill (`1cf61180`), Omnibox as the guest default (`5196706b`). Decided: footer buttons stay Windows-native rather than the Mac's pills, and the guest list leaves out AI features that are off on Windows. Next Steps still says "Onboard your AI assistant", which the Mac keeps too. Box-verified on nuc, signed in and as a guest. |
| Onboarding keyboard and screen reader (Mac `8d998ebc`, 2026-10-30) | VoiceOver reads page titles, steps and a card summary, and hears page changes | Tab moves through drawn controls with a focus ring, Space and Enter activate them, page titles, bullets and steps are UIA text, the card and name field are named, page changes are announced | ✅ | Ported 2026-11-02, box-verified on nuc through UIA and real keys on every page: the consent pages finish keyboard-only and the consent row's name is right. No Narrator run. |
| Onboarding: browser import step | Chrome / Safari / Arc / Dia / file | "Import browser data" link under Next steps | ◐ | A dedicated step with source picker is missing; see §1 import row. |
| Onboarding: password manager choice (iCloud Passwords) | yes | skipped | — | iCloud Passwords is Mac-only (`phi_onboarding_window.cc`). A Windows equivalent (Windows Hello / Credential Manager) would be a new decision. |
| Guest mode + guest → account migration | yes | built, not live-tested | ◐ | `user-data-parity.md` S6. The Mac explains an unavailable Guest store at sign-in since 2026-11-22 (`fd93aae4`, `oobe.guestMigration.failure.guestDataUnavailableMessage`); Windows has no such message. |
| Hosted login (Auth0), reauthentication, sign out | yes | yes, off-the-record sign-in window | ✅ | |
| Sync keys: recovery code, device approval, profile / Space matching, sync method (Merge or Use your account's data, Mac `40312627`) | yes | profiles page only | ◐ | Corrected 2026-10-27: the Windows pairing wizard has no Spaces step and no method / replace step (`user-data-parity.md` S5 "Not yet"). No wire change — a Windows peer sees a Mac join as before. The recovery-code confirmation page, its durable pending flag (Mac `32799547`) and its Back (`a8a8f23e`) ported 2026-11-02, see §9. The warning about losing the code and the "Forgot your recovery code?" Help link (`a7ac1c07`) ported 2026-11-04, see §9; the Help article itself names only the Mac. |
| Crash feedback ("Phi quit unexpectedly") on next launch | `docs/crash-feedback.md` | Crashpad → Sentry, no prompt | 🔧 | Needs the last-run status from Crashpad and the existing Send feedback form with the crash id. |
| Send feedback: files, pasted images, diagnostics, outbox | 10 × 20 MiB, Sentinel logs | 5 × 10 MB, week-long outbox | ✅ | Sentinel logs are ⛔ AI suite. |
| Sync problem telemetry (Mac `9921ac9a`, 2026-11-03): `sync_attention_entered` / `_snapshot` / `_cleared`, held counts by reason, sync counts in the launch snapshot, sync keys in feedback | PostHog, Sentry and feedback, enum values and counts only (Mac `docs/sync.md`, "Sync status contract") | `user_defaults_snapshot` carries `sync_enabled` and the three counts from a local mirror; feedback carries `sync_phase`. No `sync_attention_*` events and no attention keys in feedback | ◐ | Partly done 2026-11-04, see §9 port 7. The reporter was not built: this engine has no held counts by reason, no problem categories and none of the helper conditions. No Sentry half. |
| Account deletion local data removal | yes | none | 🔧 | With the Delete account row above. |
| Sign out discards the session (Mac `8655ec6b`, 2026-11-09) | Sign out marks a discard before closing windows; the next launch or the next sign-in or Guest entry empties Session restore and Undo Close for every profile, so the next account does not get the previous account's windows | Sign out marks the same discard (`MarkSignOutSessionDiscard`). Windows keeps its windows hidden behind the login gate rather than closing them, so at the next sign-in or Guest entry it closes every tab they hold, then consumes the mark (`PhiCoordinator::DiscardSignedOutSession`) | ✅ | Ported 2026-11-10, run on nuc 2026-11-11 with a stand-in session (no tenant contact). Same run: Sign out, then Continue as Guest closed all 6 tabs in two Spaces and Ctrl+Shift+T brought nothing back. Relaunch: Sign out, quit, launch emptied the session files and the guest got one new tab; without the Sign out the same quit and launch restored the tabs. The account's Spaces stay. Framework ABI 3. |
| Membership card: holographic foil card with member ID (SHA-256(sub)[0:8]), "Date Joined", random style + PNG export, on the onboarding Welcome page and Settings ▸ You (Mac `6b300dc0`, `cda12eaf`, 2026-10-20/26) | `HolographicCardView` | `PhiHolographicCard` (`client/phi_holographic_card.cc`): the same layer stack composed in software — palette base + iridescence + radial, one of 6 procedural foil textures, seeded grain, logo + sweep, one of 6 seal ornaments, sheen, edge; pointer light + tilt, entrance spin, flat PNG export at 2× on the accent canvas | ◐ | Ported 2026-10-27 (`windows-native-ui-impl.md` Round K); the Mac has moved since, see the end of this row. Foil / grain / seal masks are rasterised once per style and size with GDI+ on a worker and cached as PNG under `<user data>\PhiCardArtwork\v1`; the light-driven layers recompose per frame. Member id from `auth().user().sub`, join month from the profile's `created_at`, style in the account defaults under the Mac's `profileCardStyle` key (the Settings card keeps its own palette since 2026-11-02, `40ede7a7`). The conch frame sets (12.5 MB) are gone. Not ported: the Mac's `.random()` seeding details (both are uniform), IvyPresto Headline Light now ships for the name. Mac 2026-10-27/29 follow-ups ported 2026-11-02 (`92816ab6`, `40ede7a7`, `ba180400`, `1cf61180`, `8fb61ef5`): no radial highlight, a dark logo at rest, no printed member ID, no shadow in Settings, a style saved as soon as it is randomized. 2026-11-10: look refreshed to Mac `0b1ee030` (foil on both faces, hairline edge, "Member Since" in Share Tech Mono, a random style always has an ornament); looked at on nuc 2026-11-11, on the Welcome page and in Settings ▸ You, the back face caught during the entrance turn. |

## 6. Agents and automation

| Feature | Mac | Windows | Status | Gap / decision |
|---|---|---|---|---|
| Agent Spaces: overlay, fog, ring, pill (Take control / Hand back / Finish), "The agent needs you", Autoview | yes | yes | ✅ | Agent cues D26–D30 deferred: brand accent per agent, pulsing ring, window title, ⋯ on the capsule, omnibox dim/eye. |
| Agent viewport follows the agent Space's own page area (Mac `1c39d48c`, 2026-10-29) | `agentSpace.panelSize` answers for the aimed Space, re-measured before each action | same; `VerbAgentPanelSize` answers in CSS pixels for the shell hosting the watched Space, else the focused user window, else the parked agent shell | ✅ | Ported 2026-11-02, box-verified on nuc at 300 % and 150 %: an agent tab matches a real tab's size hidden and watched, and follows sidebar and window changes at the next action. Before, the skill's un-aimed call was refused (`scope_not_allowed`), so agent pages laid out at the parked shell's fixed frame (1027×684 at 300 %, 1386×849 at 150 %) whatever the user's window was. The skill names the task, not the Mac's `windowId`, which resolves to the user's shell while watched. Hand-off fixed the same day, see `1c39d48c` in §9: an exact-size override made Chromium restore the hidden frame's size on clear (fork `web_contents_impl.cc`), and the pill's Take control left the page pinned until the agent exited (skill `ownershipChanged`). Also verified: several windows of different sizes, a visible bookmark bar, the Horizontal layout, no user window. 2026-11-11 (Mac `ff8e9238`): a hidden Space now answers with the page area it holds (the first one measured, then the last one it had on screen), so collapsing the user's sidebar no longer reflows the agent's page. nuc: 848 wide before and after hiding the sidebar, where a task first measured then got 1088. Proposed 2026-11-24 (Mac `dfbcc7d5`, PHI-2155): the skill re-measures about once a second through the whole round, so an idle agent's viewport follows too; the Windows `helpers.mjs` measures before each action only. |
| Agent Transcript console and the skill's session mirror | View menu, pip "Show Transcript"; the skill's mirror daemon and Pi companion extension | not on Windows | ⛔ | Decided off 2026-09-09; the console, its store, the skill's mirror scripts and the Pi companion were removed 2026-11-12. `agentSpace.log` is still accepted and its lines dropped; `agentSpace.list` reports `transcriptMirror: false`. |
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
| Folio: Save to Folio, Open Folio, auto-save on site actions, Add Note | yes; never in an incognito window or the Incognito Space (Mac `5d08a38b`, 2026-11-20) | yes, also off the record | ◐ | `folio-port.md`. Gap found 2026-11-24: `PhiFolio::CanSave` has no off-the-record check, so Save to Folio, Open Folio and auto-save run in incognito windows and the Incognito Space, keeping a durable copy of private browsing. Port proposed (§9), S, client. |
| Folio library | Mirage `library.html` — the Mac DELETED its native library 2026-10-19 (`45a10144`; `docs/folio-native-library.md` now says the extension page is the only Folio UI) | Mirage `library.html` (`PhiFolio::OpenLibrary`) | ✅ | The 2026-10-19 "implement the native library" decision is void: both sides open `library.html`. The Library surface's Folio section (§2) hosts the same page. |

## 7. Summary (2026-11-24)

| Status | Rows |
|---|---|
| ✅ ported | 73 |
| ◐ partial | 32 |
| 🔧 to implement | 25 |
| ⛔ deliberately left | 21 |
| ❓ needs a decision | 0 |
| — not applicable | 4 |

Counted from the tables on 2026-11-24. Before this pass the tables held
72 / 28 / 24 / 22 / 0 / 4, where this summary still said 70 / 29: rows
changed on dev after 2026-11-17 (the layouts row, the Space switch rows)
without the counts. This pass: tab audio 🔧 → ✅ (a correction), Space
icons and Folio ✅ → ◐, edge-to-edge ⛔ → ◐, three new 🔧 rows (capture
indicators, the background-tab toast, the unified omnibox). Since then:
Capture Full Page 🔧 → ◐ (PHI-2251, 2026-11-27); two new ✅ rows, the PiP
window's YouTube chapters and Skip Ad, already in 0.8.0.25 through the fork
and Mirage (PHI-2252, 2026-11-27).

**The 🔧 list, roughly by user impact:** multi-selection ·
Bitwarden Settings rows and standing grants ·
crash feedback prompt · image
preview · Arc/Zen migration wizard · Share ·
Export/Import user data · account export + deletion · invitation code
(backend) · full screen · client-side
external-link hook (Kiosk preference, App rules, return to the sending
app) · `agentSpace.app.info` ·
bookmark Separate/Back-to-saved (the slash is there) ·
What's new / Documentation / Improve
translations rows · Export logs / Extension info rows · settings deeplink ·
sync problem telemetry · (2026-11-14) sidebar on the right · (2026-11-17,
each waiting on a decision) drag-to-split in the sidebar, folder / Space
hover preview, Top Buttons and Space icons in the header · (2026-11-24)
the background-tab toast (port proposed); waiting on a decision: the
unified omnibox, tab capture indicators with "Share this tab instead".

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

### 2026-11-24 — phibrowser-mac `b9b16fed` → `93b1d0aa` (84 commits, 4 merges)

Triage only (PHI-2204): read every diff and the Windows code on `origin/dev` `5ec533e` (and the
fork on `phi-r158` where a Mac commit touched it); nothing built, run or ported. The range ends one
commit past PHI-2204's `01be27ec`: `93b1d0aa` landed on dev on 2026-11-24. `release/2.18.0` and
`main` (`8a4fcd21`) hold nothing that dev lacks. Four commits re-apply last pass's commits onto
the release branch (`cab78990` = `95a537c0`, `3b00b1c3` = `ffe02f70`, `01c8d189` = `461fd42d`,
`64efce74` = `b9b16fed`) and keep last pass's verdicts. Six commits dated 2026-10-29 to 11-04 are
the screen-share branch merged on 2026-11-23 (`7e047411`).

Verdicts: 15 port, 10 decide, 5 watch, 10 done, 44 skip. Most skips are the Mac's sidebar
performance work (AppKit rows, the animated column, the render-server slide), which has no twin:
Windows collapses the docked sidebar at once and paints its rows itself. Ledger changes: three
new 🔧 rows (§2 capture indicators, §2 background-tab toast, §3 unified omnibox); §2 Space icons
and §6 Folio go ◐; §3 edge-to-edge goes ⛔ → ◐ (Windows has Page margin since `29beb69`); §2 tab
audio corrected 🔧 → ✅ (ported 2026-11-15).

**Proposed ports, by value** (each S unless marked, client only, no fork, no ABI):

| # | Mac | What, where | Box test that proves it |
|---|---|---|---|
| 1 | `5d08a38b` | No Folio off the record: hide Save to Folio / Open Folio in an incognito window and the Incognito Space, refuse the chord, no auto-save (`PhiFolio::CanSave`, `phi_app_menu.cc`) | in the Incognito Space the ⋯ menu has no Folio rows, the Save to Folio chord (Ctrl+Alt+S) saves nothing, an auto-save site saves nothing; a normal Space still saves |
| 2 | `739fd939` | The Mac's Heroicons v2 icon set: 70 new ids and the old 88 redrawn under their ids (`phi_icons.cc`, `phi_icon_masks.cc`, picker order). M | a Space set to a new id (`phi:phi-icon-banknotes`) draws a glyph in its pip and the picker; the old ids still draw |
| 3 | `dfbcc7d5`, `1f90982b` | Skill `helpers.mjs`: the round-long page-area follower; stop a navigation that never commits | an idle agent Space resized while hidden fills the window when watched; `goto` to a server that never answers fails in time and the next round attaches |
| 4 | `1f585bac`, `919ba4cb` (+ `c397227c`) | "New Tab Created" toast for a background link tab while the sidebar is hidden, the whole toast clickable (`OnUserOpenedLinkTab`, `phi_toast`). S–M | sidebar hidden, Ctrl-click a link: the toast shows; clicking it activates the new tab; sidebar shown: no toast |
| 5 | `dacfd786`, `01be27ec` | The floating panel shows and tucks at once when the system's Animation effects are off (`SPI_GETCLIENTAREAANIMATION`) | with animations off the panel is at its resting rect on the first frame |
| 6 | `c813ad28` (+ `0cb50faa`) | A hover "…" on the Space root row opening the trimmed tab-area menu (`phi_sidebar_view.cc`, `PhiBuildTabAreaMenu`) | hover the root row: a named button; the click opens New Tab, New Folder and the Space rows without Edit Theme |
| 7 | `0416448a` (half) | Ctrl+S with the pointer on the docked sidebar leaves the floating panel out in place; dock and undock keep the list's scroll | pointer on the sidebar, Ctrl+S: the panel is up, rows where they were |
| 8 | `996edcb1` (half) | Ignore key repeats for the sidebar toggle; check first that holding Ctrl+S does flip it repeatedly | a held Ctrl+S (repeat keydowns) toggles once |
| 9 | `234efbac` (+ `0cb50faa`) | Settings ▸ General: At the top / At the bottom cards with pictures in place of the "Open new tabs at top" toggle | the cards are named and selected by `newTabsAtTop`; a click sets it |
| 10 | `29179ff9` (half) | A pointer that ran from the floating panel past the window's sidebar edge keeps the panel out | leave the window's left edge from the panel: the panel stays |

**Waiting on the user's decision:**

| Mac | What | Size |
|---|---|---|
| `8820a54e`, `a35cb0f7` | the unified omnibox (pins, bookmarks, splits, commands as rows), out of Canary on the Mac | L, fork + client, ABI |
| `00c25130` … `75d5f1c7` (8 commits) | tab capture badges, the shared-page outline, "Share this tab instead" on hover | L, fork (capture flags + tab sharing proxy `ee1384d0b39` export) + client, ABI |
| `01c8d189` | 3–4 pane split sync (re-apply of `461fd42d`, still open from 2026-11-17) | bookmark half M, pin half L |

**Worth a box check** (no port yet): `02b41cee`. An external link that a rule sends to a Space
shown in another window probably switches the last active window on Windows (§1 external-link
row).

| Mac commit | Subject | Verdict | Windows action / reason |
|---|---|---|---|
| `374fcb27` | fix: stop long sidebar scroll hitches when tab groups scroll into view | skip | AppKit: a group row's nested NSTableView rebuilt on reuse. Windows paints a group's members as flat rows. |
| `b98e0520` | improve: cut layout work for each row scrolled into the sidebar | skip | AppKit key-view loop and SwiftUI row layout. |
| `0d2cd0a7` | improve: set up sidebar tab previews on first hover | skip | AppKit row setup. Windows builds a hover preview only on hover already (`phi_tab_preview`). |
| `90c72aa6` | improve: restore responsive scrolling in the sidebar tab list | skip | NSScrollView responsive scrolling. |
| `e97da0e8` | fix: stop the sidebar jumping when selecting a tab inside a long tab group | skip | AppKit: focusing a member scrolled the whole nested group cell. The Windows reveal targets the member's own flat row. |
| `0a95bd79` | fix: release a closed tab group's sidebar cell right away | skip | AppKit group-cell cache leak. |
| `9fc008f9` | improve: smooth the sidebar collapse and expand animation | skip | The Mac's animated sidebar collapse. Windows collapses the docked sidebar at once (`SetSidebarCollapsed`), with no slide, so there is no per-step work to cut. |
| `996edcb1` | improve: let the sidebar toggle respond to every press | port | Half: holding the toggle chord flips the sidebar once. Windows passes key repeats through (`phi_keyboard_accelerators.cc` reads bit 30 only for the Alt tap), so holding Ctrl+S probably flips it on every repeat. Check on a box, then drop repeats for kCmdToggleSidebar. S, client. The other half (retarget a running animation) has no twin. |
| `1f585bac` | feat: show clickable background tab toast when sidebar is collapsed | port | New §2 row. When a link opens a background tab while the sidebar is hidden in a vertical layout, a clickable "New Tab Created" toast (top trailing) switches to it. Windows: the toast from `PhiShellWindow::OnUserOpenedLinkTab` (fork hook exists) when `sidebar_collapsed_`, plus a whole-toast click in `phi_toast`. S–M, client. |
| `bde0d73c` | fix: keep the floating sidebar open while hovering a folder preview | watch | §2 hover preview row (🔧, waiting on a decision): the floating panel stays out under a folder preview. |
| `cab78990` | fix: open a URL rule match in the window it was clicked in, switching that window to the target Space, instead of in another window already showing it | done | Re-apply of `95a537c0` (§9 2026-11-17): client `7348b4a` + fork `b7b1ce681f5`. |
| `3b00b1c3` | fix: restore a Peek opened over a split pane over its split instead of as a normal tab | skip | Re-apply of `ffe02f70`: Windows does not restore Peeks. |
| `c397227c` | i18n: translations for the release/2.18.0 strings up to 3b00b1c3 (background tab toast) | port | Translations for `1f585bac`'s string; lands with that port (`tools/gen-strings-catalog.py`). |
| `919ba4cb` | fix: activate background tabs correctly from toast | port | With `1f585bac`: activate through the framework (the tab may be in another split) before the sidebar follows. |
| `01c8d189` | fix: sync split views of three or four panes and split orientation for bookmarks and pinned tabs (#362) | decide | Re-apply of `461fd42d` (3–4 pane split sync); still waiting on the user (§9 2026-11-17). |
| `a47a8c40` | fix: close the floating sidebar when a window resize or full screen leaves the pointer off it (#367) | done | Windows re-reads the pointer every 30 ms while the panel is out (`kFloatingTickTimer`), so a resize or full screen that leaves it off the panel tucks the panel. |
| `4bcad6e4` | fix: keep arrow-key selection in the Spaces and Profiles settings lists after switching panes (PHI-2051) | skip | SwiftUI `FocusState` bug. The Windows Settings lists are drawn by the Settings window, which keeps the keyboard across panes (`SetFocusItem`). |
| `032a3b4e` | fix: keep the drop line on the tab side of New Tab when dragging a tab group | watch | §2 drag & drop row, D33: Windows has no whole-group drag yet. When it lands, its gap must stay on the tab side of the New Tab row. |
| `64efce74` | fix: keep the sidebar drop line on the tab side of the New Tab row | done | Re-apply of `b9b16fed`: Windows draws a gap before the New Tab row (box check 2026-11-17). |
| `00e54030` | fix: align the sidebar Spaces strip with the address bar and pinned tabs | done | The Windows strip already insets by `kSpacesStripPad = kEdgeInset` (8 dip), the address row's and the pinned grid's inset. |
| `09377313` | fix: prevent crashes when purging Time Machine backups | skip | Time Machine backups, ⛔ §1. |
| `9f0dcdcf` | fix: dock the sidebar at the floating panel's width after relaunch | watch | §2 floating sidebar row: the edge resize (`bb4082e1`) is waiting on a decision. Without it the Windows panel has no width of its own. |
| `8b570893` | fix: prevent sidebar crash on tab focus after pinning or bookmarking a visible tab | skip | NSOutlineView item hashing. |
| `ff7186aa` | fix: show the traffic lights only after the sidebar finishes opening | skip | Traffic lights. |
| `cc786458` | improve: use the system traffic lights in the floating sidebar | skip | Traffic lights. |
| `9b4bfcd1` | fix: float the active split bookmark when its folder is collapsed | skip | Windows has no floating row under a collapsed root or folder, by decision (§2 Space bookmark root row). |
| `5779a8d1` | fix: treat splits consistently in tab switching shortcuts | done | Next / Previous Tab step through a stored split's open panes (`PhiSidebarView::StepTab`, `1a7315f`, PHI-2067). The Cmd+1–9 half is ⛔ §1 (Ctrl+1–9 switches Spaces). |
| `da714d1a` | fix: keep the page inside its card while the window shrinks or the sidebar opens | skip | The Mac content surface outran its card. The Windows page is a child HWND that clips its own frames. |
| `03368767` | chore: remove the split views' unused delegate methods and draw override | skip | AppKit cleanup. |
| `caa7b60b` | fix: allow tearing tabs off from the floating sidebar into a new window | skip | Windows decides a tear-off by the window rect for the docked and the floating sidebar alike; the page band takes a split drop (`DropBandAt`, §4.8). |
| `6329b469` | improve: make the sidebar toggle start fast and ease out | skip | Toggle animation curve. Windows has no toggle animation. |
| `8820a54e` | feat: search pins and bookmarks in the omnibox in all builds | decide | Unified omnibox out of Canary on the Mac. This was watched since 2026-11-14 until "the Mac takes it out of Canary"; that has happened. New §3 row. L, fork (`IsUnifiedOmniboxEnabled`, providers, framework session calls, ABI) + client. |
| `cd45bf26` | docs: refresh README for the new Phi Browser positioning | done | Windows README refreshed the same day (`6d5ee48`). |
| `0416448a` | feat: pin and unpin the sidebar in place from the floating sidebar | port | Half. Unpinning with the shortcut while the pointer is on the docked column should leave the floating panel out in place, not tucked; docking from the panel should keep the list's scroll position. Windows today: Ctrl+S tucks the panel and the pointer has to go back to the edge. S, client (`SetSidebarCollapsed`, floating panel show). The morph animation has no twin. |
| `c813ad28` | feat: add a more menu to the sidebar Space folder row | port | Space root row: a hover "…" button opening a trimmed tab-area menu (New Tab, New Folder unless incognito, the active-Space rows without Edit Theme). Windows: `space_root_` in `phi_sidebar_view.cc` and a trimmed `PhiBuildTabAreaMenu`. S, client; `sidebar.spaceRoot.moreButton` string. |
| `a36be0ec` | fix: prevent floating sidebar crashes on quit and restart | skip | AppKit teardown crash. |
| `487c224f` | fix: hide the dot at the sidebar and chat sidebar edges | skip | NSSplitView divider style. |
| `1337f965` | fix: keep each page inside its own card while the window animates into or out of full screen | skip | Mac content-surface mask during the full-screen animation. Windows has no such animation. |
| `80060da7` | perf: skip the full screen mask outside transitions and document it | skip | Same. |
| `dfbcc7d5` | fix: keep the agent viewport following the page area while the agent idles so a watched Space shows no blank strip (PHI-2155) | port | Windows skill (`tools/phi-browser-skill/scripts/lib/helpers.mjs`): add the round-long `followPageArea` timer, so an idle agent's viewport follows the page area. S, skill only; check on a box by watching an idle agent Space after a window resize. |
| `a35cb0f7` | chore: document omnibox commands filling vacant rows | decide | With `8820a54e` (unified omnibox doc). |
| `1fc0ec18` | Update app icon appearance and edge coverage | skip | macOS Icon Composer settings (`PhiIcon.icon`). |
| `234efbac` | improve: pick new tab position with illustrated cards in General settings | port | Settings ▸ General: "Open new tabs at top" becomes two illustrated cards, At the top / At the bottom, still `newTabsAtTop`. Windows: the toggle at `phi_settings_window.cc:1453` → two picture cards like the layout picker (PHI-2105), two illustrations. S, client; three strings. |
| `5c1aa0a1` | chore: show right-sidebar entries only in nightly builds | skip | The Mac hides the right-sidebar option outside nightly. Windows has no right sidebar; §2 row noted. |
| `0cb50faa` | i18n: translations for the release/2.18.0 strings up to 5c1aa0a1 (new tab position picker, Space row more button) | port | Translations for `234efbac` and `c813ad28`; land with those ports. |
| `9bef2cf2` | fix: keep pinned tab emoji icons from being clipped | done | Windows already sizes an icon emoji by its ink, not its em (`PhiDrawColorIcon`). |
| `8a4fcd21` | Merge pull request #391 from phibrowser/release/2.18.0 | skip | Merge of release/2.18.0 into main, no change of its own. |
| `609caf37` | Merge main into dev | skip | Merge of main into dev. |
| `5d08a38b` | fix: hide Folio in incognito windows and the Incognito Space, and never save incognito pages to Folio (PHI-2157) | port | Folio in incognito (PHI-2157): Windows offers Save to Folio / Open Folio and auto-save in incognito windows and the Incognito Space (`PhiFolio::CanSave` has no off-the-record check). Hide the ⋯ rows there, refuse the chord, skip auto-save. S, client. |
| `02b41cee` | fix: open a link from another app that a URL rule sends to a Space in the window already showing that Space instead of switching the last active window (PHI-2134) | watch | §1 external-link hook (🔧). Windows probably has the same bug: an external link comes in through ProcessSingleton with the last active window as its source, and `RouteUrlToSpace` keeps the link in the source window, switching it even when another window shows the target Space. The client cannot tell the link came from another app (row "Open external links in Kiosk"). Not checked on a box. |
| `5b68675b` | fix: clear keyboard focus before the Library Spaces create card, Library window or Settings tears down its SwiftUI content (PHI-1968) | skip | SwiftUI focus teardown. |
| `9576ebe8` | fix: keep the window still when dragging a split view divider, including its top inside the titlebar strip (PHI-2171) | skip | macOS titlebar drag region over the split divider; the Windows divider is inside the page HWND. |
| `46a81d62` | fix: fit the onboarding window and the first browser window to the screen's usable area on small displays (PHI-1472) | done | Onboarding scales its page to the work area (`PhiOnboardingWindow::FitPage`, 2026-11-16), and the first window is clamped to `rcWork` (PHI-2176). |
| `a8773e98` | improve: save Cmd+D bookmarks without opening the modal editor and show a toast with an Edit button (PHI-1744) | done | PHI-2202 (`494325d`): Ctrl+D saves without an editor, with the "Added to Bookmarks" / "Already Bookmarked" toast and an Edit button. |
| `1f90982b` | fix: stop a stuck page load when Page.enable stalls so agent Space rounds recover instead of timing out (PHI-2158) | port | Windows skill `helpers.mjs`: `stopStuckNavigation` before a re-attach, and `goto` stops a navigation that never commits (PHI-2158). S, skill only. |
| `58dc96e1` | fix: bind sidebar rows to their own Space's theme and rebind on appearance changes so rows built during a Space swipe don't keep another Space's colours (PHI-1863) | skip | SwiftUI theme environment captured during a swipe. Windows paints each row from its slot's palette at paint time. |
| `1b6530e9` | fix: return to the app the global Kiosk was opened from when it closes instead of raising the main window (PHI-2038) | skip | Global Kiosk shortcut, ⛔ §1. |
| `494dab6a` | fix: hand focus from a lone picture-in-picture window to the last browser window when Cmd+Tab or the Dock activates Phi (PHI-2144) | skip | macOS app activation (Cmd+Tab or the Dock raises the PiP window). On Windows Alt+Tab and the taskbar raise the window the user picks. |
| `da036e8a` | fix: retire pre-import Chromium windows no imported Space maps to on the first launch after a user-data import instead of parking them as ghosts (PHI-1832) | watch | §1 Export / Import user data (🔧): retire pre-import windows on the first launch after an import. |
| `fd93aae4` | fix: move a local store whose schema matches no shipped version into a compatibility backup and reopen it empty instead of failing every launch, and explain an unavailable Guest store at sign-in (PHI-1674) | skip | The SwiftData store from 0.x installs matches no schema. The Windows SQLite store (`phi_local_store.cc`) has no such stores. The Guest-store message half watches §5 guest migration. |
| `739fd939` | feat: redraw the icon picker with Heroicons v2 mini and add 70 icons | port | The Space / pin / folder icon picker: 70 Heroicons v2 icons added and the old ones redrawn, keeping their ids. Windows has the 88 old ids only (`phi_icons.cc`), so a Mac Space or pin synced with a new icon has no icon here. Port the masks (regenerate from the Mac's SVGs) and the picker order. M, client. |
| `e4c5f491` | chore: add a debug FPS monitor and a sidebar toggle and scroll bench | skip | Debug FPS monitor and bench scripts. |
| `2061e633` | improve: smoother sidebar scrolling with AppKit tab and bookmark rows | skip | AppKit row rewrite (NSHostingView → NSView). Windows rows are drawn already. |
| `64c1156b` | improve: keep hidden Spaces and hidden views out of sidebar toggle work | skip | AppKit hidden-Space layout work. |
| `95a47792` | improve: slide the sidebar on the render server with a custom shell split | skip | Render-server sidebar slide. Windows has no docked slide. |
| `e122bd14` | improve: stop per-step layout work during window resize | skip | AppKit per-step resize work. |
| `edcd673e` | fix: keep the page in step with the sidebar while it slides | skip | Mac page lag during the slide. |
| `3a0e10c6` | improve: smoother sidebar scrolling, toggling and window resizing | skip | Merge of the sidebar performance branch. |
| `c3c1387b` | feat: add debug logging for capture flags in Tab class and new script for building with logging enabled | skip | Debug logging for the capture flags. |
| `07cdf373` | feat: add environment variable for debug capture flags in OpenSource scheme | skip | Xcode scheme variable. |
| `00c25130` | feat: enhance tab capture indicators with new badges for media capture and screen sharing | decide | Tab capture badges (microphone / camera, presenting, being shared) on favicons and rows. New §2 row, with the rest of the series. Windows' snapshot has no capture state (only `audible`, ABI 7). |
| `54defc51` | feat: introduce new tab capture badges for enhanced visibility during media capture and screen sharing | decide | Same series. |
| `ee7b0682` | refactor: simplify capture badge logic by removing screen share indicator and consolidating conditions | decide | Same series. |
| `18588cc7` | feat: add ScreenShareMirroredBorderOverlayView for enhanced visual feedback during screen sharing | decide | Same series: the outline around a shared page. |
| `466fdbb7` | feat: implement visual indicators for shared tabs with new sidebar row outline and tooltip enhancements | decide | Same series: the sidebar row outline and tooltip. |
| `14eeab31` | feat: add hover "share this tab instead" button for running tab shares | decide | Same series: hover "Share this tab instead" on tab rows and strip cells. The fork's tab sharing proxy is on phi-r158 (`ee1384d0b39`); Windows needs it exported in `phi_framework_api.h` (ABI) plus the capture flags. L, fork + client. |
| `99282330` | fix: square video corners in content fullscreen | done | With a page margin on, Windows drops the margin in full screen and the fork rounds the page only while it is inset, so the full-screen page is square. |
| `081b7ee4` | fix: keep the window still when dragging pinned tabs (#392) | skip | AppKit movable-by-background. Windows pinned cells hit-test as client. |
| `dacfd786` | fix: honor Reduce Motion for browser sidebars | port | Reduce Motion for sidebar transitions. Windows' floating panel slide ignores the system "Animation effects" switch (only the Library and the card read `SPI_GETCLIENTAREAANIMATION`). Make the panel show and tuck at once when it is off. S, client. |
| `75d5f1c7` | fix: make shared tab switching reliable across tab layouts | decide | Same series as `14eeab31`. |
| `7e047411` | Merge pull request #413 from phibrowser/feat/screen-share-quick-switch | skip | Merge of feat/screen-share-quick-switch. |
| `29179ff9` | fix: keep the floating sidebar in step with a fast pointer at the window's edge | port | Small. A pointer that reached the floating panel and runs on past the window's sidebar edge, level with the panel, keeps it out. Windows tucks it 150 ms after the pointer leaves the panel rect. S, client (the 30 ms tick). The fast-exit reveal half does not fit the Windows 300 ms dwell. |
| `01be27ec` | fix: honor Reduce Motion when reopening the floating sidebar | port | With `dacfd786`. |
| `93b1d0aa` | feat: hold Option over a split's sidebar row to remove a pane from the split without the right-click menu | skip | The Windows split row has one close box; Remove from Split is in the hover chip and the row menu (2026-11-17 verdict on `95860066`). |

### 2026-11-17 — phibrowser-mac `13f47f02` → `b9b16fed` (31 commits, no merges)

Read through every diff and the Windows code (and the fork on `phi/phi-r156` `a7c49b2f5e6` where
a Mac commit touched it). The first pass on the dev branch model: client work on `origin/dev`
`85aec33`, the fork change built locally from `phi/phi-r156`. Three new 🔧 rows (§2: Top Buttons
and Space icons in the header, drag-to-split in the sidebar, folder / Space hover preview), one
new ⛔ (§1: Toggle Balanced Layout), the floating sidebar row back to ◐.

**Ported this pass (small and clear, no decision needed):**

| # | Mac | Windows change | Test | Result (gmk, 100 %) |
|---|---|---|---|---|
| 1 | `8eca5e62` | `phi_sidebar_drag_session.{h,cc}`: the empty pinned band's drop zone is held back at the lift and opened by `RevealPinnedDropZone` once the pointer is over the sidebar header, then kept to the drop | suite-pinzone | 5/5; baseline 4/5 (the rows jumped 108 px at the lift) |
| 2 | `57f08e2a` | `phi_shell_window.{h,cc}`: a WA_INACTIVE to one of the shell's own WS_EX_TOOLWINDOW flyouts while the floating panel is out no longer tucks it; the tick holds it until that flyout closes (`floating_flyout_`) | suite-floatinghold | 5/5; baseline 4/5 (popover up, panel already tucked) |
| 3 | `711f8a61`, `c7f41199` | `phi_shell_window.{h,cc}` `SideButtonSwitchSpace`: WM_XBUTTONDOWN over the docked sidebar or the floating panel steps Space, no wrap; the release is swallowed so no WM_APPCOMMAND follows | suite-sidebuttons (input rig `xclick`) | 6/6; baseline 5/6 (forward did nothing) |
| 4 | `b0b3cd4c` (Settings half) | `phi_settings_window.cc` AddProfileBody and BuildProfileDetail: a profile no Space uses greys its three data links and shows the Mac's note; the rows now carry their title for UIA | suite-profilenospace | 4/4; baseline 3/4 (live links) |
| 5 | `5e8905b4` | `phi_settings_window.{h,cc}`: one in-place EDIT (`BeginInlineRename` / `EndInlineRename` / `LayoutInlineRename`) for a Space row (double-click, Return on the selected row, the bar's rename button), a profile's Name field and the account name; the modal prompt paths are gone, with their five strings | suite-renameinplace | 6/6; baseline 2/6 (no field) |
| 6 | `2fec54f4` | client: prefs `autoPeekSameSiteLinksEnabled` and `peekPanelSize` (local), two Navigation rows, `settings.get/set` keys, `PhiPeekPanel::PaneInsetX` (Medium 0.8 / Small 0.68 of the page, 480 dip floor), open Peeks re-fit (`RepositionPeeks`); fork: `SetAutoPeekSameSiteLinks` (ABI 10) and the same-site exemption in `ShouldAutoPeekLinkChild` | suite-peeksize | 6/6 (width share Large 0.943, Medium 0.801, Small 0.680); baseline 2/6 |
| — | `c79cd76b` | `client/resources/strings.json` regenerated against `b9b16fed`: the eight new keys above come with the Mac's translations; the seven keys whose translations the Mac dropped with `5e8905b4`'s prompts keep theirs as `interim` blocks | test-host strings check | host tests pass |

**Waiting on the user's decision** (product calls, or over a day):

| Mac | What | Size |
|---|---|---|
| `aa3bba6c`, `59e65c80` (+ `33ac26c5`) | drag a tab onto another tab's row to split, onto a split's row to add a pane | M, client |
| `ca07af8f` | hover preview + search of a collapsed folder or the Space root | L, client |
| `bb4082e1` | resize the floating sidebar by its edge (the width is the docked one) | M, client |
| `20876a28` | spring the floating sidebar in (ζ 0.8, ~1.5 % overshoot) | S, client; a motion call |
| `b7bee5f1`, `823dba25` | Top Buttons submenu; Space icons up into an empty header | M, client; Windows header layout |
| `461fd42d` | sync 3–4 pane splits and split orientation for bookmarks and pins | bookmark half M; pin half L (Windows keeps a wider pinned split on ONE row, the Mac uses one pin per pane); sync wire with Mac peers |

Baseline: origin/dev `85aec33` with dev hooks on framework win-v0.6.0.21. Ports 1–5 ran as an
ABI 9 build of the same client (all but the framework call of port 6) on the same framework. All six then
passed together (32/32) on the final client with the ABI 10 framework built locally from
`phi/phi-r156` `a7c49b2f5e6` plus the port 6 patch (`out/PhiFxWinPeek`, every framework file
from that build).

**Box checks:** `b9b16fed` passed as it is: holding a tab drag below the New Tab row opens the
gap after the last tab, the row moves down under it, and the drop lands before it (gmk). Also
checked: `b0b3cd4c` window half, not reproduced. A Space on its own profile, shown, then
deleted, leaves that profile in Chromium's last-active list; the next launch (restore on) loads
it and parks its window as a cold-start ghost (`ColdStartParked profile=Profile 1`) instead of
hosting it, so only the Default Space's window shows (gmk). Not ported: the Mac also drops such
ghosts from the session; on Windows the parked ghost is kept and parked again each launch.

| Commit | Subject | Verdict | Windows note |
|---|---|---|---|
| `7ae1fda6` | debug-only menu item that force-resets an account's sync data | skip | Debug tooling (§1 *DEBUG* row). |
| `2fec54f4` | same-site auto Peek option and Peek panel size | port | Port 6. |
| `5e8905b4` | rename Spaces, profiles and the account name in place in Settings | port | Port 5. |
| `711f8a61` | switch Spaces with mouse back/forward buttons over the sidebar | port | Port 3. The client ignored XBUTTON over the sidebar; nothing there fought it. |
| `b7bee5f1` | hide the sidebar header's buttons, move the Space icons up into an empty header | decide | New §2 row. |
| `57f08e2a` | keep the floating sidebar open while a popover shown from it is up | port | Port 2. Same bug: WM_ACTIVATE/WA_INACTIVE tucked the panel unconditionally. |
| `c7f41199` | swipe-shaped mouse side buttons over the sidebar | skip | macOS swipe event; the Windows buttons are port 3. |
| `823dba25` | harden the Space icons moved up into the header | decide | With `b7bee5f1`. |
| `b0b3cd4c` | close a window opened on a profile no Space uses | port (half) | Port 4 (Settings note). The window half did not reproduce on Windows (box check above). |
| `aa3bba6c` | create split views by dragging tabs onto each other | decide | New §2 row. |
| `fad22ac0` | fixed-size account Space icons in the Space matching picker | watch | The Windows pairing wizard has no Spaces step (§5 sync keys row). |
| `ca07af8f` | preview and search a collapsed folder or Space on hover | decide | New §2 row. |
| `8eca5e62` | sidebar rows stay put when a drag starts with no pins | port | Port 1. Same bug (`ReserveDropZones` at the lift). |
| `59e65c80` | add a tab to a split by dragging it onto the split's row | decide | With `aa3bba6c`. |
| `33ac26c5` | grouped split row going blank when the tab above joins | skip | AppKit nested group table; Windows rebuilds its rows whole. A case for the drag-to-split test if that is built. |
| `0fecf9c8` | Control-Shift-L toggles Balanced / Performance | skip | New ⛔ §1 row: no Performance layout; Ctrl+Shift+L is the Library. |
| `559d95e6` | direct touch on macOS 27 | skip | macOS 27 only. |
| `36bce7f1` | hover sidebar and top bar in full screen before macOS 26 | skip | AppKit split-view behaviour. |
| `4f76e90c` | Option-click the floating sidebar's zoom light | skip | No traffic lights on Windows. |
| `0893b8b3` | floating sidebar traffic light spacing | skip | Same. |
| `bb4082e1` | resize the floating sidebar by its edge | decide | Floating sidebar row. |
| `20876a28` | spring the floating sidebar into place | decide | Floating sidebar row. |
| `eb2e599c` | pinned tabs and Bookmarks stay in their Space despite URL rules | done | Fork `155d78da82b` (in `phi-r156`) exempts a tab with a custom value; every pane of a Windows split bookmark is tagged (`TagForPane`), so the Mac's `setPhiBoundTab:` mark has nothing to do here. The "separate into a background tab bypassing routing" half waits on Separate From Bookmark (§2 favicon actions row). |
| `95860066`, `44ffb0c1` | four-pane split row close buttons at the minimum width | skip | The Windows merged row has one close box; per-pane close lives in the hover chip, a popover with its own size. |
| `c79cd76b` | translations for the dev strings up to `44ffb0c1` | port | Catalog regenerated, see above. |
| `4a95c43e` | native NTP in a new incognito window's first tab | done | The Windows NTP is URL-driven (`PhiNativePagesHost::WantsNtp`), and fork `2123f1dc30a` stops the "You've gone Incognito" page. |
| `95a537c0` | URL rule match opens in the window it was clicked in | done | Client `7348b4a` + fork `b7b1ce681f5`, framework win-v0.6.0.21. |
| `ffe02f70` | restore a Peek opened over a split pane over its split | skip | Windows does not restore Peeks (§3 Peek row). |
| `461fd42d` | sync 3–4 pane splits and orientation for bookmarks and pins | decide | §3 splits row. |
| `b9b16fed` | sidebar drop line stays on the tab side of the New Tab row | done | Windows draws a gap, not a line; the gap opens before the New Tab row and the row follows it down (box check above). |

### 2026-11-14 — phibrowser-mac `80bd9bdb` → `13f47f02` (64 commits, 3 merges)

Read through the diffs and the Windows code (and the fork on `phi/phi-r156` where a Mac
commit touched it). Nothing built or run when this table was written. Nine commits are
release-branch copies (Telegram bots, stale strings) and share a row. The Windows strings
catalog regenerated from the `13f47f02` Mac catalog is byte-identical to the committed one,
so the stale-string removals cost Windows no translation. Three new 🔧 rows (§2: sidebar on
the right, group tabs opened from links, new tabs at the top).

**Port recommendations, smallest first.** S under half a day, M about a day, L several days.

1. `00e3ad2d` omnibox × on hover only. S, client.
2. `ca764c6e` default accent at the palette centre. S, client.
3. `843817e5` (nearby gap) Edit URL on a single pin or bookmark loads the new address in its open bound tab. S, client.
4. `9f3756f2` Bookmarks page shows every pane of a split of three or four. S–M, client.
5. `eb53f280` Library create card follows the form's theme (S), then gradient card grounds (M). Client.
6. `91740002` pinned tab icon. S–M, client.
7. `0c35bdfd` focus stays in a split when its focused pane closes. S, fork, no ABI; box check first.
8. `7f937b6b` family: group tabs opened from links. M, fork + ABI 8 (with 9).
9. `5590a1d1` new tabs at the top. M–L, fork + ABI 8 (with 8).
10. `574c20c4` sidebar on the right. L, client; design (caption buttons in the sidebar header) to confirm.

Box checks first, ported only if Windows fails them: `42668491`, `1ddf16c7`, `1802b94c`, `a4863fc0`.

**Port status (2026-11-15).** The user approved ports 1–7; 8–10 wait. All seven are committed
(client branch `feat/mac-parity-13f47f02`; port 7 on fork `phi/phi-r156`, needs a framework
release). Each suite fails on the baseline (client `416af7c2`, origin/main
`1b4bf96`, dev-hooks) and passes on the port. Tests in phibrowser-win-test branch
`test/mac-parity-13f47f02`.

| # | Mac | Windows change | Test | Result |
|---|---|---|---|---|
| 1 | `00e3ad2d` | `phi_omnibox_session.cc` RowDeleteRect: × only on the hovered row | suite-omniboxdelete, cu omnibox-delete-hover | gmk 3/3 (baseline fails), scenario PASS |
| 2 | `ca764c6e` | `phi_theme_palette.cc` Accent: zero saturation → #3AA4D5 / #1E7099 | suite-paletteaccent, cu palette-centre-accent | gmk 4/4 (baseline keeps the old hue), scenario PASS |
| 3 | `843817e5` | `phi_coordinator.cc` SetStoredRowUrl navigates the bound tab in place (not when it is already there) | suite-pinediturl, cu pin-edit-url-loads | gmk 5/5 (baseline 3/5), scenario PASS 3/3 |
| 4 | `9f3756f2` | `phi_bookmark_manager.{h,cc}`: extra panes in icons, address column, search, Open | suite-bookmarksplit4, cu bookmarks-page-split4 | nuc 7/7 (baseline 5/7), scenario PASS 3/3 on gmk |
| 5 | `eb53f280` | `phi_library.cc` RegradeCard (gradient cards), FormThemeChanged / FormCorners host callbacks in `phi_create_space_panel.{h,cc}` | cu library-card-theme (no pipe access to the Library) | nuc scenario PASS 3/3, baseline FAIL C1/C2 |
| 6 | `91740002` | menu rows, `LocalStore::UpdatePinIcon`, `PhiTabStore` pin icon, cell paint, band signature, `pinned.list` `icon`, two strings | suite-pinicon, cu pinned-tab-icon | nuc 5/5 (baseline fails), scenario PASS 5/5 on gmk |
| 7 | `0c35bdfd` | fork `tab_strip_model.cc` RemoveTabFromIndexImpl, `IS_PHI_ABI_CLIENT`: a surviving split focuses the next pane, else the previous (the fork chose the first pane) | suite-splitfocus, cu split-focus-close | nuc 5/5 twice with chrome.dll `526d99ea` (baseline 3/5), scenario PASS 2/2 on gmk |
| 8 | `7f937b6b`, `4964a1e3`, `33199022`, `2f4cf3b6` | fork: `ScopedUserOpenedNewTab` on Windows (`browser_web_contents_delegate.cc`, stub in `phi_bridge_wrapper_stub.cc`), `OnUserOpenedLinkTab` hook + posted delegate call with the opener's search terms (`phi_framework_impl.cc`, `phi_framework_hooks.*`); client `PhiShellWindow::OnUserOpenedLinkTab`, Advanced row, pref, `auto_tab_groups_enabled` | suite-linkgroups, cu link-tab-groups (written, not run) | gmk 12/12 (baseline 2/12) |
| 9 | `5590a1d1` | fork `PlaceNewTabBefore` over `TabsProxy::PlaceNewTabBefore`; client `OmniboxPlaceOrdinaryNewTab` after each ordinary `NewTab`, New Tab row placement in `phi_shell_layout.cc` / `phi_sidebar_view.cc` (in the flow, sticky edge, hairline, row clip), General row, synced `newTabsAtTop`, `window.layout` `newTabRow` / `newTabRowSeparator` | suite-newtabtop, cu new-tabs-at-top (written, not run) | gmk 10/10 (baseline 2/10) |

**Ports 8 and 9 (2026-11-15, approved by the user: "Mac ports others except the right sidebar
feature"; port 10 not approved).** One framework ABI bump for both, 7 → 8 (`OnUserOpenedLinkTab`
appended to the delegate, `PlaceNewTabBefore` to `PhiFramework`). Fork change built out of tree
from `phi/phi-r156` `a509931ee6e` (patch, nothing pushed); the box run used that framework with
the uncommitted client (dev hooks). Baseline: client `7880a8c` on the mute-tab ABI 7 framework.
nuc's Smart App Control refused the freshly linked `phi_client.dll`, so both ran on gmk. Tests on
phibrowser-win-test branch `test/mac-parity-8-9`.

Final check: suites 1, 2, 3, 4 and 6 all pass on nuc against the combined client `e0f7862a`
(3/3, 4/4, 5/5, 7/7, 5/5). Per-port diffs: `wt/mac-parity-patches/`.

| Commit | Subject | Verdict | Windows note |
|---|---|---|---|
| `f181bd16`, `c24a080f`, `764be18c`, `74f9dee0`, `7195439e`, `1b44e8aa` | custom Telegram bots (release/2.16.1) | skip | Phi Link, ⛔ AI suite. |
| `10c3dfb4`, `755a2758`, `6245d153`, `aa39fd90` | i18n: stale strings, catalog order, Toolbar Buttons | skip | Catalog regenerated against `13f47f02`: no change. The Toolbar Buttons keys are not asked for (see `c592b83d`). |
| `42668491` | fix: bring Phi to the front when switching to the agent Space from the handoff prompt | box check | The prompt activates Phi when clicked and `Switch()` hands activation to its owner shell, but `RequestWatch` never raises the hosting window. If the box shows it behind another app: `BringToFront()` on the watcher after `HostAgentSpace`. XS. |
| `98e9b12b` | Report edge-to-edge content preference in launch snapshot | skip | Edge-to-edge is ⛔ (§3). |
| `eb53f280` | fix: Library new Space card follows palette edits and shows gradients | port | `phi_library.cc` computes `creation_ground_` once from the source Space; the form tells its host nothing about theme changes. Add a `FormThemeChanged` host callback; then paint card grounds from `ground_stops` with `PhiOklabGradient`. |
| `c592b83d` | feat: let users choose which toolbar buttons are shown in Comfortable | watch | Windows' horizontal address row has no Profile, Downloads, Memory or Feedback button (2026-09-20 decision). Revisit if one is added. |
| `62fd9b82` | fix: remove the gray line around the page when edge-to-edge is on | skip | Edge-to-edge is ⛔. |
| `122b7f69`, `deb16820`, `005548b1` | Pi Agent Transcript mirror and companion extension removal | skip | Already removed on Windows 2026-11-12. |
| `0f0732d9` | fix: register supported file types for macOS Open With | skip | The NSIS installer registers Chromium-for-Windows' association set (`phi-installer.nsi`). |
| `9b6824d7`, `3906564a` | Cmd-W on a pane of a stored split keeps the saved panes | skip | Both reverted by `a424c64b`; Windows went through the same two steps (8cc8172, e74483e). |
| `7100f07a` | fix: host the omnibox in the browser window above Peek and Reader | skip | The suggestion list is an owned popup with a DWM shadow, above the child Peek panel; nothing clips it. |
| `29b57b4b` | fix: keep the header row uncovered, clickable and undimmed while a Peek is open | skip | The Peek and its dim sit in the content rect only (`phi_peek_panel.cc`). |
| `c1c471c3`, `30c855ab` | keep the panes of an opening or closing bookmark split out of the tab list | skip | `OpenCell` opens, tags and splits in one call before the posted rebuild; a tagged pane outside a split is held by its cell, not listed. |
| `1ddf16c7` | feat: show the sidebar context menu when right-clicking the bottom bar | box check | Code says Windows already does it (each click hit-tests its own point, the bar falls through to `ShowTabAreaMenu`, the floating panel forwards to the same handler). |
| `ca764c6e` | fix: use default accent when theme palette returns to center | port | Same bug: the centre snap keeps the hue and `PhiThemePalette::Accent` ignores saturation. Return #3AA4D5 / #1E7099 at saturation 0. |
| `e0b0bd8e` | feat: admit Athena under its new bundle ids | skip | Companion apps ⛔ (§6). |
| `9f3756f2` | fix: display all split bookmark panes in bookmark manager | port | `phi_bookmark_manager.cc` reads `url_b` only; carry `PhiStoredRowView::more` through `Row`, search and paint. The white / black ground is skipped (`window_bg`). |
| `a424c64b` | fix: close the whole split when Cmd-W closes a pane of a pinned or bookmark split | done | Windows e74483e (2026-11-12). The comment in `phi_shell_window.h` that says the Mac closes only the pane is updated. |
| `82c45010` | fix: open an Option-clicked link in a new tab when its split is full | skip | Alt-click in a split loads the link in another pane on Windows (stock Chromium), so no link is dropped. Windows replaces a pane where the Mac adds one: a product difference, not this commit. |
| `1802b94c` | fix: let links in an unfocused split pane respond to clicks | box check | Windows panes are Chromium's MultiContentsView; the client does not focus panes on click. Check a first click on a link in an unfocused pane. |
| `cf42447e` | fix: truncate kiosk URL before the profile button in narrow windows | skip | `phi_kiosk_window.cc` already partitions the capsule; the rects cannot overlap. |
| `4eba5dce` | fix: keep a split whole when its tab group moves to bookmarks | watch | Windows cannot drag a whole group onto bookmarks yet (D33). |
| `db5ed88f` | refactor: inject a tab-group bridge into BrowserState | skip | Test seam. |
| `7f937b6b`, `4964a1e3`, `33199022`, `2f4cf3b6` | group tabs opened from links (setting, search-term names, pinned and Bookmark Tab openers, launch snapshot) | port | New §2 row. Fork callback with the opener and its search terms, ABI 7; client grouping, Advanced row, analytics property. |
| `c804680d`, `880bdf61` | docs: Space routing of link tabs through redirects | skip | Describes fork behaviour Windows shares (`PhiLinkTabMarker`, the Redirector put-back). |
| `6e387f16` | chore: resolve macOS Phi Framework releases by mac-v tags | skip | Mac build script; Windows releases are `win-v*` and never Latest. |
| `c58e44be`, `74bb3a5f` | merges (release/2.17.0, main into dev) | skip | Their content is in the rows here and earlier passes. |
| `574c20c4` | Add an option to put the sidebar on the right | port | New §2 row, L. |
| `5e90e343`, `adb88ae4` | refactor + tests: route tab binding changes through one entry | skip | Windows writes tags in one place (`TagTab`). |
| `60475b75` | fix: keep a Peek attached when its opener's binding changes | skip | Windows does not restore Peeks; the tag never holds Peek markers. |
| `843817e5` | fix: navigate bound tabs in place instead of clearing their binding | port (nearby gap) | Pin return and the split editor already use `NavigateTabInPlace`. Gap: single Edit URL (`SetStoredRowUrl`) saves only, the open tab stays on the old page. |
| `de19a7e4` | fix: pin a tab with a stale binding as a new pinned record | skip | Pin always makes a new row and retags; a stale tag names no record. |
| `9aebe163` | fix: keep a split bookmark active when its focused pane joins the split | skip | `active` is recomputed from each snapshot. |
| `3478c44b` | fix: stop the split AI Chat panel going blank | skip | ⛔ AI suite. |
| `0c35bdfd` | fix: keep focus in a split when one of its panes closes | port | Chromium's `DetermineNewSelectedIndex` runs the opener rules before the split-sibling rule, so a pane that opened a background tab hands focus out of the split. Fork, `IS_PHI_ABI_CLIENT`; box check first. |
| `d0887f1f`, `1a296ec2` | bind a restored split to its bookmark again (relaunch, reopened window) | skip | The tag is in `PhiTabData::custom_value`, saved by session and tab restore. |
| `a4863fc0` | fix: keep an automatic Peek in its Space through redirects | box check | Windows marks the Peek right after `OpenURL`, before redirects; open question is a rule matching the first request. If it fails: `ExemptNextNavigation` before `OpenURL` in `OpenUrlAsPeek` (fork, S). |
| `f4c5316c` | fix: stop a late page title from overwriting a tab's newer title | skip | One coalesced snapshot path on Windows. |
| `8ff5cbb7` | fix: keep the Peek card on whole points | skip | Integer physical pixels on Windows. |
| `91740002` | feat: add custom icons for pinned tabs | port | §2 row stays ◐ until done. |
| `5590a1d1` | Add configurable new-tab placement with split-safe ordering | port | New §2 row. |
| `92ba12c5`, `df77cf4a`, `13f47f02` | unified omnibox: Phi pins, bookmarks, splits and commands as rows | watch | Mac Canary only; §3 omnibox row. |
| `00e3ad2d` | fix: right-align omnibox switch hints and show delete only on hover | port | Hints already right-aligned; `RowDeleteRect` also shows the × on the selected row. |

### 2026-11-11 — phibrowser-mac `28df0d0d` → `80bd9bdb` (16 commits, no merges)

Read through the diffs and the Windows code, then ported on request the same day. Built
(fork + client, framework ABI 3 → 4) and run on nuc at 300 % with a throwaway script over the
control pipe and real input; nothing was added to the box tests. Two new ✅ rows (keep pinned
tabs loaded §2, View ▸ Appearance §1); the bookmark favicon row went 🔧 → ◐.

**The two stored-split items first left open were ported later the same day,** see the
rows `8422f31d` and `6e786436` below and the §3 row.

| Commit | Subject | Verdict | Windows note |
|---|---|---|---|
| `ff8e9238` | fix: keep a hidden agent Space's viewport from following the user's window layout | done | `VerbAgentPanelSize` holds the size on the task record (`PhiAgentSpaces::Space::held_page_*`). nuc: held at 848 with the sidebar hidden. §6 row. |
| `e917b732` | feat: show a slash before the title when an open bookmark navigated away | done | `ShortcutRow::navigated_away`, drawn by `PhiFillSlantedCapsuleAA`. nuc: no slash on the saved page or its `www.` twin, a slash after a move to another URL. No hover hiding: Windows has no favicon return button. §2 row. |
| `2749084b` | fix: keep a closing split bookmark pane out of the sidebar tab list | skip | A Mac ordering fix in `updateNormalTabs`. The Windows rows come from one snapshot per rebuild. Not checked on a box whether a row flashes when a pane of a bound split closes. |
| `153044b1` | improve: split context menu moves the focused pane and says so | done | The row menu passes the focused pane, greyed at each end and when the focus is outside the split. nuc: with A focused and the menu opened on the row, the move turned ABC into BAC. The hover chip's rows follow the same rule and wording since later that day (nuc: opened over pane C, they moved A). |
| `7ac5208c` | fix: keep file and extension URLs intact when saving bookmarks and opening typed addresses | skip | The Windows client has no URL fix-up of its own; the fork's omnibox classifies typed text. |
| `8422f31d` | fix: edit every pane of a three- or four-pane split bookmark or pinned split | done | Windows had no split editor, so it got one for pairs too: `PhiPromptPanes` (a name and an address per pane, numbered for three or four), `SidebarHost::SetStoredSplitPanes`. nuc: the third pane's address edited on a pinned split of three, stored and loaded in the open pane. The pair form and the bookmark form were not run. Addresses are stored as typed, as the single Edit URL prompt does. |
| `6e786436` | feat: save pane reorder, replace and removal into the split's bookmark or pinned tabs | done | `NoteStoredPaneClosing` (Ctrl+W on the focused pane, the chip's Close pane) and `NoteStoredPaneReplaced` (a tab dropped on a pane) rewrite the cell in place and move the tags. nuc: a dropped tab was saved into a bookmark-bound split, and closing the whole split left three panes stored. The close half was changed later the same day on request: Ctrl+W on one pane no longer edits the cell; the panes that stay are unbound and move to the tab list. Reorder is not saved because it is refused (`b36d2c7d`). A close the page holds back with an unload prompt has already been saved. |
| `b36d2c7d` | fix: lock pane move and removal on a split attached to a bookmark, as on a pinned split | done | In the vertical layout a stored split has no row or chip. In the horizontal one a bookmark-bound split has a strip cell: its move, reverse and Remove from Split rows are greyed, and the commands themselves refuse a split whose panes carry a cell's tag. nuc: both rows did nothing on a bookmark-bound split of three. |
| `6952fddf` | feat: add Appearance submenu to the View menu | done | ⋯ ▸ Layout ▸ Appearance. nuc: Dark, Light and Use system setting each land. §1 row. |
| `375fed24`, `734319a9` | feat: keep pinned tabs loaded, with its setting | done | Fork `SetTabKeepLoaded` over the Mac's `PhiKeepLoaded`, client `PhiSidebarView::PushKeepLoaded`, Advanced switch. §2 row. |
| `ae257280`, `436aafc6` | i18n: split editor labels, keep pinned tabs loaded, View menu Appearance, focused pane wording, Bottom Buttons | done | Catalog regenerated; the four move rows changed wording in every language. The split editor keys are not taken. |
| `14b4586c` | fix: keep extensions popup padding even when the grid scrolls | skip | A SwiftUI ScrollView width. The Windows popover draws its own grid and scroll cue. |
| `ce1f357f` | feat: let users choose which sidebar bottom buttons are shown | done | Submenu at the end of the sidebar area menu, vertical layout only. nuc: the Profile row unticks and ticks through the real menu, and the bar closes up. §2 row. |
| `80bd9bdb` | improve: Remove from Split dissolves the whole split, whatever its width | done | Row menu and hover chip both break the split. nuc: three panes became three tabs. `splitView` verbs can still take one pane out. |

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

**Decided 2026-11-11:**

* Time Machine (`81602df9`): not built while the Mac keeps it off (§1, ⛔).
* Kiosk launch omnibox (`ebdbb259`): not ported. It asks for a URL for the global
  Kiosk shortcut and the New Kiosk menu command; Windows has neither (§1). One piece of
  the same commit does apply and is ported: a new Kiosk opens 28 units right of the
  frontmost Kiosk on that display, and back at the left edge once it would pass the
  right one. Windows stacked them exactly. Run on nuc with eight Kiosks.
* About conch (`094e85c8`): left out (§4 About row).

**Results, 2026-11-10.** Ports 1 to 9 and the small fixes are in the tree; where a line
says "nuc" it was run there by real input or over the control pipe.

* 1 sign-out discard: run on nuc 2026-11-11, both the same-run and the relaunch path
  (§3 row).
* 2 splits of up to four panes: done, see the §3 row.
* 3 Tab Switcher across Spaces, 4 Replace Saved URL: done, nuc.
* 5 edge-to-edge: nothing to port, the Windows page has no outer margin (§3, ⛔).
* 6 theme editor presets: three pages of recommended palettes, saved colours with
  Save, Reset and a delete badge, paging that stops at each end. nuc, by screenshots.
  The Mac's slide between pages is not ported.
* 7 membership card: both faces carry the foil, a dark hairline edge, the name pressed
  in with a lower highlight, "Member Since" with the date in Share Tech Mono (the font
  and its OFL licence ship in `resources\fonts`), and a random style always has an
  ornament. Looked at on nuc 2026-11-11 with a stand-in session and a cached join date.
* 8 omnibox: a double-click selects one part of an address (a number keeps its dots).
  nuc. Sideways wheel scrolling is not ported: a single-line EDIT scrolls only by
  moving its caret. The wheel no longer falls through the field to the page behind.
* 9 page colour crossfade: a quarter of a second on a same-tab change. nuc.
* 10 sidebar gradient through a Space switch: already there (`LerpPalette` blends the
  gradient stops; a flat side repeats its colour).
* Small fixes: `01dafe08` nuc; `1e890904` and `fd7357d1` by reading only.
* `a910dca8`: the Kiosk opened centred on the primary display whatever display the user
  was on. It now opens on the display of the Phi window under the pointer, else the one
  with the keyboard. On nuc's one display it still opens centred (2026-11-11); the
  choice between displays is not run, neither box has a second one.
* `d2497fae`: nothing to port. Checked on nuc 2026-11-11: with the floating sidebar up,
  the address field's suggestions and Tab Search both draw above the panel.
* `0ad6c4ae`: nothing to port. Checked on nuc 2026-11-11: a top-level folder dragged
  beside a collapsed sibling opens the gap and the drop lands. Seen on the way and fixed
  the same day: pressing a folder row to drag it also toggled the folder. The toggle now
  comes on the release of a plain click, and an open folder's children fold away while
  it is dragged and come back where it lands. Run on nuc.

| Mac commit | Subject | Verdict | Windows action / reason |
|---|---|---|---|
| `e24c450a` | feat: open Phi Chat's Private AI settings from the AI settings row | skip | ⛔ AI suite. |
| `422c08c0` | fix: crossfade the sidebar's navigation gradient through a Space switch instead of dropping it | done | `LerpPalette` (`phi_spaces_strip.cc`) already blends the gradient stops through a Space switch. |
| `860d24e7` | fix: show the arrow cursor over Next steps consent text and a hand over its links | done | Copy of `da5da806`, port 9 of 2026-11-04. |
| `84bfeaf3` | fix: let Tab reach OOBE footer buttons and follow the Keyboard navigation setting | done | Copy of `597d76b5`, triaged 2026-11-03. |
| `d2497fae` | fix: draw the omnibox and tab search above the floating sidebar they open from | skip | Checked on nuc 2026-11-11: both already draw above the floating sidebar. |
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
| `0ad6c4ae` | fix: show the drop indicator when dragging a folder between top-level folders | skip | Checked on nuc 2026-11-11: the gap shows and the drop lands. |
| `5142015f`, `4ffbbe39`, `441030bb` | xcstrings order and stale strings | skip | Catalog housekeeping. A removed key the Windows catalog still uses shows up at the next regeneration. |
| `f6a9a59f`, `aabf38a0` | Auth0 credentials to the data protection keychain | skip | macOS keychain. |
| `aff124e6`, `8295a1e8` | docs: bridge header comments | skip | Comments. |
| `62e549a0` | feat: add the setting for PiP on other apps' fullscreen desktops | skip | macOS Spaces and full screen desktops. |
| `f77afd55` | feat: grey out Phi menus while only a PiP is on the desktop | skip | macOS menu bar. |
| `d12d4bbd`, `7bfc1239`, `4c7f766e`, `e123af5e`, `3c405222` | custom Telegram bots | skip | Phi Link, ⛔ AI suite. |
| `cde2a6a1` | feat: add a setting to let the Tab Switcher include all Spaces | port | M. New §2 row. |
| `c8c96bca` | fix: keep all-Spaces Tab Switcher recency across Space switches, limit it to matching Spaces, and support empty Spaces | port | With `cde2a6a1`. |
| `094e85c8` | Add interactive conch logo to About Phi | skip | Decided 2026-11-11: left out, 8.5 MB of frames for an About decoration (§4). |
| `22fbb87d` | fix: stop the system beep when typing with focus outside web content | skip | AppKit responder chain. |
| `81602df9` | feat: hide Time Machine backups and purge existing snapshots on launch | skip | Decided 2026-11-11: the §1 row is ⛔ while the Mac keeps the feature off. |
| `8655ec6b`, `0246e720` | feat: mark the session discard on sign-out, consume it on sign-in | port | M. New §5 row. |
| `b3a379e3` | feat: support splits of up to four tabs, including pinned tabs and bookmarks | port | L. New §3 row. |
| `1e890904` | fix: install the Grok Bot skill link into the common agents folder | port | S. `phi_skill_installer.cc` links Grok Bot to `.grok\skills`; it should be `.agents\skills`. Update the comment and the skill's `install.md`. |
| `776f80d3` | fix: vendor a Bitwarden helper that doesn't embed the builder's home directory | skip | The Mac's vendored binary. Not checked: whether `PhiBitwardenHelper.exe` embeds a build path. |
| `0d8342c4` | feat: replace saved pin and bookmark URLs with current URL | port | S. New §2 row. |
| `ebdbb259` | Kiosk: ask for the URL in a Spotlight-style omnibox, and polish window placement and focus | done | Decided 2026-11-11: the launch omnibox is skipped (no global shortcut, no New Kiosk command on Windows). The shift of a new Kiosk right of the one already there is ported (`phi_kiosk_window.cc`), nuc. |
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
| `1c39d48c` | fix: size the agent viewport from the agent Space's own page area | done | Ported 2026-11-02, box-verified on nuc at 300 % and 150 %, hidden and watched (§6 row). `VerbAgentPanelSize` (`phi_verbs_lifecycle.cc`) answers from `ctx.scope` in CSS pixels, rounded up as Chromium rounds a view; `helpers.mjs` and the skill docs took the Mac's patch, naming the task and dropping the AI Chat panel. The skill selftest passes as far as the old skill's: both stop at `Storage.setCookies`. Its drag step needs a page area about 674 px tall (the fixture's target gets centred), which nuc at 300 % cannot give; it passes at 150 %. **Hand-off, found 2026-11-02 on nuc while closing the test gaps:** watched and handed off, the page laid out at the hidden frame's 1386×849 inside a 1191×750 area, right edge cut off, until the next resize. `ClearDeviceEmulationSize` restores the pre-emulation size whenever the current size equals the emulated one, and this port made them equal. The old skill ended correct and hosting without a skill sizes correctly (1192×750), so `c50918f` exposed it. Fixed in the fork: restore to the container's real size (`IS_PHI_ABI_CLIENT`, Mac unchanged; check whether the Mac hits the same case). The pill's Take control also left the page pinned while the agent stayed alive; fixed in the skill's `ownershipChanged`, a patch the Mac's skill lacks too. |
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
    seam under `IS_PHI_ABI_CLIENT` (Peek row gap).

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
| `fec4739a`, `2ac14793`, `7b0c35ec`, `0f0aa5c3`, `e4b5b572`, `c11e968c`, `715c8b67`, `f7bc5b67`, `916b0303`, `39681178`, `6e399950`, `c8c6ac28` | feat/fix: screenshot annotation editor series (crop, floating tools, scrolling selection, blur, Bezier strokes, PNG, PDF leaks, export, toolbar order) | ported | Capture Full Page ◐ (§1, PHI-2251, 2026-11-27): PNG since `f7bc5b67`, Capture Area… with scrolling selection, the attached annotation editor with blur, Bézier freehand, crop and PNG / JPG / PDF export. |
| `63238441`, `100a2a2f` | fix/test: preserve mini player visibility across navigation | done | Ported with the Mini Player 2026-11-13: a shown source stays while paused until visited, a track change keeps its replacement. Tests: `suite-miniplayer` (Windows). |
| `29cb09ed`, `ffcee6bf` | fix: theme tab colour / flashes while the extension side panel is open | skip | Windows has no page-card separation; `ComputeActivePageColor` ignores the side panel. |
| `896b36d0` | Feat/shortcut engine (#203) | done (Windows deltas) | Ported 2026-10-27 the parts a Windows user sees: Delete-to-unassign + per-row Reset, Chromium page commands (Open File, Print, Find, View Source, DevTools, Console, zoom, Back/Forward/Home, Settings) rebindable, effective chords in the ⋯ menu, displaced default chords swallowed by the hook. Not needed: owner/scope inventory, NSMenu scan, informational conflicts (Windows keeps confirm-and-steal), filters, system-hotkey warnings, `excludedKeys`, Kiosk global hotkey (⛔). |
| `93ca9d0e`, `a34b8627` | fix: Swift 6.3 filtering workarounds | skip | Compiler workarounds. |
| `0a0f483e`, `d9b2d481`, `69035603` | i18n: shortcuts / screenshot / reset strings | done | Imported by key on the 2026-10-27 catalog regen; screenshot-editor keys wait on the Capture row. |
| `3a4469a3` | fix: close auxiliary windows before showing login after logout | done | Ported 2026-10-27 in `PhiCoordinator::RequireLogin`. |
| `4d6a7bf2` | fix: keep pins parked for another pinned-tab scope out of held and Needs attention (#206) | done | Ported 2026-10-27: `OwnedCursor::parked_for_scope`, set in `PinOwnedKind::Land`, excluded from `FinishStatusRound`'s pending-inbound count. |
| `c89c05cb` | fix: localize missing shortcut command titles | done | Ported 2026-10-27: every row, fixed row and group heading of the Shortcuts pane resolves a Mac `settings.shortcuts.*` key. |
| `7e731204` | feat: add customizable capture shortcuts and clarify key labels | done | Capture rows: two `kRebindableShortcuts` rows `captureFullPage` / `captureArea`, Ctrl+Shift+1/2 (PHI-2251). Key labels: Windows chords are VK-based (`PhiFormatChord` prints Ctrl+Shift+1), so the shifted-symbol ambiguity cannot exist. |
| `ce059932` | fix: prevent false close conflicts and disable split tab shortcuts | done | Close half n/a (Windows compares its own rows only). Split half ported 2026-10-27: the fork's Alt+Shift+N → `IDC_NEW_SPLIT_TAB` row is excluded under `IS_PHI_ABI_CLIENT`; split stays a menu / Alt-click action. |
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
| `936d090e` | feat: open the omnibox instead of auto-creating a tab in empty windows under the Omnibox new tab behavior | done | Ported 2026-10-27 (fork + client): pushed `SetNewTabOpensOmnibox`; `Browser::TabStripEmpty` skips the refill tab and shows the placeholder at once under `IS_PHI_ABI_CLIENT`; new user Space windows are seeded tab-less and enter placeholder mode (`EnterPlaceholderModeIfEmpty`), which focuses the address field — the Windows twin of the floating omnibox. |
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
| `4e6de510` | fix: preserve media controls and tab indicators when muted | done / watch | Mini Player half ported 2026-11-13 (element mute, a silenced card keeps its controls); the tab audio indicator half stays with that row (§2 🔧). |
| `c3ffbcb9` | fix: keep Mini Player volume controls open and unmute on click | done | Ported 2026-11-13: the volume row stays open off hover; a click while muted unmutes. |
| `83722665` | fix: preserve Mini Player visibility and theme across Space switches | done | Ported 2026-11-13: one card per shell across its Spaces, painted in the presented Space's palette. |
| `55d1519a` | fix: reduce Mini Player flashes during Space auto-PiP transitions | done | Not ported with the Mini Player (2026-11-13). Ported 2026-11-27 (PHI-2252): `PhiMiniPlayer` holds a playing, PiP-capable video that leaves with its Space off the card for 250 ms (`pip-extras.md`). |
| `9ee39fa4` | fix: show webpage volume in the Mini Player slider | done | Ported 2026-11-13: the slider reads and sets the element volume. |
| `d00df7b2` | fix: keep muted Mini Player cards available after switching sources | done | Ported 2026-11-13 (`silent_retained_`). |
| `f6e13a27` | fix: prevent Mini Player slider drags from switching sources | done | Ported 2026-11-13: only the card's plain area starts a source swipe. |
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

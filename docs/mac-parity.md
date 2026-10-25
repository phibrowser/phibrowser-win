# Mac parity: what the Windows client has, lacks and leaves out

This is the weekly feature-parity ledger between the macOS client
(`phibrowser-mac`) and this Windows client. It lists every user-facing Mac
feature once, says whether Windows has it, and for every gap records
either the decision to leave it out or that it still needs to be built.
It is a list of surfaces, not of code: how a feature is implemented is in
the owning doc each row points to.

**Checked against phibrowser-mac `68638148` (2026-10-15) and
phibrowser-win `4f538b2` (2026-10-16), on 2026-10-19.**

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
| Capture Full Page (JPEG to clipboard) | File menu, `docs/page-screenshots.md` | none | 🔧 | CDP `Page.captureScreenshot` is available through the framework; needs a menu row and the clip/budget logic. |
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
| Open external links in Kiosk | Navigation setting | row not drawn | ⛔ | No external-link entry point on Windows yet (`phi_settings_window.cc`). Revisit when Phi registers as a default-browser handler for links from other apps. |

## 2. Sidebar, tabs, Spaces

| Feature | Mac | Windows | Status | Gap / decision |
|---|---|---|---|---|
| Docked sidebar: Space pips, pinned grid, bookmarks, tab tree, bottom bar | yes | yes, 200–480 dip | ✅ | `sidebar-architecture.md`. |
| Floating sidebar (edge hover) | yes | yes | ✅ | No fade on slide-in (impl doc §4.4). |
| Sidebar bottom bar: profile, chat, memory, downloads, feedback | all five | Downloads + Feedback pill | ◐ | Chat and memory are ⛔ AI suite; the profile button's menu moved to Settings ▸ You and the app menu (`windows-native-ui.md` §4.5). |
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
| Space switch slide animation, Ctrl+n, hover card | yes | yes | ✅ | Windows hover card also shows agent status. |
| Incognito Space | ⌃⇧N | Ctrl+Alt+N | ✅ | `incognito-space-port.md`; not ported: account-menu entry, scripting command, pre-warmed tree, new-pip chip flight. |
| URL rules (Domain / contains / suffix / URL → Space / Incognito / Kiosk / Ask) | yes | yes | ✅ | |
| Pinned tab scope follows the synced setting (per-account follower, 2026-10-14 Mac `05160468`) | yes | Windows syncs `PhiPinnedTabScope` | ◐ | Verify the generation fence / blocked-round silence semantics landed in `phi_sync_settings_win.cc`; new Mac behaviour this cycle. |
| Library (overlay / window: Space cards, pins, bookmarks, Downloads, Folio) | View ▸ Library | none | 🔧 | Decided 2026-10-19: implement as a Windows surface (app menu row + Ctrl chord), hosting the Spaces cards, Downloads and Folio sections. Re-adds `library_section_viewed` and `space_created` with `surface: library`. |
| Group Overview page | yes | yes | ✅ | |

## 3. Address bar and page

| Feature | Mac | Windows | Status | Gap / decision |
|---|---|---|---|---|
| Omnibox with suggestions, keyword chip ("Tab to search") | yes | yes | ✅ | |
| "Hold ⌘ to switch" to an already-open tab | yes | none | 🔧 | Chromium's switch-to-tab suggestion is not surfaced in the Windows popup. |
| Site menu: security row, Content blocking on this site, Always show URL path, Site settings, Clear cache / cookie, Extensions ▸, Share ▸ | yes | all but Share ▸ and Extensions ▸ | ◐ | Share: see §1. Extensions live in the field's puzzle button instead (design). |
| Site menu: Collect Site Memories / Delete Site Memories | yes | none | ⛔ | AI suite (site memory is browser memory). |
| Copy link in the field, Reader button (hover) | yes | yes | ✅ | |
| Reader View (typography prefs) | yes | yes + read-aloud HUD | ✅ | Windows has read-aloud; Mac does not. `-phi-auto-reader` automation entry point not ported (analytics doc). |
| Peek (Shift-click, auto-peek from pins/bookmarks) | chips: Close, Open as Tab, Open as Split, Reader View | chips: Close, Open as Tab, Open as Split | ◐ | No Reader chip on the Peek card. Peek is omitted from session restore (recorded in `analytics.md`). |
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
| Sync: status, set up, devices approve/deny, remove device | yes | yes | ✅ | `user-data-parity.md`. |
| Shortcuts: grouped table, search, record, restore, conflicts, Go to Space rows | yes | yes | ✅ | Kiosk & Peek group (⌘O / ⇧⌘O inside those windows) has no Windows rows. |
| Advanced: restore session, full URL, previews, indicators, shorten links, Mini Player, auto PiP, developer mode, Chromium settings | yes | all but Shorten links (⛔) and Mini Player (🔧) | ◐ | Rows regrouped into Appearance / General on Windows. |
| Developer: Allow agents (CDP), per-agent Allowed / Blocked lists, "Allow all agents" | yes | one switch | ⛔ | No code-signature peer identity on the Windows pipe (`phi_settings_window.cc`, `phi_control_server.cc`). Revisit if the pipe gains a caller identity. |
| Developer: Allow agents to operate Spaces, operated-page rendering, profiles agents can use, install skill | yes | yes (no "exact rendering" toggle) | ◐ | The operated-page mask toggle has no Windows row. |
| Developer: Agent Password Manager (Bitwarden sign in / unlock / timeout), Agent Credential Approvals (standing grants, revoke) | yes, `docs/bitwarden-password-manager.md` | helper process + `credentials.*` verbs + Allow once / 10 min prompt; no Settings rows | ◐ | `PhiBitwardenHelper.exe` and the broker are in; the sign-in sheet, session settings, approvals list and "Always" grants are not. `analytics.md` records `approval_type: always` and `kind: run` as not ported. |
| About page | About window | About page + notice | ✅ | |

## 5. Onboarding, account, data

| Feature | Mac | Windows | Status | Gap / decision |
|---|---|---|---|---|
| Onboarding: welcome, name, sign in / guest, consent, layout, next steps | yes | yes | ✅ | `login-port.md`. |
| Onboarding: browser import step | Chrome / Safari / Arc / Dia / file | "Import browser data" link under Next steps | ◐ | A dedicated step with source picker is missing; see §1 import row. |
| Onboarding: password manager choice (iCloud Passwords) | yes | skipped | — | iCloud Passwords is Mac-only (`phi_onboarding_window.cc`). A Windows equivalent (Windows Hello / Credential Manager) would be a new decision. |
| Guest mode + guest → account migration | yes | built, not live-tested | ◐ | `user-data-parity.md` S6. |
| Hosted login (Auth0), reauthentication, sign out | yes | yes, off-the-record sign-in window | ✅ | |
| Sync keys: recovery code, device approval, profile/Space matching | yes | yes | ✅ | |
| Crash feedback ("Phi quit unexpectedly") on next launch | `docs/crash-feedback.md` | Crashpad → Sentry, no prompt | 🔧 | Needs the last-run status from Crashpad and the existing Send feedback form with the crash id. |
| Send feedback: files, pasted images, diagnostics, outbox | 10 × 20 MiB, Sentinel logs | 5 × 10 MB, week-long outbox | ✅ | Sentinel logs are ⛔ AI suite. |
| Account deletion local data removal | yes | none | 🔧 | With the Delete account row above. |

## 6. Agents and automation

| Feature | Mac | Windows | Status | Gap / decision |
|---|---|---|---|---|
| Agent Spaces: overlay, fog, ring, pill (Take control / Hand back / Finish), "The agent needs you", Autoview | yes | yes | ✅ | Agent cues D26–D30 deferred: brand accent per agent, pulsing ring, window title, ⋯ on the capsule, omnibox dim/eye. |
| Agent Transcript console | View menu, pip "Show Transcript" | built, `kPhiTranscriptEnabled = false` | ⛔ | Decided off 2026-09-09 (`phi_features.h`). |
| Agent approval (Allow Once / Always / Deny per agent) | yes | one admission switch | ⛔ | See Developer row in §4. |
| Shadow windows, cursor / effects, captureWindow, reader verbs, user messages | yes | yes | ✅ | |
| User-Space verbs: spaces, profiles, bookmarks, pins, groups, split, URL rules, downloads | yes | yes, plus window / menu / layout / theme / settings / feedback / account / onboarding / sync families | ✅ | Windows is a superset. |
| `agentSpace.app.info` (which Phi build is answering) | `docs/agent-app-info.md` | `capabilities` + `ping`, no `app.info` | 🔧 | One verb; the skill should see the same reply shape on both platforms. |
| Notification cards (`notification.show`, Run, paging) | yes, with Mute / Pop up menu | yes | ◐ | The Mute / Pop up presentation menu is deliberately absent (`analytics.md`). |
| Credentials: `credentials.status/get/getTotp/autofill` | Bitwarden | Bitwarden helper | ✅ | UI gaps in §4. |
| Organize tabs with AI (⌃⇧G) | yes | none | ⛔ | AI suite. |
| Sidecar Travel Back, Live Mode setting (Mac `a6f31c0a`, 2026-10-15) | yes | none | ⛔ | AI suite. |
| Service broker for bundled extensions | yes | none | ⛔ | AI suite (Sentinel). |
| Sentinel helper, watchdog, Phi Chat hotkey | yes | none | ⛔ | AI suite. |
| Folio: Save to Folio, Open Folio, auto-save on site actions, Add Note | yes | yes | ✅ | `folio-port.md`. |
| Native Folio library (All / Articles / Videos / Highlights, reading mode) | `docs/folio-native-library.md` | Mirage `library.html` | 🔧 | Decided 2026-10-19: implement the native library (it is also the Library's Folio section above); Open Folio then stops opening `library.html`. Video articles (`videoGist`) come with it. |

## 7. Summary (2026-10-19; import row ✅ 2026-10-23)

| Status | Rows |
|---|---|
| ✅ ported | 50 |
| ◐ partial | 18 |
| 🔧 to implement | 25 |
| ⛔ deliberately left | 19 |
| ❓ needs a decision | 0 |
| — not applicable | 4 |

**The 🔧 list, roughly by user impact:** Mini Player / media controls ·
content blocking switches · Library surface · native Folio library ·
multi-selection · Bitwarden Settings rows and standing grants ·
Time Machine-style snapshots + restore · crash feedback prompt · image
preview · Arc/Zen migration wizard · Share · Capture Full Page ·
Export/Import user data · account export + deletion · invitation code
(backend) · "Hold to switch" omnibox row · full screen ·
`agentSpace.app.info` · tab audio indicator ·
bookmark Separate/Back-to-saved · What's new / Documentation / Improve
translations rows · Export logs / Extension info rows · settings deeplink.

**Decided 2026-10-19:** the five rows that were ❓ in the first pass
(Library, Time Machine snapshots, native Folio library, Arc/Zen wizard,
Export Logs / Extension Info) are all to be implemented. No ❓ rows remain.

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

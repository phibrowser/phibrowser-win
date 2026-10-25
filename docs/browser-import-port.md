# Import from another browser (Mac port)

The Mac's "Import from Another Browser…" window
(`Sources/UserInterface/Onboarding/Importer`: `ImportFromOtherBrowserViewController`
in its standalone `.normal` mode, `BrowserDataImporter`, `ImportDataType`,
`ArcDataParserTool`) on Windows. It replaces the Chrome-only confirm the
app menu used to show. Ported 2026-10-23.

The separate **Migrate from Another Browser** wizard (Arc / Dia / Zen
profiles and Spaces rebuilt as Phi Spaces) is not part of this port; it is
its own 🔧 row in `mac-parity.md`.

## What the user sees

App menu ▸ "Import from another browser…" opens one window per app
(`client/phi_import_window.{h,cc}`), a `PhiSecondaryWindow` drawn like the
other Windows-native dialogs:

* **Title and target.** "Browser data", and under it the Space the import
  lands in, "Space (Profile)" (the Mac's `formatImportTargetLabel`). Opening
  it again from another Space retargets it unless an import is running (the
  Mac's `rebindTarget`).
* **One row per installed source**, in this order: Chrome, Edge, Brave,
  Arc, then "From a file" (always shown). A browser's row shows when its
  user-data folder exists (the Mac's `hasChromeData`). The row's icon is the
  browser's own, from its App Paths executable; Arc (an MSIX package) and
  the file row use Fluent glyphs.
* **Single-open accordion.** A row expands to its data-type toggles:
  Bookmarks, Browsing history, Cookies, Extensions (the Mac's
  `ImportDataType`; the Mac offers no Passwords toggle and neither does
  this). Under them, "Please close <browser> before importing its data." —
  the Mac's Safari reminder, for every source here: a running Chromium on
  Windows holds its cookie database exclusively.
* **Profile picker.** A Chromium row with more than one profile shows a
  pill listing "Name (account)" from its `Local State` (the Mac's
  `fillChromiumProfileSelector`); changing it clears that row's toggles.
  The Arc row's picker lists Arc's Spaces by title, "Space (profile)".
* **File row.** "Choose File…" opens the system picker for a bookmarks HTML
  file, a Safari History JSON or a Safari export archive; Chromium decides
  what the file holds.
* **Import** (enabled once any row is configured) runs the configured rows
  one by one; the status line names the source being imported. When every
  source succeeded the window closes, as the Mac's does. **Unlike the Mac**,
  a run with failures keeps the window open on "Import completed with
  errors. Failed to import from: …" with a Done button, instead of closing
  over a status nobody sees. Closing the window mid-run does not stop the
  run; a failure then still gets a notice.
* An Incognito Space gets the Mac's "Can't Import Browser Data" alert (the
  menu item is also disabled there).

### Cookies and app-bound encryption

Chrome 127+ seals cookies (and, as found on the test box, Chrome 156's
account passwords) with an **app-bound** key (`v20` values,
`os_crypt.app_bound_encrypted_key` in `Local State`) that only Chrome's own
elevation service can unwrap. **Edge 155 does the same.** Phi cannot read
those values, so a source whose `Local State` carries an app-bound key
offers no Cookies toggle (`PhiImportOfferedTypes`). Sources that still use
the DPAPI-wrapped `v10` key (Brave, Arc, older installs) get the toggle and
their cookies are decrypted by the fork (below).

## How it runs

* **Model** — `client/phi_browser_import.{h,cc}`: the sources, data types,
  the `Local State` profile reader, the Arc `StorableSidebar.json` parser
  (the Mac's `ArcDataParserTool.parse` without the Favorites, theme and icon
  the wizard needs: id-then-object arrays, synced items overridden by the
  live sidebar's, the pinned/unpinned section markers, empty "Folder"
  placeholders dropped, a two-tab split as one split bookmark and any other
  split as its tabs in place, unreadable profile records as "unknown"), and
  the landing into the client store.
* **Run** — `PhiCoordinator::StartBrowserImport` (the Mac's
  `startImportData`): captures `import_types_selected` / `import_started`,
  drops an unreadable file as failed (`file_unreadable`), empties the window
  profile's Chromium bookmark model when anything will stage bookmarks
  there, then `RunNextImport` starts one source at a time through the
  framework and `OnImportCompleted` advances it. A source the framework
  refuses at once reports its completion from inside the start call; the
  run records the awaited source before dispatching and takes that early
  result, so it never stalls. Arc's bookmarks are never sent to Chromium
  (they come from the sidebar); Arc data for a Space whose profile record is
  unreadable is skipped as failed, never read from `Default`.
* **Landing** — `FinishImportRun`: the bookmark bar's children of the
  Chromium tree (the fork's `ProfileWriter` files each source under
  "Imported From <browser>") go to the Space's root, Chromium-sourced; the
  chosen Arc Space goes under "Imported From Arc" ▸ <Space>, Arc-sourced,
  splits included; then the root is reordered — other folders, the imported
  browser folders in row order, the rest (the Mac's
  `reorderImportedBrowserFolders`). A Space deleted mid-run lands nothing
  (`bookmark_persistence_failed`). Then `import_finished`, and the first
  success is a `first_time_action`.

## Framework (fork) changes

* `ImporterType`: `TYPE_PHI_EDGE` (12) and `TYPE_BRAVE` (13), Windows-only.
  Upstream's `TYPE_EDGE` is the legacy EdgeHTML favorites importer and is
  left alone. IPC traits, UMA buckets and the localized folder names
  ("Imported From Edge" reuses upstream's string; "Imported From Brave" and
  "Brave" are new) follow.
* `chrome/common/importer/phi_win_chromium_importer_utils.*` (user-data
  folders), `chrome/browser/importer/phi_win_chromium_importer_utils.*`
  (the import, Dia's shape for any Windows Chromium type),
  `chrome/utility/importer/phi_win_chromium_importer.*` (the utility-side
  importer: `ChromiumImporter` with the folder name per type).
* Arc for Windows: `GetArcUserDataFolder` reads
  `%LOCALAPPDATA%\Packages\TheBrowserCompany.Arc_*\LocalCache\Local\Arc\User Data`
  (the client reads `StorableSidebar.json` beside it). **Unverified against
  a real Arc install** — the box has none; the layout was tested with a
  fixture in that place.
* `chrome/utility/importer/chromium_import_decryptor_win.cc`: the Windows
  `ChromiumImportDecryptor` — the `Local State` key unwrapped with DPAPI
  (`CryptUnprotectData`; the profile-import utility runs unsandboxed), `v10`
  / `v11` values opened with AES-256-GCM, pre-80 unprefixed values with
  DPAPI alone, `v20` refused. `ChromiumImporter`'s password and cookie paths
  and `ChromiumImporterCanImport`'s probes are widened to Windows; the
  cookie store is `Network\Cookies` when present.
* `PhiFramework`: `StartChromeImport` is replaced by
  `StartBrowserImport(window, source, profile, data_types)`,
  `StartFileImport(window, path)` and `RemoveAllBookmarks(window)`;
  `OnImportCompleted` names every source ("chrome", "edge", "brave", "arc",
  "file").

## Harness

`agentSpace.import.open` / `.status` / `.drive` (actions `expand`,
`toggle`, `profile`, `file`, `import`, `cancel`) open, read and press the
window; `.status` also says whether a run is going.

## Verified on the box (2026-10-23, dev-hooks client)

* Detection: Chrome (signed-in profile with its account), Edge, a Brave
  fixture, an Arc fixture (two Spaces, a blank title shown as "Untitled
  Space", sorted by title), the file row.
* Chrome bookmarks + history with Chrome running: 126 bookmarks landed;
  history rows arrived (imported visits in Phi's History). Cookies were not
  offered (app-bound).
* Edge bookmarks + history: 138 bookmarks landed. Edge's cookies are all
  `v20`; none can be imported, which is what led to hiding the toggle.
* Brave fixture (`v10` cookies under a DPAPI-wrapped key): the three test
  cookies landed in Phi's cookie store, re-sealed by Phi, with the lengths
  of the original values — the Windows decryptor works.
* Arc fixture: the Work Space's pins landed under "Imported From Arc" ▸
  Work — the unpinned tab left out, a folder kept, a two-tab split as one
  split bookmark with its second page, a three-tab split as three tabs.
  The split's layout word is not reported by the harness, so the
  orientation mapping is checked by reading the code only.
* File: a bookmarks HTML file landed under "Imported".
* A run with a failing source (Arc data with nothing matching) stays open
  naming Arc; the root order came out "Imported", Chrome, Edge, Brave, Arc.
* Analytics: `import_viewed`, `import_types_selected` (one per source),
  `import_started`, `import_finished` with `failed_sources` and
  `error_code`.

## Not done / differences

* Favicons: the Mac hands imported bookmarks without an icon to its favicon
  backfill; this client does not backfill them yet.
* The Mac's `ImportTargetLock` (refusing to delete a Space an import is
  writing into) is replaced by the landing-time check only.
* The fork's `ProfileWriter` nests each source's bar under its bar folder
  ("Imported From Chrome" ▸ "Bookmarks bar" ▸ …); that is the shared
  framework's behaviour on both platforms and is unchanged here.
* Onboarding is unchanged: its "Next steps" card still points at the menu
  item (the onboarding import row of `mac-parity.md` §5 stays ◐).

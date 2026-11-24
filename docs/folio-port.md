# Folio on Windows (port of the Mac's Save for Later)

Folio is the Mac's "save this page" feature: a save writes the page as a
Markdown article and an MHTML webpage copy, one shared basename, flat in a
folder (`Documents\PhiFolio` by default). Identifiers keep the original
`SaveForLater` / `saveForLater` names; only user-facing strings say Folio.
Mac sources: `Sources/States/SaveForLaterService.swift`,
`Sources/Notifications/MessageCard/ExtensionMessageRouter.swift`,
`docs/folio-capture.md`, `docs/folio-native-library.md` (phibrowser-mac,
read at `208eede5`).

## Who does what

The **Mirage extension owns the save end to end**: extraction, the
Markdown document, the MHTML capture (`chrome.pageCapture`), basename
choice, highlights and the `library.html` / `triggers.html` pages. It ships
in the fork (`chrome/browser/resources/phi_mirage`), so Windows already has
it — the bundled build speaks exactly the Mac's `saveForLater.*` contract.

The **native client** owns the gates, the folder preference, a jailed file
broker the extension writes through, the save RPC, and window chrome
(toasts, the Add Note prompt, Reveal). That is the half this port adds, in
`client/phi_folio.{h,cc}` (pure helpers in `phi_folio_paths.{h,cc}`).

## Message contract (extension ⇄ app)

| type | direction | Windows |
|---|---|---|
| `saveForLater.save` | app → Mirage (broadcast with `windowId`) | sent by Save to Folio; 12 s acceptance budget |
| `saveForLater.saveResult` | Mirage → app | settles the pending save; late = `{"accepted":false}` |
| `saveForLater.toast` | Mirage → app | localized toast; Undo (auto save) / Add Note (highlight) actions |
| `saveForLater.addNote` | app → Mirage | after the Add Note prompt |
| `saveForLater.getArmed` / `armedChanged` | both | `{armed, enabled}`; broadcast once per open profile |
| `saveForLater.trigger` | Mirage → app | site-action auto save; re-gated, URL must still match, 10 min dedupe |
| `saveForLater.fs.list` / `fs.read` | Mirage → app | listing with bounded frontmatter heads; base64 chunk reads |
| `saveForLater.fs.writeText` / `writeBegin` / `writeChunk` / `writeEnd` | Mirage → app | atomic text write; streamed part file, 180 s idle sweep |
| `saveForLater.fs.rename` | Mirage → app | pair rename to the first free basename |
| `saveForLater.delete` / `reveal` | library page → app | permanent delete / Explorer select |
| `saveForLater.videoArticles` / `videoGist` | Mirage → app | unavailable: Phi AI is off on Windows (`kPhiAiEnabled`) |
| `saveForLater.highlight` | old Mirage → app | "Folio needs a browser restart" toast |

Every handler checks the sender is Mirage and the feature is on, and answers
every request (an error reply, never silence). File names go through ONE
rule (`PhiFolioBrokerFileName`: bare `<name>.md|.mhtml`, no separators, no
leading dot) for writing, reading, renaming, revealing and deleting.

Folder resolution follows the Mac: a request names the tab it acts for; the
folder is the one **pinned when the save started** (1 h), else the tab's
profile's, else the active window's profile's. Profile → folder is the
per-profile override, else the global folder.

File I/O runs on one worker thread (FIFO, so a stream's chunks stay in
order); replies are marshalled back to the UI thread through a message-only
window before they reach the framework.

## Windows surfaces

* **⋯ app menu**: *Save to Folio* (Ctrl+Alt+S, the Mac's ⌘⌥S; rebindable as
  `saveToFolio`) and *Open Folio* (Mirage's `library.html` in a new tab of the
  active window). Save is enabled only for an http(s) page with no save in
  flight.
* **Settings ▸ General ▸ Folio**: *Save pages to* (folder picker) and, behind
  the auto-trigger switch, *Save when you like or bookmark on supported
  sites* with *Choose sites…* (`triggers.html`).
* **Settings ▸ Profiles ▸ (profile)**: *Folio Folder* override with *Same as
  General* / clear.
* **Toasts**: the shared top-centre toast gained one optional action button.

## Gates

The Mac gates Folio and its auto-trigger on PostHog flags. The Windows
client has no remote flags, so both are compile-time constants in
`phi_features.h` (`kPhiFolioEnabled`, `kPhiFolioAutoTriggerEnabled`), on.
The user's opt-in for site-action saves stays off by default, so nothing
writes on a like until the user asks for it. There is no remote kill switch
for misfiring site rules on Windows; turning the constant off is a release.

Folio is never offered off the record (the Mac's PHI-2157,
`SaveForLaterService.isAvailable(in:)`): in an incognito window, the
Incognito Space or a peek over either, the ⋯ menu has no Folio rows, the
Save chord and Open Folio do nothing, and a site-action trigger is dropped.
`PhiFolio::AvailableIn` decides, from the framework snapshot's `incognito`
(and the shell's own state); suite `suite-folioincognito` checks it.

## Not ported

* The Mac's **native Folio library window** and the in-window Library
  overlay's Folio section (SwiftUI Markdown reader). Windows opens Mirage's
  `library.html`, which is what the Mac's File ▸ Open Folio does too.
* **Video articles** (`videoGist`): Phi AI is not on Windows; Mirage keeps a
  video as its link and webpage copy.
* PostHog flag plumbing (see Gates).

## Prefs (PhiPreferences.json, the Mac's UserDefaults keys)

`PhiSaveForLaterFolder` (empty = default), `PhiSaveForLaterProfileFolders`
(profile id → path), `PhiSaveForLaterAutoSaveOnSiteActions` (default false).

## Verification (2026-10-15, x64 box, 100 %)

Box-verified with real input (PhiDragCap): Ctrl+Alt+S saves a page (Mirage
wrote `<date> Folio.md` + an 840 KB `.mhtml` through the broker) with the
"Saved to Folio" toast; a second save says "Already in Folio" (listing);
⋯ ▸ Open Folio shows the item in `library.html`, its article and its
webpage copy (chunked reads); Show in Finder opens Explorer with the
Markdown selected; Delete (armed, then confirmed) removes the pair;
"Save Highlight to Folio" (Mirage's page context menu) saves a page around
a highlight, a later highlight toasts "Highlight saved" with **Add Note**,
and the note lands under that highlight; the General ▸ Folio switch
re-arms Mirage (`armed=1` / `armed=0`) and persists; Settings ▸ Profiles
shows the Folio Folder row and its menu.

Not exercised on the box: a site-action auto save and its Undo (needs a
YouTube like or an X/Reddit/Stack Overflow bookmark), a pair renamed while
its webpage copy is still streaming, and a per-profile folder override
taking effect on a save. Host tests: `test/model/folio_paths_test.cc`.

Known, outside this port: Mirage's library labels its reveal button "Show
in Finder" (extension string); the shared toast draws square black corner
pixels on its layered window (pre-existing).

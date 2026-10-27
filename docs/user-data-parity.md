# User data + sync parity with phibrowser-mac — research + plan

Status: IN PROGRESS (2026-09-24, rev 3). D1 decided: C++ port. S0 and S1
done (see §8 Progress); next S2–S5. Plan rev 2 written after pulling
phibrowser-mac `origin/dev` @ `9ff12904` (Phi sync: #145, #151, #153).

Goal: phibrowser-win joins the **same Phi sync account** as the Mac —
same server, same wire entities, same merge rules — and keeps its local
user data in a shape that sync can drive exactly the way the Mac engine
drives SwiftData. phibrowser-win is unreleased: today's Windows stores are
replaced, not migrated.

Supersedes rev 1 of this doc and `login-port.md` §1 decision 3 ("stay
flat").

Mac references: `docs/sync.md` (design, 1.1k lines),
`Sources/Sync/Phi/Proto/README.md` (wire), `docs/sync-e2e-test-cases.md`
(acceptance), `Tests/SyncConvergence/` (hostless merge harness).

---

## 1. What Mac sync is

Two independent channels, both on `https://sync{.stag}.phibrowser.com`,
both authenticated with the Auth0 access token, both end-to-end encrypted:

| Channel | What syncs | Who implements it | Key |
|---|---|---|---|
| **Chromium sync** (`/chromium-sync/<profile_uuid>`, namespace `chromium:<profile_uuid>`) | Per Chromium profile: preferences, search engines, dictionary, themes, extensions (+settings), open tabs (SESSIONS), history (+delete directives), device info. **Not** passwords, autofill or Chromium bookmarks. | The fork (`components/sync/service/phi_sync_*`, portable C++, already in `feat/phi-r152-framework-win`) | Per-profile 32-byte key, delivered as a CUSTOM_PASSPHRASE (64 hex chars) |
| **Phi native sync** (`/chromium-sync/phi/command/`, data type **2000**, namespace `chromium:phi`) | Per account: settings (12 keys), Spaces, bookmarks + folders, pinned tabs, URL rules | The **Mac client** (`Sources/Sync/Phi`, Swift; engine 8.1k lines + 5.2k pure merge code) | Account domain key `phi` |

### 1.1 Keys and devices (`Sources/Sync/Keys`, `/keys/v1`)

- **ARK** (account root key, 256-bit), kept only in memory and wrapped
  server-side three ways: by the recovery key (AES-256-GCM), per device
  (HPKE X25519/HKDF-SHA256/ChaCha20-Poly1305,
  `info="phibrowser-sync-hpke-v1"`), and per join grant.
- **Device key**: X25519, stored in the Keychain as
  `com.phibrowser.sync.device-key` / `device-x25519-v1:<account>`.
  `device_key_id = base64url(SHA256(pub)[0:16])`. This is also the sync
  `client_id` / `cache_guid`.
- **Recovery code**: 16 random bytes → 26 Crockford base32 + 1 check
  character; key = HKDF-SHA256(entropy, salt, `"phibrowser-sync-recovery-v1"`).
- **Profile keys** (for Chromium) and the **domain key** (for Phi
  entities): 32 random bytes, sealed with the ARK as JSON
  `{"v":1,"key":b64,"name":…}`; first writer wins (409 → re-GET and adopt).
- Join flow: join request → another device shows a verification code
  (SHA-256 of the public key → 8 Crockford characters) → approves by
  HPKE-sealing the ARK to the requester.
- Endpoints: `account`, `devices`, `devices/{id}/envelope|revoke`,
  `join-requests[/{id}[/approve|deny]]`, `profiles[/{uuid}]`,
  `domains/{domain}` (full table in the wire research, §7).

### 1.2 Wire (native, type 2000)

`SyncEntity.specifics(21).phi(2000).ciphertext =
0x01‖nonce12‖AES-256-GCM(domainKey, phi.PhiEntity)‖tag16`, no AAD.
`phi.PhiEntity` is a oneof of `setting | space | bookmark | pin_tab |
url_rule`. Every mutable field is a `PhiSettingValue{updated_at_ms, oneof
bool/string/int}`, so each field merges by per-field last-writer-wins
(LWW). Ties break on the serialized value bytes. Timestamps come from a
hybrid logical clock (HLC), and unknown fields must survive round trips.

Client tags are `phi-settings`, `phi-space:<uuid>`, `phi-bookmark:<uuid>`,
`phi-pin:<lineage>:<space|profile uuid|app>` and `phi-urlrule:<uuid>`.
`client_tag_hash = b64(SHA1(0x82 0x7D 0x00 ‖ tag))`; the Mac tests pin
four vectors. Ordering on the wire is a **fractional base-62 `rank`
string**, not the local dense index.

Protocol: pull (GetUpdates, 64 pages, marker + store birthday) must
drain before any commit. Commits go in batches of 25. A CONFLICT gets one
scoped retry after a fresh pull. `NOT_MY_BIRTHDAY` → "Set up sync again".
Invalidation: SSE `GET /sync/invalidations` (`ready` / `invalidate`),
with polling every 60 s while the stream is unhealthy and 300 s while it
is healthy.

### 1.3 What sync needs from local data (Mac schema V13)

The sync engine does **not** care about local id formats: Spaces and
Profiles are mapped to account UUIDs (`sync.spaceGlobalUuids`,
`sync.profileGlobalUuids`); `default-space` is the one fixed identity. What
it needs is the sync-visible fields and the local-write rules:

| Entity | Sync-visible fields | Edit-time columns (feed the HLC stamps) |
|---|---|---|
| Bookmark / folder | `syncId` (lowercase, minted at first publish, persisted by `.claim`), type, parent, index, spaceId/profileId, title, url, secondaryUrl/Title, source, createdDate | `contentUpdatedDate` (only when title/url/secondary/icon actually change), `locationUpdatedDate` (only a user move to another parent or Space — not a reorder, not a landed remote move, not a subtree retag) |
| Pinned tab | `pinLineageId` (wire id = lowercase), owner (Space / Profile / App), index, title, url, **`splitPartnerGuid` (split pin = TWO rows)**, source, createdDate, dormant, scope setting | `contentUpdatedDate` |
| URL rule | `syncId`, spaceId (target), host, pathPrefix, askBeforeRouting, sortOrder | `contentUpdatedDate`, `targetUpdatedDate`; plus `deletedDate` (**soft delete**, purged after tombstone ack or 30 days), `pendingLocalEdit`, `mergePartnerSyncId` |
| Space | SpaceModel fields + from account defaults: `spaceThemeIds`, `spaceOverlayOpacities` {light,dark} (wire: milli, −1 = none); profile via mapping | none on the row — per-field edit stamps live in the Space cursor (`pendingProjection`) |
| Settings | 12 keys (§4.4), each with `.phiSyncTs` / `.phiSyncVal` sidecars | the sidecars |

Not synced: favicons, lastSeen, layout, folder icon **is** content (it
bumps `contentUpdatedDate`) but is not on the wire, `spaceThemeSaturations`,
`spacePureThemeSliderValues`, Kiosk rules, incognito/agent Spaces, the
Phi Chat and agent-fallback profiles.

Sync state on disk (per account, `Phi/users/<sub>/`):
`sync/marker.json`, `sync/{bookmarks,pins,urlrules}-cursors.json`, and
account defaults `sync.phiSpaces`, `sync.profileGlobalUuids`,
`sync.spaceGlobalUuids`, `sync.pairingEnrollment`. The device-wide ones
are `phi.sync.{entityId,version,lastEntity,tombstoneRounds,hasAdopted,
unreadableSettings,hlcMax,wallClockOffsetMs,cursorAccount}` and
`phi.sync.pendingDeviceRegistration.<id>`.

### 1.4 Mac ⇄ framework contract (Chromium channel)

Chromium → client: `getAuth0AccessTokenSyncly`, `getPhiSyncAccountInfo`
(tri-state: `{subject,email}` / `{}` signed out / nil unresolved),
`getPhiProfileSyncInfo:profileId` → `{uuid, passphrase}` (nil until paired
and the key is resolved).
Client → Chromium: `notifyPhiAuthStateChanged`, `notifyPhiSyncKeysChanged`,
`notifyPhiSyncInvalidationForAccount:…`, and `getProfileSyncStatus:` (the
Mac calls it, but **no fork branch implements it yet**).
Launch: `--sync-url=https://sync{.stag}.phibrowser.com/chromium-sync`.

---

## 2. Where Windows stands

| Area | Windows today | Needed |
|---|---|---|
| Account scoping | flat `<UserData>\Phi*.json`, shared by all accounts | `Phi\users\<sub>\…` (sync state is per account) |
| Store | JSON, whole-file writes, no transactions | per-page **transactional landing** of batches → SQLite (§3 D1) |
| Bookmarks | no syncId / edit dates / source / createdDate; implicit root | V13 fields + write rules; explicit root per (profile, Space) |
| Pins | split = ONE row with pane B; lineage never used after create | two rows + `splitPartnerGuid`; lineage is the wire identity |
| Pin scope change | re-owner + merge; narrowing hides rows (bug) | Mac algorithm (inactive backups, lineage merge, dormant) |
| URL rules | no id/sortOrder/dates; replace-all saves | stable `syncId`, 6 sync columns, soft delete, edit-set API |
| Spaces | ids `"1"`,`"2"`; themeId + saturation on the Space | any local id + `default-space` + mapping; `spaceThemeIds` + overlay opacity light/dark |
| Settings | own key names | the 12 synced keys under the Mac names, with sidecars |
| Keys / pairing / recovery UI | none | full port (§5 S3) |
| Framework sync hooks | `phi_bridge_wrapper_stub.cc` returns `""` / nullopt / empty; no notify API; no `--sync-url` | Windows hooks in `phi_framework_api.h` (§5 S2) |
| Crypto / proto / HTTP | client already links BoringSSL, `protobuf_lite`, `crcrypto`; has WinHTTP (`phi_http.cc`) | enough; no new heavy dependencies except SQLite |

---

## 3. Decisions

**D1 — Engine: hand-port to C++ (recommended).** The options were:
- **(a) C++ port** in the client: ~15–18k lines. It uses protobuf-lite
  (which keeps unknown fields), BoringSSL (AES-GCM, HPKE, HKDF, SHA1/256,
  X25519) and WinHTTP.
- **(b) Swift core on Windows behind a C ABI**: exact parity, but it needs
  a Windows-hosted Swift toolchain (we cross-build from Linux), ships the
  Swift runtime + Foundation/ICU (tens of MB) next to chrome.dll, and
  collides with the Win32 loop through `@MainActor`. Foundation's URL
  parsing also differs from Darwin's, so parity isn't free there either.
- **(c) Shared C++ engine in the fork**: the data lives on the client side
  of the bridge, so every access call would cross it, and the Mac would
  have to drop a shipped engine.

(a) is the only option that fits our build (Linux cross-build, one fat
chrome.dll, x64 + arm64). Its risk is drift, so it comes with a
**conformance-vector gate** (D2).

**D2 — Oracle: conformance vectors generated from the Mac Swift code.**
The Mac XCTests are compile-only; the convergence harness is the one
suite that runs. We add a vector emitter to the harness (a Mac-repo
change, harness only) that dumps inputs and outputs of `merge`, `stamp`,
`plan`, rank, normalisation, tag hash and entity bytes as JSON.
Windows gtests replay them. The C++ merge layer is not done until it is
vector-clean; every Mac ruling change regenerates the vectors.

**D3 — Local store: SQLite (vendored amalgamation in `third_party/sqlite`,
not in the kit today; Chromium's copy is `chrome_`-prefixed, so no
clash).** Landing is "one page = one transaction", including the URL-rule
merge tail that runs inside the transaction. JSON whole-file writes
cannot do that safely.

**D4 — Mac quirks.** Port the wire-visible behaviour exactly. Where the
Mac is lax *locally* in a way that only makes stamps coarser, Windows
follows the stricter rule, because the result is still a legal input to
the same merge. Example: Mac UI pin edits don't set
`contentUpdatedDate`; Windows sets it on every real content change.
Anything that changes what goes on the wire goes to the Mac first.

**D5 — Account dir name.** `|` and `:` are illegal in Windows file names,
so sanitise as Sentinel does (`/ : | \ * ? " < >` → `_`). The raw `sub`
stays the identity (and the DPAPI-bound device-key name).

---

## 4. Target Windows layout and model

```
<UserData>\                                   Chromium user-data-dir (unchanged)
  Local State, Default\, Profile N\           global profiles (Chromium sync lives in each)
  PhiPreferences.json                         device-wide keys, Mac names + .phiSyncTs/.phiSyncVal sidecars
                                              + phi.sync.* engine state (≈ UserDefaults.standard)
  PhiAuth.dat, PhiConsent.*, phi_telemetry*   stay
  Phi\
    users\<sanitised sub | default-account-id>\
      localDB\LocalStore.sqlite + LocalStoreCompatibility.json (format 13)
      defaults\account_defaults.json          Mac per-account keys incl. sync.* and Space theme maps
      sync\marker.json, bookmarks-cursors.json, pins-cursors.json, urlrules-cursors.json
      sync\device-key.dat                     DPAPI-sealed X25519 key (Keychain equivalent)
      CustomShortcuts.json, PhiRestore.json, PhiAccountProfile.json, PhiAvatar.img, feedbackOutbox\
```

### 4.1 Tables (mirror V13)

- `profile(profileId PK, displayName, bookmarkRootGuid)`
- `tab(guid PK, type, title, idx, url, favicon PNG, createdDate, updatedDate,
  contentUpdatedDate, locationUpdatedDate, syncId, spaceId, profileId, parentGuid, source,
  secondaryUrl, secondaryTitle, splitPartnerGuid, layout, lastSeen, icon, pinLineageId,
  isPinnedTabDormant, overrideTitle)`
- `space(spaceId PK, profileId, name, colorHex, iconName, sortOrder, createdDate, updatedDate,
  bookmarkRootGuid)`
- `space_url_rule(id PK, syncId UNIQUE, spaceId, host, pathPrefix, askBeforeRouting, sortOrder,
  createdDate, contentUpdatedDate, targetUpdatedDate, deletedDate, pendingLocalEdit,
  mergePartnerSyncId)`
- `browser_data_settings(id PK, pinnedTabScopeRawValue)`

Split **bookmarks** keep one row; Windows' extra pane-B favicon is local-only
(not on the wire). Split **pins** become two rows.

### 4.2 Code ownership (new files)

| File | Role (Mac counterpart) |
|---|---|
| `phi_account_storage` | `Account.userDataStorage`; opens and closes per-account stores on sign-in/out/switch |
| `phi_local_store` (+ `_bookmark`, `_space`, `_pins`, `_url_rules`) | `LocalStore*`, including the sync batch/landing primitives |
| `phi_sync_crypto` | `PhiKeyCrypto`, `RecoveryCode`, `PhiEntityCodec` |
| `phi_sync_keys` | `AccountKeyManager`, `DeviceKeyStore`, `ProfileKeyManager`, `PhiDomainKeyManager`, `KeyEnvelopeAPIClient`, mapping stores, `SyncKeyController` |
| `phi_sync_protocol` | `PhiSyncProtocolClient`, SSE invalidation |
| `phi_sync_merge` (settings/spaces/bookmark/pin/urlrule kinds, owned items, HLC, mirrors) | the 5.2k pure files, vector-tested |
| `phi_sync_engine` | `PhiSyncEngine` rounds, cursors, marker, landing adapters |
| `phi_sync_coordinator` | the sync half of `PhiChromiumCoordinator` (debounce, gates, lifecycle, Chromium hand-off) |
| `phi_sync_*_window` | pairing wizard, recovery code, device list, Settings → Sync pane (Windows-native look) |

`PhiSpaceManager`, `PhiUrlRouter`, `PhiSidebarModel` and the verbs keep
their shape; only their backing store changes. The Chromium tags
`<guid>` / `<guid>#b` stay; pane B of a split pin is the partner row's
guid.

### 4.3 Per-account defaults (`account_defaults.json`)

`loginPhase`, `cachedUserName`, `cachedProfile`, `lastKnownSidebarWidth`,
`activeSpaceId`, `defaultSpaceId`, `spaceThemeIds`,
`spaceOverlayOpacities`, `spaceThemeSaturations`,
`spacePureThemeSliderValues`, `migratedBrowserSources`, `sync.phiSpaces`,
`sync.profileGlobalUuids`, `sync.spaceGlobalUuids`,
`sync.pairingEnrollment`.

### 4.4 Synced settings on Windows

| Mac key (wire) | Windows today | Note |
|---|---|---|
| `openNewTabPageOnCmdT` | `newTabBehavior` | bool |
| `alwaysShowURLPath` | `alwaysShowFullUrl` | |
| `alwaysShowBookmarkBar` | same | |
| `showBookmarkBarOnNewTabPage` | `bookmarkBarOnNewTab` | Mac default true |
| `suppressCloseIncognitoSpaceWarning` | — | add |
| `layoutMode` | same | Windows removed Performance: show `performance` as balanced **without writing it back**, so another Mac's value is not overwritten |
| `autoPictureInPictureMode` | Advanced → Auto picture-in-picture | Off / Normal / Park at edge; queried live by the framework |
| `PhiPinnedTabScope` | `pinScope` | mirror of the store row |
| `PhiDefaultSpaceUuid` | `PhiSpaces.defaultSpaceId` | mirror of the role |
| `PhiUserAppearanceChoice` | `appearance` string | 0/1/2 |
| `PhiCurrentThemeId` | `themeId` | **check that theme ids match the Mac catalogue** |
| `PhiSelectionTintEnabled` | — | add |

Keys Windows doesn't implement still round-trip (merge carries unknown
keys; `apply` skips unregistered ones). Other Windows settings get Mac
names too (rev 1 table) but are not synced.

---

## 5. Phases

| # | Work | Depends | Done when |
|---|---|---|---|
| **S0** | Conformance vectors: emitter in the Mac `Tests/SyncConvergence` harness + a vector format doc; C++ gtest runner skeleton. Confirm with sync-service: platform `"windows"` accepted. | — | vectors generated from `9ff12904` |
| **S1** | Local foundation: `phi_account_storage`, SQLite LocalStore V13 (tables, root rows, `default-space`, two-row split pins, soft-deleted rules, edit-date write rules, PNG favicons), per-account defaults, preference renames; rewire Spaces/sidebar/router/verbs; Mac pin-scope, transfer and cascade semantics. | — | UI runs on the new store; account switch swaps data |
| **S2** | Framework hooks (fork, `IS_WIN_PHI_CLIENT`): `phi_framework_api` setters for the token, account tri-state and per-profile `{uuid, passphrase}`; notify-auth/keys/invalidation; route `phi_bridge_wrapper_stub.cc` to them; client passes `--sync-url` per channel (and the non-component switch mirror). The status API is new on both platforms — coordinate with the Mac owner. | — | Chromium sync runs for a profile given a hand-fed key |
| **S3** | Key layer + pairing: crypto primitives (vector-tested against Mac golden bytes), `/keys/v1` client, DPAPI device key, bootstrap / recovery / join / approve / revoke, profile keys → S2, domain key, pairing wizard + recovery + Settings → Sync pane. | S2 | a Windows device joins a Mac account; Chromium history/tabs/prefs sync both ways |
| **S4** | Native engine, merge layer: HLC, settings, Spaces, bookmark/pin/URL-rule kinds, owned-item planner (cycle break, edit beats delete, adoption, claims, collapse). | S0 | all vectors pass |
| **S5** | Native engine, rounds: protocol client + SSE, marker/cursor stores, pull-before-commit, landing adapters over S1's store, conflicts, birthday/reconfigure, retention, favicon backfill, coordinator lifecycle (sign-out, account switch, self-revoke cleanup). Bring kinds up in Mac order: settings → Spaces → bookmarks → pins → URL rules. | S1, S3, S4 | each kind passes its rows of `sync-e2e-test-cases.md` against a Mac |
| **S6** | Cross-account flows: guest→account migration, backup/restore of `Phi\` (marker travels with it; `hlcMax` does not), local data removal. | S1, S5 | |

S0, S1 and S2 can start in parallel. The largest blocks are S4 + S5 (the
Mac's engine + kinds: ~13k Swift lines plus ~1k of landing inside
LocalStore).

---

## 6. Open questions

1. **D1**: C++ port with Swift-generated vectors (recommended), or Swift
   on Windows?
2. **D2** touches the Mac repo (harness only). OK to add the emitter
   there, or keep it as a separate tool?
3. **Server**: does sync-service accept `platform: "windows"` on
   `devices` / `join-requests`? — YES (2026-09-24): the box is listed as
   `gmk · windows · active` beside the Mac.
4. **Themes**: the Windows theme ids and saturation model vs the Mac's
   `PhiCurrentThemeId` / overlay opacity. Should the two catalogues be
   unified first?
5. **`getProfileSyncStatus`**: the Mac already calls it and no fork has it.
   Who adds it (it is shared C++ + two bridges)? — Windows done
   (2026-09-24): `phi_framework/phi_profile_sync_status.*` builds the
   payload; the Mac bridge can call the same function.
6. **Phi Chat / agent-fallback profiles** on Windows: are there any, and
   are they excluded the same way?
7. **Fork branch**: during this research, `feat/phi-r152-framework-win`
   was amended locally to `8d73925ef7` (the pushed tip is `9a647c9024`),
   by another session. S2 must be based on whichever is current.

---

## 7. Research notes (condensed)

- **Wire:** `Sources/Sync/Phi/Proto/phi_entity.proto`; the sync_pb protos
  trimmed from Chromium 150 (`CommitResponse.EntryResponse` must stay a
  proto2 **group**); command URL
  `POST {base}/chromium-sync/phi/command/?client_id=<deviceKeyId>`,
  `application/octet-stream`; `share=""`, `protocol_version=99`.
- **`/keys/v1`:** JSON, base64std binaries, no response envelope;
  409 semantics per endpoint (`KeyEnvelopeAPIClient.swift:265-469`).
- **Engine map:** `PhiSyncEngine.swift` — pull :1911, page loop :2081,
  publish :4255, kind adapters :5499 / :6390 / :7144; `OwnedItemKind`
  protocol `SyncableOwnedItems.swift:341`; LocalAccess protocols in
  `Phi*LocalAccess.swift`; in-memory fakes of all of them in
  `Tests/.../OwnedItemsTestSupport.swift` (the best host-contract
  reference).
- **Fork:** Chromium sync commits `0b193b9c0c` … `f7958d5aff` are on
  `phi/phi-r152` and `feat/phi-r152-framework-win`; they are portable
  C++, but on Windows the bridge is `phi_bridge_wrapper_stub.cc` (:314,
  :350, :361). The data-type allowlist is in
  `components/sync/service/phi_sync_data_type_policy.h`.

---

## 8. Progress

### S0 — conformance vectors (done 2026-09-24)

- phibrowser-mac `Tests/SyncConvergence` (uncommitted there): builds on
  Linux (Swift 6.4 from swift.org + a pure-Swift `CryptoKit` stand-in in
  `LinuxShims/`, SHA-1 only, plus `UserDefaults.value(forKey:)`), and emits
  vectors with `SYNC_CONV_EMIT_VECTORS=<dir>` (`Vectors.swift`, format in
  `VECTORS.md`). The gate still passes on Linux (222 properties, the two
  registered expected failures reproduce).
- `test/sync-vectors/` — 14 families, 10.4k cases, from `9ff12904`.
  Tag hashes match the Mac's pinned golden values.
- `test/model/sync_vectors_test.cc` replays them. Implemented and green:
  HLC (`client/phi_sync_clock.h`), ranks (`client/phi_sync_rank.*`), URL
  rule normalisation (`client/phi_url_rule_normalize.*`). PENDING (S4): the
  five entity merges, LWW, tag hash.
- Finding: Foundation's `urlPathAllowed` excludes `;` (the Mac stores
  `/a;b` as `/a%3Bb`); the old Windows normaliser and its test assumed
  otherwise, and turned `/` into "any path" where the Mac keeps it
  root-only. Both now follow the Mac.

### S1 — local foundation (done 2026-09-24)

Compile-clean for Windows, host-tested, deployed and smoke-tested on the
box (`test/probes/store-smoke.mjs`, `test/probes/prefs-smoke.mjs`; the
files read back on Linux):

- `client/phi_local_store.*` — the Mac's LocalStore V13 in SQLite
  (vendored amalgamation `third_party/sqlite`, compiled as C by
  build-client.py from the kit's recipe): the five entities with every
  column, `LocalStoreCompatibility.json` (format 13, newer = refused), and
  the Mac's write rules: explicit bookmark roots (+ heal-on-read), dense
  renumbering, content/location stamping, pin-scope migration (inactive
  backups, lineage/variant merge, dormant), pin transfer, split pins as two
  rows, Space cascade (rules soft-deleted), the URL rule edit set
  (9 steps). `test/model/local_store_test.cc`.
- Per account, under `<UserData>\Phi\users\<sub>` (Sentinel's sanitiser:
  `auth0|x` → `auth0_x`; guest = `default-account-id`), like the Mac's
  `users/<sub>/`:
  `localDB\` (the store), `defaults\account_defaults.json`
  (`defaultSpaceId`, `spaceThemeIds`, `loginPhase`,
  `lastKnownSidebarWidth`; Windows-only `winSpaceSaturations`),
  `CustomShortcuts.json` (the Mac's file and place; Windows chords keyed by
  rebind name — chords are per platform and not synced),
  `PhiRestore.json`, `PhiAccountProfile.json` + `PhiAvatar.img` (bound as
  the session names the subject), `feedbackOutbox\`.
- `<UserData>\PhiPreferences.json` (was PhiSettings.json) — the Mac's
  UserDefaults.standard key names and types (§4.4), including the synced
  keys Windows had no UI for (`suppressCloseIncognitoSpaceWarning`,
  `autoPictureInPictureMode`, `PhiSelectionTintEnabled`), the pin-scope
  mirror `PhiPinnedTabScope` and the app-level `phiLoginPhase`. Mac
  defaults adopted: `showBookmarkBarOnNewTabPage` = true,
  `PhiAgentUserSpaceOperationsEnabled` = true (still behind
  `PhiCDPAgentAccessEnabled`). Windows-only keys keep Windows names
  (settingsWindowWidth/Height, saturation, restoreLastSession, testRig,
  skipOnboarding, analyticsEnabled). `phibrowser-win-build/seed-fx-prefs.sh`
  seeds the new names; the installer's data-removal check reads the new
  file.
- Coordinator: the session loads before the stores; the data account
  follows access (`OpenDataAccount` / `SyncDataAccountToAccess`: sign-in,
  Guest, finished onboarding), preferences re-bind their account-scoped
  values, and windows on a missing Space retreat to the default one.
- `PhiTabStore` is a facade over the store (pinned pairs composed into one
  cell; `SwapPanes` returns the new cell name); live pane tags are kept
  canonical (`CanonicalizePinTags`).
- Spaces: UUID ids, `default-space` built-in, model ⇄ store reconcile in
  `PhiSpaceManager::Save`; URL rules keep stable ids (old `g<gen>-<i>`
  still resolves).
- Chromium profile display names mirrored onto ProfileModel
  (`MirrorProfileNames`); `lastSeen` stamped when a pin or bookmark is
  opened/focused from the sidebar (both halves of a pinned pair); Chrome
  imports carry `source = 1`; a cross-Space bookmark MOVE is the Mac's
  `moveBookmarks(toSpace:)` (same row and identity, location edit), a
  clone is a new row.

Notes carried forward:

- Pins do not move between Spaces from the Windows UI (unchanged); the
  store's `TransferPin` is ready for S5 landing and any future UI.
- Unknown keys in PhiPreferences.json are not preserved on save yet; S5's
  settings sidecars (`<key>.phiSyncTs` / `.phiSyncVal`) will need that.
- `test/model/run.sh` stops early (`set -e`) at a stale
  `shell_layout_test` that predates the Windows-native redesign.

### S2 — framework hooks for Chromium sync (done 2026-09-24)

Fork `feat/phi-r152-framework-win` (uncommitted), all Windows-gated or in
Phi-owned files:

- `phi_framework_api.h`: delegate queries `GetPhiSyncAccessToken(sink)`,
  `GetPhiSyncAccount(int* state, subject, email)` (0 unresolved / 1 signed
  in / 2 signed out, the Mac's nil / {subject,email} / {}),
  `GetPhiProfileSyncInfo(profile_id, uuid, passphrase)`; framework nudges
  `NotifyPhiAuthStateChanged`, `NotifyPhiSyncKeysChanged`,
  `NotifyPhiSyncInvalidation` (the Mac bridge's bounds on the hint).
- `phi_framework_hooks` carries the three queries to
  `phi_bridge_wrapper_stub.cc`, which answers the fork's existing
  `PhiSyncAuthProviderImpl` / `PhiSyncKeyProviderImpl` from them under
  `IS_WIN_PHI_CLIENT` (same semantics as `phi_bridge_wrapper.mm`).
- `chrome_main_delegate.cc`: `--sync-url` from the exe's optional
  `PhiClientSyncServerUrl` export unless the launch carries one (the Mac's
  ChromiumLauncher passes the switch). The factory's origin gate still
  decides whether the Phi providers exist at all.

Client:

- `client/phi_chromium_sync.*` — what Chromium is told: account tri-state
  (Guest and no session answer signed out), the bearer (only while signed
  in), per-profile keys in memory (cleared on sign-out); notifies only on
  change.
- `PhiAuth::AccessTokenForSync` — the cached token while it has >60 s
  left, else one background refresh and an empty answer (sync backs off;
  the refresh's auth-changed nudge re-reads). Never blocks the UI thread.
- `PhiAuth::SyncServerUrl` — `PHI_SYNC_URL`, else the tenant's:
  `https://sync.stag.phibrowser.com/chromium-sync` for a `.stag.` Auth0
  domain, `https://sync.phibrowser.com/chromium-sync` otherwise. Token and
  server always belong to the same tenant.
- Coordinator: `SyncChromiumAccount` on framework ready, access changes and
  every auth change. Rig verbs `agentSpace.sync.status`,
  `agentSpace.sync.setProfileKey` (host only); probe
  `test/probes/sync-smoke.mjs` (key steps only signed out or `--live`).

Box-verified: signed in to the staging tenant, status reports the account,
a token and the staging server; a hand-fed key made Chromium start its sync
engine and POST a GetUpdates to staging (aborted when the key was removed
200 ms later — nothing committed; the request likely reached the server
under namespace `PROBE-UUID`). A real sync round needs a real profile key,
which is S3.

Not done here: the Mac's per-profile sync STATUS query (open question —
missing in every fork); invalidations have no source until S5's
notification stream.

### S3 — key layer and pairing (in progress, 2026-09-24)

Crypto: BoringSSL. The framework kit already links `boringssl.lib` into
`Phi.exe`; host tests build the fork's copy with CMake once
(`/tmp/phi-bssl`, `test/model/run.sh`).

Done (compile-clean for Windows, host-tested):

- `client/phi_key_crypto.*` — the Mac's `PhiKeyCrypto` + `RecoveryCode`:
  HPKE base mode DHKEM(X25519, HKDF-SHA256) / HKDF-SHA256 /
  ChaCha20-Poly1305 with info `phibrowser-sync-hpke-v1` and empty AAD
  (`[01][enc:32][ct]`), AES-256-GCM combined (`[01][nonce:12][ct][tag]`),
  recovery HKDF, verification code, `device_key_id`, recovery code
  encode/decode. `test/model/key_crypto_test.cc`: the HPKE core against
  RFC 9180 A.2.1 (seal with the fixed ephemeral key, open), the codes
  against an independent transcription of the Swift.
- `client/phi_sync_keys.*` — host-clean `AccountKeyManager` (bootstrap +
  parked registration, recovery join, approval join with stale-ticket
  withdrawal and cancellation, silent unlock, 409 → rotate + retry once),
  `DeviceApprovalService`, `ProfileKeyManager` (register / adopt / resolve,
  `alreadyMapped` refusal), and `SyncKeyCore` — `SyncKeyController`'s
  mapping pass (unknown locals and a failed listing HOLD; register only when
  every remote is claimed; 1:1 adoption), the enrollment gate
  (`sync.pairingEnrollment`, keys exposed only while paired), the wizard's
  candidate load and decision apply, and Remove this device.
  `test/model/sync_keys_test.cc` runs them device to device against an
  in-memory `/keys/v1` that keeps the server's refusals.
- `client/phi_key_api_win.*` — the WinHTTP `/keys/v1` client (the Mac's
  paths, bodies and status mapping; no token → no request; a 401 renews the
  token and retries once), the device key under DPAPI in
  `Phi\users\<account>\sync\device_key.dat` (rotation replaces in place),
  and the key-layer state in `sync\key_state.json` (the Mac's key names; its
  own file so a retired account's worker can never write into the next
  account's defaults).
- `client/phi_sync_key_service.*` — one account's key layer on its own
  worker thread; results come back through a message-only window and are
  dropped once the service is gone. The coordinator builds it for a
  finished sign-in, retires it on sign-out / Guest / account switch, feeds
  its exposed keys to `PhiChromiumSync`, and re-resolves when the profile
  list changes. Requests carry `PhiAuth::AccessTokenBlockingFor(sub)` — a
  token whose `sub` is not the layer's account is never sent.
- Rig verb `agentSpace.sync.keyOp` (unlock, resolve, beginSetup,
  bootstrap (needs `confirm:"bootstrap"`), requestJoin, pollJoin,
  cancelJoin, recover, startPairing, applyDecisions, refresh, approve,
  deny, removeDevice); `agentSpace.sync.status` reports `keyLayer` and
  `lastOp`.

Box, staging account: silent unlock answered 404 for this device
(`needsJoin`), and `beginSetup` routed to **introduction** — the account
has no key state on staging yet (`GET /keys/v1/account` 404), so no Mac
has set up sync for it there.

UI (compile-clean, deployed, rendered on the box):

- `client/phi_sync_setup_window.*` — "Finish setting up sync": every page
  in the plan below (introduction → bootstrap → recovery code with Copy;
  choose join; waiting for approval with the 3 s poll and the countdown;
  recovery code entry; denied / expired / error; Match your profiles with
  the per-row pickers and the Mac's default matching; applying). Self-owned,
  replies dropped once it is gone; close/Escape refused while an operation
  must finish or the code is shown; "Create on this PC" goes through the
  profile service's new `kSyncPairing` follow-up. Rig verbs
  `agentSpace.sync.setup.open|status|drive`.
- Settings ▸ **Sync** pane (rail entry after the account block): the
  account email; by key-layer state — checking / sign in / couldn't load
  (Retry) / not set up (Set up sync on this device) / not paired (Continue
  setup) / paired; Devices (pending requests with Approve / Deny and the
  verification code, then active devices with "This PC"); Recovery and
  removal with the confirmed Remove this device. Polls every 3 s while the
  pane is up.
- Strings: `sync.setup.*`, `settings.sync.*` in
  `client/resources/strings-source.json` (English; the Mac's wording with
  "PC" for this machine).
- Box capture: `PhiUiCap` task → `phitest\uicap.ps1` (script rewritten
  here) saves the window named in `phitest\uicap.txt` to `uicap.png`.

Next: the live join against a Mac (waits on the user: the staging account
has no keys yet — who goes first).

#### S3 UI plan (the Mac's flows, Windows-native look)

Two surfaces, both opened only on request (the Mac never pops setup on its
own): the **setup window** and **Settings ▸ Sync**. Mac strings verbatim
unless marked; "Mac" becomes "PC"/"device" where it names this machine.

Setup window ("Finish setting up sync", 720×560, one window, the pages
below). Close / Esc = Finish later, refused while an operation runs, while
the recovery code is shown, or while decisions apply.

| Page | Content | Actions |
|---|---|---|
| working | spinner | — |
| introduction | "Set up sync" / "Bring your Spaces, bookmarks, pinned tabs and supported browsing data to your other devices. Save a recovery code, then choose how this PC joins your account." | Continue → bootstrap (already initialised → choose) |
| showingRecoveryCode | "Save your recovery code" / "Store this code somewhere safe. You will need it to add another device to your account." + code | Copy (→ "Copied" 1.2 s), I've saved it → profiles |
| chooseJoinMethod | "Add this device to your account" / "Choose how to set up sync on this device." | Request approval from another device · Enter a recovery code |
| waitingForApproval | "Waiting for approval" / "On an authorized device, open Settings → Sync and approve this request. Make sure this code matches:" + code + "Expires in m:ss"; poll every 3 s | Use a recovery code · Cancel (both withdraw the request) |
| enteringRecoveryCode | "Enter your recovery code" / "Enter the recovery code you saved when you set up sync on another device." + field | Submit (disabled while empty) |
| joinDenied / joinExpired | "Request denied" / "The other device denied this request." · "Request expired" / "This request timed out. You can try again." | Try another way |
| error | "Something went wrong" + message | Try again |
| profiles | "Match your profiles" / body; one row per local (picker: account profiles, "Register as new"; default = same-name remote), then "Unclaimed account profiles" (picker: Choose… / Create on this PC / Assign to X; unreadable → read-only line) | Finish later · Continue (all readable remotes decided) |
| submitting | "Applying your choices…" | — |

A brand-new account (no remote profiles) finishes without showing the
profiles page. Continue applies the decisions directly (the Spaces step and
the overwrite review are S5), then completes enrollment and re-resolves.
Errors: the Mac's KeyLayerStrings / PairingWizardStrings.

Settings ▸ Sync pane (title "Sync"): account email; a "Not paired / Sync
has not started / Continue setup" banner when unlocked but unpaired; by
unlock state — loading spinner, "Sign in to sync across devices" (Sign in),
"Couldn't load sync information…" (Retry), "This device isn't set up for
sync yet." (Set up sync on this device); when unlocked: **Devices** (pending
requests first — "<name> · <platform>", "Expires: <time>", "Verify this
code matches the other device: <CODE>", Approve / Deny — then active
devices, "This PC" on our own row), and **Recovery and removal** ("To join
on another device, approve its request here or use your saved recovery
code. The code is shown only when sync is first set up." + Remove this
device from sync…, confirmed by "Remove this PC from account sync?").
Refreshes every 3 s while shown. The sync STATUS card is S5's (status
card).

**Default-profile rule (2026-10-19, Mac `7a967bd3` / `e13e6dca`):** the
profile envelope carries `"default"` (true for the registering device's
Default — Chromium's `Default` profile here — false otherwise; absent =
legacy, unknown). A Default never adopts a known non-default
(`kDefaultMismatch`, refused before the mapping is written); the mapping
pass gives a free Default the first account default by uuid before any
name match, demotes every other account default to an ordinary profile
(logged once per uuid), and backfills the flag onto this device's legacy
envelopes through `PATCH /keys/v1/profiles/<uuid>` with the expected
ciphertext (409 → re-read, three attempts). The pairing sheet fixes the
Default's row to the first readable account default (shown, not chosen),
filters every picker and "Assign to" by the rule and never offers "Create
on this PC" for the authoritative default. Not live-tested yet.

### S4 — merge layer (done 2026-09-24)

- `client/phi_sync_proto.*` — `phi.PhiEntity` as structs with a hand-written
  protobuf codec (STL only) that reproduces swift-protobuf's bytes: field
  order, proto3 defaults omitted, message presence (an assigned empty value
  is `tag 00`), oneof members always written, map entries in key order with
  key and value, unknown fields kept and written last, repeated message
  fields merged on parse.
- `client/phi_sync_merge.*` — the shared LWW winner (stamp, then serialized
  bytes), `ValueSignature`, and the five kinds' merges with the owned-item
  signatures / location and content stamps, Space refusal (agent Spaces),
  pin identity, client tags and `ClientTagHash` (SHA-1 over `82 7D 00` +
  tag).
- `test/sync-vectors`: all 14 families, 10,461 cases, agree; none pending.

### S5 — native engine (done 2026-09-24)

Done, live against the staging account together with the Mac:

- `client/phi_sync_protocol.*` — GetUpdates / Commit on data type 2000
  (`/chromium-sync/phi/command/?client_id=<device key id>`), EntryResponse
  as a group or a message, the Date header.
- `client/phi_sync_engine.*` — the Mac's PhiSyncEngine rounds: pull before
  commit, 64-page budget with a follow-up round, the marker as each page's
  last write, store birthday / NOT_MY_BIRTHDAY, hybrid clock + wall-clock
  correction, one conflict retry, batches of 25.
  - **Settings**: sidecars, adopt-on-first-pull, the unreadable record and
    its one-replay heal, tombstone heal after 3 rounds.
    `phi_sync_settings_win.*` registers 10 of the Mac's 12 keys; a peer's
    value Windows cannot show (the Mac's `performance` layout, a theme id
    not in the catalogue) is HELD and echoed, not refused.
  - **Spaces**: the gate (open while unlocked and paired — exactly while
    the engine exists), the replay latch (marker nil first), guard 2
    (empty-table replay), unreadable tag hashes, landing with parking
    (unmapped profile), holds (profile binding), rebinds, the default
    Space's constant identity, the rank order projection, tombstones (never
    the last live Space), local deletions, stamping local edits at edit
    time, publishing, 30-day retention.
- `phi_sync_spaces.*` (host-clean: table, snapshot, landing, order) and
  `phi_sync_spaces_win.*` (`PhiSyncSpaceAccess`: the identity mappings on
  the worker — `sync.spaceGlobalUuids` in key_state.json — and the Space
  list on the UI thread through `PhiSpaceManager::Sync*`).
  `<account>\sync\spaces.json` holds the table (`PhiSpaceTableFile`).
- `PhiSpaceManager`: a sync-hidden Space leaves the model (every list) but
  stays stored; Save neither deletes nor reorders it away. An icon Windows
  cannot show is no longer rewritten to the stand-in on save. Remote theme
  ids are kept raw; the Mac's `spaceOverlayOpacities` are carried in the
  account defaults.
- Host tests: `test/model/sync_engine_test.cc` — 14 scenarios over a fake
  server (settings: adopt, both-way edits, conflicts, refusal, tombstone
  heal, unreadable, paging/birthday, clock; Spaces: gate + first publish, a
  fresh device, both-way edits and order, deletion/tombstone/retention,
  parking until the profile exists, unreadable rows).
- **Bookmarks and pins** (the Mac's SyncableOwnedItems + BookmarkKind /
  PinKind + their registrations): `phi_sync_owned.*` (cursor table, the
  generic snapshot / diff / plan with cycle breaking, A9, C4 yields,
  projection domain), `phi_sync_owned_kinds.*` (the two kinds),
  `phi_sync_bookmarks.*` (adoption, claims, landing with folder lifts) and
  `phi_sync_pins.*` (scope parking, variant reminting, split partners). The
  engine's owned path (routing by tag, parked claims, apply, publish with
  the 250-per-round slices, rule 3b yields, retention cascade) is in
  `phi_sync_engine.cc`. Windows side: `phi_sync_owned_win.*` (cursor files
  `<account>\sync\bookmarks-cursors.json` / `pins-cursors.json`, the
  bookmark / pin local access on the UI thread) and two LocalStore batch
  writers, `ApplyBookmarkSyncBatch` / `ApplyPinSyncBatch` (one flush, all or
  nothing). The store signals row edits (`set_on_rows_changed`, favicons
  and last-seen excepted) → a round 2 s later.
- `PhiPinnedTabScope` is registered: landing migrates the store's scope
  (the coordinator's SetPinScope). A key registered after adoption is
  seeded at stamp 0 (the Mac's reseed), so the account's value wins.
- Pin-scope fence (2026-10-19, the Mac's B-3 stage 1, `05160468`):
  `LocalStore::pin_scope_generation` is bumped by every `ChangePinScope`;
  `PinOwnedKind::BeginRound` samples it before the rows and `RescanScopes`
  compares it with the two scope names (a round trip X → Y → X blocks);
  `CommitFenceHolds` re-probes right before each commit batch and drops it
  unsent; `PhiSyncPinAccess::Apply` writes nothing and returns kFailed
  (parked) when the generation or scope moved since the sample. The Mac's
  follower and "not applied yet" status line have no Windows twin: the
  migration and the key write are one UI call here.
- Windows-only: `SpaceSyncTable.replayed_kinds` — a kind the last full
  replay did not route (a newer build added it) arms one replay, marker
  first.
- Host tests: 19 scenarios in `sync_engine_test.cc` (+ bookmarks: tree to a
  fresh device, rename, folder delete in two rounds; adoption claims a twin;
  pins both ways; scope mismatch parks; a new kind replays once).
- Live (box + Mac, staging): the Mac's "Space 2" (Profile 4, W icon, theme
  `pure`) landed on Windows; a rename on Windows reached the server under
  the Mac's uuid and was renamed back. All four Mac bookmarks landed in
  their Spaces (two in Default, two in Space 2), the account's pin scope
  `app` migrated the store, and both pins landed one round later (a scope
  that moves mid-round parks the pins, by design). A bookmark added on
  Windows reached the account and its removal published a tombstone.

- **URL rules** (URLRuleKind + the rule registration + the store's
  applyURLRuleSyncBatchBody): `phi_sync_rules.*` — stamps, merge, refusals,
  signatures, claims of never-published twins (rekey), landing (create /
  update / move / reorder / delete), edit transfer onto a quiescent merge
  partner (§8.4.4 α/β), the merge tail (pointer pass + convergence that
  collapses duplicate rules), pending-edit clearing, hard delete after a
  published tombstone and the 30-day soft-delete purge. The batch body is a
  pure function over the rule rows; the store only commits the resulting
  table (`ReplaceUrlRulesForSync`), and the Space model reloads its rules
  after a landing. Cursor file `<account>\sync\urlrules-cursors.json`.
- `PhiDefaultSpaceUuid` (PhiDefaultSpaceMirror): the register lives beside
  the sidecars (`PhiDefaultSpaceUuid.mirror`), is seeded at stamp 0 from
  the local role holder, published on a user hand-off (deleting the role
  holder), and a landed value moves the local role when the uuid resolves.
  All twelve Mac settings keys are registered now.
- The invalidation stream (PhiSyncInvalidation):
  `<sync base>/sync/invalidations?client_id=<device key id>`, SSE parsed by
  `phi_sync_invalidation.*` (host-tested), read by
  `phi_sync_invalidation_win.*` on its own thread (45 s watchdog, capped
  backoff). `ready` pulls every kind and asks every Chromium profile to
  catch up; a `chromium:phi` hint for type 2000 from another device pulls;
  `chromium:<uuid>` hints go to the framework (NotifyPhiSyncInvalidation).
  While healthy the periodic pull stretches to 300 s.
- Account profiles this PC lacks are created and adopted automatically
  (ensureLocalProfilesForAccount: a same-named unmapped profile first, at
  most three per round), so a Space bound to them lands.
- A first-landing account Space only adopts an already mapped local Space.
  Unmapped same-named Spaces stay separate: a name is not consent to
  overwrite local fields. Windows does not yet offer the Mac's Spaces
  matching and overwrite-review step.
- Self-removal and confirmed reconfiguration stop native writers and keep
  `marker.json` as a cleanup journal. Cursor deletion must succeed before
  clearing bookmark identities; all remaining metadata writes must succeed
  before deleting the journal. A failed step stays paused and can be retried
  through Settings → Sync → Set up sync again, including after restart.
  Reconfiguration preserves device registration; removal rotates the key.
- The default-Space role register is reconciled after every round, so a
  setting received before its Space can take effect once the mapping and
  local row exist, without changing the register's timestamp.
- `PhiStore` keeps top-level keys it does not know when it saves
  (PhiPreferences.json and friends): a newer build's or the Mac's keys
  survive an older build's save.
- Theme ids: both catalogues are the same eight (pure, mint, mist, aqua,
  iris, petal, coral, amber; "default" = pure) — nothing to unify.
- Live: URL rule add/delete round-tripped through the account; the Mac's
  new pin and its deleted bookmark reached Windows; the stream connects and
  reports ready.
- Host tests: 26 engine scenarios (+ rules both ways, rule twin claim,
  duplicate-rule collapse, separate same-name Spaces) and the stream parser.
  Recovery tests inject failures at every cleanup step and cover resuming
  after a server reset, last-device reconfiguration, and deferred default
  role application.

- Favicon backfill (the Mac's PhiFaviconBackfillQueue): after a round that
  landed bookmarks or pins, and once per session, rows with no icon get
  `https://<host>/favicon.ico` (5 s, 64 KB, WIC-decoded to 32 px) through a
  quiet store write that does not start a round.
- A synced delete of a row open in a tab clears that tab's row tag
  (`UnbindTabsOfMissingRows`), as a local removal does.
- The status card (the Mac's SyncStatusState / SyncStatusSummary /
  ChromiumSyncStatus): the engine keeps a phase (checking, initialSync,
  syncing, upToDate, offline, needsAttention) with the last success time,
  set at the start and end of every data round (`finishStatusRound`'s
  pending-in/out rules; a transport failure is offline). Chromium's per-
  profile phase comes from the new framework call `ProfileSyncStatusJson`
  (fork: `phi_framework/phi_profile_sync_status.*`, the Mac bridge's
  getProfileSyncStatus payload — version 1, phase, enabled_categories,
  last_success_ms — derived from TransportState, the actionable error and
  the connection status). `phi_sync_status.h` reduces them (host-tested).
  Settings ▸ Sync shows the summary, "Last successful sync on this PC", and
  one row per context: "Phi data" first, then each LOADED profile (Windows
  loads profiles lazily and the query never loads one; the Mac lists every
  profile). Box: "Up to date" with Phi data and the Default profile.
- Host tests: `sync_engine_test` 27 scenarios (+ status phases incl.
  offline), `sync_status_test`.

- Unanswered is never "gone" (2026-10-13, release review): the Space
  access's presence queries are tri-state (`LocalPresence`: present /
  absent / unreachable) and its Space lists are nullopt when unread. A
  stalled or withdrawn UI hop (`RunOnUiAndWait` withdraws a hop it gave up
  on, so a late write never lands) makes the engine drop, create and
  publish nothing for Spaces; the page's marker stays and the next round
  replays it (outcome `local_read_failed`). A store that cannot open
  (failed, needs a newer Phi — `space_store_unavailable`) is no stall:
  Space arrivals park in `pending_apply`, the owned kinds park, and the
  marker, settings and the other kinds move on. An account switch or the
  guest-migration window counts as a stall.
- Files that are there but unreadable (an antivirus or backup lock) read
  as `kUnreadable`, never "missing": the engine's state, sidecars, marker,
  Space and owned tables, key_state.json and the PhiStore files keep
  defaults in memory and refuse every save until a read succeeds; a locked
  device key is never re-minted. A refusal of the settings entity already
  on record no longer rewinds the marker every round.

Not yet:
- The Spaces step of the setup wizard (map local Spaces to account Spaces
  before the first publish); unmapped Spaces are kept separate meanwhile.
- A remote rebind of the Space holding the default role is refused by
  `SetSpaceProfile` and parks.

### S6 — cross-account flows (in progress, 2026-09-24)

- **Guest → account** (the Mac's GuestDataMigration): signing in from Guest
  Mode moves the Guest account's Spaces, bookmarks, pins, URL rules and
  Space themes into the account, then deletes the Guest directory.
  `phi_guest_migration.*` (host-clean, `guest_migration_test`): the Mac's
  snapshot (hidden roots cut, live rules only, missing custom Space =
  refusal), planner (ids kept unless taken → `guest-import-<op>-<kind>-<n>`;
  default-Space bookmarks into an "Imported from Guest" folder; pins
  projected onto the target scope and deduplicated by lineage + content,
  split partners remapped; a rule whose (Space, host, path) the target has
  is skipped), `LocalStore::ImportGuestData` (ONE flush; present = no-op,
  partial = refused) and the receipt verification.
  `phi_guest_migration_win.*`: the journal
  `<UserData>\Phi\guestDataMigration\pending.json` (prepared →
  targetImported → sourceDirectoryStaged), the receipt
  `<account>\guestDataMigrations\receipts\<op>.json`, theme merge (target
  wins: spaceThemeIds, spaceOverlayOpacities, winSpaceSaturations), and the
  cleanup (rename to `.default-account-id.guest-migration-<op>.pending-delete`,
  delete). The coordinator runs it where access moves from Guest to a
  finished account (`ReconcileAccessAfterAuth`, `OnboardingFinished` from
  Guest): Guest store closed → migrate → the account opens → open tabs are
  re-tagged through the receipt's modelGUIDs → Guest directory removed.
  `LoadStores` resumes a journal before any account opens (same target:
  finish; another account opening: a finished import only removes the
  Guest directory, a prepared one that never landed is dropped).
  Deviations: an imported rule gets a sync id (the Mac leaves it nil until
  its V12 backfill); a failed migration does not block the sign-in — the
  Guest data stays in the Guest account (the Mac refuses the sign-in).
  NOT live-tested: it needs a sign-out and an interactive Auth0 sign-in.
- **Backup/restore**: the invariant already holds by layout — the marker
  and cursor tables live in the account folder and roll back with it,
  `hlcMax` and the rest of the engine state live in
  `Phi\syncstate\<account>.json` outside it. Windows has no Export/Import
  User Data (the Mac's Help menu zip, with Sentinel AI data) — a separate
  feature.
- **Local data removal**: the Mac's UserDataRemoval runs for account
  deletion and the debug "Clear User Data"; Windows has neither flow, so
  nothing to port yet.

### Mac parity ports (2026-10-27)

- **Pins parked for another pinned-tab scope** (Mac 4d6a7bf2, #206):
  `OwnedCursor.parked_for_scope` (file key `parkedForScope`, absent = false)
  marks a pin landing parked only because its owner resolves locally but in
  another scope (owner `app`, a mapped Space or a mapped Profile while the
  device runs another scope — a dormant copy of another shape). Set from
  `OwnedLandingOutcome.parked_for_scope` (PinOwnedKind::Land), cleared on
  every other cursor write (arrival, land, tombstone, pending-delete,
  minted write-back, any other parking) and for every identity a landing
  round attempted without parking it for scope again. `FinishStatusRound`
  folds `pending_apply` into pending inbound only when the flag is clear,
  so such a device is not "Needs attention" forever; the copy is still
  replanned every round and lands when the scope changes. The engine has no
  logger, so the per-kind count travels as
  `OwnedRoundCounters.parked_for_scope` and the key service logs
  `[PhiSync] pins parked for another scope=N`.
- **Full sync reset from the last device** (Mac 055df916, PHI-1797):
  `KeyApi::ResetAccount` = `PUT /keys/v1/reset-operations/{uuid}` with
  `{"device_key_id"}`; 200 `{"outcome":"reset"}` → ok, 409 `not_last_device`
  → `kNotLastDevice`, 404 `device_not_found` → `kDeviceNotFound` (matched on
  the body `code`; any other status is `kHttp`). `RevokeDevice` now maps 409
  to `kLastActiveDevice` ONLY with code `last_device`. `MarkerFile.
  reset_operation_id` (`resetOperationId`) is the journal:
  `PhiSyncKeyService::ResetSyncAndRemoveThisDevice` stops the engine
  (which writes marker.json from its mirror — no file lock needed because
  the engine never starts while the UUID is set), journals the UUID
  (reusing one left by an earlier attempt), PUTs, and on `reset` runs the
  shared removal cleanup (`ClearLocalSyncState(true)` → marker deleted →
  UUID gone). A refusal clears the UUID and leaves local state; any other
  failure keeps it. `Worker::NeedsReconfiguration()` is true while the
  UUID is set, so a relaunched device offers "Set up sync again";
  `ReconfigureSync` retries the journaled PUT first (`reset` → cleanup with
  removal; refusal → plain reconfiguration; other failure → reported, UUID
  kept). `Snapshot.reset_resume_pending` picks the confirmation copy
  (`sync.reconfigure.resumeResetBody.windows`); a journaled reset also
  hides the exposed keys like a pending removal. Settings → Sync "Removal"
  is ONE row: "Reset sync / Reset…" when the device list shows this PC as
  the only active device or after a `last_device` refusal (two destructive
  confirmations: the row, then `sync.removal.resetConfirm*`), otherwise
  "Remove this PC from sync / Remove…"; `not_last_device` shows
  `sync.removal.resetRefusedNotLastDevice.windows` and reloads the list.
  Control pipe: `agentSpace.sync.keyOp` op `resetAccount`. The marker lives
  under the account dir on Windows, so nothing about account switching
  changes (the Mac's single marker store needed the keep-journal rule).
  Strings: Mac `sync.removal.*` keys reused where the copy is device-neutral
  (translations join by key); PC-worded variants carry a `.windows` suffix
  and are English-only until the translation repo has them (the generator
  joins by key, so reusing a Mac key would attach "this Mac" translations).
  Retired keys: `settings.sync.recoveryTitle/Body`, `settings.sync.remove`,
  `settings.sync.remove.lastDevice` (their Mac sources were deleted in
  3b450442/055df916; a regen would have dropped them to English anyway).
- **App URL rules** (Mac 48d4c4a6, #220): an App rule is an ordinary rule
  whose host is `app:` + lowercased bundle id, path prefix empty
  (`store::IsSourceAppRuleHost`: prefix case-insensitive, remainder
  non-empty and colon-free). `UrlRuleKind::Refuses` accepts it before the
  colon → `kMalformedHost` test. `SpaceSyncTable.inbound_acceptance_version`
  (`inboundAcceptanceVersion`, absent = 0, fresh table = current = 1) is the
  third `ArmSpaceReplayIfNeeded` trigger: a table an older build wrote
  replays data type 2000 once (marker nil first, version latched in the same
  write as the drain flags) so the rules that build refused — which recorded
  nothing — arrive. `UrlRuleMatchType::kSourceApp` shows the row in the URL
  rules editor as "App" with the bare bundle id (retargetable, ask-able,
  deletable; the picker still offers the four Windows kinds, so none is
  created here); `PushRoutingTableNow` skips App rules (Windows cannot know
  which app opened a URL, matching stays Mac-only). The Mac's
  `space gate open (… acceptance=a/b)` log line has no counterpart (pure
  engine, no logger).
- Host tests for these live outside the public repo (test/model was removed
  in 4f538b2): the engine, key-layer and URL-router suites were restored from
  that commit's parent, brought up to date with the current interfaces, and
  extended (`TestPinParkedForAnotherScopeIsNotHeld`,
  `TestAppRuleHostIsAccepted`, `TestStaleAcceptanceVersionReplaysOnce`,
  App-rule cases in url_router_test); they pass against this tree.

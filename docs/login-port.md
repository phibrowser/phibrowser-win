# Login on phibrowser-win — research and port plan

Date: 2026-09-09
Status: stages 1 and 2 built (see §5); box verification blocked on the
Auth0 dashboard callback (decision 1)
Sources: `phibrowser-mac` `Sources/UserInterface/Onboarding/*`, `AccountController/*`,
`Utilities/SharedAuth*`, `docs/plans/2026-07-31-guest-mode-design.md`;
`phibrowser-win` `client/phi_auth.*`, `phi_account_feedback.*`,
`phi_settings_window.cc`, `phi_feedback_outbox.cc`; fork
`chrome/browser/phinomenon/phi_bridge_wrapper_stub.cc`,
`phi_browser_proxy.cc`, `extensions/component_loader.cc`,
`ui/webui/settings/profile_info_handler.cc`;
`chromuim/src/windows-framework-status.md` §"BUCKET D: AUTH, stage 1".

## 0. Where Windows stands

**Stage 1 of auth exists and is committed** (`cdb30fb`, reworked by the
architecture-review phases 0/4). `client/phi_auth.{h,cc}` is the Mac
`AuthManager` core:

| Concern | Windows today | Mac |
|---|---|---|
| Flow | Auth0 Universal Login, auth-code + PKCE, hosted page opens in a Phi tab; **the Mac's callback URL** as redirect_uri, intercepted by a fork navigation throttle (was a loopback redirect until 2026-09-09) | Same hosted login, PKCE via Auth0.swift, **custom-scheme redirect** `com.phibrowser.Mac://auth.../callback`, opened in the *external* browser (Safari if Phi is default) because Phi owns http/https on the Mac |
| Tenant | staging triple hard-coded, `PHI_AUTH0_DOMAIN/_CLIENT_ID/_AUDIENCE` env overrides (**not** gated by `kPhiDevHooks`) | Debug: staging or prod triple in code; Release/Nightly: `Resources/Auth0.plist` (repo carries staging; release swaps the plist) |
| Scope | `openid profile email offline_access` | same |
| Session store | `PhiAuth.dat`, DPAPI, `{refreshToken, user{sub,name,email,picture}}`, atomic write via `PhiWriteFileAtomic`; access token memory-only | SDK keychain item (`service = bundle id`, `account = credentials`/`credentials-nightly`) + App-Group shared keychain for Sentinel + flock lock + heartbeat file |
| Refresh | lazy: `AccessTokenBlocking()` refreshes when no cached AT; launch validation in background; `refresh_lock_` serialises rotation; `generation_` drops stale outcomes | proactive timer (3600 s, PostHog-tunable), urgent window, two clocks, shared-store import, 45 s timeout, `AuthSessionGeneration` |
| Session death | `invalid_grant` ⇒ **silent local sign-out** | `invalid_refresh_token` ⇒ **reauthentication-required** state (modal "Sign in again"), same-`sub` check on the new login, grace session keeps the UI signed in |
| Sign-out | local only (delete `PhiAuth.dat`) | `/v2/logout` in external browser (`federated:false`) + fire-and-forget `/oauth/revoke` of the RT + local clear + PostHog reset, with a timeout that completes locally anyway |
| Identity | `PhiAuthUser{sub,name,email,picture}` from id_token claims; `sub` required | `User{sub,name,email,picture}`; `sub` required; then `GET /api/auth/profile` cached per account (`Profile{id,auth0_id,email,name,picture,last_login,created_at}`) + avatar PNG via Kingfisher |
| UI | Settings ▸ You pane (glyph avatar, name/email, Sign in / Logout); Send-Feedback "sign in to send" alert | 640×800 onboarding window: Login → SetName → SetTheme → LayoutSelection → PasswordManager → NextStep; Guest branch with privacy/consent page; Settings account card; AI "Sign in to use Phi AI" fallbacks |
| Consumers | feedback outbox (bearer on `PhiAuth::AccountBaseUrl()`), harness `agentSpace.feedback.setAccount` stand-in | Chromium bridge (`getAuth0AccessTokenSyncly` several times/s, `getPhiAccountInfo`, `isPhiGuestMode`, `canShowChromiumWindow`, `showLoginUI`), APIClient, Sentinel, service broker, PostHog/Sentry identify |

**No real login has ever completed on the box.** The 2026-08-20 status entry
stops at the Auth0 tenant answering *"Callback URL mismatch"*; nothing later
retracts it. The exchange, persist, rotation and refresh paths are
unexercised.

**Windows has no login wall.** `PhiBridgeWrapper::CanShowChromiumWindow()`
returns `true` and `IsPhiGuestMode()` returns `false` in the non-Mac stub.
The Mac gates *all* browser access behind login-or-guest; Windows is, in
Mac terms, permanently "guest" with an optional account on top.

**The token never reaches Chromium on Windows.**
`PhiBrowserProxy::GetAuth0AccessToken()` returns `""` outside
`IS_MAC_PHI`, and `PhiFrameworkDelegate` has no auth hook. Who would ask:
`chrome.phinomenonPrivate.getAuth0AccessToken` from the extensions
(Lexington / Sidecar / Kensington are **not loaded** on `IS_WIN_PHI_CLIENT`;
Mirage is, and uses it only to extract `sub` for analytics), and
`highlight_link_tab_helper.cc` (short highlight links, Mac-gated). So
nothing in Chromium is *broken* by the missing token today.

## 1. Decisions needed

1. **Auth0 dashboard.** RESOLVED 2026-09-09 (Jixiang): Windows uses the
   Mac's callback URL, `<bundle id>://<tenant>/macos/<bundle id>/callback`,
   which the tenants' applications already list (callback and logout) —
   staging under the canary bundle id `com.phibrowser.canary.Mac`,
   production under `com.phibrowser.Mac` (box-probed: staging refuses the
   release id). The browser intercepts the redirect itself (see §6). No
   dashboard change.
2. **Login wall or optional account?** Recommendation: **optional account,
   no wall, no onboarding gate.** Windows already behaves as Mac Guest
   Mode; the Mac's wall exists to feed the AI stack, which Windows does not
   ship. This decision removes the biggest Mac subsystems from the port:
   `BrowserAccessState`, the phase machine, `canShowChromiumWindow`
   gating, and the entire guest→account migration journal (2.6k lines).
3. **Per-account local stores or flat?** Mac keeps
   `users/<sub>/` per account (SwiftData V9) so Guest data can be merged
   into an account and later accounts stay separate. Recommendation:
   **stay flat.** Windows stores are account-blind (`PhiTabData.json`,
   `PhiSpaces.json`, …), there is no multi-account product, and sign-out
   on the Mac never deletes local data either. Record the account as an
   identity attached to the install, not a data partition. Revisit only if
   multi-account or a login wall is decided.
4. **Production tenant selection for release.** Env overrides are the only
   switch and they are read in release builds. Pick one: bake the prod
   triple behind a `build-client.py --release` define (mirrors the Mac
   plist swap), or a `PhiAuth0.json` resource. Either way gate the env
   overrides behind `kPhiDevHooks` like the other dev hooks.
5. **Telemetry identify on login.** Mac does PostHog `identify(sub)` +
   Sentry user after login and `reset()` on logout, but only when metrics
   consent is on. Windows telemetry has no consent surface yet
   (architecture-review open decision 6). Do not tie telemetry to the
   account until that pref exists.
6. **Chromium token hook — now or with the AI stack?** Recommendation:
   defer to the AI-stack port, but decide the shape now (§3 stage 3) so
   `phi_framework_api.h` grows once.

## 2. What to carry from the Mac, and what not to

Carry (semantics, not code):
- `sub` is the account key; missing `sub` fails closed (already done).
- On reauthentication, refuse a *different* `sub` than the one signed in
  (`AuthenticatedAccountIdentityPolicy`).
- Profile GET is a **compare-and-set** write against the cached profile so
  an in-flight SetName PUT is not overwritten by a slower GET.
- Avatar: the picture URL is stable while content changes, so fetch with
  cache bypass and fall back to the on-disk copy offline; the fork's
  `profile_info_handler.cc` caps bridge-supplied PNGs at 5 MB.
- Logout: remote session clear is best effort with a timeout; local
  clear always completes. Revoke the refresh token fire-and-forget.
- Never log `sub` or the redirect URI at INFO (lane 7 finding 10, still
  open at `phi_auth.cc:436,516,743`; `PhiRedactId` exists).

Do not port:
- Auth0.swift / JWTDecode — WinHTTP + `base::JSONReader` already cover it.
- External-browser launch and the custom scheme — loopback in a Phi tab is
  the RFC 8252 shape and avoids the Mac's http-handler conflict.
- App-Group shared keychain, `SharedTokenLock`, heartbeat file,
  `SharedAuth0Config` — all exist for Sentinel, which Windows lacks.
- The proactive renew timer, urgent window, two-clock bookkeeping and
  shared-store import/reverse-sync — they defend a multi-process rotating
  refresh token; Windows has one process and `refresh_lock_`. Lazy refresh
  on demand is enough until something needs a token continuously.
- The account-deletion credential fence file, unless deletion is ported.
- Guest privacy/consent page as a *gate* — but its two checkboxes (legal,
  metrics) are the natural first-run consent surface the review wants.

## 3. Change list, by stage

### Stage 1 close-out — make the mechanism real (small)

- Dashboard callback (decision 1), then box-verify: sign in, relaunch
  (restore + background validation), sign out, sign in again, and a forced
  `invalid_grant` (revoke the RT in the dashboard) to exercise
  `kSessionDead`.
- `phi_auth.cc`: redact `sub`/redirect URI in logs; gate `EnvOr` behind
  `kPhiDevHooks`; tenant selection per decision 4.
- `phi_settings_window.cc` `BuildAccount`: replace the six hard-coded
  strings with the catalog keys that already exist
  (`settings.youAreNotSignedIn`, `settings.signedInAs`,
  `settings.signInToUseYour`, `settings.logout`,
  `settings.finishSigningInFromThe`, `settings.account.displayName.fallback`);
  delete the stale "Sign-in does not exist on this build" comment.
- Tests: `test/model` unit tests for PKCE (verifier/challenge vectors),
  callback-line parsing + state check, id_token claim parsing, and the
  `PhiAuth.dat` round trip (DPAPI is user-bound, so round-trip on the same
  box). Harness verbs `account.status` (`signedIn`, `signingIn`, `sub`
  redacted, `email`) and `account.signOut`, `test_rig_only`, so
  `suite-feedback.mjs` can stop asserting that uploads *fail*.

### Stage 2 — identity surface (the "login feature" users see)

- **`PhiAccountService`** (new, owned by `PhiAccountAndFeedback`): on
  sign-in and on launch-restore, `GET {AccountBaseUrl}/api/auth/profile`
  on a worker with `AccessTokenBlocking()`, 401 ⇒ `InvalidateAccessToken`
  + one retry; cache `Profile` in `PhiAuth.dat` next to the id_token
  identity (CAS write); `PUT` for display-name edits. Avatar: WinHTTP GET
  of `picture`, decode with WIC, store `PhiAvatar.png` in the user-data
  dir keyed by `sub`, invalidate on sign-out.
- **Settings ▸ You**: real avatar (round, via the `phi_dwrite` AA layer),
  editable display name, email, Sign out; signing-in state with a Cancel
  that closes the loopback listener (today a started sign-in can only
  time out after 300 s).
- **Entry points**: the Mac has none beyond the login window, Settings
  and the AI prompts — no sidebar avatar, no app-menu item. Windows keeps
  Settings ▸ You and the feedback form's prompt.
- **Reauthentication instead of silent sign-out**: on `kSessionDead` keep
  the cached identity, mark `reauth_required`, show a `PhiNotice`
  ("Sign in again to continue") with Sign in / Sign out; `AccessTokenBlocking`
  returns empty while required; the new login must return the same `sub`
  or is rejected. Persist the flag in `PhiAuth.dat`.
- **Real sign-out**: open `https://{domain}/v2/logout?client_id=…&returnTo=…`
  in a tab (needs `returnTo` allow-listed in the dashboard), POST
  `/oauth/revoke {client_id, token}` fire-and-forget, then the local clear.
- **Sign-in as a foreground tab**: reuse the tab for the loopback reply
  page; today the reply is unstyled HTML — give it the Phi look and, on
  success, close the tab or navigate it back (`FocusWindowForBrowser` exists).

### Stage 3 — Chromium bridge (with the AI-stack port; shape decided now)

Add to `phi_framework_api.h`, gated the same way as the other
`IS_WIN_PHI_CLIENT` hooks:
- `PhiFramework::SetPhiAccountSession(const char* access_token, int64_t expires_at_unix, const char* sub)` — the **client pushes** the token so the fork's sync hot path (`GetAuth0AccessTokenSyncly`, called several times per second on the Mac) reads process memory instead of crossing to the client; the client re-pushes after every refresh and pushes empty on sign-out.
- `PhiFramework::SetPhiAccountInfo(const char* nickname, const char* email, const uint8_t* avatar_png, size_t len)` for `chrome://settings` people page (`phiName`, `email`, `phiIconUrl`, `phiGuest`), and `IsPhiGuestMode` ⇒ `!signed_in`.
- `PhiFrameworkDelegate::OnShowLoginUI()` for `PhiAccountHandler::HandleShowLoginUI` → `RequestSignIn()`.
- `phi_bridge_wrapper_stub.cc`: route `GetAuth0AccessTokenSyncly`, `GetPhiAccountInfo`, `IsPhiGuestMode`, `ShowLoginUI` to the framework-held values under `IS_WIN_PHI_CLIENT`; leave `CanShowChromiumWindow` at `true` (decision 2).
- `PhiBrowserProxy::GetAuth0AccessToken` and `profile_info_handler.cc`
  currently narrow to `IS_MAC_PHI`; widen to `IS_PHI_BROWSER` at that point.

### Stage 4 — product-dependent (not scheduled)

- First-run window: Mac's Login/Guest → SetName → Theme → Layout →
  Password manager → Next step. Windows has layout selection in the menu
  and a native settings window; the only piece with an independent reason
  to exist now is a first-run **consent** page (legal + metrics), which
  the telemetry review already asks for.
- Account data export and deletion (Oblivion, email-code verification,
  the deletion fence), invitations, connectors — all backend features the
  Windows client has no UI for.
- Per-account stores and guest→account migration — only if decision 2 or
  3 flips.

## 4. Reference: Mac constants worth having at hand

- Staging: `auth.stag.phibrowser.com` / `360ZYEcp1T2AexR9rzoUkIn0VTVWc0pv` /
  `https://phibrowser-stag.us.auth0.com/api/v2/`, account service
  `https://account.stag.phibrowser.com`.
- Production: `auth.phibrowser.com` / `jrFgfm2FA8DoPBzs4ysOePZKwFCcDbo4` /
  `https://phibrowser.us.auth0.com/api/v2/`, account service
  `https://account.phibrowser.com`.
- Profile API: `GET|PUT /api/auth/profile`, bearer AT, body `{"name"}` on
  PUT, `Response<Profile>` with `code == 0` on success.
- Mac renew defaults: check every 3600 s, renew when < 3600 s left
  (PostHog flag `auth0-refresh-timing`), 45 s renew timeout.
- Mac logout: `clearSession(federated: false)`; revoke via
  `authenticationClient.revoke(refreshToken:)`, never
  `CredentialsManager.revoke()` (its callback clears whatever is stored).
- PostHog events: `user_logged_in`, `user_logged_out`, `login_retried`,
  `guest_mode_entered`, `guest_mode_exited{trigger}`, `metrics_reporting_changed`.

## 5. What was built (2026-09-09)

Decisions taken as recommended: optional account, flat stores, no
onboarding gate, telemetry untouched, Chromium hook deferred. Tenant: was
by channel (`PHI_WIN_CHANNEL` "stable" ⇒ production, anything else ⇒
staging, the Mac's Release/Nightly split); since 2026-10-16 every ring
signs in against PRODUCTION — Windows testers hold production accounts —
and the ring (feed pairing, feedback and analytics channel) is untouched.
Staging is reached only through the env overrides, under `kPhiDevHooks`.

New files:
- `client/phi_auth_codec.{h,cc}` — the pure half of the sign-in: base64url,
  SHA-256, the PKCE challenge, percent-coding, the callback request line,
  the JWT payload split, the tenant → account-service rule, the loopback
  reply page. Host-tested by `test/model/auth_codec_test.cc` (RFC 7636 and
  FIPS vectors).
- `client/phi_http.{h,cc}` — the one WinHTTP round trip; the outbox's copy
  now delegates to it.
- `client/phi_account_profile.{h,cc}` — `PhiAccountProfileService`: GET/PUT
  `/api/auth/profile` with bearer + 401 retry, CAS-free but subject-checked,
  cached in `PhiAccountProfile.json`; avatar bytes in `PhiAvatar.img`,
  decoded through WIC into a circle per size on demand. Single-writer like
  PhiAuth (workers post through the same notify message).
- `client/phi_verbs_account.cc` — `agentSpace.account.status / signIn /
  cancelSignIn / signOut` (kHost, test rig only).

Changed:
- `phi_auth.{h,cc}`: codec + shared HTTP; tenant by channel; `sub` and the
  redirect redacted from logs (port only); **cancel** (an event beside the
  accept event, `WSAEventSelect`); **reauthentication-required** state on
  `invalid_grant` (identity kept, no bearer, persisted as
  `reauthRequired`), other refresh errors no longer kill the session; a
  reauthentication returning another `sub` is refused
  (`last_error = account_mismatch`); `SignOut` revokes the refresh token
  (`/oauth/revoke`, fire-and-forget) and `LogoutUrl()` gives the `/v2/logout`
  page; `callback_origin()` names the loopback tab; the reply page is
  styled and localized.
- `phi_account_feedback.{h,cc}`: owns the profile service; closes the
  loopback tab when the exchange lands (`Host::CloseTabsAtOrigin`); sign-out
  opens the tenant's logout page in a tab; rename, avatar editor (the
  account service's `/avatar` page in a tab, re-fetch on the next
  settings activation), rate-limited refresh.
- `phi_settings_window.{h,cc}`: the You pane — real avatar (`kAvatar`,
  click = editor), profile name, reauthentication warning +
  Reauthenticate, Cancel while signing in, sign-out confirmation, error
  line, Change Name… (PhiPromptText + the Mac's two validations, 100
  chars) / Change Picture… rows; every string from the catalog; re-fetch
  on WM_ACTIVATE. `PhiSettingsHost::Account()` is mutable now.
- `phi_coordinator.{h,cc}`: `CloseTabsAtOrigin` over the window model.
- Strings: the Mac's `settings.account.*` / `oobe.reauthentication.*` keys
  (translations by key) plus Windows-only `auth.callback.*`,
  `settings.account.error.*`, `settings.account.signedInAs` ({0}) etc.;
  `settings.signedInAs` (trailing-space concatenation) and `settings.logout`
  retired.

Not done / deliberately skipped:
- No modal "Sign in again" alert when the session dies mid-run (the Mac
  shows one); the pane warning and the feedback prompt carry it. Add if
  users miss it.
- `/v2/logout` has no `returnTo` (needs an allow-list entry); the tab
  lands on Auth0's own signed-out page.
- Box-smoked 2026-09-09 (`test/probes/account-smoke.mjs`, CDP probes):
  launch restores cleanly, the You pane renders, `account.signIn` opens the
  hosted login and the staging tenant serves the REAL login page ("Log in
  to Phi Browser Stag"), `cancelSignIn` returns the pane to signed-out with
  the error line, and a CDP `Page.navigate` of a tab to the callback scheme
  is taken by the throttle and the tab retired (no external-protocol
  dialog). Everything past the login page needs a human with staging
  credentials. Verification checklist: sign in;
  loopback tab closes; avatar + name appear; relaunch restores; rename
  round-trips; revoke the RT in the dashboard ⇒ pane shows
  reauthentication ⇒ Reauthenticate with the same account clears it, with
  another account is refused; sign out ⇒ logout tab, `PhiAuth.dat`,
  `PhiAccountProfile.json`, `PhiAvatar.img` gone.

## 6. The callback: the Mac's URL, intercepted in the browser (2026-09-09)

Jixiang's call: no loopback, no dashboard entry — Windows sends the exact
redirect_uri the Mac app registers as a URL scheme,
`com.phibrowser.Mac://<tenant>/macos/com.phibrowser.Mac/callback`, and since
Phi is the browser the hosted login runs in, the redirect to that scheme is
a navigation Phi sees before anything else.

- **Fork** (`IS_WIN_PHI_CLIENT` only): `chrome/browser/phinomenon/
  phi_auth_callback_navigation_throttle.{h,cc}`, registered after the Space
  routing throttle. Main-frame `WillStartRequest`/`WillRedirectRequest`: a
  `com.phibrowser.mac` / `com.phibrowser.canary.mac` scheme goes to the
  client through the new delegate `PhiFrameworkDelegate::OnAuthCallbackUrl
  (url, bool* taken)` (hook plumbing in `phi_framework_hooks` +
  `phi_framework_impl`). Taken ⇒ `CANCEL_AND_IGNORE` — this runs before
  `HandleExternalProtocol`, so no "open external app" dialog — and the tab
  the callback landed in is retired on a posted task (closed, or sent to a
  new tab when it is the window's last). Not taken ⇒ `PROCEED`, Chromium's
  usual unknown-scheme handling.
- **Client**: `PhiAuth::BeginSignIn` keeps the PKCE verifier and state in
  memory and opens the authorize URL; `PhiAuth::HandleCallbackUrl` (from
  `PhiFrameworkDispatcher::OnAuthCallbackUrl`, inside the throttle — no UI,
  no blocking) checks scheme/host/path against the tenant's callback and
  the `state`, then runs the code exchange on a worker; the bare callback
  (the logout page's `returnTo`, the Mac's `clearSession` shape) is
  acknowledged so its tab closes too. `CancelSignIn` bumps the attempt so a
  late exchange is dropped. `redirect_uri` on the token exchange is the
  same string. The bundle id follows the tenant — `com.phibrowser.canary.Mac`
  on staging, `com.phibrowser.Mac` on production — the pairing the Mac's
  Nightly and Release builds ship with.
- The codec grew `PhiMacCallbackUri` and `PhiParseCallbackUrl` (host-tested,
  incl. GURL's lower-cased scheme/host); the loopback listener, its reply
  page and the `auth.callback.*` strings are gone.

## 7. The You pane, the Mac's (2026-09-09, after the first real sign-in)

Jixiang signed in on the box (exchange → profile → avatar all landed), and
asked for the pane to match the Mac: the profile card, and the name where
"You" was.

- `client/phi_profile_card.{h,cc}` — the Mac's ProfileCardViewController +
  ConchFrameAnimationBaseViewController: a 240×380 card, a 10×10 grid of
  conch renders the user drags through (20 dips a step, spring-back to the
  centre in 150 ms, ease-out cubic), a 24-frame intro played once, all
  colourised to the theme (black → white, ≥10% grey → HSB(hue, .85, .75);
  Pure = grey), a 10% tint and the dot pattern at 7.5% over it, the
  sign-up date in Impact (70 → 20 dips until it fits) hanging 12 dips off
  the top and fading out over its lower 40%, the name (20 chars + "…") in
  the accent (HSB(hue, 1, .37); Pure = black) 32 dips up. "Download image"
  renders the card at 2× centred on an accent canvas twice its size and
  saves a PNG (the Mac's snapshotAndExport). Frames decode on a worker at
  the card's pixel size — centre, intro, then by distance from the centre.
- Assets: `client/resources/profile-card/` — the Mac's
  `setting_<r>-<c>.jpg` (→ `frame-<r>-<c>.jpg`), `setting-pre-<n>.jpg`
  (→ `pre-<n>.jpg`) and `setting-dot-bg` (a PDF, rasterised at 144 dpi to
  `dot-bg.png`); 9 MB, copied by build-client.py beside the exe.
- The pane: signed in, the card on the left with the button under it; the
  right column is the Mac's AccountView row — the avatar with its pencil
  badge (click = avatar editor), the name with a trailing pencil (click =
  rename), the email, a status line (reauthentication / saving / error),
  Sign out — then Default Browser. Signed out, the card is hidden and the
  column takes the width, as the Mac's Guest presentation does. The
  Change Name / Change Picture rows went (the pencils are those).
- The Share section (invitation codes) is not ported — invitations are
  bucket D backend work.

## 8. The first-launch flow and the gate (2026-09-10)

Jixiang: "migrate the first install login step, gate the browser like
mac." Decision 2 (optional account, no wall) is therefore reversed;
decision 3 (flat stores, no guest→account migration) stands — a guest who
signs in keeps the same stores.

**Access** (`PhiCoordinator::Access`, the Mac's BrowserAccessState):
`kLoginRequired | kGuest | kSignedIn`. Persisted: `guestModeEnabled` and
`onboardingPhases{sub: phase}` in PhiSettings.json (the Mac's
`guestModeEnabled` and per-account `loginPhase`), plus `skipOnboarding`
(test rig; also `--skip-onboarding`). Resolved in `ResolveInitialAccess`
after the stores and the session load, before any window: a session whose
phase is 6 ⇒ signed in; else the guest choice (an unfinished session is
discarded, the Mac's discardUncommittedLoginCredentials); else skip; else
login required.

**The gate**: `PhiShellWindow(…, start_hidden)` — shells announced while
login is required are built and laid out but not shown (`ShowHeldShell` on
grant, `HideShell` on a sign-out, which re-arms the gate). The default-shell
rescue defers behind the gate. `GrantAccess` shows the held shells, closes
the onboarding window and its browser, and opens a shell if none exists.
Closing the onboarding window behind the gate quits (every browser is
closed); over a usable browser (a guest signing in) it is just dismissed.

**The window**: `client/phi_onboarding_window.{h,cc}`, a 640×800
PhiSecondaryWindow with the Mac's pages — Login (the video's resting frame
as a still, the brand, the gradient-ring "Sign in" pill, "Explore Phi
without signing in"), SigningIn (the hosted login EMBEDDED: a plain
browser requested by the coordinator, reparented into the window; a shadow
browser was tried first and cannot take the keyboard because its view skips
activation), Before we begin (guest consents; metrics default off in
Europe), What should I call you? (an EDIT; PUT only when changed),
Welcome (the membership card — PhiHolographicCard, docs/windows-native-ui-impl.md
Round K — centred on the dark ground with a dice that randomises its style
and switches the app theme to the palette's; no title, no swatches), Layout
selection, Next steps
(the four steps + consents + Let's Begin). Phase 4 (Passwords Manager)
is skipped. Titles use IvyPresto Display SemiBold Italic, registered
privately from resources/fonts (Latin only; Segoe UI otherwise).

**Chromium's first run**: the client main now passes `--no-first-run` and
`--no-default-browser-check` — the "set as default" welcome page has no
place behind the onboarding.

**Lesson**: opening the window from inside OnFrameworkReady starved
Chromium's startup window (it never got created); the open is posted to a
turn of its own (`WM_PHI_OPEN_ONBOARDING`).

**Harness**: `agentSpace.onboarding.status` / `.drive {action, value}`
(test rig). Rig: `PhiFxLaunchOobe` launches a fresh profile at
`C:\phi-fx-oobe`; `set-shot.ps1` takes `PHI_SETSHOT_CLASS` / `_HWND`.
`seed-fx-prefs.sh` seeds `skipOnboarding` so the suites run ungated.

## 9. The hosted login goes off the record (2026-10-16)

Jixiang: "run the hosted login in an off-the-record browser." Until now the
Auth0 pages ran in a browsing profile — the onboarding's embedded browser in
`Default`, a Settings ▸ You sign-in as a tab of the first user Space — so the
tenant's SSO cookie persisted in that profile's jar next to the user's own
cookies, and sign-out depended on the logout page landing in the right jar.

**Now**: `RequestOnboardingBrowser` asks for the embedded browser with
`incognito=true`. The framework's `kSpace + incognito` answer is a window in
its Incognito Space OTR profile (`CreateIncognitoSpaceBrowser`), which dies
with its last window — this one, unless an Incognito Space is open, in which
case the cookie lives until that Space closes. The Auth0 cookie never reaches
a persistent jar.

**One path for every sign-in**: `PhiAccountAndFeedback::RequestSignIn` →
`Host::OpenHostedLogin(url)` → `PhiCoordinator::OpenHostedLogin`, which opens
the onboarding window on its SigningIn page when it is not up (a guest from
Settings ▸ You, a reauthentication-required account's "Sign in again", the
feedback form, `agentSpace.account.signIn`) and navigates the embedded
browser. Such a window is a **standalone sign-in**
(`PhiOnboardingWindow::SetStandaloneSignIn`): cancel, a refused sign-in or
closing the window dismiss it (and abandon the attempt) instead of showing
the gate's Login page; success runs the usual `OnAccountChanged` route
(finished account ⇒ `OnboardingFinished` ⇒ `GrantAccess`, which closes the
window and its browser; a new account ⇒ its remaining pages).
`OpenUrlInFirstUserSlot` remains for the account site's own pages (avatar
editor). `RequestSignInVia` is gone.

**Sign-out** no longer opens the tenant's logout page: there is no cookie in
a browsing profile to end; the refresh-token revoke stays. `LogoutUrl()` is
kept but unused. A cookie left by an OLDER build in `Default` is not wiped
(it only matters to a tab the user opens on the tenant domain by hand).

**Trade-off**: the tenant asks for credentials on every sign-in (no SSO
cookie survives). The refresh token in `PhiAuth.dat` is the session; this
only shows after an explicit sign-out.

**Sign-out from Settings** (box test, Jixiang): the Settings window stayed up
over the Login page. `RequireLogin` now posts it `WM_CLOSE` (which hides it)
along with hiding the shells.

**Harness**: `agentSpace.account.signIn` no longer needs a user window;
`agentSpace.onboarding.drive {action:"cancel"}` on a standalone window
closes it.

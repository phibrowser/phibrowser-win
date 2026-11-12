// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_AUTH_H_
#define PHIBROWSER_WIN_CLIENT_PHI_AUTH_H_

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace phi_client {

// The signed-in identity, from the Auth0 id_token's claims (the Mac
// AuthManager's UserInfo shape).
struct PhiAuthUser {
  std::string sub;      // The Auth0 subject — the Mac Account's userID.
  std::string name;
  std::string email;
  std::string picture;  // Avatar URL; PhiAccountProfile fetches it.
};

// The Windows twin of the Mac AuthManager's core: Auth0 sign-in and the
// session it leaves behind (docs/login-port.md). The account is OPTIONAL
// here — there is no login wall, no onboarding gate and no guest journal;
// Windows is the Mac's Guest Mode with an account on top.
//
// The sign-in is the Mac's: Auth0's hosted login with authorization code +
// PKCE, redirecting to the SAME callback the Mac app registers as a URL
// scheme — `com.phibrowser.Mac://<tenant>/macos/com.phibrowser.Mac/callback`
// — so the tenant's application needs no Windows-specific entry. Phi is a
// browser: the hosted page opens in a Phi tab, and the redirect to that
// scheme is a navigation the framework intercepts
// (phi_auth_callback_navigation_throttle → PhiFrameworkDelegate::
// OnAuthCallbackUrl → HandleCallbackUrl here) before Chromium would try to
// launch an external app for it. The code exchange happens over WinHTTP.
// The refresh token (and the cached identity) persist DPAPI-encrypted in
// PhiAuth.dat — the PhiCredentials.dat pattern; access tokens live in
// memory only.
//
// Tenant: production for every ring (since 2026-10-16; the canary ring no
// longer follows the Mac Nightly onto staging). A dev-hooks build
// (kPhiDevHooks) also honours PHI_AUTH0_DOMAIN / _CLIENT_ID / _AUDIENCE and
// PHI_ACCOUNT_BASE_URL, which is how staging is still reached.
//
// Threading: SINGLE WRITER. The session (signed_in_, user_, refresh_token_)
// is UI-thread state, read by the coordinator and written only by
// ApplyOutcome on the UI thread. The exchange and refresh threads never
// touch it: they run the network round trip, then post an Outcome to
// PhiAuth's own message-only window, whose WndProc applies it, persists,
// and forwards `notify_msg` to the coordinator. What a worker needs to do
// its job (is there a session, which refresh token, the cached access
// token) lives in a lock-protected copy the UI thread keeps in step.
//
// Session death: when the tenant refuses the refresh token (invalid_grant —
// revoked, expired, rotated away) the session enters REAUTHENTICATION
// REQUIRED rather than vanishing: the identity stays for the UI, no bearer
// is handed out, and the next sign-in must return the SAME subject (the
// Mac's AuthenticatedAccountIdentityPolicy) or is refused.
class PhiAuth {
 public:
  PhiAuth();
  ~PhiAuth();

  PhiAuth(const PhiAuth&) = delete;
  PhiAuth& operator=(const PhiAuth&) = delete;

  // Loads the persisted session (if any) and kicks a BACKGROUND refresh to
  // validate it — an invalid_grant (revoked/expired session) enters the
  // reauthentication state and notifies, the Mac's refreshAuthStatus-at-
  // launch. `notify_hwnd` gets `notify_msg` posted after any state change,
  // always from the UI thread and only after the change has been applied.
  // UI thread.
  void Load(const std::wstring& user_data_dir,
            HWND notify_hwnd,
            UINT notify_msg);

  // Starts the interactive sign-in: builds the PKCE authorize URL and opens
  // it through `open_url` (a Phi tab), then waits for HandleCallbackUrl.
  // No-op while one is already in flight. Completion (either way) posts the
  // notify message. UI thread.
  void BeginSignIn(std::function<void(const std::string& url)> open_url);
  // Abandons the sign-in in flight: a callback arriving later is ignored.
  // No-op when none is in flight. UI thread.
  void CancelSignIn();
  // A navigation reached the callback scheme (the framework's throttle,
  // synchronously inside it — no UI here). True when the URL is this
  // tenant's callback: a code for the sign-in in flight starts the exchange
  // on a worker, a refusal ends the attempt, and the bare callback (the
  // logout page's return) is simply acknowledged. False for anything else,
  // which Chromium then treats as any unknown scheme. UI thread.
  bool HandleCallbackUrl(const std::string& url);

  // Drops the local session (PhiAuth.dat and memory) and revokes the
  // refresh token at the tenant (fire-and-forget, the Mac's
  // startRefreshTokenRevocationForLogout). The tenant's SSO cookie lived in
  // the off-the-record login browser and went with it: nothing to end. UI
  // thread.
  void SignOut();
  // https://<domain>/v2/logout?client_id=…&returnTo=<callback> — ends the
  // hosted-login session and returns to the callback, which
  // HandleCallbackUrl acknowledges. Unused since the login went off the
  // record (kept for the dev hooks and a persistent-profile fallback).
  std::string LogoutUrl() const;
  // "https://<domain>/": where the hosted login and the logout page live.
  std::string TenantOrigin() const;

  // The bearer token for an account-service call, for a WORKER thread: the
  // cached access token, or a fresh one from a refresh round trip when none
  // is cached — the Mac's getAccessTokenSyncly. Empty when signed out,
  // reauthentication is required, or the refresh fails. Blocks on the
  // network, so never from the UI thread.
  std::string AccessTokenBlocking();
  // The bearer for Chromium sync, for the UI THREAD (the Mac's
  // getAuth0AccessTokenSyncly as Chromium's sync service calls it): the
  // cached access token while it has at least a minute left, else empty —
  // and then ONE background refresh starts, whose success posts the notify
  // message so the caller asks again. Never blocks.
  std::string AccessTokenForSync();
  // Chromium sync's server for this tenant, the value of --sync-url
  // (the Mac ChromiumLauncher's default): https://sync[.stag].phibrowser.com
  // /chromium-sync, following the auth domain so the token and the server
  // always belong together. PHI_SYNC_URL overrides it in a dev-hooks build.
  static std::string SyncServerUrl();
  // The sync service's origin (no path): /keys/v1 and /chromium-sync live
  // under it — the Mac's KeyEnvelopeAPIClient.syncBaseURL, which follows
  // the tenant the same way. PHI_SYNC_BASE_URL overrides it.
  static std::string SyncBaseUrl();
  // AccessTokenBlocking, BOUND to an account: empty unless the token's
  // `sub` is `subject` — a key flow started for one account never sends its
  // next request with another account's bearer (the Mac's
  // SyncKeyStack.boundToken). Renews a token with under a minute left.
  // Worker threads only.
  std::string AccessTokenBlockingFor(const std::string& subject);
  // Forgets the cached access token so the next AccessTokenBlocking refreshes
  // — a caller's answer to a 401. Any thread.
  void InvalidateAccessToken();
  // The account service this tenant's tokens are for — the Mac APIClient's
  // accountBaseURL (no trailing slash).
  static std::string AccountBaseUrl();

  bool signed_in() const { return signed_in_; }
  bool signing_in() const { return signing_in_; }
  // Signed in, but the tenant no longer honours the session: the identity
  // is shown, no token is served, and Sign in again is the way out.
  bool reauth_required() const { return reauth_required_; }
  const PhiAuthUser& user() const { return user_; }
  // Why the last sign-in attempt did not land, for the Account pane:
  // "account_mismatch" (a reauthentication returned another subject),
  // "refused" (the tenant said no), "incomplete" (abandoned / offline /
  // a malformed exchange). Empty after a successful sign-in or a fresh
  // attempt.
  const std::string& last_error() const { return last_error_; }

 private:
  struct ExchangeJob;
  // A worker's result, applied on the UI thread.
  struct Outcome;

  // Worker side. Exchanges `code`; posts kSignedIn or kSignInFailed.
  void CompleteSignIn(const std::string& code,
                      const std::string& verifier,
                      uint32_t attempt);
  // Worker side, under refresh_lock_: the grant_type=refresh_token round
  // trip. Posts kRefreshed or kSessionDead; nothing on a transport failure.
  // True when the session survives.
  bool RefreshSession();
  // Any thread: hands `outcome` to the UI thread.
  void PostOutcome(std::unique_ptr<Outcome> outcome);
  // UI thread: the ONE writer of the session (a rotated refresh token alone
  // is stored by RefreshSession first, so a crash cannot lose it).
  void ApplyOutcome(const Outcome& outcome);
  void Persist();
  void NotifyChanged();

  static DWORD WINAPI ExchangeThread(void* param);
  static DWORD WINAPI RefreshThread(void* param);
  static DWORD WINAPI SyncRefreshThread(void* param);
  static DWORD WINAPI RevokeThread(void* param);
  static LRESULT CALLBACK OutcomeWndProc(HWND hwnd,
                                         UINT msg,
                                         WPARAM wparam,
                                         LPARAM lparam);

  std::wstring store_path_;
  HWND notify_hwnd_ = nullptr;
  UINT notify_msg_ = 0;
  // PhiAuth's own message-only window (UI thread), the sole road back from
  // a worker: Outcomes arrive here and ApplyOutcome runs in its WndProc.
  HWND outcome_hwnd_ = nullptr;

  // UI-thread-owned session. Written by ApplyOutcome and SignOut only.
  bool signed_in_ = false;
  bool signing_in_ = false;
  bool reauth_required_ = false;
  PhiAuthUser user_;
  std::string refresh_token_;
  std::string last_error_;
  // The sign-in in flight: the PKCE verifier and the state the callback
  // must echo. Cleared when the callback lands or the attempt is cancelled.
  std::string pending_verifier_;
  std::string pending_state_;
  // Which attempt a posted exchange belongs to; a result for an earlier,
  // cancelled attempt is dropped.
  uint32_t attempt_ = 0;
  // Bumped on every sign-in and sign-out so an Outcome computed against an
  // earlier session (a refresh that was in flight when the user signed out)
  // is recognised and dropped rather than applied to the next one.
  uint32_t generation_ = 0;

  // The workers' copy of the session, under token_lock_. The UI thread
  // rewrites it on sign-in/out; a refresh round trip updates the tokens
  // itself (a rotating tenant revokes the old refresh token at once, so the
  // next worker must see the new one before the UI thread gets around to
  // the Outcome). The access token lives ONLY here: memory, never disk.
  CRITICAL_SECTION token_lock_;
  uint32_t worker_generation_ = 0;
  bool worker_signed_in_ = false;
  std::string worker_refresh_token_;
  std::string worker_access_token_;
  // The identity the session file carries, for a worker storing a rotated
  // refresh token itself (RefreshSession persists before it publishes).
  PhiAuthUser worker_user_;
  // Serializes refresh round trips: a rotating tenant hands out a new
  // refresh token per exchange, so two concurrent refreshes (the launch
  // validation and a worker asking for a bearer token) would invalidate
  // each other's session.
  CRITICAL_SECTION refresh_lock_;
  // AccessTokenForSync's one background refresh at a time.
  std::atomic<bool> sync_refresh_in_flight_{false};
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_AUTH_H_

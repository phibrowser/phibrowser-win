// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_auth.h"

#include <bcrypt.h>
#include <wincrypt.h>

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "client/phi_auth_codec.h"
#include "client/phi_features.h"
#include "client/phi_file_io.h"
#include "client/phi_http.h"
#include "client/phi_log.h"
#include "client/phi_version.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace phi_client {

namespace {

// The two Auth0 applications the Mac ships: its Debug/Nightly builds use
// the staging tenant, Release the production one (AuthManager.swift's
// useStagingAuth0 / Resources/Auth0.plist). Windows keys the same split on
// its release channel (phi_version.h).
struct Tenant {
  const char* domain;
  const char* client_id;
  const char* audience;
};
[[maybe_unused]] constexpr Tenant kStagingTenant = {
    "auth.stag.phibrowser.com", "360ZYEcp1T2AexR9rzoUkIn0VTVWc0pv",
    "https://phibrowser-stag.us.auth0.com/api/v2/"};
constexpr Tenant kProductionTenant = {
    "auth.phibrowser.com", "jrFgfm2FA8DoPBzs4ysOePZKwFCcDbo4",
    "https://phibrowser.us.auth0.com/api/v2/"};
// Every Windows ring signs in against production (decided 2026-10-16): the
// canary ring used to follow the Mac Nightly onto staging, but Windows
// testers hold production accounts and their data lives there. The staging
// tenant stays reachable through the dev-hooks overrides below.
constexpr const Tenant& kTenant = kProductionTenant;

// The Mac WebAuth's scope: offline_access buys the refresh token.
constexpr char kScope[] = "openid profile email offline_access";
// The bundle id whose URL scheme the Mac app registers — Auth0.swift builds
// its default callback from it, and that callback is what each tenant's
// application lists: the Nightly (canary) bundle id on staging, the release
// bundle id on production (box-probed 2026-09-09: staging refuses the
// release id). The pairing follows the tenant, so a dev-hooks override of
// the domain picks the matching id too.
constexpr char kStagingBundleId[] = "com.phibrowser.canary.Mac";
constexpr char kProductionBundleId[] = "com.phibrowser.Mac";

// A dev-hooks build may point every tenant value elsewhere; a release
// build compiles the lookups out (phi_features.h).
std::string EnvOr(const char* name, const char* fallback) {
  if constexpr (kPhiDevHooks) {
    char value[512] = {};
    const DWORD length = ::GetEnvironmentVariableA(name, value, sizeof(value));
    if (length > 0 && length < sizeof(value)) {
      return std::string(value, length);
    }
  }
  return fallback;
}

std::string Domain() {
  return EnvOr("PHI_AUTH0_DOMAIN", kTenant.domain);
}
std::string ClientId() {
  return EnvOr("PHI_AUTH0_CLIENT_ID", kTenant.client_id);
}
std::string Audience() {
  return EnvOr("PHI_AUTH0_AUDIENCE", kTenant.audience);
}
std::string RedirectUri() {
  const std::string domain = Domain();
  const bool staging = domain.find(".stag.") != std::string::npos;
  return PhiMacCallbackUri(staging ? kStagingBundleId : kProductionBundleId,
                           domain);
}

bool RandomBytes(uint8_t* out, size_t length) {
  return BCryptGenRandom(nullptr, out, static_cast<ULONG>(length),
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::string RandomBase64Url(size_t bytes) {
  std::vector<uint8_t> random(bytes);
  if (!RandomBytes(random.data(), random.size())) {
    return std::string();
  }
  return PhiBase64UrlEncode(std::string_view(
      reinterpret_cast<const char*>(random.data()), random.size()));
}

// --- DPAPI store (the PhiCredentials.dat pattern) ---------------------------

std::optional<std::string> DpapiDecrypt(const std::string& blob) {
  DATA_BLOB in = {static_cast<DWORD>(blob.size()),
                  reinterpret_cast<BYTE*>(const_cast<char*>(blob.data()))};
  DATA_BLOB out = {};
  if (!::CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0,
                            &out)) {
    return std::nullopt;
  }
  std::string result(reinterpret_cast<char*>(out.pbData), out.cbData);
  ::LocalFree(out.pbData);
  return result;
}

std::optional<std::string> DpapiEncrypt(const std::string& plain) {
  DATA_BLOB in = {static_cast<DWORD>(plain.size()),
                  reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
  DATA_BLOB out = {};
  if (!::CryptProtectData(&in, L"PhiAuth", nullptr, nullptr, nullptr, 0,
                          &out)) {
    return std::nullopt;
  }
  std::string result(reinterpret_cast<char*>(out.pbData), out.cbData);
  ::LocalFree(out.pbData);
  return result;
}

// The session file: the refresh token, the reauthentication flag and the
// identity, DPAPI-sealed and written atomically. False when it did not land.
bool WriteSession(const std::wstring& path,
                  const std::string& refresh_token,
                  bool reauth_required,
                  const PhiAuthUser& session_user) {
  base::DictValue root;
  root.Set("refreshToken", refresh_token);
  root.Set("reauthRequired", reauth_required);
  base::DictValue user;
  user.Set("sub", session_user.sub);
  user.Set("name", session_user.name);
  user.Set("email", session_user.email);
  user.Set("picture", session_user.picture);
  root.Set("user", std::move(user));
  std::string json;
  base::JSONWriter::Write(root, &json);
  const std::optional<std::string> blob = DpapiEncrypt(json);
  return blob && PhiWriteFileAtomic(path, *blob);
}

// --- The tenant's token endpoint -----------------------------------------

// POSTs `body` (form-encoded) to https://<domain>/oauth/token. False on
// transport failure (HTTP error bodies still return true — the caller reads
// the JSON's `error`).
bool PostToken(const std::string& body, std::string* response) {
  const PhiHttpResult result = PhiHttpRequest(
      L"POST", "https://" + Domain() + "/oauth/token",
      L"Content-Type: application/x-www-form-urlencoded\r\n", body);
  if (!result.transport_ok) {
    return false;
  }
  *response = result.body;
  return true;
}

// Parses the id_token's claim segment (payload only — the token arrived
// over the code exchange's TLS channel directly from the issuer, the same
// trust the Mac's SDK places in it).
std::optional<PhiAuthUser> UserFromIdToken(const std::string& id_token) {
  const std::optional<std::string> payload = PhiJwtPayload(id_token);
  if (!payload) {
    return std::nullopt;
  }
  std::optional<base::DictValue> claims =
      base::JSONReader::ReadDict(*payload, base::JSON_PARSE_RFC);
  if (!claims) {
    return std::nullopt;
  }
  PhiAuthUser user;
  if (const std::string* sub = claims->FindString("sub")) {
    user.sub = *sub;
  }
  if (user.sub.empty()) {
    return std::nullopt;  // The Mac refuses accounts without a subject too.
  }
  if (const std::string* name = claims->FindString("name")) {
    user.name = *name;
  }
  if (const std::string* email = claims->FindString("email")) {
    user.email = *email;
  }
  if (const std::string* picture = claims->FindString("picture")) {
    user.picture = *picture;
  }
  return user;
}

// PhiAuth's message-only window: one private message, lparam = Outcome*.
constexpr wchar_t kOutcomeClassName[] = L"PhiAuthOutcome";
constexpr UINT kOutcomeMsg = WM_APP + 1;

// The refresh token to revoke, for the fire-and-forget thread.
struct RevokeJob {
  std::string refresh_token;
};

}  // namespace

// The code exchange's cross-thread bag, owned by the worker after start.
struct PhiAuth::ExchangeJob {
  // The owning PhiAuth outlives the worker by design.
  RAW_PTR_EXCLUSION PhiAuth* auth = nullptr;
  std::string code;
  std::string verifier;
  uint32_t attempt = 0;
};

// What a worker computed, for the UI thread to apply. Tokens and identity
// travel here rather than being written from the worker, so the session has
// exactly one writer.
struct PhiAuth::Outcome {
  enum class Kind {
    kSignedIn,      // Code exchange succeeded: user + tokens are filled.
    kSignInFailed,  // Abandoned, refused or incomplete; nothing changes.
    kRefreshed,     // Refresh succeeded: tokens (and maybe user) are filled.
    kSessionDead,   // invalid_grant: the session needs reauthentication.
  };
  Kind kind = Kind::kSignInFailed;
  // The session the worker was working from; ApplyOutcome drops a stale one.
  uint32_t generation = 0;
  // kSignedIn / kSignInFailed: the sign-in attempt the exchange served; a
  // cancelled attempt's result is dropped.
  uint32_t attempt = 0;
  std::optional<PhiAuthUser> user;
  std::string refresh_token;
  std::string access_token;
  // kSignInFailed: "refused" or "incomplete" (see PhiAuth::last_error).
  std::string error;
};

PhiAuth::PhiAuth() {
  ::InitializeCriticalSection(&token_lock_);
  ::InitializeCriticalSection(&refresh_lock_);
}

PhiAuth::~PhiAuth() {
  if (outcome_hwnd_) {
    ::SetWindowLongPtrW(outcome_hwnd_, GWLP_USERDATA, 0);
    ::DestroyWindow(outcome_hwnd_);
  }
  ::DeleteCriticalSection(&refresh_lock_);
  ::DeleteCriticalSection(&token_lock_);
}

std::string PhiAuth::AccessTokenBlocking() {
  std::string token;
  ::EnterCriticalSection(&token_lock_);
  const bool signed_in = worker_signed_in_;
  token = worker_access_token_;
  ::LeaveCriticalSection(&token_lock_);
  if (!signed_in) {
    return std::string();
  }
  if (!token.empty()) {
    return token;
  }
  ::EnterCriticalSection(&refresh_lock_);
  // Another worker may have refreshed while we waited for the lock.
  ::EnterCriticalSection(&token_lock_);
  token = worker_access_token_;
  ::LeaveCriticalSection(&token_lock_);
  if (token.empty()) {
    RefreshSession();
    ::EnterCriticalSection(&token_lock_);
    token = worker_access_token_;
    ::LeaveCriticalSection(&token_lock_);
  }
  ::LeaveCriticalSection(&refresh_lock_);
  return token;
}

namespace {

// Seconds until a JWT's `exp`, or nullopt when it carries none.
std::optional<int64_t> JwtSecondsLeft(const std::string& jwt) {
  const std::optional<std::string> payload = PhiJwtPayload(jwt);
  if (!payload) {
    return std::nullopt;
  }
  std::optional<base::DictValue> claims =
      base::JSONReader::ReadDict(*payload, base::JSON_PARSE_RFC);
  if (!claims) {
    return std::nullopt;
  }
  const std::optional<double> exp = claims->FindDouble("exp");
  if (!exp) {
    return std::nullopt;
  }
  return static_cast<int64_t>(*exp) -
         static_cast<int64_t>(base::Time::Now().InSecondsFSinceUnixEpoch());
}

}  // namespace

std::string PhiAuth::AccessTokenForSync() {
  if (!signed_in_ || reauth_required_) {
    return std::string();
  }
  ::EnterCriticalSection(&token_lock_);
  const std::string token = worker_access_token_;
  ::LeaveCriticalSection(&token_lock_);
  if (!token.empty()) {
    const std::optional<int64_t> left = JwtSecondsLeft(token);
    if (!left || *left > 60) {
      return token;
    }
  }
  bool expected = false;
  if (sync_refresh_in_flight_.compare_exchange_strong(expected, true)) {
    HANDLE thread = ::CreateThread(nullptr, 0, &PhiAuth::SyncRefreshThread,
                                   this, 0, nullptr);
    if (thread) {
      ::CloseHandle(thread);
    } else {
      sync_refresh_in_flight_ = false;
    }
  }
  return std::string();
}

// static
DWORD WINAPI PhiAuth::SyncRefreshThread(void* param) {
  auto* auth = static_cast<PhiAuth*>(param);
  // The cached token is stale (or absent): drop it so the blocking fetch
  // refreshes. The refresh's Outcome posts the notify message on success.
  auth->InvalidateAccessToken();
  auth->AccessTokenBlocking();
  auth->sync_refresh_in_flight_ = false;
  return 0;
}

// static
std::string PhiAuth::SyncServerUrl() {
  const std::string override = EnvOr("PHI_SYNC_URL", "");
  if (!override.empty()) {
    return override;
  }
  return SyncBaseUrl() + "/chromium-sync";
}

// static
std::string PhiAuth::SyncBaseUrl() {
  const std::string override = EnvOr("PHI_SYNC_BASE_URL", "");
  if (!override.empty()) {
    return override;
  }
  return Domain().find(".stag.") != std::string::npos
             ? "https://sync.stag.phibrowser.com"
             : "https://sync.phibrowser.com";
}

std::string PhiAuth::AccessTokenBlockingFor(const std::string& subject) {
  std::string token = AccessTokenBlocking();
  std::optional<int64_t> left = JwtSecondsLeft(token);
  if (!token.empty() && left && *left <= 60) {
    // About to lapse mid-flow: renew now rather than eat a 401.
    InvalidateAccessToken();
    token = AccessTokenBlocking();
  }
  const std::optional<std::string> payload = PhiJwtPayload(token);
  std::optional<base::DictValue> claims =
      payload ? base::JSONReader::ReadDict(*payload, base::JSON_PARSE_RFC)
              : std::nullopt;
  const std::string* sub = claims ? claims->FindString("sub") : nullptr;
  if (!sub || subject.empty() || *sub != subject) {
    return std::string();
  }
  return token;
}

void PhiAuth::InvalidateAccessToken() {
  ::EnterCriticalSection(&token_lock_);
  worker_access_token_.clear();
  ::LeaveCriticalSection(&token_lock_);
}

// static
std::string PhiAuth::AccountBaseUrl() {
  const std::string override = EnvOr("PHI_ACCOUNT_BASE_URL", "");
  if (!override.empty()) {
    return override;
  }
  return PhiAccountBaseUrlForDomain(Domain());
}

std::string PhiAuth::LogoutUrl() const {
  // The Mac's clearSession: returnTo is the callback URL, which the
  // application's logout allow-list carries for the same reason.
  return "https://" + Domain() + "/v2/logout?client_id=" +
         PhiUrlEncode(ClientId()) + "&returnTo=" + PhiUrlEncode(RedirectUri());
}

std::string PhiAuth::TenantOrigin() const {
  return "https://" + Domain() + "/";
}

void PhiAuth::Load(const std::wstring& user_data_dir,
                   HWND notify_hwnd,
                   UINT notify_msg) {
  store_path_ = user_data_dir + L"\\PhiAuth.dat";
  notify_hwnd_ = notify_hwnd;
  notify_msg_ = notify_msg;

  if (!outcome_hwnd_) {
    static const bool registered = [] {
      WNDCLASSW cls = {};
      cls.lpfnWndProc = &PhiAuth::OutcomeWndProc;
      cls.hInstance = ::GetModuleHandleW(nullptr);
      cls.lpszClassName = kOutcomeClassName;
      return ::RegisterClassW(&cls) != 0;
    }();
    if (registered) {
      outcome_hwnd_ = ::CreateWindowExW(0, kOutcomeClassName, L"", 0, 0, 0, 0,
                                        0, HWND_MESSAGE, nullptr,
                                        ::GetModuleHandleW(nullptr), nullptr);
    }
    if (outcome_hwnd_) {
      ::SetWindowLongPtrW(outcome_hwnd_, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(this));
    } else {
      LOG(ERROR) << "[PhiAuth] no outcome window; sign-in disabled";
      return;
    }
  }

  std::string blob;
  if (!PhiReadFileBytes(store_path_, &blob) || blob.empty()) {
    return;
  }
  const std::optional<std::string> plain = DpapiDecrypt(blob);
  if (!plain) {
    LOG(ERROR) << "[PhiAuth] stored session unreadable; ignoring";
    return;
  }
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(*plain, base::JSON_PARSE_RFC);
  if (!root) {
    return;
  }
  if (const std::string* token = root->FindString("refreshToken")) {
    refresh_token_ = *token;
  }
  if (const base::DictValue* user = root->FindDict("user")) {
    const auto read = [&](const char* key) {
      const std::string* value = user->FindString(key);
      return value ? *value : std::string();
    };
    user_.sub = read("sub");
    user_.name = read("name");
    user_.email = read("email");
    user_.picture = read("picture");
  }
  const bool reauth = root->FindBool("reauthRequired").value_or(false);
  if (user_.sub.empty() || (refresh_token_.empty() && !reauth)) {
    refresh_token_.clear();
    user_ = PhiAuthUser();
    return;
  }
  signed_in_ = true;
  reauth_required_ = reauth;
  ++generation_;
  ::EnterCriticalSection(&token_lock_);
  worker_generation_ = generation_;
  worker_signed_in_ = !reauth;
  worker_refresh_token_ = reauth ? std::string() : refresh_token_;
  worker_access_token_.clear();
  worker_user_ = user_;
  ::LeaveCriticalSection(&token_lock_);
  LOG(INFO) << "[PhiAuth] session restored for " << PhiRedactId(user_.sub)
            << (reauth ? " (reauthentication required)" : "");
  if (reauth) {
    return;  // Nothing to validate: the tenant already said no.
  }
  // Validate in the background (the Mac's refreshAuthStatus at launch): a
  // revoked session enters the reauthentication state and notifies.
  HANDLE thread =
      ::CreateThread(nullptr, 0, &PhiAuth::RefreshThread, this, 0, nullptr);
  if (thread) {
    ::CloseHandle(thread);
  }
}

void PhiAuth::BeginSignIn(std::function<void(const std::string&)> open_url) {
  if (signing_in_ || !outcome_hwnd_) {
    return;
  }
  // PKCE material: a 32-byte verifier and its S256 challenge, and the state
  // that ties the callback to this attempt.
  const std::string verifier = RandomBase64Url(32);
  const std::string state = RandomBase64Url(16);
  if (verifier.empty() || state.empty()) {
    return;
  }
  const std::string url =
      "https://" + Domain() + "/authorize?response_type=code&client_id=" +
      PhiUrlEncode(ClientId()) + "&redirect_uri=" + PhiUrlEncode(RedirectUri()) +
      "&scope=" + PhiUrlEncode(kScope) + "&audience=" +
      PhiUrlEncode(Audience()) + "&code_challenge=" +
      PhiPkceChallenge(verifier) + "&code_challenge_method=S256&state=" +
      state;
  signing_in_ = true;
  last_error_.clear();
  pending_verifier_ = verifier;
  pending_state_ = state;
  ++attempt_;
  LOG(INFO) << "[PhiAuth] sign-in started";
  open_url(url);
}

void PhiAuth::CancelSignIn() {
  if (!signing_in_) {
    return;
  }
  LOG(INFO) << "[PhiAuth] sign-in cancelled";
  signing_in_ = false;
  pending_verifier_.clear();
  pending_state_.clear();
  ++attempt_;  // An exchange already running lands on a dead attempt.
  last_error_ = "incomplete";
  NotifyChanged();
}

bool PhiAuth::HandleCallbackUrl(const std::string& url) {
  const std::optional<PhiCallbackUrl> callback = PhiParseCallbackUrl(url);
  if (!callback) {
    return false;
  }
  // Ours: the scheme is the bundle id, the host the tenant, the path the
  // Mac's — the Mac checks host and state, so do we.
  const std::optional<PhiCallbackUrl> expected =
      PhiParseCallbackUrl(RedirectUri());
  if (!expected || callback->scheme != expected->scheme ||
      callback->host != expected->host || callback->path != expected->path) {
    return false;
  }
  if (!signing_in_) {
    // The logout page's return, or a stray replay: nothing to exchange.
    LOG(INFO) << "[PhiAuth] callback acknowledged (no sign-in pending)";
    NotifyChanged();
    return true;
  }
  if (callback->state != pending_state_) {
    LOG(WARNING) << "[PhiAuth] callback state mismatch; ignored";
    NotifyChanged();
    return true;
  }
  const std::string verifier = pending_verifier_;
  pending_verifier_.clear();
  pending_state_.clear();
  if (callback->code.empty()) {
    // Refused (access_denied and friends) or malformed.
    LOG(INFO) << "[PhiAuth] sign-in refused by the tenant: "
              << (callback->error.empty() ? "no code" : callback->error);
    signing_in_ = false;
    last_error_ = callback->error.empty() ? "incomplete" : "refused";
    NotifyChanged();
    return true;
  }
  auto* job = new ExchangeJob();
  job->auth = this;
  job->code = callback->code;
  job->verifier = verifier;
  job->attempt = attempt_;
  HANDLE thread =
      ::CreateThread(nullptr, 0, &PhiAuth::ExchangeThread, job, 0, nullptr);
  if (!thread) {
    delete job;
    signing_in_ = false;
    last_error_ = "incomplete";
  } else {
    ::CloseHandle(thread);
  }
  NotifyChanged();
  return true;
}

// static
DWORD WINAPI PhiAuth::ExchangeThread(void* param) {
  std::unique_ptr<ExchangeJob> job(static_cast<ExchangeJob*>(param));
  job->auth->CompleteSignIn(job->code, job->verifier, job->attempt);
  return 0;
}

void PhiAuth::CompleteSignIn(const std::string& code,
                             const std::string& verifier,
                             uint32_t attempt) {
  // Every early return is a failed sign-in; the UI thread must hear about
  // it to clear signing_in_. The attempt rides along: a cancel meanwhile
  // makes the result moot.
  auto outcome = std::make_unique<Outcome>();
  outcome->error = "incomplete";
  outcome->attempt = attempt;
  const std::string body =
      "grant_type=authorization_code&client_id=" + PhiUrlEncode(ClientId()) +
      "&code_verifier=" + verifier + "&code=" + PhiUrlEncode(code) +
      "&redirect_uri=" + PhiUrlEncode(RedirectUri());
  std::string response;
  if (!PostToken(body, &response)) {
    LOG(ERROR) << "[PhiAuth] token exchange transport failure";
    PostOutcome(std::move(outcome));
    return;
  }
  std::optional<base::DictValue> json =
      base::JSONReader::ReadDict(response, base::JSON_PARSE_RFC);
  if (!json) {
    PostOutcome(std::move(outcome));
    return;
  }
  if (const std::string* error = json->FindString("error")) {
    LOG(ERROR) << "[PhiAuth] token exchange refused: " << *error;
    outcome->error = "refused";
    PostOutcome(std::move(outcome));
    return;
  }
  const std::string* id_token = json->FindString("id_token");
  const std::string* refresh = json->FindString("refresh_token");
  const std::string* access = json->FindString("access_token");
  std::optional<PhiAuthUser> user =
      id_token ? UserFromIdToken(*id_token) : std::nullopt;
  if (!user || !refresh) {
    LOG(ERROR) << "[PhiAuth] token exchange incomplete";
    PostOutcome(std::move(outcome));
    return;
  }
  outcome->kind = Outcome::Kind::kSignedIn;
  outcome->error.clear();
  outcome->user = std::move(user);
  outcome->refresh_token = *refresh;
  outcome->access_token = access ? *access : std::string();
  PostOutcome(std::move(outcome));
}

// static
DWORD WINAPI PhiAuth::RefreshThread(void* param) {
  auto* auth = static_cast<PhiAuth*>(param);
  ::EnterCriticalSection(&auth->refresh_lock_);
  auth->RefreshSession();  // A dead session posts its own outcome.
  ::LeaveCriticalSection(&auth->refresh_lock_);
  return 0;
}

bool PhiAuth::RefreshSession() {
  ::EnterCriticalSection(&token_lock_);
  const uint32_t generation = worker_generation_;
  const bool signed_in = worker_signed_in_;
  const std::string refresh_token = worker_refresh_token_;
  ::LeaveCriticalSection(&token_lock_);
  if (!signed_in || refresh_token.empty()) {
    return false;
  }
  const std::string body =
      "grant_type=refresh_token&client_id=" + PhiUrlEncode(ClientId()) +
      "&refresh_token=" + PhiUrlEncode(refresh_token);
  std::string response;
  if (!PostToken(body, &response)) {
    // Transport failure (offline) keeps the cached session — the Mac's
    // offline launch behaves the same.
    return true;
  }
  std::optional<base::DictValue> json =
      base::JSONReader::ReadDict(response, base::JSON_PARSE_RFC);
  if (!json) {
    return true;
  }
  auto outcome = std::make_unique<Outcome>();
  outcome->generation = generation;
  if (const std::string* error = json->FindString("error")) {
    if (*error != "invalid_grant") {
      // A tenant-side hiccup (rate limit, misconfiguration) is not a dead
      // session; the Mac only reacts to the refresh token being refused.
      LOG(WARNING) << "[PhiAuth] refresh refused: " << *error;
      return true;
    }
    // The refresh token is gone: the session needs reauthentication. The
    // workers stop handing out the stale bearer now.
    ::EnterCriticalSection(&token_lock_);
    if (worker_generation_ == generation) {
      worker_signed_in_ = false;
      worker_refresh_token_.clear();
      worker_access_token_.clear();
    }
    ::LeaveCriticalSection(&token_lock_);
    outcome->kind = Outcome::Kind::kSessionDead;
    PostOutcome(std::move(outcome));
    return false;
  }
  outcome->kind = Outcome::Kind::kRefreshed;
  outcome->refresh_token = refresh_token;
  if (const std::string* access = json->FindString("access_token")) {
    outcome->access_token = *access;
  }
  if (const std::string* rotated = json->FindString("refresh_token")) {
    if (!rotated->empty()) {
      outcome->refresh_token = *rotated;  // Rotation-enabled tenants.
    }
  }
  if (const std::string* id_token = json->FindString("id_token")) {
    outcome->user = UserFromIdToken(*id_token);
  }
  // The workers' copy first: the old refresh token may already be revoked,
  // and the next round trip must not wait on the UI thread to learn that.
  // A rotated token reaches the disk BEFORE anyone uses it — an exit or a
  // crash before the UI applied the outcome must not leave the revoked one
  // stored — and a token that could not be stored is never used: the
  // session keeps the one on disk.
  ::EnterCriticalSection(&token_lock_);
  if (worker_generation_ == generation && worker_signed_in_) {
    const PhiAuthUser user = outcome->user.value_or(worker_user_);
    if (outcome->refresh_token != refresh_token || outcome->user) {
      const bool written =
          store_path_.empty() ||
          WriteSession(store_path_, outcome->refresh_token, false, user) ||
          WriteSession(store_path_, outcome->refresh_token, false, user);
      if (written) {
        worker_refresh_token_ = outcome->refresh_token;
        worker_user_ = user;
      } else {
        LOG(ERROR) << "[PhiAuth] failed to persist the rotated session; "
                      "keeping the stored refresh token";
        outcome->refresh_token = refresh_token;
      }
    }
    worker_access_token_ = outcome->access_token;
  }
  ::LeaveCriticalSection(&token_lock_);
  PostOutcome(std::move(outcome));
  return true;
}

// static
DWORD WINAPI PhiAuth::RevokeThread(void* param) {
  std::unique_ptr<RevokeJob> job(static_cast<RevokeJob*>(param));
  // https://auth0.com/docs/api/authentication#revoke-refresh-token — a
  // public client sends just the id and the token. Best effort: nobody
  // waits for the answer, and a failure leaves a token the tenant will
  // expire on its own.
  base::DictValue body;
  body.Set("client_id", ClientId());
  body.Set("token", job->refresh_token);
  std::string json;
  base::JSONWriter::Write(body, &json);
  const PhiHttpResult result =
      PhiHttpRequest(L"POST", "https://" + Domain() + "/oauth/revoke",
                     L"Content-Type: application/json\r\n", json);
  if (!result.transport_ok || result.status < 200 || result.status >= 300) {
    LOG(WARNING) << "[PhiAuth] refresh token revocation did not land"
                 << (result.transport_ok
                         ? " (HTTP " + std::to_string(result.status) + ")"
                         : " (offline)");
  }
  return 0;
}

void PhiAuth::PostOutcome(std::unique_ptr<Outcome> outcome) {
  if (outcome_hwnd_ &&
      ::PostMessageW(outcome_hwnd_, kOutcomeMsg, 0,
                     reinterpret_cast<LPARAM>(outcome.get()))) {
    outcome.release();  // The WndProc owns it now.
  }
  // Else the window is gone (shutdown): the result has nobody to apply it.
}

// static
LRESULT CALLBACK PhiAuth::OutcomeWndProc(HWND hwnd,
                                         UINT msg,
                                         WPARAM wparam,
                                         LPARAM lparam) {
  if (msg == kOutcomeMsg) {
    std::unique_ptr<Outcome> outcome(reinterpret_cast<Outcome*>(lparam));
    auto* self =
        reinterpret_cast<PhiAuth*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && outcome) {
      self->ApplyOutcome(*outcome);
    }
    return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

void PhiAuth::ApplyOutcome(const Outcome& outcome) {
  switch (outcome.kind) {
    case Outcome::Kind::kSignedIn: {
      if (outcome.attempt != attempt_) {
        return;  // Cancelled (or superseded) while the exchange ran.
      }
      signing_in_ = false;
      if (signed_in_ && outcome.user->sub != user_.sub) {
        // A reauthentication that came back as somebody else: the session
        // it was meant to restore is not this one. The Mac refuses the
        // same way (AuthenticatedAccountIdentityPolicy); the user signs
        // out first if a switch is what they want.
        LOG(WARNING) << "[PhiAuth] sign-in returned another subject; refused";
        last_error_ = "account_mismatch";
        break;
      }
      signed_in_ = true;
      reauth_required_ = false;
      last_error_.clear();
      user_ = *outcome.user;
      refresh_token_ = outcome.refresh_token;
      ++generation_;
      ::EnterCriticalSection(&token_lock_);
      worker_generation_ = generation_;
      worker_signed_in_ = true;
      worker_refresh_token_ = refresh_token_;
      worker_access_token_ = outcome.access_token;
      worker_user_ = user_;
      ::LeaveCriticalSection(&token_lock_);
      Persist();
      LOG(INFO) << "[PhiAuth] signed in as " << PhiRedactId(user_.sub);
      break;
    }
    case Outcome::Kind::kSignInFailed:
      if (outcome.attempt != attempt_) {
        return;
      }
      signing_in_ = false;
      last_error_ = outcome.error;
      break;
    case Outcome::Kind::kRefreshed: {
      if (!signed_in_ || reauth_required_ ||
          outcome.generation != generation_) {
        return;  // A round trip for a session that is no longer this one.
      }
      // The worker already stored what it rotated; the session follows the
      // workers' copy (a later refresh may have rotated again since — an
      // older outcome must not write its token back over the newer one).
      ::EnterCriticalSection(&token_lock_);
      if (worker_generation_ == generation_ && worker_signed_in_) {
        refresh_token_ = worker_refresh_token_;
      }
      ::LeaveCriticalSection(&token_lock_);
      if (outcome.user) {
        user_ = *outcome.user;
      }
      break;
    }
    case Outcome::Kind::kSessionDead:
      if (!signed_in_ || reauth_required_ ||
          outcome.generation != generation_) {
        return;
      }
      LOG(INFO) << "[PhiAuth] stored session no longer valid; "
                   "reauthentication required";
      reauth_required_ = true;
      refresh_token_.clear();
      Persist();
      break;
  }
  NotifyChanged();
}

void PhiAuth::SignOut() {
  if (!refresh_token_.empty()) {
    auto* job = new RevokeJob();
    job->refresh_token = refresh_token_;
    HANDLE thread =
        ::CreateThread(nullptr, 0, &PhiAuth::RevokeThread, job, 0, nullptr);
    if (thread) {
      ::CloseHandle(thread);
    } else {
      delete job;
    }
  }
  signed_in_ = false;
  reauth_required_ = false;
  signing_in_ = false;
  pending_verifier_.clear();
  pending_state_.clear();
  ++attempt_;
  last_error_.clear();
  user_ = PhiAuthUser();
  refresh_token_.clear();
  ++generation_;
  ::EnterCriticalSection(&token_lock_);
  worker_generation_ = generation_;
  worker_signed_in_ = false;
  worker_refresh_token_.clear();
  worker_access_token_.clear();
  worker_user_ = PhiAuthUser();
  ::LeaveCriticalSection(&token_lock_);
  if (!store_path_.empty()) {
    ::DeleteFileW(store_path_.c_str());
  }
  NotifyChanged();
}

void PhiAuth::Persist() {
  // Under token_lock_: a worker storing a rotated token writes the same file.
  ::EnterCriticalSection(&token_lock_);
  const bool written =
      WriteSession(store_path_, refresh_token_, reauth_required_, user_);
  ::LeaveCriticalSection(&token_lock_);
  if (!written) {
    LOG(ERROR) << "[PhiAuth] failed to persist the session";
  }
}

void PhiAuth::NotifyChanged() {
  if (notify_hwnd_ && ::IsWindow(notify_hwnd_)) {
    ::PostMessageW(notify_hwnd_, notify_msg_, 0, 0);
  }
}

}  // namespace phi_client

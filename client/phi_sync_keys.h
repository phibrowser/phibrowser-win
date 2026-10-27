// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_KEYS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_KEYS_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_key_crypto.h"

// The sync key layer (docs/user-data-parity.md S3): the Mac's
// Sources/Sync/Keys minus its UI — AccountKeyManager (bootstrap, recovery
// join, approval join, silent unlock, parked registration),
// DeviceApprovalService, ProfileKeyManager, and SyncKeyController's
// mapping pass and pairing gate — as plain C++ over three seams:
//
//   KeyApi          /keys/v1, BLOCKING (phi_key_api_win.cc: WinHTTP + JSON)
//   DeviceKeyStore  this device's X25519 key (phi_key_api_win.cc: DPAPI)
//   KeyStateStore   the account's persisted key-layer state: the profile
//                   mapping, the pairing enrollment, a parked registration
//
// Host-clean (STL + BoringSSL) and SYNCHRONOUS: the Windows client runs
// every call on ONE key worker thread per account (PhiSyncKeyService),
// which is what gives the Mac's single-flight rules (one resolve pass at a
// time, join requests serialized) for free. The only state read from
// another thread is the ARK, behind its own lock.
namespace phi_client::keys {

// --- Errors ------------------------------------------------------------------

enum class KeyErrorKind {
  kOk,
  // No bearer token: signed out, another account now, or a renewal in
  // flight. Never sent as an empty bearer (the server answers 401, which
  // callers read as a sign-out).
  kNoToken,
  kTransport,  // offline, DNS, TLS
  kHttp,       // an unexpected status; http_status + detail (the body)
  kDecode,     // a response that does not parse
  // Mapped statuses (KeyEnvelopeAPIClient's own mapping). Refusals that
  // change what the client does next are matched on the error body's
  // `code`, never on the status alone.
  kLastActiveDevice,  // revoke: 409 last_device
  // The account reset's two refusals; neither deleted anything server-side.
  kNotLastDevice,     // reset: 409 not_last_device
  kDeviceNotFound,    // reset: 404 device_not_found
  kTooManyPending,    // join: 429
  kNotPending,        // join: 409
  kNotFound,          // join: 404
  kInvalidRequest,    // join: 400
  // The managers' own refusals.
  kAlreadyInitialized,
  kBadRecoveryCode,
  kNotInitialized,
  kNotUnlocked,
  kBadEnvelope,
  kAlreadyMapped,
  // The default-profile rule (docs/sync.md on the Mac, "Profile pairing and
  // the default profile"): a device's Default never takes a profile another
  // device registered as non-default. Refused at the mapping write.
  kDefaultMismatch,
  kMetadataConflict,  // envelope replace: three 409s in a row
  kCrypto,
  kDeviceKey,
  kStorage,
  kCancelled,
};

struct KeyStatus {
  KeyErrorKind kind = KeyErrorKind::kOk;
  int http_status = 0;
  std::string detail;

  bool ok() const { return kind == KeyErrorKind::kOk; }
  static KeyStatus Ok() { return KeyStatus(); }
  static KeyStatus Error(KeyErrorKind kind, std::string detail = std::string());
  static KeyStatus Http(int status, std::string body);
};

// "no_token", "http_409", "bad_recovery_code", … for logs and the verbs.
std::string KeyStatusName(const KeyStatus& status);

// --- /keys/v1 ----------------------------------------------------------------
// Binary fields are standard base64 on the wire; times are RFC 3339, carried
// here as Unix milliseconds. Responses are NOT the shared {code,message,data}
// envelope.

struct AccountKeyState {
  Bytes recovery_salt;
  std::string kdf_version;
  std::string kdf_params_json;  // passed through untouched
  Bytes recovery_ark_envelope;
  int ark_generation = 0;
};

struct AccountDevice {
  std::string device_key_id;
  std::string name;
  std::string platform;
  std::string status;  // "active" / "revoked"
  int64_t created_at_ms = 0;  // creation, never "last seen"
  std::optional<int64_t> revoked_at_ms;
};

struct JoinRequestSummary {
  std::string request_id;
  Bytes requesting_public_key;
  std::string name;
  std::string platform;
  std::string status;  // "pending" / "approved" / "denied" / "expired"
  int64_t created_at_ms = 0;
};

struct JoinRequest : JoinRequestSummary {
  Bytes granted_ark_envelope;  // empty until approved
  std::string resolved_by_device_key_id;
};

struct ProfileSummary {
  std::string profile_uuid;
  bool has_envelope = false;
  int64_t created_at_ms = 0;
};

class KeyApi {
 public:
  virtual ~KeyApi() = default;

  virtual KeyStatus ListDevices(std::vector<AccountDevice>* out) = 0;
  // `created` false: the account key state already existed (409).
  virtual KeyStatus PutAccount(const Bytes& salt,
                               const std::string& kdf_version,
                               const std::string& kdf_params_json,
                               const Bytes& recovery_envelope,
                               bool* created) = 0;
  // nullopt: the account was never bootstrapped (404).
  virtual KeyStatus GetAccount(std::optional<AccountKeyState>* out) = 0;
  virtual KeyStatus PostDevice(const std::string& device_key_id,
                               const Bytes& public_key,
                               const std::string& name,
                               const std::string& platform,
                               const Bytes& ark_envelope) = 0;
  // nullopt: this device has no envelope (404).
  virtual KeyStatus GetDeviceEnvelope(const std::string& device_key_id,
                                      std::optional<Bytes>* out) = 0;
  virtual KeyStatus RevokeDevice(const std::string& device_key_id) = 0;
  // PUT /keys/v1/reset-operations/{operation_id}: the account's last active
  // device deletes every server-side sync row of the account (keys
  // included). Idempotent by operation id with no receipt: a retry after a
  // lost success answers `reset` again. kNotLastDevice / kDeviceNotFound
  // deleted nothing.
  virtual KeyStatus ResetAccount(const std::string& operation_id,
                                 const std::string& device_key_id) = 0;
  virtual KeyStatus PostJoinRequest(const Bytes& public_key,
                                    const std::string& name,
                                    const std::string& platform,
                                    std::string* request_id) = 0;
  virtual KeyStatus ListPendingJoinRequests(
      std::vector<JoinRequestSummary>* out) = 0;
  virtual KeyStatus GetJoinRequest(const std::string& request_id,
                                   JoinRequest* out) = 0;
  virtual KeyStatus ApproveJoinRequest(const std::string& request_id,
                                       const Bytes& granted_ark_envelope,
                                       const std::string& resolved_by) = 0;
  virtual KeyStatus DenyJoinRequest(const std::string& request_id) = 0;
  virtual KeyStatus ListProfiles(std::vector<ProfileSummary>* out) = 0;
  // nullopt: no key under this uuid (404).
  virtual KeyStatus GetProfileKey(const std::string& uuid,
                                  std::optional<Bytes>* envelope) = 0;
  // `created` false: an envelope already exists (409) — first writer wins.
  virtual KeyStatus PutProfileKey(const std::string& uuid,
                                  const Bytes& envelope,
                                  bool* created) = 0;
  // PATCH: replaces the envelope only while the server still holds
  // `expected` byte for byte. `replaced` false: it changed underneath (409).
  virtual KeyStatus ReplaceProfileKeyEnvelope(const std::string& uuid,
                                              const Bytes& expected,
                                              const Bytes& envelope,
                                              bool* replaced) = 0;
  virtual KeyStatus GetDomainKey(const std::string& domain,
                                 std::optional<Bytes>* envelope) = 0;
  virtual KeyStatus PutDomainKey(const std::string& domain,
                                 const Bytes& envelope,
                                 bool* created) = 0;
};

// --- Local persistence -------------------------------------------------------

class DeviceKeyStore {
 public:
  virtual ~DeviceKeyStore() = default;
  // This device's key for the account, created on first use.
  virtual KeyStatus LoadOrCreate(DeviceKeyPair* out) = 0;
  // A fresh identity IN PLACE (never delete-then-create): the server never
  // un-revokes a fingerprint, so a revoked one must be replaced.
  virtual KeyStatus Rotate() = 0;
};

// The Mac's SyncPairingRecord, persisted as sync.pairingEnrollment.
struct EnrollmentRecord {
  int version = 1;
  std::string device_key_id;
  bool paired = false;
};
bool IsPaired(const std::optional<EnrollmentRecord>& record,
              const std::string& device_key_id);

class KeyStateStore {
 public:
  virtual ~KeyStateStore() = default;
  // sync.profileGlobalUuids: local profile id → account profile uuid.
  virtual std::map<std::string, std::string> ProfileMappings() const = 0;
  virtual bool SetProfileMappings(
      const std::map<std::string, std::string>& mappings) = 0;
  // phi.sync.pendingDeviceRegistration.<device key id>: the ARK sealed to
  // this device's own key, parked when bootstrap's registration failed.
  virtual std::optional<Bytes> PendingRegistration(
      const std::string& device_key_id) const = 0;
  virtual void SetPendingRegistration(const std::string& device_key_id,
                                      const Bytes& envelope) = 0;
  virtual void ClearPendingRegistration(const std::string& device_key_id) = 0;
  virtual std::optional<EnrollmentRecord> Enrollment() const = 0;
  virtual bool SetEnrollment(const EnrollmentRecord& record) = 0;
};

// The profile key envelope's plaintext,
// {"v":1,"key":<base64>,"name":…,"default":<bool>}. "default" is additive:
// every registration writes it (true only for the registering device's
// Default profile); an envelope without it was sealed before the flag
// existed and reads as UNKNOWN (nullopt), never as false. `is_default`
// nullopt on Encode writes no key — the shape of the Phi domain-key
// envelope, which is not a profile.
class ProfilePayloadCodec {
 public:
  virtual ~ProfilePayloadCodec() = default;
  virtual std::string Encode(const Bytes& key,
                             const std::string& name,
                             std::optional<bool> is_default) const = 0;
  // False unless v == 1 and key is valid base64; a missing name is "".
  virtual bool Decode(const std::string& json,
                      Bytes* key,
                      std::string* name,
                      std::optional<bool>* is_default) const = 0;
  // `json` with its "default" set to `is_default`, every other field kept
  // (the backfill keeps unknown fields). Empty when `json` is not an object.
  virtual std::string WithDefaultFlag(const std::string& json,
                                      bool is_default) const = 0;
};

// Lowercase random (v4) UUID, the Mac's UUID().uuidString.lowercased().
std::string NewLowercaseUuid();

// --- AccountKeyManager ---------------------------------------------------------

enum class UnlockResult { kUnlocked, kNeedsJoin, kNotSignedIn };

struct JoinTicket {
  std::string request_id;
  std::string verification_code;
};

enum class JoinPollState { kPending, kApproved, kDenied, kExpired };
struct JoinPoll {
  JoinPollState state = JoinPollState::kPending;
  int64_t deadline_ms = 0;  // pending: created_at + 15 min
};

inline constexpr int64_t kJoinRequestLifetimeMs = 15 * 60 * 1000;

// What this device calls itself on the account (POST devices, join
// requests): the Mac sends Host.current().localizedName and "macos".
struct DeviceIdentity {
  std::string name;
  std::string platform;
};

class AccountKeyManager {
 public:
  AccountKeyManager(KeyApi& api,
                    DeviceKeyStore& device_keys,
                    KeyStateStore& state,
                    DeviceIdentity identity);
  ~AccountKeyManager();

  AccountKeyManager(const AccountKeyManager&) = delete;
  AccountKeyManager& operator=(const AccountKeyManager&) = delete;

  // First device: mint the ARK and a recovery code, PUT the account, then
  // register this device. The code comes back even when the registration
  // fails — that one is parked and finished by UnlockAtStartup.
  KeyStatus Bootstrap(std::string* recovery_code_display);
  KeyStatus JoinWithRecoveryCode(std::string_view code);
  // Silent: this device's envelope → the ARK. 401 = kNotSignedIn.
  KeyStatus UnlockAtStartup(UnlockResult* result);
  KeyStatus AccountExists(bool* exists);

  // A new device asks an authorized one to admit it. Withdraws this
  // device's own stale pending requests (matched by the full public key)
  // first.
  KeyStatus RequestJoinApproval(JoinTicket* ticket);
  // The requester withdraws its ticket (deny); terminal/missing is fine.
  KeyStatus CancelJoinApproval(const std::string& request_id);
  // On approval: open the granted envelope, register, unlock. `cancelled`
  // is consulted after each network step so a withdrawn flow never
  // consumes a late approval.
  KeyStatus PollJoin(const std::string& request_id,
                     const std::function<bool()>& cancelled,
                     JoinPoll* poll);

  // The ARK, when this device is unlocked. Any thread.
  std::optional<Bytes> current_ark() const;
  // Self-revoke: drop it for good (announces nothing).
  void DiscardArk();
  // Reconfiguration: drop the ARK and any parked registration.
  KeyStatus DiscardLocalRegistration();

  KeyStatus DeviceKeyId(std::string* id);
  KeyStatus DevicePublicKey(Bytes* public_key);
  // Self-revoke: a fresh device identity in place.
  KeyStatus RotateDeviceKey();
  const DeviceIdentity& identity() const { return identity_; }

  // Called (on the calling thread) the moment an ARK is cached where none
  // was — every unlock path: silent, bootstrap, approval, recovery code.
  void set_on_unlocked(std::function<void()> callback) {
    on_unlocked_ = std::move(callback);
  }

 private:
  KeyStatus RegisterThisDevice(const Bytes& ark);
  KeyStatus PostRegistration(const Bytes& ark);
  void ParkRegistration(const Bytes& ark);
  void SetArk(const Bytes& ark);

  RAW_PTR_EXCLUSION KeyApi& api_;
  RAW_PTR_EXCLUSION DeviceKeyStore& device_keys_;
  RAW_PTR_EXCLUSION KeyStateStore& state_;
  const DeviceIdentity identity_;
  mutable std::mutex ark_lock_;
  std::optional<Bytes> ark_;
  std::function<void()> on_unlocked_;
};

// --- DeviceApprovalService ---------------------------------------------------

struct PendingApproval {
  std::string id;
  std::string name;
  std::string platform;
  std::string verification_code;
  int64_t deadline_ms = 0;
  Bytes requesting_public_key;
};

class DeviceApprovalService {
 public:
  DeviceApprovalService(KeyApi& api, AccountKeyManager& manager);
  ~DeviceApprovalService();

  KeyStatus ListDevices(std::vector<AccountDevice>* out);
  KeyStatus ListPendingApprovals(std::vector<PendingApproval>* out);
  // Seals the in-memory ARK to the requester's public key.
  KeyStatus Approve(const PendingApproval& approval);
  KeyStatus Deny(const std::string& request_id);

 private:
  RAW_PTR_EXCLUSION KeyApi& api_;
  RAW_PTR_EXCLUSION AccountKeyManager& manager_;
};

// --- ProfileKeyManager ---------------------------------------------------------

struct ProfileKeyRecord {
  std::string uuid;
  std::string passphrase;  // the 32-byte key as 64 lowercase hex chars
  std::string name;
  std::optional<bool> is_default;  // the envelope's flag; nullopt = legacy
};

struct RemoteProfile {
  std::string uuid;
  std::optional<std::string> name;  // nullopt: the envelope does not open
  // True for the account's default profile, false for any other, nullopt
  // when unknown: a legacy envelope, or one that does not open.
  std::optional<bool> is_default;

  // The default-profile rule for one pairing. Exactly one account default
  // is authoritative on this device: the one its Default is live-mapped to,
  // or, while the Default is undecided, the one chosen for it. Every other
  // account default is demoted to an ordinary profile (two concurrently
  // paired devices can create that). A local Default may take any account
  // default or a legacy profile, never a known non-default; any other local
  // may take anything but the authoritative default.
  bool MayPair(bool local_is_default,
               const std::optional<std::string>& authoritative_default_uuid)
      const {
    if (local_is_default) {
      return is_default != false;
    }
    return !(is_default == true && authoritative_default_uuid &&
             uuid == *authoritative_default_uuid);
  }
};

class ProfileKeyManager {
 public:
  ProfileKeyManager(KeyApi& api,
                    AccountKeyManager& manager,
                    KeyStateStore& state,
                    const ProfilePayloadCodec& codec);
  ~ProfileKeyManager();

  std::optional<std::string> MappedGlobalUuid(const std::string& profile_id) const;
  // Mint a uuid + key for a local profile; a 409 adopts the winner.
  // kAlreadyMapped when the profile still has a live server record.
  // `is_default`: the local is this device's Default, so its envelope
  // registers as the account's default profile.
  KeyStatus RegisterLocalProfile(const std::string& profile_id,
                                 const std::string& display_name,
                                 bool is_default,
                                 ProfileKeyRecord* out);
  // `is_default`: the local is this device's Default; a known non-default
  // envelope is refused for it with kDefaultMismatch before any mapping is
  // written. A legacy envelope (no flag) is accepted either way.
  KeyStatus AdoptRemoteProfile(const std::string& uuid,
                               const std::string& profile_id,
                               bool is_default,
                               ProfileKeyRecord* out);
  // Best-effort upgrade of a mapped legacy envelope with the "default" flag
  // so OTHER devices' pairing can tell this device's Default from its other
  // profiles. True is monotonic (false may become true, never the reverse).
  // Keeps the whole payload. `confirmed` true once the envelope is known to
  // carry a flag this device agrees with (already there, or written now);
  // false with Ok() when there is nothing to do (no mapping, envelope gone).
  // kMetadataConflict after three 409s; the next pass retries.
  KeyStatus BackfillDefaultFlag(const std::string& profile_id,
                                bool is_default,
                                bool* confirmed);
  // nullopt: unmapped, or the mapping's remote is gone (404).
  KeyStatus ResolvedRecord(const std::string& profile_id,
                           std::optional<ProfileKeyRecord>* out);
  // Every account profile, names decrypted where possible, listing order.
  KeyStatus AccountProfiles(std::vector<RemoteProfile>* out);
  KeyStatus AccountProfileUuids(std::set<std::string>* out);
  KeyStatus Remote(const std::string& uuid, RemoteProfile* out);
  // The smallest local id mapped to `uuid` (two locals on one uuid is not
  // meant to happen; every device then picks the same one).
  std::optional<std::string> LocalProfileIdFor(const std::string& uuid) const;
  void RemoveMapping(const std::string& profile_id);
  bool RemoveAllMappings();
  std::map<std::string, std::string> AllMappings() const;
  KeyStatus RevokeDevice(const std::string& device_key_id);
  // Account reset: THIS device, the last active one, deletes the account's
  // server-side sync data. kNotLastDevice / kDeviceNotFound: the server
  // refused and deleted nothing.
  KeyStatus ResetAccount(const std::string& operation_id,
                         const std::string& device_key_id);

  static std::optional<Bytes> SealPayload(const ProfilePayloadCodec& codec,
                                          const Bytes& key,
                                          const std::string& name,
                                          std::optional<bool> is_default,
                                          const Bytes& ark);
  static bool OpenPayload(const ProfilePayloadCodec& codec,
                          const Bytes& envelope,
                          const Bytes& ark,
                          Bytes* key,
                          std::string* name,
                          std::optional<bool>* is_default = nullptr);

 private:
  bool SetMapping(const std::string& profile_id, const std::string& uuid);

  RAW_PTR_EXCLUSION KeyApi& api_;
  RAW_PTR_EXCLUSION AccountKeyManager& manager_;
  RAW_PTR_EXCLUSION KeyStateStore& state_;
  RAW_PTR_EXCLUSION const ProfilePayloadCodec& codec_;
};

// --- PhiDomainKeyManager ---------------------------------------------------------
// The account-wide Phi domain key (native sync, S5): one 32-byte AES-GCM key
// under the fixed domain "phi", escrowed sealed under the ARK in the profile
// payload codec (name = the domain). GET; if absent mint + PUT; a lost
// first-use race (409) re-GETs and adopts the winner. Cached against a
// fingerprint of the ARK it came from, so an account switch never serves the
// previous account's key.
class DomainKeyManager {
 public:
  static constexpr char kDomain[] = "phi";

  DomainKeyManager(KeyApi& api,
                   AccountKeyManager& manager,
                   const ProfilePayloadCodec& codec);
  ~DomainKeyManager();

  // kNotUnlocked without an ARK: never mint a key that cannot be escrowed.
  KeyStatus DomainKey(Bytes* out);
  void Clear();

 private:
  KeyStatus Unseal(const Bytes& envelope, const Bytes& ark, Bytes* key) const;

  RAW_PTR_EXCLUSION KeyApi& api_;
  RAW_PTR_EXCLUSION AccountKeyManager& manager_;
  RAW_PTR_EXCLUSION const ProfilePayloadCodec& codec_;
  std::optional<std::pair<Bytes, Bytes>> cached_;  // ARK fingerprint, key
};

// --- The controller's mapping pass and pairing gate ----------------------------

struct LocalProfile {
  std::string profile_id;
  std::string display_name;
  bool is_default = false;  // this device's Default profile
};

// What an announcement says about the two pairing predicates: only
// kMeasured is an answer (SyncKeyController.MappingsOutcome).
enum class MappingsOutcome { kMeasured, kHeld, kCleared };

struct ResolvedKey {
  std::string uuid;
  std::string passphrase;
};

// The pairing wizard's inputs: locals that are unmapped (or mapped to a uuid
// the account no longer has) and account profiles no local has claimed.
struct PairingCandidates {
  std::vector<LocalProfile> locals;
  std::vector<RemoteProfile> remotes;
};

// One profile decision from the wizard (PairingDecision; createLocal
// arrives with the profile the caller created for it).
struct PairingDecision {
  enum class Kind { kAdopt, kRegisterNew, kCreateLocal };
  Kind kind = Kind::kRegisterNew;
  std::string local_profile_id;
  std::string remote_uuid;
  std::string display_name;
};

// SyncKeyController without the main actor: owns the resolved cache and the
// pairing predicates, and is driven by the key worker.
class SyncKeyCore {
 public:
  SyncKeyCore(AccountKeyManager& manager,
              ProfileKeyManager& profiles,
              KeyStateStore& state);
  ~SyncKeyCore();

  // Metadata-only warnings (the default-profile conflicts, backfill
  // failures) for the owner to log; this layer has no logger of its own.
  void set_warning_sink(std::function<void(const std::string&)> sink) {
    warn_ = std::move(sink);
  }

  SyncKeyCore(const SyncKeyCore&) = delete;
  SyncKeyCore& operator=(const SyncKeyCore&) = delete;

  // The Mac's retire(): every write after a suspension checks it. Any
  // thread (the owner retires from the UI thread while the worker runs).
  void Retire() { retired_ = true; }
  bool retired() const { return retired_; }

  // --- The pairing gate (ProfilePairingGate's enrollment) --------------------
  bool IsPaired();
  // Writes paired:false for the current device id (sync pauses).
  KeyStatus BeginEnrollment();
  // Writes paired:true for the device id the join ended with.
  KeyStatus CompleteEnrollment();
  // Writes paired:false (removal / reconfiguration).
  KeyStatus InvalidateEnrollment();

  // Startup/login: unlock, then resolve. Anything but kUnlocked clears.
  void SilentUnlockAndResolve(const std::vector<LocalProfile>& locals,
                              UnlockResult* unlock,
                              KeyStatus* unlock_status);
  void ResolveMappings(const std::vector<LocalProfile>& locals);
  // Lock / sign-out / teardown: drop every cached key.
  void ClearResolved();

  // What Chromium may see: the resolved keys, only while paired.
  std::map<std::string, ResolvedKey> ExposedKeys();
  const std::map<std::string, ResolvedKey>& resolved() const {
    return resolved_;
  }
  bool needs_pairing() const { return needs_pairing_; }
  bool needs_pairing_actionable() const { return needs_pairing_actionable_; }
  MappingsOutcome last_outcome() const { return last_outcome_; }

  // --- The wizard's profile step --------------------------------------------
  KeyStatus StartPairing(const std::vector<LocalProfile>& locals,
                         PairingCandidates* out);
  // Applies each decision (alreadyMapped counts as done), checks every
  // local is mapped, completes enrollment, re-resolves.
  KeyStatus ApplyDecisions(const std::vector<PairingDecision>& decisions,
                           const std::vector<LocalProfile>& locals);

  // Remote refusal leaves local state untouched. The service journals the
  // successful removal before calling ClearLocalSyncState.
  KeyStatus RevokeThisDevice();
  // The account reset for THIS device under `operation_id` (journaled by
  // the service before the call). Remote first, like RevokeThisDevice: a
  // refusal (kNotLastDevice, kDeviceNotFound) or a transport failure leaves
  // everything local as it was.
  KeyStatus ResetAccountFromThisDevice(const std::string& operation_id);
  // Idempotent local cleanup, also used for confirmed reconfiguration.
  KeyStatus ClearLocalSyncState(bool removing_device);

 private:
  void ResolveOnce(const std::vector<LocalProfile>& locals);
  KeyStatus CurrentDeviceKeyId(std::string* id);

  RAW_PTR_EXCLUSION AccountKeyManager& manager_;
  RAW_PTR_EXCLUSION ProfileKeyManager& profiles_;
  RAW_PTR_EXCLUSION KeyStateStore& state_;
  std::atomic<bool> retired_{false};
  std::map<std::string, ResolvedKey> resolved_;
  bool needs_pairing_ = false;
  bool needs_pairing_actionable_ = false;
  std::set<std::string> undecryptable_remote_uuids_;
  // The default-profile rule's bookkeeping: extra account defaults already
  // demoted and logged (once per uuid, not every pass); legacy envelopes of
  // this device's mappings whose "default" flag is confirmed (profile id ->
  // uuid, so a changed mapping re-checks); backfill failures logged once.
  std::set<std::string> logged_default_conflict_uuids_;
  std::map<std::string, std::string> confirmed_default_flag_;
  std::set<std::string> logged_default_flag_failure_uuids_;
  std::function<void(const std::string&)> warn_;
  MappingsOutcome last_outcome_ = MappingsOutcome::kCleared;
};

}  // namespace phi_client::keys

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_KEYS_H_

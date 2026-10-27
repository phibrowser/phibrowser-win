// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_sync_keys.h"

#include <algorithm>
#include <utility>

namespace phi_client::keys {

// --- Errors ------------------------------------------------------------------

KeyStatus KeyStatus::Error(KeyErrorKind kind, std::string detail) {
  KeyStatus status;
  status.kind = kind;
  status.detail = std::move(detail);
  return status;
}

KeyStatus KeyStatus::Http(int status_code, std::string body) {
  KeyStatus status;
  status.kind = KeyErrorKind::kHttp;
  status.http_status = status_code;
  status.detail = std::move(body);
  return status;
}

std::string KeyStatusName(const KeyStatus& status) {
  switch (status.kind) {
    case KeyErrorKind::kOk:
      return "ok";
    case KeyErrorKind::kNoToken:
      return "no_token";
    case KeyErrorKind::kTransport:
      return "transport";
    case KeyErrorKind::kHttp:
      return "http_" + std::to_string(status.http_status);
    case KeyErrorKind::kDecode:
      return "decode";
    case KeyErrorKind::kLastActiveDevice:
      return "last_active_device";
    case KeyErrorKind::kNotLastDevice:
      return "not_last_device";
    case KeyErrorKind::kDeviceNotFound:
      return "device_not_found";
    case KeyErrorKind::kTooManyPending:
      return "too_many_pending";
    case KeyErrorKind::kNotPending:
      return "not_pending";
    case KeyErrorKind::kNotFound:
      return "not_found";
    case KeyErrorKind::kInvalidRequest:
      return "invalid_request";
    case KeyErrorKind::kAlreadyInitialized:
      return "already_initialized";
    case KeyErrorKind::kBadRecoveryCode:
      return "bad_recovery_code";
    case KeyErrorKind::kNotInitialized:
      return "not_initialized";
    case KeyErrorKind::kNotUnlocked:
      return "not_unlocked";
    case KeyErrorKind::kBadEnvelope:
      return "bad_envelope";
    case KeyErrorKind::kAlreadyMapped:
      return "already_mapped";
    case KeyErrorKind::kDefaultMismatch:
      return "default_mismatch";
    case KeyErrorKind::kMetadataConflict:
      return "metadata_conflict";
    case KeyErrorKind::kCrypto:
      return "crypto";
    case KeyErrorKind::kDeviceKey:
      return "device_key";
    case KeyErrorKind::kStorage:
      return "storage";
    case KeyErrorKind::kCancelled:
      return "cancelled";
  }
  return "unknown";
}

bool IsPaired(const std::optional<EnrollmentRecord>& record,
              const std::string& device_key_id) {
  return record && !device_key_id.empty() && record->version == 1 &&
         record->device_key_id == device_key_id && record->paired;
}

std::string NewLowercaseUuid() {
  Bytes bytes = RandomBytes(16);
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);  // version 4
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);  // RFC 4122
  const std::string hex = HexLower(bytes);
  return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) +
         "-" + hex.substr(16, 4) + "-" + hex.substr(20);
}

// --- AccountKeyManager ---------------------------------------------------------

namespace {
constexpr char kKdfVersion[] = "hkdf-sha256-v1";
constexpr size_t kSaltBytes = 16;
}  // namespace

AccountKeyManager::AccountKeyManager(KeyApi& api,
                                     DeviceKeyStore& device_keys,
                                     KeyStateStore& state,
                                     DeviceIdentity identity)
    : api_(api),
      device_keys_(device_keys),
      state_(state),
      identity_(std::move(identity)) {}

AccountKeyManager::~AccountKeyManager() = default;

std::optional<Bytes> AccountKeyManager::current_ark() const {
  std::lock_guard<std::mutex> lock(ark_lock_);
  return ark_;
}

void AccountKeyManager::SetArk(const Bytes& ark) {
  bool was_locked = false;
  {
    std::lock_guard<std::mutex> lock(ark_lock_);
    was_locked = !ark_.has_value();
    ark_ = ark;
  }
  // Announced outside the lock: a listener's first move is to read it back.
  if (was_locked && on_unlocked_) {
    on_unlocked_();
  }
}

void AccountKeyManager::DiscardArk() {
  std::lock_guard<std::mutex> lock(ark_lock_);
  ark_.reset();
}

KeyStatus AccountKeyManager::DiscardLocalRegistration() {
  DiscardArk();
  std::string id;
  KeyStatus status = DeviceKeyId(&id);
  if (!status.ok()) {
    return status;
  }
  state_.ClearPendingRegistration(id);
  return KeyStatus::Ok();
}

KeyStatus AccountKeyManager::RotateDeviceKey() {
  return device_keys_.Rotate();
}

KeyStatus AccountKeyManager::DevicePublicKey(Bytes* public_key) {
  DeviceKeyPair pair;
  KeyStatus status = device_keys_.LoadOrCreate(&pair);
  if (status.ok()) {
    *public_key = pair.public_key;
  }
  return status;
}

KeyStatus AccountKeyManager::DeviceKeyId(std::string* id) {
  Bytes public_key;
  KeyStatus status = DevicePublicKey(&public_key);
  if (status.ok()) {
    *id = keys::DeviceKeyId(public_key);
  }
  return status;
}

KeyStatus AccountKeyManager::Bootstrap(std::string* recovery_code_display) {
  const Bytes ark = GenerateArk();
  const RecoveryCode code = GenerateRecoveryCode();
  const Bytes salt = RandomBytes(kSaltBytes);
  const Bytes recovery_key = DeriveRecoveryKey(code.entropy, salt);
  const std::optional<Bytes> recovery_envelope =
      SealWithSymmetric(ark, recovery_key);
  if (!recovery_envelope) {
    return KeyStatus::Error(KeyErrorKind::kCrypto, "seal recovery envelope");
  }
  bool created = false;
  KeyStatus status =
      api_.PutAccount(salt, kKdfVersion, "{}", *recovery_envelope, &created);
  if (!status.ok()) {
    return status;
  }
  if (!created) {
    return KeyStatus::Error(KeyErrorKind::kAlreadyInitialized);
  }
  // From here the account exists on the server and this process holds the
  // only copy of the ARK outside the recovery envelope: cache it first, and
  // park a registration that fails rather than losing both (review A8).
  SetArk(ark);
  if (!RegisterThisDevice(ark).ok()) {
    ParkRegistration(ark);
  }
  *recovery_code_display = code.display;
  return KeyStatus::Ok();
}

void AccountKeyManager::ParkRegistration(const Bytes& ark) {
  DeviceKeyPair pair;
  if (!device_keys_.LoadOrCreate(&pair).ok()) {
    return;  // The recovery code is then the only way back in.
  }
  const std::optional<Bytes> envelope = SealToPublicKey(ark, pair.public_key);
  if (envelope) {
    state_.SetPendingRegistration(keys::DeviceKeyId(pair.public_key),
                                  *envelope);
  }
}

KeyStatus AccountKeyManager::JoinWithRecoveryCode(std::string_view code) {
  const std::optional<Bytes> entropy = DecodeRecoveryCode(code);
  if (!entropy) {
    return KeyStatus::Error(KeyErrorKind::kBadRecoveryCode);
  }
  std::optional<AccountKeyState> account;
  KeyStatus status = api_.GetAccount(&account);
  if (!status.ok()) {
    return status;
  }
  if (!account) {
    return KeyStatus::Error(KeyErrorKind::kNotInitialized);
  }
  const Bytes recovery_key = DeriveRecoveryKey(*entropy, account->recovery_salt);
  // A well-formed code that does not open the envelope is the wrong code.
  const std::optional<Bytes> ark =
      OpenWithSymmetric(account->recovery_ark_envelope, recovery_key);
  if (!ark) {
    return KeyStatus::Error(KeyErrorKind::kBadRecoveryCode);
  }
  status = RegisterThisDevice(*ark);
  if (!status.ok()) {
    return status;
  }
  SetArk(*ark);
  return KeyStatus::Ok();
}

KeyStatus AccountKeyManager::UnlockAtStartup(UnlockResult* result) {
  DeviceKeyPair pair;
  KeyStatus status = device_keys_.LoadOrCreate(&pair);
  if (!status.ok()) {
    return status;
  }
  const std::string device_key_id = keys::DeviceKeyId(pair.public_key);
  std::optional<Bytes> envelope;
  status = api_.GetDeviceEnvelope(device_key_id, &envelope);
  if (status.kind == KeyErrorKind::kHttp && status.http_status == 401) {
    // No valid token: not signed in, distinct from "not joined yet".
    *result = UnlockResult::kNotSignedIn;
    return KeyStatus::Ok();
  }
  if (!status.ok()) {
    return status;
  }
  if (!envelope) {
    // Before "needs to join": a bootstrap here may have parked its
    // registration (review A8). Finish it; if the server still refuses,
    // this session runs on the ARK it holds and the next unlock retries.
    const std::optional<Bytes> parked =
        state_.PendingRegistration(device_key_id);
    if (parked) {
      std::optional<Bytes> ark = OpenWithPrivateKey(*parked, pair.private_key);
      if (!ark) {
        return KeyStatus::Error(KeyErrorKind::kCrypto, "open parked envelope");
      }
      if (RegisterThisDevice(*ark).ok()) {
        state_.ClearPendingRegistration(device_key_id);
      }
      SetArk(*ark);
      *result = UnlockResult::kUnlocked;
      return KeyStatus::Ok();
    }
    *result = UnlockResult::kNeedsJoin;
    return KeyStatus::Ok();
  }
  const std::optional<Bytes> ark =
      OpenWithPrivateKey(*envelope, pair.private_key);
  if (!ark) {
    return KeyStatus::Error(KeyErrorKind::kCrypto, "open device envelope");
  }
  SetArk(*ark);
  *result = UnlockResult::kUnlocked;
  return KeyStatus::Ok();
}

KeyStatus AccountKeyManager::AccountExists(bool* exists) {
  std::optional<AccountKeyState> account;
  KeyStatus status = api_.GetAccount(&account);
  if (status.ok()) {
    *exists = account.has_value();
  }
  return status;
}

KeyStatus AccountKeyManager::RequestJoinApproval(JoinTicket* ticket) {
  Bytes public_key;
  KeyStatus status = DevicePublicKey(&public_key);
  if (!status.ok()) {
    return status;
  }
  // Older clients and interrupted cancellations may leave pending tickets
  // behind. Match the full public key, never the name or its short code; a
  // failed withdrawal blocks the replacement.
  std::vector<JoinRequestSummary> pending;
  status = api_.ListPendingJoinRequests(&pending);
  if (!status.ok()) {
    return status;
  }
  for (const JoinRequestSummary& request : pending) {
    if (request.requesting_public_key == public_key) {
      status = CancelJoinApproval(request.request_id);
      if (!status.ok()) {
        return status;
      }
    }
  }
  std::string request_id;
  status = api_.PostJoinRequest(public_key, identity_.name, identity_.platform,
                                &request_id);
  if (!status.ok()) {
    return status;
  }
  ticket->request_id = request_id;
  ticket->verification_code = VerificationCode(public_key);
  return KeyStatus::Ok();
}

KeyStatus AccountKeyManager::CancelJoinApproval(const std::string& request_id) {
  KeyStatus status = api_.DenyJoinRequest(request_id);
  if (status.kind == KeyErrorKind::kNotPending ||
      status.kind == KeyErrorKind::kNotFound) {
    return KeyStatus::Ok();
  }
  return status;
}

KeyStatus AccountKeyManager::PollJoin(const std::string& request_id,
                                      const std::function<bool()>& cancelled,
                                      JoinPoll* poll) {
  JoinRequest request;
  KeyStatus status = api_.GetJoinRequest(request_id, &request);
  if (!status.ok()) {
    return status;
  }
  if (cancelled && cancelled()) {
    return KeyStatus::Error(KeyErrorKind::kCancelled);
  }
  if (request.status == "approved") {
    DeviceKeyPair pair;
    status = device_keys_.LoadOrCreate(&pair);
    if (!status.ok()) {
      return status;
    }
    const std::optional<Bytes> ark =
        OpenWithPrivateKey(request.granted_ark_envelope, pair.private_key);
    if (!ark) {
      return KeyStatus::Error(KeyErrorKind::kCrypto, "open granted envelope");
    }
    status = RegisterThisDevice(*ark);
    if (!status.ok()) {
      return status;
    }
    if (cancelled && cancelled()) {
      return KeyStatus::Error(KeyErrorKind::kCancelled);
    }
    SetArk(*ark);
    poll->state = JoinPollState::kApproved;
  } else if (request.status == "denied") {
    poll->state = JoinPollState::kDenied;
  } else if (request.status == "expired") {
    poll->state = JoinPollState::kExpired;
  } else {
    poll->state = JoinPollState::kPending;
    poll->deadline_ms = request.created_at_ms + kJoinRequestLifetimeMs;
  }
  return KeyStatus::Ok();
}

KeyStatus AccountKeyManager::RegisterThisDevice(const Bytes& ark) {
  KeyStatus status = PostRegistration(ark);
  if (status.kind == KeyErrorKind::kHttp && status.http_status == 409) {
    // device_revoked is forever for that fingerprint: re-mint the identity
    // (the ARK is already in hand) and retry once (review A10).
    status = device_keys_.Rotate();
    if (!status.ok()) {
      return status;
    }
    status = PostRegistration(ark);
  }
  return status;
}

KeyStatus AccountKeyManager::PostRegistration(const Bytes& ark) {
  DeviceKeyPair pair;
  KeyStatus status = device_keys_.LoadOrCreate(&pair);
  if (!status.ok()) {
    return status;
  }
  const std::optional<Bytes> envelope = SealToPublicKey(ark, pair.public_key);
  if (!envelope) {
    return KeyStatus::Error(KeyErrorKind::kCrypto, "seal device envelope");
  }
  return api_.PostDevice(keys::DeviceKeyId(pair.public_key), pair.public_key,
                         identity_.name, identity_.platform, *envelope);
}

// --- DeviceApprovalService ---------------------------------------------------

DeviceApprovalService::DeviceApprovalService(KeyApi& api,
                                             AccountKeyManager& manager)
    : api_(api), manager_(manager) {}

DeviceApprovalService::~DeviceApprovalService() = default;

KeyStatus DeviceApprovalService::ListDevices(std::vector<AccountDevice>* out) {
  return api_.ListDevices(out);
}

KeyStatus DeviceApprovalService::ListPendingApprovals(
    std::vector<PendingApproval>* out) {
  std::vector<JoinRequestSummary> pending;
  KeyStatus status = api_.ListPendingJoinRequests(&pending);
  if (!status.ok()) {
    return status;
  }
  out->clear();
  for (const JoinRequestSummary& request : pending) {
    PendingApproval approval;
    approval.id = request.request_id;
    approval.name = request.name;
    approval.platform = request.platform;
    approval.verification_code = VerificationCode(request.requesting_public_key);
    approval.deadline_ms = request.created_at_ms + kJoinRequestLifetimeMs;
    approval.requesting_public_key = request.requesting_public_key;
    out->push_back(std::move(approval));
  }
  return KeyStatus::Ok();
}

KeyStatus DeviceApprovalService::Approve(const PendingApproval& approval) {
  const std::optional<Bytes> ark = manager_.current_ark();
  if (!ark) {
    return KeyStatus::Error(KeyErrorKind::kNotUnlocked);
  }
  const std::optional<Bytes> envelope =
      SealToPublicKey(*ark, approval.requesting_public_key);
  if (!envelope) {
    return KeyStatus::Error(KeyErrorKind::kCrypto, "seal granted envelope");
  }
  std::string resolved_by;
  KeyStatus status = manager_.DeviceKeyId(&resolved_by);
  if (!status.ok()) {
    return status;
  }
  return api_.ApproveJoinRequest(approval.id, *envelope, resolved_by);
}

KeyStatus DeviceApprovalService::Deny(const std::string& request_id) {
  return api_.DenyJoinRequest(request_id);
}

// --- ProfileKeyManager ---------------------------------------------------------

ProfileKeyManager::ProfileKeyManager(KeyApi& api,
                                     AccountKeyManager& manager,
                                     KeyStateStore& state,
                                     const ProfilePayloadCodec& codec)
    : api_(api), manager_(manager), state_(state), codec_(codec) {}

ProfileKeyManager::~ProfileKeyManager() = default;

std::optional<Bytes> ProfileKeyManager::SealPayload(
    const ProfilePayloadCodec& codec,
    const Bytes& key,
    const std::string& name,
    std::optional<bool> is_default,
    const Bytes& ark) {
  const std::string payload = codec.Encode(key, name, is_default);
  return SealWithSymmetric(Bytes(payload.begin(), payload.end()), ark);
}

bool ProfileKeyManager::OpenPayload(const ProfilePayloadCodec& codec,
                                    const Bytes& envelope,
                                    const Bytes& ark,
                                    Bytes* key,
                                    std::string* name,
                                    std::optional<bool>* is_default) {
  const std::optional<Bytes> plain = OpenWithSymmetric(envelope, ark);
  std::optional<bool> flag;
  const bool ok =
      plain && codec.Decode(std::string(plain->begin(), plain->end()), key,
                            name, &flag);
  if (is_default) {
    *is_default = ok ? flag : std::nullopt;
  }
  return ok;
}

KeyStatus ProfileKeyManager::BackfillDefaultFlag(const std::string& profile_id,
                                                 bool is_default,
                                                 bool* confirmed) {
  *confirmed = false;
  const std::optional<std::string> uuid = MappedGlobalUuid(profile_id);
  if (!uuid) {
    return KeyStatus::Ok();
  }
  const std::optional<Bytes> ark = manager_.current_ark();
  if (!ark) {
    return KeyStatus::Error(KeyErrorKind::kNotUnlocked);
  }
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (MappedGlobalUuid(profile_id) != uuid) {
      return KeyStatus::Ok();  // remapped underneath: the next pass decides
    }
    std::optional<Bytes> envelope;
    KeyStatus status = api_.GetProfileKey(*uuid, &envelope);
    if (!status.ok()) {
      return status;
    }
    if (!envelope) {
      return KeyStatus::Ok();  // gone: mapping repair's business, never re-mint
    }
    Bytes key;
    std::string name;
    std::optional<bool> flag;
    if (!OpenPayload(codec_, *envelope, *ark, &key, &name, &flag)) {
      return KeyStatus::Error(KeyErrorKind::kBadEnvelope);
    }
    // True wins across devices: a Default promotes false to true, an
    // ordinary local never demotes another device's Default.
    if (flag == true || flag == is_default) {
      *confirmed = true;
      return KeyStatus::Ok();
    }
    const std::optional<Bytes> plain = OpenWithSymmetric(*envelope, *ark);
    const std::string rewritten = codec_.WithDefaultFlag(
        std::string(plain->begin(), plain->end()), is_default);
    if (rewritten.empty()) {
      return KeyStatus::Error(KeyErrorKind::kBadEnvelope);
    }
    const std::optional<Bytes> sealed =
        SealWithSymmetric(Bytes(rewritten.begin(), rewritten.end()), *ark);
    if (!sealed) {
      return KeyStatus::Error(KeyErrorKind::kCrypto, "seal profile envelope");
    }
    bool replaced = false;
    status = api_.ReplaceProfileKeyEnvelope(*uuid, *envelope, *sealed, &replaced);
    if (!status.ok()) {
      return status;
    }
    if (replaced) {
      *confirmed = true;
      return KeyStatus::Ok();
    }
  }
  return KeyStatus::Error(KeyErrorKind::kMetadataConflict);
}

std::optional<std::string> ProfileKeyManager::MappedGlobalUuid(
    const std::string& profile_id) const {
  const std::map<std::string, std::string> mappings = state_.ProfileMappings();
  const auto it = mappings.find(profile_id);
  if (it == mappings.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool ProfileKeyManager::SetMapping(const std::string& profile_id,
                                   const std::string& uuid) {
  std::map<std::string, std::string> mappings = state_.ProfileMappings();
  mappings[profile_id] = uuid;
  return state_.SetProfileMappings(mappings);
}

KeyStatus ProfileKeyManager::RegisterLocalProfile(
    const std::string& profile_id,
    const std::string& display_name,
    bool is_default,
    ProfileKeyRecord* out) {
  if (const std::optional<std::string> prior = MappedGlobalUuid(profile_id)) {
    // Only a DEFINITIVE missing record permits a replacement: minting over
    // a live mapping would orphan the account's real envelope.
    std::optional<Bytes> existing;
    KeyStatus status = api_.GetProfileKey(*prior, &existing);
    if (!status.ok()) {
      return status;
    }
    if (existing || MappedGlobalUuid(profile_id) != prior) {
      return KeyStatus::Error(KeyErrorKind::kAlreadyMapped);
    }
  }
  const std::optional<Bytes> ark = manager_.current_ark();
  if (!ark) {
    return KeyStatus::Error(KeyErrorKind::kNotUnlocked);
  }
  const Bytes key = RandomBytes(kKeyBytes);
  const std::string uuid = NewLowercaseUuid();
  // Every registration writes the flag, false included: an omitted key
  // reads as unknown and would leave a new non-default pairable with
  // another device's Default.
  const std::optional<Bytes> envelope =
      SealPayload(codec_, key, display_name, is_default, *ark);
  if (!envelope) {
    return KeyStatus::Error(KeyErrorKind::kCrypto, "seal profile envelope");
  }
  bool created = false;
  KeyStatus status = api_.PutProfileKey(uuid, *envelope, &created);
  if (!status.ok()) {
    return status;
  }
  if (!created) {
    return AdoptRemoteProfile(uuid, profile_id, is_default, out);
  }
  if (!SetMapping(profile_id, uuid)) {
    // The envelope is up but unmapped here: the account-profile refresh
    // adopts it by name rather than this profile minting another.
    return KeyStatus::Error(KeyErrorKind::kStorage, "save profile mapping");
  }
  out->uuid = uuid;
  out->passphrase = HexLower(key);
  out->name = display_name;
  out->is_default = is_default;
  return KeyStatus::Ok();
}

KeyStatus ProfileKeyManager::AdoptRemoteProfile(const std::string& uuid,
                                                const std::string& profile_id,
                                                bool is_default,
                                                ProfileKeyRecord* out) {
  const std::optional<Bytes> ark = manager_.current_ark();
  if (!ark) {
    return KeyStatus::Error(KeyErrorKind::kNotUnlocked);
  }
  std::optional<Bytes> envelope;
  KeyStatus status = api_.GetProfileKey(uuid, &envelope);
  if (!status.ok()) {
    return status;
  }
  Bytes key;
  std::string name;
  std::optional<bool> remote_default;
  if (!envelope ||
      !OpenPayload(codec_, *envelope, *ark, &key, &name, &remote_default)) {
    return KeyStatus::Error(KeyErrorKind::kBadEnvelope);
  }
  // A device's Default never takes a known non-default; refused before
  // anything is written. A legacy envelope (no flag) is accepted.
  if (is_default && remote_default == false) {
    return KeyStatus::Error(KeyErrorKind::kDefaultMismatch);
  }
  // The mapping is written only once the envelope has opened.
  if (!SetMapping(profile_id, uuid)) {
    return KeyStatus::Error(KeyErrorKind::kStorage, "save profile mapping");
  }
  out->uuid = uuid;
  out->passphrase = HexLower(key);
  out->name = name;
  out->is_default = remote_default;
  return KeyStatus::Ok();
}

KeyStatus ProfileKeyManager::ResolvedRecord(
    const std::string& profile_id,
    std::optional<ProfileKeyRecord>* out) {
  out->reset();
  const std::optional<std::string> uuid = MappedGlobalUuid(profile_id);
  if (!uuid) {
    return KeyStatus::Ok();
  }
  const std::optional<Bytes> ark = manager_.current_ark();
  if (!ark) {
    return KeyStatus::Error(KeyErrorKind::kNotUnlocked);
  }
  std::optional<Bytes> envelope;
  KeyStatus status = api_.GetProfileKey(*uuid, &envelope);
  if (!status.ok() || !envelope) {
    return status;
  }
  Bytes key;
  std::string name;
  std::optional<bool> is_default;
  if (!OpenPayload(codec_, *envelope, *ark, &key, &name, &is_default)) {
    // The Mac throws here: a record that will not open is UNKNOWN, never
    // "unmapped" (which would re-register over it).
    return KeyStatus::Error(KeyErrorKind::kBadEnvelope);
  }
  *out = ProfileKeyRecord{*uuid, HexLower(key), name, is_default};
  return KeyStatus::Ok();
}

KeyStatus ProfileKeyManager::AccountProfiles(std::vector<RemoteProfile>* out) {
  const std::optional<Bytes> ark = manager_.current_ark();
  if (!ark) {
    return KeyStatus::Error(KeyErrorKind::kNotUnlocked);
  }
  std::vector<ProfileSummary> summaries;
  KeyStatus status = api_.ListProfiles(&summaries);
  if (!status.ok()) {
    return status;
  }
  out->clear();
  for (const ProfileSummary& summary : summaries) {
    // A transport failure on any envelope fails the whole call; one that
    // merely will not open yields a nameless row in place.
    RemoteProfile remote;
    status = Remote(summary.profile_uuid, &remote);
    if (!status.ok()) {
      return status;
    }
    out->push_back(std::move(remote));
  }
  return KeyStatus::Ok();
}

KeyStatus ProfileKeyManager::AccountProfileUuids(std::set<std::string>* out) {
  // The ARK guard belongs here: listing is a plain bearer call, and a
  // locked device must read as "skipped", not "ran and changed nothing".
  if (!manager_.current_ark()) {
    return KeyStatus::Error(KeyErrorKind::kNotUnlocked);
  }
  std::vector<ProfileSummary> summaries;
  KeyStatus status = api_.ListProfiles(&summaries);
  if (!status.ok()) {
    return status;
  }
  out->clear();
  for (const ProfileSummary& summary : summaries) {
    out->insert(summary.profile_uuid);
  }
  return KeyStatus::Ok();
}

KeyStatus ProfileKeyManager::Remote(const std::string& uuid,
                                    RemoteProfile* out) {
  const std::optional<Bytes> ark = manager_.current_ark();
  if (!ark) {
    return KeyStatus::Error(KeyErrorKind::kNotUnlocked);
  }
  out->uuid = uuid;
  out->name.reset();
  out->is_default.reset();
  std::optional<Bytes> envelope;
  KeyStatus status = api_.GetProfileKey(uuid, &envelope);
  if (!status.ok()) {
    return status;
  }
  Bytes key;
  std::string name;
  std::optional<bool> is_default;
  if (envelope &&
      OpenPayload(codec_, *envelope, *ark, &key, &name, &is_default)) {
    out->name = name;
    out->is_default = is_default;
  }
  return KeyStatus::Ok();
}

std::optional<std::string> ProfileKeyManager::LocalProfileIdFor(
    const std::string& uuid) const {
  // std::map iterates in key order: the first match is the smallest id.
  for (const auto& [profile_id, mapped] : state_.ProfileMappings()) {
    if (mapped == uuid) {
      return profile_id;
    }
  }
  return std::nullopt;
}

void ProfileKeyManager::RemoveMapping(const std::string& profile_id) {
  std::map<std::string, std::string> mappings = state_.ProfileMappings();
  if (mappings.erase(profile_id)) {
    state_.SetProfileMappings(mappings);
  }
}

bool ProfileKeyManager::RemoveAllMappings() {
  return state_.SetProfileMappings({});
}

std::map<std::string, std::string> ProfileKeyManager::AllMappings() const {
  return state_.ProfileMappings();
}

KeyStatus ProfileKeyManager::RevokeDevice(const std::string& device_key_id) {
  return api_.RevokeDevice(device_key_id);
}

KeyStatus ProfileKeyManager::ResetAccount(const std::string& operation_id,
                                          const std::string& device_key_id) {
  return api_.ResetAccount(operation_id, device_key_id);
}

// --- DomainKeyManager ---------------------------------------------------------

DomainKeyManager::DomainKeyManager(KeyApi& api,
                                   AccountKeyManager& manager,
                                   const ProfilePayloadCodec& codec)
    : api_(api), manager_(manager), codec_(codec) {}

DomainKeyManager::~DomainKeyManager() = default;

void DomainKeyManager::Clear() {
  cached_.reset();
}

KeyStatus DomainKeyManager::Unseal(const Bytes& envelope,
                                   const Bytes& ark,
                                   Bytes* key) const {
  std::string name;
  if (!ProfileKeyManager::OpenPayload(codec_, envelope, ark, key, &name)) {
    return KeyStatus::Error(KeyErrorKind::kBadEnvelope);
  }
  return KeyStatus::Ok();
}

KeyStatus DomainKeyManager::DomainKey(Bytes* out) {
  const std::optional<Bytes> ark = manager_.current_ark();
  if (!ark) {
    return KeyStatus::Error(KeyErrorKind::kNotUnlocked);
  }
  const Bytes fingerprint = Sha256(*ark);
  if (cached_ && cached_->first == fingerprint) {
    *out = cached_->second;
    return KeyStatus::Ok();
  }
  cached_.reset();
  std::optional<Bytes> envelope;
  KeyStatus status = api_.GetDomainKey(kDomain, &envelope);
  if (!status.ok()) {
    return status;
  }
  Bytes key;
  if (envelope) {
    status = Unseal(*envelope, *ark, &key);
  } else {
    key = RandomBytes(kKeyBytes);
    const std::optional<Bytes> sealed =
        ProfileKeyManager::SealPayload(codec_, key, kDomain, std::nullopt,
                                       *ark);
    if (!sealed) {
      return KeyStatus::Error(KeyErrorKind::kCrypto, "seal domain envelope");
    }
    bool created = false;
    status = api_.PutDomainKey(kDomain, *sealed, &created);
    if (status.ok() && !created) {
      // Lost the race: adopt the winner, drop ours — data sealed with it
      // would be unreadable everywhere else.
      status = api_.GetDomainKey(kDomain, &envelope);
      if (status.ok()) {
        status = envelope ? Unseal(*envelope, *ark, &key)
                          : KeyStatus::Error(KeyErrorKind::kBadEnvelope);
      }
    }
  }
  if (!status.ok()) {
    return status;
  }
  cached_ = std::make_pair(fingerprint, key);
  *out = key;
  return KeyStatus::Ok();
}

// --- SyncKeyCore ------------------------------------------------------------------

SyncKeyCore::SyncKeyCore(AccountKeyManager& manager,
                         ProfileKeyManager& profiles,
                         KeyStateStore& state)
    : manager_(manager), profiles_(profiles), state_(state) {}

SyncKeyCore::~SyncKeyCore() = default;

KeyStatus SyncKeyCore::CurrentDeviceKeyId(std::string* id) {
  return manager_.DeviceKeyId(id);
}

bool SyncKeyCore::IsPaired() {
  std::string id;
  if (!CurrentDeviceKeyId(&id).ok()) {
    return false;
  }
  return keys::IsPaired(state_.Enrollment(), id);
}

KeyStatus SyncKeyCore::BeginEnrollment() {
  std::string id;
  KeyStatus status = CurrentDeviceKeyId(&id);
  if (!status.ok()) {
    return status;
  }
  if (!state_.SetEnrollment(EnrollmentRecord{1, id, false})) {
    return KeyStatus::Error(KeyErrorKind::kStorage, "enrollment");
  }
  return KeyStatus::Ok();
}

KeyStatus SyncKeyCore::CompleteEnrollment() {
  // The post-registration identity: a revoked-key retry may have rotated it.
  std::string id;
  KeyStatus status = CurrentDeviceKeyId(&id);
  if (!status.ok()) {
    return status;
  }
  if (!state_.SetEnrollment(EnrollmentRecord{1, id, true})) {
    return KeyStatus::Error(KeyErrorKind::kStorage, "enrollment");
  }
  return KeyStatus::Ok();
}

KeyStatus SyncKeyCore::InvalidateEnrollment() {
  return BeginEnrollment();
}

void SyncKeyCore::ClearResolved() {
  resolved_.clear();
  needs_pairing_ = false;
  needs_pairing_actionable_ = false;
  last_outcome_ = MappingsOutcome::kCleared;
}

std::map<std::string, ResolvedKey> SyncKeyCore::ExposedKeys() {
  if (!IsPaired()) {
    return {};
  }
  return resolved_;
}

void SyncKeyCore::SilentUnlockAndResolve(
    const std::vector<LocalProfile>& locals,
    UnlockResult* unlock,
    KeyStatus* unlock_status) {
  if (retired_) {
    return;
  }
  // Fails CLOSED: anything but an unlock (a transient error included)
  // clears the cache — Chromium then pulls nothing and the gate shuts.
  UnlockResult result = UnlockResult::kNeedsJoin;
  const KeyStatus status = manager_.UnlockAtStartup(&result);
  if (unlock) {
    *unlock = result;
  }
  if (unlock_status) {
    *unlock_status = status;
  }
  if (retired_) {
    return;
  }
  if (!status.ok() || result != UnlockResult::kUnlocked) {
    ClearResolved();
    return;
  }
  ResolveMappings(locals);
}

void SyncKeyCore::ResolveMappings(const std::vector<LocalProfile>& locals) {
  ResolveOnce(locals);
}

void SyncKeyCore::ResolveOnce(const std::vector<LocalProfile>& locals) {
  if (retired_) {
    return;
  }
  std::map<std::string, ResolvedKey> next;
  std::vector<LocalProfile> unmapped;
  bool has_unknown_local = false;
  // Each mapped local's envelope flag as this pass read it: the backfill
  // below needs no second read for one that already carries it.
  std::map<std::string, std::optional<bool>> mapped_flags;
  for (const LocalProfile& local : locals) {
    std::optional<ProfileKeyRecord> record;
    const KeyStatus status = profiles_.ResolvedRecord(local.profile_id, &record);
    if (!status.ok()) {
      // UNKNOWN, not absent: keep what was resolved, and hold this local
      // (and so the whole register/adopt decision) out of this pass.
      has_unknown_local = true;
      const auto previous = resolved_.find(local.profile_id);
      if (previous != resolved_.end()) {
        next[local.profile_id] = previous->second;
      }
    } else if (record) {
      next[local.profile_id] = ResolvedKey{record->uuid, record->passphrase};
      mapped_flags[local.profile_id] = record->is_default;
    } else {
      unmapped.push_back(local);
    }
  }

  std::set<std::string> remote_uuids;
  const KeyStatus listing = profiles_.AccountProfileUuids(&remote_uuids);
  if (retired_) {
    return;
  }
  if (!listing.ok()) {
    // Unknown remote set: hold both predicates (never flip a gate on a
    // blip) and keep the monotonic cache.
    for (auto& [id, key] : next) {
      resolved_[id] = key;
    }
    last_outcome_ = MappingsOutcome::kHeld;
    return;
  }

  if (!has_unknown_local && IsPaired()) {
    std::set<std::string> claimed;
    for (const auto& [id, key] : next) {
      claimed.insert(key.uuid);
    }
    // The unclaimed account profiles WITH their envelopes' flags (uuid
    // order, as the Mac's scan): the default-profile rule needs to know
    // which are account defaults. A read that fails holds every decision
    // this pass — never register or adopt around a profile that could not
    // be read.
    std::vector<RemoteProfile> unclaimed;
    bool unclaimed_unknown = false;
    for (const std::string& uuid : remote_uuids) {
      if (claimed.count(uuid)) {
        continue;
      }
      RemoteProfile remote;
      if (!profiles_.Remote(uuid, &remote).ok()) {
        unclaimed_unknown = true;
        break;
      }
      unclaimed.push_back(std::move(remote));
    }
    if (retired_) {
      return;
    }
    // The authoritative account default: the one this device's Default is
    // live-mapped to (its record is in the listing, or it would not be in
    // `next`). `free_default` is the Default while it is still unmapped.
    std::optional<std::string> authoritative;
    const LocalProfile* free_default = nullptr;
    for (const LocalProfile& local : locals) {
      if (!local.is_default) {
        continue;
      }
      const auto mapped = next.find(local.profile_id);
      if (mapped != next.end()) {
        authoritative = mapped->second.uuid;
      } else if (!has_unknown_local) {
        free_default = &local;
      }
    }
    if (!unclaimed_unknown) {
      // 1. A free Default adopts an account default first, regardless of
      //    name (the first by uuid). One attempt per pass: a failure leaves
      //    it free and the next pass retries; no unflagged profile may take
      //    it while a flagged one is among the candidates.
      bool flagged_candidate = false;
      if (free_default) {
        for (const RemoteProfile& remote : unclaimed) {
          if (remote.is_default != true) {
            continue;
          }
          flagged_candidate = true;
          if (retired_) {
            return;
          }
          ProfileKeyRecord record;
          if (profiles_
                  .AdoptRemoteProfile(remote.uuid, free_default->profile_id,
                                      /*is_default=*/true, &record)
                  .ok()) {
            next[free_default->profile_id] =
                ResolvedKey{record.uuid, record.passphrase};
            claimed.insert(remote.uuid);
            authoritative = remote.uuid;
            free_default = nullptr;
          }
          break;
        }
      }
      std::vector<LocalProfile> still_unmapped;
      for (const LocalProfile& local : unmapped) {
        if (!next.count(local.profile_id)) {
          still_unmapped.push_back(local);
        }
      }
      std::vector<RemoteProfile> still_unclaimed;
      for (const RemoteProfile& remote : unclaimed) {
        if (!claimed.count(remote.uuid)) {
          still_unclaimed.push_back(remote);
        }
      }
      // 2. Every account default other than the authoritative one is an
      //    ordinary profile here (two devices paired concurrently); the
      //    conflict is logged once per uuid, metadata only.
      for (const RemoteProfile& remote : still_unclaimed) {
        if (remote.is_default == true && authoritative &&
            logged_default_conflict_uuids_.insert(remote.uuid).second && warn_) {
          warn_("[SyncKeys] account default " + remote.uuid.substr(0, 8) +
                " demoted to an ordinary profile (authoritative " +
                authoritative->substr(0, 8) + ")");
        }
      }
      if (still_unclaimed.empty()) {
        // First device (or every remote claimed): register the rest, the
        // Default as the account default.
        for (const LocalProfile& local : still_unmapped) {
          if (retired_) {
            return;
          }
          ProfileKeyRecord record;
          if (profiles_
                  .RegisterLocalProfile(local.profile_id, local.display_name,
                                        local.is_default, &record)
                  .ok()) {
            next[local.profile_id] =
                ResolvedKey{record.uuid, record.passphrase};
          }
        }
      } else if (still_unclaimed.size() == 1 && still_unmapped.size() == 1) {
        const RemoteProfile& remote = still_unclaimed.front();
        const LocalProfile& local = still_unmapped.front();
        // Under the rule: a Default never takes a known non-default (and
        // waits while a flagged default it could not adopt is among the
        // candidates); another local never takes the authoritative default.
        const bool blocked_default =
            local.is_default && flagged_candidate && remote.is_default != true;
        if (!blocked_default && remote.MayPair(local.is_default, authoritative)) {
          if (retired_) {
            return;
          }
          ProfileKeyRecord record;
          if (profiles_
                  .AdoptRemoteProfile(remote.uuid, local.profile_id,
                                      local.is_default, &record)
                  .ok()) {
            next[local.profile_id] =
                ResolvedKey{record.uuid, record.passphrase};
          }
        }
      }
    }
  }

  if (retired_) {
    return;
  }
  // Monotonic within a session: merge, never replace.
  for (auto& [id, key] : next) {
    resolved_[id] = key;
  }
  // Best effort, after the pass's decisions: the "default" flag onto this
  // device's mapped legacy envelopes, so OTHER devices' pairing can tell
  // its Default from its other profiles. Nothing here reads the result;
  // it never holds the outcome below. A uuid is confirmed once its
  // envelope carries a flag this device agrees with (read above, or
  // written now); failures are logged once per uuid and retried next pass.
  for (const LocalProfile& local : locals) {
    const auto mapped = next.find(local.profile_id);
    if (mapped == next.end()) {
      continue;
    }
    const std::string& uuid = mapped->second.uuid;
    const auto confirmed = confirmed_default_flag_.find(local.profile_id);
    if (confirmed != confirmed_default_flag_.end() &&
        confirmed->second == uuid) {
      continue;
    }
    const auto flag = mapped_flags.find(local.profile_id);
    if (flag != mapped_flags.end() &&
        (flag->second == true || flag->second == local.is_default)) {
      confirmed_default_flag_[local.profile_id] = uuid;
      continue;
    }
    if (retired_) {
      return;
    }
    bool ok = false;
    const KeyStatus status =
        profiles_.BackfillDefaultFlag(local.profile_id, local.is_default, &ok);
    if (status.ok()) {
      if (ok) {
        confirmed_default_flag_[local.profile_id] = uuid;
      }
    } else if (logged_default_flag_failure_uuids_.insert(uuid).second &&
               warn_) {
      warn_("[SyncKeys] default flag backfill failed for " +
            uuid.substr(0, 8) + ": " + KeyStatusName(status));
    }
  }
  if (has_unknown_local) {
    last_outcome_ = MappingsOutcome::kHeld;
    return;
  }
  std::set<std::string> claimed_after;
  for (const auto& [id, key] : next) {
    claimed_after.insert(key.uuid);
  }
  bool still_unmapped = false;
  for (const LocalProfile& local : locals) {
    if (!next.count(local.profile_id)) {
      still_unmapped = true;
    }
  }
  bool still_unclaimed = false;
  bool actionable_unclaimed = false;
  for (const std::string& uuid : remote_uuids) {
    if (!claimed_after.count(uuid)) {
      still_unclaimed = true;
      if (!undecryptable_remote_uuids_.count(uuid)) {
        actionable_unclaimed = true;
      }
    }
  }
  needs_pairing_ = still_unmapped || still_unclaimed;
  needs_pairing_actionable_ = still_unmapped || actionable_unclaimed;
  last_outcome_ = MappingsOutcome::kMeasured;
}

KeyStatus SyncKeyCore::StartPairing(const std::vector<LocalProfile>& locals,
                                    PairingCandidates* out) {
  std::vector<RemoteProfile> account;
  KeyStatus status = profiles_.AccountProfiles(&account);
  if (!status.ok()) {
    return status;
  }
  if (retired_) {
    return KeyStatus::Error(KeyErrorKind::kCancelled);
  }
  std::set<std::string> remote_uuids;
  for (const RemoteProfile& remote : account) {
    remote_uuids.insert(remote.uuid);
  }
  std::set<std::string> claimed;
  out->locals.clear();
  for (const LocalProfile& local : locals) {
    const std::optional<std::string> uuid =
        profiles_.MappedGlobalUuid(local.profile_id);
    if (uuid && remote_uuids.count(*uuid)) {
      claimed.insert(*uuid);
    } else {
      out->locals.push_back(local);
    }
  }
  out->remotes.clear();
  for (const RemoteProfile& remote : account) {
    if (claimed.count(remote.uuid)) {
      continue;
    }
    if (remote.name) {
      undecryptable_remote_uuids_.erase(remote.uuid);
    } else {
      undecryptable_remote_uuids_.insert(remote.uuid);
    }
    out->remotes.push_back(remote);
  }
  return KeyStatus::Ok();
}

KeyStatus SyncKeyCore::ApplyDecisions(
    const std::vector<PairingDecision>& decisions,
    const std::vector<LocalProfile>& locals) {
  const auto local_is_default = [&locals](const std::string& profile_id) {
    for (const LocalProfile& local : locals) {
      if (local.profile_id == profile_id) {
        return local.is_default;
      }
    }
    return false;
  };
  for (const PairingDecision& decision : decisions) {
    if (retired_) {
      return KeyStatus::Error(KeyErrorKind::kCancelled);
    }
    ProfileKeyRecord record;
    KeyStatus status;
    switch (decision.kind) {
      case PairingDecision::Kind::kAdopt:
        status = profiles_.AdoptRemoteProfile(
            decision.remote_uuid, decision.local_profile_id,
            local_is_default(decision.local_profile_id), &record);
        break;
      case PairingDecision::Kind::kRegisterNew:
        status = profiles_.RegisterLocalProfile(
            decision.local_profile_id, decision.display_name,
            local_is_default(decision.local_profile_id), &record);
        break;
      case PairingDecision::Kind::kCreateLocal:
        // Idempotent replay: an account uuid already mapped here is done.
        if (profiles_.LocalProfileIdFor(decision.remote_uuid)) {
          continue;
        }
        // A profile created for an account profile is never the Default.
        status = profiles_.AdoptRemoteProfile(decision.remote_uuid,
                                              decision.local_profile_id,
                                              /*is_default=*/false, &record);
        break;
    }
    if (!status.ok() && status.kind != KeyErrorKind::kAlreadyMapped) {
      return status;
    }
  }
  // Enrollment opens only once every local profile is mapped.
  for (const LocalProfile& local : locals) {
    if (!profiles_.MappedGlobalUuid(local.profile_id)) {
      return KeyStatus::Error(KeyErrorKind::kStorage, "unmapped local profile");
    }
  }
  if (retired_) {
    return KeyStatus::Error(KeyErrorKind::kCancelled);
  }
  KeyStatus status = CompleteEnrollment();
  if (!status.ok()) {
    return status;
  }
  ResolveMappings(locals);
  return KeyStatus::Ok();
}

KeyStatus SyncKeyCore::RevokeThisDevice() {
  if (retired_) {
    return KeyStatus::Error(KeyErrorKind::kCancelled);
  }
  std::string id;
  KeyStatus status = CurrentDeviceKeyId(&id);
  if (!status.ok()) {
    return status;
  }
  // Remote first: a refusal (the last active device, or offline) leaves
  // everything local as it was.
  return profiles_.RevokeDevice(id);
}

KeyStatus SyncKeyCore::ResetAccountFromThisDevice(
    const std::string& operation_id) {
  if (retired_) {
    return KeyStatus::Error(KeyErrorKind::kCancelled);
  }
  std::string id;
  KeyStatus status = CurrentDeviceKeyId(&id);
  if (!status.ok()) {
    return status;
  }
  return profiles_.ResetAccount(operation_id, id);
}

KeyStatus SyncKeyCore::ClearLocalSyncState(bool removing_device) {
  if (retired_) {
    return KeyStatus::Error(KeyErrorKind::kCancelled);
  }
  ClearResolved();
  KeyStatus status = InvalidateEnrollment();
  if (!status.ok()) {
    return status;
  }
  status = manager_.DiscardLocalRegistration();
  if (!status.ok()) {
    return status;
  }
  // A new fingerprint is what lets this device rejoin: the server never
  // un-revokes the old one.
  if (removing_device) {
    status = manager_.RotateDeviceKey();
    if (!status.ok()) {
      return status;
    }
  }
  if (!profiles_.RemoveAllMappings() || !profiles_.AllMappings().empty()) {
    return KeyStatus::Error(KeyErrorKind::kStorage, "mappings");
  }
  return KeyStatus::Ok();
}

}  // namespace phi_client::keys

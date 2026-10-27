// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_KEY_API_WIN_H_
#define PHIBROWSER_WIN_CLIENT_PHI_KEY_API_WIN_H_

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "base/values.h"
#include "phibrowser-win/client/phi_sync_keys.h"
#include "phibrowser-win/client/phi_sync_protocol.h"

// The Windows halves of the key layer's seams (phi_sync_keys.h): the
// /keys/v1 client over WinHTTP, this device's key under DPAPI, the
// account's key-layer state file, and the profile payload's JSON. All of
// them are called on the key worker thread (PhiSyncKeyService).
namespace phi_client {

// /keys/v1 — the Mac's KeyEnvelopeAPIClient: the same paths, bodies and
// status mapping. Blocking. `token` is asked before every request and must
// answer for the account the client was built for (PhiAuth::
// AccessTokenBlockingFor) — no token, no request. A 401 drops the cached
// token (`invalidate_token`) and retries once with a fresh one.
class PhiKeyApiClient : public keys::KeyApi {
 public:
  PhiKeyApiClient(std::string base_url,
                  std::function<std::string()> token,
                  std::function<void()> invalidate_token);
  ~PhiKeyApiClient() override;

  keys::KeyStatus ListDevices(std::vector<keys::AccountDevice>* out) override;
  keys::KeyStatus PutAccount(const keys::Bytes& salt,
                             const std::string& kdf_version,
                             const std::string& kdf_params_json,
                             const keys::Bytes& recovery_envelope,
                             bool* created) override;
  keys::KeyStatus GetAccount(
      std::optional<keys::AccountKeyState>* out) override;
  keys::KeyStatus PostDevice(const std::string& device_key_id,
                             const keys::Bytes& public_key,
                             const std::string& name,
                             const std::string& platform,
                             const keys::Bytes& ark_envelope) override;
  keys::KeyStatus GetDeviceEnvelope(const std::string& device_key_id,
                                    std::optional<keys::Bytes>* out) override;
  keys::KeyStatus RevokeDevice(const std::string& device_key_id) override;
  keys::KeyStatus ResetAccount(const std::string& operation_id,
                               const std::string& device_key_id) override;
  keys::KeyStatus PostJoinRequest(const keys::Bytes& public_key,
                                  const std::string& name,
                                  const std::string& platform,
                                  std::string* request_id) override;
  keys::KeyStatus ListPendingJoinRequests(
      std::vector<keys::JoinRequestSummary>* out) override;
  keys::KeyStatus GetJoinRequest(const std::string& request_id,
                                 keys::JoinRequest* out) override;
  keys::KeyStatus ApproveJoinRequest(const std::string& request_id,
                                     const keys::Bytes& granted_ark_envelope,
                                     const std::string& resolved_by) override;
  keys::KeyStatus DenyJoinRequest(const std::string& request_id) override;
  keys::KeyStatus ListProfiles(
      std::vector<keys::ProfileSummary>* out) override;
  keys::KeyStatus GetProfileKey(const std::string& uuid,
                                std::optional<keys::Bytes>* envelope) override;
  keys::KeyStatus PutProfileKey(const std::string& uuid,
                                const keys::Bytes& envelope,
                                bool* created) override;
  keys::KeyStatus ReplaceProfileKeyEnvelope(const std::string& uuid,
                                            const keys::Bytes& expected,
                                            const keys::Bytes& envelope,
                                            bool* replaced) override;
  keys::KeyStatus GetDomainKey(const std::string& domain,
                               std::optional<keys::Bytes>* envelope) override;
  keys::KeyStatus PutDomainKey(const std::string& domain,
                               const keys::Bytes& envelope,
                               bool* created) override;

 private:
  struct Response {
    int status = 0;
    std::string body;
  };
  // One authorized round trip. Not ok() only for no token / transport.
  keys::KeyStatus Send(const wchar_t* method,
                       const std::string& path,
                       const base::DictValue* body,
                       Response* response);

  const std::string base_url_;
  std::function<std::string()> token_;
  std::function<void()> invalidate_token_;
};

// This device's X25519 key for one account, DPAPI-protected (the current
// Windows user) in <account dir>\sync\device_key.dat — the Mac's
// account-scoped Keychain item: never uploaded, never synced, and useless
// once copied to another machine or user (what the Mac's
// ThisDeviceOnly class buys). Rotation replaces the file in one
// MoveFileEx, never delete-then-create.
class PhiDpapiDeviceKeyStore : public keys::DeviceKeyStore {
 public:
  explicit PhiDpapiDeviceKeyStore(std::wstring path);
  ~PhiDpapiDeviceKeyStore() override;

  keys::KeyStatus LoadOrCreate(keys::DeviceKeyPair* out) override;
  keys::KeyStatus Rotate() override;

 private:
  keys::KeyStatus Write(const keys::Bytes& private_key);

  const std::wstring path_;
  std::mutex lock_;
  std::optional<keys::DeviceKeyPair> cached_;
};

// The account's key-layer state in <account dir>\sync\key_state.json,
// under the Mac's key names (sync.profileGlobalUuids,
// sync.pairingEnrollment, phi.sync.pendingDeviceRegistration.<id>). Its own
// file rather than account_defaults.json: a store bound to ONE account's
// path can never write into the next account's defaults after a switch.
// Thread-safe; every write is write-temp-then-replace.
class PhiKeyStateFile : public keys::KeyStateStore {
 public:
  explicit PhiKeyStateFile(std::wstring path);
  ~PhiKeyStateFile() override;

  std::map<std::string, std::string> ProfileMappings() const override;
  bool SetProfileMappings(
      const std::map<std::string, std::string>& mappings) override;
  std::optional<keys::Bytes> PendingRegistration(
      const std::string& device_key_id) const override;
  void SetPendingRegistration(const std::string& device_key_id,
                              const keys::Bytes& envelope) override;
  void ClearPendingRegistration(const std::string& device_key_id) override;
  std::optional<keys::EnrollmentRecord> Enrollment() const override;
  bool SetEnrollment(const keys::EnrollmentRecord& record) override;

  // sync.spaceGlobalUuids: local Space id → account Space uuid (the Mac's
  // AccountSpaceSyncMappingStore). The default Space is never stored.
  std::map<std::string, std::string> SpaceMappings() const;
  bool SetSpaceMappings(const std::map<std::string, std::string>& mappings);

 private:
  bool SaveLocked();
  // Sets `key` and saves; on a failed save `key` is restored in memory.
  bool SetAndSaveLocked(std::string_view key, base::DictValue value);

  const std::wstring path_;
  mutable std::mutex lock_;
  base::DictValue values_;
  // The file was there but unreadable at construction: never overwritten.
  bool save_blocked_ = false;
};

class PhiProfilePayloadJson : public keys::ProfilePayloadCodec {
 public:
  std::string Encode(const keys::Bytes& key,
                     const std::string& name,
                     std::optional<bool> is_default) const override;
  bool Decode(const std::string& json,
              keys::Bytes* key,
              std::string* name,
              std::optional<bool>* is_default) const override;
  std::string WithDefaultFlag(const std::string& json,
                              bool is_default) const override;
};

// Phi native sync's transport: POST <base>/chromium-sync/phi/command/
// ?client_id=<device key id>, application/octet-stream, with the same
// account-bound bearer as /keys/v1 (no token → no request; a 401 renews
// and retries once). Blocking.
class PhiSyncHttpTransport : public sync::SyncTransport {
 public:
  PhiSyncHttpTransport(std::string base_url,
                       std::function<std::string()> token,
                       std::function<void()> invalidate_token);
  ~PhiSyncHttpTransport() override;

  sync::ProtocolStatus Post(const std::string& client_id,
                            const std::string& body,
                            int* status,
                            std::string* response,
                            std::optional<int64_t>* server_date_ms) override;

 private:
  const std::string base_url_;
  std::function<std::string()> token_;
  std::function<void()> invalidate_token_;
};

// How this device names itself on the account: the computer's name (what
// Settings ▸ System ▸ About shows) and platform "windows".
keys::DeviceIdentity PhiThisDeviceIdentity();

// RFC 3339 (with or without fractional seconds, Z or an offset) → Unix ms.
std::optional<int64_t> PhiParseRfc3339Ms(const std::string& text);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_KEY_API_WIN_H_

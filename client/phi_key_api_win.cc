// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_key_api_win.h"

#include <windows.h>

#include <dpapi.h>

#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "phibrowser-win/client/phi_file_io.h"
#include "phibrowser-win/client/phi_http.h"

namespace phi_client {

namespace {

using keys::Bytes;
using keys::KeyErrorKind;
using keys::KeyStatus;

std::string BodyText(const std::string& body) {
  // Enough of the server's refusal for a log line or a verb, never more.
  return body.substr(0, 256);
}

// --- Response decoding (the Mac DTOs' rules: a malformed field fails the
// whole decode rather than inventing a value) --------------------------------

bool ReadBase64(const base::DictValue& dict, const char* key, Bytes* out) {
  const std::string* text = dict.FindString(key);
  if (!text) {
    return false;
  }
  std::optional<Bytes> bytes = keys::Base64Decode(*text);
  if (!bytes) {
    return false;
  }
  *out = std::move(*bytes);
  return true;
}

bool ReadString(const base::DictValue& dict, const char* key, std::string* out) {
  const std::string* text = dict.FindString(key);
  if (!text) {
    return false;
  }
  *out = *text;
  return true;
}

bool ReadTime(const base::DictValue& dict, const char* key, int64_t* out) {
  const std::string* text = dict.FindString(key);
  const std::optional<int64_t> ms = text ? PhiParseRfc3339Ms(*text) : std::nullopt;
  if (!ms) {
    return false;
  }
  *out = *ms;
  return true;
}

bool ReadSummary(const base::DictValue& dict, keys::JoinRequestSummary* out) {
  return ReadString(dict, "request_id", &out->request_id) &&
         ReadBase64(dict, "requesting_public_key",
                    &out->requesting_public_key) &&
         ReadString(dict, "name", &out->name) &&
         ReadString(dict, "platform", &out->platform) &&
         ReadString(dict, "status", &out->status) &&
         ReadTime(dict, "created_at", &out->created_at_ms);
}

std::optional<base::Value> ParseJson(const std::string& body) {
  return base::JSONReader::Read(body, base::JSON_PARSE_RFC);
}

KeyStatus Unexpected(int status, const std::string& body) {
  return KeyStatus::Http(status, BodyText(body));
}

// The `code` of a /keys/v1 error body ({"code": "...", "message": "..."}),
// or "" when the body is not one (KeyEnvelopeAPIClient.errorCode). Refusals
// that change what the client does next are matched on this code, never on
// the status alone.
std::string ErrorCode(const std::string& body) {
  std::optional<base::Value> json = ParseJson(body);
  const base::DictValue* dict = json ? json->GetIfDict() : nullptr;
  const std::string* code = dict ? dict->FindString("code") : nullptr;
  return code ? *code : std::string();
}

// KeyEnvelopeAPIClient.mapJoinError.
KeyStatus JoinError(int status, const std::string& body) {
  switch (status) {
    case 429:
      return KeyStatus::Error(KeyErrorKind::kTooManyPending);
    case 409:
      return KeyStatus::Error(KeyErrorKind::kNotPending);
    case 404:
      return KeyStatus::Error(KeyErrorKind::kNotFound);
    case 400:
      return KeyStatus::Error(KeyErrorKind::kInvalidRequest, BodyText(body));
    default:
      return Unexpected(status, body);
  }
}

bool EnsureDirectoryFor(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return true;
  }
  const std::wstring dir = path.substr(0, slash);
  return ::CreateDirectoryW(dir.c_str(), nullptr) ||
         ::GetLastError() == ERROR_ALREADY_EXISTS;
}

std::optional<std::string> Protect(const Bytes& plain) {
  DATA_BLOB in = {static_cast<DWORD>(plain.size()),
                  const_cast<BYTE*>(plain.data())};
  DATA_BLOB out = {};
  if (!::CryptProtectData(&in, L"PhiSyncDeviceKey", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
    return std::nullopt;
  }
  std::string result(reinterpret_cast<char*>(out.pbData), out.cbData);
  ::LocalFree(out.pbData);
  return result;
}

std::optional<Bytes> Unprotect(const std::string& blob) {
  DATA_BLOB in = {static_cast<DWORD>(blob.size()),
                  reinterpret_cast<BYTE*>(const_cast<char*>(blob.data()))};
  DATA_BLOB out = {};
  if (!::CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
    return std::nullopt;
  }
  const std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
  ::SecureZeroMemory(out.pbData, out.cbData);
  ::LocalFree(out.pbData);
  return Bytes(plain.begin(), plain.end());
}

constexpr char kMappingsKey[] = "sync.profileGlobalUuids";
constexpr char kSpaceMappingsKey[] = "sync.spaceGlobalUuids";
constexpr char kEnrollmentKey[] = "sync.pairingEnrollment";
constexpr char kPendingPrefix[] = "phi.sync.pendingDeviceRegistration.";

}  // namespace

// --- PhiKeyApiClient -----------------------------------------------------------

PhiKeyApiClient::PhiKeyApiClient(std::string base_url,
                                 std::function<std::string()> token,
                                 std::function<void()> invalidate_token)
    : base_url_(std::move(base_url)),
      token_(std::move(token)),
      invalidate_token_(std::move(invalidate_token)) {}

PhiKeyApiClient::~PhiKeyApiClient() = default;

KeyStatus PhiKeyApiClient::Send(const wchar_t* method,
                                const std::string& path,
                                const base::DictValue* body,
                                Response* response) {
  std::string payload;
  if (body && !base::JSONWriter::Write(*body, &payload)) {
    return KeyStatus::Error(KeyErrorKind::kDecode, "encode body");
  }
  const bool is_get = std::wstring(method) == L"GET";
  for (int attempt = 0; attempt < 2; ++attempt) {
    // No token, no request: an empty bearer is answered 401, which callers
    // read as a sign-out.
    const std::string token = token_ ? token_() : std::string();
    if (token.empty()) {
      return KeyStatus::Error(KeyErrorKind::kNoToken);
    }
    std::wstring headers = L"Authorization: Bearer " +
                           base::UTF8ToWide(token) + L"\r\n";
    if (is_get) {
      headers += L"Cache-Control: no-cache\r\n";
    }
    if (body) {
      headers += L"Content-Type: application/json\r\n";
    }
    const PhiHttpResult result =
        PhiHttpRequest(method, base_url_ + path, headers, payload);
    if (!result.transport_ok) {
      return KeyStatus::Error(KeyErrorKind::kTransport);
    }
    if (result.status == 401 && attempt == 0 && invalidate_token_) {
      invalidate_token_();  // a stale cached token: renew and retry once
      continue;
    }
    response->status = result.status;
    response->body = result.body;
    return KeyStatus::Ok();
  }
  return KeyStatus::Ok();
}

KeyStatus PhiKeyApiClient::ListDevices(std::vector<keys::AccountDevice>* out) {
  Response r;
  KeyStatus status = Send(L"GET", "/keys/v1/devices", nullptr, &r);
  if (!status.ok()) {
    return status;
  }
  if (r.status != 200) {
    return Unexpected(r.status, r.body);
  }
  std::optional<base::Value> json = ParseJson(r.body);
  if (!json || !json->is_list()) {
    return KeyStatus::Error(KeyErrorKind::kDecode);
  }
  out->clear();
  for (const base::Value& item : json->GetList()) {
    const base::DictValue* dict = item.GetIfDict();
    keys::AccountDevice device;
    if (!dict || !ReadString(*dict, "device_key_id", &device.device_key_id) ||
        device.device_key_id.empty() || !ReadString(*dict, "name", &device.name) ||
        !ReadString(*dict, "platform", &device.platform) ||
        !ReadString(*dict, "status", &device.status) ||
        !ReadTime(*dict, "created_at", &device.created_at_ms)) {
      return KeyStatus::Error(KeyErrorKind::kDecode);
    }
    if (const std::string* revoked = dict->FindString("revoked_at")) {
      device.revoked_at_ms = PhiParseRfc3339Ms(*revoked);
      if (!device.revoked_at_ms) {
        return KeyStatus::Error(KeyErrorKind::kDecode);
      }
    }
    out->push_back(std::move(device));
  }
  return KeyStatus::Ok();
}

KeyStatus PhiKeyApiClient::PutAccount(const Bytes& salt,
                                      const std::string& kdf_version,
                                      const std::string& kdf_params_json,
                                      const Bytes& recovery_envelope,
                                      bool* created) {
  base::DictValue body;
  body.Set("recovery_salt", keys::Base64Encode(salt));
  body.Set("kdf_version", kdf_version);
  std::optional<base::Value> params = ParseJson(kdf_params_json);
  body.Set("kdf_params", params ? std::move(*params)
                                : base::Value(base::DictValue()));
  body.Set("recovery_ark_envelope", keys::Base64Encode(recovery_envelope));
  Response r;
  KeyStatus status = Send(L"PUT", "/keys/v1/account", &body, &r);
  if (!status.ok()) {
    return status;
  }
  if (r.status == 200 || r.status == 204) {
    *created = true;
    return KeyStatus::Ok();
  }
  if (r.status == 409) {
    *created = false;
    return KeyStatus::Ok();
  }
  return Unexpected(r.status, r.body);
}

KeyStatus PhiKeyApiClient::GetAccount(
    std::optional<keys::AccountKeyState>* out) {
  Response r;
  KeyStatus status = Send(L"GET", "/keys/v1/account", nullptr, &r);
  if (!status.ok()) {
    return status;
  }
  if (r.status == 404) {
    out->reset();
    return KeyStatus::Ok();
  }
  if (r.status != 200) {
    return Unexpected(r.status, r.body);
  }
  std::optional<base::Value> json = ParseJson(r.body);
  const base::DictValue* dict = json ? json->GetIfDict() : nullptr;
  keys::AccountKeyState state;
  const base::Value* params = dict ? dict->Find("kdf_params") : nullptr;
  const std::optional<int> generation =
      dict ? dict->FindInt("ark_generation") : std::nullopt;
  if (!dict || !ReadBase64(*dict, "recovery_salt", &state.recovery_salt) ||
      !ReadString(*dict, "kdf_version", &state.kdf_version) || !params ||
      !ReadBase64(*dict, "recovery_ark_envelope",
                  &state.recovery_ark_envelope) ||
      !generation) {
    return KeyStatus::Error(KeyErrorKind::kDecode);
  }
  base::JSONWriter::Write(*params, &state.kdf_params_json);
  state.ark_generation = *generation;
  *out = std::move(state);
  return KeyStatus::Ok();
}

KeyStatus PhiKeyApiClient::PostDevice(const std::string& device_key_id,
                                      const Bytes& public_key,
                                      const std::string& name,
                                      const std::string& platform,
                                      const Bytes& ark_envelope) {
  base::DictValue body;
  body.Set("device_key_id", device_key_id);
  body.Set("public_key", keys::Base64Encode(public_key));
  body.Set("name", name);
  body.Set("platform", platform);
  if (!ark_envelope.empty()) {
    body.Set("ark_envelope", keys::Base64Encode(ark_envelope));
  }
  Response r;
  KeyStatus status = Send(L"POST", "/keys/v1/devices", &body, &r);
  if (!status.ok()) {
    return status;
  }
  if (r.status == 200 || r.status == 204) {
    return KeyStatus::Ok();
  }
  return Unexpected(r.status, r.body);
}

KeyStatus PhiKeyApiClient::GetDeviceEnvelope(const std::string& device_key_id,
                                             std::optional<Bytes>* out) {
  Response r;
  KeyStatus status = Send(
      L"GET", "/keys/v1/devices/" + device_key_id + "/envelope", nullptr, &r);
  if (!status.ok()) {
    return status;
  }
  if (r.status == 404) {
    out->reset();
    return KeyStatus::Ok();
  }
  if (r.status != 200) {
    return Unexpected(r.status, r.body);
  }
  std::optional<base::Value> json = ParseJson(r.body);
  const base::DictValue* dict = json ? json->GetIfDict() : nullptr;
  Bytes envelope;
  if (!dict || !ReadBase64(*dict, "ark_envelope", &envelope)) {
    return KeyStatus::Error(KeyErrorKind::kDecode);
  }
  *out = std::move(envelope);
  return KeyStatus::Ok();
}

KeyStatus PhiKeyApiClient::RevokeDevice(const std::string& device_key_id) {
  Response r;
  KeyStatus status = Send(
      L"POST", "/keys/v1/devices/" + device_key_id + "/revoke", nullptr, &r);
  if (!status.ok()) {
    return status;
  }
  if (r.status == 200 || r.status == 204) {
    return KeyStatus::Ok();
  }
  // 409 means the last active device ONLY with the body code `last_device`
  // (the server's locked decision; the client never pre-probes the device
  // count). Any other 409 is an ordinary failure.
  if (r.status == 409 && ErrorCode(r.body) == "last_device") {
    return KeyStatus::Error(KeyErrorKind::kLastActiveDevice);
  }
  return Unexpected(r.status, r.body);
}

KeyStatus PhiKeyApiClient::ResetAccount(const std::string& operation_id,
                                        const std::string& device_key_id) {
  // The server keeps no receipt; a retry with the same operation id is
  // answered from the account's state, so a retry after a lost success
  // answers `reset` again. 409 not_last_device (another device is active)
  // and 404 device_not_found (the account was re-enrolled without this
  // device) deleted nothing.
  base::DictValue body;
  body.Set("device_key_id", device_key_id);
  Response r;
  KeyStatus status = Send(
      L"PUT", "/keys/v1/reset-operations/" + base::ToLowerASCII(operation_id),
      &body, &r);
  if (!status.ok()) {
    return status;
  }
  if (r.status == 200) {
    std::optional<base::Value> json = ParseJson(r.body);
    const base::DictValue* dict = json ? json->GetIfDict() : nullptr;
    const std::string* outcome = dict ? dict->FindString("outcome") : nullptr;
    return outcome && *outcome == "reset"
               ? KeyStatus::Ok()
               : KeyStatus::Error(KeyErrorKind::kDecode);
  }
  const std::string code = ErrorCode(r.body);
  if (r.status == 409 && code == "not_last_device") {
    return KeyStatus::Error(KeyErrorKind::kNotLastDevice);
  }
  if (r.status == 404 && code == "device_not_found") {
    return KeyStatus::Error(KeyErrorKind::kDeviceNotFound);
  }
  return Unexpected(r.status, r.body);
}

KeyStatus PhiKeyApiClient::PostJoinRequest(const Bytes& public_key,
                                           const std::string& name,
                                           const std::string& platform,
                                           std::string* request_id) {
  base::DictValue body;
  body.Set("requesting_public_key", keys::Base64Encode(public_key));
  body.Set("name", name);
  body.Set("platform", platform);
  Response r;
  KeyStatus status = Send(L"POST", "/keys/v1/join-requests", &body, &r);
  if (!status.ok()) {
    return status;
  }
  if (r.status != 200 && r.status != 204) {
    return JoinError(r.status, r.body);
  }
  std::optional<base::Value> json = ParseJson(r.body);
  const base::DictValue* dict = json ? json->GetIfDict() : nullptr;
  if (!dict || !ReadString(*dict, "request_id", request_id)) {
    return KeyStatus::Error(KeyErrorKind::kDecode);
  }
  return KeyStatus::Ok();
}

KeyStatus PhiKeyApiClient::ListPendingJoinRequests(
    std::vector<keys::JoinRequestSummary>* out) {
  Response r;
  KeyStatus status =
      Send(L"GET", "/keys/v1/join-requests?status=pending", nullptr, &r);
  if (!status.ok()) {
    return status;
  }
  if (r.status != 200) {
    return JoinError(r.status, r.body);
  }
  std::optional<base::Value> json = ParseJson(r.body);
  if (!json || !json->is_list()) {
    return KeyStatus::Error(KeyErrorKind::kDecode);
  }
  out->clear();
  for (const base::Value& item : json->GetList()) {
    keys::JoinRequestSummary summary;
    if (!item.is_dict() || !ReadSummary(item.GetDict(), &summary)) {
      return KeyStatus::Error(KeyErrorKind::kDecode);
    }
    out->push_back(std::move(summary));
  }
  return KeyStatus::Ok();
}

KeyStatus PhiKeyApiClient::GetJoinRequest(const std::string& request_id,
                                          keys::JoinRequest* out) {
  Response r;
  KeyStatus status =
      Send(L"GET", "/keys/v1/join-requests/" + request_id, nullptr, &r);
  if (!status.ok()) {
    return status;
  }
  if (r.status != 200) {
    return JoinError(r.status, r.body);
  }
  std::optional<base::Value> json = ParseJson(r.body);
  const base::DictValue* dict = json ? json->GetIfDict() : nullptr;
  if (!dict || !ReadSummary(*dict, out)) {
    return KeyStatus::Error(KeyErrorKind::kDecode);
  }
  // Always present, "" until approved.
  const std::string* granted = dict->FindString("granted_ark_envelope");
  out->granted_ark_envelope.clear();
  if (granted && !granted->empty()) {
    std::optional<Bytes> envelope = keys::Base64Decode(*granted);
    if (!envelope) {
      return KeyStatus::Error(KeyErrorKind::kDecode);
    }
    out->granted_ark_envelope = std::move(*envelope);
  }
  const std::string* resolved = dict->FindString("resolved_by_device_key_id");
  out->resolved_by_device_key_id = resolved ? *resolved : std::string();
  return KeyStatus::Ok();
}

KeyStatus PhiKeyApiClient::ApproveJoinRequest(const std::string& request_id,
                                              const Bytes& granted_ark_envelope,
                                              const std::string& resolved_by) {
  base::DictValue body;
  body.Set("granted_ark_envelope", keys::Base64Encode(granted_ark_envelope));
  body.Set("resolved_by_device_key_id", resolved_by);
  Response r;
  KeyStatus status = Send(
      L"POST", "/keys/v1/join-requests/" + request_id + "/approve", &body, &r);
  if (!status.ok()) {
    return status;
  }
  if (r.status != 200 && r.status != 204) {
    return JoinError(r.status, r.body);
  }
  return KeyStatus::Ok();
}

KeyStatus PhiKeyApiClient::DenyJoinRequest(const std::string& request_id) {
  Response r;
  KeyStatus status = Send(
      L"POST", "/keys/v1/join-requests/" + request_id + "/deny", nullptr, &r);
  if (!status.ok()) {
    return status;
  }
  if (r.status != 200 && r.status != 204) {
    return JoinError(r.status, r.body);
  }
  return KeyStatus::Ok();
}

KeyStatus PhiKeyApiClient::ListProfiles(
    std::vector<keys::ProfileSummary>* out) {
  Response r;
  KeyStatus status = Send(L"GET", "/keys/v1/profiles", nullptr, &r);
  if (!status.ok()) {
    return status;
  }
  if (r.status != 200) {
    return Unexpected(r.status, r.body);
  }
  std::optional<base::Value> json = ParseJson(r.body);
  if (!json || !json->is_list()) {
    return KeyStatus::Error(KeyErrorKind::kDecode);
  }
  out->clear();
  for (const base::Value& item : json->GetList()) {
    const base::DictValue* dict = item.GetIfDict();
    keys::ProfileSummary summary;
    const std::optional<bool> has =
        dict ? dict->FindBool("has_envelope") : std::nullopt;
    if (!dict || !ReadString(*dict, "profile_uuid", &summary.profile_uuid) ||
        !has || !ReadTime(*dict, "created_at", &summary.created_at_ms)) {
      return KeyStatus::Error(KeyErrorKind::kDecode);
    }
    summary.has_envelope = *has;
    out->push_back(std::move(summary));
  }
  return KeyStatus::Ok();
}

KeyStatus PhiKeyApiClient::GetProfileKey(const std::string& uuid,
                                         std::optional<Bytes>* envelope) {
  Response r;
  KeyStatus status = Send(L"GET", "/keys/v1/profiles/" + uuid, nullptr, &r);
  if (!status.ok()) {
    return status;
  }
  if (r.status == 404) {
    envelope->reset();
    return KeyStatus::Ok();
  }
  if (r.status != 200) {
    return Unexpected(r.status, r.body);
  }
  std::optional<base::Value> json = ParseJson(r.body);
  const base::DictValue* dict = json ? json->GetIfDict() : nullptr;
  Bytes bytes;
  if (!dict || !ReadBase64(*dict, "profile_key_envelope", &bytes)) {
    return KeyStatus::Error(KeyErrorKind::kDecode);
  }
  *envelope = std::move(bytes);
  return KeyStatus::Ok();
}

KeyStatus PhiKeyApiClient::PutProfileKey(const std::string& uuid,
                                         const Bytes& envelope,
                                         bool* created) {
  base::DictValue body;
  body.Set("profile_key_envelope", keys::Base64Encode(envelope));
  Response r;
  KeyStatus status = Send(L"PUT", "/keys/v1/profiles/" + uuid, &body, &r);
  if (!status.ok()) {
    return status;
  }
  if (r.status == 200 || r.status == 204) {
    *created = true;
    return KeyStatus::Ok();
  }
  if (r.status == 409) {
    *created = false;
    return KeyStatus::Ok();
  }
  return Unexpected(r.status, r.body);
}

KeyStatus PhiKeyApiClient::ReplaceProfileKeyEnvelope(const std::string& uuid,
                                                     const Bytes& expected,
                                                     const Bytes& envelope,
                                                     bool* replaced) {
  base::DictValue body;
  body.Set("expected_profile_key_envelope", keys::Base64Encode(expected));
  body.Set("profile_key_envelope", keys::Base64Encode(envelope));
  Response r;
  KeyStatus status = Send(L"PATCH", "/keys/v1/profiles/" + uuid, &body, &r);
  if (!status.ok()) {
    return status;
  }
  if (r.status == 200 || r.status == 204) {
    *replaced = true;
    return KeyStatus::Ok();
  }
  if (r.status == 409) {
    *replaced = false;  // changed underneath: re-read and merge again
    return KeyStatus::Ok();
  }
  return Unexpected(r.status, r.body);
}

KeyStatus PhiKeyApiClient::GetDomainKey(const std::string& domain,
                                        std::optional<Bytes>* envelope) {
  Response r;
  KeyStatus status = Send(L"GET", "/keys/v1/domains/" + domain, nullptr, &r);
  if (!status.ok()) {
    return status;
  }
  if (r.status == 404) {
    envelope->reset();
    return KeyStatus::Ok();
  }
  if (r.status != 200) {
    return Unexpected(r.status, r.body);
  }
  std::optional<base::Value> json = ParseJson(r.body);
  const base::DictValue* dict = json ? json->GetIfDict() : nullptr;
  Bytes bytes;
  if (!dict || !ReadBase64(*dict, "domain_key_envelope", &bytes)) {
    return KeyStatus::Error(KeyErrorKind::kDecode);
  }
  *envelope = std::move(bytes);
  return KeyStatus::Ok();
}

KeyStatus PhiKeyApiClient::PutDomainKey(const std::string& domain,
                                        const Bytes& envelope,
                                        bool* created) {
  base::DictValue body;
  body.Set("domain_key_envelope", keys::Base64Encode(envelope));
  Response r;
  KeyStatus status = Send(L"PUT", "/keys/v1/domains/" + domain, &body, &r);
  if (!status.ok()) {
    return status;
  }
  if (r.status == 200 || r.status == 201 || r.status == 204) {
    *created = true;
    return KeyStatus::Ok();
  }
  if (r.status == 409) {
    *created = false;
    return KeyStatus::Ok();
  }
  return Unexpected(r.status, r.body);
}

// --- PhiDpapiDeviceKeyStore ----------------------------------------------------

PhiDpapiDeviceKeyStore::PhiDpapiDeviceKeyStore(std::wstring path)
    : path_(std::move(path)) {}

PhiDpapiDeviceKeyStore::~PhiDpapiDeviceKeyStore() = default;

KeyStatus PhiDpapiDeviceKeyStore::Write(const Bytes& private_key) {
  const std::optional<std::string> blob = Protect(private_key);
  if (!blob || !EnsureDirectoryFor(path_) || !PhiWriteFileAtomic(path_, *blob)) {
    return KeyStatus::Error(KeyErrorKind::kDeviceKey, "write");
  }
  return KeyStatus::Ok();
}

KeyStatus PhiDpapiDeviceKeyStore::LoadOrCreate(keys::DeviceKeyPair* out) {
  std::lock_guard<std::mutex> lock(lock_);
  if (cached_) {
    *out = *cached_;
    return KeyStatus::Ok();
  }
  std::string blob;
  const PhiReadStatus read = PhiReadFile(path_, &blob);
  if (read == PhiReadStatus::kError) {
    // There but locked or denied: the next call retries; a new key would
    // replace the device's registered identity.
    return KeyStatus::Error(KeyErrorKind::kDeviceKey, "read");
  }
  if (read == PhiReadStatus::kOk) {
    std::optional<Bytes> private_key = Unprotect(blob);
    if (!private_key || private_key->size() != keys::kKeyBytes) {
      // Unreadable (another machine or user, or damage): never mint over
      // it silently — the file may be the account's only registered
      // identity for this device.
      LOG(ERROR) << "[PhiSyncKeys] device key unreadable";
      return KeyStatus::Error(KeyErrorKind::kDeviceKey, "unreadable");
    }
    keys::DeviceKeyPair pair;
    pair.private_key = std::move(*private_key);
    pair.public_key = keys::PublicKeyFor(pair.private_key);
    cached_ = pair;
    *out = pair;
    return KeyStatus::Ok();
  }
  const keys::DeviceKeyPair pair = keys::GenerateDeviceKeyPair();
  KeyStatus status = Write(pair.private_key);
  if (!status.ok()) {
    return status;
  }
  cached_ = pair;
  *out = pair;
  return KeyStatus::Ok();
}

KeyStatus PhiDpapiDeviceKeyStore::Rotate() {
  std::lock_guard<std::mutex> lock(lock_);
  const keys::DeviceKeyPair pair = keys::GenerateDeviceKeyPair();
  KeyStatus status = Write(pair.private_key);
  if (status.ok()) {
    cached_ = pair;
  }
  return status;
}

// --- PhiKeyStateFile -----------------------------------------------------------

PhiKeyStateFile::PhiKeyStateFile(std::wstring path) : path_(std::move(path)) {
  int schema = 0;
  if (PhiReadJsonFile(path_, &values_, &schema) ==
      PhiJsonFileStatus::kUnreadable) {
    // Locked or denied: empty in memory, and no write may replace the
    // mappings on disk this session (every Set* then fails, so sync parks).
    save_blocked_ = true;
    LOG(ERROR) << "[PhiSyncKeys] key state could not be read; saves off";
  }
}

PhiKeyStateFile::~PhiKeyStateFile() = default;

bool PhiKeyStateFile::SaveLocked() {
  if (save_blocked_) {
    return false;
  }
  std::string json;
  if (!base::JSONWriter::WriteWithOptions(
          values_, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json)) {
    return false;
  }
  return EnsureDirectoryFor(path_) && PhiWriteFileAtomic(path_, json);
}

bool PhiKeyStateFile::SetAndSaveLocked(std::string_view key,
                                       base::DictValue value) {
  // A write that did not land leaves memory as the disk has it: a mapping
  // must never be used before it is durable.
  std::optional<base::Value> before;
  if (const base::Value* old = values_.Find(key)) {
    before = old->Clone();
  }
  values_.Set(key, std::move(value));
  if (SaveLocked()) {
    return true;
  }
  if (before) {
    values_.Set(key, std::move(*before));
  } else {
    values_.Remove(key);
  }
  return false;
}

std::map<std::string, std::string> PhiKeyStateFile::ProfileMappings() const {
  std::lock_guard<std::mutex> lock(lock_);
  std::map<std::string, std::string> out;
  if (const base::DictValue* dict = values_.FindDict(kMappingsKey)) {
    for (const auto [key, value] : *dict) {
      if (value.is_string()) {
        out[key] = value.GetString();
      }
    }
  }
  return out;
}

bool PhiKeyStateFile::SetProfileMappings(
    const std::map<std::string, std::string>& mappings) {
  std::lock_guard<std::mutex> lock(lock_);
  base::DictValue dict;
  for (const auto& [key, value] : mappings) {
    dict.Set(key, value);
  }
  return SetAndSaveLocked(kMappingsKey, std::move(dict));
}

std::map<std::string, std::string> PhiKeyStateFile::SpaceMappings() const {
  std::lock_guard<std::mutex> lock(lock_);
  std::map<std::string, std::string> out;
  if (const base::DictValue* dict = values_.FindDict(kSpaceMappingsKey)) {
    for (const auto [key, value] : *dict) {
      if (value.is_string()) {
        out[key] = value.GetString();
      }
    }
  }
  return out;
}

bool PhiKeyStateFile::SetSpaceMappings(
    const std::map<std::string, std::string>& mappings) {
  std::lock_guard<std::mutex> lock(lock_);
  base::DictValue dict;
  for (const auto& [key, value] : mappings) {
    dict.Set(key, value);
  }
  return SetAndSaveLocked(kSpaceMappingsKey, std::move(dict));
}

std::optional<Bytes> PhiKeyStateFile::PendingRegistration(
    const std::string& device_key_id) const {
  std::lock_guard<std::mutex> lock(lock_);
  const std::string* text =
      values_.FindString(std::string(kPendingPrefix) + device_key_id);
  return text ? keys::Base64Decode(*text) : std::nullopt;
}

void PhiKeyStateFile::SetPendingRegistration(const std::string& device_key_id,
                                             const Bytes& envelope) {
  std::lock_guard<std::mutex> lock(lock_);
  values_.Set(std::string(kPendingPrefix) + device_key_id,
              keys::Base64Encode(envelope));
  SaveLocked();
}

void PhiKeyStateFile::ClearPendingRegistration(
    const std::string& device_key_id) {
  std::lock_guard<std::mutex> lock(lock_);
  if (values_.Remove(std::string(kPendingPrefix) + device_key_id)) {
    SaveLocked();
  }
}

std::optional<keys::EnrollmentRecord> PhiKeyStateFile::Enrollment() const {
  std::lock_guard<std::mutex> lock(lock_);
  // The Mac stores the record's JSON; here it is the same object inline.
  // Anything malformed reads as no record (unpaired).
  const base::DictValue* dict = values_.FindDict(kEnrollmentKey);
  if (!dict) {
    return std::nullopt;
  }
  const std::optional<int> version = dict->FindInt("version");
  const std::string* id = dict->FindString("deviceKeyID");
  const std::optional<bool> paired = dict->FindBool("paired");
  if (!version || !id || !paired) {
    return std::nullopt;
  }
  return keys::EnrollmentRecord{*version, *id, *paired};
}

bool PhiKeyStateFile::SetEnrollment(const keys::EnrollmentRecord& record) {
  std::lock_guard<std::mutex> lock(lock_);
  base::DictValue dict;
  dict.Set("version", record.version);
  dict.Set("deviceKeyID", record.device_key_id);
  dict.Set("paired", record.paired);
  return SetAndSaveLocked(kEnrollmentKey, std::move(dict));
}

// --- PhiProfilePayloadJson -----------------------------------------------------

std::string PhiProfilePayloadJson::Encode(const Bytes& key,
                                          const std::string& name,
                                          std::optional<bool> is_default) const {
  base::DictValue dict;
  dict.Set("v", 1);
  dict.Set("key", keys::Base64Encode(key));
  dict.Set("name", name);
  if (is_default) {
    dict.Set("default", *is_default);
  }
  std::string json;
  base::JSONWriter::Write(dict, &json);
  return json;
}

std::string PhiProfilePayloadJson::WithDefaultFlag(const std::string& json,
                                                   bool is_default) const {
  std::optional<base::Value> value = ParseJson(json);
  base::DictValue* dict = value ? value->GetIfDict() : nullptr;
  if (!dict) {
    return std::string();
  }
  dict->Set("default", is_default);
  std::string out;
  base::JSONWriter::Write(*dict, &out);
  return out;
}

bool PhiProfilePayloadJson::Decode(const std::string& json,
                                   Bytes* key,
                                   std::string* name,
                                   std::optional<bool>* is_default) const {
  std::optional<base::Value> value = ParseJson(json);
  const base::DictValue* dict = value ? value->GetIfDict() : nullptr;
  // The Mac's `obj["v"] as? Int == 1`: a JSON 1 (or 1.0) only.
  const std::optional<double> v = dict ? dict->FindDouble("v") : std::nullopt;
  if (!v || *v != 1.0 || !ReadBase64(*dict, "key", key)) {
    return false;
  }
  const std::string* text = dict->FindString("name");
  *name = text ? *text : std::string();
  // Tri-state, as the Mac's `obj["default"] as? Bool`: absent = unknown.
  *is_default = dict->FindBool("default");
  return true;
}

// --- PhiSyncHttpTransport ---------------------------------------------------------

PhiSyncHttpTransport::PhiSyncHttpTransport(std::string base_url,
                                           std::function<std::string()> token,
                                           std::function<void()> invalidate_token)
    : base_url_(std::move(base_url)),
      token_(std::move(token)),
      invalidate_token_(std::move(invalidate_token)) {}

PhiSyncHttpTransport::~PhiSyncHttpTransport() = default;

sync::ProtocolStatus PhiSyncHttpTransport::Post(
    const std::string& client_id,
    const std::string& body,
    int* status,
    std::string* response,
    std::optional<int64_t>* server_date_ms) {
  sync::ProtocolStatus result;
  std::string query;
  for (const char c : client_id) {
    // device key ids are base64url, but encode defensively.
    const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                       c == '.' || c == '~';
    if (plain) {
      query.push_back(c);
    } else {
      static constexpr char kHex[] = "0123456789ABCDEF";
      query.push_back('%');
      query.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 15]);
      query.push_back(kHex[static_cast<unsigned char>(c) & 15]);
    }
  }
  const std::string url =
      base_url_ + "/chromium-sync/phi/command/?client_id=" + query;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const std::string token = token_ ? token_() : std::string();
    if (token.empty()) {
      result.error = sync::ProtocolError::kNoToken;
      return result;
    }
    const std::wstring headers = L"Authorization: Bearer " +
                                 base::UTF8ToWide(token) +
                                 L"\r\nContent-Type: application/octet-stream\r\n";
    // A replay's GetUpdates page can carry far more than the default cap.
    constexpr size_t kSyncResponseMaxBytes = 64u * 1024 * 1024;
    PhiHttpResult http =
        PhiHttpRequest(L"POST", url, headers, body, kSyncResponseMaxBytes);
    if (!http.transport_ok) {
      result.error = sync::ProtocolError::kTransport;
      return result;
    }
    if (http.status == 401 && attempt == 0 && invalidate_token_) {
      invalidate_token_();
      continue;
    }
    *status = http.status;
    *response = std::move(http.body);
    *server_date_ms = http.server_date_ms;
    return result;
  }
  return result;
}

// --- Helpers -------------------------------------------------------------------

keys::DeviceIdentity PhiThisDeviceIdentity() {
  wchar_t buffer[256] = {};
  DWORD size = static_cast<DWORD>(std::size(buffer));
  std::string name = "Windows PC";
  if (::GetComputerNameExW(ComputerNamePhysicalDnsHostname, buffer, &size) &&
      size > 0) {
    name = base::WideToUTF8(std::wstring(buffer, size));
  }
  return keys::DeviceIdentity{name, "windows"};
}

std::optional<int64_t> PhiParseRfc3339Ms(const std::string& text) {
  // YYYY-MM-DDTHH:MM:SS[.fraction](Z|±HH:MM) — Go's RFC3339Nano included.
  size_t pos = 0;
  // `count` digits at pos, then (when non-zero) the separator `after`.
  auto field = [&](size_t count, char after, int* out) {
    int value = 0;
    for (size_t i = 0; i < count; ++i, ++pos) {
      if (pos >= text.size() || text[pos] < '0' || text[pos] > '9') {
        return false;
      }
      value = value * 10 + (text[pos] - '0');
    }
    *out = value;
    if (after) {
      const bool date_time = after == 'T';
      if (pos >= text.size() ||
          (date_time ? (text[pos] != 'T' && text[pos] != 't' &&
                        text[pos] != ' ')
                     : text[pos] != after)) {
        return false;
      }
      ++pos;
    }
    return true;
  };
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (!field(4, '-', &year) || !field(2, '-', &month) ||
      !field(2, 'T', &day) || !field(2, ':', &hour) ||
      !field(2, ':', &minute) || !field(2, 0, &second)) {
    return std::nullopt;
  }
  int64_t millis = 0;
  if (pos < text.size() && text[pos] == '.') {
    ++pos;
    int digits = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
      if (digits < 3) {
        millis = millis * 10 + (text[pos] - '0');
      }
      ++digits;
      ++pos;
    }
    if (digits == 0) {
      return std::nullopt;
    }
    for (; digits < 3; ++digits) {
      millis *= 10;
    }
  }
  int64_t offset_minutes = 0;
  if (pos < text.size() && (text[pos] == 'Z' || text[pos] == 'z')) {
    ++pos;
  } else if (pos + 6 == text.size() &&
             (text[pos] == '+' || text[pos] == '-') && text[pos + 3] == ':') {
    const int sign = text[pos] == '-' ? -1 : 1;
    int hours = 0;
    int minutes = 0;
    ++pos;
    if (!field(2, ':', &hours) || !field(2, 0, &minutes)) {
      return std::nullopt;
    }
    offset_minutes = sign * (hours * 60 + minutes);
  } else {
    return std::nullopt;
  }
  if (pos != text.size()) {
    return std::nullopt;
  }
  base::Time::Exploded exploded = {};
  exploded.year = year;
  exploded.month = month;
  exploded.day_of_month = day;
  exploded.hour = hour;
  exploded.minute = minute;
  exploded.second = second;
  base::Time time;
  if (!base::Time::FromUTCExploded(exploded, &time)) {
    return std::nullopt;
  }
  return time.InMillisecondsSinceUnixEpoch() + millis -
         offset_minutes * 60 * 1000;
}

}  // namespace phi_client

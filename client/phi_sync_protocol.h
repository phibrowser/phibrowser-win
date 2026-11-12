// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_PROTOCOL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_PROTOCOL_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"

// Phi native sync's wire protocol (docs/user-data-parity.md S5): the
// Chromium sync protocol's GetUpdates / Commit for data type 2000, POSTed
// to <sync base>/chromium-sync/phi/command/?client_id=<device key id> —
// phibrowser-mac PhiSyncHTTPClient, with the trimmed sync_pb messages
// (Sources/Sync/Phi/Proto/sync) encoded by hand. Host-clean; the HTTP round
// trip is behind SyncTransport (Windows: WinHTTP, tests: a fake server).
namespace phi_client::sync {

inline constexpr int kPhiDataTypeId = 2000;

// One entity as the server handed it back.
struct RemoteEntity {
  std::string entity_id;
  std::string client_tag_hash;
  int64_t version = 0;
  std::string ciphertext;  // PhiSpecifics.ciphertext; empty for a tombstone
  bool deleted = false;
};

// One entry of a batch commit. No ciphertext + deleted is a tombstone.
struct CommitEntry {
  std::optional<std::string> entity_id;  // nullopt on create
  std::string client_tag_hash;
  // The per-kind plaintext name the server stores ("phi-settings",
  // "phi-space", "phi-bookmark", "phi-pin", "phi-urlrule").
  std::string name;
  std::optional<std::string> ciphertext;
  bool deleted = false;
  int64_t base_version = 0;  // 0 on create
};

// What one entry's response collapses to.
struct CommitOutcome {
  enum class Kind { kApplied, kConflict, kInvalidMessage, kRejected };
  Kind kind = Kind::kRejected;
  std::string entity_id;       // kApplied; kConflict when the server named one
  int64_t version = 0;         // kApplied
  std::optional<int64_t> server_version;  // kConflict
  std::string store_birthday;  // kApplied
  int response_type = 0;       // kRejected: CommitResponse.ResponseType
};

enum class ProtocolError {
  kNone,
  kNoToken,         // no bearer: signed out, another account, renewing
  kTransport,       // offline, DNS, TLS
  kHttp,            // a non-200 status
  kNotMyBirthday,   // the store was reset: every cursor for it is void
  kServer,          // a typed error_code other than SUCCESS
  kMalformed,       // bytes that do not parse, a response count mismatch
};

struct ProtocolStatus {
  ProtocolError error = ProtocolError::kNone;
  int http_status = 0;
  int server_error = 0;  // SyncEnums.ErrorType
  bool ok() const { return error == ProtocolError::kNone; }
};
const char* ProtocolErrorName(ProtocolError error);

struct GetUpdatesResult {
  std::vector<RemoteEntity> entities;
  // The new watermark: the one sent, when the server named none for 2000.
  std::string new_marker;
  std::string store_birthday;
  bool changes_remaining = false;
};

// --- Message codecs (exposed for tests) ------------------------------------------

std::string EncodeGetUpdates(const std::string& marker,
                             const std::string& store_birthday);
std::string EncodeCommit(const std::vector<CommitEntry>& entries,
                         const std::string& cache_guid,
                         const std::string& store_birthday);

struct DecodedResponse {
  bool has_error_code = false;
  int error_code = 0;
  std::string store_birthday;
  bool has_get_updates = false;
  std::vector<RemoteEntity> entities;
  // (data_type_id, token) for every new_progress_marker.
  std::vector<std::pair<int, std::string>> markers;
  int64_t changes_remaining = 0;
  bool has_commit = false;
  struct EntryResponse {
    int response_type = 0;
    std::string id_string;
    bool has_version = false;
    int64_t version = 0;
  };
  std::vector<EntryResponse> entry_responses;
};
bool DecodeResponse(const std::string& bytes, DecodedResponse* out);

// --- The client ------------------------------------------------------------------------

class SyncTransport {
 public:
  virtual ~SyncTransport() = default;
  // One POST of `body` to the command endpoint as `client_id`. False on a
  // transport failure (no status); otherwise `status` and `response`, and
  // `server_date_ms` from the `Date` header when it parsed.
  virtual ProtocolStatus Post(const std::string& client_id,
                              const std::string& body,
                              int* status,
                              std::string* response,
                              std::optional<int64_t>* server_date_ms) = 0;
};

class SyncProtocolClient {
 public:
  // `client_id` is this device's key id (the Mac's deviceKeyIDProvider): the
  // command's client_id and the commit's cache_guid. Asked per request — a
  // revoked-key rotation changes it.
  SyncProtocolClient(SyncTransport& transport,
                     std::function<std::string()> client_id);
  ~SyncProtocolClient();

  SyncProtocolClient(const SyncProtocolClient&) = delete;
  SyncProtocolClient& operator=(const SyncProtocolClient&) = delete;

  ProtocolStatus GetUpdates(const std::string& marker,
                            const std::string& store_birthday,
                            GetUpdatesResult* out);
  // Outcomes pair with `entries` by index; a different count is malformed.
  ProtocolStatus Commit(const std::vector<CommitEntry>& entries,
                        const std::string& store_birthday,
                        std::vector<CommitOutcome>* out);

  // AM-2: the `Date` header of the last well-formed response, epoch ms.
  std::optional<int64_t> last_server_date_ms() const {
    return last_server_date_ms_;
  }

 private:
  ProtocolStatus Send(const std::string& body, DecodedResponse* response);

  RAW_PTR_EXCLUSION SyncTransport& transport_;
  std::function<std::string()> client_id_;
  std::optional<int64_t> last_server_date_ms_;
};

}  // namespace phi_client::sync

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_PROTOCOL_H_

// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_sync_protocol.h"

#include <utility>

#include "client/phi_sync_proto.h"

namespace phi_client::sync {

namespace {

using wire::Reader;
using wire::WireType;

// ClientToServerMessage.Contents.
constexpr int kContentsCommit = 1;
constexpr int kContentsGetUpdates = 2;
// SyncEnums.GetUpdatesOrigin.PERIODIC.
constexpr int kOriginPeriodic = 4;
// ClientToServerMessage.protocol_version's proto2 default.
constexpr int kProtocolVersion = 99;
// SyncEnums.ErrorType.
constexpr int kErrorSuccess = 0;
constexpr int kErrorNotMyBirthday = 2;
// CommitResponse.ResponseType.
constexpr int kResponseSuccess = 1;
constexpr int kResponseConflict = 2;
constexpr int kResponseInvalidMessage = 4;

std::string NewMessage(int contents,
                       int payload_field,
                       const std::string& payload,
                       const std::string& store_birthday) {
  std::string out;
  wire::PutBytesField(&out, 1, "");  // share: required
  wire::PutVarintField(&out, 2, kProtocolVersion);
  wire::PutVarintField(&out, 3, contents);
  wire::PutBytesField(&out, payload_field, payload);
  wire::PutBytesField(&out, 7, store_birthday);
  return out;
}

bool ReadVarint(Reader& reader, WireType type, uint64_t* value) {
  return type == wire::kVarint && reader.Varint(value);
}

bool ReadBytes(Reader& reader, WireType type, std::string* value) {
  std::string_view bytes;
  if (type != wire::kLengthDelimited || !reader.Bytes(&bytes)) {
    return false;
  }
  *value = std::string(bytes);
  return true;
}

// EntitySpecifics → phi (2000) → ciphertext (1).
bool DecodeSpecifics(std::string_view bytes, std::string* ciphertext) {
  Reader reader(bytes);
  while (!reader.done()) {
    int field = 0;
    WireType type = wire::kVarint;
    if (!reader.Next(&field, &type)) {
      return false;
    }
    if (field != kPhiDataTypeId || type != wire::kLengthDelimited) {
      if (!reader.Skip(type, nullptr)) {
        return false;
      }
      continue;
    }
    std::string_view phi;
    if (!reader.Bytes(&phi)) {
      return false;
    }
    Reader inner(phi);
    while (!inner.done()) {
      int inner_field = 0;
      WireType inner_type = wire::kVarint;
      if (!inner.Next(&inner_field, &inner_type)) {
        return false;
      }
      if (inner_field == 1) {
        if (!ReadBytes(inner, inner_type, ciphertext)) {
          return false;
        }
      } else if (!inner.Skip(inner_type, nullptr)) {
        return false;
      }
    }
  }
  return true;
}

bool DecodeEntity(std::string_view bytes, RemoteEntity* out) {
  Reader reader(bytes);
  while (!reader.done()) {
    int field = 0;
    WireType type = wire::kVarint;
    if (!reader.Next(&field, &type)) {
      return false;
    }
    bool ok = true;
    uint64_t number = 0;
    switch (field) {
      case 1:
        ok = ReadBytes(reader, type, &out->entity_id);
        break;
      case 4:
        ok = ReadVarint(reader, type, &number);
        out->version = static_cast<int64_t>(number);
        break;
      case 18:
        ok = ReadVarint(reader, type, &number);
        out->deleted = number != 0;
        break;
      case 21: {
        std::string_view specifics;
        ok = type == wire::kLengthDelimited && reader.Bytes(&specifics) &&
             DecodeSpecifics(specifics, &out->ciphertext);
        break;
      }
      case 23:
        ok = ReadBytes(reader, type, &out->client_tag_hash);
        break;
      default:
        ok = reader.Skip(type, nullptr);
        break;
    }
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool DecodeEntryResponse(std::string_view bytes,
                         DecodedResponse::EntryResponse* out) {
  Reader reader(bytes);
  while (!reader.done()) {
    int field = 0;
    WireType type = wire::kVarint;
    if (!reader.Next(&field, &type)) {
      return false;
    }
    bool ok = true;
    uint64_t number = 0;
    switch (field) {
      case 2:
        ok = ReadVarint(reader, type, &number);
        out->response_type = static_cast<int>(number);
        break;
      case 3:
        ok = ReadBytes(reader, type, &out->id_string);
        break;
      case 6:
        ok = ReadVarint(reader, type, &number);
        out->has_version = ok;
        out->version = static_cast<int64_t>(number);
        break;
      default:
        ok = reader.Skip(type, nullptr);
        break;
    }
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool DecodeCommitResponse(std::string_view bytes, DecodedResponse* out) {
  Reader reader(bytes);
  while (!reader.done()) {
    int field = 0;
    WireType type = wire::kVarint;
    if (!reader.Next(&field, &type)) {
      return false;
    }
    if (field != 1) {
      if (!reader.Skip(type, nullptr)) {
        return false;
      }
      continue;
    }
    // EntryResponse is a proto2 group; accept a nested message too.
    std::string_view body;
    if (type == wire::kStartGroup) {
      if (!reader.Group(field, &body)) {
        return false;
      }
    } else if (type != wire::kLengthDelimited || !reader.Bytes(&body)) {
      return false;
    }
    DecodedResponse::EntryResponse entry;
    if (!DecodeEntryResponse(body, &entry)) {
      return false;
    }
    out->entry_responses.push_back(std::move(entry));
  }
  return true;
}

bool DecodeMarker(std::string_view bytes, std::pair<int, std::string>* out) {
  Reader reader(bytes);
  while (!reader.done()) {
    int field = 0;
    WireType type = wire::kVarint;
    if (!reader.Next(&field, &type)) {
      return false;
    }
    bool ok = true;
    uint64_t number = 0;
    if (field == 1) {
      ok = ReadVarint(reader, type, &number);
      out->first = static_cast<int>(number);
    } else if (field == 2) {
      ok = ReadBytes(reader, type, &out->second);
    } else {
      ok = reader.Skip(type, nullptr);
    }
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool DecodeGetUpdates(std::string_view bytes, DecodedResponse* out) {
  Reader reader(bytes);
  while (!reader.done()) {
    int field = 0;
    WireType type = wire::kVarint;
    if (!reader.Next(&field, &type)) {
      return false;
    }
    bool ok = true;
    std::string_view body;
    uint64_t number = 0;
    switch (field) {
      case 1: {
        RemoteEntity entity;
        ok = type == wire::kLengthDelimited && reader.Bytes(&body) &&
             DecodeEntity(body, &entity);
        if (ok) {
          out->entities.push_back(std::move(entity));
        }
        break;
      }
      case 4:
        ok = ReadVarint(reader, type, &number);
        out->changes_remaining = static_cast<int64_t>(number);
        break;
      case 5: {
        std::pair<int, std::string> marker;
        ok = type == wire::kLengthDelimited && reader.Bytes(&body) &&
             DecodeMarker(body, &marker);
        if (ok) {
          out->markers.push_back(std::move(marker));
        }
        break;
      }
      default:
        ok = reader.Skip(type, nullptr);
        break;
    }
    if (!ok) {
      return false;
    }
  }
  return true;
}

}  // namespace

const char* ProtocolErrorName(ProtocolError error) {
  switch (error) {
    case ProtocolError::kNone:
      return "ok";
    case ProtocolError::kNoToken:
      return "no_token";
    case ProtocolError::kTransport:
      return "transport";
    case ProtocolError::kHttp:
      return "http";
    case ProtocolError::kNotMyBirthday:
      return "not_my_birthday";
    case ProtocolError::kServer:
      return "server";
    case ProtocolError::kMalformed:
      return "malformed";
  }
  return "unknown";
}

std::string EncodeGetUpdates(const std::string& marker,
                             const std::string& store_birthday) {
  std::string progress;
  wire::PutVarintField(&progress, 1, kPhiDataTypeId);
  wire::PutBytesField(&progress, 2, marker);
  std::string get_updates;
  // The server walks only the markers the client sends: without this one
  // the response carries no phi entities at all.
  wire::PutBytesField(&get_updates, 6, progress);
  wire::PutVarintField(&get_updates, 9, kOriginPeriodic);
  return NewMessage(kContentsGetUpdates, 5, get_updates, store_birthday);
}

std::string EncodeCommit(const std::vector<CommitEntry>& entries,
                         const std::string& cache_guid,
                         const std::string& store_birthday) {
  std::string commit;
  for (const CommitEntry& entry : entries) {
    std::string entity;
    if (entry.entity_id) {
      wire::PutBytesField(&entity, 1, *entry.entity_id);
    }
    wire::PutVarintField(&entity, 4, static_cast<uint64_t>(entry.base_version));
    // A per-kind constant name keeps the server's "did anything change"
    // comparison from bumping the version on an identical commit.
    wire::PutBytesField(&entity, 7, entry.name);
    if (entry.deleted) {
      wire::PutVarintField(&entity, 18, 1);
    }
    if (entry.ciphertext) {
      std::string phi;
      wire::PutBytesField(&phi, 1, *entry.ciphertext);
      std::string specifics;
      wire::PutBytesField(&specifics, kPhiDataTypeId, phi);
      wire::PutBytesField(&entity, 21, specifics);
    }
    wire::PutBytesField(&entity, 23, entry.client_tag_hash);
    wire::PutBytesField(&commit, 1, entity);
  }
  wire::PutBytesField(&commit, 2, cache_guid);
  return NewMessage(kContentsCommit, 4, commit, store_birthday);
}

bool DecodeResponse(const std::string& bytes, DecodedResponse* out) {
  Reader reader(bytes);
  while (!reader.done()) {
    int field = 0;
    WireType type = wire::kVarint;
    if (!reader.Next(&field, &type)) {
      return false;
    }
    bool ok = true;
    std::string_view body;
    uint64_t number = 0;
    switch (field) {
      case 1:
        ok = type == wire::kLengthDelimited && reader.Bytes(&body) &&
             DecodeCommitResponse(body, out);
        out->has_commit = ok;
        break;
      case 2:
        ok = type == wire::kLengthDelimited && reader.Bytes(&body) &&
             DecodeGetUpdates(body, out);
        out->has_get_updates = ok;
        break;
      case 4:
        ok = ReadVarint(reader, type, &number);
        out->has_error_code = ok;
        out->error_code = static_cast<int>(number);
        break;
      case 6:
        ok = ReadBytes(reader, type, &out->store_birthday);
        break;
      default:
        ok = reader.Skip(type, nullptr);
        break;
    }
    if (!ok) {
      return false;
    }
  }
  return true;
}

// --- SyncProtocolClient ------------------------------------------------------------------

SyncProtocolClient::SyncProtocolClient(SyncTransport& transport,
                                       std::function<std::string()> client_id)
    : transport_(transport), client_id_(std::move(client_id)) {}

SyncProtocolClient::~SyncProtocolClient() = default;

ProtocolStatus SyncProtocolClient::Send(const std::string& body,
                                        DecodedResponse* response) {
  int status = 0;
  std::string bytes;
  std::optional<int64_t> date;
  ProtocolStatus result =
      transport_.Post(client_id_ ? client_id_() : std::string(), body, &status,
                      &bytes, &date);
  if (!result.ok()) {
    return result;
  }
  if (status != 200) {
    result.error = ProtocolError::kHttp;
    result.http_status = status;
    return result;
  }
  if (!DecodeResponse(bytes, response)) {
    result.error = ProtocolError::kMalformed;
    return result;
  }
  // AM-2: a NOT_MY_BIRTHDAY or typed error is still a well-formed 200 whose
  // Date header is as good an estimate as any.
  last_server_date_ms_ = date;
  if (response->has_error_code && response->error_code != kErrorSuccess) {
    result.error = response->error_code == kErrorNotMyBirthday
                       ? ProtocolError::kNotMyBirthday
                       : ProtocolError::kServer;
    result.server_error = response->error_code;
  }
  return result;
}

ProtocolStatus SyncProtocolClient::GetUpdates(const std::string& marker,
                                              const std::string& store_birthday,
                                              GetUpdatesResult* out) {
  DecodedResponse response;
  ProtocolStatus status = Send(EncodeGetUpdates(marker, store_birthday), &response);
  if (!status.ok()) {
    return status;
  }
  out->entities = std::move(response.entities);
  // No marker for our type means "no new watermark": keep the one sent
  // rather than rewinding to a full resync.
  out->new_marker = marker;
  for (const auto& [type, token] : response.markers) {
    if (type == kPhiDataTypeId) {
      out->new_marker = token;
      break;
    }
  }
  out->store_birthday = response.store_birthday;
  out->changes_remaining = response.changes_remaining > 0;
  return status;
}

ProtocolStatus SyncProtocolClient::Commit(
    const std::vector<CommitEntry>& entries,
    const std::string& store_birthday,
    std::vector<CommitOutcome>* out) {
  out->clear();
  if (entries.empty()) {
    return ProtocolStatus();
  }
  DecodedResponse response;
  ProtocolStatus status = Send(
      EncodeCommit(entries, client_id_ ? client_id_() : std::string(),
                   store_birthday),
      &response);
  if (!status.ok()) {
    return status;
  }
  if (response.entry_responses.size() != entries.size()) {
    status.error = ProtocolError::kMalformed;
    return status;
  }
  for (const DecodedResponse::EntryResponse& entry : response.entry_responses) {
    CommitOutcome outcome;
    switch (entry.response_type) {
      case kResponseSuccess:
        outcome.kind = CommitOutcome::Kind::kApplied;
        outcome.entity_id = entry.id_string;
        outcome.version = entry.version;
        outcome.store_birthday = response.store_birthday;
        break;
      case kResponseConflict:
        outcome.kind = CommitOutcome::Kind::kConflict;
        outcome.entity_id = entry.id_string;  // empty = none named
        if (entry.has_version) {
          outcome.server_version = entry.version;
        }
        break;
      case kResponseInvalidMessage:
        outcome.kind = CommitOutcome::Kind::kInvalidMessage;
        break;
      default:
        outcome.kind = CommitOutcome::Kind::kRejected;
        outcome.response_type = entry.response_type;
        break;
    }
    out->push_back(std::move(outcome));
  }
  return status;
}

}  // namespace phi_client::sync

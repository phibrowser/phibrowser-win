// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_INVALIDATION_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_INVALIDATION_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The sync server's invalidation stream (docs/user-data-parity.md S5) —
// phibrowser-mac PhiSyncInvalidation / PhiSyncInvalidationParser: a
// text/event-stream at <sync base>/sync/invalidations?client_id=<device key
// id> whose events are `ready`, `invalidate` {namespace, data_types,
// source_client_id} and `:` heartbeats. Host-clean.
namespace phi_client::sync {

struct InvalidationHint {
  std::string name_space;  // "chromium:phi" or "chromium:<profile uuid>"
  std::vector<int> data_types;
  std::string source_client_id;

  // The profile uuid of a Chromium sync namespace; nullopt for Phi's.
  std::optional<std::string> profile_uuid() const;
  bool IsValid() const;
};

struct InvalidationEvent {
  enum class Kind { kReady, kInvalidate, kHeartbeat };
  Kind kind = Kind::kHeartbeat;
  InvalidationHint hint;
};

// Incremental SSE parser: feed bytes as they arrive. False on a malformed or
// oversized event (the stream is then dropped and reconnected).
class InvalidationParser {
 public:
  static constexpr size_t kMaximumEventBytes = 16 * 1024;

  InvalidationParser();
  ~InvalidationParser();

  bool Feed(std::string_view bytes, std::vector<InvalidationEvent>* events);

 private:
  bool FinishLine(std::optional<InvalidationEvent>* event);

  std::string line_;
  std::string event_name_;
  std::vector<std::string> data_lines_;
  size_t event_bytes_ = 0;
  bool previous_was_cr_ = false;
  bool first_line_ = true;
};

}  // namespace phi_client::sync

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_INVALIDATION_H_

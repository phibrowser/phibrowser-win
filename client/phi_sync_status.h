// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_STATUS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_STATUS_H_

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// The Sync pane's status card (the Mac's SyncStatusSnapshot.swift +
// ChromiumSyncStatus.decode): one context per sync source — "phi" (the
// native engine) and each Chromium profile — reduced to one summary line.
//
// PURE: STL only, header-only, host-tested (test/model/sync_status_test).

namespace phi_client::sync {

// SyncSummaryPhase (a context's SyncContextPhase is the same set without
// kNotStarted).
enum class SummaryPhase {
  kNotStarted,
  kChecking,
  kInitialSync,
  kSyncing,
  kUpToDate,
  kOffline,
  kNeedsAttention,
};

inline std::optional<SummaryPhase> ContextPhaseFromName(const std::string& n) {
  if (n == "checking") return SummaryPhase::kChecking;
  if (n == "initialSync") return SummaryPhase::kInitialSync;
  if (n == "syncing") return SummaryPhase::kSyncing;
  if (n == "upToDate") return SummaryPhase::kUpToDate;
  if (n == "offline") return SummaryPhase::kOffline;
  if (n == "needsAttention") return SummaryPhase::kNeedsAttention;
  return std::nullopt;
}

// The Mac's SyncSummaryPhase raw values, which feedback reports.
inline const char* SummaryPhaseName(SummaryPhase phase) {
  switch (phase) {
    case SummaryPhase::kNotStarted:
      return "notStarted";
    case SummaryPhase::kChecking:
      return "checking";
    case SummaryPhase::kInitialSync:
      return "initialSync";
    case SummaryPhase::kSyncing:
      return "syncing";
    case SummaryPhase::kUpToDate:
      return "upToDate";
    case SummaryPhase::kOffline:
      return "offline";
    case SummaryPhase::kNeedsAttention:
      return "needsAttention";
  }
  return "notStarted";
}

// SyncContextSnapshot.
struct ContextStatus {
  std::string id;  // "phi" or a Chromium profile id
  SummaryPhase phase = SummaryPhase::kChecking;
  std::optional<int64_t> last_success_ms;
  uint64_t revision = 0;
};

// ChromiumSyncStatus.decode over the framework's payload fields: an
// unknown version or phase reads as checking; a success time in the future
// (beyond a minute of skew) is dropped; upToDate without a time is
// incomplete evidence, so checking.
inline ContextStatus DecodeProfileStatus(const std::string& profile_id,
                                         int version,
                                         const std::string& phase_name,
                                         std::optional<double> last_success_ms,
                                         int64_t now_ms) {
  ContextStatus out;
  out.id = profile_id;
  const std::optional<SummaryPhase> phase = ContextPhaseFromName(phase_name);
  if (version != 1 || !phase) {
    return out;
  }
  if (last_success_ms && *last_success_ms > 0 &&
      *last_success_ms <= static_cast<double>(now_ms + 60'000)) {
    out.last_success_ms = static_cast<int64_t>(*last_success_ms);
  }
  out.phase = *phase == SummaryPhase::kUpToDate && !out.last_success_ms
                  ? SummaryPhase::kChecking
                  : *phase;
  return out;
}

struct StatusSummary {
  SummaryPhase phase = SummaryPhase::kNotStarted;
  std::optional<int64_t> last_success_ms;
};

// SyncStatusSummary.reduce: needsAttention wins; any missing or checking
// context keeps the whole card checking; then offline, initialSync,
// syncing; up to date only when every required context reported a success
// time (the OLDEST of them is shown).
inline StatusSummary ReduceStatus(bool paired,
                                  const std::set<std::string>& required_ids,
                                  const std::vector<ContextStatus>& snapshots) {
  if (!paired) {
    return {};
  }
  std::map<std::string, ContextStatus> newest;
  for (const ContextStatus& snapshot : snapshots) {
    if (!required_ids.count(snapshot.id)) {
      continue;
    }
    const auto old = newest.find(snapshot.id);
    if (old != newest.end() && old->second.revision > snapshot.revision) {
      continue;
    }
    newest[snapshot.id] = snapshot;
  }
  std::set<SummaryPhase> phases;
  std::vector<int64_t> times;
  for (const auto& [id, snapshot] : newest) {
    phases.insert(snapshot.phase);
    if (snapshot.last_success_ms) {
      times.push_back(*snapshot.last_success_ms);
    }
  }
  std::optional<int64_t> last;
  if (!required_ids.empty() && times.size() == required_ids.size()) {
    last = *std::min_element(times.begin(), times.end());
  }
  if (phases.count(SummaryPhase::kNeedsAttention)) {
    return {SummaryPhase::kNeedsAttention, last};
  }
  if (required_ids.empty() || newest.size() != required_ids.size() ||
      phases.count(SummaryPhase::kChecking)) {
    return {SummaryPhase::kChecking, last};
  }
  for (const SummaryPhase phase :
       {SummaryPhase::kOffline, SummaryPhase::kInitialSync,
        SummaryPhase::kSyncing}) {
    if (phases.count(phase)) {
      return {phase, last};
    }
  }
  return {last ? SummaryPhase::kUpToDate : SummaryPhase::kChecking, last};
}

}  // namespace phi_client::sync

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_STATUS_H_

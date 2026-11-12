// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_LAUNCH_METRICS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_LAUNCH_METRICS_H_

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/synchronization/lock.h"
#include "base/values.h"

namespace phi_client::sync {

// The sync fields of the launch-time user_defaults_snapshot (the Mac's
// SyncLaunchAnalytics, its docs/sync.md "Launch snapshot caches"). The snapshot
// is captured before the account and its keys are loaded, so the sync layer
// mirrors what it already learns into a file beside the telemetry config and
// the snapshot reads the mirror. Telemetry only: no sync decision reads it.
// The file holds account Profile uuids, locally; the snapshot reports counts
// and one bool, never a uuid.
//
// The key service's worker writes the counts and the UI thread the rest, hence
// the lock. `epoch()` names one account: ResetForAccountChange moves it on, and
// a late answer for the previous account is dropped.
class PhiSyncLaunchMetrics {
 public:
  // Envelope reads one mapping pass may spend on uuids whose flag was never
  // read, as on the Mac.
  static constexpr size_t kDefaultFlagFillPerPass = 3;

  PhiSyncLaunchMetrics();
  ~PhiSyncLaunchMetrics();

  // Reads the mirror once; later calls do nothing.
  void Load(const std::wstring& user_data_dir);

  uint64_t epoch() const;
  // Sign-out or another account: nothing cached describes the next one.
  void ResetForAccountChange();
  // This device left sync, same account.
  void Reset();

  void RecordPaired(bool paired);
  void RecordActiveDeviceCount(uint64_t epoch, int count);
  // The account's Profile uuids; flags of uuids no longer listed are dropped.
  void RecordAccountProfiles(uint64_t epoch, const std::set<std::string>& uuids);
  void RecordDefaultFlag(uint64_t epoch,
                         const std::string& uuid,
                         bool is_default);

  // Listed uuids whose flag was never read, in uuid order, at most
  // kDefaultFlagFillPerPass, skipping `excluded` (envelopes that do not open,
  // which would otherwise be re-read every pass).
  std::vector<std::string> UuidsNeedingDefaultFlag(
      const std::set<std::string>& listing,
      const std::set<std::string>& excluded) const;

  // sync_enabled always. The counts only while enabled and known: an unknown
  // count is left out, never reported as 0. The default-flag count is known
  // once every listed Profile's flag has been read.
  void AddSnapshotProperties(base::DictValue* properties) const;

 private:
  void ClearLocked();
  void SaveLocked();

  mutable base::Lock lock_;
  std::wstring path_;
  uint64_t epoch_ = 1;
  bool enabled_ = false;
  std::optional<int> active_devices_;
  std::optional<std::set<std::string>> listing_;
  std::map<std::string, bool> flags_;
};

}  // namespace phi_client::sync

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_LAUNCH_METRICS_H_

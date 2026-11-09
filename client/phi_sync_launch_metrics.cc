// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_sync_launch_metrics.h"

#include <algorithm>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "client/phi_file_io.h"

namespace phi_client::sync {

PhiSyncLaunchMetrics::PhiSyncLaunchMetrics() = default;
PhiSyncLaunchMetrics::~PhiSyncLaunchMetrics() = default;

void PhiSyncLaunchMetrics::Load(const std::wstring& user_data_dir) {
  base::AutoLock hold(lock_);
  if (!path_.empty() || user_data_dir.empty()) {
    return;
  }
  path_ = user_data_dir + L"\\PhiSyncLaunchMetrics.json";
  std::string bytes;
  if (PhiReadFile(path_, &bytes) != PhiReadStatus::kOk) {
    return;
  }
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(bytes, base::JSON_PARSE_RFC);
  if (!dict) {
    return;
  }
  enabled_ = dict->FindBool("enabled").value_or(false);
  active_devices_ = dict->FindInt("activeDevices");
  if (const base::ListValue* listing = dict->FindList("accountProfiles")) {
    listing_.emplace();
    for (const base::Value& uuid : *listing) {
      if (uuid.is_string()) {
        listing_->insert(uuid.GetString());
      }
    }
  }
  if (const base::DictValue* flags = dict->FindDict("defaultFlags")) {
    for (const auto [uuid, flag] : *flags) {
      if (flag.is_bool()) {
        flags_[uuid] = flag.GetBool();
      }
    }
  }
}

uint64_t PhiSyncLaunchMetrics::epoch() const {
  base::AutoLock hold(lock_);
  return epoch_;
}

void PhiSyncLaunchMetrics::ResetForAccountChange() {
  base::AutoLock hold(lock_);
  ++epoch_;
  ClearLocked();
}

void PhiSyncLaunchMetrics::Reset() {
  base::AutoLock hold(lock_);
  ClearLocked();
}

void PhiSyncLaunchMetrics::ClearLocked() {
  enabled_ = false;
  active_devices_.reset();
  listing_.reset();
  flags_.clear();
  SaveLocked();
}

void PhiSyncLaunchMetrics::RecordPaired(bool paired) {
  base::AutoLock hold(lock_);
  if (enabled_ != paired) {
    enabled_ = paired;
    SaveLocked();
  }
}

void PhiSyncLaunchMetrics::RecordActiveDeviceCount(uint64_t epoch, int count) {
  base::AutoLock hold(lock_);
  if (epoch == epoch_ && active_devices_ != count) {
    active_devices_ = count;
    SaveLocked();
  }
}

void PhiSyncLaunchMetrics::RecordAccountProfiles(
    uint64_t epoch,
    const std::set<std::string>& uuids) {
  base::AutoLock hold(lock_);
  if (epoch != epoch_) {
    return;
  }
  const size_t flags_before = flags_.size();
  std::erase_if(flags_, [&](const auto& flag) {
    return !uuids.count(flag.first);
  });
  if (listing_ != uuids || flags_.size() != flags_before) {
    listing_ = uuids;
    SaveLocked();
  }
}

void PhiSyncLaunchMetrics::RecordDefaultFlag(uint64_t epoch,
                                             const std::string& uuid,
                                             bool is_default) {
  base::AutoLock hold(lock_);
  if (epoch != epoch_) {
    return;
  }
  const auto known = flags_.find(uuid);
  if (known == flags_.end() || known->second != is_default) {
    flags_[uuid] = is_default;
    SaveLocked();
  }
}

std::vector<std::string> PhiSyncLaunchMetrics::UuidsNeedingDefaultFlag(
    const std::set<std::string>& listing,
    const std::set<std::string>& excluded) const {
  base::AutoLock hold(lock_);
  std::vector<std::string> uuids;
  for (const std::string& uuid : listing) {
    if (uuids.size() < kDefaultFlagFillPerPass && !excluded.count(uuid) &&
        !flags_.count(uuid)) {
      uuids.push_back(uuid);
    }
  }
  return uuids;
}

void PhiSyncLaunchMetrics::AddSnapshotProperties(
    base::DictValue* properties) const {
  base::AutoLock hold(lock_);
  properties->Set("sync_enabled", enabled_);
  if (!enabled_) {
    return;
  }
  if (active_devices_) {
    properties->Set("sync_paired_device_count", *active_devices_);
  }
  if (!listing_) {
    return;
  }
  properties->Set("sync_account_profile_count",
                  static_cast<int>(listing_->size()));
  int flagged = 0;
  for (const std::string& uuid : *listing_) {
    const auto flag = flags_.find(uuid);
    if (flag == flags_.end()) {
      return;  // Not every flag read yet: the count is unknown.
    }
    flagged += flag->second ? 1 : 0;
  }
  properties->Set("sync_default_flagged_profile_count", flagged);
}

void PhiSyncLaunchMetrics::SaveLocked() {
  if (path_.empty()) {
    return;
  }
  base::DictValue dict;
  dict.Set("enabled", enabled_);
  if (active_devices_) {
    dict.Set("activeDevices", *active_devices_);
  }
  if (listing_) {
    base::ListValue listing;
    for (const std::string& uuid : *listing_) {
      listing.Append(uuid);
    }
    dict.Set("accountProfiles", std::move(listing));
  }
  base::DictValue flags;
  for (const auto& [uuid, flag] : flags_) {
    flags.Set(uuid, flag);
  }
  dict.Set("defaultFlags", std::move(flags));
  std::string bytes;
  if (base::JSONWriter::Write(dict, &bytes)) {
    PhiWriteFileAtomic(path_, bytes);
  }
}

}  // namespace phi_client::sync

// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_STORE_WIN_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_STORE_WIN_H_

#include <functional>
#include <mutex>
#include <string>

#include "base/values.h"
#include "client/phi_sync_engine.h"
#include "client/phi_sync_spaces.h"

// Phi native sync's persisted state on Windows (docs/user-data-parity.md
// S5), where the Mac keeps it:
//   marker.json     <account dir>\sync\marker.json — the shared progress
//                   marker and store birthday, BESIDE the per-kind cursor
//                   tables so a restored account folder rolls them back
//                   together (the Mac's M3-4a §2.10);
//   engine state    <UserData>\Phi\syncstate\<account dir name>.json — the
//                   `phi.sync.*` cursor keys, hlcMax included, OUTSIDE the
//                   account folder: logical time must never roll back with
//                   a restore (the Mac keeps them in UserDefaults.standard);
//   spaces.json     <account dir>\sync\spaces.json — the Space cursor table
//                   and the shared drain flags (the Mac's
//                   PhiSpaceSyncState), beside the marker;
//   sidecars        <UserData>\PhiSyncSidecars.json — `<key>.phiSyncTs` /
//                   `<key>.phiSyncVal` per synced setting, device-wide like
//                   the preferences they shadow.
// Each is a JSON object behind a lock, written with write-temp-then-replace.
namespace phi_client {

class PhiSyncMarkerFile : public sync::MarkerStore {
 public:
  explicit PhiSyncMarkerFile(std::wstring path);
  ~PhiSyncMarkerFile() override;

  sync::MarkerFile Load() override;
  bool Save(const sync::MarkerFile& file) override;
  bool Remove();

 private:
  const std::wstring path_;
  std::mutex lock_;
  // The last Load found the file there but unreadable.
  bool save_blocked_ = false;
};

class PhiSyncStateFile : public sync::EngineStateStore {
 public:
  explicit PhiSyncStateFile(std::wstring path);
  ~PhiSyncStateFile() override;

  std::optional<std::string> Get(const std::string& key) const override;
  bool Set(const std::string& key,
           const std::optional<std::string>& value) override;
  bool ClearSyncState();

 private:
  const std::wstring path_;
  mutable std::mutex lock_;
  base::DictValue values_;
  // The file was there but unreadable at construction: never overwritten.
  bool save_blocked_ = false;
};

class PhiSpaceTableFile : public sync::SpaceTableStore {
 public:
  explicit PhiSpaceTableFile(std::wstring path);
  ~PhiSpaceTableFile() override;

  sync::SpaceSyncTable Load() override;
  bool Save(const sync::SpaceSyncTable& table) override;
  bool Remove();
  // Runs after every successful save (worker thread).
  void set_on_saved(std::function<void(const sync::SpaceSyncTable&)> on_saved) {
    on_saved_ = std::move(on_saved);
  }

 private:
  const std::wstring path_;
  std::mutex lock_;
  std::function<void(const sync::SpaceSyncTable&)> on_saved_;
  // The last Load found the file unreadable or a newer build's.
  bool save_blocked_ = false;
};

class PhiSyncSidecarFile : public sync::SettingsSidecars {
 public:
  explicit PhiSyncSidecarFile(std::wstring path);
  ~PhiSyncSidecarFile() override;

  std::optional<int64_t> Timestamp(const std::string& key) const override;
  std::optional<std::string> Value(const std::string& key) const override;
  bool Set(const std::string& key,
           int64_t stamp,
           const std::string& value_signature) override;

  // Free-form device-wide entries beside the sidecars (the registry's HELD
  // values: a peer's value this platform shows through a local stand-in).
  // Self-removal (the Mac's clearLocalSyncState): every `<key>.phiSyncTs` /
  // `.phiSyncVal` pair and the account's default-Space mirror go; other
  // extras stay. False leaves the in-memory state unchanged for retry.
  bool ClearSidecars();

  std::optional<std::string> Extra(const std::string& key) const;
  // False when it did not persist (memory keeps the old value).
  bool SetExtra(const std::string& key, const std::optional<std::string>& value);

 private:
  const std::wstring path_;
  mutable std::mutex lock_;
  base::DictValue values_;
  // The file was there but unreadable at construction: never overwritten.
  bool save_blocked_ = false;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_STORE_WIN_H_

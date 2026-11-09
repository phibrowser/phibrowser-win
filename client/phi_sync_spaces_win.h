// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_SPACES_WIN_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_SPACES_WIN_H_

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_sync_keys.h"
#include "client/phi_sync_spaces.h"
#include "client/phi_sync_settings_win.h"

namespace phi_client {

class PhiKeyStateFile;
class PhiSpaceManager;

// The Mac's PhiSpaceLocalAccess over the Windows Space list
// (docs/user-data-parity.md S5). Called on the key worker: the identity
// mappings (profiles, Spaces) are read and written right there — they live
// in the account's key_state.json — and everything touching the Space list
// runs on the UI thread through `run_on_ui`, against the PhiSpaceManager
// `spaces` hands out (null when this account's data is not the one open:
// every read is then unanswered — never empty — and every write fails;
// `store_unavailable` tells a store that cannot open from a transient gap).
class PhiSyncSpaceAccess : public sync::SpaceLocalAccess {
 public:
  // What the account-profile refresh needs (ensureLocalProfilesForAccount):
  // the user's profiles (UI), a profile creation that answers with the new
  // basename or "" (UI; asynchronous), and the key layer's re-resolve after
  // a mapping was added (worker).
  struct ProfileHooks {
    std::function<std::vector<keys::LocalProfile>()> local_profiles;
    std::function<void(const std::string& display_name,
                       std::function<void(const std::string& profile_id)> done)>
        create_profile;
    std::function<void()> mappings_changed;
  };

  PhiSyncSpaceAccess(PhiKeyStateFile& state,
                     keys::ProfileKeyManager& profiles,
                     PhiRunOnUi run_on_ui,
                     std::function<PhiSpaceManager*()> spaces,
                     std::function<bool()> store_unavailable,
                     std::function<std::set<std::string>()> known_profiles,
                     ProfileHooks profile_hooks);
  ~PhiSyncSpaceAccess() override;

  PhiSyncSpaceAccess(const PhiSyncSpaceAccess&) = delete;
  PhiSyncSpaceAccess& operator=(const PhiSyncSpaceAccess&) = delete;

  // The hidden cursors as local Space ids (the Mac's refreshCaches).
  std::set<std::string> HiddenLocalIds(const sync::SpaceSyncTable& table);

  std::optional<std::vector<sync::LocalSpace>> CurrentSpaces() override;
  std::optional<std::vector<sync::LocalSpace>> AllSpacesForOrdering() override;
  std::optional<std::vector<sync::LocalSpace>> PairableSpaces() override;
  bool SpaceStoreUnavailable() override;
  std::optional<std::string> GlobalUuid(const std::string& profile_id) override;
  std::optional<std::string> LocalProfileId(const std::string& uuid) override;
  sync::LocalPresence IsKnownLocalProfile(
      const std::string& profile_id) override;
  void DropProfileMapping(const std::string& profile_id) override;
  std::string DefaultProfileId() override;
  std::optional<std::string> SyncUuid(const std::string& space_id) override;
  std::optional<std::string> LocalSpaceId(const std::string& uuid) override;
  std::optional<std::string> EnsureMapped(const std::string& space_id) override;
  bool MapSpace(const std::string& space_id, const std::string& uuid) override;
  void DropSpaceMapping(const std::string& space_id) override;
  sync::LocalPresence IsKnownLocalSpace(const std::string& space_id) override;
  std::map<std::string, std::string> AllSpaceMappings() override;
  bool IsImporting(const std::string& space_id) override;
  sync::ProfileRefreshOutcome RefreshAccountProfiles() override;
  bool Create(const sync::LocalSpace& space) override;
  bool Update(const std::string& space_id,
              const std::optional<std::string>& name,
              const std::optional<std::string>& color_hex,
              const std::optional<std::string>& icon_name,
              std::optional<int64_t> created_ms) override;
  bool Rebind(const std::string& space_id,
              const std::string& profile_id) override;
  bool ApplyThemeState(const std::string& space_id,
                       const std::optional<std::string>& theme_id,
                       std::optional<double> opacity_light,
                       std::optional<double> opacity_dark) override;
  bool ApplyOrder(const std::vector<std::string>& ordered_space_ids) override;
  bool Hide(const std::string& space_id) override;
  bool Purge(const std::string& space_id) override;

 private:
  struct Row {
    sync::LocalSpace space;
    bool hidden = false;
  };
  // Every stored Space (hidden ones included), one UI hop; nullopt when the
  // hop failed or this account's Space list is not open.
  std::optional<std::vector<Row>> Rows();
  // One write on the UI thread; false when it did not run or did not land.
  bool Write(std::function<bool(PhiSpaceManager&)> write);

  RAW_PTR_EXCLUSION PhiKeyStateFile& state_;
  RAW_PTR_EXCLUSION keys::ProfileKeyManager& profiles_;
  PhiRunOnUi run_on_ui_;
  std::function<PhiSpaceManager*()> spaces_;
  std::function<bool()> store_unavailable_;
  // The last unanswered Rows() found the store unable to open (no stall).
  bool last_read_store_unavailable_ = false;
  std::function<std::set<std::string>()> known_profiles_;
  ProfileHooks profile_hooks_;
  // uuid → the profile a previous round created for it (only the adopt
  // failed): reused rather than creating a duplicate.
  std::map<std::string, std::string> pending_created_;
  // One profile creation's answer, filled on the UI thread.
  struct Creation {
    std::mutex lock;
    std::condition_variable done;
    bool answered = false;
    std::string profile_id;
    std::chrono::steady_clock::time_point started;
  };
  // uuid → a creation whose answer the wait gave up on: it may still land,
  // so no second one is started for that uuid (kCreationGiveUp bounds it).
  std::map<std::string, std::shared_ptr<Creation>> creating_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_SPACES_WIN_H_

// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_PROFILE_SERVICE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_PROFILE_SERVICE_H_

#include <windows.h>

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/values.h"
#include "client/phi_ids.h"
#include "client/phi_settings_types.h"
#include "client/phi_sync_status.h"

namespace phi_framework {
class PhiFramework;
}  // namespace phi_framework

namespace phi_client {

// Chromium profiles (docs/architecture-review.md §3.4, lane 1 §1.2 cluster
// 14): the asynchronous creation with request tokens, rename and delete,
// the profile listing reads, the per-profile search engines and download
// directory.
//
// Invariant (§3.4): NO POLLING. A CreateProfile is filed as a PendingProfile
// under the DISPLAY NAME it was asked for — the only thing the client knows
// about the profile before the framework reports its basename — and answered
// by OnProfileCreated; a request the framework never answers is failed by
// the sweep's deadline (the framework has no failure callback), never by a
// timer that keeps looking.
class PhiProfileService {
 public:
  // A CreateProfile in flight and what to do with the profile once
  // OnProfileCreated names it. The oldest request is the fallback when the
  // name cannot be read back. Swept with kCreateTimeoutMs.
  struct PendingProfile {
    enum class Then {
      kOpenSpace,  // "+ Space & profile": create a Space on it, switch.
      kFillForm,   // The create-Space form's picker selects it.
      kReplyOnly,  // profiles.create: answer the waiting verb.
      kSyncPairing,  // The sync setup's "Create on this PC": adopt into it.
    };
    std::string display_name;
    SlotId slot;
    Then then = Then::kReplyOnly;
    int pending_reply_id = 0;
    uint64_t deadline_tick = 0;
  };

  // What the service needs from the app once a creation lands, plus the two
  // cross-domain questions it cannot answer itself.
  class Host {
   public:
    virtual ~Host() = default;
    // Answers a verb waiting on a deferred reply (the dispatcher's slot).
    virtual void CompleteProfileReply(int reply_id, std::string json) = 0;
    // Then::kFillForm — the create-Space form's profile picker selects it.
    virtual void FillSpaceFormProfile(SlotId slot,
                                      const std::string& profile_id) = 0;
    // Then::kOpenSpace — "+ Space & profile": the Space binds the new
    // profile (bindings are immutable, which is why the Space waited for the
    // basename) and the slot enters it.
    virtual void OpenSpaceOnNewProfile(SlotId slot,
                                       const std::string& profile_id) = 0;
    // Then::kSyncPairing — the sync setup window's requested profile landed
    // (`profile_id` empty: the creation failed or timed out).
    virtual void SyncPairingProfileCreated(const std::string& display_name,
                                           const std::string& profile_id) = 0;
    // A profile was renamed or deleted: the Settings panes re-read the list.
    virtual void NoteProfilesChanged() = 0;
    // Whether any Space is still bound to `profile_id` — such a profile is
    // not deletable (the Spaces must be re-profiled or deleted first).
    virtual bool AnySpaceUsesProfile(const std::string& profile_id) const = 0;
    // Something joined or left the "in flight" set: re-arm (or stop) the
    // coordinator's sweep.
    virtual void UpdateSweepTimer() = 0;
    // The client's one analytics capture path (phi_client_analytics.h):
    // the harness ring, then the collector's consent gate and allow-list.
    virtual void CaptureAnalytics(const char* module,
                                  const char* event_name,
                                  base::DictValue properties) = 0;
  };

  PhiProfileService(phi_framework::PhiFramework& framework, Host& host);
  ~PhiProfileService();

  PhiProfileService(const PhiProfileService&) = delete;
  PhiProfileService& operator=(const PhiProfileService&) = delete;

  // --- Asynchronous creation ----------------------------------------------
  // Files a CreateProfile and asks the framework for it. A refusal answers
  // any waiting reply create_failed on the spot.
  void RequestProfile(PendingProfile pending);
  // The framework named (or failed) the profile a request asked for.
  void OnProfileCreated(const char* profile_id);
  // The requests in flight (agentSpace.profiles.create's duplicate check).
  const std::deque<PendingProfile>& pending() const { return pending_; }
  bool HasPending() const { return !pending_.empty(); }
  // Fails every request past its deadline with `timeout_reply`. Called from
  // the coordinator's sweep; the framework has no failure callback, so the
  // deadline is the only way a waiting verb learns its profile is not coming.
  void ExpirePending(uint64_t now_ms, const char* timeout_reply);

  // --- Reads ---------------------------------------------------------------
  // Every profile the framework knows, id and display name.
  std::vector<PhiProfileSummary> Profiles() const;
  // The same list as "<id>\t<name>" rows — the SHELL host's spelling
  // (SpacesHost::ProfileSummaries, which the create-Space form, the menu
  // bar and the Kiosk chooser read). Built from Profiles(); the settings
  // panes take the structs.
  std::vector<std::string> ProfileSummaries() const;
  // The display name the framework reports for `profile_id`, or empty.
  std::string ProfileDisplayName(const std::string& profile_id) const;
  // Whether the framework knows `profile_id` (the rename callback is
  // fire-and-forget, so the check has to happen on this side).
  bool KnowsProfile(const std::string& profile_id) const;

  // --- Writes --------------------------------------------------------------
  // Settings' "New Profile": a bare creation nothing is waiting on.
  void CreateProfileNamed(const std::string& name);
  bool RenameProfileById(const std::string& profile_id,
                         const std::string& name);
  bool DeleteProfileById(const std::string& profile_id);

  // --- Per-profile settings ------------------------------------------------
  // The profile's engines, the default flagged. EMPTY while the profile is
  // not loaded — which is what greys the Settings row out.
  std::vector<PhiSearchEngineRow> SearchEngines(
      const std::string& profile_id) const;
  bool SetSearchEngine(const std::string& profile_id,
                       const std::string& keyword);
  // Chromium sync's status for a LOADED profile (the Mac's
  // ChromiumSyncStatus.read over the framework's ProfileSyncStatusJson);
  // checking when the payload is unknown, nullopt when the profile is not
  // loaded (its sync is not running; the query never loads it). Its
  // enabled categories, when `categories` is given.
  std::optional<sync::ContextStatus> ProfileSyncStatus(
      const std::string& profile_id,
      std::vector<std::string>* categories = nullptr) const;
  // The inputs of the sync summary (the Mac's DevicesSettingView statusSection):
  // "Phi data" first, then the profiles in the framework's order. A profile
  // that is not loaded has no running sync to report and is left out (the Mac
  // lists every profile; Windows loads them lazily, and the status query must
  // never load one).
  struct SyncStatusRows {
    std::vector<sync::ContextStatus> contexts;
    std::set<std::string> required;
    std::vector<std::string> rows;
  };
  SyncStatusRows SyncStatus(std::optional<sync::ContextStatus> native) const;
  std::string DownloadDirectory(const std::string& profile_id) const;
  bool SetDownloadDirectory(const std::string& profile_id,
                            const std::string& dir);

  // How long a CreateProfile may stay unanswered before the sweep presumes
  // it lost. Shared with the window-creation deadline.
  static constexpr uint64_t kCreateTimeoutMs = 20 * 1000;

 private:
  RAW_PTR_EXCLUSION phi_framework::PhiFramework& framework_;
  RAW_PTR_EXCLUSION Host& host_;
  std::deque<PendingProfile> pending_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_PROFILE_SERVICE_H_

// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_CONTENT_BLOCKING_H_
#define PHIBROWSER_WIN_CLIENT_PHI_CONTENT_BLOCKING_H_

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/values.h"

namespace phi_framework {
class PhiFramework;
}

namespace phi_client {

// The three switches of a profile's Content blocking group (the Mac's
// ContentBlockingCategory).
enum class PhiContentBlockingCategory { kAds, kCookieBanners, kTrackers };

// One filter list as the framework reports it (the Mac's ContentBlockingList
// before its Mac-side strings are attached).
struct PhiContentBlockingList {
  std::string id;
  // ads | trackers | cookies | regional | phi | custom.
  std::string category;
  std::string name;  // Custom lists: the user's name.
  bool custom = false;
  std::string homepage;
  std::string license;
  std::string source_url;  // A downloaded custom list's URL.
  bool checked = false;
  bool default_checked = false;  // The catalog recommends it.
  bool available = false;        // Its text is on disk.
  bool downloading = false;
  int64_t downloaded_bytes = 0;
  int64_t total_bytes = -1;  // -1: the server gave no size.
  double fetched_at_ms = 0;  // 0: never.
  std::string last_error;    // The last download failure.
};

// A profile's content blocking state (the Mac's ContentBlockingState).
struct PhiContentBlockingState {
  bool block_ads = false;
  bool block_cookie_banners = false;
  bool block_trackers = false;
  std::vector<PhiContentBlockingList> lists;
  std::vector<std::string> site_exceptions;
  // active | building | degraded | disabled | no_lists.
  std::string status = "disabled";
  std::string status_detail;
  double session_blocked_count = 0;  // Diagnosis only; never shown.
  std::string last_build_log;

  bool Switch(PhiContentBlockingCategory category) const;
  void SetSwitch(PhiContentBlockingCategory category, bool on);
  PhiContentBlockingList* Find(const std::string& id);
};

// The framework's settings JSON (phi_framework_api.h,
// RequestContentBlockingSettings); false when it does not parse.
bool PhiParseContentBlockingState(const std::string& json,
                                  PhiContentBlockingState* out);

// The Mac's ContentBlockingRuleSets, pure so the pane, the chooser and the
// tests agree. A switch's catalog lists (ads also takes the regional ones),
// in catalog order.
std::vector<const PhiContentBlockingList*> PhiContentBlockingListsFor(
    PhiContentBlockingCategory category,
    const PhiContentBlockingState& state);
// The user's custom lists, which apply to every switch.
std::vector<const PhiContentBlockingList*> PhiContentBlockingCustomLists(
    const PhiContentBlockingState& state);
// True when the switch would have nothing to apply: none of its checked
// lists (custom lists included) is on disk.
bool PhiContentBlockingNeedsDownload(PhiContentBlockingCategory category,
                                     const PhiContentBlockingState& state);
// Switches that are on with nothing usable (their last list was unchecked or
// removed); the pane turns them off, as the Mac's does.
std::vector<PhiContentBlockingCategory> PhiContentBlockingTogglesToTurnOff(
    const PhiContentBlockingState& state);
// What the chooser shows for a switch: its lists, then the custom ones.
std::vector<const PhiContentBlockingList*> PhiContentBlockingShownLists(
    PhiContentBlockingCategory category,
    const PhiContentBlockingState& state);
// Checked lists Done still has to download.
std::vector<std::string> PhiContentBlockingMissingIds(
    PhiContentBlockingCategory category,
    const PhiContentBlockingState& state);
bool PhiContentBlockingAnyDownloading(PhiContentBlockingCategory category,
                                      const PhiContentBlockingState& state);
// Lists shown for the switch that are on disk and can be fetched again
// ("Update Downloaded"); a pasted custom list has no source.
std::vector<std::string> PhiContentBlockingDownloadedIds(
    PhiContentBlockingCategory category,
    const PhiContentBlockingState& state);

// Told whenever a profile's state changed (a reply landed, a write went out
// optimistically, the framework reported a rule set or download change).
class PhiContentBlockingObserver {
 public:
  virtual void OnContentBlockingChanged(const std::string& profile_id) = 0;

 protected:
  virtual ~PhiContentBlockingObserver() = default;
};

// The one client-side accessor for content blocking: the Mac's
// ContentBlockingSettings for every profile at once. The state lives in the
// framework (Chromium prefs and each profile's rule set); this keeps the
// last read per profile, sends writes through the framework's ABI 9 calls,
// updates the copy optimistically and reverts it when the framework refuses.
// Views never call the framework themselves.
//
// It also keeps the launch snapshot's mirror (the Mac's
// ContentBlockingAnalytics): the switches live in Chromium prefs, which the
// launch-time user_defaults_snapshot cannot read, so every state learned
// here is mirrored into PhiContentBlockingMetrics.json and the snapshot
// reads that. Three booleans per profile, never a list, URL or site.
//
// UI thread only.
class PhiContentBlocking {
 public:
  // Error text when the framework could not take the request at all.
  static constexpr char kUnavailable[] = "unavailable";
  using Done = std::function<void(bool ok, const std::string& result)>;

  PhiContentBlocking();
  ~PhiContentBlocking();
  PhiContentBlocking(const PhiContentBlocking&) = delete;
  PhiContentBlocking& operator=(const PhiContentBlocking&) = delete;

  // Hands over the framework and reads the analytics mirror.
  void Init(phi_framework::PhiFramework* framework,
            const std::wstring& user_data_dir);

  void AddObserver(PhiContentBlockingObserver* observer);
  void RemoveObserver(PhiContentBlockingObserver* observer);

  // The last state read for `profile_id`, or null before the first answer.
  const PhiContentBlockingState* State(const std::string& profile_id) const;
  // The last read failed (the profile is gone, or the framework refused);
  // the pane shows the switches disabled.
  bool Unavailable(const std::string& profile_id) const;
  // Reads the state again; observers hear when it lands.
  void Refresh(const std::string& profile_id);

  void SetCategory(const std::string& profile_id,
                   PhiContentBlockingCategory category,
                   bool enabled,
                   Done done = nullptr);
  void SetList(const std::string& profile_id,
               const std::string& list_id,
               bool checked,
               Done done = nullptr);
  // Exactly one of `url` / `rules` non-empty; `done` gets the new list id
  // or the error.
  void AddCustomList(const std::string& profile_id,
                     const std::string& name,
                     const std::string& url,
                     const std::string& rules,
                     Done done);
  void RemoveCustomList(const std::string& profile_id,
                        const std::string& list_id,
                        Done done = nullptr);
  // The lists' rows show "Downloading…" until the framework reports.
  void DownloadLists(const std::string& profile_id,
                     const std::vector<std::string>& list_ids,
                     Done done = nullptr);
  void RefreshLists(const std::string& profile_id, Done done = nullptr);

  // PhiFrameworkDelegate's two ABI 9 callbacks, from the dispatcher.
  void OnReply(int64_t request_id, bool success, const char* payload);
  void OnFrameworkChanged(const char* profile_id);

  // Drops the mirror's entries for profiles that no longer exist.
  void PruneProfiles(const std::set<std::string>& known_profile_ids);
  // The profiles that exist now; every mirror write prunes the rest (the
  // Mac's knownProfileIds).
  void SetKnownProfiles(std::function<std::set<std::string>()> known);
  // block_ads_enabled / block_cookie_banners_enabled / block_trackers_enabled:
  // true when any profile has that switch on, as of the last time it was
  // learned (this session or a previous one).
  void AddSnapshotProperties(base::DictValue* properties) const;

 private:
  enum class Kind { kRead, kWrite, kAdd };
  struct Pending {
    std::string profile_id;
    Kind kind = Kind::kWrite;
    // The state before an optimistic write, restored when it is refused.
    std::optional<PhiContentBlockingState> previous;
    Done done;
  };

  // Starts a request (`request_id` 0 means the framework took nothing) and
  // records what its reply should do.
  void Track(int64_t request_id, Pending pending);
  // An optimistic edit of the cached state, then the observers.
  bool Edit(const std::string& profile_id,
            const std::function<void(PhiContentBlockingState*)>& edit,
            std::optional<PhiContentBlockingState>* previous);
  void Notify(const std::string& profile_id);
  bool WritePending(const std::string& profile_id) const;
  void Mirror(const std::string& profile_id,
              const PhiContentBlockingState& state);
  void SaveMirror() const;

  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  std::map<std::string, PhiContentBlockingState> states_;
  std::set<std::string> unavailable_;
  std::set<std::string> reading_;  // A read is in flight.
  std::set<std::string> reread_;   // ... and another was asked for since.
  std::map<int64_t, Pending> pending_;
  std::vector<PhiContentBlockingObserver*> observers_;
  // profile -> {ads, cookie_banners, trackers}.
  std::map<std::string, std::map<std::string, bool>> mirror_;
  std::wstring mirror_path_;
  std::function<std::set<std::string>()> known_profiles_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CONTENT_BLOCKING_H_

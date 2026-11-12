// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_ACCOUNT_PROFILE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_ACCOUNT_PROFILE_H_

#include <windows.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace phi_client {

class PhiAuth;

// The account service's profile record (the Mac's Profile model,
// Sources/Networking/Models/Profile.swift): GET/PUT /api/auth/profile.
struct PhiAccountProfile {
  int id = 0;
  std::string auth0_id;  // Must equal the session's `sub` or it is ignored.
  std::string email;
  std::string name;
  std::string picture;  // Avatar URL — stable while its content changes.
  std::string last_login;
  std::string created_at;
};

// The signed-in account's profile and avatar — the Mac AccountController's
// prefetchProfile / refreshAvatar / storeAvatarImage, minus Kingfisher.
//
// Cached on disk in the ACCOUNT's directory (Phi\users\<sub>, as the Mac
// keeps cachedProfile in the account's plist): PhiAccountProfile.json (the
// record; not a secret) and PhiAvatar.img (the picture's bytes as served).
// Both go on sign-out; another subject reads its own.
//
// Threading: SINGLE WRITER, the same shape as PhiAuth. The UI thread owns
// `profile_` and the avatar; fetch and update run on worker threads, park
// their result under a lock and post `notify_msg` to `notify_hwnd`; the
// owner then calls ApplyPending() on the UI thread, which moves the result
// in, persists it and reports whether anything changed.
class PhiAccountProfileService {
 public:
  PhiAccountProfileService();
  ~PhiAccountProfileService();

  PhiAccountProfileService(const PhiAccountProfileService&) = delete;
  PhiAccountProfileService& operator=(const PhiAccountProfileService&) =
      delete;

  // Remembers where accounts live; the caches are read per subject as
  // SyncWithSession names one. UI thread, once.
  void Load(const std::wstring& user_data_dir,
            HWND notify_hwnd,
            UINT notify_msg);

  // The session moved: `sub` is the signed-in subject (empty when signed
  // out or reauthentication is required — no token, so no fetch). A cache
  // for another subject is dropped; a live session with no fresh profile
  // starts a fetch. `fallback_picture` is the id_token's picture claim,
  // fetched when the profile GET fails so an offline launch still has a
  // face. UI thread.
  void SyncWithSession(PhiAuth* auth,
                       const std::string& sub,
                       const std::string& fallback_picture);
  // Re-fetches the profile (and then the avatar) unless one is in flight or
  // the last fetch was recent (`force` ignores the interval) — the settings
  // window coming back to the front after the avatar editor. UI thread.
  void Refresh(PhiAuth* auth, bool force);
  // PUT {"name": …}; the pane hears the result through the notify message.
  // The caller validates (non-empty, at most kMaxNameLength). UI thread.
  void UpdateName(PhiAuth* auth, const std::string& name);
  // The notify message arrived: moves a worker's result in. True when the
  // profile, the avatar, the busy flag or the error changed. UI thread.
  bool ApplyPending();
  // Sign-out: drops memory and both cache files. UI thread.
  void Clear();

  const std::optional<PhiAccountProfile>& profile() const { return profile_; }
  // A fetch or update is in flight.
  bool busy() const { return busy_; }
  // The last fetch/update's failure, for the pane ("profile" or "update");
  // empty after a success.
  const std::string& last_error() const { return last_error_; }
  bool has_avatar() const { return !avatar_bytes_.empty(); }
  // The avatar as a `px`-pixel circle, premultiplied BGRA for PhiDrawBgra /
  // PhiBlendBgra; empty when there is none or it does not decode. Decoded
  // on demand and cached per size. UI thread.
  const std::vector<uint8_t>& AvatarCircleBgra(int px);

  // The Mac's maxUserNameLength.
  static constexpr int kMaxNameLength = 100;

 private:
  struct Job;
  struct Result;

  void StartJob(std::unique_ptr<Job> job);
  static DWORD WINAPI JobThread(void* param);
  void PersistProfile();
  void PersistAvatar();
  // Points the caches at `sub`'s account directory and reads them.
  void BindSubject(const std::string& sub);

  std::wstring user_data_dir_;
  std::wstring profile_path_;
  std::wstring avatar_path_;
  HWND notify_hwnd_ = nullptr;
  UINT notify_msg_ = 0;

  // UI-thread state.
  std::optional<PhiAccountProfile> profile_;
  std::string avatar_bytes_;  // As served; empty = none.
  std::string avatar_url_;    // What avatar_bytes_ came from.
  std::map<int, std::vector<uint8_t>> avatar_circles_;  // px -> BGRA.
  bool busy_ = false;
  std::string last_error_;
  std::string synced_sub_;
  uint64_t last_fetch_tick_ = 0;
  // Bumped on every subject change; a result from an earlier subject is
  // dropped rather than applied.
  uint32_t generation_ = 0;

  // The workers' mailbox.
  CRITICAL_SECTION lock_;
  std::unique_ptr<Result> pending_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_ACCOUNT_PROFILE_H_

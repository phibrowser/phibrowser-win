// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_CHROMIUM_SYNC_H_
#define PHIBROWSER_WIN_CLIENT_PHI_CHROMIUM_SYNC_H_

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"

namespace phi_framework {
class PhiFramework;
}  // namespace phi_framework

namespace phi_client {

// Chromium's own sync, as the Windows client drives it
// (docs/user-data-parity.md S2) — the Mac's bridge half of it: Chromium's sync
// service asks this object for the Auth0 bearer token, the account's stable
// identity and each profile's resolved key (PhiFrameworkDelegate
// GetPhiSyncAccessToken / GetPhiSyncAccount / GetPhiProfileSyncInfo), and this
// object nudges Chromium when any of them moves (PhiFramework
// NotifyPhiAuthStateChanged / NotifyPhiSyncKeysChanged /
// NotifyPhiSyncInvalidation). The server is named once, at launch, by the
// exe's PhiClientSyncServerUrl export (PhiAuth::SyncServerUrl).
//
// What it holds is memory only: the keys arrive from the key layer (S3, the
// Mac's SyncKeyController.profileSyncInfo) every launch, never from disk.
// UI thread only.
class PhiChromiumSync {
 public:
  // The Mac's getPhiSyncAccountInfo answers: nil / {subject,email} / {}.
  enum class AccountState { kUnresolved = 0, kSignedIn = 1, kSignedOut = 2 };
  struct ProfileKey {
    std::string uuid;        // The account-global profile uuid.
    std::string passphrase;  // The 32-byte key as 64 lowercase hex chars.
  };

  PhiChromiumSync();
  ~PhiChromiumSync();

  PhiChromiumSync(const PhiChromiumSync&) = delete;
  PhiChromiumSync& operator=(const PhiChromiumSync&) = delete;

  // The framework the notifications go to, and where the bearer token comes
  // from (non-blocking: PhiAuth::AccessTokenForSync).
  void Bind(phi_framework::PhiFramework* framework,
            std::function<std::string()> token_source);

  // The account moved. Notifies Chromium only when the answer changed.
  void SetAccount(AccountState state,
                  const std::string& subject,
                  const std::string& email);
  // The session changed without the account changing (a renewed token,
  // reauthentication): Chromium re-reads.
  void NotifyAuthChanged();

  // A profile's key resolved or changed (empty uuid or passphrase removes
  // it). Notifies Chromium when the set changed.
  void SetProfileKey(const std::string& profile_id, const ProfileKey& key);
  void ClearProfileKeys();
  // The key layer's whole answer at once (PhiSyncKeyService's exposed
  // keys): one notification when anything differs.
  void ReplaceProfileKeys(const std::map<std::string, ProfileKey>& keys);
  const std::map<std::string, ProfileKey>& keys() const { return keys_; }

  // An invalidation hint from the notification stream for
  // chromium:<profile_uuid> (S5); forwarded as is.
  void Invalidate(const std::string& account_id,
                  const std::string& profile_uuid,
                  const std::vector<int>& data_type_ids,
                  const std::string& excluding_client_id);

  // --- The delegate's answers -----------------------------------------------
  std::string AccessToken() const;
  AccountState Account(std::string* subject, std::string* email) const;
  bool ProfileKeyFor(const std::string& profile_id, ProfileKey* out) const;

 private:
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  std::function<std::string()> token_source_;
  AccountState state_ = AccountState::kUnresolved;
  std::string subject_;
  std::string email_;
  std::map<std::string, ProfileKey> keys_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CHROMIUM_SYNC_H_

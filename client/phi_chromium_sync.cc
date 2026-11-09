// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_chromium_sync.h"

#include <algorithm>
#include <utility>

#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"

namespace phi_client {

PhiChromiumSync::PhiChromiumSync() = default;
PhiChromiumSync::~PhiChromiumSync() = default;

void PhiChromiumSync::Bind(phi_framework::PhiFramework* framework,
                           std::function<std::string()> token_source) {
  framework_ = framework;
  token_source_ = std::move(token_source);
}

void PhiChromiumSync::SetAccount(AccountState state,
                                 const std::string& subject,
                                 const std::string& email) {
  const std::string new_subject =
      state == AccountState::kSignedIn ? subject : std::string();
  const std::string new_email =
      state == AccountState::kSignedIn ? email : std::string();
  if (state == state_ && new_subject == subject_ && new_email == email_) {
    return;
  }
  state_ = state;
  subject_ = new_subject;
  email_ = new_email;
  if (state_ == AccountState::kSignedOut) {
    // A key belongs to the account that unlocked it; the next account's
    // key layer supplies its own.
    keys_.clear();
  }
  NotifyAuthChanged();
  if (framework_) {
    framework_->NotifyPhiSyncKeysChanged();
  }
}

void PhiChromiumSync::NotifyAuthChanged() {
  if (framework_) {
    framework_->NotifyPhiAuthStateChanged();
  }
}

void PhiChromiumSync::SetProfileKey(const std::string& profile_id,
                                    const ProfileKey& key) {
  if (profile_id.empty()) {
    return;
  }
  const auto it = keys_.find(profile_id);
  if (key.uuid.empty() || key.passphrase.empty()) {
    if (it == keys_.end()) {
      return;
    }
    keys_.erase(it);
  } else {
    if (it != keys_.end() && it->second.uuid == key.uuid &&
        it->second.passphrase == key.passphrase) {
      return;
    }
    keys_[profile_id] = key;
  }
  if (framework_) {
    framework_->NotifyPhiSyncKeysChanged();
  }
}

void PhiChromiumSync::ClearProfileKeys() {
  if (keys_.empty()) {
    return;
  }
  keys_.clear();
  if (framework_) {
    framework_->NotifyPhiSyncKeysChanged();
  }
}

void PhiChromiumSync::ReplaceProfileKeys(
    const std::map<std::string, ProfileKey>& keys) {
  const bool same =
      keys.size() == keys_.size() &&
      std::equal(keys.begin(), keys.end(), keys_.begin(),
                 [](const auto& a, const auto& b) {
                   return a.first == b.first &&
                          a.second.uuid == b.second.uuid &&
                          a.second.passphrase == b.second.passphrase;
                 });
  if (same) {
    return;
  }
  keys_ = keys;
  if (framework_) {
    framework_->NotifyPhiSyncKeysChanged();
  }
}

void PhiChromiumSync::Invalidate(const std::string& account_id,
                                 const std::string& profile_uuid,
                                 const std::vector<int>& data_type_ids,
                                 const std::string& excluding_client_id) {
  if (!framework_) {
    return;
  }
  framework_->NotifyPhiSyncInvalidation(
      account_id.c_str(), profile_uuid.c_str(), data_type_ids.data(),
      static_cast<int>(data_type_ids.size()), excluding_client_id.c_str());
}

std::string PhiChromiumSync::AccessToken() const {
  // No bearer for an account Chromium does not know is signed in: the
  // provider would refuse to pair it with the identity anyway.
  if (state_ != AccountState::kSignedIn || !token_source_) {
    return std::string();
  }
  return token_source_();
}

PhiChromiumSync::AccountState PhiChromiumSync::Account(
    std::string* subject,
    std::string* email) const {
  if (state_ == AccountState::kSignedIn) {
    *subject = subject_;
    *email = email_;
  }
  return state_;
}

bool PhiChromiumSync::ProfileKeyFor(const std::string& profile_id,
                                    ProfileKey* out) const {
  if (state_ != AccountState::kSignedIn) {
    return false;
  }
  const auto it = keys_.find(profile_id);
  if (it == keys_.end()) {
    return false;
  }
  *out = it->second;
  return true;
}

}  // namespace phi_client

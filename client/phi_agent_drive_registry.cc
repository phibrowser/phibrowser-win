// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_agent_drive_registry.h"

#include <algorithm>
#include <utility>

namespace phi_client {

PhiAgentDriveRegistry::PhiAgentDriveRegistry() = default;
PhiAgentDriveRegistry::~PhiAgentDriveRegistry() = default;

void PhiAgentDriveRegistry::ClearHoldLocked(int64_t session_id) {
  auto it = holds_.find(session_id);
  if (it == holds_.end()) {
    return;
  }
  const int tab_id = it->second.tab_id;
  holds_.erase(it);
  if (changed_) {
    changed_(tab_id);
  }
}

void PhiAgentDriveRegistry::OnDrove(int tab_id,
                                    int64_t window_id,
                                    int64_t session_id) {
  if (tab_id == 0) {
    return;
  }
  auto it = holds_.find(session_id);
  if (it != holds_.end() && it->second.tab_id != tab_id) {
    // The driver MOVED: one session holds one tab, so the tab it left
    // unmasks before the new one arms.
    const int previous = it->second.tab_id;
    holds_.erase(it);
    if (changed_) {
      changed_(previous);
    }
    it = holds_.end();
  }
  const bool arming = it == holds_.end();
  Hold& hold = holds_[session_id];
  hold.tab_id = tab_id;
  hold.window_id = window_id;
  hold.last_seen = ::GetTickCount64();
  if (arming && changed_) {
    changed_(tab_id);
  }
}

void PhiAgentDriveRegistry::OnObserved(int tab_id,
                                       int64_t window_id,
                                       int64_t session_id) {
  auto it = holds_.find(session_id);
  // Observing NEVER arms a mask — it only keeps one already up alive. A
  // session with no hold is reading a tab it has not claimed.
  if (it == holds_.end() || it->second.tab_id != tab_id) {
    return;
  }
  it->second.window_id = window_id;
  it->second.last_seen = ::GetTickCount64();
}

void PhiAgentDriveRegistry::OnStopped(int tab_id, int64_t session_id) {
  auto it = holds_.find(session_id);
  if (it == holds_.end()) {
    return;
  }
  // The browser reports the tab it believes the session held; trust the
  // record when they disagree (the session may have moved since).
  (void)tab_id;
  ClearHoldLocked(session_id);
}

void PhiAgentDriveRegistry::Reclaim(int tab_id) {
  if (tab_id == 0 || reclaimed_.count(tab_id) > 0) {
    return;
  }
  reclaimed_[tab_id] = ::GetTickCount64();
  PushReclaimed();
  if (changed_) {
    changed_(tab_id);
  }
}

void PhiAgentDriveRegistry::Release(int tab_id) {
  if (reclaimed_.erase(tab_id) == 0) {
    return;
  }
  PushReclaimed();
  if (changed_) {
    changed_(tab_id);
  }
}

void PhiAgentDriveRegistry::PushReclaimed() {
  if (!reclaimed_changed_) {
    return;
  }
  std::vector<int> tabs;
  tabs.reserve(reclaimed_.size());
  for (const auto& [tab_id, at] : reclaimed_) {
    tabs.push_back(tab_id);
  }
  reclaimed_changed_(tabs);
}

bool PhiAgentDriveRegistry::IsMasked(int tab_id) const {
  return std::any_of(holds_.begin(), holds_.end(),
                     [tab_id](const auto& entry) {
                       return entry.second.tab_id == tab_id;
                     });
}

bool PhiAgentDriveRegistry::IsReclaimed(int tab_id) const {
  return reclaimed_.count(tab_id) > 0;
}

std::vector<int> PhiAgentDriveRegistry::MaskedTabs() const {
  std::vector<int> tabs;
  for (const auto& [session_id, hold] : holds_) {
    if (std::find(tabs.begin(), tabs.end(), hold.tab_id) == tabs.end()) {
      tabs.push_back(hold.tab_id);
    }
  }
  return tabs;
}

void PhiAgentDriveRegistry::Sweep() {
  const ULONGLONG now = ::GetTickCount64();

  // Idle holds. A RECLAIMED tab is exempt: the browser is refusing that
  // driver, so no reports arrive for it, and an idle rule would retire the
  // pill — and its "Hand back" — seconds after the user took the tab.
  std::vector<int64_t> stale;
  for (const auto& [session_id, hold] : holds_) {
    if (reclaimed_.count(hold.tab_id) > 0) {
      continue;
    }
    if (now - hold.last_seen > kIdleTimeoutMs) {
      stale.push_back(session_id);
    }
  }
  for (int64_t session_id : stale) {
    ClearHoldLocked(session_id);
  }

  // Aged-out takeovers.
  std::vector<int> expired;
  for (const auto& [tab_id, at] : reclaimed_) {
    if (now - at > kReclaimGraceMs) {
      expired.push_back(tab_id);
    }
  }
  if (!expired.empty()) {
    for (int tab_id : expired) {
      reclaimed_.erase(tab_id);
    }
    PushReclaimed();
    for (int tab_id : expired) {
      if (changed_) {
        changed_(tab_id);
      }
    }
  }
}

}  // namespace phi_client

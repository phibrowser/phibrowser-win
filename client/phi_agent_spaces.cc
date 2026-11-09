// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_agent_spaces.h"

#include <algorithm>
#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/uuid.h"

namespace phi_client {

namespace {

const char* OwnershipWire(PhiAgentSpaces::Ownership o) {
  return o == PhiAgentSpaces::Ownership::kAgent ? "agent" : "user";
}

std::string NewUpperUuid() {
  std::string s = base::Uuid::GenerateRandomV4().AsLowercaseString();
  std::transform(s.begin(), s.end(), s.begin(),
                 [](char c) { return static_cast<char>(::toupper(c)); });
  return s;
}

}  // namespace

PhiAgentSpaces::PhiAgentSpaces() = default;
PhiAgentSpaces::~PhiAgentSpaces() = default;

PhiAgentSpaces::Space* PhiAgentSpaces::Find(const std::string& task_id) {
  auto it = spaces_.find(task_id);
  return it == spaces_.end() ? nullptr : &it->second;
}

PhiAgentSpaces::Space* PhiAgentSpaces::FindByWindow(BrowserId window_id) {
  for (auto& [task_id, space] : spaces_) {
    if (space.window_id == window_id) {
      return &space;
    }
  }
  return nullptr;
}

PhiAgentSpaces::Space& PhiAgentSpaces::Create(const std::string& task_id,
                                              BrowserId window_id,
                                              const std::string& profile_id,
                                              bool persistent,
                                              uint64_t now_ms) {
  auto it = spaces_.find(task_id);
  if (it != spaces_.end()) {
    // Re-bind: keep the record, refresh its window and clock.
    it->second.window_id = window_id;
    it->second.status = "running";
    if (!persistent) {
      it->second.keepalive_deadline_ms =
          now_ms + static_cast<uint64_t>(kDefaultTtlSeconds) * 1000;
    }
    return it->second;
  }
  Space space;
  space.task_id = task_id;
  space.space_id = "agentspace-" + base::NumberToString(next_ordinal_++);
  space.number = NextNumber();
  space.window_id = window_id;
  space.profile_id = profile_id;
  space.persistent = persistent;
  space.ownership = Ownership::kAgent;
  space.status = "running";
  space.keepalive_deadline_ms =
      persistent ? 0
                 : now_ms + static_cast<uint64_t>(kDefaultTtlSeconds) * 1000;
  auto [inserted, ok] = spaces_.emplace(task_id, std::move(space));
  return inserted->second;
}

void PhiAgentSpaces::Remove(const std::string& task_id) {
  spaces_.erase(task_id);
}

int PhiAgentSpaces::NextNumber() const {
  int number = 1;
  for (bool taken = true; taken; ++number) {
    taken = false;
    for (const auto& [task_id, space] : spaces_) {
      if (space.number == number) {
        taken = true;
        break;
      }
    }
    if (!taken) {
      return number;
    }
  }
  return number;
}

void PhiAgentSpaces::Ping(const std::string& task_id,
                          int ttl_seconds,
                          uint64_t now_ms) {
  Space* space = Find(task_id);
  if (!space || space->persistent ||
      space->ownership == Ownership::kUser) {
    return;
  }
  if (ttl_seconds > 0) {
    // Explicit: authoritative, shorter or longer than what stands.
    const int ttl = std::max(1, std::min(ttl_seconds, kMaxTtlSeconds));
    space->keepalive_deadline_ms =
        now_ms + static_cast<uint64_t>(ttl) * 1000;
    return;
  }
  // A bare touch only ever EXTENDS.
  space->keepalive_deadline_ms =
      std::max(space->keepalive_deadline_ms,
               now_ms + static_cast<uint64_t>(kDefaultTtlSeconds) * 1000);
}

void PhiAgentSpaces::ResetTtl(const std::string& task_id,
                              int seconds,
                              uint64_t now_ms) {
  Space* space = Find(task_id);
  if (!space || space->persistent ||
      space->ownership == Ownership::kUser) {
    return;
  }
  const int ttl = std::max(1, std::min(seconds, kMaxTtlSeconds));
  space->keepalive_deadline_ms = now_ms + static_cast<uint64_t>(ttl) * 1000;
}

void PhiAgentSpaces::SetState(const std::string& task_id,
                              const std::string* caption,
                              const std::string* state,
                              uint64_t now_ms) {
  Space* space = Find(task_id);
  if (!space) {
    return;
  }
  if (caption) {
    space->caption = *caption;
  }
  if (state && *state == "running") {
    space->status = "running";
    if (!space->persistent && space->ownership == Ownership::kAgent) {
      space->keepalive_deadline_ms =
          now_ms + static_cast<uint64_t>(kDefaultTtlSeconds) * 1000;
    }
  } else if (state && *state == "idle") {
    space->status = "idle";
  }
}

void PhiAgentSpaces::SetOwnership(const std::string& task_id,
                                  Ownership owner,
                                  uint64_t now_ms) {
  Space* space = Find(task_id);
  if (!space) {
    return;
  }
  space->ownership = owner;
  if (owner == Ownership::kUser) {
    space->keepalive_deadline_ms = 0;  // The clock pauses under user control.
  } else if (!space->persistent) {
    space->keepalive_deadline_ms =
        now_ms + static_cast<uint64_t>(kInterRoundTtlSeconds) * 1000;
  }
}

base::ListValue PhiAgentSpaces::DrainMessages(const std::string& task_id) {
  base::ListValue out;
  Space* space = Find(task_id);
  if (!space) {
    return out;
  }
  for (const UserMessage& m : space->pending_messages) {
    base::DictValue d;
    d.Set("id", m.id);
    d.Set("text", m.text);
    d.Set("ts", static_cast<double>(m.ts_ms));
    out.Append(std::move(d));
  }
  space->pending_messages.clear();
  return out;
}

std::string PhiAgentSpaces::PushMessage(const std::string& task_id,
                                        const std::string& text,
                                        int64_t ts_ms) {
  Space* space = Find(task_id);
  if (!space) {
    return std::string();
  }
  UserMessage m;
  m.id = NewUpperUuid();
  // The Mac caps each message at 2000 chars.
  m.text = text.size() > 2000 ? text.substr(0, 2000) : text;
  m.ts_ms = ts_ms;
  space->pending_messages.push_back(m);
  while (space->pending_messages.size() > kMaxQueued) {
    space->pending_messages.pop_front();
  }
  return m.id;
}

base::ListValue PhiAgentSpaces::TasksJson(uint64_t now_ms) const {
  base::ListValue tasks;
  for (const auto& [task_id, space] : spaces_) {
    base::DictValue t;
    t.Set("taskId", space.task_id);
    t.Set("spaceId", space.space_id);
    t.Set("number", space.number);
    t.Set("windowId", static_cast<double>(space.window_id.value));
    t.Set("ownership", OwnershipWire(space.ownership));
    t.Set("status", space.status);
    t.Set("caption", space.caption);
    t.Set("persistent", space.persistent);
    t.Set("pendingUserMessages",
          static_cast<int>(space.pending_messages.size()));
    t.Set("agentName", space.agent_name);
    // null while the user owns it, or when persistent / no deadline.
    if (space.keepalive_deadline_ms == 0 ||
        space.ownership == Ownership::kUser) {
      t.Set("keepAliveRemainingSeconds", base::Value());
    } else {
      const int64_t remaining_ms =
          static_cast<int64_t>(space.keepalive_deadline_ms) -
          static_cast<int64_t>(now_ms);
      t.Set("keepAliveRemainingSeconds",
            static_cast<int>(std::max<int64_t>(0, remaining_ms / 1000)));
    }
    tasks.Append(std::move(t));
  }
  return tasks;
}

std::vector<std::string> PhiAgentSpaces::Expired(uint64_t now_ms) const {
  std::vector<std::string> out;
  for (const auto& [task_id, space] : spaces_) {
    if (space.persistent || space.ownership == Ownership::kUser ||
        space.keepalive_deadline_ms == 0) {
      continue;
    }
    if (space.keepalive_deadline_ms <= now_ms) {
      out.push_back(task_id);
    }
  }
  return out;
}

}  // namespace phi_client

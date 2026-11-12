// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_AGENT_SPACES_H_
#define PHIBROWSER_WIN_CLIENT_PHI_AGENT_SPACES_H_

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "base/values.h"
#include "client/phi_ids.h"

namespace phi_client {

// The client-side model of the phi-browser skill's AGENT SPACES — the Windows
// twin of the Mac AgentSpaceManager's bookkeeping. An agent Space is a hidden
// browser window (hosted off-screen by a PhiShellWindow) keyed by the
// skill's `taskId`, with an ownership state (agent vs the user who took
// control), a keep-alive deadline, and a queue of messages from the user.
//
// This class is PURE STATE + TIME MATH — it owns no windows and calls no
// framework. The coordinator creates/tears down the backing window and passes
// the current monotonic tick (GetTickCount64) into every method that touches
// the keep-alive clock, so the whole thing is deterministic and testable.
// An agent Space's id ("agentspace-N"). While the user watches one, the
// slot hosts its browser under this id exactly like a user Space — so
// nothing that persists a Space (session restore, the routing table, the
// last-active note) may take it for one.
inline bool PhiIsAgentSpaceId(const std::string& space_id) {
  return space_id.rfind("agentspace-", 0) == 0;
}

// Everything runs on the browser UI thread; no locking.
class PhiAgentSpaces {
 public:
  // Keep-alive TTLs, mirroring AgentSpaceManager (seconds).
  static constexpr int kDefaultTtlSeconds = 120;    // a live driving round
  static constexpr int kInterRoundTtlSeconds = 1800;  // between rounds
  static constexpr int kMaxTtlSeconds = 3600;       // ping ceiling

  enum class Ownership { kAgent, kUser };

  struct UserMessage {
    std::string id;    // uppercase UUID, the Mac wire shape
    std::string text;
    int64_t ts_ms = 0;  // epoch milliseconds
  };

  struct Space {
    std::string task_id;
    std::string space_id;
    // The Mac's task number: the lowest unused ordinal (1, 2, 3…), reused
    // once a Space closes — the pip's "R1".
    int number = 0;
    // The hidden browser; its value is what CDP's Browser.getWindowForTarget
    // reports, so the skill correlates tabs to it unchanged.
    BrowserId window_id;
    std::string profile_id;
    std::string agent_name;
    Ownership ownership = Ownership::kAgent;
    // starting | running | idle | completed | failed
    std::string status = "running";
    std::string caption;
    // markError's message — the pill shows it red while status is failed.
    std::string error_message;
    // markError set it; the user surfacing the Space (watching) clears it.
    // The pip wears the red badge while it stands.
    bool unseen_error = false;
    bool persistent = false;
    // Absolute keep-alive deadline in GetTickCount64 ms; 0 = no deadline
    // (persistent, or while the user holds control).
    uint64_t keepalive_deadline_ms = 0;
    std::deque<UserMessage> pending_messages;  // capped kMaxQueued
    // The page area (CSS px) agentSpace.panelSize answers with while the
    // Space is hidden: the first one measured, then the last one it had on
    // screen (the Mac's AgentTask.heldPageArea, `ff8e9238`). 0 = none yet.
    int held_page_width = 0;
    int held_page_height = 0;
  };

  PhiAgentSpaces();
  ~PhiAgentSpaces();

  PhiAgentSpaces(const PhiAgentSpaces&) = delete;
  PhiAgentSpaces& operator=(const PhiAgentSpaces&) = delete;

  Space* Find(const std::string& task_id);
  Space* FindByWindow(BrowserId window_id);
  bool empty() const { return spaces_.empty(); }
  // Read-only view of every record (for the switcher's agent pips).
  const std::map<std::string, Space>& entries() const { return spaces_; }

  // Creates the record (or returns the existing one for a re-bind). The
  // caller supplies the id of the window the framework announced for it.
  Space& Create(const std::string& task_id,
                BrowserId window_id,
                const std::string& profile_id,
                bool persistent,
                uint64_t now_ms);

  void Remove(const std::string& task_id);

  // The Mac's touchKeepAlive: a plain touch (`ttl_seconds <= 0`) only
  // EXTENDS, to max(existing, now + default); an explicit ttl is
  // authoritative in BOTH directions, clamped to [1, kMaxTtlSeconds].
  // Persistent and user-owned spaces have no deadline and are untouched.
  void Ping(const std::string& task_id, int ttl_seconds, uint64_t now_ms);
  // Sets the deadline to exactly now + seconds (the round-start reset, the
  // agent's resume after a hand-back), subject to the same exemptions.
  void ResetTtl(const std::string& task_id, int seconds, uint64_t now_ms);

  // setState: `caption` and `state` are independent and optional. `state`
  // "running" resets the keep-alive to kDefaultTtlSeconds; "idle" leaves it.
  void SetState(const std::string& task_id,
                const std::string* caption,
                const std::string* state,
                uint64_t now_ms);

  // Flips ownership. Handing to the user clears the deadline (their control
  // pauses the clock); handing back to the agent re-arms it with the
  // INTER-ROUND grace (kInterRoundTtlSeconds) — the agent may take a while
  // to notice — which the next round start resets to the driving clock.
  void SetOwnership(const std::string& task_id,
                    Ownership owner,
                    uint64_t now_ms);

  // Drains the message queue into a JSON list `[{"id","text","ts"}]`.
  base::ListValue DrainMessages(const std::string& task_id);
  // Queues a console message from the user; returns its id (empty if unknown).
  std::string PushMessage(const std::string& task_id,
                          const std::string& text,
                          int64_t ts_ms);

  // The `tasks` array for agentSpace.list.
  base::ListValue TasksJson(uint64_t now_ms) const;

  // Task ids whose keep-alive has lapsed (agent-owned, non-persistent, with a
  // deadline in the past) — the coordinator completes and closes these.
  std::vector<std::string> Expired(uint64_t now_ms) const;

 private:
  static constexpr size_t kMaxQueued = 50;
  // The lowest ordinal no live Space holds.
  int NextNumber() const;

  std::map<std::string, Space> spaces_;
  int next_ordinal_ = 1;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_AGENT_SPACES_H_

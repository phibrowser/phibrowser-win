// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_AGENT_SPACE_MANAGER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_AGENT_SPACE_MANAGER_H_

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_agent_drive_registry.h"
#include "client/phi_agent_space_route.h"
#include "client/phi_agent_spaces.h"
#include "client/phi_ids.h"
#include "client/phi_shell_window.h"

namespace phi_framework {
class PhiFramework;
}  // namespace phi_framework

namespace phi_client {

// The phi-browser skill's hidden windows (docs/architecture-review.md §3.4,
// lane 1 §1.2 clusters 12 and 18): agent Spaces (a hidden browser with a
// presence — a pip, a watch mode and an ownership state),
// shadow windows (the same hidden surface WITHOUT a presence), the keep-alive
// sweep, and the operating mask over the user's OWN tabs.
//
// Invariants (§3.4): the watch overlay is owned per shell and unhooked
// before the shell it is bound to goes; ownership transitions are ENFORCED
// (the record moves first, then the driver is told — a driver cannot decline
// a takeover); and a task whose record is already gone is deliberately not
// announced, because drivers learn of an ended task from its absence in
// agentSpace.list.
class PhiAgentSpaceManager {
 public:
  // A shadow window is an agent-style off-screen window WITHOUT a presence:
  // no pip, no watch, no ownership — the Mac's
  // AgentSpaceManager+Shadow registry, keyed by the skill's taskId. Kept out
  // of the agent Spaces so nothing that draws or surfaces agent Spaces sees
  // it; the window itself is adopted exactly like an agent Space's (hidden
  // controller, parked off-screen, composited for CDP).
  struct ShadowWindow {
    std::string task_id;
    BrowserId window_id;  // Its value is the CDP windowId.
    std::string profile_id;
    bool incognito = false;
    int64_t created_ms = 0;              // epoch ms (the list's createdAt)
    uint64_t keepalive_deadline_ms = 0;  // GetTickCount64 ms; swept when past
  };

  // What the manager needs from the app: the shell behind a hidden window,
  // the repaints, the sweep arming, the wire and the deferred replies.
  class Host {
   public:
    virtual ~Host() = default;
    // The controller hosting `window_id` (agent windows live in a slot of
    // their own, keyed by the browser that opened them), or null.
    virtual PhiShellWindow* ControllerForAgentWindow(
        BrowserId window_id) = 0;
    // The shell in `slot` (a user window's slot id), or null.
    virtual PhiShellWindow* ControllerForSlot(int64_t slot) = 0;
    // The user shell whose frame holds the agent Space `task_id`'s browser
    // (PhiShellWindow::hosted_agent_task) — hosting it for a live watch, or
    // still handing it back after one — or null when its own agent shell
    // has it.
    virtual PhiShellWindow* SlotHostingAgentTask(
        const std::string& task_id) = 0;
    // While a user slot hosts an agent browser for a watch, the browser's
    // model pushes must reach THAT shell: rebinds the browser to `slot`
    // (0: back to its own agent slot, or unbound if that shell is gone).
    virtual void RebindAgentBrowser(BrowserId browser, int64_t slot) = 0;
    // The agent pips live in the user windows' switchers.
    virtual void InvalidateUserSlots() = 0;
    // The operating mask changed: every window repaints its rows.
    virtual void InvalidateAll() = 0;
    // Something joined or left the swept set.
    virtual void UpdateSweepTimer() = 0;
    // agentSpace.* events out to the connected drivers.
    virtual void BroadcastControlEvent(const std::string& type,
                                       const std::string& payload_json) = 0;
    // The waiting verb's deferred reply (window creation).
    virtual void CompleteAgentReply(int reply_id, std::string json) = 0;
    // The agent handed control to the user and asked for them (the Mac's
    // "The agent needs you" floating prompt): show it for `task_id` with
    // the agent's message, or take it down again (hand-back, resume,
    // completion, close — only ever the prompt for that task).
    virtual void ShowAgentHandoffPrompt(const std::string& task_id,
                                        const std::string& message) = 0;
    virtual void DismissAgentHandoffPrompt(const std::string& task_id) = 0;
    // A run-state edge for Agent Autoview (View ▸ Agent Autoview): the
    // owner re-evaluates which running agent Space to surface.
    virtual void AgentRunStateChanged() = 0;
  };

  explicit PhiAgentSpaceManager(Host& host);
  ~PhiAgentSpaceManager();

  PhiAgentSpaceManager(const PhiAgentSpaceManager&) = delete;
  PhiAgentSpaceManager& operator=(const PhiAgentSpaceManager&) = delete;

  // The framework arrives with OnFrameworkReady, and the operating mask's
  // callbacks are wired here. Every framework call below is guarded exactly
  // as the coordinator's `if (framework_)` was.
  void Init(phi_framework::PhiFramework* framework);

  // The records. Published rather than wrapped: the verb ladder addresses
  // both registries directly (the same documented exception
  // PhiSpaceManager::mutable_model() is).
  PhiAgentSpaces& spaces() { return spaces_; }
  std::map<std::string, ShadowWindow>& shadows() { return shadows_; }
  // Which of the user's own tabs an agent currently holds.
  PhiAgentDriveRegistry& drives() { return drives_; }
  const PhiAgentDriveRegistry& drives() const { return drives_; }
  bool Empty() const { return spaces_.empty() && shadows_.empty(); }

  // --- Adoption ------------------------------------------------------------
  // The hidden window for `task_id` has arrived: file it as an agent Space
  // (`agent` true) or a shadow, and fill `reply` with the wire fields the
  // waiting verb expects (spaceId + ownership, or incognito).
  void NoteAgentWindow(bool agent,
                       const std::string& task_id,
                       BrowserId window,
                       const std::string& profile_id,
                       const std::string& agent_name,
                       bool persistent,
                       bool incognito,
                       uint64_t now_ms,
                       base::DictValue* reply);
  // The hidden window behind `browser` is closing: drop its record. True
  // when it WAS one of ours (an agent Space or a shadow).
  bool RetireHiddenWindow(BrowserId browser);

  // --- Pips, watch mode and the overlay ------------------------------------
  // The pips `slot`'s switchers offer: every live agent Space minus those
  // another user slot hosts (Presents). 0 lists them all — an agent window's
  // own strip, which is never a host.
  std::vector<PhiAgentPipView> Pips(int64_t slot) const;
  // Where a switch to `task_id` asked from `slot` lands, and whether `slot`
  // presents the Space at all (offered iff kLocal) — the rule in
  // phi_agent_space_route.h. Every user Space is offered everywhere; an
  // agent Space only by the window it is on screen in (or, unwatched, by
  // every window). Strips re-evaluate this through InvalidateUserSlots when
  // the hosting map changes: a watch starting or ending, and the browser
  // landing back in its shell (OnAgentBrowserReturned).
  PhiAgentSpaceSwitchRoute RouteFor(const std::string& task_id,
                                    int64_t slot) const;
  bool Presents(const std::string& task_id, int64_t slot) const {
    return RouteFor(task_id, slot) == PhiAgentSpaceSwitchRoute::kLocal;
  }
  const std::string& watched_task() const { return watched_task_; }
  // Watch mode: the agent's window takes the frame of the user window in
  // `watcher_slot` (the one whose pip was clicked) and that window is
  // concealed — one window on screen, the Mac's slot.activate. Clicking
  // the watched pip again ends the watch; a different pip swaps. Asked
  // for a Space another window hosts, it surfaces that window instead
  // (RouteFor: the browser never moves between user slots).
  void RequestWatch(const std::string& task_id, int64_t watcher_slot);
  // Ends the watch, if any. In the slot-hosted form (the normal one) the
  // hosting user window swaps back to `leave_to` (empty: the Space it came
  // from) with its own Space swap and hands the browser back when the
  // slide lands; in the fallback (an agent window surfaced on its own)
  // the agent window is parked (`park_agent` false when it is closing
  // anyway). Returns the watcher's slot (0 when nothing was watched).
  int64_t EndWatch(bool park_agent, const std::string& leave_to = "");
  // The hosting slot let the agent browser go (see EndWatch): it goes back
  // into its own hidden agent shell.
  void OnAgentBrowserReturned(const std::string& task_id, HWND child);
  bool hosting() const { return hosting_; }
  // Mounts / updates / tears down the watch-mode overlay for the watched task.
  void RefreshOverlay();
  // Hide() only if the overlay is bound to the shell that is going.
  void HideOverlayBoundTo(const PhiShellWindow* controller);

  // --- Ownership -----------------------------------------------------------
  void RequestTakeControl(const std::string& task_id);
  // Tells the framework who drives the task's window, from its ownership.
  void NoteAgentDriving(const std::string& task_id);
  void RequestHandBack(const std::string& task_id);
  // The pill's Finish (user-owned): completes the task successfully,
  // keeping a persistent Space — the Mac's finishAgentTask.
  void RequestFinish(const std::string& task_id);
  // The task's end, from every path — agentSpace.complete, the pill's
  // Finish, the keep-alive sweep ("expired: no agent activity"). Appends
  // "Task completed" / "Task failed" (+ message), takes down the handoff
  // prompt, then: a persistent Space with `keep` is marked completed and
  // its hidden window stays for the next re-bind (the Mac closes the
  // window and re-binds from the Space row; Windows has no Space row to
  // re-bind from); anything else is torn down.
  void CompleteTask(const std::string& task_id,
                    bool success,
                    bool keep,
                    const std::string& message);
  // agentSpace.handoff: the agent hands control to the user with a message
  // and the user is asked for (the floating prompt).
  void HandoffToUser(const std::string& task_id, const std::string& message);
  // agentSpace.takeover: the agent resumes — only from user ownership, and
  // with an explicit round TTL (a plain touch could not shorten a banked
  // inter-round window). No-op when the agent already holds it.
  void ResumeAgentControl(const std::string& task_id);
  // Pushes agentSpace.ownershipChanged to connected drivers — the Mac's
  // broadcastToTaskDriver. Called after every ownership transition.
  void PushOwnershipChanged(const std::string& task_id);
  // Tears down an agent Space: closes its hidden window and drops the record.
  void CloseAgentSpace(const std::string& task_id);

  // --- Shadow windows ------------------------------------------------------
  ShadowWindow* FindShadowByWindow(BrowserId window_id);
  // The Mac's keep-alive contract, shared with agent Spaces: extends the
  // deadline to max(existing, now + ttl); ttl <= 0 is the default round TTL.
  void TouchShadow(ShadowWindow* shadow, int ttl_seconds, uint64_t now_ms);
  void CloseShadowWindow(const std::string& task_id, const char* reason);
  void CloseAllShadowWindows(const char* reason);

  // --- The keep-alive sweep ------------------------------------------------
  // Completes and closes expired Spaces and shadows. The coordinator's sweep
  // runs this, then expires the creations in flight.
  void Sweep(uint64_t now_ms);

  // Queues a message from the user for the task's driver (it drains the
  // queue on its next poll) and wakes the driver. False when the task is
  // gone.
  bool PushUserMessage(const std::string& task_id, const std::string& text);

 private:
  // The controller of the agent Space `task_id`'s window, or null.
  PhiShellWindow* ControllerFor(const std::string& task_id);
  // The user slot whose window the agent Space is on screen in: the slot
  // hosting its browser (live, or `in_flight` — the swap back has not
  // landed), or the one the fallback's agent window stands over; 0 when
  // the browser is parked in its shell and nobody watches it.
  int64_t HostingSlotFor(const std::string& task_id, bool* in_flight) const;
  // RouteFor said kSurfaceInHost: bring the window the Space is on screen
  // in to the front.
  void SurfaceInHost(const std::string& task_id);

  RAW_PTR_EXCLUSION Host& host_;
  // Non-owning, across a module boundary, so not a raw_ptr. Null until Init.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  PhiAgentSpaces spaces_;
  // The agent Space whose window is currently surfaced for the user to watch
  // (empty = none). At most one is watched at a time.
  std::string watched_task_;
  // The user window watching (0 = none): the slot hosting the agent Space,
  // or — in the fallback — the one the agent window stands over. A slot
  // id, resolved through the host each time: the window may go.
  int64_t watcher_slot_ = 0;
  // Whether the watch is the slot-hosted form (the agent browser is a Space
  // of the watcher's slot) rather than the fallback agent window.
  bool hosting_ = false;
  // The shadow registry, keyed by the skill's taskId.
  std::map<std::string, ShadowWindow> shadows_;
  // The operating mask over the user's OWN tabs, fed by the framework's
  // three drive reports.
  PhiAgentDriveRegistry drives_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_AGENT_SPACE_MANAGER_H_

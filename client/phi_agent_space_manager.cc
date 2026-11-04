// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_agent_space_manager.h"

#include <algorithm>
#include <utility>

#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_agent_icons.h"
#include "phibrowser-win/client/phi_agent_overlay.h"
#include "phibrowser-win/client/phi_util.h"

namespace phi_client {

namespace {

// Epoch milliseconds, for the wire's createdAt.
int64_t EpochMsNow() {
  FILETIME ft = {};
  ::GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER u;
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  return static_cast<int64_t>((u.QuadPart - 116444736000000000ULL) / 10000);
}

}  // namespace

PhiAgentSpaceManager::PhiAgentSpaceManager(Host& host) : host_(host) {}

PhiAgentSpaceManager::~PhiAgentSpaceManager() = default;

void PhiAgentSpaceManager::Init(phi_framework::PhiFramework* framework) {
  framework_ = framework;
  // The operating mask: a hold changing repaints the owning shell, and the
  // reclaimed set is pushed to the browser, which then REFUSES drive commands
  // aimed at those tabs (the refusal is the browser's, so a driver cannot
  // decline to honour the takeover).
  drives_.set_changed_callback([this](int tab_id) { host_.InvalidateAll(); });
  drives_.set_reclaimed_callback([this](const std::vector<int>& tabs) {
    if (framework_) {
      framework_->SetUserReclaimedTabs(tabs.empty() ? nullptr : tabs.data(),
                                       static_cast<int>(tabs.size()));
    }
  });
}

PhiShellWindow* PhiAgentSpaceManager::ControllerFor(
    const std::string& task_id) {
  PhiAgentSpaces::Space* space = spaces_.Find(task_id);
  return space ? host_.ControllerForAgentWindow(space->window_id) : nullptr;
}

void PhiAgentSpaceManager::NoteAgentWindow(bool agent,
                                           const std::string& task_id,
                                           BrowserId window,
                                           const std::string& profile_id,
                                           const std::string& agent_name,
                                           bool persistent,
                                           bool incognito,
                                           uint64_t now_ms,
                                           base::DictValue* reply) {
  if (agent) {
    PhiAgentSpaces::Space& space =
        spaces_.Create(task_id, window, profile_id, persistent, now_ms);
    space.agent_name = agent_name;
    space.unseen_error = false;
    space.error_message.clear();
    transcript_.NoteTaskLive(task_id, space.number, EpochMsNow());
    host_.InvalidateUserSlots();  // A new agent pip appears in the switcher.
    host_.AgentRunStateChanged();
    reply->Set("spaceId", space.space_id);
    reply->Set("ownership", "agent");
    return;
  }
  ShadowWindow& shadow = shadows_[task_id];
  shadow.task_id = task_id;
  shadow.window_id = window;
  shadow.profile_id = profile_id;
  shadow.incognito = incognito;
  shadow.created_ms = EpochMsNow();
  TouchShadow(&shadow, 0, now_ms);
  LOG(INFO) << "[PhiClient] shadow window created: " << task_id
            << " window=" << window.value << (incognito ? " (incognito)" : "");
  // Echoed on purpose: a driver that asked for incognito checks this flag
  // and refuses to proceed if it is missing (an older build).
  reply->Set("incognito", incognito);
}

bool PhiAgentSpaceManager::RetireHiddenWindow(BrowserId browser) {
  if (ShadowWindow* shadow = FindShadowByWindow(browser)) {
    const std::string task_id = shadow->task_id;  // Copied: erase frees it.
    shadows_.erase(task_id);
    return true;
  }
  if (PhiAgentSpaces::Space* space = spaces_.FindByWindow(browser)) {
    const std::string task_id = space->task_id;
    if (watched_task_ == task_id) {
      EndWatch(/*park_agent=*/false);  // Closing: the watcher comes back.
    }
    // The window went under the task (the user closed it, or it died):
    // the Mac's spaceWasDeleted line, then the record goes so a stateless
    // driver stops finding a dead task.
    AppendTranscript(task_id, PhiTranscriptEntry::Kind::kStatus,
                     "Space closed by the user — task ended");
    spaces_.Remove(task_id);
    transcript_.NoteTaskEnded(task_id, EpochMsNow());
    host_.DismissAgentHandoffPrompt(task_id);
    host_.AgentRunStateChanged();
    return true;
  }
  return false;
}

void PhiAgentSpaceManager::HideOverlayBoundTo(
    const PhiShellWindow* controller) {
  PhiAgentOverlay::HideIfBoundTo(controller);
}

void PhiAgentSpaceManager::Sweep(uint64_t now_ms) {
  for (const std::string& task_id : spaces_.Expired(now_ms)) {
    LOG(INFO) << "[PhiClient] agent space expired: " << task_id;
    CompleteTask(task_id, /*success=*/false, /*keep=*/false,
                 "expired: no agent activity");
  }
  // Shadow windows have no user to hold them open: the sweep is their only
  // backstop against an abandoned driver.
  std::vector<std::string> expired_shadows;
  for (const auto& [task_id, shadow] : shadows_) {
    if (shadow.keepalive_deadline_ms && shadow.keepalive_deadline_ms < now_ms) {
      expired_shadows.push_back(task_id);
    }
  }
  for (const std::string& task_id : expired_shadows) {
    CloseShadowWindow(task_id, "keep-alive expired");
  }
}

int64_t PhiAgentSpaceManager::HostingSlotFor(const std::string& task_id,
                                             bool* in_flight) const {
  *in_flight = false;
  if (PhiShellWindow* shell = host_.SlotHostingAgentTask(task_id)) {
    // Hosting for the live watch — or the watch is over and the browser
    // is still in that frame until the swap back lands (FinishUnhost).
    *in_flight = !(hosting_ && watched_task_ == task_id);
    return shell->slot().value;
  }
  if (!hosting_ && watched_task_ == task_id) {
    return watcher_slot_;  // The fallback: standing over its watcher.
  }
  return 0;
}

PhiAgentSpaceSwitchRoute PhiAgentSpaceManager::RouteFor(
    const std::string& task_id,
    int64_t slot) const {
  bool in_flight = false;
  const int64_t host = HostingSlotFor(task_id, &in_flight);
  const bool elsewhere = host != 0 && host != slot;
  return PhiAgentSpaceSwitchRouteFor(
      /*hosts_browser_here=*/host != 0 && host == slot,
      /*another_slot_hosts_browser=*/elsewhere && !in_flight,
      /*another_slot_holds_in_flight=*/elsewhere && in_flight);
}

std::vector<PhiAgentPipView> PhiAgentSpaceManager::Pips(int64_t slot) const {
  std::vector<PhiAgentPipView> out;
  for (const auto& [task_id, space] : spaces_.entries()) {
    if (slot != 0 && !Presents(task_id, slot)) {
      continue;  // On screen in another window: that window's pip alone.
    }
    PhiAgentPipView pip;
    pip.task_id = task_id;
    pip.space_id = space.space_id;
    pip.caption = space.caption;
    // The driver as it declared itself on create, resolved to the Mac's
    // badge: a known brand wears its mark and canonical name, anything
    // else the robot and whatever name arrived.
    const PhiAgentDriverBadge badge = PhiAgentDriverBadgeFor(space.agent_name);
    pip.agent_name = badge.label;
    pip.icon = badge.icon.empty() ? L"\U0001F916"  // 🤖
                                  : PhiAgentIconGlyph(badge.icon);
    pip.number = space.number;
    pip.profile_id = space.profile_id;
    pip.watched = task_id == watched_task_;
    // The Mac's agentBadge precedence.
    using Badge = PhiAgentPipView::Badge;
    if (space.status == "failed" || space.unseen_error) {
      pip.badge = Badge::kError;
    } else if (space.status == "completed") {
      pip.badge = Badge::kCompleted;
    } else if (space.ownership == PhiAgentSpaces::Ownership::kUser) {
      pip.badge = Badge::kUserHolds;
    } else if (space.status == "idle") {
      pip.badge = Badge::kIdle;
    } else {
      pip.badge = Badge::kRunning;
    }
    out.push_back(std::move(pip));
  }
  // Strip order is the task ordinal's — R1, R2, … — the order the strip,
  // the switcher menus, Ctrl+N and the slide direction all read.
  std::stable_sort(out.begin(), out.end(),
                   [](const PhiAgentPipView& a, const PhiAgentPipView& b) {
                     return a.number < b.number;
                   });
  return out;
}

void PhiAgentSpaceManager::RequestWatch(const std::string& task_id,
                                        int64_t watcher_slot) {
  if (!spaces_.Find(task_id)) {
    return;
  }
  // Clicking the watched pip again ends the watch; a different one swaps.
  // A swap asked from INSIDE the watched agent window (its strip lists
  // the other agents too) keeps the user window that was concealed as the
  // watcher — an agent window is never the frame to stand in.
  const bool toggling_off = watched_task_ == task_id;
  if (PhiShellWindow* from = host_.ControllerForSlot(watcher_slot);
      from && from->is_agent() && watcher_slot_ != 0) {
    watcher_slot = watcher_slot_;
  }
  // The route, ahead of every side effect (phi_agent_space_route.h): a
  // Space another window has on screen is surfaced there — ending that
  // window's watch to host it here used to pull the page out of the
  // user's frame, and its toggle-off from the wrong window was the "click
  // did nothing" report — and one still in flight back to its shell is
  // left alone. Only the hosting window's own click toggles below.
  switch (RouteFor(task_id, watcher_slot)) {
    case PhiAgentSpaceSwitchRoute::kLocal:
      break;
    case PhiAgentSpaceSwitchRoute::kSurfaceInHost:
      LOG(INFO) << "[PhiAgent] watch " << task_id
                << ": on screen in another window, surfacing it there";
      SurfaceInHost(task_id);
      return;
    case PhiAgentSpaceSwitchRoute::kDropWhileInFlight:
      LOG(INFO) << "[PhiAgent] watch " << task_id
                << ": browser still in flight back to its shell, ignoring";
      return;
  }
  // Agent to agent in the frame that hosts the watched one: the Mac's
  // slot.activate between two agent Spaces — one swap, the entering
  // browser attached as the hosted one slides out (HostAgentSpace). Ending
  // the watch first slid the window back to the user Space and left the
  // next agent's window to surface on its own: no slide, a second window,
  // and its NOACTIVATE frame never took the keyboard.
  PhiShellWindow* watcher = host_.ControllerForSlot(watcher_slot);
  const bool hand_over = hosting_ && !toggling_off &&
                         watcher_slot == watcher_slot_ && watcher &&
                         watcher->hosted_agent_task() == watched_task_;
  if (!watched_task_.empty() && !hand_over) {
    EndWatch(/*park_agent=*/true);
  }
  if (toggling_off) {
    RefreshOverlay();
    return;
  }
  PhiShellWindow* agent = ControllerFor(task_id);
  PhiAgentSpaces::Space* space = spaces_.Find(task_id);
  if (!agent || !space) {
    return;
  }
  // The user surfaced it: an error they had not seen is seen now (the
  // Mac's userDidSurface).
  space->unseen_error = false;
  const bool usable = watcher && !watcher->is_agent() &&
                      watcher->shell_hwnd() &&
                      (hand_over || !watcher->HostsAgentSpace());
  // The Mac's slot.activate: the agent Space is a Space of the watching
  // slot — its browser moves into that slot and the slot's own Space swap
  // shows it (band slide, pill slide, page reveal). The agent shell keeps
  // the record and gets the child back when the watch ends.
  HWND child = usable ? agent->ReleaseAgentBrowserForHosting() : nullptr;
  if (!child && hand_over) {
    LOG(INFO) << "[PhiAgent] watch " << task_id
              << ": no browser to hand over to, keeping the watch";
    return;  // The hosted one stays on screen, watched.
  }
  if (child) {
    watcher->HostAgentSpace(space->space_id, space->window_id, child,
                            task_id);
    host_.RebindAgentBrowser(space->window_id, watcher_slot);
    watcher_slot_ = watcher_slot;
    hosting_ = true;
  } else {
    // Fallback (a pip clicked from nowhere): the agent window itself,
    // centred on the work area, or over the watcher as its owned window.
    RECT frame = {};
    const bool in_frame = usable &&
                          ::GetWindowRect(watcher->shell_hwnd(), &frame);
    agent->SetAgentVisible(true, in_frame ? &frame : nullptr,
                           in_frame ? watcher->shell_hwnd() : nullptr);
    if (in_frame) {
      watcher_slot_ = watcher_slot;
    }
    hosting_ = false;
  }
  watched_task_ = task_id;
  RefreshOverlay();
  host_.InvalidateUserSlots();
}

void PhiAgentSpaceManager::SurfaceInHost(const std::string& task_id) {
  // The window the Space is on screen in: the hosting user slot, or — in
  // the fallback — the agent window standing over its watcher.
  PhiShellWindow* shell = host_.SlotHostingAgentTask(task_id);
  if (!shell) {
    shell = ControllerFor(task_id);
  }
  if (shell) {
    shell->BringToFront();
  }
  // The user surfaced it (the Mac's userDidSurface): an error they had
  // not seen is seen now.
  if (PhiAgentSpaces::Space* space = spaces_.Find(task_id);
      space && space->unseen_error) {
    space->unseen_error = false;
    host_.InvalidateUserSlots();  // The pip's badge.
  }
}

int64_t PhiAgentSpaceManager::EndWatch(bool park_agent,
                                       const std::string& leave_to) {
  if (watched_task_.empty()) {
    return 0;
  }
  const std::string task_id = watched_task_;
  const bool hosting = hosting_;
  watched_task_.clear();
  hosting_ = false;
  const int64_t slot = watcher_slot_;
  watcher_slot_ = 0;
  PhiAgentOverlay::Hide();
  PhiShellWindow* watcher = host_.ControllerForSlot(slot);
  if (hosting) {
    // The slot swaps back to a user Space with its own animation and hands
    // the browser back when the slide lands (OnAgentBrowserReturned).
    if (watcher && watcher->hosted_agent_task() == task_id) {
      watcher->LeaveHostedAgentSpace(leave_to);
    }
  } else {
    if (park_agent) {
      if (PhiShellWindow* agent = ControllerFor(task_id)) {
        agent->SetAgentVisible(false);
      }
    }
    if (watcher) {
      watcher->SetConcealedForWatch(false);
    }
  }
  host_.InvalidateUserSlots();
  return slot;
}

void PhiAgentSpaceManager::OnAgentBrowserReturned(const std::string& task_id,
                                                  HWND child) {
  PhiAgentSpaces::Space* space = spaces_.Find(task_id);
  PhiShellWindow* agent = space ? host_.ControllerForAgentWindow(
                                      space->window_id)
                                : nullptr;
  if (agent && child && ::IsWindow(child)) {
    agent->ReadoptAgentBrowser(child);  // Back off-screen, still rendering.
  }
  if (space) {
    host_.RebindAgentBrowser(space->window_id, 0);
  }
  if (watched_task_ == task_id) {
    // The hosting window went away under a live watch (it was closed):
    // the watch is over, whatever the record says.
    watched_task_.clear();
    watcher_slot_ = 0;
    hosting_ = false;
    PhiAgentOverlay::Hide();
  }
  // The browser is nobody's again: every window's strip offers the pip
  // once more (Presents — while it was in flight only the hosting window
  // did).
  host_.InvalidateUserSlots();
}

void PhiAgentSpaceManager::RefreshOverlay() {
  if (watched_task_.empty()) {
    PhiAgentOverlay::Hide();
    return;
  }
  PhiAgentSpaces::Space* space = spaces_.Find(watched_task_);
  if (!space) {
    PhiAgentOverlay::Hide();
    return;
  }
  // The overlay rides the window the agent's page is IN: the hosting user
  // slot during a slot-hosted watch, else the agent window itself.
  PhiShellWindow* controller =
      hosting_ ? host_.ControllerForSlot(watcher_slot_)
               : host_.ControllerForAgentWindow(space->window_id);
  if (!controller) {
    PhiAgentOverlay::Hide();
    return;
  }
  const std::wstring label =
      base::UTF8ToWide(PhiAgentDriverBadgeFor(space->agent_name).label);
  const bool agent_owns = space->ownership == PhiAgentSpaces::Ownership::kAgent;
  std::wstring caption = base::UTF8ToWide(space->caption);
  if (caption.empty()) {
    caption = agent_owns ? L"Agent is working\u2026" : L"Agent paused";
  }
  if (space->status == "failed") {
    caption = L"\u26A0 " + caption;
  }
  controller->SetAgentOwned(agent_owns);
  // The capsule's mark and state word (§5.10): the brand glyph the pip
  // wears, and the five states' words.
  const PhiAgentDriverBadge badge = PhiAgentDriverBadgeFor(space->agent_name);
  const std::wstring mark =
      badge.icon.empty() ? std::wstring() : PhiAgentIconGlyph(badge.icon);
  const bool waiting = space->status == "waiting";
  std::wstring status = L"Running";
  if (!agent_owns) {
    status = L"You have control";
  } else if (space->status == "failed") {
    status = L"Error";
  } else if (space->status == "completed") {
    status = L"Done";
  } else if (waiting) {
    status = L"Waiting for you";
  }
  PhiAgentOverlay::Show(controller, watched_task_, label, caption, agent_owns,
                        mark, status, waiting);
}

void PhiAgentSpaceManager::NoteAgentDriving(const std::string& task_id) {
  // The Mac's setAgentMode: the window suppresses activation and fullscreen
  // only while the agent holds it, so a user in control can use them.
  const PhiAgentSpaces::Space* space = spaces_.Find(task_id);
  if (space && framework_) {
    framework_->SetAgentMode(
        space->window_id.value,
        space->ownership == PhiAgentSpaces::Ownership::kAgent);
  }
}

void PhiAgentSpaceManager::RequestTakeControl(const std::string& task_id) {
  if (!spaces_.Find(task_id)) {
    return;
  }
  spaces_.SetOwnership(task_id, PhiAgentSpaces::Ownership::kUser,
                       static_cast<uint64_t>(::GetTickCount64()));
  NoteAgentDriving(task_id);
  AppendTranscript(task_id, PhiTranscriptEntry::Kind::kStatus,
                   "You took control");
  PushOwnershipChanged(task_id);  // The driver must learn it lost control.
  host_.DismissAgentHandoffPrompt(task_id);
  RefreshOverlay();
  host_.InvalidateUserSlots();  // The pip's badge (the raised hand).
}

void PhiAgentSpaceManager::RequestHandBack(const std::string& task_id) {
  PhiAgentSpaces::Space* space = spaces_.Find(task_id);
  if (!space) {
    return;
  }
  // The clock restarts with the inter-round grace (SetOwnership); the
  // agent may take a while to notice — an idle Codex only at its next
  // turn, which the Mac's terminalNudgeHint says on the line.
  spaces_.SetOwnership(task_id, PhiAgentSpaces::Ownership::kAgent,
                       static_cast<uint64_t>(::GetTickCount64()));
  NoteAgentDriving(task_id);
  AppendTranscript(task_id, PhiTranscriptEntry::Kind::kStatus,
                   "You handed control back to the agent",
                   TerminalNudgeHint(*space));
  PushOwnershipChanged(task_id);
  host_.DismissAgentHandoffPrompt(task_id);
  RefreshOverlay();
  host_.InvalidateUserSlots();
}

void PhiAgentSpaceManager::RequestFinish(const std::string& task_id) {
  // The pill's Finish: a successful completion that keeps a persistent
  // Space — the Mac's finishAgentTask(keep: true).
  CompleteTask(task_id, /*success=*/true, /*keep=*/true, std::string());
}

void PhiAgentSpaceManager::CompleteTask(const std::string& task_id,
                                        bool success,
                                        bool keep,
                                        const std::string& message) {
  PhiAgentSpaces::Space* space = spaces_.Find(task_id);
  if (!space) {
    return;
  }
  AppendTranscript(task_id,
                   success ? PhiTranscriptEntry::Kind::kStatus
                           : PhiTranscriptEntry::Kind::kError,
                   success ? "Task completed" : "Task failed", message);
  host_.DismissAgentHandoffPrompt(task_id);
  if (space->persistent && keep) {
    // Marked, not torn down: the hidden window stays for the next
    // re-bind of this taskId. The watch ends — a completed Space is not
    // something to keep standing in the user's frame.
    if (watched_task_ == task_id) {
      EndWatch(/*park_agent=*/true);
    }
    space->status = success ? "completed" : "failed";
    space->caption.clear();
    space->error_message = success ? std::string() : message;
    space->ownership = PhiAgentSpaces::Ownership::kAgent;
    space->keepalive_deadline_ms = 0;
    // Nobody drives a finished Space until the next re-bind.
    if (framework_) {
      framework_->SetAgentMode(space->window_id.value, false);
    }
    transcript_.NoteTaskEnded(task_id, EpochMsNow());
    PhiAgentOverlay::Hide();
    host_.InvalidateUserSlots();
    host_.AgentRunStateChanged();
    return;
  }
  CloseAgentSpace(task_id);
}

void PhiAgentSpaceManager::HandoffToUser(const std::string& task_id,
                                         const std::string& message) {
  if (!spaces_.Find(task_id)) {
    return;
  }
  spaces_.SetOwnership(task_id, PhiAgentSpaces::Ownership::kUser,
                       static_cast<uint64_t>(::GetTickCount64()));
  NoteAgentDriving(task_id);
  AppendTranscript(task_id, PhiTranscriptEntry::Kind::kStatus,
                   "Agent handed control to you", message);
  PushOwnershipChanged(task_id);
  RefreshOverlay();
  host_.InvalidateUserSlots();
  host_.ShowAgentHandoffPrompt(task_id, message);
}

void PhiAgentSpaceManager::ResumeAgentControl(const std::string& task_id) {
  PhiAgentSpaces::Space* space = spaces_.Find(task_id);
  if (!space || space->ownership != PhiAgentSpaces::Ownership::kUser) {
    return;  // Only from user ownership; already the agent's is a no-op.
  }
  const uint64_t now = static_cast<uint64_t>(::GetTickCount64());
  spaces_.SetOwnership(task_id, PhiAgentSpaces::Ownership::kAgent, now);
  NoteAgentDriving(task_id);
  // Explicit: the resume starts a driving round, not a banked wait.
  spaces_.ResetTtl(task_id, PhiAgentSpaces::kDefaultTtlSeconds, now);
  AppendTranscript(task_id, PhiTranscriptEntry::Kind::kStatus,
                   "Agent resumed control");
  PushOwnershipChanged(task_id);
  host_.DismissAgentHandoffPrompt(task_id);
  RefreshOverlay();
  host_.InvalidateUserSlots();
}

void PhiAgentSpaceManager::AppendTranscript(const std::string& task_id,
                                            PhiTranscriptEntry::Kind kind,
                                            const std::string& text,
                                            const std::string& detail,
                                            const std::string& agent) {
  PhiTranscriptEntry entry;
  entry.kind = kind;
  entry.text = text;
  entry.detail = detail;
  entry.agent = agent;
  entry.ts_ms = EpochMsNow();
  if (PhiAgentSpaces::Space* space = spaces_.Find(task_id)) {
    entry.task_number = space->number;
  }
  if (transcript_.Append(task_id, std::move(entry))) {
    PhiTranscriptPanel::RefreshOpen();
  }
}

void PhiAgentSpaceManager::ReapTranscripts(const std::string& keep) {
  transcript_.Reap(EpochMsNow(), keep);
}

// static
std::string PhiAgentSpaceManager::TerminalNudgeHint(
    const PhiAgentSpaces::Space& space) {
  // Codex only reads its queue at its next turn; an idle one has to be
  // woken from its own terminal. Nil for every other driver, as on Mac.
  const std::string name = PhiLower(space.agent_name);
  if (name.find("codex") == std::string::npos || space.status == "running") {
    return std::string();
  }
  return "Codex only sees this at its next turn — go to its terminal and "
         "type \"continue\" to wake it now.";
}

std::vector<PhiTranscriptEntry> PhiAgentSpaceManager::TranscriptEntries(
    const std::string& task_id) {
  if (!task_id.empty()) {
    return transcript_.Entries(task_id);
  }
  // Every task, interleaved in append order.
  std::vector<PhiTranscriptEntry> all;
  for (const PhiTranscriptTaskInfo& info : transcript_.Tasks()) {
    std::vector<PhiTranscriptEntry> one = transcript_.Entries(info.task_id);
    all.insert(all.end(), one.begin(), one.end());
  }
  std::stable_sort(all.begin(), all.end(),
                   [](const PhiTranscriptEntry& a,
                      const PhiTranscriptEntry& b) { return a.seq < b.seq; });
  return all;
}

std::string PhiAgentSpaceManager::TranscriptActivity(
    const std::string& task_id) {
  return transcript_.Activity(task_id);
}

std::vector<PhiTranscriptTaskInfo> PhiAgentSpaceManager::TranscriptTasks() {
  return transcript_.Tasks();
}

std::string PhiAgentSpaceManager::TranscriptAgentName(
    const std::string& task_id) {
  PhiAgentSpaces::Space* space = spaces_.Find(task_id);
  return PhiAgentDriverBadgeFor(space ? space->agent_name : std::string())
      .label;
}

bool PhiAgentSpaceManager::TranscriptTaskLive(const std::string& task_id,
                                              bool* running) {
  PhiAgentSpaces::Space* space = spaces_.Find(task_id);
  if (!space) {
    *running = false;
    return false;
  }
  *running = space->status == "running";
  return true;
}

bool PhiAgentSpaceManager::PushTranscriptMessage(const std::string& task_id,
                                                 const std::string& text) {
  PhiAgentSpaces::Space* space = spaces_.Find(task_id);
  if (!space) {
    return false;
  }
  const std::string id = spaces_.PushMessage(
      task_id, text, base::Time::Now().InMillisecondsSinceUnixEpoch());
  if (id.empty()) {
    return false;
  }
  // The console shows the user's line inline (the Mac's .user echo), and
  // the driver is woken by the advisory event — the drain queue stays
  // authoritative.
  AppendTranscript(task_id, PhiTranscriptEntry::Kind::kUser, text,
                   TerminalNudgeHint(*space));
  base::DictValue payload;
  payload.Set("taskId", task_id);
  payload.Set("id", id);
  payload.Set("text", text);
  std::string payload_json;
  base::JSONWriter::Write(payload, &payload_json);
  host_.BroadcastControlEvent("agentSpace.userMessage", payload_json);
  return true;
}

void PhiAgentSpaceManager::ClearTranscript(const std::string& task_id) {
  transcript_.Clear(task_id);
}

void PhiAgentSpaceManager::TranscriptConsoleClosed() {
  ReapTranscripts(std::string());
}

void PhiAgentSpaceManager::CloseAgentSpace(const std::string& task_id) {
  PhiAgentSpaces::Space* space = spaces_.Find(task_id);
  if (!space) {
    return;
  }
  const BrowserId window_id = space->window_id;
  if (watched_task_ == task_id) {
    EndWatch(/*park_agent=*/false);  // Closing: the watcher comes back.
  }
  spaces_.Remove(task_id);
  transcript_.NoteTaskEnded(task_id, EpochMsNow());
  host_.DismissAgentHandoffPrompt(task_id);
  PhiTranscriptPanel::RefreshOpen();  // "taskId · ended" in the console.
  if (PhiShellWindow* shell = host_.ControllerForAgentWindow(window_id)) {
    shell->RequestClose();  // Fires OnBrowserWindowClosing → cleanup.
  } else if (framework_ && window_id.valid()) {
    framework_->CloseBrowserWindow(window_id.value);
  }
  host_.UpdateSweepTimer();
  host_.InvalidateUserSlots();  // The agent pip disappears.
}

PhiAgentSpaceManager::ShadowWindow* PhiAgentSpaceManager::FindShadowByWindow(
    BrowserId window_id) {
  for (auto& [task_id, shadow] : shadows_) {
    if (shadow.window_id == window_id) {
      return &shadow;
    }
  }
  return nullptr;
}

void PhiAgentSpaceManager::CloseShadowWindow(const std::string& task_id,
                                             const char* reason) {
  auto it = shadows_.find(task_id);
  if (it == shadows_.end()) {
    return;
  }
  const BrowserId window_id = it->second.window_id;
  shadows_.erase(it);
  LOG(INFO) << "[PhiClient] shadow window closed (" << reason
            << "): " << task_id;
  if (PhiShellWindow* shell = host_.ControllerForAgentWindow(window_id)) {
    shell->RequestClose();  // Fires OnBrowserWindowClosing.
  } else if (framework_ && window_id.valid()) {
    framework_->CloseBrowserWindow(window_id.value);
  }
  host_.UpdateSweepTimer();
}

void PhiAgentSpaceManager::CloseAllShadowWindows(const char* reason) {
  std::vector<std::string> task_ids;
  for (const auto& [task_id, shadow] : shadows_) {
    task_ids.push_back(task_id);
  }
  for (const std::string& task_id : task_ids) {
    CloseShadowWindow(task_id, reason);
  }
}

void PhiAgentSpaceManager::TouchShadow(ShadowWindow* shadow,
                                       int ttl_seconds,
                                       uint64_t now_ms) {
  if (ttl_seconds <= 0) {
    ttl_seconds = PhiAgentSpaces::kDefaultTtlSeconds;
  }
  ttl_seconds = std::clamp(ttl_seconds, 1, PhiAgentSpaces::kMaxTtlSeconds);
  shadow->keepalive_deadline_ms =
      std::max(shadow->keepalive_deadline_ms,
               now_ms + static_cast<uint64_t>(ttl_seconds) * 1000);
}

void PhiAgentSpaceManager::PushOwnershipChanged(const std::string& task_id) {
  PhiAgentSpaces::Space* space = spaces_.Find(task_id);
  if (!space) {
    return;  // Ended task: its disappearance from list() is the signal.
  }
  base::DictValue payload;
  payload.Set("taskId", task_id);
  payload.Set("owner", space->ownership == PhiAgentSpaces::Ownership::kUser
                           ? "user"
                           : "agent");
  std::string payload_json;
  base::JSONWriter::Write(payload, &payload_json);
  host_.BroadcastControlEvent("agentSpace.ownershipChanged", payload_json);
}

}  // namespace phi_client

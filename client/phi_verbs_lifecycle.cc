// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <windows.h>

#include <algorithm>
#include <bit>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_agent_overlay.h"
#include "phibrowser-win/client/phi_agent_spaces.h"
#include "phibrowser-win/client/phi_client_analytics.h"
#include "phibrowser-win/client/phi_control_dispatcher.h"
#include "phibrowser-win/client/phi_coordinator.h"
#include "phibrowser-win/client/phi_features.h"
#include "phibrowser-win/client/phi_telemetry.h"
#include "phibrowser-win/client/phi_transcript_panel.h"
#include "phibrowser-win/client/phi_util.h"
#include "phibrowser-win/client/phi_window_capture.h"
#include "phibrowser-win/client/phi_shell_window.h"
#include "phibrowser-win/client/phi_window_registry.h"

// The agent-Space lifecycle, shadow windows, the reader, the task-less
// probes and the box's own hooks — one handler per registry row
// (PhiControlDispatcher::Registry, phi_verb_registry.cc). Every handler
// runs on the UI thread with its scope already resolved and its payload
// already validated against the row's schema; it answers a PhiVerbResult
// and never touches the wire.

namespace phi_client {

namespace {

using ShadowWindow = PhiCoordinator::ShadowWindow;

// The names of the capability bits a principal holds, for
// agentSpace.capabilities.
struct CapabilityName {
  uint32_t bit;
  std::string_view name;
};
constexpr CapabilityName kCapabilityNames[] = {
    {kAgentSpace, "agentSpace"},
    {kUserSpaceRead, "userSpaceRead"},
    {kUserSpaceWrite, "userSpaceWrite"},
    {kDestructive, "destructive"},
    {kAppSettings, "appSettings"},
    {kHost, "host"},
    {kCredentialFill, "credentialFill"},
    {kCredentialReveal, "credentialReveal"},
};

// The profile a new agent or shadow window is created in, for Developer ▸
// "Profiles agents can use". A named profile is itself (the caller checks
// it). "" would let the framework pick its default (the first loaded
// profile, else the first of any) — possibly one the user switched off —
// so it resolves here instead, in the framework's order, to the first
// profile agents may use. Empty when none qualifies.
std::string ResolveAgentProfile(phi_framework::PhiFramework* framework,
                                const PhiPreferences& preferences,
                                const std::string& requested) {
  if (!requested.empty() || !framework) {
    return requested;
  }
  std::optional<base::Value> parsed = base::JSONReader::Read(
      framework->ListProfilesJson(), base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    return std::string();
  }
  std::string first_allowed;
  for (const base::Value& v : parsed->GetList()) {
    const base::DictValue* d = v.GetIfDict();
    const std::string* id = d ? d->FindString("id") : nullptr;
    if (!id || id->empty() || !preferences.AgentProfileAllowed(*id)) {
      continue;
    }
    if (d->FindBool("loaded").value_or(false)) {
      return *id;
    }
    if (first_allowed.empty()) {
      first_allowed = *id;
    }
  }
  return first_allowed;
}

}  // namespace

// --- Liveness and orientation -------------------------------------------------

PhiVerbResult PhiControlDispatcher::VerbPing(const PhiVerbContext& ctx) {
  // The lifecycle phase and the saves still coalescing, so a suite can wait
  // for "running" / 0 instead of sleeping (and never kill the process out
  // from under a write it just asked for).
  base::DictValue out;
  out.Set("phase", PhiCoordinator::AppPhaseName(coordinator_->phase()));
  out.Set("pendingSaves", std::popcount(coordinator_->dirty_stores()));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbList(const PhiVerbContext& ctx) {
  base::DictValue out;
  base::ListValue list;
  for (PhiShellWindow* controller :
       coordinator_->registry().Controllers()) {
    list.Append(static_cast<double>(controller->slot().value));
  }
  out.Set("windows", std::move(list));
  // The skill reads `tasks`; the box probes read `windows`. Ship both.
  // Only the caller's own tasks, unless it is the host: another agent's
  // work is not this one's to see (or complete).
  base::ListValue tasks =
      coordinator_->agent_spaces().TasksJson(ctx.now_ms);
  if (!(ctx.principal.caps & kHost)) {
    base::ListValue mine;
    for (base::Value& row : tasks) {
      const base::DictValue* d = row.GetIfDict();
      const std::string* task_id = d ? d->FindString("taskId") : nullptr;
      if (task_id && ListsTask(ctx.principal, *task_id)) {
        mine.Append(std::move(row));
      }
    }
    tasks = std::move(mine);
  }
  out.Set("tasks", std::move(tasks));
  // Whether the Agent Transcript console is open (the Mac's View ▸ Agent
  // Transcript). The skill lists tasks at every round bind and runs its
  // session-mirror daemon only while this reads true — with the console
  // closed nobody is watching a mirror, and completion is immediate.
  out.Set("transcriptMirror",
          kPhiTranscriptEnabled && PhiTranscriptPanel::IsOpen());
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbCapabilities(
    const PhiVerbContext& ctx) {
  // What this connection may do, in the vocabulary the registry uses: the
  // capability bits it holds right now (the settings toggles are
  // re-evaluated per request), the tasks it owns, the wire it negotiated,
  // and every verb name it could call — the answer an agent needs before it
  // asks for something it is not allowed to have.
  base::DictValue out;
  out.Set("protocol", ctx.principal.protocol);
  out.Set("capabilityId", ctx.principal.capability_id);
  base::ListValue caps;
  for (const CapabilityName& row : kCapabilityNames) {
    if (ctx.principal.caps & row.bit) {
      caps.Append(row.name);
    }
  }
  out.Set("capabilities", std::move(caps));
  // The union across the agent's connections: its CDP session and its
  // control channel are two connections of one principal.
  std::set<std::string> task_ids = ctx.principal.tasks;
  if (const PhiAgentCapability* record =
          FindCapability(ctx.principal.capability_id)) {
    task_ids.insert(record->tasks.begin(), record->tasks.end());
  }
  base::ListValue tasks;
  for (const std::string& task_id : task_ids) {
    tasks.Append(task_id);
  }
  out.Set("tasks", std::move(tasks));
  base::ListValue spaces;
  for (const std::string& space_id : ctx.principal.granted_spaces) {
    spaces.Append(space_id);
  }
  out.Set("grantedSpaces", std::move(spaces));
  base::ListValue verbs;
  std::string_view previous;
  for (const PhiVerbSpec& spec : Registry()) {
    // Two rows can share a name (a selector settles which claims a
    // request); the caller wants the name once.
    if (spec.name == previous) {
      continue;
    }
    if ((CapsToCall(spec) & ~ctx.principal.caps) == 0) {
      verbs.Append(spec.name);
      previous = spec.name;
    }
  }
  out.Set("verbs", std::move(verbs));
  base::ListValue codes;
  for (std::string_view name : PhiErrorCodeNames()) {
    codes.Append(name);
  }
  out.Set("errorCodes", std::move(codes));
  return PhiVerbResult::Ok(std::move(out));
}

// --- The agent-Space lifecycle -------------------------------------------------

PhiVerbResult PhiControlDispatcher::VerbAgentCreate(
    const PhiVerbContext& ctx) {
  const std::string task_id = ctx.params.String("taskId");
  // Re-bind to an existing agent Space if its window is still alive AND
  // still has a tab. A window whose tabs were all closed is a ZOMBIE: on
  // the Mac it is already gone (a tabless window closes), and the skill's
  // heal contract is that re-ensuring the task yields a fresh Space rather
  // than a stranded record — the selftest's "zombie Space heals on
  // re-ensure". The Windows shell keeps a tabless browser alive, so the
  // same contract is kept by closing it here and recreating below.
  if (PhiAgentSpaces::Space* existing =
          coordinator_->agent_spaces().Find(task_id)) {
    const bool live = coordinator_->registry().IsBound(existing->window_id);
    bool zombie = false;
    if (live && framework_) {
      // A window the model has not heard of yet is pulled, not presumed
      // empty — presuming would heal a healthy Space into a fresh one.
      if (!coordinator_->window_model().Has(existing->window_id)) {
        coordinator_->window_model().Refresh(framework_, existing->window_id);
      }
      const phi_framework::WindowSnapshot* window =
          coordinator_->window_model().Get(existing->window_id);
      zombie = !window || window->tabs.empty();
    }
    if (zombie) {
      LOG(INFO) << "[PhiClient] agent space " << task_id
                << " is tabless — healing with a fresh window";
      coordinator_->CloseAgentSpace(task_id);
    }
    if (!zombie && live) {
      coordinator_->agent_spaces().Create(task_id, existing->window_id,
                                          existing->profile_id,
                                          existing->persistent, ctx.now_ms);
      RecordTask(ctx.principal, task_id);  // A re-bind (or an adoption).
      // The Mac counts a re-bind as a task start too (its record is
      // "created or rebound").
      coordinator_->CaptureAnalytics(
          kAnalyticsModuleAgent, kEventAgentTaskStarted,
          PhiAgentTaskProperties("phi_agent", existing->persistent,
                                 existing->agent_name));
      coordinator_->NoteFirstTimeAction(kFirstTimeActionAgentTask);
      base::DictValue out;
      out.Set("spaceId", existing->space_id);
      out.Set("windowId", static_cast<double>(existing->window_id.value));
      out.Set("ownership",
              existing->ownership == PhiAgentSpaces::Ownership::kUser
                  ? "user"
                  : "agent");
      return PhiVerbResult::Ok(std::move(out));
    }
    coordinator_->agent_spaces().Remove(
        task_id);  // Stale record; recreate below.
  }
  const std::string profile_id = ResolveAgentProfile(
      framework_, coordinator_->preferences(),
      ctx.params.String("profileId"));
  const bool persistent = ctx.params.Bool("persistent", false);
  // Developer ▸ "Profiles agents can use". The refusal lands HERE and not
  // at the re-bind above: a Space already open in a profile the user has
  // since switched off keeps working, exactly as the pane's description
  // promises — only the next new Space is stopped. The default profile was
  // resolved above, so it is checked too.
  if (profile_id.empty() ||
      !coordinator_->preferences().AgentProfileAllowed(profile_id)) {
    LOG(INFO) << "[PhiAgent] refused an agent Space in profile "
              << profile_id << " (switched off in Settings)";
    return PhiVerbResult::Fail("profile_not_agent_allowed");
  }
  if (!framework_) {
    return PhiVerbResult::Fail("create_failed");
  }
  if (coordinator_->registry().HasPendingTask(task_id)) {
    return PhiVerbResult::Fail("busy");  // Its window is already on its way.
  }
  // Ask for the hidden agent browser. It arrives later (contract rule 3)
  // in OnBrowserWindowCreated -> HostAgentWindow, which builds the record
  // and answers the deferred reply with the same shape as the re-bind
  // above; the pipe thread waits on it meanwhile.
  PendingCreation pending;
  pending.task_id = task_id;
  pending.persistent = persistent;
  // The driving agent's name rides the create call; the watch-mode pill
  // shows it so a watching user sees WHO holds the Space, not just "an
  // agent" (the Mac's AgentDriverBadge).
  pending.agent_name = ctx.params.String("agentName");
  // The reply slot is handed to the waiter whatever happens: a refused
  // request answers it create_failed inside RequestCreation, and the
  // pipe thread collects that the same way it collects a success.
  pending.pending_reply_id =
      RegisterPendingReply(/*document=*/false, /*include_html=*/false);
  const int reply_id = pending.pending_reply_id;
  // Owned from the request on: the window arrives later, and the owner's
  // CDP session must see it the moment the reply says so.
  RecordTask(ctx.principal, task_id);
  coordinator_->RequestCreation(phi_framework::WindowKind::kAgent, profile_id,
                                /*incognito=*/false, BrowserId(),
                                std::move(pending));
  return PhiVerbResult::Deferred(reply_id);
}

PhiVerbResult PhiControlDispatcher::VerbAgentPing(const PhiVerbContext& ctx) {
  const std::string task_id = ctx.params.String("taskId");
  if (!coordinator_->agent_spaces().Find(task_id)) {
    return PhiVerbResult::Fail("unknown_task");
  }
  coordinator_->agent_spaces().Ping(task_id, ctx.params.Int("ttlSeconds", 0),
                                    ctx.now_ms);
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbAgentComplete(
    const PhiVerbContext& ctx) {
  const std::string task_id = ctx.params.String("taskId");
  PhiAgentSpaces::Space* space = coordinator_->agent_spaces().Find(task_id);
  if (!space) {
    return PhiVerbResult::Fail("unknown_task");
  }
  // The Mac's taskDidComplete: `status` "success" (default) or a failure,
  // `message` the closing transcript line, `keep` (default true) whether a
  // persistent Space survives — see PhiAgentSpaceManager::CompleteTask.
  const bool success = ctx.params.String("status", "success") == "success";
  // Captured BEFORE CompleteTask, which may free the record. Only this
  // path counts (the Mac's "ends through the completion path"): a Space
  // the user closes under a task is not a completion.
  coordinator_->CaptureAnalytics(
      kAnalyticsModuleAgent, kEventAgentTaskCompleted,
      PhiAgentTaskCompletedProperties("phi_agent", space->persistent,
                                      space->agent_name, success));
  coordinator_->agent_manager().CompleteTask(
      task_id, success, ctx.params.Bool("keep", true),
      ctx.params.String("message"));
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbAgentGetOwnership(
    const PhiVerbContext& ctx) {
  const std::string task_id = ctx.params.String("taskId");
  PhiAgentSpaces::Space* space =
      task_id.empty() ? nullptr : coordinator_->agent_spaces().Find(task_id);
  // Unknown/absent task answers "none" (never unknown_task), matching Mac.
  base::DictValue out;
  out.Set("owner", !space ? "none"
                   : space->ownership == PhiAgentSpaces::Ownership::kUser
                       ? "user"
                       : "agent");
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::SetTaskOwnership(const PhiVerbContext& ctx,
                                                     bool to_user) {
  const std::string task_id = ctx.params.String("taskId");
  if (!coordinator_->agent_spaces().Find(task_id)) {
    return PhiVerbResult::Fail("unknown_task");
  }
  coordinator_->agent_spaces().SetOwnership(
      task_id,
      to_user ? PhiAgentSpaces::Ownership::kUser
              : PhiAgentSpaces::Ownership::kAgent,
      ctx.now_ms);
  coordinator_->PushOwnershipChanged(task_id);
  coordinator_->RefreshAgentOverlay();
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbAgentHandoff(
    const PhiVerbContext& ctx) {
  // The agent hands control to the user and asks for them: the transcript
  // line carries its message, and the floating "The agent needs you"
  // prompt offers to switch to the Space.
  const std::string task_id = ctx.params.String("taskId");
  if (!coordinator_->agent_spaces().Find(task_id)) {
    return PhiVerbResult::Fail("unknown_task");
  }
  coordinator_->agent_manager().HandoffToUser(task_id,
                                              ctx.params.String("message"));
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbAgentTakeover(
    const PhiVerbContext& ctx) {
  // The agent resumes after a hand-off — only from user ownership, with a
  // fresh round TTL. The policy (never seize without the user's go-ahead)
  // is the driver's; this only records what it says it did.
  const std::string task_id = ctx.params.String("taskId");
  if (!coordinator_->agent_spaces().Find(task_id)) {
    return PhiVerbResult::Fail("unknown_task");
  }
  coordinator_->agent_manager().ResumeAgentControl(task_id);
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbAgentSetState(
    const PhiVerbContext& ctx) {
  const std::string task_id = ctx.params.String("taskId");
  if (!coordinator_->agent_spaces().Find(task_id)) {
    return PhiVerbResult::Fail("unknown_task");
  }
  coordinator_->agent_spaces().SetState(task_id,
                                        ctx.params.FindString("caption"),
                                        ctx.params.FindString("state"),
                                        ctx.now_ms);
  // The pill shows the caption the agent just set; the pip's badge follows
  // the run state, and Agent Autoview re-evaluates on every edge.
  coordinator_->RefreshAgentOverlay();
  coordinator_->InvalidateAgentPips();
  if (ctx.params.FindString("state")) {
    coordinator_->NoteAgentRunStateChanged();
  }
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbAgentLog(const PhiVerbContext& ctx) {
  const std::string task_id = ctx.params.String("taskId");
  if (!coordinator_->agent_spaces().Find(task_id)) {
    return PhiVerbResult::Fail("unknown_task");
  }
  // A single line (top-level `text` + kind/detail/agent) or an `entries`
  // array, oldest first, capped at 200 — the Mac's agentSpace.log. Rules
  // (AgentSpaceRouter.log): the status/round kinds are the app's own, a
  // driver's line of those kinds is downgraded to an action (a driver
  // cannot forge "You took control"); kind "activity" is the driver's
  // live tail row, control-only for codex/claude and dropped otherwise;
  // `ts` (epoch ms) is clamped to now-24h … now+120s.
  const int64_t now_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
  PhiAgentSpaceManager& manager = coordinator_->agent_manager();
  bool any = false;
  const auto append = [&](const base::DictValue& d) {
    const std::string* kind = d.FindString("kind");
    const std::string* agent = d.FindString("agent");
    const std::string agent_key = agent ? PhiLower(*agent) : std::string();
    if (kind && *kind == "activity") {
      if (agent_key == "codex") {
        const std::string* activity = d.FindString("activity");
        manager.transcript().SetActivity(
            task_id, activity && *activity == "waiting"
                         ? "Codex is waiting for input…"
                         : activity && *activity == "working"
                               ? "Codex is working…"
                               : std::string());
        any = true;
      } else if (agent_key == "claude") {
        const std::string* phase = d.FindString("phase");
        manager.transcript().SetActivity(
            task_id, phase && *phase == "working" ? "Claude is working…"
                                                  : std::string());
        any = true;
      }
      return;
    }
    const std::string* text = d.FindString("text");
    if (!text) {
      return;
    }
    PhiTranscriptEntry entry;
    using Kind = PhiTranscriptEntry::Kind;
    const std::string k = kind ? *kind : "action";
    entry.kind = k == "narration"   ? Kind::kNarration
                 : k == "error"     ? Kind::kError
                 : k == "user"      ? Kind::kUser
                 : k == "assistant" ? Kind::kAssistant
                 : k == "thinking"  ? Kind::kThinking
                 : k == "tool"      ? Kind::kTool
                                    : Kind::kAction;  // + status/round
    entry.text = *text;
    if (const std::string* detail = d.FindString("detail")) {
      entry.detail = *detail;
    }
    entry.agent = agent_key;
    if (const std::string* source = d.FindString("sourceId")) {
      entry.source_id = *source;
    }
    if (const std::string* state = d.FindString("toolState")) {
      entry.tool_state = *state;
    } else if (entry.kind == Kind::kTool) {
      entry.tool_state = "pending";
    }
    const std::optional<double> ts = d.FindDouble("ts");
    entry.ts_ms = ts ? static_cast<int64_t>(*ts) : now_ms;
    entry.ts_ms = std::max(now_ms - 24 * 3600 * 1000LL,
                           std::min(entry.ts_ms, now_ms + 120 * 1000LL));
    if (manager.transcript().Append(task_id, std::move(entry))) {
      any = true;
    }
  };
  if (const base::ListValue* entries = ctx.params.FindList("entries")) {
    size_t count = 0;
    for (const base::Value& v : *entries) {
      if (const base::DictValue* d = v.GetIfDict()) {
        append(*d);
      }
      if (++count >= 200) {
        break;
      }
    }
  } else if (ctx.params.FindString("text")) {
    base::DictValue one;
    for (const char* key : {"text", "kind", "detail", "agent", "sourceId",
                            "toolState"}) {
      if (const std::string* value = ctx.params.FindString(key)) {
        one.Set(key, *value);
      }
    }
    append(one);
  }
  if (!any) {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload);
  }
  // Refresh keep-alive unless touch:false (mirror prose proves the
  // SESSION is alive, not that anyone is driving).
  if (ctx.params.Bool("touch", true)) {
    coordinator_->agent_spaces().Ping(task_id, 0, ctx.now_ms);
  }
  PhiTranscriptPanel::RefreshOpen();
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbAgentMarkError(
    const PhiVerbContext& ctx) {
  const std::string task_id = ctx.params.String("taskId");
  PhiAgentSpaces::Space* space = coordinator_->agent_spaces().Find(task_id);
  if (!space) {
    return PhiVerbResult::Fail("unknown_task");
  }
  // The pill turns red with the message, the pip wears the error badge
  // until the user surfaces the Space (the Mac's hasUnseenError).
  const std::string message = ctx.params.String("message", "error");
  space->status = "failed";
  space->error_message = message;
  space->unseen_error = true;
  coordinator_->agent_manager().AppendTranscript(
      task_id, PhiTranscriptEntry::Kind::kError, message);
  coordinator_->RefreshAgentOverlay();
  coordinator_->InvalidateAgentPips();
  coordinator_->NoteAgentRunStateChanged();
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbAgentReadUserMessages(
    const PhiVerbContext& ctx) {
  const std::string task_id = ctx.params.String("taskId");
  if (!coordinator_->agent_spaces().Find(task_id)) {
    return PhiVerbResult::Fail("unknown_task");
  }
  base::DictValue out;
  out.Set("messages", coordinator_->agent_spaces().DrainMessages(task_id));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbAgentUserMessage(
    const PhiVerbContext& ctx) {
  // Inbound console message injection (used by the mirror / tests); the Mac
  // wire has this as an event, but accepting it as a request lets the
  // console flow work without a push channel.
  const std::string task_id = ctx.params.String("taskId");
  const std::string text = ctx.params.String("text");
  // Queues, echoes the user's line and wakes the driver, exactly as the
  // console's own Send does.
  if (!coordinator_->agent_manager().PushTranscriptMessage(task_id, text)) {
    return PhiVerbResult::Fail("unknown_task");
  }
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbAgentOpenTab(
    const PhiVerbContext& ctx) {
  const std::string task_id = ctx.params.String("taskId");
  PhiAgentSpaces::Space* space = coordinator_->agent_spaces().Find(task_id);
  if (!space || !framework_) {
    return PhiVerbResult::Fail("unknown_task");
  }
  framework_->OpenTab(space->window_id, ctx.params.String("url").c_str());
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbAgentListProfiles(
    const PhiVerbContext& ctx) {
  base::ListValue profiles;
  if (framework_) {
    std::optional<base::Value> parsed = base::JSONReader::Read(
        framework_->ListProfilesJson(), base::JSON_PARSE_RFC);
    if (parsed && parsed->is_list()) {
      for (const base::Value& v : parsed->GetList()) {
        const base::DictValue* d = v.GetIfDict();
        if (!d) {
          continue;
        }
        base::DictValue e;
        const std::string* id = d->FindString("id");
        const std::string* name = d->FindString("name");
        e.Set("profileId", id ? *id : "");
        e.Set("displayName", name ? *name : (id ? *id : ""));
        // Whether an agent may open its own Spaces in this profile —
        // Developer ▸ "Profiles agents can use", the check agentSpace.create
        // and shadow.create make (the skill's listProfiles contract).
        e.Set("agentSpacesAllowed",
              id && coordinator_->preferences().AgentProfileAllowed(*id));
        profiles.Append(std::move(e));
      }
    }
  }
  base::DictValue out;
  out.Set("profiles", std::move(profiles));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbAgentPanelSize(
    const PhiVerbContext& ctx) {
  // The Mac's visible content panel size, so a driver can size its
  // captures to what the user would see: the watched agent window's
  // content, else the first user window's, else no_visible_panel. A
  // {taskId} that names no task is refused like every task verb.
  if (!ctx.scope.valid()) {
    return PhiVerbResult::Fail(ctx.scope.error);
  }
  PhiShellWindow* shell = nullptr;
  const std::string& watched = coordinator_->agent_manager().watched_task();
  if (!watched.empty()) {
    if (PhiAgentSpaces::Space* space =
            coordinator_->agent_spaces().Find(watched)) {
      shell = coordinator_->AgentWindowShell(space->window_id);
    }
  }
  if (!shell) {
    shell = coordinator_->FirstUserShell();
  }
  if (!shell || !shell->shell_hwnd()) {
    return PhiVerbResult::Fail("no_visible_panel");
  }
  const RECT content = shell->ContentRect();
  base::DictValue out;
  out.Set("width", static_cast<int>(content.right - content.left));
  out.Set("height", static_cast<int>(content.bottom - content.top));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbAgentCursor(
    const PhiVerbContext& ctx) {
  // The cursor glide / input-mirror effects render on the WATCHER's
  // overlay (the Mac AgentSpaceOverlayView); with nobody watching they
  // are accepted no-ops so the skill's mirroring never errors.
  const std::string task_id = ctx.params.String("taskId");
  if (!coordinator_->agent_spaces().Find(task_id)) {
    return PhiVerbResult::Fail("unknown_task");
  }
  PhiAgentOverlay::SetAgentCursor(task_id, ctx.params.Double("x", 0),
                                  ctx.params.Double("y", 0));
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbAgentEffect(
    const PhiVerbContext& ctx) {
  const std::string task_id = ctx.params.String("taskId");
  if (!coordinator_->agent_spaces().Find(task_id)) {
    return PhiVerbResult::Fail("unknown_task");
  }
  const std::optional<double> x = ctx.params.FindDouble("x");
  const std::optional<double> y = ctx.params.FindDouble("y");
  PhiAgentOverlay::PlayAgentEffect(
      task_id, ctx.params.String("kind"), x.has_value() && y.has_value(),
      x.value_or(0), y.value_or(0), ctx.params.Double("w", 0),
      ctx.params.Double("h", 0), ctx.params.Double("dy", 1));
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbAgentCaptureWindow(
    const PhiVerbContext& ctx) {
  // Whole-window capture (screenshotBrowser): render the client-drawn
  // chrome and composite the CDP web capture the caller already took over
  // the content rect — the hidden agent window's DComp surface cannot be
  // read back, exactly as the Mac's GPU surface cannot.
  const std::string task_id = ctx.params.String("taskId");
  // `outPath` (a path of the caller's choosing) is the host's; an agent
  // gets the PNG in a fresh file under the user's temp folder instead.
  std::string out_path = ctx.params.String("outPath");
  if (out_path.empty()) {
    wchar_t temp[MAX_PATH + 1] = {};
    const DWORD length = ::GetTempPathW(MAX_PATH + 1, temp);
    if (length == 0 || length > MAX_PATH) {
      return PhiVerbResult::Fail("capture_failed");
    }
    out_path = base::WideToUTF8(std::wstring(temp, length)) +
               "phi-window-" + std::to_string(::GetCurrentProcessId()) + "-" +
               std::to_string(::GetTickCount64()) + ".png";
  }
  PhiAgentSpaces::Space* space = coordinator_->agent_spaces().Find(task_id);
  if (!space || !space->window_id.valid()) {
    return PhiVerbResult::Fail("unknown_task");
  }
  PhiShellWindow* shell =
      coordinator_->registry().Resolve(space->window_id.value);
  if (!shell || !shell->shell_hwnd()) {
    return PhiVerbResult::Fail("no_window");
  }
  const std::string web_path = ctx.params.String("webPath");
  if (!PhiCaptureWindowToPng(
          shell->shell_hwnd(), shell->content_rect(),
          web_path.empty() ? std::wstring() : base::UTF8ToWide(web_path),
          base::UTF8ToWide(out_path))) {
    return PhiVerbResult::Fail("capture_failed");
  }
  base::DictValue out;
  out.Set("path", out_path);
  return PhiVerbResult::Ok(std::move(out));
}

// --- The reader ----------------------------------------------------------------

PhiVerbResult PhiControlDispatcher::ReaderExtract(const PhiVerbContext& ctx,
                                                  bool document) {
  // The skill's readerArticle() / saveArticle(): distil the scope's tab
  // (targetId, else the window's active tab) without navigating it. The
  // distillation is asynchronous, so the reply is DEFERRED.
  const BrowserId browser = ctx.scope.browser;
  if (!ctx.scope.valid() || !browser.valid() || !framework_) {
    return PhiVerbResult::Fail(
        ctx.scope.error.empty() ? "unknown_task" : ctx.scope.error);
  }
  const std::string target = ctx.params.String("targetId");
  const int id =
      RegisterPendingReply(document, ctx.params.Bool("includeHTML", true));
  // Reader View's own pipeline first — the same site rules, ladder and
  // coverage gate the reader button runs, which is what the Mac answers
  // this verb with. It needs a chrome tab id rather than a CDP target.
  if (coordinator_->reader().ExtensionAlive()) {
    const TabId tab_id{!target.empty()
                           ? framework_->TabIdForCdpTarget(target.c_str())
                           : coordinator_->ActiveTabId(browser).value};
    // Settling walks the page for lazy content first: nobody is watching
    // this tab, so the wait costs the caller time and nothing else.
    if (tab_id.valid() && coordinator_->reader().StartExtraction(
                              tab_id.value, id, /*settle=*/true)) {
      return PhiVerbResult::Deferred(id);
    }
  }
  if (!framework_->StartReaderExtraction(browser.value, target.c_str(), id)) {
    CompletePendingReply(id, "{\"ok\":false,\"error\":\"unknown_target\"}");
  }
  return PhiVerbResult::Deferred(id);
}

PhiVerbResult PhiControlDispatcher::VerbReaderArticle(
    const PhiVerbContext& ctx) {
  return ReaderExtract(ctx, /*document=*/false);
}

PhiVerbResult PhiControlDispatcher::VerbReaderDocument(
    const PhiVerbContext& ctx) {
  return ReaderExtract(ctx, /*document=*/true);
}

// --- Shadow windows -------------------------------------------------------------

PhiVerbResult PhiControlDispatcher::VerbShadowList(
    const PhiVerbContext& ctx) {
  // The caller's live shadow windows — the Mac scopes this to the calling
  // driver's principal, and so does this: another agent's background work
  // is not this one's to see. The host sees them all. A {taskId} that
  // names no task is refused, not answered with an empty list.
  if (!ctx.scope.valid()) {
    return PhiVerbResult::Fail(ctx.scope.error);
  }
  base::ListValue rows;
  for (const auto& [task_id, shadow] : coordinator_->shadow_windows()) {
    if (!coordinator_->registry().IsBound(shadow.window_id)) {
      continue;
    }
    if (!(ctx.principal.caps & kHost) && !ListsTask(ctx.principal, task_id)) {
      continue;
    }
    base::DictValue row;
    row.Set("taskId", task_id);
    row.Set("windowId", static_cast<double>(shadow.window_id.value));
    row.Set("profileId", shadow.profile_id);
    row.Set("incognito", shadow.incognito);
    row.Set("createdAt", static_cast<double>(shadow.created_ms));
    rows.Append(std::move(row));
  }
  base::DictValue out;
  out.Set("shadows", std::move(rows));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbShadowCreate(
    const PhiVerbContext& ctx) {
  const std::string task_id = ctx.params.String("taskId");
  const bool incognito = ctx.params.Bool("incognito", false);
  const std::string profile_id = ctx.params.String("profileId");
  // Re-binding to a live shadow window of the same taskId is a success,
  // not a conflict, so a driver's next round reattaches. Its incognito-ness
  // is fixed at creation.
  auto existing = coordinator_->shadow_windows().find(task_id);
  if (existing != coordinator_->shadow_windows().end()) {
    if (coordinator_->registry().IsBound(existing->second.window_id)) {
      if (existing->second.incognito != incognito) {
        return PhiVerbResult::Fail("shadow_incognito_mismatch");
      }
      coordinator_->TouchShadow(&existing->second, 0, ctx.now_ms);
      RecordTask(ctx.principal, task_id);  // A re-bind (or an adoption).
      base::DictValue out;
      out.Set("windowId",
              static_cast<double>(existing->second.window_id.value));
      out.Set("incognito", existing->second.incognito);
      return PhiVerbResult::Ok(std::move(out));
    }
    coordinator_->shadow_windows().erase(
        existing);  // Stale record; recreate below.
  }
  if (!framework_) {
    return PhiVerbResult::Fail("create_failed");
  }
  // Developer ▸ "Profiles agents can use" binds shadow windows as it binds
  // agent Spaces (an Incognito one too: it is that profile's off-the-record
  // session). Like agentSpace.create, only a NEW window is refused.
  const std::string resolved_profile =
      ResolveAgentProfile(framework_, coordinator_->preferences(), profile_id);
  if (resolved_profile.empty() ||
      !coordinator_->preferences().AgentProfileAllowed(resolved_profile)) {
    LOG(INFO) << "[PhiAgent] refused a shadow window in profile "
              << resolved_profile << " (switched off in Settings)";
    return PhiVerbResult::Fail("profile_not_agent_allowed");
  }
  if (coordinator_->registry().HasPendingTask(task_id)) {
    return PhiVerbResult::Fail("busy");  // Its window is already on its way.
  }
  // Ask for the hidden browser; HostAgentWindow files the record when it
  // arrives and answers the deferred reply ({ok, windowId, incognito}).
  PendingCreation pending;
  pending.task_id = task_id;
  pending.pending_reply_id =
      RegisterPendingReply(/*document=*/false, /*include_html=*/false);
  const int reply_id = pending.pending_reply_id;
  RecordTask(ctx.principal, task_id);  // Owned from the request on.
  coordinator_->RequestCreation(phi_framework::WindowKind::kShadow,
                                resolved_profile, incognito, BrowserId(),
                                std::move(pending));  // Answers if refused.
  return PhiVerbResult::Deferred(reply_id);
}

namespace {

// The shadow window `task_id` names when its browser is bound; null
// otherwise — the three verbs below all need one.
ShadowWindow* BoundShadow(PhiCoordinator* coordinator,
                          const std::string& task_id) {
  auto it = coordinator->shadow_windows().find(task_id);
  if (it == coordinator->shadow_windows().end() ||
      !coordinator->registry().IsBound(it->second.window_id)) {
    return nullptr;
  }
  return &it->second;
}

}  // namespace

PhiVerbResult PhiControlDispatcher::VerbShadowOpenTab(
    const PhiVerbContext& ctx) {
  ShadowWindow* shadow =
      BoundShadow(coordinator_, ctx.params.String("taskId"));
  if (!shadow || !framework_) {
    return PhiVerbResult::Fail("unknown_task");
  }
  framework_->OpenTab(shadow->window_id, ctx.params.String("url").c_str());
  coordinator_->TouchShadow(shadow, 0, ctx.now_ms);
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbShadowPing(
    const PhiVerbContext& ctx) {
  ShadowWindow* shadow =
      BoundShadow(coordinator_, ctx.params.String("taskId"));
  if (!shadow) {
    return PhiVerbResult::Fail("unknown_task");
  }
  coordinator_->TouchShadow(shadow, ctx.params.Int("ttlSeconds", 0),
                            ctx.now_ms);
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbShadowClose(
    const PhiVerbContext& ctx) {
  const std::string task_id = ctx.params.String("taskId");
  if (!BoundShadow(coordinator_, task_id)) {
    return PhiVerbResult::Fail("unknown_task");
  }
  coordinator_->CloseShadowWindow(task_id, "driver closed");
  return PhiVerbResult::Ok();
}

// --- Task-less probes -------------------------------------------------------------

PhiVerbResult PhiControlDispatcher::VerbResolveTabIds(
    const PhiVerbContext& ctx) {
  base::ListValue ids;
  if (const base::ListValue* targets = ctx.params.FindList("targetIds")) {
    for (const base::Value& v : *targets) {
      // A target outside the caller's windows names nothing it may see.
      ids.Append(framework_ && v.is_string() &&
                         TargetInScope(ctx.principal, v.GetString())
                     ? framework_->TabIdForCdpTarget(v.GetString().c_str())
                     : -1);
    }
  }
  base::DictValue out;
  out.Set("tabIds", std::move(ids));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbJsDialogState(
    const PhiVerbContext& ctx) {
  // PhiAgentSpace.getJavaScriptDialogState: the Mac serves this inside its
  // DevTools server; cdp.mjs re-routes it to this channel on Windows (the
  // relay is a byte pipe), same result shape.
  const std::string target = ctx.params.String("targetId");
  if (!framework_) {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload);
  }
  std::optional<base::DictValue> state = base::JSONReader::ReadDict(
      framework_->JavaScriptDialogStateJson(target.c_str()),
      base::JSON_PARSE_RFC);
  if (!state || !state->FindBool("found").value_or(false)) {
    return PhiVerbResult::Fail("unknown_target");
  }
  base::DictValue out;
  out.Set("open", state->FindBool("open").value_or(false));
  if (const std::string* dialog_type = state->FindString("type")) {
    out.Set("type", *dialog_type);
  }
  if (const std::string* message = state->FindString("message")) {
    out.Set("message", *message);
  }
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbJsDialogHandle(
    const PhiVerbContext& ctx) {
  const std::string target = ctx.params.String("targetId");
  if (!framework_) {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload);
  }
  const std::string* prompt_text = ctx.params.FindString("promptText");
  bool handled = false;
  if (!framework_->HandleJavaScriptDialog(
          target.c_str(), ctx.params.Bool("accept", false),
          prompt_text ? prompt_text->c_str() : nullptr, &handled)) {
    return PhiVerbResult::Fail("unknown_target");
  }
  base::DictValue out;
  out.Set("handled", handled);
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbTelemetryStatus(
    const PhiVerbContext& ctx) {
  // What the outbound pipeline is doing. `configured:false` is the normal
  // answer until a collector is named in phi_telemetry.json.
  base::DictValue out;
  if (std::optional<base::Value> status = base::JSONReader::Read(
          PhiTelemetry::Get().StatusJson(), base::JSON_PARSE_RFC)) {
    if (base::DictValue* dict = status->GetIfDict()) {
      out.Merge(std::move(*dict));
    }
  }
  return PhiVerbResult::Ok(std::move(out));
}

// --- The box's own hooks -----------------------------------------------------------

PhiVerbResult PhiControlDispatcher::VerbAnalyticsList(
    const PhiVerbContext& ctx) {
  // Everything phi_analytics has delivered this session, oldest first. The
  // only end-to-end observable of the sink while the client has no
  // analytics pipeline: an empty list here means the browser's events are
  // not crossing the boundary.
  base::ListValue events;
  for (const PhiCoordinator::AnalyticsEvent& event :
       coordinator_->analytics_events()) {
    base::DictValue row;
    row.Set("module", event.module);
    row.Set("event", event.name);
    if (std::optional<base::Value> props = base::JSONReader::Read(
            event.properties_json, base::JSON_PARSE_RFC)) {
      row.Set("properties", std::move(*props));
    }
    events.Append(std::move(row));
  }
  base::DictValue out;
  out.Set("events", std::move(events));
  return PhiVerbResult::Ok(std::move(out));
}

// Deliberately a thin passthrough to the framework: the park/materialize
// state machine is phase 3, and these exist so the receipt and the three
// materialize outcomes can be exercised BEFORE any client state depends on
// them.
PhiVerbResult PhiControlDispatcher::VerbLazyRestoreGhosts(
    const PhiVerbContext& ctx) {
  if (!framework_) {
    return PhiVerbResult::Fail("no_framework");
  }
  base::DictValue out;
  if (std::optional<base::Value> parked = base::JSONReader::Read(
          framework_->GhostWindowsJson(), base::JSON_PARSE_RFC)) {
    out.Set("parked", std::move(*parked));
  }
  out.Set("receipt", coordinator_->last_restore_receipt());
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbLazyRestoreRestore(
    const PhiVerbContext& ctx) {
  if (!framework_) {
    return PhiVerbResult::Fail("no_framework");
  }
  std::vector<int> eager;
  if (const base::ListValue* ids = ctx.params.FindList("eagerWindowIds")) {
    for (const base::Value& v : *ids) {
      if (v.is_int()) {
        eager.push_back(v.GetInt());
      }
    }
  }
  coordinator_->ClearLastRestoreReceipt();
  framework_->RestorePreviousSession(ctx.params.String("profile").c_str(),
                                     ctx.params.Bool("armed", false),
                                     eager.empty() ? nullptr : eager.data(),
                                     static_cast<int>(eager.size()));
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::LazyRestoreGhostOp(
    const PhiVerbContext& ctx,
    bool drop) {
  if (!framework_) {
    return PhiVerbResult::Fail("no_framework");
  }
  const std::string profile = ctx.params.String("profile");
  const int window_id = ctx.params.Int("windowId", 0);
  base::DictValue out;
  if (drop) {
    out.Set("dropped",
            framework_->DropGhostWindow(profile.c_str(), window_id));
    return PhiVerbResult::Ok(std::move(out));
  }
  const int outcome =
      framework_->MaterializeGhostWindow(profile.c_str(), window_id);
  // Reported by NAME: "refusedForNow" and "noSuchGhost" mean opposite
  // things to a caller holding a park record (retry vs give up), and a
  // bare number invites collapsing them.
  out.Set("outcome", outcome == 0   ? "materialized"
                     : outcome == 1 ? "refusedForNow"
                                    : "noSuchGhost");
  if (outcome != 0) {
    return PhiVerbResult::Refuse(PhiErrorCode::kBusy, std::move(out));
  }
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbLazyRestoreMaterialize(
    const PhiVerbContext& ctx) {
  return LazyRestoreGhostOp(ctx, /*drop=*/false);
}

PhiVerbResult PhiControlDispatcher::VerbLazyRestoreDrop(
    const PhiVerbContext& ctx) {
  return LazyRestoreGhostOp(ctx, /*drop=*/true);
}

}  // namespace phi_client

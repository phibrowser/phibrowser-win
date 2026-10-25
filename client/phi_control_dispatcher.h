// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_CONTROL_DISPATCHER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_CONTROL_DISPATCHER_H_

#include <windows.h>

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/synchronization/lock.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_app_model.h"
#include "phibrowser-win/client/phi_control_server.h"
#include "phibrowser-win/client/phi_credential_service.h"
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_sync_keys.h"
#include "phibrowser-win/client/phi_verb_registry.h"

namespace phi_framework {
class PhiFramework;
}

namespace phi_client {

class PhiCoordinator;
class PhiShellWindow;

// One request marshalled from the control server's I/O thread to the UI
// thread (a control verb or a peer-consent decision). Defined in the .cc;
// the dispatcher only queues pointers to it.
struct PhiUiCall;

// One accepted pipe connection's identity and grants. Created when
// AuthorizeAgentPeer consents to the connection, retired by
// OnConnectionClosed. Every principal of one agent shares a capability
// (PhiAgentCapability) — the skill opens its CDP session and its control
// channel on separate connections, and the tasks one creates must be
// visible to the other — so task ownership is keyed on `capability_id`,
// not on the connection.
struct PhiPrincipal {
  uint64_t connection_id = 0;
  PhiAgentPeer peer;
  // The agent capability this connection holds ("internal" for the
  // client's own UI). Ownership and DevTools visibility follow it.
  std::string capability_id;
  // PhiCapability bits as last evaluated (per request for verbs; at consent
  // and every scope sync for the DevTools side).
  uint32_t caps = 0;
  // The control wire this connection negotiated on its upgrade (1 or 2);
  // reported by agentSpace.capabilities so an agent can confirm what it got.
  int protocol = 1;
  // Tasks created ON THIS CONNECTION (the capability record carries the
  // union across the agent's connections).
  std::set<std::string> tasks;
  // User Spaces this connection worked in through a user-window scope
  // (the skill's enterContext({kind:'user'})); with kUserSpaceWrite their
  // open windows join the DevTools scope.
  std::set<std::string> granted_spaces;
  // The window set the principal's DevTools sessions were last scoped to
  // (browser ids); empty = nothing but the browser target. Cached here so
  // a session opened on the I/O thread can be scoped at once, before the
  // UI thread's next sync.
  std::set<int64_t> granted_windows;
  // Live DevTools sessions (PhiFramework session ids) of this connection.
  std::vector<uint64_t> devtools_sessions;
};

// The agent capability behind X-Phi-Agent-Capability (lane 5 §4.1): the
// principal a connection acts as, echoed on its 101 and presented by the
// agent's later connections and its delegated helpers to join the same
// principal. Windows cannot tell one local program from another (no
// code-signature identity — see AuthorizeAgentPeerOnUi), so there is ONE
// capability per Phi session for every admitted connection that presents
// none; task ownership and window scoping then separate agents from the
// user, not agents from each other. IN-MEMORY: records die with the process.
struct PhiAgentCapability {
  uint64_t minted_tick = 0;   // GetTickCount64 ms
  uint64_t expires_tick = 0;  // minted + 24 h
  // Union of the tasks and user Spaces across the connections that hold it.
  std::set<std::string> tasks;
  std::set<std::string> granted_spaces;
};

// The control plane (docs/architecture-review.md §3.4 "PhiControlDispatcher",
// §3.7): the typed verb registry behind the /phi-agent channel, the
// agent-peer consent decision, the per-connection principals with their
// capabilities and task ownership, and the deferred-reply map, behind the
// pipe server's PhiControlDelegate.
//
// One request runs ONE pipeline (Execute): parse → look the verb up in the
// registry (PhiVerbSpec, Registry()) → resolve its scope once
// (ResolveScope) → the capability gate on that scope (AuthorizeVerb) →
// validate the payload against the verb's schema (PhiParams) → serialise
// prompting verbs → the handler → wrap. Every verb is one handler member
// (the Verb* methods below, grouped by family in phi_verbs_*.cc) reached
// only through the table; the handlers reach the rest of the app through
// the coordinator's public accessors. The pipe server owns the wire
// (protocol v1 or v2); this class never sees an envelope.
//
// Threading: HandleControlCommand / AuthorizeAgentPeer / the session and
// connection notices run on the control server's connection threads.
// Verbs and consent marshal to the UI thread by POSTING to a message window
// of the dispatcher's own; everything else runs on the UI thread, from the
// main loop only (never inside a nested modal loop). The pending-reply map
// and the principal map are the only state touched from both sides.
class PhiControlDispatcher : public PhiControlDelegate {
 public:
  // Who is asking: the pipe (an agent — gated by its principal's
  // capabilities) or the client's own UI, which calls the verbs as its
  // internal API under a principal that holds every capability.
  enum class ControlCaller { kInternal, kPipe };

  // What a verb is aimed at, resolved ONCE per request (ResolveScope) from
  // the payload's {taskId} → {windowId} → {spaceId} → nothing, and used
  // both by the capability gate (a user window needs the verb's user
  // capability, an agent/shadow window only kAgentSpace plus ownership)
  // and by the handlers.
  struct Scope {
    enum class Kind { kAgent, kShadow, kUser };
    // The shell hosting `browser`; null when the id names nothing live.
    RAW_PTR_EXCLUSION PhiShellWindow* controller = nullptr;
    BrowserId browser;
    Kind kind = Kind::kUser;
    // The skill's taskId for an agent/shadow scope (from the payload, or
    // from the window's record when only a windowId was given).
    std::string task_id;
    // The user Space the scope shows, when known.
    std::string space_id;
    // Empty when the scope names a live window; else the handler's answer
    // (unknown_task / space_not_open / window_not_open / invalid_payload).
    std::string error;
    // The payload carried no scope key at all (and the verb has no
    // default): the gate treats the verb as app-wide and the schema says
    // what is missing.
    bool unscoped = false;
    bool valid() const { return error.empty(); }
  };

  // UI thread. `coordinator` owns the dispatcher and outlives it;
  // `framework` is the framework command interface, fixed for the process.
  // Creates the message window the I/O thread posts to.
  PhiControlDispatcher(PhiCoordinator* coordinator,
                       phi_framework::PhiFramework* framework);
  ~PhiControlDispatcher() override;

  PhiControlDispatcher(const PhiControlDispatcher&) = delete;
  PhiControlDispatcher& operator=(const PhiControlDispatcher&) = delete;

  // The verb table (phi_verb_registry.cc): every verb the control plane
  // serves, in one place. Static so verbs.md and agentSpace.capabilities
  // read the same rows the dispatcher does.
  static base::span<const PhiVerbSpec> Registry();

  // PhiControlDelegate (called on the control server's connection threads):
  PhiVerbResult HandleControlCommand(uint64_t connection_id,
                                     const std::string& type,
                                     base::DictValue payload) override;
  bool AuthorizeAgentPeer(uint64_t connection_id,
                          const PhiAgentPeer& peer,
                          std::string* capability_id) override;
  // The user changed "Allow agents to control Phi": drop this session's
  // remembered "Deny" so the next connection asks (or is admitted) afresh.
  // UI thread.
  void ForgetAgentControlDenial() { agent_control_denied_ = false; }
  // The user turned "Allow agents to control Phi" OFF: every live agent
  // connection is cut and every capability minted this session expires, so
  // nothing admitted before the switch keeps driving — the Mac's
  // AgentCDPListener.revokeActiveAccess. An agent that connects later
  // meets the switch (the turn-on prompt) like a stranger. UI thread.
  void RevokeAgentAccess();
  void OnControlProtocol(uint64_t connection_id, int protocol) override;
  void OnDevToolsSessionOpened(uint64_t connection_id,
                               uint64_t session_id) override;
  void OnDevToolsSessionClosed(uint64_t connection_id,
                               uint64_t session_id) override;
  std::vector<bool> DevToolsTargetsInScope(
      uint64_t connection_id,
      const std::vector<PhiDevToolsTargetRef>& targets) override;
  bool DevToolsMayCreateTargets(uint64_t connection_id) override;
  std::optional<std::set<uint64_t>> EventAudience(
      const std::string& type,
      const base::DictValue& payload) override;
  void OnConnectionClosed(uint64_t connection_id) override;

  // Runs a control command on the UI thread; returns the legacy (v1) JSON
  // body — the coordinator's own UI calls the verbs through this as its
  // internal API (Space switch / create) and reads `id` and friends off
  // the flat body. A deferred verb answers `{"pendingReply":<id>}`.
  // `connection_id` names the pipe principal (ControlCaller::kPipe); the
  // internal caller passes none.
  std::string ExecuteControlOnUi(const std::string& type,
                                 const std::string& payload_json,
                                 ControlCaller caller,
                                 uint64_t connection_id = 0);

  // --- Deferred control replies -------------------------------------------
  // A verb whose answer arrives asynchronously (reader extraction, a window
  // or profile creation) returns PhiVerbResult::Deferred(id) from the UI
  // thread; HandleControlCommand (the pipe I/O thread) then waits on the
  // id's event and returns what the UI thread stored for it.
  // UI thread: registers a slot and returns its id.
  int RegisterPendingReply(bool document, bool include_html);
  // UI thread: stores the answer (legacy JSON) and wakes the waiter (no-op
  // if it gave up). The coordinator answers window/profile creations and
  // reader articles through this. Re-syncs the DevTools scopes first: a
  // task window that just arrived must be visible to its owner's sessions
  // BEFORE the owner learns of it from the reply.
  void CompletePendingReply(int id, std::string json);
  // The live connections acting as capability `capability_id` — where an
  // event meant for one agent session goes. Any thread.
  std::set<uint64_t> ConnectionsOfCapability(
      const std::string& capability_id);
  // UI thread: the reader options a slot was registered with — false when
  // the waiter already gave up and released it.
  bool PendingReplyShape(int id, bool* document, bool* include_html);

 private:
  // The model's value types under the names the handlers have always used.
  using Space = model::Space;
  using CredentialAccessKind = model::CredentialAccessKind;

  // What "no scope at all" means: the client dialect's "the first user
  // window" (the verbs that take windowId 0), or nothing.
  enum class ScopeFallback { kNone, kFirstUserWindow };
  Scope ResolveScope(const base::DictValue& payload, ScopeFallback fallback);

  static LRESULT CALLBACK MsgWndProc(HWND hwnd,
                                     UINT msg,
                                     WPARAM wparam,
                                     LPARAM lparam);

  // Runs `call` now, or queues it while a nested modal loop is on the UI
  // thread (PhiModalDepth() > 0), a prompting verb is in flight, or the
  // queue is non-empty, and arms kDeferredUiCallTimer to drain the queue
  // once the loop unwinds. The UI side owns its reference to the call and
  // releases it after running it.
  void RunOrDeferUiCall(PhiUiCall* call);
  // The shell window a consent prompt is owned by; null before the first.
  HWND PromptOwner();
  // True when an admission would prompt but no window exists to ask in —
  // RunUiCall then parks the call in awaiting_window_.
  bool AuthorizeWaitsForWindow();
  // kAwaitWindowTimer: runs the parked admissions once a window exists (or
  // the switch answers them), and lets go of those whose waiter gave up.
  void ResumeAuthorizationsAwaitingWindow();
  void RunUiCall(PhiUiCall* call);
  void DrainDeferredUiCalls();
  // I/O thread: waits up to `timeout_ms` and returns the stored reply (or
  // a timeout failure), releasing the slot either way.
  PhiVerbResult WaitPendingReply(int id, DWORD timeout_ms);

  // The pipeline (UI thread): the registry lookup, the scope, the gate,
  // the schema, the prompt mutex and the handler, for one request.
  PhiVerbResult Execute(const std::string& type,
                        const base::DictValue& payload,
                        ControlCaller caller,
                        uint64_t connection_id);

  // Agent admission: the UI-thread half of AuthorizeAgentPeer. A presented
  // live capability joins its principal; otherwise the "Allow agents to
  // control Phi" setting (or the test rig) admits the connection outright,
  // and when it is off the user is asked ONCE whether to turn it on. No
  // identity check: the peer's image path is shown and logged, never
  // judged. On success the connection's principal is registered under the
  // capability it now holds (returned in `capability_id`).
  bool AuthorizeAgentPeerOnUi(uint64_t connection_id,
                              const PhiAgentPeer& peer,
                              std::string* capability_id);

  // --- Principals ----------------------------------------------------------
  // The capability bits a pipe principal holds RIGHT NOW, from the
  // coordinator's settings (see PhiCapability).
  uint32_t EvaluatePipeCaps() const;
  // The principal behind a request: the internal one (every capability)
  // for the client's own UI, else a copy of the connection's — with
  // connection_id 0 when the connection was never consented.
  PhiPrincipal PrincipalFor(ControlCaller caller, uint64_t connection_id);
  // The capability record `id` names, if it is live; expired records are
  // dropped on the way.
  PhiAgentCapability* FindCapability(const std::string& id);
  // Registers the connection's principal under `capability_id` (UI thread;
  // takes principals_lock_) and scopes it.
  void RegisterPrincipal(uint64_t connection_id,
                         const PhiAgentPeer& peer,
                         const std::string& capability_id);
  // True when `principal` may act on `task_id`: it holds kHost, the task
  // is owned by its capability, or nobody owns the task (an orphan of an
  // expired capability, adoptable by whoever touches it first).
  bool OwnsTask(const PhiPrincipal& principal, const std::string& task_id);
  // The driving agent's name for the analytics: the agentName of a task
  // the principal opened, else its process image's file stem. Raw — the
  // capture site reduces it to a brand label (PhiAgentTelemetryName).
  std::string AgentNameFor(const PhiPrincipal& principal);
  // Records `task_id` as created by `principal` (its connection, its
  // capability, and the owner map).
  void RecordTask(const PhiPrincipal& principal, const std::string& task_id);
  // Records that `principal` worked in user Space `space_id`.
  void GrantSpace(const PhiPrincipal& principal, const std::string& space_id);
  // Drops owner entries whose task is no longer live (the coordinator's
  // sweep closes tasks without telling the dispatcher).
  void PruneTaskOwners();
  // The capability gate, run before any handler for a pipe caller, on the
  // scope Execute already resolved. False with `*code` set to the refusal;
  // true with `drive_tag` set to the tag the agent drive context should
  // carry (empty: do not arm) when the verb mutates a USER window on an
  // agent's (never the host's) behalf.
  bool AuthorizeVerb(const PhiPrincipal& principal,
                     const PhiVerbSpec& spec,
                     const PhiParams& params,
                     const Scope& scope,
                     PhiErrorCode* code,
                     std::string* drive_tag);
  // The capability bits a principal needs to call `spec` on the most
  // permissive scope open to it (its own tasks for a task-scoped verb).
  static uint32_t CapsToCall(const PhiVerbSpec& spec);
  // The browser ids a principal's DevTools sessions may see: the agent and
  // shadow windows of the tasks its capability owns, plus — only with
  // kUserSpaceWrite — the open windows of the Spaces it entered. Empty for
  // kHost (unrestricted, `*unrestricted` set).
  std::set<int64_t> VisibleWindows(const PhiPrincipal& principal,
                                   bool* unrestricted);
  // UI thread: re-evaluates every principal's caps and window set and
  // pushes changed scopes to their DevTools sessions
  // (PhiFramework::SetDevToolsSessionScope). Runs after every pipe verb,
  // at every deferred-reply completion, and on kScopeSyncTimer while any
  // session is open — the settings toggles and the keep-alive sweep change
  // the answer without going through a verb.
  void SyncDevToolsScopes();
  // Pushes `principal`'s cached scope to one session (any thread).
  void ApplySessionScope(const PhiPrincipal& principal, uint64_t session_id);
  // THE target check, for /json/list, the CDP session open and every verb
  // that takes a `targetId`: true when the DevTools target's tab sits in a
  // window `principal` may see (VisibleWindows), always for an unrestricted
  // one. An unknown target, or one with no tab (a worker), is out of scope
  // for everyone else. UI thread; reads only, so it may run inside a nested
  // modal loop.
  bool TargetInScope(const PhiPrincipal& principal,
                     const std::string& target_id);
  // TargetInScope's tab lookup against an already computed window set.
  bool TargetWindowIn(const std::string& target_id,
                      const std::set<int64_t>& windows);
  // The UI-thread half of DevToolsTargetsInScope.
  std::vector<bool> TargetsInScopeOnUi(
      uint64_t connection_id,
      const std::vector<PhiDevToolsTargetRef>& targets);

  // The registry's trampoline. PhiVerbSpec cannot hold a pointer-to-member
  // (see PhiVerbHandler): the spec type is complete before this class is
  // declared. Every row names an instantiation of this instead — private,
  // like the handlers it reaches, so the table needs no `friend`.
  template <PhiVerbResult (PhiControlDispatcher::*Method)(
      const PhiVerbContext&)>
  static PhiVerbResult Call(PhiControlDispatcher* self,
                            const PhiVerbContext& ctx) {
    return (self->*Method)(ctx);
  }

  // --- Shared handler helpers ----------------------------------------------
  // Whether `principal` owns `task_id` per task_owners_ (no host bypass,
  // no adoption) — the filter the list verbs apply.
  bool ListsTask(const PhiPrincipal& principal,
                 const std::string& task_id) const;
  // The (profile, space) the skill's pinned/bookmark dialect verbs target:
  // an explicit profile id wins, else the focused window's active Space.
  // Not a window scope (a profile has no window), so not ResolveScope's.
  bool DialectOwner(const std::string& profile_id_or_empty,
                    std::string* profile_id, std::string* space_id);
  // The Space `id` names in the spaces model, or null.
  Space* FindSpace(const std::string& id);
  bool SpaceExists(const std::string& id);
  // The user Space window a spaces.* dialect payload names: kind == kUser
  // with a known Space, else the refusal token in `*error`.
  BrowserId UserWindowOf(const Scope& scope,
                         std::string* space_out,
                         std::string* error);
  // The active tab of `browser` as the skill's {tabId, url, title} row.
  base::DictValue ActiveTabRow(BrowserId browser);
  // The reader verbs' shared body (`document` picks readerDocument).
  PhiVerbResult ReaderExtract(const PhiVerbContext& ctx, bool document);
  // Ownership transitions shared by handoff/takeover.
  PhiVerbResult SetTaskOwnership(const PhiVerbContext& ctx, bool to_user);
  // The three shadow verbs that need a bound window.
  PhiVerbResult NavigateHistory(const PhiVerbContext& ctx,
                                std::string_view op);
  PhiVerbResult TabByIndex(const PhiVerbContext& ctx, std::string_view op);
  PhiVerbResult GroupByIndex(const PhiVerbContext& ctx, bool group);
  PhiVerbResult GroupOp(const PhiVerbContext& ctx, std::string_view op);
  PhiVerbResult TabGroupsOp(const PhiVerbContext& ctx, std::string_view op);
  PhiVerbResult SplitViewOp(const PhiVerbContext& ctx, std::string_view op);
  PhiVerbResult DownloadsOp(const PhiVerbContext& ctx, std::string_view op);
  PhiVerbResult PinnedTabsOp(const PhiVerbContext& ctx, std::string_view op);
  PhiVerbResult BookmarkTreeOp(const PhiVerbContext& ctx,
                               std::string_view op);
  PhiVerbResult BookmarkTreeAdd(const PhiVerbContext& ctx, bool folder);
  PhiVerbResult UrlRuleEdit(const PhiVerbContext& ctx, bool remove);
  PhiVerbResult StoredRowRemove(const PhiVerbContext& ctx);
  PhiVerbResult LazyRestoreGhostOp(const PhiVerbContext& ctx, bool drop);

  // --- The handlers (one per registry row) ---------------------------------
  // phi_verbs_lifecycle.cc: the agent-Space lifecycle, shadow windows, the
  // reader, the task-less probes and the host hooks.
  PhiVerbResult VerbPing(const PhiVerbContext& ctx);
  PhiVerbResult VerbList(const PhiVerbContext& ctx);
  PhiVerbResult VerbCapabilities(const PhiVerbContext& ctx);
  PhiVerbResult VerbAgentCreate(const PhiVerbContext& ctx);
  PhiVerbResult VerbAgentPing(const PhiVerbContext& ctx);
  PhiVerbResult VerbAgentComplete(const PhiVerbContext& ctx);
  PhiVerbResult VerbAgentGetOwnership(const PhiVerbContext& ctx);
  PhiVerbResult VerbAgentHandoff(const PhiVerbContext& ctx);
  PhiVerbResult VerbAgentTakeover(const PhiVerbContext& ctx);
  PhiVerbResult VerbAgentSetState(const PhiVerbContext& ctx);
  PhiVerbResult VerbAgentLog(const PhiVerbContext& ctx);
  PhiVerbResult VerbAgentMarkError(const PhiVerbContext& ctx);
  PhiVerbResult VerbAgentReadUserMessages(const PhiVerbContext& ctx);
  PhiVerbResult VerbAgentUserMessage(const PhiVerbContext& ctx);
  PhiVerbResult VerbAgentOpenTab(const PhiVerbContext& ctx);
  PhiVerbResult VerbAgentListProfiles(const PhiVerbContext& ctx);
  PhiVerbResult VerbAgentPanelSize(const PhiVerbContext& ctx);
  PhiVerbResult VerbAgentCursor(const PhiVerbContext& ctx);
  PhiVerbResult VerbAgentEffect(const PhiVerbContext& ctx);
  PhiVerbResult VerbAgentCaptureWindow(const PhiVerbContext& ctx);
  PhiVerbResult VerbReaderArticle(const PhiVerbContext& ctx);
  PhiVerbResult VerbReaderDocument(const PhiVerbContext& ctx);
  PhiVerbResult VerbShadowList(const PhiVerbContext& ctx);
  PhiVerbResult VerbShadowCreate(const PhiVerbContext& ctx);
  PhiVerbResult VerbShadowOpenTab(const PhiVerbContext& ctx);
  PhiVerbResult VerbShadowPing(const PhiVerbContext& ctx);
  PhiVerbResult VerbShadowClose(const PhiVerbContext& ctx);
  PhiVerbResult VerbResolveTabIds(const PhiVerbContext& ctx);
  PhiVerbResult VerbJsDialogState(const PhiVerbContext& ctx);
  PhiVerbResult VerbJsDialogHandle(const PhiVerbContext& ctx);
  PhiVerbResult VerbTelemetryStatus(const PhiVerbContext& ctx);
  PhiVerbResult VerbAnalyticsList(const PhiVerbContext& ctx);
  PhiVerbResult VerbLazyRestoreGhosts(const PhiVerbContext& ctx);
  PhiVerbResult VerbLazyRestoreRestore(const PhiVerbContext& ctx);
  PhiVerbResult VerbLazyRestoreMaterialize(const PhiVerbContext& ctx);
  PhiVerbResult VerbLazyRestoreDrop(const PhiVerbContext& ctx);
  // phi_verbs_spaces.cc: the user's Spaces, profiles, rules, rows, pins
  // and bookmarks.
  PhiVerbResult VerbSpacesList(const PhiVerbContext& ctx);
  PhiVerbResult VerbSpacesCreate(const PhiVerbContext& ctx);
  PhiVerbResult VerbSpacesSwitch(const PhiVerbContext& ctx);
  PhiVerbResult VerbSpacesRename(const PhiVerbContext& ctx);
  PhiVerbResult VerbSpacesDelete(const PhiVerbContext& ctx);
  PhiVerbResult VerbSpacesReorder(const PhiVerbContext& ctx);
  PhiVerbResult VerbSpacesSetTheme(const PhiVerbContext& ctx);
  PhiVerbResult VerbSpacesSetIcon(const PhiVerbContext& ctx);
  PhiVerbResult VerbSpacesListTabs(const PhiVerbContext& ctx);
  PhiVerbResult VerbSpacesOpenTab(const PhiVerbContext& ctx);
  PhiVerbResult VerbSpacesFocus(const PhiVerbContext& ctx);
  PhiVerbResult VerbSpacesActivate(const PhiVerbContext& ctx);
  PhiVerbResult VerbSpacesUpdate(const PhiVerbContext& ctx);
  PhiVerbResult VerbProfilesList(const PhiVerbContext& ctx);
  PhiVerbResult VerbProfilesCreate(const PhiVerbContext& ctx);
  PhiVerbResult VerbProfilesRename(const PhiVerbContext& ctx);
  PhiVerbResult VerbProfilesDelete(const PhiVerbContext& ctx);
  PhiVerbResult VerbUrlRulesList(const PhiVerbContext& ctx);
  PhiVerbResult VerbUrlRulesAdd(const PhiVerbContext& ctx);
  PhiVerbResult VerbUrlRulesUpdate(const PhiVerbContext& ctx);
  PhiVerbResult VerbUrlRulesDelete(const PhiVerbContext& ctx);
  PhiVerbResult VerbPinnedScopeGet(const PhiVerbContext& ctx);
  PhiVerbResult VerbPinnedScopeSet(const PhiVerbContext& ctx);
  PhiVerbResult VerbPinnedList(const PhiVerbContext& ctx);
  PhiVerbResult VerbPinnedOpen(const PhiVerbContext& ctx);
  PhiVerbResult VerbPinnedAdd(const PhiVerbContext& ctx);
  PhiVerbResult VerbPinnedRemove(const PhiVerbContext& ctx);
  PhiVerbResult VerbBookmarksListFlat(const PhiVerbContext& ctx);
  PhiVerbResult VerbBookmarksAddFlat(const PhiVerbContext& ctx);
  PhiVerbResult VerbBookmarksListTree(const PhiVerbContext& ctx);
  PhiVerbResult VerbBookmarksAddTree(const PhiVerbContext& ctx);
  PhiVerbResult VerbBookmarksAddFolder(const PhiVerbContext& ctx);
  PhiVerbResult VerbBookmarksUpdate(const PhiVerbContext& ctx);
  PhiVerbResult VerbBookmarksMove(const PhiVerbContext& ctx);
  PhiVerbResult VerbBookmarksRemove(const PhiVerbContext& ctx);
  PhiVerbResult VerbPinnedTabsList(const PhiVerbContext& ctx);
  PhiVerbResult VerbPinnedTabsAdd(const PhiVerbContext& ctx);
  PhiVerbResult VerbPinnedTabsUpdate(const PhiVerbContext& ctx);
  PhiVerbResult VerbPinnedTabsRemove(const PhiVerbContext& ctx);
  PhiVerbResult VerbRowsReorder(const PhiVerbContext& ctx);
  PhiVerbResult VerbEmojiGroups(const PhiVerbContext& ctx);
  PhiVerbResult VerbEmojiList(const PhiVerbContext& ctx);
  // phi_verbs_window.cc: windows, tabs, navigation, groups, split view,
  // downloads, and the app-wide looks and settings.
  PhiVerbResult VerbNewWindow(const PhiVerbContext& ctx);
  PhiVerbResult VerbClose(const PhiVerbContext& ctx);
  PhiVerbResult VerbNavigate(const PhiVerbContext& ctx);
  PhiVerbResult VerbWindowProfile(const PhiVerbContext& ctx);
  PhiVerbResult VerbWindowLayout(const PhiVerbContext& ctx);
  PhiVerbResult VerbWindowBounds(const PhiVerbContext& ctx);
  PhiVerbResult VerbWindowSetBounds(const PhiVerbContext& ctx);
  PhiVerbResult VerbWindowHandle(const PhiVerbContext& ctx);
  PhiVerbResult VerbMenuList(const PhiVerbContext& ctx);
  PhiVerbResult VerbMenuClick(const PhiVerbContext& ctx);
  PhiVerbResult VerbTabsList(const PhiVerbContext& ctx);
  PhiVerbResult VerbTabsNew(const PhiVerbContext& ctx);
  PhiVerbResult VerbTabsActivate(const PhiVerbContext& ctx);
  PhiVerbResult VerbTabsClose(const PhiVerbContext& ctx);
  PhiVerbResult VerbTabsSplit(const PhiVerbContext& ctx);
  PhiVerbResult VerbTabsUnsplit(const PhiVerbContext& ctx);
  PhiVerbResult VerbTabsMove(const PhiVerbContext& ctx);
  PhiVerbResult VerbTabsGroup(const PhiVerbContext& ctx);
  PhiVerbResult VerbTabsUngroup(const PhiVerbContext& ctx);
  PhiVerbResult VerbTabsStoreSplit(const PhiVerbContext& ctx);
  PhiVerbResult VerbNavState(const PhiVerbContext& ctx);
  PhiVerbResult VerbNavBack(const PhiVerbContext& ctx);
  PhiVerbResult VerbNavForward(const PhiVerbContext& ctx);
  PhiVerbResult VerbNavReload(const PhiVerbContext& ctx);
  PhiVerbResult VerbNavStop(const PhiVerbContext& ctx);
  PhiVerbResult VerbGroupsList(const PhiVerbContext& ctx);
  PhiVerbResult VerbGroupsRename(const PhiVerbContext& ctx);
  PhiVerbResult VerbGroupsColor(const PhiVerbContext& ctx);
  PhiVerbResult VerbGroupsCollapse(const PhiVerbContext& ctx);
  PhiVerbResult VerbGroupsUngroup(const PhiVerbContext& ctx);
  PhiVerbResult VerbGroupsClose(const PhiVerbContext& ctx);
  PhiVerbResult VerbTabGroupsList(const PhiVerbContext& ctx);
  PhiVerbResult VerbTabGroupsCreate(const PhiVerbContext& ctx);
  PhiVerbResult VerbTabGroupsUpdate(const PhiVerbContext& ctx);
  PhiVerbResult VerbTabGroupsAddTabs(const PhiVerbContext& ctx);
  PhiVerbResult VerbTabGroupsRemoveTabs(const PhiVerbContext& ctx);
  PhiVerbResult VerbTabGroupsUngroup(const PhiVerbContext& ctx);
  PhiVerbResult VerbTabGroupsClose(const PhiVerbContext& ctx);
  PhiVerbResult VerbSplitViewList(const PhiVerbContext& ctx);
  PhiVerbResult VerbSplitViewCreate(const PhiVerbContext& ctx);
  PhiVerbResult VerbSplitViewUpdate(const PhiVerbContext& ctx);
  PhiVerbResult VerbSplitViewSwap(const PhiVerbContext& ctx);
  PhiVerbResult VerbSplitViewRemove(const PhiVerbContext& ctx);
  PhiVerbResult VerbDownloadsList(const PhiVerbContext& ctx);
  PhiVerbResult VerbDownloadsGet(const PhiVerbContext& ctx);
  PhiVerbResult VerbDownloadsPause(const PhiVerbContext& ctx);
  PhiVerbResult VerbDownloadsResume(const PhiVerbContext& ctx);
  PhiVerbResult VerbDownloadsCancel(const PhiVerbContext& ctx);
  PhiVerbResult VerbDownloadsRemove(const PhiVerbContext& ctx);
  PhiVerbResult VerbLayoutGet(const PhiVerbContext& ctx);
  PhiVerbResult VerbLayoutSet(const PhiVerbContext& ctx);
  PhiVerbResult VerbThemeGet(const PhiVerbContext& ctx);
  PhiVerbResult VerbThemeSet(const PhiVerbContext& ctx);
  PhiVerbResult VerbSettingsGet(const PhiVerbContext& ctx);
  PhiVerbResult VerbSettingsSet(const PhiVerbContext& ctx);
  PhiVerbResult VerbSettingsOpen(const PhiVerbContext& ctx);
  PhiVerbResult VerbFeedbackStatus(const PhiVerbContext& ctx);
  PhiVerbResult VerbFeedbackOpen(const PhiVerbContext& ctx);
  PhiVerbResult VerbFeedbackForm(const PhiVerbContext& ctx);
  PhiVerbResult VerbFeedbackSetAccount(const PhiVerbContext& ctx);
  // phi_verbs_account.cc: the Auth0 session (test rig only).
  PhiVerbResult VerbAccountStatus(const PhiVerbContext& ctx);
  PhiVerbResult VerbAccountSignIn(const PhiVerbContext& ctx);
  PhiVerbResult VerbAccountCancelSignIn(const PhiVerbContext& ctx);
  PhiVerbResult VerbAccountSignOut(const PhiVerbContext& ctx);
  PhiVerbResult VerbOnboardingStatus(const PhiVerbContext& ctx);
  PhiVerbResult VerbOnboardingDrive(const PhiVerbContext& ctx);
  PhiVerbResult VerbSyncStatus(const PhiVerbContext& ctx);
  PhiVerbResult VerbSyncSetProfileKey(const PhiVerbContext& ctx);
  PhiVerbResult VerbSyncKeyOp(const PhiVerbContext& ctx);
  PhiVerbResult VerbSyncSetupOpen(const PhiVerbContext& ctx);
  PhiVerbResult VerbSyncSetupStatus(const PhiVerbContext& ctx);
  PhiVerbResult VerbSyncSetupDrive(const PhiVerbContext& ctx);
  PhiVerbResult VerbImportOpen(const PhiVerbContext& ctx);
  PhiVerbResult VerbImportStatus(const PhiVerbContext& ctx);
  PhiVerbResult VerbImportDrive(const PhiVerbContext& ctx);
  // Records a finished key operation for agentSpace.sync.status.
  void FinishKeyOp(int seq, keys::KeyStatus status, base::DictValue result);
  // phi_verbs_credentials.cc: the vault.
  PhiVerbResult VerbCredentialsStatus(const PhiVerbContext& ctx);
  PhiVerbResult VerbCredentialsGet(const PhiVerbContext& ctx);
  PhiVerbResult VerbCredentialsGetTotp(const PhiVerbContext& ctx);
  PhiVerbResult VerbCredentialsAutofill(const PhiVerbContext& ctx);
  // phi_verbs_notifications.cc: agent notification cards.
  PhiVerbResult VerbNotificationShow(const PhiVerbContext& ctx);
  // The Mac's agent_credential_access_requested (every decision) and
  // _approved (the allowed ones), with the caller's brand label.
  void CaptureCredentialDecision(const PhiVerbContext& ctx,
                                 const char* kind,
                                 const PhiCredentialService::Decision& decision);

  // The owner; outlives this.
  RAW_PTR_EXCLUSION PhiCoordinator* coordinator_ = nullptr;
  // The framework command interface (a singleton in chrome.dll); non-owning,
  // across a module boundary, so not a raw_ptr.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  // Message-only window on the UI thread; control commands marshal here.
  HWND msg_hwnd_ = nullptr;
  // Drains deferred_ui_calls_ once no modal loop is up.
  static constexpr UINT_PTR kDeferredUiCallTimer = 1;
  static constexpr UINT kDeferredUiCallPollMs = 50;
  // Re-syncs the DevTools scopes while any session is open (see
  // SyncDevToolsScopes).
  static constexpr UINT_PTR kScopeSyncTimer = 2;
  static constexpr UINT kScopeSyncMs = 1000;
  // Polls for the first window while an admission waits for one.
  static constexpr UINT_PTR kAwaitWindowTimer = 3;
  static constexpr UINT kAwaitWindowPollMs = 250;
  // Verbs and consent decisions that arrived while a modal loop was on the
  // UI thread, in arrival order (see RunOrDeferUiCall).
  std::deque<PhiUiCall*> deferred_ui_calls_;
  // Admissions parked until a window can own the turn-on prompt.
  std::vector<PhiUiCall*> awaiting_window_;
  bool draining_ui_calls_ = false;
  // The prompt mutex: a verb whose spec says `prompts` is running (its
  // dialog may be up); every other call parks in the deferred queue until
  // it returns, so two prompts never stack and a verb never runs under a
  // prompt's nested loop. UI thread only.
  bool prompt_in_flight_ = false;
  // The user answered "Deny" to the turn-on prompt this session: later
  // connections are refused without another dialog until the user touches
  // the switch in Settings (ForgetAgentControlDenial). UI thread only.
  bool agent_control_denied_ = false;
  // The agent capabilities live this session, by id, and the one every
  // admitted connection that presents none is issued — the skill's CDP
  // session, its control channel, and its next round all act as this one
  // principal. UI thread only.
  std::map<std::string, PhiAgentCapability> capabilities_;
  std::string session_capability_id_;
  // taskId → the capability that created it. UI thread only; pruned of
  // dead tasks lazily (PruneTaskOwners).
  std::map<std::string, std::string> task_owners_;
  // The live principals by connection id — the second piece of state
  // touched from the connection threads (session and connection notices)
  // and the UI thread (consent, verbs, sync).
  base::Lock principals_lock_;
  std::map<uint64_t, PhiPrincipal> principals_;
  // The deferred-reply slots (see RegisterPendingReply). Touched from two
  // threads.
  struct PendingReply {
    HANDLE event = nullptr;  // Manual-reset; owned by the I/O waiter.
    std::string json;
    bool document = false;      // readerDocument (else readerArticle)
    bool include_html = true;   // readerArticle {includeHTML}
  };
  base::Lock pending_replies_lock_;
  std::map<int, PendingReply> pending_replies_;
  int next_pending_reply_ = 1;
  // agentSpace.sync.keyOp's last operation (the key layer answers
  // asynchronously; agentSpace.sync.status reports it), and the approvals
  // its last refresh listed, which approve/deny pick from. UI thread.
  int key_op_seq_ = 0;
  base::DictValue key_op_;
  std::vector<keys::PendingApproval> key_op_pending_;
};

// What a handler receives: the caller, the scope the dispatcher resolved,
// the typed payload view, and the row that named it. Stack-scoped to one
// request on the UI thread; the references outlive the handler.
struct PhiVerbContext {
  RAW_PTR_EXCLUSION const PhiPrincipal& principal;
  RAW_PTR_EXCLUSION const PhiControlDispatcher::Scope& scope;
  RAW_PTR_EXCLUSION const PhiParams& params;
  RAW_PTR_EXCLUSION const PhiVerbSpec& spec;
  PhiControlDispatcher::ControlCaller caller;
  // GetTickCount64() at dispatch.
  uint64_t now_ms;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CONTROL_DISPATCHER_H_

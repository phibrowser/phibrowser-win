// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_control_dispatcher.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <optional>
#include <string_view>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_agent_overlay.h"
#include "phibrowser-win/client/phi_agent_spaces.h"
#include "phibrowser-win/client/phi_client_analytics.h"
#include "phibrowser-win/client/phi_coordinator.h"
#include "phibrowser-win/client/phi_credentials.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_feedback_outbox.h"
#include "phibrowser-win/client/phi_feedback_window.h"
#include "phibrowser-win/client/phi_features.h"
#include "phibrowser-win/client/phi_folder_icons.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_app_menu.h"
#include "phibrowser-win/client/phi_modal_scope.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_settings_window.h"
#include "phibrowser-win/client/phi_tab_store.h"
#include "phibrowser-win/client/phi_telemetry.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_transcript_panel.h"
#include "phibrowser-win/client/phi_window_capture.h"
#include "phibrowser-win/client/phi_shell_window.h"
#include "phibrowser-win/client/phi_window_registry.h"

namespace phi_client {

namespace {
// The coordinator's record types under the names the ladder has always
// used (they are the coordinator's: the verbs file and read them, the
// coordinator's creation paths and sweep answer them).
using PendingProfile = PhiCoordinator::PendingProfile;
using ShadowWindow = PhiCoordinator::ShadowWindow;
using AnalyticsEvent = PhiCoordinator::AnalyticsEvent;

constexpr wchar_t kMsgClassName[] = L"PhiClientControlDispatch";
constexpr UINT WM_PHI_CONTROL = WM_APP + 1;
constexpr UINT WM_PHI_AUTHORIZE = WM_APP + 2;
// A connection thread asking the UI thread to re-sync the DevTools scopes
// (a session opened or closed); arms kScopeSyncTimer while any is open.
constexpr UINT WM_PHI_SCOPE_SYNC = WM_APP + 3;
// A connection thread asking which DevTools targets its principal may see
// (/json/list, a session open). Read-only, so it runs at once — even inside
// a nested modal loop — rather than queueing behind a prompt.
constexpr UINT WM_PHI_TARGET_SCOPE = WM_APP + 4;
// How long a minted agent capability stays presentable.
constexpr uint64_t kCapabilityTtlMs = 24ull * 60 * 60 * 1000;

// How long the I/O thread waits for the UI thread to run a marshalled call.
// Generous on purpose: a consent or credential prompt can sit for minutes,
// and the skill itself waits up to 180 s for consent.
constexpr DWORD kUiCallTimeoutMs = 180 * 1000;

// How long the I/O thread waits for a deferred reply the UI thread promised
// (a reader extraction, a window or profile creation). Outlasts the
// coordinator's own creation timeout plus a sweep tick, so a lost creation
// answers create_timeout rather than timeout.
constexpr DWORD kPendingReplyTimeoutMs = 40 * 1000;
}  // namespace

// The request marshalled from the control server's I/O thread to the UI
// thread. POSTED, never sent: a cross-thread SendMessage is serviced by
// whatever message loop the UI thread happens to be pumping — a context
// menu's, a prompt's — so a verb could run, and destroy a controller, under
// that controller's own stack frame (docs/architecture-review.md §4 item
// 11). A posted message is only dispatched from the main loop, and
// RunOrDeferUiCall parks it when a nested loop is up.
//
// Heap-owned by TWO references, the I/O waiter's and the UI thread's, so
// whichever finishes last frees it: a waiter that gave up (timeout) must
// not pull the call from under a UI thread still about to run it, and the
// UI thread must not free a result the waiter is about to read.
struct PhiUiCall {
  enum class Kind { kControl, kAuthorize, kTargetScope };
  Kind kind = Kind::kControl;
  // The pipe connection asking (its principal's key).
  uint64_t connection_id = 0;
  // kControl: the verb and its stringified payload / reply.
  std::string type;
  std::string payload;
  std::string result;
  // kAuthorize: the peer asking for consent, the answer, and the agent
  // capability the connection holds when allowed.
  PhiAgentPeer peer;
  bool allowed = false;
  std::string capability_id;
  // kTargetScope: the targets asked about and one answer per target.
  std::vector<PhiDevToolsTargetRef> targets;
  std::vector<bool> in_scope;
  // When a kAuthorize call was parked waiting for a window (GetTickCount64).
  uint64_t parked_tick = 0;
  // Manual-reset; signalled once the UI thread has run the call.
  HANDLE done = nullptr;
  std::atomic<int> refs{2};

  void Release() {
    if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      if (done) {
        ::CloseHandle(done);
      }
      delete this;
    }
  }
};

namespace {
// Allocates a call with its event; null when the event could not be made.
PhiUiCall* NewUiCall(PhiUiCall::Kind kind) {
  PhiUiCall* call = new PhiUiCall;
  call->kind = kind;
  call->done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!call->done) {
    delete call;
    return nullptr;
  }
  return call;
}

// Waits for the UI thread to run `call` (I/O thread). True when it did;
// false on timeout, in which case the UI side still owns its reference and
// will run and free the call on its own later — the waiter must not touch
// the call after this returns false beyond releasing its own reference.
bool WaitUiCall(PhiUiCall* call) {
  return ::WaitForSingleObject(call->done, kUiCallTimeoutMs) == WAIT_OBJECT_0;
}
}  // namespace

PhiControlDispatcher::PhiControlDispatcher(
    PhiCoordinator* coordinator,
    phi_framework::PhiFramework* framework)
    : coordinator_(coordinator), framework_(framework) {
  // Message-only window on the UI thread (the coordinator constructs the
  // dispatcher from OnFrameworkReady, which runs on it), so the control
  // server's I/O thread can marshal verbs and consent decisions here by
  // posting (see PhiUiCall).
  static bool class_registered = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &PhiControlDispatcher::MsgWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kMsgClassName;
    return ::RegisterClassExW(&wc) != 0;
  }();
  (void)class_registered;
  msg_hwnd_ = ::CreateWindowExW(0, kMsgClassName, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr,
                                ::GetModuleHandleW(nullptr), this);
}

PhiControlDispatcher::~PhiControlDispatcher() {
  // Consent requests still waiting for a window are refused.
  for (PhiUiCall* call : awaiting_window_) {
    ::SetEvent(call->done);
    call->Release();
  }
  awaiting_window_.clear();
  if (msg_hwnd_) {
    // Detach first: a call still in the queue is answered not_ready by the
    // window procedure rather than run against a dead dispatcher.
    ::SetWindowLongPtrW(msg_hwnd_, GWLP_USERDATA, 0);
    ::DestroyWindow(msg_hwnd_);
    msg_hwnd_ = nullptr;
  }
}

// static
LRESULT CALLBACK PhiControlDispatcher::MsgWndProc(HWND hwnd,
                                                  UINT msg,
                                                  WPARAM wparam,
                                                  LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_TIMER) {
    auto* self = reinterpret_cast<PhiControlDispatcher*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && wparam == kDeferredUiCallTimer) {
      self->DrainDeferredUiCalls();
    } else if (self && wparam == kScopeSyncTimer) {
      self->SyncDevToolsScopes();
    } else if (self && wparam == kAwaitWindowTimer) {
      self->ResumeAuthorizationsAwaitingWindow();
    }
    return 0;
  }
  if (msg == WM_PHI_SCOPE_SYNC) {
    auto* self = reinterpret_cast<PhiControlDispatcher*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
      self->SyncDevToolsScopes();
    }
    return 0;
  }
  if (msg == WM_PHI_TARGET_SCOPE) {
    auto* self = reinterpret_cast<PhiControlDispatcher*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    auto* call = reinterpret_cast<PhiUiCall*>(lparam);
    if (!call) {
      return 0;
    }
    if (self) {
      call->in_scope =
          self->TargetsInScopeOnUi(call->connection_id, call->targets);
    }
    ::SetEvent(call->done);
    call->Release();
    return 0;
  }
  if (msg == WM_PHI_CONTROL || msg == WM_PHI_AUTHORIZE) {
    auto* self = reinterpret_cast<PhiControlDispatcher*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    auto* call = reinterpret_cast<PhiUiCall*>(lparam);
    if (!call) {
      return 0;
    }
    if (self) {
      self->RunOrDeferUiCall(call);
    } else {
      // No dispatcher behind the window any more: answer "refused" so the
      // waiter does not sit out the whole timeout, and drop our reference.
      call->result = "{\"ok\":false,\"error\":\"not_ready\"}";
      ::SetEvent(call->done);
      call->Release();
    }
    return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

PhiVerbResult PhiControlDispatcher::HandleControlCommand(
    uint64_t connection_id,
    const std::string& type,
    base::DictValue payload) {
  // Called on the control server's I/O thread. Marshal to the UI thread by
  // POSTING (see PhiUiCall) and wait for its event; the UI side runs one
  // Execute from the main loop only. The call carries the payload as JSON
  // because that is what crosses the thread boundary cheaply and what the
  // deferred-reply slots already speak; the UI side parses it once.
  if (!msg_hwnd_) {
    return PhiVerbResult::Fail("not_ready");
  }
  PhiUiCall* call = NewUiCall(PhiUiCall::Kind::kControl);
  if (!call) {
    return PhiVerbResult::Fail(PhiErrorCode::kInternal);
  }
  call->connection_id = connection_id;
  call->type = type;
  base::JSONWriter::Write(payload, &call->payload);
  if (!::PostMessageW(msg_hwnd_, WM_PHI_CONTROL, 0,
                      reinterpret_cast<LPARAM>(call))) {
    call->Release();  // The UI side's reference: it will never take it.
    call->Release();
    return PhiVerbResult::Fail("not_ready");
  }
  if (!WaitUiCall(call)) {
    call->Release();
    return PhiVerbResult::Fail(PhiErrorCode::kTimeout);
  }
  const std::string body = std::move(call->result);
  call->Release();
  PhiVerbResult result = PhiVerbResult::FromLegacyJson(body);
  // A deferred reply: the UI side registered a slot and started the work
  // (a reader extraction, a window or profile creation the framework will
  // announce later); wait for it HERE, on the I/O thread, and hand back
  // what it stored. The wait outlasts kCreateTimeoutMs plus a sweep tick,
  // so a lost creation answers create_timeout rather than timeout.
  if (result.deferred()) {
    return WaitPendingReply(result.pending_reply, kPendingReplyTimeoutMs);
  }
  return result;
}

bool PhiControlDispatcher::AuthorizeAgentPeer(uint64_t connection_id,
                                              const PhiAgentPeer& peer,
                                              std::string* capability_id) {
  // I/O thread; the decision (and the turn-on prompt) runs on the UI
  // thread, posted like a verb so it never lands inside a nested loop.
  // While the prompt is up every later request waits in the deferred
  // queue and then finds the setting the answer flipped (or the session's
  // "Deny") instead of raising a second dialog — one prompt, however many
  // connections arrive at once.
  capability_id->clear();
  if (!msg_hwnd_) {
    return false;
  }
  PhiUiCall* call = NewUiCall(PhiUiCall::Kind::kAuthorize);
  if (!call) {
    return false;
  }
  call->connection_id = connection_id;
  call->peer = peer;
  if (!::PostMessageW(msg_hwnd_, WM_PHI_AUTHORIZE, 0,
                      reinterpret_cast<LPARAM>(call))) {
    call->Release();
    call->Release();
    return false;
  }
  if (!WaitUiCall(call)) {
    call->Release();
    return false;  // Nobody answered in time: no consent.
  }
  const bool allowed = call->allowed;
  if (allowed) {
    *capability_id = std::move(call->capability_id);
  }
  call->Release();
  return allowed;
}

bool PhiControlDispatcher::AuthorizeAgentPeerOnUi(uint64_t connection_id,
                                                  const PhiAgentPeer& peer,
                                                  std::string* capability_id) {
  // NO IDENTITY CHECK. The Mac keys consent on the peer's code signature
  // (team + signing id, walked up to the responsible agent process); the
  // only thing Windows can attach to a pipe client is its pid and image
  // path, and an image path names nothing — the skill always arrives as
  // node.exe, and any program can be renamed. So the peer's path is shown
  // in the prompt and written to the log for the user's benefit, and the
  // decision is the one switch: Developer ▸ "Allow agents to control Phi".
  // The launch token (a process that can read the profile folder) and the
  // pipe ACL are the whole admission test once that switch is on.
  const std::string who = base::WideToUTF8(
      peer.image_path.empty() ? std::wstring(L"<unknown image>")
                              : peer.image_path) +
      " (pid " + std::to_string(peer.pid) + ")";
  // A live capability the peer presented (lane 5 §4.1): the agent's own
  // later connection, or a helper it delegated to, joins the principal the
  // capability names. The id is a secret Phi handed to an admitted process
  // over the pipe, so presenting it is admission in itself.
  const model::PhiSettings& settings = coordinator_->settings();
  // A capability only ever stands in for the switch it was minted under:
  // with the switch off it admits nothing (and RevokeAgentAccess has
  // dropped it anyway), so an agent cut off by the switch cannot walk back
  // in on the id it was handed.
  if (!peer.presented_capability.empty() && settings.allow_agent_control) {
    if (FindCapability(peer.presented_capability)) {
      *capability_id = peer.presented_capability;
      RegisterPrincipal(connection_id, peer, *capability_id);
      return true;
    }
    LOG(INFO) << "[PhiAgent] unknown or expired capability presented by "
              << who << "; admitting on the setting instead";
  }
  // The one capability every admitted connection of this session shares —
  // minted on the first, reused by the rest (the skill's CDP session and
  // control channel are two connections of one agent, and a later round
  // is a new process that must still see the tasks the last one created).
  auto admit = [&]() {
    if (session_capability_id_.empty() ||
        !FindCapability(session_capability_id_)) {
      session_capability_id_ =
          base::HexEncodeLower(base::RandBytesAsVector(32));
      PhiAgentCapability& record = capabilities_[session_capability_id_];
      record.minted_tick = ::GetTickCount64();
      record.expires_tick = record.minted_tick + kCapabilityTtlMs;
    }
    *capability_id = session_capability_id_;
    LOG(INFO) << "[PhiAgent] admitted " << who;
    RegisterPrincipal(connection_id, peer, *capability_id);
    return true;
  };
  // The switch alone decides. The test rig used to admit as well, which
  // made the switch a no-op on a seeded profile — off must mean off; the
  // harness seeds allowAgentControl beside testRig (seed-fx-prefs.sh) and
  // keeps testRig for its own verbs only.
  if (settings.allow_agent_control) {
    return admit();
  }
  if (agent_control_denied_) {
    LOG(INFO) << "[PhiAgent] refused " << who
              << " (agent control off, denied this session)";
    return false;
  }
  // The switch is off: ask once whether to turn it on — the Mac's consent
  // prompt "offers to turn it on" the same way, so the skill never has to
  // send the user into Settings first. Allow flips the setting (persisted,
  // shown in Developer ▸ Agent control); Deny refuses this and every later
  // connection until the user touches the switch. Owned by a shell window
  // when one exists; a connection that arrives before any window waits for
  // one (RunUiCall parks it) instead of meeting a MessageBox any program
  // could click.
  std::wstring message =
      L"A program wants to connect to Phi as an agent:\n" +
      base::UTF8ToWide(who) +
      L"\n\nAllowing turns on \u201CAllow agents to control Phi\u201D in "
      L"Settings \u25B8 Developer. Any program running as you that can "
      L"read Phi\u2019s profile folder may then open its own agent Spaces "
      L"and drive them";
  if (settings.allow_agent_user_spaces) {
    message += L", and read and change your own Spaces";
  }
  message += L". Phi cannot tell such programs apart, so this stays on "
             L"for every one of them until you turn it off.";
  HWND owner = PromptOwner();
  if (!owner) {
    // Only reachable if the window went away after the call was parked
    // for it: refuse this one, remember nothing.
    LOG(INFO) << "[PhiAgent] refused " << who << " (no window to ask in)";
    return false;
  }
  // Deny is the default action, so a stray Enter refuses.
  const std::vector<std::wstring> options = {L"Allow", L"Deny"};
  const int deny_index = 1;
  const int chosen =
      PhiPromptChoose(owner, L"Agent access request", message.c_str(),
                      options, deny_index, L"Continue", L"Cancel",
                      /*cancel_is_default=*/true,
                      /*require_hardware_input=*/true);
  if (chosen == 0) {
    coordinator_->SetAllowAgentControl(true);  // Persists; Settings refreshes.
    return admit();
  }
  LOG(INFO) << "[PhiAgent] user denied " << who;
  agent_control_denied_ = true;
  return false;
}

void PhiControlDispatcher::RunOrDeferUiCall(PhiUiCall* call) {
  // A nested modal loop is pumping (a context menu, a prompt, a MessageBox
  // — every one wrapped in PhiModalScope): do NOT run the verb inside it.
  // Park it and poll for the loop to unwind. A prompting verb in flight
  // parks newcomers for the same reason even before its dialog is up, and a
  // non-empty queue parks them too, so calls run in arrival order.
  if (PhiModalDepth() > 0 || prompt_in_flight_ || draining_ui_calls_ ||
      !deferred_ui_calls_.empty()) {
    deferred_ui_calls_.push_back(call);
    if (msg_hwnd_) {
      ::SetTimer(msg_hwnd_, kDeferredUiCallTimer, kDeferredUiCallPollMs,
                 nullptr);
    }
    return;
  }
  RunUiCall(call);
}

void PhiControlDispatcher::RunUiCall(PhiUiCall* call) {
  switch (call->kind) {
    case PhiUiCall::Kind::kControl:
      call->result = ExecuteControlOnUi(call->type, call->payload,
                                        ControlCaller::kPipe,
                                        call->connection_id);
      break;
    case PhiUiCall::Kind::kAuthorize:
      if (AuthorizeWaitsForWindow()) {
        // The turn-on prompt needs a window to ask in; startup connections
        // arrive before the first one. Parked, not answered: the waiter
        // keeps waiting (up to kUiCallTimeoutMs).
        call->parked_tick = ::GetTickCount64();
        awaiting_window_.push_back(call);
        if (msg_hwnd_) {
          ::SetTimer(msg_hwnd_, kAwaitWindowTimer, kAwaitWindowPollMs,
                     nullptr);
        }
        return;
      }
      call->allowed = AuthorizeAgentPeerOnUi(call->connection_id, call->peer,
                                             &call->capability_id);
      break;
    case PhiUiCall::Kind::kTargetScope:
      // Never queued (WM_PHI_TARGET_SCOPE answers at once); fail closed.
      call->in_scope.assign(call->targets.size(), false);
      break;
  }
  ::SetEvent(call->done);
  call->Release();  // The UI side's reference; the waiter holds the other.
}

HWND PhiControlDispatcher::PromptOwner() {
  for (PhiShellWindow* controller : coordinator_->registry().Controllers()) {
    if (controller->shell_hwnd()) {
      return controller->shell_hwnd();
    }
  }
  return nullptr;
}

bool PhiControlDispatcher::AuthorizeWaitsForWindow() {
  // Exactly the case that would prompt (switch off, no "Deny" this
  // session) with nothing to own the prompt.
  return !coordinator_->settings().allow_agent_control &&
         !agent_control_denied_ && !PromptOwner();
}

void PhiControlDispatcher::ResumeAuthorizationsAwaitingWindow() {
  if (!awaiting_window_.empty() && AuthorizeWaitsForWindow()) {
    // Still no window: let go of the ones whose waiter has given up.
    const uint64_t now = ::GetTickCount64();
    std::vector<PhiUiCall*> keep;
    for (PhiUiCall* call : awaiting_window_) {
      if (now - call->parked_tick >= kUiCallTimeoutMs) {
        ::SetEvent(call->done);  // `allowed` stays false.
        call->Release();
      } else {
        keep.push_back(call);
      }
    }
    awaiting_window_.swap(keep);
    if (!awaiting_window_.empty()) {
      return;
    }
  }
  if (msg_hwnd_) {
    ::KillTimer(msg_hwnd_, kAwaitWindowTimer);
  }
  std::vector<PhiUiCall*> calls;
  calls.swap(awaiting_window_);
  for (PhiUiCall* call : calls) {
    RunOrDeferUiCall(call);  // Prompts now, or meets the switch's answer.
  }
}

void PhiControlDispatcher::DrainDeferredUiCalls() {
  // The timer also ticks INSIDE the modal loop it is waiting out (a nested
  // loop pumps every message); the depth check is what keeps the calls
  // parked until it has unwound. A drained call may itself open a prompt
  // (credentials, consent): the loop re-checks after each one and leaves
  // the rest for the next tick.
  if (PhiModalDepth() > 0 || prompt_in_flight_ || draining_ui_calls_) {
    return;
  }
  draining_ui_calls_ = true;
  while (!deferred_ui_calls_.empty() && PhiModalDepth() == 0 &&
         !prompt_in_flight_) {
    PhiUiCall* call = deferred_ui_calls_.front();
    deferred_ui_calls_.pop_front();
    RunUiCall(call);
  }
  draining_ui_calls_ = false;
  if (deferred_ui_calls_.empty() && msg_hwnd_) {
    ::KillTimer(msg_hwnd_, kDeferredUiCallTimer);
  }
}

int PhiControlDispatcher::RegisterPendingReply(bool document,
                                               bool include_html) {
  base::AutoLock lock(pending_replies_lock_);
  const int id = next_pending_reply_++;
  PendingReply& slot = pending_replies_[id];
  slot.event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  slot.document = document;
  slot.include_html = include_html;
  return id;
}

void PhiControlDispatcher::CompletePendingReply(int id, std::string json) {
  // A creation reply carries a window the waiter will attach a CDP session
  // to next; its owner's sessions must already see it (UI thread, so the
  // registries are ours to read). Before the lock: the sync takes
  // principals_lock_, and nothing here needs both.
  SyncDevToolsScopes();
  base::AutoLock lock(pending_replies_lock_);
  auto it = pending_replies_.find(id);
  if (it == pending_replies_.end()) {
    return;  // The waiter timed out and released the slot.
  }
  it->second.json = std::move(json);
  ::SetEvent(it->second.event);
}

PhiVerbResult PhiControlDispatcher::WaitPendingReply(int id,
                                                     DWORD timeout_ms) {
  HANDLE event = nullptr;
  {
    base::AutoLock lock(pending_replies_lock_);
    auto it = pending_replies_.find(id);
    if (it == pending_replies_.end()) {
      return PhiVerbResult::Fail(PhiErrorCode::kInternal);
    }
    event = it->second.event;
  }
  const DWORD waited = ::WaitForSingleObject(event, timeout_ms);
  std::string json;
  {
    base::AutoLock lock(pending_replies_lock_);
    auto it = pending_replies_.find(id);
    if (it != pending_replies_.end()) {
      json = std::move(it->second.json);
      pending_replies_.erase(it);
    }
  }
  ::CloseHandle(event);
  if (waited != WAIT_OBJECT_0 || json.empty()) {
    return PhiVerbResult::Fail(PhiErrorCode::kTimeout);
  }
  // The coordinator answers a slot in legacy JSON (it also feeds the
  // client's own UI through ExecuteControlOnUi); FromLegacyJson turns it
  // back into the typed result the server formats.
  return PhiVerbResult::FromLegacyJson(json);
}

bool PhiControlDispatcher::PendingReplyShape(int id,
                                             bool* document,
                                             bool* include_html) {
  base::AutoLock lock(pending_replies_lock_);
  auto it = pending_replies_.find(id);
  if (it == pending_replies_.end()) {
    return false;
  }
  *document = it->second.document;
  *include_html = it->second.include_html;
  return true;
}

namespace {

// The refusal code for the capability bits a request lacks. The user-Space
// toggle is the thing the user can flip, so it is named first; the host
// bits are the box's and nothing the user can enable; the rest is the
// generic "this principal cannot".
PhiErrorCode RefusalCodeFor(uint32_t missing) {
  if (missing & (kUserSpaceRead | kUserSpaceWrite)) {
    return PhiErrorCode::kUserSpaceOperationsDisabled;
  }
  if (missing & (kHost | kDestructive)) {
    return PhiErrorCode::kTestRigOnly;
  }
  return PhiErrorCode::kCapabilityRequired;
}

// The agent drive context around one verb (task 6 of the phase 4 control
// plane; PhiFramework::SetAgentDriveContext). Armed once the capability
// gate has found a verb that mutates a USER window on an agent's behalf,
// released when the verb returns whichever way it leaves — the framework
// then reports the tabs the verb touched as agent-driven (the operating
// mask arms) and refuses the ones the user reclaimed, the guarantees a
// CDP drive already has. Never armed for the client's own UI.
class AgentDriveContext {
 public:
  explicit AgentDriveContext(phi_framework::PhiFramework* framework)
      : framework_(framework) {}
  ~AgentDriveContext() {
    if (armed_) {
      framework_->SetAgentDriveContext("");
    }
  }
  AgentDriveContext(const AgentDriveContext&) = delete;
  AgentDriveContext& operator=(const AgentDriveContext&) = delete;

  void Arm(const std::string& tag) {
    if (tag.empty() || !framework_) {
      return;
    }
    framework_->SetAgentDriveContext(tag.c_str());
    armed_ = true;
  }

 private:
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_;
  bool armed_ = false;
};

}  // namespace

// --- Principals -------------------------------------------------------------

uint32_t PhiControlDispatcher::EvaluatePipeCaps() const {
  const model::PhiSettings& settings = coordinator_->settings();
  // The box IS the host: every hook, every task. A --dev-hooks build only:
  // testRig lives in the user-writable PhiPreferences.json, so in a release
  // any process running as the user could grant itself kAll with it.
  if constexpr (kPhiDevHooks) {
    if (settings.test_rig) {
      return kAll;
    }
  }
  uint32_t caps = kAgentSpace | kCredentialFill | kCredentialReveal;
  if (settings.allow_agent_user_spaces) {
    caps |= kUserSpaceRead | kUserSpaceWrite;
  }
  if (settings.developer_mode) {
    caps |= kAppSettings;
  }
  return caps;
}

PhiPrincipal PhiControlDispatcher::PrincipalFor(ControlCaller caller,
                                                uint64_t connection_id) {
  if (caller == ControlCaller::kInternal) {
    PhiPrincipal internal;
    internal.capability_id = "internal";
    internal.caps = kAll;
    return internal;
  }
  base::AutoLock lock(principals_lock_);
  auto it = principals_.find(connection_id);
  if (it == principals_.end()) {
    return PhiPrincipal();  // Never consented: connection_id stays 0.
  }
  it->second.caps = EvaluatePipeCaps();  // The toggles, as of this request.
  return it->second;
}

std::set<uint64_t> PhiControlDispatcher::ConnectionsOfCapability(
    const std::string& capability_id) {
  std::set<uint64_t> ids;
  if (capability_id.empty()) {
    return ids;
  }
  base::AutoLock lock(principals_lock_);
  for (const auto& [id, principal] : principals_) {
    if (principal.capability_id == capability_id) {
      ids.insert(id);
    }
  }
  return ids;
}

PhiAgentCapability* PhiControlDispatcher::FindCapability(
    const std::string& id) {
  auto it = capabilities_.find(id);
  if (it == capabilities_.end()) {
    return nullptr;
  }
  if (::GetTickCount64() >= it->second.expires_tick) {
    capabilities_.erase(it);
    return nullptr;
  }
  return &it->second;
}

void PhiControlDispatcher::RegisterPrincipal(uint64_t connection_id,
                                             const PhiAgentPeer& peer,
                                             const std::string& capability_id) {
  PhiPrincipal principal;
  principal.connection_id = connection_id;
  principal.peer = peer;
  principal.capability_id = capability_id;
  principal.caps = EvaluatePipeCaps();
  // Scoped now, on the UI thread, so the CDP session this connection is
  // about to open (on its own thread, right after the 101) finds its
  // windows in the cache.
  bool unrestricted = false;
  principal.granted_windows = VisibleWindows(principal, &unrestricted);
  base::AutoLock lock(principals_lock_);
  principals_[connection_id] = std::move(principal);
}

bool PhiControlDispatcher::OwnsTask(const PhiPrincipal& principal,
                                    const std::string& task_id) {
  if (principal.caps & kHost) {
    return true;
  }
  auto it = task_owners_.find(task_id);
  if (it == task_owners_.end() || it->second == principal.capability_id) {
    return true;
  }
  // An orphan: its owner's capability expired (24 h) while the task lived
  // on — a persistent Space, typically. Whoever touches it next adopts it;
  // otherwise the agent that made it could never re-bind until Phi
  // restarts.
  if (it->second != "internal" && !FindCapability(it->second)) {
    task_owners_.erase(it);
    return true;
  }
  return false;
}

std::string PhiControlDispatcher::AgentNameFor(const PhiPrincipal& principal) {
  // The name an agent declared when it opened a Space (agentName on
  // create) is the one the Mac's badge shows; a principal that has none
  // live is named by its process image, which the brand resolver reduces
  // to a label or "other" — the raw path never leaves the machine.
  for (const std::string& task_id : principal.tasks) {
    if (const PhiAgentSpaces::Space* space =
            coordinator_->agent_spaces().Find(task_id);
        space && !space->agent_name.empty()) {
      return space->agent_name;
    }
  }
  const std::wstring& image = principal.peer.image_path;
  const size_t slash = image.find_last_of(L"\\/");
  std::wstring stem =
      slash == std::wstring::npos ? image : image.substr(slash + 1);
  if (const size_t dot = stem.rfind(L'.'); dot != std::wstring::npos) {
    stem.erase(dot);
  }
  return base::WideToUTF8(stem);
}

void PhiControlDispatcher::RecordTask(const PhiPrincipal& principal,
                                      const std::string& task_id) {
  task_owners_[task_id] = principal.capability_id;
  if (PhiAgentCapability* record = FindCapability(principal.capability_id)) {
    record->tasks.insert(task_id);
  }
  base::AutoLock lock(principals_lock_);
  auto it = principals_.find(principal.connection_id);
  if (it != principals_.end()) {
    it->second.tasks.insert(task_id);
  }
}

void PhiControlDispatcher::GrantSpace(const PhiPrincipal& principal,
                                      const std::string& space_id) {
  if (PhiAgentCapability* record = FindCapability(principal.capability_id)) {
    record->granted_spaces.insert(space_id);
  }
  base::AutoLock lock(principals_lock_);
  auto it = principals_.find(principal.connection_id);
  if (it != principals_.end()) {
    it->second.granted_spaces.insert(space_id);
  }
}

void PhiControlDispatcher::PruneTaskOwners() {
  for (auto it = task_owners_.begin(); it != task_owners_.end();) {
    const std::string& task_id = it->first;
    // A task whose window is still on its way (agentSpace.create answered
    // pendingReply) is live: its owner was recorded before the request.
    const bool live = coordinator_->agent_spaces().Find(task_id) ||
                      coordinator_->shadow_windows().count(task_id) ||
                      coordinator_->registry().HasPendingTask(task_id);
    if (live) {
      ++it;
      continue;
    }
    if (PhiAgentCapability* record = FindCapability(it->second)) {
      record->tasks.erase(task_id);
    }
    it = task_owners_.erase(it);
  }
}

PhiControlDispatcher::Scope PhiControlDispatcher::ResolveScope(
    const base::DictValue& payload,
    ScopeFallback fallback) {
  Scope scope;
  // {taskId}: an agent Space or a shadow window — agent territory whatever
  // the verb. A task nobody knows is still agent territory (the handler
  // answers unknown_task; the gate must not answer for the user's side).
  if (const std::string* task = payload.FindString("taskId")) {
    scope.task_id = *task;
    if (PhiAgentSpaces::Space* space =
            coordinator_->agent_spaces().Find(*task)) {
      scope.kind = Scope::Kind::kAgent;
      scope.controller =
          coordinator_->registry().Resolve(space->window_id.value);
      scope.browser = coordinator_->ResolveBrowser(space->window_id.value);
      if (!scope.controller) {
        scope.error = "unknown_task";
      }
      return scope;
    }
    auto shadow = coordinator_->shadow_windows().find(*task);
    if (shadow != coordinator_->shadow_windows().end() &&
        coordinator_->registry().IsBound(shadow->second.window_id)) {
      scope.kind = Scope::Kind::kShadow;
      scope.controller =
          coordinator_->registry().Resolve(shadow->second.window_id.value);
      scope.browser =
          coordinator_->ResolveBrowser(shadow->second.window_id.value);
      return scope;
    }
    scope.kind = Scope::Kind::kAgent;
    scope.error = "unknown_task";
    return scope;
  }
  const std::string* space_id = payload.FindString("spaceId");
  if (!space_id) {
    space_id = payload.FindString("space");  // The schema's declared alias.
  }
  const int64_t window_id =
      static_cast<int64_t>(payload.FindDouble("windowId").value_or(0));
  // {windowId}: a slot number (the browser it is showing) or a browser id
  // (an agent window, a Space browser the caller learned from spaces.list).
  // An agent shell is agent territory, with its task from the record; the
  // rest is the user's.
  if (window_id) {
    PhiShellWindow* controller =
        coordinator_->registry().Resolve(window_id);
    if (!controller) {
      scope.error = "window_not_open";
      return scope;
    }
    scope.controller = controller;
    scope.browser = coordinator_->registry().Has(SlotId{window_id})
                        ? controller->active_browser_id()
                        : BrowserId{window_id};
    if (controller->is_agent()) {
      if (ShadowWindow* shadow =
              coordinator_->FindShadowByWindow(scope.browser)) {
        scope.kind = Scope::Kind::kShadow;
        scope.task_id = shadow->task_id;
      } else {
        scope.kind = Scope::Kind::kAgent;
        if (PhiAgentSpaces::Space* space =
                coordinator_->agent_spaces().FindByWindow(scope.browser)) {
          scope.task_id = space->task_id;
        }
      }
      return scope;
    }
    scope.kind = Scope::Kind::kUser;
    scope.space_id = controller->SpaceForBrowser(scope.browser);
    // A pinned window that does not show the named Space is
    // window_not_open — never a cue to fall back elsewhere, the skill's
    // contract for a {window} binding.
    if (space_id && !space_id->empty() && scope.space_id != *space_id) {
      scope.error = "window_not_open";
    }
    return scope;
  }
  // {spaceId}: a user Space's open window (possibly behind a sibling).
  if (space_id && !space_id->empty()) {
    scope.kind = Scope::Kind::kUser;
    scope.space_id = *space_id;
    scope.browser = coordinator_->BrowserForSpace(*space_id);
    scope.controller =
        scope.browser.valid()
            ? coordinator_->registry().ControllerForBrowser(scope.browser)
            : nullptr;
    if (!scope.browser.valid()) {
      scope.error = "space_not_open";
    }
    return scope;
  }
  scope.kind = Scope::Kind::kUser;
  if (fallback == ScopeFallback::kFirstUserWindow) {
    if (PhiShellWindow* first =
            coordinator_->registry().FirstUserController()) {
      scope.controller = first;
      scope.browser = first->active_browser_id();
      scope.space_id = first->active_space();
    } else {
      scope.error = "no_window";
    }
    return scope;
  }
  scope.error = "invalid_payload";
  scope.unscoped = true;
  return scope;
}

// static
uint32_t PhiControlDispatcher::CapsToCall(const PhiVerbSpec& spec) {
  // A scoped verb aimed at the caller's OWN task needs nothing but
  // kAgentSpace; `required` is what it costs on a user window. The
  // capabilities report names the cheapest way in, which is what an agent
  // wants to know before it calls.
  return (spec.scopes & kScopeTask) ? kAgentSpace : spec.required;
}

bool PhiControlDispatcher::AuthorizeVerb(const PhiPrincipal& principal,
                                         const PhiVerbSpec& spec,
                                         const PhiParams& params,
                                         const Scope& scope,
                                         PhiErrorCode* code,
                                         std::string* drive_tag) {
  drive_tag->clear();
  uint32_t required = spec.required;
  const bool scoped = spec.scopes != kScopeNone;
  bool user_scope = false;
  std::string entered_space;
  if (scoped) {
    PruneTaskOwners();  // A completed task must not block its name's reuse.
    // The spec says which kinds of window this verb may act on; a resolved
    // kind outside that set is refused before the handler runs, whatever
    // the caller's capabilities (docs/architecture-review.md §3.7 item 3:
    // "a verb declared scope: agent given a user windowId fails
    // scope_not_allowed before its handler runs").
    const uint8_t resolved = scope.kind == Scope::Kind::kUser ? kScopeUser
                             : scope.kind == Scope::Kind::kShadow
                                 ? kScopeShadow
                                 : kScopeAgent;
    if (!(spec.scopes & resolved)) {
      *code = PhiErrorCode::kScopeNotAllowed;
      return false;
    }
    if (scope.kind == Scope::Kind::kUser) {
      // Off the record stays off the agents' record: an incognito window or
      // an Incognito Space is no agent's scope (the host's alone).
      if (!(principal.caps & kHost) &&
          ((scope.controller && scope.controller->is_incognito()) ||
           model::IsIncognitoSpaceId(scope.space_id))) {
        *code = PhiErrorCode::kScopeNotAllowed;
        return false;
      }
      // A scope that names nothing live is gated as the user's: the
      // handler's own answer (window_not_open, space_not_open, ...) is for
      // callers allowed to ask about the user's windows at all.
      user_scope = true;
      if (spec.flags & kVerbDestructiveOnUser) {
        required |= kDestructive;
      }
      if (scope.valid()) {
        entered_space = scope.space_id;
      }
    } else {
      required = kAgentSpace;
      if (!scope.task_id.empty() && !OwnsTask(principal, scope.task_id)) {
        *code = PhiErrorCode::kNotOwner;
        return false;
      }
    }
  }
  // The two payload-shaped cases, declared by the spec's flags rather than
  // by the gate knowing verb names.
  if ((spec.flags & kVerbHostForOutPath) && params.Has("outPath")) {
    required |= kHost;
  }
  if (spec.flags & kVerbDestructiveOnFolder) {
    const std::string* guid = params.FindString("guid");
    const std::optional<PhiRow> row =
        guid ? coordinator_->tab_store().Find(*guid) : std::nullopt;
    if (row && row->type == PhiRowType::kBookmarkFolder) {
      required |= kDestructive;
    }
  }
  if (const uint32_t missing = required & ~principal.caps) {
    *code = RefusalCodeFor(missing);
    return false;
  }
  // Mutating the user's state on an agent's behalf: the framework reports
  // it under the agent's capability. An app-wide mutation (a Space switch,
  // create) may surface a window too, so it arms as well. Never for the
  // host: the box is the user, not an agent, and a mask on the tabs a
  // suite touches would change what the suite is looking at.
  if ((spec.flags & kVerbMutates) && (user_scope || !scoped) &&
      !(principal.caps & kHost)) {
    *drive_tag = principal.capability_id;
  }
  if (!entered_space.empty()) {
    GrantSpace(principal, entered_space);
  }
  return true;
}

// --- DevTools session scope -------------------------------------------------

std::set<int64_t> PhiControlDispatcher::VisibleWindows(
    const PhiPrincipal& principal,
    bool* unrestricted) {
  std::set<int64_t> windows;
  *unrestricted = (principal.caps & kHost) != 0;
  if (*unrestricted) {
    return windows;
  }
  for (const auto& [task_id, owner] : task_owners_) {
    if (owner != principal.capability_id) {
      continue;
    }
    if (PhiAgentSpaces::Space* space =
            coordinator_->agent_spaces().Find(task_id)) {
      if (coordinator_->registry().IsBound(space->window_id)) {
        windows.insert(space->window_id.value);
      }
      continue;
    }
    auto shadow = coordinator_->shadow_windows().find(task_id);
    if (shadow != coordinator_->shadow_windows().end() &&
        coordinator_->registry().IsBound(shadow->second.window_id)) {
      windows.insert(shadow->second.window_id.value);
    }
  }
  if (principal.caps & kUserSpaceWrite) {
    std::set<std::string> spaces = principal.granted_spaces;
    if (const PhiAgentCapability* record =
            FindCapability(principal.capability_id)) {
      spaces.insert(record->granted_spaces.begin(),
                    record->granted_spaces.end());
    }
    for (const std::string& space_id : spaces) {
      // An Incognito Space is never an agent's to see, whatever it entered.
      if (model::IsIncognitoSpaceId(space_id)) {
        continue;
      }
      for (PhiShellWindow* controller :
           coordinator_->registry().Controllers()) {
        if (controller->is_agent() || controller->is_incognito()) {
          continue;
        }
        const BrowserId browser = controller->BrowserIdForSpace(space_id);
        if (browser.valid()) {
          windows.insert(browser.value);
        }
      }
    }
  }
  return windows;
}

void PhiControlDispatcher::ApplySessionScope(const PhiPrincipal& principal,
                                             uint64_t session_id) {
  if (!framework_) {
    return;
  }
  if (principal.caps & kHost) {
    framework_->SetDevToolsSessionScope(session_id, nullptr, -1);
    return;
  }
  const std::vector<int64_t> windows(principal.granted_windows.begin(),
                                     principal.granted_windows.end());
  framework_->SetDevToolsSessionScope(session_id, windows.data(),
                                      static_cast<int>(windows.size()));
}

void PhiControlDispatcher::SyncDevToolsScopes() {
  PruneTaskOwners();
  struct Push {
    uint64_t session;
    std::vector<int64_t> windows;
    bool unrestricted;
  };
  std::vector<Push> pushes;
  bool any_sessions = false;
  {
    base::AutoLock lock(principals_lock_);
    const uint32_t caps = EvaluatePipeCaps();
    for (auto& [connection_id, principal] : principals_) {
      const bool was_host = (principal.caps & kHost) != 0;
      principal.caps = caps;
      bool unrestricted = false;
      std::set<int64_t> windows = VisibleWindows(principal, &unrestricted);
      const bool changed =
          was_host != unrestricted || windows != principal.granted_windows;
      principal.granted_windows = std::move(windows);
      any_sessions = any_sessions || !principal.devtools_sessions.empty();
      if (!changed) {
        continue;
      }
      for (uint64_t session : principal.devtools_sessions) {
        pushes.push_back({session,
                          std::vector<int64_t>(principal.granted_windows.begin(),
                                               principal.granted_windows.end()),
                          unrestricted});
      }
    }
  }
  // Outside the lock: the framework call is any-thread and must never wait
  // on a connection thread that is waiting on us.
  if (framework_) {
    for (const Push& push : pushes) {
      framework_->SetDevToolsSessionScope(
          push.session, push.windows.data(),
          push.unrestricted ? -1 : static_cast<int>(push.windows.size()));
    }
  }
  // The toggles and the keep-alive sweep change the answer without a verb
  // passing through here, so poll while anyone is listening.
  if (msg_hwnd_) {
    if (any_sessions) {
      ::SetTimer(msg_hwnd_, kScopeSyncTimer, kScopeSyncMs, nullptr);
    } else {
      ::KillTimer(msg_hwnd_, kScopeSyncTimer);
    }
  }
}

void PhiControlDispatcher::OnControlProtocol(uint64_t connection_id,
                                             int protocol) {
  // Connection thread, before the peer's first request: record what wire it
  // negotiated so agentSpace.capabilities can confirm it.
  base::AutoLock lock(principals_lock_);
  auto it = principals_.find(connection_id);
  if (it != principals_.end()) {
    it->second.protocol = protocol;
  }
}

void PhiControlDispatcher::OnDevToolsSessionOpened(uint64_t connection_id,
                                                   uint64_t session_id) {
  // Connection thread. Scoped from the cached window set AT ONCE — the
  // framework's default is "nothing", and the peer's first message follows
  // the 101 immediately; the UI thread's sync (posted) catches anything
  // that changed since the cache was computed.
  PhiPrincipal snapshot;
  {
    base::AutoLock lock(principals_lock_);
    auto it = principals_.find(connection_id);
    if (it == principals_.end()) {
      return;  // Never consented: the session keeps the empty default.
    }
    it->second.devtools_sessions.push_back(session_id);
    snapshot = it->second;
  }
  ApplySessionScope(snapshot, session_id);
  if (msg_hwnd_) {
    ::PostMessageW(msg_hwnd_, WM_PHI_SCOPE_SYNC, 0, 0);
  }
}

void PhiControlDispatcher::OnDevToolsSessionClosed(uint64_t connection_id,
                                                   uint64_t session_id) {
  {
    base::AutoLock lock(principals_lock_);
    auto it = principals_.find(connection_id);
    if (it == principals_.end()) {
      return;
    }
    std::vector<uint64_t>& sessions = it->second.devtools_sessions;
    sessions.erase(std::remove(sessions.begin(), sessions.end(), session_id),
                   sessions.end());
  }
  if (msg_hwnd_) {
    ::PostMessageW(msg_hwnd_, WM_PHI_SCOPE_SYNC, 0, 0);
  }
}

std::vector<bool> PhiControlDispatcher::DevToolsTargetsInScope(
    uint64_t connection_id,
    const std::vector<PhiDevToolsTargetRef>& targets) {
  // Connection thread. The tab and window registries are the UI thread's,
  // so the answer is computed there (WM_PHI_TARGET_SCOPE); anything short
  // of an answer refuses every target.
  std::vector<bool> none(targets.size(), false);
  if (!msg_hwnd_ || targets.empty()) {
    return none;
  }
  PhiUiCall* call = NewUiCall(PhiUiCall::Kind::kTargetScope);
  if (!call) {
    return none;
  }
  call->connection_id = connection_id;
  call->targets = targets;
  if (!::PostMessageW(msg_hwnd_, WM_PHI_TARGET_SCOPE, 0,
                      reinterpret_cast<LPARAM>(call))) {
    call->Release();
    call->Release();
    return none;
  }
  if (!WaitUiCall(call)) {
    call->Release();
    return none;
  }
  std::vector<bool> result = std::move(call->in_scope);
  call->Release();
  return result.size() == targets.size() ? result : none;
}

bool PhiControlDispatcher::DevToolsMayCreateTargets(uint64_t connection_id) {
  // Connection thread: the caps cached at the last request / scope sync
  // (kScopeSyncTimer re-evaluates them every second while a session is
  // open), so no UI hop on the CDP path.
  base::AutoLock lock(principals_lock_);
  auto it = principals_.find(connection_id);
  return it != principals_.end() &&
         (it->second.caps & (kHost | kUserSpaceWrite)) != 0;
}

std::optional<std::set<uint64_t>> PhiControlDispatcher::EventAudience(
    const std::string& type,
    const base::DictValue& payload) {
  // UI thread (task_owners_ is its). An event about a task reaches the
  // connections of the capability that owns it — and the host's, which
  // sees everything — never the other agents connected to the same Phi.
  const std::string* task_id = payload.FindString("taskId");
  if (!task_id) {
    return std::nullopt;  // App-wide: every channel.
  }
  auto owner = task_owners_.find(*task_id);
  std::set<uint64_t> ids;
  base::AutoLock lock(principals_lock_);
  for (const auto& [id, principal] : principals_) {
    if ((principal.caps & kHost) ||
        (owner != task_owners_.end() &&
         principal.capability_id == owner->second)) {
      ids.insert(id);
    }
  }
  return ids;
}

std::vector<bool> PhiControlDispatcher::TargetsInScopeOnUi(
    uint64_t connection_id,
    const std::vector<PhiDevToolsTargetRef>& targets) {
  std::vector<bool> out(targets.size(), false);
  const PhiPrincipal principal =
      PrincipalFor(ControlCaller::kPipe, connection_id);
  if (principal.connection_id == 0) {
    return out;  // Never consented (or already gone).
  }
  bool unrestricted = false;
  const std::set<int64_t> windows = VisibleWindows(principal, &unrestricted);
  for (size_t i = 0; i < targets.size(); ++i) {
    if (unrestricted) {
      out[i] = true;
      continue;
    }
    // Only targets that live in a tab have a window to be scoped by;
    // service and shared workers, extension backgrounds and the rest stay
    // the host's.
    const std::string& type = targets[i].type;
    if (!type.empty() && type != "page" && type != "iframe" && type != "tab") {
      continue;
    }
    out[i] = TargetWindowIn(targets[i].id, windows);
  }
  return out;
}

bool PhiControlDispatcher::TargetInScope(const PhiPrincipal& principal,
                                         const std::string& target_id) {
  bool unrestricted = false;
  const std::set<int64_t> windows = VisibleWindows(principal, &unrestricted);
  return unrestricted || TargetWindowIn(target_id, windows);
}

bool PhiControlDispatcher::TargetWindowIn(const std::string& target_id,
                                          const std::set<int64_t>& windows) {
  if (target_id.empty() || windows.empty() || !framework_) {
    return false;
  }
  const TabId tab{framework_->TabIdForCdpTarget(target_id.c_str())};
  if (!tab.valid()) {
    return false;
  }
  if (std::optional<BrowserId> browser =
          coordinator_->window_model().BrowserOfTab(tab)) {
    return windows.count(browser->value) > 0;
  }
  // A tab the model has not heard of yet (the push trails the strip): pull
  // the candidate windows into local copies — never into the shared model,
  // since this may run inside a nested loop.
  for (int64_t window : windows) {
    phi_framework::WindowSnapshot snapshot;
    if (!PhiWindowModel::Pull(framework_, BrowserId{window}, &snapshot)) {
      continue;
    }
    for (const phi_framework::TabSnapshot& candidate : snapshot.tabs) {
      if (candidate.id == tab) {
        return true;
      }
    }
  }
  return false;
}

void PhiControlDispatcher::RevokeAgentAccess() {
  capabilities_.clear();
  session_capability_id_.clear();
  LOG(INFO) << "[PhiAgent] agent control off: capabilities revoked";
  // The connections unwind on their own threads and report themselves
  // closed (OnConnectionClosed), which drops their principals and re-syncs
  // the scopes.
  coordinator_->control_server().SeverAgentConnections();
}

void PhiControlDispatcher::OnConnectionClosed(uint64_t connection_id) {
  // The capability record outlives the connection on purpose: the agent's
  // next connection presents it and finds its tasks.
  {
    base::AutoLock lock(principals_lock_);
    principals_.erase(connection_id);
  }
  if (msg_hwnd_) {
    ::PostMessageW(msg_hwnd_, WM_PHI_SCOPE_SYNC, 0, 0);
  }
}
// --- The pipeline -------------------------------------------------------------

std::string PhiControlDispatcher::ExecuteControlOnUi(
    const std::string& type,
    const std::string& payload_json,
    ControlCaller caller,
    uint64_t connection_id) {
  // The client's own UI calls the verbs through here and reads the flat
  // legacy body; the pipe never does (the server formats a PhiVerbResult).
  std::optional<base::DictValue> parsed =
      base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
  base::DictValue payload;
  if (parsed) {
    payload = std::move(*parsed);
  }
  return Execute(type, payload, caller, connection_id).ToLegacyJson();
}

PhiVerbResult PhiControlDispatcher::Execute(const std::string& type,
                                            const base::DictValue& payload,
                                            ControlCaller caller,
                                            uint64_t connection_id) {
  // Runs on the UI thread — safe to touch the coordinator's registries and
  // call framework_. ONE pass: registry → scope → gate → schema → prompt
  // mutex → handler. Nothing below this line knows a verb's name.
  const PhiPrincipal principal = PrincipalFor(caller, connection_id);
  AgentDriveContext drive(framework_);
  // A pipe verb's DevTools scopes are re-synced on the way out, whichever
  // return it takes: a task closed, a Space entered, a toggle flipped.
  struct SyncOnExit {
    RAW_PTR_EXCLUSION PhiControlDispatcher* self;
    bool enabled;
    ~SyncOnExit() {
      if (enabled) {
        self->SyncDevToolsScopes();
      }
    }
  } sync_on_exit{this, caller == ControlCaller::kPipe};

  const PhiVerbSpec* spec = FindVerbSpec(Registry(), type, payload);
  if (!spec) {
    PhiVerbResult result = PhiVerbResult::Fail(PhiErrorCode::kUnknownVerb);
    result.message = "no verb named `" + type + "`";
    return result;
  }
  // The window a scoped verb is aimed at, resolved ONCE: the gate decides
  // agent-vs-user on it and the handler reads its browser and shell from it
  // instead of resolving windowId again. Left empty for an app-wide verb.
  Scope scope;
  if (spec->scopes != kScopeNone) {
    scope = ResolveScope(payload, (spec->flags & kVerbDefaultsToUser)
                                      ? ScopeFallback::kFirstUserWindow
                                      : ScopeFallback::kNone);
  }
  const PhiParams params(&payload, spec->params);
  if (caller == ControlCaller::kPipe) {
    // The capability gate, BEFORE the handler (lane 5 §4.2): what the
    // principal holds against what the verb needs on the scope it resolved
    // to, and ownership of the task it names. The internal callers (the
    // client's own UI) are never gated — they ARE the user.
    if (principal.connection_id == 0) {
      return PhiVerbResult::Fail(PhiErrorCode::kCapabilityRequired);
    }
    PhiErrorCode code = PhiErrorCode::kCapabilityRequired;
    std::string drive_tag;
    if (!AuthorizeVerb(principal, *spec, params, scope, &code, &drive_tag)) {
      return PhiVerbResult::Fail(code);
    }
    // A `targetId` names a CDP target, not a window, so ResolveScope never
    // saw it: the target's tab must sit in a window this principal may see
    // (the same rule as /json/list and the CDP session open), or an agent
    // could read or drive any tab — the user's, an Incognito Space's — by
    // id.
    if (const std::string* target = payload.FindString("targetId");
        target && !target->empty() && !TargetInScope(principal, *target)) {
      return PhiVerbResult::Fail(PhiErrorCode::kScopeNotAllowed);
    }
    drive.Arm(drive_tag);
    // The Mac's agent_user_space_command: an agent's command that passed
    // the gate INTO the user's own Spaces — one aimed at a user window, or
    // an app-wide one that needs the user-Space capability (spaces.create,
    // a switch). Agent-Space work is the agent's own and is not counted;
    // neither is the box (kHost is the user, not an agent), nor a read or
    // a liveness probe (PhiAgentUserSpaceCommandIsTracked).
    const bool user_space_verb =
        (spec->scopes != kScopeNone && scope.kind == Scope::Kind::kUser) ||
        (spec->required & (kUserSpaceRead | kUserSpaceWrite));
    if (user_space_verb && !(principal.caps & kHost) &&
        PhiAgentUserSpaceCommandIsTracked(spec->name)) {
      coordinator_->CaptureAnalytics(
          kAnalyticsModuleAgent, kEventAgentUserSpaceCommand,
          PhiAgentUserSpaceCommandProperties(std::string(spec->name),
                                             AgentNameFor(principal)));
    }
  }
  // The schema, after the gate: a caller that may not ask at all learns
  // nothing about the shape of what it asked for.
  std::string field;
  std::string message;
  if (!params.Validate(&field, &message)) {
    PhiVerbResult result = PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload);
    result.message = std::move(message);  // Names `field`; v2 only.
    return result;
  }
  const PhiVerbContext ctx{principal, scope,  params,
                           *spec,     caller, ::GetTickCount64()};
  PhiVerbResult result;
  if (!spec->prompts) {
    result = spec->handler(this, ctx);
  } else {
    // The prompt mutex: while this verb's dialog is up every other call
    // parks in the deferred queue (RunOrDeferUiCall), so two prompts never
    // stack.
    prompt_in_flight_ = true;
    result = spec->handler(this, ctx);
    prompt_in_flight_ = false;
  }
  // A refusal its handler left unnamed (PhiVerbOk(false), Refuse) is named
  // here: by the scope when the payload's window, Space or task names
  // nothing — that is why the handler found nothing to act on — else by its
  // code. No refusal reaches the wire as a bare {ok:false}.
  if (!result.ok && !result.deferred() && result.reason.empty()) {
    if (!scope.valid() && !scope.unscoped) {
      result.code = PhiErrorCodeForReason(scope.error);
      result.reason = scope.error;
    } else {
      result.reason = std::string(PhiErrorCodeName(result.code));
    }
  }
  return result;
}

// --- Shared handler helpers ---------------------------------------------------

bool PhiControlDispatcher::ListsTask(const PhiPrincipal& principal,
                                     const std::string& task_id) const {
  auto owner = task_owners_.find(task_id);
  return owner != task_owners_.end() &&
         owner->second == principal.capability_id;
}

PhiControlDispatcher::Space* PhiControlDispatcher::FindSpace(
    const std::string& id) {
  for (Space& space : coordinator_->spaces_model().spaces) {
    if (space.id == id) {
      return &space;
    }
  }
  return nullptr;
}

bool PhiControlDispatcher::SpaceExists(const std::string& id) {
  // The two action targets (Incognito, Kiosk) are legal rule targets even
  // though neither is a listed Space.
  return UrlRuleTargetIsAction(id) || FindSpace(id) != nullptr;
}

BrowserId PhiControlDispatcher::UserWindowOf(const Scope& scope,
                                             std::string* space_out,
                                             std::string* error) {
  if (!scope.valid()) {
    *error = scope.error;
    return BrowserId();
  }
  // An agent or shadow window is never a user Space's window, and a pinned
  // window that does not show the Space already failed above.
  if (scope.kind != Scope::Kind::kUser || scope.space_id.empty()) {
    *error = "window_not_open";
    return BrowserId();
  }
  *space_out = scope.space_id;
  return scope.browser;
}

base::DictValue PhiControlDispatcher::ActiveTabRow(BrowserId browser) {
  base::DictValue row;
  if (const phi_framework::TabSnapshot* tab =
          coordinator_->window_model().ActiveTab(browser)) {
    row.Set("tabId", tab->id.value);
    row.Set("url", tab->url);
    row.Set("title", tab->title);
  }
  return row;
}

bool PhiControlDispatcher::DialectOwner(const std::string& profile_id_or_empty,
                                        std::string* profile_id,
                                        std::string* space_id) {
  if (!profile_id_or_empty.empty()) {
    *profile_id = profile_id_or_empty;
    // Prefer a window whose active Space runs this profile (its space is the
    // natural owner under space scope); fall back to the profile's first
    // Space, then to no Space (profile/app-scoped pins still resolve).
    for (PhiShellWindow* controller :
         coordinator_->registry().Controllers()) {
      if (controller->is_agent()) {
        continue;
      }
      std::string p, s;
      if (coordinator_->OwnerForSlot(controller->slot(), &p, &s) &&
          p == *profile_id) {
        *space_id = s;
        return true;
      }
    }
    for (const Space& space : coordinator_->spaces_model().spaces) {
      if (space.profile_id == *profile_id) {
        *space_id = space.id;
        return true;
      }
    }
    space_id->clear();
    return true;
  }
  PhiShellWindow* focused = coordinator_->FocusedUserController();
  return focused &&
         coordinator_->OwnerForSlot(focused->slot(), profile_id, space_id);
}
}  // namespace phi_client

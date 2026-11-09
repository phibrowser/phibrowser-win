// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_CONTROL_SERVER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_CONTROL_SERVER_H_

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/synchronization/lock.h"
#include "base/values.h"
#include "client/phi_verb_registry.h"

namespace phi_framework {
class PhiFramework;
}

namespace phi_client {

// Who is on the other end of a pipe connection, resolved at accept. FOR
// DISPLAY AND LOGGING ONLY: Windows has no code-signature identity for a
// pipe client (the Mac walks the process tree to a signed agent), so
// nothing is decided on these fields — admission is the "Allow agents to
// control Phi" setting plus the launch token.
struct PhiAgentPeer {
  DWORD pid = 0;
  // Full image path of the connecting process; empty when the process died
  // or could not be opened.
  std::wstring image_path;
  // The `X-Phi-Agent-Capability` the request carried (lane 5 §4.1): an
  // opaque id Phi minted for an earlier consented connection, which a
  // later connection — or a helper the agent delegated to — presents to
  // join that principal without a second prompt. Already validated to the
  // wire alphabet ([A-Za-z0-9_-]{32,128}); empty when absent. Whether it
  // still names a live capability is the delegate's call.
  std::string presented_capability;
};

// One DevTools target as /json/list names it, for the delegate's scope
// check. `type` is the descriptor's type ("page", "iframe", "service_worker",
// ...); empty when the caller does not know it (a session open).
struct PhiDevToolsTargetRef {
  std::string id;
  std::string type;
};

// Handles /phi-agent control commands and the per-connection trust
// bookkeeping. Every method is called on a control server connection
// thread; the implementation marshals to the browser UI thread itself.
//
// `connection_id` is the server's handle for ONE accepted pipe connection
// (never reused within a process). The delegate keys its principal on it
// (docs/architecture-review.md §3.7 item 3): AuthorizeAgentPeer creates the
// principal, HandleControlCommand and the DevTools-session notices act on
// it, OnConnectionClosed retires it.
class PhiControlDelegate {
 public:
  virtual ~PhiControlDelegate() = default;
  // `type` is the envelope verb (e.g. "agentSpace.list"); `payload` is the
  // request's payload, already parsed (v1 parses `payloadJson` for the
  // delegate; v2 carries the object itself). The answer is the TYPED result
  // — success fields, or a code from the closed enum — which the server
  // formats onto whichever protocol the connection speaks. The delegate
  // never sees an envelope and never spells a wire shape.
  virtual PhiVerbResult HandleControlCommand(uint64_t connection_id,
                                             const std::string& type,
                                             base::DictValue payload) = 0;
  // Called ONCE per accepted connection, BEFORE any request is served —
  // control channel and CDP session alike (CDP grants browser control, so
  // both families sit behind the same gate). Only connections that
  // already presented the launch token reach this; the implementation
  // marshals to the UI thread and may block on the turn-on prompt when
  // agent control is off. False refuses the connection. On success `capability_id` receives the agent
  // capability the connection now holds (minted here, or the one the peer
  // presented), which the server echoes on the 101 as
  // `X-Phi-Agent-Capability`; empty when the delegate issued none.
  virtual bool AuthorizeAgentPeer(uint64_t connection_id,
                                  const PhiAgentPeer& peer,
                                  std::string* capability_id) = 0;
  // The control wire the connection negotiated on its /phi-agent upgrade
  // (1, or 2 when it sent `X-Phi-Protocol: 2`). Called once, right after
  // the 101 and before the first request. A connection that never opens a
  // control channel (a CDP session) is never told.
  virtual void OnControlProtocol(uint64_t connection_id, int protocol) = 0;
  // A CDP session (PhiFramework::OpenDevToolsSession) was opened / closed
  // on the connection. Reported right after the open succeeded and before
  // the 101 goes out, so the delegate can scope the session
  // (SetDevToolsSessionScope) before the peer's first message.
  virtual void OnDevToolsSessionOpened(uint64_t connection_id,
                                       uint64_t session_id) = 0;
  virtual void OnDevToolsSessionClosed(uint64_t connection_id,
                                       uint64_t session_id) = 0;
  // The client's half of the DevTools scope (phi_devtools_transport.h: the
  // framework filters neither discovery nor the target a session is opened
  // on). One answer per entry of `targets`: true when the connection's
  // principal may see that target — its tab's window is in the set its
  // sessions are scoped to, or the principal is unrestricted. Fails closed
  // (all false) for an unknown connection or when the UI thread does not
  // answer. The implementation marshals to the UI thread.
  virtual std::vector<bool> DevToolsTargetsInScope(
      uint64_t connection_id,
      const std::vector<PhiDevToolsTargetRef>& targets) = 0;
  // Whether the connection's CDP sessions may create targets
  // (Target.createTarget opens a tab in the user's window): only a principal
  // that may write the user's Spaces, or an unrestricted one. Connection
  // thread; answers from the principal's cached capabilities, no UI hop.
  virtual bool DevToolsMayCreateTargets(uint64_t connection_id) = 0;
  // Which connections a pushed event may reach (BroadcastEvent): nullopt
  // for every channel, else exactly the returned set — an event about a
  // task (its payload names `taskId`) goes to that task's owner only. UI
  // thread.
  virtual std::optional<std::set<uint64_t>> EventAudience(
      const std::string& type,
      const base::DictValue& payload) = 0;
  // The connection is gone (after its sessions closed). Called only for
  // connections AuthorizeAgentPeer accepted.
  virtual void OnConnectionClosed(uint64_t connection_id) = 0;
};

// The client's unified control socket — the Windows twin of the Mac
// AgentCDPListener. It owns ONE named pipe and speaks HTTP/1.1 over it,
// multiplexing two families by request path (like the Mac UDS):
//
//   /phi-agent...          -> WebSocket, client control served HERE
//                             (dispatched to the delegate).
//   /json, /json/version,  -> the DevTools discovery documents, answered
//   /json/list                from the framework (plain HTTP GET); the
//                             target lists keep only what the principal
//                             may see (DevToolsTargetsInScope).
//   /devtools/<kind>/<id>  -> WebSocket, a CDP session pumped through the
//                             framework's IN-PROCESS DevTools transport
//                             (PhiFramework::OpenDevToolsSession). There is
//                             no loopback TCP DevTools server; this pipe is
//                             the only door to the protocol.
//
// Trust model (docs/architecture-review.md §3.7, lane5 §4.1) — three gates,
// in order, every connection:
//   1. The pipe's SDDL: owner + SYSTEM + Administrators, medium integrity,
//      remote clients rejected. Other users and sandboxed (low-IL) processes
//      cannot open it at all. The name carries a per-launch random suffix and
//      the first instance is created FILE_FLAG_FIRST_PIPE_INSTANCE, so a
//      squatter cannot pre-create it.
//   2. A per-launch token: 32 random bytes, hex, on line 2 of the pointer
//      file `<user-data-dir>\CDPAgentSocket` (line 1 is the pipe name — the
//      Mac discovery contract). The pointer file has an owner + SYSTEM DACL
//      and is written only after the first pipe instance exists. Every
//      request must carry `X-Phi-Token: <hex>`; a missing or wrong token is
//      answered 403 before anything else happens — no delegate call, no UI —
//      so a process that cannot read the profile directory cannot even make
//      the turn-on prompt appear.
//   3. The switch: PhiControlDelegate::AuthorizeAgentPeer admits the
//      connection when Developer ▸ "Allow agents to control Phi" is on,
//      asks the user once to turn it on when it is off,
//      and registers a per-connection principal under the session's agent
//      capability (or the one the peer presented); that capability is
//      echoed on the 101. The peer's pid and image path are shown and
//      logged, never judged — Windows has no signature identity to key a
//      per-program grant on.
// Plus a cap of kMaxConnections concurrent connections (429 beyond it, no
// delegate call) so a prompt cannot be spammed.
//
// Runs on its own Win32 threads with blocking overlapped I/O; control
// commands marshal to the browser UI thread. DevTools callbacks arrive on
// the UI thread and are queued for the connection thread to write, so a
// wedged peer never stalls the UI.
class PhiControlServer {
 public:
  PhiControlServer();
  ~PhiControlServer();

  PhiControlServer(const PhiControlServer&) = delete;
  PhiControlServer& operator=(const PhiControlServer&) = delete;

  // Starts the listener thread. `user_data_dir` is where the CDPAgentSocket
  // pointer file is written. `delegate` (owned by the caller, must outlive
  // this) handles /phi-agent control commands and consent. `framework`
  // (owned by the framework, outlives this) carries CDP: every
  // non-/phi-agent request is served through its in-process DevTools
  // transport (PhiFramework::OpenDevToolsSession and friends). Idempotent.
  void Start(const std::wstring& user_data_dir,
             PhiControlDelegate* delegate,
             phi_framework::PhiFramework* framework);

  // Pushes an unsolicited event frame — {"event":<type>,"payloadJson":<json>}
  // — to the live control channels the delegate's EventAudience allows
  // (every one for an app-wide event; a task's owner only for an event about
  // a task) — the Mac's broadcastToTaskDriver. UI thread (the audience reads
  // the task owners). Never blocks: the frame is queued on each channel and
  // that channel's own thread writes it, so a wedged peer stalls only itself.
  void BroadcastEvent(const std::string& type,
                      const std::string& payload_json);
  // The same push, to the control channels of `connection_ids` only — an
  // event meant for one agent session (notification.response), never the
  // other agents connected to the same Phi.
  void SendEventToConnections(const std::string& type,
                              const std::string& payload_json,
                              const std::set<uint64_t>& connection_ids);

  // Cuts every admitted connection — control channels and CDP sessions
  // alike — the way the Mac's AgentCDPListener.revokeActiveAccess does when
  // "Allow agents to control Phi" goes off: the pending I/O on each pipe is
  // cancelled and the pipe disconnected, so its serving thread unwinds
  // through its normal teardown (the DevTools session closed, the delegate
  // told). The listener stays up; the next connection meets the switch.
  // Safe to call from any thread.
  void SeverAgentConnections();

 private:
  // One accepted pipe plus its read-ahead buffer; defined in the .cc.
  struct Connection;

  // One live /phi-agent channel. Only its serve loop ever writes the pipe:
  // a pushed event (any thread) is queued in `outbox` and `wake` tells the
  // loop to write it. Shared (shared_ptr) between the loop and any pusher
  // that snapshotted it, so it owns `wake` and closes it in its destructor —
  // a pusher can never signal a closed handle.
  struct ControlChannel {
    ControlChannel();
    ~ControlChannel();
    ControlChannel(const ControlChannel&) = delete;
    ControlChannel& operator=(const ControlChannel&) = delete;
    // Auto-reset: one wake drains the whole outbox.
    HANDLE wake = nullptr;
    // The wire the peer asked for on its upgrade (1 unless it sent
    // `X-Phi-Protocol: 2`); a pushed event is shaped for it.
    int protocol = 1;
    // The connection this channel serves (see PhiControlDelegate).
    uint64_t connection_id = 0;
    // Event frames waiting for the serve loop, and their total size;
    // `overflow` (sticky) when a peer that stopped reading let them pile
    // past the cap — the loop then drops the connection.
    base::Lock outbox_lock;
    std::deque<std::string> outbox;
    size_t outbox_bytes = 0;
    bool overflow = false;
  };
  // BroadcastEvent's body; a null `connection_ids` reaches every channel.
  void PushEvent(const std::string& type,
                 const std::string& payload_json,
                 const std::set<uint64_t>* connection_ids);
  void RegisterChannel(const std::shared_ptr<ControlChannel>& channel);
  void UnregisterChannel(const std::shared_ptr<ControlChannel>& channel);

  void ListenLoop();
  // Writes the two-line pointer file with its restricted DACL. Called from
  // ListenLoop once the first pipe instance exists.
  bool WritePointerFile();
  void HandleConnection(HANDLE pipe);
  // Serves the /phi-agent WebSocket control channel on an accepted
  // connection whose HTTP head has been read and validated. Dispatches each
  // message to delegate_ under `connection_id`. `capability_id` (may be
  // empty) is echoed on the 101 as X-Phi-Agent-Capability.
  void ServeControlChannel(Connection* conn,
                           const std::string& head,
                           uint64_t connection_id,
                           const std::string& capability_id);
  // True when the peer's CDP `message` must not reach the browser for this
  // connection (Target.createTarget without user-Space write), with the
  // protocol error to answer in `reply`.
  bool RefusedCdpCommand(const std::string& message,
                         uint64_t connection_id,
                         std::string* reply);
  // Serves one CDP WebSocket session (/devtools/<kind>/<id>) through the
  // framework's in-process transport until either side closes, reporting
  // the session to delegate_ so it can be scoped to the principal. An
  // empty `target_id` is the browser target; any other must be in the
  // principal's scope (DevToolsTargetsInScope) or the open is refused.
  void ServeDevToolsSession(Connection* conn,
                            const std::string& head,
                            const std::string& target_id,
                            uint64_t connection_id,
                            const std::string& capability_id);

  std::wstring user_data_dir_;
  std::wstring pipe_name_;
  // Lower-case hex of the per-launch token; empty until Start.
  std::string token_hex_;
  // Non-owning; owned by the caller (the coordinator), outlives this.
  RAW_PTR_EXCLUSION PhiControlDelegate* delegate_ = nullptr;
  // Non-owning; owned by the framework, outlives this.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  HANDLE listen_thread_ = nullptr;
  volatile bool started_ = false;
  // Connections accepted and not yet finished, for the concurrency cap.
  std::atomic<int> active_connections_{0};
  // The next connection id (see PhiControlDelegate); 0 is never handed out
  // — the dispatcher uses it for its internal principal.
  std::atomic<uint64_t> next_connection_id_{1};
  // Live control channels, for BroadcastEvent. The shared_ptr keeps a
  // channel (and its wake event) alive while a pusher still holds it.
  std::vector<std::shared_ptr<ControlChannel>> channels_;
  mutable base::Lock channels_lock_;
  // Every admitted connection's pipe while it is served, for
  // SeverAgentConnections. A handle is only ever closed by its own serving
  // thread, after it left this list.
  std::vector<HANDLE> live_pipes_;
  mutable base::Lock live_pipes_lock_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CONTROL_SERVER_H_

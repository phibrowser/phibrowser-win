// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_AGENT_DRIVE_REGISTRY_H_
#define PHIBROWSER_WIN_CLIENT_PHI_AGENT_DRIVE_REGISTRY_H_

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace phi_client {

// The Mac's AgentUserSpaceDriveRegistry: which tabs in the USER's own Spaces
// an agent currently holds, so the operating mask can be shown on exactly
// those and the user can take one back.
//
// An agent driving a user-Space tab is invisible to this client — page
// automation rides the CDP relay, whose frames we pass straight through — so
// every input here comes from the browser (phi::PhiUserSpaceDrive, reported
// through the framework delegate). The three inputs are deliberately NOT
// symmetric:
//
//   DRIVE    arms a mask, or re-arms it. A session that names a DIFFERENT tab
//            than it held has MOVED, so its previous tab unmasks — one driver
//            holds one tab.
//   OBSERVE  never arms anything. It only keeps an EXISTING hold alive across
//            the gaps between an agent's rounds, which are far longer than
//            the gaps between its individual commands. Opening a tab and
//            reading it is not claiming it.
//   STOP     the driving session went away. A real close event, which is why
//            it exists at all — otherwise "finished" could only be a timeout.
class PhiAgentDriveRegistry {
 public:
  // Notified whenever a tab's masked state changes (armed, moved, cleared,
  // reclaimed), so the shell can repaint and mount or drop the pill.
  using ChangedCallback = std::function<void(int tab_id)>;
  // Called whenever the reclaimed SET changes; the client is the source of
  // truth and re-pushes the whole set to the browser, which then refuses
  // drive commands aimed at those tabs.
  using ReclaimedCallback = std::function<void(const std::vector<int>& tabs)>;

  PhiAgentDriveRegistry();
  ~PhiAgentDriveRegistry();

  PhiAgentDriveRegistry(const PhiAgentDriveRegistry&) = delete;
  PhiAgentDriveRegistry& operator=(const PhiAgentDriveRegistry&) = delete;

  void set_changed_callback(ChangedCallback cb) { changed_ = std::move(cb); }
  void set_reclaimed_callback(ReclaimedCallback cb) {
    reclaimed_changed_ = std::move(cb);
  }

  // Browser reports, straight off the framework delegate.
  void OnDrove(int tab_id, int64_t window_id, int64_t session_id);
  void OnObserved(int tab_id, int64_t window_id, int64_t session_id);
  void OnStopped(int tab_id, int64_t session_id);

  // The user pressed "Take control" on `tab_id`. The tab keeps its pill (so
  // control can be handed back) but the browser now refuses that tab's drive
  // commands outright.
  void Reclaim(int tab_id);
  // The user handed control back.
  void Release(int tab_id);

  // True while an agent holds `tab_id` — i.e. the mask belongs on it.
  bool IsMasked(int tab_id) const;
  // True once the user has taken `tab_id` back; the pill stays, the mask
  // reads as "you have control".
  bool IsReclaimed(int tab_id) const;
  // Every tab currently masked, for the shell's paint pass.
  std::vector<int> MaskedTabs() const;

  // Drops holds whose driver has gone quiet and takeovers that have aged
  // out. Driven by the shell's timer; separated so tests can step it.
  void Sweep();

  // The Mac's timings, in milliseconds. A driver that stops sending but stays
  // attached leaves no close event, so idle expiry is the backstop: an
  // agent's turnaround between rounds is tens of seconds, and a mask that
  // blinks off mid-task is worse than one lingering a moment past it.
  static constexpr ULONGLONG kIdleTimeoutMs = 30 * 1000;
  // A takeover outlives any one driving session on purpose: if it died with
  // the session, the agent's next run would silently re-mask the tab the user
  // just took back.
  static constexpr ULONGLONG kReclaimGraceMs = 30 * 60 * 1000;
  static constexpr int kSweepIntervalMs = 2000;

 private:
  struct Hold {
    int tab_id = 0;
    int64_t window_id = 0;
    ULONGLONG last_seen = 0;
  };

  void ClearHoldLocked(int64_t session_id);
  void PushReclaimed();

  // One record per driving session — a session IS the (driver, tab) pair the
  // mask is keyed on, so no separate driver registry is needed.
  std::map<int64_t, Hold> holds_;
  // Tab -> when the user took it back.
  std::map<int, ULONGLONG> reclaimed_;
  ChangedCallback changed_;
  ReclaimedCallback reclaimed_changed_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_AGENT_DRIVE_REGISTRY_H_

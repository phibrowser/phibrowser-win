// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SESSION_RESTORE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SESSION_RESTORE_H_

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_app_model.h"
#include "phibrowser-win/client/phi_ghost_spaces.h"
#include "phibrowser-win/client/phi_ids.h"

namespace phi_framework {
class PhiFramework;
}  // namespace phi_framework

namespace phi_client {

template <typename T>
class PhiStore;

// Lazy Space restore (docs/architecture-review.md §3.4, lane 1 §1.2 cluster
// 11): the ghost Spaces (windows the last session parked), the cold-start
// plan, the reopen receipt and the last-active Space — one model, one file
// (PhiRestore.json).
//
// Invariants (§3.4): a Space↔window record is written only from an explicit
// attach or a restore match; deleting a Space erases its record and drops
// the parked window on the session side too; parked windows no Space claims
// are dropped once the restore settles, or the next unfiltered replay would
// bring them back as loose windows — but a parked window a Space's
// SECONDARY row claims (its second window, release review B6) is re-hosted
// as a shell of its own, never dropped.
//
// The app phases are NOT this service's: it reports the milestones it
// reaches through Host and the coordinator owns the ladder.
class PhiSessionRestore {
 public:
  class Host {
   public:
    virtual ~Host() = default;
    // The Space the user is looking at RIGHT NOW, from the live user
    // windows, or empty when none is up (a cold start).
    virtual std::string ActiveSpaceOfLiveWindow() const = 0;
    // The Space holding the default role — what a fresh shell opens with.
    virtual std::string DefaultSpaceId() const = 0;
    // Whether the Space list still holds `space_id` (an id remembered from
    // the last run may have been deleted since).
    virtual bool HasSpace(const std::string& space_id) const = 0;
    // The Space list gained or lost a window id: re-push the wire's menu.
    virtual void PushSpaceMenu() = 0;
    // The cold start armed an eager set: if no user window is up once the
    // replay settles, open a default shell. Re-armed shorter once the park
    // list lands; disarmed by the first user window.
    virtual void ArmColdStartFallback(unsigned delay_ms) = 0;
    virtual void DisarmColdStartFallback() = 0;
    // The milestones. `NoteRestorePlanned` is reached from the plan
    // callback (armed or not), `NoteRestoring` from the first park receipt,
    // and `OnRestoreSettled` carries the rescue and the phase move.
    virtual void NoteRestorePlanned(const char* reason) = 0;
    virtual void NoteRestoring() = 0;
    // True once the restore's verdict is in (the coordinator's kSettled).
    virtual bool RestoreSettled() const = 0;
    virtual void OnRestoreSettled(const char* reason) = 0;
  };

  // How long the cold-start rescue waits for the replay to have its say,
  // and the shorter re-arm once the park list has landed.
  static constexpr unsigned kColdStartFallbackMs = 4000;
  static constexpr unsigned kColdStartSettleMs = 2000;

  using DirtyCallback = std::function<void()>;

  PhiSessionRestore(phi_framework::PhiFramework& framework,
                    Host& host,
                    DirtyCallback dirty);
  ~PhiSessionRestore();

  PhiSessionRestore(const PhiSessionRestore&) = delete;
  PhiSessionRestore& operator=(const PhiSessionRestore&) = delete;

  // Binds and loads PhiRestore.json (a missing file imports what it
  // replaces; a corrupt one is quarantined and defaults stand).
  void Load(const std::wstring& user_data_dir);
  void Save();
  // The shutdown pass: the last-active Space is re-derived from the live
  // window before the file is written.
  void PrepareForShutdown();

  // --- The last-active Space ----------------------------------------------
  // A live slot's current Space when there is one (a reopen mid-run); at
  // cold start what PhiRestore.json remembers from the last run, falling
  // back to the default Space when that id was deleted since.
  std::string LastActiveSpaceId() const;
  // Notes the Space the user is in — on every switch; written by the
  // coalesced save. Empty and unchanged ids are no-ops.
  void NoteActiveSpace(const std::string& space_id);

  // --- The framework's restore callbacks ----------------------------------
  void OnColdStartRestorePlan(phi_framework::ColdStartPlan* plan, bool* armed);
  void OnColdStartParkedWindows(const char* parked_json,
                                const char* reporting_profile);
  void OnSessionRestoreSettled(const char* receipt_json);
  // The kSettled work, reached from BOTH restore paths — the reopen
  // receipt, the cold start's fallback timer, or, when neither will fire,
  // the first user shell's first OnWindowChanged: drops the parked windows
  // no Space claims, then hands the rescue and the phase move to the Host.
  void Settle(const char* reason);
  // Replays the last session with no user window up, as a cold start does:
  // the last-active Space's windows come back and the rest park. It ends in
  // OnSessionRestoreSettled.
  void BeginWindowlessRestore();
  // The cold-start fallback fired: nothing disarmed it, so settle.
  void OnColdStartFallbackTimer();
  // A user window exists: the fallback has nothing to rescue.
  void NoteUserWindowUp();

  // --- The ghost records ---------------------------------------------------
  // The Space a materialized window belonged to, or null. `secondary`
  // (optional) says the window was one of the Space's secondary windows.
  const std::string* SpaceForWindowId(BrowserId window_id,
                                      bool* secondary = nullptr) const;
  // The Space's primary remembered window, if any.
  std::optional<PhiGhostSpaces::Remembered> PrimaryWindow(
      const std::string& space_id) const;
  // True while the Space's primary row still names a window of the LAST
  // run that may yet come back this run: in the cold start's eager set and
  // not announced, or of a profile whose cold start has not reported what
  // it parked (the report lands after that profile's eager windows are
  // built). Such a row must not be overwritten by a sibling window.
  bool PrimaryAwaited(const std::string& space_id) const;
  // Whether `window_id` is one of the Space's secondary windows.
  bool HasSecondaryWindow(const std::string& space_id,
                          BrowserId window_id) const;
  // A restored window (previous-session id) was announced.
  void NoteRestoredWindowArrived(BrowserId restored_from);
  // The parked record for `space_id`, or null when it has none.
  const PhiGhostSpaces::Remembered* Parked(const std::string& space_id) const;
  // The Space's window is live now (an attach or a restore match).
  void NoteLiveWindow(const std::string& space_id,
                      const std::string& profile,
                      BrowserId window_id);
  // A second live window of a Space whose primary still stands (see
  // KeepsPrimary): remembered beside it so it comes back as a shell of its
  // own (PhiGhostSpaces::NoteSecondaryWindow).
  void NoteSecondaryWindow(const std::string& space_id,
                           const std::string& profile,
                           BrowserId window_id);
  // The secondary window closed for good, or is re-keyed under this run's
  // id.
  void ForgetSecondaryWindow(const std::string& space_id, BrowserId window_id);
  // The parked window was materialized, refused for good, or belongs to a
  // profile the Space no longer lives in: stop offering it.
  void ClearParked(const std::string& space_id);
  // The Space is gone: erase its record, and when it existed only as a
  // PARKED window from the last session drop that window on the session
  // side too — the next unfiltered restore would otherwise replay it as a
  // loose window of a Space that no longer exists.
  void ForgetSpace(const std::string& space_id);

  // --- The harness surface (agentSpace.lazyRestore.*) ---------------------
  // The last lazy-restore receipt, verbatim.
  const std::string& last_receipt() const { return last_receipt_; }
  void ClearLastReceipt() { last_receipt_.clear(); }

 private:
  // Drops (framework side) and forgets the parked windows no Space claims,
  // and re-hosts the parked secondary windows of Spaces that still exist —
  // a window holding a live Space's tabs is never dropped.
  void DropOrphanGhosts();

  // The framework command interface; handed over at construction and never
  // null (across a module boundary, so not a raw_ptr).
  RAW_PTR_EXCLUSION phi_framework::PhiFramework& framework_;
  RAW_PTR_EXCLUSION Host& host_;
  DirtyCallback dirty_;

  // PhiRestore.json: the remembered windows, the parked list and the
  // last-active Space. The store is held behind a pointer so this header
  // stays host-clean.
  model::PhiRestoreModel model_;
  std::unique_ptr<PhiStore<model::PhiRestoreModel>> store_;
  // Which Spaces exist only as a parked window, and the window each Space
  // had last run — a view over `model_` (declared above, so it outlives it).
  PhiGhostSpaces ghosts_{&model_};
  // The last lazy-restore receipt, verbatim (the lazyRestore.* harness verbs
  // read it and clear it before a replay).
  std::string last_receipt_;
  // True from OnColdStartRestorePlan arming a filter until a user window
  // exists — the window the fallback timer checks for.
  bool cold_start_armed_ = false;
  // The cold start's eager window ids (previous-session) not announced yet;
  // emptied by a receipt or the fallback settle (SettleLiftsGuards).
  std::set<int64_t> awaiting_eager_;
  // The profiles the armed cold start has not heard a parked report from
  // yet (one arrives per profile as its diversion lands); emptied by a
  // receipt or the fallback settle (SettleLiftsGuards), never by the first
  // user shell's snapshot, which a report can trail.
  std::set<std::string> unreported_profiles_;

 public:
  // --- Pure helpers (host-tested; test/model/session_restore_test.cc) ------
  // What a reopen receipt says the settle reason is. `matchedNothing` (an
  // armed eager set that matched no window) and a replay that restored
  // nothing both leave the session with no window up and must be rescued;
  // anything else is an ordinary receipt. Bare strings, so the rule is
  // testable without JSON: the caller decodes.
  static const char* SettleReasonFor(bool eager_filter_matched_nothing,
                                     bool restored_any) {
    if (eager_filter_matched_nothing) {
      return "matchednothing";
    }
    return restored_any ? "receipt" : "nothingrestored";
  }
  // Whether a settle with this reason is a verdict on the replay — a
  // receipt ("receipt", "matchednothing", "nothingrestored") or the cold
  // start's fallback timer ("fallback") — and so lifts the PrimaryAwaited
  // guards. The first user shell's snapshot ("window") is not one: a
  // profile's park report may still land after it, and a row unguarded
  // before then is overwritten by a fresh window of its Space, leaving the
  // parked window to the orphan drop (2026-10-14 review H3).
  static bool SettleLiftsGuards(const char* reason) {
    return reason && std::string_view(reason) != "window";
  }
  // Release review B6: whether a Space's newly noted window must stand
  // BESIDE the Space's remembered primary (a secondary row) rather than
  // replace it. It must while that primary still holds tabs the user can
  // get back — parked, on its way from the cold start's eager set, or live
  // as the Space's browser in another shell. Overwriting it then left the
  // primary unclaimed and the settle pass orphan-dropped it, deleting its
  // tabs (a second window via Ctrl+N / tear-off, or a restored window
  // hosted on the default Space).
  static bool KeepsPrimary(bool has_primary,
                           bool primary_is_this_window,
                           bool primary_parked,
                           bool primary_awaited,
                           bool primary_live_elsewhere) {
    if (!has_primary || primary_is_this_window) {
      return false;
    }
    return primary_parked || primary_awaited || primary_live_elsewhere;
  }
  // The last-active Space, given what the live window says and what the
  // file remembers. The live window always wins; a remembered id that no
  // longer names a Space falls back to the default.
  static std::string ResolveLastActiveSpace(const std::string& live_space,
                                            const std::string& remembered,
                                            bool remembered_still_exists,
                                            const std::string& default_space) {
    if (!live_space.empty()) {
      return live_space;
    }
    if (!remembered.empty() && remembered_still_exists) {
      return remembered;
    }
    return default_space;
  }
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SESSION_RESTORE_H_

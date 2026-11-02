// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_session_restore.h"

#include <optional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_store.h"

namespace phi_client {

PhiSessionRestore::PhiSessionRestore(phi_framework::PhiFramework& framework,
                                     Host& host,
                                     DirtyCallback dirty)
    : framework_(framework),
      host_(host),
      dirty_(std::move(dirty)),
      store_(std::make_unique<PhiStore<model::PhiRestoreModel>>()) {
  // The ghost view's mutations ride the coalesced-save timer like every
  // other client store.
  ghosts_.set_dirty_callback([this] { dirty_(); });
}

PhiSessionRestore::~PhiSessionRestore() = default;

void PhiSessionRestore::Load(const std::wstring& user_data_dir) {
  store_->Init(user_data_dir);
  store_->Load(&model_);
}

void PhiSessionRestore::Save() {
  store_->Save(model_);
}

void PhiSessionRestore::PrepareForShutdown() {
  model_.last_active_space_id = LastActiveSpaceId();  // The live window's.
}

std::string PhiSessionRestore::LastActiveSpaceId() const {
  // A live slot's current Space wins when there is one (a reopen mid-run);
  // at cold start there is no window yet, so answer what PhiRestore.json
  // remembers from the last run — falling back to the default Space when
  // that id was deleted since.
  const std::string live = host_.ActiveSpaceOfLiveWindow();
  return ResolveLastActiveSpace(live, model_.last_active_space_id,
                                !model_.last_active_space_id.empty() &&
                                    host_.HasSpace(model_.last_active_space_id),
                                host_.DefaultSpaceId());
}

void PhiSessionRestore::NoteActiveSpace(const std::string& space_id) {
  // Coalesced: a switch is one keypress, and the store rewrites the whole
  // file. The save timer folds a burst into one write; a shutdown flushes
  // it early (PrepareForShutdown).
  if (space_id.empty() || space_id == model_.last_active_space_id) {
    return;
  }
  model_.last_active_space_id = space_id;
  dirty_();
}

void PhiSessionRestore::OnColdStartRestorePlan(
    phi_framework::ColdStartPlan* plan,
    bool* armed) {
  // Refuse to gate until the Space records are actually loaded. Arming with
  // an empty set here would park every window the user expected to see; an
  // unfiltered replay is the correct fallback, and `*armed` arrives false.
  if (!plan || !armed || ghosts_.Empty()) {
    LOG(INFO) << "[PhiClient] ColdStartRestorePlan: NOT armed (records "
              << (ghosts_.Empty() ? "not loaded" : "loaded") << ")";
    host_.NoteRestorePlanned("unfiltered");
    return;
  }
  host_.NoteRestorePlanned("armed");
  // The active Space's window is rebuilt now; every other saved window parks
  // and materializes when the user switches to it. The ids go through the
  // framework's sink (api rule 6), never into its containers.
  const std::vector<int> eager = ghosts_.EagerWindowIds(LastActiveSpaceId());
  for (int window_id : eager) {
    plan->AddEagerWindow(window_id);
    awaiting_eager_.insert(window_id);
  }
  unreported_profiles_ = ghosts_.Profiles();
  *armed = true;
  LOG(INFO) << "[PhiClient] ColdStartRestorePlan: armed space="
            << LastActiveSpaceId() << " eager="
            << (eager.empty() ? 0 : eager.front());
  // A cold start has no settlement callback, so the "eager set matched
  // nothing" case (the ids named windows the session file no longer holds:
  // every window parks, none appears) is caught by a timer: once the
  // replay has had its say and no user window exists, open a default
  // shell. A user window arriving disarms it (NoteUserWindowUp).
  cold_start_armed_ = true;
  host_.ArmColdStartFallback(kColdStartFallbackMs);
}

void PhiSessionRestore::OnColdStartParkedWindows(
    const char* parked_json,
    const char* reporting_profile) {
  // Same shape as the reopen receipt's `parked`, so it feeds the one path.
  // Wrapped to match, since the cold start reports the list bare.
  base::DictValue receipt;
  std::optional<base::Value> parked = base::JSONReader::Read(
      parked_json ? parked_json : "[]", base::JSON_PARSE_RFC);
  if (parked && parked->is_list()) {
    receipt.Set("parked", std::move(*parked));
  } else {
    receipt.Set("parked", base::ListValue());
  }
  std::string json;
  base::JSONWriter::Write(receipt, &json);
  LOG(INFO) << "[PhiClient] ColdStartParked profile=" << reporting_profile
            << " parked=" << (parked_json ? parked_json : "(null)");
  ghosts_.ReplaceFromReceipt(json);
  if (reporting_profile) {
    unreported_profiles_.erase(reporting_profile);
  }
  host_.PushSpaceMenu();
  host_.NoteRestoring();
  if (host_.RestoreSettled()) {
    // The diversion landed AFTER the restore settled (a user shell's first
    // snapshot beat it): the settle-time orphan drop found nothing, so run
    // it for this receipt — the parked windows no Space claims must not be
    // replayed as loose windows by the next unfiltered restore.
    DropOrphanGhosts();
  }
  if (cold_start_armed_) {
    // The diversion landed; give the eager window (if any) a moment to be
    // built before deciding nothing came.
    host_.ArmColdStartFallback(kColdStartSettleMs);
  }
}

void PhiSessionRestore::OnSessionRestoreSettled(const char* receipt_json) {
  last_receipt_ = receipt_json ? receipt_json : "";
  // REPLACE, never merge — the receipt describes the whole ghost store.
  ghosts_.ReplaceFromReceipt(last_receipt_);
  unreported_profiles_.clear();
  host_.PushSpaceMenu();
  // The receipt's verdict names the settle reason: an armed eager set that
  // matched nothing parks the whole session with no window up, and a replay
  // that never started restores nothing either. Settle rescues both (and is
  // a no-op rescue when a window is up).
  std::optional<base::DictValue> receipt =
      base::JSONReader::ReadDict(last_receipt_, base::JSON_PARSE_RFC);
  const bool matched_nothing =
      receipt && receipt->FindBool("eagerFilterMatchedNothing").value_or(false);
  const bool restored_any =
      receipt && receipt->FindBool("restoredAny").value_or(true);
  Settle(SettleReasonFor(matched_nothing, restored_any));
}

void PhiSessionRestore::Settle(const char* reason) {
  // After a receipt or the fallback timer, an eager window that has not
  // come is not coming (the session file no longer held it), and a profile
  // that has not reported its parked windows replays nothing more this
  // launch: their Spaces' primary rows stop being guarded (PrimaryAwaited).
  // The first user shell's snapshot settles the phase too, but it is no
  // verdict on the replay: it can beat a profile's park report (see
  // OnColdStartParkedWindows), and a primary row unguarded in that gap is
  // overwritten by the next fresh window of its Space — the report then
  // finds the parked window unclaimed and the orphan drop deletes its
  // tabs. Those rows stay guarded until each profile reports or a receipt
  // replaces the store.
  if (SettleLiftsGuards(reason)) {
    awaiting_eager_.clear();
    unreported_profiles_.clear();
  }
  // Parked windows no Space claims (the Space was deleted while its window
  // sat parked) would be replayed as loose windows by the next unfiltered
  // restore; drop them once the receipt is in, whichever path brought it.
  DropOrphanGhosts();
  // The rescue and the phase move are the coordinator's.
  host_.OnRestoreSettled(reason);
}

void PhiSessionRestore::BeginWindowlessRestore() {
  // An unfiltered replay when the Space records are not loaded, as at a cold
  // start: arming with nothing would park every window.
  const bool armed = !ghosts_.Empty();
  const std::vector<int> eager =
      armed ? ghosts_.EagerWindowIds(LastActiveSpaceId()) : std::vector<int>();
  awaiting_eager_.insert(eager.begin(), eager.end());
  last_receipt_.clear();
  LOG(INFO) << "[PhiClient] windowless restore: armed=" << armed << " space="
            << LastActiveSpaceId() << " eager=" << eager.size();
  framework_.RestorePreviousSession("", armed,
                                    eager.empty() ? nullptr : eager.data(),
                                    static_cast<int>(eager.size()));
}

void PhiSessionRestore::OnColdStartFallbackTimer() {
  // The replay had its say and no user window disarmed this: the cold
  // start is settled either way (the rescue opens a shell when the eager
  // set matched nothing).
  host_.DisarmColdStartFallback();
  cold_start_armed_ = false;
  Settle("fallback");
}

void PhiSessionRestore::NoteUserWindowUp() {
  if (!cold_start_armed_) {
    return;
  }
  cold_start_armed_ = false;
  host_.DisarmColdStartFallback();
}

const std::string* PhiSessionRestore::SpaceForWindowId(
    BrowserId window_id,
    bool* secondary) const {
  return ghosts_.SpaceForWindowId(window_id, secondary);
}

std::optional<PhiGhostSpaces::Remembered> PhiSessionRestore::PrimaryWindow(
    const std::string& space_id) const {
  return ghosts_.PrimaryWindow(space_id);
}

bool PhiSessionRestore::PrimaryAwaited(const std::string& space_id) const {
  const std::optional<PhiGhostSpaces::Remembered> primary =
      ghosts_.PrimaryWindow(space_id);
  if (!primary || ghosts_.PrimaryNotedThisRun(space_id)) {
    return false;  // A row of this run names a window of this run.
  }
  return awaiting_eager_.count(primary->window_id.value) > 0 ||
         unreported_profiles_.count(primary->profile) > 0;
}

bool PhiSessionRestore::HasSecondaryWindow(const std::string& space_id,
                                           BrowserId window_id) const {
  return ghosts_.HasSecondaryWindow(space_id, window_id);
}

void PhiSessionRestore::NoteRestoredWindowArrived(BrowserId restored_from) {
  awaiting_eager_.erase(restored_from.value);
}

const PhiGhostSpaces::Remembered* PhiSessionRestore::Parked(
    const std::string& space_id) const {
  return ghosts_.Parked(space_id);
}

void PhiSessionRestore::NoteLiveWindow(const std::string& space_id,
                                       const std::string& profile,
                                       BrowserId window_id) {
  ghosts_.NoteLiveWindow(space_id, profile, window_id);
}

void PhiSessionRestore::NoteSecondaryWindow(const std::string& space_id,
                                            const std::string& profile,
                                            BrowserId window_id) {
  ghosts_.NoteSecondaryWindow(space_id, profile, window_id);
}

void PhiSessionRestore::ForgetSecondaryWindow(const std::string& space_id,
                                              BrowserId window_id) {
  ghosts_.ForgetSecondaryWindow(space_id, window_id);
}

void PhiSessionRestore::ClearParked(const std::string& space_id) {
  ghosts_.ClearParked(space_id);
}

void PhiSessionRestore::ForgetSpace(const std::string& space_id) {
  const bool was_parked = ghosts_.Parked(space_id) != nullptr;
  // Its parked secondary windows go with it, like its primary.
  for (const PhiGhostSpaces::Stray& stray : ghosts_.TakeStrays(space_id)) {
    framework_.DropGhostWindow(stray.window.profile.c_str(),
                               static_cast<int>(stray.window.window_id.value));
  }
  const PhiGhostSpaces::Remembered ghost = ghosts_.Forget(space_id);
  if (was_parked && ghost.window_id.valid()) {
    framework_.DropGhostWindow(ghost.profile.c_str(),
                               static_cast<int>(ghost.window_id.value));
  }
}

void PhiSessionRestore::DropOrphanGhosts() {
  // Only windows NO row claims are dropped: one a secondary row claims
  // holds the tabs of a Space that still exists. It cannot park behind the
  // Space (the primary does), so it is re-hosted as the shell of its own
  // it was — announced with its previous-session id, AttachRestoredGhost
  // finds the row and hosts it on its Space. Taken first: a
  // materialization may announce inside the call.
  for (const PhiGhostSpaces::Stray& stray : ghosts_.TakeStrays()) {
    const int window_id = static_cast<int>(stray.window.window_id.value);
    if (!host_.HasSpace(stray.space_id)) {
      framework_.DropGhostWindow(stray.window.profile.c_str(), window_id);
      continue;
    }
    const int outcome = framework_.MaterializeGhostWindow(
        stray.window.profile.c_str(), window_id);
    LOG(INFO) << "[PhiClient] re-hosting parked window " << window_id
              << " of Space " << stray.space_id << " outcome=" << outcome;
    if (outcome == 2) {
      // No such ghost: nothing will ever bring it back.
      ghosts_.ForgetSecondaryWindow(stray.space_id, stray.window.window_id);
    }
    // 1 (refused for now) leaves it parked: the next receipt offers it
    // again. Never dropped.
  }
  for (const PhiGhostSpaces::Remembered& orphan : ghosts_.Orphans()) {
    framework_.DropGhostWindow(orphan.profile.c_str(),
                               static_cast<int>(orphan.window_id.value));
  }
  ghosts_.ClearOrphans();
}

}  // namespace phi_client

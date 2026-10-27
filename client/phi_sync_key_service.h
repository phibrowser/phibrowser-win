// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_KEY_SERVICE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_KEY_SERVICE_H_

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "phibrowser-win/client/phi_sync_engine.h"
#include "phibrowser-win/client/phi_sync_keys.h"
#include "phibrowser-win/client/phi_sync_owned_win.h"

namespace phi_client {

class PhiAuth;
class PhiSpaceManager;
class PhiSyncSidecarFile;

// ONE signed-in account's sync key layer (docs/user-data-parity.md S3) —
// the Mac's SyncKeyController as the coordinator owns it: built on sign-in,
// retired on sign-out or an account switch, never shared between accounts.
//
// Every key operation runs on this service's own worker thread (they block
// on /keys/v1), one at a time, in the order asked; results come back on the
// UI thread through a message-only window, and a result for a service that
// has since been destroyed is dropped. Destruction retires the key layer at
// once — a call parked in the network finishes on the worker and writes
// nothing (SyncKeyController.retire, review A11) — and never blocks the UI
// thread on it.
//
// UI thread only (the worker is private).
class PhiSyncKeyService {
 public:
  enum class Unlock { kUnknown, kWorking, kUnlocked, kNeedsJoin, kNotSignedIn,
                      kFailed };

  // The key layer as the UI and Chromium see it, refreshed after every
  // operation.
  struct Snapshot {
    Unlock unlock = Unlock::kUnknown;
    bool paired = false;
    bool needs_pairing = false;
    bool needs_pairing_actionable = false;
    std::string device_key_id;
    // What Chromium may be handed: the resolved keys, only while paired.
    std::map<std::string, keys::ResolvedKey> exposed;
    // KeyStatusName of the last failed background pass ("" after success).
    std::string last_error;
    // Phi native sync (S5): the engine runs while unlocked and paired.
    bool native_sync_active = false;
    std::string native_sync_outcome;  // the last round's outcome
    int native_sync_pages = 0;
    bool native_sync_needs_reconfiguration = false;
    // An account reset is journaled (marker.json resetOperationId): Set up
    // sync again will finish it and delete the account's sync data, so the
    // confirmation copy must say so (the Mac's resetResumePending).
    bool reset_resume_pending = false;
    bool invalidation_stream_healthy = false;
    // The status card's "Phi data" context (the engine's SyncStatus).
    sync::SyncStatus native_sync_status;
    // The Space kind: the gate and the last round's counters.
    bool native_sync_spaces_enabled = false;
    sync::SpaceRoundCounters native_sync_spaces;
    std::map<std::string, sync::OwnedRoundCounters> native_sync_owned;
  };

  // Where Phi native sync keeps its state, and the settings it syncs.
  struct NativeSyncConfig {
    std::wstring marker_path;    // <account dir>\sync\marker.json
    std::wstring state_path;     // <UserData>\Phi\syncstate\<account>.json
    std::wstring sidecar_path;   // <UserData>\PhiSyncSidecars.json
    std::string build_identity;
    // The settings registry; its reads and writes marshal to the UI thread
    // through the function it is handed.
    // `space_lookup` maps an account Space uuid to its local id (worker).
    std::function<std::vector<sync::SyncableSetting>(
        std::function<bool(const std::function<void()>&)>,
        PhiSyncSidecarFile*,
        std::function<std::optional<std::string>(const std::string&)>)>
        make_settings;
    // The local default-Space role holder (UI thread), for seeding the
    // PhiDefaultSpaceUuid register.
    std::function<std::string()> default_space_id;
    // The Space kind (all UI thread): the account's Space list (null when
    // this account's data is not the one open), the user's local profile
    // ids, and where the sync table's hidden set goes after every write.
    std::wstring space_table_path;  // <account dir>\sync\spaces.json
    std::function<PhiSpaceManager*()> spaces;
    // True when `spaces` is null because this account's store could not
    // open (failed, needs a newer Phi) — no stall: Space arrivals park and
    // the rest of sync moves on.
    std::function<bool()> space_store_unavailable;
    std::function<std::set<std::string>()> known_profiles;
    // Creates a local profile for an account profile this device lacks
    // (UI thread; `done` gets the basename, "" on failure).
    std::function<void(const std::string& display_name,
                       std::function<void(const std::string&)> done)>
        create_profile;
    std::function<void(std::set<std::string>)> set_hidden_spaces;
    // The owned kinds (bookmarks, pins): their cursor tables and the store.
    std::wstring bookmarks_table_path;  // <account dir>\sync\bookmarks-cursors.json
    std::wstring pins_table_path;       // <account dir>\sync\pins-cursors.json
    std::wstring rules_table_path;      // <account dir>\sync\urlrules-cursors.json
    PhiOwnedSyncHost owned;
    // A Chromium sync hint from the invalidation stream (UI thread): an
    // empty uuid and no types ask every profile to catch up.
    std::function<void(const std::string& profile_uuid,
                       const std::vector<int>& data_types,
                       const std::string& excluding_client_id)>
        chromium_invalidation;
  };

  // KeyLayerViewModel.beginSetup's routes.
  enum class SetupRoute { kDone, kReadyToPair, kNotSignedIn, kChooseJoin,
                          kIntroduction, kNeedsReconfiguration, kError };

  struct DeviceList {
    keys::KeyStatus status;
    std::vector<keys::AccountDevice> devices;
    std::vector<keys::PendingApproval> pending;
  };

  // `account_dir` is the account's Phi\users\<dir>; `subject` its Auth0 sub
  // (every request's bearer must carry it). `auth` outlives the service.
  // `local_profiles` lists the user's Chromium profiles (UI thread);
  // `on_changed` runs after every snapshot change.
  PhiSyncKeyService(std::wstring account_dir,
                    std::string subject,
                    PhiAuth* auth,
                    std::function<std::vector<keys::LocalProfile>()> local_profiles,
                    std::function<void()> on_changed,
                    NativeSyncConfig native_sync);
  ~PhiSyncKeyService();

  PhiSyncKeyService(const PhiSyncKeyService&) = delete;
  PhiSyncKeyService& operator=(const PhiSyncKeyService&) = delete;

  const std::string& subject() const { return subject_; }
  const Snapshot& snapshot() const;

  // --- Background passes (login, a profile list change) --------------------
  void SilentUnlockAndResolve();
  void ResolveMappings();

  // --- The setup flow ---------------------------------------------------------
  void BeginSetup(std::function<void(SetupRoute, keys::KeyStatus)> done);
  // First device: a fresh account key; `code` is shown ONCE.
  void Bootstrap(std::function<void(keys::KeyStatus, std::string code)> done);
  void RequestJoin(
      std::function<void(keys::KeyStatus, keys::JoinTicket)> done);
  void PollJoin(const std::string& request_id,
                std::function<void(keys::KeyStatus, keys::JoinPoll)> done);
  // Withdraws the request (fire and forget) and makes any poll in flight
  // unable to consume a late approval.
  void CancelJoin(const std::string& request_id);
  void JoinWithRecoveryCode(const std::string& code,
                            std::function<void(keys::KeyStatus)> done);
  void StartPairing(
      std::function<void(keys::KeyStatus, keys::PairingCandidates)> done);
  void ApplyDecisions(std::vector<keys::PairingDecision> decisions,
                      std::function<void(keys::KeyStatus)> done);

  // --- Settings ▸ Sync ----------------------------------------------------------
  void Refresh(std::function<void(DeviceList)> done);
  void Approve(const keys::PendingApproval& approval,
               std::function<void(keys::KeyStatus)> done);
  void Deny(const std::string& request_id,
            std::function<void(keys::KeyStatus)> done);
  void RemoveThisDevice(std::function<void(keys::KeyStatus)> done);
  // The account reset (the Mac's resetSyncAndRemoveThisDevice), offered on
  // the account's only active device: the engine stops, the operation UUID
  // is journaled in marker.json (an existing one is reused), the server
  // deletes every sync row of the account, and only then does the shared
  // removal cleanup run. kNotLastDevice / kDeviceNotFound: the server
  // deleted nothing, the journal forgets the operation and local state
  // stays. Any other failure keeps the UUID; from the moment it is
  // journaled the snapshot reports native_sync_needs_reconfiguration and
  // ReconfigureSync resumes the operation first.
  void ResetSyncAndRemoveThisDevice(std::function<void(keys::KeyStatus)> done);
  // Explicitly confirmed reset; ordinary setup/retry never clears metadata.
  // A journaled account reset is retried first with the same UUID (`reset`
  // → cleanup with removal; a refusal → ordinary reconfiguration).
  void ReconfigureSync(std::function<void(keys::KeyStatus)> done);

  // --- Phi native sync (S5) -------------------------------------------------
  // A round now (pull, then publish).
  void SyncNow();
  // A synced preference changed locally: a round runs ~2 s after the last
  // change. Ignored while the engine itself is writing preferences.
  void NoteLocalSettingsChanged();
  // True while a pulled settings entity is being written into the
  // preferences (the host's observer must not read that as a local edit).
  bool applying_remote() const;
  // A local Space edit (name, icon, colour, order, theme, profile): a round
  // runs ~2 s after the last one. The host skips the sync's own writes
  // (PhiSpaceManager::sync_applying).
  void NoteLocalSpacesChanged();
  // The user deleted a local Space: its account entity is tombstoned at the
  // next push. Before the Space's mapping could go.
  void NoteLocalSpaceDeleted(const std::string& space_id);
  // A bookmark or pinned-tab row changed locally (the store's signal, not
  // the engine's own landing): a round runs ~2 s after the last one.
  void NoteLocalOwnedChanged();
  // The local default-Space role moved to `space_id` (a user delete handed
  // it on): the register publishes its account uuid.
  void NoteDefaultSpaceHandOff(const std::string& space_id);

  // --- Test rig ---------------------------------------------------------------
  // READ-ONLY look at the account's Phi native sync data (S5 bring-up): the
  // domain key by GET only (never minted), a full GetUpdates from an empty
  // marker, every entity decrypted and summarized per kind. Nothing is
  // stored. `done` gets a JSON object.
  void PeekPhiData(std::function<void(std::string json)> done);

 private:
  struct Worker;
  struct UiState;

  // Runs `job` on the worker with this call's local profiles; `job`
  // returns a closure that runs on the UI thread with the fresh snapshot
  // already applied.
  using Job = std::function<std::function<void()>(Worker&)>;
  void Post(Job job);

  const std::string subject_;
  std::function<std::vector<keys::LocalProfile>()> local_profiles_;
  std::shared_ptr<UiState> ui_;
  std::shared_ptr<Worker> worker_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_KEY_SERVICE_H_

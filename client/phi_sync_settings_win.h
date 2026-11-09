// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_SETTINGS_WIN_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_SETTINGS_WIN_H_

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "client/phi_sync_engine.h"

namespace phi_client {

class PhiPreferences;
class PhiSyncSidecarFile;

// Runs `work` on the UI thread and waits for it; false when it could not
// (the client is shutting down).
using PhiRunOnUi = std::function<bool(const std::function<void()>& work)>;

// The Mac's SyncableSettings.all over the Windows preferences
// (docs/user-data-parity.md §4.4): the same keys, the same value types, the
// same "refuse what does not fit" writes. Reads and writes run on the UI
// thread through `run_on_ui` (the engine runs on the key worker).
//
// All twelve of the Mac's keys are registered.
// Two keys carry a peer's value this platform cannot show as is — the
// Mac's "performance" layout (retired here) and a theme id that is not a
// catalog theme ("default", a newer build's theme). Refusing them would get
// the local value re-published over the peer's (a refused write leaves the
// sidecars alone), so they are HELD instead: the preference takes the local
// stand-in (balanced / the current theme), and the key keeps reading back
// the held value until the user changes that preference here.
//
// PhiPinnedTabScope (the Mac's PinnedTabScopeMirror) reads the store's
// scope; a peer's value lands by migrating the store to it at once through
// `set_pin_scope` ("space" / "profile" / "app"), so the account's scope and
// this device's never stay apart.
//
// PhiDefaultSpaceUuid (the Mac's PhiDefaultSpaceMirror) is the account's
// default-Space role register, kept device-wide beside the sidecars
// (kDefaultSpaceMirrorKey). A landed value moves the local role when the
// uuid resolves to a Space here (`space_lookup` on the worker, then
// `apply_default_space` on the UI thread); a local hand-off writes the
// register (PhiSyncKeyService::NoteDefaultSpaceHandOff).
inline constexpr char kDefaultSpaceMirrorKey[] = "PhiDefaultSpaceUuid.mirror";
struct PhiSyncSettingsHooks {
  std::function<void(const std::string& scope_name)> set_pin_scope;  // UI
  std::function<std::optional<std::string>(const std::string& uuid)>
      space_lookup;                                                     // worker
  std::function<void(const std::string& space_id)> apply_default_space;  // UI
};
std::vector<sync::SyncableSetting> PhiSyncableSettings(
    PhiPreferences* prefs,
    PhiRunOnUi run_on_ui,
    PhiSyncSidecarFile* held,
    PhiSyncSettingsHooks hooks);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_SETTINGS_WIN_H_

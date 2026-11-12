// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_CLEANUP_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_CLEANUP_H_

#include <functional>

#include "client/phi_sync_engine.h"

namespace phi_client::sync {

// The caller stops writers before entering. Every step is idempotent: a
// failed step leaves the account's journal intact for explicit retry, even
// after a restart. Never clear row identities while old cursors still exist.
struct CleanupActions {
  std::function<bool()> clear_keys;
  std::function<bool()> remove_owned_cursors;
  std::function<bool()> clear_bookmark_ids;
  std::function<bool()> clear_engine_state;
  std::function<bool()> clear_spaces;
  std::function<bool()> remove_marker;
};

inline bool CleanupSyncState(MarkerStore& store,
                             bool removing_device,
                             const CleanupActions& actions) {
  MarkerFile journal = store.Load();
  journal.requires_reconfiguration = true;
  journal.removal_pending = journal.removal_pending || removing_device;
  return store.Save(journal) && actions.clear_keys() &&
         actions.remove_owned_cursors() && actions.clear_bookmark_ids() &&
         actions.clear_engine_state() && actions.clear_spaces() &&
         actions.remove_marker();
}

}  // namespace phi_client::sync

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_CLEANUP_H_

// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_GUEST_MIGRATION_WIN_H_
#define PHIBROWSER_WIN_CLIENT_PHI_GUEST_MIGRATION_WIN_H_

#include <string>

#include "client/phi_guest_migration.h"

// The Guest → account migration's files and directories on Windows (the
// Mac's GuestDataMigrationCoordinator, journal store and receipt store):
//
//   <UserData>\Phi\guestDataMigration\pending.json      the journal
//   <account dir>\guestDataMigrations\receipts\<op>.json the receipt
//   <UserData>\Phi\users\.default-account-id.guest-migration-<op>.pending-delete
//                                                        the staged Guest dir
//
// The Guest store must be CLOSED (the coordinator's local_store_ holds it
// while Guest is open) before Migrate runs; both stores are opened here by
// path. The journal makes every step resumable: a relaunch after a crash
// resumes against the same target and never imports twice.

namespace phi_client {

struct PhiGuestMigrationResult {
  enum class Outcome {
    kNothing,   // No Guest directory: nothing to move.
    kImported,  // The target holds the Guest data; cleanup may run.
    // The import did not happen; the Guest directory is intact and no
    // journal is left, so the Guest data stays where it was.
    kFailedGuestIntact,
    // A journal names another target, or a partial import exists: nothing
    // is touched and the journal is kept for a same-target retry.
    kNeedsRecovery,
  };
  Outcome outcome = Outcome::kNothing;
  store::GuestMappings mappings;
  std::string error;
};

// GuestDataMigrationCoordinator.migrate (plus recoverStagedCleanupIfNeeded's
// resume): snapshot, plan, journal, import, verify, themes, receipt.
PhiGuestMigrationResult PhiMigrateGuestData(const std::wstring& user_data_dir,
                                            const std::string& target_account,
                                            const std::string& folder_title);

// finalizeSourceDirectoryCleanup: stages the Guest directory (a rename),
// deletes it and retires the journal. False keeps the journal for a retry.
bool PhiFinalizeGuestCleanup(const std::wstring& user_data_dir,
                             const std::string& target_account,
                             std::string* error);

// The launch half, before any account's data opens: a journal left by a
// crash is resumed when `opening_account` is its target; otherwise a
// finished import only has its Guest directory removed, and a prepared
// journal whose import never landed is dropped (Guest intact).
void PhiRecoverGuestMigration(const std::wstring& user_data_dir,
                              const std::string& opening_account,
                              const std::string& folder_title);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_GUEST_MIGRATION_WIN_H_

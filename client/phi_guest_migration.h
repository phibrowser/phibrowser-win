// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_GUEST_MIGRATION_H_
#define PHIBROWSER_WIN_CLIENT_PHI_GUEST_MIGRATION_H_

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "client/phi_local_store.h"

// Guest → account data migration (docs/user-data-parity.md S6): the Mac's
// GuestDataMigration.swift. Signing in from Guest Mode moves the Guest
// account's Spaces, bookmarks, pinned tabs, URL rules and Space themes into
// the signed-in account's store, then removes the Guest directory:
//
//   snapshot (Guest store) → plan (against the target store) → journal
//   "prepared" → import (ONE target flush) → verify → receipt → journal
//   "targetImported" → the target opens → the Guest directory is staged
//   (renamed) → journal "sourceDirectoryStaged" → deleted → journal removed.
//
// The planner and the import are the Mac's, rule for rule: ids are kept
// unless the target already uses them (then "guest-import-<op>-<kind>-<n>"),
// default-Space bookmarks go into an "Imported from Guest" folder, pins are
// projected onto the target's pin scope and deduplicated by lineage +
// content, and a URL rule whose (Space, host, path) the target already has
// is skipped (target wins). Theme values merge with target winning.
//
// PURE: STL + the LocalStore, host-tested (test/model/guest_migration_test).
// The journal files and the directory work are phi_guest_migration_win.

namespace phi_client::store {

// GuestDataMigrationThemeSnapshot over the Windows keys.
struct GuestThemes {
  std::map<std::string, std::string> theme_ids;  // spaceThemeIds
  // spaceOverlayOpacities: {spaceId: {"light": x, "dark": y}}.
  std::map<std::string, std::map<std::string, double>> overlay_opacities;
  std::map<std::string, int> saturations;  // winSpaceSaturations
  GuestThemes Retaining(const std::set<std::string>& space_ids) const;
  bool operator==(const GuestThemes&) const = default;
};

// GuestDataMigrationSnapshot. Rows are the store's own types: bookmarks
// with the Guest's hidden roots cut (a top-level row has no parent) and no
// account identity; pins as stored; live URL rules only.
struct GuestSnapshot {
  std::string snapshot_id;
  std::string source_user_id;
  TimeMs captured_at = 0;
  PinScope source_scope = PinScope::kProfile;
  std::vector<Profile> profiles;
  std::vector<Space> spaces;
  std::vector<Tab> bookmarks;
  std::vector<Tab> pins;
  std::vector<UrlRule> rules;
  GuestThemes themes;
  // hasSameMigratableContent: identity and capture time are not content.
  bool SameContent(const GuestSnapshot& other) const;
};

struct GuestPinTarget {
  std::string source_guid;
  std::string target_guid;
  std::optional<std::string> target_profile_id;
  std::optional<std::string> target_space_id;
  int target_index = 0;
  std::optional<std::string> target_split_partner_guid;
  bool should_insert = false;
  bool dormant = false;
  bool operator==(const GuestPinTarget&) const = default;
};

struct GuestRuleTarget {
  std::string source_id;
  std::string target_id;
  std::string target_space_id;
  int target_sort_order = 0;
  bool operator==(const GuestRuleTarget&) const = default;
};

struct GuestSkippedRule {
  std::string source_id;
  std::string target_space_id;
  std::string host;
  std::optional<std::string> path_prefix;
  bool operator==(const GuestSkippedRule&) const = default;
};

// GuestDataMigrationIdentifierMappings.
struct GuestMappings {
  std::map<std::string, std::string> profile_ids;
  std::map<std::string, std::string> space_ids;
  std::map<std::string, std::string> bookmark_guids;
  std::vector<GuestPinTarget> pin_targets;
  std::map<std::string, std::string> pin_lineage_ids;
  std::vector<GuestRuleTarget> rule_targets;
  std::vector<GuestSkippedRule> skipped_rules;
  std::optional<std::string> default_import_folder_guid;
  PinScope target_scope = PinScope::kProfile;
  std::map<std::string, std::string> target_space_profile_ids;
  std::vector<std::string> inserted_profile_ids;
  std::vector<std::string> inserted_space_ids;

  // modelGUIDs: bookmarks one-to-one, then each pin's first target (a pin
  // may fan out to several owners). What open tabs are rebound through.
  std::map<std::string, std::string> ModelGuids() const;
  // The row guids the import creates (bookmarks, the import folder, the
  // pins it inserts).
  std::set<std::string> NewModelGuids() const;
  bool operator==(const GuestMappings&) const = default;
};

enum class GuestPresence { kAbsent, kComplete, kPartial };

// makeGuestDataMigrationSnapshot. False (with `error`) when Guest rows name
// a custom Space the store does not hold.
bool MakeGuestSnapshot(const LocalStore& source,
                       const std::string& source_user_id,
                       const GuestThemes& themes,
                       const std::string& snapshot_id,
                       TimeMs now,
                       GuestSnapshot* out,
                       std::string* error);

// planGuestDataImport against the target's current rows. `operation_id` is
// the lowercase uuid that generated ids carry.
bool PlanGuestImport(const GuestSnapshot& snapshot,
                     const LocalStore& target,
                     const std::string& operation_id,
                     GuestMappings* out,
                     std::string* error);

// guestImportPresence: none, all or part of the planned ids exist.
GuestPresence GuestImportPresence(const LocalStore& target,
                                  const GuestMappings& mappings,
                                  std::string* detail);

// verifyGuestDataMigrationReceipt's store half.
bool VerifyGuestImport(const LocalStore& target,
                       const GuestSnapshot& snapshot,
                       const GuestMappings& mappings,
                       std::string* error);

// mergeGuestSpaceThemes: Guest values land under the mapped Space ids only
// where the target has none (target wins).
GuestThemes MergeGuestThemes(const GuestThemes& target,
                             const GuestThemes& guest,
                             const std::map<std::string, std::string>& space_ids);

// guestBookmarkTargetIndexes: each imported bookmark's index among its
// imported siblings.
std::map<std::string, int> GuestBookmarkTargetIndexes(
    const GuestSnapshot& snapshot,
    const GuestMappings& mappings);

}  // namespace phi_client::store

#endif  // PHIBROWSER_WIN_CLIENT_PHI_GUEST_MIGRATION_H_

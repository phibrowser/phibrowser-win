// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_MERGE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_MERGE_H_

#include <cstdint>
#include <string>

#include "client/phi_sync_proto.h"

// Phi native sync's merge layer (docs/user-data-parity.md S4): the pure,
// symmetric merge of one entity's local and remote versions, per kind —
// phibrowser-mac SyncableSettings / SyncableSpaces / BookmarkKind / PinKind /
// URLRuleKind — plus the client tags and their hash. Host-clean; pinned by
// test/sync-vectors (every expected byte there is what the Swift computed).
//
// Two rules hold everywhere:
//   - LWW: the larger updated_at_ms wins; on a tie the lexicographically
//     greater SERIALIZED bytes win — symmetric, so two devices merging the
//     same pair agree.
//   - A merge starts from REMOTE (the server's bytes), so fields a newer
//     client wrote and this build cannot name survive.
namespace phi_client::sync {

// SyncableSettings.lwwWinner.
SettingValue LwwWinner(const SettingValue& lhs, const SettingValue& rhs);
// The value's bytes with updated_at_ms zeroed: "did it change?" and every
// content signature below.
std::string ValueSignature(const SettingValue& value);

// Settings: per-key LWW over the union of keys; unknown keys carried through.
SettingEntity MergeSettings(const SettingEntity& local,
                            const SettingEntity& remote);

// Spaces: field LWW, min() for created_at_ms, profile/theme only when either
// side carries them.
SpaceEntity MergeSpace(const SpaceEntity& local, const SpaceEntity& remote);
// An agent Space's signature: never materialized from a peer.
bool SpaceRefuses(const SpaceEntity& entity);

// The owned kinds' two non-LWW folds, shared by merge and stamping (a
// projection that folded differently would republish forever): source keeps
// the nonzero, then the smaller; created_at_ms the earliest nonzero.
int32_t MergedSource(int32_t left, int32_t right);
int64_t MergedCreatedAtMs(int64_t left, int64_t right);

// Bookmarks: LOCATION (space_uuid for a root, parent_uuid for a descendant)
// merges as one unit with one stamp; rank follows the location winner unless
// both name the same location; content fields LWW one by one; is_folder is
// the remote's (refusal guards mismatches); source / created_at_ms fold.
BookmarkEntity MergeBookmark(const BookmarkEntity& local,
                             const BookmarkEntity& remote);
std::string BookmarkContentSignature(const BookmarkEntity& entity);
int64_t BookmarkLocationStamp(const BookmarkEntity& entity);
int64_t BookmarkContentStamp(const BookmarkEntity& entity);

// Pinned tabs: plain field LWW (owner is identity, never merged).
PinTabEntity MergePin(const PinTabEntity& local, const PinTabEntity& remote);
// "<lineage>:<owner key>" (owner key: the Space / profile uuid, or "app");
// "" for an empty lineage.
std::string PinIdentity(const PinTabEntity& entity);
std::string PinOwnerKey(const PinTabEntity& entity);
std::string PinContentSignature(const PinTabEntity& entity);
int64_t PinLocationStamp(const PinTabEntity& entity);
int64_t PinContentStamp(const PinTabEntity& entity);

// URL rules: the content group (host + path_prefix + ask) merges as one unit
// on host's stamp; target LWW; rank follows the target winner unless the
// targets agree.
UrlRuleEntity MergeUrlRule(const UrlRuleEntity& local,
                           const UrlRuleEntity& remote);
std::string UrlRuleContentSignature(const UrlRuleEntity& entity);
int64_t UrlRuleLocationStamp(const UrlRuleEntity& entity);
int64_t UrlRuleContentStamp(const UrlRuleEntity& entity);

// --- Client tags (PhiSyncEntity) ------------------------------------------------

inline constexpr char kSettingsClientTag[] = "phi-settings";
std::string SpaceClientTag(const std::string& space_uuid);
std::string BookmarkClientTag(const std::string& bookmark_uuid);
std::string PinClientTag(const std::string& lineage,
                         const std::string& owner_key);
std::string UrlRuleClientTag(const std::string& rule_uuid);
// Chromium's rule: base64(SHA1(<serialized EntitySpecifics{phi:{}}> + tag)),
// the prefix being 82 7D 00 (field 2000, length 0).
std::string ClientTagHash(const std::string& tag);

}  // namespace phi_client::sync

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_MERGE_H_

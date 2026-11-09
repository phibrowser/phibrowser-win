// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_OWNED_KINDS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_OWNED_KINDS_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "client/phi_sync_owned.h"
#include "client/phi_sync_proto.h"

// The bookmark and pinned-tab kinds (phibrowser-mac BookmarkKind / PinKind):
// codec, identity, projection, stamping, merge and refusal over the local
// rows sync reads (PhiLocalBookmark / PhiLocalPin). Host-clean.
namespace phi_client::sync {

inline constexpr char kBookmarkEntityName[] = "phi-bookmark";
inline constexpr char kPinEntityName[] = "phi-pin";

// One bookmark or folder as sync sees it (PhiLocalBookmark).
struct LocalBookmark {
  std::optional<std::string> sync_id;  // the account uuid; nullopt = unminted
  std::string guid;
  std::string space_id;
  std::string profile_id;
  std::optional<std::string> parent_guid;  // nullopt = the Space root
  int index = 0;
  bool is_folder = false;
  std::string title;
  std::string url;
  std::optional<std::string> secondary_url;
  std::optional<std::string> secondary_title;
  int source = 0;
  int64_t created_ms = 0;
  std::optional<int64_t> content_updated_ms;
  std::optional<int64_t> location_updated_ms;
  bool operator==(const LocalBookmark& other) const = default;
};

// One pinned tab as sync sees it (PhiLocalPin). Owner: Space when
// `space_id`, else profile when `profile_id`, else the app.
struct LocalPin {
  std::string lineage_id;
  std::string guid;
  std::optional<std::string> space_id;
  std::optional<std::string> profile_id;
  int index = 0;
  std::string title;
  std::string url;
  std::optional<std::string> split_partner_lineage_id;
  int source = 0;
  int64_t created_ms = 0;
  std::optional<int64_t> content_updated_ms;
  bool is_dormant = false;
  bool operator==(const LocalPin& other) const = default;
};

struct BookmarkKind {
  using Entity = BookmarkEntity;
  using Local = LocalBookmark;
  static constexpr bool kTombstoneYieldsToLocalEdits = true;

  static std::string ClientTag(const std::string& identity);
  static std::string Identity(const Entity& entity) {
    return entity.bookmark_uuid;
  }
  static std::optional<std::string> LocalIdentity(
      const Local& local,
      const OwnerResolver&,
      const std::optional<std::string>&) {
    return local.sync_id;
  }
  static PhiEntity Envelope(const Entity& entity);
  static std::optional<Entity> FromEnvelope(const PhiEntity& envelope);
  static std::string LocalId(const Local& local) { return local.guid; }
  static std::optional<std::string> LocalParentId(const Local& local) {
    return local.parent_guid;
  }
  static std::optional<std::string> EligibilityOwner(
      const Local& local,
      const OwnerResolver& resolve,
      const std::optional<std::string>&) {
    return resolve.sync_uuid(local.space_id);
  }
  static std::vector<std::string> OwnerUuids(const Entity& entity);
  static std::optional<Entity> Project(
      const Local& local,
      const OwnerResolver& resolve,
      const std::optional<std::string>& scope,
      const std::optional<std::string>& parent_identity);
  static std::string Rank(const Entity& entity) {
    return entity.rank.get().str();
  }
  static std::string ContentSignature(const Entity& entity);
  static int64_t LocationStamp(const Entity& entity);
  static int64_t ContentStamp(const Entity& entity);
  static std::optional<Entity> Reverted(const Entity& entity,
                                        const std::optional<Entity>& baseline,
                                        int64_t stamp);
  static Entity Stamp(const Entity& projected,
                      const std::optional<Entity>& baseline,
                      const Local& local,
                      const std::string& rank,
                      int64_t now,
                      int64_t hlc_max,
                      int64_t wall_offset_ms);
  static Entity Merge(const Entity& local, const Entity& remote);
  static std::optional<OwnedRefusal> Refuses(
      const Entity& entity,
      const std::optional<Entity>& baseline);
  static std::optional<std::string> TargetOwnerUuid(const Entity&) {
    return std::nullopt;
  }
  static std::optional<RuleProjection> TransferSource(const Entity&,
                                                      const OwnerResolver&) {
    return std::nullopt;
  }
  // Wire ranks → dense sibling indexes (unranked rows first, then rank, then
  // identity), keyed by guid.
  static std::map<std::string, int> RankToIndex(
      const std::vector<Local>& siblings,
      const std::map<std::string, std::string>& ranks);
};

struct PinKind {
  using Entity = PinTabEntity;
  using Local = LocalPin;
  static constexpr bool kTombstoneYieldsToLocalEdits = true;

  // Lineage ids compare lowercase.
  static std::string LineageKey(const std::string& lineage_id);
  // "<lineage>:<owner key>"
  static std::string ClientTag(const std::string& identity);
  static std::string Identity(const Entity& entity);
  static std::optional<std::string> LocalIdentity(
      const Local& local,
      const OwnerResolver& resolve,
      const std::optional<std::string>& scope);
  static PhiEntity Envelope(const Entity& entity);
  static std::optional<Entity> FromEnvelope(const PhiEntity& envelope);
  static std::string LocalId(const Local& local) { return local.guid; }
  static std::optional<std::string> LocalParentId(const Local&) {
    return std::nullopt;
  }
  static std::optional<std::string> EligibilityOwner(
      const Local& local,
      const OwnerResolver& resolve,
      const std::optional<std::string>& scope);
  static std::vector<std::string> OwnerUuids(const Entity& entity);
  static std::optional<Entity> Project(
      const Local& local,
      const OwnerResolver& resolve,
      const std::optional<std::string>& scope,
      const std::optional<std::string>& parent_identity);
  static std::string Rank(const Entity& entity) {
    return entity.rank.get().str();
  }
  static std::string ContentSignature(const Entity& entity);
  static int64_t LocationStamp(const Entity& entity);
  static int64_t ContentStamp(const Entity& entity);
  static std::optional<Entity> Reverted(const Entity&,
                                        const std::optional<Entity>&,
                                        int64_t) {
    return std::nullopt;  // no parent reference: never in a cycle
  }
  static Entity Stamp(const Entity& projected,
                      const std::optional<Entity>& baseline,
                      const Local& local,
                      const std::string& rank,
                      int64_t now,
                      int64_t hlc_max,
                      int64_t wall_offset_ms);
  static Entity Merge(const Entity& local, const Entity& remote);
  static std::optional<OwnedRefusal> Refuses(
      const Entity& entity,
      const std::optional<Entity>& baseline);
  static std::optional<std::string> TargetOwnerUuid(const Entity&) {
    return std::nullopt;
  }
  static std::optional<RuleProjection> TransferSource(const Entity&,
                                                      const OwnerResolver&) {
    return std::nullopt;
  }
  // The row's owner key: its Space id, else profile id, else "app" (local
  // ids, before translation).
  static std::string LocalOwnerKey(const Local& local);
  // The deterministic lineage a variant row is reminted to (§7.2 / A11).
  static std::string MintedLineage(const std::string& lineage, int ordinal);
  // "<lineage>:<owner>" → its halves.
  static std::pair<std::string, std::string> IdentityHalves(
      const std::string& identity);
};

}  // namespace phi_client::sync

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_OWNED_KINDS_H_

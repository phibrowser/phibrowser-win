// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_sync_owned_kinds.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include "client/phi_key_crypto.h"
#include "client/phi_sync_clock.h"
#include "client/phi_sync_merge.h"
#include "client/phi_sync_rank.h"

namespace phi_client::sync {

namespace {

SettingValue StringValue(const std::string& value) {
  SettingValue out;
  out.SetString(value);
  return out;
}

// A changed value is stamped max(edit, overwritten + 1) (AM-1); an unchanged
// one keeps the baseline's stamp.
int64_t Restamped(const OptionalValue& value,
                  const OptionalValue& baseline,
                  int64_t edit_wall_ms) {
  return ValueSignature(value.get()) == ValueSignature(baseline.get())
             ? baseline.get().updated_at_ms
             : HybridClock::EditStamp(edit_wall_ms,
                                      baseline.get().updated_at_ms);
}

void SetStamp(OptionalValue& value, int64_t stamp) {
  value.mut().updated_at_ms = stamp;
}

// The expected account-uuid shape: no uppercase, no whitespace (a device
// GUID is uppercase).
bool IsAccountUuid(const std::string& uuid) {
  if (uuid.empty()) {
    return false;
  }
  for (const char c : uuid) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isupper(u) || std::isspace(u)) {
      return false;
    }
  }
  return true;
}

bool IsNormalizedLineage(const std::string& lineage) {
  if (!IsAccountUuid(lineage)) {
    return false;
  }
  return lineage.find(':') == std::string::npos &&
         lineage.find('\0') == std::string::npos;
}

// Foundation's URL(string:) refuses an empty string; anything else a peer
// stored is taken as it is (Windows keeps URLs as strings).
bool IsUrl(const std::string& url) {
  return !url.empty();
}

// Location: a root is (space, ""), a descendant ("", parent) — a
// descendant's space_uuid is diagnostic only.
std::pair<std::string, std::string> LocationValue(const BookmarkEntity& e) {
  const std::string parent = e.parent_uuid.get().str();
  return parent.empty()
             ? std::make_pair(e.space_uuid.get().str(), std::string())
             : std::make_pair(std::string(), parent);
}

}  // namespace

// --- BookmarkKind ---------------------------------------------------------------------

std::string BookmarkKind::ClientTag(const std::string& identity) {
  return BookmarkClientTag(identity);
}

PhiEntity BookmarkKind::Envelope(const Entity& entity) {
  PhiEntity out;
  out.kind = PhiEntity::Kind::kBookmark;
  out.bookmark = entity;
  return out;
}

std::optional<BookmarkEntity> BookmarkKind::FromEnvelope(
    const PhiEntity& envelope) {
  if (envelope.kind != PhiEntity::Kind::kBookmark) {
    return std::nullopt;
  }
  return envelope.bookmark;
}

// What must resolve before landing: the parent for a descendant, the Space
// for a root. Never a descendant's (diagnostic) space_uuid.
std::vector<std::string> BookmarkKind::OwnerUuids(const Entity& entity) {
  const std::string parent = entity.parent_uuid.get().str();
  if (!parent.empty()) {
    return {parent};
  }
  const std::string space = entity.space_uuid.get().str();
  return space.empty() ? std::vector<std::string>()
                       : std::vector<std::string>{space};
}

std::optional<BookmarkEntity> BookmarkKind::Project(
    const Local& local,
    const OwnerResolver& resolve,
    const std::optional<std::string>&,
    const std::optional<std::string>& parent_identity) {
  const std::optional<std::string> space_uuid = resolve.sync_uuid(local.space_id);
  if (!space_uuid || (local.parent_guid && !parent_identity)) {
    return std::nullopt;
  }
  BookmarkEntity entity;
  entity.bookmark_uuid = local.sync_id.value_or(std::string());
  entity.space_uuid.set(StringValue(*space_uuid));
  entity.parent_uuid.set(StringValue(parent_identity.value_or(std::string())));
  entity.rank.set(StringValue(std::string()));
  entity.is_folder = local.is_folder;
  entity.title.set(StringValue(local.title));
  entity.url.set(StringValue(local.url));
  entity.secondary_url.set(
      StringValue(local.secondary_url.value_or(std::string())));
  entity.secondary_title.set(
      StringValue(local.secondary_title.value_or(std::string())));
  entity.source = local.source;
  entity.created_at_ms = local.created_ms;
  return entity;
}

std::string BookmarkKind::ContentSignature(const Entity& entity) {
  return BookmarkContentSignature(entity);
}

int64_t BookmarkKind::LocationStamp(const Entity& entity) {
  return BookmarkLocationStamp(entity);
}

int64_t BookmarkKind::ContentStamp(const Entity& entity) {
  return BookmarkContentStamp(entity);
}

// C5-a's loser: back where its baseline says, the location stamped `stamp`,
// the baseline's (legal) rank with it; no baseline → its Space root.
std::optional<BookmarkEntity> BookmarkKind::Reverted(
    const Entity& entity,
    const std::optional<Entity>& baseline,
    int64_t stamp) {
  BookmarkEntity out = entity;
  out.space_uuid = baseline ? baseline->space_uuid : entity.space_uuid;
  if (baseline) {
    out.parent_uuid = baseline->parent_uuid;
  } else {
    out.parent_uuid.set(StringValue(std::string()));
  }
  SetStamp(out.space_uuid, stamp);
  SetStamp(out.parent_uuid, stamp);
  if (baseline && IsLegalRank(baseline->rank.get().str())) {
    out.rank = baseline->rank;
  }
  return out;
}

BookmarkEntity BookmarkKind::Stamp(const Entity& projected,
                                   const std::optional<Entity>& baseline,
                                   const Local& local,
                                   const std::string& rank,
                                   int64_t now,
                                   int64_t hlc_max,
                                   int64_t wall_offset_ms) {
  BookmarkEntity out = projected;
  out.rank.set(StringValue(rank));
  const int64_t content_stamp = HybridClock::Corrected(
      local.content_updated_ms.value_or(local.created_ms), wall_offset_ms);
  const std::optional<int64_t> location_edit =
      local.location_updated_ms
          ? std::optional<int64_t>(HybridClock::Corrected(
                *local.location_updated_ms, wall_offset_ms))
          : std::nullopt;

  if (!baseline) {
    const int64_t created = HybridClock::EditStamp(content_stamp, hlc_max);
    const int64_t located =
        location_edit ? HybridClock::EditStamp(*location_edit, hlc_max) : 0;
    SetStamp(out.space_uuid, located);
    SetStamp(out.parent_uuid, located);
    SetStamp(out.rank, 0);
    SetStamp(out.title, created);
    SetStamp(out.url, created);
    SetStamp(out.secondary_url, created);
    SetStamp(out.secondary_title, created);
    return out;
  }

  // R-exec-16: the non-LWW folds follow the baseline, as merge would.
  out.created_at_ms = MergedCreatedAtMs(out.created_at_ms, baseline->created_at_ms);
  if (baseline->source != 0) {
    out.source = baseline->source;
  }
  // A descendant's space_uuid is never republished (R-M3-3-18).
  if (!out.parent_uuid.get().str().empty()) {
    out.space_uuid = baseline->space_uuid;
  }
  const int64_t baseline_location = LocationStamp(*baseline);
  int64_t location_stamp = now;
  if (LocationValue(out) == LocationValue(*baseline)) {
    location_stamp = baseline_location;
  } else if (location_edit) {
    location_stamp = HybridClock::EditStamp(*location_edit, baseline_location);
  }
  SetStamp(out.space_uuid, location_stamp);
  SetStamp(out.parent_uuid, location_stamp);
  SetStamp(out.rank, Restamped(out.rank, baseline->rank, now));
  SetStamp(out.title, Restamped(out.title, baseline->title, content_stamp));
  SetStamp(out.url, Restamped(out.url, baseline->url, content_stamp));
  SetStamp(out.secondary_url,
           Restamped(out.secondary_url, baseline->secondary_url, content_stamp));
  SetStamp(out.secondary_title, Restamped(out.secondary_title,
                                          baseline->secondary_title,
                                          content_stamp));
  return out;
}

BookmarkEntity BookmarkKind::Merge(const Entity& local, const Entity& remote) {
  return MergeBookmark(local, remote);
}

std::optional<OwnedRefusal> BookmarkKind::Refuses(
    const Entity& entity,
    const std::optional<Entity>& baseline) {
  if (!IsAccountUuid(entity.bookmark_uuid)) {
    return OwnedRefusal::kInvalidUuid;
  }
  if (!IsLegalRank(entity.rank.get().str())) {
    return OwnedRefusal::kIllegalRank;
  }
  if (entity.parent_uuid.get().str() == entity.bookmark_uuid) {
    return OwnedRefusal::kSelfReference;
  }
  if (!entity.is_folder && !IsUrl(entity.url.get().str())) {
    return OwnedRefusal::kInvalidUrl;
  }
  if (baseline && baseline->is_folder != entity.is_folder) {
    return OwnedRefusal::kIsFolderMismatch;
  }
  return std::nullopt;
}

std::map<std::string, int> BookmarkKind::RankToIndex(
    const std::vector<Local>& siblings,
    const std::map<std::string, std::string>& ranks) {
  std::vector<const Local*> ordered;
  for (const Local& row : siblings) {
    ordered.push_back(&row);
  }
  auto rank_of = [&](const Local* row) {
    if (!row->sync_id) {
      return std::string();
    }
    const auto it = ranks.find(*row->sync_id);
    return it == ranks.end() ? std::string() : it->second;
  };
  std::stable_sort(ordered.begin(), ordered.end(),
                   [&](const Local* a, const Local* b) {
                     const std::string left = rank_of(a);
                     const std::string right = rank_of(b);
                     if (left != right) {
                       return left < right;
                     }
                     return a->sync_id.value_or(a->guid) <
                            b->sync_id.value_or(b->guid);
                   });
  std::map<std::string, int> out;
  for (size_t index = 0; index < ordered.size(); ++index) {
    out[ordered[index]->guid] = static_cast<int>(index);
  }
  return out;
}

// --- PinKind --------------------------------------------------------------------------

namespace {

const std::string kUnresolvedOwnerKey("\0unresolved-owner", 17);

// The row's account owner: Space uuid, profile uuid, or the app; nullopt
// when a Space / profile is not mapped.
std::optional<std::pair<PinTabEntity::Owner, std::string>> PinOwnerOf(
    const LocalPin& local,
    const OwnerResolver& resolve) {
  if (local.space_id) {
    const std::optional<std::string> uuid = resolve.sync_uuid(*local.space_id);
    if (!uuid) {
      return std::nullopt;
    }
    return std::make_pair(PinTabEntity::Owner::kSpace, *uuid);
  }
  if (local.profile_id) {
    const std::optional<std::string> uuid =
        resolve.global_uuid(*local.profile_id);
    if (!uuid) {
      return std::nullopt;
    }
    return std::make_pair(PinTabEntity::Owner::kProfile, *uuid);
  }
  return std::make_pair(PinTabEntity::Owner::kApp, std::string());
}

}  // namespace

std::string PinKind::LineageKey(const std::string& lineage_id) {
  std::string out = lineage_id;
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

std::pair<std::string, std::string> PinKind::IdentityHalves(
    const std::string& identity) {
  const size_t separator = identity.find(':');
  if (separator == std::string::npos) {
    return {identity, std::string()};
  }
  return {identity.substr(0, separator), identity.substr(separator + 1)};
}

std::string PinKind::ClientTag(const std::string& identity) {
  const auto [lineage, owner] = IdentityHalves(identity);
  return PinClientTag(LineageKey(lineage), owner);
}

std::string PinKind::Identity(const Entity& entity) {
  return PinIdentity(entity);
}

std::optional<std::string> PinKind::LocalIdentity(
    const Local& local,
    const OwnerResolver& resolve,
    const std::optional<std::string>& scope) {
  if (local.is_dormant) {
    return std::nullopt;
  }
  const std::string lineage = LineageKey(local.lineage_id);
  if (lineage.empty()) {
    return std::nullopt;
  }
  const std::optional<std::string> owner =
      EligibilityOwner(local, resolve, scope);
  return lineage + ":" + owner.value_or(kUnresolvedOwnerKey);
}

PhiEntity PinKind::Envelope(const Entity& entity) {
  PhiEntity out;
  out.kind = PhiEntity::Kind::kPinTab;
  out.pin_tab = entity;
  return out;
}

std::optional<PinTabEntity> PinKind::FromEnvelope(const PhiEntity& envelope) {
  if (envelope.kind != PhiEntity::Kind::kPinTab) {
    return std::nullopt;
  }
  return envelope.pin_tab;
}

std::optional<std::string> PinKind::EligibilityOwner(
    const Local& local,
    const OwnerResolver& resolve,
    const std::optional<std::string>&) {
  const auto owner = PinOwnerOf(local, resolve);
  if (!owner) {
    return std::nullopt;
  }
  return owner->first == PinTabEntity::Owner::kApp ? std::string(kAppOwnerKey)
                                                   : owner->second;
}

std::vector<std::string> PinKind::OwnerUuids(const Entity& entity) {
  return {PinOwnerKey(entity)};
}

std::optional<PinTabEntity> PinKind::Project(
    const Local& local,
    const OwnerResolver& resolve,
    const std::optional<std::string>&,
    const std::optional<std::string>&) {
  const auto owner = PinOwnerOf(local, resolve);
  if (!owner) {
    return std::nullopt;
  }
  PinTabEntity entity;
  entity.pin_uuid = LineageKey(local.lineage_id);
  entity.owner = owner->first;
  entity.owner_uuid = owner->second;
  entity.rank.set(StringValue(std::string()));
  entity.title.set(StringValue(local.title));
  entity.url.set(StringValue(local.url));
  entity.split_partner_uuid.set(StringValue(
      local.split_partner_lineage_id ? LineageKey(*local.split_partner_lineage_id)
                                     : std::string()));
  entity.source = local.source;
  entity.created_at_ms = local.created_ms;
  return entity;
}

std::string PinKind::ContentSignature(const Entity& entity) {
  return PinContentSignature(entity);
}

int64_t PinKind::LocationStamp(const Entity& entity) {
  return PinLocationStamp(entity);
}

int64_t PinKind::ContentStamp(const Entity& entity) {
  return PinContentStamp(entity);
}

PinTabEntity PinKind::Stamp(const Entity& projected,
                            const std::optional<Entity>& baseline,
                            const Local& local,
                            const std::string& rank,
                            int64_t now,
                            int64_t hlc_max,
                            int64_t wall_offset_ms) {
  PinTabEntity out = projected;
  out.rank.set(StringValue(rank));
  const int64_t content_stamp = HybridClock::Corrected(
      local.content_updated_ms.value_or(local.created_ms), wall_offset_ms);
  // A half-landed split pair keeps its baseline link (§7.4).
  if (out.split_partner_uuid.get().str().empty() && baseline &&
      !baseline->split_partner_uuid.get().str().empty()) {
    out.split_partner_uuid = baseline->split_partner_uuid;
  }
  if (!baseline) {
    const int64_t created = HybridClock::EditStamp(content_stamp, hlc_max);
    SetStamp(out.rank, 0);
    SetStamp(out.title, created);
    SetStamp(out.url, created);
    SetStamp(out.split_partner_uuid, now);
    return out;
  }
  out.created_at_ms = MergedCreatedAtMs(out.created_at_ms, baseline->created_at_ms);
  if (baseline->source != 0) {
    out.source = baseline->source;
  }
  SetStamp(out.rank, Restamped(out.rank, baseline->rank, now));
  SetStamp(out.title, Restamped(out.title, baseline->title, content_stamp));
  SetStamp(out.url, Restamped(out.url, baseline->url, content_stamp));
  SetStamp(out.split_partner_uuid,
           Restamped(out.split_partner_uuid, baseline->split_partner_uuid, now));
  return out;
}

PinTabEntity PinKind::Merge(const Entity& local, const Entity& remote) {
  return MergePin(local, remote);
}

std::optional<OwnedRefusal> PinKind::Refuses(const Entity& entity,
                                             const std::optional<Entity>&) {
  if (!IsNormalizedLineage(entity.pin_uuid)) {
    return OwnedRefusal::kInvalidUuid;
  }
  if (!IsLegalRank(entity.rank.get().str())) {
    return OwnedRefusal::kIllegalRank;
  }
  if (!IsUrl(entity.url.get().str())) {
    return OwnedRefusal::kInvalidUrl;
  }
  return std::nullopt;
}

std::string PinKind::LocalOwnerKey(const Local& local) {
  if (local.space_id) {
    return *local.space_id;
  }
  if (local.profile_id) {
    return *local.profile_id;
  }
  return kAppOwnerKey;
}

std::string PinKind::MintedLineage(const std::string& lineage, int ordinal) {
  const std::string seed =
      "phi-pin-variant|" + lineage + "|" + std::to_string(ordinal);
  keys::Bytes digest = keys::Sha1(seed);
  digest.resize(16);
  const std::string hex = keys::HexLower(digest);
  std::string out;
  for (size_t offset = 0; offset < hex.size(); ++offset) {
    if (offset == 8 || offset == 12 || offset == 16 || offset == 20) {
      out.push_back('-');
    }
    out.push_back(hex[offset]);
  }
  return out;
}

}  // namespace phi_client::sync

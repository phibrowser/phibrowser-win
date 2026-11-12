// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_sync_owned.h"

#include <functional>
#include <memory>
#include <tuple>

#include "client/phi_sync_spaces.h"

namespace phi_client::sync {

void OwnedTable::DropExpiredTombstones(int64_t now_ms) {
  for (auto it = cursors.begin(); it != cursors.end();) {
    if (it->second.deleted_at_ms &&
        now_ms - *it->second.deleted_at_ms > kSpaceRetentionMs) {
      it = cursors.erase(it);
    } else {
      ++it;
    }
  }
}

OwnedOwnerMaps::OwnedOwnerMaps() = default;
OwnedOwnerMaps::OwnedOwnerMaps(const OwnedOwnerMaps&) = default;
OwnedOwnerMaps& OwnedOwnerMaps::operator=(const OwnedOwnerMaps&) = default;
OwnedOwnerMaps::~OwnedOwnerMaps() = default;

OwnerResolver OwnedOwnerMaps::Resolver() const {
  // A copy: the resolver outlives nothing it captures.
  auto maps = std::make_shared<OwnedOwnerMaps>(*this);
  auto find = [](const std::map<std::string, std::string>& table,
                 const std::string& key) -> std::optional<std::string> {
    const auto it = table.find(key);
    return it == table.end() ? std::nullopt
                             : std::optional<std::string>(it->second);
  };
  // "app" and "incognito-space" map to themselves: app-scoped pins and
  // incognito rules resolve without a stored mapping.
  auto self_mapped = [find](const std::map<std::string, std::string>& table,
                            const std::string& uuid) -> std::optional<std::string> {
    if (uuid == kAppOwnerKey || uuid == kIncognitoSpaceUuid) {
      return uuid;
    }
    return find(table, uuid);
  };
  OwnerResolver resolve;
  resolve.sync_uuid = [maps, find](const std::string& id) {
    return find(maps->sync_uuid_by_space_id, id);
  };
  resolve.local_space_id = [maps, find](const std::string& uuid) {
    return find(maps->local_space_id_by_sync_uuid, uuid);
  };
  resolve.is_eligible_space = [maps](const std::string& uuid) {
    return uuid == kAppOwnerKey || uuid == kIncognitoSpaceUuid ||
           maps->eligible_space_uuids.count(uuid) > 0;
  };
  resolve.global_uuid = [maps, self_mapped](const std::string& id) {
    return self_mapped(maps->global_uuid_by_profile_id, id);
  };
  resolve.local_profile_id = [maps, self_mapped](const std::string& uuid) {
    return self_mapped(maps->local_profile_id_by_global_uuid, uuid);
  };
  resolve.local_profile_id_for_space = [maps, find](const std::string& id) {
    return find(maps->local_profile_id_by_space_id, id);
  };
  return resolve;
}

bool RuleSignature::operator<(const RuleSignature& other) const {
  const auto key = [](const RuleSignature& s) {
    return std::make_tuple(std::cref(s.host), s.path_prefix ? 1 : 0,
                           s.path_prefix.value_or(std::string()),
                           std::cref(s.owner));
  };
  return key(*this) < key(other);
}

OwnedPlan::OwnedPlan() = default;
OwnedPlan::OwnedPlan(const OwnedPlan&) = default;
OwnedPlan::OwnedPlan(OwnedPlan&&) = default;
OwnedPlan& OwnedPlan::operator=(const OwnedPlan&) = default;
OwnedPlan& OwnedPlan::operator=(OwnedPlan&&) = default;
OwnedPlan::~OwnedPlan() = default;

std::map<std::string, int> OwnedDepths(
    const std::vector<std::string>& identities,
    const std::map<std::string, std::string>& parent_of) {
  std::map<std::string, int> out;
  const size_t limit = parent_of.size();
  for (const std::string& identity : identities) {
    if (out.count(identity)) {
      continue;
    }
    int hops = 0;
    auto it = parent_of.find(identity);
    while (it != parent_of.end() && static_cast<size_t>(hops) < limit) {
      ++hops;
      it = parent_of.find(it->second);
    }
    out[identity] = hops;
  }
  return out;
}

int OwnedStepPhase(OwnedStepKind kind) {
  switch (kind) {
    case OwnedStepKind::kClaim:
    case OwnedStepKind::kCreate:
    case OwnedStepKind::kMove:
      return 1;
    case OwnedStepKind::kUpdate:
      return 2;
    case OwnedStepKind::kTransfer:
      return 3;
    case OwnedStepKind::kDelete:
      return 4;
  }
  return 4;
}

std::set<std::string> OwnedUnpublishedMerges(
    const std::map<std::string, std::string>& projections,
    const OwnedTable& table) {
  std::set<std::string> out;
  for (const auto& [identity, bytes] : projections) {
    const auto cursor = table.cursors.find(identity);
    if (cursor == table.cursors.end() || !cursor->second.server ||
        !cursor->second.reconciled ||
        *cursor->second.server == *cursor->second.reconciled) {
      continue;
    }
    out.insert(identity);
  }
  return out;
}

}  // namespace phi_client::sync

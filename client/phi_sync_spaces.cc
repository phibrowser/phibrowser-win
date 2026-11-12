// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_sync_spaces.h"

#include <algorithm>
#include <cmath>
#include <utility>

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

SettingValue MilliValue(std::optional<double> value) {
  SettingValue out;
  out.SetInt(OpacityMilliUnits(value));
  return out;
}

std::optional<SpaceEntity> ParseSpace(const std::optional<std::string>& bytes) {
  SpaceEntity entity;
  if (!bytes || !entity.ParseFrom(*bytes)) {
    return std::nullopt;
  }
  return entity;
}

// Keep the baseline's stamp when the value is unchanged, the pending
// projection's when it matches that, else stamp the edit one above the
// value it overwrites (AM-1).
SettingValue Stamped(SettingValue value,
                     const std::optional<SettingValue>& baseline,
                     const std::optional<SettingValue>& pending,
                     int64_t now) {
  const std::string signature = ValueSignature(value);
  if (baseline && ValueSignature(*baseline) == signature) {
    value.updated_at_ms = baseline->updated_at_ms;
  } else if (pending && ValueSignature(*pending) == signature) {
    value.updated_at_ms = pending->updated_at_ms;
  } else {
    const int64_t overwritten = pending    ? pending->updated_at_ms
                                : baseline ? baseline->updated_at_ms
                                           : 0;
    value.updated_at_ms = HybridClock::EditStamp(now, overwritten);
  }
  return value;
}

std::optional<SettingValue> FieldOf(const std::optional<SpaceEntity>& entity,
                                    const OptionalValue SpaceEntity::*field) {
  if (!entity) {
    return std::nullopt;
  }
  return ((*entity).*field).get();
}

std::optional<std::string> ThemeIdOf(const SpaceEntity& entity) {
  const std::string id = entity.theme_id.get().str();
  return id.empty() ? std::nullopt : std::optional<std::string>(id);
}

}  // namespace

int64_t OpacityMilliUnits(std::optional<double> value) {
  return value ? static_cast<int64_t>(std::llround(*value * 1000)) : -1;
}

std::optional<double> OpacityFromMilli(const SettingValue& value) {
  const int64_t milli = value.integer();
  return milli < 0 ? std::nullopt
                   : std::optional<double>(static_cast<double>(milli) / 1000);
}

// --- SpaceSyncTable -------------------------------------------------------------------

bool SpaceSyncTable::RecordLocalDeletion(const std::string& uuid) {
  const auto it = cursors.find(uuid);
  if (it == cursors.end() || !it->second.entity_id ||
      it->second.deleted_at_ms || it->second.hidden ||
      it->second.pending_delete) {
    return false;
  }
  it->second.pending_delete = true;
  return true;
}

std::vector<std::string> SpaceSyncTable::PurgeExpired(int64_t now_ms) {
  std::vector<std::string> purged;
  for (auto& [uuid, cursor] : cursors) {
    if (!cursor.deleted_at_ms || cursor.purged_at_ms ||
        now_ms - *cursor.deleted_at_ms <= kSpaceRetentionMs) {
      continue;
    }
    SpaceCursor tombstone;
    tombstone.entity_id = cursor.entity_id;
    tombstone.version = cursor.version;
    tombstone.hidden = true;
    tombstone.deleted_at_ms = cursor.deleted_at_ms;
    tombstone.purged_at_ms = now_ms;
    cursor = std::move(tombstone);
    purged.push_back(uuid);
  }
  return purged;  // std::map order: already sorted
}

// --- Snapshot ---------------------------------------------------------------------------

std::map<std::string, SpaceEntity> SnapshotSpaces(
    const std::vector<LocalSpace>& spaces,
    const SpaceSyncTable& table,
    const std::function<std::optional<std::string>(const std::string&)>& global_uuid,
    const std::function<std::optional<std::string>(const std::string&)>& sync_uuid,
    int64_t now) {
  struct Eligible {
    size_t index;  // into `spaces`
    std::string uuid;
  };
  std::vector<Eligible> eligible;
  for (size_t index = 0; index < spaces.size(); ++index) {
    const LocalSpace& space = spaces[index];
    const std::optional<std::string> uuid = sync_uuid(space.space_id);
    if (!uuid) {
      continue;
    }
    const auto it = table.cursors.find(*uuid);
    if (it != table.cursors.end()) {
      const SpaceCursor& cursor = it->second;
      if (cursor.hidden || cursor.deleted_at_ms || cursor.refused_at_ms) {
        continue;
      }
      // A PARKED uuid is never published over: the account holds an entity
      // this device has not landed yet.
      if (cursor.pending_apply) {
        continue;
      }
    }
    eligible.push_back({index, *uuid});
  }

  std::map<std::string, SpaceEntity> baselines;
  std::map<std::string, SpaceEntity> pendings;
  for (const Eligible& item : eligible) {
    const auto it = table.cursors.find(item.uuid);
    if (it == table.cursors.end()) {
      continue;
    }
    if (std::optional<SpaceEntity> baseline = ParseSpace(it->second.reconciled)) {
      baselines[item.uuid] = std::move(*baseline);
      if (std::optional<SpaceEntity> pending =
              ParseSpace(it->second.pending_projection)) {
        pendings[item.uuid] = std::move(*pending);
      }
    }
  }
  auto baseline_rank = [&](const std::string& uuid) -> std::optional<std::string> {
    const auto it = baselines.find(uuid);
    if (it == baselines.end()) {
      return std::nullopt;
    }
    const std::string rank = it->second.rank.get().str();
    return IsLegalRank(rank) ? std::optional<std::string>(rank) : std::nullopt;
  };
  std::vector<RankedItem> order;
  for (const Eligible& item : eligible) {
    order.push_back({item.uuid, baseline_rank(item.uuid)});
  }
  const std::map<std::string, std::string> new_ranks = AssignRanks(order);

  std::map<std::string, SpaceEntity> out;
  for (const Eligible& item : eligible) {
    const LocalSpace& space = spaces[item.index];
    const std::string& uuid = item.uuid;
    const auto base_it = baselines.find(uuid);
    const auto pend_it = pendings.find(uuid);
    const std::optional<SpaceEntity> baseline =
        base_it == baselines.end() ? std::nullopt
                                   : std::optional<SpaceEntity>(base_it->second);
    const std::optional<SpaceEntity> pending =
        pend_it == pendings.end() ? std::nullopt
                                  : std::optional<SpaceEntity>(pend_it->second);
    const bool is_default = uuid == kDefaultSpaceUuid;

    SpaceEntity entity;
    entity.space_uuid = uuid;
    entity.name.set(Stamped(StringValue(space.name),
                            FieldOf(baseline, &SpaceEntity::name),
                            FieldOf(pending, &SpaceEntity::name), now));
    entity.icon_name.set(Stamped(StringValue(space.icon_name),
                                 FieldOf(baseline, &SpaceEntity::icon_name),
                                 FieldOf(pending, &SpaceEntity::icon_name), now));
    entity.color_hex.set(Stamped(StringValue(space.color_hex),
                                 FieldOf(baseline, &SpaceEntity::color_hex),
                                 FieldOf(pending, &SpaceEntity::color_hex), now));

    // With no baseline the rank is DERIVED from local order, so it carries
    // stamp 0 and never beats a real drag anywhere in the account.
    const auto new_rank = new_ranks.find(uuid);
    const std::string rank_value =
        new_rank != new_ranks.end() ? new_rank->second
                                    : baseline_rank(uuid).value_or("V");
    SettingValue rank = StringValue(rank_value);
    if (!baseline) {
      rank.updated_at_ms = 0;
    } else if (new_rank != new_ranks.end()) {
      const SpaceEntity& stamp_source = pending ? *pending : *baseline;
      const int64_t overwritten = stamp_source.rank.get().updated_at_ms;
      rank.updated_at_ms =
          pending && pending->rank.get().str() == rank_value
              ? pending->rank.get().updated_at_ms
              : HybridClock::EditStamp(now, overwritten);
    } else {
      rank.updated_at_ms = baseline->rank.get().updated_at_ms;
    }
    entity.rank.set(rank);

    if (!is_default) {
      // A held remote binding is echoed with the BASELINE's stamp, and only
      // while the row is still on the profile it was taken against.
      const auto cursor_it = table.cursors.find(uuid);
      const SpaceCursor* cursor =
          cursor_it == table.cursors.end() ? nullptr : &cursor_it->second;
      const bool hold_applies =
          cursor && cursor->held_for_local_profile_id &&
          *cursor->held_for_local_profile_id == space.profile_id;
      if (hold_applies && cursor->held_profile_uuid && baseline) {
        SettingValue binding = StringValue(*cursor->held_profile_uuid);
        binding.updated_at_ms = baseline->profile_uuid.get().updated_at_ms;
        entity.profile_uuid.set(binding);
      } else if (const std::optional<std::string> profile_uuid =
                     global_uuid(space.profile_id)) {
        entity.profile_uuid.set(
            Stamped(StringValue(*profile_uuid),
                    FieldOf(baseline, &SpaceEntity::profile_uuid),
                    FieldOf(pending, &SpaceEntity::profile_uuid), now));
      } else {
        // No mapping: never put a device-local basename on the wire.
        continue;
      }
      entity.theme_id.set(Stamped(StringValue(space.theme_id.value_or("")),
                                  FieldOf(baseline, &SpaceEntity::theme_id),
                                  FieldOf(pending, &SpaceEntity::theme_id), now));
    }
    entity.overlay_opacity_light.set(
        Stamped(MilliValue(space.opacity_light),
                FieldOf(baseline, &SpaceEntity::overlay_opacity_light),
                FieldOf(pending, &SpaceEntity::overlay_opacity_light), now));
    entity.overlay_opacity_dark.set(
        Stamped(MilliValue(space.opacity_dark),
                FieldOf(baseline, &SpaceEntity::overlay_opacity_dark),
                FieldOf(pending, &SpaceEntity::overlay_opacity_dark), now));
    entity.created_at_ms = space.created_ms;
    out[uuid] = std::move(entity);
  }
  return out;
}

// --- Landing ----------------------------------------------------------------------------

std::optional<std::string> LandSpace(const SpaceEntity& merged,
                                     const std::optional<LocalSpace>& existing,
                                     const std::optional<std::string>& local_space_id,
                                     const std::optional<std::string>& profile_id,
                                     SpaceLocalAccess& access,
                                     const std::function<std::string()>& new_id,
                                     int64_t now_ms) {
  const bool is_default = merged.space_uuid == kDefaultSpaceUuid;
  const int64_t created = merged.created_at_ms > 0 ? merged.created_at_ms : now_ms;
  const std::optional<double> light = OpacityFromMilli(merged.overlay_opacity_light.get());
  const std::optional<double> dark = OpacityFromMilli(merged.overlay_opacity_dark.get());

  if (!existing) {
    // A new Space: its profile must resolve (the caller parked it
    // otherwise), or, for the well-known default identity, this device's
    // own default profile.
    if (!profile_id) {
      return std::nullopt;
    }
    const std::string id = local_space_id ? *local_space_id : new_id();
    LocalSpace space;
    space.space_id = id;
    space.profile_id = *profile_id;
    space.name = merged.name.get().str();
    space.color_hex = merged.color_hex.get().str();
    space.icon_name = merged.icon_name.get().str();
    space.sort_order = INT32_MAX;
    space.created_ms = created;
    space.theme_id = is_default ? std::nullopt : ThemeIdOf(merged);
    space.opacity_light = light;
    space.opacity_dark = dark;
    if (!access.Create(space)) {
      return std::nullopt;
    }
    if (!is_default &&
        !access.ApplyThemeState(id, ThemeIdOf(merged), light, dark)) {
      return std::nullopt;
    }
    return id;
  }

  const std::string local_id =
      local_space_id ? *local_space_id : existing->space_id;
  if (!is_default || light != existing->opacity_light ||
      dark != existing->opacity_dark) {
    if (!access.ApplyThemeState(local_id,
                                is_default ? existing->theme_id
                                           : ThemeIdOf(merged),
                                light, dark)) {
      return std::nullopt;
    }
  }
  // A rebind is this one field changing on the wire.
  if (!is_default && profile_id && *profile_id != existing->profile_id) {
    if (!access.Rebind(local_id, *profile_id)) {
      return std::nullopt;
    }
  }
  const std::string name = merged.name.get().str();
  const std::string color = merged.color_hex.get().str();
  const std::string icon = merged.icon_name.get().str();
  const bool created_changed = created != existing->created_ms;
  if (name != existing->name || color != existing->color_hex ||
      icon != existing->icon_name || created_changed) {
    if (!access.Update(
            local_id,
            name == existing->name ? std::nullopt : std::optional<std::string>(name),
            color == existing->color_hex ? std::nullopt
                                         : std::optional<std::string>(color),
            icon == existing->icon_name ? std::nullopt
                                        : std::optional<std::string>(icon),
            created_changed ? std::optional<int64_t>(created) : std::nullopt)) {
      return std::nullopt;
    }
  }
  return local_id;
}

// --- Order projection ------------------------------------------------------------------

std::vector<std::string> PlannedSpaceOrder(
    const std::vector<LocalSpace>& local_order,
    const std::map<std::string, std::pair<std::string, std::string>>& synced_ranks) {
  std::vector<size_t> slots;
  std::vector<std::string> synced;
  for (size_t i = 0; i < local_order.size(); ++i) {
    if (synced_ranks.count(local_order[i].space_id)) {
      slots.push_back(i);
      synced.push_back(local_order[i].space_id);
    }
  }
  std::stable_sort(synced.begin(), synced.end(),
                   [&](const std::string& a, const std::string& b) {
                     const auto& l = synced_ranks.at(a);
                     const auto& r = synced_ranks.at(b);
                     return l.first == r.first ? l.second < r.second
                                               : l.first < r.first;
                   });
  std::vector<std::string> out;
  for (const LocalSpace& space : local_order) {
    out.push_back(space.space_id);
  }
  for (size_t i = 0; i < slots.size(); ++i) {
    out[slots[i]] = synced[i];
  }
  return out;
}

}  // namespace phi_client::sync

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_sync_spaces_win.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

#include "client/phi_agent_spaces.h"
#include "client/phi_key_api_win.h"
#include "client/phi_local_store.h"
#include "client/phi_space_manager.h"

namespace phi_client {

namespace {

bool IsReservedUuid(const std::string& uuid) {
  return uuid == sync::kDefaultSpaceUuid || uuid == sync::kIncognitoSpaceUuid;
}

}  // namespace

PhiSyncSpaceAccess::PhiSyncSpaceAccess(
    PhiKeyStateFile& state,
    keys::ProfileKeyManager& profiles,
    PhiRunOnUi run_on_ui,
    std::function<PhiSpaceManager*()> spaces,
    std::function<bool()> store_unavailable,
    std::function<std::set<std::string>()> known_profiles,
    ProfileHooks profile_hooks)
    : state_(state),
      profiles_(profiles),
      run_on_ui_(std::move(run_on_ui)),
      spaces_(std::move(spaces)),
      store_unavailable_(std::move(store_unavailable)),
      known_profiles_(std::move(known_profiles)),
      profile_hooks_(std::move(profile_hooks)) {}

PhiSyncSpaceAccess::~PhiSyncSpaceAccess() = default;

std::optional<std::vector<PhiSyncSpaceAccess::Row>> PhiSyncSpaceAccess::Rows() {
  // The closure owns what it fills: a hop that times out finds nothing
  // dangling. A failed hop or no open Space list is no answer, never an
  // empty list.
  struct Read {
    std::optional<std::vector<PhiSpaceManager::SyncRow>> rows;
    bool store_unavailable = false;
  };
  auto read = std::make_shared<Read>();
  std::function<PhiSpaceManager*()> spaces = spaces_;
  std::function<bool()> unavailable = store_unavailable_;
  last_read_store_unavailable_ = false;
  if (!run_on_ui_([read, spaces, unavailable] {
        if (PhiSpaceManager* manager = spaces()) {
          read->rows = manager->SyncRows();
        } else if (unavailable) {
          read->store_unavailable = unavailable();
        }
      })) {
    return std::nullopt;  // a stalled or retired hop: transient
  }
  if (!read->rows) {
    last_read_store_unavailable_ = read->store_unavailable;
    return std::nullopt;
  }
  std::vector<Row> out;
  int order = 0;
  for (const PhiSpaceManager::SyncRow& row : *read->rows) {
    if (PhiIsAgentSpaceId(row.id)) {
      continue;
    }
    Row item;
    item.space.space_id = row.id;
    item.space.profile_id = row.profile_id;
    item.space.name = row.name;
    item.space.color_hex = row.color_hex;
    item.space.icon_name = row.icon;
    item.space.sort_order = order++;
    item.space.created_ms = row.created_ms;
    if (!row.theme_id.empty()) {
      item.space.theme_id = row.theme_id;
    }
    item.space.opacity_light = row.opacity_light;
    item.space.opacity_dark = row.opacity_dark;
    item.hidden = row.hidden;
    out.push_back(std::move(item));
  }
  return out;
}

bool PhiSyncSpaceAccess::Write(std::function<bool(PhiSpaceManager&)> write) {
  auto landed = std::make_shared<bool>(false);
  std::function<PhiSpaceManager*()> spaces = spaces_;
  return run_on_ui_([landed, spaces, write] {
           if (PhiSpaceManager* manager = spaces()) {
             *landed = write(*manager);
           }
         }) &&
         *landed;
}

std::set<std::string> PhiSyncSpaceAccess::HiddenLocalIds(
    const sync::SpaceSyncTable& table) {
  std::set<std::string> hidden;
  for (const auto& [uuid, cursor] : table.cursors) {
    if (cursor.hidden || cursor.deleted_at_ms) {
      if (const std::optional<std::string> local = LocalSpaceId(uuid)) {
        hidden.insert(*local);
      }
    }
  }
  return hidden;
}

// --- Reads ----------------------------------------------------------------------------

std::optional<std::vector<sync::LocalSpace>>
PhiSyncSpaceAccess::CurrentSpaces() {
  std::optional<std::vector<Row>> rows = Rows();
  if (!rows) {
    return std::nullopt;
  }
  std::vector<sync::LocalSpace> out;
  for (Row& row : *rows) {
    if (row.hidden) {
      continue;
    }
    // A Space whose profile has no account uuid is never published (the
    // default Space always is: its binding is not on the wire).
    if (row.space.space_id != sync::kDefaultSpaceUuid &&
        !GlobalUuid(row.space.profile_id)) {
      continue;
    }
    out.push_back(std::move(row.space));
  }
  return out;
}

std::optional<std::vector<sync::LocalSpace>>
PhiSyncSpaceAccess::AllSpacesForOrdering() {
  std::optional<std::vector<Row>> rows = Rows();
  if (!rows) {
    return std::nullopt;
  }
  std::vector<sync::LocalSpace> out;
  for (Row& row : *rows) {
    out.push_back(std::move(row.space));
  }
  return out;
}

std::optional<std::vector<sync::LocalSpace>>
PhiSyncSpaceAccess::PairableSpaces() {
  std::optional<std::vector<Row>> rows = Rows();
  if (!rows) {
    return std::nullopt;
  }
  std::vector<sync::LocalSpace> out;
  for (Row& row : *rows) {
    if (!row.hidden) {
      out.push_back(std::move(row.space));
    }
  }
  return out;
}

bool PhiSyncSpaceAccess::SpaceStoreUnavailable() {
  return last_read_store_unavailable_;
}

std::optional<std::string> PhiSyncSpaceAccess::GlobalUuid(
    const std::string& profile_id) {
  return profiles_.MappedGlobalUuid(profile_id);
}

std::optional<std::string> PhiSyncSpaceAccess::LocalProfileId(
    const std::string& uuid) {
  return profiles_.LocalProfileIdFor(uuid);
}

sync::LocalPresence PhiSyncSpaceAccess::IsKnownLocalProfile(
    const std::string& profile_id) {
  auto known = std::make_shared<std::set<std::string>>();
  std::function<std::set<std::string>()> list = known_profiles_;
  if (!list || !run_on_ui_([known, list] { *known = list(); })) {
    return sync::LocalPresence::kUnreachable;
  }
  return known->count(profile_id) ? sync::LocalPresence::kPresent
                                  : sync::LocalPresence::kAbsent;
}

void PhiSyncSpaceAccess::DropProfileMapping(const std::string& profile_id) {
  profiles_.RemoveMapping(profile_id);
}

std::string PhiSyncSpaceAccess::DefaultProfileId() {
  return store::kDefaultProfileId;
}

// --- The Space identity mapping (the Mac's SpaceSyncMappingManager) --------------------
// The default Space is a constant branch both ways, never a stored row.

std::optional<std::string> PhiSyncSpaceAccess::SyncUuid(
    const std::string& space_id) {
  if (space_id == sync::kDefaultSpaceUuid) {
    return std::string(sync::kDefaultSpaceUuid);
  }
  const std::map<std::string, std::string> mappings = state_.SpaceMappings();
  const auto it = mappings.find(space_id);
  return it == mappings.end() ? std::nullopt
                              : std::optional<std::string>(it->second);
}

std::optional<std::string> PhiSyncSpaceAccess::LocalSpaceId(
    const std::string& uuid) {
  if (uuid == sync::kDefaultSpaceUuid) {
    return std::string(sync::kDefaultSpaceUuid);
  }
  // Two locals on one uuid is not meant to happen; the smallest id wins.
  for (const auto& [space_id, mapped] : state_.SpaceMappings()) {
    if (mapped == uuid) {
      return space_id;
    }
  }
  return std::nullopt;
}

std::optional<std::string> PhiSyncSpaceAccess::EnsureMapped(
    const std::string& space_id) {
  if (std::optional<std::string> uuid = SyncUuid(space_id)) {
    return uuid;
  }
  // Minted lowercase (local ids are uppercase), persisted BEFORE it is
  // used: a uuid published without a durable mapping would mint a duplicate
  // account Space after a restart.
  std::map<std::string, std::string> mappings = state_.SpaceMappings();
  const std::string uuid = keys::NewLowercaseUuid();
  mappings[space_id] = uuid;
  if (!state_.SetSpaceMappings(mappings)) {
    return std::nullopt;
  }
  return uuid;
}

bool PhiSyncSpaceAccess::MapSpace(const std::string& space_id,
                                  const std::string& uuid) {
  if (IsReservedUuid(uuid) || space_id == sync::kDefaultSpaceUuid ||
      space_id.empty()) {
    return false;
  }
  std::map<std::string, std::string> mappings = state_.SpaceMappings();
  if (mappings.count(space_id)) {
    return false;
  }
  for (const auto& [id, mapped] : mappings) {
    if (mapped == uuid) {
      return false;  // another local Space claimed it
    }
  }
  mappings[space_id] = uuid;
  return state_.SetSpaceMappings(mappings);
}

void PhiSyncSpaceAccess::DropSpaceMapping(const std::string& space_id) {
  std::map<std::string, std::string> mappings = state_.SpaceMappings();
  if (mappings.erase(space_id)) {
    state_.SetSpaceMappings(mappings);
  }
}

sync::LocalPresence PhiSyncSpaceAccess::IsKnownLocalSpace(
    const std::string& space_id) {
  const std::optional<std::vector<Row>> rows = Rows();
  if (!rows) {
    return sync::LocalPresence::kUnreachable;
  }
  for (const Row& row : *rows) {
    if (row.space.space_id == space_id) {
      return sync::LocalPresence::kPresent;
    }
  }
  return sync::LocalPresence::kAbsent;
}

std::map<std::string, std::string> PhiSyncSpaceAccess::AllSpaceMappings() {
  return state_.SpaceMappings();
}

bool PhiSyncSpaceAccess::IsImporting(const std::string&) {
  return false;  // Windows imports have no per-Space lock yet
}

namespace {

constexpr int kMaxAutoCreatesPerRound = 3;
// An unanswered profile creation blocks another one for its uuid this long.
constexpr auto kCreationGiveUp = std::chrono::minutes(10);

std::string LowerAscii(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

}  // namespace

// ensureLocalProfilesForAccount: an account profile this device has no
// mapping for gets a local profile — an unmapped same-named one first, else
// a new one — adopted under its uuid, at most three per round.
sync::ProfileRefreshOutcome PhiSyncSpaceAccess::RefreshAccountProfiles() {
  if (!profile_hooks_.local_profiles || !profile_hooks_.create_profile) {
    return sync::ProfileRefreshOutcome::kSkipped;
  }
  std::set<std::string> account;
  if (!profiles_.AccountProfileUuids(&account).ok()) {
    return sync::ProfileRefreshOutcome::kFailed;
  }
  std::set<std::string> mapped;
  std::set<std::string> mapped_ids;
  for (const auto& [profile_id, uuid] : profiles_.AllMappings()) {
    mapped.insert(uuid);
    mapped_ids.insert(profile_id);
  }
  std::vector<std::string> missing;
  for (const std::string& uuid : account) {  // uuid order: device-independent
    if (!mapped.count(uuid)) {
      missing.push_back(uuid);
    }
  }
  if (missing.empty()) {
    return sync::ProfileRefreshOutcome::kUnchanged;
  }
  auto locals = std::make_shared<std::vector<keys::LocalProfile>>();
  std::function<std::vector<keys::LocalProfile>()> list = profile_hooks_.local_profiles;
  if (!run_on_ui_([locals, list] { *locals = list(); })) {
    return sync::ProfileRefreshOutcome::kFailed;
  }
  int created = 0;
  for (const std::string& uuid : missing) {
    if (created >= kMaxAutoCreatesPerRound) {
      break;
    }
    // A creation an earlier round stopped waiting for: its answer is the
    // profile to reuse; while it may still land, nothing more is created.
    if (const auto it = creating_.find(uuid); it != creating_.end()) {
      bool answered = false;
      std::string answered_id;
      {
        std::lock_guard<std::mutex> hold(it->second->lock);
        answered = it->second->answered;
        answered_id = it->second->profile_id;
      }
      if (!answered &&
          std::chrono::steady_clock::now() - it->second->started <
              kCreationGiveUp) {
        continue;
      }
      creating_.erase(it);
      if (!answered_id.empty()) {
        pending_created_[uuid] = answered_id;
      }
    }
    keys::RemoteProfile remote;
    if (!profiles_.Remote(uuid, &remote).ok() || !remote.name) {
      continue;  // unreadable envelope: nothing it could sync
    }
    keys::ProfileKeyRecord record;
    // A profile an earlier round created for this uuid outranks a name twin.
    std::optional<std::string> reuse;
    if (const auto it = pending_created_.find(uuid); it != pending_created_.end()) {
      const bool exists = std::any_of(locals->begin(), locals->end(),
                                      [&](const keys::LocalProfile& p) {
                                        return p.profile_id == it->second;
                                      });
      if (exists && !mapped_ids.count(it->second)) {
        reuse = it->second;
      } else {
        pending_created_.erase(it);
      }
    }
    if (!reuse) {
      const auto twin = std::find_if(
          locals->begin(), locals->end(), [&](const keys::LocalProfile& p) {
            return !mapped_ids.count(p.profile_id) &&
                   LowerAscii(p.display_name) == LowerAscii(*remote.name);
          });
      if (twin != locals->end()) {
        // The default-profile rule rides along: a twin that is this device's
        // Default refuses a known non-default (kDefaultMismatch).
        if (profiles_
                .AdoptRemoteProfile(uuid, twin->profile_id,
                                    twin->profile_id == "Default", &record)
                .ok()) {
          mapped_ids.insert(twin->profile_id);
          ++created;
        }
        continue;
      }
    }
    std::string profile_id;
    if (reuse) {
      profile_id = *reuse;
    } else {
      // A unique name ("Work", "Work 2", …) among the local profiles.
      std::string name = *remote.name;
      for (int n = 2; std::any_of(locals->begin(), locals->end(),
                                  [&](const keys::LocalProfile& p) {
                                    return LowerAscii(p.display_name) ==
                                           LowerAscii(name);
                                  });
           ++n) {
        name = *remote.name + " " + std::to_string(n);
      }
      // The creation is asynchronous on the UI thread: wait for its answer.
      // A wait that timed out may still create the profile, so it is
      // remembered rather than retried (a failed hop never runs).
      auto wait = std::make_shared<Creation>();
      wait->started = std::chrono::steady_clock::now();
      auto create = profile_hooks_.create_profile;
      if (!run_on_ui_([create, name, wait] {
            create(name, [wait](const std::string& id) {
              std::lock_guard<std::mutex> hold(wait->lock);
              wait->answered = true;
              wait->profile_id = id;
              wait->done.notify_all();
            });
          })) {
        continue;
      }
      std::unique_lock<std::mutex> hold(wait->lock);
      if (!wait->done.wait_for(hold, std::chrono::seconds(60),
                               [&wait] { return wait->answered; })) {
        creating_[uuid] = wait;
        continue;
      }
      profile_id = wait->profile_id;
      if (profile_id.empty()) {
        continue;
      }
      pending_created_[uuid] = profile_id;
      locals->push_back({profile_id, name});
    }
    // Another path may have claimed the uuid meanwhile: never two locals
    // on one uuid.
    if (profiles_.LocalProfileIdFor(uuid)) {
      continue;
    }
    if (profiles_
            .AdoptRemoteProfile(uuid, profile_id, profile_id == "Default",
                                &record)
            .ok()) {
      pending_created_.erase(uuid);
      mapped_ids.insert(profile_id);
      ++created;
    }
  }
  if (created > 0 && profile_hooks_.mappings_changed) {
    profile_hooks_.mappings_changed();  // the new passphrases reach Chromium
  }
  return created > 0 ? sync::ProfileRefreshOutcome::kChanged
                     : sync::ProfileRefreshOutcome::kUnchanged;
}

// --- Writes -----------------------------------------------------------------------------

bool PhiSyncSpaceAccess::Create(const sync::LocalSpace& space) {
  PhiSpaceManager::SyncRow row;
  row.id = space.space_id;
  row.profile_id = space.profile_id;
  row.name = space.name;
  row.color_hex = space.color_hex;
  row.icon = space.icon_name;
  row.theme_id = space.theme_id.value_or(std::string());
  row.opacity_light = space.opacity_light;
  row.opacity_dark = space.opacity_dark;
  row.created_ms = space.created_ms;
  return Write([row](PhiSpaceManager& manager) {
    return manager.SyncCreate(row);
  });
}

bool PhiSyncSpaceAccess::Update(const std::string& space_id,
                                const std::optional<std::string>& name,
                                const std::optional<std::string>& color_hex,
                                const std::optional<std::string>& icon_name,
                                std::optional<int64_t> created_ms) {
  return Write([=](PhiSpaceManager& manager) {
    return manager.SyncUpdate(space_id, name, color_hex, icon_name,
                              created_ms);
  });
}

bool PhiSyncSpaceAccess::Rebind(const std::string& space_id,
                                const std::string& profile_id) {
  return Write([=](PhiSpaceManager& manager) {
    return manager.SyncRebind(space_id, profile_id);
  });
}

bool PhiSyncSpaceAccess::ApplyThemeState(
    const std::string& space_id,
    const std::optional<std::string>& theme_id,
    std::optional<double> opacity_light,
    std::optional<double> opacity_dark) {
  return Write([=](PhiSpaceManager& manager) {
    return manager.SyncThemeState(space_id, theme_id, opacity_light,
                                  opacity_dark);
  });
}

bool PhiSyncSpaceAccess::ApplyOrder(
    const std::vector<std::string>& ordered_space_ids) {
  return Write([ordered_space_ids](PhiSpaceManager& manager) {
    return manager.SyncOrder(ordered_space_ids);
  });
}

bool PhiSyncSpaceAccess::Hide(const std::string& space_id) {
  return Write([space_id](PhiSpaceManager& manager) {
    return manager.SyncHide(space_id);
  });
}

bool PhiSyncSpaceAccess::Purge(const std::string& space_id) {
  return Write([space_id](PhiSpaceManager& manager) {
    return manager.SyncPurge(space_id);
  });
}

}  // namespace phi_client

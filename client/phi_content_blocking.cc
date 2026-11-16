// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_content_blocking.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "client/phi_file_io.h"

namespace phi_client {

namespace {

using Category = PhiContentBlockingCategory;

// The mirror's per-profile keys (the Mac's ContentBlockingAnalytics).
constexpr char kMirrorAds[] = "ads";
constexpr char kMirrorCookieBanners[] = "cookie_banners";
constexpr char kMirrorTrackers[] = "trackers";

bool InSections(Category category, const std::string& list_category) {
  switch (category) {
    case Category::kAds:
      return list_category == "ads" || list_category == "regional";
    case Category::kTrackers:
      return list_category == "trackers";
    case Category::kCookieBanners:
      return list_category == "cookies";
  }
  return false;
}

phi_framework::PhiFramework::ContentBlockingCategory ToFramework(
    Category category) {
  using Framework = phi_framework::PhiFramework::ContentBlockingCategory;
  switch (category) {
    case Category::kAds:
      return Framework::kAds;
    case Category::kCookieBanners:
      return Framework::kCookieBanners;
    case Category::kTrackers:
      return Framework::kTrackers;
  }
  return Framework::kAds;
}

std::string StringOr(const base::DictValue& dict, const char* key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

}  // namespace

bool PhiContentBlockingState::Switch(Category category) const {
  switch (category) {
    case Category::kAds:
      return block_ads;
    case Category::kCookieBanners:
      return block_cookie_banners;
    case Category::kTrackers:
      return block_trackers;
  }
  return false;
}

void PhiContentBlockingState::SetSwitch(Category category, bool on) {
  switch (category) {
    case Category::kAds:
      block_ads = on;
      break;
    case Category::kCookieBanners:
      block_cookie_banners = on;
      break;
    case Category::kTrackers:
      block_trackers = on;
      break;
  }
}

PhiContentBlockingList* PhiContentBlockingState::Find(const std::string& id) {
  for (PhiContentBlockingList& list : lists) {
    if (list.id == id) {
      return &list;
    }
  }
  return nullptr;
}

bool PhiParseContentBlockingState(const std::string& json,
                                  PhiContentBlockingState* out) {
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!dict) {
    return false;
  }
  PhiContentBlockingState state;
  state.block_ads = dict->FindBool("blockAds").value_or(false);
  state.block_cookie_banners =
      dict->FindBool("blockCookieBanners").value_or(false);
  state.block_trackers = dict->FindBool("blockTrackers").value_or(false);
  state.status = StringOr(*dict, "status");
  if (state.status.empty()) {
    state.status = "disabled";
  }
  state.status_detail = StringOr(*dict, "statusDetail");
  state.session_blocked_count =
      dict->FindDouble("sessionBlockedCount").value_or(0);
  state.last_build_log = StringOr(*dict, "lastBuildLog");
  if (const base::ListValue* exceptions = dict->FindList("siteExceptions")) {
    for (const base::Value& domain : *exceptions) {
      if (domain.is_string()) {
        state.site_exceptions.push_back(domain.GetString());
      }
    }
  }
  if (const base::ListValue* lists = dict->FindList("lists")) {
    for (const base::Value& value : *lists) {
      if (!value.is_dict()) {
        continue;
      }
      const base::DictValue& item = value.GetDict();
      PhiContentBlockingList list;
      list.id = StringOr(item, "id");
      if (list.id.empty()) {
        continue;
      }
      list.category = StringOr(item, "category");
      list.name = StringOr(item, "name");
      list.custom = item.FindBool("custom").value_or(false);
      list.homepage = StringOr(item, "homepage");
      list.license = StringOr(item, "license");
      list.source_url = StringOr(item, "sourceUrl");
      list.checked = item.FindBool("checked").value_or(false);
      list.default_checked = item.FindBool("defaultChecked").value_or(false);
      list.available = item.FindBool("available").value_or(false);
      list.downloading = item.FindBool("downloading").value_or(false);
      list.downloaded_bytes =
          static_cast<int64_t>(item.FindDouble("downloadedBytes").value_or(0));
      list.total_bytes =
          static_cast<int64_t>(item.FindDouble("totalBytes").value_or(-1));
      list.fetched_at_ms = item.FindDouble("fetchedAt").value_or(0);
      list.last_error = StringOr(item, "lastError");
      state.lists.push_back(std::move(list));
    }
  }
  *out = std::move(state);
  return true;
}

std::vector<const PhiContentBlockingList*> PhiContentBlockingListsFor(
    Category category,
    const PhiContentBlockingState& state) {
  std::vector<const PhiContentBlockingList*> out;
  for (const PhiContentBlockingList& list : state.lists) {
    if (!list.custom && InSections(category, list.category)) {
      out.push_back(&list);
    }
  }
  return out;
}

std::vector<const PhiContentBlockingList*> PhiContentBlockingCustomLists(
    const PhiContentBlockingState& state) {
  std::vector<const PhiContentBlockingList*> out;
  for (const PhiContentBlockingList& list : state.lists) {
    if (list.custom) {
      out.push_back(&list);
    }
  }
  return out;
}

std::vector<const PhiContentBlockingList*> PhiContentBlockingShownLists(
    Category category,
    const PhiContentBlockingState& state) {
  std::vector<const PhiContentBlockingList*> out =
      PhiContentBlockingListsFor(category, state);
  for (const PhiContentBlockingList* list :
       PhiContentBlockingCustomLists(state)) {
    out.push_back(list);
  }
  return out;
}

bool PhiContentBlockingNeedsDownload(Category category,
                                     const PhiContentBlockingState& state) {
  for (const PhiContentBlockingList* list :
       PhiContentBlockingShownLists(category, state)) {
    if (list->checked && list->available) {
      return false;
    }
  }
  return true;
}

std::vector<Category> PhiContentBlockingTogglesToTurnOff(
    const PhiContentBlockingState& state) {
  std::vector<Category> off;
  for (Category category :
       {Category::kAds, Category::kCookieBanners, Category::kTrackers}) {
    if (state.Switch(category) &&
        PhiContentBlockingNeedsDownload(category, state)) {
      off.push_back(category);
    }
  }
  return off;
}

std::vector<std::string> PhiContentBlockingMissingIds(
    Category category,
    const PhiContentBlockingState& state) {
  std::vector<std::string> out;
  for (const PhiContentBlockingList* list :
       PhiContentBlockingShownLists(category, state)) {
    if (list->checked && !list->available && !list->downloading) {
      out.push_back(list->id);
    }
  }
  return out;
}

bool PhiContentBlockingAnyDownloading(Category category,
                                      const PhiContentBlockingState& state) {
  for (const PhiContentBlockingList* list :
       PhiContentBlockingShownLists(category, state)) {
    if (list->downloading) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> PhiContentBlockingDownloadedIds(
    Category category,
    const PhiContentBlockingState& state) {
  std::vector<std::string> out;
  for (const PhiContentBlockingList* list :
       PhiContentBlockingShownLists(category, state)) {
    if (list->available && !list->downloading &&
        (!list->custom || !list->source_url.empty())) {
      out.push_back(list->id);
    }
  }
  return out;
}

PhiContentBlocking::PhiContentBlocking() = default;
PhiContentBlocking::~PhiContentBlocking() = default;

void PhiContentBlocking::Init(phi_framework::PhiFramework* framework,
                              const std::wstring& user_data_dir) {
  framework_ = framework;
  if (user_data_dir.empty() || !mirror_path_.empty()) {
    return;
  }
  mirror_path_ = user_data_dir + L"\\PhiContentBlockingMetrics.json";
  std::string bytes;
  if (PhiReadFile(mirror_path_, &bytes) != PhiReadStatus::kOk) {
    return;
  }
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(bytes, base::JSON_PARSE_RFC);
  const base::DictValue* profiles = dict ? dict->FindDict("profiles") : nullptr;
  if (!profiles) {
    return;
  }
  for (const auto [profile_id, value] : *profiles) {
    if (!value.is_dict()) {
      continue;
    }
    std::map<std::string, bool>& entry = mirror_[profile_id];
    for (const char* key : {kMirrorAds, kMirrorCookieBanners, kMirrorTrackers}) {
      entry[key] = value.GetDict().FindBool(key).value_or(false);
    }
  }
}

void PhiContentBlocking::AddObserver(PhiContentBlockingObserver* observer) {
  if (std::find(observers_.begin(), observers_.end(), observer) ==
      observers_.end()) {
    observers_.push_back(observer);
  }
}

void PhiContentBlocking::RemoveObserver(PhiContentBlockingObserver* observer) {
  observers_.erase(std::remove(observers_.begin(), observers_.end(), observer),
                   observers_.end());
}

const PhiContentBlockingState* PhiContentBlocking::State(
    const std::string& profile_id) const {
  auto it = states_.find(profile_id);
  return it == states_.end() ? nullptr : &it->second;
}

bool PhiContentBlocking::Unavailable(const std::string& profile_id) const {
  return unavailable_.count(profile_id) > 0;
}

void PhiContentBlocking::Refresh(const std::string& profile_id) {
  if (!framework_ || profile_id.empty()) {
    return;
  }
  if (reading_.count(profile_id)) {
    // The read in flight may predate whatever asked; read once more after.
    reread_.insert(profile_id);
    return;
  }
  const int64_t request_id =
      framework_->RequestContentBlockingSettings(profile_id.c_str());
  if (request_id == 0) {
    unavailable_.insert(profile_id);
    Notify(profile_id);
    return;
  }
  reading_.insert(profile_id);
  Pending pending;
  pending.profile_id = profile_id;
  pending.kind = Kind::kRead;
  Track(request_id, std::move(pending));
}

void PhiContentBlocking::SetCategory(const std::string& profile_id,
                                     Category category,
                                     bool enabled,
                                     Done done) {
  Pending pending;
  pending.profile_id = profile_id;
  pending.done = std::move(done);
  if (!framework_ ||
      !Edit(profile_id,
            [&](PhiContentBlockingState* state) {
              state->SetSwitch(category, enabled);
            },
            &pending.previous)) {
    if (pending.done) {
      pending.done(false, kUnavailable);
    }
    return;
  }
  Track(framework_->SetContentBlockingCategory(
            profile_id.c_str(), ToFramework(category), enabled),
        std::move(pending));
}

void PhiContentBlocking::SetList(const std::string& profile_id,
                                 const std::string& list_id,
                                 bool checked,
                                 Done done) {
  Pending pending;
  pending.profile_id = profile_id;
  pending.done = std::move(done);
  bool known = false;
  if (!framework_ ||
      !Edit(profile_id,
            [&](PhiContentBlockingState* state) {
              if (PhiContentBlockingList* list = state->Find(list_id)) {
                list->checked = checked;
                known = true;
              }
            },
            &pending.previous) ||
      !known) {
    if (pending.done) {
      pending.done(false, kUnavailable);
    }
    return;
  }
  Track(framework_->SetContentBlockingList(profile_id.c_str(), list_id.c_str(),
                                           checked),
        std::move(pending));
}

void PhiContentBlocking::AddCustomList(const std::string& profile_id,
                                       const std::string& name,
                                       const std::string& url,
                                       const std::string& rules,
                                       Done done) {
  Pending pending;
  pending.profile_id = profile_id;
  pending.kind = Kind::kAdd;
  pending.done = std::move(done);
  if (!framework_) {
    if (pending.done) {
      pending.done(false, kUnavailable);
    }
    return;
  }
  Track(framework_->AddContentBlockingCustomList(
            profile_id.c_str(), name.c_str(), url.c_str(), rules.c_str()),
        std::move(pending));
}

void PhiContentBlocking::RemoveCustomList(const std::string& profile_id,
                                          const std::string& list_id,
                                          Done done) {
  Pending pending;
  pending.profile_id = profile_id;
  pending.done = std::move(done);
  if (!framework_ ||
      !Edit(profile_id,
            [&](PhiContentBlockingState* state) {
              state->lists.erase(
                  std::remove_if(state->lists.begin(), state->lists.end(),
                                 [&](const PhiContentBlockingList& list) {
                                   return list.custom && list.id == list_id;
                                 }),
                  state->lists.end());
            },
            &pending.previous)) {
    if (pending.done) {
      pending.done(false, kUnavailable);
    }
    return;
  }
  Track(framework_->RemoveContentBlockingCustomList(profile_id.c_str(),
                                                    list_id.c_str()),
        std::move(pending));
}

void PhiContentBlocking::DownloadLists(const std::string& profile_id,
                                       const std::vector<std::string>& list_ids,
                                       Done done) {
  Pending pending;
  pending.profile_id = profile_id;
  pending.done = std::move(done);
  base::ListValue ids;
  for (const std::string& id : list_ids) {
    ids.Append(id);
  }
  std::string ids_json;
  if (!framework_ || list_ids.empty() ||
      !base::JSONWriter::Write(ids, &ids_json) ||
      !Edit(profile_id,
            [&](PhiContentBlockingState* state) {
              for (const std::string& id : list_ids) {
                if (PhiContentBlockingList* list = state->Find(id)) {
                  list->downloading = true;
                  list->last_error.clear();
                }
              }
            },
            &pending.previous)) {
    if (pending.done) {
      pending.done(false, kUnavailable);
    }
    return;
  }
  Track(framework_->DownloadContentBlockingLists(profile_id.c_str(),
                                                 ids_json.c_str()),
        std::move(pending));
}

void PhiContentBlocking::RefreshLists(const std::string& profile_id,
                                      Done done) {
  Pending pending;
  pending.profile_id = profile_id;
  pending.done = std::move(done);
  if (!framework_) {
    if (pending.done) {
      pending.done(false, kUnavailable);
    }
    return;
  }
  Track(framework_->RefreshContentBlockingLists(profile_id.c_str()),
        std::move(pending));
}

void PhiContentBlocking::Track(int64_t request_id, Pending pending) {
  if (request_id == 0) {
    // Nothing started: undo the optimistic edit and say so.
    if (pending.kind == Kind::kRead) {
      reading_.erase(pending.profile_id);
    }
    if (pending.previous) {
      states_[pending.profile_id] = std::move(*pending.previous);
      Notify(pending.profile_id);
    }
    if (pending.done) {
      pending.done(false, kUnavailable);
    }
    return;
  }
  pending_[request_id] = std::move(pending);
}

bool PhiContentBlocking::Edit(
    const std::string& profile_id,
    const std::function<void(PhiContentBlockingState*)>& edit,
    std::optional<PhiContentBlockingState>* previous) {
  auto it = states_.find(profile_id);
  if (it == states_.end()) {
    return false;  // Nothing read yet: the pane offers no control.
  }
  *previous = it->second;
  edit(&it->second);
  Mirror(profile_id, it->second);
  Notify(profile_id);
  return true;
}

void PhiContentBlocking::Notify(const std::string& profile_id) {
  // A copy: an observer may remove itself (a window closing on a reply).
  const std::vector<PhiContentBlockingObserver*> observers = observers_;
  for (PhiContentBlockingObserver* observer : observers) {
    if (std::find(observers_.begin(), observers_.end(), observer) !=
        observers_.end()) {
      observer->OnContentBlockingChanged(profile_id);
    }
  }
}

void PhiContentBlocking::OnReply(int64_t request_id,
                                 bool success,
                                 const char* payload) {
  auto it = pending_.find(request_id);
  if (it == pending_.end()) {
    return;
  }
  Pending pending = std::move(it->second);
  pending_.erase(it);
  const std::string text = payload ? payload : "";
  const std::string& profile_id = pending.profile_id;
  if (pending.kind == Kind::kRead) {
    reading_.erase(profile_id);
    const bool stale = reread_.erase(profile_id) > 0 || WritePending(profile_id);
    if (stale && success) {
      // A write went out after this read started: its answer re-reads, and
      // applying this one would flick the switch back meanwhile.
      Refresh(profile_id);
      return;
    }
    PhiContentBlockingState state;
    if (success && PhiParseContentBlockingState(text, &state)) {
      unavailable_.erase(profile_id);
      Mirror(profile_id, state);
      states_[profile_id] = std::move(state);
    } else {
      LOG(WARNING) << "[ContentBlocking] read failed for a profile: " << text;
      unavailable_.insert(profile_id);
    }
    Notify(profile_id);
    if (pending.done) {
      pending.done(success, text);
    }
    return;
  }
  if (!success) {
    LOG(WARNING) << "[ContentBlocking] write refused: " << text;
    if (pending.previous) {
      Mirror(profile_id, *pending.previous);
      states_[profile_id] = std::move(*pending.previous);
      Notify(profile_id);
    }
  }
  // Accepted or not, re-read: the framework's state is the truth, and an
  // add has no row until it is read back.
  Refresh(profile_id);
  if (pending.done) {
    pending.done(success, text);
  }
}

bool PhiContentBlocking::WritePending(const std::string& profile_id) const {
  for (const auto& [request_id, pending] : pending_) {
    if (pending.kind != Kind::kRead && pending.profile_id == profile_id) {
      return true;
    }
  }
  return false;
}

void PhiContentBlocking::OnFrameworkChanged(const char* profile_id) {
  if (profile_id && *profile_id) {
    Refresh(profile_id);
  }
}

void PhiContentBlocking::Mirror(const std::string& profile_id,
                                const PhiContentBlockingState& state) {
  std::map<std::string, bool> entry = {
      {kMirrorAds, state.block_ads},
      {kMirrorCookieBanners, state.block_cookie_banners},
      {kMirrorTrackers, state.block_trackers},
  };
  auto it = mirror_.find(profile_id);
  if (it != mirror_.end() && it->second == entry) {
    return;
  }
  mirror_[profile_id] = std::move(entry);
  std::set<std::string> known =
      known_profiles_ ? known_profiles_() : std::set<std::string>();
  if (!known.empty()) {  // Empty: the profile list is not in yet.
    known.insert(profile_id);
    for (auto stale = mirror_.begin(); stale != mirror_.end();) {
      stale = known.count(stale->first) ? std::next(stale)
                                        : mirror_.erase(stale);
    }
  }
  SaveMirror();
}

void PhiContentBlocking::SetKnownProfiles(
    std::function<std::set<std::string>()> known) {
  known_profiles_ = std::move(known);
}

void PhiContentBlocking::PruneProfiles(
    const std::set<std::string>& known_profile_ids) {
  if (known_profile_ids.empty()) {
    return;  // The profile list is not in yet; prune nothing.
  }
  bool changed = false;
  for (auto it = mirror_.begin(); it != mirror_.end();) {
    if (known_profile_ids.count(it->first)) {
      ++it;
    } else {
      it = mirror_.erase(it);
      changed = true;
    }
  }
  if (changed) {
    SaveMirror();
  }
}

void PhiContentBlocking::SaveMirror() const {
  if (mirror_path_.empty()) {
    return;
  }
  base::DictValue profiles;
  for (const auto& [profile_id, entry] : mirror_) {
    base::DictValue switches;
    for (const auto& [key, on] : entry) {
      switches.Set(key, on);
    }
    profiles.Set(profile_id, std::move(switches));
  }
  base::DictValue dict;
  dict.Set("profiles", std::move(profiles));
  std::string bytes;
  if (base::JSONWriter::Write(dict, &bytes)) {
    PhiWriteFileAtomic(mirror_path_, bytes);
  }
}

void PhiContentBlocking::AddSnapshotProperties(
    base::DictValue* properties) const {
  const auto any_on = [this](const char* key) {
    for (const auto& [profile_id, entry] : mirror_) {
      auto it = entry.find(key);
      if (it != entry.end() && it->second) {
        return true;
      }
    }
    return false;
  };
  properties->Set("block_ads_enabled", any_on(kMirrorAds));
  properties->Set("block_cookie_banners_enabled", any_on(kMirrorCookieBanners));
  properties->Set("block_trackers_enabled", any_on(kMirrorTrackers));
}

}  // namespace phi_client

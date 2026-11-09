// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_guest_migration.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <tuple>
#include <utility>

#include "client/phi_url_rule_normalize.h"

namespace phi_client::store {

namespace {

std::string Lower(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

bool IsBookmarkKind(const Tab& tab) {
  return tab.is_bookmark_kind();
}

// pinnedTab(_:belongsTo:).
bool PinBelongsTo(const Tab& tab, PinScope scope) {
  switch (scope) {
    case PinScope::kSpace:
      return tab.space_id.has_value();
    case PinScope::kProfile:
      return tab.profile_id.has_value() && !tab.space_id.has_value();
    case PinScope::kApp:
      break;
  }
  return !tab.profile_id.has_value() && !tab.space_id.has_value();
}

// The active pins of `scope`, plus Space scope's dormant profile rows.
bool PinIsActive(const Tab& tab, PinScope scope) {
  return PinBelongsTo(tab, scope) ||
         (scope == PinScope::kSpace && tab.pin_dormant &&
          PinBelongsTo(tab, PinScope::kProfile));
}

// guestMigrationIdentifier.
std::string MigrationIdentifier(const std::string& operation_id,
                                const std::string& kind,
                                int index,
                                const std::set<std::string>& used) {
  for (int suffix = 0;; ++suffix) {
    std::string candidate = "guest-import-" + operation_id + "-" + kind + "-" +
                            std::to_string(index);
    if (suffix) {
      candidate += "-" + std::to_string(suffix);
    }
    if (!used.count(candidate)) {
      return candidate;
    }
  }
}

// guestRuleSignature.
std::string RuleSignature(const std::string& space_id,
                          const std::string& host,
                          const std::optional<std::string>& path_prefix) {
  return space_id + std::string(1, '\0') + Lower(host) + std::string(1, '\0') +
         NormalizedRulePathPrefix(path_prefix).value_or("");
}

struct PinOwner {
  std::optional<std::string> profile_id;
  std::optional<std::string> space_id;
  std::string SortKey() const {
    return profile_id.value_or("") + std::string(1, '\0') +
           space_id.value_or("");
  }
  bool operator<(const PinOwner& o) const { return SortKey() < o.SortKey(); }
  bool operator==(const PinOwner&) const = default;
};

// guestPinnedContentSignature + guestPinnedVariantSignature, flattened.
using ContentSignature = std::tuple<std::string, std::string, std::string>;
struct VariantKey {
  std::string lineage;
  ContentSignature content;
  std::optional<std::string> partner_lineage;
  std::optional<ContentSignature> partner_content;
  bool operator<(const VariantKey& o) const {
    return std::tie(lineage, content, partner_lineage, partner_content) <
           std::tie(o.lineage, o.content, o.partner_lineage,
                    o.partner_content);
  }
};

ContentSignature ContentOf(const Tab& tab) {
  return {tab.title, tab.url, tab.favicon};
}

std::string LineageOf(const Tab& tab) {
  return tab.pin_lineage_id.value_or(tab.guid);
}

VariantKey VariantOf(const Tab& tab,
                     const std::map<std::string, const Tab*>& rows_by_guid) {
  VariantKey key;
  key.lineage = LineageOf(tab);
  key.content = ContentOf(tab);
  if (tab.split_partner_guid) {
    const auto it = rows_by_guid.find(*tab.split_partner_guid);
    if (it != rows_by_guid.end()) {
      key.partner_lineage = LineageOf(*it->second);
      key.partner_content = ContentOf(*it->second);
    }
  }
  return key;
}

bool PinSourceLess(const Tab& a, const Tab& b) {
  return std::make_tuple(a.profile_id.value_or(""), a.space_id.value_or(""),
                         a.index, a.guid) <
         std::make_tuple(b.profile_id.value_or(""), b.space_id.value_or(""),
                         b.index, b.guid);
}

// guestTargetOwners.
std::vector<PinOwner> TargetOwners(
    const Tab& source,
    PinScope source_scope,
    PinScope target_scope,
    const std::map<std::string, std::string>& profile_ids,
    const std::map<std::string, std::string>& space_ids,
    const std::set<std::string>& target_profile_ids,
    const std::map<std::string, std::string>& target_space_profile_ids) {
  auto mapped_space_owner =
      [&](bool with_space) -> std::optional<PinOwner> {
    if (!source.space_id) {
      return std::nullopt;
    }
    const auto space = space_ids.find(*source.space_id);
    if (space == space_ids.end()) {
      return std::nullopt;
    }
    const auto profile = target_space_profile_ids.find(space->second);
    if (profile == target_space_profile_ids.end()) {
      return std::nullopt;
    }
    return PinOwner{profile->second,
                    with_space ? std::optional<std::string>(space->second)
                               : std::nullopt};
  };
  const std::string source_profile =
      source.profile_id.value_or(kDefaultProfileId);
  const auto mapped_profile = profile_ids.find(source_profile);
  const std::string target_profile = mapped_profile != profile_ids.end()
                                         ? mapped_profile->second
                                         : source_profile;
  switch (target_scope) {
    case PinScope::kApp:
      return {PinOwner{}};
    case PinScope::kProfile: {
      if (source_scope == PinScope::kApp && !source.pin_dormant) {
        std::vector<PinOwner> out;
        for (const std::string& profile : target_profile_ids) {
          out.push_back(PinOwner{profile, std::nullopt});
        }
        return out;
      }
      if (auto owner = mapped_space_owner(/*with_space=*/false)) {
        return {*owner};
      }
      return {PinOwner{target_profile, std::nullopt}};
    }
    case PinScope::kSpace: {
      if (source_scope == PinScope::kApp && !source.pin_dormant) {
        std::vector<PinOwner> out;
        for (const auto& [space, profile] : target_space_profile_ids) {
          out.push_back(PinOwner{profile, space});
        }
        return out;
      }
      if (auto owner = mapped_space_owner(/*with_space=*/true)) {
        return {*owner};
      }
      std::vector<PinOwner> out;
      for (const auto& [space, profile] : target_space_profile_ids) {
        if (profile == target_profile) {
          out.push_back(PinOwner{target_profile, space});
        }
      }
      if (out.empty()) {
        out.push_back(PinOwner{target_profile, std::nullopt});
      }
      return out;
    }
  }
  return {};
}

// planGuestPinnedTargets.
std::vector<GuestPinTarget> PlanPinTargets(
    const std::vector<Tab>& source,
    PinScope source_scope,
    const std::vector<const Tab*>& target,
    PinScope target_scope,
    const std::map<std::string, std::string>& profile_ids,
    const std::map<std::string, std::string>& space_ids,
    const std::set<std::string>& target_profile_ids,
    const std::map<std::string, std::string>& target_space_profile_ids,
    const std::string& operation_id,
    std::set<std::string>* used_model_guids) {
  std::map<std::string, const Tab*> source_by_guid;
  for (const Tab& row : source) {
    source_by_guid[row.guid] = &row;
  }
  std::map<std::string, const Tab*> target_by_guid;
  for (const Tab* row : target) {
    target_by_guid.emplace(row->guid, row);
  }
  std::map<PinOwner, std::vector<const Tab*>> target_by_owner;
  for (const Tab* row : target) {
    if (PinIsActive(*row, target_scope)) {
      target_by_owner[PinOwner{row->profile_id, row->space_id}].push_back(row);
    }
  }
  std::map<PinOwner, std::vector<const Tab*>> projected;
  for (const Tab& row : source) {
    for (const PinOwner& owner :
         TargetOwners(row, source_scope, target_scope, profile_ids, space_ids,
                      target_profile_ids, target_space_profile_ids)) {
      projected[owner].push_back(&row);
    }
  }
  std::vector<GuestPinTarget> result;
  std::map<std::pair<std::string, std::string>, size_t> index_by_projected;
  int generated = 0;
  for (auto& [owner, source_rows] : projected) {
    std::vector<const Tab*> existing = target_by_owner[owner];
    std::sort(existing.begin(), existing.end(),
              [](const Tab* a, const Tab* b) {
                return std::tie(a->index, a->guid) <
                       std::tie(b->index, b->guid);
              });
    std::map<VariantKey, std::pair<std::string, int>> canonical;
    int next_index = 0;
    for (const Tab* row : existing) {
      canonical.emplace(VariantOf(*row, target_by_guid),
                        std::make_pair(row->guid, row->index));
      next_index = std::max(next_index, row->index + 1);
    }
    std::sort(source_rows.begin(), source_rows.end(),
              [](const Tab* a, const Tab* b) { return PinSourceLess(*a, *b); });
    for (const Tab* row : source_rows) {
      const VariantKey key = VariantOf(*row, source_by_guid);
      GuestPinTarget mapping;
      mapping.source_guid = row->guid;
      mapping.target_profile_id = owner.profile_id;
      mapping.target_space_id = owner.space_id;
      mapping.dormant = target_scope == PinScope::kSpace && !owner.space_id;
      const auto found = canonical.find(key);
      if (found != canonical.end()) {
        mapping.target_guid = found->second.first;
        mapping.target_index = found->second.second;
        mapping.should_insert = false;
      } else {
        std::string guid = row->guid;
        if (used_model_guids->count(guid)) {
          guid = MigrationIdentifier(operation_id, "pin", generated,
                                     *used_model_guids);
        }
        ++generated;
        used_model_guids->insert(guid);
        canonical[key] = {guid, next_index};
        mapping.target_guid = guid;
        mapping.target_index = next_index++;
        mapping.should_insert = true;
      }
      index_by_projected[{row->guid, owner.SortKey()}] = result.size();
      result.push_back(std::move(mapping));
    }
  }
  for (GuestPinTarget& mapping : result) {
    const auto source_row = source_by_guid.find(mapping.source_guid);
    if (source_row == source_by_guid.end() ||
        !source_row->second->split_partner_guid) {
      continue;
    }
    const PinOwner owner{mapping.target_profile_id, mapping.target_space_id};
    const auto partner = index_by_projected.find(
        {*source_row->second->split_partner_guid, owner.SortKey()});
    if (partner != index_by_projected.end()) {
      mapping.target_split_partner_guid = result[partner->second].target_guid;
    }
  }
  return result;
}

}  // namespace

// --- Values --------------------------------------------------------------------

GuestThemes GuestThemes::Retaining(const std::set<std::string>& space_ids) const {
  GuestThemes out;
  for (const auto& [id, value] : theme_ids) {
    if (space_ids.count(id)) {
      out.theme_ids[id] = value;
    }
  }
  for (const auto& [id, value] : overlay_opacities) {
    if (space_ids.count(id)) {
      out.overlay_opacities[id] = value;
    }
  }
  for (const auto& [id, value] : saturations) {
    if (space_ids.count(id)) {
      out.saturations[id] = value;
    }
  }
  return out;
}

bool GuestSnapshot::SameContent(const GuestSnapshot& other) const {
  return source_user_id == other.source_user_id &&
         source_scope == other.source_scope && profiles == other.profiles &&
         spaces == other.spaces && bookmarks == other.bookmarks &&
         pins == other.pins && rules == other.rules && themes == other.themes;
}

std::map<std::string, std::string> GuestMappings::ModelGuids() const {
  std::map<std::string, std::string> out = bookmark_guids;
  for (const GuestPinTarget& pin : pin_targets) {
    out.emplace(pin.source_guid, pin.target_guid);
  }
  return out;
}

std::set<std::string> GuestMappings::NewModelGuids() const {
  std::set<std::string> out;
  for (const auto& [source, target] : bookmark_guids) {
    out.insert(target);
  }
  if (default_import_folder_guid) {
    out.insert(*default_import_folder_guid);
  }
  for (const GuestPinTarget& pin : pin_targets) {
    if (pin.should_insert) {
      out.insert(pin.target_guid);
    }
  }
  return out;
}

// --- Snapshot ------------------------------------------------------------------

bool MakeGuestSnapshot(const LocalStore& source,
                       const std::string& source_user_id,
                       const GuestThemes& themes,
                       const std::string& snapshot_id,
                       TimeMs now,
                       GuestSnapshot* out,
                       std::string* error) {
  const std::vector<Space> stored_spaces = source.Spaces();
  std::set<std::string> hidden_roots;
  std::set<std::string> referenced_profiles;
  std::set<std::string> referenced_spaces;
  std::set<std::string> stored_space_ids;
  for (const auto& [id, profile] : source.profiles()) {
    referenced_profiles.insert(id);
    if (profile.bookmark_root) {
      hidden_roots.insert(*profile.bookmark_root);
    }
  }
  for (const Space& space : stored_spaces) {
    referenced_spaces.insert(space.space_id);
    stored_space_ids.insert(space.space_id);
    if (space.bookmark_root) {
      hidden_roots.insert(*space.bookmark_root);
    }
  }
  for (const auto& [guid, tab] : source.tabs()) {
    if (!IsBookmarkKind(tab) && tab.type != RowType::kPinnedTab) {
      continue;
    }
    if (tab.profile_id) {
      referenced_profiles.insert(*tab.profile_id);
    }
    if (tab.space_id) {
      referenced_spaces.insert(*tab.space_id);
    } else if (IsBookmarkKind(tab)) {
      referenced_spaces.insert(kDefaultSpaceId);
    }
  }
  const std::vector<UrlRule> live_rules = source.UrlRules(false);
  for (const UrlRule& rule : live_rules) {
    if (rule.space_id != kKioskRuleTargetId) {
      referenced_spaces.insert(rule.space_id);
    }
  }
  std::string missing;
  for (const std::string& id : referenced_spaces) {
    if (!stored_space_ids.count(id) && id != kDefaultSpaceId) {
      missing += (missing.empty() ? "" : ", ") + id;
    }
  }
  if (!missing.empty()) {
    *error = "Guest data references missing Spaces: " + missing;
    return false;
  }

  GuestSnapshot snapshot;
  snapshot.snapshot_id = snapshot_id;
  snapshot.source_user_id = source_user_id;
  snapshot.captured_at = now;
  snapshot.source_scope = source.pin_scope();
  for (const std::string& id : referenced_profiles) {
    Profile record;
    record.profile_id = id;
    if (const Profile* stored = source.FindProfile(id)) {
      record.guid = stored->guid;
      record.display_name = stored->display_name;
    } else {
      record.guid = id;
    }
    snapshot.profiles.push_back(std::move(record));
  }
  std::sort(snapshot.profiles.begin(), snapshot.profiles.end(),
            [](const Profile& a, const Profile& b) {
              return std::tie(a.profile_id, a.guid) <
                     std::tie(b.profile_id, b.guid);
            });
  for (Space space : stored_spaces) {
    space.bookmark_root.reset();
    snapshot.spaces.push_back(std::move(space));
  }
  for (const auto& [guid, tab] : source.tabs()) {
    if (!IsBookmarkKind(tab) || hidden_roots.count(guid)) {
      continue;
    }
    Tab record = tab;
    if (record.parent_guid && hidden_roots.count(*record.parent_guid)) {
      record.parent_guid.reset();
    }
    record.profile_id = tab.profile_id.value_or(kDefaultProfileId);
    record.space_id = tab.space_id.value_or(kDefaultSpaceId);
    // The account identity is the target's to mint.
    record.sync_id.reset();
    record.content_updated.reset();
    record.location_updated.reset();
    snapshot.bookmarks.push_back(std::move(record));
  }
  std::sort(snapshot.bookmarks.begin(), snapshot.bookmarks.end(),
            [](const Tab& a, const Tab& b) {
              return std::make_tuple(*a.space_id, *a.profile_id,
                                     a.parent_guid.value_or(""), a.index,
                                     a.guid) <
                     std::make_tuple(*b.space_id, *b.profile_id,
                                     b.parent_guid.value_or(""), b.index,
                                     b.guid);
            });
  for (const auto& [guid, tab] : source.tabs()) {
    if (tab.type != RowType::kPinnedTab ||
        !PinIsActive(tab, snapshot.source_scope)) {
      continue;
    }
    Tab record = tab;
    record.pin_lineage_id = LineageOf(tab);
    snapshot.pins.push_back(std::move(record));
  }
  std::sort(snapshot.pins.begin(), snapshot.pins.end(), PinSourceLess);
  for (UrlRule rule : live_rules) {
    rule.host = Lower(rule.host);
    rule.path_prefix = NormalizedRulePathPrefix(rule.path_prefix);
    rule.content_updated.reset();
    rule.target_updated.reset();
    rule.deleted.reset();
    rule.pending_local_edit = false;
    rule.merge_partner_sync_id.reset();
    snapshot.rules.push_back(std::move(rule));
  }
  std::sort(snapshot.rules.begin(), snapshot.rules.end(),
            [](const UrlRule& a, const UrlRule& b) {
              return std::tie(a.space_id, a.sort_order, a.created, a.id) <
                     std::tie(b.space_id, b.sort_order, b.created, b.id);
            });
  snapshot.themes = themes.Retaining(referenced_spaces);
  *out = std::move(snapshot);
  return true;
}

// --- Plan ----------------------------------------------------------------------

bool PlanGuestImport(const GuestSnapshot& snapshot,
                     const LocalStore& target,
                     const std::string& operation_id,
                     GuestMappings* out,
                     std::string* error) {
  const std::vector<Space> target_spaces = target.Spaces();
  const std::vector<UrlRule> target_rules = target.UrlRules(false);
  const PinScope target_scope = target.pin_scope();
  GuestMappings plan;
  plan.target_scope = target_scope;

  std::set<std::string> existing_profiles;
  for (const auto& [id, profile] : target.profiles()) {
    existing_profiles.insert(id);
  }
  std::set<std::string> source_profiles;
  for (const Profile& profile : snapshot.profiles) {
    source_profiles.insert(profile.profile_id);
  }
  for (const Space& space : snapshot.spaces) {
    source_profiles.insert(space.profile_id);
  }
  for (const Tab& row : snapshot.bookmarks) {
    source_profiles.insert(*row.profile_id);
  }
  for (const Tab& row : snapshot.pins) {
    if (row.profile_id) {
      source_profiles.insert(*row.profile_id);
    }
  }
  if (source_profiles.empty() && !snapshot.pins.empty()) {
    source_profiles.insert(kDefaultProfileId);
  }
  std::set<std::string> inserted_profiles;
  for (const std::string& id : source_profiles) {
    plan.profile_ids[id] = id;
    if (!existing_profiles.count(id)) {
      inserted_profiles.insert(id);
    }
  }

  std::set<std::string> used_space_ids;
  for (const Space& space : target_spaces) {
    used_space_ids.insert(space.space_id);
  }
  std::vector<std::string> inserted_spaces;
  // snapshot.spaces is already in (sortOrder, profileId, createdDate, id).
  for (size_t i = 0; i < snapshot.spaces.size(); ++i) {
    const Space& space = snapshot.spaces[i];
    if (space.space_id == kDefaultSpaceId) {
      plan.space_ids[space.space_id] = kDefaultSpaceId;
      if (!used_space_ids.count(kDefaultSpaceId)) {
        used_space_ids.insert(kDefaultSpaceId);
        inserted_spaces.push_back(kDefaultSpaceId);
      }
      continue;
    }
    std::string target_id = space.space_id;
    if (used_space_ids.count(target_id)) {
      target_id = MigrationIdentifier(operation_id, "space",
                                      static_cast<int>(i), used_space_ids);
    }
    plan.space_ids[space.space_id] = target_id;
    used_space_ids.insert(target_id);
    inserted_spaces.push_back(target_id);
  }

  bool mapped_default = false;
  for (const auto& [source, mapped] : plan.space_ids) {
    mapped_default = mapped_default || mapped == kDefaultSpaceId;
  }
  bool needs_implicit_default =
      snapshot.themes.theme_ids.count(kDefaultSpaceId) ||
      snapshot.themes.overlay_opacities.count(kDefaultSpaceId) ||
      snapshot.themes.saturations.count(kDefaultSpaceId) ||
      (target_scope == PinScope::kSpace && !snapshot.pins.empty() &&
       target_spaces.empty() && !mapped_default);
  for (const Tab& row : snapshot.bookmarks) {
    needs_implicit_default =
        needs_implicit_default || *row.space_id == kDefaultSpaceId;
  }
  for (const UrlRule& rule : snapshot.rules) {
    needs_implicit_default =
        needs_implicit_default || rule.space_id == kDefaultSpaceId;
  }
  if (needs_implicit_default && !plan.space_ids.count(kDefaultSpaceId)) {
    plan.space_ids[kDefaultSpaceId] = kDefaultSpaceId;
    if (!used_space_ids.count(kDefaultSpaceId)) {
      used_space_ids.insert(kDefaultSpaceId);
      inserted_spaces.push_back(kDefaultSpaceId);
    }
  }
  if (needs_implicit_default && !plan.profile_ids.count(kDefaultProfileId) &&
      !target.FindSpace(kDefaultSpaceId)) {
    plan.profile_ids[kDefaultProfileId] = kDefaultProfileId;
    if (!existing_profiles.count(kDefaultProfileId)) {
      inserted_profiles.insert(kDefaultProfileId);
    }
  }
  plan.inserted_profile_ids.assign(inserted_profiles.begin(),
                                   inserted_profiles.end());
  std::set<std::string> seen_spaces;
  for (const std::string& id : inserted_spaces) {
    if (seen_spaces.insert(id).second) {
      plan.inserted_space_ids.push_back(id);
    }
  }

  for (const Space& space : target_spaces) {
    plan.target_space_profile_ids[space.space_id] = space.profile_id;
  }
  std::map<std::string, const Space*> source_space_by_id;
  for (const Space& space : snapshot.spaces) {
    source_space_by_id[space.space_id] = &space;
  }
  for (const auto& [source_id, target_id] : plan.space_ids) {
    if (plan.target_space_profile_ids.count(target_id)) {
      continue;
    }
    const auto source_space = source_space_by_id.find(source_id);
    if (source_space != source_space_by_id.end()) {
      const std::string& profile = source_space->second->profile_id;
      const auto mapped = plan.profile_ids.find(profile);
      plan.target_space_profile_ids[target_id] =
          mapped != plan.profile_ids.end() ? mapped->second : profile;
    } else {
      const auto mapped = plan.profile_ids.find(kDefaultProfileId);
      plan.target_space_profile_ids[target_id] =
          mapped != plan.profile_ids.end() ? mapped->second
                                           : std::string(kDefaultProfileId);
    }
  }

  std::set<std::string> used_model_guids;
  for (const auto& [guid, tab] : target.tabs()) {
    used_model_guids.insert(guid);
  }
  for (size_t i = 0; i < snapshot.bookmarks.size(); ++i) {
    const Tab& row = snapshot.bookmarks[i];
    std::string guid = row.guid;
    if (used_model_guids.count(guid)) {
      guid = MigrationIdentifier(operation_id, "bookmark", static_cast<int>(i),
                                 used_model_guids);
    }
    plan.bookmark_guids[row.guid] = guid;
    used_model_guids.insert(guid);
  }
  const bool has_default_bookmarks = std::any_of(
      snapshot.bookmarks.begin(), snapshot.bookmarks.end(),
      [](const Tab& row) { return *row.space_id == kDefaultSpaceId; });
  if (has_default_bookmarks) {
    const std::string guid = MigrationIdentifier(
        operation_id, "default-bookmarks", 0, used_model_guids);
    used_model_guids.insert(guid);
    plan.default_import_folder_guid = guid;
  }

  std::vector<const Tab*> target_pins;
  for (const auto& [guid, tab] : target.tabs()) {
    if (tab.type == RowType::kPinnedTab) {
      target_pins.push_back(&tab);
    }
  }
  std::set<std::string> target_profile_ids = existing_profiles;
  for (const auto& [source, mapped] : plan.profile_ids) {
    target_profile_ids.insert(mapped);
  }
  plan.pin_targets = PlanPinTargets(
      snapshot.pins, snapshot.source_scope, target_pins, target_scope,
      plan.profile_ids, plan.space_ids, target_profile_ids,
      plan.target_space_profile_ids, operation_id, &used_model_guids);
  std::sort(plan.pin_targets.begin(), plan.pin_targets.end(),
            [](const GuestPinTarget& a, const GuestPinTarget& b) {
              return std::make_tuple(a.target_profile_id.value_or(""),
                                     a.target_space_id.value_or(""),
                                     a.target_index, a.source_guid,
                                     a.target_guid) <
                     std::make_tuple(b.target_profile_id.value_or(""),
                                     b.target_space_id.value_or(""),
                                     b.target_index, b.source_guid,
                                     b.target_guid);
            });
  for (const Tab& row : snapshot.pins) {
    plan.pin_lineage_ids.emplace(*row.pin_lineage_id, *row.pin_lineage_id);
  }

  std::set<std::string> existing_signatures;
  std::map<std::string, int> next_rule_index;
  std::set<std::string> used_rule_ids;
  for (const UrlRule& rule : target_rules) {
    existing_signatures.insert(
        RuleSignature(rule.space_id, rule.host, rule.path_prefix));
    int& next = next_rule_index[rule.space_id];
    next = std::max(next, rule.sort_order + 1);
  }
  for (const UrlRule& rule : target.UrlRules(true)) {
    used_rule_ids.insert(rule.id);
  }
  for (size_t i = 0; i < snapshot.rules.size(); ++i) {
    const UrlRule& rule = snapshot.rules[i];
    std::string target_space;
    if (rule.space_id == kKioskRuleTargetId) {
      target_space = kKioskRuleTargetId;
    } else if (const auto mapped = plan.space_ids.find(rule.space_id);
               mapped != plan.space_ids.end()) {
      target_space = mapped->second;
    } else {
      *error = "URL rule " + rule.id + " has no target Space mapping";
      return false;
    }
    if (existing_signatures.count(
            RuleSignature(target_space, rule.host, rule.path_prefix))) {
      plan.skipped_rules.push_back(
          {rule.id, target_space, Lower(rule.host),
           NormalizedRulePathPrefix(rule.path_prefix)});
      continue;
    }
    std::string target_id = rule.id;
    if (used_rule_ids.count(target_id)) {
      target_id = MigrationIdentifier(operation_id, "url-rule",
                                      static_cast<int>(i), used_rule_ids);
    }
    int& next = next_rule_index[target_space];
    used_rule_ids.insert(target_id);
    plan.rule_targets.push_back({rule.id, target_id, target_space, next++});
  }
  *out = std::move(plan);
  return true;
}

// --- Indexes, presence, themes -----------------------------------------------

std::map<std::string, int> GuestBookmarkTargetIndexes(
    const GuestSnapshot& snapshot,
    const GuestMappings& mappings) {
  std::map<std::string, const Tab*> by_guid;
  for (const Tab& row : snapshot.bookmarks) {
    by_guid[row.guid] = &row;
  }
  std::map<std::string, std::vector<const Tab*>> grouped;
  for (const Tab& row : snapshot.bookmarks) {
    const auto space = mappings.space_ids.find(*row.space_id);
    const std::string target_space =
        space != mappings.space_ids.end() ? space->second : *row.space_id;
    std::string parent_key = "<root>";
    if (row.parent_guid) {
      const auto parent = mappings.bookmark_guids.find(*row.parent_guid);
      if (parent != mappings.bookmark_guids.end()) {
        parent_key = parent->second;
      }
    }
    if (parent_key == "<root>" && *row.space_id == kDefaultSpaceId &&
        mappings.default_import_folder_guid) {
      parent_key = *mappings.default_import_folder_guid;
    }
    grouped[target_space + std::string(1, '\0') + parent_key].push_back(&row);
  }
  std::map<std::string, int> result;
  for (auto& [key, rows] : grouped) {
    auto parent_index = [&](const Tab* row) {
      if (!row->parent_guid) {
        return 0;
      }
      const auto parent = by_guid.find(*row->parent_guid);
      return parent != by_guid.end() ? parent->second->index : 0;
    };
    std::sort(rows.begin(), rows.end(), [&](const Tab* a, const Tab* b) {
      return std::make_tuple(parent_index(a), a->index, a->guid) <
             std::make_tuple(parent_index(b), b->index, b->guid);
    });
    for (size_t i = 0; i < rows.size(); ++i) {
      result[rows[i]->guid] = static_cast<int>(i);
    }
  }
  return result;
}

GuestPresence GuestImportPresence(const LocalStore& target,
                                  const GuestMappings& mappings,
                                  std::string* detail) {
  std::vector<std::pair<std::string, bool>> expected;
  for (const std::string& id : mappings.inserted_profile_ids) {
    expected.push_back({"profile:" + id, target.FindProfile(id) != nullptr});
  }
  for (const std::string& id : mappings.inserted_space_ids) {
    expected.push_back({"space:" + id, target.FindSpace(id) != nullptr});
  }
  for (const std::string& guid : mappings.NewModelGuids()) {
    expected.push_back({"model:" + guid, target.FindTab(guid) != nullptr});
  }
  std::set<std::string> rule_ids;
  for (const UrlRule& rule : target.UrlRules(true)) {
    rule_ids.insert(rule.id);
  }
  for (const GuestRuleTarget& rule : mappings.rule_targets) {
    expected.push_back(
        {"rule:" + rule.target_id, rule_ids.count(rule.target_id) > 0});
  }
  size_t present = 0;
  std::vector<std::string> missing;
  for (const auto& [name, found] : expected) {
    if (found) {
      ++present;
    } else {
      missing.push_back(name);
    }
  }
  if (present == 0) {
    return GuestPresence::kAbsent;
  }
  if (present == expected.size()) {
    return GuestPresence::kComplete;
  }
  if (detail) {
    std::sort(missing.begin(), missing.end());
    *detail = "only part of a prior import exists; missing";
    for (const std::string& name : missing) {
      *detail += " " + name;
    }
  }
  return GuestPresence::kPartial;
}

GuestThemes MergeGuestThemes(
    const GuestThemes& target,
    const GuestThemes& guest,
    const std::map<std::string, std::string>& space_ids) {
  GuestThemes out = target;
  for (const auto& [source_id, target_id] : space_ids) {
    if (const auto it = guest.theme_ids.find(source_id);
        it != guest.theme_ids.end()) {
      out.theme_ids.emplace(target_id, it->second);
    }
    if (const auto it = guest.overlay_opacities.find(source_id);
        it != guest.overlay_opacities.end()) {
      out.overlay_opacities.emplace(target_id, it->second);
    }
    if (const auto it = guest.saturations.find(source_id);
        it != guest.saturations.end()) {
      out.saturations.emplace(target_id, it->second);
    }
  }
  return out;
}

// --- Import --------------------------------------------------------------------

bool LocalStore::ImportGuestData(const GuestSnapshot& snapshot,
                                 const GuestMappings& mappings,
                                 const std::string& folder_title,
                                 std::string* error) {
  if (!db_) {
    *error = "the target store is not open";
    return false;
  }
  switch (GuestImportPresence(*this, mappings, error)) {
    case GuestPresence::kComplete:
      return true;
    case GuestPresence::kPartial:
      return false;
    case GuestPresence::kAbsent:
      break;
  }
  auto fail = [&](std::string message) {
    Discard();
    *error = std::move(message);
    return false;
  };

  std::map<std::string, const Profile*> source_profiles;
  for (const Profile& profile : snapshot.profiles) {
    source_profiles[profile.profile_id] = &profile;
  }
  for (const std::string& id : mappings.inserted_profile_ids) {
    if (FindProfile(id)) {
      return fail("Profile " + id + " appeared after the migration was planned");
    }
    Profile profile;
    profile.profile_id = id;
    profile.guid = id;
    for (const auto& [source_id, mapped] : mappings.profile_ids) {
      if (mapped == id && source_profiles.count(source_id)) {
        profile.guid = source_profiles[source_id]->guid;
        profile.display_name = source_profiles[source_id]->display_name;
        break;
      }
    }
    PutProfile(profile);
  }

  std::map<std::string, const Space*> source_spaces;
  for (const Space& space : snapshot.spaces) {
    source_spaces[space.space_id] = &space;
  }
  int max_order = -1;
  for (const auto& [id, space] : spaces_) {
    max_order = std::max(max_order, space.sort_order);
  }
  int offset = 0;
  for (const std::string& target_id : mappings.inserted_space_ids) {
    if (FindSpace(target_id)) {
      return fail("Space " + target_id +
                  " appeared after the migration was planned");
    }
    const Space* source = nullptr;
    for (const auto& [source_id, mapped] : mappings.space_ids) {
      if (mapped == target_id && source_spaces.count(source_id)) {
        source = source_spaces[source_id];
        break;
      }
    }
    std::string profile_id = kDefaultProfileId;
    if (const auto it = mappings.target_space_profile_ids.find(target_id);
        it != mappings.target_space_profile_ids.end()) {
      profile_id = it->second;
    } else if (source) {
      profile_id = source->profile_id;
    }
    EnsureProfile(profile_id);
    Space space;
    space.space_id = target_id;
    space.profile_id = profile_id;
    space.name = source ? source->name : kDefaultSpaceName;
    space.color_hex = source ? source->color_hex : kDefaultSpaceColorHex;
    space.icon_name = source ? source->icon_name : kDefaultSpaceIconName;
    space.sort_order = max_order + 1 + offset++;
    space.created = source ? source->created : snapshot.captured_at;
    space.updated = source ? source->updated : snapshot.captured_at;
    PutSpace(space);
  }

  for (const std::string& guid : mappings.NewModelGuids()) {
    if (FindTab(guid)) {
      return fail("model identifier " + guid + " appeared after planning");
    }
  }

  // A (profile, Space) root: the existing one, else a new linked row.
  auto root_for = [&](const std::string& space_id) -> std::string {
    const auto profile = mappings.target_space_profile_ids.find(space_id);
    if (profile == mappings.target_space_profile_ids.end()) {
      return std::string();
    }
    std::string root = ExistingBookmarkRoot(profile->second, space_id);
    return root.empty() ? NewBookmarkRoot(profile->second, space_id) : root;
  };
  std::map<std::string, std::string> container_by_space;
  if (mappings.default_import_folder_guid) {
    const std::string root = root_for(kDefaultSpaceId);
    if (root.empty()) {
      return fail("the target default bookmark root is unavailable");
    }
    int next = 0;
    for (const auto& [guid, tab] : tabs_) {
      if (tab.parent_guid == root) {
        next = std::max(next, tab.index + 1);
      }
    }
    Tab folder;
    folder.guid = *mappings.default_import_folder_guid;
    folder.type = RowType::kBookmarkFolder;
    folder.title = folder_title;
    folder.index = next;
    folder.url = kFolderPlaceholderUrl;
    folder.created = snapshot.captured_at;
    folder.updated = snapshot.captured_at;
    folder.profile_id = mappings.target_space_profile_ids.at(kDefaultSpaceId);
    folder.space_id = kDefaultSpaceId;
    folder.parent_guid = root;
    PutTab(folder);
    container_by_space[kDefaultSpaceId] = folder.guid;
  }
  // Every inserted Space gets its root at once (the Windows createSpace
  // rule); the bookmarks' Spaces need theirs.
  for (const std::string& space_id : mappings.inserted_space_ids) {
    if (root_for(space_id).empty()) {
      return fail("bookmark root is unavailable for Space " + space_id);
    }
  }
  for (const Tab& row : snapshot.bookmarks) {
    if (*row.space_id == kDefaultSpaceId) {
      continue;
    }
    const auto mapped = mappings.space_ids.find(*row.space_id);
    if (mapped == mappings.space_ids.end()) {
      continue;
    }
    if (!container_by_space.count(mapped->second)) {
      const std::string root = root_for(mapped->second);
      if (root.empty()) {
        return fail("bookmark root is unavailable for Space " +
                    mapped->second);
      }
      container_by_space[mapped->second] = root;
    }
  }

  const std::map<std::string, int> indexes =
      GuestBookmarkTargetIndexes(snapshot, mappings);
  for (const Tab& source : snapshot.bookmarks) {
    const auto guid = mappings.bookmark_guids.find(source.guid);
    const auto space = mappings.space_ids.find(*source.space_id);
    if (guid == mappings.bookmark_guids.end() ||
        space == mappings.space_ids.end() ||
        !mappings.target_space_profile_ids.count(space->second)) {
      return fail("bookmark " + source.guid +
                  " has incomplete identifier mappings");
    }
    Tab row = source;
    row.guid = guid->second;
    const auto index = indexes.find(source.guid);
    row.index = index != indexes.end() ? index->second : source.index;
    row.space_id = space->second;
    row.profile_id = mappings.target_space_profile_ids.at(space->second);
    std::optional<std::string> parent;
    if (source.parent_guid) {
      const auto imported = mappings.bookmark_guids.find(*source.parent_guid);
      if (imported != mappings.bookmark_guids.end()) {
        parent = imported->second;
      }
    }
    if (!parent) {
      const auto container = container_by_space.find(space->second);
      if (container != container_by_space.end()) {
        parent = container->second;
      }
    }
    row.parent_guid = parent;
    PutTab(row);
  }

  std::map<std::string, const Tab*> source_pins;
  for (const Tab& pin : snapshot.pins) {
    source_pins[pin.guid] = &pin;
  }
  for (const GuestPinTarget& mapping : mappings.pin_targets) {
    if (!mapping.should_insert) {
      continue;
    }
    const auto source = source_pins.find(mapping.source_guid);
    if (source == source_pins.end()) {
      return fail("pinned source " + mapping.source_guid + " is missing");
    }
    Tab row = *source->second;
    row.guid = mapping.target_guid;
    row.index = mapping.target_index;
    row.profile_id = mapping.target_profile_id;
    row.space_id = mapping.target_space_id;
    row.split_partner_guid = mapping.target_split_partner_guid;
    const std::string lineage = *source->second->pin_lineage_id;
    const auto mapped_lineage = mappings.pin_lineage_ids.find(lineage);
    row.pin_lineage_id = mapped_lineage != mappings.pin_lineage_ids.end()
                             ? mapped_lineage->second
                             : lineage;
    row.pin_dormant = mapping.dormant;
    PutTab(row);
  }

  std::map<std::string, const UrlRule*> source_rules;
  for (const UrlRule& rule : snapshot.rules) {
    source_rules[rule.id] = &rule;
  }
  std::set<std::string> used_sync_ids;
  for (const auto& [id, rule] : rules_) {
    if (rule.sync_id) {
      used_sync_ids.insert(*rule.sync_id);
    }
  }
  for (const GuestRuleTarget& mapping : mappings.rule_targets) {
    const auto source = source_rules.find(mapping.source_id);
    if (source == source_rules.end()) {
      return fail("URL rule source " + mapping.source_id + " is missing");
    }
    UrlRule rule;
    rule.id = mapping.target_id;
    rule.space_id = mapping.target_space_id;
    rule.host = Lower(source->second->host);
    rule.path_prefix = NormalizedRulePathPrefix(source->second->path_prefix);
    rule.ask = source->second->ask;
    rule.sort_order = mapping.target_sort_order;
    rule.created = source->second->created;
    // A live rule always has an account identity (the Mac's V12 backfill);
    // a new local row publishes on the next round.
    std::string sync_id = source->second->sync_id.value_or("");
    if (sync_id.empty() || used_sync_ids.count(sync_id)) {
      sync_id = Lower(env_.new_uuid());
    }
    used_sync_ids.insert(sync_id);
    rule.sync_id = sync_id;
    rule.pending_local_edit = true;
    PutRule(rule);
  }
  if (!Flush()) {
    *error = "the target store could not be written";
    return false;
  }
  return true;
}

// --- Verify --------------------------------------------------------------------

bool VerifyGuestImport(const LocalStore& target,
                       const GuestSnapshot& snapshot,
                       const GuestMappings& mappings,
                       std::string* error) {
  for (const auto& [source, profile] : mappings.profile_ids) {
    if (!target.FindProfile(profile)) {
      *error = "missing Profile " + profile;
      return false;
    }
  }
  for (const auto& [source, space_id] : mappings.space_ids) {
    const Space* space = target.FindSpace(space_id);
    const auto profile = mappings.target_space_profile_ids.find(space_id);
    if (!space || profile == mappings.target_space_profile_ids.end() ||
        space->profile_id != profile->second) {
      *error = "Space " + space_id + " is missing or belongs to another Profile";
      return false;
    }
  }
  const std::map<std::string, int> indexes =
      GuestBookmarkTargetIndexes(snapshot, mappings);
  std::map<std::string, const Tab*> source_bookmarks;
  for (const Tab& row : snapshot.bookmarks) {
    source_bookmarks[row.guid] = &row;
  }
  for (const auto& [source_guid, target_guid] : mappings.bookmark_guids) {
    const auto source_it = source_bookmarks.find(source_guid);
    const Tab* row = target.FindTab(target_guid);
    if (source_it == source_bookmarks.end() || !row) {
      *error = "bookmark " + source_guid + " is missing";
      return false;
    }
    const Tab& source = *source_it->second;
    const auto space = mappings.space_ids.find(*source.space_id);
    const auto index = indexes.find(source_guid);
    if (row->type != source.type || row->title != source.title ||
        row->url != source.url || row->favicon != source.favicon ||
        index == indexes.end() || row->index != index->second ||
        space == mappings.space_ids.end() || row->space_id != space->second ||
        row->profile_id !=
            mappings.target_space_profile_ids.at(space->second)) {
      *error = "bookmark " + source_guid + " does not match its imported record";
      return false;
    }
    std::optional<std::string> expected_parent;
    if (source.parent_guid) {
      expected_parent = mappings.bookmark_guids.at(*source.parent_guid);
    } else if (*source.space_id == kDefaultSpaceId) {
      expected_parent = mappings.default_import_folder_guid;
    } else {
      const Space* target_space = target.FindSpace(space->second);
      if (target_space) {
        expected_parent = target_space->bookmark_root;
      }
    }
    if (!expected_parent || row->parent_guid != expected_parent) {
      *error = "bookmark " + source_guid + " has the wrong parent";
      return false;
    }
  }
  if (mappings.default_import_folder_guid) {
    const Tab* folder = target.FindTab(*mappings.default_import_folder_guid);
    const Space* space = target.FindSpace(kDefaultSpaceId);
    if (!folder || folder->type != RowType::kBookmarkFolder ||
        folder->space_id != kDefaultSpaceId || !space ||
        folder->parent_guid !=
            target.ExistingBookmarkRoot(space->profile_id, kDefaultSpaceId)) {
      *error = "the default-Space import folder is missing";
      return false;
    }
  }
  std::map<std::string, const Tab*> source_pins;
  for (const Tab& row : snapshot.pins) {
    source_pins[row.guid] = &row;
  }
  for (const GuestPinTarget& mapping : mappings.pin_targets) {
    const auto source_it = source_pins.find(mapping.source_guid);
    const Tab* row = target.FindTab(mapping.target_guid);
    if (source_it == source_pins.end() || !row) {
      *error = "pinned tab " + mapping.source_guid + " is missing";
      return false;
    }
    const Tab& source = *source_it->second;
    const std::string lineage = *source.pin_lineage_id;
    const auto mapped_lineage = mappings.pin_lineage_ids.find(lineage);
    const std::string expected_lineage =
        mapped_lineage != mappings.pin_lineage_ids.end()
            ? mapped_lineage->second
            : lineage;
    if (row->type != RowType::kPinnedTab || row->title != source.title ||
        row->url != source.url || row->favicon != source.favicon ||
        row->profile_id != mapping.target_profile_id ||
        row->space_id != mapping.target_space_id ||
        row->pin_lineage_id.value_or(row->guid) != expected_lineage ||
        row->split_partner_guid != mapping.target_split_partner_guid) {
      *error = "pinned tab " + mapping.source_guid +
               " does not match its target owner";
      return false;
    }
    if (mapping.should_insert &&
        (row->index != mapping.target_index ||
         row->override_title != source.override_title ||
         row->secondary_url != source.secondary_url ||
         row->secondary_title != source.secondary_title ||
         row->layout != source.layout || row->pin_dormant != mapping.dormant)) {
      *error = "pinned tab " + mapping.target_guid + " lost persisted fields";
      return false;
    }
  }
  std::map<std::string, const UrlRule*> target_rules;
  const std::vector<UrlRule> rules = target.UrlRules(true);
  std::set<std::string> signatures;
  for (const UrlRule& rule : rules) {
    target_rules[rule.id] = &rule;
    if (!rule.deleted) {
      signatures.insert(
          RuleSignature(rule.space_id, rule.host, rule.path_prefix));
    }
  }
  std::map<std::string, const UrlRule*> source_rules;
  for (const UrlRule& rule : snapshot.rules) {
    source_rules[rule.id] = &rule;
  }
  for (const GuestRuleTarget& mapping : mappings.rule_targets) {
    const auto source = source_rules.find(mapping.source_id);
    const auto row = target_rules.find(mapping.target_id);
    if (source == source_rules.end() || row == target_rules.end() ||
        row->second->space_id != mapping.target_space_id ||
        row->second->host != Lower(source->second->host) ||
        row->second->path_prefix !=
            NormalizedRulePathPrefix(source->second->path_prefix) ||
        row->second->ask != source->second->ask ||
        row->second->sort_order != mapping.target_sort_order) {
      *error = "URL rule " + mapping.source_id +
               " does not match its imported record";
      return false;
    }
  }
  for (const GuestSkippedRule& skipped : mappings.skipped_rules) {
    if (!signatures.count(RuleSignature(skipped.target_space_id, skipped.host,
                                        skipped.path_prefix))) {
      *error = "the target-winning URL rule for " + skipped.source_id +
               " disappeared";
      return false;
    }
  }
  return true;
}

}  // namespace phi_client::store

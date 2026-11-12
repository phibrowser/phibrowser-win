// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_sync_merge.h"

#include <algorithm>
#include <initializer_list>

#include "client/phi_key_crypto.h"

namespace phi_client::sync {

namespace {

SettingValue StringValue(const std::string& value) {
  SettingValue out;
  out.SetString(value);
  return out;
}


std::string Signatures(std::initializer_list<const OptionalValue*> values) {
  std::string out;
  for (const OptionalValue* value : values) {
    out += ValueSignature(value->get());
    out.push_back('\0');
  }
  return out;
}

// AgentSpaceManager's agent Space signatures.
constexpr char kAgentIcon[] = "emoji:1F916";
constexpr char kAgentColor[] = "#8E8E93";
constexpr char kPersistentAgentColor[] = "#5856D6";

bool IsAgentSpaceName(const std::string& name) {
  // "R" then one or more digits (the Mac's Character.isNumber; the names it
  // mints are ASCII).
  if (name.size() < 2 || name[0] != 'R') {
    return false;
  }
  return std::all_of(name.begin() + 1, name.end(),
                     [](char c) { return c >= '0' && c <= '9'; });
}

// --- Bookmarks ---------------------------------------------------------------------

// (space, parent): a root is (space_uuid, ""), a descendant ("", parent).
std::pair<std::string, std::string> BookmarkLocation(const BookmarkEntity& e) {
  const std::string parent = e.parent_uuid.get().str();
  return parent.empty() ? std::make_pair(e.space_uuid.get().str(), std::string())
                        : std::make_pair(std::string(), parent);
}

// The whole location as one LWW ballot; "s"/"p" keep a Space X root apart
// from a child of folder X.
SettingValue BookmarkBallot(const BookmarkEntity& e) {
  const auto location = BookmarkLocation(e);
  SettingValue ballot =
      StringValue(location.second.empty()
                      ? std::string("s") + '\0' + location.first
                      : std::string("p") + '\0' + location.second);
  ballot.updated_at_ms = BookmarkLocationStamp(e);
  return ballot;
}

SettingValue UrlRuleBallot(const UrlRuleEntity& e) {
  SettingValue ballot = StringValue(e.host.get().str() + '\0' +
                                    e.path_prefix.get().str() + '\0' +
                                    (e.ask.get().flag() ? "1" : "0"));
  ballot.updated_at_ms = e.host.get().updated_at_ms;
  return ballot;
}

}  // namespace

int32_t MergedSource(int32_t left, int32_t right) {
  if (left == 0) {
    return right;
  }
  if (right == 0) {
    return left;
  }
  return std::min(left, right);
}

int64_t MergedCreatedAtMs(int64_t left, int64_t right) {
  if (left > 0 && right > 0) {
    return std::min(left, right);
  }
  if (left > 0) {
    return left;
  }
  return right > 0 ? right : 0;
}

SettingValue LwwWinner(const SettingValue& lhs, const SettingValue& rhs) {
  if (lhs.updated_at_ms != rhs.updated_at_ms) {
    return lhs.updated_at_ms > rhs.updated_at_ms ? lhs : rhs;
  }
  // Byte order, unsigned (Swift's lexicographicallyPrecedes on Data).
  return lhs.Serialize() < rhs.Serialize() ? rhs : lhs;
}

std::string ValueSignature(const SettingValue& value) {
  SettingValue stripped = value;
  stripped.updated_at_ms = 0;
  return stripped.Serialize();
}

SettingEntity MergeSettings(const SettingEntity& local,
                            const SettingEntity& remote) {
  SettingEntity merged = local;
  for (const auto& [key, remote_value] : remote.values) {
    const auto it = merged.values.find(key);
    if (it == merged.values.end()) {
      merged.values[key] = remote_value;
    } else {
      it->second = LwwWinner(it->second, remote_value);
    }
  }
  return merged;
}

SpaceEntity MergeSpace(const SpaceEntity& local, const SpaceEntity& remote) {
  SpaceEntity merged = remote;
  merged.space_uuid =
      local.space_uuid.empty() ? remote.space_uuid : local.space_uuid;
  merged.name.set(LwwWinner(local.name.get(), remote.name.get()));
  merged.icon_name.set(LwwWinner(local.icon_name.get(), remote.icon_name.get()));
  merged.color_hex.set(LwwWinner(local.color_hex.get(), remote.color_hex.get()));
  merged.rank.set(LwwWinner(local.rank.get(), remote.rank.get()));
  if (local.profile_uuid.has() || remote.profile_uuid.has()) {
    merged.profile_uuid.set(
        LwwWinner(local.profile_uuid.get(), remote.profile_uuid.get()));
  }
  if (local.theme_id.has() || remote.theme_id.has()) {
    merged.theme_id.set(LwwWinner(local.theme_id.get(), remote.theme_id.get()));
  }
  merged.overlay_opacity_light.set(LwwWinner(local.overlay_opacity_light.get(),
                                             remote.overlay_opacity_light.get()));
  merged.overlay_opacity_dark.set(LwwWinner(local.overlay_opacity_dark.get(),
                                            remote.overlay_opacity_dark.get()));
  merged.created_at_ms =
      MergedCreatedAtMs(local.created_at_ms, remote.created_at_ms);
  return merged;
}

bool SpaceRefuses(const SpaceEntity& entity) {
  const std::string name = entity.name.get().str();
  const std::string icon = entity.icon_name.get().str();
  const std::string color = entity.color_hex.get().str();
  if (IsAgentSpaceName(name) && icon == kAgentIcon && color == kAgentColor) {
    return true;
  }
  return icon == kAgentIcon && color == kPersistentAgentColor;
}

// --- Bookmarks -----------------------------------------------------------------------

BookmarkEntity MergeBookmark(const BookmarkEntity& local,
                             const BookmarkEntity& remote) {
  BookmarkEntity merged = remote;
  merged.bookmark_uuid =
      local.bookmark_uuid.empty() ? remote.bookmark_uuid : local.bookmark_uuid;

  const SettingValue local_ballot = BookmarkBallot(local);
  const SettingValue remote_ballot = BookmarkBallot(remote);
  const bool local_wins = LwwWinner(local_ballot, remote_ballot) == local_ballot;
  const BookmarkEntity& winner = local_wins ? local : remote;
  if (local_ballot == remote_ballot) {
    // Both name the same location: resolve the members through the shared
    // winner on their own values (symmetric), not from an arbitrary side.
    merged.space_uuid.set(LwwWinner(local.space_uuid.get(),
                                    remote.space_uuid.get()));
    merged.parent_uuid.set(LwwWinner(local.parent_uuid.get(),
                                     remote.parent_uuid.get()));
  } else {
    merged.space_uuid = winner.space_uuid;
    merged.parent_uuid = winner.parent_uuid;
  }
  // Both location members carry the winner's carrier stamp.
  const int64_t stamp = BookmarkLocationStamp(winner);
  merged.space_uuid.mut().updated_at_ms = stamp;
  merged.parent_uuid.mut().updated_at_ms = stamp;
  merged.rank = local_ballot.str() == remote_ballot.str()
                    ? [&] {
                        OptionalValue rank;
                        rank.set(LwwWinner(local.rank.get(), remote.rank.get()));
                        return rank;
                      }()
                    : winner.rank;

  merged.is_folder = remote.is_folder;
  merged.title.set(LwwWinner(local.title.get(), remote.title.get()));
  merged.url.set(LwwWinner(local.url.get(), remote.url.get()));
  merged.secondary_url.set(
      LwwWinner(local.secondary_url.get(), remote.secondary_url.get()));
  merged.secondary_title.set(
      LwwWinner(local.secondary_title.get(), remote.secondary_title.get()));
  merged.source = MergedSource(local.source, remote.source);
  merged.created_at_ms =
      MergedCreatedAtMs(local.created_at_ms, remote.created_at_ms);
  return merged;
}

std::string BookmarkContentSignature(const BookmarkEntity& entity) {
  std::string out = Signatures({&entity.title, &entity.url,
                                &entity.secondary_url, &entity.secondary_title});
  out.push_back(entity.is_folder ? '\1' : '\0');
  return out;
}

int64_t BookmarkLocationStamp(const BookmarkEntity& entity) {
  return entity.parent_uuid.get().str().empty()
             ? entity.space_uuid.get().updated_at_ms
             : entity.parent_uuid.get().updated_at_ms;
}

int64_t BookmarkContentStamp(const BookmarkEntity& entity) {
  return std::max({entity.title.get().updated_at_ms,
                   entity.url.get().updated_at_ms,
                   entity.secondary_url.get().updated_at_ms,
                   entity.secondary_title.get().updated_at_ms});
}

// --- Pinned tabs ---------------------------------------------------------------------

PinTabEntity MergePin(const PinTabEntity& local, const PinTabEntity& remote) {
  PinTabEntity merged = remote;
  merged.pin_uuid = local.pin_uuid.empty() ? remote.pin_uuid : local.pin_uuid;
  merged.rank.set(LwwWinner(local.rank.get(), remote.rank.get()));
  merged.title.set(LwwWinner(local.title.get(), remote.title.get()));
  merged.url.set(LwwWinner(local.url.get(), remote.url.get()));
  merged.split_partner_uuid.set(LwwWinner(local.split_partner_uuid.get(),
                                          remote.split_partner_uuid.get()));
  merged.source = MergedSource(local.source, remote.source);
  merged.created_at_ms =
      MergedCreatedAtMs(local.created_at_ms, remote.created_at_ms);
  return merged;
}

std::string PinOwnerKey(const PinTabEntity& entity) {
  return entity.owner == PinTabEntity::Owner::kApp ? std::string("app")
                                                    : entity.owner_uuid;
}

std::string PinIdentity(const PinTabEntity& entity) {
  if (entity.pin_uuid.empty()) {
    return std::string();
  }
  return entity.pin_uuid + ":" + PinOwnerKey(entity);
}

std::string PinContentSignature(const PinTabEntity& entity) {
  return Signatures({&entity.title, &entity.url, &entity.split_partner_uuid});
}

int64_t PinLocationStamp(const PinTabEntity& entity) {
  return entity.rank.get().updated_at_ms;
}

int64_t PinContentStamp(const PinTabEntity& entity) {
  return std::max({entity.title.get().updated_at_ms,
                   entity.url.get().updated_at_ms,
                   entity.split_partner_uuid.get().updated_at_ms});
}

// --- URL rules -----------------------------------------------------------------------

UrlRuleEntity MergeUrlRule(const UrlRuleEntity& local,
                           const UrlRuleEntity& remote) {
  UrlRuleEntity merged = remote;
  merged.rule_uuid = local.rule_uuid.empty() ? remote.rule_uuid : local.rule_uuid;

  const SettingValue local_ballot = UrlRuleBallot(local);
  const SettingValue remote_ballot = UrlRuleBallot(remote);
  const UrlRuleEntity& content_winner =
      LwwWinner(local_ballot, remote_ballot) == local_ballot ? local : remote;
  if (local_ballot == remote_ballot) {
    merged.host.set(LwwWinner(local.host.get(), remote.host.get()));
    merged.path_prefix.set(
        LwwWinner(local.path_prefix.get(), remote.path_prefix.get()));
    merged.ask.set(LwwWinner(local.ask.get(), remote.ask.get()));
  } else {
    merged.host = content_winner.host;
    merged.path_prefix = content_winner.path_prefix;
    merged.ask = content_winner.ask;
  }
  // All three content members carry the winning carrier's stamp.
  const int64_t stamp = content_winner.host.get().updated_at_ms;
  merged.host.mut().updated_at_ms = stamp;
  merged.path_prefix.mut().updated_at_ms = stamp;
  merged.ask.mut().updated_at_ms = stamp;

  const SettingValue target_winner =
      LwwWinner(local.target_space_uuid.get(), remote.target_space_uuid.get());
  merged.target_space_uuid.set(target_winner);
  if (local.target_space_uuid.get().str() ==
      remote.target_space_uuid.get().str()) {
    merged.rank.set(LwwWinner(local.rank.get(), remote.rank.get()));
  } else {
    merged.rank.set(target_winner == local.target_space_uuid.get()
                        ? local.rank.get()
                        : remote.rank.get());
  }
  merged.source = MergedSource(local.source, remote.source);
  merged.created_at_ms =
      MergedCreatedAtMs(local.created_at_ms, remote.created_at_ms);
  return merged;
}

std::string UrlRuleContentSignature(const UrlRuleEntity& entity) {
  return Signatures({&entity.host, &entity.path_prefix, &entity.ask});
}

int64_t UrlRuleLocationStamp(const UrlRuleEntity& entity) {
  return entity.target_space_uuid.get().updated_at_ms;
}

int64_t UrlRuleContentStamp(const UrlRuleEntity& entity) {
  return entity.host.get().updated_at_ms;
}

// --- Client tags ---------------------------------------------------------------------

std::string SpaceClientTag(const std::string& space_uuid) {
  return "phi-space:" + space_uuid;
}

std::string BookmarkClientTag(const std::string& bookmark_uuid) {
  return "phi-bookmark:" + bookmark_uuid;
}

std::string PinClientTag(const std::string& lineage,
                         const std::string& owner_key) {
  return "phi-pin:" + lineage + ":" + owner_key;
}

std::string UrlRuleClientTag(const std::string& rule_uuid) {
  return "phi-urlrule:" + rule_uuid;
}

std::string ClientTagHash(const std::string& tag) {
  std::string input("\x82\x7D\x00", 3);
  input += tag;
  return keys::Base64Encode(keys::Sha1(input));
}

}  // namespace phi_client::sync

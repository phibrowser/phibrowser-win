// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_sync_rules.h"

#include <algorithm>
#include <climits>
#include <tuple>
#include <utility>

#include "phibrowser-win/client/phi_sync_clock.h"
#include "phibrowser-win/client/phi_sync_merge.h"
#include "phibrowser-win/client/phi_sync_rank.h"
#include "phibrowser-win/client/phi_sync_spaces.h"
#include "phibrowser-win/client/phi_url_rule_normalize.h"

namespace phi_client::sync {

namespace {

SettingValue StringValue(const std::string& value) {
  SettingValue out;
  out.SetString(value);
  return out;
}

SettingValue BoolValue(bool value) {
  SettingValue out;
  out.SetBool(value);
  return out;
}

void SetStamp(OptionalValue& value, int64_t stamp) {
  value.mut().updated_at_ms = stamp;
}

// Host, path and ask are one merge unit on host's stamp.
void SetContentStamp(UrlRuleEntity& entity, int64_t stamp) {
  SetStamp(entity.host, stamp);
  SetStamp(entity.path_prefix, stamp);
  SetStamp(entity.ask, stamp);
}

int64_t Restamped(const OptionalValue& value,
                  const OptionalValue& baseline,
                  int64_t edit_wall_ms) {
  return ValueSignature(value.get()) == ValueSignature(baseline.get())
             ? baseline.get().updated_at_ms
             : HybridClock::EditStamp(edit_wall_ms, baseline.get().updated_at_ms);
}

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

bool IsIncognitoSpaceId(const std::string& id) {
  return id.rfind(kIncognitoRuleTargetId, 0) == 0;
}

bool IsRoutableRuleTarget(const std::string& id) {
  if (id == kKioskRuleTargetId) {
    return true;
  }
  return !IsIncognitoSpaceId(id) || id == kIncognitoRuleTargetId;
}

std::optional<int64_t> StampOrNone(int64_t ms) {
  return ms == 0 ? std::nullopt : std::optional<int64_t>(ms);
}

int64_t OrMin(const std::optional<int64_t>& ms) {
  return ms.value_or(INT64_MIN);
}

std::optional<RuleSignature> SignatureFor(const std::string& host,
                                          const std::optional<std::string>& path,
                                          const std::string& owner,
                                          const OwnerResolver& resolve) {
  if (owner.empty()) {
    return std::nullopt;
  }
  if (resolve.local_space_id(owner) && !resolve.is_eligible_space(owner)) {
    return std::nullopt;
  }
  auto [normalized_host, normalized_path] = NormalizedRule(host, path);
  return RuleSignature{normalized_host, normalized_path, owner};
}

const LocalUrlRule* RowBySyncId(const std::vector<LocalUrlRule>& rows,
                                const std::string& sync_id) {
  for (const LocalUrlRule& row : rows) {
    if (row.sync_id == sync_id) {
      return &row;
    }
  }
  return nullptr;
}

LocalUrlRule* MutableRowBySyncId(std::vector<LocalUrlRule>& rows,
                                 const std::string& sync_id) {
  for (LocalUrlRule& row : rows) {
    if (row.sync_id == sync_id) {
      return &row;
    }
  }
  return nullptr;
}

// --- The merge tail -----------------------------------------------------------------

// The effective account stamps of the identities asked for: this page's
// landed values first, else the (rebaselined or reconciled) baseline.
std::map<std::string, UrlRuleEffectiveStamps> EffectiveAccountStamps(
    const std::map<std::string, UrlRuleLandingValues>& landed,
    const std::map<std::string, std::string>& rebaselined,
    const OwnedTable& table,
    const std::set<std::string>& identities) {
  std::map<std::string, UrlRuleEffectiveStamps> out;
  for (const std::string& identity : identities) {
    UrlRuleEffectiveStamps stamps;
    if (const auto it = landed.find(identity); it != landed.end()) {
      stamps.content = StampOrNone(it->second.content_updated_ms);
      stamps.target = StampOrNone(it->second.target_updated_ms);
    }
    if (!stamps.content || !stamps.target) {
      std::optional<std::string> bytes;
      if (const auto it = rebaselined.find(identity); it != rebaselined.end()) {
        bytes = it->second;
      } else if (const auto cursor = table.cursors.find(identity);
                 cursor != table.cursors.end()) {
        bytes = cursor->second.reconciled;
      }
      if (const std::optional<UrlRuleEntity> entity =
              DecodeOwned<UrlRuleKind>(bytes)) {
        if (!stamps.content) {
          stamps.content = StampOrNone(entity->host.get().updated_at_ms);
        }
        if (!stamps.target) {
          stamps.target = StampOrNone(entity->target_space_uuid.get().updated_at_ms);
        }
      }
    }
    if (stamps.content || stamps.target) {
      out[identity] = stamps;
    }
  }
  return out;
}

// Pointers from each duplicate to the smallest anchored identity sharing its
// signature (current, then pre-landing).
std::map<std::string, std::string> MergePointerPass(
    const std::vector<LocalUrlRule>& live_rows,
    const std::vector<LocalUrlRule>& anchor_rows,
    const std::set<std::string>& landed_this_page,
    const std::set<std::string>& published_identities,
    const std::map<std::string, RuleSignature>& pre_landing_signatures,
    const OwnerResolver& resolve) {
  std::set<std::string> live_identities;
  std::map<std::string, std::string> pointer;
  for (const LocalUrlRule& row : live_rows) {
    if (!row.sync_id) {
      continue;
    }
    live_identities.insert(*row.sync_id);
    if (row.merge_partner_sync_id) {
      pointer[*row.sync_id] = *row.merge_partner_sync_id;
    }
  }
  std::map<std::string, std::string> out;
  auto pass = [&](const std::function<std::optional<RuleSignature>(
                      const LocalUrlRule&)>& key_of,
                  const std::vector<LocalUrlRule>& anchors_from) {
    std::map<RuleSignature, std::vector<const LocalUrlRule*>> groups;
    for (const LocalUrlRule& row : live_rows) {
      if (!row.sync_id) {
        continue;
      }
      if (std::optional<RuleSignature> key = key_of(row)) {
        groups[*key].push_back(&row);
      }
    }
    std::map<RuleSignature, std::vector<std::string>> anchor_groups;
    for (const LocalUrlRule& row : anchors_from) {
      if (!row.sync_id || (!published_identities.count(*row.sync_id) &&
                           !landed_this_page.count(*row.sync_id))) {
        continue;
      }
      if (std::optional<RuleSignature> key = key_of(row)) {
        anchor_groups[*key].push_back(*row.sync_id);
      }
    }
    for (auto& [key, members] : groups) {  // sorted keys
      std::sort(members.begin(), members.end(),
                [](const LocalUrlRule* a, const LocalUrlRule* b) {
                  return a->sync_id.value_or("") < b->sync_id.value_or("");
                });
      std::vector<std::string> anchors = anchor_groups[key];
      std::sort(anchors.begin(), anchors.end());
      if (anchors.size() < 2) {
        continue;
      }
      const std::string anchor = anchors.front();
      for (const LocalUrlRule* row : members) {
        const std::string& identity = *row->sync_id;
        if (identity == anchor) {
          continue;
        }
        const auto current = pointer.find(identity);
        if (current != pointer.end() && live_identities.count(current->second)) {
          continue;
        }
        if (current != pointer.end() && current->second == anchor) {
          continue;
        }
        out[identity] = anchor;
        pointer[identity] = anchor;
      }
    }
  };
  pass([&](const LocalUrlRule& row) { return UrlRuleKind::SignatureOf(row, resolve); },
       anchor_rows);
  pass(
      [&](const LocalUrlRule& row) -> std::optional<RuleSignature> {
        if (row.sync_id) {
          const auto key = pre_landing_signatures.find(*row.sync_id);
          if (key != pre_landing_signatures.end()) {
            return key->second;
          }
        }
        return UrlRuleKind::SignatureOf(row, resolve);
      },
      live_rows);
  return out;
}

struct ContentGroupWrite {
  std::string sync_id;
  std::string host;
  std::optional<std::string> path_prefix;
  bool ask = false;
  int64_t content_updated_ms = 0;
};

struct Convergence {
  std::vector<ContentGroupWrite> content_group_writes;
  std::vector<std::pair<std::string, std::string>> soft_deletes;  // loser, winner
  std::vector<std::string> cleared_partners;
  int collapsed = 0;
  std::set<std::string> touched_buckets;
};

// Duplicates at rest collapse into the smallest identity, which takes the
// newest content.
Convergence ConvergePass(
    std::map<RuleSignature, std::vector<const LocalUrlRule*>>& groups,
    const std::set<std::string>& at_rest,
    const std::map<std::string, int64_t>& account_stamps) {
  Convergence out;
  auto content = [](const LocalUrlRule& row) {
    auto [host, path] = NormalizedRule(row.host, row.path_prefix);
    return std::make_tuple(host, path, row.ask);
  };
  auto stamp = [&](const LocalUrlRule& row) -> std::optional<int64_t> {
    if (!row.sync_id) {
      return std::nullopt;
    }
    const auto it = account_stamps.find(*row.sync_id);
    return it == account_stamps.end() ? std::nullopt
                                      : std::optional<int64_t>(it->second);
  };
  for (auto& [key, members] : groups) {
    std::sort(members.begin(), members.end(),
              [](const LocalUrlRule* a, const LocalUrlRule* b) {
                return a->sync_id.value_or("") < b->sync_id.value_or("");
              });
    std::vector<const LocalUrlRule*> settled;
    for (const LocalUrlRule* row : members) {
      if (row->sync_id && at_rest.count(*row->sync_id)) {
        settled.push_back(row);
      }
    }
    if (settled.size() < 2) {
      if (members.size() == 1 && members[0]->sync_id &&
          at_rest.count(*members[0]->sync_id) &&
          members[0]->merge_partner_sync_id) {
        out.cleared_partners.push_back(*members[0]->sync_id);
      }
      continue;
    }
    const LocalUrlRule* winner = settled.front();
    const LocalUrlRule* source = *std::max_element(
        settled.begin(), settled.end(),
        [&](const LocalUrlRule* a, const LocalUrlRule* b) {
          return std::make_pair(OrMin(stamp(*a)), a->sync_id.value_or("")) <
                 std::make_pair(OrMin(stamp(*b)), b->sync_id.value_or(""));
        });
    if (const std::optional<int64_t> source_stamp = stamp(*source)) {
      const int64_t winner_stamp = OrMin(stamp(*winner));
      if (content(*source) != content(*winner) || *source_stamp > winner_stamp) {
        auto [host, path] = NormalizedRule(source->host, source->path_prefix);
        out.content_group_writes.push_back(
            {*winner->sync_id, host, path, source->ask, *source_stamp});
      }
    }
    for (size_t i = 1; i < settled.size(); ++i) {
      out.soft_deletes.push_back({*settled[i]->sync_id, *winner->sync_id});
      ++out.collapsed;
      out.touched_buckets.insert(settled[i]->space_id);
    }
  }
  return out;
}

// transferDecision: each unit is written only when the source is newer
// than what the target already holds (its row or its account stamp).
std::pair<bool, bool> TransferDecision(const LocalUrlRule& target,
                                       const RuleProjection& source,
                                       const UrlRuleEffectiveStamps& stamps) {
  const int64_t target_content =
      std::max(OrMin(target.content_updated_ms), OrMin(stamps.content));
  const bool writes_content = OrMin(source.content_updated_ms) > target_content;
  bool writes_target = false;
  if (source.target_space_id) {
    const int64_t target_target =
        std::max(OrMin(target.target_updated_ms), OrMin(stamps.target));
    writes_target = OrMin(source.target_updated_ms) > target_target;
  }
  return {writes_content, writes_target};
}

}  // namespace

std::pair<std::string, std::optional<std::string>> NormalizedRule(
    const std::string& host,
    const std::optional<std::string>& path_prefix) {
  return {store::NormalizedRuleHost(host),
          store::NormalizedRulePathPrefix(path_prefix)};
}

// --- UrlRuleKind ------------------------------------------------------------------------

std::string UrlRuleKind::ClientTag(const std::string& identity) {
  return UrlRuleClientTag(identity);
}

PhiEntity UrlRuleKind::Envelope(const Entity& entity) {
  PhiEntity out;
  out.kind = PhiEntity::Kind::kUrlRule;
  out.url_rule = entity;
  return out;
}

std::optional<UrlRuleEntity> UrlRuleKind::FromEnvelope(const PhiEntity& envelope) {
  if (envelope.kind != PhiEntity::Kind::kUrlRule) {
    return std::nullopt;
  }
  return envelope.url_rule;
}

// The Incognito target is the reserved account id; Kiosk and stale
// runtime Incognito ids never publish.
std::optional<std::string> UrlRuleKind::EligibilityOwner(
    const Local& local,
    const OwnerResolver& resolve,
    const std::optional<std::string>&) {
  if (local.space_id == kIncognitoRuleTargetId) {
    return std::string(kIncognitoSpaceUuid);
  }
  if (!IsRoutableRuleTarget(local.space_id)) {
    return std::nullopt;
  }
  return resolve.sync_uuid(local.space_id);
}

std::optional<std::string> UrlRuleKind::LocalSpaceIdOf(
    const std::string& target,
    const OwnerResolver& resolve) {
  if (target.empty()) {
    return std::nullopt;
  }
  if (target == kIncognitoSpaceUuid) {
    return std::string(kIncognitoRuleTargetId);
  }
  return resolve.local_space_id(target);
}

std::optional<RuleProjection> UrlRuleKind::TransferSource(
    const Entity& entity,
    const OwnerResolver& resolve) {
  const std::string host = entity.host.get().str();
  if (host.empty()) {
    return std::nullopt;
  }
  const std::string path = entity.path_prefix.get().str();
  const std::string target = entity.target_space_uuid.get().str();
  RuleProjection out;
  out.host = host;
  out.path_prefix = path.empty() ? std::nullopt : std::optional<std::string>(path);
  out.ask = entity.ask.get().flag();
  out.content_updated_ms = StampOrNone(entity.host.get().updated_at_ms);
  out.target_owner_uuid = target;
  out.target_space_id = LocalSpaceIdOf(target, resolve);
  out.target_updated_ms =
      StampOrNone(entity.target_space_uuid.get().updated_at_ms);
  return out;
}

std::optional<UrlRuleEntity> UrlRuleKind::Project(
    const Local& local,
    const OwnerResolver& resolve,
    const std::optional<std::string>& scope,
    const std::optional<std::string>&) {
  const std::optional<std::string> target = EligibilityOwner(local, resolve, scope);
  if (!target) {
    return std::nullopt;
  }
  UrlRuleEntity entity;
  entity.rule_uuid = local.sync_id.value_or(std::string());
  entity.host.set(StringValue(local.host));
  entity.path_prefix.set(StringValue(local.path_prefix.value_or(std::string())));
  entity.ask.set(BoolValue(local.ask));
  entity.target_space_uuid.set(StringValue(*target));
  entity.rank.set(StringValue(std::string()));
  entity.created_at_ms = local.created_ms;
  entity.source = 0;
  return entity;
}

std::string UrlRuleKind::ContentSignature(const Entity& entity) {
  return UrlRuleContentSignature(entity);
}

UrlRuleEntity UrlRuleKind::Stamp(const Entity& projected,
                                 const std::optional<Entity>& baseline,
                                 const Local& local,
                                 const std::string& rank,
                                 int64_t now,
                                 int64_t hlc_max,
                                 int64_t wall_offset_ms) {
  UrlRuleEntity out = projected;
  out.rank.set(StringValue(rank));
  const int64_t content_edit = HybridClock::Corrected(
      local.content_updated_ms.value_or(local.created_ms), wall_offset_ms);
  const int64_t target_edit = HybridClock::Corrected(
      local.target_updated_ms.value_or(local.created_ms), wall_offset_ms);
  if (!baseline) {
    SetContentStamp(out, HybridClock::EditStamp(content_edit, hlc_max));
    SetStamp(out.target_space_uuid, HybridClock::EditStamp(target_edit, hlc_max));
    SetStamp(out.rank, 0);
    return out;
  }
  out.created_at_ms = MergedCreatedAtMs(out.created_at_ms, baseline->created_at_ms);
  if (baseline->source != 0) {
    out.source = baseline->source;
  }
  const bool content_changed = ContentSignature(out) != ContentSignature(*baseline);
  SetContentStamp(out, content_changed
                           ? HybridClock::EditStamp(
                                 content_edit, baseline->host.get().updated_at_ms)
                           : baseline->host.get().updated_at_ms);
  const bool target_changed = ValueSignature(out.target_space_uuid.get()) !=
                              ValueSignature(baseline->target_space_uuid.get());
  if (target_changed) {
    SetStamp(out.target_space_uuid,
             HybridClock::EditStamp(target_edit,
                                    baseline->target_space_uuid.get().updated_at_ms));
    SetStamp(out.rank, now);
  } else {
    SetStamp(out.target_space_uuid, baseline->target_space_uuid.get().updated_at_ms);
    SetStamp(out.rank, Restamped(out.rank, baseline->rank, now));
  }
  return out;
}

UrlRuleEntity UrlRuleKind::Merge(const Entity& local, const Entity& remote) {
  return MergeUrlRule(local, remote);
}

std::optional<OwnedRefusal> UrlRuleKind::Refuses(const Entity& entity,
                                                 const std::optional<Entity>&) {
  if (!IsAccountUuid(entity.rule_uuid)) {
    return OwnedRefusal::kInvalidUuid;
  }
  if (!IsLegalRank(entity.rank.get().str())) {
    return OwnedRefusal::kIllegalRank;
  }
  const std::string host = entity.host.get().str();
  if (host.empty()) {
    return OwnedRefusal::kEmptyHost;
  }
  if (host == "*" || host == "*.") {
    return OwnedRefusal::kDegenerateHost;
  }
  if (host.find('/') != std::string::npos) {
    return OwnedRefusal::kMalformedHost;
  }
  // An App rule host ("app:<bundle id>", phibrowser-mac 48d4c4a6) carries
  // the one colon this predicate accepts besides a bracketed IPv6 literal.
  // Windows stores and shows it but never matches it (docs/url-rules.md on
  // the Mac: only the Mac knows which app sent a URL). Like the IPv6 case,
  // a refusal here would recur on every peer every round.
  if (store::IsSourceAppRuleHost(host)) {
    return std::nullopt;
  }
  if (host.find(':') != std::string::npos &&
      !(host.front() == '[' && host.back() == ']')) {
    return OwnedRefusal::kMalformedHost;
  }
  return std::nullopt;
}

std::optional<RuleSignature> UrlRuleKind::SignatureOf(const Local& row,
                                                      const OwnerResolver& resolve) {
  const std::optional<std::string> owner =
      EligibilityOwner(row, resolve, std::nullopt);
  return owner ? SignatureFor(row.host, row.path_prefix, *owner, resolve)
               : std::nullopt;
}

std::optional<RuleSignature> UrlRuleKind::SignatureOf(const Entity& entity,
                                                      const OwnerResolver& resolve) {
  const std::string path = entity.path_prefix.get().str();
  return SignatureFor(entity.host.get().str(),
                      path.empty() ? std::nullopt : std::optional<std::string>(path),
                      entity.target_space_uuid.get().str(), resolve);
}

std::optional<RuleSignature> UrlRuleKind::BaselineSignature(
    const std::string& identity,
    const OwnedTable& table,
    const OwnerResolver& resolve) {
  const auto cursor = table.cursors.find(identity);
  if (cursor == table.cursors.end()) {
    return std::nullopt;
  }
  const std::optional<UrlRuleEntity> entity =
      DecodeOwned<UrlRuleKind>(cursor->second.reconciled);
  return entity ? SignatureOf(*entity, resolve) : std::nullopt;
}

bool UrlRuleKind::IsAtRest(const Local& row,
                           const OwnedCursor* cursor,
                           const OwnerResolver& resolve,
                           const std::set<std::string>& tombstones_this_page) {
  if (!row.sync_id || !cursor || !cursor->server ||
      cursor->server != cursor->reconciled || cursor->pending_apply ||
      cursor->pending_delete || cursor->pending_tombstone ||
      row.pending_local_edit || row.deleted_ms || !SignatureOf(row, resolve) ||
      (cursor->entity_id.empty() && cursor->reconciled) ||
      tombstones_this_page.count(*row.sync_id)) {
    return false;
  }
  return true;
}

std::map<std::string, int> UrlRuleKind::RankToSortOrder(
    const std::vector<Local>& siblings,
    const std::map<std::string, std::string>& ranks) {
  std::vector<const Local*> ordered;
  for (const Local& row : siblings) {
    if (!row.deleted_ms) {
      ordered.push_back(&row);
    }
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
                     return a->sync_id.value_or(a->id) < b->sync_id.value_or(b->id);
                   });
  std::map<std::string, int> out;
  for (size_t i = 0; i < ordered.size(); ++i) {
    out[ordered[i]->id] = static_cast<int>(i);
  }
  return out;
}

RuleProjection UrlRuleKind::ClearingProjection(const Local& row) {
  auto [host, path] = NormalizedRule(row.host, row.path_prefix);
  RuleProjection out;
  out.host = host;
  out.path_prefix = path;
  out.ask = row.ask;
  out.content_updated_ms = row.content_updated_ms;
  out.target_space_id = row.space_id;
  out.target_updated_ms = row.target_updated_ms;
  out.sort_order = row.sort_order;
  return out;
}

bool UrlRuleKind::ClearingProjectionMatches(const RuleProjection& row,
                                            const RuleProjection& confirmed) {
  return row.host == confirmed.host && row.path_prefix == confirmed.path_prefix &&
         row.ask == confirmed.ask &&
         row.content_updated_ms == confirmed.content_updated_ms &&
         confirmed.target_space_id && row.target_space_id == confirmed.target_space_id &&
         row.target_updated_ms == confirmed.target_updated_ms &&
         confirmed.sort_order && row.sort_order == confirmed.sort_order;
}

bool UrlRuleKind::TransferSourceUnchanged(const Local* row,
                                          const RuleProjection& source) {
  if (!row || row->deleted_ms) {
    return false;
  }
  const auto row_content = NormalizedRule(row->host, row->path_prefix);
  const auto source_content = NormalizedRule(source.host, source.path_prefix);
  if (row_content != source_content || row->ask != source.ask ||
      std::optional<std::string>(row->space_id) != source.target_space_id) {
    return false;
  }
  if (row->content_updated_ms &&
      *row->content_updated_ms > OrMin(source.content_updated_ms)) {
    return false;
  }
  if (row->target_updated_ms &&
      *row->target_updated_ms > OrMin(source.target_updated_ms)) {
    return false;
  }
  return true;
}

// --- The batch ----------------------------------------------------------------------------

std::vector<UrlRuleSyncOp> OrderUrlRuleOps(
    const std::vector<UrlRuleSyncOp>& unordered,
    const std::map<std::string, std::string>& current_space_ids) {
  using Kind = UrlRuleSyncOp::Kind;
  struct Slot {
    std::optional<UrlRuleLandingValues> create;
    std::optional<UrlRuleLandingValues> update;
    std::optional<UrlRuleLandingValues> move;
    std::optional<std::pair<std::string, int>> reorder;
    bool remove = false;
    std::optional<std::pair<std::string, std::optional<UrlRuleLandingValues>>> rekey;
  };
  std::vector<std::string> order;
  std::map<std::string, Slot> slots;
  std::set<std::string> rekeyed_local_ids;
  std::vector<UrlRuleSyncOp> passthrough;
  std::vector<UrlRuleSyncOp> transfers;
  for (const UrlRuleSyncOp& op : unordered) {
    switch (op.kind) {
      case Kind::kSoftDelete:
      case Kind::kSetMergePartner:
      case Kind::kSetContentGroup:
        passthrough.push_back(op);
        continue;
      case Kind::kTransfer:
        transfers.push_back(op);
        continue;
      default:
        break;
    }
    if (op.kind == Kind::kRekey && !rekeyed_local_ids.insert(op.local_id).second) {
      continue;  // a local row claimed twice in one page
    }
    auto [it, inserted] = slots.try_emplace(op.sync_id);
    if (inserted) {
      order.push_back(op.sync_id);
    }
    Slot& slot = it->second;
    switch (op.kind) {
      case Kind::kCreate:
        slot.create = op.values;
        break;
      case Kind::kUpdate:
        slot.update = op.values;
        break;
      case Kind::kMove:
        slot.move = op.values;
        break;
      case Kind::kReorder:
        slot.reorder = std::make_pair(op.space_id, op.sort_order);
        break;
      case Kind::kDelete:
        slot.remove = true;
        break;
      case Kind::kRekey:
        slot.rekey = std::make_pair(op.local_id, op.values);
        break;
      default:
        break;
    }
  }
  std::vector<UrlRuleSyncOp> upgrades;
  std::vector<UrlRuleSyncOp> deletes;
  for (const std::string& sync_id : order) {
    const Slot& slot = slots[sync_id];
    std::optional<UrlRuleSyncOp> merged;
    auto make = [&](Kind kind, const std::optional<UrlRuleLandingValues>& values) {
      UrlRuleSyncOp op;
      op.kind = kind;
      op.sync_id = sync_id;
      op.values = values;
      return op;
    };
    if (slot.rekey) {
      UrlRuleSyncOp op = make(Kind::kRekey,
                              slot.rekey->second    ? slot.rekey->second
                              : slot.update         ? slot.update
                              : slot.move           ? slot.move
                                                    : slot.create);
      op.local_id = slot.rekey->first;
      merged = std::move(op);
    } else if (slot.move) {
      const auto current = current_space_ids.find(sync_id);
      if (current != current_space_ids.end() &&
          current->second == slot.move->space_id) {
        if (slot.update) {
          merged = make(Kind::kUpdate, slot.update);
        } else if (slot.create) {
          merged = make(Kind::kCreate, slot.create);
        } else {
          UrlRuleSyncOp op = make(Kind::kReorder, std::nullopt);
          op.space_id = slot.move->space_id;
          op.sort_order = slot.move->sort_order;
          merged = std::move(op);
        }
      } else {
        merged = make(Kind::kMove, slot.move);
      }
    } else if (slot.create) {
      merged = make(Kind::kCreate, slot.create);
    } else if (slot.update) {
      merged = make(Kind::kUpdate, slot.update);
    } else if (slot.reorder) {
      UrlRuleSyncOp op = make(Kind::kReorder, std::nullopt);
      op.space_id = slot.reorder->first;
      op.sort_order = slot.reorder->second;
      merged = std::move(op);
    }
    if (slot.remove) {
      deletes.push_back(make(Kind::kDelete, std::nullopt));
    }
    if (merged) {
      upgrades.push_back(std::move(*merged));
    }
  }
  std::vector<UrlRuleSyncOp> out = std::move(upgrades);
  out.insert(out.end(), transfers.begin(), transfers.end());
  out.insert(out.end(), passthrough.begin(), passthrough.end());
  out.insert(out.end(), deletes.begin(), deletes.end());
  return out;
}

UrlRuleApplyError ApplyUrlRuleOps(std::vector<LocalUrlRule>* rows_out,
                                  const std::vector<UrlRuleSyncOp>& ops,
                                  const UrlRuleMergeTail& merge_tail,
                                  const std::function<std::string()>& new_id,
                                  int64_t now_ms,
                                  UrlRuleBatchOutcome* outcome) {
  using Kind = UrlRuleSyncOp::Kind;
  std::vector<LocalUrlRule> rows = *rows_out;
  UrlRuleBatchOutcome result;
  std::set<std::string> touched;
  std::set<std::string> alpha_sources;
  for (const UrlRuleSyncOp& op : ops) {
    if (op.kind == Kind::kDelete) {
      alpha_sources.insert(op.sync_id);
    }
  }
  auto upsert = [&](const UrlRuleLandingValues& values) -> UrlRuleApplyError {
    auto [host, path] = NormalizedRule(values.host, values.path_prefix);
    LocalUrlRule* row = MutableRowBySyncId(rows, values.sync_id);
    if (!row) {
      LocalUrlRule created;
      created.id = new_id();
      created.sync_id = values.sync_id;
      created.space_id = values.space_id;
      created.host = host;
      created.path_prefix = path;
      created.ask = values.ask;
      created.sort_order = values.sort_order;
      created.created_ms = values.created_ms;
      created.content_updated_ms = values.content_updated_ms;
      created.target_updated_ms = values.target_updated_ms;
      rows.push_back(std::move(created));
      return UrlRuleApplyError::kNone;
    }
    row->space_id = values.space_id;
    row->host = host;
    row->path_prefix = path;
    row->ask = values.ask;
    row->sort_order = values.sort_order;
    row->created_ms = values.created_ms;
    row->content_updated_ms = values.content_updated_ms;
    row->target_updated_ms = values.target_updated_ms;
    if (row->deleted_ms) {
      row->deleted_ms.reset();
      row->merge_partner_sync_id.reset();
    }
    return UrlRuleApplyError::kNone;
  };
  auto hard_delete = [&](const std::string& sync_id) -> std::optional<std::string> {
    std::optional<std::string> bucket;
    for (auto it = rows.begin(); it != rows.end();) {
      if (it->sync_id == sync_id) {
        if (!bucket) {
          bucket = it->space_id;
        }
        it = rows.erase(it);
      } else {
        ++it;
      }
    }
    return bucket;
  };
  std::function<UrlRuleApplyError(const UrlRuleSyncOp&)> execute =
      [&](const UrlRuleSyncOp& op) -> UrlRuleApplyError {
    switch (op.kind) {
      case Kind::kCreate:
        if (op.values) {
          upsert(*op.values);
          touched.insert(op.values->space_id);
        }
        break;
      case Kind::kUpdate: {
        if (!op.values) {
          break;
        }
        const LocalUrlRule* existing = RowBySyncId(rows, op.values->sync_id);
        const std::optional<std::string> source_bucket =
            existing ? std::optional<std::string>(existing->space_id) : std::nullopt;
        const bool enters_bucket = !existing || existing->deleted_ms;
        upsert(*op.values);
        if (enters_bucket) {
          touched.insert(op.values->space_id);
        } else if (source_bucket && *source_bucket != op.values->space_id) {
          touched.insert(*source_bucket);
          touched.insert(op.values->space_id);
        }
        break;
      }
      case Kind::kMove: {
        if (!op.values) {
          break;
        }
        const LocalUrlRule* existing = RowBySyncId(rows, op.values->sync_id);
        if (existing) {
          touched.insert(existing->space_id);
        }
        upsert(*op.values);
        touched.insert(op.values->space_id);
        break;
      }
      case Kind::kReorder:
        if (LocalUrlRule* row = MutableRowBySyncId(rows, op.sync_id)) {
          row->sort_order = op.sort_order;
          touched.insert(row->space_id);
        }
        break;
      case Kind::kDelete:
        if (!result.deferred_tombstones.count(op.sync_id)) {
          if (std::optional<std::string> bucket = hard_delete(op.sync_id)) {
            touched.insert(*bucket);
          }
        }
        break;
      case Kind::kTransfer: {
        if (alpha_sources.count(op.from_sync_id) &&
            !UrlRuleKind::TransferSourceUnchanged(
                RowBySyncId(rows, op.from_sync_id), op.source)) {
          result.deferred_tombstones.insert(op.from_sync_id);
          break;
        }
        LocalUrlRule* row = MutableRowBySyncId(rows, op.sync_id);
        if (!row) {
          ++result.transfer_superseded_by_delete;
          break;
        }
        const auto [writes_content, writes_target] =
            TransferDecision(*row, op.source, op.target_stamps);
        if (!writes_content) {
          ++result.transfer_superseded_by_delete;
        }
        if (writes_content) {
          auto [host, path] = NormalizedRule(op.source.host, op.source.path_prefix);
          row->host = host;
          row->path_prefix = path;
          row->ask = op.source.ask;
          row->content_updated_ms = op.source.content_updated_ms;
        }
        if (writes_target && op.source.target_space_id) {
          if (row->space_id != *op.source.target_space_id) {
            touched.insert(row->space_id);
            row->space_id = *op.source.target_space_id;
            touched.insert(row->space_id);
          }
          row->target_updated_ms = op.source.target_updated_ms;
        }
        if (writes_content || writes_target) {
          row->pending_local_edit = true;
          ++result.transferred;
        }
        break;
      }
      case Kind::kRekey: {
        LocalUrlRule* row = nullptr;
        for (LocalUrlRule& candidate : rows) {
          if (candidate.id == op.local_id) {
            row = &candidate;
          }
        }
        if (!row || row->deleted_ms) {
          return UrlRuleApplyError::kRowNotFound;
        }
        if (row->sync_id != op.sync_id) {
          const LocalUrlRule* owner = RowBySyncId(rows, op.sync_id);
          if (owner && owner != row) {
            return UrlRuleApplyError::kRowAlreadyMapped;
          }
          row->sync_id = op.sync_id;
        }
        if (op.values) {
          const std::string source_bucket = row->space_id;
          upsert(*op.values);
          touched.insert(op.values->space_id);
          if (source_bucket != op.values->space_id) {
            touched.insert(source_bucket);
          }
        }
        break;
      }
      case Kind::kSoftDelete:
        if (LocalUrlRule* row = MutableRowBySyncId(rows, op.sync_id)) {
          if (!row->deleted_ms) {
            row->deleted_ms = now_ms;
          }
          row->merge_partner_sync_id = op.merge_partner;
          touched.insert(row->space_id);
        }
        break;
      case Kind::kSetMergePartner:
        if (LocalUrlRule* row = MutableRowBySyncId(rows, op.sync_id)) {
          row->merge_partner_sync_id = op.merge_partner;
        }
        break;
      case Kind::kSetContentGroup:
        if (LocalUrlRule* row = MutableRowBySyncId(rows, op.sync_id)) {
          auto [host, path] = NormalizedRule(op.host, op.path_prefix);
          row->host = host;
          row->path_prefix = path;
          row->ask = op.ask;
          row->content_updated_ms = op.content_updated_ms;
        }
        break;
    }
    return UrlRuleApplyError::kNone;
  };
  for (const UrlRuleSyncOp& op : ops) {
    if (const UrlRuleApplyError error = execute(op);
        error != UrlRuleApplyError::kNone) {
      return error;
    }
  }
  if (merge_tail) {
    const UrlRuleMergeResult tail = merge_tail(rows);
    for (const UrlRuleSyncOp& op : tail.ops) {
      if (const UrlRuleApplyError error = execute(op);
          error != UrlRuleApplyError::kNone) {
        return error;
      }
    }
    touched.insert(tail.touched_buckets.begin(), tail.touched_buckets.end());
    result.collapsed = tail.collapsed;
    result.merge_changed_routing = tail.changed_routing;
  }
  for (const std::string& bucket : touched) {
    std::vector<LocalUrlRule*> live;
    for (LocalUrlRule& row : rows) {
      if (row.space_id == bucket && !row.deleted_ms) {
        live.push_back(&row);
      }
    }
    std::sort(live.begin(), live.end(), [](const LocalUrlRule* a, const LocalUrlRule* b) {
      return std::tie(a->sort_order, a->id) < std::tie(b->sort_order, b->id);
    });
    for (size_t i = 0; i < live.size(); ++i) {
      live[i]->sort_order = static_cast<int>(i);
    }
  }
  *rows_out = std::move(rows);
  *outcome = result;
  return UrlRuleApplyError::kNone;
}

UrlRuleMergeResult UrlRuleMergePass(
    const std::vector<LocalUrlRule>& rows,
    const std::set<std::string>& landed_this_page,
    const std::set<std::string>& published_identities,
    const std::map<std::string, RuleSignature>& pre_landing_signatures,
    const std::set<std::string>& at_rest,
    const std::map<std::string, UrlRuleLandingValues>& landed,
    const std::map<std::string, std::string>& rebaselined,
    const OwnedTable& table,
    bool converge_allowed,
    const OwnerResolver& resolve) {
  UrlRuleMergeResult out;
  std::vector<LocalUrlRule> live;
  for (const LocalUrlRule& row : rows) {
    if (!row.deleted_ms) {
      live.push_back(row);
    }
  }
  std::map<RuleSignature, std::vector<const LocalUrlRule*>> groups;
  for (const LocalUrlRule& row : live) {
    if (!row.sync_id) {
      continue;
    }
    if (std::optional<RuleSignature> key = UrlRuleKind::SignatureOf(row, resolve)) {
      groups[*key].push_back(&row);
    }
  }
  std::map<std::string, const LocalUrlRule*> row_of;
  for (const LocalUrlRule& row : rows) {
    if (!row.sync_id) {
      continue;
    }
    if (!row.deleted_ms || !row_of.count(*row.sync_id)) {
      row_of[*row.sync_id] = &row;
    }
  }
  std::set<std::string> candidates;
  for (const std::string& identity : at_rest) {
    const auto row = row_of.find(identity);
    if (row != row_of.end() && !row->second->pending_local_edit &&
        !row->second->deleted_ms) {
      candidates.insert(identity);
    }
  }
  std::set<std::string> asked = landed_this_page;
  for (const auto& [key, members] : groups) {
    for (const LocalUrlRule* row : members) {
      asked.insert(*row->sync_id);
    }
  }
  std::map<std::string, int64_t> content_stamps;
  for (const auto& [identity, pair] :
       EffectiveAccountStamps(landed, rebaselined, table, asked)) {
    if (pair.content) {
      content_stamps[identity] = *pair.content;
    }
  }
  Convergence convergence;
  if (converge_allowed) {
    convergence = ConvergePass(groups, candidates, content_stamps);
  }
  std::set<std::string> collapsed_ids;
  for (const auto& [loser, winner] : convergence.soft_deletes) {
    collapsed_ids.insert(loser);
  }
  std::vector<LocalUrlRule> live_after;
  for (const LocalUrlRule& row : live) {
    if (!row.sync_id || !collapsed_ids.count(*row.sync_id)) {
      live_after.push_back(row);
    }
  }
  const std::map<std::string, std::string> pointers =
      MergePointerPass(live_after, live, landed_this_page, published_identities,
                       pre_landing_signatures, resolve);

  using Kind = UrlRuleSyncOp::Kind;
  std::sort(convergence.content_group_writes.begin(),
            convergence.content_group_writes.end(),
            [](const ContentGroupWrite& a, const ContentGroupWrite& b) {
              return a.sync_id < b.sync_id;
            });
  for (const ContentGroupWrite& write : convergence.content_group_writes) {
    UrlRuleSyncOp op;
    op.kind = Kind::kSetContentGroup;
    op.sync_id = write.sync_id;
    op.host = write.host;
    op.path_prefix = write.path_prefix;
    op.ask = write.ask;
    op.content_updated_ms = write.content_updated_ms;
    out.ops.push_back(std::move(op));
  }
  std::sort(convergence.soft_deletes.begin(), convergence.soft_deletes.end());
  for (const auto& [loser, winner] : convergence.soft_deletes) {
    UrlRuleSyncOp op;
    op.kind = Kind::kSoftDelete;
    op.sync_id = loser;
    op.merge_partner = winner;
    out.ops.push_back(std::move(op));
  }
  std::sort(convergence.cleared_partners.begin(), convergence.cleared_partners.end());
  for (const std::string& identity : convergence.cleared_partners) {
    UrlRuleSyncOp op;
    op.kind = Kind::kSetMergePartner;
    op.sync_id = identity;
    out.ops.push_back(std::move(op));
  }
  for (const auto& [identity, partner] : pointers) {
    UrlRuleSyncOp op;
    op.kind = Kind::kSetMergePartner;
    op.sync_id = identity;
    op.merge_partner = partner;
    out.ops.push_back(std::move(op));
  }
  out.collapsed = convergence.collapsed;
  out.touched_buckets = convergence.touched_buckets;
  out.changed_routing =
      !convergence.soft_deletes.empty() || !convergence.content_group_writes.empty();
  return out;
}

bool ClearRulePendingLocalEdit(std::vector<LocalUrlRule>* rows,
                               const std::string& sync_id,
                               const RuleProjection& confirmed,
                               bool clears_partner) {
  LocalUrlRule* row = MutableRowBySyncId(*rows, sync_id);
  if (!row) {
    return false;
  }
  bool changed = false;
  if (clears_partner && row->merge_partner_sync_id) {
    row->merge_partner_sync_id.reset();
    changed = true;
  }
  if (!row->pending_local_edit ||
      !UrlRuleKind::ClearingProjectionMatches(UrlRuleKind::ClearingProjection(*row),
                                              confirmed)) {
    return changed;
  }
  row->pending_local_edit = false;
  return true;
}

// --- Round queries (URLRuleSignatureQueries) ----------------------------------------------

namespace {

std::map<RuleSignature, std::vector<LocalUrlRule>> SignatureIndex(
    const std::vector<LocalUrlRule>& rows,
    const OwnerResolver& resolve) {
  std::map<RuleSignature, std::vector<LocalUrlRule>> out;
  for (const LocalUrlRule& row : rows) {
    if (row.deleted_ms) {
      continue;
    }
    if (std::optional<RuleSignature> key = UrlRuleKind::SignatureOf(row, resolve)) {
      out[*key].push_back(row);
    }
  }
  for (auto& [key, members] : out) {
    std::sort(members.begin(), members.end(),
              [](const LocalUrlRule& a, const LocalUrlRule& b) {
                return std::make_pair(a.sync_id.value_or(""), a.id) <
                       std::make_pair(b.sync_id.value_or(""), b.id);
              });
  }
  return out;
}

std::set<std::string> PendingLocalEditIdentities(
    const std::vector<LocalUrlRule>& rows,
    const OwnerResolver& resolve) {
  std::set<std::string> out;
  for (const LocalUrlRule& row : rows) {
    if (!row.deleted_ms && row.pending_local_edit && row.sync_id &&
        UrlRuleKind::SignatureOf(row, resolve)) {
      out.insert(*row.sync_id);
    }
  }
  return out;
}

std::set<std::string> UnpublishedIdentities(const std::vector<LocalUrlRule>& rows,
                                            const OwnedTable& table,
                                            const OwnerResolver& resolve) {
  std::set<std::string> out;
  for (const LocalUrlRule& row : rows) {
    if (row.deleted_ms || !row.sync_id) {
      continue;
    }
    const auto cursor = table.cursors.find(*row.sync_id);
    if (cursor == table.cursors.end() || !cursor->second.server ||
        !cursor->second.reconciled ||
        *cursor->second.server == *cursor->second.reconciled ||
        !UrlRuleKind::SignatureOf(row, resolve)) {
      continue;
    }
    out.insert(*row.sync_id);
  }
  return out;
}

std::map<std::string, std::string> MergePartners(
    const std::vector<LocalUrlRule>& rows,
    const OwnedTable& table,
    const OwnerResolver& resolve,
    const std::set<std::string>& tombstones_this_page) {
  std::map<std::string, const LocalUrlRule*> resting_by_sync_id;
  std::map<RuleSignature, std::vector<const LocalUrlRule*>> resting_by_signature;
  for (const LocalUrlRule& row : rows) {
    if (row.deleted_ms || !row.sync_id) {
      continue;
    }
    const auto cursor = table.cursors.find(*row.sync_id);
    if (!UrlRuleKind::IsAtRest(row,
                               cursor == table.cursors.end() ? nullptr : &cursor->second,
                               resolve, tombstones_this_page)) {
      continue;
    }
    const std::optional<RuleSignature> key = UrlRuleKind::SignatureOf(row, resolve);
    if (!key) {
      continue;
    }
    resting_by_sync_id[*row.sync_id] = &row;
    resting_by_signature[*key].push_back(&row);
  }
  for (auto& [key, members] : resting_by_signature) {
    std::sort(members.begin(), members.end(),
              [](const LocalUrlRule* a, const LocalUrlRule* b) {
                return a->sync_id.value_or("") < b->sync_id.value_or("");
              });
  }
  std::map<std::string, std::string> out;
  for (const LocalUrlRule& x : rows) {
    if (!x.sync_id) {
      continue;
    }
    const std::string& x_id = *x.sync_id;
    if (x.merge_partner_sync_id && *x.merge_partner_sync_id != x_id &&
        resting_by_sync_id.count(*x.merge_partner_sync_id)) {
      out[x_id] = *x.merge_partner_sync_id;
      continue;
    }
    const std::optional<RuleSignature> baseline =
        UrlRuleKind::BaselineSignature(x_id, table, resolve);
    if (!baseline) {
      continue;
    }
    const auto group = resting_by_signature.find(*baseline);
    if (group == resting_by_signature.end()) {
      continue;
    }
    for (const LocalUrlRule* partner : group->second) {
      if (partner->sync_id != x.sync_id) {
        out[x_id] = *partner->sync_id;
        break;
      }
    }
  }
  return out;
}

std::set<std::string> PartnerNotAtRest(const OwnedTable& table,
                                       const std::vector<LocalUrlRule>& rows,
                                       const OwnerResolver& resolve,
                                       const std::set<std::string>& tombstones_this_page) {
  const std::map<std::string, std::string> partners =
      MergePartners(rows, table, resolve, tombstones_this_page);
  std::set<std::string> live_sync_ids;
  std::map<RuleSignature, std::vector<std::string>> live_by_signature;
  for (const LocalUrlRule& row : rows) {
    if (row.deleted_ms || !row.sync_id) {
      continue;
    }
    live_sync_ids.insert(*row.sync_id);
    if (std::optional<RuleSignature> key = UrlRuleKind::SignatureOf(row, resolve)) {
      live_by_signature[*key].push_back(*row.sync_id);
    }
  }
  std::set<std::string> out;
  for (const LocalUrlRule& row : rows) {
    if (!row.sync_id || partners.count(*row.sync_id)) {
      continue;
    }
    const std::string& identity = *row.sync_id;
    bool has_partner_row = row.merge_partner_sync_id &&
                           *row.merge_partner_sync_id != identity &&
                           live_sync_ids.count(*row.merge_partner_sync_id);
    if (!has_partner_row) {
      if (const std::optional<RuleSignature> baseline =
              UrlRuleKind::BaselineSignature(identity, table, resolve)) {
        const auto group = live_by_signature.find(*baseline);
        if (group != live_by_signature.end()) {
          has_partner_row = std::any_of(
              group->second.begin(), group->second.end(),
              [&](const std::string& other) { return other != identity; });
        }
      }
    }
    if (has_partner_row) {
      out.insert(identity);
    }
  }
  return out;
}

}  // namespace

// --- The kind -----------------------------------------------------------------------------

UrlRuleOwnedKind::UrlRuleOwnedKind(UrlRuleLocalAccess& access,
                                   OwnedTableStore& store)
    : access_(access), store_(store) {}

UrlRuleOwnedKind::~UrlRuleOwnedKind() = default;

const char* UrlRuleOwnedKind::label() const {
  return "urlrules";
}

const char* UrlRuleOwnedKind::entity_name() const {
  return kUrlRuleEntityName;
}

OwnedKindId UrlRuleOwnedKind::kind_id() const {
  return OwnedKindId::kUrlRules;
}

OwnedTableStore& UrlRuleOwnedKind::store() {
  return store_;
}

bool UrlRuleOwnedKind::tombstone_yields_to_local_edits() const {
  return UrlRuleKind::kTombstoneYieldsToLocalEdits;
}

// The merge tail runs every page, with or without arrivals.
bool UrlRuleOwnedKind::lands_empty_batch() const {
  return true;
}

std::optional<std::string> UrlRuleOwnedKind::Identity(const PhiEntity& envelope) const {
  const std::optional<UrlRuleEntity> entity = UrlRuleKind::FromEnvelope(envelope);
  if (!entity || entity->rule_uuid.empty()) {
    return std::nullopt;
  }
  return entity->rule_uuid;
}

std::string UrlRuleOwnedKind::ClientTag(const std::string& identity) const {
  return UrlRuleKind::ClientTag(identity);
}

std::vector<std::string> UrlRuleOwnedKind::Owners(const std::string& payload) const {
  const std::optional<UrlRuleEntity> entity = DecodeOwned<UrlRuleKind>(payload);
  return entity ? UrlRuleKind::OwnerUuids(*entity) : std::vector<std::string>();
}

void UrlRuleOwnedKind::Reload(const std::vector<LocalUrlRule>& rows) {
  rows_ = rows;
  page_reload_failed_ = false;
}

std::vector<LocalUrlRule> UrlRuleOwnedKind::Live() const {
  std::vector<LocalUrlRule> out;
  for (const LocalUrlRule& row : rows_) {
    if (!row.deleted_ms) {
      out.push_back(row);
    }
  }
  return out;
}

bool UrlRuleOwnedKind::BeginRound() {
  std::vector<LocalUrlRule> rows;
  if (!access_.AllRules(&rows)) {
    Reload({});
    return false;
  }
  Reload(rows);
  return true;
}

std::set<std::string> UrlRuleOwnedKind::LocalIdentities() {
  std::set<std::string> out;
  for (const LocalUrlRule& row : rows_) {
    if (row.sync_id) {
      out.insert(*row.sync_id);
    }
  }
  return out;
}

OwnedSnapshotBytes UrlRuleOwnedKind::Snapshot(const OwnedTable& table,
                                              const OwnedOwnerMaps& maps,
                                              int64_t now,
                                              int64_t hlc_max,
                                              int64_t wall_offset_ms) {
  OwnedSnapshotBytes out;
  const OwnerResolver resolve = maps.Resolver();
  publish_baseline_.clear();
  publish_resolver_ = resolve;
  const std::vector<LocalUrlRule> live = Live();
  std::map<std::string, const LocalUrlRule*> row_of;
  for (const LocalUrlRule& row : live) {
    if (row.sync_id) {
      row_of.try_emplace(*row.sync_id, &row);
    }
  }
  const OwnedSnapshotResult<UrlRuleEntity> result = OwnedSnapshot<UrlRuleKind>(
      live, table, resolve, std::nullopt, now, hlc_max, wall_offset_ms);
  out.skipped_unmapped_owner = result.skipped_unmapped_owner;
  out.skipped_ineligible_owner = result.skipped_ineligible_owner;
  for (const auto& [identity, entity] : result.entities) {
    out.entities[identity] = EncodeOwned<UrlRuleKind>(entity);
    if (const auto row = row_of.find(identity); row != row_of.end()) {
      publish_baseline_[identity] = UrlRuleKind::ClearingProjection(*row->second);
    }
  }
  for (const LocalUrlRule& row : live) {
    if (!row.sync_id) {
      continue;
    }
    if (std::optional<std::string> owner =
            UrlRuleKind::EligibilityOwner(row, resolve, std::nullopt)) {
      out.owner_uuids[*row.sync_id] = *owner;
    }
  }
  return out;
}

// Soft-deleted rows with a baseline are explicit deletions (a user delete
// of the Space took the owner gate with it); a partner not at rest defers.
bool UrlRuleOwnedKind::Tombstones(const OwnedTable& table,
                                  const OwnedOwnerMaps& maps,
                                  int64_t now,
                                  OwnedTombstoneResult* out) {
  std::vector<LocalUrlRule> rows;
  if (!access_.AllRules(&rows)) {
    return false;
  }
  const OwnerResolver resolve = maps.Resolver();
  std::vector<LocalUrlRule> locals;
  std::set<std::string> explicit_deletions;
  for (const LocalUrlRule& row : rows) {
    if (!row.sync_id) {
      continue;
    }
    if (!row.deleted_ms) {
      locals.push_back(row);
      continue;
    }
    const auto cursor = table.cursors.find(*row.sync_id);
    if (cursor != table.cursors.end() && cursor->second.reconciled) {
      explicit_deletions.insert(*row.sync_id);
    }
  }
  std::set<std::string> deferred;
  for (const std::string& identity : PartnerNotAtRest(table, rows, resolve, {})) {
    const auto cursor = table.cursors.find(identity);
    if (cursor != table.cursors.end() && cursor->second.pending_apply) {
      deferred.insert(identity);
    }
  }
  *out = OwnedTombstones<UrlRuleKind>(locals, table, resolve, std::nullopt, now,
                                      {}, explicit_deletions, deferred);
  return true;
}

OwnedParkedClaimResult UrlRuleOwnedKind::RetryParkedClaims(
    const std::map<std::string, ParkedOwnedItem>&,
    const OwnedOwnerMaps&,
    int64_t) {
  return {};
}

OwnedPlanOutput UrlRuleOwnedKind::Plan(const OwnedPlanInput& input) {
  OwnedPlanOutput out;
  const OwnerResolver resolve = input.maps.Resolver();
  std::vector<OwnedArrival<UrlRuleEntity>> arrivals;
  for (const OwnedPlanInput::Arrival& item : input.arrivals) {
    std::optional<UrlRuleEntity> entity = DecodeOwned<UrlRuleKind>(item.payload);
    if (!entity) {
      continue;
    }
    out.server_bytes[entity->rule_uuid] = item.payload;
    const std::string target = entity->target_space_uuid.get().str();
    if (!target.empty()) {
      out.deferred_owners.insert(target);
    }
    arrivals.push_back({std::move(*entity), item.entity_id, item.version});
  }
  // A peer's host / path in a non-normal form is normalized as it lands and
  // republished.
  std::set<std::string> normalized;
  for (auto& item : arrivals) {
    const std::string wire_host = item.entity.host.get().str();
    const std::string wire_path = item.entity.path_prefix.get().str();
    auto [host, path] = NormalizedRule(
        wire_host, wire_path.empty() ? std::nullopt : std::optional<std::string>(wire_path));
    const std::string new_path = path.value_or(std::string());
    if (host == wire_host && new_path == wire_path) {
      continue;
    }
    item.entity.host.mut().SetString(host);
    item.entity.path_prefix.mut().SetString(new_path);
    if (!item.entity.rule_uuid.empty()) {
      normalized.insert(item.entity.rule_uuid);
    }
  }
  out.normalized = static_cast<int>(normalized.size());

  const std::vector<LocalUrlRule> live = Live();
  OwnedPlanContext context;
  context.tombstoned_identities = input.tombstoned;

  // Claims: an arrival whose signature matches a never-published local rule
  // takes that row over (the local identity retires).
  std::set<std::string> local_identities = LocalIdentities();
  std::map<std::string, UrlRuleEntity> candidates;
  for (const auto& [identity, item] : input.parked) {
    if (std::optional<UrlRuleEntity> entity = DecodeOwned<UrlRuleKind>(item.payload)) {
      candidates[identity] = std::move(*entity);
    }
  }
  for (const auto& item : arrivals) {
    candidates[item.entity.rule_uuid] = item.entity;
  }
  std::set<std::string> arriving;
  for (const auto& [identity, entity] : candidates) {
    arriving.insert(identity);
  }
  std::set<std::string> claim_must_republish;
  std::map<RuleSignature, std::vector<std::pair<std::string, UrlRuleEntity>>> remote_groups;
  for (const auto& [identity, entity] : candidates) {
    if (identity.empty() || local_identities.count(identity)) {
      continue;
    }
    if (std::optional<RuleSignature> key = UrlRuleKind::SignatureOf(entity, resolve)) {
      remote_groups[*key].push_back({identity, entity});
    }
  }
  if (!remote_groups.empty()) {
    const std::map<RuleSignature, std::vector<LocalUrlRule>> index =
        SignatureIndex(live, resolve);
    for (auto& [key, remotes] : remote_groups) {
      std::sort(remotes.begin(), remotes.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
      std::vector<LocalUrlRule> ordered_local;
      if (const auto it = index.find(key); it != index.end()) {
        for (const LocalUrlRule& row : it->second) {
          if (!row.sync_id || arriving.count(*row.sync_id)) {
            continue;
          }
          const auto cursor = input.table.cursors.find(*row.sync_id);
          if (cursor == input.table.cursors.end() ||
              (cursor->second.entity_id.empty() && !cursor->second.server &&
               !cursor->second.reconciled)) {
            ordered_local.push_back(row);
          }
        }
      }
      for (size_t offset = 0; offset < remotes.size() && offset < ordered_local.size();
           ++offset) {
        const LocalUrlRule& row = ordered_local[offset];
        const auto& [remote_identity, remote] = remotes[offset];
        const std::optional<UrlRuleEntity> projected =
            UrlRuleKind::Project(row, resolve, std::nullopt, std::nullopt);
        if (!projected) {
          continue;
        }
        UrlRuleEntity local = UrlRuleKind::Stamp(
            *projected, std::nullopt, row, UrlRuleKind::Rank(remote), input.now, 0,
            input.wall_offset_ms);
        local.rule_uuid = remote_identity;
        const UrlRuleEntity merged = UrlRuleKind::Merge(local, remote);
        const std::string merged_bytes = EncodeOwned<UrlRuleKind>(merged);
        context.pairs[remote_identity] = row.id;
        out.retired_identities[remote_identity] = *row.sync_id;
        context.adopted_merges[remote_identity] = merged_bytes;
        if (merged_bytes != EncodeOwned<UrlRuleKind>(local)) {
          context.adopted_field_writes.insert(remote_identity);
        }
        if (merged_bytes != EncodeOwned<UrlRuleKind>(remote)) {
          claim_must_republish.insert(remote_identity);
        }
      }
    }
  }
  out.claimed_local_ids = context.pairs;
  out.adopted = static_cast<int>(context.pairs.size());

  // Local projections for this page's identities.
  std::set<std::string> domain = input.tombstoned;
  for (const auto& [identity, item] : input.parked) {
    domain.insert(identity);
  }
  for (const auto& item : arrivals) {
    domain.insert(item.entity.rule_uuid);
  }
  for (const LocalUrlRule& row : live) {
    if (!row.sync_id || !domain.count(*row.sync_id) ||
        context.local_projections.count(*row.sync_id)) {
      continue;
    }
    const auto cursor = input.table.cursors.find(*row.sync_id);
    if (cursor == input.table.cursors.end()) {
      continue;
    }
    const std::optional<UrlRuleEntity> baseline =
        DecodeOwned<UrlRuleKind>(cursor->second.reconciled);
    const std::optional<UrlRuleEntity> projected =
        UrlRuleKind::Project(row, resolve, std::nullopt, std::nullopt);
    if (!baseline || !projected) {
      continue;
    }
    context.local_projections[*row.sync_id] = EncodeOwned<UrlRuleKind>(
        UrlRuleKind::Stamp(*projected, baseline, row, UrlRuleKind::Rank(*baseline),
                           input.now, 0, input.wall_offset_ms));
  }
  context.pending_local_edits = PendingLocalEditIdentities(live, resolve);
  context.unpublished = UnpublishedIdentities(live, input.table, resolve);
  context.merge_partners = MergePartners(rows_, input.table, resolve, input.tombstoned);
  context.partner_not_at_rest =
      PartnerNotAtRest(input.table, rows_, resolve, input.tombstoned);
  std::map<RuleSignature, std::vector<std::string>> live_by_signature;
  for (const LocalUrlRule& row : live) {
    if (!row.sync_id) {
      continue;
    }
    const std::optional<RuleSignature> key = UrlRuleKind::SignatureOf(row, resolve);
    if (!key) {
      continue;
    }
    context.local_signatures[*row.sync_id] = *key;
    live_by_signature[*key].push_back(*row.sync_id);
    const auto cursor = input.table.cursors.find(*row.sync_id);
    if (UrlRuleKind::IsAtRest(
            row, cursor == input.table.cursors.end() ? nullptr : &cursor->second,
            resolve, input.tombstoned)) {
      out.at_rest_identities.insert(*row.sync_id);
    }
  }
  out.plan = OwnedPlanFor<UrlRuleKind>(arrivals, input.parked, input.table, resolve,
                                       context);
  out.must_republish = normalized;
  out.must_republish.insert(out.plan.must_republish.begin(),
                            out.plan.must_republish.end());
  out.must_republish.insert(claim_must_republish.begin(), claim_must_republish.end());

  // Diagnostics: a yield with no duplicate anchored anywhere.
  std::map<std::string, const LocalUrlRule*> row_by_sync_id;
  for (const LocalUrlRule& row : rows_) {
    if (row.sync_id) {
      row_by_sync_id.try_emplace(*row.sync_id, &row);
    }
  }
  for (const std::string& identity : out.plan.yielded_tombstones) {
    const auto row = row_by_sync_id.find(identity);
    if (row != row_by_sync_id.end() && row->second->merge_partner_sync_id) {
      continue;
    }
    const std::optional<RuleSignature> baseline =
        UrlRuleKind::BaselineSignature(identity, input.table, resolve);
    bool anchored = false;
    if (baseline) {
      const auto group = live_by_signature.find(*baseline);
      anchored = group != live_by_signature.end() &&
                 std::any_of(group->second.begin(), group->second.end(),
                             [&](const std::string& other) { return other != identity; });
    }
    if (!anchored) {
      ++out.yield_no_partner;
    }
  }
  for (const OwnedApplyStep& step : out.plan.steps) {
    if (step.kind == OwnedStepKind::kTransfer) {
      out.server_bytes.erase(step.identity);
    }
  }
  return out;
}

OwnedLandingOutcome UrlRuleOwnedKind::Land(const OwnedLandingInput& input) {
  OwnedLandingOutcome out;
  const OwnerResolver resolve = input.maps.Resolver();
  const std::vector<LocalUrlRule> live = Live();

  std::map<std::string, LocalUrlRule> row_of;
  for (const LocalUrlRule& row : live) {
    if (row.sync_id) {
      row_of.try_emplace(*row.sync_id, row);
    }
  }
  for (const LocalUrlRule& row : rows_) {
    if (row.sync_id) {
      row_of.try_emplace(*row.sync_id, row);
    }
  }
  std::map<std::string, LocalUrlRule> row_by_id;
  for (const LocalUrlRule& row : rows_) {
    row_by_id[row.id] = row;
  }
  std::map<std::string, std::string> rank_of;
  for (const auto& [identity, cursor] : input.table.cursors) {
    if (std::optional<UrlRuleEntity> entity = DecodeOwned<UrlRuleKind>(cursor.reconciled)) {
      rank_of[identity] = UrlRuleKind::Rank(*entity);
    }
  }

  struct Landing {
    UrlRuleEntity entity;
    std::string payload;
    std::string space_id;
    std::optional<LocalUrlRule> row;
    bool moves = false;
    bool writes_content = false;
    std::optional<std::string> claims_local_id;
  };
  std::vector<std::string> order;
  std::map<std::string, Landing> landing;
  std::vector<std::string> delete_identities;
  std::vector<OwnedApplyStep> transfers;

  for (const OwnedApplyStep& step : input.steps) {
    const std::string& identity = step.identity;
    std::optional<std::string> claimed_local_id;
    std::optional<LocalUrlRule> claimed_row;
    if (step.kind == OwnedStepKind::kDelete) {
      delete_identities.push_back(identity);
      continue;
    }
    if (step.kind == OwnedStepKind::kTransfer) {
      transfers.push_back(step);
      continue;
    }
    if (step.kind == OwnedStepKind::kClaim) {
      const auto local_id = input.claimed_local_ids.find(identity);
      const auto row = local_id == input.claimed_local_ids.end()
                           ? row_by_id.end()
                           : row_by_id.find(local_id->second);
      if (row == row_by_id.end() || row->second.deleted_ms) {
        out.refused.insert(identity);
        landing.erase(identity);
        continue;
      }
      claimed_local_id = local_id->second;
      claimed_row = row->second;
    }
    if (out.parked.count(identity) || out.refused.count(identity)) {
      continue;
    }
    const std::optional<UrlRuleEntity> entity =
        step.payload ? DecodeOwned<UrlRuleKind>(*step.payload) : std::nullopt;
    if (!entity) {
      out.refused.insert(identity);
      landing.erase(identity);
      continue;
    }
    std::string target;
    if (step.kind == OwnedStepKind::kMove) {
      if (!step.new_owner_uuid) {
        out.refused.insert(identity);
        landing.erase(identity);
        continue;
      }
      target = *step.new_owner_uuid;
    } else {
      target = UrlRuleKind::TargetOwnerUuid(*entity).value_or(std::string());
    }
    const std::optional<std::string> space_id =
        UrlRuleKind::LocalSpaceIdOf(target, resolve);
    if (!space_id) {
      out.parked.insert(identity);
      landing.erase(identity);
      continue;
    }
    auto [it, inserted] = landing.try_emplace(identity);
    if (inserted) {
      order.push_back(identity);
      it->second.row = claimed_row;
      if (!claimed_row) {
        if (const auto row = row_of.find(identity); row != row_of.end()) {
          it->second.row = row->second;
        }
      }
      it->second.claims_local_id = claimed_local_id;
    }
    it->second.entity = *entity;
    it->second.payload = *step.payload;
    it->second.space_id = *space_id;
    if (step.kind == OwnedStepKind::kMove) {
      it->second.moves = true;
    } else if (step.kind != OwnedStepKind::kClaim) {
      it->second.writes_content = true;
    }
    rank_of[identity] = UrlRuleKind::Rank(*entity);
  }
  std::vector<std::string> active;
  for (const std::string& identity : order) {
    if (landing.count(identity)) {
      active.push_back(identity);
    }
  }

  using Kind = UrlRuleSyncOp::Kind;
  std::vector<UrlRuleSyncOp> delete_ops;
  std::set<std::string> deleted_with_row;
  std::set<std::string> touched;
  for (const std::string& identity : delete_identities) {
    const auto row = row_of.find(identity);
    if (row == row_of.end()) {
      out.landed.insert(identity);
      out.deleted.insert(identity);
      continue;
    }
    UrlRuleSyncOp op;
    op.kind = Kind::kDelete;
    op.sync_id = identity;
    delete_ops.push_back(std::move(op));
    deleted_with_row.insert(identity);
    touched.insert(row->second.space_id);
  }
  for (const std::string& identity : active) {
    const Landing& item = landing[identity];
    if (item.row) {
      if (item.row->space_id != item.space_id) {
        touched.insert(item.row->space_id);
        touched.insert(item.space_id);
      } else if (item.moves || item.row->deleted_ms || item.claims_local_id) {
        touched.insert(item.space_id);
      }
    } else {
      touched.insert(item.space_id);
    }
  }

  // Wire ranks → sort orders in every touched bucket.
  std::map<std::string, int> sort_order_of;
  std::set<std::string> claimed_local_ids;
  for (const std::string& identity : active) {
    if (landing[identity].claims_local_id) {
      claimed_local_ids.insert(*landing[identity].claims_local_id);
    }
  }
  for (const std::string& bucket : touched) {
    std::vector<LocalUrlRule> members;
    for (const LocalUrlRule& row : live) {
      if (row.space_id != bucket || claimed_local_ids.count(row.id)) {
        continue;
      }
      if (row.sync_id) {
        if (deleted_with_row.count(*row.sync_id)) {
          continue;
        }
        const auto item = landing.find(*row.sync_id);
        if (item != landing.end() && item->second.space_id != bucket) {
          continue;
        }
      }
      members.push_back(row);
    }
    std::set<std::string> present;
    for (const LocalUrlRule& row : members) {
      if (row.sync_id) {
        present.insert(*row.sync_id);
      }
    }
    for (const std::string& identity : active) {
      const Landing& item = landing[identity];
      if (item.space_id != bucket || present.count(identity)) {
        continue;
      }
      present.insert(identity);
      LocalUrlRule row;
      if (item.row) {
        row = *item.row;
        row.space_id = bucket;
        row.deleted_ms.reset();
        row.sync_id = identity;
      } else {
        row.id = identity;
        row.sync_id = identity;
        row.space_id = bucket;
        row.host = item.entity.host.get().str();
        row.sort_order = INT32_MAX;
      }
      members.push_back(row);
    }
    const std::map<std::string, int> projected =
        UrlRuleKind::RankToSortOrder(members, rank_of);
    for (const LocalUrlRule& row : members) {
      const auto position = projected.find(row.id);
      if (row.sync_id && position != projected.end()) {
        sort_order_of[*row.sync_id] = position->second;
      }
    }
  }

  std::vector<UrlRuleSyncOp> ops;
  std::map<std::string, std::string> payload_of;
  std::map<std::string, UrlRuleLandingValues> landed_values;
  for (const std::string& identity : active) {
    const Landing& item = landing[identity];
    payload_of[identity] = item.payload;
    const std::string wire_path = item.entity.path_prefix.get().str();
    UrlRuleLandingValues values;
    values.sync_id = identity;
    values.space_id = item.space_id;
    values.host = item.entity.host.get().str();
    values.path_prefix =
        wire_path.empty() ? std::nullopt : std::optional<std::string>(wire_path);
    values.ask = item.entity.ask.get().flag();
    const auto order_it = sort_order_of.find(identity);
    values.sort_order = order_it != sort_order_of.end()
                            ? order_it->second
                            : (item.row ? item.row->sort_order : 0);
    values.created_ms = item.entity.created_at_ms;
    values.content_updated_ms = item.entity.host.get().updated_at_ms;
    values.target_updated_ms = item.entity.target_space_uuid.get().updated_at_ms;
    landed_values[identity] = values;
    auto op_with = [&](Kind kind) {
      UrlRuleSyncOp op;
      op.kind = kind;
      op.sync_id = identity;
      op.values = values;
      return op;
    };
    if (!item.row) {
      ops.push_back(op_with(Kind::kCreate));
      continue;
    }
    if (item.claims_local_id) {
      UrlRuleSyncOp rekey;
      rekey.kind = Kind::kRekey;
      rekey.sync_id = identity;
      rekey.local_id = *item.claims_local_id;
      ops.push_back(std::move(rekey));
      const bool repositioned =
          order_it != sort_order_of.end() && order_it->second != item.row->sort_order;
      if (item.writes_content || repositioned) {
        ops.push_back(op_with(Kind::kUpdate));
      }
      continue;
    }
    if (item.moves || item.row->space_id != item.space_id) {
      ops.push_back(op_with(Kind::kMove));
    }
    if (item.writes_content) {
      ops.push_back(op_with(Kind::kUpdate));
    }
  }
  for (const auto& [identity, position] : sort_order_of) {
    if (landing.count(identity)) {
      continue;
    }
    const auto row = row_of.find(identity);
    if (row == row_of.end() || row->second.deleted_ms ||
        row->second.sort_order == position) {
      continue;
    }
    UrlRuleSyncOp op;
    op.kind = Kind::kReorder;
    op.sync_id = identity;
    op.space_id = row->second.space_id;
    op.sort_order = position;
    ops.push_back(std::move(op));
  }
  ops.insert(ops.end(), delete_ops.begin(), delete_ops.end());

  std::set<std::string> landed_this_page;
  std::set<std::string> transfer_targets;
  for (const OwnedApplyStep& step : input.steps) {
    switch (step.kind) {
      case OwnedStepKind::kClaim:
      case OwnedStepKind::kCreate:
      case OwnedStepKind::kMove:
      case OwnedStepKind::kUpdate:
        landed_this_page.insert(step.identity);
        break;
      case OwnedStepKind::kTransfer:
        transfer_targets.insert(step.transfer_to);
        break;
      case OwnedStepKind::kDelete:
        break;
    }
  }
  std::set<std::string> merge_candidates;
  for (const std::string& identity : input.at_rest_identities) {
    if (!transfer_targets.count(identity)) {
      merge_candidates.insert(identity);
    }
  }
  std::set<std::string> published;
  for (const auto& [identity, cursor] : input.table.cursors) {
    if (cursor.server) {
      published.insert(identity);
    }
  }
  std::set<std::string> stamp_identities = landed_this_page;
  stamp_identities.insert(transfer_targets.begin(), transfer_targets.end());
  for (const LocalUrlRule& row : live) {
    if (row.sync_id) {
      stamp_identities.insert(*row.sync_id);
    }
  }
  const std::map<std::string, UrlRuleEffectiveStamps> account_stamps =
      EffectiveAccountStamps(landed_values, input.rebaselined, input.table,
                             stamp_identities);
  std::sort(transfers.begin(), transfers.end(),
            [](const OwnedApplyStep& a, const OwnedApplyStep& b) {
              return a.identity < b.identity;
            });
  for (const OwnedApplyStep& transfer : transfers) {
    if (!transfer.transfer_source) {
      continue;
    }
    UrlRuleSyncOp op;
    op.kind = Kind::kTransfer;
    op.from_sync_id = transfer.identity;
    op.sync_id = transfer.transfer_to;
    op.source = *transfer.transfer_source;
    if (const auto stamps = account_stamps.find(transfer.transfer_to);
        stamps != account_stamps.end()) {
      op.target_stamps = stamps->second;
    }
    ops.push_back(std::move(op));
  }
  const OwnedTable table = input.table;
  const std::map<std::string, RuleSignature> pre_landing = input.pre_landing_signatures;
  const std::map<std::string, std::string> rebaselined = input.rebaselined;
  const bool converge_allowed = input.converge_allowed;
  const UrlRuleMergeTail merge_tail =
      [landed_this_page, published, pre_landing, merge_candidates, landed_values,
       rebaselined, table, converge_allowed,
       resolve](const std::vector<LocalUrlRule>& rows) {
        return UrlRuleMergePass(rows, landed_this_page, published, pre_landing,
                                merge_candidates, landed_values, rebaselined, table,
                                converge_allowed, resolve);
      };

  std::set<std::string> identities(active.begin(), active.end());
  identities.insert(deleted_with_row.begin(), deleted_with_row.end());
  for (const OwnedApplyStep& transfer : transfers) {
    identities.insert(transfer.identity);
  }
  std::map<std::string, std::string> current_space_ids;
  for (const LocalUrlRule& row : live) {
    if (row.sync_id) {
      current_space_ids.try_emplace(*row.sync_id, row.space_id);
    }
  }
  for (const LocalUrlRule& row : rows_) {
    if (row.sync_id) {
      current_space_ids.try_emplace(*row.sync_id, row.space_id);
    }
  }
  const std::vector<UrlRuleSyncOp> batch = OrderUrlRuleOps(ops, current_space_ids);
  bool failed = false;
  UrlRuleBatchOutcome batch_outcome;
  const UrlRuleApplyError error =
      access_.Apply(batch, merge_tail, &failed, &batch_outcome);
  if (failed || error != UrlRuleApplyError::kNone) {
    (error == UrlRuleApplyError::kRowAlreadyMapped ? out.refused : out.parked)
        .insert(identities.begin(), identities.end());
    return out;
  }
  out.collapsed = batch_outcome.collapsed;
  out.transferred = batch_outcome.transferred;
  out.superseded_by_delete = batch_outcome.transfer_superseded_by_delete;
  out.deferred_tombstones = batch_outcome.deferred_tombstones;

  std::vector<LocalUrlRule> after;
  const bool reloaded = access_.CachedRules(&after);
  std::set<std::string> live_after;
  for (const LocalUrlRule& row : after) {
    if (row.sync_id && !row.deleted_ms) {
      live_after.insert(*row.sync_id);
    }
  }
  for (const std::string& identity : active) {
    if (!live_after.count(identity)) {
      out.parked.insert(identity);
      continue;
    }
    out.landed.insert(identity);
    out.reconciled[identity] = payload_of[identity];
  }
  for (const std::string& identity : deleted_with_row) {
    if (batch_outcome.deferred_tombstones.count(identity)) {
      continue;
    }
    if (live_after.count(identity)) {
      out.parked.insert(identity);
    } else {
      out.landed.insert(identity);
      out.deleted.insert(identity);
    }
  }
  for (const OwnedApplyStep& transfer : transfers) {
    if (std::find(delete_identities.begin(), delete_identities.end(),
                  transfer.identity) == delete_identities.end()) {
      out.landed.insert(transfer.identity);
    }
  }
  if (reloaded) {
    Reload(after);
  } else {
    page_reload_failed_ = true;
  }
  for (const UrlRuleSyncOp& op : batch) {
    if (op.kind == Kind::kMove) {
      ++out.owner_moved;
    }
  }
  return out;
}

std::set<std::string> UrlRuleOwnedKind::ClaimIdentities(
    const std::map<std::string, std::string>&) {
  return {};  // rules mint their sync id at creation
}

bool UrlRuleOwnedKind::LiveOwners(const std::set<std::string>& candidates,
                                  const OwnedOwnerMaps& maps,
                                  OwnedLiveRows* out) {
  std::vector<LocalUrlRule> rows;
  if (!access_.AllRules(&rows)) {
    return false;
  }
  for (const LocalUrlRule& row : rows) {
    if (!row.deleted_ms && row.sync_id && candidates.count(*row.sync_id)) {
      out->claimed.insert(*row.sync_id);
    }
  }
  const OwnerResolver resolve = maps.Resolver();
  for (const LocalUrlRule& row : Live()) {
    if (!row.sync_id || !out->claimed.count(*row.sync_id)) {
      continue;
    }
    if (std::optional<std::string> owner =
            UrlRuleKind::EligibilityOwner(row, resolve, std::nullopt)) {
      out->owners[*row.sync_id] = *owner;
    }
  }
  return true;
}

// §8.4.5: an accepted publish clears the pending-edit flag of a row still
// equal to what went out (and drops its merge pointer).
void UrlRuleOwnedKind::NotePublishApplied(const std::set<std::string>& identities) {
  std::map<std::string, RuleProjection> entries;
  for (const std::string& identity : identities) {
    if (const auto baseline = publish_baseline_.find(identity);
        baseline != publish_baseline_.end()) {
      entries[identity] = baseline->second;
    }
  }
  if (!entries.empty()) {
    access_.ClearPendingLocalEdits(entries, /*clears_partner=*/true);
  }
}

void UrlRuleOwnedKind::ClearPendingLocalEdits(const std::set<std::string>& candidates) {
  if (candidates.empty() || !publish_resolver_) {
    return;
  }
  const std::set<std::string> flagged =
      PendingLocalEditIdentities(Live(), *publish_resolver_);
  std::map<std::string, RuleProjection> entries;
  for (const std::string& identity : candidates) {
    const auto baseline = publish_baseline_.find(identity);
    if (baseline != publish_baseline_.end() && flagged.count(identity)) {
      entries[identity] = baseline->second;
    }
  }
  if (!entries.empty()) {
    access_.ClearPendingLocalEdits(entries, /*clears_partner=*/false);
  }
}

void UrlRuleOwnedKind::HardDeleteAfterTombstone(const std::set<std::string>& identities) {
  for (const std::string& identity : identities) {
    access_.HardDelete(identity);
  }
}

int UrlRuleOwnedKind::PurgeSoftDeletedRows(int64_t cutoff_ms) {
  return access_.PurgeSoftDeleted(cutoff_ms);
}

}  // namespace phi_client::sync

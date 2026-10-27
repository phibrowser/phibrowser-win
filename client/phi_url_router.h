// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_URL_ROUTER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_URL_ROUTER_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_app_model.h"
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_url_rule_normalize.h"
#include "phibrowser-win/client/phi_settings_types.h"
#include "phibrowser-win/client/phi_window_registry.h"

// The Space URL routing service (docs/architecture-review.md §3.4, the
// PhiUrlRouter row): the rules half of PhiSpaces.json, the routing
// decisions the framework hands back (Open Link in Space, a silent rule
// into a cold Space, the "ask first" prompt) and the framework's compiled
// PhiURLRouter table.
//
// THE MODEL IS SHARED: Spaces and rules are one file, so the router does
// not own a model — it holds a reference to the space manager's
// model::PhiSpacesModel and writes ONLY `url_rules` and
// `url_rules_generation` there (the list is the manager's; the router reads
// it for names and profiles). Both services mark the same store dirty; the
// coordinator coalesces the write.
//
// INVARIANTS. A rule's id is the stored SpaceURLRule.id: minted once, stable
// across edits, never reused (the Mac's contract; the old per-write
// "g<gen>-<index>" ids are gone). Every write stores the CANONICAL pattern
// (NormalizeUrlRuleHost / NormalizeUrlRulePathPrefix below — the Mac's
// LocalStore normalisation), so identity and the routing table agree with
// the Mac's. A rule may target two
// ACTIONS beside the user's Spaces — Incognito (kPhiIncognitoRuleTargetId)
// and Kiosk (kPhiKioskRuleTargetId) — the Mac's incognitoRuleTargetId /
// kioskRuleTargetId; the Kiosk id is a wire value shared with the fork's
// phi::kKioskRuleTargetId and must never change. The ask prompt is
// DEFERRED: OnAskSpaceForURL arrives inside a navigation-throttle
// callback, and a modal loop there is the lane 1 §3.1 hazard — the request
// is queued and answered on a posted turn.
//
// HOST-CLEAN ON PURPOSE: STL plus the model, ids and registry headers the
// host tests already compile — no windows.h, and of base/ only the
// RAW_PTR_EXCLUSION macro (the host build shims it, test/model/stubs) — so
// the pure rule operations below are host-tested
// (test/model/url_router_test.cc).

namespace phi_framework {
class PhiFramework;
}  // namespace phi_framework

namespace phi_client {

// --- Rule targets that are actions, not Spaces ---------------------------------
// Stable ids a rule (or the "ask first" chooser's pick) may carry instead
// of a Space id. Kiosk is shared with the fork's phi::kKioskRuleTargetId:
// the throttle resolves it itself (RouteKind::kKiosk). Incognito is the
// verb ladder's spelling, kept: the client resolves it to the profile's
// off-the-record window at route time.
inline constexpr char kPhiKioskRuleTargetId[] = "__phi_kiosk__";
inline constexpr char kPhiIncognitoRuleTargetId[] = "space.incognito";

inline bool UrlRuleTargetIsKiosk(std::string_view id) {
  return id == kPhiKioskRuleTargetId;
}
inline bool UrlRuleTargetIsIncognito(std::string_view id) {
  return id == kPhiIncognitoRuleTargetId;
}
inline bool UrlRuleTargetIsAction(std::string_view id) {
  return UrlRuleTargetIsKiosk(id) || UrlRuleTargetIsIncognito(id);
}

// --- Canonical pattern forms (host-tested) --------------------------------------

// The Mac's LocalStore.normalizedHost / normalizedPathPrefix, byte for byte
// (phi_url_rule_normalize.h, pinned by the Mac-generated vectors). A path
// prefix of "" means any path and "/" the ROOT only — the Chromium router's
// PathMatches reads them the same way. The model spells the Mac's nil path
// as "".
inline std::string NormalizeUrlRuleHost(const std::string& host) {
  return store::NormalizedRuleHost(host);
}

inline std::string NormalizeUrlRulePathPrefix(const std::string& raw) {
  return store::NormalizedRulePathPrefix(std::optional<std::string>(raw))
      .value_or(std::string());
}

// --- The editor's match types (host-tested) -------------------------------------
// The Mac URLRulesEditor.MatchType: what a stored (host, pathPrefix) pair
// SHOWS as, and how a typed value under a chosen type ENCODES back. The
// type is the single source of truth — typed "*." / "*…*" sigils are
// stripped and the type's own form re-added, so the stored host is always
// one of the three wire forms.

enum class UrlRuleMatchType {
  kDomainSuffix,    // host + all subdomains ("*.host")
  kDomain,          // exact host only
  kDomainContains,  // host contains a substring ("*needle*")
  kUrl,             // exact host + path prefix
  // The Mac's App rule ("app:<bundle id>", phibrowser-mac 48d4c4a6): links a
  // named Mac app opens. Windows shows, retargets, asks and deletes such a
  // row but never creates one, and never matches it (only the Mac knows
  // which app sent a URL).
  kSourceApp,
};

inline UrlRuleMatchType UrlRuleMatchTypeOf(const std::string& host,
                                           const std::string& path_prefix) {
  if (!path_prefix.empty()) {
    return UrlRuleMatchType::kUrl;
  }
  if (store::IsSourceAppRuleHost(host)) {
    return UrlRuleMatchType::kSourceApp;
  }
  if (host.size() > 2 && host.front() == '*' && host.back() == '*') {
    return UrlRuleMatchType::kDomainContains;
  }
  if (host.rfind("*.", 0) == 0) {
    return UrlRuleMatchType::kDomainSuffix;
  }
  return UrlRuleMatchType::kDomain;
}

// The user-facing value: the stored pair minus the wire sigils (the Mac's
// MatchType.decode).
inline std::string UrlRuleDisplayValue(const std::string& host,
                                       const std::string& path_prefix) {
  switch (UrlRuleMatchTypeOf(host, path_prefix)) {
    case UrlRuleMatchType::kDomainSuffix:
      return host.substr(2);
    case UrlRuleMatchType::kDomainContains:
      return host.substr(1, host.size() - 2);
    case UrlRuleMatchType::kUrl:
      return host + path_prefix;
    case UrlRuleMatchType::kSourceApp:
      return host.substr(store::kSourceAppRuleHostPrefix.size());
    case UrlRuleMatchType::kDomain:
      break;
  }
  return host;
}

// Drops a pasted ":8080" — GURL hosts never carry one. Only cuts when
// everything after the last ':' is digits, leaving IPv6 "[::1]" alone.
inline std::string StripUrlRulePort(const std::string& host) {
  const size_t colon = host.rfind(':');
  if (colon == std::string::npos || colon + 1 >= host.size()) {
    return host;
  }
  for (size_t i = colon + 1; i < host.size(); ++i) {
    if (host[i] < '0' || host[i] > '9') {
      return host;
    }
  }
  return host.substr(0, colon);
}

// The Mac's MatchType.encode: (type, typed value) -> the canonical stored
// (host, pathPrefix). The host-only types reduce the value to a bare host
// (a pasted full URL would otherwise leave the scheme or a "/" inside the
// stored host and the rule would silently never match); kUrl keeps the
// path, split at the first "/". An input that reduces to nothing (a bare
// "*.") yields an EMPTY host, which the caller drops.
inline void UrlRuleEncode(UrlRuleMatchType type,
                          std::string value,
                          std::string* host,
                          std::string* path_prefix) {
  host->clear();
  path_prefix->clear();
  // The editor's own reading of what was typed: trimmed and lowercased.
  // Not the stored-form normaliser, which also strips trailing dots — that
  // would turn a half-typed "*." into "*" here; the pieces this builds are
  // canonicalised when they are stored.
  {
    const size_t begin = value.find_first_not_of(" \t\r\n");
    const size_t end = value.find_last_not_of(" \t\r\n");
    value = begin == std::string::npos ? std::string()
                                       : value.substr(begin, end - begin + 1);
    for (char& c : value) {
      if (c >= 'A' && c <= 'Z') {
        c = static_cast<char>(c - 'A' + 'a');
      }
    }
  }
  const size_t scheme = value.find("://");
  if (scheme != std::string::npos) {
    value = value.substr(scheme + 3);
  }
  if (type == UrlRuleMatchType::kUrl) {
    std::string host_part = value;
    std::string path_part;
    const size_t slash = value.find('/');
    if (slash != std::string::npos) {
      host_part = value.substr(0, slash);
      path_part = value.substr(slash);
    }
    if (host_part.rfind("*.", 0) == 0) {
      host_part = host_part.substr(2);
    }
    *host = StripUrlRulePort(host_part);
    *path_prefix = NormalizeUrlRulePathPrefix(path_part);
    return;
  }
  const size_t slash = value.find('/');
  if (slash != std::string::npos) {
    value = value.substr(0, slash);
  }
  value = StripUrlRulePort(value);
  switch (type) {
    case UrlRuleMatchType::kDomainSuffix: {
      const std::string bare =
          value.rfind("*.", 0) == 0 ? value.substr(2) : value;
      *host = bare.empty() ? std::string() : "*." + bare;
      break;
    }
    case UrlRuleMatchType::kDomain:
      *host = value.rfind("*.", 0) == 0 ? value.substr(2) : value;
      break;
    case UrlRuleMatchType::kDomainContains: {
      std::string needle = value;
      while (!needle.empty() && needle.front() == '*') {
        needle.erase(needle.begin());
      }
      while (!needle.empty() && needle.back() == '*') {
        needle.pop_back();
      }
      *host = needle.empty() ? std::string() : "*" + needle + "*";
      break;
    }
    case UrlRuleMatchType::kSourceApp: {
      // The row shows the bare bundle id; a pasted "app:" prefix is not
      // doubled. The id is case-insensitive and already lowercased above.
      const std::string bare =
          store::IsSourceAppRuleHost(value)
              ? value.substr(store::kSourceAppRuleHostPrefix.size())
              : value;
      *host = bare.empty() || bare.find(':') != std::string::npos
                  ? std::string()
                  : std::string(store::kSourceAppRuleHostPrefix) + bare;
      break;
    }
    case UrlRuleMatchType::kUrl:
      break;  // Handled above.
  }
}

// The match type a typed pattern implies (the verb ladder and a pasted
// value): an explicit path means URL, explicit sigils keep their form, and
// a bare host defaults to the Mac's domain suffix (host + subdomains).
inline UrlRuleMatchType UrlRuleMatchTypeForInput(const std::string& value) {
  std::string work = value;
  const size_t scheme = work.find("://");
  if (scheme != std::string::npos) {
    work = work.substr(scheme + 3);
  }
  const size_t slash = work.find('/');
  if (slash != std::string::npos && slash + 1 < work.size()) {
    return UrlRuleMatchType::kUrl;
  }
  if (slash != std::string::npos) {
    work = work.substr(0, slash);
  }
  if (work.size() > 2 && work.front() == '*' && work.back() == '*') {
    return UrlRuleMatchType::kDomainContains;
  }
  return UrlRuleMatchType::kDomainSuffix;  // Also the bare-host default.
}

// --- Pure rule operations (host-tested) ---------------------------------------
// Each write bumps the generation; each returns whether it changed anything.
// Every write stores the canonical forms above.

// A rule the router should compile: a host to match and somewhere to send
// it — a Space, Incognito or Kiosk. (Incognito rules used to be held back
// while the client had no Incognito windows to route into; the client
// resolves them now, see PhiUrlRouter::RouteUrlToSpace.)
inline bool UrlRuleIsLive(const model::UrlRule& rule) {
  return !rule.host.empty() && !rule.space_id.empty();
}

// The canonical copy of a rule as every write stores it.
inline model::UrlRule CanonicalUrlRule(const model::UrlRule& rule) {
  model::UrlRule out = rule;
  out.host = NormalizeUrlRuleHost(rule.host);
  out.path_prefix = NormalizeUrlRulePathPrefix(rule.path_prefix);
  return out;
}

// The urlRules.* verbs' wire encoding of the rule list
// (PhiUrlRuleWireRow, phi_settings_types.h). The SETTINGS pane takes
// RulesWithIds() instead — structs carrying each rule's wire id.
inline std::vector<std::string> UrlRuleRowsOf(
    const std::vector<model::UrlRule>& rules) {
  std::vector<std::string> rows;
  rows.reserve(rules.size());
  for (const model::UrlRule& rule : rules) {
    rows.push_back(PhiUrlRuleWireRow(rule));
  }
  return rows;
}

// Refused for an empty Space id or host.
inline bool AddUrlRuleTo(model::PhiSpacesModel* model,
                         const std::string& space_id,
                         const std::string& host,
                         const std::string& path_prefix,
                         bool ask) {
  model::UrlRule rule =
      CanonicalUrlRule({space_id, host, path_prefix, ask, std::string()});
  if (rule.space_id.empty() || rule.host.empty()) {
    return false;
  }
  rule.id = store::NewRuleId();
  model->url_rules.push_back(rule);
  ++model->url_rules_generation;
  return true;
}

// Refused out of range or for an empty host.
inline bool SetUrlRulePatternIn(model::PhiSpacesModel* model,
                                int index,
                                const std::string& host,
                                const std::string& path_prefix) {
  const std::string canonical_host = NormalizeUrlRuleHost(host);
  if (index < 0 || index >= static_cast<int>(model->url_rules.size()) ||
      canonical_host.empty()) {
    return false;
  }
  model::UrlRule& rule = model->url_rules[static_cast<size_t>(index)];
  rule.host = canonical_host;
  rule.path_prefix = NormalizeUrlRulePathPrefix(path_prefix);
  ++model->url_rules_generation;
  return true;
}

// The universal editor's Save (the Mac's SpaceManager.setAllRules): the
// whole list at once, in the order the user authored — the vector order IS
// the sortOrder tiebreak. Rules that reduce to nothing (empty host after
// canonicalization, or no target) are dropped rather than stored inert.
// Returns false when the canonical result equals what is stored, in which
// case nothing moves — not even the generation.
inline bool ReplaceUrlRulesIn(model::PhiSpacesModel* model,
                              const std::vector<model::UrlRule>& rules) {
  std::vector<model::UrlRule> canonical;
  canonical.reserve(rules.size());
  for (const model::UrlRule& rule : rules) {
    model::UrlRule c = CanonicalUrlRule(rule);
    if (c.host.empty() || c.space_id.empty()) {
      continue;
    }
    canonical.push_back(std::move(c));
  }
  if (canonical == model->url_rules) {
    return false;
  }
  // A rule the editor added has no id yet; it gets one now, and keeps it.
  for (model::UrlRule& rule : canonical) {
    if (rule.id.empty()) {
      rule.id = store::NewRuleId();
    }
  }
  model->url_rules = std::move(canonical);
  ++model->url_rules_generation;
  return true;
}

// Refused out of range or for an empty Space id.
inline bool SetUrlRuleTargetIn(model::PhiSpacesModel* model,
                               int index,
                               const std::string& space_id,
                               bool ask) {
  if (index < 0 || index >= static_cast<int>(model->url_rules.size()) ||
      space_id.empty()) {
    return false;
  }
  model::UrlRule& rule = model->url_rules[static_cast<size_t>(index)];
  rule.space_id = space_id;
  rule.ask = ask;
  ++model->url_rules_generation;
  return true;
}

inline bool RemoveUrlRuleFrom(model::PhiSpacesModel* model, int index) {
  if (index < 0 || index >= static_cast<int>(model->url_rules.size())) {
    return false;
  }
  model->url_rules.erase(model->url_rules.begin() + index);
  ++model->url_rules_generation;
  return true;
}

// Drops every rule targeting `space_id` (a deleted Space). The generation
// moves whether or not any rule went — the write happened.
inline size_t DropUrlRulesForSpace(model::PhiSpacesModel* model,
                                   std::string_view space_id) {
  const size_t dropped = std::erase_if(
      model->url_rules,
      [&](const model::UrlRule& r) { return r.space_id == space_id; });
  ++model->url_rules_generation;
  return dropped;
}

// Resolves a rule id to its index: the stored rule id first; the old
// "g<generation>-<index>" spelling is still read (against the CURRENT
// generation) for probes written before rule ids were stable. False for an
// unknown or stale id.
inline bool ResolveUrlRuleIndex(const model::PhiSpacesModel& model,
                                std::string_view id,
                                size_t* index) {
  for (size_t i = 0; i < model.url_rules.size(); ++i) {
    if (!model.url_rules[i].id.empty() && model.url_rules[i].id == id) {
      *index = i;
      return true;
    }
  }
  size_t parsed = 0;
  if (!model::ParseUrlRuleWireId(id, model.url_rules_generation, &parsed) ||
      parsed >= model.url_rules.size()) {
    return false;
  }
  *index = parsed;
  return true;
}

// The id a rule is named by: its stored id, or (before its first save) the
// generation spelling.
inline std::string UrlRuleIdAt(const model::PhiSpacesModel& model,
                               size_t index) {
  if (index < model.url_rules.size() && !model.url_rules[index].id.empty()) {
    return model.url_rules[index].id;
  }
  return model::UrlRuleWireId(model.url_rules_generation, index);
}

// --- The service --------------------------------------------------------------

class PhiUrlRouter {
 public:
  // What routing needs from the rest of the app (the coordinator
  // implements it): browser creation for a cold Space, the coalesced
  // pushes, and the posted turn the ask prompt runs on.
  class Host {
   public:
    // Asks the framework for a browser in `profile_id` carrying `pending`
    // (a cold Space's routed link opens once it attaches) — the
    // coordinator's RequestCreation for WindowKind::kSpace.
    virtual void RequestSpaceBrowser(const std::string& profile_id,
                                     PendingCreation pending) = 0;
    // The generic Incognito target's Space: the first live Incognito Space,
    // else a new one (not switched to — the route surfaces it) — the Mac's
    // resolveIncognitoRouteTarget.
    virtual std::string ResolveIncognitoRouteSpace() = 0;
    // The routing table is due a (coalesced) push — every rule write.
    virtual void SchedulePushRoutingTable() = 0;
    // The Space menu (and with it the table) is due a push — the visible-
    // Space map changed under a route.
    virtual void SchedulePushSpaceMenu() = 0;
    // An ask is queued: post RunPendingAsk() to the next turn.
    virtual void PostAskSpace() = 0;

   protected:
    virtual ~Host() = default;
  };

  // A mutation writes nothing itself: it calls `dirty`, and the owner folds
  // the write into its coalesced-save timer.
  using DirtyCallback = std::function<void()>;
  // `model` is the space manager's; every reference outlives the service.
  PhiUrlRouter(model::PhiSpacesModel& model,
               PhiWindowRegistry& registry,
               phi_framework::PhiFramework& framework,
               Host& host,
               DirtyCallback dirty);
  ~PhiUrlRouter();

  PhiUrlRouter(const PhiUrlRouter&) = delete;
  PhiUrlRouter& operator=(const PhiUrlRouter&) = delete;

  // --- Rules (the Settings host's spellings and the urlRules.* verbs' ops) --
  const std::vector<model::UrlRule>& rules() const { return model_.url_rules; }
  int generation() const { return model_.url_rules_generation; }
  // ListRules: the wire rows, and the wire id of the rule at `index`.
  std::vector<std::string> RuleRows() const {
    return UrlRuleRowsOf(model_.url_rules);
  }
  std::string RuleWireId(size_t index) const {
    return UrlRuleIdAt(model_, index);
  }
  bool ResolveRuleIndex(std::string_view wire_id, size_t* index) const {
    return ResolveUrlRuleIndex(model_, wire_id, index);
  }
  // Every rule with its CURRENT wire id — what the Settings editor reads
  // and what its edits name back. Position is not identity here: a write
  // bumps the generation and every id with it, so an index the pane read
  // before an edit would land on whatever moved into that slot (lane 1 §5
  // D7). Reject a stale id instead of retargeting.
  std::vector<PhiUrlRuleRow> RulesWithIds() const;
  // Each write persists and re-pushes the routing table at once.
  void AddRule(const std::string& space_id,
               const std::string& host,
               const std::string& path_prefix,
               bool ask);
  void SetRulePattern(int index,
                      const std::string& host,
                      const std::string& path_prefix);
  void SetRuleTarget(int index, const std::string& space_id, bool ask);
  void RemoveRule(int index);
  // The universal editor's Save: the whole list, authored order. A no-op
  // list (canonically equal to what is stored) writes and pushes nothing.
  void ReplaceRules(const std::vector<model::UrlRule>& rules);
  // The same three writes addressed by wire id. False when the id no
  // longer names a rule (a prior generation, or a list that shrank) —
  // the caller re-lists rather than hitting a neighbour.
  bool SetRulePatternById(const std::string& rule_id,
                          const std::string& host,
                          const std::string& path_prefix);
  bool SetRuleTargetById(const std::string& rule_id,
                         const std::string& space_id,
                         bool ask);
  bool RemoveRuleById(const std::string& rule_id);
  // A Space is going: its rules go with it (the space manager's DeleteSpace,
  // through the coordinator).
  void DropRulesForSpace(const std::string& space_id);

  // --- Routing decisions (the framework delegate's three callbacks) --------
  // The page context menu's "Open Link in Space".
  void OnOpenLinkInSpace(int64_t source_window_id,
                         const char* space_id,
                         const char* url);
  // A silent rule matched but the target Space had no visible browser; the
  // navigation is already cancelled, so surface the Space and re-open the
  // URL there (never silently dropped).
  void OnRouteURLToSpace(const char* space_id,
                         const char* url,
                         int64_t source_window_id);
  // An "ask first" rule matched: queue the chooser for the next turn (see
  // the header comment). `source_is_new_tab` says the cancelled navigation
  // was replacing a fresh new tab, which the answer re-issues in place.
  void OnAskSpaceForURL(const char* url,
                        const char* default_space_id,
                        int64_t source_window_id,
                        bool source_is_new_tab);
  // The posted half of OnAskSpaceForURL: runs ONE queued chooser (modal)
  // and answers it — the chosen Space through RouteUrlToSpace, "keep it
  // here" through a routing-bypassing open in the source window.
  void RunPendingAsk();
  // Surfaces `space_id` (cold-spawning its browser if needed) and opens
  // `url` there with a one-shot routing bypass — the shared tail of the
  // context menu's "Open Link in Space", the silent auto-route into a
  // hidden/cold Space, the "ask first" prompt's pick and a tab's move to a
  // cold Space. The two ACTION targets take their own exits: Incognito
  // opens in the source profile's off-the-record window (an open one, else
  // a new one — the Mac's resolveIncognitoRouteTarget, created on demand);
  // Kiosk opens a fresh ephemeral Kiosk window.
  void RouteUrlToSpace(const std::string& space_id,
                       const std::string& url,
                       BrowserId source_window);

  // --- The framework's copy -------------------------------------------------
  // Compiles the live rules plus the Space -> VISIBLE browser map into the
  // framework's PhiURLRouter table — the body; the coordinator coalesces
  // the calls to one per turn. The Incognito target is aliased to the first
  // open Incognito shell's browser (the Mac's currentSpaceWindowMap alias)
  // so the fork routes straight into it; with none open the id stays
  // unmapped and the match comes back through OnRouteURLToSpace.
  void PushRoutingTableNow();

 private:
  struct PendingAsk {
    std::string url;
    std::string default_space_id;
    int64_t source_window_id = 0;
    bool source_is_new_tab = false;
  };
  void MarkDirty();

  // Non-owning; all outlive the service (see the constructor). The
  // framework is a singleton across a module boundary.
  RAW_PTR_EXCLUSION model::PhiSpacesModel& model_;
  RAW_PTR_EXCLUSION PhiWindowRegistry& registry_;
  RAW_PTR_EXCLUSION phi_framework::PhiFramework& framework_;
  RAW_PTR_EXCLUSION Host& host_;
  DirtyCallback dirty_;
  // Asks queued for their posted turn, in arrival order.
  std::deque<PendingAsk> pending_asks_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_URL_ROUTER_H_

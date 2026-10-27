// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_url_router.h"

#include <windows.h>

#include <utility>

#include "base/json/json_writer.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_space_manager.h"
#include "phibrowser-win/client/phi_url_rule_normalize.h"
#include "phibrowser-win/client/phi_shell_window.h"

namespace phi_client {

PhiUrlRouter::PhiUrlRouter(model::PhiSpacesModel& model,
                           PhiWindowRegistry& registry,
                           phi_framework::PhiFramework& framework,
                           Host& host,
                           DirtyCallback dirty)
    : model_(model),
      registry_(registry),
      framework_(framework),
      host_(host),
      dirty_(std::move(dirty)) {}

PhiUrlRouter::~PhiUrlRouter() = default;

// --- Rules ----------------------------------------------------------------------

std::vector<PhiUrlRuleRow> PhiUrlRouter::RulesWithIds() const {
  std::vector<PhiUrlRuleRow> rows;
  rows.reserve(model_.url_rules.size());
  for (size_t i = 0; i < model_.url_rules.size(); ++i) {
    rows.push_back({UrlRuleIdAt(model_, i), model_.url_rules[i]});
  }
  return rows;
}

void PhiUrlRouter::AddRule(const std::string& space_id,
                           const std::string& host,
                           const std::string& path_prefix,
                           bool ask) {
  if (!AddUrlRuleTo(&model_, space_id, host, path_prefix, ask)) {
    return;
  }
  MarkDirty();
  host_.SchedulePushRoutingTable();
}

void PhiUrlRouter::SetRulePattern(int index,
                                  const std::string& host,
                                  const std::string& path_prefix) {
  if (!SetUrlRulePatternIn(&model_, index, host, path_prefix)) {
    return;
  }
  MarkDirty();
  host_.SchedulePushRoutingTable();
}

void PhiUrlRouter::SetRuleTarget(int index,
                                 const std::string& space_id,
                                 bool ask) {
  if (!SetUrlRuleTargetIn(&model_, index, space_id, ask)) {
    return;
  }
  MarkDirty();
  host_.SchedulePushRoutingTable();
}

void PhiUrlRouter::RemoveRule(int index) {
  if (!RemoveUrlRuleFrom(&model_, index)) {
    return;
  }
  MarkDirty();
  host_.SchedulePushRoutingTable();
}

void PhiUrlRouter::ReplaceRules(const std::vector<model::UrlRule>& rules) {
  if (!ReplaceUrlRulesIn(&model_, rules)) {
    return;  // Canonically what is stored already: nothing to write or push.
  }
  MarkDirty();
  host_.SchedulePushRoutingTable();
}

bool PhiUrlRouter::SetRulePatternById(const std::string& rule_id,
                                     const std::string& host,
                                     const std::string& path_prefix) {
  size_t index = 0;
  if (!ResolveRuleIndex(rule_id, &index)) {
    return false;
  }
  SetRulePattern(static_cast<int>(index), host, path_prefix);
  return true;
}

bool PhiUrlRouter::SetRuleTargetById(const std::string& rule_id,
                                     const std::string& space_id,
                                     bool ask) {
  size_t index = 0;
  if (!ResolveRuleIndex(rule_id, &index)) {
    return false;
  }
  SetRuleTarget(static_cast<int>(index), space_id, ask);
  return true;
}

bool PhiUrlRouter::RemoveRuleById(const std::string& rule_id) {
  size_t index = 0;
  if (!ResolveRuleIndex(rule_id, &index)) {
    return false;
  }
  RemoveRule(static_cast<int>(index));
  return true;
}

void PhiUrlRouter::DropRulesForSpace(const std::string& space_id) {
  // The generation moves either way (the Space list's write is the same
  // file); the manager's own save carries the rules, so no push of its own
  // is needed beyond the one every Spaces flush makes — but one is cheap
  // and keeps the table honest should the flush be far off.
  DropUrlRulesForSpace(&model_, space_id);
  MarkDirty();
  host_.SchedulePushRoutingTable();
}

// --- Routing decisions ----------------------------------------------------------

void PhiUrlRouter::OnOpenLinkInSpace(int64_t source_window_id,
                                     const char* space_id_c,
                                     const char* url_c) {
  const std::string space_id = space_id_c ? space_id_c : "";
  const std::string url = url_c ? url_c : "";
  if (space_id.empty() || url.empty()) {
    return;
  }
  RouteUrlToSpace(space_id, url, BrowserId{source_window_id});
}

void PhiUrlRouter::OnRouteURLToSpace(const char* space_id_c,
                                     const char* url_c,
                                     int64_t source_window_id) {
  const std::string space_id = space_id_c ? space_id_c : "";
  const std::string url = url_c ? url_c : "";
  if (space_id.empty() || url.empty()) {
    return;
  }
  RouteUrlToSpace(space_id, url, BrowserId{source_window_id});
}

void PhiUrlRouter::OnAskSpaceForURL(const char* url_c,
                                    const char* default_space_id_c,
                                    int64_t source_window_id,
                                    bool source_is_new_tab) {
  PendingAsk ask;
  ask.url = url_c ? url_c : "";
  ask.default_space_id = default_space_id_c ? default_space_id_c : "";
  ask.source_window_id = source_window_id;
  ask.source_is_new_tab = source_is_new_tab;
  if (ask.url.empty() || model_.spaces.empty()) {
    return;
  }
  // Deferred (see the header): the throttle callback this arrives in is no
  // place for a modal loop. The navigation is already cancelled, so the
  // chooser loses nothing by running a turn later.
  pending_asks_.push_back(std::move(ask));
  host_.PostAskSpace();
}

void PhiUrlRouter::RunPendingAsk() {
  if (pending_asks_.empty()) {
    return;
  }
  const PendingAsk ask = std::move(pending_asks_.front());
  pending_asks_.pop_front();
  if (model_.spaces.empty()) {
    return;
  }
  // The source slot's shell owns the (modal) chooser; its active Space is
  // the "keep here" target.
  const BrowserId source{ask.source_window_id};
  PhiShellWindow* controller = registry_.ControllerForBrowser(source);
  const std::string current_space_id =
      controller ? controller->active_space() : std::string();
  const bool current_is_incognito = controller && controller->is_incognito();

  // The Mac chooser's order (PhiChromiumCoordinator.askSpace): the current
  // Space first, then the rest in strip order, then ONE generic Incognito
  // choice — leading instead when the navigation started off-the-record —
  // and Kiosk last, in a section of its own: an action, not a Space.
  std::vector<std::string> ids;
  std::vector<PhiChoiceRow> rows;
  auto add = [&](const std::string& id, std::wstring label,
                 bool starts_section, bool current) {
    ids.push_back(id);
    rows.push_back({std::move(label), starts_section, current});
  };
  for (const model::Space& space : model_.spaces) {
    if (space.id == current_space_id) {
      add(space.id, base::UTF8ToWide(space.name), false, true);
    }
  }
  if (current_is_incognito) {
    add(kPhiIncognitoRuleTargetId,
        PhiStr("spaces.builtIn.incognito.routingTargetName"), false, true);
  }
  for (const model::Space& space : model_.spaces) {
    if (space.id != current_space_id) {
      add(space.id, base::UTF8ToWide(space.name), false, false);
    }
  }
  if (!current_is_incognito) {
    add(kPhiIncognitoRuleTargetId,
        PhiStr("spaces.builtIn.incognito.routingTargetName"), false, false);
  }
  add(kPhiKioskRuleTargetId, PhiStr("sidebar.urlRulesEditor.target.kioskOption"),
      true, false);
  int default_index = 0;
  for (size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] == ask.default_space_id) {
      default_index = static_cast<int>(i);
      break;
    }
  }
  const int chosen = PhiPromptChooseRows(
      controller ? controller->shell_hwnd() : nullptr,
      PhiStr("app.openInSpace"), base::UTF8ToWide(ask.url).c_str(), rows,
      default_index);
  const std::string chosen_id =
      chosen >= 0 && chosen < static_cast<int>(ids.size())
          ? ids[static_cast<size_t>(chosen)]
          : std::string();
  const bool keep_here =
      chosen_id.empty() || chosen_id == current_space_id ||
      (current_is_incognito && UrlRuleTargetIsIncognito(chosen_id));
  if (keep_here) {
    // Keep it here. The cancelled navigation was replacing the active tab
    // when it started from a fresh new tab — re-issue it in place there;
    // otherwise open a fresh foreground tab (the Mac split). Both bypass
    // routing so the same rule cannot re-prompt.
    if (ask.source_is_new_tab) {
      framework_.NavigateActiveTabBypassingSpaceRouting(ask.source_window_id,
                                                        ask.url.c_str());
    } else {
      framework_.OpenTabBypassingSpaceRouting(ask.source_window_id,
                                              ask.url.c_str());
    }
    if (controller) {
      controller->ScheduleRefresh();
    }
    return;
  }
  // Leaving the source: a new tab stranded by the cancelled navigation goes
  // back to a clean new-tab page (the Mac's refreshActiveNewTab) before the
  // destination takes over.
  if (ask.source_is_new_tab && controller) {
    controller->ResetNativeNtp();
  }
  RouteUrlToSpace(chosen_id, ask.url, source);
}

void PhiUrlRouter::RouteUrlToSpace(const std::string& space_id,
                                   const std::string& url,
                                   BrowserId source_window) {
  if (UrlRuleTargetIsKiosk(space_id)) {
    // An ephemeral Kiosk in the source's profile; the framework announces
    // it as WindowKind::kKiosk and the coordinator hosts it. A refusal
    // (source gone, bad URL) keeps the URL in the source window rather
    // than dropping it — the Mac's spawn-failure fallback.
    if (!framework_.OpenUrlInKiosk(source_window.value, url.c_str())) {
      framework_.OpenTabBypassingSpaceRouting(source_window.value,
                                              url.c_str());
    }
    return;
  }
  if (UrlRuleTargetIsIncognito(space_id)) {
    // The Mac's generic Incognito target resolves to a LIVE Incognito
    // Space, created on demand (docs/incognito-space-port.md D8) — then it
    // is routed like any Space below.
    const std::string incognito_space = host_.ResolveIncognitoRouteSpace();
    if (incognito_space.empty()) {
      framework_.OpenTabBypassingSpaceRouting(source_window.value,
                                              url.c_str());
      return;
    }
    RouteUrlToSpace(incognito_space, url, source_window);
    return;
  }
  // A slot that already holds the Space: switch to it there and open the
  // link (the Mac activates the Space's window and opens the tab). The open
  // bypasses URL routing — the URL may be the very navigation a rule just
  // re-homed, and re-matching it would loop. User shells only: an incognito
  // window and a hidden agent shell nominally hold the DEFAULT Space (their
  // first browser is adopted under it) but are never its window.
  for (PhiShellWindow* controller : registry_.UserControllers()) {
    if (!controller->HasSpace(space_id)) {
      continue;
    }
    controller->ShowSpace(space_id);
    framework_.OpenTabBypassingSpaceRouting(
        controller->active_browser_id().value, url.c_str());
    if (controller->shell_hwnd()) {
      ::SetForegroundWindow(controller->shell_hwnd());
    }
    controller->ScheduleRefresh();
    host_.SchedulePushSpaceMenu();  // The visible-Space routing map changed.
    return;
  }
  // Cold Space: ask for its browser in its profile; the link opens once the
  // browser attaches (the spaces.switch machinery), so a request already in
  // flight for this slot and Space just gains the url.
  // An Incognito Space is no stored Space: its browser is the shared
  // off-the-record session's, whose parent the framework picks.
  std::string profile_id = "Default";
  if (!model::IsIncognitoSpaceId(space_id)) {
    const model::Space* space = FindSpaceIn(model_.spaces, space_id);
    if (!space) {
      return;
    }
    profile_id = space->profile_id;
  }
  SlotId slot =
      registry_.SlotFor(source_window).value_or(registry_.FirstUserSlot());
  // An incognito window or an agent shell holds no Spaces: the first user
  // window takes the Space instead.
  if (PhiShellWindow* source = registry_.Get(slot);
      source && (source->is_incognito() || source->is_agent())) {
    slot = registry_.FirstUserSlot();
  }
  if (!slot.valid()) {
    return;
  }
  if (PendingCreation* in_flight = registry_.FindPendingSpace(slot, space_id)) {
    in_flight->open_urls.push_back(url);
    return;
  }
  PendingCreation pending;
  pending.slot = slot;
  pending.space_id = space_id;
  pending.open_urls.push_back(url);
  host_.RequestSpaceBrowser(profile_id, std::move(pending));
}

// --- The framework's copy -------------------------------------------------------

void PhiUrlRouter::PushRoutingTableNow() {
  base::DictValue root;
  base::ListValue rules;
  int sort_order = 0;
  for (const model::UrlRule& rule : model_.url_rules) {
    if (!UrlRuleIsLive(rule)) {
      continue;
    }
    // App rules ("app:<bundle id>") match a sending Mac app, never a URL:
    // the framework's matcher compares GURL hosts, so one would be inert
    // anyway, but it has no business in the routing table.
    if (store::IsSourceAppRuleHost(rule.host)) {
      continue;
    }
    base::DictValue d;
    d.Set("spaceId", rule.space_id);
    d.Set("host", store::RuleHostForRouting(rule.host));
    if (!rule.path_prefix.empty()) {
      d.Set("pathPrefix", rule.path_prefix);
    }
    d.Set("ask", rule.ask);
    d.Set("sortOrder", sort_order++);
    rules.Append(std::move(d));
  }
  root.Set("rules", std::move(rules));
  // Each Space's VISIBLE browser only — a Space hidden behind a slot's
  // active sibling is deliberately absent, so a match on it resolves to
  // OnRouteURLToSpace (which re-enters ShowSpace) instead of a direct open
  // into an off-screen browser.
  base::DictValue map;
  for (PhiShellWindow* controller : registry_.Controllers()) {
    if (controller->is_agent()) {
      continue;
    }
    // An incognito WINDOW is no routing target any more: the generic
    // Incognito target means an Incognito Space (D8).
    if (controller->is_incognito()) {
      continue;
    }
    const std::string& space_id = controller->active_space();
    const BrowserId browser_id = controller->active_browser_id();
    if (space_id.empty() || !browser_id.valid() || map.Find(space_id)) {
      continue;
    }
    map.Set(space_id, static_cast<double>(browser_id.value));
    // An Incognito Space on screen also stands in for the generic target
    // (the first one wins, the Mac's strip-order alias).
    if (model::IsIncognitoSpaceId(space_id) &&
        !map.Find(kPhiIncognitoRuleTargetId)) {
      map.Set(kPhiIncognitoRuleTargetId, static_cast<double>(browser_id.value));
    }
  }
  root.Set("spaceWindows", std::move(map));
  std::string json;
  base::JSONWriter::Write(root, &json);
  framework_.SetSpaceRoutingTable(json.c_str());
}

void PhiUrlRouter::MarkDirty() {
  if (dirty_) {
    dirty_();
  }
}

}  // namespace phi_client

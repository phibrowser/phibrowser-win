// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include <windows.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/json/json_reader.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "client/phi_content_blocking.h"
#include "client/phi_content_blocking_window.h"
#include "client/phi_control_dispatcher.h"
#include "client/phi_coordinator.h"
#include "client/phi_shell_window.h"
#include "client/phi_window_registry.h"

// The box's content blocking hooks (test rig only, kHost): read a profile's
// state as the Settings group sees it, drive the same facade the group and
// the rule set chooser drive, and read what the address bar's site menu
// would show. The settings UI itself is driven with real input; these set a
// profile up, clean it afterwards and check the outcome.

namespace phi_client {

namespace {

std::optional<PhiContentBlockingCategory> CategoryNamed(
    const std::string& name) {
  if (name == "ads") {
    return PhiContentBlockingCategory::kAds;
  }
  if (name == "cookieBanners") {
    return PhiContentBlockingCategory::kCookieBanners;
  }
  if (name == "trackers") {
    return PhiContentBlockingCategory::kTrackers;
  }
  return std::nullopt;
}

base::DictValue StateJson(const PhiContentBlockingState& state) {
  base::DictValue out;
  out.Set("blockAds", state.block_ads);
  out.Set("blockCookieBanners", state.block_cookie_banners);
  out.Set("blockTrackers", state.block_trackers);
  out.Set("status", state.status);
  out.Set("statusDetail", state.status_detail);
  out.Set("sessionBlockedCount", state.session_blocked_count);
  out.Set("lastBuildLog", state.last_build_log);
  base::ListValue exceptions;
  for (const std::string& domain : state.site_exceptions) {
    exceptions.Append(domain);
  }
  out.Set("siteExceptions", std::move(exceptions));
  base::ListValue lists;
  for (const PhiContentBlockingList& list : state.lists) {
    base::DictValue item;
    item.Set("id", list.id);
    item.Set("category", list.category);
    item.Set("name", list.name);
    item.Set("custom", list.custom);
    item.Set("sourceUrl", list.source_url);
    item.Set("checked", list.checked);
    item.Set("available", list.available);
    item.Set("downloading", list.downloading);
    item.Set("lastError", list.last_error);
    lists.Append(std::move(item));
  }
  out.Set("lists", std::move(lists));
  // What the Settings group draws from it.
  base::DictValue needs;
  base::DictValue summaries;
  for (const auto& [name, category] :
       {std::pair{"ads", PhiContentBlockingCategory::kAds},
        std::pair{"cookieBanners", PhiContentBlockingCategory::kCookieBanners},
        std::pair{"trackers", PhiContentBlockingCategory::kTrackers}}) {
    needs.Set(name, PhiContentBlockingNeedsDownload(category, state));
    summaries.Set(name, base::WideToUTF8(PhiContentBlockingSummary(
                            category, state)));
  }
  out.Set("needsDownload", std::move(needs));
  out.Set("summaries", std::move(summaries));
  return out;
}

}  // namespace

PhiVerbResult PhiControlDispatcher::VerbContentBlockingGet(
    const PhiVerbContext& ctx) {
  // {profileId, refresh?}: the facade's last read (and a fresh read asked
  // for unless refresh is false). `ready` false until the first answer.
  const std::string profile_id = ctx.params.String("profileId");
  if (profile_id.empty()) {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload);
  }
  PhiContentBlocking& blocking = coordinator_->ContentBlocking();
  if (ctx.params.Bool("refresh", true)) {
    blocking.Refresh(profile_id);
  }
  base::DictValue out;
  const PhiContentBlockingState* state = blocking.State(profile_id);
  out.Set("ready", state != nullptr);
  out.Set("unavailable", blocking.Unavailable(profile_id));
  out.Set("chooserOpen", PhiContentBlockingWindow::Handle() != nullptr);
  out.Set("chooserHwnd",
          static_cast<double>(reinterpret_cast<uintptr_t>(
              PhiContentBlockingWindow::Handle())));
  if (state) {
    out.Set("state", StateJson(*state));
  }
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbContentBlockingSet(
    const PhiVerbContext& ctx) {
  // {profileId, and ONE of: category + enabled, list + checked,
  // download: [ids], refresh: true, addCustomList: {name, url | rules},
  // removeCustomList: id}. Answers once the write is out; read the result
  // back with contentBlocking.get.
  const std::string profile_id = ctx.params.String("profileId");
  if (profile_id.empty()) {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload);
  }
  PhiContentBlocking& blocking = coordinator_->ContentBlocking();
  if (!blocking.State(profile_id)) {
    blocking.Refresh(profile_id);
    return PhiVerbResult::Fail("not_ready");
  }
  if (ctx.params.Has("category")) {
    const std::optional<PhiContentBlockingCategory> category =
        CategoryNamed(ctx.params.String("category"));
    if (!category || !ctx.params.FindBool("enabled")) {
      return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload);
    }
    blocking.SetCategory(profile_id, *category,
                         ctx.params.Bool("enabled", false));
  } else if (ctx.params.Has("list")) {
    if (!ctx.params.FindBool("checked")) {
      return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload);
    }
    blocking.SetList(profile_id, ctx.params.String("list"),
                     ctx.params.Bool("checked", false));
  } else if (ctx.params.Has("download")) {
    blocking.DownloadLists(profile_id, ctx.params.StringList("download"));
  } else if (ctx.params.Bool("refreshLists", false)) {
    blocking.RefreshLists(profile_id);
  } else if (const base::DictValue* add =
                 ctx.params.FindDict("addCustomList")) {
    const std::string* name = add->FindString("name");
    const std::string* url = add->FindString("url");
    const std::string* rules = add->FindString("rules");
    blocking.AddCustomList(profile_id, name ? *name : std::string(),
                           url ? *url : std::string(),
                           rules ? *rules : std::string(), nullptr);
  } else if (ctx.params.Has("removeCustomList")) {
    blocking.RemoveCustomList(profile_id,
                              ctx.params.String("removeCustomList"));
  } else {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload);
  }
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbContentBlockingSiteMenu(
    const PhiVerbContext& ctx) {
  // {windowId?}: the framework's SiteMenuJson for the slot's active tab —
  // what the site menu reads to build "Content blocking on this site".
  // {blocking: bool} also sets the row, as clicking it does (no reload).
  PhiShellWindow* controller = nullptr;
  if (const int64_t window_id = ctx.params.Int64("windowId", 0)) {
    controller = coordinator_->registry().Resolve(window_id);
    if (!controller) {
      return PhiVerbResult::Fail(PhiErrorCode::kWindowNotOpen);
    }
  }
  if (!controller) {
    controller = coordinator_->FocusedUserController();
  }
  const BrowserId browser =
      controller ? controller->active_browser_id() : BrowserId{};
  if (!framework_ || !browser.valid()) {
    return PhiVerbResult::Fail(PhiErrorCode::kWindowNotOpen);
  }
  base::DictValue out;
  if (const std::optional<bool> blocking = ctx.params.FindBool("blocking")) {
    out.Set("applied",
            framework_->SetActiveSiteContentBlocking(browser.value, *blocking));
  }
  const char* json = framework_->SiteMenuJson(browser.value);
  std::optional<base::DictValue> info =
      json ? base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC)
           : std::nullopt;
  if (info) {
    out.Set("menu", std::move(*info));
  }
  return PhiVerbResult::Ok(std::move(out));
}

}  // namespace phi_client

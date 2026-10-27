// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <windows.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json/json_reader.h"
#include "base/rand_util.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_agent_spaces.h"
#include "phibrowser-win/client/phi_client_analytics.h"
#include "phibrowser-win/client/phi_control_dispatcher.h"
#include "phibrowser-win/client/phi_coordinator.h"
#include "phibrowser-win/client/phi_sidebar_model.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_features.h"
#include "phibrowser-win/client/phi_folder_icons.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_tab_store.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_shell_window.h"
#include "phibrowser-win/client/phi_window_registry.h"

// The user's own state: Spaces, profiles, url routing rules, pins,
// bookmarks and the emoji catalog. One handler per registry row.

namespace phi_client {

namespace {

// The slot a payload names, taken from the ALREADY RESOLVED scope. A wire
// windowId may be a slot id, a browser bound to one (what spaces.list
// reports in windowIds) or 0 for "the window", and only ResolveScope knows
// which (docs/architecture-review.md §3.7 item 5). Building a SlotId from
// the raw number instead made a browser id name no slot at all, so a pin
// list aimed at a Space's own window came back empty.
SlotId SlotOf(const PhiVerbContext& ctx) {
  return ctx.scope.controller ? ctx.scope.controller->slot()
                              : SlotId{ctx.params.Int64("windowId", 0)};
}

// A rule host that is really a URL ("https://host/", "host/path"): the
// Mac editor never stores one, and stored as typed it could never match a
// navigation's host, so the verbs refuse it rather than accept a dead rule.
bool RuleHostLooksLikeUrl(std::string_view host) {
  return host.find_first_of("/?#") != std::string_view::npos;
}

// Splits a tab-separated row (the coordinator's URLRuleRows() encoding).
// Keeps empty fields so positional indices stay stable.
std::vector<std::string> SplitTabRow(const std::string& value) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    size_t tab = value.find('\t', start);
    if (tab == std::string::npos) {
      parts.push_back(value.substr(start));
      break;
    }
    parts.push_back(value.substr(start, tab - start));
    start = tab + 1;
  }
  return parts;
}

// The theme a spaces.create / spaces.update payload asks to pin (the Mac's
// requestedThemeId): `themeId` names a built-in theme by id or display
// name, a bare `colorHex` snaps to the nearest one by hue; `themeId` wins
// when both are given. A Space's colour IS its pinned theme — storing the
// hex alone answered ok while the window kept its old colour (PHI-1648).
// Leaves `theme_id` empty when the payload names none; false with `error`
// set on a value nothing matches.
bool RequestedThemeId(const PhiParams& params,
                      std::string* theme_id,
                      std::string* error) {
  const std::string theme = std::string(
      base::TrimWhitespaceASCII(params.String("themeId"), base::TRIM_ALL));
  if (!theme.empty()) {
    *theme_id = PhiThemeIdForName(theme);
    if (theme_id->empty()) {
      *error = "unknown_theme";
      return false;
    }
    return true;
  }
  const std::string hex = std::string(
      base::TrimWhitespaceASCII(params.String("colorHex"), base::TRIM_ALL));
  if (!hex.empty()) {
    PhiColor color = 0;
    if (!PhiParseColorHex(hex, &color)) {
      *error = "invalid_color";
      return false;
    }
    *theme_id = PhiNearestThemeId(color);
  }
  return true;
}

// The payload's `iconName` normalized to a storage value the strip can
// draw (PhiNormalizeIconName); empty when it names none. An unknown value
// is `invalid_icon` rather than a Space stored with a blank icon
// (PHI-1649).
bool RequestedIconName(const PhiParams& params,
                       std::string* icon,
                       std::string* error) {
  const std::string raw = params.String("iconName");
  if (base::TrimWhitespaceASCII(raw, base::TRIM_ALL).empty()) {
    return true;
  }
  *icon = PhiNormalizeIconName(raw);
  if (icon->empty()) {
    *error = "invalid_icon";
    return false;
  }
  return true;
}

}  // namespace

// --- Spaces ---------------------------------------------------------------------

PhiVerbResult PhiControlDispatcher::VerbSpacesList(const PhiVerbContext& ctx) {
  PhiShellWindow* shell =
      coordinator_->registry().Resolve(ctx.params.Int64("windowId", 0));
  base::ListValue list;
  int sort_order = 0;
  for (const Space& space : coordinator_->spaces_model().spaces) {
    base::DictValue s;
    // The client-UI shape (id/icon/themeId) AND the skill's management shape
    // (spaceId/colorHex/iconName/sortOrder/isDefault/isActive) — one list
    // serves the box probes and the phi-browser skill both.
    s.Set("id", space.id);
    s.Set("spaceId", space.id);
    s.Set("name", space.name);
    s.Set("profileId", space.profile_id);
    s.Set("icon", space.icon);
    s.Set("iconName", space.icon);
    s.Set("colorHex", space.color_hex);
    // What the Space renders with: its pinned theme, else the app-wide one
    // (the Mac's resolvedThemeId) — never empty, so a caller can compare
    // it with what spaces.update answered.
    s.Set("themeId", space.theme_id.empty() ? coordinator_->ThemeId()
                                            : space.theme_id);
    s.Set("iconText", PhiIconDisplayText(space.icon));
    s.Set("sortOrder", sort_order++);
    s.Set("isDefault", space.id == coordinator_->DefaultSpaceId());
    const bool active = shell && shell->active_space() == space.id;
    s.Set("active", active);
    s.Set("isActive", active);
    s.Set("open", shell && shell->HasSpace(space.id));
    // Every user window showing the Space, as CDP windowIds (the Space's
    // browser in each slot) — what the skill pins a binding to
    // (enterContext({kind:'user', window})); empty when it is not open.
    base::ListValue window_ids;
    for (PhiShellWindow* other :
         coordinator_->registry().Controllers()) {
      if (other->is_agent()) {
        continue;
      }
      const BrowserId browser = other->BrowserIdForSpace(space.id);
      if (browser.valid()) {
        window_ids.Append(static_cast<double>(browser.value));
      }
    }
    s.Set("windowIds", std::move(window_ids));
    list.Append(std::move(s));
  }
  base::DictValue out;
  out.Set("spaces", std::move(list));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSpacesCreate(
    const PhiVerbContext& ctx) {
  // A Space id is a UUID, as the Mac mints them (SpaceManager); the
  // number only names a Space nobody named.
  const std::string id = store::NewUppercaseUuid();
  std::string name = ctx.params.String("name");
  if (name.empty()) {
    name = "Space " +
           std::to_string(coordinator_->spaces_model().spaces.size() + 1);
  }
  std::string profile_id = ctx.params.String("profileId");
  if (profile_id.empty()) {
    profile_id = "Default";  // A Space always binds exactly one profile.
  }
  // An agent's payload is validated before anything is written: an unknown
  // theme, colour or icon fails the create instead of storing a Space that
  // renders blank or in the wrong colour. Every agent-made Space owns a
  // pinned theme, as the Mac's does — the one asked for, else the look it
  // would render with today (the app-wide theme). The user's own forms
  // (kInternal) pin their theme themselves and keep the lenient path.
  const bool from_agent = ctx.caller != ControlCaller::kInternal;
  std::string theme_id, icon_name, error;
  if (from_agent) {
    if (!RequestedThemeId(ctx.params, &theme_id, &error) ||
        !RequestedIconName(ctx.params, &icon_name, &error)) {
      return PhiVerbResult::Fail(error);
    }
    if (theme_id.empty()) {
      theme_id = coordinator_->ThemeId();
    }
  } else {
    icon_name = ctx.params.String("iconName");
  }
  // A named icon when one was given and valid; a random catalog draw
  // otherwise, as the Mac's create panel does.
  const auto& catalog = PhiIconCatalogAll();
  const std::string icon =
      PhiIconIsValid(icon_name)
          ? icon_name
          : "phi:" +
                std::string(catalog[base::RandGenerator(catalog.size())].id);
  Space space{id, name, profile_id, icon};
  space.color_hex = from_agent ? PhiThemeOverlayHex(theme_id)
                               : ctx.params.String("colorHex");
  coordinator_->spaces_model().spaces.push_back(std::move(space));
  // Now, not on the save timer: a bookmark or pin added to the Space by the
  // next verb needs its store row (read-your-writes).
  coordinator_->SaveNow(PhiCoordinator::StoreDomain::kSpaces);
  if (from_agent) {
    coordinator_->SetSpaceTheme(id, theme_id);
  }
  if (ctx.caller == ControlCaller::kInternal) {
    // The user's own create form (the Mac's space_created is the panel's,
    // never an agent's spaces.create): counts and the form's surface
    // (RequestCreateSpaceWithOptions), not the Space.
    coordinator_->CaptureAnalytics(
        kAnalyticsModuleSpaces, kEventSpaceCreated,
        PhiSpaceCreatedProperties(
            static_cast<int>(coordinator_->spaces_model().spaces.size()),
            profile_id != "Default", ctx.params.String("surface")));
    coordinator_->NoteFirstTimeAction(kFirstTimeActionSpaceCreated);
  }
  // A wire colorHex tints the new Space.
  coordinator_->RebuildSpacePalettes();
  // The list changed under every window: the strips rebuild their pips
  // (and a lone Space's hidden strip comes back), Settings relists — the
  // same broadcast a rename or a delete makes from the manager.
  coordinator_->NoteSpaceListChanged();
  if (ctx.params.Bool("activate", false)) {
    // Surface it in the user's focused window — explicit opt-in only (the
    // skill's default create never yanks the user's window).
    PhiShellWindow* focused = coordinator_->FocusedUserController();
    bool pending = false, cold = false;
    coordinator_->SwitchSlotToSpace(focused ? focused->slot() : SlotId(), id,
                                    &pending, &cold);
  }
  base::DictValue out;
  out.Set("id", id);
  out.Set("spaceId", id);
  out.Set("name", name);
  out.Set("profileId", profile_id);
  if (from_agent) {
    // What landed, so the caller reports the snapped theme rather than the
    // hex it asked for.
    out.Set("themeId", theme_id);
    out.Set("iconName", icon);
  }
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSpacesSwitch(
    const PhiVerbContext& ctx) {
  // Only a user shell shows Spaces: an agent's window belongs to its task
  // and a lone incognito window to its off-the-record session, so neither
  // may be switched — by anyone. And an agent never surfaces an Incognito
  // Space (the host may).
  const SlotId slot = SlotOf(ctx);
  if (slot.value && (coordinator_->registry().IsAgentSlot(slot) ||
                     coordinator_->registry().IsIncognitoSlot(slot))) {
    return PhiVerbResult::Fail(PhiErrorCode::kScopeNotAllowed);
  }
  const std::string space_id = ctx.params.String("spaceId");
  if (ctx.caller == ControlCaller::kPipe && !(ctx.principal.caps & kHost) &&
      model::IsIncognitoSpaceId(space_id)) {
    return PhiVerbResult::Fail(PhiErrorCode::kScopeNotAllowed);
  }
  bool pending = false, cold = false;
  const bool ok = coordinator_->SwitchSlotToSpace(slot, space_id, &pending,
                                                  &cold);
  base::DictValue out;
  if (cold) {
    out.Set("pending", pending);  // Attaching when the browser arrives.
  }
  return PhiVerbOk(ok, std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSpacesRename(
    const PhiVerbContext& ctx) {
  if (!FindSpace(ctx.params.String("spaceId"))) {
    return PhiVerbResult::Fail("unknown_space");
  }
  return PhiVerbOk(coordinator_->RenameSpace(ctx.params.String("spaceId"),
                                             ctx.params.String("name")));
}

PhiVerbResult PhiControlDispatcher::VerbSpacesDelete(
    const PhiVerbContext& ctx) {
  // The manager directly: the Mac's space_deleted is its two confirm
  // dialogs' (the coordinator's host override), never an agent's verb.
  const std::string space_id = ctx.params.String("spaceId");
  if (model::IsIncognitoSpaceId(space_id)) {
    // Never an agent's to close (see VerbSpacesSwitch).
    if (ctx.caller == ControlCaller::kPipe && !(ctx.principal.caps & kHost)) {
      return PhiVerbResult::Fail(PhiErrorCode::kScopeNotAllowed);
    }
    // Deleting an Incognito Space closes it (the Mac passes it through).
    const bool live = coordinator_->space_manager().Find(space_id) != nullptr;
    coordinator_->RequestCloseIncognitoSpace(space_id);
    return PhiVerbOk(live);
  }
  if (!FindSpace(space_id)) {
    return PhiVerbResult::Fail("unknown_space");
  }
  return PhiVerbOk(coordinator_->space_manager().DeleteSpace(space_id));
}

PhiVerbResult PhiControlDispatcher::VerbSpacesReorder(
    const PhiVerbContext& ctx) {
  const std::string space_id = ctx.params.String("spaceId");
  const int index = ctx.params.Int("index", -1);
  if (!FindSpace(space_id)) {
    return PhiVerbResult::Fail("unknown_space");
  }
  const bool ok = index >= 0;
  if (ok) {
    coordinator_->ReorderSpace(space_id, index);
  }
  return PhiVerbOk(ok);
}

PhiVerbResult PhiControlDispatcher::VerbSpacesSetTheme(
    const PhiVerbContext& ctx) {
  const std::string space_id = ctx.params.String("spaceId");
  const std::string theme_id = ctx.params.String("themeId");
  if (!FindSpace(space_id)) {
    return PhiVerbResult::Fail("unknown_space");
  }
  // An empty id is legal: it clears the override.
  const bool ok = theme_id.empty() || theme_id == PhiThemeById(theme_id).id;
  if (ok) {
    coordinator_->SetSpaceTheme(space_id, theme_id);
  }
  base::DictValue out;
  out.Set("themeId", coordinator_->SpaceThemeId(space_id));
  return PhiVerbOk(ok, std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSpacesSetIcon(
    const PhiVerbContext& ctx) {
  const std::string icon = ctx.params.String("icon");
  Space* space = FindSpace(ctx.params.String("spaceId"));
  if (!space) {
    return PhiVerbResult::Fail("unknown_space");
  }
  const bool ok = PhiIconIsValid(icon);
  if (ok) {
    space->icon = icon;
    coordinator_->ScheduleSave(PhiCoordinator::StoreDomain::kSpaces);
    coordinator_->registry().InvalidateAll();
  }
  base::DictValue out;
  out.Set("icon", icon);
  return PhiVerbOk(ok, std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSpacesListTabs(
    const PhiVerbContext& ctx) {
  std::string space_id, error;
  const BrowserId browser = UserWindowOf(ctx.scope, &space_id, &error);
  if (!browser.valid() || !framework_) {
    return PhiVerbResult::Fail(error.empty() ? "space_not_open" : error);
  }
  // The window exists but its state has not attached yet (a fresh
  // window's first push is still in flight): a transient worth retrying,
  // never a silent empty list — the Mac's window_not_ready.
  const phi_framework::WindowSnapshot* window =
      coordinator_->window_model().Get(browser);
  if (!window) {
    return PhiVerbResult::Fail("window_not_ready");
  }
  base::ListValue rows;
  {
    for (const phi_framework::TabSnapshot& tab : window->tabs) {
      base::DictValue row;
      row.Set("tabId", tab.id.value);
      row.Set("index", tab.index);
      row.Set("url", tab.url);
      row.Set("title", tab.title);
      row.Set("active", tab.active);
      // The full listable inventory (the Mac's agentTabInventory): normal,
      // open pinned and bookmark-opened tabs alike, `kind` saying which —
      // a bound tab's tag is the stored row's guid (TagFor).
      const char* kind = "normal";
      SidebarTag parsed;
      if (!tab.tag.empty() && ParseTag(tab.tag, &parsed)) {
        if (const std::optional<PhiRow> stored =
                coordinator_->tab_store().Find(parsed.guid)) {
          kind = stored->type == PhiRowType::kPinnedTab ? "pinned"
                                                        : "bookmark";
        }
      }
      row.Set("kind", kind);
      // Present only when set: the stored-cell binding tag (TagFor) and
      // the live split, so a probe can assert bindings without pixels.
      if (!tab.tag.empty()) {
        row.Set("tag", tab.tag);
      }
      if (!tab.split_id.empty()) {
        row.Set("splitId", tab.split_id);
      }
      rows.Append(std::move(row));
    }
  }
  base::DictValue out;
  out.Set("tabs", std::move(rows));
  out.Set("spaceId", space_id);  // A window-only bind learns its Space.
  out.Set("windowId", static_cast<double>(browser.value));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSpacesOpenTab(
    const PhiVerbContext& ctx) {
  std::string space_id, error;
  const BrowserId browser = UserWindowOf(ctx.scope, &space_id, &error);
  const bool activate = ctx.params.Bool("activate", true);
  if (framework_ && !browser.valid() && error == "space_not_open" &&
      !ctx.params.Has("windowId")) {
    // A closed window is not a closed Space: a Space with no window gets
    // one first — the one way to reach it when the user has no browser
    // window at all (the Mac's SpaceManager.openWindow). Surfaced when
    // `activate`, opened behind the user's windows otherwise; answered
    // once the window is up, with windowOpened:true. A {windowId} never
    // opens one: it names a window that must already show the Space.
    const int reply_id =
        RegisterPendingReply(/*document=*/false, /*include_html=*/false);
    coordinator_->OpenUserSpaceWindow(ctx.scope.space_id,
                                      ctx.params.String("url"), activate,
                                      reply_id);
    return PhiVerbResult::Deferred(reply_id);
  }
  if (!browser.valid() || !framework_) {
    return PhiVerbResult::Fail(error.empty() ? "space_not_open" : error);
  }
  const TabId previous_active =
      activate ? TabId() : coordinator_->ActiveTabId(browser);
  // Bypassed like every client-side deliberate open: a URL rule pointing
  // at another Space must not bounce a tab the skill placed HERE.
  framework_->OpenTabBypassingSpaceRouting(browser.value,
                                           ctx.params.String("url").c_str());
  if (previous_active.valid()) {
    // The new tab opened foreground; hand the selection back by id.
    framework_->ActivateTabById(browser.value, previous_active.value);
  }
  base::DictValue out;
  out.Set("windowId", static_cast<double>(browser.value));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSpacesFocus(
    const PhiVerbContext& ctx) {
  // A surfaced agent window in the foreground answers as the agent Space.
  const HWND foreground = ::GetForegroundWindow();
  for (PhiShellWindow* controller :
       coordinator_->registry().Controllers()) {
    if (!controller->is_agent() || !foreground ||
        controller->shell_hwnd() != foreground) {
      continue;
    }
    // An agent window's slot is the browser it opened with (it never
    // switches Space), so the slot number names its window.
    PhiAgentSpaces::Space* agent = coordinator_->agent_spaces().FindByWindow(
        BrowserId{controller->slot().value});
    if (!agent) {
      break;
    }
    base::DictValue out;
    out.Set("spaceId", agent->space_id);
    out.Set("spaceName", "Agent Space");
    out.Set("isAgentSpace", true);
    out.Set("isIncognito", false);
    out.Set("windowId",
            static_cast<double>(
                coordinator_->ResolveBrowser(controller->slot()).value));
    return PhiVerbResult::Ok(std::move(out));
  }
  PhiShellWindow* controller = coordinator_->FocusedUserController();
  if (!controller) {
    return PhiVerbResult::Fail("no_window");
  }
  const std::string space_id = controller->active_space();
  std::string name = space_id;
  // The Space list's own lookup: it knows the live Incognito Spaces too.
  if (const Space* space = coordinator_->space_manager().Find(space_id)) {
    name = space->name;
  }
  // An Incognito Space is runtime-only and off the managed surface: no
  // window and no tab detail (the Mac's handleSpacesFocus).
  const bool incognito_space = model::IsIncognitoSpaceId(space_id);
  base::DictValue out;
  out.Set("spaceId", space_id);
  out.Set("spaceName", name);
  out.Set("isAgentSpace", false);
  out.Set("isIncognito", controller->is_incognito() || incognito_space);
  const BrowserId browser = controller->active_browser_id();
  if (browser.valid() && !incognito_space) {
    out.Set("windowId", static_cast<double>(browser.value));
    if (coordinator_->window_model().ActiveTab(browser)) {
      out.Set("tab", ActiveTabRow(browser));
    }
  }
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSpacesActivate(
    const PhiVerbContext& ctx) {
  const std::string space_id = ctx.params.String("spaceId");
  // A slot already hosting the Space surfaces it there; otherwise the
  // user's focused window takes it (cold-spawning the browser).
  for (PhiShellWindow* controller :
       coordinator_->registry().Controllers()) {
    if (controller->is_agent() || !controller->HasSpace(space_id)) {
      continue;
    }
    const bool ok = controller->ShowSpace(space_id);
    if (ok) {
      coordinator_->PushSpaceMenu();
      if (controller->shell_hwnd()) {
        ::SetForegroundWindow(controller->shell_hwnd());
      }
      controller->ScheduleRefresh();
    }
    return PhiVerbOk(ok);
  }
  PhiShellWindow* focused = coordinator_->FocusedUserController();
  if (!focused) {
    // No user window at all: there is nothing to switch, and saying so
    // beats reporting a switch that did not happen. spaces.openTab opens
    // a window regardless.
    return PhiVerbResult::Fail("no_focused_window");
  }
  bool pending = false, cold = false;
  if (!coordinator_->SwitchSlotToSpace(focused->slot(), space_id, &pending,
                                       &cold)) {
    return PhiVerbResult::Fail("unknown_space");
  }
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbSpacesUpdate(
    const PhiVerbContext& ctx) {
  const std::string space_id = ctx.params.String("spaceId");
  Space* entry = FindSpace(space_id);
  if (!entry) {
    return PhiVerbResult::Fail("unknown_space");
  }
  // Validate everything before writing anything, so a bad field never
  // leaves a half-applied update behind.
  std::string theme_id, icon, error;
  if (!RequestedThemeId(ctx.params, &theme_id, &error) ||
      !RequestedIconName(ctx.params, &icon, &error)) {
    return PhiVerbResult::Fail(error);
  }
  bool ok = true;
  if (const std::string* name = ctx.params.FindString("name");
      name && !base::TrimWhitespaceASCII(*name, base::TRIM_ALL).empty()) {
    ok &= coordinator_->RenameSpace(space_id, *name);
  }
  if (!icon.empty()) {
    entry->icon = icon;
    coordinator_->ScheduleSave(PhiCoordinator::StoreDomain::kSpaces);
    coordinator_->registry().InvalidateAll();
  }
  if (!theme_id.empty()) {
    // The theme is what the window paints with; the stored hex follows it
    // (the Mac's setTheme re-derives colorHex), so a later unpin falls back
    // to the same colour rather than a stale one.
    entry->color_hex = PhiThemeOverlayHex(theme_id);
    coordinator_->ScheduleSave(PhiCoordinator::StoreDomain::kSpaces);
    coordinator_->SetSpaceTheme(space_id, theme_id);
    for (PhiShellWindow* controller :
         coordinator_->registry().Controllers()) {
      controller->ApplyPalette();
    }
  }
  base::DictValue out;
  out.Set("spaceId", space_id);
  // What landed: the theme the Space now renders with and the normalized
  // icon, so the caller can see a snapped colour for what it is.
  const std::string pinned = coordinator_->SpaceThemeId(space_id);
  out.Set("themeId", pinned.empty() ? coordinator_->ThemeId() : pinned);
  if (!icon.empty()) {
    out.Set("iconName", icon);
  }
  return PhiVerbOk(ok, std::move(out));
}

// --- Profiles ---------------------------------------------------------------------

PhiVerbResult PhiControlDispatcher::VerbProfilesList(
    const PhiVerbContext& ctx) {
  base::DictValue out;
  if (framework_) {
    std::optional<base::Value> v = base::JSONReader::Read(
        framework_->ListProfilesJson(), base::JSON_PARSE_RFC);
    if (v && v->is_list()) {
      out.Set("profiles", std::move(*v));
    }
  }
  if (!out.contains("profiles")) {
    out.Set("profiles", base::ListValue());
  }
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbProfilesCreate(
    const PhiVerbContext& ctx) {
  const std::string name = ctx.params.String("name");
  if (!framework_ || name.empty()) {
    // Nothing to ask and nothing to name: the bare refusal, as before.
    return PhiVerbResult::Refuse(PhiErrorCode::kInvalidPayload);
  }
  // Creation is fire-and-forget in the framework, so the duplicate
  // display-name refusal has to happen here — against the profiles that
  // exist AND the ones already on their way.
  std::optional<base::Value> profiles = base::JSONReader::Read(
      framework_->ListProfilesJson(), base::JSON_PARSE_RFC);
  if (profiles && profiles->is_list()) {
    for (const base::Value& v : profiles->GetList()) {
      const base::DictValue* d = v.GetIfDict();
      const std::string* existing = d ? d->FindString("name") : nullptr;
      if (existing && *existing == name) {
        return PhiVerbResult::Fail("duplicate_name");
      }
    }
  }
  for (const PhiCoordinator::PendingProfile& pending :
       coordinator_->pending_profiles()) {
    if (pending.display_name == name) {
      return PhiVerbResult::Fail("duplicate_name");
    }
  }
  // Deferred: the basename exists only once OnProfileCreated names it,
  // and the skill's `const {profileId} = await createProfile(...)` needs
  // it in the reply.
  PhiCoordinator::PendingProfile pending;
  pending.display_name = name;
  pending.then = PhiCoordinator::PendingProfile::Then::kReplyOnly;
  pending.pending_reply_id =
      RegisterPendingReply(/*document=*/false, /*include_html=*/false);
  const int reply_id = pending.pending_reply_id;
  coordinator_->RequestProfile(std::move(pending));
  return PhiVerbResult::Deferred(reply_id);
}

PhiVerbResult PhiControlDispatcher::VerbProfilesRename(
    const PhiVerbContext& ctx) {
  // Both key spellings ride the schema's aliases: the box probes'
  // {id,name} and the Mac skill's {profileId,displayName}.
  return PhiVerbOk(coordinator_->RenameProfileById(ctx.params.String("id"),
                                                   ctx.params.String("name")));
}

PhiVerbResult PhiControlDispatcher::VerbProfilesDelete(
    const PhiVerbContext& ctx) {
  // Deleting a profile erases its data on disk. The skill contract
  // (references/management.md) promises agents can create and rename
  // profiles but never delete them, so from the pipe this is a test-rig
  // verb only; and even the rig has to say `confirm:true`, the nod the
  // Settings pane asks the user for.
  // testRig counts only in a --dev-hooks build (see EvaluatePipeCaps).
  bool test_rig = false;
  if constexpr (kPhiDevHooks) {
    test_rig = coordinator_->settings().test_rig;
  }
  if (ctx.caller == ControlCaller::kPipe && !test_rig) {
    return PhiVerbResult::Fail(PhiErrorCode::kTestRigOnly);
  }
  if (!ctx.params.Bool("confirm", false)) {
    return PhiVerbResult::Fail(PhiErrorCode::kConfirmRequired);
  }
  return PhiVerbOk(coordinator_->DeleteProfileById(ctx.params.String("id")));
}

// --- URL routing rules -------------------------------------------------------------
// Rule "id" is the stored rule's stable id (the Mac's SpaceURLRule.id); the
// old "g<generation>-<index>" spelling still resolves for older probes.

PhiVerbResult PhiControlDispatcher::VerbUrlRulesList(
    const PhiVerbContext& ctx) {
  base::ListValue rules;
  const std::vector<std::string> rows = coordinator_->URLRuleRows();
  for (size_t i = 0; i < rows.size(); ++i) {
    const std::vector<std::string> parts = SplitTabRow(rows[i]);
    base::DictValue d;
    d.Set("id", UrlRuleIdAt(coordinator_->spaces_model(), i));
    d.Set("spaceId", parts.size() > 0 ? parts[0] : "");
    d.Set("host", parts.size() > 1 ? parts[1] : "");
    if (parts.size() > 2 && !parts[2].empty()) {
      d.Set("pathPrefix", parts[2]);
    } else {
      d.Set("pathPrefix", base::Value());
    }
    d.Set("ask", parts.size() > 3 && parts[3] == "1");
    d.Set("sortOrder", static_cast<int>(i));
    rules.Append(std::move(d));
  }
  base::DictValue out;
  out.Set("rules", std::move(rules));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbUrlRulesAdd(
    const PhiVerbContext& ctx) {
  const std::string space_id = ctx.params.String("spaceId");
  if (!SpaceExists(space_id)) {
    return PhiVerbResult::Fail("unknown_space");
  }
  if (RuleHostLooksLikeUrl(ctx.params.String("host"))) {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload);
  }
  coordinator_->AddURLRule(space_id,
                           base::ToLowerASCII(ctx.params.String("host")),
                           ctx.params.String("pathPrefix"),
                           ctx.params.Bool("ask", false));
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::UrlRuleEdit(const PhiVerbContext& ctx,
                                                bool remove) {
  int index = -1;
  if (const std::string* id = ctx.params.FindString("id")) {
    size_t parsed = 0;
    if (ResolveUrlRuleIndex(coordinator_->spaces_model(), *id, &parsed)) {
      index = static_cast<int>(parsed);
    } else {
      return PhiVerbResult::Fail("unknown_rule");
    }
  }
  const std::vector<std::string> rows = coordinator_->URLRuleRows();
  if (index < 0 || index >= static_cast<int>(rows.size())) {
    return PhiVerbResult::Fail(index < 0 ? "invalid_payload" : "unknown_rule");
  }
  if (remove) {
    coordinator_->RemoveURLRule(index);
    return PhiVerbResult::Ok();
  }
  // Update: host/pathPrefix change the pattern; spaceId/ask change the
  // target. Absent fields keep their current value.
  const std::vector<std::string> parts = SplitTabRow(rows[index]);
  std::string space_id = parts.size() > 0 ? parts[0] : "";
  std::string host = parts.size() > 1 ? parts[1] : "";
  std::string prefix = parts.size() > 2 ? parts[2] : "";
  bool ask = parts.size() > 3 && parts[3] == "1";
  if (const std::string* h = ctx.params.FindString("host")) {
    if (RuleHostLooksLikeUrl(*h)) {
      return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload);
    }
    host = base::ToLowerASCII(*h);
  }
  // pathPrefix: key presence is meaningful (present clears/sets, absent
  // keeps) — but our storage can't distinguish; treat present as set.
  if (ctx.params.Has("pathPrefix")) {
    prefix = ctx.params.String("pathPrefix");
  }
  if (const std::string* s = ctx.params.FindString("spaceId")) {
    if (!SpaceExists(*s)) {
      return PhiVerbResult::Fail("unknown_space");
    }
    space_id = *s;
  }
  if (ctx.params.Has("ask")) {
    ask = ctx.params.Bool("ask", ask);
  }
  coordinator_->SetURLRulePattern(index, host, prefix);
  coordinator_->SetURLRuleTarget(index, space_id, ask);
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbUrlRulesUpdate(
    const PhiVerbContext& ctx) {
  return UrlRuleEdit(ctx, /*remove=*/false);
}

PhiVerbResult PhiControlDispatcher::VerbUrlRulesDelete(
    const PhiVerbContext& ctx) {
  return UrlRuleEdit(ctx, /*remove=*/true);
}

// --- Pins and bookmarks, the client dialect (windowId-scoped, flat) ---------------

PhiVerbResult PhiControlDispatcher::VerbPinnedScopeGet(
    const PhiVerbContext& ctx) {
  base::DictValue out;
  out.Set("scope", PhiPinScopeName(coordinator_->tab_store().pin_scope()));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbPinnedScopeSet(
    const PhiVerbContext& ctx) {
  PhiPinScope scope = coordinator_->tab_store().pin_scope();
  bool ok = false;
  if (const std::string* name = ctx.params.FindString("scope")) {
    ok = PhiPinScopeFromName(*name, &scope);
  }
  if (ok) {
    coordinator_->tab_store().SetPinScope(scope);
    coordinator_->MirrorPinScope();
    coordinator_->registry().InvalidateAll();
  }
  base::DictValue out;
  out.Set("scope", PhiPinScopeName(coordinator_->tab_store().pin_scope()));
  return PhiVerbOk(ok, std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbPinnedList(
    const PhiVerbContext& ctx) {
  if (!ctx.scope.valid()) {
    return PhiVerbResult::Fail(ctx.scope.error);  // Not an empty slot.
  }
  const SlotId slot = SlotOf(ctx);
  std::string profile_id, space_id;
  base::ListValue list;
  if (coordinator_->OwnerForSlot(slot, &profile_id, &space_id)) {
    for (const PhiRow& row :
         coordinator_->tab_store().PinnedFor(profile_id, space_id)) {
      base::DictValue entry;
      entry.Set("guid", row.guid);
      entry.Set("title", row.title);
      entry.Set("url", row.url);
      entry.Set("index", row.index);
      entry.Set("favicon", row.favicon_base64);
      entry.Set("faviconWidth", row.favicon_width);
      entry.Set("faviconHeight", row.favicon_height);
      entry.Set("secondaryUrl", row.url_b);
      entry.Set("secondaryTitle", row.title_b);
      entry.Set("layout", row.layout);
      list.Append(std::move(entry));
    }
  }
  base::DictValue out;
  out.Set("pinned", std::move(list));
  out.Set("scope", PhiPinScopeName(coordinator_->tab_store().pin_scope()));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbPinnedOpen(
    const PhiVerbContext& ctx) {
  // The Mac's openOrFocusPinnedTab, the same path a sidebar pin click
  // takes: focus the pin's bound live tab, or open one bound to the pin.
  const std::string guid = ctx.params.String("guid");
  PhiShellWindow* controller = ctx.scope.controller;
  std::string profile_id, space_id, url;
  bool known = false;
  if (controller &&
      coordinator_->OwnerForSlot(controller->slot(), &profile_id, &space_id)) {
    // Pins and sidebar bookmarks both bind (bookmarkGuidBacking) — resolve
    // the guid against either kind of stored row.
    if (const std::optional<PhiRow> row = coordinator_->tab_store().Find(guid);
        row && row->type != PhiRowType::kBookmarkFolder) {
      url = row->url;
      known = true;
    }
  }
  if (!known) {
    return PhiVerbResult::Fail(controller            ? "unknown_pin"
                               : ctx.scope.valid() ? "window_not_open"
                                                   : ctx.scope.error);
  }
  controller->OpenOrFocusPinned(guid, url);
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbPinnedAdd(const PhiVerbContext& ctx) {
  const SlotId slot = SlotOf(ctx);
  std::string title = ctx.params.String("title");
  std::string url = ctx.params.String("url");
  std::string profile_id, space_id, guid;
  base::DictValue out;
  if (coordinator_->OwnerForSlot(slot, &profile_id, &space_id)) {
    // No url given: pin what the slot is showing.
    if (url.empty()) {
      if (const phi_framework::WindowSnapshot* window =
              coordinator_->window_model().Get(
                  coordinator_->ResolveBrowser(slot.value))) {
        url = window->nav.url;
        if (title.empty()) {
          title = window->nav.title;
        }
      }
    }
    int fw = 0, fh = 0;
    // Only when we are pinning what the slot is SHOWING: the live favicon
    // belongs to the active tab, not to some other url the caller named.
    const std::string favicon = url == coordinator_->LiveUrl(slot)
                                    ? coordinator_->CaptureFavicon(slot, &fw,
                                                                   &fh)
                                    : std::string();
    guid = coordinator_->tab_store().AddPinned(profile_id, space_id, title, url,
                                               favicon, fw, fh);
    // Report what was captured: a caller (or a test) can then tell "no icon
    // yet" apart from "the pin failed".
    out.Set("faviconWidth", fw);
    out.Set("faviconHeight", fh);
    out.Set("liveUrl", coordinator_->LiveUrl(slot));
  }
  if (!guid.empty()) {
    coordinator_->registry().InvalidateAll();
  }
  out.Set("guid", guid);
  return PhiVerbOk(!guid.empty(), std::move(out));
}

PhiVerbResult PhiControlDispatcher::StoredRowRemove(
    const PhiVerbContext& ctx) {
  // Route through RemoveStoredRow: it also clears the identity tag from
  // any tab bound to the row (verb and UI paths must behave alike).
  const std::string guid = ctx.params.String("guid");
  const std::optional<PhiRow> row =
      guid.empty() ? std::nullopt : coordinator_->tab_store().Find(guid);
  if (!row) {
    return PhiVerbResult::Refuse(PhiErrorCode::kInvalidPayload);
  }
  // The owner rides the response so the skill can settle against the
  // right Space's list after the row is gone.
  base::DictValue out;
  out.Set("spaceId", row->space_id);
  coordinator_->RemoveStoredRow(guid);
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbPinnedRemove(
    const PhiVerbContext& ctx) {
  return StoredRowRemove(ctx);
}

PhiVerbResult PhiControlDispatcher::VerbBookmarksRemove(
    const PhiVerbContext& ctx) {
  return StoredRowRemove(ctx);
}

PhiVerbResult PhiControlDispatcher::VerbBookmarksListFlat(
    const PhiVerbContext& ctx) {
  if (!ctx.scope.valid()) {
    return PhiVerbResult::Fail(ctx.scope.error);
  }
  const SlotId slot = SlotOf(ctx);
  const std::string parent = ctx.params.String("parentGuid");
  std::string profile_id, space_id;
  base::ListValue list;
  if (coordinator_->OwnerForSlot(slot, &profile_id, &space_id)) {
    for (const PhiRow& row : coordinator_->tab_store().BookmarksFor(
             profile_id, space_id, parent)) {
      base::DictValue entry;
      entry.Set("guid", row.guid);
      entry.Set("title", row.title);
      entry.Set("url", row.url);
      entry.Set("index", row.index);
      entry.Set("folder", row.type == PhiRowType::kBookmarkFolder);
      entry.Set("favicon", row.favicon_base64);
      entry.Set("faviconWidth", row.favicon_width);
      entry.Set("faviconHeight", row.favicon_height);
      entry.Set("secondaryUrl", row.url_b);
      entry.Set("secondaryTitle", row.title_b);
      list.Append(std::move(entry));
    }
  }
  base::DictValue out;
  out.Set("bookmarks", std::move(list));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbBookmarksAddFlat(
    const PhiVerbContext& ctx) {
  const SlotId slot = SlotOf(ctx);
  const bool folder = ctx.params.Bool("folder", false);
  std::string title = ctx.params.String("title");
  std::string url = ctx.params.String("url");
  const std::string parent = ctx.params.String("parentGuid");
  std::string profile_id, space_id, guid;
  if (coordinator_->OwnerForSlot(slot, &profile_id, &space_id)) {
    if (!folder && url.empty()) {
      if (const phi_framework::WindowSnapshot* window =
              coordinator_->window_model().Get(
                  coordinator_->ResolveBrowser(slot.value))) {
        url = window->nav.url;
        if (title.empty()) {
          title = window->nav.title;
        }
      }
    }
    int fw = 0, fh = 0;
    const std::string favicon =
        (!folder && url == coordinator_->LiveUrl(slot))
            ? coordinator_->CaptureFavicon(slot, &fw, &fh)
            : std::string();
    guid = coordinator_->tab_store().AddBookmark(
        profile_id, space_id, parent, title, url, folder, favicon, fw, fh);
  }
  if (!guid.empty()) {
    coordinator_->registry().InvalidateAll();
  }
  base::DictValue out;
  out.Set("guid", guid);
  return PhiVerbOk(!guid.empty(), std::move(out));
}

// --- Bookmarks, the skill's Space-addressed tree forms ------------------------------

namespace {

// The Space a tree-shaped bookmark payload targets: an explicit spaceId,
// else the default Space (else the first).
const model::Space* TreeTargetSpace(PhiCoordinator* coordinator,
                                    const std::string& space_id,
                                    bool* unknown) {
  *unknown = false;
  const auto& spaces = coordinator->spaces_model().spaces;
  if (!space_id.empty()) {
    for (const model::Space& space : spaces) {
      if (space.id == space_id) {
        return &space;
      }
    }
    *unknown = true;
    return nullptr;
  }
  if (spaces.empty()) {
    return nullptr;
  }
  const model::Space* target = &spaces.front();
  for (const model::Space& space : spaces) {
    if (space.id == coordinator->DefaultSpaceId()) {  // The default role.
      target = &space;
      break;
    }
  }
  return target;
}

}  // namespace

PhiVerbResult PhiControlDispatcher::VerbBookmarksListTree(
    const PhiVerbContext& ctx) {
  bool unknown = false;
  const Space* target =
      TreeTargetSpace(coordinator_, ctx.params.String("spaceId"), &unknown);
  if (!target) {
    return PhiVerbResult::Fail("unknown_space");
  }
  (void)unknown;
  const std::string profile_id = target->profile_id;
  const std::string space_id = target->id;
  // Folders carry `children`, leaves carry `url` — the Mac tree shape.
  auto build = [&](auto&& self, const std::string& parent) -> base::ListValue {
    base::ListValue nodes;
    for (const PhiRow& row : coordinator_->tab_store().BookmarksFor(
             profile_id, space_id, parent)) {
      base::DictValue node;
      node.Set("guid", row.guid);
      node.Set("title", row.title);
      node.Set("index", row.index);
      if (row.type == PhiRowType::kBookmarkFolder) {
        node.Set("children", self(self, row.guid));
      } else {
        node.Set("url", row.url);
      }
      nodes.Append(std::move(node));
    }
    return nodes;
  };
  base::DictValue out;
  out.Set("bookmarks", build(build, ""));
  out.Set("spaceId", space_id);
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::BookmarkTreeAdd(const PhiVerbContext& ctx,
                                                    bool folder) {
  bool unknown = false;
  const Space* target =
      TreeTargetSpace(coordinator_, ctx.params.String("spaceId"), &unknown);
  if (!target) {
    return PhiVerbResult::Fail("unknown_space");
  }
  (void)unknown;
  const std::string profile_id = target->profile_id;
  const std::string space_id = target->id;
  std::string parent;
  if (const std::string* p = ctx.params.FindString("parentGuid")) {
    const std::optional<PhiRow> parent_row = coordinator_->tab_store().Find(*p);
    if (!parent_row || parent_row->type != PhiRowType::kBookmarkFolder ||
        parent_row->space_id != space_id) {
      return PhiVerbResult::Fail("unknown_folder");
    }
    parent = *p;
  }
  const std::string guid = coordinator_->tab_store().AddBookmark(
      profile_id, space_id, parent, ctx.params.String("title"),
      folder ? std::string() : ctx.params.String("url"), folder,
      /*favicon_base64=*/"", 0, 0);
  if (guid.empty()) {
    return PhiVerbResult::Fail("create_failed");
  }
  if (std::optional<int> index = ctx.params.FindInt("index")) {
    coordinator_->tab_store().Reorder(guid, *index);
  }
  coordinator_->registry().InvalidateAll();
  base::DictValue out;
  out.Set("guid", guid);
  out.Set("spaceId", space_id);
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbBookmarksAddTree(
    const PhiVerbContext& ctx) {
  return BookmarkTreeAdd(ctx, /*folder=*/false);
}

PhiVerbResult PhiControlDispatcher::VerbBookmarksAddFolder(
    const PhiVerbContext& ctx) {
  return BookmarkTreeAdd(ctx, /*folder=*/true);
}

PhiVerbResult PhiControlDispatcher::BookmarkTreeOp(const PhiVerbContext& ctx,
                                                   std::string_view op) {
  const std::string guid = ctx.params.String("guid");
  const std::optional<PhiRow> row = coordinator_->tab_store().Find(guid);
  if (!row || (row->type != PhiRowType::kBookmark &&
               row->type != PhiRowType::kBookmarkFolder)) {
    return PhiVerbResult::Fail("unknown_bookmark");
  }
  const std::string owner_space = row->space_id;
  bool ok = true;
  if (op == "update") {
    if (const std::string* title = ctx.params.FindString("title")) {
      ok &= coordinator_->RenameStoredRow(guid, *title);
    }
    if (const std::string* icon = ctx.params.FindString("icon")) {
      // Folders only, as on Mac; an unknown id resolves to "default".
      ok &= row->type == PhiRowType::kBookmarkFolder &&
            coordinator_->SetStoredRowIcon(guid, PhiResolveFolderIcon(*icon));
    }
    if (const std::string* layout = ctx.params.FindString("layout")) {
      // Split rows only; the divider axis the pair reopens with.
      ok &= (*layout == "vertical" || *layout == "horizontal") &&
            coordinator_->SetStoredRowLayout(guid, *layout);
    }
    if (const std::string* url = ctx.params.FindString("url")) {
      ok &= coordinator_->tab_store().SetRowUrl(guid, *url);
    }
  } else {
    ok = coordinator_->tab_store().Move(guid, ctx.params.String("parentGuid"),
                                        ctx.params.Int("index", -1));
  }
  coordinator_->registry().InvalidateAll();
  base::DictValue out;
  out.Set("spaceId", owner_space);
  return PhiVerbOk(ok, std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbBookmarksUpdate(
    const PhiVerbContext& ctx) {
  return BookmarkTreeOp(ctx, "update");
}

PhiVerbResult PhiControlDispatcher::VerbBookmarksMove(
    const PhiVerbContext& ctx) {
  return BookmarkTreeOp(ctx, "move");
}

// --- Pinned tabs, the skill dialect (per-profile records) ---------------------------

PhiVerbResult PhiControlDispatcher::VerbPinnedTabsList(
    const PhiVerbContext& ctx) {
  std::string profile_id, space_id;
  if (!DialectOwner(ctx.params.String("profileId"), &profile_id, &space_id)) {
    return PhiVerbResult::Fail("no_window");
  }
  base::ListValue rows;
  for (const PhiRow& row :
       coordinator_->tab_store().PinnedFor(profile_id, space_id)) {
    base::DictValue entry;
    entry.Set("guid", row.guid);
    entry.Set("url", row.url);
    entry.Set("title", row.title);
    entry.Set("index", row.index);
    entry.Set("profileId",
              row.profile_id.empty() ? profile_id : row.profile_id);
    rows.Append(std::move(entry));
  }
  base::DictValue out;
  out.Set("pinnedTabs", std::move(rows));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbPinnedTabsAdd(
    const PhiVerbContext& ctx) {
  std::string profile_id, space_id;
  if (!DialectOwner(ctx.params.String("profileId"), &profile_id, &space_id)) {
    return PhiVerbResult::Fail("no_window");
  }
  const std::string guid = coordinator_->tab_store().AddPinned(
      profile_id, space_id, ctx.params.String("title"),
      ctx.params.String("url"), /*favicon_base64=*/"", 0, 0);
  if (guid.empty()) {
    return PhiVerbResult::Fail("create_failed");
  }
  if (std::optional<int> index = ctx.params.FindInt("index")) {
    coordinator_->tab_store().Reorder(guid, *index);
  }
  coordinator_->registry().InvalidateAll();
  const std::optional<PhiRow> row = coordinator_->tab_store().Find(guid);
  base::DictValue out;
  out.Set("guid", guid);
  out.Set("profileId",
          row && !row->profile_id.empty() ? row->profile_id : profile_id);
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::PinnedTabsOp(const PhiVerbContext& ctx,
                                                 std::string_view op) {
  const std::string guid = ctx.params.String("guid");
  if (!coordinator_->tab_store().Contains(guid)) {
    return PhiVerbResult::Fail("unknown_pin");
  }
  if (op == "remove") {
    coordinator_->RemoveStoredRow(guid);
    return PhiVerbResult::Ok();
  }
  bool ok = true;
  if (const std::string* title = ctx.params.FindString("title")) {
    ok &= coordinator_->RenameStoredRow(guid, *title);
  }
  if (const std::string* url = ctx.params.FindString("url")) {
    ok &= coordinator_->tab_store().SetRowUrl(guid, *url);
  }
  coordinator_->registry().InvalidateAll();
  return PhiVerbOk(ok);
}

PhiVerbResult PhiControlDispatcher::VerbPinnedTabsUpdate(
    const PhiVerbContext& ctx) {
  return PinnedTabsOp(ctx, "update");
}

PhiVerbResult PhiControlDispatcher::VerbPinnedTabsRemove(
    const PhiVerbContext& ctx) {
  return PinnedTabsOp(ctx, "remove");
}

// --- Rows and the emoji catalog --------------------------------------------------

PhiVerbResult PhiControlDispatcher::VerbRowsReorder(
    const PhiVerbContext& ctx) {
  const std::string guid = ctx.params.String("guid");
  const int index = ctx.params.Int("index", -1);
  const bool ok =
      !guid.empty() && index >= 0 && coordinator_->tab_store().Contains(guid);
  if (ok && coordinator_->tab_store().Reorder(guid, index)) {
    coordinator_->registry().InvalidateAll();
  }
  return PhiVerbOk(ok);
}

PhiVerbResult PhiControlDispatcher::VerbEmojiGroups(
    const PhiVerbContext& ctx) {
  // The catalog the Mac bundles, offered verbatim.
  base::ListValue groups;
  for (const PhiEmojiGroup& group : PhiEmojiGroups()) {
    base::DictValue g;
    g.Set("name", group.name);
    g.Set("count", static_cast<int>(group.items.size()));
    groups.Append(std::move(g));
  }
  base::DictValue out;
  out.Set("groups", std::move(groups));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbEmojiList(const PhiVerbContext& ctx) {
  const std::string want = ctx.params.String("group");
  const int limit = ctx.params.Int("limit", 64);
  base::ListValue items;
  for (const PhiEmojiGroup& group : PhiEmojiGroups()) {
    if (!want.empty() && group.name != want) {
      continue;
    }
    for (const PhiEmoji& emoji : group.items) {
      if (static_cast<int>(items.size()) >= limit) {
        break;
      }
      base::DictValue e;
      e.Set("id", emoji.id);
      e.Set("text", emoji.text);
      e.Set("name", emoji.name);
      items.Append(std::move(e));
    }
  }
  base::DictValue out;
  out.Set("emoji", std::move(items));
  return PhiVerbResult::Ok(std::move(out));
}

}  // namespace phi_client

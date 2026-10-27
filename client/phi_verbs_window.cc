// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_control_dispatcher.h"
#include "phibrowser-win/client/phi_coordinator.h"
#include "phibrowser-win/client/phi_feedback_outbox.h"
#include "phibrowser-win/client/phi_feedback_window.h"
#include "phibrowser-win/client/phi_app_menu.h"
#include "phibrowser-win/client/phi_settings_window.h"
#include "phibrowser-win/client/phi_tab_store.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_shell_window.h"
#include "phibrowser-win/client/phi_window_registry.h"

// Windows, tabs, navigation, tab groups, split view, downloads, and the
// app-wide looks, settings and feedback verbs. One handler per registry row.

namespace phi_client {

namespace {

// The wire name of a tab group colour — the TabGroupsJson vocabulary, so
// the skill's tabGroups.list reads the same whether it came from the model
// or the framework's own serialisation.
const char* GroupColorName(phi_framework::TabGroupColor color) {
  using phi_framework::TabGroupColor;
  switch (color) {
    case TabGroupColor::kGrey:
      return "grey";
    case TabGroupColor::kBlue:
      return "blue";
    case TabGroupColor::kRed:
      return "red";
    case TabGroupColor::kYellow:
      return "yellow";
    case TabGroupColor::kGreen:
      return "green";
    case TabGroupColor::kPink:
      return "pink";
    case TabGroupColor::kPurple:
      return "purple";
    case TabGroupColor::kCyan:
      return "cyan";
    case TabGroupColor::kOrange:
      return "orange";
  }
  return "grey";
}

// The `tabIds` list as the JSON string the framework's by-id calls take.
std::string TabIdsJson(const PhiParams& params) {
  std::string json = "[]";
  if (const base::ListValue* ids = params.FindList("tabIds")) {
    base::JSONWriter::Write(*ids, &json);
  }
  return json;
}

base::DictValue RectDict(const RECT& r) {
  base::DictValue d;
  d.Set("x", static_cast<int>(r.left));
  d.Set("y", static_cast<int>(r.top));
  d.Set("width", static_cast<int>(r.right - r.left));
  d.Set("height", static_cast<int>(r.bottom - r.top));
  return d;
}

}  // namespace

// --- Windows -------------------------------------------------------------------

PhiVerbResult PhiControlDispatcher::VerbNewWindow(const PhiVerbContext& ctx) {
  // A kUser request in the first user window's profile — the framework
  // wants a LIVE browser as the source, so the slot resolves to the
  // browser it is showing, never its (possibly long gone) original. With
  // no user window at all, the default-shell rescue does the same job.
  const SlotId first = coordinator_->registry().FirstUserSlot();
  if (!first.valid()) {
    coordinator_->OpenDefaultShellIfNoUserWindow(
        "newWindow without a user window");
    return PhiVerbOk(coordinator_->registry().HasPending());
  }
  PendingCreation pending;
  pending.kind = phi_framework::WindowKind::kUser;
  const RequestToken token = coordinator_->RequestCreation(
      phi_framework::WindowKind::kUser, std::string(),
      /*incognito=*/false, coordinator_->ResolveBrowser(first),
      std::move(pending));
  return PhiVerbOk(token.valid());
}

PhiVerbResult PhiControlDispatcher::VerbClose(const PhiVerbContext& ctx) {
  // Closing a SLOT means closing every Space browser it hosts, not just the
  // browser its id names — same reason the shell window's close box does.
  const int64_t window_id = ctx.params.Int64("windowId", 0);
  bool ok = false;
  if (PhiShellWindow* controller = ctx.scope.controller) {
    ok = true;  // Set first: the call below may destroy the controller.
    controller->RequestClose();
  } else if (framework_ && window_id &&
             framework_->Snapshot(BrowserId{window_id})) {
    // A browser no shell hosts (a hosted window); an id the framework does
    // not know either is the scope's refusal, never an accepted close.
    framework_->CloseBrowserWindow(window_id);
    ok = true;
  }
  return PhiVerbOk(ok);
}

PhiVerbResult PhiControlDispatcher::VerbNavigate(const PhiVerbContext& ctx) {
  const int64_t window_id = ctx.params.Int64("windowId", 0);
  const std::string url = ctx.params.String("url");
  const bool ok =
      framework_ && window_id && ctx.scope.valid() && !url.empty();
  if (ok) {
    framework_->Navigate(ctx.scope.browser.value, url.c_str());
  }
  return PhiVerbOk(ok);
}

PhiVerbResult PhiControlDispatcher::VerbWindowProfile(
    const PhiVerbContext& ctx) {
  base::DictValue out;
  if (const phi_framework::WindowSnapshot* window =
          coordinator_->window_model().Get(ctx.scope.browser)) {
    out.Set("id", window->profile_id);
    out.Set("name", coordinator_->ProfileDisplayName(window->profile_id));
    out.Set("incognito", window->incognito);
  }
  const bool ok = out.contains("id");
  return PhiVerbOk(ok, std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbWindowLayout(
    const PhiVerbContext& ctx) {
  // The chrome geometry a slot is rendering, so layout can be asserted
  // without going through pixels.
  PhiShellWindow* shell = ctx.scope.controller;
  if (!shell) {
    return PhiVerbOk(false);
  }
  shell->FlushPendingRebuild();  // The rows below must not be stale.
  base::DictValue out;
  out.Set("content", RectDict(shell->content_rect()));
  // Geometry is in PHYSICAL pixels; a caller comparing against design
  // constants has to scale them by this.
  out.Set("dpi", static_cast<int>(shell->dpi()));
  out.Set("urlField", RectDict(shell->url_field_rect()));
  // Reader View's button, and the two verdicts behind it. An empty rect
  // means the reader is not being offered for the active tab; the two
  // flags say which signal (if either) would have offered it.
  out.Set("reader", RectDict(shell->reader_button_rect()));
  out.Set("readerProbe", shell->ActiveTabExtensionOfferable());
  out.Set("readerDistillable", shell->ActiveTabDistillable());
  out.Set("readerActive", shell->ActiveTabShowsReader());
  // The caption band's sidebar toggle (the vertical layouts), and the
  // collapse it drives — the floating panel's trigger is the collapsed
  // card's left margin.
  out.Set("sidebarToggle", RectDict(shell->sidebar_toggle_rect()));
  // The app menu button (⋯), every layout's way to the menu.
  out.Set("appMenu", RectDict(shell->app_menu_rect()));
  // The Update reminder pill; empty unless an update is on offer.
  out.Set("update", RectDict(shell->update_rect()));
  // The in-field Copy link / site-menu pair (laid out always, painted only
  // while the field is hovered).
  out.Set("copyLink", RectDict(shell->copy_link_rect()));
  out.Set("siteMenu", RectDict(shell->site_menu_rect()));
  out.Set("sidebarCollapsed", shell->sidebar_collapsed());
  out.Set("sidebarFloating", shell->sidebar_floating());
  out.Set("tabsTop", shell->tabs_top());
  out.Set("rowHeight", shell->tab_row_height());
  out.Set("rowGap", shell->tab_row_gap());
  out.Set("titleBar", shell->title_bar_height());
  out.Set("sidebarWidth", shell->sidebar_width());
  out.Set("contentMargin", shell->content_margin());
  const POINT frame = shell->frame_offset();
  out.Set("frameLeft", static_cast<int>(frame.x));
  out.Set("frameTop", static_cast<int>(frame.y));
  out.Set("mode", PhiLayoutModeName(coordinator_->settings().layout_mode));
  // The page colour the address row follows (the Mac's WebContentHeader page
  // colour), "" when the row keeps the native chrome.
  if (const std::optional<PhiColor> page = shell->ActivePageColor()) {
    out.Set("pageColor", base::StringPrintf("#%02X%02X%02X", PhiRed(*page),
                                            PhiGreen(*page), PhiBlue(*page)));
  } else {
    out.Set("pageColor", "");
  }
  out.Set("rows", shell->LayoutRowsJson());
  out.Set("drag", shell->DragStateJson());
  // The surface registry's view of this window: origin tab -> kind.
  base::DictValue surfaces;
  for (const auto& [tab, kind] :
       coordinator_->surfaces().Surfaces(shell->active_browser_id())) {
    surfaces.Set(std::to_string(tab),
                 kind == SidebarSurface::kPeek ? "peek" : "reader");
  }
  out.Set("surfaces", std::move(surfaces));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbWindowBounds(
    const PhiVerbContext& ctx) {
  PhiShellWindow* shell = ctx.scope.controller;
  int x = 0, y = 0, w = 0, h = 0;
  if (!shell || !shell->GetBounds(&x, &y, &w, &h)) {
    return PhiVerbOk(false);
  }
  base::DictValue out;
  out.Set("x", x);
  out.Set("y", y);
  out.Set("width", w);
  out.Set("height", h);
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbWindowSetBounds(
    const PhiVerbContext& ctx) {
  const int x = static_cast<int>(ctx.params.Double("x", 0));
  const int y = static_cast<int>(ctx.params.Double("y", 0));
  const int w = static_cast<int>(ctx.params.Double("width", 0));
  const int h = static_cast<int>(ctx.params.Double("height", 0));
  PhiShellWindow* shell = ctx.scope.controller;
  return PhiVerbOk(shell && w > 0 && h > 0 && shell->SetBounds(x, y, w, h));
}

PhiVerbResult PhiControlDispatcher::VerbWindowHandle(
    const PhiVerbContext& ctx) {
  // The slot's shell HWND, so a test/screenshot tool can capture exactly
  // the window it is driving (several slots may exist).
  PhiShellWindow* shell =
      coordinator_->registry().Resolve(ctx.params.Int64("windowId", 0));
  if (!shell || !shell->shell_hwnd()) {
    return PhiVerbResult::Fail(PhiErrorCode::kWindowNotOpen);
  }
  base::DictValue out;
  out.Set("hwnd",
          static_cast<double>(reinterpret_cast<intptr_t>(shell->shell_hwnd())));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbMenuList(const PhiVerbContext& ctx) {
  // The shell window's app menu (⋯) as the user would see it, dynamic rows
  // included — the automated stand-in for opening it by hand. `menus` is
  // the root's rows; submenus nest under "items".
  PhiShellWindow* controller = ctx.scope.controller;
  if (!controller || !controller->app_menu()) {
    return PhiVerbOk(false);
  }
  std::optional<base::Value> menus = base::JSONReader::Read(
      controller->app_menu()->ListJson(), base::JSON_PARSE_RFC);
  if (!menus) {
    return PhiVerbOk(false);
  }
  base::DictValue out;
  out.Set("menus", std::move(*menus));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbMenuClick(const PhiVerbContext& ctx) {
  // Activates a menu item by its title path from the app menu's root, e.g.
  // ["New tab"] or ["More tools","Task manager"]. Disabled items refuse,
  // as a real click would.
  const std::vector<std::string> path = ctx.params.StringList("path");
  PhiShellWindow* controller = ctx.scope.controller;
  return PhiVerbOk(controller && controller->app_menu() && !path.empty() &&
                   controller->app_menu()->InvokePath(path));
}

// --- Tabs -----------------------------------------------------------------------

PhiVerbResult PhiControlDispatcher::VerbTabsList(const PhiVerbContext& ctx) {
  if (!ctx.scope.valid()) {
    return PhiVerbResult::Fail(ctx.scope.error);  // Not an empty strip.
  }
  const int64_t window_id = ctx.params.Int64("windowId", 0);
  base::DictValue out;
  if (framework_ && window_id) {
    std::optional<base::Value> tabs = base::JSONReader::Read(
        framework_->ListTabsJson(ctx.scope.browser.value), base::JSON_PARSE_RFC);
    if (tabs && tabs->is_list()) {
      out.Set("tabs", std::move(*tabs));
    }
  }
  if (!out.contains("tabs")) {
    out.Set("tabs", base::ListValue());
  }
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbTabsNew(const PhiVerbContext& ctx) {
  const int64_t window_id = ctx.params.Int64("windowId", 0);
  TabId opened;
  int index = -1;
  if (framework_ && window_id) {
    const BrowserId browser = ctx.scope.browser;
    opened = framework_->OpenTab(browser, ctx.params.String("url").c_str());
    // The wire reply is index-shaped; the strip just changed, so pull.
    if (opened.valid() &&
        coordinator_->window_model().Refresh(framework_, browser)) {
      if (const phi_framework::TabSnapshot* tab =
              coordinator_->window_model().FindTab(browser, opened)) {
        index = tab->index;
      }
    }
  }
  base::DictValue out;
  out.Set("index", index);
  out.Set("tabId", opened.value);
  return PhiVerbOk(opened.valid(), std::move(out));
}

PhiVerbResult PhiControlDispatcher::TabByIndex(const PhiVerbContext& ctx,
                                               std::string_view op) {
  const int64_t window_id = ctx.params.Int64("windowId", 0);
  const int index = ctx.params.Int("index", -1);
  bool ok = false;
  // The wire names a strip index; the model turns it into the id the
  // framework is asked about, so a missing or stale index names nothing
  // (never tab 0).
  const BrowserId browser = ctx.scope.browser;
  const phi_framework::TabSnapshot* tab =
      (framework_ && window_id)
          ? coordinator_->window_model().FindTabAt(browser, index)
          : nullptr;
  if (tab) {
    const TabId id = tab->id;
    if (op == "activate") {
      ok = framework_->ActivateTabById(browser.value, id.value);
    } else if (op == "close") {
      ok = framework_->CloseTabById(browser.value, id.value);
    } else if (op == "split") {
      // Splits the ACTIVE tab with the tab at `index` (native side-by-side;
      // the sidebar shows one merged row).
      const TabId active = coordinator_->ActiveTabId(browser);
      const char* created =
          active.valid() && active != id
              ? framework_->CreateSplitViewForTabs(browser.value, active.value,
                                                   id.value, "vertical")
              : nullptr;
      ok = created && *created;
    } else {
      ok = framework_->UnsplitById(browser, id);
    }
  }
  return PhiVerbOk(ok);
}

PhiVerbResult PhiControlDispatcher::VerbTabsActivate(
    const PhiVerbContext& ctx) {
  return TabByIndex(ctx, "activate");
}

PhiVerbResult PhiControlDispatcher::VerbTabsClose(const PhiVerbContext& ctx) {
  return TabByIndex(ctx, "close");
}

PhiVerbResult PhiControlDispatcher::VerbTabsSplit(const PhiVerbContext& ctx) {
  return TabByIndex(ctx, "split");
}

PhiVerbResult PhiControlDispatcher::VerbTabsUnsplit(
    const PhiVerbContext& ctx) {
  return TabByIndex(ctx, "unsplit");
}

PhiVerbResult PhiControlDispatcher::VerbTabsMove(const PhiVerbContext& ctx) {
  const int64_t window_id = ctx.params.Int64("windowId", 0);
  const int from = ctx.params.Int("from", -1);
  const int to = ctx.params.Int("to", -1);
  // MoveTab(from, to) meant "final index `to`"; by id that is "after the
  // tab now at `to`" when moving down and "before it" when moving up.
  bool ok = false;
  if (framework_ && window_id && from >= 0 && to >= 0) {
    const BrowserId browser = ctx.scope.browser;
    const phi_framework::TabSnapshot* moving =
        coordinator_->window_model().FindTabAt(browser, from);
    const phi_framework::TabSnapshot* anchor =
        coordinator_->window_model().FindTabAt(browser, to);
    // The verb's contract (suite-tabs): a split member is refused rather
    // than dragging its pair along, which is what the anchored move would
    // do. The sidebar's own drags go through SidebarMove and keep the pair
    // semantics.
    if (moving && anchor && moving->split_id.empty()) {
      ok = from == to ||
           (to > from ? framework_->MoveTabAfter(browser.value,
                                                 moving->id.value,
                                                 anchor->id.value, nullptr)
                      : framework_->MoveTabBefore(browser.value,
                                                  moving->id.value,
                                                  anchor->id.value, nullptr));
    }
  }
  return PhiVerbOk(ok);
}

PhiVerbResult PhiControlDispatcher::GroupByIndex(const PhiVerbContext& ctx,
                                                 bool group) {
  const int64_t window_id = ctx.params.Int64("windowId", 0);
  const int index = ctx.params.Int("index", -1);
  const BrowserId browser = ctx.scope.browser;
  const phi_framework::TabSnapshot* tab =
      (framework_ && window_id)
          ? coordinator_->window_model().FindTabAt(browser, index)
          : nullptr;
  bool ok = false;
  if (tab) {
    ok = group ? framework_->AddTabToNewGroupById(browser, tab->id)
               : framework_->RemoveTabFromGroupById(browser, tab->id);
  }
  return PhiVerbOk(ok);
}

PhiVerbResult PhiControlDispatcher::VerbTabsGroup(const PhiVerbContext& ctx) {
  return GroupByIndex(ctx, /*group=*/true);
}

PhiVerbResult PhiControlDispatcher::VerbTabsUngroup(
    const PhiVerbContext& ctx) {
  return GroupByIndex(ctx, /*group=*/false);
}

PhiVerbResult PhiControlDispatcher::VerbTabsStoreSplit(
    const PhiVerbContext& ctx) {
  // Stores a live split the row menu's two ways: a pinned pair
  // (`asBookmark` false) or a split bookmark.
  const int64_t window_id = ctx.params.Int64("windowId", 0);
  const BrowserId browser = ctx.scope.browser;
  const phi_framework::TabSnapshot* a = coordinator_->window_model().FindTabAt(
      browser, ctx.params.Int("indexA", -1));
  const phi_framework::TabSnapshot* b = coordinator_->window_model().FindTabAt(
      browser, ctx.params.Int("indexB", -1));
  bool ok = false;
  if (window_id && a && b) {
    coordinator_->StoreSplitInternal(SlotId{window_id}, a->id, b->id,
                                     ctx.params.Bool("asBookmark", false),
                                     /*insert_index=*/-1,
                                     /*bind_bookmark=*/false);
    ok = true;
  }
  return PhiVerbOk(ok);
}

// --- Navigation --------------------------------------------------------------------

PhiVerbResult PhiControlDispatcher::VerbNavState(const PhiVerbContext& ctx) {
  if (!ctx.scope.valid()) {
    return PhiVerbResult::Fail(ctx.scope.error);
  }
  const int64_t window_id = ctx.params.Int64("windowId", 0);
  base::DictValue out;
  if (const phi_framework::WindowSnapshot* window =
          window_id ? coordinator_->window_model().Get(ctx.scope.browser)
                    : nullptr) {
    out.Set("url", window->nav.url);
    out.Set("title", window->nav.title);
    out.Set("canGoBack", window->nav.can_go_back);
    out.Set("canGoForward", window->nav.can_go_forward);
    out.Set("loading", window->nav.loading);
  }
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::NavigateHistory(const PhiVerbContext& ctx,
                                                    std::string_view op) {
  const int64_t window_id = ctx.params.Int64("windowId", 0);
  bool ok = false;
  if (framework_ && window_id) {
    const int64_t browser = ctx.scope.browser.value;
    if (op == "back") {
      ok = framework_->GoBack(browser);
    } else if (op == "forward") {
      ok = framework_->GoForward(browser);
    } else if (op == "reload") {
      ok = framework_->Reload(browser, ctx.params.Bool("bypassCache", false));
    } else {
      ok = framework_->StopLoading(browser);
    }
  }
  return PhiVerbOk(ok);
}

PhiVerbResult PhiControlDispatcher::VerbNavBack(const PhiVerbContext& ctx) {
  return NavigateHistory(ctx, "back");
}

PhiVerbResult PhiControlDispatcher::VerbNavForward(const PhiVerbContext& ctx) {
  return NavigateHistory(ctx, "forward");
}

PhiVerbResult PhiControlDispatcher::VerbNavReload(const PhiVerbContext& ctx) {
  return NavigateHistory(ctx, "reload");
}

PhiVerbResult PhiControlDispatcher::VerbNavStop(const PhiVerbContext& ctx) {
  return NavigateHistory(ctx, "stop");
}

// --- Tab groups, the client dialect --------------------------------------------------

PhiVerbResult PhiControlDispatcher::VerbGroupsList(const PhiVerbContext& ctx) {
  if (!ctx.scope.valid()) {
    return PhiVerbResult::Fail(ctx.scope.error);
  }
  const int64_t window_id = ctx.params.Int64("windowId", 0);
  if (!framework_ || !window_id) {
    return PhiVerbOk(false);
  }
  base::DictValue out;
  if (std::optional<base::Value> groups = base::JSONReader::Read(
          framework_->TabGroupsJson(ctx.scope.browser.value),
          base::JSON_PARSE_RFC)) {
    out.Set("groups", std::move(*groups));
  }
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::GroupOp(const PhiVerbContext& ctx,
                                            std::string_view op) {
  const int64_t window_id = ctx.params.Int64("windowId", 0);
  const std::string group_id = ctx.params.String("groupId");
  bool ok = false;
  if (framework_ && window_id && !group_id.empty()) {
    const int64_t browser = ctx.scope.browser.value;
    if (op == "rename") {
      ok = framework_->RenameTabGroup(browser, group_id.c_str(),
                                      ctx.params.String("title").c_str());
    } else if (op == "color") {
      ok = framework_->SetTabGroupColor(browser, group_id.c_str(),
                                        ctx.params.String("color").c_str());
    } else if (op == "collapse") {
      ok = framework_->SetTabGroupCollapsed(
          browser, group_id.c_str(), ctx.params.Bool("collapsed", false));
    } else if (op == "ungroup") {
      ok = framework_->UngroupTabGroup(browser, group_id.c_str());
    } else {
      ok = framework_->CloseTabGroup(browser, group_id.c_str());
    }
  }
  return PhiVerbOk(ok);
}

PhiVerbResult PhiControlDispatcher::VerbGroupsRename(
    const PhiVerbContext& ctx) {
  return GroupOp(ctx, "rename");
}

PhiVerbResult PhiControlDispatcher::VerbGroupsColor(
    const PhiVerbContext& ctx) {
  return GroupOp(ctx, "color");
}

PhiVerbResult PhiControlDispatcher::VerbGroupsCollapse(
    const PhiVerbContext& ctx) {
  return GroupOp(ctx, "collapse");
}

PhiVerbResult PhiControlDispatcher::VerbGroupsUngroup(
    const PhiVerbContext& ctx) {
  return GroupOp(ctx, "ungroup");
}

PhiVerbResult PhiControlDispatcher::VerbGroupsClose(
    const PhiVerbContext& ctx) {
  return GroupOp(ctx, "close");
}

// --- Tab groups, the skill dialect (scope + stable tab ids) ---------------------------

PhiVerbResult PhiControlDispatcher::TabGroupsOp(const PhiVerbContext& ctx,
                                                std::string_view op) {
  const BrowserId browser = ctx.scope.browser;
  if (!ctx.scope.valid() || !browser.valid() || !framework_) {
    return PhiVerbResult::Fail(
        ctx.scope.error.empty() ? "space_not_open" : ctx.scope.error);
  }
  const std::string token = ctx.params.String("token");
  const std::string tab_ids_json = TabIdsJson(ctx.params);
  if (op == "list") {
    base::ListValue groups;
    if (const phi_framework::WindowSnapshot* window =
            coordinator_->window_model().Get(browser)) {
      for (const phi_framework::GroupSnapshot& g : window->groups) {
        base::DictValue row;
        row.Set("token", g.id);
        row.Set("title", g.title);
        row.Set("color", GroupColorName(g.color));
        row.Set("collapsed", g.collapsed);
        base::ListValue member_ids;
        for (const phi_framework::TabSnapshot& tab : window->tabs) {
          if (tab.group_id == g.id) {
            member_ids.Append(tab.id.value);
          }
        }
        row.Set("tabIds", std::move(member_ids));
        groups.Append(std::move(row));
      }
    }
    base::DictValue out;
    out.Set("groups", std::move(groups));
    return PhiVerbResult::Ok(std::move(out));
  }
  if (op == "create") {
    const std::string created =
        framework_->CreateTabGroupForTabs(browser.value, tab_ids_json.c_str());
    if (created.empty()) {
      return PhiVerbResult::Fail("create_failed");
    }
    if (const std::string* title = ctx.params.FindString("title")) {
      framework_->RenameTabGroup(browser.value, created.c_str(),
                                 title->c_str());
    }
    if (const std::string* color = ctx.params.FindString("color")) {
      framework_->SetTabGroupColor(browser.value, created.c_str(),
                                   color->c_str());
    }
    base::DictValue out;
    out.Set("token", created);
    return PhiVerbResult::Ok(std::move(out));
  }
  if (op == "update") {
    bool ok = true;
    if (const std::string* title = ctx.params.FindString("title")) {
      ok &= framework_->RenameTabGroup(browser.value, token.c_str(),
                                       title->c_str());
    }
    if (const std::string* color = ctx.params.FindString("color")) {
      ok &= framework_->SetTabGroupColor(browser.value, token.c_str(),
                                         color->c_str());
    }
    if (std::optional<bool> collapsed = ctx.params.FindBool("collapsed")) {
      ok &= framework_->SetTabGroupCollapsed(browser.value, token.c_str(),
                                             *collapsed);
    }
    return PhiVerbOk(ok);
  }
  if (op == "addTabs") {
    return PhiVerbOk(framework_->AddTabsToGroup(browser.value, token.c_str(),
                                                tab_ids_json.c_str()));
  }
  if (op == "removeTabs") {
    return PhiVerbOk(framework_->RemoveTabsFromGroupByIds(
        browser.value, tab_ids_json.c_str()));
  }
  return PhiVerbOk(op == "ungroup"
                       ? framework_->UngroupTabGroup(browser.value,
                                                     token.c_str())
                       : framework_->CloseTabGroup(browser.value,
                                                   token.c_str()));
}

PhiVerbResult PhiControlDispatcher::VerbTabGroupsList(
    const PhiVerbContext& ctx) {
  return TabGroupsOp(ctx, "list");
}

PhiVerbResult PhiControlDispatcher::VerbTabGroupsCreate(
    const PhiVerbContext& ctx) {
  return TabGroupsOp(ctx, "create");
}

PhiVerbResult PhiControlDispatcher::VerbTabGroupsUpdate(
    const PhiVerbContext& ctx) {
  return TabGroupsOp(ctx, "update");
}

PhiVerbResult PhiControlDispatcher::VerbTabGroupsAddTabs(
    const PhiVerbContext& ctx) {
  return TabGroupsOp(ctx, "addTabs");
}

PhiVerbResult PhiControlDispatcher::VerbTabGroupsRemoveTabs(
    const PhiVerbContext& ctx) {
  return TabGroupsOp(ctx, "removeTabs");
}

PhiVerbResult PhiControlDispatcher::VerbTabGroupsUngroup(
    const PhiVerbContext& ctx) {
  return TabGroupsOp(ctx, "ungroup");
}

PhiVerbResult PhiControlDispatcher::VerbTabGroupsClose(
    const PhiVerbContext& ctx) {
  return TabGroupsOp(ctx, "close");
}

// --- Split view -----------------------------------------------------------------------

PhiVerbResult PhiControlDispatcher::SplitViewOp(const PhiVerbContext& ctx,
                                                std::string_view op) {
  const BrowserId browser = ctx.scope.browser;
  if (!ctx.scope.valid() || !browser.valid() || !framework_) {
    return PhiVerbResult::Fail(
        ctx.scope.error.empty() ? "space_not_open" : ctx.scope.error);
  }
  if (op == "list") {
    base::ListValue splits;
    if (const phi_framework::WindowSnapshot* window =
            coordinator_->window_model().Get(browser)) {
      for (const phi_framework::SplitSnapshot& split : window->splits) {
        base::DictValue row;
        row.Set("splitId", split.id);
        row.Set("layout",
                split.layout == phi_framework::SplitLayout::kHorizontal
                    ? "horizontal"
                    : "vertical");
        row.Set("ratio", split.ratio);
        row.Set("primaryTabId", split.primary.value);
        row.Set("secondaryTabId", split.secondary.value);
        splits.Append(std::move(row));
      }
    }
    base::DictValue out;
    out.Set("splits", std::move(splits));
    return PhiVerbResult::Ok(std::move(out));
  }
  if (op == "create") {
    const std::string created = framework_->CreateSplitViewForTabs(
        browser.value, ctx.params.Int("primaryTabId", -1),
        ctx.params.Int("secondaryTabId", -1),
        ctx.params.String("layout").c_str());
    if (created.empty()) {
      return PhiVerbResult::Fail("create_failed");
    }
    base::DictValue out;
    out.Set("splitId", created);
    return PhiVerbResult::Ok(std::move(out));
  }
  const std::string split_id = ctx.params.String("splitId");
  if (op == "update") {
    return PhiVerbOk(framework_->UpdateSplitView(
        browser.value, split_id.c_str(), ctx.params.String("layout").c_str(),
        ctx.params.Double("ratio", -1.0)));
  }
  if (op == "swap") {
    return PhiVerbOk(
        framework_->SwapSplitView(browser.value, split_id.c_str()));
  }
  return PhiVerbOk(
      framework_->RemoveSplitById(browser.value, split_id.c_str()));
}

PhiVerbResult PhiControlDispatcher::VerbSplitViewList(
    const PhiVerbContext& ctx) {
  return SplitViewOp(ctx, "list");
}

PhiVerbResult PhiControlDispatcher::VerbSplitViewCreate(
    const PhiVerbContext& ctx) {
  return SplitViewOp(ctx, "create");
}

PhiVerbResult PhiControlDispatcher::VerbSplitViewUpdate(
    const PhiVerbContext& ctx) {
  return SplitViewOp(ctx, "update");
}

PhiVerbResult PhiControlDispatcher::VerbSplitViewSwap(
    const PhiVerbContext& ctx) {
  return SplitViewOp(ctx, "swap");
}

PhiVerbResult PhiControlDispatcher::VerbSplitViewRemove(
    const PhiVerbContext& ctx) {
  return SplitViewOp(ctx, "remove");
}

// --- Downloads (observe and control only — never open) ----------------------------------

PhiVerbResult PhiControlDispatcher::DownloadsOp(const PhiVerbContext& ctx,
                                                std::string_view op) {
  const BrowserId browser = ctx.scope.browser;
  if (!ctx.scope.valid() || !browser.valid() || !framework_) {
    return PhiVerbResult::Fail(
        ctx.scope.error.empty() ? "space_not_open" : ctx.scope.error);
  }
  if (op == "list") {
    std::optional<base::Value> parsed = base::JSONReader::Read(
        framework_->DownloadsJsonForAgent(browser.value), base::JSON_PARSE_RFC);
    base::DictValue out;
    out.Set("downloads", parsed && parsed->is_list()
                             ? std::move(*parsed)
                             : base::Value(base::ListValue()));
    return PhiVerbResult::Ok(std::move(out));
  }
  const std::string guid = ctx.params.String("guid");
  if (op == "get") {
    std::optional<base::Value> parsed = base::JSONReader::Read(
        framework_->DownloadsJsonForAgent(browser.value), base::JSON_PARSE_RFC);
    if (parsed && parsed->is_list()) {
      for (const base::Value& v : parsed->GetList()) {
        const base::DictValue* d = v.GetIfDict();
        const std::string* g = d ? d->FindString("guid") : nullptr;
        if (g && *g == guid) {
          base::DictValue out;
          out.Set("download", d->Clone());
          return PhiVerbResult::Ok(std::move(out));
        }
      }
    }
    return PhiVerbResult::Fail("unknown_download");
  }
  const std::string action(op);
  if (!framework_->DownloadActionByGuid(browser.value, guid.c_str(),
                                        action.c_str())) {
    return PhiVerbResult::Fail("unknown_download");
  }
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbDownloadsList(
    const PhiVerbContext& ctx) {
  return DownloadsOp(ctx, "list");
}

PhiVerbResult PhiControlDispatcher::VerbDownloadsGet(
    const PhiVerbContext& ctx) {
  return DownloadsOp(ctx, "get");
}

PhiVerbResult PhiControlDispatcher::VerbDownloadsPause(
    const PhiVerbContext& ctx) {
  return DownloadsOp(ctx, "pause");
}

PhiVerbResult PhiControlDispatcher::VerbDownloadsResume(
    const PhiVerbContext& ctx) {
  return DownloadsOp(ctx, "resume");
}

PhiVerbResult PhiControlDispatcher::VerbDownloadsCancel(
    const PhiVerbContext& ctx) {
  return DownloadsOp(ctx, "cancel");
}

PhiVerbResult PhiControlDispatcher::VerbDownloadsRemove(
    const PhiVerbContext& ctx) {
  return DownloadsOp(ctx, "remove");
}

// --- App-wide looks and settings ----------------------------------------------------

PhiVerbResult PhiControlDispatcher::VerbLayoutGet(const PhiVerbContext& ctx) {
  base::DictValue out;
  out.Set("mode", PhiLayoutModeName(coordinator_->settings().layout_mode));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbLayoutSet(const PhiVerbContext& ctx) {
  PhiLayoutMode mode = coordinator_->settings().layout_mode;
  bool ok = false;
  const std::string* name = ctx.params.FindString("mode");
  if (name) {
    ok = PhiLayoutModeFromName(*name, &mode);
  }
  if (std::optional<int> width = ctx.params.FindInt("sidebarWidth")) {
    coordinator_->RequestSidebarWidth(*width);
    ok = true;
  }
  // {sidebar: "docked" | "hidden"} docks or hides THIS window's sidebar
  // (the floating panel then stays tucked at the edge, §4.4).
  if (const std::string* sidebar = ctx.params.FindString("sidebar")) {
    // An app-wide verb (no scope): {windowId} names the window, else the
    // first user window takes it.
    const int64_t window_id =
        static_cast<int64_t>(ctx.params.FindDouble("windowId").value_or(0));
    PhiShellWindow* shell = window_id
                                ? coordinator_->registry().Resolve(window_id)
                                : coordinator_->FirstUserShell();
    if (shell && (*sidebar == "docked" || *sidebar == "hidden")) {
      shell->SetSidebarCollapsed(*sidebar == "hidden");
      ok = true;
    }
  }
  if (ok && name) {
    coordinator_->RequestLayoutMode(mode);
  }
  base::DictValue out;
  out.Set("mode", PhiLayoutModeName(coordinator_->settings().layout_mode));
  out.Set("sidebarWidth", coordinator_->settings().sidebar_width);
  return PhiVerbOk(ok, std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbThemeGet(const PhiVerbContext& ctx) {
  base::DictValue out;
  out.Set("themeId", coordinator_->settings().theme_id);
  out.Set("appearance", PhiAppearanceName(coordinator_->Appearance()));
  base::ListValue names;
  for (const PhiTheme& theme : PhiBuiltInThemes()) {
    base::DictValue entry;
    entry.Set("id", theme.id);
    entry.Set("name", theme.name);
    names.Append(std::move(entry));
  }
  out.Set("themes", std::move(names));
  out.Set("dark", coordinator_->Palette().dark);
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbThemeSet(const PhiVerbContext& ctx) {
  bool ok = false;
  if (const std::string* id = ctx.params.FindString("themeId")) {
    if (*id == PhiThemeById(*id).id) {
      coordinator_->SetThemeId(*id);
      ok = true;
    }
  }
  if (const std::string* name = ctx.params.FindString("appearance")) {
    PhiAppearance parsed = PhiAppearance::kSystem;
    if (PhiAppearanceFromName(*name, &parsed)) {
      coordinator_->SetAppearance(parsed);
      ok = true;
    }
  }
  base::DictValue out;
  out.Set("themeId", coordinator_->settings().theme_id);
  out.Set("appearance", PhiAppearanceName(coordinator_->Appearance()));
  out.Set("dark", coordinator_->Palette().dark);
  return PhiVerbOk(ok, std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSettingsGet(
    const PhiVerbContext& ctx) {
  // Everything the settings window shows, in one read.
  base::DictValue out;
  out.Set("section", PhiSettingsWindow::CurrentSection());
  base::ListValue sections;
  // The Mac's panes(), in its order; Developer only in developer mode.
  // (There is no Phi Link pane on Mac — IMChannels exists in its source
  // but is never mounted.)
  // The Windows Settings pages in rail order (docs/windows-native-ui.md
  // §7); Developer only in developer mode.
  for (const char* name : {"account", "general", "appearance", "spaces",
                           "profiles", "navigation", "ai", "shortcuts",
                           "advanced"}) {
    sections.Append(name);
  }
  if (coordinator_->settings().developer_mode) {
    sections.Append("developer");
  }
  sections.Append("about");
  out.Set("sections", std::move(sections));
  out.Set("developerMode", coordinator_->settings().developer_mode);
  out.Set("allowAgentControl", coordinator_->settings().allow_agent_control);
  out.Set("allowAgentUserSpaces",
          coordinator_->settings().allow_agent_user_spaces);
  // Developer ▸ "Profiles agents can use", as the refusals the pane
  // stores. Readable so a caller can see why a create was turned down;
  // settable only from that pane (below).
  base::ListValue blocked_profiles;
  for (const std::string& id :
       coordinator_->settings().disallowed_agent_profile_ids) {
    blocked_profiles.Append(id);
  }
  out.Set("disallowedAgentProfileIds", std::move(blocked_profiles));
  out.Set("themeId", coordinator_->settings().theme_id);
  out.Set("appearance", PhiAppearanceName(coordinator_->Appearance()));
  out.Set("saturation", coordinator_->settings().saturation);
  out.Set("layout", PhiLayoutModeName(coordinator_->settings().layout_mode));
  out.Set("newTabBehavior",
          coordinator_->NewTabBehavior() == PhiNewTabBehavior::kOmnibox
              ? "omnibox"
              : "newTabPage");
  out.Set("alwaysShowFullUrl", coordinator_->settings().always_show_full_url);
  out.Set("showTabPreviews", coordinator_->settings().show_tab_previews);
  out.Set("showOpenTabIndicators",
          coordinator_->settings().show_open_tab_indicators);
  out.Set("dimUnloadedTabIcons",
          coordinator_->settings().dim_unloaded_tab_icons);
  out.Set("restoreLastSession", coordinator_->settings().restore_last_session);
  out.Set("analyticsEnabled", coordinator_->settings().analytics_enabled);
  out.Set("peekViewEnabled", coordinator_->settings().peek_view_enabled);
  out.Set("autoPeekViewEnabled",
          coordinator_->settings().auto_peek_view_enabled);
  out.Set("openKioskOnCtrlAltClick",
          coordinator_->settings().open_kiosk_on_ctrl_alt_click);
  out.Set("pinScope", PhiPinScopeName(coordinator_->tab_store().pin_scope()));
  // With a profileId, the per-profile browsing settings too.
  if (const std::string* profile_id = ctx.params.FindString("profileId")) {
    base::ListValue engines;
    for (const std::string& row : coordinator_->SearchEngines(*profile_id)) {
      engines.Append(row);
    }
    out.Set("searchEngines", std::move(engines));
    out.Set("downloadDirectory", coordinator_->DownloadDirectory(*profile_id));
  }
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSettingsSet(
    const PhiVerbContext& ctx) {
  // The keys that decide what an agent may reach — or what the user has
  // agreed to share — are never the agent's to set: developerMode unlocks
  // the pane that holds the toggles, allowAgentControl admits agents at
  // all, allowAgentUserSpaces widens their reach, testRig unlocks the
  // harness verbs, analyticsEnabled is the telemetry consent. Refused
  // whole — a payload that carries one gets none of its keys.
  if (ctx.caller == ControlCaller::kPipe &&
      (ctx.params.Has("developerMode") ||
       ctx.params.Has("allowAgentControl") ||
       ctx.params.Has("allowAgentUserSpaces") || ctx.params.Has("testRig") ||
       ctx.params.Has("analyticsEnabled") ||
       ctx.params.Has("disallowedAgentProfileIds"))) {
    return PhiVerbResult::Fail(PhiErrorCode::kPrefNotAgentSettable);
  }
  bool ok = false;
  if (std::optional<int> saturation = ctx.params.FindInt("saturation")) {
    // Out-of-range is clamped, not rejected: a slider cannot overshoot.
    coordinator_->SetSaturation(*saturation);
    ok = true;
  }
  if (std::optional<bool> full = ctx.params.FindBool("alwaysShowFullUrl")) {
    coordinator_->SetAlwaysShowFullUrl(*full);
    ok = true;
  }
  if (std::optional<bool> previews = ctx.params.FindBool("showTabPreviews")) {
    coordinator_->SetShowTabPreviews(*previews);
    ok = true;
  }
  if (std::optional<bool> dots = ctx.params.FindBool("showOpenTabIndicators")) {
    coordinator_->SetShowOpenTabIndicators(*dots);
    ok = true;
  }
  if (std::optional<bool> dim = ctx.params.FindBool("dimUnloadedTabIcons")) {
    coordinator_->SetDimUnloadedTabIcons(*dim);
    ok = true;
  }
  if (std::optional<bool> restore =
          ctx.params.FindBool("restoreLastSession")) {
    coordinator_->SetRestoreLastSession(*restore);
    ok = true;
  }
  // Peek's master first: turning it off drags the sub-switch down with it,
  // so a payload carrying both still ends where it asked to.
  if (std::optional<bool> peek = ctx.params.FindBool("peekViewEnabled")) {
    coordinator_->SetPeekViewEnabled(*peek);
    coordinator_->SetAutoPeekViewEnabled(*peek);
    ok = true;
  }
  if (std::optional<bool> auto_peek =
          ctx.params.FindBool("autoPeekViewEnabled")) {
    coordinator_->SetAutoPeekViewEnabled(*auto_peek);
    ok = true;
  }
  if (std::optional<bool> kiosk =
          ctx.params.FindBool("openKioskOnCtrlAltClick")) {
    coordinator_->SetOpenKioskOnCtrlAltClick(*kiosk);
    ok = true;
  }
  if (const std::string* behavior = ctx.params.FindString("newTabBehavior")) {
    if (*behavior == "omnibox" || *behavior == "newTabPage") {
      coordinator_->SetNewTabBehavior(*behavior == "omnibox"
                                          ? PhiNewTabBehavior::kOmnibox
                                          : PhiNewTabBehavior::kNewTabPage);
      ok = true;
    }
  }
  if (std::optional<bool> dev = ctx.params.FindBool("developerMode")) {
    coordinator_->SetDeveloperMode(*dev);
    ok = true;
  }
  if (const std::string* section = ctx.params.FindString("section")) {
    // Developer exists only in developer mode — refuse it here too: the
    // window-side check cannot run before the window ever opens. The
    // refusal leaves `ok` untouched so a combined payload's other keys
    // still report their own success.
    if (*section != "developer" || coordinator_->settings().developer_mode) {
      ok = PhiSettingsWindow::ShowSection(*section) || ok;
    }
  }
  // Per-profile browsing settings, keyed by profileId.
  if (const std::string* profile_id = ctx.params.FindString("profileId")) {
    if (const std::string* keyword = ctx.params.FindString("searchEngine")) {
      ok = coordinator_->SetSearchEngine(*profile_id, *keyword) || ok;
      PhiSettingsWindow::Refresh();
    }
    if (const std::string* dir = ctx.params.FindString("downloadDirectory")) {
      ok = coordinator_->SetDownloadDirectory(*profile_id, *dir) || ok;
      PhiSettingsWindow::Refresh();
    }
  }
  base::DictValue out;
  out.Set("developerMode", coordinator_->settings().developer_mode);
  out.Set("saturation", coordinator_->settings().saturation);
  out.Set("alwaysShowFullUrl", coordinator_->settings().always_show_full_url);
  out.Set("showTabPreviews", coordinator_->settings().show_tab_previews);
  out.Set("showOpenTabIndicators",
          coordinator_->settings().show_open_tab_indicators);
  out.Set("dimUnloadedTabIcons",
          coordinator_->settings().dim_unloaded_tab_icons);
  out.Set("restoreLastSession", coordinator_->settings().restore_last_session);
  out.Set("peekViewEnabled", coordinator_->settings().peek_view_enabled);
  out.Set("autoPeekViewEnabled",
          coordinator_->settings().auto_peek_view_enabled);
  out.Set("openKioskOnCtrlAltClick",
          coordinator_->settings().open_kiosk_on_ctrl_alt_click);
  out.Set("newTabBehavior",
          coordinator_->NewTabBehavior() == PhiNewTabBehavior::kOmnibox
              ? "omnibox"
              : "newTabPage");
  out.Set("section", PhiSettingsWindow::CurrentSection());
  return PhiVerbOk(ok, std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSettingsOpen(
    const PhiVerbContext& ctx) {
  coordinator_->RequestSettings();
  base::DictValue out;
  out.Set("hwnd", static_cast<double>(reinterpret_cast<intptr_t>(
                      PhiSettingsWindow::Handle())));
  return PhiVerbOk(PhiSettingsWindow::Handle() != nullptr, std::move(out));
}

// --- Feedback ----------------------------------------------------------------------

PhiVerbResult PhiControlDispatcher::VerbFeedbackStatus(
    const PhiVerbContext& ctx) {
  // The outbox (every job of the current account with its state) and the
  // form, when open — the harness's view of the feedback pipeline.
  base::DictValue out;
  if (std::optional<base::Value> status = base::JSONReader::Read(
          PhiFeedbackOutbox::Get().StatusJson(), base::JSON_PARSE_RFC)) {
    if (base::DictValue* dict = status->GetIfDict()) {
      out.Merge(std::move(*dict));
    }
  }
  if (std::optional<base::Value> form = base::JSONReader::Read(
          PhiFeedbackWindow::StateJson(), base::JSON_PARSE_RFC)) {
    out.Set("form", std::move(*form));
  }
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbFeedbackOpen(
    const PhiVerbContext& ctx) {
  // Opens the form for {windowId} (a slot), else the focused window —
  // exactly what Help ▸ Report an Issue does. A {windowId} that names no
  // window is refused, never a cue to open the form somewhere else.
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
  if (controller) {
    controller->OpenFeedback();
  }
  const HWND form = PhiFeedbackWindow::Handle();
  base::DictValue out;
  out.Set("hwnd", static_cast<double>(reinterpret_cast<uintptr_t>(form)));
  return PhiVerbOk(form != nullptr, std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbFeedbackForm(
    const PhiVerbContext& ctx) {
  // Drives the open form: {description?, url?, attachFiles?:[path],
  // send?:true} — the Send path is the button's own.
  std::vector<std::wstring> files;
  for (const std::string& path : ctx.params.StringList("attachFiles")) {
    files.push_back(base::UTF8ToWide(path));
  }
  const std::string* description = ctx.params.FindString("description");
  const std::string* url = ctx.params.FindString("url");
  const std::string error = PhiFeedbackWindow::Drive(
      description ? *description : std::string(), description != nullptr,
      url ? *url : std::string(), url != nullptr, files,
      ctx.params.Bool("send", false));
  if (!error.empty()) {
    return PhiVerbResult::Fail(error);
  }
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbFeedbackSetAccount(
    const PhiVerbContext& ctx) {
  // Harness only: {userId, email} stands in for a signed-in account on a
  // box without one (empty clears). A real session always wins.
  coordinator_->SetTestFeedbackAccount(ctx.params.String("userId"),
                                       ctx.params.String("email"));
  base::DictValue out;
  out.Set("signedIn", coordinator_->auth().signed_in());
  return PhiVerbResult::Ok(std::move(out));
}

}  // namespace phi_client

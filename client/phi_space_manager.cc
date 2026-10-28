// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_space_manager.h"

#include "phibrowser-win/client/phi_account_storage.h"
#include "phibrowser-win/client/phi_agent_spaces.h"

#include <cmath>
#include <set>
#include <utility>

#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_preferences.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_shell_window.h"

namespace phi_client {

namespace {

// The Mac's ThemePalette coding (version, hue, saturation, brightness,
// colorCount, harmony), decoded with its rules: a record before version 2
// takes brightness 1 (the removed slider's right endpoint); no harmony
// means the one its colour count implied.
std::optional<PhiThemePalette> PaletteFromDict(const base::DictValue& record) {
  const std::optional<double> hue = record.FindDouble("hue");
  const std::optional<double> saturation = record.FindDouble("saturation");
  if (!hue || !saturation) {
    return std::nullopt;
  }
  const int version = record.FindInt("version").value_or(1);
  const std::optional<double> brightness = record.FindDouble("brightness");
  const int color_count = record.FindInt("colorCount").value_or(1);
  PhiThemePalette::Harmony harmony =
      PhiThemePalette::HarmonyForCount(color_count);
  if (const std::string* name = record.FindString("harmony")) {
    PhiThemePalette::Harmony named;
    if (PhiThemePalette::HarmonyFromName(*name, &named)) {
      harmony = named;
    }
  }
  return PhiThemePalette::Make(
      *hue, *saturation, version < 2 ? 1.0 : brightness.value_or(1.0),
      harmony);
}

base::DictValue PaletteToDict(const PhiThemePalette& palette) {
  base::DictValue record;
  record.Set("version", PhiThemePalette::kVersion);
  record.Set("hue", palette.hue);
  record.Set("saturation", palette.saturation);
  record.Set("brightness", palette.brightness);
  record.Set("colorCount", palette.color_count());
  record.Set("harmony", PhiThemePalette::HarmonyName(palette.harmony));
  return record;
}

}  // namespace

PhiSpaceManager::PhiSpaceManager(PhiWindowRegistry& registry,
                                 const PhiWindowModel& window_model,
                                 const PhiPreferences& preferences,
                                 phi_framework::PhiFramework& framework,
                                 Host& host,
                                 store::LocalStore& store,
                                 PhiAccountDefaults& defaults,
                                 DirtyCallback dirty)
    : registry_(registry),
      window_model_(window_model),
      preferences_(preferences),
      framework_(framework),
      host_(host),
      store_(store),
      defaults_(defaults),
      dirty_(std::move(dirty)) {}

PhiSpaceManager::~PhiSpaceManager() = default;

void PhiSpaceManager::AddObserver(Observer* observer) {
  observers_.push_back(observer);
}

void PhiSpaceManager::RemoveObserver(Observer* observer) {
  std::erase(observers_, observer);
}

void PhiSpaceManager::Load() {
  model_ = model::PhiSpacesModel();
  if (!store_.is_open()) {
    return;  // The built-in default Space stands in; nothing is saved.
  }
  if (store_.Spaces().empty()) {
    // The Mac's first-launch bootstrap: `default-space`, on the Default
    // profile, only in an EMPTY store.
    store_.EnsureDefaultSpace(store::kDefaultProfileId);
  }
  const std::map<std::string, std::string> themes =
      defaults_.GetStringMap(kDefaultsSpaceThemeIds);
  const std::map<std::string, int> saturations =
      defaults_.GetIntMap(kDefaultsWinSpaceSaturations);
  const base::DictValue* palettes =
      defaults_.GetDict(kDefaultsSpaceThemePalettes);
  std::vector<model::Space> spaces;
  for (const store::Space& row : store_.Spaces()) {
    model::Space space;
    space.id = row.space_id;
    space.name = row.name;
    space.profile_id = row.profile_id;
    space.icon = PhiIconIsValid(row.icon_name) ? row.icon_name
                                               : store::kDefaultSpaceIconName;
    space.color_hex = row.color_hex;
    if (const auto t = themes.find(row.space_id); t != themes.end()) {
      space.theme_id = t->second;
    }
    if (const auto v = saturations.find(row.space_id);
        v != saturations.end()) {
      space.saturation = v->second < 0 ? -1 : std::min(100, v->second);
    }
    if (const base::DictValue* record =
            palettes ? palettes->FindDict(row.space_id) : nullptr) {
      space.palette = PaletteFromDict(*record);
    }
    spaces.push_back(std::move(space));
  }
  if (!spaces.empty()) {
    model_.spaces = std::move(spaces);
  }
  hidden_.clear();
  ApplyHiddenSet();
  model_.default_space_id = model::ResolveDefaultSpaceId(
      model_.spaces, defaults_.GetString(kDefaultsDefaultSpaceId));
  ReloadUrlRulesFromStore();
}

void PhiSpaceManager::ReloadUrlRulesFromStore() {
  model_.url_rules.clear();
  for (const store::UrlRule& row : store_.UrlRules()) {
    model::UrlRule rule;
    rule.id = row.id;
    rule.space_id = row.space_id;
    rule.host = row.host;
    rule.path_prefix = row.path_prefix.value_or(std::string());
    rule.ask = row.ask;
    model_.url_rules.push_back(std::move(rule));
  }
}

void PhiSpaceManager::Save() {
  if (!store_.is_open()) {
    return;
  }
  // Spaces: create, re-bind, update — only what differs, so an unchanged
  // Space's updatedDate stays put.
  std::map<std::string, store::Space> stored;
  for (const store::Space& row : store_.Spaces()) {
    stored[row.space_id] = row;
  }
  std::set<std::string> live;
  for (const model::Space& space : hidden_) {
    live.insert(space.id);
  }
  for (const model::Space& space : model_.spaces) {
    live.insert(space.id);
    // The Mac's colorHex is never empty; a Space nobody coloured carries the
    // Mac's default.
    const std::string color = space.color_hex.empty()
                                  ? std::string(store::kDefaultSpaceColorHex)
                                  : space.color_hex;
    const auto it = stored.find(space.id);
    if (it == stored.end()) {
      store_.CreateSpace(space.id, space.profile_id, space.name, color,
                         space.icon);
      continue;
    }
    const store::Space& row = it->second;
    if (row.profile_id != space.profile_id) {
      store_.ChangeSpaceProfile(space.id, space.profile_id);
    }
    // An icon this build cannot show (a newer peer's) reads as the
    // stand-in; the stand-in is never written back over it.
    const bool icon_changed =
        row.icon_name != space.icon &&
        (PhiIconIsValid(row.icon_name) ||
         space.icon != store::kDefaultSpaceIconName);
    if (row.name != space.name || row.color_hex != color || icon_changed) {
      store_.UpdateSpace(
          space.id,
          row.name != space.name ? std::optional(space.name) : std::nullopt,
          row.color_hex != color ? std::optional(color) : std::nullopt,
          icon_changed ? std::optional(space.icon) : std::nullopt);
    }
  }
  for (const auto& [id, row] : stored) {
    if (!live.count(id)) {
      // Normally already gone (DeleteSpace's ForgetSpaceData cascades at
      // once); this catches a Space removed any other way.
      store_.DeleteSpaceCascade(id, /*user_intent=*/true);
    }
  }
  std::vector<std::string> order;
  for (const model::Space& space : model_.spaces) {
    order.push_back(space.id);
  }
  for (const model::Space& space : hidden_) {
    order.push_back(space.id);
  }
  std::vector<std::string> stored_order;
  for (const store::Space& row : store_.Spaces()) {
    stored_order.push_back(row.space_id);
  }
  if (order != stored_order) {
    store_.ReorderSpaces(order);
  }

  // URL rules: one edit set. The model's order is each target bucket's
  // order; units that did not change are omitted, so they stamp nothing.
  std::map<std::string, store::UrlRule> stored_rules;
  for (const store::UrlRule& row : store_.UrlRules()) {
    stored_rules[row.id] = row;
  }
  std::vector<store::UrlRuleDraft> drafts;
  std::set<std::string> kept;
  std::map<std::string, int> next_in_bucket;
  for (model::UrlRule& rule : model_.url_rules) {
    if (rule.id.empty()) {
      rule.id = store::NewUppercaseUuid();
    }
    kept.insert(rule.id);
    const int sort_order = next_in_bucket[rule.space_id]++;
    const std::optional<std::string> path =
        rule.path_prefix.empty() ? std::nullopt
                                 : std::optional<std::string>(rule.path_prefix);
    store::UrlRuleDraft draft;
    draft.id = rule.id;
    const auto it = stored_rules.find(rule.id);
    if (it == stored_rules.end()) {
      draft.content = store::UrlRuleDraft::Content{rule.host, path, rule.ask};
      draft.space_id = rule.space_id;
      draft.sort_order = sort_order;
      drafts.push_back(std::move(draft));
      continue;
    }
    const store::UrlRule& row = it->second;
    bool changed = false;
    if (row.host != store::NormalizedRuleHost(rule.host) ||
        row.path_prefix != store::NormalizedRulePathPrefix(path) ||
        row.ask != rule.ask) {
      draft.content = store::UrlRuleDraft::Content{rule.host, path, rule.ask};
      changed = true;
    }
    if (row.space_id != rule.space_id) {
      draft.space_id = rule.space_id;
      changed = true;
    }
    if (row.sort_order != sort_order) {
      draft.sort_order = sort_order;
      changed = true;
    }
    if (changed) {
      drafts.push_back(std::move(draft));
    }
  }
  std::set<std::string> deleted;
  for (const auto& [id, row] : stored_rules) {
    if (!kept.count(id)) {
      deleted.insert(id);
    }
  }
  if (!drafts.empty() || !deleted.empty()) {
    const store::WriteError error = store_.ApplyUrlRuleEdits(drafts, deleted);
    if (error != store::WriteError::kNone) {
      LOG(ERROR) << "[PhiClient] URL rule save failed ("
                 << static_cast<int>(error) << ")";
    }
  }

  // The account defaults: the default role and the theme overrides.
  defaults_.SetString(kDefaultsDefaultSpaceId, model_.default_space_id);
  std::map<std::string, std::string> themes;
  std::map<std::string, int> saturations;
  base::DictValue palettes;
  std::vector<model::Space> all = model_.spaces;
  all.insert(all.end(), hidden_.begin(), hidden_.end());
  for (const model::Space& space : all) {
    if (!space.theme_id.empty()) {
      themes[space.id] = space.theme_id;
    }
    if (space.saturation >= 0) {
      saturations[space.id] = space.saturation;
    }
    if (space.palette) {
      palettes.Set(space.id, PaletteToDict(*space.palette));
    }
  }
  defaults_.SetStringMap(kDefaultsSpaceThemeIds, themes);
  defaults_.SetIntMap(kDefaultsWinSpaceSaturations, saturations);
  defaults_.SetDict(kDefaultsSpaceThemePalettes, std::move(palettes));
}

void PhiSpaceManager::RetreatFromMissingSpaces() {
  const std::string retreat = DefaultSpaceId();
  // Over a copy: a retreat can close a slot (its last browser going).
  const std::vector<PhiShellWindow*> controllers = registry_.Controllers();
  for (PhiShellWindow* controller : controllers) {
    if (controller->is_agent()) {
      continue;
    }
    std::vector<std::string> missing;
    bool has_valid = false;
    for (const std::string& id : controller->space_order()) {
      if (Has(id)) {
        has_valid = true;
      } else if (!PhiIsAgentSpaceId(id)) {
        missing.push_back(id);
      }
    }
    if (missing.empty()) {
      continue;
    }
    if (!has_valid) {
      // Nothing valid left here: the vanished Spaces' browsers stay on
      // screen, retired, until the default Space's attaches — closing them
      // first closed the window (the app, if it was the last) before the
      // replacement came.
      host_.SurfaceSpaceInSlot(controller->slot(), retreat, nullptr);
      for (const std::string& id : missing) {
        controller->RetireAndUnmapSpaceBrowser(id);
      }
      continue;
    }
    for (const std::string& id : missing) {
      controller->CloseSpaceBrowser(id);
    }
  }
  RebuildSpacePalettes();
  Notify(SpacesChange::kList);
}

// --- Reads --------------------------------------------------------------------

const model::Space* PhiSpaceManager::Find(const std::string& id) const {
  if (const model::Space* stored = FindSpaceIn(model_.spaces, id)) {
    return stored;
  }
  for (const IncognitoEntry& entry : incognito_) {
    if (entry.space.id == id) {
      return &entry.space;
    }
  }
  return nullptr;
}

std::vector<PhiSpaceView> PhiSpaceManager::Spaces() const {
  std::vector<PhiSpaceView> out;
  out.reserve(model_.spaces.size() + incognito_.size());
  const auto add = [&](const model::Space& space) {
    PhiSpaceView view;
    view.id = space.id;
    view.name = space.name;
    view.profile_id = space.profile_id;
    view.icon = space.icon;
    view.icon_text = PhiIconDisplayText(space.icon);
    view.incognito = model::IsIncognitoSpaceId(space.id);
    out.push_back(std::move(view));
  };
  for (const model::Space& space : model_.spaces) {
    add(space);
  }
  // After the user Spaces, before the agent pips (the Mac's order).
  for (const IncognitoEntry& entry : incognito_) {
    add(entry.space);
  }
  return out;
}

std::vector<PhiSpaceSummary> PhiSpaceManager::Summaries() const {
  std::vector<PhiSpaceSummary> out;
  out.reserve(model_.spaces.size());
  for (const model::Space& space : model_.spaces) {
    out.push_back({space.id, space.name, PhiIconDisplayText(space.icon),
                   space.profile_id, space.theme_id,
                   SpaceSaturation(space.id)});
    out.back().icon = space.icon;
  }
  return out;
}

std::string PhiSpaceManager::DefaultSpaceId() const {
  return DefaultSpaceIdOf(model_.spaces, model_.default_space_id);
}

bool PhiSpaceManager::CanDeleteSpace(const std::string& space_id) const {
  return CanDeleteSpaceIn(model_.spaces, space_id);
}

std::string PhiSpaceManager::KioskTargetSpaceId() const {
  if (PhiShellWindow* main = registry_.FirstUserController()) {
    const std::string& active = main->active_space();
    // A Kiosk page never lands in an Incognito Space by default: the regular
    // profile's page would re-open off the record.
    if (!active.empty() && !model::IsIncognitoSpaceId(active)) {
      return active;
    }
  }
  // DefaultSpaceId itself falls back to the first Space, so this always
  // names one as long as any exists.
  return DefaultSpaceId();
}

std::string PhiSpaceManager::SpaceThemeId(const std::string& space_id) const {
  const model::Space* space = Find(space_id);
  return space ? space->theme_id : std::string();
}

int PhiSpaceManager::SpaceSaturation(const std::string& space_id) const {
  const model::Space* space = Find(space_id);
  if (space && space->palette) {
    // The Mac's effectiveOverlaySaturation: the palette's radius is what
    // the Space's slider shows (and edits — SetSpaceSaturation).
    return static_cast<int>(std::lround(space->palette->saturation * 100));
  }
  return space ? space->saturation : -1;  // -1 follows the app-wide slider.
}

std::optional<PhiThemePalette> PhiSpaceManager::SpacePalette(
    const std::string& space_id) const {
  const model::Space* space = Find(space_id);
  return space ? space->palette : std::nullopt;
}

// --- Writes -------------------------------------------------------------------

bool PhiSpaceManager::RenameSpace(const std::string& space_id,
                                  const std::string& name) {
  if (name.empty()) {
    return false;
  }
  for (model::Space& space : model_.spaces) {
    if (space.id != space_id) {
      continue;
    }
    space.name = name;
    MarkDirty();
    Notify(SpacesChange::kList);
    return true;
  }
  return false;
}

bool PhiSpaceManager::DeleteSpace(const std::string& space_id) {
  // Only the LAST Space is undeletable (the Mac's last-Space rule); the
  // default Space itself may go, handing its role on below.
  if (!CanDeleteSpace(space_id)) {
    return false;
  }
  auto it = std::find_if(
      model_.spaces.begin(), model_.spaces.end(),
      [&](const model::Space& s) { return s.id == space_id; });
  if (it == model_.spaces.end()) {
    return false;
  }
  RetreatFromSpace(space_id);
  // The Space's bookmarks and space-scoped pins go with it (the Mac's
  // delete-confirmation promise) — and its URL rules, which would otherwise
  // route into a Space that no longer exists, and its ghost record (a
  // parked window is dropped on the session side too).
  host_.ForgetSpaceData(space_id);
  model_.spaces.erase(it);
  // The list, the default role and the rules are one file: one save.
  MarkDirty();
  RebuildSpacePalettes();
  Notify(SpacesChange::kList);
  return true;
}

void PhiSpaceManager::ReorderSpace(const std::string& space_id,
                                   int new_index) {
  auto it = std::find_if(
      model_.spaces.begin(), model_.spaces.end(),
      [&](const model::Space& s) { return s.id == space_id; });
  if (it == model_.spaces.end()) {
    return;
  }
  model::Space moved = *it;
  model_.spaces.erase(it);
  const size_t clamped = ClampSpaceInsertIndex(model_.spaces.size(), new_index);
  model_.spaces.insert(model_.spaces.begin() + static_cast<ptrdiff_t>(clamped),
                       std::move(moved));
  MarkDirty();
  Notify(SpacesChange::kOrder);
}

void PhiSpaceManager::SetSpaceIcon(const std::string& space_id,
                                   const std::string& icon) {
  if (!PhiIconIsValid(icon)) {
    return;
  }
  // An Incognito Space's icon is the user's for its lifetime only.
  for (IncognitoEntry& entry : incognito_) {
    if (entry.space.id == space_id) {
      entry.space.icon = icon;
      Notify(SpacesChange::kList);
      return;
    }
  }
  for (model::Space& space : model_.spaces) {
    if (space.id != space_id) {
      continue;
    }
    space.icon = icon;
    MarkDirty();
    Notify(SpacesChange::kList);
    return;
  }
}

void PhiSpaceManager::SetSpaceTheme(const std::string& space_id,
                                    const std::string& theme_id) {
  for (model::Space& space : model_.spaces) {
    if (space.id != space_id) {
      continue;
    }
    // An empty id clears the override, putting the Space back on the app-wide
    // theme — the Mac's "no entry in spaceThemeIds" state. A preset choice
    // ends the linked palette (the Mac's setTheme removes the record).
    space.theme_id =
        theme_id.empty() ? std::string() : PhiThemeById(theme_id).id;
    space.palette.reset();
    MarkDirty();
    RebuildSpacePalettes();
    Notify(SpacesChange::kTheme);
    return;
  }
}

void PhiSpaceManager::SetSpaceSaturation(const std::string& space_id,
                                         int value) {
  for (model::Space& space : model_.spaces) {
    if (space.id != space_id) {
      continue;
    }
    if (space.palette) {
      // The Mac's setOverlaySaturation under a palette: the slider moves
      // the palette's radius; the legacy per-Space value stays untouched.
      space.palette = space.palette->SettingSaturation(
          std::max(0, std::min(100, value)) / 100.0);
    } else {
      // -1 follows the app-wide slider; anything else clamps to its range.
      space.saturation = value < 0 ? -1 : (value > 100 ? 100 : value);
    }
    MarkDirty();
    RebuildSpacePalettes();
    Notify(SpacesChange::kTheme);
    return;
  }
}

void PhiSpaceManager::PinSpaceTheme(const std::string& space_id,
                                    const std::string& theme_id,
                                    int saturation) {
  for (model::Space& space : model_.spaces) {
    if (space.id == space_id) {
      space.theme_id = PhiThemeById(theme_id).id;
      space.saturation = saturation < 0 ? -1 : std::min(100, saturation);
      space.palette.reset();
      break;
    }
  }
  MarkDirty();
  RebuildSpacePalettes();
  Notify(SpacesChange::kTheme);
}

void PhiSpaceManager::SetSpacePalette(const std::string& space_id,
                                      const PhiThemePalette& palette) {
  for (model::Space& space : model_.spaces) {
    if (space.id != space_id) {
      continue;
    }
    if (space.palette != palette) {
      space.palette = palette;
      MarkDirty();
    }
    // Even when unchanged: a preview may have left the windows ahead of
    // the model, so settle every window on the record.
    RebuildSpacePalettes();
    Notify(SpacesChange::kTheme);
    return;
  }
}

void PhiSpaceManager::PreviewSpacePalette(const std::string& space_id,
                                          const PhiThemePalette& palette) {
  const model::Space* space = FindSpaceIn(model_.spaces, space_id);
  if (!space) {
    return;
  }
  // The Mac's previewThemePalette: the Space's windows take the resolved
  // theme; account preferences and the app-wide notification path are
  // left alone until the editor commits. Only this Space's entry moves,
  // and only the shells repaint — no observer, no sync note.
  const std::string& base =
      space->theme_id.empty() ? preferences_.settings().theme_id
                              : space->theme_id;
  palette_by_space_[space_id] = std::make_unique<PhiPalette>(
      PhiResolvePalette(PhiThemeById(base), preferences_.Appearance(),
                        palette));
  for (PhiShellWindow* controller : registry_.Controllers()) {
    controller->ApplyPalette();
  }
}

bool PhiSpaceManager::SetSpaceProfile(const std::string& space_id,
                                      const std::string& profile_id) {
  if (space_id.empty() || profile_id.empty()) {
    return false;
  }
  auto it = std::find_if(
      model_.spaces.begin(), model_.spaces.end(),
      [&](const model::Space& s) { return s.id == space_id; });
  if (it == model_.spaces.end() || it->profile_id == profile_id) {
    return false;
  }
  // The default Space keeps its binding: it is what a fresh shell's FIRST
  // browser lands on, and that browser exists before any Space is chosen —
  // the Mac refuses the same Space for the same reason.
  const std::string retreat = DefaultSpaceId();
  if (space_id == retreat) {
    return false;
  }
  // A window's profile is baked in at spawn, so the Space's browser cannot
  // be re-pointed: it is closed and respawned in the new profile with its
  // open tabs reopened there (the Mac's changeProfile promise).
  std::vector<std::string> urls;
  SlotId respawn_slot;
  for (PhiShellWindow* controller : registry_.Controllers()) {
    if (controller->is_agent() || !controller->HasSpace(space_id)) {
      continue;
    }
    if (!respawn_slot.valid()) {
      respawn_slot = controller->slot();
      if (const phi_framework::WindowSnapshot* window =
              window_model_.Get(controller->BrowserIdForSpace(space_id))) {
        for (const phi_framework::TabSnapshot& tab : window->tabs) {
          if (!tab.url.empty()) {
            urls.push_back(tab.url);
          }
        }
      }
    }
    // A slot whose ONLY browser is this Space's would be torn down by the
    // close below (a window closes with its last browser — the app with
    // its last window) before any replacement exists. Its browser is
    // RETIRED instead: it stays on screen, no longer holding the Space,
    // until the replacement attaches — the respawn in the slot that gets
    // one, the default Space in any other.
    bool has_other = false;
    for (const model::Space& space : model_.spaces) {
      if (space.id != space_id && controller->HasSpace(space.id)) {
        has_other = true;
        break;
      }
    }
    if (!has_other) {
      if (controller->slot() != respawn_slot) {
        host_.SurfaceSpaceInSlot(controller->slot(), retreat, nullptr);
      }
      controller->RetireAndUnmapSpaceBrowser(space_id);
    }
  }
  for (PhiShellWindow* controller : registry_.Controllers()) {
    controller->CloseSpaceBrowser(space_id);  // No-op where retired above.
  }
  it->profile_id = profile_id;
  MarkDirty();
  // The rows the Space owns move with it — the Mac's "bookmarks stay with
  // the Space" — and any parked window from the previous session belongs to
  // the OLD profile; materializing it now would reopen the Space where it
  // no longer lives.
  host_.RehomeSpaceData(space_id, profile_id);
  Notify(SpacesChange::kProfile);  // Repaint, Settings, the wire's list.
  if (respawn_slot.valid()) {
    // The Space has no browser any more, so this is a request; the tabs
    // ride on it and reopen once the replacement attaches.
    host_.SurfaceSpaceInSlot(respawn_slot, space_id, &urls);
  }
  LOG(INFO) << "[PhiClient] Space " << space_id << " re-bound to profile "
            << profile_id;
  return true;
}

// --- Incognito Spaces ---------------------------------------------------------

namespace {

// "Incognito" while it is the only one, else "Incognito N" (the Mac's
// incognitoSpaceDisplayName).
std::string IncognitoSpaceName(int ordinal, size_t count) {
  if (count <= 1) {
    return base::WideToUTF8(PhiStr("spaces.builtIn.incognito.singleSpaceName"));
  }
  return base::WideToUTF8(PhiStrF("spaces.builtIn.incognito.numberedName",
                                  {base::NumberToWString(ordinal)}));
}

}  // namespace

std::string PhiSpaceManager::CreateIncognitoSpace() {
  std::vector<int> used;
  for (const IncognitoEntry& entry : incognito_) {
    used.push_back(entry.ordinal);
  }
  IncognitoEntry entry;
  entry.ordinal = NextIncognitoOrdinal(used);
  entry.space.id = std::string(model::kIncognitoSpaceIdPrefix) +
                   base::Uuid::GenerateRandomV4().AsLowercaseString();
  // The OTR session's parent; the framework ignores it for an incognito
  // request (the parent is always the initial profile).
  entry.space.profile_id = "Default";
  entry.space.icon = model::kIncognitoSpaceIcon;
  entry.space.color_hex = model::kIncognitoSpaceColorHex;
  const std::string id = entry.space.id;
  incognito_.push_back(std::move(entry));
  for (IncognitoEntry& each : incognito_) {
    each.space.name = IncognitoSpaceName(each.ordinal, incognito_.size());
  }
  LOG(INFO) << "[PhiClient] Incognito Space " << id << " created";
  Notify(SpacesChange::kList);
  return id;
}

bool PhiSpaceManager::RemoveIncognitoSpace(const std::string& space_id) {
  const auto it = std::find_if(
      incognito_.begin(), incognito_.end(),
      [&](const IncognitoEntry& entry) { return entry.space.id == space_id; });
  if (it == incognito_.end()) {
    return false;
  }
  incognito_.erase(it);
  for (IncognitoEntry& each : incognito_) {
    each.space.name = IncognitoSpaceName(each.ordinal, incognito_.size());
  }
  LOG(INFO) << "[PhiClient] Incognito Space " << space_id << " closed";
  Notify(SpacesChange::kList);
  return true;
}

std::vector<std::string> PhiSpaceManager::IncognitoSpaceIds() const {
  std::vector<std::string> ids;
  for (const IncognitoEntry& entry : incognito_) {
    ids.push_back(entry.space.id);
  }
  return ids;
}

// --- Palettes -----------------------------------------------------------------

void PhiSpaceManager::RebuildSpacePalettes() {
  const model::PhiSettings& settings = preferences_.settings();
  const PhiAppearance appearance = preferences_.Appearance();
  palette_by_space_.clear();
  // The agent ground (windows-native-ui.md §5.10): a neutral #E8EAEE
  // (dark #26282C) chrome tint whatever the user's theme, so a glance
  // tells a machine's window from a person's. The accent stays the
  // app's (the brand accent is impl log D26).
  {
    PhiPalette graphite = PhiResolvePalette(PhiThemeById("pure"), appearance,
                                            /*saturation=*/0);
    const COLORREF ground =
        graphite.dark ? RGB(0x26, 0x28, 0x2C) : RGB(0xE8, 0xEA, 0xEE);
    graphite.ground_top = ground;
    graphite.ground_bottom = ground;
    graphite.ground_stops.fill(ground);
    graphite.window_bg = ground;
    agent_palette_ = std::make_unique<PhiPalette>(graphite);
  }
  // Incognito Spaces: the incognito window's fixed-dark chrome, whatever
  // the theme or appearance (the Mac's Theme.incognito, forced dark).
  incognito_palette_ = std::make_unique<PhiPalette>(
      PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kDark));
  for (const model::Space& space : model_.spaces) {
    COLORREF accent = 0;
    // The Mac's default colorHex is what every Space nobody coloured
    // carries (the store never holds an empty one), so it means "no
    // colour choice", not a tint.
    const bool has_hex = space.color_hex != store::kDefaultSpaceColorHex &&
                         PhiParseColorHex(space.color_hex, &accent);
    if (space.palette) {
      // The Mac's resolvedTheme: a linked palette beats the pin's colours,
      // the saturation and the wire hex; the pin (else the app-wide theme)
      // is the base whose window background shows through.
      const std::string& base =
          space.theme_id.empty() ? settings.theme_id : space.theme_id;
      palette_by_space_[space.id] = std::make_unique<PhiPalette>(
          PhiResolvePalette(PhiThemeById(base), appearance, *space.palette));
      continue;
    }
    if (space.theme_id.empty() && space.saturation < 0 && !has_hex) {
      continue;  // Follows the app-wide theme.
    }
    const int saturation =
        space.saturation < 0 ? settings.saturation : space.saturation;
    if (space.theme_id.empty() && has_hex) {
      // No pinned theme but a wire colorHex: the hex drives the Space's tint
      // (the Mac CreateSpacePanel's "colorHex drives the sidebar tint"
      // contract — Spaces created by the skill carry only the hex).
      PhiTheme custom = PhiThemeById(std::string());  // Pure's grounds.
      custom.light.overlay = accent;
      custom.light.accent = accent;
      custom.dark.overlay = accent;
      custom.dark.accent = accent;
      palette_by_space_[space.id] = std::make_unique<PhiPalette>(
          PhiResolvePalette(custom, appearance, saturation));
      continue;
    }
    const std::string& id =
        space.theme_id.empty() ? settings.theme_id : space.theme_id;
    palette_by_space_[space.id] = std::make_unique<PhiPalette>(
        PhiResolvePalette(PhiThemeById(id), appearance, saturation));
  }
}

const PhiPalette& PhiSpaceManager::PaletteForSpace(
    const std::string& space_id) const {
  auto found = palette_by_space_.find(space_id);
  if (found != palette_by_space_.end()) {
    return *found->second;
  }
  if (agent_palette_ && PhiIsAgentSpaceId(space_id)) {
    return *agent_palette_;
  }
  if (incognito_palette_ && model::IsIncognitoSpaceId(space_id)) {
    return *incognito_palette_;
  }
  return preferences_.palette();
}

const PhiPalette& PhiSpaceManager::PaletteForSlot(int64_t slot) const {
  if (PhiShellWindow* window = registry_.Resolve(slot)) {
    return PaletteForSpace(window->active_space());
  }
  return preferences_.palette();
}

// --- The framework's copy -----------------------------------------------------

void PhiSpaceManager::PushSpaceMenuNow() {
  base::ListValue list;
  // "Open Link in Space" lists the live Incognito Spaces too (the Mac's
  // payload); the menu is not offered from an off-the-record page.
  std::vector<const model::Space*> listed;
  for (const model::Space& space : model_.spaces) {
    listed.push_back(&space);
  }
  for (const IncognitoEntry& entry : incognito_) {
    listed.push_back(&entry.space);
  }
  for (const model::Space* listed_space : listed) {
    const model::Space& space = *listed_space;
    base::DictValue d;
    d.Set("id", space.id);
    d.Set("name", space.name);
    // The session id of the Space's open browser (any slot), so the menu
    // can exclude the Space the user is already looking at.
    // User shells only: an incognito window or a hidden agent shell
    // nominally holds the default Space and is never its window.
    BrowserId browser_id;
    for (PhiShellWindow* controller : registry_.UserControllers()) {
      browser_id = controller->BrowserIdForSpace(space.id);
      if (browser_id.valid()) {
        break;
      }
    }
    d.Set("windowId", static_cast<double>(browser_id.value));
    list.Append(std::move(d));
  }
  std::string json;
  base::JSONWriter::Write(list, &json);
  framework_.SetSpaceMenu(json.c_str());
}

void PhiSpaceManager::MarkDirty() {
  if (dirty_) {
    dirty_();
  }
}

void PhiSpaceManager::Notify(SpacesChange what) {
  // Over a copy: an observer may unsubscribe from inside the callback.
  const std::vector<Observer*> observers = observers_;
  for (Observer* observer : observers) {
    observer->OnSpacesChanged(what);
  }
}

void PhiSpaceManager::RetreatFromSpace(const std::string& space_id) {
  if (space_id == model_.default_space_id) {
    // The default role passes to the first REMAINING Space (strip order),
    // before the windows retreat so anything landing on the default lands
    // on the successor; the Spaces save below persists the hand-off.
    const std::string successor =
        DefaultRoleSuccessor(model_.spaces, space_id);
    if (!successor.empty()) {
      model_.default_space_id = successor;
      // A user hand-off publishes; sync's own hide does not.
      if (!sync_applying_ && on_default_role_moved_) {
        on_default_role_moved_(successor);
      }
    }
    LOG(INFO) << "[PhiClient] default Space role handed to "
              << model_.default_space_id;
  }
  const std::string retreat = DefaultSpaceId();
  for (PhiShellWindow* controller : registry_.Controllers()) {
    if (controller->is_agent() || !controller->HasSpace(space_id) ||
        retreat == space_id) {
      continue;
    }
    bool has_other = false;
    for (const model::Space& space : model_.spaces) {
      if (space.id != space_id && controller->HasSpace(space.id)) {
        has_other = true;
        break;
      }
    }
    if (!has_other) {
      host_.SurfaceSpaceInSlot(controller->slot(), retreat, nullptr);
    }
  }
  for (PhiShellWindow* controller : registry_.Controllers()) {
    if (controller->is_agent()) {
      controller->CloseSpaceBrowser(space_id);
    } else {
      controller->RetireSpaceBrowser(space_id);
    }
  }
}

// --- Phi native sync ---------------------------------------------------------

namespace {

// Clears sync_applying_ on every return path.
class ScopedFlag {
 public:
  explicit ScopedFlag(bool& flag) : flag_(flag) { flag_ = true; }
  ~ScopedFlag() { flag_ = false; }
  ScopedFlag(const ScopedFlag&) = delete;
  ScopedFlag& operator=(const ScopedFlag&) = delete;

 private:
  RAW_PTR_EXCLUSION bool& flag_;
};

std::optional<double> OpacityIn(const base::DictValue* entry,
                                std::string_view key) {
  if (!entry) {
    return std::nullopt;
  }
  return entry->FindDouble(key);
}

}  // namespace

std::vector<PhiSpaceManager::SyncRow> PhiSpaceManager::SyncRows() const {
  std::vector<SyncRow> rows;
  if (!store_.is_open()) {
    return rows;
  }
  const base::DictValue* opacities =
      defaults_.GetDict(kDefaultsSpaceOverlayOpacities);
  auto add = [&](const model::Space& space, bool hidden) {
    const store::Space* stored = store_.FindSpace(space.id);
    if (!stored) {
      return;  // not flushed yet: next round
    }
    SyncRow row;
    row.id = space.id;
    row.profile_id = stored->profile_id;
    row.name = stored->name;
    row.color_hex = stored->color_hex;
    row.icon = stored->icon_name;  // raw: a peer's icon is echoed as is
    row.theme_id = space.theme_id;
    const base::DictValue* entry =
        opacities ? opacities->FindDict(space.id) : nullptr;
    row.opacity_light = OpacityIn(entry, "light");
    row.opacity_dark = OpacityIn(entry, "dark");
    row.created_ms = stored->created;
    row.hidden = hidden;
    rows.push_back(std::move(row));
  };
  for (const model::Space& space : model_.spaces) {
    add(space, false);
  }
  for (const model::Space& space : hidden_) {
    add(space, true);
  }
  return rows;
}

bool PhiSpaceManager::SyncCreate(const SyncRow& row) {
  if (!store_.is_open() || row.id.empty() || row.profile_id.empty() ||
      Has(row.id) || PhiIsAgentSpaceId(row.id)) {
    return false;
  }
  ScopedFlag applying(sync_applying_);
  const std::string color = row.color_hex.empty()
                                ? std::string(store::kDefaultSpaceColorHex)
                                : row.color_hex;
  if (!store_.CreateSpace(row.id, row.profile_id, row.name, color, row.icon,
                          row.created_ms > 0
                              ? std::optional<store::TimeMs>(row.created_ms)
                              : std::nullopt)) {
    return false;
  }
  model::Space space;
  space.id = row.id;
  space.name = row.name;
  space.profile_id = row.profile_id;
  space.icon = PhiIconIsValid(row.icon) ? row.icon
                                        : store::kDefaultSpaceIconName;
  space.color_hex = color;
  space.theme_id = row.theme_id;
  model_.spaces.push_back(std::move(space));
  MarkDirty();
  RebuildSpacePalettes();
  Notify(SpacesChange::kList);
  return true;
}

bool PhiSpaceManager::SyncUpdate(const std::string& space_id,
                                 const std::optional<std::string>& name,
                                 const std::optional<std::string>& color_hex,
                                 const std::optional<std::string>& icon,
                                 std::optional<int64_t> created_ms) {
  model::Space* space = nullptr;
  for (model::Space& candidate : model_.spaces) {
    if (candidate.id == space_id) {
      space = &candidate;
    }
  }
  for (model::Space& candidate : hidden_) {
    if (candidate.id == space_id) {
      space = &candidate;
    }
  }
  if (!space || !store_.is_open()) {
    return false;
  }
  ScopedFlag applying(sync_applying_);
  if (!store_.UpdateSpace(space_id, name, color_hex, icon, created_ms)) {
    return false;
  }
  if (name) {
    space->name = *name;
  }
  if (color_hex) {
    space->color_hex = *color_hex;
  }
  if (icon) {
    space->icon =
        PhiIconIsValid(*icon) ? *icon : store::kDefaultSpaceIconName;
  }
  MarkDirty();
  Notify(SpacesChange::kList);
  return true;
}

bool PhiSpaceManager::SyncRebind(const std::string& space_id,
                                 const std::string& profile_id) {
  const model::Space* space = Find(space_id);
  if (!space) {
    return false;
  }
  if (space->profile_id == profile_id) {
    return true;
  }
  ScopedFlag applying(sync_applying_);
  // The local rebind path (browser respawn, rows move with it); the store
  // write rides the flush, so it is made here at once.
  if (!SetSpaceProfile(space_id, profile_id)) {
    return false;
  }
  const store::Space* stored = store_.FindSpace(space_id);
  if (stored && stored->profile_id != profile_id &&
      !store_.ChangeSpaceProfile(space_id, profile_id)) {
    return false;
  }
  return true;
}

bool PhiSpaceManager::SyncThemeState(const std::string& space_id,
                                     const std::optional<std::string>& theme_id,
                                     std::optional<double> opacity_light,
                                     std::optional<double> opacity_dark) {
  model::Space* space = nullptr;
  for (model::Space& candidate : model_.spaces) {
    if (candidate.id == space_id) {
      space = &candidate;
    }
  }
  for (model::Space& candidate : hidden_) {
    if (candidate.id == space_id) {
      space = &candidate;
    }
  }
  if (!space) {
    return false;
  }
  ScopedFlag applying(sync_applying_);
  // The raw id: a theme this build does not have paints as the app-wide
  // one and is echoed back unchanged.
  space->theme_id = theme_id.value_or(std::string());
  base::DictValue opacities;
  if (const base::DictValue* stored =
          defaults_.GetDict(kDefaultsSpaceOverlayOpacities)) {
    opacities = stored->Clone();
  }
  base::DictValue entry;
  if (opacity_light) {
    entry.Set("light", *opacity_light);
  }
  if (opacity_dark) {
    entry.Set("dark", *opacity_dark);
  }
  if (entry.empty()) {
    opacities.Remove(space_id);
  } else {
    opacities.Set(space_id, std::move(entry));
  }
  defaults_.SetDict(kDefaultsSpaceOverlayOpacities, std::move(opacities));
  MarkDirty();
  RebuildSpacePalettes();
  Notify(SpacesChange::kTheme);
  return true;
}

bool PhiSpaceManager::SyncOrder(const std::vector<std::string>& ordered) {
  std::vector<model::Space> spaces;
  for (const std::string& id : ordered) {
    if (const model::Space* space = Find(id)) {
      spaces.push_back(*space);
    }
  }
  if (spaces.size() != model_.spaces.size()) {
    return false;  // the list moved under the round: next round
  }
  if (spaces == model_.spaces) {
    return true;
  }
  ScopedFlag applying(sync_applying_);
  model_.spaces = std::move(spaces);
  MarkDirty();
  Notify(SpacesChange::kOrder);
  return true;
}

bool PhiSpaceManager::SyncHide(const std::string& space_id) {
  if (hidden_ids_.count(space_id)) {
    return true;
  }
  if (!Find(space_id) || model_.spaces.size() <= 1) {
    return false;
  }
  ScopedFlag applying(sync_applying_);
  hidden_ids_.insert(space_id);
  RetreatFromSpace(space_id);
  ApplyHiddenSet();
  MarkDirty();
  RebuildSpacePalettes();
  Notify(SpacesChange::kList);
  return true;
}

bool PhiSpaceManager::SyncPurge(const std::string& space_id) {
  if (!store_.is_open()) {
    return false;
  }
  ScopedFlag applying(sync_applying_);
  if (Find(space_id)) {
    if (model_.spaces.size() <= 1) {
      return false;
    }
    RetreatFromSpace(space_id);
    std::erase_if(model_.spaces,
                  [&](const model::Space& s) { return s.id == space_id; });
  }
  std::erase_if(hidden_,
                [&](const model::Space& s) { return s.id == space_id; });
  hidden_ids_.erase(space_id);
  if (store_.FindSpace(space_id) &&
      !store_.DeleteSpaceCascade(space_id, /*user_intent=*/false)) {
    return false;
  }
  MarkDirty();
  RebuildSpacePalettes();
  Notify(SpacesChange::kList);
  return true;
}

void PhiSpaceManager::ApplyAccountDefaultSpace(const std::string& space_id) {
  if (!Find(space_id) || model_.default_space_id == space_id) {
    return;
  }
  ScopedFlag applying(sync_applying_);
  model_.default_space_id = space_id;
  MarkDirty();
  Notify(SpacesChange::kList);
}

void PhiSpaceManager::SetSyncHiddenSpaces(const std::set<std::string>& ids) {
  if (ids == hidden_ids_) {
    return;
  }
  ScopedFlag applying(sync_applying_);
  const std::set<std::string> before = hidden_ids_;
  hidden_ids_ = ids;
  for (const std::string& id : ids) {
    // Never hide the last visible Space.
    if (!before.count(id) && Find(id)) {
      if (model_.spaces.size() <= 1) {
        hidden_ids_.erase(id);
        continue;
      }
      RetreatFromSpace(id);
    }
  }
  ApplyHiddenSet();
  RebuildSpacePalettes();
  Notify(SpacesChange::kList);
}

void PhiSpaceManager::ApplyHiddenSet() {
  // Unhide: back at the end of the strip.
  for (auto it = hidden_.begin(); it != hidden_.end();) {
    if (!hidden_ids_.count(it->id)) {
      model_.spaces.push_back(std::move(*it));
      it = hidden_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = model_.spaces.begin(); it != model_.spaces.end();) {
    if (hidden_ids_.count(it->id) && model_.spaces.size() > 1) {
      hidden_.push_back(std::move(*it));
      it = model_.spaces.erase(it);
    } else {
      ++it;
    }
  }
  if (!Find(model_.default_space_id) && !model_.spaces.empty()) {
    model_.default_space_id = model_.spaces.front().id;
  }
}

}  // namespace phi_client

// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_sync_settings_win.h"

#include <memory>
#include <string>
#include <utility>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_local_store.h"
#include "client/phi_preferences.h"
#include "client/phi_shell_layout.h"
#include "client/phi_sync_store_win.h"
#include "client/phi_theme.h"

namespace phi_client {

namespace {

using sync::SettingValue;
using sync::SyncableSetting;

// A read on the UI thread; nullopt when the UI thread could not run it.
std::optional<SettingValue> ReadOnUi(const PhiRunOnUi& run_on_ui,
                                     std::function<SettingValue()> read) {
  // Captured by value: a read that times out may still run later.
  auto value = std::make_shared<std::optional<SettingValue>>();
  if (!run_on_ui([value, read] { *value = read(); })) {
    return std::nullopt;
  }
  return *value;
}

SettingValue Bool(bool value) {
  SettingValue out;
  out.SetBool(value);
  return out;
}

SettingValue String(const std::string& value) {
  SettingValue out;
  out.SetString(value);
  return out;
}

SyncableSetting BoolSetting(const std::string& key,
                            PhiRunOnUi run_on_ui,
                            std::function<bool()> get,
                            std::function<void(bool)> set) {
  return {key,
          [run_on_ui, get] {
            return ReadOnUi(run_on_ui, [get] { return Bool(get()); });
          },
          [run_on_ui, set](const SettingValue& value) {
            if (value.kind != SettingValue::Kind::kBool) {
              return;
            }
            const bool flag = value.bool_value;
            run_on_ui([set, flag] { set(flag); });
          }};
}

// A held peer value (see the header): `<key>.phiSyncHeld` is the peer's raw
// value, `<key>.phiSyncHeldLocal` the local value it stands in as.
struct Held {
  RAW_PTR_EXCLUSION PhiSyncSidecarFile* store;
  std::string key;

  // What the key reads as: the held value while the preference still shows
  // its stand-in; the local value (and the hold dropped) once the user has
  // changed it here.
  std::string Read(const std::string& local) const {
    const std::optional<std::string> held = store->Extra(key + ".phiSyncHeld");
    const std::optional<std::string> stand_in =
        store->Extra(key + ".phiSyncHeldLocal");
    if (held && stand_in && *stand_in == local) {
      return *held;
    }
    if (held) {
      Clear();
    }
    return local;
  }
  void Hold(const std::string& value, const std::string& stand_in) const {
    store->SetExtra(key + ".phiSyncHeld", value);
    store->SetExtra(key + ".phiSyncHeldLocal", stand_in);
  }
  void Clear() const {
    store->SetExtra(key + ".phiSyncHeld", std::nullopt);
    store->SetExtra(key + ".phiSyncHeldLocal", std::nullopt);
  }
};

}  // namespace

std::vector<SyncableSetting> PhiSyncableSettings(
    PhiPreferences* prefs,
    PhiRunOnUi run_on_ui,
    PhiSyncSidecarFile* held_store,
    PhiSyncSettingsHooks hooks) {
  std::vector<SyncableSetting> out;
  std::function<void(const std::string&)> set_pin_scope = hooks.set_pin_scope;

  // The default-Space role register (String): absent until seeded.
  const auto reconcile_default_space = [held_store, run_on_ui, hooks] {
    const std::optional<std::string> uuid =
        held_store->Extra(kDefaultSpaceMirrorKey);
    if (!uuid || !hooks.space_lookup || !hooks.apply_default_space) {
      return;
    }
    const std::optional<std::string> local = hooks.space_lookup(*uuid);
    if (local) {
      run_on_ui([apply = hooks.apply_default_space, space_id = *local] {
        apply(space_id);
      });
    }
  };
  out.push_back(
      {"PhiDefaultSpaceUuid",
       [held_store]() -> std::optional<SettingValue> {
         const std::optional<std::string> uuid =
             held_store->Extra(kDefaultSpaceMirrorKey);
         if (!uuid || uuid->empty()) {
           return std::nullopt;
         }
         return String(*uuid);
       },
       [held_store, reconcile_default_space](const SettingValue& value) {
         const std::string uuid = value.str();
         if (value.kind != SettingValue::Kind::kString || uuid.empty()) {
           return;
         }
         held_store->SetExtra(kDefaultSpaceMirrorKey, uuid);
         reconcile_default_space();
       },
       reconcile_default_space});

  // The pinned-tab scope (String): landing migrates the pins.
  out.push_back(
      {"PhiPinnedTabScope",
       [run_on_ui, prefs] {
         return ReadOnUi(run_on_ui, [prefs] {
           return String(store::PinScopeName(prefs->settings().pin_scope));
         });
       },
       [run_on_ui, set_pin_scope](const SettingValue& value) {
         const std::string raw = value.str();
         store::PinScope scope = store::PinScope::kProfile;
         if (value.kind != SettingValue::Kind::kString ||
             !store::PinScopeFromName(raw, &scope) || !set_pin_scope) {
           return;
         }
         run_on_ui([set_pin_scope, raw] { set_pin_scope(raw); });
       }});

  // General (Bool).
  out.push_back(BoolSetting(
      "openNewTabPageOnCmdT", run_on_ui,
      [prefs] {
        return prefs->new_tab_behavior() == model::NewTabBehavior::kNewTabPage;
      },
      [prefs](bool ntp) {
        prefs->SetNewTabBehavior(ntp ? model::NewTabBehavior::kNewTabPage
                                     : model::NewTabBehavior::kOmnibox);
      }));
  out.push_back(BoolSetting(
      "alwaysShowURLPath", run_on_ui,
      [prefs] { return prefs->AlwaysShowFullUrl(); },
      [prefs](bool on) { prefs->SetAlwaysShowFullUrl(on); }));
  out.push_back(BoolSetting(
      "alwaysShowBookmarkBar", run_on_ui,
      [prefs] { return prefs->AlwaysShowBookmarkBar(); },
      [prefs](bool on) { prefs->SetAlwaysShowBookmarkBar(on); }));
  out.push_back(BoolSetting(
      "showBookmarkBarOnNewTabPage", run_on_ui,
      [prefs] { return prefs->BookmarkBarOnNewTab(); },
      [prefs](bool on) { prefs->SetBookmarkBarOnNewTab(on); }));
  out.push_back(BoolSetting(
      "suppressCloseIncognitoSpaceWarning", run_on_ui,
      [prefs] { return prefs->SuppressCloseIncognitoSpaceWarning(); },
      [prefs](bool on) { prefs->SetSuppressCloseIncognitoSpaceWarning(on); }));

  // Layout (String). Windows has no Performance layout: a peer's
  // "performance" does not fit and is refused (the local value is kept and
  // not marked synced).
  const Held layout{held_store, "layoutMode"};
  out.push_back(
      {"layoutMode",
       [run_on_ui, prefs, layout] {
         return ReadOnUi(run_on_ui, [prefs, layout] {
           return String(layout.Read(PhiLayoutModeName(prefs->LayoutMode())));
         });
       },
       [run_on_ui, prefs, layout](const SettingValue& value) {
         const std::string raw = value.str();
         if (value.kind != SettingValue::Kind::kString ||
             (raw != "balanced" && raw != "comfortable" &&
              raw != "performance")) {
           return;  // not a layout at all: refused
         }
         run_on_ui([prefs, raw, layout] {
           if (raw == "performance") {
             // Retired here: shown as Balanced, held as the Mac's value.
             prefs->RequestLayoutMode(PhiLayoutMode::kBalanced);
             layout.Hold(raw, "balanced");
             return;
           }
           PhiLayoutMode mode = PhiLayoutMode::kBalanced;
           if (PhiLayoutModeFromName(raw, &mode)) {
             layout.Clear();
             prefs->RequestLayoutMode(mode);
           }
         });
       }});

  // Auto picture-in-picture (String).
  out.push_back(
      {"autoPictureInPictureMode",
       [run_on_ui, prefs] {
         return ReadOnUi(run_on_ui, [prefs] {
           return String(prefs->AutoPictureInPictureMode());
         });
       },
       [run_on_ui, prefs](const SettingValue& value) {
         if (value.kind != SettingValue::Kind::kString) {
           return;
         }
         const std::string mode = value.string_value;
         run_on_ui([prefs, mode] { prefs->SetAutoPictureInPictureMode(mode); });
       }});

  // Theme: appearance (Int 0 system / 1 light / 2 dark), the theme id, the
  // selection tint.
  out.push_back(
      {"PhiUserAppearanceChoice",
       [run_on_ui, prefs] {
         return ReadOnUi(run_on_ui, [prefs] {
           SettingValue v;
           v.SetInt(prefs->Appearance() == PhiAppearance::kLight  ? 1
                    : prefs->Appearance() == PhiAppearance::kDark ? 2
                                                                  : 0);
           return v;
         });
       },
       [run_on_ui, prefs](const SettingValue& value) {
         if (value.kind != SettingValue::Kind::kInt || value.int_value < 0 ||
             value.int_value > 2) {
           return;
         }
         const PhiAppearance appearance =
             value.int_value == 1   ? PhiAppearance::kLight
             : value.int_value == 2 ? PhiAppearance::kDark
                                    : PhiAppearance::kSystem;
         run_on_ui([prefs, appearance] { prefs->SetAppearance(appearance); });
       }});
  // The two catalogs share their ids. The Mac also sends "default" (a user
  // who never picked a theme: Pure) and may send a newer build's theme:
  // both are HELD — "default" shows as Pure, an unknown id leaves the
  // current theme showing (SetThemeId would quietly land on Pure).
  const Held theme{held_store, "PhiCurrentThemeId"};
  out.push_back(
      {"PhiCurrentThemeId",
       [run_on_ui, prefs, theme] {
         return ReadOnUi(run_on_ui, [prefs, theme] {
           return String(theme.Read(prefs->ThemeId()));
         });
       },
       [run_on_ui, prefs, theme](const SettingValue& value) {
         if (value.kind != SettingValue::Kind::kString ||
             value.string_value.empty()) {
           return;
         }
         const std::string id = value.string_value;
         run_on_ui([prefs, id, theme] {
           if (id == "default") {
             prefs->SetThemeId("pure");
             theme.Hold(id, "pure");
           } else if (PhiThemeById(id).id == id) {
             theme.Clear();
             prefs->SetThemeId(id);
           } else {
             theme.Hold(id, prefs->ThemeId());
           }
         });
       }});
  out.push_back(BoolSetting(
      "PhiSelectionTintEnabled", run_on_ui,
      [prefs] { return prefs->SelectionTintEnabled(); },
      [prefs](bool on) { prefs->SetSelectionTintEnabled(on); }));
  return out;
}

}  // namespace phi_client

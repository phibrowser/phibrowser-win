// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SPACE_MANAGER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SPACE_MANAGER_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_app_model.h"
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_local_store.h"
#include "phibrowser-win/client/phi_settings_types.h"
#include "phibrowser-win/client/phi_window_model.h"
#include "phibrowser-win/client/phi_window_registry.h"

// The Space list service (docs/architecture-review.md §3.4, the
// PhiSpaceManager row): the Spaces, the default role, the per-Space
// palettes and PhiSpaces.json (the URL rules share the file; PhiUrlRouter
// is their writer, through the same model).
//
// HOST-CLEAN ON PURPOSE: this header is STL plus the model, the ids and the
// registry/window-model headers the host tests already compile — no
// windows.h, and of base/ only the RAW_PTR_EXCLUSION macro (the host build
// shims it, test/model/stubs) — so the pure helpers below are host-tested
// (test/model/space_manager_test.cc). The store, the palettes and the
// framework are reached through forward declarations and the .cc.

namespace phi_framework {
class PhiFramework;
}  // namespace phi_framework

namespace phi_client {

class PhiAccountDefaults;
class PhiPreferences;
struct PhiPalette;
struct PhiSpaceView;

// --- Pure helpers (host-tested) ---------------------------------------------

inline const model::Space* FindSpaceIn(const std::vector<model::Space>& spaces,
                                       std::string_view id) {
  for (const model::Space& space : spaces) {
    if (space.id == id) {
      return &space;
    }
  }
  return nullptr;
}

// The Space holding the DEFAULT role at run time: the persisted holder when
// it still exists, "default-space" (the built-in default) when the list is
// empty, else the first Space. (The decoder's ResolveDefaultSpaceId is the load-time
// rule; this is what DefaultSpaceId() has always answered.)
inline std::string DefaultSpaceIdOf(const std::vector<model::Space>& spaces,
                                    const std::string& persisted) {
  if (FindSpaceIn(spaces, persisted)) {
    return persisted;
  }
  return spaces.empty() ? std::string("default-space") : spaces.front().id;
}

// Who inherits the default role when `deleted_id` (its holder) goes: the
// first REMAINING Space in strip order; empty when none remains.
inline std::string DefaultRoleSuccessor(const std::vector<model::Space>& spaces,
                                        std::string_view deleted_id) {
  for (const model::Space& space : spaces) {
    if (space.id != deleted_id) {
      return space.id;
    }
  }
  return std::string();
}

// Everything is deletable except the LAST remaining Space: retreat targets
// and new-window fallbacks need one persisted Space to land on.
inline bool CanDeleteSpaceIn(const std::vector<model::Space>& spaces,
                             std::string_view id) {
  return FindSpaceIn(spaces, id) != nullptr && spaces.size() > 1;
}

// The ordinal a new Incognito Space takes: the lowest one not in `used`
// (the Mac's lowest free ordinal), so "Incognito 2" comes back after the
// second one closes.
inline int NextIncognitoOrdinal(const std::vector<int>& used) {
  int ordinal = 1;
  while (std::find(used.begin(), used.end(), ordinal) != used.end()) {
    ++ordinal;
  }
  return ordinal;
}

// Where a Space re-enters a list of `count` (after its own removal) when
// asked for `new_index`: clamped to [0, count].
inline size_t ClampSpaceInsertIndex(size_t count, int new_index) {
  return static_cast<size_t>(
      std::max(0, std::min(new_index, static_cast<int>(count))));
}

// --- The service --------------------------------------------------------------

class PhiSpaceManager {
 public:
  // Which fan-out a list change needs — one value per shape the
  // coordinator used to hand-write after each mutation.
  enum class SpacesChange {
    kList,     // Name, icon, membership: every slot repaints, Settings too.
    kOrder,    // A reorder: every slot repaints (Settings keeps its own).
    kTheme,    // A per-Space theme or saturation: palettes re-applied.
    kProfile,  // A re-binding: repaint, and the wire's Space list moves.
  };
  class Observer {
   public:
    virtual void OnSpacesChanged(SpacesChange what) = 0;

   protected:
    virtual ~Observer() = default;
  };

  // What DeleteSpace and SetSpaceProfile need from the rest of the app
  // (the coordinator implements it): the slot retreat/respawn, and the data
  // a Space owns outside this list — its rows, its rules, its ghost record.
  class Host {
   public:
    // SwitchSlotToSpace: surfaces `space_id` in `slot`, requesting its
    // browser when the slot has none; `reopen_urls` (may be null) open in
    // that browser once it attaches.
    virtual void SurfaceSpaceInSlot(
        SlotId slot,
        const std::string& space_id,
        const std::vector<std::string>* reopen_urls) = 0;
    // A Space is being deleted: its bookmarks and space-scoped pins, its
    // URL rules, and its ghost/parked window go with it.
    virtual void ForgetSpaceData(const std::string& space_id) = 0;
    // A Space was re-bound to `profile_id`: the rows it owns move with it,
    // and any parked window from the OLD profile is dropped.
    virtual void RehomeSpaceData(const std::string& space_id,
                                 const std::string& profile_id) = 0;

   protected:
    virtual ~Host() = default;
  };

  // A mutation writes nothing itself: it calls `dirty`, and the owner folds
  // the write into its coalesced-save timer (then calls Save()).
  using DirtyCallback = std::function<void()>;
  // Every reference outlives the service (the coordinator owns them all).
  // Every reference outlives the service (the coordinator owns them all).
  // `store` and `defaults` are the open account's (OpenDataAccount); the
  // manager's model is loaded from and saved into them.
  PhiSpaceManager(PhiWindowRegistry& registry,
                  const PhiWindowModel& window_model,
                  const PhiPreferences& preferences,
                  phi_framework::PhiFramework& framework,
                  Host& host,
                  store::LocalStore& store,
                  PhiAccountDefaults& defaults,
                  DirtyCallback dirty);
  ~PhiSpaceManager();

  PhiSpaceManager(const PhiSpaceManager&) = delete;
  PhiSpaceManager& operator=(const PhiSpaceManager&) = delete;

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // --- The store ----------------------------------------------------------
  // Reads the model from the open account: the Spaces and URL rules from its
  // LocalStore, the default role and the per-Space theme overrides from its
  // account defaults. An empty store gets the Mac's first-launch
  // `default-space` (ensureDefaultSpace). No observer hears a load; the
  // owner rebuilds the palettes once the preferences are in.
  void Load();
  // Reconciles the store with the model: creates, updates, re-binds,
  // deletes and reorders Spaces, applies the rule changes as ONE edit set
  // (the Mac's applyURLRuleEdits — stable ids, soft deletes, pending
  // flags), and writes the default role and theme overrides. Idempotent.
  // The owner's flush is the only caller.
  void Save();
  // After the model was reloaded for another account: every window showing
  // a Space this account does not have retreats to the default Space and
  // closes that browser (the delete path's order), then everyone repaints.
  void RetreatFromMissingSpaces();
  const model::PhiSpacesModel& model() const { return model_; }
  // THE exception to "this service is the only writer of the list": the
  // verb ladder's agentSpace.spaces.create (and the urlRules.* verbs, on
  // the router's half) still write the model directly until the typed verb
  // registry lands. Nothing else may.
  model::PhiSpacesModel& mutable_model() { return model_; }

  // --- Reads --------------------------------------------------------------
  const std::vector<model::Space>& spaces() const { return model_.spaces; }
  // A stored Space or a live Incognito Space.
  const model::Space* Find(const std::string& id) const;
  bool Has(const std::string& id) const { return Find(id) != nullptr; }
  // The number a new Space's default name carries ("Space 3"). Only a
  // name: Space ids are UUIDs, as on the Mac.
  int next_space_id() const {
    return static_cast<int>(model_.spaces.size()) + 1;
  }
  // Every Space, in display order, as the chips row and menus consume it.
  std::vector<PhiSpaceView> Spaces() const;
  // Every Space as the SETTINGS panes read it: id, name, display icon,
  // profile, and the per-Space theme/saturation overrides. A struct, not a
  // packed row — the panes name the fields (lane 1 §1.3).
  std::vector<PhiSpaceSummary> Summaries() const;
  // The Space holding the DEFAULT role (the Mac's currentDefaultSpaceId):
  // the landing target for retreats and windows opened without a Space and
  // the pre-selected bookmark target. Starts on "1" and,
  // when its holder is deleted, is handed to the first remaining Space —
  // persisted with the Space list so the hand-off survives relaunches.
  std::string DefaultSpaceId() const;
  bool CanDeleteSpace(const std::string& space_id) const;
  // The Space a Kiosk offers to hand its page to: the main window's ACTIVE
  // Space, the Space holding the default role when no window is up — the
  // Mac's KioskSpaceMenuTargetResolver.primarySpace.
  std::string KioskTargetSpaceId() const;
  std::string SpaceThemeId(const std::string& space_id) const;
  // -1 follows the app-wide slider. A Space with a linked palette answers
  // the palette's own saturation (its radius on the field, as 0..100) —
  // the Mac's effectiveOverlaySaturation.
  int SpaceSaturation(const std::string& space_id) const;
  // The Space's linked palette (the Mac's themePalette(forSpaceId:)), or
  // nullopt when it paints with its theme pin.
  std::optional<PhiThemePalette> SpacePalette(const std::string& space_id) const;

  // --- Writes (each persists and publishes SpacesChanged) -----------------
  bool RenameSpace(const std::string& space_id, const std::string& name);
  // Only the LAST Space is undeletable (the Mac's last-Space rule); the
  // default Space itself may go, handing its role on. Slots whose only
  // browser is the Space's retreat to the default Space first (through the
  // Host), the Space's browsers close, its data is forgotten (Host), then
  // the list entry goes.
  bool DeleteSpace(const std::string& space_id);
  // Moves a Space to `new_index` in the list (chip drag).
  void ReorderSpace(const std::string& space_id, int new_index);
  // Refused for an icon the catalogue does not know.
  void SetSpaceIcon(const std::string& space_id, const std::string& icon);
  // An empty id clears the override, putting the Space back on the app-wide
  // theme — the Mac's "no entry in spaceThemeIds" state. Choosing a preset
  // also DROPS the Space's linked palette (the Mac's setTheme).
  void SetSpaceTheme(const std::string& space_id, const std::string& theme_id);
  // -1 follows the app-wide slider; anything else clamps to 0..100. With a
  // linked palette the value writes the PALETTE's saturation instead (the
  // Mac's setOverlaySaturation), so the Settings slider still works.
  void SetSpaceSaturation(const std::string& space_id, int value);
  // The create-Space form's pin: theme and saturation in one write (any
  // palette goes, as for SetSpaceTheme; the form sets its own after).
  void PinSpaceTheme(const std::string& space_id,
                     const std::string& theme_id,
                     int saturation);
  // The Space theme editor's writes (the Mac's setThemePalette /
  // previewThemePalette). Set persists the record and fans the theme change
  // out like any other; Preview repaints the Space's windows with the
  // record WITHOUT writing it, publishing a sync note or waking Settings —
  // pointer tracking previews per move and sets once on release.
  void SetSpacePalette(const std::string& space_id,
                       const PhiThemePalette& palette);
  void PreviewSpacePalette(const std::string& space_id,
                           const PhiThemePalette& palette);
  // Re-binds a Space to another profile (the Mac's changeProfile): its
  // browser is closed and respawned in the new profile with its open tabs,
  // and the rows it owns move with it. Refused for the default Space, whose
  // browser is the one a fresh shell binds to. False when nothing changed.
  bool SetSpaceProfile(const std::string& space_id,
                       const std::string& profile_id);

  // --- Incognito Spaces (docs/incognito-space-port.md) ----------------------
  // Runtime-only Spaces that ride the list's reads — Find, Spaces, the
  // palettes, the framework's Space menu — and none of its writes: Save,
  // sync, Summaries and the default role never see them, since they live
  // apart from model_. Adds one after every Space ("Incognito", or
  // "Incognito N" once there are several) and answers its id.
  std::string CreateIncognitoSpace();
  // Drops the entry. The caller has moved every slot off it and retired its
  // browsers first (the coordinator's CloseIncognitoSpace).
  bool RemoveIncognitoSpace(const std::string& space_id);
  // The live Incognito Spaces in strip order.
  std::vector<std::string> IncognitoSpaceIds() const;

  // --- Palettes -------------------------------------------------------------
  // Re-resolves the per-Space overrides against the app-wide theme. Run
  // after a load, after any theme or saturation change (app-wide or
  // per-Space) and after a Space list change.
  void RebuildSpacePalettes();
  // The palette `space_id` paints with: its override, else the app-wide one.
  const PhiPalette& PaletteForSpace(const std::string& space_id) const;
  // The palette a SLOT paints with: its active Space's.
  const PhiPalette& PaletteForSlot(int64_t slot) const;

  // --- The framework's copy -------------------------------------------------
  // Publishes the Space list (with each Space's open browser id) into the
  // framework for the page context menu's "Open Link in Space" submenu —
  // the body; the coordinator coalesces the calls to one per turn.
  void PushSpaceMenuNow();

  // --- Phi native sync (docs/user-data-parity.md S5) ------------------------
  // The Space kind's local side (phi_sync_spaces_win.cc marshals here). The
  // owner flushes the model into the store before SyncRows. Every Sync*
  // write lands in the store at once AND in the model, publishes
  // SpacesChanged, and returns false when it did not land; while one runs
  // sync_applying() is true, so the owner does not read it as a local edit.
  struct SyncRow {
    std::string id;
    std::string profile_id;
    std::string name;
    std::string color_hex;
    std::string icon;
    std::string theme_id;  // "" = follows the app-wide theme
    std::optional<double> opacity_light;
    std::optional<double> opacity_dark;
    int64_t created_ms = 0;
    bool hidden = false;
  };
  // Every stored Space in strip order, hidden ones last.
  std::vector<SyncRow> SyncRows() const;
  bool SyncCreate(const SyncRow& row);
  bool SyncUpdate(const std::string& space_id,
                  const std::optional<std::string>& name,
                  const std::optional<std::string>& color_hex,
                  const std::optional<std::string>& icon,
                  std::optional<int64_t> created_ms);
  bool SyncRebind(const std::string& space_id, const std::string& profile_id);
  // The Mac's applyRemoteThemeState: the theme pin and the overlay
  // opacities (spaceOverlayOpacities, carried for the Mac; Windows paints
  // with its own saturation).
  bool SyncThemeState(const std::string& space_id,
                      const std::optional<std::string>& theme_id,
                      std::optional<double> opacity_light,
                      std::optional<double> opacity_dark);
  bool SyncOrder(const std::vector<std::string>& ordered_space_ids);
  // A remote soft delete: the Space leaves every list, its windows retreat,
  // its row stays stored until retention purges it. Refused for the last
  // visible Space.
  bool SyncHide(const std::string& space_id);
  // Retention: the stored row and what it owns go for good.
  bool SyncPurge(const std::string& space_id);
  // The account's default-Space register landed: the role moves to
  // `space_id` when it is a Space here (applyAccountDefaultSpace).
  void ApplyAccountDefaultSpace(const std::string& space_id);
  // Told when the default role moves to another Space on a user delete.
  void set_on_default_role_moved(std::function<void(const std::string&)> callback) {
    on_default_role_moved_ = std::move(callback);
  }
  // Sync landed URL rules in the store: the model's rules follow (Save
  // diffs the model against the store, so a stale model would undo them).
  void ReloadUrlRulesFromStore();
  // The sync table's hidden set (local ids), after every table write.
  void SetSyncHiddenSpaces(const std::set<std::string>& ids);
  bool sync_applying() const { return sync_applying_; }
  // The sync-hidden Spaces: in no list, but still stored and still bound
  // to their profiles (a hidden Space can come back).
  const std::vector<model::Space>& hidden_spaces() const { return hidden_; }

 private:
  void MarkDirty();
  void Notify(SpacesChange what);
  // DeleteSpace's window half, shared with SyncHide: the default role moves
  // on, slots showing only this Space retreat to the default Space, and its
  // browsers close.
  void RetreatFromSpace(const std::string& space_id);
  // Moves Spaces between the model and hidden_ to match hidden_ids_.
  void ApplyHiddenSet();

  model::PhiSpacesModel model_;  // Spaces + rules, as loaded/saved above.
  // Sync-hidden Spaces: out of the model (so out of every list), still
  // stored — Save neither deletes nor reorders them away.
  std::set<std::string> hidden_ids_;
  std::vector<model::Space> hidden_;
  bool sync_applying_ = false;
  std::function<void(const std::string&)> on_default_role_moved_;
  // One resolved palette per Space that overrides the app-wide theme; a
  // Space with no override is not in here and uses the preferences' palette.
  std::map<std::string, std::unique_ptr<PhiPalette>> palette_by_space_;
  // §5.10: the fixed graphite ground every agent Space wears (never a
  // user theme), rebuilt with the Space palettes.
  std::unique_ptr<PhiPalette> agent_palette_;
  // The live Incognito Spaces, apart from model_ so nothing persists them;
  // each keeps the ordinal its name was given for its whole life.
  struct IncognitoEntry {
    model::Space space;
    int ordinal = 1;
  };
  std::vector<IncognitoEntry> incognito_;
  // Their fixed-dark palette (the incognito window's), whatever the theme.
  std::unique_ptr<PhiPalette> incognito_palette_;
  // Non-owning; all outlive the service (see the constructor). The
  // framework is a singleton across a module boundary.
  RAW_PTR_EXCLUSION PhiWindowRegistry& registry_;
  RAW_PTR_EXCLUSION const PhiWindowModel& window_model_;
  RAW_PTR_EXCLUSION const PhiPreferences& preferences_;
  RAW_PTR_EXCLUSION phi_framework::PhiFramework& framework_;
  RAW_PTR_EXCLUSION Host& host_;
  RAW_PTR_EXCLUSION store::LocalStore& store_;
  RAW_PTR_EXCLUSION PhiAccountDefaults& defaults_;
  DirtyCallback dirty_;
  std::vector<Observer*> observers_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SPACE_MANAGER_H_

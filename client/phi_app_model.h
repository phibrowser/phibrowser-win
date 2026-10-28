// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_APP_MODEL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_APP_MODEL_H_

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "phibrowser-win/client/phi_shell_layout.h"
#include "phibrowser-win/client/phi_tab_store.h"
#include "phibrowser-win/client/phi_theme_palette.h"

// The application model (docs/architecture-review.md §3.4, lane 4 §5): the
// four persisted domains the coordinator's ad-hoc load/save pairs are
// being folded into, as plain values. Each struct is the WHOLE contents of
// one file; PhiStore<T> (phi_store.h) reads and writes it through the
// PhiStoreTraits specialisations in phi_app_model.cc.
//
// PURE ON PURPOSE: STL only, no Win32, no base/ — so the defaults and the
// id helpers are host-tested (test/model/app_model_test.cc). Anything that
// needs base::Value lives in the .cc.
//
// NAMESPACE: phi_client::model. Space and UrlRule are THE definitions (the
// coordinator aliases them); ShortcutChord and ReaderStyle twin
// PhiShortcutChord and PhiReaderStyle, whose headers pull in windows.h /
// base::Value, and are copied field-wise at the coordinator's boundary.
// Nothing in this header collides with a name outside the model namespace.

namespace phi_client {
namespace model {

// --- Shared value types ----------------------------------------------------

// A Space binds EXACTLY ONE profile; several Spaces may share a profile
// (the fork's model). The binding is fixed for the life of a browser
// window — changing it goes through SetSpaceProfile, which respawns the
// Space's window.
struct Space {
  std::string id;
  std::string name;
  std::string profile_id = "Default";  // On-disk basename.
  // Icon in the Mac's storage form ("emoji:<id>" / "phi:<id>"). The default
  // is kPhiDefaultSpaceIcon; spelled out here so the header stays free of
  // phi_emoji.h (the .cc validates against the catalogue on decode).
  std::string icon = "phi:phi-icon-view-grid-add";
  // Per-Space THEME OVERRIDE; empty id and -1 saturation mean "follow the
  // app-wide choice".
  std::string theme_id;
  int saturation = -1;
  // The Space's linked palette (the Mac's spaceThemePalettes record): when
  // set it beats the theme pin's colours AND the saturation above — the
  // pin still names the base theme (its window background, what a peer
  // renders). LOCAL ONLY: never on the Space sync payload.
  std::optional<PhiThemePalette> palette;
  std::string color_hex;  // "#RRGGBB", cosmetic, echoed for the Mac wire.
  bool operator==(const Space&) const = default;
};

// An Incognito Space (docs/incognito-space-port.md): a RUNTIME-only Space —
// never stored, synced or restored — whose browser runs in the shared
// "PhiIncognitoSpace" off-the-record profile. Its id is this prefix plus a
// UUID (the Mac's SpaceManager.incognitoSpaceIdPrefix); the bare
// "space.incognito" is the URL-rule TARGET, not a Space.
inline constexpr char kIncognitoSpaceIdPrefix[] = "space.incognito.";
// The Mac's incognito Space icon (🥷) and colour.
inline constexpr char kIncognitoSpaceIcon[] = "emoji:1F977";
inline constexpr char kIncognitoSpaceColorHex[] = "#5F6368";

inline bool IsIncognitoSpaceId(std::string_view id) {
  return id.starts_with(kIncognitoSpaceIdPrefix);
}

// A Space URL routing rule (the Mac's SpaceURLRule). `host` is in the wire
// form ("github.com" exact, "*.foo.com" suffix, "*needle*" contains);
// `path_prefix` empty means any path; `ask` prompts instead of routing
// silently. Vector order is the sortOrder tiebreak.
struct UrlRule {
  std::string space_id;
  std::string host;
  std::string path_prefix;  // Empty = any path; "/" = the root only.
  bool ask = false;
  // The stored rule's stable id (SpaceURLRule.id), the one the Settings
  // editor and the urlRules.* verbs name it by. Empty until the rule is
  // first saved: the save mints it.
  std::string id;
  bool operator==(const UrlRule&) const = default;
};

// One keyboard chord. vk 0 means "explicitly unbound". Plain unsigned
// rather than UINT so the header has no windows.h; the two are the same
// type on the target.
struct ShortcutChord {
  unsigned vk = 0;
  bool ctrl = false;
  bool shift = false;
  bool alt = false;
  bool operator==(const ShortcutChord&) const = default;
};

// Reader View's presentation (PhiReaderStyle's fields and defaults; the
// wire names are the extension's). Validation of the string values stays
// in PhiReaderStyle::MergeFrom — the .cc decodes through it.
struct ReaderStyle {
  int font_size = 19;
  std::string width = "normal";
  std::string theme = "matchBrowser";
  std::string typeface = "serif";
  std::string line_height = "normal";
  std::string letter_spacing = "normal";
  bool shows_images = true;
  bool highlights_links = true;
  bool operator==(const ReaderStyle&) const = default;
};

// PhiAppearance / PhiNewTabBehavior twins. Their homes (phi_theme.h,
// phi_settings_window.h) include windows.h, which would take this header
// off the host; same enumerators in the same order so a static_cast is a
// faithful bridge until the follow-up aliases them.
enum class Appearance { kSystem, kLight, kDark };
enum class NewTabBehavior { kNewTabPage, kOmnibox };

// Wire names shared with the Mac client ("system"/"light"/"dark",
// "newTabPage"/"omnibox"). FromName leaves `out` untouched on an unknown
// name so a hand-edited file cannot pick a value the UI cannot.
inline const char* AppearanceName(Appearance appearance) {
  switch (appearance) {
    case Appearance::kLight:
      return "light";
    case Appearance::kDark:
      return "dark";
    case Appearance::kSystem:
      break;
  }
  return "system";
}

inline bool AppearanceFromName(std::string_view name, Appearance* out) {
  if (name == "system") {
    *out = Appearance::kSystem;
  } else if (name == "light") {
    *out = Appearance::kLight;
  } else if (name == "dark") {
    *out = Appearance::kDark;
  } else {
    return false;
  }
  return true;
}

inline const char* NewTabBehaviorName(NewTabBehavior behavior) {
  return behavior == NewTabBehavior::kOmnibox ? "omnibox" : "newTabPage";
}

inline bool NewTabBehaviorFromName(std::string_view name,
                                   NewTabBehavior* out) {
  if (name == "omnibox") {
    *out = NewTabBehavior::kOmnibox;
  } else if (name == "newTabPage") {
    *out = NewTabBehavior::kNewTabPage;
  } else {
    return false;
  }
  return true;
}

// --- PhiPreferences.json (schema 1) -----------------------------------------
//
// Device-wide, the Mac's UserDefaults.standard: keys spelled as the Mac
// spells them (docs/user-data-parity.md §4.4) so the synced ones are the
// same keys on both. Windows-only keys keep Windows names.
//
// TRUTH OWNER: client Settings (lane 4 §5 table, "restore-on-startup,
// language, peek prefs") for EVERY key. RULE: the framework holds a pushed
// MIRROR of the few it needs — restoreLastSession (each profile's
// SessionStartupPref), language (Chromium's locale pref), the peek pair —
// pushed on profile load and on change, never from "the path that loaded
// the file", and never read back from it. The chrome layout and the theme
// are client-only concepts (nothing in Chromium mirrors them); the
// palettes are derived from the theme, never persisted. Folds the
// pre-phase-3 PhiPrefs.json + PhiLayout.json + PhiTheme.json. Defaults are
// today's coordinator defaults (the Mac's defaultValues).
struct PhiSettings {
  // Layout (PhiLayout.json).
  PhiLayoutMode layout_mode = PhiLayoutMode::kPerformance;
  // ACCOUNT-SCOPED (the Mac's lastKnownSidebarWidth): the bound account's
  // value, never in the global file.
  int sidebar_width = 240;  // kSidebarWidthDefault; clamped on decode.
  // The Settings window's last client size in dip (windows-native-ui.md
  // §7: a Windows Settings window remembers its size). 0 = the default.
  int settings_window_width = 0;
  int settings_window_height = 0;
  // Theme (PhiTheme.json).
  std::string theme_id = "pure";
  Appearance appearance = Appearance::kSystem;
  // General (PhiPrefs.json).
  int saturation = 10;  // A light wash (Jixiang, 2026-09-10).
  NewTabBehavior new_tab_behavior = NewTabBehavior::kNewTabPage;
  bool always_show_full_url = false;
  bool show_tab_previews = true;
  // The Mac's GeneralSettings.showOpenTabIndicators: a dot on inactive
  // pinned tabs and bookmarks whose tab is open. Off by default, as on Mac.
  bool show_open_tab_indicators = false;
  // The Mac's GeneralSettings.dimUnloadedTabIcons: a reclaimed (unloaded or
  // discarded) tab's favicon and open dot draw at 30%. On by default.
  bool dim_unloaded_tab_icons = true;
  // Appearance ▸ "Sidebar transparency", 0-100 %: how much of the Desktop
  // Acrylic backdrop shows through the sidebar's theme tint. 0 paints the
  // palette opaque; 20 is the Mac's 80 % overlay. Windows only.
  int sidebar_transparency = 50;
  bool peek_view_enabled = true;
  bool auto_peek_view_enabled = true;
  // The Mac's GeneralSettings.openKioskOnCommandOptionClick: a link clicked
  // with Ctrl+Alt held (the Mac's Cmd+Opt) opens in a Kiosk window. On by
  // default, as on Mac.
  bool open_kiosk_on_ctrl_alt_click = true;
  // Folio (docs/folio-port.md), the Mac's PhiPreferences.SaveForLater:
  // the global folder (empty = Documents\PhiFolio), per-profile overrides
  // keyed by profile id, and the opt-in for site-action auto-saves.
  std::string folio_folder;
  std::map<std::string, std::string> folio_profile_folders;
  bool folio_auto_save_on_site_actions = false;
  bool restore_last_session = false;
  bool always_show_bookmark_bar = false;
  // The Mac's showBookmarkBarOnNewTabPage, and its default.
  bool bookmark_bar_on_new_tab = true;
  // ACCOUNT-SCOPED (the Mac's CustomShortcuts.json beside the account's
  // store): loaded by PhiPreferences::BindAccount, never in the global
  // file. Keyed by the shortcut's STABLE NAME, never the command int.
  std::map<std::string, ShortcutChord> shortcut_overrides;
  ReaderStyle reader;
  std::string language;  // Empty = follow the system.
  bool developer_mode = false;
  // The runtime gates. Never settable over the control channel; this file,
  // hand-edited or seeded by the box runner, is their only source.
  // Developer ▸ "Allow agents to control Phi": the master switch. On, any
  // process that reaches the pipe with the launch token is admitted with
  // no further check — Windows has no code-signature identity for the
  // peer, so there is nothing meaningful to ask the user about it. Off,
  // the first connection raises a prompt that offers to turn it on.
  bool allow_agent_control = false;
  // The Mac's PhiAgentUserSpaceOperationsEnabled, behind the master switch
  // above. OFF by default on Windows (the Mac defaults it on): with no
  // program identity to judge, admitted agents start in their own Spaces
  // and the user opts into the rest in Settings ▸ Developer.
  bool allow_agent_user_spaces = false;
  // View ▸ Agent Autoview (the Mac's AgentSpaces.autoViewEnabled): while
  // on, the focused window follows the operating agent — a running,
  // agent-owned Space is surfaced when the user is not already watching
  // one. Off by default.
  bool agent_auto_view = false;
  // Profiles an agent may NOT bind a new agent Space to (the Mac's
  // AgentSpaces.disallowedAgentProfileIds). Stored as the refusals rather
  // than the permissions so a profile created later is allowed by default,
  // which is what the Mac's set does. Never settable over the control
  // channel — an agent cannot widen its own reach.
  std::vector<std::string> disallowed_agent_profile_ids;
  bool test_rig = false;
  // Where a pinned tab lives (the Mac's PinnedTabScope). The TAB STORE is
  // its truth (its rows re-own themselves when it changes, so the value
  // and the rows must move together in PhiTabData.json); this is a
  // write-through MIRROR the coordinator refreshes after every tab-store
  // change and load, so the two files cannot disagree. Read from
  // PhiTabData.json on import.
  PhiPinScope pin_scope = PhiPinScope::kProfile;
  // Telemetry consent (phase 3 "telemetry consent pref"). OFF by default:
  // nothing leaves the machine until the user opts in.
  bool analytics_enabled = false;
  // Browser access (docs/login-port.md §8, the Mac's ApplicationState):
  // the user's explicit "Explore Phi without signing in" choice — the ONE
  // access state that persists; signed-in access is derived from the Auth0
  // session plus the account's finished onboarding.
  bool guest_mode_enabled = false;
  // The Mac's app-level phiLoginPhase: the phase last written for any
  // account (0 = at the login screen). The per-account `loginPhase` itself
  // lives in each account's defaults (PhiPreferences::OnboardingPhase).
  int login_phase = 0;
  // Synced Mac settings Windows carries without a UI of its own yet, so a
  // value set on a Mac is kept, not reset (docs/user-data-parity.md §4.4).
  bool suppress_close_incognito_space_warning = false;
  std::string auto_picture_in_picture_mode = "normal";  // off|normal|parked
  bool selection_tint_enabled = true;
  // Test rig: never gate the browser and never show onboarding (the Mac's
  // -skip-onboarding). Never agent-settable.
  bool skip_onboarding = false;
  bool operator==(const PhiSettings&) const = default;
};

// --- PhiSpaces.json (schema 2) ----------------------------------------------
//
// TRUTH OWNER: client (PhiSpaceManager / PhiUrlRouter in §3.4) — the Space
// list, the default role and the rules. RULE: the space manager is the
// only writer of the list and publishes SpacesChanged; rule ids are
// regenerated on every write. The framework holds only the pushed Space
// menu and the PhiURLRouter table, compiled copies pushed on every edit.
// Schema 1 is the pre-phase-3 {spaces, defaultSpaceId} file; schema 2
// carries the URL rules too (they were PhiUrlRules.json), because
// DeleteSpace already edits both together.
struct PhiSpacesModel {
  std::vector<Space> spaces{Space{"default-space", "Default"}};
  // The default ROLE's holder; validated against the list on decode.
  std::string default_space_id = "default-space";
  int next_space_id = 2;
  std::vector<UrlRule> url_rules;
  // Bumped on every rule write; the wire ids ("g<gen>-<index>") carry it so
  // stale ids stop resolving. Persisted so a restart cannot hand out an id
  // a pre-restart caller still holds.
  int url_rules_generation = 1;
  bool operator==(const PhiSpacesModel&) const = default;
};

// --- PhiRestore.json (schema 1) ---------------------------------------------
//
// TRUTH OWNER: client Restore (PhiSessionRestore). RULES: the Space-to-
// window binding is written ONLY from an explicit attach with a Space or a
// RestoredFromWindowId match — never from "the first shell is the default
// Space"; DeleteSpace erases the record and drops the ghost; the last
// active Space is written on every switch (coalesced) and is the cold
// start's eager input. The parked set is the framework receipt's — it is
// REPLACED from every receipt, so what is stored here is advisory only.
struct RememberedWindow {
  std::string space_id;
  std::string profile;  // On-disk basename, the ghost store's key.
  // The window's SessionID in the PREVIOUS session; names nothing live
  // until the ghost materialises under it.
  int64_t window_id = 0;
  bool operator==(const RememberedWindow&) const = default;
};

struct PhiRestoreModel {
  std::vector<RememberedWindow> remembered;
  // Profile basename -> parked window ids, as of the last receipt.
  std::map<std::string, std::vector<int64_t>> parked;
  std::string last_active_space_id;  // Empty = the default Space.
  bool operator==(const PhiRestoreModel&) const = default;
};

// --- PhiConsent.json (schema 1, DPAPI + HMAC) ------------------------------
//
// TRUTH OWNER: client Consent (PhiCredentialService) — the credential
// audit trail exists nowhere else; the framework never sees it. Standing
// grants of any kind are gone (agent admission is the allowAgentControl
// setting; a credential approval is once or ten minutes, in memory), so
// the file holds only the audit. It stays wrapped for the user (DPAPI) and
// sealed against a same-user edit (HMAC, lane 5 §4.6); a file that fails
// either check is quarantined and the model falls back to an empty trail.
enum class CredentialAccessKind { kFill, kReveal };

struct PhiConsentModel {
  // Audit lines (tab-separated), append-only.
  std::vector<std::string> credential_audit;
  bool operator==(const PhiConsentModel&) const = default;
};

// --- Pure helpers (host-tested) -------------------------------------------

// Leading decimal digits of `text` (an optional '-' first), stopping at
// the first other character — atoi's reading, which the Space-id loader
// has always used. False when there is no digit at all. Saturates rather
// than overflowing. A character loop, not from_chars: Chromium's
// -Wunsafe-buffer-usage forbids the pointer arithmetic in a header.
inline bool LeadingInt(std::string_view text, int* out) {
  bool negative = false;
  bool any = false;
  long long value = 0;
  for (char c : text) {
    if (!any && !negative && c == '-') {
      negative = true;
      continue;
    }
    if (c < '0' || c > '9') {
      break;
    }
    any = true;
    value = std::min<long long>(value * 10 + (c - '0'), 0x7fffffff);
  }
  if (!any) {
    return false;
  }
  *out = static_cast<int>(negative ? -value : value);
  return true;
}

// The next numeric Space id: one past the largest numeric id in the list,
// and never below 2 (id "1" is the built-in default's).
inline int NextSpaceIdAfter(const std::vector<Space>& spaces) {
  int max_numeric_id = 1;
  for (const Space& space : spaces) {
    int value = 0;
    // Non-numeric ids (a Mac-created uuid, say) do not take part.
    if (LeadingInt(space.id, &value)) {
      max_numeric_id = std::max(max_numeric_id, value);
    }
  }
  return max_numeric_id + 1;
}

// The default ROLE's holder: the persisted one when it still exists, else
// "default-space" when that does, else the first Space — a restored backup
// may name a Space that is gone. "default-space" when the list is empty
// (the built-in default, the Mac's LocalStore.defaultSpaceId).
inline std::string ResolveDefaultSpaceId(const std::vector<Space>& spaces,
                                         const std::string& persisted) {
  const auto exists = [&](std::string_view id) {
    return std::any_of(spaces.begin(), spaces.end(),
                       [&](const Space& s) { return s.id == id; });
  };
  if (!persisted.empty() && exists(persisted)) {
    return persisted;
  }
  if (exists("default-space") || spaces.empty()) {
    return "default-space";
  }
  return spaces.front().id;
}

// Wire rule id "g<generation>-<index>": ids from before a write stop
// resolving, the Mac contract that callers re-list before update/delete.
inline std::string UrlRuleWireId(int generation, size_t index) {
  return "g" + std::to_string(generation) + "-" + std::to_string(index);
}

// Resolves a wire id against the CURRENT generation. A bare index (no '-')
// is the legacy probe form and is accepted as-is. False for a stale
// generation or a malformed id.
inline bool ParseUrlRuleWireId(std::string_view id,
                               int generation,
                               size_t* index) {
  const std::string current = "g" + std::to_string(generation) + "-";
  std::string_view digits;
  if (id.substr(0, current.size()) == current) {
    digits = id.substr(current.size());
  } else if (id.find('-') == std::string_view::npos) {
    digits = id;
  } else {
    return false;
  }
  if (digits.empty()) {
    return false;
  }
  // Every character a digit: "g3-5x" is not an index.
  size_t value = 0;
  for (char c : digits) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = std::min<size_t>(value * 10 + static_cast<size_t>(c - '0'),
                             0x7fffffff);
  }
  *index = value;
  return true;
}

}  // namespace model
}  // namespace phi_client

// --- Store bindings ---------------------------------------------------------
//
// The PhiStoreTraits specialisations (phi_store.h) for the four files, so
// any TU can instantiate PhiStore<model::X>. Declared here with a forward-
// declared base::DictValue — the bodies are in phi_app_model.cc — which
// keeps this header off base/ and on the host.

namespace base {
class DictValue;
}  // namespace base

namespace phi_client {

template <typename T>
struct PhiStoreTraits;

template <>
struct PhiStoreTraits<model::PhiSettings> {
  static constexpr int kSchema = 1;
  static constexpr const wchar_t* kFile = L"PhiPreferences.json";
  static constexpr bool kProtected = false;
  static base::DictValue Encode(const model::PhiSettings& value);
  static bool Decode(const base::DictValue& dict,
                     int schema,
                     model::PhiSettings* out);
  static bool Import(const std::wstring& dir, model::PhiSettings* out);
};

template <>
struct PhiStoreTraits<model::PhiRestoreModel> {
  static constexpr int kSchema = 1;
  static constexpr const wchar_t* kFile = L"PhiRestore.json";
  static constexpr bool kProtected = false;
  static base::DictValue Encode(const model::PhiRestoreModel& value);
  static bool Decode(const base::DictValue& dict,
                     int schema,
                     model::PhiRestoreModel* out);
  static bool Import(const std::wstring& dir, model::PhiRestoreModel* out);
};

template <>
struct PhiStoreTraits<model::PhiConsentModel> {
  static constexpr int kSchema = 1;
  static constexpr const wchar_t* kFile = L"PhiConsent.json";
  static constexpr bool kProtected = true;
  static base::DictValue Encode(const model::PhiConsentModel& value);
  static bool Decode(const base::DictValue& dict,
                     int schema,
                     model::PhiConsentModel* out);
  static bool Import(const std::wstring& dir, model::PhiConsentModel* out);
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_APP_MODEL_H_

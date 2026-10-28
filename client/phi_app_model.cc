// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_app_model.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <optional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_file_io.h"
#include "phibrowser-win/client/phi_reader.h"
#include "phibrowser-win/client/phi_store.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_shell_window.h"

// The on-disk codecs for the four model files and the one-shot import of
// the files they replace. The legacy shapes here are EXACTLY what the
// coordinator's Load*/Save* pairs and PhiGhostSpaces wrote up to client
// 3447dd7 (field names, defaults, the phase-0 "schema" stamp); a field
// that is absent keeps the model default, as those loaders did.

namespace phi_client {

using model::PhiConsentModel;
using model::PhiRestoreModel;
using model::PhiSettings;

namespace {

// Legacy file names (pre-phase-3), imported once and then renamed.
constexpr const wchar_t* kLegacyPrefs = L"PhiPrefs.json";
constexpr const wchar_t* kLegacyGhosts = L"PhiGhostSpaces.json";
constexpr const wchar_t* kLegacyCredentialGrants = L"PhiCredentialGrants.json";

std::wstring Join(const std::wstring& dir, const wchar_t* name) {
  return dir + L"\\" + name;
}

// A legacy file is read through the same quarantining reader as the live
// ones, so a truncated pre-phase-3 file ends up as evidence, not defaults.
PhiJsonFileStatus ReadLegacy(const std::wstring& dir,
                             const wchar_t* name,
                             base::DictValue* dict) {
  return PhiReadJsonFile(Join(dir, name), dict);
}

// The legacy file has been folded into its new home: keep it, renamed, so
// a rollback (or a doubting engineer) still has the original. Never
// deleted.
void MarkImported(const std::wstring& dir, const wchar_t* name) {
  const std::wstring from = Join(dir, name);
  const std::wstring to = from + L".imported";
  if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    LOG(INFO) << "[PhiStore] imported " << base::WideToUTF8(name) << " -> "
              << base::WideToUTF8(name) << ".imported";
  } else {
    LOG(ERROR) << "[PhiStore] imported " << base::WideToUTF8(name)
               << " but could not rename it, err=" << ::GetLastError();
  }
}

// --- Settings ---------------------------------------------------------------

// PhiPreferences.json — the Mac's UserDefaults.standard keys and types
// (docs/user-data-parity.md §4.4). Windows-only keys keep Windows names:
// settingsWindowWidth/Height, saturation, restoreLastSession, testRig,
// skipOnboarding, analyticsEnabled.
void DecodePreferences(const base::DictValue& dict, PhiSettings* out) {
  if (const std::string* name = dict.FindString("layoutMode")) {
    PhiLayoutMode parsed = PhiLayoutMode::kPerformance;
    if (PhiLayoutModeFromName(*name, &parsed)) {
      out->layout_mode = parsed;
    }
  }
  if (std::optional<int> width = dict.FindInt("settingsWindowWidth")) {
    out->settings_window_width = std::max(0, *width);
  }
  if (std::optional<int> height = dict.FindInt("settingsWindowHeight")) {
    out->settings_window_height = std::max(0, *height);
  }
  // The Mac's "default" names Pure; unknown ids normalise to Pure too.
  if (const std::string* id = dict.FindString("PhiCurrentThemeId")) {
    out->theme_id = PhiThemeById(*id == "default" ? "pure" : *id).id;
  }
  // UserAppearanceChoice: 0 system, 1 light, 2 dark.
  if (std::optional<int> choice = dict.FindInt("PhiUserAppearanceChoice")) {
    out->appearance = *choice == 1   ? model::Appearance::kLight
                      : *choice == 2 ? model::Appearance::kDark
                                     : model::Appearance::kSystem;
  }
  out->saturation = dict.FindInt("saturation").value_or(out->saturation);
  if (std::optional<bool> ntp = dict.FindBool("openNewTabPageOnCmdT")) {
    out->new_tab_behavior = *ntp ? model::NewTabBehavior::kNewTabPage
                                 : model::NewTabBehavior::kOmnibox;
  }
  out->always_show_full_url =
      dict.FindBool("alwaysShowURLPath").value_or(out->always_show_full_url);
  out->show_tab_previews =
      dict.FindBool("showTabPreviews").value_or(out->show_tab_previews);
  out->show_open_tab_indicators = dict.FindBool("showOpenTabIndicators")
                                      .value_or(out->show_open_tab_indicators);
  out->dim_unloaded_tab_icons = dict.FindBool("dimUnloadedTabIcons")
                                    .value_or(out->dim_unloaded_tab_icons);
  out->sidebar_transparency = std::clamp(
      dict.FindInt("sidebarTransparency").value_or(out->sidebar_transparency),
      0, 100);
  out->agent_auto_view =
      dict.FindBool("PhiAgentSpaceAutoView").value_or(out->agent_auto_view);
  out->peek_view_enabled =
      dict.FindBool("peekViewEnabled").value_or(out->peek_view_enabled);
  out->auto_peek_view_enabled = dict.FindBool("autoPeekViewEnabled")
                                    .value_or(out->auto_peek_view_enabled);
  out->open_kiosk_on_ctrl_alt_click =
      dict.FindBool("openKioskOnCommandOptionClick")
          .value_or(out->open_kiosk_on_ctrl_alt_click);
  if (const std::string* folder = dict.FindString("PhiSaveForLaterFolder")) {
    out->folio_folder = *folder;
  }
  if (const base::DictValue* folders =
          dict.FindDict("PhiSaveForLaterProfileFolders")) {
    for (const auto [profile, path] : *folders) {
      if (path.is_string() && !profile.empty() && !path.GetString().empty()) {
        out->folio_profile_folders[profile] = path.GetString();
      }
    }
  }
  out->folio_auto_save_on_site_actions =
      dict.FindBool("PhiSaveForLaterAutoSaveOnSiteActions")
          .value_or(out->folio_auto_save_on_site_actions);
  out->restore_last_session =
      dict.FindBool("restoreLastSession").value_or(out->restore_last_session);
  out->always_show_bookmark_bar = dict.FindBool("alwaysShowBookmarkBar")
                                      .value_or(out->always_show_bookmark_bar);
  out->bookmark_bar_on_new_tab = dict.FindBool("showBookmarkBarOnNewTabPage")
                                     .value_or(out->bookmark_bar_on_new_tab);
  out->developer_mode =
      dict.FindBool("PhiDeveloperModeEnabled").value_or(out->developer_mode);
  out->allow_agent_control = dict.FindBool("PhiCDPAgentAccessEnabled")
                                 .value_or(out->allow_agent_control);
  out->allow_agent_user_spaces =
      dict.FindBool("PhiAgentUserSpaceOperationsEnabled")
          .value_or(out->allow_agent_user_spaces);
  if (const base::ListValue* blocked =
          dict.FindList("PhiAgentDisallowedProfileIds")) {
    out->disallowed_agent_profile_ids.clear();
    for (const base::Value& entry : *blocked) {
      if (const std::string* id = entry.GetIfString()) {
        out->disallowed_agent_profile_ids.push_back(*id);
      }
    }
  }
  out->test_rig = dict.FindBool("testRig").value_or(out->test_rig);
  // appLanguagePreference: "system" (the model's "") or a catalog tag.
  if (const std::string* language = dict.FindString("appLanguagePreference")) {
    out->language = *language == "system" ? std::string() : *language;
  }
  // The Mac's PhiReader* keys, through PhiReaderStyle so the range checks
  // live in one place.
  {
    base::DictValue reader;
    const auto copy_int = [&](const char* from, const char* to) {
      if (std::optional<int> v = dict.FindInt(from)) {
        reader.Set(to, *v);
      }
    };
    const auto copy_string = [&](const char* from, const char* to) {
      if (const std::string* v = dict.FindString(from)) {
        reader.Set(to, *v);
      }
    };
    const auto copy_bool = [&](const char* from, const char* to) {
      if (std::optional<bool> v = dict.FindBool(from)) {
        reader.Set(to, *v);
      }
    };
    copy_int("PhiReaderFontSize", "fontSize");
    copy_string("PhiReaderWidth", "width");
    copy_string("PhiReaderTheme", "theme");
    copy_string("PhiReaderTypeface", "typeface");
    copy_string("PhiReaderLineHeight", "lineHeight");
    copy_string("PhiReaderLetterSpacing", "letterSpacing");
    copy_bool("PhiReaderShowsImages", "showsImages");
    copy_bool("PhiReaderHighlightsLinks", "highlightsLinks");
    PhiReaderStyle style;
    style.font_size = out->reader.font_size;
    style.width = out->reader.width;
    style.theme = out->reader.theme;
    style.typeface = out->reader.typeface;
    style.line_height = out->reader.line_height;
    style.letter_spacing = out->reader.letter_spacing;
    style.shows_images = out->reader.shows_images;
    style.highlights_links = out->reader.highlights_links;
    style.MergeFrom(reader);
    out->reader.font_size = style.font_size;
    out->reader.width = style.width;
    out->reader.theme = style.theme;
    out->reader.typeface = style.typeface;
    out->reader.line_height = style.line_height;
    out->reader.letter_spacing = style.letter_spacing;
    out->reader.shows_images = style.shows_images;
    out->reader.highlights_links = style.highlights_links;
  }
  if (const std::string* scope = dict.FindString("PhiPinnedTabScope")) {
    PhiPinScopeFromName(*scope, &out->pin_scope);
  }
  out->analytics_enabled =
      dict.FindBool("analyticsEnabled").value_or(out->analytics_enabled);
  out->guest_mode_enabled =
      dict.FindBool("guestModeEnabled").value_or(out->guest_mode_enabled);
  out->login_phase = dict.FindInt("phiLoginPhase").value_or(out->login_phase);
  out->skip_onboarding =
      dict.FindBool("skipOnboarding").value_or(out->skip_onboarding);
  out->suppress_close_incognito_space_warning =
      dict.FindBool("suppressCloseIncognitoSpaceWarning")
          .value_or(out->suppress_close_incognito_space_warning);
  if (const std::string* mode = dict.FindString("autoPictureInPictureMode")) {
    if (*mode == "off" || *mode == "normal" || *mode == "parked") {
      out->auto_picture_in_picture_mode = *mode;
    }
  }
  out->selection_tint_enabled = dict.FindBool("PhiSelectionTintEnabled")
                                    .value_or(out->selection_tint_enabled);
}

// --- Restore ----------------------------------------------------------------

// {spaceId, profile, windowId} rows — the ghost store's shape, kept for the
// new file's "remembered" list so the two read alike.
void DecodeRememberedList(const base::ListValue& list,
                          std::vector<model::RememberedWindow>* out) {
  for (const base::Value& v : list) {
    const base::DictValue* d = v.GetIfDict();
    if (!d) {
      continue;
    }
    const std::string* space = d->FindString("spaceId");
    const std::string* profile = d->FindString("profile");
    const std::optional<int> window_id = d->FindInt("windowId");
    if (!space || space->empty() || !profile || !window_id) {
      continue;
    }
    out->push_back({*space, *profile, *window_id});
  }
}

// Which legacy prefs file still holds lastActiveSpaceId: the live one if
// the settings store has not imported it yet, else its .imported copy.
// Neither is renamed here — that is the settings store's move.
std::optional<std::string> LegacyLastActiveSpaceId(const std::wstring& dir) {
  for (const std::wstring& name :
       {std::wstring(kLegacyPrefs), std::wstring(kLegacyPrefs) + L".imported"}) {
    base::DictValue prefs;
    if (PhiReadJsonFile(Join(dir, name.c_str()), &prefs) !=
        PhiJsonFileStatus::kOk) {
      continue;
    }
    if (const std::string* last = prefs.FindString("lastActiveSpaceId")) {
      return *last;
    }
    return std::nullopt;  // The file is there but never stored one.
  }
  return std::nullopt;
}

// --- Consent ----------------------------------------------------------------

void DecodeAudit(const base::ListValue& list, std::vector<std::string>* out) {
  for (const base::Value& v : list) {
    if (v.is_string()) {
      out->push_back(v.GetString());
    }
  }
}

}  // namespace

// =============================================================================
// PhiPreferences.json (schema 1)
// =============================================================================

// static
base::DictValue PhiStoreTraits<PhiSettings>::Encode(const PhiSettings& value) {
  base::DictValue root;
  root.Set("layoutMode", PhiLayoutModeName(value.layout_mode));
  if (value.settings_window_width > 0 && value.settings_window_height > 0) {
    root.Set("settingsWindowWidth", value.settings_window_width);
    root.Set("settingsWindowHeight", value.settings_window_height);
  }
  root.Set("PhiCurrentThemeId", value.theme_id);
  root.Set("PhiUserAppearanceChoice",
           value.appearance == model::Appearance::kLight  ? 1
           : value.appearance == model::Appearance::kDark ? 2
                                                          : 0);
  root.Set("saturation", value.saturation);
  root.Set("openNewTabPageOnCmdT",
           value.new_tab_behavior == model::NewTabBehavior::kNewTabPage);
  root.Set("alwaysShowURLPath", value.always_show_full_url);
  root.Set("showTabPreviews", value.show_tab_previews);
  root.Set("showOpenTabIndicators", value.show_open_tab_indicators);
  root.Set("dimUnloadedTabIcons", value.dim_unloaded_tab_icons);
  root.Set("sidebarTransparency", value.sidebar_transparency);
  root.Set("PhiAgentSpaceAutoView", value.agent_auto_view);
  root.Set("peekViewEnabled", value.peek_view_enabled);
  root.Set("autoPeekViewEnabled", value.auto_peek_view_enabled);
  root.Set("openKioskOnCommandOptionClick", value.open_kiosk_on_ctrl_alt_click);
  if (!value.folio_folder.empty()) {
    root.Set("PhiSaveForLaterFolder", value.folio_folder);
  }
  if (!value.folio_profile_folders.empty()) {
    base::DictValue folders;
    for (const auto& [profile, path] : value.folio_profile_folders) {
      folders.Set(profile, path);
    }
    root.Set("PhiSaveForLaterProfileFolders", std::move(folders));
  }
  root.Set("PhiSaveForLaterAutoSaveOnSiteActions",
           value.folio_auto_save_on_site_actions);
  root.Set("restoreLastSession", value.restore_last_session);
  root.Set("alwaysShowBookmarkBar", value.always_show_bookmark_bar);
  root.Set("showBookmarkBarOnNewTabPage", value.bookmark_bar_on_new_tab);
  root.Set("PhiReaderFontSize", value.reader.font_size);
  root.Set("PhiReaderWidth", value.reader.width);
  root.Set("PhiReaderTheme", value.reader.theme);
  root.Set("PhiReaderTypeface", value.reader.typeface);
  root.Set("PhiReaderLineHeight", value.reader.line_height);
  root.Set("PhiReaderLetterSpacing", value.reader.letter_spacing);
  root.Set("PhiReaderShowsImages", value.reader.shows_images);
  root.Set("PhiReaderHighlightsLinks", value.reader.highlights_links);
  root.Set("appLanguagePreference",
           value.language.empty() ? std::string("system") : value.language);
  root.Set("PhiDeveloperModeEnabled", value.developer_mode);
  root.Set("PhiCDPAgentAccessEnabled", value.allow_agent_control);
  root.Set("PhiAgentUserSpaceOperationsEnabled",
           value.allow_agent_user_spaces);
  base::ListValue blocked;
  for (const std::string& id : value.disallowed_agent_profile_ids) {
    blocked.Append(id);
  }
  root.Set("PhiAgentDisallowedProfileIds", std::move(blocked));
  root.Set("testRig", value.test_rig);
  root.Set("PhiPinnedTabScope", PhiPinScopeName(value.pin_scope));
  root.Set("analyticsEnabled", value.analytics_enabled);
  root.Set("guestModeEnabled", value.guest_mode_enabled);
  root.Set("phiLoginPhase", value.login_phase);
  root.Set("skipOnboarding", value.skip_onboarding);
  root.Set("suppressCloseIncognitoSpaceWarning",
           value.suppress_close_incognito_space_warning);
  root.Set("autoPictureInPictureMode", value.auto_picture_in_picture_mode);
  root.Set("PhiSelectionTintEnabled", value.selection_tint_enabled);
  return root;
}

// static
bool PhiStoreTraits<PhiSettings>::Decode(const base::DictValue& dict,
                                         int /*schema*/,
                                         PhiSettings* out) {
  // Field-tolerant: a hand-edited or partial file keeps the defaults for
  // what it does not name, so there is no shape that fails to decode.
  DecodePreferences(dict, out);
  return true;
}

// static
bool PhiStoreTraits<PhiSettings>::Import(const std::wstring& /*dir*/,
                                         PhiSettings* /*out*/) {
  // phibrowser-win is unreleased: the old PhiSettings.json (and the
  // pre-phase-3 files before it) are not carried over
  // (docs/user-data-parity.md).
  return false;
}

// =============================================================================
// PhiRestore.json (schema 1)
// =============================================================================

// static
base::DictValue PhiStoreTraits<PhiRestoreModel>::Encode(
    const PhiRestoreModel& value) {
  base::DictValue root;
  base::ListValue remembered;
  for (const model::RememberedWindow& row : value.remembered) {
    base::DictValue d;
    d.Set("spaceId", row.space_id);
    d.Set("profile", row.profile);
    // SessionIDs are 32-bit on the framework side; the ghost store wrote
    // them as int too.
    d.Set("windowId", static_cast<int>(row.window_id));
    remembered.Append(std::move(d));
  }
  root.Set("remembered", std::move(remembered));
  base::DictValue parked;
  for (const auto& [profile, ids] : value.parked) {
    base::ListValue list;
    for (int64_t id : ids) {
      list.Append(static_cast<int>(id));
    }
    parked.Set(profile, std::move(list));
  }
  root.Set("parked", std::move(parked));
  root.Set("lastActiveSpaceId", value.last_active_space_id);
  return root;
}

// static
bool PhiStoreTraits<PhiRestoreModel>::Decode(const base::DictValue& dict,
                                             int /*schema*/,
                                             PhiRestoreModel* out) {
  if (const base::ListValue* list = dict.FindList("remembered")) {
    DecodeRememberedList(*list, &out->remembered);
  }
  if (const base::DictValue* parked = dict.FindDict("parked")) {
    for (const auto [profile, value] : *parked) {
      const base::ListValue* ids = value.GetIfList();
      if (!ids) {
        continue;
      }
      std::vector<int64_t>& rows = out->parked[profile];
      for (const base::Value& id : *ids) {
        if (id.is_int() && id.GetInt() > 0) {
          rows.push_back(id.GetInt());
        }
      }
    }
  }
  if (const std::string* last = dict.FindString("lastActiveSpaceId")) {
    out->last_active_space_id = *last;
  }
  return true;
}

// static
bool PhiStoreTraits<PhiRestoreModel>::Import(const std::wstring& dir,
                                             PhiRestoreModel* out) {
  bool imported = false;
  base::DictValue dict;
  if (ReadLegacy(dir, kLegacyGhosts, &dict) == PhiJsonFileStatus::kOk) {
    if (const base::ListValue* rows = dict.FindList("spaces")) {
      DecodeRememberedList(*rows, &out->remembered);
    }
    MarkImported(dir, kLegacyGhosts);
    imported = true;
  }
  if (std::optional<std::string> last = LegacyLastActiveSpaceId(dir)) {
    out->last_active_space_id = *last;
    LOG(INFO) << "[PhiStore] imported lastActiveSpaceId from "
              << base::WideToUTF8(kLegacyPrefs);
    imported = true;
  }
  return imported;
}

// =============================================================================
// PhiConsent.json (schema 1, sealed)
// =============================================================================

// static
base::DictValue PhiStoreTraits<PhiConsentModel>::Encode(
    const PhiConsentModel& value) {
  base::DictValue root;
  base::ListValue audit;
  for (const std::string& line : value.credential_audit) {
    audit.Append(line);
  }
  root.Set("credentialAudit", std::move(audit));
  return root;
}

// static
bool PhiStoreTraits<PhiConsentModel>::Decode(const base::DictValue& dict,
                                             int /*schema*/,
                                             PhiConsentModel* out) {
  // "agentAlways" (per-image agent grants) and "credentialGrants" (standing
  // credential approvals) are ignored: neither kind of standing grant
  // exists any more — agent admission is the allowAgentControl setting.
  if (const base::ListValue* audit = dict.FindList("credentialAudit")) {
    DecodeAudit(*audit, &out->credential_audit);
  }
  return true;
}

// static
bool PhiStoreTraits<PhiConsentModel>::Import(const std::wstring& dir,
                                             PhiConsentModel* out) {
  bool imported = false;
  base::DictValue dict;
  if (ReadLegacy(dir, kLegacyCredentialGrants, &dict) ==
      PhiJsonFileStatus::kOk) {
    if (const base::ListValue* audit = dict.FindList("audit")) {
      DecodeAudit(*audit, &out->credential_audit);
    }
    MarkImported(dir, kLegacyCredentialGrants);
    imported = true;
  }
  return imported;
}

// Compile every member of PhiStore for the four files here, so a codec or
// seal bug surfaces in this TU's build rather than in whichever TU first
// touches that store.
template class PhiStore<PhiSettings>;
template class PhiStore<PhiRestoreModel>;
template class PhiStore<PhiConsentModel>;

}  // namespace phi_client

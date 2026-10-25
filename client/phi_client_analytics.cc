// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_client_analytics.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "phibrowser-win/client/phi_agent_icons.h"
#include "phibrowser-win/client/phi_file_io.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_shell_layout.h"
#include "phibrowser-win/client/phi_version.h"

namespace phi_client {

base::DictValue PhiAppOpenedProperties(const model::PhiSettings& settings) {
  base::DictValue props;
  props.Set("from_background", false);
  props.Set("version", PHI_WIN_VERSION_FULL_STRING);
  props.Set("layout_mode", PhiLayoutModeName(settings.layout_mode));
  // The Mac's beforeSend enrichment. AI features do not exist in this
  // client yet, so that one is reported false rather than omitted: an
  // insight that filters on it keeps counting Windows. Guest Mode is the
  // onboarding's "Explore Phi without signing in" (the Mac's
  // ApplicationState.isGuest).
  props.Set("ai_enabled", false);
  props.Set("is_guest_mode", settings.guest_mode_enabled);
  return props;
}

base::DictValue PhiFlagProperties(const char* key, bool value) {
  base::DictValue props;
  props.Set(key, value);
  return props;
}

base::DictValue PhiUserDefaultsSnapshotProperties(
    const model::PhiSettings& settings,
    bool is_default_browser) {
  base::DictValue props;
  props.Set("new_tab_behavior",
            settings.new_tab_behavior == model::NewTabBehavior::kOmnibox
                ? "omnibox"
                : "ntp");
  props.Set("layout_mode", PhiLayoutModeName(settings.layout_mode));
  // The locale the process is running in, which is the stored preference
  // or, when that is empty, the system's — the Mac's
  // activeProcessAppLanguage.
  props.Set("app_language", PhiActiveLocale());
  // The user's CHOICE (the Mac's UserAppearanceChoice), not the resolved
  // appearance; the model already speaks the Mac's wire names.
  props.Set("appearance", model::AppearanceName(settings.appearance));
  props.Set("is_phi_default_browser", is_default_browser);
  props.Set("peek_view_enabled", settings.peek_view_enabled);
  props.Set("always_show_full_url", settings.always_show_full_url);
  props.Set("show_tab_previews", settings.show_tab_previews);
  props.Set("show_open_tab_indicators", settings.show_open_tab_indicators);
  props.Set("dim_unloaded_tab_icons", settings.dim_unloaded_tab_icons);
  props.Set("restore_last_session_enabled", settings.restore_last_session);
  props.Set("developer_mode_enabled", settings.developer_mode);
  // The Mac's cdpAgentAccessEnabled is the same switch as Developer ▸
  // "Allow agents to control Phi".
  props.Set("cdp_agent_access_enabled", settings.allow_agent_control);
  // The Mac's key for its Cmd+Option click; the Windows chord is Ctrl+Alt.
  props.Set("open_kiosk_on_command_option_click",
            settings.open_kiosk_on_ctrl_alt_click);
  // The Mac's AutoPictureInPictureMode raw value: off / normal / parked.
  props.Set("auto_picture_in_picture_mode",
            settings.auto_picture_in_picture_mode);
  // Windows-only switches, named for what they are.
  props.Set("agent_user_spaces_enabled", settings.allow_agent_user_spaces);
  props.Set("always_show_bookmark_bar", settings.always_show_bookmark_bar);
  return props;
}

PhiVersionTransition PhiNoteAppVersion(const std::wstring& user_data_dir) {
  PhiVersionTransition out;
  if (user_data_dir.empty()) {
    return out;
  }
  const std::wstring path = user_data_dir + L"\\phi_last_version";
  std::string previous;
  if (PhiReadFile(path, &previous) == PhiReadStatus::kError) {
    return out;  // locked: never "installed", never written over
  }
  while (!previous.empty() &&
         (previous.back() == '\n' || previous.back() == '\r' ||
          previous.back() == ' ')) {
    previous.pop_back();
  }
  const std::string current = PHI_WIN_VERSION_FULL_STRING;
  if (previous == current) {
    return out;  // The common case: nothing to say.
  }
  // Recorded BEFORE the event is built, so a launch that dies between here
  // and the upload reports the transition once, not on every launch until
  // one survives. The event itself is best-effort, as all of them are.
  PhiWriteFileAtomic(path, current);
  out.properties.Set("version", current);
  out.properties.Set("build", PHI_WIN_VERSION_BUILD);
  if (previous.empty()) {
    out.event = kEventAppInstalled;
  } else {
    out.event = kEventAppUpdated;
    out.properties.Set("previous_version", previous);
  }
  return out;
}

base::DictValue PhiSpaceCreatedProperties(int total_spaces,
                                          bool non_default_profile,
                                          const std::string& surface) {
  base::DictValue props;
  props.Set("total_spaces", total_spaces);
  props.Set("non_default_profile", non_default_profile);
  props.Set("surface", surface);
  return props;
}

base::DictValue PhiSpaceSwitchedProperties(int total_spaces) {
  base::DictValue props;
  props.Set("total_spaces", total_spaces);
  return props;
}

std::string PhiAgentTelemetryName(const std::string& agent_name) {
  bool blank = true;
  for (const char c : agent_name) {
    if (c != ' ' && c != '\t') {
      blank = false;
      break;
    }
  }
  if (blank) {
    return "unknown";
  }
  // The badge resolver is the one place a name is matched to a brand; a
  // name it gives no mark to is "other", never the name itself.
  const PhiAgentDriverBadge badge = PhiAgentDriverBadgeFor(agent_name);
  return badge.icon.empty() ? std::string("other") : badge.label;
}

bool PhiAgentUserSpaceCommandIsTracked(std::string_view verb) {
  static constexpr std::string_view kUntracked[] = {
      "agentSpace.bookmarks.list", "agentSpace.pinnedTabs.list",
      "agentSpace.shadow.list",    "agentSpace.shadow.ping",
      "agentSpace.spaces.list",    "agentSpace.spaces.listTabs",
      "agentSpace.urlRules.list",  "credentials.status",
  };
  for (const std::string_view untracked : kUntracked) {
    if (verb == untracked) {
      return false;
    }
  }
  return true;
}

base::DictValue PhiAgentTaskProperties(const std::string& origin,
                                       bool persistent,
                                       const std::string& agent_name) {
  base::DictValue props;
  props.Set("origin", origin);
  props.Set("persistent", persistent);
  props.Set("agent_name", PhiAgentTelemetryName(agent_name));
  return props;
}

base::DictValue PhiAgentTaskCompletedProperties(const std::string& origin,
                                                bool persistent,
                                                const std::string& agent_name,
                                                bool success) {
  base::DictValue props = PhiAgentTaskProperties(origin, persistent, agent_name);
  props.Set("success", success);
  return props;
}

base::DictValue PhiAgentUserSpaceCommandProperties(
    const std::string& command,
    const std::string& agent_name) {
  base::DictValue props;
  props.Set("command", command);
  props.Set("agent_name", PhiAgentTelemetryName(agent_name));
  return props;
}

base::DictValue PhiAgentShadowWindowProperties(const std::string& origin,
                                               bool incognito) {
  base::DictValue props;
  props.Set("origin", origin);
  props.Set("incognito", incognito);
  return props;
}

base::DictValue PhiCredentialRequestedProperties(
    const char* kind,
    bool approved,
    bool prompted,
    const std::string& agent_name) {
  base::DictValue props;
  props.Set("kind", kind);
  props.Set("approved", approved);
  props.Set("prompted", prompted);
  props.Set("agent_name", PhiAgentTelemetryName(agent_name));
  return props;
}

base::DictValue PhiCredentialApprovedProperties(const char* kind,
                                                const char* approval_type,
                                                const std::string& agent_name) {
  base::DictValue props;
  props.Set("kind", kind);
  props.Set("approval_type", approval_type);
  props.Set("agent_name", PhiAgentTelemetryName(agent_name));
  return props;
}

base::DictValue PhiProfileCountProperties(int total_profiles) {
  base::DictValue props;
  props.Set("total_profiles", total_profiles);
  return props;
}

base::DictValue PhiPeekOpenedProperties(const char* entry_point,
                                        const char* opener_kind) {
  base::DictValue props;
  props.Set("entry_point", entry_point);
  props.Set("opener_kind", opener_kind);
  return props;
}

base::DictValue PhiPeekEndedProperties(const char* reason,
                                       int duration_seconds) {
  base::DictValue props;
  props.Set("reason", reason);
  props.Set("duration_seconds", duration_seconds);
  return props;
}

base::DictValue PhiReaderOpenedProperties(const char* entry_point) {
  base::DictValue props;
  props.Set("entry_point", entry_point);
  return props;
}

base::DictValue PhiReaderOpenFailedProperties(const char* entry_point,
                                              const std::string& reason) {
  base::DictValue props = PhiReaderOpenedProperties(entry_point);
  props.Set("reason", reason);
  return props;
}

base::DictValue PhiOobeEventProperties(
    const PhiOobeAnalyticsSession::Event& event) {
  base::DictValue props;
  const std::string_view name = event.name ? event.name : "";
  if (name == "oobe_finished") {
    props.Set("is_guest", event.is_guest);
    props.Set("steps_completed", event.steps_completed);
    props.Set("total_duration_seconds", event.total_duration_seconds);
    return props;
  }
  if (name == "oobe_interrupted") {
    props.Set("last_step", event.step);
    props.Set("step_index", event.step_index);
    props.Set("is_guest", event.is_guest);
    props.Set("total_duration_seconds", event.total_duration_seconds);
    props.Set("reason", event.reason);
    return props;
  }
  props.Set("step", event.step);
  props.Set("step_index", event.step_index);
  props.Set("is_guest", event.is_guest);
  if (name == "oobe_step_completed") {
    props.Set("duration_seconds", event.duration_seconds);
  }
  return props;
}

base::DictValue PhiGuestModeExitedProperties(const char* trigger) {
  base::DictValue props;
  props.Set("trigger", trigger);
  return props;
}

base::DictValue PhiLanguageChangedProperties(const std::string& from,
                                             const std::string& to) {
  base::DictValue props;
  props.Set("from", from.empty() ? std::string("system") : from);
  props.Set("to", to.empty() ? std::string("system") : to);
  return props;
}

base::DictValue PhiFeatureEntryProperties(const char* button,
                                          const char* surface) {
  base::DictValue props;
  props.Set("button", button);
  props.Set("surface", surface);
  return props;
}

base::DictValue PhiNotificationCardActionProperties(const char* action) {
  base::DictValue props;
  props.Set("action", action);
  return props;
}

base::DictValue PhiImportViewedProperties() {
  base::DictValue props;
  props.Set("entry_point", "menu");
  return props;
}

namespace {

base::ListValue SortedUnique(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  base::ListValue list;
  for (std::string& value : values) {
    list.Append(std::move(value));
  }
  return list;
}

}  // namespace

base::DictValue PhiImportTypesSelectedProperties(
    const std::string& source,
    std::vector<std::string> types) {
  base::DictValue props;
  props.Set("source_browser", source);
  props.Set("types", SortedUnique(std::move(types)));
  return props;
}

base::DictValue PhiImportStartedProperties(std::vector<std::string> sources) {
  base::DictValue props;
  props.Set("source_browsers", SortedUnique(std::move(sources)));
  return props;
}

base::DictValue PhiImportFinishedProperties(std::vector<std::string> sources,
                                            std::vector<std::string> failed,
                                            int duration_seconds,
                                            const std::string& error_code) {
  base::DictValue props = PhiImportStartedProperties(std::move(sources));
  props.Set("success", failed.empty());
  props.Set("failed_sources", SortedUnique(std::move(failed)));
  props.Set("duration_seconds", std::max(0, duration_seconds));
  if (!error_code.empty()) {
    props.Set("error_code", error_code);
  }
  return props;
}

PhiFirstTimeActions::PhiFirstTimeActions() = default;
PhiFirstTimeActions::~PhiFirstTimeActions() = default;

void PhiFirstTimeActions::Load(const std::wstring& user_data_dir,
                               int64_t now_unix_ms) {
  if (user_data_dir.empty()) {
    return;
  }
  path_ = user_data_dir + L"\\phi_first_time_actions";
  std::string bytes;
  recorded_.clear();
  installed_unix_ms_ = 0;
  if (PhiReadFile(path_, &bytes) == PhiReadStatus::kError) {
    // Locked: nothing is recorded (or written over) this session.
    path_.clear();
    return;
  }
  for (std::string_view line : base::SplitStringPiece(
           bytes, "\r\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (installed_unix_ms_ == 0) {
      // The first line is the install moment; anything unparsable there
      // means a record this build does not understand — start over.
      if (!base::StringToInt64(line, &installed_unix_ms_) ||
          installed_unix_ms_ <= 0) {
        break;
      }
      continue;
    }
    recorded_.emplace_back(line);
  }
  if (installed_unix_ms_ <= 0) {
    // A profile from before this record existed counts from now, as the
    // Mac's launchContext.firstLaunchDate does on its first run.
    installed_unix_ms_ = now_unix_ms;
    recorded_.clear();
    PhiWriteFileAtomic(path_, base::NumberToString(installed_unix_ms_) + "\n");
  }
}

bool PhiFirstTimeActions::Record(const char* action,
                                 int64_t now_unix_ms,
                                 base::DictValue* properties) {
  if (path_.empty() || !action || !*action ||
      std::find(recorded_.begin(), recorded_.end(), action) !=
          recorded_.end()) {
    return false;
  }
  recorded_.emplace_back(action);
  std::string bytes = base::NumberToString(installed_unix_ms_) + "\n";
  for (const std::string& recorded : recorded_) {
    bytes += recorded + "\n";
  }
  // Recorded BEFORE the event is built, as the version file is: a launch
  // that dies between here and the upload reports the action once.
  PhiWriteFileAtomic(path_, bytes);
  properties->Set("action", action);
  properties->Set(
      "seconds_since_install",
      static_cast<int>(std::max<int64_t>(
          0, (now_unix_ms - installed_unix_ms_) / 1000)));
  return true;
}

}  // namespace phi_client

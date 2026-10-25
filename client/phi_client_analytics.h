// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_CLIENT_ANALYTICS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_CLIENT_ANALYTICS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"
#include "phibrowser-win/client/phi_app_model.h"
#include "phibrowser-win/client/phi_oobe_analytics.h"

namespace phi_client {

// The CLIENT-originated half of the product-analytics namespace — the events
// the Mac app captures from its own shell code (docs/analytics.md there),
// carried here under the SAME names and property keys so one PostHog insight
// spans both platforms. The Chromium-originated half (memory_saver_state,
// renderer_health_*) arrives through the framework delegate and is
// registered in chrome/browser/phinomenon/analytics/README.md.
//
// This file only BUILDS property sets; nothing here captures. Every event
// goes out through PhiCoordinator::CaptureAnalytics, which is the client's
// one capture path (the harness ring, then PhiTelemetry's consent gate and
// allow-list). Adding an event is three edits: a builder here, its capture
// site, and the module's row in PhiTelemetry::AllowedProperties — the
// allow-list is the privacy review's reading list, so a property that is
// not fenced there is dropped at egress, however it was built.
//
// The registry of these events is docs/analytics.md in this repository.

// Module tags — the rows in PhiTelemetry::AllowedProperties.
inline constexpr char kAnalyticsModuleApp[] = "app";
inline constexpr char kAnalyticsModuleSettings[] = "settings";
inline constexpr char kAnalyticsModuleSpaces[] = "spaces";
inline constexpr char kAnalyticsModuleAgent[] = "agent";
inline constexpr char kAnalyticsModulePeek[] = "peek";
inline constexpr char kAnalyticsModuleReader[] = "reader";
inline constexpr char kAnalyticsModuleBookmarks[] = "bookmarks";
inline constexpr char kAnalyticsModuleKiosk[] = "kiosk";
inline constexpr char kAnalyticsModuleProfiles[] = "profiles";
inline constexpr char kAnalyticsModuleOobe[] = "oobe";
inline constexpr char kAnalyticsModuleAccount[] = "account";
inline constexpr char kAnalyticsModuleImport[] = "import";
inline constexpr char kAnalyticsModuleFeature[] = "feature";
inline constexpr char kAnalyticsModuleNotifications[] = "notifications";

// Event names. The four capitalised ones are what the Mac PostHog SDK
// auto-captures for the app lifecycle; matching them verbatim is what makes
// the Mac's DAU and retention insights count Windows sessions too.
inline constexpr char kEventAppOpened[] = "Application Opened";
inline constexpr char kEventAppInstalled[] = "Application Installed";
inline constexpr char kEventAppUpdated[] = "Application Updated";
inline constexpr char kEventUserDefaultsSnapshot[] = "user_defaults_snapshot";
inline constexpr char kEventSpaceCreated[] = "space_created";
inline constexpr char kEventSpaceSwitched[] = "space_switched";
inline constexpr char kEventAgentTaskStarted[] = "agent_task_started";
inline constexpr char kEventAgentTaskCompleted[] = "agent_task_completed";
inline constexpr char kEventAgentUserSpaceCommand[] =
    "agent_user_space_command";
inline constexpr char kEventAgentShadowWindowOpened[] =
    "agent_shadow_window_opened";
inline constexpr char kEventAgentCredentialRequested[] =
    "agent_credential_access_requested";
inline constexpr char kEventAgentCredentialApproved[] =
    "agent_credential_access_approved";
inline constexpr char kEventSpaceDeleted[] = "space_deleted";
inline constexpr char kEventSpaceProfileChanged[] = "space_profile_changed";
inline constexpr char kEventProfileCreated[] = "profile_created";
inline constexpr char kEventPeekOpened[] = "peek_view_opened";
inline constexpr char kEventPeekEnded[] = "peek_view_ended";
inline constexpr char kEventPeekSettingChanged[] = "peek_view_setting_changed";
inline constexpr char kEventPeekAutoSettingChanged[] =
    "peek_view_auto_setting_changed";
inline constexpr char kEventReaderOpened[] = "reader_view_opened";
inline constexpr char kEventReaderOpenFailed[] = "reader_view_open_failed";
inline constexpr char kEventBookmarkManagerOpened[] = "bookmark_manager_opened";
inline constexpr char kEventBookmarkManagerEdited[] = "bookmark_manager_edited";
inline constexpr char kEventKioskOpened[] = "kiosk_opened";
inline constexpr char kEventKioskOpenedInSpace[] = "kiosk_opened_in_space";
inline constexpr char kEventOobeStepViewed[] = "oobe_step_viewed";
inline constexpr char kEventOobeStepCompleted[] = "oobe_step_completed";
inline constexpr char kEventOobeFinished[] = "oobe_finished";
inline constexpr char kEventOobeInterrupted[] = "oobe_interrupted";
inline constexpr char kEventGuestModeEntered[] = "guest_mode_entered";
inline constexpr char kEventGuestModeExited[] = "guest_mode_exited";
inline constexpr char kEventUserLoggedIn[] = "user_logged_in";
inline constexpr char kEventUserLoggedOut[] = "user_logged_out";
inline constexpr char kEventLoginRetried[] = "login_retried";
inline constexpr char kEventMetricsReportingChanged[] =
    "metrics_reporting_changed";
inline constexpr char kEventLanguageChanged[] = "language_changed";
inline constexpr char kEventFeatureEntryTapped[] = "feature_entry_tapped";
inline constexpr char kEventImportViewed[] = "import_viewed";
inline constexpr char kEventImportTypesSelected[] = "import_types_selected";
inline constexpr char kEventImportStarted[] = "import_started";
inline constexpr char kEventImportFinished[] = "import_finished";
inline constexpr char kEventFirstTimeAction[] = "first_time_action";
inline constexpr char kEventNotificationCardAction[] =
    "notification_card_action";

// "Application Opened": once per launch, in the shape the Mac SDK's
// fresh-launch lifecycle event had (`from_background` false, the version)
// plus the enrichment AppController's beforeSend hook adds (layout_mode,
// ai_enabled, is_guest_mode). Never on a return to the foreground: the Mac
// (e748d9f4) turned the SDK's didBecomeActive / didResignActive pair off —
// every Cmd-Tab was an Opened / Backgrounded pair, ~35 a user a day — and
// captures the one launch event by hand; no Backgrounded event exists.
base::DictValue PhiAppOpenedProperties(const model::PhiSettings& settings);

// A single boolean property, for the events that carry exactly one
// ("enabled", "incognito", "success", …).
base::DictValue PhiFlagProperties(const char* key, bool value);

// "user_defaults_snapshot": the launch-time state of the settings the Mac
// snapshot reports, under the Mac's keys, for the ones that exist on
// Windows. `is_default_browser` is the caller's (a registry read the
// settings window already performs).
base::DictValue PhiUserDefaultsSnapshotProperties(
    const model::PhiSettings& settings,
    bool is_default_browser);

// "Application Installed" on a profile's first launch, "Application Updated"
// when the version differs from the one recorded at the previous launch,
// nothing otherwise. Reads and rewrites `<user_data_dir>\phi_last_version`.
// `event` is null when there is nothing to capture.
struct PhiVersionTransition {
  const char* event = nullptr;
  base::DictValue properties;
};
PhiVersionTransition PhiNoteAppVersion(const std::wstring& user_data_dir);

// "space_created" / "space_switched": the Mac's counts, never a Space's
// name, id or profile. `surface` is where the create form was hosted
// ("sidebar" / "standalone_window"; the Mac also has "library").
base::DictValue PhiSpaceCreatedProperties(int total_spaces,
                                          bool non_default_profile,
                                          const std::string& surface);
base::DictValue PhiSpaceSwitchedProperties(int total_spaces);

// The Mac's AgentDriverBadge.telemetryName: a known brand's product label
// ("Claude Code"), "other" for a name that is not one, "unknown" for none.
// Never the raw name — a process name or path is not for the collector.
std::string PhiAgentTelemetryName(const std::string& agent_name);

// The Mac's ExtensionMessageRouter.untrackedUserSpaceCommands: false for
// the verbs that read state or probe liveness (the lists, shadow.ping,
// credentials.status) — they ride machine loops (a CDP connection's 30 s
// spaces.list heartbeat, the shadow keep-alive) and were 98.7% of a 2.2M-
// event week on the Mac with no user action behind them. Every mutating
// verb is tracked, as are credentials.get / getTotp: reading a secret is
// consequential even though it changes nothing.
bool PhiAgentUserSpaceCommandIsTracked(std::string_view verb);

// "agent_task_started" / "agent_task_completed" / "agent_user_space_command".
// `origin` is the Mac's "cdp" or "phi_agent"; on Windows every task arrives
// over the control pipe, so the callers pass "phi_agent".
base::DictValue PhiAgentTaskProperties(const std::string& origin,
                                       bool persistent,
                                       const std::string& agent_name);
base::DictValue PhiAgentTaskCompletedProperties(const std::string& origin,
                                                bool persistent,
                                                const std::string& agent_name,
                                                bool success);
base::DictValue PhiAgentUserSpaceCommandProperties(
    const std::string& command,
    const std::string& agent_name);
// "agent_shadow_window_opened": a surface without a task.
base::DictValue PhiAgentShadowWindowProperties(const std::string& origin,
                                               bool incognito);

// "agent_credential_access_requested" / "_approved": the Mac's
// CredentialAccessCoordinator metric — which access kind ("fill" /
// "reveal"), whether it was allowed, whether a prompt was shown, and for an
// approval how ("existing_grant", "once", "ten_minutes"). Never the scope
// (a domain), the purpose or anything from the vault.
base::DictValue PhiCredentialRequestedProperties(const char* kind,
                                                 bool approved,
                                                 bool prompted,
                                                 const std::string& agent_name);
base::DictValue PhiCredentialApprovedProperties(const char* kind,
                                                const char* approval_type,
                                                const std::string& agent_name);

// "space_profile_changed" / "profile_created": the profile COUNT, never a
// profile's id or name.
base::DictValue PhiProfileCountProperties(int total_profiles);

// "peek_view_opened": how the peek came to be ("auto" — the cross-site
// diversion, "explicit" — the context menu) and what its opener was bound
// to ("bookmark", "pinned", "normal"). "peek_view_ended": how it ended
// ("closed", "opened_as_tab", "opened_as_split", "opener_closed",
// "page_closed", "replaced") and how long it lived.
base::DictValue PhiPeekOpenedProperties(const char* entry_point,
                                        const char* opener_kind);
base::DictValue PhiPeekEndedProperties(const char* reason,
                                       int duration_seconds);

// "reader_view_opened" / "reader_view_open_failed": which control asked
// ("address_bar", "view_menu", "shortcut", "context_menu", "unknown") and,
// for a refusal, the extension's reason token.
base::DictValue PhiReaderOpenedProperties(const char* entry_point);
base::DictValue PhiReaderOpenFailedProperties(const char* entry_point,
                                              const std::string& reason);

// One onboarding event (phi_oobe_analytics.h) as the collector's
// properties, under the Mac's keys for that event.
base::DictValue PhiOobeEventProperties(
    const PhiOobeAnalyticsSession::Event& event);

// "guest_mode_exited": what pulled the user out of Guest Mode
// ("account_setting" — Settings ▸ You's sign-in).
base::DictValue PhiGuestModeExitedProperties(const char* trigger);

// "language_changed": the stored preference before and after, "system" for
// the follow-the-OS choice (the Mac's storageValue).
base::DictValue PhiLanguageChangedProperties(const std::string& from,
                                             const std::string& to);

// "feature_entry_tapped": which entry ("download", …) on which surface
// ("sidebar" / "web_content_header").
base::DictValue PhiFeatureEntryProperties(const char* button,
                                          const char* surface);

// "notification_card_action" (the Mac's NotificationCardAnalytics): what
// the user did with an agent notification card — "show" (a card became
// the visible one), "run" (its button), "dismiss" (the hide glyph),
// "delete" (the close glyph). The Mac's "popup" / "mute" are its sidebar
// presentation settings, which this client does not have.
base::DictValue PhiNotificationCardActionProperties(const char* action);

// The browser-data import, the Mac's BrowserImportAnalytics. Sources are
// the import window's keys ("chrome", "edge", "brave", "arc", "file"),
// sorted and unique as the Mac sends them; types are the normalised words
// ("bookmarks", "cookies", "extensions", "history"), sorted.
// "import_viewed" carries the entry point ("menu"); "import_types_selected"
// is sent once per source with the types the run takes from it (none for
// the file); "import_started" the sources; "import_finished" the aggregate
// outcome, the failed sources, the wall time and — when something failed —
// a low-cardinality error code (no_window, file_unreadable,
// source_import_failed, bookmark_persistence_failed).
base::DictValue PhiImportViewedProperties();
base::DictValue PhiImportTypesSelectedProperties(
    const std::string& source,
    std::vector<std::string> types);
base::DictValue PhiImportStartedProperties(std::vector<std::string> sources);
base::DictValue PhiImportFinishedProperties(std::vector<std::string> sources,
                                            std::vector<std::string> failed,
                                            int duration_seconds,
                                            const std::string& error_code);

// "first_time_action": the Mac's FirstTimeActionTracker — an eligible
// product action's FIRST success on this installation, with the seconds
// since the install. The record of what has fired lives beside the
// version file (`<user_data_dir>\phi_first_time_actions`, one action per
// line, the first line the install's unix time), so the event fires once
// per user data directory, not once per launch.
class PhiFirstTimeActions {
 public:
  PhiFirstTimeActions();
  ~PhiFirstTimeActions();

  PhiFirstTimeActions(const PhiFirstTimeActions&) = delete;
  PhiFirstTimeActions& operator=(const PhiFirstTimeActions&) = delete;

  // Reads the record; a missing one is the install moment (now_unix_ms).
  void Load(const std::wstring& user_data_dir, int64_t now_unix_ms);
  // True the FIRST time `action` is recorded — the caller captures the
  // event then. `properties` is filled with the Mac's two keys. Rewrites
  // the record. False (and nothing filled) when it already fired, or
  // before Load.
  bool Record(const char* action,
              int64_t now_unix_ms,
              base::DictValue* properties);

 private:
  std::wstring path_;
  int64_t installed_unix_ms_ = 0;
  std::vector<std::string> recorded_;
};

// The actions this client can perform (the Mac's FirstTimeAction raw
// values); the AI ones have no Windows counterpart.
inline constexpr char kFirstTimeActionSpaceCreated[] = "space_created";
inline constexpr char kFirstTimeActionImportFinished[] = "import_finished";
inline constexpr char kFirstTimeActionAgentTask[] = "agent_task";

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CLIENT_ANALYTICS_H_

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_framework_dispatcher.h"

#include <utility>

#include "client/phi_agent_space_manager.h"
#include "client/phi_client_logging.h"
#include "client/phi_coordinator.h"
#include "client/phi_profile_service.h"
#include "client/phi_session_restore.h"
#include "client/phi_surface_coordinator.h"
#include "client/phi_url_router.h"

namespace phi_client {

PhiFrameworkDispatcher::PhiFrameworkDispatcher(PhiCoordinator& coordinator)
    : coordinator_(coordinator) {}

PhiFrameworkDispatcher::~PhiFrameworkDispatcher() = default;

// --- Bootstrap and profiles ------------------------------------------------

void PhiFrameworkDispatcher::OnFrameworkReady(
    phi_framework::PhiFramework* framework,
    const char* user_data_dir_utf8) {
  // First: from here on the exe's own log lines must land where chrome.dll's
  // do (no-op in the component build, where they already share base.dll).
  AttachClientLoggingToBrowserLog(user_data_dir_utf8 ? user_data_dir_utf8
                                                     : "");
  coordinator_.OnFrameworkReady(framework, user_data_dir_utf8);
}

void PhiFrameworkDispatcher::GetAutoPictureInPicturePreferences(
    bool* enabled, bool* park_at_edge) {
  const auto& mode = coordinator_.preferences().AutoPictureInPictureMode();
  *enabled = mode != "off";
  *park_at_edge = mode == "parked";
}

void PhiFrameworkDispatcher::OnProfileLoaded(const char* profile_id) {
  coordinator_.OnProfileLoaded(profile_id);
}

void PhiFrameworkDispatcher::OnProfileCreated(const char* profile_id) {
  coordinator_.profiles().OnProfileCreated(profile_id);
}

// --- Windows ---------------------------------------------------------------

void PhiFrameworkDispatcher::OnBrowserWindowCreated(
    const phi_framework::WindowCreation& creation) {
  // Captured into a value FIRST: the creation's markers (restored_from, the
  // kiosk flag, the peek opener, the reader origin) are documented as valid
  // only during this call, and the switch below runs long enough to build a
  // shell window (lane 1 §4.3).
  const phi_framework::WindowCreation captured = creation;
  coordinator_.AdoptCreatedWindow(captured);
}

void PhiFrameworkDispatcher::OnBrowserWindowClosing(int64_t window_id) {
  coordinator_.RetireBrowserWindow(BrowserId{window_id});
}

void PhiFrameworkDispatcher::OnHostedWindowShow(int64_t window_id,
                                                bool activate,
                                                bool* handled) {
  *handled = coordinator_.OnHostedWindowShow(BrowserId{window_id}, activate);
}

void PhiFrameworkDispatcher::OnHostedWindowFullscreen(int64_t window_id,
                                                      bool fullscreen,
                                                      int x,
                                                      int y,
                                                      int width,
                                                      int height,
                                                      bool* handled) {
  const RECT target = {x, y, x + width, y + height};
  *handled = coordinator_.OnHostedWindowFullscreen(BrowserId{window_id},
                                                   fullscreen, target);
}

void PhiFrameworkDispatcher::OnWindowChanged(
    const phi_framework::WindowSnapshotView& snapshot,
    phi_framework::WindowChangeReason reason) {
  coordinator_.NoteWindowSnapshot(phi_framework::ToOwnedSnapshot(snapshot),
                                  reason);
}

void PhiFrameworkDispatcher::OnTargetURLChanged(int64_t window_id,
                                                const char* url) {
  coordinator_.OnTargetURLChanged(window_id, url);
}

void PhiFrameworkDispatcher::OnOmniboxResults(int64_t window_id,
                                              const char* json) {
  coordinator_.OnOmniboxResults(window_id, json);
}

void PhiFrameworkDispatcher::OnPlaceholderModeChanged(int64_t window_id,
                                                      bool entered) {
  coordinator_.OnPlaceholderModeChanged(window_id, entered);
}

void PhiFrameworkDispatcher::OnRefreshNewTab(int64_t window_id) {
  coordinator_.OnRefreshNewTab(window_id);
}

void PhiFrameworkDispatcher::OnContentFullscreenChanged(int64_t window_id,
                                                        bool fullscreen) {
  coordinator_.OnContentFullscreenChanged(window_id, fullscreen);
}

void PhiFrameworkDispatcher::OnContentBoundsChanged(int64_t window_id) {
  coordinator_.OnContentBoundsChanged(window_id);
}

void PhiFrameworkDispatcher::OnExtensionsChanged(int64_t window_id) {
  coordinator_.OnExtensionsChanged(window_id);
}

void PhiFrameworkDispatcher::OnExtensionsButtonRectRequested(int64_t window_id,
                                                             int* left,
                                                             int* top,
                                                             int* right,
                                                             int* bottom,
                                                             bool* found) {
  // Synchronous, inside Chromium's dialog construction: a layout read only.
  RECT rect = {};
  *found = coordinator_.ExtensionsButtonScreenRect(window_id, &rect);
  if (!*found) {
    return;
  }
  *left = rect.left;
  *top = rect.top;
  *right = rect.right;
  *bottom = rect.bottom;
}

void PhiFrameworkDispatcher::OnImportCompleted(const char* source,
                                               bool success) {
  coordinator_.OnImportCompleted(source, success);
}

// --- Routing ---------------------------------------------------------------

void PhiFrameworkDispatcher::OnOpenLinkInSpace(int64_t source_window_id,
                                               const char* space_id,
                                               const char* url) {
  coordinator_.url_router().OnOpenLinkInSpace(source_window_id, space_id, url);
}

void PhiFrameworkDispatcher::OnAskSpaceForURL(const char* url,
                                              const char* default_space_id,
                                              int64_t source_window_id,
                                              bool source_is_new_tab) {
  coordinator_.url_router().OnAskSpaceForURL(
      url, default_space_id, source_window_id, source_is_new_tab);
}

void PhiFrameworkDispatcher::OnRouteURLToSpace(const char* space_id,
                                               const char* url,
                                               int64_t source_window_id) {
  coordinator_.url_router().OnRouteURLToSpace(space_id, url, source_window_id);
}

void PhiFrameworkDispatcher::OnAuthCallbackUrl(const char* url,
                                               bool* taken) {
  // Inside a navigation throttle: the account service checks the URL and
  // starts the exchange on a worker; the tab is closed on a posted turn.
  *taken = coordinator_.account().HandleAuthCallbackUrl(url ? url : "");
}

void PhiFrameworkDispatcher::OnUnhandledPageKey(int64_t window_id,
                                                int vk,
                                                bool ctrl,
                                                bool shift,
                                                bool alt,
                                                bool* handled) {
  *handled = coordinator_.OnUnhandledPageKey(vk, ctrl, shift, alt);
}

// --- Phi sync (Chromium's side of it; PhiChromiumSync) --------------------

namespace {
void Put(phi_framework::PhiStringSink* sink, const std::string& value) {
  if (sink && !value.empty()) {
    sink->Append(value.data(), value.size());
  }
}
}  // namespace

void PhiFrameworkDispatcher::GetPhiSyncAccessToken(
    phi_framework::PhiStringSink* token) {
  Put(token, coordinator_.chromium_sync().AccessToken());
}

void PhiFrameworkDispatcher::GetPhiSyncAccount(
    int* state,
    phi_framework::PhiStringSink* subject,
    phi_framework::PhiStringSink* email) {
  std::string sub;
  std::string mail;
  *state =
      static_cast<int>(coordinator_.chromium_sync().Account(&sub, &mail));
  Put(subject, sub);
  Put(email, mail);
}

void PhiFrameworkDispatcher::GetPhiProfileSyncInfo(
    const char* profile_id,
    phi_framework::PhiStringSink* uuid,
    phi_framework::PhiStringSink* passphrase) {
  PhiChromiumSync::ProfileKey key;
  if (coordinator_.chromium_sync().ProfileKeyFor(profile_id ? profile_id : "",
                                                 &key)) {
    Put(uuid, key.uuid);
    Put(passphrase, key.passphrase);
  }
}

// --- Reader View and extensions -------------------------------------------

void PhiFrameworkDispatcher::OnReaderArticleExtracted(int request_id,
                                                      const char* json) {
  coordinator_.surface_coordinator().OnReaderArticleExtracted(request_id, json);
}

void PhiFrameworkDispatcher::OnOpenReaderView(int64_t window_id,
                                              int64_t tab_id) {
  coordinator_.surface_coordinator().OnOpenReaderView(window_id, tab_id);
}

void PhiFrameworkDispatcher::OnOpenLinkAsSplitFromSurface(
    int64_t surface_window_id,
    int64_t surface_tab_id,
    const char* url) {
  // Copied: the framework's string lives only for this call.
  coordinator_.surface_coordinator().OpenSurfaceLinkAsSplit(
      BrowserId{surface_window_id}, url ? std::string(url) : std::string());
}

void PhiFrameworkDispatcher::OnTabDistillable(int64_t window_id,
                                              int tab_id,
                                              bool distillable) {
  coordinator_.OnTabDistillable(window_id, tab_id, distillable);
}

void PhiFrameworkDispatcher::OnExtensionMessage(const char* request_id,
                                                const char* sender_id,
                                                const char* type,
                                                const char* payload_json) {
  coordinator_.OnExtensionMessage(request_id, sender_id, type, payload_json);
}

// --- Feedback --------------------------------------------------------------

void PhiFrameworkDispatcher::OnShowFeedbackRequested() {
  coordinator_.OnShowFeedbackRequested();
}

void PhiFrameworkDispatcher::OnTabSurfaceCaptured(int request_id,
                                                  const unsigned char* bgra,
                                                  int width,
                                                  int height) {
  coordinator_.OnTabSurfaceCaptured(request_id, bgra, width, height);
}

void PhiFrameworkDispatcher::OnFeedbackSystemLogs(int request_id,
                                                  const char* text) {
  coordinator_.account().OnFeedbackSystemLogs(request_id, text);
}

// --- The agent operating mask ----------------------------------------------
// The browser is the only thing that can see an agent driving one of the
// user's own tabs, so these three are its report channel
// (phi::PhiUserSpaceDrive). The registry owns the lifecycle rules.

void PhiFrameworkDispatcher::OnAgentDroveUserSpaceTab(int64_t tab_id,
                                                      int64_t window_id,
                                                      int64_t session_id) {
  coordinator_.agent_manager().drives().OnDrove(static_cast<int>(tab_id),
                                                window_id, session_id);
}

void PhiFrameworkDispatcher::OnAgentObservedUserSpaceTab(int64_t tab_id,
                                                         int64_t window_id,
                                                         int64_t session_id) {
  coordinator_.agent_manager().drives().OnObserved(static_cast<int>(tab_id),
                                                   window_id, session_id);
}

void PhiFrameworkDispatcher::OnAgentStoppedUserSpaceTab(int64_t tab_id,
                                                        int64_t session_id) {
  coordinator_.agent_manager().drives().OnStopped(static_cast<int>(tab_id),
                                                  session_id);
}

// --- Session restore -------------------------------------------------------

void PhiFrameworkDispatcher::OnSessionRestoreSettled(const char* receipt_json) {
  coordinator_.session_restore().OnSessionRestoreSettled(receipt_json);
}

void PhiFrameworkDispatcher::OnColdStartRestorePlan(
    phi_framework::ColdStartPlan* plan,
    bool* armed) {
  coordinator_.session_restore().OnColdStartRestorePlan(plan, armed);
}

void PhiFrameworkDispatcher::OnColdStartParkedWindows(
    const char* parked_json,
    const char* reporting_profile) {
  coordinator_.session_restore().OnColdStartParkedWindows(parked_json,
                                                          reporting_profile);
}

// --- Telemetry -------------------------------------------------------------

void PhiFrameworkDispatcher::OnAnalyticsEventCaptured(
    const char* module,
    const char* event_name,
    const char* properties_json) {
  coordinator_.RecordAnalyticsEvent(module, event_name, properties_json);
}

}  // namespace phi_client

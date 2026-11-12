// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_FRAMEWORK_DISPATCHER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_FRAMEWORK_DISPATCHER_H_

#include <cstdint>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "chrome/browser/phinomenon/phi_framework/phi_window_snapshot.h"

namespace phi_client {

class PhiCoordinator;

// The client's framework delegate (docs/architecture-review.md §3.4, lane 1
// §4.3): the ONE place that sees the raw framework callbacks, registered
// with the framework before ChromeMain and called back on the browser UI
// thread.
//
// INVARIANT: every override is at most ten lines and does exactly one of two
// things — record the callback's values into a model or registry, or forward
// them to the service that owns the domain. It holds no business state of
// its own, makes no decisions, and never calls the framework back. A
// callback that needs work done gets a named method on the owning service;
// a callback whose values are "valid only during the call" (the window
// creation's restored-from / kiosk / peek-opener / reader-origin markers) is
// captured into a value here and handed on.
class PhiFrameworkDispatcher : public phi_framework::PhiFrameworkDelegate {
 public:
  explicit PhiFrameworkDispatcher(PhiCoordinator& coordinator);
  ~PhiFrameworkDispatcher() override;

  PhiFrameworkDispatcher(const PhiFrameworkDispatcher&) = delete;
  PhiFrameworkDispatcher& operator=(const PhiFrameworkDispatcher&) = delete;

  // phi_framework::PhiFrameworkDelegate:
  void OnFrameworkReady(phi_framework::PhiFramework* framework,
                        const char* user_data_dir_utf8) override;
  void OnProfileLoaded(const char* profile_id) override;
  void GetAutoPictureInPicturePreferences(bool* enabled,
                                         bool* park_at_edge) override;
  void OnBrowserWindowCreated(
      const phi_framework::WindowCreation& creation) override;
  void OnBrowserWindowClosing(int64_t window_id) override;
  void OnHostedWindowShow(int64_t window_id,
                          bool activate,
                          bool* handled) override;
  void OnHostedWindowFullscreen(int64_t window_id,
                                bool fullscreen,
                                int x,
                                int y,
                                int width,
                                int height,
                                bool* handled) override;
  void OnWindowChanged(const phi_framework::WindowSnapshotView& snapshot,
                       phi_framework::WindowChangeReason reason) override;
  void OnOpenLinkInSpace(int64_t source_window_id,
                         const char* space_id,
                         const char* url) override;
  void OnAskSpaceForURL(const char* url,
                        const char* default_space_id,
                        int64_t source_window_id,
                        bool source_is_new_tab) override;
  void OnRouteURLToSpace(const char* space_id,
                         const char* url,
                         int64_t source_window_id) override;
  void OnTargetURLChanged(int64_t window_id, const char* url) override;
  void OnOmniboxResults(int64_t window_id, const char* json) override;
  void OnImportCompleted(const char* source, bool success) override;
  void OnAuthCallbackUrl(const char* url, bool* taken) override;
  void OnUnhandledPageKey(int64_t window_id,
                          int vk,
                          bool ctrl,
                          bool shift,
                          bool alt,
                          bool* handled) override;
  void GetPhiSyncAccessToken(phi_framework::PhiStringSink* token) override;
  void GetPhiSyncAccount(int* state,
                         phi_framework::PhiStringSink* subject,
                         phi_framework::PhiStringSink* email) override;
  void GetPhiProfileSyncInfo(const char* profile_id,
                             phi_framework::PhiStringSink* uuid,
                             phi_framework::PhiStringSink* passphrase) override;
  void OnPlaceholderModeChanged(int64_t window_id, bool entered) override;
  void OnRefreshNewTab(int64_t window_id) override;
  void OnContentFullscreenChanged(int64_t window_id, bool fullscreen) override;
  void OnContentBoundsChanged(int64_t window_id) override;
  void OnExtensionsChanged(int64_t window_id) override;
  void OnExtensionsButtonRectRequested(int64_t window_id,
                                       int* left,
                                       int* top,
                                       int* right,
                                       int* bottom,
                                       bool* found) override;
  void OnProfileCreated(const char* profile_id) override;
  void OnReaderArticleExtracted(int request_id, const char* json) override;
  void OnShowFeedbackRequested() override;
  void OnFeedbackSystemLogs(int request_id, const char* text) override;
  void OnTabSurfaceCaptured(int request_id,
                            const unsigned char* bgra,
                            int width,
                            int height) override;
  void OnAgentDroveUserSpaceTab(int64_t tab_id,
                                int64_t window_id,
                                int64_t session_id) override;
  void OnAgentObservedUserSpaceTab(int64_t tab_id,
                                   int64_t window_id,
                                   int64_t session_id) override;
  void OnAgentStoppedUserSpaceTab(int64_t tab_id, int64_t session_id) override;
  void OnTabDistillable(int64_t window_id,
                        int tab_id,
                        bool distillable) override;
  void OnExtensionMessage(const char* request_id,
                          const char* sender_id,
                          const char* type,
                          const char* payload_json) override;
  void OnOpenReaderView(int64_t window_id, int64_t tab_id) override;
  void OnOpenLinkAsSplitFromSurface(int64_t surface_window_id,
                                    int64_t surface_tab_id,
                                    const char* url) override;
  void OnSessionRestoreSettled(const char* receipt_json) override;
  void OnAnalyticsEventCaptured(const char* module,
                                const char* event_name,
                                const char* properties_json) override;
  void OnColdStartRestorePlan(phi_framework::ColdStartPlan* plan,
                              bool* armed) override;
  void OnColdStartParkedWindows(const char* parked_json,
                                const char* reporting_profile) override;

 private:
  // The composition root that owns this. Non-owning back-pointer.
  RAW_PTR_EXCLUSION PhiCoordinator& coordinator_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_FRAMEWORK_DISPATCHER_H_

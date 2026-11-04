// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_ACCOUNT_FEEDBACK_H_
#define PHIBROWSER_WIN_CLIENT_PHI_ACCOUNT_FEEDBACK_H_

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_account_profile.h"
#include "phibrowser-win/client/phi_auth.h"
#include "phibrowser-win/client/phi_feedback_window.h"

namespace phi_client {

// The account and the feedback pipeline (docs/architecture-review.md §3.4,
// lane 1 §1.2 cluster 15; docs/login-port.md): the Auth0 session
// (`PhiAuth`, whose ONE writer is the UI thread — workers post their
// results back), the account profile and avatar behind it
// (`PhiAccountProfileService`, the same single-writer shape), the feedback
// outbox the session names the account of, and the Send Feedback window's
// host.
//
// It owns the session, the profile and the harness stand-in account and
// nothing else. Everything it needs from the app — a tab to open the hosted
// login in, raising a window, telling the Settings panes the account moved
// — comes through Host; the framework calls (system
// logs, the extension list) go straight to the framework it was initialised
// with.
class PhiAccountAndFeedback : public PhiFeedbackHost {
 public:
  class Host {
   public:
    virtual ~Host() = default;
    // Opens `url` as a foreground tab in the FIRST user window (the account
    // site's own pages, the avatar editor). A no-op when no user window is up.
    virtual void OpenUrlInFirstUserSlot(const std::string& url) = 0;
    // Loads the hosted login at `url` into the onboarding window's embedded
    // OFF-THE-RECORD browser (opening the window on its signing-in page when
    // it is not up). The tenant's session cookie therefore never lands in a
    // browsing profile: it dies with that browser. The redirect to the
    // callback scheme comes back through HandleAuthCallbackUrl.
    virtual void OpenHostedLogin(const std::string& url) = 0;
    // Raises the shell showing `browser_window_id`.
    virtual void FocusWindowForBrowser(int64_t browser_window_id) = 0;
    // The session or profile state moved: the Account pane re-reads it.
    virtual void NoteAccountChanged() = 0;
    // sync_phase for a feedback report: the summary the Sync pane shows.
    virtual std::string SyncSummaryPhaseName() const = 0;
    // The client's one analytics capture path (phi_client_analytics.h):
    // the harness ring, then the collector's consent gate and allow-list.
    virtual void CaptureAnalytics(const char* module,
                                  const char* event_name,
                                  base::DictValue properties) = 0;
  };

  explicit PhiAccountAndFeedback(Host& host);
  ~PhiAccountAndFeedback() override;

  PhiAccountAndFeedback(const PhiAccountAndFeedback&) = delete;
  PhiAccountAndFeedback& operator=(const PhiAccountAndFeedback&) = delete;

  // Restores the persisted session and profile (the session is validated in
  // the background; `notify_hwnd` gets `notify_msg` posted on every settled
  // state change) and starts the outbox, whose jobs earlier runs queued
  // resume uploading once the account is known. UI thread, from
  // OnFrameworkReady.
  void Init(phi_framework::PhiFramework* framework,
            const std::wstring& user_data_dir,
            HWND notify_hwnd,
            UINT notify_msg);
  // The notify message arrived: the Account pane re-reads state, the
  // profile service moves a worker's result in, and the outbox learns whose
  // jobs to work on.
  void OnAuthChanged();
  // Everything that must reach disk before the session ends. Idempotent.
  void FlushForShutdown();

  // --- The account ---------------------------------------------------------
  // The signed-in user's name / email, or false when signed out. Either
  // out-parameter may be null. The name is the profile's (the editable
  // one) when the profile has loaded, else the id_token's.
  bool AccountInfo(std::string* name, std::string* email) const;
  bool AccountSigningIn() const;
  // Signed in, but the tenant no longer honours the session (see
  // PhiAuth::reauth_required): the pane shows the warning and offers
  // Reauthenticate, which is RequestSignIn.
  bool AccountReauthRequired() const;
  // A profile fetch or name update is in flight.
  bool AccountBusy() const;
  // The pane's error line: PhiAuth::last_error, else the profile service's.
  std::string AccountLastError() const;
  const PhiAccountProfileService& Profile() const { return profile_; }
  // The avatar as a `px` circle (premultiplied BGRA), empty when none.
  const std::vector<uint8_t>& AccountAvatarCircle(int px);
  // Starts the hosted login (Host::OpenHostedLogin opens it).
  void RequestSignIn();
  void RequestCancelSignIn();
  // The framework's auth-callback throttle saw a navigation to the callback
  // scheme (PhiFrameworkDelegate::OnAuthCallbackUrl, inside the throttle —
  // no UI here). True when the session took it; the framework then retires
  // the tab it landed in.
  bool HandleAuthCallbackUrl(const std::string& url);
  // Drops the session, revokes the refresh token and opens the tenant's
  // logout page in a tab so the hosted-login session ends too; the
  // account's unsent feedback reports are deleted from disk. The
  // confirmation is the caller's (the pane asks; the harness does not).
  void RequestSignOut();
  // Drops the local session without the tenant's logout page — the
  // persisted Guest choice discarding an unfinished sign-in at launch.
  void DiscardSession();
  // PUT the display name (validated by the caller: non-empty, at most
  // PhiAccountProfileService::kMaxNameLength).
  void RequestRenameAccount(const std::string& name);
  // Opens the account service's avatar editor in a tab; the next
  // RefreshAccountProfile re-fetches regardless of the interval.
  void RequestEditAvatar();
  // Re-fetches the profile and avatar (the settings window coming back to
  // the front); rate-limited unless `force`.
  void RefreshAccountProfile(bool force);
  // The Auth0 session, for the harness verbs' reports.
  const PhiAuth& auth() const { return auth_; }
  // The key layer's bound token calls (AccessTokenBlockingFor,
  // InvalidateAccessToken) are thread-safe members of a non-const PhiAuth.
  PhiAuth* auth_for_sync_keys() { return &auth_; }
  // Chromium sync's bearer (PhiAuth::AccessTokenForSync). UI thread.
  std::string AccessTokenForSync() { return auth_.AccessTokenForSync(); }
  // Harness only (agentSpace.feedback.setAccount): a stand-in account for a
  // box with no Auth0 session; empty clears. Ignored while a real session
  // is signed in.
  void SetTestFeedbackAccount(const std::string& user_id,
                              const std::string& email);

  // --- Send Feedback -------------------------------------------------------
  // Opens (or rebinds) the app-global feedback window over `owner`.
  void OpenFeedback(HWND owner, const PhiFeedbackContext& context);
  // The framework answered a FetchFeedbackSystemLogs (or it timed out).
  void OnFeedbackSystemLogs(int request_id, const char* text);

  // PhiFeedbackHost:
  bool FeedbackAccount(std::string* email) override;
  void RequestFeedbackSignIn() override;
  void OpenFeedbackLink(int64_t window_id, const std::string& url) override;
  bool FetchFeedbackSystemLogs(int64_t window_id, int request_id) override;
  std::map<std::string, std::string> FeedbackSyncStatus() override;
  std::map<std::string, std::string> PhiExtensionVersions(
      int64_t window_id) override;

 private:
  // Hands the signed-in account to the outbox (whose jobs are filed and
  // uploaded per account) after every auth change.
  void SyncFeedbackAccount();
  // The subject the profile service should serve: the session's, or empty
  // when there is no token to fetch with.
  std::string ProfileSubject() const;

  RAW_PTR_EXCLUSION Host& host_;
  // The framework command interface (a singleton in chrome.dll); non-owning,
  // across a module boundary, so not a raw_ptr. Null until Init.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  // The Auth0 session (bucket D auth).
  PhiAuth auth_;
  // The account profile and avatar behind the session.
  PhiAccountProfileService profile_;
  // RequestEditAvatar was used: the next refresh ignores the interval.
  bool avatar_edit_pending_ = false;
  // Whether a user-initiated sign-in was in flight at the last settle, so
  // the settle that lands it is the Mac's user_logged_in (a background
  // refresh or a restored session is not).
  bool sign_in_in_flight_ = false;
  // A harness-set stand-in account (agentSpace.feedback.setAccount) for a
  // box with no Auth0 session: reports file and prepare under it, and the
  // upload then fails for want of a token — which is the testable part.
  // Ignored while a real session is signed in.
  std::string test_feedback_user_id_;
  std::string test_feedback_email_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_ACCOUNT_FEEDBACK_H_

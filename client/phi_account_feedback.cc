// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_account_feedback.h"

#include <optional>
#include <utility>
#include <vector>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_client_analytics.h"
#include "phibrowser-win/client/phi_feedback_outbox.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_toast.h"

namespace phi_client {

namespace {

// The outbox gave up on reports the user already saw "sent" for: say so
// over whichever Phi window is in front. None in front (the user is in
// another app) — the log line is all there is.
void NotifyAbandonedFeedback(int count) {
  if (count <= 0) {
    return;
  }
  HWND owner = ::GetForegroundWindow();
  DWORD pid = 0;
  if (owner) {
    ::GetWindowThreadProcessId(owner, &pid);
  }
  if (!owner || pid != ::GetCurrentProcessId()) {
    LOG(WARNING) << "[Feedback] " << count
                 << " abandoned report(s); no window to tell the user in";
    return;
  }
  owner = ::GetAncestor(owner, GA_ROOT);
  const std::vector<PhiTheme>& themes = PhiBuiltInThemes();
  if (!owner || themes.empty()) {
    return;
  }
  const PhiPalette palette =
      PhiResolvePalette(themes.front(), PhiAppearance::kSystem);
  PhiShowToast(owner, palette, PhiStr("feedback.outbox.abandoned.title"),
               PhiStr("feedback.outbox.abandoned.message"), 6000);
}

}  // namespace

PhiAccountAndFeedback::PhiAccountAndFeedback(Host& host) : host_(host) {}

PhiAccountAndFeedback::~PhiAccountAndFeedback() = default;

void PhiAccountAndFeedback::Init(phi_framework::PhiFramework* framework,
                                 const std::wstring& user_data_dir,
                                 HWND notify_hwnd,
                                 UINT notify_msg) {
  framework_ = framework;
  // Restore the Auth0 session (validated in the background; the message
  // refreshes the settings pane whenever the state settles) and the cached
  // profile behind it.
  auth_.Load(user_data_dir, notify_hwnd, notify_msg);
  profile_.Load(user_data_dir, notify_hwnd, notify_msg);
  profile_.SyncWithSession(&auth_, ProfileSubject(), auth_.user().picture);
  // The feedback outbox: reports queued by earlier runs resume uploading
  // once the account is known (the Mac's FeedbackOutboxUploader.start).
  PhiFeedbackOutbox::Get().Start(user_data_dir, &auth_);
  // An abandoned report rides the same message back to the UI thread.
  PhiFeedbackOutbox::Get().SetNotify(notify_hwnd, notify_msg);
  SyncFeedbackAccount();
}

void PhiAccountAndFeedback::OnAuthChanged() {
  // The Mac's user_logged_in: the sign-in the user started came back as a
  // session (LoginViewController's capture on credentials). A failure or
  // a cancel settles the same way without one and counts as nothing.
  if (sign_in_in_flight_ && !auth_.signing_in()) {
    sign_in_in_flight_ = false;
    if (auth_.signed_in() && auth_.last_error().empty()) {
      host_.CaptureAnalytics(kAnalyticsModuleAccount, kEventUserLoggedIn,
                             base::DictValue());
    }
  }
  profile_.SyncWithSession(&auth_, ProfileSubject(), auth_.user().picture);
  profile_.ApplyPending();
  host_.NoteAccountChanged();
  SyncFeedbackAccount();
  NotifyAbandonedFeedback(PhiFeedbackOutbox::Get().TakeAbandonedJobCount());
}

void PhiAccountAndFeedback::FlushForShutdown() {
  // Best-effort and idempotent: a second call finds no thread to join.
  PhiFeedbackOutbox::Get().Stop();
}

std::string PhiAccountAndFeedback::ProfileSubject() const {
  return auth_.signed_in() && !auth_.reauth_required() ? auth_.user().sub
                                                        : std::string();
}

bool PhiAccountAndFeedback::AccountInfo(std::string* name,
                                        std::string* email) const {
  if (!auth_.signed_in()) {
    return false;
  }
  const std::optional<PhiAccountProfile>& profile = profile_.profile();
  const bool profile_matches =
      profile && profile->auth0_id == auth_.user().sub;
  if (name) {
    *name = profile_matches && !profile->name.empty() ? profile->name
                                                       : auth_.user().name;
  }
  if (email) {
    *email = profile_matches && !profile->email.empty() ? profile->email
                                                         : auth_.user().email;
  }
  return true;
}

bool PhiAccountAndFeedback::AccountSigningIn() const {
  return auth_.signing_in();
}

bool PhiAccountAndFeedback::AccountReauthRequired() const {
  return auth_.signed_in() && auth_.reauth_required();
}

bool PhiAccountAndFeedback::AccountBusy() const {
  return profile_.busy();
}

std::string PhiAccountAndFeedback::AccountLastError() const {
  if (!auth_.last_error().empty()) {
    return auth_.last_error();
  }
  return profile_.last_error();
}

const std::vector<uint8_t>& PhiAccountAndFeedback::AccountAvatarCircle(
    int px) {
  static const std::vector<uint8_t> kNone;
  if (!auth_.signed_in()) {
    return kNone;
  }
  return profile_.AvatarCircleBgra(px);
}

void PhiAccountAndFeedback::RequestSignIn() {
  auth_.BeginSignIn([this](const std::string& url) {
    // The hosted login rides in the onboarding window's off-the-record
    // browser, never a browsing-profile tab (its cookies would persist
    // there); its redirect to the callback scheme comes back through
    // HandleAuthCallbackUrl.
    host_.OpenHostedLogin(url);
  });
  sign_in_in_flight_ = auth_.signing_in();
  host_.NoteAccountChanged();
}

void PhiAccountAndFeedback::RequestCancelSignIn() {
  auth_.CancelSignIn();  // Notifies; the onboarding window closes its browser.
}

bool PhiAccountAndFeedback::HandleAuthCallbackUrl(const std::string& url) {
  return auth_.HandleCallbackUrl(url);
}

void PhiAccountAndFeedback::RequestSignOut() {
  // No logout page: the tenant's session cookie lived in the off-the-record
  // login browser and died with it, so there is nothing in a browsing
  // profile to end. The refresh token is revoked below.
  // The account's queued feedback (description, page URL, attachments)
  // does not outlive its sign-out on a shared machine.
  const std::string signed_out_user =
      auth_.signed_in() ? auth_.user().sub : std::string();
  if (auth_.signed_in()) {
    // The Mac's user_logged_out, captured before the session clears (the
    // identity it belongs to is still the sender's).
    host_.CaptureAnalytics(kAnalyticsModuleAccount, kEventUserLoggedOut,
                           base::DictValue());
  }
  auth_.SignOut();  // Revokes the refresh token, notifies.
  profile_.Clear();
  avatar_edit_pending_ = false;
  host_.NoteAccountChanged();
  SyncFeedbackAccount();
  PhiFeedbackOutbox::Get().PurgeAccount(signed_out_user);
}

void PhiAccountAndFeedback::DiscardSession() {
  auth_.SignOut();
  profile_.Clear();
  avatar_edit_pending_ = false;
  SyncFeedbackAccount();
}

void PhiAccountAndFeedback::RequestRenameAccount(const std::string& name) {
  if (ProfileSubject().empty()) {
    return;
  }
  profile_.UpdateName(&auth_, name);
  host_.NoteAccountChanged();  // The pane shows the busy state.
}

void PhiAccountAndFeedback::RequestEditAvatar() {
  if (ProfileSubject().empty()) {
    return;
  }
  // The Mac opens the same page in an account web window
  // (AccountSettingViewController.avatarEditURL) and re-fetches when it
  // closes; here the settings window's next activation re-fetches.
  avatar_edit_pending_ = true;
  host_.OpenUrlInFirstUserSlot(PhiAuth::AccountBaseUrl() + "/avatar");
}

void PhiAccountAndFeedback::RefreshAccountProfile(bool force) {
  if (ProfileSubject().empty()) {
    return;
  }
  const bool forced = force || avatar_edit_pending_;
  avatar_edit_pending_ = false;
  profile_.Refresh(&auth_, forced);
  if (profile_.busy()) {
    host_.NoteAccountChanged();
  }
}

void PhiAccountAndFeedback::SetTestFeedbackAccount(const std::string& user_id,
                                                   const std::string& email) {
  test_feedback_user_id_ = user_id;
  test_feedback_email_ = email;
  SyncFeedbackAccount();
}

void PhiAccountAndFeedback::SyncFeedbackAccount() {
  // A session that needs reauthentication has no bearer to upload with:
  // the outbox keeps its jobs (they are the account's) but stops trying,
  // and the form asks for a sign-in instead of filing a report that would
  // burn its retries on "no account".
  if (auth_.signed_in() && !auth_.reauth_required()) {
    PhiFeedbackOutbox::Get().SetAccount(auth_.user().sub, auth_.user().email);
  } else {
    PhiFeedbackOutbox::Get().SetAccount(test_feedback_user_id_,
                                        test_feedback_email_);
  }
  // An open form swaps its "Sending as" line and the sign-in InfoBar.
  PhiFeedbackWindow::OnAccountChanged();
}

void PhiAccountAndFeedback::OpenFeedback(HWND owner,
                                         const PhiFeedbackContext& context) {
  PhiFeedbackWindow::Open(this, context, owner);
}

void PhiAccountAndFeedback::OnFeedbackSystemLogs(int request_id,
                                                 const char* text) {
  PhiFeedbackWindow::OnSystemLogs(request_id, text);
}

bool PhiAccountAndFeedback::FeedbackAccount(std::string* email) {
  if (auth_.signed_in() && !auth_.reauth_required()) {
    *email = auth_.user().email;
    return true;
  }
  if (!test_feedback_user_id_.empty()) {
    *email = test_feedback_email_;
    return true;
  }
  return false;
}

void PhiAccountAndFeedback::RequestFeedbackSignIn() {
  RequestSignIn();
}

void PhiAccountAndFeedback::OpenFeedbackLink(int64_t window_id,
                                             const std::string& url) {
  if (!framework_) {
    return;
  }
  framework_->OpenTab(BrowserId{window_id}, url.c_str());
  host_.FocusWindowForBrowser(window_id);
}

bool PhiAccountAndFeedback::FetchFeedbackSystemLogs(int64_t window_id,
                                                    int request_id) {
  return framework_ &&
         framework_->FetchFeedbackSystemLogs(window_id, request_id);
}

std::map<std::string, std::string> PhiAccountAndFeedback::FeedbackSyncStatus() {
  return {{"sync_phase", host_.SyncSummaryPhaseName()}};
}

std::map<std::string, std::string> PhiAccountAndFeedback::PhiExtensionVersions(
    int64_t window_id) {
  std::map<std::string, std::string> versions;
  if (!framework_) {
    return versions;
  }
  const char* json = framework_->ListExtensionsJson(window_id);
  std::optional<base::Value> list =
      base::JSONReader::Read(json ? json : "[]", base::JSON_PARSE_RFC);
  if (!list || !list->is_list()) {
    return versions;
  }
  for (const base::Value& row : list->GetList()) {
    const base::DictValue* dict = row.GetIfDict();
    const std::string* id = dict ? dict->FindString("id") : nullptr;
    const std::string* name = dict ? dict->FindString("name") : nullptr;
    const std::string* version = dict ? dict->FindString("version") : nullptr;
    if (id && name && version && PhiFeedbackOutbox::IsPhiExtensionId(*id)) {
      versions[*name] = *version;
    }
  }
  return versions;
}

}  // namespace phi_client

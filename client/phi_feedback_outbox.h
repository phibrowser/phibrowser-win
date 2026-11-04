// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_FEEDBACK_OUTBOX_H_
#define PHIBROWSER_WIN_CLIENT_PHI_FEEDBACK_OUTBOX_H_

#include <windows.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"

namespace phi_client {

class PhiAuth;

// One attachment the user added to the form: a file they picked, or an image
// they pasted (already PNG-encoded).
struct PhiFeedbackDraftAttachment {
  std::wstring filename;
  int64_t size = 0;
  bool is_image = false;
  std::wstring file_path;  // A picked file, or empty for...
  std::string pasted_png;  // ...a pasted image's bytes.
};

// What the form hands over on Send — the Mac's FeedbackDraft.
struct PhiFeedbackDraft {
  std::string description;
  std::string page_url;
  std::string page_title;
  // Phi's own extensions, name -> version (the Mac's phiExtensionVersions).
  std::map<std::string, std::string> extension_versions;
  // The sync status keys frozen at Send (the Mac's syncStatusMetadata): enum
  // raw values only.
  std::map<std::string, std::string> sync_status;
  // Chromium's scrubbed system logs, when the framework produced them.
  std::optional<std::string> chromium_system_logs;
  std::vector<PhiFeedbackDraftAttachment> attachments;
};

// The Windows twin of the Mac's FeedbackOutbox + FeedbackOutboxUploader
// (UserInterface/Feedback/FeedbackOutbox.swift): a feedback report is first
// written to disk as a JOB — its description, metadata, copies of the
// attachments — and a worker then prepares the archives, uploads them
// through the account service's presigned URLs and submits the report,
// retrying with backoff across restarts, so a report survives the network
// being down when the user hit Send.
//
// Layout, under the user data dir:
//   PhiFeedbackOutbox\<account>\<jobId>\manifest.json
//                                       images\  files\  logs\  prepared\
// The manifest carries the job's state machine (queued → preparing →
// uploading → submitting → submitted | failedWaitingRetry) and every
// prepared attachment's upload state, so an interrupted job resumes where
// it stopped instead of re-uploading.
//
// Jobs are per ACCOUNT (the bearer token that submits them), as on the Mac:
// another account's jobs wait untouched until that account signs in again.
class PhiFeedbackOutbox {
 public:
  // The submit endpoint takes at most five attachments; a picked file may be
  // 10 MB, any single prepared attachment 20 MB.
  // "Windows 10.0.26100 (24H2)" — what a report names the OS as (the
  // form's diagnostic-info expander shows it before Send).
  static std::string OsVersion();

  static constexpr int kMaxSubmitAttachments = 5;
  static constexpr int64_t kMaxSelectedAttachmentBytes = 10 * 1024 * 1024;
  static constexpr int64_t kMaxAttachmentBytes = 20 * 1024 * 1024;
  // The planning budget for one zip's INPUT bytes; the real output is
  // measured afterwards and split when it still exceeds the limit.
  static constexpr int64_t kZipPlanningBytes =
      kMaxAttachmentBytes - 512 * 1024;
  // A failing job keeps retrying (with capped backoff) for a week of wall
  // clock, and is abandoned only once it is that old AND has had at least
  // this many attempts — so a report filed just before a week offline
  // still gets its tries when the network comes back.
  static constexpr int64_t kMaxJobAgeMs = 7LL * 24 * 60 * 60 * 1000;
  static constexpr int kMinJobRetryCount = 5;
  // Bumped when the archive layout changes: older jobs are re-prepared.
  static constexpr int kArchiveStrategyVersion = 6;

  static PhiFeedbackOutbox& Get();

  PhiFeedbackOutbox(const PhiFeedbackOutbox&) = delete;
  PhiFeedbackOutbox& operator=(const PhiFeedbackOutbox&) = delete;

  // Binds the outbox root and the token source and starts the worker.
  // `auth` must outlive the process (it does: the coordinator owns it).
  void Start(const std::wstring& user_data_dir, PhiAuth* auth);
  // The signed-in account, or empty ids when signed out. Jobs are filed and
  // processed under `user_id`; a change wakes the worker (the Mac's
  // mainAccountChanged observer).
  void SetAccount(const std::string& user_id, const std::string& email);
  // Files `draft` as a job for the current account (copying the attachments
  // in) and wakes the worker. UI thread. False with `error` set when there
  // is no account, the description is blank, or a copy failed.
  bool Enqueue(const PhiFeedbackDraft& draft, std::string* error);
  // Wakes the worker to scan the outbox now.
  void Kick();
  // Stops the worker; a job mid-flight resumes on the next run.
  void Stop();
  // `hwnd` gets `msg` posted (from the worker) when a job is abandoned;
  // the UI thread then drains TakeAbandonedJobCount() and tells the user.
  void SetNotify(HWND hwnd, UINT msg);
  // Jobs abandoned since the last call (the user already saw "sent" for
  // each). UI thread.
  int TakeAbandonedJobCount();
  // `user_id` signed out: its queued reports (description, page URL,
  // attachments) are deleted. Runs on the worker, between jobs, so it never
  // races an upload in flight; skipped when that account is current again
  // by then (a quick sign-in back keeps its reports).
  void PurgeAccount(const std::string& user_id);

  // The outbox as the harness sees it: the account, the root, and every job
  // with its state — {"account","root","jobs":[{id,status,retryCount,
  // lastError,nextAttemptAtMs,sourceImages,sourceFiles,hasSystemLogs,
  // prepared,uploaded}]}.
  std::string StatusJson();

  // Validates a file the user picked (the Mac's selectedAttachmentInfo):
  // regular files only (no directories, no symlinks), at most 10 MB.
  static bool SelectedAttachmentInfo(const std::wstring& path,
                                     int64_t* size,
                                     std::string* mime_type,
                                     bool* is_image,
                                     std::string* error);
  // The registry's Content Type for the path's extension, with a built-in
  // fallback for the common image types; application/octet-stream when
  // unknown.
  static std::string MimeTypeForPath(const std::wstring& path);
  // "debug" for a DCHECK build, else PHI_WIN_CHANNEL.
  static std::string BrowserChannelName();
  // One of Phi's own extensions (the Mac's phiExtensionIds) — the ones
  // whose versions a report carries as components.
  static bool IsPhiExtensionId(const std::string& id);
  static bool ShouldDiscardFailedJob(int retry_count,
                                     int64_t created_at_ms,
                                     int64_t now_ms) {
    return retry_count >= kMinJobRetryCount &&
           now_ms - created_at_ms > kMaxJobAgeMs;
  }

 private:
  PhiFeedbackOutbox();
  ~PhiFeedbackOutbox();

  static DWORD WINAPI WorkerThread(void* self);
  void RunLoop();
  // One pass over the current account's jobs. True when some job wants
  // another attempt later.
  bool ProcessOutbox();
  bool ProcessJob(const std::wstring& job_root, const std::string& user_id);
  // The account this thread should be working for, or empty.
  std::string CurrentUserId();
  std::wstring AccountRoot(const std::string& user_id) const;
  // Deletes the outboxes PurgeAccount queued. Worker thread.
  void RunPendingPurges();
  // A job was given up on: count it and wake the UI thread.
  void NoteJobAbandoned();

  CRITICAL_SECTION lock_;
  HANDLE wake_ = nullptr;
  HANDLE thread_ = nullptr;
  bool stopping_ = false;
  bool started_ = false;
  std::wstring root_;
  std::wstring user_data_dir_;
  // Process-lifetime, owned by the coordinator.
  RAW_PTR_EXCLUSION PhiAuth* auth_ = nullptr;
  std::string user_id_;
  std::string email_;
  std::vector<std::string> pending_purges_;
  int abandoned_jobs_ = 0;
  HWND notify_hwnd_ = nullptr;
  UINT notify_msg_ = 0;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_FEEDBACK_OUTBOX_H_

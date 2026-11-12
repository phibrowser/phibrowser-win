// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_TELEMETRY_H_
#define PHIBROWSER_WIN_CLIENT_PHI_TELEMETRY_H_

#include <windows.h>

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

#include "base/values.h"

namespace phi_client {

// The client's outbound telemetry: product-analytics events produced anywhere
// in the browser process (phi_analytics -> the framework delegate -> here),
// spooled to disk and POSTed to a collector in batches.
//
// BACKEND-AGNOSTIC BY DESIGN. The destination is deployment configuration —
// `phi_telemetry.json` in the user data directory, the same file the crash
// reporter reads its collector out of — and an unconfigured build spools
// NOTHING and sends NOTHING. Picking a service later is an edit to that file
// plus, when its ingest shape differs, one function here (BuildBatchBody).
//
//   {
//     "analytics_endpoint": "https://collector.example/ingest",
//     "analytics_key": "<write key, sent as a bearer token>",
//     "crash_endpoint": "https://collector.example/minidump"
//   }
//
// Durability is deliberately modest, matching what the events are worth: the
// spool survives a restart, a batch that fails is retried with backoff, and a
// spool that outgrows its bound drops its OLDEST entries. Nothing here is
// worth blocking the UI thread or the user's shutdown for.
class PhiTelemetry {
 public:
  static PhiTelemetry& Get();

  PhiTelemetry(const PhiTelemetry&) = delete;
  PhiTelemetry& operator=(const PhiTelemetry&) = delete;

  // Reads the config, reloads any spool left by the previous run, and starts
  // the uploader — but only when an analytics endpoint is configured.
  // Idempotent; called once the framework hands over the user data dir.
  void Start(const std::wstring& user_data_dir);

  // The user's consent (Settings ▸ General ▸ "Share anonymous usage
  // analytics and crash reports", PhiPreferences.json analyticsEnabled; the
  // coordinator pushes the same switch to Chromium's metrics + crash-upload
  // consent through PhiFramework::SetMetricsReportingEnabled). Off until
  // set: an event
  // recorded while disabled is DROPPED, not spooled — nothing the user did
  // not agree to share ever reaches disk, let alone the collector — and
  // the uploader sends nothing while consent is off, whatever the queue
  // holds. Withdrawing it is the end of the line for what is queued too:
  // a live opt-out gets ONE final pass (what was captured under consent,
  // the opt-out event itself last; no retry), then the queue and the spool
  // file are purged; consent found off at startup discards, unsent, any
  // spool the previous run left behind.
  void SetEnabled(bool enabled);

  // Queues one event. Safe from any thread, never blocks on the network: the
  // caller is the UI thread delivering a framework callback. A no-op while
  // unconfigured or while the user has not consented (SetEnabled), so
  // neither state writes behavioural data to disk.
  //
  // Properties are filtered against the per-module allow-list below BEFORE
  // the event is queued, so an unexpected key never reaches the spool file,
  // let alone the collector.
  void RecordEvent(const std::string& module,
                   const std::string& name,
                   const std::string& properties_json);

  // Stops the uploader and flushes the spool to disk. Best-effort: whatever
  // has not uploaded stays spooled for the next run.
  void Stop();

  // What the pipeline is doing, for the harness and for support:
  // {"configured":bool,"endpoint":"...","queued":n,"sent":n,"dropped":n,
  //  "failures":n,"lastStatus":n,"lastError":"..."}
  // The endpoint is reported, the KEY never is.
  std::string StatusJson();

  // The property keys `module` is allowed to attach, as a "|"-fenced list
  // ("|enabled|aggressiveness|"), or an EMPTY view when the module is not
  // registered — in which case its events carry no properties at all.
  //
  // WHY AN ALLOW-LIST AND NOT A DENY-LIST. Properties arrive from the
  // framework as an opaque JSON object (phi_framework_api.h:389-405) and are
  // forwarded verbatim today. The Chromium side's privacy contract — no
  // URLs, origins, titles or search terms in a property — is enforced at
  // REVIEW time by its own admission (analytics/README.md, "Privacy
  // contract"). That is a fine rule for the component and a poor one for the
  // egress: this client is where the bytes leave the machine, and a review
  // that happens in another repository cannot be the last gate. So the
  // egress names what it will carry, and drops the rest with a count.
  //
  // Adding a property is therefore two edits — the Chromium call site and
  // this table — which is the point: the table is the list a privacy review
  // can read without reading the browser.
  static std::string_view AllowedProperties(std::string_view module);

  // How many properties have been dropped for not being on the list. Read by
  // StatusJson and by the host test; a non-zero count in the field means the
  // two sides have drifted.
  uint64_t ScrubbedPropertyCount();

 private:
  // `properties_json` with every key not on `module`'s allow-list removed.
  // A payload that is not a JSON object at all becomes "{}".
  std::string ScrubProperties(const std::string& module,
                              const std::string& properties_json);

  PhiTelemetry();
  ~PhiTelemetry();

  struct Event {
    std::string module;
    std::string name;
    std::string properties_json;
    int64_t unix_ms = 0;
  };

  static DWORD WINAPI UploadThread(void* self);
  void RunUploadLoop();
  // Takes up to kBatchEvents off the front of the queue. Empty when idle,
  // and empty without consent (except during the opt-out's final pass).
  std::deque<Event> TakeBatch();
  // Puts a failed batch back at the FRONT, preserving capture order.
  void ReturnBatch(std::deque<Event> batch);
  // Consent is gone: drops `in_flight` undelivered events and the whole
  // queue, counted as dropped, and ends any final pass.
  void DiscardQueue(size_t in_flight);
  // The request body for one batch. THE ONE PLACE a collector's ingest shape
  // lives: a JSON object with a "batch" array of {module,event,properties,ts}.
  std::string BuildBatchBody(const std::deque<Event>& batch) const;
  bool PostBatch(const std::string& body, int* http_status, std::string* err);
  void LoadSpool();
  void WriteSpool();
  // Reads the install id beside the spool, generating and persisting one on
  // first run.
  void EnsureDistinctId(const std::wstring& user_data_dir);
  // The "$lib" / "$os" / version stamps every PostHog event carries.
  static base::DictValue SenderProperties();

  // Guards every member below except the thread handle and the config, which
  // are written once by Start() before the thread exists.
  CRITICAL_SECTION lock_;
  HANDLE wake_ = nullptr;    // Signalled on a new event and on Stop().
  HANDLE thread_ = nullptr;
  bool stopping_ = false;
  bool started_ = false;
  // The consent switch (SetEnabled). Defaults to no consent.
  bool enabled_ = false;
  // A live opt-out's final pass is under way: the uploader may still take
  // what was queued under consent, once, with no retry. Cleared when the
  // queue empties or a batch fails.
  bool draining_ = false;

  std::string endpoint_;   // Empty == unconfigured == the whole thing is off.
  std::string api_key_;
  // "posthog" shapes the batch for PostHog's capture API; anything else (the
  // default) sends the generic envelope. This is the ONLY vendor knowledge in
  // the pipeline — see BuildBatchBody.
  std::string format_;
  // Stable per install, generated once and kept beside the spool. PostHog
  // requires a distinct_id per event; anonymous-but-stable is the honest
  // default, and nothing here ever ties it to an account.
  std::string distinct_id_;
  // What every PostHog event carries about the sender ($lib, $os, the app
  // version and namespace): computed once in Start, cloned into each event
  // at send time. The Mac SDK stamps the same keys, and they are what tells
  // a Windows event from a Mac one in a shared insight.
  base::DictValue sender_props_;
  std::wstring spool_path_;

  std::deque<Event> queue_;
  uint64_t sent_ = 0;
  uint64_t dropped_ = 0;
  // Properties removed because the module did not declare them.
  uint64_t scrubbed_ = 0;
  uint64_t failures_ = 0;
  int last_status_ = 0;
  std::string last_error_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_TELEMETRY_H_

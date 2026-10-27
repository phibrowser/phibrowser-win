// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_telemetry.h"

#include <winhttp.h>

#include <objbase.h>

#include <iterator>
#include <optional>
#include <utility>
#include <vector>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "base/win/windows_version.h"
#include "phibrowser-win/client/phi_feedback_outbox.h"
#include "phibrowser-win/client/phi_version.h"

#pragma comment(lib, "winhttp.lib")

namespace phi_client {
namespace {

// One batch never grows past this; the rest waits for the next round.
constexpr size_t kBatchEvents = 64;
// The spool's bound. Older events are dropped first — recent behaviour is
// what has diagnostic value, and an unbounded spool on a machine that never
// reaches its collector is a slow disk leak.
constexpr size_t kMaxQueuedEvents = 2000;
// Idle wake-up. A batch that has been sitting goes out even with no new
// events behind it.
constexpr DWORD kIdleFlushMs = 30 * 1000;
// Backoff after a failed batch: doubles to the cap, resets on success.
constexpr DWORD kBackoffStartMs = 5 * 1000;
constexpr DWORD kBackoffMaxMs = 10 * 60 * 1000;

int64_t NowUnixMs() {
  FILETIME ft = {};
  ::GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER v = {};
  v.LowPart = ft.dwLowDateTime;
  v.HighPart = ft.dwHighDateTime;
  // FILETIME is 100ns ticks since 1601; 11644473600s to the Unix epoch.
  return static_cast<int64_t>(v.QuadPart / 10000ULL) - 11644473600000LL;
}

// Is `host` a literal address on this machine or this LAN? Only these may be
// reached over plain HTTP — see SplitEndpointUrl.
bool IsPrivateHost(const std::string& host) {
  if (host == "localhost") {
    return true;
  }
  // Hand-parsed rather than scanf'd: the build's unsafe-libc gate rules those
  // out, and four bounded integers are not worth a dependency.
  // Named locals, not an array: -Wunsafe-buffer-usage rejects the indexing,
  // and only the first two octets decide the range anyway.
  int first = 0;
  int second = 0;
  size_t start = 0;
  for (int i = 0; i < 4; ++i) {
    const size_t dot = host.find('.', start);
    const size_t end = (i == 3) ? host.size() : dot;
    if ((i < 3 && dot == std::string::npos) || end <= start ||
        end - start > 3) {
      return false;  // A NAME, not a literal: it could resolve anywhere.
    }
    int value = 0;
    for (size_t j = start; j < end; ++j) {
      if (host[j] < '0' || host[j] > '9') {
        return false;
      }
      value = value * 10 + (host[j] - '0');
    }
    if (value > 255) {
      return false;
    }
    if (i == 0) {
      first = value;
    } else if (i == 1) {
      second = value;
    }
    start = end + 1;
  }
  return first == 127 ||                                // loopback
         first == 10 ||                                 // 10/8
         (first == 172 && second >= 16 && second < 32) ||  // 172.16/12
         (first == 192 && second == 168);               // 192.168/16
}

// Splits an endpoint into host / port / path and says whether to use TLS.
//
// HTTPS IS THE RULE. Plain http is accepted for one case only: a LITERAL
// private or loopback address, which is a collector on this machine or this
// LAN — a self-hosted PostHog or Sentry during bring-up, where an IP has no
// certificate to be issued for and the traffic never leaves the network. A
// hostname is never eligible, however private it looks, because a name
// resolves wherever DNS says. Anything else returns false, which switches the
// pipeline off rather than quietly downgrading it.
bool SplitEndpointUrl(const std::string& url,
                      std::wstring* host,
                      std::wstring* path,
                      int* port,
                      bool* secure) {
  constexpr char kHttps[] = "https://";
  constexpr char kHttp[] = "http://";
  std::string rest;
  if (url.compare(0, sizeof(kHttps) - 1, kHttps) == 0) {
    *secure = true;
    rest = url.substr(sizeof(kHttps) - 1);
  } else if (url.compare(0, sizeof(kHttp) - 1, kHttp) == 0) {
    *secure = false;
    rest = url.substr(sizeof(kHttp) - 1);
  } else {
    return false;
  }
  if (rest.empty()) {
    return false;
  }
  const size_t slash = rest.find('/');
  std::string authority =
      slash == std::string::npos ? rest : rest.substr(0, slash);
  const std::string path_part =
      slash == std::string::npos ? std::string("/") : rest.substr(slash);
  *port = *secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
  const size_t colon = authority.find(':');
  if (colon != std::string::npos) {
    const std::string port_part = authority.substr(colon + 1);
    authority = authority.substr(0, colon);
    if (port_part.empty() ||
        port_part.find_first_not_of("0123456789") != std::string::npos) {
      return false;
    }
    const int parsed = std::atoi(port_part.c_str());
    if (parsed <= 0 || parsed > 65535) {
      return false;
    }
    *port = parsed;
  }
  if (authority.empty()) {
    return false;
  }
  if (!*secure && !IsPrivateHost(authority)) {
    return false;  // Cleartext off this machine's network: refused.
  }
  *host = base::UTF8ToWide(authority);
  *path = base::UTF8ToWide(path_part);
  return true;
}

// PostHog's capture API wants an ISO-8601 UTC timestamp.
std::string Iso8601Utc(int64_t unix_ms) {
  const int64_t ticks =
      (unix_ms + 11644473600000LL) * 10000LL;  // Back to FILETIME.
  ULARGE_INTEGER v = {};
  v.QuadPart = static_cast<ULONGLONG>(ticks);
  FILETIME ft = {};
  ft.dwLowDateTime = v.LowPart;
  ft.dwHighDateTime = v.HighPart;
  SYSTEMTIME st = {};
  if (!::FileTimeToSystemTime(&ft, &st)) {
    return std::string();
  }
  return base::StringPrintf("%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", st.wYear,
                           st.wMonth, st.wDay, st.wHour, st.wMinute,
                           st.wSecond, st.wMilliseconds);
}

std::wstring ConfigPath(const std::wstring& user_data_dir) {
  return user_data_dir + L"\\phi_telemetry.json";
}

std::string ReadFileUtf8(const std::wstring& path) {
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return std::string();
  }
  std::string out;
  char chunk[8192];
  DWORD read = 0;
  while (::ReadFile(file, chunk, sizeof(chunk), &read, nullptr) && read > 0) {
    out.append(chunk, read);
    if (out.size() > 8u * 1024 * 1024) {
      break;  // A spool this large is corrupt; take what we have.
    }
  }
  ::CloseHandle(file);
  return out;
}

// Atomic replace, the same shape phi_tab_store uses: write a temp beside the
// target, then MoveFileEx over it, so a crash mid-write cannot truncate the
// spool it was rewriting.
bool WriteFileAtomic(const std::wstring& path, const std::string& contents) {
  const std::wstring temp = path + L".tmp";
  HANDLE file = ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  const BOOL ok = ::WriteFile(file, contents.data(),
                              static_cast<DWORD>(contents.size()), &written,
                              nullptr);
  ::CloseHandle(file);
  if (!ok || written != contents.size()) {
    ::DeleteFileW(temp.c_str());
    return false;
  }
  return ::MoveFileExW(temp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
         0;
}

}  // namespace

PhiTelemetry& PhiTelemetry::Get() {
  // Process-lifetime, deliberately never destroyed: the upload thread may be
  // mid-request while the process tears down, and a destructor racing that is
  // worse than leaking one object.
  static PhiTelemetry* instance = new PhiTelemetry();
  return *instance;
}

PhiTelemetry::PhiTelemetry() {
  ::InitializeCriticalSection(&lock_);
  wake_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

PhiTelemetry::~PhiTelemetry() = default;

void PhiTelemetry::Start(const std::wstring& user_data_dir) {
  ::EnterCriticalSection(&lock_);
  const bool already = started_;
  started_ = true;
  ::LeaveCriticalSection(&lock_);
  if (already || user_data_dir.empty()) {
    return;
  }

  const std::string config = ReadFileUtf8(ConfigPath(user_data_dir));
  std::string endpoint;
  std::string key;
  std::string format;
  if (!config.empty()) {
    if (std::optional<base::DictValue> parsed =
            base::JSONReader::ReadDict(config, base::JSON_PARSE_RFC)) {
      if (const std::string* value = parsed->FindString("analytics_endpoint")) {
        endpoint = *value;
      }
      if (const std::string* value = parsed->FindString("analytics_key")) {
        key = *value;
      }
      if (const std::string* value = parsed->FindString("analytics_format")) {
        format = *value;
      }
    } else {
      LOG(ERROR) << "[telemetry] phi_telemetry.json is not valid JSON; "
                 << "the pipeline stays off.";
    }
  }
  std::wstring host;
  std::wstring path;
  int port = 0;
  bool secure = false;
  if (!endpoint.empty() &&
      !SplitEndpointUrl(endpoint, &host, &path, &port, &secure)) {
    LOG(ERROR) << "[telemetry] analytics_endpoint must be https, or http to a "
               << "literal private address; the pipeline stays off.";
    endpoint.clear();
  }
  if (endpoint.empty()) {
    // Unconfigured is the norm until a collector is chosen. Say so once, at
    // a level that does not shout in a normal session.
    VLOG(1) << "[telemetry] no analytics endpoint configured; events are "
            << "dropped after the framework hands them over.";
    return;
  }

  ::EnterCriticalSection(&lock_);
  endpoint_ = endpoint;
  api_key_ = key;
  format_ = format;
  spool_path_ = user_data_dir + L"\\phi_telemetry_spool.json";
  sender_props_ = SenderProperties();
  ::LeaveCriticalSection(&lock_);

  EnsureDistinctId(user_data_dir);
  LoadSpool();
  thread_ = ::CreateThread(nullptr, 0, &PhiTelemetry::UploadThread, this, 0,
                           nullptr);
  LOG(INFO) << "[telemetry] uploader started for " << endpoint;
}

void PhiTelemetry::SetEnabled(bool enabled) {
  bool drain = false;
  size_t discarded = 0;
  ::EnterCriticalSection(&lock_);
  const bool changed = enabled_ != enabled;
  enabled_ = enabled;
  if (!enabled) {
    if (changed && !queue_.empty()) {
      // A live opt-out. Everything queued was captured under consent, and
      // the last of it is the opt-out itself (metrics_reporting_changed is
      // captured before the switch moves): the uploader sends it in one
      // pass, no retry, and purges whatever does not go. From here on
      // RecordEvent drops, TakeBatch takes nothing, WriteSpool deletes.
      draining_ = true;
      drain = true;
    } else if (!changed) {
      // Consent is off and has not been on this run. The queue is the
      // backlog LoadSpool reloaded from the previous run — captured under
      // a consent the user has since withdrawn (or a final pass a crash
      // cut short). Their answer today is no: it goes unsent.
      discarded = queue_.size();
      dropped_ += discarded;
      queue_.clear();
      draining_ = false;
    }
  }
  ::LeaveCriticalSection(&lock_);
  if (changed) {
    LOG(INFO) << "[telemetry] analytics " << (enabled ? "enabled" : "disabled")
              << " by the user";
  }
  if (discarded) {
    LOG(INFO) << "[telemetry] discarded " << discarded
              << " spooled events unsent; consent is off.";
  }
  if (!enabled && !drain) {
    WriteSpool();  // Deletes the file; the uploader does so after a pass.
  }
  if (drain && wake_) {
    ::SetEvent(wake_);
  }
}

std::string_view PhiTelemetry::AllowedProperties(std::string_view module) {
  // Two registries, one table. The Chromium-originated modules are
  // chrome/browser/phinomenon/analytics/README.md; the client's own are
  // docs/analytics.md in this repository (built in phi_client_analytics.cc).
  // A row there and a fence here are added in the same change. The fences
  // are "|"-delimited so a lookup is one substring search with no allocation
  // and no partial-name match ("count" cannot pass as "zombie_count").
  if (module == "memory_saver") {
    return "|enabled|aggressiveness|";
  }
  if (module == "renderer_health") {
    return "|age_bucket|cpu_percent|process_uptime_minutes|zombie_count|"
           "renderer_count|";
  }
  if (module == "app") {
    return "|from_background|version|build|previous_version|layout_mode|"
           "ai_enabled|is_guest_mode|action|seconds_since_install|";
  }
  if (module == "settings") {
    return "|new_tab_behavior|layout_mode|app_language|appearance|"
           "is_phi_default_browser|peek_view_enabled|always_show_full_url|"
           "show_tab_previews|show_open_tab_indicators|"
           "dim_unloaded_tab_icons|restore_last_session_enabled|"
           "developer_mode_enabled|cdp_agent_access_enabled|"
           "agent_user_spaces_enabled|always_show_bookmark_bar|"
           "open_kiosk_on_command_option_click|auto_picture_in_picture_mode|"
           "enabled|from|to|";
  }
  if (module == "spaces") {
    return "|total_spaces|non_default_profile|total_profiles|surface|";
  }
  if (module == "profiles") {
    return "|total_profiles|";
  }
  if (module == "agent") {
    return "|origin|persistent|agent_name|success|command|incognito|kind|"
           "approved|prompted|approval_type|";
  }
  if (module == "peek") {
    return "|entry_point|opener_kind|reason|duration_seconds|enabled|";
  }
  if (module == "reader") {
    return "|entry_point|reason|";
  }
  if (module == "bookmarks") {
    return "||";  // Both events carry no properties at all.
  }
  if (module == "kiosk") {
    return "|total_spaces|";
  }
  if (module == "oobe") {
    return "|step|step_index|is_guest|duration_seconds|steps_completed|"
           "total_duration_seconds|last_step|reason|";
  }
  if (module == "account") {
    return "|trigger|";
  }
  if (module == "import") {
    return "|entry_point|source_browser|types|source_browsers|success|"
           "failed_sources|duration_seconds|error_code|";
  }
  if (module == "feature") {
    return "|button|surface|";
  }
  if (module == "notifications") {
    return "|action|";
  }
  if (module == "library") {
    return "|section|is_standalone_window|";
  }
  return std::string_view();
}

base::DictValue PhiTelemetry::SenderProperties() {
  // PostHog's own keys, as its SDKs set them, so the Activity view's
  // Library column and the OS / version breakdowns work for this client the
  // way they do for the Mac's. The namespace carries the channel the way
  // the Mac bundle id does (".canary." in it for a canary build), which is
  // how docs/analytics.md there splits the channels in dashboards.
  base::DictValue props;
  props.Set("$lib", "phibrowser-win");
  props.Set("$lib_version", PHI_WIN_VERSION_FULL_STRING);
  props.Set("$app_version", PHI_WIN_VERSION_FULL_STRING);
  props.Set("$app_build", PHI_WIN_VERSION_BUILD);
  // The channel the feedback form reports ("canary", or "debug" for a
  // DCHECK build), spelled into the namespace the way the Mac bundle id
  // carries it; the stable channel is the bare name.
  const std::string channel = PhiFeedbackOutbox::BrowserChannelName();
  props.Set("$app_namespace", channel == "stable"
                                  ? "com.phibrowser.win"
                                  : "com.phibrowser." + channel + ".win");
  props.Set("$os", "Windows");
  const base::win::OSInfo::VersionNumber& v =
      base::win::OSInfo::GetInstance()->version_number();
  props.Set("$os_version", base::StringPrintf("%u.%u.%u", v.major, v.minor,
                                              v.build));
  props.Set("$device_type", "Desktop");
  return props;
}

std::string PhiTelemetry::ScrubProperties(const std::string& module,
                                          const std::string& properties_json) {
  const std::string_view allowed = AllowedProperties(module);
  if (allowed.empty()) {
    // An unregistered module sends its name and its timing and nothing else.
    // It is still counted and still uploaded: knowing an unknown module is
    // firing is the signal that says the table needs a row.
    return "{}";
  }
  std::optional<base::DictValue> props =
      base::JSONReader::ReadDict(properties_json, base::JSON_PARSE_RFC);
  if (!props) {
    return "{}";
  }
  base::DictValue kept;
  for (const auto [key, value] : *props) {
    const std::string fenced = "|" + std::string(key) + "|";
    if (allowed.find(fenced) == std::string_view::npos) {
      ++scrubbed_;
      continue;
    }
    kept.Set(key, value.Clone());
  }
  std::optional<std::string> json = base::WriteJson(kept);
  return json ? *json : std::string("{}");
}

uint64_t PhiTelemetry::ScrubbedPropertyCount() {
  ::EnterCriticalSection(&lock_);
  const uint64_t count = scrubbed_;
  ::LeaveCriticalSection(&lock_);
  return count;
}

void PhiTelemetry::RecordEvent(const std::string& module,
                               const std::string& name,
                               const std::string& properties_json) {
  ::EnterCriticalSection(&lock_);
  // Two gates, both required: a collector is configured AND the user
  // consented. Without consent the event is dropped here — never queued, so
  // it cannot reach the spool file either.
  const bool configured = !endpoint_.empty() && enabled_;
  if (configured) {
    Event event;
    event.module = module;
    event.name = name;
    // The allow-list runs HERE, before the event is queued: an unexpected
    // property must not reach the spool on disk, because the spool is
    // exactly what a later crash-time inspection or a support bundle would
    // pick up.
    event.properties_json =
        properties_json.empty() ? std::string("{}")
                                : ScrubProperties(module, properties_json);
    event.unix_ms = NowUnixMs();
    queue_.push_back(std::move(event));
    while (queue_.size() > kMaxQueuedEvents) {
      queue_.pop_front();
      ++dropped_;
    }
  }
  ::LeaveCriticalSection(&lock_);
  if (configured && wake_) {
    ::SetEvent(wake_);
  }
}

void PhiTelemetry::Stop() {
  HANDLE thread = nullptr;
  ::EnterCriticalSection(&lock_);
  stopping_ = true;
  thread = thread_;
  thread_ = nullptr;
  ::LeaveCriticalSection(&lock_);
  if (wake_) {
    ::SetEvent(wake_);
  }
  if (thread) {
    // Bounded: a request in flight must not hold up the user's shutdown.
    ::WaitForSingleObject(thread, 2000);
    ::CloseHandle(thread);
  }
  WriteSpool();
}

std::string PhiTelemetry::StatusJson() {
  base::DictValue out;
  ::EnterCriticalSection(&lock_);
  out.Set("configured", !endpoint_.empty());
  out.Set("enabled", enabled_);  // The user's consent switch.
  out.Set("scrubbedProperties", static_cast<double>(scrubbed_));
  out.Set("endpoint", endpoint_);  // The key is deliberately not reported.
  out.Set("queued", static_cast<int>(queue_.size()));
  out.Set("sent", static_cast<double>(sent_));
  out.Set("dropped", static_cast<double>(dropped_));
  out.Set("failures", static_cast<double>(failures_));
  out.Set("lastStatus", last_status_);
  out.Set("lastError", last_error_);
  ::LeaveCriticalSection(&lock_);
  std::optional<std::string> json = base::WriteJson(out);
  return json ? *json : std::string("{}");
}

DWORD WINAPI PhiTelemetry::UploadThread(void* self) {
  static_cast<PhiTelemetry*>(self)->RunUploadLoop();
  return 0;
}

void PhiTelemetry::RunUploadLoop() {
  DWORD backoff = 0;
  for (;;) {
    ::WaitForSingleObject(wake_, backoff ? backoff : kIdleFlushMs);
    ::EnterCriticalSection(&lock_);
    const bool stopping = stopping_;
    ::LeaveCriticalSection(&lock_);
    if (stopping) {
      return;
    }
    std::deque<Event> batch = TakeBatch();
    if (batch.empty()) {
      backoff = 0;
      continue;
    }
    const std::string body = BuildBatchBody(batch);
    int status = 0;
    std::string error;
    const bool ok = PostBatch(body, &status, &error);
    // 4xx is the collector rejecting THIS payload; retrying it forever would
    // wedge the queue behind a poison batch, so it is dropped and counted.
    // Transport failures and 5xx are retried.
    const bool permanent = !ok && status >= 400 && status < 500;
    ::EnterCriticalSection(&lock_);
    last_status_ = status;
    last_error_ = error;
    const bool consented = enabled_;
    if (ok) {
      sent_ += batch.size();
    } else {
      ++failures_;
      if (permanent) {
        dropped_ += batch.size();
      }
    }
    ::LeaveCriticalSection(&lock_);
    if (ok) {
      backoff = 0;
      WriteSpool();
      continue;
    }
    if (permanent) {
      LOG(ERROR) << "[telemetry] collector rejected a batch (" << status
                 << "); dropping " << batch.size() << " events.";
      backoff = 0;
      WriteSpool();
      continue;
    }
    if (!consented) {
      // Consent went away while this batch was out (or this is the
      // opt-out's final pass): there is no retry to schedule. What could
      // not be delivered now is not kept for later — not in memory, not on
      // disk.
      DiscardQueue(batch.size());
      WriteSpool();
      backoff = 0;
      continue;
    }
    ReturnBatch(std::move(batch));
    WriteSpool();
    backoff = backoff ? (backoff * 2 > kBackoffMaxMs ? kBackoffMaxMs
                                                     : backoff * 2)
                      : kBackoffStartMs;
  }
}

std::deque<PhiTelemetry::Event> PhiTelemetry::TakeBatch() {
  std::deque<Event> batch;
  ::EnterCriticalSection(&lock_);
  // Nothing leaves without consent. The queue is not empty in that state
  // only between Start (which reloads the spool) and the settings arriving
  // (SetEnabled): that backlog waits for a yes and is discarded on a no.
  // The one exception is the opt-out's final pass.
  if (enabled_ || draining_) {
    while (!queue_.empty() && batch.size() < kBatchEvents) {
      batch.push_back(std::move(queue_.front()));
      queue_.pop_front();
    }
  }
  if (queue_.empty()) {
    draining_ = false;  // The pass took its last batch; nothing follows.
  }
  ::LeaveCriticalSection(&lock_);
  return batch;
}

void PhiTelemetry::DiscardQueue(size_t in_flight) {
  ::EnterCriticalSection(&lock_);
  dropped_ += in_flight + queue_.size();
  queue_.clear();
  draining_ = false;
  ::LeaveCriticalSection(&lock_);
}

void PhiTelemetry::ReturnBatch(std::deque<Event> batch) {
  ::EnterCriticalSection(&lock_);
  while (!batch.empty()) {
    queue_.push_front(std::move(batch.back()));
    batch.pop_back();
  }
  while (queue_.size() > kMaxQueuedEvents) {
    queue_.pop_front();
    ++dropped_;
  }
  ::LeaveCriticalSection(&lock_);
}

std::string PhiTelemetry::BuildBatchBody(const std::deque<Event>& batch) const {
  // THE ONE PLACE a collector's ingest shape lives. The generic envelope is
  // also the SPOOL's on-disk format, so LoadSpool can read back what this
  // writes — a vendor shape is built for the wire only, at send time.
  const bool posthog = format_ == "posthog";
  base::ListValue rows;
  for (const Event& event : batch) {
    base::DictValue row;
    std::optional<base::Value> props = base::JSONReader::Read(
        event.properties_json, base::JSON_PARSE_RFC);
    if (posthog) {
      // PostHog's capture API: one object per event, distinct_id inside
      // properties, ISO-8601 timestamp. The module is not a PostHog concept,
      // so it rides along as a property rather than being dropped.
      // The sender stamps go on first so an event's own key of the same
      // name (none today; the allow-list has no "$" keys) would win.
      base::DictValue properties = sender_props_.Clone();
      if (props && props->is_dict()) {
        properties.Merge(std::move(*props).TakeDict());
      }
      properties.Set("distinct_id", distinct_id_);
      properties.Set("phi_module", event.module);
      row.Set("event", event.name);
      row.Set("properties", std::move(properties));
      row.Set("timestamp", Iso8601Utc(event.unix_ms));
    } else {
      row.Set("module", event.module);
      row.Set("event", event.name);
      row.Set("ts", static_cast<double>(event.unix_ms));
      if (props) {
        row.Set("properties", std::move(*props));
      }
    }
    rows.Append(std::move(row));
  }
  base::DictValue body;
  if (posthog) {
    body.Set("api_key", api_key_);
  }
  body.Set("batch", std::move(rows));
  std::optional<std::string> json = base::WriteJson(body);
  return json ? *json : std::string("{\"batch\":[]}");
}

void PhiTelemetry::EnsureDistinctId(const std::wstring& user_data_dir) {
  const std::wstring path = user_data_dir + L"\\phi_telemetry_id";
  std::string id = ReadFileUtf8(path);
  while (!id.empty() && (id.back() == '\n' || id.back() == '\r' ||
                         id.back() == ' ')) {
    id.pop_back();
  }
  if (id.empty()) {
    GUID guid = {};
    if (::CoCreateGuid(&guid) == S_OK) {
      wchar_t buffer[40] = {};
      ::StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
      id = base::WideToUTF8(buffer);
    }
    if (id.empty()) {
      return;  // No id, no PostHog batch; the generic envelope is unaffected.
    }
    WriteFileAtomic(path, id);
  }
  ::EnterCriticalSection(&lock_);
  distinct_id_ = id;
  ::LeaveCriticalSection(&lock_);
}

bool PhiTelemetry::PostBatch(const std::string& body,
                             int* http_status,
                             std::string* err) {
  std::string endpoint;
  std::string key;
  ::EnterCriticalSection(&lock_);
  endpoint = endpoint_;
  key = api_key_;
  ::LeaveCriticalSection(&lock_);

  std::wstring host;
  std::wstring path;
  int port = 0;
  bool secure = false;
  if (!SplitEndpointUrl(endpoint, &host, &path, &port, &secure)) {
    *err = "bad endpoint";
    return false;
  }
  HINTERNET session = ::WinHttpOpen(L"PhiBrowser-Win",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    *err = "WinHttpOpen failed";
    return false;
  }
  bool ok = false;
  HINTERNET connect = ::WinHttpConnect(session, host.c_str(),
                                       static_cast<INTERNET_PORT>(port), 0);
  if (connect) {
    HINTERNET request = ::WinHttpOpenRequest(
        connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (request) {
      std::wstring headers = L"Content-Type: application/json";
      if (!key.empty()) {
        headers += L"\r\nAuthorization: Bearer " + base::UTF8ToWide(key);
      }
      if (::WinHttpSendRequest(request, headers.c_str(),
                               static_cast<DWORD>(-1),
                               const_cast<char*>(body.data()),
                               static_cast<DWORD>(body.size()),
                               static_cast<DWORD>(body.size()), 0) &&
          ::WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD size = sizeof(status);
        ::WinHttpQueryHeaders(
            request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        *http_status = static_cast<int>(status);
        ok = status >= 200 && status < 300;
        if (!ok) {
          *err = "http " + std::to_string(status);
        }
      } else {
        *err = "send failed (" + std::to_string(::GetLastError()) + ")";
      }
      ::WinHttpCloseHandle(request);
    } else {
      *err = "WinHttpOpenRequest failed";
    }
    ::WinHttpCloseHandle(connect);
  } else {
    *err = "WinHttpConnect failed";
  }
  ::WinHttpCloseHandle(session);
  if (ok) {
    err->clear();
  }
  return ok;
}

void PhiTelemetry::LoadSpool() {
  std::wstring path;
  ::EnterCriticalSection(&lock_);
  path = spool_path_;
  ::LeaveCriticalSection(&lock_);
  if (path.empty()) {
    return;
  }
  const std::string contents = ReadFileUtf8(path);
  if (contents.empty()) {
    return;
  }
  std::optional<base::DictValue> parsed =
      base::JSONReader::ReadDict(contents, base::JSON_PARSE_RFC);
  const base::ListValue* rows = parsed ? parsed->FindList("batch") : nullptr;
  if (!rows) {
    return;
  }
  ::EnterCriticalSection(&lock_);
  for (const base::Value& value : *rows) {
    const base::DictValue* row = value.GetIfDict();
    if (!row) {
      continue;
    }
    Event event;
    if (const std::string* module = row->FindString("module")) {
      event.module = *module;
    }
    if (const std::string* name = row->FindString("event")) {
      event.name = *name;
    }
    event.unix_ms = static_cast<int64_t>(row->FindDouble("ts").value_or(0));
    if (const base::Value* props = row->Find("properties")) {
      // The file is outside our control between runs (an older build, an
      // edit, a narrowed table): re-apply today's allow-list, as RecordEvent
      // does, before anything from it can reach the collector.
      std::optional<std::string> json = base::WriteJson(*props);
      event.properties_json =
          json ? ScrubProperties(event.module, *json) : std::string("{}");
    } else {
      event.properties_json = "{}";
    }
    if (event.name.empty()) {
      continue;
    }
    queue_.push_back(std::move(event));
  }
  while (queue_.size() > kMaxQueuedEvents) {
    queue_.pop_front();
    ++dropped_;
  }
  const size_t restored = queue_.size();
  ::LeaveCriticalSection(&lock_);
  if (restored) {
    LOG(INFO) << "[telemetry] reloaded " << restored
              << " events left by the previous run.";
  }
}

void PhiTelemetry::WriteSpool() {
  std::wstring path;
  std::deque<Event> snapshot;
  ::EnterCriticalSection(&lock_);
  path = spool_path_;
  // The spool exists only under consent. Without it the queue is empty
  // anyway (SetEnabled purged it), except during a final pass, whose
  // remainder is deliberately never written down, and in the window before
  // the settings are read — a quit inside it loses the reloaded backlog,
  // which is the right way round to lose.
  if (enabled_) {
    snapshot = queue_;
  }
  ::LeaveCriticalSection(&lock_);
  if (path.empty()) {
    return;
  }
  if (snapshot.empty()) {
    ::DeleteFileW(path.c_str());
    return;
  }
  // The spool is always the generic envelope — LoadSpool reads this back, and
  // a vendor's wire shape is not a storage format.
  base::ListValue rows;
  for (const Event& event : snapshot) {
    base::DictValue row;
    row.Set("module", event.module);
    row.Set("event", event.name);
    row.Set("ts", static_cast<double>(event.unix_ms));
    if (std::optional<base::Value> props = base::JSONReader::Read(
            event.properties_json, base::JSON_PARSE_RFC)) {
      row.Set("properties", std::move(*props));
    }
    rows.Append(std::move(row));
  }
  base::DictValue body;
  body.Set("batch", std::move(rows));
  if (std::optional<std::string> json = base::WriteJson(body)) {
    WriteFileAtomic(path, *json);
  }
}

}  // namespace phi_client

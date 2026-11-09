// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_sync_invalidation_win.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "client/phi_http.h"

#pragma comment(lib, "winhttp.lib")

namespace phi_client {

namespace {

constexpr int kReadWatchdogMs = 45000;
// A connection resets the backoff only after staying healthy this long: a
// server that sends `ready` and closes must not get a reconnect (and a full
// catch-up on every client) each second.
constexpr auto kHealthyResetAfter = std::chrono::seconds(60);

std::wstring QueryHeader(HINTERNET request, DWORD query) {
  DWORD size = 0;
  ::WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                        &size, WINHTTP_NO_HEADER_INDEX);
  if (size == 0) {
    return std::wstring();
  }
  std::wstring value(size / sizeof(wchar_t) + 1, L'\0');
  if (!::WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX,
                             value.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
    return std::wstring();
  }
  value.resize(size / sizeof(wchar_t));
  return value;
}

}  // namespace

PhiSyncInvalidationStream::PhiSyncInvalidationStream(std::string base_url,
                                                     Callbacks callbacks)
    : base_url_(std::move(base_url)), callbacks_(std::move(callbacks)) {}

PhiSyncInvalidationStream::~PhiSyncInvalidationStream() {
  Stop();
}

void PhiSyncInvalidationStream::Start() {
  if (thread_.joinable()) {
    return;
  }
  stop_ = false;
  thread_ = std::thread([this] { Run(); });
}

void PhiSyncInvalidationStream::Stop() {
  stop_ = true;
  {
    std::lock_guard<std::mutex> hold(lock_);
    if (request_) {
      ::WinHttpCloseHandle(request_);  // cancels a blocking read
      request_ = nullptr;
    }
  }
  wake_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void PhiSyncInvalidationStream::SetHealthy(bool healthy) {
  if (healthy_ == healthy) {
    return;
  }
  healthy_ = healthy;
  if (callbacks_.on_health) {
    callbacks_.on_health(healthy);
  }
}

void PhiSyncInvalidationStream::Run() {
  std::mt19937 random(std::random_device{}());
  int failures = 0;
  while (!stop_) {
    const bool was_healthy = Connect();
    SetHealthy(false);
    if (stop_) {
      return;
    }
    failures = was_healthy ? 0 : std::min(failures + 1, 6);
    // min(60, 2^failures) s, jittered to 80–100 %.
    const double base = std::min(60.0, static_cast<double>(1 << failures));
    std::uniform_real_distribution<double> jitter(0.8, 1.0);
    const auto delay =
        std::chrono::milliseconds(static_cast<int>(base * jitter(random) * 1000));
    std::unique_lock<std::mutex> hold(lock_);
    wake_.wait_for(hold, delay, [this] { return stop_.load(); });
  }
}

bool PhiSyncInvalidationStream::Connect() {
  const std::string device_id =
      callbacks_.device_id ? callbacks_.device_id() : std::string();
  const std::string token = callbacks_.token ? callbacks_.token() : std::string();
  if (device_id.empty() || token.empty() || stop_) {
    LOG(INFO) << "[PhiSync] invalidation stream: no "
              << (device_id.empty() ? "device id" : "token");
    return false;
  }
  std::string base = base_url_;
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  std::wstring host;
  std::wstring path;
  int port = 0;
  bool secure = false;
  if (!PhiSplitUrl(base + "/sync/invalidations?client_id=" + device_id, &host,
                   &path, &port, &secure) ||
      (!secure && host != L"localhost" && host != L"127.0.0.1")) {
    return false;
  }
  HINTERNET session = ::WinHttpOpen(L"PhiBrowser-Win", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    return false;
  }
  ::WinHttpSetTimeouts(session, 30000, 30000, 30000, kReadWatchdogMs);
  HINTERNET connect = ::WinHttpConnect(session, host.c_str(),
                                       static_cast<INTERNET_PORT>(port), 0);
  HINTERNET request =
      connect ? ::WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     secure ? WINHTTP_FLAG_SECURE : 0)
              : nullptr;
  bool was_healthy = false;
  std::chrono::steady_clock::time_point ready_at;
  if (request) {
    {
      std::lock_guard<std::mutex> hold(lock_);
      if (stop_) {
        ::WinHttpCloseHandle(request);
        request = nullptr;
      } else {
        request_ = request;
      }
    }
  }
  if (request) {
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    ::WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirects,
                       sizeof(redirects));
    const std::wstring headers = L"Authorization: Bearer " + base::UTF8ToWide(token) +
                                 L"\r\nAccept: text/event-stream\r\nCache-Control: no-cache";
    const bool sent =
        ::WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1),
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        ::WinHttpReceiveResponse(request, nullptr);
    if (!sent && !stop_) {
      LOG(INFO) << "[PhiSync] invalidation stream: request failed err="
                << ::GetLastError();
    }
    if (sent) {
      DWORD status = 0;
      DWORD size = sizeof(status);
      ::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
      std::wstring type = QueryHeader(request, WINHTTP_QUERY_CONTENT_TYPE);
      type = type.substr(0, type.find(L';'));
      while (!type.empty() && type.back() == L' ') {
        type.pop_back();
      }
      std::transform(type.begin(), type.end(), type.begin(), ::towlower);
      LOG(INFO) << "[PhiSync] invalidation stream status=" << status
                << " type=" << base::WideToUTF8(type);
      if (status == 401 && callbacks_.invalidate_token) {
        callbacks_.invalidate_token();
      }
      if (status == 200 && type == L"text/event-stream") {
        sync::InvalidationParser parser;
        std::vector<char> buffer(4096);
        for (;;) {
          // Blocks until some bytes arrived (a read of a full buffer would
          // wait for 4 KB of a trickling stream), then takes exactly those.
          DWORD available = 0;
          if (stop_ || !::WinHttpQueryDataAvailable(request, &available) ||
              available == 0) {
            break;  // a closed stream, the watchdog, or Stop()
          }
          DWORD read = 0;
          if (!::WinHttpReadData(request, buffer.data(),
                                 std::min<DWORD>(available,
                                                 static_cast<DWORD>(buffer.size())),
                                 &read) ||
              read == 0) {
            break;
          }
          std::vector<sync::InvalidationEvent> events;
          if (!parser.Feed(std::string_view(buffer.data(), read), &events)) {
            LOG(WARNING) << "[PhiSync] invalidation stream: malformed event";
            break;  // malformed: reconnect
          }
          bool dropped = false;
          for (const sync::InvalidationEvent& event : events) {
            if (event.kind == sync::InvalidationEvent::Kind::kReady) {
              LOG(INFO) << "[PhiSync] invalidation stream ready";
              if (!was_healthy) {
                ready_at = std::chrono::steady_clock::now();
              }
              was_healthy = true;
              SetHealthy(true);
              if (callbacks_.on_ready) {
                callbacks_.on_ready();
              }
            } else if (event.kind == sync::InvalidationEvent::Kind::kInvalidate) {
              if (!healthy_) {
                dropped = true;  // a hint before ready is a protocol error
                break;
              }
              VLOG(1) << "[PhiSync] invalidation hint " << event.hint.name_space;
              if (callbacks_.on_hint) {
                callbacks_.on_hint(event.hint);
              }
            }
          }
          if (dropped) {
            break;
          }
        }
      }
    }
    std::lock_guard<std::mutex> hold(lock_);
    if (request_) {
      ::WinHttpCloseHandle(request_);
      request_ = nullptr;
    }
  }
  if (connect) {
    ::WinHttpCloseHandle(connect);
  }
  ::WinHttpCloseHandle(session);
  return was_healthy &&
         std::chrono::steady_clock::now() - ready_at >= kHealthyResetAfter;
}

}  // namespace phi_client

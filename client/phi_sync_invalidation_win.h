// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_INVALIDATION_WIN_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_INVALIDATION_WIN_H_

#include <windows.h>

#include <winhttp.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_sync_invalidation.h"

namespace phi_client {

// The invalidation stream's connection (the Mac's
// PhiSyncInvalidationHTTPStream + the coordinator's reconnect loop): one
// long GET on its own thread, a 45 s read watchdog, capped exponential
// backoff with jitter, reconnect after any error. Every callback runs on the
// stream thread.
class PhiSyncInvalidationStream {
 public:
  struct Callbacks {
    std::function<std::string()> token;  // blocking; "" = none
    std::function<void()> invalidate_token;
    std::function<std::string()> device_id;
    std::function<void()> on_ready;  // the stream is healthy: catch up
    std::function<void(const sync::InvalidationHint&)> on_hint;
    std::function<void(bool healthy)> on_health;
  };

  PhiSyncInvalidationStream(std::string base_url, Callbacks callbacks);
  ~PhiSyncInvalidationStream();

  PhiSyncInvalidationStream(const PhiSyncInvalidationStream&) = delete;
  PhiSyncInvalidationStream& operator=(const PhiSyncInvalidationStream&) = delete;

  void Start();
  // Cancels the request in flight and joins the thread.
  void Stop();

 private:
  void Run();
  // One connection; true when it stayed healthy for a minute after `ready`.
  bool Connect();
  void SetHealthy(bool healthy);

  const std::string base_url_;
  const Callbacks callbacks_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  bool healthy_ = false;
  std::mutex lock_;
  std::condition_variable wake_;
  // The request in flight, closed by Stop() to cancel a blocking read.
  RAW_PTR_EXCLUSION HINTERNET request_ = nullptr;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_INVALIDATION_WIN_H_

// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_LOGGING_LOGGING_SETTINGS_H_
#define BASE_LOGGING_LOGGING_SETTINGS_H_

#include <cstdint>
#include <string>

namespace logging {

enum LoggingDestination : uint32_t {
  LOG_NONE = 0,
  LOG_TO_FILE = 1 << 0,
  LOG_TO_SYSTEM_DEBUG_LOG = 1 << 1,
  LOG_TO_STDERR = 1 << 2,
};
enum LogLockingState { LOCK_LOG_FILE, DONT_LOCK_LOG_FILE };
enum OldFileDeletionState { DELETE_OLD_LOG_FILE, APPEND_TO_OLD_LOG_FILE };

struct LoggingSettings {
  uint32_t logging_dest = LOG_NONE;
  std::wstring log_file_path;
  LogLockingState lock_log = LOCK_LOG_FILE;
  OldFileDeletionState delete_old = APPEND_TO_OLD_LOG_FILE;
};

bool InitLogging(const LoggingSettings& settings);
void SetLogItems(bool enable_process_id,
                 bool enable_thread_id,
                 bool enable_timestamp,
                 bool enable_tickcount);

}  // namespace logging

#endif  // BASE_LOGGING_LOGGING_SETTINGS_H_

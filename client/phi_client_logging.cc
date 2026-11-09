// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_client_logging.h"

#include <string>

#include "base/command_line.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/logging/logging_settings.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/common/env_vars.h"
#include "content/public/common/content_switches.h"

namespace phi_client {

namespace {

// Mirrors chrome/common/logging_chrome.cc's DetermineLoggingDestination for
// the BROWSER process of a release (NDEBUG, logging-off-by-default) build.
uint32_t BrowserLoggingDestination(const base::CommandLine& command_line) {
  if (!command_line.HasSwitch(switches::kEnableLogging)) {
    return logging::LOG_NONE;
  }
  const std::string destination =
      command_line.GetSwitchValueASCII(switches::kEnableLogging);
  if (destination == "stderr") {
    return logging::LOG_TO_SYSTEM_DEBUG_LOG | logging::LOG_TO_STDERR;
  }
  return logging::LOG_TO_FILE;
}

// Mirrors GetLogFileName: --log-file, then $CHROME_LOG_FILE (absolute paths
// only — the exe has cd'd to chrome.dll's directory by now), else the user
// data dir's chrome_debug.log.
base::FilePath BrowserLogFile(const base::CommandLine& command_line,
                              const std::string& user_data_dir_utf8) {
  std::wstring name = command_line.GetSwitchValueNative(switches::kLogFile);
  if (name.empty()) {
    name = base::UTF8ToWide(
        base::Environment::Create()->GetVar(env_vars::kLogFileName).value_or(
            ""));
  }
  if (!name.empty()) {
    base::FilePath candidate = base::FilePath(name).NormalizePathSeparators();
    if (candidate.IsAbsolute()) {
      return candidate;
    }
  }
  return base::FilePath(base::UTF8ToWide(user_data_dir_utf8))
      .Append(FILE_PATH_LITERAL("chrome_debug.log"));
}

}  // namespace

bool ExeOwnsLogging() {
#if defined(COMPONENT_BUILD)
  return false;
#else
  return true;
#endif
}

void InitEarlyClientLogging() {
  if (!ExeOwnsLogging()) {
    return;
  }
  logging::LoggingSettings settings;
  settings.logging_dest =
      logging::LOG_TO_SYSTEM_DEBUG_LOG | logging::LOG_TO_STDERR;
  logging::InitLogging(settings);
}

void AttachClientLoggingToBrowserLog(const std::string& user_data_dir_utf8) {
  if (!ExeOwnsLogging()) {
    return;
  }
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  logging::LoggingSettings settings;
  settings.logging_dest = BrowserLoggingDestination(command_line);
  if (settings.logging_dest & logging::LOG_TO_FILE) {
    settings.log_file_path =
        BrowserLogFile(command_line, user_data_dir_utf8).value();
    settings.lock_log = logging::LOCK_LOG_FILE;
    // chrome.dll truncated or kept the file at startup; the exe only ever
    // appends to what the browser is writing.
    settings.delete_old = logging::APPEND_TO_OLD_LOG_FILE;
  }
  logging::InitLogging(settings);
  // The same prefix chrome.dll's lines carry, so one file reads as one log.
  logging::SetLogItems(/*enable_process_id=*/true, /*enable_thread_id=*/true,
                       /*enable_timestamp=*/true, /*enable_tickcount=*/false);
  if (command_line.HasSwitch(switches::kLoggingLevel)) {
    int level = 0;
    if (base::StringToInt(
            command_line.GetSwitchValueASCII(switches::kLoggingLevel),
            &level) &&
        level >= 0 && level < logging::LOGGING_NUM_SEVERITIES) {
      logging::SetMinLogLevel(level);
    }
  }
}

}  // namespace phi_client

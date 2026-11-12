// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_CLIENT_LOGGING_H_
#define PHIBROWSER_WIN_CLIENT_PHI_CLIENT_LOGGING_H_

#include <string>

// Where the exe's own LOG lines go (docs/static-build-plan.md §1).
//
// In the component build the exe and chrome.dll share base.dll, so the
// client's LOG/VLOG/PHI_LIFECYCLE lines ride the logging state chrome.dll's
// InitChromeLogging sets up — chrome_debug.log in the user data dir under
// --enable-logging, the file every feedback report zips. In a non-component
// build the exe carries its OWN copy of base, whose Windows default is a
// debug.log next to the exe: a stray file beside Phi.exe and no client line
// in any support bundle. These two calls point the exe's copy at the same
// destination chrome.dll chose. Both are no-ops in the component build, so
// the same client source builds against either framework.
namespace phi_client {

// True when this exe carries its own copy of base (non-component build).
bool ExeOwnsLogging();

// Every process type, right after CommandLine::Init and before ChromeMain:
// nothing to a file. Early lines (a failed chrome.dll load, the crashpad
// handler) go to stderr and the debugger, as --enable-logging=stderr would
// send them.
void InitEarlyClientLogging();

// The browser process, from OnFrameworkReady: re-points the exe's logging at
// what InitChromeLogging computed for the browser — LOG_NONE without
// --enable-logging, stderr for --enable-logging=stderr, else
// --log-file / $CHROME_LOG_FILE / <user data dir>\chrome_debug.log, appended
// (chrome.dll already rotated it at startup). base allows exactly this one
// re-init (logging.h: "may be called a second time to re-direct logging").
void AttachClientLoggingToBrowserLog(const std::string& user_data_dir_utf8);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CLIENT_LOGGING_H_

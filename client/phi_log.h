// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_LOG_H_
#define PHIBROWSER_WIN_CLIENT_PHI_LOG_H_

#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace phi_client {

// The client's logging policy, in code. The prose is docs/logging.md; this
// header is the part a compiler can hold you to.
//
// WHY THIS EXISTS AT ALL. Every feedback report zips `chrome_debug.log` and
// ships it to the vendor (phi_feedback_outbox.cc, the logs.zip attachment).
// Whatever the client writes to a log therefore LEAVES THE MACHINE the moment
// a user reports a bug. Today an Auth0 subject id and page URLs are written
// at INFO, so a support bundle carries an account identity and a browsing
// trail that nobody agreed to send. Nothing below is a style preference.
//
// The four rules (docs/logging.md has the reasoning and the CI greps):
//   1. INFO is for LIFECYCLE TRANSITIONS only — a service started, a window
//      opened, a language switched. Never per navigation, per tab, per paint.
//      Write it through PHI_LIFECYCLE so the grep that enforces this has a
//      single exemption to carve out.
//   2. ERROR only when the user gets a degraded result, and it carries
//      ::GetLastError() or the HTTP status. Everything recoverable is
//      WARNING.
//   3. NO URL, title, email, account id, token, host-with-path or path under
//      the user profile reaches a log without passing through a PhiRedact*
//      function below. Not at INFO, not at ERROR, not behind a DCHECK.
//   4. One tag per module, spelled `[PhiModule]`, declared once at the top of
//      the file as `constexpr char kTag[] = "[PhiDownloads] ";`.
//
// The redactors are pure and host-tested (test/model/util_test.cc): they must
// stay that way, because the one thing worse than an unredacted log is a
// redactor that crashes while writing one.

// A URL reduced to what a bug report actually needs: scheme and host, no
// path, no query, no fragment, no credentials, no port.
//   "https://mail.example.com/u/0/#inbox?q=x" -> "https://mail.example.com"
//   "file:///C:/Users/ada/secret.pdf"         -> "file://"
// Anything that does not look like a URL comes back as "<url>": guessing is
// how a path leaks.
std::string PhiRedactUrl(std::string_view url);

// An opaque identifier — an Auth0 `sub`, a GUID, a distinct_id — reduced to
// its last four characters, which is enough to correlate two lines in one
// log and not enough to name anybody.
//   "auth0|66f0c1d2e3b4a5968778" -> "...8778"
// Shorter than four characters redacts to "..." entirely.
std::string PhiRedactId(std::string_view id);

// An address reduced to its first character and its domain.
//   "ada.lovelace@example.com" -> "a***@example.com"
// Without an "@" this falls through to PhiRedactId, because a caller that
// thought it had an address and did not is exactly the leak this prevents.
std::string PhiRedactEmail(std::string_view email);

// A filesystem path reduced to its last two components, so a log still says
// WHICH file went wrong without naming the user or their disk layout.
//   "C:\Users\ada\AppData\Local\Phi\User Data\Default\PhiPrefs.json"
//     -> "...\Default\PhiPrefs.json"
// A bare leaf name is returned as it is.
std::string PhiRedactPath(std::string_view path);

#if defined(_WIN32)
// The wide overloads the Win32 call sites need. Narrow with base's UTF-8
// conversion first, then redact, so a non-ASCII profile name cannot smuggle
// bytes past the rules above.
std::string PhiRedactPath(const std::wstring& path);
std::string PhiRedactUrl(const std::wstring& url);
#endif

// INFO, and only for a lifecycle transition (rule 1). `tag` is the module's
// kTag. Written as a macro so that `LOG(INFO)` outside this header is a CI
// failure and the exemption list is one line long.
//
//   PHI_LIFECYCLE(kTag) << "telemetry started, endpoint " << host;
#if defined(_WIN32)
#define PHI_LIFECYCLE(tag) LOG(INFO) << (tag)
#endif

// The wrapper for the ~60 bool-returning framework verbs that are fired and
// forgotten today (lane 2's list: ActivateTab x9, SetTabTag x10, CloseTab x3,
// SplitActiveWith x4, …). A refused write currently leaves the client's store
// and the strip disagreeing with no trace at all.
//
// Wrap the call, keep the bool:
//
//   if (!PhiFw::Must(framework_->ActivateTab(browser, index),
//                    "ActivateTab", browser, index)) {
//     return;                       // A caller that cannot recover returns.
//   }
//
// A refusal logs one WARNING carrying the verb and the ids; success costs a
// branch. The ids are streamed with operator<<, so anything a log line can
// take can be passed — but they are IDS, not payloads: a URL or a title goes
// through PhiRedact* first, same as any other line.
namespace PhiFw {

// The logging half, out of line so the header does not pull in base/logging.h
// and stays host-compilable. Always returns `ok`.
bool MustImpl(bool ok, const char* verb, const std::string& ids);

template <typename... Ids>
bool Must(bool ok, const char* verb, Ids&&... ids) {
  if (ok) {
    return true;  // The hot path allocates nothing.
  }
  std::ostringstream out;
  ((out << ' ' << std::forward<Ids>(ids)), ...);
  return MustImpl(false, verb, out.str());
}

}  // namespace PhiFw

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_LOG_H_

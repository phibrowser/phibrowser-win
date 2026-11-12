// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_HTTP_H_
#define PHIBROWSER_WIN_CLIENT_PHI_HTTP_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace phi_client {

// One HTTP round trip over WinHTTP — the client's requests to the Auth0
// tenant, the account service and the feedback uploads' presigned URLs
// (lane 7 §5.5 counted four copies of this loop). BLOCKING: worker threads
// only, never the UI thread.
struct PhiHttpResult {
  // The request reached a server and a status line came back. False on a
  // DNS/TCP/TLS failure (offline) or a URL the helper cannot parse; `status`
  // and `body` are meaningless then. An HTTP error (4xx/5xx) is still
  // transport_ok — the caller reads `status`.
  bool transport_ok = false;
  int status = 0;
  std::string body;
  // The response's `Date` header as Unix ms, when present and parseable
  // (Phi sync's clock correction, AM-2).
  std::optional<int64_t> server_date_ms;
  // A redirect's `Location` header, for a request that does not follow
  // redirects itself (PhiHttpRequestBounded). Empty otherwise.
  std::string location;
};

// The response body cap PhiHttpRequest applies unless told otherwise.
inline constexpr size_t kPhiHttpDefaultMaxBytes = 8u * 1024 * 1024;

// `url` is absolute (http or https, an optional port). `headers` is zero or
// more "Name: value\r\n" lines; `body` is sent as is (empty = no body). The
// proxy is Windows' automatic configuration. A response body longer than
// `max_bytes` is a transport failure. A request carrying an Authorization
// header never follows a redirect (the 3xx comes back as the status), so
// the credential cannot ride to another host.
PhiHttpResult PhiHttpRequest(const wchar_t* method,
                             const std::string& url,
                             const std::wstring& headers,
                             const std::string& body,
                             size_t max_bytes = kPhiHttpDefaultMaxBytes);
// The same with every phase bounded by `timeout_ms`, no cookies, NO
// redirects (a 3xx returns its status and `location`, for the caller to vet
// and follow) and at most `max_bytes` of body (a longer body is a transport
// failure).
PhiHttpResult PhiHttpRequestBounded(const wchar_t* method,
                                    const std::string& url,
                                    const std::wstring& headers,
                                    int timeout_ms,
                                    size_t max_bytes);

// Splits an absolute URL into WinHTTP's parts. Exposed for the outbox's
// presigned URLs; false when the scheme or authority is not usable.
bool PhiSplitUrl(const std::string& url,
                 std::wstring* host,
                 std::wstring* path,
                 int* port,
                 bool* secure);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_HTTP_H_

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_log.h"

#include <algorithm>

#include "client/phi_util.h"

#if defined(_WIN32)
#include "base/logging.h"
#endif

namespace phi_client {

namespace {

constexpr char kRedactedUrl[] = "<url>";
constexpr char kElision[] = "...";

// The authority of `url` — what sits between "://" and the first '/', '?' or
// '#' — with any "user:password@" and any ":port" removed.
std::string_view Authority(std::string_view rest) {
  const size_t end = rest.find_first_of("/?#");
  std::string_view authority =
      end == std::string_view::npos ? rest : rest.substr(0, end);
  const size_t at = authority.rfind('@');
  if (at != std::string_view::npos) {
    authority = authority.substr(at + 1);
  }
  // An IPv6 literal ends in ']', so a colon inside one is not a port.
  const size_t colon = authority.rfind(':');
  const bool bracketed = authority.find(']') != std::string_view::npos;
  if (colon != std::string_view::npos && !bracketed) {
    authority = authority.substr(0, colon);
  }
  return authority;
}

}  // namespace

std::string PhiRedactUrl(std::string_view url) {
  const size_t scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos || scheme_end == 0) {
    // "mailto:", "about:blank", "chrome://" without a host, or plain text.
    // Only the schemes that carry nothing personal are spelled out.
    const size_t colon = url.find(':');
    if (colon != std::string_view::npos && colon > 0) {
      const std::string scheme = PhiLower(url.substr(0, colon));
      if (scheme == "about" || scheme == "chrome" || scheme == "phi") {
        return std::string(url.substr(0, colon)) + ":";
      }
    }
    return kRedactedUrl;
  }
  const std::string scheme = PhiLower(url.substr(0, scheme_end));
  for (char c : scheme) {
    const bool legal = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                       c == '+' || c == '-' || c == '.';
    if (!legal) {
      return kRedactedUrl;
    }
  }
  const std::string_view authority = Authority(url.substr(scheme_end + 3));
  // "file://" has no host, and its path is the whole secret.
  return scheme + "://" + std::string(authority);
}

std::string PhiRedactId(std::string_view id) {
  if (id.size() < 4) {
    return kElision;
  }
  return std::string(kElision) + std::string(id.substr(id.size() - 4));
}

std::string PhiRedactEmail(std::string_view email) {
  const size_t at = email.find('@');
  if (at == std::string_view::npos || at == 0) {
    return PhiRedactId(email);
  }
  return std::string(email.substr(0, 1)) + "***" +
         std::string(email.substr(at));
}

std::string PhiRedactPath(std::string_view path) {
  // Walk back over at most two separators; whatever is left of them goes.
  size_t cut = std::string_view::npos;
  for (int found = 0; found < 2; ++found) {
    const size_t at = path.find_last_of("\\/", cut == std::string_view::npos
                                                   ? std::string_view::npos
                                                   : cut - 1);
    if (at == std::string_view::npos || at == 0) {
      break;
    }
    cut = at;
  }
  if (cut == std::string_view::npos) {
    return std::string(path);  // A bare leaf name names nobody.
  }
  return std::string(kElision) + std::string(path.substr(cut));
}

#if defined(_WIN32)

std::string PhiRedactPath(const std::wstring& path) {
  return PhiRedactPath(std::string_view(PhiNarrow(path)));
}

std::string PhiRedactUrl(const std::wstring& url) {
  return PhiRedactUrl(std::string_view(PhiNarrow(url)));
}

#endif  // defined(_WIN32)

namespace PhiFw {

bool MustImpl(bool ok, const char* verb, const std::string& ids) {
#if defined(_WIN32)
  if (!ok) {
    LOG(WARNING) << "[PhiFramework] refused: " << (verb ? verb : "?") << ids;
  }
#else
  (void)verb;
  (void)ids;
#endif
  return ok;
}

}  // namespace PhiFw

}  // namespace phi_client

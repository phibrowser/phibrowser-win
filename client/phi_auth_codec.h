// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_AUTH_CODEC_H_
#define PHIBROWSER_WIN_CLIENT_PHI_AUTH_CODEC_H_

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace phi_client {

// The PURE half of the Auth0 sign-in (phi_auth.cc): the codecs the RFC 8252
// native flow is made of, with no Win32, no base/ and no network, so the
// host builds and tests them (test/model/auth_codec_test.cc). The sockets,
// WinHTTP, DPAPI and the message window stay in phi_auth.cc.

// base64url without padding — RFC 7636's alphabet for the PKCE verifier and
// challenge, and RFC 7515's for JWT segments.
std::string PhiBase64UrlEncode(std::string_view bytes);
// Decodes base64url with or without padding; nullopt on a bad alphabet.
std::optional<std::string> PhiBase64UrlDecode(std::string_view text);

// SHA-256 (FIPS 180-4), for the PKCE S256 challenge.
std::array<uint8_t, 32> PhiSha256(std::string_view data);
// base64url(SHA-256(verifier)) — RFC 7636 §4.2.
std::string PhiPkceChallenge(std::string_view verifier);

// RFC 3986 percent-encoding of everything outside the unreserved set.
std::string PhiUrlEncode(std::string_view in);
// Percent-decoding, with '+' read as a space (form encoding).
std::string PhiUrlDecode(std::string_view in);

// The Auth0 callback the Mac app registers and Windows intercepts in the
// browser (phi_auth_callback_navigation_throttle):
//   <bundle_id>://<domain>/macos/<bundle_id>/callback
// Auth0.swift's default redirect URL for macOS, listed on the tenant's
// application, so one dashboard entry serves both platforms.
std::string PhiMacCallbackUri(std::string_view bundle_id,
                              std::string_view domain);

// What the redirect carried, parsed from the URL a navigation was cancelled
// on. `error` is Auth0's `error` parameter when the user was refused
// (access_denied, …); a logout return carries neither code nor error.
struct PhiCallbackUrl {
  std::string scheme;  // Lower-cased.
  std::string host;    // Lower-cased.
  std::string path;    // "/macos/<bundle_id>/callback"
  std::string code;    // Decoded.
  std::string state;   // Decoded.
  std::string error;   // Decoded; empty on success.
};
// nullopt when `url` has no "<scheme>://<host>/…" shape at all.
std::optional<PhiCallbackUrl> PhiParseCallbackUrl(std::string_view url);

// The claims segment of a compact JWS (`header.payload.signature`),
// base64url-decoded to its JSON text. nullopt when the token has no three
// segments or the payload is not base64url. The signature is NOT checked:
// the id_token arrives over the code exchange's TLS channel straight from
// the issuer, the trust the Mac's SDK places in it too.
std::optional<std::string> PhiJwtPayload(std::string_view jwt);

// The account service behind a tenant — the Mac APIClient's accountBaseURL:
// the staging host for a staging domain, production otherwise.
std::string PhiAccountBaseUrlForDomain(std::string_view domain);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_AUTH_CODEC_H_

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_URL_RULE_NORMALIZE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_URL_RULE_NORMALIZE_H_

#include <optional>
#include <string>
#include <string_view>

// URL rule normalisation — phibrowser-mac LocalStore.normalizedHost /
// normalizedPathPrefix (LocalStore+SpaceURLRule.swift), ported byte for
// byte: a rule's identity, its merge-partner signature and the routing
// table all read these canonical forms, so Windows and the Mac must produce
// the same bytes from the same input. Pinned by the Mac-generated vectors
// (test/sync-vectors: urlrule_normalized_host, urlrule_normalized_path_prefix).
//
//   host: leading White_Space trimmed; the trailing run of White_Space and
//         '.' stripped in one scan; Unicode full lowercase (per scalar, as
//         Swift's lowercased()).
//   path: nil stays nil; White_Space trimmed; "/" stays "/" (the root
//         ONLY — not "any path"); empty is nil (any path); a leading '/' is
//         ensured; percent-decoded when that is valid UTF-8, re-encoded
//         with urlPathAllowed; trailing '/' removed (never the lone one).
//
// PURE: STL only, host-tested.

namespace phi_client::store {

std::string NormalizedRuleHost(std::string_view raw);
// A new SpaceURLRule id, the Mac's UUID().uuidString form (uppercase).
std::string NewRuleId();
std::optional<std::string> NormalizedRulePathPrefix(
    const std::optional<std::string>& raw);

// An App rule's host (phibrowser-mac 48d4c4a6, SpaceRoutingRule.
// sourceAppHostPrefix): "app:" + the lowercased bundle id of the Mac app
// that sent the URL, path prefix empty, the same sync columns as any rule.
// The prefix compares case-insensitively, the remainder must be non-empty
// and hold no further ':'. Only the Mac matches these (it knows which app
// opened a URL); Windows accepts, stores, syncs and shows them, and keeps
// them out of the framework's routing table.
inline constexpr std::string_view kSourceAppRuleHostPrefix = "app:";
bool IsSourceAppRuleHost(std::string_view host);

// The ROUTING-TABLE form of a stored (canonical, Unicode) rule host: the
// fork matches against GURL::host(), which spells an internationalised
// domain in its ASCII form, so a Unicode host would never match. Each
// label holding a non-ASCII scalar becomes "xn--" + its RFC 3492 Punycode
// (IDNA's ACE form); the wildcard sigils stay where they were ("*.bücher.de"
// -> "*.xn--bcher-kva.de", "*bücher*" -> "*xn--bcher-kva*"), and the IDNA
// full stops U+3002 / U+FF0E / U+FF61 separate labels as '.' does. The
// stored rule — what the editor shows and sync carries — keeps the Unicode.
// Limits: no NFC step (the stored host is already lowercased, and typed
// text is NFC in practice), and a "contains" needle matches only when it is
// whole labels — a fragment of a label has no ACE form of its own. ASCII
// hosts, and bytes that are not UTF-8, come back unchanged.
std::string RuleHostForRouting(std::string_view host);

}  // namespace phi_client::store

#endif  // PHIBROWSER_WIN_CLIENT_PHI_URL_RULE_NORMALIZE_H_

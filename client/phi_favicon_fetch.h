// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_FAVICON_FETCH_H_
#define PHIBROWSER_WIN_CLIENT_PHI_FAVICON_FETCH_H_

#include <string>

namespace phi_client {

// The Mac's PhiFaviconHostFilter: http(s) with a real host — never
// localhost, *.local or anything that parses as an IP (incl. the legacy
// "0x7f.1" / "2130706433" forms). Textual only; PhiFetchSiteFavicon also
// checks what the host resolves to.
bool PhiFaviconHostAllowed(const std::string& page_url);

// The favicon backfill's network step (PhiFaviconFetcher.networkFavicon):
// https://<host>/favicon.ico, no cookies, 5 s, 64 KB, at most 3 vetted
// https redirects, every host resolving to public addresses only; then
// decoded (PNG or ICO containers only, frames up to 256×256) to `px`×`px`
// premultiplied BGRA. Blocking; any thread.
bool PhiFetchSiteFavicon(const std::string& page_url,
                         int px,
                         std::string* bgra);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_FAVICON_FETCH_H_

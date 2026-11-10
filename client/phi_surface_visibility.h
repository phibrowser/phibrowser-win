// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SURFACE_VISIBILITY_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SURFACE_VISIBILITY_H_

#include <map>
#include <set>
#include <vector>

namespace phi_client {

// Which attached surfaces are on screen over one window — the Mac's
// BrowserState.visiblePeek / visibleReaderOverlays (phibrowser-mac
// 89dd8a48). Everything is a tab id: `focused` is the window's active tab
// (0 = none), `partner` its split partner (0 = not in a split), `peeks` maps
// an opener tab to its peek's own tab (0 while that is unknown), `readers`
// holds the origin tabs a reader stands over. Pure and header-only so the
// rules are pinned by test/model/surface_visibility_test.cc; the surface
// coordinator feeds it from the window model and the panel registry.

struct PhiVisiblePeek {
  int opener = 0;    // The tab the peek is bound to; 0 = no peek on screen.
  int peek_tab = 0;  // The peek's own tab, when known.
  bool valid() const { return opener != 0; }
};

// The peek on screen for `focused`: its own, else a split partner's — a
// peek covers its opener's whole split, so it stays up whichever pane is
// focused. The focused pane's own peek wins. `partners` are the split's
// other panes in slot order (one to three; empty outside a split).
inline PhiVisiblePeek PhiVisiblePeekFor(int focused,
                                        const std::vector<int>& partners,
                                        const std::map<int, int>& peeks) {
  if (focused == 0) {
    return {};
  }
  if (const auto own = peeks.find(focused); own != peeks.end()) {
    return {focused, own->second};
  }
  for (const int partner : partners) {
    if (const auto other = peeks.find(partner); other != peeks.end()) {
      return {partner, other->second};
    }
  }
  return {};
}

// The origins whose reader is on screen for `focused`. A reader covers its
// origin's page: in a split, each pane's reader covers that pane and stays
// up whichever pane is focused. A peek sits in front of the whole page card,
// so while one is up only a reader over the peek shows — or, for a single
// tab, the opener's own reader, which covers the card peek included.
inline std::vector<int> PhiVisibleReaderOrigins(
    int focused,
    const std::vector<int>& partners,
    const std::map<int, int>& peeks,
    const std::set<int>& readers) {
  std::vector<int> out;
  if (focused == 0) {
    return out;
  }
  const PhiVisiblePeek peek = PhiVisiblePeekFor(focused, partners, peeks);
  if (peek.valid()) {
    if (partners.empty() && readers.count(focused)) {
      out.push_back(focused);
    } else if (peek.peek_tab != 0 && readers.count(peek.peek_tab)) {
      out.push_back(peek.peek_tab);
    }
    return out;
  }
  if (readers.count(focused)) {
    out.push_back(focused);
  }
  for (const int tab : partners) {
    if (tab != 0 && readers.count(tab)) {
      out.push_back(tab);
    }
  }
  return out;
}

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SURFACE_VISIBILITY_H_

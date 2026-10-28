// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_VERSION_H_
#define PHIBROWSER_WIN_CLIENT_PHI_VERSION_H_

// The client's own version — what the updater reports to the appcast, what
// About shows, and what the exe's VERSIONINFO (phi_client.rc) carries. The
// release pipeline (installer/make-installer.sh) greps the string literal
// below, so it stays a literal; the numeric parts feed FILEVERSION, which
// needs four integers. Bump BOTH together — the static_assert at the bottom
// refuses a build where they disagree. The test-release GitHub Action stamps
// PHI_WIN_VERSION_BUILD with its increasing run number while building, then
// restores this source file. The full X.Y.Z.build version is used for update
// comparison and displayed to users.
#define PHI_WIN_VERSION_MAJOR 0
#define PHI_WIN_VERSION_MINOR 3
#define PHI_WIN_VERSION_PATCH 0
#define PHI_WIN_VERSION_BUILD 0
#define PHI_WIN_VERSION_STRING "0.3.0"
#define PHI_WIN_VERSION_WIDE L"0.3.0"

#define PHI_WIN_VERSION_STRINGIFY_(x) #x
#define PHI_WIN_VERSION_STRINGIFY(x) PHI_WIN_VERSION_STRINGIFY_(x)
#define PHI_WIN_VERSION_WIDEN_(x) L##x
#define PHI_WIN_VERSION_WIDEN(x) PHI_WIN_VERSION_WIDEN_(x)
#define PHI_WIN_VERSION_FULL_STRING \
  PHI_WIN_VERSION_STRING "." PHI_WIN_VERSION_STRINGIFY(PHI_WIN_VERSION_BUILD)
#define PHI_WIN_VERSION_FULL_WIDE \
  PHI_WIN_VERSION_WIDE L"." \
      PHI_WIN_VERSION_WIDEN(PHI_WIN_VERSION_STRINGIFY(PHI_WIN_VERSION_BUILD))

// The release ring this build ships on, as feedback reports name it (the
// Mac maps its nightly builds to "canary" and everything else to "stable";
// a DCHECK build reports "debug" regardless). Set alongside the version
// when a ring changes.
#define PHI_WIN_CHANNEL "canary"

// The resource compiler includes this header too (RC_INVOKED); only the C++
// side can check that the literal and the numerics still agree.
#ifndef RC_INVOKED
#include <string_view>
static_assert(std::string_view(PHI_WIN_VERSION_STRING) ==
                  PHI_WIN_VERSION_STRINGIFY(PHI_WIN_VERSION_MAJOR) "."
                  PHI_WIN_VERSION_STRINGIFY(PHI_WIN_VERSION_MINOR) "."
                  PHI_WIN_VERSION_STRINGIFY(PHI_WIN_VERSION_PATCH),
              "PHI_WIN_VERSION_STRING must match PHI_WIN_VERSION_MAJOR/"
              "MINOR/PATCH");
#endif  // RC_INVOKED

#endif  // PHIBROWSER_WIN_CLIENT_PHI_VERSION_H_

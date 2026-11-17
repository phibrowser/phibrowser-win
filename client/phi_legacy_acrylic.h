// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_LEGACY_ACRYLIC_H_
#define PHIBROWSER_WIN_CLIENT_PHI_LEGACY_ACRYLIC_H_

#include <windows.h>

namespace phi_client {

// The shell's acrylic where DWMWA_SYSTEMBACKDROP_TYPE is missing: Windows 10
// (1803 and later) and Windows 11 before 22H2 (GitHub
// phibrowser/phibrowser-win#10). DWM's accent policy blurs what is behind
// the whole window; the shell's per-pixel alpha then lays the theme tint
// over it exactly as it does over the system backdrop.

// Windows 10 1803 (build 17134) or later: the first with an acrylic accent.
bool PhiLegacyAcrylicSupported();

// Turns `hwnd`'s acrylic accent on or off. `dark` picks the neutral the
// blur settles to (the system backdrop's own: near-black or near-white),
// so the tint maths in phi_backdrop.h reads the same on both paths. False
// when the accent could not be set (then the shell stays opaque).
bool PhiSetLegacyAcrylic(HWND hwnd, bool enabled, bool dark);

// Windows 11 22H2 (build 22621) or later: DWMWA_SYSTEMBACKDROP_TYPE exists,
// so Mica (and the system acrylic) can be chosen.
bool PhiSystemBackdropSupported();

// --phi-legacy-acrylic, dev-hooks builds only: take the accent path even
// where the system backdrop exists, so a Windows 11 test box can run it.
bool PhiForceLegacyAcrylic();

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_LEGACY_ACRYLIC_H_

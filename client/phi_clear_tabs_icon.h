// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_CLEAR_TABS_ICON_H_
#define PHIBROWSER_WIN_CLIENT_PHI_CLEAR_TABS_ICON_H_

#include <windows.h>

#include <vector>

namespace phi_client {

// The sidebar separator's Clear button artwork, the Mac's "clear-tabs"
// Lottie (phibrowser-mac Resources/LottieFiles/clear-tabs.json): a bin of
// three stroked layers in a 60-unit frame, 60 fps, frames 0..30. A click
// plays it once and the cleanup runs when it ends (SidebarCleanupButton
// .playClearAnimation): the lid tips 18 degrees about its left end and lifts,
// the body squashes to 94 % about its base, the two inner lines dim to 30 %,
// all on cubic-bezier(0.42, 0, 0.58, 1).
inline constexpr int kClearTabsAnimationMs = 500;

// The bin's coverage at `t` (0..1; 0 and 1 are the resting frame) as a
// `size` x `size` alpha mask, for PhiTintedIconBgra.
std::vector<unsigned char> PhiClearTabsIconMask(double t, int size);

// Paints the bin into the square `art` in `ink` (the Mac tints it
// textMuted).
void PhiPaintClearTabsIcon(HDC dc, const RECT& art, COLORREF ink, double t);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CLEAR_TABS_ICON_H_

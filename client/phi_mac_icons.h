// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_MAC_ICONS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_MAC_ICONS_H_

#include <windows.h>

#include <vector>

namespace phi_client {

// The Mac sidebar's own icon artwork (memory-icon, sidebar-chat,
// sidebar-feedback and left-sidebar-toggle imagesets), rasterized from
// their PDFs into 48px alpha masks. All three are template images on Mac — the SHAPE is the asset and
// the ink comes from the theme — so the client tints them at draw time.
struct PhiMacIcon {
  int width;
  int height;
  const unsigned char* alpha;
};

const PhiMacIcon& PhiMemoryIcon();
// The bug on the sidebar's Feedback capsule and the app menu's Report an
// issue row (sidebar-feedback.imageset/solar_bug-line-duotone.pdf): the
// Fluent face has no bug near it, so this one mask stays (§2.2a's exception
// list).
const PhiMacIcon& PhiFeedbackIcon();
const PhiMacIcon& PhiChatIcon();
// The sidebar bottom bar's Download line icon (Figma node 9044:27222): a
// 24x24-frame artwork, so it is drawn at 24 dip and keeps its own margin.
const PhiMacIcon& PhiDownloadIcon();

// The native new-tab page's artwork (native-NTP-icon / native-NTP-incognito
// imagesets), 192px masks generated into phi_ntp_art.cc by
// tools/gen-ntp-art.py. The Mac shows them at 64pt in black (light) or
// #F8F8F8 (dark); the page tints them the same way.
const PhiMacIcon& PhiNativeNtpLogo();
const PhiMacIcon& PhiNativeNtpIncognito();

// `icon` tinted `color` as premultiplied BGRA at `out_size` pixels square
// (area-averaged from the 48px mask), ready for PhiBlendBgra — the Mac's
// template rendering. Pass out_size <= 0 to keep the mask's own size.
std::vector<unsigned char> PhiTintedIconBgra(const PhiMacIcon& icon,
                                             COLORREF color,
                                             int out_size);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_MAC_ICONS_H_

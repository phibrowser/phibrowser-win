// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_FOLDER_ICONS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_FOLDER_ICONS_H_

#include <windows.h>

#include <string>
#include <vector>

#include "client/phi_mac_icons.h"

namespace phi_client {

// The Mac's BookmarkFolderIcon catalog (Sidebar/TabList/Views/
// BookmarkFolderIcon.swift): the 24 symbols a sidebar bookmark FOLDER can
// wear, with the SAME raw values and the SAME picker order, so a folder icon
// stored by either client means the same thing on both. "default" is the
// plain folder.
//
// The Mac animates the folder shell with Lottie and paints the symbol on its
// face; this client has no Lottie runtime, so it draws the symbol artwork
// from the same imageset SVGs the Mac's own picker uses, tinted at draw time
// like every other template icon here.
struct PhiFolderIcon {
  const char* id;    // Storage value — the Mac's raw value.
  const char* name;  // Picker tooltip.
};

// All 24, in the Mac picker's order (`BookmarkFolderIcon.allCases`).
const std::vector<PhiFolderIcon>& PhiFolderIconsAll();

// The catalog's default id ("default"), which is also what an unknown or
// empty stored value resolves to — the Mac's BookmarkFolderIcon.resolve.
const char* PhiFolderIconDefaultId();
std::string PhiResolveFolderIcon(const std::string& stored);

// The 48px alpha mask for `id`, or null when `id` is not in the catalog
// (generated phi_folder_icon_masks.cc). Feed it to PhiTintedIconBgra.
const PhiMacIcon* PhiFolderIconMask(const std::string& id);

// The plain folder a "default" row wears, open or closed — the Mac's own
// Lottie-fallback art, so the default state is real artwork rather than a
// font glyph (Segoe MDL2's folder codepoint does not render here).
const PhiMacIcon* PhiPlainFolderMask(bool expanded);

// The plain folder as the SIDEBAR draws it: the Mac's two-tone art (back
// body, front panel, 1pt stroke) tinted from the Space accent the way
// BookmarkFolderIconPalette tints the Lottie shell — light: back at 65%
// saturation and 15% darker than the accent, front a 20% wash at full
// brightness, stroke 65% saturation at 30% brightness; dark: back is the
// window background, front the accent a touch deeper, stroke white.
// Premultiplied BGRA `px` square, ready for PhiBlendBgra.
std::vector<unsigned char> PhiPlainFolderBgra(bool expanded,
                                              int px,
                                              COLORREF accent,
                                              bool dark,
                                              COLORREF window_bg);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_FOLDER_ICONS_H_

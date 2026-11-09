// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_DOWNLOADS_POPOVER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_DOWNLOADS_POPOVER_H_

#include <windows.h>

#include <cstdint>

#include "client/phi_ids.h"
#include "client/phi_theme.h"

namespace phi_framework {
class PhiFramework;
}

namespace phi_client {

// The Mac DownloadsListView popover, opened from the sidebar bottom bar's
// download button: a 340x317 panel above the anchor listing the window
// profile's downloads (56pt rows — file icon, name, progress or relative
// date, per-state actions) over an "All Downloads" footer. One popover
// exists at a time; deactivation dismisses it, and clicking the button
// while it is open closes rather than reopens it.
class PhiDownloadsPopover {
 public:
  // Toggles the popover for `owner`. `anchor_screen` is the download
  // button's rect in screen coordinates; `browser_id` addresses the
  // window's profile on the framework.
  static void Toggle(HWND owner,
                     const RECT& anchor_screen,
                     const PhiPalette& palette,
                     phi_framework::PhiFramework* framework,
                     BrowserId browser);
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_DOWNLOADS_POPOVER_H_

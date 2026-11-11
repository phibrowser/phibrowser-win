// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_POP_ANIMATION_H_
#define PHIBROWSER_WIN_CLIENT_PHI_POP_ANIMATION_H_

#include <windows.h>

// How every menu, popover and dialog arrives and leaves (GitHub issue 9):
// it grows out of the thing that opened it — the button, the row, the
// point that was clicked — and shrinks back into it when it closes, the way
// the Peek card does (phi_card_flight.h).
//
// A live window cannot be scaled, so a picture of it does the flying. On
// the way in the window is shown fully transparent, its picture is taken as
// soon as it has painted, that picture grows from the source into the
// window's place, and the window itself is revealed under the last frame.
// On the way out a picture of it shrinks back while the window is already
// gone. The window works from the first moment either way.
//
// Always on: Windows' "Animation effects" switch does not turn it off.

namespace phi_client {

// Whether the pop-in runs. Always, at present.
bool PhiPopAnimationsEnabled();

// Arms the pop-in for a top-level window that is about to be shown. Call it
// while the window is still hidden. `source`, in screen coordinates, is
// where it grows from; null takes the pointer. UI thread only.
void PhiPopIn(HWND hwnd, const POINT* source = nullptr);

// The way back: a picture of `hwnd`, taken now, shrinks into the source its
// pop-in started from. Call it while the window is still on screen, right
// before hiding or destroying it. A window that never popped in is left
// alone.
void PhiPopOut(HWND hwnd);

// TrackPopupMenu, with the pop-in and pop-out on the menu and on every
// submenu it opens. Same arguments and result. The system's own menu fade
// is turned off for the call, so the two never stack.
BOOL PhiTrackPopupMenu(HMENU menu,
                       UINT flags,
                       int x,
                       int y,
                       int reserved,
                       HWND owner,
                       const RECT* rect);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_POP_ANIMATION_H_

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_OPAQUE_EDIT_H_
#define PHIBROWSER_WIN_CLIENT_PHI_OPAQUE_EDIT_H_

#include <windows.h>

#include <memory>

namespace phi_client {

class PhiLayerSurface;

// A native EDIT inside the shell. The shell extends its DWM frame under the
// whole client area, so the compositor reads the alpha channel of every
// child window's pixels. GDI leaves that alpha at zero, so the backdrop is
// ADDED to the EDIT's ground and glyphs: a light block inside a dark field.
// These helpers route the EDIT's own drawing through an offscreen 32bpp
// surface and make every pixel opaque before presenting. This is the
// address field's technique (PhiOmniboxSession::PaintField), shared.

// Handles WM_PAINT for `field`: fills its client area with `ground`, lets
// the native EDIT draw its text, selection, cue banner and IME composition
// into the surface through `base_proc` (WM_PRINTCLIENT; the EDIT still asks
// its parent for colours via WM_CTLCOLOREDIT), forces opaque alpha and
// blits. `surface` is the caller's cache, resized as needed. Returns false
// when no surface could be made; the caller then lets `base_proc` paint.
bool PhiPaintEditOpaque(HWND field,
                        WNDPROC base_proc,
                        COLORREF ground,
                        std::unique_ptr<PhiLayerSurface>& surface);

// True for the messages after which the EDIT has drawn synchronously,
// outside WM_PAINT (typing, selection, caret moves, IME). The caller
// invalidates the field once the base proc has run, so that output is
// redrawn through PhiPaintEditOpaque.
bool PhiEditDrawsDirectly(UINT msg, WPARAM wparam);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_OPAQUE_EDIT_H_

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_TOAST_H_
#define PHIBROWSER_WIN_CLIENT_PHI_TOAST_H_

#include <windows.h>

#include <functional>
#include <string>

#include "client/phi_theme.h"

namespace phi_client {

// Tells a screen reader `text` without moving its focus (a page changed, a
// toast appeared). Silent with no reader listening or on an older system.
void PhiAnnounceToScreenReaders(HWND hwnd, const std::wstring& text);

// The Mac OverlayToastCenter: a transient top-center text toast over
// `owner`. One toast at a time — a new one replaces the current (the Mac
// queues; replacement is the simpler v1). Click-through, non-activating,
// auto-dismisses after `duration_ms` with a short fade.
void PhiShowToast(HWND owner,
                  const PhiPalette& palette,
                  const std::wstring& title,
                  const std::wstring& message = std::wstring(),
                  int duration_ms = 3000);

// The same toast with one action button at its trailing edge (the Mac's
// OverlayToastAction: Folio's Undo and Add Note). Such a toast takes clicks
// — only the button does anything — and still never takes focus. A click
// on the button runs `action` once and dismisses the toast.
void PhiShowToastWithAction(HWND owner,
                            const PhiPalette& palette,
                            const std::wstring& title,
                            const std::wstring& message,
                            int duration_ms,
                            const std::wstring& action_label,
                            std::function<void()> action);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_TOAST_H_

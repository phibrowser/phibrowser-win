// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_WINDOW_CAPTURE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_WINDOW_CAPTURE_H_

#include <windows.h>

#include <string>

namespace phi_client {

// Renders `shell` (a client-drawn shell window, visible or not) to a PNG at
// `out_path` — the Windows twin of the Mac's captureWindow compositing.
//
// The reparented browser child paints through DirectComposition, which
// PrintWindow cannot read (the same blindness the Mac hits with its GPU web
// surface), so the CALLER supplies the web content it already captured over
// CDP as `web_path`; it is drawn over `content_client` (the content rect in
// the shell's CLIENT coordinates), white-backed first because a CDP capture
// is transparent wherever the page paints no background. An empty `web_path`
// yields chrome only.
//
// False when the window cannot be rendered or the file cannot be written.
bool PhiCaptureWindowToPng(HWND shell,
                           const RECT& content_client,
                           const std::wstring& web_path,
                           const std::wstring& out_path);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_WINDOW_CAPTURE_H_

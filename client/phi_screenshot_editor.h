// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SCREENSHOT_EDITOR_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SCREENSHOT_EDITOR_H_

#include <windows.h>

#include <string>

#include "base/values.h"
#include "client/phi_screenshot_model.h"

namespace phi_client {

class PhiShellWindow;

// The screenshot editor (docs/page-screenshots.md "Editor"): a child panel
// over `shell`'s content card holding one document — the captured image and
// its annotations — with the Mac's tools, undo and PNG / JPG / PDF export.
// One editor at a time; it is self-owned and goes with its shell.
class PhiScreenshotEditor {
 public:
  // Opens over `shell`. An editor already up is replaced when it is clean;
  // a dirty one asks first, and a "Keep Editing" leaves it (and returns
  // false). `title` names exported files.
  // `device_pixel_ratio` is the capture's, which scales the default
  // stroke and text sizes (PhiScreenshotStyleUnit).
  static bool Open(PhiShellWindow* shell,
                   PhiScreenshotImageRef image,
                   const std::wstring& title,
                   double device_pixel_ratio = 2);
  static bool IsOpen();
  static bool OwnsFocus();
  // Closes; with unsaved edits and `discard` false, asks first. True when
  // the editor is gone.
  static bool RequestClose(bool discard);

  // The shell's lifecycle (no-ops unless the editor belongs to `shell`).
  static void CloseIfBoundTo(PhiShellWindow* shell);
  static void RepositionIfBoundTo(PhiShellWindow* shell);
  static void RaiseIfBoundTo(PhiShellWindow* shell);
  static bool IsBoundTo(PhiShellWindow* shell);
  // The shell was activated: the editor over it takes the keyboard. False
  // when there is none.
  static bool FocusIfBoundTo(PhiShellWindow* shell);

  // Test hooks.
  static base::DictValue StateForTest();
  static bool ExportForTest(const std::string& format,
                            const std::wstring& path,
                            base::DictValue* out,
                            std::string* error);
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SCREENSHOT_EDITOR_H_

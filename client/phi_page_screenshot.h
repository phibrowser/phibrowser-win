// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_PAGE_SCREENSHOT_H_
#define PHIBROWSER_WIN_CLIENT_PHI_PAGE_SCREENSHOT_H_

#include <string>

#include "base/values.h"
#include "client/phi_screenshot_model.h"

namespace phi_client {

class PhiShellWindow;

// Capture Full Page / Capture Area… (docs/page-screenshots.md): the Mac's
// PageScreenshotService over the framework's in-process DevTools
// transport. One capture runs at a time; a capture copies a PNG to the
// clipboard, offers Save in a toast and opens the screenshot editor.
class PhiPageScreenshot {
 public:
  enum class Mode { kFullPage, kArea };

  // Whether the menu rows are enabled for `shell`: a page in front of the
  // user that is not covered by a native page, and no capture running.
  static bool CanCapture(PhiShellWindow* shell);
  // Starts a capture of `shell`'s page in front of the user. A no-op when
  // one is already running or CanCapture is false.
  static void Start(PhiShellWindow* shell, Mode mode);
  // The test hook: as Start, but an area capture with `rect` (document CSS
  // px) skips the overlay. False with `error` when it could not start.
  static bool StartForTest(PhiShellWindow* shell,
                           Mode mode,
                           const PhiRectF* rect,
                           std::string* error);
  static bool Busy();
  // `shell` is going away: a capture of it is abandoned (no toast, no
  // clipboard write).
  static void ShellGoingAway(PhiShellWindow* shell);

  // What the last capture did and what is running now (the test verb).
  static base::DictValue StateForTest();
};

// While the area overlay or the editor has the keyboard, every chord is
// theirs: the client keyboard hook lets it through untouched.
bool PhiScreenshotOwnsKeyboard();

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_PAGE_SCREENSHOT_H_

// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SCREENSHOT_AREA_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SCREENSHOT_AREA_H_

#include <windows.h>

#include "base/values.h"
#include "client/phi_screenshot_model.h"

namespace phi_client {

class PhiShellWindow;

// What the area overlay asks of the capture that opened it.
class PhiScreenshotAreaHost {
 public:
  virtual ~PhiScreenshotAreaHost() = default;
  // Scroll the main document by (dx, dy) CSS px and answer with
  // PhiScreenshotArea::SetViewport once the page reports where it is.
  // At most one is in flight; the overlay coalesces the rest.
  virtual void AreaScroll(double dx, double dy) = 0;
  // The user finished a selection: `rect` in document CSS px.
  virtual void AreaDone(const PhiRectF& rect) = 0;
  // Escape, a right click, focus loss: nothing is captured.
  virtual void AreaCancel() = 0;
};

// Capture Area…'s overlay (the Mac's PageScreenshotRegionSelector): a
// layered child panel over the page's viewport that dims it, takes a drag,
// scrolls the document on the wheel and at the edges, and hands back the
// selection in document coordinates. One at a time.
class PhiScreenshotArea {
 public:
  // `viewport` is the page's rect in `shell`'s client pixels; `v` the page
  // metrics measured against it (v.left / v.top are 0: the overlay's own
  // pixels). False when the overlay could not be created.
  static bool Open(PhiShellWindow* shell,
                   const RECT& viewport,
                   const PhiScreenshotViewport& v,
                   PhiScreenshotAreaHost* host);
  // The page's metrics after an AreaScroll (scroll offsets moved).
  static void SetViewport(const PhiScreenshotViewport& v);
  // Closes without calling the host.
  static void Close();
  static bool IsOpen();
  static bool OwnsFocus();
  static void CloseIfBoundTo(PhiShellWindow* shell);
  // Back above the browser child (the shell's raise-caps turn).
  static void RaiseIfBoundTo(PhiShellWindow* shell);
  static bool IsBoundTo(PhiShellWindow* shell);
  // The shell was activated: the overlay over it takes the keyboard.
  static bool FocusIfBoundTo(PhiShellWindow* shell);
  static base::DictValue StateForTest();
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SCREENSHOT_AREA_H_

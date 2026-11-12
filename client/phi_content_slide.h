// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_CONTENT_SLIDE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_CONTENT_SLIDE_H_

#include <windows.h>

#include "base/memory/raw_ptr_exclusion.h"

namespace phi_client {

// What the slide asks of the shell window it animates.
class PhiContentSlideShell {
 public:
  virtual HWND ContentSlideWindow() const = 0;
  // A tick moved the entering side: compose the frame again.
  virtual void ContentSlideMoved() = 0;
  // The slide landed (or was cut short): the entering Space shows for real.
  virtual void ContentSlideLanded() = 0;

 protected:
  // The shell owns the slide, never the reverse.
  ~PhiContentSlideShell() = default;
};

// The COMFORTABLE layout's Space switch — the Mac's
// performHorizontalWindowSlide. There is no sidebar band to push in, so the
// whole client area goes: a composite of the LEAVING Space (its chrome and
// its page, as they were on screen at the switch) slides out one side while
// a composite of the ENTERING Space — its chrome painted by the shell, its
// page copied from the compositor — slides in from the other, and the
// caption buttons stand still over both the way the Mac's traffic lights
// do. 200 ms, ease-in-out; a switch forward (the entering Space later in
// the strip) comes in from the right.
//
// Both sides are pixels in the shell's own surface, deliberately. The page
// is a DirectComposition child window: moved along per tick it reaches the
// desktop compositor a frame apart from the chrome painted around it, and
// the two jitter against each other. The Mac slides its live views because
// they share one render tree; here the entering child stays parked until
// the slide lands, as the vertical layouts' band slide already keeps it.
//
// The vertical layouts' switch is the Spaces strip's band slide
// (PhiSpacesStrip::BeginSwap) and is untouched by this: the shell starts one
// or the other by layout.
//
// Life: Begin() takes the leaving composite and HOLDS at the start position
// (the frame still shows the leaving Space) while the shell fetches the
// entering page; SetEnteringPage() files it; Release() runs the slide.
class PhiContentSlide {
 public:
  explicit PhiContentSlide(PhiContentSlideShell* shell);
  ~PhiContentSlide();

  PhiContentSlide(const PhiContentSlide&) = delete;
  PhiContentSlide& operator=(const PhiContentSlide&) = delete;

  // Starts a slide over a `width` x `height` client area, taking ownership
  // of `leaving` (a compatible bitmap of that size), held at its start. Any
  // slide already running lands first.
  void Begin(HBITMAP leaving, int width, int height, bool forward);
  // The entering page's pixels — `page_width` x `page_height` BGRA rows
  // packed tight — laid at `at` (client coordinates) in the entering
  // composite. Without one the content rect keeps the shell's ground.
  void SetEnteringPage(const unsigned char* bgra,
                       int page_width,
                       int page_height,
                       const RECT& at);
  // Runs the held slide.
  void Release();
  // Lands the slide now (a resize, a superseding switch, the window going).
  void Cancel();
  // Begun and not yet landed: held or sliding.
  bool running() const { return leaving_ != nullptr; }
  bool holding() const { return running() && animation_ == 0 && !released_; }
  // Where the entering side is right now: its client x offset, 0 at rest.
  int dx() const { return dx_; }

  // The scratch the shell paints the entering chrome into, at rest
  // coordinates. Null when no slide runs.
  HDC EnteringDC();
  // Lays the entering page into the scratch (after the chrome is painted,
  // which leaves the content rect untouched).
  void LayEnteringPage();
  // Lays the entering composite at dx() and the leaving composite beside it
  // into `dc`, a client-sized frame.
  void Compose(HDC dc) const;

  static constexpr int kDurationMs = 200;

 private:
  void DropBitmaps();
  void Finish();

  RAW_PTR_EXCLUSION PhiContentSlideShell* shell_;
  HBITMAP leaving_ = nullptr;
  HDC leaving_dc_ = nullptr;
  HBITMAP entering_ = nullptr;
  HDC entering_dc_ = nullptr;
  HBITMAP page_ = nullptr;
  HDC page_dc_ = nullptr;
  int page_width_ = 0;
  int page_height_ = 0;
  RECT page_at_{};
  int width_ = 0;
  int height_ = 0;
  int start_dx_ = 0;  // Where the entering side began: +width or -width.
  int dx_ = 0;
  bool released_ = false;
  int animation_ = 0;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CONTENT_SLIDE_H_

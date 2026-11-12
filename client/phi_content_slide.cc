// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_content_slide.h"

#include <cmath>

#include "client/phi_animator.h"

namespace phi_client {

PhiContentSlide::PhiContentSlide(PhiContentSlideShell* shell)
    : shell_(shell) {}

PhiContentSlide::~PhiContentSlide() {
  if (animation_) {
    PhiAnimator::ForThread().Cancel(animation_);
    animation_ = 0;
  }
  DropBitmaps();
}

void PhiContentSlide::Begin(HBITMAP leaving,
                            int width,
                            int height,
                            bool forward) {
  Cancel();
  const HWND shell = shell_->ContentSlideWindow();
  if (!leaving || width <= 0 || height <= 0 || !shell) {
    if (leaving) {
      ::DeleteObject(leaving);
    }
    return;
  }
  HDC screen = ::GetDC(nullptr);
  leaving_dc_ = ::CreateCompatibleDC(screen);
  entering_dc_ = ::CreateCompatibleDC(screen);
  entering_ = ::CreateCompatibleBitmap(screen, width, height);
  ::ReleaseDC(nullptr, screen);
  if (!leaving_dc_ || !entering_dc_ || !entering_) {
    ::DeleteObject(leaving);
    DropBitmaps();
    return;
  }
  leaving_ = leaving;
  ::SelectObject(leaving_dc_, leaving_);
  ::SelectObject(entering_dc_, entering_);
  width_ = width;
  height_ = height;
  // The entering side starts one client width off — past the right edge
  // when the switch runs forward, past the left one when it runs back —
  // and the leaving composite goes out the opposite way. Held there until
  // Release().
  start_dx_ = forward ? width : -width;
  dx_ = start_dx_;
  released_ = false;
}

void PhiContentSlide::SetEnteringPage(const unsigned char* bgra,
                                      int page_width,
                                      int page_height,
                                      const RECT& at) {
  if (!running() || !bgra || page_width <= 0 || page_height <= 0) {
    return;
  }
  if (page_) {
    ::DeleteObject(page_);
    page_ = nullptr;
  }
  HDC screen = ::GetDC(nullptr);
  if (!page_dc_) {
    page_dc_ = ::CreateCompatibleDC(screen);
  }
  page_ = ::CreateCompatibleBitmap(screen, page_width, page_height);
  if (page_ && page_dc_) {
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = page_width;
    info.bmiHeader.biHeight = -page_height;  // Top-down, as the rows come.
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    ::SetDIBits(screen, page_, 0, static_cast<UINT>(page_height), bgra, &info,
                DIB_RGB_COLORS);
    ::SelectObject(page_dc_, page_);
    page_width_ = page_width;
    page_height_ = page_height;
    page_at_ = at;
  }
  ::ReleaseDC(nullptr, screen);
}

void PhiContentSlide::Release() {
  if (!running() || released_) {
    return;
  }
  released_ = true;
  animation_ = PhiAnimator::ForThread().Animate(
      shell_->ContentSlideWindow(), kDurationMs, PhiEasing::kEaseInOut,
      [this](double t, RECT*) {
        dx_ = static_cast<int>(std::lround(start_dx_ * (1.0 - t)));
        // The shell composes the frame itself (no dirty rect here): the
        // two composites are already painted, so a tick is two blits.
        shell_->ContentSlideMoved();
      },
      [this] {
        animation_ = 0;
        Finish();
      });
}

void PhiContentSlide::Cancel() {
  if (!running()) {
    return;
  }
  if (animation_) {
    PhiAnimator::ForThread().Cancel(animation_);
    animation_ = 0;
  }
  Finish();
}

HDC PhiContentSlide::EnteringDC() {
  return running() ? entering_dc_ : nullptr;
}

void PhiContentSlide::LayEnteringPage() {
  if (!running() || !page_ || !page_dc_) {
    return;
  }
  const int w = page_at_.right - page_at_.left;
  const int h = page_at_.bottom - page_at_.top;
  if (w <= 0 || h <= 0) {
    return;
  }
  if (w == page_width_ && h == page_height_) {
    ::BitBlt(entering_dc_, page_at_.left, page_at_.top, w, h, page_dc_, 0, 0,
             SRCCOPY);
  } else {
    ::SetStretchBltMode(entering_dc_, HALFTONE);
    ::SetBrushOrgEx(entering_dc_, 0, 0, nullptr);
    ::StretchBlt(entering_dc_, page_at_.left, page_at_.top, w, h, page_dc_, 0,
                 0, page_width_, page_height_, SRCCOPY);
  }
}

void PhiContentSlide::Compose(HDC dc) const {
  if (!running()) {
    return;
  }
  ::BitBlt(dc, dx_, 0, width_, height_, entering_dc_, 0, 0, SRCCOPY);
  // The leaving side's edge meets the entering side's: it left from rest
  // by exactly what the entering side has still to travel.
  ::BitBlt(dc, dx_ - start_dx_, 0, width_, height_, leaving_dc_, 0, 0,
           SRCCOPY);
}

void PhiContentSlide::DropBitmaps() {
  for (HDC* dc : {&leaving_dc_, &entering_dc_, &page_dc_}) {
    if (*dc) {
      ::DeleteDC(*dc);
      *dc = nullptr;
    }
  }
  for (HBITMAP* bitmap : {&leaving_, &entering_, &page_}) {
    if (*bitmap) {
      ::DeleteObject(*bitmap);
      *bitmap = nullptr;
    }
  }
  page_width_ = page_height_ = 0;
}

void PhiContentSlide::Finish() {
  dx_ = 0;
  start_dx_ = 0;
  released_ = false;
  DropBitmaps();
  shell_->ContentSlideLanded();
}

}  // namespace phi_client

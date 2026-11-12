// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_window_capture.h"

#include <objbase.h>
// gdiplus.h needs min/max and the ole headers above it.
#include <algorithm>

#include <gdiplus.h>

#include <memory>

#include "base/compiler_specific.h"
#include "client/phi_gdiplus.h"

#pragma comment(lib, "gdiplus.lib")

namespace phi_client {

namespace {

// CLSID of the built-in PNG encoder, resolved from the installed codec list.
bool PngEncoderClsid(CLSID* clsid) {
  UINT count = 0;
  UINT bytes = 0;
  if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok ||
      bytes == 0) {
    return false;
  }
  std::unique_ptr<char[]> buffer(new char[bytes]);
  auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.get());
  if (Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok) {
    return false;
  }
  for (UINT i = 0; i < count; ++i) {
    // SAFETY: `codecs` addresses `count` entries by GetImageEncodersSize's
    // own contract, which also sized the buffer above.
    const Gdiplus::ImageCodecInfo& codec = UNSAFE_BUFFERS(codecs[i]);
    if (std::wstring(codec.MimeType) == L"image/png") {
      *clsid = codec.Clsid;
      return true;
    }
  }
  return false;
}

}  // namespace

bool PhiCaptureWindowToPng(HWND shell,
                           const RECT& content_client,
                           const std::wstring& web_path,
                           const std::wstring& out_path) {
  if (!shell || !::IsWindow(shell) || out_path.empty() || !PhiEnsureGdiPlus()) {
    return false;
  }
  RECT client = {};
  ::GetClientRect(shell, &client);
  const int width = client.right - client.left;
  const int height = client.bottom - client.top;
  if (width <= 1 || height <= 1) {
    return false;
  }

  HDC screen = ::GetDC(nullptr);
  HDC mem = ::CreateCompatibleDC(screen);
  HBITMAP bitmap = ::CreateCompatibleBitmap(screen, width, height);
  ::ReleaseDC(nullptr, screen);
  if (!mem || !bitmap) {
    if (mem) {
      ::DeleteDC(mem);
    }
    if (bitmap) {
      ::DeleteObject(bitmap);
    }
    return false;
  }
  HGDIOBJ previous = ::SelectObject(mem, bitmap);

  // PW_RENDERFULLCONTENT asks even DirectComposition-backed children to
  // paint; the reparented browser child still comes back blank (hence the
  // web_path composite below), but the client's own chrome renders.
  bool rendered =
      ::PrintWindow(shell, mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT) != FALSE;
  if (!rendered) {
    rendered = ::PrintWindow(shell, mem, PW_CLIENTONLY) != FALSE;
  }

  if (rendered && !web_path.empty()) {
    std::unique_ptr<Gdiplus::Bitmap> web(
        Gdiplus::Bitmap::FromFile(web_path.c_str(), FALSE));
    if (web && web->GetLastStatus() == Gdiplus::Ok) {
      Gdiplus::Graphics graphics(mem);
      const Gdiplus::Rect target(
          content_client.left, content_client.top,
          content_client.right - content_client.left,
          content_client.bottom - content_client.top);
      if (target.Width > 0 && target.Height > 0) {
        // A real browser shows white where the page paints nothing, not the
        // chrome behind it.
        Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
        graphics.FillRectangle(&white, target);
        graphics.SetInterpolationMode(
            Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.DrawImage(web.get(), target);
      }
    }
  }

  bool saved = false;
  if (rendered) {
    CLSID png = {};
    if (PngEncoderClsid(&png)) {
      Gdiplus::Bitmap shot(bitmap, nullptr);
      saved = shot.GetLastStatus() == Gdiplus::Ok &&
              shot.Save(out_path.c_str(), &png, nullptr) == Gdiplus::Ok;
    }
  }

  ::SelectObject(mem, previous);
  ::DeleteObject(bitmap);
  ::DeleteDC(mem);
  return saved;
}

}  // namespace phi_client

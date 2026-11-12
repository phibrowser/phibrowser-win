// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_glyph_bitmap.h"

#include <algorithm>
#include <cstring>
#include <iterator>

#include "base/compiler_specific.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_dwrite.h"
#include "client/phi_icons.h"

namespace phi_client {

namespace {

// A top-down 32bpp DIB with a memory DC over it, every byte `fill`.
struct Surface {
  HBITMAP dib = nullptr;
  HGDIOBJ previous = nullptr;
  HDC dc = nullptr;
  // The section's pixels, owned by `dib`; a plain pointer by design.
  RAW_PTR_EXCLUSION unsigned char* bits = nullptr;
};

Surface MakeSurface(int px, unsigned char fill) {
  Surface surface;
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = px;
  info.bmiHeader.biHeight = -px;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  surface.dib =
      ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!surface.dib || !bits) {
    if (surface.dib) {
      ::DeleteObject(surface.dib);
    }
    return Surface();
  }
  surface.bits = static_cast<unsigned char*>(bits);
  // SAFETY: the section was sized px*px*4 above.
  UNSAFE_BUFFERS(
      std::memset(bits, fill, static_cast<size_t>(px) * px * 4));
  surface.dc = ::CreateCompatibleDC(nullptr);
  if (!surface.dc) {
    ::DeleteObject(surface.dib);
    return Surface();
  }
  surface.previous = ::SelectObject(surface.dc, surface.dib);
  return surface;
}

// Tears the DC down; keeps the bitmap when `keep_dib`.
void Release(Surface* surface, bool keep_dib) {
  if (surface->dc) {
    if (surface->previous) {
      ::SelectObject(surface->dc, surface->previous);
    }
    ::DeleteDC(surface->dc);
  }
  if (surface->dib && !keep_dib) {
    ::DeleteObject(surface->dib);
  }
  *surface = Surface();
}

// The sidebar's ladder, onto whatever `dc` already holds.
void DrawGlyph(HDC dc, int px, const std::wstring& glyph, COLORREF ink) {
  const RECT rc = {0, 0, px, px};
  if (PhiDrawCatalogArtwork(dc, rc, glyph, px, ink)) {
    return;
  }
  const int em = std::max(1, px * 13 / 16);
  if (!PhiIsPrivateGlyph(glyph) && PhiDrawColorText(dc, rc, glyph, em, ink)) {
    return;
  }
  LOGFONTW lf = {};
  lf.lfHeight = -em;
  lf.lfCharSet = DEFAULT_CHARSET;
  const std::wstring face =
      PhiIsPrivateGlyph(glyph) ? PhiIconFaceName() : L"Segoe UI Emoji";
  std::copy_n(face.begin(), std::min(face.size(), std::size(lf.lfFaceName) - 1),
              std::begin(lf.lfFaceName));
  HFONT font = ::CreateFontIndirectW(&lf);
  HGDIOBJ previous = font ? ::SelectObject(dc, font) : nullptr;
  ::SetBkMode(dc, TRANSPARENT);
  ::SetTextColor(dc, ink);
  RECT text_rc = rc;
  ::DrawTextW(dc, glyph.c_str(), -1, &text_rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  if (previous) {
    ::SelectObject(dc, previous);
  }
  if (font) {
    ::DeleteObject(font);
  }
}

}  // namespace

HBITMAP PhiSpaceGlyphBitmap(const std::wstring& glyph, int px, COLORREF ink) {
  if (glyph.empty() || px <= 0) {
    return nullptr;
  }
  // None of the draw paths yields alpha directly (D2D clears to a sampled
  // background, GDI text has none), so draw twice — over black and over
  // white — and recover coverage from how much white leaked through: every
  // path composites, so the black render IS the premultiplied colour and
  // alpha = 255 - (white - black).
  Surface on_black = MakeSurface(px, 0x00);
  Surface on_white = MakeSurface(px, 0xFF);
  Surface out = MakeSurface(px, 0x00);
  if (!on_black.bits || !on_white.bits || !out.bits) {
    Release(&on_black, false);
    Release(&on_white, false);
    Release(&out, false);
    return nullptr;
  }
  DrawGlyph(on_black.dc, px, glyph, ink);
  DrawGlyph(on_white.dc, px, glyph, ink);
  ::GdiFlush();
  const size_t count = static_cast<size_t>(px) * px;
  for (size_t i = 0; i < count; ++i) {
    // SAFETY: all three sections are px*px*4 bytes; i < px*px.
    UNSAFE_BUFFERS({
      const unsigned char* b = on_black.bits + i * 4;
      const unsigned char* w = on_white.bits + i * 4;
      unsigned char* o = out.bits + i * 4;
      const int leaked = ((w[0] - b[0]) + (w[1] - b[1]) + (w[2] - b[2])) / 3;
      const int alpha = std::clamp(255 - leaked, 0, 255);
      o[0] = static_cast<unsigned char>(std::min<int>(b[0], alpha));
      o[1] = static_cast<unsigned char>(std::min<int>(b[1], alpha));
      o[2] = static_cast<unsigned char>(std::min<int>(b[2], alpha));
      o[3] = static_cast<unsigned char>(alpha);
    });
  }
  Release(&on_black, false);
  Release(&on_white, false);
  HBITMAP result = out.dib;
  Release(&out, true);
  return result;
}

void PhiSetMenuItemBitmap(HMENU menu, UINT id, HBITMAP bitmap) {
  if (!menu || !bitmap) {
    return;
  }
  MENUITEMINFOW info = {};
  info.cbSize = sizeof(info);
  info.fMask = MIIM_BITMAP;
  info.hbmpItem = bitmap;
  ::SetMenuItemInfoW(menu, id, FALSE, &info);
}

}  // namespace phi_client

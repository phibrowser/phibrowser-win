// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_fluent.h"

#include <algorithm>
#include <iterator>
#include <string>

#include "client/phi_dwrite.h"

namespace phi_client {

namespace {

bool FaceInstalled(const wchar_t* face) {
  LOGFONTW probe = {};
  probe.lfCharSet = DEFAULT_CHARSET;
  const std::wstring name = face;
  std::copy_n(name.begin(),
              std::min(name.size(), std::size(probe.lfFaceName) - 1),
              std::begin(probe.lfFaceName));
  bool found = false;
  HDC screen = ::GetDC(nullptr);
  if (!screen) {
    return false;
  }
  ::EnumFontFamiliesExW(
      screen, &probe,
      [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) {
        *reinterpret_cast<bool*>(found) = true;
        return 0;
      },
      reinterpret_cast<LPARAM>(&found), 0);
  ::ReleaseDC(nullptr, screen);
  return found;
}

void SetFace(LOGFONTW* lf, const wchar_t* face) {
  const std::wstring name = face;
  std::fill(std::begin(lf->lfFaceName), std::end(lf->lfFaceName), L'\0');
  std::copy_n(name.begin(),
              std::min(name.size(), std::size(lf->lfFaceName) - 1),
              std::begin(lf->lfFaceName));
}

}  // namespace

const wchar_t* PhiTextFaceName(bool display) {
  static const bool variable = FaceInstalled(L"Segoe UI Variable Text");
  if (!variable) {
    return L"Segoe UI";
  }
  return display ? L"Segoe UI Variable Display" : L"Segoe UI Variable Text";
}

HFONT PhiFluentFont(UINT dpi, int size, int weight) {
  LOGFONTW lf = {};
  // Segoe UI Variable exposes its weights on the one family; plain Segoe UI
  // keeps semibold as a face of its own, so name it there.
  const bool display = size >= 20;
  const wchar_t* face = PhiTextFaceName(display);
  if (weight >= FW_SEMIBOLD && std::wstring_view(face) == L"Segoe UI") {
    face = L"Segoe UI Semibold";
  }
  SetFace(&lf, face);
  lf.lfHeight = -::MulDiv(size, static_cast<int>(dpi), 96);
  lf.lfWeight = weight;
  lf.lfCharSet = DEFAULT_CHARSET;
  lf.lfQuality = CLEARTYPE_QUALITY;
  return ::CreateFontIndirectW(&lf);
}

HFONT PhiIconFont(UINT dpi, int size) {
  LOGFONTW lf = {};
  SetFace(&lf, PhiIconFaceName());
  lf.lfHeight = -::MulDiv(size, static_cast<int>(dpi), 96);
  lf.lfWeight = FW_NORMAL;
  lf.lfCharSet = DEFAULT_CHARSET;
  return ::CreateFontIndirectW(&lf);
}

COLORREF PhiButtonInk(PhiButtonState state,
                      const PhiPalette& palette,
                      COLORREF ink) {
  switch (state) {
    case PhiButtonState::kPressed:
      // The icon at 75 %: the ink pulled a quarter of the way to the ground.
      return PhiBlend(ink, palette.ground_top, 191);
    case PhiButtonState::kDisabled:
      return PhiBlend(ink, palette.ground_top, 92);  // 36 %.
    case PhiButtonState::kToggled:
      return palette.accent;
    case PhiButtonState::kRest:
    case PhiButtonState::kHover:
      break;
  }
  return ink;
}

void PhiPaintButtonWash(HDC dc,
                        const RECT& rect,
                        PhiButtonState state,
                        const PhiPalette& palette,
                        int radius_px) {
  if (rect.right <= rect.left || rect.bottom <= rect.top) {
    return;
  }
  switch (state) {
    case PhiButtonState::kHover:
      PhiFillRoundRectAA(dc, rect, radius_px, palette.hover);
      break;
    case PhiButtonState::kPressed:
      PhiFillRoundRectAA(dc, rect, radius_px, palette.pressed);
      break;
    case PhiButtonState::kToggled:
      PhiFillRoundRectAA(dc, rect, radius_px, palette.accent_soft);
      break;
    case PhiButtonState::kRest:
    case PhiButtonState::kDisabled:
      break;
  }
}

void PhiPaintIconButton(HDC dc,
                        const RECT& rect,
                        const wchar_t* glyph,
                        PhiButtonState state,
                        const PhiPalette& palette,
                        COLORREF ink,
                        int glyph_px,
                        int radius_px) {
  if (rect.right <= rect.left || rect.bottom <= rect.top || !glyph) {
    return;
  }
  PhiPaintButtonWash(dc, rect, state, palette, radius_px);
  const COLORREF color = PhiButtonInk(state, palette, ink);
  if (PhiDrawIconGlyph(dc, rect, glyph, glyph_px, color)) {
    return;
  }
  // No DirectWrite: GDI in the icon face at the pixel size asked for. One
  // font per call is fine on this path — it only runs without D2D.
  LOGFONTW lf = {};
  SetFace(&lf, PhiIconFaceName());
  lf.lfHeight = -glyph_px;
  lf.lfCharSet = DEFAULT_CHARSET;
  HFONT font = ::CreateFontIndirectW(&lf);
  HGDIOBJ previous = font ? ::SelectObject(dc, font) : nullptr;
  ::SetBkMode(dc, TRANSPARENT);
  ::SetTextColor(dc, color);
  RECT box = rect;
  ::DrawTextW(dc, glyph, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  if (previous) {
    ::SelectObject(dc, previous);
  }
  if (font) {
    ::DeleteObject(font);
  }
}

}  // namespace phi_client

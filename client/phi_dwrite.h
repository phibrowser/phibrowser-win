// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_DWRITE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_DWRITE_H_

#include <windows.h>

#include <string>
#include <string_view>

#include "base/functional/function_ref.h"

namespace phi_client {

// THE icon face (docs/windows-native-ui.md §2.2a): "Segoe Fluent Icons"
// when the system collection has it (Windows 11), else "Segoe MDL2 Assets"
// (Windows 10) — the same code points in both. Resolved once; every glyph
// site names its font through this, never through a literal.
const wchar_t* PhiIconFaceName();
// True when `face` is the icon face (either spelling).
bool PhiIsIconFace(std::wstring_view face);

// Draws `text` centred in `rect` with COLOR-FONT rendering (Segoe UI Emoji's
// COLR layers) — what GDI's DrawText cannot do. The pixel under the rect's
// corner is read back as the background, so callers paint their fills first.
// Returns false when Direct2D/DirectWrite are unavailable; the caller then
// falls back to the monochrome GDI path.
bool PhiDrawColorText(HDC dc,
                      const RECT& rect,
                      const std::wstring& text,
                      int pixel_size,
                      COLORREF ink);

// Draws `text` so its INK — not its em box — spans `ink_size` pixels,
// centred in `rect`. Use it wherever a run of glyphs has to read as ONE
// size: every emoji covers a different part of its em, so PhiDrawColorText
// at a fixed size renders them visibly unequal. Same fallback contract.
bool PhiDrawColorIcon(HDC dc,
                      const RECT& rect,
                      const std::wstring& text,
                      int ink_size,
                      COLORREF ink);

// Draws one MONOCHROME icon glyph (a Segoe MDL2 Assets code point) centred
// in `rect` at `pixel_size` em, in `ink`. DirectWrite rasterizes it with
// grayscale coverage in BOTH axes and composites through the alpha blend,
// so it lands over whatever the caller already painted (a hover wash, a
// tab fill) — where GDI's DrawText renders these with ClearType, which
// smooths horizontally only and fringes an icon's curves and diagonals.
// Placement mirrors DT_CENTER | DT_VCENTER so a call site can swap either
// way. Returns false when DirectWrite is unavailable OR the face has no
// such glyph; the caller then falls back to the GDI path.
bool PhiDrawIconGlyph(HDC dc,
                      const RECT& rect,
                      const std::wstring& glyph,
                      int pixel_size,
                      COLORREF ink);

// A DrawTextW stand-in for icon sites. When the DC's selected font is the
// Segoe MDL2 Assets face and `text` is ONE private-use code point, the glyph
// goes through PhiDrawIconGlyph at the font's own em size, in the DC's
// current text colour, placed per DT_LEFT / DT_CENTER / DT_RIGHT and
// DT_TOP / DT_VCENTER / DT_BOTTOM — so a site keeps its GDI font plumbing
// and loses the ClearType fringe. DT_WORDBREAK text, measured or drawn, gets
// its line breaks from PhiWrapTextLines first, so measure and draw it through
// here. Anything else (plain text, DT_CALCRECT, a missing face) is handed to
// DrawTextW unchanged. Returns what DrawTextW returns (the text height), or
// the em size after a DirectWrite draw.
int PhiDrawTextAA(HDC dc,
                  const wchar_t* text,
                  int length,
                  RECT* rect,
                  UINT format);

// Draws a PREMULTIPLIED BGRA buffer scaled into `dest` with high-quality
// interpolation — crisp favicons and logos at any DPI, where GDI's
// StretchDIBits stair-steps. Same fallback contract as PhiDrawColorText.
bool PhiDrawBgra(HDC dc,
                 const RECT& dest,
                 const unsigned char* bgra,
                 int width,
                 int height);

// Alpha-blends a PREMULTIPLIED BGRA buffer over whatever the DC already
// holds — no background read-back, so it survives gradients and curved
// fills where PhiDrawBgra's flat-colour assumption breaks. GDI blending is
// unfiltered, so pass a buffer already at `dest`'s pixel size.
bool PhiBlendBgra(HDC dc,
                  const RECT& dest,
                  const unsigned char* bgra,
                  int width,
                  int height);

// ---------------------------------------------------------------------------
// Anti-aliased shapes. Every GDI shape primitive (RoundRect, Ellipse, a pen
// around either) lands on the pixel grid stair-stepped; these rasterise the
// shape's coverage — through Direct2D, or the software distance field
// behind it — and alpha-blend it over the DC, so a pill, a card corner or a
// dot reads as a curve at any DPI, over whatever was already painted there.
// All of them take TRUE geometry: `radius` is the corner radius (GDI's
// RoundRect takes the corner ellipse's DIAMETER — half the values here),
// and a fill covers exactly `rect` (GDI's NULL_PEN fill stops a pixel short
// on the right and bottom). Strokes hug the INSIDE of the boundary. They
// never fail for a drawable rect: when no blend surface can be built at all
// they draw the same shape with GDI, so call sites carry no fallback of
// their own.

// A rounded rectangle of corner `radius`; a radius of half the height is a
// capsule (pill). Large rects cost little: the straight runs fill through
// GDI and only the corner tiles are software-rendered.
bool PhiFillRoundRectAA(HDC dc, const RECT& rect, int radius, COLORREF fill);
bool PhiStrokeRoundRectAA(HDC dc,
                          const RECT& rect,
                          int radius,
                          COLORREF ink,
                          int width);

// Capsule (corner radius = half the height) fill and stroke.
bool PhiFillCapsuleAA(HDC dc, const RECT& rect, COLORREF fill);
bool PhiStrokeCapsuleAA(HDC dc, const RECT& rect, COLORREF ink, int width);

// The ellipse inscribed in `rect` (a circle for a square rect).
bool PhiFillEllipseAA(HDC dc, const RECT& rect, COLORREF fill);
bool PhiStrokeEllipseAA(HDC dc, const RECT& rect, COLORREF ink, int width);

// Dashed strokes: `dash` pixels of ink then `gap` pixels of air along the
// perimeter, the period stretched slightly so the pattern closes on itself
// instead of leaving an odd stub at the seam.
bool PhiStrokeRoundRectDashedAA(HDC dc,
                                const RECT& rect,
                                int radius,
                                COLORREF ink,
                                int width,
                                int dash,
                                int gap);
bool PhiStrokeEllipseDashedAA(HDC dc,
                              const RECT& rect,
                              COLORREF ink,
                              int width,
                              int dash,
                              int gap);

// Paints `outside` over the part of `rect` OUTSIDE the rounded rectangle of
// `radius` — the corner bites, antialiased. For content that cannot be
// drawn rounded in the first place (a gradient built from FillRect bands,
// a bitmap): draw it square, then knock the corners back to the ground
// colour with this, instead of clipping to a stair-stepped region.
bool PhiKnockOutRoundRectAA(HDC dc,
                            const RECT& rect,
                            int radius,
                            COLORREF outside);

// ---------------------------------------------------------------------------
// Shaped layered windows. A window region (SetWindowRgn) cuts a shape on
// the pixel grid — every rounded corner it makes is stair-stepped. A
// WS_EX_LAYERED window (top-level anywhere; a child since Windows 8) can
// carry per-pixel alpha instead, so its edge is a coverage ramp like any
// other antialiased shape.

// Commits `paint`'s output as the layered window's surface, `width` x
// `height` pixels: `paint` draws OPAQUE content into a 32-bit DC exactly as
// a WM_PAINT handler would (fills, ClearType text, bitmaps), then each
// pixel's alpha is `coverage(x, y)` (0..1, sampled at the pixel centre) —
// the window's shape — and its colour is premultiplied to match. The window
// must have WS_EX_LAYERED; once committed it no longer paints through
// WM_PAINT, so call this wherever the old code invalidated. Returns false
// when the surface or UpdateLayeredWindow fails, in which case the caller
// keeps its region-and-paint path. `alpha` scales the whole surface, for a
// window that fades; such a window must not call SetLayeredWindowAttributes,
// which makes every later UpdateLayeredWindow fail.
bool PhiCommitShapedLayer(HWND hwnd,
                          int width,
                          int height,
                          base::FunctionRef<void(HDC)> paint,
                          base::FunctionRef<double(double, double)> coverage,
                          BYTE alpha = 255);

// Coverage (0..1) at (`px`, `py`) of a rounded rectangle of `radius` filling
// `width` x `height` — for PhiCommitShapedLayer callers whose shape is the
// window's own rounded outline. `corners` masks which corners round, bits
// 0..3 = top-left, top-right, bottom-left, bottom-right.
double PhiRoundRectCoverage(double px,
                            double py,
                            double width,
                            double height,
                            double radius,
                            unsigned corners = 0xF);

// A content-card corner cap: a `radius`-square layered child that shows
// `tint` OUTSIDE the quarter circle of `radius` centred on the cap's inner
// corner (`corner` 0..3 = top-left, top-right, bottom-left, bottom-right,
// naming which corner of the CARD it caps) and nothing inside it, so the
// page beneath reads as a rounded card. `ring` is the card's 1px hairline,
// continued around the arc just outside the circle — the same circle a
// PhiStrokeRoundRectAA frame of `radius` + 1 around the card follows.
// Returns false when the layer cannot be committed.
bool PhiCommitCornerCap(HWND cap,
                        int corner,
                        int radius,
                        COLORREF tint,
                        COLORREF ring);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_DWRITE_H_

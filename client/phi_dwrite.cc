// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_dwrite.h"

#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/no_destructor.h"
#include "phibrowser-win/client/phi_text_wrap.h"

namespace phi_client {

namespace {

using Microsoft::WRL::ComPtr;

// A string's INK — the pixels it actually covers — inside a centre-aligned
// layout box: its size, and how far its centre sits from the box's. Both
// are properties of the glyph and its em size alone (they cancel the box
// size out), which is what lets one measurement serve every draw.
struct InkMetrics {
  float dx = 0.0f;
  float dy = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
};

// A 32bpp top-down scratch surface — a DIB with a memory DC over it — that
// every alpha-blended draw in this file borrows. Held for the process and
// grown to the largest shape asked for: the shell paints these on every
// frame, and a CreateDIBSection per call is exactly the allocation this
// layer must not make. UI thread only, and used by one draw at a time.
struct ScratchSurface {
  HDC dc = nullptr;
  HBITMAP dib = nullptr;
  // The section's pixels, owned by `dib`; a plain pointer by design.
  RAW_PTR_EXCLUSION unsigned char* bits = nullptr;
  int width = 0;
  int height = 0;
};

// Past this the surface is not worth holding for the process: the shapes
// this layer draws are chips, pills, rings and card corners, all far under
// it. A larger request takes the per-call path instead.
constexpr int kMaxScratch = 1024;

// A shell frame fills from a small palette, so the straight runs of every
// rounded rectangle can share brushes rather than create one per FillRect.
// The caps are insurance against a caller sweeping a gradient ramp.
constexpr size_t kMaxBrushes = 64;
constexpr size_t kMaxDashStyles = 32;

// Shared, lazily created, never torn down (process-lifetime singletons).
struct DWriteState {
  ComPtr<ID2D1Factory> d2d;
  ComPtr<IDWriteFactory> dwrite;
  ComPtr<ID2D1DCRenderTarget> target;
  // The ink brush the text target draws with, kept rather than created per
  // call: every emoji and icon draw needs exactly one.
  ComPtr<ID2D1SolidColorBrush> text_brush;
  bool target_failed = false;
  // The shape rasteriser: a PREMULTIPLIED target over the scratch surface,
  // plus the brush and dash styles a shape draw would otherwise allocate on
  // every frame.
  ComPtr<ID2D1DCRenderTarget> shape_target;
  ComPtr<ID2D1SolidColorBrush> shape_brush;
  bool shape_failed = false;
  bool shape_validated = false;
  std::map<std::pair<float, float>, ComPtr<ID2D1StrokeStyle>> dash_styles;
  ScratchSurface scratch;
  std::map<COLORREF, HBRUSH> brushes;
  std::map<int, ComPtr<IDWriteTextFormat>> formats;  // By pixel size.
  // The icon face (Segoe MDL2 Assets), resolved once: glyph rasterization
  // goes straight through the font face, no layout in the way.
  ComPtr<IDWriteFontFace> icon_face;
  bool icon_face_failed = false;
  // What a string's ink measures, and where it sits, by (em size, text).
  std::map<std::pair<int, std::wstring>, InkMetrics> ink_metrics;
  bool failed = false;
};

DWriteState& State() {
  static base::NoDestructor<DWriteState> state;
  return *state;
}

// The two factories. Split from the render targets so a target that cannot
// be made does not also take the other one — and so the shape rasteriser
// can be had on a machine whose text target failed, and the other way round.
bool EnsureFactories() {
  DWriteState& state = State();
  if (state.failed) {
    return false;
  }
  if (state.d2d && state.dwrite) {
    return true;
  }
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               state.d2d.GetAddressOf()))) {
    state.failed = true;
    return false;
  }
  if (FAILED(DWriteCreateFactory(
          DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
          reinterpret_cast<IUnknown**>(state.dwrite.GetAddressOf())))) {
    state.failed = true;
    return false;
  }
  return true;
}

bool EnsureState() {
  DWriteState& state = State();
  if (!EnsureFactories()) {
    return false;
  }
  if (state.target) {
    return true;
  }
  if (state.target_failed) {
    return false;
  }
  // 96 DPI: one DIP == one pixel, so callers pass physical pixel sizes.
  // SOFTWARE, as for the shape target: these draws are an icon or a
  // thumbnail into a GDI DC, where a GPU target pays a submit and a readback
  // per call and puts different pixels on different machines.
  const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_IGNORE),
      96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
  if (FAILED(state.d2d->CreateDCRenderTarget(&properties,
                                             state.target.GetAddressOf())) ||
      FAILED(state.target->CreateSolidColorBrush(
          D2D1::ColorF(D2D1::ColorF::Black),
          state.text_brush.GetAddressOf()))) {
    state.target.Reset();
    state.text_brush.Reset();
    state.target_failed = true;
    return false;
  }
  return true;
}

// The scratch surface, at least `width` x `height`. Null when it cannot be
// had — an outsized request, or GDI out of objects — and the caller then
// takes its own per-call path.
ScratchSurface* EnsureScratch(int width, int height) {
  if (width <= 0 || height <= 0 || width > kMaxScratch ||
      height > kMaxScratch) {
    return nullptr;
  }
  DWriteState& state = State();
  ScratchSurface& scratch = state.scratch;
  if (scratch.dib && scratch.width >= width && scratch.height >= height) {
    return &scratch;
  }
  // Grown in both axes at once, so a wide shape and a tall one do not trade
  // the surface back and forth.
  const int w = std::max(width, scratch.width);
  const int h = std::max(height, scratch.height);
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = w;
  info.bmiHeader.biHeight = -h;  // Top-down.
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib =
      ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) {
    if (dib) {
      ::DeleteObject(dib);
    }
    return nullptr;
  }
  if (!scratch.dc) {
    // Created once and never destroyed: the DC outlives every draw, and a
    // stable handle is worth more than putting its original bitmap back.
    scratch.dc = ::CreateCompatibleDC(nullptr);
    if (!scratch.dc) {
      ::DeleteObject(dib);
      return nullptr;
    }
  }
  if (!::SelectObject(scratch.dc, dib)) {
    ::DeleteObject(dib);
    return nullptr;
  }
  if (scratch.dib) {
    ::DeleteObject(scratch.dib);  // No longer selected: the line above.
  }
  scratch.dib = dib;
  scratch.bits = static_cast<unsigned char*>(bits);
  scratch.width = w;
  scratch.height = h;
  return &scratch;
}

// A solid brush of `color`, cached: FillRect on a paint path should not be
// creating a GDI object. Null only when GDI has none left.
HBRUSH BrushFor(COLORREF color) {
  DWriteState& state = State();
  auto found = state.brushes.find(color);
  if (found != state.brushes.end()) {
    return found->second;
  }
  if (state.brushes.size() >= kMaxBrushes) {
    for (const auto& [unused, brush] : state.brushes) {
      ::DeleteObject(brush);
    }
    state.brushes.clear();
  }
  HBRUSH brush = ::CreateSolidBrush(color);
  if (!brush) {
    return nullptr;
  }
  return state.brushes.emplace(color, brush).first->second;
}

IDWriteTextFormat* FormatFor(int pixel_size) {
  DWriteState& state = State();
  auto it = state.formats.find(pixel_size);
  if (it != state.formats.end()) {
    return it->second.Get();
  }
  ComPtr<IDWriteTextFormat> format;
  if (FAILED(state.dwrite->CreateTextFormat(
          L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
          static_cast<FLOAT>(pixel_size), L"", format.GetAddressOf()))) {
    return nullptr;
  }
  format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  return state.formats.emplace(pixel_size, std::move(format))
      .first->second.Get();
}

// How far a string's INK sits from the centre of a centre-aligned layout
// box. A glyph's ink is rarely centred on its advance — colour emoji in
// particular ride low and wide in their line box — so a box centred by
// metrics still reads off-centre inside a pill. The correction cancels the
// box size out (it depends only on the glyph and its size), so one
// measurement per string serves every draw; the map keeps it off the paint
// path after the first.
const InkMetrics& MetricsFor(const std::wstring& text, int pixel_size) {
  DWriteState& state = State();
  const auto key = std::make_pair(pixel_size, text);
  auto cached = state.ink_metrics.find(key);
  if (cached != state.ink_metrics.end()) {
    return cached->second;
  }
  // Any box measures the same ink; a generous one keeps a wide glyph off
  // the wrap.
  const float box = static_cast<float>(pixel_size) * 4.0f;
  InkMetrics metrics;
  IDWriteTextFormat* format = FormatFor(pixel_size);
  ComPtr<IDWriteTextLayout> layout;
  DWRITE_OVERHANG_METRICS overhang = {};
  if (format &&
      SUCCEEDED(state.dwrite->CreateTextLayout(
          text.c_str(), static_cast<UINT32>(text.size()), format, box, box,
          layout.GetAddressOf())) &&
      SUCCEEDED(layout->GetOverhangMetrics(&overhang))) {
    // Each overhang is the ink's distance beyond that edge of the box: their
    // sum with the box is the ink's own size, and half the difference
    // between opposite pairs is how far off centre it sits. A font that
    // reports no ink bounds gives zeros and nothing moves; half the glyph
    // caps anything wilder.
    const float limit = pixel_size / 2.0f;
    metrics.dx =
        std::clamp((overhang.left - overhang.right) / 2.0f, -limit, limit);
    metrics.dy =
        std::clamp((overhang.top - overhang.bottom) / 2.0f, -limit, limit);
    metrics.width = std::max(0.0f, box + overhang.left + overhang.right);
    metrics.height = std::max(0.0f, box + overhang.top + overhang.bottom);
  }
  return state.ink_metrics.emplace(key, metrics).first->second;
}

}  // namespace

const wchar_t* PhiIconFaceName() {
  // Fluent when installed (Windows 11 ships it; Windows 10 does not), MDL2
  // otherwise. GDI's own font mapper answers the presence question without
  // DirectWrite, so a machine with no D2D still picks the right face.
  static const wchar_t* const face = [] {
    LOGFONTW probe = {};
    probe.lfCharSet = DEFAULT_CHARSET;
    const std::wstring fluent = L"Segoe Fluent Icons";
    std::copy_n(fluent.begin(),
                std::min(fluent.size(), std::size(probe.lfFaceName) - 1),
                std::begin(probe.lfFaceName));
    bool found = false;
    HDC screen = ::GetDC(nullptr);
    if (screen) {
      ::EnumFontFamiliesExW(
          screen, &probe,
          [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) {
            *reinterpret_cast<bool*>(found) = true;
            return 0;
          },
          reinterpret_cast<LPARAM>(&found), 0);
      ::ReleaseDC(nullptr, screen);
    }
    return found ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
  }();
  return face;
}

bool PhiIsIconFace(std::wstring_view face) {
  return face == L"Segoe Fluent Icons" || face == L"Segoe MDL2 Assets";
}

namespace {

// The icon face, created on first use. A missing face (a stripped Windows
// install) fails once and stays failed — every draw then takes the caller's
// GDI path.
IDWriteFontFace* IconFace() {
  DWriteState& state = State();
  if (state.icon_face) {
    return state.icon_face.Get();
  }
  if (state.icon_face_failed) {
    return nullptr;
  }
  state.icon_face_failed = true;  // Cleared on success below.
  ComPtr<IDWriteFontCollection> collection;
  UINT32 index = 0;
  BOOL exists = FALSE;
  ComPtr<IDWriteFontFamily> family;
  ComPtr<IDWriteFont> font;
  if (FAILED(state.dwrite->GetSystemFontCollection(collection.GetAddressOf(),
                                                   FALSE)) ||
      FAILED(collection->FindFamilyName(PhiIconFaceName(), &index,
                                        &exists)) ||
      !exists ||
      FAILED(collection->GetFontFamily(index, family.GetAddressOf())) ||
      FAILED(family->GetFirstMatchingFont(
          DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
          DWRITE_FONT_STYLE_NORMAL, font.GetAddressOf())) ||
      FAILED(font->CreateFontFace(state.icon_face.GetAddressOf()))) {
    state.icon_face.Reset();
    return nullptr;
  }
  state.icon_face_failed = false;
  return state.icon_face.Get();
}

// Drops the text target so the next call rebuilds it. The brush is a
// device-dependent resource of that target and cannot outlive it.
void DropTextTarget() {
  DWriteState& state = State();
  state.target.Reset();
  state.text_brush.Reset();
}

D2D1_COLOR_F ToColor(COLORREF color) {
  return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
                      GetBValue(color) / 255.0f);
}

}  // namespace

namespace {

// The ground colour the caller already painted under `rect`, sampled just
// inside its corner — pulled inside the DC's clip box first: a pixel outside
// the clip reads CLR_INVALID (a pip half-hidden by the Spaces strip's
// viewport clip would otherwise clear to white).
// `centre` samples the middle instead — for a glyph drawn over a rounded
// pill, whose corner pixel is the ground OUTSIDE the pill.
COLORREF SampleBackground(HDC dc, const RECT& rect, bool centre = false) {
  POINT sample = {rect.left + 1, rect.top + 1};
  if (centre) {
    sample = {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
  }
  RECT clip = {};
  if (::GetClipBox(dc, &clip) > NULLREGION && clip.right > clip.left &&
      clip.bottom > clip.top) {
    sample.x = std::max(clip.left, std::min(sample.x, clip.right - 1));
    sample.y = std::max(clip.top, std::min(sample.y, clip.bottom - 1));
  }
  return ::GetPixel(dc, sample.x, sample.y);
}

// Defined with the shapes below; the colour text borrows its scratch blend.
template <typename Draw>
bool BlendD2DShape(HDC dc,
                   const RECT& dest,
                   int origin_x,
                   int origin_y,
                   COLORREF ink,
                   Draw draw);

}  // namespace

bool PhiDrawColorText(HDC dc,
                      const RECT& rect,
                      const std::wstring& text,
                      int pixel_size,
                      COLORREF ink) {
  if (text.empty() || rect.right <= rect.left || rect.bottom <= rect.top ||
      !EnsureState()) {
    return false;
  }
  IDWriteTextFormat* format = FormatFor(pixel_size);
  if (!format) {
    return false;
  }
  DWriteState& state = State();
  const float width = static_cast<float>(rect.right - rect.left);
  const float height = static_cast<float>(rect.bottom - rect.top);
  // Drawn ink-centred in the rect, not advance-centred (see above).
  const InkMetrics& metrics = MetricsFor(text, pixel_size);
  const D2D1_RECT_F bounds =
      D2D1::RectF(metrics.dx, metrics.dy, width + metrics.dx,
                  height + metrics.dy);
  // First choice: over a transparent clear on the shape target, blended onto
  // the DC like any shape — so a rounded hover or selection fill under the
  // glyph survives. Grayscale: ClearType needs an opaque ground to resolve
  // against, and a premultiplied target has none.
  if (BlendD2DShape(dc, rect, 0, 0, ink,
                    [&](ID2D1RenderTarget* target, ID2D1Brush* brush) {
                      target->SetTextAntialiasMode(
                          D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                      target->DrawTextW(
                          text.c_str(), static_cast<UINT32>(text.size()),
                          format, bounds, brush,
                          D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                    })) {
    return true;
  }
  // No scratch to be had (an outsized rect): bound straight to the DC. D2D
  // repaints the whole bound rect on EndDraw, so start from what is already
  // painted beneath — sampled at the centre, inside the caller's pill.
  const COLORREF background = SampleBackground(dc, rect, /*centre=*/true);
  if (FAILED(state.target->BindDC(dc, &rect))) {
    return false;
  }
  state.target->BeginDraw();
  state.target->Clear(
      ToColor(background == CLR_INVALID ? RGB(0xFF, 0xFF, 0xFF)
                                        : background));
  state.text_brush->SetColor(ToColor(ink));
  // ENABLE_COLOR_FONT is the whole point: COLR glyph layers render in
  // colour; non-colour glyphs take the ink brush.
  state.target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()),
                          format, bounds, state.text_brush.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
  if (FAILED(state.target->EndDraw())) {
    DropTextTarget();  // A lost target rebuilds on the next call.
    return false;
  }
  return true;
}

bool PhiDrawColorIcon(HDC dc,
                      const RECT& rect,
                      const std::wstring& text,
                      int ink_size,
                      COLORREF ink) {
  if (text.empty() || ink_size <= 0 || !EnsureState()) {
    return false;
  }
  // An em size says nothing about how much of it a glyph covers: a face
  // emoji nearly fills its line box where a heart or a cog sits well inside
  // it, so a row drawn at one em size reads as a row of different sizes.
  // Measure the ink at the asked-for size and scale the em by what it
  // missed — ink is linear in em, so one correction lands it.
  const InkMetrics& probe = MetricsFor(text, ink_size);
  const float span = std::max(probe.width, probe.height);
  int em = ink_size;
  if (span > 0.5f) {
    em = std::clamp(
        static_cast<int>(std::lround(ink_size * ink_size / span)), 1,
        ink_size * 4);
  }
  return PhiDrawColorText(dc, rect, text, em, ink);
}

bool PhiDrawIconGlyph(HDC dc,
                      const RECT& rect,
                      const std::wstring& glyph,
                      int pixel_size,
                      COLORREF ink) {
  if (glyph.empty() || pixel_size <= 0 || rect.right <= rect.left ||
      rect.bottom <= rect.top || !EnsureState()) {
    return false;
  }
  DWriteState& state = State();
  IDWriteFontFace* face = IconFace();
  if (!face) {
    return false;
  }
  // One code point, surrogate pair included — the MDL2 icons are BMP
  // private-use characters, but the pair costs two lines to honour.
  UINT32 codepoint = static_cast<UINT32>(glyph[0]);
  if (glyph.size() >= 2 && glyph[0] >= 0xD800 && glyph[0] <= 0xDBFF &&
      glyph[1] >= 0xDC00 && glyph[1] <= 0xDFFF) {
    codepoint = 0x10000 + ((codepoint - 0xD800) << 10) +
                (static_cast<UINT32>(glyph[1]) - 0xDC00);
  }
  UINT16 index = 0;
  if (FAILED(face->GetGlyphIndices(&codepoint, 1, &index)) || index == 0) {
    return false;  // .notdef: not this face's glyph, let GDI try.
  }
  DWRITE_FONT_METRICS font_metrics = {};
  face->GetMetrics(&font_metrics);
  DWRITE_GLYPH_METRICS glyph_metrics = {};
  if (!font_metrics.designUnitsPerEm ||
      FAILED(face->GetDesignGlyphMetrics(&index, 1, &glyph_metrics, FALSE))) {
    return false;
  }
  // Placed exactly where DT_CENTER | DT_VCENTER would put it: the advance
  // centred across the box, the ascent + descent line box centred down it.
  const float em = static_cast<float>(pixel_size);
  const float upem = static_cast<float>(font_metrics.designUnitsPerEm);
  const float ascent = em * font_metrics.ascent / upem;
  const float descent = em * font_metrics.descent / upem;
  const float advance = em * glyph_metrics.advanceWidth / upem;
  const float box_w = static_cast<float>(rect.right - rect.left);
  const float box_h = static_cast<float>(rect.bottom - rect.top);
  const float origin_x = (box_w - advance) / 2.0f;
  const float origin_y = (box_h - (ascent + descent)) / 2.0f + ascent;
  DWRITE_GLYPH_RUN run = {};
  run.fontFace = face;
  run.fontEmSize = em;
  run.glyphCount = 1;
  run.glyphIndices = &index;
  ComPtr<IDWriteGlyphRunAnalysis> analysis;
  // NATURAL_SYMMETRIC is what makes this worth doing: outlines are hinted
  // vertically only and antialiased in both axes, so a circle stays round
  // instead of being snapped onto the pixel grid stem by stem.
  if (FAILED(state.dwrite->CreateGlyphRunAnalysis(
          &run, 1.0f, nullptr, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
          DWRITE_MEASURING_MODE_NATURAL, origin_x, origin_y,
          analysis.GetAddressOf()))) {
    return false;
  }
  // The coverage texture is three bytes per pixel (one per subpixel);
  // averaging them is the grayscale coverage, free of ClearType's colour.
  RECT bounds = {};
  if (FAILED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1,
                                             &bounds)) ||
      bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
    return false;
  }
  // Clipped to the box, as DrawText clips to its rect.
  bounds.left = std::max<LONG>(bounds.left, 0);
  bounds.top = std::max<LONG>(bounds.top, 0);
  bounds.right = std::min<LONG>(bounds.right, static_cast<LONG>(box_w));
  bounds.bottom = std::min<LONG>(bounds.bottom, static_cast<LONG>(box_h));
  if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
    return false;
  }
  const int w = bounds.right - bounds.left;
  const int h = bounds.bottom - bounds.top;
  std::vector<unsigned char> coverage(static_cast<size_t>(w) * h * 3);
  if (FAILED(analysis->CreateAlphaTexture(
          DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds, coverage.data(),
          static_cast<UINT32>(coverage.size())))) {
    return false;
  }
  std::vector<unsigned char> bgra(static_cast<size_t>(w) * h * 4);
  const unsigned r = GetRValue(ink);
  const unsigned g = GetGValue(ink);
  const unsigned b = GetBValue(ink);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t from = (static_cast<size_t>(y) * w + x) * 3;
      const unsigned a =
          (static_cast<unsigned>(coverage[from]) + coverage[from + 1] +
           coverage[from + 2] + 1) /
          3;
      const size_t at = (static_cast<size_t>(y) * w + x) * 4;
      bgra[at] = static_cast<unsigned char>(b * a / 255);
      bgra[at + 1] = static_cast<unsigned char>(g * a / 255);
      bgra[at + 2] = static_cast<unsigned char>(r * a / 255);
      bgra[at + 3] = static_cast<unsigned char>(a);
    }
  }
  const RECT dest = {rect.left + bounds.left, rect.top + bounds.top,
                     rect.left + bounds.right, rect.top + bounds.bottom};
  return PhiBlendBgra(dc, dest, bgra.data(), w, h);
}

bool PhiDrawBgra(HDC dc,
                 const RECT& dest,
                 const unsigned char* bgra,
                 int width,
                 int height) {
  if (!bgra || width <= 0 || height <= 0 || dest.right <= dest.left ||
      dest.bottom <= dest.top || !EnsureState()) {
    return false;
  }
  DWriteState& state = State();
  const COLORREF background = SampleBackground(dc, dest);
  if (FAILED(state.target->BindDC(dc, &dest))) {
    return false;
  }
  ComPtr<ID2D1Bitmap> bitmap;
  const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (FAILED(state.target->CreateBitmap(
          D2D1::SizeU(static_cast<UINT32>(width),
                      static_cast<UINT32>(height)),
          bgra, static_cast<UINT32>(width) * 4, properties,
          bitmap.GetAddressOf()))) {
    return false;
  }
  state.target->BeginDraw();
  state.target->Clear(
      ToColor(background == CLR_INVALID ? RGB(0xFF, 0xFF, 0xFF)
                                        : background));
  const D2D1_RECT_F bounds =
      D2D1::RectF(0.0f, 0.0f, static_cast<FLOAT>(dest.right - dest.left),
                  static_cast<FLOAT>(dest.bottom - dest.top));
  // A device context can interpolate with high-quality cubic; the plain DC
  // target only offers linear — still far above StretchDIBits.
  ComPtr<ID2D1DeviceContext> context;
  if (SUCCEEDED(state.target.As(&context))) {
    context->DrawBitmap(bitmap.Get(), &bounds, 1.0f,
                        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
  } else {
    state.target->DrawBitmap(bitmap.Get(), bounds, 1.0f,
                             D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
  }
  if (FAILED(state.target->EndDraw())) {
    DropTextTarget();
    return false;
  }
  return true;
}

bool PhiBlendBgra(HDC dc,
                  const RECT& dest,
                  const unsigned char* bgra,
                  int width,
                  int height) {
  if (!bgra || width <= 0 || height <= 0 || dest.right <= dest.left ||
      dest.bottom <= dest.top) {
    return false;
  }
  const BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  if (ScratchSurface* scratch = EnsureScratch(width, height)) {
    // The shared surface is at least this big and usually bigger, so the
    // rows are copied at ITS stride, not the buffer's.
    const size_t row = static_cast<size_t>(width) * 4;
    const size_t stride = static_cast<size_t>(scratch->width) * 4;
    // SAFETY: the section holds scratch->height rows of `stride` bytes and
    // height <= scratch->height; the caller's buffer holds `height` rows of
    // `row` bytes by this function's contract.
    UNSAFE_BUFFERS({
      for (int y = 0; y < height; ++y) {
        ::memcpy(scratch->bits + static_cast<size_t>(y) * stride,
                 bgra + static_cast<size_t>(y) * row, row);
      }
    });
    return ::GdiAlphaBlend(dc, dest.left, dest.top, dest.right - dest.left,
                           dest.bottom - dest.top, scratch->dc, 0, 0, width,
                           height, blend) != FALSE;
  }
  // No shared surface to be had: one for this call, as before.
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // Top-down, matching the buffer.
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib =
      ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) {
    if (dib) {
      ::DeleteObject(dib);
    }
    return false;
  }
  // SAFETY: the DIB section was sized width*height*4 above, and callers
  // pass a buffer of exactly that size.
  UNSAFE_BUFFERS(
      ::memcpy(bits, bgra, static_cast<size_t>(width) * height * 4));
  HDC src_dc = ::CreateCompatibleDC(dc);
  if (!src_dc) {
    ::DeleteObject(dib);
    return false;
  }
  HGDIOBJ previous = ::SelectObject(src_dc, dib);
  if (!previous) {
    ::DeleteDC(src_dc);
    ::DeleteObject(dib);
    return false;
  }
  const BOOL ok = ::GdiAlphaBlend(dc, dest.left, dest.top,
                                  dest.right - dest.left,
                                  dest.bottom - dest.top, src_dc, 0, 0, width,
                                  height, blend);
  ::SelectObject(src_dc, previous);
  ::DeleteDC(src_dc);
  ::DeleteObject(dib);
  return ok != FALSE;
}

// ---------------------------------------------------------------------------
// Anti-aliased shapes.
//
// Direct2D rasterises them, into the scratch surface, premultiplied over a
// transparent clear; GdiAlphaBlend then composites that over the DC. The
// blend — rather than binding the target to the caller's DC as the text
// path does — is what lets a curve land over a gradient or a bitmap: a DC
// render target does not read the DC back, so drawing straight onto it
// would first have to clear the shape's whole bounding rect to a sampled
// flat colour.
//
// Behind that sits the software rasteriser this layer shipped before, kept
// as the fallback (and, for the knock-out, as the only path): every shape
// is a signed-distance field sampled at pixel centres, negative inside,
// positive outside, in pixels. A fill's coverage is 0.5 - d clamped to
// 0..1 (a pixel whose centre sits on the boundary is half covered); a
// stroke of `width` is the band of distances [-width, 0] with the same
// half-pixel feather at both edges. The coverage becomes the alpha of a
// premultiplied buffer blended the same way, so a machine with no Direct2D
// at all still draws every curve antialiased.
//
// The two agree on GEOMETRY exactly — same rect, same true radius, strokes
// inside the boundary — and differ only in how a partly covered pixel is
// weighed: Direct2D integrates the area, the field takes 0.5 - d at the
// centre. On an arc that is worth a few levels of alpha, nowhere a whole
// pixel (test/model/dwrite_geometry_test.cc pins the bound).

namespace {

constexpr double kPi = 3.14159265358979323846;

double Clamp01(double v) {
  return std::min(1.0, std::max(0.0, v));
}

// Signed distance to a rounded rectangle's boundary: the distance to the
// inner core rectangle's nearest point, minus the corner radius. A corner
// left out of `corners` (bit 0..3 = TL, TR, BL, BR) stays square.
double RoundRectDistance(double px,
                         double py,
                         double w,
                         double h,
                         double radius,
                         unsigned corners = 0xF) {
  radius = std::max(0.0, std::min(radius, std::min(w, h) / 2.0));
  const bool left = px < w / 2.0;
  const bool top = py < h / 2.0;
  const unsigned bit = (top ? 0u : 2u) + (left ? 0u : 1u);
  const double r = (corners & (1u << bit)) ? radius : 0.0;
  const double cx = std::min(std::max(px, r), w - r);
  const double cy = std::min(std::max(py, r), h - r);
  const double dx = px - cx;
  const double dy = py - cy;
  if (dx == 0.0 || dy == 0.0) {
    // On an edge's run: the distance to the nearest of the four sides,
    // negative inside — the core-rect distance with r taken back off would
    // only be right for a pixel out past a corner.
    const double inside = std::min(std::min(px, w - px), std::min(py, h - py));
    if (px >= 0.0 && px <= w && py >= 0.0 && py <= h) {
      return -inside;
    }
    return std::sqrt(dx * dx + dy * dy) - r;
  }
  return std::sqrt(dx * dx + dy * dy) - r;
}

// Signed distance to the ellipse inscribed in w x h. Exact for a circle;
// for an ellipse the first-order estimate (f - 1) / |grad f| on the
// normalized radius f, which is accurate within a pixel of the boundary —
// the only place coverage is fractional.
double EllipseDistance(double px, double py, double w, double h) {
  const double a = w / 2.0;
  const double b = h / 2.0;
  if (a <= 0.0 || b <= 0.0) {
    return 1.0;
  }
  const double x = px - a;
  const double y = py - b;
  const double f = std::sqrt((x * x) / (a * a) + (y * y) / (b * b));
  if (f < 1e-6) {
    return -std::min(a, b);
  }
  const double grad =
      std::sqrt((x * x) / (a * a * a * a) + (y * y) / (b * b * b * b)) / f;
  return grad > 0.0 ? (f - 1.0) / grad : -std::min(a, b);
}

// Where along the rounded rectangle's perimeter the boundary point nearest
// (px, py) lies, in pixels from the top-left arc's end, clockwise — the
// parameter a dash pattern runs on. `*perimeter` receives the total.
double RoundRectPerimeterAt(double px,
                            double py,
                            double w,
                            double h,
                            double radius,
                            double* perimeter) {
  const double r = std::max(0.0, std::min(radius, std::min(w, h) / 2.0));
  const double run_x = w - 2.0 * r;
  const double run_y = h - 2.0 * r;
  const double arc = kPi * r / 2.0;
  *perimeter = 2.0 * run_x + 2.0 * run_y + 4.0 * arc;
  const double cx = std::min(std::max(px, r), w - r);
  const double cy = std::min(std::max(py, r), h - r);
  const double dx = px - cx;
  const double dy = py - cy;
  if (dx != 0.0 && dy != 0.0) {
    const double theta = std::atan2(dy, dx);  // -pi..pi, y down.
    if (dx > 0.0 && dy < 0.0) {               // Top-right arc.
      return run_x + (theta + kPi / 2.0) * r;
    }
    if (dx > 0.0) {                           // Bottom-right arc.
      return run_x + arc + run_y + theta * r;
    }
    if (dy > 0.0) {                           // Bottom-left arc.
      return 2.0 * run_x + 2.0 * arc + run_y + (theta - kPi / 2.0) * r;
    }
    return 2.0 * run_x + 3.0 * arc + 2.0 * run_y + (theta + kPi) * r;
  }
  // On a straight run: nearest side decides.
  const double to_top = py;
  const double to_bottom = h - py;
  const double to_left = px;
  const double to_right = w - px;
  const double nearest =
      std::min(std::min(to_top, to_bottom), std::min(to_left, to_right));
  if (nearest == to_top) {
    return cx - r;
  }
  if (nearest == to_right) {
    return run_x + arc + (cy - r);
  }
  if (nearest == to_bottom) {
    return run_x + 2.0 * arc + run_y + (w - r - cx);
  }
  return 2.0 * run_x + 3.0 * arc + run_y + (h - r - cy);
}

// The same for the inscribed ellipse: the angle scaled by the mean radius.
double EllipsePerimeterAt(double px,
                          double py,
                          double w,
                          double h,
                          double* perimeter) {
  const double a = w / 2.0;
  const double b = h / 2.0;
  const double mean = (a + b) / 2.0;
  *perimeter = 2.0 * kPi * mean;
  const double theta = std::atan2((py - b) / std::max(b, 1e-6),
                                  (px - a) / std::max(a, 1e-6));
  return (theta + kPi) * mean;
}

// Coverage of a dash pattern at perimeter position `t`: `dash` on, `gap`
// off, the period stretched so a whole number of them fit `perimeter`;
// dash ends feathered half a pixel like every other edge.
double DashCoverage(double t, double perimeter, int dash, int gap) {
  const double period = static_cast<double>(dash + gap);
  if (period <= 0.0 || perimeter <= period) {
    return 1.0;
  }
  const double count = std::max(1.0, std::round(perimeter / period));
  const double fitted = perimeter / count;
  const double on = dash * fitted / period;
  double s = std::fmod(t, fitted);
  if (s < 0.0) {
    s += fitted;
  }
  return Clamp01(std::min(s, on - s) + 0.5);
}

double FillCoverage(double d) {
  return Clamp01(0.5 - d);
}

double BandCoverage(double d, double width) {
  const double half = width / 2.0;
  return Clamp01(half + 0.5 - std::abs(d + half));
}

// Software-renders `coverage(x, y)` (0..1, in the rect's own frame) of
// `ink` into a premultiplied buffer and blends it over the DC.
template <typename Coverage>
bool BlendCoverage(HDC dc, const RECT& rect, COLORREF ink, Coverage coverage) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  if (w <= 0 || h <= 0) {
    return true;  // Nothing to draw is not a failure.
  }
  std::vector<unsigned char> bgra(static_cast<size_t>(w) * h * 4);
  const unsigned r = GetRValue(ink);
  const unsigned g = GetGValue(ink);
  const unsigned b = GetBValue(ink);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const double c = Clamp01(coverage(x + 0.5, y + 0.5));
      const unsigned a = static_cast<unsigned>(c * 255.0 + 0.5);
      const size_t at = (static_cast<size_t>(y) * w + x) * 4;
      bgra[at] = static_cast<unsigned char>(b * a / 255);
      bgra[at + 1] = static_cast<unsigned char>(g * a / 255);
      bgra[at + 2] = static_cast<unsigned char>(r * a / 255);
      bgra[at + 3] = static_cast<unsigned char>(a);
    }
  }
  return PhiBlendBgra(dc, rect, bgra.data(), w, h);
}

void GdiFill(HDC dc, const RECT& rect, COLORREF fill) {
  if (rect.right <= rect.left || rect.bottom <= rect.top) {
    return;
  }
  if (HBRUSH brush = BrushFor(fill)) {
    ::FillRect(dc, &rect, brush);
  }
}

// The GDI rendering of the same shape, for when a blend surface cannot be
// had (a DIB allocation failing under memory pressure). Stair-stepped, but
// the shape is there.
void GdiRoundRect(HDC dc,
                  const RECT& rect,
                  int radius,
                  const COLORREF* fill,
                  const COLORREF* ink,
                  int width) {
  HBRUSH brush = fill ? ::CreateSolidBrush(*fill) : nullptr;
  HPEN pen = ink ? ::CreatePen(PS_SOLID, width, *ink) : nullptr;
  HGDIOBJ old_brush = ::SelectObject(
      dc, brush ? static_cast<HGDIOBJ>(brush) : ::GetStockObject(NULL_BRUSH));
  HGDIOBJ old_pen = ::SelectObject(
      dc, pen ? static_cast<HGDIOBJ>(pen) : ::GetStockObject(NULL_PEN));
  // A NULL_PEN fill stops a pixel short; a stroked one does not.
  const int grow = pen ? 0 : 1;
  ::RoundRect(dc, rect.left, rect.top, rect.right + grow, rect.bottom + grow,
              2 * radius, 2 * radius);
  ::SelectObject(dc, old_pen);
  ::SelectObject(dc, old_brush);
  if (pen) {
    ::DeleteObject(pen);
  }
  if (brush) {
    ::DeleteObject(brush);
  }
}

void GdiEllipse(HDC dc,
                const RECT& rect,
                const COLORREF* fill,
                const COLORREF* ink,
                int width) {
  HBRUSH brush = fill ? ::CreateSolidBrush(*fill) : nullptr;
  HPEN pen = ink ? ::CreatePen(PS_SOLID, width, *ink) : nullptr;
  HGDIOBJ old_brush = ::SelectObject(
      dc, brush ? static_cast<HGDIOBJ>(brush) : ::GetStockObject(NULL_BRUSH));
  HGDIOBJ old_pen = ::SelectObject(
      dc, pen ? static_cast<HGDIOBJ>(pen) : ::GetStockObject(NULL_PEN));
  const int grow = pen ? 0 : 1;
  ::Ellipse(dc, rect.left, rect.top, rect.right + grow, rect.bottom + grow);
  ::SelectObject(dc, old_pen);
  ::SelectObject(dc, old_brush);
  if (pen) {
    ::DeleteObject(pen);
  }
  if (brush) {
    ::DeleteObject(brush);
  }
}

// Walks the four `tile`-square corners of `rect` — the straight runs
// between them are the caller's to fill, opaque and cheap — and stops at
// the first `visit` that fails. `visit` takes the tile's rect on the DC and
// the tile's offset inside `rect`.
template <typename Visit>
bool ForEachCornerTile(const RECT& rect, int tile, Visit visit) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  tile = std::min(tile, std::min(w, h));
  if (tile <= 0) {
    return true;
  }
  const int xs[2] = {0, w - tile};
  const int ys[2] = {0, h - tile};
  for (int y : ys) {
    for (int x : xs) {
      const RECT part = {rect.left + x, rect.top + y, rect.left + x + tile,
                         rect.top + y + tile};
      if (!visit(part, x, y)) {
        return false;
      }
      if (w <= tile) {
        break;  // One column: both tiles are the same pixels.
      }
    }
    if (h <= tile) {
      break;
    }
  }
  return true;
}

// Software-renders `coverage` (in `rect`'s frame) over those corner tiles.
template <typename Coverage>
bool BlendCornerTiles(HDC dc,
                      const RECT& rect,
                      int tile,
                      COLORREF ink,
                      Coverage coverage) {
  return ForEachCornerTile(rect, tile, [&](const RECT& part, int x, int y) {
    return BlendCoverage(dc, part, ink, [&](double px, double py) {
      return coverage(px + x, py + y);
    });
  });
}

// Direct2D hands its surface back through GDI on EndDraw. A path that
// dropped the alpha channel on the way would turn every antialiased shape
// into an opaque rectangle — the loudest regression this file could ship —
// so one 4x4 probe on first use settles it: an opaque left half, a cleared
// right half, read back out of the section. A failure pins the layer to the
// software field for the life of the process.
bool ShapeTargetDrawsAlpha(ID2D1DCRenderTarget* target,
                           const ScratchSurface* scratch) {
  const RECT bind = {0, 0, 4, 4};
  if (FAILED(target->BindDC(scratch->dc, &bind))) {
    return false;
  }
  DWriteState& state = State();
  state.shape_brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
  target->BeginDraw();
  target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
  target->FillRectangle(D2D1::RectF(0.0f, 0.0f, 2.0f, 4.0f),
                        state.shape_brush.Get());
  if (FAILED(target->EndDraw())) {
    return false;
  }
  ::GdiFlush();
  // SAFETY: the section holds scratch->height (>= 4) rows of scratch->width
  // (>= 4) pixels, four bytes each; this reads two pixels of the first row.
  const auto row = UNSAFE_BUFFERS(base::span<const unsigned char>(
      static_cast<const unsigned char*>(scratch->bits), size_t{16}));
  return row[3] == 255 && row[15] == 0;
}

// The shape render target. SOFTWARE on purpose: these shapes are tens of
// pixels across and land in a DIB, so a GPU target would pay a submit and a
// readback on every one of the dozens a frame draws — and the software
// rasteriser puts the same pixels on every machine, which the box's pixel
// suites depend on. Null when Direct2D cannot supply one.
ID2D1DCRenderTarget* ShapeTarget() {
  DWriteState& state = State();
  if (state.shape_failed) {
    return nullptr;
  }
  if (!state.shape_target) {
    if (!EnsureFactories()) {
      return nullptr;
    }
    // 96 DPI: one DIP == one pixel, as for the text target.
    const D2D1_RENDER_TARGET_PROPERTIES properties =
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
    if (FAILED(state.d2d->CreateDCRenderTarget(
            &properties, state.shape_target.GetAddressOf())) ||
        FAILED(state.shape_target->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::Black),
            state.shape_brush.GetAddressOf()))) {
      // One that cannot be made now will not be made on the next frame
      // either; the software field takes over for good.
      state.shape_target.Reset();
      state.shape_brush.Reset();
      state.shape_failed = true;
      return nullptr;
    }
  }
  // Probed on the first draw the scratch surface can serve. Handing the
  // target out unprobed before then costs nothing: a draw with no scratch
  // cannot happen either.
  if (!state.shape_validated) {
    if (const ScratchSurface* scratch = EnsureScratch(4, 4)) {
      if (!ShapeTargetDrawsAlpha(state.shape_target.Get(), scratch)) {
        state.shape_target.Reset();
        state.shape_brush.Reset();
        state.shape_failed = true;
        return nullptr;
      }
      state.shape_validated = true;
    }
  }
  return state.shape_target.Get();
}

// Drops the shape target after a lost device so the next call rebuilds it.
// The brush belongs to that target; the dash styles come from the factory
// and outlive it.
void DropShapeTarget() {
  DWriteState& state = State();
  state.shape_target.Reset();
  state.shape_brush.Reset();
}

// A dash pattern's stroke style, cached by its two lengths (in stroke
// widths, which is the unit Direct2D dashes in). The same ring is stroked
// on every frame — a dormant tab's favicon, the new-Space placeholder — and
// CreateStrokeStyle is a COM allocation.
ID2D1StrokeStyle* DashStyle(float on, float off) {
  DWriteState& state = State();
  const auto key = std::make_pair(on, off);
  auto found = state.dash_styles.find(key);
  if (found != state.dash_styles.end()) {
    return found->second.Get();
  }
  if (state.dash_styles.size() >= kMaxDashStyles) {
    state.dash_styles.clear();  // A caller sweeping a size, not a pattern.
  }
  const FLOAT dashes[2] = {on, off};
  const D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties(
      D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
      D2D1_LINE_JOIN_MITER, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f);
  ComPtr<ID2D1StrokeStyle> style;
  if (FAILED(state.d2d->CreateStrokeStyle(properties, dashes, 2,
                                          style.GetAddressOf()))) {
    return nullptr;
  }
  return state.dash_styles.emplace(key, std::move(style)).first->second.Get();
}

// The dash lengths that make a `dash` on / `gap` off pattern close on
// itself around a closed path of `perimeter` pixels — the same stretch the
// software rasteriser applies. False when the period is no shorter than the
// path, which draws solid.
bool FitDash(double perimeter, int dash, int gap, float* on, float* off) {
  const double period = static_cast<double>(dash) + gap;
  if (period <= 0.0 || perimeter <= period) {
    return false;
  }
  const double count = std::max(1.0, std::round(perimeter / period));
  const double fitted = perimeter / count;
  const double ink = dash * fitted / period;
  *on = static_cast<float>(ink);
  *off = static_cast<float>(fitted - ink);
  return true;
}

// Renders `draw` — geometry in the SHAPE's own frame, whose point
// (`origin_x`, `origin_y`) lands on `dest`'s top-left — into the scratch
// surface in `ink`, then alpha-blends it over `dc`. Nothing reaches the DC
// unless the whole render succeeded, so a false leaves the caller free to
// draw the same shape another way.
template <typename Draw>
bool BlendD2DShape(HDC dc,
                   const RECT& dest,
                   int origin_x,
                   int origin_y,
                   COLORREF ink,
                   Draw draw) {
  const int w = dest.right - dest.left;
  const int h = dest.bottom - dest.top;
  if (w <= 0 || h <= 0) {
    return true;  // Nothing to draw is not a failure.
  }
  ID2D1DCRenderTarget* target = ShapeTarget();
  ScratchSurface* scratch = target ? EnsureScratch(w, h) : nullptr;
  if (!scratch) {
    return false;
  }
  const RECT bind = {0, 0, w, h};
  if (FAILED(target->BindDC(scratch->dc, &bind))) {
    DropShapeTarget();
    return false;
  }
  DWriteState& state = State();
  state.shape_brush->SetColor(ToColor(ink));
  target->BeginDraw();
  target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
  target->SetTransform(D2D1::Matrix3x2F::Translation(
      static_cast<FLOAT>(-origin_x), static_cast<FLOAT>(-origin_y)));
  draw(target, state.shape_brush.Get());
  target->SetTransform(D2D1::Matrix3x2F::Identity());
  if (FAILED(target->EndDraw())) {
    DropShapeTarget();
    return false;
  }
  // EndDraw hands the surface back through GDI: make sure it has landed in
  // the section before the blend reads it.
  ::GdiFlush();
  const BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  return ::GdiAlphaBlend(dc, dest.left, dest.top, w, h, scratch->dc, 0, 0, w,
                         h, blend) != FALSE;
}

// One shape through whichever rasteriser will take it.
template <typename Draw, typename Coverage>
bool BlendShape(HDC dc,
                const RECT& dest,
                int origin_x,
                int origin_y,
                COLORREF ink,
                Draw draw,
                Coverage coverage) {
  if (BlendD2DShape(dc, dest, origin_x, origin_y, ink, draw)) {
    return true;
  }
  return BlendCoverage(dc, dest, ink, [&](double px, double py) {
    return coverage(px + origin_x, py + origin_y);
  });
}

// The corner tiles of `rect`, each through whichever rasteriser will take
// it. `draw` speaks in `rect`'s frame; the tile offset is applied for it.
template <typename Draw, typename Coverage>
bool BlendShapeTiles(HDC dc,
                     const RECT& rect,
                     int tile,
                     COLORREF ink,
                     Draw draw,
                     Coverage coverage) {
  return ForEachCornerTile(rect, tile, [&](const RECT& part, int x, int y) {
    return BlendShape(dc, part, x, y, ink, draw, coverage);
  });
}

}  // namespace

bool PhiFillRoundRectAA(HDC dc, const RECT& rect, int radius, COLORREF fill) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  if (w <= 0 || h <= 0) {
    return false;
  }
  const int r = std::max(0, std::min(radius, std::min(w, h) / 2));
  if (r == 0) {
    GdiFill(dc, rect, fill);
    return true;
  }
  // The cross between the corner tiles is straight-edged on pixel bounds:
  // opaque, through GDI.
  GdiFill(dc, {rect.left + r, rect.top, rect.right - r, rect.bottom}, fill);
  GdiFill(dc, {rect.left, rect.top + r, rect.left + r, rect.bottom - r}, fill);
  GdiFill(dc, {rect.right - r, rect.top + r, rect.right, rect.bottom - r},
          fill);
  const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(
      D2D1::RectF(0.0f, 0.0f, static_cast<FLOAT>(w), static_cast<FLOAT>(h)),
      static_cast<FLOAT>(r), static_cast<FLOAT>(r));
  if (!BlendShapeTiles(
          dc, rect, r, fill,
          [&](ID2D1RenderTarget* target, ID2D1Brush* brush) {
            target->FillRoundedRectangle(rounded, brush);
          },
          [&](double px, double py) {
            return FillCoverage(RoundRectDistance(px, py, w, h, r));
          })) {
    GdiRoundRect(dc, rect, r, &fill, nullptr, 0);
  }
  return true;
}

bool PhiStrokeRoundRectAA(HDC dc,
                          const RECT& rect,
                          int radius,
                          COLORREF ink,
                          int width) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  if (w <= 0 || h <= 0 || width <= 0) {
    return false;
  }
  if (2 * width >= std::min(w, h)) {
    return PhiFillRoundRectAA(dc, rect, radius, ink);  // All band.
  }
  const int r = std::max(0, std::min(radius, std::min(w, h) / 2));
  // The tiles must hold the whole band where it turns: the larger of the
  // radius and the stroke width.
  const int tile = std::min(std::max(r, width), std::min(w, h) / 2);
  GdiFill(dc, {rect.left + tile, rect.top, rect.right - tile, rect.top + width},
          ink);
  GdiFill(dc,
          {rect.left + tile, rect.bottom - width, rect.right - tile,
           rect.bottom},
          ink);
  GdiFill(dc,
          {rect.left, rect.top + tile, rect.left + width, rect.bottom - tile},
          ink);
  GdiFill(dc,
          {rect.right - width, rect.top + tile, rect.right, rect.bottom - tile},
          ink);
  // Direct2D centres a stroke on the path, so the path is the boundary
  // pulled in by half the width — whose corner arc is that much tighter.
  // That band is exactly the field's [-width, 0].
  const float half = width / 2.0f;
  const float inner = std::max(0.0f, r - half);
  const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(
      D2D1::RectF(half, half, static_cast<FLOAT>(w) - half,
                  static_cast<FLOAT>(h) - half),
      inner, inner);
  if (!BlendShapeTiles(
          dc, rect, tile, ink,
          [&](ID2D1RenderTarget* target, ID2D1Brush* brush) {
            target->DrawRoundedRectangle(rounded, brush,
                                         static_cast<FLOAT>(width), nullptr);
          },
          [&](double px, double py) {
            return BandCoverage(RoundRectDistance(px, py, w, h, r), width);
          })) {
    GdiRoundRect(dc, rect, r, nullptr, &ink, width);
  }
  return true;
}

bool PhiFillCapsuleAA(HDC dc, const RECT& rect, COLORREF fill) {
  return PhiFillRoundRectAA(dc, rect, (rect.bottom - rect.top) / 2, fill);
}

bool PhiStrokeCapsuleAA(HDC dc, const RECT& rect, COLORREF ink, int width) {
  return PhiStrokeRoundRectAA(dc, rect, (rect.bottom - rect.top) / 2, ink,
                              width);
}

bool PhiFillEllipseAA(HDC dc, const RECT& rect, COLORREF fill) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  if (w <= 0 || h <= 0) {
    return false;
  }
  const D2D1_ELLIPSE ellipse = D2D1::Ellipse(
      D2D1::Point2F(w / 2.0f, h / 2.0f), w / 2.0f, h / 2.0f);
  if (!BlendShape(
          dc, rect, 0, 0, fill,
          [&](ID2D1RenderTarget* target, ID2D1Brush* brush) {
            target->FillEllipse(ellipse, brush);
          },
          [&](double px, double py) {
            return FillCoverage(EllipseDistance(px, py, w, h));
          })) {
    GdiEllipse(dc, rect, &fill, nullptr, 0);
  }
  return true;
}

bool PhiStrokeEllipseAA(HDC dc, const RECT& rect, COLORREF ink, int width) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  if (w <= 0 || h <= 0 || width <= 0) {
    return false;
  }
  const auto field = [&](double px, double py) {
    return BandCoverage(EllipseDistance(px, py, w, h), width);
  };
  // As for the rounded rect: the stroked path is the boundary pulled in by
  // half the width. A band wider than the ellipse has no such path, and
  // only the field can draw it.
  const float half = width / 2.0f;
  const float rx = w / 2.0f - half;
  const float ry = h / 2.0f - half;
  bool drawn = false;
  if (rx > 0.0f && ry > 0.0f) {
    const D2D1_ELLIPSE ellipse =
        D2D1::Ellipse(D2D1::Point2F(w / 2.0f, h / 2.0f), rx, ry);
    drawn = BlendD2DShape(dc, rect, 0, 0, ink,
                          [&](ID2D1RenderTarget* target, ID2D1Brush* brush) {
                            target->DrawEllipse(ellipse, brush,
                                                static_cast<FLOAT>(width),
                                                nullptr);
                          });
  }
  if (!drawn && !BlendCoverage(dc, rect, ink, field)) {
    GdiEllipse(dc, rect, nullptr, &ink, width);
  }
  return true;
}

bool PhiStrokeRoundRectDashedAA(HDC dc,
                                const RECT& rect,
                                int radius,
                                COLORREF ink,
                                int width,
                                int dash,
                                int gap) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  if (w <= 0 || h <= 0 || width <= 0) {
    return false;
  }
  const int r = std::max(0, std::min(radius, std::min(w, h) / 2));
  // Direct2D runs the pattern along the path it strokes — the boundary
  // pulled in by half the width — so the fit is to THAT perimeter.
  const double half = width / 2.0;
  const double inner = std::max(0.0, r - half);
  const double run_x = std::max(0.0, w - width - 2.0 * inner);
  const double run_y = std::max(0.0, h - width - 2.0 * inner);
  const double perimeter = 2.0 * (run_x + run_y) + 2.0 * kPi * inner;
  float on = 0.0f;
  float off = 0.0f;
  const bool dashed = FitDash(perimeter, dash, gap, &on, &off);
  ID2D1StrokeStyle* style =
      dashed ? DashStyle(on / width, off / width) : nullptr;
  const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(
      D2D1::RectF(static_cast<FLOAT>(half), static_cast<FLOAT>(half),
                  static_cast<FLOAT>(w - half), static_cast<FLOAT>(h - half)),
      static_cast<FLOAT>(inner), static_cast<FLOAT>(inner));
  if ((!dashed || style) &&
      BlendD2DShape(dc, rect, 0, 0, ink,
                    [&](ID2D1RenderTarget* target, ID2D1Brush* brush) {
                      target->DrawRoundedRectangle(
                          rounded, brush, static_cast<FLOAT>(width), style);
                    })) {
    return true;
  }
  if (!BlendCoverage(dc, rect, ink, [&](double px, double py) {
        const double band =
            BandCoverage(RoundRectDistance(px, py, w, h, r), width);
        if (band <= 0.0) {
          return 0.0;
        }
        double dash_perimeter = 0.0;
        const double t =
            RoundRectPerimeterAt(px, py, w, h, r, &dash_perimeter);
        return band * DashCoverage(t, dash_perimeter, dash, gap);
      })) {
    GdiRoundRect(dc, rect, r, nullptr, &ink, width);
  }
  return true;
}

bool PhiStrokeEllipseDashedAA(HDC dc,
                              const RECT& rect,
                              COLORREF ink,
                              int width,
                              int dash,
                              int gap) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  if (w <= 0 || h <= 0 || width <= 0) {
    return false;
  }
  const double half = width / 2.0;
  const double rx = w / 2.0 - half;
  const double ry = h / 2.0 - half;
  bool drawn = false;
  if (rx > 0.0 && ry > 0.0) {
    // The mean-radius circumference the field's dash parameter uses, taken
    // on the stroked path.
    const double perimeter = kPi * (rx + ry);
    float on = 0.0f;
    float off = 0.0f;
    const bool dashed = FitDash(perimeter, dash, gap, &on, &off);
    ID2D1StrokeStyle* style =
        dashed ? DashStyle(on / width, off / width) : nullptr;
    const D2D1_ELLIPSE ellipse =
        D2D1::Ellipse(D2D1::Point2F(w / 2.0f, h / 2.0f),
                      static_cast<FLOAT>(rx), static_cast<FLOAT>(ry));
    drawn = (!dashed || style) &&
            BlendD2DShape(dc, rect, 0, 0, ink,
                          [&](ID2D1RenderTarget* target, ID2D1Brush* brush) {
                            target->DrawEllipse(ellipse, brush,
                                                static_cast<FLOAT>(width),
                                                style);
                          });
  }
  if (drawn) {
    return true;
  }
  if (!BlendCoverage(dc, rect, ink, [&](double px, double py) {
        const double band = BandCoverage(EllipseDistance(px, py, w, h), width);
        if (band <= 0.0) {
          return 0.0;
        }
        double dash_perimeter = 0.0;
        const double t = EllipsePerimeterAt(px, py, w, h, &dash_perimeter);
        return band * DashCoverage(t, dash_perimeter, dash, gap);
      })) {
    GdiEllipse(dc, rect, nullptr, &ink, width);
  }
  return true;
}

bool PhiKnockOutRoundRectAA(HDC dc,
                            const RECT& rect,
                            int radius,
                            COLORREF outside) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  if (w <= 0 || h <= 0) {
    return false;
  }
  const int r = std::max(0, std::min(radius, std::min(w, h) / 2));
  if (r == 0) {
    return true;
  }
  // The one shape that stays on the field. Its coverage is the INVERSE of a
  // rounded rectangle's, which Direct2D can only draw as the difference of
  // two geometries — a path object built per call, on a path that paints
  // gradients. The field inverts for free, and this way the corner bites
  // that back the Space theme slider and the settings artwork are pixel-for
  // -pixel what they were.
  return BlendCornerTiles(dc, rect, r, outside, [&](double px, double py) {
    return 1.0 - FillCoverage(RoundRectDistance(px, py, w, h, r));
  });
}

double PhiRoundRectCoverage(double px,
                            double py,
                            double width,
                            double height,
                            double radius,
                            unsigned corners) {
  return FillCoverage(
      RoundRectDistance(px, py, width, height, radius, corners));
}

// ---------------------------------------------------------------------------
// Icon glyphs and wrapped text through DrawText's signature.

int PhiDrawTextAA(HDC dc,
                  const wchar_t* text,
                  int length,
                  RECT* rect,
                  UINT format) {
  // SAFETY: DrawTextW's contract — `text` holds `length` characters, or is
  // NUL-terminated when `length` is -1.
  const std::wstring_view view =
      !text ? std::wstring_view()
            : UNSAFE_BUFFERS(
                  length < 0
                      ? std::wstring_view(text)
                      : std::wstring_view(text, static_cast<size_t>(length)));
  if (text && rect && (format & DT_WORDBREAK) && (format & DT_NOPREFIX) &&
      !(format & DT_SINGLELINE)) {
    // A cut text keeps its overflow past the last line that shows, or there
    // is nothing for DT_END_ELLIPSIS to cut.
    int max_lines = 0;
    TEXTMETRICW metrics = {};
    if ((format & DT_END_ELLIPSIS) && !(format & DT_CALCRECT) &&
        ::GetTextMetricsW(dc, &metrics) && metrics.tmHeight > 0) {
      max_lines = std::max<int>(
          1, (rect->bottom - rect->top) / metrics.tmHeight);
    }
    const std::wstring wrapped =
        PhiWrapTextLines(dc, view, rect->right - rect->left, max_lines);
    if (!wrapped.empty()) {
      return ::DrawTextW(dc, wrapped.c_str(), static_cast<int>(wrapped.size()),
                         rect, format);
    }
  }
  if (text && rect && !(format & DT_CALCRECT)) {
    // One code point, surrogate pair allowed; the MDL2 icons are BMP
    // private-use characters (U+E000..U+F8FF).
    const bool one = view.size() == 1 ||
                     (view.size() == 2 && view[0] >= 0xD800 &&
                      view[0] <= 0xDBFF);
    if (one && view[0] >= 0xE000 && view[0] <= 0xF8FF) {
      LOGFONTW lf = {};
      HGDIOBJ font = ::GetCurrentObject(dc, OBJ_FONT);
      if (font && ::GetObjectW(font, sizeof(lf), &lf) &&
          PhiIsIconFace(lf.lfFaceName) &&
          lf.lfHeight != 0) {
        // A negative height is the em size; a positive one the cell
        // height, which for this face is the em plus a little leading.
        const int em = std::abs(static_cast<int>(lf.lfHeight));
        RECT box = *rect;
        if (format & DT_RIGHT) {
          box.left = std::max(box.left, box.right - em);
        } else if (!(format & DT_CENTER)) {
          box.right = std::min(box.right, box.left + em);
        }
        if (format & DT_BOTTOM) {
          box.top = std::max(box.top, box.bottom - em);
        } else if (!(format & DT_VCENTER)) {
          box.bottom = std::min(box.bottom, box.top + em);
        }
        if (PhiDrawIconGlyph(dc, box, std::wstring(view), em,
                             ::GetTextColor(dc))) {
          return em;
        }
      }
    }
  }
  return ::DrawTextW(dc, text, length, rect, format);
}

// ---------------------------------------------------------------------------
// Shaped layered windows.

bool PhiCommitShapedLayer(HWND hwnd,
                          int width,
                          int height,
                          base::FunctionRef<void(HDC)> paint,
                          base::FunctionRef<double(double, double)> coverage,
                          BYTE alpha) {
  if (!hwnd || !::IsWindow(hwnd) || width <= 0 || height <= 0) {
    return false;
  }
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // Top-down.
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib =
      ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) {
    if (dib) {
      ::DeleteObject(dib);
    }
    return false;
  }
  HDC screen = ::GetDC(nullptr);
  HDC mem = ::CreateCompatibleDC(screen);
  HGDIOBJ previous = mem ? ::SelectObject(mem, dib) : nullptr;
  if (!previous) {
    if (mem) {
      ::DeleteDC(mem);
    }
    if (screen) {
      ::ReleaseDC(nullptr, screen);
    }
    ::DeleteObject(dib);
    return false;
  }
  paint(mem);
  ::GdiFlush();
  const size_t bytes = static_cast<size_t>(width) * height * 4;
  // SAFETY: the DIB section was sized width * height * 4 above.
  const auto pixels = UNSAFE_BUFFERS(
      base::span<unsigned char>(static_cast<unsigned char*>(bits), bytes));
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t at = (static_cast<size_t>(y) * width + x) * 4;
      const double c = Clamp01(coverage(x + 0.5, y + 0.5));
      const unsigned a = static_cast<unsigned>(c * 255.0 + 0.5);
      // The interior and the outside are almost all of a card, and both
      // premultiply to what the byte already holds (v * 255 / 255 == v,
      // v * 0 / 255 == 0) — so only the coverage ramp does the arithmetic.
      if (a == 255) {
        pixels[at + 3] = 255;
        continue;
      }
      if (a == 0) {
        pixels[at] = 0;
        pixels[at + 1] = 0;
        pixels[at + 2] = 0;
        pixels[at + 3] = 0;
        continue;
      }
      pixels[at] = static_cast<unsigned char>(pixels[at] * a / 255);
      pixels[at + 1] = static_cast<unsigned char>(pixels[at + 1] * a / 255);
      pixels[at + 2] = static_cast<unsigned char>(pixels[at + 2] * a / 255);
      pixels[at + 3] = static_cast<unsigned char>(a);
    }
  }
  SIZE size = {width, height};
  POINT origin = {0, 0};
  BLENDFUNCTION blend = {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
  const BOOL ok = ::UpdateLayeredWindow(hwnd, nullptr, nullptr, &size, mem,
                                        &origin, 0, &blend, ULW_ALPHA);
  ::SelectObject(mem, previous);
  ::DeleteDC(mem);
  if (screen) {
    ::ReleaseDC(nullptr, screen);
  }
  ::DeleteObject(dib);
  return ok != FALSE;
}

bool PhiCommitCornerCap(HWND cap,
                        int corner,
                        int radius,
                        COLORREF tint,
                        COLORREF ring) {
  if (radius <= 0 || corner < 0 || corner > 3) {
    return false;
  }
  // The circle is centred on the cap's inner corner — the card's corner
  // centre — so the quarter inside the cap is the card's arc.
  const double cx = (corner == 0 || corner == 2) ? radius : 0.0;
  const double cy = corner < 2 ? radius : 0.0;
  const double r = radius;
  auto ring_at = [&](double px, double py) {
    const double d = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
    return Clamp01(1.0 - std::abs(d - r - 0.5));  // The band [r, r + 1].
  };
  return PhiCommitShapedLayer(
      cap, radius, radius,
      [&](HDC dc) {
        // The tint everywhere, then the hairline's colour where the ring
        // runs — the coverage pass keys the alpha.
        GdiFill(dc, {0, 0, radius, radius}, tint);
        BlendCoverage(dc, {0, 0, radius, radius}, ring, ring_at);
      },
      [&](double px, double py) {
        const double d =
            std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
        return Clamp01(0.5 + (d - r));  // Outside the circle.
      });
}

}  // namespace phi_client

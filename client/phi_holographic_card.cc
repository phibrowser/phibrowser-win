// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_holographic_card.h"

#include <windowsx.h>

#include <objidl.h>
#include <gdiplus.h>

#include <commdlg.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <random>
#include <string_view>
#include <utility>

#include "base/containers/span.h"
#include "base/logging.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/no_destructor.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_fonts.h"
#include "phibrowser-win/client/phi_gdiplus.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "third_party/boringssl/src/include/openssl/sha.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "comdlg32.lib")

namespace phi_client {

namespace {

using Microsoft::WRL::ComPtr;
using Style = PhiHolographicCardStyle;

constexpr double kPi = 3.14159265358979323846;
constexpr double kDesignW = PhiHolographicCard::kDesignWidthDip;
constexpr double kCorner = PhiHolographicCard::kCornerDip;
// HolographicCardStyle's fixed rendering parameters.
constexpr double kGradientAngle = 200.0;
constexpr double kTextureOpacity = 0.72;
constexpr double kIridescenceOpacity = 0.78;
constexpr double kTextureContrast = 1.18;
// The entrance (HolographicCardMotion): 1.7 s, fading in over its first 18%.
constexpr double kEntranceSeconds = 1.7;
constexpr double kFadeFraction = 0.18;
// The 3D perspective (sublayerTransform.m34 = -1/1100) and the tilt range.
constexpr double kFocal = 1100.0;
constexpr double kTiltX = 10.0, kTiltY = 13.0, kTiltZ = 1.7;
// The shadow (the face layer's): black 24%, radius 20, offset (0, 14).
constexpr double kShadowOpacity = 0.24;
constexpr double kShadowRadius = 20.0;
constexpr double kShadowOffsetY = 14.0;
constexpr int kMarginDip = 40;
// The artwork version: bump when a drawing formula changes.
constexpr wchar_t kCacheVersion[] = L"v1";
constexpr char kDateJoinedKey[] = "profile.card.dateJoined";

// --- Colour helpers --------------------------------------------------------

struct Rgb {
  int r = 0, g = 0, b = 0;
};

constexpr Rgb Hex(uint32_t value) {
  return {static_cast<int>((value >> 16) & 255), static_cast<int>((value >> 8) & 255),
          static_cast<int>(value & 255)};
}

// A gradient stop: a position 0..1, a colour and an alpha 0..1.
struct Stop {
  double pos;
  Rgb rgb;
  double alpha = 1.0;
};

// 256 samples of a gradient, packed B, G, R, A (straight alpha).
struct Lut {
  std::array<uint32_t, 256> px{};
  uint32_t operator[](size_t i) const { return px[i]; }
};

Lut MakeLut(const std::vector<Stop>& stops) {
  Lut lut;
  for (size_t i = 0; i < 256; ++i) {
    const double t = i / 255.0;
    size_t k = 0;
    while (k + 1 < stops.size() && stops[k + 1].pos < t) {
      ++k;
    }
    const Stop& a = stops[k];
    const Stop& b = stops[std::min(k + 1, stops.size() - 1)];
    const double span = b.pos - a.pos;
    const double f = t <= a.pos ? 0.0 : t >= b.pos ? 1.0 : span > 0 ? (t - a.pos) / span : 1.0;
    const auto mix = [&](double x, double y) {
      return static_cast<uint32_t>(std::lround(std::clamp(x + (y - x) * f, 0.0, 255.0)));
    };
    lut.px[i] = mix(a.rgb.b, b.rgb.b) | (mix(a.rgb.g, b.rgb.g) << 8) |
                (mix(a.rgb.r, b.rgb.r) << 16) |
                (mix(a.alpha * 255, b.alpha * 255) << 24);
  }
  return lut;
}

// 256 x 256 blend tables: table[b * 256 + s] for a backdrop b and source s.
const std::vector<uint8_t>& OverlayTable() {
  static const base::NoDestructor<std::vector<uint8_t>> table([] {
    std::vector<uint8_t> t(65536);
    for (int b = 0; b < 256; ++b) {
      for (int s = 0; s < 256; ++s) {
        const int v = b < 128 ? (2 * b * s + 127) / 255
                              : 255 - (2 * (255 - b) * (255 - s) + 127) / 255;
        t[static_cast<size_t>(b) * 256 + s] = static_cast<uint8_t>(std::clamp(v, 0, 255));
      }
    }
    return t;
  }());
  return *table;
}

const std::vector<uint8_t>& SoftLightTable() {
  static const base::NoDestructor<std::vector<uint8_t>> table([] {
    std::vector<uint8_t> t(65536);
    for (int bi = 0; bi < 256; ++bi) {
      const double b = bi / 255.0;
      for (int si = 0; si < 256; ++si) {
        const double s = si / 255.0;
        double v = 0;
        if (s <= 0.5) {
          v = b - (1 - 2 * s) * b * (1 - b);
        } else {
          const double d = b <= 0.25 ? ((16 * b - 12) * b + 4) * b : std::sqrt(b);
          v = b + (2 * s - 1) * (d - b);
        }
        t[static_cast<size_t>(bi) * 256 + si] =
            static_cast<uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255));
      }
    }
    return t;
  }());
  return *table;
}

inline int Mul255(int a, int b) {
  return (a * b + 127) / 255;
}

// a + (b - a) * t / 255.
inline int Lerp255(int a, int b, int t) {
  return a + ((b - a) * t + (b >= a ? 127 : -127)) / 255;
}

// --- Geometry ---------------------------------------------------------------

// A linear gradient's parameter as an affine function of the dip position:
// t = ax * x + ay * y + c (the Mac's CAGradientLayer start/end points, from
// HolographicCardFace.light's `position`).
struct Grad {
  double ax = 0, ay = 0, c = 0;
  double At(double x, double y) const { return ax * x + ay * y + c; }
};

Grad MakeGrad(double angle_deg, double factor, bool moving, double lx, double ly,
              double width, double height) {
  const double rad = angle_deg * kPi / 180.0;
  const double dx = std::sin(rad), dy = -std::cos(rad);
  const double gw = width * factor, gh = height * factor;
  const double length = std::abs(dx) * gw + std::abs(dy) * gh;
  const double mx = moving ? 0.5 + lx * 0.35 : 0.5;
  const double my = moving ? 0.5 + ly * 0.35 : 0.5;
  const double cx = (width - gw) * mx + gw / 2;
  const double cy = (height - gh) * my + gh / 2;
  const double sx = cx - dx * length / 2, sy = cy - dy * length / 2;
  Grad g;
  g.ax = dx / length;
  g.ay = dy / length;
  g.c = -(sx * dx + sy * dy) / length;
  return g;
}

// The seal's placement: a 100 x 100 local space scaled to 46% of the card's
// width, rotated -12 degrees, centred at (1.11 W, 1.07 H) less half its
// width — the bottom-right corner, running off the card.
struct SealPlace {
  double cx, cy, k;  // Centre in dips; dips per local unit.
};

SealPlace MakeSealPlace(double width, double height) {
  const double sw = width * 0.46;
  return {width * 1.11 - sw / 2, height * 1.07 - sw / 2, sw / 100.0};
}

// The seal's gradient parameter as a function of the dip position: the
// SVG's 0,0 -> 100,100 line rotated by `rotation` and shifted by the light.
Grad MakeSealGrad(const SealPlace& place, double rotation, double lx, double ly) {
  const double cr = std::cos(rotation), sr = std::sin(rotation);
  // sealPoint(0) and (100) in local units.
  const double sx = 50 - 50 * cr + 50 * sr + lx * 34;
  const double sy = 50 - 50 * sr - 50 * cr + ly * 30;
  const double ddx = 100 * (cr - sr), ddy = 100 * (sr + cr);
  const double len2 = ddx * ddx + ddy * ddy;
  // local = 50 + R(+12deg) * (p - centre) / k.
  const double c12 = std::cos(12 * kPi / 180), s12 = std::sin(12 * kPi / 180);
  // lx(p) = 50 + ((px - cx) c12 - (py - cy) s12) / k
  // ly(p) = 50 + ((px - cx) s12 + (py - cy) c12) / k
  // t = ((lx - sx) ddx + (ly - sy) ddy) / len2
  Grad g;
  g.ax = (c12 * ddx + s12 * ddy) / (place.k * len2);
  g.ay = (-s12 * ddx + c12 * ddy) / (place.k * len2);
  const double lx0 = 50 - (place.cx * c12 - place.cy * s12) / place.k;
  const double ly0 = 50 - (place.cx * s12 + place.cy * c12) / place.k;
  g.c = ((lx0 - sx) * ddx + (ly0 - sy) * ddy) / len2;
  return g;
}

// The Phi logo's vector outline (HolographicCardArtwork.logoPath): a
// 960-unit path placed at (0.0647 W, 0.06 H) and scaled to 0.1376 W.
struct PathCmd {
  int op;  // 0 move, 1 line, 2 cubic, 3 close
  float a, b, c, d, e, f;
};

constexpr std::array<PathCmd, 19> kLogoPath = {{
    {0, 696.692f, 392.315f, 0, 0, 0, 0},
    {2, 696.692f, 298.625f, 620.397f, 222.389f, 526.566f, 222.389f},
    {2, 414.639f, 222.389f, 323.463f, 313.426f, 323.463f, 425.336f},
    {2, 323.463f, 558.679f, 432.036f, 667.169f, 565.482f, 667.169f},
    {2, 724.219f, 667.169f, 853.333f, 538.224f, 853.333f, 379.539f},
    {2, 853.333f, 191.392f, 700.115f, 38.2222f, 511.685f, 38.2222f},
    {2, 288.32f, 38.2222f, 106.667f, 219.736f, 106.667f, 442.929f},
    {2, 106.667f, 701.308f, 312.424f, 912.562f, 568.905f, 921.778f},
    {1, 568.905f, 701.727f, 0, 0, 0, 0},
    {1, 565.552f, 701.727f, 0, 0, 0, 0},
    {2, 413.033f, 701.727f, 288.949f, 577.738f, 288.949f, 425.336f},
    {2, 288.949f, 294.437f, 395.566f, 187.832f, 526.636f, 187.832f},
    {2, 639.541f, 187.832f, 731.346f, 279.566f, 731.346f, 392.315f},
    {2, 731.346f, 488.727f, 653.584f, 566.987f, 557.447f, 568.453f},
    {2, 507.055f, 568.453f, 463.896f, 527.946f, 463.896f, 476.579f},
    {2, 463.896f, 426.174f, 505.187f, 384.426f, 556.12f, 384.426f},
    {2, 607.053f, 384.426f, 648.344f, 425.685f, 648.344f, 476.579f},
    {2, 648.344f, 485.794f, 645.619f, 494.172f, 643.104f, 502.55f},
    {2, 675.522f, 476.509f, 696.762f, 437.065f, 696.762f, 392.315f},
}};

void AddPathCmds(Gdiplus::GraphicsPath* path, base::span<const PathCmd> cmds) {
  Gdiplus::PointF current(0, 0);
  for (const PathCmd& cmd : cmds) {
    switch (cmd.op) {
      case 0:
        path->StartFigure();
        current = Gdiplus::PointF(cmd.a, cmd.b);
        break;
      case 1:
        path->AddLine(current, Gdiplus::PointF(cmd.a, cmd.b));
        current = Gdiplus::PointF(cmd.a, cmd.b);
        break;
      case 2:
        path->AddBezier(current, Gdiplus::PointF(cmd.a, cmd.b),
                        Gdiplus::PointF(cmd.c, cmd.d), Gdiplus::PointF(cmd.e, cmd.f));
        current = Gdiplus::PointF(cmd.e, cmd.f);
        break;
      default:
        path->CloseFigure();
        break;
    }
  }
}

// The seal formulas (HolographicCardArtwork.makeSealPath): polylines in a
// 100 x 100 space, three decimals like the SVG's serialisation.
std::vector<std::vector<Gdiplus::PointF>> SealContours(Style::Ornament ornament) {
  std::vector<std::vector<Gdiplus::PointF>> contours;
  if (ornament == Style::Ornament::kNone) {
    return contours;
  }
  const bool closed = ornament == Style::Ornament::kLace || ornament == Style::Ornament::kLotus;
  const bool epicycloid = ornament == Style::Ornament::kEpicycloid;
  const int steps = closed ? 2880 : (epicycloid ? 16 : 20) * 720;
  const int lines = closed ? 6 : 1;
  for (int line = 0; line < lines; ++line) {
    std::vector<Gdiplus::PointF> points;
    points.reserve(static_cast<size_t>(steps) + 1);
    for (int sample = 0; sample <= steps; ++sample) {
      const double u = static_cast<double>(sample) / steps * kPi * 2;
      double x = 0, y = 0;
      if (closed) {
        const bool lotus = ornament == Style::Ornament::kLotus;
        const double phase = lotus ? u + kPi / 10 : u;
        const double radius = lotus ? 28.5 + (12.3 + line * 0.46) * std::cos(10 * u)
                                    : 32 + (9.5 + line * 0.48) * std::cos(18 * u);
        const double tangent = lotus ? (10.5 + line * 0.32) * std::sin(10 * u)
                                     : (5.8 + line * 0.28) * std::sin(18 * u);
        x = radius * std::cos(phase) - tangent * std::sin(phase);
        y = radius * std::sin(phase) + tangent * std::cos(phase);
      } else if (epicycloid) {
        const double r = 6.2, big_r = 9.4, t = u * 16;
        const double scale = 44 / (big_r + 2 * r), k = (big_r + r) / r;
        x = scale * ((big_r + r) * std::cos(t) - r * std::cos(k * t));
        y = scale * ((big_r + r) * std::sin(t) - r * std::sin(k * t));
      } else {
        const bool modulated = ornament == Style::Ornament::kModulated;
        const double p = modulated ? 1.5 : 0, c = modulated ? 0.02 : 0;
        const double t = u * 20;
        const double radius = 44 / (10 + std::abs(p)) * (8 - 2 * std::cos(6 * t) - p * std::cos(c * t));
        x = radius * std::cos(0.99 * t);
        y = radius * std::sin(0.99 * t);
      }
      points.emplace_back(static_cast<float>(std::round((50 + x) * 1000) / 1000),
                          static_cast<float>(std::round((50 + y) * 1000) / 1000));
    }
    contours.push_back(std::move(points));
  }
  return contours;
}

// --- Pixel buffers ------------------------------------------------------------

// A GDI+ drawing surface over a caller-owned premultiplied BGRA buffer
// (PixelFormat32bppPARGB), antialiased. GDI+ writes straight into `pixels`.
class Canvas {
 public:
  Canvas(std::vector<uint8_t>* pixels, int width, int height)
      : bitmap_(width, height, width * 4, PixelFormat32bppPARGB, pixels->data()),
        graphics_(&bitmap_) {
    graphics_.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics_.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics_.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
  }
  ~Canvas() { graphics_.Flush(Gdiplus::FlushIntentionSync); }
  Gdiplus::Graphics& graphics() { return graphics_; }

 private:
  Gdiplus::Bitmap bitmap_;
  Gdiplus::Graphics graphics_;
};

Gdiplus::Color Argb(uint32_t rgb, double alpha = 1.0) {
  return Gdiplus::Color(static_cast<BYTE>(std::lround(std::clamp(alpha, 0.0, 1.0) * 255)),
                        static_cast<BYTE>((rgb >> 16) & 255), static_cast<BYTE>((rgb >> 8) & 255),
                        static_cast<BYTE>(rgb & 255));
}

// A linear gradient brush that CLAMPS beyond its ends, like CGGradient with
// drawsBefore/AfterStartLocation: the line is stretched to cover `cover`
// and the stops remapped, since GDI+ would tile the gradient instead.
std::unique_ptr<Gdiplus::LinearGradientBrush> ClampedGradient(
    Gdiplus::PointF start, Gdiplus::PointF end,
    const std::vector<std::pair<uint32_t, float>>& stops, const Gdiplus::RectF& cover) {
  const double dx = end.X - start.X, dy = end.Y - start.Y;
  const double len2 = std::max(1e-6, dx * dx + dy * dy);
  double tmin = 0, tmax = 1;
  for (int i = 0; i < 4; ++i) {
    const double px = (i & 1) ? cover.X + cover.Width : cover.X;
    const double py = (i & 2) ? cover.Y + cover.Height : cover.Y;
    const double t = ((px - start.X) * dx + (py - start.Y) * dy) / len2;
    tmin = std::min(tmin, t);
    tmax = std::max(tmax, t);
  }
  tmin -= 0.01;
  tmax += 0.01;
  const Gdiplus::PointF s(static_cast<float>(start.X + dx * tmin), static_cast<float>(start.Y + dy * tmin));
  const Gdiplus::PointF e(static_cast<float>(start.X + dx * tmax), static_cast<float>(start.Y + dy * tmax));
  auto brush = std::make_unique<Gdiplus::LinearGradientBrush>(
      s, e, Argb(stops.front().first), Argb(stops.back().first));
  std::vector<Gdiplus::Color> colors;
  std::vector<float> positions;
  colors.push_back(Argb(stops.front().first));
  positions.push_back(0.0f);
  for (const auto& [rgb, pos] : stops) {
    colors.push_back(Argb(rgb));
    positions.push_back(static_cast<float>((pos - tmin) / (tmax - tmin)));
  }
  colors.push_back(Argb(stops.back().first));
  positions.push_back(1.0f);
  brush->SetInterpolationColors(colors.data(), positions.data(), static_cast<INT>(colors.size()));
  return brush;
}

// --- WIC -------------------------------------------------------------------------

ComPtr<IWICImagingFactory> WicFactory() {
  ComPtr<IWICImagingFactory> factory;
  ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&factory));
  return factory;
}

// Writes `bgra` (straight alpha, width x height) as a 32-bit PNG.
bool WritePng(IWICImagingFactory* factory, const std::wstring& path,
              const std::vector<uint8_t>& bgra, int width, int height) {
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  if (!factory || FAILED(factory->CreateStream(&stream)) ||
      FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
      FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
      FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) {
    return false;
  }
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (FAILED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height))) ||
      FAILED(frame->SetPixelFormat(&format))) {
    return false;
  }
  std::vector<uint8_t> pixels = bgra;
  return SUCCEEDED(frame->WritePixels(static_cast<UINT>(height), static_cast<UINT>(width * 4),
                                      static_cast<UINT>(pixels.size()), pixels.data())) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

// Reads a 32-bit PNG of exactly `width` x `height` into straight BGRA.
bool ReadPng(IWICImagingFactory* factory, const std::wstring& path, int width, int height,
             std::vector<uint8_t>* out) {
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  if (!factory ||
      FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnDemand, &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame))) {
    return false;
  }
  UINT w = 0, h = 0;
  if (FAILED(frame->GetSize(&w, &h)) || w != static_cast<UINT>(width) ||
      h != static_cast<UINT>(height)) {
    return false;
  }
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
    return false;
  }
  out->assign(static_cast<size_t>(width) * height * 4, 0);
  return SUCCEEDED(converter->CopyPixels(nullptr, static_cast<UINT>(width * 4),
                                         static_cast<UINT>(out->size()), out->data()));
}

// A top-down 32bpp DIB with a DC, for GDI text coverage.
struct Scratch {
  HDC dc = nullptr;
  HBITMAP bitmap = nullptr;
  HGDIOBJ previous = nullptr;
  RAW_PTR_EXCLUSION uint8_t* bits = nullptr;  // Owned by `bitmap`.
  int width = 0;
  int height = 0;

  base::span<uint8_t> pixels() const {
    // SAFETY: the DIB section is width * height * 4 bytes, allocated by
    // CreateDIBSection for exactly that BITMAPINFO.
    return UNSAFE_BUFFERS(base::span(bits, static_cast<size_t>(width) * height * 4));
  }
  bool Create(int w, int h) {
    width = w;
    height = h;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* raw = nullptr;
    bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
    if (!bitmap) {
      return false;
    }
    bits = static_cast<uint8_t*>(raw);
    dc = ::CreateCompatibleDC(nullptr);
    previous = ::SelectObject(dc, bitmap);
    return true;
  }
  ~Scratch() {
    if (dc) {
      ::SelectObject(dc, previous);
      ::DeleteDC(dc);
    }
    if (bitmap) {
      ::DeleteObject(bitmap);
    }
  }
};

HFONT MakeFont(std::wstring_view face, int pixel_height, int weight) {
  LOGFONTW lf = {};
  lf.lfHeight = -std::max(1, pixel_height);
  lf.lfWeight = weight;
  lf.lfCharSet = DEFAULT_CHARSET;
  lf.lfQuality = ANTIALIASED_QUALITY;
  const base::span<wchar_t> name(lf.lfFaceName);
  const size_t n = std::min(face.size(), name.size() - 1);
  std::ranges::copy(face.substr(0, n), name.begin());
  name[n] = L'\0';
  return ::CreateFontIndirectW(&lf);
}

// Renders `text` (single line, left, top, end-ellipsis) into a `width` x
// `height` coverage plane.
std::vector<uint8_t> TextCoverage(const std::wstring& text, HFONT font, int width, int height) {
  std::vector<uint8_t> coverage(static_cast<size_t>(std::max(0, width)) * std::max(0, height), 0);
  Scratch scratch;
  if (width <= 0 || height <= 0 || !font || !scratch.Create(width, height)) {
    return coverage;
  }
  std::ranges::fill(scratch.pixels(), 0);
  HGDIOBJ old_font = ::SelectObject(scratch.dc, font);
  ::SetBkMode(scratch.dc, TRANSPARENT);
  ::SetTextColor(scratch.dc, RGB(255, 255, 255));
  RECT rc = {0, 0, width, height};
  PhiDrawTextAA(scratch.dc, text.c_str(), -1, &rc,
                DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  ::SelectObject(scratch.dc, old_font);
  ::GdiFlush();
  const base::span<uint8_t> pixels = scratch.pixels();
  for (size_t i = 0; i < coverage.size(); ++i) {
    coverage[i] = pixels[i * 4 + 1];
  }
  return coverage;
}

// Composites `ink` at `alpha` through `coverage` (w x h at (left, top)) over
// a premultiplied BGRA layer of `width` x `height`.
void InkOver(std::vector<uint8_t>* layer, int width, int height,
             const std::vector<uint8_t>& coverage, int w, int h, int left, int top,
             Rgb ink, double alpha) {
  for (int y = 0; y < h; ++y) {
    const int ly = top + y;
    if (ly < 0 || ly >= height) {
      continue;
    }
    for (int x = 0; x < w; ++x) {
      const int lx = left + x;
      const int a = static_cast<int>(std::lround(coverage[static_cast<size_t>(y) * w + x] * alpha));
      if (lx < 0 || lx >= width || a == 0) {
        continue;
      }
      const size_t p = (static_cast<size_t>(ly) * width + lx) * 4;
      const int inv = 255 - a;
      (*layer)[p] = static_cast<uint8_t>(Mul255(ink.b, a) + Mul255((*layer)[p], inv));
      (*layer)[p + 1] = static_cast<uint8_t>(Mul255(ink.g, a) + Mul255((*layer)[p + 1], inv));
      (*layer)[p + 2] = static_cast<uint8_t>(Mul255(ink.r, a) + Mul255((*layer)[p + 2], inv));
      (*layer)[p + 3] = static_cast<uint8_t>(a + Mul255((*layer)[p + 3], inv));
    }
  }
}

uint64_t NowMicros() {
  static const LARGE_INTEGER frequency = [] {
    LARGE_INTEGER f;
    ::QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER counter;
  ::QueryPerformanceCounter(&counter);
  return static_cast<uint64_t>(counter.QuadPart * 1000000.0 / frequency.QuadPart);
}

}  // namespace

// --- Style ---------------------------------------------------------------------

namespace {

constexpr std::array<const char*, Style::kPaletteCount> kPaletteNames = {
    "silver", "green", "aqua", "blue", "violet", "pink", "coral", "gold"};
constexpr std::array<const char*, Style::kPaletteCount> kPaletteThemes = {
    "pure", "mint", "aqua", "mist", "iris", "petal", "coral", "amber"};
constexpr std::array<const char*, Style::kTextureCount> kTextureNames = {
    "arcs", "waves", "guilloche", "brushed", "diamond", "stardust"};
constexpr std::array<const char*, Style::kOrnamentCount> kOrnamentNames = {
    "lace", "lotus", "periodic", "modulated", "epicycloid", "none"};
// Primary, secondary and accent per palette (app.js presets).
constexpr std::array<std::array<uint32_t, 3>, Style::kPaletteCount> kPaletteColors = {{
    {0xBFC3C9, 0xE8EAEC, 0x929AA6},
    {0x8DDA83, 0xEBDD87, 0x43BFC5},
    {0x72D9DF, 0xB5E9A1, 0x8980E5},
    {0x63CFFD, 0x83E4C5, 0xA777E8},
    {0x887EFA, 0xE3A0D7, 0x54C8DE},
    {0xDB79E5, 0xF5B98D, 0x818CEB},
    {0xF9857B, 0xF5D781, 0xBF75D4},
    {0xF6D879, 0xA6DFBB, 0xE88B9E},
}};

template <typename E, size_t N>
E ParseName(const std::array<const char*, N>& names, const std::string& value, E fallback) {
  for (size_t i = 0; i < N; ++i) {
    if (value == names[i]) {
      return static_cast<E>(i);
    }
  }
  return fallback;
}

}  // namespace

// static
PhiHolographicCardStyle PhiHolographicCardStyle::Random() {
  std::random_device device;
  std::mt19937 rng(device());
  PhiHolographicCardStyle style;
  style.palette = static_cast<Palette>(
      std::uniform_int_distribution<int>(0, kPaletteCount - 1)(rng));
  style.texture = static_cast<Texture>(
      std::uniform_int_distribution<int>(0, kTextureCount - 1)(rng));
  style.ornament = static_cast<Ornament>(
      std::uniform_int_distribution<int>(0, kOrnamentCount - 1)(rng));
  return style;
}

// static
PhiHolographicCardStyle::Palette PhiHolographicCardStyle::PaletteForTheme(
    const std::string& theme_id) {
  return ParseName(kPaletteThemes, theme_id, Palette::kSilver);
}

// static
const char* PhiHolographicCardStyle::ThemeIdFor(Palette palette) {
  return kPaletteThemes[static_cast<size_t>(palette)];
}

// static
const char* PhiHolographicCardStyle::Name(Palette palette) {
  return kPaletteNames[static_cast<size_t>(palette)];
}
// static
const char* PhiHolographicCardStyle::Name(Texture texture) {
  return kTextureNames[static_cast<size_t>(texture)];
}
// static
const char* PhiHolographicCardStyle::Name(Ornament ornament) {
  return kOrnamentNames[static_cast<size_t>(ornament)];
}

std::map<std::string, std::string> PhiHolographicCardStyle::ToMap() const {
  return {{"palette", Name(palette)}, {"texture", Name(texture)}, {"ornament", Name(ornament)}};
}

// static
PhiHolographicCardStyle PhiHolographicCardStyle::FromMap(
    const std::map<std::string, std::string>& map) {
  PhiHolographicCardStyle style;
  const auto value = [&](const char* key) {
    const auto found = map.find(key);
    return found == map.end() ? std::string() : found->second;
  };
  style.palette = ParseName(kPaletteNames, value("palette"), Palette::kSilver);
  style.texture = ParseName(kTextureNames, value("texture"), Texture::kArcs);
  style.ornament = ParseName(kOrnamentNames, value("ornament"), Ornament::kLace);
  return style;
}

bool PhiHolographicCardStyle::enhanced_ornament() const {
  return ornament == Ornament::kPeriodic || ornament == Ornament::kModulated ||
         ornament == Ornament::kEpicycloid;
}

// --- Materials ----------------------------------------------------------------

// The worker's product for one texture, ornament and pixel size.
struct PhiHolographicCard::Material {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> foil;          // Opaque BGRA, contrast applied.
  std::vector<uint8_t> grain_factor;  // Per-channel multiply factor, BGRx; 255 = none.
  std::vector<uint8_t> seal_groove;   // Coverage 0..255.
  std::vector<uint8_t> seal_reflect;  // Coverage 0..255.
  RECT seal_box = {0, 0, 0, 0};       // Where either coverage is non-zero.
};

struct PhiHolographicCard::Mailbox {
  CRITICAL_SECTION lock;
  std::vector<std::pair<std::string, std::shared_ptr<const Material>>> done;
  Mailbox() { ::InitializeCriticalSection(&lock); }
  ~Mailbox() { ::DeleteCriticalSection(&lock); }
};

struct PhiHolographicCard::Job {
  std::shared_ptr<Mailbox> mailbox;
  std::string key;
  Style style;
  int width = 0;
  int height = 0;
  std::wstring dir;  // The versioned cache directory, or empty.
  HWND notify_hwnd = nullptr;
  UINT notify_msg = 0;
};

namespace {

std::string MaterialKey(const Style& style, int width, int height) {
  return std::string(Style::Name(style.texture)) + "-" + Style::Name(style.ornament) + "-" +
         std::to_string(width) + "x" + std::to_string(height);
}

std::wstring SizeTag(int width, int height) {
  return std::to_wstring(width) + L"x" + std::to_wstring(height);
}

std::wstring Widen(const char* ascii) {
  return base::ASCIIToWide(ascii);
}

// The foil texture (HolographicCardArtwork.makeFoil): a grey ground with
// one of six engraved patterns, then CSS contrast(1.18). Drawn in dips
// through a scale transform so the engraving's hairlines scale with DPI.
std::vector<uint8_t> MakeFoil(Style::Texture texture, int width, int height) {
  const double s = width / kDesignW;
  const float w = static_cast<float>(kDesignW), h = static_cast<float>(height / s);
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4, 0);
  {
    Canvas canvas(&pixels, width, height);
    Gdiplus::Graphics& g = canvas.graphics();
    g.Clear(Argb(0x898989));
    g.ScaleTransform(static_cast<float>(s), static_cast<float>(s));
    int64_t seed = 91;
    const auto random = [&seed] {
      seed = seed * 16807 % 2147483647;
      return (seed - 1) / 2147483646.0;
    };
    const auto engrave = [&g](Gdiplus::GraphicsPath& path, double light = 0.8, double dark = 0.6,
                              double width_dip = 0.65) {
      Gdiplus::Pen dark_pen(Argb(0x0f141e, dark), static_cast<float>(width_dip));
      Gdiplus::Pen light_pen(Argb(0xffffff, light), static_cast<float>(width_dip));
      const Gdiplus::GraphicsState state = g.Save();
      g.TranslateTransform(0.6f, 0.7f);
      g.DrawPath(&dark_pen, &path);
      g.Restore(state);
      g.DrawPath(&light_pen, &path);
    };
    const auto diamond = [](Gdiplus::GraphicsPath& path, float x, float y, float r) {
      const std::array<Gdiplus::PointF, 4> pts = {
          Gdiplus::PointF(x, y - r), Gdiplus::PointF(x + r, y), Gdiplus::PointF(x, y + r),
          Gdiplus::PointF(x - r, y)};
      path.AddPolygon(pts.data(), 4);
    };
    switch (texture) {
      case Style::Texture::kArcs: {
        const float radius = 32, spacing = 51.2f, row_height = 19.2f;
        for (int row = -2; row * row_height < h + radius; ++row) {
          const float cy = row * row_height;
          const float offset = row % 2 == 0 ? 0 : spacing / 2;
          for (float cx = -spacing + offset; cx < w + spacing; cx += spacing) {
            Gdiplus::GraphicsPath cell;
            cell.AddArc(Gdiplus::RectF(cx - radius, cy - radius, 2 * radius, 2 * radius), 0, 180);
            cell.CloseFigure();
            const Gdiplus::GraphicsState state = g.Save();
            g.SetClip(&cell);
            auto brush = ClampedGradient(
                Gdiplus::PointF(cx - radius, cy - radius), Gdiplus::PointF(cx + radius, cy),
                {{0xa0a0a0, 0.0f}, {0x858585, 0.5f}, {0x969696, 1.0f}},
                Gdiplus::RectF(cx - radius, cy, 2 * radius, radius));
            g.FillRectangle(brush.get(), Gdiplus::RectF(cx - radius, cy - 1, 2 * radius, radius + 2));
            for (float r = 4; r < radius; r += 2.16f) {
              Gdiplus::GraphicsPath arc;
              arc.AddArc(Gdiplus::RectF(cx - r, cy - r, 2 * r, 2 * r), 0, 180);
              engrave(arc, 0.85, 0.65, 0.56);
            }
            g.Restore(state);
          }
        }
        break;
      }
      case Style::Texture::kWaves: {
        for (float row = -45; row < h + 45; row += 3.1f) {
          std::vector<Gdiplus::PointF> pts;
          for (float x = -5; x < w + 5; x += 2) {
            const double y = row + 14 * std::sin(x * 0.031 + row * 0.008) +
                             7 * std::sin(x * 0.063 - row * 0.012);
            pts.emplace_back(x, static_cast<float>(y));
          }
          Gdiplus::GraphicsPath path;
          path.AddLines(pts.data(), static_cast<INT>(pts.size()));
          engrave(path);
        }
        break;
      }
      case Style::Texture::kGuilloche: {
        for (float cy = 35; cy < h + 65; cy += 85) {
          for (float cx = 30; cx < w + 65; cx += 85) {
            for (float r = 8; r < 57; r += 2.4f) {
              std::vector<Gdiplus::PointF> pts;
              for (double t = 0; t <= kPi * 2 + 0.02; t += 0.02) {
                const double radius = r + 5 * std::sin(8 * t + r * 0.07);
                pts.emplace_back(static_cast<float>(cx + radius * std::cos(t)),
                                 static_cast<float>(cy + radius * std::sin(t)));
              }
              Gdiplus::GraphicsPath path;
              path.AddLines(pts.data(), static_cast<INT>(pts.size()));
              engrave(path, 0.7, 0.5, 0.5);
            }
          }
        }
        break;
      }
      case Style::Texture::kBrushed: {
        for (float y = 0; y < h; y += 0.8f) {
          const double start = random() * w * 0.25;
          const double length = w * (0.35 + random() * 0.7);
          const double light = 0.15 + random() * 0.35;
          const double dark = 0.12 + random() * 0.3;
          Gdiplus::GraphicsPath path;
          path.AddLine(Gdiplus::PointF(static_cast<float>(start), y),
                       Gdiplus::PointF(static_cast<float>(start + length), y + 1.5f));
          engrave(path, light, dark, 0.4);
        }
        break;
      }
      case Style::Texture::kDiamond: {
        for (float y = -24; y < h + 24; y += 24) {
          for (float x = -24; x < w + 24; x += 24) {
            Gdiplus::GraphicsPath cell;
            diamond(cell, x + 12, y + 12, 12);
            const Gdiplus::GraphicsState state = g.Save();
            g.SetClip(&cell);
            auto brush = ClampedGradient(
                Gdiplus::PointF(x, y), Gdiplus::PointF(x + 24, y + 24),
                {{0xc2c2c2, 0.0f}, {0x858585, 0.5f}, {0x626262, 1.0f}},
                Gdiplus::RectF(x, y, 24, 24));
            g.FillRectangle(brush.get(), Gdiplus::RectF(x - 1, y - 1, 26, 26));
            g.Restore(state);
            for (float r = 3; r <= 12; r += 3) {
              Gdiplus::GraphicsPath ring;
              diamond(ring, x + 12, y + 12, r);
              engrave(ring, 0.7, 0.5, 0.55);
            }
          }
        }
        break;
      }
      case Style::Texture::kStardust: {
        const int motes = static_cast<int>(std::ceil(w * h / 64));
        for (int i = 0; i < motes; ++i) {
          const float x = static_cast<float>(random() * w), y = static_cast<float>(random() * h);
          const float size = static_cast<float>(0.65 + random() * 1.05);
          Gdiplus::SolidBrush shadow(Argb(0x0c121c, 0.55 + random() * 0.3));
          g.FillRectangle(&shadow, Gdiplus::RectF(x + 0.6f, y + 0.75f, size, size));
          Gdiplus::SolidBrush mote(Argb(0xffffff, 0.75 + random() * 0.25));
          g.FillRectangle(&mote, Gdiplus::RectF(x, y, size, size));
        }
        const int sparks = static_cast<int>(std::ceil(w * h / 5200));
        for (int i = 0; i < sparks; ++i) {
          const float x = static_cast<float>(random() * w), y = static_cast<float>(random() * h);
          const float r = static_cast<float>(1.5 + random() * 2);
          Gdiplus::GraphicsPath cross;
          cross.AddLine(Gdiplus::PointF(x - r, y), Gdiplus::PointF(x + r, y));
          cross.StartFigure();
          cross.AddLine(Gdiplus::PointF(x, y - r), Gdiplus::PointF(x, y + r));
          engrave(cross, 0.95, 0.7, 0.8);
        }
        break;
      }
    }
  }
  // CSS contrast(1.18) around sRGB 0.5, on the colour channels.
  for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
    for (size_t c = 0; c < 3; ++c) {
      const double v = (pixels[i + c] / 255.0 - 0.5) * kTextureContrast + 0.5;
      pixels[i + c] = static_cast<uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255));
    }
    pixels[i + 3] = 255;
  }
  return pixels;
}

// The grain (HolographicCardArtwork.grain): SVG feTurbulence defaults —
// seed 0, fractalNoise, base frequency .85, three octaves, a stitched
// 180-dip tile at opacity .8 — rasterised as a 360 x 360 tile and laid
// across the card at 180 dips a tile. Premultiplied BGRA.
std::vector<uint8_t> MakeGrain(int width, int height) {
  constexpr int kTile = 360;
  int64_t seed = 1;
  const auto random = [&seed] {
    seed = seed * 16807 % 2147483647;
    return static_cast<int>(seed);
  };
  std::array<int, 256> permutation{};
  for (int i = 0; i < 256; ++i) {
    permutation[static_cast<size_t>(i)] = i;
  }
  std::array<std::array<std::pair<double, double>, 256>, 4> vectors{};
  for (auto& channel : vectors) {
    for (auto& v : channel) {
      const double x = (random() % 512 - 256) / 256.0;
      const double y = (random() % 512 - 256) / 256.0;
      const double length = std::max(0.000001, std::hypot(x, y));
      v = {x / length, y / length};
    }
  }
  for (int i = 255; i >= 1; --i) {
    std::swap(permutation[static_cast<size_t>(i)], permutation[static_cast<size_t>(random() % 256)]);
  }
  const auto noise = [&](double x, double y, int channel, int period) {
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    const double fx = x - ix, fy = y - iy;
    const auto wrap = [period](int n) { return ((n % period + period) % period) & 255; };
    const auto dot = [&](int dx, int dy) {
      const size_t index = static_cast<size_t>(
          (permutation[static_cast<size_t>(wrap(ix + dx))] + wrap(iy + dy)) & 255);
      const auto& v = vectors[static_cast<size_t>(channel)][permutation[index]];
      return v.first * (fx - dx) + v.second * (fy - dy);
    };
    const double sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const double a = dot(0, 0) + sx * (dot(1, 0) - dot(0, 0));
    const double b = dot(0, 1) + sx * (dot(1, 1) - dot(0, 1));
    return a + sy * (b - a);
  };
  // The tile: straight sRGB colour and alpha (GDI+ premultiplies on draw).
  std::vector<uint8_t> tile(static_cast<size_t>(kTile) * kTile * 4, 0);
  for (int y = 0; y < kTile; ++y) {
    for (int x = 0; x < kTile; ++x) {
      std::array<double, 4> channels{};
      for (int channel = 0; channel < 4; ++channel) {
        double value = 0, frequency = 0.85, amplitude = 1;
        int period = 153;
        for (int octave = 0; octave < 3; ++octave) {
          value += noise((x + 0.5) / 2 * frequency, (y + 0.5) / 2 * frequency, channel, period) /
                   amplitude;
          frequency *= 2;
          amplitude *= 2;
          period *= 2;
        }
        channels[static_cast<size_t>(channel)] = std::clamp((value + 1) / 2, 0.0, 1.0);
      }
      const double alpha = channels[3] * 0.8;
      const size_t p = (static_cast<size_t>(y) * kTile + x) * 4;
      for (int channel = 0; channel < 3; ++channel) {
        const double linear = channels[static_cast<size_t>(channel)];
        const double srgb = linear <= 0.0031308 ? linear * 12.92
                                                : 1.055 * std::pow(linear, 1 / 2.4) - 0.055;
        // BGRA: channel 0 (R) lands in byte 2.
        tile[p + static_cast<size_t>(2 - channel)] =
            static_cast<uint8_t>(std::lround(std::clamp(srgb, 0.0, 1.0) * 255));
      }
      tile[p + 3] = static_cast<uint8_t>(std::lround(alpha * 255));
    }
  }
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4, 0);
  {
    Canvas canvas(&pixels, width, height);
    Gdiplus::Bitmap tile_bitmap(kTile, kTile, kTile * 4, PixelFormat32bppARGB, tile.data());
    canvas.graphics().Clear(Gdiplus::Color(0, 0, 0, 0));
    canvas.graphics().SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
    const double s = width / kDesignW;
    const float cell = static_cast<float>(180 * s);
    for (float y = 0; y < height; y += cell) {
      for (float x = 0; x < width; x += cell) {
        canvas.graphics().DrawImage(&tile_bitmap, Gdiplus::RectF(x, y, cell, cell), 0, 0,
                                    static_cast<float>(kTile), static_cast<float>(kTile),
                                    Gdiplus::UnitPixel);
      }
    }
  }
  return pixels;
}

// The seal's two stroked masks (the groove and its offset reflection) as
// one BGRA image: B = groove coverage, G = reflection coverage.
std::vector<uint8_t> MakeSealMasks(Style::Ornament ornament, bool enhanced, int width,
                                   int height) {
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4, 0);
  const auto contours = SealContours(ornament);
  if (contours.empty()) {
    return pixels;
  }
  const double s = width / kDesignW;
  const SealPlace place = MakeSealPlace(kDesignW, height / s);
  const bool closed = ornament == Style::Ornament::kLace || ornament == Style::Ornament::kLotus;
  const auto build = [&](float offset_x, float offset_y) {
    auto path = std::make_unique<Gdiplus::GraphicsPath>();
    for (const auto& contour : contours) {
      path->StartFigure();
      path->AddLines(contour.data(), static_cast<INT>(contour.size()));
      if (closed) {
        path->CloseFigure();
      }
    }
    Gdiplus::Matrix matrix;
    matrix.Translate(static_cast<float>(place.cx * s), static_cast<float>(place.cy * s));
    matrix.Rotate(-12.0f);
    matrix.Scale(static_cast<float>(place.k * s), static_cast<float>(place.k * s));
    matrix.Translate(-50.0f + offset_x, -50.0f + offset_y);
    path->Transform(&matrix);
    return path;
  };
  const auto stroke = [&](std::vector<uint8_t>* out, float offset_x, float offset_y,
                          double local_width) {
    auto path = build(offset_x, offset_y);
    Canvas canvas(out, width, height);
    canvas.graphics().Clear(Gdiplus::Color(0, 0, 0, 0));
    Gdiplus::Pen pen(Gdiplus::Color(255, 255, 255, 255),
                     static_cast<float>(std::max(0.5, local_width * place.k * s)));
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    canvas.graphics().DrawPath(&pen, path.get());
  };
  std::vector<uint8_t> groove(pixels.size(), 0), reflect(pixels.size(), 0);
  stroke(&groove, 0, 0, enhanced ? 0.26 : 0.22);
  stroke(&reflect, enhanced ? 0.2f : 0.16f, enhanced ? 0.25f : 0.2f, enhanced ? 0.21 : 0.16);
  for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
    pixels[i] = groove[i + 3];
    pixels[i + 1] = reflect[i + 3];
    pixels[i + 3] = 255;
  }
  return pixels;
}

bool EnsureDir(const std::wstring& dir) {
  if (dir.empty()) {
    return false;
  }
  // Two levels at most: <parent>\PhiCardArtwork\v1.
  const size_t slash = dir.find_last_of(L'\\');
  if (slash != std::wstring::npos) {
    ::CreateDirectoryW(dir.substr(0, slash).c_str(), nullptr);
  }
  ::CreateDirectoryW(dir.c_str(), nullptr);
  const DWORD attributes = ::GetFileAttributesW(dir.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Loads `file` from the cache or makes it with `make` and stores it.
std::vector<uint8_t> CachedImage(IWICImagingFactory* factory, const std::wstring& dir,
                                 const std::wstring& file, int width, int height,
                                 std::vector<uint8_t> (*make)(int, int)) {
  std::vector<uint8_t> pixels;
  const std::wstring path = dir.empty() ? std::wstring() : dir + L"\\" + file;
  if (!path.empty() && ReadPng(factory, path, width, height, &pixels)) {
    return pixels;
  }
  pixels = make(width, height);
  if (!path.empty() && EnsureDir(dir) && !WritePng(factory, path, pixels, width, height)) {
    LOG(WARNING) << "[HoloCard] cache write failed";
  }
  return pixels;
}

}  // namespace

// static
std::shared_ptr<const PhiHolographicCard::Material> PhiHolographicCard::BuildMaterial(
    IWICImagingFactory* factory, const std::wstring& dir, const PhiHolographicCardStyle& style,
    int width, int height) {
  auto material = std::make_shared<Material>();
  material->width = width;
  material->height = height;
  const std::wstring size = SizeTag(width, height);
  // The foil, by texture. The maker's signature fixes the texture through a
  // small trampoline table.
  {
    using Maker = std::vector<uint8_t> (*)(int, int);
    static constexpr std::array<Maker, Style::kTextureCount> kMakers = {
        [](int w, int h) { return MakeFoil(Style::Texture::kArcs, w, h); },
        [](int w, int h) { return MakeFoil(Style::Texture::kWaves, w, h); },
        [](int w, int h) { return MakeFoil(Style::Texture::kGuilloche, w, h); },
        [](int w, int h) { return MakeFoil(Style::Texture::kBrushed, w, h); },
        [](int w, int h) { return MakeFoil(Style::Texture::kDiamond, w, h); },
        [](int w, int h) { return MakeFoil(Style::Texture::kStardust, w, h); },
    };
    material->foil = CachedImage(
        factory, dir,
        L"foil-" + Widen(Style::Name(style.texture)) + L"-" + size + L".png", width,
        height, kMakers[static_cast<size_t>(style.texture)]);
  }
  {
    // The grain's multiply factor at opacity .13: 1 - .13 * (a - a*g), from
    // the premultiplied tile.
    const std::vector<uint8_t> grain =
        CachedImage(factory, dir, L"grain-" + size + L".png", width, height, &MakeGrain);
    material->grain_factor.assign(grain.size(), 255);
    for (size_t i = 0; i + 3 < grain.size(); i += 4) {
      const int a = grain[i + 3];
      for (size_t c = 0; c < 3; ++c) {
        const int m = 255 - static_cast<int>(std::lround(0.13 * (a - grain[i + c])));
        material->grain_factor[i + c] = static_cast<uint8_t>(std::clamp(m, 0, 255));
      }
    }
  }
  {
    using Maker = std::vector<uint8_t> (*)(int, int);
    static constexpr std::array<Maker, Style::kOrnamentCount> kMakers = {
        [](int w, int h) { return MakeSealMasks(Style::Ornament::kLace, false, w, h); },
        [](int w, int h) { return MakeSealMasks(Style::Ornament::kLotus, false, w, h); },
        [](int w, int h) { return MakeSealMasks(Style::Ornament::kPeriodic, true, w, h); },
        [](int w, int h) { return MakeSealMasks(Style::Ornament::kModulated, true, w, h); },
        [](int w, int h) { return MakeSealMasks(Style::Ornament::kEpicycloid, true, w, h); },
        [](int w, int h) { return MakeSealMasks(Style::Ornament::kNone, false, w, h); },
    };
    const size_t count = static_cast<size_t>(width) * height;
    material->seal_groove.assign(count, 0);
    material->seal_reflect.assign(count, 0);
    if (style.ornament != Style::Ornament::kNone) {
      const std::vector<uint8_t> masks = CachedImage(
          factory, dir,
          L"seal-" + Widen(Style::Name(style.ornament)) + L"-" + size + L".png", width,
          height, kMakers[static_cast<size_t>(style.ornament)]);
      RECT box = {width, height, 0, 0};
      for (size_t i = 0; i < count && i * 4 + 1 < masks.size(); ++i) {
        material->seal_groove[i] = masks[i * 4];
        material->seal_reflect[i] = masks[i * 4 + 1];
        if (masks[i * 4] || masks[i * 4 + 1]) {
          const LONG x = static_cast<LONG>(i % static_cast<size_t>(width));
          const LONG y = static_cast<LONG>(i / static_cast<size_t>(width));
          box.left = std::min(box.left, x);
          box.top = std::min(box.top, y);
          box.right = std::max(box.right, x + 1);
          box.bottom = std::max(box.bottom, y + 1);
        }
      }
      if (box.right > box.left && box.bottom > box.top) {
        material->seal_box = box;
      }
    }
  }
  return material;
}

// static
DWORD WINAPI PhiHolographicCard::MaterialThread(void* param) {
  std::unique_ptr<Job> job(static_cast<Job*>(param));
  const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ComPtr<IWICImagingFactory> factory = WicFactory();
  std::shared_ptr<const Material> material =
      BuildMaterial(factory.Get(), job->dir, job->style, job->width, job->height);
  ::EnterCriticalSection(&job->mailbox->lock);
  job->mailbox->done.emplace_back(job->key, std::move(material));
  ::LeaveCriticalSection(&job->mailbox->lock);
  if (job->notify_hwnd && ::IsWindow(job->notify_hwnd)) {
    ::PostMessageW(job->notify_hwnd, job->notify_msg, 0, 0);
  }
  if (SUCCEEDED(com)) {
    ::CoUninitialize();
  }
  return 0;
}

void PhiHolographicCard::PumpMailbox() {
  if (!mailbox_) {
    return;
  }
  std::vector<std::pair<std::string, std::shared_ptr<const Material>>> done;
  ::EnterCriticalSection(&mailbox_->lock);
  done.swap(mailbox_->done);
  ::LeaveCriticalSection(&mailbox_->lock);
  for (auto& [key, material] : done) {
    materials_[key] = std::move(material);
    pending_.erase(key);
  }
}

std::shared_ptr<const PhiHolographicCard::Material> PhiHolographicCard::MaterialFor(
    const PhiHolographicCardStyle& style, int width, int height, bool start_job) {
  PumpMailbox();
  const std::string key = MaterialKey(style, width, height);
  const auto found = materials_.find(key);
  if (found != materials_.end()) {
    return found->second;
  }
  if (!start_job || pending_.count(key)) {
    return nullptr;
  }
  if (!PhiEnsureGdiPlus()) {
    LOG(ERROR) << "[HoloCard] GDI+ unavailable; the card stays hidden";
    return nullptr;
  }
  if (!mailbox_) {
    mailbox_ = std::make_shared<Mailbox>();
  }
  auto* job = new Job();
  job->mailbox = mailbox_;
  job->key = key;
  job->style = style;
  job->width = width;
  job->height = height;
  job->dir = cache_dir_.empty() ? std::wstring() : cache_dir_ + L"\\" + kCacheVersion;
  job->notify_hwnd = notify_hwnd_;
  job->notify_msg = notify_msg_;
  HANDLE thread = ::CreateThread(nullptr, 0, &PhiHolographicCard::MaterialThread, job, 0, nullptr);
  if (!thread) {
    delete job;
    return nullptr;
  }
  ::CloseHandle(thread);
  pending_[key] = 1;
  return nullptr;
}

// --- Static layers ------------------------------------------------------------

// The light-independent layers for one (size, style, copy): the palette's
// base gradient, the top layer (tear line, logo ink, copy, edge hairline),
// the rounded-corner coverage, the logo's coverage (for the sweep) and the
// shadow.
struct PhiHolographicCard::Statics {
  int width = 0;
  int height = 0;
  double scale = 1.0;
  int margin = 0;
  Style style;
  std::string name, member_id, joined;
  std::vector<uint8_t> base;       // Opaque BGRA.
  std::vector<uint8_t> top_front;  // Premultiplied BGRA.
  std::vector<uint8_t> top_back;   // Premultiplied BGRA.
  std::vector<uint8_t> corner;     // Coverage.
  std::vector<uint8_t> logo;       // Coverage.
  RECT logo_box = {0, 0, 0, 0};
  std::vector<uint8_t> shadow;     // Premultiplied BGRA, (w + 2m) x (h + 2m).
};

struct PhiHolographicCard::Light {
  double x = 0;
  double y = 0;
  double facing = 1;
};

struct PhiHolographicCard::Motion {
  double elapsed = 0;
  double px = 0, py = 0;
  double Entrance() const {
    return 1 - std::pow(1 - std::min(1.0, elapsed / kEntranceSeconds), 3.0);
  }
  void FinishEntrance() { elapsed = std::max(elapsed, kEntranceSeconds); }
  void Advance(double delta, const std::optional<std::pair<double, double>>& pointer) {
    if (!std::isfinite(delta) || delta <= 0) {
      return;
    }
    elapsed += delta;
    const double tx = pointer ? pointer->first : 0.0;
    const double ty = pointer ? pointer->second : 0.0;
    // The reference's 60 Hz response at any tick rate.
    const double smoothing = 1 - std::pow(1 - 0.075, delta * 60);
    px += (tx - px) * smoothing;
    py += (ty - py) * smoothing;
    if (!pointer && std::abs(px) < 0.0001 && std::abs(py) < 0.0001) {
      px = py = 0;
    }
  }
  bool Needs(bool has_pointer) const {
    return Entrance() < 1 || has_pointer || px != 0 || py != 0;
  }
};

namespace {

std::vector<Stop> BaseStops(Style::Palette palette, bool back) {
  if (back) {
    return {{0, Hex(0x08090b)}, {0.42, Hex(0x171a1e)}, {1, Hex(0x050607)}};
  }
  if (palette == Style::Palette::kSilver) {
    return {{0, Hex(0x898e95)},    {0.22, Hex(0xd0d3d7)}, {0.36, Hex(0xf1f2f3)},
            {0.5, Hex(0xb2b7be)},  {0.68, Hex(0xdadddf)}, {0.86, Hex(0x9299a2)},
            {1, Hex(0xe0e3e6)}};
  }
  const auto& c = kPaletteColors[static_cast<size_t>(palette)];
  return {{0, Hex(c[1])}, {0.43, Hex(c[0])}, {1, Hex(c[2])}};
}

std::vector<Stop> IridescenceStops(Style::Palette palette) {
  if (palette == Style::Palette::kSilver) {
    return {{0, Hex(0x929ba9)},    {0.25, Hex(0xebedef)}, {0.45, Hex(0xbbc2c9)},
            {0.58, Hex(0xf5f5f4)}, {0.78, Hex(0xa8a5ad)}, {1, Hex(0xcdd4d8)}};
  }
  const auto& c = kPaletteColors[static_cast<size_t>(palette)];
  return {{0, Hex(c[1])}, {0.4, Hex(c[0])}, {0.76, Hex(c[2])}, {1, Hex(c[1])}};
}

const Lut& SheenLut() {
  static const Lut lut = MakeLut({{0.1, Hex(0xffffff), 0},
                                  {0.24, Hex(0xffffff), 0},
                                  {0.39, Hex(0xffffff), 136 / 255.0},
                                  {0.49, Hex(0xffffff), 17 / 255.0},
                                  {0.66, Hex(0x19224d), 74 / 255.0},
                                  {0.85, Hex(0x000000), 0}});
  return lut;
}

const Lut& SweepLut() {
  static const Lut lut = [] {
    constexpr std::array<double, 14> alphas = {0.0,  0.015, 0.06, 0.18, 0.44, 0.78, 0.94,
                                               0.94, 0.78,  0.44, 0.18, 0.06, 0.015, 0.0};
    constexpr std::array<double, 14> positions = {0.2,  0.25,  0.29,  0.32, 0.345, 0.365, 0.38,
                                                  0.4,  0.415, 0.435, 0.46, 0.49,  0.53,  0.58};
    std::vector<Stop> stops;
    for (size_t i = 0; i < alphas.size(); ++i) {
      stops.push_back({positions[i], Hex(0xffffff), alphas[i]});
    }
    return MakeLut(stops);
  }();
  return lut;
}

const Lut& GrooveLut() {
  static const Lut lut = MakeLut({{0, Hex(0x25303d)},
                                  {0.38, Hex(0x151e29)},
                                  {0.56, Hex(0x56616d)},
                                  {0.78, Hex(0x1c2734)},
                                  {1, Hex(0x455260)}});
  return lut;
}

const Lut& ReflectionLut(bool enhanced) {
  static const Lut plain = MakeLut({{0, Hex(0x707b87)},
                                    {0.3, Hex(0x8795a2)},
                                    {0.42, Hex(0xd3edf7)},
                                    {0.49, Hex(0xffffff)},
                                    {0.54, Hex(0xffffff)},
                                    {0.63, Hex(0xb6d1df)},
                                    {0.76, Hex(0x768391)},
                                    {1, Hex(0x929daa)}});
  static const Lut strong = MakeLut({{0, Hex(0x596572)},
                                     {0.3, Hex(0x6f7e8c)},
                                     {0.42, Hex(0xd3edf7)},
                                     {0.49, Hex(0xffffff)},
                                     {0.54, Hex(0xffffff)},
                                     {0.63, Hex(0xb6d1df)},
                                     {0.76, Hex(0x5e6c7b)},
                                     {1, Hex(0x788695)}});
  return enhanced ? strong : plain;
}

// Fills `out` (opaque BGRA) with the linear gradient `grad` sampled from
// `lut`, in dips through `scale`.
void FillGradient(std::vector<uint8_t>* out, int width, int height, double scale,
                  const Grad& grad, const Lut& lut) {
  for (int y = 0; y < height; ++y) {
    const double uy = (y + 0.5) / scale;
    for (int x = 0; x < width; ++x) {
      const double t = grad.At((x + 0.5) / scale, uy);
      const uint32_t c = lut[static_cast<size_t>(std::clamp(static_cast<int>(t * 255 + 0.5), 0, 255))];
      const size_t p = (static_cast<size_t>(y) * width + x) * 4;
      (*out)[p] = static_cast<uint8_t>(c & 255);
      (*out)[p + 1] = static_cast<uint8_t>((c >> 8) & 255);
      (*out)[p + 2] = static_cast<uint8_t>((c >> 16) & 255);
      (*out)[p + 3] = 255;
    }
  }
}

void AddRoundRect(Gdiplus::GraphicsPath* path, float x, float y, float w, float h, float r) {
  const float d = 2 * r;
  path->StartFigure();
  path->AddArc(x, y, d, d, 180, 90);
  path->AddArc(x + w - d, y, d, d, 270, 90);
  path->AddArc(x + w - d, y + h - d, d, d, 0, 90);
  path->AddArc(x, y + h - d, d, d, 90, 90);
  path->CloseFigure();
}

// A horizontal-then-vertical box blur of radius `r` on a coverage plane.
void BoxBlur(std::vector<uint8_t>* plane, int width, int height, int r) {
  if (r <= 0) {
    return;
  }
  std::vector<uint8_t> tmp(plane->size());
  const int window = 2 * r + 1;
  for (int y = 0; y < height; ++y) {
    int sum = 0;
    for (int x = -r; x <= r; ++x) {
      sum += (*plane)[static_cast<size_t>(y) * width + std::clamp(x, 0, width - 1)];
    }
    for (int x = 0; x < width; ++x) {
      tmp[static_cast<size_t>(y) * width + x] = static_cast<uint8_t>(sum / window);
      sum -= (*plane)[static_cast<size_t>(y) * width + std::clamp(x - r, 0, width - 1)];
      sum += (*plane)[static_cast<size_t>(y) * width + std::clamp(x + r + 1, 0, width - 1)];
    }
  }
  for (int x = 0; x < width; ++x) {
    int sum = 0;
    for (int y = -r; y <= r; ++y) {
      sum += tmp[static_cast<size_t>(std::clamp(y, 0, height - 1)) * width + x];
    }
    for (int y = 0; y < height; ++y) {
      (*plane)[static_cast<size_t>(y) * width + x] = static_cast<uint8_t>(sum / window);
      sum -= tmp[static_cast<size_t>(std::clamp(y - r, 0, height - 1)) * width + x];
      sum += tmp[static_cast<size_t>(std::clamp(y + r + 1, 0, height - 1)) * width + x];
    }
  }
}

}  // namespace

void PhiHolographicCard::EnsureStatics(int width, int height, const PhiHolographicCardStyle& style) {
  if (statics_ && statics_->width == width && statics_->height == height &&
      statics_->style == style && statics_->name == name_ &&
      statics_->member_id == member_id_ && statics_->joined == joined_) {
    return;
  }
  auto st = std::make_unique<Statics>();
  st->width = width;
  st->height = height;
  st->scale = width / kDesignW;
  st->margin = MarginPx({0, 0, width, height});
  st->style = style;
  st->name = name_;
  st->member_id = member_id_;
  st->joined = joined_;
  const double s = st->scale;
  const double design_h = height / s;
  const size_t count = static_cast<size_t>(width) * height;
  const bool silver = style.palette == Style::Palette::kSilver;

  st->base.assign(count * 4, 255);
  FillGradient(&st->base, width, height, s,
               MakeGrad(kGradientAngle, 1, false, 0, 0, kDesignW, design_h),
               MakeLut(BaseStops(style.palette, false)));

  // The corner coverage: 255 inside, the rounded rect's edge ramp at the
  // corners.
  st->corner.assign(count, 255);
  const double radius = kCorner * s;
  const int span = static_cast<int>(std::ceil(radius)) + 1;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      if (x >= span && x < width - span && y >= span && y < height - span) {
        continue;
      }
      const double c = PhiRoundRectCoverage(x + 0.5, y + 0.5, width, height, radius);
      st->corner[static_cast<size_t>(y) * width + x] =
          static_cast<uint8_t>(std::lround(std::clamp(c, 0.0, 1.0) * 255));
    }
  }

  st->top_front.assign(count * 4, 0);
  st->top_back.assign(count * 4, 0);
  st->logo.assign(count, 0);
  if (PhiEnsureGdiPlus()) {
    // The logo: its ink into the front, its coverage for the sweep.
    Gdiplus::GraphicsPath logo;
    AddPathCmds(&logo, kLogoPath);
    Gdiplus::Matrix place;
    place.Translate(static_cast<float>(kDesignW * 0.064711 * s),
                    static_cast<float>(design_h * 0.06 * s));
    const float k = static_cast<float>(kDesignW * 0.1376 / 960 * s);
    place.Scale(k, k);
    logo.Transform(&place);
    {
      std::vector<uint8_t> mask(count * 4, 0);
      {
        Canvas canvas(&mask, width, height);
        Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
        canvas.graphics().FillPath(&white, &logo);
      }
      RECT box = {width, height, 0, 0};
      for (size_t i = 0; i < count; ++i) {
        st->logo[i] = mask[i * 4 + 3];
        if (st->logo[i]) {
          const LONG x = static_cast<LONG>(i % static_cast<size_t>(width));
          const LONG y = static_cast<LONG>(i / static_cast<size_t>(width));
          box.left = std::min(box.left, x);
          box.top = std::min(box.top, y);
          box.right = std::max(box.right, x + 1);
          box.bottom = std::max(box.bottom, y + 1);
        }
      }
      st->logo_box = box.right > box.left ? box : RECT{0, 0, 0, 0};
    }
    {
      Canvas canvas(&st->top_front, width, height);
      Gdiplus::Graphics& g = canvas.graphics();
      Gdiplus::SolidBrush ink(Gdiplus::Color(255, 0, 0, 0));
      g.FillPath(&ink, &logo);
      // The tear line: dashed [2, 2], 0.7 wide, black 15%, at 69% down.
      Gdiplus::Pen tear(Argb(0x000000, 0.15), static_cast<float>(0.7 * s));
      const std::array<float, 2> dashes = {2.0f / 0.7f, 2.0f / 0.7f};
      tear.SetDashPattern(dashes.data(), 2);
      const float ty = static_cast<float>(design_h * 0.69 * s);
      g.DrawLine(&tear, static_cast<float>(kDesignW * 0.05 * s), ty,
                 static_cast<float>(kDesignW * 0.95 * s), ty);
    }
    for (int face = 0; face < 2; ++face) {
      Canvas canvas(face == 0 ? &st->top_front : &st->top_back, width, height);
      Gdiplus::GraphicsPath edge;
      const float inset = static_cast<float>(0.5 * s);
      AddRoundRect(&edge, inset, inset, static_cast<float>(width - s),
                   static_cast<float>(height - s), static_cast<float>(radius - 0.5 * s));
      Gdiplus::Pen pen(Argb(0xffffff, face == 0 ? 0.55 : 0.14), static_cast<float>(s));
      canvas.graphics().DrawPath(&pen, &edge);
    }
  }

  // The copy (front only), in the Mac's frames; the name in the brand face
  // when it has the glyphs.
  {
    const Rgb ink = Hex(silver ? 0x303640 : 0x233338);
    const auto draw = [&](const std::wstring& text, std::wstring_view face, int weight,
                          double size_dip, double x, double y, double w, double h,
                          double alpha) {
      if (text.empty()) {
        return;
      }
      HFONT font = MakeFont(face, static_cast<int>(std::lround(size_dip * s)), weight);
      if (!font) {
        return;
      }
      const int pw = static_cast<int>(std::lround(w * s));
      const int ph = static_cast<int>(std::lround(h * s * 1.3));  // Room for descenders.
      const std::vector<uint8_t> coverage = TextCoverage(text, font, pw, ph);
      ::DeleteObject(font);
      InkOver(&st->top_front, width, height, coverage, pw, ph,
              static_cast<int>(std::lround(x * s)), static_cast<int>(std::lround(y * s)), ink,
              alpha);
    };
    const std::wstring name = base::UTF8ToWide(name_);
    draw(name, PhiBrandFaceRenders(name) ? L"IvyPresto Headline Light" : L"Segoe UI Light",
         FW_LIGHT, 31, 23.36, 304, 247, 42, 1.0);
    draw(base::UTF8ToWide(member_id_), L"Consolas", FW_NORMAL, 10, 23.36, 348, 210, 15, 1.0);
    if (!joined_.empty()) {
      draw(PhiStr(kDateJoinedKey), L"Segoe UI", FW_NORMAL, 7, 23.36, 377, 145, 12, 0.66);
      draw(base::UTF8ToWide(joined_), L"Segoe UI", FW_NORMAL, 11, 23.36, 390, 155, 16, 1.0);
    }
  }

  // The shadow: the card's outline offset down and blurred, black at 24%.
  {
    const int m = st->margin;
    const int lw = width + 2 * m, lh = height + 2 * m;
    std::vector<uint8_t> plane(static_cast<size_t>(lw) * lh, 0);
    const int dy = static_cast<int>(std::lround(kShadowOffsetY * s));
    for (int y = 0; y < height; ++y) {
      const int ly = y + m + dy;
      if (ly < 0 || ly >= lh) {
        continue;
      }
      for (int x = 0; x < width; ++x) {
        plane[static_cast<size_t>(ly) * lw + (x + m)] = st->corner[static_cast<size_t>(y) * width + x];
      }
    }
    const int blur = std::max(1, static_cast<int>(std::lround(kShadowRadius * s / 2)));
    BoxBlur(&plane, lw, lh, blur);
    BoxBlur(&plane, lw, lh, blur);
    st->shadow.assign(static_cast<size_t>(lw) * lh * 4, 0);
    for (size_t i = 0; i < plane.size(); ++i) {
      st->shadow[i * 4 + 3] = static_cast<uint8_t>(std::lround(plane[i] * kShadowOpacity));
    }
  }
  statics_ = std::move(st);
  layer_key_.clear();
}

// --- Composition ------------------------------------------------------------

std::vector<uint8_t> PhiHolographicCard::ComposeFace(const Material& material,
                                                     const Light& light, bool back) const {
  const Statics& st = *statics_;
  const int width = st.width, height = st.height;
  const double s = st.scale;
  const double design_h = height / s;
  const size_t count = static_cast<size_t>(width) * height;
  std::vector<uint8_t> out(count * 4, 255);
  const bool silver = st.style.palette == Style::Palette::kSilver;
  const std::vector<uint8_t>& ov = OverlayTable();
  const std::vector<uint8_t>& sl = SoftLightTable();
  const double light_angle = 125 + light.x * 24 - light.y * 20;
  const auto index = [](double t) {
    return static_cast<size_t>(std::clamp(static_cast<int>(t * 255 + 0.5), 0, 255));
  };

  if (back) {
    // The reverse: the dark gradient at the light angle, the foil at soft
    // light, the sheen screened, the grain.
    FillGradient(&out, width, height, s,
                 MakeGrad(light_angle, 1, false, 0, 0, kDesignW, design_h),
                 MakeLut(BaseStops(st.style.palette, true)));
    const Grad sheen = MakeGrad(light_angle, 2.2, true, light.x, light.y, kDesignW, design_h);
    const Lut& sheen_lut = SheenLut();
    const int foil_a = static_cast<int>(std::lround(kTextureOpacity * 0.85 * 255));
    for (int y = 0; y < height; ++y) {
      const double uy = (y + 0.5) / s;
      for (int x = 0; x < width; ++x) {
        const double ux = (x + 0.5) / s;
        const size_t p = (static_cast<size_t>(y) * width + x) * 4;
        const uint32_t sc = sheen_lut[index(sheen.At(ux, uy))];
        const int sa = Mul255(static_cast<int>(sc >> 24), 82);  // .32
        for (size_t c = 0; c < 3; ++c) {
          int v = out[p + c];
          v = Lerp255(v, sl[static_cast<size_t>(v) * 256 + material.foil[p + c]], foil_a);
          if (sa) {
            const int src = static_cast<int>((sc >> (8 * c)) & 255);
            v = Lerp255(v, v + src - Mul255(v, src), sa);
          }
          v = Mul255(v, material.grain_factor[p + c]);
          out[p + c] = static_cast<uint8_t>(std::clamp(v, 0, 255));
        }
      }
    }
  } else {
    const Grad irid = MakeGrad(kGradientAngle + (silver ? 0 : 38) + light.x * 32 + light.y * 22,
                               silver ? 2 : 1.25, true, light.x, light.y, kDesignW, design_h);
    const Lut irid_lut = MakeLut(IridescenceStops(st.style.palette));
    const Grad sheen = MakeGrad(light_angle, 2.2, true, light.x, light.y, kDesignW, design_h);
    const Lut& sheen_lut = SheenLut();
    const int sheen_a = static_cast<int>(std::lround((silver ? 0.6 : 0.95) * 255));
    const int irid_a = static_cast<int>(std::lround(kIridescenceOpacity * 255));
    const int foil_a = static_cast<int>(std::lround(kTextureOpacity * 255));
    // The radial highlight (not for silver): white 85/255 at its centre to
    // nothing at 52% of an ellipse spanning 1.25 of the card, both scaled by
    // the group's opacity.
    const double mx = 0.5 + light.x * 0.35, my = 0.5 + light.y * 0.35;
    const double rcx = mx * kDesignW, rcy = my * design_h;
    const double rx = std::max(std::abs(mx), std::abs(1 - mx)) * kDesignW * 1.25 * std::sqrt(2.0);
    const double ry = std::max(std::abs(my), std::abs(1 - my)) * design_h * 1.25 * std::sqrt(2.0);
    std::array<uint8_t, 1024> radial{};
    for (size_t i = 0; i < radial.size(); ++i) {
      const double q = i / 1024.0 * 0.2704;
      const double t = std::sqrt(q);
      radial[i] = static_cast<uint8_t>(std::lround(
          std::max(0.0, 1 - t / 0.52) * 85 / 255.0 * kIridescenceOpacity * 255));
    }
    for (int y = 0; y < height; ++y) {
      const double uy = (y + 0.5) / s;
      const double dyr = (uy - rcy) / ry;
      for (int x = 0; x < width; ++x) {
        const double ux = (x + 0.5) / s;
        const size_t p = (static_cast<size_t>(y) * width + x) * 4;
        const uint32_t ic = irid_lut[index(irid.At(ux, uy))];
        int ra = 0;
        if (!silver) {
          const double dxr = (ux - rcx) / rx;
          const double q = dxr * dxr + dyr * dyr;
          if (q < 0.2704) {
            ra = radial[static_cast<size_t>(q / 0.2704 * 1023)];
          }
        }
        const uint32_t sc = sheen_lut[index(sheen.At(ux, uy))];
        const int sa = Mul255(static_cast<int>(sc >> 24), sheen_a);
        for (size_t c = 0; c < 3; ++c) {
          int v = st.base[p + c];
          const int ir = static_cast<int>((ic >> (8 * c)) & 255);
          v = silver ? Lerp255(v, sl[static_cast<size_t>(v) * 256 + ir], irid_a)
                     : Lerp255(v, ir, irid_a);
          if (ra) {
            v = Lerp255(v, 255, ra);
          }
          v = Lerp255(v, ov[static_cast<size_t>(v) * 256 + material.foil[p + c]], foil_a);
          if (sa) {
            const int src = static_cast<int>((sc >> (8 * c)) & 255);
            const int blended = silver ? v + src - Mul255(v, src)
                                       : ov[static_cast<size_t>(v) * 256 + src];
            v = Lerp255(v, blended, sa);
          }
          v = Mul255(v, material.grain_factor[p + c]);
          out[p + c] = static_cast<uint8_t>(std::clamp(v, 0, 255));
        }
      }
    }
    // The seal: its groove and reflection gradients, hard light through
    // their stroked masks.
    const RECT& sb = material.seal_box;
    if (sb.right > sb.left) {
      const SealPlace place = MakeSealPlace(kDesignW, design_h);
      const double rotation = (light_angle - 135 + 12) * kPi / 180;
      const Grad grad = MakeSealGrad(place, rotation, light.x, light.y);
      const bool enhanced = st.style.enhanced_ornament();
      const Lut& groove = GrooveLut();
      const Lut& reflect = ReflectionLut(enhanced);
      const int groove_a = static_cast<int>(std::lround((enhanced ? 0.95 : 0.7) * 255));
      for (int y = std::max(0, static_cast<int>(sb.top)); y < std::min(height, static_cast<int>(sb.bottom)); ++y) {
        const double uy = (y + 0.5) / s;
        for (int x = std::max(0, static_cast<int>(sb.left)); x < std::min(width, static_cast<int>(sb.right)); ++x) {
          const size_t i = static_cast<size_t>(y) * width + x;
          const int cg = material.seal_groove[i], cr = material.seal_reflect[i];
          if (!cg && !cr) {
            continue;
          }
          const size_t ti = index(grad.At((x + 0.5) / s, uy));
          const uint32_t gc = groove[ti], rc = reflect[ti];
          const size_t p = i * 4;
          for (size_t c = 0; c < 3; ++c) {
            int v = out[p + c];
            if (cg) {
              const int src = static_cast<int>((gc >> (8 * c)) & 255);
              v = Lerp255(v, ov[static_cast<size_t>(src) * 256 + v], Mul255(cg, groove_a));
            }
            if (cr) {
              const int src = static_cast<int>((rc >> (8 * c)) & 255);
              v = Lerp255(v, ov[static_cast<size_t>(src) * 256 + v], cr);
            }
            out[p + c] = static_cast<uint8_t>(std::clamp(v, 0, 255));
          }
        }
      }
    }
  }
  // The top layer over, then (front) the logo's sweep.
  const std::vector<uint8_t>& top = back ? st.top_back : st.top_front;
  for (size_t p = 0; p < count * 4; p += 4) {
    const int ta = top[p + 3];
    if (!ta) {
      continue;
    }
    const int inv = 255 - ta;
    for (size_t c = 0; c < 3; ++c) {
      out[p + c] = static_cast<uint8_t>(std::min(255, top[p + c] + Mul255(out[p + c], inv)));
    }
  }
  if (!back && st.logo_box.right > st.logo_box.left) {
    const Grad sweep = MakeGrad(light_angle, 2.2, true, light.x, light.y, kDesignW, design_h);
    const Lut& lut = SweepLut();
    for (int y = st.logo_box.top; y < st.logo_box.bottom; ++y) {
      const double uy = (y + 0.5) / s;
      for (int x = st.logo_box.left; x < st.logo_box.right; ++x) {
        const size_t i = static_cast<size_t>(y) * width + x;
        const int cov = st.logo[i];
        if (!cov) {
          continue;
        }
        const int a = Mul255(static_cast<int>(lut[index(sweep.At((x + 0.5) / s, uy))] >> 24), cov);
        if (!a) {
          continue;
        }
        for (size_t c = 0; c < 3; ++c) {
          out[i * 4 + c] = static_cast<uint8_t>(Lerp255(out[i * 4 + c], 255, a));
        }
      }
    }
  }
  // The shade (the face turning away) and the rounded corners, premultiplied.
  const int shade = static_cast<int>(std::lround((1 - 0.34 * (1 - std::max(0.0, light.facing))) * 255));
  for (size_t i = 0; i < count; ++i) {
    const int cov = st.corner[i];
    const size_t p = i * 4;
    for (size_t c = 0; c < 3; ++c) {
      int v = out[p + c];
      if (shade < 255) {
        v = Mul255(v, shade);
      }
      out[p + c] = static_cast<uint8_t>(cov == 255 ? v : Mul255(v, cov));
    }
    out[p + 3] = static_cast<uint8_t>(cov);
  }
  return out;
}

namespace {

// Samples a premultiplied BGRA image bilinearly at (fx, fy) in pixel
// coordinates (pixel centres at .5); transparent outside.
inline std::array<int, 4> SampleBilinear(const std::vector<uint8_t>& image, int width,
                                         int height, double fx, double fy) {
  std::array<int, 4> out{};
  fx -= 0.5;
  fy -= 0.5;
  const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
  const double tx = fx - x0, ty = fy - y0;
  const std::array<double, 4> weights = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
  for (int k = 0; k < 4; ++k) {
    const int x = x0 + (k & 1), y = y0 + (k >> 1);
    if (x < 0 || y < 0 || x >= width || y >= height) {
      continue;
    }
    const size_t p = (static_cast<size_t>(y) * width + x) * 4;
    const double w = weights[static_cast<size_t>(k)];
    for (size_t c = 0; c < 4; ++c) {
      out[c] += static_cast<int>(image[p + c] * w + 0.5);
    }
  }
  for (int& v : out) {
    v = std::min(255, v);
  }
  return out;
}

// The projective map of the unit square onto `q` (Heckbert), as the 3x3
// matrix of (x, y, w) = H * (u, v, 1).
std::array<double, 9> SquareToQuad(const std::array<std::pair<double, double>, 4>& q) {
  const double x0 = q[0].first, y0 = q[0].second, x1 = q[1].first, y1 = q[1].second;
  const double x2 = q[2].first, y2 = q[2].second, x3 = q[3].first, y3 = q[3].second;
  const double sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
  std::array<double, 9> h{};
  if (std::abs(sx) < 1e-9 && std::abs(sy) < 1e-9) {
    h = {x1 - x0, x3 - x0, x0, y1 - y0, y3 - y0, y0, 0, 0, 1};
    return h;
  }
  const double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
  const double den = dx1 * dy2 - dx2 * dy1;
  const double g = (sx * dy2 - dx2 * sy) / den;
  const double hh = (dx1 * sy - sx * dy1) / den;
  h = {x1 - x0 + g * x1, x3 - x0 + hh * x3, x0, y1 - y0 + g * y1, y3 - y0 + hh * y3, y0, g, hh, 1};
  return h;
}

std::array<double, 9> Invert3(const std::array<double, 9>& m) {
  const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7],
               i = m[8];
  const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  const double inv = std::abs(det) < 1e-12 ? 0 : 1 / det;
  return {(e * i - f * h) * inv, (c * h - b * i) * inv, (b * f - c * e) * inv,
          (f * g - d * i) * inv, (a * i - c * g) * inv, (c * d - a * f) * inv,
          (d * h - e * g) * inv, (b * g - a * h) * inv, (a * e - b * d) * inv};
}

}  // namespace

std::vector<uint8_t> PhiHolographicCard::ComposeLayer(const Material& material, int margin) {
  const Statics& st = *statics_;
  const int width = st.width, height = st.height;
  const double s = st.scale;
  const int lw = width + 2 * margin, lh = height + 2 * margin;
  const bool reduced = ReducedMotion();
  const double entrance = reduced ? 1.0 : motion_->Entrance();
  const double x = reduced ? 0.0 : motion_->px;
  const double y = reduced ? 0.0 : motion_->py;
  const double spin = -2 * kPi * (1 - entrance);
  const double opacity = reduced ? 1.0 : std::min(1.0, motion_->elapsed / (kEntranceSeconds * kFadeFraction));

  // The shadow, fading in with the entrance.
  std::vector<uint8_t> layer = st.shadow;
  if (entrance < 1) {
    for (size_t i = 3; i < layer.size(); i += 4) {
      layer[i] = static_cast<uint8_t>(std::lround(layer[i] * entrance * opacity));
    }
  }

  const bool flat = entrance >= 1 && std::abs(x) < 1e-5 && std::abs(y) < 1e-5;
  if (flat) {
    const std::vector<uint8_t> face = ComposeFace(material, Light{0, 0, 1}, false);
    for (int yy = 0; yy < height; ++yy) {
      for (int xx = 0; xx < width; ++xx) {
        const size_t src = (static_cast<size_t>(yy) * width + xx) * 4;
        const size_t dst = (static_cast<size_t>(yy + margin) * lw + (xx + margin)) * 4;
        const int a = face[src + 3];
        const int inv = 255 - a;
        for (size_t c = 0; c < 4; ++c) {
          layer[dst + c] = static_cast<uint8_t>(std::min(255, face[src + c] + Mul255(layer[dst + c], inv)));
        }
      }
    }
    return layer;
  }

  // The Mac's rotor: scale (0.16 -> 1), then rotations about x (tilt from
  // the pointer's y), z (a little lean) and y (the pointer's x plus the
  // entrance spin), under a perspective of 1/1100.
  const double sigma = 0.16 + 0.84 * entrance;
  const double ax = -y * kTiltX * kPi / 180 * entrance;
  const double az = x * kTiltZ * kPi / 180 * entrance;
  const double ay = x * kTiltY * kPi / 180 * entrance + spin;
  const auto project = [&](double px, double py) {
    double X = px, Y = py, Z = 0;
    // About y.
    double nx = X * std::cos(ay) + Z * std::sin(ay), nz = -X * std::sin(ay) + Z * std::cos(ay);
    X = nx;
    Z = nz;
    // About z.
    nx = X * std::cos(az) - Y * std::sin(az);
    const double ny = X * std::sin(az) + Y * std::cos(az);
    X = nx;
    Y = ny;
    // About x.
    const double ny2 = Y * std::cos(ax) - Z * std::sin(ax);
    nz = Y * std::sin(ax) + Z * std::cos(ax);
    Y = ny2;
    Z = nz;
    X *= sigma;
    Y *= sigma;
    Z *= sigma;
    const double w = std::max(0.05, 1 - Z / kFocal);
    return std::make_pair(X / w * s + lw / 2.0, Y / w * s + lh / 2.0);
  };
  const double hw = kDesignW / 2, hh = (height / s) / 2;
  const std::array<std::pair<double, double>, 4> quad = {
      project(-hw, -hh), project(hw, -hh), project(hw, hh), project(-hw, hh)};
  const double cross = (quad[1].first - quad[0].first) * (quad[3].second - quad[0].second) -
                       (quad[1].second - quad[0].second) * (quad[3].first - quad[0].first);
  const bool back = cross < 0;
  Light light;
  const double face_spin = back ? spin + kPi : spin;
  light.x = x + 1.45 * std::sin(face_spin);
  light.y = y + 0.25 * (1 - std::cos(face_spin));
  light.facing = std::cos(face_spin);
  const std::vector<uint8_t> face = ComposeFace(material, light, back);
  const std::array<double, 9> inverse = Invert3(SquareToQuad(quad));
  double minx = lw, miny = lh, maxx = 0, maxy = 0;
  for (const auto& [qx, qy] : quad) {
    minx = std::min(minx, qx);
    maxx = std::max(maxx, qx);
    miny = std::min(miny, qy);
    maxy = std::max(maxy, qy);
  }
  const int x0 = std::max(0, static_cast<int>(std::floor(minx)) - 1);
  const int x1 = std::min(lw, static_cast<int>(std::ceil(maxx)) + 1);
  const int y0 = std::max(0, static_cast<int>(std::floor(miny)) - 1);
  const int y1 = std::min(lh, static_cast<int>(std::ceil(maxy)) + 1);
  const int alpha = static_cast<int>(std::lround(std::clamp(opacity, 0.0, 1.0) * 255));
  for (int yy = y0; yy < y1; ++yy) {
    for (int xx = x0; xx < x1; ++xx) {
      const double X = xx + 0.5, Y = yy + 0.5;
      const double w = inverse[6] * X + inverse[7] * Y + inverse[8];
      if (std::abs(w) < 1e-9) {
        continue;
      }
      double u = (inverse[0] * X + inverse[1] * Y + inverse[2]) / w;
      const double v = (inverse[3] * X + inverse[4] * Y + inverse[5]) / w;
      if (u < -0.002 || u > 1.002 || v < -0.002 || v > 1.002) {
        continue;
      }
      if (back) {
        u = 1 - u;  // The reverse is seen mirrored.
      }
      std::array<int, 4> px = SampleBilinear(face, width, height, u * width, v * height);
      if (!px[3]) {
        continue;
      }
      const size_t dst = (static_cast<size_t>(yy) * lw + xx) * 4;
      const int a = Mul255(px[3], alpha);
      const int inv = 255 - a;
      for (size_t c = 0; c < 4; ++c) {
        layer[dst + c] = static_cast<uint8_t>(
            std::min(255, Mul255(px[c], alpha) + Mul255(layer[dst + c], inv)));
      }
    }
  }
  return layer;
}

// --- The card -------------------------------------------------------------------

PhiHolographicCard::PhiHolographicCard() : motion_(std::make_unique<Motion>()) {}

PhiHolographicCard::~PhiHolographicCard() = default;

void PhiHolographicCard::SetNotify(HWND hwnd, UINT msg) {
  notify_hwnd_ = hwnd;
  notify_msg_ = msg;
}

void PhiHolographicCard::SetCacheDir(const std::wstring& dir) {
  cache_dir_ = dir;
}

void PhiHolographicCard::SetIdentity(const std::string& name, const std::string& member_id,
                                     const std::string& joined) {
  name_ = name;
  member_id_ = member_id;
  joined_ = joined;
}

void PhiHolographicCard::SetStyle(const PhiHolographicCardStyle& style) {
  if (style == style_) {
    return;
  }
  style_ = style;
  if (width_ > 0 && height_ > 0) {
    MaterialFor(style_, width_, height_, /*start_job=*/true);
  }
}

void PhiHolographicCard::Prepare(const RECT& card_rect) {
  const int width = card_rect.right - card_rect.left;
  const int height = card_rect.bottom - card_rect.top;
  if (width <= 0 || height <= 0) {
    return;
  }
  width_ = width;
  height_ = height;
  MaterialFor(style_, width_, height_, /*start_job=*/true);
}

bool PhiHolographicCard::ready() const {
  return width_ > 0 && materials_.count(MaterialKey(style_, width_, height_)) > 0;
}

void PhiHolographicCard::SetPointer(const std::optional<POINT>& client_pt, const RECT& card_rect) {
  const int width = card_rect.right - card_rect.left;
  const int height = card_rect.bottom - card_rect.top;
  if (!client_pt || width <= 0 || height <= 0) {
    pointer_.reset();
    return;
  }
  pointer_ = std::make_pair(
      std::clamp((client_pt->x - card_rect.left) / static_cast<double>(width) * 2 - 1, -1.0, 1.0),
      std::clamp((client_pt->y - card_rect.top) / static_cast<double>(height) * 2 - 1, -1.0, 1.0));
}

void PhiHolographicCard::ReplayEntrance() {
  motion_ = std::make_unique<Motion>();
  last_tick_ = 0;
}

bool PhiHolographicCard::ReducedMotion() const {
  BOOL animations = TRUE;
  if (!::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0)) {
    return false;
  }
  return !animations;
}

bool PhiHolographicCard::Tick() {
  PumpMailbox();
  const uint64_t now = NowMicros();
  const double delta =
      last_tick_ ? std::min(1.0 / 15, std::max(0.0, (now - last_tick_) / 1e6)) : 0.0;
  last_tick_ = now;
  if (ReducedMotion()) {
    motion_->FinishEntrance();
    pointer_.reset();
  }
  motion_->Advance(delta, pointer_);
  return true;
}

bool PhiHolographicCard::animating() const {
  return ready() && !ReducedMotion() && motion_->Needs(pointer_.has_value());
}

// static
int PhiHolographicCard::MarginPx(const RECT& card_rect) {
  const int width = card_rect.right - card_rect.left;
  return static_cast<int>(std::lround(kMarginDip * width / kDesignW));
}

void PhiHolographicCard::Paint(HDC dc, const RECT& card_rect) {
  const int width = card_rect.right - card_rect.left;
  const int height = card_rect.bottom - card_rect.top;
  if (width <= 0 || height <= 0) {
    return;
  }
  Prepare(card_rect);
  std::shared_ptr<const Material> material = MaterialFor(style_, width, height, true);
  PhiHolographicCardStyle style = style_;
  if (!material && shown_style_) {
    // The new style's materials are still being made: keep the old card.
    material = MaterialFor(*shown_style_, width, height, false);
    style = *shown_style_;
  }
  if (!material) {
    return;  // Hidden until the artwork is ready.
  }
  shown_style_ = style;
  EnsureStatics(width, height, style);
  const int margin = MarginPx(card_rect);
  const bool reduced = ReducedMotion();
  const std::string key = base::StringPrintf(
      "%.4f|%.4f|%.4f|%.3f|%d", reduced ? 0.0 : motion_->px, reduced ? 0.0 : motion_->py,
      reduced ? 1.0 : motion_->Entrance(), reduced ? 1.0 : motion_->elapsed, margin);
  if (layer_key_ != key || layer_margin_ != margin) {
    layer_ = ComposeLayer(*material, margin);
    layer_key_ = key;
    layer_margin_ = margin;
  }
  const RECT dest = {card_rect.left - margin, card_rect.top - margin, card_rect.right + margin,
                     card_rect.bottom + margin};
  PhiBlendBgra(dc, dest, layer_.data(), width + 2 * margin, height + 2 * margin);
}

bool PhiHolographicCard::ExportPng(HWND owner, COLORREF canvas) {
  std::wstring file = base::UTF8ToWide(name_.empty() ? "Phi" : name_);
  constexpr std::wstring_view kReserved = L"\\/:*?\"<>|";
  for (wchar_t& c : file) {
    if (kReserved.find(c) != std::wstring_view::npos || c < 0x20) {
      c = L'_';
    }
  }
  file += L".png";
  std::wstring path(MAX_PATH, L'\0');
  std::ranges::copy(file.substr(0, MAX_PATH - 1), path.begin());
  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = L"PNG image (*.png)\0*.png\0\0";
  ofn.lpstrFile = path.data();
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrDefExt = L"png";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!::GetSaveFileNameW(&ofn)) {
    return false;
  }
  path.resize(std::wcslen(path.c_str()));
  if (!PhiEnsureGdiPlus()) {
    return false;
  }
  // The front at 2x, flat light, on a canvas twice its size.
  const int card_w = kDesignWidthDip * 2, card_h = kDesignHeightDip * 2;
  const int canvas_w = card_w * 2, canvas_h = card_h * 2;
  ComPtr<IWICImagingFactory> factory = WicFactory();
  const std::wstring dir = cache_dir_.empty() ? std::wstring() : cache_dir_ + L"\\" + kCacheVersion;
  std::shared_ptr<const Material> material = BuildMaterial(factory.Get(), dir, style_, card_w, card_h);
  if (!material) {
    return false;
  }
  // The statics at the export size stand in for the live card's for the
  // render; the next paint rebuilds its own.
  statics_.reset();
  EnsureStatics(card_w, card_h, style_);
  const std::vector<uint8_t> face = ComposeFace(*material, Light{0, 0, 1}, false);
  statics_.reset();
  layer_key_.clear();
  std::vector<uint8_t> pixels(static_cast<size_t>(canvas_w) * canvas_h * 4, 255);
  for (size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i] = GetBValue(canvas);
    pixels[i + 1] = GetGValue(canvas);
    pixels[i + 2] = GetRValue(canvas);
  }
  const int left = (canvas_w - card_w) / 2, top = (canvas_h - card_h) / 2;
  for (int y = 0; y < card_h; ++y) {
    for (int x = 0; x < card_w; ++x) {
      const size_t src = (static_cast<size_t>(y) * card_w + x) * 4;
      const size_t dst = (static_cast<size_t>(y + top) * canvas_w + (x + left)) * 4;
      const int inv = 255 - face[src + 3];
      for (size_t c = 0; c < 3; ++c) {
        pixels[dst + c] = static_cast<uint8_t>(std::min(255, face[src + c] + Mul255(pixels[dst + c], inv)));
      }
    }
  }
  if (!WritePng(factory.Get(), path, pixels, canvas_w, canvas_h)) {
    LOG(ERROR) << "[HoloCard] export: PNG write failed";
    return false;
  }
  return true;
}

// static
std::string PhiHolographicCard::MemberId(const std::string& sub) {
  if (sub.empty()) {
    return std::string();
  }
  std::array<uint8_t, SHA256_DIGEST_LENGTH> digest{};
  SHA256(reinterpret_cast<const uint8_t*>(sub.data()), sub.size(), digest.data());
  std::string hex;
  constexpr std::string_view kDigits = "0123456789abcdef";
  for (size_t i = 0; i < 8; ++i) {
    hex += kDigits[digest[i] >> 4];
    hex += kDigits[digest[i] & 15];
  }
  return hex;
}

// static
std::string PhiHolographicCard::FormatJoinDate(const std::string& iso) {
  const auto digits = [&](size_t at, size_t count, int* out) {
    int value = 0;
    for (size_t i = at; i < at + count; ++i) {
      if (i >= iso.size() || iso[i] < '0' || iso[i] > '9') {
        return false;
      }
      value = value * 10 + (iso[i] - '0');
    }
    *out = value;
    return true;
  };
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
  if (iso.size() < 19 || iso[4] != '-' || iso[7] != '-' || iso[10] != 'T' || iso[13] != ':' ||
      iso[16] != ':' || !digits(0, 4, &y) || !digits(5, 2, &mo) || !digits(8, 2, &d) ||
      !digits(11, 2, &h) || !digits(14, 2, &mi) || !digits(17, 2, &sec)) {
    return std::string();
  }
  SYSTEMTIME utc = {};
  utc.wYear = static_cast<WORD>(y);
  utc.wMonth = static_cast<WORD>(mo);
  utc.wDay = static_cast<WORD>(d);
  utc.wHour = static_cast<WORD>(h);
  utc.wMinute = static_cast<WORD>(mi);
  utc.wSecond = static_cast<WORD>(sec);
  SYSTEMTIME local = utc;
  if (iso.back() == 'Z' || iso.find('+', 19) != std::string::npos ||
      iso.find('-', 19) != std::string::npos) {
    // Offsets other than Z are rare from the service; treat them as UTC
    // rather than mis-parsing.
    ::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
  }
  return base::StringPrintf("%04d.%02d", local.wYear, local.wMonth);
}

}  // namespace phi_client

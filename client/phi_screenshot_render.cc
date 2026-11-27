// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_screenshot_render.h"

#include <windows.h>

#include <shlwapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "third_party/zlib/zlib.h"

namespace phi_client {

using Microsoft::WRL::ComPtr;

namespace {

D2D1_COLOR_F ColorF(uint32_t argb, float alpha_scale = 1.0f) {
  return D2D1::ColorF(((argb >> 16) & 0xFF) / 255.0f,
                      ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f,
                      ((argb >> 24) & 0xFF) / 255.0f * alpha_scale);
}

D2D1_POINT_2F Pt(PhiPointF p) {
  return D2D1::Point2F(static_cast<float>(p.x), static_cast<float>(p.y));
}

D2D1_RECT_F Rc(const PhiRectF& r) {
  return D2D1::RectF(static_cast<float>(std::min(r.left, r.right)),
                     static_cast<float>(std::min(r.top, r.bottom)),
                     static_cast<float>(std::max(r.left, r.right)),
                     static_cast<float>(std::max(r.top, r.bottom)));
}

ComPtr<IWICImagingFactory> Wic() {
  ComPtr<IWICImagingFactory> factory;
  ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&factory));
  return factory;
}

bool StreamBytes(IStream* stream, std::string* out) {
  ULARGE_INTEGER size = {};
  if (FAILED(::IStream_Size(stream, &size)) || size.QuadPart > (1ull << 31)) {
    return false;
  }
  out->resize(static_cast<size_t>(size.QuadPart));
  if (FAILED(::IStream_Reset(stream))) {
    return false;
  }
  return out->empty() ||
         SUCCEEDED(::IStream_Read(stream, out->data(),
                                  static_cast<ULONG>(out->size())));
}

// Opaque premultiplied BGRA → packed 24-bit BGR rows (stride width * 3).
std::vector<uint8_t> ToBgr24(const PhiScreenshotImage& image) {
  std::vector<uint8_t> out(static_cast<size_t>(image.width) * image.height *
                           3);
  const size_t pixels = static_cast<size_t>(image.width) * image.height;
  for (size_t i = 0; i < pixels; ++i) {
    out[i * 3] = image.bgra[i * 4];
    out[i * 3 + 1] = image.bgra[i * 4 + 1];
    out[i * 3 + 2] = image.bgra[i * 4 + 2];
  }
  return out;
}

bool EncodeWic(const PhiScreenshotImage& image,
               const GUID& container,
               bool jpeg,
               std::string* bytes) {
  ComPtr<IWICImagingFactory> wic = Wic();
  ComPtr<IStream> stream;
  stream.Attach(::SHCreateMemStream(nullptr, 0));
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> options;
  if (!wic || !stream ||
      FAILED(wic->CreateEncoder(container, nullptr, &encoder)) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
      FAILED(encoder->CreateNewFrame(&frame, &options))) {
    return false;
  }
  if (jpeg && options) {
    PROPBAG2 option = {};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT value;
    ::VariantInit(&value);
    value.vt = VT_R4;
    value.fltVal = 0.9f;
    options->Write(1, &option, &value);
  }
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  std::vector<uint8_t> rows = ToBgr24(image);
  if (FAILED(frame->Initialize(options.Get())) ||
      FAILED(frame->SetSize(static_cast<UINT>(image.width),
                            static_cast<UINT>(image.height))) ||
      FAILED(frame->SetPixelFormat(&format)) ||
      format != GUID_WICPixelFormat24bppBGR ||
      FAILED(frame->WritePixels(static_cast<UINT>(image.height),
                                static_cast<UINT>(image.width) * 3,
                                static_cast<UINT>(rows.size()), rows.data())) ||
      FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
    return false;
  }
  return StreamBytes(stream.Get(), bytes);
}

}  // namespace

const wchar_t* PhiScreenshotFormatExt(PhiScreenshotFormat format) {
  switch (format) {
    case PhiScreenshotFormat::kPng:
      return L"png";
    case PhiScreenshotFormat::kJpg:
      return L"jpg";
    case PhiScreenshotFormat::kPdf:
      return L"pdf";
  }
  return L"png";
}

bool PhiScreenshotParseFormat(const std::string& name,
                              PhiScreenshotFormat* format) {
  if (name == "png") {
    *format = PhiScreenshotFormat::kPng;
  } else if (name == "jpg" || name == "jpeg") {
    *format = PhiScreenshotFormat::kJpg;
  } else if (name == "pdf") {
    *format = PhiScreenshotFormat::kPdf;
  } else {
    return false;
  }
  return true;
}

PhiScreenshotImageRef PhiScreenshotDecodePng(const std::string& png) {
  ComPtr<IWICImagingFactory> wic = Wic();
  if (!wic || png.empty()) {
    return nullptr;
  }
  ComPtr<IStream> stream;
  stream.Attach(::SHCreateMemStream(
      reinterpret_cast<const BYTE*>(png.data()), static_cast<UINT>(png.size())));
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICBitmapSource> converted;
  if (!stream ||
      FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr,
                                          WICDecodeMetadataCacheOnDemand,
                                          &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame)) ||
      FAILED(::WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA,
                                      frame.Get(), &converted))) {
    return nullptr;
  }
  UINT w = 0;
  UINT h = 0;
  if (FAILED(converted->GetSize(&w, &h)) || w == 0 || h == 0 ||
      w > static_cast<UINT>(kPhiScreenshotMaxDecodeAxis) ||
      h > static_cast<UINT>(kPhiScreenshotMaxDecodeAxis) ||
      static_cast<int64_t>(w) * h > kPhiScreenshotMaxPixels) {
    return nullptr;
  }
  auto image = std::make_shared<PhiScreenshotImage>();
  image->width = static_cast<int>(w);
  image->height = static_cast<int>(h);
  image->bgra.resize(static_cast<size_t>(w) * h * 4);
  if (FAILED(converted->CopyPixels(nullptr, w * 4,
                                   static_cast<UINT>(image->bgra.size()),
                                   image->bgra.data()))) {
    return nullptr;
  }
  return image;
}

// --- Painter --------------------------------------------------------------------

PhiScreenshotPainter::PhiScreenshotPainter() {
  D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                    factory_.GetAddressOf());
  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                      reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
  if (factory_) {
    factory_->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                    D2D1_CAP_STYLE_ROUND,
                                    D2D1_LINE_JOIN_ROUND),
        nullptr, 0, &round_);
  }
}

PhiScreenshotPainter::~PhiScreenshotPainter() = default;

ComPtr<ID2D1Bitmap> PhiScreenshotPainter::Upload(
    ID2D1RenderTarget* target,
    const PhiScreenshotImage& image) {
  ComPtr<ID2D1Bitmap> bitmap;
  if (!target || image.width <= 0 || image.height <= 0) {
    return bitmap;
  }
  const UINT32 max = target->GetMaximumBitmapSize();
  if (static_cast<UINT32>(image.width) > max ||
      static_cast<UINT32>(image.height) > max) {
    return bitmap;
  }
  target->CreateBitmap(
      D2D1::SizeU(static_cast<UINT32>(image.width),
                  static_cast<UINT32>(image.height)),
      image.bgra.data(), static_cast<UINT32>(image.width) * 4,
      D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                               D2D1_ALPHA_MODE_PREMULTIPLIED),
                             96.0f, 96.0f),
      &bitmap);
  return bitmap;
}

ComPtr<IDWriteTextLayout> PhiScreenshotPainter::Layout(const PhiAnnotation& a) {
  ComPtr<IDWriteTextFormat> format;
  ComPtr<IDWriteTextLayout> layout;
  if (!dwrite_ ||
      FAILED(dwrite_->CreateTextFormat(
          L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
          static_cast<float>(std::max(1.0, a.font_size)), L"", &format))) {
    return layout;
  }
  format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
  const std::wstring text = a.text.empty() ? std::wstring(L" ") : a.text;
  const float width = static_cast<float>(std::max(1.0, a.frame.width()));
  dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                            format.Get(), width, 1.0e6f, &layout);
  return layout;
}

double PhiScreenshotPainter::TextHeight(const PhiAnnotation& a) {
  ComPtr<IDWriteTextLayout> layout = Layout(a);
  DWRITE_TEXT_METRICS metrics = {};
  if (!layout || FAILED(layout->GetMetrics(&metrics))) {
    return a.font_size * 1.4;
  }
  return std::max<double>(metrics.height, a.font_size);
}

void PhiScreenshotPainter::DrawAnnotation(ID2D1RenderTarget* target,
                                          const PhiScreenshotImage& image,
                                          const PhiAnnotation& a) {
  ComPtr<ID2D1SolidColorBrush> ink;
  target->CreateSolidColorBrush(ColorF(a.color), &ink);
  if (!ink) {
    return;
  }
  const float width = static_cast<float>(std::max(0.5, a.stroke));
  switch (a.kind) {
    case PhiAnnotationKind::kRectangle: {
      const D2D1_RECT_F r = Rc(a.frame);
      if (a.fill) {
        ComPtr<ID2D1SolidColorBrush> fill;
        target->CreateSolidColorBrush(ColorF(a.color, 0.8f), &fill);
        if (fill) {
          target->FillRectangle(r, fill.Get());
        }
      }
      target->DrawRectangle(r, ink.Get(), width, round_.Get());
      return;
    }
    case PhiAnnotationKind::kEllipse: {
      const D2D1_RECT_F r = Rc(a.frame);
      const D2D1_ELLIPSE e = D2D1::Ellipse(
          D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2),
          (r.right - r.left) / 2, (r.bottom - r.top) / 2);
      if (a.fill) {
        ComPtr<ID2D1SolidColorBrush> fill;
        target->CreateSolidColorBrush(ColorF(a.color, 0.8f), &fill);
        if (fill) {
          target->FillEllipse(e, fill.Get());
        }
      }
      target->DrawEllipse(e, ink.Get(), width, round_.Get());
      return;
    }
    case PhiAnnotationKind::kArrow: {
      target->DrawLine(Pt(a.from), Pt(a.to), ink.Get(), width, round_.Get());
      const double dx = a.to.x - a.from.x;
      const double dy = a.to.y - a.from.y;
      const double distance = std::sqrt(dx * dx + dy * dy);
      if (distance <= 0) {
        return;
      }
      // The Mac's open head: two 45-degree wings, round-joined at the tip.
      const double h = PhiArrowHeadLength(a, distance);
      const double ux = -dx * h / distance;
      const double uy = -dy * h / distance;
      const double c = 0.70710678118654752;
      const double s = 0.70710678118654752;
      const PhiPointF w1 = {a.to.x + c * ux - s * uy, a.to.y + s * ux + c * uy};
      const PhiPointF w2 = {a.to.x + c * ux + s * uy, a.to.y + c * uy - s * ux};
      ComPtr<ID2D1PathGeometry> head;
      ComPtr<ID2D1GeometrySink> sink;
      if (SUCCEEDED(factory_->CreatePathGeometry(&head)) &&
          SUCCEEDED(head->Open(&sink))) {
        sink->BeginFigure(Pt(w1), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(Pt(a.to));
        sink->AddLine(Pt(w2));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        target->DrawGeometry(head.Get(), ink.Get(), width, round_.Get());
      }
      return;
    }
    case PhiAnnotationKind::kFreehand: {
      if (a.points.empty()) {
        return;
      }
      if (a.points.size() == 1) {
        target->FillEllipse(D2D1::Ellipse(Pt(a.points[0]), width / 2, width / 2),
                            ink.Get());
        return;
      }
      std::vector<PhiPointF> curve;
      PhiFreehandCurve(a.points, &curve);
      ComPtr<ID2D1PathGeometry> path;
      ComPtr<ID2D1GeometrySink> sink;
      if (FAILED(factory_->CreatePathGeometry(&path)) ||
          FAILED(path->Open(&sink))) {
        return;
      }
      sink->BeginFigure(Pt(curve[0]), D2D1_FIGURE_BEGIN_HOLLOW);
      for (size_t i = 1; i + 1 < curve.size(); i += 2) {
        sink->AddQuadraticBezier(
            D2D1::QuadraticBezierSegment(Pt(curve[i]), Pt(curve[i + 1])));
      }
      sink->EndFigure(D2D1_FIGURE_END_OPEN);
      sink->Close();
      target->DrawGeometry(path.Get(), ink.Get(), width, round_.Get());
      return;
    }
    case PhiAnnotationKind::kText: {
      if (a.text.empty()) {
        return;
      }
      ComPtr<IDWriteTextLayout> layout = Layout(a);
      if (layout) {
        target->DrawTextLayout(
            D2D1::Point2F(static_cast<float>(std::min(a.frame.left,
                                                      a.frame.right)),
                          static_cast<float>(std::min(a.frame.top,
                                                      a.frame.bottom))),
            layout.Get(), ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
      }
      return;
    }
    case PhiAnnotationKind::kBlur: {
      std::vector<PhiBlurBlock> blocks;
      PhiBlurBlocks(image, a.frame, kPhiBlurBlock, &blocks);
      const D2D1_ANTIALIAS_MODE mode = target->GetAntialiasMode();
      // Blocks tile without seams.
      target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
      ComPtr<ID2D1SolidColorBrush> brush;
      target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush);
      for (const PhiBlurBlock& block : blocks) {
        // Premultiplied and opaque: the channels ARE the colour.
        brush->SetColor(D2D1::ColorF(((block.bgra >> 16) & 0xFF) / 255.0f,
                                     ((block.bgra >> 8) & 0xFF) / 255.0f,
                                     (block.bgra & 0xFF) / 255.0f, 1.0f));
        target->FillRectangle(Rc(block.rect), brush.Get());
      }
      target->SetAntialiasMode(mode);
      return;
    }
  }
}

void PhiScreenshotPainter::Draw(ID2D1RenderTarget* target,
                                ID2D1Bitmap* image_bitmap,
                                const PhiScreenshotDocument::State& state,
                                uint64_t skip_id) {
  const PhiScreenshotImage& image = *state.image;
  const D2D1_RECT_F canvas = D2D1::RectF(0, 0, static_cast<float>(image.width),
                                         static_cast<float>(image.height));
  ComPtr<ID2D1SolidColorBrush> white;
  target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
  if (white) {
    target->FillRectangle(canvas, white.Get());
  }
  if (image_bitmap) {
    target->DrawBitmap(image_bitmap, canvas, 1.0f,
                       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
  }
  // Annotations past the image edge are clipped to the canvas, as the
  // export's page is.
  target->PushAxisAlignedClip(canvas, D2D1_ANTIALIAS_MODE_ALIASED);
  for (const PhiAnnotation& a : state.annotations) {
    if (a.id != skip_id) {
      DrawAnnotation(target, image, a);
    }
  }
  target->PopAxisAlignedClip();
}

bool PhiScreenshotPainter::Flatten(const PhiScreenshotDocument::State& state,
                                   PhiScreenshotImage* out) {
  ComPtr<IWICImagingFactory> wic = Wic();
  const PhiScreenshotImage& image = *state.image;
  ComPtr<IWICBitmap> bitmap;
  ComPtr<ID2D1RenderTarget> target;
  if (!wic || !factory_ ||
      FAILED(wic->CreateBitmap(static_cast<UINT>(image.width),
                               static_cast<UINT>(image.height),
                               GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapCacheOnLoad, &bitmap)) ||
      FAILED(factory_->CreateWicBitmapRenderTarget(
          bitmap.Get(),
          D2D1::RenderTargetProperties(
              D2D1_RENDER_TARGET_TYPE_SOFTWARE,
              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                D2D1_ALPHA_MODE_PREMULTIPLIED),
              96.0f, 96.0f),
          &target))) {
    return false;
  }
  target->BeginDraw();
  target->Clear(D2D1::ColorF(1, 1, 1, 1));
  ComPtr<ID2D1Bitmap> source = Upload(target.Get(), image);
  Draw(target.Get(), source.Get(), state, 0);
  if (FAILED(target->EndDraw())) {
    return false;
  }
  out->width = image.width;
  out->height = image.height;
  out->bgra.resize(static_cast<size_t>(image.width) * image.height * 4);
  if (FAILED(bitmap->CopyPixels(nullptr, static_cast<UINT>(image.width) * 4,
                                static_cast<UINT>(out->bgra.size()),
                                out->bgra.data()))) {
    return false;
  }
  // The white backdrop makes every pixel opaque; settle the rounding.
  for (size_t i = 3; i < out->bgra.size(); i += 4) {
    out->bgra[i] = 0xFF;
  }
  return true;
}

// --- Files --------------------------------------------------------------------------

bool PhiScreenshotWriteBytes(const std::string& bytes,
                             const std::wstring& path,
                             std::string* error) {
  const std::wstring part = path + L".part";
  HANDLE file = ::CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    *error = "open_failed";
    return false;
  }
  size_t written = 0;
  bool ok = true;
  while (ok && written < bytes.size()) {
    const DWORD chunk =
        static_cast<DWORD>(std::min<size_t>(bytes.size() - written, 1u << 24));
    DWORD wrote = 0;
    ok = ::WriteFile(file, UNSAFE_BUFFERS(bytes.data() + written), chunk,
                     &wrote, nullptr) &&
         wrote == chunk;
    written += wrote;
  }
  ok = ::FlushFileBuffers(file) && ok;
  ::CloseHandle(file);
  if (!ok) {
    ::DeleteFileW(part.c_str());
    *error = "write_failed";
    return false;
  }
  if (!::MoveFileExW(part.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    ::DeleteFileW(part.c_str());
    *error = "rename_failed";
    return false;
  }
  return true;
}

std::string PhiScreenshotPdf(const PhiScreenshotImage& image) {
  if (image.width <= 0 || image.height <= 0) {
    return std::string();
  }
  // DeviceRGB rows, top-down (PDF images are top row first).
  const size_t pixels = static_cast<size_t>(image.width) * image.height;
  std::string rgb(pixels * 3, '\0');
  for (size_t i = 0; i < pixels; ++i) {
    rgb[i * 3] = static_cast<char>(image.bgra[i * 4 + 2]);
    rgb[i * 3 + 1] = static_cast<char>(image.bgra[i * 4 + 1]);
    rgb[i * 3 + 2] = static_cast<char>(image.bgra[i * 4]);
  }
  z_stream strm = {};
  if (deflateInit(&strm, Z_DEFAULT_COMPRESSION) != Z_OK) {
    return std::string();
  }
  std::string packed(deflateBound(&strm, static_cast<uLong>(rgb.size())), '\0');
  strm.next_in = reinterpret_cast<Bytef*>(rgb.data());
  strm.avail_in = static_cast<uInt>(rgb.size());
  strm.next_out = reinterpret_cast<Bytef*>(packed.data());
  strm.avail_out = static_cast<uInt>(packed.size());
  const int rc = deflate(&strm, Z_FINISH);
  const size_t produced = packed.size() - strm.avail_out;
  deflateEnd(&strm);
  if (rc != Z_STREAM_END) {
    return std::string();
  }
  packed.resize(produced);

  std::string pdf;
  std::vector<size_t> offsets;
  auto object = [&](const std::string& body) {
    offsets.push_back(pdf.size());
    pdf += std::to_string(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
  };
  const std::string w = std::to_string(image.width);
  const std::string h = std::to_string(image.height);
  pdf += "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
  object("<< /Type /Catalog /Pages 2 0 R >>");
  object("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
  object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + w + " " + h +
         "] /Resources << /XObject << /Im0 4 0 R >> >> /Contents 5 0 R >>");
  object("<< /Type /XObject /Subtype /Image /Width " + w + " /Height " + h +
         " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode"
         " /Length " +
         std::to_string(packed.size()) + " >>\nstream\n" + packed +
         "\nendstream");
  const std::string content = "q " + w + " 0 0 " + h + " 0 0 cm /Im0 Do Q";
  object("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" +
         content + "\nendstream");
  const size_t xref = pdf.size();
  pdf += "xref\n0 " + std::to_string(offsets.size() + 1) +
         "\n0000000000 65535 f \n";
  for (size_t at : offsets) {
    char line[32];
    std::snprintf(line, sizeof(line), "%010zu 00000 n \n", at);
    pdf += line;
  }
  pdf += "trailer\n<< /Size " + std::to_string(offsets.size() + 1) +
         " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
  return pdf;
}

bool PhiScreenshotWriteFile(const PhiScreenshotImage& image,
                            PhiScreenshotFormat format,
                            const std::wstring& path,
                            std::string* error) {
  std::string bytes;
  switch (format) {
    case PhiScreenshotFormat::kPng:
      if (!EncodeWic(image, GUID_ContainerFormatPng, false, &bytes)) {
        *error = "encode_failed";
        return false;
      }
      break;
    case PhiScreenshotFormat::kJpg:
      if (!EncodeWic(image, GUID_ContainerFormatJpeg, true, &bytes)) {
        *error = "encode_failed";
        return false;
      }
      break;
    case PhiScreenshotFormat::kPdf:
      bytes = PhiScreenshotPdf(image);
      if (bytes.empty()) {
        *error = "encode_failed";
        return false;
      }
      break;
  }
  return PhiScreenshotWriteBytes(bytes, path, error);
}

}  // namespace phi_client

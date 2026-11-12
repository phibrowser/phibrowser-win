// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_feedback_image.h"

#include <windows.h>

#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <utility>

#include "base/logging.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

namespace phi_client {

namespace {

using Microsoft::WRL::ComPtr;

ComPtr<IWICImagingFactory> Factory() {
  ComPtr<IWICImagingFactory> factory;
  ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&factory));
  return factory;
}

// Encodes `source` (any WIC bitmap source) with `container` into `stream`,
// optionally at JPEG `quality` (ignored by PNG).
bool Encode(IWICImagingFactory* factory,
            IWICBitmapSource* source,
            const GUID& container,
            float quality,
            IStream* stream) {
  ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(factory->CreateEncoder(container, nullptr, &encoder)) ||
      FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) {
    return false;
  }
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> options;
  if (FAILED(encoder->CreateNewFrame(&frame, &options))) {
    return false;
  }
  if (container == GUID_ContainerFormatJpeg && options) {
    PROPBAG2 property = {};
    wchar_t name[] = L"ImageQuality";
    property.pstrName = name;
    VARIANT value;
    ::VariantInit(&value);
    value.vt = VT_R4;
    value.fltVal = quality;
    options->Write(1, &property, &value);
  }
  if (FAILED(frame->Initialize(options.Get()))) {
    return false;
  }
  UINT width = 0;
  UINT height = 0;
  source->GetSize(&width, &height);
  frame->SetSize(width, height);
  // JPEG has no alpha: ask for an opaque pixel format so a transparent
  // screenshot does not fail the conversion.
  WICPixelFormatGUID format = container == GUID_ContainerFormatJpeg
                                  ? GUID_WICPixelFormat24bppBGR
                                  : GUID_WICPixelFormat32bppBGRA;
  frame->SetPixelFormat(&format);
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(source, format, WICBitmapDitherTypeNone,
                                   nullptr, 0.0,
                                   WICBitmapPaletteTypeMedianCut))) {
    return false;
  }
  return SUCCEEDED(frame->WriteSource(converter.Get(), nullptr)) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

bool StreamToString(IStream* stream, std::string* out) {
  STATSTG stat = {};
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) {
    return false;
  }
  out->resize(static_cast<size_t>(stat.cbSize.QuadPart));
  LARGE_INTEGER zero = {};
  if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) {
    return false;
  }
  ULONG read = 0;
  return out->empty() ||
         (SUCCEEDED(stream->Read(out->data(),
                                 static_cast<ULONG>(out->size()), &read)) &&
          read == out->size());
}

bool WriteFileBytes(const std::wstring& path, const std::string& bytes) {
  HANDLE f = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  const BOOL ok = ::WriteFile(f, bytes.data(),
                              static_cast<DWORD>(bytes.size()), &written,
                              nullptr);
  ::CloseHandle(f);
  return ok && written == bytes.size();
}

}  // namespace

bool PhiClipboardImageToPng(std::string* png) {
  if (!::IsClipboardFormatAvailable(CF_BITMAP) &&
      !::IsClipboardFormatAvailable(CF_DIB)) {
    return false;
  }
  if (!::OpenClipboard(nullptr)) {
    return false;
  }
  // CF_BITMAP is synthesized from CF_DIB and vice versa, so one HBITMAP
  // covers both; WIC reads it straight off the GDI handle.
  HBITMAP bitmap = static_cast<HBITMAP>(::GetClipboardData(CF_BITMAP));
  bool ok = false;
  if (bitmap) {
    if (ComPtr<IWICImagingFactory> factory = Factory()) {
      ComPtr<IWICBitmap> source;
      if (SUCCEEDED(factory->CreateBitmapFromHBITMAP(
              bitmap, nullptr, WICBitmapUseAlpha, &source))) {
        ComPtr<IStream> stream(::SHCreateMemStream(nullptr, 0));
        ok = stream && Encode(factory.Get(), source.Get(),
                              GUID_ContainerFormatPng, 1.0f, stream.Get()) &&
             StreamToString(stream.Get(), png);
      }
    }
  }
  ::CloseClipboard();
  return ok;
}

bool PhiEncodeJpegUnderLimit(const std::wstring& source,
                             int64_t max_bytes,
                             const std::wstring& destination) {
  ComPtr<IWICImagingFactory> factory = Factory();
  if (!factory) {
    return false;
  }
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(factory->CreateDecoderFromFilename(
          source.c_str(), nullptr, GENERIC_READ,
          WICDecodeMetadataCacheOnDemand, &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame))) {
    return false;
  }
  UINT width = 0;
  UINT height = 0;
  if (FAILED(frame->GetSize(&width, &height)) || !width || !height) {
    return false;
  }
  // The Mac's ladder: dimensions outermost, qualities within.
  constexpr std::array<UINT, 7> kDimensions = {8192, 6144, 4096, 3072,
                                               2048, 1536, 1024};
  constexpr std::array<float, 6> kQualities = {0.92f, 0.82f, 0.72f,
                                               0.62f, 0.52f, 0.42f};
  const UINT longest = width > height ? width : height;
  for (UINT dimension : kDimensions) {
    ComPtr<IWICBitmapSource> scaled;
    if (longest > dimension) {
      const double ratio = static_cast<double>(dimension) / longest;
      ComPtr<IWICBitmapScaler> scaler;
      if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
          FAILED(scaler->Initialize(
              frame.Get(), static_cast<UINT>(width * ratio),
              static_cast<UINT>(height * ratio),
              WICBitmapInterpolationModeFant))) {
        continue;
      }
      scaled = scaler;
    } else {
      scaled = frame;
    }
    for (float quality : kQualities) {
      ComPtr<IStream> stream(::SHCreateMemStream(nullptr, 0));
      std::string bytes;
      if (!stream ||
          !Encode(factory.Get(), scaled.Get(), GUID_ContainerFormatJpeg,
                  quality, stream.Get()) ||
          !StreamToString(stream.Get(), &bytes)) {
        continue;
      }
      if (static_cast<int64_t>(bytes.size()) <= max_bytes) {
        return WriteFileBytes(destination, bytes);
      }
    }
  }
  return false;
}

bool PhiDecodeImageThumbnail(const std::wstring& path,
                             const std::string& bytes,
                             int width,
                             int height,
                             std::vector<unsigned char>* bgra) {
  if (width <= 0 || height <= 0) {
    return false;
  }
  ComPtr<IWICImagingFactory> factory = Factory();
  if (!factory) {
    return false;
  }
  ComPtr<IWICBitmapDecoder> decoder;
  if (!path.empty()) {
    if (FAILED(factory->CreateDecoderFromFilename(
            path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder))) {
      return false;
    }
  } else {
    ComPtr<IStream> stream;
    stream.Attach(::SHCreateMemStream(
        reinterpret_cast<const BYTE*>(bytes.data()),
        static_cast<UINT>(bytes.size())));
    if (!stream ||
        FAILED(factory->CreateDecoderFromStream(
            stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
            &decoder))) {
      return false;
    }
  }
  ComPtr<IWICBitmapFrameDecode> frame;
  UINT source_w = 0;
  UINT source_h = 0;
  if (FAILED(decoder->GetFrame(0, &frame)) ||
      FAILED(frame->GetSize(&source_w, &source_h)) || !source_w ||
      !source_h) {
    return false;
  }
  // Cover: the scale that makes BOTH sides at least the tile's.
  const double scale =
      std::max(static_cast<double>(width) / source_w,
               static_cast<double>(height) / source_h);
  const UINT scaled_w = std::max<UINT>(
      static_cast<UINT>(width), static_cast<UINT>(source_w * scale + 0.5));
  const UINT scaled_h = std::max<UINT>(
      static_cast<UINT>(height), static_cast<UINT>(source_h * scale + 0.5));
  ComPtr<IWICBitmapScaler> scaler;
  if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
      FAILED(scaler->Initialize(frame.Get(), scaled_w, scaled_h,
                                WICBitmapInterpolationModeHighQualityCubic))) {
    return false;
  }
  ComPtr<IWICBitmapClipper> clipper;
  const WICRect crop = {static_cast<INT>((scaled_w - width) / 2),
                        static_cast<INT>((scaled_h - height) / 2), width,
                        height};
  if (FAILED(factory->CreateBitmapClipper(&clipper)) ||
      FAILED(clipper->Initialize(scaler.Get(), &crop))) {
    return false;
  }
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(clipper.Get(), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeMedianCut))) {
    return false;
  }
  const UINT stride = static_cast<UINT>(width) * 4;
  bgra->assign(static_cast<size_t>(stride) * static_cast<size_t>(height), 0);
  return SUCCEEDED(converter->CopyPixels(
      nullptr, stride, static_cast<UINT>(bgra->size()), bgra->data()));
}

}  // namespace phi_client

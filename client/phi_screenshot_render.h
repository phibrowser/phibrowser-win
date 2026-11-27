// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SCREENSHOT_RENDER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SCREENSHOT_RENDER_H_

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>

#include "client/phi_screenshot_model.h"

// Page screenshots (docs/page-screenshots.md): the drawing the editor's
// canvas and the export share — one Direct2D pass over the image and its
// annotations, so preview and file never disagree — plus WIC decode /
// encode and the one-page PDF writer.

namespace phi_client {

enum class PhiScreenshotFormat { kPng, kJpg, kPdf };
const wchar_t* PhiScreenshotFormatExt(PhiScreenshotFormat format);
// "png" / "jpg" / "pdf"; false for anything else.
bool PhiScreenshotParseFormat(const std::string& name,
                              PhiScreenshotFormat* format);

// Decodes `png` into a premultiplied BGRA image. Null when it does not
// decode. UI thread (COM).
PhiScreenshotImageRef PhiScreenshotDecodePng(const std::string& png);

class PhiScreenshotPainter {
 public:
  PhiScreenshotPainter();
  ~PhiScreenshotPainter();
  PhiScreenshotPainter(const PhiScreenshotPainter&) = delete;
  PhiScreenshotPainter& operator=(const PhiScreenshotPainter&) = delete;

  bool ok() const { return factory_ && dwrite_; }
  ID2D1Factory* factory() const { return factory_.Get(); }
  IDWriteFactory* dwrite() const { return dwrite_.Get(); }

  // `image` as a bitmap of `target` (premultiplied BGRA). Null on failure
  // (a target's maximum bitmap size, out of memory).
  Microsoft::WRL::ComPtr<ID2D1Bitmap> Upload(ID2D1RenderTarget* target,
                                             const PhiScreenshotImage& image);

  // Draws `image_bitmap` (the state's image on this target; null draws
  // white) and every annotation of `state` except `skip_id` (the text being
  // typed). `target`'s transform maps SOURCE pixels to the target.
  void Draw(ID2D1RenderTarget* target,
            ID2D1Bitmap* image_bitmap,
            const PhiScreenshotDocument::State& state,
            uint64_t skip_id);
  // Draws one annotation.
  void DrawAnnotation(ID2D1RenderTarget* target,
                      const PhiScreenshotImage& image,
                      const PhiAnnotation& a);

  // The laid-out height of `a`'s text at its frame's width (source px);
  // an empty text measures one line.
  double TextHeight(const PhiAnnotation& a);

  // The flattened document at source size on a white backdrop, opaque.
  bool Flatten(const PhiScreenshotDocument::State& state,
               PhiScreenshotImage* out);

 private:
  Microsoft::WRL::ComPtr<IDWriteTextLayout> Layout(const PhiAnnotation& a);

  Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
  Microsoft::WRL::ComPtr<ID2D1StrokeStyle> round_;
};

// Writes the flattened, opaque `image` to `path` as `format` (PNG lossless,
// JPG quality 0.9, PDF one page at a point per pixel), through a temporary
// file renamed over the target. False with `error` set on failure.
bool PhiScreenshotWriteFile(const PhiScreenshotImage& image,
                            PhiScreenshotFormat format,
                            const std::wstring& path,
                            std::string* error);

// The PDF bytes for an opaque `image`: one page, the picture as a
// Flate-compressed DeviceRGB image. Empty on failure.
std::string PhiScreenshotPdf(const PhiScreenshotImage& image);

// Writes `bytes` to `path` through `path`.part and a rename.
bool PhiScreenshotWriteBytes(const std::string& bytes,
                             const std::wstring& path,
                             std::string* error);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SCREENSHOT_RENDER_H_

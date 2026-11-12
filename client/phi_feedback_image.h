// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_FEEDBACK_IMAGE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_FEEDBACK_IMAGE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace phi_client {

// The feedback form's image codecs, over WIC — the Mac's FeedbackImageEncoder
// (NSBitmapImageRep) twin.

// True when the clipboard holds an image the form can attach (a DIB or
// bitmap); `png` receives it PNG-encoded. UI thread (opens the clipboard).
bool PhiClipboardImageToPng(std::string* png);

// Re-encodes the image at `source` as a JPEG no larger than `max_bytes`,
// walking the Mac's ladder (longest side 8192 → 1024, quality 0.92 → 0.42)
// and writing the first fit to `destination`. False when no rung fits or
// the source does not decode. Any thread with COM initialized.
bool PhiEncodeJpegUnderLimit(const std::wstring& source,
                             int64_t max_bytes,
                             const std::wstring& destination);

// Decodes the image at `path` (or, when `path` is empty, the encoded
// `bytes`) into a thumbnail that COVERS exactly `width` x `height` device
// px — scaled so the shorter side fits, the overflow cropped evenly — as
// premultiplied BGRA rows, the attachment tile's picture. False when the
// image does not decode. UI thread (COM initialized).
bool PhiDecodeImageThumbnail(const std::wstring& path,
                             const std::string& bytes,
                             int width,
                             int height,
                             std::vector<unsigned char>* bgra);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_FEEDBACK_IMAGE_H_

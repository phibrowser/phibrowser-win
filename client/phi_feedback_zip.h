// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_FEEDBACK_ZIP_H_
#define PHIBROWSER_WIN_CLIENT_PHI_FEEDBACK_ZIP_H_

#include <cstdint>
#include <string>
#include <vector>

namespace phi_client {

// One entry of a feedback archive — the Mac FeedbackOutbox's ArchiveItem: a
// slice of a file on disk (`offset` + `length`, the whole file when offset is
// 0 and length its size) or an inline note, filed under `archive_path`
// (forward slashes, UTF-8: "PhiLogs/chrome_debug.log").
struct PhiArchiveItem {
  std::wstring source_path;  // Empty for an inline item.
  std::string inline_data;
  uint64_t offset = 0;
  uint64_t length = 0;
  std::string archive_path;
};

// Writes `items` to a zip at `destination` (replaced when present). The Mac
// shells out to /usr/bin/zip; Windows has no zip tool to lean on, so this is
// a small writer of its own over the framework's zlib: DEFLATE entries,
// data descriptors, a UTF-8 central directory — the subset every reader
// understands. False with `error` set on any I/O or zlib failure; a missing
// source file is an error, not a silent gap, since the manifest promised it.
bool PhiWriteZip(const std::vector<PhiArchiveItem>& items,
                 const std::wstring& destination,
                 std::string* error);

// Splits `items` into consecutive groups whose summed lengths stay under
// `planning_bytes` (an item larger than the budget gets a group of its own)
// — the Mac's bucketArchiveItems, the first cut before the actual zip sizes
// are measured.
std::vector<std::vector<PhiArchiveItem>> PhiBucketArchiveItems(
    const std::vector<PhiArchiveItem>& items,
    int64_t planning_bytes);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_FEEDBACK_ZIP_H_

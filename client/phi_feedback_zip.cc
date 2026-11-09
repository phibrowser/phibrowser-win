// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_feedback_zip.h"

#include <cstdio>
#include <ctime>
#include <memory>
#include <utility>

#include "base/compiler_specific.h"
#include "third_party/zlib/zlib.h"

namespace phi_client {

namespace {

// One central-directory record, remembered while the local entries stream
// out.
struct Entry {
  std::string name;
  uint32_t crc = 0;
  uint64_t compressed = 0;
  uint64_t uncompressed = 0;
  uint64_t local_header_offset = 0;
  uint16_t method = 8;
};

struct FileCloser {
  void operator()(FILE* f) const {
    if (f) {
      std::fclose(f);
    }
  }
};
using FilePtr = std::unique_ptr<FILE, FileCloser>;

// The entry timestamp in DOS form: "now", as the zip tool stamps files it
// writes. The archive is a transport, not a record of mtimes.
void DosTimestamp(uint16_t* time, uint16_t* date) {
  const std::time_t now = std::time(nullptr);
  std::tm local = {};
#if defined(_WIN32)
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  const int year = local.tm_year + 1900;
  *date = static_cast<uint16_t>(((year < 1980 ? 0 : year - 1980) << 9) |
                                ((local.tm_mon + 1) << 5) | local.tm_mday);
  *time = static_cast<uint16_t>((local.tm_hour << 11) | (local.tm_min << 5) |
                                (local.tm_sec / 2));
}

void Put16(std::string* out, uint32_t v) {
  out->push_back(static_cast<char>(v & 0xff));
  out->push_back(static_cast<char>((v >> 8) & 0xff));
}

void Put32(std::string* out, uint32_t v) {
  Put16(out, v & 0xffff);
  Put16(out, (v >> 16) & 0xffff);
}

bool Write(FILE* f, const std::string& bytes, std::string* error) {
  if (bytes.empty()) {
    return true;
  }
  // The stdio calls are bounded by the containers they read from and write
  // to; the unsafe-libc gate cannot see that.
  if (UNSAFE_BUFFERS(std::fwrite(bytes.data(), 1, bytes.size(), f)) !=
      bytes.size()) {
    *error = "zip write failed";
    return false;
  }
  return true;
}

// Feeds `chunk` through the deflater, appending output to the archive.
bool DeflateChunk(z_stream* strm,
                  FILE* out,
                  const std::string& chunk,
                  int flush,
                  uint64_t* compressed,
                  std::string* error) {
  strm->next_in = reinterpret_cast<Bytef*>(const_cast<char*>(chunk.data()));
  strm->avail_in = static_cast<uInt>(chunk.size());
  std::string buffer(64 * 1024, '\0');
  do {
    strm->next_out = reinterpret_cast<Bytef*>(buffer.data());
    strm->avail_out = static_cast<uInt>(buffer.size());
    const int rc = deflate(strm, flush);
    if (rc == Z_STREAM_ERROR) {
      *error = "deflate failed";
      return false;
    }
    const size_t produced = buffer.size() - strm->avail_out;
    if (produced) {
      if (UNSAFE_BUFFERS(std::fwrite(buffer.data(), 1, produced, out)) !=
          produced) {
        *error = "zip write failed";
        return false;
      }
      *compressed += produced;
    }
  } while (strm->avail_out == 0);
  return true;
}

// Streams one item's bytes (a file slice or the inline note) into the
// deflater, computing the CRC on the way.
bool WriteItemData(const PhiArchiveItem& item,
                   FILE* out,
                   Entry* entry,
                   std::string* error) {
  z_stream strm = {};
  if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS,
                   8, Z_DEFAULT_STRATEGY) != Z_OK) {
    *error = "deflateInit failed";
    return false;
  }
  bool ok = true;
  uLong crc = crc32(0L, Z_NULL, 0);
  if (item.source_path.empty()) {
    crc = crc32(crc, reinterpret_cast<const Bytef*>(item.inline_data.data()),
                static_cast<uInt>(item.inline_data.size()));
    entry->uncompressed = item.inline_data.size();
    ok = DeflateChunk(&strm, out, item.inline_data, Z_FINISH,
                      &entry->compressed, error);
  } else {
    FilePtr in;
#if defined(_WIN32)
    in.reset(_wfopen(item.source_path.c_str(), L"rb"));
#else
    // The host self-test builds this file on Linux: narrow the path.
    std::string narrow(item.source_path.begin(), item.source_path.end());
    in.reset(std::fopen(narrow.c_str(), "rb"));
#endif
    if (!in) {
      *error = "zip source missing: " + item.archive_path;
      deflateEnd(&strm);
      return false;
    }
    if (item.offset && std::fseek(in.get(), static_cast<long>(item.offset),
                                  SEEK_SET) != 0) {
      *error = "zip source seek failed: " + item.archive_path;
      deflateEnd(&strm);
      return false;
    }
    uint64_t remaining = item.length;
    std::string chunk;
    while (ok) {
      const size_t want = static_cast<size_t>(
          remaining < 1024 * 1024 ? remaining : 1024 * 1024);
      chunk.resize(want);
      const size_t got =
          want ? UNSAFE_BUFFERS(std::fread(chunk.data(), 1, want, in.get()))
               : 0;
      chunk.resize(got);
      // A short read ends the entry: the file shrank since it was measured
      // (a live log rotated) — what is there is what ships, as with zip.
      const bool last = got < want || remaining - got == 0;
      if (got) {
        crc = crc32(crc, reinterpret_cast<const Bytef*>(chunk.data()),
                    static_cast<uInt>(chunk.size()));
        entry->uncompressed += got;
        remaining -= got;
      }
      ok = DeflateChunk(&strm, out, chunk, last ? Z_FINISH : Z_NO_FLUSH,
                        &entry->compressed, error);
      if (last) {
        break;
      }
    }
  }
  deflateEnd(&strm);
  entry->crc = static_cast<uint32_t>(crc);
  return ok;
}

}  // namespace

bool PhiWriteZip(const std::vector<PhiArchiveItem>& items,
                 const std::wstring& destination,
                 std::string* error) {
  error->clear();
  FilePtr out;
#if defined(_WIN32)
  out.reset(_wfopen(destination.c_str(), L"wb"));
#else
  std::string narrow(destination.begin(), destination.end());
  out.reset(std::fopen(narrow.c_str(), "wb"));
#endif
  if (!out) {
    *error = "zip destination could not be created";
    return false;
  }
  uint16_t dos_time = 0;
  uint16_t dos_date = 0;
  DosTimestamp(&dos_time, &dos_date);
  // General-purpose flags: bit 3 (sizes and CRC follow the data in a
  // descriptor, since the data streams), bit 11 (names are UTF-8).
  constexpr uint16_t kFlags = 0x0808;

  std::vector<Entry> entries;
  uint64_t offset = 0;
  for (const PhiArchiveItem& item : items) {
    Entry entry;
    entry.name = item.archive_path;
    entry.local_header_offset = offset;
    std::string header;
    Put32(&header, 0x04034b50);
    Put16(&header, 20);  // Version needed: 2.0 (deflate).
    Put16(&header, kFlags);
    Put16(&header, entry.method);
    Put16(&header, dos_time);
    Put16(&header, dos_date);
    Put32(&header, 0);  // CRC, sizes: in the descriptor.
    Put32(&header, 0);
    Put32(&header, 0);
    Put16(&header, static_cast<uint32_t>(entry.name.size()));
    Put16(&header, 0);  // Extra field length.
    header += entry.name;
    if (!Write(out.get(), header, error) ||
        !WriteItemData(item, out.get(), &entry, error)) {
      return false;
    }
    std::string descriptor;
    Put32(&descriptor, 0x08074b50);
    Put32(&descriptor, entry.crc);
    Put32(&descriptor, static_cast<uint32_t>(entry.compressed));
    Put32(&descriptor, static_cast<uint32_t>(entry.uncompressed));
    if (!Write(out.get(), descriptor, error)) {
      return false;
    }
    offset += header.size() + entry.compressed + descriptor.size();
    entries.push_back(std::move(entry));
  }

  const uint64_t central_offset = offset;
  std::string central;
  for (const Entry& entry : entries) {
    Put32(&central, 0x02014b50);
    Put16(&central, 20);  // Version made by: 2.0, MS-DOS attributes.
    Put16(&central, 20);  // Version needed.
    Put16(&central, kFlags);
    Put16(&central, entry.method);
    Put16(&central, dos_time);
    Put16(&central, dos_date);
    Put32(&central, entry.crc);
    Put32(&central, static_cast<uint32_t>(entry.compressed));
    Put32(&central, static_cast<uint32_t>(entry.uncompressed));
    Put16(&central, static_cast<uint32_t>(entry.name.size()));
    Put16(&central, 0);  // Extra.
    Put16(&central, 0);  // Comment.
    Put16(&central, 0);  // Disk number start.
    Put16(&central, 0);  // Internal attributes.
    Put32(&central, 0);  // External attributes.
    Put32(&central, static_cast<uint32_t>(entry.local_header_offset));
    central += entry.name;
  }
  std::string end;
  Put32(&end, 0x06054b50);
  Put16(&end, 0);  // This disk.
  Put16(&end, 0);  // Disk with the central directory.
  Put16(&end, static_cast<uint32_t>(entries.size()));
  Put16(&end, static_cast<uint32_t>(entries.size()));
  Put32(&end, static_cast<uint32_t>(central.size()));
  Put32(&end, static_cast<uint32_t>(central_offset));
  Put16(&end, 0);  // Comment length.
  if (!Write(out.get(), central, error) || !Write(out.get(), end, error)) {
    return false;
  }
  if (std::fflush(out.get()) != 0) {
    *error = "zip flush failed";
    return false;
  }
  return true;
}

std::vector<std::vector<PhiArchiveItem>> PhiBucketArchiveItems(
    const std::vector<PhiArchiveItem>& items,
    int64_t planning_bytes) {
  std::vector<std::vector<PhiArchiveItem>> buckets;
  std::vector<PhiArchiveItem> current;
  int64_t current_size = 0;
  for (const PhiArchiveItem& item : items) {
    const int64_t item_size = static_cast<int64_t>(item.length);
    if (!current.empty() && current_size + item_size > planning_bytes) {
      buckets.push_back(std::move(current));
      current.clear();
      current_size = 0;
    }
    current.push_back(item);
    current_size += item_size;
  }
  if (!current.empty()) {
    buckets.push_back(std::move(current));
  }
  return buckets;
}

}  // namespace phi_client

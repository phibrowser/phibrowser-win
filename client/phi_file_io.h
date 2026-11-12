// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_FILE_IO_H_
#define PHIBROWSER_WIN_CLIENT_PHI_FILE_IO_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/values.h"

namespace phi_client {

// The ONE way the client reads and writes its own files under the user data
// dir (docs/architecture-review.md §3.4). Raw Win32 rather than base/files
// on purpose: base::ReadFileToString DCHECKs on the browser UI thread and
// every client store is loaded there.
//
enum class PhiReadStatus {
  kOk,
  kNotFound,  // ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND only
  kError,     // there (or unknowable), but not readable: locked, denied, I/O
};

// Reads the whole file; `out` is left empty unless kOk. A failure other
// than "not there" (an antivirus or backup scanner's lock, a sharing
// violation) is retried briefly, then kError — never "missing", or a caller
// would write defaults over the user's file.
PhiReadStatus PhiReadFile(const std::wstring& path, std::string* out);

// PhiReadFile() == kOk: for callers that never write the file back.
bool PhiReadFileBytes(const std::wstring& path, std::string* out);

// Atomic replace: write `<path>.tmp`, flush, ReplaceFileW (MoveFileEx on a
// first save). The live file is never truncated, so a crash or power cut
// mid-write leaves the previous contents intact. The write and flush results
// are checked; false means nothing changed on disk (logged with the error).
bool PhiWriteFileAtomic(const std::wstring& path, std::string_view bytes);

// Missing is already removed; access and sharing failures must be retried.
bool PhiRemoveFileIfPresent(const std::wstring& path);

// Renames a file the loader could not parse to `<path>.corrupt-<unix ms>` so
// the next Save cannot overwrite the evidence with defaults. Logs at ERROR
// with `reason`. Nothing happens if the file is missing.
void PhiQuarantineFile(const std::wstring& path, const char* reason);

enum class PhiJsonFileStatus {
  kMissing,     // no file — first run; caller keeps its defaults
  kOk,          // parsed; *dict is filled
  kCorrupt,     // unparseable or not an object; the file was QUARANTINED
  kUnreadable,  // there but not readable (locked, denied): the caller keeps
                // defaults IN MEMORY and must not save over the file
};

// Load-with-quarantine for every JSON store: read, parse as an object, and
// on failure move the file aside. `schema_out` receives the top-level
// "schema" integer (0 when absent) so the caller can migrate.
PhiJsonFileStatus PhiReadJsonFile(const std::wstring& path,
                                  base::DictValue* dict,
                                  int* schema_out = nullptr);

// Serialise and PhiWriteFileAtomic. `schema` > 0 is stamped as "schema".
bool PhiWriteJsonFile(const std::wstring& path,
                      base::DictValue dict,
                      int schema = 0);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_FILE_IO_H_

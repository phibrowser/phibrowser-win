// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_FILES_FILE_UTIL_H_
#define BASE_FILES_FILE_UTIL_H_

#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"

namespace base {

bool ReadFileToString(const FilePath& path, std::string* contents);
std::optional<std::vector<uint8_t>> ReadFileToBytes(const FilePath& path);

}  // namespace base

#endif  // BASE_FILES_FILE_UTIL_H_

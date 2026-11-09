// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_JSON_JSON_READER_H_
#define BASE_JSON_JSON_READER_H_

#include <cstddef>
#include <optional>
#include <string_view>

#include "base/values.h"

namespace base {

enum JSONParserOptions {
  JSON_PARSE_RFC = 0,
  JSON_ALLOW_TRAILING_COMMAS = 1 << 0,
  JSON_REPLACE_INVALID_CHARACTERS = 1 << 1,
  JSON_ALLOW_COMMENTS = 1 << 2,
  JSON_ALLOW_VERT_TAB = 1 << 3,
  JSON_ALLOW_X_ESCAPES = 1 << 4,
  JSON_ALLOW_NEWLINES_IN_STRINGS = 1 << 5,
  JSON_PARSE_CHROMIUM_EXTENSIONS = JSON_ALLOW_COMMENTS |
                                   JSON_ALLOW_NEWLINES_IN_STRINGS |
                                   JSON_ALLOW_X_ESCAPES,
};

class JSONReader {
 public:
  static constexpr size_t kAbsoluteMaxDepth = 200;
  JSONReader() = delete;
  static std::optional<Value> Read(
      std::string_view json,
      int options = JSON_PARSE_CHROMIUM_EXTENSIONS,
      size_t max_depth = kAbsoluteMaxDepth);
  static std::optional<DictValue> ReadDict(
      std::string_view json,
      int options = JSON_PARSE_CHROMIUM_EXTENSIONS,
      size_t max_depth = kAbsoluteMaxDepth);
  static std::optional<ListValue> ReadList(
      std::string_view json,
      int options = JSON_PARSE_CHROMIUM_EXTENSIONS,
      size_t max_depth = kAbsoluteMaxDepth);
};

}  // namespace base

#endif  // BASE_JSON_JSON_READER_H_

// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_JSON_JSON_WRITER_H_
#define BASE_JSON_JSON_WRITER_H_

#include <cstddef>
#include <optional>
#include <string>

#include "base/values.h"

namespace base {

enum JsonOptions {
  OPTIONS_OMIT_BINARY_VALUES = 1 << 0,
  OPTIONS_OMIT_DOUBLE_TYPE_PRESERVATION = 1 << 1,
  OPTIONS_PRETTY_PRINT = 1 << 2,
};

std::optional<std::string> WriteJson(ValueView node, size_t max_depth = 200);
std::optional<std::string> WriteJsonWithOptions(ValueView node,
                                                int options,
                                                size_t max_depth = 200);

class JSONWriter {
 public:
  enum Options {
    OPTIONS_OMIT_BINARY_VALUES = base::OPTIONS_OMIT_BINARY_VALUES,
    OPTIONS_OMIT_DOUBLE_TYPE_PRESERVATION =
        base::OPTIONS_OMIT_DOUBLE_TYPE_PRESERVATION,
    OPTIONS_PRETTY_PRINT = base::OPTIONS_PRETTY_PRINT,
  };
  static bool Write(ValueView node, std::string* json);
  static bool WriteWithOptions(ValueView node, int options, std::string* json);
};

}  // namespace base

#endif  // BASE_JSON_JSON_WRITER_H_

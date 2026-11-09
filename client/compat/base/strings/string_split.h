// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_STRINGS_STRING_SPLIT_H_
#define BASE_STRINGS_STRING_SPLIT_H_

#include <string_view>
#include <vector>

namespace base {

enum WhitespaceHandling { KEEP_WHITESPACE, TRIM_WHITESPACE };
enum SplitResult { SPLIT_WANT_ALL, SPLIT_WANT_NONEMPTY };

std::vector<std::string_view> SplitStringPiece(std::string_view input,
                                               std::string_view separators,
                                               WhitespaceHandling whitespace,
                                               SplitResult result_type);

}  // namespace base

#endif  // BASE_STRINGS_STRING_SPLIT_H_

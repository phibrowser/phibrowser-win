// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_STRINGS_STRING_UTIL_H_
#define BASE_STRINGS_STRING_UTIL_H_

#include <string>
#include <string_view>

namespace base {

enum TrimPositions {
  TRIM_NONE = 0,
  TRIM_LEADING = 1 << 0,
  TRIM_TRAILING = 1 << 1,
  TRIM_ALL = TRIM_LEADING | TRIM_TRAILING,
};

std::string_view TrimWhitespaceASCII(std::string_view input,
                                     TrimPositions positions);
bool TrimWhitespaceASCII(std::string_view input,
                         TrimPositions positions,
                         std::string* output);
std::wstring ToLowerASCII(std::wstring_view text);
std::string ToLowerASCII(std::string_view text);
int CompareCaseInsensitiveASCII(std::string_view a, std::string_view b);

}  // namespace base

#endif  // BASE_STRINGS_STRING_UTIL_H_

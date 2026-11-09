// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_STRINGS_UTF_STRING_CONVERSIONS_H_
#define BASE_STRINGS_UTF_STRING_CONVERSIONS_H_

#include <string>
#include <string_view>

namespace base {

std::wstring UTF8ToWide(std::string_view utf8);
std::string WideToUTF8(std::wstring_view wide);
std::wstring ASCIIToWide(std::string_view ascii);

}  // namespace base

#endif  // BASE_STRINGS_UTF_STRING_CONVERSIONS_H_

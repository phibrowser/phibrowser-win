// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_STRINGS_ESCAPE_H_
#define BASE_STRINGS_ESCAPE_H_

#include <string>
#include <string_view>

namespace base {

std::string EscapeForHTML(std::string_view text);
std::string UnescapeBinaryURLComponent(std::string_view escaped);

}  // namespace base

#endif  // BASE_STRINGS_ESCAPE_H_

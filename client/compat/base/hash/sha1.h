// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_HASH_SHA1_H_
#define BASE_HASH_SHA1_H_

#include <string>
#include <string_view>

namespace base {

std::string SHA1HashString(std::string_view text);

}  // namespace base

#endif  // BASE_HASH_SHA1_H_

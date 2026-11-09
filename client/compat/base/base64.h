// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_BASE64_H_
#define BASE_BASE64_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"

namespace base {

std::string Base64Encode(base::span<const uint8_t> input);
std::string Base64Encode(std::string_view input);
bool Base64Decode(std::string_view input, std::string* output);

}  // namespace base

#endif  // BASE_BASE64_H_

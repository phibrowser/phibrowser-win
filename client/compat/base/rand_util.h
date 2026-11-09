// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_RAND_UTIL_H_
#define BASE_RAND_UTIL_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace base {

void RandBytes(void* output, size_t length);
std::vector<uint8_t> RandBytesAsVector(size_t length);
// A uniform value in [0, range).
uint64_t RandGenerator(uint64_t range);

}  // namespace base

#endif  // BASE_RAND_UTIL_H_

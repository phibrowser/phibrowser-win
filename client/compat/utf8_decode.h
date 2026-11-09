// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef CLIENT_COMPAT_UTF8_DECODE_H_
#define CLIENT_COMPAT_UTF8_DECODE_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace base::internal {

// Reads one code point at `*index` and advances past it. An ill-formed sequence
// returns -1 and advances past its longest valid prefix, the rule ICU and Rust
// share, so one bad sequence becomes one replacement character.
inline int32_t NextUtf8(std::string_view text, size_t* index) {
  const unsigned char lead = static_cast<unsigned char>(text[*index]);
  ++*index;
  if (lead < 0x80) {
    return lead;
  }
  size_t trail_count;
  unsigned char low = 0x80;
  unsigned char high = 0xBF;
  int32_t code_point;
  if (lead >= 0xC2 && lead <= 0xDF) {
    trail_count = 1;
    code_point = lead & 0x1F;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    trail_count = 2;
    code_point = lead & 0x0F;
    if (lead == 0xE0) low = 0xA0;
    if (lead == 0xED) high = 0x9F;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    trail_count = 3;
    code_point = lead & 0x07;
    if (lead == 0xF0) low = 0x90;
    if (lead == 0xF4) high = 0x8F;
  } else {
    return -1;
  }
  for (size_t i = 0; i < trail_count; ++i) {
    if (*index >= text.size()) {
      return -1;
    }
    const unsigned char next = static_cast<unsigned char>(text[*index]);
    if (next < low || next > high) {
      return -1;
    }
    code_point = (code_point << 6) | (next & 0x3F);
    ++*index;
    low = 0x80;
    high = 0xBF;
  }
  return code_point;
}

}  // namespace base::internal

#endif  // CLIENT_COMPAT_UTF8_DECODE_H_

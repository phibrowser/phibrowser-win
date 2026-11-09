// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_STRINGS_STRING_NUMBER_CONVERSIONS_H_
#define BASE_STRINGS_STRING_NUMBER_CONVERSIONS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "base/containers/span.h"

namespace base {

std::string NumberToString(int value);
std::string NumberToString(unsigned value);
std::string NumberToString(long value);
std::string NumberToString(unsigned long value);
std::string NumberToString(long long value);
std::string NumberToString(unsigned long long value);
std::string NumberToString(double value);
std::wstring NumberToWString(int value);
std::wstring NumberToWString(unsigned value);
std::wstring NumberToWString(long value);
std::wstring NumberToWString(unsigned long value);
std::wstring NumberToWString(long long value);
std::wstring NumberToWString(unsigned long long value);
std::wstring NumberToWString(double value);

bool StringToInt(std::string_view input, int* output);
bool StringToInt(std::wstring_view input, int* output);
bool StringToInt64(std::string_view input, int64_t* output);
bool StringToInt64(std::wstring_view input, int64_t* output);
bool HexStringToUInt(std::string_view input, uint32_t* output);
std::string HexEncodeLower(base::span<const uint8_t> bytes);

}  // namespace base

#endif  // BASE_STRINGS_STRING_NUMBER_CONVERSIONS_H_

// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.
//
// The rest of the small base:: layer the client uses: text, numbers, time,
// randomness, the command line and logging.

#include <windows.h>

#include <bcrypt.h>
#include <shellapi.h>

#include <algorithm>
#include <charconv>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <mutex>
#include <vector>

#include "base/base64.h"
#include "base/command_line.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/hash/sha1.h"
#include "base/logging.h"
#include "base/logging/logging_settings.h"
#include "base/path_service.h"
#include "base/rand_util.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "base/win/registry.h"
#include "base/win/windows_version.h"
#include "third_party/boringssl/src/include/openssl/base64.h"
#include "third_party/boringssl/src/include/openssl/sha.h"
#include "utf8_decode.h"

namespace base {

// --- UTF conversion --------------------------------------------------------

std::wstring UTF8ToWide(std::string_view utf8) {
  std::wstring out;
  out.reserve(utf8.size());
  size_t i = 0;
  while (i < utf8.size()) {
    int32_t code_point = internal::NextUtf8(utf8, &i);
    if (code_point < 0) {
      code_point = 0xFFFD;
    }
    if (code_point >= 0x10000) {
      code_point -= 0x10000;
      out.push_back(static_cast<wchar_t>(0xD800 + (code_point >> 10)));
      out.push_back(static_cast<wchar_t>(0xDC00 + (code_point & 0x3FF)));
    } else {
      out.push_back(static_cast<wchar_t>(code_point));
    }
  }
  return out;
}

std::string WideToUTF8(std::wstring_view wide) {
  std::string out;
  out.reserve(wide.size());
  for (size_t i = 0; i < wide.size(); ++i) {
    uint32_t code_point = static_cast<uint16_t>(wide[i]);
    if (code_point >= 0xD800 && code_point <= 0xDBFF && i + 1 < wide.size() &&
        wide[i + 1] >= 0xDC00 && wide[i + 1] <= 0xDFFF) {
      code_point = 0x10000 + ((code_point - 0xD800) << 10) +
                   (static_cast<uint16_t>(wide[i + 1]) - 0xDC00);
      ++i;
    } else if (code_point >= 0xD800 && code_point <= 0xDFFF) {
      code_point = 0xFFFD;
    }
    if (code_point < 0x80) {
      out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
      out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
      out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
  }
  return out;
}

std::wstring ASCIIToWide(std::string_view ascii) {
  return std::wstring(ascii.begin(), ascii.end());
}

// --- Numbers ---------------------------------------------------------------

namespace {

template <typename T>
std::string IntegerToString(T value) {
  char buffer[32];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return std::string(buffer, result.ptr);
}

template <typename T>
std::wstring IntegerToWString(T value) {
  const std::string text = IntegerToString(value);
  return std::wstring(text.begin(), text.end());
}

// base's rules: *output always gets the best value read so far; leading space,
// junk, or overflow (which saturates) report false.
template <typename T>
bool ParseInteger(std::string_view input, int radix, T* output) {
  size_t i = 0;
  bool valid = true;
  *output = 0;
  while (i < input.size() &&
         std::string_view(" \t\n\v\f\r").find(input[i]) !=
             std::string_view::npos) {
    valid = false;
    ++i;
  }
  bool negative = false;
  if (i < input.size() && input[i] == '-') {
    if (!std::is_signed_v<T>) {
      return false;
    }
    negative = true;
    ++i;
  } else if (i < input.size() && input[i] == '+') {
    ++i;
  }
  if (radix == 16 && input.size() - i > 2 && input[i] == '0' &&
      (input[i + 1] == 'x' || input[i + 1] == 'X')) {
    i += 2;
  }
  if (i == input.size()) {
    return false;
  }
  T value = 0;
  for (; i < input.size(); ++i) {
    const char c = input[i];
    int digit;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (radix == 16 && c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (radix == 16 && c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else {
      *output = value;
      return false;
    }
    if (digit >= radix) {
      *output = value;
      return false;
    }
    const T min = std::numeric_limits<T>::min();
    const T max = std::numeric_limits<T>::max();
    const T step = static_cast<T>(digit);
    const T base_value = static_cast<T>(radix);
    if (negative ? value < static_cast<T>((min + step) / base_value)
                 : value > static_cast<T>((max - step) / base_value)) {
      *output = negative ? min : max;
      return false;
    }
    value = negative ? static_cast<T>(value * base_value - step)
                     : static_cast<T>(value * base_value + step);
  }
  *output = value;
  return valid;
}

std::string Narrow(std::wstring_view input) {
  std::string out;
  for (wchar_t c : input) {
    out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  }
  return out;
}

}  // namespace

std::string NumberToString(int value) { return IntegerToString(value); }
std::string NumberToString(unsigned value) { return IntegerToString(value); }
std::string NumberToString(long value) { return IntegerToString(value); }
std::string NumberToString(unsigned long value) { return IntegerToString(value); }
std::string NumberToString(long long value) { return IntegerToString(value); }
std::string NumberToString(unsigned long long value) { return IntegerToString(value); }
std::wstring NumberToWString(int value) { return IntegerToWString(value); }
std::wstring NumberToWString(unsigned value) { return IntegerToWString(value); }
std::wstring NumberToWString(long value) { return IntegerToWString(value); }
std::wstring NumberToWString(unsigned long value) { return IntegerToWString(value); }
std::wstring NumberToWString(long long value) { return IntegerToWString(value); }
std::wstring NumberToWString(unsigned long long value) { return IntegerToWString(value); }

// The shortest round-tripping digits, laid out the way base's converter does:
// plain decimals for exponents -6 < e <= 12, otherwise "1.5e+21".
std::string NumberToString(double value) {
  if (value == 0) {
    return std::signbit(value) ? "-0" : "0";
  }
  char buffer[64];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                    std::chars_format::scientific);
  std::string_view text(buffer, result.ptr - buffer);
  const bool negative = text[0] == '-';
  if (negative) {
    text.remove_prefix(1);
  }
  const size_t e = text.find('e');
  std::string digits(text.substr(0, 1));
  if (e > 2) {
    digits += text.substr(2, e - 2);
  }
  int exponent = 0;
  std::string_view exponent_text = text.substr(e + 1);
  if (exponent_text[0] == '+') {
    exponent_text.remove_prefix(1);
  }
  std::from_chars(exponent_text.data(),
                  exponent_text.data() + exponent_text.size(), exponent);
  const int decimal_point = exponent + 1;
  std::string out = negative ? "-" : "";
  if (decimal_point > -6 && decimal_point <= 12) {
    if (decimal_point <= 0) {
      out += "0.";
      out.append(static_cast<size_t>(-decimal_point), '0');
      out += digits;
    } else if (static_cast<size_t>(decimal_point) >= digits.size()) {
      out += digits;
      out.append(static_cast<size_t>(decimal_point) - digits.size(), '0');
    } else {
      out += digits.substr(0, decimal_point);
      out += '.';
      out += digits.substr(decimal_point);
    }
    return out;
  }
  out += digits[0];
  if (digits.size() > 1) {
    out += '.';
    out += digits.substr(1);
  }
  out += 'e';
  out += exponent < 0 ? '-' : '+';
  out += std::to_string(std::abs(exponent));
  return out;
}

std::wstring NumberToWString(double value) {
  const std::string text = NumberToString(value);
  return std::wstring(text.begin(), text.end());
}

bool StringToInt(std::string_view input, int* output) {
  return ParseInteger(input, 10, output);
}
bool StringToInt(std::wstring_view input, int* output) {
  return ParseInteger(Narrow(input), 10, output);
}
bool StringToInt64(std::string_view input, int64_t* output) {
  return ParseInteger(input, 10, output);
}
bool StringToInt64(std::wstring_view input, int64_t* output) {
  return ParseInteger(Narrow(input), 10, output);
}
bool HexStringToUInt(std::string_view input, uint32_t* output) {
  return ParseInteger(input, 16, output);
}

std::string HexEncodeLower(base::span<const uint8_t> bytes) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (uint8_t byte : bytes) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0xF]);
  }
  return out;
}

// --- Text ------------------------------------------------------------------

std::string_view TrimWhitespaceASCII(std::string_view input,
                                     TrimPositions positions) {
  static constexpr std::string_view kWhitespace = "\t\n\v\f\r ";
  if (positions & TRIM_LEADING) {
    const size_t first = input.find_first_not_of(kWhitespace);
    input.remove_prefix(first == std::string_view::npos ? input.size() : first);
  }
  if (positions & TRIM_TRAILING) {
    const size_t last = input.find_last_not_of(kWhitespace);
    input = input.substr(0, last == std::string_view::npos ? 0 : last + 1);
  }
  return input;
}

bool TrimWhitespaceASCII(std::string_view input,
                         TrimPositions positions,
                         std::string* output) {
  const std::string_view trimmed = TrimWhitespaceASCII(input, positions);
  const bool changed = trimmed.size() != input.size();
  *output = std::string(trimmed);
  return changed;
}

template <typename Char>
static Char ToLowerChar(Char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<Char>(c + ('a' - 'A')) : c;
}

std::wstring ToLowerASCII(std::wstring_view text) {
  std::wstring out(text);
  for (wchar_t& c : out) {
    c = ToLowerChar(c);
  }
  return out;
}

std::string ToLowerASCII(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = ToLowerChar(c);
  }
  return out;
}

int CompareCaseInsensitiveASCII(std::string_view a, std::string_view b) {
  const size_t common = std::min(a.size(), b.size());
  for (size_t i = 0; i < common; ++i) {
    const unsigned char left = ToLowerChar(a[i]);
    const unsigned char right = ToLowerChar(b[i]);
    if (left != right) {
      return left < right ? -1 : 1;
    }
  }
  return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

std::string StringPrintf(const char* format, ...) {
  va_list args;
  va_start(args, format);
  va_list copy;
  va_copy(copy, args);
  const int length = vsnprintf(nullptr, 0, format, copy);
  va_end(copy);
  std::string out(length > 0 ? static_cast<size_t>(length) : 0, '\0');
  if (length > 0) {
    vsnprintf(out.data(), out.size() + 1, format, args);
  }
  va_end(args);
  return out;
}

std::vector<std::string_view> SplitStringPiece(std::string_view input,
                                               std::string_view separators,
                                               WhitespaceHandling whitespace,
                                               SplitResult result_type) {
  std::vector<std::string_view> out;
  if (input.empty()) {
    return out;
  }
  size_t start = 0;
  while (start != std::string_view::npos) {
    const size_t end = input.find_first_of(separators, start);
    std::string_view piece =
        end == std::string_view::npos
            ? input.substr(start)
            : input.substr(start, end - start);
    start = end == std::string_view::npos ? end : end + 1;
    if (whitespace == TRIM_WHITESPACE) {
      piece = TrimWhitespaceASCII(piece, TRIM_ALL);
    }
    if (result_type == SPLIT_WANT_ALL || !piece.empty()) {
      out.push_back(piece);
    }
  }
  return out;
}

std::string EscapeForHTML(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    switch (c) {
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::string UnescapeBinaryURLComponent(std::string_view escaped) {
  const auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(escaped.size());
  for (size_t i = 0; i < escaped.size(); ++i) {
    if (escaped[i] == '%' && i + 2 < escaped.size() + 0 &&
        hex(escaped[i + 1]) >= 0 && hex(escaped[i + 2]) >= 0) {
      out.push_back(static_cast<char>(hex(escaped[i + 1]) * 16 +
                                      hex(escaped[i + 2])));
      i += 2;
    } else {
      out.push_back(escaped[i]);
    }
  }
  return out;
}

// --- Encoding and hashing --------------------------------------------------

std::string Base64Encode(base::span<const uint8_t> input) {
  size_t length = 0;
  if (!EVP_EncodedLength(&length, input.size())) {
    return std::string();
  }
  std::string out(length, '\0');
  const size_t written = EVP_EncodeBlock(
      reinterpret_cast<uint8_t*>(out.data()), input.data(), input.size());
  out.resize(written);
  return out;
}

std::string Base64Encode(std::string_view input) {
  return Base64Encode(as_byte_span(input));
}

bool Base64Decode(std::string_view input, std::string* output) {
  size_t length = 0;
  if (!EVP_DecodedLength(&length, input.size())) {
    return false;
  }
  std::string out(length, '\0');
  size_t written = 0;
  if (!EVP_DecodeBase64(reinterpret_cast<uint8_t*>(out.data()), &written,
                        length,
                        reinterpret_cast<const uint8_t*>(input.data()),
                        input.size())) {
    return false;
  }
  out.resize(written);
  *output = std::move(out);
  return true;
}

std::string SHA1HashString(std::string_view text) {
  std::string out(SHA_DIGEST_LENGTH, '\0');
  SHA1(reinterpret_cast<const uint8_t*>(text.data()), text.size(),
       reinterpret_cast<uint8_t*>(out.data()));
  return out;
}

// --- Randomness ------------------------------------------------------------

void RandBytes(void* output, size_t length) {
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, static_cast<PUCHAR>(output),
                                      static_cast<ULONG>(length),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    __fastfail(0);
  }
}

std::vector<uint8_t> RandBytesAsVector(size_t length) {
  std::vector<uint8_t> out(length);
  RandBytes(out.data(), out.size());
  return out;
}

uint64_t RandGenerator(uint64_t range) {
  // Rejects the short top bucket so every value is equally likely.
  const uint64_t limit = UINT64_MAX - (UINT64_MAX % range + 1) % range;
  uint64_t value;
  do {
    RandBytes(&value, sizeof(value));
  } while (value > limit);
  return value % range;
}

Uuid Uuid::GenerateRandomV4() {
  uint8_t bytes[16];
  RandBytes(bytes, sizeof(bytes));
  bytes[6] = (bytes[6] & 0x0F) | 0x40;
  bytes[8] = (bytes[8] & 0x3F) | 0x80;
  const std::string hex = HexEncodeLower(base::span<const uint8_t>(bytes, 16));
  Uuid uuid;
  uuid.text_ = hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" +
               hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" +
               hex.substr(20);
  return uuid;
}

// --- Time ------------------------------------------------------------------

namespace {
constexpr int64_t kFileTimeToUnixEpochUs = 11644473600000000;
}  // namespace

Time Time::Now() {
  FILETIME now;
  ::GetSystemTimePreciseAsFileTime(&now);
  ULARGE_INTEGER ticks;
  ticks.LowPart = now.dwLowDateTime;
  ticks.HighPart = now.dwHighDateTime;
  Time time;
  time.us_since_unix_epoch_ =
      static_cast<int64_t>(ticks.QuadPart / 10) - kFileTimeToUnixEpochUs;
  return time;
}

bool Time::FromUTCExploded(const Exploded& exploded, Time* time) {
  SYSTEMTIME st = {};
  st.wYear = static_cast<WORD>(exploded.year);
  st.wMonth = static_cast<WORD>(exploded.month);
  st.wDay = static_cast<WORD>(exploded.day_of_month);
  st.wHour = static_cast<WORD>(exploded.hour);
  st.wMinute = static_cast<WORD>(exploded.minute);
  st.wSecond = static_cast<WORD>(exploded.second);
  st.wMilliseconds = static_cast<WORD>(exploded.millisecond);
  FILETIME file_time;
  if (!::SystemTimeToFileTime(&st, &file_time)) {
    *time = Time();
    return false;
  }
  ULARGE_INTEGER ticks;
  ticks.LowPart = file_time.dwLowDateTime;
  ticks.HighPart = file_time.dwHighDateTime;
  time->us_since_unix_epoch_ =
      static_cast<int64_t>(ticks.QuadPart / 10) - kFileTimeToUnixEpochUs;
  return true;
}

int64_t Time::InMillisecondsSinceUnixEpoch() const {
  return us_since_unix_epoch_ / 1000;
}

double Time::InSecondsFSinceUnixEpoch() const {
  return us_since_unix_epoch_ / 1000000.0;
}

// --- Files and paths -------------------------------------------------------

bool FilePath::IsAbsolute() const {
  if (path_.size() >= 3 && path_[1] == L':' &&
      (path_[2] == L'\\' || path_[2] == L'/')) {
    return true;
  }
  return path_.size() >= 2 && (path_[0] == L'\\' || path_[0] == L'/') &&
         (path_[1] == L'\\' || path_[1] == L'/');
}

FilePath FilePath::Append(StringViewType component) const {
  FilePath out = *this;
  if (!out.path_.empty() && out.path_.back() != L'\\' &&
      out.path_.back() != L'/') {
    out.path_.push_back(L'\\');
  }
  out.path_ += component;
  return out;
}

FilePath FilePath::AppendASCII(std::string_view component) const {
  return Append(UTF8ToWide(component));
}

FilePath FilePath::NormalizePathSeparators() const {
  FilePath out = *this;
  std::replace(out.path_.begin(), out.path_.end(), L'/', L'\\');
  return out;
}

std::string FilePath::AsUTF8Unsafe() const { return WideToUTF8(path_); }

std::ostream& operator<<(std::ostream& out, const FilePath& path) {
  return out << WideToUTF8(path.value());
}

bool ReadFileToString(const FilePath& path, std::string* contents) {
  HANDLE file = ::CreateFileW(path.value().c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  contents->clear();
  char buffer[16 * 1024];
  DWORD read = 0;
  bool ok = true;
  while ((ok = ::ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) &&
         read > 0) {
    contents->append(buffer, read);
  }
  ::CloseHandle(file);
  return ok;
}

std::optional<std::vector<uint8_t>> ReadFileToBytes(const FilePath& path) {
  std::string contents;
  if (!ReadFileToString(path, &contents)) {
    return std::nullopt;
  }
  return std::vector<uint8_t>(contents.begin(), contents.end());
}

bool PathService::Get(int key, FilePath* path) {
  if (key != DIR_EXE) {
    return false;
  }
  std::wstring buffer(32768, L'\0');
  const DWORD length =
      ::GetModuleFileNameW(nullptr, buffer.data(),
                           static_cast<DWORD>(buffer.size()));
  if (!length || length >= buffer.size()) {
    return false;
  }
  buffer.resize(length);
  buffer.resize(buffer.find_last_of(L'\\'));
  *path = FilePath(buffer);
  return true;
}

std::optional<std::string> Environment::GetVar(std::string_view name) {
  const std::wstring wide_name = UTF8ToWide(name);
  const DWORD length = ::GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
  if (!length) {
    return std::nullopt;
  }
  std::wstring value(length, L'\0');
  const DWORD written =
      ::GetEnvironmentVariableW(wide_name.c_str(), value.data(), length);
  value.resize(written);
  return WideToUTF8(value);
}

// --- Command line ----------------------------------------------------------

namespace {
CommandLine* g_command_line = nullptr;

// "--", "-" and "/" start a switch on Windows; keys are lower-cased.
void ParseSwitch(std::wstring_view arg,
                 std::map<std::string, std::wstring, std::less<>>* out) {
  size_t prefix = 0;
  if (arg.substr(0, 2) == L"--") {
    prefix = 2;
  } else if (!arg.empty() && (arg[0] == L'-' || arg[0] == L'/')) {
    prefix = 1;
  } else {
    return;
  }
  arg.remove_prefix(prefix);
  const size_t equals = arg.find(L'=');
  const std::wstring_view key =
      equals == std::wstring_view::npos ? arg : arg.substr(0, equals);
  if (key.empty()) {
    return;
  }
  (*out)[ToLowerASCII(WideToUTF8(key))] =
      equals == std::wstring_view::npos ? std::wstring()
                                        : std::wstring(arg.substr(equals + 1));
}
}  // namespace

void CommandLine::Init(int, const char* const*) {
  if (g_command_line) {
    return;
  }
  g_command_line = new CommandLine();
  int argc = 0;
  wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  for (int i = 1; argv && i < argc; ++i) {
    if (std::wstring_view(argv[i]) == L"--") {
      break;
    }
    ParseSwitch(argv[i], &g_command_line->switches_);
  }
  if (argv) {
    ::LocalFree(argv);
  }
}

CommandLine* CommandLine::ForCurrentProcess() { return g_command_line; }

bool CommandLine::HasSwitch(std::string_view name) const {
  return switches_.find(name) != switches_.end();
}

std::string CommandLine::GetSwitchValueASCII(std::string_view name) const {
  return WideToUTF8(GetSwitchValueNative(name));
}

std::wstring CommandLine::GetSwitchValueNative(std::string_view name) const {
  auto it = switches_.find(name);
  return it == switches_.end() ? std::wstring() : it->second;
}

void CommandLine::AppendSwitch(std::string_view name) {
  switches_[std::string(name)] = std::wstring();
}

void CommandLine::AppendSwitchASCII(std::string_view name,
                                    std::string_view value) {
  switches_[std::string(name)] = UTF8ToWide(value);
}

// --- Windows helpers -------------------------------------------------------

namespace win {

OSInfo* OSInfo::GetInstance() {
  static OSInfo* instance = new OSInfo();
  return instance;
}

OSInfo::OSInfo() {
  OSVERSIONINFOW info = {};
  info.dwOSVersionInfoSize = sizeof(info);
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  if (const auto get = reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(
          ::GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) {
    get(&info);
  }
  version_.major = static_cast<int>(info.dwMajorVersion);
  version_.minor = static_cast<int>(info.dwMinorVersion);
  version_.build = static_cast<int>(info.dwBuildNumber);
  HKEY key;
  if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                      KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
    DWORD ubr = 0;
    DWORD size = sizeof(ubr);
    if (::RegQueryValueExW(key, L"UBR", nullptr, nullptr,
                           reinterpret_cast<BYTE*>(&ubr),
                           &size) == ERROR_SUCCESS) {
      version_.patch = static_cast<int>(ubr);
    }
    ::RegCloseKey(key);
  }
}

RegKey::RegKey(HKEY root, const wchar_t* subkey, REGSAM access) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(root, subkey, 0, access, &key) == ERROR_SUCCESS) {
    key_ = key;
  }
}

RegKey::~RegKey() {
  if (key_) {
    ::RegCloseKey(key_);
  }
}

LONG RegKey::ReadValue(const wchar_t* name, std::wstring* out) const {
  DWORD size = 0;
  DWORD type = 0;
  LONG result = ::RegQueryValueExW(key_, name, nullptr, &type, nullptr, &size);
  if (result != ERROR_SUCCESS) {
    return result;
  }
  if (type != REG_SZ && type != REG_EXPAND_SZ) {
    return ERROR_CANTREAD;
  }
  std::wstring value(size / sizeof(wchar_t), L'\0');
  result = ::RegQueryValueExW(key_, name, nullptr, nullptr,
                              reinterpret_cast<BYTE*>(value.data()), &size);
  if (result == ERROR_SUCCESS) {
    value.resize(wcsnlen(value.c_str(), value.size()));
    *out = std::move(value);
  }
  return result;
}

LONG RegKey::ReadValueDW(const wchar_t* name, DWORD* out) const {
  DWORD size = sizeof(DWORD);
  DWORD type = 0;
  const LONG result = ::RegQueryValueExW(key_, name, nullptr, &type,
                                         reinterpret_cast<BYTE*>(out), &size);
  return result == ERROR_SUCCESS && type != REG_DWORD ? ERROR_CANTREAD : result;
}

LONG RegKey::WriteValue(const wchar_t* name, const wchar_t* value) {
  return ::RegSetValueExW(
      key_, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
      static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
}

LONG RegKey::WriteValue(const wchar_t* name, DWORD value) {
  return ::RegSetValueExW(key_, name, 0, REG_DWORD,
                          reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

}  // namespace win
}  // namespace base

// --- Logging ---------------------------------------------------------------

namespace logging {

namespace {

std::mutex& LogMutex() {
  static std::mutex* mutex = new std::mutex();
  return *mutex;
}

uint32_t g_destination = LOG_TO_SYSTEM_DEBUG_LOG | LOG_TO_STDERR;
HANDLE g_file = INVALID_HANDLE_VALUE;
int g_min_level = 0;
bool g_process_id = false;
bool g_thread_id = false;
bool g_timestamp = false;

constexpr const char* kSeverityNames[] = {"INFO", "WARNING", "ERROR", "FATAL"};

}  // namespace

bool ShouldLog(LogSeverity severity) { return severity >= g_min_level; }

std::ostream& NullStream() {
  static std::ostringstream* stream = new std::ostringstream();
  return *stream;
}

void SetMinLogLevel(int level) { g_min_level = level; }

bool InitLogging(const LoggingSettings& settings) {
  std::lock_guard<std::mutex> hold(LogMutex());
  if (g_file != INVALID_HANDLE_VALUE) {
    ::CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
  }
  g_destination = settings.logging_dest;
  if (!(g_destination & LOG_TO_FILE)) {
    return true;
  }
  // FILE_APPEND_DATA makes each write an atomic append, so these lines
  // interleave with the framework's without tearing.
  g_file = ::CreateFileW(settings.log_file_path.c_str(), FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  return g_file != INVALID_HANDLE_VALUE;
}

void SetLogItems(bool enable_process_id,
                 bool enable_thread_id,
                 bool enable_timestamp,
                 bool) {
  g_process_id = enable_process_id;
  g_thread_id = enable_thread_id;
  g_timestamp = enable_timestamp;
}

LogMessage::LogMessage(const char* file, int line, LogSeverity severity)
    : file_(file), line_(line), severity_(severity) {
  std::ostream& out = stream_;
  out << '[';
  char buffer[32];
  if (g_process_id) {
    out << ::GetCurrentProcessId() << ':';
  }
  if (g_thread_id) {
    out << ::GetCurrentThreadId() << ':';
  }
  if (g_timestamp) {
    SYSTEMTIME now;
    ::GetLocalTime(&now);
    snprintf(buffer, sizeof(buffer), "%02d%02d/%02d%02d%02d.%03d:", now.wMonth,
             now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    out << buffer;
  }
  std::string_view path(file_);
  const size_t slash = path.find_last_of("\\/");
  out << kSeverityNames[severity_] << ':'
      << (slash == std::string_view::npos ? path : path.substr(slash + 1))
      << '(' << line_ << ")] ";
}

LogMessage::~LogMessage() {
  stream_ << '\n';
  const std::string text = stream_.str();
  {
    std::lock_guard<std::mutex> hold(LogMutex());
    if (g_destination & LOG_TO_SYSTEM_DEBUG_LOG) {
      ::OutputDebugStringA(text.c_str());
    }
    if (g_destination & LOG_TO_STDERR) {
      fwrite(text.data(), 1, text.size(), stderr);
    }
    if ((g_destination & LOG_TO_FILE) && g_file != INVALID_HANDLE_VALUE) {
      DWORD written;
      ::WriteFile(g_file, text.data(), static_cast<DWORD>(text.size()),
                  &written, nullptr);
    }
  }
  if (severity_ == LOGGING_FATAL) {
    __fastfail(0);
  }
}

[[noreturn]] void CheckFailed(const char* file,
                              int line,
                              const char* what,
                              const std::string& message) {
  {
    LogMessage log(file, line, LOGGING_ERROR);
    log.stream() << "Check failed: " << what << ". " << message;
  }
  __debugbreak();
  __fastfail(0);
}

}  // namespace logging

namespace std {
ostream& operator<<(ostream& out, wstring_view text) {
  return out << base::WideToUTF8(text);
}
}  // namespace std

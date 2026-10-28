// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_util.h"

#include <algorithm>
#include <chrono>
#include <cwctype>

#if defined(_WIN32)
#include "base/strings/utf_string_conversions.h"
#endif

namespace phi_client {

namespace {

constexpr wchar_t kWhitespace[] = L" \t\r\n";
constexpr char kWhitespaceNarrow[] = " \t\r\n";

// The whitespace trim, once, for both widths.
template <typename View>
View TrimView(View text, View whitespace) {
  const size_t begin = text.find_first_not_of(whitespace);
  if (begin == View::npos) {
    return View();
  }
  return text.substr(begin, text.find_last_not_of(whitespace) - begin + 1);
}

}  // namespace

PhiColor PhiBlend(PhiColor over, PhiColor base, int alpha255) {
  const int alpha = PhiClamp(alpha255, 0, 255);
  const auto mix = [alpha](int o, int b) {
    return (o * alpha + b * (255 - alpha)) / 255;
  };
  return PhiRgb(mix(PhiRed(over), PhiRed(base)),
                mix(PhiGreen(over), PhiGreen(base)),
                mix(PhiBlue(over), PhiBlue(base)));
}

PhiColor PhiLerpColor(PhiColor from, PhiColor to, double t) {
  return PhiBlend(to, from, static_cast<int>(PhiClamp01(t) * 255.0 + 0.5));
}

int PhiDip(int dpi, int design) {
  // ::MulDiv's rounding: to nearest, halves away from zero. Reproduced rather
  // than called so a layout that scales stays host-testable.
  if (dpi <= 0) {
    dpi = 96;
  }
  const int64_t product = static_cast<int64_t>(design) * dpi;
  const int64_t half = product < 0 ? -48 : 48;
  return static_cast<int>((product + half) / 96);
}

std::string PhiTrim(std::string_view text) {
  return std::string(TrimView<std::string_view>(text, kWhitespaceNarrow));
}

std::wstring PhiTrim(std::wstring_view text) {
  return std::wstring(TrimView<std::wstring_view>(text, kWhitespace));
}

std::string PhiLower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

std::wstring PhiLower(std::wstring_view text) {
  std::wstring out(text);
  for (wchar_t& c : out) {
    c = static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(c)));
  }
  return out;
}

std::wstring PhiSubstitute(std::wstring_view pattern,
                           const std::vector<std::wstring>& args) {
  // ONE left-to-right pass over the pattern, not one pass per argument. The
  // difference shows when an argument's own text contains a placeholder — a
  // host, a file name, a Space called "{1}" — which a per-argument loop would
  // then substitute into a second time.
  std::wstring out;
  out.reserve(pattern.size() + 16);
  size_t at = 0;
  while (at < pattern.size()) {
    const wchar_t c = pattern[at];
    if (c != L'{') {
      out.push_back(c);
      ++at;
      continue;
    }
    size_t digits = at + 1;
    size_t index = 0;
    while (digits < pattern.size() && pattern[digits] >= L'0' &&
           pattern[digits] <= L'9') {
      index = index * 10 + static_cast<size_t>(pattern[digits] - L'0');
      ++digits;
    }
    const bool is_token = digits > at + 1 && digits < pattern.size() &&
                          pattern[digits] == L'}';
    if (is_token && index < args.size()) {
      out += args[index];
      at = digits + 1;
      continue;
    }
    // Not a placeholder, or a placeholder with no argument. Either way the
    // text stays VISIBLE rather than being dropped, so a mistranslated
    // pattern shows up in a screenshot instead of losing data silently.
    out.push_back(c);
    ++at;
  }
  return out;
}

PhiPlural PhiPluralFor(std::string_view locale, long long n) {
  // The language subtag is what decides; the script and region never do for
  // the categories these ten locales use, with one exception: CLDR splits
  // Portuguese, where Brazil counts zero as singular and Portugal does not.
  const size_t dash = locale.find('-');
  const std::string language = PhiLower(
      dash == std::string_view::npos ? locale : locale.substr(0, dash));
  if (language == "ja" || language == "ko" || language == "zh") {
    return PhiPlural::kOther;  // One form; a count never changes the noun.
  }
  if (language == "fr" || (language == "pt" && PhiLower(locale) != "pt-pt")) {
    return (n == 0 || n == 1) ? PhiPlural::kOne : PhiPlural::kOther;
  }
  return n == 1 ? PhiPlural::kOne : PhiPlural::kOther;
}

const char* PhiPluralSuffix(PhiPlural category) {
  return category == PhiPlural::kOne ? "one" : "other";
}

std::wstring PhiGroupDigits(long long value,
                            std::wstring_view group_sep,
                            int primary,
                            int secondary) {
  const bool negative = value < 0;
  // Built from the least significant digit up, so the negation cannot
  // overflow on the most negative value.
  std::wstring digits;
  unsigned long long magnitude =
      negative ? 0ULL - static_cast<unsigned long long>(value)
               : static_cast<unsigned long long>(value);
  int group = primary > 0 ? primary : 0;
  int since_separator = 0;
  do {
    if (group > 0 && since_separator == group && !group_sep.empty()) {
      digits.append(group_sep.rbegin(), group_sep.rend());
      since_separator = 0;
      group = secondary > 0 ? secondary : group;
    }
    digits.push_back(static_cast<wchar_t>(L'0' + magnitude % 10));
    magnitude /= 10;
    ++since_separator;
  } while (magnitude > 0);
  if (negative) {
    digits.push_back(L'-');
  }
  std::reverse(digits.begin(), digits.end());
  return digits;
}

int64_t PhiNowUnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

PhiRelativeSpan PhiRelativeSpanFor(long long delta_seconds) {
  // The downloads popover's ladder, unchanged except that the caller now
  // gets the COUNT and picks a plural form with it.
  if (delta_seconds < 60) {
    return {PhiRelativeUnit::kJustNow, 0};
  }
  if (delta_seconds < 3600) {
    return {PhiRelativeUnit::kMinutes, delta_seconds / 60};
  }
  if (delta_seconds < 12 * 3600) {
    return {PhiRelativeUnit::kHours, delta_seconds / 3600};
  }
  if (delta_seconds < 3 * 86400) {
    const long long days = delta_seconds / 86400;
    if (days == 0) {
      return {PhiRelativeUnit::kToday, 0};
    }
    if (days == 1) {
      return {PhiRelativeUnit::kYesterday, 0};
    }
    return {PhiRelativeUnit::kDays, days};
  }
  return {PhiRelativeUnit::kAbsolute, 0};
}

#if defined(_WIN32)

std::wstring PhiWiden(const std::string& utf8) {
  return base::UTF8ToWide(utf8);
}

std::string PhiNarrow(const std::wstring& wide) {
  return base::WideToUTF8(wide);
}

#endif  // defined(_WIN32)

}  // namespace phi_client

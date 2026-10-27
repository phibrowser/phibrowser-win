// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_url_rule_normalize.h"

#include <algorithm>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <random>

namespace phi_client::store {

namespace {

struct LowerMapping {
  uint32_t scalar;
  const char* lower;
};

constexpr LowerMapping kLowerTable[] = {
#include "phibrowser-win/client/phi_unicode_lower.inc"
};

// Unicode White_Space (= Foundation's whitespacesAndNewlines).
bool IsWhiteSpace(uint32_t c) {
  return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 ||
         c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
         c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

// Decodes one UTF-8 scalar at `at`; invalid bytes read as themselves (one
// byte, U+FFFD-like) so nothing is ever dropped.
uint32_t DecodeAt(std::string_view s, size_t at, size_t* length) {
  const auto byte = [&](size_t i) {
    return static_cast<unsigned char>(s[i]);
  };
  const unsigned char b0 = byte(at);
  if (b0 < 0x80) {
    *length = 1;
    return b0;
  }
  const size_t need = b0 >= 0xF0 ? 4 : b0 >= 0xE0 ? 3 : b0 >= 0xC0 ? 2 : 0;
  if (need == 0 || at + need > s.size()) {
    *length = 1;
    return 0xFFFD;
  }
  uint32_t cp = b0 & (need == 2 ? 0x1F : need == 3 ? 0x0F : 0x07);
  for (size_t i = 1; i < need; ++i) {
    if ((byte(at + i) & 0xC0) != 0x80) {
      *length = 1;
      return 0xFFFD;
    }
    cp = (cp << 6) | (byte(at + i) & 0x3F);
  }
  *length = need;
  return cp;
}

bool ValidUtf8(std::string_view s) {
  size_t at = 0;
  while (at < s.size()) {
    const unsigned char b0 = static_cast<unsigned char>(s[at]);
    size_t len = 0;
    const uint32_t cp = DecodeAt(s, at, &len);
    if (b0 >= 0x80 && (cp == 0xFFFD && len == 1)) {
      return false;
    }
    // Overlong or surrogate forms are invalid too.
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
        (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
      return false;
    }
    at += len;
  }
  return true;
}

std::string UnicodeLower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  size_t at = 0;
  while (at < s.size()) {
    size_t len = 0;
    const uint32_t cp = DecodeAt(s, at, &len);
    const auto* end = std::end(kLowerTable);
    const auto* hit = std::lower_bound(
        std::begin(kLowerTable), end, cp,
        [](const LowerMapping& m, uint32_t v) { return m.scalar < v; });
    if (hit != end && hit->scalar == cp) {
      out += hit->lower;
    } else {
      out.append(s.substr(at, len));
    }
    at += len;
  }
  return out;
}

// Trims White_Space scalars from both ends (and, with `dots`, '.' from the
// right edge in the same scan, as normalizedHost does).
std::string_view TrimWhiteSpace(std::string_view s, bool trailing_dots) {
  size_t begin = 0;
  while (begin < s.size()) {
    size_t len = 0;
    const uint32_t cp = DecodeAt(s, begin, &len);
    if (!IsWhiteSpace(cp)) {
      break;
    }
    begin += len;
  }
  size_t end = s.size();
  while (end > begin) {
    // Step back one scalar.
    size_t start = end - 1;
    while (start > begin &&
           (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) {
      --start;
    }
    size_t len = 0;
    const uint32_t cp = DecodeAt(s, start, &len);
    if (!(IsWhiteSpace(cp) || (trailing_dots && cp == '.'))) {
      break;
    }
    end = start;
  }
  return s.substr(begin, end - begin);
}

// String.removingPercentEncoding: nil for a malformed escape or bytes that
// are not UTF-8.
std::optional<std::string> RemovingPercentEncoding(std::string_view s) {
  const auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  };
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '%') {
      out.push_back(s[i]);
      continue;
    }
    if (i + 2 >= s.size()) {
      return std::nullopt;
    }
    const int hi = hex(s[i + 1]);
    const int lo = hex(s[i + 2]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    out.push_back(static_cast<char>(hi * 16 + lo));
    i += 2;
  }
  if (!ValidUtf8(out)) {
    return std::nullopt;
  }
  return out;
}

// CharacterSet.urlPathAllowed. Note ';' is NOT in it (the Mac encodes it
// as %3B; the vectors pin that).
bool UrlPathAllowed(unsigned char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9')) {
    return true;
  }
  switch (c) {
    case '!': case '$': case '&': case '\'': case '(': case ')': case '*':
    case '+': case ',': case '-': case '.': case '/': case ':': case '=':
    case '@': case '_': case '~':
      return true;
    default:
      return false;
  }
}

std::string AddingPathPercentEncoding(std::string_view s) {
  constexpr std::string_view kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (UrlPathAllowed(c)) {
      out.push_back(ch);
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0xF]);
    }
  }
  return out;
}

// RFC 3492 §6.1 bias adaptation.
uint32_t PunycodeAdapt(uint32_t delta, uint32_t points, bool first) {
  constexpr uint32_t kBase = 36, kTMin = 1, kTMax = 26, kSkew = 38,
                     kDamp = 700;
  delta = first ? delta / kDamp : delta / 2;
  delta += delta / points;
  uint32_t k = 0;
  while (delta > ((kBase - kTMin) * kTMax) / 2) {
    delta /= kBase - kTMin;
    k += kBase;
  }
  return k + (kBase - kTMin + 1) * delta / (delta + kSkew);
}

// RFC 3492 §6.3 encoding of one label's scalars. False on overflow.
bool PunycodeEncode(const std::u32string& input, std::string* out) {
  constexpr uint32_t kBase = 36, kTMin = 1, kTMax = 26;
  const auto digit = [](uint32_t d) {
    return static_cast<char>(d < 26 ? 'a' + d : '0' + (d - 26));
  };
  uint32_t n = 0x80;
  uint32_t delta = 0;
  uint32_t bias = 72;
  uint32_t basic = 0;
  for (char32_t c : input) {
    if (c < 0x80) {
      out->push_back(static_cast<char>(c));
      ++basic;
    }
  }
  uint32_t handled = basic;
  if (basic > 0) {
    out->push_back('-');
  }
  const uint32_t length = static_cast<uint32_t>(input.size());
  while (handled < length) {
    uint32_t m = UINT32_MAX;
    for (char32_t c : input) {
      if (c >= n && c < m) {
        m = c;
      }
    }
    if (m - n > (UINT32_MAX - delta) / (handled + 1)) {
      return false;
    }
    delta += (m - n) * (handled + 1);
    n = m;
    for (char32_t c : input) {
      if (c < n && ++delta == 0) {
        return false;
      }
      if (c != n) {
        continue;
      }
      uint32_t q = delta;
      for (uint32_t k = kBase;; k += kBase) {
        const uint32_t t =
            k <= bias ? kTMin : (k >= bias + kTMax ? kTMax : k - bias);
        if (q < t) {
          break;
        }
        out->push_back(digit(t + (q - t) % (kBase - t)));
        q = (q - t) / (kBase - t);
      }
      out->push_back(digit(q));
      bias = PunycodeAdapt(delta, handled + 1, handled == basic);
      delta = 0;
      ++handled;
    }
    ++delta;
    ++n;
  }
  return true;
}

// One label of RuleHostForRouting: sigils kept, the core ACE-encoded when it
// is not ASCII. False when it cannot be encoded.
bool AppendRoutingLabel(const std::u32string& label, std::string* out) {
  size_t begin = 0;
  size_t end = label.size();
  while (begin < end && label[begin] == U'*') {
    ++begin;
  }
  while (end > begin && label[end - 1] == U'*') {
    --end;
  }
  out->append(begin, '*');
  const std::u32string core = label.substr(begin, end - begin);
  const bool ascii = std::all_of(core.begin(), core.end(),
                                 [](char32_t c) { return c < 0x80; });
  if (ascii) {
    for (char32_t c : core) {
      out->push_back(static_cast<char>(c));
    }
  } else {
    out->append("xn--");
    if (!PunycodeEncode(core, out)) {
      return false;
    }
  }
  out->append(label.size() - end, '*');
  return true;
}

}  // namespace

std::string RuleHostForRouting(std::string_view host) {
  const bool ascii = std::all_of(host.begin(), host.end(), [](char c) {
    return static_cast<unsigned char>(c) < 0x80;
  });
  if (ascii || !ValidUtf8(host)) {
    return std::string(host);
  }
  std::string out;
  std::u32string label;
  size_t at = 0;
  while (at <= host.size()) {
    char32_t cp = U'.';  // The end closes the last label.
    size_t len = 1;
    if (at < host.size()) {
      cp = DecodeAt(host, at, &len);
    }
    const bool stop = cp == U'.' || cp == 0x3002 || cp == 0xFF0E ||
                      cp == 0xFF61;
    if (!stop) {
      label.push_back(cp);
      at += len;
      continue;
    }
    if (!AppendRoutingLabel(label, &out)) {
      return std::string(host);
    }
    label.clear();
    if (at < host.size()) {
      out.push_back('.');
    }
    at += len;
  }
  return out;
}

bool IsSourceAppRuleHost(std::string_view host) {
  const size_t n = kSourceAppRuleHostPrefix.size();
  if (host.size() <= n) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    char c = host[i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if (c != kSourceAppRuleHostPrefix[i]) {
      return false;
    }
  }
  return host.find(':', n) == std::string_view::npos;
}

std::string NewRuleId() {
  static thread_local std::mt19937_64 rng([] {
    std::random_device device;
    return (static_cast<uint64_t>(device()) << 32) ^ device() ^
           static_cast<uint64_t>(
               std::chrono::steady_clock::now().time_since_epoch().count());
  }());
  uint64_t hi = (rng() & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
  uint64_t lo = (rng() & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
  char text[37];
  std::snprintf(text, sizeof(text), "%08X-%04X-%04X-%04X-%012llX",
                static_cast<unsigned>(hi >> 32),
                static_cast<unsigned>((hi >> 16) & 0xFFFF),
                static_cast<unsigned>(hi & 0xFFFF),
                static_cast<unsigned>(lo >> 48),
                static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL));
  return text;
}

std::string NormalizedRuleHost(std::string_view raw) {
  return UnicodeLower(TrimWhiteSpace(raw, /*trailing_dots=*/true));
}

std::optional<std::string> NormalizedRulePathPrefix(
    const std::optional<std::string>& raw) {
  if (!raw) {
    return std::nullopt;
  }
  const std::string trimmed(TrimWhiteSpace(*raw, /*trailing_dots=*/false));
  if (trimmed == "/") {
    return std::string("/");  // Root only, which is not "any path".
  }
  if (trimmed.empty()) {
    return std::nullopt;
  }
  std::string s = trimmed.front() == '/' ? trimmed : "/" + trimmed;
  if (std::optional<std::string> decoded = RemovingPercentEncoding(s)) {
    s = *decoded;
  }
  s = AddingPathPercentEncoding(s);
  while (s.size() > 1 && s.back() == '/') {
    s.pop_back();
  }
  return s;
}

}  // namespace phi_client::store

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_auth_codec.h"

#include <algorithm>

namespace phi_client {

namespace {

constexpr std::string_view kBase64UrlAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int Base64UrlValue(char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '-' || c == '+') {
    return 62;
  }
  if (c == '_' || c == '/') {
    return 63;
  }
  return -1;
}

int HexValue(char c) {
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
}

// --- SHA-256 ---------------------------------------------------------------

constexpr std::array<uint32_t, 64> kSha256K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t RotateRight(uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

// One 64-byte block into the running state. `block` is exactly 64 bytes.
void Sha256Block(std::array<uint32_t, 8>& state, std::string_view block) {
  std::array<uint32_t, 64> w;
  for (size_t i = 0; i < 16; ++i) {
    w[i] = (uint32_t{static_cast<uint8_t>(block[i * 4])} << 24) |
           (uint32_t{static_cast<uint8_t>(block[i * 4 + 1])} << 16) |
           (uint32_t{static_cast<uint8_t>(block[i * 4 + 2])} << 8) |
           uint32_t{static_cast<uint8_t>(block[i * 4 + 3])};
  }
  for (size_t i = 16; i < 64; ++i) {
    const uint32_t s0 = RotateRight(w[i - 15], 7) ^ RotateRight(w[i - 15], 18) ^
                        (w[i - 15] >> 3);
    const uint32_t s1 = RotateRight(w[i - 2], 17) ^ RotateRight(w[i - 2], 19) ^
                        (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
  for (size_t i = 0; i < 64; ++i) {
    const uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = h + s1 + ch + kSha256K[i] + w[i];
    const uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

}  // namespace

std::string PhiBase64UrlEncode(std::string_view bytes) {
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  size_t i = 0;
  while (i + 3 <= bytes.size()) {
    const uint32_t n = (uint32_t{static_cast<uint8_t>(bytes[i])} << 16) |
                       (uint32_t{static_cast<uint8_t>(bytes[i + 1])} << 8) |
                       uint32_t{static_cast<uint8_t>(bytes[i + 2])};
    out += kBase64UrlAlphabet[(n >> 18) & 63];
    out += kBase64UrlAlphabet[(n >> 12) & 63];
    out += kBase64UrlAlphabet[(n >> 6) & 63];
    out += kBase64UrlAlphabet[n & 63];
    i += 3;
  }
  const size_t rest = bytes.size() - i;
  if (rest == 1) {
    const uint32_t n = uint32_t{static_cast<uint8_t>(bytes[i])} << 16;
    out += kBase64UrlAlphabet[(n >> 18) & 63];
    out += kBase64UrlAlphabet[(n >> 12) & 63];
  } else if (rest == 2) {
    const uint32_t n = (uint32_t{static_cast<uint8_t>(bytes[i])} << 16) |
                       (uint32_t{static_cast<uint8_t>(bytes[i + 1])} << 8);
    out += kBase64UrlAlphabet[(n >> 18) & 63];
    out += kBase64UrlAlphabet[(n >> 12) & 63];
    out += kBase64UrlAlphabet[(n >> 6) & 63];
  }
  return out;
}

std::optional<std::string> PhiBase64UrlDecode(std::string_view text) {
  // Padding is optional; whatever is there must be trailing '='.
  while (!text.empty() && text.back() == '=') {
    text.remove_suffix(1);
  }
  if (text.size() % 4 == 1) {
    return std::nullopt;  // No byte count encodes to 4k+1 characters.
  }
  std::string out;
  out.reserve(text.size() * 3 / 4);
  uint32_t acc = 0;
  int bits = 0;
  for (char c : text) {
    const int v = Base64UrlValue(c);
    if (v < 0) {
      return std::nullopt;
    }
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((acc >> bits) & 0xFF);
    }
  }
  return out;
}

std::array<uint8_t, 32> PhiSha256(std::string_view data) {
  std::array<uint32_t, 8> state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                   0xa54ff53a, 0x510e527f, 0x9b05688c,
                                   0x1f83d9ab, 0x5be0cd19};
  size_t offset = 0;
  while (data.size() - offset >= 64) {
    Sha256Block(state, data.substr(offset, 64));
    offset += 64;
  }
  // The tail: what is left, 0x80, zeros, and the bit length big-endian —
  // one block, or two when the length no longer fits the first.
  const std::string_view rest = data.substr(offset);
  std::string tail(rest);
  tail += static_cast<char>(0x80);
  const size_t tail_len = rest.size() + 1 + 8 <= 64 ? 64 : 128;
  tail.resize(tail_len, '\0');
  const uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8;
  for (size_t i = 0; i < 8; ++i) {
    tail[tail_len - 1 - i] = static_cast<char>(bit_len >> (8 * i));
  }
  Sha256Block(state, std::string_view(tail).substr(0, 64));
  if (tail_len == 128) {
    Sha256Block(state, std::string_view(tail).substr(64, 64));
  }
  std::array<uint8_t, 32> digest;
  for (size_t i = 0; i < 8; ++i) {
    digest[i * 4] = static_cast<uint8_t>(state[i] >> 24);
    digest[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
    digest[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
    digest[i * 4 + 3] = static_cast<uint8_t>(state[i]);
  }
  return digest;
}

std::string PhiPkceChallenge(std::string_view verifier) {
  const std::array<uint8_t, 32> digest = PhiSha256(verifier);
  return PhiBase64UrlEncode(
      std::string_view(reinterpret_cast<const char*>(digest.data()),
                       digest.size()));
}

std::string PhiUrlEncode(std::string_view in) {
  static constexpr std::string_view kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size() * 3);
  for (unsigned char c : in) {
    const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      c == '.' || c == '~';
    if (safe) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0xF];
    }
  }
  return out;
}

std::string PhiUrlDecode(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < in.size() && HexValue(in[i + 1]) >= 0 &&
               HexValue(in[i + 2]) >= 0) {
      out += static_cast<char>(HexValue(in[i + 1]) * 16 + HexValue(in[i + 2]));
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

namespace {

// Fills the code/state/error triple from "k=v&k=v", percent-decoding values.
void ReadCallbackQuery(std::string_view query,
                       std::string* code,
                       std::string* state,
                       std::string* error) {
  while (!query.empty()) {
    const size_t amp = query.find('&');
    const std::string_view pair =
        amp == std::string_view::npos ? query : query.substr(0, amp);
    query = amp == std::string_view::npos ? std::string_view()
                                          : query.substr(amp + 1);
    const size_t eq = pair.find('=');
    const std::string_view key = pair.substr(0, eq);
    const std::string value =
        eq == std::string_view::npos ? std::string()
                                     : PhiUrlDecode(pair.substr(eq + 1));
    if (key == "code") {
      *code = value;
    } else if (key == "state") {
      *state = value;
    } else if (key == "error") {
      *error = value;
    }
  }
}

std::string Lowered(std::string_view in) {
  std::string out(in);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

}  // namespace

std::string PhiMacCallbackUri(std::string_view bundle_id,
                              std::string_view domain) {
  std::string uri(bundle_id);
  uri += "://";
  uri += domain;
  uri += "/macos/";
  uri += bundle_id;
  uri += "/callback";
  return uri;
}

std::optional<PhiCallbackUrl> PhiParseCallbackUrl(std::string_view url) {
  const size_t scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos || scheme_end == 0) {
    return std::nullopt;
  }
  PhiCallbackUrl parsed;
  parsed.scheme = Lowered(url.substr(0, scheme_end));
  std::string_view rest = url.substr(scheme_end + 3);
  const size_t fragment = rest.find('#');
  if (fragment != std::string_view::npos) {
    rest = rest.substr(0, fragment);
  }
  const size_t slash = rest.find('/');
  const size_t question = rest.find('?');
  const size_t host_end = std::min(slash, question);
  parsed.host = Lowered(rest.substr(0, host_end));
  if (parsed.host.empty()) {
    return std::nullopt;
  }
  if (slash != std::string_view::npos && slash < question) {
    parsed.path = std::string(rest.substr(slash, question == std::string_view::npos ? std::string_view::npos : question - slash));
  } else {
    parsed.path = "/";
  }
  if (question != std::string_view::npos) {
    ReadCallbackQuery(rest.substr(question + 1), &parsed.code, &parsed.state,
                      &parsed.error);
  }
  return parsed;
}

std::optional<std::string> PhiJwtPayload(std::string_view jwt) {
  const size_t first = jwt.find('.');
  if (first == std::string_view::npos) {
    return std::nullopt;
  }
  const size_t second = jwt.find('.', first + 1);
  if (second == std::string_view::npos) {
    return std::nullopt;
  }
  return PhiBase64UrlDecode(jwt.substr(first + 1, second - first - 1));
}

std::string PhiAccountBaseUrlForDomain(std::string_view domain) {
  return domain.find(".stag.") != std::string_view::npos
             ? "https://account.stag.phibrowser.com"
             : "https://account.phibrowser.com";
}

}  // namespace phi_client

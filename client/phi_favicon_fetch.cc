// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_favicon_fetch.h"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>

#include "base/containers/span.h"
#include "client/phi_http.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ws2_32.lib")

namespace phi_client {

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kTimeoutMs = 5000;
constexpr size_t kMaxBytes = 65536;

// The URL's host, lowercased, brackets stripped; "" when it has none.
std::string HostOf(const std::string& url, std::string* scheme) {
  const size_t colon = url.find("://");
  if (colon == std::string::npos) {
    return std::string();
  }
  *scheme = url.substr(0, colon);
  std::transform(scheme->begin(), scheme->end(), scheme->begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::string rest = url.substr(colon + 3);
  rest = rest.substr(0, rest.find_first_of("/?#"));
  if (const size_t at = rest.rfind('@'); at != std::string::npos) {
    rest = rest.substr(at + 1);
  }
  std::string host;
  if (!rest.empty() && rest[0] == '[') {
    host = rest.substr(1, rest.find(']') - 1);
  } else {
    host = rest.substr(0, rest.find(':'));
  }
  std::transform(host.begin(), host.end(), host.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return host;
}

// True when `host` could be read as an IP address by anyone downstream —
// inet_pton's strict forms, an IPv6 literal, or what the URL Standard (and
// inet_aton) parse as IPv4: a last label that is a decimal, octal or 0x-hex
// number ("0x7f.1", "2130706433", "127.1", "017700000001").
bool IsIpLiteral(const std::string& host) {
  IN6_ADDR v6 = {};
  IN_ADDR v4 = {};
  if (::inet_pton(AF_INET6, host.c_str(), &v6) == 1 ||
      ::inet_pton(AF_INET, host.c_str(), &v4) == 1) {
    return true;
  }
  std::string last = host;
  if (!last.empty() && last.back() == '.') {
    last.pop_back();  // "127.0.0.1." is the same host.
  }
  last = last.substr(last.rfind('.') == std::string::npos ? 0
                                                          : last.rfind('.') + 1);
  if (last.empty()) {
    return false;
  }
  if (std::all_of(last.begin(), last.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c));
      })) {
    return true;
  }
  return last.size() >= 2 && last[0] == '0' && last[1] == 'x' &&
         std::all_of(last.begin() + 2, last.end(), [](char c) {
           return std::isxdigit(static_cast<unsigned char>(c));
         });
}

bool EndsWith(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Loopback, private, link-local, CGNAT, multicast, reserved — anything a
// background fetch must not reach.
bool IsNonPublicV4(base::span<const uint8_t, 4> a) {
  return a[0] == 0 || a[0] == 10 || a[0] == 127 ||
         (a[0] == 100 && (a[1] & 0xC0) == 64) ||     // 100.64/10 CGNAT
         (a[0] == 169 && a[1] == 254) ||             // link-local
         (a[0] == 172 && (a[1] & 0xF0) == 16) ||     // 172.16/12
         (a[0] == 192 && a[1] == 168) ||
         (a[0] == 192 && a[1] == 0 && a[2] == 0) ||  // 192.0.0/24
         (a[0] == 198 && (a[1] & 0xFE) == 18) ||     // 198.18/15
         a[0] >= 224;                                // multicast, reserved
}

bool IsNonPublicV6(base::span<const uint8_t, 16> a) {
  static constexpr std::array<uint8_t, 12> kZero = {};
  static constexpr std::array<uint8_t, 12> kMapped = {0, 0, 0, 0, 0,    0,
                                                      0, 0, 0, 0, 0xFF, 0xFF};
  static constexpr std::array<uint8_t, 12> kNat64 = {0, 0x64, 0xFF, 0x9B,
                                                     0, 0,    0,    0,
                                                     0, 0,    0,    0};
  if (std::ranges::equal(a.first<12>(), kZero)) {
    return true;  // ::, ::1 and the deprecated IPv4-compatible block
  }
  if (std::ranges::equal(a.first<12>(), kMapped) ||
      std::ranges::equal(a.first<12>(), kNat64)) {
    return IsNonPublicV4(a.last<4>());
  }
  if (a[0] == 0x20 && a[1] == 0x02) {
    return IsNonPublicV4(a.subspan<2, 4>());  // 6to4 carries its IPv4 next
  }
  return (a[0] & 0xFE) == 0xFC ||                    // fc00::/7 ULA
         (a[0] == 0xFE && (a[1] & 0xC0) == 0x80) ||  // fe80::/10
         (a[0] == 0xFE && (a[1] & 0xC0) == 0xC0) ||  // fec0::/10
         a[0] == 0xFF;                               // multicast
}

// Resolves `host` and answers true only when EVERY address is public. The
// fetch is https-only, so a DNS answer that changes between this check and
// WinHTTP's own lookup still has to present a valid certificate for `host`
// before a request byte is sent.
bool ResolvesToPublicOnly(const std::string& host) {
  WSADATA wsa = {};
  if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    return false;
  }
  addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* list = nullptr;
  bool ok = ::getaddrinfo(host.c_str(), nullptr, &hints, &list) == 0 && list;
  for (const addrinfo* it = list; ok && it; it = it->ai_next) {
    if (it->ai_family == AF_INET &&
        it->ai_addrlen >= sizeof(sockaddr_in)) {
      const auto* in = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
      const ULONG v4 = ::ntohl(in->sin_addr.s_addr);
      const std::array<uint8_t, 4> octets = {
          static_cast<uint8_t>(v4 >> 24), static_cast<uint8_t>(v4 >> 16),
          static_cast<uint8_t>(v4 >> 8), static_cast<uint8_t>(v4)};
      ok = !IsNonPublicV4(octets);
    } else if (it->ai_family == AF_INET6 &&
               it->ai_addrlen >= sizeof(sockaddr_in6)) {
      const auto* in6 = reinterpret_cast<const sockaddr_in6*>(it->ai_addr);
      ok = !IsNonPublicV6(base::span<const uint8_t, 16>(in6->sin6_addr.s6_addr));
    } else {
      ok = false;
    }
  }
  if (list) {
    ::freeaddrinfo(list);
  }
  ::WSACleanup();
  return ok;
}

// The two containers a favicon may be; anything else is never handed to a
// WIC decoder (no codec sniffing, no third-party codecs).
const CLSID* DecoderFor(const std::string& body) {
  static constexpr std::array<uint8_t, 8> kPng = {0x89, 'P',  'N',  'G',
                                                  '\r', '\n', 0x1A, '\n'};
  static constexpr std::array<uint8_t, 4> kIco = {0, 0, 1, 0};
  const base::span<const uint8_t> bytes = base::as_byte_span(body);
  if (bytes.size() >= kPng.size() &&
      std::ranges::equal(bytes.first(kPng.size()), kPng)) {
    return &CLSID_WICPngDecoder;
  }
  if (bytes.size() >= kIco.size() &&
      std::ranges::equal(bytes.first(kIco.size()), kIco)) {
    return &CLSID_WICIcoDecoder;
  }
  return nullptr;
}

// True when the authority WinHTTP will connect to (PhiSplitUrl's: up to the
// first '/', minus a :port) is exactly the vetted `host` — no userinfo, no
// '?'/'#'/'\\' tricks that make the two parsers disagree.
bool AuthorityIsHost(const std::string& https_url, const std::string& host) {
  std::string authority = https_url.substr(8);
  authority = authority.substr(0, authority.find('/'));
  if (const size_t colon = authority.find(':'); colon != std::string::npos) {
    const std::string port = authority.substr(colon + 1);
    if (port.empty() || !std::all_of(port.begin(), port.end(), [](char c) {
          return std::isdigit(static_cast<unsigned char>(c));
        })) {
      return false;
    }
    authority = authority.substr(0, colon);
  }
  std::transform(authority.begin(), authority.end(), authority.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return !host.empty() && authority == host;
}

// The largest frame decoded, either side.
constexpr UINT kMaxFrameSide = 256;
// Redirect hops followed, each re-vetted.
constexpr int kMaxRedirects = 3;

}  // namespace

bool PhiFaviconHostAllowed(const std::string& page_url) {
  std::string scheme;
  const std::string host = HostOf(page_url, &scheme);
  if ((scheme != "http" && scheme != "https") || host.empty()) {
    return false;
  }
  // A canonical (punycode) host name only: '%', brackets and anything else
  // a resolver might reinterpret are refused outright.
  if (!std::all_of(host.begin(), host.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
               c == '-' || c == '_';
      })) {
    return false;
  }
  if (host == "localhost" || EndsWith(host, ".localhost") || host == "local" ||
      EndsWith(host, ".local")) {
    return false;
  }
  return !IsIpLiteral(host);
}

bool PhiFetchSiteFavicon(const std::string& page_url, int px, std::string* bgra) {
  if (!PhiFaviconHostAllowed(page_url) || px <= 0) {
    return false;
  }
  std::string scheme;
  std::string host = HostOf(page_url, &scheme);
  std::string url = "https://" + host + "/favicon.ico";
  PhiHttpResult result;
  // Redirects are followed here, hop by hop, so every host is vetted —
  // textually and by what it resolves to — and every hop stays https.
  for (int hop = 0;; ++hop) {
    if (!ResolvesToPublicOnly(host)) {
      return false;
    }
    result = PhiHttpRequestBounded(L"GET", url, L"Accept: image/*\r\n",
                                   kTimeoutMs, kMaxBytes);
    if (result.location.empty()) {
      break;
    }
    if (hop >= kMaxRedirects || result.location.rfind("https://", 0) != 0 ||
        !PhiFaviconHostAllowed(result.location)) {
      return false;  // relative, downgraded or private: not followed
    }
    url = result.location;
    host = HostOf(url, &scheme);
    if (!AuthorityIsHost(url, host)) {
      return false;
    }
  }
  if (!result.transport_ok || result.status < 200 || result.status >= 300 ||
      result.body.empty()) {
    return false;
  }
  const CLSID* decoder_clsid = DecoderFor(result.body);
  if (!decoder_clsid) {
    return false;
  }
  const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ok = false;
  {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                     CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<char*>(result.body.data())),
            static_cast<DWORD>(result.body.size()))) &&
        SUCCEEDED(::CoCreateInstance(*decoder_clsid, nullptr,
                                     CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&decoder))) &&
        SUCCEEDED(decoder->Initialize(stream.Get(),
                                      WICDecodeMetadataCacheOnDemand))) {
      // An .ico holds several sizes: the smallest one at least `px`, else
      // the largest.
      UINT frames = 0;
      decoder->GetFrameCount(&frames);
      ComPtr<IWICBitmapFrameDecode> best;
      UINT best_size = 0;
      for (UINT i = 0; i < frames; ++i) {
        ComPtr<IWICBitmapFrameDecode> frame;
        UINT width = 0;
        UINT height = 0;
        if (FAILED(decoder->GetFrame(i, &frame)) ||
            FAILED(frame->GetSize(&width, &height)) || !width || !height ||
            width > kMaxFrameSide || height > kMaxFrameSide) {
          continue;  // an oversized frame is never decoded or scaled
        }
        const UINT size = std::max(width, height);
        const bool better =
            !best ||
            (size >= static_cast<UINT>(px)
                 ? (best_size < static_cast<UINT>(px) || size < best_size)
                 : (best_size < static_cast<UINT>(px) && size > best_size));
        if (better) {
          best = frame;
          best_size = size;
        }
      }
      ComPtr<IWICBitmapScaler> scaler;
      ComPtr<IWICFormatConverter> converter;
      if (best && SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
          SUCCEEDED(scaler->Initialize(best.Get(), static_cast<UINT>(px),
                                       static_cast<UINT>(px),
                                       WICBitmapInterpolationModeFant)) &&
          SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
          SUCCEEDED(converter->Initialize(
              scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
              nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        bgra->assign(static_cast<size_t>(px) * px * 4, '\0');
        ok = SUCCEEDED(converter->CopyPixels(
            nullptr, static_cast<UINT>(px * 4), static_cast<UINT>(bgra->size()),
            reinterpret_cast<BYTE*>(bgra->data())));
      }
    }
  }
  if (SUCCEEDED(com)) {
    ::CoUninitialize();
  }
  if (!ok) {
    bgra->clear();
  }
  return ok;
}

}  // namespace phi_client

// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_http.h"

#include <windows.h>

#include <winhttp.h>

#include <cstdlib>
#include <cwctype>
#include <string_view>

#include "base/strings/utf_string_conversions.h"

#pragma comment(lib, "winhttp.lib")

namespace phi_client {

namespace {

// True when `headers` carries an Authorization line (case-insensitive name).
bool HasAuthorization(const std::wstring& headers) {
  static constexpr std::wstring_view kName = L"authorization:";
  std::wstring lower = headers;
  for (wchar_t& c : lower) {
    c = static_cast<wchar_t>(::towlower(c));
  }
  size_t line = 0;
  while (line < lower.size()) {
    if (lower.compare(line, kName.size(), kName) == 0) {
      return true;
    }
    const size_t next = lower.find(L'\n', line);
    if (next == std::wstring::npos) {
      break;
    }
    line = next + 1;
  }
  return false;
}

void DisableRedirects(HINTERNET request) {
  DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  ::WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy,
                     sizeof(policy));
}

}  // namespace

bool PhiSplitUrl(const std::string& url,
                 std::wstring* host,
                 std::wstring* path,
                 int* port,
                 bool* secure) {
  std::string rest;
  if (url.rfind("https://", 0) == 0) {
    *secure = true;
    rest = url.substr(8);
  } else if (url.rfind("http://", 0) == 0) {
    *secure = false;
    rest = url.substr(7);
  } else {
    return false;
  }
  const size_t slash = rest.find('/');
  std::string authority =
      slash == std::string::npos ? rest : rest.substr(0, slash);
  const std::string path_part =
      slash == std::string::npos ? std::string("/") : rest.substr(slash);
  *port = *secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
  const size_t colon = authority.find(':');
  if (colon != std::string::npos) {
    *port = std::atoi(authority.substr(colon + 1).c_str());
    authority = authority.substr(0, colon);
  }
  if (authority.empty() || *port <= 0 || *port > 65535) {
    return false;
  }
  *host = base::UTF8ToWide(authority);
  *path = base::UTF8ToWide(path_part);
  return true;
}

PhiHttpResult PhiHttpRequest(const wchar_t* method,
                             const std::string& url,
                             const std::wstring& headers,
                             const std::string& body,
                             size_t max_bytes) {
  PhiHttpResult result;
  std::wstring host;
  std::wstring path;
  int port = 0;
  bool secure = false;
  if (!PhiSplitUrl(url, &host, &path, &port, &secure)) {
    return result;
  }
  HINTERNET session = ::WinHttpOpen(L"PhiBrowser-Win",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    return result;
  }
  HINTERNET connect = ::WinHttpConnect(session, host.c_str(),
                                       static_cast<INTERNET_PORT>(port), 0);
  if (connect) {
    HINTERNET request = ::WinHttpOpenRequest(
        connect, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (request) {
      if (HasAuthorization(headers)) {
        DisableRedirects(request);
      }
      if (::WinHttpSendRequest(request,
                               headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                               : headers.c_str(),
                               static_cast<DWORD>(-1),
                               body.empty() ? WINHTTP_NO_REQUEST_DATA
                                            : const_cast<char*>(body.data()),
                               static_cast<DWORD>(body.size()),
                               static_cast<DWORD>(body.size()), 0) &&
          ::WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD size = sizeof(status);
        ::WinHttpQueryHeaders(
            request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
            WINHTTP_NO_HEADER_INDEX);
        result.status = static_cast<int>(status);
        result.transport_ok = true;
        SYSTEMTIME date = {};
        DWORD date_size = sizeof(date);
        FILETIME file_time = {};
        if (::WinHttpQueryHeaders(request,
                                  WINHTTP_QUERY_DATE | WINHTTP_QUERY_FLAG_SYSTEMTIME,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &date,
                                  &date_size, WINHTTP_NO_HEADER_INDEX) &&
            ::SystemTimeToFileTime(&date, &file_time)) {
          const uint64_t ticks =
              (static_cast<uint64_t>(file_time.dwHighDateTime) << 32) |
              file_time.dwLowDateTime;
          // 100 ns since 1601 → ms since 1970.
          result.server_date_ms =
              static_cast<int64_t>(ticks / 10000) - 11644473600000LL;
        }
        DWORD available = 0;
        while (::WinHttpQueryDataAvailable(request, &available) &&
               available > 0) {
          if (result.body.size() + available > max_bytes) {
            result.transport_ok = false;  // too large
            result.body.clear();
            break;
          }
          std::string chunk(available, '\0');
          DWORD read = 0;
          if (!::WinHttpReadData(request, chunk.data(), available, &read) ||
              read == 0) {
            break;
          }
          result.body.append(chunk.data(), read);
        }
      }
      ::WinHttpCloseHandle(request);
    }
    ::WinHttpCloseHandle(connect);
  }
  ::WinHttpCloseHandle(session);
  return result;
}

PhiHttpResult PhiHttpRequestBounded(const wchar_t* method,
                                    const std::string& url,
                                    const std::wstring& headers,
                                    int timeout_ms,
                                    size_t max_bytes) {
  PhiHttpResult result;
  std::wstring host;
  std::wstring path;
  int port = 0;
  bool secure = false;
  if (!PhiSplitUrl(url, &host, &path, &port, &secure)) {
    return result;
  }
  HINTERNET session = ::WinHttpOpen(L"PhiBrowser-Win",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    return result;
  }
  ::WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
  HINTERNET connect = ::WinHttpConnect(session, host.c_str(),
                                       static_cast<INTERNET_PORT>(port), 0);
  HINTERNET request =
      connect ? ::WinHttpOpenRequest(connect, method, path.c_str(), nullptr,
                                     WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     secure ? WINHTTP_FLAG_SECURE : 0)
              : nullptr;
  if (request) {
    // No cookies and no credentials ride a background fetch.
    DWORD no_cookies = WINHTTP_DISABLE_COOKIES;
    ::WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &no_cookies,
                       sizeof(no_cookies));
    // Nor does it follow a redirect blind: the caller vets every hop.
    DisableRedirects(request);
    if (::WinHttpSendRequest(request,
                             headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                             : headers.c_str(),
                             static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0,
                             0, 0) &&
        ::WinHttpReceiveResponse(request, nullptr)) {
      DWORD status = 0;
      DWORD size = sizeof(status);
      ::WinHttpQueryHeaders(
          request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
      result.status = static_cast<int>(status);
      result.transport_ok = true;
      if (status >= 300 && status < 400) {
        DWORD location_size = 0;
        ::WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                              WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                              &location_size, WINHTTP_NO_HEADER_INDEX);
        if (location_size > 0 && location_size < 8192) {
          std::wstring location(location_size / sizeof(wchar_t) + 1, L'\0');
          DWORD bytes = static_cast<DWORD>(location.size() * sizeof(wchar_t));
          if (::WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                                    WINHTTP_HEADER_NAME_BY_INDEX,
                                    location.data(), &bytes,
                                    WINHTTP_NO_HEADER_INDEX)) {
            location.resize(bytes / sizeof(wchar_t));
            result.location = base::WideToUTF8(location);
          }
        }
      }
      DWORD available = 0;
      // A redirect's body is never wanted.
      while (result.location.empty() &&
             ::WinHttpQueryDataAvailable(request, &available) && available > 0) {
        if (result.body.size() + available > max_bytes) {
          result.transport_ok = false;  // too large
          result.body.clear();
          break;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!::WinHttpReadData(request, chunk.data(), available, &read) ||
            read == 0) {
          break;
        }
        result.body.append(chunk.data(), read);
      }
    }
    ::WinHttpCloseHandle(request);
  }
  if (connect) {
    ::WinHttpCloseHandle(connect);
  }
  ::WinHttpCloseHandle(session);
  return result;
}

}  // namespace phi_client

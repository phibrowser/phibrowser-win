// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_credentials.h"

#include <windows.h>

#include <wincrypt.h>

#include <algorithm>
#include <optional>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "client/phi_features.h"

#pragma comment(lib, "crypt32.lib")

namespace phi_client {

namespace {

std::string ToLower(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](char c) { return static_cast<char>(::tolower(c)); });
  return out;
}

// Reads the whole file into `out`. False when it does not exist.
bool ReadFileBytes(const std::wstring& path, std::string* out) {
  HANDLE f = ::CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return false;
  }
  char buf[8192];
  DWORD read = 0;
  while (::ReadFile(f, buf, sizeof(buf), &read, nullptr) && read > 0) {
    out->append(buf, read);
  }
  ::CloseHandle(f);
  return true;
}

// DPAPI-decrypts `blob` (current user). Returns nullopt when it is not a DPAPI
// blob (or was wrapped by another user); the caller decides what that means.
std::optional<std::string> DpapiDecrypt(const std::string& blob) {
  DATA_BLOB in = {static_cast<DWORD>(blob.size()),
                  reinterpret_cast<BYTE*>(const_cast<char*>(blob.data()))};
  DATA_BLOB out = {};
  if (!::CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
    return std::nullopt;
  }
  std::string result(reinterpret_cast<char*>(out.pbData), out.cbData);
  ::LocalFree(out.pbData);
  return result;
}

}  // namespace

PhiCredentialVault::PhiCredentialVault() = default;
PhiCredentialVault::~PhiCredentialVault() = default;

void PhiCredentialVault::Load(const std::wstring& user_data_dir) {
  items_.clear();
  present_ = false;
  std::string blob;
  if (!ReadFileBytes(user_data_dir + L"\\PhiCredentials.dat", &blob) ||
      blob.empty()) {
    return;
  }
  present_ = true;
  std::string json;
  if (std::optional<std::string> decrypted = DpapiDecrypt(blob)) {
    json = std::move(*decrypted);
  } else if constexpr (kPhiDevHooks) {
    // A plaintext dev seed (a JSON array) is accepted as-is in a --dev-hooks
    // build only; the box rigs write one instead of driving DPAPI.
    json = blob;
  } else {
    // Release: a file DPAPI cannot unwrap is not our vault, whoever wrote it.
    // Refuse it rather than parse it — the plaintext path would let any file
    // dropped into the profile dir masquerade as the user's passwords.
    LOG(ERROR) << "[PhiCredentials] PhiCredentials.dat is not a DPAPI blob "
                  "for this user; vault left empty";
    return;
  }
  std::optional<base::Value> root =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!root || !root->is_list()) {
    return;
  }
  for (const base::Value& v : root->GetList()) {
    const base::DictValue* d = v.GetIfDict();
    if (!d) {
      continue;
    }
    PhiCredentialItem item;
    auto get = [&](const char* key) -> std::string {
      const std::string* s = d->FindString(key);
      return s ? *s : std::string();
    };
    item.type = get("type").empty() ? "login" : get("type");
    item.name = get("name");
    item.domain = ToLower(get("domain"));
    item.username = get("username");
    item.password = get("password");
    item.uri = get("uri");
    item.credential_id = get("id").empty() ? get("credentialId") : get("id");
    item.notes = get("notes");
    // Derive a domain from the uri when absent, so a uri-only login still
    // matches host queries.
    if (item.domain.empty() && !item.uri.empty()) {
      item.domain = HostOfUri(item.uri);
    }
    items_.push_back(std::move(item));
  }
}

// static
std::string PhiCredentialVault::HostOfUri(const std::string& uri) {
  std::string trimmed;
  base::TrimWhitespaceASCII(uri, base::TRIM_ALL, &trimmed);
  size_t scheme = trimmed.find("://");
  std::string after_scheme =
      scheme != std::string::npos ? trimmed.substr(scheme + 3) : trimmed;
  // Authority ends at the first /, ? or #.
  size_t end = after_scheme.find_first_of("/?#");
  std::string authority =
      end != std::string::npos ? after_scheme.substr(0, end) : after_scheme;
  // Strip userinfo (everything through the LAST '@'), then the port.
  size_t at = authority.rfind('@');
  std::string host =
      at != std::string::npos ? authority.substr(at + 1) : authority;
  if (!host.empty() && host.front() == '[') {
    // A bracketed IPv6 literal keeps its brackets and every colon inside
    // them; the port, if any, follows the ']'. Cutting at the first ':' the
    // way a name's port is cut would fold every address sharing a leading
    // group into one host ("[2001", or "[" for anything starting "::"),
    // and the origin binding would then pass a credential saved for one
    // address to a page on another. No closing bracket is no host.
    const size_t close = host.find(']');
    if (close == std::string::npos) {
      return std::string();
    }
    host = host.substr(0, close + 1);
  } else {
    size_t colon = host.find(':');
    if (colon != std::string::npos) {
      host = host.substr(0, colon);
    }
  }
  return ToLower(host);
}

// static
bool PhiCredentialVault::HostMatches(const std::string& page_host,
                                     const std::string& cred_host) {
  const std::string a = ToLower(page_host);
  const std::string b = ToLower(cred_host);
  if (a.empty() || b.empty()) {
    return false;
  }
  if (a == b) {
    return true;
  }
  // One a subdomain of the other; the parent must itself contain a dot so a
  // bare public suffix ("com") is never treated as everyone's parent.
  auto ends_with_dot = [](const std::string& host, const std::string& parent) {
    return parent.find('.') != std::string::npos &&
           host.size() > parent.size() + 1 &&
           host.compare(host.size() - parent.size() - 1, parent.size() + 1,
                        "." + parent) == 0;
  };
  return ends_with_dot(a, b) || ends_with_dot(b, a);
}

std::vector<const PhiCredentialItem*> PhiCredentialVault::Match(
    const std::string& domain,
    const std::string& id,
    const std::string& search,
    const std::string& username) const {
  std::vector<const PhiCredentialItem*> out;
  const std::string want_user = ToLower(username);
  if (!domain.empty()) {
    const std::string host = HostOfUri(domain).empty() ? ToLower(domain)
                                                        : HostOfUri(domain);
    for (const PhiCredentialItem& item : items_) {
      const std::string cred_host =
          item.domain.empty() ? HostOfUri(item.uri) : item.domain;
      if (!HostMatches(host, cred_host)) {
        continue;
      }
      if (!want_user.empty() && ToLower(item.username) != want_user) {
        continue;
      }
      out.push_back(&item);
    }
    return out;
  }
  if (!id.empty()) {
    for (const PhiCredentialItem& item : items_) {
      if (item.credential_id == id) {
        out.push_back(&item);
      }
    }
    return out;
  }
  if (!search.empty()) {
    const std::string needle = ToLower(search);
    for (const PhiCredentialItem& item : items_) {
      if (ToLower(item.name).find(needle) != std::string::npos ||
          item.domain.find(needle) != std::string::npos) {
        out.push_back(&item);
      }
    }
  }
  return out;
}

}  // namespace phi_client

// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_account_storage.h"

#if defined(_WIN32)
#include <windows.h>

#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "client/phi_file_io.h"
#endif

namespace phi_client {

std::string PhiAccountDirName(std::string_view account_id) {
  std::string out(account_id);
  for (char& c : out) {
    switch (c) {
      case '/':
      case ':':
      case '|':
      case '\\':
      case '*':
      case '?':
      case '"':
      case '<':
      case '>':
        c = '_';
        break;
      default:
        // Control characters are as illegal in a file name as the rest.
        if (static_cast<unsigned char>(c) < 0x20) {
          c = '_';
        }
        break;
    }
  }
  // "." and ".." (and trailing dots or spaces, which Windows strips) must
  // not name a directory that is not this account's.
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
    out.back() = '_';
  }
  return out.empty() ? std::string(kGuestAccountId) : out;
}

std::string PhiDataAccountFor(bool has_session,
                              std::string_view subject,
                              int onboarding_phase,
                              bool guest_mode_enabled) {
  if (has_session && !subject.empty() && onboarding_phase >= 6) {
    return std::string(subject);
  }
  if (guest_mode_enabled) {
    return kGuestAccountId;
  }
  // Behind the login wall: an unfinished account is the one onboarding is
  // setting up; with no session at all nothing is known yet.
  if (has_session && !subject.empty()) {
    return std::string(subject);
  }
  return kGuestAccountId;
}

#if defined(_WIN32)

namespace {

bool EnsureDirectory(const std::wstring& path) {
  if (::CreateDirectoryW(path.c_str(), nullptr)) {
    return true;
  }
  return ::GetLastError() == ERROR_ALREADY_EXISTS;
}

}  // namespace

std::wstring PhiAccountDirPath(const std::wstring& user_data_dir,
                               const std::string& account_id) {
  return user_data_dir + L"\\Phi\\users\\" +
         base::UTF8ToWide(PhiAccountDirName(account_id));
}

std::wstring PhiEnsureAccountDir(const std::wstring& user_data_dir,
                                 const std::string& account_id) {
  if (user_data_dir.empty()) {
    return std::wstring();
  }
  const std::wstring phi = user_data_dir + L"\\Phi";
  const std::wstring users = phi + L"\\users";
  const std::wstring dir =
      users + L"\\" + base::UTF8ToWide(PhiAccountDirName(account_id));
  if (!EnsureDirectory(phi) || !EnsureDirectory(users) ||
      !EnsureDirectory(dir) || !EnsureDirectory(dir + L"\\localDB") ||
      !EnsureDirectory(dir + L"\\defaults")) {
    LOG(ERROR) << "[PhiClient] cannot create the account directory for "
               << PhiAccountDirName(account_id);
    return std::wstring();
  }
  return dir;
}

PhiAccountDefaults::PhiAccountDefaults() = default;
PhiAccountDefaults::~PhiAccountDefaults() = default;

void PhiAccountDefaults::Load(const std::wstring& account_dir) {
  values_ = base::DictValue();
  dir_ = account_dir;
  path_ = account_dir.empty()
              ? std::wstring()
              : account_dir + L"\\defaults\\account_defaults.json";
  save_blocked_ = false;
  if (!path_.empty()) {
    // A corrupt file is quarantined by the reader and reads as empty; a
    // locked one reads as empty too but is never saved over this session.
    if (PhiReadJsonFile(path_, &values_) == PhiJsonFileStatus::kUnreadable) {
      save_blocked_ = true;
      LOG(ERROR) << "[PhiClient] account defaults could not be read; "
                    "saves off";
    }
  }
}

void PhiAccountDefaults::Clear() {
  save_blocked_ = false;
  values_ = base::DictValue();
  dir_.clear();
  path_.clear();
}

int PhiAccountDefaults::GetInt(std::string_view key, int fallback) const {
  return values_.FindInt(key).value_or(fallback);
}

void PhiAccountDefaults::SetInt(std::string_view key, int value) {
  if (values_.FindInt(key) == value) {
    return;
  }
  values_.Set(key, value);
  Save();
}

double PhiAccountDefaults::GetDouble(std::string_view key,
                                     double fallback) const {
  // FindDouble also reads an int, as a plist's number would.
  return values_.FindDouble(key).value_or(fallback);
}

void PhiAccountDefaults::SetDouble(std::string_view key, double value) {
  if (values_.FindDouble(key) == value) {
    return;
  }
  values_.Set(key, value);
  Save();
}

std::string PhiAccountDefaults::GetString(std::string_view key) const {
  const std::string* value = values_.FindString(key);
  return value ? *value : std::string();
}

void PhiAccountDefaults::SetString(std::string_view key,
                                   const std::string& value) {
  if (GetString(key) == value && values_.Find(key)) {
    return;
  }
  values_.Set(key, value);
  Save();
}

std::map<std::string, std::string> PhiAccountDefaults::GetStringMap(
    std::string_view key) const {
  std::map<std::string, std::string> out;
  if (const base::DictValue* dict = values_.FindDict(key)) {
    for (const auto [k, v] : *dict) {
      if (v.is_string()) {
        out[k] = v.GetString();
      }
    }
  }
  return out;
}

void PhiAccountDefaults::SetStringMap(
    std::string_view key,
    const std::map<std::string, std::string>& value) {
  if (GetStringMap(key) == value && values_.Find(key)) {
    return;
  }
  base::DictValue dict;
  for (const auto& [k, v] : value) {
    dict.Set(k, v);
  }
  values_.Set(key, std::move(dict));
  Save();
}

std::map<std::string, int> PhiAccountDefaults::GetIntMap(
    std::string_view key) const {
  std::map<std::string, int> out;
  if (const base::DictValue* dict = values_.FindDict(key)) {
    for (const auto [k, v] : *dict) {
      if (v.is_int()) {
        out[k] = v.GetInt();
      }
    }
  }
  return out;
}

void PhiAccountDefaults::SetIntMap(std::string_view key,
                                   const std::map<std::string, int>& value) {
  if (GetIntMap(key) == value && values_.Find(key)) {
    return;
  }
  base::DictValue dict;
  for (const auto& [k, v] : value) {
    dict.Set(k, v);
  }
  values_.Set(key, std::move(dict));
  Save();
}

const base::DictValue* PhiAccountDefaults::GetDict(std::string_view key) const {
  return values_.FindDict(key);
}

void PhiAccountDefaults::SetDict(std::string_view key, base::DictValue value) {
  if (const base::DictValue* current = values_.FindDict(key);
      current && *current == value) {
    return;
  }
  values_.Set(key, std::move(value));
  Save();
}

void PhiAccountDefaults::Save() {
  if (path_.empty() || save_blocked_) {
    return;
  }
  if (!PhiWriteJsonFile(path_, values_.Clone())) {
    LOG(ERROR) << "[PhiClient] account defaults: write failed";
  }
}

#endif  // defined(_WIN32)

}  // namespace phi_client

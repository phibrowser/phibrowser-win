// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_startup_profile.h"

#include <windows.h>

#include "base/strings/utf_string_conversions.h"
#include "client/phi_file_io.h"

namespace phi_client {

namespace {

std::wstring FilePath(const std::wstring& user_data_dir) {
  return user_data_dir + L"\\" + kPhiStartupProfileFile;
}

}  // namespace

bool PhiIsProfileBasename(std::string_view name) {
  if (name.empty() || name.size() > 64 || name == "." || name == "..") {
    return false;
  }
  for (const char c : name) {
    const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') || c == ' ' || c == '_' ||
                         c == '-' || c == '.';
    if (!allowed) {
      return false;
    }
  }
  return true;
}

std::string PhiReadStartupProfile(const std::wstring& user_data_dir) {
  if (user_data_dir.empty()) {
    return std::string();
  }
  std::string bytes;
  if (!PhiReadFileBytes(FilePath(user_data_dir), &bytes)) {
    return std::string();
  }
  // One line; tolerate a trailing newline from a hand edit.
  while (!bytes.empty() && (bytes.back() == '\n' || bytes.back() == '\r')) {
    bytes.pop_back();
  }
  if (!PhiIsProfileBasename(bytes)) {
    return std::string();
  }
  const DWORD attributes = ::GetFileAttributesW(
      (user_data_dir + L"\\" + base::UTF8ToWide(bytes)).c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    return std::string();
  }
  return bytes;
}

bool PhiWriteStartupProfile(const std::wstring& user_data_dir,
                            const std::string& profile_id) {
  if (user_data_dir.empty() || !PhiIsProfileBasename(profile_id)) {
    return false;
  }
  return PhiWriteFileAtomic(FilePath(user_data_dir), profile_id);
}

}  // namespace phi_client

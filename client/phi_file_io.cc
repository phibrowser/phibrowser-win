// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_file_io.h"

#include <windows.h>

#include <chrono>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"

namespace phi_client {

bool PhiRemoveFileIfPresent(const std::wstring& path) {
  if (::DeleteFileW(path.c_str())) {
    return true;
  }
  const DWORD error = ::GetLastError();
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

namespace {

PhiReadStatus ReadOnce(const std::wstring& path,
                       std::string* out,
                       DWORD* error) {
  out->clear();
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    *error = ::GetLastError();
    return *error == ERROR_FILE_NOT_FOUND || *error == ERROR_PATH_NOT_FOUND
               ? PhiReadStatus::kNotFound
               : PhiReadStatus::kError;
  }
  char buffer[16384];
  DWORD read = 0;
  bool ok = true;
  for (;;) {
    if (!::ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) {
      ok = false;
      break;
    }
    if (read == 0) {
      break;
    }
    out->append(buffer, read);
  }
  if (!ok) {
    *error = ::GetLastError();
    out->clear();
  }
  ::CloseHandle(file);
  return ok ? PhiReadStatus::kOk : PhiReadStatus::kError;
}

}  // namespace

PhiReadStatus PhiReadFile(const std::wstring& path, std::string* out) {
  // A scanner's lock is usually gone in well under a second.
  constexpr int kAttempts = 3;
  DWORD error = 0;
  for (int attempt = 1;; ++attempt) {
    const PhiReadStatus status = ReadOnce(path, out, &error);
    if (status != PhiReadStatus::kError) {
      return status;
    }
    if (attempt == kAttempts) {
      break;
    }
    ::Sleep(100);
  }
  LOG(ERROR) << "[PhiFile] read failed " << base::WideToUTF8(path)
             << " err=" << error;
  return PhiReadStatus::kError;
}

bool PhiReadFileBytes(const std::wstring& path, std::string* out) {
  return PhiReadFile(path, out) == PhiReadStatus::kOk;
}

bool PhiWriteFileAtomic(const std::wstring& path, std::string_view bytes) {
  const std::wstring temp = path + L".tmp";
  HANDLE file = ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    LOG(ERROR) << "[PhiFile] create failed " << base::WideToUTF8(temp)
               << " err=" << ::GetLastError();
    return false;
  }
  DWORD wrote = 0;
  const BOOL written = ::WriteFile(
      file, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
  const DWORD write_err = ::GetLastError();
  const BOOL flushed = ::FlushFileBuffers(file);
  ::CloseHandle(file);
  if (!written || wrote != bytes.size() || !flushed) {
    LOG(ERROR) << "[PhiFile] write failed " << base::WideToUTF8(temp)
               << " err=" << write_err << " wrote=" << wrote << "/"
               << bytes.size();
    ::DeleteFileW(temp.c_str());
    return false;
  }
  if (::ReplaceFileW(path.c_str(), temp.c_str(), nullptr,
                     REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
    return true;
  }
  // First save: there is nothing to replace yet.
  if (::MoveFileExW(temp.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return true;
  }
  LOG(ERROR) << "[PhiFile] replace failed " << base::WideToUTF8(path)
             << " err=" << ::GetLastError();
  ::DeleteFileW(temp.c_str());
  return false;
}

void PhiQuarantineFile(const std::wstring& path, const char* reason) {
  if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return;
  }
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const std::wstring aside = path + L".corrupt-" + std::to_wstring(now);
  if (::MoveFileExW(path.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    LOG(ERROR) << "[PhiFile] quarantined " << base::WideToUTF8(path)
               << " -> " << base::WideToUTF8(aside) << " (" << reason << ")";
  } else {
    LOG(ERROR) << "[PhiFile] could not quarantine " << base::WideToUTF8(path)
               << " err=" << ::GetLastError() << " (" << reason << ")";
  }
}

PhiJsonFileStatus PhiReadJsonFile(const std::wstring& path,
                                  base::DictValue* dict,
                                  int* schema_out) {
  if (schema_out) {
    *schema_out = 0;
  }
  std::string json;
  switch (PhiReadFile(path, &json)) {
    case PhiReadStatus::kNotFound:
      return PhiJsonFileStatus::kMissing;
    case PhiReadStatus::kError:
      return PhiJsonFileStatus::kUnreadable;
    case PhiReadStatus::kOk:
      break;
  }
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root) {
    PhiQuarantineFile(path, "not a JSON object");
    return PhiJsonFileStatus::kCorrupt;
  }
  if (schema_out) {
    *schema_out = root->FindInt("schema").value_or(0);
  }
  *dict = std::move(*root);
  return PhiJsonFileStatus::kOk;
}

bool PhiWriteJsonFile(const std::wstring& path,
                      base::DictValue dict,
                      int schema) {
  if (schema > 0) {
    dict.Set("schema", schema);
  }
  std::string json;
  if (!base::JSONWriter::Write(dict, &json)) {
    LOG(ERROR) << "[PhiFile] serialise failed " << base::WideToUTF8(path);
    return false;
  }
  return PhiWriteFileAtomic(path, json);
}

}  // namespace phi_client

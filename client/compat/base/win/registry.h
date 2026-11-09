// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_WIN_REGISTRY_H_
#define BASE_WIN_REGISTRY_H_

#include <windows.h>

#include <string>

namespace base::win {

class RegKey {
 public:
  RegKey() = default;
  RegKey(HKEY root, const wchar_t* subkey, REGSAM access);
  RegKey(const RegKey&) = delete;
  RegKey& operator=(const RegKey&) = delete;
  ~RegKey();

  bool Valid() const { return key_ != nullptr; }
  LONG ReadValue(const wchar_t* name, std::wstring* out) const;
  LONG ReadValueDW(const wchar_t* name, DWORD* out) const;
  LONG WriteValue(const wchar_t* name, const wchar_t* value);
  LONG WriteValue(const wchar_t* name, DWORD value);

 private:
  HKEY key_ = nullptr;
};

}  // namespace base::win

#endif  // BASE_WIN_REGISTRY_H_

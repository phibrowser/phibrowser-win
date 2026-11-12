// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_fonts.h"

#include <windows.h>

#include <string>

#include "base/logging.h"

namespace phi_client {

void PhiLoadPrivateFonts() {
  static bool loaded = false;
  if (loaded) {
    return;
  }
  loaded = true;
  wchar_t exe_path[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  std::wstring dir(exe_path);
  const size_t slash = dir.find_last_of(L'\\');
  if (slash == std::wstring::npos) {
    return;
  }
  dir.resize(slash + 1);
  dir += L"resources\\fonts\\";
  for (const wchar_t* file : {L"IvyPrestoDisplay-SemiBoldItalic.otf",
                              L"IvyPrestoHeadline-SemiBold.otf",
                              L"IvyPrestoHeadline-Light.otf",
                              // The membership card's join date (SIL OFL 1.1,
                              // its licence ships beside it).
                              L"ShareTechMono-Regular.ttf"}) {
    const std::wstring path = dir + file;
    if (::AddFontResourceExW(path.c_str(), FR_PRIVATE, nullptr) == 0) {
      LOG(WARNING) << "[PhiFonts] private font not loaded: "
                   << std::string(path.begin(), path.end());
    }
  }
}

bool PhiBrandFaceRenders(const std::wstring& text) {
  for (wchar_t c : text) {
    if (c == L' ' || c == L'\t') {
      continue;
    }
    // Basic Latin, Latin-1 and the Latin Extended blocks; anything beyond
    // (CJK, Cyrillic, Greek, …) the Ivy faces do not carry.
    if (c > 0x024F && !(c >= 0x2010 && c <= 0x2027)) {
      return false;
    }
  }
  return true;
}

}  // namespace phi_client

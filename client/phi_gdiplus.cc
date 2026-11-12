// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_gdiplus.h"

#include <windows.h>

#include <objbase.h>
// gdiplus.h needs min/max and the ole headers above it.
#include <algorithm>

#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

namespace phi_client {

bool PhiEnsureGdiPlus() {
  // A function-local static: the initialisation is guaranteed to run once
  // even if two call sites race, and a bool has no exit-time destructor.
  // The token is intentionally dropped — see the header for why nothing
  // shuts GDI+ down.
  static const bool started = [] {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
  }();
  return started;
}

}  // namespace phi_client

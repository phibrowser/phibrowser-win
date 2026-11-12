// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_modal_scope.h"

#include <algorithm>
#include <vector>

#include "base/no_destructor.h"

namespace phi_client {

namespace {

// The open scopes' windows, innermost last (scopes nest on the stack).
std::vector<HWND>& ScopeRoots() {
  static base::NoDestructor<std::vector<HWND>> roots;  // UI thread only.
  return *roots;
}

HWND RootOf(HWND window) {
  return window ? ::GetAncestor(window, GA_ROOTOWNER) : nullptr;
}

}  // namespace

int& PhiModalDepthRef() {
  static int depth = 0;  // UI thread only.
  return depth;
}

int PhiModalDepth() {
  return PhiModalDepthRef();
}

int PhiModalDepthFor(HWND window) {
  const HWND root = RootOf(window);
  return static_cast<int>(std::count_if(
      ScopeRoots().begin(), ScopeRoots().end(),
      [root](HWND scope) { return !scope || scope == root; }));
}

PhiModalScope::PhiModalScope() : PhiModalScope(::GetActiveWindow()) {}

PhiModalScope::PhiModalScope(HWND owner) {
  ++PhiModalDepthRef();
  ScopeRoots().push_back(RootOf(owner));
}

PhiModalScope::~PhiModalScope() {
  --PhiModalDepthRef();
  if (!ScopeRoots().empty()) {
    ScopeRoots().pop_back();
  }
}

}  // namespace phi_client

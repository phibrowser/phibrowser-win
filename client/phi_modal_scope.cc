// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_modal_scope.h"

#include <algorithm>
#include <vector>

#include "base/no_destructor.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"

namespace phi_client {

namespace {

// The open scopes' windows, innermost last (scopes nest on the stack).
std::vector<HWND>& ScopeRoots() {
  static base::NoDestructor<std::vector<HWND>> roots;  // UI thread only.
  return *roots;
}

phi_framework::PhiFramework*& FrameworkRef() {
  static phi_framework::PhiFramework* framework = nullptr;  // UI thread only.
  return framework;
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
  if (phi_framework::PhiFramework* framework = FrameworkRef()) {
    framework->BeginNativeNestedLoop();
    began_ = framework;
  }
}

PhiModalScope::~PhiModalScope() {
  if (began_) {
    began_->EndNativeNestedLoop();
  }
  --PhiModalDepthRef();
  if (!ScopeRoots().empty()) {
    ScopeRoots().pop_back();
  }
}

// static
void PhiModalScope::SetFramework(phi_framework::PhiFramework* framework) {
  FrameworkRef() = framework;
}

}  // namespace phi_client

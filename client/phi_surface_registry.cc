// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_surface_registry.h"

namespace phi_client {

int64_t PhiSurfacePanel::surface_tab_id() const {
  return 0;
}

bool PhiSurfacePanel::ContainsWindow(HWND window) const {
  const HWND card = hwnd();
  return card && window && (window == card || ::IsChild(card, window));
}

bool PhiSurfacePanel::ClickDismisses(POINT) const {
  return true;
}

PhiSurfaceRegistry::PhiSurfaceRegistry() = default;
PhiSurfaceRegistry::~PhiSurfaceRegistry() = default;

void PhiSurfaceRegistry::Attach(std::unique_ptr<PhiSurfacePanel> panel) {
  if (!panel) {
    return;
  }
  const BrowserId id{panel->window_id()};
  panels_[id] = std::move(panel);
}

std::optional<PhiSurfaceRegistry::Origin> PhiSurfaceRegistry::Detach(
    BrowserId window_id) {
  const auto it = panels_.find(window_id);
  if (it == panels_.end()) {
    return std::nullopt;
  }
  Origin origin;
  origin.kind = it->second->kind();
  origin.window_id = window_id;
  origin.origin_window_id = BrowserId{it->second->origin_window_id()};
  origin.origin_tab_id = TabId{static_cast<int32_t>(it->second->origin_tab_id())};
  origin.hands_back_strip = it->second->hands_back_strip();
  panels_.erase(it);
  return origin;
}

PhiSurfacePanel* PhiSurfaceRegistry::Find(BrowserId window_id) const {
  const auto it = panels_.find(window_id);
  return it == panels_.end() ? nullptr : it->second.get();
}

PhiSurfacePanel* PhiSurfaceRegistry::ForOrigin(TabId origin_tab_id,
                                               SidebarSurface kind) const {
  if (!origin_tab_id.valid()) {
    return nullptr;
  }
  for (const auto& [id, panel] : panels_) {
    if (panel->origin_tab_id() == origin_tab_id.value &&
        (kind == SidebarSurface::kContent || panel->kind() == kind)) {
      return panel.get();
    }
  }
  return nullptr;
}

std::vector<PhiSurfacePanel*> PhiSurfaceRegistry::OverWindow(
    BrowserId origin_window_id) const {
  std::vector<PhiSurfacePanel*> over;
  for (const auto& [id, panel] : panels_) {
    if (panel->origin_window_id() == origin_window_id.value) {
      over.push_back(panel.get());
    }
  }
  return over;
}

std::map<int, SidebarSurface> PhiSurfaceRegistry::Surfaces(
    BrowserId origin_window_id) const {
  std::map<int, SidebarSurface> surfaces;
  // A reader covers the page; a peek floats over it. When both are attached
  // to one origin the reader is what the user sees, so it is what the row
  // shows.
  for (const auto& [id, panel] : panels_) {
    if (panel->origin_window_id() != origin_window_id.value ||
        panel->origin_tab_id() == 0) {
      continue;
    }
    const int tab = static_cast<int>(panel->origin_tab_id());
    const auto existing = surfaces.find(tab);
    if (existing == surfaces.end() ||
        panel->kind() == SidebarSurface::kReader) {
      surfaces[tab] = panel->kind();
    }
  }
  return surfaces;
}

bool PhiSurfaceRegistry::HasKind(SidebarSurface kind) const {
  for (const auto& [id, panel] : panels_) {
    if (panel->kind() == kind) {
      return true;
    }
  }
  return false;
}

void PhiSurfaceRegistry::ForEach(
    const std::function<void(PhiSurfacePanel&)>& fn) const {
  for (const auto& [id, panel] : panels_) {
    fn(*panel);
  }
}

}  // namespace phi_client

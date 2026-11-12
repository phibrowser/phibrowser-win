// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_theme_bus.h"

#include <algorithm>
#include <utility>

#include "client/phi_window.h"

namespace phi_client {

namespace {

const wchar_t kBusClassName[] = L"PhiThemeBus";

}  // namespace

// static
PhiThemeBus& PhiThemeBus::Get() {
  // Leaked on purpose: the bus outlives every window, and a static object
  // with a destructor would trip -Wexit-time-destructors.
  static PhiThemeBus* bus = new PhiThemeBus();
  return *bus;
}

PhiThemeBus::PhiThemeBus()
    : system_dark_(PhiSystemPrefersDark()),
      high_contrast_(PhiHighContrastActive()) {
  EnsureWindow();
}

PhiThemeBus::~PhiThemeBus() = default;

void PhiThemeBus::EnsureWindow() {
  if (hwnd_) {
    return;
  }
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &PhiThemeBus::BusProc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = kBusClassName;
  if (::RegisterClassExW(&wc) == 0 &&
      ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return;
  }
  // HWND_MESSAGE would not receive WM_SETTINGCHANGE: the shell broadcasts
  // it to TOP-LEVEL windows only. A 0x0, never-shown, tool-window popup is
  // top-level for that purpose and invisible for every other.
  hwnd_ = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            kBusClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr,
                            nullptr, ::GetModuleHandleW(nullptr), nullptr);
}

// static
LRESULT CALLBACK PhiThemeBus::BusProc(HWND hwnd,
                                      UINT message,
                                      WPARAM wparam,
                                      LPARAM lparam) {
  switch (message) {
    case WM_SETTINGCHANGE: {
      const auto* area = reinterpret_cast<const wchar_t*>(lparam);
      if ((area && ::lstrcmpW(area, L"ImmersiveColorSet") == 0) ||
          wparam == SPI_SETHIGHCONTRAST) {
        PhiThemeBus::Get().PollSystemAppearance();
      }
      break;
    }
    case WM_THEMECHANGED:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
      PhiThemeBus::Get().PollSystemAppearance();
      break;
    case WM_SYSCOLORCHANGE:
      // Under a contrast theme the palette IS the system colours, so a
      // change of them (a different contrast theme, an edited one) is a
      // palette change even though nothing PollSystemAppearance compares
      // has moved.
      if (PhiHighContrastActive()) {
        PhiThemeBus::Get().Broadcast();
      } else {
        PhiThemeBus::Get().PollSystemAppearance();
      }
      break;
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

void PhiThemeBus::PollSystemAppearance() {
  const bool dark = PhiSystemPrefersDark();
  const bool high_contrast = PhiHighContrastActive();
  if (dark == system_dark_ && high_contrast == high_contrast_) {
    return;
  }
  system_dark_ = dark;
  high_contrast_ = high_contrast;
  Broadcast();
}

void PhiThemeBus::Register(PhiWindow* window) {
  if (!window) {
    return;
  }
  if (std::find(windows_.begin(), windows_.end(), window) == windows_.end()) {
    windows_.push_back(window);
  }
}

void PhiThemeBus::Unregister(PhiWindow* window) {
  windows_.erase(std::remove(windows_.begin(), windows_.end(), window),
                 windows_.end());
}

int PhiThemeBus::AddObserver(std::function<void()> on_theme_changed) {
  if (!on_theme_changed) {
    return 0;
  }
  Observer observer;
  observer.token = next_token_++;
  observer.callback = std::move(on_theme_changed);
  observers_.push_back(std::move(observer));
  return observers_.back().token;
}

void PhiThemeBus::RemoveObserver(int token) {
  observers_.erase(
      std::remove_if(observers_.begin(), observers_.end(),
                     [token](const Observer& o) { return o.token == token; }),
      observers_.end());
}

void PhiThemeBus::Broadcast() {
  // Copy first: an observer may open or close a window, and a window's
  // OnThemeChanged may destroy it.
  const std::vector<Observer> observers = observers_;
  for (const Observer& observer : observers) {
    // Still subscribed?
    if (std::any_of(observers_.begin(), observers_.end(),
                    [&observer](const Observer& live) {
                      return live.token == observer.token;
                    })) {
      observer.callback();
    }
  }
  const std::vector<PhiWindow*> windows = windows_;
  for (PhiWindow* window : windows) {
    if (std::find(windows_.begin(), windows_.end(), window) !=
        windows_.end()) {
      window->NotifyThemeChanged();
    }
  }
}

}  // namespace phi_client

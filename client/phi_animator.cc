// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_animator.h"

#include <algorithm>
#include <utility>

namespace phi_client {

namespace {

const wchar_t kClockClassName[] = L"PhiAnimatorClock";
constexpr UINT_PTR kClockTimer = 1;

}  // namespace

// static
PhiAnimator& PhiAnimator::ForThread() {
  // One per UI thread. The client runs its whole UI on one thread; the
  // thread_local keeps that a property of the code rather than a comment.
  // Leaked on purpose (no exit-time destructor, no late tick into a dead
  // object).
  static thread_local PhiAnimator* animator = new PhiAnimator();
  return *animator;
}

PhiAnimator::PhiAnimator() {
  EnsureWindow();
}

PhiAnimator::~PhiAnimator() = default;

void PhiAnimator::EnsureWindow() {
  if (hwnd_) {
    return;
  }
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &PhiAnimator::ClockProc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = kClockClassName;
  if (::RegisterClassExW(&wc) == 0 &&
      ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return;
  }
  hwnd_ = ::CreateWindowExW(0, kClockClassName, L"", 0, 0, 0, 0, 0,
                            HWND_MESSAGE, nullptr,
                            ::GetModuleHandleW(nullptr), nullptr);
}

// static
LRESULT CALLBACK PhiAnimator::ClockProc(HWND hwnd,
                                        UINT message,
                                        WPARAM wparam,
                                        LPARAM lparam) {
  if (message == WM_TIMER && wparam == kClockTimer) {
    PhiAnimator::ForThread().Tick();
    return 0;
  }
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

void PhiAnimator::SyncTimer() {
  if (!hwnd_) {
    return;
  }
  const bool want = !animations_.empty();
  if (want == timer_running_) {
    return;
  }
  if (want) {
    ::SetCoalescableTimer(hwnd_, kClockTimer, kTickMs, nullptr,
                          TIMERV_DEFAULT_COALESCING);
  } else {
    ::KillTimer(hwnd_, kClockTimer);
  }
  timer_running_ = want;
}

int PhiAnimator::Animate(HWND target,
                         int duration_ms,
                         PhiEasing easing,
                         TickFn on_tick,
                         DoneFn on_done) {
  if (!target || !::IsWindow(target) || duration_ms <= 0 || !on_tick) {
    // Degenerate: land on the end state immediately so no caller needs a
    // second copy of it.
    if (on_tick) {
      RECT dirty = {};
      on_tick(1.0, &dirty);
      if (target && ::IsWindow(target) && dirty.right > dirty.left &&
          dirty.bottom > dirty.top) {
        ::InvalidateRect(target, &dirty, FALSE);
      }
    }
    if (on_done) {
      on_done();
    }
    return 0;
  }
  Animation animation;
  animation.handle = next_handle_++;
  animation.target = target;
  animation.duration_ms = duration_ms;
  animation.easing = easing;
  animation.on_tick = std::move(on_tick);
  animation.on_done = std::move(on_done);
  const int handle = animation.handle;
  animations_.push_back(std::move(animation));
  SyncTimer();
  return handle;
}

void PhiAnimator::Cancel(int handle) {
  if (!handle) {
    return;
  }
  animations_.erase(
      std::remove_if(
          animations_.begin(), animations_.end(),
          [handle](const Animation& a) { return a.handle == handle; }),
      animations_.end());
  if (!ticking_) {
    SyncTimer();
  }
}

void PhiAnimator::CancelForWindow(HWND target) {
  if (!target) {
    return;
  }
  animations_.erase(
      std::remove_if(animations_.begin(), animations_.end(),
                     [target](const Animation& a) {
                       return a.target == target;
                     }),
      animations_.end());
  if (!ticking_) {
    SyncTimer();
  }
}

bool PhiAnimator::IsRunning(int handle) const {
  return handle != 0 &&
         std::any_of(
             animations_.begin(), animations_.end(),
             [handle](const Animation& a) { return a.handle == handle; });
}

void PhiAnimator::Tick() {
  if (ticking_) {
    return;  // A callback pumped messages; one advance per tick.
  }
  ticking_ = true;
  // Snapshot the handles: a callback may start or cancel animations.
  std::vector<int> handles;
  handles.reserve(animations_.size());
  for (const Animation& animation : animations_) {
    handles.push_back(animation.handle);
  }

  for (const int handle : handles) {
    auto it = std::find_if(
        animations_.begin(), animations_.end(),
        [handle](const Animation& a) { return a.handle == handle; });
    if (it == animations_.end()) {
      continue;
    }
    if (!it->target || !::IsWindow(it->target)) {
      animations_.erase(it);
      continue;
    }
    it->elapsed_ms += static_cast<int>(kTickMs);
    const bool finished = it->elapsed_ms >= it->duration_ms;
    const double raw = finished ? 1.0
                                : static_cast<double>(it->elapsed_ms) /
                                      static_cast<double>(it->duration_ms);
    // Copy what the callback needs: the vector may move under it.
    const HWND target = it->target;
    TickFn tick = it->on_tick;
    DoneFn done;
    if (finished) {
      done = std::move(it->on_done);
    }
    const double eased = PhiEase(it->easing, raw);
    RECT dirty = {};
    tick(eased, &dirty);
    if (::IsWindow(target) && dirty.right > dirty.left &&
        dirty.bottom > dirty.top) {
      ::InvalidateRect(target, &dirty, FALSE);
    }
    if (!finished) {
      continue;
    }
    // Remove BEFORE on_done: it may destroy the window, the caller, or
    // itself (the toast's fade-out does).
    auto live = std::find_if(
        animations_.begin(), animations_.end(),
        [handle](const Animation& a) { return a.handle == handle; });
    if (live != animations_.end()) {
      animations_.erase(live);
    }
    if (done) {
      done();
    }
  }
  ticking_ = false;
  SyncTimer();
}

}  // namespace phi_client

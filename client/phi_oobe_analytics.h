// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_OOBE_ANALYTICS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_OOBE_ANALYTICS_H_

#include <cstdint>
#include <functional>
#include <optional>

namespace phi_client {

// The onboarding's usage metric — the Mac's OOBEAnalyticsSession
// (Sources/UserInterface/Onboarding/OOBEAnalyticsSession.swift), same
// event names, same step vocabulary and indices, same clock discipline:
// a step's duration runs from its presentation to its completion or to
// the next presentation (a step re-presented after a step-back pauses the
// old one rather than counting it twice), and the total is the sum of
// active step time. One session per onboarding window; once it ends
// (finished or interrupted) it reports nothing more.
//
// Pure — no base/, no Win32 — so test/model/oobe_analytics_test.cc proves
// it on the host (with phi_oobe_analytics.cc, which holds only the
// constructor and destructor chromium-style wants out of line). The events
// come out as a struct; the onboarding window's sink turns them into the
// collector's property dictionaries (PhiOobeEventProperties in
// phi_client_analytics.h).
class PhiOobeAnalyticsSession {
 public:
  // The Mac's Step raw values. `kPasswordManager` has no Windows page but
  // keeps its index so the funnel's step numbers agree across platforms.
  enum class Step {
    kLogin,
    kSetName,
    kSetTheme,
    kLayoutSelection,
    kPasswordManager,
    kNextStep,
    kGuestPrivacy,
  };
  enum class Interruption { kWindowClosed, kAppTerminated };

  // One captured event. `name` is the PostHog event; the other fields are
  // valid for the events that carry them (the Mac's property sets):
  //   oobe_step_viewed      step, step_index, is_guest
  //   oobe_step_completed   step, step_index, is_guest, duration_seconds
  //   oobe_finished         is_guest, steps_completed, total_duration_seconds
  //   oobe_interrupted      step (as last_step), step_index, is_guest,
  //                         total_duration_seconds, reason
  struct Event {
    const char* name = nullptr;
    const char* step = nullptr;
    int step_index = 0;
    bool is_guest = false;
    double duration_seconds = 0;
    int steps_completed = 0;
    double total_duration_seconds = 0;
    const char* reason = nullptr;
  };
  using Sink = std::function<void(const Event&)>;

  explicit PhiOobeAnalyticsSession(Sink sink);
  ~PhiOobeAnalyticsSession();

  PhiOobeAnalyticsSession(const PhiOobeAnalyticsSession&) = delete;
  PhiOobeAnalyticsSession& operator=(const PhiOobeAnalyticsSession&) = delete;

  static const char* StepName(Step step) {
    switch (step) {
      case Step::kLogin:
        return "login";
      case Step::kSetName:
        return "set_name";
      case Step::kSetTheme:
        return "set_theme";
      case Step::kLayoutSelection:
        return "layout_selection";
      case Step::kPasswordManager:
        return "password_manager";
      case Step::kNextStep:
        return "next_step";
      case Step::kGuestPrivacy:
        return "guest_privacy";
    }
    return "login";
  }
  static int StepIndex(Step step) {
    switch (step) {
      case Step::kLogin:
        return 1;
      case Step::kSetName:
      case Step::kGuestPrivacy:
        return 2;
      case Step::kSetTheme:
        return 3;
      case Step::kLayoutSelection:
        return 4;
      case Step::kPasswordManager:
        return 5;
      case Step::kNextStep:
        return 6;
    }
    return 0;
  }
  static bool StepIsGuest(Step step) { return step == Step::kGuestPrivacy; }

  bool ended() const { return ended_; }

  // A step came on screen. Re-presenting the current step is nothing; a
  // different step pauses the current one (its time counts, no completion
  // event) and starts the new one's clock. `uptime_seconds` is a monotonic
  // clock (the Mac's ProcessInfo.systemUptime).
  void Present(Step step, double uptime_seconds) {
    if (ended_ || (current_ && *current_ == step)) {
      return;
    }
    Pause(uptime_seconds);
    current_ = step;
    started_at_ = uptime_seconds;
    last_presented_ = step;
    Event event;
    event.name = "oobe_step_viewed";
    FillStep(&event, step);
    sink_(event);
  }

  // The current step accepted its user action.
  void CompleteCurrentStep(double uptime_seconds) {
    if (ended_ || !current_) {
      return;
    }
    const double duration = Elapsed(uptime_seconds);
    total_ += duration;
    Event event;
    event.name = "oobe_step_completed";
    FillStep(&event, *current_);
    event.duration_seconds = duration;
    sink_(event);
    current_.reset();
  }

  // Access was granted. `steps_completed` overrides the last presented
  // step's index (the Mac passes 2 for the Guest path); nullopt takes it.
  void Finish(bool is_guest,
              std::optional<int> steps_completed,
              double uptime_seconds) {
    if (ended_) {
      return;
    }
    CompleteCurrentStep(uptime_seconds);
    Event event;
    event.name = "oobe_finished";
    event.is_guest = is_guest;
    event.steps_completed =
        steps_completed
            ? *steps_completed
            : (last_presented_ ? StepIndex(*last_presented_) : 0);
    event.total_duration_seconds = total_;
    sink_(event);
    End();
  }

  // The window went away before success. Nothing is reported when no step
  // was ever presented.
  void Interrupt(Interruption reason, double uptime_seconds) {
    if (ended_ || !last_presented_) {
      return;
    }
    Pause(uptime_seconds);
    Event event;
    event.name = "oobe_interrupted";
    FillStep(&event, *last_presented_);
    event.total_duration_seconds = total_;
    event.reason = reason == Interruption::kWindowClosed ? "window_closed"
                                                          : "app_terminated";
    sink_(event);
    End();
  }

  // The window is closing for a reason that is neither success nor the
  // user's abandonment (the Mac's closeLoginWindow after a completion that
  // already reported): end the session silently.
  void SuppressInterruption() {
    if (!ended_) {
      End();
    }
  }

 private:
  double Elapsed(double uptime_seconds) const {
    const double elapsed = uptime_seconds - started_at_;
    return elapsed > 0 ? elapsed : 0;
  }
  void Pause(double uptime_seconds) {
    if (current_) {
      total_ += Elapsed(uptime_seconds);
    }
    current_.reset();
  }
  void FillStep(Event* event, Step step) const {
    event->step = StepName(step);
    event->step_index = StepIndex(step);
    event->is_guest = StepIsGuest(step);
  }
  void End() {
    ended_ = true;
    current_.reset();
  }

  Sink sink_;
  std::optional<Step> current_;
  double started_at_ = 0;
  std::optional<Step> last_presented_;
  double total_ = 0;
  bool ended_ = false;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_OOBE_ANALYTICS_H_

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_coordinator.h"

#include <windows.h>

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/compiler_specific.h"
#include "base/json/json_reader.h"
#include "third_party/abseil-cpp/absl/functional/overload.h"
#include "base/json/json_writer.h"
#include "base/command_line.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/escape.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_agent_handoff_prompt.h"
#include "phibrowser-win/client/phi_agent_overlay.h"
#include "phibrowser-win/client/phi_client_analytics.h"
#include "phibrowser-win/client/phi_control_dispatcher.h"
#include "phibrowser-win/client/phi_create_space_window.h"
#include "phibrowser-win/client/phi_extensions_popover.h"
#include "phibrowser-win/client/phi_features.h"
#include "phibrowser-win/client/phi_file_io.h"
#include "phibrowser-win/client/phi_guest_migration_win.h"
#include "phibrowser-win/client/phi_fonts.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_modal_scope.h"
#include "phibrowser-win/client/phi_new_tab_page.h"
#include "phibrowser-win/client/phi_popup_chrome.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_startup_profile.h"
#include "phibrowser-win/client/phi_sync_settings_win.h"
#include "phibrowser-win/client/phi_telemetry.h"
#include "phibrowser-win/client/phi_theme_bus.h"
#include "phibrowser-win/client/phi_toast.h"
#include "phibrowser-win/client/phi_updater.h"
#include "phibrowser-win/client/phi_url_rules_window.h"
#include "phibrowser-win/client/phi_version.h"

namespace phi_client {

namespace {
// The one coordinator, reachable from the static keyboard hook. The client
// creates exactly one for the process lifetime.
PhiCoordinator* g_coordinator = nullptr;

}  // namespace

PhiCoordinator::PhiCoordinator() : tab_store_(local_store_) {
  g_coordinator = this;
  // Every secondary window follows the app palette from here on.
  PhiSetPopupPalette(&preferences_.palette());
  // The preferences' fan-out (windows, framework mirrors, telemetry, the
  // settings window) is this class's observer; the Space manager's is
  // subscribed once it exists (OnFrameworkReady).
  preferences_.AddObserver(this);
  // A live OS light/dark or contrast switch re-resolves every Space's
  // palette and repaints the shells, as a theme pick does (phi_theme_bus.h).
  PhiThemeBus::Get().AddObserver([this] {
    preferences_.RefreshPalette();  // Popups read it through a pointer.
    if (space_manager_) {
      space_manager_->RebuildSpacePalettes();
      for (PhiShellWindow* controller : registry_.Controllers()) {
        controller->ApplyPalette();
      }
    }
  });
}

PhiCoordinator::~PhiCoordinator() {
  if (mouse_hook_) {
    ::UnhookWindowsHookEx(mouse_hook_);
  }
  if (session_hwnd_) {
    ::DestroyWindow(session_hwnd_);  // Its proc must not reach `this` after.
  }
  g_coordinator = nullptr;
}

namespace {
constexpr wchar_t kMsgClassName[] = L"PhiClientControlMsg";
constexpr wchar_t kSessionClassName[] = L"PhiClientSessionEnd";
// (WM_APP + 1 / + 2 were WM_PHI_CONTROL / WM_PHI_AUTHORIZE: the control
// verbs and consent decisions now marshal to PhiControlDispatcher's own
// message window.)
// A worker thread finished changing the Auth0 session state.
constexpr UINT WM_PHI_AUTH_CHANGED = WM_APP + 3;
// (WM_APP + 4 was WM_PHI_READER_SWEEP: the reader-panel settle now runs
// straight from OnWindowChanged, which contract rule 1 delivers after commit
// from a fresh task, coalesced per window — the deferral bought nothing the
// framework does not already do.)
// Reader View: hand a reader panel's page back to its origin window as a tab.
// Still DEFERRED: the panel's constructor runs RefreshState, and a panel
// born on a non-reader page would ask for this from inside
// OnBrowserWindowCreated, where creating a tab and closing a browser are
// forbidden (contract rule 2). wparam carries the panel window id.
constexpr UINT WM_PHI_READER_EXPAND = WM_APP + 5;
// Reader View: close a panel browser whose surface tab has gone. DEFERRED:
// OnPlaceholderModeChanged carries no rule-1 promise — it is reported from
// inside the tab removal that caused it, where closing a browser trips
// `!observers_.is_notifying_observers()`. wparam carries the panel window id.
constexpr UINT WM_PHI_READER_RETIRE = WM_APP + 6;
// The coalesced Space-menu / routing-table push (PushSpaceMenu): however
// many mutations ask in one turn, the framework hears once, on the next.
constexpr UINT WM_PHI_PUSH_SPACES = WM_APP + 8;
// The URL router's "ask first" chooser, DEFERRED out of the navigation-
// throttle callback it is requested from (a modal loop there is the lane 1
// §3.1 hazard); the router runs one queued ask per message.
constexpr UINT WM_PHI_ASK_SPACE = WM_APP + 9;
// Agent Autoview's re-evaluation, DEFERRED out of the verb handler or the
// teardown that reported the run-state edge: a window swap must not start
// inside either.
constexpr UINT WM_PHI_AUTOVIEW = WM_APP + 10;
// Opens the onboarding window on a turn of its own: OnFrameworkReady runs
// inside Chromium's startup, before its first window, and a window shown
// from there sits in front of that startup.
constexpr UINT WM_PHI_OPEN_ONBOARDING = WM_APP + 11;
// WinSparkle has launched the downloaded installer and wants the app gone
// (PhiUpdater's shutdown-request callback, posted from WinSparkle's own
// thread): QuitForUpdate closes every window through the normal paths.
constexpr UINT WM_PHI_QUIT_FOR_UPDATE = WM_APP + 12;
// A mouse button went down somewhere in a shell: dismiss the peek it landed
// outside of, and focus the split pane whose reader it landed in.
// DEFERRED out of the WH_MOUSE hook, where it used to run:
// closing the peek's browser inside the button-down hook — a focused window
// destroyed under a press in flight — cost the click itself, so a Shift-click
// meant to open the NEXT peek opened nothing. wparam is the window the
// button went down in, lparam the press's screen point (x, y packed).
constexpr UINT WM_PHI_DISMISS_PEEKS = WM_APP + 13;
// PhiUpdater::UpdateAvailable changed (posted from WinSparkle's thread):
// every window shows or drops its Update reminder.
constexpr UINT WM_PHI_UPDATE_STATE = WM_APP + 14;
// A user window came up: agent notification cards that waited for one show
// in it, on a turn after the shell finished hosting its browser.
constexpr UINT WM_PHI_REFRESH_CARDS = WM_APP + 15;
// The surface coordinator's deferred turn: closes of surface browsers and
// split opens from a surface that may not run on the stack that asked —
// a Peek replaced from inside OnBrowserWindowCreated (contract rule 2),
// "Open in Split View" from a surface page's own gesture, where that
// page's WebContents is on the stack (2026-10-14 review H1/H2).
constexpr UINT WM_PHI_SURFACE_TURN = WM_APP + 16;
}  // namespace

void PhiCoordinator::OnFrameworkReady(phi_framework::PhiFramework* framework,
                                     const char* user_data_dir_utf8) {
  framework_ = framework;

  // Start the outbound telemetry pipeline. It reads phi_telemetry.json from
  // this directory and stays entirely inert — spooling nothing, sending
  // nothing — until a collector is named there.
  if (user_data_dir_utf8) {
    PhiTelemetry::Get().Start(base::UTF8ToWide(user_data_dir_utf8));
  }

  // Message-only window on the UI thread (this callback runs on it): the
  // coordinator's timers, the auth worker's completion and the deferred
  // reader-panel messages land here. Control verbs do not — the dispatcher
  // marshals them on a message window of its own.
  static bool class_registered = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &PhiCoordinator::MsgWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kMsgClassName;
    return ::RegisterClassExW(&wc) != 0;
  }();
  (void)class_registered;
  msg_hwnd_ = ::CreateWindowExW(0, kMsgClassName, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr,
                                ::GetModuleHandleW(nullptr), this);
  // Logoff / shutdown reach top-level windows only (a message-only window
  // gets no broadcasts): a hidden popup of our own takes them. Never shown,
  // no owner, a tool window so it never shows on the taskbar.
  static bool session_class_registered = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &PhiCoordinator::SessionWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kSessionClassName;
    return ::RegisterClassExW(&wc) != 0;
  }();
  (void)session_class_registered;
  session_hwnd_ = ::CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kSessionClassName, L"", WS_POPUP,
      0, 0, 0, 0, nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
  // The control plane (docs/architecture-review.md §3.4): the verb ladder,
  // peer consent and the deferred replies, behind the pipe server's
  // delegate. Built before anything can ask it to run a verb.
  dispatcher_ = std::make_unique<PhiControlDispatcher>(this, framework_);
  // The surfaces service can exist without the framework (the popup chrome
  // and the reader bridge are built with the coordinator); this is where it
  // learns the framework it acts through.
  surface_coordinator_.Init(framework_);
  // The Space manager and the URL router (docs/architecture-review.md
  // §3.4): built now that the framework is known (it hands itself over
  // and is never null), over the registry and window model declared
  // before them. Both mark the one Spaces file dirty; this class is their
  // host and the manager's observer.
  space_manager_ = std::make_unique<PhiSpaceManager>(
      registry_, window_model_, preferences_, *framework_, *this, local_store_,
      account_defaults_, [this] { ScheduleSave(StoreDomain::kSpaces); });
  space_manager_->AddObserver(this);
  url_router_ = std::make_unique<PhiUrlRouter>(
      space_manager_->mutable_model(), registry_, *framework_, *this,
      [this] { ScheduleSave(StoreDomain::kSpaces); });
  // Lazy Space restore (docs/architecture-review.md §3.4): the ghosts, the
  // cold-start plan and the last-active Space, over PhiRestore.json.
  session_restore_ = std::make_unique<PhiSessionRestore>(
      *framework_, *this, [this] { ScheduleSave(StoreDomain::kRestore); });
  // Chromium profiles (docs/architecture-review.md §3.4): the async
  // creation with tokens, the listing reads and the per-profile settings.
  profile_service_ = std::make_unique<PhiProfileService>(*framework_, *this);

  // Menu-bar accelerators (PhiKeyboardAccelerators): the service installs
  // its own thread-scoped WH_KEYBOARD hook on this thread and reads the
  // user's chord overrides from the preferences.
  accelerators_ =
      std::make_unique<PhiKeyboardAccelerators>(preferences_, *this);
  accelerators_->Install();
  mouse_hook_ = ::SetWindowsHookExW(WH_MOUSE, &MouseHookProc, nullptr,
                                    ::GetCurrentThreadId());

  // The agent Spaces, the shadow windows and the operating mask.
  agent_manager_.Init(framework_);
  if (msg_hwnd_) {
    ::SetTimer(msg_hwnd_, kAgentDriveSweepTimer,
               PhiAgentDriveRegistry::kSweepIntervalMs, nullptr);
  }

  const std::wstring user_data_dir =
      base::UTF8ToWide(user_data_dir_utf8 ? user_data_dir_utf8 : "");
  user_data_dir_ = user_data_dir;
  // The credential providers (PhiCredentialService): the real out-of-process
  // vault helper, with the local DPAPI dev vault as the --dev-hooks stand-in.
  credentials_.Init(user_data_dir);
  // The Auth0 session and the feedback outbox (PhiAccountAndFeedback) come
  // before the stores: the session decides WHICH account's data opens.
  account_.Init(framework_, user_data_dir, msg_hwnd_, WM_PHI_AUTH_CHANGED);
  chromium_sync_.Bind(framework_,
                      [this] { return account_.AccessTokenForSync(); });
  LoadStores(user_data_dir);
  MirrorPinScope();  // The local store's settings row is the scope's truth.
  // The framework mirrors (restore-on-startup, peek, display language) are
  // NOT pushed here: no profile is loaded yet, and OnProfileLoaded — which
  // follows this callback for every profile — is their one push point.
  Transition(AppPhase::kStoresLoaded, "loaded");
  PushSpaceMenu();  // The page context menu's "Open Link in Space" list.

  control_server_.Start(user_data_dir, dispatcher_.get(), framework_);
  notification_cards_ = std::make_unique<PhiNotificationCards>(
      // A decision is the asking agent session's alone (the Mac's
      // principal-scoped broadcastToAgent): every live connection of that
      // capability, no other agent's.
      [this](const std::string& capability_id,
             const std::string& payload_json) {
        control_server_.SendEventToConnections(
            "notification.response", payload_json,
            dispatcher().ConnectionsOfCapability(capability_id));
      },
      // The user's focused window, when one is on screen.
      [this]() -> HWND {
        PhiShellWindow* shell = FocusedUserController();
        return shell && ::IsWindowVisible(shell->shell_hwnd())
                   ? shell->shell_hwnd()
                   : nullptr;
      },
      [this] { return Palette(); },
      // The Mac's notification_card_action.
      [this](const char* action) {
        CaptureAnalytics(kAnalyticsModuleNotifications,
                         kEventNotificationCardAction,
                         PhiNotificationCardActionProperties(action));
      });

  // Background update checks (WinSparkle over the Mac's appcast pipeline).
  // A no-op when WinSparkle.dll is not deployed beside the client.
  PhiUpdater::Init(msg_hwnd_, WM_PHI_QUIT_FOR_UPDATE, WM_PHI_UPDATE_STATE);

  // The brand faces for the onboarding titles (and the profile card).
  PhiLoadPrivateFonts();
  // The onboarding gate: decided here, with the stores and the session
  // loaded and no window announced yet (Chromium's startup window follows
  // this callback).
  ResolveInitialAccess();
  SyncChromiumAccount();
}

// static
LRESULT CALLBACK PhiCoordinator::SessionWndProc(HWND hwnd,
                                                UINT msg,
                                                WPARAM wparam,
                                                LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiCoordinator*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_QUERYENDSESSION) {
    // Only a question — the logoff may still be cancelled. Save what is
    // dirty (a flushed store is never wrong) and change nothing else: no
    // phase move, no service stopped, so a cancelled logoff leaves a whole
    // app behind (and QuitForUpdate still runs).
    if (self) {
      self->FlushDirtyStores();
    }
    return TRUE;
  }
  if (msg == WM_ENDSESSION) {
    // wparam TRUE: the session IS ending, and no second chance follows —
    // the shutdown flush now. FALSE: the logoff was cancelled; nothing.
    if (self && wparam) {
      if (self->phase_ < AppPhase::kClosing) {
        self->Transition(AppPhase::kClosing, "endsession");
      }
      self->FlushForShutdown();
    }
    return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

// static
LRESULT CALLBACK PhiCoordinator::MsgWndProc(HWND hwnd,
                                            UINT msg,
                                            WPARAM wparam,
                                            LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_TIMER) {
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) {
      return 0;
    }
    switch (wparam) {
      case kAgentSpaceSweepTimer:
        self->SweepAgentSpaces();  // Agent-space keep-alive sweep (10s).
        return 0;
      case kAgentDriveSweepTimer:
        // Retires holds whose driver went quiet and takeovers that aged out.
        self->agent_manager_.drives().Sweep();
        return 0;
      case kSaveTimer:
        self->FlushDirtyStores();
        return 0;
      case kColdStartFallbackTimer:
        self->session_restore_->OnColdStartFallbackTimer();
        return 0;
      default:
        return 0;
    }
  }
  if (msg == WM_PHI_AUTOVIEW) {
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
      self->AutoViewReevaluate();
    }
    return 0;
  }
  if (msg == WM_PHI_DISMISS_PEEKS) {
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    const HWND target = reinterpret_cast<HWND>(wparam);
    if (self && ::IsWindow(target)) {
      const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      self->surface_coordinator_.DismissPeeksForShell(
          ::GetAncestor(target, GA_ROOT), target, &pt);
      if (::IsWindow(target)) {
        self->surface_coordinator_.ActivatePaneUnderPress(target);
      }
    }
    return 0;
  }
  if (msg == WM_PHI_READER_RETIRE) {
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
      self->surface_coordinator_.RetireReaderPanel(
          BrowserId{static_cast<int64_t>(wparam)});
    }
    return 0;
  }
  if (msg == WM_PHI_READER_EXPAND) {
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
      self->surface_coordinator_.FinishReaderPanelExpand(
          BrowserId{static_cast<int64_t>(wparam)});
    }
    return 0;
  }
  if (msg == WM_PHI_SURFACE_TURN) {
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
      self->surface_coordinator_.RunDeferredSurfaceWork();
    }
    return 0;
  }
  if (msg == WM_PHI_OPEN_ONBOARDING) {
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && !self->CanUseBrowser()) {
      self->OpenOnboarding(
          static_cast<PhiOnboardingWindow::Page>(static_cast<int>(wparam)));
    }
    return 0;
  }
  if (msg == WM_PHI_REFRESH_CARDS) {
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->notification_cards_) {
      self->notification_cards_->Refresh();
    }
    return 0;
  }
  if (msg == WM_PHI_UPDATE_STATE) {
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
      for (PhiShellWindow* controller : self->registry_.Controllers()) {
        controller->NoteUpdateAvailabilityChanged();
      }
    }
    return 0;
  }
  if (msg == WM_PHI_QUIT_FOR_UPDATE) {
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
      self->QuitForUpdate();
    }
    return 0;
  }
  if (msg == WM_PHI_AUTH_CHANGED) {
    // The sign-in/refresh worker settled; the Account pane re-reads state,
    // and the feedback outbox learns whose jobs to work on.
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
      self->account_.OnAuthChanged();
    }
    return 0;
  }
  if (msg == WM_PHI_PUSH_SPACES) {
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
      self->FlushSpacePushes();
    }
    return 0;
  }
  if (msg == WM_PHI_ASK_SPACE) {
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->url_router_) {
      self->url_router_->RunPendingAsk();
    }
    return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

void PhiCoordinator::FlushForShutdown() {
  // An onboarding still up is the funnel's app_terminated interruption
  // (the Mac's recordOOBEAppTermination); before the spool closes below.
  if (onboarding_) {
    onboarding_->NoteAppTerminating();
  }
  // Coalesced saves land now rather than on a timer that will never fire.
  // The restore file is written whether or not it is dirty: the last-active
  // Space is re-derived here from the live window, and the write is cheap
  // insurance for a ghost record noted inside the replay.
  session_restore_->PrepareForShutdown();  // The live window's Space.
  dirty_stores_ |= StoreBit(StoreDomain::kRestore);
  FlushDirtyStores();
  // The spool and the outbox worker: both are best-effort and idempotent
  // (a second call finds no thread to join and rewrites the same spool).
  PhiTelemetry::Get().Stop();
  account_.FlushForShutdown();
  PhiUpdater::Shutdown();
  if (phase_ < AppPhase::kFlushed) {
    Transition(AppPhase::kFlushed, "flushed");
  }
}

// static
const char* PhiCoordinator::AppPhaseName(AppPhase phase) {
  switch (phase) {
    case AppPhase::kBooting:
      return "booting";
    case AppPhase::kStoresLoaded:
      return "storesLoaded";
    case AppPhase::kRestorePlanned:
      return "restorePlanned";
    case AppPhase::kRestoring:
      return "restoring";
    case AppPhase::kSettled:
      return "settled";
    case AppPhase::kRunning:
      return "running";
    case AppPhase::kClosing:
      return "closing";
    case AppPhase::kFlushed:
      return "flushed";
  }
  return "unknown";
}

bool PhiCoordinator::Transition(AppPhase next, const char* reason) {
  if (next == phase_) {
    return false;
  }
  if (next < phase_) {
    // Refused, not clamped: a phase is a promise the rest of the coordinator
    // relies on (stores loaded, restore decided, flushed), and a caller that
    // wants to go back has misread where the app is.
    LOG(WARNING) << "[PhiLifecycle] refused " << AppPhaseName(phase_) << " -> "
                 << AppPhaseName(next) << " (" << reason << ")";
    return false;
  }
  LOG(INFO) << "[PhiLifecycle] " << AppPhaseName(phase_) << " -> "
            << AppPhaseName(next) << " (" << reason << ")";
  phase_ = next;
  return true;
}

void PhiCoordinator::ScheduleSave(StoreDomain domain) {
  const bool armed = dirty_stores_ != 0;
  dirty_stores_ |= StoreBit(domain);
  if (phase_ >= AppPhase::kFlushed || !msg_hwnd_) {
    // Past the flush nothing will run the timer, and before the message
    // window exists nothing can: write now rather than lose it.
    FlushDirtyStores();
    return;
  }
  if (!armed) {
    ::SetTimer(msg_hwnd_, kSaveTimer, kSaveDelayMs, nullptr);
  }
}

void PhiCoordinator::SaveNow(StoreDomain domain) {
  dirty_stores_ |= StoreBit(domain);
  FlushDirtyStores();
}

void PhiCoordinator::FlushDirtyStores() {
  if (msg_hwnd_) {
    ::KillTimer(msg_hwnd_, kSaveTimer);
  }
  // Taken before writing: a Save* that marks a domain dirty again (none
  // does today) must land in the next flush, not loop this one.
  const uint32_t dirty = dirty_stores_;
  dirty_stores_ = 0;
  if (dirty & StoreBit(StoreDomain::kSettings)) {
    preferences_.Save();
  }
  if (dirty & StoreBit(StoreDomain::kSpaces)) {
    space_manager_->Save();
    // Every Space-list or rule change reaches here; keep the page context
    // menu's "Open Link in Space" list and the routing table in step.
    PushSpaceMenu();
  }
  if (dirty & StoreBit(StoreDomain::kRestore)) {
    session_restore_->Save();
  }
  if (dirty & StoreBit(StoreDomain::kConsent)) {
    consent_store_.Save(consent_);
  }
}

void PhiCoordinator::MirrorProfileNames() {
  // The Mac keeps each Chromium profile's display name on its ProfileModel
  // row (upsertProfileDisplayNames), so the store — and a backup of it —
  // can name the profiles its Spaces are bound to.
  if (!framework_ || !local_store_.is_open()) {
    return;
  }
  std::optional<base::Value> list = base::JSONReader::Read(
      framework_->ListProfilesJson(), base::JSON_PARSE_RFC);
  if (!list || !list->is_list()) {
    return;
  }
  std::map<std::string, std::string> names;
  for (const base::Value& entry : list->GetList()) {
    const base::DictValue* d = entry.GetIfDict();
    const std::string* id = d ? d->FindString("id") : nullptr;
    const std::string* name = d ? d->FindString("name") : nullptr;
    if (id && name && !id->empty()) {
      names[*id] = *name;
    }
  }
  local_store_.UpsertProfileDisplayNames(names);
}

void PhiCoordinator::OnProfileLoaded(const char* profile_id) {
  const std::string id = profile_id ? profile_id : "";
  LOG(INFO) << "[PhiClient] OnProfileLoaded " << id;
  if (!framework_ || id.empty()) {
    return;
  }
  MirrorProfileNames();
  // A profile the key layer has not seen (created, or loaded for the first
  // time): unlock and resolve again so it gets its account key.
  if (sync_keys_) {
    std::set<std::string> ids;
    for (const keys::LocalProfile& local : SyncLocalProfiles()) {
      ids.insert(local.profile_id);
    }
    if (ids != sync_profile_ids_) {
      sync_profile_ids_ = std::move(ids);
      sync_keys_->SilentUnlockAndResolve();
    }
  }
  // Per profile: Chromium's SessionStartupPref is the profile's own copy
  // of the app-wide "Restore last session" choice, so each one takes it as
  // it loads (a cold Space switch, CreateProfile, RequestBrowser included).
  framework_->SetRestoreOnStartup(id.c_str(),
                                  preferences_.RestoreLastSession());
  if (global_mirrors_pushed_) {
    return;
  }
  global_mirrors_pushed_ = true;
  // Global, once: the peek and Kiosk switches live on the framework, not a
  // profile; the display language reconciles a PhiPreferences.json edited while
  // the browser was closed — Chromium read its own locale pref long before
  // this, and the first window is still to come, so it comes up in the
  // chosen language rather than flashing the old one.
  PushPeekPreferences();
  PushKioskPreferences();
  PushDisplayLanguageToChromium();
}

void PhiCoordinator::CompleteProfileReply(int reply_id, std::string json) {
  dispatcher().CompletePendingReply(reply_id, std::move(json));
}

void PhiCoordinator::FillSpaceFormProfile(SlotId slot,
                                          const std::string& profile_id) {
  if (PhiShellWindow* controller = ControllerForSlot(slot)) {
    controller->OnSpaceFormProfileCreated(profile_id);
  }
}

void PhiCoordinator::OpenSpaceOnNewProfile(SlotId slot,
                                           const std::string& profile_id) {
  base::DictValue payload;
  payload.Set("name",
              "Space " + std::to_string(space_manager_->next_space_id()));
  payload.Set("profileId", profile_id);
  std::string json;
  base::JSONWriter::Write(payload, &json);
  const std::string created = dispatcher().ExecuteControlOnUi(
      "agentSpace.spaces.create", json,
      PhiControlDispatcher::ControlCaller::kInternal);
  std::optional<base::DictValue> r =
      base::JSONReader::ReadDict(created, base::JSON_PARSE_RFC);
  const std::string* space_id = r ? r->FindString("id") : nullptr;
  if (space_id) {
    RequestSwitchSpace(slot.value, *space_id);
  }
}

void PhiCoordinator::NoteProfilesChanged() {
  NotifySettingsObservers(PhiSettingsChange::kProfiles);
}

bool PhiCoordinator::AnySpaceUsesProfile(const std::string& profile_id) const {
  for (const Space& space : space_manager_->spaces()) {
    if (space.profile_id == profile_id) {
      return true;
    }
  }
  // A sync-hidden Space is still stored and bound: deleting its profile
  // would strand it (and its data) the moment it is un-hidden.
  for (const Space& space : space_manager_->hidden_spaces()) {
    if (space.profile_id == profile_id) {
      return true;
    }
  }
  return false;
}

void PhiCoordinator::UpdateSweepTimer() {
  if (!msg_hwnd_) {
    return;
  }
  const bool needed = !agent_manager_.Empty() || registry_.HasPending() ||
                      profile_service_->HasPending();
  if (needed) {
    // Re-arming a running timer only restarts its interval; a sweep delayed
    // by one tick is harmless, a sweep that never runs is not.
    ::SetTimer(msg_hwnd_, kAgentSpaceSweepTimer, kAgentSpaceSweepIntervalMs,
               nullptr);
  } else {
    ::KillTimer(msg_hwnd_, kAgentSpaceSweepTimer);
  }
}

HWND PhiCoordinator::CredentialPromptOwner() {
  for (PhiShellWindow* controller : registry_.Controllers()) {
    if (controller->shell_hwnd()) {
      return controller->shell_hwnd();
    }
  }
  return nullptr;
}

void PhiCoordinator::NoteCredentialConsentChanged() {
  NotifySettingsObservers(PhiSettingsChange::kConsent);
}

void PhiCoordinator::PushSpaceMenu() {
  SchedulePush(kPushMenu | kPushRouting);
}

void PhiCoordinator::SchedulePushRoutingTable() {
  SchedulePush(kPushRouting);
}

void PhiCoordinator::SchedulePushSpaceMenu() {
  PushSpaceMenu();
}

void PhiCoordinator::SchedulePush(uint32_t bits) {
  const bool posted = pending_pushes_ != 0;
  pending_pushes_ |= bits;
  if (!msg_hwnd_ || !space_manager_) {
    FlushSpacePushes();  // Nothing to post to: push now.
    return;
  }
  if (!posted) {
    ::PostMessageW(msg_hwnd_, WM_PHI_PUSH_SPACES, 0, 0);
  }
}

void PhiCoordinator::FlushSpacePushes() {
  const uint32_t bits = pending_pushes_;
  pending_pushes_ = 0;
  if (!framework_ || !space_manager_) {
    return;
  }
  if (bits & kPushMenu) {
    space_manager_->PushSpaceMenuNow();
  }
  // The routing table shares the menu's trigger set (rules edits add one
  // more), the Mac's pushSpaceStateToChromium coupling.
  if (bits & (kPushMenu | kPushRouting)) {
    url_router_->PushRoutingTableNow();
  }
}

void PhiCoordinator::PostAskSpace() {
  if (msg_hwnd_) {
    ::PostMessageW(msg_hwnd_, WM_PHI_ASK_SPACE, 0, 0);
  }
}

void PhiCoordinator::RequestSpaceBrowser(const std::string& profile_id,
                                         PendingCreation pending) {
  const bool incognito = model::IsIncognitoSpaceId(pending.space_id);
  RequestCreation(phi_framework::WindowKind::kSpace, profile_id, incognito,
                  BrowserId(), std::move(pending));
}

std::string PhiCoordinator::ResolveIncognitoRouteSpace() {
  const std::vector<std::string> live = space_manager_->IncognitoSpaceIds();
  return live.empty() ? space_manager_->CreateIncognitoSpace() : live.front();
}

bool PhiCoordinator::OwnerForSlot(SlotId slot, std::string* profile_id,
                                  std::string* space_id) const {
  PhiShellWindow* controller = registry_.Get(slot);
  if (!controller) {
    return false;
  }
  // Off the record — an incognito window, or a shell showing an Incognito
  // Space — owns no rows. Every store write keyed on a slot (Ctrl+D,
  // AddBookmarkRow/Folder, a drop into the sidebar, a split, the verbs,
  // the Chrome import) resolves its owner here, and would otherwise file
  // an incognito page in the regular, synced store.
  if (registry_.IsIncognitoSlot(slot) || controller->is_incognito() ||
      model::IsIncognitoSpaceId(controller->active_space())) {
    return false;
  }
  *space_id = controller->active_space();
  *profile_id = "Default";
  if (const Space* space = space_manager_->Find(*space_id)) {
    *profile_id = space->profile_id;
  }
  return true;
}

std::string PhiCoordinator::LiveUrl(SlotId slot) const {
  const phi_framework::WindowSnapshot* window =
      window_model_.Get(ResolveBrowser(slot));
  return window ? window->nav.url : std::string();
}

std::string PhiCoordinator::CaptureFavicon(SlotId slot, int* width,
                                           int* height) {
  *width = 0;
  *height = 0;
  if (!framework_) {
    return std::string();
  }
  const BrowserId browser = ResolveBrowser(slot);
  const TabId active = ActiveTabId(browser);
  if (!active.valid()) {
    return std::string();
  }
  const unsigned char* pixels = framework_->TabFaviconBgraById(
      browser.value, active.value, width, height);
  if (!pixels || *width <= 0 || *height <= 0) {
    return std::string();
  }
  const size_t bytes =
      static_cast<size_t>(*width) * static_cast<size_t>(*height) * 4;
  // SAFETY: the framework reports the dimensions of the buffer it returned.
  const auto view = UNSAFE_BUFFERS(base::span<const uint8_t>(pixels, bytes));
  return base::Base64Encode(view);
}

namespace {

// Whether `bytes` is exactly a width x height BGRA image. A stored favicon
// whose size disagrees with its dimensions (a corrupt or synced-in row)
// would have the painter read past the end of the buffer: it is dropped.
bool BgraSizeMatches(size_t bytes, int width, int height) {
  return width > 0 && height > 0 &&
         bytes == static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
}

// Store row → chrome view, decoding the favicon ONCE here instead of at
// every consumer (the old JSON interface base64-round-tripped per rebuild).
PhiStoredRowView StoredRowView(const PhiRow& row) {
  PhiStoredRowView view;
  view.guid = row.guid;
  view.title = row.title;
  view.url = row.url;
  view.folder = row.type == PhiRowType::kBookmarkFolder;
  view.url_b = row.url_b;
  view.title_b = row.title_b;
  view.icon = row.icon;
  view.layout = row.layout;
  // The sidebar reads the store's raw bytes (PhiFaviconForm::kRaw), sized
  // and checked there; base64 is the fallback for any other reader.
  if (!row.favicon_raw.empty()) {
    if (BgraSizeMatches(row.favicon_raw.size(), row.favicon_width,
                        row.favicon_height)) {
      view.favicon_bgra.assign(row.favicon_raw.begin(), row.favicon_raw.end());
      view.favicon_width = row.favicon_width;
      view.favicon_height = row.favicon_height;
    }
  } else if (!row.favicon_base64.empty()) {
    std::string decoded;
    if (base::Base64Decode(row.favicon_base64, &decoded) &&
        BgraSizeMatches(decoded.size(), row.favicon_width,
                        row.favicon_height)) {
      view.favicon_bgra.assign(decoded.begin(), decoded.end());
      view.favicon_width = row.favicon_width;
      view.favicon_height = row.favicon_height;
    }
  }
  if (!row.favicon_b_raw.empty()) {
    if (BgraSizeMatches(row.favicon_b_raw.size(), row.favicon_b_width,
                        row.favicon_b_height)) {
      view.favicon_b_bgra.assign(row.favicon_b_raw.begin(),
                                 row.favicon_b_raw.end());
      view.favicon_b_width = row.favicon_b_width;
      view.favicon_b_height = row.favicon_b_height;
    }
  } else if (!row.favicon_b_base64.empty()) {
    std::string decoded;
    if (base::Base64Decode(row.favicon_b_base64, &decoded) &&
        BgraSizeMatches(decoded.size(), row.favicon_b_width,
                        row.favicon_b_height)) {
      view.favicon_b_bgra.assign(decoded.begin(), decoded.end());
      view.favicon_b_width = row.favicon_b_width;
      view.favicon_b_height = row.favicon_b_height;
    }
  }
  return view;
}

}  // namespace

void PhiCoordinator::NoteStoredRowSeen(const std::string& guid) {
  const store::Tab* row = local_store_.FindTab(guid);
  if (!row) {
    return;
  }
  // A pinned pair is seen as one: the Mac stamps both halves.
  const std::optional<std::string> partner =
      row->type == store::RowType::kPinnedTab ? row->split_partner_guid
                                              : std::nullopt;
  const store::TimeMs now = store::DefaultEnv().now();
  local_store_.SetLastSeen(guid, now);
  if (partner) {
    local_store_.SetLastSeen(*partner, now);
  }
}

std::vector<PhiStoredRowView> PhiCoordinator::PinnedRows(int64_t slot) const {
  std::vector<PhiStoredRowView> out;
  std::string profile_id, space_id;
  if (OwnerForSlot(SlotId{slot}, &profile_id, &space_id)) {
    for (const PhiRow& row : tab_store_.PinnedFor(profile_id, space_id,
                                                  PhiFaviconForm::kRaw)) {
      out.push_back(StoredRowView(row));
      UpgradeFavicon(SlotId{slot}, row.url, &out.back());
    }
  }
  return out;
}

std::vector<PhiStoredRowView> PhiCoordinator::BookmarkRows(
    int64_t slot,
    const std::string& parent_guid) const {
  std::vector<PhiStoredRowView> out;
  std::string profile_id, space_id;
  if (OwnerForSlot(SlotId{slot}, &profile_id, &space_id)) {
    for (const PhiRow& row :
         tab_store_.BookmarksFor(profile_id, space_id, parent_guid,
                                 PhiFaviconForm::kRaw)) {
      out.push_back(StoredRowView(row));
      UpgradeFavicon(SlotId{slot}, row.url, &out.back());
    }
  }
  return out;
}

std::vector<SidebarStoredItem> PhiCoordinator::SidebarStoredItems(
    int64_t slot) const {
  std::vector<SidebarStoredItem> items;
  std::string profile_id, space_id;
  if (!OwnerForSlot(SlotId{slot}, &profile_id, &space_id)) return items;
  // Keys only: no favicon is read, let alone encoded, on this per-rebuild
  // path.
  for (const PhiRow& row : tab_store_.PinnedFor(profile_id, space_id,
                                                PhiFaviconForm::kNone)) {
    items.push_back({row.guid, SidebarStoredKind::kPin, {}, row.index,
                     !row.url_b.empty()});
  }
  // Read the entire owned tree, including children of collapsed folders.
  // Expansion is presentation state and cannot decide whether a tab binds.
  for (const PhiRow& row : tab_store_.BookmarkTree(profile_id, space_id,
                                                   PhiFaviconForm::kNone)) {
    items.push_back({row.guid,
                     row.type == PhiRowType::kBookmarkFolder
                         ? SidebarStoredKind::kFolder
                         : SidebarStoredKind::kBookmark,
                     row.parent_guid, row.index, !row.url_b.empty()});
  }
  return items;
}

std::map<int, SidebarSurface> PhiCoordinator::AttachedSurfaces(
    int64_t browser) const {
  return surface_coordinator_.registry().Surfaces(BrowserId{browser});
}

// ---- The sidebar's one mutator (docs/sidebar-architecture.md §5) ---------

namespace {

std::string IdsJson(int a, int b) {
  std::string json = "[" + std::to_string(a);
  if (b != 0 && b != a) {
    json += "," + std::to_string(b);
  }
  return json + "]";
}

}  // namespace

std::string PhiCoordinator::StoreTabsById(SlotId slot,
                                          TabId tab_a,
                                          TabId tab_b,
                                          bool as_bookmark,
                                          const std::string& parent_guid,
                                          int insert_index,
                                          bool bind) {
  std::string profile_id, space_id;
  if (!framework_ || !OwnerForSlot(slot, &profile_id, &space_id)) {
    return std::string();
  }
  const BrowserId browser = ResolveBrowser(slot);
  const phi_framework::TabSnapshot* a = window_model_.FindTab(browser, tab_a);
  if (!a || a->url.empty()) {
    return std::string();
  }
  const bool split = tab_b.valid() && tab_b != tab_a;
  const phi_framework::TabSnapshot* b =
      split ? window_model_.FindTab(browser, tab_b) : nullptr;
  if (split && (!b || b->url.empty())) {
    return std::string();
  }
  struct Icon {
    std::string base64;
    int width = 0, height = 0;
  };
  const auto favicon = [&](TabId tab_id) {
    Icon icon;
    const unsigned char* pixels = framework_->TabFaviconBgraById(
        browser.value, tab_id.value, &icon.width, &icon.height);
    if (pixels && icon.width > 0 && icon.height > 0) {
      const size_t bytes = static_cast<size_t>(icon.width) *
                           static_cast<size_t>(icon.height) * 4;
      // SAFETY: TabFaviconBgraById reports the buffer's own dimensions.
      const auto view =
          UNSAFE_BUFFERS(base::span<const uint8_t>(pixels, bytes));
      icon.base64 = base::Base64Encode(view);
    } else {
      icon.width = 0;
      icon.height = 0;
    }
    return icon;
  };
  const Icon icon_a = favicon(tab_a);
  const std::string layout = split ? LiveSplitLayout(browser, tab_a)
                                   : std::string();
  // Pins have no tree; a bookmark lands under `parent_guid`.
  const std::string guid =
      as_bookmark
          ? tab_store_.AddBookmark(profile_id, space_id, parent_guid, a->title,
                                   a->url, /*folder=*/false, icon_a.base64,
                                   icon_a.width, icon_a.height)
          : tab_store_.AddPinned(profile_id, space_id, a->title, a->url,
                                 icon_a.base64, icon_a.width, icon_a.height);
  if (guid.empty()) {
    return guid;
  }
  if (split) {
    const Icon icon_b = favicon(tab_b);
    tab_store_.SetRowSecondary(guid, b->url, b->title, icon_b.base64,
                               icon_b.width, icon_b.height);
    if (!layout.empty()) {
      tab_store_.SetRowLayout(guid, layout);
    }
  }
  if (insert_index >= 0) {
    tab_store_.Reorder(guid, insert_index);
  }
  if (bind) {
    // The Mac's updateTabCustomValue: the live tab(s) carry the cell's
    // guid and move into it.
    TagTab(browser, tab_a.value,
                              TagFor(guid, false).c_str());
    if (split) {
      TagTab(browser, tab_b.value,
                                TagFor(guid, true).c_str());
    }
  }
  registry_.InvalidateAll();
  return guid;
}

void PhiCoordinator::TagTab(BrowserId browser, int tab_id, const char* tag) {
  // A tag is snapshot state. The framework marks the window dirty and a
  // push follows, but the rebuild this gesture posts may run BEFORE that
  // push lands (a posted message vs a Chromium task), so pull now: the
  // binding must be right on the very next frame, not one frame later.
  if (!framework_) {
    return;
  }
  framework_->SetTabTagById(browser.value, tab_id, tag);
  window_model_.Refresh(framework_, browser);
}

TabId PhiCoordinator::OpenCell(BrowserId browser, const std::string& guid) {
  const std::optional<PhiRow> row = tab_store_.Find(guid);
  if (!framework_ || !row || row->url.empty() ||
      row->type == PhiRowType::kBookmarkFolder) {
    return TabId();
  }
  // Copied out: the store may move under the tag writes below.
  const std::string url_b = row->url_b;
  const std::string layout = row->layout;
  const TabId a = framework_->OpenTab(browser, row->url.c_str());
  if (!a.valid()) {
    return TabId();
  }
  TagTab(browser, a.value, TagFor(guid, false).c_str());
  if (!url_b.empty()) {
    const TabId b = framework_->OpenTab(browser, url_b.c_str());
    if (b.valid()) {
      TagTab(browser, b.value,
                                TagFor(guid, true).c_str());
      framework_->CreateSplitViewForTabs(
          browser.value, a.value, b.value,
          layout.empty() ? "vertical" : layout.c_str());
    }
  }
  return a;
}

bool PhiCoordinator::ApplyMove(SlotId slot,
                               const SidebarMove& move,
                               const SidebarTree& tree) {
  const SidebarKey& key = move.key;
  const SidebarPlace& place = move.place;
  if (!framework_ || key.empty() || MoveIsNoop(tree, key, place)) {
    return false;
  }
  const BrowserId browser = ResolveBrowser(slot);
  const bool live = key.kind == SidebarKey::Kind::kTab ||
                    key.kind == SidebarKey::Kind::kSplit;
  const bool stored = key.kind == SidebarKey::Kind::kCell ||
                      key.kind == SidebarKey::Kind::kFolder;
  const SidebarCell* cell = stored ? tree.CellFor(key.id) : nullptr;
  if (stored && !cell) {
    return false;
  }
  const auto refresh = [&] { registry_.InvalidateAll(); };
  if (place.section == SidebarPlace::Section::kTabs) {
    const int anchor = PlaceAnchorTab(tree, place);
    const char* group = place.group_explicit ? place.group_id.c_str() : nullptr;
    if (live) {
      // A reorder (the pair moves as a unit inside the framework), possibly
      // joining or leaving a group on the way.
      const bool ok = framework_->MoveTabBefore(
          browser.value, TabOfKey(tree, key, 0), anchor, group);
      refresh();
      return ok;
    }
    if (key.kind != SidebarKey::Kind::kCell) {
      return false;  // Folders and groups never become tabs.
    }
    // Unpin / un-bookmark into the list (the Mac's movePinnedTabOut /
    // moveBookmarkOut(toNormalTabs:)): the cell's tab(s) — opened first
    // when it was closed — become plain and land at the anchor; the cell
    // goes.
    TabId tab{cell->binding.tab_a ? cell->binding.tab_a
                                  : cell->binding.tab_b};
    if (!tab.valid()) {
      tab = OpenCell(browser, key.id);
    }
    RemoveStoredRow(key.id);  // Clears the tags; the tabs stay plain.
    if (tab.valid() && (anchor != 0 || group)) {
      framework_->MoveTabBefore(browser.value, tab.value, anchor, group);
    }
    refresh();
    return true;
  }
  // A stored destination: the pinned grid or a bookmark folder.
  const bool as_bookmark = place.section == SidebarPlace::Section::kBookmarks;
  const std::string parent = as_bookmark ? place.parent_guid : std::string();
  if (live) {
    // The drag conversions (the Mac's moveNormalTab(toPinnd:/toBookmark:)):
    // the tab(s) become a bound cell at the anchor.
    const TabId tab_b{key.kind == SidebarKey::Kind::kSplit
                          ? TabOfKey(tree, key, 1)
                          : 0};
    return !StoreTabsById(slot, TabId{TabOfKey(tree, key, 0)}, tab_b,
                          as_bookmark, parent,
                          PlaceSiblingIndex(tree, place, ""),
                          /*bind=*/true)
                .empty();
  }
  const bool is_pin = cell->kind == SidebarStoredKind::kPin;
  if (is_pin == !as_bookmark) {
    // Same section: a reorder (pins) or a tree move (bookmarks, folders).
    const int index = PlaceSiblingIndex(tree, place, key.id);
    const bool ok = is_pin ? tab_store_.Reorder(key.id, index)
                           : tab_store_.Move(key.id, parent, index);
    if (ok) {
      refresh();
      NotifySettingsObservers(PhiSettingsChange::kStoredRows);
    }
    return ok;
  }
  if (cell->kind == SidebarStoredKind::kFolder) {
    return false;  // A folder cannot become a pin.
  }
  // Across the two stored sections: the row changes kind, its live panes
  // following it (the Mac's movePinnedTabOut(toBookmark:) and
  // moveBookmarkOut(toPinnedTabs:)).
  return ConvertStoredRow(slot, key.id, as_bookmark, parent,
                          PlaceSiblingIndex(tree, place, ""));
}

bool PhiCoordinator::Apply(int64_t slot_number,
                           const SidebarCommand& command,
                           const SidebarTree& tree) {
  if (!framework_) {
    return false;
  }
  const SlotId slot{slot_number};
  const BrowserId browser = ResolveBrowser(slot);
  const auto refresh = [&] { registry_.InvalidateAll(); };
  // The live split a key stands for: a split row's token, or a split cell's
  // live split when both panes are open.
  const auto split_of = [&](const SidebarKey& key) -> std::string {
    if (key.kind == SidebarKey::Kind::kSplit) {
      return key.id;
    }
    if (key.kind == SidebarKey::Kind::kCell) {
      if (const SidebarCell* cell = tree.CellFor(key.id)) {
        return cell->binding.split_id;
      }
    }
    if (key.kind == SidebarKey::Kind::kTab) {
      if (const phi_framework::TabSnapshot* tab =
              window_model_.FindTab(browser, TabId{key.tab_id()})) {
        return tab->split_id;
      }
    }
    return std::string();
  };
  // A key's tab, opening a closed cell when the command needs a live one.
  const auto live_or_open = [&](const SidebarKey& key) {
    int tab = TabOfKey(tree, key, 0);
    if (tab == 0 && key.kind == SidebarKey::Kind::kCell) {
      tab = OpenCell(browser, key.id).value;
    }
    return tab;
  };
  return std::visit(
      absl::Overload{
          [&](const SidebarMove& move) { return ApplyMove(slot, move, tree); },
          [&](const SidebarActivate& cmd) {
            const int tab = TabOfKey(tree, cmd.key, cmd.pane);
            if (tab == 0) {
              return false;  // A closed cell: the controller opens it.
            }
            const bool ok = framework_->ActivateTabById(browser.value, tab);
            refresh();
            return ok;
          },
          [&](const SidebarClose& cmd) {
            // The close box on an Incognito Space's last tab ends the Space
            // (asked first), as Ctrl+W does.
            if (PhiShellWindow* shell = registry_.Get(slot);
                shell && shell->CloseTabEndsIncognitoSpace()) {
              return true;
            }
            bool ok = false;
            for (int pane : {1, 0}) {  // Pane B first; ids hold either way.
              if (cmd.pane >= 0 && cmd.pane != pane) {
                continue;
              }
              const int tab = TabOfKey(tree, cmd.key, pane);
              if (tab != 0) {
                ok = framework_->CloseTabById(browser.value, tab) || ok;
              }
            }
            refresh();
            return ok;
          },
          [&](const SidebarRemoveCell& cmd) {
            if (cmd.key.kind != SidebarKey::Kind::kCell &&
                cmd.key.kind != SidebarKey::Kind::kFolder) {
              return false;
            }
            RemoveStoredRow(cmd.key.id);
            return true;
          },
          [&](const SidebarFormSplit& cmd) {
            const int a = live_or_open(cmd.a);
            const int b = live_or_open(cmd.b);
            if (a == 0 || b == 0 || a == b) {
              return false;
            }
            // A split never lives in a stored cell by accident: both panes
            // become plain first (their cells stay as closed placeholders).
            TagTab(browser, a, "");
            TagTab(browser, b, "");
            const char* created = framework_->CreateSplitViewForTabs(
                browser.value, a, b,
                cmd.layout.empty() ? "vertical" : cmd.layout.c_str());
            refresh();
            return created && *created;
          },
          [&](const SidebarBreakSplit& cmd) {
            const std::string split_id = split_of(cmd.key);
            if (split_id.empty()) {
              return false;
            }
            if (cmd.key.kind == SidebarKey::Kind::kCell) {
              // The cell stays as a closed split placeholder.
              for (int pane : {0, 1}) {
                if (const int tab = TabOfKey(tree, cmd.key, pane)) {
                  TagTab(browser, tab, "");
                }
              }
            }
            const bool ok = framework_->RemoveSplitById(browser.value, split_id.c_str());
            refresh();
            return ok;
          },
          [&](const SidebarSwapPanes& cmd) {
            const std::string split_id = split_of(cmd.key);
            bool ok = false;
            if (!split_id.empty()) {
              ok = framework_->SwapSplitView(browser.value, split_id.c_str());
            }
            if (cmd.key.kind == SidebarKey::Kind::kCell) {
              // The stored panes and their tags follow the live order.
              const int a = TabOfKey(tree, cmd.key, 0);
              const int b = TabOfKey(tree, cmd.key, 1);
              // A pinned pair's cell is named by its first row, which the
              // swap changes: retag under the name it has now.
              const std::string cell = tab_store_.SwapPanes(cmd.key.id);
              ok = !cell.empty() || ok;
              const std::string& name = cell.empty() ? cmd.key.id : cell;
              if (a) TagTab(browser, a, TagFor(name, true).c_str());
              if (b) TagTab(browser, b, TagFor(name, false).c_str());
            }
            refresh();
            return ok;
          },
          [&](const SidebarSetLayout& cmd) {
            bool ok = false;
            const std::string split_id = split_of(cmd.key);
            if (!split_id.empty()) {
              ok = framework_->UpdateSplitView(browser.value, split_id.c_str(),
                                               cmd.layout.c_str(), -1.0);
            }
            // The stored cell remembers the axis so the split reopens with
            // it — the cell itself, or the cell a split row's pane is bound to.
            std::string guid;
            if (cmd.key.kind == SidebarKey::Kind::kCell) {
              guid = cmd.key.id;
            } else {
              const int tab = TabOfKey(tree, cmd.key, 0);
              const auto owner = tree.owner_of.find(tab);
              if (owner != tree.owner_of.end()) {
                guid = owner->second.id;
              }
            }
            if (!guid.empty()) {
              ok = tab_store_.SetRowLayout(guid, cmd.layout) || ok;
            }
            refresh();
            return ok;
          },
          [&](const SidebarJoinGroup& cmd) {
            const int a = TabOfKey(tree, cmd.key, 0);
            if (a == 0) {
              return false;
            }
            const std::string ids = IdsJson(a, TabOfKey(tree, cmd.key, 1));
            bool ok;
            if (cmd.group_id.empty()) {
              const char* created =
                  framework_->CreateTabGroupForTabs(browser.value, ids.c_str());
              ok = created && *created;
            } else {
              ok = framework_->AddTabsToGroup(browser.value, cmd.group_id.c_str(),
                                              ids.c_str());
            }
            refresh();
            return ok;
          },
          [&](const SidebarLeaveGroup& cmd) {
            const int a = TabOfKey(tree, cmd.key, 0);
            if (a == 0) {
              return false;
            }
            const std::string ids = IdsJson(a, TabOfKey(tree, cmd.key, 1));
            const bool ok = framework_->RemoveTabsFromGroupByIds(browser.value, ids.c_str());
            refresh();
            return ok;
          },
          [&](const SidebarSetGroupCollapsed& cmd) {
            if (cmd.key.kind != SidebarKey::Kind::kGroup) {
              return false;
            }
            const bool ok = framework_->SetTabGroupCollapsed(
                browser.value, cmd.key.id.c_str(), cmd.collapsed);
            refresh();
            return ok;
          },
          [&](const SidebarBookmarkCopy& cmd) {
            const TabId tab_b{cmd.key.kind == SidebarKey::Kind::kSplit
                                  ? TabOfKey(tree, cmd.key, 1)
                                  : 0};
            return !StoreTabsById(slot, TabId{TabOfKey(tree, cmd.key, 0)},
                                  tab_b, /*as_bookmark=*/true, std::string(),
                                  /*insert_index=*/-1, /*bind=*/false)
                        .empty();
          },
          [&](const SidebarCloseSurface& cmd) {
            bool any = false;
            for (int pane : {0, 1}) {
              const TabId tab{TabOfKey(tree, cmd.key, pane)};
              if (tab.valid() && surfaces().ForOrigin(tab)) {
                CloseSurfacesForOrigin(tab);
                any = true;
              }
            }
            return any;
          },
          [&](const SidebarMoveToSpace& cmd) {
            if (cmd.key.kind == SidebarKey::Kind::kCell) {
              return MoveStoredRowToSpace(cmd.key.id, cmd.space_id, cmd.clone);
            }
            const TabId tab{TabOfKey(tree, cmd.key, 0)};
            if (!window_model_.FindTab(browser, tab)) {
              return false;
            }
            RequestTabToSpace(slot, tab, cmd.space_id, !cmd.clone);
            return true;
          },
      },
      command);
}

void PhiCoordinator::UpgradeFavicon(SlotId slot,
                                    const std::string& url,
                                    PhiStoredRowView* view) const {
  if (!framework_ || url.empty()) {
    return;
  }
  // The history-backed icon (async; misses schedule a fetch and repaint on
  // arrival) beats the 16px live-tab capture stored with the row.
  int width = 0;
  int height = 0;
  const unsigned char* pixels = framework_->FaviconForUrlBgra(
      ResolveBrowser(slot).value, url.c_str(), &width, &height);
  if (!pixels || width <= view->favicon_width) {
    return;
  }
  const size_t bytes = static_cast<size_t>(width) * height * 4;
  // SAFETY: FaviconForUrlBgra reports the dimensions of the buffer it just
  // returned.
  const auto span =
      UNSAFE_BUFFERS(base::span<const unsigned char>(pixels, bytes));
  view->favicon_bgra.assign(span.begin(), span.end());
  view->favicon_width = width;
  view->favicon_height = height;
}

void PhiCoordinator::RequestOpen(int64_t slot, const std::string& url) {
  if (framework_ && !url.empty()) {
    framework_->OpenTab(ResolveBrowser(slot), url.c_str());
  }
}

std::string PhiCoordinator::LiveSplitLayout(BrowserId browser,
                                            TabId tab) const {
  const phi_framework::SplitSnapshot* split =
      window_model_.SplitOfTab(browser, tab);
  if (!split) {
    return std::string();
  }
  return split->layout == phi_framework::SplitLayout::kHorizontal
             ? "horizontal"
             : "vertical";
}

void PhiCoordinator::StoreSplitInternal(SlotId slot, TabId tab_a,
                                        TabId tab_b, bool as_bookmark,
                                        int insert_index,
                                        bool bind_bookmark) {
  std::string profile_id, space_id;
  if (!framework_ || !OwnerForSlot(slot, &profile_id, &space_id)) {
    return;
  }
  const BrowserId browser = ResolveBrowser(slot);
  struct Pane {
    std::string title, url, favicon;
    int favicon_width = 0, favicon_height = 0;
  };
  const auto capture = [&](TabId tab, Pane* pane) {
    const phi_framework::TabSnapshot* live = window_model_.FindTab(browser, tab);
    if (!live) {
      return;
    }
    pane->title = live->title;
    pane->url = live->url;
    int width = 0, height = 0;
    const unsigned char* pixels = framework_->TabFaviconBgraById(
        browser.value, tab.value, &width, &height);
    if (pixels && width > 0 && height > 0) {
      const size_t bytes =
          static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
      // SAFETY: TabFaviconBgraById reports the buffer's own dimensions.
      const auto view =
          UNSAFE_BUFFERS(base::span<const uint8_t>(pixels, bytes));
      pane->favicon = base::Base64Encode(view);
      pane->favicon_width = width;
      pane->favicon_height = height;
    }
  };
  Pane a, b;
  capture(tab_a, &a);
  capture(tab_b, &b);
  if (a.url.empty() || b.url.empty()) {
    return;
  }
  // Remember the arrangement the live split is in, so pinning or
  // bookmarking a STACKED split reopens it stacked (the Mac persists the
  // layout on the stored rows, not just on live ones).
  std::string layout = LiveSplitLayout(browser, tab_a);
  // ONE row per split cell, pin or bookmark alike (docs/sidebar-architecture
  // §4.1): pane A is the row, pane B rides url_b/title_b/favicon_b. A live
  // pane binds by tag — TagFor(guid, false) / TagFor(guid, true). A pinned
  // split always binds; a split bookmark binds only on the drag path
  // (the Mac's bindLiveSplit), the menu leaves an independent copy.
  const std::string guid =
      as_bookmark
          ? tab_store_.AddBookmark(profile_id, space_id, std::string(),
                                   a.title, a.url, /*folder=*/false,
                                   a.favicon, a.favicon_width,
                                   a.favicon_height)
          : tab_store_.AddPinned(profile_id, space_id, a.title, a.url,
                                 a.favicon, a.favicon_width,
                                 a.favicon_height);
  if (!guid.empty()) {
    tab_store_.SetRowSecondary(guid, b.url, b.title, b.favicon,
                               b.favicon_width, b.favicon_height);
    if (insert_index >= 0) {
      tab_store_.Reorder(guid, insert_index);
    }
    if (!layout.empty()) {
      tab_store_.SetRowLayout(guid, layout);
    }
    if (!as_bookmark || bind_bookmark) {
      TagTab(browser, tab_a.value,
                                TagFor(guid, false).c_str());
      TagTab(browser, tab_b.value,
                                TagFor(guid, true).c_str());
    }
  }
  registry_.InvalidateAll();
}

// Every live tab, in every browser the model knows, whose tag binds it to
// the stored row `guid` — either pane. Across every browser because a
// profile- or app-scoped pin can be open in any of them.
std::vector<PhiCoordinator::BoundTab> PhiCoordinator::TabsBoundTo(
    const std::string& guid) const {
  std::vector<BoundTab> bound;
  for (const BrowserId browser : window_model_.Browsers()) {
    for (const phi_framework::TabSnapshot& tab :
         window_model_.Get(browser)->tabs) {
      SidebarTag parsed;
      if (ParseTag(tab.tag, &parsed) && parsed.guid == guid) {
        bound.push_back({browser, tab.id, parsed.pane_b});
      }
    }
  }
  return bound;
}

void PhiCoordinator::UnbindTabsOfMissingRows() {
  if (!framework_) {
    return;
  }
  // A synced delete removed rows under open tabs: those tabs become normal
  // tabs again, as RemoveStoredRow makes them.
  std::vector<BoundTab> stale;
  for (const BrowserId browser : window_model_.Browsers()) {
    for (const phi_framework::TabSnapshot& tab : window_model_.Get(browser)->tabs) {
      SidebarTag parsed;
      if (ParseTag(tab.tag, &parsed) && !parsed.guid.empty() &&
          !local_store_.FindTab(parsed.guid)) {
        stale.push_back({browser, tab.id, parsed.pane_b});
      }
    }
  }
  for (const BoundTab& bound : stale) {
    TagTab(bound.browser, bound.tab.value, "");
  }
}

void PhiCoordinator::BeginGuestMigration(const std::string& target) {
  guest_rebind_.clear();
  guest_cleanup_pending_ = false;
  if (data_account_ != kGuestAccountId || target == kGuestAccountId) {
    return;
  }
  // The Guest store is read by path: its pending writes go first, then it
  // closes (the target opens after the import, in OpenDataAccount).
  FlushDirtyStores();
  local_store_.Close();
  const PhiGuestMigrationResult result = PhiMigrateGuestData(
      user_data_dir_, target,
      base::WideToUTF8(
          PhiStr("localData.bookmarks.importedFromGuestFolderTitle")));
  if (result.outcome == PhiGuestMigrationResult::Outcome::kImported) {
    guest_rebind_ = result.mappings.ModelGuids();
    guest_cleanup_pending_ = true;
  } else if (result.outcome != PhiGuestMigrationResult::Outcome::kNothing) {
    // The Guest data stays in the Guest account (kFailedGuestIntact) or
    // waits for a same-account retry at the next launch (kNeedsRecovery).
    LOG(ERROR) << "[PhiClient] guest data not moved: " << result.error;
  }
}

void PhiCoordinator::FinishGuestMigration(const std::string& target) {
  if (framework_ && !guest_rebind_.empty()) {
    // The Mac's rebindWindowControllers(migrationReceipt:): an open tab
    // bound to a Guest row follows it to the row it became.
    std::vector<std::pair<BoundTab, std::string>> moved;
    for (const BrowserId browser : window_model_.Browsers()) {
      for (const phi_framework::TabSnapshot& tab :
           window_model_.Get(browser)->tabs) {
        SidebarTag parsed;
        if (!ParseTag(tab.tag, &parsed)) {
          continue;
        }
        const auto mapped = guest_rebind_.find(parsed.guid);
        if (mapped != guest_rebind_.end() && mapped->second != parsed.guid) {
          moved.push_back({{browser, tab.id, parsed.pane_b},
                           TagFor(mapped->second, parsed.pane_b)});
        }
      }
    }
    for (const auto& [bound, tag] : moved) {
      TagTab(bound.browser, bound.tab.value, tag.c_str());
    }
  }
  guest_rebind_.clear();
  UnbindTabsOfMissingRows();
  if (!guest_cleanup_pending_) {
    return;
  }
  guest_cleanup_pending_ = false;
  std::string error;
  if (!PhiFinalizeGuestCleanup(user_data_dir_, target, &error)) {
    // The journal keeps it: the next launch retries (never a re-import).
    LOG(ERROR) << "[PhiClient] guest directory cleanup deferred: " << error;
  }
}

void PhiCoordinator::RemoveStoredRow(const std::string& guid) {
  if (tab_store_.Remove(guid)) {
    // Clear the identity tag from any tab bound to the removed row (either
    // pane) — it returns to the tab list as a normal tab, and a stale tag
    // would keep it excluded from tab groups (the Phi-managed rule).
    if (framework_) {
      for (const BoundTab& bound : TabsBoundTo(guid)) {
        TagTab(bound.browser, bound.tab.value, "");
      }
    }
    registry_.InvalidateAll();
    NotifySettingsObservers(PhiSettingsChange::kStoredRows);
  }
}

bool PhiCoordinator::SetStoredRowIcon(const std::string& guid,
                                     const std::string& icon) {
  if (!tab_store_.SetRowIcon(guid, icon)) {
    return false;
  }
  registry_.InvalidateAll();
  NotifySettingsObservers(PhiSettingsChange::kStoredRows);
  return true;
}

bool PhiCoordinator::SetStoredRowUrl(const std::string& guid,
                                    const std::string& url) {
  if (!tab_store_.SetRowUrl(guid, url)) {
    return false;
  }
  registry_.InvalidateAll();
  NotifySettingsObservers(PhiSettingsChange::kStoredRows);
  return true;
}

bool PhiCoordinator::IsTabAgentDriven(int tab_id) const {
  return agent_manager_.drives().IsMasked(tab_id);
}

bool PhiCoordinator::IsTabReclaimed(int tab_id) const {
  return agent_manager_.drives().IsReclaimed(tab_id);
}

void PhiCoordinator::RequestReclaimTab(int tab_id) {
  agent_manager_.drives().Reclaim(tab_id);
}

void PhiCoordinator::RequestReleaseTab(int tab_id) {
  agent_manager_.drives().Release(tab_id);
}

std::string PhiCoordinator::LastActiveSpaceId() const {
  return session_restore_->LastActiveSpaceId();
}

void PhiCoordinator::NoteActiveSpace(const std::string& space_id) {
  if (closing_all_windows_) {
    return;  // Frozen by QuitForUpdate on the focused window's Space.
  }
  if (PhiIsAgentSpaceId(space_id) || model::IsIncognitoSpaceId(space_id)) {
    // A hosted agent Space is never the one to restore first, and an
    // Incognito Space does not outlive the run (the slot is remembered on
    // its last regular Space, the Mac's persistableLandingSpaceId).
    return;
  }
  session_restore_->NoteActiveSpace(space_id);
}

// The live user window's Space, for the restore service's LastActiveSpaceId.
std::string PhiCoordinator::ActiveSpaceOfLiveWindow() const {
  // A regular window's regular Space, the foreground one first: an
  // incognito window (it nominally shows the default Space) or an
  // Incognito Space is never the Space to restore, and map order says
  // nothing about where the user is.
  const HWND foreground = ::GetForegroundWindow();
  std::string first;
  for (PhiShellWindow* controller : registry_.UserControllers()) {
    const std::string& active = controller->active_space();
    if (active.empty() || model::IsIncognitoSpaceId(active) ||
        PhiIsAgentSpaceId(active)) {
      continue;
    }
    if (controller->shell_hwnd() == foreground) {
      return active;
    }
    if (first.empty()) {
      first = active;
    }
  }
  return first;
}

bool PhiCoordinator::HasSpace(const std::string& space_id) const {
  return space_manager_->Has(space_id);
}

void PhiCoordinator::ArmColdStartFallback(unsigned delay_ms) {
  if (msg_hwnd_) {
    ::SetTimer(msg_hwnd_, kColdStartFallbackTimer, delay_ms, nullptr);
  }
}

void PhiCoordinator::DisarmColdStartFallback() {
  if (msg_hwnd_) {
    ::KillTimer(msg_hwnd_, kColdStartFallbackTimer);
  }
}

void PhiCoordinator::NoteRestorePlanned(const char* reason) {
  Transition(AppPhase::kRestorePlanned, reason);
}

void PhiCoordinator::NoteRestoring() {
  if (phase_ < AppPhase::kRestoring) {
    Transition(AppPhase::kRestoring, "parked");
  }
}

bool PhiCoordinator::RestoreSettled() const {
  return phase_ >= AppPhase::kSettled;
}

void PhiCoordinator::OnRestoreSettled(const char* reason) {
  // A restore that produced no user window (the eager set matched nothing,
  // nothing restored, the cold start parked everything) leaves the process
  // alive with nothing on screen: open a default shell. A no-op when the
  // user has a window, so every path may ask.
  OpenDefaultShellIfNoUserWindow(reason);
  if (phase_ < AppPhase::kSettled) {
    Transition(AppPhase::kSettled, reason);
    // Nothing sits between the two today; kept as distinct phases so a
    // post-settle step (a deferred migration, say) has a slot to land in.
    Transition(AppPhase::kRunning, "settled");
  }
}

void PhiCoordinator::OpenDefaultShellIfNoUserWindow(const char* reason) {
  if (registry_.FirstUserSlot().valid() || !framework_) {
    return;  // The user has a window; nothing to rescue.
  }
  if (!CanUseBrowser()) {
    // Behind the onboarding gate: GrantAccess asks again once the user is
    // through (a shell opened now would only sit hidden).
    LOG(INFO) << "[PhiClient] default-shell rescue deferred by the gate ("
              << reason << ")";
    return;
  }
  // A restore that produced no window would leave the process alive with
  // nothing on screen (lane 4 §6 item 6). Ask for a browser in the default
  // Space's profile with NO slot to attach to, which is how a request says
  // "host it as a new shell" (AttachRequestedSpaceBrowser). One at a time:
  // a second call before the first arrives would open two.
  std::string profile = "Default";
  const std::string default_space = DefaultSpaceId();
  if (const Space* space = space_manager_->Find(default_space)) {
    profile = space->profile_id;
  }
  if (registry_.FindPendingSpace(SlotId(), default_space)) {
    return;
  }
  LOG(WARNING) << "[PhiClient] no user window after restore (" << reason
               << "); opening a default shell in " << profile;
  PendingCreation pending;
  pending.space_id = default_space;
  RequestCreation(phi_framework::WindowKind::kSpace, profile,
                  /*incognito=*/false, BrowserId(), std::move(pending));
}

void PhiCoordinator::SettleRestore(const char* reason) {
  session_restore_->Settle(reason);
}

void PhiCoordinator::OnTabDistillable(int64_t window_id, int tab_id,
                                     bool distillable) {
  // Route to the shell hosting this browser (every browser a slot shows,
  // its original included, is bound in the registry).
  PhiShellWindow* controller =
      registry_.ControllerForBrowser(BrowserId{window_id});
  LOG(INFO) << "[PhiClient] Distillable win=" << window_id << " tab=" << tab_id
            << " d=" << distillable << " routed=" << (controller != nullptr);
  if (controller) {
    controller->NoteTabDistillable(TabId{tab_id}, distillable);
  }
}

void PhiCoordinator::NoteTabDistillable(int64_t window_id, int tab_id,
                                        bool distillable) {
  OnTabDistillable(window_id, tab_id, distillable);
}

void PhiCoordinator::OnExtensionMessage(const char* request_id,
                                        const char* sender_id,
                                        const char* type,
                                        const char* payload_json) {
  const std::string id = request_id ? request_id : "";
  const std::string sender = sender_id ? sender_id : "";
  const std::string message_type = type ? type : "";
  if (folio_.HandleExtensionMessage(
          id, sender, message_type,
          payload_json && *payload_json ? payload_json : "{}")) {
    return;
  }
  if (surface_coordinator_.HandleExtensionMessage(
          id, sender, message_type,
          payload_json && *payload_json ? payload_json : "{}")) {
    return;
  }
  // Nothing here speaks this type. Fail it rather than leave the extension
  // waiting out its own timeout for an answer that will never come.
  LOG(INFO) << "[PhiClient] unhandled extension message " << message_type
            << " from " << sender;
  if (framework_ && !id.empty()) {
    framework_->FailExtensionMessage(id.c_str(), "Unhandled message type");
  }
}

PhiShellWindow* PhiCoordinator::WindowShowingTab(TabId tab_id) {
  const std::optional<BrowserId> browser = window_model_.BrowserOfTab(tab_id);
  if (!browser) {
    return nullptr;
  }
  PhiShellWindow* window = registry_.ControllerForBrowser(*browser);
  // A slot hosts N Space browsers; only the one on screen "shows" the tab.
  return window && window->active_browser_id() == *browser ? window : nullptr;
}

void PhiCoordinator::ScheduleRefreshForTab(TabId tab_id) {
  // The reader button is laid out from this verdict, so the window showing
  // the tab has to repaint. At the next idle: the report can arrive during a
  // navigation the strip has not caught up with yet.
  if (PhiShellWindow* window = WindowShowingTab(tab_id)) {
    window->ScheduleRefresh();
  }
}

SlotId PhiCoordinator::SlotForBrowser(BrowserId browser) const {
  const std::optional<SlotId> slot = registry_.SlotFor(browser);
  return slot ? *slot : SlotId();
}

void PhiCoordinator::PersistReaderStyle(const model::ReaderStyle& style) {
  preferences_.SetReaderStyle(style);
}

bool PhiCoordinator::PendingReplyShape(int request_id,
                                       bool* document,
                                       bool* include_html) {
  return dispatcher().PendingReplyShape(request_id, document, include_html);
}

void PhiCoordinator::CompleteReaderReply(int request_id, std::string json) {
  dispatcher().CompletePendingReply(request_id, std::move(json));
}

void PhiCoordinator::PostReaderExpand(BrowserId panel) {
  if (msg_hwnd_) {
    ::PostMessageW(msg_hwnd_, WM_PHI_READER_EXPAND,
                   static_cast<WPARAM>(panel.value), 0);
  }
}

void PhiCoordinator::PostReaderRetire(BrowserId panel) {
  if (msg_hwnd_) {
    ::PostMessageW(msg_hwnd_, WM_PHI_READER_RETIRE,
                   static_cast<WPARAM>(panel.value), 0);
  }
}

void PhiCoordinator::PostSurfaceTurn() {
  if (msg_hwnd_) {
    ::PostMessageW(msg_hwnd_, WM_PHI_SURFACE_TURN, 0, 0);
  }
}

bool PhiCoordinator::IsReaderOfferable(int tab_id,
                                       const std::string& url) const {
  return surface_coordinator_.IsReaderOfferable(tab_id, url);
}

bool PhiCoordinator::IsReaderActive(int tab_id, const std::string& url) const {
  return surface_coordinator_.IsReaderActive(tab_id, url);
}

void PhiCoordinator::RequestToggleReaderView(int tab_id,
                                             const std::string& url,
                                             const char* entry_point) {
  surface_coordinator_.RequestToggleReaderView(tab_id, url, entry_point);
}

bool PhiCoordinator::CanSaveToFolio(int tab_id) const {
  return folio_.CanSave(tab_id);
}

void PhiCoordinator::RequestSaveToFolio(int tab_id) {
  folio_.Save(tab_id);
}

void PhiCoordinator::RequestOpenFolio() {
  folio_.OpenLibrary();
}

// PhiFolio::Host --------------------------------------------------------------

void PhiCoordinator::RespondToExtension(const std::string& request_id,
                                        const std::string& json) {
  if (framework_ && !request_id.empty()) {
    framework_->RespondToExtensionMessage(request_id.c_str(), json.c_str());
  }
}

bool PhiCoordinator::BroadcastToExtensions(const std::string& type,
                                           const std::string& payload_json) {
  return framework_ &&
         framework_->BroadcastToExtensions(type.c_str(), payload_json.c_str());
}

std::optional<PhiFolio::TabInfo> PhiCoordinator::FolioFindTab(
    int tab_id) const {
  const std::optional<BrowserId> browser =
      window_model_.BrowserOfTab(TabId{tab_id});
  if (!browser) {
    return std::nullopt;
  }
  const PhiWindowModel::Tab* tab = window_model_.FindTab(*browser, TabId{tab_id});
  if (!tab) {
    return std::nullopt;
  }
  PhiFolio::TabInfo info;
  info.window_id = browser->value;
  info.url = tab->url;
  info.title = tab->title;
  return info;
}

int64_t PhiCoordinator::FolioActiveWindow() const {
  // The window in front, else the first user window.
  const HWND foreground = ::GetForegroundWindow();
  PhiShellWindow* chosen = nullptr;
  for (PhiShellWindow* controller : registry_.Controllers()) {
    if (!controller->active_browser_id().valid()) {
      continue;
    }
    if (!chosen || controller->shell_hwnd() == foreground) {
      chosen = controller;
    }
  }
  return chosen ? chosen->active_browser_id().value : 0;
}

std::string PhiCoordinator::FolioProfileOfWindow(int64_t window_id) const {
  if (!framework_ || window_id == 0) {
    return std::string();
  }
  const char* json = framework_->WindowProfileJson(window_id);
  std::optional<base::DictValue> profile =
      base::JSONReader::ReadDict(json ? json : "", base::JSON_PARSE_RFC);
  const std::string* id = profile ? profile->FindString("id") : nullptr;
  return id ? *id : std::string();
}

std::vector<int64_t> PhiCoordinator::FolioWindowPerProfile() const {
  std::vector<int64_t> windows;
  std::set<std::string> profiles;
  for (PhiShellWindow* controller : registry_.Controllers()) {
    const BrowserId browser = controller->active_browser_id();
    if (!browser.valid()) {
      continue;
    }
    const std::string profile = FolioProfileOfWindow(browser.value);
    if (!profile.empty() && profiles.insert(profile).second) {
      windows.push_back(browser.value);
    }
  }
  return windows;
}

HWND PhiCoordinator::FolioShellForWindow(int64_t window_id) const {
  if (PhiShellWindow* window =
          registry_.ControllerForBrowser(BrowserId{window_id})) {
    return window->shell_hwnd();
  }
  return ShellForFocus();
}

void PhiCoordinator::FolioShowToast(int64_t window_id,
                                    const std::wstring& title,
                                    const std::wstring& message,
                                    int duration_ms,
                                    const std::wstring& action_label,
                                    std::function<void()> action) {
  PhiShellWindow* window =
      registry_.ControllerForBrowser(BrowserId{window_id});
  if (!window) {
    window = registry_.ControllerForBrowser(BrowserId{FolioActiveWindow()});
  }
  if (!window || !window->shell_hwnd()) {
    return;
  }
  PhiShowToastWithAction(window->shell_hwnd(), window->CurrentPalette(), title,
                         message, duration_ms, action_label,
                         std::move(action));
}

PhiPreferences& PhiCoordinator::FolioPreferences() {
  return preferences_;
}

void PhiCoordinator::FolioOpenTab(const std::string& url) {
  const int64_t window = FolioActiveWindow();
  if (framework_ && window != 0) {
    framework_->OpenTab(BrowserId{window}, url.c_str());
  }
}

void PhiCoordinator::SetFolioGlobalFolder(const std::string& path) {
  preferences_.SetFolioGlobalFolder(path);
}

void PhiCoordinator::SetFolioFolderOverride(const std::string& profile_id,
                                            const std::string& path) {
  preferences_.SetFolioFolderOverride(profile_id, path);
}

void PhiCoordinator::SetFolioAutoSaveOnSiteActions(bool enabled) {
  preferences_.SetFolioAutoSaveOnSiteActions(enabled);
}

void PhiCoordinator::OpenFolioAutoSaveSites() {
  folio_.OpenAutoSaveSites();
}

void PhiCoordinator::RequestCloseSurfaces(int tab_id) {
  surface_coordinator_.CloseSurfacesForOrigin(TabId{tab_id});
}

bool PhiCoordinator::RequestClosePartnerPeek(int64_t browser, int tab_id) {
  return surface_coordinator_.ClosePartnerPeek(BrowserId{browser},
                                               TabId{tab_id});
}

bool PhiCoordinator::SurfaceFaviconForOrigin(int tab_id,
                                             std::vector<unsigned char>* bgra,
                                             int* width,
                                             int* height) {
  bgra->clear();
  *width = *height = 0;
  const PhiSurfacePanel* panel =
      surface_coordinator_.registry().ForOrigin(TabId{tab_id});
  if (!panel || !framework_) {
    return false;
  }
  // A reader shows the ORIGIN page (reader.html has no favicon of its
  // own); a peek shows the page it previews.
  int64_t window_id = panel->window_id();
  int64_t tab = panel->surface_tab_id();
  if (panel->kind() == SidebarSurface::kReader) {
    window_id = panel->origin_window_id();
    tab = panel->origin_tab_id();
  }
  if (tab <= 0) {
    return false;
  }
  const unsigned char* pixels = framework_->TabFaviconBgraById(
      window_id, static_cast<int>(tab), width, height);
  if (!pixels || *width <= 0 || *height <= 0) {
    return false;
  }
  const size_t bytes =
      static_cast<size_t>(*width) * static_cast<size_t>(*height) * 4;
  // SAFETY: the framework reports the dimensions of the buffer it returned.
  const auto view = UNSAFE_BUFFERS(base::span<const unsigned char>(pixels, bytes));
  bgra->assign(view.begin(), view.end());
  return true;
}

void PhiCoordinator::RaiseReaderPanels(int64_t slot) {
  surface_coordinator_.RaiseReaderPanels(SlotId{slot});
}

void PhiCoordinator::NoteContentLayoutChanged(int64_t slot) {
  surface_coordinator_.NoteContentLayoutChanged(SlotId{slot});
}

void PhiCoordinator::CanonicalizePinTags(BrowserId window_id) {
  const phi_framework::WindowSnapshot* snapshot = window_model_.Get(window_id);
  if (!framework_ || !snapshot) {
    return;
  }
  bool changed = false;
  for (const phi_framework::TabSnapshot& tab : snapshot->tabs) {
    SidebarTag parsed;
    if (tab.tag.empty() || !ParseTag(tab.tag, &parsed)) {
      continue;
    }
    const store::Tab* row = local_store_.FindTab(parsed.guid);
    if (!row || row->type != store::RowType::kPinnedTab) {
      continue;  // Bookmarks are one row: their tags never drift.
    }
    // The row this tab SHOWS: the tagged one, or for "#b" its partner.
    std::string shows = row->guid;
    if (parsed.pane_b) {
      if (!row->split_partner_guid) {
        continue;
      }
      shows = *row->split_partner_guid;
    }
    const std::optional<PhiRow> cell = tab_store_.Find(shows);
    if (!cell) {
      continue;
    }
    const std::string canonical = TagFor(cell->guid, cell->guid != shows);
    if (canonical != tab.tag) {
      TagTab(window_id, tab.id.value, canonical.c_str());
      changed = true;
    }
  }
  if (changed) {
    registry_.InvalidateAll();
  }
}

TabId PhiCoordinator::ActiveTabId(BrowserId browser) const {
  const phi_framework::TabSnapshot* active = window_model_.ActiveTab(browser);
  return active ? active->id : TabId();
}

void PhiCoordinator::NoteSlotActiveSpace(const std::string& space_id) {
  NoteActiveSpace(space_id);
}

void PhiCoordinator::NoteSpaceWindow(const std::string& space_id,
                                     int64_t browser_window_id) {
  if (model::IsIncognitoSpaceId(space_id)) {
    return;  // Off the record: never a window a later run rebuilds.
  }
  std::string profile = "Default";
  if (const Space* space = space_manager_->Find(space_id)) {
    profile = space->profile_id;
  }
  const BrowserId browser{browser_window_id};
  // Release review B6: a second window of the Space (Ctrl+N, a tear-off,
  // a switch in another shell, a sibling restored first) must not take the
  // Space's remembered row while the window it names still holds tabs —
  // the overwritten window was orphan-dropped on the next restore. It is
  // remembered beside it instead and comes back as a shell of its own. A
  // window already remembered that way stays so.
  if (session_restore_->HasSecondaryWindow(space_id, browser)) {
    session_restore_->NoteSecondaryWindow(space_id, profile, browser);
    return;
  }
  const std::optional<PhiGhostSpaces::Remembered> primary =
      session_restore_->PrimaryWindow(space_id);
  bool primary_live_elsewhere = false;
  if (primary && primary->window_id != browser) {
    for (PhiShellWindow* controller : registry_.UserControllers()) {
      if (controller->BrowserIdForSpace(space_id) == primary->window_id) {
        primary_live_elsewhere = true;
        break;
      }
    }
  }
  if (PhiSessionRestore::KeepsPrimary(
          primary.has_value(), primary && primary->window_id == browser,
          session_restore_->Parked(space_id) != nullptr,
          session_restore_->PrimaryAwaited(space_id),
          primary_live_elsewhere)) {
    session_restore_->NoteSecondaryWindow(space_id, profile, browser);
    return;
  }
  session_restore_->NoteLiveWindow(space_id, profile, browser);
}

void PhiCoordinator::RecordAnalyticsEvent(const char* module,
                                          const char* event_name,
                                          const char* properties_json) {
  if (!module || !event_name) {
    return;
  }
  // Out to the collector (a no-op until one is configured), and into the ring
  // below so the harness can see what crossed regardless.
  PhiTelemetry::Get().RecordEvent(module, event_name,
                                  properties_json ? properties_json : "{}");
  AnalyticsEvent event;
  event.module = module;
  event.name = event_name;
  event.properties_json = properties_json ? properties_json : "{}";
  // The name and module go to the log; the PROPERTIES do not. The component's
  // privacy contract keeps event data out of logs, and this is a log the user
  // can read off disk.
  VLOG(1) << "[analytics] " << event.module << "/" << event.name;
  analytics_events_.push_back(std::move(event));
  while (analytics_events_.size() > kAnalyticsRingSize) {
    analytics_events_.pop_front();
  }
}

void PhiCoordinator::CaptureAnalytics(const char* module,
                                      const char* event_name,
                                      base::DictValue properties) {
  std::string json;
  base::JSONWriter::Write(properties, &json);
  RecordAnalyticsEvent(module, event_name, json.c_str());
}

void PhiCoordinator::NoteFirstTimeAction(const char* action) {
  base::DictValue props;
  if (first_time_actions_.Record(action, PhiNowUnixMs(), &props)) {
    CaptureAnalytics(kAnalyticsModuleApp, kEventFirstTimeAction,
                     std::move(props));
  }
}

void PhiCoordinator::RequestBrowserImport() {
  PhiShellWindow* focused = FocusedUserController();
  HWND owner = focused ? focused->shell_hwnd() : nullptr;
  std::string profile_id, space_id;
  if (!focused || !framework_ ||
      !OwnerForSlot(focused->slot(), &profile_id, &space_id)) {
    return;
  }
  if (focused->ShowsIncognito() || model::IsIncognitoSpaceId(space_id)) {
    // The Mac's unavailableInIncognito alert: an off-the-record profile
    // keeps nothing an import would bring.
    PhiAlert(owner,
             PhiStr("app.importBrowserData.unavailableInIncognito.title"),
             PhiStr("app.importBrowserData.unavailableInIncognito.message"),
             {{PhiStr("app.importBrowserData.unavailableInIncognito."
                      "dismissButton"),
               PhiAlertRole::kPrimary}},
             0, 0);
    return;
  }
  PhiImportTarget target;
  target.profile_id = profile_id;
  target.space_id = space_id;
  target.window_id = ResolveBrowser(focused->slot()).value;
  // "Space (Profile)" — the Mac's formatImportTargetLabel.
  const model::Space* space = FindSpaceIn(space_manager_->spaces(), space_id);
  std::wstring label =
      space ? base::UTF8ToWide(space->name)
            : std::wstring(PhiStr(
                  "oobe.importBrowserData.target.currentSpaceFallback"));
  const std::string profile_name = ProfileDisplayName(profile_id);
  if (!profile_name.empty()) {
    label += L" (" + base::UTF8ToWide(profile_name) + L")";
  }
  target.label = std::move(label);
  if (!PhiImportWindow::Current()) {
    CaptureAnalytics(kAnalyticsModuleImport, kEventImportViewed,
                     PhiImportViewedProperties());
  }
  PhiImportWindow::Open(this, target, owner);
}

bool PhiCoordinator::StartBrowserImport(const PhiImportRequest& request) {
  // Reentrancy gate: a second start while a run is unresolved would
  // overwrite the source the framework's completion is matched against.
  if (import_run_ || request.sources.empty() || !framework_) {
    return false;
  }
  ImportRun run;
  run.request = request;
  run.started_tick = ::GetTickCount64();
  std::vector<std::string> sources;
  // The Mac sends the selections in source-name order.
  std::vector<PhiImportSource> by_name = request.sources;
  std::sort(by_name.begin(), by_name.end(),
            [](PhiImportSource a, PhiImportSource b) {
              return std::string_view(PhiImportSourceKey(a)) <
                     PhiImportSourceKey(b);
            });
  for (PhiImportSource source : by_name) {
    sources.push_back(PhiImportSourceKey(source));
    std::vector<std::string> types;
    auto it = request.types.find(source);
    if (it != request.types.end()) {
      for (PhiImportDataType type : it->second) {
        types.push_back(PhiImportDataTypeAnalytics(type));
        // Arc's bookmarks come from its sidebar, not from Chromium.
        if (type == PhiImportDataType::kBookmarks &&
            source != PhiImportSource::kArc) {
          run.chromium_bookmarks = true;
        }
      }
    }
    if (source == PhiImportSource::kFile) {
      run.chromium_bookmarks = true;  // The file's bookmarks are staged too.
    }
    CaptureAnalytics(kAnalyticsModuleImport, kEventImportTypesSelected,
                     PhiImportTypesSelectedProperties(
                         PhiImportSourceKey(source), std::move(types)));
  }
  CaptureAnalytics(kAnalyticsModuleImport, kEventImportStarted,
                   PhiImportStartedProperties(sources));
  // Validate the file before anything destructive: a path gone since it
  // was picked drops the file source as failed, not silently.
  if (std::find(run.request.sources.begin(), run.request.sources.end(),
                PhiImportSource::kFile) != run.request.sources.end()) {
    const DWORD attributes =
        ::GetFileAttributesW(run.request.file_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
      std::erase(run.request.sources, PhiImportSource::kFile);
      run.failed.push_back(PhiImportSource::kFile);
      run.error_code = "file_unreadable";
    }
  }
  LOG(INFO) << "[Import] start: " << run.request.sources.size()
            << " source(s) into space " << request.target.space_id
            << " window " << request.target.window_id;
  // The Chromium bookmark model is the staging area: empty it so only this
  // run's bookmarks are copied out afterwards.
  if (run.chromium_bookmarks) {
    framework_->RemoveAllBookmarks(request.target.window_id);
  }
  import_run_ = std::move(run);
  RunNextImport();
  return true;
}

bool PhiCoordinator::BrowserImportRunning() const {
  return import_run_.has_value();
}

void PhiCoordinator::RunNextImport() {
  while (import_run_ && import_run_->next < import_run_->request.sources.size()) {
    ImportRun& run = *import_run_;
    const PhiImportSource source = run.request.sources[run.next++];
    const int64_t window = run.request.target.window_id;
    std::wstring status;
    if (source == PhiImportSource::kFile) {
      status = PhiStr("oobe.importBrowserData.progress.importingFile");
    } else {
      status = PhiStrF("oobe.importBrowserData.progress.importingBrowser",
                       {PhiImportSourceName(source)});
    }
    if (PhiImportWindow* window_ui = PhiImportWindow::Current()) {
      window_ui->SetStatus(status);
    }
    bool started = false;
    std::string types;
    std::string profile;
    if (source != PhiImportSource::kFile) {
      // For Arc the bookmarks are its sidebar's: only the rest goes to
      // Chromium.
      auto it = run.request.types.find(source);
      if (it != run.request.types.end()) {
        for (PhiImportDataType type : it->second) {
          if (source == PhiImportSource::kArc &&
              type == PhiImportDataType::kBookmarks) {
            continue;
          }
          types += (types.empty() ? "" : ",") +
                   std::string(PhiImportDataTypeWire(type));
        }
      }
      if (source == PhiImportSource::kArc) {
        if (types.empty()) {
          continue;  // Bookmarks only: nothing for Chromium to do.
        }
        if (!run.request.arc_space ||
            !run.request.arc_space->profile_directory) {
          // An unreadable Space profile imports its bookmarks only and never
          // falls back to Default's data; say so rather than skip silently.
          LOG(WARNING) << "[Import] Arc data skipped: the Space's profile is "
                          "unresolved";
          run.failed.push_back(source);
          if (run.error_code.empty()) {
            run.error_code = "source_import_failed";
          }
          continue;
        }
        profile = *run.request.arc_space->profile_directory;
      } else {
        auto p = run.request.profiles.find(source);
        if (p != run.request.profiles.end()) {
          profile = p->second;
        }
      }
    }
    // The framework may refuse at once and say so from inside the call: the
    // source is awaited before it is dispatched.
    run.awaiting = source;
    run.early_result.reset();
    run.dispatching = true;
    started = source == PhiImportSource::kFile
                  ? framework_->StartFileImport(
                        window, base::WideToUTF8(run.request.file_path).c_str())
                  : framework_->StartBrowserImport(
                        window, PhiImportSourceKey(source), profile.c_str(),
                        types.c_str());
    run.dispatching = false;
    if (started && run.early_result) {
      run.awaiting.reset();
      LOG(INFO) << "[Import] " << PhiImportSourceKey(source)
                << (*run.early_result ? " finished" : " failed")
                << " at once";
      if (!*run.early_result) {
        run.failed.push_back(source);
        if (run.error_code.empty()) {
          run.error_code = "source_import_failed";
        }
      }
      continue;
    }
    if (!started) {
      run.awaiting.reset();
      LOG(WARNING) << "[Import] " << PhiImportSourceKey(source)
                   << " was not accepted";
      run.failed.push_back(source);
      if (run.error_code.empty()) {
        run.error_code = "source_import_failed";
      }
      continue;
    }
    return;  // OnImportCompleted continues.
  }
  if (import_run_) {
    FinishImportRun();
  }
}

void PhiCoordinator::NoteImportSourceDone(PhiImportSource source,
                                          bool success) {
  ImportRun& run = *import_run_;
  run.awaiting.reset();
  LOG(INFO) << "[Import] " << PhiImportSourceKey(source)
            << (success ? " finished" : " failed");
  if (!success) {
    run.failed.push_back(source);
    if (run.error_code.empty()) {
      run.error_code = "source_import_failed";
    }
  }
  RunNextImport();
}

void PhiCoordinator::FinishImportRun() {
  ImportRun run = std::move(*import_run_);
  const PhiImportTarget& target = run.request.target;
  // The Space may have gone while the sources ran (the Mac's
  // importTargetSpaceIsWritable backstop): nothing lands then.
  const bool space_alive =
      FindSpaceIn(space_manager_->spaces(), target.space_id) != nullptr;
  int landed = 0;
  bool persisted = true;
  if (run.chromium_bookmarks) {
    std::optional<base::Value> tree = base::JSONReader::Read(
        framework_->AllBookmarksJson(target.window_id), base::JSON_PARSE_RFC);
    const int written =
        space_alive && tree && tree->is_list()
            ? PhiLandChromiumBookmarks(tab_store_, target.profile_id,
                                       target.space_id, tree->GetList())
            : -1;
    if (written < 0) {
      persisted = false;
      for (PhiImportSource source : run.request.sources) {
        if (source != PhiImportSource::kArc) {
          run.failed.push_back(source);
        }
      }
    } else {
      landed += written;
    }
  }
  const std::string arc_title =
      base::WideToUTF8(PhiStr("localData.bookmarks.importedFromArcFolderTitle"));
  const auto arc_types = run.request.types.find(PhiImportSource::kArc);
  if (run.request.arc_space && arc_types != run.request.types.end() &&
      std::find(arc_types->second.begin(), arc_types->second.end(),
                PhiImportDataType::kBookmarks) != arc_types->second.end()) {
    if (space_alive) {
      landed += PhiLandArcSpace(tab_store_, target.profile_id, target.space_id,
                                *run.request.arc_space, arc_title);
    } else {
      persisted = false;
      run.failed.push_back(PhiImportSource::kArc);
    }
  }
  if (space_alive) {
    PhiReorderImportedFolders(tab_store_, target.profile_id, target.space_id,
                              arc_title);
  }
  std::sort(run.failed.begin(), run.failed.end());
  run.failed.erase(std::unique(run.failed.begin(), run.failed.end()),
                   run.failed.end());
  if (!persisted && run.error_code.empty()) {
    run.error_code = "bookmark_persistence_failed";
  }
  LOG(INFO) << "[Import] done: " << landed << " bookmark(s) landed, "
            << run.failed.size() << " source(s) failed";

  std::vector<std::string> sources, failed;
  for (PhiImportSource source : run.request.sources) {
    sources.push_back(PhiImportSourceKey(source));
  }
  for (PhiImportSource source : run.failed) {
    sources.push_back(PhiImportSourceKey(source));  // file_unreadable's file
    failed.push_back(PhiImportSourceKey(source));
  }
  CaptureAnalytics(
      kAnalyticsModuleImport, kEventImportFinished,
      PhiImportFinishedProperties(
          std::move(sources), std::move(failed),
          static_cast<int>((::GetTickCount64() - run.started_tick) / 1000),
          run.failed.empty() ? std::string() : run.error_code));
  if (run.failed.empty()) {
    NoteFirstTimeAction(kFirstTimeActionImportFinished);
  }
  import_run_.reset();
  registry_.InvalidateAll();
  NotifySettingsObservers(PhiSettingsChange::kProfiles);
  if (PhiImportWindow* window = PhiImportWindow::Current()) {
    window->Finish(run.failed);
  } else if (!run.failed.empty()) {
    // The window was closed mid-run; the failure still gets said.
    std::wstring names;
    for (PhiImportSource source : run.failed) {
      names += (names.empty() ? L"" : L", ") +
               std::wstring(PhiImportSourceName(source));
    }
    PhiShellWindow* focused = FocusedUserController();
    PhiNotice(focused ? focused->shell_hwnd() : nullptr, PhiStr("app.import"),
              PhiStrF("oobe.importBrowserData.progress.completedWithErrors",
                      {names})
                  .c_str());
  }
}

void PhiCoordinator::NoteAppActivation(bool active) {
  // WM_ACTIVATEAPP reaches every top-level window of the process on each
  // transition. No analytics ride it any more: the Opened / Backgrounded
  // pair this used to capture was the Mac SDK's Cmd-Tab noise, and the Mac
  // (e748d9f4) now sends Application Opened once per launch (LoadStores).
  if (accelerators_) {
    accelerators_->OnAppActivationChanged();  // Alt+Tab is no solo Alt.
  }
}

void PhiCoordinator::NoteUserSpaceSwitched() {
  CaptureAnalytics(kAnalyticsModuleSpaces, kEventSpaceSwitched,
                   PhiSpaceSwitchedProperties(
                       static_cast<int>(space_manager_->spaces().size())));
}

bool PhiCoordinator::SetStoredRowLayout(const std::string& guid,
                                       const std::string& layout) {
  if (!tab_store_.SetRowLayout(guid, layout)) {
    return false;
  }
  registry_.InvalidateAll();
  return true;
}

bool PhiCoordinator::RenameStoredRow(const std::string& guid,
                                     const std::string& name) {
  if (!tab_store_.RenameRow(guid, name)) {
    return false;
  }
  registry_.InvalidateAll();
  NotifySettingsObservers(PhiSettingsChange::kStoredRows);
  return true;
}

std::string PhiCoordinator::AddBookmarkFolder(int64_t slot,
                                              const std::string& parent_guid,
                                              const std::string& title) {
  std::string profile_id, space_id;
  if (title.empty() || !OwnerForSlot(SlotId{slot}, &profile_id, &space_id)) {
    return std::string();
  }
  const std::string guid = tab_store_.AddBookmark(
      profile_id, space_id, parent_guid, title, std::string(),
      /*folder=*/true, std::string(), 0, 0);
  if (!guid.empty()) {
    registry_.InvalidateAll();
    NotifySettingsObservers(PhiSettingsChange::kStoredRows);
  }
  return guid;
}

std::string PhiCoordinator::AddBookmarkRow(int64_t slot,
                                           const std::string& parent_guid,
                                           const std::string& title,
                                           const std::string& url) {
  std::string profile_id, space_id;
  if (url.empty() || !OwnerForSlot(SlotId{slot}, &profile_id, &space_id)) {
    return std::string();
  }
  const std::string guid = tab_store_.AddBookmark(
      profile_id, space_id, parent_guid, !title.empty() ? title : url, url,
      /*folder=*/false, std::string(), 0, 0);
  if (!guid.empty()) {
    registry_.InvalidateAll();
    NotifySettingsObservers(PhiSettingsChange::kStoredRows);
  }
  return guid;
}

bool PhiCoordinator::MoveStoredRowToSpace(const std::string& guid,
                                          const std::string& space_id,
                                          bool clone) {
  const std::optional<PhiRow> found = tab_store_.Find(guid);
  if (!found || found->type != PhiRowType::kBookmark || found->url.empty() ||
      found->space_id == space_id) {
    return false;  // Folders and pins do not travel between Spaces.
  }
  const PhiRow row = *found;  // AddBookmark may reallocate the store's rows.
  const Space* target = space_manager_->Find(space_id);
  if (!target) {
    return false;
  }
  if (!clone) {
    // A MOVE is the Mac's moveBookmarks(toSpace:): the same row — its
    // identity, and with it its sync id — re-homed under the target's
    // root, with the move recorded as a location edit. Tabs bound to it in
    // the source Space go back to being plain tabs there, as before.
    const std::vector<BoundTab> bound = TabsBoundTo(guid);
    if (!local_store_.MoveBookmarksToSpace({guid}, row.profile_id,
                                           target->profile_id, space_id,
                                           std::nullopt, std::nullopt)) {
      return false;
    }
    if (framework_) {
      for (const BoundTab& b : bound) {
        TagTab(b.browser, b.tab.value, "");
      }
    }
    registry_.InvalidateAll();
    NotifySettingsObservers(PhiSettingsChange::kStoredRows);
    return true;
  }
  // A CLONE is a new bookmark (the Mac's cloneBookmarks): at the target's
  // root, carrying the favicon and — for a split bookmark — the second pane
  // and its arrangement.
  const std::string added = tab_store_.AddBookmark(
      target->profile_id, space_id, std::string(), row.title, row.url,
      /*folder=*/false, row.favicon_base64, row.favicon_width,
      row.favicon_height);
  if (added.empty()) {
    return false;
  }
  if (!row.url_b.empty()) {
    tab_store_.SetRowSecondary(added, row.url_b, row.title_b,
                               row.favicon_b_base64, row.favicon_b_width,
                               row.favicon_b_height);
  }
  if (!row.layout.empty()) {
    tab_store_.SetRowLayout(added, row.layout);
  }
  if (!row.icon.empty()) {
    tab_store_.SetRowIcon(added, row.icon);
  }
  registry_.InvalidateAll();
  NotifySettingsObservers(PhiSettingsChange::kStoredRows);
  return true;
}

int PhiCoordinator::SidebarWidth() const {
  return preferences_.SidebarWidth();
}

void PhiCoordinator::SetSettingsWindowSize(int width, int height) {
  preferences_.RequestSettingsWindowSize(width, height);
}

void PhiCoordinator::RequestSidebarWidth(int dip) {
  preferences_.RequestSidebarWidth(dip);
}

void PhiCoordinator::ReorderSpace(const std::string& space_id, int new_index) {
  space_manager_->ReorderSpace(space_id, new_index);
}

void PhiCoordinator::RequestBookmarkActiveTab(int64_t slot_number) {
  const SlotId slot{slot_number};
  std::string profile_id, space_id;
  if (!framework_ || !OwnerForSlot(slot, &profile_id, &space_id)) {
    return;
  }
  std::string title, url;
  if (const phi_framework::WindowSnapshot* window =
          window_model_.Get(ResolveBrowser(slot))) {
    url = window->nav.url;
    title = window->nav.title;
  }
  if (url.empty()) {
    return;  // Nothing to bookmark (the Mac disables the item then).
  }
  int fw = 0, fh = 0;
  const std::string favicon = CaptureFavicon(slot, &fw, &fh);
  const std::string guid =
      tab_store_.AddBookmark(profile_id, space_id, std::string(), title, url,
                             /*folder=*/false, favicon, fw, fh);
  if (!guid.empty()) {
    registry_.InvalidateAll();
  }
}

void PhiCoordinator::LoadStores(const std::wstring& user_data_dir) {
  consent_store_.Init(user_data_dir);
  // Each Load leaves its model usable: decoded, imported from the
  // pre-phase-3 files on a first run over an old profile, or defaults over
  // a quarantined file (the truth-owner rules are in phi_app_model.h). The
  // preferences apply the display language and resolve the palette as they
  // load; the Space palettes follow once both are in.
  preferences_.Load(user_data_dir);
  // The account's own data (docs/user-data-parity.md): its LocalStore
  // (Spaces, rules, pins, bookmarks) and defaults, and its restore file —
  // the account the session and the Guest choice resolve to.
  {
    const PhiAuth& auth = account_.auth();
    const std::string& sub = auth.user().sub;
    const std::string account = PhiDataAccountFor(
        auth.signed_in(), sub,
        auth.signed_in() ? preferences_.OnboardingPhase(sub) : 0,
        preferences_.GuestModeEnabled());
    // A Guest migration a crash interrupted resumes (or is retired) before
    // any account's data opens.
    PhiRecoverGuestMigration(
        user_data_dir, account,
        base::WideToUTF8(
            PhiStr("localData.bookmarks.importedFromGuestFolderTitle")));
    OpenDataAccount(account, /*reconcile_windows=*/false);
  }
  if (consent_store_.Load(&consent_) == PhiJsonFileStatus::kCorrupt) {
    // Fail closed (lane 5 §4.6): a file whose seal did not check out is
    // quarantined beside its replacement and the model holds NO grants, so
    // every agent connection and credential request prompts as on a first
    // run until the user grants again.
    LOG(ERROR) << "[PhiClient] consent store unreadable; standing grants "
                  "dropped until re-granted";
  }
  const model::PhiSettings& settings = preferences_.settings();
  LOG(INFO) << "[PhiClient] stores loaded: " << space_manager_->spaces().size()
            << " spaces, " << url_router_->rules().size() << " rules, layout "
            << PhiLayoutModeName(settings.layout_mode) << ", theme "
            << settings.theme_id << " / " << PhiAppearanceName(Appearance());
  // What the loaded settings drive outside the services.
  surface_coordinator_.SetReaderStyleFromModel(settings.reader);
  space_manager_->RebuildSpacePalettes();
  // The consent reaches the pipeline the moment it is known: Start() ran
  // before this file was read, and every event until now was dropped. The
  // same switch is Chromium's metrics + crash-upload consent (release
  // review R1): pushed through the framework, as the Mac's onboarding does,
  // so Crashpad and UMA follow the user's choice and nothing else.
  PhiTelemetry::Get().SetEnabled(settings.analytics_enabled);
  framework_->SetMetricsReportingEnabled(settings.analytics_enabled);
  // The launch's events, now that both the settings they describe and the
  // consent that gates them are in: the Mac SDK's install / update /
  // opened lifecycle, then the settings snapshot AppController takes.
  if (PhiVersionTransition transition = PhiNoteAppVersion(user_data_dir);
      transition.event) {
    CaptureAnalytics(kAnalyticsModuleApp, transition.event,
                     std::move(transition.properties));
  }
  CaptureAnalytics(kAnalyticsModuleApp, kEventAppOpened,
                   PhiAppOpenedProperties(settings));
  CaptureAnalytics(
      kAnalyticsModuleSettings, kEventUserDefaultsSnapshot,
      PhiUserDefaultsSnapshotProperties(settings, PhiThisExeIsDefaultBrowser()));
  // The first-time-action record, beside the version file it pairs with.
  first_time_actions_.Load(user_data_dir, PhiNowUnixMs());
}

void PhiCoordinator::OpenDataAccount(const std::string& account_id,
                                     bool reconcile_windows) {
  if (account_id == data_account_ && local_store_.is_open()) {
    return;
  }
  // A pending Spaces/restore write belongs to the account being left.
  FlushDirtyStores();
  data_account_ = account_id;
  const std::wstring dir = PhiEnsureAccountDir(user_data_dir_, account_id);
  local_store_.Close();
  if (!dir.empty()) {
    const store::LocalStore::OpenResult opened =
        local_store_.Open(base::WideToUTF8(dir + L"\\localDB"));
    if (opened == store::LocalStore::OpenResult::kRequiresNewerApp) {
      // The Mac's requiresNewerApp: a newer Phi wrote this store. Nothing
      // is read or written; the built-in default Space stands in.
      LOG(ERROR) << "[PhiClient] the local store needs a newer Phi";
    } else if (opened != store::LocalStore::OpenResult::kOk) {
      LOG(ERROR) << "[PhiClient] the local store could not be opened";
    }
  }
  account_defaults_.Load(dir);
  // The account-scoped preferences (sidebar width, shortcuts) follow.
  preferences_.BindAccount(&account_defaults_);
  space_manager_->Load();
  RememberStartupProfile();
  session_restore_->Load(dir.empty() ? user_data_dir_ : dir);
  LOG(INFO) << "[PhiClient] data account "
            << (account_id == kGuestAccountId ? "guest" : "signed-in")
            << ": " << space_manager_->spaces().size() << " spaces, "
            << local_store_.tabs().size() << " rows";
  if (!reconcile_windows) {
    return;
  }
  MirrorProfileNames();
  MirrorPinScope();
  space_manager_->RetreatFromMissingSpaces();
  PushSpaceMenu();
  SchedulePushRoutingTable();
  registry_.InvalidateAll();
  NotifySettingsObservers(PhiSettingsChange::kStoredRows);
}

void PhiCoordinator::SyncDataAccountToAccess() {
  const PhiAuth& auth = account_.auth();
  const std::string& sub = auth.user().sub;
  const bool session = auth.signed_in();
  OpenDataAccount(
      PhiDataAccountFor(session, sub,
                        session ? preferences_.OnboardingPhase(sub) : 0,
                        access_ == Access::kGuest),
      /*reconcile_windows=*/true);
}

void PhiCoordinator::MirrorPinScope() {
  preferences_.SetPinScopeMirror(tab_store_.pin_scope());
}

std::string PhiCoordinator::LanguagePreference() const {
  return preferences_.LanguagePreference();
}

void PhiCoordinator::PushDisplayLanguageToChromium() {
  if (!framework_) {
    return;
  }
  // The preference, not the active locale: an empty tag is the instruction to
  // follow the operating system, and Chromium resolves that for itself — it
  // ships far more languages than the client's catalog does.
  const char* applied =
      framework_->SetDisplayLanguage(PhiLanguagePreference().c_str());
  LOG(INFO) << "[PhiClient] chromium display language -> "
            << (applied && *applied ? applied : "(unresolved)");
}

void PhiCoordinator::SetLanguagePreference(const std::string& tag) {
  preferences_.SetLanguage(tag);
}

int PhiCoordinator::Saturation() const {
  return preferences_.Saturation();
}

void PhiCoordinator::SetSaturation(int value) {
  preferences_.SetSaturation(value);
}

PhiNewTabBehavior PhiCoordinator::NewTabBehavior() const {
  // PhiNewTabBehavior IS model::NewTabBehavior now (phi_settings_window.h):
  // the UI side stopped keeping a twin to cast between.
  return preferences_.new_tab_behavior();
}

bool PhiCoordinator::NewTabOpensOmnibox() const {
  return preferences_.NewTabOpensOmnibox();
}

void PhiCoordinator::SetNewTabBehavior(PhiNewTabBehavior behavior) {
  preferences_.SetNewTabBehavior(behavior);
}

void PhiCoordinator::RebindShortcut(const std::string& name,
                                    UINT vk,
                                    bool ctrl,
                                    bool shift,
                                    bool alt) {
  preferences_.RebindShortcut(name, vk, ctrl, shift, alt);
}

void PhiCoordinator::RestoreAllShortcuts() {
  preferences_.RestoreAllShortcuts();
}

void PhiCoordinator::FocusWindowForBrowser(int64_t browser_window_id) {
  const BrowserId browser{browser_window_id};
  PhiShellWindow* controller = registry_.ControllerForBrowser(browser);
  if (!controller) {
    return;
  }
  if (controller->active_browser_id() != browser) {
    // The browser backs a HIDDEN Space of that slot — surface it first.
    for (const std::string& space : controller->space_order()) {
      if (controller->BrowserIdForSpace(space) == browser) {
        controller->ShowSpace(space);
        break;
      }
    }
  }
  if (controller->shell_hwnd()) {
    ::SetForegroundWindow(controller->shell_hwnd());
  }
  controller->ScheduleRefresh();
}

void PhiCoordinator::RequestTabToSpace(SlotId slot,
                                       TabId tab,
                                       const std::string& space_id,
                                       bool move) {
  PhiShellWindow* source = registry_.Get(slot);
  if (!source || !framework_) {
    return;
  }
  if (source->active_space() == space_id) {
    return;
  }
  // An off-the-record tab never lands in a regular Space: the cold path
  // below re-opens its URL in the regular profile (history, cookies, sync)
  // and closes the original. Incognito → Incognito Space stays allowed.
  if (source->ShowsIncognito() && !model::IsIncognitoSpaceId(space_id)) {
    return;
  }
  const BrowserId source_browser = ResolveBrowser(slot);
  // The tab's URL — the clone and cold-Space paths re-open it.
  std::string url;
  if (const phi_framework::TabSnapshot* live =
          window_model_.FindTab(source_browser, tab)) {
    url = live->url;
  }
  // An open target Space (any slot) gets the real move — within one
  // profile. Into an Incognito Space's off-the-record session the framework
  // refuses, and the URL is re-opened there and the original closed (the
  // Mac's transfer for differing profiles).
  BrowserId target_browser;
  for (PhiShellWindow* controller : registry_.UserControllers()) {
    target_browser = controller->BrowserIdForSpace(space_id);
    if (target_browser.valid()) {
      break;
    }
  }
  if (move && target_browser.valid()) {
    if (framework_->MoveTabToWindowById(source_browser, tab, target_browser)) {
      FocusWindowForBrowser(target_browser.value);
      source->ScheduleRefresh();
      return;
    }
    if (!model::IsIncognitoSpaceId(space_id)) {
      return;
    }
  }
  if (url.empty()) {
    return;
  }
  if (target_browser.valid()) {
    framework_->OpenTabBypassingSpaceRouting(target_browser.value,
                                             url.c_str());
    FocusWindowForBrowser(target_browser.value);
  } else {
    // Cold Space: surface it and open there (the routed-URL tail).
    url_router_->RouteUrlToSpace(space_id, url, source_browser);
  }
  if (move) {
    framework_->CloseTabById(source_browser.value, tab.value);
  }
  source->ScheduleRefresh();
}

bool PhiCoordinator::RestoreLastSession() const {
  return preferences_.RestoreLastSession();
}

void PhiCoordinator::SetRestoreLastSession(bool enabled) {
  preferences_.SetRestoreLastSession(enabled);
}

void PhiCoordinator::PushRestorePreference() {
  // The ON-CHANGE push: every LOADED profile takes the new app-wide choice.
  // A profile loading later gets it from OnProfileLoaded, never from the
  // path that happened to load it.
  if (!framework_) {
    return;
  }
  std::optional<base::Value> list = base::JSONReader::Read(
      framework_->ListProfilesJson(), base::JSON_PARSE_RFC);
  if (!list || !list->is_list()) {
    return;
  }
  for (const base::Value& v : list->GetList()) {
    if (!v.is_dict()) {
      continue;
    }
    const std::string* id = v.GetDict().FindString("id");
    if (id && v.GetDict().FindBool("loaded").value_or(false)) {
      framework_->SetRestoreOnStartup(id->c_str(),
                                      preferences_.RestoreLastSession());
    }
  }
}

bool PhiCoordinator::AnalyticsEnabled() const {
  return preferences_.AnalyticsEnabled();
}

void PhiCoordinator::SetAnalyticsEnabled(bool enabled) {
  preferences_.SetAnalyticsEnabled(enabled);
}

bool PhiCoordinator::AlwaysShowFullUrl() const {
  return preferences_.AlwaysShowFullUrl();
}

void PhiCoordinator::SetAlwaysShowFullUrl(bool enabled) {
  preferences_.SetAlwaysShowFullUrl(enabled);
}

bool PhiCoordinator::SuppressCloseIncognitoSpaceWarning() const {
  return preferences_.SuppressCloseIncognitoSpaceWarning();
}

void PhiCoordinator::SetSuppressCloseIncognitoSpaceWarning(bool suppress) {
  preferences_.SetSuppressCloseIncognitoSpaceWarning(suppress);
}

bool PhiCoordinator::ShowTabPreviews() const {
  return preferences_.ShowTabPreviews();
}

void PhiCoordinator::SetShowTabPreviews(bool enabled) {
  preferences_.SetShowTabPreviews(enabled);
}

bool PhiCoordinator::ShowOpenTabIndicators() const {
  return preferences_.ShowOpenTabIndicators();
}

void PhiCoordinator::SetShowOpenTabIndicators(bool enabled) {
  preferences_.SetShowOpenTabIndicators(enabled);
}

bool PhiCoordinator::DimUnloadedTabIcons() const {
  return preferences_.DimUnloadedTabIcons();
}

void PhiCoordinator::SetDimUnloadedTabIcons(bool enabled) {
  preferences_.SetDimUnloadedTabIcons(enabled);
}

bool PhiCoordinator::PeekViewEnabled() const {
  return preferences_.PeekViewEnabled();
}

void PhiCoordinator::SetPeekViewEnabled(bool enabled) {
  preferences_.SetPeekViewEnabled(enabled);
}

bool PhiCoordinator::AutoPeekViewEnabled() const {
  return preferences_.AutoPeekViewEnabled();
}

void PhiCoordinator::SetAutoPeekViewEnabled(bool enabled) {
  preferences_.SetAutoPeekViewEnabled(enabled);
}

void PhiCoordinator::PushPeekPreferences() {
  if (framework_) {
    framework_->SetPeekPreferences(preferences_.PeekViewEnabled(),
                                   preferences_.AutoPeekViewEnabled());
  }
}

bool PhiCoordinator::OpenKioskOnCtrlAltClick() const {
  return preferences_.OpenKioskOnCtrlAltClick();
}

void PhiCoordinator::SetOpenKioskOnCtrlAltClick(bool enabled) {
  preferences_.SetOpenKioskOnCtrlAltClick(enabled);
}

void PhiCoordinator::PushKioskPreferences() {
  if (framework_) {
    framework_->SetKioskPreferences(preferences_.OpenKioskOnCtrlAltClick());
  }
}

PhiPinScope PhiCoordinator::PinScope() const {
  return tab_store_.pin_scope();
}

void PhiCoordinator::SetPinScope(PhiPinScope scope) {
  // The Mac passes the window the change came from: its collection's order
  // wins where collections merge.
  std::string profile_id, space_id;
  if (PhiShellWindow* window = registry_.FirstUserController()) {
    OwnerForSlot(window->slot(), &profile_id, &space_id);
  }
  if (!tab_store_.SetPinScope(scope, profile_id, space_id)) {
    return;
  }
  MirrorPinScope();  // Its kPinScope notification refreshes Settings.
  registry_.InvalidateAll();
}

void PhiCoordinator::SetSpaceIcon(const std::string& space_id,
                                  const std::string& icon) {
  space_manager_->SetSpaceIcon(space_id, icon);
}

int PhiCoordinator::SpaceSaturation(const std::string& space_id) const {
  return space_manager_->SpaceSaturation(space_id);
}

void PhiCoordinator::SetSpaceSaturation(const std::string& space_id,
                                        int value) {
  space_manager_->SetSpaceSaturation(space_id, value);
}

bool PhiCoordinator::RenameSpace(const std::string& space_id,
                                 const std::string& name) {
  return space_manager_->RenameSpace(space_id, name);
}

bool PhiCoordinator::DeleteSpace(const std::string& space_id) {
  // The hosts' spelling — the menu bar's and Settings' confirmed deletes.
  // The agent's spaces.delete reaches the manager directly and is not the
  // Mac's space_deleted (which its two confirm dialogs capture).
  if (model::IsIncognitoSpaceId(space_id)) {
    // Deleting an Incognito Space is closing it (the Mac's deleteSpace
    // redirect); nothing persisted goes, so no space_deleted.
    const bool live = space_manager_->Find(space_id) != nullptr;
    RequestCloseIncognitoSpace(space_id);
    return live;
  }
  if (!space_manager_->DeleteSpace(space_id)) {
    return false;
  }
  CaptureAnalytics(kAnalyticsModuleSpaces, kEventSpaceDeleted,
                   base::DictValue());
  return true;
}

std::string PhiCoordinator::DefaultSpaceId() const {
  return space_manager_->DefaultSpaceId();
}

bool PhiCoordinator::CanDeleteSpace(const std::string& space_id) const {
  return space_manager_->CanDeleteSpace(space_id);
}

bool PhiCoordinator::SetSpaceProfile(const std::string& space_id,
                                     const std::string& profile_id) {
  if (!space_manager_->SetSpaceProfile(space_id, profile_id)) {
    return false;
  }
  // The Mac's space_profile_changed: a validated change began. The
  // profile COUNT, never which.
  CaptureAnalytics(
      kAnalyticsModuleSpaces, kEventSpaceProfileChanged,
      PhiProfileCountProperties(
          static_cast<int>(profile_service_->Profiles().size())));
  return true;
}

// PhiSpaceManager::Host -------------------------------------------------------

void PhiCoordinator::SurfaceSpaceInSlot(
    SlotId slot,
    const std::string& space_id,
    const std::vector<std::string>* reopen_urls) {
  bool pending = false, cold = false;
  SwitchSlotToSpace(slot, space_id, &pending, &cold, reopen_urls);
}

void PhiCoordinator::ForgetSpaceData(const std::string& space_id) {
  // The Space's bookmarks and space-scoped pins (the Mac's delete-
  // confirmation promise), its URL rules (which would otherwise route into
  // a Space that no longer exists), and its ghost record — when the Space
  // existed only as a PARKED window from the last session, that window is
  // dropped on the session side too, or the next unfiltered restore would
  // replay it as a loose window of a Space that no longer exists.
  tab_store_.RemoveForSpace(space_id);
  url_router_->DropRulesForSpace(space_id);
  session_restore_->ForgetSpace(space_id);
  // A user delete (not sync's own purge) tombstones the account's Space.
  if (sync_keys_ && !space_manager_->sync_applying()) {
    sync_keys_->NoteLocalSpaceDeleted(space_id);
  }
}

void PhiCoordinator::RehomeSpaceData(const std::string& space_id,
                                     const std::string& profile_id) {
  // The rows the Space owns move with it — the Mac's "bookmarks stay with
  // the Space". Pins held at profile or app scope carry no Space and stay
  // with their profile, so a re-profiled Space picks the new one's up.
  tab_store_.RehomeSpace(space_id, profile_id);
  // Any parked window from the previous session belongs to the OLD profile;
  // materializing it now would reopen the Space where it no longer lives.
  session_restore_->ClearParked(space_id);
}

bool PhiCoordinator::DeleteProfileById(const std::string& profile_id) {
  return profile_service_->DeleteProfileById(profile_id);
}

bool PhiCoordinator::RenameProfileById(const std::string& profile_id,
                                       const std::string& name) {
  const bool renamed = profile_service_->RenameProfileById(profile_id, name);
  if (renamed) {
    MirrorProfileNames();
  }
  return renamed;
}

bool PhiCoordinator::DeveloperMode() const {
  return preferences_.DeveloperMode();
}

void PhiCoordinator::SetDeveloperMode(bool enabled) {
  preferences_.SetDeveloperMode(enabled);
  if (enabled) {
    return;
  }
  // The Mac's setDeveloperModeEnabled(false), a kill switch for everything
  // the mode governs: agent control goes off — its own change handler
  // severs every live connection and revokes the session capabilities —
  // and so does every credential approval. Idempotent when the switch was
  // already off: the revoke still runs, in case anything is connected. (The
  // Mac also forgets its per-agent grants and locks the password manager;
  // Windows keeps no grants, and its vault helper has no lock verb.)
  SetAllowAgentControl(false);
  if (dispatcher_) {
    dispatcher_->RevokeAgentAccess();
  }
  credentials_.RevokeAllGrants();
}

bool PhiCoordinator::AllowAgentUserSpaces() const {
  return preferences_.AllowAgentUserSpaces();
}

bool PhiCoordinator::AgentAutoView() const {
  return preferences_.AgentAutoView();
}

void PhiCoordinator::RequestAgentAutoView(bool enabled) {
  preferences_.SetAgentAutoView(enabled);
}

bool PhiCoordinator::AllowAgentControl() const {
  return preferences_.AllowAgentControl();
}

void PhiCoordinator::SetAllowAgentControl(bool enabled) {
  preferences_.SetAllowAgentControl(enabled);
}

void PhiCoordinator::SetAllowAgentUserSpaces(bool enabled) {
  preferences_.SetAllowAgentUserSpaces(enabled);
}

void PhiCoordinator::SetAgentProfileAllowed(const std::string& profile_id,
                                            bool allowed) {
  preferences_.SetAgentProfileAllowed(profile_id, allowed);
}

void PhiCoordinator::OpenBrowserSettings() {
  OpenBrowserPage("chrome://settings");
}

void PhiCoordinator::OpenBrowserPage(const std::string& url) {
  if (const SlotId slot = registry_.FirstUserSlot(); slot.valid()) {
    RequestOpen(slot.value, url);
  }
}

void PhiCoordinator::OpenBrowserPageInProfile(const std::string& profile_id,
                                              const std::string& url) {
  // Settings ▸ Profiles' data pages (Clear browsing data and friends) act
  // on the profile whose tab shows them: opened in the first window, they
  // cleared ANOTHER profile's data. So a window of this profile only —
  // nothing at all rather than a fallback into another one.
  if (!framework_ || profile_id.empty() || url.empty()) {
    return;
  }
  const auto of_profile = [&](const std::string& space_id) {
    const Space* space = space_manager_->Find(space_id);
    return space && space->profile_id == profile_id &&
           !model::IsIncognitoSpaceId(space_id) &&
           !PhiIsAgentSpaceId(space_id);
  };
  // A user window showing one of its Spaces: the focused one first.
  PhiShellWindow* showing = nullptr;
  const HWND foreground = ::GetForegroundWindow();
  for (PhiShellWindow* controller : registry_.UserControllers()) {
    if (!of_profile(controller->active_space())) {
      continue;
    }
    if (!showing || controller->shell_hwnd() == foreground) {
      showing = controller;
    }
  }
  if (showing) {
    RequestOpen(showing->slot().value, url);
    return;
  }
  // Else a window of its own for one of its Spaces — the last-active one
  // when it is this profile's, else the first in strip order — with the
  // page opened in it once its browser exists.
  std::string space_id;
  if (const std::string last = LastActiveSpaceId(); of_profile(last)) {
    space_id = last;
  }
  for (const Space& space : space_manager_->spaces()) {
    if (!space_id.empty()) {
      break;
    }
    if (of_profile(space.id)) {
      space_id = space.id;
    }
  }
  if (space_id.empty() || !CanUseBrowser()) {
    LOG(WARNING) << "[PhiClient] no window or Space of profile " << profile_id
                 << " to open " << url << " in";
    return;
  }
  PendingCreation creation;
  creation.space_id = space_id;
  creation.open_urls.push_back(url);
  RequestSpaceBrowser(std::move(creation), /*pending=*/nullptr);
}

std::vector<std::string> PhiCoordinator::ProfileSummaries() const {
  return profile_service_->ProfileSummaries();
}

void PhiCoordinator::CreateProfileNamed(const std::string& name) {
  profile_service_->CreateProfileNamed(name);
}

std::vector<std::string> PhiCoordinator::SearchEngines(
    const std::string& profile_id) const {
  std::vector<std::string> rows;
  for (const PhiSearchEngineRow& engine :
       profile_service_->SearchEngines(profile_id)) {
    rows.push_back(PhiSearchEngineWireRow(engine));
  }
  return rows;
}

bool PhiCoordinator::SetSearchEngine(const std::string& profile_id,
                                     const std::string& keyword) {
  return profile_service_->SetSearchEngine(profile_id, keyword);
}

std::string PhiCoordinator::DownloadDirectory(
    const std::string& profile_id) const {
  return profile_service_->DownloadDirectory(profile_id);
}

bool PhiCoordinator::SetDownloadDirectory(const std::string& profile_id,
                                          const std::string& dir) {
  return profile_service_->SetDownloadDirectory(profile_id, dir);
}

void PhiCoordinator::CreateSpaceNamed(const std::string& name) {
  if (name.empty()) {
    return;
  }
  base::DictValue payload;
  payload.Set("name", name);
  std::string json;
  base::JSONWriter::Write(payload, &json);
  // The same path the control verb takes — one creation flow.
  dispatcher().ExecuteControlOnUi(
      "agentSpace.spaces.create", json,
      PhiControlDispatcher::ControlCaller::kInternal);
}

bool PhiCoordinator::OpenCreateSpaceWindow(
    const std::string& initial_profile_id,
    HWND anchor) {
  // The focused browser window's slot presents the new Space, as it does
  // for its own "New Space…"; seeded like the strip's form — the active
  // Space's theme is what the random pick must differ from.
  PhiShellWindow* controller = FocusedUserController();
  if (!controller) {
    return false;
  }
  std::string theme_id = SpaceThemeId(controller->active_space());
  if (theme_id.empty()) {
    theme_id = preferences_.ThemeId();
  }
  PhiCreateSpaceWindow::Open(controller, initial_profile_id, theme_id,
                             preferences_.Saturation(), anchor);
  return true;
}

std::vector<std::string> PhiCoordinator::URLRuleRows() const {
  return url_router_->RuleRows();
}

const PhiPreferences& PhiCoordinator::Preferences() const {
  return preferences_;
}

const PhiSpaceManager& PhiCoordinator::SpaceList() const {
  return *space_manager_;
}

const PhiProfileService& PhiCoordinator::ProfileList() const {
  return *profile_service_;
}

PhiAccountAndFeedback& PhiCoordinator::Account() {
  return account_;
}

PhiUrlRouter& PhiCoordinator::UrlRules() {
  return *url_router_;
}

void PhiCoordinator::AddURLRule(const std::string& space_id,
                                const std::string& host,
                                const std::string& path_prefix,
                                bool ask) {
  url_router_->AddRule(space_id, host, path_prefix, ask);
}

void PhiCoordinator::SetURLRulePattern(int index,
                                       const std::string& host,
                                       const std::string& path_prefix) {
  url_router_->SetRulePattern(index, host, path_prefix);
}

void PhiCoordinator::SetURLRuleTarget(int index,
                                      const std::string& space_id,
                                      bool ask) {
  url_router_->SetRuleTarget(index, space_id, ask);
}

void PhiCoordinator::RemoveURLRule(int index) {
  url_router_->RemoveRule(index);
}

void PhiCoordinator::AddSettingsObserver(PhiSettingsObserver* observer) {
  settings_observers_.push_back(observer);
}

void PhiCoordinator::RemoveSettingsObserver(PhiSettingsObserver* observer) {
  std::erase(settings_observers_, observer);
}

void PhiCoordinator::NotifySettingsObservers(PhiSettingsChange what) {
  // Over a copy: a window may close (and unsubscribe) from inside.
  const std::vector<PhiSettingsObserver*> observers = settings_observers_;
  for (PhiSettingsObserver* observer : observers) {
    observer->OnSettingsChanged(what);
  }
}

// PhiPreferences::Observer ----------------------------------------------------

void PhiCoordinator::OnPreferencesChanged(
    PhiPreferences::PreferenceChange what) {
  using PreferenceChange = PhiPreferences::PreferenceChange;
  // A synced preference changed here: Phi native sync publishes it after a
  // short debounce (the service ignores its own writes of pulled values).
  if (sync_keys_) {
    switch (what) {
      case PreferenceChange::kLayoutMode:
      case PreferenceChange::kBookmarkBar:
      case PreferenceChange::kTheme:
      case PreferenceChange::kNewTab:
      case PreferenceChange::kFullUrl:
      case PreferenceChange::kSyncedMisc:
      case PreferenceChange::kPinScope:
        sync_keys_->NoteLocalSettingsChanged();
        break;
      default:
        break;
    }
  }
  switch (what) {
    case PreferenceChange::kAccess:
      return;  // Read at the next resolve; nothing live to redo.
    case PreferenceChange::kLayoutMode:
      // Every slot re-lays out: the layout is a client-wide setting, so two
      // windows side by side must never disagree.
      for (PhiShellWindow* controller : registry_.Controllers()) {
        controller->ApplyLayoutMode(preferences_.LayoutMode());
      }
      return;
    case PreferenceChange::kSidebarWidth:
    case PreferenceChange::kBookmarkBar:
      // The sidebar, or the bar's band, takes/releases part of the content
      // rect in every window.
      for (PhiShellWindow* controller : registry_.Controllers()) {
        controller->LayoutContents();
      }
      registry_.InvalidateAll();
      return;
    case PreferenceChange::kTheme:
    case PreferenceChange::kSaturation:
      // The app-wide palette is re-resolved; the per-Space overrides follow
      // it, and every slot repaints with the result.
      space_manager_->RebuildSpacePalettes();
      for (PhiShellWindow* controller : registry_.Controllers()) {
        controller->ApplyPalette();
      }
      break;
    case PreferenceChange::kFullUrl:
      registry_.InvalidateAll();  // The address fields repaint.
      break;
    case PreferenceChange::kOpenIndicators:
    case PreferenceChange::kDimUnloaded:
      registry_.InvalidateAll();  // The sidebars repaint their icons and dots.
      break;
    case PreferenceChange::kPeek:
      PushPeekPreferences();  // The framework owns both decisions it gates.
      break;
    case PreferenceChange::kKiosk:
      PushKioskPreferences();  // Likewise: the click is remapped there.
      break;
    case PreferenceChange::kRestore:
      PushRestorePreference();  // Every LOADED profile takes the choice.
      break;
    case PreferenceChange::kLanguage:
      PushDisplayLanguageToChromium();
      break;
    case PreferenceChange::kLocaleStrings:
      // Every piece of chrome caches the strings it was built with, so the
      // whole UI is rebuilt: the app menus are recreated from their (new)
      // labels.
      for (PhiShellWindow* controller : registry_.Controllers()) {
        controller->RebuildAppMenu();
        if (HWND shell = controller->shell_hwnd()) {
          ::InvalidateRect(shell, nullptr, TRUE);
        }
      }
      registry_.InvalidateAll();
      return;
    case PreferenceChange::kAnalytics:
      PhiTelemetry::Get().SetEnabled(preferences_.AnalyticsEnabled());
      if (framework_) {
        framework_->SetMetricsReportingEnabled(preferences_.AnalyticsEnabled());
      }
      break;
    case PreferenceChange::kShortcuts:
      return;  // The keyboard hook reads the effective chords live.
    case PreferenceChange::kNewTab:
    case PreferenceChange::kPreviews:
    case PreferenceChange::kAgentControl:
      // A "Deny" at the connection prompt stands only until the user next
      // touches the switch; the next connection asks again (or is admitted).
      if (dispatcher_) {
        dispatcher_->ForgetAgentControlDenial();
        if (!preferences_.AllowAgentControl()) {
          // OFF cuts every agent that is connected now — the switch is not
          // only a door for newcomers (the Mac severs all live connections).
          dispatcher_->RevokeAgentAccess();
        }
      }
      break;
    case PreferenceChange::kAgentAutoView:
      AgentRunStateChanged();  // Turning it on surfaces a running agent now.
      break;
    case PreferenceChange::kFolio:
      // The opt-in arms Mirage's site rules; a folder change needs nothing
      // live (each save resolves its folder when it starts).
      folio_.BroadcastArmedState();
      break;
    case PreferenceChange::kDeveloper:
    case PreferenceChange::kAgentUserSpaces:
    case PreferenceChange::kPinScope:
    case PreferenceChange::kSyncedMisc:
      break;  // Settings shows them; nothing else consumes a change.
  }
  NotifySettingsObservers(PhiSettingsChange::kPreferences);
}

// PhiSpaceManager::Observer ---------------------------------------------------

void PhiCoordinator::OnSpacesChanged(PhiSpaceManager::SpacesChange what) {
  using SpacesChange = PhiSpaceManager::SpacesChange;
  RememberStartupProfile();  // The default Space or its binding may move.
  // Phi native sync publishes the user's Space edits (not its own).
  if (sync_keys_ && !space_manager_->sync_applying()) {
    sync_keys_->NoteLocalSpacesChanged();
  }
  switch (what) {
    case SpacesChange::kOrder:
      // The chips, and Settings' Space list (a strip drag reorders it too).
      registry_.InvalidateAll();
      break;
    case SpacesChange::kList:
      registry_.InvalidateAll();
      break;
    case SpacesChange::kTheme:
      for (PhiShellWindow* controller : registry_.Controllers()) {
        controller->ApplyPalette();
      }
      break;
    case SpacesChange::kProfile:
      registry_.InvalidateAll();
      PushSpaceMenu();  // The wire's Space list carries the binding.
      break;
  }
  NotifySettingsObservers(PhiSettingsChange::kSpaces);
  if (what == SpacesChange::kList) {
    for (PhiShellWindow* controller : registry_.Controllers()) {
      if (controller->NeedsSpaceReplacement()) {
        SurfaceSpaceInSlot(controller->slot(), space_manager_->DefaultSpaceId(),
                           nullptr);
      }
    }
  }
}

void PhiCoordinator::RebuildSpacePalettes() {
  space_manager_->RebuildSpacePalettes();
}

void PhiCoordinator::OpenUrlInMainWindow(const std::string& url) {
  if (!framework_ || url.empty()) {
    return;
  }
  // The focused user window; a Kiosk has no Space of its own to prefer, so
  // the page lands wherever the user's browsing already lives — in the
  // browser that slot is SHOWING, never its original.
  PhiShellWindow* main = FocusedUserController();
  if (!main) {
    return;
  }
  framework_->OpenTab(ResolveBrowser(main->slot()), url.c_str());
  if (HWND shell = main->shell_hwnd()) {
    ::SetForegroundWindow(shell);
  }
}

void PhiCoordinator::OnOpenLinkInSpace(int64_t source_window_id,
                                       const char* space_id,
                                       const char* url) {
  url_router_->OnOpenLinkInSpace(source_window_id, space_id, url);
}

std::string PhiCoordinator::KioskTargetSpaceId() const {
  return space_manager_->KioskTargetSpaceId();
}

std::string PhiCoordinator::KioskIncognitoSpaceId() {
  return ResolveIncognitoRouteSpace();
}

const PhiPalette& PhiCoordinator::PaletteForSlot(int64_t slot) const {
  return space_manager_->PaletteForSlot(slot);
}

const PhiPalette& PhiCoordinator::PaletteForSpace(
    const std::string& space_id) const {
  return space_manager_->PaletteForSpace(space_id);
}

std::string PhiCoordinator::SpaceThemeId(const std::string& space_id) const {
  return space_manager_->SpaceThemeId(space_id);
}

void PhiCoordinator::SetSpaceTheme(const std::string& space_id,
                                   const std::string& theme_id) {
  space_manager_->SetSpaceTheme(space_id, theme_id);
}

const PhiPalette& PhiCoordinator::Palette() const {
  return preferences_.Palette();
}

std::string PhiCoordinator::ThemeId() const {
  return preferences_.ThemeId();
}

PhiAppearance PhiCoordinator::Appearance() const {
  return preferences_.Appearance();
}

void PhiCoordinator::SetThemeId(const std::string& id) {
  preferences_.SetThemeId(id);
}

void PhiCoordinator::SetAutoPictureInPictureMode(const std::string& mode) {
  preferences_.SetAutoPictureInPictureMode(mode);
}

void PhiCoordinator::SetAppearance(PhiAppearance appearance) {
  preferences_.SetAppearance(appearance);
}

void PhiCoordinator::RequestSettings() {
  PhiSettingsWindow::Open(this);
}

void PhiCoordinator::RequestUrlRulesEditor(HWND anchor) {
  // The universal URL Rules editor: one app-global window over the router,
  // the Space list and the profile names (all PhiSettingsHost reads).
  PhiUrlRulesWindow::Open(this, anchor);
}

void PhiCoordinator::RequestFeedback(int64_t slot,
                                     const PhiFeedbackContext& context) {
  PhiShellWindow* controller = registry_.Resolve(slot);
  account_.OpenFeedback(controller ? controller->shell_hwnd() : nullptr,
                        context);
}

void PhiCoordinator::OnShowFeedbackRequested() {
  // Chromium's IDC_FEEDBACK (Help ▸ Report an Issue via the framework, the
  // sad tab's Send feedback, …): the window the user is looking at —
  // an incognito one included.
  const HWND foreground = ::GetForegroundWindow();
  for (PhiShellWindow* controller : registry_.Controllers()) {
    if (!controller->is_agent() && controller->shell_hwnd() == foreground) {
      controller->OpenFeedback();
      return;
    }
  }
  if (PhiShellWindow* controller = FocusedUserController()) {
    controller->OpenFeedback();
  }
}

void PhiCoordinator::RequestSpaceIconPicker(const std::string& space_id) {
  PhiSettingsWindow::Open(this);
  PhiSettingsWindow::ShowSpacePicker(space_id);
}

int64_t PhiCoordinator::SlotAtScreenPoint(POINT screen,
                                          int64_t exclude_slot) const {
  // WindowFromPoint respects z-order; a hit on the browser child (or any
  // shell child) normalizes to its top-level shell.
  const HWND at = ::GetAncestor(::WindowFromPoint(screen), GA_ROOT);
  if (!at) {
    return 0;
  }
  for (PhiShellWindow* controller : registry_.Controllers()) {
    if (controller->slot() != SlotId{exclude_slot} &&
        controller->shell_hwnd() == at) {
      return controller->slot().value;
    }
  }
  return 0;
}

void PhiCoordinator::NoteTearOffDrop(POINT screen) {
  tear_off_drop_ = screen;
  tear_off_drop_at_ = ::GetTickCount64();
}

bool PhiCoordinator::TakeTearOffDrop(POINT* screen) {
  if (!tear_off_drop_at_) {
    return false;
  }
  // The Mac's tearOffPlacementTimeout: a tear-off the framework never
  // answered must not place some unrelated window later.
  constexpr ULONGLONG kTearOffPlacementTimeoutMs = 4000;
  const bool fresh =
      ::GetTickCount64() - tear_off_drop_at_ <= kTearOffPlacementTimeoutMs;
  tear_off_drop_at_ = 0;
  if (!fresh) {
    return false;
  }
  *screen = tear_off_drop_;
  return true;
}

bool PhiCoordinator::ConvertStoredRow(SlotId slot,
                                      const std::string& guid,
                                      bool to_bookmark,
                                      const std::string& parent_guid,
                                      int index) {
  const std::optional<PhiRow> found = tab_store_.Find(guid);
  if (!found) {
    return false;
  }
  const PhiRow row = *found;  // The store mutates below; keep a copy.
  if (to_bookmark ? row.type != PhiRowType::kPinnedTab
                  : row.type != PhiRowType::kBookmark) {
    return false;  // Folders and same-kind rows never convert.
  }
  // The NEW row belongs to the Space it was dropped in (the Mac passes the
  // state's profileId/spaceId) — not to the old row's owner: a profile- or
  // app-scoped pin carries no Space, and a bookmark made under that owner
  // would show in no sidebar at all.
  std::string profile_id, space_id;
  if (!OwnerForSlot(slot, &profile_id, &space_id)) {
    return false;
  }
  // Every live tab bound to the row — either pane — by tag.
  const std::vector<BoundTab> bound = TabsBoundTo(guid);
  // The other kind of row, same panes, same favicons, same arrangement —
  // the Mac's movePinnedTabOut(toBookmark:) / moveBookmarkOut(toPinnedTabs:).
  const std::string new_guid =
      to_bookmark
          ? tab_store_.AddBookmark(profile_id, space_id, parent_guid,
                                   row.title, row.url, /*folder=*/false,
                                   row.favicon_base64, row.favicon_width,
                                   row.favicon_height)
          : tab_store_.AddPinned(profile_id, space_id, row.title, row.url,
                                 row.favicon_base64, row.favicon_width,
                                 row.favicon_height);
  if (new_guid.empty()) {
    return false;
  }
  if (!row.url_b.empty()) {
    tab_store_.SetRowSecondary(new_guid, row.url_b, row.title_b,
                               row.favicon_b_base64, row.favicon_b_width,
                               row.favicon_b_height);
  }
  if (!row.layout.empty()) {
    tab_store_.SetRowLayout(new_guid, row.layout);
  }
  tab_store_.Reorder(new_guid, index >= 0 ? index
                                          : std::numeric_limits<int>::max());
  // Retarget the live tab(s) at the new row — the Mac's updateTabCustomValue
  // — BEFORE the old row goes, so nothing reads as closed in between.
  if (framework_) {
    for (const BoundTab& b : bound) {
      TagTab(b.browser, b.tab.value,
                                TagFor(new_guid, b.pane_b).c_str());
    }
  }
  tab_store_.Remove(guid);
  registry_.InvalidateAll();
  NotifySettingsObservers(PhiSettingsChange::kStoredRows);
  return true;
}

bool PhiCoordinator::RequestDropTabOnWindow(int64_t source_slot,
                                            int tab_a,
                                            int tab_b,
                                            int64_t target_slot) {
  PhiShellWindow* source = registry_.Resolve(source_slot);
  PhiShellWindow* target = registry_.Resolve(target_slot);
  if (!source || !target || !framework_) {
    return false;
  }
  // Never across the off-the-record boundary.
  if (source->is_incognito() != target->is_incognito()) {
    return false;
  }
  const BrowserId source_browser = ResolveBrowser(SlotId{source_slot});
  const BrowserId target_browser = ResolveBrowser(SlotId{target_slot});
  // By id, one move per member: a split pair moves both, and the framework
  // resolves each index inside the call, after the previous move shifted
  // the strip. A tab no longer in the source is a refused drop.
  bool ok = true;
  for (const TabId tab : {TabId{tab_a}, TabId{tab_b}}) {
    if (!tab.valid()) {
      continue;
    }
    ok = window_model_.FindTab(source_browser, tab) &&
         framework_->MoveTabToWindowById(source_browser, tab, target_browser) &&
         ok;
  }
  if (ok) {
    source->ScheduleRefresh();
    target->ScheduleRefresh();
  }
  return ok;
}

bool PhiCoordinator::AlwaysShowBookmarkBar() const {
  return preferences_.AlwaysShowBookmarkBar();
}

void PhiCoordinator::SetAlwaysShowBookmarkBar(bool enabled) {
  preferences_.SetAlwaysShowBookmarkBar(enabled);
}

bool PhiCoordinator::BookmarkBarOnNewTab() const {
  return preferences_.BookmarkBarOnNewTab();
}

void PhiCoordinator::SetBookmarkBarOnNewTab(bool enabled) {
  preferences_.SetBookmarkBarOnNewTab(enabled);
}

bool PhiCoordinator::AccountInfo(std::string* name,
                                 std::string* email) const {
  return account_.AccountInfo(name, email);
}

bool PhiCoordinator::AccountSigningIn() const {
  return account_.AccountSigningIn();
}

void PhiCoordinator::RequestSignIn() {
  account_.RequestSignIn();
}

void PhiCoordinator::RequestSignOut() {
  account_.RequestSignOut();
  if (access_ == Access::kSignedIn) {
    RequireLogin();  // The Mac's requireLogin: never silently to guest.
  }
}

// --- The onboarding gate ------------------------------------------------------

void PhiCoordinator::ResolveInitialAccess() {
  const bool skip =
      preferences_.SkipOnboarding() ||
      base::CommandLine::ForCurrentProcess()->HasSwitch("skip-onboarding");
  const PhiAuth& auth = account_.auth();
  const bool session = auth.signed_in();
  const int phase = session ? preferences_.OnboardingPhase(auth.user().sub) : 0;
  // The Mac's resolveInitialAccess: a finished account, else the persisted
  // guest choice, else the sign-in screen.
  if (session && phase >= 6) {
    access_ = Access::kSignedIn;
    preferences_.SetGuestModeEnabled(false);
    LOG(INFO) << "[PhiClient] access: signed in";
    return;
  }
  if (preferences_.GuestModeEnabled()) {
    if (session) {
      // A persisted Guest choice never revives an unfinished session (the
      // Mac's discardUncommittedLoginCredentials).
      account_.DiscardSession();
    }
    access_ = Access::kGuest;
    LOG(INFO) << "[PhiClient] access: guest";
    return;
  }
  if (skip) {
    access_ = session ? Access::kSignedIn : Access::kGuest;
    LOG(INFO) << "[PhiClient] access: onboarding skipped (test rig)";
    return;
  }
  access_ = Access::kLoginRequired;
  LOG(INFO) << "[PhiClient] access: login required (session=" << session
            << " phase=" << phase << ")";
  PhiOnboardingWindow::Page page = PhiOnboardingWindow::Page::kLogin;
  if (session) {
    if (phase == 0) {
      preferences_.SetOnboardingPhase(auth.user().sub, 1);
    }
    page = PhiOnboardingWindow::PageForPhase(std::max(1, phase));
  }
  // Deferred a turn (see WM_PHI_OPEN_ONBOARDING).
  ::PostMessageW(msg_hwnd_, WM_PHI_OPEN_ONBOARDING,
                 static_cast<WPARAM>(static_cast<int>(page)), 0);
}

void PhiCoordinator::GrantAccess(Access access) {
  access_ = access;
  // The account now known is the one whose data the windows show.
  SyncDataAccountToAccess();
  SyncChromiumAccount();
  for (PhiShellWindow* controller : registry_.Controllers()) {
    controller->ShowHeldShell();
  }
  CloseOnboarding();
  if (!registry_.FirstUserSlot().valid()) {
    OpenDefaultShellIfNoUserWindow("access granted");
  }
  PushSpaceMenu();
  NotifySettingsObservers(PhiSettingsChange::kAccount);
}

void PhiCoordinator::RequireLogin() {
  access_ = Access::kLoginRequired;
  SyncChromiumAccount();
  preferences_.SetGuestModeEnabled(false);
  for (PhiShellWindow* controller : registry_.UserControllers()) {
    controller->HideShell();
  }
  // Incognito windows are no user slots, so the loop above left them up
  // and usable behind the login gate. They hold nothing worth keeping:
  // closed (through the normal close path), not hidden.
  for (PhiShellWindow* controller : registry_.Controllers()) {
    if (controller->is_incognito() && !controller->is_agent() &&
        controller->shell_hwnd()) {
      ::PostMessageW(controller->shell_hwnd(), WM_CLOSE, 0, 0);
    }
  }
  // The Settings window (whose Sign out usually brought us here) belongs to
  // the signed-in session: hidden with the shells, not left over the Login
  // page. Posted — this runs inside its own click handler. Its WM_CLOSE
  // hides it; reopening after the sign-in is instant.
  if (const HWND settings = PhiSettingsWindow::Handle()) {
    ::PostMessageW(settings, WM_CLOSE, 0, 0);
  }
  OpenOnboarding(PhiOnboardingWindow::Page::kLogin);
}

void PhiCoordinator::OpenOnboarding(PhiOnboardingWindow::Page page) {
  if (onboarding_) {
    onboarding_->ShowPage(page);
    ::SetForegroundWindow(onboarding_->hwnd());
    return;
  }
  onboarding_ =
      std::make_unique<PhiOnboardingWindow>(this, &preferences_, &account_);
  onboarding_->SetNavigator([this](BrowserId browser, const std::string& url) {
    if (framework_ && browser.valid()) {
      framework_->OpenTab(browser, url.c_str());
    }
  });
  if (!onboarding_->Open(page)) {
    onboarding_.reset();
    return;
  }
  if (onboarding_browser_.valid()) {
    // A browser from an earlier opening is still alive: nothing to attach
    // (it went back to the desktop when that window closed) — ask anew.
    if (framework_) {
      framework_->CloseBrowserWindow(onboarding_browser_.value);
    }
    onboarding_browser_ = BrowserId();
  }
}

void PhiCoordinator::CloseOnboarding() {
  if (!onboarding_) {
    return;
  }
  onboarding_->DetachBrowser();
  const HWND hwnd = onboarding_->hwnd();
  if (hwnd) {
    ::DestroyWindow(hwnd);
  }
  onboarding_.reset();
  if (onboarding_browser_.valid() && framework_) {
    // Only ever after GrantAccess showed a shell (or on quit): with no
    // other browser alive this would be the last one and Chromium would
    // start shutting down.
    framework_->CloseBrowserWindow(onboarding_browser_.value);
    onboarding_browser_ = BrowserId();
  }
}

void PhiCoordinator::RequestOnboardingBrowser() {
  if (onboarding_browser_.valid() || onboarding_browser_pending_ ||
      !framework_) {
    return;
  }
  PendingCreation pending;
  pending.onboarding = true;
  onboarding_browser_pending_ =
      // kSpace (kUser needs a source window to take the profile from, and
      // there is none behind the gate) and OFF THE RECORD: the framework
      // answers with a window in its Incognito Space OTR profile, so the
      // tenant's session cookie never reaches a browsing profile and goes
      // when that OTR profile's last window closes (this one, unless an
      // Incognito Space is open). The pending marker, not the kind or the
      // incognito bit, decides the hosting.
      RequestCreation(phi_framework::WindowKind::kSpace, "Default",
                      /*incognito=*/true, BrowserId(), std::move(pending))
          .valid();
}

void PhiCoordinator::ReconcileAccessAfterAuth() {
  const PhiAuth& auth = account_.auth();
  if (auth.signing_in() || !auth.signed_in() || auth.reauth_required()) {
    return;
  }
  if (access_ != Access::kGuest) {
    return;  // Signed in already, or the onboarding window is driving.
  }
  // A sign-in from Guest Mode (Settings ▸ You): a finished account is in
  // at once; a new one runs its remaining onboarding over the browser.
  const std::string& sub = auth.user().sub;
  int phase = preferences_.OnboardingPhase(sub);
  if (phase >= 6) {
    // The Mac's guest_mode_exited: the account is in, from Settings ▸ You.
    CaptureAnalytics(kAnalyticsModuleAccount, kEventGuestModeExited,
                     PhiGuestModeExitedProperties("account_setting"));
    // The Guest data moves into the account (the Mac's GuestDataMigration)
    // before the account's store opens.
    BeginGuestMigration(sub);
    preferences_.SetGuestModeEnabled(false);
    access_ = Access::kSignedIn;
    SyncDataAccountToAccess();
    FinishGuestMigration(sub);
    NotifySettingsObservers(PhiSettingsChange::kAccount);
    return;
  }
  if (phase == 0) {
    phase = 1;
    preferences_.SetOnboardingPhase(sub, phase);
  }
  if (!onboarding_) {
    OpenOnboarding(PhiOnboardingWindow::PageForPhase(phase));
  }
}

void PhiCoordinator::OnboardingSignIn() {
  account_.RequestSignIn();  // Lands in OpenHostedLogin below.
}

void PhiCoordinator::OpenHostedLogin(const std::string& url) {
  // Every sign-in — the gate's Login page, Settings ▸ You, the feedback
  // form, the test rig — rides in the onboarding window's embedded
  // off-the-record browser. Opened here when it is not up (a guest or a
  // reauthentication-required account signing in over the browser): a
  // STANDALONE sign-in, whose cancel or refusal dismisses the window rather
  // than falling back to the gate's Login page.
  if (!onboarding_) {
    OpenOnboarding(PhiOnboardingWindow::Page::kSigningIn);
    if (!onboarding_) {
      return;
    }
    onboarding_->SetStandaloneSignIn(true);
  } else if (onboarding_->page() != PhiOnboardingWindow::Page::kSigningIn) {
    onboarding_->ShowPage(PhiOnboardingWindow::Page::kSigningIn);
  }
  RequestOnboardingBrowser();
  onboarding_->NavigateEmbedded(url);
}

void PhiCoordinator::OnboardingCancelSignIn() {
  account_.RequestCancelSignIn();
}

void PhiCoordinator::OnboardingContinueAsGuest(bool share_metrics) {
  preferences_.SetAnalyticsEnabled(share_metrics);
  preferences_.SetGuestModeEnabled(true);
  // Guests get the Comfortable layout (the Mac's completeGuestOOBE).
  preferences_.RequestLayoutMode(PhiLayoutMode::kComfortable);
  GrantAccess(Access::kGuest);
}

void PhiCoordinator::OnboardingSetPhase(int phase) {
  const PhiAuth& auth = account_.auth();
  if (auth.signed_in()) {
    preferences_.SetOnboardingPhase(auth.user().sub, phase);
  }
}

void PhiCoordinator::OnboardingFinished(bool share_metrics) {
  preferences_.SetAnalyticsEnabled(share_metrics);
  const PhiAuth& auth = account_.auth();
  if (auth.signed_in()) {
    preferences_.SetOnboardingPhase(auth.user().sub, 6);
    if (preferences_.GuestModeEnabled()) {
      // A guest who signed in from Settings ▸ You and finished the rest of
      // the onboarding over the browser: the Mac's guest_mode_exited.
      CaptureAnalytics(kAnalyticsModuleAccount, kEventGuestModeExited,
                       PhiGuestModeExitedProperties("account_setting"));
    }
  }
  const bool from_guest = preferences_.GuestModeEnabled() && auth.signed_in();
  if (from_guest) {
    BeginGuestMigration(auth.user().sub);
  }
  preferences_.SetGuestModeEnabled(false);
  GrantAccess(auth.signed_in() ? Access::kSignedIn : Access::kGuest);
  if (from_guest) {
    FinishGuestMigration(auth.user().sub);
  }
}

void PhiCoordinator::OnboardingClosed() {
  if (CanUseBrowser()) {
    // Onboarding over a usable browser: just dismissed. A sign-in still
    // pending loses its browser with the window, so it is abandoned here
    // (the Settings pane's "finish signing in" note would otherwise stay).
    if (account_.auth().signing_in()) {
      account_.RequestCancelSignIn();
    }
    CloseOnboarding();
    return;
  }
  // Behind the gate the window IS the app: closing it quits. Every browser
  // goes, the onboarding one last; Chromium exits with its last Browser.
  LOG(INFO) << "[PhiClient] onboarding closed behind the gate; quitting";
  if (onboarding_) {
    onboarding_->DetachBrowser();
  }
  if (framework_) {
    for (const BrowserId browser : window_model_.Browsers()) {
      if (browser != onboarding_browser_) {
        framework_->CloseBrowserWindow(browser.value);
      }
    }
    if (onboarding_browser_.valid()) {
      framework_->CloseBrowserWindow(onboarding_browser_.value);
      onboarding_browser_ = BrowserId();
    }
  }
}

void PhiCoordinator::OnboardingFocusBrowser(BrowserId browser) {
  if (framework_ && browser.valid()) {
    framework_->FocusBrowserWindow(browser.value);
  }
}

void PhiCoordinator::OnboardingOpenLink(const std::string& url) {
  ::ShellExecuteW(nullptr, L"open", base::UTF8ToWide(url).c_str(), nullptr,
                  nullptr, SW_SHOWNORMAL);
}

void PhiCoordinator::OpenUrlInFirstUserSlot(const std::string& url) {
  if (const SlotId slot = registry_.FirstUserSlot(); slot.valid()) {
    RequestOpen(slot.value, url);
  }
}

void PhiCoordinator::NoteAccountChanged() {
  NotifySettingsObservers(PhiSettingsChange::kAccount);
  if (onboarding_) {
    onboarding_->OnAccountChanged();
  }
  ReconcileAccessAfterAuth();
  // Chromium sync re-reads the account, and the token a refresh brought.
  SyncChromiumAccount();
  chromium_sync_.NotifyAuthChanged();
}

void PhiCoordinator::SyncChromiumAccount() {
  const PhiAuth& auth = account_.auth();
  if (access_ == Access::kGuest || !auth.signed_in()) {
    chromium_sync_.SetAccount(PhiChromiumSync::AccountState::kSignedOut,
                              std::string(), std::string());
  } else {
    // Reauthentication keeps the identity; the token is what it withholds.
    chromium_sync_.SetAccount(PhiChromiumSync::AccountState::kSignedIn,
                              auth.user().sub, auth.user().email);
  }
  UpdateSyncKeyService();
}

void PhiCoordinator::UpdateSyncKeyService() {
  const PhiAuth& auth = account_.auth();
  // The key layer belongs to a finished sign-in (the Mac builds it on
  // loginCompleted); Guest Mode and a pending onboarding have none.
  const std::string subject =
      access_ == Access::kSignedIn && auth.signed_in() ? auth.user().sub
                                                       : std::string();
  if (sync_keys_ && sync_keys_->subject() == subject) {
    return;
  }
  if (sync_keys_) {
    // Retire first: nothing of the previous account's may reach Chromium or
    // the next account's files. Its setup window goes with it.
    PhiSyncSetupWindow::CloseForAccountChange();
    sync_keys_.reset();
    chromium_sync_.ClearProfileKeys();
    sync_profile_ids_.clear();
    space_manager_->SetSyncHiddenSpaces({});
  }
  if (subject.empty() || user_data_dir_.empty()) {
    return;
  }
  const std::wstring account_dir = PhiEnsureAccountDir(user_data_dir_, subject);
  if (account_dir.empty()) {
    return;
  }
  // Phi native sync's state (docs/user-data-parity.md S5): the marker beside
  // the account's data, the engine cursor outside it (logical time must not
  // roll back with a restored folder), the settings sidecars device-wide.
  PhiSyncKeyService::NativeSyncConfig native;
  native.marker_path = account_dir + L"\\sync\\marker.json";
  native.state_path = user_data_dir_ + L"\\Phi\\syncstate\\" +
                      base::UTF8ToWide(PhiAccountDirName(subject)) + L".json";
  native.sidecar_path = user_data_dir_ + L"\\PhiSyncSidecars.json";
  native.build_identity = std::string("windows/") + PHI_WIN_VERSION_FULL_STRING;
  native.make_settings =
      [this, subject](
          std::function<bool(const std::function<void()>&)> run_on_ui,
          PhiSyncSidecarFile* held,
          std::function<std::optional<std::string>(const std::string&)>
              space_lookup) {
        PhiSyncSettingsHooks hooks;
        hooks.set_pin_scope = [this, subject](const std::string& scope_name) {
          store::PinScope scope = store::PinScope::kProfile;
          if (data_account_ == subject &&
              store::PinScopeFromName(scope_name, &scope) &&
              scope != tab_store_.pin_scope()) {
            SetPinScope(scope);
          }
        };
        hooks.space_lookup = std::move(space_lookup);
        hooks.apply_default_space = [this, subject](const std::string& space_id) {
          if (data_account_ == subject) {
            space_manager_->ApplyAccountDefaultSpace(space_id);
          }
        };
        return PhiSyncableSettings(&preferences_, std::move(run_on_ui), held,
                                   std::move(hooks));
      };
  native.default_space_id = [this] { return space_manager_->DefaultSpaceId(); };
  native.create_profile =
      [this](const std::string& display_name,
             std::function<void(const std::string&)> done) {
        sync_profile_creations_[display_name].push_back(std::move(done));
        PendingProfile pending;
        pending.display_name = display_name;
        pending.then = PendingProfile::Then::kSyncPairing;
        RequestProfile(std::move(pending));
      };
  native.chromium_invalidation = [this, subject](
                                     const std::string& uuid,
                                     const std::vector<int>& types,
                                     const std::string& excluding) {
    chromium_sync_.Invalidate(subject, uuid, types, excluding);
  };
  space_manager_->set_on_default_role_moved([this](const std::string& space_id) {
    if (sync_keys_) {
      sync_keys_->NoteDefaultSpaceHandOff(space_id);
    }
  });
  // The Space kind: this account's Space list, flushed so the store holds
  // the user's latest edits, and only while this account's data is open.
  native.space_table_path = account_dir + L"\\sync\\spaces.json";
  native.spaces = [this, subject]() -> PhiSpaceManager* {
    if (data_account_ != subject || !local_store_.is_open()) {
      return nullptr;
    }
    FlushDirtyStores();
    return space_manager_.get();
  };
  // This account's data is the one meant to be open, yet its store is not
  // (failed, or needs a newer Phi): persistent, unlike an account switch.
  native.space_store_unavailable = [this, subject] {
    return data_account_ == subject && !local_store_.is_open();
  };
  native.known_profiles = [this] {
    std::set<std::string> ids = {store::kDefaultProfileId};
    for (const keys::LocalProfile& local : SyncLocalProfiles()) {
      ids.insert(local.profile_id);
    }
    return ids;
  };
  native.set_hidden_spaces = [this, subject](std::set<std::string> hidden) {
    if (data_account_ == subject) {
      space_manager_->SetSyncHiddenSpaces(hidden);
    }
  };
  // Bookmarks and pins: the open account's store; a landing repaints.
  native.bookmarks_table_path = account_dir + L"\\sync\\bookmarks-cursors.json";
  native.pins_table_path = account_dir + L"\\sync\\pins-cursors.json";
  native.rules_table_path = account_dir + L"\\sync\\urlrules-cursors.json";
  native.owned.store = [this, subject]() -> store::LocalStore* {
    return data_account_ == subject && local_store_.is_open() ? &local_store_
                                                              : nullptr;
  };
  native.owned.applying = [this](bool applying) {
    owned_sync_applying_ = applying;
  };
  native.owned.landed = [this] {
    UnbindTabsOfMissingRows();
    registry_.InvalidateAll();
    NotifySettingsObservers(PhiSettingsChange::kStoredRows);
  };
  native.owned.flush_rules = [this] { FlushDirtyStores(); };
  native.owned.rules_landed = [this] {
    // The Space model mirrors the rules and Save diffs against the store.
    space_manager_->ReloadUrlRulesFromStore();
    SchedulePushRoutingTable();
    NotifySettingsObservers(PhiSettingsChange::kSpaces);
  };
  // The user's own row edits start a round (not sync's landings).
  local_store_.set_on_rows_changed([this] {
    if (sync_keys_ && !owned_sync_applying_) {
      sync_keys_->NoteLocalOwnedChanged();
    }
  });
  sync_keys_ = std::make_unique<PhiSyncKeyService>(
      account_dir, subject, account_.auth_for_sync_keys(),
      [this] { return SyncLocalProfiles(); }, [this] { OnSyncKeysChanged(); },
      std::move(native));
  for (const keys::LocalProfile& local : SyncLocalProfiles()) {
    sync_profile_ids_.insert(local.profile_id);
  }
  sync_keys_->SilentUnlockAndResolve();
}

void PhiCoordinator::OnSyncKeysChanged() {
  if (!sync_keys_) {
    return;
  }
  std::map<std::string, PhiChromiumSync::ProfileKey> exposed;
  for (const auto& [profile_id, key] : sync_keys_->snapshot().exposed) {
    exposed[profile_id] = PhiChromiumSync::ProfileKey{key.uuid, key.passphrase};
  }
  chromium_sync_.ReplaceProfileKeys(exposed);
  NotifySettingsObservers(PhiSettingsChange::kAccount);
}

PhiSyncKeyService* PhiCoordinator::SyncKeys() {
  return sync_keys_.get();
}

void PhiCoordinator::RequestSyncSetup(HWND anchor) {
  PhiSyncSetupWindow::Open(this, anchor);
}

void PhiCoordinator::SyncSetupCreateProfile(const std::string& display_name) {
  PendingProfile pending;
  pending.display_name = display_name;
  pending.then = PendingProfile::Then::kSyncPairing;
  RequestProfile(std::move(pending));
}

bool PhiCoordinator::SyncProfileNameExists(const std::string& display_name) {
  for (const keys::LocalProfile& local : SyncLocalProfiles()) {
    if (local.display_name == display_name) {
      return true;
    }
  }
  return false;
}

void PhiCoordinator::SyncSetupClosed() {
  NotifySettingsObservers(PhiSettingsChange::kAccount);
}

void PhiCoordinator::SyncPairingProfileCreated(const std::string& display_name,
                                               const std::string& profile_id) {
  const auto waiting = sync_profile_creations_.find(display_name);
  if (waiting != sync_profile_creations_.end() && !waiting->second.empty()) {
    // Sync's own request (an account profile this PC lacked).
    std::function<void(const std::string&)> done = std::move(waiting->second.front());
    waiting->second.erase(waiting->second.begin());
    if (waiting->second.empty()) {
      sync_profile_creations_.erase(waiting);
    }
    done(profile_id);
    return;
  }
  PhiSyncSetupWindow::NoteProfileCreated(display_name, profile_id);
}

std::vector<keys::LocalProfile> PhiCoordinator::SyncLocalProfiles() {
  std::vector<keys::LocalProfile> out;
  if (!framework_) {
    return out;
  }
  // phi::ListProfiles already leaves out the reserved (Phi Chat) profile;
  // Windows has no agent fallback profile to exclude.
  std::optional<base::Value> list = base::JSONReader::Read(
      framework_->ListProfilesJson(), base::JSON_PARSE_RFC);
  if (!list || !list->is_list()) {
    return out;
  }
  for (const base::Value& entry : list->GetList()) {
    const base::DictValue* d = entry.GetIfDict();
    const std::string* id = d ? d->FindString("id") : nullptr;
    const std::string* name = d ? d->FindString("name") : nullptr;
    if (id && !id->empty()) {
      // Chromium's "Default" basename is this device's Default profile
      // (the Mac's LocalStore.defaultProfileId): the one that registers as
      // the account default and pairs only with one.
      out.push_back(keys::LocalProfile{*id, name ? *name : std::string(),
                                       *id == "Default"});
    }
  }
  return out;
}

void PhiCoordinator::RequestDefaultBrowser() {
  if (framework_) {
    framework_->RequestDefaultBrowser();
  }
}

PhiLayoutMode PhiCoordinator::LayoutMode() const {
  return preferences_.LayoutMode();
}

void PhiCoordinator::RequestLayoutMode(PhiLayoutMode mode) {
  preferences_.RequestLayoutMode(mode);
}

std::vector<PhiSpaceView> PhiCoordinator::Spaces() const {
  return space_manager_->Spaces();
}

std::vector<PhiAgentPipView> PhiCoordinator::AgentSpaces(
    int64_t slot) const {
  return agent_manager_.Pips(slot);
}

void PhiCoordinator::RequestWatchAgentSpace(const std::string& task_id,
                                            int64_t watcher_slot) {
  agent_manager_.RequestWatch(task_id, watcher_slot);
}

void PhiCoordinator::RequestLeaveWatchedAgentSpace(
    const std::string& space_id) {
  // Slot-hosted: the swap back IS the leave (EndWatch runs it, to the
  // Space the user picked). Fallback: the watcher comes back first, then
  // shows the Space the user picked (attached already, or requested cold).
  const bool hosting = agent_manager_.hosting();
  const int64_t slot = agent_manager_.EndWatch(/*park_agent=*/true, space_id);
  if (hosting || slot == 0 || space_id.empty()) {
    return;
  }
  // The Space the window already shows needs no swap — running one
  // anyway re-slid the band over a page that never changed (part of the
  // "whole window blinked" on the way back from an agent Space).
  if (PhiShellWindow* shell = ControllerForSlot(slot);
      shell && shell->active_space() == space_id) {
    return;
  }
  bool pending = false;
  bool cold = false;
  SwitchSlotToSpace(SlotId(slot), space_id, &pending, &cold);
}

void PhiCoordinator::RequestAgentTakeControl(const std::string& task_id) {
  agent_manager_.RequestTakeControl(task_id);
}

void PhiCoordinator::RequestAgentHandBack(const std::string& task_id) {
  agent_manager_.RequestHandBack(task_id);
}

void PhiCoordinator::RequestAgentFinish(const std::string& task_id) {
  agent_manager_.RequestFinish(task_id);
}

PhiShellWindow* PhiCoordinator::ControllerForAgentWindow(
    BrowserId window_id) {
  return registry_.Resolve(window_id.value);
}

PhiShellWindow* PhiCoordinator::ControllerForSlot(int64_t slot) {
  return registry_.Resolve(slot);
}

PhiShellWindow* PhiCoordinator::SlotHostingAgentTask(
    const std::string& task_id) {
  // A browser sits in one frame, so at most one shell answers. Agent
  // shells never host (a watch asked from one is remapped to its
  // watcher); incognito windows have no strip to ask from but are walked
  // for the same reason the handoff prompt may anchor on one.
  for (PhiShellWindow* controller : registry_.Controllers()) {
    if (!controller->is_agent() && controller->HoldsAgentTask(task_id)) {
      return controller;
    }
  }
  return nullptr;
}

void PhiCoordinator::RebindAgentBrowser(BrowserId browser, int64_t slot) {
  if (slot != 0) {
    registry_.Bind(browser, SlotId(slot));
    return;
  }
  const SlotId own = SlotId::OpenedBy(browser);
  if (registry_.Get(own)) {
    registry_.Bind(browser, own);
  } else {
    registry_.Unbind(browser);
  }
}

void PhiCoordinator::OnAgentBrowserReturned(const std::string& task_id,
                                            HWND child) {
  agent_manager_.OnAgentBrowserReturned(task_id, child);
}

void PhiCoordinator::ShowAgentHandoffPrompt(const std::string& task_id,
                                            const std::string& message) {
  PhiAgentHandoffPrompt::Show(FirstUserController(), task_id, message, this);
}

void PhiCoordinator::DismissAgentHandoffPrompt(const std::string& task_id) {
  PhiAgentHandoffPrompt::DismissFor(task_id);
}

void PhiCoordinator::AgentRunStateChanged() {
  if (msg_hwnd_) {
    ::PostMessageW(msg_hwnd_, WM_PHI_AUTOVIEW, 0, 0);
  }
}

void PhiCoordinator::AutoViewReevaluate() {
  if (!preferences_.AgentAutoView() || !preferences_.AllowAgentControl()) {
    return;
  }
  PhiAgentSpaces& spaces = agent_manager_.spaces();
  const std::string watched = agent_manager_.watched_task();
  if (!watched.empty()) {
    if (const PhiAgentSpaces::Space* current = spaces.Find(watched)) {
      // Watching a running agent, or holding control of one — stay put.
      if (current->status == "running" ||
          current->ownership == PhiAgentSpaces::Ownership::kUser) {
        return;
      }
    }
  }
  const PhiAgentSpaces::Space* next = nullptr;
  for (const auto& [task_id, space] : spaces.entries()) {
    if (space.status == "running" &&
        space.ownership == PhiAgentSpaces::Ownership::kAgent &&
        space.window_id.valid() && (!next || space.number < next->number)) {
      next = &space;
    }
  }
  if (!next || next->task_id == watched) {
    return;
  }
  LOG(INFO) << "[PhiAgent] autoview: surfacing running task "
            << next->task_id;
  PhiShellWindow* watcher = FirstUserController();
  agent_manager_.RequestWatch(next->task_id,
                              watcher ? watcher->slot().value : 0);
}

PhiShellWindow* PhiCoordinator::FirstUserController() {
  // A regular window: an incognito one has no Spaces to switch, watch an
  // agent in, or present a hand-over over.
  for (PhiShellWindow* controller : registry_.UserControllers()) {
    if (controller->shell_hwnd()) {
      return controller;
    }
  }
  return nullptr;
}

void PhiCoordinator::InvalidateUserSlots() {
  registry_.InvalidateUserSlots();
}

void PhiCoordinator::InvalidateAll() {
  registry_.InvalidateAll();
}

void PhiCoordinator::BroadcastControlEvent(const std::string& type,
                                           const std::string& payload_json) {
  control_server_.BroadcastEvent(type, payload_json);
}

void PhiCoordinator::CompleteAgentReply(int reply_id, std::string json) {
  dispatcher().CompletePendingReply(reply_id, std::move(json));
}

// The keep-alive sweep: the agent Spaces and shadows are the manager's, the
// creations in flight are the registry's and the profile service's. The
// framework has no failure callback for either creation, so the deadline is
// the only way a waiting verb learns its window (or profile) is not coming.
void PhiCoordinator::SweepAgentSpaces() {
  const uint64_t now_ms = ::GetTickCount64();
  agent_manager_.Sweep(now_ms);
  for (const PendingCreation& lost : registry_.ExpirePending(now_ms)) {
    LOG(ERROR) << "[PhiClient] window request timed out (kind "
               << static_cast<int>(lost.kind) << ", profile '"
               << lost.profile_id << "', space '" << lost.space_id
               << "', task '" << lost.task_id << "')";
    if (lost.pending_reply_id) {
      dispatcher().CompletePendingReply(lost.pending_reply_id,
                                        kCreateTimeoutReply);
    }
  }
  profile_service_->ExpirePending(now_ms, kCreateTimeoutReply);
  UpdateSweepTimer();
}

void PhiCoordinator::RequestShowTranscript(const std::string& task_id) {
  if (!kPhiTranscriptEnabled || !agent_manager_.spaces().Find(task_id)) {
    return;
  }
  // Own the panel from the first USER window's shell — the panel floats, so
  // any live shell serves as its owner.
  if (PhiShellWindow* controller = FirstUserController()) {
    PhiTranscriptPanel::Toggle(controller->shell_hwnd(),
                               PaletteForSlot(controller->slot().value),
                               task_id, &agent_manager_);
  }
}

void PhiCoordinator::RequestNewWindow(int64_t slot) {
  // Same request the agentSpace.newWindow verb makes: a kUser window whose
  // source is the LIVE browser the slot shows.
  const SlotId source = SlotId{slot};
  const BrowserId browser = ResolveBrowser(source);
  if (!browser.valid()) {
    OpenDefaultShellIfNoUserWindow("New Window from a slot with no browser");
    return;
  }
  PendingCreation pending;
  pending.kind = phi_framework::WindowKind::kUser;
  RequestCreation(phi_framework::WindowKind::kUser, std::string(),
                  /*incognito=*/false, browser, std::move(pending));
}

void PhiCoordinator::RequestSwitchSpace(int64_t slot,
                                        const std::string& space_id) {
  base::DictValue payload;
  payload.Set("windowId", static_cast<double>(slot));
  payload.Set("spaceId", space_id);
  std::string json;
  base::JSONWriter::Write(payload, &json);
  dispatcher().ExecuteControlOnUi(
      "agentSpace.spaces.switch", json,
      PhiControlDispatcher::ControlCaller::kInternal);
}

void PhiCoordinator::RequestCreateSpace(int64_t slot, bool new_profile) {
  // Same profile as the slot's current Space unless a new one was asked for.
  std::string profile_id = "Default";
  if (PhiShellWindow* controller = registry_.Resolve(slot)) {
    if (const Space* s = space_manager_->Find(controller->active_space())) {
      profile_id = s->profile_id;
    }
  }
  if (new_profile && framework_) {
    // Profile creation is asynchronous and the basename is only known once
    // OnProfileCreated reports it; the Space (bound to that profile —
    // bindings are immutable, so it must not be created against the wrong
    // one first) follows there.
    PendingProfile pending;
    pending.display_name =
        "Phi Space " + std::to_string(space_manager_->next_space_id());
    pending.slot = SlotId{slot};
    pending.then = PendingProfile::Then::kOpenSpace;
    RequestProfile(std::move(pending));
    return;
  }
  base::DictValue payload;
  payload.Set("name",
              "Space " + std::to_string(space_manager_->next_space_id()));
  payload.Set("profileId", profile_id);
  std::string json;
  base::JSONWriter::Write(payload, &json);
  const std::string created = dispatcher().ExecuteControlOnUi(
      "agentSpace.spaces.create", json,
      PhiControlDispatcher::ControlCaller::kInternal);
  std::optional<base::DictValue> r =
      base::JSONReader::ReadDict(created, base::JSON_PARSE_RFC);
  if (r) {
    if (const std::string* id = r->FindString("id")) {
      RequestSwitchSpace(slot, *id);
    }
  }
}

void PhiCoordinator::RequestNewIncognitoSpace(int64_t slot) {
  // The Mac's openNewIncognitoSpace: the Space joins the asking window's
  // strip and that window switches to it. An incognito window or an agent
  // shell has no strip: the first user window takes it instead.
  // (No analytics event: the Mac fires none for it.)
  PhiShellWindow* asking = registry_.Resolve(slot);
  PhiShellWindow* shell = asking;
  if (!shell || shell->is_agent() || shell->is_incognito()) {
    shell = FirstUserController();
  }
  const std::string id = space_manager_->CreateIncognitoSpace();
  if (shell) {
    RequestSwitchSpace(shell->slot().value, id);
    if (shell != asking) {
      shell->BringToFront();
    }
    return;
  }
  // No user window: its browser comes up in a shell of its own
  // (AdoptCreatedWindow hosts a slot-less incognito Space browser so).
  PendingCreation pending;
  pending.space_id = id;
  RequestCreation(phi_framework::WindowKind::kSpace, "Default",
                  /*incognito=*/true, BrowserId(), std::move(pending));
}

void PhiCoordinator::RequestCloseIncognitoSpace(
    const std::string& requested_space) {
  // A copy: a caller's reference may be a shell's active_space(), which the
  // switch-away below rewrites — the retire would then take the REGULAR
  // Space's browser and leave the Incognito Space standing.
  const std::string space_id = requested_space;
  if (!model::IsIncognitoSpaceId(space_id) ||
      !space_manager_->Find(space_id)) {
    return;
  }
  // The Mac's closeIncognitoSpace: every slot showing it moves to its last
  // regular Space (else the default) first, so no window is left blank;
  // then its browsers go — a slot with nothing else keeps its browser
  // until the replacement attaches (PhiSpaceBrowsers::NeedsReplacement) —
  // and the entry with them. When the last one is closed the framework
  // tears the shared off-the-record session down with its last browser.
  for (PhiShellWindow* controller : registry_.Controllers()) {
    if (controller->is_agent() || controller->active_space() != space_id) {
      continue;
    }
    std::string back = controller->last_regular_space();
    if (back.empty() || !space_manager_->Find(back)) {
      back = DefaultSpaceId();
    }
    SurfaceSpaceInSlot(controller->slot(), back, nullptr);
  }
  for (PhiShellWindow* controller : registry_.Controllers()) {
    if (!controller->is_agent() && controller->HasSpace(space_id)) {
      controller->RetireSpaceBrowser(space_id);
    }
  }
  space_manager_->RemoveIncognitoSpace(space_id);
  PushSpaceMenu();
}

void PhiCoordinator::RequestCreateProfileForSpaceForm(int64_t slot,
                                                      const std::string& name) {
  if (name.empty()) {
    return;
  }
  // Same asynchronous flow as "New Space with New Profile" — except the
  // form gets the id (OnProfileCreated) instead of a ready-made Space.
  PendingProfile pending;
  pending.display_name = name;
  pending.slot = SlotId{slot};
  pending.then = PendingProfile::Then::kFillForm;
  RequestProfile(std::move(pending));
}

std::string PhiCoordinator::RequestCreateSpaceWithOptions(
    int64_t slot,
    const PhiNewSpaceSpec& spec,
    const char* surface) {
  // CreateSpacePanel.create: the Space through the one creation flow, then
  // its theme pinned (setTheme / setOverlaySaturation), then activated in
  // the window the form was in.
  base::DictValue payload;
  if (!spec.name.empty()) {
    payload.Set("name", spec.name);
  }
  payload.Set("profileId",
              spec.profile_id.empty() ? std::string("Default") : spec.profile_id);
  if (!spec.icon.empty()) {
    payload.Set("iconName", spec.icon);
  }
  if (!spec.color_hex.empty()) {
    payload.Set("colorHex", spec.color_hex);
  }
  // For space_created's `surface` only; an internal field the verb's
  // schema does not list, so the pipe never learns it.
  payload.Set("surface", surface);
  std::string json;
  base::JSONWriter::Write(payload, &json);
  const std::string created = dispatcher().ExecuteControlOnUi(
      "agentSpace.spaces.create", json,
      PhiControlDispatcher::ControlCaller::kInternal);
  std::optional<base::DictValue> r =
      base::JSONReader::ReadDict(created, base::JSON_PARSE_RFC);
  const std::string* id = r ? r->FindString("id") : nullptr;
  if (!id) {
    return std::string();
  }
  const std::string new_id = *id;
  if (!spec.theme_id.empty()) {
    space_manager_->PinSpaceTheme(new_id, spec.theme_id, spec.saturation);
  }
  RequestSwitchSpace(slot, new_id);
  return new_id;
}

PhiShellWindow* PhiCoordinator::ControllerForSlot(SlotId slot) {
  // Wire windowIds may name a slot, a browser bound to one (what
  // spaces.list reports), or 0 for "the window": one resolver for all.
  return registry_.Resolve(slot.value);
}

BrowserId PhiCoordinator::ResolveBrowser(int64_t slot_or_browser) const {
  const BrowserId shown = ResolveBrowser(SlotId{slot_or_browser});
  return shown.valid() ? shown : BrowserId{slot_or_browser};
}

BrowserId PhiCoordinator::ResolveBrowser(SlotId slot) const {
  PhiShellWindow* controller = registry_.Resolve(slot.value);
  return controller ? controller->active_browser_id() : BrowserId();
}

RequestToken PhiCoordinator::RequestCreation(phi_framework::WindowKind kind,
                                             const std::string& profile_id,
                                             bool incognito,
                                             BrowserId source_window,
                                             PendingCreation pending) {
  phi_framework::CreateRequest request;
  request.kind = kind;
  request.profile_id = profile_id.c_str();
  request.incognito = incognito;
  request.source_window = source_window;
  const RequestToken token =
      framework_ ? framework_->RequestBrowser(request) : RequestToken();
  if (!token.valid()) {
    LOG(ERROR) << "[PhiClient] RequestBrowser refused (kind "
               << static_cast<int>(kind) << ", profile '" << profile_id
               << "')";
    if (pending.pending_reply_id) {
      dispatcher().CompletePendingReply(pending.pending_reply_id,
                           "{\"ok\":false,\"error\":\"create_failed\"}");
    }
    return token;
  }
  pending.kind = kind;
  pending.profile_id = profile_id;
  pending.deadline_tick = ::GetTickCount64() + kCreateTimeoutMs;
  LOG(INFO) << "[PhiClient] RequestBrowser token=" << token.value << " kind="
            << static_cast<int>(kind) << " profile='" << profile_id
            << "' slot=" << pending.slot.value << " space='"
            << pending.space_id << "' task='" << pending.task_id << "'";
  registry_.AddPending(token, std::move(pending));
  UpdateSweepTimer();
  return token;
}

void PhiCoordinator::AdoptCreatedWindow(
    const phi_framework::WindowCreation& creation) {
  using phi_framework::WindowKind;
  const BrowserId browser = creation.window;
  const HWND hwnd = reinterpret_cast<HWND>(creation.hwnd);
  LOG(INFO) << "[PhiClient] OnBrowserWindowCreated windowId=" << browser.value
            << " kind=" << static_cast<int>(creation.kind)
            << " token=" << creation.request_token.value
            << " opener=" << creation.opener_window.value
            << " restoredFrom=" << creation.restored_from
            << " incognito=" << creation.incognito;
  if (phase_ < AppPhase::kRestoring) {
    Transition(AppPhase::kRestoring, "window");
  }
  // The model learns the browser BEFORE anything is built on it: a
  // controller constructed below reads its rows from the model, and the
  // first push for this window is still a task away.
  window_model_.Refresh(framework_, browser);
  // A requested kind is matched to what RequestCreation filed by TOKEN — the
  // whole point of the contract (lane 1 F5): two cold switches in flight
  // can no longer take each other's browser. A token nothing waits for
  // (expired by the sweep, or unknown) has no instructions to follow, so
  // the window is hosted as the user's.
  WindowKind kind = creation.kind;
  std::optional<PendingCreation> pending;
  if (creation.request_token.valid()) {
    pending = registry_.TakePending(creation.request_token);
    if (!pending) {
      if (kind == WindowKind::kSpace) {
        // A Space browser past its request's deadline. One that carries
        // pages to open (Open Link in Space, a routed URL, a re-profiled
        // Space's tabs) is the only place those pages still exist: host it
        // as a shell of its own instead of closing it with them.
        if (std::optional<PendingCreation> late =
                registry_.TakeExpiredSpace(creation.request_token)) {
          HostLateSpaceBrowser(browser, hwnd, creation, std::move(*late));
          UpdateSweepTimer();
          return;
        }
        ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
        UpdateSweepTimer();
        return;
      }
      LOG(WARNING) << "[PhiClient] window " << browser.value
                   << " carries token " << creation.request_token.value
                   << " that is not pending; hosting as a user window";
      kind = WindowKind::kUser;
    }
    UpdateSweepTimer();
  } else if (kind == WindowKind::kSpace || kind == WindowKind::kAgent ||
             kind == WindowKind::kShadow) {
    LOG(WARNING) << "[PhiClient] window " << browser.value
                 << " announced as requested kind "
                 << static_cast<int>(kind) << " without a token";
    kind = WindowKind::kUser;
  }
  if (pending && pending->onboarding) {
    // The onboarding window's embedded browser — a plain off-the-record
    // browser (a shadow one skips activation and could never take the
    // keyboard), reparented into that window here (inside the announcement:
    // nothing else) and navigated on a later turn. Its bounds are a child's inside a dialog:
    // "space.agent" keeps them out of the profile's saved placement, as an
    // agent window's are.
    onboarding_browser_pending_ = false;
    onboarding_browser_ = browser;
    if (framework_) {
      framework_->SetWindowSpace(browser.value, "space.agent");
    }
    if (onboarding_) {
      onboarding_->AttachBrowser(hwnd, browser);
    }
    return;
  }
  switch (kind) {
    case WindowKind::kKiosk:
      // Ephemeral, single-tab, no Space and no slot.
      // An off-the-record Kiosk hands its page only to an Incognito Space.
      registry_.AddKiosk(browser, std::make_unique<PhiKioskWindow>(
                                      browser.value, hwnd, framework_,
                                      &window_model_, this,
                                      creation.incognito));
      // The Mac's kiosk_opened (its window is up with its one tab; this
      // client has no profile-replacement Kiosk to exclude).
      CaptureAnalytics(kAnalyticsModuleKiosk, kEventKioskOpened,
                       base::DictValue());
      return;
    case WindowKind::kPeek:
    case WindowKind::kReaderPanel:
      if (HostSurfacePanel(creation)) {
        return;
      }
      break;  // No opener to float over: a plain shell beats a stray window.
    case WindowKind::kAgent:
    case WindowKind::kShadow:
      if (pending->profile_id.empty() && creation.profile_id) {
        pending->profile_id = creation.profile_id;  // "" = first profile.
      }
      HostAgentWindow(browser, hwnd, creation.incognito, *pending);
      return;
    case WindowKind::kSpace:
      if (AttachRequestedSpaceBrowser(browser, hwnd, *pending)) {
        return;
      }
      if (space_manager_->Find(pending->space_id)) {
        // No slot to join (none asked for, or it closed meanwhile): a
        // regular shell of its own whose first Space it is — for an
        // Incognito Space too, NOT an incognito-window shell, which has no
        // Spaces at all.
        HostRequestedSpaceShell(browser, hwnd, *pending);
        return;
      }
      break;
    case WindowKind::kPopup:
      // A window.open popup (a site's sign-in window), an app window,
      // undocked DevTools: Chromium frames and places it itself, with its
      // own caption and read-only location bar, as the Mac leaves these
      // alone too. Announced as kUser it was wrapped in a full shell —
      // sidebar, strip and all (feedback 206). The model keeps the id so
      // a quit still closes it; nothing of ours is built on it.
      return;
    case WindowKind::kUser:
      break;
  }
  // LAZY RESTORE: a materialized ghost carries the PREVIOUS session's window
  // id, which is how it is matched back to the Space that owned it — this
  // run's session_id() is freshly allocated and never matches what the
  // client persisted.
  if (creation.restored_from > 0 &&
      AttachRestoredGhost(browser, hwnd, BrowserId{creation.restored_from},
                          creation.profile_id ? creation.profile_id : "")) {
    return;
  }
  HostUserShell(browser, hwnd, creation.incognito,
                creation.incognito
                    ? std::string()
                    : SpaceForUserBrowser(creation.profile_id
                                              ? creation.profile_id
                                              : ""));
}

void PhiCoordinator::HostLateSpaceBrowser(
    BrowserId browser,
    HWND hwnd,
    const phi_framework::WindowCreation& creation,
    PendingCreation late) {
  // Its waiting reply was already answered (create_timeout), and the slot
  // it was for has moved on: a shell of its own, on its Space when that
  // still exists in this browser's profile, else where any user window of
  // the profile lands.
  late.slot = SlotId();
  late.pending_reply_id = 0;
  late.background = false;
  const std::string profile = creation.profile_id ? creation.profile_id : "";
  const Space* space = space_manager_->Find(late.space_id);
  LOG(WARNING) << "[PhiClient] Space browser " << browser.value
               << " arrived after its request expired; hosting it with "
               << late.open_urls.size() << " url(s)";
  if (space && (profile.empty() || space->profile_id == profile)) {
    HostRequestedSpaceShell(browser, hwnd, late);
    return;
  }
  if (creation.incognito) {
    // Its Incognito Space is gone: an incognito window, never a regular
    // Space's.
    HostUserShell(browser, hwnd, /*incognito=*/true);
  } else {
    late.space_id = SpaceForUserBrowser(profile);  // "" = the default.
    HostUserShell(browser, hwnd, /*incognito=*/false, late.space_id);
  }
  FinishRequestedSpaceBrowser(browser, late);
}

std::string PhiCoordinator::SpaceForUserBrowser(
    const std::string& profile_id) const {
  // A window's profile is baked in at spawn, so a user window shows a Space
  // of ITS profile: hosted as the default Space of another profile it
  // would run that Space's pages, cookies and extensions in the wrong
  // profile (the startup browser did, before the launch followed the
  // default Space's profile — PhiClientStartupProfile).
  const model::Space* fallback =
      space_manager_->Find(space_manager_->DefaultSpaceId());
  if (profile_id.empty() || !fallback || fallback->profile_id == profile_id) {
    return std::string();
  }
  // The Space the user last had up, when it is this profile's; else the
  // first of this profile's in strip order.
  std::string chosen;
  const std::string last = LastActiveSpaceId();
  if (const model::Space* space = space_manager_->Find(last);
      space && space->profile_id == profile_id &&
      !model::IsIncognitoSpaceId(last)) {
    chosen = last;
  }
  if (chosen.empty()) {
    for (const model::Space& space : space_manager_->spaces()) {
      if (space.profile_id == profile_id) {
        chosen = space.id;
        break;
      }
    }
  }
  if (!chosen.empty()) {
    LOG(INFO) << "[PhiClient] user window of profile " << profile_id
              << " hosted on Space " << chosen << ", not the default Space "
              << "(profile " << fallback->profile_id << ")";
    return chosen;
  }
  LOG(WARNING) << "[PhiClient] a user window of profile " << profile_id
               << " has no Space of its profile; hosted as the default "
                  "Space (profile "
               << fallback->profile_id << ")";
  return std::string();
}

void PhiCoordinator::RememberStartupProfile() {
  const model::Space* space =
      space_manager_->Find(space_manager_->DefaultSpaceId());
  if (!space || user_data_dir_.empty() ||
      space->profile_id == startup_profile_written_) {
    return;
  }
  if (PhiWriteStartupProfile(user_data_dir_, space->profile_id)) {
    startup_profile_written_ = space->profile_id;
    LOG(INFO) << "[PhiClient] startup profile -> " << space->profile_id;
  }
}

bool PhiCoordinator::HostSurfacePanel(
    const phi_framework::WindowCreation& creation) {
  const BrowserId browser = creation.window;
  const HWND hwnd = reinterpret_cast<HWND>(creation.hwnd);
  BrowserId opener_window = creation.opener_window;
  TabId opener_tab = creation.opener_tab;
  if (creation.kind == phi_framework::WindowKind::kPeek) {
    // A link inside a surface — a Peek, a Reader View panel — peeks from the
    // strip tab that surface stands over: the surface's own browser has no
    // shell to float in. One peek per opener, so from inside a Peek the new
    // one replaces it (the Mac's openLinkAsPeek, phibrowser-mac 89dd8a48).
    if (const PhiSurfacePanel* source = surfaces().Find(opener_window)) {
      opener_window = BrowserId{source->origin_window_id()};
      opener_tab = TabId{static_cast<int32_t>(source->origin_tab_id())};
    }
  }
  // The opener is named by BROWSER id; the controller is filed under its
  // slot, and the two only coincide until that slot switches Space.
  PhiShellWindow* opener = registry_.ControllerForBrowser(opener_window);
  if (!opener) {
    LOG(WARNING) << "[PhiClient] surface " << browser.value
                 << " without an opener window " << opener_window.value;
    return false;
  }
  if (creation.kind == phi_framework::WindowKind::kReaderPanel) {
    // A Reader View surface: the extension's reading page, moved out of its
    // origin window's strip into a browser of its own so it can be hosted
    // over that origin's live page.
    LOG(INFO) << "[PhiClient] reader panel " << browser.value << " over tab "
              << creation.opener_tab.value << " in window "
              << creation.opener_window.value;
    surfaces().Attach(std::make_unique<PhiReaderPanel>(
        browser.value, hwnd, creation.opener_window.value,
        creation.opener_tab.value, opener, framework_, &window_model_, this));
    return true;
  }
  // A Peek: floats inside its opener window's content card rather than
  // taking a window of its own — over the opener's whole split when it is a
  // pane. One peek per opener: a second replaces the first (the Mac's
  // presentPeek `.replaced`).
  LOG(INFO) << "[PhiClient] peek " << browser.value << " over tab "
            << opener_tab.value << " in window " << opener_window.value;
  // The replaced peek hands no focus back: the new one takes its place
  // over whichever pane is focused. DEFERRED: this runs inside
  // Browser::Create (contract rule 2 — no closing browsers here), and for
  // a peek opened from inside a Peek the replaced one is the page whose
  // Shift-click is still on the stack.
  surface_coordinator_.DeferCloseSurfacesForOrigin(
      opener_tab, SidebarSurface::kPeek, "replaced",
      /*hands_back_strip=*/false);
  surfaces().Attach(std::make_unique<PhiPeekPanel>(
      browser.value, hwnd, opener_window.value, opener_tab.value, opener,
      framework_, &window_model_, this));
  // The Mac's peek_view_opened: the panel actually presented. "restored"
  // never happens here — a peek is omitted from session restore.
  CaptureAnalytics(
      kAnalyticsModulePeek, kEventPeekOpened,
      PhiPeekOpenedProperties(creation.peek_explicit ? "explicit" : "auto",
                              opener->PeekOpenerKind(opener_tab.value)));
  return true;
}

void PhiCoordinator::HostAgentWindow(BrowserId browser,
                                     HWND hwnd,
                                     bool incognito,
                                     const PendingCreation& pending) {
  // An agent Space (or a shadow window, which is the same hidden surface
  // without a presence): host the browser OFF-SCREEN in a slot of its own.
  const SlotId slot = SlotId::OpenedBy(browser);
  // Hosted with the agent chrome whether or not the session is off the
  // record: nobody sees it, and the incognito shell's registry opt-out is
  // the agent path's already. The flag lives on the shadow record.
  PhiShellWindow* controller = registry_.Add(
      slot,
      std::make_unique<PhiShellWindow>(slot, browser, hwnd, framework_,
                                            SlotServices(), &window_model_,
                                            /*agent=*/true),
      /*agent=*/true, incognito);
  registry_.Bind(browser, slot);
  // Registered BEFORE the first rebuild, so every host lookup the rebuild
  // makes (OwnerForSlot, PaletteForSlot) already finds the slot (lane 1 D12).
  controller->ScheduleRefresh();
  const uint64_t now_ms = ::GetTickCount64();
  base::DictValue reply;
  reply.Set("ok", true);
  reply.Set("windowId", static_cast<double>(browser.value));
  agent_manager_.NoteAgentWindow(
      pending.kind == phi_framework::WindowKind::kAgent, pending.task_id,
      browser, pending.profile_id, pending.agent_name, pending.persistent,
      incognito, now_ms, &reply);
  if (pending.kind == phi_framework::WindowKind::kAgent) {
    // The Mac's "agent task record is created" — its window is up.
    CaptureAnalytics(kAnalyticsModuleAgent, kEventAgentTaskStarted,
                     PhiAgentTaskProperties("phi_agent", pending.persistent,
                                            pending.agent_name));
    NoteFirstTimeAction(kFirstTimeActionAgentTask);
  } else {
    // A shadow window is a surface without a task: the Mac's
    // agent_shadow_window_opened, whose origin here is always the pipe.
    CaptureAnalytics(kAnalyticsModuleAgent, kEventAgentShadowWindowOpened,
                     PhiAgentShadowWindowProperties("phi_agent", incognito));
  }
  UpdateSweepTimer();  // The keep-alive sweep, while anything needs it.
  if (pending.pending_reply_id) {
    std::string json;
    base::JSONWriter::Write(reply, &json);
    dispatcher().CompletePendingReply(pending.pending_reply_id,
                                      std::move(json));
  }
}

bool PhiCoordinator::AttachRequestedSpaceBrowser(
    BrowserId browser,
    HWND hwnd,
    const PendingCreation& pending) {
  const Space* space = space_manager_->Find(pending.space_id);
  if (!space || space->profile_id != pending.profile_id) {
    ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
    if (pending.pending_reply_id) {
      dispatcher().CompletePendingReply(pending.pending_reply_id,
                                        kCreateFailedReply);
    }
    return true;
  }
  PhiShellWindow* target = registry_.Get(pending.slot);
  if (!target) {
    if (pending.slot.valid()) {
      LOG(WARNING) << "[PhiClient] Space browser " << browser.value
                   << " arrived for slot " << pending.slot.value
                   << ", which is gone";
    }
    return false;
  }
  // A Space browser for an existing slot: attach, don't open a new shell.
  target->AttachSpaceBrowser(pending.space_id, browser, hwnd);
  registry_.Bind(browser, pending.slot);
  if (!target->is_agent()) {
    NoteActiveSpace(target->active_space());
  }
  PushSpaceMenu();  // The Space gained a window id.
  FinishRequestedSpaceBrowser(browser, pending);
  return true;
}

void PhiCoordinator::HostRequestedSpaceShell(BrowserId browser,
                                             HWND hwnd,
                                             const PendingCreation& pending) {
  // Read BEFORE the shell exists: hosting it may already make it the
  // foreground window.
  const HWND foreground = pending.background ? ::GetForegroundWindow()
                                             : nullptr;
  HostUserShell(browser, hwnd, /*incognito=*/false, pending.space_id);
  if (pending.background) {
    if (PhiShellWindow* shell = registry_.ControllerForBrowser(browser)) {
      shell->PresentBehindOnFirstShow(foreground);
    }
  }
  FinishRequestedSpaceBrowser(browser, pending);
}

void PhiCoordinator::FinishRequestedSpaceBrowser(
    BrowserId browser,
    const PendingCreation& pending) {
  // "Open Link in Space" / a routed URL into a cold Space / a re-profiled
  // Space's tabs / spaces.openTab: they open once the Space's browser
  // exists. Bypassing URL routing — the URL may be the navigation a rule
  // just re-homed HERE, and the routing table rebuilt with the Space's new
  // window races the open, so exempt it outright.
  for (const std::string& url : pending.open_urls) {
    if (framework_) {
      framework_->OpenTabBypassingSpaceRouting(browser.value, url.c_str());
    }
  }
  if (pending.pending_reply_id) {
    base::DictValue reply;
    reply.Set("ok", true);
    reply.Set("windowId", static_cast<double>(browser.value));
    reply.Set("windowOpened", true);
    std::string json;
    base::JSONWriter::Write(reply, &json);
    dispatcher().CompletePendingReply(pending.pending_reply_id,
                                      std::move(json));
  }
}

void PhiCoordinator::OpenUserSpaceWindow(const std::string& space_id,
                                         const std::string& url,
                                         bool activate,
                                         int reply_id) {
  const Space* space = space_manager_->Find(space_id);
  if (!space || model::IsIncognitoSpaceId(space_id) ||
      PhiIsAgentSpaceId(space_id)) {
    dispatcher().CompletePendingReply(
        reply_id, "{\"ok\":false,\"error\":\"unknown_space\"}");
    return;
  }
  if (!CanUseBrowser()) {
    // Behind the onboarding gate a shell would only sit hidden.
    dispatcher().CompletePendingReply(reply_id, kCreateFailedReply);
    return;
  }
  PendingCreation creation;
  creation.space_id = space_id;
  creation.pending_reply_id = reply_id;
  // A fresh window seeds one New Tab, so "open a New Tab there" and "open
  // the Space" are the same ask — never a doubled New Tab.
  if (!url.empty() && !PhiNewTabPage::IsNewTabUrl(url)) {
    creation.open_urls.push_back(url);
  }
  if (activate) {
    // The switcher click: the user's focused window shows it. With no user
    // window at all, a window of its own (shown and activated as usual).
    if (PhiShellWindow* focused = FocusedUserController()) {
      creation.slot = focused->slot();
    }
  } else {
    creation.background = true;
  }
  LOG(INFO) << "[PhiClient] openWindow(" << space_id
            << ") activate=" << activate
            << " slot=" << creation.slot.value;
  RequestSpaceBrowser(std::move(creation), /*pending=*/nullptr);
}

bool PhiCoordinator::AttachRestoredGhost(BrowserId browser,
                                         HWND hwnd,
                                         BrowserId restored_from,
                                         const std::string& profile_id) {
  // The slot that asked for the materialization when one did
  // (SwitchSlotToSpace). A window restore rebuilt on its own joins no slot:
  // it is the shell of its own it was last run (below).
  std::optional<PendingCreation> pending =
      registry_.TakePendingGhost(restored_from);
  UpdateSweepTimer();
  session_restore_->NoteRestoredWindowArrived(restored_from);
  if (!pending) {
    bool secondary = false;
    const std::string* remembered =
        session_restore_->SpaceForWindowId(restored_from, &secondary);
    if (!remembered) {
      return false;  // No Space claims it: a user window of its profile.
    }
    const std::string space_id = *remembered;
    const Space* space = space_manager_->Find(space_id);
    if (!space || (!profile_id.empty() && space->profile_id != profile_id)) {
      // Its Space is gone, or moved to another profile since (its pages
      // would run in the wrong one): the caller hosts it as a user window
      // of its own profile, where the shell's note files it afresh.
      if (secondary) {
        session_restore_->ForgetSecondaryWindow(space_id, restored_from);
      }
      return false;
    }
    if (secondary) {
      // One of the Space's further windows: re-keyed under this run's id
      // BEFORE the shell exists, so its note keeps it beside the Space's
      // own window (still parked, or on its way) instead of over it.
      session_restore_->ForgetSecondaryWindow(space_id, restored_from);
      session_restore_->NoteSecondaryWindow(space_id, space->profile_id,
                                            browser);
    } else {
      // The Space's own window: its primary under this run's id.
      session_restore_->NoteLiveWindow(space_id, space->profile_id, browser);
    }
    // Release review B6: a shell of its own bound to THIS Space. With no
    // slot to join (the cold start's first window) this used to fall
    // through to the default Space: it overwrote the default Space's
    // remembered window, and the settle pass dropped the real one, deleting
    // its tabs.
    HostUserShell(browser, hwnd, /*incognito=*/false, space_id);
    return true;
  }
  const std::string space_id = pending->space_id;
  const SlotId slot = pending->slot;
  PhiShellWindow* target = registry_.Get(slot);
  const Space* space = space_manager_->Find(space_id);
  if (!space || space->profile_id != pending->profile_id) {
    ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
    if (pending->pending_reply_id) {
      dispatcher().CompletePendingReply(pending->pending_reply_id,
                                        kCreateFailedReply);
    }
    return true;
  }
  // The Space's parked window, materialized: its primary under this run's
  // id — before it is hosted (the materialization may announce before the
  // caller has cleared the parked record, and the host's own note would
  // then keep the old row).
  session_restore_->ClearParked(space_id);
  session_restore_->NoteLiveWindow(space_id, space->profile_id, browser);
  if (!target || target->HasSpace(space_id)) {
    // Asked for with no slot to join (OpenUserSpaceWindow), or the slot
    // that asked closed (or got the Space another way) meanwhile: the
    // rebuilt window is a shell of its own, as a fresh one would have been.
    HostRequestedSpaceShell(browser, hwnd, *pending);
    return true;
  }
  target->AttachSpaceBrowser(space_id, browser, hwnd);
  registry_.Bind(browser, slot);
  NoteActiveSpace(target->active_space());
  PushSpaceMenu();
  FinishRequestedSpaceBrowser(browser, *pending);
  return true;
}

void PhiCoordinator::HostUserShell(BrowserId browser, HWND hwnd,
                                   bool incognito,
                                   const std::string& first_space) {
  // Option B: the client owns a shell window and reparents Chromium's
  // content in. The shell routes activation back through the framework. An
  // off-the-record browser (IDC_NEW_INCOGNITO_WINDOW) becomes an INCOGNITO
  // shell: fixed-dark, no Spaces/pins/bookmarks, unrouted.
  const SlotId slot = SlotId::OpenedBy(browser);
  PhiShellWindow* controller = registry_.Add(
      slot,
      std::make_unique<PhiShellWindow>(slot, browser, hwnd, framework_,
                                            SlotServices(), &window_model_,
                                            /*agent=*/false, incognito,
                                            /*start_hidden=*/!CanUseBrowser(),
                                            first_space),
      /*agent=*/false, incognito);
  registry_.Bind(browser, slot);
  controller->ScheduleRefresh();  // After registration (lane 1 D12).
  if (incognito) {
    return;
  }
  // A user window exists: the cold-start fallback has nothing to rescue.
  session_restore_->NoteUserWindowUp();
  NoteActiveSpace(controller->active_space());
  PushSpaceMenu();  // The slot's Space gained a window id.
  if (notification_cards_) {
    // Cards that waited for a window show in this one once it is up.
    ::PostMessageW(msg_hwnd_, WM_PHI_REFRESH_CARDS, 0, 0);
  }
}

// static
LRESULT CALLBACK PhiCoordinator::MouseHookProc(int code,
                                               WPARAM wparam,
                                               LPARAM lparam) {
  PhiCoordinator* self = g_coordinator;
  const bool button_down =
      wparam == WM_LBUTTONDOWN || wparam == WM_RBUTTONDOWN ||
      wparam == WM_MBUTTONDOWN || wparam == WM_NCLBUTTONDOWN ||
      wparam == WM_NCRBUTTONDOWN || wparam == WM_NCMBUTTONDOWN;
  if (code == HC_ACTION && self && button_down) {
    // A click during an Alt hold makes it a chord, not the bare tap that
    // summons the menu titles on release.
    if (self->accelerators_) {
      self->accelerators_->OnMouseButtonDown();
    }
    if (self->surface_coordinator_.HasSurfaces() && self->msg_hwnd_) {
      const auto* hook = reinterpret_cast<MOUSEHOOKSTRUCT*>(lparam);
      // The click is delivered regardless: dismissing is a side effect of
      // clicking elsewhere, not a swallowed click (the Mac lets the click
      // through too) — and it runs on a turn of its own, after this press
      // has been dispatched (WM_PHI_DISMISS_PEEKS). The press's point goes
      // along: only a press on the page pane dismisses. A press in a pane's
      // reader focuses that pane on the same turn.
      ::PostMessageW(self->msg_hwnd_, WM_PHI_DISMISS_PEEKS,
                     reinterpret_cast<WPARAM>(hook->hwnd),
                     MAKELPARAM(hook->pt.x, hook->pt.y));
    }
  }
  return ::CallNextHookEx(nullptr, code, wparam, lparam);
}

HWND PhiCoordinator::ShellForFocus() const {
  HWND focus = ::GetFocus();
  if (!focus) {
    return nullptr;
  }
  HWND root = ::GetAncestor(focus, GA_ROOT);
  for (PhiShellWindow* controller : registry_.Controllers()) {
    if (controller->shell_hwnd() == root) {
      return root;
    }
  }
  return nullptr;
}

HWND PhiCoordinator::FocusedShellHwnd() const {
  return ShellForFocus();
}

BrowserId PhiCoordinator::FocusedFindBrowser(HWND shell) const {
  BrowserId target;
  const HWND focus = ::GetFocus();
  surface_coordinator_.registry().ForEach([&](PhiSurfacePanel& panel) {
    if (panel.kind() == SidebarSurface::kPeek &&
        ::IsWindowVisible(panel.hwnd()) &&
        ::GetAncestor(panel.hwnd(), GA_ROOT) == shell &&
        panel.ContainsWindow(focus)) {
      target = BrowserId{panel.window_id()};
    }
  });
  return target;
}

bool PhiCoordinator::DismissPeeksForShell(HWND shell) {
  // Esc's window-level escapes, nearest first: the floating sidebar slid
  // in over the page (windows-native-ui.md §4.4), then the Peek cards.
  for (PhiShellWindow* controller : registry_.Controllers()) {
    if (controller->shell_hwnd() != shell) {
      continue;
    }
    if (controller->CancelSidebarDrag()) {
      return true;  // §4.8: Esc cancels a lifted row first.
    }
    if (controller->TuckFloatingSidebar()) {
      return true;
    }
  }
  return surface_coordinator_.DismissPeeksForShell(shell, nullptr);
}

bool PhiCoordinator::ExtensionsButtonScreenRect(int64_t window_id,
                                                RECT* rect) {
  PhiShellWindow* shell = registry_.ControllerForBrowser(BrowserId{window_id});
  return shell && shell->ExtensionsButtonScreenRect(rect);
}

void PhiCoordinator::OnExtensionsChanged(int64_t window_id) {
  PhiExtensionsPopover::RefreshOpen();
  // The address capsule's pinned-extension icons follow the same change
  // notification (a pin toggle, an install, an icon decode).
  for (PhiShellWindow* controller : registry_.Controllers()) {
    controller->RefreshFieldExtensions();
  }
}

void PhiCoordinator::OnContentFullscreenChanged(int64_t window_id,
                                                bool fullscreen) {
  const BrowserId browser{window_id};
  if (PhiShellWindow* shell = registry_.ControllerForBrowser(browser)) {
    shell->OnContentFullscreenChanged(browser, fullscreen);
  }
}

bool PhiCoordinator::OnHostedWindowShow(BrowserId browser, bool activate) {
  if (PhiShellWindow* shell = registry_.ControllerForBrowser(browser)) {
    return shell->PresentBrowser(browser, activate);
  }
  return false;
}

bool PhiCoordinator::OnHostedWindowFullscreen(BrowserId browser,
                                              bool fullscreen,
                                              const RECT& target) {
  if (PhiShellWindow* shell = registry_.ControllerForBrowser(browser)) {
    shell->SetShellFullscreen(browser, fullscreen, target);
    return true;
  }
  return false;
}

void PhiCoordinator::OnContentBoundsChanged(int64_t window_id) {
  const BrowserId browser{window_id};
  if (PhiShellWindow* shell = registry_.ControllerForBrowser(browser)) {
    shell->OnContentBoundsChanged(browser);
    // A reader over one split pane follows that pane: a divider drag, the
    // panes' first layout after a tab switch.
    surface_coordinator_.NoteContentLayoutChanged(shell->slot());
  }
}

void PhiCoordinator::OnTabSurfaceCaptured(int request_id,
                                          const unsigned char* bgra,
                                          int width,
                                          int height) {
  for (PhiShellWindow* shell : registry_.Controllers()) {
    if (shell->OnTabSurfaceCaptured(request_id, bgra, width, height)) {
      return;
    }
  }
}

void PhiCoordinator::OnTargetURLChanged(int64_t window_id, const char* url) {
  const BrowserId browser{window_id};
  if (PhiShellWindow* shell = registry_.ControllerForBrowser(browser)) {
    shell->OnTargetURLChanged(browser, base::UTF8ToWide(url ? url : ""));
  }
}

void PhiCoordinator::OnOmniboxResults(int64_t window_id, const char* json) {
  const BrowserId browser{window_id};
  if (PhiShellWindow* shell = registry_.ControllerForBrowser(browser)) {
    shell->OnOmniboxResults(browser, json ? json : "");
  }
}

void PhiCoordinator::OnImportCompleted(const char* source, bool success) {
  // One source of the running import finished (the Mac matches its
  // continuation by browser type the same way).
  const std::optional<PhiImportSource> done =
      PhiImportSourceFromKey(source ? source : "");
  if (!import_run_ || !import_run_->awaiting || !done ||
      *done != *import_run_->awaiting) {
    LOG(WARNING) << "[Import] completion for " << (source ? source : "?")
                 << " matches no running import; dropped";
    return;
  }
  if (import_run_->dispatching) {
    import_run_->early_result = success;  // RunNextImport takes it from here.
    return;
  }
  NoteImportSourceDone(*done, success);
}

void PhiCoordinator::OnPlaceholderModeChanged(int64_t window_id,
                                              bool entered) {
  // A reader panel holds exactly the reading surface. Losing it means the
  // reader is over — but this fork keeps a tabless browser ALIVE in
  // placeholder mode, so the panel would otherwise stay parented over the
  // origin's page showing a new-tab page instead of revealing the article.
  // Retire it. Deferred: this arrives from inside the tab removal that
  // caused it, and closing a browser from there trips
  // `!observers_.is_notifying_observers()`.
  const BrowserId browser{window_id};
  if (surface_coordinator_.DeferRetireOnPlaceholder(browser, entered)) {
    return;
  }
  if (PhiShellWindow* shell = registry_.ControllerForBrowser(browser)) {
    shell->OnPlaceholderModeChanged(browser, entered);
  }
}

void PhiCoordinator::OnRefreshNewTab(int64_t window_id) {
  // A Space URL rule re-homed a navigation that began on this window's new
  // tab: the tab is a new tab again, so its native page goes back to its
  // clean state (the Mac's resetToInitialState).
  if (PhiShellWindow* shell =
          registry_.ControllerForBrowser(BrowserId{window_id})) {
    shell->ResetNativeNtp();
  }
}

void PhiCoordinator::NoteWindowSnapshot(
    const phi_framework::WindowSnapshot& snapshot,
    phi_framework::WindowChangeReason reason) {
  const BrowserId browser = snapshot.window;
  // Stored first: everything below, and every controller rebuild this
  // posts, reads the model rather than the framework.
  window_model_.Update(snapshot);
  if (PhiKioskWindow* kiosk = registry_.Kiosk(browser)) {
    kiosk->RefreshState();
    return;
  }
  if (PhiSurfacePanel* panel = surfaces().Find(browser)) {
    // A surface's own browser: the only thing that can change in it is the
    // page (a reader's only page change that matters is leaving reader.html).
    panel->RefreshState();
    return;
  }
  if (reason != phi_framework::WindowChangeReason::kFaviconReady &&
      reason != phi_framework::WindowChangeReason::kPageColor) {
    // The surfaces over this window settle now (re-fit, show only over the
    // active tab, close when the origin left the strip) and stale pin tags
    // are folded: rule 1 already puts us after commit, on a fresh task,
    // once per window per flush, so there is nothing to defer for. A
    // favicon landing or a page colour moves no rows and needs neither.
    CanonicalizePinTags(browser);
    surface_coordinator_.UpdateReaderPanelsForWindow(browser);
  }
  if (PhiShellWindow* shell = registry_.ControllerForBrowser(browser)) {
    // Coalesced: the sidebar repaints once per turn.
    shell->ScheduleRefresh();
    // The shell switches Spaces on its own (ShowSpace) without telling the
    // owner; strip activity in the browser on screen is the next best
    // signal for which Space the user is in.
    if (!shell->is_agent() && !shell->is_incognito() &&
        shell->active_browser_id() == browser) {
      NoteActiveSpace(shell->active_space());
      // A user shell is up and showing content: whatever restore path ran
      // (an unfiltered replay, a fresh window, an armed replay whose eager
      // window arrived and disarmed the fallback) has nothing left to
      // report, and no receipt or timer will settle it.
      if (phase_ < AppPhase::kSettled) {
        SettleRestore("window");
      }
    }
  }
}

void PhiCoordinator::RetireBrowserWindow(BrowserId browser) {
  LOG(INFO) << "[PhiClient] OnBrowserWindowClosing windowId=" << browser.value;
  if (browser == onboarding_browser_) {
    onboarding_browser_ = BrowserId();
    if (onboarding_) {
      onboarding_->DetachBrowser();
    }
  }
  window_model_.Remove(browser);
  RetireClosingBrowser(browser);
  MaybeEnterClosing();
}

void PhiCoordinator::QuitForUpdate() {
  if (phase_ >= AppPhase::kClosing) {
    return;
  }
  LOG(INFO) << "[PhiClient] quitting for the update installer";
  if (!CanUseBrowser()) {
    OnboardingClosed();  // Behind the gate the onboarding window is the app.
    return;
  }
  // The user's own quit is WM_CLOSE on a shell (menu Quit, the caption's
  // close): every shell and Kiosk gets that, so each saves and unwinds as
  // it would by hand; the onboarding window, if up over a usable browser,
  // is dismissed. Chromium exits with its last Browser (MaybeEnterClosing
  // flushes the stores then) — the installer waits for that.
  CloseOnboarding();
  // The Space the user is looking at is the one the next launch opens on;
  // the shells closing in map order must not overwrite it.
  if (PhiShellWindow* focused = FocusedUserController()) {
    NoteActiveSpace(focused->active_space());
  }
  closing_all_windows_ = true;  // A quit: every window's restore rows stay.
  for (PhiShellWindow* controller : registry_.Controllers()) {
    ::PostMessageW(controller->shell_hwnd(), WM_CLOSE, 0, 0);
  }
  for (const BrowserId browser : window_model_.Browsers()) {
    if (PhiKioskWindow* kiosk = registry_.Kiosk(browser)) {
      ::PostMessageW(kiosk->hwnd(), WM_CLOSE, 0, 0);
    } else if (!registry_.IsBound(browser) && framework_) {
      // A browser no window of ours hosts (a shadow window, a surface whose
      // shell is already gone): the framework closes it directly.
      framework_->CloseBrowserWindow(browser.value);
    }
  }
}

void PhiCoordinator::MaybeEnterClosing() {
  if (phase_ >= AppPhase::kClosing || !window_model_.Browsers().empty() ||
      registry_.HasPending()) {
    return;
  }
  // No browser of any kind is left and none is on its way: Chromium exits
  // once its last Browser goes, and no WM_ENDSESSION accompanies that, so
  // flush here or the coalesced saves die with the timer. (A keep-alive —
  // a download prompt, a background extension — can keep the process up
  // past this point; the flush is then early but never wrong.)
  Transition(AppPhase::kClosing, "lastwindow");
  FlushForShutdown();
}

void PhiCoordinator::RetireClosingBrowser(BrowserId browser) {
  if (registry_.RemoveKiosk(browser)) {
    return;  // A Kiosk owns nothing else — no slot, no Space, no bookkeeping.
  }
  // A surface's own browser closing (its close chip, Esc, the reader's
  // surface tab removed, click-outside) is the whole teardown. Hand the
  // strip back to the origin, the way the Mac reveals the page the overlay
  // was covering, and repaint the row that carried the indicator.
  if (PhiSurfacePanel* panel = surfaces().Find(browser);
      panel && panel->kind() == SidebarSurface::kPeek &&
      phase_ < AppPhase::kClosing) {
    // The Mac's peek_view_ended: how it ended and how long it lived. A
    // close nobody announced is the page's own (window.close()); window
    // teardown reports nothing — the orphan sweep below detaches those
    // before their browsers close, and the app's own exit is gated out.
    const auto* peek = static_cast<const PhiPeekPanel*>(panel);
    const char* reason = peek->end_reason() ? peek->end_reason() : "page_closed";
    CaptureAnalytics(
        kAnalyticsModulePeek, kEventPeekEnded,
        PhiPeekEndedProperties(
            reason, static_cast<int>((::GetTickCount64() - peek->opened_tick()) /
                                     1000)));
  }
  if (const std::optional<PhiSurfaceRegistry::Origin> gone =
          surfaces().Detach(browser)) {
    if (framework_ && gone->hands_back_strip) {
      framework_->ActivateTabById(gone->origin_window_id.value,
                                  gone->origin_tab_id.value);
    }
    // Repaint by the origin WINDOW: the origin tab may be hidden in a pin or
    // bookmark cell, which no visible-row lookup would find — and when it
    // was already active, re-activating it raises no tab notification.
    if (PhiShellWindow* origin =
            registry_.ControllerForBrowser(gone->origin_window_id)) {
      origin->ScheduleRefresh();
    } else if (PhiShellWindow* shown =
                   WindowShowingTab(gone->origin_tab_id)) {
      shown->ScheduleRefresh();
    }
    return;
  }
  // An origin window is going: every surface over it goes with it. They are
  // browsers of their own, so nothing else would close them — and a
  // surviving panel browser keeps the whole process alive after the last
  // window is gone (the Mac's teardown*ForWindowClose, for BOTH kinds).
  std::vector<BrowserId> orphaned;
  for (PhiSurfacePanel* panel : surfaces().OverWindow(browser)) {
    orphaned.push_back(BrowserId{panel->window_id()});
  }
  for (const BrowserId panel_id : orphaned) {
    surfaces().Detach(panel_id);
    if (framework_) {
      framework_->CloseBrowserWindow(panel_id.value);
    }
  }
  // A shadow window closing (shadow.close, the keep-alive sweep, a crash)
  // drops its record; it had no presence to take down. An agent Space's
  // hidden window closing (via complete(), the user closing the surfaced
  // window, or a crash) drops its record — the skill learns the task ended
  // from its absence in the list. Both live in a slot of their own.
  const SlotId agent_slot = SlotId::OpenedBy(browser);
  if (agent_manager_.RetireHiddenWindow(browser)) {
    // The watch overlay may be bound to this shell: unhook it BEFORE the
    // controller (and its HWND) goes, or it keeps a dangling binding.
    if (PhiShellWindow* shell = registry_.Get(agent_slot)) {
      agent_manager_.HideOverlayBoundTo(shell);
    }
    registry_.Remove(agent_slot);  // Drops the browser's binding with it.
    UpdateSweepTimer();
    registry_.InvalidateUserSlots();
    return;
  }
  const std::optional<SlotId> slot = registry_.SlotFor(browser);
  if (!slot) {
    return;  // A browser no slot ever hosted (an unrequested stray).
  }
  registry_.Unbind(browser);
  PhiShellWindow* shell = registry_.Get(*slot);
  if (!shell) {
    return;
  }
  // The Space on screen as the window goes is what the next cold start
  // should rebuild first — noted before the controller can vanish. Not for
  // a window closing whole: RequestClose noted it once, and the Spaces the
  // slot falls back to as its browsers go one by one are no user choice.
  if (!shell->is_incognito() && !shell->closing()) {
    NoteActiveSpace(shell->active_space());
  }
  const std::string closed_space = shell->SpaceForBrowser(browser);
  if (!shell->is_incognito() && !closing_all_windows_ &&
      phase_ < AppPhase::kClosing) {
    // A window of a Space's SECONDARY row closed by hand while other user
    // windows stay up is closed for good: forget the row, or the next run
    // would re-host it (the session keeps every close pending). The last
    // window's close is the quit, and keeps everything.
    for (PhiShellWindow* controller : registry_.UserControllers()) {
      if (controller->slot() != *slot) {
        session_restore_->ForgetSecondaryWindow(closed_space, browser);
        break;
      }
    }
  }
  // The slot survives while any Space browser remains.
  if (!shell->ForgetBrowser(browser)) {
    registry_.Remove(*slot);
  }
  // An Incognito Space whose last browser went another way than its own
  // close (a script's window.close(), Chromium closing it) goes with it —
  // the Mac's reapIncognitoSpaceIfWindowless. Its own close already
  // dropped the entry.
  if (model::IsIncognitoSpaceId(closed_space) &&
      space_manager_->Find(closed_space)) {
    bool held = registry_.FindPendingSpace(SlotId(), closed_space) != nullptr;
    for (PhiShellWindow* controller : registry_.Controllers()) {
      held = held || controller->HasSpace(closed_space) ||
             registry_.FindPendingSpace(controller->slot(), closed_space);
    }
    if (!held) {
      space_manager_->RemoveIncognitoSpace(closed_space);
    }
  }
  PushSpaceMenu();  // A Space lost its window id.
}

bool PhiCoordinator::SwitchSlotToSpace(
    SlotId slot,
    const std::string& space_id,
    bool* pending,
    bool* cold,
    const std::vector<std::string>* reopen_urls) {
  PhiShellWindow* shell = ControllerForSlot(slot);
  const Space* space = space_manager_->Find(space_id);
  if (!shell || !space) {
    return false;
  }
  // An incognito window has no Spaces, and a hidden agent shell switches
  // through the agent manager: neither takes a user Space switch (it
  // hijacked a lone incognito window for Ctrl+Alt+N).
  if (shell->is_incognito() || shell->is_agent()) {
    return false;
  }
  if (shell->HasSpace(space_id)) {
    const bool ok = shell->ShowSpace(space_id);
    if (ok) {
      if (!shell->is_agent()) {
        NoteActiveSpace(space_id);
      }
      PushSpaceMenu();  // The visible-Space routing map changed.
    }
    return ok;
  }
  if (!framework_) {
    return false;
  }
  // What OnBrowserWindowCreated does with the browser when it arrives.
  PendingCreation creation;
  creation.slot = shell->slot();
  creation.space_id = space_id;
  if (reopen_urls) {
    creation.open_urls = *reopen_urls;
  }
  if (cold) {
    *cold = true;
  }
  return RequestSpaceBrowser(std::move(creation), pending);
}

bool PhiCoordinator::RequestSpaceBrowser(PendingCreation creation,
                                         bool* pending) {
  const Space* space = space_manager_->Find(creation.space_id);
  if (!framework_ || !space) {
    if (creation.pending_reply_id) {
      dispatcher().CompletePendingReply(creation.pending_reply_id,
                                        kCreateFailedReply);
    }
    return false;
  }
  const std::string space_id = creation.space_id;
  if (pending) {
    *pending = true;
  }
  // Already asked for: a second switch before the browser arrives must not
  // ask for a second browser (the doubled-Space bug of the single-flight
  // globals). Anything new to open rides on the request in flight, and so
  // does a waiting reply when the request in flight has none.
  if (PendingCreation* in_flight =
          registry_.FindPendingSpace(creation.slot, space_id)) {
    in_flight->open_urls.insert(in_flight->open_urls.end(),
                                creation.open_urls.begin(),
                                creation.open_urls.end());
    if (creation.pending_reply_id) {
      if (in_flight->pending_reply_id) {
        dispatcher().CompletePendingReply(creation.pending_reply_id,
                                          kCreateFailedReply);
      } else {
        in_flight->pending_reply_id = creation.pending_reply_id;
      }
    }
    return true;
  }
  // LAZY RESTORE: this Space may exist only as a PARKED window from the
  // previous session. Rebuild that window rather than spawning a fresh one —
  // the session file still describes the parked one, so a fresh window would
  // stand beside it as a doubled Space. A materialized window is announced
  // with the PREVIOUS session's id rather than a token, so that id keys the
  // pending record — filed BEFORE the call, which may announce inside it.
  if (const PhiGhostSpaces::Remembered* ghost =
          session_restore_->Parked(space_id)) {
    const std::string profile = ghost->profile;
    const BrowserId window_id = ghost->window_id;
    creation.profile_id = profile;
    creation.deadline_tick = ::GetTickCount64() + kCreateTimeoutMs;
    registry_.AddPendingGhost(window_id, creation);
    UpdateSweepTimer();
    const int outcome = framework_->MaterializeGhostWindow(
        profile.c_str(), static_cast<int>(window_id.value));
    switch (outcome) {
      case 0:  // Materialized: the window arrives via OnBrowserWindowCreated.
        session_restore_->ClearParked(space_id);
        return true;
      case 2:
        // No such ghost — nothing will ever satisfy this. DROP the record,
        // or the Space stays pinned on a materialization that cannot
        // succeed, and fall through to spawning it fresh.
        registry_.TakePendingGhost(window_id);
        session_restore_->ClearParked(space_id);
        break;
      default:
        // Refused FOR NOW (profile still loading, a replay in flight). KEEP
        // the record so the next switch retries; spawning instead would
        // double the Space.
        registry_.TakePendingGhost(window_id);
        UpdateSweepTimer();
        if (pending) {
          *pending = false;
        }
        if (creation.pending_reply_id) {
          dispatcher().CompletePendingReply(creation.pending_reply_id,
                                            kCreateFailedReply);
        }
        return false;
    }
  }
  // First visit: ask for this Space's browser IN ITS PROFILE; the request
  // attaches it to the slot when it arrives (a cold profile loads first).
  // An Incognito Space's browser is the shared incognito-Space session.
  const RequestToken token = RequestCreation(
      phi_framework::WindowKind::kSpace, space->profile_id,
      /*incognito=*/model::IsIncognitoSpaceId(space_id), BrowserId(),
      std::move(creation));
  if (!token.valid() && pending) {
    *pending = false;
  }
  return token.valid();
}

BrowserId PhiCoordinator::BrowserForSpace(const std::string& space_id) const {
  if (space_id.empty()) {
    return BrowserId();
  }
  // Only the Space's own user-facing windows count: an agent or shadow
  // shell nominally adopts the DEFAULT Space as its first Space, and
  // resolving that here listed the agent's tabs under the user's Space —
  // or landed the user's tab in an invisible window (the Mac's a4e46767).
  const bool agent_space = PhiIsAgentSpaceId(space_id);
  for (PhiShellWindow* controller : registry_.Controllers()) {
    if (controller->is_agent() && !agent_space) {
      continue;
    }
    // Nor an incognito WINDOW: it nominally holds the default Space too,
    // and a regular Space's tab must never resolve into it.
    if (controller->is_incognito()) {
      continue;
    }
    const BrowserId browser = controller->BrowserIdForSpace(space_id);
    if (browser.valid()) {
      return browser;
    }
  }
  return BrowserId();
}

PhiShellWindow* PhiCoordinator::FocusedUserController() {
  // Regular windows only: what callers do with it (open a Space or a page,
  // present a Space form) must never land in an incognito window.
  const HWND foreground = ::GetForegroundWindow();
  PhiShellWindow* first_user = nullptr;
  for (PhiShellWindow* controller : registry_.UserControllers()) {
    if (foreground && controller->shell_hwnd() == foreground) {
      return controller;
    }
    if (!first_user) {
      first_user = controller;
    }
  }
  return first_user;
}

}  // namespace phi_client

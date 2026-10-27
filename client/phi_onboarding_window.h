// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_ONBOARDING_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_ONBOARDING_WINDOW_H_

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_oobe_analytics.h"
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_holographic_card.h"
#include "phibrowser-win/client/phi_secondary_window.h"
#include "phibrowser-win/client/phi_shell_layout.h"

namespace phi_client {

class PhiAccountAndFeedback;
class PhiPreferences;

// The first-launch flow — the Mac's OnboardingWindowController and its
// pages (docs/login-port.md §8), a fixed 640x800 window that GATES the
// browser: no shell is shown until the user signs in or explores as a
// guest. On a work area shorter than that (1366x768, 1080p at 150 %) the
// window is cut to fit and the page splits — its body scrolls (wheel,
// arrows, Page Up/Down) above a footer anchored to the bottom edge, so
// the primary button is always on screen. Pages, in the Mac's order and
// phase numbering:
//
//   Login          "Sign in" / "Explore Phi without signing in"
//   SigningIn      the Auth0 hosted login, embedded (a shadow browser the
//                  coordinator hands in — the Mac opens Safari; here Phi
//                  is the browser, so the page rides inside this window)
//   GuestPrivacy   "Before we begin": the legal and metrics consents
//   SetName   (1)  "What should I call you?"
//   Welcome   (2)  the membership card (PhiHolographicCard) and its dice
//   Layout    (3)  Balanced / Performance / Comfortable
//   NextSteps (5)  the four steps + the consents + "Let's Begin"
//
// The Mac's phase 4 (Passwords Manager — the iCloud Passwords extension)
// has no Windows counterpart and is skipped. Every page persists what it
// chose through the host as it advances, so a closed window resumes at the
// phase reached. Closing the window while the browser is gated quits.
class PhiOnboardingWindow : public PhiSecondaryWindow {
 public:
  enum class Page {
    kLogin,
    kSigningIn,
    kGuestPrivacy,
    kSetName,
    kWelcome,
    kLayout,
    kNextSteps,
  };

  class Host {
   public:
    virtual ~Host() = default;
    // Login: starts the Auth0 flow; the authorize URL comes back through
    // NavigateEmbedded on the window once the embedded browser is up.
    virtual void OnboardingSignIn() = 0;
    virtual void OnboardingCancelSignIn() = 0;
    // Guest: persists the choice and grants access.
    virtual void OnboardingContinueAsGuest(bool share_metrics) = 0;
    // The account's onboarding phase moved (persisted per subject).
    virtual void OnboardingSetPhase(int phase) = 0;
    // The last page's "Let's Begin": phase done, access granted.
    virtual void OnboardingFinished(bool share_metrics) = 0;
    // The window was closed by the user (the caption's close).
    virtual void OnboardingClosed() = 0;
    // A page's link (Privacy Policy, Terms): the system browser, the
    // gated Phi having no window to show it in.
    virtual void OnboardingOpenLink(const std::string& url) = 0;
    // The embedded browser should own the keyboard: a WS_CHILD never gets
    // WM_ACTIVATE, so Chromium's own activation is driven across the
    // boundary (the shell's FocusActiveBrowser).
    virtual void OnboardingFocusBrowser(BrowserId browser) = 0;
    // The client's one analytics capture path (phi_client_analytics.h):
    // the harness ring, then the collector's consent gate and allow-list.
    virtual void CaptureAnalytics(const char* module,
                                  const char* event_name,
                                  base::DictValue properties) = 0;
  };

  PhiOnboardingWindow(Host* host,
                      PhiPreferences* prefs,
                      PhiAccountAndFeedback* account);
  ~PhiOnboardingWindow() override;

  // Creates the window on `page` and shows it centred on the work area.
  bool Open(Page page);
  // Re-routes the window to `page` (the coordinator's, after the account
  // settled): the step on screen is paused, not completed.
  void ShowPage(Page page);
  Page page() const { return page_; }
  // A sign-in requested over a usable browser (Settings ▸ You, the feedback
  // form): the window exists for the hosted login alone, so a cancelled or
  // refused sign-in dismisses it instead of showing the gate's Login page.
  void SetStandaloneSignIn(bool standalone) { standalone_sign_in_ = standalone; }
  // Phi is exiting while this window is up: the funnel's interruption.
  void NoteAppTerminating();

  // The embedded browser the coordinator requested for the hosted login
  // arrived (inside the announcement: reparent only, nothing else).
  void AttachBrowser(HWND browser_hwnd, BrowserId browser);
  // Hands the browser HWND back to the desktop (before the coordinator
  // closes it, and before this window goes).
  void DetachBrowser();
  BrowserId embedded_browser() const { return browser_; }
  // The authorize URL to load into the embedded browser; queued until the
  // browser exists, then navigated through `navigate`.
  void NavigateEmbedded(const std::string& url);
  void SetNavigator(std::function<void(BrowserId, const std::string&)> fn) {
    navigate_ = std::move(fn);
  }

  // The account state moved (sign-in landed or failed, a rename settled):
  // the page advances or shows its error.
  void OnAccountChanged();

  // The phase -> page map (the Mac's LoginController.Phase).
  static Page PageForPhase(int phase);
  static int PhaseForPage(Page page);
  static const char* PageName(Page page);

  // The harness (agentSpace.onboarding.*): the page's state, and a control
  // pressed by name — "signIn", "guest", "cancel", "legal", "metrics"
  // (toggles), "name" (value = the text), "swatch" (value = theme id: the
  // card's palette and the app theme), "randomize" (the dice), "layout"
  // (value = mode name), "next", "close". Empty on success, else an error
  // token.
  std::string StateJson() const;
  std::string Drive(const std::string& action, const std::string& value);

 protected:
  // PhiWindow / PhiSecondaryWindow:
  void Paint(HDC dc, const RECT& dirty) override;
  void OnHover(int hit, int previous) override;
  void OnWheel(int delta, POINT client_pt) override;
  bool OnKey(WPARAM virtual_key) override;
  void OnMouseDown(int hit, POINT client_pt) override;
  void OnClick(int hit, POINT client_pt) override;
  void OnEscape() override;
  void OnEnter() override;
  void OnTimerFired(UINT_PTR id) override;
  void OnCaptureLost() override;
  void OnDpiChanged(UINT old_dpi) override;
  void OnNcDestroy() override;
  PhiPopupColors QueryColors() const override;
  LRESULT OnWindowMessage(UINT message,
                          WPARAM wparam,
                          LPARAM lparam,
                          bool* handled) override;

 private:
  struct Image;
  // One clickable region of a page, in dips relative to the content rect.
  enum class Control {
    kNone,
    kSignIn,
    kContinueAsGuest,
    kCancelSignIn,
    kLegalCheck,
    kMetricsCheck,
    kPrivacyLink,
    kTermsLink,
    kNext,
    kLayoutRow,   // key = layout mode
    kCard,        // the membership card (Welcome): the pointer lights it
    kDice,        // the Welcome page's randomize button
  };
  struct Hit {
    Control control = Control::kNone;
    int key = 0;
    RECT bounds{};  // Client pixels.
    bool enabled = true;
  };

  void Layout();  // Rebuilds hits_ for the page and the hit list.
  // A page taller than the window (the work area cut it down) SPLITS: the
  // part above FooterTopDip() scrolls by scroll_, the footer below it is
  // anchored to the window's bottom so the primary button stays on
  // screen. On a page that fits, scroll_ stays 0 and nothing changes.
  int FooterTopDip() const;
  int MaxScroll() const;  // 0 when the page fits.
  // The design page in client px: scrolled, or bottom-anchored (`footer`).
  RECT PageRect(bool footer) const;
  int FooterSplit() const;  // Client y where the footer begins.
  void ScrollTo(int px);
  // Back to the design size, cut to the work area, laid out again (a DPI,
  // display or taskbar change).
  void Refit();
  void PaintPage(HDC dc, const RECT& content);
  void PaintGround(HDC dc, const RECT& content);
  // The sign-in / guest / Next row and the Login retry note.
  void PaintFooterControls(HDC dc);
  void PaintLogin(HDC dc, const RECT& content);
  void PaintSigningIn(HDC dc, const RECT& content);
  void PaintGuestPrivacy(HDC dc, const RECT& content);
  void PaintSetName(HDC dc, const RECT& content);
  void PaintWelcome(HDC dc, const RECT& content);
  void PaintLayout(HDC dc, const RECT& content);
  void PaintNextSteps(HDC dc, const RECT& content);
  // Shared pieces.
  void PaintDarkGround(HDC dc, const RECT& content);
  void PaintTitle(HDC dc, const RECT& content, const std::wstring& text,
                  COLORREF color, int top_dip);
  void PaintPillButton(HDC dc, const RECT& bounds, const std::wstring& text,
                       bool enabled, bool hot, bool gradient_border,
                       COLORREF ink, COLORREF border, COLORREF fill);
  // A consent row: the check image and the text, whose "{0}" / "{1}"
  // placeholders become the underlined links. The runs are laid out with a
  // DC so Layout() can register the links as hits and Paint draws the same.
  struct Run {
    std::wstring text;
    int link = -1;  // 0 = link_a, 1 = link_b.
    RECT rect{};
  };
  std::vector<Run> CheckRowRuns(HDC dc, const RECT& row,
                                const std::wstring& pattern,
                                const std::wstring& link_a,
                                const std::wstring& link_b);
  void PaintCheckRow(HDC dc, const RECT& row, bool checked,
                     const std::wstring& text, const std::wstring& link_a,
                     const std::wstring& link_b);
  void AddConsentLinks(const RECT& row, const char* pattern_key,
                       const char* link_a_key, const char* link_b_key);
  void PaintImage(HDC dc, const RECT& dest, Image& image, bool blend);
  Image& ResourceImage(const wchar_t* file, int width, int height);
  HFONT BrandTitleFont(const std::wstring& text, int size_dip);
  void Activate(const Hit& hit);
  // The theme in force on the Welcome page: the app theme AND the card's
  // palette (the Mac's themeChanged).
  void ApplyTheme(const std::string& theme_id);
  // The Welcome card's rect in client px (centred, centre-y 337.5 dips).
  RECT CardRect() const;
  // The dice: a random style, and the app theme follows its palette.
  void RandomizeCard();
  // The Welcome page's copy and persisted style for the signed-in account.
  void LoadCard();
  void StartCardTimer();
  void PaintDice(HDC dc, const RECT& bounds, bool hot);
  void SubmitName();
  void NoteSignInPressed();
  // The user's action moved the flow forward: the step on screen completes
  // and `page` presents (the Mac's setContent).
  void AdvanceTo(Page page);
  // The funnel's step for a page — the sign-in page IS the login step.
  static PhiOobeAnalyticsSession::Step StepFor(Page page);
  static double Uptime();
  void PlaceEmbeddedBrowser();
  // Hands the keyboard to the embedded browser while the SigningIn page is
  // up: Win32 focus on its HWND plus the framework's activation.
  void FocusEmbeddedBrowser();
  void EnsureNameField();
  RECT ContentDip(int x, int y, int w, int h) const;
  void AddControl(Control control, int key, const RECT& bounds,
                  std::wstring name, PhiHitRole role, bool selected,
                  bool enabled);
  const Hit* HitOf(int index) const;

  RAW_PTR_EXCLUSION Host* host_;
  RAW_PTR_EXCLUSION PhiPreferences* prefs_;
  RAW_PTR_EXCLUSION PhiAccountAndFeedback* account_;
  Page page_ = Page::kLogin;
  bool standalone_sign_in_ = false;
  std::vector<Hit> hits_;
  // Per-page state.
  bool legal_checked_ = false;
  bool metrics_checked_ = true;
  int layout_choice_ = 1;  // PhiLayoutMode::kBalanced (the Mac's default).
  std::string theme_id_;
  std::wstring name_error_;
  bool name_submitting_ = false;
  std::string original_name_;
  HWND name_field_ = nullptr;
  HBRUSH field_brush_ = nullptr;
  // The embedded browser.
  HWND browser_hwnd_ = nullptr;
  BrowserId browser_;
  std::string pending_url_;
  std::function<void(BrowserId, const std::string&)> navigate_;
  // The Welcome membership card, its chosen style (palette = the theme in
  // force) and its animation timer.
  PhiHolographicCard card_;
  PhiHolographicCardStyle card_style_;
  Timer card_timer_;
  // Decoded resources, keyed by file and size.
  std::map<std::wstring, std::unique_ptr<Image>> images_;
  int pressed_ = -1;
  int scroll_ = 0;  // Device px the page is scrolled (see MaxScroll).
  // The Mac's OOBEAnalyticsSession, fed through the host's capture path.
  PhiOobeAnalyticsSession oobe_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_ONBOARDING_WINDOW_H_

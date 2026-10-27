// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_onboarding_window.h"

#include <commctrl.h>
#include <windowsx.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "base/containers/span.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "phibrowser-win/client/phi_account_feedback.h"
#include "phibrowser-win/client/phi_client_analytics.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_fonts.h"
#include "phibrowser-win/client/phi_gdiplus.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_popup_chrome.h"
#include "phibrowser-win/client/phi_preferences.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_theme.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "comctl32.lib")

namespace phi_client {

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kClassName[] = L"PhiOnboardingWindow";
// The Mac window's content, in dips.
constexpr int kWidth = 640;
constexpr int kHeight = 800;
// Shared metrics (OnboardingBaseViewController / the pages).
constexpr int kTitleTop = 96;
constexpr int kTitleSize = 46;
constexpr int kTitleHeight = 64;
constexpr int kButtonBottom = 96;   // The Next / Begin pill's bottom inset.
constexpr int kPillWidth = 120;
constexpr int kPillHeight = 40;
constexpr int kCardWidth = 472;
constexpr int kCardHeight = 459;
constexpr int kCardRadius = 14;
constexpr int kCardPad = 24;
constexpr int kCheckSize = 18;
constexpr int kCheckGap = 10;
constexpr int kNameFieldWidth = 398;
constexpr int kNameFieldHeight = 54;
constexpr int kNameFieldRadius = 10;
constexpr int kNameFieldInset = 17;
// The Welcome page (OnboardingWelcomeViewController after cda12eaf): the
// card centred at y 337.5, the 40-dip dice button at y 585.
constexpr int kCardTop = 126;  // 337.5 - 423 / 2
constexpr int kDiceTop = 585;
constexpr int kDiceSize = 40;
constexpr int kLayoutRowWidth = 456;
constexpr int kLayoutRowHeight = 116;
constexpr int kLayoutPreviewWidth = 140;
constexpr int kLayoutPreviewHeight = 81;
constexpr int kStepMarker = 24;
constexpr int kStepGap = 18;
constexpr int kGuideHeight = 260;
// A page taller than the work area (1366x768 at 100 %, 1080p at 150 %)
// splits here: everything above scrolls, the footer below stays anchored to
// the window's bottom edge so the primary button never sits under the
// taskbar. The Next pill's top less 16 of air; the Login page's footer
// starts above its sign-in pill instead (its guest link and retry note
// sit below that).
constexpr int kFooterTop = kHeight - kButtonBottom - kPillHeight - 16;
constexpr int kLoginFooterTop = kHeight - 91 - 22 - 12 - kPillHeight - 16;
// One wheel notch / arrow key of scroll.
constexpr int kScrollStep = 48;
constexpr UINT kMsgNavigate = WM_APP + 0x3B1;
constexpr UINT_PTR kIdNameField = 1;
// The Mac's link gradient #9452F9 -> #E8C0FF, sampled at its middle.
constexpr COLORREF kLinkInk = RGB(0xBE, 0x89, 0xFC);
constexpr char kPrivacyUrl[] = "http://phibrowser.com/privacy/";
constexpr char kTermsUrl[] = "http://phibrowser.com/terms/";
// The angular gradient of the Mac's GradientBorderButton.
constexpr std::array<COLORREF, 5> kPillGradient = {
    RGB(0xFF, 0xCA, 0xE5), RGB(0x00, 0xAA, 0xFF), RGB(0x94, 0x52, 0xF9),
    RGB(0xFF, 0x8E, 0xC7), RGB(0xFF, 0xCA, 0xE5)};

// One SVG path command of the dice icon: 0 move, 1 line, 2 cubic, 3 close.
struct DiceSeg {
  int op;
  float a, b, c, d, e, f;
};
constexpr float kDiceIconW = 22.073f;
constexpr float kDiceIconH = 20.598f;
constexpr std::array<DiceSeg, 10> kDiceFront = {{
    {0, 14.928f, 14.321f, 0, 0, 0, 0}, {1, 13.178f, 7.792f, 0, 0, 0, 0},
    {2, 12.749f, 6.191f, 11.104f, 5.241f, 9.504f, 5.670f}, {1, 2.974f, 7.420f, 0, 0, 0, 0},
    {2, 1.374f, 7.849f, 0.424f, 9.494f, 0.853f, 11.094f}, {1, 2.603f, 17.624f, 0, 0, 0, 0},
    {2, 3.032f, 19.224f, 4.677f, 20.174f, 6.277f, 19.745f}, {1, 12.807f, 17.995f, 0, 0, 0, 0},
    {2, 14.407f, 17.567f, 15.357f, 15.922f, 14.928f, 14.321f}, {3, 0, 0, 0, 0, 0, 0},
}};
constexpr std::array<DiceSeg, 14> kDiceBack = {{
    {0, 8.186f, 5.977f, 0, 0, 0, 0}, {1, 8.226f, 5.807f, 0, 0, 0, 0}, {1, 8.244f, 5.739f, 0, 0, 0, 0},
    {2, 8.962f, 3.044f, 9.325f, 1.681f, 10.396f, 1.067f},
    {2, 11.476f, 0.447f, 12.846f, 0.807f, 15.586f, 1.547f}, {1, 16.286f, 1.727f, 0, 0, 0, 0},
    {1, 16.351f, 1.744f, 0, 0, 0, 0}, {2, 19.047f, 2.472f, 20.391f, 2.835f, 21.006f, 3.907f},
    {2, 21.626f, 4.987f, 21.266f, 6.347f, 20.536f, 9.077f}, {1, 19.816f, 11.767f, 0, 0, 0, 0},
    {2, 19.696f, 12.317f, 19.518f, 12.853f, 19.286f, 13.367f},
    {2, 19.021f, 13.842f, 18.632f, 14.237f, 18.160f, 14.508f},
    {2, 17.688f, 14.780f, 17.150f, 14.917f, 16.606f, 14.907f},
    {2, 16.236f, 14.907f, 15.806f, 14.777f, 14.956f, 14.547f},
}};
constexpr std::array<std::pair<float, float>, 3> kDicePips = {{{9.55f, 9.86f}, {17.36f, 6.14f}, {6.23f, 15.72f}}};

void AddDiceSegs(Gdiplus::GraphicsPath* path, base::span<const DiceSeg> segs) {
  Gdiplus::PointF current(0, 0);
  for (const DiceSeg& seg : segs) {
    switch (seg.op) {
      case 0:
        path->StartFigure();
        current = Gdiplus::PointF(seg.a, seg.b);
        break;
      case 1:
        path->AddLine(current, Gdiplus::PointF(seg.a, seg.b));
        current = Gdiplus::PointF(seg.a, seg.b);
        break;
      case 2:
        path->AddBezier(current, Gdiplus::PointF(seg.a, seg.b), Gdiplus::PointF(seg.c, seg.d),
                        Gdiplus::PointF(seg.e, seg.f));
        current = Gdiplus::PointF(seg.e, seg.f);
        break;
      default:
        path->CloseFigure();
        break;
    }
  }
}

std::wstring ResourceDir() {
  wchar_t exe_path[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  std::wstring path(exe_path);
  const size_t slash = path.find_last_of(L'\\');
  if (slash == std::wstring::npos) {
    return std::wstring();
  }
  path.resize(slash + 1);
  return path + L"resources\\onboarding\\";
}

COLORREF Mix(COLORREF a, COLORREF b, double t) {
  const auto ch = [&](int x, int y) {
    return static_cast<int>(std::lround(x + (y - x) * t));
  };
  return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)),
             ch(GetBValue(a), GetBValue(b)));
}

// The Mac's NextStepConsentState: metrics sharing defaults OFF for a
// European locale, ON elsewhere.
bool MetricsDefaultForLocale() {
  wchar_t region[16] = {};
  if (::GetUserDefaultGeoName(region, 16) == 0) {
    return false;
  }
  static constexpr std::wstring_view kEurope[] = {
      L"AD", L"AL", L"AT", L"AX", L"BA", L"BE", L"BG", L"BY", L"CH", L"CY",
      L"CZ", L"DE", L"DK", L"EE", L"ES", L"FI", L"FO", L"FR", L"GB", L"GG",
      L"GI", L"GR", L"HR", L"HU", L"IE", L"IM", L"IS", L"IT", L"JE", L"LI",
      L"LT", L"LU", L"LV", L"MC", L"MD", L"ME", L"MK", L"MT", L"NL", L"NO",
      L"PL", L"PT", L"RO", L"RS", L"RU", L"SE", L"SI", L"SJ", L"SK", L"SM",
      L"UA", L"VA", L"XK"};
  const std::wstring_view mine(region);
  for (std::wstring_view code : kEurope) {
    if (code == mine) {
      return false;
    }
  }
  return true;
}

std::wstring Trim(const std::wstring& in) {
  const size_t first = in.find_first_not_of(L" \t\r\n");
  if (first == std::wstring::npos) {
    return std::wstring();
  }
  const size_t last = in.find_last_not_of(L" \t\r\n");
  return in.substr(first, last - first + 1);
}

size_t CodePoints(const std::wstring& text) {
  size_t count = 0;
  for (wchar_t c : text) {
    if (c < 0xDC00 || c > 0xDFFF) {
      ++count;  // Low surrogates ride with their high half.
    }
  }
  return count;
}

int TextWidth(HDC dc, HFONT font, const std::wstring& text) {
  HGDIOBJ old_font = ::SelectObject(dc, font);
  RECT rc = {0, 0, 0, 0};
  PhiDrawTextAA(dc, text.c_str(), -1, &rc, DT_CALCRECT | DT_SINGLELINE);
  ::SelectObject(dc, old_font);
  return rc.right - rc.left;
}

}  // namespace

// A decoded resource image at one pixel size (premultiplied BGRA).
struct PhiOnboardingWindow::Image {
  std::vector<uint8_t> bgra;
  int width = 0;
  int height = 0;
  bool ok = false;
};

PhiOnboardingWindow::PhiOnboardingWindow(Host* host,
                                         PhiPreferences* prefs,
                                         PhiAccountAndFeedback* account)
    : host_(host),
      prefs_(prefs),
      account_(account),
      oobe_([this](const PhiOobeAnalyticsSession::Event& event) {
        host_->CaptureAnalytics(kAnalyticsModuleOobe, event.name,
                                PhiOobeEventProperties(event));
      }) {
  metrics_checked_ = MetricsDefaultForLocale();
}

// static
PhiOobeAnalyticsSession::Step PhiOnboardingWindow::StepFor(Page page) {
  using Step = PhiOobeAnalyticsSession::Step;
  switch (page) {
    case Page::kLogin:
    case Page::kSigningIn:
      return Step::kLogin;
    case Page::kGuestPrivacy:
      return Step::kGuestPrivacy;
    case Page::kSetName:
      return Step::kSetName;
    case Page::kWelcome:
      return Step::kSetTheme;
    case Page::kLayout:
      return Step::kLayoutSelection;
    case Page::kNextSteps:
      return Step::kNextStep;
  }
  return Step::kLogin;
}

// static
double PhiOnboardingWindow::Uptime() {
  return static_cast<double>(::GetTickCount64()) / 1000.0;
}

void PhiOnboardingWindow::AdvanceTo(Page page) {
  oobe_.CompleteCurrentStep(Uptime());
  ShowPage(page);
}

void PhiOnboardingWindow::NoteAppTerminating() {
  oobe_.Interrupt(PhiOobeAnalyticsSession::Interruption::kAppTerminated,
                  Uptime());
}

PhiOnboardingWindow::~PhiOnboardingWindow() {
  if (field_brush_) {
    ::DeleteObject(field_brush_);
  }
}

// static
PhiOnboardingWindow::Page PhiOnboardingWindow::PageForPhase(int phase) {
  switch (phase) {
    case 2:
      return Page::kWelcome;
    case 3:
      return Page::kLayout;
    case 4:  // Passwords Manager: no Windows counterpart, skipped.
    case 5:
      return Page::kNextSteps;
    default:
      return Page::kSetName;
  }
}

// static
int PhiOnboardingWindow::PhaseForPage(Page page) {
  switch (page) {
    case Page::kSetName:
      return 1;
    case Page::kWelcome:
      return 2;
    case Page::kLayout:
      return 3;
    case Page::kNextSteps:
      return 5;
    default:
      return 0;
  }
}

// static
const char* PhiOnboardingWindow::PageName(Page page) {
  switch (page) {
    case Page::kLogin:
      return "login";
    case Page::kSigningIn:
      return "signingIn";
    case Page::kGuestPrivacy:
      return "guestPrivacy";
    case Page::kSetName:
      return "setName";
    case Page::kWelcome:
      return "welcome";
    case Page::kLayout:
      return "layout";
    case Page::kNextSteps:
      return "nextSteps";
  }
  return "";
}

std::string PhiOnboardingWindow::StateJson() const {
  std::string json = "{\"page\":\"";
  json += PageName(page_);
  json += "\",\"legal\":";
  json += legal_checked_ ? "true" : "false";
  json += ",\"metrics\":";
  json += metrics_checked_ ? "true" : "false";
  json += ",\"theme\":\"" + theme_id_ + "\",\"cardStyle\":{";
  bool first = true;
  for (const auto& [key, value] : card_style_.ToMap()) {
    json += std::string(first ? "" : ",") + "\"" + key + "\":\"" + value + "\"";
    first = false;
  }
  json += "},\"memberId\":\"" +
          PhiHolographicCard::MemberId(account_->auth().user().sub) + "\",\"layout\":\"";
  json += PhiLayoutModeName(static_cast<PhiLayoutMode>(layout_choice_));
  json += "\",\"embeddedBrowser\":";
  json += browser_.valid() ? "true" : "false";
  json += ",\"nameSubmitting\":";
  json += name_submitting_ ? "true" : "false";
  json += ",\"nameError\":\"" + base::WideToUTF8(name_error_) + "\"}";
  return json;
}

std::string PhiOnboardingWindow::Drive(const std::string& action,
                                       const std::string& value) {
  Hit hit;
  if (action == "signIn") {
    if (page_ != Page::kLogin) {
      return "wrong_page";
    }
    hit.control = Control::kSignIn;
  } else if (action == "guest") {
    if (page_ != Page::kLogin) {
      return "wrong_page";
    }
    hit.control = Control::kContinueAsGuest;
  } else if (action == "cancel") {
    hit.control = Control::kCancelSignIn;
  } else if (action == "legal") {
    hit.control = Control::kLegalCheck;
  } else if (action == "metrics") {
    hit.control = Control::kMetricsCheck;
  } else if (action == "next") {
    OnEnter();
    return std::string();
  } else if (action == "close") {
    oobe_.Interrupt(PhiOobeAnalyticsSession::Interruption::kWindowClosed,
                    Uptime());
    host_->OnboardingClosed();
    return std::string();
  } else if (action == "name") {
    if (page_ != Page::kSetName || !name_field_) {
      return "wrong_page";
    }
    ::SetWindowTextW(name_field_, base::UTF8ToWide(value).c_str());
    return std::string();
  } else if (action == "swatch") {
    if (page_ != Page::kWelcome) {
      return "wrong_page";
    }
    const PhiTheme& theme = PhiThemeById(value);
    if (value != theme.id) {
      return "unknown_theme";
    }
    // Live, as the Mac's themeChanged: the selection IS the saved theme.
    prefs_->SetThemeId(theme.id);
    ApplyTheme(theme.id);
    Layout();
    Invalidate();
    return std::string();
  } else if (action == "randomize") {
    if (page_ != Page::kWelcome) {
      return "wrong_page";
    }
    hit.control = Control::kDice;
  } else if (action == "layout") {
    hit.control = Control::kLayoutRow;
    if (value == "performance") {
      hit.key = static_cast<int>(PhiLayoutMode::kPerformance);
    } else if (value == "balanced") {
      hit.key = static_cast<int>(PhiLayoutMode::kBalanced);
    } else if (value == "comfortable") {
      hit.key = static_cast<int>(PhiLayoutMode::kComfortable);
    } else {
      return "unknown_layout";
    }
  } else {
    return "unknown_action";
  }
  Activate(hit);
  return std::string();
}

bool PhiOnboardingWindow::Open(Page page) {
  ClassSpec spec;
  spec.name = kClassName;
  Options options;
  options.caption_buttons = kPhiCaptionClose;
  options.dialog_keys = kPhiDialogTab;
  const RECT bounds = {CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT + 100,
                       CW_USEDEFAULT + 100};
  if (!CreateSecondary(spec, 0,
                       WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                       bounds, nullptr, L"Phi", options)) {
    LOG(ERROR) << "[Onboarding] CreateWindowEx failed err=" << ::GetLastError();
    return false;
  }
  const SIZE outer = chrome().OuterSize(kWidth, kHeight);
  ::SetWindowPos(hwnd(), nullptr, 0, 0, outer.cx, outer.cy,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  CenterOn(nullptr);
  card_timer_ = NewTimer();
  card_.SetNotify(hwnd(), kMsgNavigate + 1);
  card_.SetCacheDir(prefs_->user_data_dir() + L"\\PhiCardArtwork");
  theme_id_ = prefs_->ThemeId();
  // Pre-warm the default style's artwork so the Welcome page's card is
  // ready when the flow reaches it.
  card_style_.palette = PhiHolographicCardStyle::PaletteForTheme(theme_id_);
  card_.SetStyle(card_style_);
  card_.Prepare(CardRect());
  ShowPage(page);
  ::ShowWindow(hwnd(), SW_SHOW);
  ::SetForegroundWindow(hwnd());
  return true;
}

void PhiOnboardingWindow::ShowPage(Page page) {
  page_ = page;
  pressed_ = -1;
  scroll_ = 0;
  oobe_.Present(StepFor(page), Uptime());
  if (page == Page::kSetName) {
    std::string name;
    account_->AccountInfo(&name, nullptr);
    original_name_ = name;
    name_error_.clear();
    name_submitting_ = false;
    EnsureNameField();
    ::SetWindowTextW(name_field_, base::UTF8ToWide(name).c_str());
    ::ShowWindow(name_field_, SW_SHOW);
    ::SetFocus(name_field_);
    ::SendMessageW(name_field_, EM_SETSEL, 0, -1);
  } else if (name_field_) {
    ::ShowWindow(name_field_, SW_HIDE);
  }
  if (page == Page::kWelcome) {
    theme_id_ = prefs_->ThemeId();
    LoadCard();
    ApplyTheme(theme_id_);
    card_.ReplayEntrance();
  }
  if (page == Page::kGuestPrivacy || page == Page::kNextSteps) {
    legal_checked_ = false;
    metrics_checked_ = MetricsDefaultForLocale();
  }
  PlaceEmbeddedBrowser();
  Layout();
  Invalidate();
}

RECT PhiOnboardingWindow::ContentDip(int x, int y, int w, int h) const {
  // Footer rows ride the bottom-anchored page, everything else the
  // scrolled one; on a page that fits the two are the same rect.
  const RECT content = PageRect(/*footer=*/y >= FooterTopDip());
  return RECT{content.left + Scaled(x), content.top + Scaled(y),
              content.left + Scaled(x + w), content.top + Scaled(y + h)};
}

int PhiOnboardingWindow::FooterTopDip() const {
  return page_ == Page::kLogin ? kLoginFooterTop : kFooterTop;
}

int PhiOnboardingWindow::MaxScroll() const {
  if (page_ == Page::kSigningIn) {
    return 0;  // The hosted login fills the window and scrolls itself.
  }
  const RECT content = ContentRect();
  const int short_by = Scaled(kHeight) - (content.bottom - content.top);
  // A pixel or two is the caption's rounding, not a short screen.
  return short_by > 2 ? short_by : 0;
}

RECT PhiOnboardingWindow::PageRect(bool footer) const {
  const RECT content = ContentRect();
  const int top = footer && MaxScroll() > 0
                      ? content.bottom - Scaled(kHeight)
                      : content.top - scroll_;
  return RECT{content.left, top, content.right, top + Scaled(kHeight)};
}

int PhiOnboardingWindow::FooterSplit() const {
  return PageRect(/*footer=*/true).top + Scaled(FooterTopDip());
}

void PhiOnboardingWindow::ScrollTo(int px) {
  const int next = std::clamp(px, 0, MaxScroll());
  if (next == scroll_) {
    return;
  }
  scroll_ = next;
  Layout();
  if (name_field_ && page_ == Page::kSetName) {
    EnsureNameField();  // Re-seats the EDIT with the page.
  }
  Invalidate();
}

void PhiOnboardingWindow::Refit() {
  // The design size at this DPI, cut down to the work area again.
  const SIZE outer = chrome().OuterSize(kWidth, kHeight);
  ::SetWindowPos(hwnd(), nullptr, 0, 0, outer.cx, outer.cy,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  FitToWorkArea();
  Layout();
  if (name_field_ && page_ == Page::kSetName) {
    EnsureNameField();
  }
  PlaceEmbeddedBrowser();
  Invalidate();
}

void PhiOnboardingWindow::AddControl(Control control, int key,
                                     const RECT& bounds, std::wstring name,
                                     PhiHitRole role, bool selected,
                                     bool enabled) {
  Hit hit;
  hit.control = control;
  hit.key = key;
  hit.bounds = bounds;
  hit.enabled = enabled;
  hits_.push_back(hit);
  // On a split page the pointer reaches only the part of a control showing
  // in its own region: a row scrolled under the footer is not clickable
  // through the pill. The footer's controls are the ones ContentDip
  // anchors (their tops all sit below FooterTopDip()).
  RECT reach = bounds;
  if (MaxScroll() > 0) {
    const RECT content = ContentRect();
    const int split = FooterSplit();
    const bool footer = control == Control::kNext ||
                        control == Control::kSignIn ||
                        control == Control::kContinueAsGuest;
    const RECT region = {content.left, footer ? split : content.top,
                         content.right, footer ? content.bottom : split};
    if (!::IntersectRect(&reach, &bounds, &region)) {
      reach = RECT{};
    }
  }
  AddHit(reach, static_cast<int>(hits_.size()) - 1, std::move(name), role,
         selected, enabled);
}

const PhiOnboardingWindow::Hit* PhiOnboardingWindow::HitOf(int index) const {
  if (index < 0 || index >= static_cast<int>(hits().size())) {
    return nullptr;
  }
  const int key = hits()[static_cast<size_t>(index)].key;
  if (key < 0 || key >= static_cast<int>(hits_.size())) {
    return nullptr;
  }
  return &hits_[static_cast<size_t>(key)];
}

void PhiOnboardingWindow::Layout() {
  ClearHits();
  hits_.clear();
  scroll_ = std::clamp(scroll_, 0, MaxScroll());
  const bool guest = prefs_->GuestModeEnabled();
  switch (page_) {
    case Page::kLogin: {
      const int guest_top = kHeight - 91 - 22;
      AddControl(Control::kSignIn, 0,
                 ContentDip((kWidth - kPillWidth) / 2, guest_top - 12 - kPillHeight,
                            kPillWidth, kPillHeight),
                 PhiStr("oobe.login.loginButton"), PhiHitRole::kButton, false,
                 !account_->AccountSigningIn());
      if (!guest) {
        AddControl(Control::kContinueAsGuest, 0,
                   ContentDip(120, guest_top, kWidth - 240, 22),
                   PhiStr("oobe.login.continueAsGuestButton"),
                   PhiHitRole::kButton, false, true);
      }
      break;
    }
    case Page::kSigningIn:
      // No header row: the hosted login fills the window (Escape and the
      // window's close button still cancel).
      break;
    case Page::kGuestPrivacy:
    case Page::kNextSteps: {
      const int card_top =
          page_ == Page::kGuestPrivacy ? kTitleTop + kTitleHeight + 12
                                       : kHeight - kButtonBottom - kPillHeight - 24 -
                                             kCardHeight;
      const int card_left = (kWidth - kCardWidth) / 2;
      const int divider = card_top + 20 + kGuideHeight + 20;
      const int rows_top = divider + 28;
      const int row_left = card_left + kCardPad;
      const int row_width = kCardWidth - 2 * kCardPad;
      const RECT legal = ContentDip(row_left, rows_top, row_width, 22);
      AddControl(Control::kLegalCheck, 0, legal,
                 PhiStr(page_ == Page::kGuestPrivacy
                            ? "oobe.guestPrivacy.legalAgreement"
                            : "oobe.nextSteps.legalAgreement"),
                 PhiHitRole::kToggle, legal_checked_, true);
      if (page_ == Page::kGuestPrivacy) {
        AddConsentLinks(legal, "oobe.guestPrivacy.legalAgreement",
                        "oobe.guestPrivacy.privacyPolicyLink",
                        "oobe.guestPrivacy.termsOfServiceLink");
      } else {
        AddConsentLinks(legal, "oobe.nextSteps.legalAgreement",
                        "oobe.nextSteps.privacyLink", "oobe.nextSteps.termsLink");
      }
      AddControl(Control::kMetricsCheck, 0,
                 ContentDip(row_left, rows_top + 22 + 12, row_width, 44),
                 PhiStr(page_ == Page::kGuestPrivacy
                            ? "oobe.guestPrivacy.metricsConsent"
                            : "oobe.nextSteps.metricsConsent"),
                 PhiHitRole::kToggle, metrics_checked_, true);
      const std::wstring begin = PhiStr(page_ == Page::kGuestPrivacy
                                            ? "oobe.guestPrivacy.beginButton"
                                            : "oobe.nextSteps.beginButton");
      HDC dc = ::GetDC(hwnd());
      const int text_w =
          dc ? TextWidth(dc, fonts().Get(16, FW_MEDIUM), begin) : Scaled(90);
      if (dc) {
        ::ReleaseDC(hwnd(), dc);
      }
      const int pill_w = std::clamp(
          static_cast<int>(std::lround(text_w / (dpi() / 96.0))) + 32,
          kPillWidth, kCardWidth);
      AddControl(Control::kNext, 0,
                 ContentDip((kWidth - pill_w) / 2,
                            kHeight - kButtonBottom - kPillHeight, pill_w,
                            kPillHeight),
                 begin, PhiHitRole::kButton, false, legal_checked_);
      break;
    }
    case Page::kSetName:
      AddControl(Control::kNext, 0,
                 ContentDip((kWidth - kPillWidth) / 2,
                            kHeight - kButtonBottom - kPillHeight, kPillWidth,
                            kPillHeight),
                 PhiStr("oobe.navigation.continueButton"), PhiHitRole::kButton,
                 false, !name_submitting_);
      break;
    case Page::kWelcome: {
      // The membership card, centred, and the dice under it.
      std::string name;
      account_->AccountInfo(&name, nullptr);
      AddControl(Control::kCard, 0, CardRect(), base::UTF8ToWide(name),
                 PhiHitRole::kImage, false, true);
      AddControl(Control::kDice, 0,
                 ContentDip((kWidth - kDiceSize) / 2, kDiceTop, kDiceSize, kDiceSize),
                 PhiStr("oobe.welcome.randomizeCard"), PhiHitRole::kButton, false,
                 true);
      AddControl(Control::kNext, 0,
                 ContentDip((kWidth - kPillWidth) / 2,
                            kHeight - kButtonBottom - kPillHeight, kPillWidth,
                            kPillHeight),
                 PhiStr("oobe.welcome.nextButton"), PhiHitRole::kButton, false,
                 true);
      break;
    }
    case Page::kLayout: {
      // Two layouts (impl log D9), Edge's words.
      const int container_h = 8 + 2 * kLayoutRowHeight + 8 + 8;
      const int container_top = (kHeight - container_h) / 2;
      const std::array<std::pair<PhiLayoutMode, const char*>, 2> rows = {{
          {PhiLayoutMode::kBalanced, "appMenu.verticalTabs"},
          {PhiLayoutMode::kComfortable, "appMenu.horizontalTabs"},
      }};
      for (size_t i = 0; i < rows.size(); ++i) {
        const int top = container_top + 8 + static_cast<int>(i) * (kLayoutRowHeight + 8);
        AddControl(Control::kLayoutRow, static_cast<int>(rows[i].first),
                   ContentDip(84 + 8, top, kLayoutRowWidth, kLayoutRowHeight),
                   PhiStr(rows[i].second), PhiHitRole::kListItem,
                   layout_choice_ == static_cast<int>(rows[i].first), true);
      }
      AddControl(Control::kNext, 0,
                 ContentDip((kWidth - kPillWidth) / 2,
                            kHeight - kButtonBottom - kPillHeight, kPillWidth,
                            kPillHeight),
                 PhiStr("oobe.navigation.continueButton"), PhiHitRole::kButton,
                 false, true);
      break;
    }
  }
}

// --- Resources --------------------------------------------------------------

PhiOnboardingWindow::Image& PhiOnboardingWindow::ResourceImage(
    const wchar_t* file, int width, int height) {
  const std::wstring key = std::wstring(file) + L"@" + std::to_wstring(width) +
                           L"x" + std::to_wstring(height);
  auto found = images_.find(key);
  if (found != images_.end()) {
    return *found->second;
  }
  auto image = std::make_unique<Image>();
  image->width = width;
  image->height = height;
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICBitmapScaler> scaler;
  ComPtr<IWICFormatConverter> converter;
  const std::wstring path = ResourceDir() + file;
  if (SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory))) &&
      SUCCEEDED(factory->CreateDecoderFromFilename(
          path.c_str(), nullptr, GENERIC_READ,
          WICDecodeMetadataCacheOnDemand, &decoder)) &&
      SUCCEEDED(decoder->GetFrame(0, &frame)) &&
      SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
      SUCCEEDED(scaler->Initialize(frame.Get(), static_cast<UINT>(width),
                                   static_cast<UINT>(height),
                                   WICBitmapInterpolationModeFant)) &&
      SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
      SUCCEEDED(converter->Initialize(
          scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
          nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
    image->bgra.assign(static_cast<size_t>(width) * height * 4, 0);
    image->ok = SUCCEEDED(converter->CopyPixels(
        nullptr, static_cast<UINT>(width * 4),
        static_cast<UINT>(image->bgra.size()), image->bgra.data()));
  }
  if (!image->ok) {
    LOG(WARNING) << "[Onboarding] resource did not decode: "
                 << base::WideToUTF8(file);
  }
  Image& ref = *image;
  images_[key] = std::move(image);
  return ref;
}

void PhiOnboardingWindow::PaintImage(HDC dc, const RECT& dest, Image& image,
                                     bool blend) {
  if (!image.ok) {
    return;
  }
  if (blend) {
    PhiBlendBgra(dc, dest, image.bgra.data(), image.width, image.height);
  } else {
    PhiDrawBgra(dc, dest, image.bgra.data(), image.width, image.height);
  }
}

HFONT PhiOnboardingWindow::BrandTitleFont(const std::wstring& text,
                                          int size_dip) {
  PhiFontDesc desc;
  desc.size = size_dip;
  desc.weight = FW_SEMIBOLD;
  if (PhiBrandFaceRenders(text)) {
    desc.face = kPhiBrandDisplayFace;
    desc.italic = true;
  }
  return fonts().Get(desc);
}

RECT PhiOnboardingWindow::CardRect() const {
  return ContentDip((kWidth - PhiHolographicCard::kDesignWidthDip) / 2, kCardTop,
                    PhiHolographicCard::kDesignWidthDip,
                    PhiHolographicCard::kDesignHeightDip);
}

void PhiOnboardingWindow::LoadCard() {
  // The Mac's OnboardingWelcomeViewController.requestProfile: the member id
  // from the Auth0 subject, the name and the join date from the profile
  // (already fetched by the sign-in), and the account's saved style when it
  // has one (a repeat of the page).
  const std::string sub = account_->auth().user().sub;
  std::string name;
  account_->AccountInfo(&name, nullptr);
  const std::optional<PhiAccountProfile>& profile = account_->Profile().profile();
  const bool profile_matches = profile && profile->auth0_id == sub;
  card_.SetIdentity(name.empty() ? "Phi" : name, PhiHolographicCard::MemberId(sub),
                    profile_matches ? PhiHolographicCard::FormatJoinDate(profile->created_at)
                                    : std::string());
  const std::map<std::string, std::string> saved = prefs_->ProfileCardStyle(sub);
  if (!saved.empty()) {
    card_style_ = PhiHolographicCardStyle::FromMap(saved);
  }
}

void PhiOnboardingWindow::ApplyTheme(const std::string& theme_id) {
  theme_id_ = theme_id;
  card_style_.palette = PhiHolographicCardStyle::PaletteForTheme(theme_id_);
  card_.SetStyle(card_style_);
}

void PhiOnboardingWindow::RandomizeCard() {
  // The Mac's randomizeCard: every selection anew, and the app theme
  // follows the palette.
  card_style_ = PhiHolographicCardStyle::Random();
  const std::string theme_id = PhiHolographicCardStyle::ThemeIdFor(card_style_.palette);
  prefs_->SetThemeId(theme_id);
  ApplyTheme(theme_id);
  Layout();
  Invalidate();
}

void PhiOnboardingWindow::StartCardTimer() {
  if (card_.animating() && !card_timer_.running()) {
    card_timer_.Start(16, /*coalescable=*/true);
  }
}

// --- Paint ------------------------------------------------------------------

PhiPopupColors PhiOnboardingWindow::QueryColors() const {
  // The pages are the Mac's dark onboarding (forced dark colour scheme);
  // the caption band follows.
  return PhiPopupColorsFor(/*dark=*/true, prefs_->Palette().accent);
}

void PhiOnboardingWindow::Paint(HDC dc, const RECT&) {
  const RECT content = ContentRect();
  ::SetBkMode(dc, TRANSPARENT);
  if (MaxScroll() == 0) {
    PaintPage(dc, page_ == Page::kSigningIn ? content : PageRect(false));
    PaintFooterControls(dc);
  } else {
    // Taller than the work area: the page above the split scrolls in its
    // viewport; below it the footer — the page's own bottom, anchored —
    // keeps the primary button on screen.
    const int split = FooterSplit();
    int saved = ::SaveDC(dc);
    ::IntersectClipRect(dc, content.left, content.top, content.right, split);
    PaintPage(dc, PageRect(/*footer=*/false));
    ::RestoreDC(dc, saved);
    saved = ::SaveDC(dc);
    ::IntersectClipRect(dc, content.left, split, content.right, content.bottom);
    PaintGround(dc, PageRect(/*footer=*/true));
    PaintFooterControls(dc);
    ::RestoreDC(dc, saved);
    if (scroll_ < MaxScroll()) {
      // More page under the footer: its edge, and a thin scroll thumb.
      RECT edge = {content.left, split, content.right,
                   split + std::max(1, Scaled(1))};
      HBRUSH brush = ::CreateSolidBrush(RGB(0x44, 0x44, 0x44));
      ::FillRect(dc, &edge, brush);
      ::DeleteObject(brush);
    }
    const int view_h = split - content.top;
    const int total = view_h + MaxScroll();
    const int thumb_h =
        std::max(Scaled(24), static_cast<int>(static_cast<int64_t>(view_h) *
                                              view_h / std::max(1, total)));
    const int thumb_top =
        content.top + static_cast<int>(static_cast<int64_t>(view_h - thumb_h) *
                                       scroll_ / MaxScroll());
    const RECT thumb = {content.right - Scaled(6), thumb_top + Scaled(2),
                        content.right - Scaled(3),
                        thumb_top + thumb_h - Scaled(2)};
    PhiFillCapsuleAA(dc, thumb, RGB(0x8A, 0x8A, 0x8A));
  }
  PaintBand(dc);
}

void PhiOnboardingWindow::PaintPage(HDC dc, const RECT& content) {
  switch (page_) {
    case Page::kLogin:
      PaintLogin(dc, content);
      break;
    case Page::kSigningIn:
      PaintSigningIn(dc, content);
      break;
    case Page::kGuestPrivacy:
      PaintGuestPrivacy(dc, content);
      break;
    case Page::kSetName:
      PaintSetName(dc, content);
      break;
    case Page::kWelcome:
      PaintWelcome(dc, content);
      break;
    case Page::kLayout:
      PaintLayout(dc, content);
      break;
    case Page::kNextSteps:
      PaintNextSteps(dc, content);
      break;
  }
}

void PhiOnboardingWindow::PaintGround(HDC dc, const RECT& content) {
  switch (page_) {
    case Page::kLogin:
      PaintImage(dc, content,
                 ResourceImage(L"login-bg.jpg", content.right - content.left,
                               content.bottom - content.top),
                 false);
      break;
    case Page::kSigningIn:
      break;
    default:
      PaintDarkGround(dc, content);
      break;
  }
}

void PhiOnboardingWindow::PaintFooterControls(HDC dc) {
  // The page's bottom row — painted after the page, or alone in the
  // anchored footer when the page is split.
  for (size_t i = 0; i < hits_.size(); ++i) {
    const Hit& hit = hits_[i];
    const bool hot = hovered() == static_cast<int>(i);
    if (hit.control == Control::kSignIn) {
      PaintPillButton(dc, hit.bounds, PhiStr("oobe.login.loginButton"),
                      hit.enabled, hot, /*gradient_border=*/true,
                      RGB(255, 255, 255), RGB(255, 255, 255), RGB(0, 0, 0));
    } else if (hit.control == Control::kContinueAsGuest) {
      HGDIOBJ old_font = ::SelectObject(dc, fonts().Get(13, FW_NORMAL));
      ::SetTextColor(dc, hot ? RGB(0xFF, 0xFF, 0xFF) : RGB(0xA6, 0xA6, 0xA6));
      RECT rc = hit.bounds;
      PhiDrawTextAA(dc, PhiStr("oobe.login.continueAsGuestButton"), -1, &rc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      ::SelectObject(dc, old_font);
    } else if (hit.control == Control::kNext && page_ == Page::kWelcome) {
      PaintPillButton(dc, hit.bounds, PhiStr("oobe.welcome.nextButton"),
                      hit.enabled, hot, true, RGB(255, 255, 255),
                      RGB(255, 255, 255), RGB(0x80, 0x80, 0x80));
    } else if (hit.control == Control::kNext) {
      const char* label = page_ == Page::kGuestPrivacy
                              ? "oobe.guestPrivacy.beginButton"
                          : page_ == Page::kNextSteps
                              ? "oobe.nextSteps.beginButton"
                              : "oobe.navigation.continueButton";
      PaintPillButton(dc, hit.bounds, PhiStr(label), hit.enabled, hot, true,
                      RGB(255, 255, 255), RGB(255, 255, 255), RGB(0, 0, 0));
    }
  }
  if (page_ != Page::kLogin) {
    return;
  }
  // A failed attempt's note, under the guest link (the Mac's retry hint).
  const std::string error = account_->AccountLastError();
  if (!error.empty() && !account_->AccountSigningIn()) {
    const char* key = error == "account_mismatch"
                          ? "settings.account.error.accountMismatch"
                      : error == "refused" ? "settings.account.error.refused"
                                           : "settings.account.error.incomplete";
    HGDIOBJ old_font = ::SelectObject(dc, fonts().Get(13, FW_NORMAL));
    ::SetTextColor(dc, kLinkInk);
    RECT rc = ContentDip(40, kHeight - 60, kWidth - 80, 22);
    PhiDrawTextAA(dc, PhiStr(key), -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SelectObject(dc, old_font);
  }
}

void PhiOnboardingWindow::PaintDarkGround(HDC dc, const RECT& content) {
  // OnboardingBaseViewController's ground: the wallpaper still with the dot
  // pattern at 8%.
  const int w = content.right - content.left;
  const int h = content.bottom - content.top;
  PaintImage(dc, content, ResourceImage(L"wallpaper.png", w, h), false);
  Image& dots = ResourceImage(L"dot-bg.png", w, h);
  if (dots.ok) {
    // The pattern is dark dots on white; tint = white at 8% of the dots'
    // darkness.
    std::vector<uint8_t> layer(dots.bgra.size(), 0);
    for (size_t i = 0; i + 3 < layer.size(); i += 4) {
      const uint8_t a = static_cast<uint8_t>((255 - dots.bgra[i]) * 8 / 100);
      layer[i] = a;
      layer[i + 1] = a;
      layer[i + 2] = a;
      layer[i + 3] = a;
    }
    PhiBlendBgra(dc, content, layer.data(), w, h);
  }
}

void PhiOnboardingWindow::PaintTitle(HDC dc, const RECT& content,
                                     const std::wstring& text, COLORREF color,
                                     int top_dip) {
  HFONT font = BrandTitleFont(text, kTitleSize);
  HGDIOBJ old_font = ::SelectObject(dc, font);
  ::SetTextColor(dc, color);
  RECT rc = {content.left + Scaled(30), content.top + Scaled(top_dip),
             content.right - Scaled(30), content.top + Scaled(top_dip + kTitleHeight)};
  PhiDrawTextAA(dc, text.c_str(), -1, &rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
  ::SelectObject(dc, old_font);
}

void PhiOnboardingWindow::PaintPillButton(HDC dc, const RECT& bounds,
                                          const std::wstring& text,
                                          bool enabled, bool hot,
                                          bool gradient_border, COLORREF ink,
                                          COLORREF border, COLORREF fill) {
  const int w = bounds.right - bounds.left;
  const int h = bounds.bottom - bounds.top;
  const double radius = h / 2.0;
  const double stroke = std::max(1.0, dpi() / 96.0);
  // The pill as a premultiplied layer: the fill inside, the ring on top
  // (the Mac's angular gradient border, or a flat colour).
  std::vector<uint8_t> layer(static_cast<size_t>(w) * h * 4, 0);
  const double cx = w / 2.0, cy = h / 2.0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const double outer = PhiRoundRectCoverage(x + 0.5, y + 0.5, w, h, radius);
      if (outer <= 0.0) {
        continue;
      }
      const double inner = PhiRoundRectCoverage(x + 0.5 - stroke, y + 0.5 - stroke,
                                                w - 2 * stroke, h - 2 * stroke,
                                                radius - stroke);
      COLORREF ring = border;
      if (gradient_border) {
        double angle = std::atan2(y - cy, x - cx) / (2 * 3.14159265358979) + 0.5;
        angle = std::fmod(angle * 4.0, 4.0);
        const size_t seg = static_cast<size_t>(std::floor(angle));
        ring = Mix(kPillGradient[seg], kPillGradient[seg + 1], angle - seg);
      }
      double alpha = outer * (enabled ? 1.0 : 0.5);
      // Composite: fill under the ring.
      const double fill_a = GetRValue(fill) + GetGValue(fill) + GetBValue(fill) == 0
                                ? 0.0
                                : 1.0;
      double r = 0, g = 0, b = 0, a = 0;
      if (fill_a > 0) {
        const double fa = inner * 0.10 * alpha;  // The Mac's fill is at 10%.
        r += GetRValue(fill) * fa;
        g += GetGValue(fill) * fa;
        b += GetBValue(fill) * fa;
        a += fa;
      }
      const double ra = (outer - inner) * alpha * (hot ? 1.0 : 0.85);
      r += GetRValue(ring) * ra * (1 - a) + r * 0;
      g += GetGValue(ring) * ra * (1 - a);
      b += GetBValue(ring) * ra * (1 - a);
      a += ra * (1 - a);
      const size_t p = (static_cast<size_t>(y) * w + x) * 4;
      layer[p] = static_cast<uint8_t>(std::lround(std::min(255.0, b)));
      layer[p + 1] = static_cast<uint8_t>(std::lround(std::min(255.0, g)));
      layer[p + 2] = static_cast<uint8_t>(std::lround(std::min(255.0, r)));
      layer[p + 3] = static_cast<uint8_t>(std::lround(std::min(255.0, a * 255)));
    }
  }
  PhiBlendBgra(dc, bounds, layer.data(), w, h);
  HGDIOBJ old_font = ::SelectObject(dc, fonts().Get(16, FW_MEDIUM));
  ::SetTextColor(dc, enabled ? ink : PhiPopupBlend(ink, RGB(0, 0, 0), 128));
  RECT rc = bounds;
  PhiDrawTextAA(dc, text.c_str(), -1, &rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  ::SelectObject(dc, old_font);
}

std::vector<PhiOnboardingWindow::Run> PhiOnboardingWindow::CheckRowRuns(
    HDC dc, const RECT& row, const std::wstring& pattern,
    const std::wstring& link_a, const std::wstring& link_b) {
  // "I agree to the {0} and {1}": the pattern's pieces along one line.
  std::vector<Run> runs;
  std::wstring rest = pattern;
  for (;;) {
    const size_t a = rest.find(L"{0}");
    const size_t b = rest.find(L"{1}");
    const size_t next = std::min(a, b);
    if (next == std::wstring::npos) {
      runs.push_back({rest, -1, {}});
      break;
    }
    if (next > 0) {
      runs.push_back({rest.substr(0, next), -1, {}});
    }
    runs.push_back({next == a ? link_a : link_b, next == a ? 0 : 1, {}});
    rest = rest.substr(next + 3);
  }
  HFONT font = fonts().Get(15, FW_NORMAL);
  int x = row.left + Scaled(kCheckSize) + Scaled(kCheckGap);
  const int line_h = Scaled(22);
  for (Run& run : runs) {
    const int width = TextWidth(dc, font, run.text);
    run.rect = {x, row.top, x + width, row.top + line_h};
    x += width;
  }
  return runs;
}

void PhiOnboardingWindow::AddConsentLinks(const RECT& row,
                                          const char* pattern_key,
                                          const char* link_a_key,
                                          const char* link_b_key) {
  HDC dc = ::GetDC(hwnd());
  if (!dc) {
    return;
  }
  const std::vector<Run> runs = CheckRowRuns(dc, row, PhiStr(pattern_key),
                                             PhiStr(link_a_key),
                                             PhiStr(link_b_key));
  ::ReleaseDC(hwnd(), dc);
  for (const Run& run : runs) {
    if (run.link == 0) {
      AddControl(Control::kPrivacyLink, 0, run.rect, PhiStr(link_a_key),
                 PhiHitRole::kButton, false, true);
    } else if (run.link == 1) {
      AddControl(Control::kTermsLink, 0, run.rect, PhiStr(link_b_key),
                 PhiHitRole::kButton, false, true);
    }
  }
}

void PhiOnboardingWindow::PaintCheckRow(HDC dc, const RECT& row, bool checked,
                                        const std::wstring& text,
                                        const std::wstring& link_a,
                                        const std::wstring& link_b) {
  const int check = Scaled(kCheckSize);
  RECT box = {row.left, row.top + Scaled(1), row.left + check,
              row.top + Scaled(1) + check};
  PaintImage(dc, box,
             ResourceImage(checked ? L"check.png" : L"uncheck.png", check, check),
             true);
  HFONT font = fonts().Get(15, FW_NORMAL);
  HGDIOBJ old_font = ::SelectObject(dc, font);
  const COLORREF body = RGB(0xD9, 0xD9, 0xD9);  // white 85% on the ground.
  if (link_a.empty()) {
    RECT text_rc = {row.left + check + Scaled(kCheckGap), row.top, row.right,
                    row.bottom};
    ::SetTextColor(dc, body);
    PhiDrawTextAA(dc, text.c_str(), -1, &text_rc,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    ::SelectObject(dc, old_font);
    return;
  }
  for (const Run& run : CheckRowRuns(dc, row, text, link_a, link_b)) {
    RECT rc = run.rect;
    ::SetTextColor(dc, run.link >= 0 ? kLinkInk : body);
    PhiDrawTextAA(dc, run.text.c_str(), -1, &rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (run.link >= 0) {
      RECT underline = {rc.left, rc.bottom - Scaled(4), rc.right,
                        rc.bottom - Scaled(4) + std::max(1, Scaled(1))};
      HBRUSH brush = ::CreateSolidBrush(kLinkInk);
      ::FillRect(dc, &underline, brush);
      ::DeleteObject(brush);
    }
  }
  ::SelectObject(dc, old_font);
}

void PhiOnboardingWindow::PaintLogin(HDC dc, const RECT& content) {
  const int w = content.right - content.left;
  const int h = content.bottom - content.top;
  PaintImage(dc, content, ResourceImage(L"login-bg.jpg", w, h), false);
  // The brand mark at the top of the control container (top 91).
  const int brand_w = Scaled(39), brand_h = Scaled(20);
  RECT brand = {content.left + (w - brand_w) / 2, content.top + Scaled(91),
                content.left + (w - brand_w) / 2 + brand_w,
                content.top + Scaled(91) + brand_h};
  PaintImage(dc, brand, ResourceImage(L"brand.png", brand_w, brand_h), true);
  // The sign-in pill, the guest link and the retry note are the footer
  // (PaintFooterControls).
}

void PhiOnboardingWindow::PaintSigningIn(HDC dc, const RECT& content) {
  // The embedded hosted login fills the content and paints itself.
  if (!browser_hwnd_) {
    // Waiting for the browser: the Mac's "Finish signing in in your
    // browser" over the dark ground.
    HBRUSH dark = ::CreateSolidBrush(RGB(0x12, 0x12, 0x12));
    ::FillRect(dc, &content, dark);
    ::DeleteObject(dark);
    HFONT font = BrandTitleFont(PhiStr("oobe.login.progressTitle"), 40);
    HGDIOBJ old_font = ::SelectObject(dc, font);
    ::SetTextColor(dc, RGB(255, 255, 255));
    RECT rc = {content.left + Scaled(40), content.top, content.right - Scaled(40),
               content.bottom};
    PhiDrawTextAA(dc, PhiStr("oobe.login.progressTitle"), -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
    ::SelectObject(dc, old_font);
  }
}

void PhiOnboardingWindow::PaintGuestPrivacy(HDC dc, const RECT& content) {
  PaintDarkGround(dc, content);
  PaintTitle(dc, content, PhiStr("oobe.guestPrivacy.title"), RGB(255, 255, 255),
             kTitleTop);
  const int card_top = kTitleTop + kTitleHeight + 12;
  const RECT card = ContentDip((kWidth - kCardWidth) / 2, card_top, kCardWidth,
                               kCardHeight);
  PhiFillRoundRectAA(dc, card, Scaled(kCardRadius), RGB(0x2A, 0x2A, 0x2A));
  // The three bullets, centred in the guide's height.
  const std::array<const char*, 3> bullets = {
      "oobe.guestPrivacy.aiFeaturesUnavailable",
      "oobe.guestPrivacy.browserMemoryUnavailable",
      "oobe.guestPrivacy.signInAnytime"};
  const int row_h = Scaled(28), row_gap = Scaled(20);
  const int block = 3 * row_h + 2 * row_gap;
  int y = card.top + Scaled(20) + (Scaled(kGuideHeight) - block) / 2;
  HFONT dot_font = fonts().Get(28, FW_MEDIUM);
  HFONT text_font = fonts().Get(20, FW_NORMAL);
  ::SetTextColor(dc, RGB(0xE6, 0xE6, 0xE6));
  for (const char* key : bullets) {
    RECT dot = {card.left + Scaled(kCardPad), y, card.left + Scaled(kCardPad + 14),
                y + row_h};
    HGDIOBJ old_font = ::SelectObject(dc, dot_font);
    PhiDrawTextAA(dc, L"·", -1, &dot, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ::SelectObject(dc, text_font);
    RECT rc = {dot.right + Scaled(12), y, card.right - Scaled(kCardPad), y + row_h};
    PhiDrawTextAA(dc, PhiStr(key), -1, &rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
    ::SelectObject(dc, old_font);
    y += row_h + row_gap;
  }
  // The divider and the consents.
  const int divider_y = card_top + 20 + kGuideHeight + 20;
  RECT divider = ContentDip((kWidth - kCardWidth) / 2 + kCardPad, divider_y,
                            kCardWidth - 2 * kCardPad, 1);
  HBRUSH line = ::CreateSolidBrush(RGB(0x3A, 0x3A, 0x3A));
  ::FillRect(dc, &divider, line);
  ::DeleteObject(line);
  for (const Hit& hit : hits_) {
    if (hit.control == Control::kLegalCheck) {
      PaintCheckRow(dc, hit.bounds, legal_checked_,
                    PhiStr("oobe.guestPrivacy.legalAgreement"),
                    PhiStr("oobe.guestPrivacy.privacyPolicyLink"),
                    PhiStr("oobe.guestPrivacy.termsOfServiceLink"));
    } else if (hit.control == Control::kMetricsCheck) {
      PaintCheckRow(dc, hit.bounds, metrics_checked_,
                    PhiStr("oobe.guestPrivacy.metricsConsent"), std::wstring(),
                    std::wstring());
    }
  }
}

void PhiOnboardingWindow::PaintNextSteps(HDC dc, const RECT& content) {
  PaintDarkGround(dc, content);
  PaintTitle(dc, content, PhiStr("oobe.nextSteps.title"), RGB(255, 255, 255),
             kTitleTop);
  const int card_top = kHeight - kButtonBottom - kPillHeight - 24 - kCardHeight;
  const RECT card = ContentDip((kWidth - kCardWidth) / 2, card_top, kCardWidth,
                               kCardHeight);
  PhiFillRoundRectAA(dc, card, Scaled(kCardRadius), RGB(0x2A, 0x2A, 0x2A));
  // The four numbered steps with the connector between their markers.
  const std::array<const char*, 4> steps = {
      "oobe.nextSteps.onboardAssistant", "oobe.nextSteps.importBrowserData",
      "oobe.nextSteps.understandFinePrint", "oobe.nextSteps.enjoyPhi"};
  const int marker = Scaled(kStepMarker);
  const int gap = Scaled(kStepGap);
  int y = card.top + Scaled(20) + Scaled(8);
  HFONT number_font = fonts().Get(13, FW_MEDIUM);
  HFONT text_font = fonts().Get(16, FW_NORMAL);
  for (size_t i = 0; i < steps.size(); ++i) {
    RECT circle = {card.left + Scaled(kCardPad), y,
                   card.left + Scaled(kCardPad) + marker, y + marker};
    PhiFillEllipseAA(dc, circle, RGB(0x3A, 0x3A, 0x3A));
    PhiStrokeEllipseAA(dc, circle, RGB(0x4A, 0x4A, 0x4A), std::max(1, Scaled(1)));
    HGDIOBJ old_font = ::SelectObject(dc, number_font);
    ::SetTextColor(dc, RGB(0xE6, 0xE6, 0xE6));
    PhiDrawTextAA(dc, std::to_wstring(i + 1).c_str(), -1, &circle,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    ::SelectObject(dc, text_font);
    RECT rc = {circle.right + Scaled(14), y, card.right - Scaled(kCardPad),
               y + marker};
    PhiDrawTextAA(dc, PhiStr(steps[i]), -1, &rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
    ::SelectObject(dc, old_font);
    if (i + 1 < steps.size()) {
      RECT connector = {circle.left + marker / 2, circle.bottom + Scaled(6),
                        circle.left + marker / 2 + std::max(1, Scaled(1)),
                        circle.bottom + gap + marker - Scaled(6) - Scaled(2)};
      HBRUSH brush = ::CreateSolidBrush(RGB(0x4A, 0x4A, 0x4A));
      ::FillRect(dc, &connector, brush);
      ::DeleteObject(brush);
    }
    y += marker + gap + Scaled(12);
  }
  const int divider_y = card_top + 20 + kGuideHeight + 20;
  RECT divider = ContentDip((kWidth - kCardWidth) / 2 + kCardPad, divider_y,
                            kCardWidth - 2 * kCardPad, 1);
  HBRUSH line = ::CreateSolidBrush(RGB(0x3A, 0x3A, 0x3A));
  ::FillRect(dc, &divider, line);
  ::DeleteObject(line);
  for (const Hit& hit : hits_) {
    if (hit.control == Control::kLegalCheck) {
      PaintCheckRow(dc, hit.bounds, legal_checked_,
                    PhiStr("oobe.nextSteps.legalAgreement"),
                    PhiStr("oobe.nextSteps.privacyLink"),
                    PhiStr("oobe.nextSteps.termsLink"));
    } else if (hit.control == Control::kMetricsCheck) {
      PaintCheckRow(dc, hit.bounds, metrics_checked_,
                    PhiStr("oobe.nextSteps.metricsConsent"), std::wstring(),
                    std::wstring());
    }
  }
}

void PhiOnboardingWindow::PaintSetName(HDC dc, const RECT& content) {
  PaintDarkGround(dc, content);
  PaintTitle(dc, content, PhiStr("oobe.setName.title"), RGB(255, 255, 255),
             kTitleTop);
  const int field_top = kTitleTop + kTitleHeight + 130;
  const RECT field = ContentDip((kWidth - kNameFieldWidth) / 2, field_top,
                                kNameFieldWidth, kNameFieldHeight);
  PhiFillRoundRectAA(dc, field, Scaled(kNameFieldRadius), RGB(0x1A, 0x1A, 0x1A));
  if (!name_error_.empty()) {
    HGDIOBJ old_font = ::SelectObject(dc, fonts().Get(15, FW_NORMAL));
    ::SetTextColor(dc, kLinkInk);
    RECT rc = ContentDip(40, field_top + kNameFieldHeight + 12, kWidth - 80, 24);
    PhiDrawTextAA(dc, name_error_.c_str(), -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SelectObject(dc, old_font);
  }
}

void PhiOnboardingWindow::PaintWelcome(HDC dc, const RECT& content) {
  // The shared dark ground, the card (hidden until its artwork is ready)
  // and the dice. No title, no swatches: the card IS the page.
  PaintDarkGround(dc, content);
  card_.Paint(dc, CardRect());
  StartCardTimer();
  for (size_t i = 0; i < hits_.size(); ++i) {
    const Hit& hit = hits_[i];
    if (hit.control == Control::kDice) {
      PaintDice(dc, hit.bounds, hovered() == static_cast<int>(i));
    }
  }
}

void PhiOnboardingWindow::PaintDice(HDC dc, const RECT& bounds, bool hot) {
  // The Mac's randomButton: a 40-dip circle of white at 10%, the dice
  // glyph in white (stroked 1.5, round joins), lifted a little under the
  // pointer.
  const int w = bounds.right - bounds.left;
  const int h = bounds.bottom - bounds.top;
  if (w <= 0 || h <= 0 || !PhiEnsureGdiPlus()) {
    return;
  }
  std::vector<uint8_t> layer(static_cast<size_t>(w) * h * 4, 0);
  {
    Gdiplus::Bitmap bitmap(w, h, w * 4, PixelFormat32bppPARGB, layer.data());
    Gdiplus::Graphics g(&bitmap);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));
    Gdiplus::SolidBrush ground(Gdiplus::Color(hot ? 46 : 26, 255, 255, 255));
    g.FillEllipse(&ground, 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
    const float scale = w / static_cast<float>(kDiceSize);
    g.TranslateTransform((w - kDiceIconW * scale) / 2, (h - kDiceIconH * scale) / 2);
    g.ScaleTransform(scale, scale);
    Gdiplus::GraphicsPath path;
    AddDiceSegs(&path, kDiceFront);
    AddDiceSegs(&path, kDiceBack);
    Gdiplus::Pen pen(Gdiplus::Color(255, 255, 255, 255), 1.5f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    g.DrawPath(&pen, &path);
    Gdiplus::SolidBrush pip(Gdiplus::Color(255, 255, 255, 255));
    for (const auto& [cx, cy] : kDicePips) {
      g.FillEllipse(&pip, cx - 1.0f, cy - 1.0f, 2.0f, 2.0f);
    }
    g.Flush(Gdiplus::FlushIntentionSync);
  }
  PhiBlendBgra(dc, bounds, layer.data(), w, h);
}

void PhiOnboardingWindow::PaintLayout(HDC dc, const RECT& content) {
  PaintDarkGround(dc, content);
  PaintTitle(dc, content, PhiStr("oobe.layoutSelection.title"),
             RGB(255, 255, 255), kTitleTop);
  const int container_h = 8 + 3 * kLayoutRowHeight + 2 * 8 + 8;
  const RECT container =
      ContentDip(84, (kHeight - container_h) / 2, kCardWidth, container_h);
  PhiFillRoundRectAA(dc, container, Scaled(kCardRadius), RGB(0x2A, 0x2A, 0x2A));
  for (size_t i = 0; i < hits_.size(); ++i) {
    const Hit& hit = hits_[i];
    const bool hot = hovered() == static_cast<int>(i);
    if (hit.control == Control::kLayoutRow) {
      const bool selected = layout_choice_ == hit.key;
      if (selected || hot) {
        PhiFillRoundRectAA(dc, hit.bounds, Scaled(8),
                           selected ? RGB(0x3A, 0x3A, 0x3A) : RGB(0x32, 0x32, 0x32));
      }
      const wchar_t* file =
          hit.key == static_cast<int>(PhiLayoutMode::kBalanced)
              ? L"layout-balanced.png"
          : hit.key == static_cast<int>(PhiLayoutMode::kPerformance)
              ? L"layout-performance.png"
              : L"layout-comfortable.png";
      const char* key =
          hit.key == static_cast<int>(PhiLayoutMode::kComfortable)
              ? "appMenu.horizontalTabs"
              : "appMenu.verticalTabs";
      HGDIOBJ old_font = ::SelectObject(dc, fonts().Get(18, FW_NORMAL));
      ::SetTextColor(dc, RGB(255, 255, 255));
      RECT label = {hit.bounds.left + Scaled(18), hit.bounds.top,
                    hit.bounds.right - Scaled(220), hit.bounds.bottom};
      PhiDrawTextAA(dc, PhiStr(key), -1, &label,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      ::SelectObject(dc, old_font);
      const int check = Scaled(kCheckSize);
      RECT check_rc = {hit.bounds.right - Scaled(18) - check,
                       (hit.bounds.top + hit.bounds.bottom - check) / 2,
                       hit.bounds.right - Scaled(18),
                       (hit.bounds.top + hit.bounds.bottom + check) / 2};
      PaintImage(dc, check_rc,
                 ResourceImage(selected ? L"check.png" : L"uncheck.png", check, check),
                 true);
      const int pw = Scaled(kLayoutPreviewWidth), ph = Scaled(kLayoutPreviewHeight);
      RECT preview = {check_rc.left - Scaled(16) - pw,
                      (hit.bounds.top + hit.bounds.bottom - ph) / 2,
                      check_rc.left - Scaled(16),
                      (hit.bounds.top + hit.bounds.bottom + ph) / 2};
      PaintImage(dc, preview, ResourceImage(file, pw, ph), false);
      PhiStrokeRoundRectAA(dc, preview, Scaled(6), RGB(0x4A, 0x4A, 0x4A),
                           std::max(1, Scaled(1)));
    }
  }
}

// --- Input ------------------------------------------------------------------

void PhiOnboardingWindow::OnHover(int hit, int) {
  if (hit < 0 && page_ == Page::kWelcome) {
    // The pointer left the window: the card settles flat.
    card_.SetPointer(std::nullopt, CardRect());
    StartCardTimer();
  }
  Invalidate();
}

void PhiOnboardingWindow::OnWheel(int delta, POINT) {
  if (MaxScroll() > 0) {
    ScrollTo(scroll_ - delta * Scaled(kScrollStep) / WHEEL_DELTA);
  }
}

bool PhiOnboardingWindow::OnKey(WPARAM virtual_key) {
  // A split page scrolls from the keyboard too; the footer's button stays
  // reachable with Enter regardless.
  const int max = MaxScroll();
  if (max > 0) {
    const int stride = std::max(
        Scaled(kScrollStep),
        static_cast<int>(FooterSplit() - ContentRect().top -
                         Scaled(kScrollStep)));
    switch (virtual_key) {
      case VK_DOWN:
        ScrollTo(scroll_ + Scaled(kScrollStep));
        return true;
      case VK_UP:
        ScrollTo(scroll_ - Scaled(kScrollStep));
        return true;
      case VK_NEXT:
        ScrollTo(scroll_ + stride);
        return true;
      case VK_PRIOR:
        ScrollTo(scroll_ - stride);
        return true;
      case VK_END:
        ScrollTo(max);
        return true;
      case VK_HOME:
        ScrollTo(0);
        return true;
      default:
        break;
    }
  }
  return PhiSecondaryWindow::OnKey(virtual_key);
}

void PhiOnboardingWindow::OnMouseDown(int hit, POINT) {
  pressed_ = hit;
}

void PhiOnboardingWindow::OnClick(int hit, POINT) {
  const int pressed = pressed_;
  pressed_ = -1;
  if (hit < 0 || hit != pressed) {
    return;
  }
  const Hit* h = HitOf(hit);
  if (!h || !h->enabled) {
    return;
  }
  const Hit copy = *h;
  Activate(copy);
}

void PhiOnboardingWindow::OnCaptureLost() {}

void PhiOnboardingWindow::Activate(const Hit& hit) {
  switch (hit.control) {
    case Control::kSignIn:
      NoteSignInPressed();
      ShowPage(Page::kSigningIn);
      host_->OnboardingSignIn();
      break;
    case Control::kContinueAsGuest:
      AdvanceTo(Page::kGuestPrivacy);
      break;
    case Control::kCancelSignIn:
      host_->OnboardingCancelSignIn();
      if (standalone_sign_in_) {
        host_->OnboardingClosed();  // Dismissed; the window may be gone now.
        break;
      }
      ShowPage(Page::kLogin);
      break;
    case Control::kLegalCheck:
      legal_checked_ = !legal_checked_;
      Layout();
      Invalidate();
      break;
    case Control::kMetricsCheck:
      metrics_checked_ = !metrics_checked_;
      Layout();
      Invalidate();
      break;
    case Control::kPrivacyLink:
      host_->OnboardingOpenLink(kPrivacyUrl);
      break;
    case Control::kTermsLink:
      host_->OnboardingOpenLink(kTermsUrl);
      break;
    case Control::kDice:
      RandomizeCard();
      break;
    case Control::kLayoutRow:
      layout_choice_ = hit.key;
      Layout();
      Invalidate();
      break;
    case Control::kNext:
      OnEnter();
      break;
    case Control::kCard:
    case Control::kNone:
      break;
  }
}

void PhiOnboardingWindow::OnEnter() {
  switch (page_) {
    case Page::kLogin:
      if (!account_->AccountSigningIn()) {
        NoteSignInPressed();
        ShowPage(Page::kSigningIn);
        host_->OnboardingSignIn();
      }
      break;
    case Page::kGuestPrivacy:
      if (legal_checked_) {
        // The Mac's completeGuestOOBE: two steps, then the crossing.
        oobe_.Finish(/*is_guest=*/true, 2, Uptime());
        host_->CaptureAnalytics(kAnalyticsModuleAccount,
                                kEventGuestModeEntered, base::DictValue());
        host_->OnboardingContinueAsGuest(metrics_checked_);
      }
      break;
    case Page::kSetName:
      SubmitName();
      break;
    case Page::kWelcome:
      // The Mac's nextButtonTapped: the card's style is saved for the
      // account before the flow moves on.
      if (!prefs_->SetProfileCardStyle(account_->auth().user().sub, card_style_.ToMap())) {
        PhiNotice(hwnd(), PhiStr("oobe.welcome.cardSaveFailed"), L"");
        return;
      }
      host_->OnboardingSetPhase(3);
      AdvanceTo(Page::kLayout);
      break;
    case Page::kLayout:
      prefs_->RequestLayoutMode(static_cast<PhiLayoutMode>(layout_choice_));
      host_->OnboardingSetPhase(5);
      AdvanceTo(Page::kNextSteps);
      break;
    case Page::kNextSteps:
      if (legal_checked_) {
        oobe_.Finish(/*is_guest=*/false, std::nullopt, Uptime());
        host_->OnboardingFinished(metrics_checked_);
      }
      break;
    case Page::kSigningIn:
      break;
  }
}

void PhiOnboardingWindow::NoteSignInPressed() {
  // The Mac's login_retried: another go after a failed or abandoned
  // attempt (its "Go back and try again"; here the note under the guest
  // link is that state).
  if (!account_->AccountLastError().empty()) {
    host_->CaptureAnalytics(kAnalyticsModuleAccount, kEventLoginRetried,
                            base::DictValue());
  }
}

void PhiOnboardingWindow::OnEscape() {
  if (page_ == Page::kSigningIn) {
    host_->OnboardingCancelSignIn();
    ShowPage(Page::kLogin);
  } else if (page_ == Page::kGuestPrivacy) {
    ShowPage(Page::kLogin);
  }
}

void PhiOnboardingWindow::OnTimerFired(UINT_PTR id) {
  if (id != card_timer_.id()) {
    return;
  }
  if (card_.Tick()) {
    RECT card = CardRect();
    const int margin = PhiHolographicCard::MarginPx(card);
    ::InflateRect(&card, margin, margin);
    Invalidate(card);
  }
  if (!card_.animating()) {
    card_timer_.Stop();
  }
}

void PhiOnboardingWindow::OnDpiChanged(UINT old_dpi) {
  PhiSecondaryWindow::OnDpiChanged(old_dpi);
  images_.clear();
  if (name_field_) {
    ::SendMessageW(name_field_, WM_SETFONT,
                   reinterpret_cast<WPARAM>(fonts().Get(24, FW_NORMAL)), TRUE);
  }
  // The suggested rect scaled the old (possibly cut-down) size: back to
  // the design size at the new DPI, then into this monitor's work area.
  Refit();
}

void PhiOnboardingWindow::OnNcDestroy() {
  card_timer_.Stop();
  DetachBrowser();
  PhiSecondaryWindow::OnNcDestroy();
}

// --- The name field -----------------------------------------------------------

void PhiOnboardingWindow::EnsureNameField() {
  if (name_field_) {
    ::SetWindowPos(name_field_, nullptr, 0, 0, 0, 0,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOMOVE);
  }
  const int field_top = kTitleTop + kTitleHeight + 130;
  const RECT field = ContentDip((kWidth - kNameFieldWidth) / 2 + kNameFieldInset,
                                field_top + 12, kNameFieldWidth - 2 * kNameFieldInset,
                                kNameFieldHeight - 24);
  if (!name_field_) {
    name_field_ = ::CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, field.left,
        field.top, field.right - field.left, field.bottom - field.top, hwnd(),
        reinterpret_cast<HMENU>(kIdNameField), ::GetModuleHandleW(nullptr),
        nullptr);
    if (!name_field_) {
      return;
    }
    ::SetWindowSubclass(
        name_field_,
        [](HWND control, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR,
           DWORD_PTR ref) -> LRESULT {
          auto* self = reinterpret_cast<PhiOnboardingWindow*>(ref);
          if (msg == WM_KEYDOWN && wparam == VK_RETURN) {
            self->SubmitName();
            return 0;
          }
          if (msg == WM_CHAR && (wparam == VK_RETURN || wparam == VK_TAB)) {
            return 0;  // No beep for the keys the window owns.
          }
          return ::DefSubclassProc(control, msg, wparam, lparam);
        },
        kIdNameField, reinterpret_cast<DWORD_PTR>(this));
    ::SendMessageW(name_field_, WM_SETFONT,
                   reinterpret_cast<WPARAM>(fonts().Get(24, FW_NORMAL)), TRUE);
    field_brush_ = ::CreateSolidBrush(RGB(0x1A, 0x1A, 0x1A));
  } else {
    ::SetWindowPos(name_field_, nullptr, field.left, field.top,
                   field.right - field.left, field.bottom - field.top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

void PhiOnboardingWindow::SubmitName() {
  if (page_ != Page::kSetName || name_submitting_ || !name_field_) {
    return;
  }
  std::wstring text(1024, L'\0');
  const int n = ::GetWindowTextW(name_field_, text.data(),
                                 static_cast<int>(text.size()));
  text.resize(static_cast<size_t>(std::max(0, n)));
  const std::wstring name = Trim(text);
  if (name.empty()) {
    name_error_ = PhiStr("oobe.setName.promptForEmpty");
    Invalidate();
    return;
  }
  if (CodePoints(name) > static_cast<size_t>(PhiAccountProfileService::kMaxNameLength)) {
    name_error_ = PhiStr("oobe.setName.promptForTooLong");
    Invalidate();
    return;
  }
  name_error_.clear();
  const std::string utf8 = base::WideToUTF8(name);
  if (utf8 == original_name_) {
    // Unchanged: no round trip, the Mac's shortcut.
    host_->OnboardingSetPhase(2);
    AdvanceTo(Page::kWelcome);
    return;
  }
  name_submitting_ = true;
  account_->RequestRenameAccount(utf8);
  Layout();
  Invalidate();
}

// --- The embedded browser -----------------------------------------------------

void PhiOnboardingWindow::AttachBrowser(HWND browser_hwnd, BrowserId browser) {
  browser_hwnd_ = browser_hwnd;
  browser_ = browser;
  // The content host's recipe (phi_browser_host.cc AdoptContentChild): strip
  // the top-level styles, make it a clipped child, and force the frame
  // recalculation that retires the old non-client band.
  LONG_PTR style = ::GetWindowLongPtrW(browser_hwnd, GWL_STYLE);
  style &= ~(WS_POPUP | WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_THICKFRAME |
             WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
  style |= WS_CHILD | WS_CLIPSIBLINGS;
  ::SetWindowLongPtrW(browser_hwnd, GWL_STYLE, style);
  LONG_PTR ex = ::GetWindowLongPtrW(browser_hwnd, GWL_EXSTYLE);
  ex &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME |
          WS_EX_STATICEDGE | WS_EX_APPWINDOW);
  ::SetWindowLongPtrW(browser_hwnd, GWL_EXSTYLE, ex);
  ::SetParent(browser_hwnd, hwnd());
  ::SetWindowPos(browser_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
  PlaceEmbeddedBrowser();
  if (!pending_url_.empty()) {
    ::PostMessageW(hwnd(), kMsgNavigate, 0, 0);
  }
}

void PhiOnboardingWindow::DetachBrowser() {
  if (browser_hwnd_) {
    ::SetParent(browser_hwnd_, nullptr);
    browser_hwnd_ = nullptr;
  }
  browser_ = BrowserId();
}

void PhiOnboardingWindow::NavigateEmbedded(const std::string& url) {
  pending_url_ = url;
  if (browser_hwnd_) {
    ::PostMessageW(hwnd(), kMsgNavigate, 0, 0);
  }
}

void PhiOnboardingWindow::FocusEmbeddedBrowser() {
  if (page_ != Page::kSigningIn || !browser_hwnd_ || !browser_.valid()) {
    return;
  }
  if (::IsWindow(browser_hwnd_)) {
    ::SetFocus(browser_hwnd_);
  }
  host_->OnboardingFocusBrowser(browser_);
}

void PhiOnboardingWindow::PlaceEmbeddedBrowser() {
  if (!browser_hwnd_) {
    return;
  }
  const RECT content = ContentRect();
  if (page_ == Page::kSigningIn) {
    ::SetWindowPos(browser_hwnd_, HWND_TOP, content.left, content.top,
                   content.right - content.left, content.bottom - content.top,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    FocusEmbeddedBrowser();
  } else {
    // Parked outside the client rect (a hide would lose to Chromium's own
    // show right after creation).
    ::SetWindowPos(browser_hwnd_, nullptr, -20000, -20000,
                   content.right - content.left, content.bottom - content.top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

void PhiOnboardingWindow::OnAccountChanged() {
  if (!account_) {
    return;
  }
  if (page_ == Page::kSigningIn) {
    if (account_->AccountSigningIn()) {
      return;
    }
    if (account_->auth().signed_in() && !account_->AccountReauthRequired()) {
      const std::string sub = account_->auth().user().sub;
      int phase = prefs_->OnboardingPhase(sub);
      if (phase == 0) {
        phase = 1;  // A brand-new account starts at Set Name.
        host_->OnboardingSetPhase(phase);
      }
      if (phase >= 6) {
        // A finished account: the login step was the whole funnel.
        oobe_.Finish(/*is_guest=*/false, std::nullopt, Uptime());
        host_->OnboardingFinished(prefs_->AnalyticsEnabled());
        return;
      }
      AdvanceTo(PageForPhase(phase));
      return;
    }
    if (standalone_sign_in_) {
      host_->OnboardingClosed();  // Refused: the Settings pane shows the note.
      return;
    }
    ShowPage(Page::kLogin);  // Refused or abandoned: the note shows there.
    return;
  }
  if (page_ == Page::kSetName && name_submitting_ && !account_->AccountBusy()) {
    name_submitting_ = false;
    const std::string error = account_->AccountLastError();
    if (!error.empty()) {
      name_error_ = PhiStr(error == "update" ? "settings.account.error.update"
                                             : "settings.account.error.profile");
      Layout();
      Invalidate();
      return;
    }
    host_->OnboardingSetPhase(2);
    AdvanceTo(Page::kWelcome);
  }
}

LRESULT PhiOnboardingWindow::OnWindowMessage(UINT msg, WPARAM wparam,
                                             LPARAM lparam, bool* handled) {
  switch (msg) {
    case WM_CLOSE:
      *handled = true;
      // The user's own close before success (the Mac's willClose
      // interruption); a close after success already ended the session.
      oobe_.Interrupt(PhiOobeAnalyticsSession::Interruption::kWindowClosed,
                      Uptime());
      host_->OnboardingClosed();
      return 0;
    case WM_CTLCOLOREDIT: {
      HDC dc = reinterpret_cast<HDC>(wparam);
      ::SetTextColor(dc, RGB(255, 255, 255));
      ::SetBkColor(dc, RGB(0x1A, 0x1A, 0x1A));
      *handled = true;
      return reinterpret_cast<LRESULT>(field_brush_);
    }
    case WM_MOUSEMOVE:
      if (page_ == Page::kWelcome) {
        // The pointer over the card moves its light (the Mac's tracking
        // area); off it, the card settles. The base keeps the hover.
        const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const RECT card = CardRect();
        card_.SetPointer(::PtInRect(&card, pt) ? std::optional<POINT>(pt) : std::nullopt,
                         card);
        StartCardTimer();
      }
      break;
    case kMsgNavigate:
      if (browser_.valid() && navigate_ && !pending_url_.empty()) {
        navigate_(browser_, pending_url_);
        pending_url_.clear();
        FocusEmbeddedBrowser();
      }
      *handled = true;
      return 0;
    case WM_ACTIVATE:
      // Our top-level got the activation the child never will: pass the
      // keyboard on to the hosted login.
      if (LOWORD(wparam) != WA_INACTIVE) {
        ::PostMessageW(hwnd(), kMsgNavigate + 2, 0, 0);
      }
      break;
    case WM_SETFOCUS:
      if (page_ == Page::kSigningIn && browser_hwnd_) {
        FocusEmbeddedBrowser();
        *handled = true;
        return 0;
      }
      break;
    case kMsgNavigate + 2:
      FocusEmbeddedBrowser();
      *handled = true;
      return 0;
    case WM_DISPLAYCHANGE:
      Refit();  // A resolution or scaling change: fit the work area again.
      break;
    case WM_SETTINGCHANGE:
      if (wparam == SPI_SETWORKAREA) {
        Refit();  // The taskbar moved or grew.
      }
      break;
    case kMsgNavigate + 1:  // The card's artwork landed.
      Invalidate();
      StartCardTimer();
      *handled = true;
      return 0;
    default:
      break;
  }
  return PhiSecondaryWindow::OnWindowMessage(msg, wparam, lparam, handled);
}

}  // namespace phi_client

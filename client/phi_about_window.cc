// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_about_window.h"

#include <windowsx.h>

#include <algorithm>
#include <utility>

#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "client/phi_dwrite.h"
#include "client/phi_feedback_outbox.h"
#include "client/phi_i18n.h"
#include "client/phi_icons.h"
#include "client/phi_version.h"

namespace phi_client {

namespace {

constexpr wchar_t kClassName[] = L"PhiAboutWindow";

// AboutView.contentSize and its stack, in design units (Mac points).
constexpr int kWidthDip = 290;
constexpr int kHeightDip = 392;
constexpr int kSidePadding = 20;
constexpr int kTopSpacerMin = 28;
constexpr int kBottomSpacerMin = 42;
constexpr int kMarkBoxWidth = 112;   // The conch's frame.
constexpr int kMarkBoxHeight = 120;
constexpr int kMarkSize = 96;        // The app mark inside it.
constexpr int kMarkBottomPadding = 10;
constexpr int kStackSpacing = 18;    // VStack(spacing: 18).
constexpr int kVersionSpacing = 2;   // VStack(spacing: 2).
constexpr int kNoticeSpacing = 8;    // VStack(spacing: 8).
constexpr int kCopyrightTopPadding = 5;
constexpr int kAcknowledgementsWidth = 250;
constexpr int kTitleSize = 17;       // .title2, bold.
constexpr int kVersionSize = 10;     // .system(size: 10).
constexpr int kCaptionSize = 10;     // .caption.
constexpr int kSmallSize = 11;       // NSFont.smallSystemFontSize.
constexpr int kLinkKeyBase = 1000;   // Hit keys of the link fragments.

// AboutView's three link targets. The Mac's third, the browser's own
// third-party credits, is a credits.html in its bundle; Windows ships none,
// so it is the client's third_party folder (WinSparkle, zlib) on GitHub.
constexpr char kChromiumProjectUrl[] = "https://www.chromium.org/Home/";
constexpr char kChromiumSourceUrl[] = "chrome://credits";
constexpr char kOtherOpenSourceUrl[] =
    "https://github.com/phibrowser/phibrowser-win/tree/main/third_party";

PhiAboutWindow* g_window = nullptr;

// A line may break after a space and on either side of an ideograph (the
// sentence has no spaces in Chinese or Japanese).
bool IsWide(wchar_t c) {
  return c >= 0x2E80;
}

// Punctuation a line may not start with (the CJK line-breaking rule, and
// the Latin marks that follow a word): it stays on the line before, even a
// little past the column.
bool StartsWithCloser(const std::wstring& piece) {
  return !piece.empty() &&
         std::wstring(L".,;:!?)]}\x3001\x3002\xFF0C\xFF1A\xFF1B\xFF01"
                      L"\xFF1F\xFF09\x300D\x300F\x3011\x3009\x300B"
                      L"\x201D\x2019")
                 .find(piece.front()) != std::wstring::npos;
}

// `text` cut at its break opportunities, each piece keeping its trailing
// space.
std::vector<std::wstring> BreakPieces(const std::wstring& text) {
  std::vector<std::wstring> pieces;
  std::wstring current;
  for (size_t i = 0; i < text.size(); ++i) {
    const wchar_t c = text[i];
    if (IsWide(c) && !current.empty()) {
      pieces.push_back(current);
      current.clear();
    }
    current += c;
    if (c == L' ' || IsWide(c)) {
      pieces.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) {
    pieces.push_back(current);
  }
  return pieces;
}

}  // namespace

// static
void PhiAboutWindow::Open(HWND anchor,
                          std::function<void(const std::string&)> open_url) {
  if (!g_window || !g_window->alive()) {
    auto* window = new PhiAboutWindow(std::move(open_url));
    if (!window->OpenWindow(anchor)) {
      delete window;
      return;
    }
    // Self-owned from here; OnNcDestroy clears g_window first.
    window->DeleteWhenDestroyed();
    g_window = window;
  }
  ::ShowWindow(g_window->hwnd(),
               ::IsIconic(g_window->hwnd()) ? SW_RESTORE : SW_SHOW);
  ::SetForegroundWindow(g_window->hwnd());
}

PhiAboutWindow::PhiAboutWindow(
    std::function<void(const std::string&)> open_url)
    : open_url_(std::move(open_url)) {}

PhiAboutWindow::~PhiAboutWindow() = default;

bool PhiAboutWindow::OpenWindow(HWND anchor) {
  ClassSpec spec;
  spec.name = kClassName;
  Options options;
  // Titled and closable only, as the Mac's window: no mark, no title on
  // the band (the window text still names it for the taskbar and readers).
  options.caption_buttons = kPhiCaptionClose;
  options.show_mark = false;
  options.dialog_keys = kPhiDialogNone;
  const RECT bounds = {CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT + 100,
                       CW_USEDEFAULT + 100};
  if (!CreateSecondary(spec, 0,
                       WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, bounds,
                       nullptr, PhiStr("appMenu.about"), options)) {
    LOG(ERROR) << "[About] CreateWindowEx failed err=" << ::GetLastError();
    return false;
  }
  Layout();
  CenterOn(anchor && ::IsWindow(anchor) ? anchor : nullptr);
  return true;
}

HFONT PhiAboutWindow::FontFor(const Run& run) {
  return fonts().Get(run.size, run.weight);
}

void PhiAboutWindow::Layout() {
  if (!alive()) {
    return;
  }
  runs_.clear();
  links_.clear();
  const int width = Scaled(kWidthDip);
  const int center_x = width / 2;
  const int text_width = width - 2 * Scaled(kSidePadding);

  HDC dc = ::GetDC(hwnd());
  const auto measure = [&](Run* run, int wrap_width) {
    HGDIOBJ old = ::SelectObject(dc, FontFor(*run));
    RECT r = {0, 0, wrap_width, 0};
    PhiDrawTextAA(dc, run->text.c_str(), -1, &r,
                  DT_CALCRECT | DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    ::SelectObject(dc, old);
    return r;
  };
  // A centred block of `run` from `*y` down.
  const auto place = [&](Run run, int* y) {
    const RECT r = measure(&run, text_width);
    const int w = r.right - r.left;
    run.bounds = {center_x - w / 2, *y, center_x - w / 2 + w,
                  *y + (r.bottom - r.top)};
    *y = run.bounds.bottom;
    runs_.push_back(std::move(run));
  };

  // The stack, measured from 0; the spacers share what the 392 leaves.
  int y = 0;
  mark_ = {center_x - Scaled(kMarkSize) / 2,
           y + (Scaled(kMarkBoxHeight) - Scaled(kMarkSize)) / 2,
           center_x - Scaled(kMarkSize) / 2 + Scaled(kMarkSize), 0};
  mark_.bottom = mark_.top + Scaled(kMarkSize);
  y += Scaled(kMarkBoxHeight) + Scaled(kMarkBottomPadding) +
       Scaled(kStackSpacing);
  place({PhiStr("app.phiBrowser"), {}, kTitleSize, FW_BOLD}, &y);
  y += Scaled(kStackSpacing);
  place({PhiStrF("about.versionLabel",
                 {PHI_WIN_VERSION_WIDE,
                  PHI_WIN_VERSION_WIDEN(
                      PHI_WIN_VERSION_STRINGIFY(PHI_WIN_VERSION_BUILD))}),
         {}, kVersionSize},
        &y);
  const std::string chromium = PhiChromiumVersion();
  y += Scaled(kVersionSpacing);
  place({PhiStrF("about.chromiumVersionLabel",
                 {chromium.empty() ? std::wstring(L"Unknown")
                                   : base::UTF8ToWide(chromium)}),
         {}, kVersionSize},
        &y);
  y += Scaled(kStackSpacing) + Scaled(kCopyrightTopPadding);
  SYSTEMTIME now = {};
  ::GetLocalTime(&now);
  place({PhiStrF("about.copyrightNotice", {base::NumberToWString(now.wYear)}),
         {}, kCaptionSize},
        &y);
  y += Scaled(kNoticeSpacing);
  LayoutAcknowledgements(center_x, Scaled(kAcknowledgementsWidth), &y);
  ::ReleaseDC(hwnd(), dc);

  const int stack = y;
  const int spare = std::max(
      0, Scaled(kHeightDip) - Scaled(kTopSpacerMin) -
             Scaled(kBottomSpacerMin) - stack);
  const int top = CaptionHeight() + Scaled(kTopSpacerMin) + spare / 2;
  const int content =
      std::max(Scaled(kHeightDip),
               Scaled(kTopSpacerMin) + stack + Scaled(kBottomSpacerMin));
  ::OffsetRect(&mark_, 0, top);
  for (Run& run : runs_) {
    ::OffsetRect(&run.bounds, 0, top);
  }
  const int content_dip =
      static_cast<int>((content * 96 + dpi() - 1) / dpi());
  const SIZE outer = chrome().OuterSize(kWidthDip, content_dip);
  ::SetWindowPos(hwnd(), nullptr, 0, 0, outer.cx, outer.cy,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

  // Every line is a text hit (readers, and the suites, read it by name);
  // each link fragment a button that opens its page.
  ClearHits();
  for (size_t i = 0; i < runs_.size(); ++i) {
    const Run& run = runs_[i];
    if (run.link >= 0) {
      AddHit(run.bounds, kLinkKeyBase + run.link, links_[run.link].title,
             PhiHitRole::kButton);
    } else {
      AddHit(run.bounds, static_cast<int>(i), run.text, PhiHitRole::kText);
    }
  }
  ::InvalidateRect(hwnd(), nullptr, FALSE);
}

void PhiAboutWindow::LayoutAcknowledgements(int center_x,
                                            int width,
                                            int* y) {
  // "…the {0} open source project, Chromium's {1}, as well as other {2}.":
  // the placeholders are the three links, in order.
  links_ = {
      {PhiStr("about.acknowledgements.chromiumProjectLink"),
       kChromiumProjectUrl},
      {PhiStr("about.acknowledgements.chromiumSourceLink"), kChromiumSourceUrl},
      {PhiStr("about.acknowledgements.otherOpenSourceLink"),
       kOtherOpenSourceUrl},
  };
  struct Span {
    std::wstring text;
    int link;
  };
  std::vector<Span> spans;
  const std::wstring format = PhiStr("about.acknowledgements.text");
  size_t at = 0;
  while (at < format.size()) {
    const size_t open = format.find(L'{', at);
    if (open == std::wstring::npos) {
      spans.push_back({format.substr(at), -1});
      break;
    }
    const size_t close = format.find(L'}', open);
    int index = -1;
    if (close != std::wstring::npos && close == open + 2 &&
        format[open + 1] >= L'0' && format[open + 1] <= L'2') {
      index = format[open + 1] - L'0';
    }
    if (index < 0) {
      spans.push_back({format.substr(at, open + 1 - at), -1});
      at = open + 1;
      continue;
    }
    if (open > at) {
      spans.push_back({format.substr(at, open - at), -1});
    }
    spans.push_back({links_[index].title, index});
    at = close + 1;
  }

  // Greedy lines of pieces; a piece keeps its span's link.
  HDC dc = ::GetDC(hwnd());
  HGDIOBJ old = ::SelectObject(dc, fonts().Get(kSmallSize, FW_NORMAL));
  TEXTMETRICW metrics = {};
  ::GetTextMetricsW(dc, &metrics);
  const int line_height = metrics.tmHeight;
  const auto extent = [&](const std::wstring& text) {
    SIZE size = {};
    ::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()),
                            &size);
    return static_cast<int>(size.cx);
  };
  struct Piece {
    std::wstring text;
    int link;
  };
  std::vector<std::vector<Piece>> lines(1);
  int line_width = 0;
  for (const Span& span : spans) {
    for (const std::wstring& piece : BreakPieces(span.text)) {
      std::wstring trimmed = piece;
      while (!trimmed.empty() && trimmed.back() == L' ') {
        trimmed.pop_back();
      }
      if (line_width > 0 && line_width + extent(trimmed) > width &&
          !StartsWithCloser(trimmed)) {
        lines.emplace_back();
        line_width = 0;
      }
      std::vector<Piece>& line = lines.back();
      if (!line.empty() && line.back().link == span.link) {
        line.back().text += piece;  // One fragment per run of a kind.
      } else {
        line.push_back({piece, span.link});
      }
      line_width += extent(piece);
    }
  }
  for (std::vector<Piece>& line : lines) {
    // A line's trailing space is not part of its width.
    while (!line.empty() && !line.back().text.empty() &&
           line.back().text.back() == L' ') {
      line.back().text.pop_back();
      if (line.back().text.empty()) {
        line.pop_back();
      }
    }
    int total = 0;
    for (const Piece& piece : line) {
      total += extent(piece.text);
    }
    int x = center_x - total / 2;
    for (const Piece& piece : line) {
      const int w = extent(piece.text);
      Run run;
      run.text = piece.text;
      run.size = kSmallSize;
      run.link = piece.link;
      run.bounds = {x, *y, x + w, *y + line_height};
      runs_.push_back(std::move(run));
      x += w;
    }
    *y += line_height;
  }
  ::SelectObject(dc, old);
  ::ReleaseDC(hwnd(), dc);
}

void PhiAboutWindow::Paint(HDC dc, const RECT&) {
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  HBRUSH ground = ::CreateSolidBrush(colors().bg);
  ::FillRect(dc, &client, ground);
  ::DeleteObject(ground);
  PaintBand(dc, L"");  // The band carries the close button alone.

  const int px = mark_.right - mark_.left;
  if (HICON mark = PhiAppIcon(px)) {
    ::DrawIconEx(dc, mark_.left, mark_.top, mark, px, px, 0, nullptr,
                 DI_NORMAL);
  }
  ::SetBkMode(dc, TRANSPARENT);
  for (const Run& run : runs_) {
    HGDIOBJ old = ::SelectObject(dc, FontFor(run));
    const COLORREF ink = run.link >= 0
                             ? (run.link == hovered_link_
                                    ? colors().accent_hover
                                    : colors().accent)
                             : colors().label;
    ::SetTextColor(dc, ink);
    RECT r = run.bounds;
    if (run.link >= 0) {
      PhiDrawTextAA(dc, run.text.c_str(), -1, &r,
                    DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
      // Links are underlined, as the Mac's linkTextAttributes.
      TEXTMETRICW metrics = {};
      ::GetTextMetricsW(dc, &metrics);
      const int base = run.bounds.top + metrics.tmAscent + Scaled(1);
      RECT line = {run.bounds.left, base, run.bounds.right,
                   base + std::max(1, Scaled(1))};
      HBRUSH brush = ::CreateSolidBrush(ink);
      ::FillRect(dc, &line, brush);
      ::DeleteObject(brush);
    } else {
      PhiDrawTextAA(dc, run.text.c_str(), -1, &r,
                    DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    }
    ::SelectObject(dc, old);
  }
}

LRESULT PhiAboutWindow::OnWindowMessage(UINT msg,
                                        WPARAM wparam,
                                        LPARAM lparam,
                                        bool* handled) {
  switch (msg) {
    case WM_SETCURSOR:
      // The house rule (phi_popup_chrome.cc): a hand for links.
      if (LOWORD(lparam) == HTCLIENT && hovered_link_ >= 0) {
        ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
        *handled = true;
        return TRUE;
      }
      break;
    case WM_CLOSE:
      Destroy();
      *handled = true;
      return 0;
    default:
      break;
  }
  return PhiSecondaryWindow::OnWindowMessage(msg, wparam, lparam, handled);
}

void PhiAboutWindow::OnHover(int hit, int) {
  const int key = hit >= 0 && hit < static_cast<int>(hits().size())
                      ? hits()[hit].key
                      : -1;
  const int link = key >= kLinkKeyBase ? key - kLinkKeyBase : -1;
  if (link != hovered_link_) {
    hovered_link_ = link;
    ::InvalidateRect(hwnd(), nullptr, FALSE);
  }
}

void PhiAboutWindow::OnClick(int hit, POINT) {
  if (hit < 0 || hit >= static_cast<int>(hits().size())) {
    return;
  }
  const int key = hits()[hit].key;
  if (key < kLinkKeyBase ||
      key - kLinkKeyBase >= static_cast<int>(links_.size()) || !open_url_) {
    return;
  }
  // Copies: opening a tab may activate a browser window over this one.
  const std::string url = links_[key - kLinkKeyBase].url;
  const std::function<void(const std::string&)> open_url = open_url_;
  open_url(url);
}

void PhiAboutWindow::OnEscape() {
  Destroy();
}

void PhiAboutWindow::OnDpiChanged(UINT old_dpi) {
  PhiSecondaryWindow::OnDpiChanged(old_dpi);
  Layout();
}

void PhiAboutWindow::OnColorsChanged() {
  if (alive()) {
    ::InvalidateRect(hwnd(), nullptr, FALSE);
  }
}

void PhiAboutWindow::OnNcDestroy() {
  if (g_window == this) {
    g_window = nullptr;
  }
  PhiSecondaryWindow::OnNcDestroy();
}

}  // namespace phi_client

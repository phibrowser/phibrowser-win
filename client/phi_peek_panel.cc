// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_peek_panel.h"

#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_fluent.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_coordinator.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_shell_window.h"
#include "phibrowser-win/client/phi_window_model.h"

namespace phi_client {

namespace {

constexpr wchar_t kPeekClassName[] = L"PhiPeekPanel";
constexpr wchar_t kPeekShadowClassName[] = L"PhiPeekShadow";

// The Mac's PeekPanelController metrics, in DIP.
// §5.7: the sheet is inset 40 horizontally (24 under 900 dip of content)
// and 24 vertically; a 36 dip header holds three 28 dip buttons.
constexpr int kPaneInsetWide = 40;
constexpr int kPaneInsetNarrow = 24;
constexpr int kNarrowContentDip = 900;
constexpr int kPaneVerticalInset = 24;
constexpr int kHeaderHeight = 36;
constexpr int kHeaderPad = 12;
constexpr int kFaviconSize = 16;
constexpr int kButtonSize = 28;
constexpr int kButtonGap = 4;
constexpr int kButtonRadius = 4;
// The card itself (PeekPanelController): a 12pt rounded sheet under a soft
// drop shadow, so a peek reads as something floating OVER the page rather
// than as part of it.
constexpr int kCardRadius = 8;
// The Mac's card shadow — blur 20, 6pt down, black at 44% (60% on dark, where
// a soft black all but disappears against the page) — and the margin the
// blur needs around the card to fade out inside.
constexpr int kShadowMargin = 24;
constexpr int kShadowBlur = 20;
constexpr int kShadowOffsetY = 6;
constexpr double kShadowAlphaLight = 0.44;
constexpr double kShadowAlphaDark = 0.60;

enum HotControl {
  kHotClose = 0,
  kHotExpand,
  kHotSplit,
};

// Escapes, not literal glyphs: the private-use characters do not survive
// every editor round-trip (these two were silently written out empty once).
constexpr wchar_t kGlyphClose[] = L"\uE711";   // Cancel
constexpr wchar_t kGlyphExpand[] = L"\uE8A7";  // OpenInNewWindow

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) {
    return std::wstring();
  }
  const int size = ::MultiByteToWideChar(
      CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                        static_cast<int>(utf8.size()), out.data(), size);
  return out;
}

std::string Narrow(const std::wstring& wide) {
  if (wide.empty()) {
    return std::string();
  }
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                         static_cast<int>(wide.size()), nullptr,
                                         0, nullptr, nullptr);
  std::string out(static_cast<size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                        static_cast<int>(wide.size()), out.data(), size,
                        nullptr, nullptr);
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The drop shadow: a LAYERED, click-through sibling beneath the card. Its
// pixels are a signed-distance blur of the card's silhouette, so the
// corners fade like the edges instead of stepping.

class PhiPeekShadow : public PhiPanel {
 public:
  PhiPeekShadow() = default;
  ~PhiPeekShadow() override { Destroy(); }

  bool Open(HWND shell) {
    ClassSpec spec;
    spec.name = kPeekShadowClassName;
    spec.cursor = nullptr;
    Options options;
    options.layered = true;
    // A click in the blur is a click on the page around the card, which
    // dismisses the peek.
    options.click_through = true;
    options.ex_style = WS_EX_NOPARENTNOTIFY;
    const RECT nothing = {0, 0, 0, 0};
    return CreatePanel(spec, shell, nothing, options);
  }

  // `radius`, `blur` and `margin` are already in device pixels. A call with
  // the inputs of the last one is free: the committed layer still holds
  // those pixels, and a reposition only moves the window.
  void Render(int width,
              int height,
              int margin,
              double radius,
              double blur,
              double base_alpha) {
    if (!alive() || width <= 0 || height <= 0) {
      return;
    }
    const Inputs inputs = {width, height, margin, radius, blur, base_alpha};
    if (rendered_ && inputs == last_) {
      return;
    }
    PhiLayerSurface layer(width, height);
    if (!layer.valid()) {
      return;
    }
    // The card's silhouette, centred in the surface: everything is expressed
    // as a signed distance to it, which is what makes the corners fade like
    // the edges instead of stepping.
    const double cx = width / 2.0;
    const double cy = height / 2.0;
    const double half_x = (width - 2.0 * margin) / 2.0 - radius;
    const double half_y = (height - 2.0 * margin) / 2.0 - radius;
    // SAFETY: the layer holds exactly width*height 32-bit pixels.
    const base::span<uint32_t> pixels = UNSAFE_BUFFERS(base::span<uint32_t>(
        static_cast<uint32_t*>(layer.bits()),
        static_cast<size_t>(width) * height));
    // Everything deeper inside than half a blur past the corner arcs is at
    // full strength (t clamps to 1 there), so only a band round the edge
    // needs the distance field: a fill, then four strips, instead of a
    // sqrt for every pixel of a card-sized surface.
    std::fill(pixels.begin(), pixels.end(),
              static_cast<uint32_t>(base_alpha * 255.0 + 0.5) << 24);
    const int band = margin + static_cast<int>(std::ceil(radius + blur)) + 2;
    const int band_x = std::min(width, band);
    const int band_y = std::min(height, band);
    for (int y = 0; y < height; ++y) {
      const bool edge_row = y < band_y || y >= height - band_y;
      for (int x = 0; x < width; ++x) {
        if (!edge_row && x == band_x) {
          x = std::max(x, width - band_x);  // Skip the solid middle.
        }
        const double dx = std::abs(x + 0.5 - cx) - half_x;
        const double dy = std::abs(y + 0.5 - cy) - half_y;
        double distance;
        if (dx <= 0.0 && dy <= 0.0) {
          distance = std::max(dx, dy) - radius;  // Well inside: no sqrt.
        } else {
          const double ox = std::max(dx, 0.0);
          const double oy = std::max(dy, 0.0);
          distance = std::sqrt(ox * ox + oy * oy) - radius;
        }
        // Centred on the edge — full strength half a blur inside, nothing
        // half a blur outside, half strength ON it. Ramping from the edge
        // outwards instead leaves a hard band of full black under the
        // offset card.
        const double t = std::clamp(0.5 - distance / blur, 0.0, 1.0);
        const double alpha = base_alpha * t * t * (3.0 - 2.0 * t);
        // Premultiplied black: the colour channels stay 0 at every alpha.
        pixels[static_cast<size_t>(y) * width + x] =
            static_cast<uint32_t>(alpha * 255.0 + 0.5) << 24;
      }
    }
    rendered_ = layer.Commit(hwnd(), nullptr);
    last_ = inputs;
  }

 private:
  struct Inputs {
    int width = 0;
    int height = 0;
    int margin = 0;
    double radius = 0;
    double blur = 0;
    double alpha = 0;
    bool operator==(const Inputs& other) const {
      return width == other.width && height == other.height &&
             margin == other.margin && radius == other.radius &&
             blur == other.blur && alpha == other.alpha;
    }
  };
  Inputs last_;
  bool rendered_ = false;
};

// ---------------------------------------------------------------------------
// The control chips: a SHAPED LAYER, not a region union.
//
// Lane 3 §3.6 finding 3: this window hosted no child and cut itself into
// three rounded rects with CreateRoundRectRgn + CombineRgn, so every chip
// corner was a staircase. The silhouette is now per-pixel alpha out of
// phi_dwrite (PhiPanel::LayerShapes), and WM_NCHITTEST answers HTTRANSPARENT
// off the chips — which is what the region said by simply not being there,
// so a click in the air between two chips still reaches the page and
// dismisses the peek.


PhiPeekPanel::PhiPeekPanel(int64_t window_id,
                           HWND browser_hwnd,
                           int64_t opener_window_id,
                           int64_t opener_tab_id,
                           PhiShellWindow* opener,
                           phi_framework::PhiFramework* framework,
                           const PhiWindowModel* model,
                           PhiCoordinator* coordinator)
    : window_id_(BrowserId{window_id}),
      browser_hwnd_(browser_hwnd),
      opener_window_id_(BrowserId{opener_window_id}),
      opener_tab_id_(opener_tab_id),
      opener_(opener),
      framework_(framework),
      model_(model),
      coordinator_(coordinator),
      opened_tick_(::GetTickCount64()) {
  HWND shell = opener_ ? opener_->shell_hwnd() : nullptr;
  if (!shell) {
    LOG(ERROR) << "[PhiClient] peek has no opener shell";
    return;
  }

  shadow_ = std::make_unique<PhiPeekShadow>();
  shadow_->Open(shell);

  // A child of the opener's shell, above the hosted browser: it clips into the
  // content card and travels with the window, the same trick the bookmark
  // manager uses for its URL-mounted page.
  ClassSpec spec;
  spec.name = kPeekClassName;
  spec.style = CS_HREDRAW | CS_VREDRAW;
  Options options;
  // WS_CLIPCHILDREN for the hosted browser; WS_CLIPSIBLINGS is PhiPanel's
  // and keeps the card's corner caps visible.
  options.style = WS_CLIPCHILDREN;
  const RECT nothing = {0, 0, 0, 0};
  if (!CreatePanel(spec, shell, nothing, options)) {
    LOG(ERROR) << "[PhiClient] peek CreateWindowEx failed err="
               << ::GetLastError();
    return;
  }

  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    LONG_PTR style = ::GetWindowLongPtrW(browser_hwnd_, GWL_STYLE);
    style &= ~(WS_POPUP | WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_THICKFRAME |
               WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_MAXIMIZE |
               WS_MINIMIZE);  // A maximized child covers the header.
    style |= WS_CHILD | WS_CLIPSIBLINGS;
    ::SetWindowLongPtrW(browser_hwnd_, GWL_STYLE, style);
    LONG_PTR ex = ::GetWindowLongPtrW(browser_hwnd_, GWL_EXSTYLE);
    ex &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME |
            WS_EX_STATICEDGE | WS_EX_APPWINDOW);
    ::SetWindowLongPtrW(browser_hwnd_, GWL_EXSTYLE, ex);
    ::SetParent(browser_hwnd_, hwnd());
    ::SetWindowPos(browser_hwnd_, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                       SWP_FRAMECHANGED);
  }

  // The chips are a child of the SHELL, not of the card, and sit right above
  // the card in the shell's card stack (Raise). A child of the card lost to
  // the hosted browser: HWNDMessageHandler::Activate() does SetWindowPos(
  // HWND_TOP) on Chromium's own window — on Show(), and again whenever the
  // page activates itself — which put the browser child above the chips as
  // soon as the peek was up, and no reposition came to lift them again. A
  // sibling of the card is out of that fight: nothing inside the card can
  // reorder above it.
  Reposition();
  RefreshState();
  Show();
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    ::SetFocus(browser_hwnd_);
  }
}

void PhiPeekPanel::Show() {
  // Above the shell's hosted browser child, or the page would occlude both.
  // The shadow goes up first so the card never ends up under its own shadow,
  // the chips last so they stay above the card; the card stack keeps that
  // order once all three are in it.
  if (shadow_) {
    shadow_->ShowPanel();
  }
  if (alive()) {
    ShowPanel();
  }
}

void PhiPeekPanel::Raise() {
  // A z-order refresh for a peek that is UP. One that is down — its opener
  // is not the active tab — stays down: the caps raise (kMsgRaiseCaps) asks
  // every surface of the slot to come up, and used to bring another tab's
  // peek up over the page the user had switched to.
  if (!IsPanelVisible()) {
    return;
  }
  Show();
}

SidebarSurface PhiPeekPanel::kind() const {
  return SidebarSurface::kPeek;
}
int64_t PhiPeekPanel::window_id() const {
  return window_id_.value;
}
int64_t PhiPeekPanel::origin_window_id() const {
  return opener_window_id_.value;
}
int64_t PhiPeekPanel::origin_tab_id() const {
  return opener_tab_id_;
}
HWND PhiPeekPanel::hwnd() const {
  // PhiSurfacePanel's spelling of the base's HWND; the two names meet here.
  return PhiWindow::hwnd();
}

bool PhiPeekPanel::ClickDismisses(POINT screen_point) const {
  const HWND shell = opener_ ? opener_->shell_hwnd() : nullptr;
  if (!shell) {
    return true;
  }
  POINT pt = screen_point;
  ::ScreenToClient(shell, &pt);
  const RECT content = opener_->ContentRect();
  return ::PtInRect(&content, pt) != FALSE;
}

bool PhiPeekPanel::ContainsWindow(HWND window) const {
  if (PhiSurfacePanel::ContainsWindow(window)) {
    return true;
  }
  // The chips are the shell's child, not the card's (see the constructor),
  // so a click on one is the card's own business too.
  return false;
}

PhiPalette PhiPeekPanel::QueryPalette() const {
  if (coordinator_ && opener_) {
    return coordinator_->PaletteForSlot(opener_->slot().value);
  }
  return PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kLight);
}

PhiPeekPanel::~PhiPeekPanel() {
  // Out from under the card before the card goes, hidden first — never a
  // visible page at the desktop's top-left while Chromium closes it.
  PhiReleaseHostedChild(browser_hwnd_);
  // The chips first (they are this window's child), then the shadow, then
  // the card. Each destructor destroys its own HWND and the base clears
  // GWLP_USERDATA at WM_NCDESTROY, so no teardown message can reach a
  // half-destroyed object.
  shadow_.reset();
  Destroy();
}

void PhiPeekPanel::Reposition() {
  if (!alive() || !opener_) {
    return;
  }
  SyncDpiFromParent();
  const RECT content = opener_->ContentRect();
  const int width = content.right - content.left;
  const int height = content.bottom - content.top;
  if (width <= 0 || height <= 0) {
    return;
  }
  // The sheet sits centred in the page card (§5.7): 40 dip in from the
  // sides (24 in a narrow window), 24 top and bottom.
  const int inset_x =
      Scaled(width < Scaled(kNarrowContentDip) ? kPaneInsetNarrow
                                               : kPaneInsetWide);
  const int inset_y = Scaled(kPaneVerticalInset);
  const RECT bounds = {content.left + inset_x, content.top + inset_y,
                       content.left + inset_x + std::max(0, width - inset_x * 2),
                       content.top + inset_y +
                           std::max(0, height - inset_y * 2)};
  SetPanelBounds(bounds);
  ApplyCardShape();
  PositionContent();
  LayoutHits();
  UpdateShadow();
  Invalidate();
}

void PhiPeekPanel::ApplyCardShape() {
  if (!alive()) {
    return;
  }
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  if (client.right <= 0 || client.bottom <= 0) {
    return;
  }
  // The page is a child of this window, so this window's region rounds IT
  // too — the shell needs sibling corner caps for the same job only because
  // DirectComposition ignores a region set on the browser child itself. But
  // a region alone cuts on the pixel grid, and caps cannot paint the live
  // opener page around the card: the fringe keeps the region to the pixels
  // the curve covers fully and lays the arc's partial pixels over the page
  // in the frame's stroke, the outermost thing the card draws.
  SetCornerFringe(Scaled(kCardRadius), 0xF, QueryPalette().stroke_strong);
}

void PhiPeekPanel::OnThemeChanged() {
  PhiPanel::OnThemeChanged();
  // The fringe and the shadow keep the colours they were made with: the
  // stroke, and the shadow's strength (stronger on dark).
  ApplyCardShape();
  UpdateShadow();
}

void PhiPeekPanel::UpdateShadow() {
  if (!shadow_ || !shadow_->alive() || !alive()) {
    return;
  }
  RECT card = {};
  ::GetWindowRect(hwnd(), &card);
  const int margin = Scaled(kShadowMargin);
  const int width = (card.right - card.left) + margin * 2;
  const int height = (card.bottom - card.top) + margin * 2;
  if (width <= 0 || height <= 0) {
    return;
  }
  // The card's own top-left, in the shell's client space, then out by the
  // margin and down by the shadow's offset.
  POINT origin = {card.left, card.top};
  ::ScreenToClient(shadow_->parent(), &origin);
  const RECT bounds = {origin.x - margin,
                       origin.y - margin + Scaled(kShadowOffsetY),
                       origin.x - margin + width,
                       origin.y - margin + Scaled(kShadowOffsetY) + height};
  shadow_->SetPanelBounds(bounds);

  const PhiPalette colors = QueryPalette();
  shadow_->Render(width, height, margin, Scaled(kCardRadius),
                  std::max(1, Scaled(kShadowBlur)),
                  colors.dark ? kShadowAlphaDark : kShadowAlphaLight);
}

void PhiPeekPanel::SetSurfaceShown(bool shown) {
  if (!alive()) {
    return;
  }
  if (shown) {
    Show();  // The shadow, then the card, then the chips, in that order.
  } else {
    HidePanel();
    if (shadow_) {
      shadow_->HidePanel();
    }
  }
}

void PhiPeekPanel::RefreshState() {
  // The peek's own browser's snapshot: the model's copy, or — for a browser
  // the model has not been pushed yet (this panel is built inside its
  // Browser::Create) — pulled typed from the framework.
  const phi_framework::WindowSnapshot* snapshot =
      model_ ? model_->Get(window_id_) : nullptr;
  phi_framework::WindowSnapshot pulled;
  if (!snapshot && PhiWindowModel::Pull(framework_, window_id_, &pulled)) {
    snapshot = &pulled;
  }
  if (snapshot) {
    url_ = Widen(snapshot->nav.url);
    if (!snapshot->tabs.empty()) {
      tab_id_ = snapshot->tabs.front().id.value;
    }
  }
  // The header's favicon: the page's, copied out of the framework's
  // scratch buffer (which the next call overwrites).
  favicon_.clear();
  favicon_w_ = favicon_h_ = 0;
  if (framework_ && tab_id_ > 0) {
    int width = 0;
    int height = 0;
    const unsigned char* pixels = framework_->TabFaviconBgraById(
        window_id_.value, static_cast<int>(tab_id_), &width, &height);
    if (pixels && width > 0 && height > 0) {
      const size_t bytes =
          static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
      // SAFETY: the framework reports the buffer's own dimensions.
      const auto view =
          UNSAFE_BUFFERS(base::span<const unsigned char>(pixels, bytes));
      favicon_.assign(view.begin(), view.end());
      favicon_w_ = width;
      favicon_h_ = height;
    }
  }
  if (alive()) {
    Invalidate();
  }
}

PhiPeekPanel::Layout PhiPeekPanel::ComputeLayout() const {
  Layout out = {};
  RECT client = {};
  if (alive()) {
    ::GetClientRect(hwnd(), &client);
  }
  const int header = Scaled(kHeaderHeight);
  const int btn = Scaled(kButtonSize);
  const int gap = Scaled(kButtonGap);
  const int pad = Scaled(kHeaderPad);
  out.header = {0, 0, client.right, header};
  // A 1 px stroke frames the sheet: the page is inset by it on the three
  // sides the header does not cover.
  out.page = {1, header, std::max(1, static_cast<int>(client.right) - 1),
              std::max(header, static_cast<int>(client.bottom) - 1)};
  const int mid = header / 2;
  // Close trailing, then Split, then Open as tab (Windows keeps the close
  // box at the far right).
  int x = client.right - gap;
  out.close = {x - btn, mid - btn / 2, x, mid + btn / 2};
  x = out.close.left - gap;
  out.split = {x - btn, mid - btn / 2, x, mid + btn / 2};
  x = out.split.left - gap;
  out.expand = {x - btn, mid - btn / 2, x, mid + btn / 2};
  const int icon = Scaled(kFaviconSize);
  out.favicon = {pad, mid - icon / 2, pad + icon, mid + icon / 2};
  out.text = {out.favicon.right + Scaled(8), 0, out.expand.left - Scaled(8),
              header};
  return out;
}

void PhiPeekPanel::PositionContent() {
  if (!browser_hwnd_ || !::IsWindow(browser_hwnd_)) {
    return;
  }
  const Layout layout = ComputeLayout();
  ::SetWindowPos(browser_hwnd_, nullptr, layout.page.left, layout.page.top,
                 layout.page.right - layout.page.left,
                 layout.page.bottom - layout.page.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}


void PhiPeekPanel::Paint(HDC dc, const RECT&) {
  // The page covers everything under the header; the ground shows through
  // in the moment before the browser child has drawn, and as the 1 px frame
  // around it. Live, as before: the opener slot's palette ramps between
  // frames.
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  const PhiPalette palette = QueryPalette();
  HBRUSH ground = ::CreateSolidBrush(palette.card);
  ::FillRect(dc, &client, ground);
  ::DeleteObject(ground);
  const Layout layout = ComputeLayout();
  PaintHeader(dc, layout);
  PhiStrokeRoundRectAA(dc, client, Scaled(kCardRadius), palette.stroke_strong,
                       1);
}

void PhiPeekPanel::OnSize(int, int) {
  PositionContent();
  LayoutHits();
}

void PhiPeekPanel::PaintHeader(HDC dc, const Layout& layout) {
  const PhiPalette palette = QueryPalette();
  HBRUSH ground = ::CreateSolidBrush(palette.flyout);
  ::FillRect(dc, &layout.header, ground);
  ::DeleteObject(ground);
  RECT rule = {layout.header.left, layout.header.bottom - 1,
               layout.header.right, layout.header.bottom};
  HBRUSH sep = ::CreateSolidBrush(palette.separator);
  ::FillRect(dc, &rule, sep);
  ::DeleteObject(sep);
  ::SetBkMode(dc, TRANSPARENT);

  // The favicon (the globe until the page has one).
  if (!favicon_.empty()) {
    PhiDrawBgra(dc, layout.favicon, favicon_.data(), favicon_w_, favicon_h_);
  } else {
    PhiDrawIconGlyph(dc, layout.favicon, fluent::kGlobe, Scaled(kFaviconSize),
                     palette.label_dim);
  }
  // Host in `label`, path in `dim`, one line, ellipsised.
  std::wstring host = url_;
  std::wstring path;
  const size_t scheme = host.find(L"://");
  if (scheme != std::wstring::npos) {
    host.erase(0, scheme + 3);
  }
  const size_t slash = host.find(L'/');
  if (slash != std::wstring::npos) {
    path = host.substr(slash);
    host.resize(slash);
  }
  if (path == L"/") {
    path.clear();
  }
  PhiFontDesc text_desc;
  text_desc.size = 12;
  text_desc.face = PhiTextFaceName(/*display=*/false);
  HGDIOBJ old_font = ::SelectObject(dc, fonts().Get(text_desc));
  RECT text = layout.text;
  if (!host.empty()) {
    SIZE host_size = {};
    ::GetTextExtentPoint32W(dc, host.c_str(), static_cast<int>(host.size()),
                            &host_size);
    RECT host_rc = text;
    host_rc.right = std::min<LONG>(text.right, text.left + host_size.cx);
    ::SetTextColor(dc, palette.label);
    PhiDrawTextAA(dc, host.c_str(), -1, &host_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
    text.left = host_rc.right;
  }
  if (!path.empty() && text.right > text.left) {
    ::SetTextColor(dc, palette.label_dim);
    PhiDrawTextAA(dc, path.c_str(), -1, &text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
  }

  // The three 28 dip buttons: the ink wash under the hovered one, Fluent
  // glyphs at 16.
  const auto button = [&](int index, const RECT& rc, const wchar_t* glyph) {
    if (index == hot_) {
      PhiFillRoundRectAA(dc, rc, Scaled(kButtonRadius), palette.hover);
    }
    PhiDrawIconGlyph(dc, rc, glyph, Scaled(16), palette.label);
  };
  button(kHotExpand, layout.expand, kGlyphExpand);
  button(kHotClose, layout.close, kGlyphClose);
  // Split view is DRAWN, not lettered: Segoe MDL2 has no side-by-side mark
  // that renders the same across the Windows versions this ships on.
  {
    if (hot_ == kHotSplit) {
      PhiFillRoundRectAA(dc, layout.split, Scaled(kButtonRadius),
                         palette.hover);
    }
    RECT mark = layout.split;
    ::InflateRect(&mark, -Scaled(8), -Scaled(9));
    HPEN pen = ::CreatePen(PS_SOLID, std::max(1, Scaled(1)), palette.label);
    HGDIOBJ old_pen = ::SelectObject(dc, pen);
    HGDIOBJ old_brush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(dc, mark.left, mark.top, mark.right, mark.bottom);
    const int mid = (mark.left + mark.right) / 2;
    ::MoveToEx(dc, mid, mark.top, nullptr);
    ::LineTo(dc, mid, mark.bottom);
    ::SelectObject(dc, old_brush);
    ::SelectObject(dc, old_pen);
    ::DeleteObject(pen);
  }
  ::SelectObject(dc, old_font);
}

void PhiPeekPanel::LayoutHits() {
  ClearHits();
  if (!alive()) {
    return;
  }
  const Layout layout = ComputeLayout();
  // The tooltips and accessible names: the Mac PeekPanel's.
  AddHit(layout.close, kHotClose, PhiStr("peek.panel.closeButtonTooltip"),
         PhiHitRole::kButton);
  AddHit(layout.expand, kHotExpand, PhiStr("peek.panel.expandButtonTooltip"),
         PhiHitRole::kButton);
  AddHit(layout.split, kHotSplit, PhiStr("peek.panel.splitButtonTooltip"),
         PhiHitRole::kButton);
}

void PhiPeekPanel::OnHover(int hit, int) {
  hot_ = hit >= 0 && hit < static_cast<int>(hits_.size())
             ? hits_[static_cast<size_t>(hit)].key
             : -1;
  if (alive()) {
    Invalidate(ComputeLayout().header);
  }
}

void PhiPeekPanel::OnClick(int hit, POINT) {
  if (hit >= 0 && hit < static_cast<int>(hits_.size())) {
    ClickControl(hits_[static_cast<size_t>(hit)].key);
  }
}

int64_t PhiPeekPanel::surface_tab_id() const {
  return tab_id_;
}


void PhiPeekPanel::NoteEndReason(const char* reason) {
  end_reason_ = reason;
}

void PhiPeekPanel::Dismiss() {
  if (framework_) {
    framework_->CloseBrowserWindow(window_id_.value);
  }
}

void PhiPeekPanel::ClickControl(int index) {
  // The chip names the end for the metric (the Mac's PeekViewAnalytics
  // reasons); the close that follows is what reports it.
  if (index == kHotClose) {
    NoteEndReason("closed");
    Dismiss();
  } else if (index == kHotExpand) {
    // Expand: the page becomes a real tab in the opener window. The Mac moves
    // the live tab across; here the peek is its own browser, so the URL is
    // re-opened and the preview closed (see phi_peek_window.h).
    if (framework_ && !url_.empty()) {
      framework_->NewTab(opener_window_id_.value, Narrow(url_).c_str());
    }
    NoteEndReason("opened_as_tab");
    Dismiss();
  } else if (index == kHotSplit) {
    // An opener already in a split stays there: the page pairs with a
    // duplicate of the opener instead, as a new split (the Mac's
    // openPeekAsSplitWithOpener). That close announces its own end.
    if (coordinator_ &&
        coordinator_->surface_coordinator().OpenPeekBesideSplitOpener(
            window_id_)) {
      return;
    }
    // Split: the same page as a fresh tab, put beside the one that opened
    // the peek — which is what the reader is comparing it against. Both by
    // stable id: OpenTab answers with the new tab's, the opener's is known.
    if (framework_ && !url_.empty()) {
      const TabId fresh =
          framework_->OpenTab(opener_window_id_, Narrow(url_).c_str());
      if (fresh.valid() && opener_tab_id_ > 0) {
        framework_->CreateSplitViewForTabs(
            opener_window_id_.value, fresh.value,
            static_cast<int>(opener_tab_id_), "vertical");
      }
    }
    NoteEndReason("opened_as_split");
    Dismiss();
  }
}

}  // namespace phi_client

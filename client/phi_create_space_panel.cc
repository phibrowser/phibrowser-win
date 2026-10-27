// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_create_space_panel.h"

#include <commctrl.h>  // EM_SETCUEBANNER
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/no_destructor.h"
#include "base/rand_util.h"
#include "base/strings/utf_string_conversions.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_backdrop.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_menu_paint.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_shell_window.h"

namespace phi_client {

namespace {

constexpr wchar_t kClassName[] = L"PhiCreateSpacePanel";
constexpr UINT_PTR kFieldId = 1;

bool EnsureSurface(std::unique_ptr<PhiLayerSurface>& surface, const RECT& rc) {
  const int width = rc.right - rc.left;
  const int height = rc.bottom - rc.top;
  if (width <= 0 || height <= 0) {
    return false;
  }
  if (!surface || surface->width() != width || surface->height() != height) {
    surface = std::make_unique<PhiLayerSurface>(width, height);
  }
  return surface->valid();
}

void PrepareSurface(PhiLayerSurface& surface, const PhiPalette& palette) {
  ::GdiFlush();
  const COLORREF ground = palette.ground_top;
  const uint32_t rgb = (GetRValue(ground) << 16) |
                       (GetGValue(ground) << 8) | GetBValue(ground);
  // SAFETY: PhiLayerSurface owns a top-down, tightly packed 32bpp DIB.
  auto pixels = UNSAFE_BUFFERS(base::span<uint32_t>(
      static_cast<uint32_t*>(surface.bits()), surface.byte_size() / 4));
  const uint32_t tint = PhiBackdropPixel(rgb, palette.dark ? 0 : 0xFFFFFF);
  for (uint32_t& pixel : pixels) {
    pixel = (pixel & 0xFFFFFF) == rgb
                ? tint : PhiBackdropPixelOver(pixel, rgb, tint);
  }
}

// CreateSpacePanel's sidebar metrics (DIP).
constexpr int kContentWidth = 320;  // sidebarContentWidth
constexpr int kColumnPad = 10;      // .padding(.horizontal, 10)
constexpr int kColumnVPad = 20;     // .padding(.vertical, 20)
// The standalone window's column (CreateSpacePanel.Style.window).
constexpr int kWindowContentWidth = 280;  // windowContentWidth
constexpr int kWindowColumnPad = 20;      // .padding(.horizontal, 20)
constexpr int kWindowColumnVPad = 24;     // .padding(.vertical, 24)
constexpr int kSectionGap = 28;     // formStack VStack(spacing: 28)
constexpr int kHeaderGap = 12;      // header VStack(spacing: 12)
constexpr int kTitleSize = 18;      // .medium
constexpr int kSubtitleSize = 14;
constexpr int kBodySize = 13;
constexpr int kPillSize = 11;
constexpr int kFormGap = 4;         // form VStack(spacing: 4)
constexpr int kRowHeight = 32;
constexpr int kRowRadius = 8;
constexpr int kRowLead = 8;
constexpr int kRowTrail = 6;
constexpr int kPillHeight = 20;
constexpr int kPillLead = 12;
constexpr int kPillTrail = 8;
constexpr int kPillChevronGap = 4;
constexpr int kPillChevron = 9;
// IconPickerMetrics — the Mac picker at its preferredHeight. The Mac's
// search row is not built here (nor in the Settings pane's picker).
constexpr int kPickerHeight = 280;
constexpr int kPickerVPad = 2;      // iconBlock .padding(.vertical, 2)
constexpr int kPickerTop = 8;       // topPadding
constexpr int kSegmentHeight = 26;
constexpr int kSegmentWidth = 128;
constexpr int kSegmentToGrid = 8;
constexpr int kGridHPad = 8;
constexpr int kGridVPad = 8;
constexpr int kItem = 26;           // itemSize
constexpr int kItemIcon = 20;       // iconSize
constexpr int kEmojiFont = 14;      // emojiFontSize
constexpr int kItemRadius = 10;     // itemCornerRadius
constexpr int kRowSpacing = 4;
constexpr int kColumnSpacing = 2;   // reflowColumnSpacing
// colorBlock: 16pt dots in 20pt rings, a divider, the 20pt slider.
constexpr int kDot = 16;
constexpr int kRing = 20;
constexpr int kColorHPad = 8;
constexpr int kColorVPad = 12;
constexpr int kColorGap = 12;
constexpr int kSliderHeight = 20;
constexpr int kSliderKnob = 18;
// actions: the Create capsule (13 medium, 16/6 padding) over the Cancel text.
constexpr int kActionGap = 12;
constexpr int kButtonHPad = 16;
constexpr int kButtonVPad = 6;
constexpr int kButtonTextHeight = 16;
// The create-form swipe shares the Space-switch push-in's clock.
constexpr int kSlideMs = 150;
constexpr int kWheelStep = 3;  // Rows per notch.

COLORREF Blend(COLORREF over, COLORREF base, int alpha) {
  const auto mix = [alpha](int a, int b) {
    return (a * alpha + b * (255 - alpha)) / 255;
  };
  return RGB(mix(GetRValue(over), GetRValue(base)),
             mix(GetGValue(over), GetGValue(base)),
             mix(GetBValue(over), GetBValue(base)));
}

// Anti-aliased where Direct2D is available, GDI's RoundRect otherwise.
// Antialiased, true-radius shapes (phi_dwrite.h); they never need a GDI
// fallback of their own.
void FillRound(HDC dc, const RECT& r, int radius, COLORREF fill) {
  PhiFillRoundRectAA(dc, r, radius, fill);
}

void FillCircle(HDC dc, const RECT& r, COLORREF fill) {
  PhiFillEllipseAA(dc, r, fill);
}

void StrokeCircle(HDC dc, const RECT& r, COLORREF ink, int width) {
  PhiStrokeEllipseAA(dc, r, ink, width);
}

void RgbToHsv(COLORREF c, double* h, double* s, double* v) {
  const double r = GetRValue(c) / 255.0;
  const double g = GetGValue(c) / 255.0;
  const double b = GetBValue(c) / 255.0;
  const double mx = std::max(r, std::max(g, b));
  const double mn = std::min(r, std::min(g, b));
  const double d = mx - mn;
  *v = mx;
  *s = mx > 0 ? d / mx : 0;
  if (d <= 0) {
    *h = 0;
  } else if (mx == r) {
    *h = 60.0 * std::fmod((g - b) / d, 6.0);
  } else if (mx == g) {
    *h = 60.0 * ((b - r) / d + 2.0);
  } else {
    *h = 60.0 * ((r - g) / d + 4.0);
  }
  if (*h < 0) {
    *h += 360.0;
  }
}

COLORREF HsvToRgb(double h, double s, double v) {
  const double c = v * s;
  const double x = c * (1 - std::fabs(std::fmod(h / 60.0, 2.0) - 1));
  const double m = v - c;
  double r = 0, g = 0, b = 0;
  if (h < 60) {
    r = c, g = x;
  } else if (h < 120) {
    r = x, g = c;
  } else if (h < 180) {
    g = c, b = x;
  } else if (h < 240) {
    g = x, b = c;
  } else if (h < 300) {
    r = x, b = c;
  } else {
    r = c, b = x;
  }
  const auto to = [m](double q) {
    return static_cast<int>(std::lround((q + m) * 255.0));
  };
  return RGB(to(r), to(g), to(b));
}

std::string HexOf(COLORREF c) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", GetRValue(c), GetGValue(c),
                GetBValue(c));
  return buf;
}

int MeasureTextWidth(HDC dc, HFONT font, const std::wstring& text) {
  HGDIOBJ prev = font ? ::SelectObject(dc, font) : nullptr;
  RECT rc = {0, 0, 0, 0};
  PhiDrawTextAA(dc, text.c_str(), -1, &rc,
              DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
  if (prev) {
    ::SelectObject(dc, prev);
  }
  return rc.right - rc.left;
}

}  // namespace

struct PhiCreateSpacePanel::Geometry {
  RECT column{};
  RECT title{};
  RECT subtitle{};
  RECT name_row{};
  RECT profile_row{};
  RECT pill{};
  RECT icon_block{};
  RECT segment{};
  RECT seg_icon{};
  RECT seg_emoji{};
  RECT grid{};
  RECT color_block{};
  RECT divider{};
  RECT slider{};
  std::array<RECT, 8> dots{};
  size_t dot_count = 0;
  RECT create_button{};
  RECT cancel_button{};
  int content_height = 0;       // The column's full height (form scroll).
  int columns = 1;              // Grid columns that fit this width.
  int column_stride = 0;        // Cell-to-cell x step, spread across the grid.
  int grid_content_height = 0;  // Every cell row, for the grid's scroll.
};

PhiCreateSpacePanel::PhiCreateSpacePanel(PhiShellWindow* controller,
                                         HWND host_hwnd,
                                         PhiCreateSpaceFormHost* host)
    : controller_(controller), host_hwnd_(host_hwnd), host_(host) {}

PhiCreateSpacePanel::~PhiCreateSpacePanel() {
  Destroy();  // Takes the field with it; the base drops its fonts.
  if (field_brush_) {
    ::DeleteObject(field_brush_);
  }
}

bool PhiCreateSpacePanel::visible() const {
  return IsPanelVisible();
}

void PhiCreateSpacePanel::EnsureWindow() {
  if (hwnd() && ::IsWindow(hwnd())) {
    return;
  }
  HWND owner = host_hwnd_ ? host_hwnd_
                          : controller_ ? controller_->shell_hwnd() : nullptr;
  if (!owner) {
    return;
  }
  ClassSpec spec;
  spec.name = kClassName;
  Options options;
  options.style = WS_CLIPCHILDREN;  // The name field.
  const RECT nothing = {0, 0, 0, 0};
  if (!CreatePanel(spec, owner, nothing, options)) {
    return;
  }
  EnsureField();
}

PhiPalette PhiCreateSpacePanel::QueryPalette() const {
  return controller_ ? controller_->CurrentPalette() : PhiPalette();
}

namespace {

HFONT FaceFont(PhiFontSet* fonts, int size, int weight, const wchar_t* face) {
  PhiFontDesc desc;
  desc.size = size;
  desc.weight = weight;
  desc.face = face;
  return fonts->Get(desc);
}

}  // namespace

HFONT PhiCreateSpacePanel::TitleFont() const {
  return FaceFont(&const_cast<PhiCreateSpacePanel*>(this)->fonts(), kTitleSize,
                  FW_MEDIUM, L"Segoe UI");
}
HFONT PhiCreateSpacePanel::SubtitleFont() const {
  return FaceFont(&const_cast<PhiCreateSpacePanel*>(this)->fonts(),
                  kSubtitleSize, FW_NORMAL, L"Segoe UI");
}
HFONT PhiCreateSpacePanel::BodyFont() const {
  return FaceFont(&const_cast<PhiCreateSpacePanel*>(this)->fonts(), kBodySize,
                  FW_NORMAL, L"Segoe UI");
}
HFONT PhiCreateSpacePanel::BodyMediumFont() const {
  return FaceFont(&const_cast<PhiCreateSpacePanel*>(this)->fonts(), kBodySize,
                  FW_MEDIUM, L"Segoe UI");
}
HFONT PhiCreateSpacePanel::PillFont() const {
  return FaceFont(&const_cast<PhiCreateSpacePanel*>(this)->fonts(), kPillSize,
                  FW_NORMAL, L"Segoe UI");
}
HFONT PhiCreateSpacePanel::EmojiFont() const {
  return FaceFont(&const_cast<PhiCreateSpacePanel*>(this)->fonts(), kEmojiFont,
                  FW_NORMAL, L"Segoe UI Emoji");
}
HFONT PhiCreateSpacePanel::Mdl2Font() const {
  return FaceFont(&const_cast<PhiCreateSpacePanel*>(this)->fonts(), kEmojiFont,
                  FW_NORMAL, PhiIconFaceName());
}

void PhiCreateSpacePanel::EnsureField() {
  if (field_ || !alive()) {
    return;
  }
  field_ = ::CreateWindowExW(
      0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL, 0, 0,
      10, 10, hwnd(), reinterpret_cast<HMENU>(kFieldId),
      ::GetModuleHandleW(nullptr), nullptr);
  if (!field_) {
    return;
  }
  ::SendMessageW(field_, WM_SETFONT, reinterpret_cast<WPARAM>(BodyFont()),
                 TRUE);
  // The subclass finds the panel through the FIELD's own USERDATA: the
  // parent's now holds the PhiWindow the base bound at WM_NCCREATE.
  ::SetWindowLongPtrW(field_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  ::SendMessageW(field_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(
                     PhiStr("sidebar.createSpace.nameField.placeholder")));
  // Enter creates (the field's onSubmit), Escape cancels (.cancelAction).
  field_base_proc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
      field_, GWLP_WNDPROC,
      reinterpret_cast<LONG_PTR>(&PhiCreateSpacePanel::FieldProc)));
}

void PhiCreateSpacePanel::ReloadProfiles() {
  profiles_.clear();
  if (!controller_ || !controller_->spaces_host()) {
    return;
  }
  for (const std::string& row :
       controller_->spaces_host()->ProfileSummaries()) {
    const size_t tab = row.find('\t');
    const std::string id = row.substr(0, tab);
    const std::string name =
        tab == std::string::npos ? id : row.substr(tab + 1);
    profiles_.emplace_back(id, base::UTF8ToWide(name));
  }
}

std::wstring PhiCreateSpacePanel::SelectedProfileName() const {
  for (const auto& [id, name] : profiles_) {
    if (id == profile_id_) {
      return name;
    }
  }
  return base::UTF8ToWide(profile_id_);
}

const std::vector<std::pair<std::wstring, std::string>>&
PhiCreateSpacePanel::Cells() const {
  // One flat list per tab: the Phi catalog, or every emoji group in the
  // Mac's order (the Settings pane's picker shows the same lists).
  using CellList = std::vector<std::pair<std::wstring, std::string>>;
  static const base::NoDestructor<CellList> icons([] {
    CellList out;
    for (const PhiCatalogIcon& icon : PhiIconCatalogAll()) {
      out.emplace_back(std::wstring(1, icon.glyph),
                       std::string("phi:") + icon.id);
    }
    return out;
  }());
  static const base::NoDestructor<CellList> emoji([] {
    CellList out;
    for (const PhiEmojiGroup& group : PhiEmojiGroups()) {
      for (const PhiEmoji& item : group.items) {
        out.emplace_back(base::UTF8ToWide(item.text), "emoji:" + item.id);
      }
    }
    return out;
  }());
  return tab_ == Tab::kIcon ? *icons : *emoji;
}

void PhiCreateSpacePanel::Show(const RECT& bounds,
                               const std::string& initial_profile_id,
                               const std::string& source_theme_id,
                               int saturation) {
  EnsureWindow();
  if (!hwnd()) {
    return;
  }
  // CreateSpacePanel.onAppear: the profile the strip's "+" was pressed
  // from; a random Phi icon; a random theme distinct from the source
  // Space's so the new Space arrives looking different.
  // CreateSpacePanel's afterSpaceId: the Space active as the form opens is
  // the one the new Space lands after.
  source_space_id_ = controller_ ? controller_->active_space() : std::string();
  ReloadProfiles();
  profile_id_.clear();
  for (const auto& [id, name] : profiles_) {
    if (id == initial_profile_id) {
      profile_id_ = id;
      break;
    }
  }
  if (profile_id_.empty()) {
    profile_id_ = profiles_.empty() ? std::string("Default")
                                    : profiles_.front().first;
  }
  const auto& catalog = PhiIconCatalogAll();
  icon_ = catalog.empty()
              ? std::string(kPhiDefaultSpaceIcon)
              : "phi:" + std::string(
                             catalog[base::RandGenerator(catalog.size())].id);
  const auto& themes = PhiBuiltInThemes();
  std::vector<size_t> fresh;
  for (size_t i = 0; i < themes.size(); ++i) {
    if (themes[i].id != source_theme_id) {
      fresh.push_back(i);
    }
  }
  theme_id_ = fresh.empty() ? source_theme_id
                            : themes[fresh[base::RandGenerator(fresh.size())]].id;
  slider_ = std::max(0, std::min(100, saturation));
  user_adjusted_slider_ = false;
  tab_ = Tab::kIcon;
  grid_scroll_ = 0;
  form_scroll_ = 0;
  hover_ = Hit::kNone;
  hover_index_ = -1;
  dragging_slider_ = false;
  if (field_) {
    ::SetWindowTextW(field_, L"");
  }
  Reposition(bounds);
  // Preview at once, so the arriving surface already carries the new
  // Space's colour and the strip shows its icon. The standalone window
  // has neither: it just shows the selection (the Mac's .window style).
  if (controller_ && !standalone()) {
    controller_->SetCreateSpaceIcon(icon_);
  }
  PushPreview();
  RaisePanel();
  // The Space-switch push-in: the form arrives from the right. In its own
  // window it is simply there.
  if (standalone() ||
      !::AnimateWindow(hwnd(), kSlideMs, AW_SLIDE | AW_HOR_NEGATIVE)) {
    ::ShowWindow(hwnd(), SW_SHOWNA);
  }
  Invalidate();
}

int PhiCreateSpacePanel::ContentHeight() const {
  return hwnd() ? ComputeGeometry().content_height : 0;
}

void PhiCreateSpacePanel::Hide(bool animated) {
  if (!alive()) {
    return;
  }
  if (dragging_slider_) {
    dragging_slider_ = false;
    slider_capture_.Release();
  }
  if (IsPanelVisible()) {
    if (!animated ||
        !::AnimateWindow(hwnd(), kSlideMs,
                         AW_HIDE | AW_SLIDE | AW_HOR_POSITIVE)) {
      HidePanel();
    }
  }
}

void PhiCreateSpacePanel::Reposition(const RECT& bounds) {
  if (!alive() || !controller_) {
    return;
  }
  SyncDpiFromParent();
  const RECT fitted = {bounds.left, bounds.top,
                       bounds.left + std::max<LONG>(1, bounds.right -
                                                           bounds.left),
                       bounds.top + std::max<LONG>(1, bounds.bottom -
                                                          bounds.top)};
  SetPanelBounds(fitted);
  RaisePanel();
  LayoutField();
  Invalidate();
}

void PhiCreateSpacePanel::Refresh() {
  if (alive()) {
    NotifyThemeChanged();  // Re-reads QueryPalette() and repaints.
    if (field_) {
      ::InvalidateRect(field_, nullptr, FALSE);
    }
  }
}

void PhiCreateSpacePanel::OnDpiChanged(UINT) {
  ::SendMessageW(field_, WM_SETFONT, reinterpret_cast<WPARAM>(BodyFont()),
                 TRUE);
  LayoutField();
}

void PhiCreateSpacePanel::OnSize(int, int) {
  LayoutField();
}

void PhiCreateSpacePanel::FocusField() {
  if (visible() && field_ && ::IsWindow(field_)) {
    ::SetFocus(field_);
  }
}

void PhiCreateSpacePanel::OnProfileCreated(const std::string& profile_id) {
  ReloadProfiles();
  for (const auto& [id, name] : profiles_) {
    if (id == profile_id) {
      profile_id_ = id;
      break;
    }
  }
  Refresh();
}

PhiCreateSpacePanel::Geometry PhiCreateSpacePanel::ComputeGeometry() const {
  Geometry g;
  if (!hwnd()) {
    return g;
  }
  RECT rc = {};
  ::GetClientRect(hwnd(), &rc);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  // One bounded, centred column: the sidebar's width minus the slim
  // padding, up to 320 — or the window's 280 inside its wider padding.
  const int content_width =
      Scaled(standalone() ? kWindowContentWidth : kContentWidth);
  const int column_pad = Scaled(standalone() ? kWindowColumnPad : kColumnPad);
  const int column_vpad =
      Scaled(standalone() ? kWindowColumnVPad : kColumnVPad);
  const int column_w = std::max(1, std::min(content_width, w - 2 * column_pad));
  const int left = (w - column_w) / 2;
  const int right = left + column_w;

  HDC dc = ::GetDC(hwnd());
  int title_h = Scaled(24), subtitle_h = Scaled(18);
  int pill_w = Scaled(80), create_w = Scaled(110), cancel_w = Scaled(48);
  if (dc) {
    HGDIOBJ prev = ::SelectObject(dc, TitleFont());
    RECT m = {0, 0, column_w, 0};
    PhiDrawTextAA(dc, PhiStr("sidebar.createSpace.title"), -1, &m,
                DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    title_h = m.bottom - m.top;
    ::SelectObject(dc, SubtitleFont());
    m = {0, 0, column_w, 0};
    PhiDrawTextAA(dc, PhiStr("sidebar.createSpace.subtitle"), -1, &m,
                DT_CALCRECT | DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
    subtitle_h = m.bottom - m.top;
    ::SelectObject(dc, prev);
    pill_w = Scaled(kPillLead) +
             MeasureTextWidth(dc, PillFont(), SelectedProfileName()) +
             Scaled(kPillChevronGap) + Scaled(kPillChevron) +
             Scaled(kPillTrail);
    create_w = MeasureTextWidth(dc, BodyMediumFont(),
                                PhiStr("sidebar.createSpace.createButton")) +
               2 * Scaled(kButtonHPad);
    cancel_w = MeasureTextWidth(dc, BodyMediumFont(),
                                PhiStr("sidebar.createSpace.cancelButton")) +
               Scaled(8);
    ::ReleaseDC(hwnd(), dc);
  }

  int y = column_vpad;
  g.title = {left, y, right, y + title_h};
  y += title_h + Scaled(kHeaderGap);
  g.subtitle = {left, y, right, y + subtitle_h};
  y += subtitle_h + Scaled(kSectionGap);

  g.name_row = {left, y, right, y + Scaled(kRowHeight)};
  y += Scaled(kRowHeight) + Scaled(kFormGap);
  g.profile_row = {left, y, right, y + Scaled(kRowHeight)};
  {
    const int pill_h = Scaled(kPillHeight);
    const int pill_top = y + (Scaled(kRowHeight) - pill_h) / 2;
    const int pill_right = right - Scaled(kRowTrail);
    g.pill = {std::max(left + Scaled(kRowLead), pill_right - pill_w), pill_top,
              pill_right, pill_top + pill_h};
  }
  y += Scaled(kRowHeight) + Scaled(kFormGap);

  const int block_h = Scaled(kPickerHeight) + 2 * Scaled(kPickerVPad);
  g.icon_block = {left, y, right, y + block_h};
  {
    const int seg_w = std::min(Scaled(kSegmentWidth), column_w);
    const int seg_left = (left + right) / 2 - seg_w / 2;
    const int seg_top = y + Scaled(kPickerVPad) + Scaled(kPickerTop);
    g.segment = {seg_left, seg_top, seg_left + seg_w,
                 seg_top + Scaled(kSegmentHeight)};
    g.seg_icon = {g.segment.left, g.segment.top,
                  (g.segment.left + g.segment.right) / 2, g.segment.bottom};
    g.seg_emoji = {g.seg_icon.right, g.segment.top, g.segment.right,
                   g.segment.bottom};
    g.grid = {left + Scaled(kGridHPad), g.segment.bottom + Scaled(kSegmentToGrid),
              right - Scaled(kGridHPad),
              y + block_h - Scaled(kPickerVPad) - Scaled(kGridVPad)};
    const int grid_w = std::max(1, static_cast<int>(g.grid.right - g.grid.left));
    const int item = Scaled(kItem);
    g.columns = std::max(1, (grid_w + Scaled(kColumnSpacing)) /
                                (item + Scaled(kColumnSpacing)));
    // Flexible columns: the leftover width spreads between the cells.
    g.column_stride =
        g.columns > 1 ? (grid_w - item) / (g.columns - 1) : 0;
    const size_t count = Cells().size();
    const int rows = static_cast<int>((count + g.columns - 1) / g.columns);
    g.grid_content_height =
        rows > 0 ? rows * (item + Scaled(kRowSpacing)) - Scaled(kRowSpacing) +
                       2 * Scaled(kGridVPad)
                 : 0;
  }
  y += block_h + Scaled(kFormGap);

  {
    const int ring = Scaled(kRing);
    const int color_h = Scaled(kColorVPad) + ring + Scaled(kColorGap) + 1 +
                        Scaled(kColorGap) + Scaled(kSliderHeight) +
                        Scaled(kColorVPad);
    g.color_block = {left, y, right, y + color_h};
    const auto& themes = PhiBuiltInThemes();
    g.dot_count = std::min<size_t>(themes.size(), 8);
    const int inner_left = left + Scaled(kColorHPad);
    const int inner_right = right - Scaled(kColorHPad);
    const int cell_w = g.dot_count > 0
                           ? (inner_right - inner_left) /
                                 static_cast<int>(g.dot_count)
                           : 0;
    const int dots_top = y + Scaled(kColorVPad);
    for (size_t i = 0; i < g.dot_count; ++i) {
      const int cx = inner_left + cell_w * static_cast<int>(i) + cell_w / 2;
      g.dots[i] = {cx - ring / 2, dots_top, cx - ring / 2 + ring,
                   dots_top + ring};
    }
    const int divider_y = dots_top + ring + Scaled(kColorGap);
    g.divider = {inner_left, divider_y, inner_right, divider_y + 1};
    const int slider_top = divider_y + 1 + Scaled(kColorGap);
    g.slider = {inner_left, slider_top, inner_right,
                slider_top + Scaled(kSliderHeight)};
    y += color_h + Scaled(kSectionGap);
  }

  {
    const int button_h = Scaled(kButtonTextHeight) + 2 * Scaled(kButtonVPad);
    const int cx = (left + right) / 2;
    g.create_button = {cx - create_w / 2, y, cx - create_w / 2 + create_w,
                       y + button_h};
    y += button_h + Scaled(kActionGap);
    g.cancel_button = {cx - cancel_w / 2, y, cx - cancel_w / 2 + cancel_w,
                       y + Scaled(kButtonTextHeight)};
    y += Scaled(kButtonTextHeight) + column_vpad;
  }
  g.content_height = y;
  g.column = {left, 0, right, y};

  // Vertically centred when shorter than the sidebar; scrolled otherwise so
  // the create button stays reachable (the Mac's ScrollView).
  const int offset = g.content_height <= h
                         ? (h - g.content_height) / 2
                         : -std::max(0, std::min(form_scroll_,
                                                 g.content_height - h));
  const auto shift = [offset](RECT* r) {
    r->top += offset;
    r->bottom += offset;
  };
  for (RECT* r : {&g.column, &g.title, &g.subtitle, &g.name_row,
                  &g.profile_row, &g.pill, &g.icon_block, &g.segment,
                  &g.seg_icon, &g.seg_emoji, &g.grid, &g.color_block,
                  &g.divider, &g.slider, &g.create_button,
                  &g.cancel_button}) {
    shift(r);
  }
  for (size_t i = 0; i < g.dot_count; ++i) {
    shift(&g.dots[i]);
  }
  return g;
}

RECT PhiCreateSpacePanel::CellRect(const Geometry& g, size_t index) const {
  const int item = Scaled(kItem);
  const int col = static_cast<int>(index % static_cast<size_t>(g.columns));
  const int row = static_cast<int>(index / static_cast<size_t>(g.columns));
  const int x = g.grid.left + col * g.column_stride;
  const int y = g.grid.top + Scaled(kGridVPad) +
                row * (item + Scaled(kRowSpacing)) - grid_scroll_;
  return {x, y, x + item, y + item};
}

void PhiCreateSpacePanel::ClampScroll(const Geometry& g) {
  const int grid_h = g.grid.bottom - g.grid.top;
  grid_scroll_ = std::max(0, std::min(grid_scroll_,
                                      g.grid_content_height - grid_h));
  RECT rc = {};
  if (hwnd()) {
    ::GetClientRect(hwnd(), &rc);
  }
  form_scroll_ = std::max(
      0, std::min(form_scroll_,
                  g.content_height - static_cast<int>(rc.bottom - rc.top)));
}

void PhiCreateSpacePanel::LayoutField() {
  if (!field_) {
    return;
  }
  const Geometry g = ComputeGeometry();
  const int text_h = Scaled(17);
  const int left = g.name_row.left + Scaled(kRowLead);
  const int right = g.name_row.right - Scaled(kRowLead);
  ::SetWindowPos(field_, nullptr, left,
                 g.name_row.top + (Scaled(kRowHeight) - text_h) / 2,
                 std::max(1, right - left), text_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

PhiCreateSpacePanel::Hit PhiCreateSpacePanel::HitTest(const Geometry& g,
                                                      POINT pt,
                                                      int* index) const {
  *index = -1;
  if (::PtInRect(&g.pill, pt)) {
    return Hit::kProfilePill;
  }
  if (::PtInRect(&g.seg_icon, pt)) {
    return Hit::kTabIcon;
  }
  if (::PtInRect(&g.seg_emoji, pt)) {
    return Hit::kTabEmoji;
  }
  if (::PtInRect(&g.grid, pt)) {
    const size_t count = Cells().size();
    for (size_t i = 0; i < count; ++i) {
      const RECT cell = CellRect(g, i);
      if (cell.bottom < g.grid.top) {
        continue;
      }
      if (cell.top > g.grid.bottom) {
        break;
      }
      if (::PtInRect(&cell, pt)) {
        *index = static_cast<int>(i);
        return Hit::kCell;
      }
    }
    return Hit::kNone;
  }
  for (size_t i = 0; i < g.dot_count; ++i) {
    RECT hit = g.dots[i];
    ::InflateRect(&hit, Scaled(4), Scaled(4));
    if (::PtInRect(&hit, pt)) {
      *index = static_cast<int>(i);
      return Hit::kDot;
    }
  }
  if (::PtInRect(&g.slider, pt)) {
    return Hit::kSlider;
  }
  if (::PtInRect(&g.create_button, pt)) {
    return Hit::kCreate;
  }
  RECT cancel = g.cancel_button;
  ::InflateRect(&cancel, Scaled(6), Scaled(4));
  if (::PtInRect(&cancel, pt)) {
    return Hit::kCancel;
  }
  return Hit::kNone;
}

void PhiCreateSpacePanel::SelectTheme(size_t index) {
  const auto& themes = PhiBuiltInThemes();
  if (index >= themes.size() || theme_id_ == themes[index].id) {
    return;
  }
  theme_id_ = themes[index].id;
  PushPreview();
  Refresh();
}

void PhiCreateSpacePanel::SelectIcon(size_t index) {
  const auto& cells = Cells();
  if (index >= cells.size() || icon_ == cells[index].second) {
    return;
  }
  icon_ = cells[index].second;
  if (controller_) {
    controller_->SetCreateSpaceIcon(icon_);  // The placeholder pip follows.
  }
  Refresh();
}

void PhiCreateSpacePanel::SetSliderFromPoint(const Geometry& g, int x) {
  const int knob = Scaled(kSliderKnob);
  const int span = std::max(1, static_cast<int>(g.slider.right - g.slider.left) - knob);
  const int value = std::max(
      0, std::min(100, (x - static_cast<int>(g.slider.left) - knob / 2) * 100 / span));
  if (value == slider_ && user_adjusted_slider_) {
    return;
  }
  slider_ = value;
  user_adjusted_slider_ = true;
  PushPreview();
  Refresh();
}

void PhiCreateSpacePanel::PushPreview() {
  if (controller_ && !standalone()) {
    controller_->PreviewCreateSpaceTheme(theme_id_, slider_);
  }
}

void PhiCreateSpacePanel::ShowProfileMenu(const Geometry& g) {
  if (!controller_ || !controller_->spaces_host()) {
    return;
  }
  ReloadProfiles();
  HMENU menu = ::CreatePopupMenu();
  if (!menu) {
    return;
  }
  for (size_t i = 0; i < profiles_.size(); ++i) {
    ::AppendMenuW(menu,
                  MF_STRING | (profiles_[i].first == profile_id_ ? MF_CHECKED
                                                                 : 0),
                  static_cast<UINT>(i) + 1,
                  PhiEscapeMenuText(profiles_[i].second).c_str());
  }
  const UINT kNewProfile = static_cast<UINT>(profiles_.size()) + 1;
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kNewProfile,
                PhiStr("sidebar.createSpace.profilePicker.newProfileAction"));
  POINT screen = {g.pill.left, g.pill.bottom};
  ::ClientToScreen(hwnd(), &screen);
  const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, screen.x, screen.y,
      0, hwnd(), nullptr));
  ::DestroyMenu(menu);
  if (chosen == kNewProfile) {
    // The Mac's promptCreateProfile: a name, then the profile; the picker
    // selects it once the host reports it (OnProfileCreated).
    std::wstring name;
    if (PhiPromptText(host_hwnd_ ? host_hwnd_ : controller_->shell_hwnd(),
                      PhiStr("settings.profiles.createDialog.title"),
                      PhiStr("settings.profiles.createDialog.message"),
                      &name, /*confirm_label=*/nullptr, kPhiNameRules)) {
      controller_->spaces_host()->RequestCreateProfileForSpaceForm(
          controller_->slot().value, base::WideToUTF8(name));
    }
  } else if (chosen >= 1 && chosen <= profiles_.size()) {
    profile_id_ = profiles_[chosen - 1].first;
  }
  Refresh();
}

void PhiCreateSpacePanel::Create() {
  if (!controller_ || !controller_->spaces_host()) {
    return;
  }
  PhiNewSpaceSpec spec;
  if (field_) {
    wchar_t buffer[256] = {};
    ::GetWindowTextW(field_, buffer, 255);
    std::wstring name(buffer);
    const auto is_space = [](wchar_t c) {
      return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
    };
    while (!name.empty() && is_space(name.back())) {
      name.pop_back();
    }
    size_t start = 0;
    while (start < name.size() && is_space(name[start])) {
      ++start;
    }
    name.erase(0, start);
    if (name.empty()) {
      // "Space N" for the N-th Space — the Mac's default.
      name = PhiStrF("sidebar.createSpace.defaultName",
                     {std::to_wstring(
                         controller_->spaces_host()->Spaces().size() + 1)});
    }
    spec.name = base::WideToUTF8(name);
  }
  spec.profile_id = profile_id_;
  spec.icon = icon_;
  spec.theme_id = theme_id_;
  spec.saturation = user_adjusted_slider_ ? slider_ : -1;
  spec.after_space_id = source_space_id_;
  // The Mac derives the stored colorHex from the theme's overlay colour, so
  // the sidebar tint matches the pinned theme on both clients.
  const PhiTheme& theme = PhiThemeById(theme_id_);
  const bool dark = controller_->CurrentPalette().dark;
  spec.color_hex = HexOf(dark ? theme.dark.overlay : theme.light.overlay);
  if (host_) {
    host_->FormCommit(spec);
  } else {
    controller_->CommitCreateSpaceForm(spec);
  }
}

void PhiCreateSpacePanel::Cancel() {
  if (host_) {
    host_->FormCancel();
  } else if (controller_) {
    controller_->CancelCreateSpaceForm();
  }
}

void PhiCreateSpacePanel::Paint(HDC dc, const RECT&) {
  RECT rc = {};
  ::GetClientRect(hwnd(), &rc);
  if (UsesSidebarBackdrop() && EnsureSurface(backdrop_surface_, rc)) {
    PaintForm(backdrop_surface_->dc());
    PrepareSurface(*backdrop_surface_, QueryPalette());
    ::BitBlt(dc, 0, 0, rc.right, rc.bottom, backdrop_surface_->dc(), 0, 0,
             SRCCOPY);
    return;
  }
  PaintForm(dc);
}

bool PhiCreateSpacePanel::PreservePaintAlpha() const {
  return UsesSidebarBackdrop();
}

bool PhiCreateSpacePanel::UsesSidebarBackdrop() const {
  return !standalone() && controller_ &&
         controller_->sidebar_backdrop_enabled();
}

bool PhiCreateSpacePanel::PaintBackdropField(HWND field) {
  RECT rc = {};
  ::GetClientRect(field, &rc);
  if (!EnsureSurface(field_surface_, rc)) {
    return false;
  }
  PAINTSTRUCT ps = {};
  HDC dc = ::BeginPaint(field, &ps);
  const PhiPalette palette = QueryPalette();
  HBRUSH brush = ::CreateSolidBrush(Blend(palette.ink, palette.ground_top, 10));
  ::FillRect(field_surface_->dc(), &rc, brush);
  ::DeleteObject(brush);
  // Keep native EDIT behavior (selection, scrolling, IME and cue banner),
  // but give its GDI output the same alpha as the surrounding name row.
  ::CallWindowProcW(field_base_proc_, field, WM_PRINTCLIENT,
                    reinterpret_cast<WPARAM>(field_surface_->dc()),
                    PRF_CLIENT | PRF_ERASEBKGND);
  PrepareSurface(*field_surface_, palette);
  ::BitBlt(dc, 0, 0, rc.right, rc.bottom, field_surface_->dc(), 0, 0, SRCCOPY);
  ::EndPaint(field, &ps);
  return true;
}

void PhiCreateSpacePanel::PaintForm(HDC dc) {
  if (!controller_) {
    return;
  }
  RECT rc = {};
  ::GetClientRect(hwnd(), &rc);
  const PhiPalette palette = controller_->CurrentPalette();
  // Match the shell's flat ground. Paint() turns the sidebar form into
  // premultiplied pixels; standalone/flyout forms keep their opaque ground.
  HBRUSH brush = ::CreateSolidBrush(host_ ? host_->FormGround()
                                         : palette.ground_top);
  ::FillRect(dc, &rc, brush);
  ::DeleteObject(brush);
  const Geometry g = ComputeGeometry();
  ::SetBkMode(dc, TRANSPARENT);
  const COLORREF ink = palette.dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0);
  const COLORREF ground = host_ ? host_->FormGround() : palette.ground_top;
  const COLORREF fill4 = Blend(ink, ground, 10);    // primary 4%
  const COLORREF fill6 = Blend(ink, ground, 15);    // primary 6%
  const COLORREF fill10 = Blend(ink, ground, 25);   // hover / selected
  const COLORREF label = palette.label;             // primary 85%
  const COLORREF label50 = Blend(ink, ground, 128); // primary 50%
  const COLORREF divider = Blend(ink, ground, 31);

  // Header.
  HGDIOBJ prev = ::SelectObject(dc, TitleFont());
  ::SetTextColor(dc, label);
  RECT title = g.title;
  PhiDrawTextAA(dc, PhiStr("sidebar.createSpace.title"), -1, &title,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  ::SelectObject(dc, SubtitleFont());
  ::SetTextColor(dc, label50);
  RECT subtitle = g.subtitle;
  PhiDrawTextAA(dc, PhiStr("sidebar.createSpace.subtitle"), -1, &subtitle,
              DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);

  // Name field (the EDIT paints its own text over this row).
  FillRound(dc, g.name_row, Scaled(kRowRadius), fill4);

  // Profile row: label left, the picker pill right.
  FillRound(dc, g.profile_row, Scaled(kRowRadius), fill4);
  ::SelectObject(dc, BodyFont());
  ::SetTextColor(dc, label);
  RECT profile_label = {g.profile_row.left + Scaled(kRowLead),
                        g.profile_row.top, g.pill.left - Scaled(8),
                        g.profile_row.bottom};
  PhiDrawTextAA(dc, PhiStr("sidebar.createSpace.profilePicker.label"), -1,
              &profile_label,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  if (!PhiFillCapsuleAA(dc, g.pill,
                        hover_ == Hit::kProfilePill ? fill10 : fill6)) {
    FillRound(dc, g.pill, (g.pill.bottom - g.pill.top) / 2,
              hover_ == Hit::kProfilePill ? fill10 : fill6);
  }
  ::SelectObject(dc, PillFont());
  ::SetTextColor(dc, label);
  RECT pill_text = {g.pill.left + Scaled(kPillLead), g.pill.top,
                    g.pill.right - Scaled(kPillTrail) - Scaled(kPillChevron) -
                        Scaled(kPillChevronGap),
                    g.pill.bottom};
  const std::wstring profile_name = SelectedProfileName();
  PhiDrawTextAA(dc, profile_name.c_str(), -1, &pill_text,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                  DT_END_ELLIPSIS);
  ::SetTextColor(dc, label50);
  RECT chevron = {g.pill.right - Scaled(kPillTrail) - Scaled(kPillChevron),
                  g.pill.top, g.pill.right - Scaled(kPillTrail), g.pill.bottom};
  PhiDrawTextAA(dc, L"↕", -1, &chevron,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  // Icon picker: the segmented control over the scrolling grid.
  FillRound(dc, g.icon_block, Scaled(kRowRadius), fill4);
  FillRound(dc, g.segment, Scaled(6), fill6);
  {
    const RECT& selected = tab_ == Tab::kIcon ? g.seg_icon : g.seg_emoji;
    RECT knob = selected;
    ::InflateRect(&knob, -Scaled(2), -Scaled(2));
    FillRound(dc, knob, Scaled(5), palette.selected);
    ::SelectObject(dc, BodyFont());
    ::SetTextColor(dc, tab_ == Tab::kIcon ? label : label50);
    RECT seg = g.seg_icon;
    PhiDrawTextAA(dc, PhiStr("common.iconPicker.iconTabTitle"), -1, &seg,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SetTextColor(dc, tab_ == Tab::kEmoji ? label : label50);
    seg = g.seg_emoji;
    PhiDrawTextAA(dc, PhiStr("common.iconPicker.emojiTabTitle"), -1, &seg,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }
  PaintCells(dc, g, palette);

  // Colour: the theme dots, a divider, the saturation slider.
  FillRound(dc, g.color_block, Scaled(kRowRadius), fill4);
  {
    const auto& themes = PhiBuiltInThemes();
    const PhiAppearance appearance =
        controller_->prefs_host() ? controller_->prefs_host()->Appearance()
                                  : PhiAppearance::kSystem;
    for (size_t i = 0; i < g.dot_count; ++i) {
      const PhiPalette resolved =
          PhiResolvePalette(themes[i], appearance, slider_);
      const bool pure = i == 0;
      const COLORREF fill = pure ? RGB(0xFF, 0xFF, 0xFF) : resolved.accent;
      if (theme_id_ == themes[i].id) {
        StrokeCircle(dc, g.dots[i], resolved.accent, Scaled(2));
      }
      RECT dot = g.dots[i];
      const int inset = (Scaled(kRing) - Scaled(kDot)) / 2;
      ::InflateRect(&dot, -inset, -inset);
      FillCircle(dc, dot, fill);
      if (pure) {
        StrokeCircle(dc, dot, divider, 1);  // showsContrastBorder
      }
    }
    HBRUSH line = ::CreateSolidBrush(divider);
    ::FillRect(dc, &g.divider, line);
    ::DeleteObject(line);
    PaintSlider(dc, g.slider, palette);
  }

  // Actions.
  {
    const PhiPalette selected_palette = PhiResolvePalette(
        PhiThemeById(theme_id_),
        controller_->prefs_host() ? controller_->prefs_host()->Appearance()
                                  : PhiAppearance::kSystem,
        slider_);
    COLORREF accent = selected_palette.accent;
    if (hover_ == Hit::kCreate) {
      accent = Blend(RGB(0, 0, 0), accent, 30);
    }
    if (!PhiFillCapsuleAA(dc, g.create_button, accent)) {
      FillRound(dc, g.create_button,
                (g.create_button.bottom - g.create_button.top) / 2, accent);
    }
    ::SelectObject(dc, BodyMediumFont());
    ::SetTextColor(dc, RGB(0xF7, 0xF7, 0xF7));  // white 95%
    RECT create = g.create_button;
    PhiDrawTextAA(dc, PhiStr("sidebar.createSpace.createButton"), -1, &create,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SetTextColor(dc, hover_ == Hit::kCancel ? label : Blend(ink, ground, 200));
    RECT cancel = g.cancel_button;
    PhiDrawTextAA(dc, PhiStr("sidebar.createSpace.cancelButton"), -1, &cancel,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }
  ::SelectObject(dc, prev);
}

void PhiCreateSpacePanel::PaintCells(HDC dc,
                                     const Geometry& g,
                                     const PhiPalette& palette) {
  const COLORREF ink = palette.dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0);
  const COLORREF ground = palette.ground_top;
  const auto& cells = Cells();
  const int save = ::SaveDC(dc);
  ::IntersectClipRect(dc, g.grid.left, g.grid.top, g.grid.right,
                      g.grid.bottom);
  for (size_t i = 0; i < cells.size(); ++i) {
    const RECT cell = CellRect(g, i);
    if (cell.bottom < g.grid.top) {
      continue;
    }
    if (cell.top > g.grid.bottom) {
      break;
    }
    const bool selected = cells[i].second == icon_;
    const bool hovered =
        hover_ == Hit::kCell && hover_index_ == static_cast<int>(i);
    if (selected || hovered) {
      FillRound(dc, cell, Scaled(kItemRadius),
                Blend(ink, ground, selected ? 25 : 15));
    }
    // Inset: the colour pass repaints its whole rect from a sampled corner,
    // and a selected cell's rounded fill must survive at the corners.
    RECT glyph = cell;
    ::InflateRect(&glyph, -Scaled(3), -Scaled(3));
    const std::wstring& text = cells[i].first;
    if (PhiDrawCatalogArtwork(dc, glyph, text, Scaled(kItemIcon),
                              palette.label)) {
      continue;
    }
    if (!PhiIsPrivateGlyph(text) &&
        PhiDrawColorText(dc, glyph, text, Scaled(kEmojiFont), palette.label)) {
      continue;
    }
    HFONT font = PhiIsPrivateGlyph(text) ? Mdl2Font() : EmojiFont();
    HGDIOBJ prev = font ? ::SelectObject(dc, font) : nullptr;
    ::SetTextColor(dc, palette.label);
    RECT text_rc = cell;
    PhiDrawTextAA(dc, text.c_str(), -1, &text_rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (prev) {
      ::SelectObject(dc, prev);
    }
  }
  ::RestoreDC(dc, save);
}

void PhiCreateSpacePanel::PaintSlider(HDC dc,
                                      const RECT& bounds,
                                      const PhiPalette& palette) {
  // ThemeOpacitySliderView, as the Settings pane draws it: a 10pt rounded
  // bar ramping the theme overlay's saturation 0.1 → 0.5 (Pure ramps
  // brightness, brightest at the left), a 1pt border, an 18pt white knob.
  const PhiTheme& theme = PhiThemeById(theme_id_);
  const COLORREF overlay = palette.dark ? theme.dark.overlay : theme.light.overlay;
  const bool pure = theme_id_ == "pure";
  const int mid = (bounds.top + bounds.bottom) / 2;
  RECT track = {bounds.left, mid - Scaled(5), bounds.right, mid + Scaled(5)};
  const int span = track.right - track.left;
  double hue = 0, sat = 0, val = 0;
  RgbToHsv(overlay, &hue, &sat, &val);
  // Painted square, then the corners are knocked back to the ground
  // (antialiased) — a region clip would cut the rounded track on the
  // pixel grid.
  const int bands = 32;
  for (int i = 0; i < bands; ++i) {
    RECT band = {track.left + span * i / bands, track.top,
                 track.left + span * (i + 1) / bands, track.bottom};
    const double t = (i + 0.5) / bands;
    COLORREF tint;
    if (pure) {
      const double hi = palette.dark ? 0.40 : 0.94;
      const double lo = palette.dark ? 0.0 : 0.84;
      tint = HsvToRgb(hue, sat, hi + (lo - hi) * t);
    } else {
      tint = HsvToRgb(hue, 0.1 + 0.4 * t, val);
    }
    HBRUSH brush = ::CreateSolidBrush(tint);
    ::FillRect(dc, &band, brush);
    ::DeleteObject(brush);
  }
  PhiKnockOutRoundRectAA(dc, track, Scaled(5), palette.ground_top);
  PhiStrokeRoundRectAA(dc, track, Scaled(5), palette.border, 1);
  const int knob = Scaled(kSliderKnob);
  const int at = track.left + (span - knob) * slider_ / 100;
  RECT dot = {at, mid - knob / 2, at + knob, mid + knob / 2};
  FillCircle(dc, dot, RGB(0xFF, 0xFF, 0xFF));
  StrokeCircle(dc, dot, palette.border, 1);
}

void PhiCreateSpacePanel::OnMouseDown(int, POINT pt) {
  const Geometry g = ComputeGeometry();
  int index = -1;
  switch (HitTest(g, pt, &index)) {
    case Hit::kProfilePill:
      ShowProfileMenu(g);
      break;
    case Hit::kTabIcon:
    case Hit::kTabEmoji: {
      const Tab tab = ::PtInRect(&g.seg_emoji, pt) ? Tab::kEmoji : Tab::kIcon;
      if (tab != tab_) {
        tab_ = tab;
        grid_scroll_ = 0;
        Invalidate();
      }
      break;
    }
    case Hit::kCell:
      SelectIcon(static_cast<size_t>(index));
      break;
    case Hit::kDot:
      SelectTheme(static_cast<size_t>(index));
      break;
    case Hit::kSlider:
      dragging_slider_ = true;
      // RAII capture: WM_CAPTURECHANGED and WM_CANCELMODE both end the
      // drag through OnCaptureLost(), so a stolen capture cannot leave the
      // slider stuck (lane 3 §4.2).
      slider_capture_ = BeginCapture();
      SetSliderFromPoint(g, pt.x);
      break;
    case Hit::kCreate:
      Create();
      return;
    case Hit::kCancel:
      Cancel();
      return;
    case Hit::kNone:
      break;
  }
  // The ground takes focus, so Escape / Enter still reach the form.
  if (!dragging_slider_ && ::PtInRect(&g.name_row, pt) && field_) {
    ::SetFocus(field_);
  } else {
    ::SetFocus(hwnd());
  }
}

void PhiCreateSpacePanel::OnWheel(int delta, POINT pt) {
  Geometry g = ComputeGeometry();
  const int notches = delta / WHEEL_DELTA;
  const int step = Scaled(kItem + kRowSpacing) * kWheelStep * notches;
  if (::PtInRect(&g.grid, pt) &&
      g.grid_content_height > g.grid.bottom - g.grid.top) {
    grid_scroll_ -= step;
  } else {
    form_scroll_ -= step;
  }
  ClampScroll(g);
  LayoutField();
  Invalidate();
}

void PhiCreateSpacePanel::OnCaptureLost() {
  dragging_slider_ = false;
}

bool PhiCreateSpacePanel::OnKey(WPARAM virtual_key) {
  if (virtual_key == VK_ESCAPE) {
    Cancel();
    return true;
  }
  if (virtual_key == VK_RETURN) {
    Create();
    return true;
  }
  return PhiPanel::OnKey(virtual_key);
}

LRESULT PhiCreateSpacePanel::OnMessage(UINT message,
                                       WPARAM wparam,
                                       LPARAM lparam,
                                       bool* handled) {
  switch (message) {
    case WM_MOUSEMOVE:
      // The slider drag owns the pointer while it runs; hover is the base's
      // otherwise, over the hits the form's own HitTest resolves.
      if (dragging_slider_) {
        const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        SetSliderFromPoint(ComputeGeometry(), pt.x);
        *handled = true;
        return 0;
      }
      {
        const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const Geometry g = ComputeGeometry();
        int index = -1;
        const Hit hit = HitTest(g, pt, &index);
        if (hit != hover_ || index != hover_index_) {
          hover_ = hit;
          hover_index_ = index;
          Invalidate();
        }
      }
      break;  // The base still arms TrackMouseEvent for WM_MOUSELEAVE.
    case WM_MOUSELEAVE:
      if (hover_ != Hit::kNone) {
        hover_ = Hit::kNone;
        hover_index_ = -1;
        Invalidate();
      }
      break;  // The base clears its own leave tracking.
    case WM_LBUTTONUP:
      if (dragging_slider_) {
        dragging_slider_ = false;
        slider_capture_.Release();
        *handled = true;
        return 0;
      }
      break;
    case WM_CTLCOLOREDIT:
      if (controller_ && reinterpret_cast<HWND>(lparam) == field_) {
        const PhiPalette colors = QueryPalette();
        const COLORREF ink =
            colors.dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0);
        // The row's own wash: over the sidebar tint, or the standalone
        // window's ground.
        const COLORREF ground =
            host_ ? host_->FormGround() : colors.ground_top;
        const COLORREF fill = Blend(ink, ground, 10);
        HDC edit_dc = reinterpret_cast<HDC>(wparam);
        ::SetTextColor(edit_dc, colors.label);
        ::SetBkColor(edit_dc, fill);  // Seamless with the row's wash.
        if (!field_brush_ || field_brush_color_ != fill) {
          if (field_brush_) {
            ::DeleteObject(field_brush_);
          }
          field_brush_ = ::CreateSolidBrush(fill);
          field_brush_color_ = fill;
        }
        *handled = true;
        return reinterpret_cast<LRESULT>(field_brush_);
      }
      break;
    default:
      break;
  }
  return PhiPanel::OnMessage(message, wparam, lparam, handled);
}

// static
LRESULT CALLBACK PhiCreateSpacePanel::FieldProc(HWND hwnd,
                                                UINT msg,
                                                WPARAM w,
                                                LPARAM l) {
  auto* self = reinterpret_cast<PhiCreateSpacePanel*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  const WNDPROC base = self && self->field_base_proc_
                           ? self->field_base_proc_
                           : ::DefWindowProcW;
  switch (msg) {
    case WM_PAINT:
      if (self && self->UsesSidebarBackdrop() &&
          self->PaintBackdropField(hwnd)) {
        return 0;
      }
      break;
    case WM_ERASEBKGND:
      if (self && self->UsesSidebarBackdrop()) {
        return 1;  // The buffered paint owns the name field's whole ground.
      }
      break;
    case WM_KEYDOWN:
      if (self && w == VK_ESCAPE) {
        self->Cancel();
        return 0;
      }
      if (self && w == VK_RETURN) {
        self->Create();
        return 0;
      }
      break;
    case WM_CHAR:
      if (w == VK_RETURN || w == VK_ESCAPE) {
        return 0;  // No bell for the keys the form consumed.
      }
      break;
    default:
      break;
  }
  // EDIT also paints synchronously during editing/selection. Follow those
  // writes with the alpha-aware path so they cannot leave zero-alpha text.
  const bool repaint = self && self->UsesSidebarBackdrop() &&
      (msg == WM_CHAR || msg == WM_KEYDOWN || msg == WM_SETTEXT ||
       msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
       (msg == WM_MOUSEMOVE && (w & MK_LBUTTON)) ||
       msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == EM_SETSEL ||
       msg == EM_REPLACESEL || msg == EM_UNDO ||
       msg == WM_PASTE || msg == WM_CUT || msg == WM_CLEAR || msg == WM_UNDO ||
       msg == WM_IME_COMPOSITION);
  const LRESULT result = ::CallWindowProcW(base, hwnd, msg, w, l);
  // Native notifications may close the form; do not dereference self here.
  if (repaint && ::IsWindow(hwnd)) {
    ::InvalidateRect(hwnd, nullptr, FALSE);
  }
  return result;
}

}  // namespace phi_client

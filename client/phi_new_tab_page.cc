// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_new_tab_page.h"

#include <commctrl.h>  // EM_SETCUEBANNER
#include <windowsx.h>

#include <algorithm>
#include <vector>

#include "client/phi_dwrite.h"
#include "client/phi_i18n.h"
#include "client/phi_mac_icons.h"
#include "client/phi_theme.h"
#include "client/phi_shell_window.h"

namespace phi_client {

namespace {

constexpr wchar_t kClassName[] = L"PhiNewTabPage";
constexpr UINT_PTR kFieldId = 1;
// The three faces, in DESIGN units — the base's PhiFontSet scales them and
// drops them all on a dpi change, which is what retires the file's own
// fonts_dpi_ bookkeeping (lane 3 §3.5).
constexpr wchar_t kBrandFace[] = L"Segoe UI Light";
constexpr wchar_t kBodyFace[] = L"Segoe UI";

// NewTabViewController's metrics (DIP).
constexpr int kIconSize = 64;          // iconSize
constexpr int kIconLabelGap = 6;       // label 6pt below the icon
constexpr int kLabelBoxGap = 48;       // omnibox 48pt below the label
constexpr int kLabelFontSize = 21;     // IvyPrestoHeadline-Light 21
constexpr int kBoxWidth = 680;         // OmniBoxViewController.boxWidth
constexpr int kBoxHeight = 57;         // collapsedOmniBoxHeight
constexpr int kBoxMinWidth = 240;      // updateContentLayout's floor
constexpr int kBoxHorizontalInset = 24;
constexpr int kBoxRadius = 14;
// Inside the box: the magnifier leads, the text follows.
constexpr int kGlyphLead = 18;
constexpr int kGlyphSize = 16;
constexpr int kGlyphTextGap = 12;
constexpr int kFieldTrail = 18;
constexpr int kFieldFontSize = 16;
constexpr int kFieldHeight = 22;

// Segoe MDL2 Assets "Search".
constexpr wchar_t kSearchGlyph[] = L"\xE721";

COLORREF BlendInk(COLORREF ink, COLORREF over, int alpha255) {
  const auto mix = [alpha255](int a, int b) {
    return (a * alpha255 + b * (255 - alpha255)) / 255;
  };
  return RGB(mix(GetRValue(ink), GetRValue(over)),
             mix(GetGValue(ink), GetGValue(over)),
             mix(GetBValue(ink), GetBValue(over)));
}

}  // namespace

PhiNewTabPage::PhiNewTabPage(PhiShellWindow* controller)
    : controller_(controller) {}

PhiNewTabPage::~PhiNewTabPage() {
  Destroy();  // Takes the field with it; the base clears its own state.
  if (field_brush_) {
    ::DeleteObject(field_brush_);
  }
}

// static
bool PhiNewTabPage::IsNewTabUrl(const std::string& url) {
  return url.rfind("chrome://newtab", 0) == 0 ||
         url.rfind("chrome://new-tab-page", 0) == 0 ||
         url.rfind("phi://newtab", 0) == 0;
}

bool PhiNewTabPage::visible() const {
  return IsPanelVisible();
}

PhiPalette PhiNewTabPage::QueryPalette() const {
  // The slot's live palette, so a theme ramp (and now an OS appearance
  // switch, through the theme bus) repaints the page with the chrome.
  return controller_ ? controller_->CurrentPalette() : PhiPalette();
}

HFONT PhiNewTabPage::LabelFont() const {
  // The Mac's brand display face (IvyPresto Headline Light) is not shipped
  // here; Segoe's light cut is the system's equivalent weight.
  PhiFontDesc desc;
  desc.size = kLabelFontSize;
  desc.weight = FW_LIGHT;
  desc.face = kBrandFace;
  return const_cast<PhiNewTabPage*>(this)->fonts().Get(desc);
}

HFONT PhiNewTabPage::FieldFont() const {
  PhiFontDesc desc;
  desc.size = kFieldFontSize;
  desc.face = kBodyFace;
  return const_cast<PhiNewTabPage*>(this)->fonts().Get(desc);
}

HFONT PhiNewTabPage::GlyphFont() const {
  PhiFontDesc desc;
  desc.size = kGlyphSize;
  desc.face = PhiIconFaceName();
  return const_cast<PhiNewTabPage*>(this)->fonts().Get(desc);
}

void PhiNewTabPage::PushFieldFont() {
  if (field_) {
    ::SendMessageW(field_, WM_SETFONT,
                   reinterpret_cast<WPARAM>(FieldFont()), TRUE);
  }
}

bool PhiNewTabPage::WantsFocus() const {
  return visible() && !controls_hidden_ && field_ && ::IsWindow(field_);
}

void PhiNewTabPage::EnsureWindow() {
  if (alive()) {
    return;
  }
  HWND owner = controller_ ? controller_->shell_hwnd() : nullptr;
  if (!owner) {
    return;
  }
  ClassSpec spec;
  spec.name = kClassName;
  Options options;
  // WS_CLIPCHILDREN for the omnibox EDIT; WS_CLIPSIBLINGS is PhiPanel's,
  // and it is what keeps the corner caps stacked above this page visible
  // instead of squaring off the card's rounded corners.
  options.style = WS_CLIPCHILDREN;
  const RECT nothing = {0, 0, 0, 0};
  if (!CreatePanel(spec, owner, nothing, options)) {
    return;
  }
  EnsureField();
}

void PhiNewTabPage::EnsureField() {
  if (field_ || !alive() || !controller_) {
    return;
  }
  field_ = ::CreateWindowExW(
      0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL, 0, 0,
      10, 10, hwnd(), reinterpret_cast<HMENU>(kFieldId),
      ::GetModuleHandleW(nullptr), nullptr);
  if (!field_) {
    return;
  }
  PushFieldFont();
  // The Mac omnibox's placeholder, drawn by the EDIT itself while empty.
  ::SendMessageW(field_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(
                     PhiStr("addressBar.configuration.defaultPlaceholder")));
  // One omnibox session for the whole window: the controller's subclass
  // gives this field the address bar's Enter / Escape / arrow handling and
  // its suggestion dropdown.
  controller_->AttachOmniboxField(field_);
}

void PhiNewTabPage::Show() {
  EnsureWindow();
  if (!alive()) {
    return;
  }
  controls_hidden_ = false;
  if (field_) {
    ::SetWindowTextW(field_, L"");
  }
  Reposition();
  ShowPanel();
}

void PhiNewTabPage::Hide() {
  HidePanel();
}

void PhiNewTabPage::SetPlacement(int tab_id, const RECT* pane) {
  const bool has_pane = pane != nullptr;
  const RECT rect = has_pane ? *pane : RECT{};
  const bool moved = has_pane != has_pane_ || tab_id != tab_id_ ||
                     (has_pane && !::EqualRect(&rect, &pane_));
  tab_id_ = tab_id;
  has_pane_ = has_pane;
  pane_ = rect;
  if (moved && alive()) {
    Reposition();
  }
}

void PhiNewTabPage::Reposition() {
  if (!alive() || !controller_) {
    return;
  }
  SyncDpiFromParent();
  if (has_pane_) {
    // Inside a split the page sits INSIDE the pane's edge: the framework
    // strokes the focused pane's 2 dip accent ring within the pane's own
    // bounds (docs/split-view-chrome.md), and a child window over the whole
    // pane would cover it and square the pane's rounded corner.
    RECT inner = pane_;
    const int ring = Scaled(2);
    ::InflateRect(&inner, -ring, -ring);
    SetPanelBounds(inner);
    ApplyPaneCorners();
  } else {
    SetPagePanelBounds(controller_->ContentRect(), Scaled(8));
  }
  RaisePanel();
  LayoutField();
  Invalidate();
}

void PhiNewTabPage::ApplyPaneCorners() {
  if (!has_pane_ || !controller_) {
    return;
  }
  // Square, but where the pane meets the content card's rounded corner:
  // there the card's radius less the ring, concentric with it. Not a bare
  // region (it stair-steps): the EDIT child rules out a shaped layer, and
  // what lies outside the curve is the framework's pane chrome, which no
  // corner cap could match — so the fringe carries the arc's partial pixels
  // in the page's own ground.
  SetCornerFringe(controller_->ContentCornerRadius() - Scaled(2),
                  controller_->SplitPaneCardCorners(pane_),
                  QueryPalette().card);
}

void PhiNewTabPage::OnThemeChanged() {
  PhiPanel::OnThemeChanged();
  ApplyPaneCorners();
  // The EDIT keeps the brush and colours it was last painted with until it
  // repaints itself: WM_CTLCOLOREDIT only runs then, and an invalidate of
  // this window does not reach a child it clips out (WS_CLIPCHILDREN).
  if (field_ && ::IsWindow(field_)) {
    ::RedrawWindow(field_, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
  }
}

void PhiNewTabPage::Refresh() {
  if (alive()) {
    // Re-reads QueryPalette() and repaints: a palette ramp moves the page
    // with the chrome.
    NotifyThemeChanged();
  }
}

void PhiNewTabPage::OnDpiChanged(UINT) {
  PushFieldFont();  // The base dropped the old HFONT with the font set.
  LayoutField();
}

void PhiNewTabPage::OnSize(int, int) {
  LayoutField();
}

void PhiNewTabPage::HideControls() {
  if (controls_hidden_) {
    return;
  }
  controls_hidden_ = true;
  if (field_) {
    ::ShowWindow(field_, SW_HIDE);
  }
  Refresh();
}

void PhiNewTabPage::Reset() {
  controls_hidden_ = false;
  if (field_) {
    ::SetWindowTextW(field_, L"");
    ::ShowWindow(field_, SW_SHOW);
  }
  LayoutField();
  Refresh();
}

void PhiNewTabPage::FocusField() {
  if (WantsFocus()) {
    ::SetFocus(field_);
  }
}

RECT PhiNewTabPage::BoxScreenRect() const {
  RECT box = ComputeGeometry().box;
  if (alive()) {
    ::MapWindowPoints(hwnd(), nullptr, reinterpret_cast<POINT*>(&box), 2);
  }
  return box;
}

PhiNewTabPage::Geometry PhiNewTabPage::ComputeGeometry() const {
  Geometry g;
  if (!alive() || !controller_) {
    return g;
  }
  RECT rc = {};
  ::GetClientRect(hwnd(), &rc);
  const int clip_w = rc.right - rc.left;
  const int clip_h = rc.bottom - rc.top;
  const bool incognito = controller_->ShowsIncognito();

  // updateContentLayout: the box fits the visible clip — its natural 680
  // overflows a narrow card (a split pane) — and the column stays centred.
  const int icon = Scaled(kIconSize);
  int label_w = 0, label_h = 0;
  if (incognito) {
    HDC dc = ::GetDC(hwnd());
    if (dc) {
      HGDIOBJ prev = ::SelectObject(dc, LabelFont());
      RECT measure = {0, 0, 0, 0};
      PhiDrawTextAA(dc, PhiStr("browser.newTabPage.incognitoLabel"), -1,
                  &measure, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
      ::SelectObject(dc, prev);
      ::ReleaseDC(hwnd(), dc);
      label_w = measure.right - measure.left + 2;  // The Mac's clip guard.
      label_h = measure.bottom - measure.top;
    }
  }
  const int box_w = std::min(
      std::min(Scaled(kBoxWidth),
               std::max(clip_w - Scaled(kBoxHorizontalInset) * 2,
                        Scaled(kBoxMinWidth))),
      std::max(clip_w - Scaled(8), 1));
  const int box_h = Scaled(kBoxHeight);
  const int column_h = icon +
                       (incognito ? Scaled(kIconLabelGap) + label_h : 0) +
                       Scaled(kLabelBoxGap) + box_h;
  const int cx = clip_w / 2;
  int y = std::max(0, (clip_h - column_h) / 2);
  g.icon = {cx - icon / 2, y, cx - icon / 2 + icon, y + icon};
  y += icon;
  if (incognito) {
    y += Scaled(kIconLabelGap);
    g.label = {cx - label_w / 2, y, cx - label_w / 2 + label_w, y + label_h};
    y += label_h;
  }
  y += Scaled(kLabelBoxGap);
  g.box = {cx - box_w / 2, y, cx - box_w / 2 + box_w, y + box_h};
  return g;
}

void PhiNewTabPage::LayoutField() {
  if (!field_) {
    return;
  }
  const RECT box = ComputeGeometry().box;
  const int text_h = Scaled(kFieldHeight);
  const int left =
      box.left + Scaled(kGlyphLead) + Scaled(kGlyphSize) + Scaled(kGlyphTextGap);
  const int right = box.right - Scaled(kFieldTrail);
  ::SetWindowPos(field_, nullptr, left,
                 box.top + (box.bottom - box.top - text_h) / 2,
                 std::max(1, right - left), text_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
  ::ShowWindow(field_, controls_hidden_ ? SW_HIDE : SW_SHOW);
}

void PhiNewTabPage::Paint(HDC dc, const RECT&) {
  if (!controller_) {
    return;
  }
  RECT rc = {};
  ::GetClientRect(hwnd(), &rc);
  // QueryPalette(), not the base's cached palette(): the slot's colours
  // ramp between frames and this page follows them live. The cached one is
  // what the theme bus compares against to know it must repaint at all.
  const PhiPalette palette = QueryPalette();
  // contentOverlayBackground: the card's own ground, white in light mode.
  HBRUSH ground = ::CreateSolidBrush(palette.card);
  ::FillRect(dc, &rc, ground);
  ::DeleteObject(ground);
  if (controls_hidden_) {
    return;  // omniBoxDidClear: a blank card until the page unmounts.
  }
  const Geometry g = ComputeGeometry();
  ::SetBkMode(dc, TRANSPARENT);

  // The mark: the imageset's light variant is pure black, its dark one
  // #F8F8F8 — tinted from the shared mask rather than shipped twice.
  const PhiMacIcon& art = controller_->ShowsIncognito()
                              ? PhiNativeNtpIncognito()
                              : PhiNativeNtpLogo();
  const COLORREF ink =
      palette.dark ? RGB(0xF8, 0xF8, 0xF8) : RGB(0x00, 0x00, 0x00);
  const int icon_px = g.icon.right - g.icon.left;
  if (icon_px > 0) {
    const std::vector<unsigned char> bgra =
        PhiTintedIconBgra(art, ink, icon_px);
    PhiBlendBgra(dc, g.icon, bgra.data(), icon_px, icon_px);
  }

  if (controller_->ShowsIncognito()) {
    HGDIOBJ prev = ::SelectObject(dc, LabelFont());
    ::SetTextColor(dc, palette.label);
    RECT label = g.label;
    PhiDrawTextAA(dc, PhiStr("browser.newTabPage.incognitoLabel"), -1, &label,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SelectObject(dc, prev);
  }

  // The omnibox box: the Mac's contentOverlayBackground fill under a 1pt
  // black/white 20% border, continuous corners of radius 14.
  const COLORREF border = BlendInk(
      palette.dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00),
      palette.card, 51);
  PhiFillRoundRectAA(dc, g.box, Scaled(kBoxRadius), palette.card);
  PhiStrokeRoundRectAA(dc, g.box, Scaled(kBoxRadius), border, 1);
  {
    RECT glyph = {g.box.left + Scaled(kGlyphLead), g.box.top,
                  g.box.left + Scaled(kGlyphLead) + Scaled(kGlyphSize),
                  g.box.bottom};
    HGDIOBJ prev = ::SelectObject(dc, GlyphFont());
    ::SetTextColor(dc, palette.label_dim);  // secondaryLabelColor
    PhiDrawTextAA(dc, kSearchGlyph, -1, &glyph,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SelectObject(dc, prev);
  }
}

void PhiNewTabPage::OnMouseDown(int, POINT pt) {
  // A pane's page: the click makes its tab the active one first, as a
  // click on the pane's web content would (the omnibox commits there).
  if (tab_id_ && controller_) {
    controller_->ActivatePaneTab(tab_id_);
  }
  // The box's padding focuses the field like the Mac's box does; the
  // ground takes focus itself, which blurs the field and drops its
  // suggestion list.
  const RECT box = ComputeGeometry().box;
  if (WantsFocus() && ::PtInRect(&box, pt)) {
    ::SetFocus(field_);
    ::SendMessageW(field_, EM_SETSEL, 0, -1);
  } else {
    ::SetFocus(hwnd());
  }
}

LRESULT PhiNewTabPage::OnMessage(UINT message,
                                 WPARAM wparam,
                                 LPARAM lparam,
                                 bool* handled) {
  switch (message) {
    case WM_COMMAND:
      if (controller_ && field_ && reinterpret_cast<HWND>(lparam) == field_) {
        if (HIWORD(wparam) == EN_CHANGE) {
          *handled = true;
          controller_->OnOmniboxFieldChanged(field_);
          return 0;
        }
        if (HIWORD(wparam) == EN_SETFOCUS && tab_id_) {
          // The keyboard landed in a pane's omnibox (a click, Tab): that
          // pane's tab is where the session must commit.
          controller_->ActivatePaneTab(tab_id_);
        }
      }
      break;
    case WM_CTLCOLOREDIT:
      if (controller_ && reinterpret_cast<HWND>(lparam) == field_) {
        const PhiPalette colors = QueryPalette();
        HDC edit_dc = reinterpret_cast<HDC>(wparam);
        ::SetTextColor(edit_dc, colors.label);
        ::SetBkColor(edit_dc, colors.card);  // Seamless with the box fill.
        if (!field_brush_ || field_brush_color_ != colors.card) {
          if (field_brush_) {
            ::DeleteObject(field_brush_);
          }
          field_brush_ = ::CreateSolidBrush(colors.card);
          field_brush_color_ = colors.card;
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

}  // namespace phi_client

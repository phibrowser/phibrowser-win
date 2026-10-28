// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_floating_omnibox.h"

#include <commctrl.h>  // EM_SETCUEBANNER

#include <algorithm>

#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_omnibox_session.h"

namespace phi_client {

namespace {

// OmniBoxViewController / OmniBoxContainerViewController metrics (DIP).
constexpr int kCardWidth = 680;    // boxWidth.
constexpr int kCardHeight = 57;    // baseHeight: the field row alone.
constexpr int kCentringBand = 284;  // maxOmniBoxHeight: the box's top sits
                                    // where a full box would be centred.
constexpr int kEdgeMargin = 16;    // Never flush with the page's edges.
constexpr int kGlyphLeft = 16;     // The magnifier: 16 in, 16 wide …
constexpr int kGlyphSize = 16;
constexpr int kTextGap = 8;        // … and the text 8 after it,
constexpr int kTrailing = 12;      // 12 clear of the trailing edge.
constexpr int kFieldFontSize = 15;

}  // namespace

// static
PhiFloatingOmnibox* PhiFloatingOmnibox::Open(PhiOmniboxSession* session,
                                             HWND owner,
                                             const RECT& content) {
  auto* card = new PhiFloatingOmnibox(session);
  card->DeleteWhenDestroyed();
  if (!card->Create(owner, content)) {
    delete card;
    return nullptr;
  }
  return card;
}

PhiFloatingOmnibox::PhiFloatingOmnibox(PhiOmniboxSession* session)
    : session_(session) {}

PhiFloatingOmnibox::~PhiFloatingOmnibox() {
  if (brush_) {
    ::DeleteObject(brush_);
  }
}

bool PhiFloatingOmnibox::Create(HWND owner, const RECT& content) {
  ClassSpec spec;
  spec.name = L"PhiFloatingOmnibox";
  spec.cursor = IDC_IBEAM;

  Options options;
  // Escape belongs to the field (the session's FieldProc), which holds the
  // keys; the popover itself never has focus.
  options.dismiss_on_escape = false;

  // Created at a provisional size: the card is measured in its own dpi,
  // which exists once the window does (the toast's pattern).
  Placement placement;
  placement.side = PhiPopoverSide::kExplicit;
  placement.clamp = false;
  if (!CreatePopover(spec, owner, SIZE{1, 1}, placement, options)) {
    return false;
  }
  SetPalette(session_->CardPalette());

  // Centred over the page beside the sidebar; the top where a full box
  // would sit centred, so the list grows downward without moving the field
  // (the Mac's anchoredTop).
  const int content_width = content.right - content.left;
  const int content_height = content.bottom - content.top;
  const int width =
      std::max(Scaled(240), std::min<int>(Scaled(kCardWidth),
                                          content_width - 2 * Scaled(kEdgeMargin)));
  const int height = Scaled(kCardHeight);
  const LONG x = content.left + (content_width - width) / 2;
  const LONG y = content.top + std::max<int>(Scaled(kEdgeMargin),
                                             (content_height -
                                              Scaled(kCentringBand)) / 2);
  placement.anchor = {x, y, x, y};
  Reposition(placement, SIZE{width, height});

  field_ = ::CreateWindowExW(
      0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL, 0, 0,
      10, 10, hwnd(), nullptr, ::GetModuleHandleW(nullptr), nullptr);
  if (!field_) {
    Destroy();
    return false;
  }
  ::SendMessageW(field_, WM_SETFONT, reinterpret_cast<WPARAM>(FieldFont()),
                 FALSE);
  ::SendMessageW(field_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(
                     PhiStr("addressBar.configuration.defaultPlaceholder")));
  session_->AttachField(field_);
  Relayout();
  Show();
  ::SetFocus(field_);
  return true;
}

HFONT PhiFloatingOmnibox::FieldFont() {
  return fonts().Get(kFieldFontSize, FW_NORMAL);
}

void PhiFloatingOmnibox::Close() {
  if (!alive() || closing_) {
    return;
  }
  // Hidden now, destroyed a turn later: this runs from inside the field's
  // own key handler, which must not lose its window under it. Hiding the
  // active card deactivates it, and OnDismiss must not destroy (and so
  // delete) it there — hence closing_, set first.
  closing_ = true;
  const HWND window = hwnd();
  ::PostMessageW(window, WM_CLOSE, 0, 0);
  ::ShowWindow(window, SW_HIDE);
}

RECT PhiFloatingOmnibox::TextArea() const {
  RECT rc = {};
  ::GetClientRect(hwnd(), &rc);
  rc.left += Scaled(kGlyphLeft + kGlyphSize + kTextGap);
  rc.right -= Scaled(kTrailing);
  return rc;
}

void PhiFloatingOmnibox::Relayout() {
  if (!field_ || !session_) {
    return;
  }
  RECT area = TextArea();
  area.left += session_->ChipAdvance(field_);
  area.right -= session_->HintAdvance(field_);
  // A single-line EDIT draws at its own top: give it one line's height,
  // centred in the card.
  HDC dc = ::GetDC(field_);
  HGDIOBJ previous = ::SelectObject(dc, FieldFont());
  TEXTMETRICW metrics = {};
  ::GetTextMetricsW(dc, &metrics);
  ::SelectObject(dc, previous);
  ::ReleaseDC(field_, dc);
  const int line = metrics.tmHeight;
  const int top = (area.top + area.bottom - line) / 2;
  ::MoveWindow(field_, area.left, top,
               std::max<int>(1, area.right - area.left), line, TRUE);
  Invalidate();
}

void PhiFloatingOmnibox::Paint(HDC dc, const RECT& dirty) {
  RECT rc = {};
  ::GetClientRect(hwnd(), &rc);
  const PhiPalette& colors = palette();
  HBRUSH fill = ::CreateSolidBrush(colors.card);
  ::FillRect(dc, &rc, fill);
  ::DeleteObject(fill);
  // No rim: DWM rounds the card and draws its border and shadow.
  ::SetBkMode(dc, TRANSPARENT);
  RECT glyph = {Scaled(kGlyphLeft), rc.top,
                Scaled(kGlyphLeft + kGlyphSize), rc.bottom};
  HGDIOBJ previous = ::SelectObject(
      dc, fonts().Get(PhiFontDesc{kGlyphSize, FW_NORMAL, PhiIconFaceName()}));
  ::SetTextColor(dc, colors.label_dim);
  PhiDrawTextAA(dc, L"", 1, &glyph,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  ::SelectObject(dc, previous);
  if (session_) {
    session_->PaintKeywordIn(dc, TextArea(), colors, field_);
  }
}

void PhiFloatingOmnibox::OnSize(int width, int height) {
  Relayout();
}

void PhiFloatingOmnibox::OnDpiChanged(UINT old_dpi) {
  if (field_) {
    ::SendMessageW(field_, WM_SETFONT, reinterpret_cast<WPARAM>(FieldFont()),
                   TRUE);
  }
  Relayout();
}

void PhiFloatingOmnibox::OnDismiss() {
  // The user went elsewhere (a click outside, another app): the card goes
  // and the session drops what it was doing for it (the Mac hides on a
  // click outside or a focus change).
  if (closing_) {
    return;  // Close() already posted the WM_CLOSE that destroys it.
  }
  if (session_) {
    session_->OnFloatingDismissed();  // Closes the card (Close()) …
  }
  if (!closing_) {
    PhiPopover::OnDismiss();  // … or, with no session, the base does.
  }
}

void PhiFloatingOmnibox::OnNcDestroy() {
  if (session_) {
    session_->OnFloatingDestroyed(this);
    session_ = nullptr;
  }
  field_ = nullptr;
}

LRESULT PhiFloatingOmnibox::OnMessage(UINT message,
                                      WPARAM wparam,
                                      LPARAM lparam,
                                      bool* handled) {
  switch (message) {
    case WM_CTLCOLOREDIT:
      if (reinterpret_cast<HWND>(lparam) == field_) {
        // The EDIT on the card's own body, in its text colour.
        const PhiPalette& colors = palette();
        HDC edit_dc = reinterpret_cast<HDC>(wparam);
        ::SetTextColor(edit_dc, colors.label);
        ::SetBkColor(edit_dc, colors.card);
        if (!brush_ || brush_color_ != colors.card) {
          if (brush_) {
            ::DeleteObject(brush_);
          }
          brush_ = ::CreateSolidBrush(colors.card);
          brush_color_ = colors.card;
        }
        *handled = true;
        return reinterpret_cast<LRESULT>(brush_);
      }
      break;
    case WM_COMMAND:
      if (reinterpret_cast<HWND>(lparam) == field_ &&
          HIWORD(wparam) == EN_CHANGE && session_) {
        session_->OnFieldChanged(field_);  // The user typed: autocomplete.
        *handled = true;
        return 0;
      }
      break;
    default:
      break;
  }
  return PhiPopover::OnMessage(message, wparam, lparam, handled);
}

}  // namespace phi_client

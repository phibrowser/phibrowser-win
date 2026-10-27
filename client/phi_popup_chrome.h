// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_POPUP_CHROME_H_
#define PHIBROWSER_WIN_CLIENT_PHI_POPUP_CHROME_H_

#include <windows.h>

#include <string>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_theme.h"

namespace phi_client {

// The look every secondary window shares — the Mac's secondary windows
// (Feedback, PhiAlert, the transcript, Settings) stand on the FIXED window
// ground, `DefaultColors.windowBackground` (#EAEAEA light / #252525 dark),
// never on the per-Space tint. Cards and fields are the Mac's
// controlBackgroundColor over it with an 8% border; text is the
// textPrimary / textSecondary / textTertiary ladder. Buttons are
// PhiAlertButton's three roles: the theme accent, calibratedWhite
// 0.88 / 0.24, and systemRed — each with its hover twin.
struct PhiPopupColors {
  bool dark = false;
  COLORREF bg = RGB(0xEA, 0xEA, 0xEA);
  COLORREF ink = RGB(0, 0, 0);
  COLORREF label = RGB(0x26, 0x26, 0x26);         // textPrimary
  COLORREF label_strong = RGB(0, 0, 0);            // textPrimaryStrong
  COLORREF secondary = RGB(0x66, 0x66, 0x66);      // textSecondary
  COLORREF tertiary = RGB(0xA0, 0xA0, 0xA0);       // textTertiary
  COLORREF card = RGB(0xFF, 0xFF, 0xFF);           // controlBackgroundColor
  COLORREF card_border = RGB(0xD7, 0xD7, 0xD7);    // border 8%
  COLORREF separator = RGB(0xDC, 0xDC, 0xDC);      // separator 6%
  COLORREF well = RGB(0xE0, 0xE0, 0xE0);           // ink 4% — segment tracks.
  COLORREF hover = RGB(0xE0, 0xE0, 0xE0);          // Caption-button hover.
  COLORREF accent = RGB(0x3A, 0xA4, 0xD5);
  COLORREF accent_hover = RGB(0x24, 0x8A, 0xB9);
  COLORREF button = RGB(0xE0, 0xE0, 0xE0);         // .secondary
  COLORREF button_hover = RGB(0xD1, 0xD1, 0xD1);
  COLORREF destructive = RGB(0xFF, 0x3B, 0x30);    // systemRed
  COLORREF destructive_hover = RGB(0xE0, 0x34, 0x2A);
  COLORREF warning = RGB(0xB0, 0x80, 0x00);        // Counters near a limit.
  COLORREF error = RGB(0xC0, 0x1E, 0x1E);
};

// Resolves the ladder above for one appearance and accent.
PhiPopupColors PhiPopupColorsFor(bool dark, COLORREF accent);
// The app-wide answer: the appearance and accent of the palette registered
// below, else the system appearance with the default accent.
PhiPopupColors PhiPopupColorsNow();
// The coordinator registers its live app palette once; every popup then
// follows theme and appearance changes without being told.
void PhiSetPopupPalette(const PhiPalette* live);

// `over` at alpha/255 on `base` — the Mac's percentage overlays, opaque.
COLORREF PhiPopupBlend(COLORREF over, COLORREF base, int alpha);
// HSB -> COLORREF (the Mac's NSColor(hue:saturation:brightness:)); every
// component 0..1, the hue wrapping.
COLORREF PhiHsbColor(double hue, double saturation, double brightness);
// The hue (0..1) of a colour; 0 for a grey.
double PhiHueOf(COLORREF color);
void PhiPopupFillRounded(HDC dc, const RECT& r, COLORREF fill, int radius);
void PhiPopupStrokeRounded(HDC dc,
                           const RECT& r,
                           COLORREF fill,
                           COLORREF border,
                           int radius,
                           int width = 1);
// Segoe UI at `size` design points for `dpi`; caller owns the font.
HFONT PhiPopupFont(UINT dpi, int size, int weight = FW_NORMAL);
// The window controls' glyph face (Segoe MDL2 Assets) at the OS caption
// size; caller owns the font.
HFONT PhiPopupGlyphFont(UINT dpi);

// PhiAlertButton's roles, plus the inline text link the Feedback notice
// uses. A BUTTON control made owner-drawn through PhiPopupStyleButton is
// painted by PhiPopupDrawButton from the parent's WM_DRAWITEM.
enum class PhiPopupButtonRole { kPrimary, kSecondary, kDestructive, kLink };
void PhiPopupStyleButton(HWND button, PhiPopupButtonRole role);
bool PhiPopupDrawButton(const DRAWITEMSTRUCT& item,
                        const PhiPopupColors& colors,
                        HFONT font);
// Paints the same button directly (drawn buttons that are not controls).
void PhiPopupPaintButton(HDC dc,
                         const RECT& bounds,
                         const std::wstring& text,
                         PhiPopupButtonRole role,
                         bool hot,
                         bool pressed,
                         bool enabled,
                         const PhiPopupColors& colors,
                         HFONT font,
                         UINT dpi);
// A field or list well: the Mac's controlBackgroundColor card with its
// separator stroke, radius 8. The native EDIT / LISTBOX sits inset inside.
void PhiPopupPaintWell(HDC dc,
                       const RECT& bounds,
                       const PhiPopupColors& c,
                       UINT dpi);
// Gives a native EDIT / LISTBOX the dark scrollbars and theme of the
// appearance so it does not glare inside a dark popup.
void PhiPopupThemeControl(HWND control, bool dark);

// Which window controls the caption band shows, trailing, in the Windows
// order: minimize, maximize, close.
enum PhiPopupCaptionButtons : unsigned {
  kPhiCaptionClose = 1u << 0,
  kPhiCaptionMinimize = 1u << 1,
  kPhiCaptionMaximize = 1u << 2,
};

// The custom frame shared by every secondary window: the OS caption is
// retired (WM_NCCALCSIZE keeps only the thin side/bottom borders) and a
// 32dip band at the top of the client area carries the window title —
// the Mac look — with Windows' own min / max / close cluster in the right
// corner (46dip buttons, Win11's metric; close turns red when hot). The
// owner's window procedure hands every message to HandleMessage first and
// paints the band through PaintCaption at the top of its own WM_PAINT.
class PhiPopupChrome {
 public:
  PhiPopupChrome();
  ~PhiPopupChrome();
  PhiPopupChrome(const PhiPopupChrome&) = delete;
  PhiPopupChrome& operator=(const PhiPopupChrome&) = delete;

  // Attaches to a created window: applies the frame, the DWM corner and
  // border treatment for `colors`, and the taskbar icon.
  void Attach(HWND hwnd, unsigned buttons, const PhiPopupColors& colors);
  // Re-applies the DWM border / dark-mode attributes after a theme flip.
  void ApplyColors(const PhiPopupColors& colors);

  // The band's height in DESIGN units. 32 is the Mac secondary-window
  // look; the Kiosk's band is its 52dip toolbar and title bar at once.
  // Set before Attach().
  void SetCaptionHeight(int design_units);
  // Whether the title and the window-control glyphs dim to `tertiary`
  // while the window is inactive. Settings and the Kiosk never dimmed
  // theirs, and the migration to this frame must not start.
  void SetDimWhenInactive(bool dim);
  // A band pixel the OWNER claims: WM_NCHITTEST answers HTCLIENT for it
  // instead of HTCAPTION, so the owner's own band controls take their
  // clicks rather than dragging the window. A plain function pointer,
  // not a std::function: the frame stores no state it must destroy, and
  // the house rules allow no friend.
  using ClientProbe = bool (*)(void* context, POINT client_pt);
  void SetClientProbe(ClientProbe probe, void* context);

  UINT dpi() const { return dpi_; }
  int Scaled(int design) const;
  int CaptionHeight() const;  // Pixels.
  // The window's outer size for `content_w` x `content_h` design units of
  // client area BELOW the band.
  SIZE OuterSize(int content_w, int content_h) const;
  // The client rect below the band.
  RECT ContentRect() const;

  // True when the message was consumed; `*result` then carries the answer.
  // WM_DPICHANGED and WM_MOUSEMOVE update the chrome and return false so the
  // owner still sees them.
  bool HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam, LRESULT* result);
  // Fills the band and draws the title (centred; the Phi mark leads when
  // `show_mark`) and the window controls. `title` null reads the window
  // text; empty draws no title.
  void PaintCaption(HDC dc,
                    const PhiPopupColors& colors,
                    bool show_mark,
                    const wchar_t* title = nullptr);
  // The window controls ONLY, over a band the owner already painted (the
  // Kiosk's toolbar band is title bar and toolbar at once).
  void PaintCaptionButtons(HDC dc, const PhiPopupColors& colors);

 private:
  void EnsureFonts();
  RECT ButtonRect(int hit_code) const;  // Empty when that button is absent.
  int HitTestCaption(POINT client_pt) const;
  void SetHot(int hit_code);
  void InvalidateBand();
  // The two per-paint brushes PaintCaption used to create and delete on
  // every frame (lane 3 ranked finding 12), cached by colour.
  HBRUSH GroundBrush(COLORREF color);
  HBRUSH HotBrush(COLORREF color);

  HWND hwnd_ = nullptr;
  unsigned buttons_ = kPhiCaptionClose;
  UINT dpi_ = 96;
  UINT font_dpi_ = 0;
  int caption_height_ = 32;  // Design units.
  bool dim_when_inactive_ = true;
  ClientProbe client_probe_ = nullptr;
  // The probe's owner, which creates the chrome and outlives it.
  RAW_PTR_EXCLUSION void* client_probe_context_ = nullptr;
  HFONT title_font_ = nullptr;
  HFONT glyph_font_ = nullptr;
  HBRUSH ground_brush_ = nullptr;
  COLORREF ground_color_ = 0;
  HBRUSH hot_brush_ = nullptr;
  COLORREF hot_color_ = 0;
  int hot_ = 0;      // HTMINBUTTON / HTMAXBUTTON / HTCLOSE, or 0.
  int pressed_ = 0;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_POPUP_CHROME_H_

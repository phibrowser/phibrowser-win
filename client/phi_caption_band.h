// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_CAPTION_BAND_H_
#define PHIBROWSER_WIN_CLIENT_PHI_CAPTION_BAND_H_

#include <windows.h>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_shell_layout.h"
#include "client/phi_theme.h"

// The shell window's CAPTION BAND (docs/architecture-review.md §3.5, lane 1
// cluster C; docs/windows-native-ui.md §10 P1): the row that replaced the
// OS caption. It owns
//
//   * the custom frame itself: WM_NCCALCSIZE, WM_NCHITTEST and the
//     WM_NC*BUTTON* / WM_NCMOUSE* family,
//   * which caption button is hot and which is pressed, and their paint.
//
// The menu bar that used to live here is gone: the app menu (⋯,
// phi_app_menu.h) is the shell's own control. The shell forwards messages
// here and asks the band to paint its rect; it keeps the window, the fonts,
// the palette and the layout, which the band reads back through
// `PhiCaptionShell`. There is no `friend` in either direction.

namespace phi_client {

// The band's geometry for one turn, taken from the shell's Layout.
struct PhiCaptionRects {
  PhiLayoutMode mode = PhiLayoutMode::kBalanced;
  RECT title_bar{};
  RECT caption_min{}, caption_max{}, caption_close{};
  // The hairline closing the band's trailing cluster.
  RECT band_divider{};
  // The band's own controls: they take their own clicks, so the drag space
  // must not swallow them.
  RECT back{}, forward{}, reload{}, search_tabs{}, app_menu{};
  // The Update reminder pill, before search (empty without an update).
  RECT update{};
  // The vertical layout's leading sidebar toggle.
  RECT sidebar_toggle{};
  // The rest of the address row (vertical layout, where the row IS the
  // band): the omnibox capsule with its in-field slots (Reader, Copy link, the site menu),
  // the agent chip, the pinned-extension strip and the puzzle button. Only
  // the drag gutter between the field and the cluster drags the window.
  RECT url_field{}, agent_chip{}, field_ext_strip{}, extensions{};
  // The sidebar's header row (vertical layout): the pips take their own
  // clicks there; the empty run drags the window.
  RECT spaces_strip{};
  // Comfortable drops its caption glyphs onto the tab strip's line.
  int tabs_top = 0;
  bool horizontal_tabs = false;
};

// What the band asks of the shell that owns it.
class PhiCaptionShell {
 public:
  virtual HWND CaptionShellWindow() const = 0;
  virtual int CaptionScaled(int design_units) const = 0;
  virtual UINT CaptionDpi() const = 0;
  // The icon face at the OS caption size, for the three caption glyphs.
  virtual HFONT CaptionGlyphFont() const = 0;
  virtual PhiPalette CaptionPalette() const = 0;
  virtual PhiCaptionRects CaptionRects() = 0;
  // Comfortable's band IS the tab strip: everything the strip drew there
  // takes its own clicks, and only the bare run past it drags the window.
  virtual bool CaptionStripTakesPoint(POINT client_pt) = 0;
  // The pointer entered the band: nothing the sidebar was washing is under
  // it any more.
  virtual void CaptionClearShellHover() = 0;
  // Fullscreen claims every pixel as client, borders included.
  virtual bool CaptionContentFullscreen() const = 0;

 protected:
  // The shell owns the band, never the reverse.
  ~PhiCaptionShell() = default;
};

class PhiCaptionBand {
 public:
  explicit PhiCaptionBand(PhiCaptionShell* shell);
  ~PhiCaptionBand();

  PhiCaptionBand(const PhiCaptionBand&) = delete;
  PhiCaptionBand& operator=(const PhiCaptionBand&) = delete;

  void Install(HWND shell_hwnd);

  // Paints the band's own contents over the ground the shell already laid:
  // the trailing hairline and the three caption buttons — those only when
  // `with_buttons`.
  void Paint(HDC dc, bool with_buttons = true);
  // The three caption buttons alone (their hot fill and glyphs, no ground):
  // the comfortable Space slide paints them last, standing still over the
  // chrome sliding under them.
  void PaintButtons(HDC dc);

  // What a CLIENT point in the band hits: HTCLOSE / HTMAXBUTTON /
  // HTMINBUTTON, HTCLIENT over a control, HTCAPTION anywhere else in the
  // band, 0 below it.
  int HitTest(POINT client_pt);

  // --- The custom frame ----------------------------------------------------
  // Each returns true when it handled the message (`result` then carries the
  // return value where one is needed).
  bool OnNcCalcSize(HWND hwnd, WPARAM wparam, LPARAM lparam, LRESULT* result);
  bool OnNcHitTest(HWND hwnd, LPARAM lparam, LRESULT* result);
  // A press on a caption button CAPTURES the mouse until the release, so
  // the release lands here wherever it happens (OnLButtonUp, a client
  // message under capture) and a press that never sees its release cannot
  // stay armed: losing the capture (WM_CAPTURECHANGED, WM_CANCELMODE)
  // disarms it, as does any other non-client press.
  bool OnNcLButtonDown(WPARAM hit);
  // Minimize and close hit-test as client area (HTMINBUTTON / HTCLOSE would
  // bring up the system's own tooltip), so their press arrives as a client
  // one. True when it landed on one of them.
  bool OnLButtonDown(POINT client_pt);
  bool OnNcLButtonUp(HWND hwnd, WPARAM hit);
  // True when the release belonged to a caption press (the shell then
  // handles nothing else for it).
  bool OnLButtonUp(POINT client_pt);
  void OnCaptureLost();
  // The non-client move: the caption buttons' hover (most of the band
  // hit-tests as HTCAPTION, so the pointer crossing it never arrives as
  // WM_MOUSEMOVE).
  void OnNcMouseMove(WPARAM hit, POINT client_pt);
  void OnNcMouseLeave();
  // The client-area mirrors of the two above.
  void OnMouseMove(POINT client_pt);
  void OnMouseLeave();

 private:
  void SetCaptionHot(int code);
  void InvalidateBand();

  RAW_PTR_EXCLUSION PhiCaptionShell* const shell_;  // Owns this.
  HWND shell_hwnd_ = nullptr;
  // Which caption button is hot / pressed (an HT* code, 0 = none). Purely
  // visual except `pressed_caption_`, which arms the button-up action.
  int hot_caption_ = 0;
  int pressed_caption_ = 0;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CAPTION_BAND_H_

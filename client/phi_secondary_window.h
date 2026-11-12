// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SECONDARY_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SECONDARY_WINDOW_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "client/phi_window.h"

#if defined(_WIN32)
#include <windows.h>

#include "client/phi_popup_chrome.h"
#endif

// The SECONDARY-WINDOW family (docs/architecture-review.md §3.6, appendix
// lane3-ui-substrate.md §6.2). A secondary window is a top-level window
// that is NOT the shell: the prompt, the Feedback form, Settings and the
// Kiosk. What they share, and what
// this family owns for them:
//
//  1. ONE CAPTION FRAME. Lane 3 §2 found the custom frame implemented
//     four times — the shell (not this family's), the kiosk
//     (phi_kiosk_window.cc:919-1001), Settings
//     (phi_settings_window.cc:3552-3627) and PhiPopupChrome. Only the
//     last was reusable, so it is the one that survives: this family
//     owns a PhiPopupChrome, gives it first refusal on every message,
//     and the two in-proc copies are deleted. A window whose band is
//     TALLER than the 32dip default (the kiosk's 52dip toolbar band) or
//     that paints the band itself only says so in Options.
//  2. COLOURS FROM THE THEME BUS. The five windows snapshotted
//     PhiPopupColorsNow() at construction and never refreshed (lane 3
//     ranked finding 7): an OS dark/light switch left an open Feedback
//     window on the old ladder forever. PhiWindow
//     registers every window with PhiThemeBus, so a broadcast lands in
//     OnThemeChanged(); this family re-reads QueryColors(), re-applies
//     the DWM attributes and repaints.
//  3. DPI. The base owns dpi() and the PhiFontSet; the feedback form
//     kept its pre-move fonts after WM_DPICHANGED
//     (lane 3 ranked finding 10) purely because it built its fonts
//     once. Any font taken from fonts() is rebuilt for them.
//  4. DIALOG KEYS WITHOUT A NESTED LOOP. A window with native child
//     controls needs IsDialogMessage for Tab (and optionally Escape /
//     Return / arrows) to work at all — and the client's main message
//     loop is Chromium's, which this repo does not own. Rather than
//     spinning a nested GetMessage loop per window (lane 3 §4.4 counted
//     three of those; two are already gone), a window REGISTERS itself
//     in a thread-local "active dialog" list and one WH_GETMESSAGE hook
//     on the UI thread runs IsDialogMessageW for it inside the main
//     loop, blanking the message it consumed. This is the MFC/ATL
//     `PreTranslateMessage` shape, without the loop.
//
// THE ONE WINDOW THAT MAY STILL BLOCK is the prompt (phi_prompt.h says
// why): PhiAlert / PhiConfirm / PhiPromptText / PhiPromptChoose /
// PhiPromptShortcut are called from a dozen places that need the answer
// on the same stack. It keeps its own audited modal loop and registers
// NO dialog keys here — its loop already calls IsDialogMessageW, and its
// chord recorder must see raw key messages BEFORE the dialog manager
// does.

namespace phi_client {

// ---------------------------------------------------------------------------
// Pure caption geometry — THE hit-test and button layout of the custom
// frame, shared by PhiPopupChrome and pinned by test/model/.
//
// Design metric: Win11's caption buttons are 46 wide and as tall as the
// band; they sit at the trailing edge in the system order minimize,
// maximize, close (close outermost).

inline constexpr int kPhiCaptionHeightDip = 32;
inline constexpr int kPhiCaptionButtonWidthDip = 46;

enum class PhiCaptionPart {
  kOutside,   // Below the band entirely — the window's own client area.
  kCaption,   // The draggable band.
  kMinimize,
  kMaximize,
  kClose,
};

// One band, in DEVICE pixels. `close` / `maximize` / `minimize` say which
// buttons the band shows; absent ones take no room.
struct PhiCaptionMetrics {
  int width = 0;         // Client width.
  int height = 0;        // Band height.
  int button_width = 0;  // One caption button.
  bool close = true;
  bool maximize = false;
  bool minimize = false;
};

// The band rect for one button, or an empty rect when that button is
// absent. kCaption / kOutside are never buttons and answer empty.
PhiRectI PhiCaptionButtonBounds(const PhiCaptionMetrics& metrics,
                                PhiCaptionPart part);

// What is at (x, y) in client coordinates. Above/below the band answers
// kOutside; inside it, a button or kCaption.
PhiCaptionPart PhiCaptionHitTest(const PhiCaptionMetrics& metrics,
                                 int x,
                                 int y);

// The horizontal room the band must keep clear on BOTH sides so a centred
// title clears the leading mark and the trailing button cluster — the
// title is centred on the window, so the wider of the two wins.
int PhiCaptionTitleInset(const PhiCaptionMetrics& metrics, int leading);

// ---------------------------------------------------------------------------
// Pure placement: where a secondary window opens. Device px, any origin —
// a monitor left of or above the primary has negative coordinates and is
// no special case.
//
// `frame` moved the least distance that puts it wholly inside `work`, after
// being cut down to no wider and no taller than `work`: a dialog designed
// taller than the screen still shows its footer above the taskbar.
PhiRectI PhiFitRectInto(const PhiRectI& frame, const PhiRectI& work);
// A `width` x `height` frame centred across `anchor`, one third down (how
// all of the family open), then fitted into `work`.
PhiRectI PhiPlaceCentered(int width,
                          int height,
                          const PhiRectI& anchor,
                          const PhiRectI& work);

// ---------------------------------------------------------------------------
// Pure dialog-list decisions (see contract point 4).
//
// Virtual-key numbers, spelled here so the pure half needs no windows.h.
inline constexpr unsigned kPhiVkTab = 0x09;
inline constexpr unsigned kPhiVkReturn = 0x0D;
inline constexpr unsigned kPhiVkEscape = 0x1B;
inline constexpr unsigned kPhiVkLeft = 0x25;
inline constexpr unsigned kPhiVkUp = 0x26;
inline constexpr unsigned kPhiVkRight = 0x27;
inline constexpr unsigned kPhiVkDown = 0x28;

// Which keys a window hands to the dialog manager. Deliberately a mask
// and not "all of them": IsDialogMessageW's Escape posts IDCANCEL and its
// Return presses the default button, and the feedback form's Escape and
// Ctrl+Return already mean something else. A window opts in to exactly
// the manners it wants and keeps its own handling of the rest.
enum PhiDialogKeys : unsigned {
  kPhiDialogNone = 0,
  kPhiDialogTab = 1u << 0,     // Tab / Shift+Tab move the focus.
  kPhiDialogEscape = 1u << 1,  // Escape posts WM_COMMAND IDCANCEL.
  kPhiDialogEnter = 1u << 2,   // Return presses the default button.
  kPhiDialogArrows = 1u << 3,  // Arrows walk a radio group.
};

// The message kinds the hook can see; everything else is kOther and is
// never handed to the dialog manager.
enum class PhiDialogMessageKind { kOther, kKeyDown, kSysKeyDown, kChar };

// True when a window registered with `keys` wants `virtual_key` of `kind`
// handed to IsDialogMessage. WM_CHAR is included for the keys the dialog
// manager consumes as WM_KEYDOWN, so the EDIT never gets the character
// (and never beeps) for a key that moved the focus.
bool PhiDialogWantsKey(unsigned keys,
                       PhiDialogMessageKind kind,
                       unsigned virtual_key);

// Which registered dialog owns a message. `ancestors` is the target
// window then its parents, INNERMOST FIRST; `roots` is the registration
// list in any order. Returns the index into `roots` of the innermost
// registered ancestor, or -1 when the message belongs to no dialog. The
// innermost wins so a secondary window opened over another one takes its
// own keys.
int PhiDialogOwner(const std::vector<uintptr_t>& roots,
                   const std::vector<uintptr_t>& ancestors);

#if defined(_WIN32)

// ---------------------------------------------------------------------------
// The thread's active-dialog list.
//
// PhiSecondaryWindow drives these; the prompt's modal loop deliberately
// does not join. The WH_GETMESSAGE hook is installed on the first
// registration and removed with the last, so a client with no secondary
// window open pays nothing.
void PhiRegisterDialogWindow(HWND hwnd, unsigned keys);
void PhiUnregisterDialogWindow(HWND hwnd);
// True when the message was consumed by a registered dialog; the caller
// must then not translate or dispatch it. Exposed for the hook and for
// tests; ordinary code never calls it.
bool PhiFilterDialogMessage(MSG* msg);
// How many windows are registered right now (diagnostics / tests).
size_t PhiActiveDialogCount();

// ---------------------------------------------------------------------------

class PhiSecondaryWindow : public PhiWindow {
 public:
  struct Options {
    // Which window controls the band carries (PhiPopupCaptionButtons).
    unsigned caption_buttons = kPhiCaptionClose;
    // Band height in DESIGN units. 32 is the Mac secondary-window look;
    // the Kiosk's band is its 52dip toolbar and title bar at once.
    int caption_height_dip = kPhiCaptionHeightDip;
    // The Phi mark leads the band.
    bool show_mark = true;
    // The band's title dims to `tertiary` while the window is inactive.
    // False for the two windows that never dimmed it (Settings, Kiosk),
    // so the migration is pixel-identical.
    bool dim_when_inactive = true;
    // Keys handed to the dialog manager from the main loop; 0 registers
    // nothing.
    unsigned dialog_keys = kPhiDialogNone;
  };

  PhiSecondaryWindow();
  ~PhiSecondaryWindow() override;

  // Creates the window through PhiWindow::Create, attaches the caption
  // frame and joins the dialog list. `owner` may be null (Feedback,
  // Settings and the Kiosk are ownerless top-level windows).
  HWND CreateSecondary(const ClassSpec& spec,
                       DWORD ex_style,
                       DWORD style,
                       const RECT& bounds,
                       HWND owner,
                       const wchar_t* title,
                       const Options& options);

  PhiPopupChrome& chrome() { return chrome_; }
  const PhiPopupChrome& chrome() const { return chrome_; }
  const PhiPopupColors& colors() const { return colors_; }
  // Band height in device pixels.
  int CaptionHeight() const;
  // The client rect below the band.
  RECT ContentRect() const;
  // Centres the window over `around` (the work area of the owner's
  // monitor, else the pointer's, when null) at its current size, one third
  // down — how all five open — kept inside that monitor's work area and cut
  // down to fit it.
  void CenterOn(HWND around);
  // The work area of the monitor the window is on, in screen px.
  RECT WorkArea() const;
  // Moves the window back inside WorkArea(), shrinking it when it is
  // taller or wider (a DPI move, a display or taskbar change). A maximized
  // or minimized window is left alone.
  void FitToWorkArea();

  // PhiPopupChrome's client probe calls this and nothing else does: the
  // frame asks, through a plain function pointer, whether the band pixel
  // under the cursor belongs to the window rather than to the caption.
  // Public because the frame is not a friend (house rule); override
  // CaptionClaimsClient() instead of calling this.
  bool CaptionClaimsClientForFrame(POINT client_pt) const;

 protected:
  // Where this window's popup colours come from. The default is the
  // app-wide answer; Settings and the Kiosk read a live
  // palette instead.
  virtual PhiPopupColors QueryColors() const;
  // colors() moved (a theme bus broadcast, or a palette push). The base
  // has already re-applied the DWM attributes and invalidated; rebuild
  // brushes here.
  virtual void OnColorsChanged() {}
  // The band region at `client_pt` belongs to the SUBCLASS, not to the
  // frame: WM_NCHITTEST answers HTCLIENT so the click reaches the
  // window instead of dragging it. The Kiosk's toolbar controls live in
  // its band and are the only user.
  virtual bool CaptionClaimsClient(POINT client_pt) const;
  // The pointer moved onto the non-client frame or the band, so nothing
  // in the client area is hovered any more.
  virtual void OnFramePointer() {}

  // Paints the band at the top of the subclass's Paint(). `title` null
  // reads the window text; empty draws no title.
  void PaintBand(HDC dc, const wchar_t* title = nullptr);
  // The window controls ONLY, on a band the subclass painted itself
  // (the Kiosk's toolbar).
  void PaintCaptionButtons(HDC dc);
  // Re-reads QueryColors() and repaints. Call when the source palette
  // moved for a reason the theme bus does not know about.
  void RefreshColors();

  // The frame gets FIRST refusal on every message — WM_NCCALCSIZE,
  // WM_NCHITTEST, the window controls' press/hover, and the WM_MOUSEMOVE
  // that means "the pointer left them". Subclasses therefore override
  // OnWindowMessage(), never this: a subclass that answered WM_MOUSEMOVE
  // ahead of the frame would leave a caption button lit.
  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) final;
  // What a subclass overrides. Same contract as PhiWindow::OnMessage:
  // set *handled and return a result to stop the base handling it.
  virtual LRESULT OnWindowMessage(UINT message,
                                  WPARAM wparam,
                                  LPARAM lparam,
                                  bool* handled);
  void OnThemeChanged() override;
  void OnDpiChanged(UINT old_dpi) override;
  void OnNcDestroy() override;

  const Options& options() const { return options_; }

 private:
  PhiPopupChrome chrome_;
  PhiPopupColors colors_;
  Options options_;
  bool dialog_registered_ = false;
};

#endif  // defined(_WIN32)

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SECONDARY_WINDOW_H_

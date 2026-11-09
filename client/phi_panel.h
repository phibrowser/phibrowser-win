// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_PANEL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_PANEL_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_window.h"

#if defined(_WIN32)
#include <windows.h>
#endif

// The PANEL family (docs/architecture-review.md §3.6, appendix
// lane3-ui-substrate.md §6.2). A panel is a WS_CHILD surface over the
// shell's CONTENT CARD: the bookmark manager, the native new-tab page, the
// create-Space form, the group overview, the peek card / controls / shadow,
// the reader surface, the agent fog / pill / sprite and the split-drop
// hint cards. Twelve classes with twelve registrations, five `this`
// bindings, four copies of the double-buffer block and nine ad-hoc
// SetWindowPos(HWND_TOP) calls become one base plus twelve models.
//
// WHAT THE FAMILY OWNS, on top of everything PhiWindow already owns
// (registration, `this` at WM_NCCREATE and cleared at WM_NCDESTROY, the
// cached back buffer, dpi, fonts, hover over hits_, timers, capture — read
// phi_window.h's contract first):
//
//  1. CHILD STYLES. WS_CHILD | WS_CLIPSIBLINGS always. CLIPSIBLINGS is not
//     optional: a panel covers the content card and the corner caps are
//     LAYERED SIBLINGS stacked above it, so a panel that paints past them
//     squares off the card's rounded corners.
//  2. THE PALETTE comes from the slot the panel hangs over, through
//     QueryPalette(); the theme bus repaints every panel when the app or
//     the OS appearance moves. No panel snapshots colours any more.
//  3. Z-ORDER through ONE PhiCardStack per parent, never a bare
//     SetWindowPos(HWND_TOP). The stack is the raise-caps contract's other
//     half: the shell answers WM_PHI_RAISE_CAPS by lifting the page-level
//     surfaces and then the caps above them; the stack is what decides the
//     order AMONG the panels, and it is "last raised is topmost" — the
//     same rule the nine scattered calls implemented between them, now
//     written down and testable (PhiCardStackModel, below).
//  4. SHAPED LAYERS for the surfaces that host NO child window. A window
//     region (SetWindowRgn) cuts a shape on the pixel grid and every
//     corner it makes is stair-stepped; a WS_EX_LAYERED child carries
//     per-pixel alpha instead. See
//     ~/.claude/projects/-home-jx-Phi/memory/phi-win-antialiasing.md. A
//     panel that HOSTS a child (the peek card's reparented browser, the
//     new-tab page's EDIT) cannot be a layer: it takes a corner FRINGE
//     (SetCornerFringe) — a region of only the pixels the curve covers
//     fully, plus the arc's partial pixels on layered siblings beneath it.
//     A panel over the whole content card (the reader, the page panels) is
//     rounded by the shell's own corner caps above it and cuts nothing.
//
// A PANEL IS NOT SELF-OWNED. The controller and the surface registry own
// these objects; a subclass therefore calls Destroy() as the FIRST thing
// in its own destructor body, while its vtable and its members are still
// live. (The three self-owned ones — the group overview, the agent
// overlay's three windows and the split-drop cards — use the base's
// DeleteWhenDestroyed() instead and never `delete this` from a wndproc.)

namespace phi_client {

// ---------------------------------------------------------------------------
// Pure: the card stack's z-order model.
//
// Lane 3 §6.2 asks for "one PhiCardStack z-order list instead of five
// ad-hoc SetWindowPos(HWND_TOP) calls". This is that list, over opaque
// ids so test/model/panel_test.cc can pin the rules on the Linux host:
//
//   * a panel joins at the TOP (a surface that mounts is above the ones
//     already up — what a bare HWND_TOP did);
//   * Raise() moves it back to the top, adding it when it is not a member;
//   * order() runs BOTTOM to TOP, which is the order SetWindowPos(HWND_TOP)
//     has to be applied in for the last one to end up on top.
class PhiCardStackModel {
 public:
  PhiCardStackModel();
  ~PhiCardStackModel();

  // Adds at the top. A member already in the list is left where it is —
  // mounting twice must not reorder what the user is looking at.
  void Add(uintptr_t id);
  void Remove(uintptr_t id);
  // Moves `id` to the top, adding it when absent.
  void Raise(uintptr_t id);
  bool Contains(uintptr_t id) const;
  // Position bottom-up, or -1.
  int IndexOf(uintptr_t id) const;
  size_t size() const { return order_.size(); }
  bool empty() const { return order_.empty(); }
  void Clear();
  // Bottom to top.
  const std::vector<uintptr_t>& order() const { return order_; }

 private:
  std::vector<uintptr_t> order_;
};

// ---------------------------------------------------------------------------
// Pure: shaped-layer geometry.
//
// A shaped layer's silhouette is a union of rounded rectangles: one for
// the agent pill (a capsule), three for the peek's control chips (the
// region union at lane 3 §3.6 finding 3). The coverage below is the SAME
// distance-then-half-pixel-ramp phi_dwrite's PhiRoundRectCoverage uses, so
// a layer shaped here and a shape drawn by PhiFillRoundRectAA land on the
// same ramp and a chip's edge reads identically whichever side draws it.

struct PhiLayerShape {
  // In the LAYER's own pixels (0,0 = the layer's top-left).
  PhiRectI bounds;
  int radius = 0;
};

// Signed distance from (px, py) to the rounded rect `shape`: negative
// inside, positive outside, in pixels.
double PhiLayerShapeDistance(const PhiLayerShape& shape, double px, double py);

// Coverage (0..1) of the UNION of `shapes` at the pixel CENTRE (px, py).
// An empty list covers nothing.
double PhiLayerShapesCoverage(const std::vector<PhiLayerShape>& shapes,
                              double px,
                              double py);

// True when (x, y) is inside any shape — the hit test that has to agree
// with the silhouette, so a click in the air between two chips falls
// through to the page instead of being eaten by the layer's bounding box.
bool PhiLayerShapesContain(const std::vector<PhiLayerShape>& shapes,
                           int x,
                           int y);

#if defined(_WIN32)

// ---------------------------------------------------------------------------
// The z-order stack for one parent window's panels.

class PhiCardStack {
 public:
  // The stack over `parent` (a shell HWND), created on first use.
  static PhiCardStack& For(HWND parent);
  // Drops the stack for `parent` — its shell is going away.
  static void Forget(HWND parent);

  PhiCardStack(const PhiCardStack&) = delete;
  PhiCardStack& operator=(const PhiCardStack&) = delete;

  // Joins the stack at the top, WITHOUT touching the z-order: a panel is
  // created hidden and raises itself when it is shown.
  void Add(HWND window);
  void Remove(HWND window);
  // Moves `window` to the top of the list and re-applies the whole list's
  // z-order. THE only place a panel's z-order is written.
  void Raise(HWND window);
  // Re-applies the list's order without moving anybody: bottom to top,
  // skipping windows that have died or are hidden.
  void Restack();

  const PhiCardStackModel& model() const { return model_; }

 private:
  PhiCardStack();
  ~PhiCardStack();

  PhiCardStackModel model_;
};

// ---------------------------------------------------------------------------
// A premultiplied 32bpp top-down surface for UpdateLayeredWindow.
//
// The CreateDIBSection / CreateCompatibleDC / SelectObject /
// UpdateLayeredWindow / DeleteObject / DeleteDC block appeared in four
// places (the split-drop cards, the drag image, the agent sprite and the
// peek shadow); this is it, once. Draw into dc() with GDI or GDI+, or fill
// bits() directly, then Commit().
class PhiLayerSurface {
 public:
  PhiLayerSurface(int width, int height);
  ~PhiLayerSurface();
  PhiLayerSurface(const PhiLayerSurface&) = delete;
  PhiLayerSurface& operator=(const PhiLayerSurface&) = delete;

  bool valid() const { return dc_ != nullptr && bits_ != nullptr; }
  HDC dc() const { return dc_; }
  // width * height premultiplied BGRA pixels, top-down, no stride padding.
  void* bits() const { return bits_; }
  size_t byte_size() const;
  int width() const { return width_; }
  int height() const { return height_; }

  // Hands the surface to `hwnd` (which must be WS_EX_LAYERED). `origin`
  // moves the window at the same time when it is a top-level popup;
  // null leaves the position alone.
  // `alpha` scales the whole layer (the 85 % drag ghost, §4.8).
  bool Commit(HWND hwnd, const POINT* screen_origin, BYTE alpha = 255) const;

 private:
  int width_ = 0;
  int height_ = 0;
  HDC dc_ = nullptr;
  HBITMAP bitmap_ = nullptr;
  HGDIOBJ previous_ = nullptr;
  // Into the DIB section this object owns and outlives.
  RAW_PTR_EXCLUSION void* bits_ = nullptr;
};

// Hands a reparented browser child back to the desktop before the panel
// that hosts it goes (destroying the parent would take Chromium's HWND down
// under it). Hidden FIRST, re-styled as a popup and parked off-screen: a
// visible WS_CHILD of the desktop flashes the page at the screen's top-left
// until Chromium gets round to closing it.
void PhiReleaseHostedChild(HWND child);

class PhiCornerFringe;

// ---------------------------------------------------------------------------
// PhiPanel.

class PhiPanel : public PhiWindow {
 public:
  struct Options {
    // Added to WS_CHILD | WS_CLIPSIBLINGS.
    DWORD style = 0;
    DWORD ex_style = 0;
    // WS_EX_LAYERED: the panel's pixels come from UpdateLayeredWindow and
    // never from a WM_PAINT blit. The subclass composes them in
    // ComposeLayer() — per-pixel alpha a coverage mask cannot express (a
    // GDI+ card at 60% over live page content, the agent sprite, the
    // peek's drop shadow).
    bool layered = false;
    // A SHAPED LAYER: `layered`, plus the base does the composing. Paint()
    // draws the OPAQUE content exactly as it would to the screen and
    // LayerShapes() gives the silhouette its alpha comes from, so the whole
    // surface goes through phi_dwrite's one rasteriser. THE replacement for
    // SetWindowRgn, and only for a panel that hosts NO child window (a
    // child window always paints over its layered parent's alpha).
    bool shaped_layer = false;
    // WS_EX_TRANSPARENT — the panel is pure output and every hit test
    // falls through to whatever is under it (the sprite, the cards, the
    // peek's shadow).
    bool click_through = false;
    // The panel joins its parent's card stack. False for a surface whose
    // z-order belongs to another panel (the peek's controls are a child of
    // the peek card, not of the shell).
    bool in_card_stack = true;
  };

  PhiPanel();
  ~PhiPanel() override;

  // Registers `spec` and creates the child. `bounds` is in PARENT client
  // coordinates; a panel is created HIDDEN and appears on ShowPanel().
  bool CreatePanel(const ClassSpec& spec,
                   HWND parent,
                   const RECT& bounds,
                   const Options& options);

  HWND parent() const { return parent_; }
  bool layered() const { return options_.layered || options_.shaped_layer; }
  bool shaped_layer() const { return options_.shaped_layer; }

  // Moves and/or resizes inside the parent WITHOUT touching z-order.
  void SetPanelBounds(const RECT& bounds);
  // Native page overlays share the compositor's lower-left page clip.
  void SetPagePanelBounds(const RECT& bounds, int radius);
  // SW_SHOWNA (never activates: the page keeps the focus) and a raise.
  void ShowPanel();
  void HidePanel();
  bool IsPanelVisible() const;
  // Back to the top of the parent's card stack. The one z-order call.
  void RaisePanel();

  // A CHILD window never receives WM_DPICHANGED — the top-level shell does,
  // and Windows sends its children WM_DPICHANGED_AFTERPARENT instead. This
  // re-reads the parent's scale and, when it moved, hands the base the
  // message the base understands, so dpi() and fonts() still move in
  // exactly one place (phi_window.h contract rule 4) and the six panels
  // that polled GetDpiForWindow() on every relayout stop doing it.
  void SyncDpiFromParent();

  // Shaped layers: re-render and commit the silhouette now. Invalidating
  // a shaped panel does this too (WM_PAINT is routed here), so a subclass
  // normally just calls Invalidate() like any other surface.
  bool CommitLayer();

  // Rounds a panel no shaped layer can round: one that hosts a child
  // window, over pixels this client does not paint (the opener's live page,
  // a split pane's sheet), so no corner cap could know their colour. The
  // window region keeps only the pixels a rounded rect of `radius` covers
  // FULLY; the arcs' partly covered pixels go to four click-through layered
  // siblings stacked right beneath the panel, each carrying `fill` at its
  // coverage, which DWM composites over whatever lies under them — an
  // antialiased curve with no guess at the outside. `corners` masks which
  // corners round (PhiRoundRectCoverage's bits: TL, TR, BL, BR). From here
  // on the fringe follows the panel's size, position, visibility and
  // z-order by itself; call again when `fill` moves (a theme change).
  void SetCornerFringe(int radius, unsigned corners, COLORREF fill);
  // Squares the panel again and drops the fringe.
  void ClearCornerFringe();

 protected:
  // The default is a no-op: a LAYERED panel's pixels come from
  // ComposeLayer() and its back-buffer path is never taken. Every opaque
  // panel overrides this, exactly as PhiWindow requires.
  void Paint(HDC dc, const RECT& dirty) override;

  // A layered panel that is NOT a shaped layer composes its own surface
  // here (PhiLayerSurface, then Commit). Called whenever the panel is
  // invalidated, so a subclass keeps spelling it Invalidate().
  virtual void ComposeLayer() {}

  // The silhouette of a shaped layer, in the panel's own client pixels.
  // The default is the whole client rect with square corners, which is
  // what a layer that carries its own per-pixel alpha wants (the sprite,
  // the drop shadow). Override to cut a capsule or a set of chips.
  virtual void LayerShapes(int width,
                           int height,
                           std::vector<PhiLayerShape>* out) const;

  // A shaped layer answers WM_NCHITTEST from its silhouette, so the air
  // between two chips is not clickable. Off for a layer whose alpha covers
  // the whole rect anyway.
  virtual bool HitTestShape() const;

  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) override;
  void OnNcDestroy() override;

 private:
  HWND parent_ = nullptr;
  Options options_;
  std::unique_ptr<PhiCornerFringe> fringe_;
  // Set by SetPagePanelBounds: the panel covers the content card, whose
  // corners the shell's caps round — so a raise asks for them back on top.
  bool page_panel_ = false;
};

#endif  // defined(_WIN32)

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_PANEL_H_

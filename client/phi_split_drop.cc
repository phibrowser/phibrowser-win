// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_split_drop.h"

// GDI+ draws the cards: rounded paths with per-pixel alpha over live page
// content, which plain GDI cannot produce (the agent overlay's sprite layer
// has the same requirement).
#include <objidl.h>

#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_gdiplus.h"
#include "client/phi_panel.h"
#include "client/phi_shell_window.h"

namespace phi_client {

namespace {

constexpr wchar_t kCardsClass[] = L"PhiSplitDropCards";

// Reference geometry from the Mac's Figma node 5653:25780. The cards scale
// UNIFORMLY inside the page area (one contain-scale for all four), so a wide
// or a tall window cannot stretch a side card back into the top one. Hit
// testing is the edge thirds below and is deliberately independent of this.
// §4.8's snap tiles: 56 × 40 dip tiles, 8 apart, in a radius-8 flyout
// with 8 dip padding, hung 8 under the page's top edge.
constexpr int kTileWidth = 56;
constexpr int kTileHeight = 40;
constexpr int kTileGap = 8;
constexpr int kFlyoutPad = 8;
constexpr int kFlyoutTop = 8;
constexpr int kFlyoutRadius = 8;
constexpr int kTileRadius = 6;
constexpr int kPreviewInset = 8;
constexpr int kPreviewRadius = 8;
constexpr int kPreviewRing = 2;
constexpr double kEdgeZoneFraction = 1.0 / 3.0;
// Once a zone is active it keeps its grip until the pointer leaves it by
// this much, so tremble in a corner cannot alternate two cards.
constexpr int kZoneExitTolerance = 12;
// The framework's split geometry (phi_split_view_style.h): the panes flush
// in the page area, a 1 DIP divider between them. Only the ESTIMATE uses
// these — the real pane rects arrive with the hints.
constexpr int kSplitPaneInset = 0;
constexpr int kSplitDividerThickness = 1;

int Width(const RECT& r) {
  return r.right - r.left;
}
int Height(const RECT& r) {
  return r.bottom - r.top;
}

// The four edge-third rects a create-mode drop activates from.
std::array<RECT, 4> EdgeZoneRects(const RECT& area) {
  const int w = static_cast<int>(Width(area) * kEdgeZoneFraction);
  const int h = static_cast<int>(Height(area) * kEdgeZoneFraction);
  return {
      RECT{area.left, area.top, area.left + w, area.bottom},           // left
      RECT{area.right - w, area.top, area.right, area.bottom},         // right
      RECT{area.left, area.top, area.right, area.top + h},             // top
      RECT{area.left, area.bottom - h, area.right, area.bottom},       // bottom
  };
}

size_t ZoneSlot(PhiSplitDropZone zone) {
  switch (zone) {
    case PhiSplitDropZone::kLeft:
      return 0;
    case PhiSplitDropZone::kRight:
      return 1;
    case PhiSplitDropZone::kTop:
      return 2;
    case PhiSplitDropZone::kBottom:
      return 3;
    case PhiSplitDropZone::kNewWindow:
      return 4;
    case PhiSplitDropZone::kNone:
      break;
  }
  return 0;
}

Gdiplus::Color Blend(COLORREF color, double alpha) {
  return Gdiplus::Color(static_cast<BYTE>(std::lround(alpha * 255.0)),
                        GetRValue(color), GetGValue(color), GetBValue(color));
}

// One rounded-rectangle path, the shape every card is drawn from.
void AddRoundedPath(Gdiplus::GraphicsPath* path,
                    const Gdiplus::RectF& r,
                    Gdiplus::REAL radius) {
  const Gdiplus::REAL d = radius * 2;
  if (d <= 0 || r.Width <= d || r.Height <= d) {
    path->AddRectangle(r);
    return;
  }
  path->AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
  path->AddArc(r.GetRight() - d, r.Y, d, d, 270.0f, 90.0f);
  path->AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.0f, 90.0f);
  path->AddArc(r.X, r.GetBottom() - d, d, d, 90.0f, 90.0f);
  path->CloseFigure();
}

// The hint cards are a LAYERED, click-through child of the shell over the
// page area: their pixels come from UpdateLayeredWindow (per-pixel alpha
// over live page content), never from a WM_PAINT blit, so the panel runs
// in PhiPanel's `layered` mode and composes in ComposeLayer().
class Tiles : public PhiPanel {
 public:
  static Tiles* instance;

  explicit Tiles(PhiShellWindow* controller) : controller_(controller) {
    ClassSpec spec;
    spec.name = kCardsClass;
    spec.cursor = nullptr;
    Options options;
    options.layered = true;
    options.click_through = true;
    options.ex_style = WS_EX_NOPARENTNOTIFY;
    const RECT nothing = {0, 0, 0, 0};
    CreatePanel(spec, controller_->shell_hwnd(), nothing, options);
  }

  ~Tiles() override { Destroy(); }

  PhiShellWindow* controller() const { return controller_; }

  void Update(const PhiSplitDropHints& hints) {
    const bool same =
        shown_ && hints.area.left == hints_.area.left &&
        hints.area.top == hints_.area.top &&
        hints.area.right == hints_.area.right &&
        hints.area.bottom == hints_.area.bottom &&
        hints.replace == hints_.replace && hints.stacked == hints_.stacked &&
        hints.ratio == hints_.ratio && hints.active == hints_.active;
    hints_ = hints;
    if (same) {
      return;  // Nothing the overlay draws has moved.
    }
    SetPanelBounds(hints_.area);
    ShowPanel();
    shown_ = true;
    ComposeLayer();
  }

 protected:
  void ComposeLayer() override { Compose(); }

  void OnDpiChanged(UINT) override { Compose(); }

 private:
  // The target half (or pane) the active zone would take, in the layer's
  // own pixels; empty for New window and none.
  RECT PreviewRect() const {
    const int w = Width(hints_.area);
    const int h = Height(hints_.area);
    RECT out = {};
    if (hints_.replace) {
      RECT primary = {}, secondary = {};
      PhiSplitReplacePaneRects(hints_, &primary, &secondary);
      ::OffsetRect(&primary, -hints_.area.left, -hints_.area.top);
      ::OffsetRect(&secondary, -hints_.area.left, -hints_.area.top);
      if (hints_.active == PhiSplitDropZone::kLeft) {
        out = primary;
      } else if (hints_.active == PhiSplitDropZone::kRight) {
        out = secondary;
      }
      return out;
    }
    switch (hints_.active) {
      case PhiSplitDropZone::kLeft:
        out = {0, 0, w / 2, h};
        break;
      case PhiSplitDropZone::kRight:
        out = {w / 2, 0, w, h};
        break;
      case PhiSplitDropZone::kTop:
        out = {0, 0, w, h / 2};
        break;
      case PhiSplitDropZone::kBottom:
        out = {0, h / 2, w, h};
        break;
      case PhiSplitDropZone::kNewWindow:
      case PhiSplitDropZone::kNone:
        break;
    }
    return out;
  }

  void Compose() {
    if (!alive() || !PhiEnsureGdiPlus()) {
      return;
    }
    const int width = Width(hints_.area);
    const int height = Height(hints_.area);
    PhiLayerSurface surface(width, height);
    if (!surface.valid()) {
      return;
    }
    {
      Gdiplus::Bitmap bitmap(width, height, width * 4, PixelFormat32bppPARGB,
                             static_cast<BYTE*>(surface.bits()));
      Gdiplus::Graphics graphics(&bitmap);
      graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
      graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
      const PhiPalette& palette = controller_->CurrentPalette();
      // The preview: the target half in accent_soft with a 2 dip ring,
      // inset 8.
      RECT preview = PreviewRect();
      if (preview.right > preview.left && preview.bottom > preview.top) {
        ::InflateRect(&preview, -Scaled(kPreviewInset), -Scaled(kPreviewInset));
        const Gdiplus::RectF rc(
            static_cast<Gdiplus::REAL>(preview.left),
            static_cast<Gdiplus::REAL>(preview.top),
            static_cast<Gdiplus::REAL>(Width(preview)),
            static_cast<Gdiplus::REAL>(Height(preview)));
        Gdiplus::GraphicsPath path;
        AddRoundedPath(&path, rc, static_cast<Gdiplus::REAL>(Scaled(kPreviewRadius)));
        Gdiplus::SolidBrush fill(Blend(palette.accent, 0.14));
        graphics.FillPath(&fill, &path);
        Gdiplus::Pen ring(Blend(palette.accent, 0.9),
                          static_cast<Gdiplus::REAL>(Scaled(kPreviewRing)));
        graphics.DrawPath(&ring, &path);
      }
      // The flyout and its tiles.
      std::array<RECT, 6> tiles = {};
      PhiSnapTileRects(hints_.area, dpi(), hints_.replace, &tiles);
      for (RECT& r : tiles) {
        ::OffsetRect(&r, -hints_.area.left, -hints_.area.top);
      }
      {
        const Gdiplus::RectF fly(
            static_cast<Gdiplus::REAL>(tiles[5].left),
            static_cast<Gdiplus::REAL>(tiles[5].top),
            static_cast<Gdiplus::REAL>(Width(tiles[5])),
            static_cast<Gdiplus::REAL>(Height(tiles[5])));
        Gdiplus::GraphicsPath path;
        AddRoundedPath(&path, fly, static_cast<Gdiplus::REAL>(Scaled(kFlyoutRadius)));
        Gdiplus::SolidBrush fill(Blend(palette.flyout, 0.97));
        graphics.FillPath(&fill, &path);
        Gdiplus::Pen stroke(Blend(palette.stroke_strong, 1.0), 1.0f);
        graphics.DrawPath(&stroke, &path);
        // The divider before the New window tile.
        const Gdiplus::REAL dx =
            static_cast<Gdiplus::REAL>(tiles[4].left - Scaled(kTileGap) / 2);
        Gdiplus::Pen divider(Blend(palette.separator, 1.0), 1.0f);
        graphics.DrawLine(&divider, dx, fly.Y + Scaled(kFlyoutPad),
                          dx, fly.GetBottom() - Scaled(kFlyoutPad));
      }
      for (size_t i = 0; i < 5; ++i) {
        if (tiles[i].right <= tiles[i].left) {
          continue;  // Replace mode leaves the top / bottom tiles out.
        }
        DrawTile(&graphics, tiles[i], i,
                 hints_.active != PhiSplitDropZone::kNone &&
                     ZoneSlot(hints_.active) == i,
                 palette);
      }
    }
    surface.Commit(hwnd(), nullptr);
  }

  // One tile: the glyph is the arrangement itself — a frame with the
  // target half filled (New window: a small window with a caption bar).
  void DrawTile(Gdiplus::Graphics* graphics,
                const RECT& tile,
                size_t slot,
                bool active,
                const PhiPalette& palette) {
    const Gdiplus::RectF rc(static_cast<Gdiplus::REAL>(tile.left),
                            static_cast<Gdiplus::REAL>(tile.top),
                            static_cast<Gdiplus::REAL>(Width(tile)),
                            static_cast<Gdiplus::REAL>(Height(tile)));
    Gdiplus::GraphicsPath path;
    AddRoundedPath(&path, rc, static_cast<Gdiplus::REAL>(Scaled(kTileRadius)));
    if (active) {
      Gdiplus::SolidBrush fill(Blend(palette.accent, 0.16));
      graphics->FillPath(&fill, &path);
      Gdiplus::Pen ring(Blend(palette.accent, 1.0), 1.0f);
      graphics->DrawPath(&ring, &path);
    }
    const COLORREF ink = active ? palette.accent : palette.label_dim;
    // The frame: a 28 × 20 rect centred in the tile.
    const int fw = Scaled(28);
    const int fh = Scaled(20);
    const Gdiplus::RectF frame(
        rc.X + (rc.Width - fw) / 2, rc.Y + (rc.Height - fh) / 2,
        static_cast<Gdiplus::REAL>(fw), static_cast<Gdiplus::REAL>(fh));
    Gdiplus::Pen pen(Blend(ink, 1.0), 1.0f);
    Gdiplus::SolidBrush half(Blend(ink, active ? 0.9 : 0.5));
    if (slot == 4) {
      // New window: the frame with a caption bar.
      graphics->DrawRectangle(&pen, frame);
      Gdiplus::RectF bar(frame.X, frame.Y, frame.Width,
                         static_cast<Gdiplus::REAL>(Scaled(5)));
      graphics->FillRectangle(&half, bar);
      return;
    }
    Gdiplus::RectF target = frame;
    switch (slot) {
      case 0:
        target.Width /= 2;
        break;
      case 1:
        target.X += target.Width / 2;
        target.Width /= 2;
        break;
      case 2:
        target.Height /= 2;
        break;
      default:
        target.Y += target.Height / 2;
        target.Height /= 2;
        break;
    }
    graphics->FillRectangle(&half, target);
    graphics->DrawRectangle(&pen, frame);
  }

  RAW_PTR_EXCLUSION PhiShellWindow* controller_ = nullptr;
  bool shown_ = false;
  PhiSplitDropHints hints_;
};

Tiles* Tiles::instance = nullptr;

}  // namespace

bool PhiSplitZoneIsPrimary(PhiSplitDropZone zone) {
  return zone == PhiSplitDropZone::kLeft || zone == PhiSplitDropZone::kTop;
}

const char* PhiSplitZoneLayout(PhiSplitDropZone zone) {
  return (zone == PhiSplitDropZone::kTop || zone == PhiSplitDropZone::kBottom)
             ? "horizontal"
             : "vertical";
}

void PhiSnapTileRects(const RECT& area,
                      UINT dpi,
                      bool replace,
                      std::array<RECT, 6>* out) {
  const auto scaled = [dpi](int dip) {
    return ::MulDiv(dip, static_cast<int>(dpi), 96);
  };
  const int tw = scaled(kTileWidth);
  const int th = scaled(kTileHeight);
  const int gap = scaled(kTileGap);
  const int pad = scaled(kFlyoutPad);
  const int count = replace ? 3 : 5;  // Two panes + New window, or five.
  const int width = pad * 2 + count * tw + (count - 1) * gap;
  const int left = (area.left + area.right - width) / 2;
  const int top = area.top + scaled(kFlyoutTop);
  (*out)[5] = {left, top, left + width, top + pad * 2 + th};
  for (size_t i = 0; i < 5; ++i) {
    (*out)[i] = RECT{};
  }
  int x = left + pad;
  const auto place = [&](size_t slot) {
    (*out)[slot] = {x, top + pad, x + tw, top + pad + th};
    x += tw + gap;
  };
  if (replace) {
    place(0);
    place(1);
    place(4);
  } else {
    for (size_t i = 0; i < 5; ++i) {
      place(i);
    }
  }
}

PhiSplitDropZone PhiSplitCreateZoneAt(const RECT& area,
                                      POINT pt,
                                      PhiSplitDropZone retained,
                                      UINT dpi) {
  if (!::PtInRect(&area, pt)) {
    return PhiSplitDropZone::kNone;
  }
  constexpr std::array<PhiSplitDropZone, 5> kOrder = {
      PhiSplitDropZone::kLeft, PhiSplitDropZone::kRight,
      PhiSplitDropZone::kTop, PhiSplitDropZone::kBottom,
      PhiSplitDropZone::kNewWindow};
  // A snap tile wins over the zone under it.
  std::array<RECT, 6> tiles = {};
  PhiSnapTileRects(area, dpi, /*replace=*/false, &tiles);
  for (size_t i = 0; i < 5; ++i) {
    if (::PtInRect(&tiles[i], pt)) {
      return kOrder[i];
    }
  }
  if (::PtInRect(&tiles[5], pt)) {
    return retained;  // The flyout's padding keeps what was chosen.
  }
  const std::array<RECT, 4> zones = EdgeZoneRects(area);
  if (retained != PhiSplitDropZone::kNone &&
      retained != PhiSplitDropZone::kNewWindow) {
    RECT held = zones[ZoneSlot(retained)];
    ::InflateRect(&held, kZoneExitTolerance, kZoneExitTolerance);
    if (::PtInRect(&held, pt)) {
      return retained;
    }
  }
  for (size_t i = 0; i < 4; ++i) {
    if (::PtInRect(&zones[i], pt)) {
      return kOrder[i];
    }
  }
  // The centre of the page picks New window (Jixiang, 2026-09-17).
  return PhiSplitDropZone::kNewWindow;
}

void PhiSplitReplacePaneRects(const PhiSplitDropHints& hints,
                              RECT* primary,
                              RECT* secondary) {
  if (hints.panes) {
    *primary = hints.primary;
    *secondary = hints.secondary;
    return;
  }
  // The estimate: the panes over the page area, the divider between.
  RECT area = hints.area;
  ::InflateRect(&area, -kSplitPaneInset, -kSplitPaneInset);
  area.right = std::max(area.right, area.left);
  area.bottom = std::max(area.bottom, area.top);
  const double share = std::min(1.0, std::max(0.0, hints.ratio));
  if (hints.stacked) {
    const int usable = std::max(0, Height(area) - kSplitDividerThickness);
    const int top = static_cast<int>(usable * share);
    *primary = {area.left, area.top, area.right, area.top + top};
    *secondary = {area.left, area.top + top + kSplitDividerThickness,
                  area.right, area.bottom};
    return;
  }
  const int usable = std::max(0, Width(area) - kSplitDividerThickness);
  const int lead = static_cast<int>(usable * share);
  *primary = {area.left, area.top, area.left + lead, area.bottom};
  *secondary = {area.left + lead + kSplitDividerThickness, area.top,
                area.right, area.bottom};
}

PhiSplitDropZone PhiSplitReplaceZoneAt(const PhiSplitDropHints& hints,
                                       POINT pt,
                                       UINT dpi) {
  // The pane tiles and the New window tile first.
  std::array<RECT, 6> tiles = {};
  PhiSnapTileRects(hints.area, dpi, /*replace=*/true, &tiles);
  if (::PtInRect(&tiles[0], pt)) {
    return PhiSplitDropZone::kLeft;
  }
  if (::PtInRect(&tiles[1], pt)) {
    return PhiSplitDropZone::kRight;
  }
  if (::PtInRect(&tiles[4], pt)) {
    return PhiSplitDropZone::kNewWindow;
  }
  RECT primary = {}, secondary = {};
  PhiSplitReplacePaneRects(hints, &primary, &secondary);
  // The centre rule applies to the divider gap only: between the panes a
  // drop detaches.
  if (hints.stacked) {
    if (pt.y > primary.bottom && pt.y < secondary.top) {
      return PhiSplitDropZone::kNewWindow;
    }
    return pt.y <= primary.bottom ? PhiSplitDropZone::kLeft
                                  : PhiSplitDropZone::kRight;
  }
  if (pt.x > primary.right && pt.x < secondary.left) {
    return PhiSplitDropZone::kNewWindow;
  }
  return pt.x <= primary.right ? PhiSplitDropZone::kLeft
                               : PhiSplitDropZone::kRight;
}

// static
void PhiSplitDropOverlay::Show(PhiShellWindow* controller,
                               const PhiSplitDropHints& hints) {
  if (!controller || !controller->shell_hwnd()) {
    return;
  }
  if (Tiles::instance && Tiles::instance->controller() != controller) {
    Hide();  // The drag moved to another shell window.
  }
  if (!Tiles::instance) {
    Tiles::instance = new Tiles(controller);
  }
  Tiles::instance->Update(hints);
}

// static
void PhiSplitDropOverlay::Hide() {
  delete Tiles::instance;
  Tiles::instance = nullptr;
}

}  // namespace phi_client

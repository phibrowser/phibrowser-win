// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_sidebar_view.h"

#include "client/phi_agent_spaces.h"
#include "client/phi_app_model.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/strings/utf_string_conversions.h"
#include "client/phi_animator.h"
#include "client/phi_bookmark_manager.h"
#include "client/phi_dwrite.h"
#include "client/phi_fluent.h"
#include "client/phi_folder_icons.h"
#include "client/phi_group_overview.h"
#include "client/phi_i18n.h"
#include "client/phi_icons.h"
#include "client/phi_mac_icons.h"
#include "client/phi_new_tab_page.h"
#include "client/phi_search_tabs_panel.h"
#include "client/phi_shell_layout.h"
#include "client/phi_sidebar_drag_session.h"
#include "client/phi_spaces_strip.h"
#include "client/phi_uia.h"
#include "client/phi_shell_window.h"
#include "client/phi_window_model.h"

namespace phi_client {

namespace {

// Comfortable's strip overflow chevron in the keyboard ring, after the
// tabs. It is no row, so it borrows a key no row (and no Space id) can
// carry.
SidebarKey OverflowChevronKey() {
  return SidebarKey::Space("@strip-overflow");
}

using phi_framework::SplitSnapshot;
using phi_framework::TabSnapshot;
using phi_framework::WindowSnapshot;

using shell_metrics::kEdgeInset;
using shell_metrics::kFaviconSlot;
using shell_metrics::kFavoritesSpacer;
using shell_metrics::kNavButton;
using shell_metrics::kPinnedCellHeight;
using shell_metrics::kPinnedGap;
using shell_metrics::kTabRowGap;
using shell_metrics::kTabRowHeight;
using shell_metrics::kTopBarSpacing;

using sidebar_metrics::BookmarkIndentDepth;
using sidebar_metrics::kBookmarkIndent;
using sidebar_metrics::kChipCollapseControl;
using sidebar_metrics::kChipCollapseGap;
using sidebar_metrics::kChipCountFont;
using sidebar_metrics::kChipCountHPad;
using sidebar_metrics::kChipCountToLabel;
using sidebar_metrics::kChipCountVPad;
using sidebar_metrics::kChipDot;
using sidebar_metrics::kChipDotToLabel;
using sidebar_metrics::kChipHeight;
using sidebar_metrics::kChipLabelRightPad;
using sidebar_metrics::kChipLabelSlack;
using sidebar_metrics::kChipLeadGap;
using sidebar_metrics::kChipLeading;
using sidebar_metrics::kChipMaxWidth;
using sidebar_metrics::kChipMosaic;
using sidebar_metrics::kChipMosaicCell;
using sidebar_metrics::kChipMosaicFont;
using sidebar_metrics::kChipMosaicGap;
using sidebar_metrics::kChipMosaicMargin;
using sidebar_metrics::kChipTrailGap;
using sidebar_metrics::kChipUnderlineInset;
using sidebar_metrics::kCloseBoxSize;
using sidebar_metrics::kCornerRadius;
using sidebar_metrics::kFaviconSize;
using sidebar_metrics::kFolderIconSize;
using sidebar_metrics::kGroupHeaderHeight;
using sidebar_metrics::kGroupInnerInset;
using sidebar_metrics::kGroupMemberLeadInset;
using sidebar_metrics::kGroupMemberTrailInset;
using sidebar_metrics::kHTabActiveWidth;
using sidebar_metrics::kHTabGap;
using sidebar_metrics::kHTabHeight;
using sidebar_metrics::kHTabMinWidth;
using sidebar_metrics::kHTabWidth;
using sidebar_metrics::kPinnedFavicon;
using sidebar_metrics::kPinnedMinWidth;
using sidebar_metrics::kRowPad;
using sidebar_metrics::kStripPinFavicon;
using sidebar_metrics::kStripPinGap;
using sidebar_metrics::kStripPinSize;
using sidebar_metrics::kStripPinToTabs;
using sidebar_metrics::kAutoScrollEdge;
using sidebar_metrics::kAutoScrollMaxStep;
using sidebar_metrics::kAutoScrollStripMs;
using sidebar_metrics::kScrollbarFadeMs;
using sidebar_metrics::kScrollbarHoldMs;
using sidebar_metrics::kScrollbarInset;
using sidebar_metrics::kScrollbarMinThumb;
using sidebar_metrics::kScrollbarWidth;

// A RECLAIMED tab — restored-but-never-loaded (unloaded) or discarded —
// draws its favicon at 30% (the Mac's TabFaviconPresentation.reclaimedOpacity,
// a700125c, which replaced the short-lived dashed ring); the dimmed TITLE on
// tab rows stays, as before.
constexpr float kReclaimedOpacity = 0.3f;

// The Mac's TabOpenIndicatorView (a700125c): a 2pt dot — black in light,
// white in dark — marking an inactive pinned tab or bookmark whose tab is
// open, behind "Show open tab indicators" (off by default). It hangs 3pt
// under a pinned favicon (2pt on the Comfortable strip, where the cell is
// shorter) and leads a bookmark's favicon by 2pt. A reclaimed tab dims the
// dot with its favicon.
// windows-native-ui.md §5.8: the taskbar's running mark — an 8 × 2 dip
// pill, radius 1, `label` ink at 60 %, centred under the favicon with a
// 3 dip gap (2 on the shorter Comfortable strip cell).
constexpr int kOpenPillWidth = 8;
constexpr int kOpenPillHeight = 2;
constexpr int kOpenPillGap = 3;
constexpr int kOpenPillStripGap = 2;
constexpr float kOpenPillOpacity = 0.6f;

COLORREF OpenDotInk(const PhiPalette& palette) {
  return palette.dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
}

// `ink` at `opacity` over `ground` — the dot has no alpha channel of its own.
COLORREF FadeInk(COLORREF ink, COLORREF ground, float opacity) {
  const auto mix = [&](int a, int b) {
    return static_cast<int>(b + (a - b) * opacity + 0.5f);
  };
  return RGB(mix(GetRValue(ink), GetRValue(ground)),
             mix(GetGValue(ink), GetGValue(ground)),
             mix(GetBValue(ink), GetBValue(ground)));
}

void DrawOpenPill(HDC dc, int cx, int top, int width, int height,
                  COLORREF ink) {
  PhiFillRoundRectAA(dc, {cx - width / 2, top, cx - width / 2 + width,
                          top + height},
                     std::max(1, height / 2), ink);
}

}  // namespace


// A filled rounded rect, the Mac client's selection/field shape.
// A fill rounded on its TOP corners only, square along the bottom — the
// comfortable tab's shape, which has to sit flush on the toolbar it merges
// into. Drawing the round rect a radius taller and clipping to `r` puts the
// arcs where they belong and leaves the bottom edge straight.
// Every rounded shape here is antialiased and takes the TRUE corner radius
// (phi_dwrite.h). The GDI RoundRect these replaced took the corner
// ellipse's diameter, so a pill asked for kCornerRadius came out at half
// of it — and the active pin's ring, which passed 2 * kCornerRadius, was
// the one shape that drew the radius the constant names.
void PhiFillRoundedTop(HDC dc, const RECT& r, COLORREF fill, int radius) {
  if (r.right <= r.left || r.bottom <= r.top) {
    return;
  }
  const int save = ::SaveDC(dc);
  ::IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);
  PhiFillRoundRectAA(dc, {r.left, r.top, r.right, r.bottom + radius}, radius,
                     fill);
  ::RestoreDC(dc, save);
}

void PhiFillRounded(HDC dc, const RECT& r, COLORREF fill, int radius) {
  PhiFillRoundRectAA(dc, r, radius, fill);
}

void PhiStrokeRounded(HDC dc, const RECT& r, COLORREF fill, COLORREF border,
                   int radius) {
  PhiFillRoundRectAA(dc, r, radius, fill);
  PhiStrokeRoundRectAA(dc, r, radius, border, 1);
}

// Reader View's icon: SF Symbol `doc.plaintext`, which is what BOTH Mac
// address bars draw (SideAddressBar's HoverableButtonConfig and
// ReaderButtonView). Segoe MDL2's nearest neighbour is ReadingMode (E736), an
// OPEN BOOK — a different picture altogether — so the symbol is drawn instead
// of substituted: a rounded page outline with lines of text in it.
//
// Proportions are the symbol's own, in units of the glyph box: the page is
// 0.72 wide by 0.90 tall, corner radius 0.14, stroke 0.085, and the text runs
// as three lines inset 0.17 from each side, the last one short — four lines
// at this size close up into a smudge.
void PhiDrawReaderGlyph(HDC dc, const RECT& button, COLORREF ink, UINT dpi) {
  if (button.right <= button.left || button.bottom <= button.top) {
    return;
  }
  const auto scaled = [dpi](double units, double box) {
    return static_cast<int>(units * box * dpi / 96.0 + 0.5);
  };
  // The glyph box the symbol is laid out in: the Mac draws a 12pt image in a
  // 24pt button, so half the button, and never bigger than it fits.
  const double box = (std::min)(button.right - button.left,
                                button.bottom - button.top) * 96.0 / dpi / 2.0;
  const int page_w = scaled(0.72, box);
  const int page_h = scaled(0.90, box);
  if (page_w <= 2 || page_h <= 2) {
    return;
  }
  const int cx = (button.left + button.right) / 2;
  const int cy = (button.top + button.bottom) / 2;
  RECT page = {cx - page_w / 2, cy - page_h / 2, cx - page_w / 2 + page_w,
               cy - page_h / 2 + page_h};
  const int stroke = (std::max)(1, scaled(0.085, box));
  PhiStrokeRoundRectAA(dc, page, (std::max)(1, scaled(0.14, box)), ink, stroke);

  const int inset = scaled(0.17, box);
  const int line_left = page.left + inset;
  const int line_right = page.right - inset;
  if (line_right - line_left <= 1) {
    return;
  }
  // Evenly spaced across the page's inner height, so the block of text sits
  // optically centred rather than crowding either edge.
  const int rows = 3;
  const int top = page.top + inset;
  const int bottom = page.bottom - inset;
  // Solid fills, not the AA helper: a one-pixel bar antialiases to about half
  // coverage and the text would read lighter than the outline around it.
  HBRUSH text_ink = ::CreateSolidBrush(ink);
  for (int i = 0; i < rows; ++i) {
    const int y = top + (bottom - top - stroke) * i / (rows - 1);
    // The last line is short, as the symbol's is.
    const int right = i == rows - 1
                          ? line_left + (line_right - line_left) * 3 / 5
                          : line_right;
    RECT line = {line_left, y, right, y + stroke};
    ::FillRect(dc, &line, text_ink);
  }
  ::DeleteObject(text_ink);
}

void PhiDrawGlyph(HDC dc, const RECT& r, const wchar_t* glyph, COLORREF ink) {
  if (r.right <= r.left) {
    return;
  }
  ::SetTextColor(dc, ink);
  // An MDL2 icon under an MDL2 font goes through DirectWrite (grayscale
  // antialiasing both ways); text takes GDI as before.
  PhiDrawTextAA(dc, glyph, -1, const_cast<RECT*>(&r),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// The same, for an MDL2 ICON: rasterized by DirectWrite at `pixel_size`
// (the em the caller's GDI font was built at, so the glyph lands the same
// size in the same place) and alpha-blended over what is already painted.
// GDI renders these with ClearType — horizontal smoothing only, plus a
// grid-fit per stem — which is what stepped the magnifier's arc and fringed
// every diagonal in colour. Falls back to GDI whenever DirectWrite or the
// glyph is missing, so nothing depends on it being there.
void PhiDrawGlyphIcon(HDC dc, const RECT& r, const wchar_t* glyph,
                      int pixel_size, COLORREF ink) {
  if (r.right <= r.left) {
    return;
  }
  if (!PhiDrawIconGlyph(dc, r, glyph, pixel_size, ink)) {
    PhiDrawGlyph(dc, r, glyph, ink);
  }
}

// `over` at `alpha`/255 on `base` — the Mac's translucent overlays, opaque.
COLORREF PhiBlendOver(COLORREF over, COLORREF base, int alpha) {
  auto mix = [&](int a, int b) { return (a * alpha + b * (255 - alpha)) / 255; };
  return RGB(mix(GetRValue(over), GetRValue(base)),
             mix(GetGValue(over), GetGValue(base)),
             mix(GetBValue(over), GetBValue(base)));
}

// Readers over a WindowSnapshot — the model's copy or a freshly pulled one
// (PullSnapshot) alike. The split-drop paths map ids to tags and back
// several times inside a single drop, so they read the window ONCE and walk
// this instead.
const TabSnapshot* PhiFindTabIn(const WindowSnapshot& snapshot, TabId id) {
  if (!id.valid()) {
    return nullptr;
  }
  for (const TabSnapshot& tab : snapshot.tabs) {
    if (tab.id == id) {
      return &tab;
    }
  }
  return nullptr;
}

const TabSnapshot* PhiFindTagIn(const WindowSnapshot& snapshot,
                             const std::string& tag) {
  if (tag.empty()) {
    return nullptr;
  }
  for (const TabSnapshot& tab : snapshot.tabs) {
    if (tab.tag == tag) {
      return &tab;
    }
  }
  return nullptr;
}

const SplitSnapshot* PhiFindSplitIn(const WindowSnapshot& snapshot,
                                 const std::string& split_id) {
  if (split_id.empty()) {
    return nullptr;
  }
  for (const SplitSnapshot& split : snapshot.splits) {
    if (split.id == split_id) {
      return &split;
    }
  }
  return nullptr;
}

// The live tab bound to a stored row, or `fallback` when the row has none.
// A pinned tab or bookmark is OPENED by the very press that arms its drag, so
// the view row the drag was armed from can still say "closed" when the drop
// lands — and opening its url a second time would leave a duplicate tab
// beside the pane.
TabId PhiLiveTabForRow(const WindowSnapshot& snapshot,
                          const std::string& guid,
                          TabId fallback) {
  const TabSnapshot* tab = PhiFindTagIn(snapshot, guid);
  return tab ? tab->id : fallback;
}

// The framework names surfaces (a reader page recognised from what it
// shows, committed or not); the client parses no reader URLs.
SidebarSurface PhiSurfaceOf(const TabSnapshot& tab) {
  return tab.surface == phi_framework::TabSurface::kReader
             ? SidebarSurface::kReader
             : SidebarSurface::kContent;
}

RECT PhiToRect(const SidebarRect& r) {
  return RECT{r.left, r.top, r.right, r.bottom};
}
PhiSidebarView::PhiSidebarView(PhiSidebarShell* shell,
                               SlotId slot,
                               phi_framework::PhiFramework* framework,
                               SidebarHost* sidebar_host,
                               SpacesHost* spaces_host,
                               ChromePrefsHost* prefs_host,
                               AgentHost* agent_host,
                               WindowHost* window_host,
                               const PhiWindowModel* model,
                               bool agent,
                               bool incognito)
    : shell_(shell),
      slot_(slot),
      framework_(framework),
      sidebar_host_(sidebar_host),
      spaces_host_(spaces_host),
      prefs_host_(prefs_host),
      agent_host_(agent_host),
      window_host_(window_host),
      model_(model),
      incognito_(incognito),
      agent_(agent) {}

// A live drag goes with the view: the session's destructor IS AbortDrag, so
// the drag image and the split-drop overlay come down by ownership rather
// than by two ad-hoc calls in the shell's own destructor
// (docs/architecture-review.md §3.5).
PhiSidebarView::~PhiSidebarView() {
  CancelStripLanding();
  StopDragAutoScroll();
  if (scrollbar_animation_) {
    PhiAnimator::ForThread().Cancel(scrollbar_animation_);
    scrollbar_animation_ = 0;
  }
  for (HFONT font : {chip_count_font_, chip_mosaic_font_}) {
    if (font) {
      ::DeleteObject(font);
    }
  }
}

void PhiSidebarView::EnsureChipFonts() {
  if (chip_count_font_ && chip_mosaic_font_ && chip_fonts_dpi_ == dpi()) {
    return;
  }
  LOGFONTW base = {};
  const HFONT text_font = shell_->SidebarTextFont();
  if (!text_font || !::GetObjectW(text_font, sizeof(base), &base)) {
    return;
  }
  const auto build = [&](int height_px, int weight) -> HFONT {
    LOGFONTW lf = base;
    lf.lfHeight = -height_px;
    lf.lfWeight = weight;
    // GDI keeps Segoe UI's semibold as a family of its own (the shell's
    // rule for its own semibold face): asking "Segoe UI" for weight 600
    // rounds up to Bold, so name the face when that is the system font.
    if (weight == FW_SEMIBOLD &&
        std::wstring_view(lf.lfFaceName) == L"Segoe UI") {
      const std::wstring face = L"Segoe UI Semibold";
      std::fill(std::begin(lf.lfFaceName), std::end(lf.lfFaceName), L'\0');
      std::copy_n(face.begin(),
                  std::min(face.size(), std::size(lf.lfFaceName) - 1),
                  std::begin(lf.lfFaceName));
    }
    return ::CreateFontIndirectW(&lf);
  };
  if (HFONT built = build(Scaled(kChipCountFont), FW_BOLD)) {
    if (chip_count_font_) {
      ::DeleteObject(chip_count_font_);
    }
    chip_count_font_ = built;
  }
  if (HFONT built = build(Scaled(kChipMosaicFont), FW_SEMIBOLD)) {
    if (chip_mosaic_font_) {
      ::DeleteObject(chip_mosaic_font_);
    }
    chip_mosaic_font_ = built;
  }
  chip_fonts_dpi_ = dpi();
}

int PhiSidebarView::MeasureText(HFONT font, const std::wstring& text) const {
  const HWND hwnd = shell_hwnd();
  const HDC dc = ::GetDC(hwnd);
  if (!dc) {
    return 0;
  }
  HGDIOBJ previous = font ? ::SelectObject(dc, font) : nullptr;
  RECT rc = {};
  ::DrawTextW(dc, text.c_str(), -1, &rc,
              DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
  if (previous) {
    ::SelectObject(dc, previous);
  }
  ::ReleaseDC(hwnd, dc);
  return static_cast<int>(rc.right - rc.left);
}

HWND PhiSidebarView::shell_hwnd() const {
  return shell_->SidebarShellWindow();
}

// The disclosed folders are view state of THIS shell alone: no push comes
// for them, so the caller schedules its own rebuild.
bool PhiSidebarView::ToggleExpanded(const std::string& guid) {
  if (expanded_bookmarks_.erase(guid)) {
    return false;
  }
  expanded_bookmarks_.insert(guid);
  return true;
}

SidebarKey PhiSidebarView::SpaceRootKey() const {
  return SidebarKey::Space(shell_->SidebarActiveSpace());
}

void PhiSidebarView::ToggleSpaceRoot() {
  if (prefs_host_) {
    prefs_host_->SetBookmarkRootCollapsed(shell_->SidebarActiveSpace(),
                                          space_root_.expanded);
  }
  shell_->SidebarScheduleRefresh();
}

int PhiSidebarView::OutlineIndentDepth(int depth) const {
  return BookmarkIndentDepth(depth + (space_root_.shown ? 1 : 0));
}

DropResolution PhiSidebarView::ResolveOutlineDrop(
    int pointer_y,
    const SidebarKey& dragging) const {
  if (!space_root_.shown || pointer_y >= space_root_.frame.bottom) {
    return ResolveBookmarkDrop(BookmarkDropRows(), pointer_y, dragging);
  }
  // On the root: the item lands first among the Space's top-level bookmarks.
  // An expanded root opens the gap above its first child; a collapsed one has
  // no rows to part, so it highlights instead.
  DropResolution out;
  out.place.section = SidebarPlace::Section::kBookmarks;
  for (const SidebarCell& cell : sidebar_tree_.bookmarks) {
    if (cell.depth == 0 && cell.key != dragging) {
      out.place.parent_guid = cell.parent_guid;
      out.place.before = cell.key;
      break;
    }
  }
  if (space_root_.expanded) {
    out.gap_slot = 0;
  } else {
    out.folder_key = SpaceRootKey();
  }
  return out;
}

bool PhiSidebarView::OnStoredRowClick(POINT pt) {
  EndKeyboardFocus();  // The ring is the keyboard's; a click retires it.
  if (::PtInRect(&space_root_.bounds, pt)) {
    ToggleSpaceRoot();
    return true;
  }
  for (const ShortcutRow& pin : pinned_) {
    if (::PtInRect(&pin.bounds, pt)) {
      if (pin.attached_surface != SidebarSurface::kContent &&
          ::PtInRect(&pin.surface_box, pt)) {
        Apply(SidebarCloseSurface{pin.key});
        return true;
      }
      ArmDrag(pin.key, pt);
      OpenOrFocusPinned(pin.guid, pin.url);
      return true;
    }
  }
  for (const ShortcutRow& bookmark : bookmarks_) {
    if (::PtInRect(&bookmark.bounds, pt)) {
      if (bookmark.attached_surface != SidebarSurface::kContent &&
          ::PtInRect(&bookmark.surface_box, pt)) {
        Apply(SidebarCloseSurface{bookmark.key});
        return true;
      }
      if (bookmark.close_box.right > bookmark.close_box.left &&
          ::PtInRect(&bookmark.close_box, pt)) {
        if (!agent_holds_input_) {  // The Mac refuses its close under the agent.
          Apply(SidebarClose{bookmark.key, -1});
        }
        return true;
      }
      ArmDrag(bookmark.key, pt);
      if (bookmark.folder) {
        // A folder click discloses/collapses its children (the Mac's
        // toggleItemExpanded). The armed drag is keyed, so the rebuild
        // cannot invalidate it.
        ToggleExpanded(bookmark.guid);
        shell_->SidebarScheduleRefresh();
      } else if (!bookmark.url.empty()) {
        // Sidebar bookmark rows bind like pins (bookmarkGuidBacking); the
        // Bookmarks menu keeps opening plain tabs.
        OpenOrFocusPinned(bookmark.guid, bookmark.url);
      }
      return true;
    }
  }
  return false;
}

bool PhiSidebarView::OnContentRowClick(POINT pt) {
  if (::PtInRect(&strip_overflow_button_, pt)) {
    // Comfortable's overflow chevron: the tab search palette lists every
    // tab, the ones scrolled out of the strip's window included.
    PhiSearchTabsPanel::Toggle(shell_->SidebarOwner());
    return true;
  }
  for (const GroupHeader& gh : group_headers_) {
    if (::PtInRect(&gh.bounds, pt)) {
      const BrowserId browser = shell_->SidebarActiveBrowser();
      bool toggle = true;
      bool overview = false;
      if (gh.chip) {
        // The strip chip (TabGroupChipView.mouseUp): the chevron — its
        // frame grown 4pt by 6pt, the Mac's collapseControlRect — and a
        // collapsed chip's mosaic toggle the group; Ctrl-click (the Mac's
        // Cmd) only ever EXPANDS; the body opens the group overview.
        RECT control = gh.collapse_box;
        ::InflateRect(&control, Scaled(4), Scaled(6));
        const bool on_control = ::PtInRect(&control, pt) ||
                                (gh.collapsed && ::PtInRect(&gh.mosaic, pt));
        const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
        toggle = on_control || (ctrl && gh.collapsed);
        overview = !on_control && !ctrl;
      }
      if (toggle && framework_ && browser.valid()) {
        framework_->SetTabGroupCollapsed(browser.value, gh.id.c_str(),
                                         !gh.collapsed);
      } else if (overview) {
        PhiGroupOverview::Open(shell_->SidebarOwner(), gh.id);
      }
      return true;
    }
  }
  for (const TabRow& row : tabs_) {
    if (!::PtInRect(&row.bounds, pt)) {
      continue;
    }
    if (framework_) {
      if (row.attached_surface != SidebarSurface::kContent &&
          ::PtInRect(&row.surface_box, pt)) {
        Apply(SidebarCloseSurface{row.key});  // The indicator's minus.
      } else if (::PtInRect(&row.close_box, pt)) {
        if (!agent_holds_input_) {  // The Mac refuses ✕ under the agent.
          Apply(SidebarClose{row.key, -1});  // A merged row: both panes.
        }
      } else if (row.split_id.empty() && (::GetKeyState(VK_MENU) & 0x8000)) {
        // Alt-click, the Mac's option-click: split this tab with the active
        // one (performSplitAction). Either pane may be bound to a pinned tab
        // or a bookmark — the split takes both into the normal list.
        Apply(SidebarFormSplit{SidebarKey::Tab(sidebar_tree_.active_tab),
                               row.key, "vertical"});
      } else if (row.b_index >= 0 && ::PtInRect(&row.pane_b, pt)) {
        Apply(SidebarActivate{row.key, 1});
      } else {
        // The press only ARMS: a tab activates on the release of a plain
        // click (the session's commit), never because it was dragged — the
        // Mac's outline selects on click, not on mouse-down, so dragging a
        // background tab leaves the page the user was reading in place.
        ArmDrag(row.key, pt);
        NoteDragClickTab(row.id);
      }
    }
    return true;
  }
  return false;
}

// Middle-click closes a tab row anywhere on the row (the Mac tab list's
// otherMouseDown), merged split rows included. Pins and bookmarks are
// persistent — a middle-click must not delete them, so only tab rows react.
bool PhiSidebarView::OnMiddleClick(POINT pt) {
  if (!framework_) {
    return false;
  }
  for (const TabRow& row : tabs_) {
    if (!::PtInRect(&row.bounds, pt)) {
      continue;
    }
    Apply(SidebarClose{row.key, -1});
    return true;
  }
  return false;
}

SidebarKey PhiSidebarView::TabBodyAt(POINT pt, int* pane) const {
  *pane = -1;
  for (const TabRow& row : tabs_) {
    if (!::PtInRect(&row.bounds, pt)) {
      continue;
    }
    if (::PtInRect(&row.close_box, pt) ||
        (row.attached_surface != SidebarSurface::kContent &&
         ::PtInRect(&row.surface_box, pt))) {
      return {};
    }
    if (row.b_index >= 0) {
      *pane = ::PtInRect(&row.pane_b, pt) ? 1 : 0;
    }
    return row.key;
  }
  return {};
}

// --- The drag session -------------------------------------------------------
//
// It exists between here and the commit or the cancel, and nowhere else
// (docs/architecture-review.md §3.5): every field of a drag lives inside
// it, so a drag that ends cannot leave a flag behind.

void PhiSidebarView::ArmDrag(const SidebarKey& key, POINT pt) {
  StopDragAutoScroll();
  drag_ = std::make_unique<PhiSidebarDragSession>(this, key, pt);
  // What the drag resolves against: a Space switch, a layout change or the
  // band collapsing under it abort it (DragContextChanged).
  const PhiSidebarRects layout = shell_->SidebarRects();
  drag_space_ = shell_->SidebarActiveSpace();
  drag_horizontal_ = layout.horizontal_tabs;
  drag_band_hidden_ = BandHidden(layout);
}

bool PhiSidebarView::DragContextChanged() const {
  if (!drag_) {
    return false;
  }
  const PhiSidebarRects layout = shell_->SidebarRects();
  return shell_->SidebarActiveSpace() != drag_space_ ||
         layout.horizontal_tabs != drag_horizontal_ ||
         BandHidden(layout) != drag_band_hidden_;
}

void PhiSidebarView::NoteDragClickTab(TabId tab_id) {
  if (drag_) {
    drag_->NoteClickTab(tab_id);
  }
}

void PhiSidebarView::UpdateDrag(POINT pt) {
  if (!drag_) {
    return;
  }
  if (DragContextChanged()) {
    // The rows it was resolving against belong to another Space / layout
    // (or to a collapsed band, which would drop the tab at the list's end,
    // ungrouped): the drag is over, nothing moves.
    AbortDrag();
    return;
  }
  drag_->Update(pt);
  if (drag_ && drag_->active()) {
    UpdateDragAutoScroll(pt);
  }
}

void PhiSidebarView::CommitDrag() {
  StopDragAutoScroll();
  if (!drag_) {
    return;
  }
  if (DragContextChanged()) {
    AbortDrag();  // See UpdateDrag: a release there is no drop.
    return;
  }
  // Held while it commits: the drop can rebuild the rows under it, and the
  // session must outlive its own last statement. Its destructor then finds
  // a finished drag and aborts nothing.
  const std::unique_ptr<PhiSidebarDragSession> session = std::move(drag_);
  session->Commit();
}

void PhiSidebarView::AbortDrag() {
  StopDragAutoScroll();
  drag_.reset();  // ~PhiSidebarDragSession IS AbortDrag.
}

bool PhiSidebarView::CancelDragEscape() {
  if (!drag_ || !drag_->active()) {
    return false;
  }
  StopDragAutoScroll();
  // Out of the view FIRST: cancelling releases the mouse capture, and
  // WM_CAPTURECHANGED re-enters here to reset drag_ — under the call.
  const std::unique_ptr<PhiSidebarDragSession> session = std::move(drag_);
  session->CancelWithSlideBack();
  return true;
}

bool PhiSidebarView::drag_active() const {
  return drag_ && drag_->active();
}

void PhiSidebarView::OnHoverExpandTimer() {
  if (drag_) {
    drag_->FireHoverExpand();
  }
}

bool PhiSidebarView::OnSlideTimer() {
  if (const HWND shell = shell_hwnd()) {
    ::InvalidateRect(shell, nullptr, FALSE);
  }
  return drag_ && drag_->SlideRunning();
}

void PhiSidebarView::DrawDragIndicator(HDC dc) {
  if (drag_) {
    drag_->DrawIndicator(dc);
  }
}

PhiSlideVec PhiSidebarView::RowSlide(const std::string& guid) const {
  return drag_ ? drag_->RowSlideOffset(guid) : PhiSlideVec{};
}

double PhiSidebarView::TabSlide(TabId tab_id) const {
  return drag_ ? drag_->DragSlideOffset(tab_id) : 0.0;
}

void PhiSidebarView::BeginStripLanding(const SidebarKey& key, int drop_left) {
  CancelStripLanding();
  landing_armed_key_ = key;
  landing_drop_left_ = drop_left;
  landing_armed_at_ = ::GetTickCount64();
}

void PhiSidebarView::StartStripLanding(const PhiSidebarRects& layout) {
  if (landing_armed_key_.empty()) {
    return;
  }
  const SidebarKey key = landing_armed_key_;
  landing_armed_key_ = {};
  // The move's own rebuild is posted right behind the drop; a rebuild
  // arriving much later is someone else's, and gliding then would be a
  // tab jumping for no reason.
  constexpr ULONGLONG kStaleMs = 500;
  if (!layout.horizontal_tabs ||
      ::GetTickCount64() - landing_armed_at_ > kStaleMs) {
    return;
  }
  const TabRow* landed = nullptr;
  for (const TabRow& row : tabs_) {
    if (row.key == key && row.bounds.right > row.bounds.left) {
      landed = &row;
      break;
    }
  }
  if (!landed) {
    return;
  }
  const double from = landing_drop_left_ - landed->bounds.left;
  if (std::fabs(from) < 1.0) {
    return;  // Released in its slot: nothing to glide.
  }
  landing_key_ = key;
  landing_from_ = from;
  landing_dx_ = from;
  // The band the glide sweeps: the strip's row, either side of the slot.
  const RECT band = {layout.tabs_left, layout.tabs_top, layout.tabs_limit,
                     layout.tabs_top + Scaled(kHTabHeight)};
  landing_animation_ = PhiAnimator::ForThread().Animate(
      shell_hwnd(), kLandingMs, PhiEasing::kEaseInOut,
      [this, band](double t, RECT* dirty) {
        landing_dx_ = landing_from_ * (1.0 - t);
        *dirty = band;
      },
      [this, band] {
        landing_animation_ = 0;
        landing_key_ = {};
        landing_dx_ = 0.0;
        if (const HWND shell = shell_hwnd()) {
          ::InvalidateRect(shell, &band, FALSE);  // The lifted look goes.
        }
      });
}

void PhiSidebarView::CancelStripLanding() {
  landing_armed_key_ = {};
  if (landing_animation_) {
    PhiAnimator::ForThread().Cancel(landing_animation_);
    landing_animation_ = 0;
  }
  landing_key_ = {};
  landing_dx_ = 0.0;
}

RECT PhiSidebarView::RowHoverRect(HoverKind kind, int index) const {
  const auto at = [&](const auto& rows) {
    return index >= 0 && index < static_cast<int>(rows.size())
               ? rows[static_cast<size_t>(index)].bounds
               : RECT{};
  };
  // In a scrolling band a row's hover also shows (or, leaving, hides) the
  // band's overlay scrollbar: the dirty rect reaches its whole gutter.
  const auto with_gutter = [&](RECT r, bool stored) {
    const PhiSidebarRects layout = shell_->SidebarRects();
    if (layout.horizontal_tabs || r.right <= r.left ||
        (stored ? stored_scroll_max_ : tabs_scroll_max_) <= 0) {
      return r;
    }
    const RECT gutter = {
        layout.sidebar.right - Scaled(kScrollbarWidth + 2 * kScrollbarInset),
        stored ? layout.stored_band.top : layout.tabs_top,
        layout.sidebar.right,
        stored ? layout.stored_band.bottom : layout.tabs_limit};
    ::UnionRect(&r, &r, &gutter);
    return r;
  };
  switch (kind) {
    case HoverKind::kTab:
      return with_gutter(at(tabs_), false);
    case HoverKind::kPin:
      return with_gutter(at(pinned_), true);
    case HoverKind::kBookmark:
      return with_gutter(at(bookmarks_), true);
    case HoverKind::kSpaceRoot:
      return with_gutter(space_root_.bounds, true);
    case HoverKind::kGroupHeader: {
      RECT r = with_gutter(at(group_headers_), false);
      if (index >= 0 && index < static_cast<int>(group_headers_.size()) &&
          group_headers_[static_cast<size_t>(index)].chip) {
        // The hairlines on either side of a chip hide while it is hovered:
        // they repaint with it.
        ::InflateRect(&r, Scaled(kChipTrailGap) + 1, 0);
      }
      return r;
    }
    default:
      return RECT{};  // Not one of the sidebar's own rows.
  }
}

// The rows as assistive technology and tooltips read them (§3.6, ranked
// finding 31): ONE hit per row, in paint order, named by what the row
// shows and keyed by its position in the band so a provider handed out
// before a rebuild resolves back to the same row afterwards. Built HERE,
// with the rows — never from paint (PhiWindow's contract).
void PhiSidebarView::PublishHits(const PhiSidebarRects& layout) {
  hits_.clear();
  const auto add = [&](const RECT& bounds, int key, std::wstring name,
                       PhiHitRole role, bool selected) {
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top ||
        name.empty()) {
      return;
    }
    PhiHit hit;
    hit.bounds = {bounds.left, bounds.top, bounds.right, bounds.bottom};
    hit.key = key;
    hit.name = std::move(name);
    hit.role = role;
    hit.selected = selected;
    hits_.push_back(std::move(hit));
  };
  // The keys are banded so a pin, a bookmark, a header and a tab row at the
  // same position never collide.
  constexpr int kPinKey = 0x1000;
  constexpr int kBookmarkKey = 0x2000;
  constexpr int kHeaderKey = 0x3000;
  constexpr int kTabKey = 0x4000;
  // The keyboard ring's row, as UI Automation's focused element.
  int focused_hit = -1;
  const auto note_focus = [&](const SidebarKey& key, size_t before) {
    if (kb_focus_ && key == kb_key_ && hits_.size() > before) {
      focused_hit = static_cast<int>(before);
    }
  };
  for (size_t i = 0; i < pinned_.size(); ++i) {
    const ShortcutRow& pin = pinned_[i];
    const size_t before = hits_.size();
    add(pin.bounds, kPinKey + static_cast<int>(i), pin.title,
        PhiHitRole::kListItem, pin.active);
    note_focus(pin.key, before);
  }
  if (space_root_.shown) {
    // Announced as the group it opens, its expanded state on the selection
    // flag, like a folder.
    const size_t before = hits_.size();
    add(space_root_.bounds, kBookmarkKey - 1, space_root_.title,
        PhiHitRole::kGroup, space_root_.expanded);
    note_focus(SpaceRootKey(), before);
  }
  for (size_t i = 0; i < bookmarks_.size(); ++i) {
    const ShortcutRow& row = bookmarks_[i];
    // A folder is a container the outline opens, not a page: it announces
    // as a group, and its expanded state rides the selection flag.
    const size_t before = hits_.size();
    add(row.bounds, kBookmarkKey + static_cast<int>(i), row.title,
        row.folder ? PhiHitRole::kGroup : PhiHitRole::kListItem,
        row.folder ? row.expanded : row.active);
    note_focus(row.key, before);
    if (row.close_box.right > row.close_box.left) {
      add(row.close_box, kBookmarkKey + static_cast<int>(i) + 0x800,
          PhiStr("app.closeTab"), PhiHitRole::kButton, false);
    }
  }
  for (size_t i = 0; i < group_headers_.size(); ++i) {
    const GroupHeader& header = group_headers_[i];
    const size_t before = hits_.size();
    add(header.bounds, kHeaderKey + static_cast<int>(i), header.title,
        PhiHitRole::kGroup, !header.collapsed);
    note_focus(SidebarKey::Group(header.id), before);
    if (header.chip) {
      // The chip's chevron is its own button: the body opens the overview,
      // so a screen reader needs the toggle by name.
      add(header.collapse_box, kHeaderKey + static_cast<int>(i) + 0x800,
          PhiStr(header.collapsed
                     ? "sidebar.tabGroupContextMenu.expandAction"
                     : "sidebar.tabGroupContextMenu.collapseAction"),
          PhiHitRole::kButton, false);
    }
  }
  for (size_t i = 0; i < tabs_.size(); ++i) {
    const TabRow& row = tabs_[i];
    const size_t before = hits_.size();
    add(row.bounds, kTabKey + static_cast<int>(i), row.title,
        PhiHitRole::kTab, row.active);
    note_focus(row.key, before);
    // The close box is its own button: a screen reader has to be able to
    // close a tab without simulating a click on a coordinate.
    add(row.close_box, kTabKey + static_cast<int>(i) + 0x800,
        PhiStr("app.closeTab"),
        PhiHitRole::kButton, false);
  }
  if (layout.new_tab_row.right > layout.new_tab_row.left) {
    add(layout.new_tab_row, kTabKey - 1, PhiStr("app.newTab"),
        PhiHitRole::kButton, false);
  }
  // Comfortable's overflow chevron (empty unless the strip overflows).
  const size_t before_chevron = hits_.size();
  add(strip_overflow_button_, kTabKey - 2, PhiStr("tabs.strip.overflowButton"),
      PhiHitRole::kButton, false);
  note_focus(OverflowChevronKey(), before_chevron);
  shell_->SidebarPublishHits(hits_);
  if (kb_focus_ || kb_uia_published_) {
    // The ring as UI Automation's focus (the hits were just republished, so
    // the index is the shell's): announced when it moved, cleared when the
    // keyboard leaves the sidebar.
    PhiUiaSetFocusedHit(shell_->SidebarOwner(), kb_focus_ ? focused_hit : -1,
                        kb_focus_ && kb_announce_);
    kb_uia_published_ = kb_focus_;
    kb_announce_ = false;
  }
}

// THE writer of the row vectors (docs/sidebar-architecture.md §2.2, §2.7):
// SidebarInputs -> BuildSidebar -> LayoutSidebar -> rows, in one pass, on
// the shell's posted rebuild turn. Nothing else fills tabs_, pinned_,
// bookmarks_ or group_headers_ — not paint, not input, not a menu.
void PhiSidebarView::Rebuild() {
  if (DragContextChanged()) {
    // A Space switch, a layout change or the band collapsing under a drag:
    // the rows about to be built are not the ones it was dropping among.
    AbortDrag();
  }
  tabs_.clear();
  group_headers_.clear();
  // Re-captured from the active row below; empty when the strip is
  // (placeholder mode), so no stale URL keeps a native page mounted.
  active_tab_url_.clear();
  active_tab_id_ = TabId();
  if (!framework_) {
    PublishHits(PhiSidebarRects{});
    return;
  }
  const BrowserId browser = shell_->SidebarActiveBrowser();
  if (!browser.valid()) {
    // Between the last browser going and teardown: no rows.
    PublishHits(PhiSidebarRects{});
    return;
  }
  // ONE window read per rebuild — the model's copy of the last committed
  // snapshot (docs/architecture-review.md §3.3); every row below derives
  // from it and the tree. A browser the model has not heard of yet (adopted
  // before its first push) has an empty strip for this turn: the stored
  // cells still render, the push that follows fills the rest.
  const WindowSnapshot empty;
  const WindowSnapshot* known = model_ ? model_->Get(browser) : nullptr;
  const WindowSnapshot& snapshot = known ? *known : empty;

  // Group visuals for the header rows (title/color/collapsed by id).
  std::map<std::string, GroupHeader> group_info;
  std::map<std::string, std::wstring> group_color_labels;
  for (const phi_framework::GroupSnapshot& group : snapshot.groups) {
    GroupHeader info;
    info.id = group.id;
    const PhiGroupColor& palette_entry = PhiGroupColorFor(group.color);
    info.color = palette_entry.color;
    info.name = base::UTF8ToWide(group.title);
    // An unnamed group is titled by its colour and member count
    // ("Grey · 2 tabs", displayTitle(memberCount:)); the count is filled
    // in once the strip has been walked.
    info.title = info.name;
    group_color_labels[info.id] =
        PhiStr((std::string("tabs.tabGroup.colorName.") +
                palette_entry.name).c_str());
    info.collapsed = group.collapsed;
    group_info[info.id] = std::move(info);
  }

  // The model: what every live tab IS — plain, pin-bound, bookmark-bound,
  // split pane, surface, grouped (docs/sidebar-architecture.md §4.5). A
  // reading surface still in the strip is never a row; an active one hands
  // the selection, and the address bar, to the tab it is covering.
  {
    SidebarInputs in;
    if (sidebar_host_ && prefs_host_) {
      in.stored = sidebar_host_->SidebarStoredItems(slot_.value);
      in.surfaces = sidebar_host_->AttachedSurfaces(browser.value);
      in.policy.horizontal_layout =
          prefs_host_->LayoutMode() == PhiLayoutMode::kComfortable;
    }
    in.policy.incognito = incognito();
    for (const TabSnapshot& tab : snapshot.tabs) {
      in.strip.push_back({tab.id.value, tab.index, tab.tag, tab.split_id,
                          tab.group_id, tab.active, PhiSurfaceOf(tab),
                          tab.origin.value});
    }
    for (const auto& [id, info] : group_info) {
      in.groups.push_back({id, info.collapsed});
    }
    sidebar_tree_ = BuildSidebar(in);
  }
  active_tab_id_ = TabId(sidebar_tree_.active_tab);
  if (const TabSnapshot* active = PhiFindTabIn(snapshot, active_tab_id_)) {
    // The bookmark bar's "show on new tab" and the native new-tab page
    // read this.
    active_tab_url_ = active->url;
  }
  // The bookmark bar and the reader slot follow the active tab: re-read
  // them once here, before the layout the rows are placed in is derived.
  shell_->SidebarRefreshLayoutInputs();

  // The two stored bands' sizes feed the layout, and they come from the
  // tree — the pinned grid's row count and the outline's visible rows — so
  // ComputeLayout runs ONCE here, before any row is placed. (It used to be
  // called from inside the shortcut rebuild it depended on.)
  const bool comfortable =
      prefs_host_ && prefs_host_->LayoutMode() == PhiLayoutMode::kComfortable;
  // The scroll state is the Space's own: a switch files the leaving
  // Space's offsets and brings back the incoming one's (fresh at the top).
  if (const std::string space = shell_->SidebarActiveSpace();
      space != scroll_space_) {
    scroll_by_space_[scroll_space_] = {scroll_offset_, stored_scroll_,
                                       strip_first_};
    const auto saved = scroll_by_space_.find(space);
    const ScrollState state =
        saved != scroll_by_space_.end() ? saved->second : ScrollState{};
    scroll_offset_ = state.list;
    stored_scroll_ = state.stored;
    strip_first_ = state.strip_first;
    scroll_space_ = space;
  }
  {
    std::vector<int> units;
    units.reserve(sidebar_tree_.pins.size());
    for (const SidebarCell& pin : sidebar_tree_.pins) {
      units.push_back(pin.split ? 2 : 1);
    }
    const bool isolated =
        incognito() || agent_ || PhiIsAgentSpaceId(shell_->SidebarActiveSpace());
    stored_bands_isolated_ = isolated;
    pinned_grid_rows_ =
        comfortable || isolated
            ? 0
            : LayoutPinnedGrid(units, GridMetrics(PhiSidebarRects{})).rows;
    // Only the vertical list has the outline; Comfortable's bar reads the
    // tree whatever the root says.
    space_root_.shown =
        !isolated && !comfortable && !sidebar_tree_.bookmarks.empty();
    space_root_.expanded =
        !space_root_.shown || !prefs_host_ ||
        !prefs_host_->BookmarkRootCollapsed(shell_->SidebarActiveSpace());
    space_root_.title =
        space_root_.shown ? shell_->SidebarActiveSpaceName() : std::wstring();
    space_root_.contains_active =
        space_root_.shown && !space_root_.expanded &&
        std::ranges::any_of(sidebar_tree_.bookmarks, [](const SidebarCell& cell) {
          return cell.binding.active;
        });
    shell_->SidebarSetRowCounts(
        pinned_grid_rows_,
        isolated ? 0
                 : static_cast<int>(VisibleBookmarkCells().size()) +
                       (space_root_.shown ? 1 : 0));
  }
  const PhiSidebarRects layout = shell_->SidebarRects();
  // The stored bands' scroll range: how far their natural extent runs past
  // the capped viewport. Clamped before any stored row is placed.
  stored_scroll_max_ = 0;
  if (!layout.horizontal_tabs &&
      layout.stored_band.bottom > layout.stored_band.top) {
    const LONG natural_bottom =
        std::max(layout.favorites.bottom, layout.bookmarks.bottom);
    stored_scroll_max_ =
        std::max(0, static_cast<int>(natural_bottom - layout.stored_band.bottom));
  }
  stored_scroll_ = std::max(0, std::min(stored_scroll_, stored_scroll_max_));
  RebuildShortcuts(layout, snapshot);
  // A pinned cell or a bookmark that became active — or the keyboard's
  // row — scrolled out of the stored viewport comes back into it.
  if (stored_scroll_max_ > 0) {
    RECT target = {};
    const bool newly_active = active_tab_id_ != revealed_stored_tab_;
    for (const std::vector<ShortcutRow>* rows : {&pinned_, &bookmarks_}) {
      for (const ShortcutRow& row : *rows) {
        if ((!reveal_key_.empty() && row.key == reveal_key_) ||
            (reveal_key_.empty() && newly_active && row.active)) {
          target = row.frame;
        }
      }
    }
    if (space_root_.shown && !reveal_key_.empty() &&
        reveal_key_ == SpaceRootKey()) {
      target = space_root_.frame;
    }
    const RECT& band = layout.stored_band;
    int next = stored_scroll_;
    if (target.bottom > target.top && target.top < band.top) {
      next -= band.top - target.top;
    } else if (target.bottom > target.top && target.bottom > band.bottom) {
      next += target.bottom - band.bottom;
    }
    next = std::max(0, std::min(next, stored_scroll_max_));
    if (next != stored_scroll_) {
      stored_scroll_ = next;
      RebuildShortcuts(layout, snapshot);  // Placed again at the new scroll.
    }
  }
  revealed_stored_tab_ = active_tab_id_;

  // One paint row per content node; a split node is ONE merged row.
  const auto fill_pane = [&](const TabSnapshot& tab, std::wstring* title,
                             std::wstring* url,
                             std::vector<unsigned char>* favicon, int* fw,
                             int* fh) {
    std::string label = !tab.title.empty()
                            ? tab.title
                            : (!tab.url.empty() ? tab.url
                                                : std::string("(untitled)"));
    if (PhiBookmarkManager::IsManagerUrl(tab.url)) {
      // The Chromium view behind the native page has no WebUI to load, so
      // its title would be the failed URL; the tab shows the page's name.
      label = "Bookmarks";
    }
    *title = base::UTF8ToWide(label);
    if (incognito() && PhiNewTabPage::IsNewTabUrl(tab.url)) {
      // The Mac's incognito new-tab title (Tab.incognitoNewTabTitle) in
      // place of Chromium's plain "New Tab".
      *title = PhiStr("common.newTabPage.incognitoTitle");
    }
    *url = base::UTF8ToWide(tab.url);
    int w = 0, h = 0;
    const unsigned char* pixels =
        framework_->TabFaviconBgraById(browser.value, tab.id.value, &w, &h);
    if (pixels && w > 0 && h > 0) {
      // The framework's buffer is scratch — valid only until its next call.
      const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
      // SAFETY: TabFaviconBgraById reports the dimensions of the buffer it
      // just returned.
      const auto copy =
          UNSAFE_BUFFERS(base::span<const unsigned char>(pixels, bytes));
      favicon->assign(copy.begin(), copy.end());
      *fw = w;
      *fh = h;
    }
  };
  const auto make_row = [&](const SidebarContentNode& node) {
    TabRow row;
    const TabSnapshot* a = PhiFindTabIn(snapshot, TabId(node.tab_a));
    if (!a) {
      return row;
    }
    row.key = node.key;
    row.attached_surface = node.attached_surface;
    row.index = a->index;
    row.id = a->id;
    row.active = node.active;
    row.split_id = a->split_id;
    row.group_id = node.group_id;
    row.tag = a->tag;
    row.internal_page = a->url.rfind("chrome://", 0) == 0;
    row.unloaded = a->unloaded;
    fill_pane(*a, &row.title, &row.url, &row.favicon, &row.favicon_width,
              &row.favicon_height);
    if (node.kind == SidebarNodeKind::kSplit) {
      if (const TabSnapshot* b = PhiFindTabIn(snapshot, TabId(node.tab_b))) {
        row.b_index = b->index;
        row.b_id = b->id;
        row.b_active = b->active;
        fill_pane(*b, &row.b_title, &row.b_url, &row.b_favicon,
                  &row.b_favicon_width, &row.b_favicon_height);
      }
    }
    return row;
  };

  // The native-page routes (bookmark manager, new-tab page) can only be
  // entered or left by a rebuild; mounting a page here would run inside
  // WM_PAINT, so post and do it after.
  shell_->SidebarSyncNativePages();

  // Geometry for the content nodes.
  int x = layout.tabs_left;
  int y = layout.tabs_top;
  if (layout.horizontal_tabs && !pinned_.empty()) {
    // The strip's pinned section leads; normal chips start after it.
    x = pinned_.back().bounds.right + Scaled(kStripPinToTabs);
  }
  // Chrome's strip shares its room out: tabs take their ideal width while
  // they fit and shrink toward the minimum as more open. Fixed widths meant
  // the tabs past the edge were simply dropped — and the ACTIVE tab was
  // among them as soon as a fourth opened, which is not a strip at all.
  // Resting tabs give up their width first; the active one keeps its wider
  // slot until they are all at the floor.
  StripWidths strip_widths{Scaled(kHTabWidth), Scaled(kHTabActiveWidth)};
  // The strip's group chips (the Mac's TabGroupChipView, placed by
  // TabStripLayoutEngine.layoutNormalWithGroups): a chip stands before each
  // group's first member, measured BEFORE the tabs so they share only the
  // room the chips leave, and a collapsed group's members take no slot at
  // all — the chip's favicon mosaic is what stands for them.
  std::map<std::string, GroupHeader> strip_chips;
  StripChipMetrics chip_metrics;
  chip_metrics.leading = Scaled(kChipLeading);
  chip_metrics.dot = Scaled(kChipDot);
  chip_metrics.dot_to_label = Scaled(kChipDotToLabel);
  chip_metrics.label_slack = Scaled(kChipLabelSlack);
  chip_metrics.label_right_pad = Scaled(kChipLabelRightPad);
  chip_metrics.count_to_label = Scaled(kChipCountToLabel);
  chip_metrics.count_h_pad = Scaled(kChipCountHPad);
  chip_metrics.count_v_pad = Scaled(kChipCountVPad);
  chip_metrics.mosaic = Scaled(kChipMosaic);
  chip_metrics.collapse_gap = Scaled(kChipCollapseGap);
  chip_metrics.collapse_control = Scaled(kChipCollapseControl);
  chip_metrics.max_width = Scaled(kChipMaxWidth);
  if (layout.horizontal_tabs) {
    EnsureChipFonts();
    const HFONT semibold_font = shell_->SidebarSemiboldFont();
    std::map<std::string, int> member_counts;
    for (const SidebarContentNode& node : sidebar_tree_.content) {
      if (node.kind == SidebarNodeKind::kGroup || node.group_id.empty()) {
        continue;
      }
      GroupHeader* chip = nullptr;
      if (const auto visuals = group_info.find(node.group_id);
          visuals != group_info.end()) {
        chip = &strip_chips.try_emplace(node.group_id, visuals->second)
                    .first->second;
      }
      for (const int id : {node.tab_a, node.tab_b}) {
        if (!id) {
          continue;
        }
        ++member_counts[node.group_id];
        if (!chip || chip->icons.size() >= 4) {
          continue;
        }
        // The mosaic previews the first four members (collectMosaic-
        // FaviconData): the favicon, or the Phi mark for an internal page.
        GroupHeader::MosaicIcon icon;
        if (const TabSnapshot* tab = PhiFindTabIn(snapshot, TabId(id))) {
          icon.internal_page = tab->url.rfind("chrome://", 0) == 0;
          int w = 0, h = 0;
          const unsigned char* pixels =
              framework_->TabFaviconBgraById(browser.value, id, &w, &h);
          if (pixels && w > 0 && h > 0) {
            const size_t bytes =
                static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
            // SAFETY: TabFaviconBgraById reports the dimensions of the
            // buffer it just returned.
            const auto copy = UNSAFE_BUFFERS(
                base::span<const unsigned char>(pixels, bytes));
            icon.bgra.assign(copy.begin(), copy.end());
            icon.width = w;
            icon.height = h;
          }
        }
        chip->icons.push_back(std::move(icon));
      }
    }
    int chips_room = 0;
    for (auto& [id, chip] : strip_chips) {
      chip.chip = true;
      chip.member_count = member_counts[id];
      chip.user_titled = !chip.name.empty();
      if (!chip.user_titled) {
        chip.title = PhiStrF("tabs.tabGroup.summaryWithTabCount",
                             {group_color_labels[id],
                              std::to_wstring(chip.member_count)});
      }
      const int width = SolveStripChipWidth(
          MeasureText(semibold_font, chip.title),
          MeasureText(chip_count_font_, std::to_wstring(chip.member_count)),
          chip.collapsed, chip.user_titled, chip_metrics);
      // Placed below; until then the rect carries the measured width.
      chip.bounds = {0, 0, width, 0};
      chips_room += Scaled(kChipLeadGap) + width + Scaled(kChipTrailGap);
    }
    int resting = 0;
    bool has_active = false;
    for (const SidebarContentNode& node : sidebar_tree_.content) {
      if (node.kind == SidebarNodeKind::kGroup || node.hidden) {
        continue;  // Chips are measured; a collapsed group's members hide.
      }
      if (node.active) {
        has_active = true;
      } else {
        ++resting;
      }
    }
    strip_widths = SolveStripWidths(
        resting, has_active, layout.tabs_limit - x - chips_room,
        Scaled(kHTabWidth), Scaled(kHTabActiveWidth), Scaled(kHTabMinWidth));
  }
  const auto finish_row = [&](TabRow* row) {
    const int close_box = Scaled(kCloseBoxSize);
    const int box_mid = (row->bounds.top + row->bounds.bottom) / 2;
    row->close_box = {row->bounds.right - close_box - Scaled(4),
                      box_mid - close_box / 2, row->bounds.right - Scaled(4),
                      box_mid + close_box / 2};
    if (row->attached_surface != SidebarSurface::kContent) {
      // The indicator takes the trailing slot; the row's own close box is
      // withheld while a surface is attached (its minus closes the surface).
      // A row scrolled out of the viewport has no slot to take.
      row->surface_box = row->bounds.right > row->bounds.left ? row->close_box
                                                              : RECT{};
      row->close_box = RECT{};
    }
    if (row->b_index >= 0) {
      // Right half of the merged row activates pane B (clicks in the close
      // box still win — it is tested first).
      row->pane_b = {(row->bounds.left + row->bounds.right) / 2,
                     row->bounds.top, row->bounds.right, row->bounds.bottom};
    }
  };
  int list_scroll_max = 0;
  strip_overflow_button_ = RECT{};
  if (layout.horizontal_tabs) {
    // The strip: a chip before each group's first member, the tabs abutting
    // left to right after it, a collapsed group's members hidden under its
    // chip. The tabs' widths are already solved around the chips.
    //
    // Past the tabs' floor width the items no longer fit, and the strip
    // becomes a WINDOW onto them (the Mac's scrolling strip, stepped by
    // whole items here so nothing is ever cut in half): strip_first_ items
    // scroll out ahead of it, the ones past its end hide, the overflow
    // chevron stands before the "+". A scrolled-out item keeps its row —
    // empty bounds, as a list row scrolled out of the sidebar — so the MRU,
    // the switcher, the app menu and the group menus still see it.
    struct StripItem {
      RAW_PTR_EXCLUSION const SidebarContentNode* node = nullptr;
      int lead = 0;   // Room before it (a chip's lead gap).
      int width = 0;
      int trail = 0;  // Room after it (the tab gap, a chip's trail gap).
    };
    std::vector<StripItem> items;
    int active_item = -1;
    for (const SidebarContentNode& node : sidebar_tree_.content) {
      if (node.kind == SidebarNodeKind::kGroup) {
        const auto measured = strip_chips.find(node.group_id);
        if (measured == strip_chips.end()) {
          continue;  // No visuals for it yet: members render flat.
        }
        items.push_back({&node, Scaled(kChipLeadGap),
                         static_cast<int>(measured->second.bounds.right),
                         Scaled(kChipTrailGap)});
        continue;
      }
      if (node.hidden || !PhiFindTabIn(snapshot, TabId(node.tab_a))) {
        continue;  // Under a collapsed chip, or not in the window yet.
      }
      if (node.active) {
        active_item = static_cast<int>(items.size());
      }
      // The active tab takes the wider slot — it is the one carrying the
      // page, and the extra room is what separates it from its neighbours
      // now that the tabs abut instead of standing apart.
      items.push_back({&node, 0,
                       node.active ? strip_widths.active : strip_widths.rest,
                       Scaled(kHTabGap)});
    }
    const int count = static_cast<int>(items.size());
    // A new activation — or a strip that changed under it (resized, a tab
    // opened or closed) — scrolls the active tab into the window, once: a
    // wheel may still move away from it afterwards (the Mac's
    // scrollToMakeTabVisible). A tab just dropped on the strip is revealed
    // the same way: a drop past the window's last item lands it at the
    // strip's end.
    int reveal_item = -1;
    if (!landing_armed_key_.empty()) {
      for (int i = 0; i < count; ++i) {
        if (items[static_cast<size_t>(i)].node->key == landing_armed_key_) {
          reveal_item = i;
          break;
        }
      }
    }
    for (int i = 0; reveal_item < 0 && !reveal_key_.empty() && i < count;
         ++i) {
      if (items[static_cast<size_t>(i)].node->key == reveal_key_) {
        reveal_item = i;  // The keyboard's item.
      }
    }
    if (reveal_item < 0 && (active_tab_id_ != revealed_active_ ||
                            layout.tabs_limit != revealed_strip_limit_ ||
                            count != revealed_strip_items_)) {
      reveal_item = active_item;
    }
    revealed_active_ = active_tab_id_;
    revealed_strip_limit_ = layout.tabs_limit;
    revealed_strip_items_ = count;
    std::vector<StripSpan> spans;
    spans.reserve(items.size());
    for (const StripItem& item : items) {
      spans.push_back({item.lead, item.width, item.trail});
    }
    // Overflowing, the window gives the chevron a button's room.
    const StripWindow window = SolveStripWindow(
        spans, x, layout.tabs_limit,
        Scaled(kNavButton) + Scaled(kTopBarSpacing), strip_first_,
        reveal_item);
    const bool overflow = window.overflow;
    strip_first_ = window.first;
    strip_first_max_ = window.first_max;
    const int window_end = window.end;
    strip_window_left_ = x;
    strip_window_right_ = window.limit;
    int last_right = x;
    for (int i = 0; i < count; ++i) {
      const StripItem& item = items[static_cast<size_t>(i)];
      const SidebarContentNode& node = *item.node;
      const bool shown = i >= strip_first_ && i < window_end;
      if (node.kind == SidebarNodeKind::kGroup) {
        GroupHeader chip = strip_chips.find(node.group_id)->second;
        if (!shown) {
          // Scrolled out: the group keeps its record (the group menus list
          // it), with no chip to paint or hit — its run starts at its first
          // member in view, if any.
          chip.bounds = RECT{};
          chip.container = RECT{};
          group_headers_.push_back(std::move(chip));
          continue;
        }
        const int width = item.width;
        const int left = x + item.lead;
        // 32pt, centred on the 34pt tab cell (chipY).
        const int top = y + (Scaled(kHTabHeight) - Scaled(kChipHeight)) / 2;
        const SidebarRect placed = {left, top, left + width,
                                    top + Scaled(kChipHeight)};
        chip.bounds = PhiToRect(placed);
        const StripChipPlacement parts = PlaceStripChip(
            placed,
            MeasureText(chip_count_font_, std::to_wstring(chip.member_count)),
            Scaled(kChipCountFont), chip.collapsed, chip.user_titled,
            chip_metrics);
        chip.dot = PhiToRect(parts.dot);
        chip.label = PhiToRect(parts.label);
        chip.count = PhiToRect(parts.count);
        chip.mosaic = PhiToRect(parts.mosaic);
        chip.collapse_box = PhiToRect(parts.collapse);
        // The group's run — the chip alone until a member extends it.
        chip.container = {chip.bounds.left, y, chip.bounds.right,
                          y + Scaled(kHTabHeight)};
        last_right = static_cast<int>(chip.bounds.right);
        x = last_right + item.trail;
        group_headers_.push_back(std::move(chip));
        continue;
      }
      TabRow row = make_row(node);
      if (!row.id.valid()) {
        continue;
      }
      if (shown) {
        // The tab runs from the strip's top inset to the band's bottom
        // edge, where the active one merges into the toolbar.
        row.bounds = {x, y, x + item.width, y + Scaled(kHTabHeight)};
        last_right = x + item.width;
        x = last_right + item.trail;
        finish_row(&row);
      }
      if (shown && !row.group_id.empty() && !group_headers_.empty() &&
          group_headers_.back().id == row.group_id) {
        // A member: the group's underline reaches its trailing edge — and,
        // with the chip scrolled out, starts at the first member in view.
        GroupHeader& chip = group_headers_.back();
        if (chip.container.right <= chip.container.left) {
          chip.container = {row.bounds.left, y, row.bounds.left,
                            y + Scaled(kHTabHeight)};
        }
        chip.container.right = row.bounds.right;
        chip.contains_active = chip.contains_active || row.active;
      }
      tabs_.push_back(std::move(row));
    }
    const int chevron_left = last_right + Scaled(kTopBarSpacing);
    if (overflow && chevron_left + Scaled(kNavButton) <= layout.tabs_limit) {
      // The chevron follows the window's last item, centred on the strip's
      // line, in the room the window held back; the "+" follows it.
      const int btn = Scaled(kNavButton);
      const int left = chevron_left;
      const int strip_mid = (layout.tabs_top + layout.title_bar.bottom) / 2;
      strip_overflow_button_ = {left, strip_mid - btn / 2, left + btn,
                                strip_mid + btn / 2};
      x = left + btn;
    }
  } else {
    // The list: a header row before each group's members, members hidden
    // under a collapsed header, everything scrolled and clipped to the
    // tab-list viewport (LayoutList).
    std::vector<ListItem> items;
    std::vector<const SidebarContentNode*> placed;
    std::map<std::string, int> member_counts;
    for (const SidebarContentNode& node : sidebar_tree_.content) {
      if (node.kind == SidebarNodeKind::kGroup) {
        if (!group_info.count(node.group_id)) {
          continue;  // No visuals for it yet: members render flat.
        }
        items.push_back({ListItem::Kind::kHeader, false});
      } else {
        items.push_back({ListItem::Kind::kRow, node.hidden});
        if (!node.group_id.empty()) {
          ++member_counts[node.group_id];
        }
      }
      placed.push_back(&node);
    }
    // The card's inner insets: an expanded header opens the top inset
    // (less the row gap already there) below itself, the group's last
    // visible member the bottom one. A collapsed card is its header.
    for (size_t i = 0; i < placed.size(); ++i) {
      const SidebarContentNode& node = *placed[i];
      if (node.kind == SidebarNodeKind::kGroup) {
        if (!node.collapsed) {
          items[i].pad_below =
              std::max(0, Scaled(kGroupInnerInset) - Scaled(kTabRowGap));
        }
        continue;
      }
      if (node.group_id.empty() || node.hidden) {
        continue;
      }
      const bool last_member =
          i + 1 == placed.size() ||
          placed[i + 1]->kind == SidebarNodeKind::kGroup ||
          placed[i + 1]->group_id != node.group_id;
      if (last_member) {
        items[i].pad_below = Scaled(kGroupInnerInset);
      }
    }
    ListMetrics m;
    m.left = layout.tabs_left;
    m.right = Scaled(sidebar_dip() - kEdgeInset);
    m.top = layout.tabs_top;
    m.row_height = Scaled(kTabRowHeight);
    m.row_gap = Scaled(kTabRowGap);
    m.header_height = Scaled(kGroupHeaderHeight);
    m.scroll = scroll_offset_;
    m.clip = true;
    m.viewport_top = layout.tabs_top;
    m.viewport_bottom = layout.tabs_limit;
    ListPlacement list = LayoutList(items, m);
    // Rows closed under a scrolled list shrink its range: clamp BEFORE the
    // rows are kept, or the list keeps a blank tail until the next rebuild.
    if (scroll_offset_ > list.scroll_max) {
      scroll_offset_ = list.scroll_max;
      m.scroll = scroll_offset_;
      list = LayoutList(items, m);
    }
    // A new activation scrolls its row into the viewport, once (the Mac
    // outline's scrollRowToVisible) — a later wheel may still move away. Not
    // before the band has a viewport at all (a window not yet sized).
    // The keyboard's row is revealed the same way, whenever it moves.
    const bool has_viewport = m.viewport_bottom > m.viewport_top;
    int reveal_index = -1;
    for (size_t i = 0; has_viewport && !reveal_key_.empty() &&
                       i < placed.size();
         ++i) {
      if (placed[i]->key == reveal_key_) {
        reveal_index = static_cast<int>(i);
        break;
      }
    }
    for (size_t i = 0; has_viewport && reveal_index < 0 &&
                       active_tab_id_ != revealed_active_ && i < placed.size();
         ++i) {
      if (placed[i]->kind != SidebarNodeKind::kGroup && placed[i]->active) {
        reveal_index = static_cast<int>(i);
        break;
      }
    }
    if (reveal_index >= 0) {
      // Empty under a collapsed header: nothing to reveal.
      const SidebarRect& row = list.unclipped[static_cast<size_t>(reveal_index)];
      int next = scroll_offset_;
      if (!row.empty() && row.top < m.viewport_top) {
        next -= m.viewport_top - row.top;
      } else if (!row.empty() && row.bottom > m.viewport_bottom) {
        next += row.bottom - m.viewport_bottom;
      }
      next = std::max(0, std::min(next, list.scroll_max));
      if (next != scroll_offset_) {
        scroll_offset_ = next;
        m.scroll = next;
        list = LayoutList(items, m);
      }
    }
    if (has_viewport) {
      revealed_active_ = active_tab_id_;
    }
    revealed_strip_limit_ = 0;  // Back on the strip, it reveals afresh.
    list_scroll_max = list.scroll_max;
    for (size_t i = 0; i < placed.size(); ++i) {
      const SidebarContentNode& node = *placed[i];
      if (node.kind == SidebarNodeKind::kGroup) {
        GroupHeader header = group_info.at(node.group_id);
        if (header.name.empty()) {
          header.title = PhiStrF(
              "tabs.tabGroup.summaryWithTabCount",
              {group_color_labels[node.group_id],
               std::to_wstring(member_counts[node.group_id])});
        }
        header.bounds = PhiToRect(list.rects[i]);
        // The tinted card starts at the header's placed edge even when the
        // header itself is scrolled out; member rows extend it below.
        header.container = PhiToRect(list.unclipped[i]);
        group_headers_.push_back(std::move(header));
        continue;
      }
      if (node.hidden) {
        continue;  // Hidden under a collapsed header.
      }
      TabRow row = make_row(node);
      if (!row.id.valid()) {
        continue;
      }
      row.bounds = PhiToRect(list.rects[i]);
      const bool in_card = !row.group_id.empty() && !group_headers_.empty() &&
                           group_headers_.back().id == row.group_id;
      if (in_card && row.bounds.right > row.bounds.left) {
        // A member row sits inside the card: its own edgesSpacing from the
        // card's leading edge, the inner table's inset from the trailing.
        row.bounds.left += Scaled(kGroupMemberLeadInset);
        row.bounds.right -= Scaled(kGroupMemberTrailInset);
      }
      finish_row(&row);
      if (in_card && row.bounds.right > row.bounds.left) {
        group_headers_.back().container.bottom =
            row.bounds.bottom + items[i].pad_below;
      }
      tabs_.push_back(std::move(row));
    }
  }
  if (layout.horizontal_tabs) {
    // The "+" follows the last tab, centred on the strip's line — the tabs
    // run from tabs_top to the band's bottom edge, and the band's other
    // controls are centred on that same line. It only exists once the
    // strip's width is known, which is here and not in the layout.
    const int btn = Scaled(kNavButton);
    const int gap = Scaled(kTopBarSpacing);
    const int left = x + gap;
    const int strip_mid = (layout.tabs_top + layout.title_bar.bottom) / 2;
    // tabs_limit already held this button's room back from the strip, so it
    // fits unless the tabs overran — clamp rather than draw under search.
    add_tab_button_ = left + btn + gap <= layout.search_tabs.left
                          ? RECT{left, strip_mid - btn / 2, left + btn,
                                 strip_mid + btn / 2}
                          : RECT{};
  } else {
    add_tab_button_ = RECT{};
    strip_first_max_ = 0;
    // Overflow bookkeeping for the wheel handler; the tinted group cards are
    // clamped to the viewport so they never bleed into the fixed bands.
    tabs_scroll_max_ = list_scroll_max;
    if (scroll_offset_ > tabs_scroll_max_) {
      scroll_offset_ = tabs_scroll_max_;
    }
    for (GroupHeader& gh : group_headers_) {
      gh.container.top =
          std::max(gh.container.top, static_cast<LONG>(layout.tabs_top));
      gh.container.bottom =
          std::min(gh.container.bottom, static_cast<LONG>(layout.tabs_limit));
      if (gh.container.bottom <= gh.container.top) {
        gh.container = RECT{};
      }
    }
  }
  // A tab dropped on the strip glides into the slot it was just given.
  StartStripLanding(layout);
  // MRU bookkeeping for the Ctrl+Tab switcher (the rule itself is pure and
  // host-tested): the active tab moves to the front; ids that left the
  // strip fall out.
  {
    TabId active_id;
    std::vector<TabId> live;
    live.reserve(tabs_.size());
    for (const TabRow& row : tabs_) {
      if (row.active && !active_id.valid()) {
        active_id = row.id;
      }
      live.push_back(row.id);
      if (row.b_index >= 0) {
        live.push_back(row.b_id);
      }
    }
    tab_mru_.Sync(live, active_id);
  }
  if (BandHidden(layout)) {
    // A COLLAPSED sidebar has no rows on the shell. The model above still
    // runs (the active tab, the native pages and the row counts come from
    // it), but the placement it was given is the hidden band's — an empty
    // favorites rect put every pinned cell at the window's corner, painted
    // over the caption band and hit-tested there. Blank the geometry: the
    // rows exist, nothing paints or lands on them. (The floating panel is
    // not this case: while it is up the shell rebuilds in the EXPANDED
    // layout, see PhiShellWindow::RebuildTabs.)
    const RECT none = {};
    for (ShortcutRow& pin : pinned_) {
      pin.bounds = none;
      pin.frame = none;
      pin.surface_box = none;
    }
    for (ShortcutRow& row : bookmarks_) {
      row.bounds = none;
      row.frame = none;
      row.surface_box = none;
    }
    space_root_.bounds = none;
    space_root_.frame = none;
    for (GroupHeader& header : group_headers_) {
      header.bounds = none;
      header.container = none;
    }
    for (TabRow& row : tabs_) {
      row.bounds = none;
      row.pane_b = none;
      row.close_box = none;
      row.surface_box = none;
    }
  }
  // The reveal is consumed; the keyboard's row, if it went (closed, moved
  // into a collapsed group), falls back to the active row or the first.
  reveal_key_ = {};
  if (kb_focus_) {
    const std::vector<SidebarKey> order = KeyboardOrder();
    if (std::find(order.begin(), order.end(), kb_key_) == order.end()) {
      const bool active_listed =
          std::find(order.begin(), order.end(), sidebar_tree_.active_key) !=
          order.end();
      kb_key_ = active_listed ? sidebar_tree_.active_key
                : order.empty() ? SidebarKey()
                                : order.front();
    }
  }
  // The rows the pointer, the keyboard and a screen reader all address.
  PublishHits(layout);
  // A drag in flight re-resolves against the rows it now sees.
  if (drag_) {
    drag_->OnRowsRebuilt();
  }
}

bool PhiSidebarView::incognito() const {
  return incognito_ || model::IsIncognitoSpaceId(shell_->SidebarActiveSpace());
}

void PhiSidebarView::RebuildShortcuts(const PhiSidebarRects& layout,
                                      const WindowSnapshot& snapshot) {
  space_root_.frame = RECT{};
  space_root_.bounds = RECT{};
  pinned_.clear();
  bookmarks_.clear();
  if (!sidebar_host_ || incognito() || agent_ ||
      PhiIsAgentSpaceId(shell_->SidebarActiveSpace())) {
    return;  // Nothing persists from an incognito window or an agent Space.
  }
  const auto consume = [](PhiStoredRowView view) {
    ShortcutRow row;
    row.title =
        base::UTF8ToWide(!view.title.empty() ? view.title : view.url);
    row.guid = std::move(view.guid);
    row.url = std::move(view.url);
    row.folder = view.folder;
    row.favicon = std::move(view.favicon_bgra);
    row.favicon_width = view.favicon_width;
    row.favicon_height = view.favicon_height;
    row.url_b = std::move(view.url_b);
    row.title_b = base::UTF8ToWide(view.title_b);
    row.favicon_b = std::move(view.favicon_b_bgra);
    row.favicon_b_width = view.favicon_b_width;
    row.favicon_b_height = view.favicon_b_height;
    row.icon = std::move(view.icon);
    row.layout = std::move(view.layout);
    // Stored favicons come from disk and from sync: a buffer that is not
    // exactly width*height*4 bytes is dropped here, before any painter.
    const auto checked = [](std::vector<unsigned char>* bgra, int* width,
                            int* height) {
      if (*width <= 0 || *height <= 0 ||
          bgra->size() != static_cast<size_t>(*width) *
                              static_cast<size_t>(*height) * 4) {
        bgra->clear();
        *width = 0;
        *height = 0;
      }
    };
    checked(&row.favicon, &row.favicon_width, &row.favicon_height);
    checked(&row.favicon_b, &row.favicon_b_width, &row.favicon_b_height);
    return row;
  };
  // The tree decides what a cell IS (open, active, which live panes); the
  // store view only supplies what it looks like. Whether a bound pane is
  // dormant is the live tab's own state, read from the snapshot the tree
  // was built from.
  const auto dormant = [&](int tab_id) {
    const TabSnapshot* tab = PhiFindTabIn(snapshot, TabId(tab_id));
    return tab && tab->unloaded;
  };
  const auto bind = [&](const SidebarCell& cell, ShortcutRow* row) {
    row->open = cell.binding.open();
    row->active = cell.binding.active;
    row->tab_id = TabId(cell.binding.tab_a);
    row->tab_id_b = TabId(cell.binding.tab_b);
    row->unloaded = dormant(cell.binding.tab_a);
    row->unloaded_b = dormant(cell.binding.tab_b);
    row->attached_surface = cell.attached_surface;
  };
  std::map<std::string, PhiStoredRowView> pin_views;
  for (PhiStoredRowView& view : sidebar_host_->PinnedRows(slot_.value)) {
    pin_views.emplace(view.guid, std::move(view));
  }
  for (const SidebarCell& cell : sidebar_tree_.pins) {
    const auto view = pin_views.find(cell.guid);
    if (view == pin_views.end()) {
      continue;
    }
    ShortcutRow row = consume(std::move(view->second));
    row.key = cell.key;
    bind(cell, &row);
    pinned_.push_back(std::move(row));
  }
  // The outline (the Mac's NSOutlineView): the tree's depth-first order,
  // only the cells an expanded chain reveals. Views come per parent.
  std::map<std::string, std::map<std::string, PhiStoredRowView>> by_parent;
  for (const SidebarCell* cell : VisibleBookmarkCells()) {
    auto& siblings = by_parent[cell->parent_guid];
    if (siblings.empty()) {
      for (PhiStoredRowView& view :
           sidebar_host_->BookmarkRows(slot_.value, cell->parent_guid)) {
        siblings.emplace(view.guid, std::move(view));
      }
    }
    const auto view = siblings.find(cell->guid);
    if (view == siblings.end()) {
      continue;
    }
    ShortcutRow row = consume(std::move(view->second));
    row.key = cell->key;
    row.depth = cell->depth;
    row.parent_guid = cell->parent_guid;
    row.expanded = row.folder && expanded_bookmarks_.count(row.guid) > 0;
    bind(*cell, &row);
    bookmarks_.push_back(std::move(row));
  }

  // A split cell stores pane B's favicon since schema 2; rows folded from
  // older files may lack it — those fall back to the history cache (misses
  // schedule an async load that repaints on arrival).
  const int64_t icon_browser = shell_->SidebarActiveBrowser().value;
  if (framework_ && icon_browser) {
    const auto fill_favicon_b = [&](ShortcutRow& row) {
      if (row.url_b.empty() || !row.favicon_b.empty()) {
        return;
      }
      int width = 0, height = 0;
      const unsigned char* pixels = framework_->FaviconForUrlBgra(
          icon_browser, row.url_b.c_str(), &width, &height);
      if (pixels && width > 0 && height > 0) {
        const size_t bytes =
            static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        // SAFETY: FaviconForUrlBgra reports its own buffer's dimensions.
        const auto view =
            UNSAFE_BUFFERS(base::span<const unsigned char>(pixels, bytes));
        row.favicon_b.assign(view.begin(), view.end());
        row.favicon_b_width = width;
        row.favicon_b_height = height;
      }
    };
    for (ShortcutRow& pin : pinned_) {
      fill_favicon_b(pin);
    }
    for (ShortcutRow& bookmark : bookmarks_) {
      fill_favicon_b(bookmark);
    }
  }

  if (layout.horizontal_tabs) {
    // No sidebar: pins become the strip's leading pinned section — fixed
    // 28pt cells, 2pt apart, centred on the strip row (the Mac's
    // TabStripMetrics.PinnedTab). Normal chips start after them.
    const int cell = Scaled(kStripPinSize);
    const int mid_y = layout.tabs_top + Scaled(kHTabHeight) / 2;
    int x = layout.tabs_left;
    for (ShortcutRow& pin : pinned_) {
      // A merged split cell takes two strip slots.
      const int w = pin.url_b.empty() ? cell : 2 * cell + Scaled(kStripPinGap);
      pin.bounds = {x, mid_y - cell / 2, x + w, mid_y + cell / 2};
      pin.frame = pin.bounds;
      x += w + Scaled(kStripPinGap);
    }
    const int by = layout.favorites.bottom;
    for (ShortcutRow& row : bookmarks_) {
      row.bounds = {0, by, 0, by};  // Off-strip; bookmarks live in the menu.
      row.frame = row.bounds;
    }
    return;
  }

  // The two stored bands scroll together inside their capped viewport: a
  // row sits at its scrolled frame, and is hit only where the viewport
  // shows it.
  const auto clip_to_band = [&](const RECT& frame) {
    if (layout.stored_band.bottom <= layout.stored_band.top) {
      return frame;
    }
    RECT shown = {};
    ::IntersectRect(&shown, &frame, &layout.stored_band);
    return shown;
  };

  // The pinned grid (the Mac's PinnedTabLayout) — LayoutPinnedGrid, shared
  // with the drag previews. A split cell spans two units.
  {
    std::vector<int> units;
    units.reserve(pinned_.size());
    for (const ShortcutRow& pin : pinned_) {
      units.push_back(pin.url_b.empty() ? 1 : 2);
    }
    const PinnedGridPlacement grid = LayoutPinnedGrid(units, GridMetrics(layout));
    for (size_t i = 0; i < pinned_.size(); ++i) {
      ShortcutRow& pin = pinned_[i];
      pin.frame = PhiToRect(grid.cells[i]);
      pin.bounds = clip_to_band(pin.frame);
      if (pin.attached_surface != SidebarSurface::kContent &&
          pin.bounds.right > pin.bounds.left) {
        // §5.8: an 18 dip tile overhanging the cell's top-right by 2 (the
        // Mac's PinnedPeekBadgeView circle, squared to Fluent).
        const int tile = Scaled(18);
        pin.surface_box = {pin.frame.right + Scaled(2) - tile,
                           pin.frame.top - Scaled(2),
                           pin.frame.right + Scaled(2),
                           pin.frame.top - Scaled(2) + tile};
      }
    }
  }

  // Bookmarks are ordinary rows above the open tabs (36pt + the 2pt gap).
  {
    ListMetrics m;
    m.left = layout.tabs_left;
    m.right = Scaled(sidebar_dip() - kEdgeInset);
    // The outline's band starts under its section label (the shell lays
    // the Bookmarks label between the pinned grid and the rows).
    m.top = layout.bookmarks.bottom > layout.bookmarks.top
                ? layout.bookmarks.top
                : layout.favorites.bottom +
                      (pinned_grid_rows_ > 0 ? Scaled(kFavoritesSpacer) : 0);
    m.row_height = Scaled(kTabRowHeight);
    m.row_gap = Scaled(kTabRowGap);
    m.scroll = stored_scroll_;
    // The root takes the outline's first row.
    const size_t first_row = space_root_.shown ? 1 : 0;
    const std::vector<ListItem> items(bookmarks_.size() + first_row,
                                      ListItem{});
    const ListPlacement placed = LayoutList(items, m);
    if (space_root_.shown) {
      space_root_.frame = PhiToRect(placed.rects[0]);
      space_root_.bounds = clip_to_band(space_root_.frame);
    }
    for (size_t i = 0; i < bookmarks_.size(); ++i) {
      ShortcutRow& row = bookmarks_[i];
      row.frame = PhiToRect(placed.rects[i + first_row]);
      row.bounds = clip_to_band(row.frame);
      if (row.attached_surface != SidebarSurface::kContent &&
          row.bounds.right > row.bounds.left) {
        const int box = Scaled(kCloseBoxSize);
        const int mid = (row.frame.top + row.frame.bottom) / 2;
        row.surface_box = {row.frame.right - box - Scaled(4), mid - box / 2,
                           row.frame.right - Scaled(4), mid + box / 2};
      }
      row.close_box = RECT{};
      if (row.open && !row.folder &&
          row.attached_surface == SidebarSurface::kContent &&
          row.bounds.right > row.bounds.left) {
        const int box = Scaled(kCloseBoxSize);
        const int mid = (row.frame.top + row.frame.bottom) / 2;
        row.close_box = {row.frame.right - box - Scaled(4), mid - box / 2,
                         row.frame.right - Scaled(4), mid + box / 2};
      }
    }
  }
}

PinnedGridMetrics PhiSidebarView::GridMetrics(
    const PhiSidebarRects& layout) const {
  PinnedGridMetrics m;
  m.sidebar_dip = sidebar_dip();
  m.edge_inset_dip = kEdgeInset;
  m.gap_dip = kPinnedGap;
  m.min_cell_width_dip = kPinnedMinWidth;
  m.origin_x = layout.favorites.left + Scaled(kEdgeInset);
  // The grid scrolls with the stored bands; `layout` is the shell's
  // natural one, so the real cells and the drag previews agree.
  m.origin_y = layout.favorites.top - stored_scroll_;
  m.sidebar_px = Scaled(sidebar_dip());
  m.edge_inset_px = Scaled(kEdgeInset);
  m.gap_px = Scaled(kPinnedGap);
  m.cell_height_px = Scaled(kPinnedCellHeight);
  return m;
}

std::vector<RECT> PhiSidebarView::PinnedGridWalk(
    const std::vector<int>& units, const PhiSidebarRects& layout,
    int* rows) const {
  // The pinned grid's placement (LayoutPinnedGrid), shared with the drag
  // previews so a phantom cell lands where the real one will.
  const PinnedGridPlacement grid = LayoutPinnedGrid(units, GridMetrics(layout));
  if (rows) {
    *rows = grid.rows;
  }
  std::vector<RECT> rects;
  for (const SidebarRect& cell : grid.cells) {
    rects.push_back(PhiToRect(cell));
  }
  return rects;
}

// The outline rule itself is pure and host-tested; this hands back the
// cells it names.
std::vector<const SidebarCell*> PhiSidebarView::VisibleBookmarkCells()
    const {
  std::vector<const SidebarCell*> visible;
  if (space_root_.shown && !space_root_.expanded) {
    return visible;
  }
  for (const size_t row :
       PhiVisibleOutlineRows(sidebar_tree_.bookmarks, expanded_bookmarks_)) {
    visible.push_back(&sidebar_tree_.bookmarks[row]);
  }
  return visible;
}



bool PhiSidebarView::Apply(const SidebarCommand& command) {
  return sidebar_host_ &&
         sidebar_host_->Apply(slot_.value, command, sidebar_tree_);
}

base::DictValue PhiSidebarView::DragStateJson() const {
  base::DictValue d;
  if (!drag_ || !drag_->active()) {
    return d;
  }
  const auto kind_name = [](PhiDragKind kind) {
    switch (kind) {
      case PhiDragKind::kTab: return "tab";
      case PhiDragKind::kPin: return "pin";
      case PhiDragKind::kBookmark: return "bookmark";
      case PhiDragKind::kChip: return "chip";
      case PhiDragKind::kNone: break;
    }
    return "none";
  };
  const auto band_name = [](PhiDropBand band) {
    switch (band) {
      case PhiDropBand::kTabs: return "tabs";
      case PhiDropBand::kPins: return "pins";
      case PhiDropBand::kBookmarks: return "bookmarks";
      case PhiDropBand::kOutside: return "outside";
      case PhiDropBand::kContent: return "content";
      case PhiDropBand::kNone: break;
    }
    return "none";
  };
  d.Set("kind", kind_name(drag_->kind()));
  d.Set("key", drag_->key().ToString());
  d.Set("band", band_name(drag_->band()));
  d.Set("gapSlot", drag_->insert_slot());
  d.Set("folderRow", drag_->folder_row());
  d.Set("dropFolder", drag_->drop_folder());
  return d;
}

std::vector<DropRow> PhiSidebarView::TabDropRows() const {
  // The tree's content order with each node's placed rect (empty when it is
  // hidden or scrolled out), headers included so group runs are visible.
  std::vector<DropRow> rows;
  for (const SidebarContentNode& node : sidebar_tree_.content) {
    DropRow row;
    row.key = node.key;
    row.group_id = node.group_id;
    row.group_header = node.kind == SidebarNodeKind::kGroup;
    if (row.group_header) {
      for (const GroupHeader& header : group_headers_) {
        if (header.id == node.group_id) {
          row.bounds = {header.bounds.left, header.bounds.top,
                        header.bounds.right, header.bounds.bottom};
        }
      }
    } else {
      for (const TabRow& tab : tabs_) {
        if (tab.key == node.key) {
          row.bounds = {tab.bounds.left, tab.bounds.top, tab.bounds.right,
                        tab.bounds.bottom};
        }
      }
      if (!node.group_id.empty()) {
        // The card's bottom edge: the zone under the last member that still
        // drops into the group (vertical lists only; a chip has no card).
        for (const GroupHeader& header : group_headers_) {
          if (header.id == node.group_id && !header.chip) {
            row.group_bottom = static_cast<int>(header.container.bottom);
          }
        }
      }
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<DropRow> PhiSidebarView::PinnedDropRows() const {
  std::vector<DropRow> rows;
  // The stored rows' FRAMES: where they sit, scrolled — the same geometry
  // the slide targets (PinnedGridWalk) are walked in.
  for (const ShortcutRow& pin : pinned_) {
    DropRow row;
    row.key = pin.key;
    row.bounds = {pin.frame.left, pin.frame.top, pin.frame.right,
                  pin.frame.bottom};
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<DropRow> PhiSidebarView::BookmarkDropRows() const {
  std::vector<DropRow> rows;
  for (const ShortcutRow& bookmark : bookmarks_) {
    DropRow row;
    row.key = bookmark.key;
    row.bounds = {bookmark.frame.left, bookmark.frame.top,
                  bookmark.frame.right, bookmark.frame.bottom};
    row.depth = bookmark.depth;
    row.folder = bookmark.folder;
    row.expanded = bookmark.expanded;
    row.parent_guid = bookmark.parent_guid;
    rows.push_back(std::move(row));
  }
  return rows;
}

// "Which window shows tab X" — answered from the TREE, not from the paint
// rows (docs/architecture-review.md §3.5; lane 1 §2.1 item 4). Every live
// content tab of this window appears in the tree exactly once: as a content
// node, or in owner_of, hidden inside the pinned or bookmark cell that owns
// it. Walking tabs_ answered only the first kind, so a reader surface
// opened over a PINNED tab's page found no window at all.
bool PhiSidebarView::ShowsTab(TabId tab_id) const {
  if (!tab_id.valid()) {
    return false;
  }
  for (const TabRow& row : tabs_) {
    if (row.id == tab_id || (row.b_index >= 0 && row.b_id == tab_id)) {
      return true;
    }
  }
  return sidebar_tree_.owner_of.count(tab_id.value) > 0;
}

base::ListValue PhiSidebarView::LayoutRowsJson() const {
  base::ListValue rows;
  const auto surface_name = [](SidebarSurface kind) {
    return kind == SidebarSurface::kPeek     ? "peek"
           : kind == SidebarSurface::kReader ? "reader"
                                             : "";
  };
  const auto box = [](const RECT& r) {
    base::DictValue d;
    d.Set("left", static_cast<int>(r.left));
    d.Set("top", static_cast<int>(r.top));
    d.Set("right", static_cast<int>(r.right));
    d.Set("bottom", static_cast<int>(r.bottom));
    return d;
  };
  const auto emit = [&](const char* band, const SidebarKey& key,
                        const RECT& r, base::DictValue extra) {
    base::DictValue row = std::move(extra);
    row.Set("band", band);
    row.Set("key", key.ToString());
    row.Set("left", static_cast<int>(r.left));
    row.Set("top", static_cast<int>(r.top));
    row.Set("right", static_cast<int>(r.right));
    row.Set("bottom", static_cast<int>(r.bottom));
    rows.Append(std::move(row));
  };
  for (const ShortcutRow& pin : pinned_) {
    base::DictValue extra;
    extra.Set("guid", pin.guid);
    extra.Set("open", pin.open);
    extra.Set("tabId", pin.tab_id.value);
    extra.Set("unloaded", pin.unloaded);
    extra.Set("unloadedB", pin.unloaded_b);
    extra.Set("surface", surface_name(pin.attached_surface));
    extra.Set("surfaceBox", box(pin.surface_box));
    emit("pinned", pin.key, pin.bounds, std::move(extra));
  }
  for (const ShortcutRow& bookmark : bookmarks_) {
    base::DictValue extra;
    extra.Set("guid", bookmark.guid);
    extra.Set("folder", bookmark.folder);
    extra.Set("expanded", bookmark.expanded);
    extra.Set("depth", bookmark.depth);
    extra.Set("parentGuid", bookmark.parent_guid);
    extra.Set("open", bookmark.open);
    extra.Set("tabId", bookmark.tab_id.value);
    extra.Set("unloaded", bookmark.unloaded);
    extra.Set("unloadedB", bookmark.unloaded_b);
    extra.Set("surface", surface_name(bookmark.attached_surface));
    extra.Set("surfaceBox", box(bookmark.surface_box));
    emit("bookmarks", bookmark.key, bookmark.bounds, std::move(extra));
  }
  if (space_root_.shown) {
    base::DictValue extra;
    extra.Set("title", base::WideToUTF8(space_root_.title));
    extra.Set("expanded", space_root_.expanded);
    extra.Set("containsActive", space_root_.contains_active);
    emit("spaceRoot", SpaceRootKey(), space_root_.bounds, std::move(extra));
  }
  for (const GroupHeader& header : group_headers_) {
    base::DictValue extra;
    extra.Set("groupId", header.id);
    extra.Set("collapsed", header.collapsed);
    if (header.chip) {
      // The strip chip's parts, so a probe can aim at the chevron or the
      // mosaic rather than guess from pixels.
      extra.Set("chip", true);
      extra.Set("memberCount", header.member_count);
      extra.Set("containsActive", header.contains_active);
      extra.Set("collapseBox", box(header.collapse_box));
      extra.Set("mosaic", box(header.mosaic));
      extra.Set("run", box(header.container));
    }
    emit("tabs", SidebarKey::Group(header.id), header.bounds, std::move(extra));
  }
  for (const TabRow& tab : tabs_) {
    base::DictValue extra;
    extra.Set("tabId", tab.id.value);
    extra.Set("tabIdB", tab.b_id.value);
    extra.Set("groupId", tab.group_id);
    extra.Set("active", tab.active);
    extra.Set("unloaded", tab.unloaded);
    extra.Set("surface", surface_name(tab.attached_surface));
    extra.Set("surfaceBox", box(tab.surface_box));
    emit("tabs", tab.key, tab.bounds, std::move(extra));
  }
  return rows;
}

bool PhiSidebarView::OnWheel(POINT client_pt, int wheel_delta) {
  if (!prefs_host_ || wheel_delta == 0) {
    return false;
  }
  const PhiSidebarRects layout = shell_->SidebarRects();
  if (layout.horizontal_tabs) {
    // Comfortable's strip: one item per notch, a notch toward the user
    // revealing later items (the direction a vertical list scrolls, as the
    // Spaces strip steps its pips) — over the tabs' run only.
    if (strip_first_max_ <= 0 || !::PtInRect(&layout.title_bar, client_pt) ||
        client_pt.x < layout.tabs_left ||
        client_pt.x >= layout.search_tabs.left) {
      return false;
    }
    const int next = std::max(
        0, std::min(strip_first_ + (wheel_delta < 0 ? 1 : -1),
                    strip_first_max_));
    if (next != strip_first_) {
      strip_first_ = next;
      shell_->SidebarScheduleRefresh();
    }
    return true;
  }
  PhiSpacesStrip* const strip = shell_->SidebarSpacesStrip();
  if (::PtInRect(&layout.spaces_strip, client_pt)) {
    // Over the strip the wheel slides the pip viewport by whole pips, one
    // per notch (the Mac's SpacesStripWheelTracker): a notch toward the
    // user reveals later pips, the direction a vertical list would scroll.
    return strip->OnWheel(wheel_delta);
  }
  if (!::PtInRect(&layout.sidebar, client_pt)) {
    return false;
  }
  const int step = Scaled(kTabRowHeight + kTabRowGap);
  if (stored_scroll_max_ > 0 && ::PtInRect(&layout.stored_band, client_pt)) {
    // Over the stored bands' capped viewport: the pinned grid and the
    // outline scroll together, an outline row per notch.
    int next = stored_scroll_ - wheel_delta * step / WHEEL_DELTA;
    next = std::max(0, std::min(next, stored_scroll_max_));
    if (next != stored_scroll_) {
      stored_scroll_ = next;
      ShowScrollbars();
      shell_->SidebarScheduleRefresh();
    }
    return true;
  }
  if (tabs_scroll_max_ <= 0) {
    return false;
  }
  int next = scroll_offset_ - wheel_delta * step / WHEEL_DELTA;
  next = std::max(0, std::min(next, tabs_scroll_max_));
  if (next != scroll_offset_) {
    scroll_offset_ = next;
    ShowScrollbars();
    shell_->SidebarScheduleRefresh();
  }
  return true;
}

PhiSidebarRects PhiSidebarView::ScrolledStoredBands(
    const PhiSidebarRects& layout) const {
  PhiSidebarRects out = layout;
  if (layout.horizontal_tabs ||
      layout.stored_band.bottom <= layout.stored_band.top) {
    return out;
  }
  const auto place = [&](RECT* band) {
    if (band->bottom <= band->top) {
      return;  // No band (a zero-high grid keeps its natural edge).
    }
    ::OffsetRect(band, 0, -stored_scroll_);
    RECT shown = {};
    ::IntersectRect(&shown, band, &layout.stored_band);
    *band = shown;
  };
  place(&out.favorites);
  place(&out.bookmarks);
  return out;
}

// --- The overlay scrollbars --------------------------------------------------

void PhiSidebarView::ShowScrollbars() {
  scrollbar_shown_at_ = ::GetTickCount64();
  const HWND hwnd = shell_hwnd();
  if (!hwnd) {
    return;
  }
  // The trailing gutter both bars live in: repainted on the animator's
  // clock while the bars hold and fade.
  const PhiSidebarRects layout = shell_->SidebarRects();
  const RECT gutter = {
      layout.sidebar.right - Scaled(kScrollbarWidth + 2 * kScrollbarInset),
      layout.sidebar.top, layout.sidebar.right, layout.sidebar.bottom};
  if (scrollbar_animation_) {
    PhiAnimator::ForThread().Cancel(scrollbar_animation_);
  }
  scrollbar_animation_ = PhiAnimator::ForThread().Animate(
      hwnd, kScrollbarHoldMs + kScrollbarFadeMs, PhiEasing::kLinear,
      [gutter](double, RECT* dirty) { *dirty = gutter; },
      [this, hwnd, gutter] {
        scrollbar_animation_ = 0;
        ::InvalidateRect(hwnd, &gutter, FALSE);  // Faded out.
      });
}

double PhiSidebarView::ScrollbarOpacity(bool hovered) const {
  if (hovered) {
    return 1.0;
  }
  if (!scrollbar_shown_at_) {
    return 0.0;
  }
  const ULONGLONG age = ::GetTickCount64() - scrollbar_shown_at_;
  if (age <= static_cast<ULONGLONG>(kScrollbarHoldMs)) {
    return 1.0;
  }
  if (age >= static_cast<ULONGLONG>(kScrollbarHoldMs + kScrollbarFadeMs)) {
    return 0.0;
  }
  return 1.0 - static_cast<double>(age - kScrollbarHoldMs) / kScrollbarFadeMs;
}

void PhiSidebarView::PaintScrollbar(HDC dc,
                                    const RECT& track,
                                    int scroll,
                                    int scroll_max,
                                    const PhiPalette& palette,
                                    double opacity) {
  const int track_h = static_cast<int>(track.bottom - track.top);
  if (scroll_max <= 0 || opacity <= 0.0 ||
      track_h < Scaled(kScrollbarMinThumb)) {
    return;
  }
  // Windows 11's thin overlay bar: the thumb's length is the viewport's
  // share of the content, its travel the scroll's share of the range.
  const long long content = static_cast<long long>(track_h) + scroll_max;
  const int thumb_h = std::min(
      track_h, std::max(Scaled(kScrollbarMinThumb),
                        static_cast<int>(static_cast<long long>(track_h) *
                                         track_h / content)));
  const int top = static_cast<int>(
      track.top + static_cast<long long>(track_h - thumb_h) *
                      std::max(0, std::min(scroll, scroll_max)) / scroll_max);
  const int width = Scaled(kScrollbarWidth);
  const int right = static_cast<int>(track.right) - Scaled(kScrollbarInset);
  const RECT thumb = {right - width, top, right, top + thumb_h};
  const int alpha =
      static_cast<int>((palette.dark ? 110 : 90) * std::min(1.0, opacity));
  PhiFillRoundRectAA(dc, thumb, width / 2,
                     PhiBlendOver(palette.ink, palette.ground_top, alpha));
}

// --- Drag auto-scroll (D31) ---------------------------------------------------

int PhiSidebarView::DragAutoScrollZone(POINT pt, int* target,
                                       int* depth) const {
  const PhiSidebarRects layout = shell_->SidebarRects();
  const int edge = Scaled(kAutoScrollEdge);
  const auto zone = [&](int near_edge, int far_edge, int v) {
    // -1 within `edge` either side of the near (top/left) edge, +1 of the
    // far one; the depth is how far into the zone, capped at its width.
    if (v < near_edge + edge && v >= near_edge - edge) {
      *depth = std::min(edge, near_edge + edge - v);
      return -1;
    }
    if (v > far_edge - edge && v <= far_edge + edge) {
      *depth = std::min(edge, v - (far_edge - edge));
      return 1;
    }
    return 0;
  };
  if (layout.horizontal_tabs) {
    if (strip_first_max_ <= 0 || pt.y < layout.title_bar.top ||
        pt.y >= layout.title_bar.bottom) {
      return 0;
    }
    *target = 2;
    return zone(strip_window_left_, strip_window_right_, pt.x);
  }
  if (BandHidden(layout) || pt.x < layout.sidebar.left ||
      pt.x >= layout.sidebar.right) {
    return 0;
  }
  const RECT& band = layout.stored_band;
  if (stored_scroll_max_ > 0 && pt.y >= band.top && pt.y < band.bottom) {
    // Inside the viewport only: the zones must not reach the header.
    *target = 1;
    if (pt.y < band.top + edge) {
      *depth = static_cast<int>(band.top + edge - pt.y);
      return -1;
    }
    if (pt.y > band.bottom - edge) {
      *depth = static_cast<int>(pt.y - (band.bottom - edge));
      return 1;
    }
    return 0;
  }
  if (tabs_scroll_max_ > 0) {
    *target = 0;
    return zone(layout.tabs_top, layout.tabs_limit, pt.y);
  }
  return 0;
}

void PhiSidebarView::UpdateDragAutoScroll(POINT pt) {
  auto_scroll_pt_ = pt;
  int target = 0;
  int depth = 0;
  if (DragAutoScrollZone(pt, &target, &depth) == 0) {
    StopDragAutoScroll();
    return;
  }
  const HWND hwnd = shell_hwnd();
  if (auto_scroll_animation_ || !hwnd) {
    return;  // Already running: the tick reads the new point.
  }
  auto_scroll_last_ = 0;
  // An open-ended run on the animator's clock (a minute; a drag resting
  // longer re-arms on its next move).
  auto_scroll_animation_ = PhiAnimator::ForThread().Animate(
      hwnd, 60000, PhiEasing::kLinear,
      [this](double, RECT*) { DragAutoScrollTick(); },
      [this] { auto_scroll_animation_ = 0; });
}

void PhiSidebarView::DragAutoScrollTick() {
  if (!drag_ || !drag_->active()) {
    return;  // Stopped by the drag's own end.
  }
  int target = 0;
  int depth = 0;
  const int dir = DragAutoScrollZone(auto_scroll_pt_, &target, &depth);
  if (dir == 0) {
    return;
  }
  bool moved = false;
  if (target == 2) {
    // The strip steps by whole items, at a walking pace.
    const ULONGLONG now = ::GetTickCount64();
    if (auto_scroll_last_ &&
        now - auto_scroll_last_ < static_cast<ULONGLONG>(kAutoScrollStripMs)) {
      return;
    }
    auto_scroll_last_ = now;
    const int next = std::max(0, std::min(strip_first_ + dir, strip_first_max_));
    moved = next != strip_first_;
    strip_first_ = next;
  } else {
    // Faster the deeper into the zone.
    const int edge = Scaled(kAutoScrollEdge);
    const int step =
        std::max(1, Scaled(kAutoScrollMaxStep) * std::max(1, depth) / edge);
    int& scroll = target == 1 ? stored_scroll_ : scroll_offset_;
    const int limit = target == 1 ? stored_scroll_max_ : tabs_scroll_max_;
    const int next = std::max(0, std::min(scroll + dir * step, limit));
    moved = next != scroll;
    scroll = next;
    if (moved) {
      ShowScrollbars();
    }
  }
  if (moved) {
    // The rebuild re-resolves the drag against the moved rows
    // (OnRowsRebuilt).
    shell_->SidebarScheduleRefresh();
  }
}

void PhiSidebarView::StopDragAutoScroll() {
  if (auto_scroll_animation_) {
    PhiAnimator::ForThread().Cancel(auto_scroll_animation_);
    auto_scroll_animation_ = 0;
  }
}

// --- Keyboard (F6) ------------------------------------------------------------

std::vector<SidebarKey> PhiSidebarView::KeyboardOrder() const {
  std::vector<SidebarKey> order;
  for (const ShortcutRow& pin : pinned_) {
    order.push_back(pin.key);
  }
  if (space_root_.frame.right > space_root_.frame.left) {
    order.push_back(SpaceRootKey());
  }
  for (const ShortcutRow& row : bookmarks_) {
    if (row.frame.right > row.frame.left) {
      order.push_back(row.key);  // Not comfortable's parked rows.
    }
  }
  // The content in tree order: a group's header (or chip) before its
  // members, members of a collapsed group skipped.
  for (const SidebarContentNode& node : sidebar_tree_.content) {
    if (node.kind == SidebarNodeKind::kGroup) {
      for (const GroupHeader& header : group_headers_) {
        if (header.id == node.group_id) {
          order.push_back(SidebarKey::Group(header.id));
          break;
        }
      }
      continue;
    }
    if (node.hidden) {
      continue;
    }
    for (const TabRow& row : tabs_) {
      if (row.key == node.key) {
        order.push_back(row.key);
        break;
      }
    }
  }
  if (strip_overflow_button_.right > strip_overflow_button_.left) {
    order.push_back(OverflowChevronKey());
  }
  return order;
}

RECT PhiSidebarView::KeyRect(const SidebarKey& key) const {
  if (key.empty()) {
    return RECT{};
  }
  if (key == OverflowChevronKey()) {
    return strip_overflow_button_;
  }
  for (const std::vector<ShortcutRow>* rows : {&pinned_, &bookmarks_}) {
    for (const ShortcutRow& row : *rows) {
      if (row.key == key) {
        return row.bounds;
      }
    }
  }
  if (key.kind == SidebarKey::Kind::kSpace) {
    return space_root_.bounds;
  }
  if (key.kind == SidebarKey::Kind::kGroup) {
    for (const GroupHeader& header : group_headers_) {
      if (header.id == key.id) {
        return header.bounds;
      }
    }
    return RECT{};
  }
  for (const TabRow& row : tabs_) {
    if (row.key == key) {
      return row.bounds;
    }
  }
  return RECT{};
}

bool PhiSidebarView::BeginKeyboardFocus() {
  const PhiSidebarRects layout = shell_->SidebarRects();
  const std::vector<SidebarKey> order = KeyboardOrder();
  if (BandHidden(layout) || order.empty()) {
    return false;
  }
  // The ring starts on the active row, as a list's focus does.
  const bool active_listed =
      std::find(order.begin(), order.end(), sidebar_tree_.active_key) !=
      order.end();
  // On BEFORE the shell takes the focus: its WM_SETFOCUS hands the focus to
  // the page unless the sidebar holds it.
  kb_focus_ = true;
  if (const HWND hwnd = shell_hwnd()) {
    ::SetFocus(hwnd);
  }
  MoveKeyboardFocus(active_listed ? sidebar_tree_.active_key : order.front());
  return true;
}

void PhiSidebarView::EndKeyboardFocus() {
  if (!kb_focus_) {
    return;
  }
  kb_focus_ = false;
  RECT ring = KeyRect(kb_key_);
  ::InflateRect(&ring, Scaled(3), Scaled(3));
  if (const HWND hwnd = shell_hwnd()) {
    ::InvalidateRect(hwnd, &ring, FALSE);
  }
  shell_->SidebarScheduleRefresh();  // Republishes the hits unfocused.
}

void PhiSidebarView::MoveKeyboardFocus(const SidebarKey& key) {
  const HWND hwnd = shell_hwnd();
  const SidebarKey before = kb_key_;
  for (const SidebarKey* k : {&before, &key}) {
    RECT ring = KeyRect(*k);
    ::InflateRect(&ring, Scaled(3), Scaled(3));
    if (hwnd && ring.right > ring.left) {
      ::InvalidateRect(hwnd, &ring, FALSE);
    }
  }
  kb_key_ = key;
  kb_announce_ = true;
  // The rebuild scrolls the row into view and publishes it to UI Automation.
  reveal_key_ = key;
  shell_->SidebarScheduleRefresh();
}

void PhiSidebarView::ActivateKey(const SidebarKey& key, bool toggle) {
  if (key == OverflowChevronKey()) {
    // As its click: the tab search palette lists every tab.
    PhiSearchTabsPanel::Toggle(shell_->SidebarOwner());
    return;
  }
  switch (key.kind) {
    case SidebarKey::Kind::kSpace:
      ToggleSpaceRoot();
      return;
    case SidebarKey::Kind::kFolder:
      ToggleExpanded(key.id);
      shell_->SidebarScheduleRefresh();
      return;
    case SidebarKey::Kind::kGroup:
      for (const GroupHeader& header : group_headers_) {
        const BrowserId browser = shell_->SidebarActiveBrowser();
        if (header.id == key.id && framework_ && browser.valid()) {
          framework_->SetTabGroupCollapsed(browser.value, header.id.c_str(),
                                           !header.collapsed);
          return;
        }
      }
      return;
    case SidebarKey::Kind::kCell:
      for (const std::vector<ShortcutRow>* rows : {&pinned_, &bookmarks_}) {
        for (const ShortcutRow& row : *rows) {
          if (row.key == key && !row.url.empty()) {
            OpenOrFocusPinned(row.guid, row.url);
            return;
          }
        }
      }
      return;
    case SidebarKey::Kind::kTab:
    case SidebarKey::Kind::kSplit:
      Apply(SidebarActivate{key, 0});
      return;
    case SidebarKey::Kind::kNone:
      break;
  }
  (void)toggle;  // Space on a row that has nothing to toggle activates it.
}

bool PhiSidebarView::OnKeyDown(UINT vk) {
  if (!kb_focus_) {
    return false;
  }
  const std::vector<SidebarKey> order = KeyboardOrder();
  if (order.empty()) {
    EndKeyboardFocus();
    return false;
  }
  const auto found = std::find(order.begin(), order.end(), kb_key_);
  const int at = found == order.end()
                     ? -1
                     : static_cast<int>(found - order.begin());
  const int last = static_cast<int>(order.size()) - 1;
  const auto go = [&](int index) {
    MoveKeyboardFocus(order[static_cast<size_t>(std::max(0, std::min(index, last)))]);
    return true;
  };
  switch (vk) {
    case VK_UP:
    case VK_LEFT:
      return go(at < 0 ? 0 : at - 1);
    case VK_DOWN:
    case VK_RIGHT:
      return go(at + 1);
    case VK_HOME:
      return go(0);
    case VK_END:
      return go(last);
    case VK_RETURN:
    case VK_SPACE:
      if (at >= 0) {
        ActivateKey(order[static_cast<size_t>(at)], vk == VK_SPACE);
      }
      return true;
    case VK_DELETE: {
      if (at < 0 || agent_holds_input_) {
        return true;
      }
      const SidebarKey key = order[static_cast<size_t>(at)];
      if (key.kind != SidebarKey::Kind::kTab &&
          key.kind != SidebarKey::Kind::kSplit) {
        return true;  // Pins and bookmarks are persistent (the menu deletes).
      }
      // The ring moves on to the next row (the previous at the end) before
      // the row goes.
      const int next = at < last ? at + 1 : at - 1;
      if (next >= 0) {
        MoveKeyboardFocus(order[static_cast<size_t>(next)]);
      }
      Apply(SidebarClose{key, -1});
      return true;
    }
    case VK_ESCAPE:
      EndKeyboardFocus();
      if (const HWND hwnd = shell_hwnd()) {
        ::PostMessageW(hwnd, kMsgPhiCycleFocus, kPhiFocusPage, 0);
      }
      return true;
    default:
      return false;
  }
}

bool PhiSidebarView::KeyboardMenuPoint(POINT* client_pt) const {
  if (!kb_focus_) {
    return false;
  }
  const RECT r = KeyRect(kb_key_);
  if (r.right <= r.left || r.bottom <= r.top) {
    return false;
  }
  *client_pt = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
  return true;
}

void PhiSidebarView::OpenOrFocusPinned(const std::string& guid,
                                            const std::string& url) {
  if (!framework_ || guid.empty()) {
    return;
  }
  if (sidebar_host_) {
    sidebar_host_->NoteStoredRowSeen(guid);
  }
  // Split rows route through their own open path (the Mac's
  // openOrFocusPinnedTab pair branch / openBookmark secondary branch).
  for (const ShortcutRow& pin : pinned_) {
    if (!pin.url_b.empty() && pin.guid == guid) {
      OpenStoredSplit(pin);
      return;
    }
  }
  for (const ShortcutRow& bookmark : bookmarks_) {
    if (!bookmark.url_b.empty() && bookmark.guid == guid) {
      OpenStoredSplit(bookmark);
      return;
    }
  }
  const BrowserId browser = shell_->SidebarActiveBrowser();
  if (!browser.valid()) {
    return;
  }
  // The strip as it IS, not as the model last heard it: the row may have
  // been opened a moment ago by this very gesture, and a second open would
  // leave a duplicate bound tab beside the first.
  WindowSnapshot strip;
  if (const TabSnapshot* bound =
          shell_->SidebarPullSnapshot(browser, &strip)
              ? PhiFindTagIn(strip, guid)
              : nullptr) {
    framework_->ActivateTabById(browser.value, bound->id.value);
    return;  // The framework's push schedules the rebuild.
  }
  if (url.empty()) {
    return;
  }
  const TabId created = framework_->OpenTab(browser, url.c_str());
  if (created.valid()) {
    // The Mac's createTab(customGuid:): the new tab carries the row's guid
    // as its identity, and session restore carries it across restarts.
    framework_->SetTabTagById(browser.value, created.value, guid.c_str());
  }
}

void PhiSidebarView::OpenStoredSplit(const ShortcutRow& row) {
  const BrowserId browser = shell_->SidebarActiveBrowser();
  WindowSnapshot strip;
  if (!framework_ || !browser.valid() ||
      !shell_->SidebarPullSnapshot(browser, &strip)) {
    return;
  }
  // Each pane binds by tag: TagFor(guid, false) for pane A, TagFor(guid,
  // true) for pane B — the same for a pinned cell and a split bookmark. The
  // strip is pulled fresh for the reason OpenOrFocusPinned pulls it.
  const auto lookup = [&](const std::string& tag, TabId* id,
                          std::string* split) {
    *id = TabId();
    split->clear();
    if (const TabSnapshot* tab = PhiFindTagIn(strip, tag)) {
      *id = tab->id;
      *split = tab->split_id;
    }
  };
  TabId id_a, id_b;
  std::string split_a, split_b;
  lookup(TagFor(row.guid, false), &id_a, &split_a);
  lookup(TagFor(row.guid, true), &id_b, &split_b);
  // Ensure BOTH panes live (a created pane binds by its own pane tag), then
  // marry them if Chromium lost the split — the Mac's openPinnedSplit /
  // openTwoURLsAsSplit in one path. A pane already in a DIFFERENT split is
  // left alone: the cell reads as partial and nothing is torn apart here.
  // OpenTab answers with the STABLE id, so nothing is re-read between the
  // two opens.
  if (!id_a.valid() && !row.url.empty()) {
    id_a = framework_->OpenTab(browser, row.url.c_str());
    if (id_a.valid()) {
      framework_->SetTabTagById(browser.value, id_a.value,
                                TagFor(row.guid, false).c_str());
    }
  }
  if (!id_b.valid() && !row.url_b.empty()) {
    id_b = framework_->OpenTab(browser, row.url_b.c_str());
    if (id_b.valid()) {
      framework_->SetTabTagById(browser.value, id_b.value,
                                TagFor(row.guid, true).c_str());
    }
  }
  if (!id_a.valid() || !id_b.valid()) {
    if (id_a.valid() || id_b.valid()) {
      framework_->ActivateTabById(browser.value,
                                  (id_a.valid() ? id_a : id_b).value);
    }
    return;
  }
  framework_->ActivateTabById(browser.value, id_a.value);
  if (split_a.empty() && split_b.empty()) {
    // Married in the axis the cell remembers; the framework's default is
    // side-by-side, and Chromium picks the leading pane itself.
    framework_->CreateSplitViewForTabs(
        browser.value, id_a.value, id_b.value,
        row.layout == "horizontal" ? "horizontal" : "vertical");
  }
}

std::string PhiSidebarView::FormSplitOrdered(BrowserId browser,
                                                  TabId primary_id,
                                                  TabId secondary_id,
                                                  const char* layout) {
  if (!framework_ || !primary_id.valid() || !secondary_id.valid() ||
      primary_id == secondary_id) {
    return std::string();
  }
  std::string split(framework_->CreateSplitViewForTabs(
      browser.value, primary_id.value, secondary_id.value, layout));
  if (split.empty()) {
    return split;
  }
  // Chromium normalizes the primary pane to the LOWER-indexed tab, so the
  // pair can land the other way round from what the drop asked for. Reversing
  // only reorders the strip — no tab is re-activated, so no pane blinks. The
  // split was created a line ago, so the model cannot know it yet: pull.
  WindowSnapshot fresh;
  if (!shell_->SidebarPullSnapshot(browser, &fresh)) {
    return split;
  }
  if (const SplitSnapshot* made = PhiFindSplitIn(fresh, split);
      made && made->primary != primary_id) {
    framework_->SwapSplitView(browser.value, split.c_str());
  }
  return split;
}

void PhiSidebarView::UnbindStoredTab(BrowserId browser, TabId tab_id) {
  if (!framework_ || !tab_id.valid()) {
    return;
  }
  // The stored row keeps its place and its url — it simply stops being
  // "open", which is the Mac's placeholder left behind by
  // demotePinnedTabLeavingPlaceholder. Unconditional: the tag may have been
  // set by this very gesture, before the model heard of it, so "is it
  // tagged?" cannot be answered from the model and clearing an empty tag is
  // harmless.
  framework_->SetTabTagById(browser.value, tab_id.value, "");
}

bool PhiSidebarView::SplitTabsNormalized(BrowserId browser,
                                              TabId primary,
                                              TabId secondary) {
  if (!framework_ || !primary.valid() || !secondary.valid() ||
      primary == secondary) {
    return false;
  }
  UnbindStoredTab(browser, primary);
  UnbindStoredTab(browser, secondary);
  // Ids carry across the unbinding (a cleared tag moves nothing); Chromium
  // picks the leading pane, as SplitActiveWith did.
  const char* split = framework_->CreateSplitViewForTabs(
      browser.value, primary.value, secondary.value, "vertical");
  return split && *split;
}

void PhiSidebarView::DrawFavicon(HDC dc,
                                 const RECT& dest,
                                 const std::vector<unsigned char>& bgra,
                                 int width,
                                 int height,
                                 COLORREF background,
                                 float opacity) {
  // Stored favicons arrive from disk and from sync: a buffer that is not
  // exactly width*height*4 bytes is not drawn (the raw-pointer form below
  // trusts its caller's dimensions).
  if (width <= 0 || height <= 0 ||
      bgra.size() != static_cast<size_t>(width) * static_cast<size_t>(height) *
                         4) {
    return;
  }
  DrawFavicon(dc, dest, bgra.data(), width, height, background, opacity);
}

void PhiSidebarView::DrawFavicon(HDC dc,
                                     const RECT& dest,
                                     const unsigned char* bgra,
                                     int width,
                                     int height,
                                     COLORREF background,
                                     float opacity) {
  // A faded icon (a reclaimed tab): the pixels are PREMULTIPLIED, so
  // scaling all four channels is the whole blend — both paths below then
  // composite it over the DC exactly as they would a translucent icon.
  std::vector<unsigned char> faded;
  if (opacity < 1.0f && bgra && width > 0 && height > 0) {
    const size_t bytes =
        static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    // SAFETY: the framework's contract is that `bgra` holds width*height*4
    // bytes (phi_framework_api.h).
    const auto source =
        UNSAFE_BUFFERS(base::span<const unsigned char>(bgra, bytes));
    faded.resize(bytes);
    const int scale = static_cast<int>(std::clamp(opacity, 0.0f, 1.0f) * 256);
    for (size_t i = 0; i < bytes; ++i) {
      faded[i] = static_cast<unsigned char>(source[i] * scale / 256);
    }
    bgra = faded.data();
  }
  // NEVER upscale: a 16px favicon inflated to a larger slot only blurs —
  // 1:1 native pixels centred in the slot are the sharpest a small source
  // can look (the driver rarely holds more than 16px). Downscaling larger
  // art is fine and goes through the high-quality path.
  RECT draw_rc = dest;
  const int dest_w = dest.right - dest.left;
  const int dest_h = dest.bottom - dest.top;
  if (width < dest_w && height < dest_h) {
    draw_rc.left = dest.left + (dest_w - width) / 2;
    draw_rc.top = dest.top + (dest_h - height) / 2;
    draw_rc.right = draw_rc.left + width;
    draw_rc.bottom = draw_rc.top + height;
  }
  // Premultiplied BGRA — exactly what the DirectWrite path draws with
  // high-quality interpolation. GDI's stretch below is the fallback.
  if (PhiDrawBgra(dc, draw_rc, bgra, width, height)) {
    return;
  }

  if (!bgra || width <= 0 || height <= 0) {
    return;
  }
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // Top-down, the order Skia gave us.
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  // Composite over the known background. The pixels are PREMULTIPLIED, so the
  // source contribution is already scaled: dst = src + bg * (1 - a).
  std::vector<unsigned char> composited(static_cast<size_t>(width) *
                                        static_cast<size_t>(height) * 4);
  const int br = GetRValue(background);
  const int bg_g = GetGValue(background);
  const int bb = GetBValue(background);
  // SAFETY: the framework's contract is that `bgra` holds width*height*4 bytes
  // (phi_framework_api.h). A raw pointer and a size is all a C++ ABI across the
  // module boundary can carry, so bounding it here is the first safe moment.
  const auto source = UNSAFE_BUFFERS(base::span<const unsigned char>(
      bgra, static_cast<size_t>(width) * static_cast<size_t>(height) * 4));
  for (size_t i = 0; i + 3 < composited.size(); i += 4) {
    const int sb = source[i], sg = source[i + 1], sr = source[i + 2],
              sa = source[i + 3];
    const int inv = 255 - sa;
    composited[i] = static_cast<unsigned char>(sb + bb * inv / 255);
    composited[i + 1] = static_cast<unsigned char>(sg + bg_g * inv / 255);
    composited[i + 2] = static_cast<unsigned char>(sr + br * inv / 255);
    composited[i + 3] = 255;
  }
  const int prev_mode = ::SetStretchBltMode(dc, HALFTONE);
  ::StretchDIBits(dc, dest.left, dest.top, dest.right - dest.left,
                  dest.bottom - dest.top, 0, 0, width, height,
                  composited.data(), &info, DIB_RGB_COLORS, SRCCOPY);
  ::SetStretchBltMode(dc, prev_mode);
}

// A bookmark folder's icon slot: the catalog symbol it wears, or the
// outline folder glyph for the catalog's "default" (docs/windows-native-ui.md
// §4.2: 16 dip, `dim`, the flap lifted while the folder is open).
void PhiSidebarView::DrawFolderIcon(HDC dc,
                                    const RECT& slot,
                                    const std::string& icon_id,
                                    COLORREF ink,
                                    bool expanded) {
  const wchar_t* glyph = expanded ? fluent::kOpenFolder : fluent::kFolder;
  const std::string resolved = PhiResolveFolderIcon(icon_id);
  const int px = std::min(slot.right - slot.left, slot.bottom - slot.top);
  if (px > 0 && resolved != PhiFolderIconDefaultId()) {
    RECT art = {slot.left + ((slot.right - slot.left) - px) / 2,
                slot.top + ((slot.bottom - slot.top) - px) / 2, 0, 0};
    art.right = art.left + px;
    art.bottom = art.top + px;
    if (const PhiMacIcon* mask = PhiFolderIconMask(resolved)) {
      const std::vector<unsigned char> bgra = PhiTintedIconBgra(*mask, ink, px);
      if (!bgra.empty()) {
        PhiBlendBgra(dc, art, bgra.data(), px, px);
        return;
      }
    }
  }
  if (px > 0 && PhiDrawIconGlyph(dc, slot, glyph, px, ink)) {
    return;
  }
  const HFONT glyph_font = shell_->SidebarGlyphFont();
  HGDIOBJ previous = glyph_font ? ::SelectObject(dc, glyph_font)
                                   : nullptr;
  PhiDrawGlyph(dc, slot, glyph, ink);
  if (previous) {
    ::SelectObject(dc, previous);
  }
}

void PhiSidebarView::PaintDropZone(HDC dc,
                                   const RECT& zone,
                                   bool hot,
                                   const wchar_t* glyph,
                                   int glyph_dip,
                                   const wchar_t* hint,
                                   bool stacked,
                                   const PhiPalette& palette) {
  if (zone.right <= zone.left || zone.bottom <= zone.top) {
    return;
  }
  const int radius = Scaled(kCornerRadius);
  // Resting: the 4% wash under a dashed hairline. Hovered by the drag: the
  // strong wash and the accent ring (the Mac's selectedControlColor at 40%
  // on the emptyView; the folder-highlight look here).
  PhiFillRounded(dc, zone, hot ? palette.hover_strong : palette.hover, radius);
  if (hot) {
    RECT ring = zone;
    ::InflateRect(&ring, -Scaled(1), -Scaled(1));
    PhiStrokeRoundRectAA(dc, ring, radius, palette.accent, Scaled(2));
  } else {
    PhiStrokeRoundRectDashedAA(dc, zone, radius, palette.border, Scaled(1),
                               Scaled(4), Scaled(3));
  }
  const COLORREF ink = hot ? palette.accent : palette.label_tertiary;
  const HFONT glyph_font = shell_->SidebarGlyphFont();
  const int glyph_px = Scaled(glyph_dip);
  RECT icon{};
  RECT text{};
  UINT format = 0;
  if (stacked) {
    // The glyph over the wrapped hint, the pair centred in the zone.
    const int pad = Scaled(12);
    RECT measure = {zone.left + pad, 0, zone.right - pad, 0};
    PhiDrawTextAA(dc, hint, -1, &measure,
                  DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    const int text_h = measure.bottom - measure.top;
    const int gap = Scaled(6);
    const int total = glyph_px + gap + text_h;
    const int top = (zone.top + zone.bottom - total) / 2;
    const int mid_x = (zone.left + zone.right) / 2;
    icon = {mid_x - glyph_px / 2, top, mid_x + glyph_px / 2, top + glyph_px};
    text = {zone.left + pad, top + glyph_px + gap, zone.right - pad,
            top + total};
    format = DT_CENTER | DT_WORDBREAK | DT_NOPREFIX;
  } else {
    // A row: the glyph 6 in, an 8 gap, the hint (the "+ New Tab" row's
    // arrangement).
    const int mid = (zone.top + zone.bottom) / 2;
    icon = {zone.left + Scaled(6), mid - glyph_px / 2,
            zone.left + Scaled(6) + glyph_px, mid + glyph_px / 2};
    text = zone;
    text.left = icon.right + Scaled(8);
    text.right -= Scaled(6);
    format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
             DT_NOPREFIX;
  }
  HGDIOBJ previous = glyph_font ? ::SelectObject(dc, glyph_font) : nullptr;
  PhiDrawGlyphIcon(dc, icon, glyph, glyph_px, ink);
  if (previous) {
    ::SelectObject(dc, previous);
  }
  ::SetTextColor(dc, ink);
  PhiDrawTextAA(dc, hint, -1, &text, format);
}

// static
void PhiSidebarView::DrawSurfaceTile(HDC dc,
                                     const RECT& box,
                                     SidebarSurface kind,
                                     bool hovered,
                                     COLORREF ground,
                                     TabId origin_tab,
                                     int tile_dip) {
  if (box.right <= box.left || kind == SidebarSurface::kContent ||
      !spaces_host_) {
    return;
  }
  const PhiPalette& palette = spaces_host_->PaletteForSlot(slot_.value);
  const int size = Scaled(tile_dip);
  RECT tile = {(box.left + box.right - size) / 2, (box.top + box.bottom - size) / 2,
               0, 0};
  tile.right = tile.left + size;
  tile.bottom = tile.top + size;
  const int radius = Scaled(5);
  // A 0 1 2 shadow, as one darker line under the plate.
  RECT shadow = tile;
  ::OffsetRect(&shadow, 0, 1);
  PhiFillRounded(dc, shadow, PhiBlendOver(RGB(0, 0, 0), ground, 24), radius);
  PhiStrokeRounded(dc, tile, hovered ? palette.hover_strong : palette.card,
                   palette.stroke_strong, radius);
  if (hovered) {
    // The tile's own hover: the close ✕ (12 dip) — the click closes the
    // surface without activating the row.
    PhiDrawIconGlyph(dc, tile, fluent::kCancel, Scaled(12), palette.label);
    return;
  }
  if (kind == SidebarSurface::kReader) {
    PhiDrawReaderGlyph(dc, tile, palette.label, dpi());
    return;
  }
  std::vector<unsigned char> bgra;
  int width = 0;
  int height = 0;
  if (shell_->SidebarSurfaceFavicon(origin_tab, &bgra, &width, &height)) {
    const int icon_px = Scaled(tile_dip - 6);
    RECT icon = {(tile.left + tile.right - icon_px) / 2,
                 (tile.top + tile.bottom - icon_px) / 2, 0, 0};
    icon.right = icon.left + icon_px;
    icon.bottom = icon.top + icon_px;
    DrawFavicon(dc, icon, bgra, width, height, palette.card);
    return;
  }
  PhiDrawIconGlyph(dc, tile, fluent::kOpenInNewWindow, Scaled(12),
                   palette.label);
}

void PhiSidebarView::PaintBand(HDC dc,
                               const PhiSidebarRects& layout,
                               const PhiPalette& palette) {
  // Read ONCE for the whole band: the two chrome fonts the rows borrow, the
  // hover cursor the shell owns across every band, and the drag in flight.
  // Nothing below writes a row — paint reads a finished layout (§3.1
  // principle 7).
  const HFONT glyph_font = shell_->SidebarGlyphFont();
  const HFONT semibold_font = shell_->SidebarSemiboldFont();
  const std::string band_key = PhiSidebarDragSession::kBandShiftKey;
  const HoverKind hover_kind = shell_->SidebarHoverKind();
  const int hover_index = shell_->SidebarHoverIndex();
  const bool drag_active = drag_ && drag_->active();
  const bool drag_capture = drag_ && drag_->capture();
  const PhiDragKind drag_kind =
      drag_ ? drag_->kind() : PhiDragKind::kNone;
  const int drag_index = drag_ ? drag_->Index() : -1;
  // The folder the outline drop lands INSIDE: it wears the container wash
  // + accent ring for as long as that holds (a gap among its children, or
  // its collapsed row), so "the end of this folder" never reads like
  // "after this folder" (§4.8 "Ring").
  const std::string drop_folder = drag_ ? drag_->drop_folder() : std::string();
  // Palette-derived locals the band rows use (same derivations as
  // PaintChrome's own).
  const COLORREF kTintTop = palette.ground_top;
  const COLORREF kRowSelectedBg = palette.selected;
  const COLORREF kLabel = palette.label;
  const COLORREF kLabelDim = palette.label_dim;
  const COLORREF kBorder = palette.border;
  const COLORREF kActivePillBorder =
      palette.dark ? PhiBlendOver(RGB(0xFF, 0xFF, 0xFF), kRowSelectedBg, 51)
                   : kRowSelectedBg;

  // Pinned tabs: PinnedTabItem cells -- resting on the 4% wash, an 18pt
  // favicon centred, radius 8. The ACTIVE pin (its bound live tab is the
  // active tab) gets the selected wash alone — no accent ring, the white
  // cell reads as active by itself (Jixiang, 2026-09-10); in the
  // horizontal strip the cells are the 28pt pinned section and the active one
  // reads like the neighbouring strip tabs (selected wash, no ring).
  const bool pin_sliding = drag_active && drag_kind == PhiDragKind::kPin &&
                           !layout.horizontal_tabs && drag_index >= 0 &&
                           drag_index < static_cast<int>(pinned_.size());
  // The open-tab dots (preference, off by default).
  const bool show_dots = prefs_host_ && prefs_host_->ShowOpenTabIndicators();
  // "Dim unloaded tab icons" (on by default): a reclaimed tab's favicon and
  // open dot at 30%; the dimmed title on tab rows stays regardless.
  const bool dim_reclaimed = !prefs_host_ || prefs_host_->DimUnloadedTabIcons();
  const COLORREF dot_ink = OpenDotInk(palette);
  // An EMPTY stored band while a drag is in flight shows its reserved drop
  // zone (the layout keeps the band's rect only then): the Mac's
  // PinnedTabViewController.emptyView, and a one-row twin for bookmarks.
  const PhiDropBand drag_band = drag_ ? drag_->band() : PhiDropBand::kNone;
  // Everything under the pinned grid rides the band shift: the outline's
  // rows carry it in their own slides, the empty outline's zone, the
  // separator, "+ New Tab" and the tab rows take it here. It opens a gap
  // in the outline, or the row a grid preview grew (a phantom that wraps).
  const int band_shift = drag_active && !layout.horizontal_tabs
                             ? static_cast<int>(RowSlide(band_key).dy)
                             : 0;
  // The stored bands paint inside their capped viewport — scrolled, never
  // over "+ New Tab", the tab list or the bottom bar. Its bottom rides the
  // band shift with everything under the grid.
  const bool clip_stored = !layout.horizontal_tabs &&
                           layout.stored_band.bottom > layout.stored_band.top;
  int stored_clip_save = 0;
  if (clip_stored) {
    stored_clip_save = ::SaveDC(dc);
    ::IntersectClipRect(dc, layout.stored_band.left, layout.stored_band.top,
                        layout.stored_band.right,
                        layout.stored_band.bottom + band_shift);
  }
  if (drag_active && !layout.horizontal_tabs && pinned_.empty() &&
      layout.favorites.bottom > layout.favorites.top) {
    RECT zone = layout.favorites;
    ::OffsetRect(&zone, 0, -stored_scroll_);
    zone.left += Scaled(16);  // The emptyView's leading/trailing 16.
    zone.right -= Scaled(16);
    zone.top += Scaled(kPinnedGap);
    zone.bottom -= Scaled(kPinnedGap);
    PaintDropZone(dc, zone, drag_band == PhiDropBand::kPins, L"\uE734", 28,
                  PhiStr("sidebar.pinnedTabs.emptyHint"), /*stacked=*/true,
                  palette);
  }
  std::vector<size_t> pin_order;
  pin_order.reserve(pinned_.size());
  for (size_t i = 0; i < pinned_.size(); ++i) {
    if (!pin_sliding || static_cast<int>(i) != drag_index) {
      pin_order.push_back(i);
    }
  }
  if (pin_sliding) {
    pin_order.push_back(static_cast<size_t>(drag_index));
  }
  for (const size_t pin_order_index : pin_order) {
    const ShortcutRow& pin = pinned_[pin_order_index];
    const bool pin_dragged =
        pin_sliding && static_cast<int>(pin_order_index) == drag_index;
    if (pin_dragged && !drag_capture) {
      continue;  // It rides the drag image window; its slot is the gap.
    }
    // Siblings glide by their slide offsets (both axes — a grid gap wraps
    // rows); the dragged cell is only ever painted here at home, lifted,
    // for its snapshot.
    RECT cell = pin.frame;
    if (!pin_dragged && drag_active && !layout.horizontal_tabs) {
      // The cell glides AND resizes toward its previewed slot: a phantom
      // entering its row shrinks it, one leaving lets it widen.
      const SlideVec offset = RowSlide(pin.guid);
      ::OffsetRect(&cell, static_cast<int>(offset.dx),
                   static_cast<int>(offset.dy));
      cell.right += static_cast<int>(offset.dw);
      cell.bottom += static_cast<int>(offset.dh);
    }
    const bool pin_hovered =
        hover_kind == HoverKind::kPin &&
        hover_index == static_cast<int>(&pin - pinned_.data());
    const COLORREF pin_bg = pin_dragged ? palette.hover_strong
                            : pin.active
                                ? palette.selected
                                : (pin_hovered ? palette.hover_strong
                                               : palette.hover);
    PhiFillRounded(dc, cell, pin_bg, Scaled(kCornerRadius));
    if (pin_dragged) {
      PhiStrokeRounded(dc, cell, pin_bg, palette.border, Scaled(kCornerRadius));
    }
    // A bound pane whose tab is dormant fades its favicon. The active cell
    // is never dormant (its page is showing).
    // The fade is behind "Dim unloaded tab icons" (on by default).
    const bool dormant_a = pin.unloaded && !pin.active && dim_reclaimed;
    const bool dormant_b = pin.unloaded_b && !pin.active && dim_reclaimed;
    const int cx = (cell.left + cell.right) / 2;
    const int cy = (cell.top + cell.bottom) / 2;
    // The favicon's row: the open dot hangs under it.
    const int icon_size =
        Scaled(!pin.url_b.empty()        ? 16
               : layout.horizontal_tabs ? kStripPinFavicon
                                        : kPinnedFavicon);
    const int icon_bottom = cy + icon_size / 2;
    if (!pin.url_b.empty()) {
      // A merged split cell: two 16pt favicons side by side, the Mac's
      // PinnedSplitItem (16 + 4 + 16 centred in the double-width cell).
      const int size = icon_size;
      const RECT icon_a = {cx - Scaled(2) - size, cy - size / 2,
                           cx - Scaled(2), cy + size / 2};
      const RECT icon_b = {cx + Scaled(2), cy - size / 2,
                           cx + Scaled(2) + size, cy + size / 2};
      const auto pane_icon = [&](const RECT& slot,
                                 const std::vector<unsigned char>& bgra,
                                 int width, int height, bool dormant) {
        if (!bgra.empty()) {
          DrawFavicon(dc, slot, bgra, width, height, pin_bg,
                      dormant ? kReclaimedOpacity : 1.0f);
        } else {
          HGDIOBJ previous_glyph_font =
              glyph_font ? ::SelectObject(dc, glyph_font) : nullptr;
          PhiDrawGlyph(dc, slot, L"", kLabelDim);
          if (previous_glyph_font) {
            ::SelectObject(dc, previous_glyph_font);
          }
        }
      };
      pane_icon(icon_a, pin.favicon, pin.favicon_width, pin.favicon_height,
                dormant_a);
      pane_icon(icon_b, pin.favicon_b, pin.favicon_b_width,
                pin.favicon_b_height, dormant_b);
    } else if (!pin.favicon.empty()) {
      const int size = icon_size;
      const RECT icon = {cx - size / 2, cy - size / 2, cx + size / 2,
                         cy + size / 2};
      DrawFavicon(dc, icon, pin.favicon, pin.favicon_width,
                  pin.favicon_height, pin_bg,
                  dormant_a ? kReclaimedOpacity : 1.0f);
    } else {
      ::SetTextColor(dc, kLabel);
      const std::wstring initial =
          pin.title.empty() ? std::wstring(L"?") : pin.title.substr(0, 1);
      PhiDrawTextAA(dc, initial.c_str(), -1, &cell,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    // The open dot: an inactive cell whose tab is open, centred under the
    // favicon (under the PAIR for a split cell), faded when a pane is
    // dormant — the Mac's PinnedTabItem/PinnedSplitItem openIndicatorHost.
    if (show_dots && pin.open && !pin.active) {
      const int gap = Scaled(layout.horizontal_tabs ? kOpenPillStripGap
                                                    : kOpenPillGap);
      DrawOpenPill(dc, cx, icon_bottom + gap, Scaled(kOpenPillWidth),
                   Scaled(kOpenPillHeight),
                   FadeInk(dot_ink, pin_bg,
                           dormant_a || dormant_b
                               ? kOpenPillOpacity * kReclaimedOpacity
                               : kOpenPillOpacity));
    }
    if (pin.attached_surface != SidebarSurface::kContent &&
        pin.surface_box.right > pin.surface_box.left) {
      RECT badge = pin.surface_box;  // Pinned to the cell's top-right.
      ::OffsetRect(&badge, cell.right - pin.frame.right,
                   cell.top - pin.frame.top);
      DrawSurfaceTile(dc, badge, pin.attached_surface,
                      pin_hovered && !pin_dragged &&
                          shell_->SidebarSurfaceTileHovered(),
                      pin_bg, pin.tab_id, 18);
    }
  }

  // Bookmarks: rows shaped like tab rows (BookmarkCellView) -- primary-label
  // titles, folders marked with the disclosure glyph.
  const bool bookmark_sliding =
      drag_active && drag_kind == PhiDragKind::kBookmark &&
      !layout.horizontal_tabs && drag_index >= 0 &&
      drag_index < static_cast<int>(bookmarks_.size());
  if (drag_active && !layout.horizontal_tabs && OutlineEmpty() &&
      layout.bookmarks.bottom > layout.bookmarks.top) {
    RECT zone = layout.bookmarks;  // The one reserved row, pill-inset.
    ::OffsetRect(&zone, 0, band_shift - stored_scroll_);
    zone.top += Scaled(kRowPad);
    zone.bottom = zone.top + Scaled(kTabRowHeight) - 2 * Scaled(kRowPad);
    PaintDropZone(dc, zone, drag_band == PhiDropBand::kBookmarks, L"\uE8A4",
                  16, PhiStr("sidebar.bookmarks.dropHint"), /*stacked=*/false,
                  palette);
  }
  // The container the drop lands in wears the wash + accent ring over its
  // WHOLE area — header, children and the gap the drop opens among them —
  // so the folder reads as the target, not one row of it, and the space
  // made at its end (the rows below parted a pitch) is visibly ITS space
  // (Jixiang, 2026-09-18: the gap is the placeholder; nothing is drawn
  // for a folder the drag is not near).
  if (drag_active && !drop_folder.empty()) {
    RECT area = {0, 0, 0, 0};
    bool any = false;
    int folder_depth = 0;
    int last_row = -1;  // The subtree's last row (the header when empty).
    bool inside = false;
    for (size_t i = 0; i < bookmarks_.size(); ++i) {
      const ShortcutRow& row = bookmarks_[i];
      if (!inside) {
        if (row.folder && row.guid == drop_folder) {
          inside = true;
          folder_depth = row.depth;
        } else {
          continue;
        }
      } else if (row.depth <= folder_depth) {
        break;
      }
      last_row = static_cast<int>(i);
      RECT r = row.frame;
      const SlideVec offset = RowSlide(row.guid);
      ::OffsetRect(&r, static_cast<int>(offset.dx), static_cast<int>(offset.dy));
      if (!any) {
        area = r;
        any = true;
      } else {
        ::UnionRect(&area, &area, &r);
      }
    }
    // A gap right after the subtree is the folder's end: the rows under
    // it slid, the folder's own did not, so the area grows by the pitch.
    if (any && drag_->insert_slot() == last_row + 1) {
      area.bottom += Scaled(kTabRowHeight + kTabRowGap);
    }
    if (any) {
      area.top += Scaled(kRowPad);
      area.bottom -= Scaled(kRowPad);
      area.left = std::min(
          area.left + OutlineIndentDepth(folder_depth) * Scaled(kBookmarkIndent),
          area.right - Scaled(kFaviconSlot));
      PhiFillRounded(dc, area, palette.hover_strong, Scaled(kCornerRadius));
      PhiStrokeRounded(dc, area, palette.hover_strong, palette.accent,
                       Scaled(kCornerRadius));
    }
  }
  // The Space bookmark root: the chevron and the name, in primary text at 55 %
  // so it stays clear of any Space's ground (the Mac's textMuted).
  if (space_root_.bounds.right > space_root_.bounds.left) {
    RECT pill = space_root_.frame;
    if (drag_active) {
      const SlideVec offset = RowSlide(PhiSidebarDragSession::kSpaceRootSlideKey);
      ::OffsetRect(&pill, static_cast<int>(offset.dx),
                   static_cast<int>(offset.dy));
    }
    pill.top += Scaled(kRowPad);
    pill.bottom -= Scaled(kRowPad);
    if (drag_active && drag_->folder_key() == SpaceRootKey()) {
      PhiFillRounded(dc, pill, palette.hover_strong, Scaled(kCornerRadius));
      PhiStrokeRounded(dc, pill, palette.hover_strong, palette.accent,
                       Scaled(kCornerRadius));
    } else if (space_root_.contains_active) {
      PhiFillRounded(dc, pill, palette.hover_strong, Scaled(kCornerRadius));
    } else if (hover_kind == HoverKind::kSpaceRoot) {
      PhiFillRounded(dc, pill, palette.hover, Scaled(kCornerRadius));
    }
    const COLORREF ink = space_root_.contains_active
                             ? kLabel
                             : FadeInk(kLabel, kTintTop, 0.55f);
    const int mid = (pill.top + pill.bottom) / 2;
    const int chevron_px = Scaled(10);
    const int chevron_left =
        pill.left + Scaled(6) + (Scaled(kFaviconSlot) - chevron_px) / 2;
    const RECT chevron = {chevron_left, mid - chevron_px / 2,
                          chevron_left + chevron_px, mid + chevron_px / 2};
    PhiDrawIconGlyph(dc, chevron,
                     space_root_.expanded ? fluent::kChevronDown
                                          : fluent::kChevronRight,
                     chevron_px, ink);
    RECT text = pill;
    text.left += Scaled(6) + Scaled(kFaviconSlot) + Scaled(8);
    text.right -= Scaled(8);
    ::SetTextColor(dc, ink);
    PhiDrawTextAA(dc, space_root_.title.c_str(), -1, &text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
  }
  std::vector<size_t> bookmark_order;
  bookmark_order.reserve(bookmarks_.size());
  for (size_t i = 0; i < bookmarks_.size(); ++i) {
    if (!bookmark_sliding || static_cast<int>(i) != drag_index) {
      bookmark_order.push_back(i);
    }
  }
  if (bookmark_sliding) {
    bookmark_order.push_back(static_cast<size_t>(drag_index));
  }
  for (const size_t bookmark_order_index : bookmark_order) {
    const ShortcutRow& bookmark = bookmarks_[bookmark_order_index];
    if (bookmark.bounds.right <= bookmark.bounds.left) {
      continue;  // Parked: comfortable keeps its bookmarks in the menu, and
                 // an unguarded row here painted its favicon at (0,0) — in
                 // the window's top-left corner, over the tab strip.
    }
    const bool bookmark_dragged =
        bookmark_sliding &&
        static_cast<int>(bookmark_order_index) == drag_index;
    if (bookmark_dragged && !drag_capture) {
      continue;  // It rides the drag image window; its slot is the gap.
    }
    RECT pill = bookmark.frame;
    if (!bookmark_dragged && drag_active && !layout.horizontal_tabs) {
      const SlideVec offset = RowSlide(bookmark.guid);
      ::OffsetRect(&pill, static_cast<int>(offset.dx),
                   static_cast<int>(offset.dy));
    }
    pill.top += Scaled(kRowPad);
    pill.bottom -= Scaled(kRowPad);
    // Outline depth indents the pill itself, as the Mac outline rows do.
    // Capped (BookmarkIndentDepth), and never past room for the icon slot.
    pill.left = std::min(
        pill.left + OutlineIndentDepth(bookmark.depth) * Scaled(kBookmarkIndent),
        pill.right - Scaled(kFaviconSlot) - Scaled(12));
    const bool bookmark_hovered =
        hover_kind == HoverKind::kBookmark &&
        hover_index == static_cast<int>(&bookmark - bookmarks_.data());
    if (bookmark_dragged) {
      // Lifted, like the dragged tab row.
      PhiFillRounded(dc, pill, palette.hover_strong, Scaled(kCornerRadius));
      PhiStrokeRounded(dc, pill, palette.hover_strong, palette.border,
                    Scaled(kCornerRadius));
    } else if (bookmark.active) {
      // Its bound live tab is the active tab: the row IS the tab, so it
      // reads exactly like the active tab row.
      PhiFillRounded(dc, pill, kRowSelectedBg, Scaled(kCornerRadius));
      if (palette.dark) {
        PhiStrokeRounded(dc, pill, kRowSelectedBg, kActivePillBorder,
                      Scaled(kCornerRadius));
      }
    } else if (bookmark_hovered) {
      PhiFillRounded(dc, pill, palette.hover, Scaled(kCornerRadius));
    }
    RECT text = pill;
    const int mid = (pill.top + pill.bottom) / 2;
    // A 14pt favicon centred in its 16pt slot 6pt in; a folder's 16 dip
    // glyph fills the slot — both leave the title at 30pt.
    RECT icon = {pill.left + Scaled(6) +
                     (Scaled(kFaviconSlot) - Scaled(kFaviconSize)) / 2,
                 mid - Scaled(kFaviconSize) / 2, 0, 0};
    icon.right = icon.left + Scaled(kFaviconSize);
    icon.bottom = icon.top + Scaled(kFaviconSize);
    if (bookmark.folder) {
      icon = {pill.left + Scaled(6), mid - Scaled(kFolderIconSize) / 2,
              pill.left + Scaled(6) + Scaled(kFolderIconSize),
              mid - Scaled(kFolderIconSize) / 2 + Scaled(kFolderIconSize)};
    }
    // A bound pane whose tab is dormant fades its favicon and the title,
    // like a dormant tab row; the active row is never dormant.
    const bool dormant_a = bookmark.unloaded && !bookmark.active;
    const bool dormant_b = bookmark.unloaded_b && !bookmark.active;
    // The favicon and dot fade is behind "Dim unloaded tab icons"; the
    // dimmed title stays either way, as on Mac.
    const bool dim_a = dormant_a && dim_reclaimed;
    const bool dim_b = dormant_b && dim_reclaimed;
    const COLORREF bookmark_bg = bookmark.active ? kRowSelectedBg : kTintTop;
    if (bookmark.folder) {
      // The Mac paints a Lottie folder carrying the chosen symbol on its
      // face; with no Lottie runtime here the symbol itself IS the row icon,
      // drawn from the same imageset artwork the Mac's picker uses. The
      // catalog's "default" is the outline folder glyph, open or closed
      // (§4.2).
      DrawFolderIcon(dc, icon, bookmark.icon, kLabelDim, bookmark.expanded);
    } else if (!bookmark.favicon.empty()) {
      DrawFavicon(dc, icon, bookmark.favicon, bookmark.favicon_width,
                  bookmark.favicon_height, bookmark_bg,
                  dim_a ? kReclaimedOpacity : 1.0f);
    } else {
      HGDIOBJ previous_glyph_font =
          glyph_font ? ::SelectObject(dc, glyph_font) : nullptr;
      PhiDrawGlyph(dc, icon, L"\uE774", kLabelDim);
      if (previous_glyph_font) {
        ::SelectObject(dc, previous_glyph_font);
      }
    }
    text.left = pill.left + Scaled(6) + Scaled(kFaviconSlot) + Scaled(8);
    const bool split_row = !bookmark.url_b.empty();
    if (split_row) {
      // A split bookmark shows BOTH panes' favicons (the Mac's
      // BookmarkCellView secondary favicon, 24pt after the primary).
      RECT icon_b = icon;
      ::OffsetRect(&icon_b, Scaled(24), 0);
      if (!bookmark.favicon_b.empty()) {
        DrawFavicon(dc, icon_b, bookmark.favicon_b,
                    bookmark.favicon_b_width, bookmark.favicon_b_height,
                    bookmark_bg, dim_b ? kReclaimedOpacity : 1.0f);
      } else {
        HGDIOBJ previous_glyph_font =
            glyph_font ? ::SelectObject(dc, glyph_font) : nullptr;
        PhiDrawGlyph(dc, icon_b, L"\uE774", kLabelDim);
        if (previous_glyph_font) {
          ::SelectObject(dc, previous_glyph_font);
        }
      }
      text.left += Scaled(24);
    }
    // The open dot leads the favicon (the Mac's BookmarkCellView leading
    // overlay), decorative so the title never moves; faded when a bound
    // pane is dormant.
    if (show_dots && !bookmark.folder && bookmark.open && !bookmark.active) {
      // Centred under the favicon, as on the pinned cells (§5.8).
      DrawOpenPill(dc, (icon.left + icon.right) / 2,
                   icon.bottom + Scaled(kOpenPillGap), Scaled(kOpenPillWidth),
                   Scaled(kOpenPillHeight),
                   FadeInk(dot_ink, bookmark_bg,
                           dim_a || dim_b ? kOpenPillOpacity * kReclaimedOpacity
                                          : kOpenPillOpacity));
    }
    ::SetTextColor(dc, dormant_a || dormant_b ? kLabelDim : kLabel);
    if (bookmark.folder) {
      // The disclosure is a trailing 12 dip chevron in `tertiary`, right
      // when closed and down when open (§4.2); the title stops short of it.
      const int chevron_px = Scaled(12);
      const RECT chevron = {pill.right - Scaled(10) - chevron_px,
                            mid - chevron_px / 2, pill.right - Scaled(10),
                            mid + chevron_px / 2};
      PhiDrawIconGlyph(dc, chevron,
                       bookmark.expanded ? fluent::kChevronDown
                                         : fluent::kChevronRight,
                       chevron_px, palette.label_tertiary);
      text.right = chevron.left - Scaled(4);
    }
    // Folder titles carry the row's regular weight, as the design's rows do.
    std::wstring label = bookmark.title;
    if (split_row) {
      // "primary \u2022 secondary" \u2014 the secondary title, or its www.-stripped
      // host when the title duplicates the primary's (the Mac's fallback).
      std::wstring second = bookmark.title_b;
      if (second.empty()) {
        std::string host = bookmark.url_b;
        const size_t scheme = host.find("://");
        if (scheme != std::string::npos) {
          host = host.substr(scheme + 3);
        }
        const size_t slash = host.find('/');
        if (slash != std::string::npos) {
          host = host.substr(0, slash);
        }
        if (host.rfind("www.", 0) == 0) {
          host = host.substr(4);
        }
        second = base::UTF8ToWide(host);
      }
      if (!second.empty()) {
        label += L" \u2022 " + second;
      }
    }
    RECT bookmark_badge = bookmark.surface_box;
    const bool bookmark_attached =
        bookmark.attached_surface != SidebarSurface::kContent &&
        bookmark_badge.right > bookmark_badge.left;
    if (bookmark_attached) {
      const SlideVec offset = !bookmark_dragged && drag_active
                                  ? RowSlide(bookmark.guid)
                                  : SlideVec{};
      ::OffsetRect(&bookmark_badge, static_cast<int>(offset.dx),
                   static_cast<int>(offset.dy));
      text.right = bookmark_badge.left - Scaled(4);
    }
    const bool bookmark_close = bookmark_hovered && !bookmark_dragged &&
                                !drag_active &&
                                bookmark.close_box.right > bookmark.close_box.left;
    if (bookmark_close) {
      text.right = bookmark.close_box.left - Scaled(4);
    }
    PhiDrawTextAA(dc, label.c_str(), -1, &text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
    if (bookmark_attached) {
      DrawSurfaceTile(dc, bookmark_badge, bookmark.attached_surface,
                      bookmark_hovered && !bookmark_dragged &&
                          shell_->SidebarSurfaceTileHovered(),
                      bookmark.active ? kRowSelectedBg : kTintTop,
                      bookmark.tab_id, 20);
    }
    if (bookmark_close) {
      PhiDrawIconGlyph(dc, bookmark.close_box, fluent::kRemove, Scaled(12),
                       palette.label);
    }
  }

  if (clip_stored) {
    // Its overlay scrollbar, while it scrolls or the pointer is on it.
    if (!drag_capture) {
      PaintScrollbar(dc, layout.stored_band, stored_scroll_,
                     stored_scroll_max_, palette,
                     ScrollbarOpacity(hover_kind == HoverKind::kPin ||
                                      hover_kind == HoverKind::kBookmark));
    }
    ::RestoreDC(dc, stored_clip_save);
  }

  // "New Tab": the row heading the tab list -- a 16pt "+" 6pt in, an 8pt
  // gap, the title in textTertiary (NewTabButtonCellView). Hover raises the
  // Mac cell's radius-8 wash on the 2pt-inset pill.
  // Everything from here down to the tab rows rides the band shift (read
  // above).
  // The separator row: a 1pt `separator` hairline centred in its 16pt row,
  // spanning the rows' width (SeparatorCellView).
  if (layout.bookmark_separator.right > layout.bookmark_separator.left) {
    RECT sep = layout.bookmark_separator;
    ::OffsetRect(&sep, 0, band_shift);
    const int line_y = (sep.top + sep.bottom) / 2;
    const RECT line = {sep.left, line_y, sep.right, line_y + Scaled(1)};
    HBRUSH sep_brush = ::CreateSolidBrush(palette.separator);
    ::FillRect(dc, &line, sep_brush);
    ::DeleteObject(sep_brush);
  }
  if (layout.new_tab_row.right > layout.new_tab_row.left) {
    RECT row = layout.new_tab_row;
    ::OffsetRect(&row, 0, band_shift);
    if (hover_kind == HoverKind::kNewTabRow) {
      RECT pill = row;
      pill.top += Scaled(kRowPad);
      pill.bottom -= Scaled(kRowPad);
      PhiFillRounded(dc, pill, palette.hover, Scaled(kCornerRadius));
    }
    const int mid = (row.top + row.bottom) / 2;
    const RECT icon = {row.left + Scaled(6), mid - Scaled(8),
                       row.left + Scaled(6) + Scaled(16), mid + Scaled(8)};
    PhiDrawGlyph(dc, icon, L"+", palette.label_tertiary);
    RECT text = row;
    text.left = icon.right + Scaled(8);
    ::SetTextColor(dc, palette.label_tertiary);
    PhiDrawTextAA(dc, L"New Tab", -1, &text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }

  // The scrolling tab-list band (groups + tab rows) clips to its viewport:
  // a row straddling the bottom edge must not bleed into the bottom bar.
  const bool clip_rows = !layout.horizontal_tabs;
  int rows_clip_save = 0;
  if (clip_rows) {
    rows_clip_save = ::SaveDC(dc);
    // The band shift moves the viewport with the rows: an outline row
    // leaving for the list pulls them UP, and the first must not be cut
    // off at the band's resting top.
    ::IntersectClipRect(dc, 0, layout.tabs_top + std::min(0, band_shift),
                        layout.sidebar.right, layout.tabs_limit);
  }

  // Tab groups: TabGroupCellView — the whole group cell rests on an 18%
  // tint of the group color; the 32pt header row carries the chevron, the
  // color dot (white ring) and the title. Clicking the header collapses.
  // The headers are drawn through a shifted viewport rather than by
  // offsetting each of their rects (the band shift, see above).
  POINT viewport_before = {};
  ::OffsetViewportOrgEx(dc, 0, band_shift, &viewport_before);
  for (const GroupHeader& header : group_headers_) {
    if (header.chip) {
      continue;  // The strip's chips paint after the tabs (PaintStripGroups).
    }
    // During a drag the header rides its first member and the card's
    // bottom its last (the session's group slide), so a gap opening inside
    // the group grows the card around the gliding rows — for a tab reorder
    // and for a pinned cell or a bookmark entering the list alike.
    GroupHeader gh = header;
    if (drag_active && !layout.horizontal_tabs) {
      const SlideVec slide = RowSlide(SidebarKey::Group(gh.id).ToString());
      const int dy = static_cast<int>(slide.dy);
      const int dh = static_cast<int>(slide.dh);
      ::OffsetRect(&gh.bounds, 0, dy);
      if (gh.container.right > gh.container.left) {
        gh.container.top += dy;
        gh.container.bottom += dy + dh;
      }
    }
    if (gh.container.right > gh.container.left) {
      // The card: the hover wash under the 1pt `border` hairline.
      PhiStrokeRounded(dc, gh.container, palette.hover, palette.border,
                    Scaled(kCornerRadius));
    }
    if (gh.bounds.right <= gh.bounds.left) {
      continue;  // Header scrolled out of the viewport.
    }
    if (hover_kind == HoverKind::kGroupHeader &&
        hover_index == static_cast<int>(&header - group_headers_.data())) {
      PhiFillRounded(dc, gh.bounds, palette.hover_strong,
                     Scaled(kCornerRadius));
    }
    // TabGroupHeaderView: 4pt leading, the 10pt chevron in a 20pt frame, the
    // 10pt dot, a 6pt gap, the semibold title.
    const int mid = (gh.bounds.top + gh.bounds.bottom) / 2;
    RECT chevron = {gh.bounds.left + Scaled(4), mid - Scaled(10),
                    gh.bounds.left + Scaled(4) + Scaled(20), mid + Scaled(10)};
    HGDIOBJ previous_glyph_font =
        glyph_font ? ::SelectObject(dc, glyph_font) : nullptr;
    PhiDrawGlyph(dc, chevron, gh.collapsed ? L"\uE76C" : L"\uE70D", kLabelDim);
    if (previous_glyph_font) {
      ::SelectObject(dc, previous_glyph_font);
    }
    // The dot: colored centre in a 1pt white ring, 10pt over all
    // (TabGroupHeaderColorIndicator).
    const int cx = chevron.right + Scaled(5);
    PhiFillEllipseAA(dc,
                     {cx - Scaled(5), mid - Scaled(5), cx + Scaled(5),
                      mid + Scaled(5)},
                     RGB(0xFF, 0xFF, 0xFF));
    PhiFillEllipseAA(dc,
                     {cx - Scaled(4), mid - Scaled(4), cx + Scaled(4),
                      mid + Scaled(4)},
                     gh.color);
    ::SetTextColor(dc, kLabel);
    RECT title_rc = {cx + Scaled(5) + Scaled(6), gh.bounds.top,
                     gh.bounds.right - Scaled(6), gh.bounds.bottom};
    HGDIOBJ previous_title_font =
        semibold_font ? ::SelectObject(dc, semibold_font) : nullptr;
    PhiDrawTextAA(dc, gh.title.c_str(), -1, &title_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
    if (previous_title_font) {
      ::SelectObject(dc, previous_title_font);
    }
  }
  ::SetViewportOrgEx(dc, viewport_before.x, viewport_before.y, nullptr);

  // Tab rows: a 32pt pill inside the 36pt row (2pt vertical padding), radius
  // 8. The title is textPrimary whether active or not (SideTabView); only the
  // pill marks the active tab. The close box rides the ACTIVE row -- the Mac
  // shows it on hover, which GDI has no notion of yet.
  //
  // During a drag-reorder (Chrome's vertical-tab model) the DRAGGED row
  // floats with the cursor and paints LAST (on top); its siblings paint at
  // their slide offsets, gliding around the insertion gap.
  const bool sliding = drag_active && drag_kind == PhiDragKind::kTab &&
                       drag_index >= 0 &&
                       drag_index < static_cast<int>(tabs_.size());
  // The strip's landing tab glides over its neighbours into its slot, so
  // it paints after them too.
  int landing_index = -1;
  if (layout.horizontal_tabs && !landing_key_.empty()) {
    for (size_t i = 0; i < tabs_.size(); ++i) {
      if (tabs_[i].key == landing_key_) {
        landing_index = static_cast<int>(i);
      }
    }
  }
  std::vector<size_t> draw_order;
  draw_order.reserve(tabs_.size());
  for (size_t i = 0; i < tabs_.size(); ++i) {
    if ((!sliding || static_cast<int>(i) != drag_index) &&
        static_cast<int>(i) != landing_index) {
      draw_order.push_back(i);
    }
  }
  if (landing_index >= 0) {
    draw_order.push_back(static_cast<size_t>(landing_index));
  }
  if (sliding) {
    draw_order.push_back(static_cast<size_t>(drag_index));
  }
  bool rows_clip_active = clip_rows;
  // A hovered strip chip hides the hairlines on both its sides (the Mac's
  // hoveredChipToken rule): the tab standing before it drops its divider.
  LONG hovered_chip_left = -1;
  if (hover_kind == HoverKind::kGroupHeader && hover_index >= 0 &&
      hover_index < static_cast<int>(group_headers_.size()) &&
      group_headers_[static_cast<size_t>(hover_index)].chip) {
    hovered_chip_left =
        group_headers_[static_cast<size_t>(hover_index)].bounds.left;
  }
  for (const size_t order_index : draw_order) {
    const TabRow& row = tabs_[order_index];
    if (row.bounds.right <= row.bounds.left) {
      continue;  // Scrolled out of the viewport.
    }
    const bool is_dragged =
        sliding && static_cast<int>(order_index) == drag_index;
    if (is_dragged && !drag_capture) {
      continue;  // It rides the drag image window; its slot is the gap.
    }
    const bool is_landing = static_cast<int>(order_index) == landing_index;
    // Siblings glide by their slide offsets; the dragged row is only ever
    // painted here at home, lifted, for its snapshot.
    int dx = 0;
    int dy = band_shift;
    // The rows glide for ANY drag that opens a gap in the list — a tab
    // reorder, and a pinned cell or a bookmark entering it (the Mac's
    // movePinnedTabOut / moveBookmarkOut(toNormalTabs:)). The strip's
    // glide sideways.
    if (!is_dragged && drag_active) {
      if (layout.horizontal_tabs) {
        dx = static_cast<int>(TabSlide(row.id));
      } else {
        dy += static_cast<int>(TabSlide(row.id));
      }
    }
    if (is_landing) {
      dx += static_cast<int>(std::lround(landing_dx_));
    }
    RECT pill = row.bounds;
    ::OffsetRect(&pill, dx, dy);
    RECT close_box = row.close_box;
    ::OffsetRect(&close_box, dx, dy);
    if (!layout.horizontal_tabs) {
      pill.top += Scaled(kRowPad);
      pill.bottom -= Scaled(kRowPad);
    }
    const bool row_hovered =
        hover_kind == HoverKind::kTab &&
        hover_index == static_cast<int>(&row - tabs_.data());
    // The strip's lifted tab — the snapshot the drag image carries, and
    // the landing glide — wears the hover wash (the Mac proxy's
    // setDragHighlighted) and no divider: a clean tile over its neighbours.
    const bool strip_lifted =
        layout.horizontal_tabs && (is_dragged || is_landing);
    // The strip's active tab wears the address row's page colour when the row
    // follows the page (SetActiveTabPagePalette).
    const PhiPalette& row_palette =
        layout.horizontal_tabs && row.active && active_tab_page_palette_
            ? *active_tab_page_palette_
            : palette;
    if (layout.horizontal_tabs) {
      // Chrome's strip. The active tab is the toolbar's own ground carried
      // up into the band, rounded on top and flush at the bottom so the two
      // read as one surface; a resting tab shows only the window tint it
      // sits on, divided from its neighbour by a hairline. Nothing here
      // rounds the bottom — that is the whole point of the merge.
      if (row.active) {
        PhiFillRoundedTop(dc, pill, row_palette.window_bg,
                          Scaled(kCornerRadius));
      } else {
        if (row_hovered || strip_lifted) {
          PhiFillRoundedTop(dc, pill, palette.hover, Scaled(kCornerRadius));
        }
        // The divider rides the tab's trailing edge, inset vertically so it
        // reads as a separator and not as a full-height rule.
        const bool chip_after_hovered =
            hovered_chip_left >= pill.right &&
            hovered_chip_left <= pill.right + Scaled(kChipLeadGap);
        if (!chip_after_hovered && !strip_lifted) {
          RECT divider = {pill.right - 1, pill.top + Scaled(6), pill.right,
                          pill.bottom - Scaled(6)};
          HBRUSH divider_brush = ::CreateSolidBrush(palette.separator);
          ::FillRect(dc, &divider, divider_brush);
          ::DeleteObject(divider_brush);
        }
      }
    } else if (is_dragged) {
      // Lifted: the emphasized wash plus a hairline, Chrome's elevated
      // dragged view.
      PhiFillRounded(dc, pill, palette.hover_strong, Scaled(kCornerRadius));
      PhiStrokeRounded(dc, pill, palette.hover_strong, palette.border,
                    Scaled(kCornerRadius));
    } else if (row.active) {
      // Dark: the Mac's white-20% active border. Light: a hairline stands
      // in for the Mac's drop shadow — the same trade the Space chip makes,
      // or the white pill disappears on a white ground.
      PhiStrokeRounded(dc, pill, kRowSelectedBg,
                    palette.dark ? kActivePillBorder : kBorder,
                    Scaled(kCornerRadius));
    } else if (row_hovered) {
      PhiFillRounded(dc, pill, palette.hover, Scaled(kCornerRadius));
    }
    // A 14pt icon centred in the 16pt slot, 6pt from the pill's edge. A
    // merged split row draws the same shape once per pane around a hairline
    // divider (the Mac's single merged row with two panes).
    const int mid = (pill.top + pill.bottom) / 2;
    // Inside a group card the row rests on the card's wash, not the tint.
    const COLORREF ground = row.group_id.empty() ? kTintTop : palette.hover;
    const COLORREF row_bg =
        layout.horizontal_tabs
            ? (row.active ? row_palette.window_bg
                          : strip_lifted ? palette.hover : ground)
            : (is_dragged ? palette.hover_strong
                          : row.active ? kRowSelectedBg : ground);
    const auto draw_pane = [&](int left, int right, const TabRow& r,
                               bool pane_b) {
      RECT slot = {left + Scaled(6), mid - Scaled(kFaviconSlot) / 2,
                   left + Scaled(6) + Scaled(kFaviconSlot),
                   mid + Scaled(kFaviconSlot) / 2};
      RECT icon = {
          slot.left + (Scaled(kFaviconSlot) - Scaled(kFaviconSize)) / 2,
          mid - Scaled(kFaviconSize) / 2, 0, 0};
      icon.right = icon.left + Scaled(kFaviconSize);
      icon.bottom = icon.top + Scaled(kFaviconSize);
      const auto& favicon = pane_b ? r.b_favicon : r.favicon;
      const std::vector<unsigned char>& logo = PhiLogoBgra256();
      // A DORMANT tab (restored but never loaded, or discarded) draws its
      // favicon at the Mac's 30% and reads dimmer than a live one. The
      // active row is never dormant, so this never fights the selected pill.
      const bool dormant = r.unloaded && !r.active;
      const bool dim = dormant && dim_reclaimed;
      if (!favicon.empty()) {
        DrawFavicon(dc, icon, favicon,
                    pane_b ? r.b_favicon_width : r.favicon_width,
                    pane_b ? r.b_favicon_height : r.favicon_height, row_bg,
                    dim ? kReclaimedOpacity : 1.0f);
      } else if (r.internal_page && !logo.empty() &&
                 PhiDrawBgra(dc, icon, logo.data(), 256, 256)) {
        // Internal pages (chrome://newtab and friends) carry the Phi mark,
        // as the Mac shows its own logo there — not the generic globe.
      } else {
        // The Mac's no-favicon placeholder: the globe (FaviconProvider).
        HGDIOBJ previous_glyph_font =
            glyph_font ? ::SelectObject(dc, glyph_font) : nullptr;
        PhiDrawGlyph(dc, icon, L"\uE774", row_palette.label_dim);
        if (previous_glyph_font) {
          ::SelectObject(dc, previous_glyph_font);
        }
      }
      ::SetTextColor(dc, dormant ? row_palette.label_dim : row_palette.label);
      RECT title_rc = {slot.right + Scaled(8), pill.top, right - Scaled(4),
                       pill.bottom};
      PhiDrawTextAA(dc, (pane_b ? r.b_title : r.title).c_str(), -1, &title_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
    };
    // The close box shows on HOVER only (SideTabView) \u2014 the active row
    // included; the title reclaims its slot the rest of the time. A row with
    // a surface attached has no close box: the indicator holds the slot.
    const bool attached = row.attached_surface != SidebarSurface::kContent;
    const bool show_close = row_hovered && !is_dragged && !attached;
    RECT surface_box = row.surface_box;
    ::OffsetRect(&surface_box, dx, dy);
    const int trailing = show_close ? close_box.left
                         : attached ? surface_box.left
                                    : pill.right;
    if (row.b_index >= 0) {
      const int div_x = (pill.left + pill.right) / 2;
      draw_pane(pill.left, div_x, row, /*pane_b=*/false);
      draw_pane(div_x, trailing, row, /*pane_b=*/true);
      RECT divider = {div_x, pill.top + Scaled(6), div_x + 1,
                      pill.bottom - Scaled(6)};
      HBRUSH sep = ::CreateSolidBrush(palette.separator);
      ::FillRect(dc, &divider, sep);
      ::DeleteObject(sep);
    } else {
      draw_pane(pill.left, trailing, row, /*pane_b=*/false);
    }
    if (show_close) {
      PhiDrawGlyph(dc, close_box, L"\u00D7", row_palette.label_dim);
    }
    if (attached) {
      DrawSurfaceTile(dc, surface_box, row.attached_surface,
                      row_hovered && !is_dragged &&
                          shell_->SidebarSurfaceTileHovered(),
                      row_bg, row.id, 20);
    }
    // The agent operating mask: a badge on rows an agent is holding. Accent
    // while the agent drives; dimmed once the user has taken the tab back
    // (the row keeps the badge so "Hand Back" stays discoverable).
    if (agent_host_ && row.id.valid() &&
        agent_host_->IsTabAgentDriven(row.id.value)) {
      const bool reclaimed = agent_host_->IsTabReclaimed(row.id.value);
      const int dot = Scaled(8);
      const int cx = trailing - Scaled(10);
      const int cy = (pill.top + pill.bottom) / 2;
      const COLORREF ink =
          reclaimed ? palette.label_tertiary : palette.accent;
      PhiFillEllipseAA(
          dc, {cx - dot / 2, cy - dot / 2, cx + dot / 2, cy + dot / 2}, ink);
    }
  }

  if (rows_clip_active) {
    ::RestoreDC(dc, rows_clip_save);
  }
  if (!layout.horizontal_tabs && !drag_capture) {
    // The tab list's overlay scrollbar, while it scrolls or is hovered.
    const RECT track = {layout.sidebar.left, layout.tabs_top,
                        layout.sidebar.right, layout.tabs_limit};
    PaintScrollbar(dc, track, scroll_offset_, tabs_scroll_max_, palette,
                   ScrollbarOpacity(hover_kind == HoverKind::kTab ||
                                    hover_kind == HoverKind::kGroupHeader));
  }
  if (kb_focus_ && !drag_capture) {
    // The keyboard-only focus ring (F6): 2 dip in the accent round the
    // row's pill, never shown for the mouse.
    RECT ring = KeyRect(kb_key_);
    const bool pin_cell =
        std::any_of(pinned_.begin(), pinned_.end(),
                    [&](const ShortcutRow& pin) { return pin.key == kb_key_; });
    if (ring.right > ring.left) {
      if (!layout.horizontal_tabs && !pin_cell) {
        ring.top += Scaled(kRowPad);
        ring.bottom -= Scaled(kRowPad);
      }
      PhiStrokeRoundRectAA(dc, ring, Scaled(kCornerRadius), palette.accent,
                           Scaled(2));
    }
  }
  if (layout.horizontal_tabs) {
    PaintStripGroups(dc, layout, palette);
    if (strip_overflow_button_.right > strip_overflow_button_.left) {
      // The overflow chevron: a toolbar button on the strip's line, its
      // hover the shell's (nav-hover ordinal kStripOverflowNavOrdinal).
      const bool hot = hover_kind == HoverKind::kNav &&
                       hover_index == kStripOverflowNavOrdinal;
      PhiPaintIconButton(dc, strip_overflow_button_, fluent::kChevronDown,
                         hot ? PhiButtonState::kHover : PhiButtonState::kRest,
                         palette, palette.label, Scaled(fluent::kIconToolbar),
                         Scaled(fluent::kButtonRadius));
    }
  }
}

void PhiSidebarView::PaintStripGroups(HDC dc,
                                      const PhiSidebarRects& layout,
                                      const PhiPalette& palette) {
  const HFONT glyph_font = shell_->SidebarGlyphFont();
  const HFONT semibold_font = shell_->SidebarSemiboldFont();
  const HoverKind hover_kind = shell_->SidebarHoverKind();
  const int hover_index = shell_->SidebarHoverIndex();
  const int hovered_tab = hover_kind == HoverKind::kTab ? hover_index : -1;
  const std::string overview_group = PhiGroupOverview::OpenGroupId();
  const int radius = Scaled(kCornerRadius);
  const int strip_bottom = layout.tabs_top + Scaled(kHTabHeight);
  // A tab drag in flight: the chips glide with their slots and each run's
  // underline follows its gliding members (the session's group slide); the
  // dragged tab is out of the strip, so it is nobody's neighbour.
  const bool drag_active = drag_ && drag_->active();
  const int dragged_tab =
      drag_active && drag_->kind() == PhiDragKind::kTab ? drag_->Index() : -1;
  int landing_index = -1;
  if (!landing_key_.empty()) {
    for (size_t t = 0; t < tabs_.size(); ++t) {
      if (tabs_[t].key == landing_key_) {
        landing_index = static_cast<int>(t);
      }
    }
  }
  // A tab's leading edge where it is painted right now.
  const auto tab_dx = [&](size_t t) {
    int dx = drag_active ? static_cast<int>(TabSlide(tabs_[t].id)) : 0;
    if (static_cast<int>(t) == landing_index) {
      dx += static_cast<int>(std::lround(landing_dx_));
    }
    return dx;
  };
  const auto chip_dx = [&](const GroupHeader& header) {
    return drag_active
               ? static_cast<int>(
                     RowSlide(SidebarKey::Group(header.id).ToString()).dx)
               : 0;
  };
  ::SetBkMode(dc, TRANSPARENT);
  // The underline (updateGroupBoundaryLayers): an expanded group's colour
  // runs along the strip's bottom edge from the chip — or, with the chip
  // scrolled out of the strip's window, from its first member in view — to
  // its last member and, where the group holds the active tab, climbs over
  // that tab's outline — one path, so the line and the outline meet
  // without a seam. The Windows tab has no inverse corners: the outline is
  // its top-rounded shape, cut off at the strip's bottom edge so its sides
  // run straight into the line.
  const auto paint_underline = [&](const GroupHeader& gh) {
    const int line_y = strip_bottom - 1;
    const int inset = Scaled(kChipUnderlineInset);
    const int left = static_cast<int>(gh.container.left) + inset;
    const int right = static_cast<int>(gh.container.right) - inset;
    HBRUSH ink = ::CreateSolidBrush(gh.color);
    // The active member's outline, where it is painted — not while it is
    // the tab being dragged (the line runs straight under the gap).
    RECT active_bounds = {};
    bool active = false;
    if (gh.contains_active) {
      for (size_t t = 0; t < tabs_.size(); ++t) {
        const TabRow& row = tabs_[t];
        if (row.active && row.group_id == gh.id &&
            row.bounds.right > row.bounds.left &&
            static_cast<int>(t) != dragged_tab) {
          active = true;
          active_bounds = row.bounds;
          ::OffsetRect(&active_bounds, tab_dx(t), 0);
          break;
        }
      }
    }
    const auto line = [&](int from, int to) {
      if (to > from) {
        RECT r = {from, line_y, to, line_y + 1};
        ::FillRect(dc, &r, ink);
      }
    };
    if (active) {
      line(left, static_cast<int>(active_bounds.left));
      line(static_cast<int>(active_bounds.right), right);
      const int save = ::SaveDC(dc);
      ::IntersectClipRect(dc, active_bounds.left - 1, active_bounds.top - 1,
                          active_bounds.right + 1, line_y + 1);
      RECT outline = active_bounds;
      outline.bottom += radius;
      PhiStrokeRoundRectAA(dc, outline, radius, gh.color, 1);
      ::RestoreDC(dc, save);
    } else {
      line(left, right);
    }
    ::DeleteObject(ink);
  };
  for (size_t i = 0; i < group_headers_.size(); ++i) {
    const GroupHeader& header = group_headers_[i];
    if (!header.chip) {
      continue;
    }
    if (header.bounds.right <= header.bounds.left) {
      // Scrolled out of the strip's window: the members still in view
      // carry the group's underline.
      if (!header.collapsed &&
          header.container.right > header.container.left) {
        paint_underline(header);
      }
      continue;
    }
    GroupHeader gh = header;
    if (drag_active) {
      const SlideVec slide =
          RowSlide(SidebarKey::Group(gh.id).ToString());
      const int dx = static_cast<int>(slide.dx);
      const int dw = static_cast<int>(slide.dw);
      for (RECT* part : {&gh.bounds, &gh.dot, &gh.label, &gh.count,
                         &gh.mosaic, &gh.collapse_box}) {
        ::OffsetRect(part, dx, 0);
      }
      if (gh.container.right > gh.container.left) {
        gh.container.left += dx;
        gh.container.right += dx + dw;
      }
    }
    const bool hovered = hover_kind == HoverKind::kGroupHeader &&
                         hover_index == static_cast<int>(i);
    // The chip rests on the band's ground; hovered, on the same 4% wash the
    // tabs take (ThemedColor.hover), at the tabs' radius.
    const COLORREF ground = hovered ? palette.hover : palette.ground_top;
    if (hovered) {
      PhiFillRounded(dc, gh.bounds, palette.hover, radius);
    }
    if (overview_group == gh.id) {
      // The overview is open on this group: a 1pt ring in its colour
      // (setOverviewSelected).
      PhiStrokeRoundRectAA(dc, gh.bounds, radius, gh.color, 1);
    }
    PhiFillEllipseAA(dc, gh.dot, gh.color);
    {
      // The title: semibold, the row label colour (labelColor), truncated
      // with an ellipsis inside the chip's cap.
      HGDIOBJ previous =
          semibold_font ? ::SelectObject(dc, semibold_font) : nullptr;
      ::SetTextColor(dc, palette.label);
      RECT label = gh.label;
      PhiDrawTextAA(dc, gh.title.c_str(), -1, &label,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
      if (previous) {
        ::SelectObject(dc, previous);
      }
    }
    {
      // The chevron points left while the members follow the chip, right
      // once they are folded under it (chevron.left / chevron.right).
      HGDIOBJ previous = glyph_font ? ::SelectObject(dc, glyph_font) : nullptr;
      PhiDrawGlyph(dc, gh.collapse_box, gh.collapsed ? L"\uE76C" : L"\uE76B",
                   palette.label_dim);
      if (previous) {
        ::SelectObject(dc, previous);
      }
    }
    if (gh.collapsed) {
      // The 2x2 favicon mosaic (TabGroupChipMosaicView): the first members'
      // favicons, empty plates at 4% ink, and past four members "+N" — an
      // ellipsis from ten on, the 10pt cell being too small for the number.
      const std::vector<SidebarRect> cells = StripMosaicCells(
          {static_cast<int>(gh.mosaic.left), static_cast<int>(gh.mosaic.top),
           static_cast<int>(gh.mosaic.right),
           static_cast<int>(gh.mosaic.bottom)},
          Scaled(kChipMosaicCell), Scaled(kChipMosaicGap),
          Scaled(kChipMosaicMargin));
      const std::vector<StripMosaicSlot> slots =
          StripMosaicFill(gh.member_count);
      const COLORREF empty_plate = PhiBlendOver(palette.ink, ground, 10);
      const std::vector<unsigned char>& logo = PhiLogoBgra256();
      for (size_t slot_index = 0; slot_index < cells.size(); ++slot_index) {
        const RECT cell = PhiToRect(cells[slot_index]);
        const StripMosaicSlot& slot = slots[slot_index];
        if (slot.kind == StripMosaicCell::kFavicon &&
            slot.favicon_index >= 0 &&
            static_cast<size_t>(slot.favicon_index) < gh.icons.size()) {
          const GroupHeader::MosaicIcon& icon =
              gh.icons[static_cast<size_t>(slot.favicon_index)];
          if (!icon.bgra.empty()) {
            DrawFavicon(dc, cell, icon.bgra, icon.width, icon.height,
                        ground);
            continue;
          }
          if (icon.internal_page && !logo.empty() &&
              PhiDrawBgra(dc, cell, logo.data(), 256, 256)) {
            continue;
          }
          PhiFillRounded(dc, cell, empty_plate, Scaled(2));
        } else if (slot.kind == StripMosaicCell::kOverflow) {
          const std::wstring text =
              slot.overflow <= 9 ? L"+" + std::to_wstring(slot.overflow)
                                 : std::wstring(L"...");
          HGDIOBJ previous =
              chip_mosaic_font_ ? ::SelectObject(dc, chip_mosaic_font_)
                                : nullptr;
          ::SetTextColor(dc, PhiBlendOver(palette.ink, ground, 77));
          RECT rc = cell;
          PhiDrawTextAA(dc, text.c_str(), -1, &rc,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
          if (previous) {
            ::SelectObject(dc, previous);
          }
        } else {
          PhiFillRounded(dc, cell, empty_plate, Scaled(2));
        }
      }
    } else if (gh.user_titled && gh.count.right > gh.count.left) {
      // The count badge: the digits, bold and secondary, on the colour's
      // 28% tint (chipHoverTintColor) — only a titled group shows it, an
      // auto-name already carries the count.
      PhiFillCapsuleAA(dc, gh.count, PhiBlendOver(gh.color, ground, 71));
      HGDIOBJ previous =
          chip_count_font_ ? ::SelectObject(dc, chip_count_font_) : nullptr;
      ::SetTextColor(dc, palette.label_dim);
      RECT rc = gh.count;
      PhiDrawTextAA(dc, std::to_wstring(gh.member_count).c_str(), -1, &rc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      if (previous) {
        ::SelectObject(dc, previous);
      }
    }
    {
      // The hairline between the chip and what follows it
      // (updateChipRightSeparators): hidden while the chip is hovered, or
      // while its neighbour — the first member, or past a collapsed chip
      // the next tab or chip — is hovered or is the active tab.
      bool has_neighbour = false;
      bool neighbour_hides = false;
      for (size_t t = 0; t < tabs_.size(); ++t) {
        const TabRow& tab = tabs_[t];
        if (static_cast<int>(t) == dragged_tab) {
          continue;  // Out of the strip while it is dragged.
        }
        const LONG tab_left = tab.bounds.left + tab_dx(t);
        if (tab.bounds.right <= tab.bounds.left ||
            tab_left < gh.bounds.right) {
          continue;
        }
        if (tab_left - gh.bounds.right <= Scaled(kChipTrailGap)) {
          has_neighbour = true;
          neighbour_hides = tab.active || hovered_tab == static_cast<int>(t);
        }
        break;
      }
      if (!has_neighbour && i + 1 < group_headers_.size()) {
        const GroupHeader& next = group_headers_[i + 1];
        const LONG next_left = next.bounds.left + chip_dx(next);
        if (next.chip && next_left >= gh.bounds.right &&
            next_left - gh.bounds.right <=
                Scaled(kChipTrailGap) + Scaled(kChipLeadGap)) {
          has_neighbour = true;
          neighbour_hides = hover_kind == HoverKind::kGroupHeader &&
                            hover_index == static_cast<int>(i + 1);
        }
      }
      if (has_neighbour && !hovered && !neighbour_hides) {
        const int sep_x =
            static_cast<int>(gh.bounds.right) + Scaled(kChipTrailGap) / 2;
        RECT hairline = {sep_x, layout.tabs_top + Scaled(6), sep_x + 1,
                         strip_bottom - Scaled(6)};
        HBRUSH brush = ::CreateSolidBrush(palette.separator);
        ::FillRect(dc, &hairline, brush);
        ::DeleteObject(brush);
      }
    }
    if (!gh.collapsed && gh.container.right > gh.bounds.right) {
      paint_underline(gh);
    }
  }
}

// static
bool PhiSidebarView::BandHidden(const PhiSidebarRects& layout) {
  // The vertical layouts with the sidebar collapsed: the band has no width
  // (ShellLayoutInputs::sidebar_hidden). Comfortable's band is the strip in
  // the title bar and has no sidebar rect at all, so it never counts.
  return !layout.horizontal_tabs &&
         layout.sidebar.right <= layout.sidebar.left;
}

void PhiSidebarView::Paint(HDC dc,
                           const PhiSidebarRects& layout,
                           const PhiPalette& palette) {
  if (BandHidden(layout)) {
    return;  // Collapsed: the band is the floating panel's to paint.
  }
  PhiSpacesStrip* const strip = shell_->SidebarSpacesStrip();
  if (!strip->swap_running() || layout.horizontal_tabs) {
    PaintBand(dc, layout, palette);
    return;
  }
  // The Space switch slides the REAL rows (no snapshots): the leaving
  // Space's retained model paints shifted out at its frozen source palette,
  // the live (entering) model paints shifted in at the ramped palette, both
  // clipped to the band over the ramping ground.
  const double t = strip->SwapProgress();
  const bool forward = strip->swap_forward();
  const int band_w = Scaled(sidebar_dip());
  const int leave_dx = forward ? -static_cast<int>(t * band_w + 0.5)
                               : static_cast<int>(t * band_w + 0.5);
  const int enter_dx = forward ? band_w + leave_dx : leave_dx - band_w;
  const int save = ::SaveDC(dc);
  ::IntersectClipRect(dc, 0, layout.favorites.top, band_w,
                      layout.tabs_limit);
  POINT origin = {};
  ::GetViewportOrgEx(dc, &origin);
  ::SetViewportOrgEx(dc, origin.x + leave_dx, origin.y, nullptr);
  SwapBandModel();
  PaintBand(dc, layout, strip->swap_source_palette());
  SwapBandModel();
  ::SetViewportOrgEx(dc, origin.x + enter_dx, origin.y, nullptr);
  PaintBand(dc, layout, palette);
  ::SetViewportOrgEx(dc, origin.x, origin.y, nullptr);
  ::RestoreDC(dc, save);
  if (t >= 1.0) {
    // The page flips only now, the Mac's order.
    strip->NoteSlideLanded();
  }
}

void PhiSidebarView::SwapBandModel() {
  tabs_.swap(leaving_band_.tabs);
  pinned_.swap(leaving_band_.pinned);
  bookmarks_.swap(leaving_band_.bookmarks);
  group_headers_.swap(leaving_band_.headers);
}

}  // namespace phi_client

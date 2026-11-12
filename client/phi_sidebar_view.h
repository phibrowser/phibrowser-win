// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_VIEW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_VIEW_H_

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <set>
#include <optional>
#include <string>
#include <vector>

#include "client/phi_ids.h"
#include "client/phi_sidebar_model.h"

namespace phi_client {

// ---------------------------------------------------------------------------
// THE PURE HALF — no Win32, no framework, no paint. Two rules of the view
// that are decisions rather than drawing, so they are tested on the Linux
// host (test/model/sidebar_view_test.cc). A host translation unit defines
// PHI_SIDEBAR_VIEW_MODEL_ONLY before including this and gets this half
// alone; the client build sees the whole file.

// Tab ids most-recently-used first — the Ctrl+Tab switcher's order.
//
// RULES (the Mac's tabHistory): the ACTIVE tab is always the front; ids that
// have left the window fall out; everything else keeps its relative order,
// so cycling back through the switcher does not reshuffle the list behind
// it. A pane of a split counts as present — it is a live tab of this window
// even though its row is the merged one.
class PhiTabMru {
 public:
  PhiTabMru();
  ~PhiTabMru();

  const std::vector<TabId>& order() const { return order_; }
  bool empty() const { return order_.empty(); }

  // One rebuild's worth of bookkeeping: `live` is every tab id the window
  // still shows (both panes of a split included) and `active` is the one on
  // screen (invalid when the window shows none).
  void Sync(const std::vector<TabId>& live, TabId active) {
    std::erase_if(order_, [&](TabId id) {
      return std::find(live.begin(), live.end(), id) == live.end();
    });
    if (active.valid()) {
      std::erase(order_, active);
      order_.insert(order_.begin(), active);
    }
  }

 private:
  std::vector<TabId> order_;
};

// Inline, so the host test needs no .cc of its own (the rest of this file
// is Win32).
inline PhiTabMru::PhiTabMru() = default;
inline PhiTabMru::~PhiTabMru() = default;

// The bookmark cells an expanded chain reveals, in outline order (the Mac's
// NSOutlineView): the tree is depth-first, so a COLLAPSED folder hides the
// run of cells that follows it while their depth stays greater than its
// own. Returns positions in `cells`.
//
// A folder not in `expanded` is collapsed — the set is the disclosure state
// and nothing else, so a folder that goes away and comes back under the
// same guid comes back open, exactly as the Mac outline does.
// Inline, so the host test needs no .cc of its own (the rest of this file
// is Win32).
inline std::vector<size_t> PhiVisibleOutlineRows(
    const std::vector<SidebarCell>& cells,
    const std::set<std::string>& expanded) {
  // The tree is depth-first, so a collapsed folder's descendants are the
  // cells that follow it while their depth stays greater than its own.
  std::vector<size_t> visible;
  int collapsed_at = -1;
  for (size_t i = 0; i < cells.size(); ++i) {
    const SidebarCell& cell = cells[i];
    if (collapsed_at >= 0 && cell.depth > collapsed_at) {
      continue;
    }
    collapsed_at = -1;
    visible.push_back(i);
    if (cell.kind == SidebarStoredKind::kFolder &&
        !expanded.count(cell.guid)) {
      collapsed_at = cell.depth;
    }
  }
  return visible;
}

// The dragged row's current position in `rows` (-1 when it is gone). Drag
// state is keyed by SidebarKey, never by index: rows rebuild under a drag —
// a folder toggle, a notification arriving — and a key survives that where
// a position does not.
template <typename Rows>
int PhiIndexOfKey(const Rows& rows, const SidebarKey& key) {
  if (key.empty()) {
    return -1;
  }
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].key == key) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

}  // namespace phi_client

#if !defined(PHI_SIDEBAR_VIEW_MODEL_ONLY)

#include <windows.h>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "chrome/browser/phinomenon/phi_framework/phi_window_snapshot.h"
#include "client/phi_drag_image.h"
#include "client/phi_sidebar_actions.h"
#include "client/phi_sidebar_drag_session.h"
#include "client/phi_sidebar_drop.h"
#include "client/phi_sidebar_layout.h"
#include "client/phi_theme.h"
#include "client/phi_window.h"

namespace phi_client {

class AgentHost;
class ChromePrefsHost;
class PhiSidebarDragSession;
class PhiSpacesStrip;
class SidebarHost;
class SpacesHost;
class WindowHost;
class PhiShellWindow;
class PhiWindowModel;

// The sidebar's paint constants — design units (points at 100%), from the
// Mac client so the two look the same. The ones the GEOMETRY is built from
// live in phi_shell_layout.h (the pure, host-tested layout reads them);
// these are the paint-only ones, and the shell's remaining chrome takes
// the handful it shares by `using` rather than keeping a second copy
// (docs/architecture-review.md §3.9).
namespace sidebar_metrics {

// 36pt row -> 32pt pill (the vertical inset).
inline constexpr int kRowPad = 2;
inline constexpr int kCornerRadius = 6;  // The row radius (windows-native-ui.md §2.1).
inline constexpr int kFaviconSize = 14;  // The 16pt slot holds a 14pt icon.
inline constexpr int kBookmarkIndent = 10;  // SideBarOutlineView.indentation.
// The outline stops indenting past this depth: a deep tree must never push a
// row's pill past its own trailing edge (an inverted rect).
inline constexpr int kBookmarkMaxIndentDepth = 8;
inline constexpr int BookmarkIndentDepth(int depth) {
  return depth < 0 ? 0
                   : (depth > kBookmarkMaxIndentDepth ? kBookmarkMaxIndentDepth
                                                      : depth);
}
inline constexpr int kFolderIconSize = 16;  // The outline glyph fills the slot (§4.2).
// Pinned tabs: PinnedTabLayout — column limit by width breakpoint (3 at
// this sidebar width), 53pt minimum width.
inline constexpr int kPinnedMinWidth = 53;
inline constexpr int kPinnedFavicon = 18;
// The horizontal strip's pinned section (TabStripMetrics.PinnedTab): fixed
// 28pt square cells, 2pt spacing, leading the normal tabs.
inline constexpr int kStripPinSize = 28;
inline constexpr int kStripPinGap = 2;
inline constexpr int kStripPinToTabs = 6;
inline constexpr int kStripPinFavicon = 16;
// Tab groups: the Mac's 32pt TabGroupHeaderView on TabGroupCellView's card —
// the hover wash under a 1pt border hairline, radius 8; the group colour
// lives only in the header's dot. Members sit 4pt in from the header and
// the card's bottom (innerTableTop/BottomInset), 8pt in from its leading
// edge (the member cell's own edgesSpacing) and 4pt from its trailing one.
inline constexpr int kGroupHeaderHeight = 32;
inline constexpr int kGroupInnerInset = 4;
inline constexpr int kGroupMemberLeadInset = 8;
inline constexpr int kGroupMemberTrailInset = 4;
// The comfortable layout's tabs (the strip's band metrics are in
// phi_shell_layout.h): a tab runs from the strip's top inset all the way to
// the band's bottom edge — where the active one merges into the toolbar.
inline constexpr int kHTabHeight = 34;   // kTitleBarHeightTabs - kHTabStripTop
inline constexpr int kHTabWidth = 200;   // Tab.idealWidth (a resting tab)
inline constexpr int kHTabActiveWidth = 240;  // The active tab takes more room.
// Favicon, a breath, and the close box.
inline constexpr int kHTabMinWidth = 56;
inline constexpr int kHTabGap = 0;        // Tabs abut; a hairline divides them.
inline constexpr int kCloseBoxSize = 24;  // UnifiedTabCloseButton frame.
// The overlay scrollbars (Windows 11's thin bar): a 4 dip capsule 3 dip in
// from the band's trailing edge, at least 24 dip long, shown while the band
// scrolls or is hovered and faded out after it rests.
inline constexpr int kScrollbarWidth = 4;
inline constexpr int kScrollbarInset = 3;
inline constexpr int kScrollbarMinThumb = 24;
inline constexpr int kScrollbarHoldMs = 900;
inline constexpr int kScrollbarFadeMs = 300;
// Drag auto-scroll (D31): the edge zone that scrolls a band under a drag,
// the fastest step per animation tick, and the strip's pace per item.
inline constexpr int kAutoScrollEdge = 28;
inline constexpr int kAutoScrollMaxStep = 14;
inline constexpr int kAutoScrollStripMs = 250;
// The strip's group chips (the Mac's TabGroupChipView, 32pt, radius 8 like
// the tabs): the 16pt colour dot 6pt in, a 6pt gap, the semibold title,
// then trailing the 16pt chevron 6pt from the edge and, 4pt before it, the
// count badge (10pt bold digits, 6pt/1pt padding, on the colour's 28%
// tint) or the 28pt favicon mosaic (10pt cells, 2pt apart, 3pt in).
inline constexpr int kChipHeight = 32;
inline constexpr int kChipLeading = 6;
inline constexpr int kChipDot = 16;
inline constexpr int kChipDotToLabel = 6;
inline constexpr int kChipLabelSlack = 4;
inline constexpr int kChipLabelRightPad = 6;
inline constexpr int kChipCountToLabel = 6;
inline constexpr int kChipCountHPad = 6;
inline constexpr int kChipCountVPad = 1;
inline constexpr int kChipCountFont = 10;
inline constexpr int kChipMosaic = 28;
inline constexpr int kChipMosaicCell = 10;
inline constexpr int kChipMosaicGap = 2;
inline constexpr int kChipMosaicMargin = 3;
inline constexpr int kChipMosaicFont = 8;
inline constexpr int kChipCollapseGap = 4;
inline constexpr int kChipCollapseControl = 16;
inline constexpr int kChipChevron = 10;  // The chevron's point size.
inline constexpr int kChipMaxWidth = 180;
// The chip stands 2pt off the tab before it (the Mac's tab spacing) and
// 4pt off the one after, the hairline between them halfway.
inline constexpr int kChipLeadGap = 2;
inline constexpr int kChipTrailGap = 4;
// The group's underline runs 2pt in from the chip's leading edge and the
// last member's trailing one.
inline constexpr int kChipUnderlineInset = 2;

}  // namespace sidebar_metrics

// Chromium's TabGroupColorId palette (the Mac's GroupColor set is the same
// nine); the name keys the localized colour label
// (tabs.tabGroup.colorName.*) that titles an unnamed group, and the group
// header menu offers the same nine.
struct PhiGroupColor {
  const char* name;
  COLORREF color;
};
inline constexpr std::array<PhiGroupColor, 9> kPhiGroupColors = {{
    {"grey", RGB(0x80, 0x86, 0x8B)},
    {"blue", RGB(0x1A, 0x73, 0xE8)},
    {"red", RGB(0xD9, 0x30, 0x25)},
    {"yellow", RGB(0xF9, 0xAB, 0x00)},
    {"green", RGB(0x18, 0x80, 0x38)},
    {"pink", RGB(0xD0, 0x18, 0x84)},
    {"purple", RGB(0xA1, 0x42, 0xF4)},
    {"cyan", RGB(0x00, 0x7B, 0x83)},
    {"orange", RGB(0xFA, 0x90, 0x3E)},
}};
// kPhiGroupColors follows phi_framework::TabGroupColor's declaration order —
// that is what makes the enum an index into it.
inline const PhiGroupColor& PhiGroupColorFor(
    phi_framework::TabGroupColor color) {
  const size_t index = static_cast<size_t>(color);
  return index < kPhiGroupColors.size() ? kPhiGroupColors[index]
                                        : kPhiGroupColors[0];
}

// ---------------------------------------------------------------------------
// The shared antialiased shapes. EVERY curve in the client goes through
// phi_dwrite (see the fork's AA rule); these are the four call shapes the
// sidebar and the shell's remaining chrome both draw, spelled once so the
// two cannot drift apart on a radius.

// A filled rounded rect, the Mac client's selection/field shape.
void PhiFillRounded(HDC dc, const RECT& r, COLORREF fill, int radius);
// A fill rounded on its TOP corners only, square along the bottom — the
// comfortable tab's shape, which has to sit flush on the toolbar it merges
// into.
void PhiFillRoundedTop(HDC dc, const RECT& r, COLORREF fill, int radius);
void PhiStrokeRounded(HDC dc, const RECT& r, COLORREF fill, COLORREF border,
                      int radius);
void PhiDrawGlyph(HDC dc, const RECT& r, const wchar_t* glyph, COLORREF ink);
// The same, for an MDL2 ICON: rasterized by DirectWrite at `pixel_size` and
// alpha-blended over what is already painted, with the GDI path as fallback.
void PhiDrawGlyphIcon(HDC dc, const RECT& r, const wchar_t* glyph,
                      int pixel_size, COLORREF ink);
// Reader View's icon: SF Symbol `doc.plaintext`, drawn rather than
// substituted (Segoe MDL2's nearest neighbour is a different picture).
void PhiDrawReaderGlyph(HDC dc, const RECT& button, COLORREF ink, UINT dpi);
// `over` at `alpha`/255 on `base` — the Mac's translucent overlays, opaque.
COLORREF PhiBlendOver(COLORREF over, COLORREF base, int alpha);

// ---------------------------------------------------------------------------
// Readers over a WindowSnapshot — the window model's copy or a freshly
// pulled one alike. The split-drop paths map ids to tags and back several
// times inside a single drop, so they read the window ONCE and walk these
// instead of asking the framework again.
const phi_framework::TabSnapshot* PhiFindTabIn(
    const phi_framework::WindowSnapshot& snapshot, TabId id);
const phi_framework::TabSnapshot* PhiFindTagIn(
    const phi_framework::WindowSnapshot& snapshot, const std::string& tag);
const phi_framework::SplitSnapshot* PhiFindSplitIn(
    const phi_framework::WindowSnapshot& snapshot, const std::string& split_id);
// The live tab bound to a stored row, or `fallback` when the row has none.
// A pinned tab or bookmark is OPENED by the very press that arms its drag,
// so the view row the drag was armed from can still say "closed" when the
// drop lands.
TabId PhiLiveTabForRow(const phi_framework::WindowSnapshot& snapshot,
                       const std::string& guid,
                       TabId fallback);
// The framework names surfaces (a reader page recognised from what it
// shows, committed or not); the client parses no reader URLs.
SidebarSurface PhiSurfaceOf(const phi_framework::TabSnapshot& tab);
RECT PhiToRect(const SidebarRect& r);

// ---------------------------------------------------------------------------
// Which chrome element the pointer rests on. The SHELL owns the cursor —
// one hover across the caption band, the Spaces strip, the nav cluster and
// the sidebar, so that resting on one band takes the wash off the others —
// and the sidebar answers the hit tests and the wash rects for its own
// rows (PhiSidebarView::RowHoverRect).
enum class PhiChromeHover {
  kNone,
  kTab,
  kPin,
  kBookmark,
  kSpaceRoot,
  kGroupHeader,
  kChip,
  // The toolbar's back / forward / reload / search buttons — the index is
  // their position in the shell's own paint table.
  kNav,
  kNewTabRow,
  kFieldExt,
  // The Spaces strip's trailing slot — the "+" or, on overflow, the "..."
  // switcher standing in for it.
  kAddSpace,
};

struct PhiSidebarRects {
  RECT sidebar{};    // The whole band. Empty in Comfortable.
  RECT title_bar{};  // Where the horizontal strip's rows live instead.
  RECT content{};    // The page: a drag over it is a split drop.
  RECT favorites{};  // The pinned grid (or its reserved drop zone).
  // The bookmark outline's band: its rows (or the reserved drop zone) and
  // the separator row under them. Empty without bookmarks.
  RECT bookmarks{};
  // The two bands' capped viewport (ShellLayout::stored_band): `favorites`
  // and `bookmarks` are NATURAL rects that may run past it — the view
  // scrolls and clips them (PhiSidebarView::ScrolledStoredBands).
  RECT stored_band{};
  RECT bookmark_separator{};
  RECT new_tab_row{};
  RECT bottom_bar{};
  RECT spaces_strip{};
  RECT search_tabs{};  // Comfortable's "+" may not cross it.
  int tabs_left = 0, tabs_top = 0;
  int tabs_limit = 0;  // Bottom (vertical) or right (horizontal) edge.
  bool horizontal_tabs = false;
  bool spaces_switch = true;  // The Spaces strip / compact pip exists.
};

// One rendered tab row: what to draw and what a click at that row does.
struct TabRow {
  SidebarKey key;  // The tree node this row paints (kTab or kSplit).
  int index = 0;
  TabId id;  // Session id — stable while indices shift; pins bind to it.
  // The tab's favicon, copied out of the framework's scratch buffer (which
  // only survives until its next call).
  std::vector<unsigned char> favicon;
  int favicon_width = 0;
  int favicon_height = 0;
  bool active = false;
  std::wstring title;
  // The page address, as the hover preview card's second line wants it.
  std::wstring url;
  // Chromium native side-by-side: members share a splitId. The leading
  // member renders the MERGED row and absorbs its partner as pane B; the
  // partner never becomes its own row.
  std::string split_id;
  // Tab-group membership (Chromium native model); members sit under their
  // group's header row and hide when it is collapsed.
  std::string group_id;
  // The client identity tag (PhiTabData, session-persisted): a tab tagged
  // with a live pin/bookmark guid IS that row's bound tab.
  std::string tag;
  // chrome:// pages show the Phi mark when no favicon exists.
  bool internal_page = false;
  // DORMANT: holds navigation history it has not loaded yet (restored,
  // duplicated, or discarded). Phi does not load restored background tabs,
  // so this is a steady state for most of them — the row is drawn dimmed.
  bool unloaded = false;
  int b_index = -1;  // -1: not a merged row.
  TabId b_id;        // Pane B's session id, for its hover snapshot.
  bool b_active = false;
  std::wstring b_title;
  std::wstring b_url;
  std::vector<unsigned char> b_favicon;
  int b_favicon_width = 0;
  int b_favicon_height = 0;
  // A split of three or four panes: the panes after B, in order. The row
  // still merges them all; each takes an equal segment.
  struct ExtraPane {
    int index = -1;
    TabId id;
    bool active = false;
    bool unloaded = false;
    std::wstring title;
    std::wstring url;
    std::vector<unsigned char> favicon;
    int favicon_width = 0;
    int favicon_height = 0;
    RECT bounds{};  // Its segment of the row (activates the pane).
  };
  std::vector<ExtraPane> more;
  // 1 for a plain row, 2 to 4 for a merged one.
  int PaneCount() const {
    return b_index < 0 ? 1 : 2 + static_cast<int>(more.size());
  }
  // The tab in pane `pane` (0 = the leading pane); invalid past the end.
  TabId PaneId(int pane) const {
    if (pane <= 0) return id;
    if (pane == 1) return b_index >= 0 ? b_id : TabId();
    const size_t i = static_cast<size_t>(pane - 2);
    return i < more.size() ? more[i].id : TabId();
  }
  // The pane whose segment holds `pt`: 0 when the row is not merged or the
  // point is in none of the later segments.
  int PaneAt(POINT pt) const {
    if (b_index < 0) return 0;
    for (size_t i = 0; i < more.size(); ++i) {
      if (::PtInRect(&more[i].bounds, pt)) return static_cast<int>(i) + 2;
    }
    return ::PtInRect(&pane_b, pt) ? 1 : 0;
  }
  // The pane `tab` sits in, or -1.
  int PaneOf(TabId tab) const {
    for (int pane = 0; pane < PaneCount(); ++pane) {
      if (PaneId(pane) == tab) return pane;
    }
    return -1;
  }
  // Pane `pane`'s page, as its segment of the row and its hover chip draw
  // it. `pane` must be below PaneCount().
  const std::vector<unsigned char>& PaneFavicon(int pane, int* w, int* h) const {
    if (pane >= 2) {
      const ExtraPane& extra = more[static_cast<size_t>(pane - 2)];
      *w = extra.favicon_width;
      *h = extra.favicon_height;
      return extra.favicon;
    }
    *w = pane == 1 ? b_favicon_width : favicon_width;
    *h = pane == 1 ? b_favicon_height : favicon_height;
    return pane == 1 ? b_favicon : favicon;
  }
  const std::wstring& PaneUrl(int pane) const {
    return pane >= 2 ? more[static_cast<size_t>(pane - 2)].url
                     : pane == 1 ? b_url : url;
  }
  const std::wstring& PaneTitle(int pane) const {
    return pane >= 2 ? more[static_cast<size_t>(pane - 2)].title
                     : pane == 1 ? b_title : title;
  }
  // Every pane's tab, in order.
  std::vector<TabId> PaneIds() const {
    std::vector<TabId> ids;
    for (int pane = 0; pane < PaneCount(); ++pane) ids.push_back(PaneId(pane));
    return ids;
  }
  // A Peek or Reader View attached to this row's tab (either pane): the
  // trailing indicator, whose minus closes it (docs/sidebar-architecture
  // §4.6). While one is attached the row's own close box is withheld —
  // the indicator is the one close affordance, as on the Mac.
  SidebarSurface attached_surface = SidebarSurface::kContent;
  RECT bounds{};       // Row rect in client coords.
  RECT pane_b{};       // Pane B's segment of a merged row (activates it).
  RECT close_box{};    // The row's close affordance.
  RECT surface_box{};  // The attached-surface indicator.
};

// Sidebar tab-group headers (TabGroupHeaderView): chevron + color dot +
// title on the group's 18% color tint, clicking collapses/expands.
//
// On the comfortable strip the same vector holds the group CHIPS (the
// Mac's TabGroupChipView) — `chip` is set, `bounds` is the chip, and the
// part rects below place what it shows. Clicking the chip's body opens the
// group overview; the chevron (and a collapsed chip's mosaic) toggles.
struct GroupHeader {
  std::string id;
  std::wstring name;   // The stored title; empty for an unnamed group.
  std::wstring title;  // What the row shows: the name, or "Grey · 2 tabs".
  COLORREF color = 0;
  bool collapsed = false;
  RECT bounds{};     // The header row (the collapse toggle); the chip.
  // Vertical: header plus visible members — the tinted card. Strip: the
  // group's run, chip through its last visible member (the underline).
  RECT container{};

  // --- Strip chips only ---------------------------------------------------
  bool chip = false;
  bool user_titled = false;    // A stored title: the count badge shows.
  int member_count = 0;        // Tabs, both panes of a split counted.
  bool contains_active = false;
  RECT dot{};
  RECT label{};
  RECT count{};         // The count badge (expanded, user-titled).
  RECT mosaic{};        // The favicon mosaic (collapsed).
  RECT collapse_box{};  // The chevron.
  // The first four members' favicons, strip order, for the mosaic.
  struct MosaicIcon {
    std::vector<unsigned char> bgra;
    int width = 0;
    int height = 0;
    bool internal_page = false;  // chrome:// — the Phi mark stands in.
  };
  std::vector<MosaicIcon> icons;
};

// Pinned tabs (the band under the header) and bookmarks (the section above
// the open tabs) — both come from the client's own store, as on Mac.
struct ShortcutRow {
  SidebarKey key;  // kCell or kFolder.
  std::string guid;  // The store row, so the context menu can remove it.
  std::string url;
  std::wstring title;
  bool folder = false;
  // Folders only: the catalog symbol drawn in the row's icon slot.
  std::string icon;
  // Split rows: the divider axis the pair reopens with.
  std::string layout;
  // Bookmark outline (the Mac's NSOutlineView): an expanded folder splices
  // its children in below itself, one indent step deeper. `parent_guid`
  // names the folder a row sits in (empty = the root).
  int depth = 0;
  bool expanded = false;
  std::string parent_guid;
  // A pinned tab bound to a LIVE tab (openOrFocusPinnedTab): the tab lives
  // in the pin's cell — never in the tab list — and the active one gets the
  // Mac's selected wash + 2pt accent ring.
  bool open = false;
  bool active = false;
  // A Peek or Reader View attached to a bound pane — the indicator (a
  // trailing slot on a bookmark row, a corner badge on a pinned cell).
  SidebarSurface attached_surface = SidebarSurface::kContent;
  RECT surface_box{};
  // Bookmark rows while open: the hover minus that closes the bound tab and
  // leaves the bookmark (the Mac's 7ec6e1b7).
  RECT close_box{};
  // The bound tab's session id while `open` — what the hover preview asks
  // the framework for a snapshot of. A pinned PAIR binds both: `tab_id` is
  // this row's own tab, `tab_id_b` its partner's.
  TabId tab_id;
  TabId tab_id_b;
  // DORMANT bound panes (the tab row's `unloaded`): the pane's live tab
  // holds history it has not loaded — restored with the session and never
  // visited, or discarded. Its favicon, and the cell's open dot, draw at
  // the Mac's 30% (TabFaviconPresentation.reclaimedOpacity) so an open
  // cell whose page is not really there reads apart from a loaded one. A
  // closed cell has no tab and so is never dormant.
  bool unloaded = false;
  bool unloaded_b = false;
  // A single bookmark row whose open tab is on a page other than the saved
  // one: a slash sits between the favicon and the title (the Mac's
  // NavigatedAwaySeparator, `e917b732`).
  bool navigated_away = false;
  // A split cell of three or four panes: the panes after B, in order, each
  // with what the store holds for it and its bound tab while open.
  struct ExtraPane {
    std::string url;
    std::wstring title;
    std::vector<unsigned char> favicon;
    int favicon_width = 0;
    int favicon_height = 0;
    TabId tab_id;
    bool unloaded = false;
  };
  std::vector<ExtraPane> more;
  // 1 for a single cell, 2 to 4 for a split one.
  int PaneCount() const {
    return url_b.empty() ? 1 : 2 + static_cast<int>(more.size());
  }
  // Where the row SITS (scrolled with the stored bands) — what paint and
  // the drag geometry read — and that rect clipped to the stored viewport,
  // what every hit test reads: a row scrolled out of the viewport (or the
  // part of it that is) must not catch the pointer over "+ New Tab" or the
  // tab list. The two agree whenever nothing is scrolled.
  RECT frame{};
  RECT bounds{};
  // The favicon stored with the row (a pinned tab has no live tab to ask).
  std::vector<unsigned char> favicon;
  int favicon_width = 0;
  int favicon_height = 0;
  // Split cell: one row whose second pane rides url_b/title_b/favicon_b
  // (a pinned split spans two grid slots; a bookmark split draws both
  // favicons and a "primary • secondary" title). Pane B's live tab is
  // tagged TagFor(guid, true) and lands in tab_id_b.
  std::string url_b;
  std::wstring title_b;
  std::vector<unsigned char> favicon_b;
  int favicon_b_width = 0;
  int favicon_b_height = 0;
};

// What the sidebar asks of its shell (docs/architecture-review.md §3.5).
// Implemented PRIVATELY by the shell, so none of this widens its public
// surface.
class PhiSidebarShell {
 public:
  virtual HWND SidebarShellWindow() const = 0;
  // The controller the split-drop overlay and the tear-off hit test are
  // addressed against, as with the native pages and the Spaces strip.
  virtual PhiShellWindow* SidebarOwner() = 0;
  virtual int SidebarScaled(int design_units) const = 0;
  virtual UINT SidebarDpi() const = 0;
  // The sidebar's width in DIP — the shell owns the resize band.
  virtual int SidebarWidthDip() const = 0;
  // The shell's per-turn Layout, sliced. Deriving it may run ComputeLayout,
  // so the callers hold the result rather than asking per row.
  virtual PhiSidebarRects SidebarRects() = 0;
  // The two band sizes the LAYOUT is derived from, counted from the tree
  // before any row is placed.
  virtual void SidebarSetRowCounts(int pinned_rows, int bookmark_rows) = 0;
  // A drag that could land in an EMPTY stored band is in flight (or has
  // ended): the layout reserves (releases) the band's drop zone. The
  // caller schedules the rebuild that re-places the rows under it.
  virtual void SidebarSetDropZones(bool pinned, bool bookmark) = 0;
  // The bookmark-bar and reader inputs the layout caches, re-read once per
  // rebuild after the active tab is known.
  virtual void SidebarRefreshLayoutInputs() = 0;
  // The ramped palette the whole chrome is painting with right now.
  virtual PhiPalette SidebarPalette() const = 0;
  // The browser this slot SHOWS; invalid between the last browser going and
  // teardown.
  virtual BrowserId SidebarActiveBrowser() const = 0;
  // The slot's active Space id — an agent Space hosted for a watch shows
  // no pinned tabs or bookmarks, like an agent window's own sidebar.
  virtual std::string SidebarActiveSpace() const = 0;
  // Its name, for the bookmark root row.
  virtual std::wstring SidebarActiveSpaceName() const = 0;
  // The framework's CURRENT state for `browser`, pulled typed — for the
  // gesture paths that just mutated the strip and act on the result inside
  // the same turn. Never for rows or paint: those read the window model.
  virtual bool SidebarPullSnapshot(
      BrowserId browser,
      phi_framework::WindowSnapshot* out) const = 0;
  // True when the split holding `tab_id` stacks its panes.
  virtual bool SidebarSplitIsStacked(TabId tab_id) const = 0;
  // Where Chromium laid the ACTIVE tab's split panes out, in slot order
  // and shell client coordinates (the framework's TabContentBoundsById) —
  // the pane-mode drop cards trace them. Empty when the active tab is not
  // in a split or the layout has not run yet.
  virtual std::vector<RECT> SidebarSplitPaneRects() const = 0;
  // The chrome fonts the rows draw with, all built and owned by the shell.
  virtual HFONT SidebarGlyphFont() const = 0;
  virtual HFONT SidebarSemiboldFont() const = 0;
  // The rows' text face (the DC's font while the band paints), for a
  // painter drawing a row off-screen.
  virtual HFONT SidebarTextFont() const = 0;
  // The hover cursor (see PhiChromeHover): which element, and where in its
  // own vector.
  virtual PhiChromeHover SidebarHoverKind() const = 0;
  virtual int SidebarHoverIndex() const = 0;
  // §5.8: the pointer is over the hovered row's surface TILE (the tile
  // alone swaps to its close ✕; the row's hover no longer flips it).
  virtual bool SidebarSurfaceTileHovered() const = 0;
  // The favicon of the surface attached to `origin_tab_id` — the peeked
  // page's, the origin's for a reader — at its native size.
  virtual bool SidebarSurfaceFavicon(TabId origin_tab_id,
                                     std::vector<unsigned char>* bgra,
                                     int* width,
                                     int* height) const = 0;
  // The Spaces strip, whose pips ride the same drag machinery as the rows.
  virtual PhiSpacesStrip* SidebarSpacesStrip() = 0;
  // THE refresh entry point: marks the rows dirty and posts one coalesced
  // rebuild for this turn.
  virtual void SidebarScheduleRefresh() = 0;
  // A rebuild noticed the active tab crossing a native-page route.
  virtual void SidebarSyncNativePages() = 0;
  // The chrome as it paints RIGHT NOW, into a bitmap — what the drag image
  // lifts the dragged element's pixels out of.
  virtual PhiDragBitmap SidebarCaptureChrome(
      const RECT& client_rect) = 0;
  // The reparented browser child, whose on-screen pixels are the drag
  // card's page.
  virtual HWND SidebarContentWindow() const = 0;
  // The New Tab entry point (the "+ New Tab" row honours the "New tab
  // behavior" pref, which the shell owns).
  virtual void SidebarStartNewTab() = 0;
  // The rows, as accessibility sees them (docs/architecture-review.md §3.6,
  // ranked finding 31): the shell publishes them on its window.
  virtual void SidebarPublishHits(std::vector<PhiHit> hits) = 0;

 protected:
  // Non-virtual and protected: the shell OWNS the sidebar, never the other
  // way round, so nothing ever deletes through this pointer.
  ~PhiSidebarShell() = default;
};

// THE sidebar (docs/architecture-review.md §3.5; lane 1 clusters H and J
// plus the sidebar half of Q): the row pipeline SidebarInputs ->
// BuildSidebar -> LayoutSidebar -> rows, the paint of that band, its hit
// tests, its scrolling, the expanded-bookmark set and the MRU order. Every
// mutation it resolves leaves as a SidebarCommand.
//
// INVARIANTS
//  * The row vectors are written in ONE function — Rebuild() — on the
//    posted rebuild turn. Paint, input and menus READ them; none of them
//    writes one (docs/sidebar-architecture.md §2.7, §7).
//  * A hit test answers a SidebarKey, never an index: rows rebuild under a
//    drag (a folder toggle, a notification arriving) and a key survives
//    that where a position does not.
//  * "Which window shows tab X" is answered from the TREE, owner_of
//    included — a tab hidden in a pinned or bookmark cell is still shown by
//    this window (lane 1 §2.1 item 4).
//  * The drag session exists only between ArmDrag and commit or cancel; the
//    view owns it, so a sidebar going away takes the drag image and the
//    split-drop overlay with it.
class PhiSidebarView {
 public:
  // The narrow services the view calls (docs/architecture-review.md §3.5):
  // its own store rows and the sidebar mutator, the palette the band paints
  // with, the client-wide layout mode, the agent mask badges — plus the
  // window host it does not call itself and hands to the drag session for
  // tear-off and cross-window drops. Any of them may be null.
  PhiSidebarView(PhiSidebarShell* shell,
                 SlotId slot,
                 phi_framework::PhiFramework* framework,
                 SidebarHost* sidebar_host,
                 SpacesHost* spaces_host,
                 ChromePrefsHost* prefs_host,
                 AgentHost* agent_host,
                 WindowHost* window_host,
                 const PhiWindowModel* model,
                 bool agent,
                 bool incognito);
  ~PhiSidebarView();

  PhiSidebarView(const PhiSidebarView&) = delete;
  PhiSidebarView& operator=(const PhiSidebarView&) = delete;

  using HoverKind = PhiChromeHover;
  using SlideVec = PhiSlideVec;

  // --- The row pipeline ---------------------------------------------------
  // THE writer of the row vectors. Runs on the shell's posted rebuild turn.
  void Rebuild();
  const SidebarTree& tree() const { return sidebar_tree_; }
  const std::vector<TabRow>& tabs() const { return tabs_; }
  const std::vector<ShortcutRow>& pinned() const { return pinned_; }
  const std::vector<ShortcutRow>& bookmarks() const { return bookmarks_; }
  // The Space bookmark root (the Mac's SpaceRootSidebarItem): a header row
  // named after the Space above its bookmarks, collapsible per Space. It is
  // presentation only, so no stored parent changes, and it is no row of
  // bookmarks(), whose positions the drag session works in.
  struct SpaceRootRow {
    bool shown = false;
    bool expanded = true;
    // Collapsed over the active bookmark: the Mac floats that bookmark under
    // the root; here the root itself says so.
    bool contains_active = false;
    std::wstring title;
    RECT frame{};
    RECT bounds{};
  };
  const SpaceRootRow& space_root() const { return space_root_; }
  // No bookmark in the tree at all, whatever the root is hiding.
  bool OutlineEmpty() const { return sidebar_tree_.bookmarks.empty(); }
  SidebarKey SpaceRootKey() const;
  // Where a drop at `pointer_y` lands in the outline, the root row included.
  DropResolution ResolveOutlineDrop(int pointer_y,
                                    const SidebarKey& dragging) const;
  // No stored bands at all: incognito, an agent Space. A drag reserves no
  // drop zone here.
  bool stored_bands_isolated() const { return stored_bands_isolated_; }
  const std::vector<GroupHeader>& group_headers() const {
    return group_headers_;
  }
  // Comfortable's "+" in the caption band, trailing the last tab.
  const RECT& add_tab_button() const { return add_tab_button_; }
  // Comfortable's overflow chevron, between the last tab that fits and the
  // "+": up while any strip item is scrolled out of the strip's window;
  // a click opens the tab search palette over every tab. Empty otherwise.
  const RECT& strip_overflow_button() const { return strip_overflow_button_; }
  // Its ordinal in the shell's nav-hover table (PhiShellWindow::
  // NavHoverTable), whose hover wash the chevron paints with.
  static constexpr int kStripOverflowNavOrdinal = 21;
  // The ACTIVE tab, after the tree's surface -> origin redirection, and its
  // URL — what the omnibox, the reader affordance and the native-page route
  // all read.
  TabId active_tab_id() const { return active_tab_id_; }
  const std::string& active_tab_url() const { return active_tab_url_; }
  // True when `tab_id` is one this window SHOWS: a content row, or a tab
  // hidden inside one of its stored cells (the tree's owner_of).
  bool ShowsTab(TabId tab_id) const;
  // Tab ids most-recently-used first — the Ctrl+Tab switcher's order.
  const std::vector<TabId>& mru() const { return tab_mru_.order(); }
  // The placed rows (keys + client rects) for the agent dialect's
  // window.layout, so a box probe can aim a real drag without pixels.
  base::ListValue LayoutRowsJson() const;
  // The drag in flight, for a probe: kind, band, the gap slot, the folder
  // the drop lands inside. Empty when nothing drags.
  base::DictValue DragStateJson() const;
  // The rows as assistive technology sees them, rebuilt with them.
  const std::vector<PhiHit>& hits() const { return hits_; }

  // --- View state ---------------------------------------------------------
  // Folders the user has disclosed in the outline (session-local view
  // state, like the Mac outline's). Returns true when the folder ended
  // expanded.
  bool ToggleExpanded(const std::string& guid);
  void Expand(const std::string& guid) { expanded_bookmarks_.insert(guid); }
  bool IsExpanded(const std::string& guid) const {
    return expanded_bookmarks_.count(guid) > 0;
  }
  // The wheel over the sidebar — the stored bands' viewport scrolls the
  // pinned grid and the outline, anywhere else the tab list — or over
  // comfortable's strip, which steps its window one item per notch. True
  // when consumed.
  bool OnWheel(POINT client_pt, int wheel_delta);
  // `layout` (the shell's, natural) with the pinned grid and the outline
  // where they sit on screen: scrolled with the stored bands and clipped to
  // their capped viewport (layout.stored_band). What the drop bands and
  // the drop zones read; the row placement reads the natural rects.
  PhiSidebarRects ScrolledStoredBands(const PhiSidebarRects& layout) const;

  // --- Gestures -----------------------------------------------------------
  // The stored bands (the pinned grid, the bookmark outline) and the
  // content rows (group headers, tab rows). Two entry points because the
  // shell's own bands are hit-tested between them, in the order the pixels
  // stack. True when the point belonged to a row.
  bool OnStoredRowClick(POINT client_pt);
  bool OnContentRowClick(POINT client_pt);
  // Middle-click closes a tab row (the Mac tab list's otherMouseDown);
  // pins and bookmarks are persistent and ignore it.
  bool OnMiddleClick(POINT client_pt);
  // The tab row whose body is under the point (not its close box or surface
  // indicator), and for a merged row the pane (0 or 1; -1 otherwise). Empty
  // key when there is none. Double-click closing reads this.
  SidebarKey TabBodyAt(POINT client_pt, int* pane) const;

  // --- Keyboard (F6) ------------------------------------------------------
  // The sidebar in the F6 cycle (address field -> sidebar -> page): a
  // keyboard-only focus ring over the pins, the outline, the group headers
  // and the tabs, in paint order. The shell window holds the system focus
  // meanwhile and hands its keys here. Begin answers false when the band
  // has nothing to focus (collapsed, empty).
  bool BeginKeyboardFocus();
  void EndKeyboardFocus();
  bool keyboard_focused() const { return kb_focus_; }
  // Up/Down (Left/Right) move the ring, Home/End jump, Enter activates,
  // Space toggles a folder or a group (activates anything else), Delete
  // closes a tab, Esc hands the focus to the page. True when consumed.
  bool OnKeyDown(UINT vk);
  // Where Shift+F10 / the Menu key opens the focused row's context menu
  // (its centre, client coordinates); false with no focused row.
  bool KeyboardMenuPoint(POINT* client_pt) const;
  // Sends a command to the host against the current tree; the host refreshes.
  bool Apply(const SidebarCommand& command);
  // Click on a pin or a sidebar bookmark row: focus the bound live tab if it
  // still exists, otherwise open the row's url as a new bound tab.
  void OpenOrFocusPinned(const std::string& guid, const std::string& url);
  // The pinned cell under the point whose tab is open: its guid, and for a
  // pinned pair the pane (0 or 1; -1 otherwise). Empty when there is none.
  std::string OpenPinAt(POINT client_pt, int* pane) const;
  // A double-click on an open pinned cell: its tab goes back to the pinned
  // URL when it has left it (the Mac's navigatePinnedTabToOriginalURL), and
  // with `keep_current` the page it was on stays behind as a normal tab
  // (separatePinnedTabFromCurrentURL). True when a tab was sent back.
  bool ReturnPinToSavedUrl(const std::string& guid, int pane,
                           bool keep_current);
  // Splits `primary` with `secondary`, after dropping any pin or bookmark
  // binding on EITHER pane (a split never lives in a stored cell).
  bool SplitTabsNormalized(BrowserId browser, TabId primary, TabId secondary);
  // Drops a tab's pin/bookmark binding: the stored row stays put as an
  // unopened one, the tab joins the plain list.
  void UnbindStoredTab(BrowserId browser, TabId tab_id);
  // Pairs two live tabs into a split with `primary_id` leading. Returns the
  // split id.
  std::string FormSplitOrdered(BrowserId browser,
                               TabId primary_id,
                               TabId secondary_id,
                               const char* layout);

  // --- Drag (PhiSidebarDragSession) ---------------------------------------
  // Arms a drag on `key`. The session lives from here to the commit or the
  // cancel and no longer.
  void ArmDrag(const SidebarKey& key, POINT pt);
  // The tab row a press armed WITHOUT activating it: a plain click
  // activates on release, a drag never does.
  void NoteDragClickTab(TabId tab_id);
  // A folder row's click, delivered on the release by the drag session
  // when the press never became a drag.
  void ClickFolder(const std::string& guid);
  void UpdateDrag(POINT pt);
  void CommitDrag();
  // An external cancel (the capture was taken away, a panel press).
  void AbortDrag();
  // Esc during a drag (§4.8): the ghost slides home, nothing changes.
  // False when no drag was active.
  bool CancelDragEscape();
  bool drag_armed() const { return drag_ != nullptr; }
  bool drag_active() const;
  PhiSidebarDragSession* drag() const { return drag_.get(); }
  // The reorder slide's clock tick; false once the leg has settled.
  bool OnSlideTimer();
  // The hover-to-expand dwell elapsed (PhiSidebarDragSession::
  // kHoverExpandTimer, routed by the shell).
  void OnHoverExpandTimer();
  // The 2px accent insertion line, over the composed frame.
  void DrawDragIndicator(HDC dc);
  // A tab dropped on the comfortable strip lands from `drop_left` (client
  // x, where the lifted tab was released): on the rebuild that follows the
  // move it glides from there into its new slot over kLandingMs, painted
  // over its neighbours — the Mac's clearDraggingPresentation snapping the
  // source view to the proxy and performLayout(.dataChanged) animating it
  // home. Armed for one rebuild only.
  void BeginStripLanding(const SidebarKey& key, int drop_left);
  // Disarms it (the move did not happen after all) or stops a glide.
  void CancelStripLanding();
  static constexpr int kLandingMs = 180;

  // --- Paint --------------------------------------------------------------
  // The band is collapsed out of the shell (the vertical layouts'
  // sidebar_hidden): its rows keep no geometry and nothing paints.
  static bool BandHidden(const PhiSidebarRects& layout);
  // The band (pinned grid through tab list): once normally, or the two-pass
  // slide while a Space switch animates.
  void Paint(HDC dc, const PhiSidebarRects& layout, const PhiPalette& palette);
  // The palette the horizontal strip's ACTIVE tab wears: the address row's
  // page colour, which the tab merges into (the Mac's TabItemView
  // setActivePageStyle); nullopt = the chrome palette. Set before Paint.
  void SetActiveTabPagePalette(std::optional<PhiPalette> palette) {
    active_tab_page_palette_ = std::move(palette);
  }
  // The wash rect of one of the sidebar's own hovered rows, empty for any
  // other kind — so a hover change invalidates the two elements, not the
  // whole shell.
  RECT RowHoverRect(HoverKind kind, int index) const;
  // Draws a favicon's BGRA pixels into `dest`, composited over `background`
  // (GDI has no alpha blit without msimg32, and we always know what is
  // behind the icon).
  // `opacity` < 1 draws the icon faded over `background` (a reclaimed tab).
  void DrawFavicon(HDC dc, const RECT& dest, const unsigned char* bgra,
                   int width, int height, COLORREF background,
                   float opacity = 1.0f);
  // The same from a row's own buffer, which is checked first: one that is
  // not exactly width*height*4 bytes draws nothing.
  void DrawFavicon(HDC dc, const RECT& dest,
                   const std::vector<unsigned char>& bgra, int width,
                   int height, COLORREF background, float opacity = 1.0f);
  // A bookmark folder's icon slot: the catalog symbol it wears, or the
  // plain folder glyph for the catalog's "default".
  void DrawFolderIcon(HDC dc, const RECT& slot, const std::string& icon_id,
                      COLORREF ink, bool expanded);
  // The attached-surface indicator: a plate with the kind's glyph, or the
  // minus that closes it while its row is hovered.
  // An empty stored band's drop zone while a drag is in flight (the Mac's
  // PinnedTabViewController.emptyView): a dashed pill carrying a glyph and
  // a hint, washed with the accent while the drag hovers it. `stacked`
  // puts the glyph above the hint (the tall pinned zone); otherwise the
  // two sit in a row.
  void PaintDropZone(HDC dc, const RECT& zone, bool hot, const wchar_t* glyph,
                     int glyph_dip, const wchar_t* hint, bool stacked,
                     const PhiPalette& palette);
  // §5.8's tile: `tile_dip` square (20 on rows, 18 on a pinned cell)
  // centred in `box`, card fill, 1 px stroke, holding the surface tab's
  // favicon (the reader glyph for Reader View) — or a 12 dip ✕ while the
  // tile itself is hovered.
  void DrawSurfaceTile(HDC dc, const RECT& box, SidebarSurface kind,
                       bool hovered, COLORREF ground, TabId origin_tab,
                       int tile_dip);
  // leaving_band_ <-> the live row vectors. The Space slide paints both
  // models, so the strip exchanges them twice per frame.
  void SwapBandModel();
  void DropLeavingBand() { leaving_band_ = BandModel(); }

  // --- Geometry the drag session shares -----------------------------------
  // The pinned grid's placement walk over arbitrary cell widths (1 or 2
  // slots) — layout uses it for the real cells, the drag preview with a
  // phantom cell spliced in at the insertion slot.
  // `rows`, when given, receives the grid's row count for `units`.
  std::vector<RECT> PinnedGridWalk(const std::vector<int>& units,
                                   const PhiSidebarRects& layout,
                                   int* rows = nullptr) const;
  // The REAL grid's row count — what the layout's pinned band is sized to.
  int pinned_grid_rows() const { return pinned_grid_rows_; }
  // The bookmark cells an expanded chain reveals, in outline order.
  std::vector<const SidebarCell*> VisibleBookmarkCells() const;
  void ToggleSpaceRoot();
  // An outline row's indent: the root's children sit one level in.
  int OutlineIndentDepth(int depth) const;
  // The sections as the pure drop resolvers see them.
  std::vector<DropRow> TabDropRows() const;
  std::vector<DropRow> PinnedDropRows() const;
  std::vector<DropRow> BookmarkDropRows() const;
  // The shell, for the session that shares it.
  PhiSidebarShell* shell() const { return shell_; }
  phi_framework::PhiFramework* framework() const { return framework_; }
  SidebarHost* sidebar_host() const { return sidebar_host_; }
  // What the drag session it owns takes: the Space list for the pip
  // reorder and its palette, and the window set for tear-off and the
  // cross-window drop.
  SpacesHost* spaces_host() const { return spaces_host_; }
  WindowHost* window_host() const { return window_host_; }
  SlotId slot() const { return slot_; }
  // Nothing persists from here: an incognito window, or a window showing
  // an Incognito Space (docs/incognito-space-port.md).
  bool incognito() const;
  // The agent holds this (agent) window: the rows' close boxes are inert.
  void set_agent_holds_input(bool holds) { agent_holds_input_ = holds; }

 private:
  // Every constant in this class is a DESIGN UNIT — a pixel at 100%
  // scaling; nothing reaches the screen without going through here.
  int Scaled(int design_units) const {
    return shell_->SidebarScaled(design_units);
  }
  UINT dpi() const { return shell_->SidebarDpi(); }
  int sidebar_dip() const { return shell_->SidebarWidthDip(); }

  // Pinned cells and the bookmark outline as paint rows, from the tree's
  // cells (order, depth, bindings) and the store's views (titles, urls,
  // favicons), placed within `layout`. The tree must be built first.
  void RebuildShortcuts(const PhiSidebarRects& layout,
                        const phi_framework::WindowSnapshot& snapshot);
  // The pinned grid's metrics for LayoutPinnedGrid, from the sidebar width
  // and `layout.favorites`.
  PinnedGridMetrics GridMetrics(const PhiSidebarRects& layout) const;
  // Reopens or refocuses a STORED split (a pinned pair or a split
  // bookmark): missing panes are created, tags applied, and the pair
  // married.
  void OpenStoredSplit(const ShortcutRow& row);
  // The band, once, at one palette.
  void PaintBand(HDC dc, const PhiSidebarRects& layout,
                 const PhiPalette& palette);
  // The strip's group chips and each expanded group's coloured underline —
  // after the tab rows, since the underline wraps the active tab's fill.
  void PaintStripGroups(HDC dc, const PhiSidebarRects& layout,
                        const PhiPalette& palette);
  // The chip's two small faces (the count badge's 10pt bold, the mosaic's
  // 8pt semibold), derived from the shell's text face at the current DPI.
  void EnsureChipFonts();
  // `text`'s width in `font`, for the chip widths measured at rebuild.
  int MeasureText(HFONT font, const std::wstring& text) const;
  // The dragged element's slide offsets, zero with no drag in flight.
  PhiSlideVec RowSlide(const std::string& guid) const;
  double TabSlide(TabId tab_id) const;
  // The strip's landing glide: on the rebuild after the drop, the landed
  // row's offset from where it was released, eased to zero by the animator.
  void StartStripLanding(const PhiSidebarRects& layout);
  HWND shell_hwnd() const;
  // Publishes the rows into the shell's hit list — one hit per row, keyed
  // so a provider handed out before a rebuild finds its row again.
  void PublishHits(const PhiSidebarRects& layout);
  // The keyboard ring: the rows in focus order, a key's rect, a move (which
  // reveals the row on the next rebuild), and what Enter / Space do.
  std::vector<SidebarKey> KeyboardOrder() const;
  RECT KeyRect(const SidebarKey& key) const;
  void MoveKeyboardFocus(const SidebarKey& key);
  void ActivateKey(const SidebarKey& key, bool toggle);
  // The overlay scrollbars: shown (and faded) after a scroll, painted per
  // band at the opacity the clock and the hover give.
  void ShowScrollbars();
  double ScrollbarOpacity(bool hovered) const;
  void PaintScrollbar(HDC dc,
                      const RECT& track,
                      int scroll,
                      int scroll_max,
                      const PhiPalette& palette,
                      double opacity);
  // The drag's context (the Space, the layout, the band's collapse) moved
  // under it: the rows it resolved against are gone, so it aborts.
  bool DragContextChanged() const;
  // Drag auto-scroll (D31): a drag resting near a scrolling band's edge
  // scrolls it on the animator's clock until it leaves the zone.
  // `target` 0 = the tab list, 1 = the stored bands, 2 = the strip.
  int DragAutoScrollZone(POINT pt, int* target, int* depth) const;
  void UpdateDragAutoScroll(POINT pt);
  void DragAutoScrollTick();
  void StopDragAutoScroll();

  // The Mac's Tab.pushKeepLoaded (`375fed24`, `734319a9`): tells Chromium to
  // keep the live tabs of this window's pinned cells loaded while Settings ▸
  // Advanced ▸ "Keep pinned tabs loaded" is on, and to let go of the rest.
  // Bookmark-bound tabs are not kept. Runs on every rebuild, and calls the
  // framework only for a tab whose state it has not pushed yet or that
  // changed, since a fresh page starts out not kept.
  void PushKeepLoaded(BrowserId browser,
                      const phi_framework::WindowSnapshot& snapshot);
  // Live tab -> the state last pushed for it, for the tabs of the snapshot
  // last seen.
  std::map<int, bool> keep_loaded_pushed_;

  // Owns this view and outlives it.
  RAW_PTR_EXCLUSION PhiSidebarShell* const shell_;
  const SlotId slot_;
  // Owned by the framework (a singleton in chrome.dll); non-owning, and
  // across a module boundary, so not a raw_ptr.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* const framework_;
  // The coordinator's services (§3.5). It outlives every shell, so these
  // are non-owning and never rebound.
  RAW_PTR_EXCLUSION SidebarHost* const sidebar_host_;
  RAW_PTR_EXCLUSION SpacesHost* const spaces_host_;
  RAW_PTR_EXCLUSION ChromePrefsHost* const prefs_host_;
  RAW_PTR_EXCLUSION AgentHost* const agent_host_;
  RAW_PTR_EXCLUSION WindowHost* const window_host_;
  // The coordinator's window model: the one source for rows, splits and
  // groups. It owns the instance and outlives every shell.
  RAW_PTR_EXCLUSION const PhiWindowModel* const model_;
  const bool incognito_;
  // An agent Space's sidebar: isolated like incognito's — no pinned tabs,
  // no bookmarks persist there (the Mac's BrowserState agent-Space rule).
  const bool agent_;
  bool agent_holds_input_ = false;

  std::vector<TabRow> tabs_;
  std::optional<PhiPalette> active_tab_page_palette_;
  // The model — what every live tab IS (docs/sidebar-architecture.md §4.5).
  // tabs_, pinned_ and bookmarks_ are paint/hit-test caches derived from it
  // in Rebuild(); nothing decides bindings or split rows anywhere else.
  SidebarTree sidebar_tree_;
  RECT add_tab_button_{};
  std::vector<GroupHeader> group_headers_;
  std::vector<ShortcutRow> pinned_;
  std::vector<ShortcutRow> bookmarks_;
  SpaceRootRow space_root_;
  // The active tab's URL (captured by Rebuild) and its session id.
  std::string active_tab_url_;
  TabId active_tab_id_;
  // Session-local view state: the disclosed folders, the list's scroll.
  std::set<std::string> expanded_bookmarks_;
  int scroll_offset_ = 0;
  int tabs_scroll_max_ = 0;
  // The stored bands' scroll inside their capped viewport, in pixels.
  int stored_scroll_ = 0;
  int stored_scroll_max_ = 0;
  // Comfortable's strip window: how many strip items (chips and tabs, in
  // order) are scrolled out ahead of it, and the most that may be — the
  // window never ends short of the strip's last item.
  int strip_first_ = 0;
  int strip_first_max_ = 0;
  RECT strip_overflow_button_{};
  // The scroll state is per Space (a switch must not land the incoming
  // Space's list at the leaving one's offset): saved under the Space it
  // belongs to, restored when that Space shows again.
  struct ScrollState {
    int list = 0;
    int stored = 0;
    int strip_first = 0;
  };
  std::map<std::string, ScrollState> scroll_by_space_;
  std::string scroll_space_;
  // The active tab the list / the strip last scrolled into view, and the
  // strip's extent then: a new activation (or a changed strip) reveals the
  // row once, so a later wheel can still move away.
  TabId revealed_active_;
  int revealed_strip_limit_ = 0;
  int revealed_strip_items_ = 0;
  // The pinned grid's ROW count, counted from the tree before the layout is
  // derived (the band it occupies pushes everything below it down) and read
  // back when the outline is placed under it.
  int pinned_grid_rows_ = 0;
  bool stored_bands_isolated_ = false;
  // Tab ids of the ACTIVE browser, most-recently-used first.
  PhiTabMru tab_mru_;
  // The strip chips' own faces (EnsureChipFonts), rebuilt on a DPI change.
  HFONT chip_count_font_ = nullptr;
  HFONT chip_mosaic_font_ = nullptr;
  UINT chip_fonts_dpi_ = 0;
  // The rows as accessibility and tooltips read them.
  std::vector<PhiHit> hits_;

  // Space switch: the PAGE swaps instantly; the SIDEBAR BAND slides — and
  // natively, no macOS snapshot compositing: the leaving Space's laid-out
  // rows are RETAINED and painted live, shifted out at their frozen source
  // palette, while the entering rows paint shifted in at the ramping
  // palette.
  struct BandModel {
    std::vector<TabRow> tabs;
    std::vector<ShortcutRow> pinned;
    std::vector<ShortcutRow> bookmarks;
    std::vector<GroupHeader> headers;
  };
  BandModel leaving_band_;

  // Live only between ArmDrag and the commit or cancel (§3.5): its
  // destructor IS AbortDrag, and it owns the drag image and the split-drop
  // overlay, so nothing tears those down by hand.
  std::unique_ptr<PhiSidebarDragSession> drag_;

  // The strip's landing glide (BeginStripLanding). `landing_armed_key_` is
  // consumed by the next rebuild, which starts the animation on the row it
  // finds; `landing_key_` is the row in flight and `landing_dx_` how far
  // it still is from its slot.
  SidebarKey landing_armed_key_;
  int landing_drop_left_ = 0;
  ULONGLONG landing_armed_at_ = 0;
  SidebarKey landing_key_;
  double landing_from_ = 0.0;
  double landing_dx_ = 0.0;
  int landing_animation_ = 0;

  // The keyboard ring (BeginKeyboardFocus): on, the row it is on, and
  // whether the next hits publish announces it to UI Automation.
  bool kb_focus_ = false;
  SidebarKey kb_key_;
  bool kb_announce_ = false;
  bool kb_uia_published_ = false;
  // A row the next rebuild scrolls into view (the keyboard's), and the
  // active tab the stored bands last revealed a cell for.
  SidebarKey reveal_key_;
  TabId revealed_stored_tab_;
  // The overlay scrollbars' clock.
  ULONGLONG scrollbar_shown_at_ = 0;
  int scrollbar_animation_ = 0;
  // Drag auto-scroll: its animation, the pointer it reads, the strip's
  // last step, and the strip window's edges it scrolls at.
  int auto_scroll_animation_ = 0;
  POINT auto_scroll_pt_{};
  ULONGLONG auto_scroll_last_ = 0;
  int strip_window_left_ = 0;
  int strip_window_right_ = 0;
  // What the armed drag resolved against (DragContextChanged).
  std::string drag_space_;
  bool drag_horizontal_ = false;
  bool drag_band_hidden_ = false;
};

}  // namespace phi_client

#endif  // !defined(PHI_SIDEBAR_VIEW_MODEL_ONLY)

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_VIEW_H_

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SHELL_LAYOUT_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SHELL_LAYOUT_H_

#include <string>

#include "phibrowser-win/client/phi_sidebar_layout.h"

// The shell window's chrome geometry as a VALUE (docs/architecture-review.md
// §3.1 principle 6, lane 1 D4, lane 3 §3.2): a pure function of the client
// rect, the DPI, the layout mode and a handful of counts, over plain ints.
// No HWND, no DPI queries, no font creation, no host round-trips — so the
// controller can cache one per turn and the Linux host can test it
// (test/model/shell_layout_test.cc). The controller converts to RECT.

namespace phi_client {

// Where the browser's chrome lives. The Mac client's LayoutMode, same names
// and same meanings (PhiPreferences.swift), so a user moving between platforms
// gets the same three choices:
//
//   kPerformance  RETIRED on Windows (docs/windows-native-ui.md §4.3, impl
//                 log D9): the enum value stays so the Mac's stored setting
//                 still parses, but FromName answers kBalanced for it and
//                 nothing lays it out.
//   kBalanced     vertical tabs, address bar across the TOP of the web content
//   kComfortable  horizontal tabs — the traditional stacked layout, no sidebar
enum class PhiLayoutMode { kPerformance, kBalanced, kComfortable };

// Wire names, matching the Mac enum's raw values so the two clients can share
// a stored setting: "balanced", "comfortable" ("performance" reads as
// balanced).
const char* PhiLayoutModeName(PhiLayoutMode mode);
bool PhiLayoutModeFromName(const std::string& name, PhiLayoutMode* out);

// The design units the shell is laid out in (points at 100%), taken from
// the Mac client so the two look the same: WebContentConstant,
// SidebarHeaderView, SpacesStripView, PinnedTabLayout, TabStripMetrics and
// SidebarBottomBar. ONE definition, here, so the pure layout and the
// controller's paint code cannot drift apart.
namespace shell_metrics {
// The Windows-native geometry (docs/windows-native-ui.md §2.1). The OS
// caption is removed (WM_NCCALCSIZE); the vertical layout's ADDRESS ROW
// and the horizontal layout's STRIP ROW carry the minimize / maximize /
// close buttons at the window's top-right.
inline constexpr int kAddressRowHeight = 44;   // Vertical: the pane's first row.
inline constexpr int kStripRowHeight = 40;     // Horizontal: the tab strip.
inline constexpr int kToolbarRowHeight = 40;   // Horizontal: the toolbar.
inline constexpr int kCaptionButtonWidth = 46;  // The Win11 caption metric.
// THE horizontal gutter: every row applies 8 on each side itself.
inline constexpr int kEdgeInset = 8;
// The one toolbar button: 32 square, radius 4; 4 apart in a cluster.
inline constexpr int kNavButton = 32;
inline constexpr int kNavGroupSpacing = 4;
// The gap before the nav trio when the sidebar is collapsed, and between
// the nav trio and the omnibox.
inline constexpr int kGroupGap = 8;
// The tool cluster → the caption divider, and the divider's gap to the
// caption buttons.
inline constexpr int kBandTrailingGap = 16;
inline constexpr int kBandGroupGap = 8;
// The hairline standing in that gap: short enough to read as a seam between
// the app's controls and the window's, not as a border round either.
inline constexpr int kBandDividerHeight = 16;
// The omnibox: 32 high, radius 8, 24 dip in-field slots 4 from its ends.
inline constexpr int kOmniboxHeight = 32;
inline constexpr int kOmniboxRadius = 8;
inline constexpr int kFieldSlot = 24;
inline constexpr int kFieldSlotInset = 4;
inline constexpr int kFieldTextInset = 12;
// The Mac's minimumAddressTextWidth: below it the trailing stack hides.
inline constexpr int kFieldMinText = 84;
// The pinned-extension slots in the tool cluster: 28 wide, 2 apart.
inline constexpr int kExtSlot = 28;
inline constexpr int kExtSlotGap = 2;
// The Space pips: 28 in a 44 header row (vertical) / the strip row
// (horizontal), radius 6, 4 apart, the "+" trailing them.
inline constexpr int kSpaceChip = 28;
inline constexpr int kSpaceChipVertical = 28;  // The strip's "big pip" flag.
inline constexpr int kSpaceChipGap = 4;
inline constexpr int kSpacesStripPad = kEdgeInset;
inline constexpr int kSpacesStripHeight = kAddressRowHeight;  // The header row.
// The sidebar stack: header → pinned band spacer, pinned → list spacer.
inline constexpr int kHeaderSpacer = 8;
// The sidebar's section labels (Pinned / Bookmarks / Tabs): 12 dip text,
// 12 above, 4 below, 16 inset (§4.2).
inline constexpr int kSectionLabelHeight = 28;  // Only the agent's "<Agent>'s tabs" cue now.
inline constexpr int kSectionGap = 8;           // Between bands (no section labels, §4.2).
inline constexpr int kSectionLabelInset = 16;
inline constexpr int kFavoritesSpacer = 4;
inline constexpr int kTabRowHeight = 36;    // Row pitch adds kTabRowGap.
inline constexpr int kTabRowGap = 2;
inline constexpr int kBottomBarHeight = 40;
inline constexpr int kBottomSpacer = 0;
// Kept for the readers that still name them: no margin round the pane and
// no breath under the band — the pane is flush (§4.2).
inline constexpr int kContentMargin = 0;
inline constexpr int kContentTopGap = 0;
inline constexpr int kFaviconSlot = 16;  // The 16pt slot in the row.
// The separator row between the bookmark outline and the tab list.
inline constexpr int kBookmarkSeparatorHeight = 16;
inline constexpr int kNewTabRowHeight = 36;
// The empty pinned band's drop zone while a tab drag is in flight.
inline constexpr int kPinnedDropZoneHeight = 100;
// The stored bands' cap (the pinned grid and the bookmark outline share one
// scrolling viewport): they may take the room down to "+ New Tab" and this
// many tab rows under it, and never less than 2/5 of the room between the
// header and the bottom bar — so a long outline cannot push the tab list
// off the sidebar.
inline constexpr int kMinTabListRows = 4;
// Pinned tabs: 44 dip cells, 8 gaps and insets.
inline constexpr int kPinnedCellHeight = 44;
inline constexpr int kPinnedGap = 8;
inline constexpr int kBookmarkBarHeight = 28;
inline constexpr int kTopBarSpacing = kNavGroupSpacing;
// The horizontal tab strip: tabs are 34 tall over the strip's 6 top inset,
// the compact pip / Space chip leads them after the edge inset.
inline constexpr int kHTabStripTop = 6;
inline constexpr int kHTabStripLeading = 8;
// The bottom bar's Chat capsule (on the accent).
inline constexpr int kChatWidth = 58;
inline constexpr int kPillHeight = 24;
// The bottom bar's Feedback capsule (Figma Frame 48): 38 x kPillHeight,
// a 1 px ink-8 % outline round a 16 dip bug.
inline constexpr int kFeedbackPillWidth = 38;
// The Update reminder (the Mac's upgrade button): a 24 dip accent pill at
// least 56 wide, left of the tab-search button. The sidebar header drops it
// at or below 225 dip (SidebarHeaderView's tooNarrowForUpgrade).
inline constexpr int kUpdatePillMinWidth = 56;
inline constexpr int kUpdatePillMinSidebar = 225;
// Legacy names still read by the box rigs' arithmetic: the band height
// the layout verb reports. The vertical row is 44, the strip row 40.
inline constexpr int kTitleBarHeight = kAddressRowHeight;
inline constexpr int kTitleBarHeightTabs = kStripRowHeight;
}  // namespace shell_metrics

// Design units → physical pixels at `dpi` (96 = 100%). MulDiv's rounding.
int ShellScaled(int design_units, int dpi);

// Everything the layout depends on. Plain values, so the controller can
// compare one turn's inputs with the last and skip the recompute — the
// cache key (lane 3 §3.2: the layout used to be re-derived, with DPI
// queries and host round-trips, eight times per pointer move).
struct ShellLayoutInputs {
  PhiLayoutMode mode = PhiLayoutMode::kBalanced;
  int client_width = 0;
  int client_height = 0;
  int dpi = 96;
  int sidebar_dip = 240;  // Already clamped to the drag range.
  // Collapsed AND not being drawn for the floating panel: the content card
  // widens to the left margin and the sidebar's bands are empty.
  bool sidebar_hidden = false;
  // The hosted child owns every pixel; every chrome rect stays empty.
  bool content_fullscreen = false;
  bool bookmark_bar = false;      // The bar takes the content's first band.
  bool reader_offerable = false;  // The address capsule gets a reader slot.
  bool ai_enabled = false;        // The Chat / memory controls exist at all.
  int field_ext_count = 0;   // Pinned-extension icons inside the capsule.
  int pinned_row_count = 0;  // The pinned grid's rows (0 = no band).
  int bookmark_row_count = 0;  // Visible outline rows (0 = no separator).
  // A drag that could land in an EMPTY stored band is in flight: the band
  // reserves a drop zone (kPinnedDropZoneHeight / one outline row) so the
  // drag has somewhere to go. Ignored while the band has rows of its own.
  bool pinned_drop_zone = false;
  bool bookmark_drop_zone = false;
  // There is something to switch TO: more than one Space (agent pips
  // included), or a Space is being created. With a single Space the strip
  // row is pure noise (the Mac's updateSpaceSwitchVisibility): the vertical
  // layouts drop the Space row (the app header remains), comfortable
  // drops the compact pip.
  bool spaces_switch = true;
  // The floating panel keeps a Space row even with a single Space.
  bool floating_panel = false;
  // The agent Space's "<Agent>'s tabs" cue (§5.10) above the tab list;
  // the plain section labels are gone (§4.2).
  bool owned_tabs_label = false;
  // The horizontal layout's Space chip width in dip (§5.2: icon + name +
  // chevron, measured by the shell); 0 = the bare 28 dip pip.
  int space_chip_dip = 0;
  // §5.10 cue 3: the agent chip after the nav trio — the full chip's
  // width in the agent's own Space, 40 for the compact form in a user
  // Space while an agent works elsewhere, 0 for none.
  int agent_chip_dip = 0;
  // The Update reminder's measured width in dip (label + padding, at least
  // kUpdatePillMinWidth); 0 = no update on offer, no pill.
  int update_dip = 0;

  friend bool operator==(const ShellLayoutInputs& a,
                         const ShellLayoutInputs& b) {
    return a.mode == b.mode && a.client_width == b.client_width &&
           a.client_height == b.client_height && a.dpi == b.dpi &&
           a.sidebar_dip == b.sidebar_dip &&
           a.sidebar_hidden == b.sidebar_hidden &&
           a.content_fullscreen == b.content_fullscreen &&
           a.bookmark_bar == b.bookmark_bar &&
           a.reader_offerable == b.reader_offerable &&
           a.ai_enabled == b.ai_enabled &&
           a.field_ext_count == b.field_ext_count &&
           a.pinned_row_count == b.pinned_row_count &&
           a.bookmark_row_count == b.bookmark_row_count &&
           a.pinned_drop_zone == b.pinned_drop_zone &&
           a.bookmark_drop_zone == b.bookmark_drop_zone &&
           a.spaces_switch == b.spaces_switch &&
           a.floating_panel == b.floating_panel &&
           a.owned_tabs_label == b.owned_tabs_label &&
           a.space_chip_dip == b.space_chip_dip &&
           a.agent_chip_dip == b.agent_chip_dip &&
           a.update_dip == b.update_dip;
  }
  friend bool operator!=(const ShellLayoutInputs& a,
                         const ShellLayoutInputs& b) {
    return !(a == b);
  }
};

// Every rect the chrome needs, for one layout mode, in physical pixels.
struct ShellLayout {
  PhiLayoutMode mode = PhiLayoutMode::kBalanced;
  SidebarRect client;
  // The caption band: drag space, then the trailing control cluster and
  // the three caption buttons. The OS caption is gone (WM_NCCALCSIZE) —
  // this row IS the title bar.
  SidebarRect title_bar;
  SidebarRect caption_min, caption_max, caption_close;
  SidebarRect sidebar;  // Empty in Comfortable.
  // The address row (vertical: the pane's first row, from the sidebar's
  // edge to the window's; horizontal: the toolbar under the strip).
  SidebarRect top_bar;
  SidebarRect content;  // Where the reparented browser child goes.
  // The bookmark bar band, carved off the content's top when visible.
  SidebarRect bookmark_bar;
  SidebarRect app_icon;  // The vertical sidebar's persistent header mark.
  SidebarRect search_tabs;
  // The Update reminder, just before search (strip, sidebar header, or the
  // collapsed address row's lead); empty without an update on offer.
  SidebarRect update;
  // Search and the toggle trail the sidebar header. When collapsed they
  // lead the address row, keeping the sidebar one click away.
  SidebarRect sidebar_toggle;
  // The hairline closing the band's trailing cluster.
  SidebarRect band_divider;
  SidebarRect back, forward, reload;
  SidebarRect agent_chip;  // §5.10: after the nav trio; empty for none.
  SidebarRect spaces_strip;  // The band the Space chips live in.
  SidebarRect url_field;
  SidebarRect favorites;  // Pinned-tab band; empty without pins.
  // The bookmark outline's band — its rows (or the reserved drop zone)
  // and the separator row under them — empty without bookmarks. A drag
  // over any of it lands in the outline; below the last row means the
  // root's end.
  SidebarRect bookmarks;
  // The stored bands' VIEWPORT: the pinned grid and the bookmark outline
  // scroll together inside it once their natural height passes the cap
  // (kMinTabListRows). `favorites` and `bookmarks` above are their NATURAL,
  // unscrolled rects and may run past its bottom; the sidebar view scrolls
  // and clips them to this. Empty without pins or bookmarks.
  SidebarRect stored_band;
  // The 16pt separator row between the bookmark outline and "+ New Tab".
  SidebarRect bookmark_separator;
  SidebarRect new_tab_row;  // The sidebar's "+ New Tab" row.
  SidebarRect bottom_bar;
  // Bottom-bar buttons (§4.2): the profile (avatar) button, Downloads and
  // the memory chip leading, the Feedback pill (and the Chat capsule while
  // the AI suite is on) trailing. The address row has no Downloads button
  // of its own.
  // The Mac's ProfileButton: a 32 dip hit box like its neighbours, the 24
  // dip avatar disc drawn centred in it. Left-most, always present.
  SidebarRect sidebar_profile;
  SidebarRect sidebar_downloads;
  SidebarRect memory, feedback, chat;
  SidebarRect extensions;  // The puzzle button.
  // The app menu button (⋯, docs/windows-native-ui.md §3): comfortable's
  // toolbar trailing button; the vertical layout's band cluster.
  // Every layout has one — it is the only way to the menu.
  SidebarRect app_menu;
  // The omnibox's leading site-info slot (lock / page glyph).
  SidebarRect page_icon;
  // The in-field trailing slots, the Mac's stack: Reader View (empty
  // unless offered), Copy link and the site menu (CopyURLButtonView and the
  // address-bar menu button). All three show while the field is hovered,
  // Reader also while Reader View is on. No star — the Mac's address bar
  // has none; Ctrl+D and the app menu bookmark.
  SidebarRect reader;
  SidebarRect copy_link;
  SidebarRect site_menu;
  // The pinned-extension icon strip in the tool cluster; empty when nothing
  // is pinned or the icons would squeeze the omnibox text below 84 dip.
  SidebarRect field_ext_strip;
  SidebarRect add_space;  // The strip's trailing "+" chip.
  // The sidebar's section labels, each above its band; empty when the
  // band is (the Tabs label is always there in a docked sidebar).
  SidebarRect pinned_label, bookmarks_label, tabs_label;
  // The pip's edge in points (28 in both layouts).
  int chip_size = 0;
  int chips_left = 0, chips_top = 0;
  int chips_limit = 0;  // Right edge chips may not cross.
  int tabs_left = 0, tabs_top = 0;
  int tabs_limit = 0;  // Bottom (vertical) or right (horizontal) edge.
  bool horizontal_tabs = false;
  // The Spaces switch exists in this layout (see ShellLayoutInputs).
  bool spaces_switch = true;
};

ShellLayout ComputeShellLayout(const ShellLayoutInputs& in);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SHELL_LAYOUT_H_

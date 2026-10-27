// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_shell_layout.h"

#include <algorithm>

namespace phi_client {

using namespace shell_metrics;  // The metric names, unqualified.

const char* PhiLayoutModeName(PhiLayoutMode mode) {
  switch (mode) {
    case PhiLayoutMode::kBalanced:
      return "balanced";
    case PhiLayoutMode::kComfortable:
      return "comfortable";
    case PhiLayoutMode::kPerformance:
      break;
  }
  return "performance";
}

bool PhiLayoutModeFromName(const std::string& name, PhiLayoutMode* out) {
  if (name == "performance") {
    // Retired on Windows (impl log D9): a stored Performance setting lands
    // on the vertical layout.
    *out = PhiLayoutMode::kBalanced;
  } else if (name == "balanced") {
    *out = PhiLayoutMode::kBalanced;
  } else if (name == "comfortable") {
    *out = PhiLayoutMode::kComfortable;
  } else {
    return false;
  }
  return true;
}

int ShellScaled(int design_units, int dpi) {
  // MulDiv's rounding (to nearest, halves away from zero), so the host test
  // and the box agree on every odd DPI.
  const long long n = static_cast<long long>(design_units) * dpi;
  return static_cast<int>(n >= 0 ? (n + 48) / 96 : (n - 48) / 96);
}

ShellLayout ComputeShellLayout(const ShellLayoutInputs& in) {
  const auto scaled = [&](int design) { return ShellScaled(design, in.dpi); };
  ShellLayout out;
  // Performance is retired (impl log D9): a caller still holding the enum
  // value lays out the vertical layout.
  out.mode = in.mode == PhiLayoutMode::kComfortable ? PhiLayoutMode::kComfortable
                                                    : PhiLayoutMode::kBalanced;
  const bool horizontal = out.mode == PhiLayoutMode::kComfortable;
  out.chip_size = kSpaceChip;
  out.client = {0, 0, in.client_width, in.client_height};
  // Carried on EVERY path: the strip reads this flag to decide whether it
  // lays out a pip at all.
  out.spaces_switch = in.spaces_switch;
  if (in.content_fullscreen) {
    // Content fullscreen: the hosted child owns every pixel of the shell;
    // every chrome rect stays empty.
    out.content = out.client;
    return out;
  }
  const int w = in.client_width;
  const int h = in.client_height;
  const int sidebar = scaled(in.sidebar_dip);
  const bool hide_sidebar = in.sidebar_hidden;
  const int inset = scaled(kEdgeInset);
  const int btn = scaled(kNavButton);
  const int btn_gap = scaled(kNavGroupSpacing);
  const int cap = scaled(kCaptionButtonWidth);

  // The caption row: the vertical layout's address row (44) or the
  // horizontal layout's strip row (40). Its trailing end holds minimize /
  // maximize / close, square, at the window's top-right corner.
  const int tb = scaled(horizontal ? kStripRowHeight : kAddressRowHeight);
  out.title_bar = {0, 0, w, tb};
  out.caption_close = {w - cap, 0, w, tb};
  out.caption_max = {w - 2 * cap, 0, w - cap, tb};
  out.caption_min = {w - 3 * cap, 0, w - 2 * cap, tb};

  // A 32 dip control centred on `mid`, its right edge at `right`.
  const auto control_r = [&](int right, int mid) {
    return SidebarRect{right - btn, mid - btn / 2, right, mid + btn / 2};
  };
  const auto control_l = [&](int left, int mid) {
    return SidebarRect{left, mid - btn / 2, left + btn, mid + btn / 2};
  };
  // The seam between the row's last control and the caption buttons,
  // centred in kBandTrailingGap on the control's own line.
  const auto place_band_divider = [&](const SidebarRect& last) {
    const int mid = (last.top + last.bottom) / 2;
    const int half = scaled(kBandDividerHeight) / 2;
    const int x = (last.right + out.caption_min.left) / 2;
    out.band_divider = {x, mid - half, x + scaled(1), mid + half};
  };
  // The tool cluster, built right-to-left from `right` on `mid`: ⋯ app
  // menu · Extensions · the pinned-extension slots. No Downloads button
  // (Jixiang, 2026-09-20): downloads live in the sidebar's bottom bar, the
  // app menu and Ctrl+J. Returns the cluster's left edge.
  const auto place_cluster = [&](int right, int mid, bool with_ext_strip) {
    out.app_menu = control_r(right, mid);
    out.extensions = control_r(out.app_menu.left - btn_gap, mid);
    int left = out.extensions.left;
    if (with_ext_strip && in.field_ext_count > 0) {
      const int slot = scaled(kExtSlot);
      const int strip_w = in.field_ext_count * slot +
                          (in.field_ext_count - 1) * scaled(kExtSlotGap);
      out.field_ext_strip = {left - btn_gap - strip_w, mid - slot / 2,
                             left - btn_gap, mid + slot / 2};
      left = out.field_ext_strip.left;
    }
    return left;
  };
  // The omnibox between `left` and `right` on `mid`, with its in-field
  // slots: the site-info slot leading; Reader (when offered), Copy link and
  // the site menu trailing, in the Mac's order.
  const auto place_omnibox = [&](int left, int right, int mid) {
    const int field_h = scaled(kOmniboxHeight);
    out.url_field = {left, mid - field_h / 2, right, mid + field_h / 2};
    const int slot = scaled(kFieldSlot);
    const int slot_inset = scaled(kFieldSlotInset);
    out.page_icon = {left + slot_inset, mid - slot / 2, left + slot_inset + slot,
                     mid + slot / 2};
    int r = right - slot_inset;
    out.site_menu = {r - slot, mid - slot / 2, r, mid + slot / 2};
    r = out.site_menu.left - scaled(2);
    out.copy_link = {r - slot, mid - slot / 2, r, mid + slot / 2};
    r = out.copy_link.left - scaled(2);
    if (in.reader_offerable) {
      out.reader = {r - slot, mid - slot / 2, r, mid + slot / 2};
    }
  };
  // The Update reminder's pill, `in.update_dip` wide from `left`, centred
  // on `mid`.
  const auto update_l = [&](int left, int mid) {
    const int pill_h = scaled(kPillHeight);
    return SidebarRect{left, mid - pill_h / 2, left + scaled(in.update_dip),
                       mid + pill_h / 2};
  };
  // The Space pips in a header band: 28 dip, 4 apart, from the band's
  // edge inset, the "+" trailing them.
  const auto place_spaces = [&](int left, int right, int top, int height) {
    const int pip = scaled(out.chip_size);
    out.spaces_strip = {left, top, right, top + height};
    const int chip_top = top + (height - pip) / 2;
    out.chips_left = left + scaled(kSpacesStripPad);
    out.chips_top = chip_top;
    int trailing = right - scaled(kSpacesStripPad);
    if (!in.spaces_switch) {
      // The panel's row with a single Space: no pips (the tucked edge
      // docks the sidebar; the panel carries no pin button).
      out.chips_limit = out.chips_left;
      return;
    }
    out.add_space = {trailing - pip, chip_top, trailing, chip_top + pip};
    out.chips_limit = out.add_space.left - scaled(kSpaceChipGap);
  };
  // The bookmark bar takes the content's first band when visible.
  const auto carve_bookmark_bar = [&](ShellLayout* l) {
    if (!in.bookmark_bar) {
      return;
    }
    const int bar_h = scaled(kBookmarkBarHeight);
    l->bookmark_bar = {l->content.left, l->content.top, l->content.right,
                       l->content.top + bar_h};
    l->content.top += bar_h;
  };

  if (horizontal) {
    // Chrome's stack (docs/windows-native-ui.md §4.1): the tab strip on the
    // tint shares the caption row with the window controls; the toolbar
    // runs the full width under it on the card ground; the page goes edge
    // to edge. The strip's controls sit on the STRIP's centre line: the
    // strip starts kHTabStripTop below the top edge and runs to the row's
    // bottom, where the active tab merges into the toolbar.
    const int strip_mid = (scaled(kHTabStripTop) + tb) / 2;
    // The Space chip leads the strip (§5.2: icon + name + chevron, 28 high,
    // its width measured by the shell), or the bare pip until measured.
    out.chips_left = inset;
    out.chips_top = strip_mid - scaled(kSpaceChip) / 2;
    const int chip_w = scaled(std::max(kSpaceChip, in.space_chip_dip));
    out.chips_limit = in.spaces_switch ? out.chips_left + chip_w
                                       : out.chips_left;
    // Tab search trails the strip, one gap before the divider and the
    // caption buttons.
    out.search_tabs = control_r(out.caption_min.left - scaled(kBandTrailingGap),
                                strip_mid);
    place_band_divider(out.search_tabs);
    // The Update reminder leads the strip's trailing controls (the Mac's
    // TabStripRightButtons order), and the tabs stop before it.
    int strip_right = out.search_tabs.left;
    if (in.update_dip > 0) {
      out.update = update_l(strip_right - btn_gap - scaled(in.update_dip),
                            strip_mid);
      strip_right = out.update.left;
    }
    // The "+" follows the last tab; the strip is full when the next tab
    // would need the room the button is holding.
    const int new_tab_gap = scaled(kTopBarSpacing);
    out.tabs_left = (in.spaces_switch ? out.chips_limit : inset) +
                    scaled(kHTabStripLeading);
    out.tabs_top = scaled(kHTabStripTop);
    out.tabs_limit = strip_right - new_tab_gap - btn - new_tab_gap;
    out.horizontal_tabs = true;

    // The toolbar: nav trio leading, the omnibox filling the middle, the
    // tool cluster trailing.
    const int toolbar_h = scaled(kToolbarRowHeight);
    out.top_bar = {0, tb, w, tb + toolbar_h};
    const int mid = tb + toolbar_h / 2;
    out.back = control_l(inset, mid);
    out.forward = control_l(out.back.right + btn_gap, mid);
    out.reload = control_l(out.forward.right + btn_gap, mid);
    int field_left = out.reload.right + scaled(kGroupGap);
    if (in.agent_chip_dip > 0) {
      // The agent chip (§5.10): 28 high, after the trio, before the field.
      const int chip_h = scaled(28);
      out.agent_chip = {field_left, mid - chip_h / 2,
                        field_left + scaled(in.agent_chip_dip),
                        mid + chip_h / 2};
      field_left = out.agent_chip.right + scaled(kGroupGap);
    }
    const int cluster_left = place_cluster(w - inset, mid, true);
    place_omnibox(field_left, cluster_left - scaled(kGroupGap), mid);
    out.content = {0, out.top_bar.bottom, w, h};
    carve_bookmark_bar(&out);
    return out;
  }

  // The vertical layout (§4.2): the sidebar stands on the tint with its
  // own header row; the page pane runs flush to the window's top, right
  // and bottom and carries the address row with the window buttons.
  const int pane_left = hide_sidebar ? 0 : sidebar;
  out.sidebar = {0, 0, sidebar, h};
  out.top_bar = {pane_left, 0, w, tb};
  out.content = {pane_left, tb, w, h};
  carve_bookmark_bar(&out);
  const int mid = tb / 2;
  if (hide_sidebar) {
    // Keep both controls reachable while the sidebar is tucked away.
    out.sidebar_toggle = control_l(inset, mid);
    out.search_tabs = control_l(out.sidebar_toggle.right + btn_gap, mid);
    int nav_left = out.search_tabs.right + scaled(kGroupGap);
    if (in.update_dip > 0) {
      out.update = update_l(out.search_tabs.right + btn_gap, mid);
      nav_left = out.update.right + scaled(kGroupGap);
    }
    out.back = control_l(nav_left, mid);
  } else {
    out.app_icon = control_l(inset, mid);
    out.sidebar_toggle = control_r(sidebar - inset, mid);
    out.search_tabs = control_r(out.sidebar_toggle.left - btn_gap, mid);
    // The header's Update reminder, before search — unless the sidebar is
    // too narrow for it beside the app mark.
    if (in.update_dip > 0 && in.sidebar_dip > kUpdatePillMinSidebar) {
      const SidebarRect pill = update_l(
          out.search_tabs.left - btn_gap - scaled(in.update_dip), mid);
      if (pill.left >= out.app_icon.right + btn_gap) {
        out.update = pill;
      }
    }
    out.back = control_l(pane_left + inset, mid);
  }
  out.forward = control_l(out.back.right + btn_gap, mid);
  out.reload = control_l(out.forward.right + btn_gap, mid);
  // Trailing: the divider before the caption buttons, then the cluster.
  int cluster_right = out.caption_min.left - scaled(kBandTrailingGap);
  {
    // The divider centres between the cluster's last control and the
    // caption buttons: place the cluster, then the seam.
    const int cluster_left = place_cluster(cluster_right, mid, true);
    place_band_divider(out.app_menu);
    // The omnibox fills what is left, minus the reserved drag gutter —
    // after the agent chip (§5.10) when one is up.
    int field_left = out.reload.right + scaled(kGroupGap);
    if (in.agent_chip_dip > 0) {
      const int chip_h = scaled(28);
      out.agent_chip = {field_left, mid - chip_h / 2,
                        field_left + scaled(in.agent_chip_dip),
                        mid + chip_h / 2};
      field_left = out.agent_chip.right + scaled(kGroupGap);
    }
    // The field runs up to the cluster at the ordinary group gap: no
    // reserved drag run (Jixiang, 2026-09-20) — the row's controls take
    // their clicks and the window drags from the sidebar's pip row.
    int field_right = cluster_left - scaled(kGroupGap);
    // Below the minimum text width the pinned-extension slots hide first
    // (the Mac's minimumAddressTextWidth rule), then the field simply gets
    // narrow.
    const int min_field = scaled(kFieldTextInset + kFieldMinText + 3 * kFieldSlot +
                                 2 * 2 + 2 * kFieldSlotInset + 8);
    if (field_right - field_left < min_field && !out.field_ext_strip.empty()) {
      out.field_ext_strip = SidebarRect{};
      field_right = out.extensions.left - scaled(kGroupGap);
    }
    if (field_right < field_left + scaled(kFieldSlot)) {
      field_right = field_left + scaled(kFieldSlot);
    }
    place_omnibox(field_left, field_right, mid);
  }

  // The persistent app header sits above the optional Space strip, then
  // pins, bookmarks, new tab, tabs and the bottom bar.
  const bool header_row = in.spaces_switch || in.floating_panel;
  if (!hide_sidebar && header_row) {
    place_spaces(0, sidebar, tb, tb);
  }
  // No section labels (§4.2): the bands follow each other 8 dip apart;
  // only the agent Space's "<Agent>'s tabs" cue keeps a label row.
  const int label_h = scaled(kSectionLabelHeight);
  const int label_inset = scaled(kSectionLabelInset);
  const int gap = scaled(kSectionGap);
  const int sidebar_header_bottom = tb + (header_row ? tb : 0);
  // The bottom bar's top, up front: the stored bands' cap is a share of the
  // room between the header and it.
  const int bar_h = scaled(kBottomBarHeight);
  const int bar_top = h - scaled(kBottomSpacer) - bar_h;
  int y = sidebar_header_bottom;
  const int pinned_rows = in.pinned_row_count;
  const int pinned_height =
      pinned_rows > 0 ? 2 * scaled(kPinnedGap) +
                            pinned_rows * scaled(kPinnedCellHeight) +
                            (pinned_rows - 1) * scaled(kPinnedGap)
      : in.pinned_drop_zone ? scaled(kPinnedDropZoneHeight)
                            : 0;
  out.pinned_label = SidebarRect{};
  out.favorites = {0, y, sidebar, y + pinned_height};
  y = out.favorites.bottom;
  const int bookmark_rows =
      in.bookmark_row_count > 0 ? in.bookmark_row_count
      : in.bookmark_drop_zone   ? 1
                                : 0;
  const int bookmarks_height = bookmark_rows * scaled(kTabRowHeight + kTabRowGap);
  out.bookmarks_label = SidebarRect{};
  if (bookmark_rows > 0 && pinned_height > 0) {
    y += gap;
  }
  const int bookmarks_top = y;
  // The outline's trailing separator row is gone: the Tabs label parts
  // the outline from the tab list.
  const int separator_h = 0;
  const int separator_top = bookmarks_top + bookmarks_height;
  out.bookmark_separator = SidebarRect{};
  out.bookmarks =
      bookmark_rows > 0 ? SidebarRect{inset, bookmarks_top, sidebar - inset,
                                      separator_top + separator_h}
                        : SidebarRect{};
  y = separator_top + separator_h;
  // The cap: the two stored bands scroll inside one viewport once they
  // would take the tab list's minimum room ("+ New Tab" and
  // kMinTabListRows rows) — but never shrink below 2/5 of the sidebar's
  // room. The rects above stay natural; the view scrolls them.
  out.stored_band = SidebarRect{};
  if (y > sidebar_header_bottom) {
    const int room = bar_top - scaled(kFavoritesSpacer) - sidebar_header_bottom;
    const int list_floor = gap + (in.owned_tabs_label ? label_h : 0) +
                           scaled(kNewTabRowHeight) +
                           kMinTabListRows * scaled(kTabRowHeight + kTabRowGap);
    const int stored_cap =
        std::max(0, std::max(room * 2 / 5, room - list_floor));
    const int visible = std::min(y - sidebar_header_bottom, stored_cap);
    out.stored_band = {0, sidebar_header_bottom, sidebar,
                       sidebar_header_bottom + visible};
    y = out.stored_band.bottom;
  }
  if (y > sidebar_header_bottom) {
    y += gap;
  }
  if (in.owned_tabs_label) {
    out.tabs_label = {label_inset, y, sidebar - label_inset, y + label_h};
    y += label_h;
  } else {
    out.tabs_label = SidebarRect{};
  }
  out.new_tab_row = {inset, y, sidebar - inset, y + scaled(kNewTabRowHeight)};
  out.tabs_top = out.new_tab_row.bottom + scaled(kTabRowGap);

  // The bottom bar: the profile button, downloads (and the memory chip
  // while the AI suite is on) leading, the Feedback pill trailing.
  out.bottom_bar = {0, bar_top, sidebar, bar_top + bar_h};
  const int bar_mid = bar_top + bar_h / 2;
  int bx = inset;
  // The profile (avatar) button leads, as the Mac's SidebarBottomBar does
  // (ProfileButton at x 8, then its 4 dip gap): the same 32 dip hit box
  // as its neighbours, the 24 dip disc centred in it by the painter.
  out.sidebar_profile = {bx, bar_mid - btn / 2, bx + btn, bar_mid + btn / 2};
  bx += btn + btn_gap;
  // Downloads next, as the board draws it — its own slot, beside the tool
  // cluster's copy in the address row — then the memory chip.
  out.sidebar_downloads = {bx, bar_mid - btn / 2, bx + btn, bar_mid + btn / 2};
  bx += btn + btn_gap;
  if (in.ai_enabled) {
    out.memory = {bx, bar_mid - btn / 2, bx + btn, bar_mid + btn / 2};
    bx += btn + btn_gap;
  }
  int br = sidebar - inset;
  if (in.ai_enabled) {
    const int chat_top = bar_mid - scaled(kPillHeight) / 2;
    out.chat = {br - scaled(kChatWidth), chat_top, br,
                chat_top + scaled(kPillHeight)};
    br = out.chat.left - btn_gap;
  }
  // Feedback trails as an outlined capsule (Figma Frame 48).
  const int feedback_top = bar_mid - scaled(kPillHeight) / 2;
  out.feedback = {br - scaled(kFeedbackPillWidth), feedback_top, br,
                  feedback_top + scaled(kPillHeight)};

  out.tabs_left = inset;
  out.tabs_limit = out.bottom_bar.top - scaled(kFavoritesSpacer);
  if (hide_sidebar) {
    const SidebarRect zero = {};
    out.favorites = zero;
    out.bookmarks = zero;
    out.stored_band = zero;
    out.bookmark_separator = zero;
    out.pinned_label = zero;
    out.bookmarks_label = zero;
    out.tabs_label = zero;
    out.new_tab_row = zero;
    out.bottom_bar = zero;
    out.sidebar_profile = zero;
    out.sidebar_downloads = zero;
    out.memory = zero;
    out.feedback = zero;
    out.chat = zero;
    out.tabs_limit = out.tabs_top;  // No tab rows either.
  }
  return out;
}

}  // namespace phi_client

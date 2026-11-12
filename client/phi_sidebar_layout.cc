// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "phi_sidebar_layout.h"

#include <algorithm>

namespace phi_client {

int PinnedGridColumnLimit(int sidebar_dip) {
  const int decision = sidebar_dip - 8;
  return decision < 236 ? 3 : decision < 294 ? 4 : decision < 352 ? 5 : 6;
}

int PinnedGridColumns(const PinnedGridMetrics& m, int total_units) {
  const int avail = m.sidebar_dip - 2 * m.edge_inset_dip;
  int columns =
      std::max(1, std::min(PinnedGridColumnLimit(m.sidebar_dip), total_units));
  while (columns > 1 &&
         (avail - (columns - 1) * m.gap_dip) / columns < m.min_cell_width_dip) {
    --columns;
  }
  return columns;
}

PinnedGridPlacement LayoutPinnedGrid(const std::vector<int>& units,
                                     const PinnedGridMetrics& m) {
  PinnedGridPlacement out;
  int total_units = 0;
  for (const int u : units) total_units += std::max(1, u);
  out.columns = PinnedGridColumns(m, total_units);
  const int row_span =
      m.sidebar_px - 2 * m.edge_inset_px - (out.columns - 1) * m.gap_px;
  out.cells.reserve(units.size());
  int col = 0;
  int row_index = -1;
  for (const int u : units) {
    const int cu = std::min(std::max(1, u), out.columns);
    if (row_index < 0 || col + cu > out.columns) {
      ++row_index;
      col = 0;
    }
    const int y = m.origin_y + m.gap_px + row_index * (m.cell_height_px + m.gap_px);
    out.cells.push_back(
        {m.origin_x + col * m.gap_px + col * row_span / out.columns, y,
         m.origin_x + (col + cu - 1) * m.gap_px +
             (col + cu) * row_span / out.columns,
         y + m.cell_height_px});
    col += cu;
  }
  out.rows = units.empty() ? 0 : row_index + 1;
  return out;
}

ListPlacement LayoutList(const std::vector<ListItem>& items,
                         const ListMetrics& m) {
  ListPlacement out;
  out.rects.reserve(items.size());
  int y = m.top - m.scroll;
  for (const ListItem& item : items) {
    if (item.hidden) {
      out.rects.push_back({});
      out.unclipped.push_back({});
      continue;
    }
    const int h = item.kind == ListItem::Kind::kHeader ? m.header_height
                                                       : m.row_height;
    SidebarRect rect = {m.left, y, m.right, y + h};
    out.unclipped.push_back(rect);
    y += h + item.pad_below + m.row_gap;
    out.content_height += h + item.pad_below + m.row_gap;
    if (m.clip &&
        (rect.bottom <= m.viewport_top || rect.top >= m.viewport_bottom)) {
      rect = {};  // Scrolled out of the viewport.
    }
    out.rects.push_back(rect);
  }
  out.bottom = y;
  if (m.clip) {
    const int viewport = m.viewport_bottom - m.viewport_top;
    out.scroll_max = std::max(0, out.content_height - viewport);
  }
  return out;
}

StripWidths SolveStripWidths(int resting_count,
                             bool has_active,
                             int room,
                             int ideal_rest,
                             int ideal_active,
                             int floor) {
  StripWidths w{ideal_rest, has_active ? ideal_active : 0};
  if (resting_count * w.rest + w.active > room) {
    if (resting_count > 0) {
      w.rest = std::max(floor, (room - w.active) / resting_count);
    }
    if (has_active && resting_count * w.rest + w.active > room) {
      w.active = std::max(floor, room - resting_count * w.rest);
    }
  }
  return w;
}

StripWindow SolveStripWindow(const std::vector<StripSpan>& items,
                             int left,
                             int limit,
                             int chevron_room,
                             int first,
                             int reveal) {
  const int count = static_cast<int>(items.size());
  // The index past the last item that fits from `from` inside `edge`.
  const auto fit_end = [&](int from, int edge) {
    int x = left;
    int i = from;
    for (; i < count; ++i) {
      const StripSpan& item = items[static_cast<size_t>(i)];
      const int right = x + item.lead + item.width;
      if (right > edge) {
        break;
      }
      x = right + item.trail;
    }
    return i;
  };
  StripWindow out;
  out.overflow = fit_end(0, limit) < count;
  out.limit = out.overflow ? limit - chevron_room : limit;
  if (!out.overflow) {
    out.end = count;
    return out;
  }
  out.first_max = std::max(0, count - 1);
  while (out.first_max > 0 && fit_end(out.first_max - 1, out.limit) == count) {
    --out.first_max;
  }
  out.first = std::max(0, std::min(first, out.first_max));
  if (reveal >= 0 && reveal < count) {
    if (reveal < out.first) {
      out.first = reveal;
    }
    while (out.first < reveal && fit_end(out.first, out.limit) <= reveal) {
      ++out.first;
    }
  }
  out.end = fit_end(out.first, out.limit);
  return out;
}

int SolveStripChipWidth(int label_px,
                        int count_px,
                        bool collapsed,
                        bool user_titled,
                        const StripChipMetrics& m) {
  int width = m.leading + m.dot + m.dot_to_label + label_px + m.label_slack +
              m.label_right_pad;
  if (collapsed) {
    // The mosaic always reserves its room when collapsed, titled or not: it
    // is the preview of what the chip hides.
    width += m.count_to_label + m.mosaic;
  } else if (user_titled) {
    width += m.count_to_label + count_px + 2 * m.count_h_pad;
  }
  width += m.collapse_gap + m.collapse_control;
  return std::min(width, m.max_width);
}

StripChipPlacement PlaceStripChip(const SidebarRect& chip,
                                  int count_px,
                                  int count_height_px,
                                  bool collapsed,
                                  bool user_titled,
                                  const StripChipMetrics& m) {
  StripChipPlacement out;
  const int mid = (chip.top + chip.bottom) / 2;
  out.dot = {chip.left + m.leading, mid - m.dot / 2,
             chip.left + m.leading + m.dot, mid - m.dot / 2 + m.dot};
  const int collapse_left =
      chip.right - m.label_right_pad - m.collapse_control;
  out.collapse = {collapse_left, mid - m.collapse_control / 2,
                  collapse_left + m.collapse_control,
                  mid - m.collapse_control / 2 + m.collapse_control};
  const int trailing = collapse_left - m.collapse_gap;
  int label_right = trailing;
  if (collapsed) {
    out.mosaic = {trailing - m.mosaic, mid - m.mosaic / 2, trailing,
                  mid - m.mosaic / 2 + m.mosaic};
    label_right = out.mosaic.left - m.count_to_label;
  } else if (user_titled) {
    const int w = count_px + 2 * m.count_h_pad;
    const int h = count_height_px + 2 * m.count_v_pad;
    out.count = {trailing - w, mid - h / 2, trailing, mid - h / 2 + h};
    label_right = out.count.left - m.count_to_label;
  }
  const int label_left = chip.left + m.leading + m.dot + m.dot_to_label;
  out.label = {label_left, chip.top, std::max(label_left, label_right),
               chip.bottom};
  return out;
}

std::vector<SidebarRect> StripMosaicCells(const SidebarRect& mosaic,
                                          int cell,
                                          int gap,
                                          int margin) {
  std::vector<SidebarRect> cells;
  for (int i = 0; i < 4; ++i) {
    const int left = mosaic.left + margin + (i % 2) * (cell + gap);
    const int top = mosaic.top + margin + (i / 2) * (cell + gap);
    cells.push_back({left, top, left + cell, top + cell});
  }
  return cells;
}

std::vector<StripMosaicSlot> StripMosaicFill(int member_count) {
  std::vector<StripMosaicSlot> slots(4);
  if (member_count <= 0) {
    return slots;
  }
  if (member_count <= 4) {
    for (int i = 0; i < member_count; ++i) {
      slots[static_cast<size_t>(i)] = {StripMosaicCell::kFavicon, i, 0};
    }
    return slots;
  }
  for (int i = 0; i < 3; ++i) {
    slots[static_cast<size_t>(i)] = {StripMosaicCell::kFavicon, i, 0};
  }
  slots[3] = {StripMosaicCell::kOverflow, -1, member_count - 3};
  return slots;
}

}  // namespace phi_client

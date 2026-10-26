// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_sidebar_drag_session.h"

#include "phibrowser-win/client/phi_drop_badge.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "phibrowser-win/client/phi_app_model.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_new_tab_page.h"
#include "phibrowser-win/client/phi_sidebar_view.h"
#include "phibrowser-win/client/phi_spaces_strip.h"
#include "phibrowser-win/client/phi_shell_window.h"

namespace phi_client {

namespace {

using phi_framework::SplitSnapshot;
using phi_framework::TabSnapshot;
using phi_framework::WindowSnapshot;

using sidebar_metrics::BookmarkIndentDepth;
using sidebar_metrics::kBookmarkIndent;
using sidebar_metrics::kCornerRadius;
using sidebar_metrics::kHTabGap;
using sidebar_metrics::kRowPad;
using shell_metrics::kTabRowGap;
using shell_metrics::kTabRowHeight;

}  // namespace

PhiSidebarDragSession::PhiSidebarDragSession(PhiSidebarView* view,
                                             const SidebarKey& key,
                                             POINT pt)
    : view_(view),
      shell_(view->shell()),
      framework_(view->framework()),
      spaces_host_(view->spaces_host()),
      window_host_(view->window_host()),
      slot_(view->slot()),
      incognito_(view->incognito()) {
  key_ = key;
  // Which section holds a stored key is the view's to answer; the rest of
  // the mapping is the pure rule.
  kind_ = PhiDragKindForKey(key,
                            PhiIndexOfKey(view_->pinned(), key) >= 0);
  // Read BEFORE the caller acts on the press: every arming site activates the
  // row it grabbed (a tab) or opens it (a pinned tab, a bookmark), so this is
  // the last moment the page the user is actually looking at is still the
  // active one. A split drop pairs the dragged row with THAT page.
  focus_tab_id_ = view_->active_tab_id();
  origin_ = pt;
  point_ = pt;
}

// THE teardown of a drag: whatever is still in flight is undone here, so a
// sidebar (or a shell) going away mid-drag cannot leave the drag image or
// the split-drop cards on screen (docs/architecture-review.md §3.5).
PhiSidebarDragSession::~PhiSidebarDragSession() {
  AbortDrag();
}

int PhiSidebarDragSession::Scaled(int design_units) const {
  return shell_->SidebarScaled(design_units);
}

HWND PhiSidebarDragSession::shell_hwnd() const {
  return shell_->SidebarShellWindow();
}

HWND PhiSidebarDragSession::frame_hwnd() const {
  return shell_->SidebarOwner()->shell_hwnd();
}

PhiSpacesStrip* PhiSidebarDragSession::strip() const {
  return shell_->SidebarSpacesStrip();
}

// The current leg of the slide, for the shell's timer: it repaints until
// the leg settles and then kills the clock.
bool PhiSidebarDragSession::SlideRunning() const {
  return drag_slide_start_ != 0 &&
         ::GetTickCount64() - drag_slide_start_ < kSlideMs;
}

// The dragged element's place in the kind's own vector, re-resolved from
// the KEY every time — the rows rebuild under a drag and an index would not
// survive that.
int PhiSidebarDragSession::Index() const {
  switch (kind_) {
    case PhiDragKind::kTab:
      return PhiIndexOfKey(view_->tabs(), key_);
    case PhiDragKind::kPin:
      return PhiIndexOfKey(view_->pinned(), key_);
    case PhiDragKind::kBookmark:
      return PhiIndexOfKey(view_->bookmarks(), key_);
    case PhiDragKind::kChip:
      for (size_t i = 0; i < strip()->chips().size(); ++i) {
        if (strip()->chips()[i].id == key_.id) {
          return static_cast<int>(i);
        }
      }
      return -1;
    case PhiDragKind::kNone:
      break;
  }
  return -1;
}

PhiSlideVec PhiSidebarDragSession::RowSlideOffset(
    const std::string& guid) const {
  const auto to = row_slide_to_.find(guid);
  if (to == row_slide_to_.end()) {
    return SlideVec{};
  }
  SlideVec from;
  if (const auto f = row_slide_from_.find(guid);
      f != row_slide_from_.end()) {
    from = f->second;
  }
  if (!drag_slide_start_) {
    return to->second;
  }
  const ULONGLONG elapsed = ::GetTickCount64() - drag_slide_start_;
  return SlideVec{
      PhiDragSlideEase(from.dx, to->second.dx, elapsed, kSlideMs),
      PhiDragSlideEase(from.dy, to->second.dy, elapsed, kSlideMs),
      PhiDragSlideEase(from.dw, to->second.dw, elapsed, kSlideMs),
      PhiDragSlideEase(from.dh, to->second.dh, elapsed, kSlideMs)};
}

double PhiSidebarDragSession::DragSlideOffset(TabId tab_id) const {
  const auto to = drag_slide_to_.find(tab_id);
  if (to == drag_slide_to_.end()) {
    return 0.0;
  }
  double from = 0.0;
  if (const auto f = drag_slide_from_.find(tab_id);
      f != drag_slide_from_.end()) {
    from = f->second;
  }
  if (!drag_slide_start_) {
    return to->second;
  }
  return PhiDragSlideEase(from, to->second,
                          ::GetTickCount64() - drag_slide_start_, kSlideMs);
}

namespace {

// A grid cell's leg: from where it sits to the previewed cell — offset AND
// size, since the grid re-solves widths whenever a unit enters or leaves a
// row (a lone pin spans the sidebar; two split it).
PhiSlideVec GridSlide(const RECT& from, const RECT& to) {
  return PhiSlideVec{static_cast<double>(to.left - from.left),
                     static_cast<double>(to.top - from.top),
                     static_cast<double>((to.right - to.left) -
                                         (from.right - from.left)),
                     static_cast<double>((to.bottom - to.top) -
                                         (from.bottom - from.top))};
}

}  // namespace

void PhiSidebarDragSession::RetargetDragSlide() {
  // The new leg starts wherever every element currently is (mid-flight
  // retargeting, as Chromium's animating layout manager does).
  std::map<TabId, double> from;
  for (const auto& [id, target] : drag_slide_to_) {
    from[id] = DragSlideOffset(id);
  }
  std::map<std::string, SlideVec> row_from;
  for (const auto& [guid, target] : row_slide_to_) {
    row_from[guid] = RowSlideOffset(guid);
  }
  std::map<TabId, double> to;
  std::map<std::string, SlideVec> row_to;
  const PhiSidebarRects layout = shell_->SidebarRects();
  const std::vector<ShortcutRow>& pins = view_->pinned();
  const int pitch = Scaled(kTabRowHeight + kTabRowGap);
  int preview_rows = -1;  // The grid preview's rows, when one was walked.

  if (active_ && layout.horizontal_tabs) {
    RetargetStripSlide(&to, &row_to);
  }
  if (active_ && !layout.horizontal_tabs && insert_ >= 0) {
    // The visible tab rows, and the dragged one's position among them (-1:
    // what is dragged is not a tab row — a pinned cell or a bookmark
    // entering the list from a stored band).
    std::vector<const TabRow*> visible;
    int dragged_pos = -1;
    for (const TabRow& row : view_->tabs()) {
      if (row.bounds.right <= row.bounds.left) {
        continue;
      }
      if (kind_ == PhiDragKind::kTab &&
          static_cast<int>(&row - view_->tabs().data()) == Index()) {
        dragged_pos = static_cast<int>(visible.size());
      }
      visible.push_back(&row);
    }
    // A foreign row entering the list: the rows at and past the slot make
    // room for it, one pitch each.
    const auto open_list_gap = [&]() {
      for (int i = std::max(0, insert_); i < static_cast<int>(visible.size());
           ++i) {
        to[visible[static_cast<size_t>(i)]->id] = pitch;
      }
    };
    // The group headers and their cards ride the member rows: the header
    // sits above whichever comes first of its first remaining member and —
    // when the drop lands in the group — the gap's phantom slot; the card's
    // bottom sits under the later of its last remaining member and that
    // phantom. So a gap opening inside the group (under the header, between
    // members, or as its last slot) grows the card around the gliding rows
    // instead of sliding them out under its edge, and a member dragged out
    // shrinks it. Runs after `to` holds the tab rows' slides.
    const auto slide_headers = [&]() {
      const auto final_top = [&](int pos) {
        const auto it = to.find(visible[static_cast<size_t>(pos)]->id);
        return static_cast<double>(
                   visible[static_cast<size_t>(pos)]->bounds.top) +
               (it == to.end() ? 0.0 : it->second);
      };
      // Where the gap's phantom slot sits once the rows have glided: the
      // rows before the slot slid up only when the dragged row came from
      // above them (then the phantom sits one pitch above the row at the
      // slot — the dragged row's own slot when the two coincide); otherwise
      // the row at the slot slid down and the phantom takes its old place.
      const bool gap_in_list = band_ == PhiDropBand::kTabs && insert_ >= 0;
      const bool from_above = dragged_pos >= 0 && insert_ > dragged_pos;
      double phantom_top = 0.0;
      if (gap_in_list && !visible.empty()) {
        const int size = static_cast<int>(visible.size());
        if (insert_ < size) {
          phantom_top = visible[static_cast<size_t>(insert_)]->bounds.top -
                        (from_above ? pitch : 0);
        } else {
          phantom_top = visible[static_cast<size_t>(size - 1)]->bounds.top +
                        (from_above ? 0 : pitch);
        }
      }
      for (const GroupHeader& header : view_->group_headers()) {
        const bool gap_in_group = gap_in_list && gap_group_ == header.id;
        int first = -1, last = -1, first_kept = -1, last_kept = -1;
        for (int i = 0; i < static_cast<int>(visible.size()); ++i) {
          if (visible[static_cast<size_t>(i)]->group_id != header.id) {
            continue;
          }
          if (first < 0) {
            first = i;
          }
          last = i;
          if (i != dragged_pos) {
            if (first_kept < 0) {
              first_kept = i;
            }
            last_kept = i;
          }
        }
        double top_dy = 0.0;
        double bottom_dy = 0.0;
        if (first < 0) {
          // Collapsed (or every member scrolled out): the header is a row
          // of its own, riding the row that follows it — above the gap
          // when the drop lands in the collapsed group.
          int next = -1;
          for (int i = 0; i < static_cast<int>(visible.size()); ++i) {
            if (visible[static_cast<size_t>(i)]->bounds.top >
                header.bounds.top) {
              next = i;
              break;
            }
          }
          if (next >= 0) {
            double below = final_top(next);
            if (gap_in_group) {
              below = std::min(below, phantom_top);
            }
            top_dy = below - visible[static_cast<size_t>(next)]->bounds.top;
          } else if (!visible.empty()) {
            const int tail = static_cast<int>(visible.size()) - 1;
            top_dy = final_top(tail) -
                     visible[static_cast<size_t>(tail)]->bounds.top;
          }
        } else {
          // The element under the header, and the one closing the card.
          double below = first_kept >= 0 ? final_top(first_kept) : phantom_top;
          double closing = last_kept >= 0 ? final_top(last_kept) : phantom_top;
          if (gap_in_group) {
            below = std::min(below, phantom_top);
            closing = std::max(closing, phantom_top);
          } else if (first_kept < 0) {
            // Its only member is being dragged out: the card is the header.
            below = visible[static_cast<size_t>(first)]->bounds.top;
            closing = below - pitch;
          }
          top_dy = below - visible[static_cast<size_t>(first)]->bounds.top;
          bottom_dy = closing - visible[static_cast<size_t>(last)]->bounds.top;
        }
        const SlideVec v{0.0, top_dy, 0.0, bottom_dy - top_dy};
        if (v.moved()) {
          row_to[SidebarKey::Group(header.id).ToString()] = v;
        }
      }
    };
    if (kind_ == PhiDragKind::kTab && Index() >= 0 &&
        Index() < static_cast<int>(view_->tabs().size())) {
      // The dragged row vacates its list slot in EVERY band; where the gap
      // reopens depends on the target under the cursor.
      if (dragged_pos >= 0) {
        for (int i = 0; i < static_cast<int>(visible.size()); ++i) {
          if (i == dragged_pos) {
            continue;
          }
          double dy = 0.0;
          if (band_ == PhiDropBand::kTabs) {
            const int to_pos = insert_ > dragged_pos ? insert_ - 1
                                                          : insert_;
            if (i > dragged_pos && i <= to_pos) {
              dy = -pitch;
            } else if (i >= to_pos && i < dragged_pos) {
              dy = pitch;
            }
          } else {
            // Leaving the list: later rows close the vacated slot. (The
            // outline gap a bookmark conversion opens pushes the whole band
            // below it — the band shift, below.)
            if (i > dragged_pos) {
              dy = -pitch;
            }
          }
          if (dy != 0.0) {
            to[visible[static_cast<size_t>(i)]->id] = dy;
          }
        }
        slide_headers();
      }
      if (band_ == PhiDropBand::kPins && !view_->pinned().empty()) {
        // A phantom one-slot cell opens the grid gap at the insertion
        // slot; every displaced cell glides to its previewed position.
        std::vector<int> units;
        for (const ShortcutRow& pin : view_->pinned()) {
          units.push_back(pin.url_b.empty() ? 1 : 2);
        }
        const int at =
            std::min(insert_, static_cast<int>(units.size()));
        std::vector<int> preview = units;
        preview.insert(preview.begin() + at, 1);
        const std::vector<RECT> target = view_->PinnedGridWalk(preview, layout, &preview_rows);
        for (int i = 0; i < static_cast<int>(view_->pinned().size()); ++i) {
          const size_t j = static_cast<size_t>(i < at ? i : i + 1);
          const SlideVec v = GridSlide(pins[i].frame, target[j]);
          if (v.moved()) {
            row_to[view_->pinned()[static_cast<size_t>(i)].guid] = v;
          }
        }
      } else if (band_ == PhiDropBand::kBookmarks) {
        for (int i = 0; i < static_cast<int>(view_->bookmarks().size()); ++i) {
          if (i >= insert_) {
            row_to[view_->bookmarks()[static_cast<size_t>(i)].guid] =
                SlideVec{0.0, static_cast<double>(pitch)};
          }
        }
      }
    } else if (kind_ == PhiDragKind::kPin &&
               band_ != PhiDropBand::kPins && Index() >= 0 &&
               Index() < static_cast<int>(view_->pinned().size())) {
      // The cell has left the grid (unpinning into the list, or becoming a
      // bookmark): the grid closes its slot, and over the outline the
      // bookmark rows part at the target — unless the drop goes INTO a
      // collapsed folder, which highlights instead of opening a gap.
      std::vector<int> units;
      for (int i = 0; i < static_cast<int>(view_->pinned().size()); ++i) {
        if (i != Index()) {
          units.push_back(
              view_->pinned()[static_cast<size_t>(i)].url_b.empty() ? 1 : 2);
        }
      }
      const std::vector<RECT> target = view_->PinnedGridWalk(units, layout, &preview_rows);
      for (int i = 0; i < static_cast<int>(view_->pinned().size()); ++i) {
        if (i == Index()) {
          continue;
        }
        const size_t j = static_cast<size_t>(i > Index() ? i - 1 : i);
        if (j >= target.size()) {
          continue;
        }
        const SlideVec v = GridSlide(pins[i].frame, target[j]);
        if (v.moved()) {
          row_to[view_->pinned()[static_cast<size_t>(i)].guid] = v;
        }
      }
      if (band_ == PhiDropBand::kBookmarks &&
          bookmark_drop_folder_row_ < 0) {
        for (int i = 0; i < static_cast<int>(view_->bookmarks().size()); ++i) {
          if (i >= insert_) {
            row_to[view_->bookmarks()[static_cast<size_t>(i)].guid] =
                SlideVec{0.0, static_cast<double>(pitch)};
          }
        }
      } else if (band_ == PhiDropBand::kTabs) {
        // Unpinning into the list: the rows part at the slot for it.
        open_list_gap();
        slide_headers();
      }
    } else if (kind_ == PhiDragKind::kBookmark &&
               band_ == PhiDropBand::kPins && Index() >= 0 &&
               Index() < static_cast<int>(view_->bookmarks().size())) {
      // A bookmark over the grid (the Mac's moveBookmarkOut(toPinnedTabs:)):
      // a phantom cell of the width it will take opens the grid slot, and
      // the outline closes the row it is leaving.
      const ShortcutRow& dragged =
          view_->bookmarks()[static_cast<size_t>(Index())];
      std::vector<int> units;
      for (const ShortcutRow& pin : view_->pinned()) {
        units.push_back(pin.url_b.empty() ? 1 : 2);
      }
      const int at = std::max(
          0, std::min(insert_, static_cast<int>(units.size())));
      std::vector<int> preview = units;
      preview.insert(preview.begin() + at, dragged.url_b.empty() ? 1 : 2);
      const std::vector<RECT> target = view_->PinnedGridWalk(preview, layout, &preview_rows);
      for (int i = 0; i < static_cast<int>(view_->pinned().size()); ++i) {
        const size_t j = static_cast<size_t>(i < at ? i : i + 1);
        if (j >= target.size()) {
          continue;
        }
        const SlideVec v = GridSlide(pins[i].frame, target[j]);
        if (v.moved()) {
          row_to[view_->pinned()[static_cast<size_t>(i)].guid] = v;
        }
      }
      for (int i = Index() + 1; i < static_cast<int>(view_->bookmarks().size());
           ++i) {
        row_to[view_->bookmarks()[static_cast<size_t>(i)].guid] =
            SlideVec{0.0, static_cast<double>(-pitch)};
      }
    } else if (kind_ == PhiDragKind::kPin &&
               band_ == PhiDropBand::kPins && Index() >= 0 &&
               Index() < static_cast<int>(view_->pinned().size())) {
      // Grid reorder: the dragged cell floats; a phantom of its own width
      // holds the target slot open.
      std::vector<int> units;
      for (int i = 0; i < static_cast<int>(view_->pinned().size()); ++i) {
        if (i == Index()) {
          continue;
        }
        units.push_back(
            view_->pinned()[static_cast<size_t>(i)].url_b.empty() ? 1 : 2);
      }
      const int dragged_units =
          view_->pinned()[static_cast<size_t>(Index())].url_b.empty() ? 1 : 2;
      int to_pos = insert_ > Index() ? insert_ - 1
                                              : insert_;
      to_pos = std::max(0, std::min(to_pos, static_cast<int>(units.size())));
      std::vector<int> preview = units;
      preview.insert(preview.begin() + to_pos, dragged_units);
      const std::vector<RECT> target = view_->PinnedGridWalk(preview, layout, &preview_rows);
      for (int i = 0; i < static_cast<int>(view_->pinned().size()); ++i) {
        if (i == Index()) {
          continue;
        }
        const int j = i > Index() ? i - 1 : i;
        const size_t jj = static_cast<size_t>(j >= to_pos ? j + 1 : j);
        const SlideVec v = GridSlide(pins[i].frame, target[jj]);
        if (v.moved()) {
          row_to[view_->pinned()[static_cast<size_t>(i)].guid] = v;
        }
      }
    } else if (kind_ == PhiDragKind::kBookmark &&
               band_ == PhiDropBand::kTabs && Index() >= 0 &&
               Index() < static_cast<int>(view_->bookmarks().size())) {
      // A bookmark over the list (the Mac's moveBookmarkOut(toTabList:)):
      // the rows part at the slot for the tab it becomes, and the outline
      // closes the row it is leaving.
      open_list_gap();
      slide_headers();
      for (int i = Index() + 1; i < static_cast<int>(view_->bookmarks().size());
           ++i) {
        row_to[view_->bookmarks()[static_cast<size_t>(i)].guid] =
            SlideVec{0.0, static_cast<double>(-pitch)};
      }
    } else if (kind_ == PhiDragKind::kBookmark &&
               band_ == PhiDropBand::kBookmarks && Index() >= 0 &&
               Index() < static_cast<int>(view_->bookmarks().size()) &&
               bookmark_drop_folder_row_ < 0) {
      // A drop INTO a collapsed folder opens no gap — the folder row
      // highlights instead and every displaced row settles home.
      const int to_pos = insert_ > Index() ? insert_ - 1
                                                    : insert_;
      for (int i = 0; i < static_cast<int>(view_->bookmarks().size()); ++i) {
        if (i == Index()) {
          continue;
        }
        double dy = 0.0;
        if (i > Index() && i <= to_pos) {
          dy = -pitch;
        } else if (i >= to_pos && i < Index()) {
          dy = pitch;
        }
        if (dy != 0.0) {
          row_to[view_->bookmarks()[static_cast<size_t>(i)].guid] =
              SlideVec{0.0, dy};
        }
      }
    }
  }
  // A grid preview with more rows than the real band (a phantom that
  // wrapped) — or fewer (a pin leaving) — moves everything under the band:
  // the outline's rows in their own slides, the rest by the band shift.
  grid_extra_ = 0;
  if (preview_rows >= 0 && !pins.empty()) {
    const int dy = (preview_rows - view_->pinned_grid_rows()) *
                   Scaled(shell_metrics::kPinnedCellHeight +
                          shell_metrics::kPinnedGap);
    if (dy != 0) {
      grid_extra_ = std::max(0, dy);
      for (const ShortcutRow& row : view_->bookmarks()) {
        row_to[row.guid].dy += dy;
      }
      row_to[kBandShiftKey].dy += dy;
    }
  }
  // A gap opening in the outline pushes everything under it down by a row:
  // "+ New Tab", the group headers and the tab rows all ride the band shift.
  // (An EMPTY outline's reserved drop zone IS the gap: nothing shifts.)
  if (active_ && !layout.horizontal_tabs && insert_ >= 0 &&
      band_ == PhiDropBand::kBookmarks && bookmark_drop_folder_row_ < 0 &&
      !view_->bookmarks().empty() &&
      (kind_ == PhiDragKind::kTab || kind_ == PhiDragKind::kPin)) {
    row_to[kBandShiftKey].dy += pitch;
  }
  // The reverse: a bookmark leaving the outline for the list or the grid
  // pulls everything under the band up by the row it vacates — unless it
  // is the last one, when the band stays as the reserved drop zone.
  if (active_ && !layout.horizontal_tabs && insert_ >= 0 &&
      kind_ == PhiDragKind::kBookmark &&
      (band_ == PhiDropBand::kTabs || band_ == PhiDropBand::kPins) &&
      view_->bookmarks().size() > 1) {
    row_to[kBandShiftKey].dy -= pitch;
  }
  // Displaced elements whose displacement ended slide home; settled ones
  // drop out of the books entirely.
  for (const auto& [id, start] : from) {
    if (!to.count(id) && start != 0.0) {
      to[id] = 0.0;
    }
  }
  for (const auto& [id, target] : to) {
    if (!from.count(id)) {
      from[id] = 0.0;
    }
  }
  for (const auto& [guid, start] : row_from) {
    if (!row_to.count(guid) && start.moved()) {
      row_to[guid] = SlideVec{};
    }
  }
  for (const auto& [guid, target] : row_to) {
    if (!row_from.count(guid)) {
      row_from[guid] = SlideVec{};
    }
  }
  drag_slide_from_ = std::move(from);
  drag_slide_to_ = std::move(to);
  row_slide_from_ = std::move(row_from);
  row_slide_to_ = std::move(row_to);
  drag_slide_start_ = ::GetTickCount64();
  if (shell_hwnd() &&
      (!drag_slide_to_.empty() || !row_slide_to_.empty())) {
    ::SetTimer(shell_hwnd(), kSlideTimer, 16, nullptr);
  }
}

void PhiSidebarDragSession::RetargetStripSlide(
    std::map<TabId, double>* to,
    std::map<std::string, SlideVec>* row_to) {
  if (kind_ != PhiDragKind::kTab) {
    return;  // Only a tab reorders the strip; a pin keeps its line.
  }
  // The strip's placed items in order — chips and tabs, the dragged tab
  // left out — each with the id its slide is filed under.
  const std::vector<DropRow> rows = view_->TabDropRows();
  struct Item {
    bool chip = false;
    std::string group_id;
    TabId id;
    SidebarRect bounds;
  };
  std::vector<Item> items;
  int dragged_row = -1;
  int dragged_pos = -1;
  SidebarRect dragged_bounds;
  for (size_t i = 0; i < rows.size(); ++i) {
    const DropRow& row = rows[i];
    if (row.bounds.empty()) {
      continue;
    }
    if (row.key == key_) {
      dragged_row = static_cast<int>(i);
      dragged_pos = static_cast<int>(items.size());
      dragged_bounds = row.bounds;
      continue;
    }
    Item item;
    item.chip = row.group_header;
    item.group_id = row.group_id;
    if (!row.group_header) {
      for (const TabRow& tab : view_->tabs()) {
        if (tab.key == row.key) {
          item.id = tab.id;
          break;
        }
      }
    }
    item.bounds = row.bounds;
    items.push_back(std::move(item));
  }
  const int count = static_cast<int>(items.size());
  if (dragged_row < 0 || count == 0) {
    return;
  }
  // The gap is the dragged tab's own slot moved: while the drop is on the
  // strip it opens before the anchor row, and the items between there and
  // the vacated slot glide one pitch toward it; off the strip (the page,
  // another window, outside) the vacated slot closes — the items past it
  // glide up, as the vertical list's rows do.
  const int width = dragged_bounds.right - dragged_bounds.left;
  const double pitch = width + Scaled(kHTabGap);
  int gap_pos = count;
  if (band_ == PhiDropBand::kTabs && strip_anchor_ >= 0) {
    gap_pos = 0;
    for (int i = 0; i < static_cast<int>(rows.size()) && i < strip_anchor_;
         ++i) {
      if (!rows[static_cast<size_t>(i)].bounds.empty() && i != dragged_row) {
        ++gap_pos;
      }
    }
  }
  std::vector<double> dx(static_cast<size_t>(count), 0.0);
  for (int i = 0; i < count; ++i) {
    if (i >= dragged_pos && i < gap_pos) {
      dx[static_cast<size_t>(i)] = -pitch;
    } else if (i >= gap_pos && i < dragged_pos) {
      dx[static_cast<size_t>(i)] = pitch;
    }
  }
  // The gap's own rect once everything has glided — the slot a group's
  // underline stretches over when the drop joins that group (the Mac's
  // groupGeometries extending the run past its last member).
  double phantom_right = 0.0;
  if (gap_pos < count) {
    phantom_right = items[static_cast<size_t>(gap_pos)].bounds.left +
                    dx[static_cast<size_t>(gap_pos)];
  } else {
    phantom_right = items[static_cast<size_t>(count - 1)].bounds.right +
                    dx[static_cast<size_t>(count - 1)] + Scaled(kHTabGap) +
                    width;
  }
  for (int i = 0; i < count; ++i) {
    const Item& item = items[static_cast<size_t>(i)];
    const double shift = dx[static_cast<size_t>(i)];
    if (!item.chip) {
      if (shift != 0.0 && item.id.valid()) {
        (*to)[item.id] = shift;
      }
      continue;
    }
    // The chip glides with its slot; its run's right edge follows the
    // later of its remaining members' trailing edges and, when the drop
    // lands in the group, the gap — the underline grows around the
    // gliding tabs instead of sliding out from under them.
    double run_right = item.bounds.right + shift;
    for (int j = 0; j < count; ++j) {
      const Item& member = items[static_cast<size_t>(j)];
      if (!member.chip && member.group_id == item.group_id) {
        run_right = std::max(
            run_right, member.bounds.right + dx[static_cast<size_t>(j)]);
      }
    }
    if (band_ == PhiDropBand::kTabs && gap_group_ == item.group_id) {
      run_right = std::max(run_right, phantom_right);
    }
    double container_right = item.bounds.right;
    for (const GroupHeader& header : view_->group_headers()) {
      if (header.id == item.group_id &&
          header.container.right > header.container.left) {
        container_right = header.container.right;
        break;
      }
    }
    const SlideVec v{shift, 0.0, run_right - (container_right + shift), 0.0};
    if (v.moved()) {
      (*row_to)[SidebarKey::Group(item.group_id).ToString()] = v;
    }
  }
}

PhiDropBand PhiSidebarDragSession::DropBandAt(
    POINT client_pt) {
  // Outside the shell window entirely = tear-off (the Mac's pure
  // boundary hit-test — no distance threshold).
  POINT screen = client_pt;
  ::ClientToScreen(shell_hwnd(), &screen);
  RECT window_rect = {};
  ::GetWindowRect(frame_hwnd(), &window_rect);
  if (!::PtInRect(&window_rect, screen)) {
    return PhiDropBand::kOutside;
  }
  // The stored bands where they sit on screen — scrolled inside their
  // capped viewport — not their natural rects.
  const PhiSidebarRects layout =
      view_->ScrolledStoredBands(shell_->SidebarRects());
  // The PAGE first, and before the incognito and horizontal-layout exits
  // below: a split drop is offered in every layout and off the record too
  // (it forms no stored row, so there is nothing to keep).
  if (::PtInRect(&layout.content, client_pt)) {
    // §4.8: a 24 dip cancel band along the sidebar's edge and under the
    // address row, so a drag that slips just past the sidebar never
    // tears off — a release there is a cancel.
    const int band = shell_->SidebarScaled(24);
    if ((layout.content.left > 0 && client_pt.x < layout.content.left + band) ||
        client_pt.y < layout.content.top + band) {
      return PhiDropBand::kNone;
    }
    return PhiDropBand::kContent;
  }
  if (incognito_) {
    return PhiDropBand::kTabs;  // No pin/bookmark conversions off the record.
  }
  if (layout.horizontal_tabs) {
    return PhiDropBand::kTabs;  // The strip has no sidebar bands.
  }
  if (InPinnedBand(layout, client_pt)) {
    return PhiDropBand::kPins;
  }
  // The outline's whole band — its rows (or the reserved drop zone) AND the
  // separator row under them, so a pointer below the last row still lands
  // in the outline (at the root's end), as the Mac's bookmarkSectionEnd
  // row does, instead of falling through to the tab list.
  if (client_pt.x < layout.sidebar.right &&
      client_pt.y >= layout.bookmarks.top &&
      client_pt.y < layout.bookmarks.bottom) {
    return PhiDropBand::kBookmarks;
  }
  return PhiDropBand::kTabs;
}

bool PhiSidebarDragSession::InPinnedBand(const PhiSidebarRects& layout,
                                         POINT client_pt) const {
  if (::PtInRect(&layout.favorites, client_pt)) {
    return true;
  }
  // While the preview holds an extra row under the band, the pointer may
  // follow the phantom onto it without the band flipping to the outline.
  return band_ == PhiDropBand::kPins && grid_extra_ > 0 &&
         client_pt.x >= layout.favorites.left &&
         client_pt.x < layout.favorites.right &&
         client_pt.y >= layout.favorites.bottom &&
         client_pt.y < layout.favorites.bottom + grid_extra_;
}

void PhiSidebarDragSession::ReserveDropZones() {
  const PhiSidebarRects layout = shell_->SidebarRects();
  if (incognito_ || layout.horizontal_tabs ||
      view_->stored_bands_isolated()) {
    return;  // No stored bands to land in.
  }
  bool pinned = false;
  bool bookmark = false;
  switch (kind_) {
    case PhiDragKind::kTab:
      pinned = view_->pinned().empty();
      bookmark = view_->bookmarks().empty();
      break;
    case PhiDragKind::kPin:
      bookmark = view_->bookmarks().empty();
      break;
    case PhiDragKind::kBookmark: {
      // A folder cannot become a pin (the Mac's emptyView refuses it).
      const bool folder =
          Index() >= 0 &&
          Index() < static_cast<int>(view_->bookmarks().size()) &&
          view_->bookmarks()[static_cast<size_t>(Index())].folder;
      pinned = !folder && view_->pinned().empty();
      break;
    }
    case PhiDragKind::kChip:
    case PhiDragKind::kNone:
      break;
  }
  if (!pinned && !bookmark) {
    return;
  }
  zones_reserved_ = true;
  shell_->SidebarSetDropZones(pinned, bookmark);
  shell_->SidebarScheduleRefresh();
}

void PhiSidebarDragSession::ReleaseDropZones() {
  if (!zones_reserved_) {
    return;
  }
  zones_reserved_ = false;
  shell_->SidebarSetDropZones(false, false);
  shell_->SidebarScheduleRefresh();
}

void PhiSidebarDragSession::Update(POINT pt) {
  if (kind_ == PhiDragKind::kNone) {
    return;
  }
  if (!active_) {
    if (!PhiDragPassedThreshold(origin_.x, origin_.y, pt.x, pt.y,
                                ::GetSystemMetrics(SM_CXDRAG),
                                ::GetSystemMetrics(SM_CYDRAG))) {
      return;
    }
    active_ = true;
    ::SetCapture(shell_hwnd());
    BeginDragImage(pt);  // The element lifts off into its own window.
    ReserveDropZones();  // After the lift: the rows move under the zone.
  }
  // Every drag can cross bands (conversions, tear-off, a split over the
  // page): a bookmark leaves its outline for the grid (the Mac's
  // moveBookmarkOut(toPinnedTabs:)) or the tab list (moveBookmarkOut(
  // toNormalTabs:)) — a FOLDER cannot become a pin or a tab, so it stays
  // in the outline wherever it hovers inside the sidebar.
  point_ = pt;
  PhiDropBand band = DropBandAt(pt);
  if (kind_ == PhiDragKind::kBookmark) {
    const bool folder =
        Index() >= 0 && Index() < static_cast<int>(view_->bookmarks().size()) &&
        view_->bookmarks()[static_cast<size_t>(Index())].folder;
    if (folder &&
        (band == PhiDropBand::kPins || band == PhiDropBand::kTabs)) {
      band = PhiDropBand::kBookmarks;
    }
  } else if (kind_ != PhiDragKind::kTab && kind_ != PhiDragKind::kPin) {
    band = PhiDropBand::kTabs;
  }
  // §4.8 (2): a tab row over a Space pip targets that Space; the list
  // closes its gap meanwhile.
  std::string pip_target;
  std::wstring pip_label;
  if (kind_ == PhiDragKind::kTab && strip()) {
    const int chip = strip()->ChipAt(pt);
    if (chip >= 0 && chip < static_cast<int>(strip()->chips().size())) {
      const PhiSpaceChip& target = strip()->chips()[static_cast<size_t>(chip)];
      // An off-the-record tab only ever drops onto an Incognito Space's
      // pip: a regular one would re-open it in the regular profile.
      const PhiShellWindow* owner = shell_->SidebarOwner();
      const bool otr_into_regular = owner && owner->ShowsIncognito() &&
                                    !model::IsIncognitoSpaceId(target.id);
      if (!target.is_agent && !otr_into_regular &&
          target.id != shell_->SidebarActiveSpace()) {
        pip_target = target.id;
        pip_label = target.label;
      }
    }
  }
  if (!pip_target.empty()) {
    band = PhiDropBand::kNone;
  }
  const bool pip_changed = pip_target != pip_target_;
  pip_target_ = pip_target;
  pip_target_label_ = pip_label;
  const bool band_changed = band != band_ || pip_changed;
  band_ = band;
  // The image first: crossing out of the sidebar swaps the row for the page
  // card, which captures the page BEFORE the hint cards go up over it.
  SyncDragImage(pt);
  if (band == PhiDropBand::kContent) {
    CancelHoverExpand();
    // The page has no insertion slot: the drop is described by the split
    // zone under the cursor, and the hint cards are the whole feedback.
    ResolveSplitDrop(pt, /*rebuild=*/band_changed);
    if (insert_ != -1 || band_changed) {
      insert_ = -1;
      RetargetDragSlide();  // The row leaves the list while it hovers here.
      if (shell_hwnd()) {
        ::InvalidateRect(shell_hwnd(), nullptr, FALSE);
      }
    }
    UpdateBadge(pt);
    return;
  }
  PhiSplitDropOverlay::Hide();
  split_drop_valid_ = false;
  split_drop_zone_ = PhiSplitDropZone::kNone;
  if (!pip_target_.empty()) {
    if (insert_ != -1 || band_changed) {
      insert_ = -1;
      RetargetDragSlide();
      if (shell_hwnd()) {
        ::InvalidateRect(shell_hwnd(), nullptr, FALSE);
      }
    }
    UpdateBadge(pt);
    return;
  }
  if (band == PhiDropBand::kNone) {
    // §4.8's cancel band (DropBandAt): nothing is resolved here — no place,
    // no gap, no badge. The list closes the gap behind the row, as it does
    // while the row hovers the page, and a release is a cancel (Commit).
    CancelHoverExpand();
    place_valid_ = false;
    folder_key_ = {};
    bookmark_drop_folder_row_ = -1;
    strip_anchor_ = -1;
    gap_group_.clear();
    if (insert_ != -1 || band_changed) {
      insert_ = -1;
      RetargetDragSlide();
      if (shell_hwnd()) {
        ::InvalidateRect(shell_hwnd(), nullptr, FALSE);
      }
    }
    UpdateBadge(pt);  // No text for the band: the badge hides.
    return;
  }
  // The pure resolvers describe the drop as a SidebarPlace and the gap slot
  // the animation opens; a change between INTO-a-folder and a gap repaints
  // even at the same slot.
  const int folder_before = bookmark_drop_folder_row_;
  const int anchor_before = strip_anchor_;
  const int slot = DragInsertSlot(pt);
  const bool folder_target_changed = folder_before != bookmark_drop_folder_row_;
  // The strip's gap sits before a chip or a tab: the same tab slot can be
  // either side of a chip, and the chip glides differently for the two.
  const bool anchor_changed = strip_anchor_ != anchor_before;
  // The same gap slot is "under the header" or "before the group" by which
  // side of the header's midpoint the pointer is on; the group headers
  // slide differently for the two, so crossing it retargets as a slot
  // change does.
  const std::string gap_group =
      band == PhiDropBand::kTabs && place_valid_ ? place_.group_id
                                                  : std::string();
  const bool gap_group_changed = gap_group != gap_group_;
  gap_group_ = gap_group;
  SyncHoverExpand(pt);
  const bool animated_kind = kind_ == PhiDragKind::kTab ||
                             kind_ == PhiDragKind::kPin ||
                             kind_ == PhiDragKind::kBookmark;
  if (slot != insert_ || band_changed || folder_target_changed ||
      gap_group_changed || anchor_changed) {
    insert_ = slot;
    if (animated_kind) {
      RetargetDragSlide();  // The gap moved: siblings glide to the new slot.
    }
    if (shell_hwnd()) {
      ::InvalidateRect(shell_hwnd(), nullptr, FALSE);
    }
  }
  // A plain move repaints nothing: the dragged element rides the drag image
  // window, and the chrome only changes when the gap does.
  UpdateBadge(pt);
}

void PhiSidebarDragSession::UpdateBadge(POINT pt) {
  if (!active_ || !shell_hwnd()) {
    PhiDropBadge::Hide();
    return;
  }
  POINT screen = pt;
  ::ClientToScreen(shell_hwnd(), &screen);
  PhiShellWindow* owner = shell_->SidebarOwner();
  PhiDropBadge::Show(shell_hwnd(), shell_->SidebarDpi(), screen, BadgeText(),
                     /*plus=*/false,
                     owner ? owner->CurrentPalette()
                           : PhiResolvePalette(PhiThemeById("pure"),
                                               PhiAppearance::kSystem));
}

std::wstring PhiSidebarDragSession::BadgeText() const {
  if (!pip_target_.empty()) {
    return L"Move to " + pip_target_label_;
  }
  switch (band_) {
    case PhiDropBand::kNone:
      return std::wstring();
    case PhiDropBand::kContent:
      switch (split_drop_zone_) {
        case PhiSplitDropZone::kNewWindow:
          return L"New window";
        case PhiSplitDropZone::kLeft:
          return split_drop_replace_
                     ? (split_drop_stacked_ ? L"Replace top" : L"Replace left")
                     : L"Split left";
        case PhiSplitDropZone::kRight:
          return split_drop_replace_ ? (split_drop_stacked_ ? L"Replace bottom"
                                                            : L"Replace right")
                                     : L"Split right";
        case PhiSplitDropZone::kTop:
          return L"Split top";
        case PhiSplitDropZone::kBottom:
          return L"Split bottom";
        case PhiSplitDropZone::kNone:
          return std::wstring();
      }
      return std::wstring();
    case PhiDropBand::kOutside: {
      POINT screen = point_;
      ::ClientToScreen(shell_hwnd(), &screen);
      const bool other_window =
          window_host_ && window_host_->SlotAtScreenPoint(screen, slot_.value);
      return other_window ? L"Move to window" : L"New window";
    }
    case PhiDropBand::kTabs:
      return kind_ == PhiDragKind::kPin        ? L"Unpin"
             : kind_ == PhiDragKind::kBookmark ? L"Open as tab"
                                               : L"Move";
    case PhiDropBand::kPins:
      return kind_ == PhiDragKind::kPin ? L"Move" : L"Pin";
    case PhiDropBand::kBookmarks:
      return kind_ == PhiDragKind::kBookmark ? L"Move" : L"Bookmark";
  }
  return std::wstring();
}

void PhiSidebarDragSession::CancelWithSlideBack() {
  image_inside_ = true;  // Home is inside: the ghost slides back.
  AbortDrag();           // Cancels, ends the image, marks finished_.
}

void PhiSidebarDragSession::Commit() {
  // Read before the drop clears them: whether the image slides home or just
  // goes depends on where the pointer was and whether anything happened.
  const bool active = active_;
  // A release in the cancel band is a cancel: the ghost slides home even
  // though it hangs over the page as the card there.
  const bool inside = image_inside_ ||
                      (band_ == PhiDropBand::kNone && pip_target_.empty());
  finished_ = true;  // The destructor that follows aborts nothing.
  const bool acted = CommitDragImpl();
  if (active) {
    EndDragImage(acted, inside);
  }
}

bool PhiSidebarDragSession::CommitDragImpl() {
  const PhiDragKind kind = kind_;
  const PhiDropBand band = band_;
  const SidebarKey key = key_;
  const int slot = insert_;
  const bool active = active_;
  const POINT release = point_;
  const SidebarPlace place = place_;
  const bool place_valid = place_valid_;
  // What a drop on the page would do (CancelDrag clears these too).
  const PhiSplitDropZone split_zone = split_drop_zone_;
  const bool split_replace = split_drop_replace_;
  const std::string split_id = split_drop_split_id_;
  const TabId split_focus = focus_tab_id_;
  const TabId click_tab = click_tab_id_;
  const int chip_pos = Index();
  const std::string pip_target = pip_target_;
  // A tab dropped on the strip lands from where it was released: the view
  // glides it into its new slot once the rows have rebuilt (the Mac's
  // clearDraggingPresentation snapping the source view to the proxy, then
  // performLayout(.dataChanged) animating it home).
  const bool strip_drop = active && OnStrip() && band == PhiDropBand::kTabs;
  const int drop_left = strip_drop ? StripProxyRect(release).left : 0;
  CancelDrag();
  if (active && !pip_target.empty() && kind == PhiDragKind::kTab) {
    // §4.8 (2): dropped on a Space pip — the tab moves to that Space.
    view_->Apply(SidebarMoveToSpace{key, pip_target, /*clone=*/false});
    return true;
  }
  if (!active && kind == PhiDragKind::kTab && click_tab.valid()) {
    // A plain click on a tab row: the deferred activation.
    view_->Apply(SidebarActivate{SidebarKey::Tab(click_tab.value), 0});
    return false;
  }
  // The page band lands before the slot paths below: a split drop has no
  // insertion slot at all, and the dead middle band is a no-op.
  if (band == PhiDropBand::kContent) {
    if (active && split_zone == PhiSplitDropZone::kNewWindow) {
      // The page's centre / the New window tile (§4.8): detach, exactly
      // as a release outside every window does.
      return TearOffToNewWindow(kind, key, release);
    }
    if (active && split_zone != PhiSplitDropZone::kNone) {
      CommitSplitDrop(kind, key, split_focus, split_zone, split_replace,
                      split_id);
      return true;
    }
    return false;
  }
  if (!active) {
    return false;
  }
  if (band == PhiDropBand::kNone && kind != PhiDragKind::kChip) {
    return false;  // §4.8's cancel band: the release cancels.
  }
  if (band == PhiDropBand::kOutside) {
    return TearOffToNewWindow(kind, key, release);
  }
  if (kind == PhiDragKind::kChip) {
    if (slot < 0 || chip_pos < 0 ||
        chip_pos >= static_cast<int>(strip()->chips().size()) ||
        !spaces_host_) {
      return false;
    }
    // Insert-before `slot` becomes the final position by removing the
    // dragged pip first; the slot counts the WINDOWED pips, the row index
    // adds those scrolled out of the viewport ahead of it.
    const int to_pos = slot > chip_pos ? slot - 1 : slot;
    if (to_pos == chip_pos) {
      return false;
    }
    spaces_host_->ReorderSpace(
        strip()->chips()[static_cast<size_t>(chip_pos)].id,
        to_pos + strip()->first_index());
    return true;
  }
  // Everything else is ONE verb: the item goes to the resolved place — a
  // reorder, a tree move, a pin, an unpin, a bookmark, a conversion between
  // the two stored sections (docs/sidebar-architecture.md §5.2).
  if (!place_valid) {
    return false;
  }
  // Armed BEFORE the move: its rebuild may run inside Apply as well as on
  // the posted turn, and the glide has to be waiting either way.
  if (strip_drop) {
    view_->BeginStripLanding(key, drop_left);
  }
  const bool acted = view_->Apply(SidebarMove{key, place});
  if (!acted && strip_drop) {
    view_->CancelStripLanding();
  }
  return acted;
}

void PhiSidebarDragSession::SyncHoverExpand(POINT pt) {
  SidebarKey target;
  if (band_ == PhiDropBand::kBookmarks && bookmark_drop_folder_row_ >= 0 &&
      bookmark_drop_folder_row_ <
          static_cast<int>(view_->bookmarks().size())) {
    // A drop INTO a collapsed folder: resting there opens it.
    target = view_->bookmarks()[static_cast<size_t>(bookmark_drop_folder_row_)]
                 .key;
  } else if (band_ == PhiDropBand::kTabs) {
    for (const GroupHeader& header : view_->group_headers()) {
      if (header.collapsed && ::PtInRect(&header.bounds, pt)) {
        target = SidebarKey::Group(header.id);
        break;
      }
    }
  }
  if (target == hover_expand_key_) {
    return;  // Same target (or none): the dwell keeps running.
  }
  CancelHoverExpand();
  if (target.empty() || !shell_hwnd()) {
    return;
  }
  hover_expand_key_ = target;
  ::SetTimer(shell_hwnd(), kHoverExpandTimer, kHoverExpandMs, nullptr);
}

void PhiSidebarDragSession::CancelHoverExpand() {
  if (hover_expand_key_.empty()) {
    return;
  }
  hover_expand_key_ = {};
  if (shell_hwnd()) {
    ::KillTimer(shell_hwnd(), kHoverExpandTimer);
  }
}

void PhiSidebarDragSession::FireHoverExpand() {
  const SidebarKey key = hover_expand_key_;
  hover_expand_key_ = {};
  if (!active_ || key.empty()) {
    return;
  }
  if (key.kind == SidebarKey::Kind::kFolder) {
    // Outline disclosure is view state: the folder opens and stays open
    // after the drop, as the Mac's spring-loaded folders do.
    view_->Expand(key.id);
    shell_->SidebarScheduleRefresh();
  } else if (key.kind == SidebarKey::Kind::kGroup && framework_) {
    const BrowserId browser = shell_->SidebarActiveBrowser();
    if (browser.valid()) {
      framework_->SetTabGroupCollapsed(browser.value, key.id.c_str(), false);
    }
  }
}

void PhiSidebarDragSession::OnRowsRebuilt() {
  if (active_ && kind_ != PhiDragKind::kChip) {
    Update(point_);
  }
}

void PhiSidebarDragSession::CancelDrag() {
  const bool was_active = active_;
  CancelHoverExpand();
  ReleaseDropZones();
  grid_extra_ = 0;
  kind_ = PhiDragKind::kNone;
  band_ = PhiDropBand::kNone;
  key_ = {};
  active_ = false;
  insert_ = -1;
  strip_anchor_ = -1;
  place_valid_ = false;
  folder_key_ = {};
  bookmark_drop_folder_row_ = -1;
  focus_tab_id_ = TabId();
  click_tab_id_ = TabId();
  split_drop_valid_ = false;
  split_drop_zone_ = PhiSplitDropZone::kNone;
  split_drop_replace_ = false;
  split_drop_split_id_.clear();
  PhiSplitDropOverlay::Hide();
  PhiDropBadge::Hide();
  pip_target_.clear();
  pip_target_label_.clear();
  drag_slide_from_.clear();
  drag_slide_to_.clear();
  row_slide_from_.clear();
  row_slide_to_.clear();
  drag_slide_start_ = 0;
  if (shell_hwnd()) {
    ::KillTimer(shell_hwnd(), kSlideTimer);
  }
  if (was_active) {
    if (::GetCapture() == shell_hwnd()) {
      ::ReleaseCapture();
    }
    if (shell_hwnd()) {
      ::InvalidateRect(shell_hwnd(), nullptr, FALSE);
    }
  }
}

void PhiSidebarDragSession::AbortDrag() {
  if (finished_) {
    return;  // Already committed or cancelled: nothing is in flight.
  }
  finished_ = true;
  const bool active = active_;
  const bool inside = image_inside_;
  CancelDrag();
  if (active) {
    EndDragImage(/*acted=*/false, inside);
  }
}

// --- The drag image ----------------------------------------------------------
//
// The Mac's NSDraggingSession image with TabDraggingSession's switching on
// top of it. See PhiDragImage for the window; this is the shell's side —
// what to lift, where the boundary runs, and what the page card shows.

RECT PhiSidebarDragSession::DragSourceRect() {
  const PhiSidebarRects layout = shell_->SidebarRects();
  RECT r = {};
  switch (kind_) {
    case PhiDragKind::kTab:
      if (Index() >= 0 && Index() < static_cast<int>(view_->tabs().size())) {
        r = view_->tabs()[static_cast<size_t>(Index())].bounds;
        if (!layout.horizontal_tabs) {
          r.top += Scaled(kRowPad);  // The pill inside the row.
          r.bottom -= Scaled(kRowPad);
        }
      }
      break;
    case PhiDragKind::kPin:
      if (Index() >= 0 &&
          Index() < static_cast<int>(view_->pinned().size())) {
        r = view_->pinned()[static_cast<size_t>(Index())].bounds;
      }
      break;
    case PhiDragKind::kBookmark:
      if (Index() >= 0 &&
          Index() < static_cast<int>(view_->bookmarks().size())) {
        const ShortcutRow& row =
            view_->bookmarks()[static_cast<size_t>(Index())];
        r = row.bounds;
        r.top += Scaled(kRowPad);
        r.bottom -= Scaled(kRowPad);
        r.left = std::min(
            r.left + BookmarkIndentDepth(row.depth) * Scaled(kBookmarkIndent),
            r.right - Scaled(shell_metrics::kFaviconSlot) - Scaled(12));
      }
      break;
    case PhiDragKind::kChip:
    case PhiDragKind::kNone:
      break;
  }
  return r;
}

bool PhiSidebarDragSession::DragImageInside(POINT pt) {
  if (kind_ == PhiDragKind::kBookmark && Index() >= 0 &&
      Index() < static_cast<int>(view_->bookmarks().size()) &&
      view_->bookmarks()[static_cast<size_t>(Index())].folder) {
    return true;  // A folder has no page to show (shouldTrackDraggingImage).
  }
  const PhiSidebarRects layout = shell_->SidebarRects();
  RECT band = layout.sidebar;
  if (layout.horizontal_tabs) {
    band = layout.title_bar;  // The strip lives in the caption band.
  } else {
    // The Mac's boundary is the vertical tab list's view: pins, bookmarks,
    // "+ New Tab" and the tabs — not the header above them.
    band.top = layout.favorites.top;
    if (layout.bottom_bar.bottom > layout.bottom_bar.top) {
      band.bottom = layout.bottom_bar.top;
    }
  }
  if (::PtInRect(&band, pt)) {
    return true;
  }
  POINT screen = pt;
  ::ClientToScreen(shell_hwnd(), &screen);
  RECT window_rect = {};
  ::GetWindowRect(frame_hwnd(), &window_rect);
  if (::PtInRect(&window_rect, screen)) {
    return false;  // The page, the header, the caption band.
  }
  // Another shell window under the cursor takes the row into its strip, so
  // the row is what shows there (isInsideAnyOtherBrowserTabDragBoundary).
  return window_host_ &&
         window_host_->SlotAtScreenPoint(screen, slot_.value) != 0;
}

bool PhiSidebarDragSession::OnStrip() const {
  return kind_ == PhiDragKind::kTab && shell_->SidebarRects().horizontal_tabs;
}

RECT PhiSidebarDragSession::StripProxyRect(POINT pt) const {
  const int width = strip_home_.right - strip_home_.left;
  const int height = strip_home_.bottom - strip_home_.top;
  RECT r = {pt.x - strip_grip_.x, strip_home_.top, 0, 0};
  // The strip runs from its first placed slot to the "+" (or to where the
  // tabs must end when the "+" was squeezed out). The Mac's exception: the
  // strip's last tab, when grouped, may run over the "+" to the strip's own
  // right edge — the "+" trails it so closely that it could never move
  // right enough to leave its group by the trailing edge otherwise. That
  // edge is where the "+" slot ends, in front of search-tabs (the Mac's
  // combinedFrame.maxX); `tabs_limit` is only where the tabs stop so the
  // "+" fits, the last tab's own right edge on a full strip.
  const PhiSidebarRects layout = shell_->SidebarRects();
  int min_x = layout.tabs_limit;
  for (const TabRow& tab : view_->tabs()) {
    if (tab.bounds.right > tab.bounds.left) {
      min_x = std::min(min_x, static_cast<int>(tab.bounds.left));
    }
  }
  for (const GroupHeader& header : view_->group_headers()) {
    if (header.chip && header.bounds.right > header.bounds.left) {
      min_x = std::min(min_x, static_cast<int>(header.bounds.left));
    }
  }
  const RECT& plus = view_->add_tab_button();
  int max_x = plus.right > plus.left ? plus.left : layout.tabs_limit;
  const int index = Index();
  const bool rightmost_grouped =
      index >= 0 && index == static_cast<int>(view_->tabs().size()) - 1 &&
      !view_->tabs()[static_cast<size_t>(index)].group_id.empty();
  if (rightmost_grouped) {
    max_x = layout.search_tabs.left;
  }
  const int lo = min_x;
  const int hi = max_x - Scaled(6) - width;
  if (lo <= hi) {
    // A soft clamp: past either end the tab gives a little (8pt at 35%),
    // the Mac's elastic edge.
    const int limit = Scaled(8);
    constexpr double kOvershoot = 0.35;
    if (r.left < lo) {
      const int delta = std::min(static_cast<int>(lo - r.left), limit);
      r.left = lo - static_cast<int>(std::lround(delta * kOvershoot));
    } else if (r.left > hi) {
      const int delta = std::min(static_cast<int>(r.left - hi), limit);
      r.left = hi + static_cast<int>(std::lround(delta * kOvershoot));
    }
  } else {
    r.left = lo;
  }
  r.right = r.left + width;
  r.bottom = r.top + height;
  return r;
}

void PhiSidebarDragSession::BeginDragImage(POINT pt) {
  if (kind_ == PhiDragKind::kChip || kind_ == PhiDragKind::kNone ||
      !shell_hwnd()) {
    return;  // A chip drag paints its own pill in the strip.
  }
  const RECT source = DragSourceRect();
  if (source.right <= source.left || source.bottom <= source.top) {
    return;
  }
  // Lift the element's pixels straight out of the frame, painted at home in
  // its lifted style — the Mac's createDraggingSnapshot of the cell.
  capture_ = true;
  const PhiDragBitmap row = shell_->SidebarCaptureChrome(source);
  capture_ = false;
  if (row.empty()) {
    return;
  }
  POINT corners[2] = {{source.left, source.top}, {source.right, source.bottom}};
  ::MapWindowPoints(shell_hwnd(), nullptr, corners, 2);
  home_screen_ = {corners[0].x, corners[0].y, corners[1].x, corners[1].y};
  strip_home_ = source;
  strip_grip_ = {pt.x - source.left, pt.y - source.top};
  image_inside_ = true;
  card_built_ = false;
  card_ = PhiDragCard();
  POINT screen = pt;
  ::ClientToScreen(shell_hwnd(), &screen);
  PhiDragImage::Begin(shell_hwnd(), shell_->SidebarDpi(), row,
                      Scaled(kCornerRadius),
                      POINT{pt.x - source.left, pt.y - source.top}, screen);
  // The element leaves the chrome: its slot is the gap from here on.
  ::InvalidateRect(shell_hwnd(), nullptr, FALSE);
}

void PhiSidebarDragSession::SyncDragImage(POINT pt) {
  if (!active_ || !shell_hwnd() || !PhiDragImage::active()) {
    return;
  }
  POINT screen = pt;
  ::ClientToScreen(shell_hwnd(), &screen);
  const bool inside = DragImageInside(pt);
  // Over another band the element shows as what it becomes there (the
  // Mac's temporary drag image over the pinned collection): a row over the
  // grid as a cell, a cell over the outline or the list as a row. A folder
  // never reaches the grid (Update keeps it out of kPins).
  Morph morph = Morph::kNone;
  int shape_w = 0;
  int shape_h = 0;
  if (inside && band_ == PhiDropBand::kPins &&
      (kind_ == PhiDragKind::kTab || kind_ == PhiDragKind::kBookmark) &&
      DragCellSize(&shape_w, &shape_h)) {
    morph = Morph::kCell;
  } else if (inside && kind_ == PhiDragKind::kPin &&
             (band_ == PhiDropBand::kTabs ||
              band_ == PhiDropBand::kBookmarks) &&
             DragRowSize(&shape_w, &shape_h)) {
    morph = Morph::kRow;
  }
  // On the strip the row image rides the tabs' line, not the cursor.
  const PhiSidebarRects layout = shell_->SidebarRects();
  const bool on_strip = inside && OnStrip() &&
                        ::PtInRect(&layout.title_bar, pt);
  const auto place_on_strip = [&]() {
    const RECT proxy = StripProxyRect(pt);
    POINT origin = {proxy.left, proxy.top};
    ::ClientToScreen(shell_hwnd(), &origin);
    PhiDragImage::PlaceAt(origin);
  };
  if (inside == image_inside_ && morph == image_morph_ &&
      (morph == Morph::kNone || shape_w == image_morph_width_)) {
    if (on_strip) {
      place_on_strip();
    } else {
      PhiDragImage::MoveTo(screen);
    }
    return;
  }
  image_inside_ = inside;
  image_morph_ = morph;
  if (morph != Morph::kNone) {
    image_morph_width_ = shape_w;
    PhiDragImage::ShowMorph(morph == Morph::kCell
                                ? BuildDragCell(shape_w, shape_h)
                                : BuildDragRow(shape_w, shape_h),
                            Scaled(kCornerRadius), screen);
    return;
  }
  if (inside) {
    PhiDragImage::ShowRow(screen);  // Back in: the row again.
    if (on_strip) {
      place_on_strip();
    }
    return;
  }
  if (!card_built_) {
    // Composed once per drag, the first time the cursor leaves — the Mac's
    // cachedTabSnapshotImage. Out of sight first: the card may read the
    // page off the screen, and the row image is hanging over its edge.
    card_built_ = true;
    PhiDragImage::Conceal();
    card_ = BuildDragCard();
  }
  PhiDragImage::ShowCard(card_, shell_->SidebarPalette(), screen);
}

void PhiSidebarDragSession::EndDragImage(bool acted, bool inside) {
  if (!PhiDragImage::active()) {
    return;
  }
  // AppKit's animatesToStartingPositionsOnCancelOrFail: true inside the
  // sidebar, false once the drag has left it (a tear-off must not bounce).
  if (!acted && inside) {
    PhiDragImage::SlideBack(home_screen_);
  } else {
    PhiDragImage::Hide();
  }
}

PhiDragBitmap PhiSidebarDragSession::CaptureContentFromScreen() {
  PhiDragBitmap out;
  const HWND content = shell_->SidebarContentWindow();
  if (!content || !::IsWindow(content) || !::IsWindowVisible(content) ||
      !shell_hwnd() ||
      ::IsIconic(shell_hwnd())) {
    return out;
  }
  // The child paints through DirectComposition, which PrintWindow cannot
  // read (the same blindness captureWindow works around over CDP) — but
  // the screen itself can be read back, and the page is on it right now.
  RECT page = {};
  ::GetWindowRect(shell_->SidebarContentWindow(), &page);
  MONITORINFO monitor = {sizeof(monitor)};
  if (!::GetMonitorInfoW(
          ::MonitorFromWindow(shell_->SidebarContentWindow(),
                              MONITOR_DEFAULTTONULL),
          &monitor)) {
    return out;
  }
  RECT visible = {};
  if (!::IntersectRect(&visible, &page, &monitor.rcMonitor)) {
    return out;
  }
  const int w = visible.right - visible.left;
  const int h = visible.bottom - visible.top;
  if (w < 32 || h < 32) {
    return out;
  }
  // Downscale on the way in: the card is 220 DIPs wide, and four times that
  // keeps the bicubic pass sharp without dragging a 4K frame around.
  const int limit = Scaled(PhiDragImage::kCardWidth * 4);
  const double scale =
      std::min(1.0, static_cast<double>(limit) / std::max(w, h));
  const int tw = std::max(1, static_cast<int>(w * scale));
  const int th = std::max(1, static_cast<int>(h * scale));
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = tw;
  info.bmiHeader.biHeight = -th;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = ::GetDC(nullptr);
  HDC mem = ::CreateCompatibleDC(screen);
  HBITMAP dib =
      ::CreateDIBSection(mem, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) {
    if (dib) {
      ::DeleteObject(dib);
    }
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);
    return out;
  }
  HGDIOBJ old = ::SelectObject(mem, dib);
  ::SetStretchBltMode(mem, HALFTONE);
  ::SetBrushOrgEx(mem, 0, 0, nullptr);
  const bool copied = ::StretchBlt(mem, 0, 0, tw, th, screen, visible.left,
                                   visible.top, w, h, SRCCOPY) != FALSE;
  ::GdiFlush();
  if (copied) {
    const size_t bytes = static_cast<size_t>(tw) * static_cast<size_t>(th) * 4;
    // SAFETY: the DIB is tw x th at 32bpp, top-down — exactly `bytes`.
    const auto pixels = UNSAFE_BUFFERS(base::span<const unsigned char>(
        static_cast<const unsigned char*>(bits), bytes));
    out.bgra.assign(pixels.begin(), pixels.end());
    // Screen pixels carry no alpha; the card composes them premultiplied.
    for (size_t i = 3; i < out.bgra.size(); i += 4) {
      out.bgra[i] = 0xFF;
    }
    out.width = tw;
    out.height = th;
  }
  ::SelectObject(mem, old);
  ::DeleteObject(dib);
  ::DeleteDC(mem);
  ::ReleaseDC(nullptr, screen);
  if (PhiDragBitmapIsUniform(out)) {
    out = PhiDragBitmap();  // A page that has not painted yet is no page.
  }
  return out;
}

bool PhiSidebarDragSession::DragCellSize(int* width, int* height) {
  const PhiSidebarRects layout = shell_->SidebarRects();
  if (layout.horizontal_tabs) {
    return false;
  }
  bool split = false;
  if (kind_ == PhiDragKind::kTab && Index() >= 0 &&
      Index() < static_cast<int>(view_->tabs().size())) {
    split = view_->tabs()[static_cast<size_t>(Index())].b_index >= 0;
  } else if (kind_ == PhiDragKind::kBookmark && Index() >= 0 &&
             Index() < static_cast<int>(view_->bookmarks().size())) {
    split = !view_->bookmarks()[static_cast<size_t>(Index())].url_b.empty();
  } else {
    return false;
  }
  // The phantom cell of the grid preview the slide animation opens.
  std::vector<int> units;
  for (const ShortcutRow& pin : view_->pinned()) {
    units.push_back(pin.url_b.empty() ? 1 : 2);
  }
  const int at = std::max(0, std::min(insert_, static_cast<int>(units.size())));
  units.insert(units.begin() + at, split ? 2 : 1);
  const std::vector<RECT> cells = view_->PinnedGridWalk(units, layout);
  if (static_cast<size_t>(at) >= cells.size()) {
    return false;
  }
  const RECT& cell = cells[static_cast<size_t>(at)];
  // A full three-column row's cell is the widest the image gets: an only
  // pin's sidebar-wide cell would hide the very zone it hovers.
  const int row_w = layout.sidebar.right - layout.sidebar.left -
                    2 * Scaled(shell_metrics::kEdgeInset) -
                    2 * Scaled(shell_metrics::kPinnedGap);
  const int cap = std::max(Scaled(shell_metrics::kPinnedCellHeight),
                           row_w / 3 * (split ? 2 : 1));
  *width = std::min(static_cast<int>(cell.right - cell.left), cap);
  *height = cell.bottom - cell.top;
  return *width > 0 && *height > 0;
}

bool PhiSidebarDragSession::DragRowSize(int* width, int* height) {
  const PhiSidebarRects layout = shell_->SidebarRects();
  if (layout.horizontal_tabs) {
    return false;
  }
  // The rows' pill: the list's width, a row less the pad above and below.
  *width = layout.sidebar.right - layout.sidebar.left - 2 * layout.tabs_left;
  *height = Scaled(kTabRowHeight) - 2 * Scaled(kRowPad);
  return *width > 0 && *height > 0;
}

PhiDragBitmap PhiSidebarDragSession::BuildDragRow(int width, int height) {
  PhiDragBitmap out;
  if (width <= 0 || height <= 0 || kind_ != PhiDragKind::kPin ||
      Index() < 0 || Index() >= static_cast<int>(view_->pinned().size())) {
    return out;
  }
  const ShortcutRow& row = view_->pinned()[static_cast<size_t>(Index())];
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // Top-down.
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = ::GetDC(nullptr);
  HDC mem = ::CreateCompatibleDC(screen);
  HBITMAP dib =
      ::CreateDIBSection(mem, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  ::ReleaseDC(nullptr, screen);
  if (!dib || !bits) {
    if (dib) {
      ::DeleteObject(dib);
    }
    ::DeleteDC(mem);
    return out;
  }
  HGDIOBJ old = ::SelectObject(mem, dib);
  const PhiPalette palette = shell_->SidebarPalette();
  const RECT pill = {0, 0, width, height};
  // The lifted row, as the list paints the one being dragged: the strong
  // wash under the border hairline; the image rounds the corners.
  HBRUSH wash = ::CreateSolidBrush(palette.hover_strong);
  ::FillRect(mem, &pill, wash);
  ::DeleteObject(wash);
  PhiStrokeRoundRectAA(mem, pill, Scaled(kCornerRadius), palette.border,
                       Scaled(1));
  ::SetBkMode(mem, TRANSPARENT);
  // The favicon in its 16pt slot, 6 in; the title 8 after the slot.
  const int mid = height / 2;
  const int slot_px = Scaled(shell_metrics::kFaviconSlot);
  const int icon_px = Scaled(sidebar_metrics::kFaviconSize);
  const RECT slot = {Scaled(6), mid - slot_px / 2, Scaled(6) + slot_px,
                     mid + slot_px / 2};
  const RECT icon = {slot.left + (slot_px - icon_px) / 2, mid - icon_px / 2,
                     slot.left + (slot_px - icon_px) / 2 + icon_px,
                     mid + icon_px / 2};
  bool drawn = false;
  if (!row.favicon.empty() && row.favicon_width > 0 &&
      row.favicon_height > 0) {
    RECT draw = icon;
    if (row.favicon_width < icon_px && row.favicon_height < icon_px) {
      draw.left = icon.left + (icon_px - row.favicon_width) / 2;
      draw.top = icon.top + (icon_px - row.favicon_height) / 2;
      draw.right = draw.left + row.favicon_width;
      draw.bottom = draw.top + row.favicon_height;
    }
    drawn = PhiDrawBgra(mem, draw, row.favicon.data(), row.favicon_width,
                        row.favicon_height);
  }
  HFONT font = shell_->SidebarTextFont();
  HGDIOBJ previous_font = font ? ::SelectObject(mem, font) : nullptr;
  if (!drawn) {
    ::SetTextColor(mem, palette.label_dim);
    const std::wstring initial =
        row.title.empty() ? std::wstring(L"?") : row.title.substr(0, 1);
    RECT text = slot;
    PhiDrawTextAA(mem, initial.c_str(), -1, &text,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
  ::SetTextColor(mem, palette.label);
  RECT title = {slot.right + Scaled(8), 0, width - Scaled(4), height};
  PhiDrawTextAA(mem, row.title.c_str(), -1, &title,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
  if (previous_font) {
    ::SelectObject(mem, previous_font);
  }
  ::GdiFlush();
  const size_t bytes =
      static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
  out.width = width;
  out.height = height;
  // SAFETY: the DIB section was created top-down at 32bpp with exactly
  // width*height*4 bytes and no stride padding.
  const auto pixels = UNSAFE_BUFFERS(base::span<const unsigned char>(
      static_cast<const unsigned char*>(bits), bytes));
  out.bgra.assign(pixels.begin(), pixels.end());
  ::SelectObject(mem, old);
  ::DeleteObject(dib);
  ::DeleteDC(mem);
  return out;
}

PhiDragBitmap PhiSidebarDragSession::BuildDragCell(int width, int height) {
  PhiDragBitmap out;
  if (width <= 0 || height <= 0) {
    return out;
  }
  // What the cell carries: favicon(s) and the title's initial as fallback.
  const std::vector<unsigned char>* icon_a = nullptr;
  int icon_a_w = 0;
  int icon_a_h = 0;
  const std::vector<unsigned char>* icon_b = nullptr;
  int icon_b_w = 0;
  int icon_b_h = 0;
  std::wstring title;
  bool split = false;
  if (kind_ == PhiDragKind::kTab && Index() >= 0 &&
      Index() < static_cast<int>(view_->tabs().size())) {
    const TabRow& row = view_->tabs()[static_cast<size_t>(Index())];
    icon_a = &row.favicon;
    icon_a_w = row.favicon_width;
    icon_a_h = row.favicon_height;
    title = row.title;
    if (row.b_index >= 0) {
      split = true;
      icon_b = &row.b_favicon;
      icon_b_w = row.b_favicon_width;
      icon_b_h = row.b_favicon_height;
    }
  } else if (kind_ == PhiDragKind::kBookmark && Index() >= 0 &&
             Index() < static_cast<int>(view_->bookmarks().size())) {
    const ShortcutRow& row = view_->bookmarks()[static_cast<size_t>(Index())];
    icon_a = &row.favicon;
    icon_a_w = row.favicon_width;
    icon_a_h = row.favicon_height;
    title = row.title;
    if (!row.url_b.empty()) {
      split = true;
      icon_b = &row.favicon_b;
      icon_b_w = row.favicon_b_width;
      icon_b_h = row.favicon_b_height;
    }
  } else {
    return out;
  }
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // Top-down.
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = ::GetDC(nullptr);
  HDC mem = ::CreateCompatibleDC(screen);
  HBITMAP dib =
      ::CreateDIBSection(mem, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  ::ReleaseDC(nullptr, screen);
  if (!dib || !bits) {
    if (dib) {
      ::DeleteObject(dib);
    }
    ::DeleteDC(mem);
    return out;
  }
  HGDIOBJ old = ::SelectObject(mem, dib);
  const PhiPalette palette = shell_->SidebarPalette();
  const RECT cell = {0, 0, width, height};
  // The lifted cell, as the pinned painter draws the one being dragged:
  // the strong wash under the border hairline. The image rounds the
  // corners afterwards.
  HBRUSH wash = ::CreateSolidBrush(palette.hover_strong);
  ::FillRect(mem, &cell, wash);
  ::DeleteObject(wash);
  PhiStrokeRoundRectAA(mem, cell, Scaled(kCornerRadius), palette.border,
                       Scaled(1));
  ::SetBkMode(mem, TRANSPARENT);
  // A favicon never upscales (1:1 centred when smaller than its slot).
  const auto draw_icon = [&](const RECT& slot,
                             const std::vector<unsigned char>* bgra, int w,
                             int h) {
    if (!bgra || bgra->empty() || w <= 0 || h <= 0) {
      return false;
    }
    RECT draw = slot;
    if (w < slot.right - slot.left && h < slot.bottom - slot.top) {
      draw.left = slot.left + (slot.right - slot.left - w) / 2;
      draw.top = slot.top + (slot.bottom - slot.top - h) / 2;
      draw.right = draw.left + w;
      draw.bottom = draw.top + h;
    }
    PhiDrawBgra(mem, draw, bgra->data(), w, h);
    return true;
  };
  const int cx = width / 2;
  const int cy = height / 2;
  bool drawn = false;
  if (split) {
    // Two 16pt favicons side by side (the Mac's PinnedSplitItem).
    const int size = Scaled(16);
    const RECT slot_a = {cx - Scaled(2) - size, cy - size / 2, cx - Scaled(2),
                         cy + size / 2};
    const RECT slot_b = {cx + Scaled(2), cy - size / 2, cx + Scaled(2) + size,
                         cy + size / 2};
    drawn = draw_icon(slot_a, icon_a, icon_a_w, icon_a_h);
    drawn = draw_icon(slot_b, icon_b, icon_b_w, icon_b_h) || drawn;
  } else {
    const int size = Scaled(sidebar_metrics::kPinnedFavicon);
    const RECT slot = {cx - size / 2, cy - size / 2, cx + size / 2,
                       cy + size / 2};
    drawn = draw_icon(slot, icon_a, icon_a_w, icon_a_h);
  }
  if (!drawn) {
    HFONT font = shell_->SidebarSemiboldFont();
    HGDIOBJ previous_font = font ? ::SelectObject(mem, font) : nullptr;
    ::SetTextColor(mem, palette.label);
    const std::wstring initial =
        title.empty() ? std::wstring(L"?") : title.substr(0, 1);
    RECT text = cell;
    PhiDrawTextAA(mem, initial.c_str(), -1, &text,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (previous_font) {
      ::SelectObject(mem, previous_font);
    }
  }
  ::GdiFlush();
  const size_t bytes =
      static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
  out.width = width;
  out.height = height;
  // SAFETY: the DIB section was created top-down at 32bpp with exactly
  // width*height*4 bytes and no stride padding.
  out.bgra.assign(UNSAFE_BUFFERS(base::span<const unsigned char>(
                                     static_cast<const unsigned char*>(bits),
                                     bytes))
                      .begin(),
                  UNSAFE_BUFFERS(base::span<const unsigned char>(
                                     static_cast<const unsigned char*>(bits),
                                     bytes))
                      .end());
  ::SelectObject(mem, old);
  ::DeleteObject(dib);
  ::DeleteDC(mem);
  return out;
}

PhiDragCard PhiSidebarDragSession::BuildDragCard() {
  PhiDragCard card;
  const BrowserId browser = shell_->SidebarActiveBrowser();
  WindowSnapshot strip;  // Pulled: the press opened the row this turn.
  if (!shell_->SidebarPullSnapshot(browser, &strip)) {
    strip = WindowSnapshot();
  }
  const auto copy_favicon = [](const std::vector<unsigned char>& bgra,
                               int width, int height, PhiDragBitmap* out) {
    if (!bgra.empty() && width > 0 && height > 0) {
      out->bgra = bgra;
      out->width = width;
      out->height = height;
    }
  };
  TabId tab_a;
  TabId tab_b;
  switch (kind_) {
    case PhiDragKind::kTab: {
      if (Index() < 0 || Index() >= static_cast<int>(view_->tabs().size())) {
        return card;
      }
      const TabRow& row = view_->tabs()[static_cast<size_t>(Index())];
      tab_a = row.id;
      card.a.title = row.title;
      copy_favicon(row.favicon, row.favicon_width, row.favicon_height,
                   &card.a.favicon);
      if (row.b_index >= 0) {
        card.split = true;
        card.stacked = shell_->SidebarSplitIsStacked(row.id);
        tab_b = row.b_id;
        card.b.title = row.b_title;
        copy_favicon(row.b_favicon, row.b_favicon_width, row.b_favicon_height,
                     &card.b.favicon);
      }
      break;
    }
    case PhiDragKind::kPin:
    case PhiDragKind::kBookmark: {
      const std::vector<ShortcutRow>& rows =
          kind_ == PhiDragKind::kPin ? view_->pinned() : view_->bookmarks();
      if (Index() < 0 || Index() >= static_cast<int>(rows.size())) {
        return card;
      }
      const ShortcutRow& row = rows[static_cast<size_t>(Index())];
      card.a.title = row.title;
      copy_favicon(row.favicon, row.favicon_width, row.favicon_height,
                   &card.a.favicon);
      // The press that armed the drag opened the row: its live tab is found
      // by tag, which the cached view row may not know yet.
      tab_a = PhiLiveTabForRow(strip, row.guid,
                                  row.open ? row.tab_id : TabId());
      if (!row.url_b.empty()) {
        card.split = true;
        card.stacked = row.layout == "horizontal";
        card.b.title = row.title_b;
        copy_favicon(row.favicon_b, row.favicon_b_width, row.favicon_b_height,
                     &card.b.favicon);
        tab_b = PhiLiveTabForRow(strip, TagFor(row.guid, true),
                                    row.tab_id_b);
      }
      break;
    }
    case PhiDragKind::kChip:
    case PhiDragKind::kNone:
      return card;
  }
  // What is on screen beats the cache (the Mac's live capture for the
  // active tab): after the press, the dragged row's page is the one showing
  // — and a split shows both panes at once, so the capture IS the composite.
  bool showing = false;
  for (const TabSnapshot& tab : strip.tabs) {
    if (tab.active && (tab.id == tab_a || (tab_b.valid() && tab.id == tab_b))) {
      showing = true;
      break;
    }
  }
  if (showing) {
    card.whole = CaptureContentFromScreen();
  }
  if (card.whole.empty() && framework_) {
    const auto thumbnail = [&](TabId tab_id, PhiDragBitmap* out) {
      if (!tab_id.valid()) {
        return;
      }
      int width = 0;
      int height = 0;
      const unsigned char* pixels = framework_->TabThumbnailBgra(
          browser.value, tab_id.value, &width, &height);
      if (!pixels || width <= 0 || height <= 0) {
        return;
      }
      // SAFETY: TabThumbnailBgra reports the dimensions of the buffer it
      // just returned, the same contract TabFaviconBgra carries.
      const auto copy = UNSAFE_BUFFERS(base::span<const unsigned char>(
          pixels,
          static_cast<size_t>(width) * static_cast<size_t>(height) * 4));
      out->bgra.assign(copy.begin(), copy.end());
      out->width = width;
      out->height = height;
    };
    thumbnail(tab_a, &card.a.snapshot);
    if (card.split) {
      thumbnail(tab_b, &card.b.snapshot);
    }
  }
  return card;
}

bool PhiSidebarDragSession::TearOffToNewWindow(PhiDragKind kind,
                                               const SidebarKey& key,
                                               POINT release) {
  // The live tab(s) join the shell under the cursor or tear off into a new
  // window (the Mac's live-wrapper tear-off). A stored row stays behind
  // as a closed cell.
  if (kind == PhiDragKind::kPin || kind == PhiDragKind::kBookmark) {
    return TearOffStoredRow(kind, key, release);
  }
  if (kind != PhiDragKind::kTab || !framework_) {
    return false;
  }
  const TabId tab_a(TabOfKey(view_->tree(), key, 0));
  const TabId tab_b(TabOfKey(view_->tree(), key, 1));
  if (!tab_a.valid()) {
    return false;
  }
  // The drag image comes down first so the hit test cannot land on it.
  PhiDragImage::Hide();
  POINT screen = release;
  ::ClientToScreen(shell_hwnd(), &screen);
  const int64_t target =
      window_host_ ? window_host_->SlotAtScreenPoint(screen, slot_.value) : 0;
  if (!target || !window_host_ ||
      !window_host_->RequestDropTabOnWindow(slot_.value, tab_a.value,
                                            tab_b.value, target)) {
    // No browser attached: an empty strip, and nothing tears off.
    const BrowserId browser = shell_->SidebarActiveBrowser();
    if (browser.valid()) {
      if (window_host_) {
        window_host_->NoteTearOffDrop(screen);
      }
      const TabId ids[] = {tab_a, tab_b};
      framework_->MoveTabsToNewWindowByIds(browser, ids,
                                           tab_b.valid() ? 2 : 1);
    }
  }
  return true;
}

bool PhiSidebarDragSession::TearOffStoredRow(PhiDragKind kind,
                                           const SidebarKey& key,
                                           POINT release) {
  const std::vector<ShortcutRow>& rows =
      kind == PhiDragKind::kPin ? view_->pinned() : view_->bookmarks();
  const auto found =
      std::find_if(rows.begin(), rows.end(),
                   [&](const ShortcutRow& r) { return r.key == key; });
  if (!framework_ || found == rows.end()) {
    return false;
  }
  const ShortcutRow row = *found;
  if (row.folder) {
    return false;  // Nothing to open.
  }
  const BrowserId browser = shell_->SidebarActiveBrowser();
  WindowSnapshot strip;  // Pulled: the press opened the row this turn.
  if (!shell_->SidebarPullSnapshot(browser, &strip)) {
    return false;  // No browser attached: nothing is open to tear off.
  }
  // The press that armed the drag opened the row, so its live tab(s) are
  // found by tag — the Mac's live-wrapper path. A row that failed to open
  // has nothing to tear off.
  const TabId id_a = PhiLiveTabForRow(strip, row.guid,
                                         row.open ? row.tab_id : TabId());
  TabId id_b;
  if (!row.url_b.empty()) {
    id_b = PhiLiveTabForRow(strip, TagFor(row.guid, true), row.tab_id_b);
  }
  if (!id_a.valid()) {
    return false;
  }
  // The stored row stays behind as an unopened placeholder: the Mac clears
  // custom_value on every pane before the move, or the panes would snap
  // back into the cell in the new window.
  view_->UnbindStoredTab(browser, id_a);
  if (id_b.valid()) {
    view_->UnbindStoredTab(browser, id_b);
  }
  // The drag image comes down first so the hit test cannot land on it.
  PhiDragImage::Hide();
  POINT screen = release;
  ::ClientToScreen(shell_hwnd(), &screen);
  const int64_t target =
      window_host_ ? window_host_->SlotAtScreenPoint(screen, slot_.value) : 0;
  if (!target || !window_host_ ||
      !window_host_->RequestDropTabOnWindow(slot_.value, id_a.value,
                                            id_b.value, target)) {
    if (window_host_) {
      window_host_->NoteTearOffDrop(screen);
    }
    const TabId ids[] = {id_a, id_b};
    framework_->MoveTabsToNewWindowByIds(browser, ids, id_b.valid() ? 2 : 1);
  }
  return true;
}

// --- Drop on the page: form or re-pane a split -----------------------------
//
// The Mac's SplitTabDropContainer, whose three drag sources (a tab, a pinned
// tab, a bookmark) are this client's kTab / kPin / kBookmark drags.

bool PhiSidebarDragSession::SplitDragSource(PhiDragKind kind,
                                          const SidebarKey& key,
                                          TabId* tab_id,
                                          std::string* url,
                                          std::string* guid) const {
  *tab_id = TabId();
  url->clear();
  guid->clear();
  if (key.empty()) {
    return false;
  }
  if (kind == PhiDragKind::kTab) {
    const auto found =
        std::find_if(view_->tabs().begin(), view_->tabs().end(),
                     [&](const TabRow& r) { return r.key == key; });
    if (found == view_->tabs().end()) {
      return false;
    }
    const TabRow& row = *found;
    // A split row is never a source — the page cannot nest a split inside a
    // split (the Mac's isSourceASplit).
    if (!row.split_id.empty() || row.b_index >= 0) {
      return false;
    }
    *tab_id = row.id;
    return row.id.valid();
  }
  if (kind != PhiDragKind::kPin && kind != PhiDragKind::kBookmark) {
    return false;
  }
  const std::vector<ShortcutRow>& rows =
      kind == PhiDragKind::kPin ? view_->pinned() : view_->bookmarks();
  const auto found =
      std::find_if(rows.begin(), rows.end(),
                   [&](const ShortcutRow& r) { return r.key == key; });
  if (found == rows.end()) {
    return false;
  }
  const ShortcutRow& row = *found;
  // A folder holds no page, and a STORED split cell cannot become one pane
  // of another split.
  if (row.folder || !row.url_b.empty()) {
    return false;
  }
  // Both are answered: an OPEN row can hand over its live tab, and the url is
  // what a closed one (or a self-drop) opens instead.
  *tab_id = row.open ? row.tab_id : TabId();
  *url = row.url;
  *guid = row.guid;
  return tab_id->valid() || !url->empty();
}

bool PhiSidebarDragSession::ResolveSplitDrop(POINT pt, bool rebuild) {
  const auto no_drop = [&] {
    split_drop_valid_ = false;
    split_drop_zone_ = PhiSplitDropZone::kNone;
    split_drop_replace_ = false;
    split_drop_split_id_.clear();
    PhiSplitDropOverlay::Hide();
    return false;
  };
  const RECT area = shell_->SidebarRects().content;
  if (!::PtInRect(&area, pt)) {
    return no_drop();
  }
  if (!rebuild) {
    // Mid-hover: nothing the mode depends on can change while the button is
    // down, so only the zone under the pointer is recomputed.
    if (!split_drop_valid_) {
      return false;
    }
    PhiSplitDropHints hints;
    hints.area = area;
    hints.replace = split_drop_replace_;
    hints.stacked = split_drop_stacked_;
    hints.ratio = split_drop_ratio_;
    hints.panes = split_drop_panes_;
    hints.primary = split_drop_primary_;
    hints.secondary = split_drop_secondary_;
    split_drop_zone_ =
        split_drop_replace_
            ? PhiSplitReplaceZoneAt(hints, pt, shell_->SidebarDpi())
            : PhiSplitCreateZoneAt(area, pt, split_drop_zone_,
                                   shell_->SidebarDpi());
    hints.active = split_drop_zone_;
    PhiSplitDropOverlay::Show(shell_->SidebarOwner(), hints);
    return true;
  }
  const TabId focused_id =
      focus_tab_id_.valid() ? focus_tab_id_ : view_->active_tab_id();
  if (!framework_ || !focused_id.valid()) {
    return no_drop();
  }
  TabId source_id;
  std::string source_url;
  std::string source_guid;
  if (!SplitDragSource(kind_, key_, &source_id, &source_url,
                       &source_guid)) {
    return no_drop();
  }
  const BrowserId browser = shell_->SidebarActiveBrowser();
  // Pulled, not the model's copy: the press that armed this drag opened the
  // stored row it grabbed, and the drop must see that tab's tag. Once per
  // page entry — the pointer moves after this only re-hit-test the zone.
  WindowSnapshot strip;
  if (!shell_->SidebarPullSnapshot(browser, &strip)) {
    return no_drop();
  }
  source_id = PhiLiveTabForRow(strip, source_guid, source_id);
  const TabSnapshot* focused = PhiFindTabIn(strip, focused_id);
  if (!focused) {
    return no_drop();  // The page the drag started from has closed.
  }
  // A live pinned tab or bookmark tab that is already a pane cannot be
  // dragged into another split either.
  if (const TabSnapshot* source = PhiFindTabIn(strip, source_id);
      source && !source->split_id.empty()) {
    return no_drop();
  }

  PhiSplitDropHints hints;
  hints.area = area;
  if (focused->split_id.empty()) {
    // CREATE: the four edge thirds, the middle a no-drop band.
    split_drop_replace_ = false;
    split_drop_split_id_.clear();
    split_drop_zone_ = PhiSplitCreateZoneAt(area, pt, split_drop_zone_,
                                            shell_->SidebarDpi());
  } else {
    // REPLACE: the page maps onto the focused split's two panes.
    const auto stored_is_pinned = [&](const std::string& tag) {
      SidebarTag parsed;
      if (!ParseTag(tag, &parsed)) {
        return false;
      }
      for (const ShortcutRow& row : view_->pinned()) {
        if (row.guid == parsed.guid) {
          return true;
        }
      }
      return false;
    };
    for (const TabSnapshot& tab : strip.tabs) {
      if (tab.split_id != focused->split_id) {
        continue;
      }
      // Replacing a pane with itself (or with its own sibling) is a no-op,
      // and a PINNED split persists as a guid pair the swap would strand —
      // the evicted pane would stay flagged pinned while the incoming one
      // is not. Neither offers a drop.
      if (tab.id == source_id || stored_is_pinned(tab.tag)) {
        return no_drop();
      }
    }
    bool stacked = false;
    double ratio = 0.5;
    if (const SplitSnapshot* split = PhiFindSplitIn(strip, focused->split_id)) {
      stacked = split->layout == phi_framework::SplitLayout::kHorizontal;
      ratio = split->ratio;
    }
    split_drop_replace_ = true;
    split_drop_split_id_ = focused->split_id;
    split_drop_stacked_ = stacked;
    split_drop_ratio_ = ratio;
    // The real pane rects (the framework's layout) while the split is on
    // screen — the focused tab is the active one here, so they are its.
    split_drop_panes_ = shell_->SidebarSplitPaneRects(&split_drop_primary_,
                                                      &split_drop_secondary_);
    hints.replace = true;
    hints.stacked = stacked;
    hints.ratio = ratio;
    hints.panes = split_drop_panes_;
    hints.primary = split_drop_primary_;
    hints.secondary = split_drop_secondary_;
    split_drop_zone_ = PhiSplitReplaceZoneAt(hints, pt, shell_->SidebarDpi());
  }
  hints.active = split_drop_zone_;
  split_drop_valid_ = true;
  PhiSplitDropOverlay::Show(shell_->SidebarOwner(), hints);
  return true;
}

void PhiSidebarDragSession::CommitSplitDrop(PhiDragKind kind,
                                          const SidebarKey& source,
                                          TabId focused_id,
                                          PhiSplitDropZone zone,
                                          bool replace,
                                          const std::string& split_id) {
  if (!framework_ || zone == PhiSplitDropZone::kNone || !focused_id.valid()) {
    return;
  }
  TabId source_id;
  std::string source_url;
  std::string source_guid;
  if (!SplitDragSource(kind, source, &source_id, &source_url,
                       &source_guid)) {
    return;
  }
  const BrowserId browser = shell_->SidebarActiveBrowser();
  WindowSnapshot strip;  // Pulled: see ResolveSplitDrop.
  if (!shell_->SidebarPullSnapshot(browser, &strip)) {
    return;
  }
  source_id = PhiLiveTabForRow(strip, source_guid, source_id);
  // The dragged item lands in the pane the cursor was over; the layout names
  // the divider, not the arrangement.
  const bool dropped_primary = PhiSplitZoneIsPrimary(zone);
  const char* layout = PhiSplitZoneLayout(zone);

  // A fresh tab on `url`, by its STABLE id — nothing to re-read afterwards.
  const auto open_tab = [&](const std::string& url) {
    return framework_->OpenTab(browser, url.c_str());
  };

  if (replace) {
    if (split_id.empty()) {
      return;
    }
    // Which pane the drop targets, and which tab it evicts.
    const int slot = dropped_primary ? 0 : 1;
    const SplitSnapshot* target = PhiFindSplitIn(strip, split_id);
    const TabId evicted_id =
        target ? (slot == 0 ? target->primary : target->secondary) : TabId();
    if (!evicted_id.valid()) {
      return;  // The split went away mid-drag.
    }
    // The evicted pane is kept as a standalone tab beside the split, unless
    // it is an empty new-tab page — closing that beats littering the strip
    // (the Mac's splitPaneReplacementKeepsEvicted).
    const TabSnapshot* evicted = PhiFindTabIn(strip, evicted_id);
    const bool keep_evicted =
        !evicted || !PhiNewTabPage::IsNewTabUrl(evicted->url);
    TabId incoming_id = source_id;
    if (incoming_id.valid()) {
      // A live pinned tab or bookmark tab keeps its page: drop the binding
      // and let the tab itself take the pane. Unconditional, because the
      // COMFORTABLE layout leaves bookmark-bound tabs on the strip, so even
      // a plain tab drag can be carrying a binding.
      view_->UnbindStoredTab(browser, incoming_id);
    } else {
      if (source_url.empty()) {
        return;
      }
      incoming_id = open_tab(source_url);
    }
    if (!incoming_id.valid() || incoming_id == evicted_id) {
      return;
    }
    framework_->SwapTabIntoSplitById(browser, split_id.c_str(), slot,
                                     incoming_id, keep_evicted);
    return;
  }

  // CREATE. Each source keeps the Mac's own rule for what happens when it
  // resolves to the focused tab itself.
  TabId dragged_id;
  if (kind == PhiDragKind::kTab) {
    if (source_id == focused_id) {
      // Dragged = focused: a fresh new-tab page becomes the partner and takes
      // the slot OPPOSITE the drop, since the dragged tab visually lands
      // where the cursor is.
      view_->UnbindStoredTab(browser, focused_id);
      const TabId fresh = open_tab(std::string());
      if (!fresh.valid()) {
        return;
      }
      view_->FormSplitOrdered(browser, dropped_primary ? focused_id : fresh,
                       dropped_primary ? fresh : focused_id, layout);
      return;
    }
    dragged_id = source_id;
  } else if (source_id.valid() && source_id != focused_id) {
    // A live stored row: the tab joins the split and its row stays behind as
    // an unopened one.
    dragged_id = source_id;
  } else if (kind == PhiDragKind::kBookmark && source_id == focused_id) {
    // The bookmark's live tab IS the focused pane. Opening its url again
    // would just duplicate what the user is looking at, so the partner is a
    // fresh new-tab page in the opposite slot.
    view_->UnbindStoredTab(browser, focused_id);
    const TabId fresh = open_tab(std::string());
    if (!fresh.valid()) {
      return;
    }
    view_->FormSplitOrdered(browser, dropped_primary ? focused_id : fresh,
                     dropped_primary ? fresh : focused_id, layout);
    return;
  } else {
    // A closed row — or a pinned one whose live tab is the focused pane:
    // its url opens as the new pane and the stored row is left intact, so
    // the slot still exists for next time.
    if (source_url.empty()) {
      return;
    }
    dragged_id = open_tab(source_url);
  }
  if (!dragged_id.valid() || dragged_id == focused_id) {
    return;
  }
  // NEITHER pane may carry a stored binding: a split never lives inside a
  // pinned cell or a bookmark cell, it shows as one merged row in the normal
  // tab list. Each stored row stays where it is, as an unopened placeholder
  // (the Mac's makeTabNormalOpened /
  // demotePinnedTabLeavingPlaceholder, which every one of its
  // split-formation paths runs over both panes).
  view_->UnbindStoredTab(browser, focused_id);
  view_->UnbindStoredTab(browser, dragged_id);
  view_->FormSplitOrdered(browser, dropped_primary ? dragged_id : focused_id,
                   dropped_primary ? focused_id : dragged_id, layout);
}

int PhiSidebarDragSession::DragInsertSlot(POINT pt) {
  // Resolves WHERE the drag lands (place_) with the pure resolvers and
  // answers the gap slot the slide animation opens (-1 = no gap).
  place_valid_ = false;
  folder_key_ = {};
  bookmark_drop_folder_row_ = -1;
  const auto take = [&](const DropResolution& r) {
    place_ = r.place;
    place_valid_ = true;
    folder_key_ = r.folder_key;
    if (!r.folder_key.empty()) {
      for (size_t i = 0; i < view_->bookmarks().size(); ++i) {
        if (view_->bookmarks()[i].key == r.folder_key) {
          bookmark_drop_folder_row_ = static_cast<int>(i);
        }
      }
    }
    return r.gap_slot;
  };
  const PhiSidebarRects layout = shell_->SidebarRects();
  if (band_ == PhiDropBand::kNone && kind_ != PhiDragKind::kChip) {
    return -1;  // The cancel band (or a pip target): no place, no gap.
  }
  switch (kind_) {
    case PhiDragKind::kTab:
      switch (band_) {
        case PhiDropBand::kOutside:
          return 0;  // Tear-off: any non-negative slot commits.
        case PhiDropBand::kPins:
          return take(
              ResolvePinnedDrop(view_->PinnedDropRows(), pt.x, pt.y, {}));
        case PhiDropBand::kBookmarks:
          return take(ResolveBookmarkDrop(view_->BookmarkDropRows(), pt.y, {}));
        default:
          if (layout.horizontal_tabs) {
            // The strip: the gap follows the lifted tab's edges, not the
            // pointer (ResolveStripTabDrop), against the resting rows.
            const std::vector<DropRow> rows = view_->TabDropRows();
            int dragged_row = -1;
            for (size_t i = 0; i < rows.size(); ++i) {
              if (rows[i].key == key_) {
                dragged_row = static_cast<int>(i);
                break;
              }
            }
            const RECT proxy = StripProxyRect(pt);
            const DropResolution r =
                ResolveStripTabDrop(rows, dragged_row, proxy.left, proxy.right);
            strip_anchor_ = r.anchor_row;
            return take(r);
          }
          return take(ResolveTabDrop(view_->TabDropRows(), pt.x, pt.y,
                                     /*horizontal=*/false, key_));
      }
    case PhiDragKind::kPin:
      switch (band_) {
        case PhiDropBand::kTabs:
          // Unpin into the list: the rows part at the slot for the cell's
          // tab (the vertical list; the strip keeps its line).
          return take(ResolveTabDrop(view_->TabDropRows(), pt.x, pt.y,
                                     layout.horizontal_tabs, {}));
        case PhiDropBand::kOutside:
          return -1;  // The release tears off; there is no slot.
        case PhiDropBand::kBookmarks:
          // The cell becomes a bookmark at the outline slot: the rows part
          // for it (a drop INTO a collapsed folder answers no slot and
          // highlights the folder instead).
          return take(
              ResolveBookmarkDrop(view_->BookmarkDropRows(), pt.y, {}));
        default:
          return take(
              ResolvePinnedDrop(view_->PinnedDropRows(), pt.x, pt.y, key_));
      }
    case PhiDragKind::kBookmark:
      switch (band_) {
        case PhiDropBand::kOutside:
          return -1;  // The release tears off; there is no outline slot.
        case PhiDropBand::kPins:
          return take(
              ResolvePinnedDrop(view_->PinnedDropRows(), pt.x, pt.y, {}));
        case PhiDropBand::kTabs:
          // The bookmark becomes a tab at the slot: the rows part for it.
          return take(ResolveTabDrop(view_->TabDropRows(), pt.x, pt.y,
                                     layout.horizontal_tabs, {}));
        default:
          return take(
              ResolveBookmarkDrop(view_->BookmarkDropRows(), pt.y, key_));
      }
    case PhiDragKind::kChip:
      return strip()->InsertSlotAt(pt);
    case PhiDragKind::kNone:
      break;
  }
  return -1;
}

void PhiSidebarDragSession::DrawIndicator(HDC dc) {
  if (!active_ || insert_ < 0 || !spaces_host_) {
    return;
  }
  const PhiPalette& palette = spaces_host_->PaletteForSlot(slot_.value);
  // The 2px accent insertion line, horizontal for vertical lists and
  // vertical for horizontal ones.
  RECT line{};
  const auto before_edge = [&](const RECT& r, bool horizontal_list) {
    if (horizontal_list) {
      line = {r.left - Scaled(2), r.top, r.left, r.bottom};
    } else {
      line = {r.left, r.top - Scaled(2), r.right, r.top};
    }
  };
  const auto after_edge = [&](const RECT& r, bool horizontal_list) {
    if (horizontal_list) {
      line = {r.right, r.top, r.right + Scaled(2), r.bottom};
    } else {
      line = {r.left, r.bottom, r.right, r.bottom + Scaled(2)};
    }
  };
  const auto pick = [&](const auto& rows, bool horizontal_list) {
    if (rows.empty()) {
      return false;
    }
    if (insert_ < static_cast<int>(rows.size())) {
      before_edge(rows[static_cast<size_t>(insert_)].bounds,
                  horizontal_list);
    } else {
      after_edge(rows.back().bounds, horizontal_list);
    }
    return true;
  };
  bool have = false;
  const PhiSidebarRects layout = shell_->SidebarRects();
  switch (kind_) {
    case PhiDragKind::kTab:
      // Every target animates its own gap now — the vertical lists and
      // the strip alike (Chrome's vertical tabs and the Mac's strip draw
      // no line either).
      have = false;
      break;
    case PhiDragKind::kPin:
      // The grid gap is the indicator in the sidebar; the horizontal
      // strip's pinned section keeps the line.
      have = layout.horizontal_tabs && band_ == PhiDropBand::kPins
                 ? pick(view_->pinned(), /*horizontal_list=*/true)
                 : false;
      break;
    case PhiDragKind::kBookmark:
      have = false;  // The row gap is the indicator.
      break;
    case PhiDragKind::kChip:
      have = pick(strip()->chips(), /*horizontal_list=*/true);
      break;
    case PhiDragKind::kNone:
      break;
  }
  if (!have) {
    return;
  }
  HBRUSH accent = ::CreateSolidBrush(palette.accent);
  ::FillRect(dc, &line, accent);
  ::DeleteObject(accent);
}

}  // namespace phi_client

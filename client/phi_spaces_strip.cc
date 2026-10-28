// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_spaces_strip.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/strings/utf_string_conversions.h"
#include "phibrowser-win/client/phi_animator.h"
#include "phibrowser-win/client/phi_create_space_panel.h"
#include "phibrowser-win/client/phi_create_space_flyout.h"
#include "phibrowser-win/client/phi_create_space_window.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_fluent.h"
#include "phibrowser-win/client/phi_app_model.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_shell_layout.h"
#include "phibrowser-win/client/phi_shell_window.h"

namespace phi_client {

namespace {

using shell_metrics::kSpaceChip;
using shell_metrics::kSpaceChipGap;
using shell_metrics::kSpaceChipVertical;

// The pip's selection card: radius 9 on the 24pt chip, the third-of-a-side
// the Mac draws.
constexpr int kSpaceCardRadius = 6;  // windows-native-ui.md §2.1: pips r6.
constexpr int kSpaceIcon = 13;
constexpr int kSpaceIconVertical = 14;
constexpr int kCornerRadius = 8;  // The pill radius, everywhere.
// The band slide (the Mac's Space switch) and the theme ramp behind it.
constexpr int kSpaceSwapMs = 150;
constexpr int kThemeRampMs = 600;
// The pip hover card's dwell and its (rounder than the pills') radius.
constexpr int kHoverCardDelayMs = 300;
constexpr int kHoverCardRadius = 8;  // windows-native-ui.md §5.6.
// The trailing "+"'s reveal (the Mac's 0.15s easeInOut on its opacity).
constexpr int kAddSpaceFadeMs = 150;

// Smoothstep — the curve every one of these clocks eases with.
double Smoothstep(double linear) {
  const double t = std::min(1.0, std::max(0.0, linear));
  return t * t * (3.0 - 2.0 * t);
}

// `over` at `alpha`/255 on `base` — the Mac's translucent overlays, opaque.
COLORREF BlendOver(COLORREF over, COLORREF base, int alpha) {
  auto mix = [&](int a, int b) { return (a * alpha + b * (255 - alpha)) / 255; };
  return RGB(mix(GetRValue(over), GetRValue(base)),
             mix(GetGValue(over), GetGValue(base)),
             mix(GetBValue(over), GetBValue(base)));
}

COLORREF LerpColor(COLORREF a, COLORREF b, double t) {
  const auto mix = [t](int from, int to) {
    return static_cast<int>(from + (to - from) * t + 0.5);
  };
  return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)),
             mix(GetBValue(a), GetBValue(b)));
}

// The chrome's colours ramp source → target on their own clock while a
// Space switch is in flight.
PhiPalette LerpPalette(const PhiPalette& a, const PhiPalette& b, double t) {
  PhiPalette out = b;
  out.ground_top = LerpColor(a.ground_top, b.ground_top, t);
  out.ground_bottom = LerpColor(a.ground_bottom, b.ground_bottom, t);
  out.card = LerpColor(a.card, b.card, t);
  out.selected = LerpColor(a.selected, b.selected, t);
  out.hover = LerpColor(a.hover, b.hover, t);
  out.hover_strong = LerpColor(a.hover_strong, b.hover_strong, t);
  out.label = LerpColor(a.label, b.label, t);
  out.label_dim = LerpColor(a.label_dim, b.label_dim, t);
  out.label_tertiary = LerpColor(a.label_tertiary, b.label_tertiary, t);
  out.border = LerpColor(a.border, b.border, t);
  out.separator = LerpColor(a.separator, b.separator, t);
  out.window_bg = LerpColor(a.window_bg, b.window_bg, t);
  out.accent = LerpColor(a.accent, b.accent, t);
  // Unused stops repeat the last, so a flat side lerps as a flat gradient.
  for (size_t i = 0; i < out.ground_stops.size(); ++i) {
    out.ground_stops[i] = LerpColor(a.ground_stops[i], b.ground_stops[i], t);
  }
  out.ground_stop_count = std::max(a.ground_stop_count, b.ground_stop_count);
  out.dark = t < 0.5 ? a.dark : b.dark;
  return out;
}

void FillRounded(HDC dc, const RECT& r, COLORREF fill, int radius) {
  PhiFillRoundRectAA(dc, r, radius, fill);
}

void StrokeRounded(HDC dc, const RECT& r, COLORREF fill, COLORREF border,
                   int radius) {
  PhiFillRoundRectAA(dc, r, radius, fill);
  PhiStrokeRoundRectAA(dc, r, radius, border, 1);
}

void DrawGlyph(HDC dc, const RECT& r, const wchar_t* glyph, COLORREF ink) {
  if (r.right <= r.left) {
    return;
  }
  ::SetTextColor(dc, ink);
  PhiDrawTextAA(dc, glyph, -1, const_cast<RECT*>(&r),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// The chip the paint pass draws: `painted` square, centred in whatever cell
// the layout hands it. The vertical layouts' cell is the whole 32pt strip
// band — that size is hit area and spacing, and a selection card filling it
// would swallow the icon it exists to frame, so the card stays the Mac's
// 24pt chip.
RECT PaintedChip(const RECT& cell, int painted) {
  RECT chip = cell;
  ::InflateRect(
      &chip,
      -std::max(0, static_cast<int>(cell.right - cell.left) - painted) / 2,
      -std::max(0, static_cast<int>(cell.bottom - cell.top) - painted) / 2);
  return chip;
}

}  // namespace

PhiSpacesStrip::PhiSpacesStrip(PhiSpacesStripShell* shell,
                               SpacesHost* spaces_host,
                               ChromePrefsHost* prefs_host,
                               AgentHost* agent_host)
    : shell_(shell),
      spaces_host_(spaces_host),
      prefs_host_(prefs_host),
      agent_host_(agent_host) {}

PhiSpacesStrip::~PhiSpacesStrip() {
  ReleaseCardFonts();
  StopAnimations();
  form_.reset();
  if (card_popup_ && ::IsWindow(card_popup_)) {
    ::DestroyWindow(card_popup_);
  }
  card_popup_ = nullptr;
}

void PhiSpacesStrip::StopAnimations() {
  PhiAnimator& animator = PhiAnimator::ForThread();
  animator.Cancel(fade_anim_);
  animator.Cancel(swap_anim_);
  animator.Cancel(ramp_anim_);
  fade_anim_ = swap_anim_ = ramp_anim_ = 0;
}

void PhiSpacesStrip::Shutdown() {
  StopAnimations();
  form_.reset();
  if (card_popup_ && ::IsWindow(card_popup_)) {
    ::DestroyWindow(card_popup_);
  }
  card_popup_ = nullptr;
}

// ---------------------------------------------------------------------------
// The pip row.

int PhiSpacesStrip::SwitchTargets() const {
  if (!spaces_host_ || shell_->SpacesIncognito()) {
    return 0;
  }
  int count = static_cast<int>(spaces_host_->Spaces().size());
  if (agent_host_) {
    count += static_cast<int>(
        agent_host_
            ->AgentSpaces(shell_->SpacesIsAgent() ? 0
                                                  : shell_->SpacesSlot().value)
            .size());
  }
  return count;
}

void PhiSpacesStrip::Rebuild() {
  chips_.clear();
  overflow_ = false;
  clip_ = RECT{};
  first_index_ = 0;
  creating_pip_ = RECT{};
  const PhiSpacesStripRects rects = shell_->SpacesRects();
  add_slot_ = rects.add_space;
  if (!spaces_host_ || shell_->SpacesIncognito() || !rects.spaces_switch) {
    // Incognito has no Spaces strip; nor does a window with nothing to
    // switch to (the layout dropped the row).
    add_slot_ = RECT{};
    return;
  }
  const std::string& active = shell_->SpacesActiveSpace();
  // The full pip row in strip order (the Mac's stripOrderedSpaces): the
  // user Spaces, then the live agent Spaces as pips wearing their driver's
  // brand mark (the robot for an unnamed driver) — in an agent window too,
  // where the watched one wears the selection pill exactly as
  // the Mac's agent Space does in its own window's strip. Clicking one
  // surfaces its window so the user can watch the agent work live.
  std::vector<PhiSpaceChip> row;
  for (const PhiSpaceView& space : spaces_host_->Spaces()) {
    // Comfortable shows a single compact pip — the ACTIVE Space; the rest
    // live in the popup it opens.
    if (rects.horizontal_tabs && space.id != active) {
      continue;
    }
    PhiSpaceChip chip;
    chip.id = space.id;
    chip.label = !space.name.empty() ? base::UTF8ToWide(space.name)
                                     : L"Space " + base::UTF8ToWide(space.id);
    chip.icon = base::UTF8ToWide(space.icon_text);
    chip.open = shell_->SpacesHasSpace(space.id);
    row.push_back(std::move(chip));
  }
  if (agent_host_ && !rects.horizontal_tabs) {
    // This slot's own list (the Mac's slot.presentedSpaces): an agent Space
    // another window has on screen is not offered here — only the window
    // hosting it shows the pip. An agent window's strip lists them all; it
    // never hosts one itself.
    for (const PhiAgentPipView& pip : agent_host_->AgentSpaces(
             shell_->SpacesIsAgent() ? 0 : shell_->SpacesSlot().value)) {
      PhiSpaceChip chip;
      chip.id = pip.space_id;
      chip.task_id = pip.task_id;
      chip.is_agent = true;
      chip.open = pip.watched;
      chip.badge = static_cast<int>(pip.badge) + 1;
      chip.label = PhiAgentPipLabel(pip);  // "R<number> · <driver>".
      chip.icon = pip.icon;  // The driver's brand mark, or the robot.
      row.push_back(std::move(chip));
    }
  }
  const int item = shell_->SpacesScaled(rects.chip_size);
  const int gap = shell_->SpacesScaled(kSpaceChipGap);
  const auto width = [&](int pips) {
    return pips <= 0 ? 0 : pips * item + (pips - 1) * gap;
  };
  const int pips = static_cast<int>(row.size());
  // While the create form is up the row holds the dashed placeholder pip
  // (the Mac's creatingSpacePlaceholderPip) as a slot of its own, right
  // after the Space the form opened from — where the new Space lands — or
  // after the last pip once that Space has gone. The slot is laid out like
  // a pip's, so the real pips can never collide with it.
  int placeholder = -1;
  if (creating_ && !rects.horizontal_tabs) {
    placeholder = pips;
    for (int i = 0; i < pips; ++i) {
      if (row[static_cast<size_t>(i)].id == creating_source_) {
        placeholder = i + 1;
        break;
      }
    }
  }
  const int total = pips + (placeholder >= 0 ? 1 : 0);
  // The pip in a slot; the placeholder's slot has none.
  const auto pip_at = [&](int slot) -> PhiSpaceChip* {
    if (slot == placeholder) {
      return nullptr;
    }
    const int index = placeholder >= 0 && slot > placeholder ? slot - 1 : slot;
    return &row[static_cast<size_t>(index)];
  };
  // visiblePipCount: the room left of the trailing slot (`chips_limit`
  // already keeps the gap before it).
  const int budget = rects.chips_limit - rects.chips_left;
  // 65% of a pip (plus its gap) of a hidden neighbour shows past an edge
  // that hides pips — enough of the icon to read; the peeked pip is laid
  // out whole and clipped.
  const int peek = item * 13 / 20 + gap;
  // Whole pips that fit beside ONE peek (the window at either end of the
  // row) and beside both (anywhere between). The Mac reserves both at every
  // position so its animated slide never reflows; this slide is instant, so
  // the window keeps only the peeks it shows and the pips fill the room up
  // to the "…" instead of leaving a blank slot before it.
  int count_edge = total;
  int count_mid = total;
  if (total > 0 && width(total) > budget) {
    overflow_ = true;
    count_edge = total - 1;
    while (count_edge > 1 && width(count_edge) + peek > budget) {
      --count_edge;
    }
    count_mid = count_edge;
    while (count_mid > 1 && width(count_mid) + 2 * peek > budget) {
      --count_mid;
    }
  }
  const int max_start = std::max(0, total - count_edge);
  if (placeholder >= 0) {
    // The viewport keeps the source pip where it stands and scrolls only
    // when the placeholder's slot would fall outside it (the Mac keeps the
    // source in place the same way).
    if (placeholder < start_) {
      start_ = placeholder;
    } else if (placeholder >= start_ + count_mid) {
      start_ = placeholder - count_mid + 1;
    }
  } else if (anchor_space_ != active || anchor_visible_ != count_mid) {
    // ensureActivePipVisible: centre the active pip in the window, clamped
    // at the row's ends so the first/last few pips still fill it. Wheel
    // steps between switches are a manual peek and are left alone.
    anchor_space_ = active;
    anchor_visible_ = count_mid;
    int index = 0;
    for (int i = 0; i < total; ++i) {
      if (row[static_cast<size_t>(i)].id == active) {
        index = i;
        break;
      }
    }
    start_ = std::max(0, std::min(index - count_mid / 2, max_start));
  }
  start_ = std::max(0, std::min(start_, max_start));
  const bool at_edge = start_ == 0 || start_ == max_start;
  const int visible = std::min(at_edge ? count_edge : count_mid, total - start_);
  const int leading_peek = start_ > 0 ? peek : 0;
  const int trailing_peek = start_ + visible < total ? peek : 0;
  const int first = start_ - (leading_peek ? 1 : 0);
  const int end = std::min(total, start_ + visible + (trailing_peek ? 1 : 0));
  // chips_[0]'s row index: a placeholder slot before the window holds no pip.
  first_index_ = first - (placeholder >= 0 && first > placeholder ? 1 : 0);
  const int origin = rects.chips_left + leading_peek - start_ * (item + gap);
  for (int i = first; i < end; ++i) {
    const int x = origin + i * (item + gap);
    const RECT cell = {x, rects.chips_top, x + item, rects.chips_top + item};
    PhiSpaceChip* pip = pip_at(i);
    if (!pip) {
      // The same CELL a real pip gets; the dashed ring is painted inside it
      // at the chip size, exactly as a pip's card is.
      creating_pip_ = cell;
      continue;
    }
    PhiSpaceChip chip = std::move(*pip);
    chip.bounds = cell;
    if (rects.horizontal_tabs) {
      // The Space chip (§5.2): the one chip fills the width the layout
      // measured for its icon, name and chevron.
      chip.bounds.right = std::max(static_cast<int>(chip.bounds.right),
                                   rects.chips_limit);
    }
    chips_.push_back(std::move(chip));
  }
  const int viewport_right =
      rects.chips_left + leading_peek + width(visible) + trailing_peek;
  if (leading_peek || trailing_peek) {
    clip_ = {rects.chips_left, rects.chips_top, viewport_right,
             rects.chips_top + item};
  }
}

int PhiSpacesStrip::ChipAt(POINT client_pt) const {
  for (size_t i = 0; i < chips_.size(); ++i) {
    // A peeked pip is hit only on the part that shows (the Mac's
    // peekHitTarget): its full rect would leak past the clip into the
    // padding.
    if (!::PtInRect(&chips_[i].bounds, client_pt)) {
      continue;
    }
    if (clip_.right <= clip_.left || ::PtInRect(&clip_, client_pt)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

RECT PhiSpacesStrip::ChipBounds(int index) const {
  return index >= 0 && index < static_cast<int>(chips_.size())
             ? chips_[static_cast<size_t>(index)].bounds
             : RECT{};
}

int PhiSpacesStrip::InsertSlotAt(POINT client_pt) const {
  int slot = 0;
  for (const PhiSpaceChip& chip : chips_) {
    if (client_pt.x > (chip.bounds.left + chip.bounds.right) / 2) {
      ++slot;
    }
  }
  return slot;
}

void PhiSpacesStrip::StepViewport(int step) {
  // Purely a manual peek at off-screen pips, clamped at the row's ends: the
  // next Space switch re-anchors the window on the active pip. Ignored
  // mid-drag, where sliding the row under the drop targets would scramble
  // the reorder, and while the create form pins the window to the end.
  if (step == 0 || !overflow_ || shell_->SpacesDragActive() || creating_) {
    return;
  }
  // Rebuild clamps the window to the row's true end; only a step that can
  // never move it is refused here, so the repaint is not wasted.
  if (step < 0 && start_ == 0) {
    return;
  }
  start_ = std::max(0, start_ + step);
  // The pips move under a RESTING pointer: re-hover against the new row now,
  // not at the next paint, so the wash and the hover card follow the pip
  // that is under the cursor rather than the index that used to be.
  Rebuild();
  // A one-pip step often leaves the SAME chips_ index under the cursor with
  // a different pip in it; forget the old hover so the re-hover registers
  // as a change and hands the card over.
  shell_->SpacesForgetHover();
  shell_->SpacesRehoverAtCursor();
  if (const HWND shell = shell_->SpacesShellWindow()) {
    // The pips moved NOW, ahead of any rebuild: the strip repaints itself
    // (the rebuild turn compares against rows that already moved).
    const PhiSpacesStripRects rects = shell_->SpacesRects();
    ::InvalidateRect(shell, &rects.strip_repaint, FALSE);
  }
  shell_->SpacesScheduleRefresh();
}

bool PhiSpacesStrip::OnWheel(int wheel_delta) {
  // Over the strip the wheel slides the pip viewport by whole pips, one per
  // notch (the Mac's SpacesStripWheelTracker): a notch toward the user
  // reveals later pips, the direction a vertical list would scroll.
  StepViewport(wheel_delta < 0 ? 1 : -1);
  return true;
}

// ---------------------------------------------------------------------------
// The trailing "+"'s reveal fade.

void PhiSpacesStrip::SetRowHovered(bool hovered) {
  if (row_hovered_ == hovered) {
    return;
  }
  row_hovered_ = hovered;
  const ULONGLONG now = ::GetTickCount64();
  const ULONGLONG elapsed = fade_start_ ? now - fade_start_ : 0;
  // A flip mid-fade reverses from where the fade stands: the curve is
  // symmetric, so backdating the clock by the leg still to run lands the
  // reversed fade exactly on the current opacity — a quick in-and-out
  // never snaps.
  fade_start_ =
      fade_start_ && elapsed < static_cast<ULONGLONG>(kAddSpaceFadeMs)
          ? now - (static_cast<ULONGLONG>(kAddSpaceFadeMs) - elapsed)
          : now;
  StartFade(kAddSpaceFadeMs -
            static_cast<int>(::GetTickCount64() - fade_start_));
  if (const HWND shell = shell_->SpacesShellWindow()) {
    const RECT button = shell_->SpacesAddSlotDirtyRect();
    ::InvalidateRect(shell, &button, FALSE);
  }
}

void PhiSpacesStrip::StartFade(int ms) {
  PhiAnimator& animator = PhiAnimator::ForThread();
  animator.Cancel(fade_anim_);
  fade_anim_ = 0;
  const HWND shell = shell_->SpacesShellWindow();
  if (!shell) {
    return;
  }
  // The reveal itself is eased by AddSlotRevealProgress() off the clock
  // above; the animation only says WHEN to repaint and WHAT — the trailing
  // slot alone, never the window.
  fade_anim_ = animator.Animate(
      shell, std::max(1, ms), PhiEasing::kLinear,
      [this](double, RECT* dirty) { *dirty = shell_->SpacesAddSlotDirtyRect(); },
      [this] {
        fade_anim_ = 0;
        fade_start_ = 0;
      });
}

double PhiSpacesStrip::AddSlotRevealProgress() const {
  double eased = 1.0;
  if (fade_start_) {
    eased = Smoothstep(
        static_cast<double>(::GetTickCount64() - fade_start_) / kAddSpaceFadeMs);
  }
  return row_hovered_ ? eased : 1.0 - eased;
}

// ---------------------------------------------------------------------------
// The hover card.

void PhiSpacesStrip::UpdateHoverCard(int chip_index) {
  std::string chip_id;
  if (chip_index >= 0 && chip_index < static_cast<int>(chips_.size())) {
    chip_id = chips_[static_cast<size_t>(chip_index)].id;
  }
  // Click suppression means "while the cursor stays on the clicked pip":
  // genuinely moving off it re-arms the card (the Mac's hoverBegan).
  if (!suppressed_card_.empty() && chip_id != suppressed_card_) {
    suppressed_card_.clear();
  }
  if (chip_id.empty() || chip_id == suppressed_card_) {
    DismissHoverCard(chip_id);
    return;
  }
  if (chip_id == card_space_) {
    return;  // Already up for this pip.
  }
  if (!card_space_.empty()) {
    // Warm hand-off pip → pip: swap the card's content instantly, the
    // Mac's already-warm behavior (system tooltips do the same).
    ResolveHoverCard(chip_id);
    card_space_ = chip_id;
    pending_card_.clear();
    return;
  }
  // Cold: show only once the cursor settles. Re-setting the same timer id
  // restarts the countdown as the cursor walks across pips.
  pending_card_ = chip_id;
  if (const HWND shell = shell_->SpacesShellWindow()) {
    ::SetTimer(shell, kHoverCardTimer, kHoverCardDelayMs, nullptr);
  }
}

void PhiSpacesStrip::PromoteHoverCard() {
  const HWND shell = shell_->SpacesShellWindow();
  if (shell) {
    ::KillTimer(shell, kHoverCardTimer);
  }
  if (pending_card_.empty()) {
    return;
  }
  const std::string pending = pending_card_;
  pending_card_.clear();
  // Show only if the cursor is still on the scheduled pip — the Mac gates
  // presentation on the real cursor position for the same reason.
  const int hovered = shell_->SpacesHoveredChip();
  if (hovered < 0 || hovered >= static_cast<int>(chips_.size()) ||
      chips_[static_cast<size_t>(hovered)].id != pending) {
    return;
  }
  ResolveHoverCard(pending);
  card_space_ = pending;
  if (shell) {
    ::InvalidateRect(shell, nullptr, FALSE);
  }
}

void PhiSpacesStrip::DismissHoverCard(const std::string& suppress_space_id) {
  suppressed_card_ = suppress_space_id;
  if (pending_card_.empty() && card_space_.empty()) {
    return;
  }
  pending_card_.clear();
  const HWND shell = shell_->SpacesShellWindow();
  if (shell) {
    ::KillTimer(shell, kHoverCardTimer);
  }
  if (!card_space_.empty()) {
    card_space_.clear();
    if (shell) {
      ::InvalidateRect(shell, nullptr, FALSE);
    }
  }
}

void PhiSpacesStrip::ResolveHoverCard(const std::string& space_id) {
  card_profile_.clear();
  card_shortcut_ = 0;
  if (!spaces_host_) {
    return;
  }
  // The bound profile: the user Space's, or the one the agent Space runs
  // in (the Mac card shows an agent pip's the same way).
  std::string profile_id;
  bool listed = false;
  for (const PhiSpaceView& space : spaces_host_->Spaces()) {
    if (space.id == space_id) {
      profile_id = space.profile_id;
      listed = true;
      break;
    }
  }
  if (!listed && agent_host_) {
    for (const PhiAgentPipView& pip : agent_host_->AgentSpaces(
             shell_->SpacesIsAgent() ? 0 : shell_->SpacesSlot().value)) {
      if (pip.space_id == space_id) {
        profile_id = pip.profile_id;
        listed = true;
        break;
      }
    }
  }
  // The Spaces menu's Ctrl+1..9 binding: the pip's place in strip order,
  // agent pips past the user Spaces (the Mac's spaceSelectionCommand).
  const int position = StripPosition(space_id) + 1;
  if (listed && position <= 9) {
    card_shortcut_ = position;
  }
  // The bound profile's display name, from the same summaries the create
  // form and the tab-area menu read ("id\tname"); the raw id stands in
  // when unnamed.
  card_profile_ = base::UTF8ToWide(profile_id);
  if (model::IsIncognitoSpaceId(space_id)) {
    // Off the record, not the parent profile it nominally names.
    card_profile_ = PhiStr("spaces.builtIn.incognito.singleSpaceName");
    return;
  }
  if (profile_id.empty()) {
    return;
  }
  for (const std::string& summary : spaces_host_->ProfileSummaries()) {
    const size_t tab = summary.find('\t');
    if (summary.substr(0, tab) != profile_id) {
      continue;
    }
    if (tab != std::string::npos && tab + 1 < summary.size()) {
      card_profile_ = base::UTF8ToWide(summary.substr(tab + 1));
    }
    break;
  }
}

// ---------------------------------------------------------------------------
// The create-Space form.

RECT PhiSpacesStrip::FormRect() {
  const PhiSpacesStripRects rects = shell_->SpacesRects();
  return {rects.sidebar.left, rects.strip.bottom, rects.sidebar.right,
          rects.sidebar.bottom};
}

void PhiSpacesStrip::ShowCreateForm() {
  if (!spaces_host_ || !prefs_host_) {
    return;
  }
  // Seeded from the Space the "+" was pressed in: its profile opens the
  // picker, its theme is what the random pick must differ from.
  const std::string& active = shell_->SpacesActiveSpace();
  std::string profile_id = "Default";
  for (const PhiSpaceView& space : spaces_host_->Spaces()) {
    if (space.id == active) {
      profile_id = space.profile_id;
      break;
    }
  }
  std::string theme_id = spaces_host_->SpaceThemeId(active);
  if (theme_id.empty()) {
    theme_id = prefs_host_->ThemeId();
  }
  // CreateSpacePanel.requestCreation: the vertical layouts host the form
  // inline in the docked sidebar. The horizontal layout, a collapsed
  // sidebar (the floating panel does not host it here) and windows without
  // a Spaces strip open the Mac's standalone window instead.
  const RECT form = FormRect();
  if (!shell_->SpacesSidebarCanHostForm(form)) {
    // §5.5's flyout host, hung off the chip / "+" / toggle; the standalone
    // window only where nothing offers an anchor.
    RECT anchor = {};
    if (shell_->SpacesCreateFlyoutAnchor(&anchor)) {
      PhiOpenCreateSpaceFlyout(shell_->SpacesOwner(), anchor, profile_id,
                               theme_id, prefs_host_->Saturation());
    } else {
      PhiCreateSpaceWindow::Open(shell_->SpacesOwner(), profile_id, theme_id,
                                 prefs_host_->Saturation());
    }
    return;
  }
  if (creating_) {
    if (form_) {
      form_->FocusField();
    }
    return;
  }
  if (!form_) {
    form_ = std::make_unique<PhiCreateSpacePanel>(shell_->SpacesOwner());
  }
  creating_ = true;
  creating_source_ = active;  // The placeholder pip stands after this one.
  preview_until_switch_ = false;
  DismissHoverCard(std::string());
  form_->Show(form, profile_id, theme_id, prefs_host_->Saturation());
  shell_->SpacesSetCoveredContentSuppressed(true);  // After the slide-in.
  shell_->SpacesScheduleRefresh();  // The strip reserves the placeholder pip.
  form_->FocusField();
}

void PhiSpacesStrip::CommitCreateForm(const PhiNewSpaceSpec& spec) {
  if (!creating_) {
    return;
  }
  creating_ = false;
  creating_icon_.clear();
  creating_source_.clear();
  if (form_) {
    // Instant: the new Space presents over this window (the Mac's
    // `animated: false` activation after the sidebar flow).
    form_->Hide(/*animated=*/false);
  }
  shell_->SpacesSetCoveredContentSuppressed(false);
  // The previewed theme stays up until the created Space presents, so the
  // chrome never flashes back to the source theme in between.
  preview_until_switch_ = true;
  const std::string id =
      spaces_host_ ? spaces_host_->RequestCreateSpaceWithOptions(
                         shell_->SpacesSlot().value, spec, "sidebar")
                   : std::string();
  if (id.empty()) {
    EndPreview();  // Creation failed: back to the source theme.
  }
  shell_->SpacesFocusContent();
  shell_->SpacesScheduleRefresh();  // The placeholder pip leaves the strip.
}

void PhiSpacesStrip::CancelCreateForm() {
  if (!creating_) {
    return;
  }
  creating_ = false;
  creating_icon_.clear();
  creating_source_.clear();
  if (form_) {
    form_->Hide(/*animated=*/true);
  }
  shell_->SpacesSetCoveredContentSuppressed(false);
  EndPreview();
  shell_->SpacesFocusContent();
  shell_->SpacesScheduleRefresh();  // The placeholder pip leaves the strip.
}

void PhiSpacesStrip::PreviewTheme(const std::string& theme_id, int saturation) {
  if (!prefs_host_ || !creating_) {
    return;
  }
  preview_theme_id_ = theme_id;
  preview_saturation_ = saturation;
  preview_record_.reset();
  RefreshPreview();
  shell_->SpacesApplyPalette();
  shell_->SpacesRefreshTinted();
}

void PhiSpacesStrip::PreviewPalette(const std::string& theme_id,
                                    const PhiThemePalette& palette) {
  if (!prefs_host_ || !creating_) {
    return;
  }
  preview_theme_id_ = theme_id;
  preview_record_ = palette;
  RefreshPreview();
  shell_->SpacesApplyPalette();
  shell_->SpacesRefreshTinted();
}

void PhiSpacesStrip::RefreshPreview() {
  if (!prefs_host_ || preview_theme_id_.empty()) {
    return;
  }
  const PhiTheme& base = PhiThemeById(preview_theme_id_);
  const PhiAppearance appearance = prefs_host_->Appearance();
  preview_palette_ =
      preview_record_
          ? PhiResolvePalette(base, appearance, *preview_record_)
          : PhiResolvePalette(base, appearance, preview_saturation_);
  if (form_) {
    form_->Refresh();
  }
}

void PhiSpacesStrip::EndPreview() {
  if (!preview_palette_ && !preview_until_switch_) {
    return;
  }
  preview_palette_.reset();
  preview_theme_id_.clear();
  preview_record_.reset();
  preview_until_switch_ = false;
  shell_->SpacesApplyPalette();
  shell_->SpacesRefreshTinted();
}

void PhiSpacesStrip::SetFormIcon(const std::string& storage_value) {
  creating_icon_ = base::UTF8ToWide(PhiIconDisplayText(storage_value));
  if (const HWND shell = shell_->SpacesShellWindow()) {
    ::InvalidateRect(shell, nullptr, FALSE);
  }
}

void PhiSpacesStrip::OnFormProfileCreated(const std::string& profile_id) {
  if (creating_ && form_) {
    form_->OnProfileCreated(profile_id);
  }
}

void PhiSpacesStrip::RepositionForm() {
  if (creating_ && form_) {
    form_->Reposition(FormRect());
  }
}

// ---------------------------------------------------------------------------
// The Space switch.

double PhiSpacesStrip::SwapProgress() const {
  if (!swap_start_) {
    return 1.0;
  }
  return Smoothstep(static_cast<double>(::GetTickCount64() - swap_start_) /
                    kSpaceSwapMs);
}

PhiPalette PhiSpacesStrip::RampPalette(const PhiPalette& target) const {
  if (preview_palette_) {
    return *preview_palette_;  // The create form's live theme preview.
  }
  if (!ramp_start_) {
    return target;
  }
  const double t = Smoothstep(
      static_cast<double>(::GetTickCount64() - ramp_start_) / kThemeRampMs);
  return t < 1.0 ? LerpPalette(swap_source_palette_, target, t) : target;
}

void PhiSpacesStrip::BeginSwap(const std::string& entering_space_id) {
  const std::string& active = shell_->SpacesActiveSpace();
  if (!spaces_host_ || !prefs_host_ ||
      prefs_host_->LayoutMode() == PhiLayoutMode::kComfortable ||
      entering_space_id == active) {
    return;
  }
  CancelSwap();  // Settle any in-flight switch first.
  // Direction from strip order, the Mac's forward/backward semantics.
  swap_forward_ = SlideForward(entering_space_id);
  swap_source_palette_ = shell_->SpacesPalette();
  // The strip's selected pill slides from the leaving pip to the entering
  // one on the same clock; remember WHICH Space it leaves (the active one
  // is still the leaving one here) and let the paint resolve the rect, so
  // the slide survives the viewport re-centring the switch triggers.
  swap_from_space_ = active;
  // Retain the leaving Space's laid-out rows: the slide paints them LIVE
  // (the rebuild refills the working vectors with the entering Space's).
  shell_->SpacesDropLeavingBand();
  shell_->SpacesSwapBandModel();
  swap_start_ = ramp_start_ = ::GetTickCount64();

  const HWND shell = shell_->SpacesShellWindow();
  if (!shell) {
    return;
  }
  PhiAnimator& animator = PhiAnimator::ForThread();
  // Two clocks, two dirty rects: the band slide moves the sidebar column
  // (and the selected pill riding in its strip), the theme ramp retints the
  // whole chrome — which is why its dirty rect IS the client area, and why
  // the corner caps and the address EDIT are re-tinted with each of its
  // ticks.
  swap_anim_ = animator.Animate(
      shell, kSpaceSwapMs, PhiEasing::kLinear,
      [this](double, RECT* dirty) {
        const PhiSpacesStripRects rects = shell_->SpacesRects();
        *dirty = rects.sidebar;
        ::UnionRect(dirty, dirty, &rects.strip);
      },
      [this] { swap_anim_ = 0; });
  ramp_anim_ = animator.Animate(
      shell, kThemeRampMs, PhiEasing::kLinear,
      [this](double, RECT* dirty) {
        *dirty = shell_->SpacesRects().client;
        shell_->SpacesRefreshTinted();
        shell_->SpacesRepaintOmnibox();  // The EDIT rides the ramp too.
      },
      [this] {
        ramp_anim_ = 0;
        ramp_start_ = 0;
      });
}

bool PhiSpacesStrip::SlideForward(
    const std::string& entering_space_id) const {
  // An agent Space is past every user Space in strip order (the Mac sorts
  // them to the end): a switch into one slides forward, back out of one
  // slides backward, and between two of them the ordinal decides.
  return StripPosition(entering_space_id) >=
         StripPosition(shell_->SpacesActiveSpace());
}

int PhiSpacesStrip::StripPosition(const std::string& space_id) const {
  int at = 0;
  if (spaces_host_) {
    for (const PhiSpaceView& space : spaces_host_->Spaces()) {
      if (space.id == space_id) {
        return at;
      }
      ++at;
    }
  }
  if (agent_host_) {
    for (const PhiAgentPipView& pip : agent_host_->AgentSpaces(
             shell_->SpacesIsAgent() ? 0 : shell_->SpacesSlot().value)) {
      if (pip.space_id == space_id) {
        return at;
      }
      ++at;
    }
  }
  return at;  // Not listed: past the end.
}

void PhiSpacesStrip::NoteSlideLanded() {
  shell_->SpacesDropLeavingBand();
  swap_start_ = 0;
  swap_from_space_.clear();
  PhiAnimator::ForThread().Cancel(swap_anim_);
  swap_anim_ = 0;
  shell_->SpacesRevealSwappedContent();  // The page flips only now.
}

void PhiSpacesStrip::CancelSwap() {
  shell_->SpacesRevealSwappedContent();  // Aborted switches settle instantly.
  shell_->SpacesDropLeavingBand();
  swap_from_space_.clear();
  PhiAnimator& animator = PhiAnimator::ForThread();
  animator.Cancel(swap_anim_);
  animator.Cancel(ramp_anim_);
  swap_anim_ = ramp_anim_ = 0;
  swap_start_ = 0;
  ramp_start_ = 0;
  shell_->SpacesRepaintOmnibox();  // The palette jumped to its target.
}

RECT PhiSpacesStrip::ViewportRect(
    const PhiSpacesStripRects& rects) const {
  RECT view = {rects.chips_left, rects.strip.top, rects.chips_limit,
               rects.strip.bottom};
  if (clip_.right > clip_.left) {
    view.left = clip_.left;
    view.right = clip_.right;
  }
  return view;
}

bool PhiSpacesStrip::ChipShown(const std::string& space_id) const {
  for (const PhiSpaceChip& chip : chips_) {
    if (chip.id == space_id) {
      return true;
    }
  }
  return false;
}

RECT PhiSpacesStrip::SwapPillOrigin(
    const RECT& entering, const PhiSpacesStripRects& rects) const {
  // The leaving pip is still on screen: the pill starts on it, wherever the
  // viewport has since put it.
  for (const PhiSpaceChip& chip : chips_) {
    if (chip.id == swap_from_space_) {
      return PaintedChip(chip.bounds, shell_->SpacesScaled(kSpaceChip));
    }
  }
  // It scrolled out of the windowed row (or is no longer listed): the pill
  // cannot start on it, so it starts just past the viewport edge it lies
  // behind and slides in from there — the switch still reads as coming from
  // the source's direction instead of blinking into place. Forward means the
  // entering Space is LATER in strip order, so the source is behind the
  // leading edge; backward, behind the trailing one.
  const int gap = shell_->SpacesScaled(kSpaceChipGap);
  const RECT view = ViewportRect(rects);
  RECT from = entering;
  const LONG width = entering.right - entering.left;
  if (swap_forward_) {
    from.right = view.left - gap;
    from.left = from.right - width;
  } else {
    from.left = view.right + gap;
    from.right = from.left + width;
  }
  return from;
}

// ---------------------------------------------------------------------------
// Paint.

void PhiSpacesStrip::Paint(HDC dc,
                           const PhiSpacesStripRects& rects,
                           const PhiPalette& palette) {
  const COLORREF kRowSelectedBg = palette.selected;
  const COLORREF kLabel = palette.label;
  const COLORREF kLabelDim = palette.label_dim;
  const COLORREF kBorder = palette.border;
  // The active pill's 1pt outline exists only in dark mode (white 20%).
  const COLORREF kActivePillBorder =
      palette.dark ? BlendOver(RGB(0xFF, 0xFF, 0xFF), kRowSelectedBg, 51)
                   : kRowSelectedBg;
  const auto scaled = [this](int units) { return shell_->SpacesScaled(units); };
  const std::string& active_space = shell_->SpacesActiveSpace();
  const int hovered_chip = shell_->SpacesHoveredChip();

  // The selected pill first, as a background layer (the Mac's strip-level
  // glass chip sits BEHIND the pips): mid-switch it slides across
  // neighboring pips' rects on the band slide's clock, and their glyphs
  // must draw over it, not vanish under it. The pips themselves stay put.
  // The hairline stands in for the Mac chip's drop shadow — without it the
  // white pill disappears on a white ground.
  // An overflowing row paints through the viewport clip: the peeked
  // neighbours show partly, the cue that the row continues.
  // A pill sliding in from a scrolled-out source starts OUTSIDE the pips'
  // room, so clip to that room even where the row itself is not windowed:
  // the pill has to emerge from the edge, not paint over the trailing slot
  // or the padding beside it.
  const bool swap_from_offscreen =
      swap_start_ && !swap_from_space_.empty() && !ChipShown(swap_from_space_);
  const int clip_save = ::SaveDC(dc);
  if (clip_.right > clip_.left || swap_from_offscreen) {
    const RECT view = ViewportRect(rects);
    ::IntersectClipRect(dc, view.left, view.top, view.right, view.bottom);
  }
  for (const PhiSpaceChip& chip : chips_) {
    // A watched agent pip wears the selection pill too, so "which agent am I
    // watching" reads at a glance.
    if (chip.id != active_space && !(chip.is_agent && chip.open)) {
      continue;
    }
    if (shell_->SpacesIsAgent() && !chip.is_agent) {
      // In an agent window the agent Space IS the active one: the user
      // Space the slot came from wears no pill there (the Mac's strip).
      continue;
    }
    if (creating_ && !chip.is_agent) {
      // The dashed placeholder is the Space in focus while the form is up;
      // the source Space must not keep reading as active beside it.
      continue;
    }
    RECT pill = PaintedChip(chip.bounds, scaled(kSpaceChip));
    if (!chip.is_agent && swap_start_ && !swap_from_space_.empty()) {
      const double t = SwapProgress();
      const auto mix = [t](LONG from, LONG to) {
        return from + static_cast<LONG>(std::lround((to - from) * t));
      };
      const RECT to = PaintedChip(chip.bounds, scaled(kSpaceChip));
      const RECT from = SwapPillOrigin(to, rects);
      pill.left = mix(from.left, to.left);
      pill.top = mix(from.top, to.top);
      pill.right = mix(from.right, to.right);
      pill.bottom = mix(from.bottom, to.bottom);
    }
    StrokeRounded(dc, pill, kRowSelectedBg,
                  palette.dark ? kActivePillBorder : kBorder,
                  scaled(kSpaceCardRadius));
  }
  if (rects.horizontal_tabs) {
    // The Space chip (§5.2): a 28 dip card tile, 1 px stroke, the icon
    // (16) + name (12/600, ellipsised) + a chevron; the hover wash over it.
    EnsureCardFonts();
    for (const PhiSpaceChip& chip : chips_) {
      const RECT tile = chip.bounds;
      const bool hot = hovered_chip == static_cast<int>(&chip - chips_.data());
      StrokeRounded(dc, tile, hot ? BlendOver(palette.ink, palette.card, 11)
                                  : palette.card,
                    kBorder, scaled(kSpaceCardRadius));
      const int icon_px = scaled(16);
      RECT icon_rc = {tile.left + scaled(8), (tile.top + tile.bottom - icon_px) / 2,
                      tile.left + scaled(8) + icon_px,
                      (tile.top + tile.bottom + icon_px) / 2};
      if (!chip.icon.empty() &&
          PhiDrawCatalogIcon(dc, icon_rc, chip.icon, icon_px, kLabel)) {
      } else if (!chip.icon.empty() &&
                 PhiDrawColorIcon(dc, icon_rc, chip.icon, icon_px, kLabel)) {
      } else {
        ::SetTextColor(dc, kLabel);
        const std::wstring initial =
            chip.label.empty() ? std::wstring(L"?") : chip.label.substr(0, 1);
        PhiDrawTextAA(dc, initial.c_str(), -1, &icon_rc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      }
      const int chevron_px = scaled(12);
      RECT chevron_rc = {tile.right - scaled(8) - chevron_px, tile.top,
                         tile.right - scaled(8), tile.bottom};
      PhiDrawIconGlyph(dc, chevron_rc, fluent::kChevronDown, chevron_px,
                       kLabelDim);
      RECT name_rc = {icon_rc.right + scaled(6), tile.top,
                      chevron_rc.left - scaled(4), tile.bottom};
      HGDIOBJ previous_name_font =
          card_name_font_ ? ::SelectObject(dc, card_name_font_) : nullptr;
      ::SetTextColor(dc, kLabel);
      PhiDrawTextAA(dc, chip.label.c_str(), -1, &name_rc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
      if (previous_name_font) {
        ::SelectObject(dc, previous_name_font);
      }
    }
    ::RestoreDC(dc, clip_save);
    return;
  }
  for (const PhiSpaceChip& chip : chips_) {
    const bool active = (chip.id == active_space);
    if (!active && hovered_chip == static_cast<int>(&chip - chips_.data())) {
      // The Mac's dim hover wash on an inactive pip (sidebarTabHovered).
      FillRounded(dc, PaintedChip(chip.bounds, scaled(kSpaceChip)),
                  palette.hover, scaled(kSpaceCardRadius));
    }
    // Every pip's icon is the label colour, active or not — the Mac tints
    // them all Color.primary and lets the selected chip alone say which one
    // is current. Only the trailing slot's "+" / "…" is secondary.
    ::SetTextColor(dc, kLabel);
    // The vertical layouts' pip fills the strip band; its glyph grows with
    // it, at the same proportion the 24pt pip gives its own.
    const bool big_pip = rects.chip_size >= kSpaceChipVertical;
    const int icon_pt = big_pip ? kSpaceIconVertical : kSpaceIcon;
    // The glyph box is INSET FROM THE PAINTED CHIP, not from the cell: the
    // colour pass repaints its whole rect from a pixel sampled at the
    // rect's corner, so a box reaching the chip's own corners would sample
    // the ground tint outside the rounded corner and clear the card away —
    // and, where it did survive, square off the corners it covered. 3pt
    // keeps the sample inside the radius-8 arc with room for the icon.
    RECT glyph_rc = PaintedChip(chip.bounds, scaled(kSpaceChip));
    ::InflateRect(&glyph_rc, -scaled(3), -scaled(3));
    // Colour emoji take the same box: the colour pass centres their INK in
    // it, so the old hand-tuned lift (they ride low in their line box) is
    // gone — it left the repainted patch sitting higher than the pip it
    // belongs to.
    if (!chip.icon.empty() &&
        PhiDrawCatalogIcon(dc, glyph_rc, chip.icon, scaled(icon_pt), kLabel)) {
      // The Mac catalog's own artwork, tinted like the glyph would be.
    } else if (!chip.icon.empty() && !PhiIsPrivateGlyph(chip.icon) &&
               PhiDrawColorIcon(dc, glyph_rc, chip.icon, scaled(icon_pt),
                                kLabel)) {
      // Emoji rendered in COLOUR via DirectWrite; GDI below is the fallback.
    } else if (!chip.icon.empty() &&
               (shell_->SpacesEmojiFont() || shell_->SpacesChipIconFont())) {
      // A private-use glyph is one of the Phi icon catalog's MDL2 stand-ins
      // (one step above the emoji, so the two align); everything else emoji.
      // Each face comes in both pip sizes; the small one stands in if the
      // big one failed to build.
      HFONT icon_font =
          PhiIsPrivateGlyph(chip.icon)
              ? (big_pip && shell_->SpacesPipIconFont()
                     ? shell_->SpacesPipIconFont()
                     : shell_->SpacesChipIconFont())
              : (big_pip && shell_->SpacesPipEmojiFont()
                     ? shell_->SpacesPipEmojiFont()
                     : shell_->SpacesEmojiFont());
      HGDIOBJ previous_icon_font =
          icon_font ? ::SelectObject(dc, icon_font) : nullptr;
      PhiDrawTextAA(dc, chip.icon.c_str(), -1, &glyph_rc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      if (previous_icon_font) {
        ::SelectObject(dc, previous_icon_font);
      }
    } else {
      // No icon: the name's first character stands in (the pip has no room
      // for the name itself). It rides the UI font, so the big pip needs the
      // strip's own text font to keep pace with its glyphs.
      const std::wstring initial =
          chip.label.empty() ? std::wstring(L"?") : chip.label.substr(0, 1);
      HGDIOBJ previous_initial_font =
          big_pip && shell_->SpacesPipTextFont()
              ? ::SelectObject(dc, shell_->SpacesPipTextFont())
              : nullptr;
      PhiDrawTextAA(dc, initial.c_str(), -1, const_cast<RECT*>(&chip.bounds),
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      if (previous_initial_font) {
        ::SelectObject(dc, previous_initial_font);
      }
    }
    if (chip.is_agent) {
      // The Mac's agent grouping: a 1×14 rule in the gap before the first
      // agent pip, and the corner badge — red on an error the user has not
      // seen, green tick when completed, orange when the user holds it,
      // grey when idle, green while running. 8pt, inside the pip frame.
      const RECT pip_rc = PaintedChip(chip.bounds, scaled(kSpaceChip));
      const size_t index = static_cast<size_t>(&chip - chips_.data());
      if (index > 0 && !chips_[index - 1].is_agent) {
        const int mid = (pip_rc.top + pip_rc.bottom) / 2;
        const int half = scaled(7);
        const RECT rule = {chip.bounds.left - scaled(kSpaceChipGap) / 2 - 1,
                           mid - half, chip.bounds.left -
                                           scaled(kSpaceChipGap) / 2,
                           mid + half};
        HBRUSH rule_brush =
            ::CreateSolidBrush(BlendOver(kLabel, palette.ground_top, 64));
        ::FillRect(dc, &rule, rule_brush);
        ::DeleteObject(rule_brush);
      }
      COLORREF badge = RGB(0x34, 0xC7, 0x59);  // running: systemGreen
      switch (chip.badge - 1) {
        case 1:  // idle
          badge = RGB(0x8E, 0x8E, 0x93);
          break;
        case 2:  // the user holds control
          badge = RGB(0xFF, 0x95, 0x00);
          break;
        case 3:  // completed
          badge = RGB(0x34, 0xC7, 0x59);
          break;
        case 4:  // error
          badge = RGB(0xFF, 0x3B, 0x30);
          break;
        default:
          break;
      }
      RECT dot = {pip_rc.right - scaled(10), pip_rc.bottom - scaled(10),
                  pip_rc.right - scaled(1), pip_rc.bottom - scaled(1)};
      // A ground-coloured ring lifts the dot off the pip, the Mac's
      // shadowed badge.
      FillRounded(dc, dot, palette.ground_top, scaled(5));
      ::InflateRect(&dot, -scaled(1), -scaled(1));
      FillRounded(dc, dot, badge, scaled(4));
    }
  }
  ::RestoreDC(dc, clip_save);
  if (creating_ && creating_pip_.right > creating_pip_.left) {
    // The Mac's creatingSpacePlaceholderPip: a dashed outline where the new
    // Space will land, carrying the form's currently selected icon.
    const RECT pip = PaintedChip(creating_pip_, scaled(kSpaceChip));
    PhiStrokeRoundRectDashedAA(dc, pip, scaled(kCornerRadius),
                               BlendOver(kLabelDim, palette.ground_top, 166), 1,
                               scaled(3), scaled(2));
    if (!creating_icon_.empty()) {
      // Sized like a real pip's icon, through the same ink-fitting draws:
      // the old fixed-point artwork/text path rendered the chosen icon
      // visibly smaller than the pips it stands among.
      const bool big_pip = rects.chip_size >= kSpaceChipVertical;
      const int icon_pt = big_pip ? kSpaceIconVertical : kSpaceIcon;
      RECT glyph_rc = pip;
      ::InflateRect(&glyph_rc, -scaled(3), -scaled(3));
      if (PhiDrawCatalogIcon(dc, glyph_rc, creating_icon_, scaled(icon_pt),
                             kLabel)) {
        // The catalog's own artwork.
      } else if (!PhiIsPrivateGlyph(creating_icon_) &&
                 PhiDrawColorIcon(dc, glyph_rc, creating_icon_, scaled(icon_pt),
                                  kLabel)) {
        // Colour emoji.
      } else if (shell_->SpacesEmojiFont() || shell_->SpacesChipIconFont()) {
        HFONT pip_icon_font =
            PhiIsPrivateGlyph(creating_icon_)
                ? (big_pip && shell_->SpacesPipIconFont()
                       ? shell_->SpacesPipIconFont()
                       : shell_->SpacesChipIconFont())
                : (big_pip && shell_->SpacesPipEmojiFont()
                       ? shell_->SpacesPipEmojiFont()
                       : shell_->SpacesEmojiFont());
        HGDIOBJ previous_pip_font =
            pip_icon_font ? ::SelectObject(dc, pip_icon_font) : nullptr;
        ::SetTextColor(dc, kLabel);
        PhiDrawTextAA(dc, creating_icon_.c_str(), -1, &glyph_rc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (previous_pip_font) {
          ::SelectObject(dc, previous_pip_font);
        }
      }
    }
  }
  if (!shell_->SpacesIncognito() &&
      rects.add_space.right > rects.add_space.left) {
    // The Mac trailing slot: "+" creates; on overflow it becomes the "…"
    // switcher (moreButton) since not every pip is visible. The "+" is up
    // only while the pointer is over the strip's row (addButton's opacity
    // rides slot.isStripRowHovered, 0.15s easeInOut); the "…" always is.
    // Whichever shows wears the pips' hover wash (radius 6) under the
    // cursor. GDI has no opacity: the fade blends toward the ground the
    // slot sits on, sampled from its corner — nothing else paints there.
    const double reveal = overflow_ ? 1.0 : AddSlotRevealProgress();
    if (reveal > 0.0) {
      COLORREF ground =
          ::GetPixel(dc, rects.add_space.left, rects.add_space.top);
      if (ground == CLR_INVALID) {
        ground = palette.ground_top;
      }
      const int alpha = static_cast<int>(std::lround(reveal * 255.0));
      if (shell_->SpacesAddSlotHovered()) {
        FillRounded(dc, rects.add_space,
                    BlendOver(palette.hover, ground, alpha), scaled(6));
      }
      const COLORREF slot_ink = BlendOver(kLabelDim, ground, alpha);
      if (overflow_) {
        // The Mac's moreButton is SF Symbols' `ellipsis` — three ROUND dots
        // at the icon size. A text "…" is not that: its dots are small,
        // square-cornered and sit on the baseline, which reads as a typo
        // beside the icons. Three circles, 3pt across on a 5pt pitch,
        // centred in the slot.
        const int dot = scaled(3);
        const int pitch = scaled(5);
        const int mid_x = (rects.add_space.left + rects.add_space.right) / 2;
        const int mid_y = (rects.add_space.top + rects.add_space.bottom) / 2;
        for (int i = -1; i <= 1; ++i) {
          const RECT d = {mid_x + i * pitch - dot / 2, mid_y - dot / 2,
                          mid_x + i * pitch - dot / 2 + dot,
                          mid_y - dot / 2 + dot};
          PhiFillEllipseAA(dc, d, slot_ink);
        }
      } else {
        HGDIOBJ previous_slot_font =
            rects.chip_size >= kSpaceChipVertical && shell_->SpacesPipTextFont()
                ? ::SelectObject(dc, shell_->SpacesPipTextFont())
                : nullptr;
        DrawGlyph(dc, rects.add_space, L"+", slot_ink);
        if (previous_slot_font) {
          ::SelectObject(dc, previous_slot_font);
        }
      }
    }
  }
}

// A hovered pip's info card (the Mac's SpaceHoverCard): the Space as a
// tinted capsule (icon + name), the bound profile, and the Ctrl+N switch
// shortcut as keycaps, floating just above the pip on a bordered card. The
// header above the strip is all shell-drawn — no child window clips it —
// which is why the card anchors UPWARD, like the Mac panel.
void PhiSpacesStrip::EnsureCardFonts() {
  const UINT dpi = static_cast<UINT>(shell_->SpacesScaled(96));
  if (card_fonts_dpi_ == dpi && card_name_font_) {
    return;
  }
  ReleaseCardFonts();
  card_name_font_ = PhiFluentFont(dpi, 12, FW_SEMIBOLD);
  card_hint_font_ = PhiFluentFont(dpi, 11);
  card_key_font_ = PhiFluentFont(dpi, 10);
  card_fonts_dpi_ = dpi;
}

void PhiSpacesStrip::ReleaseCardFonts() {
  for (HFONT* font : {&card_name_font_, &card_hint_font_, &card_key_font_}) {
    if (*font) {
      ::DeleteObject(*font);
      *font = nullptr;
    }
  }
  card_fonts_dpi_ = 0;
}

bool PhiSpacesStrip::MeasureHoverCard(HDC dc,
                                      const PhiSpacesStripRects& rects,
                                      CardGeometry* out) {
  if (card_space_.empty() || shell_->SpacesDragActive()) {
    return false;
  }
  const PhiSpaceChip* chip = nullptr;
  for (const PhiSpaceChip& c : chips_) {
    if (c.id == card_space_) {
      chip = &c;
      break;
    }
  }
  if (!chip) {
    return false;  // The Space left the strip; the next hover change prunes it.
  }
  EnsureCardFonts();
  const auto scaled = [this](int units) { return shell_->SpacesScaled(units); };
  CardGeometry g;
  g.icon = chip->icon;
  g.agent = chip->is_agent;
  g.badge = chip->badge;
  g.name_text = chip->label.empty() ? L"Space" : chip->label;
  // Line 2: "<status> · " for an agent, then "profile · N tabs".
  std::wstring hint;
  if (chip->is_agent) {
    const char* key = "spaces.hoverCard.status.running";
    switch (chip->badge - 1) {
      case 1: key = "spaces.hoverCard.status.idle"; break;
      case 2: key = "spaces.hoverCard.status.held"; break;
      case 3: key = "spaces.hoverCard.status.completed"; break;
      case 4: key = "spaces.hoverCard.status.error"; break;
      default: break;
    }
    hint = PhiStr(key);
    hint += L" · ";
  }
  hint += card_profile_.empty() ? L"Default" : card_profile_;
  if (const int tabs = shell_->SpacesTabCount(chip->id); tabs > 0) {
    hint += L" · " + PhiStrN("spaces.hoverCard.tabs", tabs, {PhiFormatNumber(tabs)});
  }
  g.hint_text = hint;
  if (card_shortcut_ > 0) {
    g.keycap_text = L"Ctrl+" + std::to_wstring(card_shortcut_);
  }

  const auto measure = [&](HFONT font, const std::wstring& text) {
    HGDIOBJ previous = font ? ::SelectObject(dc, font) : nullptr;
    RECT rc = {0, 0, 0, 0};
    PhiDrawTextAA(dc, text.c_str(), -1, &rc,
                  DT_CALCRECT | DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);
    if (previous) {
      ::SelectObject(dc, previous);
    }
    return static_cast<int>(rc.right);
  };
  // Padding 8 × 10, the 20 dip tile, 8 to the text, the keycap (20 high)
  // trailing after 12; min width 200, max 280 — the name ellipsises first.
  const int pad_x = scaled(10);
  const int pad_y = scaled(8);
  const int tile = scaled(20);
  const int gap = scaled(8);
  const int key_w =
      g.keycap_text.empty() ? 0 : measure(card_key_font_, g.keycap_text) + scaled(12);
  const int name_w = measure(card_name_font_, g.name_text);
  const int hint_w = measure(card_hint_font_, g.hint_text);
  const int text_x = pad_x + tile + gap;
  const int min_w = scaled(200);
  const int max_w = std::min(scaled(280),
                             static_cast<int>(rects.client.right) - scaled(16));
  int card_w = text_x + std::max(name_w + (key_w ? scaled(12) + key_w : 0), hint_w) + pad_x;
  card_w = std::max(min_w, std::min(max_w, card_w));
  const int line1 = scaled(20);
  const int line2 = scaled(16);
  const int card_h = pad_y + line1 + scaled(2) + line2 + pad_y;

  // Centred under the pip, 4 below the row, clamped 8 from the window's
  // edges; always BELOW the strip, so it never covers the row.
  const int mid_x = (chip->bounds.left + chip->bounds.right) / 2;
  int left = mid_x - card_w / 2;
  left = std::min(left, static_cast<int>(rects.client.right) - scaled(8) - card_w);
  left = std::max(left, scaled(8));
  int top = std::max(static_cast<int>(chip->bounds.bottom),
                     static_cast<int>(rects.strip.bottom)) +
            scaled(4);
  top = std::min(top, static_cast<int>(rects.client.bottom) - scaled(8) - card_h);
  g.card = {left, top, left + card_w, top + card_h};
  g.tile = {left + pad_x, top + pad_y, left + pad_x + tile, top + pad_y + tile};
  RECT key = {};
  if (key_w) {
    key = {g.card.right - pad_x - key_w, top + pad_y, g.card.right - pad_x,
           top + pad_y + line1};
  }
  g.keycap = key;
  g.name = {left + text_x, top + pad_y,
            (key_w ? key.left - scaled(12) : g.card.right - pad_x),
            top + pad_y + line1};
  g.hint = {left + text_x, g.name.bottom + scaled(2), g.card.right - pad_x,
            g.name.bottom + scaled(2) + line2};
  *out = std::move(g);
  return true;
}

void PhiSpacesStrip::PaintHoverCard(HDC dc,
                                    const CardGeometry& g,
                                    const PhiPalette& palette) {
  const auto scaled = [this](int units) { return shell_->SpacesScaled(units); };
  EnsureCardFonts();
  const COLORREF ground = palette.flyout;
  StrokeRounded(dc, g.card, ground, palette.border, scaled(kHoverCardRadius));

  // Neutral throughout (the Mac card carries no Space tint): the icon in an
  // ink 8 % tile, radius 4.
  FillRounded(dc, g.tile, BlendOver(palette.ink, ground, 20), scaled(4));
  RECT icon_rc = g.tile;
  ::InflateRect(&icon_rc, -scaled(2), -scaled(2));
  const COLORREF icon_ink = palette.label;
  if (!g.icon.empty() &&
      PhiDrawCatalogIcon(dc, icon_rc, g.icon, scaled(14), icon_ink)) {
  } else if (!g.icon.empty() && !PhiIsPrivateGlyph(g.icon) &&
             PhiDrawColorIcon(dc, icon_rc, g.icon, scaled(14), icon_ink)) {
  } else {
    ::SetTextColor(dc, icon_ink);
    const std::wstring initial = g.name_text.substr(0, 1);
    HGDIOBJ previous = card_name_font_ ? ::SelectObject(dc, card_name_font_) : nullptr;
    PhiDrawTextAA(dc, initial.c_str(), -1, &icon_rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (previous) {
      ::SelectObject(dc, previous);
    }
  }
  // Line 1: the name, 12/600 in `label`; the keycap trailing.
  HGDIOBJ previous_font = card_name_font_ ? ::SelectObject(dc, card_name_font_) : nullptr;
  ::SetTextColor(dc, palette.label);
  RECT name_rc = g.name;
  PhiDrawTextAA(dc, g.name_text.c_str(), -1, &name_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  if (g.keycap.right > g.keycap.left) {
    // The keycap on the same ink 8 % wash as the tile.
    StrokeRounded(dc, g.keycap, BlendOver(palette.ink, ground, 20),
                  palette.stroke_strong, scaled(4));
    if (card_key_font_) {
      ::SelectObject(dc, card_key_font_);
    }
    ::SetTextColor(dc, palette.label_dim);
    RECT key_rc = g.keycap;
    PhiDrawTextAA(dc, g.keycap_text.c_str(), -1, &key_rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }
  // Line 2: 11 dip `dim`; an agent's leads with its status dot.
  if (card_hint_font_) {
    ::SelectObject(dc, card_hint_font_);
  }
  ::SetTextColor(dc, palette.label_dim);
  RECT hint_rc = g.hint;
  if (g.agent) {
    const int dot = scaled(7);
    const COLORREF status =
        g.badge == 2   ? palette.label_tertiary                 // Idle.
        : g.badge == 3 ? RGB(0xF7, 0x63, 0x0C)                  // Held.
        : g.badge == 4 ? RGB(0x0F, 0x7B, 0x0F)                  // Completed.
        : g.badge == 5 ? palette.destructive                    // Error.
                       : RGB(0x0F, 0x7B, 0x0F);                 // Running.
    RECT dot_rc = {hint_rc.left, (hint_rc.top + hint_rc.bottom - dot) / 2,
                   hint_rc.left + dot, (hint_rc.top + hint_rc.bottom + dot) / 2};
    PhiFillEllipseAA(dc, dot_rc, status);
    hint_rc.left += dot + scaled(5);
  }
  PhiDrawTextAA(dc, g.hint_text.c_str(), -1, &hint_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  if (previous_font) {
    ::SelectObject(dc, previous_font);
  }
}

void PhiSpacesStrip::SyncHoverCardPopup(HDC dc,
                                        const PhiSpacesStripRects& rects) {
  const HWND shell = shell_->SpacesShellWindow();
  CardGeometry g;
  if (!shell || !MeasureHoverCard(dc, rects, &g)) {
    if (card_popup_ && ::IsWindowVisible(card_popup_)) {
      ::ShowWindow(card_popup_, SW_HIDE);
    }
    return;
  }
  // Layout → screen. The floating sidebar's panel shows the expanded layout
  // from its own offset down, at its own origin.
  POINT origin = {g.card.left, g.card.top};
  int floating_offset = 0;
  if (const HWND panel = shell_->SpacesFloatingPanel(&floating_offset)) {
    origin.y -= floating_offset;
    ::ClientToScreen(panel, &origin);
  } else {
    ::ClientToScreen(shell, &origin);
  }
  const int w = g.card.right - g.card.left;
  const int h = g.card.bottom - g.card.top;
  const RECT screen = {origin.x, origin.y, origin.x + w, origin.y + h};
  if (!card_popup_) {
    static const ATOM atom = [] {
      WNDCLASSW cls = {};
      cls.lpfnWndProc = &PhiSpacesStrip::HoverCardPopupProc;
      cls.hInstance = ::GetModuleHandleW(nullptr);
      cls.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
      cls.lpszClassName = L"PhiSpaceHoverCard";
      return ::RegisterClassW(&cls);
    }();
    (void)atom;
    // Owned by the shell, so it stays above the shell and its children
    // without WS_EX_TOPMOST; transparent to the pointer, so it never takes
    // the hover it is showing for.
    // Layered: the card's corners are alpha coverage (antialiased at any
    // radius), where a window region cuts them on the pixel grid and DWM's
    // own rounding is fixed at the pills' 8.
    card_popup_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
        L"PhiSpaceHoverCard", L"", WS_POPUP, 0, 0, 0, 0, shell, nullptr,
        ::GetModuleHandleW(nullptr), this);
    if (!card_popup_) {
      return;
    }
  }
  // Painted at the popup's own origin: every rect re-based on the card.
  for (RECT* rc : {&g.tile, &g.name, &g.hint, &g.keycap}) {
    if (rc->right > rc->left) {
      ::OffsetRect(rc, -g.card.left, -g.card.top);
    }
  }
  g.card = {0, 0, w, h};
  card_geom_ = std::move(g);
  const bool moved = !::EqualRect(&screen, &card_popup_rect_);
  const int r = shell_->SpacesScaled(kHoverCardRadius);
  const bool layered =
      (::GetWindowLongPtrW(card_popup_, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
  if (moved && !layered) {
    // The region path: the corners cut on the pixel grid.
    ::SetWindowRgn(card_popup_,
                   ::CreateRoundRectRgn(0, 0, w + 1, h + 1, 2 * r, 2 * r),
                   FALSE);
  }
  if (moved) {
    card_popup_rect_ = screen;
  }
  if (moved || !::IsWindowVisible(card_popup_)) {
    ::SetWindowPos(card_popup_, HWND_TOP, screen.left, screen.top, w, h,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
  // Recommitted with every shell frame so it rides the theme ramp too.
  if (layered) {
    if (PhiCommitShapedLayer(
            card_popup_, w, h,
            [&](HDC layer_dc) { PaintHoverCardPopup(card_popup_, layer_dc); },
            [&](double px, double py) {
              return PhiRoundRectCoverage(px, py, w, h, r);
            })) {
      return;
    }
    // No layer to be had: fall back to the region for good.
    ::SetWindowLongPtrW(
        card_popup_, GWL_EXSTYLE,
        ::GetWindowLongPtrW(card_popup_, GWL_EXSTYLE) & ~WS_EX_LAYERED);
    ::SetWindowRgn(card_popup_,
                   ::CreateRoundRectRgn(0, 0, w + 1, h + 1, 2 * r, 2 * r),
                   FALSE);
  }
  ::InvalidateRect(card_popup_, nullptr, FALSE);
}

void PhiSpacesStrip::PaintHoverCardPopup(HWND popup, HDC dc) {
  RECT rc = {};
  ::GetClientRect(popup, &rc);
  if (rc.right <= 0 || rc.bottom <= 0) {
    return;
  }
  HDC mem = ::CreateCompatibleDC(dc);
  HBITMAP bitmap = ::CreateCompatibleBitmap(dc, rc.right, rc.bottom);
  HGDIOBJ old_bitmap = ::SelectObject(mem, bitmap);
  HFONT card_font =
      shell_->SpacesSmallFont() ? shell_->SpacesSmallFont() : shell_->SpacesUiFont();
  HGDIOBJ old_font = card_font ? ::SelectObject(mem, card_font) : nullptr;
  ::SetBkMode(mem, TRANSPARENT);
  const PhiPalette palette = shell_->SpacesPalette();
  HBRUSH ground = ::CreateSolidBrush(palette.flyout);
  ::FillRect(mem, &rc, ground);  // Under the region's trimmed corners.
  ::DeleteObject(ground);
  PaintHoverCard(mem, card_geom_, palette);
  ::BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
  if (old_font) {
    ::SelectObject(mem, old_font);
  }
  ::SelectObject(mem, old_bitmap);
  ::DeleteObject(bitmap);
  ::DeleteDC(mem);
}

// static
LRESULT CALLBACK PhiSpacesStrip::HoverCardPopupProc(HWND hwnd,
                                                    UINT msg,
                                                    WPARAM wparam,
                                                    LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_NCDESTROY) {
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);  // See ShellWndProc.
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiSpacesStrip*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCHITTEST:
      return HTTRANSPARENT;  // Never takes the pointer.
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_ERASEBKGND:
      return 1;  // PaintHoverCardPopup covers the whole client rect.
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (self) {
        self->PaintHoverCardPopup(hwnd, dc);
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace phi_client

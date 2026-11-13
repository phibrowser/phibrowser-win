// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_mini_player.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "client/phi_dwrite.h"
#include "client/phi_fluent.h"
#include "client/phi_i18n.h"
#include "client/phi_theme.h"

namespace phi_client {
namespace {

using MediaAction = phi_framework::PhiFramework::MediaAction;

// Design units (dip), the Mac's SidebarMediaPlayerView and its container.
constexpr int kGap = 5;              // Above and below the card's band.
constexpr int kPadX = 8;             // The card's inset from the sidebar.
constexpr int kPadY = 3;
constexpr int kCompact = 38;         // Card + kPadY twice.
constexpr int kExpanded = 124;
constexpr int kVolumeRow = 34;
constexpr int kTransport = 32;
constexpr int kRadius = 9;
constexpr int kRevealFrom = 54;      // Details fade in over their last 32.
constexpr int kRevealSpan = 32;
constexpr UINT kHoverDelayMs = 120;  // Expand / collapse after a hover.
constexpr int kAnimMs = 200;
constexpr UINT kTrackHoldMs = 5000;  // A previous/next waits this long.
constexpr int kSwipeDip = 40;        // A sideways drag this long cycles.

// Segoe Fluent Icons / MDL2 code points.
constexpr wchar_t kGlyphPrevious[] = L"";
constexpr wchar_t kGlyphNext[] = L"";
constexpr wchar_t kGlyphPlay[] = L"";
constexpr wchar_t kGlyphPause[] = L"";
constexpr wchar_t kGlyphVolume[] = L"";
constexpr wchar_t kGlyphMute[] = L"";
constexpr wchar_t kGlyphPipEnter[] = L"";
constexpr wchar_t kGlyphPipExit[] = L"";
constexpr wchar_t kGlyphChevronDown[] = L"";
constexpr wchar_t kGlyphAudio[] = L"";

std::map<UINT_PTR, PhiMiniPlayer*>& Timers() {
  static auto* timers = new std::map<UINT_PTR, PhiMiniPlayer*>();
  return *timers;
}

bool Contains(const RECT& r, POINT pt) {
  return r.right > r.left && r.bottom > r.top && ::PtInRect(&r, pt);
}

double EaseInOut(double t) {
  t = std::clamp(t, 0.0, 1.0);
  return t < 0.5 ? 2 * t * t : 1 - std::pow(-2 * t + 2, 2) / 2;
}

std::wstring TimeString(double seconds) {
  const long long whole =
      std::isfinite(seconds) ? static_cast<long long>(std::max(0.0, seconds))
                             : 0;
  const long long minutes = whole / 60, rest = whole % 60;
  wchar_t text[32];
  if (minutes >= 60) {
    swprintf(text, 32, L"%lld:%02lld:%02lld", minutes / 60, minutes % 60, rest);
  } else {
    swprintf(text, 32, L"%lld:%02lld", minutes, rest);
  }
  return text;
}

base::DictValue RectJson(const RECT& r) {
  base::DictValue d;
  d.Set("left", static_cast<int>(r.left));
  d.Set("top", static_cast<int>(r.top));
  d.Set("right", static_cast<int>(r.right));
  d.Set("bottom", static_cast<int>(r.bottom));
  return d;
}

}  // namespace

// --- PhiMediaSession -------------------------------------------------------

std::optional<double> PhiMediaSession::PositionNow(uint64_t now) const {
  if (!current_time) {
    return std::nullopt;
  }
  double position = *current_time;
  if (playing && now > read_at) {
    position += static_cast<double>(now - read_at) / 1000.0 * playback_rate;
  }
  position = std::max(0.0, position);
  return duration ? std::min(position, *duration) : position;
}

std::vector<PhiMediaSession> PhiParseMediaSessions(std::string_view json) {
  std::vector<PhiMediaSession> out;
  std::optional<base::Value> parsed =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    return out;
  }
  const uint64_t now = ::GetTickCount64();
  for (const base::Value& entry : parsed->GetList()) {
    const base::DictValue* d = entry.GetIfDict();
    if (!d) {
      continue;
    }
    const std::string* token = d->FindString("token");
    const std::string* source = d->FindString("sourceToken");
    const std::optional<double> window = d->FindDouble("windowId");
    const std::optional<int> tab = d->FindInt("tabId");
    if (!token || token->empty() || !source || source->empty() || !window ||
        !tab) {
      continue;  // The Mac's decode drops the same.
    }
    PhiMediaSession s;
    s.window = BrowserId{static_cast<int64_t>(*window)};
    s.tab_id = *tab;
    s.token = *token;
    s.source_token = *source;
    if (const std::string* title = d->FindString("title")) {
      s.title = base::UTF8ToWide(*title);
    }
    if (const std::string* artist = d->FindString("artist")) {
      s.artist = base::UTF8ToWide(*artist);
    }
    const auto flag = [&](const char* key) {
      return d->FindBool(key).value_or(false);
    };
    s.playing = flag("playing");
    s.muted = flag("muted");
    s.source_silent = flag("sourceSilent");
    s.can_set_mute = flag("canSetMute");
    s.tab_muted = flag("tabMuted");
    s.volume_pending = flag("volumePending");
    s.can_play = flag("canPlay");
    s.can_pause = flag("canPause");
    s.can_previous_track = flag("canPreviousTrack");
    s.can_next_track = flag("canNextTrack");
    s.can_enter_pip = flag("canEnterPictureInPicture");
    s.pip = flag("isPictureInPicture");
    s.can_exit_pip = flag("canExitPictureInPicture") && s.pip;
    if (auto v = d->FindDouble("volume"); v && std::isfinite(*v) &&
                                          *v >= 0 && *v <= 1) {
      s.volume = *v;
    }
    s.can_set_volume = flag("canSetVolume") && s.volume.has_value();
    if (auto t = d->FindDouble("currentTime"); t && std::isfinite(*t)) {
      s.current_time = std::max(0.0, *t);
    }
    if (auto t = d->FindDouble("duration"); t && std::isfinite(*t) && *t > 0) {
      s.duration = *t;
    }
    if (auto r = d->FindDouble("playbackRate"); r && std::isfinite(*r)) {
      s.playback_rate = *r;
    }
    s.can_seek = flag("canSeek") && s.current_time && s.duration;
    s.read_at = now;
    out.push_back(std::move(s));
  }
  return out;
}

// --- PhiMiniPlayer ---------------------------------------------------------

PhiMiniPlayer::PhiMiniPlayer(PhiMiniPlayerHost* host) : host_(host) {}

PhiMiniPlayer::~PhiMiniPlayer() {
  StopTimer(&anim_timer_);
  StopTimer(&hover_timer_);
  StopTimer(&tick_timer_);
  StopTimer(&track_timer_);
  if (title_font_) {
    ::DeleteObject(title_font_);
  }
  if (small_font_) {
    ::DeleteObject(small_font_);
  }
}

// static
PhiMiniPlayer::Mode PhiMiniPlayer::ModeFromName(const std::string& name) {
  if (name == "alwaysExpanded") {
    return Mode::kExpanded;
  }
  if (name == "alwaysCompact") {
    return Mode::kCompact;
  }
  return Mode::kDynamic;
}

void PhiMiniPlayer::SetPreference(bool enabled, Mode mode) {
  const bool changed = enabled != enabled_ || mode != mode_;
  enabled_ = enabled;
  mode_ = mode;
  if (!changed) {
    return;
  }
  if (!enabled_) {
    sessions_.clear();
    dismissed_.clear();
    silent_retained_.clear();
    retained_.reset();
    manual_tab_ = -1;
    EndTrackChange();
  }
  expanded_ = mode_ == Mode::kExpanded || (mode_ == Mode::kDynamic && hovering_);
  Refresh();
}

void PhiMiniPlayer::Refresh() {
  if (enabled_) {
    phi_framework::PhiFramework* framework = host_->MiniPlayerFramework();
    // Copied out at once: the document is framework scratch (api rule 5).
    sessions_ = framework ? PhiParseMediaSessions(framework->MediaSessionsJson())
                          : std::vector<PhiMediaSession>();
    visible_tabs_ = host_->MiniPlayerVisibleTabs();
    for (auto it = visible_tabs_.rbegin(); it != visible_tabs_.rend(); ++it) {
      std::erase(visited_, *it);
      visited_.insert(visited_.begin(), *it);
    }
    if (visited_.size() > 64) {
      visited_.resize(64);
    }
  }
  Select();
}

bool PhiMiniPlayer::IsVisibleTab(int tab_id) const {
  return std::ranges::find(visible_tabs_, tab_id) != visible_tabs_.end();
}

// The Mac's canPresent: a silent source (element mute, silent output, tab
// mute, zero volume) cannot ENTER the card; one already on it keeps its
// controls until its page is visited or another source replaces it.
bool PhiMiniPlayer::CanPresent(const PhiMediaSession& s) const {
  const bool silent = s.muted || s.source_silent || s.tab_muted ||
                      (s.volume && *s.volume == 0);
  if (!silent || (item_ && item_->tab_id == s.tab_id)) {
    return true;
  }
  auto it = silent_retained_.find(s.tab_id);
  return it != silent_retained_.end() && it->second == s.source_token;
}

std::vector<const PhiMediaSession*> PhiMiniPlayer::OrderedSources() const {
  const std::vector<BrowserId> browsers = host_->MiniPlayerBrowsers();
  std::vector<const PhiMediaSession*> ordered;
  const auto eligible = [&](const PhiMediaSession& s) {
    if (std::ranges::find(browsers, s.window) == browsers.end() ||
        IsVisibleTab(s.tab_id) || s.pip || !CanPresent(s)) {
      return false;
    }
    if (auto it = dismissed_.find(s.tab_id);
        it != dismissed_.end() && it->second == s.source_token) {
      return false;
    }
    if (auto it = silent_retained_.find(s.tab_id);
        it != silent_retained_.end() && it->second == s.source_token) {
      return true;
    }
    return s.playing ||
           (retained_ && *retained_ == Identity{s.tab_id, s.source_token});
  };
  // Visit order first, then the rest as the framework listed them.
  for (int tab : visited_) {
    for (const PhiMediaSession& s : sessions_) {
      if (s.tab_id == tab && eligible(s)) {
        ordered.push_back(&s);
      }
    }
  }
  for (const PhiMediaSession& s : sessions_) {
    if (std::ranges::find(visited_, s.tab_id) == visited_.end() &&
        eligible(s)) {
      ordered.push_back(&s);
    }
  }
  return ordered;
}

void PhiMiniPlayer::Select() {
  if (!enabled_) {
    Show(nullptr);
    return;
  }
  const auto find = [&](int tab) -> const PhiMediaSession* {
    for (const PhiMediaSession& s : sessions_) {
      if (s.tab_id == tab) {
        return &s;
      }
    }
    return nullptr;
  };
  // A dismissal lasts until the tab is visited or its source changes.
  std::erase_if(dismissed_, [&](const auto& entry) {
    const PhiMediaSession* s = find(entry.first);
    return !s || s->source_token != entry.second || IsVisibleTab(entry.first);
  });
  std::erase_if(silent_retained_, [&](const auto& entry) {
    const PhiMediaSession* s = find(entry.first);
    auto dismissed = dismissed_.find(entry.first);
    return !s || s->source_token != entry.second || IsVisibleTab(entry.first) ||
           s->pip ||
           (dismissed != dismissed_.end() && dismissed->second == entry.second);
  });
  if (retained_) {
    const PhiMediaSession* s = find(retained_->tab_id);
    if (!s || s->source_token != retained_->source_token ||
        IsVisibleTab(retained_->tab_id)) {
      retained_.reset();
    }
  }
  // A previous/next in flight: keep the card through the session gap,
  // blocking stale controls, until the new track's state arrives.
  if (pending_track_ && item_ && !IsVisibleTab(pending_track_->tab_id)) {
    const PhiMediaSession* s = find(pending_track_->tab_id);
    if (!s || s->source_token == pending_track_->source_token) {
      if (s) {
        item_ = *s;
      }
      UpdateTargetHeight();
      host_->MiniPlayerInvalidate();
      return;
    }
    // The replacement track keeps the card even if it starts paused
    // (Mac 63238441).
    if (s) {
      retained_ = Identity{s->tab_id, s->source_token};
    }
  }
  EndTrackChange();
  const std::vector<const PhiMediaSession*> choices = OrderedSources();
  source_count_ = static_cast<int>(choices.size());
  const PhiMediaSession* chosen = nullptr;
  for (const PhiMediaSession* s : choices) {
    if (s->tab_id == manual_tab_) {
      chosen = s;
    }
  }
  if (!chosen) {
    manual_tab_ = -1;
    chosen = choices.empty() ? nullptr : choices.front();
  }
  Show(chosen);
}

void PhiMiniPlayer::Show(const PhiMediaSession* chosen) {
  const bool source_changed =
      !chosen || !item_ || item_->tab_id != chosen->tab_id ||
      item_->source_token != chosen->source_token;
  if (chosen) {
    item_ = *chosen;
    retained_ = Identity{chosen->tab_id, chosen->source_token};
    if (chosen->muted || chosen->source_silent || chosen->tab_muted ||
        (chosen->volume && *chosen->volume == 0)) {
      silent_retained_[chosen->tab_id] = chosen->source_token;
    } else {
      silent_retained_.erase(chosen->tab_id);
    }
    if (!chosen->volume && !chosen->volume_pending) {
      volume_open_ = false;
    }
  } else {
    item_.reset();
  }
  if (source_changed) {
    volume_open_ = false;
    scrub_seconds_ = -1;
    if (drag_ == Drag::kTimeline || drag_ == Drag::kVolume) {
      drag_ = Drag::kNone;  // A gesture never adopts new media.
      host_->MiniPlayerCapture(false);
    }
    if (mode_ == Mode::kDynamic && !hovering_) {
      expanded_ = false;
    }
  }
  // The once-a-second position tick runs only while a timeline shows.
  if (item_ && item_->playing && item_->duration) {
    if (!tick_timer_) {
      StartTimer(&tick_timer_, 1000);
    }
  } else {
    StopTimer(&tick_timer_);
  }
  UpdateTargetHeight();
  host_->MiniPlayerInvalidate();
}

// --- Height ----------------------------------------------------------------

int PhiMiniPlayer::ReservedDip() const {
  return static_cast<int>(std::lround(height_dip_));
}

void PhiMiniPlayer::UpdateTargetHeight() {
  int target = 0;
  if (item_) {
    const bool expanded = mode_ == Mode::kExpanded ||
                          (mode_ == Mode::kDynamic && expanded_);
    target = (expanded ? kExpanded : kCompact) +
             (volume_open_ ? kVolumeRow : 0) + 2 * kGap;
  }
  if (target == height_target_) {
    return;
  }
  height_target_ = target;
  // Appearing and disappearing are immediate (the Mac adds / removes the
  // view); a live card animates between heights.
  if (target == 0 || height_dip_ < 0.5) {
    height_dip_ = target;
    StopTimer(&anim_timer_);
    host_->MiniPlayerHeightChanged();
    return;
  }
  height_from_ = height_dip_;
  height_start_ = ::GetTickCount64();
  if (!anim_timer_) {
    StartTimer(&anim_timer_, 15);
  }
}

void PhiMiniPlayer::Animate() {
  const double t =
      static_cast<double>(::GetTickCount64() - height_start_) / kAnimMs;
  height_dip_ = height_from_ + (height_target_ - height_from_) * EaseInOut(t);
  if (t >= 1) {
    height_dip_ = height_target_;
    StopTimer(&anim_timer_);
  }
  host_->MiniPlayerHeightChanged();
}

void PhiMiniPlayer::SetExpanded(bool expanded) {
  if (mode_ != Mode::kDynamic || expanded_ == expanded) {
    return;
  }
  expanded_ = expanded;
  UpdateTargetHeight();
  host_->MiniPlayerInvalidate();
}

void PhiMiniPlayer::ScheduleHover(bool entering) {
  if (mode_ != Mode::kDynamic) {
    return;
  }
  StopTimer(&hover_timer_);
  if (entering == expanded_) {
    return;
  }
  hover_entering_ = entering;
  StartTimer(&hover_timer_, kHoverDelayMs);
}

// --- Timers ----------------------------------------------------------------

void PhiMiniPlayer::StartTimer(UINT_PTR* id, UINT ms) {
  StopTimer(id);
  *id = ::SetTimer(nullptr, 0, ms, &PhiMiniPlayer::TimerProc);
  if (*id) {
    Timers()[*id] = this;
  }
}

void PhiMiniPlayer::StopTimer(UINT_PTR* id) {
  if (*id) {
    ::KillTimer(nullptr, *id);
    Timers().erase(*id);
    *id = 0;
  }
}

// static
void CALLBACK PhiMiniPlayer::TimerProc(HWND, UINT, UINT_PTR id, DWORD) {
  auto it = Timers().find(id);
  if (it != Timers().end()) {
    it->second->OnTimer(id);
  }
}

void PhiMiniPlayer::OnTimer(UINT_PTR id) {
  if (id == anim_timer_) {
    Animate();
  } else if (id == hover_timer_) {
    StopTimer(&hover_timer_);
    // Collapse waits while the volume row is open or a drag holds the card.
    if (hover_entering_ || (!volume_open_ && drag_ == Drag::kNone)) {
      SetExpanded(hover_entering_);
    }
  } else if (id == tick_timer_) {
    host_->MiniPlayerInvalidate();  // The extrapolated position moves.
  } else if (id == track_timer_) {
    EndTrackChange();  // Nothing replaced it in five seconds.
    Select();
  }
}

void PhiMiniPlayer::BeginTrackChange() {
  if (!item_) {
    return;
  }
  pending_track_ = Identity{item_->tab_id, item_->source_token};
  StartTimer(&track_timer_, kTrackHoldMs);
}

void PhiMiniPlayer::EndTrackChange() {
  pending_track_.reset();
  StopTimer(&track_timer_);
}

// --- Geometry --------------------------------------------------------------

int PhiMiniPlayer::Scaled(int dip) const {
  return ::MulDiv(dip, static_cast<int>(host_->MiniPlayerDpi()), 96);
}

PhiMiniPlayer::Geometry PhiMiniPlayer::Layout(const RECT& band) const {
  Geometry g;
  if (!item_ || band.bottom - band.top <= Scaled(2 * kGap)) {
    return g;
  }
  // The band = gap + card (with its kPadY margins) + gap.
  g.card = {band.left + Scaled(kPadX), band.top + Scaled(kGap + kPadY),
            band.right - Scaled(kPadX), band.bottom - Scaled(kGap + kPadY)};
  if (g.card.bottom - g.card.top < Scaled(kTransport)) {
    g.card.top = g.card.bottom - Scaled(kTransport);
  }
  if (source_count_ > 1) {
    g.stack = {g.card.left + Scaled(4), g.card.top - Scaled(4),
               g.card.right - Scaled(4), g.card.top + Scaled(12)};
  }
  const int card_h = g.card.bottom - g.card.top;
  const int volume_h =
      volume_open_ ? std::min(Scaled(kVolumeRow),
                              std::max(0, card_h - Scaled(kTransport)))
                   : 0;
  const int details_h = std::max(0, card_h - Scaled(kTransport) - volume_h);
  g.reveal = std::clamp(
      (details_h - Scaled(kRevealFrom)) / static_cast<double>(Scaled(kRevealSpan)),
      0.0, 1.0);

  // Transport: the source button leading, the volume button trailing, the
  // track cluster centred (22 + 2 + 28 + 2 + 22).
  const int row_top = g.card.top + details_h;
  const int mid = row_top + Scaled(kTransport) / 2;
  g.source_tab = {g.card.left + Scaled(8), mid - Scaled(12),
                  g.card.left + Scaled(32), mid + Scaled(12)};
  g.volume = {g.card.right - Scaled(32), mid - Scaled(12),
              g.card.right - Scaled(8), mid + Scaled(12)};
  const int cx = (g.card.left + g.card.right) / 2;
  g.previous = {cx - Scaled(38), mid - Scaled(11), cx - Scaled(16),
                mid + Scaled(11)};
  g.play_pause = {cx - Scaled(14), mid - Scaled(14), cx + Scaled(14),
                  mid + Scaled(14)};
  g.next = {cx + Scaled(16), mid - Scaled(11), cx + Scaled(38),
            mid + Scaled(11)};

  if (volume_h > 0) {
    g.volume_row = {g.card.left + Scaled(10), row_top + Scaled(kTransport),
                    g.card.right - Scaled(10),
                    row_top + Scaled(kTransport) + volume_h};
    const int vmid = g.volume_row.top + Scaled(kVolumeRow) / 2;
    g.volume_track = {g.volume_row.left + Scaled(4), vmid - Scaled(9),
                      g.volume_row.right - Scaled(4), vmid + Scaled(9)};
  }

  if (g.reveal > 0) {
    // Details, laid out from the card's top (their own top inset 8).
    const int left = g.card.left + Scaled(10);
    const int right = g.card.right - Scaled(10);
    int y = g.card.top + Scaled(8);
    g.dismiss = {right - Scaled(22), y, right, y + Scaled(22)};
    int title_right = g.dismiss.left - Scaled(4);
    if (item_->can_pip()) {
      g.pip = {g.dismiss.left - Scaled(26), y, g.dismiss.left - Scaled(4),
               y + Scaled(22)};
      title_right = g.pip.left - Scaled(4);
    }
    g.title = {left, y, title_right, y + Scaled(22)};
    y += Scaled(22 + 1);
    if (!item_->artist.empty()) {
      g.artist = {left, y - Scaled(4), right, y + Scaled(10)};
    }
    y += Scaled(13 + 4);
    if (item_->duration && item_->current_time) {
      g.timeline = {left, y, right, y + Scaled(18)};
      y += Scaled(18 + 2);
      g.elapsed = {left, y, (left + right) / 2, y + Scaled(13)};
      g.duration = {(left + right) / 2, y, right, y + Scaled(13)};
    }
  }
  return g;
}

PhiMiniPlayer::Part PhiMiniPlayer::HitTest(const Geometry& g,
                                           POINT pt) const {
  if (!Contains(g.card, pt)) {
    return Part::kNone;
  }
  // Details answer only once they are (nearly) revealed, as on the Mac.
  if (g.reveal >= 0.95) {
    if (Contains(g.dismiss, pt)) {
      return Part::kDismiss;
    }
    if (Contains(g.pip, pt)) {
      return Part::kPictureInPicture;
    }
    if (Contains(g.timeline, pt)) {
      return Part::kTimeline;
    }
  }
  if (Contains(g.source_tab, pt)) {
    return Part::kSourceTab;
  }
  if (Contains(g.previous, pt)) {
    return Part::kPrevious;
  }
  if (Contains(g.play_pause, pt)) {
    return Part::kPlayPause;
  }
  if (Contains(g.next, pt)) {
    return Part::kNext;
  }
  if (Contains(g.volume, pt)) {
    return Part::kVolume;
  }
  if (Contains(g.volume_row, pt)) {
    return Part::kVolumeSlider;
  }
  return Part::kCard;
}

// --- Pointer ---------------------------------------------------------------

bool PhiMiniPlayer::OnMouseMove(const RECT& band, POINT pt) {
  const Geometry g = Layout(band);
  if (drag_ == Drag::kTimeline) {
    if (item_ && item_->duration) {
      const double f =
          std::clamp((pt.x - g.timeline.left) /
                         static_cast<double>(std::max<LONG>(
                             1, g.timeline.right - g.timeline.left)),
                     0.0, 1.0);
      scrub_seconds_ = *item_->duration * f;
      host_->MiniPlayerInvalidate();
    }
    return true;
  }
  if (drag_ == Drag::kVolume) {
    SetVolumeAt(g, pt);
    return true;
  }
  if (drag_ == Drag::kSwipe) {
    return true;
  }
  const Part part = HitTest(g, pt);
  const bool on_card = part != Part::kNone;
  if (on_card != hovering_) {
    hovering_ = on_card;
    ScheduleHover(on_card);
  }
  if (part != hot_) {
    hot_ = part;
    host_->MiniPlayerInvalidate();
  }
  return on_card;
}

void PhiMiniPlayer::OnMouseLeave() {
  if (drag_ != Drag::kNone) {
    return;
  }
  if (hovering_) {
    hovering_ = false;
    ScheduleHover(false);
  }
  if (hot_ != Part::kNone) {
    hot_ = Part::kNone;
    host_->MiniPlayerInvalidate();
  }
}

bool PhiMiniPlayer::OnButtonDown(const RECT& band, POINT pt, bool alt) {
  const Geometry g = Layout(band);
  const Part part = HitTest(g, pt);
  if (part == Part::kNone) {
    return false;
  }
  pressed_ = part;
  switch (part) {
    case Part::kTimeline:
      if (item_ && item_->can_seek && !pending_track_) {
        drag_ = Drag::kTimeline;
        host_->MiniPlayerCapture(true);
        OnMouseMove(band, pt);
      }
      return true;
    case Part::kVolumeSlider:
      if (item_ && item_->can_set_volume && !item_->muted && !pending_track_) {
        drag_ = Drag::kVolume;
        host_->MiniPlayerCapture(true);
        SetVolumeAt(g, pt);
      }
      return true;
    case Part::kCard:
      // A sideways drag on the card's plain area cycles between sources.
      if (source_count_ > 1) {
        drag_ = Drag::kSwipe;
        drag_origin_ = pt;
        host_->MiniPlayerCapture(true);
      }
      return true;
    default:
      Click(part, alt);
      return true;
  }
}

bool PhiMiniPlayer::OnButtonUp(const RECT& band, POINT pt) {
  pressed_ = Part::kNone;
  const Drag drag = drag_;
  if (drag == Drag::kNone) {
    return false;
  }
  drag_ = Drag::kNone;
  host_->MiniPlayerCapture(false);
  if (drag == Drag::kTimeline && scrub_seconds_ >= 0) {
    Perform(MediaAction::kSeekTo, scrub_seconds_);
  } else if (drag == Drag::kSwipe) {
    const int dx = pt.x - drag_origin_.x;
    const int dy = pt.y - drag_origin_.y;
    if (std::abs(dx) >= Scaled(kSwipeDip) && std::abs(dx) > std::abs(dy)) {
      Cycle(dx < 0 ? 1 : -1);
    }
  }
  scrub_seconds_ = -1;
  host_->MiniPlayerInvalidate();
  OnMouseMove(band, pt);  // Hover and the collapse timer from here.
  return true;
}

void PhiMiniPlayer::OnCaptureLost() {
  if (drag_ == Drag::kNone) {
    return;
  }
  drag_ = Drag::kNone;
  scrub_seconds_ = -1;
  pressed_ = Part::kNone;
  host_->MiniPlayerInvalidate();
}

void PhiMiniPlayer::Click(Part part, bool alt) {
  if (!item_) {
    return;
  }
  phi_framework::PhiFramework* framework = host_->MiniPlayerFramework();
  switch (part) {
    case Part::kSourceTab:
      if (!pending_track_) {
        host_->MiniPlayerShowTab(item_->window, item_->tab_id);
      }
      break;
    case Part::kPlayPause:
      // The DISPLAYED intent: a stale pause never turns into play.
      Perform(item_->playing ? MediaAction::kPause : MediaAction::kPlay);
      break;
    case Part::kPrevious:
      if (item_->can_previous_track &&
          Perform(MediaAction::kPreviousTrack)) {
        BeginTrackChange();
      }
      break;
    case Part::kNext:
      if (item_->can_next_track && Perform(MediaAction::kNextTrack)) {
        BeginTrackChange();
      }
      break;
    case Part::kVolume:
      if (pending_track_ || !framework) {
        break;
      }
      // Alt-click (the Mac's Option-click), or a click while muted, toggles
      // the player's mute; a plain click opens the slider and reads the
      // element volume once.
      if (alt || item_->muted) {
        if (item_->can_set_mute) {
          framework->SetMediaSessionMuted(item_->tab_id, !item_->muted,
                                          item_->token.c_str());
        }
      } else {
        volume_open_ = !volume_open_;
        if (volume_open_ &&
            !framework->ReadMediaSessionVolume(item_->tab_id,
                                               item_->token.c_str())) {
          volume_open_ = false;
        }
        if (!volume_open_ && !hovering_) {
          ScheduleHover(false);
        }
        UpdateTargetHeight();
        host_->MiniPlayerInvalidate();
      }
      break;
    case Part::kPictureInPicture:
      if (item_->pip) {
        Perform(MediaAction::kExitPictureInPicture);
      } else {
        Perform(MediaAction::kEnterPictureInPicture);
      }
      break;
    case Part::kDismiss:
      // Hides this source without stopping it; a new source may show.
      dismissed_[item_->tab_id] = item_->source_token;
      retained_.reset();
      manual_tab_ = -1;
      EndTrackChange();
      Select();
      break;
    default:
      break;
  }
}

bool PhiMiniPlayer::Perform(MediaAction action, double seconds) {
  phi_framework::PhiFramework* framework = host_->MiniPlayerFramework();
  if (!item_ || !framework || pending_track_) {
    return false;
  }
  return framework->MediaSessionAction(item_->tab_id, action,
                                       item_->token.c_str(), seconds);
}

void PhiMiniPlayer::SetVolumeAt(const Geometry& g, POINT pt) {
  phi_framework::PhiFramework* framework = host_->MiniPlayerFramework();
  if (!item_ || !framework || g.volume_track.right <= g.volume_track.left) {
    return;
  }
  const double v = std::clamp(
      (pt.x - g.volume_track.left) /
          static_cast<double>(g.volume_track.right - g.volume_track.left),
      0.0, 1.0);
  if (framework->SetMediaSessionVolume(item_->tab_id, v,
                                       item_->token.c_str())) {
    item_->volume = v;  // Responsive while the applied value comes back.
    host_->MiniPlayerInvalidate();
  }
}

void PhiMiniPlayer::Cycle(int direction) {
  const std::vector<const PhiMediaSession*> choices = OrderedSources();
  if (choices.size() < 2 || !item_) {
    return;
  }
  int index = 0;
  for (size_t i = 0; i < choices.size(); ++i) {
    if (choices[i]->tab_id == item_->tab_id) {
      index = static_cast<int>(i);
    }
  }
  const int n = static_cast<int>(choices.size());
  manual_tab_ = choices[static_cast<size_t>(((index + direction) % n + n) % n)]
                    ->tab_id;
  EndTrackChange();
  Select();
}

// --- Paint -----------------------------------------------------------------

void PhiMiniPlayer::EnsureFonts() {
  const UINT dpi = host_->MiniPlayerDpi();
  if (font_dpi_ == dpi && title_font_) {
    return;
  }
  if (title_font_) {
    ::DeleteObject(title_font_);
  }
  if (small_font_) {
    ::DeleteObject(small_font_);
  }
  title_font_ = PhiFluentFont(dpi, 12, FW_SEMIBOLD);
  small_font_ = PhiFluentFont(dpi, 10);
  font_dpi_ = dpi;
}

void PhiMiniPlayer::PaintButton(HDC dc,
                                const RECT& rect,
                                const wchar_t* glyph,
                                Part part,
                                bool enabled,
                                bool toggled,
                                int glyph_dip,
                                const PhiPalette& palette) {
  if (rect.right <= rect.left) {
    return;
  }
  PhiButtonState state = PhiButtonState::kRest;
  if (!enabled) {
    state = PhiButtonState::kDisabled;
  } else if (toggled) {
    state = PhiButtonState::kToggled;
  } else if (pressed_ == part && hot_ == part) {
    state = PhiButtonState::kPressed;
  } else if (hot_ == part) {
    state = PhiButtonState::kHover;
  }
  PhiPaintIconButton(dc, rect, glyph, state, palette, palette.label,
                     Scaled(glyph_dip), Scaled(5));
}

void PhiMiniPlayer::Paint(HDC dc, const RECT& band, const PhiPalette& palette) {
  const Geometry g = Layout(band);
  if (!item_ || g.card.right <= g.card.left) {
    return;
  }
  EnsureFonts();
  const PhiMediaSession& s = *item_;
  const bool stale = pending_track_.has_value();
  // The second card peeking above says "more than one source".
  if (g.stack.right > g.stack.left) {
    PhiFillRoundRectAA(dc, g.stack, Scaled(kRadius),
                       PhiBlend(palette.selected, palette.ground_top, 166));
    PhiStrokeRoundRectAA(dc, g.stack, Scaled(kRadius),
                         PhiBlend(palette.border, palette.ground_top, 166), 1);
  }
  PhiFillRoundRectAA(dc, g.card, Scaled(kRadius), palette.selected);
  PhiStrokeRoundRectAA(dc, g.card, Scaled(kRadius), palette.border, 1);

  const int saved = ::SaveDC(dc);
  ::IntersectClipRect(dc, g.card.left, g.card.top, g.card.right,
                      g.card.bottom);
  ::SetBkMode(dc, TRANSPARENT);

  // Details: title (+ PiP, dismiss), artist, timeline.
  if (g.reveal > 0) {
    const int alpha = static_cast<int>(std::lround(g.reveal * 255));
    const auto ink = [&](PhiColor c) {
      return PhiBlend(c, palette.selected, alpha);
    };
    std::wstring title = s.title;
    if (title.empty()) {
      title = host_->MiniPlayerTabTitle(s.window, s.tab_id);
    }
    if (title.empty()) {
      title = PhiStr("sidebar.media.untitled");
    }
    ::SelectObject(dc, title_font_);
    ::SetTextColor(dc, ink(palette.label));
    RECT r = g.title;
    ::DrawTextW(dc, title.c_str(), static_cast<int>(title.size()), &r,
                DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (g.reveal >= 0.95) {
      PaintButton(dc, g.pip, s.pip ? kGlyphPipExit : kGlyphPipEnter,
                  Part::kPictureInPicture, !stale, false, 14, palette);
      PaintButton(dc, g.dismiss, kGlyphChevronDown, Part::kDismiss, true,
                  false, 10, palette);
    }
    ::SelectObject(dc, small_font_);
    if (!s.artist.empty()) {
      ::SetTextColor(dc, ink(palette.label_dim));
      r = g.artist;
      ::DrawTextW(dc, s.artist.c_str(), static_cast<int>(s.artist.size()), &r,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (g.timeline.right > g.timeline.left && s.duration) {
      const double position =
          scrub_seconds_ >= 0 ? scrub_seconds_
                              : s.PositionNow(::GetTickCount64()).value_or(0);
      const double fraction = std::clamp(position / *s.duration, 0.0, 1.0);
      const int tmid = (g.timeline.top + g.timeline.bottom) / 2;
      RECT track = {g.timeline.left, tmid - Scaled(3) / 2, g.timeline.right,
                    tmid - Scaled(3) / 2 + std::max(2, Scaled(3))};
      PhiFillCapsuleAA(dc, track, ink(PhiBlend(palette.label_dim,
                                               palette.selected, 87)));
      RECT done = track;
      done.right = done.left + static_cast<LONG>(std::lround(
                                   (track.right - track.left) * fraction));
      if (done.right - done.left >= track.bottom - track.top) {
        PhiFillCapsuleAA(dc, done,
                         ink(PhiBlend(palette.label, palette.selected, 204)));
      }
      ::SetTextColor(dc, ink(palette.label_dim));
      const std::wstring elapsed = TimeString(position);
      const std::wstring total = TimeString(*s.duration);
      r = g.elapsed;
      ::DrawTextW(dc, elapsed.c_str(), static_cast<int>(elapsed.size()), &r,
                  DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX);
      r = g.duration;
      ::DrawTextW(dc, total.c_str(), static_cast<int>(total.size()), &r,
                  DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX);
    }
  }

  // Transport.
  {
    RECT icon = g.source_tab;
    const int inset = (icon.right - icon.left - Scaled(18)) / 2;
    ::InflateRect(&icon, -inset, -inset);
    if (hot_ == Part::kSourceTab) {
      PhiFillRoundRectAA(dc, g.source_tab, Scaled(5), palette.hover_strong);
    }
    int w = 0, h = 0;
    phi_framework::PhiFramework* framework = host_->MiniPlayerFramework();
    const unsigned char* bgra =
        framework ? framework->TabFaviconBgraById(s.window.value, s.tab_id, &w,
                                                  &h)
                  : nullptr;
    if (!(bgra && w > 0 && h > 0 &&
          PhiDrawBgra(dc, icon, bgra, w, h))) {
      PhiDrawIconGlyph(dc, icon, kGlyphAudio, Scaled(16), palette.label_dim);
    }
  }
  PaintButton(dc, g.previous, kGlyphPrevious, Part::kPrevious,
              s.can_previous_track && !stale, false, 12, palette);
  PaintButton(dc, g.play_pause, s.playing ? kGlyphPause : kGlyphPlay,
              Part::kPlayPause, s.can_play_pause() && !stale, false, 17,
              palette);
  PaintButton(dc, g.next, kGlyphNext, Part::kNext,
              s.can_next_track && !stale, false, 12, palette);
  PaintButton(dc, g.volume, s.muted ? kGlyphMute : kGlyphVolume, Part::kVolume,
              !stale, volume_open_, 12, palette);

  // Volume row: a track, its filled part in the accent, and a knob.
  if (g.volume_track.right > g.volume_track.left) {
    if (s.volume) {
      const bool live = s.can_set_volume && !s.muted && !stale;
      const int vmid = (g.volume_track.top + g.volume_track.bottom) / 2;
      const int th = std::max(2, Scaled(4));
      RECT track = {g.volume_track.left, vmid - th / 2, g.volume_track.right,
                    vmid - th / 2 + th};
      PhiFillCapsuleAA(dc, track,
                       PhiBlend(palette.label_dim, palette.selected, 87));
      const int knob_x =
          track.left + static_cast<int>(std::lround(
                           (track.right - track.left) * *s.volume));
      RECT done = track;
      done.right = knob_x;
      const PhiColor accent =
          live ? palette.accent
               : PhiBlend(palette.label_dim, palette.selected, 140);
      if (done.right - done.left >= th) {
        PhiFillCapsuleAA(dc, done, accent);
      }
      const int k = Scaled(7);
      RECT knob = {knob_x - k, vmid - k, knob_x + k, vmid + k};
      PhiFillEllipseAA(dc, knob, PhiRgb(0xFF, 0xFF, 0xFF));
      PhiStrokeEllipseAA(dc, knob, palette.stroke_strong, 1);
    } else if (s.volume_pending) {
      ::SelectObject(dc, small_font_);
      ::SetTextColor(dc, palette.label_dim);
      RECT r = g.volume_row;
      ::DrawTextW(dc, L"…", 1, &r,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    }
  }
  ::RestoreDC(dc, saved);
}

std::wstring PhiMiniPlayer::TipAt(const RECT& band, POINT pt) const {
  if (!item_ || drag_ != Drag::kNone) {
    return std::wstring();
  }
  switch (HitTest(Layout(band), pt)) {
    case Part::kSourceTab:
      return PhiStr("sidebar.media.openTab");
    case Part::kPrevious:
      return PhiStr("sidebar.media.previousTrack");
    case Part::kNext:
      return PhiStr("sidebar.media.nextTrack");
    case Part::kPlayPause:
      return PhiStr(item_->playing ? "sidebar.media.pause"
                                   : "sidebar.media.play");
    case Part::kVolume:
      return PhiStr("sidebar.media.volumeButtonAlt");
    case Part::kVolumeSlider:
      return PhiStr("sidebar.media.volumeSlider");
    case Part::kPictureInPicture:
      return PhiStr(item_->pip ? "sidebar.media.exitPictureInPicture"
                               : "sidebar.media.enterPictureInPicture");
    case Part::kDismiss:
      return PhiStr("sidebar.media.dismiss");
    case Part::kTimeline:
      return PhiStr("sidebar.media.progress");
    default:
      return std::wstring();
  }
}

std::string PhiMiniPlayer::StateJson(const RECT& band) const {
  base::DictValue d;
  d.Set("enabled", enabled_);
  d.Set("mode", mode_ == Mode::kExpanded  ? "alwaysExpanded"
                : mode_ == Mode::kCompact ? "alwaysCompact"
                                          : "dynamic");
  d.Set("visible", item_.has_value());
  d.Set("reservedDip", ReservedDip());
  d.Set("sources", source_count_);
  d.Set("sessions", static_cast<int>(sessions_.size()));
  if (item_) {
    const Geometry g = Layout(band);
    d.Set("tabId", item_->tab_id);
    d.Set("windowId", static_cast<double>(item_->window.value));
    d.Set("title", base::WideToUTF8(item_->title));
    d.Set("artist", base::WideToUTF8(item_->artist));
    d.Set("playing", item_->playing);
    d.Set("muted", item_->muted);
    d.Set("pip", item_->pip);
    d.Set("expanded", g.reveal >= 0.95);
    d.Set("volumeOpen", volume_open_);
    if (item_->volume) {
      d.Set("volume", *item_->volume);
    }
    if (auto p = item_->PositionNow(::GetTickCount64())) {
      d.Set("position", *p);
    }
    if (item_->duration) {
      d.Set("duration", *item_->duration);
    }
    d.Set("canPrevious", item_->can_previous_track);
    d.Set("canNext", item_->can_next_track);
    d.Set("trackChange", pending_track_.has_value());
    base::DictValue parts;
    parts.Set("band", RectJson(band));
    parts.Set("card", RectJson(g.card));
    parts.Set("sourceTab", RectJson(g.source_tab));
    parts.Set("previous", RectJson(g.previous));
    parts.Set("playPause", RectJson(g.play_pause));
    parts.Set("next", RectJson(g.next));
    parts.Set("volume", RectJson(g.volume));
    parts.Set("pictureInPicture", RectJson(g.pip));
    parts.Set("dismiss", RectJson(g.dismiss));
    parts.Set("timeline", RectJson(g.timeline));
    parts.Set("volumeTrack", RectJson(g.volume_track));
    d.Set("parts", std::move(parts));
  }
  std::string json;
  base::JSONWriter::Write(d, &json);
  return json;
}

}  // namespace phi_client

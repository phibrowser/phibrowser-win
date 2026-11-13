// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_MINI_PLAYER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_MINI_PLAYER_H_

#include <windows.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "client/phi_ids.h"

namespace phi_client {

struct PhiPalette;

// One tab's native MediaSession, as PhiFramework::MediaSessionsJson reports
// it (the Mac's NativeMediaAdapter.Playback; same validation).
struct PhiMediaSession {
  BrowserId window;
  int tab_id = -1;
  std::string token;
  std::string source_token;
  std::wstring title;
  std::wstring artist;
  bool playing = false;
  bool muted = false;  // The sole player's element mute.
  bool source_silent = false;
  bool can_set_mute = false;
  bool tab_muted = false;
  bool can_set_volume = false;
  bool volume_pending = false;
  std::optional<double> volume;
  std::optional<double> current_time;  // At `read_at`.
  std::optional<double> duration;      // Finite and positive.
  double playback_rate = 1;
  bool can_play = false;
  bool can_pause = false;
  bool can_seek = false;
  bool can_previous_track = false;
  bool can_next_track = false;
  bool can_enter_pip = false;
  bool can_exit_pip = false;
  bool pip = false;
  uint64_t read_at = 0;  // GetTickCount64() when the document was read.

  bool can_play_pause() const { return playing ? can_pause : can_play; }
  bool can_pip() const { return pip ? can_exit_pip : can_enter_pip; }
  // The position now, extrapolated from `read_at` while playing.
  std::optional<double> PositionNow(uint64_t now) const;
};

// Parses the framework's document; malformed entries are dropped.
std::vector<PhiMediaSession> PhiParseMediaSessions(std::string_view json);

// What a shell tells its player. UI thread only.
class PhiMiniPlayerHost {
 public:
  virtual phi_framework::PhiFramework* MiniPlayerFramework() const = 0;
  // Every Space browser this shell hosts, presented or not.
  virtual std::vector<BrowserId> MiniPlayerBrowsers() const = 0;
  // The tabs the presented Space shows as page content: its active tab and
  // the other panes of that tab's split. A hidden Space shows nothing.
  virtual std::vector<int> MiniPlayerVisibleTabs() const = 0;
  // The tab's own title, the fallback when the session has none.
  virtual std::wstring MiniPlayerTabTitle(BrowserId browser,
                                          int tab_id) const = 0;
  virtual UINT MiniPlayerDpi() const = 0;
  // The card's height changed: the shell lays the sidebar out again.
  virtual void MiniPlayerHeightChanged() = 0;
  // The card needs a repaint (docked sidebar and floating panel alike).
  virtual void MiniPlayerInvalidate() = 0;
  // "Show playing tab": switches to the tab's Space if needed, then the tab.
  virtual void MiniPlayerShowTab(BrowserId browser, int tab_id) = 0;
  // A drag (timeline, volume, source swipe) holds the pointer.
  virtual void MiniPlayerCapture(bool capture) = 0;

 protected:
  ~PhiMiniPlayerHost() = default;
};

// The sidebar Mini Player (the Mac's SidebarMediaController +
// SidebarMediaPlayerView, docs/native-media-controls.md there): a card on
// top of the bottom bar that controls media playing in this window's
// background tabs — play/pause, previous/next track, seek, volume and mute,
// picture-in-picture, "show the tab", dismiss, and swiping between sources.
// One per shell window, shared by the docked sidebar and the floating
// panel; the shell lays out `ReservedDip()` above the bottom bar, paints
// the card into that band and forwards the pointer. All state comes from
// the framework's native MediaSession bridge; there is no script fallback.
class PhiMiniPlayer {
 public:
  enum class Mode { kDynamic, kExpanded, kCompact };

  explicit PhiMiniPlayer(PhiMiniPlayerHost* host);
  ~PhiMiniPlayer();
  PhiMiniPlayer(const PhiMiniPlayer&) = delete;
  PhiMiniPlayer& operator=(const PhiMiniPlayer&) = delete;

  // The preference. Off hides the card and forgets every source; playback
  // is untouched.
  void SetPreference(bool enabled, Mode mode);
  static Mode ModeFromName(const std::string& name);
  // Re-reads the framework's sessions and re-selects. Call on
  // OnMediaSessionsChanged and whenever the shell's tabs, Spaces or splits
  // change (visibility moves with them).
  void Refresh();

  // The band to reserve above the bottom bar, gaps included, as it animates.
  int ReservedDip() const;
  bool visible() const { return item_.has_value(); }

  // `band` is the layout's media_card on the surface being painted / hit.
  void Paint(HDC dc, const RECT& band, const PhiPalette& palette);
  // True while the pointer is on the card (the caller skips its own hover).
  bool OnMouseMove(const RECT& band, POINT pt);
  void OnMouseLeave();
  // True when the press landed on the card and was consumed.
  bool OnButtonDown(const RECT& band, POINT pt, bool alt);
  // True when a drag the card owned ended here.
  bool OnButtonUp(const RECT& band, POINT pt);
  void OnCaptureLost();
  bool dragging() const { return drag_ != Drag::kNone; }
  // The tooltip for the part under `pt`, empty for none.
  std::wstring TipAt(const RECT& band, POINT pt) const;

  // The card's state and part rects for agentSpace.window.layout (the test
  // rig's readout), in the band's coordinates.
  std::string StateJson(const RECT& band) const;

 private:
  enum class Part {
    kNone,
    kCard,
    kSourceTab,
    kPrevious,
    kPlayPause,
    kNext,
    kVolume,
    kPictureInPicture,
    kDismiss,
    kTimeline,
    kVolumeSlider,
  };
  enum class Drag { kNone, kTimeline, kVolume, kSwipe };
  struct Geometry {
    RECT card{};
    RECT stack{};  // The second card peeking above (several sources).
    RECT source_tab{}, previous{}, play_pause{}, next{}, volume{};
    RECT title{}, artist{}, pip{}, dismiss{};
    RECT timeline{}, elapsed{}, duration{};
    RECT volume_row{}, volume_track{};
    double reveal = 0;  // The details' opacity, 0..1.
  };
  struct Identity {
    int tab_id = -1;
    std::string source_token;
    friend bool operator==(const Identity&, const Identity&) = default;
  };

  int Scaled(int dip) const;
  Geometry Layout(const RECT& band) const;
  Part HitTest(const Geometry& g, POINT pt) const;
  bool IsVisibleTab(int tab_id) const;
  bool CanPresent(const PhiMediaSession& s) const;
  std::vector<const PhiMediaSession*> OrderedSources() const;
  void Select();
  void Show(const PhiMediaSession* chosen);
  void UpdateTargetHeight();
  void Animate();
  void SetExpanded(bool expanded);
  void ScheduleHover(bool entering);
  void Click(Part part, bool alt);
  bool Perform(phi_framework::PhiFramework::MediaAction action,
               double seconds = 0);
  void Seek(const Geometry& g, POINT pt);
  void SetVolumeAt(const Geometry& g, POINT pt);
  void Cycle(int direction);
  void BeginTrackChange();
  void EndTrackChange();
  void StartTimer(UINT_PTR* id, UINT ms);
  void StopTimer(UINT_PTR* id);
  void OnTimer(UINT_PTR id);
  static void CALLBACK TimerProc(HWND, UINT, UINT_PTR id, DWORD);
  void EnsureFonts();
  void PaintButton(HDC dc,
                   const RECT& rect,
                   const wchar_t* glyph,
                   Part part,
                   bool enabled,
                   bool toggled,
                   int glyph_dip,
                   const PhiPalette& palette);

  RAW_PTR_EXCLUSION PhiMiniPlayerHost* const host_;
  bool enabled_ = false;
  Mode mode_ = Mode::kDynamic;
  std::vector<PhiMediaSession> sessions_;
  std::vector<int> visible_tabs_;
  // Tabs most recently seen as page content first (the Mac's visit order).
  std::vector<int> visited_;
  std::optional<PhiMediaSession> item_;
  int source_count_ = 0;  // Eligible background sources, item included.
  // A paused source stays on the card until visited, dismissed or replaced.
  std::optional<Identity> retained_;
  std::map<int, std::string> dismissed_;  // tab -> source token.
  // Cards silenced in place (Mac d00df7b2): still eligible after a swipe to
  // another source, so the user keeps a way to restore the sound.
  std::map<int, std::string> silent_retained_;  // tab -> source token.
  int manual_tab_ = -1;                   // Chosen by a swipe.
  // An accepted previous/next keeps the card through the session gap.
  std::optional<Identity> pending_track_;

  bool expanded_ = false;
  bool hovering_ = false;
  bool volume_open_ = false;
  Part hot_ = Part::kNone;
  Part pressed_ = Part::kNone;
  Drag drag_ = Drag::kNone;
  POINT drag_origin_{};
  double scrub_seconds_ = -1;  // The timeline's drag preview, -1 = none.

  double height_dip_ = 0;      // Animated, what ReservedDip reports.
  double height_from_ = 0;
  int height_target_ = 0;
  uint64_t height_start_ = 0;

  UINT_PTR anim_timer_ = 0;
  UINT_PTR hover_timer_ = 0;
  bool hover_entering_ = false;
  UINT_PTR tick_timer_ = 0;
  UINT_PTR track_timer_ = 0;

  HFONT title_font_ = nullptr;
  HFONT small_font_ = nullptr;
  UINT font_dpi_ = 0;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_MINI_PLAYER_H_

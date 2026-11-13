// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_DOWNLOAD_INDICATOR_H_
#define PHIBROWSER_WIN_CLIENT_PHI_DOWNLOAD_INDICATOR_H_

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "client/phi_downloads_model.h"

namespace phi_client {

// The sidebar Downloads button's live state, the Mac's
// DownloadButtonViewModel + DownloadButtonView: when a download starts the
// arrow drops through the tray once (the "download-button" Lottie), and
// while any download runs a 14 x 2 dip accent bar under the icon shows the
// total progress, fading in and out over 200 ms. Before this the button
// was static and only the popover, while open, polled — so nothing showed
// that a click had started a download (GitHub phibrowser/phibrowser-win#13).
//
// The framework has no download event, so the shell feeds this the list it
// polls (PhiShellWindow::DownloadWatchTick). Ids are DownloadItem ids, which
// the download manager hands out in increasing order (history keeps its
// own), so "a download started" is an id above the highest one seen.
class PhiDownloadIndicator {
 public:
  // Poll cadence, running or not: the popover's. A link that turns into a
  // download is also caught by the quicker look after a page change
  // (PhiShellWindow::ScheduleRefresh); a download="" link or a script's
  // download changes no page, so this is its worst case.
  static constexpr UINT kActivePollMs = 500;
  static constexpr UINT kIdlePollMs = 500;
  // The Lottie runs frames 113..143 at 30 fps.
  static constexpr int kCueMs = 1000;
  static constexpr int kFadeMs = 200;

  // A fresh list from `source` (the browser it was read from), newest first.
  // A new source re-seeds silently: switching to a Space on another profile
  // is not a download starting. Returns what changed.
  struct Change {
    bool started = false;  // Play the cue.
    bool active_changed = false;  // Fade the bar in or out.
    bool repaint = false;  // Anything the button shows moved.
  };
  Change Update(int64_t source, const std::vector<PhiDownloadRow>& rows);

  bool active() const { return active_; }
  double progress() const { return progress_; }
  int cues() const { return cues_; }

  // The cue's position, 0..1, or < 0 when it is not running; the bar's
  // opacity, 0..1. The shell's animator sets them.
  void set_cue(double t) { cue_t_ = t; }
  double cue() const { return cue_t_; }
  void set_bar_opacity(double a) { bar_opacity_ = a; }
  double bar_opacity() const { return bar_opacity_; }

  // Paints the 24 dip artwork in `art` (the cue's frame while it runs) and,
  // while there is something to show, the bar under it. `ground` is what the
  // button stands on (its wash), for the bar's translucent track.
  void Paint(HDC dc,
             const RECT& art,
             COLORREF ink,
             COLORREF accent,
             COLORREF ground,
             int dip_px_numerator,
             int dip_px_denominator) const;

  // For agentSpace.window.layout: active, progress, cues, cueRunning,
  // barOpacity.
  std::string StateJson() const;

 private:
  int64_t source_ = 0;
  bool seeded_ = false;
  int max_id_ = -1;
  bool active_ = false;
  double progress_ = 0;
  int cues_ = 0;
  double cue_t_ = -1;
  double bar_opacity_ = 0;
};

// The Mac's DownloadsManager.updateTotalProgress: bytes over bytes for the
// running downloads of known size, the others' percentages averaged and
// weighted in by count. 0..1.
double PhiDownloadsTotalProgress(const std::vector<PhiDownloadRow>& rows);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_DOWNLOAD_INDICATOR_H_

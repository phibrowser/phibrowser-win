// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_ABOUT_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_ABOUT_WINDOW_H_

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "client/phi_secondary_window.h"

namespace phi_client {

// The Mac's About window (AboutView, opened by Phi ▸ About Phi Browser;
// PHI-2202): the mark, the app's name, "Version X (build)", the Chromium
// engine version, the copyright and the open-source acknowledgements with
// their links, centred in a 290 dip column. The Mac's draggable 3D conch is
// the Windows app mark here. Help ▸ About Phi opens it; Settings ▸ About
// stays the page with Check for update. One window app-wide: a second
// request brings it forward, as the Mac finds its open window by id.
class PhiAboutWindow : public PhiSecondaryWindow {
 public:
  // `open_url` opens a link in a browser tab (the Mac opens it in the
  // active window); it is called after the click, never during layout.
  static void Open(HWND anchor,
                   std::function<void(const std::string&)> open_url);

  ~PhiAboutWindow() override;

 private:
  // One run of laid-out text: a whole line, or one fragment of the
  // acknowledgements sentence (a link's words are their own fragments).
  struct Run {
    std::wstring text;
    RECT bounds = {};
    int size = 10;           // Design size of its font.
    int weight = FW_NORMAL;
    int link = -1;           // Index into links_, -1 for plain text.
  };
  struct Link {
    std::wstring title;
    std::string url;
  };

  explicit PhiAboutWindow(std::function<void(const std::string&)> open_url);
  bool OpenWindow(HWND anchor);
  // Lays every run out at the current dpi, sizes the window to it and
  // rebuilds the hit list.
  void Layout();
  // Appends the acknowledgements sentence wrapped to `width` px and centred
  // on `center_x`, from `*y` down; advances `*y`.
  void LayoutAcknowledgements(int center_x, int width, int* y);
  HFONT FontFor(const Run& run);

  // PhiSecondaryWindow.
  void Paint(HDC dc, const RECT& dirty) override;
  LRESULT OnWindowMessage(UINT msg,
                          WPARAM wparam,
                          LPARAM lparam,
                          bool* handled) override;
  void OnClick(int hit, POINT client_pt) override;
  void OnHover(int hit, int previous) override;
  void OnEscape() override;
  void OnDpiChanged(UINT old_dpi) override;
  void OnColorsChanged() override;
  void OnNcDestroy() override;

  std::function<void(const std::string&)> open_url_;
  std::vector<Run> runs_;
  std::vector<Link> links_;
  RECT mark_ = {};
  int hovered_link_ = -1;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_ABOUT_WINDOW_H_

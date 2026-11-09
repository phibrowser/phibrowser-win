// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SURFACE_REGISTRY_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SURFACE_REGISTRY_H_

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "client/phi_ids.h"
#include "client/phi_sidebar_model.h"

// Attached surfaces — docs/sidebar-architecture.md §4.6. A Peek or a Reader
// View is a browser of its own, hosted in a panel over the content card of
// the window whose tab it is attached to (the ORIGIN). Both kinds share one
// lifecycle, kept here rather than in two parallel maps:
//   origin closes / leaves the window   -> the surface closes
//   origin window closes                -> the surface closes
//   origin's window switches tabs       -> the surface shows only over its tab
//                                          (or its split: phi_surface_visibility.h)
//   the card moves                      -> the surface re-fits
//   address bar / Back / Forward on the origin -> the surface closes first
// The sidebar consumes only Surfaces(): origin tab -> kind, for the
// indicator on whichever row owns the origin.

namespace phi_client {

// What every hosted surface panel provides. PhiPeekPanel and PhiReaderPanel
// implement it; the registry and the coordinator never name the concrete
// kind except where a kind-specific gesture demands it.
class PhiSurfacePanel {
 public:
  virtual ~PhiSurfacePanel() = default;

  virtual SidebarSurface kind() const = 0;
  // The panel's own browser window id. Spelt int64_t here because the two
  // panels are implemented outside this model; the registry types them as
  // BrowserId at its own boundary.
  virtual int64_t window_id() const = 0;
  // The browser window and tab the surface is attached to.
  virtual int64_t origin_window_id() const = 0;
  virtual int64_t origin_tab_id() const = 0;
  // The panel's card HWND (what click-outside tests compare against).
  virtual HWND hwnd() const = 0;
  // Whether `window` is the card or something the panel considers part of
  // it — a click there is not a click outside.
  virtual bool ContainsWindow(HWND window) const;
  // Whether a button press at `screen_point`, outside the panel, is the
  // kind that dismisses it. Default: any.
  virtual bool ClickDismisses(POINT screen_point) const;

  // The origin window moved or resized: re-fit to its content card.
  virtual void Reposition() = 0;
  // Shows or hides the surface. The surface coordinator decides, by the
  // origin window's active tab and its split partner (phi_surface_
  // visibility.h): a peek covers its opener's whole split, a reader its
  // origin's pane. Showing never takes the keyboard.
  virtual void SetSurfaceShown(bool shown) = 0;
  // The hosted page changed.
  virtual void RefreshState() = 0;
  // Above the origin's browser child (the corner caps go above it after).
  virtual void Raise() = 0;
  // Reader only: the surface tab moved in after construction. Default no-op.
  virtual void BindSurfaceTab(int64_t surface_tab_id) {}
  // The tab the surface shows (its own browser's), 0 while unknown — what
  // the sidebar's row tile draws the favicon of (windows-native-ui.md §5.8).
  virtual int64_t surface_tab_id() const;
  // Peek only: how the peek is about to end, for the usage metric — the
  // closer says why before it closes the browser, and the closing itself
  // is what reports (PhiCoordinator::RetireClosingBrowser). A close nobody
  // announced is the page's own doing. Default no-op.
  virtual void NoteEndReason(const char* reason) {}

  // Whether closing the surface hands the strip back to its origin tab
  // (the Mac reveals the page the overlay covered). Off for a close that
  // moved the page into a new split without the origin, which keeps the
  // focus that split was given.
  bool hands_back_strip() const { return hands_back_strip_; }
  void set_hands_back_strip(bool hands_back) { hands_back_strip_ = hands_back; }

 private:
  bool hands_back_strip_ = true;
};

class PhiSurfaceRegistry {
 public:
  struct Origin {
    SidebarSurface kind = SidebarSurface::kContent;
    BrowserId window_id;  // The panel browser.
    BrowserId origin_window_id;
    TabId origin_tab_id;
    bool hands_back_strip = true;  // PhiSurfacePanel::hands_back_strip.
  };

  PhiSurfaceRegistry();
  ~PhiSurfaceRegistry();
  PhiSurfaceRegistry(const PhiSurfaceRegistry&) = delete;
  PhiSurfaceRegistry& operator=(const PhiSurfaceRegistry&) = delete;

  // Takes ownership. A second surface of the same kind on the same origin
  // replaces the first in the records (the caller closes the old browser).
  void Attach(std::unique_ptr<PhiSurfacePanel> panel);
  // Forgets the panel whose browser `window_id` is closing; what it was
  // attached to, for the caller to hand the strip back to. Nullopt when
  // `window_id` was no surface.
  std::optional<Origin> Detach(BrowserId window_id);

  PhiSurfacePanel* Find(BrowserId window_id) const;
  // The surface of `kind` (kContent = any) attached to `origin_tab_id`.
  PhiSurfacePanel* ForOrigin(TabId origin_tab_id,
                             SidebarSurface kind = SidebarSurface::kContent) const;
  // Every surface attached to a tab of `origin_window_id`.
  std::vector<PhiSurfacePanel*> OverWindow(BrowserId origin_window_id) const;
  // The sidebar's view: origin tab -> attached kind, for one browser. Keyed
  // by the raw id because it IS the SidebarHost::AttachedSurfaces reply.
  std::map<int, SidebarSurface> Surfaces(BrowserId origin_window_id) const;

  bool empty() const { return panels_.empty(); }
  bool HasKind(SidebarSurface kind) const;
  void ForEach(const std::function<void(PhiSurfacePanel&)>& fn) const;

 private:
  std::map<BrowserId, std::unique_ptr<PhiSurfacePanel>> panels_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SURFACE_REGISTRY_H_

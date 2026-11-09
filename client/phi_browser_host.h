// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_BROWSER_HOST_H_
#define PHIBROWSER_WIN_CLIENT_PHI_BROWSER_HOST_H_

#include <map>
#include <set>
#include <string>
#include <vector>

#include "client/phi_ids.h"

// The content area of a shell window, as ONE component
// (docs/architecture-review.md §3.5, lane 1 clusters E and G): Chromium's
// HWND adopted as a child, the browser this slot holds for each Space, the
// placeholder and content-fullscreen modes, and the overlays that ride above
// the page — the four rounded corner caps and the link-target bubble.
//
// INVARIANT: this owns BrowserId → HWND for the slot and exposes the active
// BrowserId; there is NO fallback to the slot number (phase 1 removed those).
// SetParent and subclassing happen here and nowhere else.
//
// The Space table below is pure — no HWNDs, no Win32 — so it is tested on
// the Linux host (test/model/browser_host_test.cc). A host translation unit
// defines PHI_BROWSER_HOST_MODEL_ONLY before including this and gets that
// half alone; the client build sees the whole file.

namespace phi_client {

// Which browser backs each Space in ONE slot, the order the Spaces were
// attached in, and which of them the slot is showing.
class PhiSpaceBrowsers {
 public:
  PhiSpaceBrowsers();
  ~PhiSpaceBrowsers();

  // Binds `browser` to `space_id` and makes it the shown one. A Space seen
  // for the first time joins the order; re-attaching keeps its place.
  void Attach(const std::string& space_id, BrowserId browser);
  // Shows a Space this already holds a browser for. False when it holds none.
  bool Show(const std::string& space_id);
  // Drops `browser` and every Space it backed. True when it was the one the
  // slot was SHOWING, so the caller must put another Space on screen.
  bool Forget(BrowserId browser);
  void Retire(const std::string& space_id);
  // Retire(), and the Space stops being held here: the slot keeps SHOWING
  // the browser until a replacement attaches (then TakeRetiredBrowser hands
  // it over for closing), but a switch to the Space asks for a NEW browser.
  // For a re-profiled Space's respawn and a vanished Space's retreat, whose
  // old browser may be the slot's only one — closing it first closed the
  // window (and, the last one, the app) before the replacement came.
  void RetireAndUnmap(const std::string& space_id);
  BrowserId TakeRetiredBrowser();
  bool NeedsReplacement() const;
  const std::set<BrowserId>& retiring() const { return retiring_; }

  bool Has(const std::string& space_id) const;
  BrowserId ForSpace(const std::string& space_id) const;
  // The Space whose browser is `browser`, or empty — the reverse of
  // ForSpace, for a payload that names a WINDOW and wants its Space.
  std::string SpaceOf(BrowserId browser) const;

  // The browser the slot is SHOWING: the active one, else the first Space
  // browser still attached, else invalid — a slot between its last browser
  // going and its own teardown, or one whose adoption failed. NEVER the slot
  // number: that names the browser the slot was OPENED with, which may be
  // long closed. Callers skip the framework call when this is invalid.
  BrowserId active() const;
  const std::string& active_space() const { return active_space_; }
  const std::vector<std::string>& order() const { return order_; }
  const std::map<std::string, BrowserId>& all() const { return by_space_; }
  bool empty() const { return by_space_.empty() && retiring_.empty(); }

 private:
  std::map<std::string, BrowserId> by_space_;
  std::vector<std::string> order_;
  std::string active_space_;
  BrowserId active_;
  std::set<BrowserId> retiring_;
  std::set<BrowserId> closing_retired_;
};

// The table's operations, inline so the host test needs no .cc of its own
// (the rest of this file is Win32).
inline PhiSpaceBrowsers::PhiSpaceBrowsers() = default;

inline PhiSpaceBrowsers::~PhiSpaceBrowsers() = default;

inline void PhiSpaceBrowsers::Attach(const std::string& space_id,
                                     BrowserId browser) {
  if (by_space_.find(space_id) == by_space_.end()) {
    order_.push_back(space_id);
  }
  by_space_[space_id] = browser;
  active_space_ = space_id;
  active_ = browser;
}

inline bool PhiSpaceBrowsers::Show(const std::string& space_id) {
  auto it = by_space_.find(space_id);
  if (it == by_space_.end()) {
    return false;
  }
  active_space_ = space_id;
  active_ = it->second;
  return true;
}

inline bool PhiSpaceBrowsers::Forget(BrowserId browser) {
  retiring_.erase(browser);
  closing_retired_.erase(browser);
  for (auto it = by_space_.begin(); it != by_space_.end();) {
    if (it->second == browser) {
      std::erase(order_, it->first);
      it = by_space_.erase(it);
    } else {
      ++it;
    }
  }
  if (active_ != browser) {
    return false;
  }
  active_ = BrowserId();
  return true;
}

inline bool PhiSpaceBrowsers::Has(const std::string& space_id) const {
  return by_space_.find(space_id) != by_space_.end();
}

inline void PhiSpaceBrowsers::Retire(const std::string& space_id) {
  const BrowserId browser = ForSpace(space_id);
  if (browser.valid()) {
    retiring_.insert(browser);
  }
}

inline void PhiSpaceBrowsers::RetireAndUnmap(const std::string& space_id) {
  const BrowserId browser = ForSpace(space_id);
  if (!browser.valid()) {
    return;
  }
  retiring_.insert(browser);
  by_space_.erase(space_id);
  std::erase(order_, space_id);
}

inline BrowserId PhiSpaceBrowsers::TakeRetiredBrowser() {
  if (NeedsReplacement()) {
    return BrowserId();
  }
  for (BrowserId browser : retiring_) {
    if (closing_retired_.insert(browser).second) {
      return browser;
    }
  }
  return BrowserId();
}

inline bool PhiSpaceBrowsers::NeedsReplacement() const {
  if (retiring_.empty()) {
    return false;
  }
  for (const auto& [space_id, browser] : by_space_) {
    if (browser.valid() && !retiring_.contains(browser)) {
      return false;
    }
  }
  return true;
}

inline BrowserId PhiSpaceBrowsers::ForSpace(const std::string& space_id) const {
  auto it = by_space_.find(space_id);
  return it != by_space_.end() ? it->second : BrowserId();
}

inline std::string PhiSpaceBrowsers::SpaceOf(BrowserId browser) const {
  for (const auto& [space, id] : by_space_) {
    if (id == browser) {
      return space;
    }
  }
  return std::string();
}

inline BrowserId PhiSpaceBrowsers::active() const {
  if (active_.valid()) {
    return active_;
  }
  for (const std::string& space : order_) {
    auto it = by_space_.find(space);
    if (it != by_space_.end() && it->second.valid()) {
      return it->second;
    }
  }
  return BrowserId();
}

}  // namespace phi_client

#if !defined(PHI_BROWSER_HOST_MODEL_ONLY)

#include <windows.h>

#include <array>
#include <set>

#include "base/memory/raw_ptr_exclusion.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "client/phi_theme.h"

namespace phi_client {

// Where the content area goes this turn, as the shell computed it.
struct PhiContentLayout {
  RECT content = {};  // The page's rect in the shell's client coords.
  RECT client = {};   // The whole client rect (the ground gradient's height).
  bool top_caps = false;   // The card's TOP corners are the content's here.
  bool right_caps = true;  // False when the pane is flush with the window's edge.
  int corner_radius = 0;   // Already scaled.
  // The child awaiting a Space swap's slide-end reveal, or null. It is
  // PARKED outside the client area rather than hidden — the parent clips
  // children, so it stays invisible even if Chromium shows it on its own
  // (fresh browsers do, post-creation).
  HWND parked = nullptr;
};

// What the content host needs from the shell window that owns it. A narrow
// interface rather than `friend`: the shell implements it privately. Every
// name carries the component's prefix — the shell implements one such
// interface per component and they must not collide.
class PhiBrowserHostShell {
 public:
  virtual HWND BrowserHostShellWindow() const = 0;
  virtual int BrowserHostScaled(int design_units) const = 0;
  virtual PhiPalette BrowserHostPalette() const = 0;
  // This turn's content and client rects (the shell's cached layout).
  virtual RECT BrowserHostContentRect() = 0;
  virtual RECT BrowserHostClientRect() = 0;
  // The 11pt face the status bubble sets its url in.
  virtual HFONT BrowserHostStatusFont() const = 0;
  virtual phi_framework::PhiFramework* BrowserHostFramework() const = 0;
  // A hidden agent surface shows no chrome (no status bubble).
  virtual bool BrowserHostIsAgent() const = 0;

  // Re-derives the geometry and repositions everything (LayoutContents).
  virtual void BrowserHostRelayout() = 0;
  // The rows this window shows changed with the browser.
  virtual void BrowserHostRefreshTabs() = 0;
  // Placeholder mode is a layout input (the bookmark bar's "on new tab", the
  // native new-tab page): the posted rebuild turn re-reads it.
  virtual void BrowserHostScheduleRefresh() = 0;

  // --- The Space swap the shell animates -----------------------------------
  // Arms the band slide and the palette ramp for the Space being entered.
  // `entering` is its browser — the one being attached, not yet in the
  // Space table when the swap starts (the comfortable slide fetches the
  // entering page from it).
  virtual void BrowserHostBeginSpaceSwap(const std::string& space_id,
                                         BrowserId entering) = 0;
  // A slide is running, so the incoming child is parked until it lands.
  virtual bool BrowserHostSwapRunning() const = 0;
  // The children the running slide reveals and conceals.
  virtual void BrowserHostNoteSwapChildren(HWND reveal, HWND conceal) = 0;
  // A live create-Space theme preview ends at the first real switch.
  virtual void BrowserHostEndSpacePreview() = 0;
  // Remembers which window shows a Space (lazy restore), and which Space
  // this slot is on (the restored-session pick).
  virtual void BrowserHostNoteSpaceWindow(const std::string& space_id,
                                          BrowserId browser) = 0;
  virtual void BrowserHostNoteActiveSpace(const std::string& space_id) = 0;

  // --- Handing the page's neighbours the news ------------------------------
  // The page under the address bar is being replaced: a half-typed query is
  // abandoned with it.
  virtual void BrowserHostAbandonTyping() = 0;
  // A switch with no ramp (comfortable) retints the chrome now.
  virtual void BrowserHostRetintChrome() = 0;
  // Placeholder mode: the address bar stays the way in.
  virtual void BrowserHostFocusAddressField() = 0;
  // The native new-tab page owns the keyboard while it is up: its omnibox IS
  // the page. True when it took the focus.
  virtual bool BrowserHostFocusNativePage() = 0;
  // Closing the slot's last browser destroys the shell; never from under a
  // nested loop of our own. True when the close was parked for a later turn.
  virtual bool BrowserHostDeferSpaceClose(const std::string& space_id) = 0;

 protected:
  // Non-virtual and protected: the shell OWNS the host, never the other way
  // round, so nothing ever deletes through this pointer.
  ~PhiBrowserHostShell() = default;
};

class PhiBrowserHost {
 public:
  // Re-assert the corner caps after the BROWSER child reorders itself: the
  // browser subclass posts this to the shell whenever its z-order changes
  // (Chromium raises its window on activation, which would bury the caps).
  static constexpr UINT kMsgRaiseCaps = WM_APP + 7;

  explicit PhiBrowserHost(PhiBrowserHostShell* shell);
  PhiBrowserHost(const PhiBrowserHost&) = delete;
  PhiBrowserHost& operator=(const PhiBrowserHost&) = delete;
  ~PhiBrowserHost();

  // --- Adoption ------------------------------------------------------------
  // The browser the slot was created with, backing `space_id`. Reparents it
  // into the shell and records the binding.
  void AdoptFirst(const std::string& space_id,
                  BrowserId browser,
                  HWND browser_hwnd);
  // Adopts an additional browser as `space_id`'s content and shows it,
  // hiding the previous one.
  // `agent`: the browser is an agent Space's, hosted here for a watch — it
  // is never noted as the Space's window nor registered with the
  // framework's Space registry (it stays "space.agent" there).
  void AttachSpaceBrowser(const std::string& space_id,
                          BrowserId browser,
                          HWND browser_hwnd,
                          bool agent = false);
  // Shows the Space's browser, hiding the current one. False if unknown.
  bool ShowSpace(const std::string& space_id);
  // Closes the browser this slot holds for `space_id` (a Space being
  // deleted), switching to another open Space first so the window never
  // shows a dead one. No-op when the Space is not open here. If it was the
  // slot's ONLY browser, the slot closes with it.
  void CloseSpaceBrowser(const std::string& space_id);
  void RetireSpaceBrowser(const std::string& space_id) {
    spaces_.Retire(space_id);
  }
  void RetireAndUnmapSpaceBrowser(const std::string& space_id) {
    spaces_.RetireAndUnmap(space_id);
  }
  BrowserId TakeRetiredBrowser() { return spaces_.TakeRetiredBrowser(); }
  // Drops a browser that went away; returns true if the slot still has one.
  bool ForgetBrowser(BrowserId browser);
  // Gives the shown browser Win32 focus AND Chromium activation: as a
  // WS_CHILD it never gets WM_ACTIVATE, so without this the keyboard still
  // goes to the browser that was visible before a Space switch.
  void FocusActiveBrowser();
  // The watch's two hand-overs of an agent browser between shells. The
  // agent shell RELEASES its child keeping every record (the browser is
  // still its Space; it merely has no window in it for now), the user
  // shell DETACHES the hosted agent Space (record and all; the child is
  // hidden and handed back) and the agent shell READOPTS it.
  HWND ReleaseContentChildKeepRecord();
  HWND DetachSpaceBrowser(const std::string& space_id);
  void ReadoptContentChild(HWND browser_hwnd);
  // Detaches the content child before the shell's window goes, so its
  // teardown is not entangled with ours.
  void ReleaseContentChild();

  // The Space table, read-only.
  const PhiSpaceBrowsers& spaces() const { return spaces_; }
  // The VISIBLE Chromium window, reparented as our child (null before the
  // first adoption, and between a browser going and the next showing).
  HWND content_hwnd() const { return content_hwnd_; }

  // --- Modes ---------------------------------------------------------------
  // `browser`'s last tab closed but the browser SURVIVES in placeholder mode
  // (entered), or a new tab ended that mode. The content child hides while
  // placeholding — the dead tab's final frame would linger otherwise.
  void OnPlaceholderModeChanged(BrowserId browser, bool entered);
  bool ActiveBrowserPlaceholding() const;
  // A tab's content (video/element) entered or left fullscreen: the hosted
  // child resizes itself to the MONITOR, so the shell must go borderless
  // fullscreen for the duration or the render is clipped to the window.
  void OnContentFullscreenChanged(BrowserId browser, bool fullscreen);
  void SetShellFullscreen(BrowserId browser,
                          bool fullscreen,
                          const RECT& target);
  bool content_fullscreen() const { return content_fullscreen_; }

  // --- The content area's geometry and overlays ----------------------------
  // Positions the content child, then (after the shell has repositioned
  // whatever rides between them) the four corner caps. Two calls, because
  // the caps must end up ABOVE the agent watch overlay the shell moves in
  // between — one call would leave the overlay on top.
  void PositionContentChild(const PhiContentLayout& layout);
  // Retain the outgoing browser until the replacement/slide frame has been
  // presented by DWM, rather than merely positioned or painted by the CPU.
  void HidePreviousContent(HWND previous);
  void PositionCornerCaps(const PhiContentLayout& layout);
  // Posts kMsgRaiseCaps to the shell (never touch sibling z-order from
  // inside a reorder).
  void RequestRaiseCaps();
  // Handles it: the caps and the bubble go back above the browser child.
  void RaiseOverlays();
  // Re-commits (or repaints) all four caps — the palette moved.
  void RefreshCornerCaps();
  // The pointered link target changed in `browser`'s page (empty url =
  // pointer left the link). Shows/hides the bubble when that browser is the
  // one this shell displays.
  void OnTargetURLChanged(BrowserId browser, const std::wstring& url);
  void SetStatusUrl(const std::wstring& url);
  // Painted by their own window procedures.
  void PaintCornerCap(HWND cap, HDC dc);
  void PaintStatusBubble(HWND bubble, HDC dc);

 private:
  // Strips the top-level chrome/styles off content_hwnd_ and makes it a
  // clipped child of the shell.
  void AdoptContentChild();
  // Commits cap `index` as a per-pixel-alpha layer (an antialiased arc);
  // false when the layer cannot be built, and the cap keeps its region cut.
  bool CommitCornerCap(int index);
  int Scaled(int design_units) const;
  HWND shell_hwnd() const;

  RAW_PTR_EXCLUSION PhiBrowserHostShell* const shell_;

  PhiSpaceBrowsers spaces_;
  // BrowserId → its adopted HWND, for the browsers of this slot only.
  std::map<BrowserId, HWND> hwnds_;
  HWND content_hwnd_ = nullptr;

  // Browsers currently in placeholder mode (empty strip, window alive).
  std::set<BrowserId> placeholder_;
  // Content-fullscreen state: the shell parks its chrome and covers the
  // monitor while a tab's content is fullscreen; placement and style are
  // restored on exit.
  bool content_fullscreen_ = false;
  bool shell_fullscreen_ = false;
  WINDOWPLACEMENT pre_fullscreen_placement_ = {};
  LONG_PTR pre_fullscreen_style_ = 0;

  // The card's four rounded corners, as shell children ABOVE the browser
  // child: the browser renders via DirectComposition, which ignores a
  // region on the child itself, but hwnd AIRSPACE is honoured.
  std::array<HWND, 4> corner_caps_ = {};
  // The Chrome-style link-target bubble at the content's bottom-left (the
  // Mac StatusURLView): a shell-child sibling raised over the browser child,
  // like the corner caps. Created on first use; text empty = hidden.
  HWND status_bubble_ = nullptr;
  std::wstring status_url_;
};

}  // namespace phi_client

#endif  // !defined(PHI_BROWSER_HOST_MODEL_ONLY)

#endif  // PHIBROWSER_WIN_CLIENT_PHI_BROWSER_HOST_H_

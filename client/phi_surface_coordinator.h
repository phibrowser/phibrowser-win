// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SURFACE_COORDINATOR_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SURFACE_COORDINATOR_H_

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_app_model.h"
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_reader.h"
#include "phibrowser-win/client/phi_sidebar_model.h"
#include "phibrowser-win/client/phi_surface_registry.h"

namespace phi_framework {
class PhiFramework;
}  // namespace phi_framework

namespace phi_client {

class PhiShellWindow;
class PhiWindowModel;

// Attached surfaces (docs/architecture-review.md §3.4, lane 1 §1.2 cluster
// 10): the Peek / Reader View panel registry, the Reader View bridge that
// talks to the Mirage extension, the two deferred hops a panel's lifecycle
// needs (expand, retire) and the click-outside dismissal decision.
//
// Invariant (§3.4): a surface is a browser of its own, addressed by
// BrowserId / TabId — this service never holds a controller pointer past a
// call, and every "which window" question goes back through the Host.
class PhiSurfaceCoordinator {
 public:
  // The app's half: which window shows a tab, which slot owns a browser,
  // where a persisted reader style goes, and the two deferred hops (which
  // ride the coordinator's message window) plus the control plane's
  // deferred reply for reader.extract.
  class Host {
   public:
    virtual ~Host() = default;
    // The window whose ON-SCREEN browser holds `tab_id` (by the model, so a
    // tab hidden in a pin or bookmark cell still counts), or null.
    virtual PhiShellWindow* WindowShowingTab(TabId tab_id) = 0;
    // The reader affordance for `tab_id` may have changed — repaint the
    // window showing it, at the next idle.
    virtual void ScheduleRefreshForTab(TabId tab_id) = 0;
    // The slot `browser` belongs to, or an invalid slot.
    virtual SlotId SlotForBrowser(BrowserId browser) const = 0;
    // A reader style axis moved: persist it (the preferences own the file).
    virtual void PersistReaderStyle(const model::ReaderStyle& style) = 0;
    // The verb's deferred reply for reader.extract: its shape, then its
    // answer. Both go through the control dispatcher.
    virtual bool PendingReplyShape(int request_id,
                                   bool* document,
                                   bool* include_html) = 0;
    virtual void CompleteReaderReply(int request_id, std::string json) = 0;
    // The two deferred hops, posted on the coordinator's message window:
    // WM_PHI_READER_EXPAND (a panel's page left reader.html) and
    // WM_PHI_READER_RETIRE (a panel lost its only tab).
    virtual void PostReaderExpand(BrowserId panel) = 0;
    virtual void PostReaderRetire(BrowserId panel) = 0;
    // The deferred surface turn (WM_PHI_SURFACE_TURN): the closes and split
    // opens that may not run on the stack that asked for them
    // (RunDeferredSurfaceWork).
    virtual void PostSurfaceTurn() = 0;
    // The client's one analytics capture path (phi_client_analytics.h):
    // the harness ring, then the collector's consent gate and allow-list.
    virtual void CaptureAnalytics(const char* module,
                                  const char* event_name,
                                  base::DictValue properties) = 0;
  };

  PhiSurfaceCoordinator(const PhiWindowModel& model, Host& host);
  ~PhiSurfaceCoordinator();

  PhiSurfaceCoordinator(const PhiSurfaceCoordinator&) = delete;
  PhiSurfaceCoordinator& operator=(const PhiSurfaceCoordinator&) = delete;

  // The framework arrives with OnFrameworkReady; every operation below is a
  // no-op until it does, exactly as the coordinator's `if (!framework_)`
  // guards were.
  void Init(phi_framework::PhiFramework* framework);

  // The panel registry. Published rather than wrapped: the creation switch
  // (which stays on the coordinator, because it builds the concrete panels)
  // and the sidebar's surface indicator both address it directly — the same
  // documented exception PhiSpaceManager::mutable_model() is.
  PhiSurfaceRegistry& registry() { return surfaces_; }
  const PhiSurfaceRegistry& registry() const { return surfaces_; }
  // Reader View's app half. Holds the live reading style (one store for
  // every reader surface; persisted through the preferences) and the per-tab
  // offer/state the extension reports.
  PhiReaderBridge& reader() { return reader_; }

  // --- The bridge's seams (PhiReaderBridge calls these) --------------------
  // The app's capture path, for the reader's usage metric.
  void CaptureAnalytics(const char* module,
                        const char* event_name,
                        base::DictValue properties);
  // Answers one extension request (PhiFramework::RespondToExtensionMessage).
  void RespondToExtension(const std::string& request_id,
                          const std::string& response_json);
  // App -> extensions. False when the broadcast reached nobody.
  bool BroadcastToExtensions(const std::string& type,
                             const std::string& payload_json);
  PhiShellWindow* WindowShowingTab(TabId tab_id);
  // A reader style axis moved: persist it.
  void OnReaderStyleChanged();
  // Seeds the live style from the loaded settings.
  void SetReaderStyleFromModel(const model::ReaderStyle& style);
  void OnReaderOfferabilityChanged(TabId tab_id);
  // Shapes the framework's article into the verb's reply and answers it.
  void OnReaderArticleExtracted(int request_id, const char* json);

  // --- Reader View panels ---------------------------------------------------
  // The reader extension's surface tab has appeared in the origin's strip:
  // move it into a browser of its own and host that in a panel over the
  // origin's page. False when there is no such tab to move.
  bool PanelizeReaderSurface(TabId origin_tab_id);
  bool HasReaderPanelForOrigin(TabId origin_tab_id) const;
  void CloseReaderPanelForOrigin(TabId origin_tab_id);
  // Closes every surface of `kind` (kContent = all kinds) attached to the
  // tab: the panel browsers close, and OnBrowserWindowClosing forgets them.
  // `peek_end_reason` is what a closing Peek reports as its end — the
  // user's own close by default ("closed": the sidebar's surface badge,
  // Back/Forward, an address-bar navigation), "replaced" from a second
  // peek on the same opener. `hands_back_strip` false leaves the focused
  // tab alone instead of re-activating the origin as the panel goes.
  void CloseSurfacesForOrigin(TabId origin_tab_id,
                              SidebarSurface kind = SidebarSurface::kContent,
                              const char* peek_end_reason = "closed",
                              bool hands_back_strip = true);
  // The same close for a caller inside OnBrowserWindowCreated, where
  // contract rule 2 forbids closing a browser — and where the surface being
  // replaced may be the very page whose gesture is still on the stack (a
  // Shift-click inside a Peek). The surfaces are marked and hidden now;
  // their browsers close on the coordinator's next turn
  // (RunDeferredSurfaceWork), and UpdateReaderPanelsForWindow treats them
  // as gone meanwhile.
  void DeferCloseSurfacesForOrigin(TabId origin_tab_id,
                                   SidebarSurface kind,
                                   const char* peek_end_reason,
                                   bool hands_back_strip);
  // The deferred turn (Host::PostSurfaceTurn): the queued split opens,
  // then the queued closes.
  void RunDeferredSurfaceWork();
  // A link inside the reading surface navigated it away from reader.html:
  // hand `url` back to the origin window as a normal tab and drop the panel.
  void ExpandReaderPanelIntoTab(int64_t panel_window_id,
                                const std::string& url);
  // The deferred half of the above (WM_PHI_READER_EXPAND).
  void FinishReaderPanelExpand(BrowserId panel_window);
  // The deferred retire (WM_PHI_READER_RETIRE): a panel that lost its only
  // tab is closed rather than left parked over the origin's page.
  void RetireReaderPanel(BrowserId panel_window);
  // A panel browser entered placeholder mode (it lost its only tab). True
  // when the retire was deferred and the caller must do nothing else.
  bool DeferRetireOnPlaceholder(BrowserId browser, bool entered);
  // A window's tab strip changed: re-fit, re-hide and retire the panels
  // bound to it. Reads the model, so it runs AFTER the snapshot that reports
  // the change has been stored. Which panels show follows the Mac's rules
  // (phi_surface_visibility.h): a peek covers its opener's whole split, each
  // pane keeps its own reader, a peek in front hides the pane readers.
  void UpdateReaderPanelsForWindow(BrowserId window_id);

  // --- Split view from a surface (phibrowser-mac 89dd8a48) ------------------
  // "Open Link in Split View" (context menu or Alt-click) on a link inside
  // the surface whose browser is `surface_window` (the framework's
  // OnOpenLinkAsSplitFromSurface). A Peek's page becomes a regular tab
  // paired with a new tab on `url` — re-opened, like every Peek expand on
  // this client — and the peek closes (the Mac's openOverlaySurfaceAsSplit).
  // A reader's page cannot be a strip tab here (the framework diverts a
  // reader page out of every user strip), so the tab the reader stands over
  // pairs with the link instead and keeps its reader, now over its pane.
  // Queued for the deferred turn: the framework reports the gesture from
  // the surface page's own context-menu command or OpenURLFromTab, with
  // that page's WebContents on the stack, and the peek case closes its
  // browser.
  void OpenSurfaceLinkAsSplit(BrowserId surface_window, const std::string& url);
  // The peek's "Open as Split View" when its opener is already a pane: the
  // opener stays in its split, and a duplicate of it (its URL, on the left)
  // pairs with the peek's page as a new split. Falls back to expanding the
  // peek into a tab when there is nothing to duplicate. False when the
  // opener is in no split, for the caller's ordinary peek-beside-opener.
  bool OpenPeekBesideSplitOpener(BrowserId peek_window);
  // Back/Forward on `tab_id` with no surface of its own: closes the peek
  // bound to its split partner, which covers this pane too. False when
  // there is none.
  bool ClosePartnerPeek(BrowserId window_id, TabId tab_id);
  // The Peek on screen over `window_id`'s active tab — its own, else its
  // split partner's (PhiVisiblePeekFor, the rule UpdateReaderPanelsForWindow
  // shows panels by) — as its own browser, or an invalid id. The Mac's
  // BrowserState.visiblePeek, which pageCommandTarget and the omnibox seed
  // from. A peek marked for a deferred close is already out of the picture.
  BrowserId PresentedPeek(BrowserId window_id) const;
  // A mouse button went down in `target`: when that is inside a reader over
  // a split pane other than the focused one, focus that pane (the Mac's
  // activateOriginPaneIfClicked) — Chromium's pane under the reader never
  // sees the click, and the address bar and the focus ring follow the
  // focused tab.
  void ActivatePaneUnderPress(HWND target);

  // --- The sidebar's reader affordance --------------------------------------
  bool IsReaderOfferable(int tab_id, const std::string& url) const;
  bool IsReaderActive(int tab_id, const std::string& url) const;
  void RequestToggleReaderView(int tab_id,
                               const std::string& url,
                               const char* entry_point);
  // Right-click "Open in Reading Mode": ENTERS rather than toggles.
  void OnOpenReaderView(int64_t window_id, int64_t tab_id);
  // The reader's half of OnExtensionMessage. False leaves the message for
  // the next handler (today: nobody).
  bool HandleExtensionMessage(const std::string& request_id,
                              const std::string& sender_id,
                              const std::string& type,
                              const std::string& payload_json);

  // --- Layout and dismissal -------------------------------------------------
  void RaiseReaderPanels(SlotId slot);
  void NoteContentLayoutChanged(SlotId slot);
  // Whether any Peek card is up (the click-outside hook's cheap guard).
  bool HasPeek() const;
  // Whether any surface at all is attached (the press hook's guard).
  bool HasSurfaces() const { return !surfaces_.empty(); }
  // Dismisses the Peek card(s) floating over `shell`, skipping any whose
  // card contains `inside` (a click landing in the card itself). Returns
  // true when one was dismissed.
  bool DismissPeeksForShell(HWND shell,
                            HWND inside,
                            const POINT* screen_point = nullptr);

 private:
  // `tab`'s split partner in `window`, or an invalid id.
  TabId SplitPartnerOf(BrowserId window, TabId tab) const;
  // Opens the peek's page and a tab on `partner_url` in the opener window,
  // in that strip order (`partner_leading` puts the partner first, on the
  // left), splits the two, focuses the partner and closes the peek without
  // handing the strip back to its opener. False, touching nothing, when the
  // peek's page has no URL yet.
  bool PairPeekPageWith(PhiSurfacePanel& peek,
                        const std::string& partner_url,
                        bool partner_leading);
  // OpenSurfaceLinkAsSplit's work, on the deferred turn.
  void FinishOpenSurfaceLinkAsSplit(BrowserId surface_window,
                                    const std::string& url);
  // Whether the panel is marked for a deferred close.
  bool PendingClose(BrowserId panel) const;

  RAW_PTR_EXCLUSION const PhiWindowModel& window_model_;
  RAW_PTR_EXCLUSION Host& host_;
  // Non-owning, across a module boundary, so not a raw_ptr. Null until Init.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  // Peek and Reader View panels — browsers of their own, hosted over an
  // origin tab's page, one lifecycle for both kinds.
  PhiSurfaceRegistry surfaces_;
  // Panels whose page left reader.html, with the url to re-open, until the
  // deferred expand runs.
  std::map<BrowserId, std::string> pending_reader_expands_;
  // Surfaces marked for a close that may not run on the caller's stack,
  // until the deferred turn closes their browsers.
  std::vector<BrowserId> pending_closes_;
  // Split opens from a surface page's own gesture, until the deferred turn.
  std::vector<std::pair<BrowserId, std::string>> pending_split_opens_;
  PhiReaderBridge reader_{this};
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SURFACE_COORDINATOR_H_

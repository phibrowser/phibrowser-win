// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_COORDINATOR_H_
#define PHIBROWSER_WIN_CLIENT_PHI_COORDINATOR_H_

#include <windows.h>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_account_storage.h"
#include "phibrowser-win/client/phi_agent_space_manager.h"
#include "phibrowser-win/client/phi_chromium_sync.h"
#include "phibrowser-win/client/phi_sync_key_service.h"
#include "phibrowser-win/client/phi_import_window.h"
#include "phibrowser-win/client/phi_sync_setup_window.h"
#include "phibrowser-win/client/phi_control_server.h"
#include "phibrowser-win/client/phi_client_analytics.h"
#include "phibrowser-win/client/phi_credential_service.h"
#include "phibrowser-win/client/phi_kiosk_window.h"
#include "phibrowser-win/client/phi_notification_cards.h"
#include "phibrowser-win/client/phi_peek_panel.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_reader_panel.h"
#include "phibrowser-win/client/phi_surface_coordinator.h"
#include "phibrowser-win/client/phi_account_feedback.h"
#include "phibrowser-win/client/phi_onboarding_window.h"
#include "phibrowser-win/client/phi_feedback_window.h"
#include "phibrowser-win/client/phi_folio.h"
#include "phibrowser-win/client/phi_settings_window.h"
#include "phibrowser-win/client/phi_transcript_panel.h"
#include "phibrowser-win/client/phi_url_router.h"
#include "phibrowser-win/client/phi_app_model.h"
#include "phibrowser-win/client/phi_session_restore.h"
#include "phibrowser-win/client/phi_framework_dispatcher.h"
#include "phibrowser-win/client/phi_keyboard_accelerators.h"
#include "phibrowser-win/client/phi_local_store.h"
#include "phibrowser-win/client/phi_preferences.h"
#include "phibrowser-win/client/phi_profile_service.h"
#include "phibrowser-win/client/phi_space_manager.h"
#include "phibrowser-win/client/phi_store.h"
#include "phibrowser-win/client/phi_tab_store.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_shell_window.h"
#include "phibrowser-win/client/phi_window_model.h"
#include "phibrowser-win/client/phi_window_registry.h"

namespace phi_client {

class PhiControlDispatcher;

// The client's implementation of the framework delegate — the Windows twin of
// the Mac PhiChromiumCoordinator. Registered with the framework before
// ChromeMain; the framework calls it back on the browser UI thread.
//
// Phase 4 (docs/architecture-review.md §3.4) is turning it into a
// composition root over services: PhiPreferences, PhiSpaceManager and
// PhiUrlRouter own their domains, stores and invariants; the coordinator
// wires them (their Host/Observer seams below), keeps the app operations
// that span several of them, and still implements the shell's and the
// settings window's host interfaces by forwarding.
class PhiCoordinator : public SidebarHost,
                       public SpacesHost,
                       public ChromePrefsHost,
                       public AgentHost,
                       public WindowHost,
                       public PhiSettingsHost,
                       public PhiPreferences::Observer,
                       public PhiSpaceManager::Observer,
                       public PhiSpaceManager::Host,
                       public PhiUrlRouter::Host,
                       public PhiKeyboardAccelerators::Host,
                       public PhiAccountAndFeedback::Host,
                       public PhiOnboardingWindow::Host,
                       public PhiCredentialService::Host,
                       public PhiSessionRestore::Host,
                       public PhiProfileService::Host,
                       public PhiSyncSetupWindow::Host,
                       public PhiImportWindow::Host,
                       public PhiSurfaceCoordinator::Host,
                       public PhiAgentSpaceManager::Host,
                       public PhiFolio::Host {
 public:
  PhiCoordinator();
  ~PhiCoordinator() override;

  PhiCoordinator(const PhiCoordinator&) = delete;
  PhiCoordinator& operator=(const PhiCoordinator&) = delete;

  // The app's lifecycle (docs/architecture-review.md §3.4), strictly
  // forward. Each phase names what the coordinator may rely on:
  //   kBooting        OnFrameworkReady is running; no store is loaded.
  //   kStoresLoaded   every client store is in memory; safe to answer the
  //                   cold-start restore plan.
  //   kRestorePlanned OnColdStartRestorePlan answered (armed or not).
  //   kRestoring      the first window or the first park receipt landed.
  //   kSettled        the restore's verdict is in: orphan ghosts dropped,
  //                   the no-window rescue decided.
  //   kRunning        steady state (folded onto kSettled immediately).
  //   kClosing        the session is ending or the last window went.
  //   kFlushed        FlushForShutdown ran; every store is on disk.
  // The value order IS the allowed order: Transition refuses to move back.
  enum class AppPhase {
    kBooting,
    kStoresLoaded,
    kRestorePlanned,
    kRestoring,
    kSettled,
    kRunning,
    kClosing,
    kFlushed,
  };
  AppPhase phase() const { return phase_; }
  static const char* AppPhaseName(AppPhase phase);

  // --- The framework's callbacks --------------------------------------------
  // Reached through PhiFrameworkDispatcher, which is the delegate: it holds
  // no state and forwards each callback to the service that owns the domain
  // (docs/architecture-review.md §3.4). What is left here is what the
  // coordinator itself owns — the bootstrap, the creation switch, the
  // window-facing relays and the analytics ring.
  void OnFrameworkReady(phi_framework::PhiFramework* framework,
                        const char* user_data_dir_utf8);
  // Pushes the client's per-profile mirrors (restore-on-startup) into the
  // profile that just loaded, and the global ones (peek, display language)
  // once, on the first load — the ONE place the framework learns them, so
  // no path that happens to load a profile has to remember to.
  void OnProfileLoaded(const char* profile_id);
  // Delivered INSIDE Browser::Create (contract rule 2): adopts the HWND by
  // `creation.kind`, matching a requested window to what RequestCreation
  // filed under its token. Creates and closes no browsers. The dispatcher
  // has already captured the struct into a value.
  void AdoptCreatedWindow(const phi_framework::WindowCreation& creation);
  void RetireBrowserWindow(BrowserId browser);
  // The ONE strip/nav/split/group notification the client consumes (the
  // legacy OnTabsChanged that follows it is left unimplemented): stores the
  // snapshot in window_model_, settles the surfaces over the window and
  // posts the slot's rebuild. Delivered under contract rule 1, so the
  // framework may be called from inside it.
  void NoteWindowSnapshot(const phi_framework::WindowSnapshot& snapshot,
                          phi_framework::WindowChangeReason reason);
  void OnTargetURLChanged(int64_t window_id, const char* url);
  void OnOmniboxResults(int64_t window_id, const char* json);
  void OnImportCompleted(const char* source, bool success);
  void OnPlaceholderModeChanged(int64_t window_id, bool entered);
  void OnRefreshNewTab(int64_t window_id);
  void OnContentFullscreenChanged(int64_t window_id, bool fullscreen);
  bool OnHostedWindowShow(BrowserId browser, bool activate);
  bool OnHostedWindowFullscreen(BrowserId browser,
                                bool fullscreen,
                                const RECT& target);
  void OnContentBoundsChanged(int64_t window_id);
  // A page's pixels the comfortable Space slide asked for
  // (PhiShellWindow::StartSlideCapture): to the shell whose request it is.
  void OnTabSurfaceCaptured(int request_id,
                            const unsigned char* bgra,
                            int width,
                            int height);
  void OnExtensionsChanged(int64_t window_id);
  // The puzzle button of the shell hosting `window_id`, in screen px — the
  // anchor Chromium asks for before it shows an extension bubble. False
  // without a shell or with the button off the layout.
  bool ExtensionsButtonScreenRect(int64_t window_id, RECT* rect);
  void OnShowFeedbackRequested();
  void OnTabDistillable(int64_t window_id, int tab_id, bool distillable);
  void OnExtensionMessage(const char* request_id,
                          const char* sender_id,
                          const char* type,
                          const char* payload_json);
  // One phi_analytics event as it crossed the framework boundary, into the
  // ring below.
  void RecordAnalyticsEvent(const char* module,
                            const char* event_name,
                            const char* properties_json);
  // The client's OWN events (phi_client_analytics.h): the same path as the
  // framework's, so the harness ring and the collector see one stream.
  void CaptureAnalytics(const char* module,
                        const char* event_name,
                        base::DictValue properties) override;
  // The Mac's first_time_action: `action` succeeded — captured the FIRST
  // time only, per user data directory (PhiFirstTimeActions).
  void NoteFirstTimeAction(const char* action);
  void RequestBrowserImport() override;
  // PhiImportWindow::Host — the Mac's BrowserDataImporter.startImportData.
  bool StartBrowserImport(const PhiImportRequest& request) override;
  bool BrowserImportRunning() const override;
  // The framework delegate itself: the client's ONE entry point from the
  // browser, handed to the framework by PhiClientCreateDelegate.
  PhiFrameworkDispatcher& framework_dispatcher() {
    return framework_dispatcher_;
  }

  // --- Reader View and Peek -------------------------------------------------
  // The surfaces service (docs/architecture-review.md §3.4) and the seams
  // the panels, the settings window and the verb ladder reach it through.
  PhiSurfaceCoordinator& surface_coordinator() { return surface_coordinator_; }
  PhiReaderBridge& reader() { return surface_coordinator_.reader(); }
  PhiFolio& folio() { return folio_; }
  // The window whose ON-SCREEN browser holds `tab_id` (by the model, so a
  // tab hidden in a pin or bookmark cell still counts), or null when the
  // tab is not on screen anywhere (a background Space, or a tab already
  // gone). PhiSurfaceCoordinator::Host.
  PhiShellWindow* WindowShowingTab(TabId tab_id) override;
  // Closes every surface of `kind` (kContent = all kinds) attached to the
  // tab: the panel browsers close, and OnBrowserWindowClosing forgets them.
  void CloseSurfacesForOrigin(TabId origin_tab_id,
                              SidebarSurface kind = SidebarSurface::kContent,
                              const char* peek_end_reason = "closed",
                              bool hands_back_strip = true) {
    surface_coordinator_.CloseSurfacesForOrigin(origin_tab_id, kind,
                                                peek_end_reason,
                                                hands_back_strip);
  }
  // A link inside the reading surface navigated it away from reader.html:
  // hand `url` back to the origin window as a normal tab and drop the panel.
  // (PhiReaderPanel's seam.)
  void ExpandReaderPanelIntoTab(int64_t panel_window_id,
                                const std::string& url) {
    surface_coordinator_.ExpandReaderPanelIntoTab(panel_window_id, url);
  }

  // The control plane: the /phi-agent verb ladder, peer consent and the
  // deferred replies (docs/architecture-review.md §3.4). Built in
  // OnFrameworkReady, before the pipe server starts; the client's own UI
  // still calls verbs through it (ExecuteControlOnUi, ControlCaller::
  // kInternal) and the creation/reader paths answer deferred replies
  // through it (CompletePendingReply).
  PhiControlDispatcher& dispatcher() { return *dispatcher_; }
  PhiControlServer& control_server() { return control_server_; }

  // What a shell is constructed with: the five interfaces this coordinator
  // implements, as one struct. It outlives every controller, so the
  // pointers are non-owning.
  ShellServices SlotServices() {
    return ShellServices{this, this, this, this, this};
  }

  // The shell's five services — SidebarHost, SpacesHost, ChromePrefsHost,
  // AgentHost and WindowHost (called on the UI thread by a slot). The
  // coordinator is the composition root, so it implements all five and
  // forwards each to the service that owns it; a shell is handed
  // SlotServices() and each of its components only the ones it calls
  // (docs/architecture-review.md §3.5).
  std::vector<PhiSpaceView> Spaces() const override;
  void RequestSwitchSpace(int64_t slot, const std::string& space_id) override;
  void RequestNewWindow(int64_t slot) override;
  void NoteAppActivation(bool active) override;
  void NoteUserSpaceSwitched() override;
  std::vector<PhiAgentPipView> AgentSpaces(int64_t slot) const override;
  void RequestWatchAgentSpace(const std::string& task_id,
                              int64_t watcher_slot) override;
  void RequestLeaveWatchedAgentSpace(const std::string& space_id) override;
  void OnAgentBrowserReturned(const std::string& task_id,
                              HWND child) override;
  void RequestAgentTakeControl(const std::string& task_id) override;
  void RequestAgentHandBack(const std::string& task_id) override;
  void RequestAgentFinish(const std::string& task_id) override;
  void RequestShowTranscript(const std::string& task_id) override;

  void RequestCreateSpace(int64_t slot, bool new_profile) override;
  void RequestNewIncognitoSpace(int64_t slot) override;
  void RequestCloseIncognitoSpace(const std::string& space_id) override;
  // WindowHost's account seam (the sidebar's profile button); RequestSignIn
  // is PhiSettingsHost's as well.
  bool AccountInfo(std::string* name, std::string* email) const override;
  bool AccountSigningIn() const;
  bool AccountIsGuest() const override;
  const std::vector<uint8_t>& AccountAvatarCircle(int px) override;
  void RequestSignIn() override;
  void RequestSignOut() override;

  // --- Browser access: the onboarding gate (docs/login-port.md §8) --------
  // The Mac's BrowserAccessState. Login required = no shell is shown; the
  // onboarding window is the only window up.
  enum class Access { kLoginRequired, kGuest, kSignedIn };
  Access access() const { return access_; }
  PhiOnboardingWindow* onboarding() { return onboarding_.get(); }
  bool CanUseBrowser() const { return access_ != Access::kLoginRequired; }
  // PhiOnboardingWindow::Host:
  void OnboardingSignIn() override;
  void OnboardingCancelSignIn() override;
  void OnboardingContinueAsGuest(bool share_metrics) override;
  void OnboardingSetPhase(int phase) override;
  void OnboardingFinished(bool share_metrics) override;
  void OnboardingClosed() override;
  void OnboardingOpenLink(const std::string& url) override;
  void OnboardingFocusBrowser(BrowserId browser) override;
  void RequestDefaultBrowser() override;
  PhiLayoutMode LayoutMode() const override;
  void RequestLayoutMode(PhiLayoutMode mode) override;
  const PhiPalette& Palette() const override;
  const PhiPalette& PaletteForSlot(int64_t slot) const override;
  const PhiPalette& PaletteForSpace(
      const std::string& space_id) const override;

  // Opens `url` as a foreground tab in the first shell window and raises it —
  // how a Kiosk window hands its page over when the user expands it.
  void OpenUrlInMainWindow(const std::string& url);
  // Routes `url` into `space_id` on the user's behalf — the Kiosk's Space
  // menu. The framework's own callback of this name goes straight to the
  // router through PhiFrameworkDispatcher; this is the in-app entry point.
  void OnOpenLinkInSpace(int64_t source_window_id,
                         const char* space_id,
                         const char* url);
  // The Space a Kiosk offers to hand its page to: the main window's ACTIVE
  // Space, the Space holding the default role when no window is up — the
  // Mac's KioskSpaceMenuTargetResolver.primarySpace.
  std::string KioskTargetSpaceId() const;
  // An OFF-THE-RECORD Kiosk's hand-over target: the first Incognito Space,
  // created when there is none. Never a regular Space.
  std::string KioskIncognitoSpaceId();
  void RequestSettings() override;
  void RequestUrlRulesEditor(HWND anchor) override;
  // PhiSettingsHost + PhiSyncSetupWindow::Host (the same answer for both).
  PhiSyncKeyService* SyncKeys() override;
  void RequestSyncSetup(HWND anchor) override;
  // PhiSyncSetupWindow::Host:
  void SyncSetupCreateProfile(const std::string& display_name) override;
  bool SyncProfileNameExists(const std::string& display_name) override;
  void SyncSetupClosed() override;
  void RequestFeedback(int64_t slot,
                       const PhiFeedbackContext& context) override;
  void RequestSpaceIconPicker(const std::string& space_id) override;

  void RequestCreateProfileForSpaceForm(int64_t slot,
                                        const std::string& name) override;
  std::string RequestCreateSpaceWithOptions(int64_t slot,
                                            const PhiNewSpaceSpec& spec,
                                            const char* surface) override;
  std::string RequestCreateSpaceInBackground(int64_t slot,
                                             const PhiNewSpaceSpec& spec,
                                             const char* surface) override;
  int64_t SlotAtScreenPoint(POINT screen, int64_t exclude_slot) const override;
  bool RequestDropTabOnWindow(int64_t source_slot,  // tab ids
                              int index,
                              int b_index,
                              int64_t target_slot) override;
  void NoteTearOffDrop(POINT screen) override;
  bool TakeTearOffDrop(POINT* screen) override;
  bool AlwaysShowBookmarkBar() const override;
  void SetAlwaysShowBookmarkBar(bool enabled) override;
  bool BookmarkBarOnNewTab() const override;
  void SetBookmarkBarOnNewTab(bool enabled) override;
  void NoteStoredRowSeen(const std::string& guid) override;
  std::vector<PhiStoredRowView> PinnedRows(int64_t slot) const override;
  std::vector<SidebarStoredItem> SidebarStoredItems(int64_t slot) const override;
  bool Apply(int64_t slot,
             const SidebarCommand& command,
             const SidebarTree& tree) override;
  std::map<int, SidebarSurface> AttachedSurfaces(int64_t browser) const override;
  BrowserId PresentedPeekBrowser(int64_t browser) const override;
  std::vector<PhiStoredRowView> BookmarkRows(
      int64_t slot,
      const std::string& parent_guid) const override;
  // The Library's Space-scoped rows (phi_library.cc).
  std::vector<PhiStoredRowView> PinnedRowsForSpace(
      const std::string& space_id) const override;
  std::vector<PhiStoredRowView> BookmarkRowsForSpace(
      const std::string& space_id,
      const std::string& parent_guid) const override;
  std::string AddPinnedToSpace(const std::string& space_id,
                               const std::string& title,
                               const std::string& url) override;
  std::string AddBookmarkToSpace(const std::string& space_id,
                                 const std::string& parent_guid,
                                 const std::string& title,
                                 const std::string& url,
                                 bool folder) override;
  bool TransferPinToSpace(const std::string& guid,
                          const std::string& space_id) override;
  void RequestOpen(int64_t slot, const std::string& url) override;
  void RequestBookmarkActiveTab(int64_t slot, int64_t browser) override;
  std::string AddBookmarkFolder(int64_t slot,
                                const std::string& parent_guid,
                                const std::string& title) override;
  void RemoveStoredRow(const std::string& guid) override;
  bool SetStoredRowIcon(const std::string& guid,
                        const std::string& icon) override;
  bool SetStoredRowUrl(const std::string& guid,
                       const std::string& url) override;
  bool SetStoredRowLayout(const std::string& guid,
                          const std::string& layout) override;
  // The divider axis ("vertical" / "horizontal") of the live split holding
  // `tab`, for stored rows; empty when the tab is in no split.
  std::string LiveSplitLayout(BrowserId browser, TabId tab) const;
  bool RenameStoredRow(const std::string& guid,
                       const std::string& name) override;
  void ReorderSpace(const std::string& space_id, int new_index) override;
  int SidebarWidth() const override;
  void RequestSidebarWidth(int dip) override;

  // PhiSettingsHost:
  std::string ThemeId() const override;
  PhiAppearance Appearance() const override;
  void RequestAppearance(PhiAppearance appearance) override;
  int Saturation() const override;
  std::optional<std::wstring> RebindableChordLabel(UINT command) const override;
  void SetThemeId(const std::string& id) override;
  void SetAppearance(PhiAppearance appearance) override;
  void SetSaturation(int value) override;
  // Kept for the VERB LADDER's settings.get (the settings window reads
  // PhiPreferences directly now); no longer a settings-host virtual.
  PhiNewTabBehavior NewTabBehavior() const;
  void SetNewTabBehavior(PhiNewTabBehavior behavior) override;
  void SetAutoPictureInPictureMode(const std::string& mode) override;
  bool AlwaysShowFullUrl() const override;
  void SetAlwaysShowFullUrl(bool enabled) override;
  bool SuppressCloseIncognitoSpaceWarning() const override;
  void SetSuppressCloseIncognitoSpaceWarning(bool suppress) override;
  bool ShowTabPreviews() const override;
  bool ShowOpenTabIndicators() const override;
  bool DimUnloadedTabIcons() const override;
  void NoteSlotActiveSpace(const std::string& space_id) override;
  void NoteSpaceWindow(const std::string& space_id,
                       int64_t browser_window_id) override;
  void NoteTabDistillable(int64_t window_id, int tab_id,
                          bool distillable) override;
  bool IsReaderOfferable(int tab_id, const std::string& url) const override;
  bool IsReaderActive(int tab_id, const std::string& url) const override;
  void RequestToggleReaderView(int tab_id,
                               const std::string& url,
                               const char* entry_point) override;
  void RequestCloseSurfaces(int tab_id) override;
  bool CanSaveToFolio(int tab_id) const override;
  void RequestSaveToFolio(int tab_id) override;
  void RequestOpenFolio() override;
  bool RequestClosePartnerPeek(int64_t browser, int tab_id) override;
  bool SurfaceFaviconForOrigin(int tab_id,
                               std::vector<unsigned char>* bgra,
                               int* width,
                               int* height) override;
  void NoteContentLayoutChanged(int64_t slot) override;
  void RaiseReaderPanels(int64_t slot) override;
  bool IsTabAgentDriven(int tab_id) const override;
  bool IsTabReclaimed(int tab_id) const override;
  void RequestReclaimTab(int tab_id) override;
  void RequestReleaseTab(int tab_id) override;
  void SetShowTabPreviews(bool enabled) override;
  void SetShowOpenTabIndicators(bool enabled) override;
  void SetDimUnloadedTabIcons(bool enabled) override;
  std::string LanguagePreference() const;
  void SetLanguagePreference(const std::string& tag) override;
  bool RestoreLastSession() const;
  void SetRestoreLastSession(bool enabled) override;
  void FocusWindowForBrowser(int64_t browser_window_id) override;
  bool NewTabOpensOmnibox() const override;
  std::string AddBookmarkRow(int64_t slot,
                             const std::string& parent_guid,
                             const std::string& title,
                             const std::string& url) override;
  // Pushes the persisted restore choice into every loaded profile — the
  // ON-CHANGE push. A profile that loads later gets it from OnProfileLoaded.
  void PushRestorePreference();
  bool PeekViewEnabled() const;
  void SetPeekViewEnabled(bool enabled) override;
  bool AutoPeekViewEnabled() const;
  void SetAutoPeekViewEnabled(bool enabled) override;
  // Pushes both Peek switches into the framework, which owns the two
  // decisions they gate (the context-menu item and the cross-domain
  // throttle's diversion). Run on the first profile load and on every change.
  void PushPeekPreferences();
  bool OpenKioskOnCtrlAltClick() const;
  void SetOpenKioskOnCtrlAltClick(bool enabled) override;
  void SetFolioGlobalFolder(const std::string& path) override;
  void SetFolioFolderOverride(const std::string& profile_id,
                              const std::string& path) override;
  void SetFolioAutoSaveOnSiteActions(bool enabled) override;
  void OpenFolioAutoSaveSites() override;
  // Pushes the Ctrl+Alt-click Kiosk switch the same way: the framework's
  // Browser::OpenURLFromTab is where the click is remapped.
  void PushKioskPreferences();
  // Pushes the "New tab behavior" switch the same way: the framework's
  // Browser::TabStripEmpty (refill tab or placeholder) and RequestBrowser
  // (blank tab or an empty window in placeholder mode) read it there.
  void PushNewTabPreferences();
  bool AnalyticsEnabled() const;
  void SetAnalyticsEnabled(bool enabled) override;
  PhiPinScope PinScope() const override;
  void SetPinScope(PhiPinScope scope) override;
  std::string SpaceThemeId(const std::string& space_id) const override;
  void SetSpaceIcon(const std::string& space_id,
                    const std::string& icon) override;
  void SetSpaceSaturation(const std::string& space_id, int value) override;
  void SetSettingsWindowSize(int width, int height) override;
  int SpaceSaturation(const std::string& space_id) const override;
  std::optional<PhiThemePalette> SpacePalette(
      const std::string& space_id) const override;
  void SetSpacePalette(const std::string& space_id,
                       const PhiThemePalette& palette) override;
  void PreviewSpacePalette(const std::string& space_id,
                           const PhiThemePalette& palette) override;
  bool RenameSpace(const std::string& space_id,
                   const std::string& name) override;
  bool DeleteSpace(const std::string& space_id) override;
  // The Space holding the DEFAULT role (the Mac's currentDefaultSpaceId):
  // the landing target for retreats and windows opened without a Space, the
  // pre-selected bookmark target, the badge in Settings. Starts on "1" and,
  // when its holder is deleted, is handed to the first remaining Space —
  // persisted with the Space list so the hand-off survives relaunches.
  std::string DefaultSpaceId() const override;
  // Everything is deletable except the LAST remaining Space: retreat targets
  // and new-window fallbacks need one persisted Space to land on.
  bool CanDeleteSpace(const std::string& space_id) const override;
  // Re-binds a Space to another profile (the Mac's changeProfile): its
  // browser is closed and respawned in the new profile with its open tabs,
  // and the rows it owns move with it. Refused for the default Space, whose
  // browser is the one a fresh shell binds to.
  bool SetSpaceProfile(const std::string& space_id,
                       const std::string& profile_id) override;
  bool DeleteProfileById(const std::string& profile_id) override;
  bool RenameProfileById(const std::string& profile_id,
                         const std::string& name) override;
  void RebindShortcut(const std::string& name,
                      UINT vk,
                      bool ctrl,
                      bool shift,
                      bool alt) override;
  void RestoreAllShortcuts() override;
  void RestoreShortcut(const std::string& name) override;
  bool DeveloperMode() const;
  void SetDeveloperMode(bool enabled) override;
  bool AllowAgentControl() const override;
  void SetAllowAgentControl(bool enabled) override;
  bool AgentAutoView() const override;
  void RequestAgentAutoView(bool enabled) override;
  bool AllowAgentUserSpaces() const;
  void SetAllowAgentUserSpaces(bool enabled) override;
  void SetAgentProfileAllowed(const std::string& profile_id,
                              bool allowed) override;
  void OpenBrowserSettings() override;
  void OpenBrowserPage(const std::string& url) override;
  void OpenBrowserPageInProfile(const std::string& profile_id,
                                const std::string& url) override;
  void SetSpaceTheme(const std::string& space_id,
                     const std::string& theme_id) override;
  std::vector<std::string> ProfileSummaries() const override;
  void CreateProfileNamed(const std::string& name) override;
  bool SetSearchEngine(const std::string& profile_id,
                       const std::string& keyword) override;
  // The VERB LADDER's search-engine door: agentSpace.settings.get answers
  // these wire rows verbatim (PhiSearchEngineWireRow), so the encoding is
  // part of the control-plane contract. The settings pane takes the structs
  // off PhiProfileService.
  std::vector<std::string> SearchEngines(const std::string& profile_id) const;
  std::string DownloadDirectory(const std::string& profile_id) const;
  bool SetDownloadDirectory(const std::string& profile_id,
                            const std::string& dir) override;
  void CreateSpaceNamed(const std::string& name) override;
  bool OpenCreateSpaceWindow(const std::string& initial_profile_id,
                             HWND anchor) override;
  bool OpenLibrarySpaces() override;
  bool OpenSpaceThemeEditor(const std::string& space_id,
                            HWND owner,
                            const RECT& anchor_screen) override;
  // The URL-rule door the VERB LADDER still takes: wire rows and POSITIONAL
  // edits (agentSpace.urlRules.*, which resolve a wire id to an index of
  // their own before calling in). The settings window takes PhiUrlRouter
  // directly and addresses rules by id; when the ladder does the same these
  // five go with them.
  std::vector<std::string> URLRuleRows() const;
  void AddURLRule(const std::string& space_id,
                  const std::string& host,
                  const std::string& path_prefix,
                  bool ask);
  void SetURLRulePattern(int index,
                         const std::string& host,
                         const std::string& path_prefix);
  void SetURLRuleTarget(int index, const std::string& space_id, bool ask);
  void RemoveURLRule(int index);
  void AddSettingsObserver(PhiSettingsObserver* observer) override;
  void RemoveSettingsObserver(PhiSettingsObserver* observer) override;
  // The services the settings window reads directly (PhiSettingsHost): the
  // owners of the domains the panes show. The coordinator has nothing to
  // add to a READ, and its per-field forwards were the only reason the
  // interface reached 75 virtuals (docs/architecture-review.md §3.5).
  const PhiPreferences& Preferences() const override;
  const PhiSpaceManager& SpaceList() const override;
  const PhiProfileService& ProfileList() const override;
  PhiAccountAndFeedback& Account() override;
  PhiUrlRouter& UrlRules() override;

  // --- The control plane's view of the app ----------------------------------
  // What the verb ladder (PhiControlDispatcher) reaches: the model values and
  // registries by reference, and the coordinator operations the verbs are
  // built on. Phase 4 step one exposes them as-is so the ladder could move
  // out unchanged; the service extraction (§3.4) replaces them with the
  // services' own interfaces and must keep every one of them working
  // meanwhile.
  // The services (docs/architecture-review.md §3.4) and, through them,
  // the persisted domains. The ladder still reads the settings and writes
  // the Spaces model directly; the next extraction step gives it the
  // services' own interfaces.
  PhiPreferences& preferences() { return preferences_; }
  PhiSpaceManager& space_manager() { return *space_manager_; }
  PhiUrlRouter& url_router() { return *url_router_; }
  const model::PhiSettings& settings() const { return preferences_.settings(); }
  // A verb edited the Space LIST in the model directly (spaces.create):
  // the broadcast the manager makes for its own list edits — every
  // window's strip rebuilds, Settings relists.
  void NoteSpaceListChanged() {
    OnSpacesChanged(PhiSpaceManager::SpacesChange::kList);
  }
  model::PhiSpacesModel& spaces_model() {
    return space_manager_->mutable_model();
  }
  model::PhiConsentModel& consent() { return consent_; }
  // Window state, the shell slots, the surfaces, the stored rows.
  PhiWindowModel& window_model() { return window_model_; }
  PhiWindowRegistry& registry() { return registry_; }
  PhiSurfaceRegistry& surfaces() { return surface_coordinator_.registry(); }
  PhiTabStore& tab_store() { return tab_store_; }
  // The agent Spaces, the shadow windows and the operating mask
  // (docs/architecture-review.md §3.4). The verb ladder's seams below are
  // one-line forwarders onto it.
  PhiAgentSpaceManager& agent_manager() { return agent_manager_; }
  PhiAgentSpaces& agent_spaces() { return agent_manager_.spaces(); }
  using ShadowWindow = PhiAgentSpaceManager::ShadowWindow;
  std::map<std::string, ShadowWindow>& shadow_windows() {
    return agent_manager_.shadows();
  }
  ShadowWindow* FindShadowByWindow(BrowserId window_id) {
    return agent_manager_.FindShadowByWindow(window_id);
  }
  void TouchShadow(ShadowWindow* shadow, int ttl_seconds, uint64_t now_ms) {
    agent_manager_.TouchShadow(shadow, ttl_seconds, now_ms);
  }
  void CloseShadowWindow(const std::string& task_id, const char* reason) {
    agent_manager_.CloseShadowWindow(task_id, reason);
  }
  void CloseAgentSpace(const std::string& task_id) {
    agent_manager_.CloseAgentSpace(task_id);
  }
  void PushOwnershipChanged(const std::string& task_id) {
    agent_manager_.PushOwnershipChanged(task_id);
  }
  void RefreshAgentOverlay() { agent_manager_.RefreshOverlay(); }
  // The verb ladder's seams onto the agent host (the overrides below are
  // private): the pips repaint, the Autoview edge, the shells.
  void InvalidateAgentPips() { registry_.InvalidateUserSlots(); }
  void NoteAgentRunStateChanged() { AgentRunStateChanged(); }
  PhiShellWindow* AgentWindowShell(BrowserId window_id) {
    return ControllerForAgentWindow(window_id);
  }
  PhiShellWindow* FirstUserShell() { return FirstUserController(); }
  // The profile service (docs/architecture-review.md §3.4) and the seams the
  // verb ladder reaches it through. `PendingProfile` keeps its old spelling
  // here so the ladder's `PhiCoordinator::PendingProfile` still names it.
  PhiProfileService& profiles() { return *profile_service_; }
  using PendingProfile = PhiProfileService::PendingProfile;
  const std::deque<PendingProfile>& pending_profiles() const {
    return profile_service_->pending();
  }
  // Files a CreateProfile and asks the framework for it.
  void RequestProfile(PendingProfile pending) {
    profile_service_->RequestProfile(std::move(pending));
  }
  // The display name the framework reports for `profile_id`, or empty.
  std::string ProfileDisplayName(const std::string& profile_id) const {
    return profile_service_->ProfileDisplayName(profile_id);
  }
  // Asks the framework for a browser (PhiFramework::RequestBrowser) and files
  // `pending` under the token it returns, so OnBrowserWindowCreated can
  // finish the job when the window arrives — `kind`, the profile and the
  // deadline are stamped here. Invalid when the framework refused; then
  // nothing was filed and a waiting reply (pending.pending_reply_id) is
  // already answered create_failed.
  RequestToken RequestCreation(phi_framework::WindowKind kind,
                               const std::string& profile_id,
                               bool incognito,
                               BrowserId source_window,
                               PendingCreation pending);
  // The browser a wire `windowId` means: a slot number resolves to the
  // browser that slot is SHOWING (the Space's browser, never the slot's
  // original), anything else names a browser itself (an agent window, a
  // Space browser the caller learned from spaces.list).
  BrowserId ResolveBrowser(int64_t slot_or_browser) const;
  BrowserId ResolveBrowser(SlotId slot) const;
  // The session id of `browser`'s active tab (what the extensions API calls
  // a tab id), or invalid — from the model.
  TabId ActiveTabId(BrowserId browser) const;
  // The slot's controller; an invalid slot (the wire's 0 / no windowId)
  // falls back to the first USER window — the verb convention for "the
  // window". Null when none exists.
  PhiShellWindow* ControllerForSlot(SlotId slot);
  // The browser hosting `space_id`'s tabs (possibly hidden behind a sibling
  // Space), or invalid when the Space has no open window.
  BrowserId BrowserForSpace(const std::string& space_id) const;
  // The USER window the user is looking at: the foreground shell when it is
  // one of ours, the first user window otherwise. Null without windows.
  PhiShellWindow* FocusedUserController();
  // Surfaces `space_id` in `slot` (invalid: the first user window),
  // requesting its browser in the Space's profile when the slot has none —
  // the spaces.switch body, shared with the skill's spaces.activate.
  // `cold` reports the request path; `pending` that the browser is still
  // to arrive (a request never attaches before it returns). `reopen_urls`
  // are opened in the new browser once it attaches (a re-profiled Space's
  // tabs).
  bool SwitchSlotToSpace(SlotId slot, const std::string& space_id,
                         bool* pending, bool* cold,
                         const std::vector<std::string>* reopen_urls = nullptr);
  // Gives user Space `space_id`, which has NO window, one — the Mac's
  // SpaceManager.openWindow behind spaces.openTab on a windowless Space.
  // `activate`: the user's focused window switches to it (a fresh window of
  // its own when the user has none open). Otherwise a window of its own is
  // opened BEHIND the foreground window, never activated, so a background
  // open never moves the user's focus — this client has no hidden sibling
  // a slot could hold the Space in, as the Mac's does. `url` (a New Tab
  // URL is satisfied by the window's own seed tab) opens once it is up.
  // ALWAYS answers `reply_id`: {ok, windowId, windowOpened:true} once the
  // browser is up, else unknown_space / create_failed / create_timeout.
  // The agent notification card queue (notification.show); null before
  // the control server starts.
  PhiNotificationCards* notification_cards() {
    return notification_cards_.get();
  }
  void OpenUserSpaceWindow(const std::string& space_id,
                           const std::string& url,
                           bool activate,
                           int reply_id);
  // The browser-request half of SwitchSlotToSpace, for a Space `creation`
  // names that its slot (`creation.slot`, invalid = a shell of its own)
  // does not host yet: rides a request already in flight, rebuilds a
  // parked lazy-restore window, or asks for a fresh browser. A waiting
  // reply in `creation` is answered whatever happens.
  bool RequestSpaceBrowser(PendingCreation creation, bool* pending);
  // Opens a shell on the default Space when a restore left no user window.
  void OpenDefaultShellIfNoUserWindow(const char* reason);
  // Publishes the Space list (with each Space's open browser id) into the
  // framework for the page context menu's "Open Link in Space" submenu,
  // and the routing table with it. COALESCED: the push runs once on the
  // next posted turn however many callers ask in this one (§3.4).
  void PushSpaceMenu() override;
  void RebuildSpacePalettes();
  // The (profile, space) a slot's pinned/bookmark rows belong to.
  bool OwnerForSlot(SlotId slot, std::string* profile_id,
                    std::string* space_id) const;
  // The active tab's favicon as base64 BGRA, for storing on a pinned/bookmark
  // row. A pinned tab has no live tab to ask later.
  std::string CaptureFavicon(SlotId slot, int* width, int* height);
  // The same for the active tab of `browser` — the slot's own, or a Peek's
  // (whose page Bookmark This Tab stores while the peek is presented).
  std::string CaptureFavicon(BrowserId browser, int* width, int* height);
  // The url the slot is showing right now (the model's nav state).
  std::string LiveUrl(SlotId slot) const;
  void StoreSplitInternal(SlotId slot, TabId tab_a, TabId tab_b,
                          bool as_bookmark, int insert_index,
                          bool bind_bookmark);
  // The pin scope's truth is the tab store (its rows re-own themselves on a
  // change); the preferences carry a write-through mirror so the two files
  // never disagree — called after every tab-store change and load.
  void MirrorPinScope();
  // --- Coalesced saves -----------------------------------------------------
  // Every client store the coordinator writes. A mutation marks its domain
  // dirty (ScheduleSave); ONE timer writes every dirty domain ~250 ms later,
  // so a burst of edits (a rename typed letter by letter, three pins from
  // one split) costs one disk round-trip per file instead of one per
  // keystroke — every write here is synchronous on the UI thread. The tab
  // store keeps its own atomic writer (docs/architecture-review.md §3.4).
  enum class StoreDomain : uint32_t {
    kSettings,  // preferences_: layout, theme, prefs, shortcuts, reader...
    kSpaces,    // space_manager_: the Space list, default role, URL rules
    kRestore,   // session_restore_: remembered windows, parked, last-active
    kConsent,   // consent_: agent "always" grants, credential grants/audit
  };
  void ScheduleSave(StoreDomain domain);
  // Writes `domain` (and whatever else is dirty) now, for a change the very
  // next verb reads back through the store rather than the model: a new
  // Space has no store row, so no bookmark root, until its save lands.
  void SaveNow(StoreDomain domain);
  // The bit set of dirty StoreDomains (1u << domain) — what agentSpace.ping
  // reports as pendingSaves.
  uint32_t dirty_stores() const { return dirty_stores_; }
  // The credential service (docs/architecture-review.md §3.4) and the four
  // seams the verb ladder reaches it through — one-line forwarders, so the
  // ladder is unchanged.
  PhiCredentialService& credential_service() { return credentials_; }
  const PhiVaultHelper& vault_helper() const {
    return credentials_.vault_helper();
  }
  const PhiCredentialVault& credentials() const {
    return credentials_.credentials();
  }
  std::string HelperStatus() { return credentials_.HelperStatus(); }
  bool ResolveCredentialItem(const std::string& domain,
                             const std::string& id,
                             const std::string& search,
                             const std::string& username,
                             base::DictValue* item,
                             base::DictValue* out) {
    return credentials_.ResolveCredentialItem(domain, id, search, username,
                                              item, out);
  }
  bool PromptCredentialApproval(const std::wstring& detail,
                                model::CredentialAccessKind kind,
                                const std::string& grant_key,
                                bool once_only,
                                PhiCredentialService::Decision* decision) {
    return credentials_.PromptCredentialApproval(detail, kind, grant_key,
                                                 once_only, decision);
  }
  void AppendCredentialAudit(const std::string& scope,
                             model::CredentialAccessKind kind,
                             const char* decision) {
    credentials_.AppendCredentialAudit(scope, kind, decision);
  }
  // The Auth0 session, for the feedback harness verb's signedIn report.
  const PhiAuth& auth() const { return account_.auth(); }
  // Chromium's Phi sync: what its sync service asks the client, and the
  // nudges back (docs/user-data-parity.md S2).
  PhiChromiumSync& chromium_sync() { return chromium_sync_; }
  // The signed-in account's sync key layer (S3); null signed out, in Guest
  // Mode, or before the session is known.
  PhiSyncKeyService* sync_keys() { return sync_keys_.get(); }
  // The account / feedback service (docs/architecture-review.md §3.4).
  PhiAccountAndFeedback& account() { return account_; }
  // Harness only (agentSpace.feedback.setAccount): a stand-in account for a
  // box with no Auth0 session; empty clears. Ignored while a real session
  // is signed in.
  void SetTestFeedbackAccount(const std::string& user_id,
                              const std::string& email) {
    account_.SetTestFeedbackAccount(user_id, email);
  }
  // One phi_analytics event as it crossed the framework boundary. The client
  // has no analytics pipeline yet, so the browser's events are kept here —
  // bounded, newest last — and read back over the control socket. Without
  // this the component's sink stayed unattached on Windows and every event
  // it produced (the launch-state snapshots, renderer health) fell out of its
  // 64-entry pre-attach queue and was discarded.
  struct AnalyticsEvent {
    std::string module;
    std::string name;
    std::string properties_json;
  };
  const std::deque<AnalyticsEvent>& analytics_events() const {
    return analytics_events_;
  }
  // The last lazy-restore receipt, verbatim (the lazyRestore.* harness
  // verbs read it and clear it before a replay).
  const std::string& last_restore_receipt() const {
    return session_restore_->last_receipt();
  }
  void ClearLastRestoreReceipt() { session_restore_->ClearLastReceipt(); }
  // The lazy-restore service (docs/architecture-review.md §3.4).
  PhiSessionRestore& session_restore() { return *session_restore_; }

 private:
  static constexpr size_t kAnalyticsRingSize = 256;
  std::deque<AnalyticsEvent> analytics_events_;

  // The model's value types under the names this file has always used
  // (docs/architecture-review.md §3.4: one definition, in phi_app_model.h).
  using Space = model::Space;
  // Agent credential approval (C2). `kind` names the ROUTE the secret takes
  // — kFill: Phi types it into the page; kReveal: it is handed to the agent
  // — for the audit line and the prompt wording ONLY. For grant purposes
  // the two are the same exposure: the agent holds a CDP session on the
  // page it asked Phi to fill and can read the field back, so a fill is a
  // reveal with extra steps (docs/architecture-review.md §4 item 2).
  using CredentialAccessKind = model::CredentialAccessKind;

  // --- Lifecycle -----------------------------------------------------------
  AppPhase phase_ = AppPhase::kBooting;
  // Every shell was asked to close at once (QuitForUpdate): their windows
  // are going away as a quit, not one closed by hand, so their secondary
  // restore rows stay (RetireClosingBrowser).
  bool closing_all_windows_ = false;
  // Moves to `next`, logging the move at INFO with `reason` (one word). A
  // move backwards is refused with a WARNING and the phase kept: the phases
  // are promises the rest of the coordinator relies on, and a promise once
  // made is not taken back. Same-phase calls are silent no-ops. Returns
  // whether the phase changed.
  bool Transition(AppPhase next, const char* reason);
  // The kSettled work, reached from BOTH restore paths — the reopen
  // receipt (OnSessionRestoreSettled), the cold start's fallback timer, or,
  // when neither will fire, the first user shell's first OnWindowChanged:
  // drops the parked windows no Space claims, rescues a session left with
  // no user window, then folds kSettled onto kRunning. A later settlement
  // (a reopen while running) does the work without moving the phase.
  void SettleRestore(const char* reason);
  // The process's last browser is gone and nothing is on its way: Chromium
  // is exiting through its own path (no WM_ENDSESSION comes), so this is
  // the last chance to flush. Enters kClosing and runs FlushForShutdown.
  void MaybeEnterClosing();
  // The OnBrowserWindowClosing body proper (per-kind bookkeeping), split
  // out so MaybeEnterClosing runs after every exit of it.
  void RetireClosingBrowser(BrowserId browser);
  // Everything that must reach disk before the session ends: pending
  // coalesced saves, the telemetry spool and the feedback outbox worker.
  // Idempotent: WM_QUERYENDSESSION and WM_ENDSESSION both call it.
  void FlushForShutdown();
  // The updater's installer is running and needs the app gone
  // (WM_PHI_QUIT_FOR_UPDATE): every shell and Kiosk closes through its
  // normal path, the onboarding window too, and Chromium exits with the
  // last Browser. Nothing when the app is already closing.
  void QuitForUpdate();

  // --- The application model and its stores --------------------------------
  // The four persisted domains (docs/architecture-review.md §3.4 store
  // mapping) all belong to their services now — settings to preferences_,
  // Spaces (+ rules) to space_manager_/url_router_, restore to
  // session_restore_ — except the consent file, whose two halves have two
  // owners: the credential grants and audit are credentials_' and the agent
  // "always" paths are the control plane's, so the model and its PhiStore
  // stay here, above both. The tab store keeps its own rows and writer.
  model::PhiConsentModel consent_;      // PhiConsent.json (sealed)
  PhiStore<model::PhiConsentModel> consent_store_;
  // Binds and loads the four stores (a missing file imports the pre-phase-3
  // files it replaces; a corrupt one is quarantined and defaults stand),
  // then applies what the loaded settings drive outside the services: the
  // reader style, the Space palettes, telemetry consent.
  void LoadStores(const std::wstring& user_data_dir);

  // --- The services' seams --------------------------------------------------
  // PhiPreferences::Observer: fans a preference change out to the windows
  // (palette, layout, repaint), the framework mirrors (restore, peek,
  // language), telemetry, and the settings window.
  void OnPreferencesChanged(PhiPreferences::PreferenceChange what) override;
  // PhiSpaceManager::Observer: repaints, re-applies palettes, refreshes the
  // settings window and re-pushes the wire's Space list.
  void OnSpacesChanged(PhiSpaceManager::SpacesChange what) override;
  // PhiSpaceManager::Host: the slot retreat/respawn and the data a Space
  // owns outside the list (rows, rules, ghosts).
  void SurfaceSpaceInSlot(SlotId slot,
                          const std::string& space_id,
                          const std::vector<std::string>* reopen_urls) override;
  void ForgetSpaceData(const std::string& space_id) override;
  void RehomeSpaceData(const std::string& space_id,
                       const std::string& profile_id) override;
  // PhiUrlRouter::Host: a cold Space's browser, the coalesced pushes, and
  // the posted turn the ask prompt runs on.
  void RequestSpaceBrowser(const std::string& profile_id,
                           PendingCreation pending) override;
  std::string ResolveIncognitoRouteSpace() override;
  void SchedulePushRoutingTable() override;
  void SchedulePushSpaceMenu() override;
  void PostAskSpace() override;
  // The settings window's subscription (PhiSettingsHost): what changed
  // behind the host — the services' notifications relayed, plus the
  // domains no service owns yet (account, profiles, stored rows).
  std::vector<PhiSettingsObserver*> settings_observers_;
  void NotifySettingsObservers(PhiSettingsChange what);
  // The coalesced framework pushes (PushSpaceMenu / the router's table):
  // the bits due, flushed once by WM_PHI_PUSH_SPACES on the next turn.
  enum PushBits : uint32_t { kPushMenu = 1u << 0, kPushRouting = 1u << 1 };
  uint32_t pending_pushes_ = 0;
  void SchedulePush(uint32_t bits);
  void FlushSpacePushes();

  // --- Coalesced saves (the domains and ScheduleSave are public, above) -----
  // Writes every dirty domain now and disarms the timer. The shutdown path,
  // the timer and SaveNow are its only callers.
  void FlushDirtyStores();
  // The bit set of dirty StoreDomains (1u << domain).
  uint32_t dirty_stores_ = 0;
  // Which domain a StoreDomain bit is, for the flush loop.
  static constexpr uint32_t StoreBit(StoreDomain domain) {
    return 1u << static_cast<uint32_t>(domain);
  }
  // Keeps a pinned pair's live tags canonical. The pair is two rows and its
  // cell is named by whichever sorts first, so a swap (or, later, a synced
  // reorder) moves the cell's name: each bound tab is retagged by the row it
  // shows — `<primary>` for pane A, `<primary>#b` for pane B.
  void CanonicalizePinTags(BrowserId window_id);
  // Swaps in the url-favicon service's larger icon when it beats the row's
  // stored 16px capture.
  void UpgradeFavicon(SlotId slot,
                      const std::string& url,
                      PhiStoredRowView* view) const;
  static LRESULT CALLBACK MsgWndProc(HWND hwnd,
                                     UINT msg,
                                     WPARAM wparam,
                                     LPARAM lparam);
  // The session-end window's proc (session_hwnd_).
  static LRESULT CALLBACK SessionWndProc(HWND hwnd,
                                         UINT msg,
                                         WPARAM wparam,
                                         LPARAM lparam);

  // The framework command interface (a singleton in chrome.dll); non-owning,
  // across a module boundary, so not a raw_ptr.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  // The control plane (see dispatcher()). Declared BEFORE the socket it is
  // the delegate of, so it outlives the server's teardown.
  std::unique_ptr<PhiControlDispatcher> dispatcher_;
  // The unified control/CDP socket.
  PhiControlServer control_server_;
  // Agent notification cards (notification.show); built with the control
  // server, decisions go back over it to the asking agent only.
  std::unique_ptr<PhiNotificationCards> notification_cards_;
  // Message-only window on the UI thread: the coordinator's timers, the
  // auth worker's completion and the deferred reader-panel messages. (The
  // control verbs marshal to the dispatcher's own window.)
  HWND msg_hwnd_ = nullptr;
  // A hidden TOP-LEVEL window (never shown, no owner, off the taskbar) for
  // WM_QUERYENDSESSION / WM_ENDSESSION: those are sent to top-level windows
  // only, never to a message-only one like msg_hwnd_.
  HWND session_hwnd_ = nullptr;
  // Thread-scoped mouse hook: a click anywhere in a shell OUTSIDE its open
  // Peek card dismisses the card (the Mac's click-outside monitor). Clicks
  // reach the browser child's own HWNDs, never the shell, so a hook is the
  // only place that sees them first.
  HHOOK mouse_hook_ = nullptr;
  static LRESULT CALLBACK MouseHookProc(int code, WPARAM wparam, LPARAM lparam);
  // The shell window owning the focus HWND, or null (focus is elsewhere).
  HWND ShellForFocus() const;
  // PhiKeyboardAccelerators::Host: the focused shell and the one key the
  // accelerators hand back to the app (Esc over a Peek card).
  HWND FocusedShellHwnd() const override;
  BrowserId FocusedFindBrowser(HWND shell) const override;
  bool DismissPeeksForShell(HWND shell) override;

  // The client's one copy of framework window state (docs/architecture-
  // review.md §3.3 "client side"): every controller reads it through a
  // const pointer, so it is declared BEFORE the registry that owns them and
  // outlives them. Fed by OnWindowChanged; pulled (Refresh) for a browser
  // adopted before its first push and after a mutation whose result is
  // needed on the spot.
  PhiWindowModel window_model_;
  // The shell slots, the browser -> slot map, the kiosks and the creations
  // in flight (docs/architecture-review.md §3.4). Every "which window"
  // question goes through here.
  PhiWindowRegistry registry_;
  // Peek and Reader View panels, the Mirage bridge and the two deferred
  // panel hops (docs/architecture-review.md §3.4). Declared after the
  // window model it reads; the framework arrives through Init.
  PhiSurfaceCoordinator surface_coordinator_{window_model_, *this};
  // Folio's native half (docs/folio-port.md): the save RPC, the extension's
  // file broker and the toasts. Mirage messages reach it before the
  // surface coordinator's.
  PhiFolio folio_{this};
  // PhiFolio::Host:
  void RespondToExtension(const std::string& request_id,
                          const std::string& json) override;
  bool BroadcastToExtensions(const std::string& type,
                             const std::string& payload_json) override;
  std::optional<PhiFolio::TabInfo> FolioFindTab(int tab_id) const override;
  int64_t FolioActiveWindow() const override;
  std::string FolioProfileOfWindow(int64_t window_id) const override;
  std::vector<int64_t> FolioWindowPerProfile() const override;
  HWND FolioShellForWindow(int64_t window_id) const override;
  void FolioShowToast(int64_t window_id,
                      const std::wstring& title,
                      const std::wstring& message,
                      int duration_ms,
                      const std::wstring& action_label,
                      std::function<void()> action) override;
  void FolioOpenTab(const std::string& url) override;
  PhiPreferences& FolioPreferences() override;
  // PhiSurfaceCoordinator::Host (WindowShowingTab is public, above):
  void ScheduleRefreshForTab(TabId tab_id) override;
  SlotId SlotForBrowser(BrowserId browser) const override;
  void PersistReaderStyle(const model::ReaderStyle& style) override;
  bool PendingReplyShape(int request_id,
                         bool* document,
                         bool* include_html) override;
  void CompleteReaderReply(int request_id, std::string json) override;
  void PostReaderExpand(BrowserId panel) override;
  void PostReaderRetire(BrowserId panel) override;
  void PostSurfaceTurn() override;
  // The services (docs/architecture-review.md §3.4). The preferences need
  // nothing but their dirty callback and exist from construction (the
  // popup chrome takes a pointer to their palette in the constructor); the
  // Space manager and the router hold the framework, the registry and the
  // window model by reference, so they are built in OnFrameworkReady once
  // the framework is known — after the members they reference, and
  // declared after them so they are destroyed first.
  PhiPreferences preferences_{[this] { ScheduleSave(StoreDomain::kSettings); }};
  PhiFirstTimeActions first_time_actions_;
  // The running browser-data import (StartBrowserImport): its request, the
  // source being imported (sources[next - 1]) and what failed so far. The
  // framework completes one source at a time; RunNextImport starts the
  // next, FinishImportRun lands the bookmarks once the last is in.
  struct ImportRun {
    PhiImportRequest request;
    size_t next = 0;
    std::optional<PhiImportSource> awaiting;
    std::vector<PhiImportSource> failed;
    std::string error_code;
    uint64_t started_tick = 0;
    bool chromium_bookmarks = false;  // Some source stages bookmarks.
    // Inside a StartBrowserImport / StartFileImport call: a refusal the
    // framework reports synchronously lands in `early_result` instead of
    // re-entering RunNextImport.
    bool dispatching = false;
    std::optional<bool> early_result;
  };
  std::optional<ImportRun> import_run_;
  void RunNextImport();
  void NoteImportSourceDone(PhiImportSource source, bool success);
  void FinishImportRun();
  std::unique_ptr<PhiSpaceManager> space_manager_;
  std::unique_ptr<PhiUrlRouter> url_router_;
  // The menu-bar accelerators (lane 1 §1.2 cluster 17): the WH_KEYBOARD
  // hook, the fixed table and the Alt-solo state, over the preferences'
  // rebindable chords. Built in OnFrameworkReady, on the UI thread the hook
  // is scoped to.
  std::unique_ptr<PhiKeyboardAccelerators> accelerators_;
  // The Auth0 session, the feedback outbox and the Send Feedback window
  // (lane 1 §1.2 cluster 15). Needs nothing but its host, so it exists from
  // construction; Init (OnFrameworkReady) hands it the framework and the
  // user data dir.
  PhiAccountAndFeedback account_{*this};
  // The onboarding gate (see Access): resolved once the stores and the
  // session are loaded, before any window is announced.
  Access access_ = Access::kLoginRequired;
  std::unique_ptr<PhiOnboardingWindow> onboarding_;
  // The shadow browser the onboarding window embeds the hosted login in.
  BrowserId onboarding_browser_;
  bool onboarding_browser_pending_ = false;
  // Resolves access at startup (session + phase, guest choice, skip).
  void ResolveInitialAccess();
  // Access moved to guest / signed in: held shells show, onboarding goes.
  void GrantAccess(Access access);
  // A sign-out re-arms the gate: shells hide, onboarding shows at Login.
  void RequireLogin();
  void OpenOnboarding(PhiOnboardingWindow::Page page);
  void CloseOnboarding();
  void RequestOnboardingBrowser();
  // After every auth change: a sign-in from Guest Mode routes through the
  // account's onboarding phase; the onboarding window advances.
  void ReconcileAccessAfterAuth();

  // PhiAccountAndFeedback::Host:
  void OpenUrlInFirstUserSlot(const std::string& url) override;
  void OpenHostedLogin(const std::string& url) override;
  void NoteAccountChanged() override;
  // The shared store-tab/split bodies behind the menu (append, bookmark
  // unbound) and drag (placed + bound) entry points.
  // One cell from live tab(s), by STABLE id: a single (tab_b 0) or a split
  // (both panes), as a pin or a bookmark under `parent_guid`, at
  // `insert_index` among its siblings (INT_MAX = end), bound when `bind`.
  // Returns the new guid, empty on refusal.
  std::string StoreTabsById(SlotId slot,
                            TabId tab_a,
                            TabId tab_b,
                            bool as_bookmark,
                            const std::string& parent_guid,
                            int insert_index,
                            bool bind);
  // A live tab bound by tag to a stored row: which browser holds it, its
  // id, and whether it is the row's pane B.
  struct BoundTab {
    BrowserId browser;
    TabId tab;
    bool pane_b = false;
  };
  // Every live tab (any browser the model knows) bound to the row `guid`.
  std::vector<BoundTab> TabsBoundTo(const std::string& guid) const;
  // The Move transition table (SidebarMove).
  bool ApplyMove(SlotId slot, const SidebarMove& move, const SidebarTree& tree);
  // Opens a closed cell's pane(s) as live tab(s) bound to it; returns pane
  // A, invalid on refusal.
  TabId OpenCell(BrowserId browser, const std::string& guid);
  // SetTabTagById + a model pull, so the row rebuild this gesture posts
  // already sees the tag (see the definition for why the push is not enough).
  void TagTab(BrowserId browser, int tab_id, const char* tag);
  // Former host verbs, now internal to Apply and the agent dialect.
  bool ConvertStoredRow(SlotId slot,
                        const std::string& guid,
                        bool to_bookmark,
                        const std::string& parent_guid,
                        int index);
  bool MoveStoredRowToSpace(const std::string& guid,
                            const std::string& space_id,
                            bool clone) override;  // Also the Library's.
  // The one creation flow behind RequestCreateSpaceWithOptions (activates
  // the new Space in `slot`) and RequestCreateSpaceInBackground (does not).
  std::string CreateSpaceWithOptions(int64_t slot,
                                     const PhiNewSpaceSpec& spec,
                                     const char* surface,
                                     bool activate);
  // A slot whose window shows `space_id`, for the favicon service's
  // per-profile lookup; any user slot when none does.
  SlotId SlotShowingSpace(const std::string& space_id) const;
  void RequestTabToSpace(SlotId slot,
                         TabId tab,
                         const std::string& space_id,
                         bool move);

  // Lazy Space restore (docs/architecture-review.md §3.4): the ghosts, the
  // cold-start plan, the receipts and the last-active Space, over
  // PhiRestore.json. Holds the framework by reference, so it is built in
  // OnFrameworkReady.
  std::unique_ptr<PhiSessionRestore> session_restore_;
  // PhiSessionRestore::Host:
  std::string ActiveSpaceOfLiveWindow() const override;
  bool HasSpace(const std::string& space_id) const override;
  void ArmColdStartFallback(unsigned delay_ms) override;
  void DisarmColdStartFallback() override;
  void NoteRestorePlanned(const char* reason) override;
  void NoteRestoring() override;
  bool RestoreSettled() const override;
  void OnRestoreSettled(const char* reason) override;
  // Which Space the cold start should rebuild eagerly: the one that was
  // active when the last run ended.
  std::string LastActiveSpaceId() const;
  // Message-window (msg_hwnd_) timer ids. (Id 1 was the profile-creation
  // poll; OnProfileCreated made it redundant.)
  // Agent-space / shadow-window keep-alive sweep (the Mac's 10 s tick) —
  // also sweeps the creations in flight (registry_ and profile_service_).
  static constexpr UINT_PTR kAgentSpaceSweepTimer = 2;
  static constexpr UINT kAgentSpaceSweepIntervalMs = 10 * 1000;
  // The operating mask's idle/takeover sweep.
  static constexpr UINT_PTR kAgentDriveSweepTimer = 3;
  // (Id 4 was the deferred-verb drain; it is the dispatcher's own timer on
  // its own window now.)
  // The one coalesced-save timer (ScheduleSave / FlushDirtyStores). Armed
  // when the first domain goes dirty, never re-armed by later marks, so a
  // steady stream of edits still lands on disk every kSaveDelayMs.
  static constexpr UINT_PTR kSaveTimer = 5;
  static constexpr UINT kSaveDelayMs = 250;
  // Cold start armed an eager set: if no user window is up once the replay
  // settles, open a default shell (lane 4 §6 item 6). Armed for the replay
  // to have its say, re-armed shorter once the park list has landed.
  static constexpr UINT_PTR kColdStartFallbackTimer = 6;
  // How long a RequestBrowser / CreateProfile request may stay unanswered
  // before the sweep presumes it lost and fails any waiting reply with
  // create_timeout. The framework has no failure callback for either.
  static constexpr uint64_t kCreateTimeoutMs =
      PhiProfileService::kCreateTimeoutMs;
  // Arms the sweep while anything needs sweeping (agent Spaces, shadow
  // windows, creations in flight) and stops it once nothing does.
  void UpdateSweepTimer() override;
  // Notes the Space the user is in (the restore service's last-active
  // Space, what LastActiveSpaceId() answers before any window is live) —
  // on every switch; written by the coalesced restore save.
  void NoteActiveSpace(const std::string& space_id);
  // The global framework mirrors (peek switches, display language) are
  // pushed once, on the first OnProfileLoaded; later loads push only the
  // per-profile restore choice.
  bool global_mirrors_pushed_ = false;
  // The JSON error a waiter gets when its creation never arrived (the
  // dispatcher's deferred-reply slot, answered by the sweep).
  static constexpr char kCreateTimeoutReply[] =
      "{\"ok\":false,\"error\":\"create_timeout\"}";
  // ... and when the request could not be made at all.
  static constexpr char kCreateFailedReply[] =
      "{\"ok\":false,\"error\":\"create_failed\"}";
  // Hands the display language to the framework, which owns Chromium's own
  // locale (context menus, error pages, Accept-Language). The client catalog
  // and Chromium keep separate copies of the choice, so this runs both when
  // it changes and once at startup, where it reconciles a preference file
  // that was edited while the browser was not running.
  void PushDisplayLanguageToChromium();
  // The signed-in (or Guest) account's local data (docs/user-data-parity.md):
  // its LocalStore — Spaces, URL rules, pinned tabs, bookmarks, profiles,
  // the pin scope — and its account defaults, both under
  // <UserData>\Phi\users\<account>. Declared before tab_store_, which
  // reads the store.
  store::LocalStore local_store_;
  PhiAccountDefaults account_defaults_;
  // The account whose data is open (kGuestAccountId for Guest Mode and the
  // pre-login placeholder), and the user-data-dir it lives under.
  std::string data_account_;
  std::wstring user_data_dir_;
  // What RememberStartupProfile last wrote ("" = nothing yet this run).
  std::string startup_profile_written_;
  // Opens `account_id`'s store and defaults (creating its directory), and
  // points the Space manager, the router and the restore service at them.
  // With `reconcile_windows`, windows showing a Space the account does not
  // have retreat to its default Space (an account switch at runtime).
  void OpenDataAccount(const std::string& account_id, bool reconcile_windows);
  // The data account the current session and access choice resolve to; a
  // different one than is open is switched to.
  void SyncDataAccountToAccess();
  // Copies every Chromium profile's display name onto its ProfileModel row.
  void MirrorProfileNames();
  // Tells Chromium sync who the account is now: signed in (the session's
  // subject), or signed out (no session, or Guest Mode — the Mac answers {}
  // for a guest).
  void SyncChromiumAccount();
  PhiChromiumSync chromium_sync_;
  // Builds, keeps or retires the key layer for the account now signed in
  // (the Mac's buildSyncKeyControllerIfNeeded / invalidateSyncKeyController)
  // and starts its silent unlock when it is new.
  void UpdateSyncKeyService();
  // The key layer's snapshot moved: hand Chromium the exposed keys.
  void OnSyncKeysChanged();
  // The user's Chromium profiles as the key layer pairs them.
  std::vector<keys::LocalProfile> SyncLocalProfiles();
  std::unique_ptr<PhiSyncKeyService> sync_keys_;
  // True while Phi native sync lands bookmarks / pins into local_store_.
  bool owned_sync_applying_ = false;
  // Clears the row tag of every open tab whose stored row is gone.
  void UnbindTabsOfMissingRows();
  // Signing in from Guest Mode (docs/user-data-parity.md S6, the Mac's
  // GuestDataMigration): BeginGuestMigration closes the Guest store and
  // moves its data into `target` before the target opens;
  // FinishGuestMigration, after it opened, rebinds open tabs to the
  // imported rows and removes the Guest directory.
  void BeginGuestMigration(const std::string& target);
  void FinishGuestMigration(const std::string& target);
  // The imported rows' ids by their Guest ids (the receipt's modelGUIDs)
  // while a migration is between Begin and Finish; empty otherwise.
  std::map<std::string, std::string> guest_rebind_;
  bool guest_cleanup_pending_ = false;
  // Sync's automatic profile creations waiting for their basename, by the
  // display name requested.
  std::map<std::string, std::vector<std::function<void(const std::string&)>>>
      sync_profile_creations_;
  // The profile ids the key layer last resolved against; a change re-runs
  // the silent unlock + resolve (the Mac's $profiles subscription).
  std::set<std::string> sync_profile_ids_;
  // Pinned tabs and bookmarks as cells, over local_store_.
  PhiTabStore tab_store_;
  // The OnBrowserWindowCreated branches. Each adopts `hwnd` for `browser`.
  // A requested kind whose pending record names a slot that is gone, and a
  // peek/reader whose opener is gone, fall back to HostUserShell.
  void HostUserShell(BrowserId browser, HWND hwnd, bool incognito,
                     const std::string& first_space = std::string());
  // The Space a new user window of `profile_id` opens on when the default
  // Space is bound to ANOTHER profile ("" = the default Space, as before).
  std::string SpaceForUserBrowser(const std::string& profile_id) const;
  // Records the default Space's profile for the next launch's startup
  // browser (phi_startup_profile.h); a no-op when it is unchanged.
  void RememberStartupProfile();
  bool AttachRequestedSpaceBrowser(BrowserId browser,
                                   HWND hwnd,
                                   const PendingCreation& pending);
  // A requested Space browser with no slot to join — none was asked for
  // (the default-shell rescue, OpenUserSpaceWindow) or the slot closed
  // meanwhile: a regular shell whose first Space it is (the Mac mints a
  // slot). `pending.background` keeps it behind the foreground window.
  void HostRequestedSpaceShell(BrowserId browser,
                               HWND hwnd,
                               const PendingCreation& pending);
  // A kSpace browser whose request expired before it came, carrying URLs
  // to open: hosted as a shell of its own (its Space if it still exists in
  // the browser's profile, else where a user window of the profile lands)
  // with those URLs, instead of being closed with them.
  void HostLateSpaceBrowser(BrowserId browser,
                            HWND hwnd,
                            const phi_framework::WindowCreation& creation,
                            PendingCreation late);
  // What every requested Space browser gets once it is hosted: the URLs
  // that waited for it (bypassing URL routing) and its waiting reply.
  void FinishRequestedSpaceBrowser(BrowserId browser,
                                   const PendingCreation& pending);
  void HostAgentWindow(BrowserId browser,
                       HWND hwnd,
                       bool incognito,
                       const PendingCreation& pending);
  // A window restore rebuilt under its previous-session id: hosted on the
  // Space that remembers it — joining a slot, or a shell of its own when
  // none can take it (never as the default Space, release review B6).
  // `profile_id` is the window's own profile ("" = unknown). False when no
  // Space claims it: the caller hosts it as a user window.
  bool AttachRestoredGhost(BrowserId browser,
                           HWND hwnd,
                           BrowserId restored_from,
                           const std::string& profile_id);
  bool HostSurfacePanel(const phi_framework::WindowCreation& creation);
  // A tear-off's drop point, waiting for the new window it asked for (the
  // Mac's pendingTearOffWindowPlacement): the next shell opens around it.
  // `tear_off_drop_at_` is the tick it was noted at; 0 = none pending.
  POINT tear_off_drop_{};
  ULONGLONG tear_off_drop_at_ = 0;
  // Chromium profiles (docs/architecture-review.md §3.4): the async
  // creation with tokens, rename/delete, the listing reads, the engines
  // and the download directory. Holds the framework by reference, so it is
  // built in OnFrameworkReady.
  std::unique_ptr<PhiProfileService> profile_service_;
  // PhiProfileService::Host:
  void CompleteProfileReply(int reply_id, std::string json) override;
  void FillSpaceFormProfile(SlotId slot,
                            const std::string& profile_id) override;
  void SyncPairingProfileCreated(const std::string& display_name,
                                 const std::string& profile_id) override;
  void OpenSpaceOnNewProfile(SlotId slot,
                             const std::string& profile_id) override;
  void NoteProfilesChanged() override;
  bool AnySpaceUsesProfile(const std::string& profile_id) const override;

  // --- Agent Spaces, shadow windows and the operating mask ----------------
  // (docs/architecture-review.md §3.4, lane 1 §1.2 clusters 12 and 18.)
  PhiAgentSpaceManager agent_manager_{*this};
  // PhiAgentSpaceManager::Host:
  PhiShellWindow* ControllerForAgentWindow(BrowserId window_id) override;
  PhiShellWindow* ControllerForSlot(int64_t slot) override;
  PhiShellWindow* SlotHostingAgentTask(const std::string& task_id) override;
  void RebindAgentBrowser(BrowserId browser, int64_t slot) override;
  void ShowAgentHandoffPrompt(const std::string& task_id,
                              const std::string& message) override;
  void DismissAgentHandoffPrompt(const std::string& task_id) override;
  void AgentRunStateChanged() override;
  // View ▸ Agent Autoview's rule (the Mac's autoViewReevaluate): stay put
  // while the watched task is running or the user holds it; otherwise
  // surface the lowest-numbered running, agent-owned task. Runs on the
  // posted turn after every run-state edge, never inside one.
  void AutoViewReevaluate();
  PhiShellWindow* FirstUserController() override;
  void InvalidateUserSlots() override;
  void InvalidateAll() override;
  void BroadcastControlEvent(const std::string& type,
                             const std::string& payload_json) override;
  void CompleteAgentReply(int reply_id, std::string json) override;
  // Keep-alive sweep (kAgentSpaceSweepTimer): completes and closes expired
  // spaces and shadows, and fails the creations that never arrived.
  void SweepAgentSpaces();

  // --- Credentials (the skill's agent password surface) --------------------
  // The vault providers, the grants, the audit and the approval prompt
  // (lane 1 §1.2 cluster 5). Writes the consent file's credential half;
  // declared after consent_ (above), which it holds by reference.
  PhiCredentialService credentials_{
      consent_, *this, [this] { ScheduleSave(StoreDomain::kConsent); }};
  // PhiCredentialService::Host:
  HWND CredentialPromptOwner() override;
  void NoteCredentialConsentChanged() override;

  // The framework delegate (docs/architecture-review.md §3.4): record or
  // forward, nothing else. Declared LAST so it is destroyed first — the
  // framework can no longer reach a half-torn-down coordinator through it.
  PhiFrameworkDispatcher framework_dispatcher_{*this};
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_COORDINATOR_H_

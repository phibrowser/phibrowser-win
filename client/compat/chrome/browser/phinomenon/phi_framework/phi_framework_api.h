// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PHINOMENON_PHI_FRAMEWORK_PHI_FRAMEWORK_API_H_
#define CHROME_BROWSER_PHINOMENON_PHI_FRAMEWORK_PHI_FRAMEWORK_API_H_

#include <cstddef>
#include <cstdint>

#include "chrome/browser/phinomenon/phi_framework/phi_ids.h"

// The PhiBrowser Windows framework/client boundary — the C++ twin of the Mac
// PhiChromiumBridgeHeader.h. This header is the whole contract the client
// (phi_client.exe) shares with the framework (chrome.dll and its component
// DLLs):
//
//   PhiFrameworkDelegate    — framework → client callbacks; the client
//                             implements it (the PhiChromiumBridgeDelegate
//                             twin).
//   PhiFramework            — client → framework commands; the framework
//                             implements it (the PhiChromiumBridgeProtocol
//                             twin).
//   PhiClientCreateDelegate — the ONE symbol the CLIENT EXE exports. The
//                             framework resolves it from the main exe module
//                             (GetModuleHandle(nullptr)) on first use and calls
//                             it to obtain the delegate. Exporting from the exe
//                             — not the DLL — is what makes this work in the
//                             component dev build, where the framework impl
//                             lives in a component sub-DLL, not chrome.dll,
//                             while the exe is always one known module. Mirrors
//                             the Mac dlsym/NSClassFromString runtime bind.
//
// Kept free of windows.h: HWNDs cross as void*.
//
// THREADING AND RE-ENTRANCY CONTRACT (phibrowser-win docs/architecture-
// review.md §3.3). Every method on both interfaces is called on the browser
// UI thread except the four DevTools transport calls, which are documented
// separately. The rules the client may rely on:
//
//  1. OnWindowChanged is a NOTIFICATION: it is delivered after the tab-strip
//     mutation it reports has committed, from a fresh task, coalesced per
//     window — never from inside a TabStripModel or WebContents observer
//     walk. The client may call any PhiFramework method from inside it,
//     including closing tabs and browsers.
//  2. OnBrowserWindowCreated is delivered INSIDE Browser::Create, before the
//     window is shown: the client must adopt (reparent) the HWND there. It
//     may call SetWindowSpace, FocusBrowserWindow and the *Json getters on
//     the announced window from inside it; it must not create or close
//     browsers there. Everything the window IS (kind, opener, restore
//     origin, request token) travels in the WindowCreation struct.
//  3. RequestBrowser never delivers its announcement before it returns: the
//     window is created from a later task and announced with the request
//     token. (MoveTabToReaderPanel is the one creator left that announces
//     re-entrantly, inside the call.)
//  4. OnBrowserWindowClosing is a notification that the browser is going
//     away; the client must not close OTHER browsers from inside it.
//  5. Scratch buffers (const char* and pixel returns) are valid until the
//     next PhiFramework call — including calls made from inside a callback.
//  6. Nothing is allocated on one side and freed on the other. In a
//     non-component build the exe and chrome.dll each carry their own
//     base, libc++ and PartitionAlloc, and freeing the other module's
//     memory dies on the spot (a std::string under 23 bytes never allocates,
//     so such a bug hides in tests and crashes in the field). Parameters are
//     POD, const char*, or const T& read during the call; results are
//     framework-owned scratch (rule 5) or are written through a sink the
//     caller owns (ColdStartPlan, PhiStringSink). No standard-library type
//     appears in this header, so the two sides need not share a compiler's
//     standard library: release-gate.py's api-boundary check enforces it.
//
// This is F2 slice 1: only the window-lifecycle handshake exists yet. Tabs,
// navigation, spaces, etc. grow additively (new methods beside old — the Mac
// version-skew discipline). windowId == Browser::session_id().id().

namespace phi_framework {

class PhiFramework;
class PhiDevToolsClient;

// --- Result sinks (rule 6) ------------------------------------------------
// Where a call has to hand a variable-length result to the other side, the
// RECEIVER owns the storage and the sender writes through one of these; the
// sink object itself is owned by whoever created it and lives for the call.
// Neither is deletable through the interface.

// The client's eager-window answer to OnColdStartRestorePlan. Framework-
// owned; valid during the callback.
class ColdStartPlan {
 public:
  virtual void AddEagerWindow(int32_t window_id) = 0;

 protected:
  ~ColdStartPlan() = default;
};

// A byte sink for a document the framework produces on demand
// (DevToolsDiscoveryJson). Client-owned; Append is called on the calling
// thread, before the producing call returns, possibly more than once.
class PhiStringSink {
 public:
  virtual void Append(const char* data, size_t length) = 0;

 protected:
  ~PhiStringSink() = default;
};

// Framework → client. The client subclasses this; default bodies are no-ops so
// unimplemented callbacks are safe (the "respondsToSelector:" analogue).

// What a new browser window IS, said once by the framework instead of being
// reconstructed by the client from four creation markers and two of its own
// globals (docs/architecture-review.md §2.2, §4 item 24).
enum class WindowKind : uint8_t {
  kUser,         // a shell window Chromium opened on its own: Ctrl+N,
                 // session restore, a tear-off, the first window
  kSpace,        // a Space browser the client asked for (RequestBrowser)
  kAgent,        // an agent Space's off-screen browser (RequestBrowser)
  kShadow,       // a shadow window (RequestBrowser)
  kKiosk,        // a single-tab popup-backed browser (minimal chrome)
  kPeek,         // floats over opener_window's content card
  kReaderPanel,  // the reader page hosted over origin_window's tab
  kPopup,        // a window Chromium frames itself and the client never
                 // hosts: a window.open popup (a site's sign-in window),
                 // an app window, undocked DevTools — every non-NORMAL
                 // Browser type. Announced so the client knows the id;
                 // it must not adopt the HWND (the Mac ignores these).
};

struct WindowCreation {
  WindowId window;
  // The window's top-level HWND as an integer (this header carries no
  // windows.h and Chromium's style checker refuses a raw pointer field):
  // reinterpret_cast<HWND>(hwnd) on the client.
  uintptr_t hwnd = 0;
  WindowKind kind = WindowKind::kUser;
  // The profile's on-disk basename. Valid only for the duration of the
  // callback — copy it.
  const char* profile_id = "";
  bool incognito = false;
  // kPeek: the window and tab the link was opened from.
  // kReaderPanel: the window and tab the reader page was moved out of.
  WindowId opener_window;
  TabId opener_tab;
  // kPeek: the user asked for it by name (the context menu's "Open Link in
  // Peek View") rather than the automatic cross-site diversion deciding it.
  // The client's peek_view_opened metric reads it as its entry point.
  bool peek_explicit = false;
  // Lazy Space restore: the PREVIOUS session's id this window re-creates
  // (> 0), -1 for session restore's stand-in fallback window, 0 when the
  // creation is not restore-driven.
  int32_t restored_from = 0;
  // The token RequestBrowser returned for this window; 0 when the window
  // was not requested through it.
  RequestToken request_token;
};

// A window the client asks the framework to create. Only kSpace, kAgent,
// kShadow and kUser are requestable; the other kinds are created by
// Chromium's own flows and merely announced.
struct CreateRequest {
  WindowKind kind = WindowKind::kSpace;
  // kSpace/kAgent/kShadow: the profile basename ("" = the first profile).
  // A not-yet-loaded profile is loaded first; the announcement follows.
  const char* profile_id = "";
  // kShadow: a fresh off-the-record session under `profile_id`.
  // kSpace: an Incognito Space browser — the shared "PhiIncognitoSpace" OTR
  // session of the initial profile (`profile_id` is ignored).
  bool incognito = false;
  // kUser: the window whose profile the new window opens in.
  WindowId source_window;
};

// --- The window snapshot (phase 2, docs/architecture-review.md §3.3) -----
// ONE typed shape for everything the client used to pull as ListTabsJson,
// NavStateJson, SplitViewsJson, TabGroupsJson and WindowProfileJson, parsed
// once into the client's PhiWindowModel instead of at 29 sites. The JSON
// getters stay for the agent dialect and are serialised FROM this struct
// in the framework, so the two cannot drift.
enum class TabSurface : uint8_t { kContent, kReader };
enum class SplitLayout : uint8_t { kVertical, kHorizontal };  // "vertical" / "horizontal"
enum class TabGroupColor : uint8_t {
  kGrey, kBlue, kRed, kYellow, kGreen, kPink, kPurple, kCyan, kOrange
};

// Chromium's RAW_PTR_EXCLUSION, spelled here because this header depends on
// nothing from base/.
#if defined(__clang__)
#define PHI_ABI_RAW_POINTER __attribute__((annotate("raw_ptr_exclusion")))
#else
#define PHI_ABI_RAW_POINTER
#endif

// The snapshot crosses the boundary as these plain views. Strings are
// NUL-terminated UTF-8 and never null ("" when absent). Each array holds its
// `*_count` elements. Everything a view points at is framework-owned, with the
// lifetime of the call that delivered it: copy it to keep it
// (phi_window_snapshot.h is the owned form both sides use).
struct TabSnapshotView {
  TabId id;
  int index = -1;  // Strip position; valid only within this snapshot.
  const char* title = "";
  const char* url = "";
  const char* tag = "";       // The client's custom_value; "" = untagged.
  const char* split_id = "";  // "" = not in a split.
  const char* group_id = "";  // "" = ungrouped.
  bool active = false;
  bool loading = false;
  bool unloaded = false;  // Dormant: history not loaded yet.
  TabSurface surface = TabSurface::kContent;
  TabId origin;  // surface != kContent: the tab the surface was opened from.
  // The page's own colour, as Phi's page-chrome-color content script reports
  // it for the current document (the Mac's WebContentWrapper.pageColor). The
  // client's chrome around the page takes it. A cross-document navigation
  // keeps it until the new document reports (cleared after 3 s without a
  // report, and at once for an error page or a non-web document); a renderer
  // crash clears it. Opaque 0xFFRRGGBB when has_page_color.
  bool has_page_color = false;
  uint32_t page_color_argb = 0;
};

struct SplitSnapshotView {
  const char* id = "";
  TabId primary;
  TabId secondary;
  SplitLayout layout = SplitLayout::kVertical;
  double ratio = 0.5;
};

struct GroupSnapshotView {
  const char* id = "";
  const char* title = "";
  TabGroupColor color = TabGroupColor::kGrey;
  bool collapsed = false;
};

struct NavStateView {
  const char* url = "";
  const char* title = "";
  bool can_go_back = false;
  bool can_go_forward = false;
  bool loading = false;
};

struct WindowSnapshotView {
  WindowId window;
  uint64_t version = 0;  // Monotonic per window; bumps on every change.
  const char* profile_id = "";  // On-disk basename.
  bool incognito = false;
  NavStateView nav;  // The active tab's.
  PHI_ABI_RAW_POINTER const TabSnapshotView* tabs = nullptr;  // Strip order.
  size_t tab_count = 0;
  PHI_ABI_RAW_POINTER const SplitSnapshotView* splits = nullptr;
  size_t split_count = 0;
  PHI_ABI_RAW_POINTER const GroupSnapshotView* groups = nullptr;
  size_t group_count = 0;
};

// The split view's chrome, per window — see SetSplitViewStyle.
struct SplitViewStyle {
  uint32_t background_argb = 0;  // Outside a pane's rounded corner.
  uint32_t border_argb = 0;      // The divider's 1 DIP hairline.
  uint32_t focus_argb = 0;       // The focused pane's 2 DIP inner ring.
  int top_inset = 0;             // DIP above the panes; sides/bottom are 0.
  // Appended (2026-10-13): the divider strip's own ground, under the
  // hairline. 0 (transparent) shows background_argb there.
  uint32_t divider_ground_argb = 0;
};

// Why a snapshot changed. kBulk is always a safe answer; the finer reasons
// let the client skip work (a favicon does not move rows).
enum class WindowChangeReason : uint8_t {
  kBulk,
  kTabs,         // added / removed / moved / activated
  kNavigation,   // url, title, loading, history state
  kGroups,
  kSplits,
  kFaviconReady, // FaviconForUrlBgra's async fetch landed; nothing structural
  kPageColor,    // a tab's TabSnapshot::page_color changed; nothing structural
};

class PhiFrameworkDelegate {
 public:
  virtual ~PhiFrameworkDelegate() = default;

  // The framework command interface is ready. Called once, before any other
  // callback. The client stores `framework` (owned by the framework; do not
  // delete) to drive commands. `user_data_dir_utf8` is the browser's user data
  // directory (UTF-8, valid only for the duration of this call — copy it) —
  // where the control socket's pointer file lives.
  virtual void OnFrameworkReady(PhiFramework* framework,
                                const char* user_data_dir_utf8) {}

  // A browser window was created; `creation.hwnd` is its top-level HWND and
  // the client adopts it and draws its chrome. Delivered inside
  // Browser::Create (contract rule 2).
  virtual void OnBrowserWindowCreated(const WindowCreation& creation) {}
  // Chromium's subsequent Show/ShowInactive decides whether a newly adopted
  // shell is revealed and activated. Returns false for non-shell surfaces.
  virtual void OnHostedWindowShow(int64_t window_id,
                                  bool activate,
                                  bool* handled) {}
  // Target monitor bounds are screen pixels. Empty means the current monitor.
  virtual void OnHostedWindowFullscreen(int64_t window_id,
                                        bool fullscreen,
                                        int x,
                                        int y,
                                        int width,
                                        int height,
                                        bool* handled) {}

  // The browser window is closing; `window_id` is no longer valid afterwards.
  virtual void OnBrowserWindowClosing(int64_t window_id) {}

  // The window's tabs or navigation state changed (tab added/removed/
  // activated/moved, title/url/loading updated, groups, splits, a favicon
  // landing). Delivered under contract rule 1 (after commit, from a fresh
  // task, coalesced per window); the client may mutate the strip from
  // inside it. `snapshot` is valid only for the duration of the callback —
  // the client copies it into its model.
  virtual void OnWindowChanged(const WindowSnapshotView& snapshot,
                               WindowChangeReason reason) {}

  // The web-content context menu's "Open Link in Space <name>" was clicked.
  // Spaces are client-owned: the client activates the target Space
  // (cold-spawning its browser if needed) and opens `url` there.
  // `source_window_id` is the window the user right-clicked in (0 unknown).
  virtual void OnOpenLinkInSpace(int64_t source_window_id,
                                 const char* space_id,
                                 const char* url) {}

  // A navigation matched a Space URL rule marked "ask first", so the
  // framework cancelled it instead of routing silently. The client prompts
  // for a destination Space (`default_space_id` is the rule's target — the
  // suggested default) and re-opens `url` there through the bypass opens
  // below, or back in `source_window_id` when the user keeps it in place.
  // `source_is_new_tab` — the navigation started from a fresh tab with
  // nothing to preserve, so a keep-in-place may navigate the active tab
  // directly instead of spawning another tab.
  virtual void OnAskSpaceForURL(const char* url,
                                const char* default_space_id,
                                int64_t source_window_id,
                                bool source_is_new_tab) {}

  // A silent Space URL rule matched but its target Space has no browser in
  // the routing table (not open, or open but not the visible Space of any
  // shell), so the framework cancelled the navigation. The client surfaces
  // the Space — cold-spawning its browser if needed — and re-opens `url`
  // there with OpenTabBypassingSpaceRouting so the same rule cannot loop.
  virtual void OnRouteURLToSpace(const char* space_id,
                                 const char* url,
                                 int64_t source_window_id) {}

  // A Space URL rule re-homed a navigation that started from `window_id`'s
  // new tab, which is now left behind. The Mac resets its native NTP here;
  // the Windows client has no native NTP overlay, so the default no-op is
  // usually right.
  virtual void OnRefreshNewTab(int64_t window_id) {}

  // The link target under the pointer changed (Chromium's status-bubble
  // text). `url` is empty when the pointer leaves a link. The client draws
  // the bottom-left status bubble over the content (the Mac StatusURLView).
  virtual void OnTargetURLChanged(int64_t window_id, const char* url) {}

  // Autocomplete results for the window's running omnibox query (coalesced;
  // fires per controller update). `json`:
  //   {"input":"typed text","matches":[{"line":0,"type":"search-what-you-
  //     typed","contents":"…","description":"…","url":"…",
  //     "fillIntoEdit":"…","inline":"…","isDefault":true,
  //     "deletable":false,"hasTabMatch":false}, …]}
  // A "search-other-engine" match also carries its engine: "keyword" (the
  // shortcut typed — "bing.com") and "engine" (its display name —
  // "Microsoft Bing"). One with an EMPTY "url" is the engine-selection hint
  // the keyword provider offers for a typed keyword prefix (the Mac's
  // "Search Microsoft Bing" row): the client enters keyword search on it
  // rather than opening it.
  // Valid only for the duration of the call — copy what you keep.
  virtual void OnOmniboxResults(int64_t window_id, const char* json) {}

  // A StartBrowserImport / StartFileImport finished. `source` names it as
  // the request did ("chrome", "edge", "brave", "arc", "file"; "other" for
  // a source this API does not start). On success the client harvests the
  // imported bookmarks via AllBookmarksJson.
  virtual void OnImportCompleted(const char* source, bool success) {}

  // The window's last tab closed but the window SURVIVES in placeholder
  // mode (or a new tab ended that mode). The client blanks/restores its
  // content area — the dead tab's final frame would otherwise linger.
  virtual void OnPlaceholderModeChanged(int64_t window_id, bool entered) {}
  // A tab's CONTENT entered/left fullscreen (video/element fullscreen). The
  // hosted browser child resizes itself to the monitor, so the client must
  // fullscreen its shell for the duration or the render is clipped.
  virtual void OnContentFullscreenChanged(int64_t window_id,
                                          bool fullscreen) {}
  // The rect a tab's content draws in moved — a split formed, dissolved,
  // swapped, flipped its axis or had its divider dragged. Anything the
  // client keeps OVER a pane (its native new-tab page) re-reads
  // TabContentBoundsById. Posted after the layout that moved it, coalesced
  // per window.
  virtual void OnContentBoundsChanged(int64_t window_id) {}
  // The enabled-extension set (or its pinned state / icons) changed, or an
  // async ListExtensionsJson refresh completed — re-read the list.
  virtual void OnExtensionsChanged(int64_t window_id) {}
  // Chromium asks where the window's extensions (puzzle) button is: the
  // anchor for the extension bubbles it would hang off its own toolbar
  // button — the post-install "added to Phi" notice, the uninstall
  // confirmation, the MV2 deprecation notices. Writes the button's SCREEN
  // rect in physical pixels and sets `*found`; left false when the window
  // shows no such button, and the bubble anchors to the page's top corner
  // instead. Called synchronously while the dialog is built: answer from
  // state the client already holds, no framework calls.
  virtual void OnExtensionsButtonRectRequested(int64_t window_id,
                                               int* left,
                                               int* top,
                                               int* right,
                                               int* bottom,
                                               bool* found) {}
  // A CreateProfile finished: `profile_id` is the new profile's on-disk
  // basename, or empty when creation failed. Chromium only knows it once
  // the profile is initialised, so this is the only place it appears.
  virtual void OnProfileCreated(const char* profile_id) {}

  // A profile finished loading and is usable: once per loaded profile at
  // startup (after OnFrameworkReady, before the profile's first window is
  // announced) and again whenever a cold profile is loaded later (a Space
  // switch, CreateProfile, RequestBrowser into it). The client pushes its
  // per-profile mirrors here — restore-on-startup, peek preferences, the
  // display language — instead of from every path that happens to load a
  // profile (docs/architecture-review.md §3.4). `profile_id` is the on-disk
  // basename, valid only for the call.
  virtual void OnProfileLoaded(const char* profile_id) {}

  // --- Agent operating mask for the USER's own Spaces ---------------------
  // An agent driving a tab in one of the user's Spaces is invisible to the
  // client: page automation rides the CDP relay, whose frames the client
  // passes straight through. These are the browser telling it what it cannot
  // otherwise see (phi::PhiUserSpaceDrive).
  //
  // A CDP client DROVE `tab_id` — synthetic input, a navigation, or a
  // file-input population. Coalesced by the browser: the first command of a
  // driving session, then at most once a second while it keeps driving. Treat
  // it as "arm or re-arm the mask on this tab"; a `session_id` that names a
  // DIFFERENT tab than before has MOVED, so unmask the one it left.
  virtual void OnAgentDroveUserSpaceTab(int64_t tab_id,
                                        int64_t window_id,
                                        int64_t session_id) {}
  // That session is still working on `tab_id` WITHOUT driving it — reading
  // the DOM, evaluating script, taking a screenshot. This must NEVER raise a
  // mask: opening a tab and reading it is not claiming it. It exists so a
  // mask already up survives the gaps between an agent's rounds, which are
  // far longer than the gaps between its commands.
  virtual void OnAgentObservedUserSpaceTab(int64_t tab_id,
                                           int64_t window_id,
                                           int64_t session_id) {}
  // The driving session went away (client detached, connection died, target
  // closed) — a real close event, not a timeout's guess.
  virtual void OnAgentStoppedUserSpaceTab(int64_t tab_id,
                                          int64_t session_id) {}

  // --- Lazy Space restore -------------------------------------------------
  // A RestorePreviousSession replay settled. `receipt_json` is the RECEIPT,
  // not a prediction:
  //   {"restoredAny":true,
  //    "eagerFilterMatchedNothing":false,
  //    "parked":[{"profile":"Default","windowIds":[3,7]}, ...]}
  //
  // `parked` is read off the ghost store AS A WHOLE, not off what this replay
  // happened to divert: a profile this reopen never replayed keeps whatever an
  // earlier one parked for it, and leaving those out would strand them. So the
  // client must REPLACE its park records with this, never merge — and an empty
  // list is a meaningful answer ("nothing is parked"), not a missing one.
  //
  // `eagerFilterMatchedNothing` is true only when THIS reopen armed a
  // non-empty eager set, diverted at least one window, and matched none of
  // them — the ids named windows the session file no longer holds, so the
  // whole session parked and no window appeared. `restoredAny` still reports
  // whether a replay STARTED, which is why the two are separate.
  // READER VIEW: upstream DOM Distiller says this tab has an article worth
  // reading. POSITIVE-ONLY — the proxy forwards only "yes" — so the client
  // shows its reader affordance exactly where Reader View has something to
  // offer, which is the Mac's rule.
  virtual void OnTabDistillable(int64_t window_id,
                                int tab_id,
                                bool distillable) {}

  virtual void OnSessionRestoreSettled(const char* receipt_json) {}

  // Lazy Space restore, COLD START. Chromium is about to replay the previous
  // session and asks which windows to rebuild eagerly; everything else parks.
  // Return false to leave the replay UNFILTERED — the safe default, and the
  // right answer before the client has loaded its Space records, because a
  // wrong eager set parks windows the user expected to see. Filling the
  // vector and returning true arms the filter; an EMPTY vector is a valid
  // armed answer that parks every saved window.
  //
  // Synchronous, on the UI thread, called from inside the replay.
  //
  // `*armed` arrives false and an implementation that wants to gate sets it
  // true; leaving it alone is the unfiltered default. (Shaped as an out
  // parameter rather than a return so the do-nothing body stays empty, which
  // is what an optional delegate callback has to be.) The eager set is
  // written through `plan`, framework-owned and valid during the callback
  // (rule 6).
  virtual void OnColdStartRestorePlan(ColdStartPlan* plan, bool* armed) {}

  // The cold start's receipt, per profile as its diversion lands (a cold
  // start has no settlement callback). `parked_json` has the same shape as
  // the reopen receipt's `parked`, so both can feed one path.
  virtual void OnColdStartParkedWindows(const char* parked_json,
                                        const char* reporting_profile) {}

  // --- Product analytics ---------------------------------------------------
  // One phi_analytics event, captured anywhere in the browser process and
  // dispatched here on the UI thread in capture order. `module` names the
  // originating functional area and `event_name` is snake_case; both are
  // registered in chrome/browser/phinomenon/analytics/README.md.
  // `properties_json` is a JSON OBJECT — the component's DictValue range is
  // JSON-shaped by contract — and, like every other string here, is valid
  // only for the duration of this call.
  //
  // Chromium never talks to an analytics service itself (ADR 0008); this is
  // the single egress, and it carries no consent check. Neither does the Mac
  // relay: the metrics-reporting switch gates identity association on the
  // client side, not event flow.
  //
  // Delivery is best-effort by the component's contract — no persistence, no
  // retry, and an event racing process exit may be lost.
  virtual void OnAnalyticsEventCaptured(const char* module,
                                        const char* event_name,
                                        const char* properties_json) {}

  // A StartReaderExtraction finished. `json` is either
  //   {"ok":true,"title":"…","sourceURL":"…","contentHTML":"…",
  //    "htmlLength":N,"pageCount":N}
  // or {"ok":false,"error":"no_article_detected"} (nothing distillable on
  // the page, or the page navigated / died mid-extraction). Valid only for
  // the duration of the call.
  virtual void OnReaderArticleExtracted(int request_id, const char* json) {}

  // READER VIEW, the extension surface. Reader View lives in the Mirage
  // extension (chrome/browser/resources/phi_mirage), which extracts the
  // article in-page and renders it on a reading surface of its own; the
  // client only relays intent and state. These two carry that traffic.
  //
  // An extension addressed the app (phinomenonPrivate.sendMessageToApp) —
  // the Mac's PhiChromiumBridgeDelegate extensionMessageReceived. The client
  // MUST answer every request, with RespondToExtensionMessage (a JSON string
  // the extension's promise resolves to) or FailExtensionMessage; an
  // unanswered request sits until the extension's own timeout expires.
  // `sender_id` is the extension's id: check it before acting, since every
  // extension shares this one channel.
  virtual void OnExtensionMessage(const char* request_id,
                                  const char* sender_id,
                                  const char* type,
                                  const char* payload_json) {}

  // Right-click "Open in Reading Mode" in the web-content context menu, which
  // is Chromium's. Named by SESSION tab id (what ListTabsJson emits as "id",
  // and what the extension calls a tab id), because indices shift.
  virtual void OnOpenReaderView(int64_t window_id, int64_t tab_id) {}

  // "Open Link in Split View" (the context menu or an Alt-click) on a link
  // inside a hosted surface — a Peek, or a Reader View panel. The surface's
  // page is the only tab of a browser of its own, so Chromium cannot pair it
  // in place; the client knows the strip tab the surface stands over and
  // forms the split there (the Mac's openOverlaySurfaceAsSplit).
  // `surface_window_id` is that surface's browser; `url` is valid only for
  // the duration of the call.
  virtual void OnOpenLinkAsSplitFromSurface(int64_t surface_window_id,
                                            int64_t surface_tab_id,
                                            const char* url) {}

  // Chromium asked for the feedback form — IDC_FEEDBACK (Help ▸ Report an
  // Issue, the page context menus, the sad tab's "Send feedback") and every
  // other chrome::ShowFeedbackPage caller. The Mac's showFeedbackDialog: the
  // client shows its form for the window the user is looking at.
  virtual void OnShowFeedbackRequested() {}

  // The answer to PhiFramework::FetchFeedbackSystemLogs: Chromium's scrubbed
  // system logs as "key: value" text (what the Mac's
  // getFeedbackSystemLogsTextWithWindowId hands back). Valid only for the
  // duration of the call.
  virtual void OnFeedbackSystemLogs(int request_id, const char* text) {}

  // The answer to PhiFramework::StartTabSurfaceCapture: the page's pixels,
  // `width` x `height` BGRA rows packed tight (premultiplied, opaque for a
  // page), or 0 x 0 with a null pointer when there was nothing to copy.
  // Valid only for the duration of the call.
  virtual void OnTabSurfaceCaptured(int request_id,
                                    const unsigned char* bgra,
                                    int width,
                                    int height) {}

  // A navigation to the Phi account's Auth0 callback URL
  // (`com.phibrowser.Mac://<tenant>/macos/com.phibrowser.Mac/callback…`,
  // the redirect the Mac app registers as a URL scheme) was seen by the
  // auth-callback navigation throttle. Windows has no scheme handler; the
  // client owns the sign-in, so it takes the URL here — the code exchange
  // after a hosted login, or the return from the tenant's logout page. Return
  // `*taken` = true CANCELS the navigation (nothing else can render it);
  // left false, Chromium's external-protocol handling has it. Called
  // synchronously inside the throttle: return fast, defer the work.
  virtual void OnAuthCallbackUrl(const char* url, bool* taken) {}

  // --- Phi sync: what Chromium's sync service asks the client --------------
  // The Mac bridge delegate's getAuth0AccessTokenSyncly, getPhiSyncAccountInfo
  // and getPhiProfileSyncInfo:, behind the same PhiBridgeWrapper seam (the
  // fork's components/sync/service/phi_sync_* providers). Called on the UI
  // thread, synchronously: answer from memory, never block on the network.
  // Strings go into the sink, copied before the call returns.
  //
  // The Auth0 access token sync sends as its bearer; nothing appended when
  // there is none yet (no session, reauthentication pending, a refresh
  // under way) — sync retries with backoff, and the client calls
  // NotifyPhiAuthStateChanged once a fresh token is in.
  virtual void GetPhiSyncAccessToken(PhiStringSink* token) {}
  // The account's stable identity, independent of the token. `*state`
  // (preset to 0) is
  //   0  unresolved (the session is still being restored) — or no client;
  //   1  signed in: `subject` (the Auth0 sub) and `email` are filled;
  //   2  confirmed signed out (also Guest Mode).
  // A signed-in account whose token is withheld (reauthentication) is still 1.
  virtual void GetPhiSyncAccount(int* state,
                                 PhiStringSink* subject,
                                 PhiStringSink* email) {}
  // The resolved sync key of Chromium profile `profile_id` (its on-disk
  // basename): the account-global profile uuid and the 64-hex-char custom
  // passphrase. Nothing appended when the profile has no resolved key (not
  // paired, key layer locked, or a profile excluded from sync) — sync then
  // stays off for that profile.
  virtual void GetPhiProfileSyncInfo(const char* profile_id,
                                     PhiStringSink* uuid,
                                     PhiStringSink* passphrase) {}

  // Auto-PiP preferences, read synchronously from the client's in-memory
  // settings on the UI thread. Defaults are Normal (enabled, not parked).
  // Manual picture-in-picture is unaffected by these preferences.
  virtual void GetAutoPictureInPicturePreferences(bool* enabled,
                                                bool* park_at_edge) {}
};

// Client → framework. Implemented by the framework and handed to the client
// via PhiFrameworkDelegate::OnFrameworkReady. Grows into the command surface
// over F3.
class PhiFramework {
 public:
  virtual ~PhiFramework() = default;

  // Activate the browser and focus its active web contents. Option B routes
  // the shell window's WM_ACTIVATE here: the reparented content child never
  // enters Chromium's own activation path (IsTopLevelWindow() == false), so
  // the client must drive activation across the boundary for input/focus/IME
  // to reach the page.
  virtual void FocusBrowserWindow(int64_t window_id, bool active = true) = 0;
  // Gives the active tab's page the keyboard. A window adopted before its page
  // arrived (a Kiosk) has no activation left to deliver that focus.
  virtual void FocusActivePage(int64_t window_id) = 0;
  // Whether an agent Space's window is agent-driven (PhiWindowState): off while
  // the user holds control, so fullscreen and activation work as in any window.
  // Does nothing for any other window.
  virtual void SetAgentMode(int64_t window_id, bool enabled) = 0;
  virtual void ExitContentFullscreen(int64_t window_id) = 0;

  // Asks for a window — a Space browser, an agent Space's off-screen
  // browser, a shadow window, or a plain user window (Ctrl+N) — and returns
  // a token the announcement will echo (WindowCreation::request_token), so
  // the client keys its pending state by token instead of by "the next
  // window that arrives". Returns 0 when the request cannot be started
  // (unknown profile, unrequestable kind, shutting down). Never announces
  // before returning (contract rule 3): for a loaded profile the window is
  // created from a posted task; for a cold profile after the load
  // completes. The ONE way the client creates browsers.
  virtual RequestToken RequestBrowser(const CreateRequest& request) = 0;

  // Close the browser window (normal close path: unload handlers,
  // beforeunload prompts, session bookkeeping). OnBrowserWindowClosing fires
  // when the Browser actually goes away — the client tears down its shell
  // window THEN, not on the WM_CLOSE that initiated this.
  virtual void CloseBrowserWindow(int64_t window_id) = 0;

  // Navigate the window's active tab to `url` (UTF-8). No-op on a bad URL or
  // unknown window.
  virtual void Navigate(int64_t window_id, const char* url) = 0;

  // --- Tabs -----------------------------------------------------------------
  // The client owns tab UI, so it drives the strip through these. Tabs are
  // addressed by their STABLE session id (TabSnapshot::id, ListTabsJson
  // "id"), which follows the tab for the whole browser session while strip
  // indices shift with every open, close and move.

  // Opens a tab at the end of the strip, optionally navigating it. Returns the
  // new tab's index, or -1 on failure. Prefer OpenTab, which answers with
  // the stable TabId.
  virtual int NewTab(int64_t window_id, const char* url) = 0;

  // Serializes the window's tabs as a JSON array string:
  // [{"index":0,"id":7,"title":"…","url":"…","active":true,"loading":false},…]
  // "id" is the tab's session id — stable for the browser session while
  // indices shift with every open/close. Empty array string on an unknown
  // window. The returned pointer is valid until the next call on this
  // interface (single-threaded UI use).
  virtual const char* ListTabsJson(int64_t window_id) = 0;

  // --- Split view (Chromium native side-by-side) ---------------------------
  // Two tabs share the content area with a draggable divider; ListTabsJson
  // marks members with a shared "splitId" so the client can draw merged rows.

  // The window's splits for the phi-browser skill, keyed by STABLE tab ids
  // (ListTabsJson "id") and the skill's layout names:
  //   [{"id":"…","layout":"vertical|horizontal","ratio":0.5,
  //     "primaryTabId":12,"secondaryTabId":13}, …]
  // "vertical" is side-by-side, "horizontal" stacked; "ratio" is the primary
  // (strip-order first) pane's share. Valid until the next call.
  virtual const char* SplitViewsJson(int64_t window_id) = 0;

  // Splits the two tabs named by STABLE ids ("" on an unknown id, the same
  // tab twice, or a tab already split). `layout` is "vertical" (default) or
  // "horizontal". Returns the new split's id.
  virtual const char* CreateSplitViewForTabs(int64_t window_id,
                                             int primary_tab_id,
                                             int secondary_tab_id,
                                             const char* layout) = 0;

  // Adjusts a split from SplitViewsJson: empty `layout` keeps the current
  // one, `ratio` outside [0,1] keeps the current one.
  virtual bool UpdateSplitView(int64_t window_id,
                               const char* split_id,
                               const char* layout,
                               double ratio) = 0;

  // Swaps the two panes of `split_id`.
  virtual bool SwapSplitView(int64_t window_id, const char* split_id) = 0;

  // Ends `split_id`; both tabs stay open as ordinary tabs.
  virtual bool RemoveSplitById(int64_t window_id, const char* split_id) = 0;

  // --- Tab groups (Chromium native model) ----------------------------------
  // Groups organize contiguous tabs under a colored, collapsible header; the
  // CLIENT draws the headers (there is no native strip in the hollowed
  // chrome). ListTabsJson rows carry "groupId" for members.

  // The window's groups as a JSON array string:
  // [{"id":"…","title":"…","color":"blue","collapsed":false},…]
  // "color" is the TabGroupColorId name (grey|blue|red|yellow|green|pink|
  // purple|cyan|orange). Pointer valid until the next call on this interface.
  virtual const char* TabGroupsJson(int64_t window_id) = 0;

  // Group operations by the id string from TabGroupsJson. Renaming and
  // recoloring keep the other visual fields. Collapsing a group that holds
  // the active tab first activates the nearest tab outside it — refused when
  // every tab is inside. Ungroup dissolves the header keeping the tabs;
  // CloseTabGroup closes every member.
  virtual bool RenameTabGroup(int64_t window_id,
                              const char* group_id,
                              const char* title) = 0;
  virtual bool SetTabGroupColor(int64_t window_id,
                                const char* group_id,
                                const char* color) = 0;
  virtual bool SetTabGroupCollapsed(int64_t window_id,
                                    const char* group_id,
                                    bool collapsed) = 0;
  virtual bool UngroupTabGroup(int64_t window_id, const char* group_id) = 0;
  virtual bool CloseTabGroup(int64_t window_id, const char* group_id) = 0;

  // Group operations by STABLE tab ids (ListTabsJson "id"), for the
  // phi-browser skill: ids resolve atomically inside the call, so they hold
  // even when indices shift under concurrent tab churn. `tab_ids_json` is a
  // JSON int array ("[12,13]"); unknown and grouping-refusing ids are
  // dropped.

  // Groups the named tabs into a fresh group and returns its id ("" when no
  // tab could be grouped). Valid until the next call on this interface.
  virtual const char* CreateTabGroupForTabs(int64_t window_id,
                                            const char* tab_ids_json) = 0;

  // Adds the named tabs to the existing group from TabGroupsJson.
  virtual bool AddTabsToGroup(int64_t window_id,
                              const char* group_id,
                              const char* tab_ids_json) = 0;

  // Takes each named tab out of whichever group holds it.
  virtual bool RemoveTabsFromGroupByIds(int64_t window_id,
                                        const char* tab_ids_json) = 0;

  // --- Navigation state & history ------------------------------------------
  // What a client toolbar needs: current url/title, whether back/forward are
  // available, and loading state — plus the actions.

  // The tab's cached page snapshot (Chromium's hover-card ThumbnailImage,
  // decoded from its JPEG) as premultiplied BGRA — null when nothing is
  // cached yet (the thumbnail records as tabs are seen/hidden). `tab_id`
  // is ListTabsJson "id". Buffer contract as TabFaviconBgraById. Feeds the
  // Ctrl+Tab switcher's and group overview's cards.
  virtual const unsigned char* TabThumbnailBgra(int64_t window_id,
                                                int tab_id,
                                                int* width,
                                                int* height) = 0;

  // The best HISTORY-backed favicon for `page_url` (fetched at up to 64px)
  // as raw premultiplied BGRA — null when nothing is cached yet, in which
  // case an async FaviconService fetch is scheduled and OnWindowChanged
  // (kFaviconReady) fires for `window_id` when it lands (the next read hits
  // the cache). Pinned cells and bookmark rows use this to beat the 16px
  // live-tab favicon. Buffer contract as TabFaviconBgraById.
  virtual const unsigned char* FaviconForUrlBgra(int64_t window_id,
                                                 const char* page_url,
                                                 int* width,
                                                 int* height) = 0;

  // Serializes the active tab's navigation state as a JSON object string:
  // {"url":"…","title":"…","canGoBack":false,"canGoForward":false,
  //  "loading":false}. "{}" on an unknown window. The pointer is valid until
  // the next call on this interface.
  virtual const char* NavStateJson(int64_t window_id) = 0;

  // --- Spaces and profiles --------------------------------------------------
  // Spaces are a CLIENT concept (as on Mac): the client owns the list and each
  // slot's active Space. The framework only creates the backing Browser
  // (RequestBrowser) and records the window->space binding, which the fork's
  // URL-routing machinery reads (PhiWindowSpaceRegistry).

  // Profiles known to the browser, as a JSON array string:
  // [{"id":"Default","name":"Person 1","loaded":true}, …]
  virtual const char* ListProfilesJson() = 0;

  // Creates a new on-disk profile; the client learns its id from a later
  // ListProfilesJson (creation is asynchronous).
  virtual void CreateProfile(const char* display_name) = 0;

  // Schedules `profile_id` (a basename) for deletion. Refused inside
  // phi::DeleteProfile while any live Browser is bound to it.
  virtual void DeleteProfile(const char* profile_id) = 0;

  // Renames `profile_id`'s display name — pure metadata, nothing reloads.
  virtual void RenameProfile(const char* profile_id,
                             const char* display_name) = 0;

  // The profile a browser window belongs to: {"id":"…","name":"…"}.
  virtual const char* WindowProfileJson(int64_t window_id) = 0;

  // Search engines of `profile_id` (a LOADED profile), as a JSON array:
  //   [{"keyword":"google.com","name":"Google","default":true}, …]
  // — the prepopulated set plus the current default, the same chooser the
  // Mac settings pane offers. Empty when the profile or its
  // TemplateURLService is not ready yet.
  virtual const char* ListSearchEnginesJson(const char* profile_id) = 0;
  // Makes `keyword`'s engine the profile's default. False when the profile
  // is not loaded, the keyword is unknown, or policy manages the default.
  virtual bool SetDefaultSearchEngine(const char* profile_id,
                                      const char* keyword) = 0;
  // The profile's download directory (UTF-8), resolved to the OS default
  // when the pref is unset; empty when the profile is not loaded.
  virtual const char* DownloadDirectoryUtf8(const char* profile_id) = 0;
  // Points the profile's downloads at `dir_utf8` (an absolute path).
  virtual bool SetDownloadDirectory(const char* profile_id,
                                    const char* dir_utf8) = 0;

  // --- Lazy Space restore -------------------------------------------------
  // Replays the previous session mid-run — what a cold start does, for a
  // reopen or an external link arriving with no window open. Settles on the
  // delegate's OnSessionRestoreSettled.
  //
  // When `armed` is false the whole session is rebuilt (today's behaviour) and
  // `eager_window_ids` is ignored. When it is true ONLY the named windows are
  // rebuilt and every other TYPE_NORMAL window of the previous session is
  // PARKED in the ghost store — written through to its profile's session file
  // — instead. An armed set of size 0 is valid and parks everything.
  virtual void RestorePreviousSession(const char* preferred_profile_basename,
                                      bool armed,
                                      const int* eager_window_ids,
                                      int count) = 0;

  // Peek View's two client preferences (the Mac's peekViewEnabled and
  // autoPeekViewEnabled). PUSHED, not queried: the "Open Link in Peek View"
  // context-menu item and the cross-domain throttle's automatic diversion
  // both decide on the UI thread, with no way back to the client. Push once
  // when the framework binds and again whenever either switch moves.
  //
  //   enabled      — the master gate: the menu item, and the diversion under
  //                  it.
  //   auto_enabled — the sub-gate: a cross-site link leaving a bookmark- or
  //                  pinned-bound tab opens as a Peek instead of a new tab.
  //
  // Both default ON until the first push, which is what this framework did
  // before the switches existed.
  virtual void SetPeekPreferences(bool enabled, bool auto_enabled) = 0;

  // The Kiosk's client preference (the Mac's openKioskOnCommandOptionClick):
  // whether a link clicked with Ctrl+Alt held opens in a Kiosk window. PUSHED
  // for the same reason as the Peek switches — Browser::OpenURLFromTab
  // decides on the UI thread. Defaults ON until the first push.
  virtual void SetKioskPreferences(bool open_on_ctrl_alt_click) = 0;

  // The "New tab behavior" preference (the Mac's openNewTabPageOnCmdT,
  // inverted): true when a new tab opens the omnibox instead of the new-tab
  // page. PUSHED for the same reason — Browser::TabStripEmpty decides on the
  // UI thread whether an emptied strip gets a refill tab or the placeholder,
  // and chrome::OpenEmptyWindow whether a new window is born with a blank tab
  // or EMPTY, in placeholder mode (both through
  // PhiBridgeWrapper::ShouldAutoCreateTabInEmptyWindow, answered on Windows
  // from this flag); a window that cannot hold a placeholder (off the
  // record) keeps its blank tab. The client's
  // placeholder handling (content hidden, the native new-tab page and its
  // field over it, the tab born on commit) is the omnibox over the empty
  // window. Defaults OFF until the first push.
  virtual void SetNewTabOpensOmnibox(bool enabled) = 0;

  // The split view's chrome for `window_id` (drawn by Chromium's
  // MultiContentsView here; the panes sit flush in the content card): the
  // ground that shows outside a pane's rounded corner, the divider's
  // hairline and the ground of its drag strip, the focused pane's 2 DIP
  // ring (inside the pane; the other pane has no edge), and the DIP between
  // the panes and the content area's top edge (the sides and bottom are 0).
  // Colours are 0xAARRGGBB; a translucent hairline blends over the strip's
  // ground. Pushed per window because each Space carries its own palette;
  // re-push on a theme, appearance or layout change. Until the first push
  // Chromium paints its own outline colours over the toolbar ground.
  virtual void SetSplitViewStyle(int64_t window_id,
                                 const SplitViewStyle& style) = 0;

  // --- Data-sharing consent -------------------------------------------------
  // The client's "share usage metrics and crash reports" switch, applied
  // through the same path as the chrome://settings toggle and the Mac
  // bridge's setMetricsReportingEnabled (metrics::ChangeMetricsReportingState
  // from kUiSettings): the Local State pref, the per-user usagestats registry
  // value, the client id, and the live start/stop of UMA and Crashpad uploads
  // all move together, so none can disagree with the switch. Policy wins
  // silently. Call it once the preference is known at startup and on every
  // change; a Windows client build reports nothing before the first call
  // (the pref defaults off there).
  virtual void SetMetricsReportingEnabled(bool enabled) = 0;

  // --- Showing a browser the shell held back -------------------------------
  // Runs Chromium's own Show (or ShowInactive) for a hosted browser again.
  // BrowserView::Show hands the reveal to the client (OnHostedWindowShow)
  // and shows its Widget only while the shell is PRESENTED; a window created
  // behind the onboarding gate therefore never turned its compositor on, and
  // the shell showing its own HWND later does not reach Chromium. Call it as
  // the held shell is shown, once per hosted browser: `activate` for the one
  // on screen (Show, with its first-show work), false for the others
  // (ShowInactive, which never switches the shell's Space).
  virtual void ShowBrowserWindow(int64_t window_id, bool activate) = 0;

  // What is parked right now, same shape as the receipt's `parked`. Lets a
  // client that restarted re-read the store instead of trusting its own
  // records.
  virtual const char* GhostWindowsJson() = 0;

  // Rebuilds a parked ghost as a live window. The three outcomes MUST stay
  // distinct to the caller: 0 materialized, 1 REFUSED FOR NOW (profile not
  // loaded, a replay still in flight, no session service — keep the record
  // and retry later), 2 NO SUCH GHOST (nothing will ever satisfy this, drop
  // the record or the Space pins forever). Never falls back to spawning a
  // fresh window: the session file may still describe the parked one, and a
  // fallback would stand beside it as a doubled Space.
  virtual int MaterializeGhostWindow(const char* profile_basename,
                                     int window_id) = 0;

  // Discards a parked ghost without rebuilding it — the session-side half of
  // closing a Space that exists only as a ghost, so it can neither be
  // materialized later nor replayed as a loose window by the next unfiltered
  // restore. False when the profile is not loaded or no such ghost is parked.
  virtual bool DropGhostWindow(const char* profile_basename,
                               int window_id) = 0;

  // The tabs whose operating mask the user has TAKEN BACK. While a tab is in
  // this set the browser REFUSES drive commands aimed at it from remote-
  // debugging clients, answering with a protocol error — so a driver cannot
  // decline to honour the takeover the way a cooperative signal would let it.
  // Replaces the whole set; pass an empty array to clear. The client is the
  // source of truth and re-pushes on every change.
  virtual void SetUserReclaimedTabs(const int* tab_ids, int count) = 0;

  // The window's profile's downloads, newest first (capped at 20), as a
  // JSON array — the DownloadsListView popover's data:
  //   [{"id":7,"name":"x.zip","url":"https://…","host":"…","path":"C:\\…",
  //     "state":"in_progress|complete|cancelled|interrupted","paused":false,
  //     "received":123,"total":456,"speed":789,"endTime":<unix seconds>,
  //     "exists":true}, …]
  // Valid until the next call on this interface.
  virtual const char* ListDownloadsJson(int64_t window_id) = 0;
  // Performs `action` on download `id` of the window's profile:
  // "open" | "show" (reveal in Explorer) | "pause" | "resume" |
  // "cancel" | "remove" (drop from history; cancels first when live).
  virtual void DownloadAction(int64_t window_id,
                              uint32_t id,
                              const char* action) = 0;

  // The window's profile's downloads for the phi-browser skill: newest
  // first, uncapped, guid identity, ms-epoch times (endTime 0 until done),
  // percentComplete -1 when the size is unknown:
  //   [{"guid":"…","url":"…","filename":"x.zip","mimeType":"…",
  //     "state":"in_progress|complete|cancelled|interrupted","paused":false,
  //     "done":false,"canResume":false,"totalBytes":456,"receivedBytes":123,
  //     "percentComplete":27,"currentSpeed":789,"startTime":0,"endTime":0,
  //     "targetPath":"C:\\…","currentPath":"C:\\…","dangerous":false,
  //     "insecure":false}, …]
  // Valid until the next call on this interface.
  virtual const char* DownloadsJsonForAgent(int64_t window_id) = 0;

  // The agent's download controls, by guid: "pause" | "resume" | "cancel" |
  // "remove" ONLY. "open"/"show" are deliberately refused — an agent may
  // observe and control downloads, never open files on the user's machine.
  virtual bool DownloadActionByGuid(int64_t window_id,
                                    const char* guid,
                                    const char* action) = 0;

  // Records which Space a browser window belongs to.
  virtual void SetWindowSpace(int64_t window_id, const char* space_id) = 0;

  // Publishes the client's Space list for the web-content context menu's
  // "Open Link in Space" submenu (PhiSpaceMenuRegistry — Chromium itself
  // knows nothing of Spaces). `json` is an array:
  //   [{"id":"…","name":"…","windowId":123}, …]
  // where windowId is the session id of the Space's open browser (0 when
  // none), used to exclude the current Space from the menu. Push it again
  // whenever Spaces change or their browsers come and go.
  virtual void SetSpaceMenu(const char* json) = 0;

  // Replaces the Space URL routing table (the fork's PhiURLRouter, read by
  // the space-routing navigation throttle) atomically. `json`:
  //   {"rules":[{"spaceId":"…","host":"github.com","pathPrefix":"/x",
  //              "ask":false,"sortOrder":0}, …],
  //    "spaceWindows":{"<spaceId>":<windowId>, …}}
  // `host` takes the Mac wire forms: "github.com" (exact), "*.foo.com"
  // (suffix wildcard) or "*needle*" (contains). "pathPrefix" and "ask" are
  // optional. `spaceWindows` maps each Space to its VISIBLE browser only —
  // a Space whose browser is hidden behind a sibling must be omitted so a
  // match resolves to OnRouteURLToSpace (the client's surface-and-open path)
  // instead of a direct open into an off-screen browser. Push on every rules
  // edit, browser attach/close and Space switch.
  virtual void SetSpaceRoutingTable(const char* json) = 0;

  // Opens `url` as a new foreground tab in `window_id`, exempting that one
  // navigation from Space URL routing (a one-shot (url, window) pass in the
  // router). Every client-side re-open of a routed URL goes through here or
  // the in-place variant below — a plain NewTab would be caught by the same
  // rule and loop.
  virtual void OpenTabBypassingSpaceRouting(int64_t window_id,
                                            const char* url) = 0;

  // Navigates `window_id`'s ACTIVE tab to `url` in place with the same
  // one-shot routing exemption — the keep-in-place answer to an "ask first"
  // prompt whose source was a fresh new tab. Falls back to a bypassed new
  // tab when the window has no active tab.
  virtual void NavigateActiveTabBypassingSpaceRouting(int64_t window_id,
                                                      const char* url) = 0;

  virtual bool GoBack(int64_t window_id) = 0;
  virtual bool GoForward(int64_t window_id) = 0;
  virtual bool Reload(int64_t window_id, bool bypass_cache) = 0;
  virtual bool StopLoading(int64_t window_id) = 0;

  // --- Credential autofill (the skill's fillCredential) ---------------------
  // The last committed URL of the tab identified by the DevTools `target_id`
  // (empty when the target is unknown). The client's credentials.autofill
  // reads this to enforce the origin binding — the page host must match the
  // credential's site — BEFORE the secret is ever touched.
  virtual const char* TargetUrlForCdpTarget(const char* target_id) = 0;

  // The STABLE tab id (ListTabsJson "id") of the tab behind the DevTools
  // `target_id`, or -1 when the target names no live tab — the lookup behind
  // the skill's PhiAgentSpace.resolveTabIds.
  virtual int TabIdForCdpTarget(const char* target_id) = 0;

  // Sets the value of the page field the skill tagged with
  // `[data-phi-autofill="<token>"]` in the tab identified by `target_id`,
  // then dispatches input/change and clears the marker. `field` is
  // "password" | "username" (the JS verifies the input's type matches).
  // `value` is the secret, passed IN-PROCESS (client vault -> framework) and
  // written straight into the page's DOM via an isolated-world script — it
  // never crosses to the agent. Returns true when the tab resolved and the
  // fill script was dispatched; false when the target is unknown.
  virtual bool FillTaggedField(const char* target_id,
                               const char* token,
                               const char* field,
                               const char* value) = 0;

  // --- Chromium command routing --------------------------------------------
  // The generic wire for features Chromium already implements but the
  // hollowed chrome has no UI for: print, zoom, fullscreen, view source,
  // DevTools, duplicate/mute tab, restore tab, task manager… `command_id` is
  // a chrome/app/chrome_command_ids.h IDC value; the command runs against the
  // window's active tab, exactly as a Chromium menu click would (the Mac's
  // commandDispatch: path). False when the window is unknown or the command
  // is unsupported/disabled there.
  virtual bool ExecuteChromiumCommand(int64_t window_id, int command_id) = 0;

  // Whether `command_id` is supported AND currently enabled for the window —
  // the client's menu validation (the Mac's canExecuteChromiumCommand twin).
  virtual bool IsChromiumCommandEnabled(int64_t window_id,
                                        int command_id) = 0;

  // --- Startup session restore ---------------------------------------------
  // Sets the profile's startup pref: LAST (restore the previous session) or
  // DEFAULT (a fresh new tab). No-op when the profile is not loaded.
  // Also flips the APP-WIDE cold-start switch (Local State), which is what
  // Phi's startup actually reads; the per-profile pref is kept in step.
  virtual void SetRestoreOnStartup(const char* profile_id, bool restore) = 0;

  // --- Omnibox autocomplete ------------------------------------------------
  // Chromium's full autocomplete stack (url/search/bookmark/history/
  // open-tab suggestions) behind the client's address field — the Mac
  // requestAutoCompleteSuggestions path over the fork's OmniboxProxy.
  // Results stream to OnOmniboxResults (coalesced) until the query stops.
  // `prevent_inline` asks for no inline autocompletion (the Mac's
  // preventInlineAutoComplete: the user just deleted text).
  virtual void StartOmniboxQuery(int64_t window_id,
                                 const char* text,
                                 bool prevent_inline = false) = 0;
  virtual void StopOmniboxQuery(int64_t window_id) = 0;

  // Opens the match at `line` of the window's LAST result set with
  // Chromium's own semantics (URL fixup, shortcuts learning), in the tab
  // `disposition` names (the Mac's PhiOmniboxSuggestionDisposition):
  // kCurrentTab navigates the active tab even when the match has an open
  // tab, kSwitchToTab activates that tab, kNewForegroundTab is the omnibox
  // new-tab mode. False when the window has no live result set.
  enum class OmniboxDisposition : uint8_t {
    kCurrentTab,
    kNewForegroundTab,
    kSwitchToTab,
  };
  virtual bool OpenOmniboxMatch(int64_t window_id,
                                int line,
                                OmniboxDisposition disposition) = 0;
  // Removes the match at `line` from its provider's store (history, a
  // search suggestion) when Chromium marks it deletable; the running query
  // reports the shortened set through OnOmniboxResults.
  virtual void DeleteOmniboxMatch(int64_t window_id, int line) = 0;

  // The search URL `keyword`'s engine builds for `query` — the Mac's
  // keywordSearchURLForKeyword path over the fork's OmniboxProxy, what the
  // address field's engine chip commits on Enter. Empty when the window's
  // profile has no such engine, the engine cannot take a query (POST,
  // starter pack, inactive), or `query` is blank. Valid until the next call
  // on this interface.
  virtual const char* KeywordSearchUrl(int64_t window_id,
                                       const char* keyword,
                                       const char* query) = 0;
  // Chromium's "Space enters keyword search" setting for the window's
  // profile (the Mac's isSearchEngineSpaceShortcutEnabled): true means Tab
  // or Space picks the highlighted engine row, false means Tab only.
  virtual bool KeywordSpaceShortcutEnabled(int64_t window_id) = 0;

  // --- Browser data import -------------------------------------------------
  // Imports `data_types` — comma-separated, of "favorites", "history",
  // "cookies", "passwords", "extensions" — from the profile directory
  // `profile` ("" = "Default") of the installed browser `source` ("chrome",
  // "edge", "brave", "arc") into `window_id`'s profile through Chromium's
  // importer (the fork's ImporterProxy; the Mac's importBrowserData). One
  // import runs at a time per profile. Asynchronous: OnImportCompleted
  // reports every accepted request, a refused one included. False when the
  // request could not be accepted at all (no such window, unknown source);
  // no completion follows then.
  virtual bool StartBrowserImport(int64_t window_id,
                                  const char* source,
                                  const char* profile,
                                  const char* data_types) = 0;
  // Imports the file at the UTF-8 `path` — a bookmarks HTML file, a Safari
  // History JSON or a Safari export archive; Chromium sniffs which — the
  // same way (the Mac's importData(fromFilePath:)). Its completion's source
  // is "file".
  virtual bool StartFileImport(int64_t window_id, const char* path) = 0;
  // Empties the window profile's CHROMIUM bookmark model, where an import's
  // bookmarks are staged before the client copies them into its own store
  // (the Mac's removeAllBookmarks ahead of an import).
  virtual void RemoveAllBookmarks(int64_t window_id) = 0;

  // The window profile's CHROMIUM bookmark model as a JSON tree — after an
  // import, the client copies these into its own store (the Mac's
  // saveChromiumBookmarksToLocalStore step):
  //   [{"title":"…","url":"…","folder":false,"children":[…]}, …]
  // Valid until the next call on this interface.
  virtual const char* AllBookmarksJson(int64_t window_id) = 0;

  // --- Tab search (the Mac SearchTabs palette) -----------------------------
  // Open tabs across the PROFILE's windows plus recently-closed entries —
  // the fork's SearchTabsProxy (Chromium Tab Search's data without WebUI):
  //   {"openTabs":[{"tabId":12,"windowId":34,"index":0,"title":"…",
  //     "url":"…","active":false,"hostWindow":true,
  //     "lastActiveText":"3 minutes ago"}, …],
  //    "recentlyClosed":[{"sessionId":56,"title":"…","url":"…",
  //     "lastActiveText":"…"}, …]}
  // Valid until the next call on this interface.
  virtual const char* SearchTabsDataJson(int64_t window_id) = 0;

  // Activates the tab ANYWHERE in the profile (cross-window focus; the
  // owning browser window is raised).
  virtual bool ActivateSearchTab(int64_t tab_id) = 0;

  // Restores the recently-closed entry into `window_id`.
  virtual bool OpenRecentlyClosedEntry(int64_t window_id,
                                       int64_t session_id) = 0;

  // Enabled extensions (the fork's ExtensionsProxy). The underlying list
  // loads ASYNCHRONOUSLY (icon files come off a thread pool): the first
  // call kicks a refresh and may return the EMPTY list; the delegate's
  // OnExtensionsChanged fires when the cache fills or the set changes, and
  // callers re-read then. Rows:
  //   [{"id":"…","name":"…","version":"…","pinned":false,
  //     "pinnedIndex":-1,"forcePinned":false,
  //     "badgeText":"","badgeBackground":"#AARRGGBB",
  //     "badgeTextColor":"#AARRGGBB","visible":true,"enabled":true,
  //     "grayscale":false}, …]
  // The last six are the action's state on `window_id`'s ACTIVE tab (the
  // Mac's badgeInfoChanged, Chrome's ExtensionActionViewModel semantics):
  // `visible` is false only for a page action hidden on this tab — the
  // client drops it from layout; `enabled` false means a click must fall
  // back to ShowExtensionContextMenu instead of running the action;
  // `grayscale` asks for the desaturated icon. OnExtensionsChanged also
  // fires when any of them changes. Valid until the next call on this
  // interface.
  virtual const char* ListExtensionsJson(int64_t window_id) = 0;

  // The extension's manifest/action icon as premultiplied BGRA (the favicon
  // buffer contract: valid until the next call on this interface). Null when
  // unknown or the icon has not decoded yet.
  virtual const unsigned char* ExtensionIconBgra(const char* extension_id,
                                                 int* width,
                                                 int* height) = 0;

  // Runs the extension's toolbar action in `window_id`. `anchor` is the
  // SCREEN rect of the clicked button — a popup-bearing action shows
  // Chromium's ExtensionPopup anchored to it.
  virtual void TriggerExtensionAction(int64_t window_id,
                                      const char* extension_id,
                                      int anchor_x,
                                      int anchor_y,
                                      int anchor_width,
                                      int anchor_height) = 0;

  // Opens the action's context menu (pin / unpin, options, manage, remove
  // — Chrome's ExtensionContextMenuModel) with its top-left at SCREEN
  // point (anchor_x, anchor_y); the Mac hands the clicked icon's
  // bottom-left corner (triggerExtensionContextMenu). A right-click on an
  // action icon, and what a left-click on a DISABLED action falls back to
  // (Chrome's ExecuteUserAction). Any dialog the menu opens (the uninstall
  // confirmation) anchors at the same point.
  // Optional native_owner is the retained client panel's HWND. For a nonzero
  // request_id and callback, completion is posted exactly once on the UI
  // thread, including rejection, replacement, cancellation and owner closure.
  // The callback must remain callable until completion; it receives only the
  // caller's token, never a borrowed client object. Legacy callers omit these.
  virtual void ShowExtensionContextMenu(int64_t window_id,
                                        const char* extension_id,
                                        int anchor_x,
                                        int anchor_y,
                                        void* native_owner = nullptr,
                                        uint64_t request_id = 0,
                                        void (*closed)(uint64_t) = nullptr) = 0;
  // Cancels only the matching request; stale tokens cannot close a newer menu.
  // Completion remains asynchronous. Call before destroying native_owner.
  virtual void CancelExtensionContextMenu(int64_t window_id,
                                          uint64_t request_id) = 0;

  // The client's own chrome (sidebar, caption band, address bar) took a
  // click in `window_id`. Chromium's action popup closes when any widget
  // of the browser window activates — a toolbar click upstream, any click
  // in the Mac's window — but the shell's chrome is no views widget, so the
  // client reports the click and the open popup closes here (unless
  // DevTools inspects it or a JavaScript dialog holds it, upstream's
  // guards). A no-op with no popup open.
  virtual void CloseExtensionPopup(int64_t window_id) = 0;

  // Pins/unpins the action on the toolbar model (the Mac popover's pin
  // toggles); `window_id` scopes to the profile. State comes back through
  // ListExtensionsJson after the change notification.
  virtual void SetExtensionPinned(int64_t window_id,
                                  const char* extension_id,
                                  bool pinned) = 0;

  // --- JavaScript dialogs (the PhiAgentSpace CDP domain's Windows twin) ---
  // The Mac fork answers PhiAgentSpace.getJavaScriptDialogState /
  // handleJavaScriptDialog inside its DevTools server; the Windows relay is
  // a byte pipe, so the client answers them itself through these.
  // The blocking JavaScript dialog (alert / confirm / prompt / beforeunload)
  // of the tab behind CDP `target_id`, as JSON:
  //   {"found":true,"open":true,"type":"confirm","message":"…"}
  //   {"found":true,"open":false}
  //   {"found":false}                    — no target with that id
  // A tab-modal dialog reports no message (Chromium keeps it renderer-side).
  // Valid until the next call on this interface.
  virtual const char* JavaScriptDialogStateJson(const char* target_id) = 0;
  // Accepts (or dismisses) that dialog; `prompt_text` (nullable) overrides a
  // prompt's answer. Returns false when the target is unknown or has no
  // dialog manager; `*handled` reports whether a dialog was actually closed.
  // Only a dialog the state call reports as open is touched — the managers
  // would otherwise act on pending, not-yet-shown dialogs.
  virtual bool HandleJavaScriptDialog(const char* target_id,
                                      bool accept,
                                      const char* prompt_text,
                                      bool* handled) = 0;

  // --- Reader extraction (the skill's readerArticle / saveArticle) --------
  // Distills the tab behind CDP `target_id` — or, with an empty target,
  // `window_id`'s active tab — through Chromium's DOM Distiller, the same
  // pipeline "Open in Reading Mode" renders, WITHOUT navigating the tab.
  // Asynchronous: the answer arrives as OnReaderArticleExtracted(request_id,
  // …). Returns false when no such tab exists (nothing will be delivered).
  virtual bool StartReaderExtraction(int64_t window_id,
                                     const char* target_id,
                                     int request_id) = 0;

  // --- Display language ---------------------------------------------------
  // Applies the client's chosen display language to CHROMIUM's own UI: the
  // strings the framework draws rather than the client — web-content context
  // menus, error pages, permission bubbles, print preview, the PDF viewer.
  // The web content's own languages (Accept-Language, navigator.language)
  // are a separate profile preference, exactly as they are in Chrome.
  //
  // `locale` is a client catalog tag ("ja", "zh-Hans"), or EMPTY to follow
  // the operating system. Chromium's own locale is a startup decision read
  // from the intl.app_locale local-state pref (chrome_resource_bundle_helper
  // .cc), so this both WRITES that pref — which is what survives a restart —
  // and reloads the locale pack so the running process switches too.
  //
  // Returns the locale actually applied ("zh-CN" for "zh-Hans"; the OS's own
  // language for an empty tag), empty when nothing could be resolved. Valid
  // until the next call on this interface. The reload is asynchronous: UI
  // already built keeps the strings it was built with, which for the
  // framework's part means everything is created on demand and comes up in
  // the new language.
  virtual const char* SetDisplayLanguage(const char* locale) = 0;

  // --- Default browser ----------------------------------------------------
  // Registers this browser with Windows and opens the system "Default apps"
  // page at its entry.
  //
  // The Mac's twin is one call — NSWorkspace.setDefaultApplication — because
  // macOS lets an app claim the http handler with a consent sheet. Windows
  // 10 and later forbid setting the default programmatically at all: the
  // choice is only ever made by the user in Settings. What the browser CAN
  // do is register itself, and until it does Windows does not offer it there
  // — which is why opening that page alone (what the client did before) is a
  // dead end. Chromium's own "Make default" button runs exactly this.
  //
  // Asynchronous and best-effort: registration happens on a worker thread
  // and the Settings app is the user's to finish. Ask IsDefaultBrowser again
  // afterwards rather than assuming.
  virtual void RequestDefaultBrowser() = 0;

  // --- Extension messaging -------------------------------------------------
  // The other half of PhiFrameworkDelegate::OnExtensionMessage. Exactly one of
  // these settles a request; a second call for the same id is dropped with a
  // warning. `response_json` is handed to the extension verbatim as the
  // resolved value of its sendMessageToApp promise (send "{}" when there is
  // nothing to say); `error` rejects it.
  virtual void RespondToExtensionMessage(const char* request_id,
                                         const char* response_json) = 0;
  virtual void FailExtensionMessage(const char* request_id,
                                    const char* error) = 0;

  // App -> extension: fires phinomenonPrivate.onAppMessage in every extension
  // that listens. The Mac's ExtensionMessaging.broadcast. A payload carrying a
  // top-level "windowId" is delivered to the profile owning THAT window; one
  // without goes to the active window's profile. False when the payload is not
  // a JSON object, is too large, or no profile could be resolved to deliver
  // it — a broadcast nobody received.
  virtual bool BroadcastToExtensions(const char* type,
                                     const char* payload_json) = 0;

  // --- Reader View surface --------------------------------------------------
  // Moves the reader extension's surface tab out of `window_id`'s strip and
  // into a browser of its own, so the client can host it in a panel over the
  // origin tab's page while that page stays live underneath. Both ids are
  // session tab ids (what ListTabsJson emits as "id").
  //
  // The surface tab is MOVED, not re-opened: the extraction it is already
  // showing is not repeatable for free, and the extension's bookkeeping keys
  // on the tab id rather than its window, so it survives the move.
  //
  // Returns the panel browser's window id — which arrives at
  // OnBrowserWindowCreated first, announced as WindowKind::kReaderPanel with
  // opener_window/opener_tab naming the origin (re-entrantly, inside this
  // call) — or 0 when there is no such tab, the move would empty the
  // window, or the browser could not be created.
  virtual int64_t MoveTabToReaderPanel(int64_t window_id,
                                       int surface_tab_id,
                                       int origin_tab_id) = 0;

  // --- Feedback -------------------------------------------------------------
  // Collects Chromium's scrubbed system logs for a feedback report — the
  // profile behind `window_id`, else the last active browser's, else the
  // last-used profile (the Mac's ResolveFeedbackSystemLogsProfile ladder).
  // Asynchronous: OnFeedbackSystemLogs(request_id, text) answers on the UI
  // thread. False when no profile is loaded, in which case nothing answers.
  virtual bool FetchFeedbackSystemLogs(int64_t window_id, int request_id) = 0;

  // --- Page surface capture (the comfortable Space slide's entering page) --
  // Copies what `tab_id`'s renderer composited last — the page at the
  // view's size, in physical pixels, BGRA — the way the Mac's slide
  // snapshots the entering window's content. The tab need not be on
  // screen: a browser window shown but clipped by its parent still
  // composites. Asynchronous: OnTabSurfaceCaptured(request_id, …) delivers
  // the pixels, or a 0 x 0 result when there is nothing to copy yet (a view
  // just shown has no frame for a beat; ask again). Returns false when no
  // such tab exists — nothing will be delivered then.
  virtual bool StartTabSurfaceCapture(int64_t window_id,
                                      int tab_id,
                                      int request_id) = 0;

  // --- Id-addressed tab operations (phibrowser-win docs/sidebar-architecture
  // §7). Strip indices shift during the very notification the client reacts
  // to; these take the STABLE session id ("id" in ListTabsJson) and resolve
  // the index inside the call. Each is false for an unknown window or tab.
  virtual bool ActivateTabById(int64_t window_id, int tab_id) = 0;
  virtual bool CloseTabById(int64_t window_id, int tab_id) = 0;
  // Puts closed pages back as unloaded background tabs in one batch, with
  // their groups and splits, moving no focus (TabsProxy::RestoreTabs, the
  // Mac's restoreTabs; Clear All Tabs' Undo). Three JSON arrays:
  //   order: the strip order wanted, {"tabId":n} for a live tab that anchors
  //     it or {"key":"k","url":"u","title":"t"} for a page to restore;
  //   groups: {"keys":["k"],"id":"<TabGroupsJson id>"} rejoins a live group,
  //     {"keys":["k"],"title":"t","color":"blue","collapsed":b} makes one;
  //   splits: {"primaryKey":"k","secondaryKey":"k","layout":"vertical",
  //     "ratio":0.5}.
  // Returns {"k":newTabId,...}: "{}" for an unknown window, under an agent's
  // drive, or when nothing could be restored.
  virtual const char* RestoreTabs(int64_t window_id,
                                  const char* order_json,
                                  const char* groups_json,
                                  const char* splits_json) = 0;
  // A client-owned string riding the tab (PhiTabData::custom_value), which
  // session restore round-trips — the Mac's customGuid. TabSnapshot::tag /
  // ListTabsJson "tag" expose it; the client stamps pin/bookmark guids on
  // bound tabs so bindings survive a restart. Empty clears. Tagged tabs are
  // excluded from tab groups (the fork's Phi-managed rule).
  virtual bool SetTabTagById(int64_t window_id, int tab_id, const char* tag) = 0;
  // The tab's favicon as raw BGRA pixels (premultiplied, top-down), the form
  // GDI's DIBs take — an encoded PNG would need WIC or GDI+ on the client just
  // to get back to pixels. Returns null when the tab has no favicon yet;
  // `*width`/`*height` receive its size. The buffer is valid until the next
  // call on this interface (single-threaded UI use), so copy what you keep.
  virtual const unsigned char* TabFaviconBgraById(int64_t window_id,
                                                  int tab_id,
                                                  int* width,
                                                  int* height) = 0;
  // The SCREEN rect, in physical pixels, that `tab_id`'s content draws in:
  // the whole content area for a lone tab, its own pane when it is in the
  // split on screen. False when the tab is not on screen at all (inactive
  // and not a pane of the active split). Chromium lays the panes out, so
  // the rect can change without any strip change — OnContentBoundsChanged
  // says when to read it again.
  virtual bool TabContentBoundsById(int64_t window_id,
                                    int tab_id,
                                    int* left,
                                    int* top,
                                    int* right,
                                    int* bottom) = 0;
  // Anchored moves: the client's sidebar order is not the strip's index space
  // (bound tabs are hidden), so a move is "before/after tab X" and Chromium
  // resolves the index. The moving unit is `tab_id`'s whole split when it is
  // a pane (the pair never separates); the anchor's unit likewise. `anchor_id`
  // 0 = the end of the strip. `group_id`: nullptr = let Chromium assign the
  // destination's group; "" = ungrouped; else that group's id. A Phi-managed
  // (tagged) tab never joins a group regardless. False on unknown ids; a
  // no-op (anchor inside the moving unit) is true.
  virtual bool MoveTabBefore(int64_t window_id,
                             int tab_id,
                             int anchor_id,
                             const char* group_id) = 0;
  virtual bool MoveTabAfter(int64_t window_id,
                            int tab_id,
                            int anchor_id,
                            const char* group_id) = 0;

  // --- Window snapshot (phase 2) ------------------------------------------
  // The window's current state — the same shape OnWindowChanged pushes.
  // Framework-owned scratch, valid until the next PhiFramework call (rule
  // 5): copy it (the copy allocates on the caller's side, rule 6). nullptr
  // for an unknown window.
  virtual const WindowSnapshotView* Snapshot(WindowId window) = 0;

  // --- The remaining id-addressed operations. Each is false for an unknown
  // window or tab.
  // Opens a foreground tab at the end of the strip, optionally navigating
  // it, and answers with the STABLE id (TabId() on failure).
  virtual TabId OpenTab(WindowId window, const char* url) = 0;
  // Moves the tab — WebContents intact — into `destination`'s strip (the
  // real move behind "Move to Space" when the target Space has an open
  // browser). False on unknown windows or when it would empty the source.
  virtual bool MoveTabToWindowById(WindowId source,
                                   TabId tab,
                                   WindowId destination) = 0;
  // Detaches the given tabs (one or two; a split's pair moves together —
  // Chromium detaches the split collection whole) into a NEW window in the
  // same profile — the drag-out gesture. The new window is announced
  // normally (kind kUser, token 0). False when they cover the whole strip.
  virtual bool MoveTabsToNewWindowByIds(WindowId window,
                                        const TabId* tabs,
                                        int count) = 0;
  // Dissolves the split containing the tab; both tabs stay open as ordinary
  // tabs. False when the tab is not part of a split.
  virtual bool UnsplitById(WindowId window, TabId tab) = 0;
  // Puts the tab into a fresh group. False when it is already grouped or
  // refuses grouping (Phi-managed).
  virtual bool AddTabToNewGroupById(WindowId window, TabId tab) = 0;
  // Takes the tab out of its group. False when it has none.
  virtual bool RemoveTabFromGroupById(WindowId window, TabId tab) = 0;
  // Puts the tab into ONE pane of `split_id`, evicting whatever was there —
  // the Mac's swapTabInSplit (TabsProxy::UpdateSplitTab). `slot` names the
  // pane: 0 the primary (leading / top), 1 the secondary. `keep_replaced`
  // stages the incoming tab beside the split first, so the evicted pane
  // lands right next to it and inherits its tab group (Chromium's kSwap);
  // false closes the evicted pane instead (kReplace). False when the split,
  // the slot or the tab is unusable — including a tab that is itself a pane
  // of ANY split, which Chromium cannot unwind.
  virtual bool SwapTabIntoSplitById(WindowId window,
                                    const char* split_id,
                                    int slot,
                                    TabId tab,
                                    bool keep_replaced) = 0;

  // --- Agent drive context (phase 4, docs/architecture-review.md §3.7) ---
  // While `session_id` is non-empty, every tab or navigation mutation the
  // client makes on a USER window (not an agent Space's browser, not a
  // shadow) is reported as agent-driven — OnAgentDroveUserSpaceTab(tab,
  // window, session) exactly as a CDP drive would be — so the operating
  // mask arms, and a tab the user reclaimed (SetUserReclaimedTabs) REFUSES:
  // the call returns false / does nothing, the same refusal the CDP path
  // answers with a protocol error. "" ends the context. UI thread; not
  // nested (setting a new tag replaces the old one). The client sets it
  // around a control verb that acts on a user window on an agent's behalf,
  // and never around its own UI gestures.
  //
  // What is gated, by the tab it targets: Navigate, GoBack, GoForward,
  // Reload, StopLoading, NavigateActiveTabBypassingSpaceRouting and
  // ExecuteChromiumCommand (the active tab); ActivateTabById, CloseTabById,
  // MoveTabBefore/After, MoveTabToWindowById, MoveTabsToNewWindowByIds,
  // AddTabToNewGroupById, RemoveTabFromGroupById (the named tab);
  // CreateSplitViewForTabs, UpdateSplitView, SwapSplitView, RemoveSplitById,
  // SwapTabIntoSplitById, UnsplitById (the split's panes and the named
  // tab); RenameTabGroup, SetTabGroupColor, SetTabGroupCollapsed,
  // UngroupTabGroup, CloseTabGroup, CreateTabGroupForTabs, AddTabsToGroup,
  // RemoveTabsFromGroupByIds (the group's members / the named tabs);
  // CloseBrowserWindow (every tab). A multi-tab mutation refuses when ANY
  // of its tabs is reclaimed and reports on the first. NewTab, OpenTab and
  // OpenTabBypassingSpaceRouting cannot be refused (a fresh tab is nobody's)
  // and report on the tab they made. TabFaviconBgraById and TabThumbnailBgra
  // raise OnAgentObservedUserSpaceTab instead (a read keeps a mask alive,
  // never raises one); Snapshot and the JSON getters raise nothing — they
  // are window reads, the twin of Target.getTargets on the browser target,
  // which the CDP path does not report either.
  //
  // The report is delivered SYNCHRONOUSLY, inside the mutating call, so the
  // client can pair the int64 session key it receives with the tag it set
  // (the key is minted per distinct tag from the same counter as CDP
  // sessions' and never collides with one). Coalesced as the CDP path is:
  // one drive per (tag, tab) per second, one observe per tag per second.
  // No OnAgentStoppedUserSpaceTab is ever raised for these holds — a control
  // verb has no session to end; the client's idle timeout retires the mask.
  virtual void SetAgentDriveContext(const char* session_id) = 0;

  // --- DevTools session scope ----------------------------------------------
  // Restricts what a DevTools session (OpenDevToolsSession) can reach:
  // Target.getTargets / Target.attachToTarget / Target.setAutoAttach and
  // the browser-level Target events only see, attach to, or auto-attach
  // targets whose window is one of `windows` (a page target's Browser's
  // session id, the value Browser.getWindowForTarget reports); a request
  // for any other target answers as if it did not exist. Browser-wide
  // methods that read across contexts (Storage.getCookies without a
  // browserContextId, Browser.grantPermissions on user contexts) are
  // refused. `count` < 0 removes the restriction (the test rig); `count`
  // == 0 leaves a session that can see nothing but the browser target
  // itself, which is the state before the first grant — EVERY session
  // starts there, so a client that never calls this gets a session that
  // can list, attach to and hear about no page at all. Any thread; takes
  // effect for the next message. Applying it to an unknown session is a
  // no-op. The exact method-by-method filter table is the SESSION SCOPE
  // block in phi_devtools_transport.h.
  virtual void SetDevToolsSessionScope(uint64_t session_id,
                                       const int64_t* windows,
                                       int count) = 0;

  // --- In-process DevTools transport. Contract, threading and the discovery
  // document shapes are documented above the PhiDevToolsClient class (after
  // this class) — ANY thread for these four, callbacks on the UI thread.
  virtual uint64_t OpenDevToolsSession(const char* target_id,
                                       PhiDevToolsClient* client) = 0;
  virtual void SendDevToolsMessage(uint64_t session_id,
                                   const char* json,
                                   size_t length) = 0;
  virtual void CloseDevToolsSession(uint64_t session_id) = 0;
  virtual bool DevToolsDiscoveryJson(const char* path, PhiStringSink* out) = 0;

  // --- URL-rule destinations that are not Spaces ----------------------------
  // The two action targets the Mac's URL rules and its "ask first" chooser
  // offer beside the user's Spaces (phibrowser-win's PhiUrlRouter). Neither
  // needs a routing exemption: a Kiosk browser and an off-the-record browser
  // are both skipped by the space-routing throttle outright.
  //
  // Opens `url` in a NEW ephemeral Kiosk window in `source_window_id`'s
  // profile (the fork's phi::OpenUrlInKiosk — announced as WindowKind::kKiosk
  // like a Kiosk rule's own open). False when the source window is unknown,
  // the URL is invalid or the Kiosk could not be created — the caller then
  // keeps the URL where it is.
  virtual bool OpenUrlInKiosk(int64_t source_window_id, const char* url) = 0;
  // Opens `url` in an off-the-record window of `source_window_id`'s profile:
  // that profile's existing Incognito browser when one is open, else a new
  // one (chrome::OpenURLOffTheRecord, announced as an incognito kUser
  // window). False when the source window is unknown or the URL is invalid.
  virtual bool OpenUrlOffTheRecord(int64_t source_window_id,
                                   const char* url) = 0;

  // --- Phi sync: what the client tells Chromium's sync service --------------
  // The Mac bridge's notifyPhiAuthStateChanged / notifyPhiSyncKeysChanged /
  // notifyPhiSyncInvalidationForAccount:… . Each is a nudge: sync re-reads
  // through the delegate queries above. UI thread.
  //
  // The session moved (sign-in, sign-out, a renewed token, reauthentication).
  virtual void NotifyPhiAuthStateChanged() = 0;
  // A profile's key resolved, changed or was withdrawn.
  virtual void NotifyPhiSyncKeysChanged() = 0;
  // An invalidation from the client's notification stream for namespace
  // chromium:<profile_uuid>: the data types (specifics field numbers) that
  // changed, and the device that caused it (its own change is ignored).
  // An empty uuid and no types ask every profile for a catch-up pull.
  virtual void NotifyPhiSyncInvalidation(const char* account_id,
                                         const char* profile_uuid,
                                         const int* data_type_ids,
                                         int data_type_count,
                                         const char* excluding_client_id) = 0;
  // The Mac bridge's getProfileSyncStatus: Chromium sync's status for the
  // LOADED profile `profile_id` (its on-disk basename), as JSON —
  //   {"version":1,"phase":"upToDate","enabled_categories":["bookmarks"],
  //    "last_success_ms":1758700000000}
  // phase ∈ checking | initialSync | syncing | upToDate | offline |
  // needsAttention. nullptr when the profile is not loaded (never loads it).
  // Buffer valid until the next call.
  virtual const char* ProfileSyncStatusJson(const char* profile_id) = 0;

  // --- The address bar's site menu (the Mac WebContentAddressBarMenu) ------
  // What the menu shows for `window_id`'s active tab, as JSON:
  //   {"url":"https://…","security":"secure","hasCertificate":true,
  //    "contentBlocking":{"domain":"example.com","blocking":true}}
  // security ∈ secure | partial | insecure | unknown | "" ("" on Chromium's
  // and Phi's own pages: no security row); contentBlocking only when its row
  // applies (a regular profile with some blocking on, an http(s) page).
  // "{}" on an unknown window. Buffer valid until the next call.
  virtual const char* SiteMenuJson(int64_t window_id) = 0;
  // Site Settings ▸ Clear Cache / Clear Cookie for the active tab's site
  // (cache by origin, cookies by registrable domain). False when there is no
  // active tab or its page has no host.
  virtual bool ClearActiveSiteData(int64_t window_id, bool cookies) = 0;
  // "Content blocking on this site": blocking on (true) or excepted (false)
  // for the active tab's registrable domain. The page picks the rule set up
  // on its next load. False when the row does not apply.
  virtual bool SetActiveSiteContentBlocking(int64_t window_id,
                                            bool blocking) = 0;
  // The security row: the native certificate viewer on the active tab's
  // certificate. False without one.
  virtual bool ShowActiveSiteCertificate(int64_t window_id) = 0;
};

// --- In-process DevTools transport (docs/architecture-review.md §3.7) ----
// The client's control socket relays CDP to the browser THROUGH these calls.
// There is no loopback TCP DevTools server any more (the client no longer
// passes --remote-debugging-port), so the client's consented pipe is the only
// door to the protocol. The framework attaches a DevToolsAgentHostClient per
// session; the client owns the sessions and pumps frames between them and
// its pipe connections.
//
// THREADING: unlike the rest of this interface, the three session calls and
// DevToolsDiscoveryJson may be called from ANY thread (the client's pipe I/O
// threads); the framework marshals to the UI thread itself. Both
// PhiDevToolsClient callbacks are delivered ON THE UI THREAD, and `json` is
// valid only for the duration of the callback — copy it. The client must
// CloseDevToolsSession (and let it return) before destroying the client
// object; after Close no callback fires for that session.
class PhiDevToolsClient {
 public:
  virtual ~PhiDevToolsClient() = default;
  // A protocol message (a response or an event) from the browser.
  virtual void OnDevToolsMessage(uint64_t session_id,
                                 const char* json,
                                 size_t length) = 0;
  // The agent host detached (target closed, browser shutting down, or the
  // target named in Open did not exist). Terminal for the session; the
  // client still calls CloseDevToolsSession to release it.
  virtual void OnDevToolsSessionClosed(uint64_t session_id) = 0;
};

// Opens a session. `target_id` "" or nullptr = the browser-wide target (what
// /devtools/browser/<id> served over TCP: Target.*, Browser.*, and
// flattened auto-attach); otherwise a DevToolsAgentHost id as listed by
// DevToolsDiscoveryJson("/json/list") — the same ids TabIdForCdpTarget and
// FillTaggedField take. Returns a non-zero session id immediately; the attach
// happens on the UI thread, and an unknown target is reported through
// OnDevToolsSessionClosed. Returns 0 only when the framework is shutting
// down.
//   virtual uint64_t OpenDevToolsSession(const char* target_id,
//                                        PhiDevToolsClient* client) = 0;
// Sends one protocol message (UTF-8 JSON) into the session. Messages are
// delivered in order. Dropped silently after the session closed.
//   virtual void SendDevToolsMessage(uint64_t session_id,
//                                    const char* json, size_t length) = 0;
// Detaches and forgets the session. Idempotent. Blocks until any in-flight
// callback for the session has returned, so the client may free its
// PhiDevToolsClient afterwards.
//   virtual void CloseDevToolsSession(uint64_t session_id) = 0;
// The discovery documents Chromium's DevTools HTTP handler used to serve, so
// the skill and the harness keep their /json/* probes unchanged:
//   "/json/version" -> {Browser, Protocol-Version, User-Agent, V8-Version,
//                       WebKit-Version, webSocketDebuggerUrl}
//   "/json/list" or "/json" -> [{id, type, title, url, description,
//                       webSocketDebuggerUrl, devtoolsFrontendUrl, ...}]
// `webSocketDebuggerUrl` values use the path form the client's pipe serves
// (ws://phi/devtools/browser/<id>, ws://phi/devtools/page/<id>); the client
// rewrites the host if it needs to. Synchronous from any non-UI thread
// (bounded wait on a UI hop); false for an unknown path or on timeout. Must
// NOT be called on the UI thread. The document is appended to `out` on the
// calling thread before the call returns (rule 6).
//   virtual bool DevToolsDiscoveryJson(const char* path, PhiStringSink* out) = 0;

// The signature the client exe's PhiClientCreateDelegate export must match.
// Returns a delegate that outlives the process (ChromeMain never returns).
using PhiClientCreateDelegateFn = PhiFrameworkDelegate* (*)();

// Identifies this header's binary contract. Bump it on any change a client
// built against the old header could not run with: a virtual added, removed or
// moved, or a struct's layout changed. The host (Phi.exe) refuses a client
// whose PhiClientAbiVersion export returns anything else.
inline constexpr uint32_t kPhiFrameworkAbiVersion = 1;
using PhiClientAbiVersionFn = uint32_t (*)();

}  // namespace phi_framework

#endif  // CHROME_BROWSER_PHINOMENON_PHI_FRAMEWORK_PHI_FRAMEWORK_API_H_

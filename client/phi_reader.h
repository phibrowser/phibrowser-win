// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_READER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_READER_H_

#include <map>
#include <optional>
#include <set>
#include <string>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/values.h"

namespace phi_client {

class PhiSurfaceCoordinator;

// How Reader View presents an article — the Mac's ReaderStyle, and the wire
// format the extension's `style.ts` speaks. Everything is a raw string here
// because the client neither renders nor validates these: the reading surface
// is the extension's page, which owns the palettes and the CSS, and clamps
// anything it does not recognise back to its own default. The client is the
// STORE, so that one preference follows the reader between tabs, windows and
// launches.
struct PhiReaderStyle {
  // Body text in points, on the reader HUD's 15..39 step-2 grid.
  int font_size = 19;
  std::string width = "normal";        // narrow | normal | wide | full
  // matchBrowser | light | sepia | yellow | blue | dark | dim | contrast.
  std::string theme = "matchBrowser";
  std::string typeface = "serif";      // serif | sans | book | rounded | mono
  std::string line_height = "normal";  // tight | normal | relaxed | loose
  std::string letter_spacing = "normal";  // normal | wide | wider
  bool shows_images = true;
  bool highlights_links = true;

  // The wire/prefs object, field names shared with the extension.
  base::DictValue ToDict() const;
  std::string ToJson() const;
  // Fills from `dict`, ignoring anything absent or out of range — a
  // hand-edited PhiPrefs.json cannot put the reader in a state its own
  // controls could not reach.
  void MergeFrom(const base::DictValue& dict);
};

// Reader View is the Mirage extension's (chrome/browser/resources/phi_mirage,
// built from phi-ai/ai-extension/mirage). It probes every page, extracts the
// article in-page and renders it on a reading surface of its own; the client's
// part is intent — the button, the menu item, the shortcut and the page
// context menu all end here, and the extension reports back what it did.
//
// Five message types meet in this class. `reader.offerable`, `reader.state`,
// `reader.getStyle`, `reader.setStyle` and `reader.extractResult` arrive from
// the extension through PhiFrameworkDelegate::OnExtensionMessage;
// `reader.open`, `reader.close`, `reader.extract` and `reader.styleChanged`
// are broadcast back. Payload field names are shared with the extension's
// `src/common/messages.ts` — keep both in sync.
//
// The Mac twin is Sources/States/ReaderExtensionBridge.swift.
class PhiReaderBridge {
 public:
  explicit PhiReaderBridge(PhiSurfaceCoordinator* owner);
  ~PhiReaderBridge();

  PhiReaderBridge(const PhiReaderBridge&) = delete;
  PhiReaderBridge& operator=(const PhiReaderBridge&) = delete;

  // The extension hosting Reader View, derived from its manifest signing key.
  // Reader messages from any other sender are DROPPED: every extension shares
  // the one bridge, and a page-installed extension must not be able to light
  // the reader button or flip a tab's state.
  static const char* extension_id();

  // True for the extension's own reading surface. The tab it stands in for
  // and the article's source are carried in the URL fragment
  // (`reader.html#tab=<originTabId>&src=<encoded url>`, the extension's
  // `readerPageUrl`); anything else — including a reader page from a build
  // without the parameters — returns nullopt.
  static bool IsReaderPageUrl(const std::string& url);
  static std::optional<int> OriginTabIdOfReaderPage(const std::string& url);
  static std::optional<std::string> SourceUrlOfReaderPage(
      const std::string& url);

  // Framework -> client. True when the message was ours and has been answered;
  // false leaves it for the next handler.
  bool HandleExtensionMessage(const std::string& request_id,
                              const std::string& sender_id,
                              const std::string& type,
                              const std::string& payload_json);

  // Whether the reader affordance should be offered for `tab_id` showing
  // `url`. The grant is held only as long as the tab stays on the document it
  // was given for — a probe verdict is about one page, not one tab.
  bool IsOfferable(int tab_id, const std::string& url) const;
  // Whether the extension's reading surface is up for this ORIGIN tab.
  bool HasSurface(int tab_id) const;
  // Whether a reader-open for this origin is still in flight. What tells the
  // surface tab in the strip apart from a leftover reader page restored from
  // a previous session, which has no open behind it and is closed on sight.
  bool HasPendingOpen(int tab_id) const;

  // Entry points. `url` is the tab's current URL, which is what tells a live
  // page apart from the reading surface standing in for one. `entry_point`
  // is which control asked — the Mac's ReaderViewAnalytics.EntryPoint:
  // "address_bar", "view_menu", "shortcut", "context_menu" — recorded here
  // and reported by the `reader.state` report that answers the request
  // (reader_view_opened / reader_view_open_failed), so the metric says what
  // the surface DID, not what was asked.
  void Toggle(int tab_id, const std::string& url, const char* entry_point);
  void Open(int tab_id, const char* entry_point);
  void Close(int tab_id);

  // The agent API's readerArticle / readerDocument, extracted WITHOUT
  // navigating the tab. `settle` walks the page for lazy content first (no
  // user is watching). The answer arrives on the coordinator's deferred-reply
  // slot `pending_reply_id`. False when the broadcast reached no extension,
  // which is the caller's cue to fall back to the framework's distiller.
  bool StartExtraction(int tab_id, int pending_reply_id, bool settle);

  const PhiReaderStyle& style() const { return style_; }
  PhiReaderStyle& mutable_style() { return style_; }

  // Whether the reader extension has spoken to this session at all. Its probe
  // content script runs on every http/https load, so a browser that has shown
  // a page and heard nothing is one where Reader View is not installed or is
  // disabled — the cue to answer an extraction from upstream's distiller
  // rather than broadcast into silence and wait out a timeout.
  bool ExtensionAlive() const { return extension_seen_; }

 private:
  // Pushes the current style to every reader page, so several open readers
  // stay in agreement.
  void BroadcastStyle();
  bool Broadcast(const std::string& type, const std::string& payload);
  // `{"tabId":N,"windowId":W<extra>}` — the window id only when the tab is on
  // screen somewhere, so the broadcast reaches the profile that owns it.
  std::string TabPayload(int tab_id, const std::string& extra = std::string());
  // The toast the Mac shows for a refused open, in the window displaying the
  // tab (nothing when the tab is not on screen anywhere).
  void ShowRefusalToast(int tab_id, const std::string& error);
  // The entry point recorded for the tab's open request, consumed; "unknown"
  // when none was.
  const char* TakeEntryPoint(int tab_id);

  RAW_PTR_EXCLUSION PhiSurfaceCoordinator* const owner_;  // Owns this.
  PhiReaderStyle style_;
  bool extension_seen_ = false;
  // The URL each tab's positive probe verdict was about. Absence = no offer,
  // and only a POSITIVE report ever writes here (see HandleExtensionMessage).
  std::map<int, std::string> offerable_;
  // ORIGIN tab ids whose reading surface is currently mounted.
  std::set<int> surfaces_;
  // Origin tabs with an open request in flight (tick when it went out), so a
  // second press while the extractor is still working cannot spawn a
  // duplicate surface tab. Cleared by the `reader.state` report that answers
  // the request — and, failing that, by age: an extension that crashed
  // mid-extraction must not lock a tab out of Reader View for the session.
  std::map<int, unsigned long long> pending_opens_;
  // The most recent open request's entry point per origin tab, consumed
  // by the state report that answers it; a report with none recorded is
  // attributed to "unknown" rather than dropped.
  std::map<int, const char*> pending_entry_points_;
  // reader.extract requests in flight: our request id -> the coordinator's
  // deferred-reply slot.
  std::map<std::string, int> pending_extractions_;
  int next_extract_id_ = 1;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_READER_H_

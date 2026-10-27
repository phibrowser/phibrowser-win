// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_surface_coordinator.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/escape.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_shell_window.h"
#include "phibrowser-win/client/phi_surface_visibility.h"
#include "phibrowser-win/client/phi_window_model.h"

namespace phi_client {

PhiSurfaceCoordinator::PhiSurfaceCoordinator(const PhiWindowModel& model,
                                             Host& host)
    : window_model_(model), host_(host) {}

PhiSurfaceCoordinator::~PhiSurfaceCoordinator() = default;

void PhiSurfaceCoordinator::Init(phi_framework::PhiFramework* framework) {
  framework_ = framework;
}

void PhiSurfaceCoordinator::CaptureAnalytics(const char* module,
                                             const char* event_name,
                                             base::DictValue properties) {
  host_.CaptureAnalytics(module, event_name, std::move(properties));
}

void PhiSurfaceCoordinator::RespondToExtension(
    const std::string& request_id,
    const std::string& response_json) {
  if (framework_ && !request_id.empty()) {
    framework_->RespondToExtensionMessage(request_id.c_str(),
                                          response_json.c_str());
  }
}

bool PhiSurfaceCoordinator::BroadcastToExtensions(
    const std::string& type,
    const std::string& payload_json) {
  return framework_ &&
         framework_->BroadcastToExtensions(type.c_str(), payload_json.c_str());
}

PhiShellWindow* PhiSurfaceCoordinator::WindowShowingTab(TabId tab_id) {
  return host_.WindowShowingTab(tab_id);
}

bool PhiSurfaceCoordinator::HandleExtensionMessage(
    const std::string& request_id,
    const std::string& sender_id,
    const std::string& type,
    const std::string& payload_json) {
  return reader_.HandleExtensionMessage(request_id, sender_id, type,
                                        payload_json);
}

void PhiSurfaceCoordinator::OnOpenReaderView(int64_t window_id,
                                             int64_t tab_id) {
  // Right-click "Open in Reading Mode" in the page's own context menu.
  // ENTERS rather than toggles: the item is only reachable by right-clicking
  // the LIVE page, which is what is on screen when the reader is closed.
  const int tab = static_cast<int>(tab_id);
  LOG(INFO) << "[PhiClient] context menu reader win=" << window_id
            << " tab=" << tab;
  if (reader_.HasSurface(tab)) {
    return;
  }
  reader_.Open(tab, "context_menu");
}

bool PhiSurfaceCoordinator::HasPeek() const {
  return surfaces_.HasKind(SidebarSurface::kPeek);
}

bool PhiSurfaceCoordinator::DeferRetireOnPlaceholder(BrowserId browser,
                                                     bool entered) {
  if (!entered || !surfaces_.Find(browser)) {
    return false;
  }
  host_.PostReaderRetire(browser);
  return true;
}

void PhiSurfaceCoordinator::RetireReaderPanel(BrowserId panel_window) {
  if (surfaces_.Find(panel_window) && framework_) {
    framework_->CloseBrowserWindow(panel_window.value);
  }
}

namespace {

// A standalone HTML document around a distilled article — the skill's
// saveArticle() writes this to disk. A <base> keeps the article's relative
// image URLs resolving against the source; images are NOT inlined (the
// Mac's inliner is its own service), so the file opens with the network.
std::string BuildReaderDocument(const std::string& title,
                                const std::string& source_url,
                                const std::string& html) {
  const std::string safe_title = base::EscapeForHTML(title);
  return "<!doctype html>\n<html><head><meta charset=\"utf-8\">\n<title>" +
         safe_title + "</title>\n<base href=\"" +
         base::EscapeForHTML(source_url) +
         "\">\n<meta name=\"viewport\" content=\"width=device-width, "
         "initial-scale=1\">\n<style>\n"
         "body{margin:0;background:#fbfaf7;color:#1c1c1e;"
         "font:18px/1.65 -apple-system,\"Segoe UI\",system-ui,sans-serif}\n"
         "main{max-width:680px;margin:0 auto;padding:48px 24px 96px}\n"
         "h1{font-size:2em;line-height:1.2;margin:0 0 .4em}\n"
         ".source{color:#6b6b70;font-size:.85em;margin:0 0 2em;"
         "word-break:break-all}\n"
         "img,video{max-width:100%;height:auto}\n"
         "pre{overflow:auto;background:#f0efe9;padding:12px;"
         "border-radius:8px}\n"
         "a{color:#2f6fd6}\n</style></head>\n<body><main>\n<h1>" +
         safe_title + "</h1>\n<p class=\"source\"><a href=\"" +
         base::EscapeForHTML(source_url) + "\">" +
         base::EscapeForHTML(source_url) + "</a></p>\n<article>\n" + html +
         "\n</article>\n</main></body></html>\n";
}

// "<title>.html" with the characters a filename cannot carry replaced.
std::string ReaderSuggestedFileName(const std::string& title) {
  std::string name;
  for (unsigned char c : title) {
    if (c < 32 || std::string_view("<>:\"/\\|?*").find(static_cast<char>(c)) !=
                      std::string_view::npos) {
      name.push_back('-');
    } else {
      name.push_back(static_cast<char>(c));
    }
  }
  while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
    name.pop_back();
  }
  if (name.size() > 80) {
    name.resize(80);
  }
  return (name.empty() ? std::string("phi-article") : name) + ".html";
}

}  // namespace

void PhiSurfaceCoordinator::OnReaderArticleExtracted(int request_id,
                                                     const char* json) {
  // Shape the framework's article into the verb's reply. The Mac's fields
  // that come from ITS extraction ladder (rung, coverage, byline, siteName,
  // lang) are answered honestly: the rung is the distiller, coverage is
  // unknown, and a page the distiller finds nothing in is
  // no_article_detected exactly as on the Mac.
  bool document = false;
  bool include_html = true;
  if (!host_.PendingReplyShape(request_id, &document, &include_html)) {
    return;
  }
  std::optional<base::DictValue> article =
      base::JSONReader::ReadDict(json ? json : "{}", base::JSON_PARSE_RFC);
  base::DictValue out;
  if (!article || !article->FindBool("ok").value_or(false)) {
    out.Set("ok", false);
    const std::string* error = article ? article->FindString("error") : nullptr;
    out.Set("error", error ? *error : "no_article_detected");
  } else {
    const std::string* title = article->FindString("title");
    const std::string* source = article->FindString("sourceURL");
    const std::string* html = article->FindString("contentHTML");
    out.Set("ok", true);
    out.Set("title", title ? *title : "");
    out.Set("sourceURL", source ? *source : "");
    // The reader extension's ladder knows which rung produced the article,
    // how much of the page it covered, who wrote it and which site rule
    // fired; the framework's distiller knows none of that, so each field
    // rides along only when its source supplied it. An answer with no rung
    // came from the distiller and says so.
    const std::string* rung = article->FindString("rung");
    out.Set("rung", rung ? *rung : "distiller");
    for (const char* key : {"byline", "siteName", "lang", "rule"}) {
      if (const std::string* value = article->FindString(key)) {
        out.Set(key, *value);
      }
    }
    if (std::optional<double> coverage = article->FindDouble("coverage")) {
      out.Set("coverage", *coverage);
    }
    if (std::optional<int> blocks = article->FindInt("codeBlockCount")) {
      out.Set("codeBlockCount", *blocks);
    }
    out.Set("isComplete", true);
    if (document) {
      out.Set("suggestedFileName",
              ReaderSuggestedFileName(title ? *title : ""));
      out.Set("document",
              BuildReaderDocument(title ? *title : "", source ? *source : "",
                                  html ? *html : ""));
    } else {
      out.Set("htmlLength", article->FindInt("htmlLength").value_or(0));
      out.Set("pageCount", article->FindInt("pageCount").value_or(1));
      if (include_html) {
        out.Set("contentHTML", html ? *html : "");
      }
    }
  }
  std::string reply;
  base::JSONWriter::Write(out, &reply);
  host_.CompleteReaderReply(request_id, std::move(reply));
}

namespace {

// The reader's live style (PhiReaderStyle, a base::Value codec) and the
// model's (STL, so the header stays on the host) are the same eight
// fields; the copy lives here, in one place each way.
model::ReaderStyle ReaderStyleToModel(const PhiReaderStyle& style) {
  model::ReaderStyle reader;
  reader.font_size = style.font_size;
  reader.width = style.width;
  reader.theme = style.theme;
  reader.typeface = style.typeface;
  reader.line_height = style.line_height;
  reader.letter_spacing = style.letter_spacing;
  reader.shows_images = style.shows_images;
  reader.highlights_links = style.highlights_links;
  return reader;
}

void ReaderStyleFromModel(const model::ReaderStyle& reader,
                          PhiReaderStyle* style) {
  style->font_size = reader.font_size;
  style->width = reader.width;
  style->theme = reader.theme;
  style->typeface = reader.typeface;
  style->line_height = reader.line_height;
  style->letter_spacing = reader.letter_spacing;
  style->shows_images = reader.shows_images;
  style->highlights_links = reader.highlights_links;
}

}  // namespace

void PhiSurfaceCoordinator::SetReaderStyleFromModel(
    const model::ReaderStyle& style) {
  ReaderStyleFromModel(style, &reader_.mutable_style());
}

void PhiSurfaceCoordinator::OnReaderStyleChanged() {
  // The reader's live style is PhiReaderBridge's; the preferences carry the
  // copy the store writes.
  host_.PersistReaderStyle(ReaderStyleToModel(reader_.style()));
}

bool PhiSurfaceCoordinator::PanelizeReaderSurface(TabId origin_tab_id) {
  if (!framework_ || HasReaderPanelForOrigin(origin_tab_id)) {
    return false;
  }
  PhiShellWindow* window = host_.WindowShowingTab(origin_tab_id);
  if (!window) {
    return false;
  }
  const BrowserId browser = window->active_browser_id();
  TabId surface_tab_id;
  if (const phi_framework::WindowSnapshot* snapshot =
          window_model_.Get(browser)) {
    for (const phi_framework::TabSnapshot& tab : snapshot->tabs) {
      if (tab.surface == phi_framework::TabSurface::kReader &&
          tab.origin == origin_tab_id) {
        surface_tab_id = tab.id;
        break;
      }
    }
  }
  if (!surface_tab_id.valid()) {
    // Not an error: the framework panels a surface at insertion (see
    // DivertReaderSurfaceAfterInsert); by the time the extension reports
    // the open, the tab is usually already out of this strip.
    return false;
  }
  const BrowserId panel{framework_->MoveTabToReaderPanel(
      browser.value, surface_tab_id.value, origin_tab_id.value)};
  if (!panel.valid()) {
    LOG(WARNING) << "[PhiClient] could not panel reader surface "
                 << surface_tab_id.value;
    return false;
  }
  if (PhiSurfacePanel* panelled = surfaces_.Find(panel)) {
    panelled->BindSurfaceTab(surface_tab_id.value);
  }
  // Chromium foregrounded the surface when the extension created it, and
  // moving it out leaves whatever neighbour it landed on active. Hand the
  // strip back to the origin so tab-scoped commands keep targeting the
  // article the reader is showing — the Mac's setAsActiveTab step.
  framework_->ActivateTabById(browser.value, origin_tab_id.value);
  window->ScheduleRefresh();
  return true;
}

bool PhiSurfaceCoordinator::HasReaderPanelForOrigin(TabId origin_tab_id) const {
  return surfaces_.ForOrigin(origin_tab_id, SidebarSurface::kReader) != nullptr;
}

void PhiSurfaceCoordinator::CloseReaderPanelForOrigin(TabId origin_tab_id) {
  CloseSurfacesForOrigin(origin_tab_id, SidebarSurface::kReader);
}

void PhiSurfaceCoordinator::CloseSurfacesForOrigin(TabId origin_tab_id,
                                                   SidebarSurface kind,
                                                   const char* peek_end_reason,
                                                   bool hands_back_strip) {
  if (!framework_) {
    return;
  }
  // Snapshot first: closing a browser runs OnBrowserWindowClosing, which
  // detaches the record from the registry.
  std::vector<BrowserId> doomed;
  surfaces_.ForEach([&](PhiSurfacePanel& panel) {
    if (panel.origin_tab_id() == origin_tab_id.value &&
        (kind == SidebarSurface::kContent || panel.kind() == kind)) {
      panel.NoteEndReason(peek_end_reason);
      if (!hands_back_strip) {
        panel.set_hands_back_strip(false);
      }
      doomed.push_back(BrowserId{panel.window_id()});
    }
  });
  for (const BrowserId window : doomed) {
    // Closing the window closes the surface tab with it; the reader
    // extension drops its cache on tabs.onRemoved, so both sides agree.
    framework_->CloseBrowserWindow(window.value);
  }
}

void PhiSurfaceCoordinator::DeferCloseSurfacesForOrigin(
    TabId origin_tab_id,
    SidebarSurface kind,
    const char* peek_end_reason,
    bool hands_back_strip) {
  if (!framework_) {
    return;
  }
  bool queued = false;
  surfaces_.ForEach([&](PhiSurfacePanel& panel) {
    const BrowserId window{panel.window_id()};
    if (panel.origin_tab_id() != origin_tab_id.value ||
        (kind != SidebarSurface::kContent && panel.kind() != kind) ||
        PendingClose(window)) {
      return;
    }
    panel.NoteEndReason(peek_end_reason);
    if (!hands_back_strip) {
      panel.set_hands_back_strip(false);
    }
    // Out of sight now; the browser goes on the next turn.
    panel.SetSurfaceShown(false);
    pending_closes_.push_back(window);
    queued = true;
  });
  if (queued) {
    host_.PostSurfaceTurn();
  }
}

bool PhiSurfaceCoordinator::PendingClose(BrowserId panel) const {
  return std::find(pending_closes_.begin(), pending_closes_.end(), panel) !=
         pending_closes_.end();
}

void PhiSurfaceCoordinator::RunDeferredSurfaceWork() {
  // Taken first: a split open closes a peek, and a close runs
  // OnBrowserWindowClosing, either of which may queue more.
  std::vector<std::pair<BrowserId, std::string>> splits;
  splits.swap(pending_split_opens_);
  for (const auto& [surface_window, url] : splits) {
    FinishOpenSurfaceLinkAsSplit(surface_window, url);
  }
  std::vector<BrowserId> closes;
  closes.swap(pending_closes_);
  for (const BrowserId window : closes) {
    // Gone already (dismissed, its opener closed) — nothing to close.
    if (framework_ && surfaces_.Find(window)) {
      framework_->CloseBrowserWindow(window.value);
    }
  }
}

void PhiSurfaceCoordinator::ExpandReaderPanelIntoTab(int64_t panel_window_id,
                                                     const std::string& url) {
  const BrowserId panel{panel_window_id};
  if (!surfaces_.Find(panel)) {
    return;
  }
  pending_reader_expands_[panel] = url;
  host_.PostReaderExpand(panel);
}

void PhiSurfaceCoordinator::FinishReaderPanelExpand(BrowserId panel_window) {
  auto pending = pending_reader_expands_.find(panel_window);
  if (pending == pending_reader_expands_.end()) {
    return;
  }
  const std::string url = pending->second;
  pending_reader_expands_.erase(pending);
  PhiSurfacePanel* reader = surfaces_.Find(panel_window);
  if (!reader || !framework_) {
    return;
  }
  const BrowserId origin_window{reader->origin_window_id()};
  // Like Peek's expand, this RE-OPENS the url rather than moving the live tab
  // across: the destination has only just started loading, and moving the tab
  // out would empty the panel browser mid-move.
  framework_->OpenTab(origin_window, url.c_str());
  framework_->CloseBrowserWindow(panel_window.value);
}

void PhiSurfaceCoordinator::UpdateReaderPanelsForWindow(BrowserId window_id) {
  const phi_framework::WindowSnapshot* snapshot = window_model_.Get(window_id);
  if (!framework_ || !snapshot) {
    return;
  }
  const phi_framework::TabSnapshot* active_snapshot =
      window_model_.ActiveTab(window_id);
  const TabId active_tab = active_snapshot ? active_snapshot->id : TabId();
  // The framework panels a reader surface at insertion and closes husks
  // (DivertReaderSurfaceAfterInsert); this is the FALLBACK for one it could
  // not place — a live surface (the extension reported it up, or an open is
  // still in flight) goes into a panel over its origin, anything else is a
  // husk and closes, as the Mac does on sight.
  for (const phi_framework::TabSnapshot& tab : snapshot->tabs) {
    if (tab.surface != phi_framework::TabSurface::kReader) {
      continue;
    }
    const TabId origin = tab.origin;
    if (!origin.valid() || HasReaderPanelForOrigin(origin)) {
      continue;
    }
    if (reader_.HasSurface(origin.value) ||
        reader_.HasPendingOpen(origin.value)) {
      PanelizeReaderSurface(origin);
      return;  // The strip changed either way; its push re-runs this.
    }
    LOG(INFO) << "[PhiClient] closing orphaned reader surface tab in window "
              << window_id.value;
    framework_->CloseTabById(window_id.value, tab.id.value);
    return;  // The close's own push re-runs this on the new strip.
  }
  // Every surface over this window, both kinds: an origin that left the
  // strip — closed, torn off, moved to another Space — takes its surface
  // with it (the Mac's origin watch); the rest re-fit and show by the Mac's
  // rules over the active tab and its split partner: a peek covers its
  // opener's whole split, each pane keeps its own reader, and a peek in
  // front hides the pane readers (phi_surface_visibility.h).
  std::vector<BrowserId> orphaned;
  std::vector<PhiSurfacePanel*> live;
  std::map<int, int> peeks;  // Opener tab -> the peek's own tab.
  std::set<int> readers;     // Origin tabs with a reader over them.
  for (PhiSurfacePanel* panel : surfaces_.OverWindow(window_id)) {
    const int origin = static_cast<int>(panel->origin_tab_id());
    if (PendingClose(BrowserId{panel->window_id()})) {
      continue;  // Closing on the next turn: already out of the picture.
    }
    if (!window_model_.FindTab(window_id, TabId{origin})) {
      panel->NoteEndReason("opener_closed");
      orphaned.push_back(BrowserId{panel->window_id()});
      continue;
    }
    live.push_back(panel);
    if (panel->kind() == SidebarSurface::kPeek) {
      peeks[origin] = static_cast<int>(panel->surface_tab_id());
    } else {
      readers.insert(origin);
    }
  }
  const int focused = active_tab.value;
  const int partner = SplitPartnerOf(window_id, active_tab).value;
  // A window hosts one browser per Space and shows one: surfaces over a
  // browser that is not on screen stay hidden (and keep their fit — the
  // pane rects are the shown browser's).
  const bool on_screen =
      active_tab.valid() && host_.WindowShowingTab(active_tab) != nullptr;
  const PhiVisiblePeek peek = PhiVisiblePeekFor(focused, partner, peeks);
  const std::vector<int> shown_readers =
      PhiVisibleReaderOrigins(focused, partner, peeks, readers);
  for (PhiSurfacePanel* panel : live) {
    const int origin = static_cast<int>(panel->origin_tab_id());
    const bool shown =
        panel->kind() == SidebarSurface::kPeek
            ? origin == peek.opener
            : std::find(shown_readers.begin(), shown_readers.end(), origin) !=
                  shown_readers.end();
    if (on_screen) {
      panel->Reposition();
    }
    panel->SetSurfaceShown(shown && on_screen);
  }
  for (const BrowserId panel_id : orphaned) {
    LOG(INFO) << "[PhiClient] surface " << panel_id.value
              << " origin left the strip — closing";
    if (framework_) {
      framework_->CloseBrowserWindow(panel_id.value);
    }
  }
}

BrowserId PhiSurfaceCoordinator::PresentedPeek(BrowserId window_id) const {
  const phi_framework::TabSnapshot* active_snapshot =
      window_model_.ActiveTab(window_id);
  if (!active_snapshot) {
    return BrowserId();
  }
  const TabId active_tab = active_snapshot->id;
  // The same picture UpdateReaderPanelsForWindow shows the panels by: the
  // live peeks over this window, opener tab -> the peek's own tab, with the
  // panel browser kept beside it. A replaced peek awaiting its deferred
  // close is hidden already and must not be the answer.
  std::map<int, int> peeks;
  std::map<int, BrowserId> browsers;  // Opener tab -> the peek's browser.
  for (PhiSurfacePanel* panel : surfaces_.OverWindow(window_id)) {
    const BrowserId panel_id{panel->window_id()};
    if (panel->kind() != SidebarSurface::kPeek || PendingClose(panel_id)) {
      continue;
    }
    const int origin = static_cast<int>(panel->origin_tab_id());
    peeks[origin] = static_cast<int>(panel->surface_tab_id());
    browsers[origin] = panel_id;
  }
  if (peeks.empty()) {
    return BrowserId();
  }
  const PhiVisiblePeek peek = PhiVisiblePeekFor(
      active_tab.value, SplitPartnerOf(window_id, active_tab).value, peeks);
  if (!peek.valid()) {
    return BrowserId();
  }
  const auto it = browsers.find(peek.opener);
  return it != browsers.end() ? it->second : BrowserId();
}

TabId PhiSurfaceCoordinator::SplitPartnerOf(BrowserId window,
                                            TabId tab) const {
  const phi_framework::SplitSnapshot* split =
      tab.valid() ? window_model_.SplitOfTab(window, tab) : nullptr;
  if (!split) {
    return TabId();
  }
  return split->primary == tab ? split->secondary : split->primary;
}

bool PhiSurfaceCoordinator::PairPeekPageWith(PhiSurfacePanel& peek,
                                             const std::string& partner_url,
                                             bool partner_leading) {
  // The peek's page, read from its own browser's snapshot: the panel's copy
  // is for its header.
  const BrowserId peek_window{peek.window_id()};
  const phi_framework::WindowSnapshot* snapshot =
      window_model_.Get(peek_window);
  const std::string page_url = snapshot ? snapshot->nav.url : std::string();
  if (!framework_ || page_url.empty() || partner_url.empty()) {
    return false;
  }
  const BrowserId window{peek.origin_window_id()};
  LOG(INFO) << "[PhiClient] peek " << peek_window.value
            << " opened as split with " << partner_url;
  // Strip order is pane order: the tab opened first is the leading pane.
  const std::string& first_url = partner_leading ? partner_url : page_url;
  const std::string& second_url = partner_leading ? page_url : partner_url;
  const TabId first = framework_->OpenTab(window, first_url.c_str());
  const TabId second = framework_->OpenTab(window, second_url.c_str());
  if (first.valid() && second.valid()) {
    framework_->CreateSplitViewForTabs(window.value, first.value, second.value,
                                       "vertical");
  }
  // The partner's new tab takes focus and the split forms around it; the
  // peek's close must not hand the strip back to its opener after that.
  const TabId partner = partner_leading ? first : second;
  if (partner.valid()) {
    framework_->ActivateTabById(window.value, partner.value);
  }
  peek.NoteEndReason("opened_as_split");
  peek.set_hands_back_strip(false);
  framework_->CloseBrowserWindow(peek_window.value);
  return true;
}

void PhiSurfaceCoordinator::OpenSurfaceLinkAsSplit(BrowserId surface_window,
                                                   const std::string& url) {
  // The framework reports this from the surface page's own gesture — the
  // context menu's command, OpenURLFromTab for an Alt-click — with that
  // page's WebContents on the stack, and the peek case closes its browser
  // (PairPeekPageWith). Deferred a turn, like the reader expand.
  if (!surfaces_.Find(surface_window) || !framework_ || url.empty()) {
    LOG(WARNING) << "[PhiClient] split from surface " << surface_window.value
                 << ": no such surface";
    return;
  }
  pending_split_opens_.emplace_back(surface_window, url);
  host_.PostSurfaceTurn();
}

void PhiSurfaceCoordinator::FinishOpenSurfaceLinkAsSplit(
    BrowserId surface_window,
    const std::string& url) {
  PhiSurfacePanel* surface = surfaces_.Find(surface_window);
  if (!surface || !framework_ || url.empty()) {
    return;  // The surface went while the turn was pending.
  }
  if (surface->kind() == SidebarSurface::kPeek) {
    if (!PairPeekPageWith(*surface, url, /*partner_leading=*/false)) {
      // The peek's page has not reported a URL yet: the link alone, as a
      // tab of the opener window, rather than nothing.
      framework_->OpenTab(BrowserId{surface->origin_window_id()},
                          url.c_str());
    }
    return;
  }
  // A reader: its origin tab pairs with the link, and the reader stays over
  // the origin's pane.
  const BrowserId window{surface->origin_window_id()};
  const TabId origin{static_cast<int32_t>(surface->origin_tab_id())};
  if (const TabId partner = SplitPartnerOf(window, origin); partner.valid()) {
    // Already a pane: the link replaces the other pane's page, as a split
    // link open from any pane does (Browser::OpenURLFromTab). Whatever
    // stood over that page goes first — it cannot stay over a page that
    // changed beneath it.
    CloseSurfacesForOrigin(partner);
    framework_->ActivateTabById(window.value, partner.value);
    framework_->Navigate(window.value, url.c_str());
    return;
  }
  const TabId link = framework_->OpenTab(window, url.c_str());
  if (!link.valid()) {
    return;
  }
  framework_->CreateSplitViewForTabs(window.value, origin.value, link.value,
                                     "vertical");
  framework_->ActivateTabById(window.value, link.value);
}

bool PhiSurfaceCoordinator::OpenPeekBesideSplitOpener(BrowserId peek_window) {
  PhiSurfacePanel* peek = surfaces_.Find(peek_window);
  if (!peek || peek->kind() != SidebarSurface::kPeek || !framework_) {
    return false;
  }
  const BrowserId window{peek->origin_window_id()};
  const TabId opener{static_cast<int32_t>(peek->origin_tab_id())};
  if (!SplitPartnerOf(window, opener).valid()) {
    return false;
  }
  const phi_framework::TabSnapshot* opener_tab =
      window_model_.FindTab(window, opener);
  const std::string opener_url = opener_tab ? opener_tab->url : std::string();
  if (PairPeekPageWith(*peek, opener_url, /*partner_leading=*/true)) {
    return true;
  }
  // Nothing to duplicate (or no page yet): expand instead, as the Mac does.
  const phi_framework::WindowSnapshot* snapshot = window_model_.Get(peek_window);
  if (snapshot && !snapshot->nav.url.empty()) {
    framework_->OpenTab(window, snapshot->nav.url.c_str());
  }
  peek->NoteEndReason("opened_as_tab");
  framework_->CloseBrowserWindow(peek_window.value);
  return true;
}

bool PhiSurfaceCoordinator::ClosePartnerPeek(BrowserId window_id,
                                             TabId tab_id) {
  const TabId partner = SplitPartnerOf(window_id, tab_id);
  if (!partner.valid() || surfaces_.ForOrigin(tab_id, SidebarSurface::kPeek) ||
      !surfaces_.ForOrigin(partner, SidebarSurface::kPeek)) {
    return false;
  }
  LOG(INFO) << "[PhiClient] back/forward on pane " << tab_id.value
            << " — closing the peek of " << partner.value;
  // The focused pane keeps focus: the opener is only the peek's anchor.
  CloseSurfacesForOrigin(partner, SidebarSurface::kPeek, "closed",
                         /*hands_back_strip=*/false);
  return true;
}

void PhiSurfaceCoordinator::ActivatePaneUnderPress(HWND target) {
  if (!framework_ || !target) {
    return;
  }
  PhiSurfacePanel* pressed = nullptr;
  surfaces_.ForEach([&](PhiSurfacePanel& panel) {
    if (panel.kind() == SidebarSurface::kReader &&
        ::IsWindowVisible(panel.hwnd()) && panel.ContainsWindow(target)) {
      pressed = &panel;
    }
  });
  if (!pressed) {
    return;
  }
  const BrowserId window{pressed->origin_window_id()};
  const TabId origin{static_cast<int32_t>(pressed->origin_tab_id())};
  const phi_framework::TabSnapshot* active = window_model_.ActiveTab(window);
  if (!SplitPartnerOf(window, origin).valid() ||
      (active && active->id == origin)) {
    return;
  }
  // Activating the pane moves Chromium's focus into the page under the
  // reader; the press already gave it to the reader, so it goes back there.
  const HWND focus = ::GetFocus();
  framework_->ActivateTabById(window.value, origin.value);
  if (focus && ::IsWindow(focus) && ::GetFocus() != focus) {
    ::SetFocus(focus);
  }
}

void PhiSurfaceCoordinator::OnReaderOfferabilityChanged(TabId tab_id) {
  // The reader button is laid out from this verdict, so the window showing
  // the tab has to repaint. At the next idle: the report can arrive during a
  // navigation the strip has not caught up with yet.
  host_.ScheduleRefreshForTab(tab_id);
}

bool PhiSurfaceCoordinator::IsReaderOfferable(int tab_id,
                                              const std::string& url) const {
  return reader_.IsOfferable(tab_id, url);
}

bool PhiSurfaceCoordinator::IsReaderActive(int tab_id,
                                           const std::string& url) const {
  return PhiReaderBridge::IsReaderPageUrl(url) || reader_.HasSurface(tab_id);
}

void PhiSurfaceCoordinator::RequestToggleReaderView(int tab_id,
                                                    const std::string& url,
                                                    const char* entry_point) {
  reader_.Toggle(tab_id, url, entry_point);
}

void PhiSurfaceCoordinator::RaiseReaderPanels(SlotId slot) {
  surfaces_.ForEach([&](PhiSurfacePanel& panel) {
    if (host_.SlotForBrowser(BrowserId{panel.origin_window_id()}) == slot) {
      panel.Raise();
    }
  });
}

void PhiSurfaceCoordinator::NoteContentLayoutChanged(SlotId slot) {
  // Re-settles every browser of the slot with a surface: a Space switch
  // changes which one is on screen, a layout change where its panes sit.
  std::set<int64_t> windows;
  surfaces_.ForEach([&](PhiSurfacePanel& panel) {
    if (host_.SlotForBrowser(BrowserId{panel.origin_window_id()}) == slot) {
      windows.insert(panel.origin_window_id());
    }
  });
  for (const int64_t window : windows) {
    UpdateReaderPanelsForWindow(BrowserId{window});
  }
}

bool PhiSurfaceCoordinator::DismissPeeksForShell(HWND shell,
                                                 HWND inside,
                                                 const POINT* screen_point) {
  if (!shell) {
    return false;
  }
  std::vector<BrowserId> to_dismiss;
  surfaces_.ForEach([&](PhiSurfacePanel& panel) {
    if (panel.kind() != SidebarSurface::kPeek) {
      return;
    }
    const HWND card = panel.hwnd();
    if (!card || ::GetAncestor(card, GA_ROOT) != shell) {
      return;
    }
    // A peek whose opener tab is not the active one is hidden, not "around"
    // anything: a click on another tab's page (or an Esc there) leaves it
    // for its own tab to come back to, as the Mac's per-tab peeks do.
    if (!::IsWindowVisible(card)) {
      return;
    }
    if (inside && panel.ContainsWindow(inside)) {
      return;  // A click INSIDE the card is the card's own business.
    }
    if (screen_point && !panel.ClickDismisses(*screen_point)) {
      return;  // Chrome, not the page pane: the peek keeps its place.
    }
    panel.NoteEndReason("closed");  // Esc / a click outside: the user's.
    to_dismiss.push_back(BrowserId{panel.window_id()});
  });
  // Closing the peek's browser is the dismissal (the card's own close chip
  // does the same); OnBrowserWindowClosing then detaches the record — so
  // iterate a snapshot, never the registry.
  for (const BrowserId window : to_dismiss) {
    if (framework_) {
      framework_->CloseBrowserWindow(window.value);
    }
  }
  return !to_dismiss.empty();
}

}  // namespace phi_client

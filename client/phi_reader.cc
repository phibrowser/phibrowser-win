// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_reader.h"

#include <algorithm>
#include <array>
#include <string_view>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "client/phi_client_analytics.h"
#include "client/phi_surface_coordinator.h"
#include "client/phi_i18n.h"
#include "client/phi_log.h"
#include "client/phi_toast.h"
#include "client/phi_shell_window.h"

namespace phi_client {

namespace {

// The extension carrying Reader View — Mirage (ai-extension/mirage, which
// ships the reading surface alongside its per-site page fixes), derived from
// its manifest signing key.
constexpr char kMirageExtensionId[] = "ickhcgejficcoofnjnnobadfdnfbilnm";

// The reading surface's page, whose fragment names the tab it stands in for.
constexpr char kReaderPagePath[] = "/reader.html";

// Message types, shared with the extension's src/common/messages.ts.
constexpr char kMsgOfferable[] = "reader.offerable";
constexpr char kMsgState[] = "reader.state";
constexpr char kMsgGetStyle[] = "reader.getStyle";
constexpr char kMsgSetStyle[] = "reader.setStyle";
constexpr char kMsgExtractResult[] = "reader.extractResult";
constexpr char kMsgOpen[] = "reader.open";
constexpr char kMsgClose[] = "reader.close";
constexpr char kMsgExtract[] = "reader.extract";
constexpr char kMsgStyleChanged[] = "reader.styleChanged";

// How long an unanswered open request suppresses the next one. Comfortably
// past the extension's own extraction budget (1.5 s prepare + 3 s ready plus
// the lazy-content settle), so this only ever fires for a reply that is never
// coming.
constexpr unsigned long long kOpenRetryMs = 30000;

// The HUD's text-size grid (ReaderDocumentBuilder's min/max on the Mac).
constexpr int kMinFontSize = 15;
constexpr int kMaxFontSize = 39;

// The axes, with the extension's own vocabulary for each. A value outside its
// axis is dropped rather than stored: the reader would clamp it anyway, and a
// stored value the controls cannot produce is a preference nobody can undo.
constexpr std::array<std::string_view, 4> kWidths = {"narrow", "normal", "wide",
                                                     "full"};
constexpr std::array<std::string_view, 8> kThemes = {
    "matchBrowser", "light", "sepia", "yellow",
    "blue",         "dark",  "dim",   "contrast"};
constexpr std::array<std::string_view, 5> kTypefaces = {"serif", "sans", "book",
                                                        "rounded", "mono"};
constexpr std::array<std::string_view, 4> kLineHeights = {"tight", "normal",
                                                          "relaxed", "loose"};
constexpr std::array<std::string_view, 3> kLetterSpacings = {"normal", "wide",
                                                             "wider"};

template <size_t N>
void TakeIfKnown(const base::DictValue& dict,
                 const char* key,
                 const std::array<std::string_view, N>& allowed,
                 std::string* out) {
  const std::string* value = dict.FindString(key);
  if (!value) {
    return;
  }
  if (std::find(allowed.begin(), allowed.end(), *value) != allowed.end()) {
    *out = *value;
  }
}

// The reader page's URL prefix for this extension.
std::string ReaderPagePrefix() {
  return std::string("chrome-extension://") + kMirageExtensionId +
         kReaderPagePath;
}

// The value of `key` in a reader page's `#a=1&b=2` fragment, still escaped.
std::optional<std::string> FragmentParam(const std::string& url,
                                         std::string_view key) {
  const size_t hash = url.find('#');
  if (hash == std::string::npos) {
    return std::nullopt;
  }
  std::string_view fragment(url);
  fragment.remove_prefix(hash + 1);
  const std::string needle = std::string(key) + "=";
  while (!fragment.empty()) {
    const size_t amp = fragment.find('&');
    std::string_view pair =
        amp == std::string_view::npos ? fragment : fragment.substr(0, amp);
    if (pair.substr(0, needle.size()) == needle) {
      return std::string(pair.substr(needle.size()));
    }
    if (amp == std::string_view::npos) {
      break;
    }
    fragment.remove_prefix(amp + 1);
  }
  return std::nullopt;
}

}  // namespace

// -----------------------------------------------------------------------
// PhiReaderStyle
// -----------------------------------------------------------------------

base::DictValue PhiReaderStyle::ToDict() const {
  base::DictValue dict;
  dict.Set("fontSize", font_size);
  dict.Set("width", width);
  dict.Set("theme", theme);
  dict.Set("typeface", typeface);
  dict.Set("lineHeight", line_height);
  dict.Set("letterSpacing", letter_spacing);
  dict.Set("showsImages", shows_images);
  dict.Set("highlightsLinks", highlights_links);
  return dict;
}

std::string PhiReaderStyle::ToJson() const {
  std::string json;
  base::JSONWriter::Write(ToDict(), &json);
  return json;
}

void PhiReaderStyle::MergeFrom(const base::DictValue& dict) {
  if (std::optional<int> size = dict.FindInt("fontSize")) {
    font_size = std::min(kMaxFontSize, std::max(kMinFontSize, *size));
  }
  TakeIfKnown(dict, "width", kWidths, &width);
  TakeIfKnown(dict, "theme", kThemes, &theme);
  TakeIfKnown(dict, "typeface", kTypefaces, &typeface);
  TakeIfKnown(dict, "lineHeight", kLineHeights, &line_height);
  TakeIfKnown(dict, "letterSpacing", kLetterSpacings, &letter_spacing);
  shows_images = dict.FindBool("showsImages").value_or(shows_images);
  highlights_links =
      dict.FindBool("highlightsLinks").value_or(highlights_links);
}

// -----------------------------------------------------------------------
// PhiReaderBridge
// -----------------------------------------------------------------------

PhiReaderBridge::PhiReaderBridge(PhiSurfaceCoordinator* owner)
    : owner_(owner) {}

PhiReaderBridge::~PhiReaderBridge() = default;

// static
const char* PhiReaderBridge::extension_id() {
  return kMirageExtensionId;
}

// static
bool PhiReaderBridge::IsReaderPageUrl(const std::string& url) {
  const std::string prefix = ReaderPagePrefix();
  if (url.compare(0, prefix.size(), prefix) != 0) {
    return false;
  }
  // Only the page itself, never a longer path that merely starts with it.
  return url.size() == prefix.size() || url[prefix.size()] == '#' ||
         url[prefix.size()] == '?';
}

// static
std::optional<int> PhiReaderBridge::OriginTabIdOfReaderPage(
    const std::string& url) {
  if (!IsReaderPageUrl(url)) {
    return std::nullopt;
  }
  std::optional<std::string> raw = FragmentParam(url, "tab");
  int tab_id = 0;
  if (!raw || !base::StringToInt(*raw, &tab_id)) {
    return std::nullopt;
  }
  return tab_id;
}

// static
std::optional<std::string> PhiReaderBridge::SourceUrlOfReaderPage(
    const std::string& url) {
  if (!IsReaderPageUrl(url)) {
    return std::nullopt;
  }
  std::optional<std::string> raw = FragmentParam(url, "src");
  if (!raw) {
    return std::nullopt;
  }
  return base::UnescapeBinaryURLComponent(*raw);
}

bool PhiReaderBridge::HandleExtensionMessage(const std::string& request_id,
                                             const std::string& sender_id,
                                             const std::string& type,
                                             const std::string& payload_json) {
  if (type != kMsgOfferable && type != kMsgState && type != kMsgGetStyle &&
      type != kMsgSetStyle && type != kMsgExtractResult) {
    return false;
  }
  // Every extension shares this channel, so a reader message from anyone but
  // Mirage is dropped — and still answered, or the sender waits out its
  // timeout.
  if (sender_id != kMirageExtensionId) {
    LOG(WARNING) << "[Reader] " << type << " dropped: sender=" << sender_id;
    owner_->RespondToExtension(request_id, "{}");
    return true;
  }
  extension_seen_ = true;
  std::optional<base::DictValue> payload =
      base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
  if (!payload) {
    LOG(WARNING) << "[Reader] " << type << " dropped: bad payload";
    owner_->RespondToExtension(request_id, "{}");
    return true;
  }

  if (type == kMsgGetStyle) {
    // The reader page is mounting and wants the one stored style, so the
    // native store stays the source of truth across surfaces.
    owner_->RespondToExtension(request_id, style_.ToJson());
    return true;
  }

  if (type == kMsgSetStyle) {
    style_.MergeFrom(*payload);
    owner_->OnReaderStyleChanged();
    BroadcastStyle();
    owner_->RespondToExtension(request_id, "{}");
    return true;
  }

  if (type == kMsgOfferable) {
    const std::optional<int> tab_id = payload->FindInt("tabId");
    const bool offerable = payload->FindBool("offerable").value_or(false);
    const std::string* url = payload->FindString("url");
    // POSITIVE-ONLY, like the native distillability sink the Mac mirrors
    // here. The extension re-probes on every URL change the page makes, and
    // a re-probe that lands while the document is being rebuilt answers no —
    // acting on that would take the button away from a page the first probe
    // accepted. What retires a grant is the URL it was given for: the tab
    // moving off that document is the only thing that can rescind it, and
    // IsOfferable checks exactly that.
    if (tab_id && offerable) {
      offerable_[*tab_id] = url ? *url : std::string();
      LOG(INFO) << "[Reader] offerable granted tab=" << *tab_id << " url="
                << (url ? PhiRedactUrl(*url) : std::string("(unnamed)"));
      owner_->OnReaderOfferabilityChanged(TabId{*tab_id});
    }
    owner_->RespondToExtension(request_id, "{}");
    return true;
  }

  if (type == kMsgState) {
    const std::optional<int> tab_id = payload->FindInt("tabId");
    const bool active = payload->FindBool("active").value_or(false);
    const std::string* error = payload->FindString("error");
    LOG(INFO) << "[Reader] state tab=" << (tab_id ? *tab_id : -1)
              << " active=" << active
              << " error=" << (error ? *error : std::string("(none)"));
    if (tab_id) {
      if (active) {
        surfaces_.insert(*tab_id);
        // The extension made its surface a tab in the origin's own window.
        // Move it into a browser of its own and host that over the origin's
        // page — the Mac's overlay, by the only means this client has. Done
        // BEFORE the pending flag is dropped, so the restored-husk sweep
        // cannot mistake the surface for a leftover in the gap.
        owner_->PanelizeReaderSurface(TabId{*tab_id});
        // The Mac's reader_view_opened: the surface actually mounted.
        owner_->CaptureAnalytics(
            kAnalyticsModuleReader, kEventReaderOpened,
            PhiReaderOpenedProperties(TakeEntryPoint(*tab_id)));
      }
      pending_opens_.erase(*tab_id);
      if (!active) {
        surfaces_.erase(*tab_id);
        // The reader is no longer standing for this tab. The extension says
        // so for every reason there is — the origin navigated (from the
        // address bar, an agent, a redirect), the surface was removed, the
        // tab went away — so this one report retires the panel rather than
        // every navigation site having to remember to.
        owner_->CloseReaderPanelForOrigin(TabId{*tab_id});
        // A refused open never creates a surface — say so where the Mac does,
        // with the same two messages, and count it with its reason.
        if (error) {
          LOG(INFO) << "[Reader] open refused tab=" << *tab_id << ": "
                    << *error;
          ShowRefusalToast(*tab_id, *error);
          owner_->CaptureAnalytics(
              kAnalyticsModuleReader, kEventReaderOpenFailed,
              PhiReaderOpenFailedProperties(TakeEntryPoint(*tab_id), *error));
        } else {
          pending_entry_points_.erase(*tab_id);
        }
      }
      owner_->OnReaderOfferabilityChanged(TabId{*tab_id});
    }
    owner_->RespondToExtension(request_id, "{}");
    return true;
  }

  // reader.extractResult — the reply to a reader.extract request.
  const std::string* extract_id = payload->FindString("requestId");
  auto pending = extract_id ? pending_extractions_.find(*extract_id)
                            : pending_extractions_.end();
  if (pending == pending_extractions_.end()) {
    owner_->RespondToExtension(request_id, "{}");
    return true;
  }
  const int reply_id = pending->second;
  pending_extractions_.erase(pending);

  // Reshape into what the framework's own extraction delivers, so one shaper
  // in the coordinator answers both paths — plus the fields only the
  // extension's ladder knows (rung, coverage, byline, the rule that fired).
  base::DictValue article;
  const base::DictValue* extracted = payload->FindDict("article");
  if (!payload->FindBool("ok").value_or(false) || !extracted) {
    const std::string* error = payload->FindString("error");
    article.Set("ok", false);
    article.Set("error", error ? *error : "no_article_detected");
  } else {
    const auto copy_string = [&](const char* from, const char* to) {
      if (const std::string* value = extracted->FindString(from)) {
        if (!value->empty()) {
          article.Set(to, *value);
        }
      }
    };
    const std::string* html = extracted->FindString("contentHtml");
    article.Set("ok", true);
    copy_string("title", "title");
    copy_string("sourceUrl", "sourceURL");
    copy_string("byline", "byline");
    copy_string("siteName", "siteName");
    copy_string("lang", "lang");
    copy_string("rung", "rung");
    if (std::optional<double> coverage = extracted->FindDouble("coverage")) {
      article.Set("coverage", *coverage);
    }
    if (const base::ListValue* blocks = extracted->FindList("codeBlocks")) {
      article.Set("codeBlockCount", static_cast<int>(blocks->size()));
    }
    if (const std::string* rule = payload->FindString("ruleHost")) {
      article.Set("rule", *rule);
    }
    article.Set("contentHTML", html ? *html : "");
    article.Set("htmlLength", static_cast<int>(html ? html->size() : 0));
    // The extension extracts the whole document in one pass — there is no
    // paginated half-answer to report, unlike the reader's own first screen.
    article.Set("pageCount", 1);
  }
  std::string json;
  base::JSONWriter::Write(article, &json);
  owner_->OnReaderArticleExtracted(reply_id, json.c_str());
  owner_->RespondToExtension(request_id, "{}");
  return true;
}

bool PhiReaderBridge::IsOfferable(int tab_id, const std::string& url) const {
  auto it = offerable_.find(tab_id);
  if (it == offerable_.end()) {
    return false;
  }
  // An empty recorded URL means the extension did not name the document; take
  // the grant at face value then, as the Mac does.
  return it->second.empty() || it->second == url;
}

bool PhiReaderBridge::HasSurface(int tab_id) const {
  return surfaces_.count(tab_id) > 0;
}

bool PhiReaderBridge::HasPendingOpen(int tab_id) const {
  auto it = pending_opens_.find(tab_id);
  return it != pending_opens_.end() &&
         ::GetTickCount64() - it->second < kOpenRetryMs;
}

void PhiReaderBridge::Toggle(int tab_id,
                             const std::string& url,
                             const char* entry_point) {
  // On the reading surface itself the toggle can only mean "leave": ask for
  // the ORIGIN tab, which is the id the extension filed the surface under.
  if (std::optional<int> origin = OriginTabIdOfReaderPage(url)) {
    Close(*origin);
    return;
  }
  if (HasSurface(tab_id)) {
    Close(tab_id);
    return;
  }
  Open(tab_id, entry_point);
}

void PhiReaderBridge::Open(int tab_id, const char* entry_point) {
  // Swallow a repeat press while an open is still extracting — a second
  // request would spawn a duplicate surface tab.
  const unsigned long long now = ::GetTickCount64();
  auto pending = pending_opens_.find(tab_id);
  if (pending != pending_opens_.end() && now - pending->second < kOpenRetryMs) {
    return;
  }
  pending_opens_[tab_id] = now;
  pending_entry_points_[tab_id] = entry_point ? entry_point : "unknown";
  if (!Broadcast(kMsgOpen, TabPayload(tab_id))) {
    pending_opens_.erase(tab_id);
    ShowRefusalToast(tab_id, "transport_unavailable");
    owner_->CaptureAnalytics(
        kAnalyticsModuleReader, kEventReaderOpenFailed,
        PhiReaderOpenFailedProperties(TakeEntryPoint(tab_id),
                                      "transport_unavailable"));
  }
}

const char* PhiReaderBridge::TakeEntryPoint(int tab_id) {
  auto it = pending_entry_points_.find(tab_id);
  if (it == pending_entry_points_.end()) {
    return "unknown";
  }
  const char* entry_point = it->second;
  pending_entry_points_.erase(it);
  return entry_point;
}

void PhiReaderBridge::Close(int tab_id) {
  pending_opens_.erase(tab_id);
  pending_entry_points_.erase(tab_id);
  Broadcast(kMsgClose, TabPayload(tab_id));
}

std::string PhiReaderBridge::TabPayload(int tab_id, const std::string& extra) {
  std::string payload = "{\"tabId\":" + base::NumberToString(tab_id);
  // A broadcast naming a window is delivered to THAT window's profile rather
  // than the last-active one, which is what keeps a reader opened in a
  // background Space from being handed to the wrong profile's extension.
  // Unresolvable (a tab in no visible strip) falls back to the active window,
  // which is the Mac's only routing.
  if (PhiShellWindow* window = owner_->WindowShowingTab(TabId{tab_id})) {
    // The browser, never the slot: a slot whose opening browser has closed
    // would name a window the extension cannot find.
    const BrowserId browser = window->active_browser_id();
    if (browser.valid()) {
      payload += ",\"windowId\":" + base::NumberToString(browser.value);
    }
  }
  return payload + extra + "}";
}

bool PhiReaderBridge::StartExtraction(int tab_id,
                                      int pending_reply_id,
                                      bool settle) {
  const std::string request_id =
      "phi-extract-" + base::NumberToString(next_extract_id_++);
  const std::string payload =
      TabPayload(tab_id, ",\"requestId\":\"" + request_id +
                             "\",\"settle\":" + (settle ? "true" : "false"));
  if (!Broadcast(kMsgExtract, payload)) {
    return false;
  }
  pending_extractions_[request_id] = pending_reply_id;
  return true;
}

void PhiReaderBridge::BroadcastStyle() {
  Broadcast(kMsgStyleChanged, style_.ToJson());
}

bool PhiReaderBridge::Broadcast(const std::string& type,
                                const std::string& payload) {
  return owner_->BroadcastToExtensions(type, payload);
}

void PhiReaderBridge::ShowRefusalToast(int tab_id, const std::string& error) {
  PhiShellWindow* window = owner_->WindowShowingTab(TabId{tab_id});
  if (!window) {
    return;
  }
  const wchar_t* title =
      (error == "no_article_detected" || error == "below_coverage_floor")
          ? PhiStr("browser.readerView.noArticleFound")
          : PhiStr("browser.readerView.unavailable");
  PhiShowToast(window->shell_hwnd(), window->CurrentPalette(), title);
}

}  // namespace phi_client

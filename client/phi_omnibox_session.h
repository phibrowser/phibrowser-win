// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_OMNIBOX_SESSION_H_
#define PHIBROWSER_WIN_CLIENT_PHI_OMNIBOX_SESSION_H_

#include <string>
#include <string_view>
#include <memory>
#include <vector>

#include "phibrowser-win/client/phi_sidebar_layout.h"

// The address field and everything that hangs off it, as ONE component of
// the shell window (docs/architecture-review.md §3.5, lane 1 cluster F): the
// EDIT child and its subclass, the suggestion dropdown and its window, the
// results model, the navigation state the toolbar draws, and the commit
// paths. The shell forwards messages here and asks this for what it paints;
// nothing else touches the field, the popup or the nav state.
//
// The top half of this header is PURE — plain ints and SidebarRect, no Win32
// — so the geometry and the selection arithmetic are tested on the Linux
// host (test/model/omnibox_session_test.cc). A host translation unit defines
// PHI_OMNIBOX_SESSION_MODEL_ONLY before including it and gets that half
// alone; the client build sees the whole file.

namespace phi_client {

// The address capsule as the session needs it: the box itself and every
// accessory that can sit INSIDE it. Which accessories exist varies by layout
// mode and by page (the reader button comes and goes), so the session reads
// them rather than assuming a set.
struct PhiOmniboxCapsule {
  SidebarRect field;            // The painted pill.
  SidebarRect page_icon;        // Comfortable's leading site chip.
  SidebarRect field_ext_strip;  // Pinned-extension icons.
  SidebarRect reader;
  SidebarRect copy_link;  // The Mac's hover-only pair: Copy link …
  SidebarRect site_menu;  // … and the site menu.
  SidebarRect extensions;
};

// The EDIT's rect inside `capsule`: a `text_height`-tall line centred in the
// box, starting after the page icon (plus `icon_gap`) or at `leading_inset`
// without one, and stopping `trailing_gap` before the LEFTMOST accessory
// that actually sits inside the capsule — the EDIT must not extend under the
// buttons or it eats their clicks. `leading_extra` is the room the keyword
// chip takes ahead of the text and `trailing_extra` what the keyword hint
// takes behind it (0 without them). All arguments are already in pixels.
inline SidebarRect PhiOmniboxTextRect(const PhiOmniboxCapsule& capsule,
                                      int text_height,
                                      int leading_inset,
                                      int icon_gap,
                                      int trailing_gap,
                                      int leading_extra = 0,
                                      int trailing_extra = 0) {
  const SidebarRect& box = capsule.field;
  // SideAddressBar: the text stops at the LEFTMOST accessory that sits
  // inside the capsule. Rects outside it (the comfortable layout keeps its
  // extensions button in the top bar) are not candidates.
  int right_stop = box.right;
  for (const SidebarRect& accessory :
       {capsule.field_ext_strip, capsule.reader, capsule.copy_link,
        capsule.site_menu, capsule.extensions}) {
    if (accessory.right > accessory.left && accessory.left > box.left &&
        accessory.left < right_stop) {
      right_stop = accessory.left;
    }
  }
  const int text_left = (capsule.page_icon.right > capsule.page_icon.left
                             ? capsule.page_icon.right + icon_gap
                             : box.left + leading_inset) +
                        leading_extra;
  const int width = right_stop - trailing_gap - trailing_extra - text_left;
  // A single-line EDIT draws its text at the top of its own rect, so filling
  // the whole box would leave the text hugging the upper edge.
  const int top = box.top + (box.bottom - box.top - text_height) / 2;
  return SidebarRect{text_left, top, text_left + (width > 0 ? width : 1),
                     top + text_height};
}

// The dropdown's height for `rows` rows of `row_height`, plus its padding.
inline int PhiOmniboxPopupHeight(int rows, int row_height, int padding) {
  return rows * row_height + padding;
}

// The row at client-y `y` in a dropdown of `rows`, or -1 outside them.
inline int PhiOmniboxRowAtY(int y, int rows, int row_height, int top_pad) {
  // Truncating division, so the top pad belongs to the first row rather than
  // to nothing: a press on the popup's own border still picks a suggestion.
  const int row = (y - top_pad) / (row_height > 0 ? row_height : 1);
  return row >= 0 && row < rows ? row : -1;
}

// Arrowing: `delta` rows from `selected` (-1 = the field's own text), with
// both ends wrapping THROUGH the field — up from it lands on the last row,
// down past the last returns to it.
inline int PhiOmniboxNextSelection(int selected, int delta, int rows) {
  int next = selected + delta;
  if (next < -1) {
    next = rows - 1;  // Up from the field wraps to the last row.
  } else if (next >= rows) {
    next = -1;  // Down past the last row returns to the field's own text.
  }
  return next;
}

// What Enter commits: the typed text with its surrounding blanks removed.
inline std::wstring PhiOmniboxTrimTyped(const std::wstring& typed) {
  const size_t first = typed.find_first_not_of(L" \t");
  const size_t last = typed.find_last_not_of(L" \t");
  return first == std::wstring::npos ? std::wstring()
                                     : typed.substr(first, last - first + 1);
}

// Chrome's StripJavascriptSchemas: skips leading blanks and control
// characters and any number of "javascript:" prefixes (ASCII case-blind);
// when at least one was found, the text from the first character after the
// last of them. A pasted bookmarklet must never run from the field.
inline std::wstring PhiOmniboxStripJavascript(const std::wstring& text) {
  constexpr std::wstring_view kScheme = L"javascript:";
  constexpr size_t kSchemeLength = kScheme.size();
  const auto is_scheme_at = [&](size_t at) {
    if (text.size() - at < kSchemeLength) {
      return false;
    }
    for (size_t k = 0; k < kSchemeLength; ++k) {
      wchar_t c = text[at + k];
      if (c >= L'A' && c <= L'Z') {
        c = static_cast<wchar_t>(c - L'A' + L'a');
      }
      if (c != kScheme[k]) {
        return false;
      }
    }
    return true;
  };
  bool found = false;
  size_t i = 0;
  while (i < text.size()) {
    const wchar_t c = text[i];
    if (c <= 0x20 || c == 0x7F || c == 0xA0 || c == 0x3000 ||
        (c >= 0x2000 && c <= 0x200B) || c == 0xFEFF) {
      ++i;  // Blank or control character.
    } else if (is_scheme_at(i)) {
      found = true;
      i += kSchemeLength;
    } else {
      break;
    }
  }
  return found ? text.substr(i) : text;
}

// Chrome's OmniboxView::SanitizeTextForPaste: leading and trailing blanks
// go; a blank run holding a line break is removed when the text looks like
// a URL wrapped across lines (no other inner blanks, and a '.' or ':' in
// it), else it collapses to one space; blank runs without a line break are
// kept. All-blank text becomes one space. Then the javascript: schemes are
// stripped.
inline std::wstring PhiOmniboxSanitizePaste(const std::wstring& text) {
  static constexpr wchar_t kBlank[] = L" \t\r\n\v\f";
  size_t end = text.find_first_not_of(kBlank);
  if (end == std::wstring::npos) {
    return text.empty() ? std::wstring() : std::wstring(L" ");
  }
  const size_t last = text.find_last_not_of(kBlank);
  const std::wstring body = text.substr(end, last - end + 1);
  // Does it read as one URL broken over lines?
  bool plain_inner_blank = false;
  for (size_t at = body.find_first_of(kBlank); at != std::wstring::npos;) {
    const size_t stop = body.find_first_not_of(kBlank, at);
    const size_t line = body.find_first_of(L"\r\n", at);
    if (line == std::wstring::npos || line >= stop) {
      plain_inner_blank = true;
    }
    at = body.find_first_of(kBlank, stop);
  }
  const bool url_like = !plain_inner_blank &&
                        body.find_first_of(L".:") != std::wstring::npos;
  std::wstring result;
  result.reserve(body.size());
  end = 0;
  for (;;) {
    const size_t begin = end;
    end = body.find_first_of(kBlank, begin);
    result += body.substr(begin, end == std::wstring::npos
                                     ? std::wstring::npos
                                     : end - begin);
    if (end == std::wstring::npos) {
      break;
    }
    const size_t run = end;
    end = body.find_first_not_of(kBlank, run);  // Never npos: body is trimmed.
    const size_t line = body.find_first_of(L"\r\n", run);
    if (line == std::wstring::npos || line >= end) {
      result += body.substr(run, end - run);
    } else if (!url_like) {
      result += L' ';
    }
  }
  return PhiOmniboxStripJavascript(result);
}

// ASCII case folding — keywords are hostnames, so this is all that matching
// one needs, and it keeps the header free of locale state.
inline std::wstring PhiOmniboxFoldAscii(std::wstring text) {
  for (wchar_t& c : text) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
  }
  return text;
}

// One autocomplete row as the framework's OnOmniboxResults json carries it.
struct PhiOmniboxRow {
  int line = 0;
  std::string type;
  std::wstring contents;
  std::wstring description;
  std::wstring fill;  // fillIntoEdit — what arrowing onto the row types.
  std::wstring url;   // The destination; EMPTY on an engine-selection hint.
  // The engine behind a search-other-engine row — its keyword ("bing.com")
  // and display name ("Microsoft Bing"); empty on every other row.
  std::wstring keyword;
  std::wstring engine;
  bool has_tab_match = false;
  // "inline": what the default row appends to the typed text, the part the
  // field shows selected after the caret (the Mac's inlineCompletionString).
  std::wstring inline_completion;
  // "isDefault": Chromium allows the row as the default match — the row
  // Enter opens unless the arrows picked another.
  bool is_default = false;
  bool deletable = false;  // Its provider can forget it (history, …).
  // "swap": the row reads description first (Chromium's
  // swap_contents_and_description).
  bool swap = false;
};

// The row belongs in the dropdown: a pedal (Chromium's "share this tab"
// commands) has nothing the client can run, and a row with no text at all
// has nothing to show (the Mac's isSupportedType / isEmpty filter).
inline bool PhiOmniboxKeepsRow(const PhiOmniboxRow& row) {
  return row.type != "pedal" &&
         !(row.contents.empty() && row.description.empty());
}

// The Mac's phiBrandEnsuredUrlString: Chromium's own pages show as phi://
// (the fork rewrites phi:// back on navigation).
inline std::wstring PhiOmniboxBrandUrl(const std::wstring& text) {
  constexpr std::wstring_view kChrome = L"chrome://";
  if (text.compare(0, kChrome.size(), kChrome) != 0) {
    return text;
  }
  return L"phi://" + text.substr(kChrome.size());
}

// The Mac's normalizedForURLComparison: no scheme, lower case, no "www.",
// no trailing slashes — "google.com/" and "https://www.google.com" match.
inline std::wstring PhiOmniboxUrlKey(const std::wstring& text) {
  const size_t scheme = text.find(L"://");
  std::wstring key = PhiOmniboxFoldAscii(
      scheme == std::wstring::npos ? text : text.substr(scheme + 3));
  if (key.compare(0, 4, L"www.") == 0) {
    key.erase(0, 4);
  }
  while (!key.empty() && key.back() == L'/') {
    key.pop_back();
  }
  return key;
}

// The inline completion a row set offers `user_text`: the default row's,
// when Chromium armed one and the typed text does not already spell the
// row out (the Mac's guard against completing "google.com/" again).
inline std::wstring PhiOmniboxInlineFor(const std::vector<PhiOmniboxRow>& rows,
                                        const std::wstring& user_text) {
  if (rows.empty() || !rows.front().is_default ||
      rows.front().inline_completion.empty() || user_text.empty()) {
    return std::wstring();
  }
  if (PhiOmniboxUrlKey(user_text) == PhiOmniboxUrlKey(rows.front().fill)) {
    return std::wstring();
  }
  return rows.front().inline_completion;
}

// The field may take (or drop) an inline completion now: it still shows
// exactly what the user typed with the caret at its end, or the completion
// applied last time with that completion still selected. Anything else —
// the caret moved, the selection changed, an arrowed row's text — means the
// user has taken the text over, and a late row set must not rewrite it.
inline bool PhiOmniboxCanInline(const std::wstring& field_text,
                                size_t sel_start,
                                size_t sel_end,
                                const std::wstring& user_text,
                                const std::wstring& shown_inline) {
  if (field_text == user_text) {
    return sel_start == user_text.size() && sel_end == user_text.size();
  }
  return !shown_inline.empty() && field_text == user_text + shown_inline &&
         sel_start == user_text.size() && sel_end == field_text.size();
}

// The row armed once a fresh row set lands (the Mac's handleSearchResults):
// a row the user ARROWED to stays armed across the streamed updates of one
// query — by its engine when it named one, else by its place — and
// otherwise the default row is armed when Chromium allows one; -1 = none,
// Enter commits the text.
inline int PhiOmniboxArmedRow(const std::vector<PhiOmniboxRow>& rows,
                              int previous,
                              const std::wstring& previous_keyword,
                              bool arrowed) {
  if (arrowed && !previous_keyword.empty()) {
    for (size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].keyword == previous_keyword) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }
  if (arrowed && previous >= 0 && previous < static_cast<int>(rows.size())) {
    return previous;
  }
  return !rows.empty() && rows.front().is_default ? 0 : -1;
}

// Where a committed row opens (the Mac's suggestionDisposition): its open
// tab while Ctrl is held on a row that has one ("Hold Ctrl to switch"),
// a fresh tab in the omnibox new-tab mode, else the current tab — never an
// automatic switch.
enum class PhiOmniboxOpen { kCurrentTab, kNewTab, kSwitchToTab };
inline PhiOmniboxOpen PhiOmniboxOpenFor(const PhiOmniboxRow& row,
                                        bool ctrl,
                                        bool new_tab) {
  if (row.has_tab_match && ctrl) {
    return PhiOmniboxOpen::kSwitchToTab;
  }
  return new_tab ? PhiOmniboxOpen::kNewTab : PhiOmniboxOpen::kCurrentTab;
}

// A brand colour (0xRRGGBB) for the keyword chip of the engines Chromium
// seeds, matched by keyword — never by the user-editable name; -1 keeps the
// accent (the Mac's OmniBoxKeywordSearchEngine.brandColor).
inline int PhiOmniboxKeywordBrand(const std::wstring& keyword) {
  const std::wstring key = PhiOmniboxFoldAscii(keyword);
  if (key == L"x.com" || key == L"twitter.com" || key == L"wikipedia.org") {
    return 0x000000;
  }
  if (key == L"spotify.com") return 0x1DB954;
  if (key == L"reddit.com") return 0xFF4500;
  if (key == L"youtube.com") return 0xFF0000;
  if (key == L"github.com") return 0x24292F;
  if (key == L"gmail.com") return 0xEA4335;
  if (key == L"docs.google.com" || key == L"calendar.google.com") {
    return 0x4285F4;
  }
  if (key == L"drive.google.com") return 0x34A853;
  return -1;
}

// The row offers keyword search for `typed` (the Mac's
// canUseKeywordSearchSuggestion): it names an engine, and either the row set
// answers exactly this text (`results_current`) or — while a fresh set is
// still on its way — it is the engine-selection hint whose keyword still
// starts with what is typed. A row with a destination is never trusted
// across a text change: its query would be stale.
inline bool PhiOmniboxRowOffersKeyword(const PhiOmniboxRow& row,
                                       const std::wstring& typed,
                                       bool results_current) {
  if (row.engine.empty() || row.keyword.empty()) {
    return false;
  }
  const std::wstring input = PhiOmniboxFoldAscii(PhiOmniboxTrimTyped(typed));
  if (input.empty()) {
    return false;
  }
  if (results_current) {
    return true;
  }
  if (!row.url.empty()) {
    return false;
  }
  const std::wstring keyword = PhiOmniboxFoldAscii(row.keyword);
  return input.size() <= keyword.size() &&
         keyword.compare(0, input.size(), input) == 0;
}

// The engine's SITE for a "Search X" row that carries no query — the Mac's
// b0a536db: Enter or a click on that row opens the engine's home instead of
// entering keyword search (Tab still does that). `search_url` is any search
// URL the engine builds; the result keeps its scheme and authority (port
// included) with path "/", no query, no fragment. Empty unless http(s) with
// a host.
inline std::string PhiOmniboxEngineSite(const std::string& search_url) {
  const size_t scheme_end = search_url.find("://");
  if (scheme_end == std::string::npos) {
    return std::string();
  }
  const std::string scheme = search_url.substr(0, scheme_end);
  if (scheme != "http" && scheme != "https") {
    return std::string();
  }
  const size_t host_begin = scheme_end + 3;
  const size_t host_end = search_url.find_first_of("/?#", host_begin);
  const std::string authority =
      search_url.substr(host_begin, host_end == std::string::npos
                                        ? std::string::npos
                                        : host_end - host_begin);
  if (authority.empty() || authority[0] == ':' || authority[0] == '@') {
    return std::string();
  }
  return scheme + "://" + authority + "/";
}

// The row whose engine the keyword HINT names ("Tab to search X") and Tab
// picks: the armed row when it offers one, else the first row that does;
// -1 for none.
inline int PhiOmniboxKeywordHintRow(const std::vector<PhiOmniboxRow>& rows,
                                    int selected,
                                    const std::wstring& typed,
                                    bool results_current) {
  if (selected >= 0 && selected < static_cast<int>(rows.size()) &&
      PhiOmniboxRowOffersKeyword(rows[static_cast<size_t>(selected)], typed,
                                 results_current)) {
    return selected;
  }
  for (size_t i = 0; i < rows.size(); ++i) {
    if (PhiOmniboxRowOffersKeyword(rows[i], typed, results_current)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// The row SPACE picks, or -1. Space is narrower than Tab (the Mac's
// canAcceptKeywordSearchWithSpace): the user must have ARMED an engine row —
// the hint alone is a Tab affordance, so "bing maps" typed at speed still
// searches the default engine — or typed the whole keyword itself (Chrome's
// own rule: "bing.com" + Space). Off entirely when the profile's "Space
// enters keyword search" setting is.
inline int PhiOmniboxSpaceKeywordRow(const std::vector<PhiOmniboxRow>& rows,
                                     int selected,
                                     const std::wstring& typed,
                                     bool results_current,
                                     bool space_shortcut) {
  if (!space_shortcut) {
    return -1;
  }
  if (selected >= 0 && selected < static_cast<int>(rows.size()) &&
      PhiOmniboxRowOffersKeyword(rows[static_cast<size_t>(selected)], typed,
                                 results_current)) {
    return selected;
  }
  const std::wstring input = PhiOmniboxFoldAscii(PhiOmniboxTrimTyped(typed));
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].url.empty() && !rows[i].keyword.empty() &&
        PhiOmniboxFoldAscii(rows[i].keyword) == input &&
        PhiOmniboxRowOffersKeyword(rows[i], typed, results_current)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// True when the page's url must show as the PLACEHOLDER rather than itself —
// the new-tab page, whose chrome:// url the user never typed (the Mac's
// updateDisplayedURL isNTP blank).
inline bool PhiOmniboxHidesUrl(const std::string& url) {
  return url.rfind("chrome://newtab", 0) == 0;
}

}  // namespace phi_client

#if !defined(PHI_OMNIBOX_SESSION_MODEL_ONLY)

#include <windows.h>

#include "base/memory/raw_ptr_exclusion.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_shell_layout.h"
#include "phibrowser-win/client/phi_theme.h"

namespace phi_client {

// What the session needs from the shell window that owns it: geometry, the
// chrome's fonts and palette, the browser its text acts on, and the two
// hand-backs (focus to the page, the reader overlay out of the way) a commit
// ends with. A narrow interface rather than `friend`: the shell implements it
// privately, so none of this widens its public surface. Every name carries
// the component's prefix — the shell implements one such interface per
// component and they must not collide.
class PhiOmniboxShell {
 public:
  // The window the field, the popup and every repaint belong to.
  virtual HWND OmniboxShellWindow() const = 0;
  // The shell is being torn down: the field's remaining messages must not
  // reach the framework for a window that is going.
  virtual bool OmniboxShellDestroying() const = 0;
  // Design units → this monitor's pixels.
  virtual int OmniboxScaled(int design_units) const = 0;
  virtual HFONT OmniboxTextFont() const = 0;
  virtual HFONT OmniboxIconFont() const = 0;
  // The keyword chip's label face (the Space chip's 13/600) and the keyword
  // hint's (the 11 small face).
  virtual HFONT OmniboxChipFont() const = 0;
  virtual HFONT OmniboxHintFont() const = 0;
  virtual PhiPalette OmniboxPalette() const = 0;
  // The palette of the row the field sits in: OmniboxPalette, or the page's
  // own colour when the address row follows the page (the Mac's
  // WebContentHeader page colour). The EDIT paints with it; the suggestion
  // popup and menus stay on OmniboxPalette.
  virtual PhiPalette OmniboxFieldPalette() const = 0;
  virtual PhiLayoutMode OmniboxLayoutMode() const = 0;
  // This turn's capsule geometry (the shell's cached layout).
  virtual PhiOmniboxCapsule OmniboxCapsule() = 0;

  virtual phi_framework::PhiFramework* OmniboxFramework() const = 0;
  // The browser the slot shows — where the suggestion query runs and a
  // new tab is born.
  virtual BrowserId OmniboxBrowser() const = 0;
  // The page the field DESCRIBES and a current-tab commit navigates: the
  // browser of the Peek presented over the active tab, else OmniboxBrowser()
  // (the Mac's pageCommandTarget seeding the omnibox, PHI-1745). A peek's
  // page is never the strip-active tab, so a commit sent to OmniboxBrowser()
  // would load behind the card.
  virtual BrowserId OmniboxPageBrowser() const = 0;
  // That browser is in placeholder mode: its strip is EMPTY, so a commit
  // must birth the tab before it navigates.
  virtual bool OmniboxBrowserPlaceholding() const = 0;
  // "New tab behavior = Omnibox": Ctrl+T focuses the field instead of
  // opening a page.
  virtual bool OmniboxNewTabFocusesField() const = 0;
  // Keys belong to the page again.
  virtual void OmniboxFocusContent() = 0;
  // The pointer pressed the field (or the native page's): a chrome press
  // for whatever the shell dismisses on one (the extension popup).
  virtual void OmniboxFieldPressed() = 0;
  // A typed URL replaces the page the reader is covering.
  virtual void OmniboxLeaveReader() = 0;

  // The native new-tab page's own omnibox, which joins this session
  // (phi_new_tab_page.cc calls AttachField on it), or null with no page up.
  virtual HWND OmniboxNativePageField() const = 0;
  // That page's box in screen coords — the dropdown grows out of it.
  virtual bool OmniboxNativePageBox(RECT* out) const = 0;
  // A commit is about to land while the native page is up: it blanks its
  // controls now (omniBoxDidClear) so nothing of it lingers over the
  // arriving page.
  virtual void OmniboxNativePageCommitting() = 0;

 protected:
  // Non-virtual and protected: the shell OWNS the session, never the other
  // way round, so nothing ever deletes through this pointer.
  ~PhiOmniboxShell() = default;
};

class PhiOmniboxSession {
 public:
  // Reverts the copy-link checkmark after its second. The shell owns the
  // timer space, so its WM_TIMER routes this id back here.
  static constexpr UINT_PTR kCopyLinkTimer = 0x523;

  explicit PhiOmniboxSession(PhiOmniboxShell* shell);
  PhiOmniboxSession(const PhiOmniboxSession&) = delete;
  PhiOmniboxSession& operator=(const PhiOmniboxSession&) = delete;
  ~PhiOmniboxSession();

  // --- The address field ---------------------------------------------------
  // A real Win32 EDIT child, so typing, selection, clipboard, IME and the
  // caret all behave natively (Chromium's own omnibox is hollowed away under
  // IS_WIN_PHI_CLIENT, and hand-rolling text editing would reproduce it
  // badly). Created once the shell window exists.
  void EnsureField();
  // Joins another EDIT to this session — the native new-tab page's omnibox
  // gets the same subclass (Enter / Escape / arrows, the dropdown) and
  // reports its EN_CHANGE here.
  void AttachField(HWND edit);
  // By const REFERENCE, so the shell can publish a view of it (PhiMenuBar
  // still reads the field by member name — see phi_shell_window.h).
  const HWND& field() const { return field_; }
  // The session's field, or the native page's, whichever holds keyboard
  // focus — the query, the dropdown and the arrow fills all act on this one.
  HWND FocusedField() const;
  // The address field itself has focus (the native page's does not count).
  bool FieldHasFocus() const;
  // The shell's UI font changed (a DPI move).
  void SetFont(HFONT font);
  // Re-seats the EDIT inside this turn's capsule. Idempotent: the paint path
  // calls it too, and moving on no change keeps it from feeding itself more
  // paints.
  void Position();
  void SetFieldVisible(bool visible);
  // Pushes the current page's URL into the field. Skipped while the user is
  // typing unless `force` (the Escape-reverts path).
  void Sync(bool force);
  // Navigates to whatever is typed in `edit` — the address field or the
  // native page's omnibox — then hands focus back to the page.
  void Commit(HWND edit);
  // Pastes the clipboard into `edit` through PhiOmniboxSanitizePaste.
  void PasteInto(HWND edit);
  // The field's right-click menu (windows-native-ui.md §3.5): the Fluent
  // flyout with the EDIT's own verbs and "Paste and go" at the top, which
  // pastes the clipboard's text over the field and commits it.
  void ShowFieldMenu(HWND edit, POINT screen);
  void CommitField();  // Commit(field_).
  // Focuses the field and selects it whole, when it is on screen; false when
  // there is no visible field to focus (placeholder mode's entry point).
  bool FocusFieldIfVisible();
  // The New Tab entry points: in omnibox mode this focuses the field and the
  // tab is born on commit; in page mode it opens a tab now.
  void StartNewTab();
  // The EDIT only repaints on its own events, so a palette change (Space
  // swap ramp, instant switch) must invalidate it or it keeps the old wash.
  void Repaint();
  // WM_CTLCOLOREDIT for `edit`: the palette's brush, or null when `edit` is
  // not this session's field.
  HBRUSH OnCtlColorEdit(HDC edit_dc, HWND edit);
  // WM_COMMAND from the shell: true when it was this field's EN_CHANGE.
  bool OnShellCommand(WPARAM wparam, LPARAM lparam);
  // EN_CHANGE while the user types: run/refresh the autocomplete query.
  void OnFieldChanged(HWND edit);

  // --- Keyword search (the Mac's search-engine badge) ----------------------
  // Typing an engine's keyword offers it (the hint at the field's trailing
  // end, the "Search X" row); Tab, Space on the armed row, or opening that
  // row turns it into a CHIP ahead of the text, and the text is then the
  // query that engine searches on Enter. Backspace on an empty query takes
  // the chip off and puts the keyword back; Escape takes it off and clears.
  // The chip is up: the field's text is the query for `keyword_engine()`.
  bool keyword_mode() const { return !keyword_keyword_.empty(); }
  const std::wstring& keyword_engine() const { return keyword_engine_; }
  // Room the chip takes ahead of the text and the hint behind it, in pixels
  // (0 without them) — the shell's capture path indents its own copy of the
  // text by the same.
  int KeywordLeadingAdvance() const;
  int KeywordTrailingAdvance() const;
  // Paints the chip and the hint into the capsule on the shell's `dc`, after
  // the field's fill.
  void PaintKeyword(HDC dc);

  // --- Navigation state ----------------------------------------------------
  // Re-reads the toolbar's navigation state from the snapshot of
  // `page_browser` — the page the field describes (OmniboxPageBrowser); null
  // `nav` = no browser, which clears it. A Peek presented or closed under a
  // focused field retargets it: the edit is dismissed (re-seeded, keys back
  // to the page) so Enter cannot land on the page behind the vanished card.
  void RebuildNavState(const phi_framework::NavState* nav,
                       BrowserId page_browser);
  // Const references for the same reason as field().
  const std::wstring& nav_url() const { return nav_url_; }
  const bool& can_go_back() const { return can_go_back_; }
  const bool& can_go_forward() const { return can_go_forward_; }
  const bool& nav_loading() const { return nav_loading_; }
  // What the field SHOWS — the typed text mid-edit, the page's url at rest.
  // The capture path paints this, because it renders without children.
  std::wstring DisplayedUrl() const;
  // The capsule's hover brighten (SideAddressBar hoveredColor: resting 4%
  // wash → emphasized 8%). Fed from BOTH sides — the shell's mouse moves for
  // the capsule's rim, the EDIT subclass for the text area the child covers.
  bool field_hover() const { return field_hover_; }
  void SetFieldHover(bool hovered);
  // The in-field copy-link button: the current URL to the clipboard, the
  // glyph swapped to a checkmark for a second (the Mac copyURLButton's
  // triggeredSystemName/triggeredRevertDelay).
  bool copy_link_flash() const { return copy_link_flash_; }
  void CopyLinkToClipboard();
  void RevertCopyLinkFlash();

  // --- Suggestion dropdown -------------------------------------------------
  // Autocomplete results for `browser_id`'s running query (the framework's
  // json — the one JSON site that stays, by design). A result for any other
  // browser, or one arriving after the user stopped typing, is stale.
  void OnResults(BrowserId browser_id, const std::string& json);
  void ClosePopup(bool stop_query);
  // Called by the dropdown's own window procedure.
  void PaintPopup(HWND popup, HDC dc);
  void PopupHover(int x, int y);  // y < 0: the pointer left the list.
  // A press at (x, y): the row's × forgets it, anywhere else opens it.
  void PopupClick(int x, int y);

 private:
  // Rows mirror the framework's OnOmniboxResults json; the popup is a
  // non-activating WS_POPUP under the address field, so the EDIT keeps focus
  // and its caret while the list is up.
  using Row = PhiOmniboxRow;

  void ShowPopup();  // Creates/positions the popup under the focused field.
  // Rows gone and the window hidden; the running query and the new-tab
  // gesture untouched (ClosePopup decides those).
  void HidePopup();
  void MoveSelection(int delta);
  void CommitSelection();  // CommitRow(selected_, Ctrl held).
  // Opens rows_[index]; `ctrl` switches to the row's open tab when it has
  // one (PhiOmniboxOpenFor).
  void CommitRow(int index, bool ctrl);
  // Shift+Delete or the row's ×: Chromium forgets a deletable row and the
  // running query reports the shorter set.
  void DeleteRow(int index);
  int RowAtY(int y) const;
  // The × of rows_[row] in popup client coords; empty unless the row is
  // deletable and lit (hovered or armed), the Mac's delete button.
  RECT RowDeleteRect(int row) const;
  // Inline autocompletion (the Mac's OmniBoxTextField): after a row set
  // lands, the default row's completion follows what the user typed,
  // selected, so the next keystroke replaces it — unless the user has since
  // taken the text over (PhiOmniboxCanInline).
  void ApplyInline(HWND edit);
  // Puts what the user typed back in `edit`, caret at its end: arrowing
  // back up to the field, or Escape on an inline or arrowed text.
  void RestoreUserText(HWND edit);
  // Sets `edit`'s text without it counting as typing; selects [start, end).
  void SetFieldText(HWND edit, const std::wstring& text, size_t start,
                    size_t end);
  int Scaled(int design_units) const;
  bool PaintField(HWND field);

  // Enters keyword search on rows_[row] (an engine row): the chip goes up,
  // the row's query (if it had one) becomes the text. False when the row
  // names no engine or the chip is already up.
  bool AcceptKeyword(int row);
  // Takes the chip down and puts `text` in the field. `requery` lets the
  // EDIT's change notification run autocomplete on it (Backspace puts the
  // keyword back and its hint returns); a silent exit is for the commit and
  // the focus-loss paths.
  void ExitKeyword(const std::wstring& text, bool requery);
  // Enter with the chip up: the engine's search URL for the text.
  void CommitKeyword(HWND edit);
  // Enter or a click on a "Search X" row with no query: the engine's site
  // (PhiOmniboxEngineSite), the Mac's b0a536db. Beeps when the engine cannot
  // name one.
  void OpenEngineSite(const PhiOmniboxRow& row);
  // The commit shared by the chip and the engine row: births the pending
  // new tab, leaves Reader, navigates, drops the chip and refocuses content.
  void NavigateCommitted(phi_framework::PhiFramework* framework,
                         BrowserId browser,
                         const std::string& url);
  // The browser a CURRENT-TAB commit navigates: the page the field describes
  // (OmniboxPageBrowser — a presented Peek's, else the window's). Invalid,
  // with the edit dismissed, when the field was aimed at a Peek that has
  // since closed (page JS window.close(), teardown) and the dismissal has not
  // caught up: the submission is dropped rather than landing on the opener
  // behind the vanished card (the Mac's navigateCurrentTab guard).
  BrowserId CurrentTabTarget();
  // Recomputes the hint from the rows, the armed row and the typed text;
  // re-seats the EDIT and repaints the capsule when it changed.
  void UpdateKeywordHint();
  bool SpaceShortcutEnabled() const;
  // The width of `text` in `font`, in pixels, on the shell's DC.
  int MeasureText(HFONT font, const std::wstring& text) const;
  // The capsule and its antialiased rim repaint.
  void InvalidateCapsule();
  // The rows answer this typed text (the framework echoes the query).
  bool ResultsCurrent(const std::wstring& typed) const;

  // Subclass proc: Enter commits, Escape reverts, focus selects all.
  static LRESULT CALLBACK FieldProc(HWND hwnd,
                                    UINT msg,
                                    WPARAM wparam,
                                    LPARAM lparam);

  RAW_PTR_EXCLUSION PhiOmniboxShell* const shell_;
  // The palette the field menu's owner-draw reads while the menu is up.
  PhiPalette menu_palette_;

  HWND field_ = nullptr;
  std::unique_ptr<class PhiLayerSurface> field_surface_;
  // The EDIT paints itself in SYSTEM colours, which clash under a dark
  // theme. WM_CTLCOLOREDIT hands it the palette instead; the brush must
  // outlive the message, so it is cached and rebuilt only when the colour
  // changes.
  HBRUSH brush_ = nullptr;
  COLORREF brush_color_ = CLR_INVALID;

  // Navigation state for the client toolbar, refreshed with the tab list.
  std::wstring nav_url_;
  // The browser nav_url_ describes (RebuildNavState's page_browser), and
  // whether that was a presented Peek rather than the window's own browser.
  BrowserId nav_browser_;
  bool nav_peek_ = false;
  // The window's own browser at that rebuild: a peek transition counts only
  // within one window (a Space switch is not one).
  BrowserId nav_window_;
  bool can_go_back_ = false;
  bool can_go_forward_ = false;
  bool nav_loading_ = false;
  bool copy_link_flash_ = false;
  bool field_hover_ = false;

  std::vector<Row> rows_;
  std::wstring results_input_;  // The typed text rows_ answers.
  int selected_ = -1;  // -1 = nothing armed; Enter commits the text.
  // The arrows picked selected_ (rather than the default row being armed
  // when the rows landed): it survives the query's streamed updates.
  bool arrowed_ = false;
  // What the user typed, without an inline completion or an arrowed row's
  // fill (the Mac's originalText) — what the query ran on.
  std::wstring user_text_;
  // The inline completion the field shows selected after user_text_.
  std::wstring inline_shown_;
  // The last edit removed text (Backspace, Delete, Cut): the query that
  // follows asks for no inline completion, or the deleted text would come
  // straight back.
  bool last_edit_deleted_ = false;
  // The row under the pointer — a lighter wash only. Only the arrows set
  // selected_, so hovering never changes what Enter commits.
  int hovered_ = -1;
  bool hovered_delete_ = false;  // The pointer is on hovered_'s ×.
  // The last popup paint drew a history row's glyph while its favicon was
  // still being fetched: the next nav refresh repaints the list.
  bool favicon_missing_ = false;
  HWND popup_ = nullptr;
  bool programmatic_ = false;  // Suppresses EN_CHANGE feedback.
  // Set by StartNewTab in omnibox mode: the next commit opens a NEW tab.
  bool new_tab_pending_ = false;

  // The chip: the engine's keyword (empty = no chip) and display name, and
  // the pill's measured width.
  std::wstring keyword_keyword_;
  std::wstring keyword_engine_;
  int chip_width_px_ = 0;
  // The hint at the field's trailing end ("Tab to search X"), measured.
  std::wstring hint_text_;
  int hint_width_px_ = 0;
};

}  // namespace phi_client

#endif  // !defined(PHI_OMNIBOX_SESSION_MODEL_ONLY)

#endif  // PHIBROWSER_WIN_CLIENT_PHI_OMNIBOX_SESSION_H_

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_library.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>  // EM_SETCUEBANNER

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_agent_spaces.h"
#include "phibrowser-win/client/phi_animator.h"
#include "phibrowser-win/client/phi_app_menu.h"
#include "phibrowser-win/client/phi_client_analytics.h"
#include "phibrowser-win/client/phi_create_space_panel.h"
#include "phibrowser-win/client/phi_downloads_model.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_fluent.h"
#include "phibrowser-win/client/phi_folder_icons.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_mac_icons.h"
#include "phibrowser-win/client/phi_modal_scope.h"
#include "phibrowser-win/client/phi_opaque_edit.h"
#include "phibrowser-win/client/phi_panel.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_secondary_window.h"
#include "phibrowser-win/client/phi_shell_menus.h"
#include "phibrowser-win/client/phi_shell_window.h"
#include "phibrowser-win/client/phi_space_theme_panel.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_util.h"

namespace phi_client {

namespace {

// --- Geometry, in dip (LibraryOverlayController / LibraryContentView) ------
constexpr int kCardInset = 36;
constexpr int kCardRadius = 14;
constexpr int kRailWidth = 136;
constexpr int kRailItemWidth = 116;
constexpr int kRailItemHeight = 88;
constexpr int kRailItemGap = 12;
constexpr int kRailPadH = 10;
constexpr int kRailPadV = 24;
constexpr int kRailIcon = 25;
constexpr int kRailIconBox = 30;
constexpr int kRailIconToLabel = 9;
constexpr int kRailItemRadius = 12;
constexpr int kRailButton = 32;
constexpr int kRailButtonGlyph = 17;
constexpr int kRailButtonBottomPad = 16;

// LibrarySpacesView / LibrarySpaceCard / LibrarySpaceManagementController.
constexpr int kSpacesPad = 12;
constexpr int kSpaceCardWidth = 240;
constexpr int kSpaceCardGap = 22;
constexpr int kSpaceCardRadius = 18;
constexpr int kSpaceRowBottomSlack = 14;  // The Mac frame's height - 14.
constexpr int kAddCircle = 48;
constexpr int kAddGlyph = 23;
constexpr int kSpaceCardPadV = 20;
constexpr int kSpaceHeaderPadH = 20;
constexpr int kSpaceHeaderHeight = 34;
constexpr int kSpaceHeaderGap = 10;
constexpr int kSpaceIconButton = 32;
constexpr int kSpaceIconPx = 23;
constexpr int kSpacePaletteButton = 28;
constexpr int kSpacePaletteGlyph = 15;
constexpr int kSpaceHeaderToBody = 18;
constexpr int kSpaceBodyPadH = 8;
constexpr int kPinCellWidth = 64;
constexpr int kPinCellHeight = 45;
constexpr int kPinCellRadius = 8;
constexpr int kPinCellGap = 8;
constexpr int kPinColumns = 3;
constexpr int kPinFavicon = 18;
constexpr int kPinSplitFavicon = 16;
constexpr int kPinsToOutline = 8;
constexpr int kOutlineRow = 36;
constexpr int kOutlineRowContent = 30;
constexpr int kOutlineRowRadius = 8;
constexpr int kOutlineIndent = 10;
constexpr int kOutlineLeadPad = 8;
constexpr int kOutlineFavicon = 14;
constexpr int kOutlineFolderGlyph = 20;
constexpr int kOutlineChevron = 10;
constexpr int kCardFooterButton = 24;
constexpr int kCardFooterGap = 8;
constexpr int kHandleLeading = 12;
constexpr int kDragThreshold = 4;

// AllDownloadsListView / DownloadItemRow.
constexpr int kDlMaxWidth = 800;
constexpr int kDlPadH = 80;
constexpr int kDlHeaderPad = 16;
constexpr int kDlSearchHeight = 32;
constexpr int kDlListPadH = 20;
constexpr int kDlListPadBottom = 16;
constexpr int kDlGroupPadH = 10;
constexpr int kDlGroupPadTop = 16;
constexpr int kDlGroupPadBottom = 4;
constexpr int kDlGroupText = 18;
constexpr int kDlRowHeight = 56;
constexpr int kDlRowPadH = 10;
constexpr int kDlRowRadius = 10;
constexpr int kDlIcon = 30;
constexpr int kDlIconGap = 12;
constexpr int kDlActionButton = 24;
constexpr int kDlActionGap = 4;
constexpr int kDlEmptyGlyph = 36;

// Motion.
constexpr int kOpenMs = 200;
constexpr int kCloseMs = 160;
constexpr int kSlideMs = 150;

// MDL2 glyphs that fluent.h does not name.
constexpr wchar_t kGlyphStack[] = L"";  // MapLayers: square.stack.3d.
constexpr wchar_t kGlyphMove[] = L"";   // Move: the drag handle.

// hits_ keys: kind in the top byte, the card ordinal in the next, the
// index (pin, row, download, action) below. Kinds are small so the key
// stays a positive int.
enum HitKind : int {
  kHitRailSpaces = 1,
  kHitRailDownloads,
  kHitRailOpenWindow,
  kHitAddSpace,
  kHitCardIcon,
  kHitCardName,
  kHitCardPalette,
  kHitCardPin,
  kHitCardRow,
  kHitCardHandle,
  kHitCardMore,
  kHitCardBody,  // The blank body: right-click target.
  kHitDownloadRow,
  kHitDownloadAction,
};

constexpr int MakeKey(HitKind kind, int card, int index) {
  return (static_cast<int>(kind) << 24) | ((card & 0xFF) << 16) |
         (index & 0xFFFF);
}
constexpr HitKind KindOf(int key) {
  return static_cast<HitKind>((key >> 24) & 0xFF);
}
constexpr int CardOf(int key) {
  return (key >> 16) & 0xFF;
}
constexpr int IndexOf(int key) {
  return key & 0xFFFF;
}

int Width(const RECT& r) {
  return r.right - r.left;
}
int Height(const RECT& r) {
  return r.bottom - r.top;
}

const char* SectionName(PhiLibrarySection section) {
  return section == PhiLibrarySection::kSpaces ? "spaces" : "downloads";
}

// True while a Win32 popup menu is up (the menu window class, #32768): a
// tracking menu never takes the focus, so the thread hook still sees the
// shell as focused and would otherwise take the menu's Escape.
bool PhiMenuIsTracking() {
  const HWND menu = ::FindWindowW(L"#32768", nullptr);
  return menu && ::IsWindowVisible(menu);
}

// The per-window memory of the last section (the Mac's NavigationState
// lives with the window's controller; Downloads is its first value).
std::map<const PhiShellWindow*, PhiLibrarySection>& LastSections() {
  static base::NoDestructor<std::map<const PhiShellWindow*, PhiLibrarySection>>
      sections;
  return *sections;
}

// A bookmark outline row as laid out: the stored row, its depth and
// whether (a folder) is disclosed.
struct OutlineRow {
  PhiStoredRowView row;
  int depth = 0;
  bool expanded = false;
  RECT bounds{};  // Card-relative, after the body scroll.
};

// One Space card's model and layout.
struct SpaceCard {
  PhiSpaceView space;
  PhiPalette palette;  // The Space's own palette (text colours, hover).
  COLORREF ground = 0;
  std::wstring icon_text;
  std::vector<PhiStoredRowView> pins;
  std::vector<OutlineRow> rows;
  int scroll = 0;  // The body's vertical scroll, pixels.
  // Selection: a pin (index) or an outline row (guid). One per card; only
  // one card holds a selection at a time (selected_card_).
  int selected_pin = -1;
  std::string selected_guid;
  // Layout, in the view's client pixels. `bounds` is the slot; `x_offset`
  // is the reorder drag / slide displacement.
  RECT bounds{};
  int slide_from = 0;
  int slide_to = 0;
};

class LibraryView;

// What a view's host (the overlay or the standalone window) answers.
class LibraryViewHost {
 public:
  // The view wants its presentation gone (an item opened into the browser,
  // the Mac's dismissLibraryIfVisible): the overlay closes, the standalone
  // window fronts the browser instead.
  virtual void ViewRequestsDismiss() = 0;
  virtual bool ViewIsStandalone() const = 0;
  // The view's HWND is gone (the host's pointer to it is dead).
  virtual void ViewDestroyed(LibraryView* view) = 0;

 protected:
  ~LibraryViewHost() = default;
};

// ---------------------------------------------------------------------------
// The Library card: rail + section content, one opaque panel. SELF-OWNED
// through the base (DeleteWhenDestroyed): the host destroys the HWND and
// the object goes at WM_NCDESTROY — Escape and the double-click open both
// tear the view down from inside its own handlers, exactly the overview's
// shape.
class LibraryView : public PhiPanel, private PhiCreateSpaceFormHost {
 public:
  LibraryView(PhiShellWindow* shell, HWND parent, LibraryViewHost* host)
      : shell_(shell), host_(host) {
    ClassSpec spec;
    spec.name = L"PhiLibraryView";
    spec.style = CS_DBLCLKS;  // Double-click opens a row / a download.
    Options options;
    options.style = WS_CLIPCHILDREN;  // The rename and search fields.
    const RECT nothing = {0, 0, 0, 0};
    if (!CreatePanel(spec, parent, nothing, options)) {
      return;
    }
    DeleteWhenDestroyed();
    EnableTooltips(true);
    poll_ = NewTimer();
    section_ = PhiLibrarySection::kDownloads;
  }

  ~LibraryView() override = default;

  PhiShellWindow* shell() const { return shell_; }
  PhiLibrarySection section() const { return section_; }

  // Shows `section`, counting the view (the Mac's task(id: selection)).
  void SetSection(PhiLibrarySection section, bool force_event) {
    const bool changed = section != section_;
    section_ = section;
    if (changed) {
      EndRename(/*commit=*/true);
      CancelCreation();
    }
    if (changed || force_event) {
      if (WindowHost* host = shell_->window_host()) {
        host->CaptureAnalytics(
            kAnalyticsModuleLibrary, kEventLibrarySectionViewed,
            PhiLibrarySectionViewedProperties(
                SectionName(section_), host_ && host_->ViewIsStandalone()));
      }
    }
    SyncPolling();
    RebuildModel();
    Relayout();
    Invalidate();
  }

  // Positions the card. `flying` paints the ground alone — the content
  // appears once the flight lands, so a 12 % card never lays out rows.
  void SetBounds(const RECT& bounds, bool flying) {
    SyncDpiFromParent();
    SetPanelBounds(bounds);
    flying_ = flying;
    Relayout();
    // The fringe follows the panel's size by itself; it is re-cut only when
    // its fill (the card ground) moves.
    const COLORREF ground = CardGround();
    if (!fringe_fill_ || *fringe_fill_ != ground) {
      fringe_fill_ = ground;
      SetCornerFringe(Scaled(kCardRadius), 0xF, ground);
    }
    Invalidate();
  }

  void Present() {
    ShowPanel();
    TakeFocus();
    // Chromium re-focuses its web contents right after a Show: take the
    // keyboard again on the next turn, and once more a beat later.
    ::PostMessageW(hwnd(), kMsgTakeFocus, 0, 0);
    focus_retake_ = NewTimer();
    focus_retake_.Start(80);
  }

  // Gives the view the keyboard unless one of its own fields (the rename,
  // the search, the create card's name) already holds it, and never when
  // another window is in front.
  void TakeFocus() {
    if (!alive() || ::GetForegroundWindow() != ::GetAncestor(hwnd(), GA_ROOT)) {
      return;
    }
    const HWND focus = ::GetFocus();
    if (focus && (focus == hwnd() || ::IsChild(hwnd(), focus))) {
      return;
    }
    ::SetFocus(hwnd());
  }

  // The shell's rows rebuilt: Spaces, pins and bookmarks re-read in place
  // (the Mac's store subscriptions), the open menus and drags untouched.
  void Refresh() {
    if (dragging_) {
      return;  // A changed collection is applied once the drag ends.
    }
    RebuildModel();
    Relayout();
    RefreshHover();
    Invalidate();
  }

  // Relayout drops the base's hover (ClearHits), and after a modal loop (a
  // menu, a prompt, the picker) or a drag no WM_MOUSEMOVE follows to set it
  // again — so a row kept, or lacked, its hover pill until the pointer
  // moved. Re-derive it from where the pointer really is now.
  void RefreshHover() {
    if (!alive()) {
      return;
    }
    POINT screen = {};
    ::GetCursorPos(&screen);
    POINT client = screen;
    ::ScreenToClient(hwnd(), &client);
    RECT rc = {};
    ::GetClientRect(hwnd(), &rc);
    if (::PtInRect(&rc, client) && ::WindowFromPoint(screen) == hwnd()) {
      ::SendMessageW(hwnd(), WM_MOUSEMOVE, 0,
                     MAKELPARAM(static_cast<WORD>(client.x),
                                static_cast<WORD>(client.y)));
    } else {
      ::SendMessageW(hwnd(), WM_MOUSELEAVE, 0, 0);
    }
  }

  // An in-flight card drag or inline rename takes Escape before the
  // overlay does (the Mac's event monitor).
  bool ConsumesEscape() const {
    return dragging_ || rename_ != nullptr || creation_ != nullptr;
  }

  // Escape — from the view's own key or the shell's keyboard hook (the page
  // may hold the focus under the overlay): a card drag snaps back, an
  // inline rename or the create card cancels, a filled search clears,
  // else the host dismisses.
  void HandleEscape() {
    if (dragging_) {
      EndDrag(/*commit=*/false);
      return;
    }
    if (rename_) {
      EndRename(/*commit=*/false);
      return;
    }
    if (creation_) {
      CancelCreation();
      return;
    }
    if (search_ && ::GetFocus() == search_ &&
        ::GetWindowTextLengthW(search_) > 0) {
      ::SetWindowTextW(search_, L"");
      return;
    }
    if (host_) {
      host_->ViewRequestsDismiss();
    }
  }

  // Ctrl+F: Spaces has no search and swallows it; Downloads focuses the
  // search field.
  bool HandleFind() {
    if (section_ == PhiLibrarySection::kDownloads && search_) {
      ::SetFocus(search_);
      ::SendMessageW(search_, EM_SETSEL, 0, -1);
    }
    return true;
  }

  // The Spaces section's cards in display order (the reorder drag's
  // preview order while one is in flight).
  void Relayout() {
    if (!alive()) {
      return;
    }
    search_fill_ = FieldFill();  // The pill and its EDIT share this value.
    ClearHits();
    RECT rc = {};
    ::GetClientRect(hwnd(), &rc);
    LayoutRail(rc);
    if (flying_) {
      return;
    }
    if (section_ == PhiLibrarySection::kSpaces) {
      LayoutSpaces(ContentRect(rc));
    } else {
      LayoutDownloads(ContentRect(rc));
    }
  }

 protected:
  // --- PhiPanel ----------------------------------------------------------
  void Paint(HDC dc, const RECT&) override;

  PhiPalette QueryPalette() const override {
    return shell_ ? shell_->CurrentPalette() : PhiPalette();
  }

  void OnThemeChanged() override {
    PhiPanel::OnThemeChanged();
    RebuildModel();  // The cards' grounds follow the appearance.
    Relayout();
    if (alive() && (!fringe_fill_ || *fringe_fill_ != CardGround())) {
      fringe_fill_ = CardGround();
      SetCornerFringe(Scaled(kCardRadius), 0xF, CardGround());
    }
    // The fields repaint through WM_CTLCOLOREDIT to pick up the new fill.
    if (search_) {
      ::InvalidateRect(search_, nullptr, TRUE);
    }
    if (rename_) {
      ::InvalidateRect(rename_, nullptr, TRUE);
    }
    Invalidate();
  }

  void OnDpiChanged(UINT) override {
    icons_.Clear();
    if (search_) {
      ::SendMessageW(search_, WM_SETFONT,
                     reinterpret_cast<WPARAM>(BodyFont()), TRUE);
    }
    Relayout();
    Invalidate();
  }

  void OnSize(int, int) override {
    Relayout();
    Invalidate();
  }

  void OnHover(int, int) override {
    // The downloads row's action buttons and the pin / row washes are
    // hover-only; the base invalidated the two hits, the row behind them
    // needs the paint too.
    Invalidate();
  }

  void OnMouseDown(int hit, POINT pt) override;
  void OnClick(int hit, POINT pt) override;
  void OnWheel(int delta, POINT pt) override;
  bool OnKey(WPARAM key) override;
  void OnEscape() override;
  void OnTimerFired(UINT_PTR id) override {
    if (id == poll_.id()) {
      PollDownloads();
    } else if (id == focus_retake_.id()) {
      focus_retake_.Stop();
      TakeFocus();
    }
  }
  void OnCaptureLost() override {
    if (dragging_) {
      EndDrag(/*commit=*/false);
    }
    press_handle_ = -1;
  }
  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) override;
  void OnNcDestroy() override {
    poll_.Stop();
    PhiAnimator::ForThread().CancelForWindow(hwnd());
    creation_.reset();
    if (host_) {
      host_->ViewDestroyed(this);
    }
    PhiPanel::OnNcDestroy();
  }

  // --- PhiCreateSpaceFormHost (the inline creation card) ----------------
  void FormCommit(const PhiNewSpaceSpec& spec) override {
    // The Mac's CreateSpacePanel(style: .library): the new Space lands
    // after the window's current one and is NOT activated — the Library
    // stays up showing the new card.
    if (SpacesHost* spaces = shell_->spaces_host()) {
      spaces->RequestCreateSpaceInBackground(shell_->slot().value, spec,
                                             "library");
    }
    // The form calls in from its own handler: tear it down on our turn.
    ::PostMessageW(hwnd(), kMsgEndCreation, 0, 0);
  }
  void FormCancel() override {
    ::PostMessageW(hwnd(), kMsgEndCreation, 0, 0);
  }
  COLORREF FormGround() const override { return creation_ground_; }

 private:
  static constexpr UINT kMsgEndCreation = WM_APP + 41;
  // Posted by Present(): retake the keyboard after Chromium's own re-focus.
  static constexpr UINT kMsgTakeFocus = WM_APP + 42;
  static constexpr UINT_PTR kRenameFieldId = 0x4C01;
  static constexpr UINT_PTR kSearchFieldId = 0x4C02;

  struct DownloadGroup {
    std::wstring label;
    std::vector<size_t> rows;  // Indexes into filtered_.
    RECT header{};
  };

  // Fonts.
  HFONT BodyFont() { return fonts().Get(13, FW_NORMAL); }
  HFONT SmallFont() { return fonts().Get(11, FW_NORMAL); }
  HFONT SmallBoldFont() { return fonts().Get(11, FW_BOLD); }
  HFONT RailFont() { return fonts().Get(12, FW_MEDIUM); }
  HFONT TitleFont() { return fonts().Get(17, FW_SEMIBOLD); }
  HFONT GroupFont() { return fonts().Get(13, FW_SEMIBOLD); }
  HFONT StrikeFont() {
    PhiFontDesc desc;
    desc.size = 13;
    desc.strikeout = true;
    return fonts().Get(desc);
  }

  // Colours.
  // The rail's and card's ground: the Mac's windowBackground, which is the
  // same material the sidebar paints its ground with (PaintChrome's
  // `ground_top` of CurrentPalette) — so a Space with a linked palette
  // colours the Library as it colours the window chrome.
  COLORREF CardGround() const { return palette().ground_top; }
  COLORREF ContentGround() const {
    // The Mac's contentOverlayBackground (white; in dark the window's
    // resolved overlay made opaque — the linked palette's when the Space
    // carries one) under a 2.5 % accent wash.
    const PhiPalette& p = palette();
    COLORREF base = PhiRgb(0xFF, 0xFF, 0xFF);
    if (p.dark) {
      base = shell_->spaces_host()
                 ? OverlayFor(*shell_->spaces_host(), shell_->active_space(),
                              /*dark=*/true)
                 : p.ground_top;
    }
    return PhiBlend(p.accent, base, 6);
  }
  // The search pill's fill, which its EDIT paints with too: ink over the
  // CONTENT ground (hover_strong sits over the sidebar's ground_top and
  // would read as a block on the white content).
  COLORREF FieldFill() const {
    const PhiPalette& p = palette();
    return PhiBlend(p.ink, ContentGround(), p.dark ? 20 : 15);
  }
  // The stored value a field erases and paints with — the same the frame
  // around it was painted with.
  COLORREF FieldColorFor(HWND field) const {
    return field == rename_ ? rename_fill_ : search_fill_;
  }

  RECT ContentRect(const RECT& client) const {
    RECT out = client;
    out.left = client.left + Scaled(kRailWidth) + 1;
    return out;
  }

  // --- Rail -------------------------------------------------------------
  void LayoutRail(const RECT& client) {
    const int x = client.left + Scaled(kRailPadH);
    int y = client.top + Scaled(kRailPadV);
    rail_spaces_ = {x, y, x + Scaled(kRailItemWidth),
                    y + Scaled(kRailItemHeight)};
    y = rail_spaces_.bottom + Scaled(kRailItemGap);
    rail_downloads_ = {x, y, x + Scaled(kRailItemWidth),
                       y + Scaled(kRailItemHeight)};
    AddHit(rail_spaces_, MakeKey(kHitRailSpaces, 0, 0),
           PhiStr("library.navigation.spaces"), PhiHitRole::kTab,
           section_ == PhiLibrarySection::kSpaces);
    AddHit(rail_downloads_, MakeKey(kHitRailDownloads, 0, 0),
           PhiStr("library.navigation.downloads"), PhiHitRole::kTab,
           section_ == PhiLibrarySection::kDownloads);
    const int button = Scaled(kRailButton);
    const int mid = client.left + Scaled(kRailWidth) / 2;
    rail_open_ = {mid - button / 2,
                  client.bottom - Scaled(kRailButtonBottomPad) - button,
                  mid + button / 2, client.bottom - Scaled(kRailButtonBottomPad)};
    if (host_ && !host_->ViewIsStandalone()) {
      AddHit(rail_open_, MakeKey(kHitRailOpenWindow, 0, 0),
             PhiStr("library.navigation.openInNewWindow"),
             PhiHitRole::kButton);
    } else {
      rail_open_ = {0, 0, 0, 0};  // The standalone window has no twin.
    }
  }

  void PaintRail(HDC dc, const RECT& client) {
    const PhiPalette& p = palette();
    RECT rail = client;
    rail.right = client.left + Scaled(kRailWidth);
    HBRUSH ground = ::CreateSolidBrush(CardGround());
    ::FillRect(dc, &rail, ground);
    ::DeleteObject(ground);
    RECT separator = {rail.right, client.top, rail.right + 1, client.bottom};
    HBRUSH line = ::CreateSolidBrush(p.separator);
    ::FillRect(dc, &separator, line);
    ::DeleteObject(line);

    const struct {
      RECT bounds;
      const wchar_t* glyph;
      const char* key;
      PhiLibrarySection section;
    } items[] = {
        {rail_spaces_, kGlyphStack, "library.navigation.spaces",
         PhiLibrarySection::kSpaces},
        {rail_downloads_, fluent::kDownload, "library.navigation.downloads",
         PhiLibrarySection::kDownloads},
    };
    const int hovered_key = HoveredKey();
    for (const auto& item : items) {
      const bool selected = item.section == section_;
      const bool hot =
          hovered_key >= 0 &&
          KindOf(hovered_key) == (item.section == PhiLibrarySection::kSpaces
                                      ? kHitRailSpaces
                                      : kHitRailDownloads);
      if (selected) {
        // accent 12 % (dark 22 %).
        PhiFillRoundRectAA(dc, item.bounds, Scaled(kRailItemRadius),
                           PhiBlend(p.accent, CardGround(), p.dark ? 56 : 31));
      }
      if (hot) {
        PhiFillRoundRectAA(dc, item.bounds, Scaled(kRailItemRadius), p.hover);
      }
      // Icon box 30 over the label, the VStack's 9 between them, the pair
      // centred in the 88.
      const int box = Scaled(kRailIconBox);
      const int label_h = Scaled(16);
      const int total = box + Scaled(kRailIconToLabel) + label_h;
      const int top = (item.bounds.top + item.bounds.bottom - total) / 2;
      const int mid = (item.bounds.left + item.bounds.right) / 2;
      const RECT icon = {mid - box / 2, top, mid + box / 2, top + box};
      PhiDrawIconGlyph(dc, icon, item.glyph, Scaled(kRailIcon), p.accent);
      RECT label = {item.bounds.left + Scaled(kRailPadH),
                    top + box + Scaled(kRailIconToLabel),
                    item.bounds.right - Scaled(kRailPadH),
                    top + box + Scaled(kRailIconToLabel) + label_h};
      HGDIOBJ old = ::SelectObject(dc, RailFont());
      ::SetTextColor(dc, p.label);
      PhiDrawTextAA(dc, PhiStr(item.key), -1, &label,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
      ::SelectObject(dc, old);
    }
    if (rail_open_.right > rail_open_.left) {
      const bool hot =
          hovered_key >= 0 && KindOf(hovered_key) == kHitRailOpenWindow;
      PhiPaintIconButton(dc, rail_open_, fluent::kOpenInNewWindow,
                         hot ? PhiButtonState::kHover : PhiButtonState::kRest,
                         p, p.accent, Scaled(kRailButtonGlyph),
                         Scaled(fluent::kButtonRadius));
    }
  }

  int HoveredKey() const {
    const int hover = hovered();
    return hover >= 0 && hover < static_cast<int>(hits_.size())
               ? hits_[static_cast<size_t>(hover)].key
               : -1;
  }

  // --- Model ------------------------------------------------------------
  void RebuildModel() {
    if (section_ == PhiLibrarySection::kSpaces) {
      RebuildCards();
    } else {
      PollDownloads(/*force=*/true);
    }
  }

  // The Space cards from the hosts: user Spaces only (no agent, no
  // Incognito), each with its palette, its pins and its outline.
  void RebuildCards() {
    SpacesHost* spaces = shell_->spaces_host();
    SidebarHost* sidebar = shell_->sidebar_host();
    std::vector<SpaceCard> fresh;
    if (spaces) {
      const bool dark = palette().dark;
      for (const PhiSpaceView& space : spaces->Spaces()) {
        if (space.incognito || PhiIsAgentSpaceId(space.id)) {
          continue;
        }
        SpaceCard card;
        card.space = space;
        card.palette = spaces->PaletteForSpace(space.id);
        card.ground = GroundFor(*spaces, space.id, dark);
        card.icon_text = base::UTF8ToWide(space.icon_text);
        if (sidebar) {
          card.pins = sidebar->PinnedRowsForSpace(space.id);
          AppendOutline(*sidebar, space.id, std::string(), 0, &card.rows);
        }
        // Carry the per-card view state across a rebuild.
        for (const SpaceCard& old : cards_) {
          if (old.space.id == space.id) {
            card.scroll = old.scroll;
            card.selected_pin = old.selected_pin;
            card.selected_guid = old.selected_guid;
            card.slide_from = old.slide_from;
            card.slide_to = old.slide_to;
          }
        }
        fresh.push_back(std::move(card));
      }
    }
    cards_ = std::move(fresh);
    ResetOrder();
    if (selected_card_ >= static_cast<int>(cards_.size())) {
      selected_card_ = -1;
    }
  }

  // The Mac's LibrarySpaceBackdrop: the Space's resolved overlay colour at
  // 80 % over the window ground — the linked palette's anchor when the
  // Space carries one, else its pinned (or the app's) theme overlay.
  COLORREF GroundFor(const SpacesHost& spaces,
                     const std::string& space_id,
                     bool dark) const {
    return PhiBlend(OverlayFor(spaces, space_id, dark), CardGround(), 204);
  }

  // A Space's resolved overlay colour: the linked palette's anchor when the
  // Space carries one, else its pinned (or the app's) theme overlay.
  COLORREF OverlayFor(const SpacesHost& spaces,
                      const std::string& space_id,
                      bool dark) const {
    if (const std::optional<PhiThemePalette> linked =
            spaces.SpacePalette(space_id)) {
      return linked->ResolvedColor(0, dark);
    }
    std::string theme_id = spaces.SpaceThemeId(space_id);
    if (theme_id.empty() && shell_->prefs_host()) {
      theme_id = shell_->prefs_host()->ThemeId();
    }
    const PhiTheme& theme = PhiThemeById(theme_id);
    return dark ? theme.dark.overlay : theme.light.overlay;
  }

  void AppendOutline(const SidebarHost& sidebar,
                     const std::string& space_id,
                     const std::string& parent,
                     int depth,
                     std::vector<OutlineRow>* out) {
    const std::set<std::string>& expanded = expanded_[space_id];
    for (PhiStoredRowView& row : sidebar.BookmarkRowsForSpace(space_id, parent)) {
      OutlineRow entry;
      entry.depth = depth;
      entry.expanded = row.folder && expanded.count(row.guid) > 0;
      const bool descend = entry.expanded;
      const std::string guid = row.guid;
      entry.row = std::move(row);
      out->push_back(std::move(entry));
      if (descend && depth < 16) {
        AppendOutline(sidebar, space_id, guid, depth + 1, out);
      }
    }
  }

  // --- Spaces layout ----------------------------------------------------
  int CardSlotX(const RECT& content, int ordinal) const {
    return content.left + Scaled(kSpacesPad) +
           ordinal * (Scaled(kSpaceCardWidth) + Scaled(kSpaceCardGap)) -
           scroll_x_;
  }

  int CardsContentWidth() const {
    const int n = static_cast<int>(cards_.size()) + (creation_ ? 1 : 0);
    return Scaled(kSpacesPad) * 2 +
           n * (Scaled(kSpaceCardWidth) + Scaled(kSpaceCardGap)) +
           Scaled(kAddCircle);
  }

  void LayoutSpaces(const RECT& content) {
    content_ = content;
    card_list_.clear();
    const int limit = std::max(0, CardsContentWidth() - Width(content));
    scroll_x_ = std::clamp(scroll_x_, 0, limit);
    const int top = content.top + Scaled(kSpacesPad);
    const int bottom = content.bottom - Scaled(kSpaceRowBottomSlack) -
                       Scaled(kSpacesPad);
    const int card_w = Scaled(kSpaceCardWidth);
    // Display order: the drag's preview order while one is in flight.
    for (size_t ordinal = 0; ordinal < order_.size(); ++ordinal) {
      const size_t index = order_[ordinal];
      if (index >= cards_.size()) {
        continue;
      }
      SpaceCard& card = cards_[index];
      const int slot_x = CardSlotX(content, static_cast<int>(ordinal));
      int x = slot_x;
      if (dragging_ && static_cast<int>(index) == drag_card_) {
        x = CardSlotX(content, drag_origin_) + drag_delta_;
      } else if (slide_handle_ && card.slide_to == slot_x) {
        x = PhiLerpInt(card.slide_from, card.slide_to, slide_t_);
      } else {
        card.slide_from = slot_x;
        card.slide_to = slot_x;
      }
      card.bounds = {x, top, x + card_w, std::max(top + 1, bottom)};
      LayoutCard(static_cast<int>(index), card);
    }
    int next = static_cast<int>(order_.size());
    if (creation_) {
      const int x = CardSlotX(content, next);
      creation_bounds_ = {x, top, x + card_w, std::max(top + 1, bottom)};
      creation_->Reposition(creation_bounds_);
      ++next;
    }
    const int circle = Scaled(kAddCircle);
    const int add_x = CardSlotX(content, next);
    const int mid = (top + bottom) / 2;
    add_bounds_ = {add_x, mid - circle / 2, add_x + circle, mid + circle / 2};
    if (!creation_) {
      AddHit(add_bounds_, MakeKey(kHitAddSpace, 0, 0),
             PhiStr("library.spaces.create"), PhiHitRole::kButton);
    }
  }

  // The card's header, pins grid, outline and footer, in view pixels,
  // with hits for every visible control. Rows are clipped to the body.
  void LayoutCard(int index, SpaceCard& card) {
    const RECT& b = card.bounds;
    const int pad_h = Scaled(kSpaceHeaderPadH);
    const int header_top = b.top + Scaled(kSpaceCardPadV);
    const int header_h = Scaled(kSpaceHeaderHeight);
    const int icon_btn = Scaled(kSpaceIconButton);
    const int palette_btn = Scaled(kSpacePaletteButton);
    const int header_mid = header_top + header_h / 2;
    const RECT icon = {b.left + pad_h, header_mid - icon_btn / 2,
                       b.left + pad_h + icon_btn, header_mid + icon_btn / 2};
    const RECT palette_rc = {b.right - pad_h - palette_btn,
                             header_mid - palette_btn / 2, b.right - pad_h,
                             header_mid + palette_btn / 2};
    const RECT name = {icon.right + Scaled(kSpaceHeaderGap), header_top,
                       palette_rc.left - Scaled(kSpaceHeaderGap),
                       header_top + header_h};
    AddHit(icon, MakeKey(kHitCardIcon, index, 0),
           PhiStr("library.spaces.editIcon"), PhiHitRole::kButton);
    AddHit(name, MakeKey(kHitCardName, index, 0),
           base::UTF8ToWide(card.space.name), PhiHitRole::kButton);
    AddHit(palette_rc, MakeKey(kHitCardPalette, index, 0),
           PhiStr("library.spaces.editTheme"), PhiHitRole::kButton);
    if (rename_ && rename_card_ == index) {
      rename_fill_ = card.ground;  // What the EDIT erases and paints with.
      ::SetWindowPos(rename_, nullptr, name.left, name.top + Scaled(4),
                     Width(name), header_h - Scaled(8),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // Body: pins grid, outline, footer.
    const int body_top = header_top + header_h + Scaled(kSpaceHeaderToBody);
    const int body_bottom = b.bottom - Scaled(kSpaceCardPadV);
    const int body_left = b.left + Scaled(kSpaceBodyPadH);
    const int body_right = b.right - Scaled(kSpaceBodyPadH);
    const int footer = Scaled(kCardFooterButton);
    const RECT handle = {body_left + Scaled(kHandleLeading),
                         body_bottom - footer,
                         body_left + Scaled(kHandleLeading) + footer,
                         body_bottom};
    const RECT more = {body_right - footer, body_bottom - footer, body_right,
                       body_bottom};
    AddHit(handle, MakeKey(kHitCardHandle, index, 0),
           PhiStr("library.spaces.reorder"), PhiHitRole::kButton);
    AddHit(more, MakeKey(kHitCardMore, index, 0),
           PhiStr("library.spaces.moreActions"), PhiHitRole::kButton);
    const RECT list = {body_left, body_top, body_right,
                       body_bottom - footer - Scaled(kCardFooterGap)};
    // The blank body is the right-click target for the New… rows; added
    // first so every row and pin over it takes the pointer.
    AddHit(list, MakeKey(kHitCardBody, index, 0), std::wstring(),
           PhiHitRole::kGroup);

    // The body's content height first, so the scroll clamps before the
    // rows are placed: pins (a 3-column grid of 64x45 cells, nothing when
    // empty), the gap, the outline rows.
    const int cell_w = Scaled(kPinCellWidth);
    const int cell_h = Scaled(kPinCellHeight);
    const int gap = Scaled(kPinCellGap);
    const int row_h = Scaled(kOutlineRow);
    const int pin_rows =
        (static_cast<int>(card.pins.size()) + kPinColumns - 1) / kPinColumns;
    const int pins_h = card.pins.empty()
                           ? 0
                           : pin_rows * cell_h + (pin_rows - 1) * gap +
                                 Scaled(kPinsToOutline);
    const int content_h = pins_h + static_cast<int>(card.rows.size()) * row_h;
    card.scroll = std::clamp(card.scroll, 0,
                             std::max(0, content_h - Height(list)));
    int y = list.top - card.scroll;
    if (!card.pins.empty()) {
      const int grid_w = kPinColumns * cell_w + (kPinColumns - 1) * gap;
      const int grid_left = (list.left + list.right - grid_w) / 2;
      for (size_t i = 0; i < card.pins.size(); ++i) {
        const int col = static_cast<int>(i) % kPinColumns;
        const int row = static_cast<int>(i) / kPinColumns;
        const RECT cell = {grid_left + col * (cell_w + gap),
                           y + row * (cell_h + gap),
                           grid_left + col * (cell_w + gap) + cell_w,
                           y + row * (cell_h + gap) + cell_h};
        RECT visible = cell;
        visible.top = std::max(visible.top, list.top);
        visible.bottom = std::min(visible.bottom, list.bottom);
        if (visible.bottom > visible.top) {
          AddHit(visible, MakeKey(kHitCardPin, index, static_cast<int>(i)),
                 RowName(card.pins[i]), PhiHitRole::kListItem,
                 selected_card_ == index &&
                     card.selected_pin == static_cast<int>(i));
        }
      }
      y += pins_h;
    }
    // The outline.
    for (size_t i = 0; i < card.rows.size(); ++i) {
      OutlineRow& row = card.rows[i];
      row.bounds = {list.left, y, list.right, y + row_h};
      RECT visible = row.bounds;
      visible.top = std::max(visible.top, list.top);
      visible.bottom = std::min(visible.bottom, list.bottom);
      if (visible.bottom > visible.top) {
        AddHit(visible, MakeKey(kHitCardRow, index, static_cast<int>(i)),
               RowName(row.row), PhiHitRole::kListItem,
               selected_card_ == index && card.selected_guid == row.row.guid);
      }
      y += row_h;
    }
    card_list_[index] = list;
  }

  static std::wstring RowName(const PhiStoredRowView& row) {
    return base::UTF8ToWide(row.title.empty() ? row.url : row.title);
  }

  // --- Spaces paint -----------------------------------------------------
  void PaintSpaces(HDC dc, const RECT& content) {
    const int saved = ::SaveDC(dc);
    ::IntersectClipRect(dc, content.left, content.top, content.right,
                        content.bottom);
    const PhiPalette& p = palette();
    // Back to front: the dragged card paints last, over its neighbours.
    std::vector<size_t> paint_order = order_;
    if (dragging_ && drag_card_ >= 0) {
      auto it = std::find(paint_order.begin(), paint_order.end(),
                          static_cast<size_t>(drag_card_));
      if (it != paint_order.end()) {
        paint_order.erase(it);
        paint_order.push_back(static_cast<size_t>(drag_card_));
      }
    }
    for (const size_t index : paint_order) {
      if (index < cards_.size()) {
        PaintCard(dc, static_cast<int>(index), cards_[index]);
      }
    }
    if (creation_) {
      // The form paints itself; its card ground and corners are ours.
      PhiFillRoundRectAA(dc, creation_bounds_, Scaled(kSpaceCardRadius),
                         creation_ground_);
    }
    // The "+" circle (the Mac's thinMaterial): ink 6 % over the content.
    if (!creation_) {
      const bool hot =
          HoveredKey() >= 0 && KindOf(HoveredKey()) == kHitAddSpace;
      PhiFillEllipseAA(dc, add_bounds_,
                       PhiBlend(p.ink, ContentGround(), hot ? 26 : 15));
      PhiDrawIconGlyph(dc, add_bounds_, fluent::kAdd, Scaled(kAddGlyph),
                       p.label);
    }
    ::RestoreDC(dc, saved);
  }

  void PaintCard(HDC dc, int index, const SpaceCard& card) {
    const RECT& b = card.bounds;
    const PhiPalette& cp = card.palette;
    // The Mac's shadow (black 28 %, radius 8, y 4) as three concentric
    // rings outside the card, 2 dip down — black 18 / 10 / 4 % (light
    // 10 / 5 / 2 %) from the edge out — so a card the colour of the content
    // ground (the active Space's, in dark) still reads; then the card and
    // its 1 px border.
    const COLORREF content = ContentGround();
    const bool dark = palette().dark;
    // Outermost ring first (index 0), so each inner ring paints over it.
    const std::array<int, 3> alphas = {dark ? 10 : 5, dark ? 26 : 13,
                                       dark ? 46 : 26};
    const int step = std::max(1, Scaled(1));
    int ring = 3;
    for (const int alpha : alphas) {
      RECT r = b;
      ::OffsetRect(&r, 0, Scaled(2));
      ::InflateRect(&r, ring * step, ring * step);
      PhiFillRoundRectAA(dc, r, Scaled(kSpaceCardRadius) + ring * step,
                         PhiBlend(PhiRgb(0, 0, 0), content, alpha));
      --ring;
    }
    PhiFillRoundRectAA(dc, b, Scaled(kSpaceCardRadius), card.ground);
    PhiStrokeRoundRectAA(dc, b, Scaled(kSpaceCardRadius), cp.border, 1);

    const int hovered_key = HoveredKey();
    const bool hover_here = hovered_key >= 0 && CardOf(hovered_key) == index;
    const HitKind hover_kind = hover_here ? KindOf(hovered_key) : kHitRailSpaces;
    const int hover_index = hover_here ? IndexOf(hovered_key) : -1;

    // Header.
    const int pad_h = Scaled(kSpaceHeaderPadH);
    const int header_top = b.top + Scaled(kSpaceCardPadV);
    const int header_h = Scaled(kSpaceHeaderHeight);
    const int header_mid = header_top + header_h / 2;
    const int icon_btn = Scaled(kSpaceIconButton);
    const int palette_btn = Scaled(kSpacePaletteButton);
    const RECT icon = {b.left + pad_h, header_mid - icon_btn / 2,
                       b.left + pad_h + icon_btn, header_mid + icon_btn / 2};
    const RECT palette_rc = {b.right - pad_h - palette_btn,
                             header_mid - palette_btn / 2, b.right - pad_h,
                             header_mid + palette_btn / 2};
    const RECT name = {icon.right + Scaled(kSpaceHeaderGap), header_top,
                       palette_rc.left - Scaled(kSpaceHeaderGap),
                       header_top + header_h};
    PhiPaintButtonWash(dc, icon,
                       hover_kind == kHitCardIcon ? PhiButtonState::kHover
                                                  : PhiButtonState::kRest,
                       cp, Scaled(fluent::kButtonRadius));
    const int icon_px = Scaled(kSpaceIconPx);
    const RECT icon_rc = {(icon.left + icon.right - icon_px) / 2,
                          (icon.top + icon.bottom - icon_px) / 2,
                          (icon.left + icon.right + icon_px) / 2,
                          (icon.top + icon.bottom + icon_px) / 2};
    if (!card.icon_text.empty() &&
        PhiDrawCatalogIcon(dc, icon_rc, card.icon_text, icon_px, cp.label)) {
    } else if (!card.icon_text.empty() &&
               PhiDrawColorIcon(dc, icon_rc, card.icon_text, icon_px,
                                cp.label)) {
    } else {
      HGDIOBJ old = ::SelectObject(dc, TitleFont());
      ::SetTextColor(dc, cp.label);
      const std::wstring initial =
          card.space.name.empty()
              ? std::wstring(L"?")
              : base::UTF8ToWide(card.space.name).substr(0, 1);
      RECT rc = icon_rc;
      PhiDrawTextAA(dc, initial.c_str(), -1, &rc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      ::SelectObject(dc, old);
    }
    if (!(rename_ && rename_card_ == index)) {
      HGDIOBJ old = ::SelectObject(dc, TitleFont());
      ::SetTextColor(dc, cp.label);
      RECT rc = name;
      PhiDrawTextAA(dc, base::UTF8ToWide(card.space.name).c_str(), -1, &rc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
      ::SelectObject(dc, old);
    }
    PhiPaintIconButton(dc, palette_rc, fluent::kColor,
                       hover_kind == kHitCardPalette ? PhiButtonState::kHover
                                                     : PhiButtonState::kRest,
                       cp, cp.label, Scaled(kSpacePaletteGlyph),
                       Scaled(fluent::kButtonRadius));

    // Body, clipped to the list area.
    auto found = card_list_.find(index);
    if (found == card_list_.end()) {
      return;
    }
    const RECT list = found->second;
    const int saved = ::SaveDC(dc);
    ::IntersectClipRect(dc, list.left, list.top, list.right, list.bottom);
    const COLORREF ink4 = PhiBlend(cp.ink, card.ground, 10);
    const COLORREF ink8 = PhiBlend(cp.ink, card.ground, 20);
    int y = list.top - card.scroll;
    const int cell_w = Scaled(kPinCellWidth);
    const int cell_h = Scaled(kPinCellHeight);
    const int gap = Scaled(kPinCellGap);
    if (!card.pins.empty()) {
      const int grid_w = kPinColumns * cell_w + (kPinColumns - 1) * gap;
      const int grid_left = (list.left + list.right - grid_w) / 2;
      for (size_t i = 0; i < card.pins.size(); ++i) {
        const int col = static_cast<int>(i) % kPinColumns;
        const int row = static_cast<int>(i) / kPinColumns;
        const RECT cell = {grid_left + col * (cell_w + gap),
                           y + row * (cell_h + gap),
                           grid_left + col * (cell_w + gap) + cell_w,
                           y + row * (cell_h + gap) + cell_h};
        if (cell.bottom < list.top || cell.top > list.bottom) {
          continue;
        }
        const bool selected = selected_card_ == index &&
                              card.selected_pin == static_cast<int>(i);
        const bool hot = hover_kind == kHitCardPin &&
                         hover_index == static_cast<int>(i);
        PhiFillRoundRectAA(dc, cell, Scaled(kPinCellRadius),
                           selected ? cp.card : hot ? ink8 : ink4);
        PaintPinFavicons(dc, cell, card.pins[i], cp);
      }
      const int rows = (static_cast<int>(card.pins.size()) + kPinColumns - 1) /
                       kPinColumns;
      y += rows * cell_h + (rows - 1) * gap + Scaled(kPinsToOutline);
    }
    HGDIOBJ old_font = ::SelectObject(dc, BodyFont());
    for (size_t i = 0; i < card.rows.size(); ++i) {
      const OutlineRow& row = card.rows[i];
      if (row.bounds.bottom < list.top || row.bounds.top > list.bottom) {
        continue;
      }
      const bool selected = selected_card_ == index &&
                            card.selected_guid == row.row.guid;
      const bool hot = hover_kind == kHitCardRow &&
                       hover_index == static_cast<int>(i);
      RECT pill = row.bounds;
      const int inset = (Scaled(kOutlineRow) - Scaled(kOutlineRowContent)) / 2;
      pill.top += inset;
      pill.bottom -= inset;
      if (selected || hot) {
        PhiFillRoundRectAA(dc, pill, Scaled(kOutlineRowRadius),
                           selected ? cp.card : ink4);
      }
      const int indent = Scaled(kOutlineLeadPad) +
                         row.depth * Scaled(kOutlineIndent);
      int x = pill.left + indent;
      const int mid = (pill.top + pill.bottom) / 2;
      if (row.row.folder) {
        const int chevron = Scaled(kOutlineChevron);
        const RECT chevron_rc = {x, mid - chevron / 2, x + chevron,
                                 mid + chevron / 2};
        PhiDrawIconGlyph(dc, chevron_rc,
                         row.expanded ? fluent::kChevronDown
                                      : fluent::kChevronRight,
                         chevron, cp.label_dim);
        x += chevron + Scaled(4);
        const int glyph = Scaled(kOutlineFolderGlyph);
        const RECT folder_rc = {x, mid - glyph / 2, x + glyph, mid + glyph / 2};
        // The folder's catalog mask, as the bookmark manager draws it: the
        // plain folder (open when disclosed) or the chosen symbol.
        const std::string resolved = PhiResolveFolderIcon(row.row.icon);
        const PhiMacIcon* mask = resolved == PhiFolderIconDefaultId()
                                     ? PhiPlainFolderMask(row.expanded)
                                     : PhiFolderIconMask(resolved);
        bool drawn = false;
        if (mask) {
          const std::vector<unsigned char> bgra =
              PhiTintedIconBgra(*mask, cp.label_dim, glyph);
          drawn = !bgra.empty() &&
                  PhiBlendBgra(dc, folder_rc, bgra.data(), glyph, glyph);
        }
        if (!drawn) {
          PhiDrawIconGlyph(dc, folder_rc, fluent::kFolder, glyph - Scaled(4),
                           cp.label);
        }
        x += glyph + Scaled(8);
      } else {
        const int fav = Scaled(kOutlineFavicon);
        const RECT fav_rc = {x, mid - fav / 2, x + fav, mid + fav / 2};
        PaintFavicon(dc, fav_rc, row.row.favicon_bgra, row.row.favicon_width,
                     row.row.favicon_height, cp);
        x += fav + Scaled(8);
      }
      RECT text = {x, pill.top, pill.right - Scaled(8), pill.bottom};
      ::SetTextColor(dc, cp.label);
      PhiDrawTextAA(dc, RowName(row.row).c_str(), -1, &text,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
    }
    ::SelectObject(dc, old_font);
    ::RestoreDC(dc, saved);

    // Footer: the drag handle and the "…" menu.
    const int body_bottom = b.bottom - Scaled(kSpaceCardPadV);
    const int body_left = b.left + Scaled(kSpaceBodyPadH);
    const int body_right = b.right - Scaled(kSpaceBodyPadH);
    const int footer = Scaled(kCardFooterButton);
    const RECT handle = {body_left + Scaled(kHandleLeading),
                         body_bottom - footer,
                         body_left + Scaled(kHandleLeading) + footer,
                         body_bottom};
    const RECT more = {body_right - footer, body_bottom - footer, body_right,
                       body_bottom};
    PhiPaintIconButton(dc, handle, kGlyphMove,
                       hover_kind == kHitCardHandle || (dragging_ &&
                                                        drag_card_ == index)
                           ? PhiButtonState::kHover
                           : PhiButtonState::kRest,
                       cp, cp.label_dim, Scaled(14), Scaled(12));
    PhiPaintIconButton(dc, more, fluent::kMore,
                       hover_kind == kHitCardMore ? PhiButtonState::kHover
                                                  : PhiButtonState::kRest,
                       cp, cp.label_dim, Scaled(14), Scaled(12));
  }

  void PaintFavicon(HDC dc,
                    const RECT& rc,
                    const std::vector<unsigned char>& bgra,
                    int width,
                    int height,
                    const PhiPalette& cp) {
    // Scaled with interpolation: a stored 16 px capture (or the history
    // service's larger icon) lands in an 18 or 14 px slot.
    if (!bgra.empty() && width > 0 && height > 0) {
      PhiDrawBgra(dc, rc, bgra.data(), width, height);
    } else {
      PhiDrawIconGlyph(dc, rc, fluent::kGlobe, Width(rc), cp.label_dim);
    }
  }

  void PaintPinFavicons(HDC dc,
                        const RECT& cell,
                        const PhiStoredRowView& pin,
                        const PhiPalette& cp) {
    const int mid_x = (cell.left + cell.right) / 2;
    const int mid_y = (cell.top + cell.bottom) / 2;
    if (pin.url_b.empty()) {
      const int fav = Scaled(kPinFavicon);
      const RECT rc = {mid_x - fav / 2, mid_y - fav / 2, mid_x + fav / 2,
                       mid_y + fav / 2};
      PaintFavicon(dc, rc, pin.favicon_bgra, pin.favicon_width,
                   pin.favicon_height, cp);
      return;
    }
    // A split pin: its two panes' icons side by side.
    const int fav = Scaled(kPinSplitFavicon);
    const int gap = Scaled(4);
    const RECT a = {mid_x - fav - gap / 2, mid_y - fav / 2, mid_x - gap / 2,
                    mid_y + fav / 2};
    const RECT b = {mid_x + gap / 2, mid_y - fav / 2, mid_x + gap / 2 + fav,
                    mid_y + fav / 2};
    PaintFavicon(dc, a, pin.favicon_bgra, pin.favicon_width,
                 pin.favicon_height, cp);
    PaintFavicon(dc, b, pin.favicon_b_bgra, pin.favicon_b_width,
                 pin.favicon_b_height, cp);
  }

  // --- Downloads --------------------------------------------------------
  void SyncPolling() {
    if (section_ == PhiLibrarySection::kDownloads) {
      EnsureSearchField();
      if (search_) {
        ::ShowWindow(search_, SW_SHOWNA);
      }
      if (!poll_.running()) {
        poll_.Start(kPhiDownloadsPollMs);
      }
    } else {
      poll_.Stop();
      if (search_) {
        ::ShowWindow(search_, SW_HIDE);
      }
    }
  }

  void EnsureSearchField() {
    if (search_ || !alive()) {
      return;
    }
    search_ = ::CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | ES_LEFT | ES_AUTOHSCROLL, 0, 0, 10, 10,
        hwnd(), reinterpret_cast<HMENU>(kSearchFieldId),
        ::GetModuleHandleW(nullptr), nullptr);
    if (!search_) {
      return;
    }
    ::SendMessageW(search_, WM_SETFONT, reinterpret_cast<WPARAM>(BodyFont()),
                   TRUE);
    ::SendMessageW(search_, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(PhiStr("downloads.all.search")));
    ::SetWindowLongPtrW(search_, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(this));
    search_base_proc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
        search_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&FieldProc)));
  }

  void PollDownloads(bool force = false) {
    phi_framework::PhiFramework* framework = shell_->framework();
    const BrowserId browser = shell_->active_browser_id();
    const char* json = framework && browser.valid()
                           ? framework->ListDownloadsJson(browser.value)
                           : "[]";
    if (!force && json == last_json_) {
      return;
    }
    last_json_ = json;
    downloads_ = PhiParseDownloadsJson(last_json_);
    FilterDownloads();
    Relayout();
    Invalidate();
  }

  // A local calendar day: an ordering key (yyyymmdd) and its midnight, for
  // the group's label.
  struct LocalDay {
    long long key = 0;
    time_t midnight = 0;
  };

  static LocalDay DayOf(double unix_seconds) {
    LocalDay day;
    const time_t t = static_cast<time_t>(unix_seconds);
    struct tm local = {};
    if (localtime_s(&local, &t) != 0) {
      return day;
    }
    day.key = (local.tm_year + 1900LL) * 10000 + (local.tm_mon + 1) * 100 +
              local.tm_mday;
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    day.midnight = mktime(&local);
    return day;
  }

  // The search over name + URL (the Mac's localizedStandardContains),
  // then the day groups, newest first. The framework's row carries
  // endTime only (0 while live), so a live download groups under today
  // and a finished one under the day it ended — the Mac groups by start.
  void FilterDownloads() {
    filtered_.clear();
    const std::wstring query = PhiLower(PhiTrim(query_));
    for (size_t i = 0; i < downloads_.size(); ++i) {
      const PhiDownloadRow& row = downloads_[i];
      if (!query.empty() &&
          PhiLower(row.name).find(query) == std::wstring::npos &&
          PhiLower(row.url).find(query) == std::wstring::npos) {
        continue;
      }
      filtered_.push_back(i);
    }
    groups_.clear();
    const double now = static_cast<double>(::time(nullptr));
    const LocalDay today = DayOf(now);
    const LocalDay yesterday = DayOf(now - 86400.0);
    std::map<long long, size_t, std::greater<long long>> by_day;
    std::vector<std::pair<long long, DownloadGroup>> built;
    for (size_t f = 0; f < filtered_.size(); ++f) {
      const PhiDownloadRow& row = downloads_[filtered_[f]];
      const LocalDay day = row.end_time > 0 ? DayOf(row.end_time) : today;
      auto found = by_day.find(day.key);
      if (found == by_day.end()) {
        DownloadGroup group;
        if (day.key == 0) {
          group.label = PhiStr("downloads.all.unknownDate");
        } else if (day.key == today.key) {
          group.label = PhiStr("downloads.item.completedTime.today");
        } else if (day.key == yesterday.key) {
          group.label = PhiStr("downloads.item.completedTime.yesterday");
        } else {
          group.label = PhiFormatDate(
              static_cast<int64_t>(day.midnight) * 1000,
              PhiDateStyle::kLongDate);
        }
        built.emplace_back(day.key, std::move(group));
        found = by_day.emplace(day.key, built.size() - 1).first;
      }
      built[found->second].second.rows.push_back(f);
    }
    // Newest day first; an unknown day (key 0) sorts last.
    std::stable_sort(built.begin(), built.end(),
                     [](const auto& a, const auto& b) {
                       return a.first > b.first;
                     });
    for (auto& [key, group] : built) {
      groups_.push_back(std::move(group));
    }
    dl_scroll_ = std::max(0, dl_scroll_);
  }

  RECT DownloadsInner(const RECT& content) const {
    const int max_w = Scaled(kDlMaxWidth);
    const int w = std::max(1, std::min(max_w, Width(content) - 2 * Scaled(kDlPadH)));
    const int left = (content.left + content.right - w) / 2;
    return {left, content.top, left + w, content.bottom};
  }

  void LayoutDownloads(const RECT& content) {
    content_ = content;
    const RECT inner = DownloadsInner(content);
    const int pad = Scaled(kDlHeaderPad);
    dl_search_ = {inner.left + pad, inner.top + pad, inner.right - pad,
                  inner.top + pad + Scaled(kDlSearchHeight)};
    if (search_) {
      // The field sits inside the pill, with the search glyph leading.
      const RECT field = {dl_search_.left + Scaled(32),
                          dl_search_.top + Scaled(7), dl_search_.right - Scaled(8),
                          dl_search_.bottom - Scaled(7)};
      ::SetWindowPos(search_, nullptr, field.left, field.top, Width(field),
                     Height(field), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    dl_list_ = {inner.left, dl_search_.bottom + pad, inner.right,
                inner.bottom};
    const int row_h = Scaled(kDlRowHeight);
    const int group_h = Scaled(kDlGroupPadTop) + Scaled(kDlGroupText) +
                        Scaled(kDlGroupPadBottom);
    // The list's full height first, so the scroll clamps before the rows
    // are placed.
    int content_h = Scaled(kDlListPadBottom);
    for (const DownloadGroup& group : groups_) {
      content_h += group_h + static_cast<int>(group.rows.size()) * row_h;
    }
    dl_max_scroll_ = std::max(0, content_h - Height(dl_list_));
    dl_scroll_ = std::clamp(dl_scroll_, 0, dl_max_scroll_);
    int y = dl_list_.top - dl_scroll_;
    dl_rows_.clear();
    for (DownloadGroup& group : groups_) {
      group.header = {dl_list_.left + Scaled(kDlListPadH), y,
                      dl_list_.right - Scaled(kDlListPadH), y + group_h};
      y += group_h;
      for (const size_t f : group.rows) {
        const RECT row = {dl_list_.left + Scaled(kDlListPadH), y,
                          dl_list_.right - Scaled(kDlListPadH), y + row_h};
        dl_rows_.push_back({f, row});
        RECT visible = row;
        visible.top = std::max(visible.top, dl_list_.top);
        visible.bottom = std::min(visible.bottom, dl_list_.bottom);
        if (visible.bottom > visible.top) {
          const PhiDownloadRow& item = downloads_[filtered_[f]];
          AddHit(visible, MakeKey(kHitDownloadRow, 0, static_cast<int>(f)),
                 item.name, PhiHitRole::kListItem);
          const std::vector<PhiDownloadAction> actions =
              PhiDownloadActionsFor(item);
          for (size_t a = 0; a < actions.size(); ++a) {
            RECT button = ActionRect(row, static_cast<int>(a),
                                     static_cast<int>(actions.size()));
            button.top = std::max(button.top, dl_list_.top);
            button.bottom = std::min(button.bottom, dl_list_.bottom);
            if (button.bottom > button.top) {
              AddHit(button,
                     MakeKey(kHitDownloadAction, static_cast<int>(a),
                             static_cast<int>(f)),
                     PhiStr(actions[a].name_key), PhiHitRole::kButton, false,
                     actions[a].enabled);
            }
          }
        }
        y += row_h;
      }
    }
  }

  RECT ActionRect(const RECT& row, int ordinal, int count) const {
    const int btn = Scaled(kDlActionButton);
    const int gap = Scaled(kDlActionGap);
    const int mid = (row.top + row.bottom) / 2;
    const int right = row.right - Scaled(kDlRowPadH) -
                      (count - 1 - ordinal) * (btn + gap);
    return {right - btn, mid - btn / 2, right, mid + btn / 2};
  }

  void PaintDownloads(HDC dc, const RECT& content) {
    const PhiPalette& p = palette();
    const int saved = ::SaveDC(dc);
    ::IntersectClipRect(dc, content.left, content.top, content.right,
                        content.bottom);
    // The search pill: resting field fill, the magnifier leading.
    PhiFillRoundRectAA(dc, dl_search_, Scaled(fluent::kButtonRadius),
                       search_fill_);
    const RECT glass = {dl_search_.left + Scaled(8), dl_search_.top,
                        dl_search_.left + Scaled(28), dl_search_.bottom};
    PhiDrawIconGlyph(dc, glass, fluent::kSearch, Scaled(14), p.label_dim);

    const COLORREF dim = p.label_dim;
    if (filtered_.empty()) {
      // The empty state: a 36 glyph over the message, centred.
      const RECT list = dl_list_;
      const int mid_y = (list.top + list.bottom) / 2;
      const int glyph = Scaled(kDlEmptyGlyph);
      const RECT icon = {(list.left + list.right) / 2 - glyph / 2,
                         mid_y - glyph - Scaled(6),
                         (list.left + list.right) / 2 + glyph / 2,
                         mid_y - Scaled(6)};
      PhiDrawIconGlyph(dc, icon,
                       PhiTrim(query_).empty() ? fluent::kDownload
                                               : fluent::kSearch,
                       glyph, dim);
      HGDIOBJ old = ::SelectObject(dc, BodyFont());
      ::SetTextColor(dc, dim);
      RECT text = {list.left, mid_y + Scaled(6), list.right,
                   mid_y + Scaled(28)};
      PhiDrawTextAA(dc,
                    PhiStr(PhiTrim(query_).empty()
                               ? "downloads.all.empty"
                               : "downloads.all.noSearchResults"),
                    -1, &text, DT_CENTER | DT_TOP | DT_SINGLELINE);
      ::SelectObject(dc, old);
      ::RestoreDC(dc, saved);
      return;
    }

    ::IntersectClipRect(dc, dl_list_.left, dl_list_.top, dl_list_.right,
                        dl_list_.bottom);
    HGDIOBJ old = ::SelectObject(dc, GroupFont());
    for (const DownloadGroup& group : groups_) {
      if (group.header.bottom < dl_list_.top ||
          group.header.top > dl_list_.bottom) {
        continue;
      }
      RECT text = {group.header.left + Scaled(kDlGroupPadH),
                   group.header.top + Scaled(kDlGroupPadTop),
                   group.header.right - Scaled(kDlGroupPadH),
                   group.header.bottom - Scaled(kDlGroupPadBottom)};
      ::SetTextColor(dc, dim);
      PhiDrawTextAA(dc, group.label.c_str(), -1, &text,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    const int hovered_key = HoveredKey();
    const int hover_row =
        hovered_key >= 0 && (KindOf(hovered_key) == kHitDownloadRow ||
                             KindOf(hovered_key) == kHitDownloadAction)
            ? IndexOf(hovered_key)
            : -1;
    const int hover_action =
        hovered_key >= 0 && KindOf(hovered_key) == kHitDownloadAction
            ? CardOf(hovered_key)
            : -1;
    for (const auto& [f, row] : dl_rows_) {
      if (row.bottom < dl_list_.top || row.top > dl_list_.bottom) {
        continue;
      }
      const PhiDownloadRow& item = downloads_[filtered_[f]];
      const bool hot = hover_row == static_cast<int>(f);
      if (hot) {
        PhiFillRoundRectAA(dc, row, Scaled(kDlRowRadius),
                           PhiBlend(p.ink, ContentGround(), 13));
      }
      const int icon = Scaled(kDlIcon);
      const int mid = (row.top + row.bottom) / 2;
      const int left = row.left + Scaled(kDlRowPadH);
      const RECT icon_rc = {left, mid - icon / 2, left + icon,
                            mid - icon / 2 + icon};
      icons_.Draw(dc, icon_rc, item.path, icon);
      const std::vector<PhiDownloadAction> actions =
          PhiDownloadActionsFor(item);
      const int actions_w =
          static_cast<int>(actions.size()) * Scaled(kDlActionButton) +
          (static_cast<int>(actions.size()) - 1) * Scaled(kDlActionGap);
      const RECT info = {left + icon + Scaled(kDlIconGap), row.top,
                         row.right - Scaled(kDlRowPadH) -
                             (hot ? actions_w + Scaled(8) : 0),
                         row.bottom};
      const bool failed = PhiDownloadFailed(item);
      ::SelectObject(dc, failed ? StrikeFont() : BodyFont());
      ::SetTextColor(dc, p.label);
      RECT name_rc = {info.left, mid - Scaled(18), info.right,
                      mid - Scaled(1)};
      const char* status_key = PhiDownloadStatusKey(item);
      if (status_key) {
        RECT name_measure = name_rc;
        PhiDrawTextAA(dc, item.name.c_str(), -1, &name_measure,
                      DT_CALCRECT | DT_SINGLELINE);
        RECT tag_measure = {0, 0, 0, 0};
        ::SelectObject(dc, SmallBoldFont());
        const std::wstring tag = std::wstring(L"· ") + PhiStr(status_key);
        PhiDrawTextAA(dc, tag.c_str(), -1, &tag_measure,
                      DT_CALCRECT | DT_SINGLELINE);
        RECT name_draw = name_rc;
        name_draw.right = std::min<LONG>(
            name_rc.right - tag_measure.right - Scaled(4),
            name_rc.left + name_measure.right);
        ::SelectObject(dc, failed ? StrikeFont() : BodyFont());
        PhiDrawTextAA(dc, item.name.c_str(), -1, &name_draw,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
        RECT tag_rc = {std::min<LONG>(name_draw.right,
                                      name_draw.left + name_measure.right) +
                           Scaled(4),
                       name_rc.top, name_rc.right, name_rc.bottom};
        ::SelectObject(dc, SmallBoldFont());
        ::SetTextColor(dc, dim);
        PhiDrawTextAA(dc, tag.c_str(), -1, &tag_rc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      } else {
        PhiDrawTextAA(dc, item.name.c_str(), -1, &name_rc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
      }
      // Second line: progress (bar, or figures while hovered) for live
      // downloads, relative date or "From host" otherwise.
      RECT sub = {info.left, mid + Scaled(2), info.right, mid + Scaled(17)};
      ::SelectObject(dc, SmallFont());
      ::SetTextColor(dc, dim);
      if (item.state == "in_progress") {
        if (hot) {
          std::wstring text = PhiStrF(
              "downloads.item.transfer.progressLabel",
              {PhiFormatDownloadBytes(item.received),
               PhiFormatDownloadBytes(item.total)});
          if (item.paused) {
            text += L" ";
            text += PhiStr("downloads.item.status.pausedIndicator");
          } else if (item.speed > 0) {
            text += L" ・ " + PhiFormatDownloadBytes(item.speed) + L"/s";
          }
          PhiDrawTextAA(dc, text.c_str(), -1, &sub,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
          const int bar_mid = (sub.top + sub.bottom) / 2;
          RECT track = {sub.left, bar_mid - Scaled(1), sub.right,
                        bar_mid + Scaled(2)};
          PhiFillRoundRectAA(dc, track, Scaled(3),
                             PhiBlend(p.accent, ContentGround(), 51));
          if (item.total > 0) {
            const double done =
                std::clamp(item.received / item.total, 0.0, 1.0);
            RECT fill = track;
            fill.right = fill.left + static_cast<int>(Width(track) * done);
            if (fill.right > fill.left) {
              PhiFillRoundRectAA(dc, fill, Scaled(3), p.accent);
            }
          }
        }
      } else {
        const std::wstring text =
            item.state == "complete" && item.end_time > 0
                ? PhiFormatDownloadRelative(item.end_time)
                : PhiStrF("downloads.item.sourceHostLabel", {item.host});
        PhiDrawTextAA(dc, text.c_str(), -1, &sub,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      }
      // Hover-only action buttons at the right.
      if (hot) {
        for (size_t a = 0; a < actions.size(); ++a) {
          const RECT button = ActionRect(row, static_cast<int>(a),
                                         static_cast<int>(actions.size()));
          const wchar_t glyph[2] = {actions[a].glyph, 0};
          PhiPaintIconButton(
              dc, button, glyph,
              !actions[a].enabled ? PhiButtonState::kDisabled
              : hover_action == static_cast<int>(a) ? PhiButtonState::kHover
                                                    : PhiButtonState::kRest,
              p, p.label, Scaled(12), Scaled(fluent::kButtonRadius));
        }
      }
    }
    ::SelectObject(dc, old);
    ::RestoreDC(dc, saved);
  }

  void RunDownloadAction(int row_index, int action_index) {
    if (row_index < 0 || row_index >= static_cast<int>(filtered_.size())) {
      return;
    }
    const PhiDownloadRow& row =
        downloads_[filtered_[static_cast<size_t>(row_index)]];
    const std::vector<PhiDownloadAction> actions = PhiDownloadActionsFor(row);
    if (action_index < 0 || action_index >= static_cast<int>(actions.size()) ||
        !actions[static_cast<size_t>(action_index)].enabled) {
      return;
    }
    const PhiDownloadAction& action =
        actions[static_cast<size_t>(action_index)];
    if (std::string_view(action.verb) == "copy") {
      PhiCopyTextToClipboard(hwnd(), row.url);
      return;
    }
    phi_framework::PhiFramework* framework = shell_->framework();
    const BrowserId browser = shell_->active_browser_id();
    if (framework && browser.valid()) {
      framework->DownloadAction(browser.value, static_cast<uint32_t>(row.id),
                                action.verb);
      last_json_.clear();  // Force the next poll to repaint.
      PollDownloads(/*force=*/true);
    }
  }

  // --- Interaction ------------------------------------------------------
  void SwitchSection(PhiLibrarySection section) {
    if (section == section_) {
      return;
    }
    LastSections()[shell_] = section;
    SetSection(section, /*force_event=*/false);
  }

  // Space-card header actions.
  void OpenIconPicker(int card) {
    if (card < 0 || card >= static_cast<int>(cards_.size())) {
      return;
    }
    const PhiHit* hit = HitByKey(MakeKey(kHitCardIcon, card, 0));
    RECT anchor = hit ? PhiToRect(hit->bounds) : cards_[card].bounds;
    ::MapWindowPoints(hwnd(), nullptr, reinterpret_cast<POINT*>(&anchor), 2);
    const std::string space_id = cards_[card].space.id;
    // The picker pumps its own loop; the Library may close (and free this
    // view) under it.
    const std::weak_ptr<int> alive_token = token_;
    shell_->ShowSpaceIconPicker(space_id, anchor);
    if (!alive_token.expired()) {
      Refresh();
    }
  }

  void OpenThemePanel(int card) {
    if (card < 0 || card >= static_cast<int>(cards_.size())) {
      return;
    }
    const PhiHit* hit = HitByKey(MakeKey(kHitCardPalette, card, 0));
    RECT anchor = hit ? PhiToRect(hit->bounds) : cards_[card].bounds;
    ::MapWindowPoints(hwnd(), nullptr, reinterpret_cast<POINT*>(&anchor), 2);
    PhiShowSpaceThemePanel(shell_, anchor, cards_[card].space.id);
  }

  void BeginRename(int card) {
    if (card < 0 || card >= static_cast<int>(cards_.size()) || !alive()) {
      return;
    }
    EndRename(/*commit=*/true);
    rename_card_ = card;
    rename_fill_ = cards_[card].ground;
    rename_ = ::CreateWindowExW(
        0, L"EDIT", base::UTF8ToWide(cards_[card].space.name).c_str(),
        WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL, 0, 0, 10, 10,
        hwnd(), reinterpret_cast<HMENU>(kRenameFieldId),
        ::GetModuleHandleW(nullptr), nullptr);
    if (!rename_) {
      rename_card_ = -1;
      return;
    }
    ::SendMessageW(rename_, WM_SETFONT, reinterpret_cast<WPARAM>(TitleFont()),
                   TRUE);
    ::SendMessageW(rename_, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(PhiStr("library.spaces.name")));
    ::SendMessageW(rename_, EM_SETLIMITTEXT, kPhiMaxNameCodePoints * 2, 0);
    ::SetWindowLongPtrW(rename_, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(this));
    rename_base_proc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
        rename_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&FieldProc)));
    Relayout();  // Places the field over the name.
    ::SetFocus(rename_);
    ::SendMessageW(rename_, EM_SETSEL, 0, -1);
    Invalidate();
  }

  // Return / blur commit (a changed, non-empty name renames the Space),
  // Escape cancels.
  void EndRename(bool commit) {
    if (!rename_) {
      return;
    }
    HWND field = rename_;
    rename_ = nullptr;  // Re-entrancy: the destroy's WM_KILLFOCUS lands here.
    const int card = rename_card_;
    rename_card_ = -1;
    std::wstring value;
    if (commit) {
      const int length = ::GetWindowTextLengthW(field);
      value.resize(static_cast<size_t>(length) + 1);
      ::GetWindowTextW(field, value.data(), length + 1);
      value.resize(static_cast<size_t>(length));
      value = PhiTrimWhitespace(value);
    }
    // Hand the EDIT its own procedure back for its teardown, then drop the
    // back-pointer so nothing routes here from the destroy.
    if (rename_base_proc_) {
      ::SetWindowLongPtrW(field, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(rename_base_proc_));
    }
    ::SetWindowLongPtrW(field, GWLP_USERDATA, 0);
    ::DestroyWindow(field);
    if (alive()) {
      ::SetFocus(hwnd());
    }
    if (commit && !value.empty() && card >= 0 &&
        card < static_cast<int>(cards_.size()) &&
        base::WideToUTF8(value) != cards_[card].space.name) {
      if (SpacesHost* spaces = shell_->spaces_host()) {
        spaces->RenameSpace(cards_[card].space.id, base::WideToUTF8(value));
      }
    }
    if (alive()) {
      Refresh();
    }
  }

  // The inline create-Space card (the Mac's creationCard): the form hosted
  // in this view at the slot after the last card.
  void BeginCreation() {
    if (creation_ || !alive()) {
      return;
    }
    std::string profile_id;
    std::string theme_id;
    int saturation = -1;
    if (SpacesHost* spaces = shell_->spaces_host()) {
      for (const PhiSpaceView& space : spaces->Spaces()) {
        if (space.id == shell_->active_space()) {
          profile_id = space.profile_id;
        }
      }
      theme_id = spaces->SpaceThemeId(shell_->active_space());
      saturation = spaces->SpaceSaturation(shell_->active_space());
    }
    if (theme_id.empty() && shell_->prefs_host()) {
      theme_id = shell_->prefs_host()->ThemeId();
      saturation = shell_->prefs_host()->Saturation();
    }
    const PhiTheme& theme = PhiThemeById(theme_id);
    creation_ground_ = PhiBlend(
        palette().dark ? theme.dark.overlay : theme.light.overlay,
        CardGround(), 204);
    creation_ = std::make_unique<PhiCreateSpacePanel>(
        shell_, hwnd(), static_cast<PhiCreateSpaceFormHost*>(this));
    Relayout();  // Sets creation_bounds_.
    creation_->Show(creation_bounds_, profile_id, theme_id, saturation);
    // Scroll the new card into view, at the trailing edge.
    scroll_x_ = std::max(0, CardsContentWidth() - Width(content_));
    Relayout();
    creation_->FocusField();
    Invalidate();
  }

  void CancelCreation() {
    if (!creation_) {
      return;
    }
    creation_.reset();
    Relayout();
    Invalidate();
  }

  void ShowMoreMenu(int card) {
    if (card < 0 || card >= static_cast<int>(cards_.size())) {
      return;
    }
    SpacesHost* spaces = shell_->spaces_host();
    if (!spaces) {
      return;
    }
    const SpaceCard snapshot = cards_[card];
    const std::string space_id = snapshot.space.id;
    PhiMenuSpec spec;
    AppendCreateItems(&spec);
    spec.items.push_back(PhiMenuItem::Separator());
    // Set Profile ▸: a radio of the user profiles; the default Space's
    // binding cannot move (a fresh shell's first browser lands there).
    std::vector<PhiMenuItem> profiles;
    for (const std::string& summary : spaces->ProfileSummaries()) {
      const size_t tab = summary.find('\t');
      const std::string id = summary.substr(0, tab);
      if (id.empty()) {
        continue;
      }
      PhiMenuItem item = PhiMenuItem::Command(
          "lib.profile",
          base::UTF8ToWide(tab == std::string::npos ? id
                                                    : summary.substr(tab + 1)));
      item.target.id = id;
      item.target.text = item.label;
      item.checked = id == snapshot.space.profile_id;
      profiles.push_back(std::move(item));
    }
    PhiMenuItem profile_menu = PhiMenuItem::Submenu(
        PhiStr("library.spaces.setProfile"), std::move(profiles));
    profile_menu.enabled = space_id != spaces->DefaultSpaceId();
    profile_menu.glyph = fluent::kContact;
    spec.items.push_back(std::move(profile_menu));
    PhiMenuItem remove = PhiMenuItem::Command(
        "lib.deleteSpace", PhiStr("library.spaces.deleteSpace"));
    remove.enabled = spaces->CanDeleteSpace(space_id);
    remove.glyph = fluent::kDelete;
    spec.items.push_back(std::move(remove));

    const PhiHit* hit = HitByKey(MakeKey(kHitCardMore, card, 0));
    const std::weak_ptr<int> alive_token = token_;
    const PhiMenuAction action = RunMenu(
        spec, hit ? PhiToRect(hit->bounds) : snapshot.bounds,
        /*above_anchor=*/true);
    if (alive_token.expired()) {
      return;  // The menu's loop closed the Library.
    }
    if (action.is("lib.profile")) {
      // Confirmed like the Mac's: the Space's window is replaced and its
      // tabs reload in the new profile.
      if (PhiConfirm(shell_->shell_hwnd(),
                     PhiStrF("app.changeSpaceProfileConfirmation.title",
                             {action.target.text})
                         .c_str(),
                     PhiStr("app.changeSpaceProfileConfirmation."
                            "profileScopedMessage"))) {
        spaces->SetSpaceProfile(space_id, action.target.id);
      }
    } else if (action.is("lib.deleteSpace")) {
      if (PhiAppMenu* menu = shell_->app_menu()) {
        menu->DeleteSpace(space_id);  // The existing confirming flow.
      }
    } else {
      RunCreateItem(action, space_id, std::string());
    }
    if (!alive_token.expired()) {
      Refresh();
    }
  }

  void AppendCreateItems(PhiMenuSpec* spec) const {
    PhiMenuItem pin =
        PhiMenuItem::Command("lib.newPin", PhiStr("library.spaces.newPin"));
    pin.glyph = fluent::kPin;
    PhiMenuItem bookmark = PhiMenuItem::Command(
        "lib.newBookmark", PhiStr("library.spaces.newBookmark"));
    bookmark.glyph = fluent::kFavoriteStar;
    PhiMenuItem folder = PhiMenuItem::Command(
        "lib.newFolder", PhiStr("library.spaces.newFolder"));
    folder.glyph = fluent::kFolder;
    spec->items.push_back(std::move(pin));
    spec->items.push_back(std::move(bookmark));
    spec->items.push_back(std::move(folder));
  }

  // The New… rows: a URL prompt for a pin or a bookmark (no title editor
  // exists here — the Mac's EditPinnedTabPresenter has no Windows twin;
  // the title defaults to the address, as the sidebar's drop does), the
  // folder-name prompt the sidebar uses for a folder.
  void RunCreateItem(const PhiMenuAction& action,
                     const std::string& space_id,
                     const std::string& parent) {
    SidebarHost* sidebar = shell_->sidebar_host();
    if (!sidebar || !action.valid()) {
      return;
    }
    // The prompts pump their own loops; `sidebar` outlives this view, the
    // members below do not.
    const std::weak_ptr<int> alive_token = token_;
    HWND owner = shell_->shell_hwnd();
    if (action.is("lib.newPin") || action.is("lib.newBookmark")) {
      std::wstring url;
      if (PhiPromptText(owner,
                        PhiStr(action.is("lib.newPin")
                                   ? "library.spaces.newPin"
                                   : "library.spaces.newBookmark"),
                        PhiStr("sidebar.address"), &url) &&
          !PhiTrimWhitespace(url).empty()) {
        const std::string spelled = base::WideToUTF8(PhiTrimWhitespace(url));
        if (action.is("lib.newPin")) {
          sidebar->AddPinnedToSpace(space_id, spelled, spelled);
        } else {
          sidebar->AddBookmarkToSpace(space_id, parent, spelled, spelled,
                                      /*folder=*/false);
        }
      }
    } else if (action.is("lib.newFolder")) {
      std::wstring name = PhiStr("common.bookmarkFolder.defaultName");
      if (PhiPromptText(owner, PhiStr("app.bookmarkFolderDialog.createTitle"),
                        PhiStr("bookmarkManager.folderName"), &name,
                        /*confirm_label=*/nullptr, kPhiNameRules)) {
        sidebar->AddBookmarkToSpace(space_id, parent, base::WideToUTF8(name),
                                    std::string(), /*folder=*/true);
        if (!parent.empty() && !alive_token.expired()) {
          expanded_[space_id].insert(parent);
        }
      }
    }
  }

  // Right-click on a pin or an outline row: Open, Edit…, Move to Space ▸,
  // Delete (the Mac's bookmarkMenu / populatePinMenu).
  void ShowItemMenu(int card, bool pin, int index, POINT pt) {
    if (card < 0 || card >= static_cast<int>(cards_.size())) {
      return;
    }
    const SpaceCard& c = cards_[card];
    const PhiStoredRowView* row = nullptr;
    if (pin && index >= 0 && index < static_cast<int>(c.pins.size())) {
      row = &c.pins[static_cast<size_t>(index)];
    } else if (!pin && index >= 0 && index < static_cast<int>(c.rows.size())) {
      row = &c.rows[static_cast<size_t>(index)].row;
    }
    if (!row) {
      return;
    }
    const PhiStoredRowView item = *row;  // The menu outlives a rebuild.
    const std::string space_id = c.space.id;
    Select(card, pin ? index : -1, pin ? std::string() : item.guid);
    PhiMenuSpec spec;
    if (!item.folder) {
      PhiMenuItem open =
          PhiMenuItem::Command("lib.open", PhiStr("library.spaces.openItem"));
      open.glyph = fluent::kGlobe;
      open.default_item = true;
      spec.items.push_back(std::move(open));
    }
    PhiMenuItem edit =
        PhiMenuItem::Command("lib.edit", PhiStr("library.spaces.editItem"));
    edit.glyph = fluent::kRename;
    spec.items.push_back(std::move(edit));
    std::vector<PhiMenuItem> targets;
    if (SpacesHost* spaces = shell_->spaces_host()) {
      for (const PhiSpaceView& space : spaces->Spaces()) {
        if (space.id == space_id || space.incognito ||
            PhiIsAgentSpaceId(space.id)) {
          continue;
        }
        if (item.folder) {
          break;  // Folders do not travel between Spaces here.
        }
        PhiMenuItem target = PhiMenuItem::Command(
            "lib.move", base::UTF8ToWide(space.name));
        target.target.id = space.id;
        target.glyph = base::UTF8ToWide(space.icon_text);
        targets.push_back(std::move(target));
      }
    }
    if (!targets.empty()) {
      PhiMenuItem move = PhiMenuItem::Submenu(
          PhiStr("library.spaces.moveToSpace"), std::move(targets));
      move.glyph = fluent::kTabs;
      spec.items.push_back(std::move(move));
    }
    PhiMenuItem remove = PhiMenuItem::Command(
        "lib.delete", PhiStr("library.spaces.deleteItem"));
    remove.glyph = fluent::kDelete;
    spec.items.push_back(std::move(remove));

    const RECT at = {pt.x, pt.y, pt.x, pt.y};
    const std::weak_ptr<int> alive_token = token_;
    const PhiMenuAction action = RunMenu(spec, at, /*above_anchor=*/false);
    if (alive_token.expired()) {
      return;  // The menu's loop closed the Library.
    }
    SidebarHost* sidebar = shell_->sidebar_host();
    if (action.is("lib.open")) {
      OpenItem(space_id, item);
      return;  // The view may be gone.
    }
    if (action.is("lib.edit") && sidebar) {
      EditItem(item);
    } else if (action.is("lib.move") && sidebar) {
      if (pin) {
        sidebar->TransferPinToSpace(item.guid, action.target.id);
      } else {
        sidebar->MoveStoredRowToSpace(item.guid, action.target.id,
                                      /*clone=*/false);
      }
    } else if (action.is("lib.delete") && sidebar) {
      sidebar->RemoveStoredRow(item.guid);
    }
    if (!alive_token.expired()) {
      Refresh();
    }
  }

  // Edit…: the sidebar's own prompts — the address for a pin or a
  // bookmark, the name for a folder.
  void EditItem(const PhiStoredRowView& item) {
    SidebarHost* sidebar = shell_->sidebar_host();
    if (!sidebar) {
      return;
    }
    if (item.folder) {
      std::wstring name = base::UTF8ToWide(item.title);
      if (PhiPromptText(shell_->shell_hwnd(),
                        PhiStr("bookmarkManager.renameFolder"),
                        PhiStr("bookmarkManager.enterANewNameFor2"), &name,
                        /*confirm_label=*/nullptr, kPhiNameRules)) {
        sidebar->RenameStoredRow(item.guid, base::WideToUTF8(name));
      }
      return;
    }
    std::wstring edited = base::UTF8ToWide(item.url);
    if (PhiPromptText(shell_->shell_hwnd(),
                      PhiStr("common.bookmarkEditor.editTitle"),
                      PhiStr("sidebar.address"), &edited) &&
        !edited.empty()) {
      sidebar->SetStoredRowUrl(item.guid, base::WideToUTF8(edited));
    }
  }

  void ShowBodyMenu(int card, POINT pt) {
    if (card < 0 || card >= static_cast<int>(cards_.size())) {
      return;
    }
    const std::string space_id = cards_[card].space.id;
    PhiMenuSpec spec;
    AppendCreateItems(&spec);
    const RECT at = {pt.x, pt.y, pt.x, pt.y};
    const std::weak_ptr<int> alive_token = token_;
    const PhiMenuAction action = RunMenu(spec, at, /*above_anchor=*/false);
    if (alive_token.expired()) {
      return;  // The menu's loop closed the Library.
    }
    RunCreateItem(action, space_id, std::string());
    if (!alive_token.expired()) {
      Refresh();
    }
  }

  // `above_anchor`: the menu opens UPWARD from the anchor's top edge — the
  // card's "…" sits at the card's foot, and the Mac pops its menu from the
  // button's top; else it drops from the point given (a right-click).
  PhiMenuAction RunMenu(const PhiMenuSpec& spec,
                        const RECT& anchor_client,
                        bool above_anchor) {
    RECT anchor = anchor_client;
    ::MapWindowPoints(hwnd(), nullptr, reinterpret_cast<POINT*>(&anchor), 2);
    PhiPopupMenuRequest request;
    request.owner = shell_->shell_hwnd();
    request.x = anchor.left;
    request.y = anchor.top;
    request.align = TPM_LEFTALIGN | TPM_RIGHTBUTTON |
                    (above_anchor ? TPM_BOTTOMALIGN : TPM_TOPALIGN);
    const PhiPalette colors = palette();
    request.palette = &colors;
    PhiModalScope modal(shell_->shell_hwnd());  // The menu pumps its loop.
    return PhiRunPopupMenu(spec, request);
  }

  void Select(int card, int pin, const std::string& guid) {
    for (SpaceCard& c : cards_) {
      c.selected_pin = -1;
      c.selected_guid.clear();
    }
    selected_card_ = card;
    if (card >= 0 && card < static_cast<int>(cards_.size())) {
      cards_[card].selected_pin = pin;
      cards_[card].selected_guid = guid;
    }
    Relayout();
    Invalidate();
  }

  // Opens a pin or a bookmark into the browser: the Library yields, the
  // window switches to the item's Space, the item opens or focuses there
  // (the Mac's open(itemID:) over slot.activate).
  void OpenItem(const std::string& space_id, const PhiStoredRowView& item) {
    if (item.folder || item.url.empty()) {
      return;
    }
    PhiShellWindow* shell = shell_;
    const std::string guid = item.guid;
    const std::string url = item.url;
    if (host_) {
      host_->ViewRequestsDismiss();  // May destroy this view.
    }
    shell->OpenStoredRowInSpace(space_id, guid, url);
  }

  void DeleteSelection() {
    if (selected_card_ < 0 ||
        selected_card_ >= static_cast<int>(cards_.size())) {
      return;
    }
    const SpaceCard& c = cards_[selected_card_];
    SidebarHost* sidebar = shell_->sidebar_host();
    if (!sidebar) {
      return;
    }
    if (c.selected_pin >= 0 &&
        c.selected_pin < static_cast<int>(c.pins.size())) {
      sidebar->RemoveStoredRow(c.pins[static_cast<size_t>(c.selected_pin)].guid);
    } else if (!c.selected_guid.empty()) {
      sidebar->RemoveStoredRow(c.selected_guid);
    }
    Select(-1, -1, std::string());
    Refresh();
  }

  void ToggleFolder(int card, int row_index) {
    if (card < 0 || card >= static_cast<int>(cards_.size())) {
      return;
    }
    SpaceCard& c = cards_[card];
    if (row_index < 0 || row_index >= static_cast<int>(c.rows.size()) ||
        !c.rows[static_cast<size_t>(row_index)].row.folder) {
      return;
    }
    std::set<std::string>& expanded = expanded_[c.space.id];
    const std::string& guid = c.rows[static_cast<size_t>(row_index)].row.guid;
    if (!expanded.insert(guid).second) {
      expanded.erase(guid);
    }
    Refresh();
  }

  // --- Card reorder drag (LibrarySpaceDragHandle + LibrarySpacesView) ---
  void BeginDrag(int card) {
    if (card < 0 || card >= static_cast<int>(cards_.size())) {
      return;
    }
    dragging_ = true;
    drag_card_ = card;
    auto it = std::find(order_.begin(), order_.end(), static_cast<size_t>(card));
    drag_origin_ = it == order_.end() ? 0 : static_cast<int>(it - order_.begin());
    original_order_ = order_;
    drag_delta_ = 0;
    Relayout();
    Invalidate();
  }

  void MoveDrag(int delta) {
    if (!dragging_) {
      return;
    }
    drag_delta_ = delta;
    const int stride = Scaled(kSpaceCardWidth) + Scaled(kSpaceCardGap);
    const int target = std::clamp(
        static_cast<int>(std::lround(
            drag_origin_ + static_cast<double>(delta) / stride)),
        0, static_cast<int>(order_.size()) - 1);
    auto it = std::find(order_.begin(), order_.end(),
                        static_cast<size_t>(drag_card_));
    if (it == order_.end()) {
      return;  // The card left the collection under the drag.
    }
    const int current = static_cast<int>(it - order_.begin());
    if (target != current) {
      // The others slide to make room: from where they are drawn now to
      // their new slots, ease-out 150 ms.
      for (size_t ordinal = 0; ordinal < order_.size(); ++ordinal) {
        SpaceCard& c = cards_[order_[ordinal]];
        c.slide_from = c.bounds.left;
      }
      order_.erase(it);
      order_.insert(order_.begin() + target, static_cast<size_t>(drag_card_));
      for (size_t ordinal = 0; ordinal < order_.size(); ++ordinal) {
        cards_[order_[ordinal]].slide_to =
            CardSlotX(content_, static_cast<int>(ordinal));
      }
      StartSlide();
    }
    Relayout();
    Invalidate();
  }

  void StartSlide() {
    PhiAnimator& animator = PhiAnimator::ForThread();
    if (slide_handle_) {
      animator.Cancel(slide_handle_);
    }
    slide_t_ = 0;
    slide_handle_ = animator.Animate(
        hwnd(), kSlideMs, PhiEasing::kEaseOut,
        [this](double t, RECT* dirty) {
          slide_t_ = t;
          Relayout();
          ::GetClientRect(hwnd(), dirty);
        },
        [this]() {
          slide_handle_ = 0;
          slide_t_ = 1;
          Relayout();
          Invalidate();
        });
  }

  void EndDrag(bool commit) {
    if (!dragging_) {
      return;
    }
    dragging_ = false;
    capture_.Release();
    const int card = drag_card_;
    drag_card_ = -1;
    if (PhiAnimator::ForThread().IsRunning(slide_handle_)) {
      PhiAnimator::ForThread().Cancel(slide_handle_);
    }
    slide_handle_ = 0;
    if (commit && card >= 0 && card < static_cast<int>(cards_.size())) {
      auto it = std::find(order_.begin(), order_.end(), static_cast<size_t>(card));
      const int target = it == order_.end() ? -1 : static_cast<int>(it - order_.begin());
      if (target >= 0 && target != drag_origin_) {
        // The index in the FULL Space list: the global position of the
        // card that held the target slot before the move (the cards are a
        // subsequence of the list, so this is where it lands among them).
        if (SpacesHost* spaces = shell_->spaces_host()) {
          const std::vector<PhiSpaceView> all = spaces->Spaces();
          const size_t displaced = original_order_[static_cast<size_t>(target)];
          int global = -1;
          for (size_t i = 0; i < all.size(); ++i) {
            if (displaced < cards_.size() &&
                all[i].id == cards_[displaced].space.id) {
              global = static_cast<int>(i);
            }
          }
          if (global >= 0) {
            spaces->ReorderSpace(cards_[static_cast<size_t>(card)].space.id,
                                 global);
          }
        }
      }
    } else {
      order_ = original_order_;
    }
    Refresh();
  }

  // The display order, rebuilt from the cards after a model rebuild
  // (the drag's preview order lives only while it is in flight).
  void ResetOrder() {
    order_.clear();
    for (size_t i = 0; i < cards_.size(); ++i) {
      order_.push_back(i);
    }
  }

  static LRESULT CALLBACK FieldProc(HWND hwnd,
                                    UINT msg,
                                    WPARAM w,
                                    LPARAM l);

  RAW_PTR_EXCLUSION PhiShellWindow* const shell_;
  RAW_PTR_EXCLUSION LibraryViewHost* host_;
  PhiLibrarySection section_ = PhiLibrarySection::kDownloads;
  bool flying_ = false;
  RECT content_{};
  // Liveness token for the code after a modal loop (a menu, a prompt, the
  // icon picker): the loop may close the Library and free this view.
  std::shared_ptr<int> token_ = std::make_shared<int>(0);
  // The EDIT fields' ground brush (WM_CTLCOLOREDIT), rebuilt on change.
  HBRUSH field_brush_ = nullptr;
  COLORREF field_brush_color_ = 0;
  // ONE value per field, computed in the layout pass and used by every
  // side: the pill's fill (what the search EDIT erases and paints with) and
  // the renamed card's ground (the rename EDIT's).
  COLORREF search_fill_ = 0;
  COLORREF rename_fill_ = 0;
  std::optional<COLORREF> fringe_fill_;

  // Rail.
  RECT rail_spaces_{}, rail_downloads_{}, rail_open_{};

  // Spaces.
  std::vector<SpaceCard> cards_;
  std::vector<size_t> order_;  // Display order (indexes into cards_).
  std::vector<size_t> original_order_;
  std::map<int, RECT> card_list_;  // Card index -> its list area.
  std::map<std::string, std::set<std::string>> expanded_;  // Per Space.
  int selected_card_ = -1;
  int scroll_x_ = 0;
  RECT add_bounds_{};
  // Rename.
  HWND rename_ = nullptr;
  WNDPROC rename_base_proc_ = nullptr;
  // The opaque-paint buffers of the two EDITs (phi_opaque_edit.h).
  std::unique_ptr<PhiLayerSurface> rename_surface_;
  std::unique_ptr<PhiLayerSurface> search_surface_;
  int rename_card_ = -1;
  // Creation.
  std::unique_ptr<PhiCreateSpacePanel> creation_;
  RECT creation_bounds_{};
  COLORREF creation_ground_ = 0;
  // Drag.
  int press_handle_ = -1;  // The card whose handle is pressed (pre-drag).
  POINT press_point_{};
  Capture capture_;
  bool dragging_ = false;
  int drag_card_ = -1;
  int drag_origin_ = 0;
  int drag_delta_ = 0;
  int slide_handle_ = 0;
  double slide_t_ = 1;

  // Downloads.
  HWND search_ = nullptr;
  WNDPROC search_base_proc_ = nullptr;
  std::wstring query_;
  Timer poll_;
  Timer focus_retake_;  // One shot after Present (see TakeFocus).
  std::string last_json_;
  std::vector<PhiDownloadRow> downloads_;
  std::vector<size_t> filtered_;
  std::vector<DownloadGroup> groups_;
  std::vector<std::pair<size_t, RECT>> dl_rows_;
  PhiDownloadIconCache icons_;
  RECT dl_search_{}, dl_list_{};
  int dl_scroll_ = 0;
  int dl_max_scroll_ = 0;
};

void LibraryView::Paint(HDC dc, const RECT&) {
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  const PhiPalette& p = palette();
  HBRUSH ground = ::CreateSolidBrush(CardGround());
  ::FillRect(dc, &client, ground);
  ::DeleteObject(ground);
  ::SetBkMode(dc, TRANSPARENT);
  if (!flying_) {
    PaintRail(dc, client);
    const RECT content = ContentRect(client);
    HBRUSH wash = ::CreateSolidBrush(ContentGround());
    ::FillRect(dc, &content, wash);
    ::DeleteObject(wash);
    if (section_ == PhiLibrarySection::kSpaces) {
      PaintSpaces(dc, content);
    } else {
      PaintDownloads(dc, content);
    }
  }
  // The 1 px border, ink 8 %, along the card's rounded edge.
  PhiStrokeRoundRectAA(dc, client, Scaled(kCardRadius), p.border, 1);
}

void LibraryView::OnMouseDown(int hit, POINT pt) {
  if (flying_) {
    return;
  }
  if (rename_ && !(hit >= 0 && KindOf(hits_[static_cast<size_t>(hit)].key) ==
                                   kHitCardName &&
                   CardOf(hits_[static_cast<size_t>(hit)].key) ==
                       rename_card_)) {
    EndRename(/*commit=*/true);  // A click elsewhere commits (blur).
  }
  ::SetFocus(hwnd());  // Keys (Escape, Delete, Ctrl+F) come here.
  // The key first: Select() below relays out and rebuilds hits_.
  const int key = hit < 0 ? 0 : hits_[static_cast<size_t>(hit)].key;
  const bool on_item =
      hit >= 0 && (KindOf(key) == kHitCardPin || KindOf(key) == kHitCardRow);
  if (selected_card_ >= 0 && !on_item) {
    // A press anywhere but on a pin or a row — a card's blank body, another
    // card, the content ground, the rail — ends the selection (the Mac's
    // blank click ends editing and deselects); a pin or row re-selects on
    // its click.
    Select(-1, -1, std::string());
  }
  if (hit < 0) {
    return;
  }
  if (KindOf(key) == kHitCardHandle) {
    // The handle arms a drag: it starts once the pointer moves > 4 dip.
    press_handle_ = CardOf(key);
    press_point_ = pt;
    capture_ = BeginCapture();
  }
}

void LibraryView::OnClick(int hit, POINT pt) {
  if (flying_) {
    return;
  }
  if (dragging_) {
    EndDrag(/*commit=*/true);
    return;
  }
  const bool armed = press_handle_ >= 0;
  press_handle_ = -1;
  capture_.Release();
  if (armed || hit < 0) {
    return;  // A handle click without a drag does nothing (the Mac's).
  }
  const int key = hits_[static_cast<size_t>(hit)].key;
  const int card = CardOf(key);
  const int index = IndexOf(key);
  switch (KindOf(key)) {
    case kHitRailSpaces:
      SwitchSection(PhiLibrarySection::kSpaces);
      break;
    case kHitRailDownloads:
      SwitchSection(PhiLibrarySection::kDownloads);
      break;
    case kHitRailOpenWindow: {
      PhiShellWindow* shell = shell_;
      const PhiLibrarySection section = section_;
      if (host_) {
        host_->ViewRequestsDismiss();  // May destroy this view.
      }
      PhiLibrary::OpenInNewWindow(shell, section);
      return;
    }
    case kHitAddSpace:
      BeginCreation();
      break;
    case kHitCardIcon:
      OpenIconPicker(card);
      break;
    case kHitCardName:
      BeginRename(card);
      break;
    case kHitCardPalette:
      OpenThemePanel(card);
      break;
    case kHitCardPin:
      Select(card, index, std::string());
      break;
    case kHitCardRow:
      if (card >= 0 && card < static_cast<int>(cards_.size()) &&
          index >= 0 && index < static_cast<int>(cards_[card].rows.size())) {
        const OutlineRow& row = cards_[card].rows[static_cast<size_t>(index)];
        if (row.row.folder) {
          ToggleFolder(card, index);  // A single click discloses.
        } else {
          Select(card, -1, row.row.guid);
        }
      }
      break;
    case kHitCardMore:
      ShowMoreMenu(card);
      break;
    case kHitDownloadAction:
      RunDownloadAction(index, card);  // card byte carries the action.
      break;
    case kHitCardHandle:
    case kHitCardBody:
    case kHitDownloadRow:
      break;
  }
}

void LibraryView::OnWheel(int delta, POINT pt) {
  if (flying_) {
    return;
  }
  if (section_ == PhiLibrarySection::kDownloads) {
    const int before = dl_scroll_;
    dl_scroll_ = std::clamp(dl_scroll_ - delta * Scaled(kDlRowHeight) /
                                             WHEEL_DELTA,
                            0, dl_max_scroll_);
    if (dl_scroll_ != before) {
      Relayout();
      Invalidate();
    }
    return;
  }
  // Inside a card's body the wheel scrolls that card; elsewhere it
  // scrolls the row of cards sideways.
  for (const auto& [index, list] : card_list_) {
    if (::PtInRect(&list, pt) && index >= 0 &&
        index < static_cast<int>(cards_.size())) {
      SpaceCard& card = cards_[index];
      const int before = card.scroll;
      card.scroll = std::max(0, card.scroll - delta / 2);
      Relayout();  // Clamps.
      if (card.scroll != before) {
        Invalidate();
      }
      return;
    }
  }
  const int before = scroll_x_;
  scroll_x_ -= delta / 2;
  Relayout();  // Clamps.
  if (scroll_x_ != before) {
    Invalidate();
  }
}

bool LibraryView::OnKey(WPARAM key) {
  const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
  if (ctrl && (key == 'F' || key == 'f')) {
    return HandleFind();
  }
  if (key == VK_DELETE && section_ == PhiLibrarySection::kSpaces) {
    DeleteSelection();
    return true;
  }
  return PhiPanel::OnKey(key);
}

void LibraryView::OnEscape() {
  HandleEscape();
}

LRESULT LibraryView::OnMessage(UINT message,
                               WPARAM wparam,
                               LPARAM lparam,
                               bool* handled) {
  switch (message) {
    case WM_MOUSEMOVE:
      if (press_handle_ >= 0 && (wparam & MK_LBUTTON)) {
        const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const int dx = pt.x - press_point_.x;
        if (!dragging_ && std::abs(dx) > Scaled(kDragThreshold)) {
          BeginDrag(press_handle_);
        }
        if (dragging_) {
          MoveDrag(dx);
          *handled = true;
          return 0;
        }
      }
      break;
    case WM_LBUTTONDBLCLK: {
      // The second press: a pin or bookmark opens, a finished download
      // opens its file.
      if (flying_) {
        break;
      }
      const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const int hit = HitTest(pt);
      *handled = true;
      if (hit < 0) {
        return 0;
      }
      const int key = hits_[static_cast<size_t>(hit)].key;
      const int card = CardOf(key);
      const int index = IndexOf(key);
      if (KindOf(key) == kHitCardPin && card >= 0 &&
          card < static_cast<int>(cards_.size()) && index >= 0 &&
          index < static_cast<int>(cards_[card].pins.size())) {
        const std::string space_id = cards_[card].space.id;
        const PhiStoredRowView item = cards_[card].pins[static_cast<size_t>(index)];
        OpenItem(space_id, item);
      } else if (KindOf(key) == kHitCardRow && card >= 0 &&
                 card < static_cast<int>(cards_.size()) && index >= 0 &&
                 index < static_cast<int>(cards_[card].rows.size())) {
        const std::string space_id = cards_[card].space.id;
        const PhiStoredRowView item =
            cards_[card].rows[static_cast<size_t>(index)].row;
        if (!item.folder) {
          OpenItem(space_id, item);
        }
      } else if (KindOf(key) == kHitDownloadRow && index >= 0 &&
                 index < static_cast<int>(filtered_.size())) {
        const PhiDownloadRow& row = downloads_[filtered_[static_cast<size_t>(index)]];
        phi_framework::PhiFramework* framework = shell_->framework();
        const BrowserId browser = shell_->active_browser_id();
        if (PhiDownloadCanOpen(row) && framework && browser.valid()) {
          framework->DownloadAction(browser.value,
                                    static_cast<uint32_t>(row.id), "open");
        }
      }
      return 0;
    }
    case WM_RBUTTONUP: {
      if (flying_) {
        break;
      }
      const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const int hit = HitTest(pt);
      *handled = true;
      if (hit < 0) {
        return 0;
      }
      const int key = hits_[static_cast<size_t>(hit)].key;
      switch (KindOf(key)) {
        case kHitCardPin:
          ShowItemMenu(CardOf(key), /*pin=*/true, IndexOf(key), pt);
          break;
        case kHitCardRow:
          ShowItemMenu(CardOf(key), /*pin=*/false, IndexOf(key), pt);
          break;
        case kHitCardBody:
          ShowBodyMenu(CardOf(key), pt);
          break;
        default:
          break;
      }
      return 0;
    }
    case WM_COMMAND:
      if (reinterpret_cast<HWND>(lparam) == search_ &&
          HIWORD(wparam) == EN_CHANGE) {
        const int length = ::GetWindowTextLengthW(search_);
        query_.resize(static_cast<size_t>(length) + 1);
        ::GetWindowTextW(search_, query_.data(), length + 1);
        query_.resize(static_cast<size_t>(length));
        FilterDownloads();
        Relayout();
        Invalidate();
        *handled = true;
        return 0;
      }
      if (reinterpret_cast<HWND>(lparam) == rename_ && rename_ &&
          HIWORD(wparam) == EN_KILLFOCUS) {
        EndRename(/*commit=*/true);
        *handled = true;
        return 0;
      }
      break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
      // The fields paint on the grounds their frames are drawn with: the
      // search inside its pill (FieldFill), the rename on its card, in that
      // surface's own label ink. WM_CTLCOLORSTATIC is what an EDIT asks
      // with while read-only or disabled — answered the same.
      const HWND field = reinterpret_cast<HWND>(lparam);
      if (!field || (field != search_ && field != rename_)) {
        break;  // Not ours (the create card answers its own field).
      }
      HDC dc = reinterpret_cast<HDC>(wparam);
      // The ONE stored value the frame was painted with (search_fill_ is
      // the pill's fill, rename_fill_ the card's ground); FieldProc erases
      // the control with the same value.
      const COLORREF ground = FieldColorFor(field);
      COLORREF ink = QueryPalette().label;
      if (field == rename_ && rename_card_ >= 0 &&
          rename_card_ < static_cast<int>(cards_.size())) {
        ink = cards_[rename_card_].palette.label;
      }
      ::SetBkColor(dc, ground);
      ::SetTextColor(dc, ink);
      if (field_brush_color_ != ground || !field_brush_) {
        if (field_brush_) {
          ::DeleteObject(field_brush_);
        }
        field_brush_ = ::CreateSolidBrush(ground);
        field_brush_color_ = ground;
      }
      *handled = true;
      return reinterpret_cast<LRESULT>(field_brush_);
    }
    case kMsgEndCreation:
      CancelCreation();
      *handled = true;
      return 0;
    case kMsgTakeFocus:
      TakeFocus();
      *handled = true;
      return 0;
    case WM_DESTROY:
      if (field_brush_) {
        ::DeleteObject(field_brush_);
        field_brush_ = nullptr;
      }
      break;
    default:
      break;
  }
  return PhiPanel::OnMessage(message, wparam, lparam, handled);
}

// static
LRESULT CALLBACK LibraryView::FieldProc(HWND hwnd,
                                        UINT msg,
                                        WPARAM w,
                                        LPARAM l) {
  auto* self = reinterpret_cast<LibraryView*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  WNDPROC base = ::DefWindowProcW;
  if (self) {
    base = hwnd == self->search_ ? self->search_base_proc_
                                 : self->rename_base_proc_;
    if (!base) {
      base = ::DefWindowProcW;
    }
  }
  switch (msg) {
    case WM_PAINT:
      // The field's interior is OUR value (the pill's fill, the card's
      // ground), painted with opaque alpha: the shell's DWM frame extends
      // under the Library, so the EDIT's own GDI output (alpha 0) would get
      // the backdrop added to it (phi_opaque_edit.h).
      if (self && PhiPaintEditOpaque(hwnd, base, self->FieldColorFor(hwnd),
                                     hwnd == self->search_
                                         ? self->search_surface_
                                         : self->rename_surface_)) {
        return 0;
      }
      break;
    case WM_ERASEBKGND:
      if (self) {
        return 1;  // WM_PAINT supplies the complete opaque interior.
      }
      break;
    case WM_KEYDOWN:
      if (self && hwnd == self->rename_) {
        if (w == VK_ESCAPE) {
          self->EndRename(/*commit=*/false);
          return 0;
        }
        if (w == VK_RETURN) {
          self->EndRename(/*commit=*/true);
          return 0;
        }
      }
      if (self && hwnd == self->search_ && w == VK_ESCAPE) {
        // Escape in the search: clear it first, then leave to the view.
        if (::GetWindowTextLengthW(hwnd) > 0) {
          ::SetWindowTextW(hwnd, L"");
          return 0;
        }
        ::SetFocus(self->hwnd());
        if (self->host_) {
          self->host_->ViewRequestsDismiss();
        }
        return 0;
      }
      break;
    case WM_CHAR:
      if (w == VK_RETURN || w == VK_ESCAPE) {
        return 0;  // No bell for the keys the form consumed.
      }
      break;
    case WM_NCDESTROY:
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      break;
    default:
      break;
  }
  // The handlers above may have destroyed the field; `base` was read first.
  // The EDIT also draws synchronously while typing or selecting: redraw that
  // output through the opaque path once the native operation has run.
  const bool repaint = self && PhiEditDrawsDirectly(msg, w);
  const LRESULT result = ::CallWindowProcW(base, hwnd, msg, w, l);
  if (repaint && ::IsWindow(hwnd)) {
    ::InvalidateRect(hwnd, nullptr, FALSE);
  }
  return result;
}

// ---------------------------------------------------------------------------
// The scrim: a layered child over the shell's client area (sidebar and
// content alike, below the caption band), black at 28 %, whose alpha the
// open/close flights ramp. A click on it dismisses the Library; it never
// takes the focus. SELF-OWNED like the view.
class LibraryScrim : public PhiPanel {
 public:
  class Owner {
   public:
    virtual void ScrimClicked() = 0;
    virtual void ScrimDestroyed() = 0;

   protected:
    ~Owner() = default;
  };

  LibraryScrim(HWND parent, Owner* owner) : owner_(owner) {
    ClassSpec spec;
    spec.name = L"PhiLibraryScrim";
    spec.cursor = IDC_ARROW;
    Options options;
    options.layered = true;
    options.ex_style = WS_EX_NOPARENTNOTIFY;
    const RECT nothing = {0, 0, 0, 0};
    if (!CreatePanel(spec, parent, nothing, options)) {
      return;
    }
    DeleteWhenDestroyed();
  }
  ~LibraryScrim() override = default;

  // 0..1 of the resting 28 %.
  void SetOpacity(double fraction) {
    opacity_ = std::clamp(fraction, 0.0, 1.0);
    ComposeLayer();
  }

  void SetBounds(const RECT& bounds) {
    SyncDpiFromParent();
    SetPanelBounds(bounds);
    ComposeLayer();
  }

 protected:
  void ComposeLayer() override {
    if (!alive()) {
      return;
    }
    RECT rc = {};
    ::GetClientRect(hwnd(), &rc);
    const int w = std::max(1, static_cast<int>(rc.right - rc.left));
    const int h = std::max(1, static_cast<int>(rc.bottom - rc.top));
    PhiLayerSurface surface(w, h);
    if (!surface.valid()) {
      return;
    }
    // Premultiplied black: only the alpha channel carries anything.
    const BYTE alpha = static_cast<BYTE>(std::lround(255 * 0.28 * opacity_));
    const uint32_t value = static_cast<uint32_t>(alpha) << 24;
    // The surface owns w*h premultiplied BGRA pixels (PhiLayerSurface).
    const base::span<uint32_t> pixels = UNSAFE_BUFFERS(base::span<uint32_t>(
        static_cast<uint32_t*>(surface.bits()), static_cast<size_t>(w) * h));
    std::ranges::fill(pixels, value);
    surface.Commit(hwnd(), nullptr, 255);
  }

  void OnMouseDown(int, POINT) override {
    if (owner_) {
      owner_->ScrimClicked();
    }
  }

  void OnNcDestroy() override {
    if (owner_) {
      owner_->ScrimDestroyed();
    }
    PhiPanel::OnNcDestroy();
  }

 private:
  RAW_PTR_EXCLUSION Owner* owner_;
  double opacity_ = 0;
};

// ---------------------------------------------------------------------------
// The overlay: the scrim and the view over one shell, plus the flight in
// and out. Not a window itself; owned by the PhiLibrary statics (the open
// one in g_overlay, closing ones in g_dying until their fade lands).
class LibraryOverlay final : public LibraryViewHost,
                             public LibraryScrim::Owner {
 public:
  LibraryOverlay(PhiShellWindow* shell,
                 PhiLibrarySection section,
                 const RECT* source_screen)
      : shell_(shell) {
    scrim_ = new LibraryScrim(shell_->shell_hwnd(), this);
    view_ = new LibraryView(shell_, shell_->shell_hwnd(), this);
    if (!scrim_->alive() || !view_->alive()) {
      return;
    }
    open_space_ = shell_->active_space();
    open_tab_ = shell_->active_tab_id();
    if (source_screen) {
      source_ = *source_screen;
      ::MapWindowPoints(nullptr, shell_->shell_hwnd(),
                        reinterpret_cast<POINT*>(&source_), 2);
      has_source_ = true;
    }
    RECT overlay = {}, card = {};
    ComputeRects(&overlay, &card);
    scrim_->SetBounds(overlay);
    scrim_->SetOpacity(0);
    scrim_->ShowPanel();
    view_->SetSection(section, /*force_event=*/true);
    const bool animate = !ReduceMotion();
    view_->SetBounds(animate ? FlightRect(card, 0) : card, animate);
    view_->Present();
    if (animate) {
      flight_ = PhiAnimator::ForThread().Animate(
          scrim_->hwnd(), kOpenMs, PhiEasing::kEaseOut,
          [this](double t, RECT*) {
            if (scrim_) {
              // The scrim lands first (160 of the 200 ms).
              scrim_->SetOpacity(std::min(1.0, t * kOpenMs / kCloseMs));
            }
            if (view_) {
              RECT ov = {}, c = {};
              ComputeRects(&ov, &c);
              view_->SetBounds(FlightRect(c, t), /*flying=*/true);
            }
          },
          [this]() {
            flight_ = 0;
            if (scrim_) {
              scrim_->SetOpacity(1);
            }
            if (view_) {
              RECT ov = {}, c = {};
              ComputeRects(&ov, &c);
              view_->SetBounds(c, /*flying=*/false);
              view_->Present();
            }
          });
    } else {
      scrim_->SetOpacity(1);
    }
    shell_->ScheduleRefresh();  // The address row drops the page colour.
  }

  ~LibraryOverlay() = default;

  bool valid() const { return scrim_ && view_ && view_->alive(); }
  PhiShellWindow* shell() const { return shell_; }
  bool closing() const { return closing_; }
  HWND view_hwnd() const { return view_ ? view_->hwnd() : nullptr; }

  void SetSection(PhiLibrarySection section) {
    if (view_) {
      view_->SetSection(section, /*force_event=*/false);
    }
  }
  PhiLibrarySection section() const {
    return view_ ? view_->section() : PhiLibrarySection::kDownloads;
  }

  void Reposition() {
    if (closing_ || !valid()) {
      return;
    }
    RECT overlay = {}, card = {};
    ComputeRects(&overlay, &card);
    scrim_->SetBounds(overlay);
    if (!flight_) {
      view_->SetBounds(card, /*flying=*/false);
    }
  }

  // The shell's rows rebuilt. A changed active tab or Space closes the
  // overlay unanimated (the Mac's focusingTab subscription: the switch
  // owns the focus). Returns false when the overlay closed.
  bool Refresh() {
    if (closing_ || !valid()) {
      return false;
    }
    if (shell_->active_space() != open_space_ ||
        shell_->active_tab_id() != open_tab_) {
      return false;
    }
    view_->Refresh();
    return true;
  }

  void Raise() {
    if (scrim_ && scrim_->alive()) {
      scrim_->RaisePanel();
    }
    if (view_ && view_->alive()) {
      view_->RaisePanel();
    }
  }

  bool ConsumesEscape() const { return view_ && view_->ConsumesEscape(); }
  // Escape for this overlay (see LibraryView::HandleEscape). True when
  // there was a view to take it.
  bool HandleEscape() {
    if (!view_ || closing_) {
      return false;
    }
    view_->HandleEscape();
    return true;
  }
  bool HandleFind() { return view_ && view_->HandleFind(); }

  // Fades out (160 ms, the card flying back to its source) and frees
  // itself; `animated` false tears down at once.
  void BeginClose(bool animated) {
    if (closing_) {
      if (!animated) {
        Finish();
      }
      return;
    }
    closing_ = true;
    if (flight_) {
      PhiAnimator::ForThread().Cancel(flight_);
      flight_ = 0;
    }
    if (shell_ && !shell_->closing()) {
      shell_->ScheduleRefresh();  // The page colour comes back.
      // The focus goes back to the page, as the Mac restores the
      // previous responder.
      shell_->FocusActiveBrowser();
    }
    if (!animated || ReduceMotion() || !valid()) {
      Finish();
      return;
    }
    RECT overlay = {}, card = {};
    ComputeRects(&overlay, &card);
    view_->SetBounds(card, /*flying=*/true);
    flight_ = PhiAnimator::ForThread().Animate(
        scrim_->hwnd(), kCloseMs, PhiEasing::kEaseIn,
        [this, card](double t, RECT*) {
          if (scrim_) {
            scrim_->SetOpacity(1 - t);
          }
          if (view_) {
            view_->SetBounds(FlightRect(card, 1 - t), /*flying=*/true);
          }
        },
        [this]() {
          flight_ = 0;
          Finish();
        });
  }

  // Tears the windows down now and frees the overlay. The panels are
  // self-owned: Destroy() frees each at its WM_NCDESTROY, which reports
  // back through ScrimDestroyed / ViewDestroyed and nulls the pointer.
  void Finish();

  // LibraryViewHost.
  void ViewRequestsDismiss() override { PhiLibrary::Close(); }
  bool ViewIsStandalone() const override { return false; }
  void ViewDestroyed(LibraryView* view) override {
    if (view_ == view) {
      view_ = nullptr;
    }
  }
  // LibraryScrim::Owner.
  void ScrimClicked() override { PhiLibrary::Close(); }
  void ScrimDestroyed() override { scrim_ = nullptr; }

 private:
  static bool ReduceMotion() {
    BOOL animate = TRUE;
    ::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
    return !animate;
  }

  // The overlay covers the shell's client area below the caption band
  // (the Mac's panel covers the content view; the title bar's buttons stay
  // reachable); the card is that inset by 36.
  void ComputeRects(RECT* overlay, RECT* card) const {
    RECT client = {};
    ::GetClientRect(shell_->shell_hwnd(), &client);
    *overlay = client;
    overlay->top = std::min<LONG>(client.bottom, client.top +
                                                      shell_->title_bar_height());
    const int inset = Scaled(kCardInset);
    *card = *overlay;
    ::InflateRect(card, -inset, -inset);
    if (card->right <= card->left || card->bottom <= card->top) {
      *card = *overlay;
    }
  }

  int Scaled(int dip) const {
    const UINT dpi = ::GetDpiForWindow(shell_->shell_hwnd());
    return static_cast<int>((dip * (dpi ? dpi : 96) + 48) / 96);
  }

  // The card at progress `t` of its flight: from a rect scaled to at most
  // 12 % (at least 2 %) centred on the source control to the full card
  // (LibraryOverlayController.flightTransform).
  RECT FlightRect(const RECT& card, double t) const {
    const int cw = std::max(1, static_cast<int>(card.right - card.left));
    const int ch = std::max(1, static_cast<int>(card.bottom - card.top));
    double scale = 0.12;
    POINT centre = {(card.left + card.right) / 2,
                    (card.top + card.bottom) / 2};
    if (has_source_) {
      const double sw = std::max<LONG>(1, source_.right - source_.left);
      const double sh = std::max<LONG>(1, source_.bottom - source_.top);
      scale = std::clamp(std::min(sw / cw, sh / ch), 0.02, 0.12);
      centre = {(source_.left + source_.right) / 2,
                (source_.top + source_.bottom) / 2};
    }
    const double s = scale + (1 - scale) * t;
    const int w = std::max(1, static_cast<int>(std::lround(cw * s)));
    const int h = std::max(1, static_cast<int>(std::lround(ch * s)));
    const int cx = PhiLerpInt(centre.x, (card.left + card.right) / 2, t);
    const int cy = PhiLerpInt(centre.y, (card.top + card.bottom) / 2, t);
    return {cx - w / 2, cy - h / 2, cx - w / 2 + w, cy - h / 2 + h};
  }

  RAW_PTR_EXCLUSION PhiShellWindow* const shell_;
  RAW_PTR_EXCLUSION LibraryScrim* scrim_ = nullptr;
  RAW_PTR_EXCLUSION LibraryView* view_ = nullptr;
  std::string open_space_;
  TabId open_tab_;
  RECT source_{};
  bool has_source_ = false;
  bool closing_ = false;
  int flight_ = 0;
};

LibraryOverlay* g_overlay = nullptr;
// Closing overlays, alive until their fade lands.
std::vector<LibraryOverlay*>& Dying() {
  static base::NoDestructor<std::vector<LibraryOverlay*>> dying;
  return *dying;
}

void LibraryOverlay::Finish() {
  if (flight_) {
    PhiAnimator::ForThread().Cancel(flight_);
    flight_ = 0;
  }
  auto dying = std::find(Dying().begin(), Dying().end(), this);
  if (dying != Dying().end()) {
    Dying().erase(dying);
  }
  if (g_overlay == this) {
    g_overlay = nullptr;
  }
  // Destroying frees each panel at WM_NCDESTROY, which nulls our pointer
  // through the owner callbacks; a panel that never got a window is freed
  // here.
  if (LibraryView* view = view_) {
    view_ = nullptr;
    if (view->alive()) {
      view->Destroy();
    } else {
      delete view;
    }
  }
  if (LibraryScrim* scrim = scrim_) {
    scrim_ = nullptr;
    if (scrim->alive()) {
      scrim->Destroy();
    } else {
      delete scrim;
    }
  }
  delete this;
}

// ---------------------------------------------------------------------------
// The standalone Library window (LibraryWindowController): 1120x780, at
// least 840x560, no title text, hosting the same view; one at a time, bound
// to the shell whose session it serves. Its frame is remembered for the
// process (the Mac's frame autosave; no preference exists for it here).
class LibraryWindow : public PhiSecondaryWindow, public LibraryViewHost {
 public:
  static LibraryWindow* instance;
  static std::optional<RECT> remembered_frame;

  explicit LibraryWindow(PhiShellWindow* shell) : shell_(shell) {}
  ~LibraryWindow() override = default;

  PhiShellWindow* shell() const { return shell_; }

  bool Open(PhiLibrarySection section) {
    ClassSpec spec;
    spec.name = L"PhiLibraryWindow";
    Options options;
    options.caption_buttons =
        kPhiCaptionClose | kPhiCaptionMinimize | kPhiCaptionMaximize;
    options.show_mark = false;  // No title, as the Mac's transparent bar.
    options.dialog_keys = kPhiDialogNone;
    RECT bounds = {CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT + 100,
                   CW_USEDEFAULT + 100};
    if (!CreateSecondary(spec, 0,
                         WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, bounds,
                         nullptr, L"", options)) {
      return false;
    }
    bg_brush_ = ::CreateSolidBrush(colors().bg);
    if (remembered_frame) {
      const RECT frame = *remembered_frame;
      ::SetWindowPos(hwnd(), nullptr, frame.left, frame.top,
                     frame.right - frame.left, frame.bottom - frame.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      FitToWorkArea();
    } else {
      const SIZE outer = chrome().OuterSize(kWidthDip, kHeightDip);
      ::SetWindowPos(hwnd(), nullptr, 0, 0, outer.cx, outer.cy,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      CenterOn(shell_->shell_hwnd());
    }
    view_ = new LibraryView(shell_, hwnd(), this);
    if (!view_->alive()) {
      delete view_;
      view_ = nullptr;
      return false;
    }
    view_->SetSection(section, /*force_event=*/true);
    view_->SetBounds(ContentRect(), /*flying=*/false);
    ::ShowWindow(hwnd(), SW_SHOW);
    view_->Present();
    ::SetForegroundWindow(hwnd());
    return true;
  }

  void Front(PhiLibrarySection section) {
    if (view_) {
      view_->SetSection(section, /*force_event=*/false);
    }
    if (::IsIconic(hwnd())) {
      ::ShowWindow(hwnd(), SW_RESTORE);
    }
    ::SetForegroundWindow(hwnd());
  }

  void RefreshView() {
    if (view_) {
      view_->Refresh();
    }
  }

  // LibraryViewHost.
  void ViewRequestsDismiss() override {
    // The standalone window stays; the browser it serves comes forward
    // (the Mac's module.onDismiss → parent.makeKeyAndOrderFront).
    if (shell_ && shell_->shell_hwnd()) {
      ::SetForegroundWindow(shell_->shell_hwnd());
    }
  }
  bool ViewIsStandalone() const override { return true; }
  void ViewDestroyed(LibraryView* view) override {
    if (view_ == view) {
      view_ = nullptr;
    }
  }

 protected:
  void Paint(HDC dc, const RECT&) override {
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    ::FillRect(dc, &client, bg_brush_);
    PaintBand(dc, L"");
  }

  LRESULT OnWindowMessage(UINT msg,
                          WPARAM wparam,
                          LPARAM lparam,
                          bool* handled) override {
    switch (msg) {
      case WM_SIZE:
        if (alive() && view_) {
          view_->SetBounds(ContentRect(), /*flying=*/false);
        }
        break;
      case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        const SIZE min = chrome().OuterSize(kMinWidthDip, kMinHeightDip);
        info->ptMinTrackSize.x = min.cx;
        info->ptMinTrackSize.y = min.cy;
        *handled = true;
        return 0;
      }
      case WM_CLOSE:
        RememberFrame();
        Destroy();
        *handled = true;
        return 0;
      default:
        break;
    }
    return PhiSecondaryWindow::OnWindowMessage(msg, wparam, lparam, handled);
  }

  void OnColorsChanged() override {
    if (bg_brush_) {
      ::DeleteObject(bg_brush_);
    }
    bg_brush_ = ::CreateSolidBrush(colors().bg);
    Invalidate();
  }

  void OnNcDestroy() override {
    RememberFrame();
    if (view_) {
      LibraryView* view = view_;
      view_ = nullptr;
      if (view->alive()) {
        view->Destroy();  // Self-owned: freed at its WM_NCDESTROY.
      } else {
        delete view;
      }
    }
    if (bg_brush_) {
      ::DeleteObject(bg_brush_);
      bg_brush_ = nullptr;
    }
    if (instance == this) {
      instance = nullptr;
    }
    PhiSecondaryWindow::OnNcDestroy();
  }

 private:
  static constexpr int kWidthDip = 1120;
  static constexpr int kHeightDip = 780;
  static constexpr int kMinWidthDip = 840;
  static constexpr int kMinHeightDip = 560;

  void RememberFrame() {
    if (!alive() || ::IsIconic(hwnd())) {
      return;
    }
    WINDOWPLACEMENT placement = {sizeof(placement)};
    if (::GetWindowPlacement(hwnd(), &placement)) {
      remembered_frame = placement.rcNormalPosition;
    }
  }

  RAW_PTR_EXCLUSION PhiShellWindow* const shell_;
  RAW_PTR_EXCLUSION LibraryView* view_ = nullptr;
  HBRUSH bg_brush_ = nullptr;
};

LibraryWindow* LibraryWindow::instance = nullptr;
std::optional<RECT> LibraryWindow::remembered_frame;

}  // namespace

// ---------------------------------------------------------------------------
// PhiLibrary.

// static
void PhiLibrary::Open(PhiShellWindow* shell, PhiLibrarySection section) {
  OpenFrom(shell, section, nullptr);
}

// static
void PhiLibrary::OpenFrom(PhiShellWindow* shell,
                          PhiLibrarySection section,
                          const RECT* source_screen) {
  if (!shell || !shell->shell_hwnd() || shell->closing()) {
    return;
  }
  LastSections()[shell] = section;
  if (g_overlay && g_overlay->shell() == shell && !g_overlay->closing()) {
    g_overlay->SetSection(section);  // Re-sectioning the open one.
    g_overlay->Raise();
    return;
  }
  Close();
  // The ⋯ button is the default source: the row the user picked lives
  // there, and the avatar / Ctrl+Shift+L have no better anchor.
  RECT source = {};
  if (!source_screen) {
    source = shell->app_menu_rect();
    ::MapWindowPoints(shell->shell_hwnd(), nullptr,
                      reinterpret_cast<POINT*>(&source), 2);
    source_screen = &source;
  }
  auto* overlay = new LibraryOverlay(shell, section, source_screen);
  if (!overlay->valid()) {
    overlay->Finish();  // Frees it.
    return;
  }
  g_overlay = overlay;
}

// static
void PhiLibrary::Toggle(PhiShellWindow* shell) {
  if (!shell) {
    return;
  }
  if (g_overlay && g_overlay->shell() == shell && !g_overlay->closing()) {
    Close();
    return;
  }
  auto found = LastSections().find(shell);
  Open(shell, found == LastSections().end() ? PhiLibrarySection::kDownloads
                                            : found->second);
}

// static
void PhiLibrary::Close() {
  if (LibraryOverlay* gone = g_overlay) {
    // Released from the singleton BEFORE the teardown starts: IsOpen() is
    // false from here, and the fade finishes on its own (g_dying).
    g_overlay = nullptr;
    Dying().push_back(gone);
    gone->BeginClose(/*animated=*/true);
  }
}

// static
bool PhiLibrary::IsOpen() {
  return g_overlay != nullptr;
}

// static
void PhiLibrary::CloseIfBoundTo(PhiShellWindow* shell) {
  if (g_overlay && g_overlay->shell() == shell) {
    LibraryOverlay* gone = g_overlay;
    g_overlay = nullptr;
    gone->BeginClose(/*animated=*/false);  // Finishes and frees.
  }
  // Fades still running over this shell end now: their panels go with the
  // shell's HWND, and nothing may run against them afterwards.
  for (size_t i = 0; i < Dying().size();) {
    if (Dying()[i]->shell() == shell) {
      Dying()[i]->Finish();  // Erases itself from Dying().
    } else {
      ++i;
    }
  }
  if (LibraryWindow::instance && LibraryWindow::instance->shell() == shell) {
    LibraryWindow::instance->Destroy();
  }
  LastSections().erase(shell);
}

// static
void PhiLibrary::RefreshIfBoundTo(PhiShellWindow* shell) {
  if (g_overlay && g_overlay->shell() == shell && !g_overlay->Refresh()) {
    // The active tab or Space moved under the Library: close unanimated.
    LibraryOverlay* gone = g_overlay;
    g_overlay = nullptr;
    gone->BeginClose(/*animated=*/false);
  }
  if (LibraryWindow::instance && LibraryWindow::instance->shell() == shell) {
    LibraryWindow::instance->RefreshView();
  }
}

// static
void PhiLibrary::RepositionIfBoundTo(PhiShellWindow* shell) {
  if (g_overlay && g_overlay->shell() == shell) {
    g_overlay->Reposition();
  }
}

// static
void PhiLibrary::RaiseIfBoundTo(PhiShellWindow* shell) {
  if (g_overlay && g_overlay->shell() == shell) {
    g_overlay->Raise();
  }
}

// static
HWND PhiLibrary::HwndIfBoundTo(PhiShellWindow* shell) {
  if (g_overlay && g_overlay->shell() == shell && !g_overlay->closing()) {
    return g_overlay->view_hwnd();
  }
  return nullptr;
}

// static
bool PhiLibrary::HandleEscapeIfBoundTo(PhiShellWindow* shell) {
  if (!g_overlay || g_overlay->shell() != shell || g_overlay->closing()) {
    return false;
  }
  if (PhiModalDepth() > 0 || PhiMenuIsTracking()) {
    // A popup menu (a row's, a card's "…", the ⋯ app menu), a prompt or a
    // picker is pumping its own loop over the Library: Escape is theirs.
    // The hook asks before they see the key, because a menu never takes
    // the focus away from the shell.
    return false;
  }
  // The view decides: a drag / rename / create card in flight cancels, a
  // filled search clears, otherwise the overlay closes — the same answer
  // the view gives its own Escape key.
  return g_overlay->HandleEscape();
}

// static
bool PhiLibrary::HandleFindIfBoundTo(PhiShellWindow* shell) {
  if (!g_overlay || g_overlay->shell() != shell || g_overlay->closing()) {
    return false;
  }
  return g_overlay->HandleFind();
}

// static
void PhiLibrary::OpenInNewWindow(PhiShellWindow* shell,
                                 PhiLibrarySection section) {
  if (!shell || !shell->shell_hwnd()) {
    return;
  }
  if (LibraryWindow::instance && LibraryWindow::instance->alive()) {
    if (LibraryWindow::instance->shell() == shell) {
      LibraryWindow::instance->Front(section);
      return;
    }
    // Another window's session has it: the new request takes over, as
    // the Mac's shared window opens for the active window.
    LibraryWindow::instance->Destroy();
  }
  auto* window = new LibraryWindow(shell);
  if (!window->Open(section)) {
    delete window;
    return;
  }
  window->DeleteWhenDestroyed();  // Self-owned from here.
  LibraryWindow::instance = window;
}

}  // namespace phi_client

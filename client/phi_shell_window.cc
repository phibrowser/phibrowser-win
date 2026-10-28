// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_features.h"
#include "phibrowser-win/client/phi_shell_window.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

// dwmapi.h reaches rpcndr.h, which #defines `small` — this file uses it as
// an identifier.
#undef small

#include <algorithm>
#include <functional>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/auto_reset.h"
#include "base/base64.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_agent_overlay.h"
#include "phibrowser-win/client/phi_agent_spaces.h"
#include "phibrowser-win/client/phi_downloads_popover.h"
#include "phibrowser-win/client/phi_library.h"
#include "phibrowser-win/client/phi_extensions_popover.h"
#include "phibrowser-win/client/phi_group_overview.h"
#include "phibrowser-win/client/phi_library.h"
#include "phibrowser-win/client/phi_search_tabs_panel.h"
#include "phibrowser-win/client/phi_space_theme_panel.h"
#include "phibrowser-win/client/phi_bookmark_manager.h"
#include "phibrowser-win/client/phi_backdrop.h"
#include "phibrowser-win/client/phi_client_analytics.h"
#include "phibrowser-win/client/phi_create_space_panel.h"
#include "phibrowser-win/client/phi_create_space_flyout.h"
#include "phibrowser-win/client/phi_create_space_window.h"
#include "phibrowser-win/client/phi_folder_icon_picker.h"
#include "phibrowser-win/client/phi_icon_picker.h"
#include "phibrowser-win/client/phi_folder_icons.h"
#include "phibrowser-win/client/phi_tab_preview.h"
#include "phibrowser-win/client/phi_tab_switcher.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_mac_icons.h"
#include "phibrowser-win/client/phi_agent_icons.h"
#include "phibrowser-win/client/phi_app_menu.h"
#include "phibrowser-win/client/phi_fluent.h"
#include "phibrowser-win/client/phi_menu_paint.h"
#include "phibrowser-win/client/phi_modal_scope.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_new_tab_page.h"
#include "phibrowser-win/client/phi_notification_cards.h"
#include "phibrowser-win/client/phi_reader.h"
#include "phibrowser-win/client/phi_reveal_veil.h"
#include "phibrowser-win/client/phi_uia.h"
#include "phibrowser-win/client/phi_updater.h"
#include "phibrowser-win/client/phi_prompt.h"

namespace phi_client {

namespace {

constexpr wchar_t kShellClassName[] = L"PhiClientShell";

using phi_framework::SplitSnapshot;
using phi_framework::TabSnapshot;
using phi_framework::WindowSnapshot;

// --- Metrics -----------------------------------------------------------------
// Design units (points at 100%), taken from the Mac client so the two look the
// same: WebContentConstant, SidebarHeaderView, SpacesStripView,
// PinnedTabLayout, TabStripMetrics and SidebarBottomBar. The ones the
// geometry is built from live in phi_shell_layout.h (the pure, host-tested
// layout reads them); the paint-only ones stay here.
using namespace shell_metrics;
// (The menu titles' padding metrics moved with them, into
// phi_caption_band.cc.)
// The selected card's corner RADIUS — and it is passed DOUBLED to the
// draws: GDI's RoundRect takes the corner's ellipse, not its radius, so
// every rounded rect in this file that hands its radius straight over is
// rounding half as hard as the Mac constant it is named for. The Mac draws
// 8 on its 24pt chip (a third of the side) as a `.continuous` squircle;
// 9 of real radius stands in for that on a circular arc. It is also about
// as far as the corner goes: the colour pass clears the glyph box whole,
// and a rounder card would have the box's own corners poking out of it and
// squaring it back off.
// ONE size per pip: what a Space's ICON measures, whichever medium it comes
// from — catalog artwork, colour emoji, or an MDL2 stand-in. Each medium
// needs a different number to land there (an imageset pads its canvas; a
// font's em is bigger than its ink, by a different amount per glyph), so
// the draws take the ink size and work the number out per glyph rather than
// the call site guessing one for all of them. The Mac carries three
// constants that all come out at ~15pt of ink in its 24pt strip item
// (phiIconSize 24, emojiFontSize 13 — "approximately 15pt visible" — and
// SF-symbol iconSize 14); this is that invariant, kept directly.
constexpr int kSpaceIcon = 13;
// The big pip keeps the sidebar's own 8pt gutter clear inside its 32pt
// pill, which is what stops a Space's icon from crowding its background.
constexpr int kSpaceIconVertical = 14;
// The strip's TEXT at the larger pip: the first letter of a Space with no
// icon, and the trailing slot's "+" / "…". A letter's ink is shorter than a
// glyph's em box, so it sits a size under the icons rather than matching
// them. (The 24pt pip draws both in the UI font, which is already this
// relation to its 13pt glyphs.)
constexpr int kSpaceTextVertical = 14;
// How far a shell with no placement to inherit sits off the work area's
// top-left corner (see the first-run placement in the constructor).
constexpr int kDefaultPlacementMargin = 24;
// The smallest page the window shrinks to beside a docked sidebar (or
// alone), and the smallest client height: below them the content card's
// rect goes negative and the caption buttons ride over the sidebar.
constexpr int kMinContentWidth = 400;
constexpr int kMinClientHeight = 320;

// A user's name (a Space, a group, a folder) as a menu row's label: the row
// painter reads a single '&' as a mnemonic marker, so a literal one is
// doubled — "R&D" must not show as "RD" with an underlined D.
std::wstring MenuLabel(std::wstring name) {
  for (size_t at = name.find(L'&'); at != std::wstring::npos;
       at = name.find(L'&', at + 2)) {
    name.insert(at, 1, L'&');
  }
  return name;
}

// A command's chord as the ⋯ menu shows it (PhiAppMenu::Label's rule): the
// user's effective binding when `id` is rebindable — empty while unbound —
// else the catalogue string's own "\t<chord>" suffix. For the menus the
// shell builds itself (the profile button's), whose rows carry the chord
// apart from their label.
std::wstring MenuChord(ChromePrefsHost* prefs, UINT id, const char* key) {
  if (prefs) {
    if (const std::optional<std::wstring> chord =
            prefs->RebindableChordLabel(id)) {
      return *chord;
    }
  }
  const std::wstring text = PhiStr(key);
  const size_t tab = text.find(L'\t');
  return tab == std::wstring::npos ? std::wstring() : text.substr(tab + 1);
}
constexpr int kContentRadius = 8;  // The pane's corners (windows-native-ui.md §2.1).
// 0x515 and 0x516 belonged to the Space swap and hover-card clocks; the
// swap now rides PhiAnimator and the card's dwell timer is
// PhiSpacesStrip::kHoverCardTimer (still 0x516), which the strip owns.
// 0x518 is PhiSidebarDragSession::kSlideTimer — the reorder slide, whose
// clock the drag session owns and whose timer rides this window; 0x519 is
// its kHoverExpandTimer (a drag resting on a collapsed folder or group).
constexpr UINT_PTR kFloatingTickTimer = 0x522;
// 0x523 is PhiOmniboxSession::kCopyLinkTimer — the shell routes it back to
// the session, which owns the copy-link flash.
// 0x524 was the Spaces strip's "+" fade; it rides PhiAnimator now.
// Re-arms a kMsgPhiDeferredClose that found a nested loop still on the
// stack (see PhiShellWindow::RequestClose).
constexpr UINT_PTR kDeferredCloseTimer = 0x525;
// The split view's hover chip poll (§4.6).
constexpr UINT_PTR kSplitChipTimer = 0x526;
// The chrome tooltip's dwell (UpdateChromeTip).
constexpr UINT_PTR kChromeTipTimer = 0x527;
constexpr UINT kDeferredCloseRetryMs = 50;

// The row metrics the sidebar owns and the chrome below shares — the pill
// radius, the favicon and close-box sizes, the outline's indent
// (phi_sidebar_view.h). The tab-group and strip-tab constants went with the
// rows they belong to.
using sidebar_metrics::kBookmarkIndent;
using sidebar_metrics::kCloseBoxSize;
using sidebar_metrics::kCornerRadius;
using sidebar_metrics::kFaviconSize;
using sidebar_metrics::kFolderIconSize;
using sidebar_metrics::kRowPad;
// The bottom bar's two capsules (Feedback bordered, Chat on the accent).
// The sidebar bottom bar is the only place Feedback has a band now: the
// comfortable header that carried the labelled variant is gone, and its
// entry moved into the overflow menu. The paint keeps the label branch —
// ViewThatFits picks it as soon as a band is wide enough for it again.

// --- Palette (light) ---------------------------------------------------------
// From Assets.xcassets/ColorSet: primary-label 85% black, common-border 8%
// black, sidebar-tab-selected white. The sidebar ground stands in for the Mac's
// material + per-Space gradient tint, which GDI cannot reproduce.
// Every colour now comes from the active theme (phi_theme.h), resolved by the
// coordinator and read at paint time.

ATOM EnsureShellClass() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    // CS_DBLCLKS: a group header's double-click opens the group overview.
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &PhiShellWindow::ShellWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    // Phi's own icon in the taskbar and alt-tab — not Chromium's.
    wc.hIcon = PhiAppIcon(32);
    wc.hIconSm = PhiAppIcon(16);
    // No background brush: an erase flashes system-white under the chrome
    // before PaintChrome covers it. WM_ERASEBKGND answers "done" instead.
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kShellClassName;
    return ::RegisterClassExW(&wc);
  }();
  return atom;
}

}  // namespace

// The Phi app icon, shipped beside the exe (resources\phi.ico — the Mac's
// own artwork). Sized copies for the taskbar/alt-tab and the caption band.
HICON PhiAppIcon(int size) {
  wchar_t exe_path[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  std::wstring path(exe_path);
  const size_t slash = path.find_last_of(L'\\');
  if (slash == std::wstring::npos) {
    return nullptr;
  }
  path.resize(slash + 1);
  path += L"resources\\phi.ico";
  return reinterpret_cast<HICON>(::LoadImageW(
      nullptr, path.c_str(), IMAGE_ICON, size, size, LR_LOADFROMFILE));
}

// The Phi logo as premultiplied BGRA, extracted ONCE from the ICO's 256px
// entry — shared by the caption band button and the internal-page favicon
// fallback (the D2D path downscales it crisply to any slot).
const std::vector<unsigned char>& PhiLogoBgra256() {
  static base::NoDestructor<std::vector<unsigned char>> store;
  static bool tried = false;
  std::vector<unsigned char>& bgra = *store;
  if (tried) {
    return bgra;
  }
  tried = true;
  HICON big = PhiAppIcon(256);
  if (!big) {
    return bgra;
  }
  ICONINFO info = {};
  if (::GetIconInfo(big, &info) && info.hbmColor) {
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = 256;
    bmi.bmiHeader.biHeight = -256;  // Top-down.
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> straight(256 * 256 * 4);
    HDC screen = ::GetDC(nullptr);
    if (screen && ::GetDIBits(screen, info.hbmColor, 0, 256, straight.data(),
                              &bmi, DIB_RGB_COLORS) == 256) {
      // Straight alpha -> premultiplied, what the D2D path expects.
      for (size_t px = 0; px + 3 < straight.size(); px += 4) {
        const unsigned a = straight[px + 3];
        straight[px] = static_cast<unsigned char>(straight[px] * a / 255);
        straight[px + 1] =
            static_cast<unsigned char>(straight[px + 1] * a / 255);
        straight[px + 2] =
            static_cast<unsigned char>(straight[px + 2] * a / 255);
      }
      bgra = std::move(straight);
    }
    if (screen) {
      ::ReleaseDC(nullptr, screen);
    }
    ::DeleteObject(info.hbmColor);
    if (info.hbmMask) {
      ::DeleteObject(info.hbmMask);
    }
  }
  ::DestroyIcon(big);
  return bgra;
}

int PhiShellWindow::Scaled(int design_units) const {
  return ::MulDiv(design_units, static_cast<int>(dpi_), 96);
}

RECT PhiShellWindow::Scaled(const RECT& design_units) const {
  return {Scaled(design_units.left), Scaled(design_units.top),
          Scaled(design_units.right), Scaled(design_units.bottom)};
}

void PhiShellWindow::EnsureFont() {
  if (ui_font_ && ui_font_dpi_ == dpi_) {
    return;
  }
  // SystemParametersInfoForDpi gives the message font already sized for this
  // monitor; the plain SPI_GETNONCLIENTMETRICS returns the PRIMARY monitor's,
  // which is wrong the moment a window moves to a display at another scale.
  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  HFONT built = nullptr;
  if (::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                   &metrics, 0, dpi_)) {
    // The face tracks the system (Segoe UI), the SIZE is the Mac's: 13pt row
    // titles everywhere (TabContentView.titleFontSize).
    LOGFONTW ui = metrics.lfMessageFont;
    ui.lfHeight = -Scaled(13);
    built = ::CreateFontIndirectW(&ui);
  }
  if (!built) {
    return;  // Keep whatever we had rather than falling back to no font.
  }
  HFONT previous = ui_font_;
  ui_font_ = built;
  ui_font_dpi_ = dpi_;
  if (omnibox_) {
    omnibox_->SetFont(ui_font_);
  }
  if (previous) {
    ::DeleteObject(previous);  // Replaced only after nothing else refers to it.
  }
  // The same face one weight up, for folder titles and group headers. GDI
  // keeps Segoe UI's semibold as a family of its own: asking "Segoe UI" for
  // weight 600 rounds up to Bold, so name the face when that is the system
  // font. Other faces (the CJK UI fonts) synthesize it.
  {
    LOGFONTW semibold = metrics.lfMessageFont;
    semibold.lfHeight = -Scaled(13);
    semibold.lfWeight = FW_SEMIBOLD;
    if (std::wstring_view(semibold.lfFaceName) == L"Segoe UI") {
      const std::wstring face = L"Segoe UI Semibold";
      std::fill(std::begin(semibold.lfFaceName),
                std::end(semibold.lfFaceName), L'\0');
      std::copy_n(face.begin(),
                  std::min(face.size(), std::size(semibold.lfFaceName) - 1),
                  std::begin(semibold.lfFaceName));
    }
    if (HFONT semibold_built = ::CreateFontIndirectW(&semibold)) {
      if (ui_semibold_font_) {
        ::DeleteObject(ui_semibold_font_);
      }
      ui_semibold_font_ = semibold_built;
    }
    // The same face at 8pt for the extension badge pill (the Mac's
    // ExtensionBadge: .system(size: 8, weight: .semibold)).
    semibold.lfHeight = -Scaled(8);
    if (HFONT badge_built = ::CreateFontIndirectW(&semibold)) {
      if (badge_font_) {
        ::DeleteObject(badge_font_);
      }
      badge_font_ = badge_built;
    }
  }

  if (emoji_font_ && emoji_font_dpi_ == dpi_) {
    return;
  }
  LOGFONTW small = metrics.lfMessageFont;
  small.lfHeight = -Scaled(11);
  if (HFONT small_built = ::CreateFontIndirectW(&small)) {
    if (small_font_) {
      ::DeleteObject(small_font_);
    }
    small_font_ = small_built;
  }
  if (HFONT label_built = PhiFluentFont(dpi_, 12)) {
    if (label_font_) {
      ::DeleteObject(label_font_);
    }
    label_font_ = label_built;
  }
  LOGFONTW emoji = metrics.lfMessageFont;
  // The unsafe-libc-call gate rules out wcscpy_s; copy the face name directly.
  const std::wstring emoji_face = L"Segoe UI Emoji";
  std::fill(std::begin(emoji.lfFaceName), std::end(emoji.lfFaceName), L'\0');
  std::copy_n(emoji_face.begin(),
              std::min(emoji_face.size(), std::size(emoji.lfFaceName) - 1),
              std::begin(emoji.lfFaceName));
  emoji.lfHeight = -Scaled(kSpaceIcon);
  HFONT emoji_built = ::CreateFontIndirectW(&emoji);
  if (emoji_built) {
    HFONT previous_emoji = emoji_font_;
    emoji_font_ = emoji_built;
    emoji_font_dpi_ = dpi_;
    if (previous_emoji) {
      ::DeleteObject(previous_emoji);
    }
  }

  if (caption_font_ && caption_font_dpi_ == dpi_) {
    return;
  }
  // The caption glyph font — the OS buttons' own glyphs (E921/E922/E923/E8BB).
  LOGFONTW caption = metrics.lfMessageFont;
  const std::wstring caption_face = PhiIconFaceName();
  std::fill(std::begin(caption.lfFaceName), std::end(caption.lfFaceName),
            L'\0');
  std::copy_n(caption_face.begin(),
              std::min(caption_face.size(),
                       std::size(caption.lfFaceName) - 1),
              std::begin(caption.lfFaceName));
  caption.lfHeight = -Scaled(10);
  HFONT caption_built = ::CreateFontIndirectW(&caption);
  if (caption_built) {
    HFONT previous_caption = caption_font_;
    caption_font_ = caption_built;
    caption_font_dpi_ = dpi_;
    if (previous_caption) {
      ::DeleteObject(previous_caption);
    }
  }
  // The same MDL2 face at the EMOJI size, for Space-pip catalog glyphs — one
  // optical size whether a pip wears an emoji or a Phi icon.
  caption.lfHeight = -Scaled(kSpaceIcon);
  if (HFONT chip_built = ::CreateFontIndirectW(&caption)) {
    if (chip_icon_font_) {
      ::DeleteObject(chip_icon_font_);
    }
    chip_icon_font_ = chip_built;
  }
  // Both pip faces again at the vertical layouts' larger pip size — the GDI
  // fallback there, for a private-use glyph with no catalog artwork and for
  // emoji when DirectWrite is unavailable.
  caption.lfHeight = -Scaled(kSpaceIconVertical);
  if (HFONT pip_icon_built = ::CreateFontIndirectW(&caption)) {
    if (pip_icon_font_) {
      ::DeleteObject(pip_icon_font_);
    }
    pip_icon_font_ = pip_icon_built;
  }
  emoji.lfHeight = -Scaled(kSpaceIconVertical);
  if (HFONT pip_emoji_built = ::CreateFontIndirectW(&emoji)) {
    if (pip_emoji_font_) {
      ::DeleteObject(pip_emoji_font_);
    }
    pip_emoji_font_ = pip_emoji_built;
  }
  LOGFONTW pip_text = metrics.lfMessageFont;
  pip_text.lfHeight = -Scaled(kSpaceTextVertical);
  if (HFONT pip_text_built = ::CreateFontIndirectW(&pip_text)) {
    if (pip_text_font_) {
      ::DeleteObject(pip_text_font_);
    }
    pip_text_font_ = pip_text_built;
  }
}

void PhiShellWindow::RebuildAppMenu() {
  if (!shell_hwnd_ || !::IsWindow(shell_hwnd_)) {
    return;
  }
  app_menu_ = std::make_unique<PhiAppMenu>(static_cast<PhiAppMenuShell*>(this));  // The old tree goes.
  app_menu_->Install(shell_hwnd_);
  ::InvalidateRect(shell_hwnd_, nullptr, TRUE);
}

// ShellLayout → the RECT-based Layout the paint and hit-test code reads.
// static
PhiShellWindow::Layout PhiShellWindow::ToLayout(
    const ShellLayout& shell) {
  Layout out;
  out.mode = shell.mode;
  out.client = PhiToRect(shell.client);
  out.title_bar = PhiToRect(shell.title_bar);
  out.caption_min = PhiToRect(shell.caption_min);
  out.caption_max = PhiToRect(shell.caption_max);
  out.caption_close = PhiToRect(shell.caption_close);
  out.sidebar = PhiToRect(shell.sidebar);
  out.top_bar = PhiToRect(shell.top_bar);
  out.content = PhiToRect(shell.content);
  out.bookmark_bar = PhiToRect(shell.bookmark_bar);
  out.app_icon = PhiToRect(shell.app_icon);
  out.search_tabs = PhiToRect(shell.search_tabs);
  out.update = PhiToRect(shell.update);
  out.band_divider = PhiToRect(shell.band_divider);
  out.back = PhiToRect(shell.back);
  out.forward = PhiToRect(shell.forward);
  out.reload = PhiToRect(shell.reload);
  out.spaces_strip = PhiToRect(shell.spaces_strip);
  out.url_field = PhiToRect(shell.url_field);
  out.favorites = PhiToRect(shell.favorites);
  out.bookmarks = PhiToRect(shell.bookmarks);
  out.stored_band = PhiToRect(shell.stored_band);
  out.bookmark_separator = PhiToRect(shell.bookmark_separator);
  out.new_tab_row = PhiToRect(shell.new_tab_row);
  out.bottom_bar = PhiToRect(shell.bottom_bar);
  out.sidebar_profile = PhiToRect(shell.sidebar_profile);
  out.sidebar_downloads = PhiToRect(shell.sidebar_downloads);
  out.memory = PhiToRect(shell.memory);
  out.feedback = PhiToRect(shell.feedback);
  out.chat = PhiToRect(shell.chat);
  out.extensions = PhiToRect(shell.extensions);
  out.app_menu = PhiToRect(shell.app_menu);
  out.page_icon = PhiToRect(shell.page_icon);
  out.pinned_label = PhiToRect(shell.pinned_label);
  out.bookmarks_label = PhiToRect(shell.bookmarks_label);
  out.tabs_label = PhiToRect(shell.tabs_label);
  out.agent_chip = PhiToRect(shell.agent_chip);
  out.reader = PhiToRect(shell.reader);
  out.copy_link = PhiToRect(shell.copy_link);
  out.site_menu = PhiToRect(shell.site_menu);
  out.sidebar_toggle = PhiToRect(shell.sidebar_toggle);
  out.field_ext_strip = PhiToRect(shell.field_ext_strip);
  out.add_space = PhiToRect(shell.add_space);
  out.chip_size = shell.chip_size;
  out.chips_left = shell.chips_left;
  out.chips_top = shell.chips_top;
  out.chips_limit = shell.chips_limit;
  out.tabs_left = shell.tabs_left;
  out.tabs_top = shell.tabs_top;
  out.tabs_limit = shell.tabs_limit;
  out.horizontal_tabs = shell.horizontal_tabs;
  out.spaces_switch = shell.spaces_switch;
  return out;
}

HFONT PhiShellWindow::EnsureChipNameFont() {
  if (!chip_name_font_ || chip_name_font_dpi_ != dpi_) {
    if (chip_name_font_) {
      ::DeleteObject(chip_name_font_);
    }
    chip_name_font_ = PhiFluentFont(dpi_, 12, FW_SEMIBOLD);
    chip_name_font_dpi_ = dpi_;
  }
  return chip_name_font_;
}

ShellLayoutInputs PhiShellWindow::CurrentLayoutInputs(bool include_floating) const {
  const bool floating = include_floating && floating_layout_;
  ShellLayoutInputs in;
  in.mode = layout_mode_;
  if (shell_hwnd_) {
    RECT client = {};
    ::GetClientRect(shell_hwnd_, &client);
    in.client_width = client.right;
    in.client_height = client.bottom;
  }
  in.dpi = static_cast<int>(dpi_);
  in.sidebar_dip = sidebar_dip_;
  // Collapsed: the content card widens to the left margin (which becomes
  // the floating sidebar's hover trigger). The floating panel paints with
  // floating_layout_ set, which restores the expanded layout.
  in.sidebar_hidden = sidebar_collapsed_ && !floating;
  in.floating_panel = floating;
  in.owned_tabs_label = !agent_chip_compact_ && !agent_chip_name_.empty();
  in.space_chip_dip = space_chip_dip_;
  in.agent_chip_dip = agent_chip_dip_;
  in.update_dip = update_dip_;
  if (floating && floating_panel_h_ > 0) {
    // The panel's own height: its bands (the bottom bar) fit the panel.
    in.client_height = floating_panel_h_;
  }
  // The floating panel keeps its sidebar over a page in content fullscreen
  // (the shell's own layout gives that page every pixel).
  in.content_fullscreen = browsers_->content_fullscreen() && !floating;
  in.bookmark_bar = layout_bookmark_bar_;
  in.reader_offerable = layout_reader_;
  in.ai_enabled = kPhiAiEnabled;
  in.field_ext_count = field_ext_count_;
  in.pinned_row_count = pinned_row_count_;
  in.bookmark_row_count = bookmark_row_count_;
  in.pinned_drop_zone = pinned_drop_zone_;
  in.bookmark_drop_zone = bookmark_drop_zone_;
  // The Mac's updateSpaceSwitchVisibility: the switch shows only with more
  // than one Space to switch between — or while the create form is up,
  // whose placeholder pip lives in the strip (forcesSpaceSwitchVisible).
  in.spaces_switch =
      spaces_ && (spaces_->SwitchTargets() > 1 || spaces_->creating());
  return in;
}

void PhiShellWindow::RefreshLayoutInputs() {
  // The sidebar width is the host's pref except while the user drags the
  // edge, when the live value wins until release persists it.
  if (!sidebar_resizing_ && services_.prefs) {
    sidebar_dip_ = std::max(kSidebarWidthMin,
                            std::min(services_.prefs->SidebarWidth(),
                                     kSidebarWidthMax));
  }
  layout_bookmark_bar_ = BookmarkBarVisible();
  layout_reader_ = ActiveTabReaderOfferable();
  // The Space chip (§5.2): 8 + icon 16 + 6 + name (≤ 120) + 4 + chevron 12
  // + 8, measured in the chip's 12/600 face.
  space_chip_dip_ = 0;
  if (layout_mode_ == PhiLayoutMode::kComfortable && services_.spaces &&
      shell_hwnd_) {
    EnsureChipNameFont();
    std::wstring name;
    for (const PhiSpaceView& space : services_.spaces->Spaces()) {
      if (space.id == active_space()) {
        name = base::UTF8ToWide(space.name);
        break;
      }
    }
    int name_dip = 0;
    if (HDC dc = ::GetDC(shell_hwnd_)) {
      HGDIOBJ previous = chip_name_font_ ? ::SelectObject(dc, chip_name_font_) : nullptr;
      SIZE extent = {};
      if (!name.empty() &&
          ::GetTextExtentPoint32W(dc, name.c_str(), static_cast<int>(name.size()),
                                  &extent)) {
        name_dip = static_cast<int>(extent.cx) * 96 / static_cast<int>(dpi_);
      }
      if (previous) {
        ::SelectObject(dc, previous);
      }
      ::ReleaseDC(shell_hwnd_, dc);
    }
    space_chip_dip_ = 8 + 16 + 6 + std::min(120, name_dip) + 4 + 12 + 8;
  }
  // The Update reminder (the Mac's upgrade button): 8 + the label in the
  // same 12/600 face + 8, never under 56 dip; 0 while nothing is on offer.
  update_dip_ = 0;
  if (shell_hwnd_ && PhiUpdater::UpdateAvailable()) {
    update_dip_ = kUpdatePillMinWidth;
    EnsureChipNameFont();
    if (HDC dc = ::GetDC(shell_hwnd_)) {
      HGDIOBJ previous =
          chip_name_font_ ? ::SelectObject(dc, chip_name_font_) : nullptr;
      const std::wstring label = PhiStr("sidebar.updateReminder.updateButton");
      SIZE extent = {};
      if (::GetTextExtentPoint32W(dc, label.c_str(),
                                  static_cast<int>(label.size()), &extent)) {
        const int label_dip = (static_cast<int>(extent.cx) * 96 +
                               static_cast<int>(dpi_) - 1) /
                              static_cast<int>(dpi_);
        update_dip_ = std::max(kUpdatePillMinWidth, 8 + label_dip + 8);
      }
      if (previous) {
        ::SelectObject(dc, previous);
      }
      ::ReleaseDC(shell_hwnd_, dc);
    }
  }
  ComputeAgentChip();
}

void PhiShellWindow::NoteUpdateAvailabilityChanged() {
  if (shell_hwnd_) {
    LayoutContents();  // The pill moves the strip's tabs and the nav trio.
  }
}

void PhiShellWindow::ComputeAgentChip() {
  agent_chip_dip_ = 0;
  agent_chip_compact_ = false;
  agent_chip_task_.clear();
  agent_chip_space_.clear();
  agent_chip_name_.clear();
  agent_chip_status_.clear();
  agent_chip_mark_.clear();
  if (!services_.agent || incognito_ || !shell_hwnd_) {
    return;
  }
  // The slot's agent pips: the one whose Space is on screen here takes the
  // full chip; failing that, the first one still running takes the
  // compact form (windows-native-ui.md §5.10 cue 3).
  const std::vector<PhiAgentPipView> pips =
      services_.agent->AgentSpaces(agent_ ? 0 : slot_.value);
  const PhiAgentPipView* shown = nullptr;
  const PhiAgentPipView* running = nullptr;
  for (const PhiAgentPipView& pip : pips) {
    if (pip.space_id == active_space()) {
      shown = &pip;
    } else if (!running && pip.badge == PhiAgentPipView::Badge::kRunning) {
      running = &pip;
    }
  }
  const PhiAgentPipView* pip = shown ? shown : running;
  if (!pip) {
    return;
  }
  agent_chip_compact_ = !shown;
  agent_chip_task_ = pip->task_id;
  agent_chip_space_ = pip->space_id;
  const PhiAgentDriverBadge badge = PhiAgentDriverBadgeFor(pip->agent_name);
  agent_chip_name_ = base::UTF8ToWide(badge.label);
  agent_chip_mark_ = pip->icon;
  // The five states' words and colours (§5.10): Running green, Waiting
  // orange, Idle grey, Done green, Error red.
  switch (pip->badge) {
    case PhiAgentPipView::Badge::kRunning:
      agent_chip_status_ = L"Running";
      agent_chip_dot_ = RGB(0x1E, 0x9E, 0x4A);
      break;
    case PhiAgentPipView::Badge::kIdle:
      agent_chip_status_ = L"Idle";
      agent_chip_dot_ = RGB(0x8E, 0x8E, 0x93);
      break;
    case PhiAgentPipView::Badge::kUserHolds:
      agent_chip_status_ = L"Waiting for you";
      agent_chip_dot_ = RGB(0xF0, 0xA0, 0x20);
      break;
    case PhiAgentPipView::Badge::kCompleted:
      agent_chip_status_ = L"Done";
      agent_chip_dot_ = RGB(0x1E, 0x9E, 0x4A);
      break;
    case PhiAgentPipView::Badge::kError:
      agent_chip_status_ = L"Error";
      agent_chip_dot_ = RGB(0xE0, 0x46, 0x3A);
      break;
  }
  if (agent_chip_compact_) {
    agent_chip_dip_ = 40;  // The mark and the dot.
    return;
  }
  EnsureChipNameFont();
  int name_dip = 60;
  int status_dip = 48;
  if (HDC dc = ::GetDC(shell_hwnd_)) {
    HGDIOBJ previous =
        chip_name_font_ ? ::SelectObject(dc, chip_name_font_) : nullptr;
    SIZE extent = {};
    if (::GetTextExtentPoint32W(dc, agent_chip_name_.c_str(),
                                static_cast<int>(agent_chip_name_.size()),
                                &extent)) {
      name_dip = static_cast<int>(extent.cx) * 96 / static_cast<int>(dpi_);
    }
    if (::GetTextExtentPoint32W(dc, agent_chip_status_.c_str(),
                                static_cast<int>(agent_chip_status_.size()),
                                &extent)) {
      status_dip = static_cast<int>(extent.cx) * 96 / static_cast<int>(dpi_);
    }
    if (previous) {
      ::SelectObject(dc, previous);
    }
    ::ReleaseDC(shell_hwnd_, dc);
  }
  // mark 16 · name · hairline · dot 7 · status · chevron 12, with 8 dip
  // padding and 6 dip gaps.
  agent_chip_dip_ = 8 + 16 + 6 + std::min(120, name_dip) + 8 + 1 + 8 + 7 +
                    4 + std::min(120, status_dip) + 8 + 6 + 12 + 8;
}

void PhiShellWindow::PaintAgentChip(HDC dc, const RECT& rect,
                                    const PhiPalette& palette, bool hot) {
  if (rect.right <= rect.left || agent_chip_task_.empty()) {
    return;
  }
  // Brand-soft ground (the accent at 12 %, 18 % hot), a capsule.
  const int radius = (rect.bottom - rect.top) / 2;
  PhiFillRoundRectAA(dc, rect, radius,
                     PhiBlend(palette.accent, palette.ground_top, hot ? 46 : 31));
  ::SetBkMode(dc, TRANSPARENT);
  const int mid = (rect.top + rect.bottom) / 2;
  int x = rect.left + Scaled(agent_chip_compact_ ? 8 : 8);
  RECT mark = {x, mid - Scaled(8), x + Scaled(16), mid + Scaled(8)};
  if (agent_chip_mark_.empty() ||
      !PhiDrawCatalogIcon(dc, mark, agent_chip_mark_, Scaled(14), palette.label)) {
    PhiDrawIconGlyph(dc, mark, fluent::kRobot, Scaled(14), palette.label);
  }
  x = mark.right + Scaled(6);
  if (agent_chip_compact_) {
    // The compact form: the status dot rides the mark's corner.
    RECT dot = {mark.right - Scaled(4), mark.bottom - Scaled(4),
                mark.right + Scaled(3), mark.bottom + Scaled(3)};
    PhiFillEllipseAA(dc, dot, agent_chip_dot_);
    return;
  }
  HGDIOBJ previous = chip_name_font_ ? ::SelectObject(dc, chip_name_font_) : nullptr;
  ::SetTextColor(dc, palette.label);
  RECT name = {x, rect.top, rect.right, rect.bottom};
  SIZE extent = {};
  ::GetTextExtentPoint32W(dc, agent_chip_name_.c_str(),
                          static_cast<int>(agent_chip_name_.size()), &extent);
  name.right = std::min<LONG>(rect.right, name.left + std::min<LONG>(extent.cx, Scaled(120)));
  PhiDrawTextAA(dc, agent_chip_name_.c_str(), -1, &name,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
  x = name.right + Scaled(8);
  RECT rule = {x, mid - Scaled(7), x + 1, mid + Scaled(7)};
  HBRUSH hairline = ::CreateSolidBrush(PhiBlend(palette.ink, palette.ground_top, 46));
  ::FillRect(dc, &rule, hairline);
  ::DeleteObject(hairline);
  x += 1 + Scaled(8);
  RECT dot = {x, mid - Scaled(3) - 1, x + Scaled(7), mid + Scaled(4)};
  PhiFillEllipseAA(dc, dot, agent_chip_dot_);
  x = dot.right + Scaled(4);
  ::SetTextColor(dc, palette.label_dim);
  RECT status = {x, rect.top, rect.right - Scaled(8 + 12 + 6), rect.bottom};
  PhiDrawTextAA(dc, agent_chip_status_.c_str(), -1, &status,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
  RECT chevron = {rect.right - Scaled(8 + 12), mid - Scaled(6),
                  rect.right - Scaled(8), mid + Scaled(6)};
  PhiDrawIconGlyph(dc, chevron, fluent::kChevronDown, Scaled(10),
                   palette.label_dim);
  if (previous) {
    ::SelectObject(dc, previous);
  }
}

PhiShellWindow::Layout PhiShellWindow::ComputeLayout(bool include_floating) {
  // A value, cached by its inputs: the geometry is recomputed only when
  // something it depends on moved (a resize, a DPI change, a mode or pref
  // change, the rebuild's counts), never per read — the old getter derived
  // it eight times per pointer move, each time re-querying the DPI and
  // creating fonts (lane 3 §3.2). dpi_ is written by WM_DPICHANGED and the
  // constructor alone.
  const ShellLayoutInputs inputs = CurrentLayoutInputs(include_floating);
  if (!layout_cache_valid_ || inputs != layout_inputs_) {
    layout_inputs_ = inputs;
    layout_cache_ = ToLayout(ComputeShellLayout(inputs));
    layout_cache_valid_ = true;
  }
  return layout_cache_;
}

PhiShellWindow::Layout PhiShellWindow::ComputeWindowLayout() {
  return ComputeLayout(/*include_floating=*/false);
}

RECT PhiShellWindow::content_rect() {
  return ComputeWindowLayout().content;
}

RECT PhiShellWindow::url_field_rect() {
  return ComputeLayout().url_field;
}

RECT PhiShellWindow::reader_button_rect() {
  return ComputeLayout().reader;
}

RECT PhiShellWindow::sidebar_toggle_rect() {
  return ComputeLayout().sidebar_toggle;
}

RECT PhiShellWindow::update_rect() {
  return ComputeLayout().update;
}

bool PhiShellWindow::ActiveTabExtensionOfferable() const {
  return services_.sidebar && active_tab_id().valid() &&
         services_.sidebar->IsReaderOfferable(active_tab_id().value,
                                              active_tab_url());
}

int PhiShellWindow::tabs_top() {
  return ComputeLayout().tabs_top;
}


int PhiShellWindow::tab_row_height() const {
  return Scaled(kTabRowHeight);
}

int PhiShellWindow::tab_row_gap() const {
  return Scaled(kTabRowGap);
}

int PhiShellWindow::title_bar_height() const {
  // Comfortable's band carries the tab strip as well as the window controls,
  // so it is taller there — this is what the control socket reports, and a
  // fixed answer would put every caller's arithmetic 8pt out in that layout.
  return Scaled(layout_mode_ == PhiLayoutMode::kComfortable
                    ? kTitleBarHeightTabs
                    : kTitleBarHeight);
}

int PhiShellWindow::sidebar_width() const {
  return Scaled(sidebar_dip_);
}

int PhiShellWindow::content_margin() const {
  return Scaled(kContentMargin);
}

POINT PhiShellWindow::frame_offset() const {
  POINT origin = {0, 0};
  if (!shell_hwnd_ || !::IsWindow(shell_hwnd_)) {
    return origin;
  }
  RECT window = {};
  ::GetWindowRect(shell_hwnd_, &window);
  ::ClientToScreen(shell_hwnd_, &origin);
  return {origin.x - window.left, origin.y - window.top};
}

void PhiShellWindow::ApplyPalette() {
  // A live create-form preview follows the appearance it was resolved in
  // until re-resolved here (the editor's Light / Dark buttons move it).
  spaces_->RefreshPreview();
  UpdateSidebarBackdrop();
  PlaceContentChildren(ComputeWindowLayout());
  RefreshTabs();  // Every colour is read from the host at paint time.
  PushSplitViewStyle();  // The framework paints the split's chrome itself.
}

void PhiShellWindow::PushSplitViewStyle() {
  if (!framework_ || agent_) {
    return;
  }
  const auto argb = [](PhiColor c) {
    return 0xFF000000u | (static_cast<uint32_t>(GetRValue(c)) << 16) |
           (static_cast<uint32_t>(GetGValue(c)) << 8) |
           static_cast<uint32_t>(GetBValue(c));
  };
  static const PhiPalette kFallback =
      PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kLight);
  // Incognito is FIXED-DARK regardless of theme (CurrentPalette's rule).
  static const PhiPalette kIncognito =
      PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kDark);
  for (const auto& [space_id, browser] : browsers_->spaces().all()) {
    if (!browser.valid()) {
      continue;
    }
    // The SETTLED palette of the browser's own Space, not the ramp a switch
    // animation is painting the chrome with: the framework should not be
    // re-laid out per frame.
    const PhiPalette& palette =
        incognito_          ? kIncognito
        : services_.spaces  ? services_.spaces->PaletteForSpace(space_id)
                            : kFallback;
    // The split (Figma 9044:26952, docs/split-view-chrome.md): two panes
    // flush in the content card, a 1 px divider and a 2 px accent ring
    // inside the focused pane. The ground shows only outside a pane's
    // rounded corner, against the sidebar, so it is the Space TINT the
    // sidebar paints; the divider is the design's neutrals — #FAFAFA with
    // 4 % ink over it in light; dark is not in the design (derived: the
    // dark ground with 6 % white). A contrast theme keeps its own edges.
    phi_framework::SplitViewStyle style;
    style.background_argb =
        sidebar_backdrop_
            ? PhiBackdropColor(argb(palette.ground_top) & 0xFFFFFF, palette.dark)
            : argb(palette.ground_top);
    if (PhiHighContrastActive()) {
      style.divider_ground_argb = argb(palette.ground_top);
      style.border_argb = argb(palette.border);
    } else if (palette.dark) {
      style.divider_ground_argb = argb(palette.ground_top);
      style.border_argb = 0x0FFFFFFFu;  // White at 6 %.
    } else {
      style.divider_ground_argb = 0xFFFAFAFAu;
      style.border_argb = 0x0A000000u;  // Black at 4 %.
    }
    style.focus_argb = argb(palette.accent);
    // Flush under the address row: the row's separator is the panes' top.
    style.top_inset = 0;
    auto it = split_style_pushed_.find(browser.value);
    if (it != split_style_pushed_.end() &&
        it->second.background_argb == style.background_argb &&
        it->second.border_argb == style.border_argb &&
        it->second.focus_argb == style.focus_argb &&
        it->second.top_inset == style.top_inset &&
        it->second.divider_ground_argb == style.divider_ground_argb) {
      continue;
    }
    split_style_pushed_[browser.value] = style;
    framework_->SetSplitViewStyle(browser.value, style);
  }
}

void PhiShellWindow::ApplyLayoutMode(PhiLayoutMode mode) {
  if (layout_mode_ == mode) {
    return;
  }
  layout_mode_ = mode;
  LayoutContents();  // Also repositions the address field for the new mode.
  RefreshTabs();
}

PhiShellWindow::PhiShellWindow(SlotId slot,
                                        BrowserId browser,
                                        HWND browser_hwnd,
                                        phi_framework::PhiFramework* framework,
                                        const ShellServices& services,
                                        const PhiWindowModel* model,
                                        bool agent,
                               bool incognito,
                               bool start_hidden,
                               const std::string& first_space)
    : // The URL-driven client-drawn pages (bookmark manager, new-tab page).
      // First, so the early return below still leaves a usable host.
      native_pages_(std::make_unique<PhiNativePagesHost>(
          static_cast<PhiNativePagesShell*>(this))),
      // The comfortable layout's Space switch: the whole client area slides.
      content_slide_(std::make_unique<PhiContentSlide>(
          static_cast<PhiContentSlideShell*>(this))),
      slot_(slot),
      // The content area's component: the adopted browser child and the
      // slot's Space -> browser table.
      browsers_(std::make_unique<PhiBrowserHost>(
          static_cast<PhiBrowserHostShell*>(this))),
      // The caption band, which hosts the menu bar.
      caption_band_(std::make_unique<PhiCaptionBand>(
          static_cast<PhiCaptionShell*>(this))),
      // The address field's component.
      omnibox_(std::make_unique<PhiOmniboxSession>(
          static_cast<PhiOmniboxShell*>(this))),
      framework_(framework),
      services_(services),
      model_(model),
      // The Spaces strip: pips, hover card, create form, switch animation.
      spaces_(std::make_unique<PhiSpacesStrip>(
          static_cast<PhiSpacesStripShell*>(this),
          services.spaces,
          services.prefs,
          services.agent)),
      // The sidebar: the row pipeline, its band's paint and its drags.
      sidebar_(std::make_unique<PhiSidebarView>(
          static_cast<PhiSidebarShell*>(this),
          slot,
          framework,
          services.sidebar,
          services.spaces,
          services.prefs,
          services.agent,
          services.window,
          model,
          agent,
          incognito)),
      agent_(agent),
      incognito_(incognito) {
  if (!browser_hwnd || !::IsWindow(browser_hwnd)) {
    LOG(ERROR) << "[PhiClient] adopt: invalid browser HWND for browser "
               << browser.value << " (slot " << slot_.value << ")";
    return;
  }
  EnsureShellClass();
  if (services_.prefs) {
    // One layout for the whole client.
    layout_mode_ = services_.prefs->LayoutMode();
  }

  // Create the client shell so its CONTENT REGION lands exactly where the
  // browser window already is — the shell is one sidebar wider, plus its frame.
  //
  // Sizing the shell to the browser's own bounds instead would hand Chromium a
  // content area one sidebar NARROWER than the bounds it restored. Chromium
  // saves that shrunken rect as its restored bounds on close, so every restart
  // would lose another 240px until the window hit Chromium's minimum size.
  // Anchoring the content instead makes the round trip a fixed point.
  RECT wr = {};
  ::GetWindowRect(browser_hwnd, &wr);
  if (const UINT dpi = ::GetDpiForWindow(browser_hwnd)) {
    dpi_ = dpi;  // Lay the first frame out for the monitor we open on.
  }
  EnsureFont();  // The fonts follow dpi_: here and in WM_DPICHANGED only.
  RefreshLayoutInputs();
  int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
  int w = Scaled(1200 + sidebar_dip_), h = Scaled(800);
  // Distrust a restored rect that is mostly off-screen (a stale placement
  // saved from an off-screen window, or a monitor that is gone): anchoring
  // the shell to it would open as a sliver at the screen edge. Chromium's
  // own restore only guarantees ~30px stays visible, so require a usable
  // amount before honoring it.
  if (wr.right > wr.left && wr.bottom > wr.top) {
    HMONITOR monitor = ::MonitorFromRect(&wr, MONITOR_DEFAULTTONULL);
    MONITORINFO info = {sizeof(info)};
    RECT visible = {};
    if (!monitor || !::GetMonitorInfoW(monitor, &info) ||
        !::IntersectRect(&visible, &wr, &info.rcWork) ||
        visible.right - visible.left < 300 ||
        visible.bottom - visible.top < 200) {
      wr = {};  // Fall through to the CW_USEDEFAULT defaults above.
    }
  }
  if (wr.right > wr.left && wr.bottom > wr.top) {
    // The shell's frame is CUSTOM (WM_NCCALCSIZE): the client reaches the
    // window's top edge and only the resize borders remain non-client, so
    // AdjustWindowRectEx (which assumes an OS caption) would be wrong. The
    // content region sits at (sidebar, titleBand + topGap) inside the client;
    // place the window so that region lands exactly on the browser's bounds.
    const int border_x = ::GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi_) +
                         ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
    const int border_y = ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi_) +
                         ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
    // Where the content region starts inside the client, PER LAYOUT — the
    // numbers ComputeLayout produces: comfortable has no sidebar and stacks
    // the taller band and the toolbar; balanced adds the navigation bar
    // below the band and the gap; performance is the band and the gap. The
    // trailing margin is the content card's everywhere but comfortable,
    // whose page runs edge to edge. A mode-blind conversion was placing
    // balanced and comfortable windows a bar (or a sidebar) off on their
    // first frame.
    // Vertical: the pane starts at the sidebar's edge under the 44 address
    // row; horizontal: the page starts under the strip and the toolbar.
    int content_left = Scaled(sidebar_dip_);
    int content_top = Scaled(kAddressRowHeight);
    const int trailing = 0;
    if (layout_mode_ == PhiLayoutMode::kComfortable) {
      content_left = 0;
      content_top = Scaled(kStripRowHeight) + Scaled(kToolbarRowHeight);
    }
    x = wr.left - content_left - border_x;
    y = wr.top - content_top;
    w = (wr.right + trailing + border_x) - x;
    h = (wr.bottom + trailing + border_y) - y;
  }

  // FIRST RUN — and any rect rejected above: there is no shell placement to
  // inherit, only Chromium's DEFAULT bounds, which sit a tile inside the work
  // area's top-left. Anchoring the CONTENT to those drags the sidebar off the
  // left edge and the caption band off the top: the window comes up with the
  // page pinned to the screen corner and its chrome out of reach. Open the
  // SHELL off that corner instead, by a margin of its own. Only the position
  // is chosen here — the size the anchor produced is Chromium's own default,
  // kept unless it would overflow the margin on the far edges. From the next
  // launch the saved rect reproduces this placement through the round trip
  // above, so this runs once per profile.
  MONITORINFO home = {sizeof(home)};
  if (::GetMonitorInfoW(
          ::MonitorFromWindow(browser_hwnd, MONITOR_DEFAULTTOPRIMARY),
          &home) &&
      (x == CW_USEDEFAULT || x < home.rcWork.left || y < home.rcWork.top)) {
    const int work_w = home.rcWork.right - home.rcWork.left;
    const int work_h = home.rcWork.bottom - home.rcWork.top;
    // The margin gives way on a work area too small to hold it.
    const int margin = std::min(Scaled(kDefaultPlacementMargin),
                                std::min(work_w, work_h) / 8);
    x = home.rcWork.left + margin;
    y = home.rcWork.top + margin;
    w = std::min(w, work_w - 2 * margin);
    h = std::min(h, work_h - 2 * margin);
  }

  // A TEAR-OFF opens AROUND the point the tab was dropped at — the Mac's
  // resolvedTearOffWindowFrame: the drop's normalized position in the work
  // area becomes its position inside the new window, so a drop near the
  // top-left corner puts the window's top-left there, and one near the
  // bottom-right pulls the window up and left to stay on screen. The size
  // is whatever the placement above produced.
  POINT drop = {};
  if (!agent_ && services_.window && services_.window->TakeTearOffDrop(&drop)) {
    MONITORINFO target = {sizeof(target)};
    if (::GetMonitorInfoW(::MonitorFromPoint(drop, MONITOR_DEFAULTTONEAREST),
                          &target)) {
      const RECT& work = target.rcWork;
      const int work_w = work.right - work.left;
      const int work_h = work.bottom - work.top;
      if (work_w > 1 && work_h > 1) {
        w = std::min(w, work_w);
        h = std::min(h, work_h);
        const double rx = std::min(
            1.0, std::max(0.0, static_cast<double>(drop.x - work.left) /
                                   work_w));
        const double ry = std::min(
            1.0, std::max(0.0, static_cast<double>(drop.y - work.top) /
                                   work_h));
        x = std::max<int>(work.left,
                          std::min<int>(drop.x - static_cast<int>(rx * w),
                                        work.right - w));
        y = std::max<int>(work.top,
                          std::min<int>(drop.y - static_cast<int>(ry * h),
                                        work.bottom - h));
      }
    }
  }

  if (agent_) {
    // A hidden agent surface: fixed size, parked far off-screen, kept off the
    // taskbar. It is still shown (below) so its content composites and CDP
    // screen capture works — the user simply never sees it.
    x = -32000;
    y = -32000;
    w = Scaled(1400 + sidebar_dip_);
    h = Scaled(900);
  }
  shell_hwnd_ = ::CreateWindowExW(
      agent_ ? (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) : 0, kShellClassName,
      L"Phi", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr,
      nullptr, ::GetModuleHandleW(nullptr), this);
  if (!shell_hwnd_) {
    LOG(ERROR) << "[PhiClient] shell CreateWindowEx failed err="
               << ::GetLastError();
    return;
  }

  // Force one frame recalculation now that the proc can see `this`: whatever
  // non-client layout the creation pass produced, this pass runs through OUR
  // WM_NCCALCSIZE and retires the OS caption for good.
  ::SetWindowPos(shell_hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);

  // The shell takes ITS monitor's scale, as PhiWindow::Create does for an
  // unowned window: a tear-off dropped on a display at another scale than
  // the browser's opens there, and creation sends no WM_DPICHANGED. The
  // size above was worked out at the browser's scale, so it follows by the
  // ratio (kept inside that monitor's work area); the position stays.
  if (const UINT actual = ::GetDpiForWindow(shell_hwnd_);
      actual && actual != dpi_ && !agent_) {
    const UINT from = dpi_;
    dpi_ = actual;
    EnsureFont();
    RefreshLayoutInputs();
    RECT placed = {};
    ::GetWindowRect(shell_hwnd_, &placed);
    int scaled_w = ::MulDiv(placed.right - placed.left,
                            static_cast<int>(actual), static_cast<int>(from));
    int scaled_h = ::MulDiv(placed.bottom - placed.top,
                            static_cast<int>(actual), static_cast<int>(from));
    MONITORINFO work = {sizeof(work)};
    if (::GetMonitorInfoW(
            ::MonitorFromWindow(shell_hwnd_, MONITOR_DEFAULTTONEAREST),
            &work)) {
      scaled_w = std::min<int>(scaled_w, work.rcWork.right - placed.left);
      scaled_h = std::min<int>(scaled_h, work.rcWork.bottom - placed.top);
    }
    ::SetWindowPos(shell_hwnd_, nullptr, 0, 0, scaled_w, scaled_h,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }

  caption_band_->Install(shell_hwnd_);
  // The app menu (⋯): a popup tree, never attached with SetMenu.
  app_menu_ = std::make_unique<PhiAppMenu>(static_cast<PhiAppMenuShell*>(this));
  app_menu_->Install(shell_hwnd_);

  // The window the slot was created with backs the DEFAULT Space — whichever
  // Space holds that role now (it moves when its holder is deleted).
  const std::string default_space =
      !first_space.empty() ? first_space
      : services_.spaces   ? services_.spaces->DefaultSpaceId()
                           : std::string("default-space");
  browsers_->AdoptFirst(default_space, browser, browser_hwnd);
  if (!model::IsIncognitoSpaceId(default_space)) {
    last_regular_space_ = default_space;
  }
  omnibox_->EnsureField();
  // An Incognito Space's browser is never a lazy-restore window.
  if (services_.spaces && !incognito_ && !agent_ &&
      !model::IsIncognitoSpaceId(default_space)) {
    // The default Space binds here rather than through AttachSpaceBrowser,
    // so it needs its own note or lazy restore would never learn its window.
    // NEVER for an agent/shadow window: it nominally binds the default Space
    // too, and noting it made the Space's remembered window THE AGENT'S —
    // an agent alive at quit then put the agent window (omitted from the
    // session) in the cold-start eager set and PARKED the user's real one.
    // A second window of a Space (Ctrl+N, a tear-off) is remembered BESIDE
    // the Space's window, never over it (PhiCoordinator::NoteSpaceWindow).
    services_.spaces->NoteSpaceWindow(default_space, browser.value);
  }
  if (framework_ && !incognito_ && !agent_) {
    // An incognito shell stays OUT of the Space registry: routed URLs must
    // never resolve into an off-the-record window — and so does an agent
    // window, which the framework registers as "space.agent" itself.
    framework_->SetWindowSpace(browser.value, default_space.c_str());
  }
  LayoutContents();
  if (start_hidden && !agent_) {
    // The onboarding gate: the shell exists (its browser is alive and laid
    // out) but stays off screen until access is granted. Chromium shows the
    // browser HWND itself right after creation, so the child stays parked
    // where LayoutContents put it — inside a window that is not shown.
    shell_hidden_ = true;
  } else if (agent_) {
    ::ShowWindow(shell_hwnd_, SW_SHOWNOACTIVATE);
  }
  LOG(INFO) << "[PhiClient] " << (agent_ ? "agent " : "") << "shell window "
            << slot_.value << " browser=" << browser.value
            << " shell_hwnd=" << shell_hwnd_ << " browser_hwnd="
            << browser_hwnd;
}

PhiShellWindow::~PhiShellWindow() {
  // From here on the shell's own messages (DestroyWindow below still
  // delivers WM_ACTIVATE / WM_SIZE / WM_CAPTURECHANGED) find a controller
  // that must do nothing but die.
  destroying_ = true;
  EndSidebarResize();
  // A hosted agent Space goes back to its own shell first, whole: the
  // browser is the agent's, and it must not die with this window — the
  // one sliding home, then the one on screen.
  FinishUnhost();
  if (HostsAgentSpace()) {
    leaving_agent_task_ = hosted_agent_task_;
    leaving_agent_space_ = hosted_agent_space_;
    hosted_agent_task_.clear();
    hosted_agent_space_.clear();
    FinishUnhost();
  }
  // A drag image that is sliding home has OUTLIVED its session (the slide
  // is the image window's own animation), so the window it was lifted out
  // of disowns it here — and BEFORE the drag is aborted below, so a window
  // closing under a live drag takes the image straight down rather than
  // animating it home over a shell that is going away.
  PhiDragImage::OwnerGone(shell_hwnd_);
  // The drag itself — the split-drop cards, the capture, the slide clock —
  // comes down with the session: ~PhiSidebarDragSession IS AbortDrag.
  sidebar_->AbortDrag();
  // The process-wide satellites bound to this window: each keeps a bare
  // PhiShellWindow* and outlives its shell otherwise (the agent
  // overlay's Reposition runs from EVERY shell's LayoutContents, the
  // overview and the switcher answer keys, the search panel a click).
  PhiAgentOverlay::HideIfBoundTo(this);
  PhiCreateSpaceWindow::CloseIfBoundTo(this);
  PhiCloseCreateSpaceFlyoutIfBoundTo(this);
  PhiGroupOverview::CloseIfBoundTo(this);
  PhiLibrary::CloseIfBoundTo(this);
  PhiTabSwitcher::CancelIfBoundTo(this);
  PhiSearchTabsPanel::CloseIfBoundTo(this);
  // Owned components and popups go BEFORE the shell: their teardown calls
  // back into a controller that is still whole, rather than running as a
  // side effect of the shell's DestroyWindow on one that is half gone.
  tab_preview_.reset();
  sidebar_.reset();
  native_pages_->Shutdown();
  spaces_->Shutdown();
  content_slide_.reset();  // Its clock stops; no landing callback runs.
  caption_band_.reset();
  if (floating_sidebar_ && ::IsWindow(floating_sidebar_)) {
    ::DestroyWindow(floating_sidebar_);
  }
  floating_sidebar_ = nullptr;
  // Detach the browser child before tearing down our window, so its teardown
  // is not entangled with ours. Full close plumbing is a later slice. The
  // caps and the status bubble are the content host's and go with it, after
  // the shell window they are children of.
  browsers_->ReleaseContentChild();
  if (shell_hwnd_ && ::IsWindow(shell_hwnd_)) {
    ::KillTimer(shell_hwnd_, kDeferredCloseTimer);
    ::DestroyWindow(shell_hwnd_);  // Takes the address field child with it.
  }
  if (ui_font_) {
    ::DeleteObject(ui_font_);
  }
  if (emoji_font_) {
    ::DeleteObject(emoji_font_);
  }
  if (caption_font_) {
    ::DeleteObject(caption_font_);
  }
  if (chip_icon_font_) {
    ::DeleteObject(chip_icon_font_);
  }
  if (pip_icon_font_) {
    ::DeleteObject(pip_icon_font_);
  }
  if (pip_emoji_font_) {
    ::DeleteObject(pip_emoji_font_);
  }
  if (pip_text_font_) {
    ::DeleteObject(pip_text_font_);
  }
  if (small_font_) {
    ::DeleteObject(small_font_);
  }
  if (chip_name_font_) {
    ::DeleteObject(chip_name_font_);
  }
  if (label_font_) {
    ::DeleteObject(label_font_);
  }
  if (ui_semibold_font_) {
    ::DeleteObject(ui_semibold_font_);
  }
  if (badge_font_) {
    ::DeleteObject(badge_font_);
  }
  if (back_buffer_) {
    ::DeleteObject(back_buffer_);
  }
}

HWND PhiShellWindow::OmniboxShellWindow() const {
  return shell_hwnd_;
}

bool PhiShellWindow::OmniboxShellDestroying() const {
  return destroying_;
}

int PhiShellWindow::OmniboxScaled(int design_units) const {
  return Scaled(design_units);
}

HFONT PhiShellWindow::OmniboxTextFont() const {
  return ui_font_;
}

HFONT PhiShellWindow::OmniboxIconFont() const {
  return chip_icon_font_;
}

HFONT PhiShellWindow::OmniboxChipFont() const {
  return ui_semibold_font_;  // The 13/600 the Space chip's label uses.
}

HFONT PhiShellWindow::OmniboxHintFont() const {
  return small_font_;
}

PhiPalette PhiShellWindow::OmniboxPalette() const {
  // CurrentPalette, not the host's: incognito overrides to fixed-dark, and
  // a Space switch ramps.
  return CurrentPalette();
}

PhiPalette PhiShellWindow::OmniboxFieldPalette() const {
  // The field sits in the address row, which follows the page.
  return BarPalette();
}

PhiLayoutMode PhiShellWindow::OmniboxLayoutMode() const {
  return layout_mode_;
}

phi_framework::PhiFramework* PhiShellWindow::OmniboxFramework() const {
  return framework_;
}

BrowserId PhiShellWindow::OmniboxBrowser() const {
  return active_browser_id();
}

BrowserId PhiShellWindow::OmniboxPageBrowser() const {
  return PageCommandBrowser();
}

void PhiShellWindow::OmniboxFieldPressed() {
  CloseExtensionPopup();
}

void PhiShellWindow::OmniboxFocusContent() {
  FocusPageBrowser();  // The presented peek's page, when one is up.
}

void PhiShellWindow::OmniboxLeaveReader() {
  LeaveReaderViewForNavigation();
}

// --- PhiCaptionShell: what the caption band asks of its shell -------------

HWND PhiShellWindow::CaptionShellWindow() const {
  return shell_hwnd_;
}

int PhiShellWindow::CaptionScaled(int design_units) const {
  return Scaled(design_units);
}

UINT PhiShellWindow::CaptionDpi() const {
  return dpi_;
}

HFONT PhiShellWindow::CaptionGlyphFont() const {
  return caption_font_;
}

PhiPalette PhiShellWindow::CaptionPalette() const {
  // The caption buttons share the address row in the vertical layout, which
  // follows the page; in comfortable they sit on the strip's tint.
  return layout_mode_ == PhiLayoutMode::kComfortable ? CurrentPalette()
                                                    : BarPalette();
}

PhiCaptionRects PhiShellWindow::CaptionRects() {
  const Layout layout = ComputeLayout();
  PhiCaptionRects rects;
  rects.mode = layout.mode;
  rects.title_bar = layout.title_bar;
  rects.caption_min = layout.caption_min;
  rects.caption_max = layout.caption_max;
  rects.caption_close = layout.caption_close;
  rects.band_divider = layout.band_divider;
  rects.back = layout.back;
  rects.forward = layout.forward;
  rects.reload = layout.reload;
  rects.search_tabs = layout.search_tabs;
  rects.update = layout.update;
  rects.sidebar_toggle = layout.sidebar_toggle;
  rects.app_menu = layout.app_menu;
  rects.url_field = layout.url_field;
  rects.agent_chip = layout.agent_chip;
  rects.field_ext_strip = layout.field_ext_strip;
  rects.extensions = layout.extensions;
  rects.spaces_strip = layout.spaces_strip;
  rects.tabs_top = layout.tabs_top;
  rects.horizontal_tabs = layout.horizontal_tabs;
  return rects;
}

bool PhiShellWindow::CaptionStripTakesPoint(POINT client_pt) {
  // Comfortable's band IS the tab strip, and the strip is the shell's: the
  // band asks whether anything drawn there is under the pointer.
  if (::PtInRect(&sidebar_->add_tab_button(), client_pt) ||
      ::PtInRect(&sidebar_->strip_overflow_button(), client_pt)) {
    return true;
  }
  for (const TabRow& row : sidebar_->tabs()) {
    if (::PtInRect(&row.bounds, client_pt)) {
      return true;
    }
  }
  for (const GroupHeader& chip : sidebar_->group_headers()) {
    if (::PtInRect(&chip.bounds, client_pt)) {
      return true;  // The strip's group chips.
    }
  }
  if (spaces_->ChipAt(client_pt) >= 0) {
    return true;
  }
  if (::PtInRect(&spaces_->add_slot(), client_pt)) {
    return true;  // The "+" / "…" slot trailing the pips.
  }
  for (const ShortcutRow& pin : sidebar_->pinned()) {
    if (::PtInRect(&pin.bounds, client_pt) ||
        ::PtInRect(&pin.surface_box, client_pt)) {
      return true;  // The cell, or its §5.8 tile overhanging its corner.
    }
  }
  for (const ShortcutRow& row : sidebar_->bookmarks()) {
    if (::PtInRect(&row.bounds, client_pt)) {
      return true;
    }
  }
  return false;
}

void PhiShellWindow::CaptionClearShellHover() {
  ClearHover();
}

bool PhiShellWindow::CaptionContentFullscreen() const {
  return browsers_->content_fullscreen();
}

// --- PhiAppMenuShell: what the app menu asks of its shell -----------------
// This interface RETIRED `friend class PhiMenuBar` (lane 1 §2.1 item 3): the
// bar used to read seventeen private members by name, which is why the shell
// had to publish const reference views onto its own components' state just
// to keep the bar compiling. Nothing below hands out a reference into a
// cache the bar could outlive.

HWND PhiShellWindow::MenuShellWindow() const {
  return shell_hwnd_;
}

SlotId PhiShellWindow::MenuSlot() const {
  return slot_;
}

void PhiShellWindow::CloseIncognitoSpaceAsking(std::string space_id) {
  // Locals only past the alert: its modal loop pumps messages.
  SpacesHost* const spaces = services_.spaces;
  ChromePrefsHost* const prefs = services_.prefs;
  if (!spaces || !model::IsIncognitoSpaceId(space_id)) {
    return;
  }
  if (!prefs || !prefs->SuppressCloseIncognitoSpaceWarning()) {
    // The Mac's NSAlert: the title alone, Close (default) and Cancel, and
    // "Do not ask again" as the suppression box (honoured on Close only).
    bool dont_ask = false;
    const int chosen = PhiAlertWithCheckbox(
        shell_hwnd_, PhiStr("spaces.closeIncognitoConfirmation.title"),
        nullptr,
        {{PhiStr("spaces.closeIncognitoConfirmation.closeButton"),
          PhiAlertRole::kPrimary},
         {PhiStr("spaces.closeIncognitoConfirmation.cancelButton"),
          PhiAlertRole::kSecondary}},
        PhiStr("spaces.closeIncognitoConfirmation.doNotAskAgainCheckbox"),
        &dont_ask, /*default_index=*/0, /*cancel_index=*/1);
    if (chosen != 0) {
      return;
    }
    if (dont_ask && prefs) {
      prefs->SetSuppressCloseIncognitoSpaceWarning(true);
    }
  }
  spaces->RequestCloseIncognitoSpace(space_id);
}

bool PhiShellWindow::CloseTabEndsIncognitoSpace() {
  const std::string space = active_space();
  if (incognito_ || agent_ || !model::IsIncognitoSpaceId(space)) {
    return false;
  }
  const BrowserId browser = active_browser_id();
  const phi_framework::WindowSnapshot* snapshot =
      model_ && browser.valid() ? model_->Get(browser) : nullptr;
  if (snapshot && snapshot->tabs.size() > 1) {
    return false;
  }
  // The Mac's Cmd-W on the last tab: the Space, not just its tab — and a
  // Cancel swallows the close.
  CloseIncognitoSpaceAsking(space);
  return true;
}

bool PhiShellWindow::MenuCloseTabEndsIncognitoSpace() {
  return CloseTabEndsIncognitoSpace();
}

void PhiShellWindow::MenuCloseIncognitoSpace() {
  if (!incognito_ && !agent_) {
    CloseIncognitoSpaceAsking(active_space());
  }
}

bool PhiShellWindow::ShowsIncognito() const {
  return incognito_ || model::IsIncognitoSpaceId(active_space());
}

bool PhiShellWindow::MenuIncognito() const {
  return ShowsIncognito();
}

SidebarHost* PhiShellWindow::MenuSidebarHost() const {
  return services_.sidebar;
}

SpacesHost* PhiShellWindow::MenuSpacesHost() const {
  return services_.spaces;
}

ChromePrefsHost* PhiShellWindow::MenuPrefsHost() const {
  return services_.prefs;
}

WindowHost* PhiShellWindow::MenuWindowHost() const {
  return services_.window;
}

phi_framework::PhiFramework* PhiShellWindow::MenuFramework() const {
  return framework_;
}

const PhiWindowModel* PhiShellWindow::MenuModel() const {
  return model_;
}

BrowserId PhiShellWindow::MenuActiveBrowser() const {
  return active_browser_id();
}

TabId PhiShellWindow::MenuActiveTab() const {
  return active_tab_id();
}

BrowserId PhiShellWindow::MenuPageCommandBrowser() const {
  return PageCommandBrowser();
}

TabId PhiShellWindow::MenuPageCommandTab() const {
  return PageCommandTab();
}

std::string PhiShellWindow::MenuActiveSpace() const {
  return active_space();
}

PhiPalette PhiShellWindow::MenuPalette() const {
  return CurrentPalette();
}

int PhiShellWindow::MenuScaled(int design_units) const {
  return Scaled(design_units);
}

HWND PhiShellWindow::MenuAddressField() const {
  return omnibox_->field();
}

void PhiShellWindow::MenuFocusPageForFind() {
  if (PhiLibrary::HandleFindIfBoundTo(this)) {
    return;  // Spaces swallows Find; Downloads takes it for its search.
  }
  const BrowserId browser = active_browser_id();
  if (PageCommandBrowser() != browser) {
    // Find opens inside the presented peek's own Views tree (the Mac's
    // pageCommandTarget); its browser is a hosted child that only
    // Chromium's activation reaches.
    FocusPageBrowser();
    return;
  }
  const HWND page = browsers_->content_hwnd();
  if (!framework_ || !browser.valid() || !page || !::IsWindow(page)) {
    return;
  }
  ::SetFocus(page);
  framework_->FocusBrowserWindow(browser.value);
}

void PhiShellWindow::MenuFocusAddressField() {
  FocusAddressField();
}

void PhiShellWindow::MenuRefreshNavState() {
  // Re-reads nav from the window model and re-syncs the field. NOT a row
  // rebuild: the rows a menu shows are always the finished ones (phase 2
  // made rebuilds posted, and a menu opening is not a reason to rebuild).
  RebuildNavState();
}

PhiAppMenuNavState PhiShellWindow::MenuNavState() const {
  PhiAppMenuNavState nav;
  nav.url = omnibox_->nav_url();
  nav.can_go_back = omnibox_->can_go_back();
  nav.can_go_forward = omnibox_->can_go_forward();
  nav.loading = omnibox_->nav_loading();
  return nav;
}

std::vector<PhiAppMenuTab> PhiShellWindow::MenuTabRows() const {
  // A SNAPSHOT of the finished rows. The bar holds it for the length of one
  // menu build; a rebuild under an open menu cannot reach into it.
  std::vector<PhiAppMenuTab> rows;
  rows.reserve(sidebar_->tabs().size());
  for (const TabRow& row : sidebar_->tabs()) {
    rows.push_back({row.id, row.title, row.active});
  }
  return rows;
}

bool PhiShellWindow::MenuReaderActive() const {
  return ActiveTabShowsReader();
}

bool PhiShellWindow::MenuReaderOfferable() const {
  return ActiveTabCanUseReader();
}

bool PhiShellWindow::MenuLeaveReaderForNavigation() {
  return LeaveReaderViewForNavigation();
}

std::vector<PhiAgentPipView> PhiShellWindow::MenuAgentSpaces() const {
  // This window's own list (PhiAgentSpaceManager::Presents); an agent
  // window's menus never switch agent Spaces — its strip does.
  if (!services_.agent || agent_ || incognito_) {
    return {};
  }
  return services_.agent->AgentSpaces(slot_.value);
}

void PhiShellWindow::MenuSwitchSpace(const std::string& space_id) {
  SwitchToSpace(space_id);
}

void PhiShellWindow::MenuNewTab() {
  StartNewTab();  // Honors the "New tab behavior" setting.
}

void PhiShellWindow::MenuToggleSidebar() {
  ToggleSidebar();
}

bool PhiShellWindow::MenuAgentHoldsInput() const {
  return AgentHoldsInput();
}

void PhiShellWindow::MenuToggleReader(bool from_accelerator) {
  ToggleReaderView(from_accelerator ? "shortcut" : "view_menu");
}

void PhiShellWindow::MenuToggleDownloads() {
  ToggleDownloadsPopover();
}

void PhiShellWindow::MenuToggleSearchTabs() {
  PhiSearchTabsPanel::Toggle(this);
}

void PhiShellWindow::MenuToggleLibrary() {
  // The ⋯ menu's row and Ctrl+Shift+L: the Library flies out of the ⋯
  // button, on the window's last section (Downloads at first).
  PhiLibrary::Toggle(this);
}

// static
PhiShellWindow* PhiShellWindow::FromHwnd(HWND hwnd) {
  if (!hwnd || !::IsWindow(hwnd)) {
    return nullptr;
  }
  wchar_t name[64] = {};
  if (!::GetClassNameW(hwnd, name, 64) ||
      std::wstring_view(name) != kShellClassName) {
    return nullptr;  // Another class's USERDATA is not a shell.
  }
  return reinterpret_cast<PhiShellWindow*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void PhiShellWindow::OpenStoredRowInSpace(const std::string& space_id,
                                          const std::string& guid,
                                          const std::string& url) {
  if (space_id.empty() || url.empty()) {
    return;
  }
  if (space_id == active_space()) {
    OpenOrFocusPinned(guid, url);
    return;
  }
  // A Space whose browser is attached presents synchronously; one that is
  // not is created by the owner and lands through BrowserHostNoteActiveSpace,
  // which runs the open then. Either way the switch goes first.
  pending_stored_open_ = PendingStoredOpen{space_id, guid, url};
  SwitchToSpace(space_id);
  if (pending_stored_open_ && active_space() == space_id) {
    pending_stored_open_.reset();
    OpenOrFocusPinned(guid, url);
  }
}

void PhiShellWindow::ConsumePendingStoredOpen(const std::string& landed_space,
                                              bool drop_on_mismatch) {
  if (!pending_stored_open_) {
    return;
  }
  if (pending_stored_open_->space_id != landed_space) {
    if (drop_on_mismatch) {
      pending_stored_open_.reset();
    }
    return;
  }
  const PendingStoredOpen pending = *pending_stored_open_;
  pending_stored_open_.reset();
  if (active_space() == pending.space_id) {
    // The sidebar resolves the row against the ACTIVE browser — the one
    // that just landed — so a bound tab focuses and an unbound row opens
    // there, never in the Space the Library was closed over.
    OpenOrFocusPinned(pending.guid, pending.url);
  }
}

void PhiShellWindow::ShowHeldShell() {
  if (!shell_hidden_ || !shell_hwnd_) {
    return;
  }
  shell_hidden_ = false;
  ::ShowWindow(shell_hwnd_, SW_SHOW);
  ::SetForegroundWindow(shell_hwnd_);
  LayoutContents();
  // Chromium showed these browsers while the shell was hidden, and
  // BrowserView::Show shows its Widget only for a PRESENTED shell
  // (OnHostedWindowShow): their compositors never came on, so the page area
  // stayed transparent after the gate opened (2026-10-16). Run Chromium's
  // Show again now that the shell is up — the active browser with its
  // first-show work, the others inactive, which never switches the Space.
  if (framework_) {
    const BrowserId active = active_browser_id();
    for (const std::string& space : browsers_->spaces().order()) {
      const BrowserId browser = browsers_->spaces().ForSpace(space);
      if (browser.valid()) {
        framework_->ShowBrowserWindow(browser.value, browser == active);
      }
    }
  }
  browsers_->FocusActiveBrowser();
}

bool PhiShellWindow::PresentBrowser(BrowserId browser, bool activate) {
  const std::string space = browsers_->spaces().SpaceOf(browser);
  if (space.empty()) {
    return false;
  }
  // Never surface an agent shell or bypass the onboarding/access gate.
  if (agent_ || shell_hidden_ || destroying_) {
    return true;
  }
  if (present_behind_pending_) {
    // A background open's first show: on screen, never in front of what
    // the user is looking at, never active.
    present_behind_pending_ = false;
    ::ShowWindow(shell_hwnd_, SW_SHOWNOACTIVATE);
    if (present_behind_ && ::IsWindow(present_behind_) &&
        present_behind_ != shell_hwnd_) {
      ::SetWindowPos(shell_hwnd_, present_behind_, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    present_behind_ = nullptr;
    return true;
  }
  // Restore and background shows may reveal the selected session only.
  if (!activate && browser != active_browser_id()) {
    return true;
  }
  if (!activate &&
      (::IsWindowVisible(shell_hwnd_) || ::IsIconic(shell_hwnd_))) {
    return true;
  }
  if (browser != active_browser_id()) {
    ShowSpace(space);
  }
  ::ShowWindow(shell_hwnd_,
               activate ? (::IsIconic(shell_hwnd_) ? SW_RESTORE : SW_SHOW)
                        : SW_SHOWNOACTIVATE);
  if (activate) {
    ::SetForegroundWindow(shell_hwnd_);
    FocusActiveBrowser();
  }
  return true;
}

void PhiShellWindow::PresentBehindOnFirstShow(HWND above) {
  if (::IsWindowVisible(shell_hwnd_)) {
    // Already up (Chromium showed it while it was being hosted): only the
    // z-order is left to fix.
    if (above && ::IsWindow(above) && above != shell_hwnd_) {
      ::SetWindowPos(shell_hwnd_, above, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      ::SetForegroundWindow(above);
    }
    return;
  }
  present_behind_pending_ = true;
  present_behind_ = above;
}

void PhiShellWindow::HideShell() {
  if (shell_hidden_ || !shell_hwnd_ || agent_) {
    return;
  }
  shell_hidden_ = true;
  ::ShowWindow(shell_hwnd_, SW_HIDE);
}

void PhiShellWindow::MenuOpenFeedback() {
  OpenFeedback();
}

void PhiShellWindow::MenuCreateSpace() {
  ShowCreateSpaceForm();  // The sidebar form, or at once without one.
}

void PhiShellWindow::MenuEditSpaceTheme() {
  // The Mac's .windowTop placement: the menu bar has no trigger point to
  // hang the panel off, so it drops under the caption, centred on the window
  // (the panel centres itself in an anchor wider than it).
  if (!services_.spaces || active_space().empty() || !shell_hwnd_) {
    return;
  }
  RECT shell = {};
  ::GetWindowRect(shell_hwnd_, &shell);
  const RECT anchor = {shell.left, shell.top, shell.right, shell.top};
  PhiShowSpaceThemePanel(this, anchor, active_space());
}

void PhiShellWindow::MenuChangeSpaceIcon() {
  // The app menu's row: no pip to hang off, so the flyout drops under the
  // ⋯ button itself.
  if (!shell_hwnd_ || active_space().empty()) {
    return;
  }
  const RECT button = app_menu_rect();
  POINT top_left = {button.left, button.top};
  POINT bottom_right = {button.right, button.bottom};
  ::ClientToScreen(shell_hwnd_, &top_left);
  ::ClientToScreen(shell_hwnd_, &bottom_right);
  ShowSpaceIconPicker(active_space(),
                      {top_left.x, top_left.y, bottom_right.x, bottom_right.y});
}

void PhiShellWindow::ShowSpaceIconPicker(const std::string& space_id,
                                         const RECT& anchor_screen) {
  if (!services_.spaces || !shell_hwnd_) {
    return;
  }
  std::string current;
  for (const PhiSpaceView& space : services_.spaces->Spaces()) {
    if (space.id == space_id) {
      current = space.icon;
    }
  }
  std::string picked;
  {
    PhiModalScope modal(shell_hwnd_);  // The picker pumps its own loop.
    picked = PhiPickSpaceIcon(shell_hwnd_, CurrentPalette(), anchor_screen,
                              current);
  }
  if (!picked.empty() && services_.spaces) {
    services_.spaces->SetSpaceIcon(space_id, picked);
  }
}

void PhiShellWindow::MenuTriggerExtension(const std::string& extension_id) {
  const BrowserId browser = active_browser_id();
  if (!framework_ || !browser.valid() || !shell_hwnd_) {
    return;
  }
  // Anchored to the app-menu button, where the row was picked from.
  const Layout layout = ComputeLayout();
  RECT anchor = layout.app_menu;
  ::MapWindowPoints(shell_hwnd_, nullptr, reinterpret_cast<POINT*>(&anchor), 2);
  framework_->TriggerExtensionAction(browser.value, extension_id.c_str(),
                                     anchor.left, anchor.top,
                                     anchor.right - anchor.left,
                                     anchor.bottom - anchor.top);
}

bool PhiShellWindow::MenuSidebarShown() const {
  return !sidebar_collapsed_;
}

void PhiShellWindow::ShowAppMenu() {
  if (!app_menu_ || !shell_hwnd_ || app_menu_->open()) {
    return;
  }
  const Layout layout = ComputeLayout();
  // Under the ⋯ button, right-aligned to its trailing edge (the button
  // trails its row, so a left-aligned popup would run off the window). A
  // layout with no button (fullscreen) drops it at the window's top-right.
  POINT anchor = layout.app_menu.right > layout.app_menu.left
                     ? POINT{layout.app_menu.right, layout.app_menu.bottom}
                     : POINT{layout.client.right, layout.client.top};
  ::ClientToScreen(shell_hwnd_, &anchor);
  // The button paints pressed while the menu is up.
  hover_kind_ = HoverKind::kNav;
  hover_index_ = 5;
  ::InvalidateRect(shell_hwnd_, &layout.app_menu, FALSE);
  app_menu_->Show(anchor.x, anchor.y, /*right_align=*/true);
  ClearHover();
}

// --- PhiBrowserHostShell: what the content host asks of its shell ---------

HWND PhiShellWindow::BrowserHostShellWindow() const {
  return shell_hwnd_;
}

int PhiShellWindow::BrowserHostScaled(int design_units) const {
  return Scaled(design_units);
}

PhiPalette PhiShellWindow::BrowserHostPalette() const {
  return CurrentPalette();
}

RECT PhiShellWindow::BrowserHostContentRect() {
  return ComputeWindowLayout().content;
}

RECT PhiShellWindow::BrowserHostClientRect() {
  return ComputeWindowLayout().client;
}

HFONT PhiShellWindow::BrowserHostStatusFont() const {
  return small_font_;
}

phi_framework::PhiFramework* PhiShellWindow::BrowserHostFramework()
    const {
  return framework_;
}

bool PhiShellWindow::BrowserHostIsAgent() const {
  return agent_;
}

void PhiShellWindow::BrowserHostRelayout() {
  LayoutContents();
}

void PhiShellWindow::BrowserHostRefreshTabs() {
  RefreshTabs();
}

void PhiShellWindow::BrowserHostScheduleRefresh() {
  ScheduleRefresh();
}

void PhiShellWindow::BrowserHostBeginSpaceSwap(
    const std::string& space_id, BrowserId entering) {
  if (layout_mode_ == PhiLayoutMode::kComfortable) {
    // No band to push in: the whole client area slides (phi_content_slide.h).
    // The content host defers the page exactly as for the band slide
    // (BrowserHostSwapRunning): the entering child parks until the slide
    // lands, the leaving one stays up until the slide starts.
    BeginContentSlide(space_id, entering);
    return;
  }
  spaces_->BeginSwap(space_id);
}

bool PhiShellWindow::BrowserHostSwapRunning() const {
  return spaces_->swap_running() || content_slide_->running();
}

void PhiShellWindow::BrowserHostNoteSwapChildren(HWND reveal,
                                                      HWND conceal) {
  swap_reveal_ = reveal;
  swap_conceal_ = conceal;
}

void PhiShellWindow::BrowserHostEndSpacePreview() {
  if (spaces_->preview_awaits_switch()) {
    spaces_->EndPreview();
  }
}

void PhiShellWindow::BrowserHostNoteSpaceWindow(
    const std::string& space_id,
    BrowserId browser) {
  if (services_.spaces) {
    services_.spaces->NoteSpaceWindow(space_id, browser.value);
  }
}

void PhiShellWindow::BrowserHostNoteActiveSpace(
    const std::string& space_id) {
  if (!PhiIsAgentSpaceId(space_id) && !model::IsIncognitoSpaceId(space_id)) {
    last_regular_space_ = space_id;  // Where closing an Incognito Space lands.
  }
  // The Library's open waited for this Space (OpenStoredRowInSpace); a
  // different Space landing in front of the user means they moved on.
  ConsumePendingStoredOpen(space_id, /*drop_on_mismatch=*/true);
  // An Incognito Space is fixed-dark whatever the app's appearance: the
  // acrylic behind the sidebar follows (DWM's dark flag), or its light
  // tint shows through under the dark palette's white text.
  if (SettledDark() != backdrop_dark_) {
    UpdateSidebarBackdrop();
  }
  if (services_.spaces && !agent_ && !closing_ &&
      !PhiIsAgentSpaceId(space_id)) {
    services_.spaces->NoteSlotActiveSpace(space_id);
  }
  // Another browser is on screen: the Peek/Reader surfaces of the one that
  // left hide, the arriving one's show again.
  if (services_.sidebar) {
    services_.sidebar->NoteContentLayoutChanged(slot_.value);
  }
}

void PhiShellWindow::BrowserHostAbandonTyping() {
  omnibox_->ClosePopup(/*stop_query=*/true);
}

void PhiShellWindow::BrowserHostRetintChrome() {
  omnibox_->Repaint();
}

void PhiShellWindow::BrowserHostFocusAddressField() {
  FocusAddressField();
}

bool PhiShellWindow::BrowserHostFocusNativePage() {
  if (!native_pages_->ntp_wants_focus()) {
    return false;
  }
  native_pages_->FocusNtpField();
  return true;
}

bool PhiShellWindow::BrowserHostDeferSpaceClose(
    const std::string& space_id) {
  if (PhiModalDepthFor(shell_hwnd_) == 0) {
    return false;
  }
  deferred_space_closes_.push_back(space_id);
  PostDeferredClose();
  return true;
}

// --- The content area, forwarded to the component -------------------------

void PhiShellWindow::AttachSpaceBrowser(const std::string& space_id,
                                             BrowserId browser,
                                             HWND browser_hwnd) {
  browsers_->AttachSpaceBrowser(space_id, browser, browser_hwnd);
  // A Space the Library asked for arrives here as a FRESH browser (the
  // attach makes it active without the ShowSpace path's active-Space note):
  // its pending open runs now, against the browser just attached.
  ConsumePendingStoredOpen(space_id, /*drop_on_mismatch=*/false);
  PostDeferredClose();
}

bool PhiShellWindow::ShowSpace(const std::string& space_id) {
  if (HostsAgentSpace() && !PhiIsAgentSpaceId(space_id) &&
      active_space() == hosted_agent_space_) {
    // Any road out of a hosted agent Space — a pip, Ctrl+N, the Spaces
    // menu, a verb — is the end of the watch: the owner runs the swap
    // back (LeaveHostedAgentSpace) and hands the browser home after it.
    if (services_.agent) {
      services_.agent->RequestLeaveWatchedAgentSpace(space_id);
      return true;
    }
  }
  return browsers_->ShowSpace(space_id);
}

bool PhiShellWindow::HasSpace(const std::string& space_id) const {
  return browsers_->spaces().Has(space_id);
}

void PhiShellWindow::SwitchToSpace(const std::string& space_id) {
  if (space_id.empty() || space_id == active_space()) {
    return;
  }
  if (PhiIsAgentSpaceId(space_id)) {
    // An agent pip's click: the watch starts (or, hosted here already,
    // the hosted one hands over to it — one swap). The pip carries the
    // task the manager keys on.
    if (!services_.agent) {
      return;
    }
    for (const PhiAgentPipView& pip :
         services_.agent->AgentSpaces(agent_ ? 0 : slot_.value)) {
      if (pip.space_id == space_id) {
        services_.agent->RequestWatchAgentSpace(pip.task_id, slot_.value);
        ScheduleRefresh();  // The pip's watched state is the owner's.
        return;
      }
    }
    return;
  }
  if (agent_ || HostsAgentSpace()) {
    // A user Space picked while the agent Space is up: the Mac's
    // slot.activate — leave the agent Space and show that Space in the
    // window that was watching. The agent's hidden browser never switches
    // Space itself.
    if (services_.agent) {
      services_.agent->RequestLeaveWatchedAgentSpace(space_id);
    }
    return;
  }
  // Below here the user is switching Spaces (a pip, the menu, Ctrl+N): the
  // Mac's space_switched, counted whichever way the browser arrives.
  if (services_.window) {
    services_.window->NoteUserSpaceSwitched();
  }
  if (HasSpace(space_id)) {
    ShowSpace(space_id);
  } else if (services_.spaces) {
    // Not yet in this slot: the owner creates its browser in the Space's
    // profile and attaches it here.
    services_.spaces->RequestSwitchSpace(slot_.value, space_id);
  }
}

void PhiShellWindow::CloseSpaceBrowser(const std::string& space_id) {
  browsers_->CloseSpaceBrowser(space_id);
}

void PhiShellWindow::RetireSpaceBrowser(const std::string& space_id) {
  browsers_->RetireSpaceBrowser(space_id);
  PostDeferredClose();
}

void PhiShellWindow::RetireAndUnmapSpaceBrowser(const std::string& space_id) {
  browsers_->RetireAndUnmapSpaceBrowser(space_id);
  PostDeferredClose();
}

bool PhiShellWindow::NeedsSpaceReplacement() const {
  return browsers_->spaces().NeedsReplacement();
}

bool PhiShellWindow::ForgetBrowser(BrowserId browser) {
  return browsers_->ForgetBrowser(browser);
}

void PhiShellWindow::FocusActiveBrowser() {
  browsers_->FocusActiveBrowser();
}

void PhiShellWindow::FocusPageBrowser() {
  const BrowserId page = PageCommandBrowser();
  if (page == active_browser_id()) {
    FocusActiveBrowser();
    return;
  }
  // A presented Peek. As FocusActiveBrowser: never steal native focus from
  // another application for a deferred switch or a background callback.
  if (framework_ && page.valid() && ::GetForegroundWindow() == shell_hwnd_) {
    framework_->FocusBrowserWindow(page.value);
  }
}

void PhiShellWindow::RaiseCornerCaps() {
  browsers_->RequestRaiseCaps();
}

void PhiShellWindow::OnTargetURLChanged(BrowserId browser_id,
                                             const std::wstring& url) {
  browsers_->OnTargetURLChanged(browser_id, url);
}

void PhiShellWindow::OnPlaceholderModeChanged(BrowserId browser_id,
                                                   bool entered) {
  browsers_->OnPlaceholderModeChanged(browser_id, entered);
  RetitleShell();
}

void PhiShellWindow::OnContentFullscreenChanged(BrowserId browser_id,
                                                     bool fullscreen) {
  browsers_->OnContentFullscreenChanged(browser_id, fullscreen);
}

void PhiShellWindow::SetShellFullscreen(BrowserId browser,
                                        bool fullscreen,
                                        const RECT& target) {
  browsers_->SetShellFullscreen(browser, fullscreen, target);
}

void PhiShellWindow::OnContentBoundsChanged(BrowserId browser_id) {
  if (browser_id == active_browser_id()) {
    native_pages_->Reposition();
    PhiLibrary::RepositionIfBoundTo(this);
  }
}

void PhiShellWindow::ActivatePaneTab(int tab_id) {
  const BrowserId browser = active_browser_id();
  if (!framework_ || !browser.valid() || tab_id <= 0 ||
      active_tab_id().value == tab_id) {
    return;
  }
  framework_->ActivateTabById(browser.value, tab_id);
}

PhiOmniboxCapsule PhiShellWindow::OmniboxCapsule() {
  const Layout layout = ComputeWindowLayout();
  auto to_pure = [](const RECT& r) {
    return SidebarRect{r.left, r.top, r.right, r.bottom};
  };
  PhiOmniboxCapsule capsule;
  capsule.field = to_pure(layout.url_field);
  capsule.page_icon = to_pure(layout.page_icon);
  capsule.field_ext_strip = to_pure(layout.field_ext_strip);
  capsule.reader = to_pure(layout.reader);
  capsule.copy_link = to_pure(layout.copy_link);
  capsule.site_menu = to_pure(layout.site_menu);
  capsule.extensions = to_pure(layout.extensions);
  return capsule;
}

bool PhiShellWindow::OmniboxBrowserPlaceholding() const {
  return browsers_->ActiveBrowserPlaceholding();
}

bool PhiShellWindow::OmniboxNewTabFocusesField() const {
  return services_.prefs && services_.prefs->NewTabOpensOmnibox();
}

HWND PhiShellWindow::OmniboxNativePageField() const {
  return native_pages_->ntp_field();
}

bool PhiShellWindow::OmniboxNativePageBox(RECT* out) const {
  return native_pages_->NtpBoxScreenRect(out);
}

void PhiShellWindow::OmniboxNativePageCommitting() {
  // omniBoxDidClear: the native page blanks its controls now, so nothing of
  // the new-tab page lingers over the arriving one; the rebuild that sees
  // the new URL unmounts it.
  const TabId committing = active_tab_id();
  if (native_pages_->HideNtpControls()) {
    ntp_commit_tab_id_ = committing;
  }
}

// --- PhiNativePagesShell ----------------------------------------------------

HWND PhiShellWindow::NativePagesShellWindow() const {
  return shell_hwnd_;
}

PhiShellWindow* PhiShellWindow::NativePagesOwner() {
  return this;
}

bool PhiShellWindow::NativePagesIncognito() const {
  return ShowsIncognito();
}

bool PhiShellWindow::NativePagesIsAgent() const {
  return agent_;
}

bool PhiShellWindow::NativePagesContentFullscreen() const {
  return browsers_->content_fullscreen();
}

bool PhiShellWindow::NativePagesPlaceholding() const {
  return browsers_->ActiveBrowserPlaceholding();
}

bool PhiShellWindow::NativePagesSwapPending() const {
  return swap_reveal_ != nullptr;
}

const std::string& PhiShellWindow::NativePagesActiveUrl() const {
  return active_tab_url();
}

int PhiShellWindow::NativePagesActiveTabId() const {
  const TabId active = active_tab_id();
  return active.valid() ? active.value : 0;
}

std::vector<NativePane> PhiShellWindow::NativePagesSplitPanes() const {
  std::vector<NativePane> panes;
  const BrowserId browser = active_browser_id();
  const TabId active = active_tab_id();
  const SplitSnapshot* split = model_ && browser.valid() && active.valid()
                                   ? model_->SplitOfTab(browser, active)
                                   : nullptr;
  if (!split || !framework_ || !shell_hwnd_) {
    return panes;
  }
  for (const TabId tab : {split->primary, split->secondary}) {
    NativePane pane;
    pane.tab_id = tab.value;
    if (const TabSnapshot* snapshot = model_->FindTab(browser, tab)) {
      pane.url = snapshot->url;
    }
    // Where Chromium laid the pane out, in SCREEN pixels; mapped into the
    // shell, whose child the page is. Empty until the layout has run —
    // the framework's OnContentBoundsChanged repositions then.
    int left = 0, top = 0, right = 0, bottom = 0;
    if (framework_->TabContentBoundsById(browser.value, tab.value, &left,
                                         &top, &right, &bottom)) {
      RECT rect = {left, top, right, bottom};
      ::MapWindowPoints(nullptr, shell_hwnd_, reinterpret_cast<POINT*>(&rect),
                        2);
      pane.rect = rect;
    }
    panes.push_back(std::move(pane));
  }
  return panes;
}

void PhiShellWindow::NativePagesRaiseCaps() {
  browsers_->RequestRaiseCaps();
}

void PhiShellWindow::NativePagesFocusContent() {
  FocusActiveBrowser();
}

HWND PhiShellWindow::NativePagesContentWindow() const {
  return browsers_->content_hwnd();
}

bool PhiShellWindow::NativePagesCommitPending() const {
  return active_tab_id() == ntp_commit_tab_id_ && omnibox_->nav_loading();
}

// --- PhiSidebarShell: what the sidebar asks of its shell --------------------

PhiSidebarRects PhiShellWindow::BandRects(const Layout& layout) const {
  PhiSidebarRects rects;
  rects.sidebar = layout.sidebar;
  rects.title_bar = layout.title_bar;
  rects.content = layout.content;
  rects.favorites = layout.favorites;
  rects.bookmarks = layout.bookmarks;
  rects.stored_band = layout.stored_band;
  rects.bookmark_separator = layout.bookmark_separator;
  rects.new_tab_row = layout.new_tab_row;
  rects.bottom_bar = layout.bottom_bar;
  rects.spaces_strip = layout.spaces_strip;
  // The strip's first trailing control, which the "+" and a dragged tab
  // stop before: the Update reminder when one shows, else search.
  rects.search_tabs = layout.update.right > layout.update.left && layout.horizontal_tabs
                          ? layout.update
                          : layout.search_tabs;
  rects.tabs_left = layout.tabs_left;
  rects.tabs_top = layout.tabs_top;
  rects.tabs_limit = layout.tabs_limit;
  rects.horizontal_tabs = layout.horizontal_tabs;
  rects.spaces_switch = layout.spaces_switch;
  return rects;
}

HWND PhiShellWindow::SidebarShellWindow() const {
  if (floating_drag_ && floating_sidebar_) {
    return floating_sidebar_;  // The drag runs on the panel showing its rows.
  }
  return shell_hwnd_;
}

PhiShellWindow* PhiShellWindow::SidebarOwner() {
  return this;
}

int PhiShellWindow::SidebarScaled(int design_units) const {
  return Scaled(design_units);
}

UINT PhiShellWindow::SidebarDpi() const {
  return dpi_;
}

int PhiShellWindow::SidebarWidthDip() const {
  return sidebar_dip_;
}

PhiSidebarRects PhiShellWindow::SidebarRects() {
  return BandRects(ComputeLayout());
}

bool PhiShellWindow::SidebarSplitPaneRects(RECT* primary,
                                           RECT* secondary) const {
  const std::vector<NativePane> panes = NativePagesSplitPanes();
  if (panes.size() != 2 || ::IsRectEmpty(&panes[0].rect) ||
      ::IsRectEmpty(&panes[1].rect)) {
    return false;
  }
  *primary = panes[0].rect;
  *secondary = panes[1].rect;
  return true;
}

void PhiShellWindow::SidebarSetRowCounts(int pinned_rows,
                                              int bookmark_rows) {
  pinned_row_count_ = pinned_rows;
  bookmark_row_count_ = bookmark_rows;
}

void PhiShellWindow::SidebarSetDropZones(bool pinned, bool bookmark) {
  pinned_drop_zone_ = pinned;
  bookmark_drop_zone_ = bookmark;
}

void PhiShellWindow::SidebarRefreshLayoutInputs() {
  RefreshLayoutInputs();
}

PhiPalette PhiShellWindow::SidebarPalette() const {
  return CurrentPalette();
}

std::string PhiShellWindow::SidebarActiveSpace() const {
  return active_space();
}

BrowserId PhiShellWindow::SidebarActiveBrowser() const {
  return active_browser_id();
}

bool PhiShellWindow::SidebarPullSnapshot(
    BrowserId browser,
    phi_framework::WindowSnapshot* out) const {
  return PullSnapshot(browser, out);
}

bool PhiShellWindow::SidebarSplitIsStacked(TabId tab_id) const {
  return SplitIsStacked(tab_id);
}

HFONT PhiShellWindow::SidebarGlyphFont() const {
  return caption_font_;
}

HFONT PhiShellWindow::SidebarSemiboldFont() const {
  return ui_semibold_font_;
}

HFONT PhiShellWindow::SidebarTextFont() const {
  return ui_font_;
}

PhiChromeHover PhiShellWindow::SidebarHoverKind() const {
  return hover_kind_;
}

int PhiShellWindow::SidebarHoverIndex() const {
  return hover_index_;
}

bool PhiShellWindow::SidebarSurfaceTileHovered() const {
  return surface_tile_hot_;
}

bool PhiShellWindow::SidebarSurfaceFavicon(TabId origin_tab_id,
                                           std::vector<unsigned char>* bgra,
                                           int* width,
                                           int* height) const {
  return services_.sidebar && origin_tab_id.valid() &&
         services_.sidebar->SurfaceFaviconForOrigin(origin_tab_id.value, bgra,
                                                    width, height);
}

PhiSpacesStrip* PhiShellWindow::SidebarSpacesStrip() {
  return spaces_.get();
}

void PhiShellWindow::SidebarScheduleRefresh() {
  ScheduleRefresh();
}

void PhiShellWindow::SidebarSyncNativePages() {
  native_pages_->SyncIfRouteChanged();
}

PhiDragBitmap PhiShellWindow::SidebarCaptureChrome(
    const RECT& client_rect) {
  return CaptureChromeRect(client_rect);
}

HWND PhiShellWindow::SidebarContentWindow() const {
  return browsers_->content_hwnd();
}

void PhiShellWindow::SidebarStartNewTab() {
  StartNewTab();
}

// The rows, as assistive technology reads them. The list is THE data source
// the UIA provider (phi_uia.h) walks — one fragment per hit, named, typed
// and invokable through the same messages a mouse click sends.
void PhiShellWindow::SidebarPublishHits(std::vector<PhiHit> hits) {
  hits_ = std::move(hits);
  ++hits_version_;
  // Cheap unless a provider was actually handed out: it tells a listening
  // screen reader the tab list changed shape rather than leaving it on a
  // stale tree.
  PhiUiaHitsChanged(this);
}

HWND PhiShellWindow::hwnd() const {
  return shell_hwnd_;
}

const std::vector<PhiHit>& PhiShellWindow::hits() const {
  return hits_;
}

uint32_t PhiShellWindow::hits_version() const {
  return hits_version_;
}

int PhiShellWindow::HitTest(POINT client_pt) const {
  return PhiHitAt(hits_, client_pt.x, client_pt.y);
}

bool PhiShellWindow::InvokeHit(int index) {
  // The mouse path, not a copy of it — the same clicks the sidebar handles.
  int x = 0;
  int y = 0;
  if (!shell_hwnd_ || !PhiUiaInvokePoint(hits_, index, &x, &y)) {
    return false;
  }
  // LOCALS ONLY below: a click can close this Space and destroy the shell.
  const HWND hwnd = shell_hwnd_;
  const LPARAM point =
      MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y));
  ::SendMessageW(hwnd, WM_LBUTTONDOWN, 0, point);
  if (::IsWindow(hwnd)) {
    ::SendMessageW(hwnd, WM_LBUTTONUP, 0, point);
  }
  return true;
}

// --- PhiSpacesStripShell ----------------------------------------------------

PhiSpacesStripRects PhiShellWindow::StripRects(
    const Layout& layout) const {
  PhiSpacesStripRects rects;
  rects.client = layout.client;
  rects.strip = layout.spaces_strip;
  rects.strip_repaint =
      layout.horizontal_tabs ? layout.title_bar : layout.spaces_strip;
  rects.sidebar = layout.sidebar;
  rects.add_space = layout.add_space;
  rects.chip_size = layout.chip_size;
  rects.chips_left = layout.chips_left;
  rects.chips_top = layout.chips_top;
  rects.chips_limit = layout.chips_limit;
  rects.horizontal_tabs = layout.horizontal_tabs;
  rects.spaces_switch = layout.spaces_switch;
  return rects;
}

HWND PhiShellWindow::SpacesShellWindow() const {
  return shell_hwnd_;
}

PhiShellWindow* PhiShellWindow::SpacesOwner() {
  return this;
}

SlotId PhiShellWindow::SpacesSlot() const {
  return slot_;
}

bool PhiShellWindow::SpacesIncognito() const {
  return incognito_;
}

bool PhiShellWindow::SpacesIsAgent() const {
  return agent_;
}

int PhiShellWindow::SpacesScaled(int design_units) const {
  return Scaled(design_units);
}

PhiSpacesStripRects PhiShellWindow::SpacesRects() {
  return StripRects(ComputeLayout());
}

const std::string& PhiShellWindow::SpacesActiveSpace() const {
  return active_space();
}

bool PhiShellWindow::SpacesHasSpace(const std::string& space_id) const {
  return HasSpace(space_id);
}

PhiPalette PhiShellWindow::SpacesPalette() const {
  return CurrentPalette();
}

bool PhiShellWindow::SpacesDragActive() const {
  return sidebar_ && sidebar_->drag_active();
}

int PhiShellWindow::SpacesHoveredChip() const {
  return hover_kind_ == HoverKind::kChip ? hover_index_ : -1;
}

int PhiShellWindow::SpacesTabCount(const std::string& space_id) const {
  const BrowserId browser = browsers_->spaces().ForSpace(space_id);
  const PhiWindowModel::Snapshot* window =
      model_ && browser.valid() ? model_->Get(browser) : nullptr;
  return window ? static_cast<int>(window->tabs.size()) : 0;
}

bool PhiShellWindow::SpacesAddSlotHovered() const {
  return hover_kind_ == HoverKind::kAddSpace;
}

RECT PhiShellWindow::SpacesAddSlotDirtyRect() {
  return HoverRect(HoverKind::kAddSpace, 0);
}

void PhiShellWindow::SpacesForgetHover() {
  hover_kind_ = HoverKind::kNone;
  hover_index_ = -1;
}

void PhiShellWindow::SpacesRehoverAtCursor() {
  POINT cursor = {};
  if (shell_hwnd_ && ::GetCursorPos(&cursor)) {
    ::ScreenToClient(shell_hwnd_, &cursor);
    OnChromeMouseMove(cursor);
  }
}

void PhiShellWindow::SpacesScheduleRefresh() {
  ScheduleRefresh();
}

void PhiShellWindow::SpacesApplyPalette() {
  ApplyPalette();
}

void PhiShellWindow::SpacesRefreshTinted() {
  native_pages_->Refresh();
  browsers_->RefreshCornerCaps();
}

void PhiShellWindow::SpacesRepaintOmnibox() {
  omnibox_->Repaint();
}

void PhiShellWindow::SpacesFocusContent() {
  FocusActiveBrowser();
}

void PhiShellWindow::SpacesSetCoveredContentSuppressed(bool suppressed) {
  // The EDIT has no WS_CLIPSIBLINGS, so it paints straight through a panel
  // raised over it; only hiding it keeps the address text off the form.
  if (suppressed) {
    omnibox_->ClosePopup(/*stop_query=*/true);
  }
  omnibox_->SetFieldVisible(!OmniboxFieldHidden());
}

bool PhiShellWindow::OmniboxFieldHidden() const {
  // While the comfortable Space slide runs the EDIT is down and the frame
  // paints the address itself (include_child_controls): a real child window
  // moves a frame apart from the parent's paint and would ghost.
  // The collapsed performance shell has no address bar of its own — but
  // the floating panel shows the sidebar's, and while the EDIT is hosted
  // there (ShowFloatingSidebar) it is that panel's live field.
  // The address row is the pane's in every layout now: a collapsed
  // sidebar never takes the field with it (the Performance layout, which
  // kept the field in the sidebar, is retired — impl log D9).
  return browsers_->content_fullscreen() || spaces_->creating() ||
         (content_slide_ && content_slide_->running());
}

void PhiShellWindow::FocusAddressField() {
  // Ctrl+L / Open Location / placeholder mode's way in. The collapsed
  // performance shell has no field of its own: the floating panel's is the
  // one to focus, so the panel comes up first (the Mac's Cmd+L raises its
  // omnibox overlay wherever the sidebar is; this is the panel's turn at
  // that). The focused field pins the panel open (FloatingSidebarTick).
  omnibox_->FocusFieldIfVisible();
}

void PhiShellWindow::SpacesRevealSwappedContent() {
  RevealSwappedContent();
}

void PhiShellWindow::SpacesSwapBandModel() {
  sidebar_->SwapBandModel();
}

void PhiShellWindow::SpacesDropLeavingBand() {
  sidebar_->DropLeavingBand();
}

HFONT PhiShellWindow::SpacesUiFont() const {
  return ui_font_;
}

HFONT PhiShellWindow::SpacesSmallFont() const {
  return small_font_;
}

HFONT PhiShellWindow::SpacesEmojiFont() const {
  return emoji_font_;
}

HFONT PhiShellWindow::SpacesChipIconFont() const {
  return chip_icon_font_;
}

HFONT PhiShellWindow::SpacesPipIconFont() const {
  return pip_icon_font_;
}

HFONT PhiShellWindow::SpacesPipEmojiFont() const {
  return pip_emoji_font_;
}

HFONT PhiShellWindow::SpacesPipTextFont() const {
  return pip_text_font_;
}

bool PhiShellWindow::SpacesSidebarCanHostForm(const RECT& form) const {
  // CreateSpacePanel.requestCreation: the vertical layouts host the form
  // inline in the docked sidebar. The horizontal layout, a collapsed
  // sidebar (the floating panel does not host it here) and windows without
  // a Spaces strip create at once instead.
  return !incognito_ && !agent_ && !floating_layout_ && !sidebar_collapsed_ &&
         layout_mode_ != PhiLayoutMode::kComfortable &&
         form.right - form.left > Scaled(120) &&
         form.bottom - form.top > Scaled(200);
}

bool PhiShellWindow::CancelSidebarDrag() {
  return sidebar_ && sidebar_->CancelDragEscape();
}

bool PhiShellWindow::SpacesCreateFlyoutAnchor(RECT* screen) const {
  if (incognito_ || agent_ || !shell_hwnd_ || !spaces_) {
    return false;
  }
  RECT anchor = {};
  if (BandOnShell()) {
    // The "+" / "…" slot when the strip shows one, else the active chip
    // (the horizontal layout's Space chip).
    const PhiSpacesStripRects rects =
        const_cast<PhiShellWindow*>(this)->SpacesRects();
    if (rects.add_space.right > rects.add_space.left) {
      anchor = rects.add_space;
    } else {
      const std::vector<PhiSpaceChip>& chips = spaces_->chips();
      for (size_t i = 0; i < chips.size(); ++i) {
        if (chips[i].id == active_space()) {
          anchor = chips[i].bounds;
        }
      }
    }
  }
  if (anchor.right <= anchor.left) {
    anchor = const_cast<PhiShellWindow*>(this)->ComputeLayout().sidebar_toggle;
  }
  if (anchor.right <= anchor.left) {
    anchor = const_cast<PhiShellWindow*>(this)->app_menu_rect();
  }
  if (anchor.right <= anchor.left) {
    return false;
  }
  *screen = LayoutToScreen(anchor);
  return true;
}

HWND PhiShellWindow::SpacesFloatingPanel(int* offset_y) const {
  if (floating_layout_ && floating_sidebar_ && ::IsWindow(floating_sidebar_)) {
    if (offset_y) {
      *offset_y = floating_offset_y_;
    }
    return floating_sidebar_;
  }
  return nullptr;
}

POINT PhiShellWindow::LayoutToScreen(POINT pt) const {
  int floating_offset = 0;
  if (const HWND panel = SpacesFloatingPanel(&floating_offset)) {
    pt.y -= floating_offset;
    ::ClientToScreen(panel, &pt);
  } else if (shell_hwnd_) {
    ::ClientToScreen(shell_hwnd_, &pt);
  }
  return pt;
}

RECT PhiShellWindow::LayoutToScreen(const RECT& rect) const {
  const POINT top_left = LayoutToScreen(POINT{rect.left, rect.top});
  const POINT bottom_right = LayoutToScreen(POINT{rect.right, rect.bottom});
  return {top_left.x, top_left.y, bottom_right.x, bottom_right.y};
}

void PhiShellWindow::AttachOmniboxField(HWND edit) {
  omnibox_->AttachField(edit);
}

void PhiShellWindow::OnOmniboxFieldChanged(HWND field) {
  omnibox_->OnFieldChanged(field);
}

void PhiShellWindow::OnOmniboxResults(BrowserId browser_id,
                                           const std::string& json) {
  omnibox_->OnResults(browser_id, json);
}

void PhiShellWindow::StartNewTab() {
  omnibox_->StartNewTab();
}

void PhiShellWindow::RebuildNavState() {
  // The field describes the page in front of the user: the Peek presented
  // over the active tab, else the tab itself (PageCommandBrowser — the
  // Mac's pageCommandTarget seeding the omnibox, PHI-1745). Back/Forward
  // stay the strip tab's: over a peek they close it rather than navigate
  // (LeaveReaderViewForNavigation), so their enablement is the opener's.
  const BrowserId browser = active_browser_id();
  const BrowserId page = PageCommandBrowser();
  const phi_framework::WindowSnapshot* snapshot =
      model_ && page.valid() ? model_->Get(page) : nullptr;
  phi_framework::WindowSnapshot pulled;
  if (!snapshot && page != browser && page.valid() &&
      PhiWindowModel::Pull(framework_, page, &pulled)) {
    snapshot = &pulled;  // A peek presented before its first push.
  }
  if (!snapshot) {
    omnibox_->RebuildNavState(nullptr, page);
    return;
  }
  phi_framework::NavState nav = snapshot->nav;
  if (page != browser) {
    if (const phi_framework::WindowSnapshot* opener = model_->Get(browser)) {
      nav.can_go_back = opener->nav.can_go_back;
      nav.can_go_forward = opener->nav.can_go_forward;
    }
  }
  omnibox_->RebuildNavState(&nav, page);
}

void PhiShellWindow::OpenFeedback() {
  if (!services_.window) {
    return;
  }
  // About the focused tab of the VISIBLE browser (the active Space's), whose
  // profile also supplies the report's system logs.
  const BrowserId browser = active_browser_id();
  if (!browser.valid()) {
    return;  // No browser, no focused tab to report about.
  }
  PhiFeedbackContext context;
  context.window_id = browser.value;
  context.incognito = incognito_ || model::IsIncognitoSpaceId(active_space());
  for (const TabRow& row : sidebar_->tabs()) {
    if (row.active) {
      context.url = base::WideToUTF8(row.url);
      context.title = base::WideToUTF8(row.title);
      break;
    }
  }
  if (context.url.empty()) {
    context.url = active_tab_url();
  }
  services_.window->RequestFeedback(slot_.value, context);
}

RECT PhiShellWindow::ContentRect() {
  return ComputeWindowLayout().content;
}

void PhiShellWindow::ToggleSidebar() {
  SetSidebarCollapsed(!sidebar_collapsed_);
}

void PhiShellWindow::SetSidebarCollapsed(bool collapsed) {
  if (ComputeLayout().horizontal_tabs || collapsed == sidebar_collapsed_) {
    return;  // Comfortable has no sidebar to collapse.
  }
  sidebar_collapsed_ = collapsed;
  if (!collapsed) {
    DestroyFloatingPanel();
  }
  LayoutContents();  // Creates the tucked panel for a hidden sidebar.
  if (shell_hwnd_) {
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
  }
}

void PhiShellWindow::ToggleDownloadsPopover() {
  // The menu / Ctrl+J entry to the same popover the sidebar button toggles.
  // The popover's own opening grace period absorbs the menu teardown's
  // activation churn, so this can run inline.
  const BrowserId browser = active_browser_id();
  if (!services_.spaces || !framework_ || !shell_hwnd_ || !browser.valid()) {
    return;
  }
  // Anchored at the sidebar's Downloads button, or — no bottom bar (a
  // collapsed sidebar, Comfortable) — at the app menu button, the row's
  // trailing control.
  const Layout layout = ComputeLayout();
  RECT anchor = layout.sidebar_downloads.right > layout.sidebar_downloads.left
                    ? layout.sidebar_downloads
                    : layout.app_menu;
  ::MapWindowPoints(shell_hwnd_, nullptr, reinterpret_cast<POINT*>(&anchor),
                    2);
  PhiDownloadsPopover::Toggle(shell_hwnd_, anchor,
                              services_.spaces->PaletteForSlot(slot_.value),
                              framework_, browser);
}

void PhiShellWindow::BeginSidebarResize() {
  CancelContentSlide();
  sidebar_resize_source_ = ComputeWindowLayout().content;
  sidebar_resize_snapshot_ = CaptureLeavingComposite();
  sidebar_resizing_ = true;
  sidebar_resize_content_ = browsers_->content_hwnd();
  if (sidebar_resize_snapshot_) {
    // Native chrome and Chromium present on different clocks. During the
    // drag, paint the page preview and chrome in ONE shell frame; keep the
    // real viewport unchanged until release.
    for (HWND child = ::GetWindow(shell_hwnd_, GW_CHILD); child;
         child = ::GetWindow(child, GW_HWNDNEXT)) {
      if (::IsWindowVisible(child)) {
        sidebar_resize_hidden_.push_back(child);
      }
    }
    for (HWND child : sidebar_resize_hidden_) {
      ::ShowWindow(child, SW_HIDE);
    }
    ::RedrawWindow(shell_hwnd_, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_UPDATENOW);
  }
  ::SetCapture(shell_hwnd_);
}

void PhiShellWindow::ResizeSidebar(int width_dip) {
  sidebar_dip_ = width_dip;
  if (!sidebar_resize_snapshot_) {
    LayoutContents();  // Allocation/capture failure retains live resizing.
  } else {
    RefreshLayoutInputs();
    ScheduleRefresh();
  }
  FlushPendingRebuild();
  ::RedrawWindow(shell_hwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW);
}

void PhiShellWindow::EndSidebarResize() {
  if (sidebar_resize_snapshot_) {
    ::DeleteObject(sidebar_resize_snapshot_);
    sidebar_resize_snapshot_ = nullptr;
    if (!destroying_) {
      // Only what still belongs on screen. The browser child is LayoutContents'
      // to show (the current Space's, and not in placeholder mode), so a
      // Space switch or an attach that landed mid-drag does not bring the
      // previous browser back; a child handed to another window is not ours.
      for (HWND child : sidebar_resize_hidden_) {
        if (child != sidebar_resize_content_ && ::IsWindow(child) &&
            ::GetParent(child) == shell_hwnd_) {
          ::ShowWindow(child, SW_SHOWNA);
        }
      }
      // Keep sidebar_resizing_ set while laying out: the live width wins
      // over the preference, which the release handler persists next.
      LayoutContents();
      FlushPendingRebuild();
      ::RedrawWindow(shell_hwnd_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
      // Navigation may have requested a native-page sync while its window
      // was hidden for the preview. Apply the current route after release.
      ::PostMessageW(shell_hwnd_, kMsgPhiSyncNativePages, 0, 0);
    }
    sidebar_resize_hidden_.clear();
  }
  sidebar_resize_content_ = nullptr;
  sidebar_resizing_ = false;
}

void PhiShellWindow::PaintSidebarResizePreview(HDC dc) {
  const RECT target = ComputeWindowLayout().content;
  HDC source = ::CreateCompatibleDC(dc);
  if (!source) return;
  HGDIOBJ old = ::SelectObject(source, sidebar_resize_snapshot_);
  // A scaled preview keeps all page content visible; the real page reflows
  // once, at the final viewport size, when the mouse is released.
  const int mode = ::SetStretchBltMode(dc, HALFTONE);
  POINT brush_origin = {};
  ::SetBrushOrgEx(dc, 0, 0, &brush_origin);
  ::StretchBlt(dc, target.left, target.top,
      target.right - target.left, target.bottom - target.top, source,
      sidebar_resize_source_.left, sidebar_resize_source_.top,
      sidebar_resize_source_.right - sidebar_resize_source_.left,
      sidebar_resize_source_.bottom - sidebar_resize_source_.top, SRCCOPY);
  ::SetBrushOrgEx(dc, brush_origin.x, brush_origin.y, nullptr);
  ::SetStretchBltMode(dc, mode);
  ::SelectObject(source, old);
  ::DeleteDC(source);
  ::GdiFlush();
  // The capture is opaque GDI RGB. Supply alpha for presentation over the
  // extended DWM frame, keeping the sidebar's existing translucent pixels.
  auto pixels = UNSAFE_BUFFERS(base::span<uint32_t>(
      static_cast<uint32_t*>(back_buffer_bits_),
      static_cast<size_t>(back_buffer_size_.cx) * back_buffer_size_.cy));
  for (int y = std::max<LONG>(0, target.top);
       y < std::min(target.bottom, back_buffer_size_.cy); ++y) {
    const int left = std::clamp<LONG>(target.left, 0, back_buffer_size_.cx);
    const int right = std::clamp<LONG>(target.right, left, back_buffer_size_.cx);
    for (auto& pixel : pixels.subspan(
             static_cast<size_t>(y) * back_buffer_size_.cx + left,
             static_cast<size_t>(right - left))) {
      pixel |= 0xFF000000u;
    }
  }
}

void PhiShellWindow::LayoutContents() {
  if (sidebar_resize_snapshot_) {
    return;  // The live viewport is committed once when the drag ends.
  }
  if (destroying_) {
    return;  // The shell's own DestroyWindow sends WM_SIZE on the way down.
  }
  if (shell_hwnd_ && ::IsIconic(shell_hwnd_)) {
    // Minimized, the client is 0x0: laying out would shrink the browser
    // child to nothing (a page relayout, and the placement Chromium saves
    // on a close while minimized). The restore's WM_SIZE lays out once.
    return;
  }
  native_pages_->Reposition();
  PhiLibrary::RepositionIfBoundTo(this);  // Covers the client area.
  // Reader and peek panels are hosted OVER the content card, in windows the
  // coordinator owns rather than this controller — they have to follow it.
  if (services_.sidebar) {
    services_.sidebar->NoteContentLayoutChanged(slot_.value);
  }
  if (!shell_hwnd_) {
    return;
  }
  RefreshLayoutInputs();  // The owner pushes width / bookmark-bar prefs here.
  ScheduleRefresh();      // Row bounds derive from the client size.
  ::InvalidateRect(shell_hwnd_, nullptr, FALSE);  // Every band moved.
  browsers_->SetStatusUrl(std::wstring());  // Anchored to the OLD rect.
  omnibox_->ClosePopup(/*stop_query=*/false);  // Anchored to the OLD rect.
  // A tab/Space action or synchronous window message may relayout while
  // floating_layout_ is set for popup hit testing. Only its rows expand;
  // the actual browser viewport must retain the collapsed shell's bounds.
  const Layout layout = ComputeWindowLayout();
  omnibox_->Position();
  UpdateSidebarBackdrop();
  if (spaces_->creating()) {
    // The form lives only in a docked vertical sidebar: a layout switch or
    // a collapse ends it (the Mac re-routes to its standalone window).
    if (layout_mode_ == PhiLayoutMode::kComfortable || sidebar_collapsed_) {
      spaces_->CancelCreateForm();
    } else {
      spaces_->RepositionForm();
    }
  }
  omnibox_->SetFieldVisible(!OmniboxFieldHidden());
  PlaceContentChildren(layout);
  PushSplitViewStyle();  // A layout-mode or Space change moves its inputs.
  // The tucked panel exists exactly while the sidebar is hidden in the
  // vertical layout; a layout switch takes it down. Content fullscreen
  // keeps it (the Mac's floating sidebar works over a fullscreen video):
  // the shell itself is the fullscreen window (SetShellFullscreen) and the
  // panel its owned popup, so it stacks over the page's child.
  if (sidebar_collapsed_ && !layout.horizontal_tabs && !shell_hidden_) {
    EnsureFloatingPanel();
    PositionFloatingPanel();
  } else {
    DestroyFloatingPanel();
  }
}

void PhiShellWindow::PlaceContentChildren(const Layout& layout) {
  if (sidebar_resize_snapshot_) return;
  PhiContentLayout content;
  content.content = layout.content;
  content.client = layout.client;
  // The address row owns the pane's top corners; the bottom-left cap
  // rounds the pane against the tint, and the bottom-right only when the
  // pane is not flush with the window's edge (DWM rounds the window there).
  content.top_caps = false;
  content.right_caps = layout.content.right < layout.client.right;
  // Compositor clipping exposes the actual backdrop. Opaque sibling caps
  // would cover it again; retain them only for the opaque fallback. A split
  // keeps them: its flush panes meet the card's corners as a page does.
  content.corner_radius = sidebar_backdrop_ ? 0 : Scaled(kContentRadius);
  // Either Space switch parks the entering child until it lands.
  content.parked = BrowserHostSwapRunning() ? swap_reveal_ : nullptr;
  browsers_->PositionContentChild(content);

  // Watch-mode overlay (fog + control pill) tracks the content rect. It goes
  // between the two: the caps must end up above it.
  PhiAgentOverlay::Reposition();

  browsers_->PositionCornerCaps(content);
}

// --- The comfortable layout's Space switch (phi_content_slide.h) ------------

void PhiShellWindow::BeginContentSlide(const std::string& space_id,
                                       BrowserId entering) {
  if (!shell_hwnd_ || destroying_ || incognito_ || agent_ || shell_hidden_ ||
      space_id == active_space()) {
    return;
  }
  if (content_slide_->running()) {
    // A switch on top of a switch: land the first, and let the screen show
    // it before the composite is read off — the desktop composes on its own
    // clock.
    CancelContentSlide();
    ::RedrawWindow(shell_hwnd_, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    ::DwmFlush();
  }
  RECT client = {};
  ::GetClientRect(shell_hwnd_, &client);
  HBITMAP leaving = CaptureLeavingComposite();
  if (!leaving) {
    return;  // Nothing to slide out: the switch shows at once.
  }
  // The entering page comes from the framework once the switch under this
  // has run (StartSlideCapture, on the posted turn): the Space's browser
  // — the one being attached, or the one the table holds — and that
  // browser's active tab.
  slide_entering_browser_ = entering;
  slide_entering_tab_ = TabId();
  if (!slide_entering_browser_.valid() && browsers_->spaces().Has(space_id)) {
    slide_entering_browser_ = browsers_->spaces().ForSpace(space_id);
  }
  if (const phi_framework::TabSnapshot* tab =
          model_ && slide_entering_browser_.valid()
              ? model_->ActiveTab(slide_entering_browser_)
              : nullptr) {
    slide_entering_tab_ = tab->id;
  }
  // A fresh id per switch: a capture from a switch already landed or cut
  // short must not fill in this one's page.
  static int next_request = 0;
  slide_capture_request_ = ++next_request;
  slide_capture_tries_ = 0;
  content_slide_->Begin(leaving, client.right, client.bottom,
                        spaces_->SlideForward(space_id));
  if (!content_slide_->running()) {
    return;
  }
  // The EDIT goes down for the slide (OmniboxFieldHidden); the content
  // host's relayout right after this would show it again otherwise.
  omnibox_->SetFieldVisible(!OmniboxFieldHidden());
  ::PostMessageW(shell_hwnd_, kMsgPhiSlideCapture, 0, 0);
}

HBITMAP PhiShellWindow::CaptureLeavingComposite() {
  if (!shell_hwnd_ || ::IsIconic(shell_hwnd_) ||
      !::IsWindowVisible(shell_hwnd_)) {
    return nullptr;
  }
  RECT client = {};
  ::GetClientRect(shell_hwnd_, &client);
  if (client.right <= 0 || client.bottom <= 0) {
    return nullptr;
  }
  HDC screen = ::GetDC(nullptr);
  HDC mem = ::CreateCompatibleDC(screen);
  HBITMAP bitmap = ::CreateCompatibleBitmap(screen, client.right, client.bottom);
  if (!mem || !bitmap) {
    if (bitmap) {
      ::DeleteObject(bitmap);
    }
    if (mem) {
      ::DeleteDC(mem);
    }
    ::ReleaseDC(nullptr, screen);
    return nullptr;
  }
  HGDIOBJ old = ::SelectObject(mem, bitmap);
  HBRUSH fill = ::CreateSolidBrush(BackgroundFill());
  ::FillRect(mem, &client, fill);
  ::DeleteObject(fill);
  const Layout layout = ComputeLayout();
  {
    // The same frame WM_PAINT composes, minus the caption buttons: they
    // stay put over the slide and are painted once, unslid, by the frame.
    // The address text is painted in (the EDIT itself is not captured).
    base::AutoReset<bool> suppress(&caption_buttons_suppressed_, true);
    PaintChrome(mem, layout, CurrentPalette(), /*include_child_controls=*/true);
  }
  // The page — and whatever sits over it (a native page, the watch fog):
  // the screen has it right now.
  const int w = layout.content.right - layout.content.left;
  const int h = layout.content.bottom - layout.content.top;
  if (w > 0 && h > 0) {
    POINT origin = {layout.content.left, layout.content.top};
    ::ClientToScreen(shell_hwnd_, &origin);
    ::BitBlt(mem, layout.content.left, layout.content.top, w, h, screen,
             origin.x, origin.y, SRCCOPY);
  }
  ::GdiFlush();
  ::SelectObject(mem, old);
  ::DeleteDC(mem);
  ::ReleaseDC(nullptr, screen);
  return bitmap;
}

void PhiShellWindow::StartSlideCapture() {
  if (!content_slide_->holding()) {
    return;
  }
  const bool asked =
      framework_ && slide_entering_browser_.valid() &&
      slide_entering_tab_.valid() &&
      framework_->StartTabSurfaceCapture(slide_entering_browser_.value,
                                         slide_entering_tab_.value,
                                         slide_capture_request_);
  if (!asked) {
    ReleaseContentSlide();  // Nothing to fetch: the page area slides blank.
    return;
  }
  // The view was shown a turn ago and may hold no frame yet: ask again
  // every tick until one comes, for a few ticks.
  if (slide_capture_tries_ == 0) {
    ::SetTimer(shell_hwnd_, kSlideHoldTimer, kSlideHoldTickMs, nullptr);
  }
  ++slide_capture_tries_;
}

bool PhiShellWindow::OnTabSurfaceCaptured(int request_id,
                                          const unsigned char* bgra,
                                          int width,
                                          int height) {
  if (request_id != slide_capture_request_) {
    return false;
  }
  if (!content_slide_->holding()) {
    return true;  // Released already (the hold ran out, or a cancel).
  }
  if (!bgra || width <= 0 || height <= 0) {
    // No frame yet: the hold timer asks again, or gives up.
    return true;
  }
  content_slide_->SetEnteringPage(bgra, width, height,
                                  ComputeLayout().content);
  ReleaseContentSlide();
  return true;
}

void PhiShellWindow::ReleaseContentSlide() {
  if (shell_hwnd_) {
    ::KillTimer(shell_hwnd_, kSlideHoldTimer);
  }
  if (!content_slide_->holding()) {
    return;
  }
  content_slide_->Release();
  // The leaving page leaves the screen for its composite: the slide owns
  // the content rect from here until it lands (RevealSwappedContent). The
  // composite must cover the parent beneath the child and reach DWM before
  // the outgoing child is hidden, or the transparent shell shows for a frame.
  ComposeSlideFrame();
  browsers_->HidePreviousContent(swap_conceal_);
}

void PhiShellWindow::CancelContentSlide() {
  if (shell_hwnd_) {
    ::KillTimer(shell_hwnd_, kSlideHoldTimer);
  }
  content_slide_->Cancel();
}

HWND PhiShellWindow::ContentSlideWindow() const {
  return shell_hwnd_;
}

void PhiShellWindow::ContentSlideMoved() {
  if (destroying_ || !shell_hwnd_) {
    return;
  }
  ComposeSlideFrame();
}

void PhiShellWindow::ContentSlideLanded() {
  if (destroying_ || !shell_hwnd_) {
    return;
  }
  ::KillTimer(shell_hwnd_, kSlideHoldTimer);
  RevealSwappedContent();  // The entering child shows; the leaving hides.
  LayoutContents();        // The EDIT comes back; the frame repaints.
}

bool PhiShellWindow::InSidebarResizeBand(POINT client_pt) {
  if (!services_.prefs ||
      services_.prefs->LayoutMode() == PhiLayoutMode::kComfortable) {
    return false;
  }
  const Layout layout = ComputeLayout();
  return client_pt.y >= layout.title_bar.bottom &&
         client_pt.x >= Scaled(sidebar_dip_) - Scaled(4) &&
         client_pt.x <= Scaled(sidebar_dip_);
}






PhiDragBitmap PhiShellWindow::CaptureChromeRect(const RECT& client_rect) {
  PhiDragBitmap out;
  if (!shell_hwnd_) {
    return out;
  }
  RECT client = {};
  ::GetClientRect(shell_hwnd_, &client);
  RECT clipped = {};
  if (!::IntersectRect(&clipped, &client_rect, &client)) {
    return out;
  }
  const int frame_w = client.right;
  const int frame_h = client.bottom;
  if (frame_w <= 0 || frame_h <= 0) {
    return out;
  }
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = frame_w;
  info.bmiHeader.biHeight = -frame_h;  // Top-down.
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = ::GetDC(nullptr);
  HDC mem = ::CreateCompatibleDC(screen);
  HBITMAP dib =
      ::CreateDIBSection(mem, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  ::ReleaseDC(nullptr, screen);
  if (!dib || !bits) {
    if (dib) {
      ::DeleteObject(dib);
    }
    ::DeleteDC(mem);
    return out;
  }
  HGDIOBJ old = ::SelectObject(mem, dib);
  HBRUSH fill = ::CreateSolidBrush(BackgroundFill());
  ::FillRect(mem, &client, fill);
  ::DeleteObject(fill);
  // The same frame WM_PAINT composes, into our own bits.
  PaintChrome(mem, ComputeLayout(), CurrentPalette(),
              /*include_child_controls=*/false);
  ::GdiFlush();
  const int w = clipped.right - clipped.left;
  const int h = clipped.bottom - clipped.top;
  const size_t row_bytes = static_cast<size_t>(w) * 4;
  // SAFETY: the DIB was created frame_w x frame_h at 32bpp, top-down, so it
  // holds exactly frame_w * frame_h * 4 bytes with no stride padding.
  const auto frame = UNSAFE_BUFFERS(base::span<const unsigned char>(
      static_cast<const unsigned char*>(bits),
      static_cast<size_t>(frame_w) * static_cast<size_t>(frame_h) * 4));
  out.bgra.resize(row_bytes * static_cast<size_t>(h));
  base::span<unsigned char> dest(out.bgra);
  for (int y = 0; y < h; ++y) {
    const size_t from =
        (static_cast<size_t>(clipped.top + y) * static_cast<size_t>(frame_w) +
         static_cast<size_t>(clipped.left)) *
        4;
    dest.subspan(static_cast<size_t>(y) * row_bytes, row_bytes)
        .copy_from(frame.subspan(from, row_bytes));
  }
  out.width = w;
  out.height = h;
  ::SelectObject(mem, old);
  ::DeleteObject(dib);
  ::DeleteDC(mem);
  return out;
}



std::array<RECT, 23> PhiShellWindow::NavHoverTable(
    const Layout& layout) const {
  // The nav buttons' hover washes (back/forward/reload/search).
  // The first six MUST stay in the paint table's order — PaintChrome keys
  // a button's hover wash on this index.
  // APPEND ONLY: PaintChrome keys a button's hover wash on its index here,
  // so inserting into the middle silently moves someone else's wash.
  // Ordinal 12 was the Phi button, gone with the menu bar: an empty rect
  // keeps every later ordinal where the rigs aim.
  // Ordinal 6 was the address row's Downloads button, 11 the copy-link
  // slot and 12 the Phi button, all gone: empty rects keep every later
  // ordinal where the rigs aim. 15 was the in-field star, gone with the
  // Mac's star-less address bar; 17 is the bottom bar's own Downloads
  // button (§4.2); 18 Copy link and 19 the site menu, back as the Mac's
  // hover-only pair (a pointer on either is on the field, which shows them).
  // 20 is the Update reminder pill; 21 comfortable's strip overflow
  // chevron (PhiSidebarView::kStripOverflowNavOrdinal); 22 the bottom
  // bar's profile (avatar) button — hovered for its tooltip only, it has
  // no wash (§4.2).
  return {layout.back,        layout.forward,    layout.reload,
          layout.search_tabs, sidebar_->add_tab_button(),   layout.app_menu,
          RECT{},             layout.memory,     layout.feedback,
          layout.chat,        layout.extensions, RECT{},
          RECT{},             layout.reader,     layout.sidebar_toggle,
          RECT{},             layout.agent_chip, layout.sidebar_downloads,
          layout.copy_link,   layout.site_menu,  layout.update,
          sidebar_->strip_overflow_button(),     layout.sidebar_profile};
}

// The client rect a hovered element's wash covers, empty for none — so a
// hover change invalidates the old and the new element, not the whole
// shell (lane 3 §7 item 3). The sidebar answers for its own rows; the
// bands the shell still paints answer here.
RECT PhiShellWindow::HoverRect(HoverKind kind, int index) {
  RECT r = sidebar_->RowHoverRect(kind, index);
  switch (kind) {
    case HoverKind::kNone:
    case HoverKind::kTab:
    case HoverKind::kPin:
    case HoverKind::kBookmark:
    case HoverKind::kGroupHeader:
      break;
    case HoverKind::kChip:
      r = spaces_->ChipBounds(index);
      break;
    case HoverKind::kNav: {
      const auto nav = NavHoverTable(ComputeLayout());
      r = index >= 0 && index < static_cast<int>(nav.size())
              ? nav[static_cast<size_t>(index)]
              : RECT{};
      break;
    }
    case HoverKind::kNewTabRow:
      r = ComputeLayout().new_tab_row;
      break;
    case HoverKind::kFieldExt:
      r = FieldExtRect(ComputeLayout(),
                       static_cast<size_t>(std::max(index, 0)));
      break;
    case HoverKind::kAddSpace:
      r = spaces_->add_slot();
      break;
  }
  if (r.right > r.left && r.bottom > r.top) {
    // The wash's antialiased rim, and a pill that overhangs its row by a
    // pixel at odd scales.
    ::InflateRect(&r, Scaled(2), Scaled(2));
  }
  return r;
}

void PhiShellWindow::SetHover(HoverKind kind, int index) {
  if (kind == hover_kind_ && index == hover_index_) {
    return;
  }
  // The model is unchanged: only the element the pointer left and the one
  // it landed on recompose (lane 3 §7 item 3), never the whole shell.
  const RECT before = HoverRect(hover_kind_, hover_index_);
  hover_kind_ = kind;
  hover_index_ = index;
  spaces_->UpdateHoverCard(kind == HoverKind::kChip ? index : -1);
  UpdateTabPreview();
  const RECT after = HoverRect(kind, index);
  if (!shell_hwnd_) {
    return;
  }
  for (const RECT* r : {&before, &after}) {
    if (r->right > r->left && r->bottom > r->top) {
      ::InvalidateRect(shell_hwnd_, r, FALSE);
    }
  }
}

void PhiShellWindow::OnChromeMouseMove(POINT client_pt) {
  HoverKind kind = HoverKind::kNone;
  int index = -1;
  // The band's chips and rows are hit-tested as they are — and while the
  // floating panel is up they are built for ITS geometry, not the collapsed
  // shell's (RebuildTabs). The shell then owns no band: a pointer over its
  // collapsed card must not land on a pip or a row the panel is showing.
  const bool band = BandOnShell();
  {
    const Layout layout = ComputeLayout();
    const auto nav = NavHoverTable(layout);
    for (size_t i = 0; i < nav.size() && kind == HoverKind::kNone; ++i) {
      if (::PtInRect(&nav[i], client_pt)) {
        kind = HoverKind::kNav;
        index = static_cast<int>(i);
      }
    }
  }
  if (band && kind == HoverKind::kNone) {
    if (const int chip = spaces_->ChipAt(client_pt); chip >= 0) {
      kind = HoverKind::kChip;
      index = chip;
    }
  }
  if (band) {
    // The strip's trailing slot ("+" / "…"), and — tracked apart from the
    // hover kind, since resting on a pip is both a pip hover and a row
    // hover — the strip's ROW: inside its 10pt padding, pips and the empty
    // run to the trailing slot included. The row is the "+" button's
    // reveal region (the Mac's onHover on the whole row). Empty slot =
    // no strip in this layout (comfortable, incognito): no row to hover.
    bool row_hovered = false;
    const RECT add_slot = spaces_->add_slot();
    if (add_slot.right > add_slot.left) {
      if (kind == HoverKind::kNone && ::PtInRect(&add_slot, client_pt)) {
        kind = HoverKind::kAddSpace;
        index = 0;
      }
      RECT row = ComputeLayout().spaces_strip;
      ::InflateRect(&row, -Scaled(kSpacesStripPad), 0);
      row_hovered = ::PtInRect(&row, client_pt) != FALSE;
    }
    spaces_->SetRowHovered(row_hovered);
  } else {
    spaces_->SetRowHovered(false);
  }
  const std::vector<ShortcutRow>& pins = sidebar_->pinned();
  for (size_t i = 0; band && i < pins.size() && kind == HoverKind::kNone;
       ++i) {
    if (::PtInRect(&pins[i].bounds, client_pt)) {
      kind = HoverKind::kPin;
      index = static_cast<int>(i);
    }
  }
  const std::vector<ShortcutRow>& marks = sidebar_->bookmarks();
  for (size_t i = 0; band && i < marks.size() && kind == HoverKind::kNone;
       ++i) {
    if (::PtInRect(&marks[i].bounds, client_pt)) {
      kind = HoverKind::kBookmark;
      index = static_cast<int>(i);
    }
  }
  const std::vector<GroupHeader>& headers = sidebar_->group_headers();
  for (size_t i = 0; band && i < headers.size() && kind == HoverKind::kNone;
       ++i) {
    if (::PtInRect(&headers[i].bounds, client_pt)) {
      kind = HoverKind::kGroupHeader;
      index = static_cast<int>(i);
    }
  }
  if (band && kind == HoverKind::kNone) {
    const RECT new_tab = ComputeLayout().new_tab_row;
    if (::PtInRect(&new_tab, client_pt)) {
      kind = HoverKind::kNewTabRow;
      index = 0;
    }
  }
  {
    // The address capsule's rim (the EDIT subclass covers the text area).
    const Layout layout = ComputeLayout();
    omnibox_->SetFieldHover(::PtInRect(&layout.url_field, client_pt) !=
                            FALSE);
    for (size_t i = 0;
         i < field_extensions_.size() && kind == HoverKind::kNone; ++i) {
      const RECT slot = FieldExtRect(layout, i);
      if (::PtInRect(&slot, client_pt)) {
        kind = HoverKind::kFieldExt;
        index = static_cast<int>(i);
      }
    }
  }
  const std::vector<TabRow>& rows = sidebar_->tabs();
  for (size_t i = 0; band && i < rows.size() && kind == HoverKind::kNone;
       ++i) {
    if (::PtInRect(&rows[i].bounds, client_pt)) {
      kind = HoverKind::kTab;
      index = static_cast<int>(i);
    }
  }
  SetHover(kind, index);
  // §5.8: the surface tile's own hover, scoped to the tile inside the
  // hovered row (a change within the row repaints just that row).
  RECT tile = {};
  if (index >= 0) {
    if (kind == HoverKind::kPin && index < static_cast<int>(pins.size())) {
      tile = pins[static_cast<size_t>(index)].surface_box;
    } else if (kind == HoverKind::kBookmark &&
               index < static_cast<int>(marks.size())) {
      tile = marks[static_cast<size_t>(index)].surface_box;
    } else if (kind == HoverKind::kTab && index < static_cast<int>(rows.size())) {
      tile = rows[static_cast<size_t>(index)].surface_box;
    }
  }
  const bool tile_hot = tile.right > tile.left && ::PtInRect(&tile, client_pt);
  if (tile_hot != surface_tile_hot_) {
    surface_tile_hot_ = tile_hot;
    const RECT row = HoverRect(kind, index);
    if (shell_hwnd_ && row.right > row.left) {
      ::InvalidateRect(shell_hwnd_, &row, FALSE);
    }
  }
}

bool PhiShellWindow::BandOnShell() const {
  // The floating panel's events and paint run under floating_layout_, and
  // the band is theirs then; the collapsed shell's own turns see no band.
  return !sidebar_collapsed_ || floating_layout_;
}

void PhiShellWindow::ClearHover() {
  surface_tile_hot_ = false;
  spaces_->DismissHoverCard(std::string());
  spaces_->SetRowHovered(false);
  if (tab_preview_) {
    tab_preview_->PointerExited();
  }
  SetHover(HoverKind::kNone, -1);
  // The capsule's hover too: it gates the in-field Copy link / site-menu
  // pair. SetFieldHover re-checks the cursor, so a leave onto the EDIT
  // child keeps the field lit.
  omnibox_->SetFieldHover(false);
}

std::wstring PhiShellWindow::ChromeTipAt(POINT pt) {
  const Layout layout = ComputeLayout();
  // A menu row's label, its "\t<chord>" dropped.
  const auto label = [](const char* key) {
    const std::wstring text = PhiStr(key);
    return text.substr(0, text.find(L'\t'));
  };
  const struct {
    RECT rect;
    const char* key;
  } kTips[] = {
      {layout.back, "appMenu.back"},
      {layout.forward, "appMenu.forward"},
      {layout.reload, "app.reloadThisPage"},
      {layout.sidebar_toggle, "app.toggleSidebar"},
      {layout.search_tabs, "searchTabs.searchField.placeholder"},
      {layout.app_menu, "shell.toolbar.appMenu"},
      {layout.extensions, "appMenu.extensions"},
      {layout.copy_link, "contextMenu.copyLink"},
      {layout.site_menu, "shell.toolbar.siteInfo"},
      {layout.reader, "appMenu.readerView"},
      {layout.sidebar_profile, "profile.button.accessibilityLabel"},
      {layout.sidebar_downloads, "app.downloads"},
      {layout.feedback, "appMenu.reportIssue"},
  };
  for (const auto& tip : kTips) {
    if (tip.rect.right > tip.rect.left && ::PtInRect(&tip.rect, pt)) {
      return label(tip.key);
    }
  }
  // The sidebar's own icon-only buttons, by the names it publishes for
  // assistive technology (a row's title is on screen already).
  const int hit = PhiHitAt(hits_, pt.x, pt.y);
  if (hit >= 0 && hits_[static_cast<size_t>(hit)].role == PhiHitRole::kButton &&
      hits_[static_cast<size_t>(hit)].name != PhiStr("app.newTab")) {
    return hits_[static_cast<size_t>(hit)].name;
  }
  return std::wstring();
}

std::wstring PhiShellWindow::CaptionTip(WPARAM hit) const {
  switch (hit) {
    case HTMINBUTTON:
      return PhiStr("menuBar.minimize");
    case HTMAXBUTTON:
      return PhiStr(shell_hwnd_ && ::IsZoomed(shell_hwnd_)
                        ? "shell.captionButton.restore"
                        : "shell.captionButton.maximize");
    case HTCLOSE:
      return PhiStr("contextMenu.close");
    default:
      return std::wstring();
  }
}

void PhiShellWindow::UpdateChromeTip(const std::wstring& text) {
  if (!shell_hwnd_ || text == chrome_tip_) {
    return;
  }
  chrome_tip_ = text;
  ::KillTimer(shell_hwnd_, kChromeTipTimer);
  PhiHideTooltipFor(shell_hwnd_);
  if (!text.empty()) {
    // The system's own tooltip delay, as PhiWindow's tips use.
    ::SetTimer(shell_hwnd_, kChromeTipTimer, ::GetDoubleClickTime(), nullptr);
  }
}

void PhiShellWindow::RetipAtCursor() {
  POINT cursor = {};
  if (!shell_hwnd_ || !::GetCursorPos(&cursor) ||
      ::WindowFromPoint(cursor) != shell_hwnd_) {
    UpdateChromeTip(std::wstring());
    return;
  }
  const LRESULT hit = ::SendMessageW(shell_hwnd_, WM_NCHITTEST, 0,
                                     MAKELPARAM(cursor.x, cursor.y));
  if (hit != HTCLIENT) {
    UpdateChromeTip(CaptionTip(static_cast<WPARAM>(hit)));
    return;
  }
  ::ScreenToClient(shell_hwnd_, &cursor);
  UpdateChromeTip(ChromeTipAt(cursor));
}

void PhiShellWindow::DismissChromeTip() {
  if (!shell_hwnd_) {
    return;
  }
  // The name stays: the same button under the pointer does not re-arm.
  ::KillTimer(shell_hwnd_, kChromeTipTimer);
  PhiHideTooltipFor(shell_hwnd_);
}

void PhiShellWindow::ShowCreateSpaceForm() {
  spaces_->ShowCreateForm();
}

void PhiShellWindow::CommitCreateSpaceForm(const PhiNewSpaceSpec& spec) {
  spaces_->CommitCreateForm(spec);
}

void PhiShellWindow::CancelCreateSpaceForm() {
  spaces_->CancelCreateForm();
}

void PhiShellWindow::PreviewCreateSpaceTheme(const std::string& theme_id,
                                                  int saturation) {
  spaces_->PreviewTheme(theme_id, saturation);
}

void PhiShellWindow::PreviewCreateSpacePalette(const std::string& theme_id,
                                                    const PhiThemePalette& palette) {
  spaces_->PreviewPalette(theme_id, palette);
}

void PhiShellWindow::SetCreateSpaceIcon(const std::string& storage_value) {
  spaces_->SetFormIcon(storage_value);
}

void PhiShellWindow::OnSpaceFormProfileCreated(
    const std::string& profile_id) {
  spaces_->OnFormProfileCreated(profile_id);
  PhiCreateSpaceWindow::NoteProfileCreated(this, profile_id);
  PhiNoteCreateSpaceFlyoutProfileCreated(this, profile_id);
}

// True when `tab_id`'s split stacks its panes.
bool PhiShellWindow::SplitIsStacked(TabId tab_id) const {
  const BrowserId browser = active_browser_id();
  const SplitSnapshot* split = model_ && browser.valid() && tab_id.valid()
                                   ? model_->SplitOfTab(browser, tab_id)
                                   : nullptr;
  return split && split->layout == phi_framework::SplitLayout::kHorizontal;
}

bool PhiShellWindow::PullSnapshot(BrowserId browser,
                                       WindowSnapshot* out) const {
  return browser.valid() && PhiWindowModel::Pull(framework_, browser, out);
}

// The hovered row's page-snapshot card (the Mac's TabPreviewRegistration,
// which every tab surface installs on its cells). Eligibility is the Mac's:
// the setting on, no drag in flight, and a LIVE tab behind the row. A split
// row previews BOTH its panes in one card, the Mac's SplitTabPreview.
void PhiShellWindow::ResetNativeNtp() {
  native_pages_->ResetNtp();
}

void PhiShellWindow::NoteTabDistillable(TabId tab_id,
                                             bool distillable) {
  const bool changed = distillable ? distillable_tabs_.insert(tab_id).second
                                   : distillable_tabs_.erase(tab_id) > 0;
  if (!changed) {
    return;
  }
  // The verdict decides whether the address capsule carves a reader slot:
  // a layout input, so the capsule (and what moves beside it) repaints.
  RefreshLayoutInputs();
  if (shell_hwnd_) {
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
  }
}

bool PhiShellWindow::ActiveTabDistillable() const {
  for (const TabRow& row : sidebar_->tabs()) {
    if (row.active) {
      return distillable_tabs_.count(row.id) > 0;
    }
  }
  return false;
}

bool PhiShellWindow::ActiveTabReaderOfferable() const {
  if (!active_tab_id().valid() || !CanOfferReaderForUrl(active_tab_url())) {
    return false;
  }
  if (ActiveTabDistillable()) {
    return true;
  }
  return services_.sidebar &&
         (services_.sidebar->IsReaderOfferable(active_tab_id().value,
                                               active_tab_url()) ||
          services_.sidebar->IsReaderActive(active_tab_id().value,
                                            active_tab_url()));
}

bool PhiShellWindow::ActiveTabShowsReader() const {
  return services_.sidebar && active_tab_id().valid() &&
         services_.sidebar->IsReaderActive(active_tab_id().value,
                                           active_tab_url());
}

bool PhiShellWindow::ActiveTabCanUseReader() const {
  return active_tab_id().valid() && CanOfferReaderForUrl(active_tab_url());
}

// static
bool PhiShellWindow::CanOfferReaderForUrl(const std::string& url) {
  // The Mac's ReaderExtractionService.canOfferReader: a real document, not
  // one of Phi's own pages, and never a PDF — the reader's extraction ladder
  // is a DOM one and a PDF has no DOM to walk (the migration plan lists PDFs
  // and file:// as out of scope on purpose). This gate sits AHEAD of both
  // verdicts, so no signal can light the button on a page the reader could
  // not open anyway.
  if (url.empty() || PhiNewTabPage::IsNewTabUrl(url)) {
    return false;
  }
  for (const char* local : {"chrome://conversation", "phi://skills",
                            "chrome://skills"}) {
    if (url.rfind(local, 0) == 0) {
      return false;
    }
  }
  // The path's extension, ignoring any query or fragment.
  const size_t cut = url.find_first_of("?#");
  std::string path = cut == std::string::npos ? url : url.substr(0, cut);
  const size_t dot = path.rfind('.');
  if (dot == std::string::npos) {
    return true;
  }
  std::string ext = path.substr(dot + 1);
  for (char& c : ext) {
    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  }
  return ext != "pdf";
}

void PhiShellWindow::CardRoundedCorners(bool* top, bool* bottom) const {
  const bool any = !browsers_->content_fullscreen();
  if (bottom) {
    *bottom = any;
  }
  // The card's top corners are rounded wherever there IS a card — in the
  // layouts with a top bar they are the BAR's corners (painted by its own
  // RoundRect), which is why a surface covering the card must round them and
  // one covering only the page below the bar must not. Comfortable has no
  // card: its page is edge to edge and the band above it is the window's.
  if (top) {
    *top = any && layout_mode_ != PhiLayoutMode::kComfortable;
  }
}


void PhiShellWindow::ToggleReaderView(const char* entry_point) {
  if (!services_.sidebar || !active_tab_id().valid()) {
    return;
  }
  services_.sidebar->RequestToggleReaderView(active_tab_id().value,
                                             active_tab_url(), entry_point);
}

bool PhiShellWindow::LeaveReaderViewForNavigation() {
  // Every surface attached to the active tab — the reader covering it, a
  // peek floating over it — closes before the page underneath changes.
  if (!services_.sidebar || !active_tab_id().valid()) {
    return false;
  }
  const int tab = active_tab_id().value;
  const bool reader = services_.sidebar->IsReaderActive(tab, active_tab_url());
  // By the ACTIVE TAB, not by the active row: a split's row carries either
  // pane's surface, and a reader over the other pane covers another page.
  const bool attached =
      services_.sidebar->AttachedSurfaces(active_browser_id().value)
          .count(tab) != 0;
  if (reader || attached) {
    services_.sidebar->RequestCloseSurfaces(tab);
    return true;
  }
  // The split partner's peek covers this pane too.
  return services_.sidebar->RequestClosePartnerPeek(active_browser_id().value,
                                                    tab);
}

bool PhiShellWindow::SplitPaneRectForTab(int tab_id, RECT* pane) const {
  for (const NativePane& candidate : NativePagesSplitPanes()) {
    if (candidate.tab_id == tab_id) {
      *pane = candidate.rect;
      return true;
    }
  }
  return false;
}

const char* PhiShellWindow::PeekOpenerKind(int tab_id) const {
  const SidebarTree& tree = sidebar_->tree();
  const auto owner = tree.owner_of.find(tab_id);
  if (owner == tree.owner_of.end() ||
      owner->second.kind != SidebarKey::Kind::kCell) {
    return "normal";
  }
  const SidebarCell* cell = tree.CellFor(owner->second.id);
  if (!cell) {
    return "normal";
  }
  return cell->kind == SidebarStoredKind::kPin ? "pinned" : "bookmark";
}

void PhiShellWindow::UpdateTabPreview() {
  if (!services_.prefs || !services_.prefs->ShowTabPreviews() ||
      sidebar_->drag_active()) {
    if (tab_preview_) {
      tab_preview_->PointerExited();
    }
    return;
  }

  PhiTabPreview::Target target;
  bool eligible = false;
  switch (hover_kind_) {
    case HoverKind::kTab: {
      const std::vector<TabRow>& tab_rows = sidebar_->tabs();
      if (hover_index_ >= 0 &&
          hover_index_ < static_cast<int>(tab_rows.size())) {
        const TabRow& row = tab_rows[static_cast<size_t>(hover_index_)];
        if (row.id.valid()) {
          target.key = "tab:" + base::NumberToString(row.id.value);
          target.tab_id = row.id;
          target.title = row.title;
          target.url = row.url;
          target.foreground = row.active && row.b_index < 0;
          target.anchor = row.bounds;
          if (row.b_index >= 0 && row.b_id.valid()) {
            // A merged split row: pane B rides along.
            target.tab_id_b = row.b_id;
            target.title_b = row.b_title;
            target.url_b = row.b_url;
            target.foreground = row.active && !row.b_active;
            target.foreground_b = row.b_active;
          }
          eligible = true;
        }
      }
      break;
    }
    case HoverKind::kPin:
    case HoverKind::kBookmark: {
      const std::vector<ShortcutRow>& rows =
          hover_kind_ == HoverKind::kPin ? sidebar_->pinned()
                                         : sidebar_->bookmarks();
      if (hover_index_ >= 0 && hover_index_ < static_cast<int>(rows.size())) {
        const ShortcutRow& row = rows[static_cast<size_t>(hover_index_)];
        // Folders have no page, and an unopened row has no live tab to
        // snapshot. A BOOKMARK split carries both panes under one guid, so
        // there is no second tab id to ask for — it keeps the single card.
        if (!row.folder && row.open && row.tab_id.valid()) {
          target.key = "row:" + row.guid;
          target.tab_id = row.tab_id;
          target.title = row.title;
          target.url = base::UTF8ToWide(row.url);
          target.foreground = row.active;
          target.anchor = row.bounds;
          if (!row.url_b.empty() && row.tab_id_b.valid()) {
            target.tab_id_b = row.tab_id_b;
            target.title_b = row.title_b;
            target.url_b = base::UTF8ToWide(row.url_b);
          }
          eligible = true;
        }
      }
      break;
    }
    default:
      break;
  }

  if (!eligible) {
    if (tab_preview_) {
      tab_preview_->PointerExited();
    }
    return;
  }

  // TabPreviewURLPolicy: the new-tab page shows no address, and the
  // bookmark manager opts out of snapshots entirely.
  const auto apply_url_policy = [&](std::wstring* url, bool* allow) {
    const std::string utf8 = base::WideToUTF8(*url);
    if (utf8 == "chrome://newtab/" || utf8 == "chrome://new-tab-page/" ||
        utf8 == "about:blank") {
      url->clear();
    }
    if (utf8.rfind("chrome://bookmarks", 0) == 0 ||
        utf8.rfind("phi://bookmarks", 0) == 0) {
      *allow = false;
    }
  };
  apply_url_policy(&target.url, &target.allow_snapshot);
  if (target.tab_id_b.valid()) {
    apply_url_policy(&target.url_b, &target.allow_snapshot);
    target.stacked = SplitIsStacked(target.tab_id);
  }

  // The card hangs off the row in SCREEN space.
  target.anchor = LayoutToScreen(target.anchor);

  // Sidebar rows hand the card off their right edge; the pinned grid and the
  // comfortable strip drop it underneath, as the Mac call sites do.
  const bool comfortable =
      services_.prefs->LayoutMode() == PhiLayoutMode::kComfortable;
  target.placement =
      (hover_kind_ == HoverKind::kPin || comfortable)
          ? PhiTabPreview::Placement::kBelowAttached
          : PhiTabPreview::Placement::kRightTopAttached;

  if (!tab_preview_) {
    tab_preview_ = std::make_unique<PhiTabPreview>(this);
  }
  tab_preview_->PointerEntered(target);
}

void PhiShellWindow::SetAgentVisible(bool visible,
                                     const RECT* frame,
                                     HWND owner) {
  if (!agent_ || !shell_hwnd_ || visible == agent_visible_) {
    return;
  }
  agent_visible_ = visible;
  if (visible && frame) {
    // Owned by the watching window for the duration, then take its frame
    // exactly, so the swap reads as that window changing Space (the Mac's
    // slot.activate), not a second one. The owner stays shown underneath:
    // an owned window is kept above its owner by the system.
    ::SetWindowLongPtrW(shell_hwnd_, GWLP_HWNDPARENT,
                        reinterpret_cast<LONG_PTR>(owner));
    {
      const base::AutoReset<bool> exact(&exact_bounds_pending_, true);
      ::SetWindowPos(shell_hwnd_, HWND_TOP, frame->left, frame->top,
                     frame->right - frame->left, frame->bottom - frame->top,
                     SWP_SHOWWINDOW);
    }
    ::SetForegroundWindow(shell_hwnd_);
    return;
  }
  if (visible) {
    // Surface centred on the work area so the user can watch the agent live.
    RECT work = {};
    RECT wr = {};
    ::GetWindowRect(shell_hwnd_, &wr);
    const int w = wr.right - wr.left;
    const int h = wr.bottom - wr.top;
    int x = 120, y = 80;
    if (::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
      x = work.left + std::max<LONG>(0, (work.right - work.left - w) / 2);
      y = work.top + std::max<LONG>(0, (work.bottom - work.top - h) / 3);
    }
    {
      const base::AutoReset<bool> exact(&exact_bounds_pending_, true);
      ::SetWindowPos(shell_hwnd_, HWND_TOP, x, y, 0, 0,
                     SWP_NOSIZE | SWP_SHOWWINDOW);
    }
    ::SetForegroundWindow(shell_hwnd_);
  } else {
    // Park off-screen again — still shown so its content keeps rendering for
    // the agent's CDP capture, just invisible to the user. The watcher it
    // covered is painted already, so nothing shows through in between;
    // then the ownership is dropped (the park moves it out from over its
    // owner first — an owned window is z-ordered with its owner).
    {
      const base::AutoReset<bool> exact(&exact_bounds_pending_, true);
      ::SetWindowPos(shell_hwnd_, HWND_BOTTOM, -32000, -32000, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
    }
    ::SetWindowLongPtrW(shell_hwnd_, GWLP_HWNDPARENT, 0);
  }
}

void PhiShellWindow::SetAgentOwned(bool agent_owned) {
  if ((!agent_ && !HostsAgentSpace()) || agent_owned_ == agent_owned) {
    return;
  }
  agent_owned_ = agent_owned;
  sidebar_->set_agent_holds_input(AgentHoldsInput());
}

bool PhiShellWindow::AgentHoldsInput() const {
  if (!agent_owned_) {
    return false;
  }
  return agent_ || (HostsAgentSpace() && active_space() == hosted_agent_space_);
}

void PhiShellWindow::HostAgentSpace(const std::string& space_id,
                                    BrowserId browser,
                                    HWND browser_hwnd,
                                    const std::string& task_id) {
  // A browser still sliding home goes now: the swap below cuts that slide
  // short anyway, and no two agent browsers wait in this frame.
  FinishUnhost();
  const bool hand_over = HostsAgentSpace();
  const std::string previous_task = hosted_agent_task_;
  const std::string previous_space = hosted_agent_space_;
  if (!hand_over) {
    hosted_return_space_ = active_space();
  }
  hosted_agent_task_ = task_id;
  hosted_agent_space_ = space_id;
  // The slot's own Space swap: band slide, pill slide, the page revealed
  // when the slide lands — the agent Space is a Space of this slot now.
  browsers_->AttachSpaceBrowser(space_id, browser, browser_hwnd,
                                /*agent=*/true);
  if (hand_over) {
    // Agent to agent: the one that was on screen is the swap's leaving
    // page. Noted only now — the swap's start settles any earlier one
    // (RevealSwappedContent), which must not send THIS browser home before
    // its page has slid out.
    leaving_agent_task_ = previous_task;
    leaving_agent_space_ = previous_space;
    if (!BrowserHostSwapRunning()) {
      FinishUnhost();
    }
  }
}

void PhiShellWindow::LeaveHostedAgentSpace(const std::string& space_id) {
  if (!HostsAgentSpace()) {
    return;
  }
  FinishUnhost();  // A hand-over still in flight: that browser goes first.
  std::string target = space_id;
  if (target.empty() || !HasSpace(target) || PhiIsAgentSpaceId(target)) {
    target = hosted_return_space_;
  }
  if (target.empty() || !HasSpace(target) || PhiIsAgentSpaceId(target)) {
    for (const std::string& other : browsers_->spaces().order()) {
      if (!PhiIsAgentSpaceId(other)) {
        target = other;
        break;
      }
    }
  }
  if (!target.empty() && active_space() != target) {
    browsers_->ShowSpace(target);  // The normal swap back (or instant).
  }
  // The hosted page is the leaving one: it paints until the slide lands.
  leaving_agent_task_ = hosted_agent_task_;
  leaving_agent_space_ = hosted_agent_space_;
  hosted_agent_task_.clear();
  hosted_agent_space_.clear();
  hosted_return_space_.clear();
  agent_owned_ = false;
  if (sidebar_) {
    sidebar_->set_agent_holds_input(false);
  }
  if (!BrowserHostSwapRunning()) {
    FinishUnhost();
  }
}

void PhiShellWindow::FinishUnhost() {
  if (leaving_agent_task_.empty()) {
    return;
  }
  const std::string task_id = leaving_agent_task_;
  HWND child = browsers_->DetachSpaceBrowser(leaving_agent_space_);
  leaving_agent_task_.clear();
  leaving_agent_space_.clear();
  if (services_.agent) {
    services_.agent->OnAgentBrowserReturned(task_id, child);
  }
  ScheduleRefresh();  // The strip's pill is the entering Space's alone.
}

HWND PhiShellWindow::ReleaseAgentBrowserForHosting() {
  return agent_ ? browsers_->ReleaseContentChildKeepRecord() : nullptr;
}

void PhiShellWindow::ReadoptAgentBrowser(HWND browser_hwnd) {
  if (agent_) {
    browsers_->ReadoptContentChild(browser_hwnd);
  }
}

void PhiShellWindow::SetConcealedForWatch(bool concealed) {
  if (agent_ || !shell_hwnd_ || concealed) {
    return;  // Stays shown under the agent window (see the header).
  }
  ::SetForegroundWindow(shell_hwnd_);
}

void PhiShellWindow::BringToFront() {
  if (!shell_hwnd_) {
    return;
  }
  if (::IsIconic(shell_hwnd_)) {
    ::ShowWindow(shell_hwnd_, SW_RESTORE);
  }
  ::SetForegroundWindow(shell_hwnd_);
}


COLORREF PhiShellWindow::BackgroundFill() const {
  return services_.spaces
             ? services_.spaces->PaletteForSlot(slot_.value).window_bg
             : RGB(0xFF, 0xFF, 0xFF);
}

void PhiShellWindow::RefreshTabs() {
  SyncSplitChipTimer();
  // A structural view change (a Space switch, a layout mode, the DPI): the
  // rows must exist in the new geometry before the next frame, so the
  // rebuild runs here and the whole shell repaints. Never for a framework
  // mutation — its push schedules the rebuild.
  rows_dirty_ = true;
  RebuildTabs();
  if (shell_hwnd_) {
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
  }
}

void PhiShellWindow::ScheduleRefresh() {
  // Coalesced: one posted rebuild per burst of changes. The rebuild turn
  // invalidates only the bands whose rows changed; nothing is invalidated
  // here, because nothing has changed on screen yet.
  rows_dirty_ = true;
  if (shell_hwnd_ && !rebuild_posted_) {
    rebuild_posted_ =
        ::PostMessageW(shell_hwnd_, kMsgPhiRebuildRows, 0, 0) != 0;
  }
}

void PhiShellWindow::FlushPendingRebuild() {
  if (!rows_dirty_) {
    return;
  }
  const BandSignature before = CurrentBandSignature();
  RebuildTabs();
  InvalidateChangedBands(before);
  // A posted kMsgPhiRebuildRows still in the queue finds the rows clean.
}

PhiShellWindow::BandSignature PhiShellWindow::CurrentBandSignature()
    const {
  // Cheap but complete: everything a band paints from its rows — key,
  // place, title, state, and a fold of the favicon bytes so an icon that
  // changes at the same size still repaints (kFaviconReady).
  const auto fold = [](const std::vector<unsigned char>& bytes) {
    uint32_t h = 2166136261u;
    for (unsigned char b : bytes) {
      h = (h ^ b) * 16777619u;
    }
    return h;
  };
  const auto rect_key = [](const RECT& r) {
    return std::to_string(r.left) + "," + std::to_string(r.top) + "," +
           std::to_string(r.right) + "," + std::to_string(r.bottom);
  };
  BandSignature out;
  for (const TabRow& row : sidebar_->tabs()) {
    out.rows.emplace_back(
        row.key.ToString() + "|" + base::WideToUTF8(row.title) + "|" +
            base::WideToUTF8(row.b_title) + "|" + (row.active ? "a" : "") +
            (row.b_active ? "b" : "") + (row.unloaded ? "u" : "") +
            std::to_string(static_cast<int>(row.attached_surface)) + "|" +
            std::to_string(fold(row.favicon)) + "|" +
            std::to_string(fold(row.b_favicon)) + "|" + rect_key(row.pane_b),
        row.bounds);
  }
  for (const GroupHeader& header : sidebar_->group_headers()) {
    out.rows.emplace_back("g:" + header.id + "|" +
                              base::WideToUTF8(header.title) + "|" +
                              std::to_string(header.color) +
                              (header.collapsed ? "c" : "") + "|" +
                              rect_key(header.container),
                          header.bounds);
  }
  const auto stored = [&](const char* band, const ShortcutRow& row) {
    out.rows.emplace_back(
        std::string(band) + row.key.ToString() + "|" +
            base::WideToUTF8(row.title) + "|" + base::WideToUTF8(row.title_b) +
            "|" + (row.open ? "o" : "") + (row.active ? "a" : "") +
            (row.expanded ? "e" : "") + std::to_string(row.depth) + "|" +
            std::to_string(static_cast<int>(row.attached_surface)) + "|" +
            std::to_string(fold(row.favicon)) + "|" +
            std::to_string(fold(row.favicon_b)),
        row.bounds);
  };
  for (const ShortcutRow& pin : sidebar_->pinned()) {
    stored("p:", pin);
  }
  for (const ShortcutRow& bookmark : sidebar_->bookmarks()) {
    stored("b:", bookmark);
  }
  for (const PhiSpaceChip& chip : spaces_->chips()) {
    out.chips.emplace_back(chip.id + "|" + base::WideToUTF8(chip.label) + "|" +
                               base::WideToUTF8(chip.icon) +
                               (chip.open ? "o" : "") +
                               (chip.is_agent ? "g" : "") +
                               std::to_string(chip.badge) +
                               (chip.id == active_space() ? "*" : ""),
                           chip.bounds);
  }
  out.chips.emplace_back("+" + std::to_string(spaces_->overflow()) +
                             (spaces_->creating() ? "c" : ""),
                         spaces_->add_slot());
  out.nav_url = omnibox_->nav_url();
  out.can_go_back = omnibox_->can_go_back();
  out.can_go_forward = omnibox_->can_go_forward();
  out.nav_loading = omnibox_->nav_loading();
  if (page_color_) {
    out.page_color = static_cast<int64_t>(*page_color_);
  }
  out.reader_shown = page_color_reader_;
  out.layout = CurrentLayoutInputs();
  return out;
}

void PhiShellWindow::InvalidateChangedBands(
    const BandSignature& before) {
  if (!shell_hwnd_) {
    return;
  }
  const BandSignature after = CurrentBandSignature();
  const auto same_rects =
      [](const std::vector<std::pair<std::string, RECT>>& a,
         const std::vector<std::pair<std::string, RECT>>& b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                          [](const auto& x, const auto& y) {
                            return x.first == y.first &&
                                   ::EqualRect(&x.second, &y.second);
                          });
      };
  if (after.layout != before.layout) {
    // The geometry itself moved (a reader slot, the bookmark bar, a band's
    // row count): every band is placed anew.
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
    return;
  }
  const Layout layout = ComputeLayout();
  const auto invalidate = [&](RECT r) {
    if (r.right > r.left && r.bottom > r.top) {
      ::InvalidateRect(shell_hwnd_, &r, FALSE);
    }
  };
  if (!same_rects(after.rows, before.rows)) {
    // The rows' band: the whole sidebar under the caption in the vertical
    // layouts, the caption band (the strip) in comfortable. The caption
    // band and the content are untouched by a row change otherwise.
    invalidate(layout.horizontal_tabs
                   ? layout.title_bar
                   : RECT{0, layout.title_bar.bottom, layout.sidebar.right,
                          layout.client.bottom});
    invalidate(layout.bookmark_bar);  // Its pills are bookmark rows.
  }
  if (after.page_color != before.page_color ||
      after.reader_shown != before.reader_shown) {
    // The page's colour: the address row, the strip's active tab and the
    // bookmark bar follow it; the EDIT repaints itself (WM_CTLCOLOREDIT).
    invalidate(layout.title_bar);
    invalidate(layout.top_bar);
    invalidate(layout.bookmark_bar);
    omnibox_->Repaint();
  }
  if (after.nav_url != before.nav_url ||
      after.can_go_back != before.can_go_back ||
      after.can_go_forward != before.can_go_forward ||
      after.nav_loading != before.nav_loading) {
    // Where the nav controls and the address capsule live per layout.
    invalidate(layout.title_bar);
    invalidate(layout.top_bar);
    RECT field = layout.url_field;
    ::InflateRect(&field, Scaled(2), Scaled(2));
    invalidate(field);
  }
  if (!same_rects(after.chips, before.chips)) {
    invalidate(layout.horizontal_tabs ? layout.title_bar
                                      : layout.spaces_strip);
  }
}

bool PhiShellWindow::SettledDark() const {
  // The palette the slot SETTLES on — not the switch ramp's frame, which
  // still reads the Space being left while the switch is noted.
  if (incognito_ || !services_.spaces) {
    return CurrentPalette().dark;
  }
  return services_.spaces->PaletteForSlot(slot_.value).dark;
}

PhiPalette PhiShellWindow::CurrentPalette() const {
  static const PhiPalette kFallback =
      PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kLight);
  if (incognito_) {
    // Incognito is FIXED-DARK regardless of theme/appearance (the Mac's
    // incognito window contract).
    static const PhiPalette kIncognito =
        PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kDark);
    return kIncognito;
  }
  // The strip owns the create form's live preview and the switch ramp: THE
  // one place a palette is overridden (phi_spaces_strip.h).
  return spaces_->RampPalette(
      services_.spaces ? services_.spaces->PaletteForSlot(slot_.value)
                       : kFallback);
}

std::optional<PhiColor> PhiShellWindow::ActivePageColor() const {
  return page_color_;
}

std::optional<PhiColor> PhiShellWindow::ComputeActivePageColor() const {
  // WebContentHeader.updatePageColor's eligibility: a real page (not the new
  // tab or bookmark page, not a placeholder), no group overview, not a split
  // -- each pane of a split is its own page. A crashed tab needs no test: the
  // framework clears its colour. The native pages are judged by the active
  // tab's URL, never by what is mounted: a Space switch rebuilds before the
  // slide lands, while the LEAVING Space's page is still up (SyncNtp waits
  // for the reveal), and nothing rebuilds after it comes down.
  if (!model_ || !browsers_ || browsers_->ActiveBrowserPlaceholding() ||
      PhiGroupOverview::IsOpen() || PhiLibrary::IsOpen() ||
      ActiveTabIsSplit()) {
    return std::nullopt;
  }
  const PhiWindowModel::Tab* tab =
      model_->FindTab(active_browser_id(), active_tab_id());
  if (!tab || !tab->has_page_color || PhiNewTabPage::IsNewTabUrl(tab->url) ||
      PhiBookmarkManager::IsManagerUrl(tab->url)) {
    return std::nullopt;
  }
  const uint32_t argb = tab->page_color_argb;
  return PhiRgb((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
}

PhiPalette PhiShellWindow::BarPalette() const {
  const PhiPalette palette = CurrentPalette();
  const std::optional<PhiColor> page = ActivePageColor();
  return page ? PhiPageBarPalette(palette, *page) : palette;
}

void PhiShellWindow::UpdateSidebarBackdrop() {
  if (!shell_hwnd_ || destroying_) {
    return;
  }
  HIGHCONTRASTW contrast = {sizeof(contrast)};
  ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
  backdrop_split_ = ActiveTabIsSplit() && !browsers_->content_fullscreen();
  const bool enabled =
      !browsers_->content_fullscreen() &&
      !(contrast.dwFlags & HCF_HIGHCONTRASTON);
  const BOOL dark = SettledDark();
  backdrop_dark_ = dark != FALSE;
  ::DwmSetWindowAttribute(shell_hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE,
                          &dark, sizeof(dark));
  // Desktop Acrylic exposes the wallpaper/windows behind the sidebar.
  // Unsupported Windows versions retain the existing opaque palette.
  const DWM_SYSTEMBACKDROP_TYPE type =
      enabled ? DWMSBT_TRANSIENTWINDOW : DWMSBT_NONE;
  sidebar_backdrop_ = SUCCEEDED(::DwmSetWindowAttribute(
      shell_hwnd_, DWMWA_SYSTEMBACKDROP_TYPE, &type, sizeof(type))) && enabled;
  MARGINS margins = {};
  if (sidebar_backdrop_) {
    // Normal page corners and the rounded address row need the backdrop
    // just as split gutters do, including with the sidebar hidden.
    margins = {-1, -1, -1, -1};
  }
  if (FAILED(::DwmExtendFrameIntoClientArea(shell_hwnd_, &margins))) {
    sidebar_backdrop_ = false;
  }
}

void PhiShellWindow::PrepareBackdropPixels(const PhiPalette& palette) {
  // A multi-colour Space palette regrades the sidebar onto its gradient
  // even on an opaque frame.
  const bool gradient = palette.ground_stop_count > 1;
  if ((!sidebar_backdrop_ && !gradient) || !back_buffer_bits_) {
    return;
  }
  // GDI can batch writes to a DIB; finish them before accessing its bits.
  ::GdiFlush();
  if (content_slide_->running()) {
    if (!sidebar_backdrop_) {
      return;
    }
    // During a full-content slide the shell owns the page pixels too:
    // they are already-composited snapshots, not holes for a browser HWND.
    // GDI copies can drop their alpha; clearing the content rectangle below
    // would instead expose the desktop for an animation frame.
    auto pixels = UNSAFE_BUFFERS(base::span<uint32_t>(
        static_cast<uint32_t*>(back_buffer_bits_),
        static_cast<size_t>(back_buffer_size_.cx) * back_buffer_size_.cy));
    for (auto& pixel : pixels) {
      pixel |= 0xFF000000u;
    }
    return;
  }
  const int width = back_buffer_size_.cx;
  const Layout layout = ComputeWindowLayout();
  const int sidebar = std::clamp(layout.content.left, 0L,
                                back_buffer_size_.cx);
  const uint32_t rgb = PhiPackedRgb(palette.ground_top);
  // SAFETY: AcquireBackBuffer allocated width * height 32-bit pixels in a
  // top-down DIB; 32bpp rows require no additional stride padding.
  auto pixels = UNSAFE_BUFFERS(base::span<uint32_t>(
      static_cast<uint32_t*>(back_buffer_bits_),
      static_cast<size_t>(width) * back_buffer_size_.cy));
  // The ground each row of the sidebar shows: the one tint, or the
  // palette's gradient down the window's full height (the Mac's sidebar
  // backdrop spans it, traffic lights to bottom bar).
  const std::vector<uint32_t> grounds =
      gradient ? PhiBackdropRowGrounds(palette, 0, back_buffer_size_.cy,
                                       back_buffer_size_.cy, sidebar_backdrop_)
               : std::vector<uint32_t>();
  if (!sidebar_backdrop_) {
    // Opaque frame: only the sidebar's ground moves.
    for (int y = 0; y < back_buffer_size_.cy; ++y) {
      auto row = pixels.subspan(static_cast<size_t>(y) * width,
                                static_cast<size_t>(sidebar));
      const uint32_t ground = grounds[static_cast<size_t>(y)];
      for (uint32_t& pixel : row) {
        pixel = (pixel & 0xFFFFFF) == rgb
                    ? ground : PhiBackdropPixelOver(pixel, rgb, ground);
      }
    }
    return;
  }
  const uint32_t flat_tint =
      PhiBackdropPixel(rgb, palette.dark ? 0 : 0xFFFFFF);
  const int radius = Scaled(kContentRadius);
  const RECT& band = layout.top_bar;
  for (int y = 0; y < back_buffer_size_.cy; ++y) {
    auto row = pixels.subspan(static_cast<size_t>(y) * width,
                              static_cast<size_t>(width));
    const uint32_t tint =
        gradient ? grounds[static_cast<size_t>(y)] : flat_tint;
    for (int x = 0; x < sidebar; ++x) {
      row[x] = (row[x] & 0xFFFFFF) == rgb
                   ? tint : PhiBackdropPixelOver(row[x], rgb, tint);
    }
    // Chromium supplies the ground tint and clipped opaque page layers.
    // Leave the shell underneath clear so its tint is not applied twice.
    // Only the rounded address-row corners expose ground in shell chrome.
    for (int x = sidebar; x < width; ++x) {
      const bool bar_corner =
          band.left > 0 && !layout.horizontal_tabs &&
          x >= band.left && x < band.left + radius &&
          y >= band.top && y < band.bottom && y < band.top + radius;
      if (x >= layout.content.left &&
          x < layout.content.right && y >= layout.content.top &&
          y < layout.content.bottom) {
        row[x] = 0;
      } else if (bar_corner) {
        const double cx = band.left + radius;
        const double cy = band.top + radius;
        const double coverage = std::clamp(
            radius + 0.5 - std::hypot(x + 0.5 - cx, y + 0.5 - cy), 0.0, 1.0);
        row[x] = PhiBackdropPixelOver(row[x], rgb, tint,
                                      static_cast<int>(coverage * 255 + 0.5));
      } else {
        row[x] |= 0xFF000000;
      }
    }
  }
}

HDC PhiShellWindow::AcquireBackBuffer(HDC dc, HGDIOBJ* old_bitmap) {
  RECT rc = {};
  ::GetClientRect(shell_hwnd_, &rc);
  const LONG bw = rc.right > 0 ? rc.right : 1;
  const LONG bh = rc.bottom > 0 ? rc.bottom : 1;
  if (back_buffer_ &&
      (back_buffer_size_.cx != bw || back_buffer_size_.cy != bh)) {
    ::DeleteObject(back_buffer_);
    back_buffer_ = nullptr;
    back_buffer_bits_ = nullptr;
  }
  if (!back_buffer_) {
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bw;
    info.bmiHeader.biHeight = -bh;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    back_buffer_ = ::CreateDIBSection(dc, &info, DIB_RGB_COLORS,
                                     &back_buffer_bits_, nullptr, 0);
    back_buffer_size_ = {bw, bh};
  }
  if (!back_buffer_) {
    return nullptr;
  }
  HDC mem = ::CreateCompatibleDC(dc);
  if (!mem) {
    return nullptr;
  }
  *old_bitmap = ::SelectObject(mem, back_buffer_);
  return mem;
}

void PhiShellWindow::ComposeSlideFrame() {
  if (!shell_hwnd_ || destroying_ || !content_slide_->running()) {
    return;
  }
  // Populate the parent surface beneath the outgoing browser too. GetDC
  // inherits WS_CLIPCHILDREN and would leave that entire rectangle unpainted
  // until after the browser is hidden.
  HDC dc = ::GetDCEx(shell_hwnd_, nullptr, DCX_CACHE | DCX_CLIPSIBLINGS);
  if (!dc) {
    return;
  }
  HGDIOBJ old_bmp = nullptr;
  if (HDC mem = AcquireBackBuffer(dc, &old_bmp)) {
    content_slide_->Compose(mem);
    caption_band_->PaintButtons(mem);
    PrepareBackdropPixels(CurrentPalette());
    ::BitBlt(dc, 0, 0, back_buffer_size_.cx, back_buffer_size_.cy, mem, 0, 0,
             SRCCOPY);
    ::SelectObject(mem, old_bmp);
    ::DeleteDC(mem);
  }
  ::ReleaseDC(shell_hwnd_, dc);
}

void PhiShellWindow::ComposeFrame(HDC mem, const PhiPalette& palette) {
  if (sidebar_resize_snapshot_) {
    PaintChrome(mem, ComputeLayout(), palette, /*include_child_controls=*/true);
    return;
  }
  if (HDC entering = content_slide_->EnteringDC()) {
    // The comfortable Space slide: the entering chrome is painted at rest
    // into the slide's scratch — no caption buttons — with the entering
    // page laid into its content rect, and the composite goes into the
    // frame beside the leaving one at their slid positions; the buttons go
    // on last, standing still over both. A tick composes from the scratch
    // as it is (ComposeSlideFrame); this repaint is a real one's — the
    // rebuild that brings the entering Space's rows, a hover.
    RECT client = {};
    ::GetClientRect(shell_hwnd_, &client);
    HBRUSH fill = ::CreateSolidBrush(BackgroundFill());
    ::FillRect(entering, &client, fill);
    ::DeleteObject(fill);
    {
      base::AutoReset<bool> suppress(&caption_buttons_suppressed_, true);
      // The address text painted in: the EDIT is hidden for the slide.
      PaintChrome(entering, ComputeLayout(), palette,
                  /*include_child_controls=*/true);
    }
    content_slide_->LayEnteringPage();
    content_slide_->Compose(mem);
    caption_band_->PaintButtons(mem);
    return;
  }
  PaintChrome(mem, ComputeLayout(), palette, /*include_child_controls=*/false);
  sidebar_->DrawDragIndicator(mem);
}

void PhiShellWindow::RevealSwappedContent() {
  const HWND reveal = swap_reveal_;
  const HWND conceal = swap_conceal_;
  swap_reveal_ = nullptr;  // The native pages' swap gate opens.
  swap_conceal_ = nullptr;
  const bool had_reveal = reveal != nullptr;
  if (had_reveal && layout_mode_ != PhiLayoutMode::kComfortable) {
    // The card as the screen shows it now holds over the swap: an area a
    // native page uncovers stays empty until the entering child's next
    // commit — a black frame. (The comfortable slide lands from its own
    // composite under the child, ReleaseContentSlide.)
    RECT card = ComputeWindowLayout().content;
    ::MapWindowPoints(shell_hwnd_, nullptr, reinterpret_cast<POINT*>(&card),
                      2);
    if (!reveal_veil_) {
      reveal_veil_ = std::make_unique<PhiRevealVeil>();
    }
    reveal_veil_->Cover(shell_hwnd_, card);
  }
  if (had_reveal) {
    // Deferred past the slide, like the content itself — and up BEFORE the
    // entering child shows, or the framework's own new-tab page under it
    // blinks on screen for a frame.
    native_pages_->MountNtpForReveal();
  }
  if (reveal && ::IsWindow(reveal)) {
    const Layout layout = ComputeWindowLayout();
    ::SetWindowPos(reveal, nullptr, layout.content.left, layout.content.top,
                   layout.content.right - layout.content.left,
                   layout.content.bottom - layout.content.top,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    // Chromium commits the moved, uncovered child on its next frame; ask
    // for one now rather than whenever the page next draws.
    ::RedrawWindow(reveal, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
  }
  browsers_->HidePreviousContent(conceal);
  if (had_reveal) {
    // The leaving content's pages come down only now it is gone.
    native_pages_->SyncNtp();
    // The entering browser was parked off-card for the swap, so the Space
    // switch's surface settle fitted pane readers to the parked panes:
    // fit them again now the panes are on the card.
    if (services_.sidebar) {
      services_.sidebar->NoteContentLayoutChanged(slot_.value);
    }
    // Only restore page focus if no newer native control/window took it.
    const HWND focus = ::GetFocus();
    const HWND page = browsers_->content_hwnd();
    if (::GetForegroundWindow() == shell_hwnd_ &&
        (!focus || focus == shell_hwnd_ || focus == page ||
         (page && ::IsChild(page, focus)))) {
      FocusActiveBrowser();
    }
  }
  FinishUnhost();  // An agent's page was the leaving one: hand it back.
}

void PhiShellWindow::RequestClose() {
  // A slot holds ONE BROWSER PER SPACE VISITED, not one per window: closing
  // only the slot's original browser (the one its window id names) leaves the
  // other Spaces' browsers alive, the slot survives ForgetBrowser, and the
  // window refuses to close after a Space switch.
  //
  // Snapshot before closing: each close can fire OnBrowserWindowClosing
  // synchronously, which mutates space_browsers_ — and destroys `this` when
  // the last browser goes. Everything below is local for that reason.
  if (PhiModalDepthFor(shell_hwnd_) > 0) {
    // A context menu, prompt or picker of this window is pumping messages
    // (a pipe verb or a WM_CLOSE reached us through its loop): destroying
    // the controller now would return into a freed frame. Park it.
    deferred_slot_close_ = true;
    PostDeferredClose();
    return;
  }
  // The Space on screen as the window goes is what the next launch should
  // rebuild first: noted ONCE, here. The browsers then close in map order
  // and the slot falls back through its other Spaces, none of which the
  // user picked (BrowserHostNoteActiveSpace stays quiet from now on).
  if (!closing_ && services_.spaces && !agent_ && !incognito_ &&
      !PhiIsAgentSpaceId(active_space())) {
    services_.spaces->NoteSlotActiveSpace(active_space());
  }
  closing_ = true;
  phi_framework::PhiFramework* framework = framework_;
  HWND shell = shell_hwnd_;
  std::vector<BrowserId> browsers;
  for (const auto& [space_id, browser_id] : browsers_->spaces().all()) {
    browsers.push_back(browser_id);
  }
  for (BrowserId browser_id : browsers_->spaces().retiring()) {
    if (std::find(browsers.begin(), browsers.end(), browser_id) == browsers.end()) {
      browsers.push_back(browser_id);
    }
  }
  if (!framework || browsers.empty()) {
    if (shell && ::IsWindow(shell)) {
      ::DestroyWindow(shell);
    }
    return;
  }
  for (BrowserId browser_id : browsers) {
    framework->CloseBrowserWindow(browser_id.value);
  }
}

void PhiShellWindow::PostDeferredClose() {
  if (shell_hwnd_ && ::IsWindow(shell_hwnd_)) {
    ::PostMessageW(shell_hwnd_, kMsgPhiDeferredClose, 0, 0);
  }
}

void PhiShellWindow::RunDeferredClose() {
  if (PhiModalDepthFor(shell_hwnd_) > 0) {
    // Still inside the loop (a posted message is dispatched by it, and so
    // is this timer): try again once it has had a chance to unwind.
    ::SetTimer(shell_hwnd_, kDeferredCloseTimer, kDeferredCloseRetryMs,
               nullptr);
    return;
  }
  // ONE parked close per turn: any of them can destroy `this`, so the next
  // is re-posted BEFORE it runs — to a window that is then simply gone.
  if (!deferred_space_closes_.empty()) {
    const std::string space_id = deferred_space_closes_.front();
    deferred_space_closes_.erase(deferred_space_closes_.begin());
    if (!deferred_space_closes_.empty() || deferred_slot_close_) {
      PostDeferredClose();
    }
    CloseSpaceBrowser(space_id);
    return;
  }
  if (deferred_slot_close_) {
    deferred_slot_close_ = false;
    RequestClose();  // May destroy `this`; nothing follows.
    return;
  }
  if (framework_) {
    const BrowserId retired = browsers_->TakeRetiredBrowser();
    if (retired.valid()) {
      PostDeferredClose();
      framework_->CloseBrowserWindow(retired.value);
    }
  }
}

bool PhiShellWindow::GetBounds(int* x, int* y, int* w, int* h) const {
  if (!shell_hwnd_ || !::IsWindow(shell_hwnd_)) {
    return false;
  }
  RECT r = {};
  if (!::GetWindowRect(shell_hwnd_, &r)) {
    return false;
  }
  *x = r.left;
  *y = r.top;
  *w = r.right - r.left;
  *h = r.bottom - r.top;
  return true;
}

bool PhiShellWindow::SetBounds(int x, int y, int w, int h) {
  if (!shell_hwnd_ || !::IsWindow(shell_hwnd_)) {
    return false;
  }
  const base::AutoReset<bool> exact(&exact_bounds_pending_, true);
  return ::SetWindowPos(shell_hwnd_, nullptr, x, y, w, h,
                        SWP_NOZORDER | SWP_NOACTIVATE) != 0;
}

void PhiShellWindow::RefreshFieldExtensions() {
  std::vector<FieldExtension> next;
  const BrowserId browser = active_browser_id();
  if (framework_ && browser.valid()) {
    std::optional<base::Value> parsed = base::JSONReader::Read(
        framework_->ListExtensionsJson(browser.value), base::JSON_PARSE_RFC);
    if (parsed && parsed->is_list()) {
      // The pinned set in toolbar order (pinnedIndex), the Mac's presented
      // pinned order.
      std::vector<std::pair<int, FieldExtension>> pinned;
      for (const base::Value& value : parsed->GetList()) {
        const base::DictValue* dict = value.GetIfDict();
        if (!dict || !dict->FindBool("pinned").value_or(false)) {
          continue;
        }
        const std::string* id = dict->FindString("id");
        if (!id) {
          continue;
        }
        FieldExtension ext;
        ext.id = *id;
        ext.state = PhiReadExtensionActionState(*dict);
        // A page action hidden on this tab is not laid out at all (the
        // Mac's visible filter, Chrome's toolbar); a browser action stays
        // whatever its state.
        if (!ext.state.visible) {
          continue;
        }
        pinned.emplace_back(dict->FindInt("pinnedIndex").value_or(0),
                            std::move(ext));
      }
      std::sort(pinned.begin(), pinned.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
      for (auto& [index, ext] : pinned) {
        int w = 0;
        int h = 0;
        const unsigned char* bgra =
            framework_->ExtensionIconBgra(ext.id.c_str(), &w, &h);
        if (bgra && w > 0 && h > 0) {
          const auto source = UNSAFE_BUFFERS(base::span<const unsigned char>(
              bgra, static_cast<size_t>(w) * h * 4));
          ext.icon.assign(source.begin(), source.end());
          ext.icon_w = w;
          ext.icon_h = h;
        }
        next.push_back(std::move(ext));
      }
    }
  }
  // Repaint only on a REAL change: this runs from paint-triggered rebuilds,
  // where an unconditional invalidate would loop.
  const bool changed =
      next.size() != field_extensions_.size() ||
      !std::equal(next.begin(), next.end(), field_extensions_.begin(),
                  [](const FieldExtension& a, const FieldExtension& b) {
                    return a.id == b.id && a.icon_w == b.icon_w &&
                           a.icon_h == b.icon_h && a.state == b.state;
                  });
  field_extensions_ = std::move(next);
  field_ext_count_ = static_cast<int>(field_extensions_.size());
  if (changed && shell_hwnd_) {
    omnibox_->Position();  // The EDIT yields the strip's width.
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
  }
}

RECT PhiShellWindow::FieldExtRect(const Layout& layout, size_t i) const {
  if (layout.field_ext_strip.right <= layout.field_ext_strip.left) {
    return RECT{};
  }
  const int slot = Scaled(kExtSlot);
  const LONG left = layout.field_ext_strip.left +
                    static_cast<LONG>(i) * (slot + Scaled(kExtSlotGap));
  return {left, layout.field_ext_strip.top, left + slot,
          layout.field_ext_strip.bottom};
}

bool PhiShellWindow::ExtensionsButtonScreenRect(RECT* rect) {
  if (!shell_hwnd_ || !::IsWindow(shell_hwnd_)) {
    return false;
  }
  const RECT button = ComputeLayout().extensions;
  if (button.right <= button.left || button.bottom <= button.top) {
    return false;
  }
  *rect = button;
  ::MapWindowPoints(shell_hwnd_, nullptr, reinterpret_cast<POINT*>(rect), 2);
  return true;
}

void PhiShellWindow::ShowFieldExtensionMenu(size_t index, const RECT& slot) {
  const BrowserId browser = active_browser_id();
  if (!framework_ || !browser.valid() || index >= field_extensions_.size()) {
    return;
  }
  // The icon's bottom-left corner in screen coordinates — the Mac's
  // ExtensionPopupAnchor.pointBelowView.
  const RECT anchor = LayoutToScreen(slot);
  framework_->ShowExtensionContextMenu(browser.value,
                                       field_extensions_[index].id.c_str(),
                                       anchor.left, anchor.bottom);
}

bool PhiShellWindow::FieldToolsShown() const {
  // The Mac fades the pair in with the address bar's hover and keeps it
  // while its menu is up; the copy flash holds Copy link for its second.
  // A window with no tab (the placeholder) offers neither.
  if (browsers_->ActiveBrowserPlaceholding()) {
    return false;
  }
  return omnibox_->field_hover() || site_menu_open_ ||
         omnibox_->copy_link_flash();
}

namespace {

// `url`'s origin ("https://example.com:8080"), or the URL itself when it
// has no authority to cut at.
std::string SiteOrigin(const std::string& url) {
  const size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos) {
    return url;
  }
  const size_t path = url.find_first_of("/?#", scheme_end + 3);
  return path == std::string::npos ? url : url.substr(0, path);
}

// A query-parameter value, percent-encoded (Chromium's
// EscapeQueryParamValue for the characters an origin can carry).
std::string EscapeQueryValue(const std::string& value) {
  static constexpr std::string_view kHex = "0123456789ABCDEF";
  std::string out;
  for (const unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0xF];
    }
  }
  return out;
}

}  // namespace

void PhiShellWindow::ShowSiteMenu(const RECT& slot) {
  const BrowserId browser = active_browser_id();
  if (!framework_ || !browser.valid()) {
    return;
  }
  // What the menu shows, read once before the loop (the Mac builds its
  // NSMenu from the tab's state the same way).
  PhiSiteMenuInputs inputs;
  std::string url;
  if (const char* json = framework_->SiteMenuJson(browser.value)) {
    std::optional<base::DictValue> info =
        base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
    if (info) {
      if (const std::string* value = info->FindString("url")) {
        url = *value;
      }
      if (const std::string* value = info->FindString("security")) {
        inputs.security = *value;
      }
      if (const base::DictValue* blocking = info->FindDict("contentBlocking")) {
        inputs.content_blocking_row = true;
        inputs.content_blocking_on =
            blocking->FindBool("blocking").value_or(true);
      }
    }
  }
  inputs.always_show_url_path =
      services_.prefs && services_.prefs->AlwaysShowFullUrl();

  // Under the button, its left edge on the button's (the Mac pops the menu
  // 6pt below its anchor).
  site_menu_open_ = true;
  ::InvalidateRect(shell_hwnd_, &slot, FALSE);
  const PhiMenuAction action = PhiRunPopupMenu(
      PhiBuildSiteMenu(inputs),
      PopupAt(POINT{slot.left, slot.bottom + Scaled(4)},
              TPM_LEFTALIGN | TPM_TOPALIGN));
  site_menu_open_ = false;
  // The loop ate the pointer's moves: settle the field's hover from where
  // the cursor is now, which also repaints the pair away if it left.
  omnibox_->SetFieldHover(false);
  ::InvalidateRect(shell_hwnd_, &slot, FALSE);

  // The browser may have gone under the loop; re-read before acting.
  const BrowserId target = active_browser_id();
  if (!action.valid() || !framework_ || !target.valid()) {
    return;
  }
  if (action.is("site.clearCache")) {
    framework_->ClearActiveSiteData(target.value, /*cookies=*/false);
  } else if (action.is("site.clearCookies")) {
    framework_->ClearActiveSiteData(target.value, /*cookies=*/true);
  } else if (action.is("site.moreSettings")) {
    // Chromium's per-site settings page for the tab's origin.
    const std::string page = "chrome://settings/content/siteDetails?site=" +
                             EscapeQueryValue(SiteOrigin(url));
    framework_->OpenTab(target, page.c_str());
  } else if (action.is("site.contentBlocking")) {
    // The page picks the rule set up on its next load, so reload it (the
    // Mac's onToggled).
    if (framework_->SetActiveSiteContentBlocking(
            target.value, !inputs.content_blocking_on)) {
      framework_->Reload(target.value, /*bypass_cache=*/false);
    }
  } else if (action.is("site.showFullUrl")) {
    if (services_.prefs) {
      services_.prefs->SetAlwaysShowFullUrl(!inputs.always_show_url_path);
    }
  } else if (action.is("site.security") || action.is("site.securityWarning")) {
    framework_->ShowActiveSiteCertificate(target.value);
  }
}

void PhiShellWindow::RebuildChips() {
  spaces_->Rebuild();
}

// The rebuild turn (docs/architecture-review.md §3.1 principle 3): the
// chrome's own caches first, then the sidebar's one pass over the model.
// Nothing here paints, and nothing else writes a row.
void PhiShellWindow::RebuildTabs() {
  SyncSplitChipTimer();  // The split state may have changed with the rows.
  // The floating panel shows the EXPANDED sidebar of a collapsed shell.
  // While it is up the rows are built in that geometry — the docked,
  // collapsed shell shows no rows of its own — so the panel paints them as
  // they are and never rebuilds inside its paint (lane 3 §3.1).
  RebuildNavState();
  RefreshFieldExtensions();
  {
    const base::AutoReset<bool> floating_scope(
        &floating_layout_, floating_layout_ || floating_sidebar_ != nullptr);
    RebuildChips();
    rows_dirty_ = false;
    sidebar_->Rebuild();
  }
  // A group overview open over this window lists the rows just rebuilt.
  PhiGroupOverview::RefreshIfBoundTo(this);
  // The Library's cards re-read their Spaces, pins and bookmarks; a changed
  // active tab or Space closes it (the Mac's focusingTab subscription).
  PhiLibrary::RefreshIfBoundTo(this);
  // After the rows: the active tab (a reader redirects to its origin) is
  // theirs. Cached here like the nav state, so the rebuild turn's signature
  // sees the change and paint reads one settled value.
  page_color_ = ComputeActivePageColor();
  page_color_reader_ = ActiveTabShowsReader();
  const bool split = ActiveTabIsSplit() && !browsers_->content_fullscreen();
  if (split != backdrop_split_) {
    UpdateSidebarBackdrop();
    PlaceContentChildren(ComputeWindowLayout());
    PushSplitViewStyle();
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
  }
  // The strip row came or went (a second Space created, the last other one
  // deleted): the address bar and every band under it moved, and nothing
  // else repaints a band that was empty a moment ago.
  const bool spaces_switch = ComputeLayout().spaces_switch;
  if (spaces_switch != laid_out_spaces_switch_) {
    laid_out_spaces_switch_ = spaces_switch;
    LayoutContents();
  }
  RetitleShell();
}

void PhiShellWindow::RetitleShell() {
  if (!shell_hwnd_ || !::IsWindow(shell_hwnd_)) {
    return;
  }
  // The active tab's title; the Space's name while the placeholder page is
  // up or the tab has none yet; "Phi" with neither. The Mac's placeholder
  // is its native new-tab page, so a tab showing ours counts as one here
  // too (Chromium would title it "New Tab"). The taskbar and Alt+Tab read
  // this — the caption band draws no title of its own.
  std::string title;
  if (!browsers_->ActiveBrowserPlaceholding() &&
      !PhiNewTabPage::IsNewTabUrl(active_tab_url()) && model_) {
    if (const PhiWindowModel::Tab* tab = model_->ActiveTab(active_browser_id())) {
      title = tab->title;
    }
  }
  if (title.empty() && services_.spaces) {
    for (const PhiSpaceView& space : services_.spaces->Spaces()) {
      if (space.id == active_space()) {
        title = space.name;
        break;
      }
    }
  }
  if (title.empty()) {
    title = "Phi";
  }
  std::wstring text = base::UTF8ToWide(title);
  if (text == shell_title_) {
    return;
  }
  shell_title_ = std::move(text);
  ::SetWindowTextW(shell_hwnd_, shell_title_.c_str());
}

// --- The sidebar's public surface, forwarded --------------------------------
// The shell keeps the spellings its callers (the coordinator, the switcher,
// the search palette, the agent dialect) already use; the rows themselves
// live in the component.

TabId PhiShellWindow::active_tab_id() const {
  return sidebar_->active_tab_id();
}

const std::string& PhiShellWindow::active_tab_url() const {
  return sidebar_->active_tab_url();
}

BrowserId PhiShellWindow::PageCommandBrowser() const {
  const BrowserId browser = active_browser_id();
  if (services_.sidebar && browser.valid()) {
    const BrowserId peek =
        services_.sidebar->PresentedPeekBrowser(browser.value);
    if (peek.valid()) {
      return peek;
    }
  }
  return browser;
}

TabId PhiShellWindow::PageCommandTab() const {
  const BrowserId page = PageCommandBrowser();
  if (page == active_browser_id()) {
    return active_tab_id();
  }
  // A presented peek's one tab — none (rather than the opener's) while its
  // browser's first snapshot is still on its way.
  const phi_framework::TabSnapshot* tab =
      model_ ? model_->ActiveTab(page) : nullptr;
  return tab ? tab->id : TabId();
}

bool PhiShellWindow::ShowsTab(TabId tab_id) const {
  return sidebar_->ShowsTab(tab_id);
}

const std::vector<TabId>& PhiShellWindow::TabMruOrder() const {
  return sidebar_->mru();
}

void PhiShellWindow::OpenOrFocusPinned(const std::string& guid,
                                            const std::string& url) {
  sidebar_->OpenOrFocusPinned(guid, url);
}

base::ListValue PhiShellWindow::LayoutRowsJson() const {
  return sidebar_->LayoutRowsJson();
}

base::DictValue PhiShellWindow::DragStateJson() const {
  return sidebar_->DragStateJson();
}

void PhiShellWindow::OnChromeMiddleClick(int x, int y) {
  CloseExtensionPopup();
  sidebar_->OnMiddleClick(POINT{x, y});
}

void PhiShellWindow::CloseExtensionPopup() {
  // Chromium closes an action popup when any widget of its window activates
  // — a click on the Mac's window, on Chrome's toolbar. The shell's chrome
  // is not a views widget, so a click here has to say so itself.
  const BrowserId browser = active_browser_id();
  if (framework_ && browser.valid()) {
    framework_->CloseExtensionPopup(browser.value);
  }
}






void PhiShellWindow::OnChromeClick(int x, int y) {
  // Any press on the chrome is an interaction: the hover card goes down and
  // stays down until the pointer leaves the row it was pressed on.
  if (tab_preview_) {
    tab_preview_->Cancel();
  }
  DismissChromeTip();  // Likewise the button's tooltip.
  CloseExtensionPopup();
  if (!floating_layout_ && floating_expanded_) {
    HideFloatingSidebar(/*animated=*/true);  // A click on the row tucks it.
  }
  // Clicking anywhere else in the chrome means the user is done editing the
  // address; hand the keyboard back to the page (the field is a child window,
  // so clicks on it never reach here).
  if (omnibox_->FieldHasFocus()) {
    FocusActiveBrowser();
  }
  const Layout layout = ComputeLayout();
  POINT pt = {x, y};
  if (framework_ && ::PtInRect(&layout.back, pt)) {
    // Back while the reader covers this tab reads as "leave the reader" —
    // the Mac's closeReaderOverlayForBackForwardNavigation, and what the
    // in-place reader did before there was an overlay.
    if (LeaveReaderViewForNavigation()) {
      return;
    }
    if (const BrowserId browser = active_browser_id(); browser.valid()) {
      framework_->GoBack(browser.value);
    }
    return;
  }
  if (framework_ && ::PtInRect(&layout.forward, pt)) {
    if (LeaveReaderViewForNavigation()) {
      return;
    }
    if (const BrowserId browser = active_browser_id(); browser.valid()) {
      framework_->GoForward(browser.value);
    }
    return;
  }
  if (framework_ && ::PtInRect(&layout.reload, pt)) {
    // The button describes the field's page — the presented peek's while
    // one is up (RebuildNavState) — so it reloads that page.
    if (const BrowserId browser = PageCommandBrowser(); browser.valid()) {
      framework_->Reload(browser.value, /*bypass_cache=*/false);
    }
    return;
  }
  if (::PtInRect(&layout.agent_chip, pt) && !agent_chip_task_.empty()) {
    // The full chip opens the agent menu; the compact one switches to the
    // agent's Space ("click to watch").
    if (agent_chip_compact_) {
      SwitchToSpace(agent_chip_space_);
    } else {
      ShowAgentChipMenu(pt, agent_chip_task_);
    }
    return;
  }
  if (::PtInRect(&layout.search_tabs, pt)) {
    PhiSearchTabsPanel::Toggle(this);  // The header's magnifier.
    return;
  }
  if (::PtInRect(&layout.update, pt)) {
    // The Update reminder: WinSparkle's update window, where the user
    // installs, skips or postpones (the Mac's checkForUpdate).
    PhiUpdater::CheckForUpdateWithUi();
    return;
  }
  if (::PtInRect(&layout.sidebar_toggle, pt)) {
    ToggleSidebar();  // The sidebar header's toggle, View ▸ Toggle Sidebar.
    return;
  }
  if (::PtInRect(&layout.sidebar_profile, pt)) {
    // The Mac ProfileButton's menu. No analytics: the Mac counts none.
    ShowProfileMenu();
    return;
  }
  if (::PtInRect(&layout.sidebar_downloads, pt)) {
    // The Mac DownloadButtonView's popover toggle — and its
    // feature_entry_tapped: the button lives in the sidebar's bottom bar
    // (the address row carries none; the app menu and Ctrl+J reach the
    // same popover).
    if (services_.window) {
      services_.window->CaptureAnalytics(
          kAnalyticsModuleFeature, kEventFeatureEntryTapped,
          PhiFeatureEntryProperties("download", "sidebar"));
    }
    const BrowserId browser = active_browser_id();
    if (services_.spaces && framework_ && browser.valid()) {
      const RECT anchor = LayoutToScreen(layout.sidebar_downloads);
      PhiDownloadsPopover::Toggle(shell_hwnd_, anchor,
                                  services_.spaces->PaletteForSlot(slot_.value),
                                  framework_, browser);
    }
    return;
  }
  if (::PtInRect(&layout.feedback, pt)) {
    OpenFeedback();  // The sidebar's Feedback capsule (SidebarBottomBar).
    return;
  }
  if (::PtInRect(&layout.extensions, pt)) {
    // The extensions popover, off the puzzle button.
    const BrowserId browser = active_browser_id();
    if (services_.spaces && framework_ && browser.valid()) {
      const RECT anchor = LayoutToScreen(layout.extensions);
      PhiExtensionsPopover::Toggle(
          shell_hwnd_, anchor, services_.spaces->PaletteForSlot(slot_.value),
          framework_, browser);
    }
    return;
  }
  for (size_t i = 0; i < field_extensions_.size(); ++i) {
    const RECT slot = FieldExtRect(layout, i);
    if (slot.right > slot.left && ::PtInRect(&slot, pt)) {
      // A pinned extension's in-field icon: run its action anchored at the
      // icon (a popup-bearing action opens Chromium's popup there). A
      // DISABLED action doesn't run; fall back to its context menu like
      // Chrome (ExecuteUserAction) — the Mac's extensionButtonClicked.
      if (!field_extensions_[i].state.enabled) {
        ShowFieldExtensionMenu(i, slot);
        return;
      }
      const BrowserId browser = active_browser_id();
      if (framework_ && browser.valid()) {
        const RECT anchor = LayoutToScreen(slot);
        framework_->TriggerExtensionAction(
            browser.value,
            field_extensions_[i].id.c_str(), anchor.left, anchor.top,
            anchor.right - anchor.left, anchor.bottom - anchor.top);
      }
      return;
    }
  }
  if ((FieldToolsShown() || ActiveTabShowsReader()) &&
      ::PtInRect(&layout.reader, pt)) {
    ToggleReaderView("address_bar");  // The in-field reader button.
    return;
  }
  if (FieldToolsShown() && ::PtInRect(&layout.copy_link, pt)) {
    // The in-field Copy link (the Mac's CopyURLButtonView): the URL to the
    // clipboard and a check mark for a second — no toast, the shortcut's.
    omnibox_->CopyLinkToClipboard();
    return;
  }
  if (FieldToolsShown() && ::PtInRect(&layout.site_menu, pt)) {
    ShowSiteMenu(layout.site_menu);
    return;
  }
  for (const BarItem& item : bar_items_) {
    if (!::PtInRect(&item.bounds, pt)) {
      continue;
    }
    if (item.overflow) {
      // Skip the pills already on the bar; the menu takes the rest.
      ShowBookmarkBarMenu(item.bounds, std::string(),
                          static_cast<size_t>(item.bookmark));
    } else if (item.bookmark >= 0 &&
               item.bookmark < static_cast<int>(sidebar_->bookmarks().size())) {
      const ShortcutRow& row =
          sidebar_->bookmarks()[static_cast<size_t>(item.bookmark)];
      if (row.folder) {
        ShowBookmarkBarMenu(item.bounds, row.guid, 0);
      } else if (!row.url.empty()) {
        OpenOrFocusPinned(row.guid, row.url);
      }
    }
    return;
  }
  // The stored bands are the sidebar's own (phi_sidebar_view.h): the pinned
  // grid and the bookmark outline, hit-tested here so the bookmark BAR's
  // pills above still take their clicks first.
  if (!BandOnShell()) {
    return;  // The band's rows are the floating panel's (OnChromeMouseMove).
  }
  if (sidebar_->OnStoredRowClick(pt)) {
    return;
  }
  if (framework_ && (::PtInRect(&layout.new_tab_row, pt) ||
                     ::PtInRect(&sidebar_->add_tab_button(), pt))) {
    if (!AgentHoldsInput()) {  // The Mac refuses New Tab under the agent.
      StartNewTab();
    }
    return;
  }
  if (::PtInRect(&layout.app_menu, pt)) {
    ShowAppMenu();
    return;
  }
  // The chips and rows are hit-tested as they are — what the last frame
  // showed under the pointer (docs/architecture-review.md §3.1 principle 7).
  if (const int hit = spaces_->ChipAt(pt); hit >= 0) {
    const PhiSpaceChip chip = spaces_->chips()[static_cast<size_t>(hit)];
    // A click means "switch", not "hover": drop the pip's card and keep it
    // down until the pointer leaves this pip — without this the switched-to
    // strip's fresh hover re-presents it right after the swap (the Mac's
    // suppressHoverCard). The modal popup below would freeze it, too.
    spaces_->DismissHoverCard(chip.id);
    // An agent pip starts watching that agent Space — or, from another
    // agent Space, swaps to it. It never reorders.
    if (chip.is_agent) {
      // The agent Space on screen (hosted in this slot, or this IS its
      // window) is the active Space: a click on its pip does nothing, as
      // on Mac. Another agent's pip swaps.
      if (chip.id == active_space() || (agent_ && chip.open)) {
        return;
      }
      SwitchToSpace(chip.id);
      return;
    }
    if (agent_ || HostsAgentSpace()) {
      // A user pip inside the watched agent window: the end of the watch.
      SwitchToSpace(chip.id);
      return;
    }
    if (!layout.horizontal_tabs) {
      // Vertical strips show every pip: reorderable by drag.
      sidebar_->ArmDrag(SidebarKey::Space(chip.id), pt);
    }
    if (layout.horizontal_tabs) {
      // Comfortable draws ONE compact pip — the active Space. Clicking it
      // opens the switcher popup over the FULL Space list with "New Space…"
      // after a rule, the Mac's spaceSwitcherMenu on its compact chip.
      ShowSpaceSwitcherMenu(pt, /*overflow=*/false);
      return;
    }
    if (spaces_->creating()) {
      return;  // Read-only while the form is up (the Mac's isCreatingSpace).
    }
    SwitchToSpace(chip.id);
    return;
  }
  if (services_.spaces && ::PtInRect(&layout.add_space, pt)) {
    if (spaces_->creating()) {
      return;  // The form is already up: requestCreation is a no-op then.
    }
    if (!spaces_->overflow()) {
      // The pointer is on the row, so the "+" is up — or would be, had the
      // hover-in reached us: a click under a pointer that never moved since
      // a menu dismissed over it reveals the button rather than being lost.
      // (The Mac's flag is watchdog-verified against the real pointer, so
      // its hidden-and-disabled "+" never sits under a pointer on the row.)
      spaces_->SetRowHovered(true);
      spaces_->ShowCreateForm();
      return;
    }
    // Overflow: the "…" opens the full switcher — every Space plus a
    // "New Space" row (the Mac moreButton's menu). Anchored under the
    // button, not at the pointer.
    const POINT anchor = {layout.add_space.left, layout.add_space.bottom};
    ShowSpaceSwitcherMenu(anchor, /*overflow=*/true);
    return;
  }
  // The content rows last, as the pixels stack: a group header or a tab row.
  sidebar_->OnContentRowClick(pt);
}


namespace {

// Puts `text` on the clipboard — the context menus' Copy Link.
void CopyTextToClipboard(HWND owner, const std::wstring& text) {
  if (!::OpenClipboard(owner)) {
    return;
  }
  ::EmptyClipboard();
  const size_t count = text.size() + 1;
  if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, count * sizeof(wchar_t))) {
    if (void* dest = ::GlobalLock(mem)) {
      // SAFETY: the allocation above is exactly `count` wchar_t.
      auto span = UNSAFE_BUFFERS(
          base::span<wchar_t>(static_cast<wchar_t*>(dest), count));
      std::copy(text.begin(), text.end(), span.begin());
      span[text.size()] = L'\0';
      ::GlobalUnlock(mem);
      if (::SetClipboardData(CF_UNICODETEXT, mem)) {
        mem = nullptr;  // The system owns it now.
      }
    }
    if (mem) {
      ::GlobalFree(mem);
    }
  }
  ::CloseClipboard();
}

}  // namespace


// --- Context menus (phi_shell_menus.h) -------------------------------------
// Every menu below is DATA (docs/architecture-review.md §3.1 principle 4,
// §3.5, lane 1 §3.2): the row's identity is copied into a snapshot, a pure
// builder turns that snapshot plus the live prefs into a PhiMenuSpec, the
// popup runs under a PhiModalScope, and the answer comes back as a key plus
// the target it names. Nothing here holds a reference into the sidebar's
// rows or the strip's chips across the popup's nested message
// loop, so the rebuild a title or favicon notification posts under an open
// menu cannot leave a dangling row behind — by construction, not by
// remembering to copy fields out first.

PhiPopupMenuRequest PhiShellWindow::PopupAt(POINT client_pt,
                                                 UINT align) const {
  const POINT screen = LayoutToScreen(client_pt);
  popup_palette_ = CurrentPalette();
  PhiPopupMenuRequest request;
  request.owner = shell_hwnd_;
  request.x = screen.x;
  request.y = screen.y;
  request.align = align;
  request.palette = &popup_palette_;
  return request;
}

bool PhiShellWindow::ActiveTabIsSplit() const {
  const BrowserId browser = active_browser_id();
  const TabId tab = active_tab_id();
  return model_ && browser.valid() && tab.valid() &&
         model_->SplitOfTab(browser, tab) != nullptr;
}

unsigned PhiShellWindow::SplitPaneCardCorners(const RECT& pane) {
  if (!shell_hwnd_ || ::IsRectEmpty(&pane) ||
      browsers_->content_fullscreen()) {
    return 0;
  }
  // Geometric, so either split axis reads the same: a pane edge on the
  // card's edge (within a DIP rounding or two).
  const RECT content = ComputeWindowLayout().content;
  const int slack = Scaled(2);
  const bool bottom = pane.bottom >= content.bottom - slack;
  unsigned corners = 0;
  if (bottom && pane.left <= content.left + slack) {
    corners |= 4;  // Against the sidebar: the card's own corner.
  }
  if (bottom && pane.right >= content.right - slack &&
      !::IsZoomed(shell_hwnd_)) {
    corners |= 8;  // The window's corner, which Windows rounds when restored.
  }
  return corners;
}

int PhiShellWindow::ContentCornerRadius() const {
  return Scaled(kContentRadius);
}

RECT PhiShellWindow::app_menu_rect() {
  return ComputeLayout().app_menu;
}

RECT PhiShellWindow::profile_rect() {
  return ComputeLayout().sidebar_profile;
}

void PhiShellWindow::InvalidateBottomBar() {
  // The profile button's picture is read at paint time, so the bar alone
  // recomposes — in the floating panel too, which paints the same layout
  // at its own offset (and whose rects the layout carries while it is up,
  // when the collapsed shell has no bar of its own to repaint).
  const RECT bar = ComputeLayout().bottom_bar;
  if (shell_hwnd_ && bar.right > bar.left && bar.bottom > bar.top) {
    ::InvalidateRect(shell_hwnd_, &bar, FALSE);
  }
  if (const HWND panel = SpacesFloatingPanel(nullptr)) {
    ::InvalidateRect(panel, nullptr, FALSE);
  }
}

RECT PhiShellWindow::copy_link_rect() {
  return ComputeLayout().copy_link;
}

RECT PhiShellWindow::site_menu_rect() {
  return ComputeLayout().site_menu;
}

std::vector<PhiMenuChoice> PhiShellWindow::SpaceMenuChoices(
    bool with_incognito) const {
  // Every Space but the one this slot shows — the Mac's
  // appendSpaceTransferMenuItems. Ids, never rows: the list may be rebuilt
  // while the menu is up.
  std::vector<PhiMenuChoice> choices;
  if (!services_.spaces) {
    return choices;
  }
  for (const PhiSpaceView& space : services_.spaces->Spaces()) {
    if (space.id == active_space() || choices.size() >= 99u ||
        (space.incognito && !with_incognito)) {
      continue;
    }
    PhiMenuChoice choice;
    choice.id = space.id;
    choice.label = MenuLabel(base::UTF8ToWide(
        !space.name.empty() ? space.name : "Space " + space.id));
    choices.push_back(std::move(choice));
  }
  return choices;
}

std::vector<PhiMenuChoice> PhiShellWindow::BookmarkFolderChoices() const {
  // This Space's bookmark folders in outline order, nesting indented.
  std::vector<PhiMenuChoice> choices;
  if (!services_.sidebar) {
    return choices;
  }
  const std::function<void(const std::string&, int)> walk =
      [&](const std::string& parent, int depth) {
        for (const PhiStoredRowView& stored :
             services_.sidebar->BookmarkRows(slot_.value, parent)) {
          if (!stored.folder || choices.size() >= 99u) {
            continue;
          }
          PhiMenuChoice choice;
          choice.id = stored.guid;
          choice.label.assign(static_cast<size_t>(depth) * 2, L' ');
          choice.label += MenuLabel(base::UTF8ToWide(
              stored.title.empty() ? "Untitled" : stored.title));
          choices.push_back(std::move(choice));
          walk(stored.guid, depth + 1);
        }
      };
  walk(std::string(), 0);
  return choices;
}

std::vector<PhiMenuChoice> PhiShellWindow::GroupMenuChoices(
    const std::string& exclude) const {
  // A group's menu title: its name (or "Untitled") with its member count,
  // the Mac's displayTitle(memberCount:).
  std::vector<PhiMenuChoice> choices;
  for (const GroupHeader& gh : sidebar_->group_headers()) {
    if (gh.id == exclude || choices.size() >= 99u) {
      continue;
    }
    int members = 0;
    for (const TabRow& tab : sidebar_->tabs()) {
      if (tab.group_id == gh.id) {
        ++members;
      }
    }
    PhiMenuChoice choice;
    choice.id = gh.id;
    choice.label =
        MenuLabel(gh.name.empty() ? std::wstring(L"Untitled") : gh.name) +
        L" (" + std::to_wstring(members) + L")";
    choices.push_back(std::move(choice));
  }
  return choices;
}

std::vector<PhiMenuChoice> PhiShellWindow::SpaceSwitcherChoices() const {
  // The FULL Space list, with each Space's glyph — the strip's chips only
  // mirror what fits.
  std::vector<PhiMenuChoice> choices;
  if (!services_.spaces) {
    return choices;
  }
  // The switcher's second line (§5.3): "profile · N tabs", and the Ctrl+n
  // keycap by strip position (user Spaces lead the strip).
  const std::vector<std::string> profiles = services_.spaces->ProfileSummaries();
  const auto profile_name = [&profiles](const std::string& id) {
    for (const std::string& summary : profiles) {
      const size_t tab = summary.find('\t');
      if (summary.substr(0, tab) == id) {
        return base::UTF8ToWide(
            tab != std::string::npos ? summary.substr(tab + 1) : id);
      }
    }
    return base::UTF8ToWide(id);
  };
  int ordinal = 0;
  for (const PhiSpaceView& space : services_.spaces->Spaces()) {
    ++ordinal;
    PhiMenuChoice choice;
    choice.id = space.id;
    choice.label = MenuLabel(base::UTF8ToWide(
        !space.name.empty() ? space.name : "Space " + space.id));
    choice.glyph = base::UTF8ToWide(space.icon_text);
    const int tabs = SpacesTabCount(space.id);
    choice.hint = (space.incognito
                       ? std::wstring(PhiStr(
                             "spaces.builtIn.incognito.singleSpaceName"))
                       : profile_name(space.profile_id)) +
                  L"  \u00B7  " + std::to_wstring(tabs) +
                  (tabs == 1 ? L" tab" : L" tabs");
    if (ordinal <= 9) {
      choice.shortcut = L"Ctrl+" + std::to_wstring(ordinal);
    }
    choices.push_back(std::move(choice));
  }
  return choices;
}

std::vector<PhiMenuBookmarkNode> PhiShellWindow::BookmarkTreeSnapshot(
    const std::string& parent_guid,
    int depth) const {
  // The store's outline as a VALUE, read before the dropdown opens.
  std::vector<PhiMenuBookmarkNode> rows;
  if (!services_.sidebar || depth > 5) {
    return rows;
  }
  for (const PhiStoredRowView& row :
       services_.sidebar->BookmarkRows(slot_.value, parent_guid)) {
    PhiMenuBookmarkNode node;
    node.guid = row.guid;
    node.url = row.url;
    node.title = base::UTF8ToWide(row.title.empty() ? row.url : row.title);
    node.folder = row.folder;
    if (row.folder) {
      node.children = BookmarkTreeSnapshot(row.guid, depth + 1);
    }
    rows.push_back(std::move(node));
  }
  return rows;
}

BrowserId PhiShellWindow::StillHeld(BrowserId opening_browser) const {
  return opening_browser.valid() &&
                 !browsers_->spaces().SpaceOf(opening_browser).empty()
             ? opening_browser
             : BrowserId();
}

void PhiShellWindow::ShowGroupHeaderMenu(POINT pt,
                                              std::string group_id,
                                              std::wstring title,
                                              COLORREF color) {
  const BrowserId opening_browser = active_browser_id();
  std::vector<PhiMenuColorOption> colors;
  colors.reserve(kPhiGroupColors.size());
  for (const PhiGroupColor& option : kPhiGroupColors) {
    colors.push_back({option.name, option.color == color});
  }
  const PhiMenuAction action =
      PhiRunPopupMenu(PhiBuildGroupHeaderMenu(colors),
                      PopupAt(pt, TPM_LEFTALIGN | TPM_TOPALIGN |
                                      TPM_RIGHTBUTTON));
  // The group's browser, checked AFTER the loop: the slot may have lost it
  // (or switched Space) under the menu.
  const int64_t browser = StillHeld(opening_browser).value;
  if (!framework_ || !browser) {
    return;
  }
  if (action.is("group.newTab")) {
    // A fresh tab, added to the group (the Mac's newTabInGroup).
    const TabId created = framework_->OpenTab(BrowserId{browser}, "");
    if (created.valid()) {
      const std::string ids = "[" + std::to_string(created.value) + "]";
      framework_->AddTabsToGroup(browser, group_id.c_str(), ids.c_str());
    }
  } else if (action.is("group.rename")) {
    std::wstring value = title;
    if (PhiPromptText(shell_hwnd_,
                      PhiStr("sidebar.tabGroupContextMenu.renameAction"),
                      PhiStr("sidebar.groupName"), &value,
                      /*confirm_label=*/nullptr, kPhiNameRules)) {
      framework_->RenameTabGroup(browser, group_id.c_str(),
                                 base::WideToUTF8(value).c_str());
    }
  } else if (action.is("group.ungroup")) {
    framework_->UngroupTabGroup(browser, group_id.c_str());
  } else if (action.is("group.close")) {
    framework_->CloseTabGroup(browser, group_id.c_str());
  } else if (action.is("group.color")) {
    framework_->SetTabGroupColor(browser, group_id.c_str(),
                                 action.target.id.c_str());
  }
}

void PhiShellWindow::ShowTabRowMenu(POINT pt, const MenuRow& row) {
  // The Mac tab menu (TabModel+Sidebar), in its order: pin, duplicate, copy,
  // the split block, bookmarks, groups, then close.
  const BrowserId opening_browser = active_browser_id();
  if (!opening_browser.valid()) {
    return;  // A row of a browser that is already gone.
  }
  // "Split with Current" pairs with the tab focused as the menu OPENED.
  const int opening_active_tab = sidebar_->tree().active_tab;
  PhiTabMenuInputs in;
  in.merged = row.merged;
  in.incognito = ShowsIncognito();
  in.grouped = !row.group_id.empty();
  in.split_bound = !row.split_id.empty();
  in.split_stacked = row.merged && SplitIsStacked(row.id);
  if (services_.agent && row.id.valid()) {
    in.agent_driven = services_.agent->IsTabAgentDriven(row.id.value);
    in.agent_reclaimed =
        in.agent_driven && services_.agent->IsTabReclaimed(row.id.value);
  }
  if (!row.merged && row.split_id.empty()) {
    // "Split with Current" pairs this row with the FOCUSED tab; on the
    // focused row itself — or when the focused tab is already split — it
    // becomes "Open as Split", a fresh pane (the Mac's two labels).
    for (const TabRow& other : sidebar_->tabs()) {
      if (other.key != row.key && other.active && other.b_index < 0 &&
          other.split_id.empty()) {
        in.current_can_pair = true;
        break;
      }
    }
  }
  // Groups take a split row whole (both panes), so every row gets the list.
  in.groups = GroupMenuChoices(row.group_id);
  if (!row.merged) {
    in.bookmark_folders = !ShowsIncognito() && services_.sidebar != nullptr;
    if (in.bookmark_folders) {
      in.folders = BookmarkFolderChoices();
    }
    // Out of an Incognito Space nothing moves (the Mac offers no Move);
    // into one, a tab may.
    if (!ShowsIncognito()) {
      in.spaces = SpaceMenuChoices(/*with_incognito=*/true);
    }
  }
  const PhiMenuAction action =
      PhiRunPopupMenu(PhiBuildTabRowMenu(in),
                      PopupAt(pt, TPM_LEFTALIGN | TPM_TOPALIGN |
                                      TPM_RIGHTBUTTON));
  if (!action.valid()) {
    return;
  }
  // The row's browser, checked after the loop — the row's identity below is
  // the snapshot's, which no rebuild can have touched; a Space switch under
  // the menu must not turn the action on another browser's tabs.
  const BrowserId browser = StillHeld(opening_browser);
  if (!browser.valid()) {
    return;
  }
  if (action.is("tab.moveToSpace")) {
    sidebar_->Apply(
        SidebarMoveToSpace{row.key, action.target.id, /*clone=*/false});
  } else if (action.is("tab.cloneToSpace")) {
    sidebar_->Apply(
        SidebarMoveToSpace{row.key, action.target.id, /*clone=*/true});
  } else if (action.is("tab.addToFolder") && services_.sidebar) {
    services_.sidebar->AddBookmarkRow(slot_.value, action.target.id,
                          base::WideToUTF8(row.title), row.url);
  } else if (action.is("tab.newFolder") && services_.sidebar) {
    std::wstring name = PhiStr("common.bookmarkFolder.defaultName");
    if (PhiPromptText(shell_hwnd_,
                      PhiStr("app.bookmarkFolderDialog.createTitle"),
                      PhiStr("bookmarkManager.folderName"), &name,
                      /*confirm_label=*/nullptr, kPhiNameRules)) {
      const std::string folder = services_.sidebar->AddBookmarkFolder(
          slot_.value, std::string(), base::WideToUTF8(name));
      if (!folder.empty()) {
        services_.sidebar->AddBookmarkRow(slot_.value, folder,
                              base::WideToUTF8(row.title), row.url);
      }
    }
  } else if (action.is("tab.joinGroup")) {
    // Joining (or moving to) an existing group: the model takes the tab out
    // of its current group atomically, so one call covers both.
    sidebar_->Apply(SidebarJoinGroup{row.key, action.target.id});
  } else if (action.is("tab.moveNewGroup") || action.is("tab.newGroup")) {
    sidebar_->Apply(SidebarJoinGroup{row.key, std::string()});
  } else if (action.is("tab.reversePanes")) {
    sidebar_->Apply(SidebarSwapPanes{row.key});
  } else if (action.is("tab.pin")) {
    // Pinning MOVES the tab (or the pair) into a cell at the grid's end.
    SidebarPlace grid;
    grid.section = SidebarPlace::Section::kPinned;
    sidebar_->Apply(SidebarMove{row.key, grid});
  } else if (action.is("tab.bookmark")) {
    // The menu's bookmark is an unbound copy (the drag path binds).
    sidebar_->Apply(SidebarBookmarkCopy{row.key});
  } else if (action.is("tab.duplicate") && framework_) {
    // A split duplicates as a fresh split (both panes), as on Mac — in the
    // same arrangement as the one it came from. Ids all the way: OpenTab
    // answers with the stable id, and the split is created in its axis
    // rather than re-oriented after the fact.
    const bool stacked = row.merged && SplitIsStacked(row.id);
    const TabId fresh_a =
        row.url.empty() ? TabId() : framework_->OpenTab(browser, row.url.c_str());
    if (row.merged && fresh_a.valid() && !row.url_b.empty()) {
      const TabId fresh_b = framework_->OpenTab(browser, row.url_b.c_str());
      framework_->ActivateTabById(browser.value, fresh_a.value);
      if (fresh_b.valid()) {
        framework_->CreateSplitViewForTabs(browser.value, fresh_a.value,
                                           fresh_b.value,
                                           stacked ? "horizontal" : "vertical");
      }
    }
  } else if (action.is("tab.copyUrl") || action.is("tab.copyLeft")) {
    CopyTextToClipboard(shell_hwnd_, base::UTF8ToWide(row.url));
  } else if (action.is("tab.copyRight")) {
    CopyTextToClipboard(shell_hwnd_, base::UTF8ToWide(row.url_b));
  } else if (action.is("tab.splitCurrent")) {
    sidebar_->Apply(SidebarFormSplit{SidebarKey::Tab(opening_active_tab),
                                     row.key, "vertical"});
  } else if (action.is("tab.openAsSplit") && framework_) {
    // A fresh blank pane beside this tab, the Mac's openAsSplit. The row
    // becomes the active pane first; everything is by stable id, so the new
    // tab's insertion shifting the strip changes nothing here.
    const TabId fresh = framework_->OpenTab(browser, "");
    if (fresh.valid()) {
      framework_->ActivateTabById(browser.value, row.id.value);
      sidebar_->SplitTabsNormalized(browser, row.id, fresh);
    }
  } else if (action.is("tab.takeControl") && services_.agent) {
    services_.agent->RequestReclaimTab(row.id.value);
  } else if (action.is("tab.handBack") && services_.agent) {
    services_.agent->RequestReleaseTab(row.id.value);
  } else if (action.is("tab.convertSplit")) {
    // Toggle the divider axis; the stored cell a pane is bound to remembers
    // it so the split reopens the same way.
    sidebar_->Apply(SidebarSetLayout{row.key,
                           SplitIsStacked(row.id) ? "vertical" : "horizontal"});
  } else if (action.is("tab.removeSplit")) {
    sidebar_->Apply(SidebarBreakSplit{row.key});
  } else if (action.is("tab.leaveGroup")) {
    sidebar_->Apply(SidebarLeaveGroup{row.key});
  } else if (action.is("tab.close")) {
    sidebar_->Apply(SidebarClose{row.key, -1});
  } else if (action.is("tab.moveToNewWindow") && framework_ &&
             browser.valid() && row.id.valid()) {
    // Chrome's "Move tab to new window": the drag-out gesture's end state
    // without the drag — the row (a split's pair together) detaches into a
    // new window of the same profile. A stored row's tab is unbound first
    // so its cell stays behind as a placeholder, as the tear-off does.
    if (sidebar_) {
      sidebar_->UnbindStoredTab(browser, row.id);
      if (row.b_id.valid()) {
        sidebar_->UnbindStoredTab(browser, row.b_id);
      }
    }
    const TabId ids[] = {row.id, row.b_id};
    framework_->MoveTabsToNewWindowByIds(browser, ids,
                                         row.b_id.valid() ? 2 : 1);
  } else if (action.is("tab.closeOthers") && framework_ && model_) {
    // Every UNBOUND tab except this row (both panes of a merged one);
    // pin/bookmark-bound tabs live in their cells and stay. Closed by stable
    // id, so the ids are collected first and the shifting strip is nobody's
    // problem.
    std::vector<TabId> doomed;
    if (const WindowSnapshot* snapshot = model_->Get(browser)) {
      for (const TabSnapshot& tab : snapshot->tabs) {
        if (tab.id == row.id || tab.id == row.b_id || !tab.tag.empty()) {
          continue;
        }
        doomed.push_back(tab.id);
      }
    }
    for (TabId id : doomed) {
      framework_->CloseTabById(browser.value, id.value);
    }
  }
}

void PhiShellWindow::ShowPinnedCellMenu(POINT pt, const MenuRow& row) {
  // The Mac pinned-cell menu: open, duplicate, copy, rename, unpin, and
  // close (the bound live tab or pair) when open.
  const BrowserId opening_browser = active_browser_id();
  PhiPinnedMenuInputs in;
  in.split_cell = !row.url_b.empty();
  in.horizontal = row.layout == "horizontal";
  in.open = row.open;
  const PhiMenuAction action =
      PhiRunPopupMenu(PhiBuildPinnedCellMenu(in),
                      PopupAt(pt, TPM_LEFTALIGN | TPM_TOPALIGN |
                                      TPM_RIGHTBUTTON));
  if (!action.valid()) {
    return;
  }
  const BrowserId browser = StillHeld(opening_browser);
  if (!browser.valid()) {
    return;  // The slot lost the browser under the menu.
  }
  if (action.is("pin.open") && !row.url.empty()) {
    OpenOrFocusPinned(row.guid, row.url);
  } else if (action.is("pin.duplicate") && framework_ && !row.url.empty()) {
    // A fresh unbound copy — for a split cell, a fresh split, in the axis
    // the cell remembers.
    const TabId fresh_a = framework_->OpenTab(browser, row.url.c_str());
    if (in.split_cell && fresh_a.valid() && !row.url_b.empty()) {
      const TabId fresh_b = framework_->OpenTab(browser, row.url_b.c_str());
      framework_->ActivateTabById(browser.value, fresh_a.value);
      if (fresh_b.valid()) {
        framework_->CreateSplitViewForTabs(
            browser.value, fresh_a.value, fresh_b.value,
            in.horizontal ? "horizontal" : "vertical");
      }
    }
  } else if (action.is("pin.copyUrl") || action.is("pin.copyLeft")) {
    CopyTextToClipboard(shell_hwnd_, base::UTF8ToWide(row.url));
  } else if (action.is("pin.copyRight")) {
    CopyTextToClipboard(shell_hwnd_, base::UTF8ToWide(row.url_b));
  } else if (action.is("pin.rename") && services_.sidebar) {
    std::wstring name = row.title;
    if (PhiPromptText(shell_hwnd_, PhiStr("sidebar.renamePinnedTab"),
                      PhiStr("sidebar.enterANewNameFor"), &name,
                      /*confirm_label=*/nullptr, kPhiNameRules)) {
      services_.sidebar->RenameStoredRow(row.guid, base::WideToUTF8(name));
    }
  } else if (action.is("pin.convertSplit")) {
    // Flip what the cell REMEMBERS, and the live split too when it is
    // currently open — the Mac's convertClosedPinnedSplitLayout.
    sidebar_->Apply(SidebarSetLayout{SidebarKey::Cell(row.guid),
                           in.horizontal ? "vertical" : "horizontal"});
  } else if (action.is("pin.editUrl") && services_.sidebar &&
             !row.guid.empty()) {
    std::wstring edited = base::UTF8ToWide(row.url);
    if (PhiPromptText(shell_hwnd_, PhiStr("common.pinnedTabEditor.editTitle"),
                      PhiStr("sidebar.address"), &edited) &&
        !edited.empty()) {
      services_.sidebar->SetStoredRowUrl(row.guid, base::WideToUTF8(edited));
    }
  } else if (action.is("pin.unpin") && !row.guid.empty()) {
    // The cell goes; a live bound tab returns to the tab list as a normal
    // tab (the tags are cleared).
    sidebar_->Apply(SidebarRemoveCell{SidebarKey::Cell(row.guid)});
  } else if (action.is("pin.close")) {
    // Close the bound live tab(s); the pinned cell stays, as on Mac.
    sidebar_->Apply(SidebarClose{SidebarKey::Cell(row.guid), -1});
  }
}

void PhiShellWindow::ShowBookmarkRowMenu(POINT pt, const MenuRow& row) {
  // The Mac bookmark-row menu: open, open as split, copy, rename, delete
  // (folders keep rename / new-nested-folder / delete).
  const BrowserId opening_browser = active_browser_id();
  PhiBookmarkMenuInputs in;
  in.folder = row.folder;
  in.split_row = !row.url_b.empty();
  in.horizontal = row.layout == "horizontal";
  if (!row.folder && !incognito_) {
    // A bookmark belongs to a Space's stored rows; an Incognito Space keeps
    // none.
    in.spaces = SpaceMenuChoices(/*with_incognito=*/false);
  }
  const PhiMenuAction action =
      PhiRunPopupMenu(PhiBuildBookmarkRowMenu(in),
                      PopupAt(pt, TPM_LEFTALIGN | TPM_TOPALIGN |
                                      TPM_RIGHTBUTTON));
  if (!action.valid()) {
    return;
  }
  const BrowserId browser = StillHeld(opening_browser);
  if (!browser.valid()) {
    return;  // The slot lost the browser under the menu.
  }
  if (action.is("bm.moveToSpace")) {
    sidebar_->Apply(SidebarMoveToSpace{SidebarKey::Cell(row.guid),
                                       action.target.id, /*clone=*/false});
  } else if (action.is("bm.cloneToSpace")) {
    sidebar_->Apply(SidebarMoveToSpace{SidebarKey::Cell(row.guid),
                                       action.target.id, /*clone=*/true});
  } else if (action.is("bm.editUrl") && services_.sidebar) {
    std::wstring edited = base::UTF8ToWide(row.url);
    if (PhiPromptText(shell_hwnd_, PhiStr("common.bookmarkEditor.editTitle"),
                      PhiStr("sidebar.address"), &edited) &&
        !edited.empty()) {
      services_.sidebar->SetStoredRowUrl(row.guid, base::WideToUTF8(edited));
    }
  } else if (action.is("bm.open") && !row.url.empty()) {
    OpenOrFocusPinned(row.guid, row.url);
  } else if (action.is("bm.openSplit") && framework_ && !row.url.empty()) {
    if (in.split_row) {
      // A split bookmark opens as its own pair either way.
      OpenOrFocusPinned(row.guid, row.url);
    } else {
      // Pair the bookmark with the currently focused tab (the Mac's
      // openAsSplit on a bookmark row → formSplitFromBookmark). The partner
      // is the MODEL's active tab, not a paint row's: a focused tab bound to a
      // pinned cell or another bookmark is not a row there, and skipping it
      // would silently do nothing. It is unbound with the new pane below, so
      // the pair lands in the normal tab list.
      const TabSnapshot* active = model_ ? model_->ActiveTab(browser) : nullptr;
      const TabId partner_id =
          active && active->split_id.empty() ? active->id : TabId();
      const TabId fresh = framework_->OpenTab(browser, row.url.c_str());
      if (partner_id.valid() && fresh.valid()) {
        framework_->ActivateTabById(browser.value, partner_id.value);
        sidebar_->SplitTabsNormalized(browser, partner_id, fresh);
      }
    }
  } else if (action.is("bm.copyUrl") || action.is("bm.copyLeft")) {
    CopyTextToClipboard(shell_hwnd_, base::UTF8ToWide(row.url));
  } else if (action.is("bm.copyRight")) {
    CopyTextToClipboard(shell_hwnd_, base::UTF8ToWide(row.url_b));
  } else if (action.is("bm.rename") && services_.sidebar) {
    std::wstring name = row.title;
    if (PhiPromptText(shell_hwnd_,
                      PhiStr(row.folder ? "bookmarkManager.renameFolder"
                                        : "bookmarkManager.renameBookmark"),
                      PhiStr(row.folder ? "bookmarkManager.enterANewNameFor2"
                                        : "bookmarkManager.enterANewNameFor"),
                      &name, /*confirm_label=*/nullptr, kPhiNameRules)) {
      services_.sidebar->RenameStoredRow(row.guid, base::WideToUTF8(name));
    }
  } else if (action.is("bm.convertSplit")) {
    sidebar_->Apply(SidebarSetLayout{SidebarKey::Cell(row.guid),
                           in.horizontal ? "vertical" : "horizontal"});
  } else if (action.is("bm.changeIcon") && row.folder && services_.sidebar) {
    // The picker hangs off the row, as the Mac's popover does.
    const RECT anchor = LayoutToScreen(row.bounds);
    std::string picked;
    {
      PhiModalScope modal(shell_hwnd_);  // The picker pumps its own loop.
      picked = PhiPickFolderIcon(this, anchor, row.icon);
    }
    if (!picked.empty()) {
      services_.sidebar->SetStoredRowIcon(row.guid, picked);
    }
  } else if (action.is("bm.newFolder") && row.folder && services_.sidebar) {
    std::wstring name = PhiStr("common.bookmarkFolder.defaultName");
    if (PhiPromptText(shell_hwnd_,
                      PhiStr("app.bookmarkFolderDialog.createTitle"),
                      PhiStr("bookmarkManager.folderName"), &name,
                      /*confirm_label=*/nullptr, kPhiNameRules)) {
      // Disclose the parent so the new child is visible where it lands.
      sidebar_->Expand(row.guid);
      services_.sidebar->AddBookmarkFolder(slot_.value, row.guid,
                                           base::WideToUTF8(name));
    }
  } else if (action.is("bm.remove") && !row.guid.empty()) {
    // A folder takes its subtree.
    sidebar_->Apply(SidebarRemoveCell{row.folder ? SidebarKey::Folder(row.guid)
                                       : SidebarKey::Cell(row.guid)});
  }
}

bool PhiShellWindow::ShowBookmarkBarContextMenu(POINT pt) {
  const Layout layout = ComputeLayout();
  if (!::PtInRect(&layout.bookmark_bar, pt)) {
    return false;
  }
  if (AgentHoldsInput()) {
    return true;  // Refused like the rows' menus while the agent drives.
  }
  for (const BarItem& item : bar_items_) {
    if (!::PtInRect(&item.bounds, pt)) {
      continue;
    }
    if (item.overflow || item.bookmark < 0 ||
        item.bookmark >= static_cast<int>(sidebar_->bookmarks().size())) {
      return true;  // The overflow button has no menu of its own.
    }
    // The same menu as the outline's row for this bookmark (the Mac's
    // makeContextMenu(source: .bookmarkBar)); the icon picker hangs off the
    // pill rather than the row.
    const ShortcutRow& bookmark =
        sidebar_->bookmarks()[static_cast<size_t>(item.bookmark)];
    MenuRow snapshot;
    snapshot.key = bookmark.key;
    snapshot.guid = bookmark.guid;
    snapshot.url = bookmark.url;
    snapshot.url_b = bookmark.url_b;
    snapshot.title = bookmark.title;
    snapshot.layout = bookmark.layout;
    snapshot.icon = bookmark.icon;
    snapshot.folder = bookmark.folder;
    snapshot.bounds = item.bounds;
    ShowBookmarkRowMenu(pt, snapshot);
    return true;
  }
  // The bar's bare run: the Mac BookmarkBar's own menu, New Folder only.
  const PhiMenuAction action =
      PhiRunPopupMenu(PhiBuildBookmarkBarAreaMenu(ShowsIncognito()),
                      PopupAt(pt, TPM_LEFTALIGN | TPM_TOPALIGN |
                                      TPM_RIGHTBUTTON));
  if (action.is("bar.newRootFolder") && services_.sidebar) {
    std::wstring name = PhiStr("common.bookmarkFolder.defaultName");
    if (PhiPromptText(shell_hwnd_,
                      PhiStr("app.bookmarkFolderDialog.createTitle"),
                      PhiStr("bookmarkManager.folderName"), &name,
                      /*confirm_label=*/nullptr, kPhiNameRules)) {
      services_.sidebar->AddBookmarkFolder(slot_.value, std::string(),
                                           base::WideToUTF8(name));
    }
  }
  return true;
}

void PhiShellWindow::ShowTabAreaMenu(POINT pt,
                                     bool space_only,
                                     std::string space_id) {
  // The Mac tab-area menu (TabAreaContextMenuHelper): new tab, reopen, the
  // bookmark block, the layout switch, then the active Space's own controls
  // (the Spaces menu's rows, for the Space under the pointer).
  if (!services_.spaces) {
    return;
  }
  // The Space the block acts on: the pip's when one was right-clicked
  // (its switch may not have landed), else the one on screen.
  const std::string space =
      space_id.empty() ? std::string(active_space()) : space_id;
  PhiTabAreaMenuInputs in;
  in.incognito = incognito_ || model::IsIncognitoSpaceId(space);
  in.incognito_space = !incognito_ && model::IsIncognitoSpaceId(space);
  in.tabs_empty = sidebar_->tabs().empty();
  in.mode = layout_mode_;
  in.space_only = space_only;
  if (!incognito_) {
    in.can_delete_space = services_.spaces->CanDeleteSpace(space);
    for (const PhiSpaceView& view : services_.spaces->Spaces()) {
      if (view.id == space) {
        in.active_profile = view.profile_id;
        break;
      }
    }
    // The default Space's window cannot be re-bound (a fresh shell's first
    // browser lands there), so its rows read the binding out but stay
    // disabled.
    in.can_reprofile = space != services_.spaces->DefaultSpaceId() &&
                       !in.incognito_space;
    for (const std::string& summary : services_.spaces->ProfileSummaries()) {
      const size_t tab = summary.find('\t');
      const std::string id = summary.substr(0, tab);
      if (id.empty()) {
        continue;
      }
      PhiMenuChoice choice;
      choice.id = id;
      choice.label = base::UTF8ToWide(
          tab == std::string::npos ? id : summary.substr(tab + 1));
      in.profiles.push_back(std::move(choice));
    }
  }
  const PhiPopupMenuRequest request =
      PopupAt(pt, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON);
  const PhiMenuAction action =
      PhiRunPopupMenu(PhiBuildTabAreaMenu(in), request);
  if (action.is("area.setTheme")) {
    // Anchored on the click, like the Mac's .nearTrigger placement.
    const RECT anchor = {request.x, request.y, request.x, request.y};
    PhiShowSpaceThemePanel(this, anchor, space);
  } else if (action.is("area.profile")) {
    // Confirmed like the Mac's: the Space's window is replaced and its tabs
    // reload in the new profile, where the old profile's logins do not
    // exist.
    if (PhiConfirm(shell_hwnd_,
                   PhiStrF("app.changeSpaceProfileConfirmation.title",
                           {action.target.text})
                       .c_str(),
                   PhiStr("app.changeSpaceProfileConfirmation."
                          "profileScopedMessage"))) {
      services_.spaces->SetSpaceProfile(space, action.target.id);
    }
  } else if (action.is("area.newTab")) {
    StartNewTab();
  } else if (action.is("area.newRootFolder")) {
    std::wstring name = PhiStr("common.bookmarkFolder.defaultName");
    if (PhiPromptText(shell_hwnd_,
                      PhiStr("app.bookmarkFolderDialog.createTitle"),
                      PhiStr("bookmarkManager.folderName"), &name,
                      /*confirm_label=*/nullptr, kPhiNameRules)) {
      services_.sidebar->AddBookmarkFolder(slot_.value, std::string(),
                               base::WideToUTF8(name));
    }
  } else if (action.is("area.layout")) {
    services_.prefs->RequestLayoutMode(action.target.id == "comfortable"
                                           ? PhiLayoutMode::kComfortable
                                           : PhiLayoutMode::kBalanced);
  } else if (PhiAppMenu* bar = app_menu(); bar && action.valid()) {
    // The rest are the app menu's own commands, run through it so the
    // sidebar and the menu cannot drift apart.
    if (action.is("area.reopen")) {
      bar->OnCommand(kCmdReopenClosedTab);
    } else if (action.is("area.bookmarkAll")) {
      bar->OnCommand(kCmdBookmarkAllTabs);
    } else if (action.is("area.newSpace")) {
      bar->OnCommand(kCmdNewSpace);
    } else if (action.is("area.renameSpace")) {
      bar->RenameSpace(space);
    } else if (action.is("area.changeIcon")) {
      // The §5.9 flyout, hung off the point the menu opened at (the pip
      // or the chip under the pointer).
      const POINT screen = LayoutToScreen(pt);
      ShowSpaceIconPicker(space,
                          {screen.x, screen.y, screen.x + 1, screen.y + 1});
    } else if (action.is("area.deleteSpace")) {
      bar->DeleteSpace(space);
    } else if (action.is("area.closeIncognitoSpace")) {
      CloseIncognitoSpaceAsking(space);
    }
  }
}

void PhiShellWindow::ShowAgentChipMenu(POINT pt, std::string task) {
  // The agent pip's own menu (the Mac agentContextMenu): the transcript
  // console and finishing the task. The Space menu below would misapply
  // rename/theme/delete to a task id.
  const PhiMenuAction action = PhiRunPopupMenu(
      PhiBuildAgentChipMenu(), PopupAt(pt, TPM_LEFTALIGN | TPM_TOPALIGN));
  if (!services_.agent) {
    return;
  }
  if (action.is("agent.transcript")) {
    services_.agent->RequestShowTranscript(task);
  } else if (action.is("agent.finish")) {
    services_.agent->RequestAgentFinish(task);
    ScheduleRefresh();  // The pip leaves the strip with the task.
  }
}

void PhiShellWindow::ShowProfileMenu() {
  // The Mac ProfileButton's showProfileMenu: a plain menu (no Space rows,
  // no bitmaps) dropped UPWARD off the bottom bar — its bottom edge 5 dip
  // above the button, left edges aligned. The window comes forward first
  // (the Mac's makeKeyAndOrderFront): the rows run this window's commands.
  const Layout layout = ComputeLayout();
  const RECT& button = layout.sidebar_profile;
  if (button.right <= button.left) {
    return;
  }
  if (shell_hwnd_ && ::GetForegroundWindow() != shell_hwnd_) {
    ::SetForegroundWindow(shell_hwnd_);
  }
  // The chords are the user's effective ones, by the ⋯ menu's rule, so the
  // two menus can never disagree about a rebound command.
  ChromePrefsHost* prefs = services_.prefs;
  PhiProfileMenuInputs in;
  in.downloads_chord = MenuChord(prefs, kCmdDownloads, "appMenu.downloads");
  in.bookmarks_chord =
      MenuChord(prefs, kCmdBookmarkManager, "appMenu.bookmarkManager");
  in.extensions_chord =
      MenuChord(prefs, kCmdManageExtensions, "appMenu.manageExtensions");
  in.new_tab_chord = MenuChord(prefs, kCmdNewTab, "appMenu.newTab");
  in.new_incognito_space_chord =
      MenuChord(prefs, kCmdNewIncognitoSpace, "appMenu.newIncognitoSpace");
  in.new_incognito_window_chord =
      MenuChord(prefs, kCmdNewIncognitoWindow, "appMenu.newIncognitoWindow");
  in.settings_chord = MenuChord(prefs, kCmdPreferences, "appMenu.settings");
  in.signed_in =
      services_.window && services_.window->AccountInfo(nullptr, nullptr);
  const PhiMenuAction action = PhiRunPopupMenu(
      PhiBuildProfileMenu(in),
      PopupAt(POINT{button.left, button.top - Scaled(5)},
              TPM_LEFTALIGN | TPM_BOTTOMALIGN));
  if (action.is("profile.downloads")) {
    // "Download List" is the Library's Downloads section (the Mac's
    // showLibrary(section: .downloads)), not the bar button's popover.
    PhiLibrary::Open(this, PhiLibrarySection::kDownloads);
  } else if (action.is("profile.signIn")) {
    if (services_.window) {
      services_.window->RequestSignIn();
    }
  } else if (PhiAppMenu* bar = app_menu(); bar && action.valid()) {
    // The rest are the ⋯ menu's own commands, run through it so the two
    // cannot drift apart.
    if (action.is("profile.bookmarks")) {
      bar->OnCommand(kCmdBookmarkManager);
    } else if (action.is("profile.extensions")) {
      bar->OnCommand(kCmdManageExtensions);
    } else if (action.is("profile.newTab")) {
      bar->OnCommand(kCmdNewTab);
    } else if (action.is("profile.newIncognitoSpace")) {
      bar->OnCommand(kCmdNewIncognitoSpace);
    } else if (action.is("profile.newIncognitoWindow")) {
      bar->OnCommand(kCmdNewIncognitoWindow);
    } else if (action.is("profile.settings")) {
      bar->OnCommand(kCmdPreferences);
    }
  }
}

void PhiShellWindow::ShowSpaceSwitcherMenu(POINT pt, bool overflow) {
  // The full Space list as a popup, then "New Space…": comfortable's
  // compact pip opens it over every Space (the Mac's spaceSwitcherMenu on
  // its compact chip), and the overflow "…" adds the new-profile row too
  // (the moreButton's menu).
  if (!services_.spaces) {
    return;
  }
  const std::vector<PhiMenuChoice> spaces = SpaceSwitcherChoices();
  PhiPopupMenuRequest request = PopupAt(pt, TPM_LEFTALIGN | TPM_TOPALIGN);
  // Each row wears its Space's icon as a bitmap — the strip's artwork or
  // colour emoji, which menu text cannot render.
  request.glyph_px = Scaled(16);
  std::vector<PhiMenuChoice> agents;
  std::string watched_agent;
  if (services_.agent && !agent_) {
    // This window's own list: an agent Space on screen in another window
    // is not offered here (PhiAgentSpaceManager::Presents).
    for (const PhiAgentPipView& pip :
         services_.agent->AgentSpaces(slot_.value)) {
      PhiMenuChoice choice;
      choice.id = pip.task_id;
      choice.label = MenuLabel(PhiAgentPipLabel(pip));
      choice.glyph = pip.icon;
      // The status word leads the agent row's hint (§5.3), then the
      // profile and the tab count like a user Space's.
      const wchar_t* status = L"Running";
      switch (pip.badge) {
        case PhiAgentPipView::Badge::kRunning:
          status = L"Running";
          break;
        case PhiAgentPipView::Badge::kIdle:
          status = L"Idle";
          break;
        case PhiAgentPipView::Badge::kUserHolds:
          status = L"You have control";
          break;
        case PhiAgentPipView::Badge::kCompleted:
          status = L"Done";
          break;
        case PhiAgentPipView::Badge::kError:
          status = L"Error";
          break;
      }
      const int tabs = SpacesTabCount(pip.space_id);
      choice.hint = std::wstring(status) + L"  \u00B7  " +
                    base::UTF8ToWide(pip.profile_id) + L"  \u00B7  " +
                    std::to_wstring(tabs) + (tabs == 1 ? L" tab" : L" tabs");
      const int position = static_cast<int>(spaces.size()) +
                           static_cast<int>(agents.size()) + 1;
      if (position <= 9) {
        choice.shortcut = L"Ctrl+" + std::to_wstring(position);
      }
      if (pip.watched) {
        watched_agent = pip.task_id;
      }
      agents.push_back(std::move(choice));
    }
  }
  const PhiMenuAction action = PhiRunPopupMenu(
      PhiBuildSpaceSwitcherMenu(spaces, agents, watched_agent, active_space(),
                                /*with_new_space=*/true,
                                /*with_new_profile=*/overflow),
      request);
  if (action.is("agent.watch")) {
    if (services_.agent) {
      services_.agent->RequestWatchAgentSpace(action.target.id, slot_.value);
    }
  } else if (action.is("space.new")) {
    ShowCreateSpaceForm();
  } else if (action.is("space.newWithProfile")) {
    services_.spaces->RequestCreateSpace(slot_.value, /*new_profile=*/true);
  } else if (action.is("space.switch")) {
    SwitchToSpace(action.target.id);
  }
}

void PhiShellWindow::OnChromeContextMenu(int x, int y) {
  if (!services_.sidebar || !shell_hwnd_) {
    return;
  }
  DismissChromeTip();
  CloseExtensionPopup();
  const POINT pt = {x, y};
  const RECT toggle_rc = ComputeLayout().sidebar_toggle;
  if (::PtInRect(&toggle_rc, pt)) {
    // The toggle's own menu (impl log D4): Show / Hide sidebar.
    PhiMenuSpec spec;
    PhiMenuItem show = PhiMenuItem::Command("sidebar.show", PhiStr("appMenu.showSidebar"));
    show.checked = !sidebar_collapsed_;
    spec.items.push_back(std::move(show));
    PhiMenuItem hide = PhiMenuItem::Command("sidebar.hide", PhiStr("sidebar.hide"));
    hide.checked = sidebar_collapsed_;
    spec.items.push_back(std::move(hide));
    const PhiMenuAction action = PhiRunPopupMenu(
        spec, PopupAt(pt, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON));
    if (action.is("sidebar.show")) {
      SetSidebarCollapsed(false);
    } else if (action.is("sidebar.hide")) {
      SetSidebarCollapsed(true);
    }
    return;
  }
  // The bookmark bar is carved off the content, not the band: its pills
  // answer whatever the sidebar's collapse state, so they go first.
  if (ShowBookmarkBarContextMenu(pt)) {
    return;
  }
  // A pinned extension's in-field icon: its action's context menu at the
  // icon's bottom-left corner — the Mac's secondaryAction on the button.
  {
    const Layout field_layout = ComputeLayout();
    for (size_t i = 0; i < field_extensions_.size(); ++i) {
      const RECT slot = FieldExtRect(field_layout, i);
      if (slot.right > slot.left && ::PtInRect(&slot, pt)) {
        ShowFieldExtensionMenu(i, slot);
        return;
      }
    }
  }
  if (!BandOnShell()) {
    return;  // Collapsed: every row here is the floating panel's.
  }
  // The chips and rows are hit-tested as the last frame showed them; what a
  // hit produces is a SNAPSHOT, copied here, and every menu below runs from
  // that copy alone.
  if (const int hit = spaces_->ChipAt(pt); hit >= 0) {
    const PhiSpaceChip& chip = spaces_->chips()[static_cast<size_t>(hit)];
    if (chip.is_agent) {
      ShowAgentChipMenu(pt, chip.task_id);
      return;
    }
    // A user Space pip (or the Space chip): the Space block — rename, icon,
    // theme, profile, delete — for THAT Space (§5.4). A pip of another
    // Space switches there first, as the Mac's does; the menu is bound to
    // the clicked id, not to active_space(), because a cold Space's switch
    // lands later and the menu would act on the Space still shown. Copied:
    // the switch rebuilds chips_, and `chip` with it.
    const std::string space_id = chip.id;
    if (space_id != active_space()) {
      SwitchToSpace(space_id);
    }
    if (!incognito_) {
      ShowTabAreaMenu(pt, /*space_only=*/true, space_id);
    }
    return;
  }
  // Not a chip: the row context menus. Tab rows work in both orientations
  // (the strip's horizontal tabs share sidebar_->tabs()). None of them
  // while the agent holds the window — the Mac's sidebar refuses its whole
  // tab-row menu then; the pill is the one way to take over.
  if (AgentHoldsInput()) {
    return;
  }
  for (const GroupHeader& gh : sidebar_->group_headers()) {
    if (::PtInRect(&gh.bounds, pt)) {
      ShowGroupHeaderMenu(pt, gh.id, gh.name, gh.color);
      return;
    }
  }
  for (const TabRow& row : sidebar_->tabs()) {
    if (!::PtInRect(&row.bounds, pt)) {
      continue;
    }
    MenuRow snapshot;
    snapshot.key = row.key;
    snapshot.id = row.id;
    snapshot.merged = row.b_index >= 0;
    snapshot.b_id = snapshot.merged ? row.b_id : TabId();
    snapshot.title = row.title;
    snapshot.url = base::WideToUTF8(row.url);
    snapshot.url_b =
        snapshot.merged ? base::WideToUTF8(row.b_url) : std::string();
    snapshot.split_id = row.split_id;
    snapshot.group_id = row.group_id;
    ShowTabRowMenu(pt, snapshot);
    return;
  }
  for (const ShortcutRow& pin : sidebar_->pinned()) {
    if (!::PtInRect(&pin.bounds, pt)) {
      continue;
    }
    MenuRow snapshot;
    snapshot.key = pin.key;
    snapshot.guid = pin.guid;
    snapshot.url = pin.url;
    snapshot.url_b = pin.url_b;
    snapshot.title = pin.title;
    snapshot.layout = pin.layout;
    snapshot.open = pin.open;
    ShowPinnedCellMenu(pt, snapshot);
    return;
  }
  for (const ShortcutRow& bookmark : sidebar_->bookmarks()) {
    if (!::PtInRect(&bookmark.bounds, pt)) {
      continue;
    }
    MenuRow snapshot;
    snapshot.key = bookmark.key;
    snapshot.guid = bookmark.guid;
    snapshot.url = bookmark.url;
    snapshot.url_b = bookmark.url_b;
    snapshot.title = bookmark.title;
    snapshot.layout = bookmark.layout;
    snapshot.icon = bookmark.icon;
    snapshot.folder = bookmark.folder;
    snapshot.bounds = bookmark.bounds;
    ShowBookmarkRowMenu(pt, snapshot);
    return;
  }
  // The tab area's background — the Mac tab-area menu's bookmark block
  // (TabAreaContextMenuHelper): root folder creation lives here. Both
  // orientations share it, as the Mac's two hosts share one
  // TabAreaContextMenuHelper: the sidebar below the band in the vertical
  // layouts, and in comfortable the caption band itself, whose bare run past
  // the last tab IS that layout's empty tab-strip space
  // (TabStripBarController hangs the same stripContextMenu off the bar).
  const Layout layout = ComputeLayout();
  const bool in_tab_area =
      layout.horizontal_tabs
          ? ::PtInRect(&layout.title_bar, pt) != 0
          : (pt.x < layout.sidebar.right && pt.y > layout.title_bar.bottom);
  if (in_tab_area) {
    ShowTabAreaMenu(pt);
  }
}

// --- The floating sidebar -------------------------------------------------
// The Mac panel (WebContentContainerViewController+FloatingSidebar): with
// the sidebar collapsed, hovering the window's left margin slides in a
// popup showing the real sidebar. It renders by running PaintChrome
// against the EXPANDED layout (floating_layout_) into a back buffer and
// blitting the sidebar band; its mouse events forward through the normal
// handlers with the band's y offset, so chips, tabs, pins, bookmarks and
// menus all behave as in the docked sidebar.

RECT PhiShellWindow::FloatingPanelRect(bool expanded) const {
  // x 8, y 52 (below the 44 address row plus 8), the docked width, bottom
  // at h − 8; in content fullscreen there is no address row, so y 8. The
  // hidden endpoint is entirely outside the client area; the window is
  // also hidden when the slide lands, removing its shadow.
  RECT client = {};
  ::GetClientRect(shell_hwnd_, &client);
  POINT origin = {0, 0};
  ::ClientToScreen(shell_hwnd_, &origin);
  const int inset = Scaled(kEdgeInset);
  const int row =
      browsers_->content_fullscreen() ? 0 : Scaled(kAddressRowHeight);
  const int w = Scaled(sidebar_dip_);
  const int x = origin.x + (expanded ? inset : -w);
  const int top = origin.y + row + inset;
  return {x, top, x + w, origin.y + client.bottom - inset};
}

void PhiShellWindow::EnsureFloatingPanel() {
  if (floating_sidebar_ || !sidebar_collapsed_ || !shell_hwnd_ ||
      layout_mode_ == PhiLayoutMode::kComfortable) {
    return;
  }
  const RECT rc = FloatingPanelRect(/*expanded=*/false);
  if (rc.bottom - rc.top <= 0 || rc.right - rc.left <= 0) {
    return;
  }
  static bool registered = false;
  if (!registered) {
    registered = true;
    WNDCLASSW cls = {};
    // OS popup shadows are not clipped by SetWindowRgn and can leave an
    // edge outside the browser as the panel slides through its left side.
    cls.lpfnWndProc = &PhiShellWindow::FloatingSidebarProc;
    cls.hInstance = ::GetModuleHandleW(nullptr);
    cls.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = L"PhiFloatingSidebar";
    ::RegisterClassW(&cls);
  }
  floating_expanded_ = false;
  floating_offset_y_ = 0;
  floating_panel_h_ = rc.bottom - rc.top;
  floating_hover_tick_ = 0;
  floating_outside_tick_ = 0;
  floating_slide_start_ = 0;
  floating_sidebar_ = ::CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"PhiFloatingSidebar", L"",
      WS_POPUP, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
      shell_hwnd_, nullptr, ::GetModuleHandleW(nullptr), nullptr);
  if (!floating_sidebar_) {
    return;
  }
  ::SetWindowLongPtrW(floating_sidebar_, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(this));
  // The rows move into the panel's geometry NOW: a viewport change, like a
  // layout mode change, not a paint-time rebuild.
  rows_dirty_ = true;
  RebuildTabs();
  const DWMNCRENDERINGPOLICY nonclient = DWMNCRP_DISABLED;
  ::DwmSetWindowAttribute(floating_sidebar_, DWMWA_NCRENDERING_POLICY,
                          &nonclient, sizeof(nonclient));
  ClipFloatingPanel();
  // Keep the popup hidden until an explicit reveal. Its timer observes an
  // invisible edge hover zone even while the native window is hidden.
  ::SetTimer(floating_sidebar_, kFloatingTickTimer, 30, nullptr);
}

void PhiShellWindow::DestroyFloatingPanel() {
  if (!floating_sidebar_) {
    return;
  }
  if (floating_drag_) {
    sidebar_->AbortDrag();  // Its capture and timers live on the panel.
    floating_drag_ = false;
    if (floating_forward_depth_ == 0) {
      floating_layout_ = false;
    }
  }
  HWND panel = floating_sidebar_;
  floating_sidebar_ = nullptr;
  floating_expanded_ = false;
  floating_panel_h_ = 0;
  ::KillTimer(panel, kFloatingTickTimer);
  ::DestroyWindow(panel);
  ScheduleRefresh();  // Back to the docked geometry on the next turn.
}

void PhiShellWindow::PositionFloatingPanel() {
  if (!floating_sidebar_ || !shell_hwnd_) {
    return;
  }
  if (::IsIconic(shell_hwnd_)) {
    ::ShowWindow(floating_sidebar_, SW_HIDE);
    return;
  }
  const RECT rc = FloatingPanelRect(floating_expanded_);
  const int h = rc.bottom - rc.top;
  if (h <= 0) {
    ::ShowWindow(floating_sidebar_, SW_HIDE);
    return;
  }
  if (h != floating_panel_h_) {
    floating_panel_h_ = h;
    rows_dirty_ = true;  // The bands are laid out for the panel's height.
    RebuildTabs();
  }
  floating_slide_start_ = 0;  // A move lands the slide where it is going.
  ::SetWindowPos(floating_sidebar_, nullptr, rc.left, rc.top, rc.right - rc.left,
                 h, SWP_NOZORDER | SWP_NOACTIVATE |
                        (floating_expanded_ ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
  ClipFloatingPanel();
}

void PhiShellWindow::ClipFloatingPanel() {
  if (!floating_sidebar_ || !shell_hwnd_) {
    return;
  }
  RECT panel_rc = {};
  ::GetWindowRect(floating_sidebar_, &panel_rc);
  RECT client = {};
  ::GetClientRect(shell_hwnd_, &client);
  ::MapWindowPoints(shell_hwnd_, nullptr, reinterpret_cast<POINT*>(&client), 2);
  RECT visible = {};
  ::IntersectRect(&visible, &panel_rc, &client);
  // Keep our rounded region even at rest: native nonclient rendering is
  // disabled so neither a border nor a shadow escapes the owner's bounds.
  // The visible part, in the panel's own coordinates, with the panel's
  // 8 dip corners kept where they are visible.
  ::OffsetRect(&visible, -panel_rc.left, -panel_rc.top);
  const int w = panel_rc.right - panel_rc.left;
  const int h = panel_rc.bottom - panel_rc.top;
  const int d = Scaled(8) * 2;
  HRGN region = ::CreateRoundRectRgn(0, 0, w + 1, h + 1, d, d);
  HRGN box = ::CreateRectRgnIndirect(&visible);
  ::CombineRgn(region, region, box, RGN_AND);
  ::DeleteObject(box);
  ::SetWindowRgn(floating_sidebar_, region, TRUE);  // The system owns it now.
}

void PhiShellWindow::SlideFloatingPanel(bool expanded) {
  if (!floating_sidebar_ || floating_expanded_ == expanded) {
    return;
  }
  floating_expanded_ = expanded;
  RECT now_rc = {};
  ::GetWindowRect(floating_sidebar_, &now_rc);
  floating_slide_from_ = now_rc.left;
  floating_slide_to_ = FloatingPanelRect(expanded).left;
  floating_slide_start_ = ::GetTickCount64();
  if (expanded) {
    floating_shown_tick_ = floating_slide_start_;
    floating_outside_tick_ = 0;
    ::SetWindowPos(floating_sidebar_, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
  floating_hover_tick_ = 0;
}

// --- The split view's hover chip (§4.6) -------------------------------------

namespace {

constexpr int kSplitChipHeight = 28;
constexpr int kSplitChipInset = 8;

std::wstring HostOfUrl(const std::wstring& url) {
  size_t start = url.find(L"://");
  start = start == std::wstring::npos ? 0 : start + 3;
  const size_t end = url.find_first_of(L"/?#", start);
  std::wstring host = url.substr(start, end == std::wstring::npos
                                            ? std::wstring::npos
                                            : end - start);
  if (host.rfind(L"www.", 0) == 0) {
    host.erase(0, 4);
  }
  return host.empty() ? url : host;
}

}  // namespace

void PhiShellWindow::SyncSplitChipTimer() {
  if (!shell_hwnd_) {
    return;
  }
  if (ActiveTabIsSplit()) {
    ::SetTimer(shell_hwnd_, kSplitChipTimer, 150, nullptr);
  } else {
    ::KillTimer(shell_hwnd_, kSplitChipTimer);
    HideSplitChip();
  }
}

const TabRow* PhiShellWindow::SplitRowForTab(int tab_id) const {
  for (const TabRow& row : sidebar_->tabs()) {
    if (row.id.value == tab_id || row.b_id.value == tab_id) {
      return &row;
    }
  }
  return nullptr;
}

void PhiShellWindow::SplitChipTick() {
  if (!shell_hwnd_ || !ActiveTabIsSplit() || floating_expanded_) {
    HideSplitChip();
    return;
  }
  const std::vector<NativePane> panes = NativePagesSplitPanes();
  if (panes.size() != 2 || ::IsRectEmpty(&panes[0].rect) ||
      ::IsRectEmpty(&panes[1].rect)) {
    HideSplitChip();
    return;
  }
  // The UNFOCUSED pane: the one the active tab is not in.
  const int active = active_tab_id().value;
  const NativePane& other = panes[0].tab_id == active ? panes[1] : panes[0];
  // No chip over a surface: a reader over the unfocused pane, or a peek
  // (either pane's covers the whole split) would sit under it.
  if (services_.sidebar) {
    const std::map<int, SidebarSurface> surfaces =
        services_.sidebar->AttachedSurfaces(active_browser_id().value);
    const auto own = surfaces.find(active);
    if (surfaces.count(other.tab_id) ||
        (own != surfaces.end() && own->second == SidebarSurface::kPeek)) {
      HideSplitChip();
      return;
    }
  }
  POINT cursor = {};
  ::GetCursorPos(&cursor);
  RECT pane_screen = other.rect;
  ::MapWindowPoints(shell_hwnd_, nullptr, reinterpret_cast<POINT*>(&pane_screen), 2);
  bool inside = ::PtInRect(&pane_screen, cursor) != FALSE;
  if (!inside && split_chip_ && ::IsWindowVisible(split_chip_)) {
    RECT chip_rc = {};
    ::GetWindowRect(split_chip_, &chip_rc);
    inside = ::PtInRect(&chip_rc, cursor) != FALSE;
  }
  const ULONGLONG now = ::GetTickCount64();
  if (inside) {
    split_chip_leave_tick_ = 0;
    ShowSplitChip(other.tab_id, other.rect);
    return;
  }
  if (!split_chip_tab_) {
    return;
  }
  if (!split_chip_leave_tick_) {
    split_chip_leave_tick_ = now;
  } else if (now - split_chip_leave_tick_ >= 600) {
    HideSplitChip();
  }
}

void PhiShellWindow::ShowSplitChip(int tab_id, const RECT& pane) {
  const TabRow* row = SplitRowForTab(tab_id);
  if (!row) {
    return;
  }
  static bool registered = false;
  if (!registered) {
    registered = true;
    WNDCLASSW cls = {};
    cls.style = CS_DROPSHADOW;
    cls.lpfnWndProc = &PhiShellWindow::SplitChipProc;
    cls.hInstance = ::GetModuleHandleW(nullptr);
    cls.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = L"PhiSplitChip";
    ::RegisterClassW(&cls);
  }
  // 8 · favicon 16 · 6 · host · 6 · ⋯ 20 · 8, 28 high.
  const std::wstring host =
      HostOfUrl(row->b_id.value == tab_id ? row->b_url : row->url);
  int host_w = Scaled(60);
  if (HDC dc = ::GetDC(shell_hwnd_)) {
    HGDIOBJ previous = label_font_ ? ::SelectObject(dc, label_font_) : nullptr;
    SIZE extent = {};
    if (::GetTextExtentPoint32W(dc, host.c_str(), static_cast<int>(host.size()),
                                &extent)) {
      host_w = std::min(static_cast<int>(extent.cx), Scaled(220));
    }
    if (previous) {
      ::SelectObject(dc, previous);
    }
    ::ReleaseDC(shell_hwnd_, dc);
  }
  const int w = Scaled(8 + 16 + 6) + host_w + Scaled(6 + 20 + 8);
  const int h = Scaled(kSplitChipHeight);
  POINT origin = {pane.right - Scaled(kSplitChipInset) - w,
                  pane.top + Scaled(kSplitChipInset)};
  ::ClientToScreen(shell_hwnd_, &origin);
  if (!split_chip_) {
    split_chip_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"PhiSplitChip", L"", WS_POPUP,
        origin.x, origin.y, w, h, shell_hwnd_, nullptr,
        ::GetModuleHandleW(nullptr), nullptr);
    if (!split_chip_) {
      return;
    }
    ::SetWindowLongPtrW(split_chip_, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(this));
    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    ::DwmSetWindowAttribute(split_chip_, DWMWA_WINDOW_CORNER_PREFERENCE,
                            &corner, sizeof(corner));
  }
  const bool changed = split_chip_tab_ != tab_id;
  split_chip_tab_ = tab_id;
  ::SetWindowPos(split_chip_, HWND_TOP, origin.x, origin.y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
  if (changed) {
    ::InvalidateRect(split_chip_, nullptr, FALSE);
  }
}

void PhiShellWindow::HideSplitChip() {
  split_chip_tab_ = 0;
  split_chip_leave_tick_ = 0;
  if (split_chip_ && ::IsWindowVisible(split_chip_)) {
    ::ShowWindow(split_chip_, SW_HIDE);
  }
}

void PhiShellWindow::PaintSplitChip(HDC dc) {
  RECT rc = {};
  ::GetClientRect(split_chip_, &rc);
  const PhiPalette palette = CurrentPalette();
  HBRUSH ground = ::CreateSolidBrush(palette.flyout);
  ::FillRect(dc, &rc, ground);
  ::DeleteObject(ground);
  const TabRow* row = SplitRowForTab(split_chip_tab_);
  if (!row) {
    return;
  }
  const bool b = row->b_id.value == split_chip_tab_;
  const std::vector<unsigned char>& favicon = b ? row->b_favicon : row->favicon;
  const int fav_w = b ? row->b_favicon_width : row->favicon_width;
  const int fav_h = b ? row->b_favicon_height : row->favicon_height;
  const int mid = (rc.top + rc.bottom) / 2;
  const int icon_px = Scaled(16);
  RECT icon = {rc.left + Scaled(8), mid - icon_px / 2,
               rc.left + Scaled(8) + icon_px, mid + icon_px / 2};
  if (!favicon.empty()) {
    sidebar_->DrawFavicon(dc, icon, favicon.data(), fav_w, fav_h,
                          palette.flyout);
  } else {
    PhiDrawIconGlyph(dc, icon, fluent::kGlobe, icon_px, palette.label_dim);
  }
  RECT more = {rc.right - Scaled(8) - Scaled(20), mid - Scaled(10),
               rc.right - Scaled(8), mid + Scaled(10)};
  POINT cursor = {};
  ::GetCursorPos(&cursor);
  ::ScreenToClient(split_chip_, &cursor);
  PhiPaintIconButton(dc, more, fluent::kMore,
                     ::PtInRect(&more, cursor) ? PhiButtonState::kHover
                                               : PhiButtonState::kRest,
                     palette, palette.label, Scaled(12),
                     Scaled(fluent::kButtonRadius));
  ::SetBkMode(dc, TRANSPARENT);
  HGDIOBJ previous = label_font_ ? ::SelectObject(dc, label_font_) : nullptr;
  ::SetTextColor(dc, palette.label_dim);
  RECT text = {icon.right + Scaled(6), rc.top, more.left - Scaled(6), rc.bottom};
  const std::wstring host = HostOfUrl(b ? row->b_url : row->url);
  PhiDrawTextAA(dc, host.c_str(), -1, &text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
  if (previous) {
    ::SelectObject(dc, previous);
  }
  HPEN pen = ::CreatePen(PS_SOLID, 1, palette.border);
  HGDIOBJ old_pen = ::SelectObject(dc, pen);
  HGDIOBJ old_brush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
  ::Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
  ::SelectObject(dc, old_brush);
  ::SelectObject(dc, old_pen);
  ::DeleteObject(pen);
}

void PhiShellWindow::OnSplitChipClick(POINT chip_pt) {
  const int tab = split_chip_tab_;
  const TabRow* found = SplitRowForTab(tab);
  const BrowserId browser = active_browser_id();
  if (!found || !framework_ || !browser.valid()) {
    return;
  }
  const TabRow row = *found;  // A snapshot: the menu loop may rebuild rows.
  RECT rc = {};
  ::GetClientRect(split_chip_, &rc);
  if (chip_pt.x < rc.right - Scaled(8 + 20)) {
    // The chip itself focuses that pane.
    framework_->ActivateTabById(browser.value, tab);
    return;
  }
  // The pane menu: Swap panes · Convert · Unsplit · ─ · Close pane.
  PhiMenuSpec spec;
  spec.items.push_back(PhiMenuItem::Command("tab.reversePanes", L"Swap panes"));
  spec.items.push_back(PhiMenuItem::Command(
      "tab.convertSplit", SplitIsStacked(row.id) ? L"Convert to side by side"
                                                 : L"Convert to stacked"));
  spec.items.push_back(PhiMenuItem::Command("tab.removeSplit", L"Unsplit"));
  spec.items.push_back(PhiMenuItem::Separator());
  spec.items.push_back(PhiMenuItem::Command("split.closePane", L"Close pane"));
  POINT screen = {rc.right, rc.bottom};
  ::ClientToScreen(split_chip_, &screen);
  ::ScreenToClient(shell_hwnd_, &screen);
  const PhiMenuAction action =
      PhiRunPopupMenu(spec, PopupAt(screen, TPM_RIGHTALIGN | TPM_TOPALIGN));
  if (action.is("tab.reversePanes")) {
    sidebar_->Apply(SidebarSwapPanes{row.key});
  } else if (action.is("tab.convertSplit")) {
    sidebar_->Apply(SidebarSetLayout{
        row.key, SplitIsStacked(row.id) ? "vertical" : "horizontal"});
  } else if (action.is("tab.removeSplit")) {
    sidebar_->Apply(SidebarBreakSplit{row.key});
  } else if (action.is("split.closePane")) {
    framework_->CloseTabById(browser.value, tab);
  }
  HideSplitChip();
}

// static
LRESULT CALLBACK PhiShellWindow::SplitChipProc(HWND hwnd,
                                               UINT msg,
                                               WPARAM wparam,
                                               LPARAM lparam) {
  if (msg == WM_NCDESTROY) {
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiShellWindow*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self || self->destroying_) {
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      self->PaintSplitChip(dc);
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_MOUSEMOVE:
      ::InvalidateRect(hwnd, nullptr, FALSE);  // The ⋯ hover wash.
      return 0;
    case WM_LBUTTONDOWN:
      self->OnSplitChipClick({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
      return 0;
    case WM_DESTROY:
      if (self->split_chip_ == hwnd) {
        self->split_chip_ = nullptr;
        self->split_chip_tab_ = 0;
      }
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool PhiShellWindow::TuckFloatingSidebar() {
  if (!floating_sidebar_ || !floating_expanded_) {
    return false;
  }
  HideFloatingSidebar(/*animated=*/true);
  return true;
}

void PhiShellWindow::ShowFloatingSidebar() {
  if (!sidebar_collapsed_ || !shell_hwnd_) {
    return;
  }
  EnsureFloatingPanel();
  SlideFloatingPanel(/*expanded=*/true);
}

void PhiShellWindow::HideFloatingSidebar(bool animated) {
  if (!floating_sidebar_) {
    return;
  }
  SlideFloatingPanel(/*expanded=*/false);
  if (!animated) {
    floating_slide_start_ = 0;
    PositionFloatingPanel();
  }
  if (browsers_->content_fullscreen()) {
    // Over a fullscreen page the keyboard goes back to the page as the
    // panel leaves (the Mac's returnKeyboardToFullscreenPage), so the next
    // Esc leaves fullscreen through the page. Posted: Esc's own handler and
    // the hover tick call in here.
    ::PostMessageW(shell_hwnd_, kMsgPhiCycleFocus, kPhiFocusPage, 0);
  }
}

void PhiShellWindow::FloatingSidebarTick() {
  if (!floating_sidebar_ || ::IsIconic(shell_hwnd_) ||
      !::IsWindowVisible(shell_hwnd_)) {
    return;
  }
  const ULONGLONG now = ::GetTickCount64();
  // The slide: 150 ms ease-out between the two rests.
  if (floating_slide_start_) {
    const double t = std::min(
        1.0, static_cast<double>(now - floating_slide_start_) / 150.0);
    const double eased = 1.0 - (1.0 - t) * (1.0 - t);
    const int x = floating_slide_from_ +
                  static_cast<int>((floating_slide_to_ - floating_slide_from_) *
                                   eased);
    RECT rc = {};
    ::GetWindowRect(floating_sidebar_, &rc);
    ::SetWindowPos(floating_sidebar_, nullptr, x, rc.top, 0, 0,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
    ClipFloatingPanel();
    if (t >= 1.0) {
      floating_slide_start_ = 0;
      if (!floating_expanded_) {
        ::ShowWindow(floating_sidebar_, SW_HIDE);
      }
    }
  }
  // Tab changes, hover cards and theme ramps repaint the shell; they land
  // in the panel on the next tick. Nothing changed, nothing is painted.
  if (::IsWindowVisible(floating_sidebar_) &&
      (floating_slide_start_ || floating_painted_at_ != chrome_paints_)) {
    floating_painted_at_ = chrome_paints_;
    ::InvalidateRect(floating_sidebar_, nullptr, FALSE);
  }
  POINT cursor = {};
  ::GetCursorPos(&cursor);
  RECT panel_rc = {};
  ::GetWindowRect(floating_sidebar_, &panel_rc);
  // The hover zone is the panel's VISIBLE part plus the strip between it
  // and the window's outer left edge: the frame's HTLEFT band slides the
  // panel in, so a pointer parked there must also hold it (else it flips
  // in and out for as long as the pointer rests on the edge), and the
  // hidden panel's off-window part is no target.
  RECT shell_rc = {};
  ::GetWindowRect(shell_hwnd_, &shell_rc);
  RECT zone = panel_rc;
  zone.left = shell_rc.left;
  if (!floating_expanded_) {
    // Preserve the 8-DIP reveal target without keeping any popup pixels
    // visible. Do not let the closing animation widen the hover target.
    POINT origin = {};
    ::ClientToScreen(shell_hwnd_, &origin);
    zone.right = origin.x + Scaled(kEdgeInset);
  }
  zone.right = std::min(zone.right, shell_rc.right);
  const bool inside = ::PtInRect(&zone, cursor) != FALSE;
  if (!inside && ::WindowFromPoint(cursor) != shell_hwnd_) {
    UpdateChromeTip(std::wstring());  // A panel button's tip: no leave event.
  }
  if (!floating_expanded_) {
    // Hidden: a 300 ms dwell at the window's edge slides the panel in.
    if (!inside) {
      floating_hover_tick_ = 0;
    } else if (!floating_hover_tick_) {
      floating_hover_tick_ = now;
    } else if (now - floating_hover_tick_ >= 300) {
      SlideFloatingPanel(/*expanded=*/true);
    }
    return;
  }
  // Slid in: it stays while the pointer is on it, a drag is in flight or a
  // menu is up from it; otherwise 150 ms after the pointer leaves (and at
  // least 500 ms after it came up) it tucks itself away.
  const bool held =
      inside || sidebar_->drag_armed() || PhiModalDepthFor(shell_hwnd_) > 0;
  if (held) {
    floating_outside_tick_ = 0;
    return;
  }
  if (!floating_outside_tick_) {
    floating_outside_tick_ = now;
    return;
  }
  if (now - floating_outside_tick_ >= 150 &&
      now - floating_shown_tick_ >= 500) {
    SlideFloatingPanel(/*expanded=*/false);
  }
}

void PhiShellWindow::PaintFloatingSidebar(HDC dc) {
  if (!floating_sidebar_) {
    return;
  }
  RECT panel_rc = {};
  ::GetClientRect(floating_sidebar_, &panel_rc);
  // The rows were built for this geometry on the rebuild turn (RebuildTabs
  // builds in the expanded layout while the panel is up); only the layout
  // override is flipped here, for PaintChrome's rects — and restored, not
  // cleared: a paint inside a menu opened from the panel must leave the
  // menu's own override standing for the handler that follows it.
  std::optional<base::AutoReset<bool>> floating_scope;
  floating_scope.emplace(&floating_layout_, true);
  const Layout layout = ComputeLayout();
  const PhiPalette palette = CurrentPalette();
  HDC mem = ::CreateCompatibleDC(dc);
  // A top-down 32bpp DIB, so a multi-colour palette can regrade its ground.
  const int width = std::max<int>(layout.client.right, 1);
  const int height = std::max<int>(layout.client.bottom, 1);
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bitmap = ::CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits,
                                      nullptr, 0);
  HGDIOBJ old = ::SelectObject(mem, bitmap);
  // include_child_controls: the panel has no live EDIT; the address text
  // is painted by hand where the layout has a field.
  PaintChrome(mem, layout, palette, /*include_child_controls=*/true);
  sidebar_->DrawDragIndicator(mem);  // As the shell's paint: after the rows.
  if (bits && palette.ground_stop_count > 1) {
    // The docked sidebar's regrade (PrepareBackdropPixels), opaque: the
    // panel is a plain window over the page.
    ::GdiFlush();
    const uint32_t rgb = PhiPackedRgb(palette.ground_top);
    const int column = std::clamp<int>(panel_rc.right, 0, width);
    const std::vector<uint32_t> grounds = PhiBackdropRowGrounds(
        palette, 0, height, height, /*glass=*/false);
    // SAFETY: CreateDIBSection allocated width * height 32-bit pixels in a
    // top-down DIB; 32bpp rows require no additional stride padding.
    auto pixels = UNSAFE_BUFFERS(base::span<uint32_t>(
        static_cast<uint32_t*>(bits), static_cast<size_t>(width) * height));
    for (int y = 0; y < height; ++y) {
      const uint32_t ground = grounds[static_cast<size_t>(y)];
      for (uint32_t& pixel : pixels.subspan(static_cast<size_t>(y) * width,
                                            static_cast<size_t>(column))) {
        pixel = (pixel & 0xFFFFFF) == rgb
                    ? ground : PhiBackdropPixelOver(pixel, rgb, ground);
      }
    }
  }
  ::BitBlt(dc, 0, 0, panel_rc.right, panel_rc.bottom, mem, 0,
           floating_offset_y_, SRCCOPY);
  ::SelectObject(mem, old);
  ::DeleteObject(bitmap);
  ::DeleteDC(mem);
  floating_scope.reset();
  // The panel's edge: 1 px stroke_strong (the DWM corners round it).
  HPEN pen = ::CreatePen(PS_SOLID, 1, palette.stroke_strong);
  HGDIOBJ old_pen = ::SelectObject(dc, pen);
  HGDIOBJ old_brush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
  ::Rectangle(dc, panel_rc.left, panel_rc.top, panel_rc.right,
              panel_rc.bottom);
  ::SelectObject(dc, old_brush);
  ::SelectObject(dc, old_pen);
  ::DeleteObject(pen);
}

// static
LRESULT CALLBACK PhiShellWindow::FloatingSidebarProc(HWND hwnd,
                                                          UINT msg,
                                                          WPARAM wparam,
                                                          LPARAM lparam) {
  if (msg == WM_NCDESTROY) {
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);  // See ShellWndProc.
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiShellWindow*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self || self->destroying_) {
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  // Events forward through the docked sidebar's handlers under the
  // expanded-layout override, shifted by the panel's band offset. The
  // outermost one settles the override: it stays while a drag armed here is
  // in flight (a release inside a commit re-enters through
  // WM_CAPTURECHANGED, which must not end the drag under its own commit).
  const auto forward = [&](auto&& call) {
    ++self->floating_forward_depth_;
    self->floating_layout_ = true;
    call();  // Reads the rows as they are (built for this geometry).
    if (--self->floating_forward_depth_ == 0) {
      self->floating_drag_ =
          self->floating_drag_ && self->sidebar_->drag_armed();
      self->floating_layout_ = self->floating_drag_;
    }
    ::InvalidateRect(hwnd, nullptr, FALSE);
  };
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;  // Focus stays with the page.
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (self) {
        self->PaintFloatingSidebar(dc);
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_PRINTCLIENT:  // AnimateWindow renders the slide through this.
      if (self) {
        self->PaintFloatingSidebar(reinterpret_cast<HDC>(wparam));
      }
      return 0;
    case WM_MOUSEMOVE:
      if (self) {
        if (self->floating_drag_) {
          // The docked band's drag move; once it lifts, the session holds
          // the capture here, so the moves keep coming off the panel too.
          forward([&] {
            if (wparam & MK_LBUTTON) {
              self->sidebar_->UpdateDrag({GET_X_LPARAM(lparam),
                                          GET_Y_LPARAM(lparam) +
                                              self->floating_offset_y_});
              self->UpdateChromeTip(std::wstring());
            }
          });
          return 0;
        }
        if (!self->floating_expanded_) {
          return 0;  // Tucked: the tick's dwell decides.
        }
        forward([&] {
          POINT pt = {GET_X_LPARAM(lparam),
                      GET_Y_LPARAM(lparam) + self->floating_offset_y_};
          self->OnChromeMouseMove(pt);
          self->UpdateChromeTip(self->ChromeTipAt(pt));
        });
        return 0;
      }
      break;
    case WM_LBUTTONDOWN:
      if (self) {
        const POINT pt = {GET_X_LPARAM(lparam),
                          GET_Y_LPARAM(lparam) + self->floating_offset_y_};
        if (!self->floating_expanded_) {
          return 0;  // Ignore clicks on the closing animation.
        }
        forward([&] {
          self->OnChromeClick(pt.x, pt.y);
          // A press on a row arms a drag, and a tab activates on its
          // release: the panel runs it to the end, as the docked band does.
          self->floating_drag_ = self->sidebar_->drag_armed();
        });
        return 0;
      }
      break;
    case WM_LBUTTONUP:
      if (self && self->floating_drag_) {
        forward([&] { self->sidebar_->CommitDrag(); });
        return 0;
      }
      break;
    case WM_CAPTURECHANGED:
      if (self && self->floating_drag_) {
        forward([&] { self->sidebar_->AbortDrag(); });
        return 0;
      }
      break;
    case WM_RBUTTONUP:
      if (self && self->floating_expanded_) {
        forward([&] {
          self->OnChromeContextMenu(GET_X_LPARAM(lparam),
                                    GET_Y_LPARAM(lparam) +
                                        self->floating_offset_y_);
        });
        return 0;
      }
      break;
    case WM_MOUSEWHEEL:
      if (self && self->floating_expanded_) {
        forward([&] {
          self->DismissChromeTip();
          POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
          ::ScreenToClient(hwnd, &pt);
          pt.y += self->floating_offset_y_;
          if (self->sidebar_->OnWheel(pt, GET_WHEEL_DELTA_WPARAM(wparam)) &&
              self->rows_dirty_) {
            // As the docked wheel: the rows moved under a resting pointer,
            // so rebuild now and re-hover the row that is under it.
            self->FlushPendingRebuild();
            self->SpacesForgetHover();
            self->OnChromeMouseMove(pt);
          }
        });
        return 0;
      }
      break;
    case WM_TIMER:
      if (self && wparam == kFloatingTickTimer) {
        self->FloatingSidebarTick();
        return 0;
      }
      // A panel drag's own clocks (SidebarShellWindow is the panel), routed
      // as the shell routes them.
      if (self && wparam == PhiSidebarDragSession::kHoverExpandTimer) {
        ::KillTimer(hwnd, PhiSidebarDragSession::kHoverExpandTimer);
        forward([&] { self->sidebar_->OnHoverExpandTimer(); });
        return 0;
      }
      if (self && wparam == PhiSidebarDragSession::kSlideTimer) {
        bool running = false;
        forward([&] { running = self->sidebar_->OnSlideTimer(); });
        if (!running) {
          ::KillTimer(hwnd, PhiSidebarDragSession::kSlideTimer);
        }
        return 0;
      }
      break;
    case WM_DESTROY:
      ::KillTimer(hwnd, kFloatingTickTimer);
      if (self && self->floating_sidebar_ == hwnd) {
        self->floating_sidebar_ = nullptr;
      }
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool PhiShellWindow::BookmarkBarVisible() {
  if (!services_.prefs || ShowsIncognito() || browsers_->content_fullscreen()) {
    return false;
  }
  // A comfortable-layout feature only (the Mac's bookmarkBarSupported =
  // traditionalLayout && ...): the vertical layouts keep bookmarks in the
  // sidebar, whatever the two prefs say.
  if (layout_mode_ != PhiLayoutMode::kComfortable) {
    return false;
  }
  if (services_.prefs->AlwaysShowBookmarkBar()) {
    return true;
  }
  if (!services_.prefs->BookmarkBarOnNewTab()) {
    return false;
  }
  // "On new tab": Chromium's NTP, a blank tab, or the placeholder shell.
  if (browsers_->ActiveBrowserPlaceholding()) {
    return true;
  }
  return active_tab_url().empty() || active_tab_url() == "about:blank" ||
         active_tab_url().rfind("chrome://newtab", 0) == 0 ||
         active_tab_url().rfind("chrome://new-tab-page", 0) == 0;
}

void PhiShellWindow::PaintBookmarkBar(HDC dc,
                                           const Layout& layout,
                                           const PhiPalette& chrome_palette) {
  bar_items_.clear();
  const RECT& band = layout.bookmark_bar;
  if (band.right <= band.left || band.bottom <= band.top) {
    return;
  }
  // The bar follows the address row's page colour (the Mac's BookmarkBar
  // setPageColorPresentation); otherwise it stands on the tint as before.
  const std::optional<PhiColor> page = ActivePageColor();
  const PhiPalette palette =
      page ? PhiPageBarPalette(chrome_palette, *page) : chrome_palette;
  if (page) {
    HBRUSH ground = ::CreateSolidBrush(*page);
    ::FillRect(dc, &band, ground);
    ::DeleteObject(ground);
  }
  // Root rows only — the sidebar outline's spliced children (depth > 0)
  // belong to the sidebar's disclosure state, not the bar.
  std::vector<int> roots;
  for (size_t i = 0; i < sidebar_->bookmarks().size(); ++i) {
    if (sidebar_->bookmarks()[i].depth == 0) {
      roots.push_back(static_cast<int>(i));
    }
  }
  ::SetBkMode(dc, TRANSPARENT);
  if (label_font_) {
    ::SelectObject(dc, label_font_);  // 12 dip (§4.1).
  }
  if (roots.empty()) {
    // The Mac bar's empty hint, dimmed and leading.
    RECT hint = band;
    hint.left += Scaled(12);
    ::SetTextColor(dc, palette.label_dim);
    PhiDrawTextAA(dc, L"Bookmarks you add appear here", -1, &hint,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    return;
  }
  const int pill_h = Scaled(24);
  const int pad = Scaled(8);
  const int icon_px = Scaled(kFaviconSize);
  const int more_w = Scaled(24);
  const int mid = (band.top + band.bottom) / 2;
  const int limit = band.right - pad - more_w;
  int x = band.left + pad;
  size_t shown = 0;
  for (const int index : roots) {
    const ShortcutRow& row = sidebar_->bookmarks()[static_cast<size_t>(index)];
    SIZE extent = {0, 0};
    ::GetTextExtentPoint32W(dc, row.title.c_str(),
                            static_cast<int>(row.title.size()), &extent);
    const int label_w = std::min<int>(extent.cx, Scaled(160));
    const int pill_w = pad + icon_px + Scaled(6) + label_w + pad;
    if (x + pill_w > limit) {
      break;
    }
    const RECT pill = {x, mid - pill_h / 2, x + pill_w, mid + pill_h / 2};
    RECT icon = {pill.left + pad, mid - icon_px / 2, pill.left + pad + icon_px,
                 mid + icon_px / 2};
    if (row.folder) {
      sidebar_->DrawFolderIcon(dc, icon, row.icon, palette.label_dim, false);
    } else if (!row.favicon.empty()) {
      sidebar_->DrawFavicon(dc, icon, row.favicon.data(), row.favicon_width,
                  row.favicon_height, palette.ground_top);
    } else {
      PhiDrawIconGlyph(dc, icon, fluent::kGlobe, icon_px, palette.label_dim);
    }
    RECT text = {icon.right + Scaled(6), pill.top, pill.right - pad,
                 pill.bottom};
    if (label_font_) {
      ::SelectObject(dc, label_font_);
    }
    ::SetTextColor(dc, palette.label);
    PhiDrawTextAA(dc, row.title.c_str(), -1, &text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
    bar_items_.push_back({pill, index, false});
    x = pill.right + Scaled(8);
    ++shown;
  }
  if (shown < roots.size()) {
    // The Mac's "More Bookmarks" chevron.
    const RECT more = {limit, mid - pill_h / 2, limit + more_w,
                       mid + pill_h / 2};
    PhiDrawIconGlyph(dc, more, fluent::kChevronRight, Scaled(12),
                     palette.label_dim);
    BarItem overflow;
    overflow.bounds = more;
    overflow.bookmark = static_cast<int>(shown);  // First hidden root.
    overflow.overflow = true;
    bar_items_.push_back(overflow);
  }
}

void PhiShellWindow::ShowBookmarkBarMenu(RECT anchor,
                                              std::string parent_guid,
                                              size_t skip_first) {
  if (!services_.sidebar) {
    return;
  }
  // The outline is read into a value FIRST: the dropdown's loop can dispatch
  // a rebuild, and the rows it walks must not be the store's live ones.
  const PhiMenuSpec spec = PhiBuildBookmarkBarMenu(
      BookmarkTreeSnapshot(parent_guid, 0), skip_first);
  const POINT at = {anchor.left, anchor.bottom};
  const PhiMenuAction action =
      PhiRunPopupMenu(spec, PopupAt(at, TPM_LEFTALIGN | TPM_TOPALIGN));
  if (action.is("bar.open")) {
    OpenOrFocusPinned(action.target.id, action.target.url);
  }
}

void PhiShellWindow::PaintChrome(HDC dc,
                                 const Layout& layout,
                                 const PhiPalette& palette,
                                 bool include_child_controls) {
  // The address row's palette: the page's own colour while the row follows
  // it (ActivePageColor), CurrentPalette otherwise. A control takes the
  // palette of the row it stands in -- the comfortable strip row keeps the
  // tint.
  const std::optional<PhiColor> page = ActivePageColor();
  const PhiPalette bar = page ? PhiPageBarPalette(palette, *page) : palette;
  const auto row_palette = [&](const RECT& r) -> const PhiPalette& {
    return r.left >= layout.top_bar.left && r.right <= layout.top_bar.right &&
                   r.top >= layout.top_bar.top && r.bottom <= layout.top_bar.bottom &&
                   layout.top_bar.bottom > layout.top_bar.top
               ? bar
               : palette;
  };
  const COLORREF kTintTop = palette.ground_top;
  const COLORREF kLabel = palette.label;
  const COLORREF kLabelDim = palette.label_dim;
  // The address field's two fills. In performance the field is a pill in the
  // SIDEBAR, so it rests on the 4% wash over the Space tint (sidebarTabHovered
  // — SideAddressBar's showBackgroundWhenInactive), brightening to 8%. In
  // balanced and comfortable it sits on the TOP BAR, whose ground is
  // windowBackground: the Mac composites the very same translucent overlays
  // there (WebContentAddressBar's .sidebarTabHovered over the header), so the
  // field carries no Space tint in those two. Blending them over the tinted
  // ground instead is what painted a themed pill on the untinted bar.
  // The address row is the card in every layout: the field's resting fill
  // is the control fill over it, its hover a step darker (§4).
  const COLORREF kFieldBg = bar.control_fill;
  const COLORREF kFieldBgHot = PhiBlend(bar.ink, bar.card, 18);
  // Paint only the bands we own. Whatever is left is the content region, and
  // the reparented browser child covers it. No seam along the content edge:
  // the chrome runs straight into the page.
  // The Space tint, painted over the WHOLE client area: the content card is
  // inset, so the ground shows around it on every side. Banded by hand rather
  // than GradientFill, which lives in msimg32 and is not linked here.
  ::ExcludeClipRect(dc, layout.content.left, layout.content.top,
                    layout.content.right, layout.content.bottom);
  // Flat (docs/windows-native-ui.md §2.3: no vertical band gradient).
  {
    HBRUSH ground = ::CreateSolidBrush(kTintTop);
    ::FillRect(dc, &layout.client, ground);
    ::DeleteObject(ground);
  }

  ::SelectClipRgn(dc, nullptr);

  // The pane (§4.2): flush to the window's top, right and bottom; its LEFT
  // corners round at kContentRadius against the sidebar's tint, a 1 px
  // stroke on that edge and the sheet shadow (§2.4) so it floats. The
  // horizontal layout has no pane: its page runs edge to edge. A split
  // keeps it: the framework's two panes sit flush inside this one card
  // (docs/split-view-chrome.md).
  if (layout.content.right > layout.content.left &&
      layout.mode != PhiLayoutMode::kComfortable && layout.content.left > 0) {
    RECT pane = layout.content;
    if (layout.top_bar.right > layout.top_bar.left) {
      pane.top = layout.top_bar.top;
    }
    const int radius = Scaled(kContentRadius);
    // The right corners are off the window: extend the rect past the edge
    // so only the left arcs are drawn.
    pane.right += radius * 2;
    // The shadow: three 1 px rings outside the edge at falling alpha
    // (0 2 8 @12 %, approximated on the tint), then the stroke.
    const std::array<int, 3> alphas = {14, 8, 4};
    for (int i = 3; i >= 1; --i) {
      RECT ring = pane;
      ::InflateRect(&ring, i, i);
      ring.top += 1;  // The shadow falls downward: none above the top edge.
      PhiStrokeRoundRectAA(dc, ring, radius + i,
                           PhiBlend(palette.ink, kTintTop, alphas[static_cast<size_t>(i - 1)]), 1);
    }
    RECT frame_rc = pane;
    ::InflateRect(&frame_rc, 1, 1);
    PhiStrokeRoundRectAA(dc, frame_rc, radius + 1, palette.border, 1);
  }

  PaintBookmarkBar(dc, layout, palette);

  ::SetBkMode(dc, TRANSPARENT);
  HGDIOBJ previous_font = ui_font_ ? ::SelectObject(dc, ui_font_) : nullptr;

  // The top bar (balanced / comfortable): the content card's FIRST band -- the
  // Mac's WebContentHeader is the card's first stack item -- so it paints on
  // the card's own ground (windowBackground) with the card's rounded top
  // corners, and a 1pt separator under it running into the page below.
  if (layout.top_bar.right > layout.top_bar.left) {
    const RECT& band = layout.top_bar;
    const int radius = Scaled(kContentRadius);
    HBRUSH bg = ::CreateSolidBrush(bar.window_bg);
    // A split leaves the row as it is: the panes sit flush under its
    // separator (docs/split-view-chrome.md).
    if (layout.mode == PhiLayoutMode::kComfortable || band.left == 0) {
      // Edge to edge and square: the horizontal toolbar, or the pane
      // filling the window with the sidebar hidden.
      ::FillRect(dc, &band, bg);
    } else {
      // The pane's top-left corner at kContentRadius — the curve the frame
      // strokes; the right end runs off the window's edge.
      RECT rounded = band;
      rounded.right += radius * 2;
      PhiFillRoundRectAA(dc, rounded, radius, bar.window_bg);
      RECT lower = {band.left, band.top + radius, band.right, band.bottom};
      ::FillRect(dc, &lower, bg);
    }
    ::DeleteObject(bg);
    RECT sep = {band.left, band.bottom - 1, band.right, band.bottom};
    HBRUSH sep_brush = ::CreateSolidBrush(bar.separator);
    ::FillRect(dc, &sep, sep_brush);
    ::DeleteObject(sep_brush);
  }

  // The caption buttons and the divider, over the row they share (the
  // address row in the vertical layout, the strip row in the horizontal):
  // painted AFTER the row's ground so the fill cannot cover them
  // (phi_caption_band.h owns their hot states).
  caption_band_->Paint(dc, /*with_buttons=*/!caption_buttons_suppressed_);

  if (layout.app_icon.right > layout.app_icon.left) {
    const auto& logo = PhiLogoBgra256();
    if (!logo.empty()) {
      RECT mark = layout.app_icon;
      ::InflateRect(&mark, -Scaled(6), -Scaled(6));
      PhiDrawBgra(dc, mark, logo.data(), 256, 256);
    }
  }

  // The toolbar buttons (§2.2a): one painter, 32 square, radius 4, a 16
  // dip glyph, the state's wash. Each mode empties the rects it hides, so
  // nothing draws where a control does not exist.
  {
    struct NavButton {
      RECT rect;
      const wchar_t* glyph;
      bool enabled;
      bool toggled;
    };
    const std::array<NavButton, 6> nav_buttons = {{
        {layout.back, fluent::kBack, omnibox_->can_go_back(), false},
        {layout.forward, fluent::kForward, omnibox_->can_go_forward(), false},
        {layout.reload, omnibox_->nav_loading() ? fluent::kStop : fluent::kRefresh,
         true, false},
        {layout.search_tabs, fluent::kSearch, true, false},
        // Comfortable's strip "+", and every layout's app menu (⋯).
        {sidebar_->add_tab_button(), fluent::kAdd, true, false},
        {layout.app_menu, fluent::kMore, true, app_menu_ && app_menu_->open()},
    }};
    for (size_t i = 0; i < nav_buttons.size(); ++i) {
      const NavButton& button = nav_buttons[i];
      if (button.rect.right <= button.rect.left) {
        continue;  // Hidden in this mode.
      }
      const bool hot = hover_kind_ == HoverKind::kNav &&
                       hover_index_ == static_cast<int>(i);
      const PhiButtonState state =
          !button.enabled ? PhiButtonState::kDisabled
          : button.toggled ? PhiButtonState::kPressed
          : hot            ? PhiButtonState::kHover
                           : PhiButtonState::kRest;
      const PhiPalette& ink = row_palette(button.rect);
      PhiPaintIconButton(dc, button.rect, button.glyph, state, ink, ink.label,
                         Scaled(fluent::kIconToolbar),
                         Scaled(fluent::kButtonRadius));
    }
    // The agent chip (§5.10), ordinal 16 in the hover table.
    PaintAgentChip(dc, layout.agent_chip, row_palette(layout.agent_chip),
                   hover_kind_ == HoverKind::kNav && hover_index_ == 16);
  }
  // The Update reminder (nav-hover ordinal 20), the Mac's upgrade button:
  // a radius 6 accent pill with the label on it, the accent's hover shade
  // under the pointer.
  if (layout.update.right > layout.update.left) {
    const bool hot = hover_kind_ == HoverKind::kNav && hover_index_ == 20;
    const PhiPalette& ink = row_palette(layout.update);
    PhiFillRounded(dc, layout.update, hot ? ink.accent_hover : ink.accent,
                   Scaled(6));
    HGDIOBJ previous_label = ::SelectObject(dc, EnsureChipNameFont());
    ::SetTextColor(dc, ink.on_accent);
    RECT text_rc = layout.update;
    PhiDrawTextAA(dc, PhiStr("sidebar.updateReminder.updateButton"), -1,
                  &text_rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                      DT_END_ELLIPSIS);
    if (previous_label) {
      ::SelectObject(dc, previous_label);
    }
  }
  // The sidebar toggle (nav-hover ordinal 14): ClosePane while the sidebar
  // is shown, OpenPane while it is hidden — the stateful Fluent pair.
  if (layout.sidebar_toggle.right > layout.sidebar_toggle.left) {
    const bool hot = hover_kind_ == HoverKind::kNav && hover_index_ == 14;
    PhiPaintIconButton(dc, layout.sidebar_toggle,
                       sidebar_collapsed_ ? fluent::kOpenPane : fluent::kClosePane,
                       hot ? PhiButtonState::kHover : PhiButtonState::kRest,
                       row_palette(layout.sidebar_toggle),
                       row_palette(layout.sidebar_toggle).label,
                       Scaled(fluent::kIconToolbar),
                       Scaled(fluent::kButtonRadius));
  }

  // Address field. In the sidebar it is a pill on the 4% wash with no border
  // (SideAddressBar); in the top bar it is bare -- transparent until hovered on
  // Mac -- so only the EDIT's text shows there. The capture path (no children
  // rendered) draws the text itself so a screenshot still shows the address.
  // The in-field trailing stack (reader, copy link, extensions menu) sits ON
  // the address field, which already wears the resting 4% wash — so a button
  // there takes the EMPHASIZED one, or it paints itself invisible. And while
  // the pointer is on one of them the field drops back to resting, so what
  // reads as lit is the button rather than the whole bar: the Mac lights the
  // button's own circle, not the address bar behind it.
  // The EDIT must give the accessories their room back the moment one
  // appears; nothing else re-fits it between layout changes. NOT from the
  // floating panel's paint: that runs against the expanded layout of a
  // collapsed shell, and re-seating the shell's live EDIT there dragged it
  // to the docked field's place on the collapsed card.
  if (!floating_layout_ && !sidebar_resize_snapshot_) {
    omnibox_->Position();
  }
  const auto on_field = [&](const RECT& r) {
    return layout.url_field.right > layout.url_field.left && r.right > r.left &&
           r.left >= layout.url_field.left && r.right <= layout.url_field.right;
  };
  const auto accessory_hot = [&](int index, const RECT& r) {
    return hover_kind_ == HoverKind::kNav && hover_index_ == index &&
           r.right > r.left;
  };
  const bool field_tools = FieldToolsShown();
  const bool pointer_on_field_accessory =
      (accessory_hot(13, layout.reader) && on_field(layout.reader)) ||
      (field_tools && accessory_hot(18, layout.copy_link) &&
       on_field(layout.copy_link)) ||
      (field_tools && accessory_hot(19, layout.site_menu) &&
       on_field(layout.site_menu));
  const bool field_hot =
      omnibox_->field_hover() && !pointer_on_field_accessory;
  const bool field_focused = omnibox_->FieldHasFocus();
  if (layout.url_field.right > layout.url_field.left) {
    // §4: rest = control fill (ink 4.5 % / white 6 %), radius 8, no stroke;
    // hover a step darker; focus = the card body with a 1 px stroke and a
    // 2 px accent bottom edge (Fluent TextBox).
    const int radius = Scaled(kOmniboxRadius);
    if (field_focused) {
      PhiFillRoundRectAA(dc, layout.url_field, radius, bar.card);
      PhiStrokeRoundRectAA(dc, layout.url_field, radius, bar.stroke_strong, 1);
      RECT edge = layout.url_field;
      edge.top = edge.bottom - Scaled(2);
      edge.left += radius;
      edge.right -= radius;
      HBRUSH accent = ::CreateSolidBrush(palette.accent);
      ::FillRect(dc, &edge, accent);
      ::DeleteObject(accent);
    } else {
      PhiFillRoundRectAA(dc, layout.url_field, radius,
                         field_hot ? kFieldBgHot : kFieldBg);
    }
    // The leading site-info slot: the page glyph, in `dim`.
    if (layout.page_icon.right > layout.page_icon.left) {
      // §4.9: an incognito window wears the incognito glyph in the
      // field's leading slot (its chrome is fixed dark, CurrentPalette).
      PhiDrawIconGlyph(dc, layout.page_icon,
                       ShowsIncognito() ? fluent::kIncognito : fluent::kGlobe,
                       Scaled(fluent::kIconToolbar), bar.label_dim);
    }
    // The keyword chip ahead of the text and the "Tab to search X" hint
    // behind it, when the session has either up.
    omnibox_->PaintKeyword(dc);
  }
  if (include_child_controls) {
    // What the user actually sees, mid-edit.
    const std::wstring shown = omnibox_->DisplayedUrl();
    ::SetTextColor(dc, shown.empty() ? bar.label_dim : bar.label);
    RECT url_rc = layout.url_field;
    url_rc.left = (layout.page_icon.right > layout.page_icon.left
                       ? layout.page_icon.right + Scaled(4)
                       : url_rc.left + Scaled(kFieldTextInset)) +
                  omnibox_->KeywordLeadingAdvance();
    // Stop before the in-field trailing stack when it exists — the leftmost
    // accessory that actually sits inside the capsule (the same rule
    // PhiOmniboxTextRect applies to the EDIT).
    int text_right = url_rc.right;
    for (const RECT& accessory :
         {layout.reader, layout.copy_link, layout.site_menu}) {
      if (accessory.right > accessory.left &&
          accessory.left > layout.url_field.left &&
          accessory.left < text_right) {
        text_right = accessory.left;
      }
    }
    url_rc.right = text_right - Scaled(8) - omnibox_->KeywordTrailingAdvance();
    PhiDrawTextAA(dc,
                  shown.empty()
                      ? (omnibox_->keyword_mode()
                             ? PhiStr("addressBar.keywordSearch.queryPlaceholder")
                             : L"Search or enter address")
                      : shown.c_str(),
                  -1, &url_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS |
                    DT_NOPREFIX);
  }
  // The pinned-extension icons leading the field's trailing stack: 16pt
  // icons in 24pt slots on the radius-4 hover wash (SideAddressBar's
  // extensionIconsStackView).
  if (layout.field_ext_strip.right > layout.field_ext_strip.left) {
    // The icons composite over the ground under them: the field's fill
    // inside the capsule (Performance, Balanced), the top bar's own ground
    // where Comfortable lays the strip beside the field.
    const POINT strip_mid = {
        (layout.field_ext_strip.left + layout.field_ext_strip.right) / 2,
        (layout.field_ext_strip.top + layout.field_ext_strip.bottom) / 2};
    const COLORREF field_fill =
        ::PtInRect(&layout.url_field, strip_mid)
            ? (omnibox_->field_hover() ? kFieldBgHot : kFieldBg)
            : static_cast<COLORREF>(bar.card);
    for (size_t i = 0; i < field_extensions_.size(); ++i) {
      const RECT slot = FieldExtRect(layout, i);
      if (slot.right <= slot.left) {
        continue;
      }
      if (hover_kind_ == HoverKind::kFieldExt &&
          hover_index_ == static_cast<int>(i)) {
        PhiFillRounded(dc, slot, bar.hover, Scaled(4));
      }
      const FieldExtension& ext = field_extensions_[i];
      const int mid_x = (slot.left + slot.right) / 2;
      const int mid_y = (slot.top + slot.bottom) / 2;
      RECT icon = {mid_x - Scaled(8), mid_y - Scaled(8), mid_x + Scaled(8),
                   mid_y + Scaled(8)};
      if (!ext.icon.empty()) {
        // A grayed action (disabled, no page interaction) renders
        // desaturated + lightened — the Mac's disabledActionVariant.
        if (ext.state.grayscale) {
          const std::vector<unsigned char> grayed =
              PhiGrayedActionIcon(ext.icon);
          sidebar_->DrawFavicon(dc, icon, grayed.data(), ext.icon_w,
                                ext.icon_h, field_fill);
        } else {
          sidebar_->DrawFavicon(dc, icon, ext.icon.data(), ext.icon_w,
                                ext.icon_h, field_fill);
        }
      } else {
        // Icon not decoded yet: the puzzle stands in.
        HGDIOBJ previous_ext_font =
            chip_icon_font_ ? ::SelectObject(dc, chip_icon_font_) : nullptr;
        PhiDrawGlyphIcon(dc, slot, fluent::kPuzzle,
                         Scaled(fluent::kIconToolbar), bar.label_dim);
        if (previous_ext_font) {
          ::SelectObject(dc, previous_ext_font);
        }
      }
      // The action badge, straddling the icon's bottom-right corner (the
      // Mac's BadgeCornerOverlay); nothing without badge text.
      PhiDrawExtensionBadge(dc, icon, ext.state, badge_font_, dpi_);
    }
  }
  // The in-field trailing slots (24, the icon in `dim`): Reader View when
  // offered, then Copy link and the site menu — all three hover-only.
  // Reader is hover-only too (the Mac's ReaderButtonView), except that it
  // stays while Reader View is on so the lit state does not vanish.
  const bool reader_active = ActiveTabShowsReader();
  if (layout.reader.right > layout.reader.left &&
      (field_tools || reader_active)) {
    const bool active = reader_active;
    PhiPaintIconButton(dc, layout.reader, fluent::kReadingMode,
                       active                        ? PhiButtonState::kToggled
                       : accessory_hot(13, layout.reader) ? PhiButtonState::kHover
                                                          : PhiButtonState::kRest,
                       bar, bar.label_dim, Scaled(fluent::kIconToolbar),
                       Scaled(fluent::kButtonRadius));
  }
  // The Mac's hover-only pair (CopyURLButtonView, the address-bar menu
  // button): shown while the pointer is on the field, the menu is open or
  // the copy flash is up. Copy link swaps to a check mark for a second; the
  // site menu's button stays lit while its menu tracks.
  if (field_tools && layout.copy_link.right > layout.copy_link.left) {
    PhiPaintIconButton(dc, layout.copy_link,
                       omnibox_->copy_link_flash() ? fluent::kCheckMark
                                                   : fluent::kLink,
                       accessory_hot(18, layout.copy_link)
                           ? PhiButtonState::kHover
                           : PhiButtonState::kRest,
                       bar, bar.label_dim, Scaled(fluent::kIconToolbar),
                       Scaled(fluent::kButtonRadius));
  }
  if (field_tools && layout.site_menu.right > layout.site_menu.left) {
    PhiPaintIconButton(dc, layout.site_menu, fluent::kEqualizer,
                       site_menu_open_ ? PhiButtonState::kPressed
                       : accessory_hot(19, layout.site_menu)
                           ? PhiButtonState::kHover
                           : PhiButtonState::kRest,
                       bar, bar.label_dim, Scaled(fluent::kIconToolbar),
                       Scaled(fluent::kButtonRadius));
  }

  // Space pips: 24pt, icon only. The selected one sits on a selected-pill
  // card (SpacesStripView's matchedGeometry indicator); the trailing "+"
  // creates a Space.
  // The pips, the create form's placeholder and the trailing "+" / "…"
  // (phi_spaces_strip.h). Only the strip paints them, and it never paints
  // anything else.
  // Not on the collapsed shell: with the floating panel up the chips and
  // rows are built for the panel's expanded geometry, and painted here they
  // would land on the widened card (the balanced top bar's own buttons).
  if (BandOnShell()) {
    spaces_->Paint(dc, StripRects(layout), palette);
    // The strip's active tab merges into the address row, so it wears the
    // row's page colour too -- except while the reader covers the page (the
    // Mac's tabStripPageColorPresentation).
    sidebar_->SetActiveTabPagePalette(
        page_color_ && !page_color_reader_
            ? std::optional<PhiPalette>(bar)
            : std::nullopt);
    sidebar_->Paint(dc, BandRects(layout), palette);
    // The section labels (§4.2): 12 dip, `dim`, over each band.
    HGDIOBJ previous_label_font =
        label_font_ ? ::SelectObject(dc, label_font_) : nullptr;
    ::SetTextColor(dc, kLabelDim);
    const auto label = [&](const RECT& rc, const wchar_t* text) {
      if (rc.right <= rc.left) {
        return;
      }
      RECT text_rc = {rc.left, rc.top + Scaled(8), rc.right, rc.bottom};
      PhiDrawTextAA(dc, text, -1, &text_rc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    };
    // The section labels are gone (§4.2); the one line left is §5.10's
    // cue 6, the agent's own Space reading "Claude's tabs".
    if (!agent_chip_compact_ && !agent_chip_name_.empty()) {
      const std::wstring owned = agent_chip_name_ + L"\u2019s tabs";
      label(layout.tabs_label, owned.c_str());
    }
    if (previous_label_font) {
      ::SelectObject(dc, previous_label_font);
    }
  }

  // Bottom bar — docs/windows-native-ui.md §4.2: a 40 dip row, 8 dip of
  // padding at each end, where the profile (avatar) button leads (the Mac's
  // ProfileButton: a 24 dip disc in a 32 hit box, no wash), then the
  // Downloads button (32 × 32, radius 4, a 16 dip glyph) with the memory
  // chip beside it while the AI suite is on, and the Feedback pill trails
  // (26 high, stroke_strong capsule, 16 glyph + 6 + the 12/600 label). The
  // Chat capsule (accent ground that darkens on hover) is AI-only and out
  // of the design for now. There is still NO settings gear here — Settings
  // is a row of the profile menu and of the app menu, as on Mac.
  // Ordinals into NavHoverTable, where the bottom bar's run starts at 6
  // (the retired address-row Downloads slot, memory, feedback, chat,
  // extensions); the bar's own Downloads button is the appended 17 and the
  // profile button 22.
  const auto bottom_hot = [&](int ordinal) {
    return hover_kind_ == HoverKind::kNav && hover_index_ == 6 + ordinal;
  };
  {
    const auto tool = [&](const RECT& rect, const wchar_t* glyph, bool hot) {
      PhiPaintIconButton(dc, rect, glyph,
                         hot ? PhiButtonState::kHover : PhiButtonState::kRest,
                         palette, kLabel, Scaled(fluent::kIconToolbar),
                         Scaled(fluent::kButtonRadius));
    };
    if (layout.sidebar_profile.right > layout.sidebar_profile.left) {
      // The profile button (nav-hover ordinal 22, tracked for its tooltip
      // only — the Mac's AvatarButton has no hover fill): the 24 dip disc
      // centred in the hit box. Signed in with a picture: the account's
      // avatar circle-clipped under a 1 px ring at ink 8 % (the Mac's
      // commonBorder); Guest Mode, signed out or no picture yet: the
      // Contact glyph (its person.crop.circle.fill), ringless.
      const RECT& b = layout.sidebar_profile;
      const int px = Scaled(24);
      const int left = (b.left + b.right - px) / 2;
      const int top = (b.top + b.bottom - px) / 2;
      const RECT disc = {left, top, left + px, top + px};
      bool drew_avatar = false;
      if (services_.window && !services_.window->AccountIsGuest() &&
          services_.window->AccountInfo(nullptr, nullptr)) {
        const std::vector<uint8_t>& circle =
            services_.window->AccountAvatarCircle(px);
        if (!circle.empty()) {
          drew_avatar = PhiBlendBgra(dc, disc, circle.data(), px, px);
        }
      }
      if (drew_avatar) {
        PhiStrokeEllipseAA(dc, disc, palette.border, std::max(1, Scaled(1)));
      } else {
        PhiDrawIconGlyph(dc, disc, fluent::kContact, px, kLabelDim);
      }
    }
    if (layout.sidebar_downloads.right > layout.sidebar_downloads.left) {
      // The bottom bar's own Downloads button (§4.2), nav-hover ordinal 17:
      // the design's line icon (a 24 dip frame, margin included) over the
      // same wash the Fluent tools use.
      const PhiButtonState state =
          bottom_hot(11) ? PhiButtonState::kHover : PhiButtonState::kRest;
      PhiPaintButtonWash(dc, layout.sidebar_downloads, state, palette,
                         Scaled(fluent::kButtonRadius));
      const int art = Scaled(24);
      const RECT& b = layout.sidebar_downloads;
      const int left = (b.left + b.right - art) / 2;
      const int top = (b.top + b.bottom - art) / 2;
      const RECT art_rc = {left, top, left + art, top + art};
      const std::vector<unsigned char> download_bgra = PhiTintedIconBgra(
          PhiDownloadIcon(), PhiButtonInk(state, palette, kLabel), art);
      PhiBlendBgra(dc, art_rc, download_bgra.data(), art, art);
    }
    if (layout.memory.right > layout.memory.left) {
      if (bottom_hot(1)) {
        PhiFillRounded(dc, layout.memory, palette.hover, Scaled(4));
      }
      const int memory_px = layout.memory.right - layout.memory.left;
      const std::vector<unsigned char> memory_bgra =
          PhiTintedIconBgra(PhiMemoryIcon(), palette.ink, memory_px);
      PhiBlendBgra(dc, layout.memory, memory_bgra.data(), memory_px,
                   memory_px);
    }
    if (layout.extensions.right > layout.extensions.left) {
      // The extensions puzzle button (nav-hover ordinal 10 = bottom_hot(4)).
      tool(layout.extensions, fluent::kPuzzle, bottom_hot(4));
    }
    if (layout.feedback.right > layout.feedback.left) {
      // Feedback (Figma Frame 48): a capsule with a 1 px ink-8 % outline
      // round the 16 dip bug; hover washes the capsule.
      const PhiButtonState state =
          bottom_hot(2) ? PhiButtonState::kHover : PhiButtonState::kRest;
      const RECT& b = layout.feedback;
      if (state == PhiButtonState::kHover) {
        PhiFillCapsuleAA(dc, b, palette.hover);
      }
      PhiStrokeCapsuleAA(dc, b, palette.border, std::max(1, Scaled(1)));
      const int art = Scaled(16);
      const int left = (b.left + b.right - art) / 2;
      const int top = (b.top + b.bottom - art) / 2;
      const RECT art_rc = {left, top, left + art, top + art};
      const std::vector<unsigned char> bug_bgra = PhiTintedIconBgra(
          PhiFeedbackIcon(), PhiButtonInk(state, palette, kLabel), art);
      PhiBlendBgra(dc, art_rc, bug_bgra.data(), art, art);
    }
  }
  if (layout.chat.right > layout.chat.left) {
    // themeColorOnHover: the accent's own hover role.
    const COLORREF chat_bg =
        bottom_hot(3) ? palette.accent_hover : palette.accent;
    PhiFillCapsuleAA(dc, layout.chat, chat_bg);
    // 22pt glyph box flush left (ChatButton's leadingPadding 0).
    const int icon_box = Scaled(22);
    const int mid = (layout.chat.top + layout.chat.bottom) / 2;
    RECT icon_rc = {layout.chat.left + Scaled(1), mid - icon_box / 2,
                    layout.chat.left + Scaled(1) + icon_box,
                    mid + icon_box / 2};
    // The Mac's chat icon (sidebar-chat.imageset), a template in whatever
    // reads on the accent (white, but not on a light accent or a contrast
    // theme).
    const std::vector<unsigned char> chat_bgra =
        PhiTintedIconBgra(PhiChatIcon(), palette.on_accent, icon_box);
    PhiBlendBgra(dc, icon_rc, chat_bgra.data(), icon_box, icon_box);
    HGDIOBJ previous_small =
        small_font_ ? ::SelectObject(dc, small_font_) : nullptr;
    ::SetTextColor(dc, palette.on_accent);
    RECT chat_rc = {icon_rc.right + Scaled(2), layout.chat.top,
                    layout.chat.right - Scaled(8), layout.chat.bottom};
    PhiDrawTextAA(dc, L"Chat", -1, &chat_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (previous_small) {
      ::SelectObject(dc, previous_small);
    }
  }

  // The Space hover card floats over the window as a popup, the way the
  // Mac's panel does; it is measured with this DC's font. While the
  // floating panel is up the card is the PANEL's (its strip is the one on
  // screen): the collapsed shell's paint would hide it, the panel's show
  // it again, each exposing the other.
  if (floating_layout_ || !floating_sidebar_ ||
      !::IsWindowVisible(floating_sidebar_)) {
    spaces_->SyncHoverCardPopup(dc, StripRects(layout));
  }

  if (previous_font) {
    ::SelectObject(dc, previous_font);
  }
}




LRESULT CALLBACK PhiShellWindow::ShellWndProc(HWND hwnd,
                                                   UINT msg,
                                                   WPARAM wparam,
                                                   LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_NCDESTROY) {
    // Final message: unhook the controller so nothing delivered to a
    // recycled HWND value can reach it (every popup and child proc below
    // does the same).
    if (auto* dying = reinterpret_cast<PhiShellWindow*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
      PhiUiaWindowDestroyed(dying);  // Release any provider a client holds.
    }
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_GETOBJECT) {
    // The sidebar's rows reach assistive technology from here: the shell is
    // the hit source (PhiHitSource), so the provider needs no window of its
    // own. Every other object id falls through to DefWindowProc, which
    // keeps answering MSAA for the child controls as before.
    auto* source = reinterpret_cast<PhiShellWindow*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    bool handled = false;
    const LRESULT result =
        source ? PhiUiaOnGetObject(source, wparam, lparam, &handled) : 0;
    if (handled) {
      return result;
    }
  }
  auto* self = reinterpret_cast<PhiShellWindow*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self && self->destroying_) {
    // The destructor's DestroyWindow: the object is going, its members are
    // half torn down — every handler below would do work on it.
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  switch (msg) {
    case WM_PARENTNOTIFY:
      // Native pages and their EDIT controls are child HWNDs. Their mouse
      // presses bypass this window's button handlers and Chromium's widget
      // activation, but Win32 forwards the press up the parent chain.
      if (self && (LOWORD(wparam) == WM_LBUTTONDOWN ||
                   LOWORD(wparam) == WM_RBUTTONDOWN ||
                   LOWORD(wparam) == WM_MBUTTONDOWN)) {
        self->CloseExtensionPopup();
      }
      break;
    case WM_ACTIVATEAPP:
      // Phi as a whole gained or lost the foreground (every top-level
      // window hears it): the accelerators drop a half-seen Alt.
      if (self && self->services_.window) {
        self->services_.window->NoteAppActivation(wparam != FALSE);
      }
      if (!wparam) {
        // Drop the active look kept for an owned popup (WM_ACTIVATE below):
        // the app itself is in the background now.
        ::DefWindowProcW(hwnd, WM_NCACTIVATE, FALSE, 0);
      }
      break;
    case WM_MOUSEACTIVATE: {
      // Let clicks activate without being eaten, and drive Chromium's own
      // activation across the boundary so input/focus/IME reach the page.
      // EXCEPT for clicks landing in our sidebar: the address field is a real
      // focusable control there, and handing focus to the page would fight the
      // click that is about to focus it.
      POINT cursor = {};
      ::GetCursorPos(&cursor);
      ::ScreenToClient(hwnd, &cursor);
      RECT content = {};
      if (self) {
        content = self->ComputeLayout().content;
      }
      const bool in_chrome = self && !::PtInRect(&content, cursor);
      // While the group overview covers the content, IT owns clicks there —
      // redirecting focus to the (hidden-under) browser child would close
      // the overview before its own WM_LBUTTONDOWN ever arrived.
      // Likewise while the native new-tab page covers the content: its own
      // window takes the click and decides where focus goes.
      const HWND page = self ? self->browsers_->content_hwnd() : nullptr;
      if (!in_chrome && self && !PhiGroupOverview::IsOpen() &&
          !PhiLibrary::IsOpen() &&
          !self->native_pages_->ntp_mounted() && page &&
          ::IsWindow(page)) {
        ::SetFocus(page);
        // The ACTIVE browser, never the slot's number: after a Space switch
        // the browser the slot opened with may already be closed.
        const BrowserId browser = self->active_browser_id();
        if (self->framework_ && browser.valid()) {
          self->framework_->FocusBrowserWindow(browser.value);
        }
      }
      return MA_ACTIVATE;
    }
    case WM_ACTIVATE:
    case WM_SETFOCUS:
      if (self && msg == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE) {
        // Where the keyboard is, for the activation that brings the window
        // back: WM_ACTIVATE arrives before the focus leaves (and before
        // WM_KILLFOCUS ends the sidebar's ring).
        self->keyboard_home_ =
            self->omnibox_->FieldHasFocus() ? KeyboardHome::kField
            : self->sidebar_ && self->sidebar_->keyboard_focused()
                ? KeyboardHome::kSidebar
                : KeyboardHome::kPage;
        self->HideFloatingSidebar(/*animated=*/true);  // Tucks on deactivate.
        self->UpdateChromeTip(std::wstring());
        // An owned popup retains this Browser's activation tree. A different
        // shell/application does not; WS_CHILD never receives this edge itself.
        const HWND next = reinterpret_cast<HWND>(lparam);
        if (next && ::GetAncestor(next, GA_ROOTOWNER) == hwnd) {
          // One of this window's own flyouts (Set theme, the icon picker,
          // a popover — the WS_EX_TOOLWINDOW family) took the keyboard: the
          // window stays ACTIVE-looking, as under an in-window flyout. DWM
          // draws the acrylic sidebar only on an active frame; inactive, it
          // falls back to an opaque fill for as long as the flyout is up.
          // An owned regular window (Settings) deactivates it as usual.
          if (::GetWindowLongPtrW(next, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) {
            ::DefWindowProcW(hwnd, WM_NCACTIVATE, TRUE, 0);
          }
        } else {
          self->CloseExtensionPopup();
          if (self->framework_ && self->active_browser_id().valid()) {
            self->framework_->FocusBrowserWindow(
                self->active_browser_id().value, false);
          }
        }
      }
      if (self && msg == WM_ACTIVATE && LOWORD(wparam) == WA_ACTIVE) {
        // Back by keyboard or program (Alt+Tab, a closing dialog): the
        // keyboard goes where it was. A click activation is the click's to
        // place (WM_MOUSEACTIVATE / the field's own press).
        const KeyboardHome home =
            std::exchange(self->keyboard_home_, KeyboardHome::kPage);
        const HWND field = self->omnibox_->field();
        if (home == KeyboardHome::kField && field && ::IsWindowVisible(field)) {
          ::SetFocus(field);  // Caret and selection as they were.
          return 0;
        }
        if (home == KeyboardHome::kSidebar && self->sidebar_ &&
            self->sidebar_->BeginKeyboardFocus()) {
          return 0;
        }
      }
      // Activation/focus arriving at the shell top-level must be forwarded to
      // the child: as a WS_CHILD, Chromium's window reports IsTopLevelWindow()
      // == false and skips its own activation path.
      if (const HWND page = self ? self->browsers_->content_hwnd() : nullptr;
          self && page && ::IsWindow(page) &&
          !self->omnibox_->FieldHasFocus() &&
          !(self->sidebar_ && self->sidebar_->keyboard_focused()) &&
          !(msg == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE)) {
        if (self->native_pages_->ntp_wants_focus()) {
          // The native new-tab page is up: its omnibox is the focus.
          self->native_pages_->FocusNtpField();
          return 0;
        }
        ::SetFocus(page);
        const BrowserId browser = self->active_browser_id();
        if (self->framework_ && browser.valid()) {
          self->framework_->FocusBrowserWindow(browser.value);
        }
      }
      return 0;
    case WM_ENABLE:
      // A modal shell dialog (the file picker) hands the activation back
      // while its owner is still disabled, and a disabled window's child
      // cannot take the focus: the forward above left the keyboard on the
      // shell itself, where every key is lost. Forward it again once the
      // dialog's run enables the owner.
      if (self && wparam && ::GetActiveWindow() == hwnd &&
          ::GetFocus() == hwnd) {
        ::PostMessageW(hwnd, WM_SETFOCUS, 0, 0);
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_CTLCOLOREDIT:
      // The EDIT would otherwise paint itself in SYSTEM colours; the session
      // hands it the palette's brush instead.
      if (self && self->services_.spaces) {
        if (HBRUSH brush = self->omnibox_->OnCtlColorEdit(
                reinterpret_cast<HDC>(wparam),
                reinterpret_cast<HWND>(lparam))) {
          return reinterpret_cast<LRESULT>(brush);
        }
      }
      break;
    case WM_GETMINMAXINFO:
      if (self) {
        // The sidebar when it docks (vertical layouts, not collapsed) plus a
        // usable page; the frame's resize borders are on top of the client.
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        const bool docked_sidebar =
            self->layout_mode_ != PhiLayoutMode::kComfortable &&
            !self->sidebar_collapsed_;
        const UINT dpi = self->dpi_;
        const int border_x = ::GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
                             ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const int border_y = ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
                             ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const int min_w =
            self->Scaled((docked_sidebar ? self->sidebar_dip_ : 0) +
                         kMinContentWidth) +
            2 * border_x;
        const int min_h = self->Scaled(kMinClientHeight) + border_y;
        info->ptMinTrackSize.x = std::max<LONG>(info->ptMinTrackSize.x, min_w);
        info->ptMinTrackSize.y = std::max<LONG>(info->ptMinTrackSize.y, min_h);
        return 0;
      }
      break;
    case WM_NCCALCSIZE:
      // The custom frame is the caption band's (phi_caption_band.h): it
      // claims the caption for the client area and leaves only the resize
      // borders non-client.
      if (self && self->caption_band_) {
        LRESULT result = 0;
        if (self->caption_band_->OnNcCalcSize(hwnd, wparam, lparam, &result)) {
          return result;
        }
      }
      break;
    case WM_NCHITTEST:
      if (self && self->caption_band_) {
        LRESULT result = 0;
        if (self->caption_band_->OnNcHitTest(hwnd, lparam, &result)) {
          return result;
        }
      }
      break;
    case WM_NCLBUTTONDOWN:
      if (self) {
        self->CloseExtensionPopup();
        self->DismissChromeTip();
      }
      // The band swallows presses on its own caption buttons; the action
      // fires on release over the same button, like the OS ones.
      if (self && self->caption_band_ &&
          self->caption_band_->OnNcLButtonDown(wparam)) {
        return 0;
      }
      break;
    case WM_NCRBUTTONDOWN:
    case WM_NCMBUTTONDOWN:
      if (self) {
        self->CloseExtensionPopup();
        self->DismissChromeTip();
        if (self->caption_band_) {
          self->caption_band_->OnCaptureLost();  // Disarms a caption press.
        }
      }
      break;
    case WM_CANCELMODE:
      if (self && self->caption_band_) {
        self->caption_band_->OnCaptureLost();
      }
      break;  // DefWindowProc releases the capture.
    case WM_NCLBUTTONUP:
      if (self && self->caption_band_ &&
          self->caption_band_->OnNcLButtonUp(hwnd, wparam)) {
        return 0;
      }
      break;
    case WM_NCRBUTTONUP:
      // Comfortable's band IS the tab strip, and its bare run past the last
      // tab drags the window (the band's HitTest serves HTCAPTION there), so a
      // right-click on it never reaches WM_RBUTTONUP — Windows would drop
      // the system menu instead of the strip's own. Claim it, the way the
      // Mac's tab-strip bar claims titlebar right-clicks
      // (shouldConsumeTitlebarEvent) to pop the same menu the sidebar shows.
      if (self && wparam == HTCAPTION) {
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ::ScreenToClient(hwnd, &pt);
        const Layout band = self->ComputeLayout();
        // The vertical layout's pip row too (its empty run opens the tab
        // area's menu, §3.5); the address row's run keeps the system menu.
        if ((band.horizontal_tabs && ::PtInRect(&band.title_bar, pt)) ||
            ::PtInRect(&band.spaces_strip, pt)) {
          self->OnChromeContextMenu(pt.x, pt.y);
          return 0;  // No WM_CONTEXTMENU, so no system menu behind ours.
        }
      }
      break;
    case WM_NCMOUSEMOVE:
      if (self) {
        POINT band_pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ::ScreenToClient(hwnd, &band_pt);
        // The caption buttons' hover and the hidden titles' reveal: most of
        // the band hit-tests as HTCAPTION, so the pointer crossing it
        // arrives here and not as WM_MOUSEMOVE.
        if (self->caption_band_) {
          self->caption_band_->OnNcMouseMove(wparam, band_pt);
        }
        self->UpdateChromeTip(self->CaptionTip(wparam));
        // The collapsed sidebar's hover trigger includes the left RESIZE
        // band — the outer frame pixels are non-client, so slamming the
        // cursor to the window edge (the Mac gesture) arrives here, not as
        // WM_MOUSEMOVE.
        if (self->sidebar_collapsed_ && !self->floating_expanded_ &&
            wparam == HTLEFT) {
          const Layout collapsed = self->ComputeLayout();
          if (!collapsed.horizontal_tabs &&
              band_pt.y > collapsed.title_bar.bottom) {
            self->ShowFloatingSidebar();
          }
        }
        // One leave notification when the mouse exits the frame entirely.
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE | TME_NONCLIENT,
                                 hwnd, 0};
        ::TrackMouseEvent(&track);
      }
      break;
    case WM_NCMOUSELEAVE:
      if (self && self->caption_band_) {
        self->caption_band_->OnNcMouseLeave();
      }
      if (self) {
        self->RetipAtCursor();
      }
      break;
    case WM_MOUSEMOVE:
      if (self) {
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        // The band's own hover: the caption buttons leave (a client move
        // means we are off them).
        if (self->caption_band_) {
          self->caption_band_->OnMouseMove(pt);
        }
        if (self->sidebar_resizing_) {
          const int dip = std::max(
              kSidebarWidthMin,
              std::min(static_cast<int>(pt.x) * 96 /
                           static_cast<int>(self->dpi_),
                       kSidebarWidthMax));
          if (dip != self->sidebar_dip_) {
            self->ResizeSidebar(dip);
          }
          return 0;
        }
        if ((wparam & MK_LBUTTON) && self->sidebar_->drag_armed()) {
          self->sidebar_->UpdateDrag(pt);
          self->UpdateChromeTip(std::wstring());
        } else {
          self->OnChromeMouseMove(pt);
          self->UpdateChromeTip(self->ChromeTipAt(pt));
        }
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&track);
      }
      break;
    case WM_MOUSELEAVE:
      if (self) {
        if (self->caption_band_) {
          self->caption_band_->OnMouseLeave();
        }
        self->ClearHover();
        self->RetipAtCursor();
      }
      break;
    case kMsgPhiSyncNativePages:
      if (self && !self->sidebar_resize_snapshot_) {
        self->native_pages_->Sync();
      }
      return 0;
    case kMsgPhiSlideCapture:
      if (self) {
        self->StartSlideCapture();
      }
      return 0;
    case kMsgPhiRebuildRows:
      if (self) {
        self->rebuild_posted_ = false;
        if (self->rows_dirty_) {
          // Once per turn, then only the bands whose rows changed repaint —
          // the caption band and the content stay put for a sidebar change.
          const BandSignature before = self->CurrentBandSignature();
          self->RebuildTabs();
          self->InvalidateChangedBands(before);
        }
      }
      return 0;
    case kMsgPhiDeferredClose:
      if (self) {
        self->RunDeferredClose();  // May destroy `self`.
      }
      return 0;
    case kMsgPhiSwitcherNext:
    case kMsgPhiSwitcherPrev:
      if (self) {
        PhiTabSwitcher::Cycle(self, msg == kMsgPhiSwitcherNext ? 1 : -1);
        return 0;
      }
      break;
    case kMsgPhiSwitcherCommit:
      PhiTabSwitcher::Commit();
      return 0;
    case PhiBrowserHost::kMsgRaiseCaps:
      if (self) {
        // Above the browser child; the bubble and caps go above it next.
        self->native_pages_->RaiseNtp();
        if (self->services_.sidebar) {
          // The reader panel covers the whole card, so it has to come up
          // before the caps that round it.
          self->services_.sidebar->RaiseReaderPanels(self->slot_.value);
        }
        self->browsers_->RaiseOverlays();
        // The Library covers the client area, caps included (the Mac's
        // panel sits over the whole content view): last, so the next raise
        // never drops it under the browser child.
        PhiLibrary::RaiseIfBoundTo(self);
      }
      return 0;
    case WM_INITMENUPOPUP:
      // A menu is about to drop down — refresh its dynamic rows and enable
      // states, the Win32 twin of the Mac's menuWillOpen rebuilds.
      if (self && self->app_menu_) {
        self->app_menu_->OnMenuOpening(reinterpret_cast<HMENU>(wparam));
        return 0;
      }
      break;
    case WM_MEASUREITEM:
      // The owner-drawn menu rows (phi_menu_paint.h): the app menu's and
      // every context menu's, all Fluent flyout rows.
      if (self && wparam == 0 &&
          PhiMenuOnMeasureItem(hwnd,
                               reinterpret_cast<MEASUREITEMSTRUCT*>(lparam))) {
        return TRUE;
      }
      break;
    case WM_DRAWITEM:
      if (self && wparam == 0 &&
          PhiMenuOnDrawItem(hwnd, *reinterpret_cast<DRAWITEMSTRUCT*>(lparam),
                            self->CurrentPalette())) {
        return TRUE;
      }
      break;
    case WM_MENUCHAR:
      // Owner-drawn rows carry no string the system can match a mnemonic
      // against, so the letter is resolved here.
      if (self) {
        if (const LRESULT pick = PhiMenuOnMenuChar(
                reinterpret_cast<HMENU>(lparam),
                static_cast<wchar_t>(LOWORD(wparam)))) {
          return pick;
        }
      }
      break;
    case WM_COMMAND:
      // lparam == 0 filters out child-control notifications (the address
      // field's EN_* arrive here too, carrying its HWND). HIWORD 0 is a
      // menu pick, 1 an accelerator — Win32's own convention, which the
      // keyboard service follows when it posts a chord (the reader's usage
      // metric tells the two apart; nothing else does).
      if (self && lparam == 0 && HIWORD(wparam) <= 1 &&
          LOWORD(wparam) == kCmdShowAppMenu) {
        self->ShowAppMenu();
        return 0;
      }
      if (self && lparam == 0 && HIWORD(wparam) <= 1 && self->app_menu_ &&
          self->app_menu_->OnCommand(LOWORD(wparam), HIWORD(wparam) == 1)) {
        return 0;
      }
      if (self && self->omnibox_->OnShellCommand(wparam, lparam)) {
        return 0;  // The address field's EN_CHANGE: autocomplete runs.
      }
      break;
    case WM_RBUTTONUP:
      if (self) {
        self->OnChromeContextMenu(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;
      }
      break;
    case WM_LBUTTONDOWN:
      if (self) {
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (self->InSidebarResizeBand(pt)) {
          self->CloseExtensionPopup();
          self->BeginSidebarResize();
          return 0;
        }
        self->OnChromeClick(pt.x, pt.y);
        return 0;
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_MBUTTONDOWN:
      if (self) {
        self->OnChromeMiddleClick(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_LBUTTONDBLCLK:
      if (self) {
        // A group header's double-click opens the group overview; anywhere
        // else the second press behaves like a plain click.
        const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        for (const GroupHeader& gh : self->sidebar_->group_headers()) {
          if (::PtInRect(&gh.bounds, pt)) {
            self->CloseExtensionPopup();
            PhiGroupOverview::Open(self, gh.id);
            return 0;
          }
        }
        self->OnChromeClick(pt.x, pt.y);
        return 0;
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_LBUTTONUP:
      if (self) {
        // A caption button's press holds the capture until this release.
        if (self->caption_band_ &&
            self->caption_band_->OnLButtonUp(
                {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) {
          return 0;
        }
        if (self->sidebar_resizing_) {
          self->EndSidebarResize();
          if (::GetCapture() == hwnd) {
            ::ReleaseCapture();
          }
          if (self->services_.prefs) {
            self->services_.prefs->RequestSidebarWidth(self->sidebar_dip_);
          }
          return 0;
        }
        self->sidebar_->CommitDrag();
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_CAPTURECHANGED:
      if (self) {
        if (self->caption_band_) {
          self->caption_band_->OnCaptureLost();
        }
        if (self->sidebar_resizing_) {
          self->EndSidebarResize();
          if (self->services_.prefs) {
            self->services_.prefs->RequestSidebarWidth(self->sidebar_dip_);
          }
        }
        self->sidebar_->AbortDrag();
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_SETCURSOR:
      if (self && LOWORD(lparam) == HTCLIENT) {
        POINT pt = {};
        ::GetCursorPos(&pt);
        ::ScreenToClient(hwnd, &pt);
        if (self->sidebar_resizing_ || self->InSidebarResizeBand(pt)) {
          ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEWE));
          return TRUE;
        }
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
      if (self) {
        self->DismissChromeTip();
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ::ScreenToClient(hwnd, &pt);
        // A tilt / two-finger sideways swipe steps comfortable's strip
        // window: rightward (positive) reveals later tabs.
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        const bool consumed =
            msg == WM_MOUSEWHEEL
                ? self->sidebar_->OnWheel(pt, delta)
                : self->layout_mode_ == PhiLayoutMode::kComfortable &&
                      self->sidebar_->OnWheel(pt, -delta);
        if (consumed) {
          if (self->rows_dirty_) {
            // The rows moved under a resting pointer: rebuild now and
            // re-hover, so the wash, the close box and the tab preview
            // follow the row under the cursor (the Spaces strip's step).
            self->FlushPendingRebuild();
            self->SpacesForgetHover();
            self->SpacesRehoverAtCursor();
          }
          return 0;
        }
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_KEYDOWN:
      // The sidebar's keyboard ring (F6) holds the shell's focus: its keys.
      if (self && self->sidebar_ &&
          self->sidebar_->OnKeyDown(static_cast<UINT>(wparam))) {
        return 0;
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_KILLFOCUS:
      if (self && self->sidebar_) {
        self->sidebar_->EndKeyboardFocus();
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_CONTEXTMENU:
      // Shift+F10 / the Menu key (lparam -1): the ring's row's menu.
      if (self && self->sidebar_ && lparam == -1) {
        POINT pt = {};
        if (self->sidebar_->KeyboardMenuPoint(&pt)) {
          self->OnChromeContextMenu(pt.x, pt.y);
          return 0;
        }
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case kMsgPhiCycleFocus:
      if (self && PhiLibrary::HwndIfBoundTo(self)) {
        PhiLibrary::Close();  // F6 leaves the Library for the chrome.
      }
      if (self && self->sidebar_) {
        // F6 / Shift+F6: address field -> sidebar -> page, round; the
        // sidebar's Esc goes straight to the page. A step the window cannot
        // take (no field, nothing in the sidebar) moves on to the next.
        int at = self->omnibox_->FieldHasFocus()      ? 0
                 : self->sidebar_->keyboard_focused() ? 1
                                                      : 2;
        for (int tries = 0; tries < 3; ++tries) {
          at = wparam == kPhiFocusPage     ? 2
               : wparam == kPhiFocusPrevious ? (at + 2) % 3
                                             : (at + 1) % 3;
          if (at == 1 && self->sidebar_->BeginKeyboardFocus()) {
            break;
          }
          self->sidebar_->EndKeyboardFocus();
          if (at == 0) {
            self->FocusAddressField();
            if (self->omnibox_->FieldHasFocus()) {
              break;
            }
          } else if (at == 2) {
            self->FocusActiveBrowser();
            break;
          }
        }
        return 0;
      }
      break;
    case WM_TIMER:
      if (self && self->tab_preview_ && self->tab_preview_->OnTimer(wparam)) {
        return 0;
      }
      if (self && wparam == kSplitChipTimer) {
        self->SplitChipTick();
        return 0;
      }
      if (wparam == kChromeTipTimer) {
        ::KillTimer(hwnd, kChromeTipTimer);
        if (self && !self->chrome_tip_.empty()) {
          PhiShowTooltipFor(hwnd, self->chrome_tip_, self->CurrentPalette());
        }
        return 0;
      }
      if (wparam == kDeferredCloseTimer) {
        ::KillTimer(hwnd, kDeferredCloseTimer);
        if (self) {
          self->RunDeferredClose();  // May destroy `self`.
        }
        return 0;
      }
      if (self && wparam == PhiSpacesStrip::kHoverCardTimer) {
        self->spaces_->PromoteHoverCard();  // Kills its own timer.
        return 0;
      }
      if (self && wparam == PhiOmniboxSession::kCopyLinkTimer) {
        self->omnibox_->RevertCopyLinkFlash();
        return 0;
      }
      if (self && wparam == PhiSidebarDragSession::kHoverExpandTimer) {
        ::KillTimer(hwnd, PhiSidebarDragSession::kHoverExpandTimer);
        self->sidebar_->OnHoverExpandTimer();
        return 0;
      }
      if (self && wparam == kSlideHoldTimer) {
        // The entering page has not come: ask once more, or slide without.
        if (self->slide_capture_tries_ < kSlideHoldTries) {
          self->StartSlideCapture();
        } else {
          self->ReleaseContentSlide();
        }
        return 0;
      }
      if (self && wparam == PhiSidebarDragSession::kSlideTimer) {
        // The reorder slide: repaint until the current leg settles. The
        // session owns the clock; the timer is the shell's because the
        // window is.
        if (!self->sidebar_->OnSlideTimer()) {
          ::KillTimer(hwnd, PhiSidebarDragSession::kSlideTimer);
        }
        return 0;
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_DWMCOMPOSITIONCHANGED:
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
      if (self) {
        const base::AutoReset<bool> docked(&self->floating_layout_, false);
        if (msg == WM_SETTINGCHANGE &&
            (wparam == SPI_SETNONCLIENTMETRICS ||
             (lparam && std::wstring_view(reinterpret_cast<const wchar_t*>(
                            lparam)) == L"WindowMetrics"))) {
          // The system font or text size moved at an unchanged DPI, which
          // EnsureFont alone would take for "nothing to do": drop its keys,
          // rebuild, and re-measure the menu rows with the new face.
          self->ui_font_dpi_ = 0;
          self->emoji_font_dpi_ = 0;
          self->caption_font_dpi_ = 0;
          self->chip_name_font_dpi_ = 0;
          self->EnsureFont();
          self->RefreshTabs();
          if (!self->app_menu_ || !self->app_menu_->open()) {
            self->RebuildAppMenu();
          }
        }
        self->UpdateSidebarBackdrop();
        self->PlaceContentChildren(self->ComputeWindowLayout());
        self->PushSplitViewStyle();
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_SIZE:
      if (self) {
        self->CloseExtensionPopup();
        // Band geometry changed under the slide.
        self->spaces_->CancelSwap();
        self->CancelContentSlide();
        if (self->reveal_veil_) {
          self->reveal_veil_->Lift();  // Pixels of the old geometry.
        }
        // A menu opened from the floating panel keeps the expanded-layout
        // override up for its loop; the shell's own geometry is the docked
        // one. (LayoutContents skips a minimized window by itself.)
        const base::AutoReset<bool> docked(&self->floating_layout_, false);
        self->LayoutContents();  // Re-seats the floating panel too.
        PhiNotificationCards::NoteOwnerMoved(hwnd);  // The card's corner.
      }
      return 0;
    case WM_MOVE:
      if (self) {
        self->CloseExtensionPopup();
        self->PositionFloatingPanel();  // The panel follows the shell.
        PhiNotificationCards::NoteOwnerMoved(hwnd);
        if (self->reveal_veil_) {
          self->reveal_veil_->Lift();  // An owned popup does not follow.
        }
      }
      break;
    case WM_DPICHANGED:
      // The window moved to a display at another scale. Windows hands us the
      // new DPI in wparam and the rect it wants in lparam; take both, rebuild
      // the fonts, then re-derive everything (the layout cache sees the new
      // DPI in its inputs). This and the constructor are the ONLY writers of
      // dpi_.
      if (self) {
        const base::AutoReset<bool> docked(&self->floating_layout_, false);
        if (const UINT dpi = LOWORD(wparam)) {
          self->dpi_ = dpi;
        }
        self->EnsureFont();
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        // A move this controller asked for (SetBounds, the watch frame)
        // keeps the size it asked for; Windows' rect is rescaled from it.
        if (suggested && !self->exact_bounds_pending_) {
          ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        self->LayoutContents();
        self->RefreshTabs();
        // Owner-drawn rows are measured once per menu (WM_MEASUREITEM is
        // cached by Windows): the tree is rebuilt to measure at this scale.
        if (!self->app_menu_ || !self->app_menu_->open()) {
          self->RebuildAppMenu();
        }
      }
      return 0;
    case WM_PRINTCLIENT: {
      // PrintWindow (used by screenshots/thumbnails) sends this instead of
      // WM_PAINT. Without handling it, a capture returns the LAST PAINTED
      // frame, not current state — so render fresh into the supplied DC.
      // The rows are painted as they are — a capture verb that needs them
      // current runs FlushPendingRebuild first; nothing rebuilds here.
      if (self) {
        const base::AutoReset<bool> docked(&self->floating_layout_, false);
        HDC dc = reinterpret_cast<HDC>(wparam);
        self->PaintChrome(dc, self->ComputeLayout(), self->CurrentPalette(),
                          /*include_child_controls=*/true);
      }
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (self) {
        // The shell paints its OWN (collapsed) geometry even while a menu
        // opened from the floating panel holds the expanded-layout override
        // for its loop — else the panel's layout is painted over the shell.
        const base::AutoReset<bool> docked(&self->floating_layout_, false);
        ++self->chrome_paints_;  // The floating panel follows (its tick).
        // The model rebuilds on its own posted turn (kMsgPhiRebuildRows),
        // never here: a paint recomposes what the last rebuild placed. A
        // dirty model with no rebuild queued (a stale flag from a path that
        // did not post) gets one now for the next turn.
        if (self->rows_dirty_ && !self->rebuild_posted_) {
          self->rebuild_posted_ =
              ::PostMessageW(hwnd, kMsgPhiRebuildRows, 0, 0) != 0;
        }
        // Compose off-screen and blit once. Painting straight to the window
        // shows the chrome rebuilding element by element — the visible blink
        // on every tab/Space change, which invalidates the whole window. The
        // backbuffer bitmap is cached; a full-window alloc per paint is real
        // work at 4K.
        RECT rc = {};
        ::GetClientRect(hwnd, &rc);
        HGDIOBJ old_bmp = nullptr;
        HDC mem = self->AcquireBackBuffer(dc, &old_bmp);
        if (mem) {
          // PaintChrome excludes the content rect (the browser child owns
          // it), so give those buffer pixels a defined color for the rare
          // blit with no child covering them (startup, degenerate states) —
          // the window ground, not white, or dark themes flash.
          HBRUSH fill = ::CreateSolidBrush(self->BackgroundFill());
          ::FillRect(mem, &rc, fill);
          ::DeleteObject(fill);
          // Freeze the animated theme for the whole frame. Alpha recovery
          // must remove the exact ground PaintChrome used: even a one-level
          // RGB drift can interpret the ground as opaque foreground and
          // flash the entire sidebar during a Space's theme ramp.
          const PhiPalette palette = self->CurrentPalette();
          self->ComposeFrame(mem, palette);
          self->PrepareBackdropPixels(palette);
          if (self->sidebar_resize_snapshot_) {
            self->PaintSidebarResizePreview(mem);
          }
          ::BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top,
                   ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left,
                   ps.rcPaint.top, SRCCOPY);
          ::SelectObject(mem, old_bmp);
        } else {
          self->PaintChrome(dc, self->ComputeLayout(), self->CurrentPalette(),
                            /*include_child_controls=*/false);
        }
        if (mem) {
          ::DeleteDC(mem);
        }
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      // Everything the shell owns is painted in WM_PAINT; a DefWindowProc
      // erase only flashes white under it.
      return 1;
    case WM_CLOSE:
      // Route through Chromium's normal close path (unload handlers, session
      // bookkeeping) for EVERY Space browser this slot hosts. When the last
      // Browser goes away the framework fires OnBrowserWindowClosing and the
      // coordinator destroys this controller — the shell window dies THEN, not
      // here. RequestClose may destroy `self`, so nothing touches it after.
      if (self) {
        self->RequestClose();
      } else {
        ::DestroyWindow(hwnd);
      }
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace phi_client

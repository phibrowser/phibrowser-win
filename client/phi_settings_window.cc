// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_settings_window.h"

#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <wrl/client.h>

// shlwapi.h (for AssocQueryStringW, the default-browser probe) defines
// StrCat as a macro; base/strings/strcat.h, reached through the preferences
// service's store, declares a function of that name and re-#defines it.
// Drop the Windows spelling — nothing here calls it.
#undef StrCat

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "phibrowser-win/client/phi_account_feedback.h"
#include "phibrowser-win/client/phi_account_profile.h"
#include "phibrowser-win/client/phi_agent_icons.h"
#include "phibrowser-win/client/phi_client_analytics.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_features.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_fluent.h"
#include "phibrowser-win/client/phi_folio.h"
#include "phibrowser-win/client/phi_sync_key_service.h"
#include "phibrowser-win/client/phi_version.h"
#include "phibrowser-win/client/phi_updater.h"
#include "phibrowser-win/client/phi_sidebar_view.h"
#include "phibrowser-win/client/phi_glyph_bitmap.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_icon_picker.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_log.h"
#include "phibrowser-win/client/phi_menu_paint.h"
#include "phibrowser-win/client/phi_popup_chrome.h"
#include "phibrowser-win/client/phi_preferences.h"
#include "phibrowser-win/client/phi_profile_service.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_skill_installer.h"
#include "phibrowser-win/client/phi_space_manager.h"
#include "phibrowser-win/client/phi_uia.h"
#include "phibrowser-win/client/phi_url_router.h"

namespace phi_client {

namespace {

constexpr wchar_t kClassName[] = L"PhiSettingsWindow";

}  // namespace

// Is Phi the browser Windows launches for an http: URL?
//
// The Mac asks LaunchServices for the handler of "http://example.com" and
// compares BUNDLE IDENTIFIERS. Windows has no bundle identity, so the same
// question is asked of the association store and answered by comparing
// EXECUTABLE PATHS — AssocQueryString resolves the user's choice through
// UserChoice and the ProgId's open command, which is the whole chain the
// shell itself walks.
//
// A registry read, not file I/O, so it does not need the raw-Win32 dance the
// rest of the client does to stay off the UI thread's blocking check.
bool PhiThisExeIsDefaultBrowser() {
  wchar_t handler[MAX_PATH * 2] = {};
  DWORD length = static_cast<DWORD>(std::size(handler));
  if (FAILED(::AssocQueryStringW(
          ASSOCF_IS_PROTOCOL | ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_EXECUTABLE,
          L"http", nullptr, handler, &length))) {
    return false;  // No handler at all, or none this API can name.
  }
  wchar_t self[MAX_PATH * 2] = {};
  if (!::GetModuleFileNameW(nullptr, self,
                            static_cast<DWORD>(std::size(self)))) {
    return false;
  }
  const bool ours =
      ::CompareStringOrdinal(handler, -1, self, -1, /*bIgnoreCase=*/TRUE) ==
      CSTR_EQUAL;
  // The one observable for a state the user cannot otherwise inspect: when
  // the row says "not your default browser", this says who is.
  // Redacted: the path runs through the user's profile folder.
  LOG(INFO) << "[PhiClient] http handler: "
            << PhiRedactPath(std::wstring(handler))
            << (ours ? " (ours)" : " (not ours)");
  return ours;
}

namespace {

// The Mac window: content is a FIXED 680x561 in every pane
// (each pane pins view.snp to that size); the toolbar rides above it.
// Windows Settings (docs/windows-native-ui.md §7): resizable, 1120 × 760
// by default, never under 760 × 560; a 280 navigation rail (48 under 900
// wide) and a page column 40 in from it.
constexpr int kWidth = 1120;
constexpr int kHeight = 760;
constexpr int kMinWidth = 760;
constexpr int kMinHeight = 560;
constexpr int kRailWidth = 280;
constexpr int kRailNarrow = 48;
constexpr int kRailBreakpoint = 900;
constexpr int kRailItemHeight = 36;
constexpr int kRailItemGap = 2;
// The rail's padding (design canvas "Settings window": 8 8 8 12), its
// account block (44 avatar; 8 above, 16 below) and the search field's
// side inset within that padding.
constexpr int kRailPadLeft = 12;
constexpr int kRailPadRight = 8;
constexpr int kRailAvatar = 44;
constexpr int kRailAccountHeight = 8 + kRailAvatar + 16;
constexpr int kRailSearchInset = 4;
// The page column: 20 from the rail, 40 from the window's right edge,
// 24 down from the caption.
constexpr int kContentLeftGap = 20;
constexpr int kContentRightPad = 40;
constexpr int kContentTop = 24;
constexpr int kContentMaxWidth = 1000;
constexpr int kPageTitleHeight = 36;   // 28 dip.
// The content column is one stack with 24 between its blocks (design node
// 9056:18105): the title, then the page.
constexpr int kPageTitleGap = 24;
// The custom caption strip: the OS caption is retired (WM_NCCALCSIZE) so
// the pane title can sit CENTERED with the Phi mark leading — the Mac
// window's look. Both the strip and its close button (Win11's 46dip
// metric) are PhiPopupChrome's now; kCaptionHeight is what this file
// still needs to know for kChromeTop.
constexpr int kCaptionHeight = kPhiCaptionHeightDip;
constexpr int kToolbarHeight = 0;  // The rail replaced the toolbar.
// Everything below the caption strip and the pane toolbar.
constexpr int kChromeTop = kCaptionHeight + kToolbarHeight;
// 20 between sections; the group header's 4 above / 6 below.
constexpr int kSectionGap = 20;
// Posted by the profile card's decode worker when a frame worth repainting
// for has landed.
constexpr UINT kMsgProfileCardFrame = WM_APP + 0x3A1;
// Posted by HandleKey: Space / Enter on the focused item, run after the
// key's WM_CHAR has been dispatched (ActivateFocus).
constexpr UINT kMsgFocusActivate = WM_APP + 0x3A2;
constexpr int kTitleToCard = 6;
constexpr int kCardPadding = 16;   // Inner horizontal padding of a card.
constexpr int kRowPadding = 12;    // Vertical padding around a row's content.
// A card row is at least 68 dip tall (padding included): the design's
// min-height, so a one-line row and a two-line row read as one family.
constexpr int kRowMinHeight = 68 - 2 * kRowPadding;
constexpr int kSectionTitleHeight = 24;  // 14/600 group headers, 4 above.
constexpr int kHintHeight = 16;          // 12 dip descriptions.
constexpr int kRowLabelHeight = 19;      // 14 dip labels.
constexpr int kCardRadius = 4;
constexpr int kRadius = 4;
// Theme swatches: a 22pt dot in a 26pt ring frame, 2pt ring when selected,
// caption (11pt) below only while selected (ThemeSwatchView).
constexpr int kSwatchDot = 22;  // Selection ring = dot + 4.
// The slider (Sidebar transparency): a 4 dip track, an 18pt white knob.
constexpr int kSliderHeight = 20;
constexpr int kSliderGeneral = 200;
constexpr int kSliderReadout = 48;  // The "NN%" after the track.
constexpr int kSliderKnob = 18;
// Layout / appearance / new-tab picture cards: 122x72 image, r5, 2pt accent
// ring when selected, 11pt caption 10 below, 16 apart (GeneralSttingCardView).
constexpr int kImageCardW = 122;
constexpr int kImageCardH = 72;
constexpr int kImageCardGap = 16;
constexpr int kImageCardBlock = 72 + 10 + 15;
// The Layout picker's two halves (design "Appearance"): a 110 preview,
// radius 6, then the radio + label row 8 below; 16 between the halves.
constexpr int kLayoutPreviewH = 110;
constexpr int kLayoutCardBlock = 110 + 8 + 20;
constexpr int kLayoutCardGap = 16;
// Switches: the Mac's .mini switch is about 26x15.
constexpr int kToggleWidth = 40;
constexpr int kToggleHeight = 20;
// Expander rows (windows-native-ui.md §7.1 / §7.2): a 68 dip header row,
// stacked in one card.
constexpr int kExpanderHeight = 68;
// The expander bodies' two columns: the label at the row text's x, the
// control 136 further in; 260 wide fields.
constexpr int kDetailLabelWidth = 136;
constexpr int kDetailFieldWidth = 260;
constexpr int kDetailRowPitch = 44;
// The rail's "Find a setting" field (§7): 32 high under the caption.
constexpr int kRailSearchHeight = 32;
constexpr int kAboutMark = 48;
// The gap between the General pane's single-profile detail cards.
constexpr int kDetailCardGap = 16;
// The Profiles trailing pill: inner width 110, H10/V5, primary@0.06, r6.
constexpr int kPillWidth = 160;
// A pill grows to fit its current choice — a translated "System default"
// ("Predeterminado del sistema") outgrew the fixed 160 — up to this, then
// ellipsizes.
constexpr int kPillMaxWidth = 280;
constexpr int kPillHeight = 32;
// A row's leading icon (the design's 20 dip glyph, 16 from the text it
// leads), so an icon-led row's text column starts 36 in; the rows grouped
// under such a header are indented to match.
constexpr int kChipSize = 20;
constexpr int kChipGap = 16;
constexpr int kChipIndent = kChipSize + kChipGap;
// Settings › Spaces (design node 9056:18105), dip. The card: 431 inside
// its 1 px border, the Space list 300 wide, a 1 px divider, the editor.
constexpr int kSpacesCardInner = 431;
constexpr int kSpacesListWidth = 300;
constexpr int kSpacesStackGap = 24;
// The list: 16 down, rows of 36 on a 2 gap — the 24 grip at 8, 10 to the
// row body (radius 5, 6 in), 4 off the divider; a 1 px rule 230 wide,
// right-aligned, between rows; a 24 bar of 24 glyph buttons under a rule.
constexpr int kSpaceListTop = 16;
constexpr int kSpaceRowHeight = 36;
constexpr int kSpaceRowGap = 2;
constexpr int kSpaceGrip = 24;
constexpr int kSpaceGripLeft = 8;
constexpr int kSpaceGripGap = 10;
constexpr int kSpaceRowRight = 4;
constexpr int kSpaceRowInset = 6;
constexpr int kSpaceRowRadius = 5;
constexpr int kSpaceIcon = 20;
constexpr int kSpaceIconGap = 6;
constexpr int kSpaceRule = 230;
constexpr int kSpaceBar = 24;
constexpr int kSpaceBarButton = 24;
// The row's profile pill: 120 x 24, radius 6, label 12 in, the up/down
// chevrons in a trailing 24 cell.
constexpr int kSpacePillWidth = 120;
constexpr int kSpacePillHeight = 24;
// The editor: 14 padding, 16 between its two wells; the appearance well's
// row is 42 with 12 side padding, its "Color" control a 20 dot and a 20
// chevron cell 2 apart (ThemeColorSettingsRow).
constexpr int kEditorPad = 14;
constexpr int kEditorGap = 16;
constexpr int kWellPadX = 12;
constexpr int kWellRow = 42;
constexpr int kWellRadius = 4;
constexpr int kThemeColorDot = 20;
constexpr int kThemeColorGap = 2;
constexpr int kThemeColorControl = 2 * kThemeColorDot + kThemeColorGap;
// The picker well: 12 top and bottom; the Icon | Emoji pill (26, 2 in,
// 4 between, segments 16 to the side), the 231 x 24 search pill 12 under
// it, then the grid 16 under that: 8 columns of 24 glyphs spread across
// the well less 24 a side, 6 between rows.
constexpr int kWellPadY = 12;
constexpr int kSegmentHeight = 26;
constexpr int kSegmentInset = 2;
constexpr int kSegmentGap = 4;
constexpr int kSegmentPadX = 16;
constexpr int kIconSearchWidth = 231;
constexpr int kIconSearchHeight = 24;
constexpr int kGridTopGap = 16;
constexpr int kGridColumns = 8;
constexpr int kGridCell = 24;
constexpr int kGridRowGap = 6;
constexpr int kGridSide = 24;
// The URL Rules row: 68 at least, 16 padding, 16 between its parts.
constexpr int kUrlRowMinHeight = 68;
constexpr int kUrlRowPad = 16;
constexpr int kUrlRowIcon = 20;
// One wheel notch (120) scrolls the list or the grid three rows.
constexpr int kWheelPerRow = 40;

// The Mac's panes(), in order (AppController+Settings.panes(): Account,
// General, Profiles, Spaces, Navigation, Phi & AI, Shortcuts, Developer —
// there is NO Phi Link pane; IMChannels exists in source but is never
// mounted). The Account pane is titled with the user's name ("You" when
// signed out).
// Glyphs are MDL2 codepoints standing in for the Mac's 32pt template PDFs
// (~14-18pt of ink); Developer mirrors the Mac's SF "hammer".
struct SectionDef {
  const char* id;
  const char* label;  // A string-catalog key (PhiStr), never the text.
  wchar_t glyph;
};
// The pages (windows-native-ui.md §7), in rail order. The ids are the
// remote surface's (`account` stays `account` — impl log D19).
constexpr std::array<SectionDef, 12> kSections = {{
    {"account", "settings.account.displayName.fallback", 0xE77B},  // Contact
    {"sync", "settings.sync.title", 0xE895},                       // Sync
    {"general", "settings.navigation.generalTitle", 0xE713},       // Settings
    {"appearance", "settings.general.appearance.sectionTitle",
     0xE790},                                                      // Color
    {"spaces", "settings.navigation.spacesTitle", 0xE8A9},         // ViewAll
    {"profiles", "settings.navigation.profilesTitle", 0xE716},     // People
    {"navigation", "settings.navigation.navigationsTitle", 0xE71B},  // Link
    {"ai", "settings.navigation.aiTitle", 0xE734},  // FavoriteStar
    {"shortcuts", "settings.navigation.shortcutsTitle", 0xE765},  // Keyboard
    {"advanced", "settings.navigation.advancedTitle", 0xE9F5},  // Processing
    // DeveloperTools.
    {"developer", "settings.navigation.developerTitle", 0xEC7A},
    {"about", "settings.navigation.aboutTitle", 0xE946},  // Info
}};

// A theme's display name ("Mint"), the Mac's themes.theme.<id>.name.
std::wstring ThemeDisplayName(const std::string& id) {
  const std::string key = "themes.theme." + id + ".name";
  return PhiStr(key.c_str());
}

PhiSettingsWindow* g_instance = nullptr;

// The shape helpers take the TRUE corner radius (a capsule is half the
// height) and render antialiased — see phi_dwrite.h. Their GDI forebears
// took the corner ellipse's diameter, so a bar asked for "radius 4" came
// out at 2, and every curve was stair-stepped.
void FillRounded(HDC dc, const RECT& r, COLORREF fill, int radius) {
  PhiFillRoundRectAA(dc, r, radius, fill);
}

void StrokeRounded(HDC dc, const RECT& r, COLORREF fill, COLORREF border,
                   int radius, int width) {
  PhiFillRoundRectAA(dc, r, radius, fill);
  PhiStrokeRoundRectAA(dc, r, radius, border, width);
}

void Ellipse(HDC dc, const RECT& r, COLORREF fill, COLORREF border,
             int border_width) {
  PhiFillEllipseAA(dc, r, fill);
  if (border_width > 0) {
    PhiStrokeEllipseAA(dc, r, border, border_width);
  }
}

// A dot filled with `colors` top to bottom — a Space palette's Oklab run
// (PhiOklabGradient, as the Mac's ThemeColorSettingsRow indicator), flat
// for one colour — with an antialiased edge.
void FillGradientDot(HDC dc,
                     const RECT& r,
                     const std::vector<COLORREF>& colors) {
  const int width = r.right - r.left;
  const int height = r.bottom - r.top;
  if (colors.empty() || width <= 0 || height <= 0) {
    return;
  }
  if (colors.size() == 1) {
    PhiFillEllipseAA(dc, r, colors.front());
    return;
  }
  std::array<PhiColor, 3> stops = {};
  const int count = static_cast<int>(std::min<size_t>(colors.size(), 3));
  for (size_t i = 0; i < stops.size(); ++i) {
    stops[i] = colors[std::min(i, static_cast<size_t>(count - 1))];
  }
  std::vector<unsigned char> bgra(static_cast<size_t>(width) * height * 4);
  const double cx = width / 2.0;
  const double cy = height / 2.0;
  const double radius = std::min(width, height) / 2.0;
  for (int y = 0; y < height; ++y) {
    const PhiColor color = PhiOklabGradient(
        stops, count, height > 1 ? y / (height - 1.0) : 0.0);
    for (int x = 0; x < width; ++x) {
      const double coverage = std::clamp(
          radius + 0.5 - std::hypot(x + 0.5 - cx, y + 0.5 - cy), 0.0, 1.0);
      const int alpha = static_cast<int>(coverage * 255 + 0.5);
      const size_t at = (static_cast<size_t>(y) * width + x) * 4;
      bgra[at] = static_cast<unsigned char>(GetBValue(color) * alpha / 255);
      bgra[at + 1] = static_cast<unsigned char>(GetGValue(color) * alpha / 255);
      bgra[at + 2] = static_cast<unsigned char>(GetRValue(color) * alpha / 255);
      bgra[at + 3] = static_cast<unsigned char>(alpha);
    }
  }
  PhiBlendBgra(dc, r, bgra.data(), width, height);
}

// `over` at alpha/255 on `base` — the Mac's percentage overlays, opaque.
COLORREF Blend(COLORREF over, COLORREF base, int alpha) {
  auto mix = [&](int a, int b) { return (a * alpha + b * (255 - alpha)) / 255; };
  return RGB(mix(GetRValue(over), GetRValue(base)),
             mix(GetGValue(over), GetGValue(base)),
             mix(GetBValue(over), GetBValue(base)));
}

// A rounded border with NO fill — for outlines over already-painted content
// (StrokeRounded fills, which erased the slider gradient it framed).
void OutlineRounded(HDC dc, const RECT& r, COLORREF border, int radius,
                    int width) {
  PhiStrokeRoundRectAA(dc, r, radius, border, width);
}

// The Mac's NSOpenPanel (directories only): the modern IFileOpenDialog in
// folder-picking mode. Empty when cancelled. The browser UI thread is
// already a COM STA, so no CoInitialize here.
std::wstring PickFolder(HWND owner, const wchar_t* title) {
  Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
  if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
    return std::wstring();
  }
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dialog->SetTitle(title);
  if (FAILED(dialog->Show(owner))) {
    return std::wstring();  // Cancelled.
  }
  Microsoft::WRL::ComPtr<IShellItem> item;
  if (FAILED(dialog->GetResult(&item))) {
    return std::wstring();
  }
  wchar_t* path = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) {
    return std::wstring();
  }
  std::wstring result = path;
  ::CoTaskMemFree(path);
  return result;
}

// A folder's last path component — the Mac pill shows only that.
std::wstring FolderLeaf(const std::string& path) {
  std::string trimmed = path;
  while (!trimmed.empty() &&
         (trimmed.back() == '\\' || trimmed.back() == '/')) {
    trimmed.pop_back();
  }
  const size_t cut = trimmed.find_last_of("\\/");
  return base::UTF8ToWide(cut == std::string::npos ? trimmed
                                                   : trimmed.substr(cut + 1));
}

// A profile's Folio Folder pill: its override's leaf, or "Same as General".
std::wstring FolioOverrideLabel(const PhiPreferences& prefs,
                                const std::string& profile_id) {
  const std::string path = prefs.FolioFolderOverride(profile_id);
  return path.empty() ? std::wstring(
                            PhiStr("settings.profiles.folio.sameAsGeneral"))
                      : FolderLeaf(path);
}

}  // namespace

PhiSettingsWindow::~PhiSettingsWindow() {
  host_->RemoveSettingsObserver(this);
  Destroy();  // The base clears GWLP_USERDATA at WM_NCDESTROY.
  if (measure_dc_) {
    ::DeleteDC(measure_dc_);
    measure_dc_ = nullptr;
  }
  for (HFONT font :
       {font_, title_font_, small_font_, hint_font_, emoji_font_, mdl2_font_,
        body_font_, page_title_font_}) {
    if (font) {
      ::DeleteObject(font);
    }
  }
  for (const auto& [size, font] : icon_fonts_) {
    (void)size;
    ::DeleteObject(font);
  }
}

PhiSettingsWindow::PhiSettingsWindow(PhiSettingsHost* host)
    : host_(host),
      prefs_(&host->Preferences()),
      spaces_(&host->SpaceList()),
      profiles_(&host->ProfileList()),
      account_(&host->Account()) {
  // Subscribed for the life of the object (the destructor unsubscribes),
  // whether or not the window below comes up.
  host_->AddSettingsObserver(this);
}

bool PhiSettingsWindow::OpenWindow() {
  ClassSpec spec;
  spec.name = kClassName;
  spec.style = CS_HREDRAW | CS_VREDRAW;
  // Phi's own icon in the taskbar and alt-tab — not the stock one.
  spec.icon = PhiAppIcon(32);
  spec.icon_small = PhiAppIcon(16);

  Options options;
  // A Windows Settings window: min / max / close, resizable (§7).
  options.caption_buttons =
      kPhiCaptionClose | kPhiCaptionMinimize | kPhiCaptionMaximize;
  options.dim_when_inactive = false;
  // The design's title bar: "Settings" at x = 12, no mark leading it.
  options.show_mark = false;
  // Every control here is drawn, so the dialog manager has nothing to move
  // the focus between: the window runs its own focus model over the drawn
  // items (HandleKey), Tab included.
  options.dialog_keys = kPhiDialogNone;

  const RECT bounds = {CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT + 100,
                       CW_USEDEFAULT + 100};
  if (!CreateSecondary(spec, 0,
                       WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                           WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
                       bounds, nullptr, PhiStr("settings.windowTitle"),
                       options)) {
    LOG(ERROR) << "[PhiClient] settings CreateWindowEx failed err="
               << ::GetLastError();
    return false;
  }
  profile_poll_ = NewTimer();
  sync_poll_ = NewTimer();
  card_timer_ = NewTimer();
  profile_card_.SetNotify(hwnd(), kMsgProfileCardFrame);
  profile_card_.SetCacheDir(prefs_->user_data_dir() + L"\\PhiCardArtwork");
  // Content 680x561 under the caption strip and toolbar band — fixed,
  // non-resizable. WM_NCCALCSIZE keeps only the thin side/bottom borders,
  // so the window size is the content plus exactly those.
  const int border_x = ::GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi()) +
                       ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi());
  const int border_y = ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi()) +
                       ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi());
  // The last size the user left it at (dip), else the §7 default.
  int width = kWidth;
  int height = kHeight;
  if (prefs_->SettingsWindowSize(&width, &height)) {
    width = std::max(kMinWidth, width);
    height = std::max(kMinHeight, height);
  }
  ::SetWindowPos(hwnd(), nullptr, 0, 0, Scaled(width) + 2 * border_x,
                 Scaled(kChromeTop + height) + border_y,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
  // The remembered size (and the 1120 x 760 default) came from whatever
  // monitor the window was last on: kept inside THIS monitor's work area,
  // and cut down to it, so a small or scaled display never opens it with
  // its bottom under the taskbar.
  FitToWorkArea();
  EnsureFont();
  SyncTitle();
  return true;
}

void PhiSettingsWindow::EnsureFont() {
  if (font_ && font_dpi_ == dpi()) {
    return;
  }
  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  if (!::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                    &metrics, 0, dpi())) {
    return;
  }
  // The Mac's ladder: 13pt rows, 12pt section titles, 11pt hints.
  auto make = [&](int size, int weight) {
    LOGFONTW lf = metrics.lfMessageFont;
    lf.lfHeight = -Scaled(size);
    lf.lfWeight = weight;
    return ::CreateFontIndirectW(&lf);
  };
  auto swap = [](HFONT* slot, HFONT built) {
    if (!built) {
      return;
    }
    if (*slot) {
      ::DeleteObject(*slot);
    }
    *slot = built;
  };
  // The §2.2 ramp: body 14, group headers 14/600, descriptions 12, the
  // page title 28/600 (Segoe UI Variable through PhiFluentFont).
  swap(&font_, PhiFluentFont(dpi(), 14));
  swap(&title_font_, PhiFluentFont(dpi(), 28, FW_SEMIBOLD));
  swap(&small_font_, PhiFluentFont(dpi(), 14, FW_SEMIBOLD));
  swap(&hint_font_, PhiFluentFont(dpi(), 12));
  // The design's 13 px body (Settings › Spaces) and the page title,
  // Segoe UI Variable Bold 28 (node 9056:18105).
  swap(&body_font_, PhiFluentFont(dpi(), 13));
  swap(&page_title_font_, PhiFluentFont(dpi(), 28, FW_BOLD));
  (void)make;
  LOGFONTW emoji = metrics.lfMessageFont;
  const std::wstring face = L"Segoe UI Emoji";
  std::fill(std::begin(emoji.lfFaceName), std::end(emoji.lfFaceName), L'\0');
  std::copy_n(face.begin(),
              std::min(face.size(), std::size(emoji.lfFaceName) - 1),
              std::begin(emoji.lfFaceName));
  emoji.lfHeight = -Scaled(14);
  swap(&emoji_font_, ::CreateFontIndirectW(&emoji));
  LOGFONTW mdl2 = metrics.lfMessageFont;
  const std::wstring mdl2_face = PhiIconFaceName();
  std::fill(std::begin(mdl2.lfFaceName), std::end(mdl2.lfFaceName), L'\0');
  std::copy_n(mdl2_face.begin(),
              std::min(mdl2_face.size(), std::size(mdl2.lfFaceName) - 1),
              std::begin(mdl2.lfFaceName));
  mdl2.lfHeight = -Scaled(14);
  swap(&mdl2_font_, ::CreateFontIndirectW(&mdl2));
  // The caption glyph font is the frame's now (PhiPopupGlyphFont).
  for (const auto& [size, cached] : icon_fonts_) {
    (void)size;
    ::DeleteObject(cached);
  }
  icon_fonts_.clear();
  font_dpi_ = dpi();
}

HFONT PhiSettingsWindow::IconFont(int size) {
  for (const auto& [cached_size, font] : icon_fonts_) {
    if (cached_size == size) {
      return font;
    }
  }
  LOGFONTW lf = {};
  if (!mdl2_font_ || !::GetObjectW(mdl2_font_, sizeof(lf), &lf)) {
    return mdl2_font_;
  }
  lf.lfHeight = -Scaled(size);
  HFONT built = ::CreateFontIndirectW(&lf);
  if (!built) {
    return mdl2_font_;
  }
  icon_fonts_.emplace_back(size, built);
  return built;
}

void PhiSettingsWindow::SyncTitle() {
  // Windows Settings: the window is "Settings"; the page title is in the
  // page (§7). Re-applied on every settings change too, so a switch of the
  // display language retitles the open window — only when it differs, as
  // WM_SETTEXT repaints the band.
  const wchar_t* title = PhiStr("settings.windowTitle");
  wchar_t current[128] = {};
  ::GetWindowTextW(hwnd(), current, static_cast<int>(std::size(current)));
  if (std::wstring(current) != title) {
    ::SetWindowTextW(hwnd(), title);
  }
}

// static
void PhiSettingsWindow::Open(PhiSettingsHost* host) {
  if (!g_instance || !g_instance->alive()) {
    delete g_instance;
    g_instance = new PhiSettingsWindow(host);
    if (!g_instance->OpenWindow()) {
      delete g_instance;
      g_instance = nullptr;
    }
  }
  if (g_instance) {
    // A minimized window stays minimized under SW_SHOW: restore it, then
    // bring it to the front.
    ::ShowWindow(g_instance->hwnd(),
                 ::IsIconic(g_instance->hwnd()) ? SW_RESTORE : SW_SHOW);
    ::SetForegroundWindow(g_instance->hwnd());
    g_instance->ScheduleRebuild();
  }
}

// static
HWND PhiSettingsWindow::Handle() {
  return g_instance && g_instance->alive() ? g_instance->hwnd() : nullptr;
}

// static
void PhiSettingsWindow::Refresh() {
  if (Handle()) {
    g_instance->ScheduleRebuild();
  }
}

void PhiSettingsWindow::OnSettingsChanged(PhiSettingsChange /*what*/) {
  // Every pane reads the whole host, so one rebuild covers any domain.
  // This is THE reason the rebuild could come off the paint path: phase
  // 4 gave the window a real change signal, so WM_PAINT no longer has to
  // assume the model moved under it.
  if (alive()) {
    SyncTitle();  // The display language may have moved.
    ScheduleRebuild();
  }
}

void PhiSettingsWindow::ScheduleRebuild() {
  if (!alive()) {
    return;
  }
  Rebuild();
  Invalidate();
  // ClearHits() drops the base's hover index, so a rebuild fired by a click
  // would leave the control still under the pointer un-lit until the mouse
  // moved. Re-run the hover through THE path — the wndproc — rather than a
  // copy of it. Never during a slider drag: that move handler rebuilds, and
  // feeding it its own message would not terminate.
  POINT pt = {};
  if (!dragging_ && !reorder_drag_ && ::GetCursorPos(&pt) &&
      ::WindowFromPoint(pt) == hwnd()) {
    ::ScreenToClient(hwnd(), &pt);
    ::SendMessageW(hwnd(), WM_MOUSEMOVE, 0,
                   MAKELPARAM(static_cast<WORD>(pt.x),
                              static_cast<WORD>(pt.y)));
  }
}

// static
std::string PhiSettingsWindow::CurrentSection() {
  return g_instance ? g_instance->section_ : std::string();
}

// static
bool PhiSettingsWindow::ShowSection(const std::string& name) {
  for (const SectionDef& section : kSections) {
    if (name != section.id) {
      continue;
    }
    // Developer exists only in developer mode, exactly as on Mac.
    if (name == "developer" && g_instance &&
        !g_instance->prefs_->DeveloperMode()) {
      return false;
    }
    // Phi & AI is not mounted while the AI suite is off.
    if (name == "ai" && !kPhiAiEnabled) {
      return false;
    }
    if (g_instance) {
      g_instance->section_ = name;
      g_instance->scroll_ = 0;
      g_instance->SyncTitle();
      Refresh();
    }
    return true;
  }
  return false;
}

// static
void PhiSettingsWindow::ShowSpacePicker(const std::string& space_id) {
  if (!g_instance) {
    return;
  }
  g_instance->section_ = "spaces";
  g_instance->SelectSpace(space_id, /*reveal=*/true);
  g_instance->rail_search_.clear();
  g_instance->search_filter_.clear();
  g_instance->scroll_ = 0;
  g_instance->SyncTitle();
  Refresh();
}

// --- Sync (docs/user-data-parity.md S3) ------------------------------------------
// The Mac's DevicesSettingView: the account, the key layer's state, the
// devices with their pending join requests, and recovery / removal. The
// sync STATUS card waits for the framework's profile sync status.

namespace {

// "3:41 PM": a deadline in the user's short time format.
std::wstring ShortTime(int64_t unix_ms) {
  const ULONGLONG ticks = static_cast<ULONGLONG>(unix_ms) * 10000ULL +
                          116444736000000000ULL;  // 1601 epoch, 100 ns
  FILETIME utc = {static_cast<DWORD>(ticks & 0xFFFFFFFF),
                  static_cast<DWORD>(ticks >> 32)};
  FILETIME local = {};
  SYSTEMTIME st = {};
  if (!::FileTimeToLocalFileTime(&utc, &local) ||
      !::FileTimeToSystemTime(&local, &st)) {
    return std::wstring();
  }
  wchar_t buffer[64] = {};
  const int n = ::GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS,
                                  &st, nullptr, buffer, 64);
  return n > 1 ? std::wstring(buffer, static_cast<size_t>(n - 1))
               : std::wstring();
}

}  // namespace

void PhiSettingsWindow::RefreshSyncDevices(bool force) {
  PhiSyncKeyService* keys = host_ ? host_->SyncKeys() : nullptr;
  if (!keys || sync_refreshing_) {
    return;
  }
  const uint64_t now = ::GetTickCount64();
  if (!force && sync_loaded_ && now - sync_refreshed_tick_ < 3000) {
    return;
  }
  sync_refreshing_ = true;
  std::weak_ptr<int> alive = sync_alive_;
  keys->Refresh([this, alive](PhiSyncKeyService::DeviceList list) {
    if (alive.expired()) {
      return;
    }
    sync_refreshing_ = false;
    sync_loaded_ = true;
    sync_refreshed_tick_ = ::GetTickCount64();
    sync_list_error_ = list.status.ok() ? std::string()
                                        : keys::KeyStatusName(list.status);
    if (list.status.ok()) {
      sync_devices_ = std::move(list.devices);
      sync_pending_ = std::move(list.pending);
    }
    ScheduleRebuild();
  });
}

namespace {

const wchar_t* SyncPhaseTitle(sync::SummaryPhase phase) {
  switch (phase) {
    case sync::SummaryPhase::kNotStarted:
      return PhiStr("sync.setup.notStarted");
    case sync::SummaryPhase::kChecking:
      return PhiStr("sync.status.checking");
    case sync::SummaryPhase::kInitialSync:
      return PhiStr("sync.status.initial");
    case sync::SummaryPhase::kSyncing:
      return PhiStr("sync.status.syncing");
    case sync::SummaryPhase::kUpToDate:
      return PhiStr("sync.status.upToDate");
    case sync::SummaryPhase::kOffline:
      return PhiStr("sync.status.offline");
    case sync::SummaryPhase::kNeedsAttention:
      break;
  }
  return PhiStr("sync.status.needsAttention");
}

}  // namespace

// The Mac's DevicesSettingView statusSection: one summary line over the
// native engine ("Phi data") and every profile's Chromium sync, then each
// context's own phase.
void PhiSettingsWindow::BuildSyncStatus(int* y) {
  PhiSyncKeyService* keys = host_ ? host_->SyncKeys() : nullptr;
  if (!keys) {
    return;
  }
  const PhiSyncKeyService::Snapshot& snap = keys->snapshot();
  std::vector<sync::ContextStatus> contexts;
  std::set<std::string> required;
  // "Phi data" first, then the profiles in the framework's order. A profile
  // that is not loaded has no running sync to report and is left out (the
  // Mac lists every profile; Windows loads them lazily, and the status
  // query must never load one).
  std::vector<std::string> rows;
  const std::vector<PhiProfileSummary> profiles = profiles_->Profiles();
  if (!profiles.empty()) {
    required.insert("phi");
    rows.push_back("phi");
  }
  if (snap.native_sync_active) {
    sync::ContextStatus native;
    native.id = "phi";
    native.phase = sync::ContextPhaseFromName(
                       sync::SyncPhaseName(snap.native_sync_status.phase))
                       .value_or(sync::SummaryPhase::kChecking);
    native.last_success_ms = snap.native_sync_status.last_success_ms;
    native.revision = snap.native_sync_status.revision;
    contexts.push_back(native);
  }
  for (const PhiProfileSummary& profile : profiles) {
    if (std::optional<sync::ContextStatus> status =
            profiles_->ProfileSyncStatus(profile.id)) {
      required.insert(profile.id);
      rows.push_back(profile.id);
      contexts.push_back(*status);
    }
  }
  const sync::StatusSummary summary =
      sync::ReduceStatus(snap.paired, required, contexts);

  std::wstring hint;
  if (summary.last_success_ms) {
    // One sentence with the date and the time as its arguments: a
    // language orders "label: date time" its own way.
    hint = PhiStrF(
        "settings.sync.status.lastSuccessAt",
        {PhiFormatDate(*summary.last_success_ms, PhiDateStyle::kShortDate),
         PhiFormatDate(*summary.last_success_ms, PhiDateStyle::kShortTime)});
  }
  if (summary.phase == sync::SummaryPhase::kNeedsAttention) {
    hint += (hint.empty() ? L"" : L"\n") +
            std::wstring(PhiStr("sync.status.partialFailure"));
  }
  BeginSection(y, nullptr);
  AddRow(y, SyncPhaseTitle(summary.phase), 0, 0,
         hint.empty() ? nullptr : hint.c_str());
  for (const std::string& id : rows) {
    AddDividerRow(y);
    sync::SummaryPhase phase = sync::SummaryPhase::kChecking;
    for (const sync::ContextStatus& context : contexts) {
      if (context.id == id) {
        phase = context.phase;
      }
    }
    std::wstring title;
    if (id == "phi") {
      title = PhiStr("sync.status.phiData");
    } else {
      const std::string name = profiles_->ProfileDisplayName(id);
      title = name.empty() ? std::wstring(PhiStr("sync.status.profile"))
                           : base::UTF8ToWide(name);
    }
    AddRow(y, title.c_str(), 0, 0, SyncPhaseTitle(phase));
  }
  EndSection(y);
}

void PhiSettingsWindow::BuildSync(int* y) {
  PhiSyncKeyService* keys = host_ ? host_->SyncKeys() : nullptr;
  const PhiAuth& auth = account_->auth();
  auto button_in = [&](const RECT& area, const wchar_t* text,
                       const char* action, bool primary, bool danger,
                       bool enabled, const std::string& payload) {
    Item button;
    button.kind = ItemKind::kButton;
    button.text = text;
    button.selected = primary;
    button.danger = danger;
    button.enabled = enabled;
    button.action = action;
    button.payload = payload;
    button.bounds = area;
    items_.push_back(std::move(button));
  };
  auto message_row = [&](const wchar_t* title, const wchar_t* hint,
                         const wchar_t* button, const char* action) {
    BeginSection(y, nullptr);
    const RECT area = AddRow(y, title, kPillHeight, button ? 230 : 0, hint);
    if (button) {
      button_in(area, button, action, true, false, true, std::string());
    }
    EndSection(y);
  };

  if (!keys) {
    // Signed out, Guest Mode, or an onboarding still running.
    message_row(PhiStr("settings.sync.signInPrompt"), nullptr,
                PhiStr("settings.sync.signIn"), "signin");
    return;
  }
  if (auth.signed_in() && !auth.user().email.empty()) {
    AddHint(y, base::UTF8ToWide(auth.user().email).c_str());
  }
  const PhiSyncKeyService::Snapshot& snap = keys->snapshot();
  if (snap.native_sync_needs_reconfiguration) {
    message_row(PhiStr("sync.reconfigure.title"),
                PhiStr("sync.reconfigure.explanation"),
                PhiStr("sync.reconfigure.title"), "syncreconfigure");
    if (!sync_action_error_.empty()) {
      AddHint(y, sync_action_error_.c_str());
    }
    return;
  }
  switch (snap.unlock) {
    case PhiSyncKeyService::Unlock::kUnknown:
    case PhiSyncKeyService::Unlock::kWorking:
      message_row(PhiStr("settings.sync.loading"), nullptr, nullptr, "");
      return;
    case PhiSyncKeyService::Unlock::kNotSignedIn:
      message_row(PhiStr("settings.sync.signInPrompt"), nullptr,
                  PhiStr("settings.sync.signIn"), "signin");
      return;
    case PhiSyncKeyService::Unlock::kFailed:
      message_row(PhiStr("settings.sync.loadFailed"), nullptr,
                  PhiStr("settings.sync.retry"), "syncretry");
      return;
    case PhiSyncKeyService::Unlock::kNeedsJoin:
      message_row(PhiStr("settings.sync.notSetUp"), nullptr,
                  PhiStr("settings.sync.setUp"), "syncsetup");
      return;
    case PhiSyncKeyService::Unlock::kUnlocked:
      break;
  }

  // Unlocked: paired (the status card) or not, then the account's devices.
  if (snap.paired) {
    BuildSyncStatus(y);
  } else {
    message_row(PhiStr("settings.sync.notPaired"),
                PhiStr("settings.sync.notStarted"),
                PhiStr("settings.sync.continueSetup"), "syncsetup");
  }

  RefreshSyncDevices(/*force=*/false);
  if (section_ == "sync" && ::IsWindowVisible(hwnd()) && !sync_poll_.running()) {
    sync_poll_.Start(3000);
  }

  BeginSection(y, PhiStr("settings.sync.devices"));
  bool first = true;
  auto divider = [&]() {
    if (!first) {
      AddDividerRow(y);
    }
    first = false;
  };
  for (const keys::PendingApproval& request : sync_pending_) {
    divider();
    const std::wstring title = PhiStrF(
        "settings.sync.requestFrom",
        {base::UTF8ToWide(request.name), base::UTF8ToWide(request.platform)});
    const std::wstring hint =
        PhiStrF("settings.sync.expires", {ShortTime(request.deadline_ms)}) +
        L"\n" +
        PhiStrF("settings.sync.verifyCode",
                {base::UTF8ToWide(request.verification_code)});
    const RECT area = AddRow(y, title.c_str(), kPillHeight, 200, hint.c_str());
    const int mid = area.left + (area.right - area.left) / 2;
    button_in({area.left, area.top, mid - Scaled(4), area.bottom},
              PhiStr("settings.sync.approve"), "syncapprove", true, false,
              true, request.id);
    button_in({mid + Scaled(4), area.top, area.right, area.bottom},
              PhiStr("settings.sync.deny"), "syncdeny", false, false, true,
              request.id);
  }
  for (const keys::AccountDevice& device : sync_devices_) {
    if (device.status != "active" || device.revoked_at_ms) {
      continue;
    }
    divider();
    const bool mine = device.device_key_id == snap.device_key_id;
    const std::wstring name = base::UTF8ToWide(device.name);
    const std::wstring platform = base::UTF8ToWide(device.platform);
    const RECT area = AddRow(y, name.c_str(), 24, mine ? 90 : 0,
                             platform.c_str());
    if (mine) {
      Item badge;
      badge.kind = ItemKind::kBadge;
      badge.text = PhiStr("settings.sync.thisDevice");
      badge.bounds = area;
      items_.push_back(std::move(badge));
    }
  }
  if (first && !sync_loaded_) {
    AddRow(y, PhiStr("settings.sync.loading"), 0, 0, nullptr);
  }
  if (!sync_list_error_.empty() || !sync_action_error_.empty()) {
    Item error;
    error.kind = ItemKind::kHint;
    error.danger = true;
    error.wrap = true;
    error.text = sync_action_error_.empty()
                     ? std::wstring(PhiStr("settings.sync.error.requestFailed"))
                     : sync_action_error_;
    const int left = content_left_ + Scaled(kCardPadding);
    const int right = content_right_ - Scaled(kCardPadding);
    const int h = WrappedHeight(hint_font_, error.text.c_str(), right - left);
    error.bounds = {left, *y, right, *y + h};
    items_.push_back(std::move(error));
    *y += h + Scaled(kRowPadding);
  }
  EndSection(y);

  // Removal (the Mac's removeDeviceSection, phibrowser-mac 055df916): ONE
  // row in one of two variants. When the loaded device list shows this PC as
  // the account's only active device, or after the server's 409 last_device
  // refusal of a removal sent from a stale list, the row is "Reset sync"
  // with a destructive button that also deletes the account's sync data;
  // otherwise it is "Remove this PC from sync". The list is a hint: the
  // server re-checks at reset time, and its 409 not_last_device shows a
  // fixed error and reloads the list, which flips the row back.
  int active_devices = 0;
  bool only_this_device = false;
  for (const keys::AccountDevice& device : sync_devices_) {
    if (device.status == "active" && !device.revoked_at_ms) {
      ++active_devices;
      only_this_device = device.device_key_id == snap.device_key_id;
    }
  }
  const bool shows_reset =
      sync_last_device_ || (active_devices == 1 && only_this_device);
  BeginSection(y, PhiStr("sync.removal.sectionTitle"));
  const wchar_t* row_title = shows_reset
                                 ? PhiStr("sync.removal.resetRowTitle")
                                 : PhiStr("sync.removal.rowTitle.windows");
  const wchar_t* row_hint =
      shows_reset ? PhiStr("sync.removal.resetRowDescription.windows")
                  : PhiStr("sync.removal.rowDescription.windows");
  const RECT area = AddRow(y, row_title, kPillHeight, 120, row_hint);
  if (shows_reset) {
    button_in(area, PhiStr("sync.removal.resetRowAction"), "syncreset", false,
              true, !sync_removing_, std::string());
  } else {
    button_in(area, PhiStr("sync.removal.rowAction"), "syncremove", false,
              true, !sync_removing_, std::string());
  }
  // A removal or reset failure, under the row: red like every other error on
  // this pane (the Mac's removalErrorNote).
  if (!sync_remove_note_.empty()) {
    Item error;
    error.kind = ItemKind::kHint;
    error.danger = true;
    error.wrap = true;
    error.text = sync_remove_note_;
    const int left = content_left_ + Scaled(kCardPadding);
    const int right = content_right_ - Scaled(kCardPadding);
    const int h = WrappedHeight(hint_font_, error.text.c_str(), right - left);
    error.bounds = {left, *y, right, *y + h};
    items_.push_back(std::move(error));
    *y += h + Scaled(kRowPadding);
  }
  EndSection(y);
}

// --- Section plumbing --------------------------------------------------------

void PhiSettingsWindow::BeginSection(int* y, const wchar_t* title) {
  // Where the section began, heading included, so a filter that keeps
  // none of its rows can take the whole thing back out.
  if (heading_pending_) {
    section_start_ = heading_start_;
    section_start_y_ = heading_start_y_;
    heading_pending_ = false;
  } else {
    section_start_ = items_.size();
    section_start_y_ = *y;
  }
  if (title && title[0]) {
    Item heading;
    heading.kind = ItemKind::kSectionTitle;
    heading.text = title;
    heading.bounds = {content_left_, *y, content_right_,
                      *y + Scaled(kSectionTitleHeight)};
    items_.push_back(std::move(heading));
    *y += Scaled(kSectionTitleHeight) + Scaled(kTitleToCard);
  }
  // The card is emitted FIRST so it paints behind its rows; EndSection sizes
  // it around whatever was added. The card OPENS at `*y` and CLOSES at the
  // `*y` a row leaves behind, so its padding is kRowPadding on both edges —
  // it used to open four pixels above its first row and close sixteen below
  // its last, which made every card top-tight and bottom-loose and left a
  // card whose only line was a hint (Developer's two empty lists) with the
  // text riding its top border.
  Item card;
  card.kind = ItemKind::kCard;
  card.bounds = {content_left_, *y, content_right_, 0};
  items_.push_back(std::move(card));
  open_card_ = items_.size() - 1;
  card_rows_ = 0;
  *y += Scaled(kRowPadding);
}

void PhiSettingsWindow::EndSection(int* y) {
  if (!search_filter_.empty()) {
    if (card_rows_ == 0) {
      // The filter kept nothing here: the card, its title and heading go.
      items_.resize(std::min(items_.size(), section_start_));
      *y = section_start_y_;
      card_rows_ = 0;
      return;
    }
    // A divider that ended up trailing the last kept row — behind the
    // empty-rect controls of the rows the filter dropped after it.
    while (items_.size() > open_card_ + 1) {
      const Item& last = items_.back();
      const bool empty = last.bounds.bottom == 0 && last.bounds.top == 0;
      if (!empty && last.kind != ItemKind::kDivider) {
        break;
      }
      items_.pop_back();
    }
  }
  items_[open_card_].bounds.bottom = *y;
  card_rows_ = 0;
  *y += Scaled(kSectionGap);
}

bool PhiSettingsWindow::SearchMatches(const wchar_t* title,
                                      const wchar_t* hint) const {
  if (search_filter_.empty()) {
    return true;
  }
  const auto contains = [this](const wchar_t* text) {
    if (!text || !*text) {
      return false;
    }
    std::wstring folded(text);
    for (wchar_t& c : folded) {
      c = static_cast<wchar_t>(::towlower(c));
    }
    return folded.find(search_filter_) != std::wstring::npos;
  };
  return contains(title) || contains(hint);
}

int PhiSettingsWindow::WrappedHeight(HFONT font, const wchar_t* text,
                                     int width) const {
  if (!font || !text || !*text || width <= 0) {
    return 0;
  }
  if (!measure_dc_) {
    measure_dc_ = ::CreateCompatibleDC(nullptr);
    if (!measure_dc_) {
      return 0;
    }
  }
  HGDIOBJ previous = ::SelectObject(measure_dc_, font);
  RECT box = {0, 0, width, 0};
  ::DrawTextW(measure_dc_, text, -1, &box,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  ::SelectObject(measure_dc_, previous);
  return box.bottom - box.top;
}

void PhiSettingsWindow::AddHint(int* y, const wchar_t* text) {
  if (!search_filter_.empty()) {
    return;  // A page's loose captions have no row to hang from here.
  }
  // A page's loose paragraph runs flush with the cards (the design's
  // footer note under the profiles).
  const int left = content_left_;
  const int right = content_right_;
  const int height = std::max(Scaled(kHintHeight),
                              WrappedHeight(hint_font_, text, right - left));
  Item hint;
  hint.kind = ItemKind::kHint;
  hint.text = text;
  hint.wrap = true;
  hint.bounds = {left, *y, right, *y + height};
  items_.push_back(std::move(hint));
  *y += height + Scaled(6);
}

RECT PhiSettingsWindow::AddRow(int* y, const wchar_t* title,
                               int control_height, int control_width,
                               const wchar_t* hint, int text_indent,
                               wchar_t chip_glyph, COLORREF chip_color,
                               int row_padding) {
  // The rail search: a row whose title and description carry none of the
  // typed text is not built at all. Its caller's control then lands on an
  // empty rect, which neither paints (it clips out under the caption) nor
  // publishes a hit.
  last_row_skipped_ = !SearchMatches(title, hint);
  if (last_row_skipped_) {
    return RECT{};
  }
  ++filter_hits_;
  const int padding = row_padding < 0 ? kRowPadding : row_padding;
  const int gutter = (content_left_ + Scaled(kCardPadding));
  const int left = gutter + Scaled(text_indent);
  const int right = (content_right_ - Scaled(kCardPadding));
  const int text_right = right - Scaled(control_width) - Scaled(12);
  const int text_width = std::max(1, text_right - left);
  // The Mac's row is ONE VStack(spacing: 4): the 13pt title over its 11pt
  // description, both wrapping, with the trailing control centred on the
  // pair. Measuring here is what stops a long line being chopped with an
  // ellipsis ("…so each profile manages its ow…").
  const int title_height = std::max(
      Scaled(kRowLabelHeight), WrappedHeight(font_, title, text_width));
  const int hint_height =
      hint && *hint ? WrappedHeight(hint_font_, hint, text_width) : 0;
  const int text_height =
      title_height + (hint_height > 0 ? Scaled(2) + hint_height : 0);
  // The design's rows are at least 68 tall, padding included, unless the
  // caller asks for a tighter one (an expander body's rows).
  const int min_height = row_padding < 0 ? Scaled(kRowMinHeight) : 0;
  const int row_height =
      std::max({text_height, Scaled(control_height), min_height});
  const int text_top = *y + (row_height - text_height) / 2;
  if (chip_glyph) {
    // The row's leading 20 dip glyph, centred on the row like the design's
    // (align-items: center).
    Item chip;
    chip.kind = ItemKind::kIconChip;
    chip.text = std::wstring(1, chip_glyph);
    chip.swatch = chip_color;
    const int chip_top = *y + (row_height - Scaled(kChipSize)) / 2;
    chip.bounds = {gutter, chip_top, gutter + Scaled(kChipSize),
                   chip_top + Scaled(kChipSize)};
    items_.push_back(std::move(chip));
  }
  Item label;
  label.kind = ItemKind::kRowLabel;
  label.text = title;
  label.wrap = title_height > Scaled(kRowLabelHeight);
  label.bounds = {left, text_top, text_right, text_top + title_height};
  items_.push_back(std::move(label));
  if (hint_height > 0) {
    Item note;
    note.kind = ItemKind::kHint;
    note.text = hint;
    note.wrap = true;
    note.bounds = {left, text_top + title_height + Scaled(2), text_right,
                   text_top + text_height};
    items_.push_back(std::move(note));
  }
  // The control is TRAILING, as on Mac (label, Spacer, accessory).
  RECT control = {right - Scaled(control_width),
                  *y + (row_height - Scaled(control_height)) / 2, right,
                  *y + (row_height + Scaled(control_height)) / 2};
  *y += row_height + Scaled(padding);
  ++card_rows_;
  return control;
}

int PhiSettingsWindow::PillWidthFor(const std::wstring& text) const {
  // The pill's own chrome: 10 before the value, the 28 chevron lane after
  // it (Paint, kPopup), and a little air.
  constexpr int kPillChrome = 10 + 28 + 4;
  int text_px = 0;
  if (font_ && !text.empty()) {
    if (!measure_dc_) {
      measure_dc_ = ::CreateCompatibleDC(nullptr);
    }
    if (measure_dc_) {
      HGDIOBJ previous = ::SelectObject(measure_dc_, font_);
      RECT box = {0, 0, 0, 0};
      ::DrawTextW(measure_dc_, text.c_str(), -1, &box,
                  DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
      ::SelectObject(measure_dc_, previous);
      text_px = box.right - box.left;
    }
  }
  const int text_dip = ::MulDiv(text_px, 96, static_cast<int>(dpi()));
  return std::max(kPillWidth, std::min(kPillMaxWidth, text_dip + kPillChrome));
}

void PhiSettingsWindow::AddPopupRow(int* y, const wchar_t* title,
                                    const std::wstring& value,
                                    const char* action, const wchar_t* hint,
                                    int text_indent, wchar_t chip_glyph) {
  const RECT area = AddRow(y, title, kPillHeight, PillWidthFor(value), hint,
                           text_indent, chip_glyph);
  Item pill;
  pill.kind = ItemKind::kPopup;
  pill.text = value;
  pill.action = action;
  pill.bounds = area;
  items_.push_back(std::move(pill));
}

void PhiSettingsWindow::AddChips(const RECT& area,
                                 const std::vector<std::wstring>& labels,
                                 int selected_index,
                                 const std::string& action) {
  // Retained for popup-style option groups: one popup pill showing the
  // current choice; clicking cycles to the next option (the Mac uses a menu).
  Item pill;
  pill.kind = ItemKind::kPopup;
  if (selected_index >= 0 &&
      selected_index < static_cast<int>(labels.size())) {
    pill.text = labels[static_cast<size_t>(selected_index)];
  }
  pill.value = selected_index;
  pill.action = action;
  pill.bounds = area;
  items_.push_back(std::move(pill));
}

std::vector<COLORREF> PhiSettingsWindow::SpaceThemeDots(
    const PhiSpaceSummary& space) const {
  // indicatorColors: a palette's resolved colours, opaque; else the
  // theme's overlay colour.
  const bool dark = prefs_->Palette().dark;
  std::vector<COLORREF> colors;
  const model::Space* record = spaces_->Find(space.id);
  if (record && record->palette) {
    for (int i = 0; i < record->palette->color_count(); ++i) {
      colors.push_back(record->palette->ResolvedColor(i, dark));
    }
  } else {
    const PhiTheme& theme = PhiThemeById(
        space.theme_id.empty() ? prefs_->ThemeId() : space.theme_id);
    colors.push_back(dark ? theme.dark.overlay : theme.light.overlay);
  }
  return colors;
}

// --- Panes -------------------------------------------------------------------

void PhiSettingsWindow::BuildGeneral(int* y) {
  BeginSection(y, PhiStr("settings.general.language.sectionTitle"));
  {
    // The row was a dead chip until the string catalog existed. Now it is the
    // language picker: "System default" leads because it is the default and
    // the only choice that follows a later OS change, and the rest are
    // endonyms, so someone hunting for their own language does not first have
    // to read the language the app happens to be in.
    AddPopupRow(y, PhiStr("settings.displayLanguage"),
                prefs_->LanguagePreference().empty()
                    ? PhiStr("settings.languageSystemDefault")
                    : PhiLocaleDisplayName(prefs_->LanguagePreference()),
                "language", PhiStr("settings.languageHint"), kChipIndent,
                0xE774);
  }
  EndSection(y);


  BeginSection(y, PhiStr("settings.general.browsing.sectionTitle"));
  {
    // "New tab behavior": two picture cards. New Tab Page opens the native
    // new-tab page (phi_new_tab_page.cc, the Mac's NewTabViewController);
    // Omnibox focuses the address field and the tab is born on commit.
    const RECT area = AddRow(y, PhiStr("settings.general.newTabShortcut.title"),
                             kImageCardBlock,
                             2 * kImageCardW + kImageCardGap, nullptr,
                             kChipIndent, 0xE710);
    const struct {
      const char* title;  // A string-catalog key.
      PhiNewTabBehavior value;
      const char* sketch;
      bool enabled;
    } kCards[] = {
        {"settings.general.newTabShortcut.newTabPageOption",
         PhiNewTabBehavior::kNewTabPage, "ntp", true},
        {"settings.general.newTabShortcut.addressBarOption",
         PhiNewTabBehavior::kOmnibox, "omnibox", true},
    };
    int i = 0;
    for (const auto& definition : kCards) {
      Item card;
      card.kind = ItemKind::kImageCard;
      card.text = PhiStr(definition.title);
      card.payload = definition.sketch;
      card.selected = prefs_->new_tab_behavior() == definition.value;
      card.enabled = definition.enabled;
      card.value = static_cast<int>(definition.value);
      card.action = "newtab";
      const int x = area.left + i * Scaled(kImageCardW + kImageCardGap);
      card.bounds = {x, area.top, x + Scaled(kImageCardW),
                     area.top + Scaled(kImageCardBlock)};
      items_.push_back(std::move(card));
      ++i;
    }
  }
  AddDividerRow(y);
  AddToggleRow(y, PhiStr("settings.general.restoreLastSession.toggle"),
               prefs_->RestoreLastSession(),
               /*enabled=*/true, "restoresession",
               PhiStr(prefs_->RestoreLastSession()
                          ? "settings.general.restoreLastSession.enabledHint"
                          : "settings.advanced.restoreLastSession."
                            "disabledHint"),
               kChipIndent, 0xE823);
  AddDividerRow(y);
  {
    // Telemetry consent (docs/architecture-review.md §3.9): the ONLY switch
    // for the analytics pipeline. Off by default, and deliberately out of
    // reach of agentSpace.settings.set. Windows-only, so it stays here
    // while the Mac's other browsing rows moved to Advanced.
    AddToggleRow(y, PhiStr("settings.general.analytics.toggle"),
                 prefs_->AnalyticsEnabled(), /*enabled=*/true, "analytics",
                 PhiStr("settings.general.analytics.hint"), kChipIndent,
                 0xE9D9);
  }
  EndSection(y);

  // Folio (docs/folio-port.md), the Mac's SaveForLaterSectionView: where
  // saves land and, behind the auto-trigger switch, the site-action opt-in.
  if (PhiFolio::Enabled()) {
    BeginSection(y, PhiStr("settings.general.folio.sectionTitle"));
    AddPopupRow(y, PhiStr("settings.general.folio.folderTitle"),
                FolderLeaf(prefs_->FolioGlobalFolder()), "foliofolder",
                PhiStr("settings.general.folio.folderHint"), kChipIndent,
                0xE8B7);  // Folder.
    if (kPhiFolioAutoTriggerEnabled) {
      AddDividerRow(y);
      AddToggleRow(y, PhiStr("settings.general.folio.autoSaveToggle"),
                   prefs_->FolioAutoSaveOnSiteActions(), /*enabled=*/true,
                   "folioautosave",
                   PhiStr("settings.general.folio.autoSaveHintWindows"),
                   kChipIndent, 0xEB51);  // Heart.
      // The sites are the extension's rule corpus, so their switches live
      // on its page; this row is the way in.
      AddDividerRow(y);
      Item row;
      row.kind = ItemKind::kListRow;
      row.action = "foliosites";
      row.enabled = prefs_->FolioAutoSaveOnSiteActions();
      items_.push_back(std::move(row));
      const size_t row_index = items_.size() - 1;
      const RECT area =
          AddRow(y, PhiStr("settings.general.folio.autoSaveSites"),
                 kRowLabelHeight, 20, nullptr, kChipIndent);
      Item chevron;
      chevron.kind = ItemKind::kIcon;
      chevron.text = std::wstring(1, wchar_t(0xE76C));  // ChevronRight
      chevron.value = 12;
      chevron.enabled = prefs_->FolioAutoSaveOnSiteActions();
      chevron.bounds = area;
      items_.push_back(std::move(chevron));
      items_[row_index].bounds = {content_left_ + Scaled(4),
                                  area.top - Scaled(6),
                                  content_right_ - Scaled(4),
                                  area.bottom + Scaled(6)};
    }
    EndSection(y);
  }

  // Profile: with several profiles just the jump link; with one, the Mac
  // INLINES the profile detail (search engine, downloads, data links).
  if (profiles_->Profiles().size() > 1) {
    BeginSection(y, PhiStr("settings.general.profile.sectionTitle"));
    const RECT area =
        AddRow(y, PhiStr("settings.general.profile.multipleProfilesHint"),
               kRowLabelHeight, 170);
    Item link;
    link.kind = ItemKind::kLink;
    link.text = PhiStr("settings.general.profile.openProfilesSettingsLink");
    link.action = "goprofiles";
    link.bounds = area;
    items_.push_back(std::move(link));
    EndSection(y);
  } else if (search_filter_.empty()) {
    // (Its detail rows are not filterable: the results page skips them.)
    Item heading;
    heading.kind = ItemKind::kSectionTitle;
    heading.text = PhiStr("settings.general.profile.sectionTitle");
    heading.bounds = {content_left_, *y, content_right_,
                      *y + Scaled(kSectionTitleHeight)};
    items_.push_back(std::move(heading));
    *y += Scaled(kSectionTitleHeight) + Scaled(kTitleToCard);
    const std::vector<PhiProfileSummary> profiles = profiles_->Profiles();
    const std::string profile_id =
        profiles.empty() ? std::string() : profiles.front().id;
    // Without the Folio Folder row: General's own Folio section above
    // already shows the folder (Mac d509f24d).
    BuildProfileDetail(y, content_left_, content_right_, profile_id,
                       /*show_folio_folder=*/false);
    *y += Scaled(kSectionGap) - Scaled(kDetailCardGap);
  }
}

// The Mac's AdvancedSettingsView (6d1f7b12): the browsing rows and the
// Developer-mode switch that used to sit in General, as four untitled
// cards — the page rows, the browsing rows, Developer mode, then the
// browser-settings link.
void PhiSettingsWindow::BuildAppearance(int* y) {
  const bool locked = spaces_->Summaries().size() > 1;

  BeginSection(y, PhiStr("appMenu.layout"));
  // The two cards' captions are what the results page searches (a haystack,
  // never shown).
  const std::wstring layout_words =
      std::wstring(PhiStr("appMenu.horizontalTabs")) + L" " +
      PhiStr("appMenu.verticalTabs");
  if (SearchMatches(PhiStr("appMenu.layout"), layout_words.c_str())) {
    // The design's picker: the card split in two, each half a 110 preview
    // of the layout (2 dip accent ring when chosen) over its radio and
    // label. Balanced is "Vertical tabs", Comfortable "Horizontal tabs".
    ++filter_hits_;
    const int left = content_left_ + Scaled(kCardPadding);
    const int right = content_right_ - Scaled(kCardPadding);
    const int half = (right - left - Scaled(kLayoutCardGap)) / 2;
    const struct {
      const char* title;  // A string-catalog key.
      PhiLayoutMode mode;
      const char* sketch;
    } kCards[] = {
        {"appMenu.horizontalTabs", PhiLayoutMode::kComfortable, "comfortable"},
        {"appMenu.verticalTabs", PhiLayoutMode::kBalanced, "balanced"},
    };
    int i = 0;
    for (const auto& definition : kCards) {
      Item card;
      card.kind = ItemKind::kLayoutCard;
      card.text = PhiStr(definition.title);
      card.payload = definition.sketch;
      card.selected = prefs_->LayoutMode() == definition.mode;
      card.value = static_cast<int>(definition.mode);
      card.action = "layout";
      const int x = left + i * (half + Scaled(kLayoutCardGap));
      card.bounds = {x, *y, x + half, *y + Scaled(kLayoutCardBlock)};
      items_.push_back(std::move(card));
      ++i;
    }
    *y += Scaled(kLayoutCardBlock) + Scaled(kRowPadding);
    ++card_rows_;
  }
  EndSection(y);

  BeginSection(y, PhiStr("settings.general.theme.colorTitle"));
  {
    // Color appearance — a ComboBox: Use system setting / Light / Dark.
    AddPopupRow(y, PhiStr("settings.general.appearance.title"),
                PhiStr(prefs_->Appearance() == PhiAppearance::kLight
                           ? "themes.appearance.lightOption"
                       : prefs_->Appearance() == PhiAppearance::kDark
                           ? "themes.appearance.darkOption"
                           : "settings.general.appearance.useSystemOption"),
                "appearancemenu", PhiStr("settings.general.appearance.hint"),
                kChipIndent, 0xE7B3);
  }
  AddDividerRow(y);
  if (locked) {
    // Space color: with several Spaces each one sets its own colours in the
    // Spaces pane, so this row only says so and links there.
    const RECT area =
        AddRow(y, PhiStr("settings.general.theme.spaceColorTitle"),
               kRowLabelHeight, 170,
               PhiStr("settings.general.theme.perSpaceHint"), kChipIndent,
               0xE790);
    Item link;
    link.kind = ItemKind::kLink;
    link.text =
        PhiStr("settings.general.pinnedTabScope.openSpacesSettingsLink");
    link.action = "gospaces";
    link.bounds = area;
    items_.push_back(std::move(link));
  } else {
    // Space color: the one Space's "Color ›" button (the Spaces pane's
    // ThemeColorSettingsRow) opens its theme editor — colours, harmony and
    // saturation in one place, replacing the theme dots and the slider.
    const std::vector<PhiSpaceSummary> spaces = spaces_->Summaries();
    const RECT area =
        AddRow(y, PhiStr("settings.general.theme.spaceColorTitle"),
               kPillHeight, kThemeColorControl + 2 * kWellPadX, nullptr,
               kChipIndent, 0xE790);
    if (!spaces.empty() && !last_row_skipped_) {
      Item row;
      row.kind = ItemKind::kThemeColor;
      row.colors = SpaceThemeDots(spaces.front());
      row.action = "spacecolor";
      row.payload = spaces.front().id;
      row.a11y = PhiStr("settings.general.theme.spaceColorTitle");
      row.bounds = area;
      items_.push_back(std::move(row));
    }
  }
  AddDividerRow(y);
  {
    // Sidebar transparency, 0-100 %: the desktop through the theme tint,
    // the value read out after the track.
    const RECT area =
        AddRow(y, PhiStr("settings.appearance.sidebarTransparency.title"),
               kSliderHeight, kSliderGeneral + kSliderReadout,
               PhiStr("settings.appearance.sidebarTransparency.hint"),
               kChipIndent, 0xE8A0);
    if (!last_row_skipped_) {
      const int value = prefs_->SidebarTransparency();
      Item slider;
      slider.kind = ItemKind::kSliderTrack;
      slider.value = value;
      slider.action = "sidebartransparency";
      slider.bounds = area;
      slider.bounds.right -= Scaled(kSliderReadout);
      items_.push_back(std::move(slider));
      Item readout;
      readout.kind = ItemKind::kHint;
      readout.text = PhiFormatNumber(static_cast<long long>(value)) + L"%";
      readout.bounds = {area.right - Scaled(kSliderReadout) + Scaled(12),
                        area.top, area.right, area.bottom};
      items_.push_back(std::move(readout));
    }
  }
  // The Mac's "Apply theme to text selection on web pages" is not drawn:
  // Windows has no web-selection tint for it to switch.
  EndSection(y);

  BeginSection(y, PhiStr("sidebar.section.tabs"));
  AddToggleRow(y, PhiStr("settings.advanced.tabPreview.showToggle"),
               prefs_->ShowTabPreviews(),
               /*enabled=*/true, "tabpreviews", nullptr, kChipIndent, 0xE7C4);
  AddDividerRow(y);
  AddToggleRow(y, PhiStr("settings.advanced.openTabIndicators.showToggle"),
               prefs_->ShowOpenTabIndicators(),
               /*enabled=*/true, "openindicators",
               PhiStr("settings.appearance.openTabIndicators.hint"),
               kChipIndent, 0xE73E);
  AddDividerRow(y);
  AddToggleRow(y, PhiStr("settings.appearance.dimUnloadedTabIcons.toggle"),
               prefs_->DimUnloadedTabIcons(),
               /*enabled=*/true, "dimunloaded",
               PhiStr("settings.appearance.dimUnloadedTabIcons.hint"),
               kChipIndent, 0xE7B3);
  AddDividerRow(y);
  AddToggleRow(y, PhiStr("settings.general.addressBar.showFullURLToggle"),
               prefs_->AlwaysShowFullUrl(),
               /*enabled=*/true, "fullurl", nullptr, kChipIndent, 0xE774);
  EndSection(y);
}

void PhiSettingsWindow::BuildAbout(int* y) {
  BeginSection(y, nullptr);
  {
    // The brand mark at 48 leading the name and version (§7 About).
    const std::wstring version =
        PhiStrF("settings.about.version", {PHI_WIN_VERSION_FULL_WIDE});
    RECT area = AddRow(y, PhiStr("app.phiBrowser"), kAboutMark, 160,
                       version.c_str(), kAboutMark + 16);
    if (area.right > area.left) {
      Item mark;
      mark.kind = ItemKind::kAppMark;
      mark.bounds = {content_left_ + Scaled(kCardPadding), area.top,
                     content_left_ + Scaled(kCardPadding) + Scaled(kAboutMark),
                     area.top + Scaled(kAboutMark)};
      items_.push_back(std::move(mark));
      const int mid = (area.top + area.bottom) / 2;
      area.top = mid - Scaled(kPillHeight) / 2;
      area.bottom = area.top + Scaled(kPillHeight);
      Item button;
      button.kind = ItemKind::kButton;
      button.text = PhiStr("settings.about.checkForUpdate");
      button.enabled = PhiUpdater::Available();
      button.action = "checkupdate";
      button.bounds = area;
      items_.push_back(std::move(button));
    }

  }
  EndSection(y);

  // "What's new" opens chrome://whats-new in a tab, as the Mac's Help menu
  // item does; the whole row is the button (behind its label, like the
  // Advanced page's browser-settings row). "Documentation" was a dead row
  // with no page behind it on either client, and is gone.
  BeginSection(y, nullptr);
  {
    Item row;
    row.kind = ItemKind::kListRow;
    row.action = "openpage";
    row.payload = "chrome://whats-new";
    items_.push_back(std::move(row));
    const size_t row_index = items_.size() - 1;
    const RECT area =
        AddRow(y, PhiStr("appMenu.whatsNew"), kRowLabelHeight, 20);
    Item chevron;
    chevron.kind = ItemKind::kIcon;
    chevron.text = std::wstring(1, wchar_t(0xE76C));  // ChevronRight
    chevron.value = 12;
    chevron.bounds = area;
    items_.push_back(std::move(chevron));
    items_[row_index].bounds = {content_left_ + Scaled(4),
                                area.top - Scaled(6),
                                content_right_ - Scaled(4),
                                area.bottom + Scaled(6)};
  }
  EndSection(y);
  AddHint(y, PhiStr("settings.about.reportIssueHint"));
}

void PhiSettingsWindow::BuildAdvanced(int* y) {
  // The Mac's "Shorten links to selected text" row is not drawn: "Copy
  // Link to Highlight" has no Windows counterpart yet, and a switch that
  // does nothing only misleads.
  BeginSection(y, nullptr);
  {
    const std::string& mode = prefs_->AutoPictureInPictureMode();
    const char* label = mode == "off"
                            ? "settings.advanced.pictureInPicture.offOption"
                        : mode == "parked"
                            ? "settings.advanced.pictureInPicture.parkAtEdgeOption"
                            : "settings.advanced.pictureInPicture.normalOption";
    const char* hint = mode == "off"
                           ? "settings.advanced.pictureInPicture.offDescription"
                       : mode == "parked"
                           ? "settings.advanced.pictureInPicture.parkAtEdgeDescription"
                           : "settings.advanced.pictureInPicture.normalDescription";
    AddPopupRow(y, PhiStr("settings.advanced.pictureInPicture.title"),
                PhiStr(label), "autopip", PhiStr(hint), kChipIndent, 0xE8A7);
  }
  EndSection(y);

  BeginSection(y, nullptr);
  // The description belongs TO the row (the Mac stacks title and
  // description in one VStack); a divider between them fenced the sentence
  // off from the switch it explains.
  AddToggleRow(y, PhiStr("settings.general.developerMode.enableToggle"),
               prefs_->DeveloperMode(),
               /*enabled=*/true, "devmode",
               PhiStr(prefs_->DeveloperMode()
                          ? "settings.advanced.developerMode.enabledHint"
                          : "settings.advanced.developerMode.disabledHint"),
               kChipIndent, 0xEC7A);
  EndSection(y);

  BeginSection(y, nullptr);
  {
    // Chevron.right, 12pt semibold textSecondary on Mac — a kIcon glyph.
    // The whole row opens chrome://settings, as on Mac — emitted FIRST so
    // its hover wash paints behind the label and the chevron.
    Item row;
    row.kind = ItemKind::kListRow;
    row.action = "browsersettings";
    items_.push_back(std::move(row));
    const size_t row_index = items_.size() - 1;
    const RECT area =
        AddRow(y, PhiStr("settings.general.additionalBrowserSettings.title"),
               kRowLabelHeight, 20, nullptr, kChipIndent, 0xE713);
    Item chevron;
    chevron.kind = ItemKind::kIcon;
    chevron.text = std::wstring(1, wchar_t(0xE76C));  // ChevronRight
    chevron.value = 12;
    chevron.bounds = area;
    items_.push_back(std::move(chevron));
    items_[row_index].bounds = {content_left_ + Scaled(4),
                                area.top - Scaled(6),
                                content_right_ - Scaled(4),
                                area.bottom + Scaled(6)};
  }
  EndSection(y);
}

void PhiSettingsWindow::AddDividerRow(int* y) {
  if (!search_filter_.empty() &&
      (last_row_skipped_ || card_rows_ == 0 ||
       (!items_.empty() && items_.back().kind == ItemKind::kDivider))) {
    return;  // Nothing above it to separate.
  }
  // The design stacks bordered row boxes: the rule runs the card's full
  // width at the boundary, and the next row's top padding follows it.
  Item divider;
  divider.kind = ItemKind::kDivider;
  divider.bounds = {content_left_ + 1, *y, content_right_ - 1, *y + 1};
  items_.push_back(std::move(divider));
  *y += Scaled(kRowPadding);
}

void PhiSettingsWindow::AddSubHint(int* y, const wchar_t* text) {
  if (!search_filter_.empty() && last_row_skipped_) {
    return;  // Its row was filtered out.
  }
  const int left = (content_left_ + Scaled(kCardPadding));
  const int right = (content_right_ - Scaled(kCardPadding));
  // Tucked under the row above it at the Mac's 4pt VStack spacing — or,
  // when it IS the card's first line (Developer's empty grant lists), on
  // the card's own padding. The old unconditional pull-up put that case
  // four pixels ABOVE the card's top edge.
  const int top =
      card_rows_ > 0 ? *y - Scaled(kRowPadding) + Scaled(4) : *y;
  const int height = std::max(Scaled(kHintHeight),
                              WrappedHeight(hint_font_, text, right - left));
  Item hint;
  hint.kind = ItemKind::kHint;
  hint.text = text;
  hint.wrap = true;
  hint.bounds = {left, top, right, top + height};
  items_.push_back(std::move(hint));
  *y = top + height + Scaled(kRowPadding);
  ++card_rows_;
}

void PhiSettingsWindow::AddSectionHeading(int* y, const wchar_t* title,
                                          const wchar_t* description) {
  heading_pending_ = true;
  heading_start_ = items_.size();
  heading_start_y_ = *y;
  Item heading;
  heading.kind = ItemKind::kSectionTitle;
  heading.text = title;
  heading.bounds = {content_left_, *y, content_right_,
                    *y + Scaled(kSectionTitleHeight)};
  items_.push_back(std::move(heading));
  *y += Scaled(kSectionTitleHeight) + Scaled(4);
  const int width = content_right_ - content_left_;
  const int height = std::max(
      Scaled(kHintHeight), WrappedHeight(hint_font_, description, width));
  Item subtitle;
  subtitle.kind = ItemKind::kSubtitle;
  subtitle.text = description;
  subtitle.wrap = true;
  subtitle.bounds = {content_left_, *y, content_right_,
                     *y + height};
  items_.push_back(std::move(subtitle));
  // Leaves `y` where BeginSection(y, nullptr) expects it: the card follows at
  // the same title-to-card gap a titled section uses.
  *y += height + Scaled(kTitleToCard);
}

void PhiSettingsWindow::AddToggleRow(int* y, const wchar_t* title, bool on,
                                     bool enabled,
                                     const std::string& action,
                                     const wchar_t* hint, int text_indent,
                                     wchar_t chip_glyph,
                                     COLORREF chip_color, int row_padding) {
  const size_t first = items_.size();
  // kToggleHeight, not kToggleHeight + 4: the row's padding is AddRow's
  // business. Handing the extra four points to the painter drew the Mac's
  // 26x15 mini switch as a 26x19 one — squat, and with barely half the
  // knob travel, which is what wedged the knob against the end.
  const RECT area = AddRow(y, title, kToggleHeight, kToggleWidth, hint,
                           text_indent, chip_glyph, chip_color, row_padding);
  // A dead row must READ dead. The label and its description used to paint
  // at full strength beside a washed-out switch, so the Kiosk pair and
  // "Apply theme to text selection" looked exactly like the live rows
  // above them and simply did nothing when clicked.
  for (size_t i = first; i < items_.size(); ++i) {
    items_[i].enabled = enabled;
  }
  Item toggle;
  toggle.kind = ItemKind::kToggle;
  toggle.on = on;
  toggle.enabled = enabled;
  toggle.action = action;
  toggle.bounds = area;
  items_.push_back(std::move(toggle));
}

void PhiSettingsWindow::AddExpander(int* y, const std::wstring& name,
                                    const std::wstring& hint,
                                    const std::wstring& icon, bool avatar,
                                    COLORREF tint, bool expanded, bool badge,
                                    const std::string& action,
                                    const std::string& payload) {
  Item header;
  header.kind = ItemKind::kExpander;
  header.text = name;
  header.detail = hint;
  header.icon = icon;
  header.value = avatar ? 1 : 0;
  header.swatch = tint;
  header.on = expanded;
  header.selected = badge;
  header.action = action;
  header.payload = payload;
  // The header row starts at `*y` (the card's edge or the divider under
  // the row above) and is the design's 68; its hover wash sits 4 inside
  // the card's border.
  header.bounds = {content_left_ + Scaled(4), *y + Scaled(2),
                   content_right_ - Scaled(4),
                   *y + Scaled(kExpanderHeight) - Scaled(2)};
  items_.push_back(std::move(header));
  *y += Scaled(kExpanderHeight);
  ++card_rows_;
}

RECT PhiSettingsWindow::AddDetailRow(int* y, const wchar_t* label,
                                     int control_height, int control_width) {
  // An expander body's row (design "Settings · Spaces / Profiles"): the
  // label in the row text's column, the control kDetailLabelWidth further
  // in, on a kDetailRowPitch grid; no dividers between them.
  const int left = content_left_ + Scaled(kCardPadding + kChipIndent);
  const int control_left = left + Scaled(kDetailLabelWidth);
  const int pitch = std::max(Scaled(kDetailRowPitch),
                             Scaled(control_height) + Scaled(12));
  Item text;
  text.kind = ItemKind::kRowLabel;
  text.text = label;
  text.bounds = {left, *y, control_left - Scaled(8), *y + pitch - Scaled(12)};
  items_.push_back(std::move(text));
  const int control_top = *y + (pitch - Scaled(12) - Scaled(control_height)) / 2;
  RECT control = {control_left, control_top,
                  control_left + Scaled(control_width),
                  control_top + Scaled(control_height)};
  *y += pitch;
  ++card_rows_;
  return control;
}

void PhiSettingsWindow::SelectSpace(const std::string& space_id,
                                    bool reveal) {
  if (selected_space_ != space_id) {
    selected_space_ = space_id;
    // A new Space's editor opens on ITS icon: the tab it lives on, no
    // search, the grid scrolled to it.
    icon_tab_ = -1;
    icon_search_.clear();
    grid_first_row_ = -1;
  }
  if (reveal) {
    list_reveal_ = true;
  }
}

// Settings › Spaces (design node 9056:18105; the Mac's SpacesSettingsView,
// master-detail): the two-pane card, then the pane-wide rows the card has
// no room for, then URL Rules.
void PhiSettingsWindow::BuildSpaces(int* y) {
  const std::vector<PhiSpaceSummary> spaces = spaces_->Summaries();
  const std::vector<PhiProfileSummary> profiles = profiles_->Profiles();
  const bool searching = !search_filter_.empty();
  // A selection always exists while a Space does: the one kept, else the
  // default role's (the Mac's selectInitialSpace).
  const auto find = [&spaces](const std::string& id) -> const PhiSpaceSummary* {
    for (const PhiSpaceSummary& space : spaces) {
      if (space.id == id) {
        return &space;
      }
    }
    return nullptr;
  };
  if (!find(selected_space_) && !spaces.empty()) {
    const PhiSpaceSummary* fallback = find(spaces_->DefaultSpaceId());
    SelectSpace(fallback ? fallback->id : spaces.front().id, /*reveal=*/true);
  }
  const PhiSpaceSummary* selected = find(selected_space_);

  // The results page shows the card when the query names a Space (or its
  // profile) or one of the card's own controls.
  bool show_card = !searching;
  if (searching) {
    for (const PhiSpaceSummary& space : spaces) {
      std::wstring profile_name;
      for (const PhiProfileSummary& profile : profiles) {
        if (profile.id == space.profile_id) {
          profile_name = base::UTF8ToWide(profile.name);
        }
      }
      show_card = show_card ||
                  SearchMatches(base::UTF8ToWide(space.name).c_str(),
                                profile_name.c_str());
    }
    for (const char* key :
         {"settings.navigation.spacesTitle", "settings.spaces.theme.colorLabel",
          "common.iconPicker.iconTabTitle", "common.iconPicker.emojiTabTitle",
          "settings.spaces.newButtonTooltip"}) {
      show_card = show_card || SearchMatches(PhiStr(key), nullptr);
    }
  }
  if (show_card && selected) {
    if (searching) {
      ++filter_hits_;
    }
    BuildSpaceCard(y, spaces, profiles);
    *y += Scaled(kSpacesStackGap);
  }

  // The Mac's "Manage Spaces in Library" button (SpacesSettingsView): the
  // Library overlay on Spaces over the focused browser window, Settings
  // hidden behind it.
  if (const wchar_t* manage = PhiStr("settings.spaces.manageInLibrary");
      SearchMatches(manage, nullptr)) {
    if (!search_filter_.empty()) {
      ++filter_hits_;
    }
    BeginSection(y, nullptr);
    const RECT area = AddRow(y, manage, kPillHeight, 110, nullptr,
                             kChipIndent, 0xE8F1);
    Item button;
    button.kind = ItemKind::kButton;
    button.text = PhiStr("library.navigation.title");
    button.action = "library";
    button.bounds = area;
    items_.push_back(std::move(button));
    EndSection(y);
  }

  // The design's URL Rules row closes its composition…
  AddSpacesUrlRow(y);

  // …and under it, what the old expander page carried that the design has
  // no place for: the pane-wide pinned-tab scope (the Mac keeps it under
  // its card too) and the selected Space's Ctrl+n, which follows the list
  // order.
  BeginSection(y, nullptr);
  {
    const char* description = "";
    switch (host_->PinScope()) {
      case PhiPinScope::kSpace:
        description = "settings.spaces.pinnedTabScope.spaceDescription";
        break;
      case PhiPinScope::kProfile:
        description = "settings.spaces.pinnedTabScope.profileDescription";
        break;
      case PhiPinScope::kApp:
        description = "settings.spaces.pinnedTabScope.appDescription";
        break;
    }
    AddPopupRow(y, PhiStr("settings.spaces.pinnedTabScope.title"),
                PhiStr(host_->PinScope() == PhiPinScope::kSpace
                           ? "settings.spaces.pinnedTabScope.spaceMenuOption"
                       : host_->PinScope() == PhiPinScope::kProfile
                           ? "settings.spaces.pinnedTabScope.profileOption"
                           : "settings.spaces.pinnedTabScope.appOption"),
                "pinscope", *description ? PhiStr(description) : nullptr,
                kChipIndent, 0xE718);
  }
  if (selected) {
    AddDividerRow(y);
    int ordinal = 0;
    for (size_t i = 0; i < spaces.size(); ++i) {
      if (spaces[i].id == selected->id) {
        ordinal = static_cast<int>(i) + 1;
      }
    }
    // "Shortcut: Work", the keycaps trailing; past the ninth Space there
    // is no chord, only the note.
    const std::wstring title =
        PhiStrF("settings.a11y.labelValue",
                {PhiStr("settings.spaces.shortcutLabel"),
                 base::UTF8ToWide(selected->name)});
    const bool chord = ordinal >= 1 && ordinal <= 9;
    const RECT area =
        AddRow(y, title.c_str(), 24, chord ? 34 + 6 + 24 : 20,
               PhiStr("settings.spaces.shortcutHint"), kChipIndent, 0xE765);
    if (chord && area.right > area.left) {
      // Key names, not numbers: the digit is the key on the keyboard.
      const std::wstring caps[] = {PhiStr("sidebar.ctrl"),
                                   std::to_wstring(ordinal)};
      int x = area.left;
      for (const std::wstring& cap_text : caps) {
        Item cap;
        cap.kind = ItemKind::kKeycap;
        cap.text = cap_text;
        const int w = Scaled(cap_text.size() > 1 ? 34 : 24);
        cap.bounds = {x, area.top, x + w, area.bottom};
        items_.push_back(std::move(cap));
        x += w + Scaled(6);
      }
    }
  }
  EndSection(y);
}

void PhiSettingsWindow::BuildSpaceCard(
    int* y,
    const std::vector<PhiSpaceSummary>& spaces,
    const std::vector<PhiProfileSummary>& profiles) {
  const RECT card = {content_left_, *y, content_right_,
                     *y + Scaled(kSpacesCardInner) + 2};
  const RECT inner = {card.left + 1, card.top + 1, card.right - 1,
                      card.bottom - 1};
  const int divider_x = inner.left + Scaled(kSpacesListWidth);
  // The bar's rule; the 24 bar sits under it, flush with the card.
  const int bar_line = inner.bottom - Scaled(kSpaceBar) - 1;
  {
    Item frame;
    frame.kind = ItemKind::kSpaceCard;
    frame.bounds = card;
    frame.value = divider_x;
    frame.total = bar_line;
    items_.push_back(std::move(frame));
  }

  // --- The list -------------------------------------------------------------
  // In drag order while the grip is held (the Mac's orderedIds), with any
  // Space the drag does not know appended; otherwise the model's order.
  std::vector<const PhiSpaceSummary*> order;
  if (reorder_drag_) {
    for (const std::string& id : reorder_order_) {
      for (const PhiSpaceSummary& space : spaces) {
        if (space.id == id) {
          order.push_back(&space);
        }
      }
    }
  }
  for (const PhiSpaceSummary& space : spaces) {
    if (std::find(order.begin(), order.end(), &space) == order.end()) {
      order.push_back(&space);
    }
  }
  const int count = static_cast<int>(order.size());
  const int list_top = inner.top + Scaled(kSpaceListTop);
  const int list_bottom = bar_line - Scaled(kSpaceRowRight);
  const int pitch = Scaled(kSpaceRowHeight + kSpaceRowGap);
  const int fit = std::max(
      1, (list_bottom - list_top + Scaled(kSpaceRowGap)) / pitch);
  int selected_index = 0;
  for (int i = 0; i < count; ++i) {
    if (order[static_cast<size_t>(i)]->id == selected_space_) {
      selected_index = i;
    }
  }
  if (list_reveal_) {
    if (selected_index < list_first_) {
      list_first_ = selected_index;
    } else if (selected_index >= list_first_ + fit) {
      list_first_ = selected_index - fit + 1;
    }
    list_reveal_ = false;
  }
  list_first_ = std::max(0, std::min(list_first_, std::max(0, count - fit)));
  list_visible_ = fit;
  list_pitch_ = pitch;
  list_rect_ = {inner.left, list_top, divider_x, list_bottom};
  const int last = std::min(count, list_first_ + fit);
  const std::string default_id = spaces_->DefaultSpaceId();
  struct RowGeometry {
    RECT grip;
    RECT body;
    RECT pill;
  };
  std::vector<RowGeometry> rows;
  for (int i = list_first_; i < last; ++i) {
    const int top = list_top + (i - list_first_) * pitch;
    const int mid = top + Scaled(kSpaceRowHeight) / 2;
    RowGeometry row;
    const int grip_left = inner.left + Scaled(kSpaceGripLeft);
    row.grip = {grip_left, mid - Scaled(kSpaceGrip) / 2,
                grip_left + Scaled(kSpaceGrip),
                mid - Scaled(kSpaceGrip) / 2 + Scaled(kSpaceGrip)};
    row.body = {row.grip.right + Scaled(kSpaceGripGap), top,
                divider_x - Scaled(kSpaceRowRight),
                top + Scaled(kSpaceRowHeight)};
    const int pill_right = row.body.right - Scaled(kSpaceRowInset);
    row.pill = {pill_right - Scaled(kSpacePillWidth),
                mid - Scaled(kSpacePillHeight) / 2, pill_right,
                mid - Scaled(kSpacePillHeight) / 2 + Scaled(kSpacePillHeight)};
    rows.push_back(row);
  }
  // Pass 1 — the row bodies, one run in build order so the list is ONE
  // focus group (Up / Down walk it, as a list box), and the rules.
  for (int i = list_first_; i < last; ++i) {
    const PhiSpaceSummary& space = *order[static_cast<size_t>(i)];
    const RowGeometry& row = rows[static_cast<size_t>(i - list_first_)];
    Item body;
    body.kind = ItemKind::kSpaceRow;
    body.text = base::UTF8ToWide(space.name);
    body.icon = base::UTF8ToWide(space.icon_text);
    body.selected = space.id == selected_space_;
    body.value = row.pill.left;  // Where the name stops.
    body.action = "pickspace";
    body.payload = space.id;
    body.bounds = row.body;
    items_.push_back(std::move(body));
  }
  for (int i = list_first_; i + 1 < last; ++i) {
    // No rule against the selected row's fill.
    if (i == selected_index || i + 1 == selected_index) {
      continue;
    }
    const RowGeometry& row = rows[static_cast<size_t>(i - list_first_)];
    Item rule;
    rule.kind = ItemKind::kHairline;
    rule.bounds = {row.body.right - Scaled(kSpaceRule), row.body.bottom,
                   row.body.right, row.body.bottom + 1};
    items_.push_back(std::move(rule));
  }
  // Pass 2 — each row's grip and profile pill. Only the selected row's
  // are Tab stops; the others take a click (and keep the keyboard focus
  // it gives them).
  for (int i = list_first_; i < last; ++i) {
    const PhiSpaceSummary& space = *order[static_cast<size_t>(i)];
    const RowGeometry& row = rows[static_cast<size_t>(i - list_first_)];
    const bool is_selected = space.id == selected_space_;
    const std::wstring name = base::UTF8ToWide(space.name);
    Item grip;
    grip.kind = ItemKind::kDragHandle;
    grip.action = "spacedrag";
    grip.payload = space.id;
    grip.a11y = PhiStrF("settings.a11y.labelValue",
                        {PhiStr("library.spaces.reorder"), name});
    grip.tab_stop = is_selected;
    grip.bounds = row.grip;
    items_.push_back(std::move(grip));
    std::wstring profile_name;
    for (const PhiProfileSummary& profile : profiles) {
      if (profile.id == space.profile_id) {
        profile_name = base::UTF8ToWide(profile.name);
      }
    }
    if (profile_name.empty()) {
      profile_name = base::UTF8ToWide(space.profile_id);
    }
    Item pill;
    pill.kind = ItemKind::kRowPopup;
    pill.text = profile_name;
    pill.on = is_selected;
    pill.action = "spaceprofile";
    pill.payload = space.id;
    // The default Space keeps its binding (PhiSpaceManager refuses it: its
    // browser exists before any Space is chosen), so its pill is inert
    // rather than a choice that is refused after the fact.
    pill.enabled = space.id != default_id;
    pill.a11y = PhiStrF(
        "settings.a11y.labelValue",
        {name, PhiStrF("settings.spaces.profileHint", {profile_name})});
    pill.tab_stop = is_selected;
    pill.bounds = row.pill;
    items_.push_back(std::move(pill));
  }

  // The bar: + / − / edit, 24 square, split by 1 px rules.
  {
    const int top = bar_line + 1;
    int x = inner.left + Scaled(4);
    const bool deletable = spaces_->CanDeleteSpace(selected_space_);
    const struct {
      const wchar_t* glyph;
      const char* action;
      const char* name;  // The Mac's .help strings.
      bool enabled;
    } kButtons[] = {
        {L"", "newspace", "settings.spaces.newButtonTooltip", true},
        {L"", "delspace", "settings.spaces.deleteButtonTooltip",
         deletable},
        {L"", "renspace", "settings.spaces.renameButtonTooltip", true},
    };
    bool first = true;
    for (const auto& definition : kButtons) {
      if (!first) {
        Item rule;
        rule.kind = ItemKind::kHairline;
        rule.bounds = {x, top, x + 1, top + Scaled(kSpaceBar)};
        items_.push_back(std::move(rule));
        x += 1;
      }
      first = false;
      Item button;
      button.kind = ItemKind::kBarButton;
      button.text = definition.glyph;
      button.action = definition.action;
      button.a11y = PhiStr(definition.name);
      button.enabled = definition.enabled;
      button.bounds = {x, top, x + Scaled(kSpaceBarButton),
                       top + Scaled(kSpaceBar)};
      items_.push_back(std::move(button));
      x += Scaled(kSpaceBarButton);
    }
  }

  // --- The editor -----------------------------------------------------------
  for (const PhiSpaceSummary& space : spaces) {
    if (space.id == selected_space_) {
      const RECT pane = {divider_x + 1 + Scaled(kEditorPad),
                         inner.top + Scaled(kEditorPad),
                         inner.right - Scaled(kEditorPad),
                         inner.bottom - Scaled(kEditorPad)};
      BuildSpaceEditor(pane, space);
      break;
    }
  }
  *y = card.bottom;
}

void PhiSettingsWindow::BuildSpaceEditor(const RECT& pane,
                                         const PhiSpaceSummary& space) {
  const bool dark = prefs_->Palette().dark;
  // The wells' ground: ink 3 % over the card (white 3 % in dark).
  const COLORREF card_ground =
      dark ? RGB(0x2B, 0x2B, 0x2B) : RGB(0xFF, 0xFF, 0xFF);
  const COLORREF well =
      Blend(dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00), card_ground,
            8);
  // A row's label, stopping 8 short of the control trailing it.
  const auto label = [&](const RECT& row, const wchar_t* text,
                         int control_width) {
    Item item;
    item.kind = ItemKind::kPaneLabel;
    item.text = text;
    item.backdrop = well;
    item.bounds = {row.left + Scaled(kWellPadX), row.top,
                   std::max<int>(row.left + Scaled(kWellPadX),
                                 row.right - Scaled(kWellPadX) -
                                     Scaled(control_width) - Scaled(8)),
                   row.bottom};
    items_.push_back(std::move(item));
  };

  // 1) Appearance: the Mac's ThemeColorSettingsRow — "Color", the theme's
  // colours as a dot, a chevron; the row opens the Space theme editor.
  const RECT appearance = {pane.left, pane.top, pane.right,
                           pane.top + Scaled(kWellRow)};
  {
    Item box;
    box.kind = ItemKind::kGroupBox;
    box.backdrop = well;
    box.bounds = appearance;
    items_.push_back(std::move(box));
  }
  {
    // indicatorColors: a palette's resolved colours, opaque; else the
    // theme's overlay colour.
    Item row;
    row.kind = ItemKind::kThemeColor;
    row.colors = SpaceThemeDots(space);
    row.backdrop = well;
    row.action = "spacecolor";
    row.payload = space.id;
    row.a11y = PhiStr("settings.spaces.theme.colorLabel");
    row.bounds = appearance;
    items_.push_back(std::move(row));
  }
  label(appearance, PhiStr("settings.spaces.theme.colorLabel"),
        kThemeColorControl);

  // 2) The picker well: Icon | Emoji, search, the grid.
  const RECT picker = {pane.left, appearance.bottom + Scaled(kEditorGap),
                       pane.right, pane.bottom};
  const size_t picker_index = items_.size();
  {
    Item box;
    box.kind = ItemKind::kGroupBox;
    box.backdrop = well;
    box.bounds = picker;
    items_.push_back(std::move(box));
  }
  const bool emoji_tab =
      icon_tab_ >= 0 ? icon_tab_ == 1
                     : space.icon.rfind(kPhiEmojiPrefix, 0) == 0;
  const int centre = (picker.left + picker.right) / 2;
  int top = picker.top + Scaled(kWellPadY);
  {
    const std::array<const wchar_t*, 2> labels = {
        PhiStr("common.iconPicker.iconTabTitle"),
        PhiStr("common.iconPicker.emojiTabTitle")};
    std::array<int, 2> widths = {};
    for (size_t i = 0; i < 2; ++i) {
      int text_px = 0;
      if (hint_font_) {
        if (!measure_dc_) {
          measure_dc_ = ::CreateCompatibleDC(nullptr);
        }
        if (measure_dc_) {
          HGDIOBJ previous = ::SelectObject(measure_dc_, hint_font_);
          RECT box = {0, 0, 0, 0};
          ::DrawTextW(measure_dc_, labels[i], -1, &box,
                      DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
          ::SelectObject(measure_dc_, previous);
          text_px = box.right - box.left;
        }
      }
      widths[i] = text_px + 2 * Scaled(kSegmentPadX);
    }
    const int inset = Scaled(kSegmentInset);
    const int pill_w = inset + widths[0] + Scaled(kSegmentGap) + widths[1] + inset;
    const int pill_left = centre - pill_w / 2;
    Item track;
    track.kind = ItemKind::kSegment;
    track.backdrop = well;
    track.bounds = {pill_left, top, pill_left + pill_w,
                    top + Scaled(kSegmentHeight)};
    items_.push_back(std::move(track));
    int x = pill_left + inset;
    for (size_t i = 0; i < 2; ++i) {
      Item segment;
      segment.kind = ItemKind::kSegment;
      segment.text = labels[i];
      segment.selected = (i == 1) == emoji_tab;
      segment.value = static_cast<int>(i);
      segment.action = "icontab";
      segment.payload = i == 0 ? "icon" : "emoji";
      segment.backdrop = well;
      segment.bounds = {x, top + inset, x + widths[i],
                        top + Scaled(kSegmentHeight) - inset};
      items_.push_back(std::move(segment));
      x += widths[i] + Scaled(kSegmentGap);
    }
    top += Scaled(kSegmentHeight) + Scaled(kWellPadY);
  }
  {
    const int width = std::min(Scaled(kIconSearchWidth),
                               static_cast<int>(picker.right - picker.left) -
                                   2 * Scaled(kWellPadX));
    Item search;
    search.kind = ItemKind::kIconSearch;
    search.text = icon_search_;
    search.detail = PhiStr(emoji_tab ? "common.iconPicker.emojiSearchPlaceholder"
                                     : "common.iconPicker.iconsSearchPlaceholder");
    search.backdrop = well;
    search.bounds = {centre - width / 2, top, centre - width / 2 + width,
                     top + Scaled(kIconSearchHeight)};
    items_.push_back(std::move(search));
    top += Scaled(kIconSearchHeight) + Scaled(kGridTopGap);
  }
  // The grid: whole rows only, scrolled by row (the wheel, or the arrows
  // walking off its edge); only the rows in view are built.
  const std::wstring needle = PhiIconSearchFold(icon_search_);
  std::vector<const PhiIconChoice*> shown;
  for (const PhiIconChoiceGroup& group : PhiIconChoiceGroups(emoji_tab)) {
    for (const PhiIconChoice& choice : group.items) {
      if (needle.empty() || choice.folded.find(needle) != std::wstring::npos) {
        shown.push_back(&choice);
      }
    }
  }
  const int cell = Scaled(kGridCell);
  const int row_pitch = cell + Scaled(kGridRowGap);
  const int grid_left = picker.left + Scaled(kGridSide);
  const int grid_right = picker.right - Scaled(kGridSide);
  const int grid_bottom = picker.bottom - Scaled(kWellPadY);
  const int visible = std::max(
      1, (grid_bottom - top + Scaled(kGridRowGap)) / row_pitch);
  const int count = static_cast<int>(shown.size());
  const int rows = (count + kGridColumns - 1) / kGridColumns;
  const int max_first = std::max(0, rows - visible);
  if (grid_first_row_ < 0) {
    // Reveal the Space's icon: its row in view, a little down from the top.
    int current = -1;
    for (int i = 0; i < count; ++i) {
      if (shown[static_cast<size_t>(i)]->storage == space.icon) {
        current = i;
      }
    }
    const int row = current < 0 ? 0 : current / kGridColumns;
    grid_first_row_ = row < visible ? 0 : row - visible / 2;
  }
  grid_first_row_ = std::max(0, std::min(grid_first_row_, max_first));
  grid_rows_ = rows;
  grid_visible_ = visible;
  grid_count_ = count;
  grid_rect_ = {picker.left, top, picker.right,
                top + visible * row_pitch - Scaled(kGridRowGap)};
  items_[picker_index].value = grid_first_row_;
  items_[picker_index].total = rows;
  items_[picker_index].visible = visible;
  if (shown.empty()) {
    Item none;
    none.kind = ItemKind::kPaneLabel;
    none.value = 1;  // Centred, secondary.
    none.text = PhiStr("common.iconPicker.searchEmptyMessage");
    none.backdrop = well;
    none.bounds = {grid_left, top, grid_right, top + row_pitch};
    items_.push_back(std::move(none));
    return;
  }
  const int span = std::max(cell, grid_right - grid_left);
  const int first = grid_first_row_ * kGridColumns;
  const int end = std::min(count, first + visible * kGridColumns);
  for (int i = first; i < end; ++i) {
    const PhiIconChoice& choice = *shown[static_cast<size_t>(i)];
    const int col = i % kGridColumns;
    const int row = i / kGridColumns - grid_first_row_;
    const int x = grid_left + (span - cell) * col / (kGridColumns - 1);
    const int cell_top = top + row * row_pitch;
    Item glyph;
    glyph.kind = ItemKind::kPickerCell;
    glyph.text = choice.text;
    glyph.a11y = choice.name;
    glyph.selected = choice.storage == space.icon;
    glyph.value = i;
    glyph.backdrop = well;
    glyph.action = "spaceicon";
    glyph.payload = choice.storage;
    glyph.space = space.id;
    glyph.bounds = {x, cell_top, x + cell, cell_top + cell};
    items_.push_back(std::move(glyph));
  }
}

// The design's "URL Rules" row under the card: the link glyph, the title
// over its description, a chevron; the whole row opens the universal
// editor (PhiUrlRulesWindow), as the Navigation page's row does.
void PhiSettingsWindow::AddSpacesUrlRow(int* y) {
  const wchar_t* title = PhiStr("settings.navigations.urlRules.sectionTitle");
  const wchar_t* subtitle =
      PhiStr("settings.navigations.urlRules.sectionDescription");
  if (!SearchMatches(title, subtitle)) {
    return;
  }
  if (!search_filter_.empty()) {
    ++filter_hits_;
  }
  const int pad = Scaled(kUrlRowPad);
  const int text_left = content_left_ + pad + Scaled(kUrlRowIcon) + pad;
  const int text_right = content_right_ - pad - Scaled(16) - pad;
  const int title_h = Scaled(18);
  const int sub_h =
      std::max(Scaled(16), WrappedHeight(hint_font_, subtitle,
                                         std::max(1, text_right - text_left)));
  const int height = std::max(Scaled(kUrlRowMinHeight),
                              title_h + Scaled(2) + sub_h + 2 * pad + 1);
  Item row;
  row.kind = ItemKind::kUrlRow;
  row.text = title;
  row.detail = subtitle;
  row.value = sub_h;
  row.action = "urlrules";
  row.bounds = {content_left_, *y, content_right_, *y + height};
  items_.push_back(std::move(row));
  *y += height + Scaled(kSpacesStackGap);
}

// The Mac's Navigations pane (NavigationsSettingsView): the link-opening
// controls, collected out of General and Spaces. Peek, the Ctrl+Alt-click
// Kiosk switch and URL Rules are real.
//
// The Mac's EXTERNAL-LINKS Kiosk switch is left out: the client has no
// external-link entry point yet. It comes back as a live row once one
// exists, the way the Ctrl+Alt row did when the framework took the Mac's
// Cmd+Opt remap (Browser::OpenURLFromTab) for Windows.
void PhiSettingsWindow::BuildNavigation(int* y) {
  AddSectionHeading(y, PhiStr("settings.navigations.kiosk.sectionTitle"),
                    PhiStr("settings.navigations.kiosk.sectionDescription"));
  BeginSection(y, nullptr);
  // The Mac's "Open external links in Kiosk" switch is not drawn until the
  // client has an external-link entry point for it to steer.
  AddToggleRow(y, PhiStr("settings.navigations.kiosk.ctrlAlt.title"),
               prefs_->OpenKioskOnCtrlAltClick(), /*enabled=*/true,
               "kioskctrlalt",
               PhiStr("settings.navigations.kiosk.ctrlAlt.description"),
               kChipIndent, 0xE765);
  AddDividerRow(y);
  AddSubHint(y, PhiStr("settings.navigations.kiosk.otherEntryPoints"));
  EndSection(y);

  AddSectionHeading(y, PhiStr("settings.navigations.peek.sectionTitle"),
                    PhiStr("settings.navigations.peek.sectionDescription"));
  const bool peek = prefs_->PeekViewEnabled();
  BeginSection(y, nullptr);
  AddToggleRow(
      y, PhiStr("settings.navigations.peek.enableToggle.title"), peek,
      /*enabled=*/true, "peekview",
      PhiStr("settings.navigations.peek.enableToggle.descriptionWindows"),
      kChipIndent, 0xE7B3);
  AddDividerRow(y);
  // The sub-switch is dimmed and inert while the master is off, as on Mac.
  AddToggleRow(y, PhiStr("settings.navigations.peek.autoToggle.title"),
               prefs_->AutoPeekViewEnabled(), /*enabled=*/peek, "autopeek",
               PhiStr("settings.navigations.peek.autoToggle.description"),
               kChipIndent, 0xE718);
  EndSection(y);

  AddUrlRulesSection(y);
}

// The "URL Rules" section the Spaces and Navigation panes share: one row
// that opens the universal editor (PhiUrlRulesWindow), as the Mac's
// "Manage URL Rules…" row does.
void PhiSettingsWindow::AddUrlRulesSection(int* y) {
  BeginSection(y, PhiStr("settings.navigations.urlRules.sectionTitle"));
  {
    // The whole row opens the editor, not just the trailing chevron —
    // emitted FIRST so its hover wash paints behind the text.
    Item row;
    row.kind = ItemKind::kListRow;
    row.action = "urlrules";
    items_.push_back(std::move(row));
    const size_t row_index = items_.size() - 1;
    const size_t first = items_.size();
    const int top = *y - Scaled(kRowPadding);
    const RECT area =
        AddRow(y, PhiStr("settings.navigations.urlRules.openButton"),
               kRowLabelHeight, 20,
               PhiStr("settings.navigations.urlRules.sectionDescription"),
               kChipIndent, 0xE71B);
    Item chevron;
    chevron.kind = ItemKind::kIcon;
    chevron.text = std::wstring(1, wchar_t(0xE76C));  // ChevronRight
    chevron.value = 12;
    chevron.bounds = area;
    items_.push_back(std::move(chevron));
    (void)first;
    items_[row_index].bounds = {content_left_ + Scaled(2), top + Scaled(2),
                                content_right_ - Scaled(2), *y - Scaled(2)};
  }
  EndSection(y);
}

void PhiSettingsWindow::AddProfileBody(
    int* y, const PhiProfileSummary& profile,
    const std::vector<PhiSpaceSummary>& spaces) {
  const std::string& id = profile.id;
  *y += Scaled(4);
  {
    const RECT area = AddDetailRow(y, PhiStr("common.editor.nameField.label"),
                                   kPillHeight, kDetailFieldWidth);
    Item field;
    field.kind = ItemKind::kSearchField;
    field.text = base::UTF8ToWide(profile.name);
    field.value = 1;
    field.action = "renprofile";
    field.payload = id;
    field.bounds = area;
    items_.push_back(std::move(field));
  }
  // Search engine / download location — live, per profile. Both read
  // empty while the profile is not loaded, and the rows grey out; one
  // short poll per rebuild fills them in once the loads land.
  {
    const std::vector<PhiSearchEngineRow> engines =
        profiles_->SearchEngines(id);
    std::wstring engine_name =
        PhiStr("settings.profiles.searchEngine.unavailableStatus");
    for (const PhiSearchEngineRow& engine : engines) {
      if (engine.is_default) {
        engine_name = base::UTF8ToWide(engine.name);
        break;
      }
    }
    const std::string download_dir = profiles_->DownloadDirectory(id);
    if ((engines.empty() || download_dir.empty()) && alive()) {
      profile_poll_.Start(700);
    }
    std::wstring download_leaf =
        PhiStr("settings.profiles.downloadLocation.choosePlaceholder");
    if (!download_dir.empty()) {
      const size_t cut = download_dir.find_last_of("\\\\/");
      download_leaf = base::UTF8ToWide(
          cut == std::string::npos ? download_dir
                                   : download_dir.substr(cut + 1));
    }
    {
      const RECT area =
          AddDetailRow(y, PhiStr("settings.profiles.searchEngine.label"),
                       kPillHeight, kDetailFieldWidth);
      Item pill;
      pill.kind = ItemKind::kPopup;
      pill.text = engine_name;
      pill.enabled = !engines.empty();
      pill.action = "searchengine";
      pill.payload = id;
      pill.bounds = area;
      items_.push_back(std::move(pill));
    }
    {
      // The location ComboBox with "Choose…" beside it; both open the
      // folder picker.
      const RECT area =
          AddDetailRow(y, PhiStr("settings.profiles.downloadLocation.label"),
                       kPillHeight, kDetailFieldWidth + 12 + 88);
      Item pill;
      pill.kind = ItemKind::kPopup;
      pill.text = download_leaf;
      pill.enabled = !download_dir.empty();
      pill.action = "downloadfolder";
      pill.payload = id;
      pill.bounds = {area.left, area.top, area.left + Scaled(kDetailFieldWidth),
                     area.bottom};
      items_.push_back(std::move(pill));
      Item choose;
      choose.kind = ItemKind::kButton;
      choose.text =
          PhiStr("settings.profiles.downloadLocation.choosePlaceholder");
      choose.enabled = !download_dir.empty();
      choose.action = "downloadfolder";
      choose.payload = id;
      choose.bounds = {area.right - Scaled(88), area.top, area.right,
                       area.bottom};
      items_.push_back(std::move(choose));
    }
    if (PhiFolio::Enabled()) {
      // Folio Folder: the profile's override, or "Same as General".
      const std::wstring label = FolioOverrideLabel(*prefs_, id);
      const RECT area =
          AddDetailRow(y, PhiStr("settings.profiles.details.folioLabel"),
                       kPillHeight, kDetailFieldWidth);
      Item pill;
      pill.kind = ItemKind::kPopup;
      pill.text = label;
      pill.action = "foliooverride";
      pill.payload = id;
      pill.bounds = area;
      items_.push_back(std::move(pill));
    }
  }
  // Used by — the Spaces bound to this profile.
  int bound = 0;
  {
    std::wstring names;
    for (const PhiSpaceSummary& space : spaces) {
      if (space.profile_id == id) {
        ++bound;
        names += (names.empty() ? L"" : L", ") + base::UTF8ToWide(space.name);
      }
    }
    const RECT area = AddDetailRow(y, PhiStr("settings.profiles.usedByLabel"),
                                   kRowLabelHeight, 400);
    Item text;
    text.kind = ItemKind::kRowLabel;
    text.enabled = false;  // The design's dim 14.
    // The count is a plural family ({0} = the count, {1} = the names).
    text.text = bound == 0
                    ? std::wstring(PhiStr("settings.profiles.usedBy.none"))
                    : PhiStrN("settings.profiles.usedBy.spaces", bound,
                              {names});
    text.bounds = area;
    items_.push_back(std::move(text));
  }
  *y += Scaled(4);
  // The data pages: a nested card of three rows into chrome://settings
  // for this profile, indented to the label column.
  {
    const int nested_left = content_left_ + Scaled(kCardPadding + kChipIndent);
    const int nested_right = content_right_ - Scaled(kCardPadding);
    const size_t card_index = items_.size();
    Item nested;
    nested.kind = ItemKind::kCard;
    nested.bounds = {nested_left, *y, nested_right, 0};
    items_.push_back(std::move(nested));
    const struct {
      wchar_t glyph;
      const char* title;  // String-catalog keys; `hint` may be null.
      const char* hint;
      const char* url;
    } kRows[] = {
        {0xE72E, "settings.profiles.dataLinks.privacy",
         "settings.profiles.dataLinks.privacyHint",
         "chrome://settings/privacy"},
        {0xE7E7, "settings.profiles.dataLinks.notifications", nullptr,
         "chrome://settings/content/notifications"},
        {0xE74D, "settings.profiles.dataLinks.clearData",
         "settings.profiles.dataLinks.clearDataHint",
         "chrome://settings/clearBrowserData"},
    };
    size_t placed = 0;
    for (const auto& def : kRows) {
      const int top = *y;
      const size_t row_index = items_.size();
      Item row;
      row.kind = ItemKind::kListRow;
      row.action = "openpage";
      row.payload = def.url;
      row.profile = id;  // Opens in THIS profile, never the first window's.
      items_.push_back(std::move(row));
      const int row_h = Scaled(kRowMinHeight + 2 * kRowPadding);
      const int gutter = nested_left + Scaled(kCardPadding);
      Item glyph;
      glyph.kind = ItemKind::kIconChip;
      glyph.text = std::wstring(1, def.glyph);
      glyph.bounds = {gutter, top + (row_h - Scaled(kChipSize)) / 2,
                      gutter + Scaled(kChipSize),
                      top + (row_h + Scaled(kChipSize)) / 2};
      items_.push_back(std::move(glyph));
      const int text_left = gutter + Scaled(kChipIndent);
      const int text_right = nested_right - Scaled(44);
      const int mid = top + row_h / 2;
      Item title;
      title.kind = ItemKind::kRowLabel;
      title.text = PhiStr(def.title);
      title.bounds = def.hint
                         ? RECT{text_left, mid - Scaled(19), text_right, mid}
                         : RECT{text_left, top, text_right, top + row_h};
      items_.push_back(std::move(title));
      if (def.hint) {
        Item note;
        note.kind = ItemKind::kHint;
        note.text = PhiStr(def.hint);
        note.bounds = {text_left, mid + Scaled(1), text_right,
                       mid + Scaled(17)};
        items_.push_back(std::move(note));
      }
      Item arrow;
      arrow.kind = ItemKind::kIcon;
      arrow.text = std::wstring(1, wchar_t(0xE76C));
      arrow.value = 12;
      arrow.bounds = {nested_right - Scaled(40), top, nested_right - Scaled(12),
                      top + row_h};
      items_.push_back(std::move(arrow));
      items_[row_index].bounds = {nested_left + Scaled(2), top + Scaled(2),
                                  nested_right - Scaled(2),
                                  top + row_h - Scaled(2)};
      *y += row_h;
      ++card_rows_;
      if (++placed < std::size(kRows)) {
        Item divider;
        divider.kind = ItemKind::kDivider;
        divider.bounds = {nested_left + 1, *y, nested_right - 1, *y + 1};
        items_.push_back(std::move(divider));
      }
    }
    items_[card_index].bounds.bottom = *y;
  }
  // Delete — Default is permanent; a profile still bound to a Space stays.
  {
    *y += Scaled(kRowPadding);
    const bool is_default = id == "Default";
    const bool deletable = !is_default && bound == 0;
    Item button;
    button.kind = ItemKind::kButton;
    button.text = PhiStr("appMenu.deleteProfile");
    button.danger = true;
    button.enabled = deletable;
    button.action = "delprofile";
    button.payload = id;
    button.bounds = {content_right_ - Scaled(kCardPadding) - Scaled(114), *y,
                     content_right_ - Scaled(kCardPadding),
                     *y + Scaled(kPillHeight)};
    items_.push_back(std::move(button));
    *y += Scaled(kPillHeight) + Scaled(kRowPadding);
    ++card_rows_;
  }
}

void PhiSettingsWindow::BuildProfiles(int* y) {
  const std::vector<PhiProfileSummary> profiles = profiles_->Profiles();
  const std::vector<PhiSpaceSummary> spaces = spaces_->Summaries();
  const bool searching = !search_filter_.empty();
  if (!searching) {
    Item heading;
    heading.kind = ItemKind::kSectionTitle;
    heading.text = PhiStr("settings.profiles.yourProfilesHeading");
    heading.bounds = {content_left_, *y, content_right_ - Scaled(140),
                      *y + Scaled(kPillHeight)};
    items_.push_back(std::move(heading));
    Item add;
    add.kind = ItemKind::kButton;
    add.text = std::wstring(L"\uE710 ") +
               PhiStr("settings.profiles.newButtonTooltip");
    add.action = "addprofile";
    add.bounds = {content_right_ - Scaled(124), *y, content_right_,
                  *y + Scaled(kPillHeight)};
    items_.push_back(std::move(add));
    *y += Scaled(kPillHeight) + Scaled(kTitleToCard);
  }
  // Avatar tints, by position — a profile has no colour of its own.
  static constexpr COLORREF kTints[] = {
      RGB(0x0F, 0x6C, 0xBD), RGB(0x0B, 0x6A, 0x0B), RGB(0xCA, 0x50, 0x10),
      RGB(0x88, 0x17, 0x98), RGB(0xC2, 0x39, 0x62), RGB(0x03, 0x83, 0x87),
  };
  // One card stacks every profile's expander row (design "Settings ·
  // Profiles"); the open one's body sits under its header.
  BeginSection(y, nullptr);
  *y -= Scaled(kRowPadding);
  size_t at = 0;
  bool first = true;
  for (const PhiProfileSummary& profile : profiles) {
    const std::wstring name = base::UTF8ToWide(profile.name);
    const COLORREF tint = kTints[at++ % std::size(kTints)];
    std::wstring used;
    int bound = 0;
    for (const PhiSpaceSummary& space : spaces) {
      if (space.profile_id == profile.id) {
        ++bound;
        used += (used.empty() ? L"" : L", ") + base::UTF8ToWide(space.name);
      }
    }
    if (searching && !SearchMatches(name.c_str(), used.c_str())) {
      continue;
    }
    ++filter_hits_;
    // The count is a plural family ({0} = the count, {1} = the names).
    std::wstring hint =
        bound == 0 ? std::wstring(PhiStr("settings.profiles.unusedStatus"))
                   : PhiStrN("settings.profiles.usedByHint", bound, {used});
    if (!first) {
      Item divider;
      divider.kind = ItemKind::kDivider;
      divider.bounds = {content_left_ + 1, *y, content_right_ - 1, *y + 1};
      items_.push_back(std::move(divider));
    }
    first = false;
    const bool expanded = !searching && selected_profile_ == profile.id;
    AddExpander(y, name, hint, std::wstring(), true, tint, expanded,
                profile.id == "Default", "selectprofile", profile.id);
    if (expanded) {
      AddProfileBody(y, profile, spaces);
    }
  }
  EndSection(y);
  if (!searching) {
    AddHint(y, PhiStr("settings.profiles.footerHint"));
  }
}

void PhiSettingsWindow::BuildProfileDetail(int* y, int left, int right,
                                           const std::string& profile_id,
                                           bool show_folio_folder) {
  // The design's card row: the bare 20 glyph in the gutter, the 14 label
  // 16 after it, the trailing control; 68 tall.
  const int row_h = Scaled(kRowMinHeight + 2 * kRowPadding);
  const auto detail_row = [&](int top, wchar_t glyph, const wchar_t* title,
                              bool enabled) {
    Item icon;
    icon.kind = ItemKind::kIconChip;
    icon.text = std::wstring(1, glyph);
    icon.enabled = enabled;
    icon.bounds = {left + Scaled(kCardPadding),
                   top + (row_h - Scaled(kChipSize)) / 2,
                   left + Scaled(kCardPadding) + Scaled(kChipSize),
                   top + (row_h + Scaled(kChipSize)) / 2};
    items_.push_back(std::move(icon));
    Item label;
    label.kind = ItemKind::kRowLabel;
    label.text = title;
    label.enabled = enabled;
    label.bounds = {left + Scaled(kCardPadding + kChipIndent), top,
                    right - Scaled(kCardPadding + kPillMaxWidth + 12),
                    top + row_h};
    items_.push_back(std::move(label));
  };
  const auto trailing_pill = [&](int top, const std::wstring& text,
                                 bool enabled, const char* action) {
    Item pill;
    pill.kind = ItemKind::kPopup;
    pill.text = text;
    pill.enabled = enabled;
    if (action) {
      pill.action = action;
      pill.payload = profile_id;
    }
    pill.bounds = {right - Scaled(kCardPadding) - Scaled(PillWidthFor(text)),
                   top + (row_h - Scaled(kPillHeight)) / 2,
                   right - Scaled(kCardPadding),
                   top + (row_h + Scaled(kPillHeight)) / 2};
    items_.push_back(std::move(pill));
  };

  // Card 1: Search Engine / Download Location — live, per profile. Both
  // read empty while the profile is not loaded, and the rows gray out.
  {
    const std::vector<PhiSearchEngineRow> engines =
        profiles_->SearchEngines(profile_id);
    std::wstring engine_name =
        PhiStr("settings.profiles.searchEngine.unavailableStatus");
    for (const PhiSearchEngineRow& engine : engines) {
      if (engine.is_default) {
        engine_name = base::UTF8ToWide(engine.name);
        break;
      }
    }
    const std::string download_dir = profiles_->DownloadDirectory(profile_id);
    // A selected profile loads on demand (the framework kicks the profile
    // and its keyword service). While either read is still pending, poll:
    // one short timer per rebuild, re-armed only while data is missing, so
    // the pane fills in by itself once the loads land.
    if ((engines.empty() || download_dir.empty()) && alive()) {
      profile_poll_.Start(700);
    }
    // The Mac pill shows only the folder's last path component.
    std::wstring download_leaf =
        PhiStr("settings.profiles.downloadLocation.choosePlaceholder");
    if (!download_dir.empty()) {
      const size_t cut = download_dir.find_last_of("\\\\/");
      download_leaf = base::UTF8ToWide(
          cut == std::string::npos ? download_dir
                                   : download_dir.substr(cut + 1));
    }

    const bool folio_row = show_folio_folder && PhiFolio::Enabled();
    const int rows = folio_row ? 3 : 2;
    Item card;
    card.kind = ItemKind::kCard;
    card.bounds = {left, *y, right, *y + rows * row_h};
    items_.push_back(std::move(card));
    detail_row(*y, 0xE721, PhiStr("settings.profiles.searchEngine.label"),
               !engines.empty());
    trailing_pill(*y, engine_name, !engines.empty(), "searchengine");
    Item divider;
    divider.kind = ItemKind::kDivider;
    divider.bounds = {left + 1, *y + row_h, right - 1, *y + row_h + 1};
    items_.push_back(std::move(divider));
    detail_row(*y + row_h, 0xE896,
               PhiStr("settings.profiles.downloadLocation.label"),
               !download_dir.empty());
    trailing_pill(*y + row_h, download_leaf, !download_dir.empty(),
                  "downloadfolder");
    if (folio_row) {
      // Folio Folder: this profile's override, or "Same as General".
      Item folio_divider;
      folio_divider.kind = ItemKind::kDivider;
      folio_divider.bounds = {left + 1, *y + 2 * row_h, right - 1,
                              *y + 2 * row_h + 1};
      items_.push_back(std::move(folio_divider));
      detail_row(*y + 2 * row_h, 0xE8B7,
                 PhiStr("settings.profiles.details.folioLabel"), true);
      trailing_pill(*y + 2 * row_h, FolioOverrideLabel(*prefs_, profile_id), true,
                    "foliooverride");
    }
    *y += rows * row_h + Scaled(kSectionGap);
  }

  // "Your data": three live links into chrome://settings for this
  // profile's data pages, each with a chevron.
  {
    Item heading;
    heading.kind = ItemKind::kSectionTitle;
    heading.text = PhiStr("settings.profiles.dataLinks.yourDataHeading");
    heading.bounds = {left, *y, right, *y + Scaled(kSectionTitleHeight)};
    items_.push_back(std::move(heading));
    *y += Scaled(kSectionTitleHeight) + Scaled(kTitleToCard);
    Item card;
    card.kind = ItemKind::kCard;
    card.bounds = {left, *y, right, *y + 3 * row_h};
    items_.push_back(std::move(card));
    const struct {
      wchar_t glyph;
      const char* title;  // A string-catalog key.
      const char* url;
    } kRows[] = {
        {0xE72E, "settings.profiles.dataLinks.privacy",
         "chrome://settings/privacy"},
        {0xE7E7, "settings.profiles.dataLinks.notifications",
         "chrome://settings/content/notifications"},
        {0xE74D, "settings.profiles.dataLinks.clearData",
         "chrome://settings/clearBrowserData"},
    };
    int i = 0;
    for (const auto& def : kRows) {
      const int top = *y + i * row_h;
      // The whole row is the button, as on Mac — behind its own content, so
      // the hover wash does not paint over the label.
      Item row;
      row.kind = ItemKind::kListRow;
      row.action = "openpage";
      row.payload = def.url;
      row.profile = profile_id;  // This profile's page, not the first window's.
      row.bounds = {left + Scaled(2), top + Scaled(2), right - Scaled(2),
                    top + row_h - Scaled(2)};
      items_.push_back(std::move(row));
      detail_row(top, def.glyph, PhiStr(def.title), true);
      Item arrow;
      arrow.kind = ItemKind::kIcon;
      arrow.text = std::wstring(1, wchar_t(0xE76C));  // ChevronRight
      arrow.value = 12;
      arrow.bounds = {right - Scaled(40), top, right - Scaled(12),
                      top + row_h};
      items_.push_back(std::move(arrow));
      if (i < 2) {
        Item divider;
        divider.kind = ItemKind::kDivider;
        divider.bounds = {left + 1, top + row_h, right - 1, top + row_h + 1};
        items_.push_back(std::move(divider));
      }
      ++i;
    }
    *y += 3 * row_h;
  }
}

void PhiSettingsWindow::BuildAccount(int* y) {
  // The Mac pane's frame: the profile card on the LEFT (264 wide at x=24,
  // holding a 240x380 card with "Download image" under it), the account
  // row and the Default Browser section stacked on the right. Signed out
  // (the Mac's Guest presentation) the card is hidden and the right column
  // takes the whole width. The account is OPTIONAL on Windows
  // (docs/login-port.md): nothing else is withheld.
  const int top = *y;
  // The Mac's `offset(24)` is 36 minus the 12pt of shadow padding its card
  // view carries INSIDE its frame; the card's VISIBLE edge sits at 36 like
  // every other pane's content: card 36..276, right column 292..644.
  const int left = content_left_;
  // The Mac's ProfileCardViewController: 240 wide at the card's 292:423.
  const int card_w = Scaled(240);
  const int card_h = static_cast<int>(std::lround(
      card_w * static_cast<double>(PhiHolographicCard::kDesignHeightDip) /
      PhiHolographicCard::kDesignWidthDip));
  const int right_edge = content_right_;
  std::string account_name;
  std::string account_email;
  const bool signed_in =
      account_->AccountInfo(&account_name, &account_email);
  const bool signing_in = account_->AccountSigningIn();
  const bool reauth = account_->AccountReauthRequired();
  const bool busy = account_->AccountBusy();
  const std::string error = account_->AccountLastError();
  const bool show_card = signed_in && !reauth;
  int bottom = top;
  if (show_card) {
    Item card;
    card.kind = ItemKind::kProfileCard;
    card.action = "card";  // Hit-tested so the pointer lights it; Apply ignores it.
    card.bounds = {left, top, left + card_w, top + card_h};
    items_.push_back(std::move(card));
    // The Mac's rounded "Download image" button, centred under the card
    // (shadow padding 10 + 4).
    Item download;
    download.kind = ItemKind::kButton;
    download.text = PhiStr("settings.account.sharing.downloadImageButton");
    download.action = "downloadcard";
    const int button_w = Scaled(150);
    download.bounds = {left + (card_w - button_w) / 2, top + card_h + Scaled(14),
                       left + (card_w + button_w) / 2,
                       top + card_h + Scaled(14) + Scaled(24)};
    bottom = download.bounds.bottom;
    items_.push_back(std::move(download));
  }
  const int right_left = show_card ? left + card_w + Scaled(16) : left;
  {
    // The account row (the Mac's 92pt AccountView): the avatar with its
    // edit badge, the name with its pencil, the email, a status line, and
    // the one action — Sign out / Reauthenticate / Cancel / Sign in.
    Item card;
    card.kind = ItemKind::kCard;
    card.bounds = {right_left, top, right_edge, top + Scaled(92)};
    items_.push_back(std::move(card));
    const int button_w = Scaled(108);
    const int text_right = right_edge - Scaled(kCardPadding) - button_w - Scaled(8);
    Item avatar;
    avatar.kind = ItemKind::kAvatar;
    avatar.text = std::wstring(1, wchar_t(0xE77B));  // Contact
    avatar.value = 36;  // Fallback glyph size.
    const int avatar_px = Scaled(56);
    avatar.bounds = {right_left + Scaled(12), top + (Scaled(92) - avatar_px) / 2,
                     right_left + Scaled(12) + avatar_px,
                     top + (Scaled(92) - avatar_px) / 2 + avatar_px};
    if (show_card) {
      avatar.action = "avatar";
      avatar.enabled = !busy;
    }
    items_.push_back(std::move(avatar));
    const int text_left = right_left + Scaled(12) + avatar_px + Scaled(12);
    std::wstring error_text;
    if (error == "account_mismatch") {
      error_text = PhiStr("settings.account.error.accountMismatch");
    } else if (error == "refused") {
      error_text = PhiStr("settings.account.error.refused");
    } else if (error == "incomplete") {
      error_text = PhiStr("settings.account.error.incomplete");
    } else if (error == "profile") {
      error_text = PhiStr("settings.account.error.profile");
    } else if (error == "update") {
      error_text = PhiStr("settings.account.error.update");
    }
    if (signed_in) {
      Item name;
      name.kind = ItemKind::kEditableLabel;
      name.text = !account_name.empty()
                      ? base::UTF8ToWide(account_name)
                      : PhiStr("settings.account.displayName.fallback");
      if (show_card) {
        name.action = "rename";
        name.enabled = !busy;
      }
      name.bounds = {text_left, top + Scaled(16), text_right, top + Scaled(46)};
      items_.push_back(std::move(name));
      Item email;
      email.kind = ItemKind::kHint;
      email.text = base::UTF8ToWide(account_email);
      email.bounds = {text_left, top + Scaled(47), text_right,
                      top + Scaled(47) + Scaled(kHintHeight)};
      items_.push_back(std::move(email));
      std::wstring status;
      if (reauth) {
        status = PhiStr("settings.account.reauthentication.warning");
      } else if (busy) {
        status = PhiStr("settings.account.saving");
      } else {
        status = error_text;
      }
      if (!status.empty()) {
        Item line;
        line.kind = ItemKind::kHint;
        line.text = status;
        line.bounds = {text_left, top + Scaled(63), text_right,
                       top + Scaled(63) + Scaled(kHintHeight)};
        items_.push_back(std::move(line));
      }
    } else {
      Item label;
      label.kind = ItemKind::kRowLabel;
      label.text = signing_in ? PhiStr("settings.finishSigningInFromThe")
                              : PhiStr("settings.youAreNotSignedIn");
      const bool has_error = !signing_in && !error_text.empty();
      label.bounds = {text_left, top, text_right,
                      has_error ? top + Scaled(58) : top + Scaled(92)};
      items_.push_back(std::move(label));
      if (has_error) {
        Item failure;
        failure.kind = ItemKind::kHint;
        failure.text = error_text;
        failure.bounds = {text_left, top + Scaled(56), text_right,
                          top + Scaled(56) + Scaled(kHintHeight)};
        items_.push_back(std::move(failure));
      }
    }
    Item button;
    button.kind = ItemKind::kButton;
    if (reauth) {
      button.text = PhiStr("oobe.reauthentication.confirmButton");
      button.action = "signin";
    } else if (signed_in) {
      button.text = PhiStr("settings.account.logoutButton");
      button.action = "signout";
    } else if (signing_in) {
      button.text = PhiStr("settings.account.cancelSignIn");
      button.action = "cancelsignin";
    } else {
      button.text = PhiStr("settings.account.guest.loginButton");
      button.action = "signin";
    }
    button.enabled = !busy;
    button.bounds = {right_edge - Scaled(kCardPadding) - button_w,
                     top + Scaled(34), right_edge - Scaled(kCardPadding),
                     top + Scaled(58)};
    items_.push_back(std::move(button));
  }
  const int section_top = top + Scaled(92) + Scaled(20);
  {
    // Default Browser (the Mac's 74pt section), with a LIVE button.
    Item card;
    card.kind = ItemKind::kCard;
    card.bounds = {right_left, section_top, right_edge,
                   section_top + Scaled(74)};
    items_.push_back(std::move(card));
    if (!is_default_browser_.has_value()) {
      is_default_browser_ = PhiThisExeIsDefaultBrowser();
    }
    const bool is_default = *is_default_browser_;
    Item label;
    label.kind = ItemKind::kRowLabel;
    label.text =
        PhiStr(is_default ? "settings.account.defaultBrowser.defaultStatus"
                          : "settings.account.defaultBrowser.notDefaultStatus");
    label.bounds = {right_left + Scaled(kCardPadding), section_top,
                    right_edge - Scaled(kCardPadding + 124),
                    section_top + Scaled(74)};
    items_.push_back(std::move(label));
    Item button;
    button.kind = ItemKind::kButton;
    button.text = PhiStr("settings.account.defaultBrowser.setDefaultButton");
    button.action = "defaultbrowser";
    // Already default: the button stays visible but goes inert and dim, as
    // it does on Mac.
    button.enabled = !is_default;
    button.bounds = {right_edge - Scaled(kCardPadding) - Scaled(116),
                     section_top + Scaled(25),
                     right_edge - Scaled(kCardPadding),
                     section_top + Scaled(49)};
    items_.push_back(std::move(button));
  }
  *y = std::max(bottom, section_top + Scaled(74)) + Scaled(kSectionGap);
}

void PhiSettingsWindow::BuildAi(int* y) {
  AddToggleRow(y, PhiStr("settings.ai.features.enableToggle"), /*on=*/true,
               /*enabled=*/false, std::string());
  *y += Scaled(8);
  BeginSection(y, PhiStr("settings.ai.browserMemory.sectionTitle"));
  {
    const size_t first = items_.size();
    const RECT area =
        AddRow(y, PhiStr("settings.ai.browserMemory.manageButtonTitle"),
               kRowLabelHeight, 20);
    items_[first].enabled = false;
    Item chevron;
    chevron.kind = ItemKind::kIcon;
    chevron.text = std::wstring(1, wchar_t(0xE76C));  // ChevronRight
    chevron.enabled = false;
    chevron.bounds = area;
    items_.push_back(std::move(chevron));
  }
  EndSection(y);
  BeginSection(y, PhiStr("settings.ai.phiSentinel.sectionTitle"));
  AddToggleRow(y, PhiStr("settings.launchPhiSentinelAutomaticallyAt"), true,
               false, std::string());
  AddDividerRow(y);
  {
    const size_t first = items_.size();
    const RECT area =
        AddRow(y, PhiStr("settings.ai.privateAI.title"), kRowLabelHeight, 20);
    items_[first].enabled = false;
    Item chevron;
    chevron.kind = ItemKind::kIcon;
    chevron.text = std::wstring(1, wchar_t(0xE76C));  // ChevronRight
    chevron.enabled = false;
    chevron.bounds = area;
    items_.push_back(std::move(chevron));
  }
  EndSection(y);
  BeginSection(y, PhiStr("settings.ai.newTabPage.sectionTitle"));
  AddToggleRow(y, PhiStr("settings.ai.newTabPage.proactiveSuggestionsToggle"),
               true, false, std::string());
  EndSection(y);
  BeginSection(y, PhiStr("settings.ai.sidebar.sectionTitle"));
  AddToggleRow(y, PhiStr("settings.ai.sidebar.includeCurrentTabToggle"), true,
               false, std::string());
  EndSection(y);
  BeginSection(y, PhiStr("settings.ai.connectors.listSectionTitle"));
  AddToggleRow(y, PhiStr("settings.ai.connectors.enableToggle"), true, false,
               std::string(), PhiStr("settings.ai.connectors.description"));
  EndSection(y);
}

void PhiSettingsWindow::BuildShortcuts(int* y) {
  // The Mac Shortcuts pane predates the shared card system: the header row
  // (title + "Restore all shortcuts") sits OUTSIDE one full-height card
  // that is WHITE in light mode, with the search field and the grouped
  // shortcut table inside.
  {
    Item heading;
    heading.kind = ItemKind::kSectionTitle;
    heading.text = PhiStr("settings.navigation.shortcutsTitle");
    heading.bounds = {content_left_, *y, content_left_ + Scaled(200),
                      *y + Scaled(kSectionTitleHeight)};
    items_.push_back(std::move(heading));
    Item restore;
    restore.kind = ItemKind::kButton;
    restore.text = PhiStr("settings.shortcuts.restoreDefaultsButton");
    restore.enabled = prefs_->AnyShortcutCustomized();
    restore.action = "restoreshortcuts";
    restore.bounds = {content_right_ - Scaled(150),
                      *y - Scaled(4),
                      content_right_,
                      *y - Scaled(4) + Scaled(22)};
    items_.push_back(std::move(restore));
    *y += Scaled(kSectionTitleHeight) + Scaled(10);
  }
  Item card;
  card.kind = ItemKind::kCard;
  card.on = true;  // The white variant.
  card.bounds = {content_left_, *y, content_right_, 0};
  items_.push_back(std::move(card));
  const size_t card_index = items_.size() - 1;

  // The in-card filter ("Find shortcut keys"): typing anywhere while this
  // pane is up feeds it (WM_CHAR), Esc clears, Backspace deletes.
  {
    Item search;
    search.kind = ItemKind::kSearchField;
    search.text = shortcut_search_;
    search.detail = PhiStr("settings.shortcuts.searchField.placeholder");
    search.bounds = {content_left_, *y, content_right_,
                     *y + Scaled(40)};
    items_.push_back(std::move(search));
    *y += Scaled(40);
    Item divider;
    divider.kind = ItemKind::kDivider;
    divider.bounds = {content_left_, *y, content_right_, *y + 1};
    items_.push_back(std::move(divider));
    *y += 1;
  }

  // The table, grouped as on Mac (App, File, Edit, View, History,
  // Bookmarks, Window, Tab, Spaces). The preferences service serves the
  // rows: rebindable ones (clickable keycap → the chord recorder) carry a
  // stable name and their EFFECTIVE chord; fixed rows are informational.
  // Plus the live "Go to Space" rows for the first nine Spaces, which are
  // the pane's own and belong to no shortcut table.
  struct ShortcutRowDef {
    std::wstring group;
    std::wstring name;
    std::wstring keys;
    std::string id;  // Rebindable stable name; empty = fixed.
    bool customized = false;
  };
  std::vector<ShortcutRowDef> rows;
  // `group` is the table's ASCII group id (kPhiShortcutGroups); the heading
  // spells it through the Mac's settings.shortcuts.group.<id> key below.
  for (const PhiShortcutRow& row : prefs_->ShortcutRows()) {
    rows.push_back({base::UTF8ToWide(row.group), base::UTF8ToWide(row.label),
                    base::UTF8ToWide(row.keys), row.name, row.customized});
  }
  {
    // The "Go to Space" rows join the table's own Spaces group.
    int at = 0;
    for (const PhiSpaceSummary& space : spaces_->Summaries()) {
      if (++at > 9) {
        break;
      }
      rows.push_back(
          {L"Spaces",
           PhiStrF("settings.shortcuts.activateSpaceCommandTitle",
                   {base::UTF8ToWide(space.name)}),
           std::wstring(PhiStr("sidebar.ctrl")) + L"+" +
               std::to_wstring(at)});
    }
  }

  std::wstring filter = shortcut_search_;
  for (wchar_t& c : filter) {
    c = towlower(c);
  }
  const auto matches = [&](const ShortcutRowDef& row) {
    if (filter.empty()) {
      return true;
    }
    std::wstring name = row.name, keys = row.keys;
    for (wchar_t& c : name) {
      c = towlower(c);
    }
    for (wchar_t& c : keys) {
      c = towlower(c);
    }
    return name.find(filter) != std::wstring::npos ||
           keys.find(filter) != std::wstring::npos;
  };

  const int row_h = Scaled(42);
  std::wstring group;
  bool any = false;
  size_t last_rule = std::numeric_limits<size_t>::max();
  for (const auto& row : rows) {
    if (!matches(row)) {
      continue;
    }
    any = true;
    if (group != row.group) {
      group = row.group;
      // Category title (12pt semibold secondary, 35pt band) over a rule —
      // the Mac's group key, so every heading is translated.
      std::string group_id = base::WideToUTF8(row.group);
      for (char& c : group_id) {
        if (c >= 'A' && c <= 'Z') {
          c = static_cast<char>(c - 'A' + 'a');
        }
      }
      Item heading;
      heading.kind = ItemKind::kSectionTitle;
      heading.text =
          PhiStr(("settings.shortcuts.group." + group_id).c_str());
      heading.bounds = {(content_left_ + Scaled(kCardPadding)), *y,
                        (content_right_ - Scaled(kCardPadding)),
                        *y + Scaled(35)};
      items_.push_back(std::move(heading));
      *y += Scaled(35);
      Item rule;
      rule.kind = ItemKind::kDivider;
      rule.bounds = {(content_left_ + Scaled(kCardPadding)), *y,
                     (content_right_ - Scaled(kCardPadding)), *y + 1};
      items_.push_back(std::move(rule));
      *y += 1;
    }
    // One pill holding the whole chord (min width 80), the Mac's keycap.
    // A rebindable row's keycap opens the chord recorder; `detail` carries
    // the label for the prompt's message, `on` marks a customised chord.
    const int cap_w =
        std::max(Scaled(80),
                 Scaled(static_cast<int>(row.keys.size()) * 7 + 24));
    Item cap;
    cap.kind = ItemKind::kKeycap;
    cap.text = row.keys;
    cap.on = row.customized;
    if (!row.id.empty()) {
      cap.action = "shortcut";
      cap.payload = row.id;
      cap.detail = row.name;
    }
    cap.bounds = {(content_right_ - Scaled(kCardPadding)) - cap_w,
                  *y + (row_h - Scaled(22)) / 2,
                  (content_right_ - Scaled(kCardPadding)),
                  *y + (row_h + Scaled(22)) / 2};
    int label_right = cap.bounds.left - Scaled(12);
    // A customised row's "Reset" before its keycap — the Mac's per-row
    // button: erases that one override, no confirmation.
    std::optional<Item> reset;
    if (row.customized && !row.id.empty()) {
      reset.emplace();
      reset->kind = ItemKind::kButton;
      reset->text = PhiStr("settings.shortcuts.restoreShortcutButton");
      reset->action = "restoreshortcut";
      reset->payload = row.id;
      const int reset_w = std::max(
          Scaled(56),
          Scaled(static_cast<int>(reset->text.size()) * 7 + 24));
      reset->bounds = {cap.bounds.left - Scaled(8) - reset_w, cap.bounds.top,
                       cap.bounds.left - Scaled(8), cap.bounds.bottom};
      label_right = reset->bounds.left - Scaled(12);
    }
    Item label;
    label.kind = ItemKind::kRowLabel;
    label.text = row.name;
    label.bounds = {(content_left_ + Scaled(kCardPadding)), *y, label_right,
                    *y + row_h};
    items_.push_back(std::move(label));
    if (reset) {
      items_.push_back(std::move(*reset));
    }
    items_.push_back(std::move(cap));
    *y += row_h;
    Item rule;
    rule.kind = ItemKind::kDivider;
    rule.bounds = {(content_left_ + Scaled(kCardPadding)), *y,
                   (content_right_ - Scaled(kCardPadding)), *y + 1};
    items_.push_back(std::move(rule));
    last_rule = items_.size() - 1;
    *y += 1;
  }
  // The table's rules go BETWEEN rows; the loop left one hanging under the
  // last row, inside the card's own bottom padding.
  if (last_rule != std::numeric_limits<size_t>::max()) {
    items_.erase(items_.begin() + static_cast<ptrdiff_t>(last_rule));
    *y -= 1;
  }
  if (!any) {
    Item empty;
    empty.kind = ItemKind::kHint;
    empty.text = PhiStr("settings.shortcuts.searchResults.emptyMessage");
    empty.bounds = {content_left_, *y + Scaled(40),
                    content_right_, *y + Scaled(40) + Scaled(16)};
    items_.push_back(std::move(empty));
    *y += Scaled(96);
  }
  *y += Scaled(8);
  items_[card_index].bounds.bottom = *y;
  *y += Scaled(kSectionGap);
}

void PhiSettingsWindow::BuildDeveloper(int* y) {
  // The Mac's two cards under one "Agent control" title: the ACCESS card
  // (the master gate) and, under it, the PERMISSIONS card — what an
  // approved agent may touch. Both are chip-led, which is the visual
  // language the whole Mac Developer pane shares.
  BeginSection(y, PhiStr("settings.developer.agentControl.sectionTitle"));
  // The master switch. Windows has no code-signature identity for a pipe
  // client, so there is no per-program approval list under it: on, every
  // local program holding the launch token is admitted without a prompt;
  // off, the first one to connect asks the user to turn this on.
  AddToggleRow(y, PhiStr("settings.developer.agentControl.enableToggle"),
               prefs_->AllowAgentControl(), /*enabled=*/true, "agentcontrol",
               PhiStr("settings.developer.agentControl.descriptionWindows"),
               kChipIndent, 0xE735, 0);
  if (prefs_->AllowAgentControl()) {
    // Under the gate, the Mac's SkillInstallRowView: the phi-browser
    // skill shipped beside the exe, linked into an agent's skills folder
    // from a menu of agents ("Add skill to…"). Shown only while agents
    // are allowed in, as on Mac.
    AddDividerRow(y);
    AddPopupRow(y, PhiStr("settings.developer.skillInstall.sectionTitle"),
                PhiStr("settings.developer.skillInstall.targetMenuButton"),
                "installskill",
                PhiStr("settings.developer.skillInstall.description"),
                kChipIndent, 0xEA86);
  }
  EndSection(y);

  BeginSection(y, nullptr);
  // The user-space gate: off, a connected agent works only inside its own
  // agent Spaces; on, the management verbs reach the user's Spaces too.
  AddToggleRow(
      y, PhiStr("settings.developer.agentBrowsingDataAccess.enableToggle"),
      prefs_->AllowAgentUserSpaces(), /*enabled=*/true, "agentuserspaces",
      PhiStr("settings.developer.agentBrowsingDataAccess.descriptionWindows"),
      kChipIndent, 0xE8A9, 0);
  AddDividerRow(y);
  // "Profiles agents can use": a chip-led header, then one switch per
  // profile INDENTED to the header's text column, exactly as the Mac's
  // profilesRows does with .padding(.leading, 36).
  AddRow(y, PhiStr("settings.developer.agentProfileAccess.title"),
         kRowLabelHeight, 0,
         PhiStr("settings.developer.agentProfileAccess.description"),
         kChipIndent, 0xE716, 0);
  const std::vector<PhiProfileSummary> profiles = profiles_->Profiles();
  if (profiles.empty()) {
    AddSubHint(y, PhiStr("settings.developer.agentProfileAccess.emptyMessage"));
  } else {
    for (const PhiProfileSummary& profile : profiles) {
      const size_t first = items_.size();
      AddToggleRow(y, base::UTF8ToWide(profile.name).c_str(),
                   prefs_->AgentProfileAllowed(profile.id),
                   /*enabled=*/true, "agentprofile", nullptr, kChipIndent,
                   /*chip_glyph=*/0, /*chip_color=*/0, /*row_padding=*/6);
      // The switch is the item AddToggleRow pushed last; the profile is
      // its identity, so a list that moves cannot revoke a neighbour.
      for (size_t i = first; i < items_.size(); ++i) {
        if (items_[i].kind == ItemKind::kToggle) {
          items_[i].payload = profile.id;
        }
      }
    }
  }
  EndSection(y);
}

void PhiSettingsWindow::Rebuild() {
  items_.clear();
  open_card_ = 0;
  // The Spaces card's inner scrollers exist only while a build lays it out.
  list_rect_ = {};
  grid_rect_ = {};
  if (!host_) {
    ClearHits();
    return;
  }
  // The navigation rail down the left (§7): 280 wide, 48 under the 900
  // breakpoint (icons only), 36 dip items with a 16 icon and a 14 label.
  // Two pages are not always mounted: Developer outside developer mode,
  // and Phi & AI while the AI suite is off (phi_features.h).
  const bool developer = prefs_->DeveloperMode();
  const auto unmounted = [developer](const char* id) {
    const std::string name(id);
    return (!developer && name == "developer") ||
           (!kPhiAiEnabled && name == "ai");
  };
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  const bool narrow = client.right < Scaled(kRailBreakpoint);
  rail_w_ = Scaled(narrow ? kRailNarrow : kRailWidth);
  content_left_ = rail_w_ + Scaled(kContentLeftGap);
  content_right_ = std::max(content_left_ + Scaled(200),
                            std::min(static_cast<int>(client.right) - Scaled(kContentRightPad),
                                     content_left_ + Scaled(kContentMaxWidth)));
  const int rail_left = Scaled(narrow ? 4 : kRailPadLeft);
  const int rail_right = rail_w_ - Scaled(narrow ? 4 : kRailPadRight);
  int rail_y = Scaled(kCaptionHeight + 8);
  if (!narrow) {
    // The account block leads the rail (design "Settings window"): the
    // 44 avatar, the name over the email; it IS the Account page's rail
    // entry. Then "Find a setting" (§7), fed by WM_CHAR on every page but
    // Shortcuts (which has its own). Both are built before the page items
    // so they paint with the rail, outside the content clip — the field
    // FIRST: build order is the Tab order, and the account block opens the
    // run of page entries the arrow keys walk (FocusOrder).
    std::string account_name;
    std::string account_email;
    const bool signed_in =
        account_->AccountInfo(&account_name, &account_email);
    Item account;
    account.kind = ItemKind::kAccountBlock;
    account.text = signed_in && !account_name.empty()
                       ? base::UTF8ToWide(account_name)
                       : std::wstring(
                             PhiStr("settings.account.displayName.fallback"));
    account.detail = signed_in
                         ? base::UTF8ToWide(account_email)
                         : std::wstring(PhiStr("settings.rail.notSignedIn"));
    account.payload = "account";
    account.selected = section_ == "account";
    account.action = "section";
    account.bounds = {rail_left, rail_y, rail_right,
                      rail_y + Scaled(kRailAccountHeight)};
    rail_y += Scaled(kRailAccountHeight);
    Item search;
    search.kind = ItemKind::kSearchField;
    search.text = rail_search_;
    search.detail = PhiStr("settings.search.placeholder");
    search.on = true;  // Draw its own field (the rail has no card).
    search.bounds = {rail_left + Scaled(kRailSearchInset), rail_y,
                     rail_right - Scaled(kRailSearchInset),
                     rail_y + Scaled(kRailSearchHeight)};
    items_.push_back(std::move(search));
    items_.push_back(std::move(account));
    rail_y += Scaled(kRailSearchHeight + 12);
  }
  for (const SectionDef& section : kSections) {
    if (unmounted(section.id)) {
      continue;
    }
    if (!narrow && std::string(section.id) == "account") {
      continue;  // The account block above is its entry.
    }
    Item entry;
    entry.kind = ItemKind::kToolbarItem;
    entry.text = narrow ? std::wstring() : PhiStr(section.label);
    entry.detail = PhiStr(section.label);  // The tooltip / narrow rail's name.
    entry.payload = section.id;
    entry.selected = section_ == section.id;
    entry.action = "section";
    entry.bounds = {rail_left, rail_y, rail_right,
                    rail_y + Scaled(kRailItemHeight)};
    items_.push_back(std::move(entry));
    rail_y += Scaled(kRailItemHeight + kRailItemGap);
  }
  if (unmounted(section_.c_str())) {
    section_ = "general";  // The page vanished under us, as on Mac.
    SyncTitle();
  }

  search_filter_.clear();
  filter_hits_ = 0;
  last_row_skipped_ = false;
  heading_pending_ = false;
  const bool searching = !rail_search_.empty() && section_ != "shortcuts";
  if (searching) {
    search_filter_ = rail_search_;
    for (wchar_t& c : search_filter_) {
      c = static_cast<wchar_t>(::towlower(c));
    }
  }
  int y = Scaled(kChromeTop + kContentTop) - scroll_;
  const int top = y;
  {
    // The page title (28/600), then the groups.
    Item title;
    title.kind = ItemKind::kPageTitle;
    for (const SectionDef& section : kSections) {
      if (section_ == section.id) {
        title.text = PhiStr(section.label);
      }
    }
    if (searching) {
      title.text = PhiStrF("settings.search.resultsTitle", {rail_search_});
    }
    // The results page carries its own way out: on the narrow rail there
    // is no search field to show (or clear) what was typed, and typing
    // still filters, so the header names the query and offers Clear.
    const int clear_w = searching ? Scaled(96) : 0;
    title.bounds = {content_left_, y,
                    content_right_ - (searching ? clear_w + Scaled(12) : 0),
                    y + Scaled(kPageTitleHeight)};
    items_.push_back(std::move(title));
    if (searching) {
      Item clear;
      clear.kind = ItemKind::kButton;
      clear.text = PhiStr("settings.search.clear");
      clear.action = "clearsearch";
      const int mid = y + Scaled(kPageTitleHeight) / 2;
      clear.bounds = {content_right_ - clear_w, mid - Scaled(kPillHeight) / 2,
                      content_right_, mid + Scaled(kPillHeight) / 2};
      items_.push_back(std::move(clear));
    }
    y += Scaled(kPageTitleHeight + kPageTitleGap);
  }
  if (searching) {
    BuildSearchResults(&y);
  } else if (section_ == "general") {
    BuildGeneral(&y);
  } else if (section_ == "appearance") {
    BuildAppearance(&y);
  } else if (section_ == "about") {
    BuildAbout(&y);
  } else if (section_ == "spaces") {
    BuildSpaces(&y);
  } else if (section_ == "navigation") {
    BuildNavigation(&y);
  } else if (section_ == "profiles") {
    BuildProfiles(&y);
  } else if (section_ == "shortcuts") {
    BuildShortcuts(&y);
  } else if (section_ == "ai") {
    BuildAi(&y);
  } else if (section_ == "account") {
    BuildAccount(&y);
  } else if (section_ == "advanced") {
    BuildAdvanced(&y);
  } else if (section_ == "developer") {
    BuildDeveloper(&y);
  } else if (section_ == "sync") {
    BuildSync(&y);
  } else {
    section_ = "general";  // A retired pane id (e.g. "phiLink").
    BuildGeneral(&y);
  }
  // The running `y` carries the trailing section gap and, on the
  // master-detail panes, none of a detail column that outgrows the band —
  // so measure the content by what was actually laid out. It decides both
  // the scroll limit and the indicator's thumb.
  int bottom = top;
  for (const Item& item : items_) {
    // The rail never scrolls, so none of it measures the page.
    if (!InRail(item)) {
      bottom = std::max<int>(bottom, item.bounds.bottom);
    }
  }
  content_height_ = bottom - top + Scaled(kContentTop);
  // The offset was clamped only by the wheel and the thumb drag, so a page
  // that SHRANK (a filter, a folded expander, another page) or a window
  // that GREW (WM_SIZE rebuilds) kept a scroll past the end and showed a
  // blank band. Pull it back and lay out once more — the content's height
  // does not depend on the offset, so the second pass is the last.
  const int max_scroll = MaxScroll();
  if (scroll_ > max_scroll) {
    scroll_ = max_scroll;
    Rebuild();
    return;
  }
  PublishHits();
  ResolveFocus();
}

// The results page (§7 "Find a setting"): every page's rows that carry
// the typed text, under that page's heading — the page builders run with
// the filter up, and AddRow / EndSection keep only what matches.
void PhiSettingsWindow::BuildSearchResults(int* y) {
  const bool developer = prefs_->DeveloperMode();
  struct Page {
    const char* id;
    void (PhiSettingsWindow::*build)(int*);
  };
  const Page kPages[] = {
      {"general", &PhiSettingsWindow::BuildGeneral},
      {"appearance", &PhiSettingsWindow::BuildAppearance},
      {"spaces", &PhiSettingsWindow::BuildSpaces},
      {"profiles", &PhiSettingsWindow::BuildProfiles},
      {"navigation", &PhiSettingsWindow::BuildNavigation},
      {"ai", &PhiSettingsWindow::BuildAi},
      {"advanced", &PhiSettingsWindow::BuildAdvanced},
      {"developer", &PhiSettingsWindow::BuildDeveloper},
      {"about", &PhiSettingsWindow::BuildAbout},
  };
  int total = 0;
  for (const Page& page : kPages) {
    const std::string id(page.id);
    if ((id == "developer" && !developer) || (id == "ai" && !kPhiAiEnabled)) {
      continue;
    }
    const size_t start = items_.size();
    const int start_y = *y;
    Item heading;
    heading.kind = ItemKind::kSectionTitle;
    for (const SectionDef& section : kSections) {
      if (id == section.id) {
        heading.text = PhiStr(section.label);
      }
    }
    heading.bounds = {content_left_, *y, content_right_,
                      *y + Scaled(kSectionTitleHeight)};
    items_.push_back(std::move(heading));
    *y += Scaled(kSectionTitleHeight) + Scaled(kTitleToCard);
    filter_hits_ = 0;
    (this->*page.build)(y);
    if (filter_hits_ == 0) {
      items_.resize(start);
      *y = start_y;
      continue;
    }
    total += filter_hits_;
  }
  if (total == 0) {
    Item empty;
    empty.kind = ItemKind::kHint;
    empty.text = PhiStrF("settings.search.noResults", {rail_search_});
    empty.bounds = {content_left_, *y, content_right_,
                    *y + Scaled(kHintHeight)};
    items_.push_back(std::move(empty));
    *y += Scaled(kHintHeight);
  }
}

// The base's hit list is THE rect list (phi_window.h): hover, clicks,
// tooltips and UI Automation all read it and nothing else. This window used
// to hit-test items_ by hand from its own WM_LBUTTONDOWN, which is why it
// had no hover state and answered a screen reader with an empty window.
void PhiSettingsWindow::PublishHits() {
  ClearHits();
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  const int content_top = Scaled(kChromeTop) + 1;
  // The row label a bare trailing control (a switch, a slider, a pill)
  // trails, inside the same card.
  const auto row_label = [this](size_t i) {
    for (size_t j = i; j-- > 0;) {
      if (items_[j].kind == ItemKind::kCard) {
        break;
      }
      if (items_[j].kind == ItemKind::kRowLabel) {
        return items_[j].text;
      }
    }
    return std::wstring();
  };
  for (size_t i = 0; i < items_.size(); ++i) {
    const Item& item = items_[i];
    const bool text_input = IsTextInput(item);
    if (item.action.empty() && !text_input) {
      continue;
    }
    // A control whose row the "Find a setting" filter dropped sits on an
    // empty rect: it was never laid out, so it is not on the page at all.
    if (item.bounds.right <= item.bounds.left ||
        item.bounds.bottom <= item.bounds.top) {
      continue;
    }
    // Pane content scrolls UNDER the caption band and off the bottom; the
    // paint pass clips it there, so the hit list has to as well. PhiHitAt
    // takes the LAST match, and content is added after the rail — an
    // unclipped row that had scrolled up would take clicks above the page.
    //
    // A row scrolled ENTIRELY out of the page stays in the list all the
    // same (it used to be dropped, so a screen reader's view of the page
    // was whatever happened to be on screen): it is published with an
    // EMPTY rect at its true top edge, which PhiUiaIsOffscreen reports off
    // screen and PhiHitAt can never land on. UIA's Invoke on one scrolls
    // it into view first (InvokeHit).
    RECT bounds = item.bounds;
    if (item.kind != ItemKind::kToolbarItem) {
      bounds.top = std::max<LONG>(bounds.top, content_top);
      bounds.bottom = std::min<LONG>(bounds.bottom, client.bottom);
      if (bounds.bottom <= bounds.top) {
        bounds = item.bounds;
        bounds.bottom = bounds.top;
      }
    }
    std::wstring name = item.text.empty() ? item.detail : item.text;
    if (!item.a11y.empty()) {
      name = item.a11y;  // The drawn text is not the name (or there is none).
    }
    switch (item.kind) {
      case ItemKind::kSwatch:
        // Every theme dot used to take its row's label, so a screen reader
        // read eight "Space color"s; each one is its theme.
        name = ThemeDisplayName(item.payload);
        break;
      case ItemKind::kSliderTrack:
        // No RangeValue pattern (PhiHit carries no range), so the value
        // rides the name — and a keyboard step renames it, which UIA
        // announces.
        name = PhiStrF("settings.a11y.sliderValue",
                       {item.a11y.empty() ? row_label(i) : item.a11y,
                        PhiFormatNumber(item.value)});
        break;
      case ItemKind::kIconSearch:
        name = item.detail;  // The placeholder names the field.
        break;
      case ItemKind::kPopup: {
        // A ComboBox announces its label AND its current choice.
        const std::wstring label = row_label(i);
        if (!label.empty()) {
          name = PhiStrF("settings.a11y.labelValue", {label, item.text});
        }
        break;
      }
      case ItemKind::kSearchField:
        if (text_input) {
          name = item.detail;  // The field's placeholder names it.
        } else {
          // A value field (an expander body's Name): label and value.
          name = PhiStrF("settings.a11y.labelValue",
                         {row_label(i), item.text});
        }
        break;
      case ItemKind::kButton:
        // "+ New Space": the leading icon-face glyph is paint, not words.
        if (!name.empty() && PhiIsPrivateGlyph(name.substr(0, 1))) {
          const size_t words = name.find_first_not_of(L' ', 1);
          name = words == std::wstring::npos ? std::wstring()
                                             : name.substr(words);
        }
        break;
      default:
        break;
    }
    if (name.empty()) {
      // A row rect that sits BEHIND its own content is named by the label
      // inside it ("Additional browser settings", the data links).
      for (size_t j = i + 1; j < items_.size(); ++j) {
        if (items_[j].kind == ItemKind::kCard) {
          break;
        }
        if (items_[j].kind == ItemKind::kRowLabel &&
            items_[j].bounds.top >= item.bounds.top &&
            items_[j].bounds.bottom <= item.bounds.bottom) {
          name = items_[j].text;
          break;
        }
      }
    }
    if (name.empty()) {
      name = row_label(i);
    }
    PhiHitRole role = PhiHitRole::kButton;
    switch (item.kind) {
      case ItemKind::kToolbarItem:
      case ItemKind::kAccountBlock:  // The Account page's rail entry.
        role = PhiHitRole::kTab;
        break;
      case ItemKind::kSearchField:
        // The provider has no Edit type; a text input is announced by its
        // name and takes the focus, but has nothing to invoke.
        role = text_input ? PhiHitRole::kNone : PhiHitRole::kButton;
        break;
      case ItemKind::kToggle:
        role = PhiHitRole::kToggle;
        break;
      case ItemKind::kSliderTrack:
        role = PhiHitRole::kSlider;
        break;
      case ItemKind::kListRow:
      case ItemKind::kSwatch:
      case ItemKind::kImageCard:
      case ItemKind::kSpaceRow:
      case ItemKind::kPickerCell:
        role = PhiHitRole::kListItem;
        break;
      case ItemKind::kSegment:
        role = PhiHitRole::kTab;
        break;
      case ItemKind::kIconSearch:
        role = PhiHitRole::kNone;  // A text input, as the rail's field.
        break;
      default:
        break;
    }
    AddHit(bounds, static_cast<int>(i), std::move(name), role,
           item.kind == ItemKind::kToggle ? item.on : item.selected,
           item.enabled);
  }
}

int PhiSettingsWindow::ItemOfHit(int hit) const {
  if (hit < 0 || hit >= static_cast<int>(hits().size())) {
    return -1;
  }
  const int index = hits()[static_cast<size_t>(hit)].key;
  return index >= 0 && index < static_cast<int>(items_.size()) ? index : -1;
}

int PhiSettingsWindow::HitOfItem(int item) const {
  if (item < 0) {
    return -1;
  }
  const std::vector<PhiHit>& list = hits();
  for (size_t i = 0; i < list.size(); ++i) {
    if (list[i].key == item) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool PhiSettingsWindow::InvokeHit(int index) {
  // The scroll is a rebuild, but only of positions: the same state builds
  // the same items in the same order, so the item index (the hit's key)
  // still names the control afterwards.
  const int item = ItemOfHit(index);
  if (item >= 0 && ScrollItemIntoView(item)) {
    index = HitOfItem(item);
  }
  return PhiWindow::InvokeHit(index);
}

// --- Keyboard focus ----------------------------------------------------------
//
// The window draws every control, so there is no child HWND for the dialog
// manager to move the focus between (dialog_keys stays kPhiDialogNone): the
// focus is one items_ index, moved by HandleKey, drawn as the Windows 11
// focus ring in Paint and reported to UI Automation (PhiUiaSetFocusedHit).

bool PhiSettingsWindow::IsTextInput(const Item& item) const {
  return (item.kind == ItemKind::kSearchField && item.action.empty()) ||
         item.kind == ItemKind::kIconSearch;
}

bool PhiSettingsWindow::InRail(const Item& item) const {
  return item.kind == ItemKind::kToolbarItem ||
         item.kind == ItemKind::kAccountBlock ||
         (IsTextInput(item) && item.on);
}

bool PhiSettingsWindow::Focusable(const Item& item) const {
  if (!item.enabled || item.bounds.right <= item.bounds.left ||
      item.bounds.bottom <= item.bounds.top) {
    return false;  // Dim and inert, or a row the filter never laid out.
  }
  if (IsTextInput(item)) {
    return true;
  }
  // The profile card's "click" is its drag; there is nothing to activate.
  return !item.action.empty() && item.kind != ItemKind::kProfileCard;
}

bool PhiSettingsWindow::SameFocusGroup(const Item& a, const Item& b) const {
  const auto rail_entry = [](const Item& item) {
    return item.kind == ItemKind::kToolbarItem ||
           item.kind == ItemKind::kAccountBlock;
  };
  if (rail_entry(a) || rail_entry(b)) {
    return rail_entry(a) && rail_entry(b);  // The pages: Up / Down.
  }
  // One row of theme dots, one picture-card picker: Left / Right. The
  // Spaces list (Up / Down), the Icon | Emoji pair and the picker grid
  // are groups the same way.
  const bool grouped = a.kind == ItemKind::kSwatch ||
                       a.kind == ItemKind::kImageCard ||
                       a.kind == ItemKind::kLayoutCard ||
                       a.kind == ItemKind::kSpaceRow ||
                       a.kind == ItemKind::kSegment ||
                       a.kind == ItemKind::kPickerCell;
  return grouped && a.kind == b.kind && a.action == b.action &&
         a.space == b.space;
}

void PhiSettingsWindow::FocusOrder(std::vector<int>* order,
                                   std::vector<int>* group) const {
  order->clear();
  group->clear();
  int run = -1;
  for (size_t i = 0; i < items_.size(); ++i) {
    if (!Focusable(items_[i]) || !items_[i].tab_stop) {
      continue;
    }
    // A group is a RUN of neighbours in build order; the same kind of
    // control further down the page is a group of its own.
    if (order->empty() ||
        !SameFocusGroup(items_[static_cast<size_t>(order->back())],
                        items_[i])) {
      ++run;
    }
    order->push_back(static_cast<int>(i));
    group->push_back(run);
  }
}

PhiSettingsWindow::FocusKey PhiSettingsWindow::FocusKeyOf(int item) const {
  FocusKey key;
  if (item < 0 || item >= static_cast<int>(items_.size())) {
    return key;
  }
  const Item& target = items_[static_cast<size_t>(item)];
  key.kind = target.kind;
  key.action = target.action;
  key.payload = target.payload;
  key.space = target.space;
  // `value` tells the picture cards apart (it is their mode) and the two
  // search fields (the rail's is `on`); a slider's value is its position,
  // which moves under the focus, so it is not part of the identity.
  key.value = target.kind == ItemKind::kImageCard ||
                      target.kind == ItemKind::kLayoutCard
                  ? target.value
              : target.kind == ItemKind::kSearchField ? (target.on ? 1 : 0)
                                                      : 0;
  for (int i = 0; i < item; ++i) {
    if (MatchesFocusKey(items_[static_cast<size_t>(i)], key)) {
      ++key.ordinal;
    }
  }
  return key;
}

bool PhiSettingsWindow::MatchesFocusKey(const Item& item,
                                        const FocusKey& key) const {
  if (item.kind != key.kind || item.action != key.action ||
      item.payload != key.payload || item.space != key.space) {
    return false;
  }
  if (item.kind == ItemKind::kImageCard ||
      item.kind == ItemKind::kLayoutCard) {
    return item.value == key.value;
  }
  if (item.kind == ItemKind::kSearchField) {
    return (item.on ? 1 : 0) == key.value;
  }
  return true;
}

void PhiSettingsWindow::ResolveFocus() {
  if (focus_ >= 0) {
    int found = -1;
    int ordinal = 0;
    for (size_t i = 0; i < items_.size(); ++i) {
      if (!MatchesFocusKey(items_[i], focus_key_)) {
        continue;
      }
      if (ordinal++ == focus_key_.ordinal) {
        found = static_cast<int>(i);
        break;
      }
    }
    if (found < 0 || !Focusable(items_[static_cast<size_t>(found)])) {
      // The focused control went away (a deleted Space, a page the filter
      // replaced) or went inert: the next focusable item from where it
      // was, so Tab carries on from about the same place.
      const int count = static_cast<int>(items_.size());
      found = -1;
      for (int i = std::min(focus_, count - 1); i >= 0 && i < count; ++i) {
        if (Focusable(items_[static_cast<size_t>(i)])) {
          found = i;
          break;
        }
      }
      for (int i = count - 1; found < 0 && i >= 0; --i) {
        if (Focusable(items_[static_cast<size_t>(i)])) {
          found = i;
        }
      }
      focus_key_ = FocusKeyOf(found);
    }
    focus_ = found;
  }
  PhiUiaSetFocusedHit(this, HitOfItem(focus_), /*announce=*/false);
}

void PhiSettingsWindow::SetFocusItem(int item, bool keyboard) {
  if (item < 0 || item >= static_cast<int>(items_.size())) {
    return;
  }
  focus_ = item;
  focus_key_ = FocusKeyOf(item);
  if (keyboard) {
    focus_visible_ = true;
  }
  if (!ScrollItemIntoView(item)) {
    Invalidate();
  }
  PhiUiaSetFocusedHit(this, HitOfItem(focus_), /*announce=*/true);
}

void PhiSettingsWindow::TabFocus(bool backward) {
  std::vector<int> order;
  std::vector<int> group;
  FocusOrder(&order, &group);
  if (order.empty()) {
    return;
  }
  const int groups = group.back() + 1;
  int current = -1;
  for (size_t i = 0; i < order.size(); ++i) {
    if (order[i] == focus_) {
      current = group[i];
      break;
    }
  }
  int next = current < 0 ? (backward ? groups - 1 : 0)
                         : (current + (backward ? groups - 1 : 1)) % groups;
  if (current < 0 && focus_ >= 0) {
    // A focused item outside the Tab walk (a click on an unselected Space
    // row's grip or pill): carry on from where it sits.
    for (size_t i = 0; i < order.size(); ++i) {
      if (!backward && order[i] > focus_) {
        next = group[i];
        break;
      }
      if (backward && order[i] < focus_) {
        next = group[i];
      }
    }
  }
  // A group is entered on its chosen member (the page shown, the theme in
  // force), else its first — the Windows radio-group rule.
  int target = -1;
  for (size_t i = 0; i < order.size(); ++i) {
    if (group[i] != next) {
      continue;
    }
    if (target < 0) {
      target = order[i];
    }
    if (items_[static_cast<size_t>(order[i])].selected) {
      target = order[i];
      break;
    }
  }
  SetFocusItem(target, /*keyboard=*/true);
}

void PhiSettingsWindow::ArrowFocus(int delta) {
  std::vector<int> order;
  std::vector<int> group;
  FocusOrder(&order, &group);
  for (size_t i = 0; i < order.size(); ++i) {
    if (order[i] != focus_) {
      continue;
    }
    const int next = static_cast<int>(i) + delta;
    if (next >= 0 && next < static_cast<int>(order.size()) &&
        group[static_cast<size_t>(next)] == group[i]) {
      SetFocusItem(order[static_cast<size_t>(next)], /*keyboard=*/true);
    }
    return;
  }
}

int PhiSettingsWindow::MaxScroll() const {
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  const int visible = client.bottom - Scaled(kChromeTop);
  return std::max(0, content_height_ - visible);
}

void PhiSettingsWindow::ScrollTo(int scroll) {
  const int next = std::max(0, std::min(MaxScroll(), scroll));
  if (next != scroll_) {
    scroll_ = next;
    ScheduleRebuild();
  }
}

bool PhiSettingsWindow::ScrollItemIntoView(int item) {
  if (item < 0 || item >= static_cast<int>(items_.size())) {
    return false;
  }
  const Item& target = items_[static_cast<size_t>(item)];
  if (InRail(target)) {
    return false;  // The rail never scrolls.
  }
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  // A little air above and below, so the ring is not cut by the band.
  const int view_top = Scaled(kChromeTop) + Scaled(8);
  const int view_bottom = client.bottom - Scaled(8);
  int delta = 0;
  if (target.bounds.top < view_top) {
    delta = target.bounds.top - view_top;
  } else if (target.bounds.bottom > view_bottom) {
    // Never so far that a control taller than the page loses its top.
    delta = std::min(target.bounds.bottom - view_bottom,
                     target.bounds.top - view_top);
  }
  const int before = scroll_;
  ScrollTo(scroll_ + delta);
  return scroll_ != before;
}

void PhiSettingsWindow::StepSlider(const Item& item, int delta) {
  if (!item.enabled) {
    return;
  }
  // Copied out first: the host's write notifies the window, whose rebuild
  // replaces items_ — and `item` with it — before the call returns.
  const std::string action = item.action;
  const int value = std::max(0, std::min(100, item.value + delta));
  if (action == "sidebartransparency") {
    host_->SetSidebarTransparency(value);
  }
  ScheduleRebuild();
}

void PhiSettingsWindow::MoveSpaceSelection(int delta) {
  const std::vector<PhiSpaceSummary> spaces = spaces_->Summaries();
  int index = -1;
  for (size_t i = 0; i < spaces.size(); ++i) {
    if (spaces[i].id == selected_space_) {
      index = static_cast<int>(i);
    }
  }
  const int next = index + delta;
  if (index < 0 || next < 0 || next >= static_cast<int>(spaces.size())) {
    return;
  }
  const std::string target = spaces[static_cast<size_t>(next)].id;
  SelectSpace(target, /*reveal=*/true);
  ScheduleRebuild();
  for (size_t i = 0; i < items_.size(); ++i) {
    if (items_[i].kind == ItemKind::kSpaceRow && items_[i].payload == target) {
      SetFocusItem(static_cast<int>(i), /*keyboard=*/true);
      return;
    }
  }
}

void PhiSettingsWindow::ReorderByKey(int delta) {
  if (focus_ < 0 || focus_ >= static_cast<int>(items_.size())) {
    return;
  }
  const std::string id = items_[static_cast<size_t>(focus_)].payload;
  const std::vector<PhiSpaceSummary> spaces = spaces_->Summaries();
  int index = -1;
  for (size_t i = 0; i < spaces.size(); ++i) {
    if (spaces[i].id == id) {
      index = static_cast<int>(i);
    }
  }
  const int next = index + delta;
  if (index < 0 || next < 0 || next >= static_cast<int>(spaces.size())) {
    return;
  }
  SelectSpace(id, /*reveal=*/true);
  host_->ReorderSpace(id, next);
  // The coordinator does not notify Settings of an order-only change.
  ScheduleRebuild();
  for (size_t i = 0; i < items_.size(); ++i) {
    if (items_[i].kind == ItemKind::kDragHandle && items_[i].payload == id) {
      SetFocusItem(static_cast<int>(i), /*keyboard=*/true);
      return;
    }
  }
}

void PhiSettingsWindow::MoveIconFocus(int delta) {
  if (focus_ < 0 || focus_ >= static_cast<int>(items_.size()) ||
      grid_count_ <= 0) {
    return;
  }
  const int from = items_[static_cast<size_t>(focus_)].value;
  const int target = std::max(0, std::min(grid_count_ - 1, from + delta));
  if (target == from) {
    return;
  }
  // Walking off the grid's edge scrolls it a row at a time.
  const int row = target / kGridColumns;
  if (row < grid_first_row_) {
    grid_first_row_ = row;
  } else if (row >= grid_first_row_ + grid_visible_) {
    grid_first_row_ = row - grid_visible_ + 1;
  }
  ScheduleRebuild();
  for (size_t i = 0; i < items_.size(); ++i) {
    if (items_[i].kind == ItemKind::kPickerCell && items_[i].value == target) {
      SetFocusItem(static_cast<int>(i), /*keyboard=*/true);
      return;
    }
  }
}

int PhiSettingsWindow::ReorderIndexAt(int y) {
  const int count = static_cast<int>(reorder_order_.size());
  if (count == 0 || list_pitch_ <= 0) {
    return 0;
  }
  const int offset = y - list_rect_.top;
  int index = offset < 0 ? list_first_ - 1 : list_first_ + offset / list_pitch_;
  index = std::max(0, std::min(count - 1, index));
  // Past either edge of the list the rows scroll under the pointer, one
  // per move.
  if (index < list_first_) {
    list_first_ = index;
  } else if (index >= list_first_ + list_visible_) {
    list_first_ = index - list_visible_ + 1;
  }
  return index;
}

void PhiSettingsWindow::EndReorder(bool commit) {
  if (!reorder_drag_) {
    return;
  }
  reorder_drag_ = false;  // Before the release: it reports a capture loss.
  drag_.Release();
  const auto it =
      std::find(reorder_order_.begin(), reorder_order_.end(), reorder_id_);
  const int index = it == reorder_order_.end()
                        ? -1
                        : static_cast<int>(it - reorder_order_.begin());
  if (commit && index >= 0 && index != reorder_from_) {
    // The Mac writes the order once, on the drop.
    host_->ReorderSpace(reorder_id_, index);
  }
  reorder_order_.clear();
  reorder_id_.clear();
  reorder_from_ = -1;
  list_reveal_ = true;
  ScheduleRebuild();
}

void PhiSettingsWindow::ActivateFocus() {
  if (focus_ < 0 || focus_ >= static_cast<int>(items_.size())) {
    return;
  }
  // By value, as OnClick takes it: Apply() may open a modal menu or prompt
  // whose nested loop rebuilds items_ underneath us.
  const Item item = items_[static_cast<size_t>(focus_)];
  if (!item.enabled || item.action.empty() ||
      item.kind == ItemKind::kSliderTrack ||
      item.kind == ItemKind::kProfileCard) {
    return;  // A slider steps on the arrows; a text field just types.
  }
  Apply(item, (item.bounds.left + item.bounds.right) / 2);
  if (alive()) {
    ScheduleRebuild();
  }
}

int PhiSettingsWindow::SearchFieldItem(bool rail) const {
  for (size_t i = 0; i < items_.size(); ++i) {
    if (items_[i].kind == ItemKind::kSearchField && IsTextInput(items_[i]) &&
        items_[i].on == rail) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool PhiSettingsWindow::HandleKey(WPARAM virtual_key, LPARAM lparam,
                                  bool alt) {
  swallow_space_char_ = false;
  if (focus_ >= static_cast<int>(items_.size())) {
    focus_ = -1;
  }
  const Item* focused =
      focus_ >= 0 ? &items_[static_cast<size_t>(focus_)] : nullptr;
  const bool typing = focused && IsTextInput(*focused);
  const bool slider = focused && focused->kind == ItemKind::kSliderTrack;
  const bool repeat = (lparam & (1 << 30)) != 0;
  const bool ctrl = ::GetKeyState(VK_CONTROL) < 0;
  const bool shift = ::GetKeyState(VK_SHIFT) < 0;
  // Activation is POSTED: TranslateMessage has already queued the key's
  // WM_CHAR, and a menu or prompt opened from here would otherwise eat it
  // inside its own loop.
  const auto activate = [this, repeat]() {
    if (!repeat) {
      ::PostMessageW(hwnd(), kMsgFocusActivate, 0, 0);
    }
  };
  const auto show_ring = [this]() {
    if (!focus_visible_) {
      focus_visible_ = true;
      Invalidate();
    }
  };
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  const int page = std::max(Scaled(40), static_cast<int>(client.bottom) -
                                            Scaled(kChromeTop) - Scaled(60));
  if (alt) {
    // Alt+Down opens a ComboBox, as on Windows; every other Alt chord is
    // the system's (the window menu, Alt+F4).
    if (virtual_key == VK_DOWN && focused &&
        (focused->kind == ItemKind::kPopup ||
         focused->kind == ItemKind::kRowPopup)) {
      show_ring();
      activate();
      return true;
    }
    return false;
  }
  switch (virtual_key) {
    case VK_TAB:
      if (ctrl) {
        return false;
      }
      TabFocus(shift);
      return true;
    case VK_SPACE:
      if (!focused || typing) {
        return false;  // Types into the filter (WM_CHAR), as before.
      }
      swallow_space_char_ = true;
      show_ring();
      activate();
      return true;
    case VK_RETURN:
      if (focused && !typing) {
        show_ring();
        activate();
      }
      return true;
    case VK_F4:
      if (focused && (focused->kind == ItemKind::kPopup ||
                      focused->kind == ItemKind::kRowPopup)) {
        show_ring();
        activate();
        return true;
      }
      return false;
    case VK_LEFT:
    case VK_RIGHT: {
      const int step = virtual_key == VK_RIGHT ? 1 : -1;
      if (focused && focused->kind == ItemKind::kPickerCell) {
        show_ring();
        MoveIconFocus(step);
        return true;
      }
      if (focused && (focused->kind == ItemKind::kSpaceRow ||
                      focused->kind == ItemKind::kDragHandle)) {
        return true;  // A vertical list: Left / Right do nothing.
      }
      if (slider) {
        show_ring();
        StepSlider(*focused, 5 * step);
        return true;
      }
      if (focused && !InRail(*focused)) {
        show_ring();
        ArrowFocus(step);  // A no-op outside a dot row / card picker.
        return true;
      }
      return false;
    }
    case VK_UP:
    case VK_DOWN: {
      const int step = virtual_key == VK_DOWN ? 1 : -1;
      if (focused && focused->kind == ItemKind::kSpaceRow) {
        show_ring();
        MoveSpaceSelection(step);  // A list box: the arrow selects.
        return true;
      }
      if (focused && focused->kind == ItemKind::kDragHandle) {
        show_ring();
        ReorderByKey(step);  // The grip moves its Space.
        return true;
      }
      if (focused && focused->kind == ItemKind::kPickerCell) {
        show_ring();
        MoveIconFocus(step * kGridColumns);
        return true;
      }
      if (focused && InRail(*focused) && !typing) {
        show_ring();
        ArrowFocus(step);  // The pages.
        return true;
      }
      if (slider) {
        show_ring();
        StepSlider(*focused, -5 * step);  // Up raises, as on Windows.
        return true;
      }
      ScrollTo(scroll_ + step * Scaled(40));
      return true;
    }
    case VK_PRIOR:
      ScrollTo(scroll_ - page);
      return true;
    case VK_NEXT:
      ScrollTo(scroll_ + page);
      return true;
    case VK_HOME:
      ScrollTo(0);
      return true;
    case VK_END:
      ScrollTo(MaxScroll());
      return true;
    default:
      break;
  }
  return false;
}

void PhiSettingsWindow::Paint(HDC dc, const RECT&) {
  if (!host_) {
    return;
  }
  // The Mac's fixedWindowBackground: flat, theme-INdependent — #EAEAEA
  // light / #252525 dark. Only the appearance choice matters here.
  const bool dark = prefs_->Palette().dark;
  const COLORREF bg = dark ? RGB(0x20, 0x20, 0x20) : RGB(0xF3, 0xF3, 0xF3);
  const COLORREF ink = dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00);
  const COLORREF label = Blend(ink, bg, dark ? 230 : 222);
  const COLORREF secondary = Blend(ink, bg, 158);
  const COLORREF tertiary = Blend(ink, bg, 107);
  const COLORREF card_bg = dark ? RGB(0x2B, 0x2B, 0x2B) : RGB(0xFF, 0xFF, 0xFF);
  const COLORREF card_border = Blend(ink, bg, dark ? 20 : 23);
  const COLORREF separator = Blend(ink, bg, 18);
  // A row divider is drawn ON the card, not on the page: blended against
  // the page ground it came out ~4% over a card already 2% dark, which on
  // a screen is no line at all. AppKit's Divider is nearer 12%.
  const COLORREF rule_ink = Blend(ink, card_bg, 18);
  // The pointer's wash — the one cue every control here lacked.
  const COLORREF hover_wash = Blend(ink, bg, dark ? 15 : 11);
  const COLORREF accent = prefs_->Palette().accent;
  // Settings › Spaces (design node 9056:18105, light only; dark derived as
  // the same alphas of white): rules and the card's border at 10 %, text
  // at 85 %, the wells at 3 %, a selected row's content at white 85 %.
  const COLORREF white = RGB(0xFF, 0xFF, 0xFF);
  const COLORREF sp_text = Blend(ink, card_bg, 217);
  const COLORREF sp_secondary = Blend(ink, card_bg, 158);
  const COLORREF sp_on_accent = Blend(white, accent, 217);
  const auto fill_rect = [dc](const RECT& rc, COLORREF color) {
    HBRUSH brush = ::CreateSolidBrush(color);
    ::FillRect(dc, &rc, brush);
    ::DeleteObject(brush);
  };
  // The ground an item says it sits on, else the card.
  const auto ground_of = [&card_bg](const Item& item) {
    return item.backdrop != CLR_INVALID ? item.backdrop : card_bg;
  };
  // A glyph from the Space-icon catalogue (artwork, colour emoji, then the
  // GDI font as the last resort) — the list's and the picker's one ladder.
  const auto draw_space_icon = [&](const RECT& rc, const std::wstring& text,
                                   int ink_size, COLORREF color) {
    if (PhiDrawCatalogIcon(dc, rc, text, ink_size, color)) {
      return;
    }
    if (!PhiIsPrivateGlyph(text) &&
        PhiDrawColorIcon(dc, rc, text, ink_size, color)) {
      return;
    }
    HFONT icon_font = PhiIsPrivateGlyph(text) ? mdl2_font_ : emoji_font_;
    if (icon_font) {
      ::SelectObject(dc, icon_font);
    }
    ::SetTextColor(dc, color);
    RECT box = rc;
    PhiDrawTextAA(dc, text.c_str(), -1, &box,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (font_) {
      ::SelectObject(dc, font_);
    }
  };

  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  HBRUSH page = ::CreateSolidBrush(bg);
  ::FillRect(dc, &client, page);
  ::DeleteObject(page);
  // The toolbar band shares the ground; a separator closes it.
  (void)separator;

  // The caption strip: Phi mark leading, the pane title CENTERED, the
  // close button trailing (Win11's metric, red when hot) — the shared
  // frame's band, at the same metrics the strip used to draw by hand.
  PaintBand(dc);

  ::SetBkMode(dc, TRANSPARENT);
  HGDIOBJ previous_font = font_ ? ::SelectObject(dc, font_) : nullptr;
  bool clipped = false;

  const int hot_item = ItemOfHit(hovered());
  for (size_t item_index = 0; item_index < items_.size(); ++item_index) {
    const Item& item = items_[item_index];
    const bool rail_field = item.kind == ItemKind::kSearchField && item.on;
    if (item.kind != ItemKind::kToolbarItem &&
        item.kind != ItemKind::kAccountBlock && !rail_field && !clipped) {
      // Pane content scrolls; nothing of it may paint over the toolbar.
      ::IntersectClipRect(dc, rail_w_, Scaled(kChromeTop), client.right,
                          client.bottom);
      clipped = true;
    }
    RECT bounds = item.bounds;
    const bool hot =
        item.enabled && static_cast<int>(item_index) == hot_item;
    const COLORREF row_ink = item.enabled ? label : Blend(label, bg, 120);
    switch (item.kind) {
      case ItemKind::kToolbarItem: {
        // A rail item (§7): the selected one on ink 6 % with a 3 × 16
        // accent indicator at its leading edge, hover ink 4.5 %, a 16 dip
        // icon and the 14 label (dropped on the narrow rail).
        if (item.selected) {
          FillRounded(dc, bounds, Blend(ink, bg, 15), Scaled(kRadius));
          const int mid = (bounds.top + bounds.bottom) / 2;
          RECT bar = {bounds.left, mid - Scaled(8), bounds.left + Scaled(3),
                      mid + Scaled(8)};
          FillRounded(dc, bar, accent, Scaled(1));
        } else if (hot) {
          FillRounded(dc, bounds, hover_wash, Scaled(kRadius));
        }
        wchar_t mark = 0xE713;
        for (const SectionDef& section : kSections) {
          if (item.payload == section.id) {
            mark = section.glyph;
            break;
          }
        }
        const int icon_px = Scaled(16);
        const int mid_y = (bounds.top + bounds.bottom) / 2;
        const int icon_left = item.text.empty()
                                  ? (bounds.left + bounds.right - icon_px) / 2
                                  : bounds.left + Scaled(12);
        RECT glyph = {icon_left, mid_y - icon_px / 2, icon_left + icon_px,
                      mid_y + icon_px / 2};
        const std::wstring mark_text(1, mark);
        PhiDrawGlyphIcon(dc, glyph, mark_text.c_str(), icon_px, row_ink);
        if (!item.text.empty()) {
          // The label at 42 in the row (design: icon 16 at 12, label 42).
          RECT caption = {bounds.left + Scaled(42), bounds.top,
                          bounds.right - Scaled(8), bounds.bottom};
          ::SetTextColor(dc, row_ink);
          PhiDrawTextAA(dc, item.text.c_str(), -1, &caption,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                            DT_END_ELLIPSIS);
        }
        break;
      }
      case ItemKind::kAccountBlock: {
        // The rail's account block: the account picture (or the Contact
        // glyph on an accent disc) at 44, the name 14/600 over the email
        // 12; the Account page's own rail entry, so it wears the rail
        // item's selection and hover washes.
        if (item.selected) {
          FillRounded(dc, bounds, Blend(ink, bg, 15), Scaled(kRadius));
        } else if (hot) {
          FillRounded(dc, bounds, hover_wash, Scaled(kRadius));
        }
        const int px = Scaled(kRailAvatar);
        RECT disc = {bounds.left + Scaled(8), bounds.top + Scaled(8),
                     bounds.left + Scaled(8) + px, bounds.top + Scaled(8) + px};
        const std::vector<uint8_t>& circle = account_->AccountAvatarCircle(px);
        if (!circle.empty()) {
          PhiBlendBgra(dc, disc, circle.data(), px, px);
        } else {
          PhiFillEllipseAA(dc, disc, accent);
          if (HFONT person = IconFont(20)) {
            ::SelectObject(dc, person);
          }
          ::SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
          PhiDrawTextAA(dc, L"\uE77B", -1, &disc,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        const int text_left = disc.right + Scaled(12);
        const int mid = (disc.top + disc.bottom) / 2;
        if (small_font_) {
          ::SelectObject(dc, small_font_);
        }
        ::SetTextColor(dc, label);
        RECT name = {text_left, mid - Scaled(19), bounds.right - Scaled(8), mid};
        PhiDrawTextAA(dc, item.text.c_str(), -1, &name,
                      DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
        if (hint_font_) {
          ::SelectObject(dc, hint_font_);
        }
        ::SetTextColor(dc, secondary);
        RECT mail = {text_left, mid, bounds.right - Scaled(8), mid + Scaled(17)};
        PhiDrawTextAA(dc, item.detail.c_str(), -1, &mail,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kPageTitle:
        // Segoe UI Variable Bold 28, #1A1A1A (white 90 % in dark).
        if (page_title_font_) {
          ::SelectObject(dc, page_title_font_);
        }
        ::SetTextColor(dc, dark ? Blend(white, bg, 230)
                                : RGB(0x1A, 0x1A, 0x1A));
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                        DT_END_ELLIPSIS);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      case ItemKind::kCard:
        // A settings card (§7): `card` fill, 1 px stroke, radius 4.
        StrokeRounded(dc, bounds, card_bg, card_border, Scaled(kCardRadius),
                      1);
        break;
      case ItemKind::kSectionTitle:
        if (small_font_) {
          ::SelectObject(dc, small_font_);
        }
        ::SetTextColor(dc, label);  // The group header, 14/600.
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      case ItemKind::kSubtitle:
      case ItemKind::kHint:
        if (hint_font_) {
          ::SelectObject(dc, hint_font_);
        }
        // A danger hint is an action's failure line (Settings ▸ Sync).
        ::SetTextColor(dc, item.danger ? prefs_->Palette().destructive
                           : item.enabled ? secondary
                                          : Blend(secondary, bg, 120));
        // Wrapped items were MEASURED at this width, so DT_TOP lands them
        // exactly in the box the layout reserved. DT_WORDBREAK silently
        // disables DT_VCENTER, which is why a one-line item keeps the
        // centred single-line path.
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    item.wrap ? (DT_LEFT | DT_TOP | DT_WORDBREAK |
                                 DT_NOPREFIX)
                              : (DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                                 DT_END_ELLIPSIS | DT_NOPREFIX));
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      case ItemKind::kRowLabel:
        ::SetTextColor(dc, row_ink);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    item.wrap ? (DT_LEFT | DT_TOP | DT_WORDBREAK |
                                 DT_NOPREFIX)
                              : (DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                                 DT_END_ELLIPSIS | DT_NOPREFIX));
        break;
      case ItemKind::kLink:
        if (small_font_) {
          ::SelectObject(dc, small_font_);
        }
        ::SetTextColor(dc, item.enabled ? accent : tertiary);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      case ItemKind::kDivider: {
        HBRUSH line = ::CreateSolidBrush(rule_ink);
        ::FillRect(dc, &bounds, line);
        ::DeleteObject(line);
        break;
      }
      case ItemKind::kSwatch: {
        // A dot (22pt, or `value` when the pane overrides — the Spaces
        // detail's 16) centred in its item width; a 2pt ring in the
        // theme's OWN accent when selected; the caption (11pt) shows only
        // for the selected swatch.
        const int cx = (bounds.left + bounds.right) / 2;
        const int dot = Scaled(item.value > 0 ? item.value : kSwatchDot);
        const int ring = dot + Scaled(4);
        RECT ring_rc = {cx - ring / 2, bounds.top, cx + ring / 2,
                        bounds.top + ring};
        const COLORREF swatch_ground = ground_of(item);
        if (item.selected) {
          // The design's selected swatch: the dot, a 2 gap of the card,
          // then 2 of the theme's own accent (`on` = a colour the Space
          // inherits from the app-wide choice: a hairline in the
          // secondary ink instead).
          const COLORREF ring_color = item.ring ? item.ring : accent;
          Ellipse(dc, ring_rc, swatch_ground,
                  !item.enabled ? tertiary
                                : (item.on ? secondary : ring_color),
                  Scaled(item.on ? 1 : 2));
        } else if (hot) {
          Ellipse(dc, ring_rc, swatch_ground, Blend(ink, bg, 46), 1);
        }
        RECT dot_rc = {cx - dot / 2, bounds.top + (ring - dot) / 2,
                       cx + dot / 2, bounds.top + (ring + dot) / 2};
        COLORREF fill = item.swatch;
        if (!item.enabled) {
          fill = Blend(fill, bg, 100);
        }
        // Pure is white and needs the contrast hairline.
        Ellipse(dc, dot_rc, fill, Blend(ink, bg, 31), 1);
        break;
      }
      case ItemKind::kImageCard: {
        // The Mac's 122x72 tile PDFs, redrawn: a mini window card (r8,
        // ink 2%) carrying per-mode chrome at ink 6%/10%, plus the baked
        // #3AA4D5 agent pill. Selection is a 2pt theme ring at r5;
        // UNSELECTED tiles have no outline at all (the Mac's lineWidth 0).
        RECT image = {bounds.left, bounds.top, bounds.right,
                      bounds.top + Scaled(kImageCardH)};
        const bool tile_dark =
            item.payload == "dark" || (item.payload != "light" &&
                                       item.payload != "system" && dark);
        const COLORREF tile_ground =
            item.payload == "dark"
                ? RGB(0x34, 0x34, 0x34)
                : (tile_dark ? RGB(0x2E, 0x2E, 0x2E) : bg);
        const COLORREF tile_ink =
            tile_dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00);
        const COLORREF kAgentPill = RGB(0x3A, 0xA4, 0xD5);
        // The window card fills the tile; "system" splits it light/dark.
        const auto draw_tile = [&](const RECT& rc, COLORREF ground,
                                   COLORREF ink_color, bool full) {
          const COLORREF card2 = Blend(ink_color, ground, 5);
          const COLORREF chrome6 = Blend(ink_color, ground, 15);
          const COLORREF chrome10 = Blend(ink_color, ground, 26);
          HBRUSH ground_brush = ::CreateSolidBrush(ground);
          ::FillRect(dc, &rc, ground_brush);
          ::DeleteObject(ground_brush);
          RECT card_rc = {image.left + Scaled(1), image.top + Scaled(1),
                          image.right - Scaled(1), image.bottom - Scaled(1)};
          const int save_tile = ::SaveDC(dc);
          ::IntersectClipRect(dc, rc.left, rc.top, rc.right, rc.bottom);
          FillRounded(dc, card_rc, card2, Scaled(8));
          const auto rr = [&](int x, int y, int w, int h, COLORREF c,
                              int r) {
            RECT shape = {image.left + Scaled(x), image.top + Scaled(y),
                          image.left + Scaled(x + w),
                          image.top + Scaled(y + h)};
            FillRounded(dc, shape, c, Scaled(r));
          };
          if (item.payload == "performance") {
            rr(4, 9, 27, 8, chrome6, 4);        // Sidebar address bar.
            rr(32, 1, 1, 70, chrome10, 0);      // Sidebar divider.
            rr(15, 59, 14, 8, kAgentPill, 8);   // Agent pill, bottom.
          } else if (item.payload == "balanced") {
            rr(32, 1, 1, 70, chrome10, 0);
            rr(44, 2, 59, 8, chrome6, 8);       // Top address bar.
            rr(105, 2, 14, 8, kAgentPill, 8);
            rr(33, 10, 88, 1, chrome10, 0);     // Header hairline.
          } else if (item.payload == "comfortable") {
            RECT tab = {image.left + Scaled(8), image.top + Scaled(2),
                        image.left + Scaled(36), image.top + Scaled(11)};
            OutlineRounded(dc, tab, chrome10, Scaled(4), 1);  // Active tab.
            rr(1, 10, 120, 1, chrome10, 0);     // Strip baseline.
            rr(38, 2, 23, 8, chrome6, 5);       // Inactive tab.
            rr(105, 2, 14, 8, kAgentPill, 8);
          } else if (item.payload == "ntp") {
            rr(37, 26, 48, 10, chrome6, 5);     // Centered NTP field.
            rr(105, 2, 14, 8, kAgentPill, 8);
          } else if (item.payload == "omnibox") {
            rr(24, 8, 74, 10, chrome6, 5);      // Dropped omnibox.
            rr(28, 22, 56, 3, chrome6, 2);      // Suggestion lines.
            rr(28, 29, 44, 3, chrome6, 2);
            rr(105, 2, 14, 8, kAgentPill, 8);
          } else {
            // The appearance tiles: two tab chips + the agent pill.
            RECT tab = {image.left + Scaled(8), image.top + Scaled(2),
                        image.left + Scaled(36), image.top + Scaled(11)};
            OutlineRounded(dc, tab, chrome10, Scaled(4), 1);
            rr(38, 2, 23, 8, chrome6, 5);
            rr(62, 2, 23, 8, chrome6, 5);
            rr(105, 2, 14, 8, kAgentPill, 8);
          }
          ::RestoreDC(dc, save_tile);
          (void)full;
        };
        if (item.payload == "system") {
          RECT left_half = {image.left, image.top,
                            (image.left + image.right) / 2, image.bottom};
          RECT right_half = {(image.left + image.right) / 2, image.top,
                             image.right, image.bottom};
          draw_tile(left_half, bg, RGB(0x00, 0x00, 0x00), false);
          draw_tile(right_half, RGB(0x34, 0x34, 0x34),
                    RGB(0xFF, 0xFF, 0xFF), false);
        } else {
          draw_tile(image, tile_ground, tile_ink, true);
        }
        if (!item.enabled) {
          // The Mac dims disabled pickers to 40%.
          HBRUSH wash = ::CreateSolidBrush(bg);
          // Approximate the opacity drop with a light hatch of the ground.
          for (int line = image.top; line < image.bottom; line += 2) {
            RECT stripe = {image.left, line, image.right, line + 1};
            ::FillRect(dc, &stripe, wash);
          }
          ::DeleteObject(wash);
        }
        if (item.selected) {
          OutlineRounded(dc, image, accent, Scaled(5), Scaled(2));
        } else if (hot) {
          OutlineRounded(dc, image, Blend(ink, bg, 60), Scaled(5), 1);
        }
        if (hint_font_) {
          ::SelectObject(dc, hint_font_);
        }
        RECT caption = {bounds.left, image.bottom + Scaled(10) - Scaled(4),
                        bounds.right, bounds.bottom};
        ::SetTextColor(dc, row_ink);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &caption,
                    DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kLayoutCard: {
        // The design's Layout half: a 110 preview on the page ground —
        // radius 6, a 1 px stroke_strong, 2 px accent when chosen — with
        // a proportional sketch of the layout inside (8 padding; the card
        // white, the chrome ink 6-8 %), then a 16 radio and the 13 label.
        RECT image = {bounds.left, bounds.top, bounds.right,
                      bounds.top + Scaled(kLayoutPreviewH)};
        FillRounded(dc, image, bg, Scaled(6));
        {
          const int save_tile = ::SaveDC(dc);
          ::IntersectClipRect(dc, image.left, image.top, image.right,
                              image.bottom);
          const COLORREF page_white = card_bg;
          const COLORREF chrome = Blend(ink, bg, 20);
          const int pad = Scaled(8);
          const int gap = Scaled(3);
          RECT inner = {image.left + pad, image.top + pad, image.right - pad,
                        image.bottom - pad};
          if (item.payload == "comfortable") {
            // A tab strip (active tab white, a second on ink 6 %), the
            // toolbar band, the page.
            RECT active = {inner.left, inner.top, inner.left + Scaled(26),
                           inner.top + Scaled(9)};
            FillRounded(dc, active, page_white, Scaled(2));
            RECT other = {active.right + gap, inner.top,
                          active.right + gap + Scaled(26), inner.top + Scaled(9)};
            FillRounded(dc, other, Blend(ink, bg, 15), Scaled(2));
            RECT bar = {inner.left, active.bottom + gap, inner.right,
                        active.bottom + gap + Scaled(10)};
            FillRounded(dc, bar, page_white, Scaled(2));
            RECT page_rc = {inner.left, bar.bottom + gap, inner.right,
                            inner.bottom};
            FillRounded(dc, page_rc, page_white, Scaled(2));
            RECT seam = {page_rc.left, page_rc.top, page_rc.right,
                         page_rc.top + Scaled(1)};
            HBRUSH seam_brush = ::CreateSolidBrush(chrome);
            ::FillRect(dc, &seam, seam_brush);
            ::DeleteObject(seam_brush);
          } else {
            // The address row across the top, then the sidebar's three
            // rows (the middle one the active tab) beside the page.
            RECT bar = {inner.left, inner.top, inner.right,
                        inner.top + Scaled(10)};
            FillRounded(dc, bar, page_white, Scaled(2));
            const int side_w = Scaled(34);
            int row_top = bar.bottom + gap;
            for (int r = 0; r < 3; ++r) {
              RECT row = {inner.left, row_top, inner.left + side_w,
                          row_top + Scaled(6)};
              FillRounded(dc, row, r == 1 ? page_white : chrome, Scaled(2));
              row_top = row.bottom + gap;
            }
            RECT page_rc = {inner.left + side_w + gap, bar.bottom + gap,
                            inner.right, inner.bottom};
            FillRounded(dc, page_rc, page_white, Scaled(4));
          }
          ::RestoreDC(dc, save_tile);
        }
        if (item.selected) {
          OutlineRounded(dc, image, accent, Scaled(6), Scaled(2));
        } else {
          OutlineRounded(dc, image, Blend(ink, bg, hot ? 60 : 41), Scaled(6),
                         1);
        }
        // The radio: a 16 ring, a 5 accent ring when chosen.
        const int radio = Scaled(16);
        const int row_mid = image.bottom + Scaled(8) + Scaled(10);
        RECT ring = {bounds.left, row_mid - radio / 2, bounds.left + radio,
                     row_mid + radio / 2};
        if (item.selected) {
          Ellipse(dc, ring, RGB(0xFF, 0xFF, 0xFF), accent, Scaled(5));
        } else {
          Ellipse(dc, ring, card_bg, Blend(ink, bg, hot ? 90 : 41), 1);
        }
        RECT caption = {ring.right + Scaled(8), row_mid - Scaled(10),
                        bounds.right, row_mid + Scaled(10)};
        ::SetTextColor(dc, row_ink);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &caption,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                        DT_END_ELLIPSIS);
        break;
      }
      case ItemKind::kToggle: {
        // The Mac's .mini switch: a 26x15 capsule carrying an 11pt knob
        // inset 2 on every side, so the knob travels its own width.
        // The design's ToggleSwitch: a 40 x 20 capsule — on, the accent
        // with a 12 white knob 4 in from the end; off, a 1 px stroke_strong
        // outline on the card with a 12 dim knob 4 in.
        const int height = bounds.bottom - bounds.top;
        const int radius = height / 2;
        COLORREF fill = item.on ? accent : card_bg;
        if (!item.enabled) {
          fill = Blend(fill, bg, 120);
        }
        if (hot) {
          // A switch has no room for a wash; the halo is the hover cue.
          RECT halo = bounds;
          ::InflateRect(&halo, Scaled(3), Scaled(3));
          FillRounded(dc, halo, hover_wash, (halo.bottom - halo.top) / 2);
        }
        FillRounded(dc, bounds, fill, radius);
        if (!item.on) {
          OutlineRounded(dc, bounds, Blend(ink, bg, item.enabled ? 41 : 25),
                         radius, 1);
        }
        const int inset = Scaled(4);
        const int knob = height - 2 * inset;
        const int left = item.on ? bounds.right - knob - inset
                                 : bounds.left + inset;
        RECT dot = {left, bounds.top + inset, left + knob,
                    bounds.top + inset + knob};
        const COLORREF knob_fill =
            item.on ? RGB(0xFF, 0xFF, 0xFF)
                    : (item.enabled ? secondary : tertiary);
        PhiFillEllipseAA(dc, dot, knob_fill);
        break;
      }
      case ItemKind::kSliderTrack: {
        // The design's Slider: a 4 dip track (ink 20 %) filled in the
        // accent up to an 18 white knob carrying an 8 accent dot.
        const int mid = (bounds.top + bounds.bottom) / 2;
        const int knob = Scaled(kSliderKnob);
        RECT track = {bounds.left, mid - Scaled(2), bounds.right,
                      mid + Scaled(2)};
        const int span = track.right - track.left;
        const COLORREF fill = item.enabled ? accent : Blend(accent, bg, 140);
        FillRounded(dc, track, Blend(ink, card_bg, 51), Scaled(2));
        const int at = track.left + (span - knob) * item.value / 100;
        RECT filled = {track.left, track.top, at + knob / 2, track.bottom};
        FillRounded(dc, filled, fill, Scaled(2));
        RECT dot = {at, mid - knob / 2, at + knob, mid + knob / 2};
        Ellipse(dc, dot, RGB(0xFF, 0xFF, 0xFF), Blend(ink, bg, 41), 1);
        RECT pip = dot;
        ::InflateRect(&pip, -Scaled(5), -Scaled(5));
        PhiFillEllipseAA(dc, pip, fill);
        break;
      }
      case ItemKind::kPopup: {
        // A ComboBox (design): the control ground (#FBFBFB light — a
        // shade off the card; a shade on in dark), a 1 px stroke_strong,
        // radius 4, the value 14 with a chevron-down trailing.
        const COLORREF combo_ground =
            dark ? Blend(RGB(0xFF, 0xFF, 0xFF), card_bg, hot ? 20 : 12)
                 : Blend(ink, card_bg, hot ? 10 : 4);
        StrokeRounded(dc, bounds, combo_ground, Blend(ink, bg, 41),
                      Scaled(kRadius), 1);
        ::SetTextColor(dc, row_ink);
        RECT text = bounds;
        text.left += Scaled(10);
        text.right -= Scaled(28);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &text,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                        DT_END_ELLIPSIS);
        RECT arrows = {bounds.right - Scaled(26), bounds.top,
                       bounds.right - Scaled(6), bounds.bottom};
        PhiDrawGlyphIcon(dc, arrows, L"\uE70D", Scaled(12),
                         item.enabled ? secondary : tertiary);
        break;
      }
      case ItemKind::kListRow: {
        // Two Mac selection styles: Spaces (`value` 1) fills the SOLID
        // accent with white content; Profiles (`value` 2) a 15% accent
        // wash with normal ink.
        const bool solid = item.value == 1;
        if (item.selected) {
          FillRounded(dc, bounds,
                      solid ? accent : Blend(accent, card_bg, 38),
                      Scaled(kRadius));
        } else if (hot) {
          FillRounded(dc, bounds, hover_wash, Scaled(kRadius));
        }
        const bool on_accent = solid && item.selected;
        const COLORREF text_ink = on_accent ? RGB(0xFF, 0xFF, 0xFF)
                                            : row_ink;
        int x = bounds.left + Scaled(solid ? 8 : 10);
        if (solid) {
          // The Spaces row's drag handle (line.3.horizontal).
          if (mdl2_font_) {
            ::SelectObject(dc, mdl2_font_);
          }
          ::SetTextColor(dc, on_accent ? RGB(0xFF, 0xFF, 0xFF) : secondary);
          RECT handle = {x, bounds.top, x + Scaled(14), bounds.bottom};
          PhiDrawTextAA(dc, L"\uE700", -1, &handle,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
          if (font_) {
            ::SelectObject(dc, font_);
          }
          x += Scaled(14) + Scaled(8);
        }
        if (!item.icon.empty()) {
          // The Space's icon in an 18pt box: catalog artwork tinted the
          // row ink, colour emoji via DirectWrite, GDI glyph as the last
          // resort — the same ladder as the picker's cells. Both draws take
          // the icon's INK size, so a column of Spaces reads as one size
          // whatever medium each icon comes from.
          const int mid = (bounds.top + bounds.bottom) / 2;
          RECT icon_rc = {x, mid - Scaled(9), x + Scaled(18), mid + Scaled(9)};
          const bool drawn =
              PhiDrawCatalogIcon(dc, icon_rc, item.icon, Scaled(13),
                                 text_ink) ||
              (!PhiIsPrivateGlyph(item.icon) &&
               PhiDrawColorIcon(dc, icon_rc, item.icon, Scaled(13),
                                text_ink));
          if (!drawn) {
            HFONT icon_font =
                PhiIsPrivateGlyph(item.icon) ? mdl2_font_ : emoji_font_;
            if (icon_font) {
              ::SelectObject(dc, icon_font);
            }
            ::SetTextColor(dc, text_ink);
            PhiDrawTextAA(dc, item.icon.c_str(), -1, &icon_rc,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (font_) {
              ::SelectObject(dc, font_);
            }
          }
          x += Scaled(18) + Scaled(8);
        }
        ::SetTextColor(dc, text_ink);
        RECT text = bounds;
        text.left = x;
        text.right -= Scaled(item.on ? 130 : 76);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &text,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
        int badge_left = text.left;
        if (!item.text.empty()) {
          RECT measured = {0, 0, 0, 0};
          PhiDrawTextAA(dc, item.text.c_str(), -1, &measured,
                      DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
          badge_left += std::min<int>(measured.right,
                                      text.right - text.left);
        }
        if (item.on) {
          // The Default badge: a 10pt capsule (secondary 15%, or white 20%
          // on the accent).
          if (hint_font_) {
            ::SelectObject(dc, hint_font_);
          }
          RECT cap = {badge_left + Scaled(8),
                      (bounds.top + bounds.bottom) / 2 - Scaled(9),
                      badge_left + Scaled(8) + Scaled(52),
                      (bounds.top + bounds.bottom) / 2 + Scaled(9)};
          FillRounded(dc, cap,
                      on_accent ? Blend(RGB(0xFF, 0xFF, 0xFF), accent, 51)
                                : Blend(ink, bg, 38),
                      (cap.bottom - cap.top) / 2);
          ::SetTextColor(dc, on_accent ? RGB(0xFF, 0xFF, 0xFF) : secondary);
          PhiDrawTextAA(dc, PhiStr("settings.defaultBadge"), -1, &cap,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
          if (font_) {
            ::SelectObject(dc, font_);
          }
        }
        if (!item.detail.empty()) {
          if (hint_font_) {
            ::SelectObject(dc, hint_font_);
          }
          RECT detail = bounds;
          detail.right -= Scaled(solid ? 20 : 8);
          ::SetTextColor(dc, on_accent ? RGB(0xFF, 0xFF, 0xFF) : secondary);
          PhiDrawTextAA(dc, item.detail.c_str(), -1, &detail,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
          if (solid) {
            // The Mac row hosts a profile picker; its chevron.up.down
            // stands in (bindings are immutable here).
            RECT chev = bounds;
            chev.right -= Scaled(6);
            PhiDrawTextAA(dc, L"↕", -1, &chev,
                        DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
          }
          if (font_) {
            ::SelectObject(dc, font_);
          }
        }
        break;
      }
      case ItemKind::kToolButton: {
        // The +/-/pencil strip glyphs are MDL2; pager arrows and plain
        // captions ride the UI font.
        if (hot && !item.action.empty()) {
          RECT wash = bounds;
          ::InflateRect(&wash, -Scaled(4), -Scaled(4));
          FillRounded(dc, wash, hover_wash, Scaled(kRadius));
        }
        const bool mdl2 = !item.text.empty() && PhiIsPrivateGlyph(item.text);
        if (mdl2 && mdl2_font_) {
          ::SelectObject(dc, mdl2_font_);
        }
        ::SetTextColor(dc, item.enabled ? row_ink : tertiary);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (mdl2 && font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kIcon: {
        // A lone MDL2 glyph: secondary at rest, accent when `selected`
        // (the data links' arrow), tertiary when disabled; `value`
        // overrides the 16pt size (the Account avatar).
        // The sized glyph comes from the cache; it used to be a
        // CreateFontIndirectW / DeleteObject pair PER ITEM PER PAINT
        // (lane 3 ranked finding 12).
        HFONT icon_font =
            item.value > 0 ? IconFont(item.value) : mdl2_font_;
        if (icon_font) {
          ::SelectObject(dc, icon_font);
        }
        ::SetTextColor(dc, !item.enabled ? tertiary
                                         : (item.selected ? accent
                                                          : secondary));
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kKeycap: {
        // The Mac keycap: one pill for the whole chord — r6,
        // controlBackground fill, hairline separator border, 13pt
        // secondary text.
        const COLORREF cap_bg =
            dark ? Blend(RGB(0xFF, 0xFF, 0xFF), bg, 13)
                 : RGB(0xFF, 0xFF, 0xFF);
        // A customised chord (`on`) shows in the accent, the Mac's cue that
        // the row departs from its default.
        StrokeRounded(dc, bounds, cap_bg,
                      item.on ? accent : (hot ? secondary : rule_ink),
                      Scaled(6), 1);
        ::SetTextColor(dc, item.on ? accent : secondary);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        break;
      }
      case ItemKind::kSearchField: {
        if (item.value == 1) {
          // A value field (an expander body's Name): the design's TextBox
          // — the card ground, a 1 px stroke_strong, radius 4, the value
          // 14; clicking it opens the rename prompt.
          StrokeRounded(dc, bounds, hot ? Blend(ink, card_bg, 6) : card_bg,
                        Blend(ink, bg, 41), Scaled(kRadius), 1);
          RECT value = {bounds.left + Scaled(12), bounds.top,
                        bounds.right - Scaled(12), bounds.bottom};
          ::SetTextColor(dc, row_ink);
          PhiDrawTextAA(dc, item.text.c_str(), -1, &value,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                            DT_NOPREFIX);
          break;
        }
        // Magnifier + the live filter text (placeholder when empty). The
        // rail's field (`on`) draws its own control fill; the Shortcuts
        // one rides its card.
        // The rail's field (design): the card ground with a 1 px stroke,
        // radius 8, the magnifier TRAILING; the Shortcuts one keeps its
        // leading glass on the card.
        RECT glass;
        RECT text;
        // A focused text input wears the Windows 11 TextBox's focus cue —
        // the accent underline and a caret — rather than the focus ring,
        // whichever way the focus got there.
        const bool field_focused = static_cast<int>(item_index) == focus_ &&
                                   window_active_ && item.action.empty();
        if (item.on) {
          StrokeRounded(dc, bounds, card_bg, card_border, Scaled(8), 1);
          if (!item.text.empty() || field_focused) {
            RECT edge = {bounds.left + Scaled(8), bounds.bottom - Scaled(2),
                         bounds.right - Scaled(8), bounds.bottom};
            FillRounded(dc, edge, accent, 1);
          }
          glass = {bounds.right - Scaled(34), bounds.top, bounds.right - Scaled(10),
                   bounds.bottom};
          text = {bounds.left + Scaled(12), bounds.top, glass.left - Scaled(4),
                  bounds.bottom};
        } else {
          glass = {bounds.left + Scaled(12), bounds.top, bounds.left + Scaled(28),
                   bounds.bottom};
          text = {glass.right + Scaled(8), bounds.top, bounds.right - Scaled(12),
                  bounds.bottom};
          if (field_focused) {
            RECT edge = {bounds.left + Scaled(8), bounds.bottom - Scaled(2),
                         bounds.right - Scaled(8), bounds.bottom};
            FillRounded(dc, edge, accent, 1);
          }
        }
        if (mdl2_font_) {
          ::SelectObject(dc, mdl2_font_);
        }
        ::SetTextColor(dc, secondary);
        PhiDrawTextAA(dc, L"", -1, &glass,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        if (item.text.empty()) {
          ::SetTextColor(dc, tertiary);
          PhiDrawTextAA(
              dc,
              item.detail.empty()
                  ? PhiStr("settings.shortcuts.searchField.placeholder")
                  : item.detail.c_str(),
              -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else {
          ::SetTextColor(dc, row_ink);
          PhiDrawTextAA(dc, item.text.c_str(), -1, &text,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        if (field_focused) {
          // The caret, after the typed text (typing only ever appends).
          RECT measured = {0, 0, 0, 0};
          if (!item.text.empty()) {
            PhiDrawTextAA(dc, item.text.c_str(), -1, &measured,
                          DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
          }
          const int x = std::min<int>(text.left + measured.right + Scaled(1),
                                      text.right);
          const int mid = (text.top + text.bottom) / 2;
          RECT caret = {x, mid - Scaled(8), x + std::max(1, Scaled(1)),
                        mid + Scaled(8)};
          HBRUSH caret_brush = ::CreateSolidBrush(row_ink);
          ::FillRect(dc, &caret, caret_brush);
          ::DeleteObject(caret_brush);
        }
        break;
      }
      case ItemKind::kAvatar: {
        // The account's picture as a circle (premultiplied BGRA from the
        // profile service, decoded once per size), the Contact glyph
        // until there is one; the Mac's 16pt pencil badge at the bottom
        // right when the picture can be edited.
        const int px = bounds.right - bounds.left;
        const std::vector<uint8_t>& circle = account_->AccountAvatarCircle(px);
        if (!circle.empty()) {
          PhiBlendBgra(dc, bounds, circle.data(), px, px);
        } else {
          HFONT icon_font =
              item.value > 0 ? IconFont(item.value) : mdl2_font_;
          if (icon_font) {
            ::SelectObject(dc, icon_font);
          }
          ::SetTextColor(dc, item.enabled ? secondary : tertiary);
          PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        if (!item.action.empty()) {
          const int badge = Scaled(16);
          RECT badge_rc = {bounds.right - badge, bounds.bottom - badge,
                           bounds.right, bounds.bottom};
          PhiFillEllipseAA(dc, badge_rc, card_bg);
          PhiStrokeEllipseAA(dc, badge_rc, card_border, std::max(1, Scaled(1)));
          if (HFONT pencil = IconFont(9)) {
            ::SelectObject(dc, pencil);
          }
          ::SetTextColor(dc, item.enabled ? secondary : tertiary);
          PhiDrawTextAA(dc, L"\uE70F", -1, &badge_rc,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kEditableLabel: {
        // The Mac's name label: 13pt semibold, a pencil trailing the text
        // (nameEditIconButton, 11pt, 4pt gap); the whole row edits.
        if (title_font_) {
          ::SelectObject(dc, title_font_);
        }
        ::SetTextColor(dc, item.enabled ? row_ink : tertiary);
        RECT measure = bounds;
        PhiDrawTextAA(dc, item.text.c_str(), -1, &measure,
                      DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        const int pencil_w = Scaled(11);
        const int text_w = std::min<int>(measure.right - measure.left,
                                         bounds.right - bounds.left - pencil_w -
                                             Scaled(4));
        RECT text_rc = {bounds.left, bounds.top, bounds.left + text_w,
                        bounds.bottom};
        PhiDrawTextAA(dc, item.text.c_str(), -1, &text_rc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
        if (!item.action.empty()) {
          RECT pencil_rc = {text_rc.right + Scaled(4), bounds.top,
                            text_rc.right + Scaled(4) + pencil_w, bounds.bottom};
          if (HFONT pencil = IconFont(11)) {
            ::SelectObject(dc, pencil);
          }
          ::SetTextColor(dc, hot ? row_ink : secondary);
          PhiDrawTextAA(dc, L"\uE70F", -1, &pencil_rc,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kProfileCard: {
        // The membership card, like the Mac's ProfileCardViewController:
        // the account's saved style under the CURRENT theme's palette
        // (re-applied here on every theme or appearance change), the name,
        // the member id from the Auth0 subject and the join month.
        const std::string sub = account_->auth().user().sub;
        PhiHolographicCardStyle style =
            PhiHolographicCardStyle::FromMap(prefs_->ProfileCardStyle(sub));
        style.palette = PhiHolographicCardStyle::PaletteForTheme(prefs_->ThemeId());
        profile_card_.SetStyle(style);
        std::string name;
        account_->AccountInfo(&name, nullptr);
        const std::optional<PhiAccountProfile>& profile =
            account_->Profile().profile();
        const bool profile_matches = profile && profile->auth0_id == sub;
        profile_card_.SetIdentity(
            name.empty() ? "Phi" : name, PhiHolographicCard::MemberId(sub),
            profile_matches ? PhiHolographicCard::FormatJoinDate(profile->created_at)
                            : std::string());
        profile_card_.Paint(dc, bounds);
        if (profile_card_.animating() && !card_timer_.running()) {
          card_timer_.Start(16, /*coalescable=*/true);
        }
        break;
      }
      case ItemKind::kIconChip: {
        // The row's leading glyph: the design's bare 20 dip icon in the
        // label ink (dim when the row is), no tile behind it.
        const std::wstring glyph(item.text);
        PhiDrawGlyphIcon(dc, bounds, glyph.c_str(), Scaled(kChipSize),
                         row_ink);
        break;
      }
      case ItemKind::kBadge: {
        FillRounded(dc, bounds, Blend(ink, bg, 25),
                    (bounds.bottom - bounds.top) / 2);
        if (hint_font_) {
          ::SelectObject(dc, hint_font_);
        }
        ::SetTextColor(dc, secondary);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kButton: {
        if (item.selected) {
          FillRounded(dc, bounds, accent, Scaled(kRadius));
        } else {
          // The design's Button: the card ground with a 1 px stroke,
          // radius 4; the pointer's wash over it.
          StrokeRounded(dc, bounds, hot ? Blend(ink, card_bg, 11) : card_bg,
                        Blend(ink, bg, 41), Scaled(kRadius), 1);
        }
        // A destructive button reads in the danger red (§2.3), dimmed
        // like any other when disabled.
        const COLORREF danger = prefs_->Palette().destructive;
        ::SetTextColor(dc, item.selected ? RGB(0xFF, 0xFF, 0xFF)
                           : item.danger   ? (item.enabled
                                                  ? danger
                                                  : Blend(danger, bg, 120))
                                           : row_ink);
        // A leading PUA glyph ("+ New Space") comes from the icon face.
        if (!item.text.empty() && PhiIsPrivateGlyph(item.text.substr(0, 1))) {
          RECT glyph = {bounds.left + Scaled(12), bounds.top,
                        bounds.left + Scaled(28), bounds.bottom};
          if (HFONT icon_font = IconFont(12)) {
            ::SelectObject(dc, icon_font);
          }
          PhiDrawTextAA(dc, item.text.substr(0, 1).c_str(), -1, &glyph,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
          if (font_) {
            ::SelectObject(dc, font_);
          }
          RECT rest = {glyph.right + Scaled(4), bounds.top,
                       bounds.right - Scaled(8), bounds.bottom};
          std::wstring tail = item.text.substr(1);
          while (!tail.empty() && tail.front() == L' ') {
            tail.erase(tail.begin());
          }
          PhiDrawTextAA(dc, tail.c_str(), -1, &rest,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
          break;
        }
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        break;
      }
      case ItemKind::kAppMark: {
        // The brand mark at the item's size (About).
        const int px = bounds.right - bounds.left;
        if (HICON mark = PhiAppIcon(px)) {
          ::DrawIconEx(dc, bounds.left, bounds.top, mark, px, px, 0, nullptr,
                       DI_NORMAL);
        }
        break;
      }
      case ItemKind::kSpaceCard: {
        // The two-pane card: white, a 10 % border at radius 4, the 1 px
        // divider between the panes and the rule over the list's bar.
        StrokeRounded(dc, bounds, card_bg, Blend(ink, bg, 26),
                      Scaled(kCardRadius), 1);
        fill_rect({item.value, bounds.top + 1, item.value + 1,
                   bounds.bottom - 1},
                  Blend(ink, card_bg, 26));
        fill_rect({bounds.left + 1, item.total, item.value, item.total + 1},
                  Blend(ink, card_bg, 26));
        break;
      }
      case ItemKind::kSpaceRow: {
        // A Space: the solid accent when selected (content white 85 %),
        // the pointer's wash otherwise; the 20 icon, 6, the 13 name.
        if (item.selected) {
          FillRounded(dc, bounds, accent, Scaled(kSpaceRowRadius));
        } else if (hot) {
          FillRounded(dc, bounds, Blend(ink, card_bg, dark ? 15 : 11),
                      Scaled(kSpaceRowRadius));
        }
        const COLORREF content = item.selected ? sp_on_accent : sp_text;
        const int mid = (bounds.top + bounds.bottom) / 2;
        const int icon_left = bounds.left + Scaled(kSpaceRowInset);
        const RECT icon_rc = {icon_left, mid - Scaled(kSpaceIcon) / 2,
                              icon_left + Scaled(kSpaceIcon),
                              mid - Scaled(kSpaceIcon) / 2 + Scaled(kSpaceIcon)};
        if (!item.icon.empty()) {
          // The Mac draws the icon at 0.8 of its 20 frame.
          draw_space_icon(icon_rc, item.icon, Scaled(16), content);
        }
        if (body_font_) {
          ::SelectObject(dc, body_font_);
        }
        ::SetTextColor(dc, content);
        RECT name = {icon_rc.right + Scaled(kSpaceIconGap), bounds.top,
                     item.value - Scaled(8), bounds.bottom};
        PhiDrawTextAA(dc, item.text.c_str(), -1, &name,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kDragHandle: {
        // Three 9 x 1 lines, 3 apart, centred in the 24 grip.
        if (hot || (reorder_drag_ && item.payload == reorder_id_)) {
          FillRounded(dc, bounds, Blend(ink, card_bg, dark ? 15 : 11),
                      Scaled(kRadius));
        }
        const int cx = (bounds.left + bounds.right) / 2;
        const int cy = (bounds.top + bounds.bottom) / 2;
        const int line_w = Scaled(9);
        const int line_h = std::max(1, Scaled(1));
        const int gap = Scaled(3) + line_h;
        for (int k = -1; k <= 1; ++k) {
          const int top = cy - line_h / 2 + k * gap;
          fill_rect({cx - line_w / 2, top, cx - line_w / 2 + line_w,
                     top + line_h},
                    sp_text);
        }
        break;
      }
      case ItemKind::kRowPopup: {
        // The row's profile pill: white 15 % on a selected row, ink 5 %
        // elsewhere; the 13 label 12 in, chevron.up.chevron.down trailing
        // in a 24 cell. Inert (dimmed) on the default Space.
        const COLORREF fill =
            item.on ? Blend(white, accent, hot && item.enabled ? 64 : 38)
                    : Blend(ink, card_bg, hot && item.enabled ? 23 : 13);
        FillRounded(dc, bounds, fill, Scaled(6));
        COLORREF content = item.on ? sp_on_accent : sp_text;
        if (!item.enabled) {
          content = Blend(content, fill, 128);
        }
        if (body_font_) {
          ::SelectObject(dc, body_font_);
        }
        ::SetTextColor(dc, content);
        RECT text = {bounds.left + Scaled(12), bounds.top,
                     bounds.right - Scaled(24), bounds.bottom};
        PhiDrawTextAA(dc, item.text.c_str(), -1, &text,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
        const int cy = (bounds.top + bounds.bottom) / 2;
        const RECT up = {bounds.right - Scaled(24), cy - Scaled(7),
                         bounds.right, cy + Scaled(1)};
        const RECT down = {bounds.right - Scaled(24), cy - Scaled(1),
                           bounds.right, cy + Scaled(7)};
        PhiDrawGlyphIcon(dc, up, L"", Scaled(8), content);
        PhiDrawGlyphIcon(dc, down, L"", Scaled(8), content);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kBarButton: {
        // + / − / edit: a 24 square, radius 4 wash under the pointer.
        if (hot) {
          FillRounded(dc, bounds, Blend(ink, card_bg, dark ? 20 : 13),
                      Scaled(kRadius));
        }
        PhiDrawGlyphIcon(dc, bounds, item.text.c_str(), Scaled(12),
                         item.enabled ? Blend(ink, card_bg, 178)
                                      : Blend(ink, card_bg, 64));
        break;
      }
      case ItemKind::kHairline:
        fill_rect(bounds, Blend(ink, ground_of(item), 26));
        break;
      case ItemKind::kGroupBox: {
        // A well: ink 3 % at radius 4. The picker's well carries its
        // grid's scroll position as a 2 px thumb in its right padding.
        FillRounded(dc, bounds, ground_of(item), Scaled(kWellRadius));
        if (item.total > item.visible && item.visible > 0 &&
            grid_rect_.bottom > grid_rect_.top) {
          const int track = grid_rect_.bottom - grid_rect_.top;
          const int thumb =
              std::max(Scaled(16), track * item.visible / item.total);
          const int at = grid_rect_.top + (track - thumb) * item.value /
                                              std::max(1, item.total -
                                                              item.visible);
          const RECT bar = {bounds.right - Scaled(8), at,
                            bounds.right - Scaled(6), at + thumb};
          FillRounded(dc, bar, Blend(ink, ground_of(item), 64), Scaled(1));
        }
        break;
      }
      case ItemKind::kPaneLabel: {
        if (body_font_) {
          ::SelectObject(dc, body_font_);
        }
        const COLORREF ground = ground_of(item);
        ::SetTextColor(dc, item.value == 1 ? Blend(ink, ground, 107)
                                           : Blend(ink, ground, 217));
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                      (item.value == 1 ? DT_CENTER : DT_LEFT) | DT_VCENTER |
                          DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kSegment: {
        const COLORREF ground = ground_of(item);
        // The pill under the segments: white 80 % (dark: white 8 %), a
        // 10 % hairline. The design's 0 20 50 6 % shadow is too faint to
        // read over the 3 % well and is not drawn.
        const COLORREF pill = dark ? Blend(white, ground, 20)
                                   : Blend(white, ground, 204);
        const int radius = (bounds.bottom - bounds.top) / 2;
        if (item.action.empty()) {
          FillRounded(dc, bounds, pill, radius);
          PhiStrokeCapsuleAA(dc, bounds, Blend(ink, ground, 26), 1);
          break;
        }
        if (item.selected) {
          FillRounded(dc, bounds, accent, radius);
        } else if (hot) {
          FillRounded(dc, bounds, Blend(ink, pill, 13), radius);
        }
        if (hint_font_) {
          ::SelectObject(dc, hint_font_);
        }
        ::SetTextColor(dc, item.selected ? white : ink);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kIconSearch: {
        // The picker's search pill: ink 5 %, a 20 magnifier and the 12
        // placeholder at 30 %; focused, an accent hairline and the caret.
        const COLORREF ground = ground_of(item);
        const COLORREF fill = Blend(ink, ground, hot ? 20 : 13);
        const int radius = (bounds.bottom - bounds.top) / 2;
        FillRounded(dc, bounds, fill, radius);
        const bool field_focused =
            static_cast<int>(item_index) == focus_ && window_active_;
        if (field_focused) {
          PhiStrokeCapsuleAA(dc, bounds, accent, 1);
        }
        const COLORREF dim = Blend(ink, fill, 77);
        const RECT glass = {bounds.left + Scaled(6), bounds.top,
                            bounds.left + Scaled(26), bounds.bottom};
        PhiDrawGlyphIcon(dc, glass, L"", Scaled(12), dim);
        if (hint_font_) {
          ::SelectObject(dc, hint_font_);
        }
        RECT text = {glass.right + Scaled(4), bounds.top,
                     bounds.right - Scaled(12), bounds.bottom};
        ::SetTextColor(dc, item.text.empty() ? dim : sp_text);
        PhiDrawTextAA(dc,
                      item.text.empty() ? item.detail.c_str()
                                        : item.text.c_str(),
                      -1, &text,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
        if (field_focused) {
          RECT measured = {0, 0, 0, 0};
          if (!item.text.empty()) {
            PhiDrawTextAA(dc, item.text.c_str(), -1, &measured,
                          DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
          }
          const int x = std::min<int>(text.left + measured.right + Scaled(1),
                                      text.right);
          const int mid = (text.top + text.bottom) / 2;
          fill_rect({x, mid - Scaled(7), x + std::max(1, Scaled(1)),
                     mid + Scaled(7)},
                    sp_text);
        }
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kPickerCell: {
        // A 24 glyph in the ink; the Space's own on a white tile (radius
        // 5) over a 1 px shadow, the pointer's on a 5 % wash.
        const COLORREF ground = ground_of(item);
        const int radius = Scaled(5);
        if (item.selected) {
          RECT shadow = bounds;
          ::OffsetRect(&shadow, 0, std::max(1, Scaled(1)));
          FillRounded(dc, shadow, Blend(ink, ground, 38), radius);
          FillRounded(dc, bounds, dark ? Blend(white, ground, 38) : white,
                      radius);
        } else if (hot) {
          FillRounded(dc, bounds, Blend(ink, ground, 13), radius);
        }
        // Inset: the colour pass repaints its whole rect, and the tile's
        // rounded corners must survive.
        RECT glyph_rc = bounds;
        ::InflateRect(&glyph_rc, -Scaled(3), -Scaled(3));
        draw_space_icon(PhiIsPrivateGlyph(item.text) ? bounds : glyph_rc,
                        item.text, Scaled(17), ink);
        break;
      }
      case ItemKind::kUrlRow: {
        // The design's row: white, a 9 % border at radius 4; the 20 link
        // glyph, 16, the 13 title over its 12 description, the chevron.
        StrokeRounded(dc, bounds, hot ? Blend(ink, card_bg, 8) : card_bg,
                      Blend(ink, bg, 23), Scaled(kCardRadius), 1);
        const int pad = Scaled(kUrlRowPad);
        const int mid = (bounds.top + bounds.bottom) / 2;
        const RECT icon = {bounds.left + pad, mid - Scaled(kUrlRowIcon) / 2,
                           bounds.left + pad + Scaled(kUrlRowIcon),
                           mid - Scaled(kUrlRowIcon) / 2 + Scaled(kUrlRowIcon)};
        PhiDrawGlyphIcon(dc, icon, L"", Scaled(kUrlRowIcon), sp_text);
        const int text_left = icon.right + pad;
        const int text_right = bounds.right - pad - Scaled(16) - pad;
        const int block = Scaled(18) + Scaled(2) + item.value;
        const int top = mid - block / 2;
        if (body_font_) {
          ::SelectObject(dc, body_font_);
        }
        ::SetTextColor(dc, sp_text);
        RECT title = {text_left, top, text_right, top + Scaled(18)};
        PhiDrawTextAA(dc, item.text.c_str(), -1, &title,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
        if (hint_font_) {
          ::SelectObject(dc, hint_font_);
        }
        ::SetTextColor(dc, sp_secondary);
        RECT subtitle = {text_left, title.bottom + Scaled(2), text_right,
                         title.bottom + Scaled(2) + item.value};
        PhiDrawTextAA(dc, item.detail.c_str(), -1, &subtitle,
                      DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        const RECT chevron = {bounds.right - pad - Scaled(16), bounds.top,
                              bounds.right - pad, bounds.bottom};
        PhiDrawGlyphIcon(dc, chevron, L"", Scaled(12), sp_secondary);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kThemeColor: {
        // ThemeColorSettingsRow: the whole row is the button (its label is
        // a kPaneLabel over it); trailing, the theme's colours as a 20 dot
        // in a 10 % hairline, then the 20 chevron cell.
        const COLORREF ground = ground_of(item);
        if (hot) {
          FillRounded(dc, bounds, Blend(ink, ground, dark ? 15 : 11),
                      Scaled(kWellRadius));
        }
        const int dot = Scaled(kThemeColorDot);
        const int mid = (bounds.top + bounds.bottom) / 2;
        const int right = bounds.right - Scaled(kWellPadX);
        const RECT chevron = {right - dot, mid - dot / 2, right,
                              mid - dot / 2 + dot};
        const RECT dot_rc = {chevron.left - Scaled(kThemeColorGap) - dot,
                             chevron.top,
                             chevron.left - Scaled(kThemeColorGap),
                             chevron.bottom};
        FillGradientDot(dc, dot_rc, item.colors);
        PhiStrokeEllipseAA(dc, dot_rc, Blend(ink, ground, 26), 1);
        PhiDrawGlyphIcon(dc, chevron, L"\uE76C", Scaled(11),
                         Blend(ink, ground, 217));
        break;
      }
      case ItemKind::kExpander: {
        // An expander header (design "Settings · Spaces / Profiles"):
        // hover wash, a bare 20 icon (the Space's, in its own colour) or
        // the profile's 20 disc with a person glyph, name 14 over hint 12,
        // an optional Default badge, and the chevron (right closed, down
        // open).
        if (hot) {
          FillRounded(dc, bounds, hover_wash, Scaled(kRadius));
        }
        const int mid = (bounds.top + bounds.bottom) / 2;
        int x = bounds.left + Scaled(kCardPadding - 4);
        RECT tile = {x, mid - Scaled(10), x + Scaled(20), mid + Scaled(10)};
        if (item.value == 1) {
          PhiFillEllipseAA(dc, tile, item.swatch);
          if (HFONT person = IconFont(11)) {
            ::SelectObject(dc, person);
          }
          ::SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
          PhiDrawTextAA(dc, L"\uE77B", -1, &tile,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else {
          const COLORREF icon_ink = item.enabled ? item.swatch : tertiary;
          bool drawn = PhiDrawCatalogIcon(dc, tile, item.icon, Scaled(20),
                                          icon_ink);
          if (!drawn && !PhiIsPrivateGlyph(item.icon)) {
            drawn = PhiDrawColorIcon(dc, tile, item.icon, Scaled(20),
                                     icon_ink);
          }
          if (!drawn) {
            HFONT cell_font =
                PhiIsPrivateGlyph(item.icon) ? IconFont(20) : emoji_font_;
            if (cell_font) {
              ::SelectObject(dc, cell_font);
            }
            ::SetTextColor(dc, icon_ink);
            PhiDrawTextAA(dc, item.icon.c_str(), -1, &tile,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
          }
        }
        x = tile.right + Scaled(kChipGap);
        int text_right = bounds.right - Scaled(40);
        if (item.selected) {
          // The "Default" badge, trailing the text block.
          RECT badge = {text_right - Scaled(60), mid - Scaled(10), text_right,
                        mid + Scaled(10)};
          FillRounded(dc, badge, Blend(ink, bg, 25), Scaled(10));
          if (hint_font_) {
            ::SelectObject(dc, hint_font_);
          }
          ::SetTextColor(dc, secondary);
          PhiDrawTextAA(dc, PhiStr("settings.defaultBadge"), -1, &badge,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
          text_right = badge.left - Scaled(12);
        }
        if (font_) {
          ::SelectObject(dc, font_);
        }
        ::SetTextColor(dc, row_ink);
        RECT name = {x, mid - Scaled(18), text_right, mid};
        PhiDrawTextAA(dc, item.text.c_str(), -1, &name,
                      DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
        if (hint_font_) {
          ::SelectObject(dc, hint_font_);
        }
        ::SetTextColor(dc, secondary);
        RECT hint = {x, mid + Scaled(2), text_right, mid + Scaled(18)};
        PhiDrawTextAA(dc, item.detail.c_str(), -1, &hint,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
        if (mdl2_font_) {
          ::SelectObject(dc, mdl2_font_);
        }
        ::SetTextColor(dc, secondary);
        RECT chevron = {bounds.right - Scaled(36), bounds.top,
                        bounds.right - Scaled(12), bounds.bottom};
        PhiDrawGlyphIcon(dc, chevron, item.on ? L"\uE70D" : L"\uE76C",
                         Scaled(12), secondary);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
    }
  }

  // The keyboard focus ring — the Windows 11 focus visual: a 2 px outer
  // stroke in the ink over a 1 px inner one in the ground, 3 px outside the
  // control and following its corners. Shown only after keyboard use
  // (focus_visible_) and never on an inactive window; a text input wears
  // its own TextBox cue instead (kSearchField above).
  if (focus_visible_ && window_active_ && focus_ >= 0 &&
      focus_ < static_cast<int>(items_.size()) &&
      !IsTextInput(items_[static_cast<size_t>(focus_)])) {
    const Item& item = items_[static_cast<size_t>(focus_)];
    const int save = ::SaveDC(dc);
    ::SelectClipRgn(dc, nullptr);
    if (InRail(item)) {
      ::IntersectClipRect(dc, 0, Scaled(kChromeTop), rail_w_, client.bottom);
    } else {
      ::IntersectClipRect(dc, rail_w_, Scaled(kChromeTop), client.right,
                          client.bottom);
    }
    RECT ring = item.bounds;
    int outset = Scaled(3);
    int radius = Scaled(kRadius);
    bool circle = false;
    switch (item.kind) {
      case ItemKind::kToolbarItem:
      case ItemKind::kAccountBlock:
      case ItemKind::kListRow:
      case ItemKind::kExpander:
        outset = 0;  // A row's ring sits on its own wash, inside the card.
        break;
      case ItemKind::kSpaceRow:
        outset = 0;
        radius = Scaled(kSpaceRowRadius);
        break;
      case ItemKind::kUrlRow:
        outset = 0;
        radius = Scaled(kCardRadius);
        break;
      case ItemKind::kRowPopup:
        radius = Scaled(6);
        break;
      case ItemKind::kSegment:
      case ItemKind::kIconSearch:
        radius = (ring.bottom - ring.top) / 2;
        break;
      case ItemKind::kPickerCell:
        radius = Scaled(5);
        break;
      case ItemKind::kToggle:
        radius = (ring.bottom - ring.top) / 2;
        break;
      case ItemKind::kSliderTrack:
        if (item.on) {
          radius = (ring.bottom - ring.top) / 2;  // The knob-high capsule.
        }
        break;
      case ItemKind::kSwatch: {
        // Round the dot's selection-ring frame, as the paint above draws it.
        const int cx = (ring.left + ring.right) / 2;
        const int frame =
            Scaled(item.value > 0 ? item.value : kSwatchDot) + Scaled(4);
        ring = {cx - frame / 2, item.bounds.top, cx + frame / 2,
                item.bounds.top + frame};
        circle = true;
        break;
      }
      case ItemKind::kAvatar:
        circle = true;
        break;
      case ItemKind::kImageCard:
        ring.bottom = ring.top + Scaled(kImageCardH);
        radius = Scaled(5);
        break;
      case ItemKind::kLayoutCard:
        ring.bottom = ring.top + Scaled(kLayoutPreviewH);
        radius = Scaled(6);
        break;
      case ItemKind::kKeycap:
        radius = Scaled(6);
        break;
      case ItemKind::kLink: {
        // The link's text, not the whole trailing slot it right-aligns in.
        HGDIOBJ held = small_font_ ? ::SelectObject(dc, small_font_) : nullptr;
        RECT measured = {0, 0, 0, 0};
        PhiDrawTextAA(dc, item.text.c_str(), -1, &measured,
                      DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        if (held) {
          ::SelectObject(dc, held);
        }
        ring.left = std::max<LONG>(ring.left, ring.right - measured.right);
        break;
      }
      default:
        break;
    }
    ::InflateRect(&ring, outset, outset);
    const COLORREF outer = dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00);
    const COLORREF inner = dark ? RGB(0x00, 0x00, 0x00) : RGB(0xFF, 0xFF, 0xFF);
    const int width = std::max(1, Scaled(2));
    const int hairline = std::max(1, Scaled(1));
    RECT inner_ring = ring;
    ::InflateRect(&inner_ring, -width, -width);
    if (circle) {
      PhiStrokeEllipseAA(dc, ring, outer, width);
      PhiStrokeEllipseAA(dc, inner_ring, inner, hairline);
    } else {
      PhiStrokeRoundRectAA(dc, ring, radius + outset, outer, width);
      PhiStrokeRoundRectAA(dc, inner_ring,
                           std::max(0, radius + outset - width), inner,
                           hairline);
    }
    ::RestoreDC(dc, save);
  }

  // Panes taller than the window (General, Developer, Shortcuts) scrolled
  // under the wheel with nothing on screen saying so, and no way to tell
  // how far down the content already was.
  const int visible = client.bottom - Scaled(kChromeTop);
  if (content_height_ > visible && visible > 0) {
    const int track_top = Scaled(kChromeTop) + Scaled(6);
    const int track_height = visible - Scaled(12);
    const int thumb_height =
        std::max(Scaled(28),
                 static_cast<int>(static_cast<int64_t>(track_height) *
                                  visible / content_height_));
    const int span = std::max(1, content_height_ - visible);
    const int at = track_top + (track_height - thumb_height) *
                                   std::min(scroll_, span) / span;
    // A Fluent scrollbar: 2 dip at rest, 6 while dragged.
    const int thick = scroll_drag_ ? Scaled(6) : Scaled(2);
    RECT thumb = {client.right - Scaled(4) - thick, at, client.right - Scaled(4),
                  at + thumb_height};
    FillRounded(dc, thumb, Blend(ink, bg, scroll_drag_ ? 90 : 46),
                thick / 2);
    // The drag target is generous: the 14 dip lane the thumb rides in.
    thumb_rect_ = {client.right - Scaled(14), at, client.right, at + thumb_height};
  } else {
    thumb_rect_ = {};
  }

  ::SelectClipRgn(dc, nullptr);
  if (previous_font) {
    ::SelectObject(dc, previous_font);
  }
}

namespace {

// Settings ▸ Developer ▸ "Add skill to…": the Mac's installSkill(for:) and
// installAll(). Every real folder in the way is asked about first; each
// outcome is presented once — one alert per agent from the menu, ONE
// aggregate alert for "All agents".
bool ConfirmSkillOverwrite(HWND owner, const std::wstring& path) {
  return PhiConfirm(
      owner,
      PhiStr("settings.developer.skillInstall.overwriteConfirmation.title"),
      PhiStrF("settings.developer.skillInstall.overwriteConfirmation.message",
              {path})
          .c_str(),
      PhiStr("settings.developer.skillInstall.overwriteConfirmation."
             "replaceButton"),
      /*destructive=*/false);
}

void InstallSkillFor(HWND owner, const PhiSkillTarget& target) {
  const PhiSkillInstallResult result = PhiInstallSkill(
      target, [owner](const std::wstring& path) {
        return ConfirmSkillOverwrite(owner, path);
      });
  switch (result.outcome) {
    case PhiSkillInstallOutcome::kSuccess:
      PhiNotice(
          owner,
          PhiStr("settings.developer.skillInstall.single.successTitle"),
          PhiStrF("settings.developer.skillInstall.single.successMessage",
                  {target.name})
              .c_str());
      break;
    case PhiSkillInstallOutcome::kCancelled:
      break;
    case PhiSkillInstallOutcome::kFailure:
      PhiNotice(owner,
                PhiStr("settings.developer.skillInstall.single.failureTitle"),
                result.error.c_str());
      break;
  }
}

void InstallSkillForAllAgents(HWND owner) {
  std::vector<std::wstring> succeeded;
  std::vector<std::wstring> failed;
  for (const PhiSkillTarget& target : PhiSkillTargets()) {
    const PhiSkillInstallResult result = PhiInstallSkill(
        target, [owner](const std::wstring& path) {
          return ConfirmSkillOverwrite(owner, path);
        });
    switch (result.outcome) {
      case PhiSkillInstallOutcome::kSuccess:
        succeeded.push_back(target.name);
        break;
      case PhiSkillInstallOutcome::kCancelled:
        break;
      case PhiSkillInstallOutcome::kFailure:
        failed.push_back(std::wstring(target.name) + L": " + result.error);
        break;
    }
  }
  if (succeeded.empty() && failed.empty()) {
    return;
  }
  if (failed.empty()) {
    std::wstring names;
    for (const std::wstring& name : succeeded) {
      names += (names.empty() ? L"" : L", ") + name;
    }
    PhiNotice(owner,
              PhiStr("settings.developer.skillInstall.all.successTitle"),
              PhiStrF("settings.developer.skillInstall.all.successMessage",
                      {names})
                  .c_str());
    return;
  }
  std::wstring body;
  for (const std::wstring& line : failed) {
    body += (body.empty() ? L"" : L"\n") + line;
  }
  PhiNotice(owner, PhiStr("settings.developer.skillInstall.all.failureTitle"),
            body.c_str());
}

}  // namespace

void PhiSettingsWindow::Apply(const Item& item, int x) {
  if (item.action == "clearsearch") {
    rail_search_.clear();
    scroll_ = 0;
  } else if (item.action == "section") {
    section_ = item.payload;
    scroll_ = 0;
    shortcut_search_.clear();
    rail_search_.clear();
    SyncTitle();
  } else if (item.action == "checkupdate") {
    if (!PhiUpdater::CheckForUpdateWithUi()) {
      PhiNotice(hwnd(), PhiStr("menuBar.checkForUpdate"),
                PhiStr("menuBar.updaterUnavailableBody"));
    }
  } else if (item.action == "theme") {
    host_->SetThemeId(item.payload);
  } else if (item.action == "appearance") {
    host_->SetAppearance(static_cast<PhiAppearance>(item.value));
  } else if (item.action == "layout") {
    host_->RequestLayoutMode(static_cast<PhiLayoutMode>(item.value));
  } else if (item.action == "newtab") {
    host_->SetNewTabBehavior(static_cast<PhiNewTabBehavior>(item.value));
  } else if (item.action == "tabpreviews") {
    host_->SetShowTabPreviews(!item.on);
  } else if (item.action == "openindicators") {
    host_->SetShowOpenTabIndicators(!item.on);
  } else if (item.action == "dimunloaded") {
    host_->SetDimUnloadedTabIcons(!item.on);
  } else if (item.action == "fullurl") {
    host_->SetAlwaysShowFullUrl(!item.on);
  } else if (item.action == "restoresession") {
    host_->SetRestoreLastSession(!item.on);
  } else if (item.action == "foliofolder") {
    const std::wstring folder =
        PickFolder(hwnd(), PhiStr("settings.general.folio.pickerMessage"));
    if (!folder.empty()) {
      host_->SetFolioGlobalFolder(base::WideToUTF8(folder));
    }
  } else if (item.action == "folioautosave") {
    host_->SetFolioAutoSaveOnSiteActions(!item.on);
  } else if (item.action == "foliosites") {
    host_->OpenFolioAutoSaveSites();
  } else if (item.action == "foliooverride") {
    // The Mac's override control: choose a folder, or follow General's.
    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING, 1,
                  PhiStr("settings.profiles.folio.chooseFolder"));
    const bool overridden =
        !prefs_->FolioFolderOverride(item.payload).empty();
    ::AppendMenuW(menu, MF_STRING | (overridden ? 0 : MF_CHECKED), 2,
                  PhiStr("settings.profiles.folio.sameAsGeneral"));
    POINT anchor = {item.bounds.left, item.bounds.bottom};
    ::ClientToScreen(hwnd(), &anchor);
    const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x,
        anchor.y, 0, hwnd(), nullptr));
    ::DestroyMenu(menu);
    if (chosen == 1) {
      const std::wstring folder = PickFolder(
          hwnd(), PhiStr("settings.profiles.folio.pickerMessage"));
      if (!folder.empty()) {
        host_->SetFolioFolderOverride(item.payload, base::WideToUTF8(folder));
      }
    } else if (chosen == 2) {
      host_->SetFolioFolderOverride(item.payload, std::string());
    }
  } else if (item.action == "analytics") {
    // The Mac's metrics_reporting_changed: the consent transition itself,
    // captured BEFORE the switch moves so an opt-out is the last event the
    // collector sees from this install (an opt-in follows the same order
    // and is simply dropped at the gate, as on Mac before identify).
    host_->CaptureAnalytics(kAnalyticsModuleSettings,
                            kEventMetricsReportingChanged,
                            PhiFlagProperties("enabled", !item.on));
    host_->SetAnalyticsEnabled(!item.on);
  } else if (item.action == "peekview") {
    const bool on = !item.on;
    host_->CaptureAnalytics(kAnalyticsModulePeek, kEventPeekSettingChanged,
                            PhiFlagProperties("enabled", on));
    host_->SetPeekViewEnabled(on);
    // The automatic sub-switch follows the master's transitions: switching
    // Peek on re-arms it, switching Peek off stands it down (the Mac's
    // NavigationsSettingsView does exactly this — and its onChange reports
    // the follow-on flip too, when it is one).
    if (prefs_->AutoPeekViewEnabled() != on) {
      host_->CaptureAnalytics(kAnalyticsModulePeek,
                              kEventPeekAutoSettingChanged,
                              PhiFlagProperties("enabled", on));
    }
    host_->SetAutoPeekViewEnabled(on);
  } else if (item.action == "autopeek") {
    host_->CaptureAnalytics(kAnalyticsModulePeek, kEventPeekAutoSettingChanged,
                            PhiFlagProperties("enabled", !item.on));
    host_->SetAutoPeekViewEnabled(!item.on);
  } else if (item.action == "kioskctrlalt") {
    host_->SetOpenKioskOnCtrlAltClick(!item.on);  // The Mac reports nothing.
  } else if (item.action == "devmode") {
    host_->SetDeveloperMode(!item.on);
  } else if (item.action == "agentprofile") {
    host_->SetAgentProfileAllowed(item.payload, !item.on);
  } else if (item.action == "agentcontrol") {
    host_->SetAllowAgentControl(!item.on);
  } else if (item.action == "agentuserspaces") {
    host_->SetAllowAgentUserSpaces(!item.on);
  } else if (item.action == "gospaces") {
    section_ = "spaces";
    scroll_ = 0;
    SyncTitle();
  } else if (item.action == "gonavigation") {
    section_ = "navigation";
    scroll_ = 0;
    SyncTitle();
  } else if (item.action == "library") {
    // Spaces ▸ "Manage Spaces in Library": the Library opens over the
    // browser window and this window steps aside (WM_CLOSE hides it).
    if (host_->OpenLibrarySpaces()) {
      ::PostMessageW(hwnd(), WM_CLOSE, 0, 0);
    }
  } else if (item.action == "urlrules") {
    // Navigation ▸ "Manage URL Rules…": the universal editor window (the
    // Mac's NavigationsSettingsView row over openURLRulesEditor).
    host_->RequestUrlRulesEditor(hwnd());
  } else if (item.action == "shortcut") {
    PhiShortcutChord chord;
    // The prompt, then the Mac recorder's help line: Escape cancels,
    // Delete removes the shortcut.
    const std::wstring message =
        PhiStrF("settings.shortcuts.recordPrompt.message", {item.detail}) +
        L"\n" + PhiStr("settings.shortcuts.detail.recordingHelp");
    if (!PhiPromptShortcut(hwnd(), PhiStr("settings.changeShortcut"),
                           message.c_str(), &chord)) {
      return;
    }
    if (!chord.vk) {
      // Delete while recording: the shortcut goes UNBOUND (a stored
      // override, so it stays off across restarts). Nothing to check it
      // against — no chord is taken from anyone.
      host_->RebindShortcut(item.payload, 0, false, false, false);
      return;
    }
    // A chord another shortcut answers to is never taken silently. A fixed
    // one (and Ctrl+1…9, the Go to Space keys) cannot be reassigned at all;
    // a rebindable one goes unbound if the user says so (the Mac pane's
    // steal, now asked about and naming the shortcut that loses it).
    const std::wstring keys = PhiFormatChord(chord);
    std::wstring owner;
    bool fixed = false;
    if (chord.ctrl && !chord.alt && !chord.shift && chord.vk >= '1' &&
        chord.vk <= '9') {
      const std::vector<PhiSpaceSummary> spaces = spaces_->Summaries();
      const size_t ordinal = chord.vk - '1';
      owner = PhiStrF(
          "settings.shortcuts.activateSpaceCommandTitle",
          {ordinal < spaces.size()
               ? base::UTF8ToWide(spaces[ordinal].name)
               : std::wstring(1, static_cast<wchar_t>(chord.vk))});
      fixed = true;
    }
    for (const PhiShortcutRow& row : prefs_->ShortcutRows()) {
      if (!owner.empty()) {
        break;
      }
      if (row.name != item.payload && base::UTF8ToWide(row.keys) == keys) {
        owner = base::UTF8ToWide(row.label);
        fixed = !row.rebindable();
      }
    }
    if (fixed) {
      PhiNotice(hwnd(), PhiStr("settings.shortcuts.inUse.title"),
                PhiStrF("settings.shortcuts.inUse.fixedMessage", {keys, owner})
                    .c_str());
      return;
    }
    if (!owner.empty() &&
        !PhiConfirm(
            hwnd(), PhiStrF("settings.shortcuts.reassign.title", {keys}).c_str(),
            PhiStrF("settings.shortcuts.reassign.message", {keys, owner})
                .c_str(),
            PhiStr("settings.shortcuts.reassign.confirmButton"),
            /*destructive=*/false)) {
      return;
    }
    host_->RebindShortcut(item.payload, chord.vk, chord.ctrl, chord.shift,
                          chord.alt);
  } else if (item.action == "restoreshortcuts") {
    // Asked first (Mac 60dfd31a): the reset is irreversible, so the
    // destructive button is never the default — Return is inert, Escape
    // cancels.
    if (PhiAlert(
            hwnd(), PhiStr("settings.shortcuts.restoreConfirmation.title"),
            PhiStr("settings.shortcuts.restoreConfirmation.message"),
            {{PhiStr("settings.shortcuts.restoreConfirmation.confirmButton"),
              PhiAlertRole::kDestructive},
             {PhiStr("settings.shortcuts.restoreConfirmation.cancelButton"),
              PhiAlertRole::kSecondary}},
            /*default_index=*/-1, /*cancel_index=*/1) == 0) {
      host_->RestoreAllShortcuts();
    }
  } else if (item.action == "restoreshortcut") {
    host_->RestoreShortcut(item.payload);
  } else if (item.action == "goprofiles") {
    section_ = "profiles";
    scroll_ = 0;
    SyncTitle();
  } else if (item.action == "gospaces") {
    section_ = "spaces";
    scroll_ = 0;
    SyncTitle();
  } else if (item.action == "browsersettings") {
    host_->OpenBrowserSettings();
  } else if (item.action == "openpage") {
    if (!item.profile.empty()) {
      host_->OpenBrowserPageInProfile(item.profile, item.payload);
    } else {
      host_->OpenBrowserPage(item.payload);
    }
  } else if (item.action == "searchengine") {
    // The Mac's inline picker as a popup: every engine, the default checked.
    const std::vector<PhiSearchEngineRow> engines =
        profiles_->SearchEngines(item.payload);
    HMENU menu = ::CreatePopupMenu();
    if (!menu || engines.empty()) {
      if (menu) {
        ::DestroyMenu(menu);
      }
      return;
    }
    std::vector<std::string> keywords;
    for (size_t i = 0; i < engines.size(); ++i) {
      keywords.push_back(engines[i].keyword);
      ::AppendMenuW(menu, MF_STRING | (engines[i].is_default ? MF_CHECKED : 0),
                    static_cast<UINT>(i) + 1,
                    PhiEscapeMenuText(base::UTF8ToWide(engines[i].name))
                        .c_str());
    }
    POINT anchor = {item.bounds.left, item.bounds.bottom};
    ::ClientToScreen(hwnd(), &anchor);
    const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x,
        anchor.y, 0, hwnd(), nullptr));
    ::DestroyMenu(menu);
    if (chosen >= 1 && chosen <= keywords.size()) {
      host_->SetSearchEngine(item.payload, keywords[chosen - 1]);
    }
  } else if (item.action == "downloadfolder") {
    const std::wstring folder = PickFolder(
        hwnd(), PhiStr("settings.profiles.downloadLocation.pickerMessage"));
    if (!folder.empty()) {
      host_->SetDownloadDirectory(item.payload, base::WideToUTF8(folder));
    }
  } else if (item.action == "defaultbrowser") {
    // Windows hands the choice itself to its own Settings page — but only
    // offers a browser that has registered, so the framework registers Phi
    // first and then deep-links to its entry there. The answer changes while
    // that page is up: WM_ACTIVATE re-asks when this window comes back.
    if (host_) {
      host_->RequestDefaultBrowser();
    } else {
      ::ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr,
                      nullptr, SW_SHOWNORMAL);
    }
  } else if (item.action == "signin") {
    // No Rebuild() here (or in any other action): OnClick calls
    // ScheduleRebuild() once, after Apply, and that is the only path.
    if (host_) {
      host_->RequestSignIn();
    }
  } else if (item.action == "syncsetup") {
    if (host_) {
      host_->RequestSyncSetup(hwnd());
    }
  } else if (item.action == "syncreconfigure") {
    PhiSyncKeyService* keys = host_ ? host_->SyncKeys() : nullptr;
    // When a journaled account reset is what Set up sync again finishes, the
    // confirmation must not promise that the synced data is kept.
    const wchar_t* confirmation =
        keys && keys->snapshot().reset_resume_pending
            ? PhiStr("sync.reconfigure.resumeResetBody.windows")
            : PhiStr("sync.reconfigure.confirmation.windows");
    if (keys && PhiConfirm(hwnd(), PhiStr("sync.reconfigure.title"),
                           confirmation, PhiStr("sync.reconfigure.title"))) {
      sync_action_error_.clear();
      std::weak_ptr<int> alive = sync_alive_;
      keys->ReconfigureSync([this, alive](keys::KeyStatus status) {
        if (alive.expired()) {
          return;
        }
        if (status.ok()) {
          host_->RequestSyncSetup(hwnd());
        } else {
          sync_action_error_ = PhiStr("sync.reconfigure.failed.windows");
        }
        ScheduleRebuild();
      });
    }
  } else if (item.action == "syncretry") {
    if (PhiSyncKeyService* keys = host_ ? host_->SyncKeys() : nullptr) {
      keys->SilentUnlockAndResolve();
    }
  } else if (item.action == "syncapprove" || item.action == "syncdeny") {
    PhiSyncKeyService* keys = host_ ? host_->SyncKeys() : nullptr;
    const auto it = std::find_if(
        sync_pending_.begin(), sync_pending_.end(),
        [&](const keys::PendingApproval& a) { return a.id == item.payload; });
    if (keys && it != sync_pending_.end()) {
      sync_action_error_.clear();
      std::weak_ptr<int> alive = sync_alive_;
      auto done = [this, alive](keys::KeyStatus status) {
        if (alive.expired()) {
          return;
        }
        if (status.kind == keys::KeyErrorKind::kNotUnlocked) {
          sync_action_error_ = PhiStr("settings.sync.error.notUnlocked");
        } else if (status.kind == keys::KeyErrorKind::kNotPending ||
                   status.kind == keys::KeyErrorKind::kNotFound) {
          sync_action_error_ = PhiStr("settings.sync.error.expired");
        } else if (!status.ok()) {
          sync_action_error_ = PhiStr("settings.sync.error.requestFailed");
        }
        RefreshSyncDevices(/*force=*/true);
      };
      if (item.action == "syncapprove") {
        keys->Approve(*it, done);
      } else {
        keys->Deny(it->id, done);
      }
      // The row leaves at once; the refresh confirms it.
      sync_pending_.erase(it);
    }
  } else if (item.action == "syncremove") {
    PhiSyncKeyService* keys = host_ ? host_->SyncKeys() : nullptr;
    if (keys && !sync_removing_ &&
        PhiConfirm(hwnd(), PhiStr("settings.sync.remove.title"),
                   PhiStr("settings.sync.remove.body"),
                   PhiStr("settings.sync.remove.confirm"))) {
      sync_remove_note_.clear();
      sync_removing_ = true;
      ScheduleRebuild();
      std::weak_ptr<int> alive = sync_alive_;
      keys->RemoveThisDevice([this, alive](keys::KeyStatus status) {
        if (alive.expired()) {
          return;
        }
        sync_removing_ = false;
        if (status.kind == keys::KeyErrorKind::kLastActiveDevice) {
          // The device list was stale: the row flips to the account reset,
          // whose description says why; nothing else to note.
          sync_last_device_ = true;
        } else if (!status.ok()) {
          sync_remove_note_ = PhiStr("settings.sync.remove.unavailable");
        }
        ScheduleRebuild();
      });
    }
  } else if (item.action == "syncreset") {
    // The account reset's own confirmation (the Mac's confirmReset), after
    // the row already said what it does; destructive by default.
    PhiSyncKeyService* keys = host_ ? host_->SyncKeys() : nullptr;
    if (keys && !sync_removing_ &&
        PhiConfirm(hwnd(), PhiStr("sync.removal.resetConfirmTitle"),
                   PhiStr("sync.removal.resetConfirmBody.windows"),
                   PhiStr("sync.removal.resetConfirmAction"))) {
      sync_remove_note_.clear();
      sync_removing_ = true;
      ScheduleRebuild();
      std::weak_ptr<int> alive = sync_alive_;
      keys->ResetSyncAndRemoveThisDevice([this, alive](keys::KeyStatus status) {
        if (alive.expired()) {
          return;
        }
        sync_removing_ = false;
        if (status.kind == keys::KeyErrorKind::kNotLastDevice ||
            status.kind == keys::KeyErrorKind::kDeviceNotFound) {
          // The server refused and deleted nothing: another device joined
          // since the list was loaded, or this PC is no longer enrolled.
          // The reloaded list flips the row back to the removal.
          sync_last_device_ = false;
          sync_remove_note_ =
              status.kind == keys::KeyErrorKind::kNotLastDevice
                  ? PhiStr("sync.removal.resetRefusedNotLastDevice.windows")
                  : PhiStr("settings.sync.remove.unavailable");
          RefreshSyncDevices(/*force=*/true);
        } else if (!status.ok()) {
          sync_remove_note_ = PhiStr("settings.sync.remove.unavailable");
        }
        ScheduleRebuild();
      });
    }
  } else if (item.action == "signout") {
    // The Mac asks first (settings.account.logoutConfirmation); its message
    // promises the sign-in screen, which Windows has none of.
    if (host_ &&
        PhiConfirm(hwnd(), PhiStr("settings.account.logoutConfirmation.title"),
                   PhiStr("settings.account.logoutConfirmation.body"),
                   PhiStr("settings.account.logoutConfirmation.logoutButton"),
                   /*destructive=*/false)) {
      host_->RequestSignOut();
    }
  } else if (item.action == "cancelsignin") {
    account_->RequestCancelSignIn();
  } else if (item.action == "rename") {
    // The Mac's rename dialog: one field, Save, and its two validations.
    std::string current;
    account_->AccountInfo(&current, nullptr);
    std::wstring value = base::UTF8ToWide(current);
    if (!PhiPromptText(hwnd(), PhiStr("settings.account.renameDialog.title"),
                       nullptr, &value,
                       PhiStr("settings.account.renameDialog.saveButton"))) {
      return;
    }
    // Trim; PhiPromptText already refuses an empty entry, a blank one
    // gets the Mac's empty-name alert.
    value = PhiTrimWhitespace(value);
    if (value.empty()) {
      PhiNotice(hwnd(),
                PhiStr("settings.account.renameDialog.emptyNameError.title"),
                PhiStr("settings.account.renameDialog.emptyNameError.message"));
      return;
    }
    // In CODE POINTS, as the onboarding name step and the Mac count: an
    // emoji is one character, not two UTF-16 units.
    if (PhiCodePointCount(value) >
        static_cast<size_t>(PhiAccountProfileService::kMaxNameLength)) {
      PhiNotice(
          hwnd(),
          PhiStr("settings.account.renameDialog.nameTooLongError.title"),
          PhiStrF("settings.account.renameDialog.nameTooLongError.message",
                  {PhiFormatNumber(static_cast<long long>(
                      PhiAccountProfileService::kMaxNameLength))})
              .c_str());
      return;
    }
    account_->RequestRenameAccount(base::WideToUTF8(value));
  } else if (item.action == "avatar") {
    account_->RequestEditAvatar();
  } else if (item.action == "downloadcard") {
    // The Mac's exportImage canvas: the theme's hue at saturation .65,
    // brightness .28; black for Pure.
    const PhiTheme& theme = PhiThemeById(prefs_->ThemeId());
    const bool dark = prefs_->Palette().dark;
    const COLORREF overlay = dark ? theme.dark.overlay : theme.light.overlay;
    const COLORREF canvas = prefs_->ThemeId() == "pure"
                                ? RGB(0, 0, 0)
                                : PhiHsbColor(PhiHueOf(overlay), 0.65, 0.28);
    profile_card_.ExportPng(hwnd(), canvas);
  } else if (item.action == "card") {
    // The card only follows the pointer (WM_MOUSEMOVE); nothing on click.
  } else if (item.action == "autopip") {
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
      return;
    }
    constexpr std::array<const char*, 3> modes = {"off", "normal", "parked"};
    constexpr std::array<const char*, 3> labels = {
        "settings.advanced.pictureInPicture.offOption",
        "settings.advanced.pictureInPicture.normalOption",
        "settings.advanced.pictureInPicture.parkAtEdgeOption"};
    const std::string current = prefs_->AutoPictureInPictureMode();
    for (UINT i = 0; i < 3; ++i) {
      ::AppendMenuW(menu, MF_STRING | (current == modes[i] ? MF_CHECKED : 0),
                    i + 1, PhiStr(labels[i]));
    }
    POINT anchor = {item.bounds.left, item.bounds.bottom};
    ::ClientToScreen(hwnd(), &anchor);
    const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x,
        anchor.y, 0, hwnd(), nullptr));
    ::DestroyMenu(menu);
    if (chosen >= 1 && chosen <= 3) {
      host_->SetAutoPictureInPictureMode(modes[chosen - 1]);
      ScheduleRebuild();
    }
  } else if (item.action == "language") {
    // System default, then every locale the catalog carries, each named in
    // its own language.
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
      return;
    }
    const std::string current = prefs_->LanguagePreference();
    ::AppendMenuW(menu, MF_STRING | (current.empty() ? MF_CHECKED : 0), 1,
                  PhiStr("settings.languageSystemDefault"));
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const std::vector<std::string>& locales = PhiAvailableLocales();
    for (size_t i = 0; i < locales.size(); ++i) {
      ::AppendMenuW(menu,
                    MF_STRING | (current == locales[i] ? MF_CHECKED : 0),
                    static_cast<UINT>(i) + 2,
                    PhiLocaleDisplayName(locales[i]));
    }
    POINT anchor = {item.bounds.left, item.bounds.bottom};
    ::ClientToScreen(hwnd(), &anchor);
    const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x,
        anchor.y, 0, hwnd(), nullptr));
    ::DestroyMenu(menu);
    const bool picked =
        chosen == 1 || (chosen >= 2 && chosen < 2 + locales.size());
    if (picked) {
      const std::string chosen_tag =
          chosen == 1 ? std::string() : locales[chosen - 2];
      if (chosen_tag != current) {
        // The Mac's language_changed: this picker only, the two stored
        // preferences ("system" for follow-the-OS).
        host_->CaptureAnalytics(
            kAnalyticsModuleSettings, kEventLanguageChanged,
            PhiLanguageChangedProperties(current, chosen_tag));
      }
      host_->SetLanguagePreference(chosen_tag);
    }
  } else if (item.action == "installskill") {
    // The Mac's Menu: "All agents", then every agent with its brand mark
    // and skills folder, ticked when that folder already links to THIS
    // build's skill (picking it again refreshes the link). The mark is the
    // row's bitmap, which hides a menu's own check glyph, so the tick is
    // the Mac's trailing ✓ in the label.
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
      return;
    }
    const std::vector<PhiSkillTarget>& targets = PhiSkillTargets();
    const std::set<std::string> installed = PhiInstalledSkillTargetIds();
    ::AppendMenuW(menu, MF_STRING, 1,
                  PhiStr("settings.developer.skillInstall.allAgentsAction"));
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    std::vector<HBITMAP> bitmaps;
    // 14 dip: the Mac's 15 pt frame around a 14/18 ink fill, so the mark
    // sits a little under the menu text's height rather than over it.
    const int icon_px = Scaled(14);
    for (size_t i = 0; i < targets.size(); ++i) {
      const bool ticked = installed.count(targets[i].id) > 0;
      const std::wstring label =
          PhiEscapeMenuText(std::wstring(targets[i].name) + L"    " +
                            PhiSkillDisplayPath(targets[i])) +
          (ticked ? L"    \u2713" : L"");
      const UINT id = static_cast<UINT>(i) + 2;
      ::AppendMenuW(menu, MF_STRING | (ticked ? MF_CHECKED : 0), id,
                    label.c_str());
      if (HBITMAP bitmap = PhiSpaceGlyphBitmap(
              PhiAgentIconGlyph(targets[i].icon), icon_px,
              ::GetSysColor(COLOR_MENUTEXT))) {
        bitmaps.push_back(bitmap);
        PhiSetMenuItemBitmap(menu, id, bitmap);
      }
    }
    POINT anchor = {item.bounds.left, item.bounds.bottom};
    ::ClientToScreen(hwnd(), &anchor);
    const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x,
        anchor.y, 0, hwnd(), nullptr));
    ::DestroyMenu(menu);
    for (HBITMAP bitmap : bitmaps) {
      ::DeleteObject(bitmap);
    }
    if (chosen == 1) {
      InstallSkillForAllAgents(hwnd());
    } else if (chosen >= 2 && chosen < 2 + targets.size()) {
      InstallSkillFor(hwnd(), targets[chosen - 2]);
    }
  } else if (item.action == "appearancemenu") {
    // The Color appearance ComboBox: Use system setting / Light / Dark.
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
      return;
    }
    const PhiAppearance current = prefs_->Appearance();
    ::AppendMenuW(menu,
                  MF_STRING | (current == PhiAppearance::kSystem ? MF_CHECKED
                                                                  : 0),
                  1, PhiStr("settings.general.appearance.useSystemOption"));
    ::AppendMenuW(menu,
                  MF_STRING | (current == PhiAppearance::kLight ? MF_CHECKED
                                                                 : 0),
                  2, PhiStr("themes.appearance.lightOption"));
    ::AppendMenuW(menu,
                  MF_STRING | (current == PhiAppearance::kDark ? MF_CHECKED
                                                                : 0),
                  3, PhiStr("themes.appearance.darkOption"));
    POINT anchor = {item.bounds.left, item.bounds.bottom};
    ::ClientToScreen(hwnd(), &anchor);
    const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x,
        anchor.y, 0, hwnd(), nullptr));
    ::DestroyMenu(menu);
    if (chosen >= 1 && chosen <= 3) {
      host_->SetAppearance(chosen == 1   ? PhiAppearance::kSystem
                           : chosen == 2 ? PhiAppearance::kLight
                                         : PhiAppearance::kDark);
    }
  } else if (item.action == "pinscope") {
    // The Mac's popup: Space / Profile / App.
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
      return;
    }
    const PhiPinScope current = host_->PinScope();
    ::AppendMenuW(menu,
                  MF_STRING | (current == PhiPinScope::kSpace ? MF_CHECKED
                                                              : 0),
                  1, PhiStr("settings.spaces.pinnedTabScope.spaceMenuOption"));
    ::AppendMenuW(menu,
                  MF_STRING | (current == PhiPinScope::kProfile ? MF_CHECKED
                                                                : 0),
                  2, PhiStr("settings.spaces.pinnedTabScope.profileOption"));
    ::AppendMenuW(menu,
                  MF_STRING | (current == PhiPinScope::kApp ? MF_CHECKED : 0),
                  3, PhiStr("settings.spaces.pinnedTabScope.appOption"));
    POINT anchor = {item.bounds.left, item.bounds.bottom};
    ::ClientToScreen(hwnd(), &anchor);
    const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x,
        anchor.y, 0, hwnd(), nullptr));
    ::DestroyMenu(menu);
    if (chosen >= 1 && chosen <= 3) {
      host_->SetPinScope(chosen == 1   ? PhiPinScope::kSpace
                         : chosen == 2 ? PhiPinScope::kProfile
                                       : PhiPinScope::kApp);
    }
  } else if (item.action == "sidebartransparency") {
    const int span = item.bounds.right - item.bounds.left;
    if (span > 0) {
      const int value = (x - item.bounds.left) * 100 / span;
      host_->SetSidebarTransparency(std::clamp(value, 0, 100));
    }
  } else if (item.action == "spacecolor") {
    // Copied out first: the editor's writes rebuild items_ under `item`.
    const std::string space_id = item.payload;
    RECT anchor = item.bounds;
    ::MapWindowPoints(hwnd(), nullptr, reinterpret_cast<POINT*>(&anchor), 2);
    host_->OpenSpaceThemeEditor(space_id, hwnd(), anchor);
  } else if (item.action == "spaceicon") {
    host_->SetSpaceIcon(item.space, item.payload);
  } else if (item.action == "pickspace") {
    SelectSpace(item.payload, /*reveal=*/false);
  } else if (item.action == "spacedrag") {
    // The grip's press is its drag (OnMouseDown); the keyboard moves it
    // with Up / Down (ReorderByKey). Nothing to do on activation.
  } else if (item.action == "icontab") {
    if (icon_tab_ != item.value) {
      icon_tab_ = item.value;
      icon_search_.clear();
      grid_first_row_ = -1;
    }
  } else if (item.action == "selectprofile") {
    selected_profile_ =
        selected_profile_ == item.payload ? "" : item.payload;
  } else if (item.action == "spaceprofile") {
    // The Profile ComboBox: a menu of the profiles, the bound one checked.
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
      return;
    }
    std::string bound;
    for (const PhiSpaceSummary& space : spaces_->Summaries()) {
      if (space.id == item.payload) {
        bound = space.profile_id;
      }
    }
    const std::vector<PhiProfileSummary> profiles = profiles_->Profiles();
    UINT id = 1;
    for (const PhiProfileSummary& profile : profiles) {
      ::AppendMenuW(
          menu, MF_STRING | (profile.id == bound ? MF_CHECKED : 0), id++,
          PhiEscapeMenuText(base::UTF8ToWide(profile.name)).c_str());
    }
    POINT anchor = {item.bounds.left, item.bounds.bottom};
    ::ClientToScreen(hwnd(), &anchor);
    const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x,
        anchor.y, 0, hwnd(), nullptr));
    ::DestroyMenu(menu);
    if (chosen >= 1 && chosen <= profiles.size()) {
      // Confirmed like the menus' (phi_app_menu.cc, the pip's menu): the
      // Space's window is replaced and its tabs reload in the new profile,
      // where the old profile's logins do not exist. The message follows
      // the pinned-tab scope, as the Mac's does.
      const PhiProfileSummary& target = profiles[chosen - 1];
      const char* message =
          host_->PinScope() == PhiPinScope::kSpace
              ? "app.changeSpaceProfileConfirmation.spaceScopedMessage"
          : host_->PinScope() == PhiPinScope::kProfile
              ? "app.changeSpaceProfileConfirmation.profileScopedMessage"
              : "app.changeSpaceProfileConfirmation.appScopedMessage";
      if (target.id != bound &&
          PhiConfirm(hwnd(),
                     PhiStrF("app.changeSpaceProfileConfirmation.title",
                             {base::UTF8ToWide(target.name)})
                         .c_str(),
                     PhiStr(message),
                     PhiStr("app.changeSpaceProfileConfirmation.confirmButton"),
                     /*destructive=*/false)) {
        host_->SetSpaceProfile(item.payload, target.id);
      }
    }
  } else if (item.action == "newspace") {
    // The create form in its own window, opened on the selected Space's
    // profile (else the first's) — the Mac's newSpace. The bare name
    // prompt stays only for a Settings window with no browser window
    // behind it to present the new Space in.
    std::string profile_id;
    for (const PhiSpaceSummary& space : spaces_->Summaries()) {
      if (profile_id.empty() || space.id == selected_space_) {
        profile_id = space.profile_id;
      }
    }
    if (!host_->OpenCreateSpaceWindow(profile_id, hwnd())) {
      std::wstring name;
      if (PhiPromptText(hwnd(), PhiStr("settings.spaces.newButtonTooltip"),
                        PhiStr("settings.enterANameForThe"), &name, nullptr,
                        kPhiNameRules)) {
        host_->CreateSpaceNamed(base::WideToUTF8(name));
      }
    }
  } else if (item.action == "renspace") {
    std::wstring name;
    for (const PhiSpaceSummary& space : spaces_->Summaries()) {
      if (space.id == selected_space_) {
        name = base::UTF8ToWide(space.name);
        break;
      }
    }
    if (PhiPromptText(hwnd(), PhiStr("app.renameSpaceDialog.title"),
                      PhiStr("app.renameSpaceDialog.message"), &name, nullptr,
                      kPhiNameRules)) {
      host_->RenameSpace(selected_space_, base::WideToUTF8(name));
    }
  } else if (item.action == "delspace") {
    if (PhiConfirm(hwnd(), PhiStr("dialog.deleteSpace.title"),
                   PhiStr("menuBar.deleteSpaceConfirmBody"),
                   PhiStr("settings.spaces.deleteConfirmation.deleteButton")) &&
        host_->DeleteSpace(selected_space_)) {
      selected_space_.clear();
    }
  } else if (item.action == "renprofile") {
    std::wstring name;
    for (const PhiProfileSummary& profile : profiles_->Profiles()) {
      if (profile.id == selected_profile_) {
        name = base::UTF8ToWide(profile.name);
        break;
      }
    }
    if (PhiPromptText(hwnd(), PhiStr("settings.profiles.renameDialog.title"),
                      PhiStr("settings.profiles.renameDialog.message"), &name,
                      nullptr, kPhiNameRules)) {
      host_->RenameProfileById(selected_profile_, base::WideToUTF8(name));
    }
  } else if (item.action == "delprofile") {
    if (PhiConfirm(
            hwnd(), PhiStr("dialog.deleteProfile.title"),
            PhiStr("menuBar.deleteProfileConfirmBody"),
            PhiStr("settings.profiles.deleteConfirmation.deleteButton")) &&
        host_->DeleteProfileById(selected_profile_)) {
      selected_profile_.clear();
    }
  } else if (item.action == "addprofile") {
    std::wstring name;
    if (PhiPromptText(hwnd(), PhiStr("settings.profiles.createDialog.title"),
                      PhiStr("settings.profiles.createDialog.message"), &name,
                      nullptr, kPhiNameRules)) {
      host_->CreateProfileNamed(base::WideToUTF8(name));
    }
  }
}

void PhiSettingsWindow::OnMouseDown(int hit, POINT client_pt) {
  pressed_ = -1;
  dragging_ = false;
  const int index = ItemOfHit(hit);
  if (index < 0 || !items_[static_cast<size_t>(index)].enabled) {
    return;
  }
  // A press focuses what it lands on, as a Windows control does (so Tab
  // carries on from there), without showing the ring — WM_LBUTTONDOWN has
  // already hidden it.
  if (Focusable(items_[static_cast<size_t>(index)]) && index != focus_) {
    focus_ = index;
    focus_key_ = FocusKeyOf(index);
    PhiUiaSetFocusedHit(this, hit, /*announce=*/true);
    Invalidate();  // A text field's caret moves with it.
  }
  pressed_ = index;
  const Item item = items_[static_cast<size_t>(index)];
  // A slider is the one control that acts on the PRESS, and it keeps
  // following the pointer — outside the window too — until the button
  // comes back up. Everything else waits for the release.
  if (item.kind == ItemKind::kSliderTrack) {
    dragging_ = true;
    drag_item_ = item;
    drag_ = Capture(this);
    Apply(item, client_pt.x);
    ScheduleRebuild();
  } else if (item.kind == ItemKind::kDragHandle) {
    // The grip: the row follows the pointer through the list (the order
    // rearranged live, as the Mac's orderedIds), written once on release.
    reorder_order_.clear();
    reorder_from_ = -1;
    for (const PhiSpaceSummary& space : spaces_->Summaries()) {
      if (space.id == item.payload) {
        reorder_from_ = static_cast<int>(reorder_order_.size());
      }
      reorder_order_.push_back(space.id);
    }
    if (reorder_from_ < 0) {
      reorder_order_.clear();
      return;
    }
    reorder_drag_ = true;
    reorder_id_ = item.payload;
    SelectSpace(item.payload, /*reveal=*/false);
    drag_ = Capture(this);
    ::SetCursor(::LoadCursorW(nullptr, IDC_SIZENS));
    ScheduleRebuild();
  }
}

void PhiSettingsWindow::OnClick(int hit, POINT client_pt) {
  const int pressed = pressed_;
  pressed_ = -1;
  if (reorder_drag_) {
    EndReorder(/*commit=*/true);
    return;
  }
  if (dragging_) {
    dragging_ = false;
    drag_.Release();
    return;
  }
  // Press and release must land on the SAME control: the old handler fired
  // from WM_LBUTTONDOWN and then again from every WM_MOUSEMOVE that still
  // had the button down, so dragging across the pane flipped every switch
  // the pointer crossed.
  const int index = ItemOfHit(hit);
  if (index < 0 || index != pressed) {
    return;
  }
  // By value: Apply() may open a modal menu or prompt whose nested loop
  // rebuilds items_ underneath us.
  const Item item = items_[static_cast<size_t>(index)];
  if (item.action.empty() || !item.enabled) {
    return;
  }
  Apply(item, client_pt.x);
  ScheduleRebuild();
}

void PhiSettingsWindow::OnWheel(int delta, POINT client_pt) {
  // Pane content can outgrow the fixed window; the Mac scrolls, so do we.
  // The offset feeds Rebuild(), so the scroll is a rebuild — not something
  // WM_PAINT discovers.
  // Over Settings › Spaces' list or icon grid, the wheel scrolls THAT by
  // whole rows while it has rows to scroll, as the Mac's inner scrollers do.
  const bool over_grid = ::PtInRect(&grid_rect_, client_pt) &&
                         grid_rows_ > grid_visible_;
  const bool over_list =
      ::PtInRect(&list_rect_, client_pt) &&
      static_cast<int>(spaces_->Summaries().size()) > list_visible_;
  if (over_grid || over_list) {
    wheel_accum_ += delta;
    const int rows = wheel_accum_ / kWheelPerRow;
    wheel_accum_ -= rows * kWheelPerRow;
    if (rows != 0) {
      if (over_grid) {
        grid_first_row_ = std::max(0, grid_first_row_ - rows);
      } else {
        list_first_ = std::max(0, list_first_ - rows);
      }
      ScheduleRebuild();  // The build clamps the far end.
    }
    return;
  }
  wheel_accum_ = 0;
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  const int visible = client.bottom - Scaled(kChromeTop);
  const int max_scroll = std::max(0, content_height_ - visible);
  scroll_ = std::max(0, std::min(max_scroll, scroll_ - delta / 2));
  ScheduleRebuild();
}

void PhiSettingsWindow::OnEscape() {
  // Esc walks back out, one step at a time — an open expander (the focus
  // lands on its header), the filter, then the window (which only hides,
  // as WM_CLOSE does). A popup menu is modal and takes its own Esc.
  if (reorder_drag_) {
    // The grip's drag is dropped where it started.
    EndReorder(/*commit=*/false);
    return;
  }
  if (!icon_search_.empty() && focus_ >= 0 &&
      focus_ < static_cast<int>(items_.size()) &&
      items_[static_cast<size_t>(focus_)].kind == ItemKind::kIconSearch) {
    icon_search_.clear();
    grid_first_row_ = -1;
    ScheduleRebuild();
    return;
  }
  for (size_t i = 0; i < items_.size(); ++i) {
    const Item& header = items_[i];
    if (header.kind != ItemKind::kExpander || !header.on) {
      continue;
    }
    if (header.action == "selectprofile") {
      selected_profile_.clear();
    } else {
      break;
    }
    // The header keeps its identity across the fold, so the focus key
    // taken now finds it again in the rebuilt page.
    focus_ = static_cast<int>(i);
    focus_key_ = FocusKeyOf(focus_);
    ScheduleRebuild();
    PhiUiaSetFocusedHit(this, HitOfItem(focus_), /*announce=*/true);
    return;
  }
  if (!shortcut_search_.empty() || !rail_search_.empty()) {
    shortcut_search_.clear();
    rail_search_.clear();
    scroll_ = 0;
    ScheduleRebuild();
    return;
  }
  ::ShowWindow(hwnd(), SW_HIDE);
}

LRESULT PhiSettingsWindow::OnWindowMessage(UINT msg,
                                        WPARAM wparam,
                                        LPARAM lparam,
                                        bool* handled) {
  switch (msg) {
    case WM_GETMINMAXINFO: {
      // Never under 760 × 560 (§7).
      MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lparam);
      info->ptMinTrackSize.x = Scaled(kMinWidth);
      info->ptMinTrackSize.y = Scaled(kMinHeight);
      *handled = true;
      return 0;
    }
    case WM_SIZE:
      // The rail's breakpoint and the page column follow the client rect.
      ScheduleRebuild();
      break;
    case WM_EXITSIZEMOVE: {
      // A resize by hand is remembered (dip, content under the caption);
      // maximize is a window state, not a size, and stays out of it.
      if (!::IsZoomed(hwnd()) && !::IsIconic(hwnd())) {
        RECT client = {};
        ::GetClientRect(hwnd(), &client);
        const int width = ::MulDiv(client.right, 96, static_cast<int>(dpi()));
        const int height =
            ::MulDiv(client.bottom, 96, static_cast<int>(dpi())) - kChromeTop;
        if (width >= kMinWidth && height >= kMinHeight) {
          host_->SetSettingsWindowSize(width, height);
        }
      }
      break;
    }
    case WM_LBUTTONDOWN: {
      // The pointer is in use: the focus ring goes until the next key.
      if (focus_visible_) {
        focus_visible_ = false;
        Invalidate();
      }
      const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (thumb_rect_.right > thumb_rect_.left && ::PtInRect(&thumb_rect_, pt)) {
        scroll_drag_ = true;
        scroll_drag_grab_ = pt.y - thumb_rect_.top;
        ::SetCapture(hwnd());
        Invalidate();
        *handled = true;
        return 0;
      }
      break;
    }
    case WM_LBUTTONUP:
      if (scroll_drag_) {
        scroll_drag_ = false;
        if (::GetCapture() == hwnd()) {
          ::ReleaseCapture();
        }
        Invalidate();
        *handled = true;
        return 0;
      }
      break;
    case WM_MOUSEMOVE:
      if (scroll_drag_) {
        RECT client = {};
        ::GetClientRect(hwnd(), &client);
        const int visible = client.bottom - Scaled(kChromeTop);
        const int track_top = Scaled(kChromeTop) + Scaled(6);
        const int track_height = visible - Scaled(12);
        const int thumb_height = thumb_rect_.bottom - thumb_rect_.top;
        const int span = std::max(1, track_height - thumb_height);
        const int max_scroll = std::max(0, content_height_ - visible);
        const int y = GET_Y_LPARAM(lparam) - scroll_drag_grab_ - track_top;
        scroll_ = std::max(0, std::min(max_scroll, max_scroll * y / span));
        ScheduleRebuild();
        *handled = true;
        return 0;
      }
      // Only a slider drag is answered here; every other move belongs to
      // the base, which updates the hover the paint pass reads. The drag
      // replays the item CAPTURED on the press, not whatever happens to
      // sit under the pointer now.
      if (reorder_drag_ && (wparam & MK_LBUTTON)) {
        const int target = ReorderIndexAt(GET_Y_LPARAM(lparam));
        const auto it = std::find(reorder_order_.begin(),
                                  reorder_order_.end(), reorder_id_);
        if (it != reorder_order_.end()) {
          const int at = static_cast<int>(it - reorder_order_.begin());
          if (at != target) {
            reorder_order_.erase(it);
            reorder_order_.insert(
                reorder_order_.begin() + static_cast<ptrdiff_t>(target),
                reorder_id_);
          }
        }
        ScheduleRebuild();
        ::SetCursor(::LoadCursorW(nullptr, IDC_SIZENS));
        *handled = true;
        return 0;
      }
      if (dragging_ && (wparam & MK_LBUTTON)) {
        Apply(drag_item_, GET_X_LPARAM(lparam));
        ScheduleRebuild();
        *handled = true;
        return 0;
      }
      // The membership card's light follows the pointer while it is over
      // the card and settles when it leaves (the base keeps the hover).
      for (const Item& item : items_) {
        if (item.kind != ItemKind::kProfileCard) {
          continue;
        }
        const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        profile_card_.SetPointer(
            ::PtInRect(&item.bounds, pt) ? std::optional<POINT>(pt) : std::nullopt,
            item.bounds);
        if (profile_card_.animating() && !card_timer_.running()) {
          card_timer_.Start(16, /*coalescable=*/true);
        }
        break;
      }
      break;
    case WM_MOUSELEAVE:
      for (const Item& item : items_) {
        if (item.kind == ItemKind::kProfileCard) {
          profile_card_.SetPointer(std::nullopt, item.bounds);
          if (profile_card_.animating() && !card_timer_.running()) {
            card_timer_.Start(16, /*coalescable=*/true);
          }
          break;
        }
      }
      break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      // The focus model (HandleKey). Esc is not one of its keys: it falls
      // through to the base, which routes it to OnEscape().
      if (HandleKey(wparam, lparam, msg == WM_SYSKEYDOWN)) {
        *handled = true;
        return 0;
      }
      break;
    case kMsgFocusActivate:
      ActivateFocus();
      *handled = true;
      return 0;
    case kMsgProfileCardFrame:
      // The card's artwork landed: repaint, and let its entrance play.
      Invalidate();
      if (profile_card_.animating() && !card_timer_.running()) {
        card_timer_.Start(16, /*coalescable=*/true);
      }
      *handled = true;
      return 0;
    case WM_SETCURSOR: {
      // The house rule (phi_popup_chrome.cc): a hand for links, the arrow
      // for everything else — the hover wash is the cue there.
      const int index = ItemOfHit(hovered());
      if (LOWORD(lparam) == HTCLIENT && index >= 0 &&
          items_[static_cast<size_t>(index)].kind == ItemKind::kLink &&
          items_[static_cast<size_t>(index)].enabled) {
        ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
        *handled = true;
        return TRUE;
      }
      // A text input (now a hit, so a click focuses it) takes the I-beam.
      if (LOWORD(lparam) == HTCLIENT && index >= 0 &&
          IsTextInput(items_[static_cast<size_t>(index)])) {
        ::SetCursor(::LoadCursorW(nullptr, IDC_IBEAM));
        *handled = true;
        return TRUE;
      }
      break;
    }
    case WM_CHAR:
      // Typing anywhere feeds a filter: the Shortcuts pane's own field,
      // or the rail's "Find a setting" on every other page — Esc clears,
      // Backspace deletes.
      if (focus_ >= 0 && focus_ < static_cast<int>(items_.size()) &&
          items_[static_cast<size_t>(focus_)].kind ==
              ItemKind::kIconSearch) {
        // The Spaces editor's picker search: typing filters the grid.
        const wchar_t c = static_cast<wchar_t>(wparam);
        if (c == VK_BACK) {
          if (!icon_search_.empty()) {
            icon_search_.pop_back();
          }
        } else if (c >= L' ') {
          icon_search_.push_back(c);
        } else {
          *handled = true;
          return 0;  // Escape is OnEscape's; Enter and Tab type nothing.
        }
        grid_first_row_ = 0;
        ScheduleRebuild();
        *handled = true;
        return 0;
      }
      {
        const bool shortcuts = section_ == "shortcuts";
        std::wstring& target = shortcuts ? shortcut_search_ : rail_search_;
        const wchar_t c = static_cast<wchar_t>(wparam);
        // Escape arrives as WM_KEYDOWN first and OnEscape() has already
        // cleared the filter by now; swallow the character it leaves. So
        // does a Space that activated the focused control (HandleKey).
        if (c == VK_ESCAPE ||
            (c == L' ' && std::exchange(swallow_space_char_, false))) {
          *handled = true;
          return 0;
        }
        if (c == VK_BACK) {
          if (!target.empty()) {
            target.pop_back();
          }
        } else if (c >= L' ') {
          target.push_back(c);
        } else {
          *handled = true;
          return 0;
        }
        // The focus follows the typing into the field that shows it, so the
        // next Space is a space in the filter and not a click on whatever
        // control had the focus.
        const int field = SearchFieldItem(/*rail=*/!shortcuts);
        if (field >= 0) {
          focus_ = field;
          focus_key_ = FocusKeyOf(field);
        }
        scroll_ = 0;
        ScheduleRebuild();
        if (field >= 0) {
          PhiUiaSetFocusedHit(this, HitOfItem(focus_), /*announce=*/true);
        }
      }
      *handled = true;
      return 0;
    case WM_ACTIVATE:
      // The "Set as default" button hands the choice to the Windows
      // Settings app, so the answer changes while this window is in the
      // background. Coming back to the front is the cue to ask again —
      // the Mac re-checks in viewWillAppear for the same reason. This is
      // the ONLY place the registry probe is dropped; everywhere else it
      // is served from the cache. Falls through so the frame and
      // DefWindowProc still see the activation.
      //
      // The focus ring shows on the active window only.
      window_active_ = LOWORD(wparam) != WA_INACTIVE;
      if (!window_active_) {
        Invalidate();
      }
      if (LOWORD(wparam) != WA_INACTIVE) {
        is_default_browser_.reset();
        // The avatar editor and the hosted login are tabs; coming back is
        // the cue to re-read the profile (rate-limited in the service).
        account_->RefreshAccountProfile(/*force=*/false);
        ScheduleRebuild();
      }
      break;
    case WM_CLOSE:
      ::ShowWindow(hwnd(), SW_HIDE);  // Keep it around; reopening is instant.
      *handled = true;
      return 0;
    default:
      break;
  }
  return PhiSecondaryWindow::OnWindowMessage(msg, wparam, lparam, handled);
}

void PhiSettingsWindow::OnDpiChanged(UINT old_dpi) {
  PhiSecondaryWindow::OnDpiChanged(old_dpi);
  EnsureFont();
  ScheduleRebuild();
}

void PhiSettingsWindow::OnTimerFired(UINT_PTR id) {
  if (id == card_timer_.id()) {
    if (profile_card_.Tick()) {
      Invalidate();
    }
    if (!profile_card_.animating()) {
      card_timer_.Stop();
    }
    return;
  }
  if (id == sync_poll_.id()) {
    // Settings ▸ Sync's poll: a rebuild refetches when the list is due and
    // re-arms while the pane stays up.
    sync_poll_.Stop();
    ScheduleRebuild();
    return;
  }
  if (id != profile_poll_.id()) {
    return;
  }
  // Re-read the pending profile; the rebuild re-arms while pending.
  profile_poll_.Stop();
  ScheduleRebuild();
}

void PhiSettingsWindow::OnCaptureLost() {
  scroll_drag_ = false;
  if (reorder_drag_) {
    // A stolen capture drops the grip's drag where it started.
    reorder_drag_ = false;
    reorder_order_.clear();
    reorder_id_.clear();
    reorder_from_ = -1;
    ScheduleRebuild();
  }
  dragging_ = false;
}

void PhiSettingsWindow::OnNcDestroy() {
  sync_poll_.Stop();
  profile_poll_.Stop();
  card_timer_.Stop();
  PhiSecondaryWindow::OnNcDestroy();
}

}  // namespace phi_client

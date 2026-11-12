// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_FLUENT_H_
#define PHIBROWSER_WIN_CLIENT_PHI_FLUENT_H_

#include <windows.h>

#include "client/phi_theme.h"

// The Windows-native design system's shared pieces
// (docs/windows-native-ui.md §2): the ONE icon glyph map, the ONE icon
// button, the type ramp. Every surface draws its buttons through
// PhiPaintIconButton so none can invent its own wash, size or weight —
// the rule Jixiang's review of the mismatched pin / reload / search glyphs
// made (§2.2a).

namespace phi_client {

// --- Glyphs (Segoe Fluent Icons / Segoe MDL2 Assets, same code points) ----
namespace fluent {
inline constexpr wchar_t kBack[] = L"";
inline constexpr wchar_t kForward[] = L"";
inline constexpr wchar_t kRefresh[] = L"";
inline constexpr wchar_t kStop[] = L"";  // Cancel: stop while loading.
inline constexpr wchar_t kSearch[] = L"";
inline constexpr wchar_t kAdd[] = L"";
inline constexpr wchar_t kMore[] = L"";  // The app menu's ⋯.
inline constexpr wchar_t kClosePane[] = L"";  // Sidebar shown.
inline constexpr wchar_t kOpenPane[] = L"";   // Sidebar hidden.
inline constexpr wchar_t kPin[] = L"";
inline constexpr wchar_t kUnpin[] = L"";
inline constexpr wchar_t kDownload[] = L"";
inline constexpr wchar_t kPuzzle[] = L"";
inline constexpr wchar_t kFavoriteStar[] = L"";
inline constexpr wchar_t kFavoriteStarFill[] = L"";
inline constexpr wchar_t kReadingMode[] = L"";
inline constexpr wchar_t kHistory[] = L"";
inline constexpr wchar_t kPrint[] = L"";
inline constexpr wchar_t kZoomIn[] = L"";
inline constexpr wchar_t kZoomOut[] = L"";
inline constexpr wchar_t kFullScreen[] = L"";
inline constexpr wchar_t kSettings[] = L"";
inline constexpr wchar_t kHelp[] = L"";
inline constexpr wchar_t kFeedback[] = L"";
inline constexpr wchar_t kChevronRight[] = L"";
inline constexpr wchar_t kChevronLeft[] = L"";
inline constexpr wchar_t kChevronDown[] = L"";
inline constexpr wchar_t kChevronUp[] = L"";
inline constexpr wchar_t kCheckMark[] = L"";
inline constexpr wchar_t kRemove[] = L"\uE738";
inline constexpr wchar_t kCancel[] = L"";
inline constexpr wchar_t kCopy[] = L"";
inline constexpr wchar_t kUndo[] = L"";
inline constexpr wchar_t kCut[] = L"";
inline constexpr wchar_t kPaste[] = L"";
inline constexpr wchar_t kGo[] = L"";  // Forward: "Paste and go".
inline constexpr wchar_t kLink[] = L"";
inline constexpr wchar_t kOpenInNewWindow[] = L"";
inline constexpr wchar_t kNewWindow[] = L"";
inline constexpr wchar_t kFolder[] = L"";
inline constexpr wchar_t kOpenFolder[] = L"";
inline constexpr wchar_t kEmoji[] = L"";
inline constexpr wchar_t kGlobe[] = L"";
inline constexpr wchar_t kDocument[] = L"";
inline constexpr wchar_t kOpenFile[] = L"";
inline constexpr wchar_t kSave[] = L"";
inline constexpr wchar_t kSaveLocal[] = L"\uE78C";  // Save to Folio.
inline constexpr wchar_t kLibrary[] = L"\uE8F1";    // Open Folio.
inline constexpr wchar_t kDelete[] = L"";
inline constexpr wchar_t kRename[] = L"";
inline constexpr wchar_t kColor[] = L"";
inline constexpr wchar_t kDeveloperTools[] = L"";
inline constexpr wchar_t kCode[] = L"";
inline constexpr wchar_t kTaskView[] = L"";
inline constexpr wchar_t kProcessing[] = L"";  // Task manager.
inline constexpr wchar_t kDockLeft[] = L"";
inline constexpr wchar_t kDockRight[] = L"";
inline constexpr wchar_t kDockBottom[] = L"";
inline constexpr wchar_t kView[] = L"";  // Layout.
inline constexpr wchar_t kInfo[] = L"";  // About.
inline constexpr wchar_t kSync[] = L"";  // Check for update.
inline constexpr wchar_t kLightbulb[] = L"";  // What's new.
inline constexpr wchar_t kLeaveChat[] = L"";
inline constexpr wchar_t kPowerButton[] = L"";  // Exit.
inline constexpr wchar_t kImport[] = L"";
inline constexpr wchar_t kExport[] = L"";
inline constexpr wchar_t kEraseTool[] = L"";  // Delete browsing data.
inline constexpr wchar_t kPeople[] = L"";
inline constexpr wchar_t kContact[] = L"";
inline constexpr wchar_t kIncognito[] = L"";  // Hide.
inline constexpr wchar_t kTabs[] = L"";  // Switch apps: Spaces.
inline constexpr wchar_t kFind[] = L"";
inline constexpr wchar_t kTag[] = L"";
inline constexpr wchar_t kMute[] = L"";
inline constexpr wchar_t kVolume[] = L"";
inline constexpr wchar_t kClose[] = L"";  // The OS close glyph, 10 dip.
inline constexpr wchar_t kMinimize[] = L"";
inline constexpr wchar_t kMaximize[] = L"";
inline constexpr wchar_t kRestore[] = L"";
inline constexpr wchar_t kAccept[] = L"";
inline constexpr wchar_t kEye[] = L"";  // Peek.
inline constexpr wchar_t kRobot[] = L"";
inline constexpr wchar_t kHome[] = L"";
// The address bar's site menu: sliders, the Mac's "set" mark
// (extension-button.json). Equalizer in both MDL2 and Fluent Icons.
inline constexpr wchar_t kEqualizer[] = L"\uE9E9";
// The site menu's rows: content blocking and the security row.
inline constexpr wchar_t kShield[] = L"\uEA18";
inline constexpr wchar_t kLock[] = L"\uE72E";
inline constexpr wchar_t kStatusError[] = L"\uEA39";
inline constexpr wchar_t kClock[] = L"";  // Recently closed.
inline constexpr wchar_t kUpload[] = L"";  // Drop zone.
inline constexpr wchar_t kDiagnostic[] = L"";  // Feedback diagnostics.

// The three icon sizes (§2.2a), in dip.
inline constexpr int kIconToolbar = 16;
inline constexpr int kIconPip = 14;
inline constexpr int kIconSettings = 20;
inline constexpr int kIconCaption = 10;
// The one toolbar button: 32 square, radius 4. In-field slots: 24.
inline constexpr int kButton = 32;
inline constexpr int kFieldButton = 24;
inline constexpr int kButtonRadius = 4;
inline constexpr int kSurfaceRadius = 8;  // Flyouts, dialogs, sheets.
}  // namespace fluent

// --- The one button --------------------------------------------------------
enum class PhiButtonState {
  kRest,      // Transparent.
  kHover,     // ink 4.5 %.
  kPressed,   // ink 9 %, icon at 75 %.
  kDisabled,  // icon at 36 %, no wash.
  kToggled,   // accent_soft fill, icon in the accent.
};

// Paints one icon button: the state's wash in a `radius` rounded rect
// over `rect`, then `glyph` centred at `glyph_px`. `ink` is the resting
// icon colour (`label` on toolbars, `label_dim` in fields); the state
// derives the rest from the palette. The DC's ground under `rect` must
// already be painted (the wash blends over it). No font needs to be
// selected: the glyph goes through DirectWrite in the icon face.
void PhiPaintIconButton(HDC dc,
                        const RECT& rect,
                        const wchar_t* glyph,
                        PhiButtonState state,
                        const PhiPalette& palette,
                        COLORREF ink,
                        int glyph_px,
                        int radius_px);

// The wash alone (for a button whose picture is not a glyph: a favicon, a
// catalog mask, the Phi mark).
void PhiPaintButtonWash(HDC dc,
                        const RECT& rect,
                        PhiButtonState state,
                        const PhiPalette& palette,
                        int radius_px);
// The icon colour the state wants, from the resting `ink`.
COLORREF PhiButtonInk(PhiButtonState state,
                      const PhiPalette& palette,
                      COLORREF ink);

// --- Type ------------------------------------------------------------------
// The §2.2 ramp: Segoe UI Variable Text (11–14) / Display (20, 28) on
// Windows 11, Segoe UI on Windows 10. `size` in dip, scaled for `dpi`;
// `weight` FW_NORMAL or FW_SEMIBOLD. Caller owns the HFONT.
HFONT PhiFluentFont(UINT dpi, int size, int weight = FW_NORMAL);
// The icon face at `size` dip (GDI fallback path for glyph sites that keep
// a font selected). Caller owns the HFONT.
HFONT PhiIconFont(UINT dpi, int size);
// The face name the ramp resolved to: "Segoe UI Variable Text" /
// "Segoe UI Variable Display" / "Segoe UI".
const wchar_t* PhiTextFaceName(bool display);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_FLUENT_H_

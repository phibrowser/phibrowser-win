// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SPACES_STRIP_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SPACES_STRIP_H_

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_theme.h"

namespace phi_client {

class AgentHost;
class ChromePrefsHost;
class PhiCreateSpacePanel;
class PhiShellWindow;
class SpacesHost;
struct PhiNewSpaceSpec;

// One Space as the strip draws it — the Mac's stripOrderedSpaces entry.
// `is_agent` pips carry a taskId in `id`, not a Space id.
struct PhiSpaceChip {
  std::string id;
  std::wstring label;
  std::wstring icon;  // The Space's emoji, empty when it has none.
  bool open = false;
  // A live agent Space pip: `id` is its Space id ("agentspace-N", what the
  // slot hosts it under, so the active pill and the swap slide find it
  // like any Space's), `task_id` the skill's taskId the watch is asked by.
  bool is_agent = false;
  std::string task_id;
  // An agent pip's corner badge (PhiAgentPipView::Badge + 1; 0 = none).
  int badge = 0;
  RECT bounds{};
};

// Where the strip is laid out, taken from the shell's per-turn Layout so the
// strip never derives geometry of its own (docs/architecture-review.md §3.5).
struct PhiSpacesStripRects {
  RECT client{};
  RECT strip{};      // The band the pips live in.
  // What a viewport step repaints: the strip band, or — where the pips
  // ride the caption row — that row.
  RECT strip_repaint{};
  RECT sidebar{};    // The create-Space form mounts under the strip in it.
  RECT add_space{};  // The trailing "+" / "…" slot.
  int chip_size = 0;
  int chips_left = 0;
  int chips_top = 0;
  int chips_limit = 0;  // Right edge the pips may not cross.
  bool horizontal_tabs = false;
  // False with nothing to switch to (one Space, no agent pips, no form):
  // the strip lays out no pips and no "+".
  bool spaces_switch = true;
};

// What the Spaces strip asks of its shell.
class PhiSpacesStripShell {
 public:
  virtual HWND SpacesShellWindow() const = 0;
  // The controller the create-Space form is constructed against (it reads
  // the shell's layout and palette through it), as with the native pages.
  virtual PhiShellWindow* SpacesOwner() = 0;
  virtual SlotId SpacesSlot() const = 0;
  virtual bool SpacesIncognito() const = 0;
  virtual bool SpacesIsAgent() const = 0;
  virtual int SpacesScaled(int design_units) const = 0;
  virtual PhiSpacesStripRects SpacesRects() = 0;
  // The Space this slot currently shows.
  virtual const std::string& SpacesActiveSpace() const = 0;
  virtual bool SpacesHasSpace(const std::string& space_id) const = 0;
  // The ramped palette the whole chrome is painting with right now.
  virtual PhiPalette SpacesPalette() const = 0;
  // A drag is live: the viewport may not slide, and the hover card stays
  // down.
  virtual bool SpacesDragActive() const = 0;
  // The chip the shell's hover state currently names, or -1.
  virtual int SpacesHoveredChip() const = 0;
  virtual bool SpacesAddSlotHovered() const = 0;
  // The trailing slot's invalidation rect (the hover wash's, inflated).
  virtual RECT SpacesAddSlotDirtyRect() = 0;
  // Forget the shell's hover so a viewport step re-registers as a change.
  virtual void SpacesForgetHover() = 0;
  // Re-run the shell's hover hit-test against the pointer where it rests.
  virtual void SpacesRehoverAtCursor() = 0;
  virtual void SpacesScheduleRefresh() = 0;
  // The palette moved: repaint the chrome that is not the strip's.
  virtual void SpacesApplyPalette() = 0;
  // The tinted satellites that ride the palette (the native pages, the
  // content card's corner caps).
  virtual void SpacesRefreshTinted() = 0;
  virtual void SpacesRepaintOmnibox() = 0;
  virtual void SpacesFocusContent() = 0;
  // The create form covers the sidebar below the strip (or has left it): the
  // covered chrome that is a real child window — the address EDIT — cannot be
  // painted over by a sibling and must hide for the duration (the Mac's
  // setCreateSpaceCoveredContentSuppressed).
  virtual void SpacesSetCoveredContentSuppressed(bool suppressed) = 0;
  // The band slide landed: the parked browser child becomes visible.
  virtual void SpacesRevealSwappedContent() = 0;
  // Swap the live sidebar rows with the retained leaving Space's — the
  // slide paints both models, so the shell exchanges them twice per frame.
  virtual void SpacesSwapBandModel() = 0;
  // Drop the retained leaving rows once the slide is over.
  virtual void SpacesDropLeavingBand() = 0;
  // The strip's fonts, all built and owned by the shell.
  virtual HFONT SpacesUiFont() const = 0;
  virtual HFONT SpacesSmallFont() const = 0;
  virtual HFONT SpacesEmojiFont() const = 0;
  virtual HFONT SpacesChipIconFont() const = 0;
  virtual HFONT SpacesPipIconFont() const = 0;
  virtual HFONT SpacesPipEmojiFont() const = 0;
  virtual HFONT SpacesPipTextFont() const = 0;
  // The open tabs a Space's browser holds (the hover flyout's "N tabs");
  // 0 when the Space has no live browser in this slot.
  virtual int SpacesTabCount(const std::string& space_id) const = 0;
  // Whether the sidebar can host the create-Space form inline, and the
  // window the hover card's popup is positioned against when the floating
  // sidebar panel is the one showing the strip.
  virtual bool SpacesSidebarCanHostForm(const RECT& form) const = 0;
  // Where the create-Space FLYOUT hangs when the sidebar cannot host the
  // form (§5.5): the Space chip or the strip's "+" while the strip rides
  // the shell, else the sidebar toggle. Screen coordinates; false in the
  // windows that take the standalone window instead (agent, incognito).
  virtual bool SpacesCreateFlyoutAnchor(RECT* screen) const = 0;
  virtual HWND SpacesFloatingPanel(int* offset_y) const = 0;

 protected:
  // Non-virtual and protected: the shell OWNS the strip, never the other
  // way round, so nothing ever deletes through this pointer.
  ~PhiSpacesStripShell() = default;
};

// THE Spaces strip (docs/architecture-review.md §3.5; lane 1 clusters K and
// O plus the chip half of H): the pips, their viewport and overflow, the
// hover card, the trailing "+" and its fade, the create-Space form host and
// the Space-switch animation.
//
// INVARIANTS
//  * `chips_` is written in ONE function — Rebuild() — from the owner's
//    full Space list plus the shell's per-turn rects. Paint, hit tests and
//    menus read it; none of them writes it.
//  * A hit test answers a chip INDEX into that vector or -1, and a peeked
//    pip is hit only on the part the viewport clip shows.
//  * Every animation the strip runs — the "+" fade, the band slide and the
//    theme ramp — is a PhiAnimator animation bound to the shell window,
//    invalidating the rect it actually moves. The strip owns no WM_TIMER.
//  * The palette override (the create form's live preview) and the ramp are
//    applied in ONE place, RampPalette(), which the shell's CurrentPalette()
//    calls; nothing else blends a palette.
class PhiSpacesStrip {
 public:
  // The three narrow services the strip calls — the Space list it draws,
  // the client-wide chrome prefs the create form seeds from, and the agent
  // pips (docs/architecture-review.md §3.5). Never the shell's whole set:
  // the strip cannot reach a store row or another window from here.
  PhiSpacesStrip(PhiSpacesStripShell* shell,
                 SpacesHost* spaces_host,
                 ChromePrefsHost* prefs_host,
                 AgentHost* agent_host);
  ~PhiSpacesStrip();

  PhiSpacesStrip(const PhiSpacesStrip&) = delete;
  PhiSpacesStrip& operator=(const PhiSpacesStrip&) = delete;

  // --- The pip row --------------------------------------------------------
  // THE writer of the chip vector. Runs on the shell's rebuild turn.
  void Rebuild();
  const std::vector<PhiSpaceChip>& chips() const { return chips_; }
  // The row index of chips()[0] — a windowed viewport starts partway along
  // the full row, and a reorder has to name the row position.
  int first_index() const { return first_index_; }
  bool overflow() const { return overflow_; }
  const RECT& add_slot() const { return add_slot_; }
  // The chip under `client_pt`, or -1. Peeked pips are hit only where the
  // viewport clip shows them.
  int ChipAt(POINT client_pt) const;
  RECT ChipBounds(int index) const;
  // The insertion slot a pip drag at `client_pt` would land in.
  int InsertSlotAt(POINT client_pt) const;
  // stepViewport: +1 slides the window toward the row's end.
  void StepViewport(int step);
  // The wheel over the strip band; true when consumed.
  bool OnWheel(int wheel_delta);
  // --- The trailing "+" reveal --------------------------------------------
  // The Mac's slot.isStripRowHovered: the "+" is up only while the pointer
  // is over the strip's ROW.
  void SetRowHovered(bool hovered);
  bool row_hovered() const { return row_hovered_; }
  double AddSlotRevealProgress() const;

  // --- The hover card -----------------------------------------------------
  // Re-aims the card at `chip_index` (-1 = none), which the shell's hover
  // state names.
  void UpdateHoverCard(int chip_index);
  // Delay elapsed: the pending card becomes the shown one.
  void PromoteHoverCard();
  // `suppress_space_id` keeps the card down until the pointer leaves that
  // pip (a click on it), empty to simply take it down.
  void DismissHoverCard(const std::string& suppress_space_id);
  // The card's popup timer id, so the shell's WM_TIMER can route to
  // PromoteHoverCard. The card is the one thing that still needs a real
  // delay timer — an animation clock is the wrong tool for a dwell.
  static constexpr UINT_PTR kHoverCardTimer = 0x516;

  // --- The create-Space form ----------------------------------------------
  void ShowCreateForm();
  void CommitCreateForm(const PhiNewSpaceSpec& spec);
  void CancelCreateForm();
  void PreviewTheme(const std::string& theme_id, int saturation);
  void PreviewPalette(const std::string& theme_id,
                      const PhiThemePalette& palette);
  // The app-wide theme or appearance moved under a live preview (the
  // editor's Light / Dark buttons): the preview is re-resolved from its
  // inputs so the chrome does not keep the other appearance's colours.
  void RefreshPreview();
  void SetFormIcon(const std::string& storage_value);
  void OnFormProfileCreated(const std::string& profile_id);
  bool creating() const { return creating_; }
  // How many pips the strip would offer: the user Spaces plus the agent
  // Spaces this slot lists. The layout drops the strip (and comfortable's
  // compact pip) when there is only one and no form is up.
  int SwitchTargets() const;
  const std::wstring& creating_icon_text() const { return creating_icon_; }
  // Where the form mounts: the sidebar below the strip.
  RECT FormRect();
  // The form's live theme preview ends (it was cancelled, or the created
  // Space has presented).
  void EndPreview();
  // The preview is being held ACROSS the created Space's arrival, so the
  // chrome never flashes the source theme in between: the Space presenting
  // is what ends it.
  bool preview_awaits_switch() const { return preview_until_switch_; }
  // The form panel follows a layout change.
  void RepositionForm();
  // The form, the card popup and every animation go before the shell
  // window they belong to.
  void Shutdown();

  // --- The Space switch ---------------------------------------------------
  // Begins the band slide + theme ramp toward `entering_space_id`.
  void BeginSwap(const std::string& entering_space_id);
  // Settles an in-flight switch instantly (a superseded switch, a resize,
  // the window going away).
  void CancelSwap();
  bool swap_running() const { return swap_start_ != 0; }
  bool swap_forward() const { return swap_forward_; }
  // Which way a switch from the active Space to `entering_space_id` runs:
  // forward when the entering Space is later in strip order (an agent
  // Space is past every user Space) — the Mac's SwapDirection. The same
  // rule BeginSwap's band slide uses; the comfortable layout's content
  // slide reads it from here since it runs no band slide.
  bool SlideForward(const std::string& entering_space_id) const;
  // A Space's place in strip order — the user Spaces, then this slot's
  // agent Spaces (the Mac's presentedSpaces) — whatever the layout shows;
  // one past the end for a Space the strip does not list.
  int StripPosition(const std::string& space_id) const;
  // The slide's eased position, 1.0 when idle.
  double SwapProgress() const;
  // The palette the leaving Space's retained rows are painted at — frozen
  // when the switch began, so the two models never share a tint.
  const PhiPalette& swap_source_palette() const {
    return swap_source_palette_;
  }
  // The slide reached its end: the shell drops the retained rows and the
  // page flips. Called from the band paint, which is where the last frame
  // of the slide is known to have been drawn.
  void NoteSlideLanded();
  // `target` blended toward from the frozen source palette while the ramp
  // runs, or replaced outright by the create form's live preview. THE one
  // place a palette is overridden.
  PhiPalette RampPalette(const PhiPalette& target) const;

  // --- Paint --------------------------------------------------------------
  // The pips, the placeholder and the trailing slot, into the shell's DC.
  void Paint(HDC dc, const PhiSpacesStripRects& rects,
             const PhiPalette& palette);
  // The hover card's own popup window, measured against the shell's DC and
  // shown / moved / hidden. Runs from the shell's paint, after Paint().
  void SyncHoverCardPopup(HDC dc, const PhiSpacesStripRects& rects);

 private:
  // The measured hover card, at the popup's own origin.
  // The two-line Fluent flyout (docs/windows-native-ui.md §5.6):
  //   [icon]  Name                     Ctrl+2
  //           profile · N tabs
  struct CardGeometry {
    RECT card{};
    RECT tile{};   // The 20 dip icon tile.
    RECT name{};   // Line 1's text.
    RECT hint{};   // Line 2's text.
    RECT keycap{}; // Trailing; empty past the ninth Space.
    std::wstring icon, name_text, hint_text, keycap_text;
    bool agent = false;
    int badge = 0;  // The agent status dot (PhiAgentPipView::Badge + 1).
  };

  bool MeasureHoverCard(HDC dc,
                        const PhiSpacesStripRects& rects,
                        CardGeometry* out);
  void PaintHoverCard(HDC dc,
                      const CardGeometry& g,
                      const PhiPalette& palette);
  void PaintHoverCardPopup(HWND popup, HDC dc);
  static LRESULT CALLBACK HoverCardPopupProc(HWND hwnd,
                                             UINT msg,
                                             WPARAM wparam,
                                             LPARAM lparam);
  void ResolveHoverCard(const std::string& space_id);
  // The room the pips may paint in: the overflow clip where the row is
  // windowed, else the span the layout gave them.
  RECT ViewportRect(const PhiSpacesStripRects& rects) const;
  // Whether the windowed row currently shows a Space's pip at all.
  bool ChipShown(const std::string& space_id) const;
  // Where the selected pill starts its slide: the leaving Space's pip where
  // the viewport still shows it, else just outside the edge the leaving pip
  // scrolled past, so the pill still arrives from the source's direction.
  RECT SwapPillOrigin(const RECT& entering,
                      const PhiSpacesStripRects& rects) const;
  // Restarts the "+" fade animation with `ms` still to run.
  void StartFade(int ms);
  void StopAnimations();

  // Owns this strip and outlives it.
  RAW_PTR_EXCLUSION PhiSpacesStripShell* const shell_;
  // The coordinator's services, handed in at construction; they outlive the
  // shell that owns this strip. Any of them may be null (a stand-in shell).
  RAW_PTR_EXCLUSION SpacesHost* const spaces_host_;
  RAW_PTR_EXCLUSION ChromePrefsHost* const prefs_host_;
  RAW_PTR_EXCLUSION AgentHost* const agent_host_;

  // The windowed pip row (the Mac's SpacesStripView viewport): `chips_`
  // holds the whole pips the window shows plus the peeked neighbours,
  // `clip_` bounds both paint and hit tests (empty when nothing is hidden),
  // and `first_index_` is the row index of chips_[0].
  std::vector<PhiSpaceChip> chips_;
  int start_ = 0;  // stripStartIndex: the first WHOLE pip shown.
  int first_index_ = 0;
  RECT clip_{};
  // The window re-centres on the active pip when the active Space or the
  // pip count changes; wheel steps move it freely in between, so both are
  // remembered to tell the two apart.
  std::string anchor_space_;
  int anchor_visible_ = -1;
  bool overflow_ = false;
  RECT add_slot_{};
  RECT creating_pip_{};

  // The trailing "+"'s reveal fade.
  bool row_hovered_ = false;
  ULONGLONG fade_start_ = 0;  // Fade clock; 0 = settled.
  int fade_anim_ = 0;

  // The hover card.
  std::string card_space_;       // Card owner (empty = no card).
  std::string pending_card_;     // Scheduled, awaiting the delay.
  std::string suppressed_card_;  // Clicked; no card until hover-off.
  std::wstring card_profile_;
  int card_shortcut_ = 0;  // 1-based Ctrl+N digit; 0 = past the ninth.
  // The flyout's type (name 12/600, hint 11, keycap 10), per DPI.
  HFONT card_name_font_ = nullptr;
  HFONT card_hint_font_ = nullptr;
  HFONT card_key_font_ = nullptr;
  UINT card_fonts_dpi_ = 0;
  void EnsureCardFonts();
  void ReleaseCardFonts();
  CardGeometry card_geom_;
  HWND card_popup_ = nullptr;
  RECT card_popup_rect_{};

  // The create-Space form.
  std::unique_ptr<PhiCreateSpacePanel> form_;
  bool creating_ = false;
  std::wstring creating_icon_;
  std::string creating_source_;  // The active Space as the form opened.
  std::optional<PhiPalette> preview_palette_;
  // The preview's inputs, so RefreshPreview can re-resolve them.
  std::string preview_theme_id_;
  int preview_saturation_ = -1;
  std::optional<PhiThemePalette> preview_record_;
  bool preview_until_switch_ = false;

  // The Space switch: the band slide's clock and the theme ramp's, plus the
  // Space the selected pill slides FROM and the frozen source palette. The
  // start rect is resolved at paint time, not captured here: the switch
  // re-centres the viewport on the entering pip, so the leaving pip has
  // usually moved (or scrolled out) by the time the first frame is drawn.
  ULONGLONG swap_start_ = 0;
  ULONGLONG ramp_start_ = 0;
  bool swap_forward_ = true;
  std::string swap_from_space_;
  PhiPalette swap_source_palette_{};
  int swap_anim_ = 0;
  int ramp_anim_ = 0;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SPACES_STRIP_H_

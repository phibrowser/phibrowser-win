// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_space_theme_panel.h"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/no_destructor.h"
#include "client/phi_dwrite.h"
#include "client/phi_fluent.h"
#include "client/phi_i18n.h"
#include "client/phi_modal_scope.h"
#include "client/phi_popover.h"
#include "client/phi_theme.h"
#include "client/phi_shell_window.h"

namespace phi_client {

namespace {

// SpaceThemeEditorView's metrics, in DIP: contentSize 260x296, the field
// 244 square under 8 of padding, a 10 gap, the 24 toolbar, 10 below.
constexpr int kPanelWidth = 260;
// 296 + the preset strip and its gap + the brightness slider and its gap.
constexpr int kPanelHeight = 352;
constexpr int kPadding = 8;
constexpr int kField = 244;
constexpr int kFieldRadius = 12;  // LiquidGlassCompatible.cornerRadius.
constexpr int kToolbarGap = 10;
constexpr int kToolbarHeight = 24;
constexpr int kToolButton = 22;  // In a 1-pt padded capsule group.
constexpr int kToolPad = 1;
// The handles: primary, second, third; their white rim; the grab radius.
constexpr std::array<int, 3> kHandleDiameter = {38, 28, 14};
constexpr int kHandleRim = 2;
constexpr int kHandleGrab = 20;
// A press that travels less than this is a click (moves the primary).
constexpr int kClickSlop = 3;
// The field bitmap's presentation softening (ThemeEditorPaletteRenderer):
// a Gaussian of 7 % of the side, then saturation 0.84 and contrast 0.96.
constexpr double kBlurFraction = 0.07;
constexpr double kFieldSaturation = 0.84;
constexpr double kFieldContrast = 0.96;
// A click's eased handle transition (timingCurve 0.22, 1, 0.36, 1; 0.3 s).
constexpr int kClickAnimationMs = 300;
constexpr int kAnimationTickMs = 16;

// hits_ keys.
constexpr int kFieldKey = 1;
constexpr int kLightKey = 2;
constexpr int kDarkKey = 3;
constexpr int kHarmonyKey = 10;  // + the harmony's index below.
constexpr int kPreviousKey = 20;
constexpr int kNextKey = 21;
constexpr int kResetKey = 22;
constexpr int kSaveKey = 23;
constexpr int kBrightnessKey = 24;
constexpr int kPresetKey = 30;  // + the slot on the page, 0 to 6.
constexpr int kDeleteKey = 40;  // + the slot.

// The preset strip under the field (the Mac's `26eeac82`): seven swatches a
// page between two paging buttons.
constexpr int kStripGap = 8;
// The brightness slider between the field and the strip (GitHub issue 7:
// a dark sidebar could not be made darker than the field's own range). The
// record's `brightness` is the Mac's, which its resolver already honours.
constexpr int kSliderGap = 8;
constexpr int kSliderHeight = 16;
constexpr int kSliderTrack = 6;
constexpr int kSliderThumb = 14;
constexpr double kSliderStep = 0.02;  // One field render per step at most.
constexpr double kSliderKeyStep = 0.05;
constexpr int kStripHeight = 24;
constexpr int kStripButton = 24;
constexpr int kSwatchesPerPage = 7;
constexpr int kSwatch = 18;       // The colour disc, in its 24 slot.
constexpr int kDeleteBadge = 10;
// A saved swatch's delete badge takes a click only after this long under
// the pointer, so a quick click on the swatch never deletes it.
constexpr ULONGLONG kDeleteArmMs = 400;

// Dusty solids, tonal pairs and balanced accents: the Mac's
// recommendedPalettes, three pages of seven.
struct Recommended {
  double hue;
  double saturation;
  double brightness;
  PhiThemePalette::Harmony harmony;
};
constexpr Recommended kRecommended[] = {
    {0.595, 0.25, 0.86, PhiThemePalette::Harmony::kSingle},  // Mist blue
    {0.375, 0.23, 0.80, PhiThemePalette::Harmony::kSingle},  // Sage
    {0.105, 0.22, 0.90, PhiThemePalette::Harmony::kSingle},  // Champagne
    {0.735, 0.24, 0.86, PhiThemePalette::Harmony::kSingle},  // Heather
    {0.960, 0.23, 0.82, PhiThemePalette::Harmony::kSingle},  // Dusty rose
    {0.055, 0.25, 0.88, PhiThemePalette::Harmony::kSingle},  // Apricot
    {0.490, 0.20, 0.92, PhiThemePalette::Harmony::kSingle},  // Sea glass
    {0.540, 0.27, 0.86, PhiThemePalette::Harmony::kAnalogous},  // Coast
    {0.085, 0.24, 0.86, PhiThemePalette::Harmony::kAnalogous},  // Meadow
    {0.815, 0.26, 0.82, PhiThemePalette::Harmony::kAnalogous},  // Orchid
    {0.360, 0.25, 0.80, PhiThemePalette::Harmony::kAnalogous},  // Eucalyptus
    {0.660, 0.28, 0.82, PhiThemePalette::Harmony::kAnalogous},  // Twilight
    {0.965, 0.25, 0.88, PhiThemePalette::Harmony::kAnalogous},  // Peach blossom
    {0.215, 0.22, 0.84, PhiThemePalette::Harmony::kAnalogous},  // Olive grove
    {0.600, 0.28, 0.80, PhiThemePalette::Harmony::kComplementary},  // Blue and clay
    {0.920, 0.25, 0.84, PhiThemePalette::Harmony::kComplementary},  // Rose and mint
    {0.290, 0.23, 0.82, PhiThemePalette::Harmony::kComplementary},  // Fern and mauve
    {0.075, 0.27, 0.86, PhiThemePalette::Harmony::kSplitComplementary},  // Dawn
    {0.720, 0.26, 0.80, PhiThemePalette::Harmony::kSplitComplementary},  // Iris garden
    {0.460, 0.24, 0.84, PhiThemePalette::Harmony::kSplitComplementary},  // Tidal pool
    {0.000, 0.22, 0.88, PhiThemePalette::Harmony::kSplitComplementary},  // Porcelain
};
constexpr int kRecommendedCount =
    static_cast<int>(sizeof(kRecommended) / sizeof(kRecommended[0]));

constexpr std::array<PhiThemePalette::Harmony, 4> kHarmonies = {
    PhiThemePalette::Harmony::kSingle, PhiThemePalette::Harmony::kAnalogous,
    PhiThemePalette::Harmony::kComplementary,
    PhiThemePalette::Harmony::kSplitComplementary};

const char* HarmonyLabelKey(PhiThemePalette::Harmony harmony) {
  switch (harmony) {
    case PhiThemePalette::Harmony::kSingle:
      return "theme.editor.singleColor";
    case PhiThemePalette::Harmony::kAnalogous:
      return "theme.editor.analogousColors";
    case PhiThemePalette::Harmony::kComplementary:
      return "theme.editor.twoColors";
    case PhiThemePalette::Harmony::kSplitComplementary:
      return "theme.editor.threeColors";
  }
  return "theme.editor.singleColor";
}

double SrgbToLinear(double c) {
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double LinearToSrgb(double c) {
  c = std::min(std::max(c, 0.0), 1.0);
  return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055;
}

// One box pass of `radius` along the rows of a `size`-square float plane
// (3 channels interleaved), edges clamped — the Mac clamps the field to
// its extent before blurring so the corners stay opaque.
void BoxBlurRows(std::vector<float>& plane, int size, int radius) {
  if (radius <= 0) {
    return;
  }
  std::vector<float> row(static_cast<size_t>(size) * 3);
  const float inv = 1.0f / static_cast<float>(2 * radius + 1);
  for (int y = 0; y < size; ++y) {
    const size_t base = static_cast<size_t>(y) * size * 3;
    const auto at = [&](int x, int c) {
      return plane[base + static_cast<size_t>(std::clamp(x, 0, size - 1)) * 3 +
                   static_cast<size_t>(c)];
    };
    for (int c = 0; c < 3; ++c) {
      float sum = 0;
      for (int k = -radius; k <= radius; ++k) {
        sum += at(k, c);
      }
      for (int x = 0; x < size; ++x) {
        row[static_cast<size_t>(x) * 3 + static_cast<size_t>(c)] = sum * inv;
        sum += at(x + radius + 1, c) - at(x - radius, c);
      }
    }
    std::copy(row.begin(), row.end(), plane.begin() + static_cast<ptrdiff_t>(base));
  }
}

void Transpose(std::vector<float>& plane, int size) {
  std::vector<float> out(plane.size());
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      for (int c = 0; c < 3; ++c) {
        out[(static_cast<size_t>(x) * size + y) * 3 + c] =
            plane[(static_cast<size_t>(y) * size + x) * 3 + c];
      }
    }
  }
  plane.swap(out);
}

// The field bitmap, `size` device pixels square, BGRA (alpha 255):
// every pixel sampled through the palette's own coordinate mapping and
// colour maps, then softened in linear light for presentation. The
// handles keep the exact colours; the field is a visual guide.
std::vector<uint8_t> RenderField(double brightness, bool dark, int size) {
  const PhiThemePalette sampler =
      PhiThemePalette::Make(0, 0, brightness, PhiThemePalette::Harmony::kSingle);
  std::array<float, 256> linear_lut;
  for (int i = 0; i < 256; ++i) {
    linear_lut[static_cast<size_t>(i)] =
        static_cast<float>(SrgbToLinear(i / 255.0));
  }
  std::vector<float> plane(static_cast<size_t>(size) * size * 3);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const PhiThemePalette::Point point = {(x + 0.5) / size,
                                            (y + 0.5) / size};
      const PhiColor rgb = PhiColorFromHsb(sampler.ColorAt(point, dark));
      const size_t px = (static_cast<size_t>(y) * size + x) * 3;
      plane[px] = linear_lut[static_cast<size_t>(PhiRed(rgb))];
      plane[px + 1] = linear_lut[static_cast<size_t>(PhiGreen(rgb))];
      plane[px + 2] = linear_lut[static_cast<size_t>(PhiBlue(rgb))];
    }
  }
  // Three box passes each way approximate the Gaussian (box radius ≈ σ).
  const int radius = static_cast<int>(std::lround(kBlurFraction * size));
  for (int pass = 0; pass < 3; ++pass) {
    BoxBlurRows(plane, size, radius);
  }
  Transpose(plane, size);
  for (int pass = 0; pass < 3; ++pass) {
    BoxBlurRows(plane, size, radius);
  }
  Transpose(plane, size);
  std::vector<uint8_t> bgra(static_cast<size_t>(size) * size * 4);
  for (size_t i = 0; i < static_cast<size_t>(size) * size; ++i) {
    const size_t px = i * 3;
    const double luma = 0.2126 * plane[px] + 0.7152 * plane[px + 1] +
                        0.0722 * plane[px + 2];
    const size_t out = i * 4;
    for (size_t c = 0; c < 3; ++c) {
      double value = luma + (plane[px + c] - luma) * kFieldSaturation;
      value = (value - 0.5) * kFieldContrast + 0.5;
      // BGRA: channel c of RGB lands at 2 - c.
      bgra[out + 2 - c] =
          static_cast<uint8_t>(std::lround(LinearToSrgb(value) * 255));
    }
    bgra[out + 3] = 255;
  }
  return bgra;
}

// The Mac's timingCurve(0.22, 1, 0.36, 1): a strong ease-out. Its cubic's
// x-channel is near-linear, so the y-channel over t stands in for it.
double EaseOut(double t) {
  t = std::min(std::max(t, 0.0), 1.0);
  const double u = 1 - t;
  // Bezier y with control points 1 and 1.
  return 3 * u * u * t * 1 + 3 * u * t * t * 1 + t * t * t;
}

double LerpHue(double from, double to, double t) {
  double delta = to - from;
  if (delta > 0.5) {
    delta -= 1;
  } else if (delta < -0.5) {
    delta += 1;
  }
  return from + delta * t;
}

RECT CircleRect(double cx, double cy, double radius) {
  return {static_cast<int>(std::lround(cx - radius)),
          static_cast<int>(std::lround(cy - radius)),
          static_cast<int>(std::lround(cx + radius)),
          static_cast<int>(std::lround(cy + radius))};
}

// The editor. Its model is the Space's record (or the form's draft), read
// back from the host on every paint so an outside change shows at once;
// the window, the placement, hover, the capture and dismissal are
// PhiPopover's.
class Panel : public PhiPopover {
 public:
  // The Space editor: reads and writes `space_id`'s record through the
  // controller's SpacesHost.
  Panel(PhiShellWindow* controller,
        std::string space_id,
        std::function<void()> on_closed)
      : controller_(controller),
        space_id_(std::move(space_id)),
        on_closed_(std::move(on_closed)) {}

  // The draft editor: edits `editor.palette` in place and reports.
  Panel(PhiShellWindow* controller, PhiThemeDraftEditor editor)
      : controller_(controller),
        draft_(true),
        editor_(std::move(editor)),
        on_closed_(std::move(editor_.on_closed)) {}

  ~Panel() override = default;

  bool Open(HWND owner, const RECT& anchor_screen, bool beside) {
    if (!owner) {
      return false;
    }
    ClassSpec spec;
    spec.name = L"PhiSpaceThemePanel";
    Options options;
    Placement placement;
    placement.anchor = anchor_screen;
    if (beside) {
      // The create form's popover: to the row's right (arrowEdge .trailing).
      placement.side = PhiPopoverSide::kRightTop;
      placement.gap_dip = 8;
    } else {
      placement.side = PhiPopoverSide::kBelow;
      // A point anchor (a menu click) hangs the panel off its left edge; an
      // anchor WIDER than the panel — a window's caption band — centres it
      // there instead, which is the Mac's nearTrigger / windowTop pair.
      placement.align = PhiPopoverAlign::kCenterIfWider;
      placement.gap_dip = 4;
    }
    SIZE probe = {1, 1};
    if (!CreatePopover(spec, owner, probe, placement, options)) {
      return false;
    }
    FocusSelectedPreset();
    BuildLayout();
    SIZE size = {Scaled(kPanelWidth), Scaled(kPanelHeight)};
    Reposition(placement, size);
    Show();
    return true;
  }

 protected:
  PhiPalette QueryPalette() const override {
    return controller_ ? controller_->CurrentPalette()
                       : PhiResolvePalette(PhiThemeById("pure"),
                                           PhiAppearance::kSystem);
  }

  void OnDpiChanged(UINT) override {
    BuildLayout();
    ::SetWindowPos(hwnd(), nullptr, 0, 0, Scaled(kPanelWidth),
                   Scaled(kPanelHeight),
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }

  void OnHover(int now, int) override {
    // Which swatch slot the pointer is over (the swatch or its badge), and
    // since when: the badge shows at once and arms after a moment.
    int slot = -1;
    if (now >= 0 && now < static_cast<int>(hits().size())) {
      const int key = hits()[static_cast<size_t>(now)].key;
      if (key >= kPresetKey && key < kPresetKey + kSwatchesPerPage) {
        slot = key - kPresetKey;
      } else if (key >= kDeleteKey && key < kDeleteKey + kSwatchesPerPage) {
        slot = key - kDeleteKey;
      }
    }
    if (slot != hovered_slot_) {
      hovered_slot_ = slot;
      hovered_since_ = ::GetTickCount64();
    }
    Invalidate();
  }

  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) override {
    // A drag owns the pointer: no hover churn while a handle is held.
    if (message == WM_MOUSEMOVE && dragging_) {
      *handled = true;
      Drag({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
      return 0;
    }
    if (message == WM_LBUTTONUP && dragging_) {
      *handled = true;
      EndDrag({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
      return 0;
    }
    return PhiPopover::OnMessage(message, wparam, lparam, handled);
  }

  void OnMouseDown(int hit, POINT client_pt) override {
    if (hit < 0) {
      return;
    }
    const int key = hits()[static_cast<size_t>(hit)].key;
    if (key == kFieldKey) {
      BeginDrag(client_pt);
      return;
    }
    if (key == kBrightnessKey) {
      BeginSliderDrag(client_pt);
      return;
    }
    if (key == kLightKey || key == kDarkKey) {
      // The app-wide appearance, as the Mac's ThemeManager choice.
      if (ChromePrefsHost* prefs =
              controller_ ? controller_->prefs_host() : nullptr) {
        prefs->RequestAppearance(key == kDarkKey ? PhiAppearance::kDark
                                                 : PhiAppearance::kLight);
      }
      RebuildHits();
      Invalidate();
      return;
    }
    if (key >= kHarmonyKey && key < kHarmonyKey + 4) {
      const PhiThemePalette before = Current();
      Edit(before, before.SettingHarmony(
                       kHarmonies[static_cast<size_t>(key - kHarmonyKey)]));
      return;
    }
    if (key == kPreviousKey || key == kNextKey) {
      // No wrap: the strip stops at its first and last page.
      preset_page_ = std::clamp(preset_page_ + (key == kNextKey ? 1 : -1), 0,
                                PageCount() - 1);
      RebuildHits();
      Invalidate();
      return;
    }
    if (key == kResetKey && unedited_) {
      const PhiThemePalette original = *unedited_;
      unedited_.reset();
      Select(original);
      return;
    }
    if (key == kSaveKey && unedited_) {
      SavePalette();
      return;
    }
    if (key >= kPresetKey && key < kPresetKey + kSwatchesPerPage) {
      const std::vector<PhiThemePalette> presets = Presets();
      const size_t index = static_cast<size_t>(
          preset_page_ * kSwatchesPerPage + (key - kPresetKey));
      if (index < presets.size()) {
        unedited_.reset();
        Select(presets[index]);
      }
      return;
    }
    if (key >= kDeleteKey && key < kDeleteKey + kSwatchesPerPage) {
      const int slot = key - kDeleteKey;
      if (slot == hovered_slot_ &&
          ::GetTickCount64() - hovered_since_ >= kDeleteArmMs) {
        RemoveSavedPalette(preset_page_ * kSwatchesPerPage + slot);
      }
    }
  }

  bool OnKey(WPARAM virtual_key) override {
    // The focused handle's arrow keys: one degree of hue either way.
    if (virtual_key == VK_LEFT || virtual_key == VK_RIGHT) {
      const PhiThemePalette before = Current();
      Edit(before, before.RotatingHue(virtual_key == VK_LEFT ? -1.0 / 360
                                                             : 1.0 / 360));
      return true;
    }
    // Up and Down: the brightness slider.
    if (virtual_key == VK_UP || virtual_key == VK_DOWN) {
      const PhiThemePalette before = Current();
      Edit(before,
           before.SettingBrightness(std::clamp(
               before.brightness +
                   (virtual_key == VK_UP ? kSliderKeyStep : -kSliderKeyStep),
               0.0, 1.0)));
      return true;
    }
    return PhiPopover::OnKey(virtual_key);
  }

  // The pointer is let go by the system (Alt+Tab, another window's
  // SetCapture): the edit so far is kept, as the Mac's onDisappear keeps
  // a drag's palette, and the drag ends.
  void OnCaptureLost() override {
    if (!dragging_) {
      return;
    }
    const bool moved = moved_;
    const PhiThemePalette palette = drag_palette_;
    dragging_ = false;
    capture_.Release();  // A no-op once the system took it; re-entry is guarded.
    if (moved) {
      Edit(press_palette_, palette);
    }
    if (dismiss_pending_) {
      dismiss_pending_ = false;
      PhiPopover::OnDismiss();
    }
  }

  // Focus can leave mid-drag (the shell repaint behind steals activation);
  // the dismissal is deferred to the end of the drag instead of dropped.
  void OnDismiss() override {
    if (dragging_) {
      dismiss_pending_ = true;
      return;
    }
    PhiPopover::OnDismiss();
  }

  void OnTimerFired(UINT_PTR id) override {
    if (id != animation_.id()) {
      return;
    }
    if (::GetTickCount64() - animation_start_ >= kClickAnimationMs) {
      animation_.Stop();
      animating_ = false;
    }
    Invalidate();
  }

  void OnNcDestroy() override {
    // The form's "Color ›" row toggles: a click that closed this must not
    // reopen it on the way down.
    RecordClose(owner());
    if (on_closed_) {
      std::function<void()> callback = std::move(on_closed_);
      on_closed_ = std::function<void()>();
      callback();
    }
  }

  void Paint(HDC dc, const RECT&) override {
    RECT rc = {};
    ::GetClientRect(hwnd(), &rc);
    // The Mac's popover material: the flyout ground, which does not move
    // with the Space tint the editor is changing.
    const PhiPalette chrome = QueryPalette();
    const bool dark = chrome.dark;
    const COLORREF ground = chrome.flyout;
    HBRUSH brush = ::CreateSolidBrush(ground);
    ::FillRect(dc, &rc, brush);
    ::DeleteObject(brush);

    const PhiThemePalette shown = Displayed();
    PaintField(dc, shown, dark, ground);
    PaintSlider(dc, shown, dark, ground);
    PaintStrip(dc, chrome);
    PaintToolbar(dc, shown, chrome);
  }

 private:
  // --- The model --------------------------------------------------------------

  // The record being edited: the drag's palette while one runs, else the
  // draft or the host's record, else the Mac's seed from the Space's
  // resolved overlay colour at full brightness.
  PhiThemePalette Current() const {
    if (dragging_) {
      return drag_palette_;
    }
    if (draft_) {
      return editor_.palette ? *editor_.palette : Seed();
    }
    SpacesHost* spaces = controller_ ? controller_->spaces_host() : nullptr;
    if (spaces) {
      if (std::optional<PhiThemePalette> stored = spaces->SpacePalette(space_id_)) {
        return *stored;
      }
    }
    return Seed();
  }

  PhiThemePalette Seed() const {
    const bool dark = QueryPalette().dark;
    const PhiTheme& theme = PhiThemeById(BaseThemeId());
    return PhiThemePalette::FromColor(dark ? theme.dark.overlay
                                           : theme.light.overlay,
                                     dark)
        .SettingBrightness(1);
  }

  // The Mac's resolvedThemeId: the Space's pin, else the app-wide theme.
  std::string BaseThemeId() const {
    if (draft_) {
      return editor_.base_theme_id;
    }
    SpacesHost* spaces = controller_ ? controller_->spaces_host() : nullptr;
    ChromePrefsHost* prefs = controller_ ? controller_->prefs_host() : nullptr;
    if (!spaces || !prefs) {
      return std::string("pure");
    }
    const std::string pinned = spaces->SpaceThemeId(space_id_);
    return pinned.empty() ? prefs->ThemeId() : pinned;
  }

  // What the handles show: mid-animation, the eased blend from the click's
  // start towards the committed record.
  PhiThemePalette Displayed() const {
    const PhiThemePalette target = Current();
    if (!animating_) {
      return target;
    }
    const double t = EaseOut(
        static_cast<double>(::GetTickCount64() - animation_start_) /
        kClickAnimationMs);
    return PhiThemePalette::Make(
        LerpHue(animation_from_.hue, target.hue, t),
        animation_from_.saturation +
            (target.saturation - animation_from_.saturation) * t,
        target.brightness, target.harmony);
  }

  // A persisted edit: the host's record (one write, fanned out to every
  // window of the Space), or the draft's binding.
  void Commit(const PhiThemePalette& palette) {
    if (draft_) {
      editor_.palette = palette;
      if (editor_.on_change) {
        editor_.on_change(palette);
      }
    } else if (SpacesHost* spaces =
                   controller_ ? controller_->spaces_host() : nullptr) {
      spaces->SetSpacePalette(space_id_, palette);
    }
    RebuildHits();
    Invalidate();
  }

  // A pointer move: the windows follow without a write.
  void Preview(const PhiThemePalette& palette) {
    if (draft_) {
      editor_.palette = palette;
      if (editor_.on_change) {
        editor_.on_change(palette);
      }
    } else if (SpacesHost* spaces =
                   controller_ ? controller_->spaces_host() : nullptr) {
      spaces->PreviewSpacePalette(space_id_, palette);
    }
    Invalidate();
  }

  // A manual edit (a handle, a harmony, an arrow key): the first one
  // remembers the palette it started from, for Reset and Save; coming back
  // to that palette forgets it again.
  void Edit(const PhiThemePalette& before, const PhiThemePalette& after) {
    if (after != before && !unedited_) {
      unedited_ = before;
    }
    if (unedited_ && after == *unedited_) {
      unedited_.reset();
    }
    Commit(after);
  }

  // A swatch or Reset: the handles ease over to the palette.
  void Select(const PhiThemePalette& palette) {
    const PhiThemePalette from = Current();
    Commit(palette);
    if (palette != from) {
      animation_from_ = from;
      animation_start_ = ::GetTickCount64();
      animating_ = true;
      if (!animation_.valid()) {
        animation_ = NewTimer();
      }
      animation_.Start(kAnimationTickMs, /*coalescable=*/true);
    }
  }

  std::vector<PhiThemePalette> SavedPalettes() const {
    SpacesHost* spaces = controller_ ? controller_->spaces_host() : nullptr;
    return spaces ? spaces->SavedThemePalettes()
                  : std::vector<PhiThemePalette>();
  }

  // The recommendations, then the saved colours after their three pages.
  std::vector<PhiThemePalette> Presets() const {
    std::vector<PhiThemePalette> presets;
    for (const Recommended& r : kRecommended) {
      presets.push_back(
          PhiThemePalette::Make(r.hue, r.saturation, r.brightness, r.harmony));
    }
    const std::vector<PhiThemePalette> saved = SavedPalettes();
    presets.insert(presets.end(), saved.begin(), saved.end());
    return presets;
  }

  int PageCount() const {
    const int count =
        kRecommendedCount + static_cast<int>(SavedPalettes().size());
    return (count + kSwatchesPerPage - 1) / kSwatchesPerPage;
  }

  // The page showing the current palette, when a swatch holds it.
  void FocusSelectedPreset() {
    const std::vector<PhiThemePalette> presets = Presets();
    const PhiThemePalette current = Current();
    for (size_t i = 0; i < presets.size(); ++i) {
      if (presets[i] == current) {
        preset_page_ = static_cast<int>(i) / kSwatchesPerPage;
        return;
      }
    }
  }

  void SavePalette() {
    SpacesHost* spaces = controller_ ? controller_->spaces_host() : nullptr;
    if (!spaces) {
      return;
    }
    std::vector<PhiThemePalette> saved = spaces->SavedThemePalettes();
    saved.push_back(Current());
    spaces->SetSavedThemePalettes(saved);
    unedited_.reset();
    preset_page_ = (kRecommendedCount + static_cast<int>(saved.size()) - 1) /
                   kSwatchesPerPage;
    hovered_slot_ = -1;
    RebuildHits();
    Invalidate();
  }

  void RemoveSavedPalette(int index) {
    SpacesHost* spaces = controller_ ? controller_->spaces_host() : nullptr;
    const int saved_index = index - kRecommendedCount;
    if (!spaces || saved_index < 0) {
      return;
    }
    std::vector<PhiThemePalette> saved = spaces->SavedThemePalettes();
    if (saved_index >= static_cast<int>(saved.size())) {
      return;
    }
    saved.erase(saved.begin() + saved_index);
    spaces->SetSavedThemePalettes(saved);
    preset_page_ = std::min(preset_page_, PageCount() - 1);
    hovered_slot_ = -1;
    RebuildHits();
    Invalidate();
  }

  // --- Geometry -----------------------------------------------------------------

  RECT FieldRect() const {
    const int pad = Scaled(kPadding);
    const int side = Scaled(kField);
    return {pad, pad, pad + side, pad + side};
  }

  POINT HandleCenter(const PhiThemePalette& palette, int index) const {
    const RECT field = FieldRect();
    const PhiThemePalette::Point p = palette.HandlePosition(index);
    return {field.left + static_cast<int>(std::lround(p.x * (field.right - field.left))),
            field.top + static_cast<int>(std::lround(p.y * (field.bottom - field.top)))};
  }

  PhiThemePalette::Point UnitPoint(POINT pt) const {
    const RECT field = FieldRect();
    return {static_cast<double>(pt.x - field.left) / (field.right - field.left),
            static_cast<double>(pt.y - field.top) / (field.bottom - field.top)};
  }

  // The toolbar's two capsule groups: appearance at the left, harmony at
  // the right; each button a 22 circle inside the group's 1 padding.
  // The preset strip: a paging button at each end, seven 24 slots spread
  // evenly between them.
  RECT SliderRect() const {
    const int top = Scaled(kPadding) + Scaled(kField) + Scaled(kSliderGap);
    return {Scaled(kPadding), top, Scaled(kPanelWidth) - Scaled(kPadding),
            top + Scaled(kSliderHeight)};
  }

  // The thumb's centre travels the slider inset by its own radius.
  double SliderValue(POINT pt) const {
    const RECT slider = SliderRect();
    const double radius = Scaled(kSliderThumb) / 2.0;
    const double span = (slider.right - slider.left) - 2 * radius;
    const double value =
        std::clamp((pt.x - slider.left - radius) / span, 0.0, 1.0);
    return std::round(value / kSliderStep) * kSliderStep;
  }

  RECT StripRect() const {
    const int top = SliderRect().bottom + Scaled(kStripGap);
    return {Scaled(kPadding), top, Scaled(kPanelWidth) - Scaled(kPadding),
            top + Scaled(kStripHeight)};
  }

  RECT StripButton(bool next) const {
    const RECT strip = StripRect();
    const int side = Scaled(kStripButton);
    return next ? RECT{strip.right - side, strip.top, strip.right, strip.bottom}
                : RECT{strip.left, strip.top, strip.left + side, strip.bottom};
  }

  RECT SwatchSlot(int slot) const {
    const RECT strip = StripRect();
    const int side = Scaled(kStripButton);
    const int left = strip.left + side;
    const int room = strip.right - side - left;
    const int centre = left + room * (2 * slot + 1) / (2 * kSwatchesPerPage);
    return {centre - side / 2, strip.top, centre - side / 2 + side,
            strip.bottom};
  }

  // The delete badge at a saved swatch's lower trailing corner.
  RECT DeleteBadge(int slot) const {
    const RECT cell = SwatchSlot(slot);
    const int badge = Scaled(kDeleteBadge);
    return {cell.right - badge, cell.bottom - badge, cell.right, cell.bottom};
  }

  RECT ToolbarRect() const {
    const int top = StripRect().bottom + Scaled(kToolbarGap);
    return {Scaled(kPadding), top, Scaled(kPanelWidth) - Scaled(kPadding),
            top + Scaled(kToolbarHeight)};
  }

  RECT AppearanceGroup() const {
    const RECT bar = ToolbarRect();
    return {bar.left, bar.top,
            bar.left + 2 * Scaled(kToolButton) + 2 * Scaled(kToolPad),
            bar.bottom};
  }

  // After the appearance group and a fixed gap, which leaves the trailing
  // end free for Reset and Save.
  RECT HarmonyGroup() const {
    const RECT bar = ToolbarRect();
    const int left = AppearanceGroup().right + Scaled(29);
    return {left, bar.top,
            left + 4 * Scaled(kToolButton) + 2 * Scaled(kToolPad), bar.bottom};
  }

  // Reset then Save at the toolbar's trailing end, 2 apart.
  RECT EditButton(bool save) const {
    const RECT bar = ToolbarRect();
    const int side = Scaled(kStripButton);
    const int right = save ? bar.right : bar.right - side - Scaled(2);
    return {right - side, bar.top, right, bar.bottom};
  }

  RECT ToolButton(const RECT& group, int index) const {
    const int button = Scaled(kToolButton);
    const int left = group.left + Scaled(kToolPad) + index * button;
    const int top = (group.top + group.bottom) / 2 - button / 2;
    return {left, top, left + button, top + button};
  }

  // THE layout pass: the field, then the hits over it.
  void BuildLayout() { RebuildHits(); }

  void RebuildHits() {
    ClearHits();
    // The field is ONE hit: the handles are found from the press point
    // (BeginDrag), as on the Mac. Per-handle hits would name them for a
    // screen reader but also raise the base's tooltip ("Color 1") under
    // the pointer on every click, which the Mac's handles never do.
    AddHit(FieldRect(), kFieldKey, std::wstring(), PhiHitRole::kGroup);
    AddHit(SliderRect(), kBrightnessKey,
           std::wstring(PhiStr("theme.editor.brightness")),
           PhiHitRole::kSlider);
    const PhiThemePalette palette = Current();
    const bool dark = QueryPalette().dark;
    const RECT appearance = AppearanceGroup();
    AddHit(ToolButton(appearance, 0), kLightKey,
           std::wstring(PhiStr("themes.appearance.lightOption")),
           PhiHitRole::kToggle, /*selected=*/!dark);
    AddHit(ToolButton(appearance, 1), kDarkKey,
           std::wstring(PhiStr("themes.appearance.darkOption")),
           PhiHitRole::kToggle, /*selected=*/dark);
    const RECT harmony = HarmonyGroup();
    for (size_t i = 0; i < kHarmonies.size(); ++i) {
      AddHit(ToolButton(harmony, static_cast<int>(i)),
             kHarmonyKey + static_cast<int>(i),
             std::wstring(PhiStr(HarmonyLabelKey(kHarmonies[i]))),
             PhiHitRole::kToggle, palette.harmony == kHarmonies[i]);
    }
    if (unedited_) {
      AddHit(EditButton(false), kResetKey,
             std::wstring(PhiStr("theme.editor.reset")), PhiHitRole::kButton);
      AddHit(EditButton(true), kSaveKey,
             std::wstring(PhiStr("theme.editor.save")), PhiHitRole::kButton);
    }
    const int pages = PageCount();
    preset_page_ = std::clamp(preset_page_, 0, pages - 1);
    if (preset_page_ > 0) {
      AddHit(StripButton(false), kPreviousKey,
             std::wstring(PhiStr("theme.editor.previousColors")),
             PhiHitRole::kButton);
    }
    if (preset_page_ + 1 < pages) {
      AddHit(StripButton(true), kNextKey,
             std::wstring(PhiStr("theme.editor.nextColors")),
             PhiHitRole::kButton);
    }
    const std::vector<PhiThemePalette> presets = Presets();
    for (int slot = 0; slot < kSwatchesPerPage; ++slot) {
      const int index = preset_page_ * kSwatchesPerPage + slot;
      if (index >= static_cast<int>(presets.size())) {
        break;
      }
      AddHit(SwatchSlot(slot), kPresetKey + slot,
             PhiStrF("theme.editor.presetColor", {std::to_wstring(index + 1)}),
             PhiHitRole::kToggle,
             presets[static_cast<size_t>(index)] == palette);
      // The badge after its swatch: the later hit is the one on top, and
      // the badge sits over the swatch's corner.
      if (index >= kRecommendedCount) {
        AddHit(DeleteBadge(slot), kDeleteKey + slot,
               std::wstring(PhiStr("theme.editor.deleteColor")),
               PhiHitRole::kButton);
      }
    }
  }

  // --- The drag -----------------------------------------------------------------

  void BeginDrag(POINT pt) {
    animating_ = false;
    animation_.Stop();
    press_palette_ = Current();
    // The handle under the press (nearest within the grab radius), else
    // the primary — which a click moves to the point.
    int index = 0;
    double best = Scaled(kHandleGrab) + 0.5;
    for (int i = 0; i < press_palette_.color_count(); ++i) {
      const POINT c = HandleCenter(press_palette_, i);
      const double distance = std::hypot(c.x - pt.x, c.y - pt.y);
      if (distance <= best) {
        best = distance;
        index = i;
      }
    }
    drag_index_ = index;
    drag_palette_ = press_palette_;
    press_pt_ = pt;
    moved_ = false;
    drag_slider_ = false;
    dragging_ = true;
    capture_ = BeginCapture();
  }

  // A press on the brightness slider: the thumb jumps to the pointer and
  // follows it, previewing like a handle drag and persisting on release.
  void BeginSliderDrag(POINT pt) {
    animating_ = false;
    animation_.Stop();
    press_palette_ = Current();
    drag_palette_ = press_palette_;
    press_pt_ = pt;
    moved_ = true;  // Never a click: the press itself sets the value.
    drag_slider_ = true;
    dragging_ = true;
    capture_ = BeginCapture();
    Drag(pt);
  }

  void Drag(POINT pt) {
    if (!moved_ &&
        std::hypot(pt.x - press_pt_.x, pt.y - press_pt_.y) < Scaled(kClickSlop)) {
      return;
    }
    moved_ = true;
    const PhiThemePalette next =
        drag_slider_
            ? press_palette_.SettingBrightness(SliderValue(pt))
            : press_palette_.MovingHandle(drag_index_, UnitPoint(pt),
                                          /*snap_to_center=*/true);
    if (next == drag_palette_) {
      return;
    }
    drag_palette_ = next;
    Preview(next);
  }

  void EndDrag(POINT pt) {
    if (!dragging_) {
      return;
    }
    const bool click = !moved_;
    const PhiThemePalette from = press_palette_;
    PhiThemePalette result = drag_palette_;
    if (click) {
      // A click always moves the PRIMARY handle, with an eased transition.
      result = press_palette_.MovingHandle(0, UnitPoint(pt),
                                           /*snap_to_center=*/true);
    }
    dragging_ = false;
    capture_.Release();
    Edit(from, result);
    if (click && result != from) {
      animation_from_ = from;
      animation_start_ = ::GetTickCount64();
      animating_ = true;
      if (!animation_.valid()) {
        animation_ = NewTimer();
      }
      animation_.Start(kAnimationTickMs, /*coalescable=*/true);
    }
    if (dismiss_pending_ || ::GetActiveWindow() != hwnd()) {
      dismiss_pending_ = false;
      PhiPopover::OnDismiss();
    }
  }

  // --- Paint --------------------------------------------------------------------

  const std::vector<uint8_t>& FieldBitmap(double brightness, bool dark,
                                          int size) {
    // For the process, not the panel: a field costs a visible beat at a high
    // scale (31 ms at 366 px on a 2018 laptop, the Mac's a6e3a27a), and the
    // editor reopens at the same size.
    static base::NoDestructor<std::vector<Field>> cache;
    std::vector<Field>& fields_ = *cache;
    for (const Field& field : fields_) {
      if (field.dark == dark && field.size == size &&
          std::fabs(field.brightness - brightness) < 0.0005) {
        return field.bgra;
      }
    }
    if (fields_.size() >= 4) {
      fields_.erase(fields_.begin());
    }
    fields_.push_back({dark, brightness, size, RenderField(brightness, dark, size)});
    return fields_.back().bgra;
  }

  void PaintField(HDC dc, const PhiThemePalette& palette, bool dark,
                  COLORREF ground) {
    const RECT field = FieldRect();
    const int size = field.right - field.left;
    const std::vector<uint8_t>& bgra =
        FieldBitmap(palette.brightness, dark, size);
    PhiDrawBgra(dc, field, bgra.data(), size, size);
    // The field's continuous corner mask: bite the corners back to the
    // ground rather than clipping the bitmap to a stair-stepped region.
    PhiKnockOutRoundRectAA(dc, field, Scaled(kFieldRadius), ground);
    // The handles, the primary first so the smaller linked ones sit on top,
    // each the exact resolved colour under a 2-pt white rim.
    for (int i = 0; i < palette.color_count(); ++i) {
      const POINT c = HandleCenter(palette, i);
      const double radius = Scaled(kHandleDiameter[static_cast<size_t>(i)]) / 2.0;
      const RECT dot = CircleRect(c.x, c.y, radius);
      PhiFillEllipseAA(dc, dot, palette.ResolvedColor(i, dark));
      PhiStrokeEllipseAA(dc, dot, RGB(0xFF, 0xFF, 0xFF), Scaled(kHandleRim));
    }
  }

  // A button's wash: selected ink 6 % (dark: white 20 %), hovered 4 %.
  // The brightness slider: a track running from the primary colour at
  // brightness 0 to the same colour at 1, in the appearance being edited,
  // under a thumb of the exact resolved colour.
  void PaintSlider(HDC dc, const PhiThemePalette& palette, bool dark,
                   COLORREF ground) {
    const RECT slider = SliderRect();
    const int mid = (slider.top + slider.bottom) / 2;
    const int track_h = Scaled(kSliderTrack);
    const RECT track = {slider.left, mid - track_h / 2, slider.right,
                        mid - track_h / 2 + track_h};
    const COLORREF low = palette.SettingBrightness(0).ResolvedColor(0, dark);
    const COLORREF high = palette.SettingBrightness(1).ResolvedColor(0, dark);
    const int width = track.right - track.left;
    for (int x = 0; x < width; ++x) {
      const RECT column = {track.left + x, track.top, track.left + x + 1,
                           track.bottom};
      HBRUSH brush =
          ::CreateSolidBrush(PhiBlend(high, low, 255 * x / (width - 1)));
      ::FillRect(dc, &column, brush);
      ::DeleteObject(brush);
    }
    PhiKnockOutRoundRectAA(dc, track, track_h / 2, ground);
    const double radius = Scaled(kSliderThumb) / 2.0;
    const double cx = slider.left + radius +
                      palette.brightness * (width - 2 * radius);
    const RECT thumb = CircleRect(cx, mid, radius);
    PhiFillEllipseAA(dc, thumb, palette.ResolvedColor(0, dark));
    PhiStrokeEllipseAA(dc, thumb, RGB(0xFF, 0xFF, 0xFF), Scaled(kHandleRim));
  }

  COLORREF ButtonWash(const PhiPalette& chrome, bool selected, bool hovered) const {
    if (selected) {
      return PhiBlend(chrome.ink, chrome.flyout, chrome.dark ? 51 : 15);
    }
    return hovered ? PhiBlend(chrome.ink, chrome.flyout, 10) : chrome.flyout;
  }

  bool Hovered(int key) const {
    const int index = hovered();
    return index >= 0 && index < static_cast<int>(hits().size()) &&
           hits()[static_cast<size_t>(index)].key == key;
  }

  void PaintStrip(HDC dc, const PhiPalette& chrome) {
    const bool dark = chrome.dark;
    const int pages = PageCount();
    // The paging chevrons, dimmed at the end they cannot pass.
    const auto chevron = [&](bool next) {
      const bool live = next ? preset_page_ + 1 < pages : preset_page_ > 0;
      const RECT button = StripButton(next);
      const int key = next ? kNextKey : kPreviousKey;
      if (live && Hovered(key)) {
        PhiFillEllipseAA(dc, button, ButtonWash(chrome, false, true));
      }
      PhiDrawIconGlyph(dc, button,
                       next ? fluent::kChevronRight : fluent::kChevronLeft,
                       Scaled(10),
                       PhiBlend(chrome.ink, chrome.flyout, live ? 255 : 64));
    };
    chevron(false);
    chevron(true);
    const std::vector<PhiThemePalette> presets = Presets();
    const PhiThemePalette current = Current();
    for (int slot = 0; slot < kSwatchesPerPage; ++slot) {
      const int index = preset_page_ * kSwatchesPerPage + slot;
      if (index >= static_cast<int>(presets.size())) {
        break;
      }
      const PhiThemePalette& preset = presets[static_cast<size_t>(index)];
      const RECT cell = SwatchSlot(slot);
      const int disc = Scaled(kSwatch);
      const int cx = (cell.left + cell.right) / 2;
      const int cy = (cell.top + cell.bottom) / 2;
      const RECT swatch = {cx - disc / 2, cy - disc / 2, cx - disc / 2 + disc,
                           cy - disc / 2 + disc};
      // The palette as the sidebar would paint it, top to bottom, cut to a
      // disc.
      std::array<PhiColor, 3> colors = {};
      const int count = std::clamp(preset.color_count(), 1, 3);
      for (int i = 0; i < 3; ++i) {
        colors[static_cast<size_t>(i)] =
            preset.ResolvedColor(std::min(i, count - 1), dark);
      }
      for (int y = swatch.top; y < swatch.bottom; ++y) {
        const double position =
            disc > 1 ? static_cast<double>(y - swatch.top) / (disc - 1) : 0;
        const RECT row = {swatch.left, y, swatch.right, y + 1};
        HBRUSH brush =
            ::CreateSolidBrush(PhiOklabGradient(colors, count, position));
        ::FillRect(dc, &row, brush);
        ::DeleteObject(brush);
      }
      PhiKnockOutRoundRectAA(dc, swatch, disc / 2, chrome.flyout);
      PhiStrokeEllipseAA(dc, swatch,
                         PhiBlend(chrome.ink, chrome.flyout, dark ? 77 : 26),
                         1);
      if (preset == current) {
        PhiStrokeEllipseAA(dc, cell,
                           PhiBlend(chrome.ink, chrome.flyout, dark ? 77 : 46),
                           1);
      }
      if (index >= kRecommendedCount && slot == hovered_slot_) {
        // The Mac's theme-editor-remove: a small disc with a bar.
        const RECT badge = DeleteBadge(slot);
        PhiFillEllipseAA(dc, badge, dark ? RGB(0, 0, 0) : RGB(255, 255, 255));
        PhiStrokeEllipseAA(dc, badge, PhiBlend(chrome.ink, chrome.flyout, 26),
                           1);
        const int mid = (badge.top + badge.bottom) / 2;
        const int inset = (badge.right - badge.left) / 4;
        const RECT bar = {badge.left + inset, mid, badge.right - inset,
                          mid + std::max(1, Scaled(1))};
        HBRUSH brush = ::CreateSolidBrush(dark ? RGB(255, 255, 255)
                                               : RGB(0, 0, 0));
        ::FillRect(dc, &bar, brush);
        ::DeleteObject(brush);
      }
    }
  }

  void PaintToolbar(HDC dc, const PhiThemePalette& palette,
                    const PhiPalette& chrome) {
    const bool dark = chrome.dark;
    if (unedited_) {
      for (const bool is_save : {false, true}) {
        const RECT button = EditButton(is_save);
        if (Hovered(is_save ? kSaveKey : kResetKey)) {
          PhiFillEllipseAA(dc, button, ButtonWash(chrome, false, true));
        }
        PhiDrawIconGlyph(dc, button, is_save ? fluent::kSave : fluent::kUndo,
                         Scaled(13), chrome.ink);
      }
    }
    const COLORREF capsule =
        PhiBlend(chrome.ink, chrome.flyout, dark ? 51 : 15);
    const RECT appearance = AppearanceGroup();
    PhiStrokeCapsuleAA(dc, appearance, capsule, 1);
    for (int i = 0; i < 2; ++i) {
      const bool selected = (i == 1) == dark;
      const int key = i == 0 ? kLightKey : kDarkKey;
      const RECT button = ToolButton(appearance, i);
      const COLORREF wash = ButtonWash(chrome, selected, Hovered(key));
      if (wash != chrome.flyout) {
        PhiFillEllipseAA(dc, button, wash);
      }
      // Glyph ink: full when selected, 30 % otherwise.
      const COLORREF ink = PhiBlend(chrome.ink, wash, selected ? 255 : 77);
      if (i == 0) {
        PaintSun(dc, button, ink);
      } else {
        PaintMoon(dc, button, ink, wash);
      }
    }
    const RECT harmony = HarmonyGroup();
    PhiStrokeCapsuleAA(dc, harmony, capsule, 1);
    for (size_t i = 0; i < kHarmonies.size(); ++i) {
      const bool selected = palette.harmony == kHarmonies[i];
      const int key = kHarmonyKey + static_cast<int>(i);
      const RECT button = ToolButton(harmony, static_cast<int>(i));
      const COLORREF wash = ButtonWash(chrome, selected, Hovered(key));
      if (wash != chrome.flyout) {
        PhiFillEllipseAA(dc, button, wash);
      }
      PaintHarmonyGlyph(dc, button, kHarmonies[i], chrome.ink, wash, dark);
    }
  }

  // Device-pixel position of a 22-grid glyph point inside `button`.
  double Gx(const RECT& button, double x) const {
    return button.left + x * (button.right - button.left) / kToolButton;
  }
  double Gy(const RECT& button, double y) const {
    return button.top + y * (button.bottom - button.top) / kToolButton;
  }
  double Gs(const RECT& button, double v) const {
    return v * (button.right - button.left) / kToolButton;
  }

  // theme-editor-sun: a 3-radius disc with eight short rays at radius ~6.
  void PaintSun(HDC dc, const RECT& button, COLORREF ink) const {
    const double cx = Gx(button, 11), cy = Gy(button, 11);
    PhiFillEllipseAA(dc, CircleRect(cx, cy, Gs(button, 3)), ink);
    const double ray = std::max(1.0, Gs(button, 0.7));
    for (int i = 0; i < 8; ++i) {
      const double a = i * 3.14159265358979323846 / 4;
      PhiFillEllipseAA(dc,
                       CircleRect(cx + std::cos(a) * Gs(button, 6.1),
                                  cy + std::sin(a) * Gs(button, 6.1), ray),
                       ink);
    }
  }

  // theme-editor-moon: a 5.5-radius disc bitten by a circle up and right.
  void PaintMoon(HDC dc, const RECT& button, COLORREF ink,
                 COLORREF wash) const {
    PhiFillEllipseAA(dc, CircleRect(Gx(button, 11), Gy(button, 11),
                                    Gs(button, 5.5)),
                     ink);
    PhiFillEllipseAA(dc, CircleRect(Gx(button, 14.2), Gy(button, 7.9),
                                    Gs(button, 4.45)),
                     wash);
  }

  // theme-editor-one-ring / count-two / analogous / count-three: a faint
  // 7-radius ring (ink 10 % light, white 30 % dark), the linked colours
  // as 1.5-radius dots joined by hairlines of the ring's ink.
  void PaintHarmonyGlyph(HDC dc, const RECT& button,
                         PhiThemePalette::Harmony harmony, COLORREF ink,
                         COLORREF wash, bool dark) const {
    const COLORREF faint = PhiBlend(ink, wash, dark ? 77 : 26);
    PhiStrokeEllipseAA(dc, CircleRect(Gx(button, 11), Gy(button, 11),
                                      Gs(button, 7)),
                       faint, std::max(1, Scaled(1)));
    std::vector<std::pair<double, double>> dots;
    switch (harmony) {
      case PhiThemePalette::Harmony::kSingle:
        dots = {{11, 11}};
        break;
      case PhiThemePalette::Harmony::kAnalogous:
        dots = {{5.5, 6.5}, {16.5, 6.5}};
        break;
      case PhiThemePalette::Harmony::kComplementary:
        dots = {{11, 4}, {11, 18}};
        break;
      case PhiThemePalette::Harmony::kSplitComplementary:
        dots = {{11, 4}, {5.5, 15.5}, {16.5, 15.5}};
        break;
    }
    if (dots.size() > 1) {
      HPEN pen = ::CreatePen(PS_SOLID, std::max(1, Scaled(1)), faint);
      HGDIOBJ prev = ::SelectObject(dc, pen);
      for (size_t i = 0; i < dots.size(); ++i) {
        const auto& [ax, ay] = dots[i];
        const auto& [bx, by] = dots[(i + 1) % dots.size()];
        if (dots.size() == 2 && i == 1) {
          break;
        }
        ::MoveToEx(dc, static_cast<int>(std::lround(Gx(button, ax))),
                   static_cast<int>(std::lround(Gy(button, ay))), nullptr);
        ::LineTo(dc, static_cast<int>(std::lround(Gx(button, bx))),
                 static_cast<int>(std::lround(Gy(button, by))));
      }
      ::SelectObject(dc, prev);
      ::DeleteObject(pen);
    }
    for (const auto& [x, y] : dots) {
      PhiFillEllipseAA(dc, CircleRect(Gx(button, x), Gy(button, y),
                                      Gs(button, 1.5)),
                       ink);
    }
  }

  struct Field {
    bool dark = false;
    double brightness = 1;
    int size = 0;
    std::vector<uint8_t> bgra;
  };

  // The controller outlives the panel (it owns the shell that owns it).
  RAW_PTR_EXCLUSION PhiShellWindow* const controller_;
  const std::string space_id_;
  const bool draft_ = false;
  PhiThemeDraftEditor editor_;
  std::function<void()> on_closed_;
  // The drag.
  Capture capture_;
  bool dragging_ = false;
  bool moved_ = false;
  bool drag_slider_ = false;  // The drag is the brightness slider's.
  int drag_index_ = 0;
  POINT press_pt_ = {};
  PhiThemePalette press_palette_;
  PhiThemePalette drag_palette_;
  bool dismiss_pending_ = false;
  // The preset strip: the page showing, the palette a manual edit started
  // from (Reset and Save show while it is set), and the swatch slot under
  // the pointer with when it got there.
  int preset_page_ = 0;
  std::optional<PhiThemePalette> unedited_;
  int hovered_slot_ = -1;
  ULONGLONG hovered_since_ = 0;
  // A click's handle transition.
  Timer animation_;
  bool animating_ = false;
  ULONGLONG animation_start_ = 0;
  PhiThemePalette animation_from_;
};

}  // namespace

void PhiOpenSpaceThemePanel(PhiShellWindow* controller,
                            const RECT& anchor_screen,
                            const std::string& space_id,
                            std::function<void()> on_closed) {
  if (!controller || !controller->shell_hwnd() || space_id.empty() ||
      !controller->spaces_host()) {
    if (on_closed) {
      on_closed();
    }
    return;
  }
  Panel* panel = new Panel(controller, space_id, std::move(on_closed));
  panel->DeleteWhenDestroyed();
  if (!panel->Open(controller->shell_hwnd(), anchor_screen, /*beside=*/false)) {
    delete panel;
  }
}

bool PhiOpenSpaceThemePanelBeside(PhiShellWindow* controller,
                                  HWND owner,
                                  const RECT& anchor_screen,
                                  const std::string& space_id) {
  if (!controller || !owner || space_id.empty() ||
      !controller->spaces_host()) {
    return false;
  }
  Panel* panel = new Panel(controller, space_id, std::function<void()>());
  panel->DeleteWhenDestroyed();
  if (!panel->Open(owner, anchor_screen, /*beside=*/true)) {
    delete panel;
    return false;
  }
  return true;
}

void PhiShowSpaceThemePanel(PhiShellWindow* controller,
                            const RECT& anchor_screen,
                            const std::string& space_id) {
  bool done = false;
  PhiOpenSpaceThemePanel(controller, anchor_screen, space_id,
                         [&done] { done = true; });
  if (done) {
    return;  // The panel never opened.
  }
  // A pipe verb that tore the controller down mid-loop would leave the
  // panel's back-pointer dangling; the coordinator defers them while a
  // modal scope is open. Unlike the loop this replaced, nothing lives on
  // this frame but the flag.
  PhiModalScope modal;
  MSG msg;
  while (!done && ::GetMessageW(&msg, nullptr, 0, 0)) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }
}

PhiPopover* PhiOpenSpaceThemeDraftPanel(PhiShellWindow* controller,
                                        HWND owner,
                                        const RECT& anchor_screen,
                                        PhiThemeDraftEditor editor) {
  std::function<void()> on_closed = editor.on_closed;
  if (!owner) {
    if (on_closed) {
      on_closed();
    }
    return nullptr;
  }
  Panel* panel = new Panel(controller, std::move(editor));
  panel->DeleteWhenDestroyed();
  if (!panel->Open(owner, anchor_screen, /*beside=*/true)) {
    delete panel;
    if (on_closed) {
      on_closed();
    }
    return nullptr;
  }
  return panel;
}

}  // namespace phi_client

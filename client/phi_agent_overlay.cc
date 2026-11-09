// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_agent_overlay.h"

#include <windowsx.h>

// GDI+ draws the sprite layer (cursor + effects): antialiased paths with
// per-pixel alpha, which plain GDI cannot produce.
#include <objidl.h>

#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_animator.h"
#include "client/phi_dwrite.h"
#include "client/phi_icons.h"
#include "client/phi_fluent.h"
#include "client/phi_gdiplus.h"
#include "client/phi_panel.h"
#include "client/phi_shell_window.h"

namespace phi_client {

namespace {

constexpr wchar_t kFogClass[] = L"PhiAgentFog";
constexpr wchar_t kPillClass[] = L"PhiAgentPill";
// The sprite rides ABOVE the fog: a per-pixel-alpha layered child carrying
// the agent cursor and the transient input-mirror effects.
constexpr wchar_t kSpriteClass[] = L"PhiAgentSprite";
// The fog is a light wash, not a blackout: the user must still SEE what the
// agent is doing — that is the whole point of watch mode.
// §5.10 cue 4: a 5 % brand fog, a 2 dip ring inside the page pane, and
// the control capsule in the page's bottom-right corner — collapsed (36
// high) by default, the 44 dip pill when expanded.
constexpr BYTE kFogAlpha = 13;
constexpr int kRingDip = 2;
constexpr int kPillHeight = 44;
constexpr int kCapsuleHeight = 36;
constexpr int kPillMaxWidth = 440;
constexpr int kCornerInsetX = 24;  // Clear of the scrollbar.
constexpr int kCornerInsetY = 16;
constexpr int kButtonHeight = 28;
constexpr UINT_PTR kExpandTimer = 41;
constexpr UINT_PTR kCollapseTimer = 42;
constexpr int kExpandDelayMs = 300;
constexpr int kCollapseDelayMs = 6000;
// The minimise chevron at the pill's trailing edge (the Mac's 44861119):
// folds the caption and the buttons away and parks the pill, label-only,
// in the pane's bottom-right corner; a click on the parked pill unfolds it.
constexpr int kChevronWidth = 22;
void FillRounded(HDC dc, const RECT& r, COLORREF color, int radius) {
  PhiFillRoundRectAA(dc, r, radius, color);
}

// What the three windows need from the overlay that owns them. A plain
// interface rather than a back-pointer plus friendship: the house style
// bans `friend`, and the three surfaces are PhiPanels of their own now.
class OverlayHost {
 public:
  virtual ~OverlayHost() = default;
  virtual void PaintPill(HDC dc) = 0;
  virtual void PillClicked(int x, int y) = 0;
  virtual void PillHovered(bool inside) = 0;
  virtual void PillTimer(UINT_PTR id) = 0;
  virtual void ComposeSprite() = 0;
};

// The 2 dip brand ring inside the page pane (§5.10): four bars in a
// shaped layer, so the page between them stays live and clickable.
class Ring : public PhiPanel {
 public:
  Ring() = default;
  ~Ring() override { Destroy(); }

  bool Open(HWND shell) {
    ClassSpec spec;
    spec.name = L"PhiAgentRing";
    Options options;
    options.shaped_layer = true;
    options.click_through = true;
    options.ex_style = WS_EX_NOPARENTNOTIFY;
    const RECT nothing = {0, 0, 0, 0};
    return CreatePanel(spec, shell, nothing, options);
  }
  void set_ink(COLORREF ink) { ink_ = ink; }
  void set_thickness(int px) { thickness_ = px; }

 protected:
  void Paint(HDC dc, const RECT&) override {
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    HBRUSH brush = ::CreateSolidBrush(ink_);
    ::FillRect(dc, &client, brush);
    ::DeleteObject(brush);
  }
  void LayerShapes(int width, int height,
                   std::vector<PhiLayerShape>* out) const override {
    const int t = std::max(1, thickness_);
    PhiLayerShape top, bottom, left, right;
    top.bounds = {0, 0, width, t};
    bottom.bounds = {0, height - t, width, height};
    left.bounds = {0, 0, t, height};
    right.bounds = {width - t, 0, width, height};
    out->push_back(top);
    out->push_back(bottom);
    out->push_back(left);
    out->push_back(right);
  }

 private:
  COLORREF ink_ = RGB(0x1E, 0x9E, 0x4A);
  int thickness_ = 2;
};

// The FOG: a constant-alpha layered barrier over the whole content area. It
// keeps WS_EX_LAYERED with SetLayeredWindowAttributes (a flat wash, not
// per-pixel alpha), so it paints through the ordinary back-buffered path.
class Fog : public PhiPanel {
 public:
  Fog() = default;
  ~Fog() override { Destroy(); }

  bool Open(HWND shell) {
    ClassSpec spec;
    spec.name = kFogClass;
    spec.cursor = IDC_NO;
    Options options;
    options.ex_style = WS_EX_LAYERED | WS_EX_NOPARENTNOTIFY;
    const RECT nothing = {0, 0, 0, 0};
    return CreatePanel(spec, shell, nothing, options);
  }

 protected:
  void Paint(HDC dc, const RECT&) override {
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    // A flat wash; WS_EX_LAYERED turns it into the fog.
    HBRUSH brush = ::CreateSolidBrush(RGB(0x10, 0x12, 0x18));
    ::FillRect(dc, &client, brush);
    ::DeleteObject(brush);
  }

  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) override {
    switch (message) {
      // The barrier itself: content input dies here. MOUSEACTIVATE keeps the
      // click from even activating the browser child beneath.
      case WM_MOUSEACTIVATE:
        *handled = true;
        return MA_NOACTIVATEANDEAT;
      case WM_LBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_RBUTTONDOWN:
      case WM_RBUTTONUP:
      case WM_MBUTTONDOWN:
      case WM_MBUTTONUP:
      case WM_MOUSEWHEEL:
      case WM_MOUSEMOVE:
        *handled = true;
        return 0;
      case WM_SETCURSOR:
        ::SetCursor(::LoadCursorW(nullptr, IDC_NO));
        *handled = true;
        return TRUE;
      default:
        break;
    }
    return PhiPanel::OnMessage(message, wparam, lparam, handled);
  }
};

// The SPRITE: the agent cursor and the input-mirror effects, per-pixel
// alpha out of GDI+ and committed through UpdateLayeredWindow.
class Sprite : public PhiPanel {
 public:
  explicit Sprite(OverlayHost* host) : host_(host) {}
  ~Sprite() override { Destroy(); }

  bool Open(HWND shell) {
    ClassSpec spec;
    spec.name = kSpriteClass;
    spec.cursor = nullptr;
    Options options;
    options.layered = true;
    // The sprite is pure output — hit tests fall through to the fog
    // barrier beneath it.
    options.click_through = true;
    options.ex_style = WS_EX_NOPARENTNOTIFY;
    const RECT nothing = {0, 0, 0, 0};
    return CreatePanel(spec, shell, nothing, options);
  }

 protected:
  void ComposeLayer() override { host_->ComposeSprite(); }

 private:
  RAW_PTR_EXCLUSION OverlayHost* const host_;
};

// The PILL: a SHAPED LAYER, not a window region.
//
// Lane 3 §3.6 finding 2: this window was cut to a capsule with
// SetWindowRgn although it hosts no child controls (its two buttons are
// RECTs painted by hand), so every one of its four corners was a
// staircase. The silhouette is now per-pixel alpha out of phi_dwrite, and
// WM_NCHITTEST answers HTTRANSPARENT off the capsule — which is what the
// region said by simply not being there.
class Pill : public PhiPanel {
 public:
  explicit Pill(OverlayHost* host) : host_(host) {}
  ~Pill() override { Destroy(); }

  bool Open(HWND shell) {
    ClassSpec spec;
    spec.name = kPillClass;
    Options options;
    options.shaped_layer = true;
    options.ex_style = WS_EX_NOPARENTNOTIFY;
    const RECT nothing = {0, 0, 0, 0};
    return CreatePanel(spec, shell, nothing, options);
  }

 protected:
  void Paint(HDC dc, const RECT&) override { host_->PaintPill(dc); }

  void LayerShapes(int width,
                   int height,
                   std::vector<PhiLayerShape>* out) const override {
    PhiLayerShape capsule;
    capsule.bounds = {0, 0, width, height};
    capsule.radius = height / 2;  // A capsule, as the region was.
    out->push_back(capsule);
  }

  bool HitTestShape() const override { return true; }

  void OnClick(int, POINT pt) override {
    host_->PillClicked(static_cast<int>(pt.x), static_cast<int>(pt.y));
  }

  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) override {
    switch (message) {
      case WM_MOUSEACTIVATE:
        *handled = true;
        return MA_NOACTIVATE;  // Clickable without stealing page focus.
      case WM_MOUSEMOVE:
        if (!tracking_) {
          TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd(), 0};
          tracking_ = ::TrackMouseEvent(&track) != FALSE;
          host_->PillHovered(true);
        }
        break;
      case WM_MOUSELEAVE:
        tracking_ = false;
        host_->PillHovered(false);
        break;
      case WM_TIMER:
        host_->PillTimer(static_cast<UINT_PTR>(wparam));
        *handled = true;
        return 0;
      default:
        break;
    }
    return PhiPanel::OnMessage(message, wparam, lparam, handled);
  }

 private:
  RAW_PTR_EXCLUSION OverlayHost* const host_;
  bool tracking_ = false;
};

class Overlay : public OverlayHost {
 public:
  static Overlay* instance;

  Overlay(PhiShellWindow* controller, const std::string& task_id)
      : controller_(controller),
        shell_(controller->shell_hwnd()),
        task_id_(task_id) {
    dpi_ = ::GetDpiForWindow(shell_);
    fog_.Open(shell_);
    ring_.Open(shell_);
    sprite_.Open(shell_);
    pill_.Open(shell_);
    EnsureFonts();
  }

  ~Overlay() override {
    for (HFONT font : {label_font_, caption_font_}) {
      if (font) {
        ::DeleteObject(font);
      }
    }
    // Each destructor destroys its own HWND; the base clears
    // GWLP_USERDATA at WM_NCDESTROY, so nothing delivered during the
    // teardown reaches a half-destroyed object.
  }

  const std::string& task_id() const { return task_id_; }
  PhiShellWindow* controller() const { return controller_; }

  void Update(const std::wstring& agent_label,
              const std::wstring& caption,
              bool agent_owns,
              const std::wstring& mark,
              const std::wstring& status,
              bool waiting) {
    agent_label_ = agent_label;
    caption_ = caption;
    mark_ = mark;
    status_ = status;
    const bool owner_changed = agent_owns_ != agent_owns;
    agent_owns_ = agent_owns;
    if (waiting && !waiting_) {
      // "Waiting for you": the capsule opens by itself and stays open.
      collapsed_ = false;
      ::KillTimer(pill_.hwnd(), kCollapseTimer);
    }
    waiting_ = waiting;
    Reposition();
    if (owner_changed) {
      ComposeSprite();
    }
    if (agent_owns_) {
      fog_.ShowPanel();
      ::SetLayeredWindowAttributes(fog_.hwnd(), 0, kFogAlpha, LWA_ALPHA);
      ring_.ShowPanel();
    } else {
      fog_.HidePanel();
      ring_.HidePanel();
    }
    pill_.ShowPanel();
    pill_.Invalidate();
  }

  void Reposition() {
    // Reached from EVERY shell's LayoutContents. The HWND is tested rather
    // than the controller: an HWND value is safe to ask about after its
    // window died, a freed controller is not.
    if (!controller_ || !shell_ || !::IsWindow(shell_)) {
      return;
    }
    const RECT content = controller_->ContentRect();
    const int width = content.right - content.left;
    fog_.SetPanelBounds(content);
    fog_.RaisePanel();
    ring_.set_ink(StatusColor());
    ring_.set_thickness(Scaled(kRingDip));
    ring_.SetPanelBounds(content);
    ring_.RaisePanel();
    ring_.Invalidate();  // A shaped layer composes on its next paint.
    sprite_.SetPanelBounds(content);
    sprite_.ShowPanel();
    ComposeSprite();
    // The capsule in the page's bottom-right corner (§5.10): 24 in from
    // the right, 16 from the bottom; 36 high collapsed (mark · dot ·
    // name), the 44 dip pill expanded.
    int pill_h = Scaled(kPillHeight);
    int pill_w = std::min(Scaled(kPillMaxWidth), width - Scaled(32));
    if (collapsed_) {
      pill_h = Scaled(kCapsuleHeight);
      pill_w = Scaled(10) + Scaled(16) + Scaled(6) + Scaled(7) + Scaled(6) +
               LabelWidth() + Scaled(12);
      pill_w = std::min(pill_w, width - Scaled(32));
    }
    const int pill_x = content.right - Scaled(kCornerInsetX) - pill_w;
    const int pill_y = content.bottom - pill_h - Scaled(kCornerInsetY);
    const RECT pill_rect = {pill_x, pill_y, pill_x + pill_w, pill_y + pill_h};
    pill_.SetPanelBounds(pill_rect);
    pill_.RaisePanel();
    LayoutButtons(pill_w, pill_h);
    // The capsule is the LAYER's silhouette now (Pill::LayerShapes), not a
    // SetWindowRgn: a region cuts on the pixel grid and stair-steps every
    // corner (lane 3 §3.6 finding 2).
    pill_.Invalidate();
  }

  bool agent_owns() const { return agent_owns_; }

  COLORREF StatusColor() const {
    if (status_ == L"Error") {
      return RGB(0xE0, 0x46, 0x3A);
    }
    if (status_ == L"Waiting for you") {
      return RGB(0xF0, 0xA0, 0x20);
    }
    if (status_ == L"Idle" || status_ == L"You have control") {
      return RGB(0x8E, 0x8E, 0x93);
    }
    return RGB(0x1E, 0x9E, 0x4A);  // Running / Done.
  }

  void PillHovered(bool inside) override {
    if (!pill_.alive()) {
      return;
    }
    if (inside) {
      ::KillTimer(pill_.hwnd(), kCollapseTimer);
      if (collapsed_) {
        ::SetTimer(pill_.hwnd(), kExpandTimer, kExpandDelayMs, nullptr);
      }
    } else {
      ::KillTimer(pill_.hwnd(), kExpandTimer);
      if (!collapsed_ && !waiting_) {
        ::SetTimer(pill_.hwnd(), kCollapseTimer, kCollapseDelayMs, nullptr);
      }
    }
  }

  void PillTimer(UINT_PTR id) override {
    if (!pill_.alive()) {
      return;
    }
    ::KillTimer(pill_.hwnd(), id);
    if (id == kExpandTimer && collapsed_) {
      collapsed_ = false;
      Reposition();
    } else if (id == kCollapseTimer && !collapsed_ && !waiting_) {
      collapsed_ = true;
      Reposition();
    }
  }

  // ---- The agent cursor + input-mirror effects (the sprite layer) ----

  void SetAgentCursor(double css_x, double css_y) {
    const Gdiplus::PointF target(ScaledF(css_x), ScaledF(css_y));
    const ULONGLONG now = ::GetTickCount64();
    if (!cursor_shown_) {
      // First appearance teleports: no sliding in from a stale position.
      cursor_shown_ = true;
      cursor_from_ = cursor_to_ = target;
      glide_start_ = 0;
    } else {
      const Gdiplus::PointF current = CursorNow(now);
      const double distance =
          std::hypot(target.X - current.X, target.Y - current.Y);
      cursor_from_ = current;
      cursor_to_ = target;
      if (distance >= ScaledF(2)) {
        // The Mac glide: duration scales with DIP distance, decel-heavy.
        glide_start_ = now;
        glide_ms_ = static_cast<DWORD>(std::clamp(
            distance * 96.0 / dpi_, 180.0, 450.0));
      } else {
        glide_start_ = 0;
      }
    }
    StartTicking();
    ComposeSprite();
  }

  void PlayEffect(const std::string& kind,
                  bool has_point,
                  double css_x,
                  double css_y,
                  double css_w,
                  double css_h,
                  double dy) {
    Effect effect;
    if (kind == "click") {
      effect.kind = Effect::kClick;
    } else if (kind == "type") {
      effect.kind = Effect::kType;
      // Rapid fills must not stack outlines: a new pulse replaces the
      // still-visible one (the Mac's typingPulseLayer swap).
      effects_.erase(std::remove_if(effects_.begin(), effects_.end(),
                                    [](const Effect& e) {
                                      return e.kind == Effect::kType;
                                    }),
                     effects_.end());
    } else if (kind == "scroll") {
      effect.kind = Effect::kScroll;
    } else {
      return;  // Unknown kinds stay cosmetic no-ops.
    }
    effect.point = has_point
                       ? Gdiplus::PointF(ScaledF(css_x), ScaledF(css_y))
                       : CursorNow(::GetTickCount64());
    effect.width = ScaledF(css_w);
    effect.height = ScaledF(css_h);
    effect.down = dy >= 0;
    effect.start = ::GetTickCount64();
    effects_.push_back(effect);
    StartTicking();
    ComposeSprite();
  }

  void OnSpriteTick() {
    const ULONGLONG now = ::GetTickCount64();
    effects_.erase(std::remove_if(effects_.begin(), effects_.end(),
                                  [now](const Effect& e) {
                                    return now - e.start > e.LifetimeMs();
                                  }),
                   effects_.end());
    ComposeSprite();
  }

 private:
  int Scaled(int design) const { return ::MulDiv(design, dpi_, 96); }
  float ScaledF(double design) const {
    return static_cast<float>(design * dpi_ / 96.0);
  }

  struct Effect {
    enum Kind { kClick, kType, kScroll };
    Kind kind = kClick;
    Gdiplus::PointF point;
    float width = 0;
    float height = 0;
    bool down = true;  // Scroll direction: page scrolls down.
    ULONGLONG start = 0;
    ULONGLONG LifetimeMs() const {
      switch (kind) {
        case kClick:
          return 700;   // dot 400 + second ring 140+550.
        case kType:
          return 1950;  // The Mac pulse's full run.
        case kScroll:
          return 850;   // Third chevron: 2*120 + 600.
      }
      return 700;
    }
  };

  // Where the cursor is RIGHT NOW: mid-glide interpolation on the Mac's
  // decel-heavy bezier (0.3, 0, 0.15, 1), approximated by its y-progress.
  Gdiplus::PointF CursorNow(ULONGLONG now) const {
    if (!glide_start_ || now - glide_start_ >= glide_ms_) {
      return cursor_to_;
    }
    const double t =
        static_cast<double>(now - glide_start_) / glide_ms_;
    // Cubic-bezier(0.3, 0, 0.15, 1) sampled by t — close enough for a
    // 450ms glide: ease-in start, long soft landing.
    const double u = 1.0 - t;
    const double eased =
        3 * u * u * t * 0.0 + 3 * u * t * t * 1.0 + t * t * t;
    return Gdiplus::PointF(
        static_cast<float>(cursor_from_.X +
                           (cursor_to_.X - cursor_from_.X) * eased),
        static_cast<float>(cursor_from_.Y +
                           (cursor_to_.Y - cursor_from_.Y) * eased));
  }

  // The sprite's clock. It was a 33 ms SetTimer of its own; it is now one
  // animation on the shared 16 ms coalescable animator (lane 3 §6.3),
  // running for exactly as long as something is still moving — the glide's
  // remaining span or the longest live effect, whichever ends last. The
  // GLIDE's own curve is untouched: CursorNow() still interpolates the
  // Mac's decel-heavy bezier off the wall clock, so only the frame rate
  // changes.
  void StartTicking() {
    if (!sprite_.alive()) {
      return;
    }
    const ULONGLONG now = ::GetTickCount64();
    long long remaining = 0;
    if (glide_start_ && now - glide_start_ < glide_ms_) {
      remaining = static_cast<long long>(glide_ms_) -
                  static_cast<long long>(now - glide_start_);
    }
    for (const Effect& effect : effects_) {
      const long long left = static_cast<long long>(effect.LifetimeMs()) -
                             static_cast<long long>(now - effect.start);
      remaining = std::max(remaining, left);
    }
    PhiAnimator& animator = PhiAnimator::ForThread();
    animator.CancelForWindow(sprite_.hwnd());
    if (remaining <= 0) {
      return;
    }
    animator.Animate(
        sprite_.hwnd(), static_cast<int>(remaining), PhiEasing::kLinear,
        // The tick only recomposes the layer, which is committed straight
        // through UpdateLayeredWindow: nothing to invalidate.
        [this](double, RECT*) { OnSpriteTick(); },
        [this] { OnSpriteTick(); });
  }

  // Renders the sprite layer — cursor + live effects — into a premultiplied
  // ARGB surface and hands it to UpdateLayeredWindow.
  void ComposeSprite() override {
    if (!sprite_.alive() || !PhiEnsureGdiPlus()) {
      return;
    }
    RECT rc = {};
    ::GetClientRect(sprite_.hwnd(), &rc);
    const int width = rc.right;
    const int height = rc.bottom;
    PhiLayerSurface layer(width, height);
    if (!layer.valid()) {
      return;
    }
    {
      Gdiplus::Bitmap surface(width, height, width * 4,
                              PixelFormat32bppPARGB,
                              static_cast<BYTE*>(layer.bits()));
      Gdiplus::Graphics graphics(&surface);
      graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
      graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
      const ULONGLONG now = ::GetTickCount64();
      for (const Effect& effect : effects_) {
        DrawEffect(&graphics, effect, now);
      }
      // Hidden while the USER holds control (the Mac's moveCursor guard):
      // the agent stops reporting on takeover, so its last position would
      // otherwise sit frozen beside the real cursor until the task ended.
      // The stored point survives the flip, so the cursor reappears where it
      // was when control is handed back.
      if (cursor_shown_ && agent_owns_) {
        const Gdiplus::PointF at = CursorNow(now);
        DrawCursor(&graphics, at);
        DrawNameTag(&graphics, at);
      }
    }
    layer.Commit(sprite_.hwnd(), nullptr);
  }

  Gdiplus::Color AccentColor(BYTE alpha) const {
    const COLORREF accent = controller_->CurrentPalette().accent;
    return Gdiplus::Color(alpha, GetRValue(accent), GetGValue(accent),
                          GetBValue(accent));
  }

  void DrawEffect(Gdiplus::Graphics* graphics,
                  const Effect& effect,
                  ULONGLONG now) const {
    const double ms = static_cast<double>(now - effect.start);
    switch (effect.kind) {
      case Effect::kClick: {
        // Center dot fading over 350ms.
        if (ms < 350) {
          const BYTE alpha = static_cast<BYTE>(255 * (1.0 - ms / 350));
          Gdiplus::SolidBrush dot(AccentColor(alpha));
          const float radius = ScaledF(4);
          graphics->FillEllipse(&dot, effect.point.X - radius,
                                effect.point.Y - radius, radius * 2,
                                radius * 2);
        }
        // Two staggered rings: scale 0.25→1 (ease-out), opacity 0→.85→0.
        const std::array<std::pair<double, float>, 2> rings = {
            {{0.0, ScaledF(18)}, {140.0, ScaledF(26)}}};
        for (const auto& [delay, full_radius] : rings) {
          const double t = (ms - delay) / 500.0;
          if (t < 0 || t > 1) {
            continue;
          }
          const double eased = 1.0 - (1.0 - t) * (1.0 - t);
          const float radius =
              full_radius * static_cast<float>(0.25 + 0.75 * eased);
          const double opacity =
              t < 0.25 ? 0.85 * (t / 0.25) : 0.85 * (1.0 - (t - 0.25) / 0.75);
          Gdiplus::Pen ring(AccentColor(static_cast<BYTE>(255 * opacity)),
                            ScaledF(2.5));
          graphics->DrawEllipse(&ring, effect.point.X - radius,
                                effect.point.Y - radius, radius * 2,
                                radius * 2);
        }
        break;
      }
      case Effect::kType: {
        // Pulsing outline around the field, dots blinking under its
        // trailing edge; the whole group fades out after 1.6s.
        const float min_w = ScaledF(40);
        const float min_h = ScaledF(22);
        const float box_w =
            std::max(effect.width > 0 ? effect.width : ScaledF(160), min_w) +
            ScaledF(8);
        const float box_h =
            std::max(effect.height > 0 ? effect.height : ScaledF(32),
                     min_h) +
            ScaledF(8);
        double group = 1.0;
        if (ms > 1600) {
          group = std::max(0.0, 1.0 - (ms - 1600) / 250.0);
        }
        // Opacity pulse 1 → 0.45 → 1 → 0.45 → 1 over the first 1.6s.
        const double phase = std::fmod(ms / 400.0, 2.0);
        const double pulse =
            1.0 - 0.55 * (phase < 1.0 ? phase : 2.0 - phase);
        const BYTE stroke_alpha =
            static_cast<BYTE>(255 * pulse * group);
        const float left = effect.point.X - box_w / 2;
        const float top = effect.point.Y - box_h / 2;
        const float corner = ScaledF(7) * 2;
        Gdiplus::GraphicsPath box;
        box.AddArc(left, top, corner, corner, 180, 90);
        box.AddArc(left + box_w - corner, top, corner, corner, 270, 90);
        box.AddArc(left + box_w - corner, top + box_h - corner, corner,
                   corner, 0, 90);
        box.AddArc(left, top + box_h - corner, corner, corner, 90, 90);
        box.CloseFigure();
        Gdiplus::SolidBrush fill(
            AccentColor(static_cast<BYTE>(255 * 0.07 * group)));
        graphics->FillPath(&fill, &box);
        Gdiplus::Pen outline(AccentColor(stroke_alpha), ScaledF(2));
        graphics->DrawPath(&outline, &box);
        // Three blinking dots below the trailing edge.
        for (int i = 0; i < 3; ++i) {
          const double blink_t =
              std::fmod((ms - i * 150.0) / 600.0, 1.0);
          double blink = 0.2;
          if (ms >= i * 150.0 && ms < i * 150.0 + 1800.0) {
            blink = blink_t < 0.5 ? 0.2 + 1.6 * blink_t
                                  : 1.0 - 1.6 * (blink_t - 0.5);
          }
          Gdiplus::SolidBrush dot(AccentColor(
              static_cast<BYTE>(255 * std::clamp(blink, 0.0, 1.0) *
                                group)));
          const float dot_r = ScaledF(2.5);
          const float dot_x =
              left + box_w - ScaledF(30) + i * ScaledF(11);
          const float dot_y = top + box_h + ScaledF(9);
          graphics->FillEllipse(&dot, dot_x - dot_r, dot_y - dot_r,
                                dot_r * 2, dot_r * 2);
        }
        break;
      }
      case Effect::kScroll: {
        // Three chevrons drifting in the scroll direction.
        const float travel = effect.down ? ScaledF(34) : -ScaledF(34);
        const float apex = effect.down ? ScaledF(5) : -ScaledF(5);
        for (int i = 0; i < 3; ++i) {
          const double t = (ms - i * 120.0) / 550.0;
          if (t < 0 || t > 1) {
            continue;
          }
          const double opacity =
              t < 0.35 ? 0.9 * (t / 0.35) : 0.9 * (1.0 - (t - 0.35) / 0.65);
          const float y = effect.point.Y - travel * 0.5f +
                          travel * 1.5f * static_cast<float>(t);
          Gdiplus::Pen pen(AccentColor(static_cast<BYTE>(255 * opacity)),
                           ScaledF(3));
          pen.SetStartCap(Gdiplus::LineCapRound);
          pen.SetEndCap(Gdiplus::LineCapRound);
          pen.SetLineJoin(Gdiplus::LineJoinRound);
          Gdiplus::PointF points[3] = {
              {effect.point.X - ScaledF(10), y - apex},
              {effect.point.X, y + apex},
              {effect.point.X + ScaledF(10), y - apex}};
          graphics->DrawLines(&pen, points, 3);
        }
        break;
      }
    }
  }

  // The Mac cursor path (a 14.0089 × 15.9927 viewBox, authored y-down — no
  // flip needed in GDI's y-down space), scaled to a 27pt-tall arrow.
  // §5.10 cue 5: the Teams / Figma-style name tag riding the cursor's
  // lower right, so its movements are never mistaken for the user's.
  void DrawNameTag(Gdiplus::Graphics* graphics,
                   const Gdiplus::PointF& at) const {
    if (agent_label_.empty()) {
      return;
    }
    Gdiplus::Font font(L"Segoe UI", static_cast<float>(Scaled(11)),
                       Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::RectF measured;
    graphics->MeasureString(agent_label_.c_str(), -1, &font,
                            Gdiplus::PointF(0, 0), &measured);
    const float pad_x = static_cast<float>(Scaled(6));
    const float pad_y = static_cast<float>(Scaled(3));
    const float w = measured.Width + pad_x * 2;
    const float h = measured.Height + pad_y * 2;
    const float x = at.X + static_cast<float>(Scaled(16));
    const float y = at.Y + static_cast<float>(Scaled(18));
    const float r = h / 2;
    Gdiplus::GraphicsPath tag;
    tag.AddArc(x, y, r * 2, r * 2, 180, 90);
    tag.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
    tag.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
    tag.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
    tag.CloseFigure();
    Gdiplus::SolidBrush fill(AccentColor(255));
    graphics->FillPath(&fill, &tag);
    Gdiplus::SolidBrush text(Gdiplus::Color(255, 255, 255, 255));
    graphics->SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    graphics->DrawString(agent_label_.c_str(), -1, &font,
                         Gdiplus::PointF(x + pad_x, y + pad_y), &text);
  }

  void DrawCursor(Gdiplus::Graphics* graphics,
                  const Gdiplus::PointF& at) const {
    const float scale = ScaledF(27) / 15.9927f;
    // The tip anchor: the Mac pins layer anchorPoint (0.1, 0.89) y-up —
    // (0.1, 0.11) of the glyph box in y-down terms.
    const float ox = at.X - 0.1f * 14.0089f * scale;
    const float oy = at.Y - 0.11f * 15.9927f * scale;
    auto point = [&](double x, double y) {
      return Gdiplus::PointF(ox + static_cast<float>(x) * scale,
                             oy + static_cast<float>(y) * scale);
    };
    Gdiplus::GraphicsPath path;
    Gdiplus::PointF segment_a[4] = {point(0.5039, 1.8181),
                                    point(0.5893, 0.6644),
                                    point(1.9677, 0.1012),
                                    point(2.8730, 0.8133)};
    path.AddBeziers(segment_a, 4);
    path.AddLine(point(2.8730, 0.8133), point(12.9990, 8.7879));
    Gdiplus::PointF segment_b[4] = {point(12.9990, 8.7879),
                                    point(14.0076, 9.5821),
                                    point(13.3921, 11.1219),
                                    point(12.1641, 11.1219)};
    path.AddBeziers(segment_b, 4);
    path.AddLine(point(12.1641, 11.1219), point(6.5400, 11.1219));
    Gdiplus::PointF segment_c[7] = {
        point(6.5400, 11.1219),  point(6.4112, 11.1219),
        point(6.2842, 11.1504),  point(6.1689, 11.2039),
        point(6.0539, 11.2573),  point(5.9535, 11.3341),
        point(5.8750, 11.4285)};
    path.AddBeziers(segment_c, 7);
    path.AddLine(point(5.8750, 11.4285), point(2.8984, 15.0076));
    Gdiplus::PointF segment_d[4] = {point(2.8984, 15.0076),
                                    point(2.1136, 15.9510),
                                    point(0.5001, 15.4459),
                                    point(0.5000, 14.1668)};
    path.AddBeziers(segment_d, 4);
    path.AddLine(point(0.5000, 14.1668), point(0.5000, 1.9314));
    path.CloseFigure();

    // Light: pale-tint → theme gradient with a dark-hue border; dark: flat
    // theme fill outlined in white (applyTheme's two appearances).
    const PhiPalette& palette = controller_->CurrentPalette();
    const COLORREF accent = palette.accent;
    const Gdiplus::Color accent_color(255, GetRValue(accent),
                                      GetGValue(accent), GetBValue(accent));
    if (palette.dark) {
      Gdiplus::SolidBrush fill(accent_color);
      graphics->FillPath(&fill, &path);
      Gdiplus::Pen border(Gdiplus::Color(255, 255, 255, 255),
                          1.0f * scale);
      border.SetLineJoin(Gdiplus::LineJoinRound);
      graphics->DrawPath(&border, &path);
    } else {
      const Gdiplus::Color pale(
          255, static_cast<BYTE>(GetRValue(accent) +
                                 (255 - GetRValue(accent)) * 72 / 100),
          static_cast<BYTE>(GetGValue(accent) +
                            (255 - GetGValue(accent)) * 72 / 100),
          static_cast<BYTE>(GetBValue(accent) +
                            (255 - GetBValue(accent)) * 72 / 100));
      Gdiplus::RectF bounds;
      path.GetBounds(&bounds);
      Gdiplus::LinearGradientBrush fill(bounds, pale, accent_color,
                                        Gdiplus::LinearGradientModeVertical);
      graphics->FillPath(&fill, &path);
      const Gdiplus::Color border_color(
          255, static_cast<BYTE>(GetRValue(accent) * 35 / 100),
          static_cast<BYTE>(GetGValue(accent) * 35 / 100),
          static_cast<BYTE>(GetBValue(accent) * 35 / 100));
      Gdiplus::Pen border(border_color, 1.0f * scale);
      border.SetLineJoin(Gdiplus::LineJoinRound);
      graphics->DrawPath(&border, &path);
    }
  }

  void EnsureFonts() {
    auto make = [&](int size, int weight) {
      return ::CreateFontW(-Scaled(size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
    };
    label_font_ = make(13, FW_SEMIBOLD);
    caption_font_ = make(12, FW_NORMAL);
  }

  void LayoutButtons(int pill_w, int pill_h) {
    const int mid = pill_h / 2;
    const int bh = Scaled(kButtonHeight);
    int right = pill_w - Scaled(10);
    chevron_ = {right - Scaled(kChevronWidth), mid - bh / 2, right,
                mid + bh / 2};
    right = chevron_.left - Scaled(6);
    if (collapsed_) {
      primary_ = RECT{};
      secondary_ = RECT{};
      return;
    }
    if (!agent_owns_) {
      // User holds control: Finish trails, Hand back leads it.
      const int finish_w = Scaled(72);
      secondary_ = {right - finish_w, mid - bh / 2, right, mid + bh / 2};
      right = secondary_.left - Scaled(8);
    } else {
      secondary_ = RECT{};
    }
    const int primary_w = Scaled(agent_owns_ ? 104 : 92);
    primary_ = {right - primary_w, mid - bh / 2, right, mid + bh / 2};
  }

  // The agent label's ink width in the label font (the collapsed pill is
  // sized to it).
  int LabelWidth() {
    if (!label_font_ || !pill_.hwnd()) {
      return Scaled(80);
    }
    HDC dc = ::GetDC(pill_.hwnd());
    HGDIOBJ old = ::SelectObject(dc, label_font_);
    RECT measured = {0, 0, 0, 0};
    ::DrawTextW(dc, agent_label_.c_str(), -1, &measured,
                DT_LEFT | DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
    ::SelectObject(dc, old);
    ::ReleaseDC(pill_.hwnd(), dc);
    return measured.right;
  }

  // The fold chevron: a two-stroke "v" (unfolded, pointing down) or "^"
  // (folded, pointing up), drawn as lines so it stays crisp at any DPI.
  void PaintChevron(HDC dc, COLORREF ink) {
    if (chevron_.right <= chevron_.left) {
      return;
    }
    const int cx = (chevron_.left + chevron_.right) / 2;
    const int cy = (chevron_.top + chevron_.bottom) / 2;
    const int half = Scaled(4);
    const int dy = collapsed_ ? -Scaled(2) : Scaled(2);
    HPEN pen = ::CreatePen(PS_SOLID, std::max(1, Scaled(2)), ink);
    HGDIOBJ old = ::SelectObject(dc, pen);
    ::MoveToEx(dc, cx - half, cy - dy, nullptr);
    ::LineTo(dc, cx, cy + dy);
    ::LineTo(dc, cx + half + 1, cy - dy - 1);
    ::SelectObject(dc, old);
    ::DeleteObject(pen);
  }

  void PaintPill(HDC dc) override {
    RECT client = {};
    ::GetClientRect(pill_.hwnd(), &client);
    const PhiPalette& palette = controller_->CurrentPalette();
    const COLORREF ground = palette.flyout;
    const COLORREF ink = palette.label;
    const COLORREF dim = palette.label_dim;
    // SQUARE fill: the capsule is the LAYER's alpha (Pill::LayerShapes).
    HBRUSH plate = ::CreateSolidBrush(ground);
    ::FillRect(dc, &client, plate);
    ::DeleteObject(plate);
    ::SetBkMode(dc, TRANSPARENT);
    const int mid = client.bottom / 2;
    // The brand mark (the robot for an unbranded agent).
    const auto draw_mark = [&](int left) {
      RECT mark = {left, mid - Scaled(8), left + Scaled(16), mid + Scaled(8)};
      if (mark_.empty() ||
          !PhiDrawCatalogIcon(dc, mark, mark_, Scaled(14), ink)) {
        PhiDrawIconGlyph(dc, mark, fluent::kRobot, Scaled(14), ink);
      }
      return static_cast<int>(mark.right);
    };

    if (collapsed_) {
      // mark · status dot · the agent's name; the whole capsule is the
      // target (no chevron in a corner).
      int x = draw_mark(Scaled(10)) + Scaled(6);
      RECT dot = {x, mid - Scaled(3) - 1, x + Scaled(7), mid + Scaled(4)};
      PhiFillEllipseAA(dc, dot, StatusColor());
      x = dot.right + Scaled(6);
      RECT text = {x, client.top, client.right - Scaled(12), client.bottom};
      ::SelectObject(dc, label_font_);
      ::SetTextColor(dc, ink);
      ::DrawTextW(dc, agent_label_.c_str(), -1, &text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
      return;
    }
    // Expanded: mark, "<Agent> is working" over the current step, a
    // divider, the brand-filled primary, Finish, the collapse chevron.
    const int text_left = draw_mark(Scaled(12)) + Scaled(10);
    RECT title = {text_left, client.top + Scaled(6), primary_.left - Scaled(16),
                  mid + Scaled(1)};
    ::SelectObject(dc, label_font_);
    ::SetTextColor(dc, ink);
    std::wstring headline = agent_label_;
    if (status_ == L"Running") {
      headline += L" is working";
    } else if (!status_.empty()) {
      headline += L" \u00B7 " + status_;
    }
    ::DrawTextW(dc, headline.c_str(), -1, &title,
                DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
    RECT caption = {text_left, mid + Scaled(1), primary_.left - Scaled(16),
                    client.bottom - Scaled(6)};
    if (caption.right > caption.left) {
      ::SelectObject(dc, caption_font_);
      ::SetTextColor(dc, dim);
      ::DrawTextW(dc, caption_.c_str(), -1, &caption,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
    }
    {
      RECT rule = {primary_.left - Scaled(10), mid - Scaled(10),
                   primary_.left - Scaled(10) + 1, mid + Scaled(10)};
      HBRUSH sep = ::CreateSolidBrush(palette.separator);
      ::FillRect(dc, &rule, sep);
      ::DeleteObject(sep);
    }

    // The primary action carries the accent; the secondary stays quiet.
    FillRounded(dc, primary_, palette.accent, Scaled(6));
    ::SelectObject(dc, label_font_);
    ::SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
    const wchar_t* primary_text = agent_owns_ ? L"Take control" : L"Hand back";
    ::DrawTextW(dc, primary_text, -1, &primary_,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (secondary_.right > secondary_.left) {
      FillRounded(dc, secondary_, palette.hover, Scaled(6));
      ::SetTextColor(dc, ink);
      ::DrawTextW(dc, L"Finish", -1, &secondary_,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    PaintChevron(dc, dim);
  }

  void PillClicked(int x, int y) override {
    const POINT pt = {x, y};
    AgentHost* host = controller_ ? controller_->agent_host() : nullptr;
    if (!host) {
      return;
    }
    if (collapsed_) {
      collapsed_ = false;  // Anywhere on the parked pill unfolds it.
      Reposition();
      return;
    }
    if (::PtInRect(&chevron_, pt)) {
      collapsed_ = true;
      Reposition();
      return;
    }
    // Copy what the handlers need: they can destroy this overlay.
    const std::string task = task_id_;
    if (::PtInRect(&primary_, pt)) {
      if (agent_owns_) {
        host->RequestAgentTakeControl(task);
      } else {
        host->RequestAgentHandBack(task);
      }
      return;
    }
    if (secondary_.right > secondary_.left && ::PtInRect(&secondary_, pt)) {
      host->RequestAgentFinish(task);
    }
  }

  RAW_PTR_EXCLUSION PhiShellWindow* controller_;
  const HWND shell_;  // controller_'s shell at construction.
  std::string task_id_;
  std::wstring agent_label_;
  std::wstring caption_;
  bool agent_owns_ = true;
  bool collapsed_ = true;   // Collapsed by default (§5.10); hover/click opens.
  bool waiting_ = false;    // "Waiting for you": open and stays open.
  std::wstring mark_;
  std::wstring status_;
  Ring ring_;
  RECT chevron_{};
  Fog fog_;
  Sprite sprite_{this};
  Pill pill_{this};
  UINT dpi_ = 96;
  // The agent cursor: target/source in sprite-local device px; a zero
  // glide_start_ means "sitting at cursor_to_".
  bool cursor_shown_ = false;
  Gdiplus::PointF cursor_from_;
  Gdiplus::PointF cursor_to_;
  ULONGLONG glide_start_ = 0;
  DWORD glide_ms_ = 0;
  std::vector<Effect> effects_;
  RECT primary_{};
  RECT secondary_{};
  HFONT label_font_ = nullptr;
  HFONT caption_font_ = nullptr;
};

Overlay* Overlay::instance = nullptr;

}  // namespace

// static
void PhiAgentOverlay::Show(PhiShellWindow* controller,
                           const std::string& task_id,
                           const std::wstring& agent_label,
                           const std::wstring& caption,
                           bool agent_owns,
                           const std::wstring& mark,
                           const std::wstring& status,
                           bool waiting) {
  if (!controller || !controller->shell_hwnd()) {
    return;
  }
  if (Overlay::instance && (Overlay::instance->controller() != controller ||
                            Overlay::instance->task_id() != task_id)) {
    Hide();  // Watching a different Space now.
  }
  if (!Overlay::instance) {
    Overlay::instance = new Overlay(controller, task_id);
  }
  Overlay::instance->Update(agent_label, caption, agent_owns, mark, status,
                            waiting);
}

// static
void PhiAgentOverlay::Hide() {
  Overlay* overlay = Overlay::instance;
  Overlay::instance = nullptr;
  delete overlay;
}

// static
void PhiAgentOverlay::HideIfBoundTo(const PhiShellWindow* controller) {
  if (Overlay::instance && Overlay::instance->controller() == controller) {
    Hide();
  }
}

// static
bool PhiAgentOverlay::IsShown() {
  return Overlay::instance != nullptr;
}

// static
void PhiAgentOverlay::Reposition() {
  if (Overlay::instance) {
    Overlay::instance->Reposition();
  }
}

// static
void PhiAgentOverlay::SetAgentCursor(const std::string& task_id,
                                     double x,
                                     double y) {
  if (Overlay::instance && Overlay::instance->task_id() == task_id) {
    Overlay::instance->SetAgentCursor(x, y);
  }
}

// static
void PhiAgentOverlay::PlayAgentEffect(const std::string& task_id,
                                      const std::string& kind,
                                      bool has_point,
                                      double x,
                                      double y,
                                      double w,
                                      double h,
                                      double dy) {
  if (Overlay::instance && Overlay::instance->task_id() == task_id) {
    Overlay::instance->PlayEffect(kind, has_point, x, y, w, h, dy);
  }
}

}  // namespace phi_client

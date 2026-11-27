// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_screenshot_area.h"

#include <windows.h>
#include <windowsx.h>
#include <imm.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_i18n.h"
#include "client/phi_panel.h"
#include "client/phi_shell_window.h"

namespace phi_client {

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kEdgeZoneDip = 32;      // The Mac's 32-point edge band.
constexpr double kEdgeStepCss = 28;   // CSS px per tick at the very edge.
constexpr UINT kEdgeTickMs = 30;
constexpr double kWheelLineCss = 40;  // CSS px per wheel line (3 per notch).
constexpr uint64_t kWheelPrecedenceMs = 220;
constexpr int kMinSelectionDip = 2;
constexpr BYTE kDimAlpha = 0x5C;      // About 36 % black over the page.

ID2D1Factory* D2d() {
  static ID2D1Factory* factory = [] {
    ID2D1Factory* f = nullptr;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &f);
    return f;
  }();
  return factory;
}

IDWriteFactory* DWrite() {
  static IDWriteFactory* factory = [] {
    IDWriteFactory* f = nullptr;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(&f));
    return f;
  }();
  return factory;
}

class AreaOverlay : public PhiPanel {
 public:
  AreaOverlay(PhiShellWindow* shell,
              const RECT& viewport,
              const PhiScreenshotViewport& v,
              PhiScreenshotAreaHost* host)
      : shell_(shell), host_(host), v_(v), edge_(NewTimer()) {
    ClassSpec spec;
    spec.name = L"PhiScreenshotArea";
    spec.cursor = IDC_CROSS;
    Options options;
    options.layered = true;
    if (!CreatePanel(spec, shell->shell_hwnd(), viewport, options)) {
      return;
    }
    DeleteWhenDestroyed();
    ::ImmAssociateContextEx(hwnd(), nullptr, 0);  // Escape, not composition.
    ShowPanel();
    ::SetFocus(hwnd());
    Invalidate();
  }

  ~AreaOverlay() override = default;

  static AreaOverlay* instance;

  PhiShellWindow* shell() const { return shell_; }

  void SetViewport(const PhiScreenshotViewport& v) {
    v_.scroll_x = v.scroll_x;
    v_.scroll_y = v.scroll_y;
    v_.doc_width = v.doc_width;
    v_.doc_height = v.doc_height;
    scroll_in_flight_ = false;
    Invalidate();
    if (pending_dx_ != 0 || pending_dy_ != 0) {
      RequestScroll(0, 0);  // Sends what the wheel queued meanwhile.
      return;
    }
    if (release_pending_) {
      Finish();
    }
  }

  base::DictValue State() const {
    base::DictValue out;
    out.Set("open", true);
    out.Set("selecting", selecting_);
    const PhiRectF r = Selection();
    base::DictValue rect;
    rect.Set("x", r.left);
    rect.Set("y", r.top);
    rect.Set("width", r.width());
    rect.Set("height", r.height());
    out.Set("rect", std::move(rect));
    out.Set("scrollX", v_.scroll_x);
    out.Set("scrollY", v_.scroll_y);
    out.Set("cssPerPx", v_.css_per_px_x());
    return out;
  }

  // Closes this overlay and tells the host; touches nothing after.
  static void EndWith(bool done, const PhiRectF& rect) {
    AreaOverlay* self = instance;
    if (!self) {
      return;
    }
    PhiScreenshotAreaHost* host = self->host_;
    instance = nullptr;
    if (self->alive()) {
      self->Destroy();  // Freed at WM_NCDESTROY.
    } else {
      delete self;
    }
    if (done) {
      host->AreaDone(rect);
    } else {
      host->AreaCancel();
    }
  }

 protected:
  void ComposeLayer() override {
    RECT rc = {};
    ::GetClientRect(hwnd(), &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
      return;
    }
    PhiLayerSurface surface(w, h);
    if (!surface.valid()) {
      return;
    }
    ComPtr<ID2D1DCRenderTarget> target;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    if (!D2d() || FAILED(D2d()->CreateDCRenderTarget(&props, &target)) ||
        FAILED(target->BindDC(surface.dc(), &rc))) {
      return;
    }
    target->BeginDraw();
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target->Clear(D2D1::ColorF(0, 0, 0, kDimAlpha / 255.0f));
    ComPtr<ID2D1SolidColorBrush> brush;
    target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush);
    const PhiRectF sel = SelectionPx();
    if (selecting_ && !sel.empty()) {
      const D2D1_RECT_F r = D2D1::RectF(
          static_cast<float>(sel.left), static_cast<float>(sel.top),
          static_cast<float>(sel.right), static_cast<float>(sel.bottom));
      // The hole: alpha 1, not 0 — a fully transparent pixel of a layered
      // window passes the mouse through to the page under it.
      target->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
      target->Clear(D2D1::ColorF(0, 0, 0, 1.0f / 255.0f));
      target->PopAxisAlignedClip();
      brush->SetColor(D2D1::ColorF(0, 0, 0, 0.45f));
      target->DrawRectangle(D2D1::RectF(r.left - 1, r.top - 1, r.right + 1,
                                        r.bottom + 1),
                            brush.Get(), 1.0f);
      brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
      target->DrawRectangle(r, brush.Get(), static_cast<float>(Scaled(1)));
      const PhiRectF css = Selection();
      DrawLabel(target.Get(), brush.Get(),
                std::to_wstring(static_cast<int>(std::lround(css.width()))) +
                    L" × " +
                    std::to_wstring(static_cast<int>(std::lround(css.height()))),
                sel.left, sel.bottom + Scaled(6), w, h);
    }
    DrawLabel(target.Get(), brush.Get(), PhiStr("browser.screenshot.areaHint"),
              -1, Scaled(14), w, h);
    target->EndDraw();
    surface.Commit(hwnd(), nullptr);
  }

  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) override {
    switch (message) {
      case WM_LBUTTONDOWN: {
        *handled = true;
        ::SetFocus(hwnd());
        capture_ = BeginCapture();
        cursor_ = {static_cast<double>(GET_X_LPARAM(lparam)),
                   static_cast<double>(GET_Y_LPARAM(lparam))};
        anchor_ = PhiScreenshotToDocument(v_, cursor_.x, cursor_.y);
        selecting_ = true;
        release_pending_ = false;
        edge_.Start(kEdgeTickMs);
        Invalidate();
        return 0;
      }
      case WM_MOUSEMOVE:
        if (selecting_) {
          *handled = true;
          cursor_ = {static_cast<double>(GET_X_LPARAM(lparam)),
                     static_cast<double>(GET_Y_LPARAM(lparam))};
          Invalidate();
          return 0;
        }
        break;
      case WM_LBUTTONUP:
        if (selecting_) {
          *handled = true;
          cursor_ = {static_cast<double>(GET_X_LPARAM(lparam)),
                     static_cast<double>(GET_Y_LPARAM(lparam))};
          edge_.Stop();
          release_pending_ = true;
          // An in-flight scroll (or wheel movement still queued) lands
          // first; SetViewport finishes then.
          if (!scroll_in_flight_ && pending_dx_ == 0 && pending_dy_ == 0) {
            Finish();
          }
          return 0;
        }
        break;
      case WM_MOUSEWHEEL:
      case WM_MOUSEHWHEEL: {
        *handled = true;
        if (!selecting_ || release_pending_) {
          return 0;
        }
        const double lines = GET_WHEEL_DELTA_WPARAM(wparam) / 120.0 * 3;
        const bool horizontal = message == WM_MOUSEHWHEEL ||
                                (GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT);
        const double step = lines * kWheelLineCss;
        wheel_until_ = ::GetTickCount64() + kWheelPrecedenceMs;
        if (horizontal) {
          RequestScroll(message == WM_MOUSEHWHEEL ? step : -step, 0);
        } else {
          RequestScroll(0, -step);
        }
        return 0;
      }
      case WM_RBUTTONDOWN:
        *handled = true;
        EndWith(false, {});
        return 0;
      case WM_KILLFOCUS:
        // Focus moved to another window (another app, the shell's chrome):
        // the selection is over.
        if (reinterpret_cast<HWND>(wparam) != hwnd()) {
          *handled = true;
          ::PostMessageW(hwnd(), kMsgCancel, 0, 0);
          return 0;
        }
        break;
      case kMsgCancel:
        *handled = true;
        if (::GetFocus() != hwnd()) {
          EndWith(false, {});
        }
        return 0;
    }
    return PhiPanel::OnMessage(message, wparam, lparam, handled);
  }

  bool OnKey(WPARAM key) override {
    if (key == VK_ESCAPE) {
      EndWith(false, {});
      return true;
    }
    return true;  // Every other key is swallowed while selecting.
  }

  void OnCaptureLost() override {
    if (selecting_ && !release_pending_) {
      ::PostMessageW(hwnd(), kMsgCancel, 0, 0);
    }
  }

  void OnTimerFired(UINT_PTR id) override {
    if (id != edge_.id() || !selecting_ || scroll_in_flight_ ||
        ::GetTickCount64() < wheel_until_) {
      return;
    }
    const PhiPointF step = PhiScreenshotEdgeScroll(
        v_, cursor_.x, cursor_.y, Scaled(kEdgeZoneDip), kEdgeStepCss);
    if (step.x != 0 || step.y != 0) {
      RequestScroll(step.x, step.y);
    }
  }

 private:
  static constexpr UINT kMsgCancel = WM_APP + 61;

  void RequestScroll(double dx, double dy) {
    pending_dx_ += dx;
    pending_dy_ += dy;
    if (scroll_in_flight_ || (pending_dx_ == 0 && pending_dy_ == 0)) {
      return;
    }
    // Don't ask past the document: the page clamps, but a request that
    // cannot move anything would only keep the pipe busy.
    const double max_x = std::max(0.0, v_.doc_width - v_.inner_width);
    const double max_y = std::max(0.0, v_.doc_height - v_.inner_height);
    const double tx = std::clamp(v_.scroll_x + pending_dx_, 0.0, max_x);
    const double ty = std::clamp(v_.scroll_y + pending_dy_, 0.0, max_y);
    const double dx_now = tx - v_.scroll_x;
    const double dy_now = ty - v_.scroll_y;
    pending_dx_ = 0;
    pending_dy_ = 0;
    if (std::abs(dx_now) < 0.5 && std::abs(dy_now) < 0.5) {
      if (release_pending_) {
        Finish();
      }
      return;
    }
    scroll_in_flight_ = true;
    host_->AreaScroll(dx_now, dy_now);
  }

  // The selection in document CSS px.
  PhiRectF Selection() const {
    if (!selecting_) {
      return {};
    }
    return PhiRectFromPoints(anchor_,
                             PhiScreenshotToDocument(v_, cursor_.x, cursor_.y));
  }

  // The selection in overlay px (may run past the overlay).
  PhiRectF SelectionPx() const {
    const PhiRectF css = Selection();
    return PhiRectFromPoints(PhiScreenshotToOverlay(v_, css.left, css.top),
                             PhiScreenshotToOverlay(v_, css.right, css.bottom));
  }

  void Finish() {
    const PhiRectF px = SelectionPx();
    if (px.width() < Scaled(kMinSelectionDip) ||
        px.height() < Scaled(kMinSelectionDip)) {
      // A click, not a drag: back to waiting for one.
      selecting_ = false;
      release_pending_ = false;
      capture_.Release();
      Invalidate();
      return;
    }
    const PhiRectF rect = Selection();
    EndWith(true, rect);
  }

  void DrawLabel(ID2D1RenderTarget* target,
                 ID2D1SolidColorBrush* brush,
                 const std::wstring& text,
                 double left,
                 double top,
                 int width,
                 int height) {
    if (!DWrite()) {
      return;
    }
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(DWrite()->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            static_cast<float>(Scaled(12)), L"", &format))) {
      return;
    }
    ComPtr<IDWriteTextLayout> layout;
    DWrite()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                               format.Get(), static_cast<float>(width), 100.0f,
                               &layout);
    DWRITE_TEXT_METRICS m = {};
    if (!layout || FAILED(layout->GetMetrics(&m))) {
      return;
    }
    const float pad_x = static_cast<float>(Scaled(10));
    const float pad_y = static_cast<float>(Scaled(5));
    const float pill_w = m.width + pad_x * 2;
    const float pill_h = m.height + pad_y * 2;
    float x = left < 0 ? (width - pill_w) / 2 : static_cast<float>(left);
    float y = static_cast<float>(top);
    x = std::clamp(x, 0.0f, std::max(0.0f, width - pill_w));
    if (y + pill_h > height) {
      y = std::max(0.0f, static_cast<float>(height) - pill_h - Scaled(6));
    }
    const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
        D2D1::RectF(x, y, x + pill_w, y + pill_h), pill_h / 2, pill_h / 2);
    brush->SetColor(D2D1::ColorF(0.1f, 0.1f, 0.12f, 0.82f));
    target->FillRoundedRectangle(pill, brush);
    brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
    target->DrawTextLayout(D2D1::Point2F(x + pad_x - m.left, y + pad_y),
                           layout.Get(), brush);
  }

  RAW_PTR_EXCLUSION PhiShellWindow* shell_;
  RAW_PTR_EXCLUSION PhiScreenshotAreaHost* host_;
  PhiScreenshotViewport v_;
  Timer edge_;
  Capture capture_;
  bool selecting_ = false;
  bool release_pending_ = false;
  bool scroll_in_flight_ = false;
  double pending_dx_ = 0;
  double pending_dy_ = 0;
  uint64_t wheel_until_ = 0;
  PhiPointF anchor_;
  PhiPointF cursor_;
};

AreaOverlay* AreaOverlay::instance = nullptr;

}  // namespace

// static
bool PhiScreenshotArea::Open(PhiShellWindow* shell,
                             const RECT& viewport,
                             const PhiScreenshotViewport& v,
                             PhiScreenshotAreaHost* host) {
  Close();
  if (!shell || !host) {
    return false;
  }
  auto* overlay = new AreaOverlay(shell, viewport, v, host);
  if (!overlay->alive()) {
    delete overlay;
    return false;
  }
  AreaOverlay::instance = overlay;
  return true;
}

// static
void PhiScreenshotArea::SetViewport(const PhiScreenshotViewport& v) {
  if (AreaOverlay::instance) {
    AreaOverlay::instance->SetViewport(v);
  }
}

// static
void PhiScreenshotArea::Close() {
  AreaOverlay* gone = AreaOverlay::instance;
  if (!gone) {
    return;
  }
  AreaOverlay::instance = nullptr;
  if (gone->alive()) {
    gone->Destroy();
  } else {
    delete gone;
  }
}

// static
bool PhiScreenshotArea::IsOpen() {
  return AreaOverlay::instance != nullptr;
}

// static
bool PhiScreenshotArea::OwnsFocus() {
  return AreaOverlay::instance && AreaOverlay::instance->alive() &&
         ::GetFocus() == AreaOverlay::instance->hwnd();
}

// static
void PhiScreenshotArea::CloseIfBoundTo(PhiShellWindow* shell) {
  if (AreaOverlay::instance && AreaOverlay::instance->shell() == shell) {
    Close();
  }
}

// static
void PhiScreenshotArea::RaiseIfBoundTo(PhiShellWindow* shell) {
  if (AreaOverlay::instance && AreaOverlay::instance->shell() == shell &&
      AreaOverlay::instance->alive()) {
    AreaOverlay::instance->RaisePanel();
  }
}

// static
bool PhiScreenshotArea::IsBoundTo(PhiShellWindow* shell) {
  return AreaOverlay::instance && AreaOverlay::instance->shell() == shell &&
         AreaOverlay::instance->alive();
}

// static
bool PhiScreenshotArea::FocusIfBoundTo(PhiShellWindow* shell) {
  if (!IsBoundTo(shell)) {
    return false;
  }
  ::SetFocus(AreaOverlay::instance->hwnd());
  return true;
}

// static
base::DictValue PhiScreenshotArea::StateForTest() {
  if (!AreaOverlay::instance) {
    base::DictValue out;
    out.Set("open", false);
    return out;
  }
  return AreaOverlay::instance->State();
}

}  // namespace phi_client

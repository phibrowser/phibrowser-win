// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_screenshot_editor.h"

#include <windows.h>
#include <windowsx.h>

#include <commctrl.h>
#include <imm.h>
#include <commdlg.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "client/phi_dwrite.h"
#include "client/phi_i18n.h"
#include "client/phi_menu_paint.h"
#include "client/phi_panel.h"
#include "client/phi_prompt.h"
#include "client/phi_screenshot_render.h"
#include "client/phi_shell_menus.h"
#include "client/phi_shell_window.h"
#include "client/phi_toast.h"

namespace phi_client {

using Microsoft::WRL::ComPtr;

namespace {

enum class Tool {
  kSelect,
  kArrow,
  kFreehand,
  kText,
  kRectangle,
  kEllipse,
  kBlur,
  kCrop,
};
constexpr Tool kTools[] = {Tool::kSelect,    Tool::kArrow,   Tool::kFreehand,
                           Tool::kText,      Tool::kRectangle, Tool::kEllipse,
                           Tool::kBlur,      Tool::kCrop};

const char* ToolName(Tool t) {
  switch (t) {
    case Tool::kSelect:
      return "select";
    case Tool::kArrow:
      return "arrow";
    case Tool::kFreehand:
      return "freehand";
    case Tool::kText:
      return "text";
    case Tool::kRectangle:
      return "rectangle";
    case Tool::kEllipse:
      return "ellipse";
    case Tool::kBlur:
      return "blur";
    case Tool::kCrop:
      return "crop";
  }
  return "select";
}

const char* ToolLabelKey(Tool t) {
  switch (t) {
    case Tool::kSelect:
      return "browser.screenshot.select";
    case Tool::kArrow:
      return "browser.screenshot.arrow";
    case Tool::kFreehand:
      return "browser.screenshot.freehand";
    case Tool::kText:
      return "browser.screenshot.text";
    case Tool::kRectangle:
      return "browser.screenshot.rectangle";
    case Tool::kEllipse:
      return "browser.screenshot.ellipse";
    case Tool::kBlur:
      return "browser.screenshot.mosaic";
    case Tool::kCrop:
      return "browser.screenshot.crop";
  }
  return "browser.screenshot.select";
}

// The Mac's tool letters.
wchar_t ToolKey(Tool t) {
  switch (t) {
    case Tool::kSelect:
      return L'V';
    case Tool::kArrow:
      return L'A';
    case Tool::kFreehand:
      return L'P';
    case Tool::kText:
      return L'T';
    case Tool::kRectangle:
      return L'R';
    case Tool::kEllipse:
      return L'O';
    case Tool::kBlur:
      return L'M';
    case Tool::kCrop:
      return L'C';
  }
  return 0;
}

std::optional<PhiAnnotationKind> KindFor(Tool t) {
  switch (t) {
    case Tool::kArrow:
      return PhiAnnotationKind::kArrow;
    case Tool::kFreehand:
      return PhiAnnotationKind::kFreehand;
    case Tool::kText:
      return PhiAnnotationKind::kText;
    case Tool::kRectangle:
      return PhiAnnotationKind::kRectangle;
    case Tool::kEllipse:
      return PhiAnnotationKind::kEllipse;
    case Tool::kBlur:
      return PhiAnnotationKind::kBlur;
    default:
      return std::nullopt;
  }
}

// Hit keys.
constexpr int kHitTool = 100;  // + index into kTools.
constexpr int kHitUndo = 200;
constexpr int kHitRedo = 201;
constexpr int kHitExport = 202;
constexpr int kHitSwatch = 300;  // + index.
constexpr int kHitSize = 400;    // + index.
constexpr int kHitFill = 500;
constexpr int kHitZoomOut = 600;
constexpr int kHitZoomLabel = 601;
constexpr int kHitZoomIn = 602;
constexpr int kHitCropCancel = 700;
constexpr int kHitCropApply = 701;

// Layout (DIP).
constexpr int kMargin = 20;
constexpr int kButton = 32;
constexpr int kBarPad = 6;
constexpr int kBarHeight = kButton + kBarPad * 2;
constexpr int kBarGap = 16;  // Under the bar.
constexpr int kStripGap = 8;
constexpr int kSeparator = 13;
constexpr int kHandle = 8;
constexpr double kMinZoom = 0.05;
constexpr double kMaxZoom = 8.0;

constexpr UINT_PTR kEditSubclass = 0x5C2E;
constexpr int kEditId = 0x5C21;

D2D1_COLOR_F Ink(PhiColor c, float alpha = 1.0f) {
  return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f,
                      GetBValue(c) / 255.0f, alpha);
}

D2D1_COLOR_F Argb(uint32_t argb) {
  return D2D1::ColorF(((argb >> 16) & 0xFF) / 255.0f,
                      ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f,
                      ((argb >> 24) & 0xFF) / 255.0f);
}

D2D1_RECT_F Rf(const RECT& r) {
  return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                     static_cast<float>(r.right), static_cast<float>(r.bottom));
}

class Editor : public PhiPanel {
 public:
  Editor(PhiShellWindow* shell,
         PhiScreenshotImageRef image,
         const std::wstring& title,
         double device_pixel_ratio)
      : shell_(shell),
        title_(title),
        doc_(std::make_unique<PhiScreenshotDocument>(std::move(image))),
        unit_(PhiScreenshotStyleUnit(device_pixel_ratio)),
        stroke_(kPhiScreenshotWidths[0] * unit_),
        font_size_(kPhiScreenshotTextSizes[1] * unit_) {
    ClassSpec spec;
    spec.name = L"PhiScreenshotEditor";
    spec.style = CS_DBLCLKS;
    spec.cursor = IDC_ARROW;
    Options options;
    const RECT nothing = {0, 0, 0, 0};
    if (!CreatePanel(spec, shell->shell_hwnd(), nothing, options)) {
      return;
    }
    DeleteWhenDestroyed();
    // The canvas takes no text: with an IME on (Chinese, Japanese, Korean)
    // the tool letters would arrive as VK_PROCESSKEY and never reach
    // OnKey (the Mac's 7fe71d5c non-Latin shortcuts). The text field keeps
    // its own IME.
    ::ImmAssociateContextEx(hwnd(), nullptr, 0);
    EnableTooltips(true);
    Reposition();
    FitZoom();
    ShowPanel();
    ::SetFocus(hwnd());
  }

  ~Editor() override = default;

  static Editor* instance;

  PhiShellWindow* shell() const { return shell_; }
  bool dirty() const { return doc_->dirty(); }
  bool busy() const { return drag_ != Drag::kNone; }

  void Reposition() {
    SetPanelBounds(shell_->ContentRect());
    ClampView();
    Invalidate();
  }

  void Raise() { RaisePanel(); }

  // The text field when one is open, else the canvas.
  void TakeFocus() { ::SetFocus(edit_ ? edit_ : hwnd()); }

  // Ends whatever is in flight (a drag, a text session) the way focus
  // loss does: drags are cancelled, text is committed.
  void SettleEdits() {
    if (drag_ != Drag::kNone) {
      CancelDrag();
    }
    CommitText();
  }

  bool ExportTo(PhiScreenshotFormat format,
                const std::wstring& path,
                base::DictValue* out,
                std::string* error) {
    SettleEdits();
    const uint64_t generation = doc_->generation();
    PhiScreenshotImage flat;
    if (!painter_.Flatten(doc_->state(), &flat)) {
      *error = "render_failed";
      return false;
    }
    if (!PhiScreenshotWriteFile(flat, format, path, error)) {
      return false;
    }
    // A later edit (none can happen while this runs on the UI thread, but
    // the rule is the Mac's) keeps the document dirty.
    if (doc_->generation() == generation) {
      doc_->MarkSaved(generation);
    }
    if (out) {
      out->Set("width", flat.width);
      out->Set("height", flat.height);
    }
    Invalidate();
    return true;
  }

  base::DictValue State() const {
    base::DictValue out;
    out.Set("open", true);
    out.Set("hwnd", static_cast<double>(reinterpret_cast<uintptr_t>(hwnd())));
    out.Set("tool", ToolName(tool_));
    out.Set("imageWidth", doc_->image().width);
    out.Set("imageHeight", doc_->image().height);
    out.Set("undo", static_cast<int>(doc_->undo_depth()));
    out.Set("redo", static_cast<int>(doc_->redo_depth()));
    out.Set("dirty", doc_->dirty());
    out.Set("zoom", zoom_);
    out.Set("editingText", editing_id_ != 0);
    out.Set("focused", OwnsFocus());
    base::ListValue objects;
    for (const PhiAnnotation& a : doc_->annotations()) {
      base::DictValue o;
      o.Set("id", static_cast<double>(a.id));
      o.Set("kind", PhiAnnotationKindName(a.kind));
      const PhiRectF b = PhiAnnotationBox(a);
      o.Set("x", b.left);
      o.Set("y", b.top);
      o.Set("width", b.width());
      o.Set("height", b.height());
      o.Set("color", static_cast<double>(a.color));
      o.Set("stroke", a.stroke);
      o.Set("fill", a.fill);
      o.Set("fontSize", a.font_size);
      o.Set("text", base::WideToUTF8(a.text));
      o.Set("selected", Selected(a.id));
      objects.Append(std::move(o));
    }
    out.Set("objects", std::move(objects));
    if (crop_) {
      base::DictValue c;
      c.Set("x", crop_->left);
      c.Set("y", crop_->top);
      c.Set("width", crop_->width());
      c.Set("height", crop_->height());
      out.Set("crop", std::move(c));
    }
    // Where the canvas maps source px, so a test can aim real input.
    RECT rc = {};
    ::GetWindowRect(hwnd(), &rc);
    base::DictValue canvas;
    canvas.Set("screenX", rc.left + origin_x_);
    canvas.Set("screenY", rc.top + origin_y_);
    canvas.Set("scale", zoom_);
    // The part of the image on screen, in source px.
    const RECT area = CanvasArea();
    const PhiRectF shown = PhiRectIntersect(
        PhiRectFromPoints(ToSource(POINT{area.left, area.top}),
                          ToSource(POINT{area.right, area.bottom})),
        PhiRectF{0, 0, static_cast<double>(doc_->image().width),
                 static_cast<double>(doc_->image().height)});
    canvas.Set("visibleX", shown.left);
    canvas.Set("visibleY", shown.top);
    canvas.Set("visibleWidth", shown.width());
    canvas.Set("visibleHeight", shown.height());
    out.Set("canvas", std::move(canvas));
    return out;
  }

  bool OwnsFocus() const {
    const HWND focus = ::GetFocus();
    return focus && (focus == hwnd() || ::GetParent(focus) == hwnd());
  }

  // Asks about unsaved edits; true when it is fine to close.
  bool ConfirmDiscard() {
    SettleEdits();
    if (!doc_->dirty()) {
      return true;
    }
    const std::wstring title = PhiStr("browser.screenshot.discardTitle");
    const std::wstring message = PhiStr("browser.screenshot.discardMessage");
    const int choice = PhiAlert(
        shell_->shell_hwnd(), title.c_str(), message.c_str(),
        {{PhiStr("browser.screenshot.keepEditing"), PhiAlertRole::kPrimary},
         {PhiStr("browser.screenshot.closeWithoutSaving"),
          PhiAlertRole::kDestructive}},
        0, 0);
    return choice == 1;
  }

 protected:
  // --- Paint --------------------------------------------------------------------
  void Paint(HDC dc, const RECT& dirty) override {
    RECT rc = {};
    ::GetClientRect(hwnd(), &rc);
    if (!EnsureTarget() || FAILED(target_->BindDC(dc, &rc))) {
      HBRUSH ground = ::CreateSolidBrush(palette().ground_fixed);
      ::FillRect(dc, &rc, ground);
      ::DeleteObject(ground);
      return;
    }
    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    const PhiPalette& p = palette();
    target_->Clear(Ink(p.dark ? PhiRgb(0x1C, 0x1C, 0x1E)
                              : PhiRgb(0xE4, 0xE4, 0xE7)));
    if (bitmap_image_ != doc_->image_ref()) {
      bitmap_ = painter_.Upload(target_.Get(), doc_->image());
      bitmap_image_ = doc_->image_ref();
    }
    // The canvas's drop shadow, then the document in source px.
    ComPtr<ID2D1SolidColorBrush> brush;
    target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.18f), &brush);
    const D2D1_RECT_F page = D2D1::RectF(
        static_cast<float>(origin_x_), static_cast<float>(origin_y_),
        static_cast<float>(origin_x_ + doc_->image().width * zoom_),
        static_cast<float>(origin_y_ + doc_->image().height * zoom_));
    for (int i = 1; i <= 3; ++i) {
      brush->SetColor(D2D1::ColorF(0, 0, 0, 0.05f));
      target_->FillRectangle(D2D1::RectF(page.left - i, page.top - i + 1,
                                         page.right + i, page.bottom + i + 1),
                             brush.Get());
    }
    target_->SetTransform(
        D2D1::Matrix3x2F::Scale(static_cast<float>(zoom_),
                                static_cast<float>(zoom_)) *
        D2D1::Matrix3x2F::Translation(static_cast<float>(origin_x_),
                                      static_cast<float>(origin_y_)));
    painter_.Draw(target_.Get(), bitmap_.Get(), doc_->state(), editing_id_);
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    PaintSelection(brush.Get());
    PaintCrop(brush.Get());
    ClearHits();
    PaintChrome(brush.Get());
    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
      target_.Reset();
      bitmap_.Reset();
      bitmap_image_ = nullptr;
      Invalidate();
    }
  }

  // --- Input ---------------------------------------------------------------------
  void OnMouseDown(int hit, POINT pt) override {
    ::SetFocus(hwnd());
    pressed_hit_ = hit;
    if (hit >= 0) {
      return;  // A control: it acts on release.
    }
    if (pt.y >= chrome_top_ || PtInRect(&zoom_rect_, pt)) {
      return;  // The chrome's air.
    }
    BeginCanvasDrag(pt);
  }

  void OnClick(int hit, POINT pt) override {
    if (drag_ != Drag::kNone) {
      EndDrag(pt);
      return;
    }
    const int pressed = pressed_hit_;
    pressed_hit_ = -1;
    if (hit < 0 || hit != pressed) {
      return;
    }
    const PhiHit* h = &hits_[static_cast<size_t>(hit)];
    if (!h->enabled) {
      return;
    }
    Invoke(h->key, pt);
  }

  bool InvokeHit(int index) override {
    if (index < 0 || static_cast<size_t>(index) >= hits_.size()) {
      return false;
    }
    Invoke(hits_[static_cast<size_t>(index)].key, POINT{});
    return true;
  }

  void OnCaptureLost() override {
    if (drag_ != Drag::kNone) {
      CancelDrag();
    }
  }

  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) override {
    switch (message) {
      case WM_MOUSEMOVE:
        if (drag_ != Drag::kNone) {
          *handled = true;
          MoveDrag({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
          return 0;
        }
        break;  // The base tracks hover.
      case WM_LBUTTONDBLCLK: {
        *handled = true;
        const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (HitTest(pt) >= 0) {
          OnMouseDown(HitTest(pt), pt);  // A second click on a control.
          return 0;
        }
        const uint64_t id = doc_->HitTest(ToSource(pt), Tolerance());
        const PhiAnnotation* a = id ? doc_->Find(id) : nullptr;
        if (a && a->kind == PhiAnnotationKind::kText) {
          BeginText(id, doc_->Snapshot());
        }
        return 0;
      }
      case WM_RBUTTONUP: {
        *handled = true;
        ContextMenu({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        return 0;
      }
      case WM_MOUSEWHEEL:
      case WM_MOUSEHWHEEL: {
        *handled = true;
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ::ScreenToClient(hwnd(), &pt);
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        const WORD keys = GET_KEYSTATE_WPARAM(wparam);
        if (message == WM_MOUSEWHEEL && (keys & MK_CONTROL)) {
          ZoomAround(zoom_ * std::pow(1.1, delta / 120.0), pt);
        } else if (message == WM_MOUSEHWHEEL || (keys & MK_SHIFT)) {
          ScrollBy(message == WM_MOUSEHWHEEL ? -delta : delta, 0);
        } else {
          ScrollBy(0, delta);
        }
        return 0;
      }
      case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT &&
            reinterpret_cast<HWND>(wparam) == hwnd()) {
          *handled = true;
          ::SetCursor(::LoadCursorW(nullptr, CursorAt()));
          return TRUE;
        }
        break;
      case WM_COMMAND:
        if (LOWORD(wparam) == kEditId && HIWORD(wparam) == EN_CHANGE) {
          *handled = true;
          OnTextChanged();
          return 0;
        }
        break;
      case WM_CTLCOLOREDIT:
        if (reinterpret_cast<HWND>(lparam) == edit_) {
          *handled = true;
          const PhiAnnotation* a = doc_->Find(editing_id_);
          const uint32_t argb = a ? a->color : 0xFF000000u;
          const COLORREF ink = RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF,
                                   argb & 0xFF);
          // A light ink types on dark paper, everything else on white.
          const int luma = ((argb >> 16) & 0xFF) * 3 +
                           ((argb >> 8) & 0xFF) * 6 + (argb & 0xFF);
          const COLORREF paper =
              luma > 2000 ? RGB(0x3A, 0x3A, 0x3C) : RGB(0xFF, 0xFF, 0xFF);
          ::SetTextColor(reinterpret_cast<HDC>(wparam), ink);
          ::SetBkColor(reinterpret_cast<HDC>(wparam), paper);
          if (!edit_brush_ || edit_paper_ != paper) {
            if (edit_brush_) {
              ::DeleteObject(edit_brush_);
            }
            edit_brush_ = ::CreateSolidBrush(paper);
            edit_paper_ = paper;
          }
          return reinterpret_cast<LRESULT>(edit_brush_);
        }
        break;
      case WM_MEASUREITEM:
        if (PhiMenuOnMeasureItem(hwnd(),
                                 reinterpret_cast<MEASUREITEMSTRUCT*>(lparam))) {
          *handled = true;
          return TRUE;
        }
        break;
      case WM_DRAWITEM:
        if (PhiMenuOnDrawItem(hwnd(),
                              *reinterpret_cast<DRAWITEMSTRUCT*>(lparam),
                              palette())) {
          *handled = true;
          return TRUE;
        }
        break;
      case kMsgCommitText:
        *handled = true;
        HandleCommitMessage();
        return 0;
      case WM_KILLFOCUS:
        // Focus leaving the editor (another app, the page) cancels a drag;
        // the EDIT taking it is ours.
        if (reinterpret_cast<HWND>(wparam) != edit_ && drag_ != Drag::kNone) {
          CancelDrag();
        }
        break;
    }
    return PhiPanel::OnMessage(message, wparam, lparam, handled);
  }

  bool OnKey(WPARAM key) override {
    const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
    if (alt) {
      return false;
    }
    if (key == VK_ESCAPE) {
      if (drag_ != Drag::kNone) {
        CancelDrag();
      } else if (tool_ == Tool::kCrop) {
        SetTool(Tool::kSelect);
      } else {
        PhiScreenshotEditor::RequestClose(false);  // May destroy this.
      }
      return true;
    }
    if (key == VK_RETURN && tool_ == Tool::kCrop) {
      ApplyCrop();
      return true;
    }
    if (ctrl) {
      switch (key) {
        case 'Z':
          shift ? Redo() : Undo();
          return true;
        case 'Y':
          Redo();
          return true;
        case 'D':
          DuplicateSelection();
          return true;
        case 'A':
          selection_.clear();
          for (const PhiAnnotation& a : doc_->annotations()) {
            selection_.push_back(a.id);
          }
          Invalidate();
          return true;
        case 'W':
          PhiScreenshotEditor::RequestClose(false);  // May destroy this.
          return true;
        case '0':
          FitZoom();
          Invalidate();
          return true;
        case VK_OEM_PLUS:
        case VK_ADD:
          if (!AdjustSelectionSize(1)) {
            ZoomAround(zoom_ * 1.25, CanvasCentre());
          }
          return true;
        case VK_OEM_MINUS:
        case VK_SUBTRACT:
          if (!AdjustSelectionSize(-1)) {
            ZoomAround(zoom_ / 1.25, CanvasCentre());
          }
          return true;
      }
      return true;
    }
    switch (key) {
      case VK_DELETE:
      case VK_BACK:
        DeleteSelection();
        return true;
      case VK_LEFT:
      case VK_RIGHT:
      case VK_UP:
      case VK_DOWN: {
        const double step = shift ? 10 : 1;
        NudgeSelection(key == VK_LEFT ? -step : key == VK_RIGHT ? step : 0,
                       key == VK_UP ? -step : key == VK_DOWN ? step : 0);
        return true;
      }
    }
    for (Tool t : kTools) {
      if (static_cast<WPARAM>(ToolKey(t)) == key) {
        SetTool(t);
        return true;
      }
    }
    return true;
  }

  void OnNcDestroy() override {
    if (edit_brush_) {
      ::DeleteObject(edit_brush_);
      edit_brush_ = nullptr;
    }
    if (edit_font_) {
      ::DeleteObject(edit_font_);
      edit_font_ = nullptr;
    }
    PhiPanel::OnNcDestroy();
  }

  void OnSize(int, int) override {
    ClampView();
    Invalidate();
  }

 private:
  enum class Drag {
    kNone,
    kDraw,
    kMove,
    kResize,
    kMarquee,
    kCropNew,
    kCropMove,
    kCropResize,
  };

  // --- Geometry -----------------------------------------------------------------
  PhiPointF ToSource(POINT pt) const {
    return {(pt.x - origin_x_) / zoom_, (pt.y - origin_y_) / zoom_};
  }
  PhiPointF ToClient(PhiPointF p) const {
    return {origin_x_ + p.x * zoom_, origin_y_ + p.y * zoom_};
  }
  PhiRectF ToClient(const PhiRectF& r) const {
    const PhiPointF a = ToClient(PhiPointF{r.left, r.top});
    const PhiPointF b = ToClient(PhiPointF{r.right, r.bottom});
    return PhiRectFromPoints(a, b);
  }
  double Tolerance() const { return Scaled(4) / zoom_; }

  // The area the canvas lives in (client px).
  RECT CanvasArea() const {
    RECT rc = {};
    ::GetClientRect(hwnd(), &rc);
    rc.left += Scaled(kMargin);
    rc.top += Scaled(kMargin);
    rc.right -= Scaled(kMargin);
    rc.bottom -= Scaled(kBarHeight + kBarGap + kStripGap + kMargin);
    if (rc.right < rc.left) {
      rc.right = rc.left;
    }
    if (rc.bottom < rc.top) {
      rc.bottom = rc.top;
    }
    return rc;
  }

  POINT CanvasCentre() const {
    const RECT a = CanvasArea();
    return {(a.left + a.right) / 2, (a.top + a.bottom) / 2};
  }

  void FitZoom() {
    const RECT a = CanvasArea();
    const double aw = std::max<LONG>(1, a.right - a.left);
    const double ah = std::max<LONG>(1, a.bottom - a.top);
    const double iw = doc_->image().width;
    const double ih = doc_->image().height;
    const double fit_all = std::min({1.0, aw / iw, ah / ih});
    const double fit_width = std::min(1.0, aw / iw);
    // A long page fits its width: the whole of it would be a ribbon.
    zoom_ = fit_all >= fit_width * 0.5 ? fit_all : fit_width;
    zoom_ = std::clamp(zoom_, kMinZoom, kMaxZoom);
    origin_x_ = a.left + (aw - iw * zoom_) / 2;
    origin_y_ = a.top + std::max(0.0, (ah - ih * zoom_) / 2);
    ClampView();
  }

  // Keeps the image reachable: centred along an axis it fits, else its
  // edges no further in than the canvas area's.
  void ClampView() {
    if (!alive()) {
      return;
    }
    const RECT a = CanvasArea();
    const double w = doc_->image().width * zoom_;
    const double h = doc_->image().height * zoom_;
    if (w <= a.right - a.left) {
      origin_x_ = a.left + (a.right - a.left - w) / 2;
    } else {
      origin_x_ = std::clamp(origin_x_, a.right - w, static_cast<double>(a.left));
    }
    if (h <= a.bottom - a.top) {
      origin_y_ = a.top + (a.bottom - a.top - h) / 2;
    } else {
      origin_y_ = std::clamp(origin_y_, a.bottom - h, static_cast<double>(a.top));
    }
  }

  void ZoomAround(double next, POINT pt) {
    next = std::clamp(next, kMinZoom, kMaxZoom);
    const PhiPointF src = ToSource(pt);
    zoom_ = next;
    origin_x_ = pt.x - src.x * zoom_;
    origin_y_ = pt.y - src.y * zoom_;
    ClampView();
    PlaceEdit();
    Invalidate();
  }

  void ScrollBy(int dx_wheel, int dy_wheel) {
    const double step = Scaled(40) * 3 / 120.0;
    origin_x_ += dx_wheel * step;
    origin_y_ += dy_wheel * step;
    ClampView();
    PlaceEdit();
    Invalidate();
  }

  // --- Selection and handles --------------------------------------------------------
  bool Selected(uint64_t id) const {
    return std::find(selection_.begin(), selection_.end(), id) !=
           selection_.end();
  }

  // Handle positions (client px) for the single selected object: eight
  // around a box, two on an arrow's ends, left / right on text.
  std::vector<PhiPointF> Handles(const PhiAnnotation& a) const {
    std::vector<PhiPointF> out;
    if (a.kind == PhiAnnotationKind::kArrow) {
      out.push_back(ToClient(a.from));
      out.push_back(ToClient(a.to));
      return out;
    }
    const PhiRectF b = ToClient(PhiAnnotationBox(a));
    const double cx = (b.left + b.right) / 2;
    const double cy = (b.top + b.bottom) / 2;
    if (a.kind == PhiAnnotationKind::kText) {
      out.push_back({b.left, cy});
      out.push_back({b.right, cy});
      return out;
    }
    out = {{b.left, b.top},  {cx, b.top},     {b.right, b.top},
           {b.right, cy},    {b.right, b.bottom}, {cx, b.bottom},
           {b.left, b.bottom}, {b.left, cy}};
    return out;
  }

  static int HandleAt(const std::vector<PhiPointF>& handles,
                      POINT pt,
                      double reach) {
    for (size_t i = 0; i < handles.size(); ++i) {
      if (std::abs(handles[i].x - pt.x) <= reach &&
          std::abs(handles[i].y - pt.y) <= reach) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  // Box handles 0..7 (TL, T, TR, R, BR, B, BL, L) moved to `p` (source px).
  static PhiRectF MoveBoxHandle(const PhiRectF& box, int handle, PhiPointF p) {
    PhiRectF r = box;
    switch (handle) {
      case 0:
        r.left = p.x;
        r.top = p.y;
        break;
      case 1:
        r.top = p.y;
        break;
      case 2:
        r.right = p.x;
        r.top = p.y;
        break;
      case 3:
        r.right = p.x;
        break;
      case 4:
        r.right = p.x;
        r.bottom = p.y;
        break;
      case 5:
        r.bottom = p.y;
        break;
      case 6:
        r.left = p.x;
        r.bottom = p.y;
        break;
      case 7:
        r.left = p.x;
        break;
    }
    return r;
  }

  LPCWSTR CursorAt() const {
    POINT pt = {};
    ::GetCursorPos(&pt);
    ::ScreenToClient(hwnd(), &pt);
    if (HitTest(pt) >= 0 || pt.y >= chrome_top_) {
      return IDC_HAND;
    }
    switch (tool_) {
      case Tool::kSelect:
        return IDC_ARROW;
      case Tool::kText:
        return IDC_IBEAM;
      default:
        return IDC_CROSS;
    }
  }

  // --- Drags -------------------------------------------------------------------------
  void BeginCanvasDrag(POINT pt) {
    CommitText();
    const PhiPointF p = ToSource(pt);
    start_ = p;
    last_ = p;
    const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    shift_at_start_ = shift;
    if (tool_ == Tool::kCrop) {
      BeginCropDrag(pt, p);
      return;
    }
    if (tool_ == Tool::kSelect) {
      if (selection_.size() == 1) {
        if (const PhiAnnotation* a = doc_->Find(selection_[0])) {
          const int handle = HandleAt(Handles(*a), pt, Scaled(kHandle));
          if (handle >= 0) {
            before_ = doc_->Snapshot();
            resize_original_ = *a;
            handle_ = a->kind == PhiAnnotationKind::kText
                          ? (handle == 0 ? 7 : 3)
                          : handle;
            drag_ = Drag::kResize;
            capture_ = BeginCapture();
            return;
          }
        }
      }
      const uint64_t id = doc_->HitTest(p, Tolerance());
      if (id) {
        if (shift) {
          if (Selected(id)) {
            std::erase(selection_, id);
          } else {
            selection_.push_back(id);
          }
        } else if (!Selected(id)) {
          selection_ = {id};
        }
        if (Selected(id)) {
          before_ = doc_->Snapshot();
          drag_ = Drag::kMove;
          capture_ = BeginCapture();
        }
        Invalidate();
        return;
      }
      if (!shift) {
        selection_.clear();
      }
      marquee_base_ = selection_;
      marquee_ = PhiRectFromPoints(p, p);
      drag_ = Drag::kMarquee;
      capture_ = BeginCapture();
      Invalidate();
      return;
    }
    if (tool_ == Tool::kText) {
      const uint64_t id = doc_->HitTest(p, Tolerance());
      const PhiAnnotation* hit = id ? doc_->Find(id) : nullptr;
      if (hit && hit->kind == PhiAnnotationKind::kText) {
        BeginText(id, doc_->Snapshot());
        return;
      }
      PhiScreenshotDocument::State before = doc_->Snapshot();
      PhiAnnotation a = NewAnnotation(PhiAnnotationKind::kText);
      const double width =
          std::max(a.font_size * 8, std::min(360.0, doc_->image().width - p.x));
      a.frame = {p.x, p.y - a.font_size * 0.6, p.x + width, 0};
      a.frame.bottom = a.frame.top + painter_.TextHeight(a);
      const uint64_t created = doc_->Add(std::move(a));
      BeginText(created, std::move(before));
      return;
    }
    const std::optional<PhiAnnotationKind> kind = KindFor(tool_);
    if (!kind) {
      return;
    }
    before_ = doc_->Snapshot();
    PhiAnnotation a = NewAnnotation(*kind);
    a.frame = PhiRectFromPoints(p, p);
    a.from = p;
    a.to = p;
    if (*kind == PhiAnnotationKind::kFreehand) {
      a.points.push_back(p);
    }
    drawing_id_ = doc_->Add(std::move(a));
    selection_.clear();
    drag_ = Drag::kDraw;
    capture_ = BeginCapture();
    Invalidate();
  }

  void MoveDrag(POINT pt) {
    const PhiPointF p = ToSource(pt);
    switch (drag_) {
      case Drag::kDraw: {
        PhiAnnotation* a = doc_->Find(drawing_id_);
        if (!a) {
          break;
        }
        if (a->kind == PhiAnnotationKind::kArrow) {
          a->to = p;
        } else if (a->kind == PhiAnnotationKind::kFreehand) {
          const PhiPointF back = a->points.back();
          if (std::hypot(back.x - p.x, back.y - p.y) * zoom_ >= 1.5) {
            a->points.push_back(p);
          }
        } else {
          a->frame = PhiRectFromPoints(start_, p);
        }
        break;
      }
      case Drag::kMove: {
        const double dx = p.x - last_.x;
        const double dy = p.y - last_.y;
        for (uint64_t id : selection_) {
          if (PhiAnnotation* a = doc_->Find(id)) {
            PhiAnnotationMove(a, dx, dy);
          }
        }
        break;
      }
      case Drag::kResize: {
        PhiAnnotation* a = doc_->Find(resize_original_.id);
        if (!a) {
          break;
        }
        *a = resize_original_;
        if (a->kind == PhiAnnotationKind::kArrow) {
          (handle_ == 0 ? a->from : a->to) = p;
        } else {
          const PhiRectF box = PhiAnnotationBox(resize_original_);
          PhiRectF next = MoveBoxHandle(box, handle_, p);
          PhiAnnotationResize(a, box, next);
          if (a->kind == PhiAnnotationKind::kText) {
            a->frame.bottom = a->frame.top + painter_.TextHeight(*a);
          }
        }
        break;
      }
      case Drag::kMarquee: {
        marquee_ = PhiRectFromPoints(start_, p);
        selection_ = marquee_base_;
        for (uint64_t id : doc_->InRect(marquee_)) {
          if (!Selected(id)) {
            selection_.push_back(id);
          }
        }
        break;
      }
      case Drag::kCropNew:
        crop_ = ClampToImage(PhiRectFromPoints(start_, p));
        break;
      case Drag::kCropMove: {
        PhiRectF r = crop_original_;
        double dx = p.x - start_.x;
        double dy = p.y - start_.y;
        dx = std::clamp(dx, -r.left, doc_->image().width - r.right);
        dy = std::clamp(dy, -r.top, doc_->image().height - r.bottom);
        crop_ = PhiRectF{r.left + dx, r.top + dy, r.right + dx, r.bottom + dy};
        break;
      }
      case Drag::kCropResize:
        crop_ = ClampToImage(MoveBoxHandle(crop_original_, handle_, p));
        break;
      case Drag::kNone:
        break;
    }
    last_ = p;
    Invalidate();
  }

  void EndDrag(POINT pt) {
    MoveDrag(pt);
    const Drag drag = drag_;
    drag_ = Drag::kNone;
    capture_.Release();
    switch (drag) {
      case Drag::kDraw: {
        PhiAnnotation* a = doc_->Find(drawing_id_);
        const double min = 2 / zoom_;
        bool keep = a != nullptr;
        if (a) {
          if (a->kind == PhiAnnotationKind::kArrow) {
            keep = std::hypot(a->to.x - a->from.x, a->to.y - a->from.y) >= min;
          } else if (a->kind == PhiAnnotationKind::kFreehand) {
            keep = a->points.size() >= 2;
          } else {
            keep = a->frame.width() >= min && a->frame.height() >= min;
          }
        }
        if (!keep) {
          // A click creates nothing.
          doc_->Restore(std::move(before_));
        } else {
          doc_->Commit(std::move(before_));
          selection_ = {drawing_id_};
          // Freehand stays armed (Mac 6e399950); Shift repeats any tool.
          if (tool_ != Tool::kFreehand && !shift_at_start_ &&
              (::GetKeyState(VK_SHIFT) & 0x8000) == 0) {
            tool_ = Tool::kSelect;
          }
        }
        drawing_id_ = 0;
        break;
      }
      case Drag::kMove:
      case Drag::kResize:
        doc_->Commit(std::move(before_));
        break;
      case Drag::kMarquee:
        marquee_ = {};
        break;
      case Drag::kCropNew:
      case Drag::kCropMove:
      case Drag::kCropResize:
        if (crop_ && (crop_->width() < 1 || crop_->height() < 1)) {
          crop_.reset();
        }
        break;
      case Drag::kNone:
        break;
    }
    Invalidate();
  }

  void CancelDrag() {
    const Drag drag = drag_;
    drag_ = Drag::kNone;
    capture_.Release();
    if (drag == Drag::kDraw || drag == Drag::kMove || drag == Drag::kResize) {
      doc_->Restore(std::move(before_));
      if (drag == Drag::kDraw) {
        drawing_id_ = 0;
        selection_.clear();
      }
    } else if (drag == Drag::kMarquee) {
      marquee_ = {};
      selection_ = marquee_base_;
    } else if (drag == Drag::kCropNew || drag == Drag::kCropMove ||
               drag == Drag::kCropResize) {
      crop_ = drag == Drag::kCropNew ? std::nullopt
                                     : std::optional<PhiRectF>(crop_original_);
    }
    Invalidate();
  }

  PhiRectF ClampToImage(const PhiRectF& r) const {
    const PhiRectF image = {0, 0, static_cast<double>(doc_->image().width),
                            static_cast<double>(doc_->image().height)};
    return PhiRectIntersect(PhiRectFromPoints({r.left, r.top},
                                              {r.right, r.bottom}),
                            image);
  }

  void BeginCropDrag(POINT pt, PhiPointF p) {
    if (crop_) {
      const PhiRectF c = ToClient(*crop_);
      const double cx = (c.left + c.right) / 2;
      const double cy = (c.top + c.bottom) / 2;
      const std::vector<PhiPointF> handles = {
          {c.left, c.top},  {cx, c.top},     {c.right, c.top},
          {c.right, cy},    {c.right, c.bottom}, {cx, c.bottom},
          {c.left, c.bottom}, {c.left, cy}};
      const int handle = HandleAt(handles, pt, Scaled(kHandle));
      crop_original_ = *crop_;
      if (handle >= 0) {
        handle_ = handle;
        drag_ = Drag::kCropResize;
        capture_ = BeginCapture();
        return;
      }
      if (crop_->Contains(p)) {
        drag_ = Drag::kCropMove;
        capture_ = BeginCapture();
        return;
      }
    }
    crop_ = ClampToImage(PhiRectFromPoints(p, p));
    crop_original_ = *crop_;
    drag_ = Drag::kCropNew;
    capture_ = BeginCapture();
    Invalidate();
  }

  // --- Commands -----------------------------------------------------------------------
  PhiAnnotation NewAnnotation(PhiAnnotationKind kind) const {
    PhiAnnotation a;
    a.kind = kind;
    a.color = color_;
    a.stroke = stroke_;
    a.fill = fill_ && (kind == PhiAnnotationKind::kRectangle ||
                       kind == PhiAnnotationKind::kEllipse);
    a.font_size = font_size_;
    return a;
  }

  void SetTool(Tool t) {
    if (drag_ != Drag::kNone) {
      CancelDrag();
    }
    CommitText();
    if (tool_ == Tool::kCrop && t != Tool::kCrop) {
      crop_.reset();
    }
    tool_ = t;
    if (t == Tool::kCrop) {
      selection_.clear();
    }
    Invalidate();
  }

  void Undo() {
    SettleEdits();
    if (doc_->Undo()) {
      PruneSelection();
      Invalidate();
    }
  }

  void Redo() {
    SettleEdits();
    if (doc_->Redo()) {
      PruneSelection();
      Invalidate();
    }
  }

  void PruneSelection() {
    std::erase_if(selection_, [&](uint64_t id) { return !doc_->Find(id); });
  }

  void DeleteSelection() {
    if (selection_.empty()) {
      return;
    }
    PhiScreenshotDocument::State before = doc_->Snapshot();
    doc_->Remove(selection_);
    doc_->Commit(std::move(before));
    selection_.clear();
    Invalidate();
  }

  void DuplicateSelection() {
    if (selection_.empty()) {
      return;
    }
    PhiScreenshotDocument::State before = doc_->Snapshot();
    selection_ = doc_->Duplicate(selection_, 12);
    doc_->Commit(std::move(before));
    Invalidate();
  }

  void NudgeSelection(double dx, double dy) {
    if (selection_.empty()) {
      return;
    }
    PhiScreenshotDocument::State before = doc_->Snapshot();
    for (uint64_t id : selection_) {
      if (PhiAnnotation* a = doc_->Find(id)) {
        PhiAnnotationMove(a, dx, dy);
      }
    }
    doc_->Commit(std::move(before));
    Invalidate();
  }

  bool AdjustSelectionSize(double delta) {
    if (selection_.empty()) {
      return false;
    }
    PhiScreenshotDocument::State before = doc_->Snapshot();
    for (uint64_t id : selection_) {
      if (PhiAnnotation* a = doc_->Find(id)) {
        if (PhiAnnotationAdjustSize(a, delta) &&
            a->kind == PhiAnnotationKind::kText) {
          a->frame.bottom = a->frame.top + painter_.TextHeight(*a);
        }
      }
    }
    doc_->Commit(std::move(before));
    Invalidate();
    return true;
  }

  // A swatch, a size or Fill: the selection (one undo step) and the
  // defaults for what is drawn next.
  void ApplyStyle(std::optional<uint32_t> color,
                  std::optional<double> size,
                  std::optional<bool> fill) {
    // Text being typed commits first, so its session and this style change
    // are two undo steps rather than one nested in the other.
    CommitText();
    const bool text_tool =
        tool_ == Tool::kText || StyleTargetIsText();
    if (color) {
      color_ = *color;
    }
    if (size) {
      (text_tool ? font_size_ : stroke_) = *size;
    }
    if (fill) {
      fill_ = *fill;
    }
    if (!selection_.empty()) {
      PhiScreenshotDocument::State before = doc_->Snapshot();
      for (uint64_t id : selection_) {
        PhiAnnotation* a = doc_->Find(id);
        if (!a || a->kind == PhiAnnotationKind::kBlur) {
          continue;
        }
        if (color) {
          a->color = *color;
        }
        if (size) {
          if (a->kind == PhiAnnotationKind::kText) {
            if (text_tool) {
              a->font_size = *size;
              a->frame.bottom = a->frame.top + painter_.TextHeight(*a);
            }
          } else if (!text_tool) {
            a->stroke = *size;
          }
        }
        if (fill && (a->kind == PhiAnnotationKind::kRectangle ||
                     a->kind == PhiAnnotationKind::kEllipse)) {
          a->fill = *fill;
        }
      }
      doc_->Commit(std::move(before));
    }
    Invalidate();
  }

  // Whether the style strip edits text sizes (a text tool, or a selection
  // made only of text).
  bool StyleTargetIsText() const {
    if (tool_ == Tool::kText) {
      return true;
    }
    if (selection_.empty()) {
      return false;
    }
    for (uint64_t id : selection_) {
      const PhiAnnotation* a = doc_->Find(id);
      if (!a || a->kind != PhiAnnotationKind::kText) {
        return false;
      }
    }
    return true;
  }

  bool StripShown() const {
    if (tool_ == Tool::kCrop || tool_ == Tool::kBlur) {
      return false;
    }
    if (tool_ != Tool::kSelect) {
      return true;
    }
    for (uint64_t id : selection_) {
      const PhiAnnotation* a = doc_->Find(id);
      if (a && a->kind != PhiAnnotationKind::kBlur) {
        return true;
      }
    }
    return false;
  }

  void ApplyCrop() {
    if (!crop_ || crop_->width() < 1 || crop_->height() < 1) {
      return;
    }
    PhiScreenshotDocument::State before = doc_->Snapshot();
    if (doc_->Crop(*crop_)) {
      doc_->Commit(std::move(before));
    }
    crop_.reset();
    tool_ = Tool::kSelect;
    PruneSelection();
    FitZoom();
    Invalidate();
  }

  void Invoke(int key, POINT pt) {
    if (key >= kHitTool && key < kHitTool + static_cast<int>(std::size(kTools))) {
      SetTool(kTools[key - kHitTool]);
      return;
    }
    if (key >= kHitSwatch && key < kHitSwatch + 6) {
      ApplyStyle(kPhiScreenshotSwatches[key - kHitSwatch], std::nullopt,
                 std::nullopt);
      return;
    }
    if (key >= kHitSize && key < kHitSize + 5) {
      const bool text = StyleTargetIsText();
      ApplyStyle(std::nullopt,
                 unit_ * (text ? kPhiScreenshotTextSizes[key - kHitSize]
                               : kPhiScreenshotWidths[key - kHitSize]),
                 std::nullopt);
      return;
    }
    switch (key) {
      case kHitUndo:
        Undo();
        return;
      case kHitRedo:
        Redo();
        return;
      case kHitExport:
        ExportMenu();
        return;
      case kHitFill:
        ApplyStyle(std::nullopt, std::nullopt, !CurrentFill());
        return;
      case kHitZoomOut:
        ZoomAround(zoom_ / 1.25, CanvasCentre());
        return;
      case kHitZoomIn:
        ZoomAround(zoom_ * 1.25, CanvasCentre());
        return;
      case kHitZoomLabel:
        FitZoom();
        PlaceEdit();
        Invalidate();
        return;
      case kHitCropCancel:
        SetTool(Tool::kSelect);
        return;
      case kHitCropApply:
        ApplyCrop();
        return;
    }
  }

  bool CurrentFill() const {
    for (uint64_t id : selection_) {
      const PhiAnnotation* a = doc_->Find(id);
      if (a && (a->kind == PhiAnnotationKind::kRectangle ||
                a->kind == PhiAnnotationKind::kEllipse)) {
        return a->fill;
      }
    }
    return fill_;
  }

  uint32_t CurrentColor() const {
    for (uint64_t id : selection_) {
      const PhiAnnotation* a = doc_->Find(id);
      if (a && a->kind != PhiAnnotationKind::kBlur) {
        return a->color;
      }
    }
    return color_;
  }

  double CurrentSize(bool text) const {
    for (uint64_t id : selection_) {
      const PhiAnnotation* a = doc_->Find(id);
      if (a && a->kind != PhiAnnotationKind::kBlur) {
        return text ? a->font_size : a->stroke;
      }
    }
    return text ? font_size_ : stroke_;
  }

  void ExportMenu() {
    SettleEdits();
    PhiMenuSpec spec;
    spec.items.push_back(PhiMenuItem::Command("png", L"PNG"));
    spec.items.push_back(PhiMenuItem::Command("jpg", L"JPG"));
    spec.items.push_back(PhiMenuItem::Command("pdf", L"PDF"));
    POINT at = {export_rect_.left, export_rect_.top};
    ::ClientToScreen(hwnd(), &at);
    PhiPopupMenuRequest request;
    request.owner = hwnd();
    request.x = at.x;
    request.y = at.y - Scaled(4);
    request.align = TPM_LEFTALIGN | TPM_BOTTOMALIGN;
    const PhiPalette pal = palette();
    request.palette = &pal;
    const PhiMenuAction action = PhiRunPopupMenu(spec, request);
    if (!action.valid() || instance != this) {
      return;
    }
    PhiScreenshotFormat format = PhiScreenshotFormat::kPng;
    PhiScreenshotParseFormat(action.key, &format);
    const wchar_t* ext = PhiScreenshotFormatExt(format);
    std::wstring name = PhiScreenshotFileName(
        title_, PhiStr("browser.screenshot.fileName"), ext);
    std::wstring buffer(MAX_PATH * 4, L'\0');
    std::copy(name.begin(),
              name.begin() + static_cast<ptrdiff_t>(
                                 std::min(name.size(), buffer.size() - 1)),
              buffer.begin());
    std::wstring upper = ext;
    for (wchar_t& c : upper) {
      c = static_cast<wchar_t>(::towupper(c));
    }
    std::wstring filter = upper;
    filter.push_back(L'\0');
    filter += L"*.";
    filter += ext;
    filter.push_back(L'\0');
    filter.push_back(L'\0');
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = shell_->shell_hwnd();
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = ext;
    if (!::GetSaveFileNameW(&ofn) || instance != this) {
      return;
    }
    std::string error;
    if (!ExportTo(format, buffer.c_str(), nullptr, &error)) {
      PhiShowToast(shell_->shell_hwnd(), palette(),
                   PhiStr("browser.screenshot.saveFailed"),
                   PhiStr("browser.screenshot.exportRetry"));
    }
  }

  void ContextMenu(POINT pt) {
    if (drag_ != Drag::kNone || tool_ == Tool::kCrop) {
      return;
    }
    CommitText();
    const uint64_t id = doc_->HitTest(ToSource(pt), Tolerance());
    if (!id) {
      return;
    }
    if (!Selected(id)) {
      selection_ = {id};
      Invalidate();
    }
    PhiMenuSpec spec;
    spec.items.push_back(PhiMenuItem::Command(
        "front", PhiStr("browser.screenshot.bringToFront")));
    spec.items.push_back(
        PhiMenuItem::Command("back", PhiStr("browser.screenshot.sendToBack")));
    spec.items.push_back(PhiMenuItem::Separator());
    spec.items.push_back(PhiMenuItem::Command(
        "duplicate", PhiStr("browser.screenshot.duplicate")));
    spec.items.push_back(
        PhiMenuItem::Command("delete", PhiStr("browser.screenshot.delete")));
    POINT at = pt;
    ::ClientToScreen(hwnd(), &at);
    PhiPopupMenuRequest request;
    request.owner = hwnd();
    request.x = at.x;
    request.y = at.y;
    const PhiPalette pal = palette();
    request.palette = &pal;
    const PhiMenuAction action = PhiRunPopupMenu(spec, request);
    if (!action.valid() || instance != this) {
      return;
    }
    PruneSelection();
    if (action.is("delete")) {
      DeleteSelection();
    } else if (action.is("duplicate")) {
      DuplicateSelection();
    } else if (action.is("front") || action.is("back")) {
      PhiScreenshotDocument::State before = doc_->Snapshot();
      if (action.is("front")) {
        doc_->BringToFront(selection_);
      } else {
        doc_->SendToBack(selection_);
      }
      doc_->Commit(std::move(before));
      Invalidate();
    }
  }

  // --- Text -----------------------------------------------------------------------------
  void BeginText(uint64_t id, PhiScreenshotDocument::State before) {
    CommitText();
    PhiAnnotation* a = doc_->Find(id);
    if (!a) {
      doc_->Restore(std::move(before));
      return;
    }
    text_before_ = std::move(before);
    editing_id_ = id;
    selection_ = {id};
    edit_ = ::CreateWindowExW(
        0, L"EDIT", a->text.c_str(),
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        0, 0, 10, 10, hwnd(), reinterpret_cast<HMENU>(kEditId),
        ::GetModuleHandleW(nullptr), nullptr);
    if (!edit_) {
      editing_id_ = 0;
      doc_->Restore(std::move(text_before_));
      return;
    }
    ::SetWindowSubclass(edit_, &Editor::EditProc, kEditSubclass,
                        reinterpret_cast<DWORD_PTR>(this));
    RefreshEditFont();
    PlaceEdit();
    ::SetFocus(edit_);
    ::SendMessageW(edit_, EM_SETSEL, 0, -1);
    Invalidate();
  }

  void RefreshEditFont() {
    const PhiAnnotation* a = doc_->Find(editing_id_);
    if (!edit_ || !a) {
      return;
    }
    if (edit_font_) {
      ::DeleteObject(edit_font_);
    }
    LOGFONTW lf = {};
    lf.lfHeight = -static_cast<LONG>(std::lround(a->font_size * zoom_));
    lf.lfWeight = FW_NORMAL;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    ::wcscpy_s(lf.lfFaceName, L"Segoe UI");
    edit_font_ = ::CreateFontIndirectW(&lf);
    ::SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_),
                   TRUE);
    ::SendMessageW(edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
  }

  void PlaceEdit() {
    const PhiAnnotation* a = doc_->Find(editing_id_);
    if (!edit_ || !a) {
      return;
    }
    const PhiRectF r = ToClient(a->frame);
    ::SetWindowPos(edit_, nullptr, static_cast<int>(std::floor(r.left)),
                   static_cast<int>(std::floor(r.top)),
                   std::max(8, static_cast<int>(std::ceil(r.width()))),
                   std::max(8, static_cast<int>(std::ceil(r.height()))),
                   SWP_NOZORDER | SWP_NOACTIVATE);
  }

  void OnTextChanged() {
    PhiAnnotation* a = doc_->Find(editing_id_);
    if (!edit_ || !a) {
      return;
    }
    const int length = ::GetWindowTextLengthW(edit_);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    ::GetWindowTextW(edit_, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    std::erase(text, L'\r');
    a->text = std::move(text);
    a->frame.bottom = a->frame.top + painter_.TextHeight(*a);
    PlaceEdit();
  }

  void CommitText() {
    if (!edit_) {
      return;
    }
    OnTextChanged();
    const uint64_t id = editing_id_;
    DestroyEdit();
    const PhiAnnotation* a = doc_->Find(id);
    if (a && a->text.empty()) {
      doc_->Remove({id});
      std::erase(selection_, id);
    }
    doc_->Commit(std::move(text_before_));
    if (tool_ == Tool::kText && (::GetKeyState(VK_SHIFT) & 0x8000) == 0) {
      tool_ = Tool::kSelect;
    }
    if (::GetFocus() == nullptr) {
      ::SetFocus(hwnd());
    }
    Invalidate();
  }

  void CancelText() {
    if (!edit_) {
      return;
    }
    DestroyEdit();
    doc_->Restore(std::move(text_before_));
    PruneSelection();
    ::SetFocus(hwnd());
    Invalidate();
  }

  void DestroyEdit() {
    HWND edit = edit_;
    edit_ = nullptr;
    editing_id_ = 0;
    if (edit) {
      ::RemoveWindowSubclass(edit, &Editor::EditProc, kEditSubclass);
      const bool had_focus = ::GetFocus() == edit;
      ::DestroyWindow(edit);
      if (had_focus) {
        ::SetFocus(hwnd());
      }
    }
  }

  static LRESULT CALLBACK EditProc(HWND hwnd,
                                   UINT message,
                                   WPARAM wparam,
                                   LPARAM lparam,
                                   UINT_PTR,
                                   DWORD_PTR data) {
    auto* self = reinterpret_cast<Editor*>(data);
    if (message == WM_KEYDOWN) {
      const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
      if (wparam == VK_ESCAPE) {
        self->CancelText();
        return 0;
      }
      if (wparam == VK_RETURN && ctrl) {
        self->CommitText();
        return 0;
      }
      if (ctrl && wparam == 'A') {
        ::SendMessageW(hwnd, EM_SETSEL, 0, -1);
        return 0;
      }
    }
    if (message == WM_CHAR && (wparam == 0x1B || wparam == 0x0A ||
                               wparam == 0x01)) {
      return 0;  // Escape, Ctrl+Return and Ctrl+A were keys, not text.
    }
    if (message == WM_KILLFOCUS && self->edit_ == hwnd) {
      // A click elsewhere, another window: the text commits (posted, as
      // destroying the field inside its own focus change is not safe).
      ::PostMessageW(self->hwnd(), kMsgCommitText, 0, 0);
    }
    if (message == WM_GETDLGCODE) {
      return DLGC_WANTALLKEYS | DLGC_HASSETSEL | DLGC_WANTCHARS;
    }
    return ::DefSubclassProc(hwnd, message, wparam, lparam);
  }

  static constexpr UINT kMsgCommitText = WM_APP + 71;

 public:
  void HandleCommitMessage() {
    if (edit_ && ::GetFocus() != edit_) {
      CommitText();
    }
  }

 private:
  // --- Painting helpers --------------------------------------------------------------
  bool EnsureTarget() {
    if (target_) {
      return true;
    }
    if (!painter_.ok()) {
      return false;
    }
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        96.0f, 96.0f);
    if (FAILED(painter_.factory()->CreateDCRenderTarget(&props, &target_))) {
      return false;
    }
    bitmap_.Reset();
    bitmap_image_ = nullptr;
    return true;
  }

  void PaintSelection(ID2D1SolidColorBrush* brush) {
    const D2D1_COLOR_F accent = Ink(palette().accent);
    for (uint64_t id : selection_) {
      const PhiAnnotation* a = doc_->Find(id);
      if (!a || id == editing_id_) {
        continue;
      }
      const PhiRectF b = ToClient(PhiAnnotationBounds(*a));
      brush->SetColor(accent);
      target_->DrawRectangle(
          D2D1::RectF(static_cast<float>(b.left) - 2, static_cast<float>(b.top) - 2,
                      static_cast<float>(b.right) + 2,
                      static_cast<float>(b.bottom) + 2),
          brush, 1.0f);
      if (selection_.size() == 1) {
        for (const PhiPointF& h : Handles(*a)) {
          const float r = static_cast<float>(Scaled(kHandle) / 2);
          const D2D1_ELLIPSE dot = D2D1::Ellipse(
              D2D1::Point2F(static_cast<float>(h.x), static_cast<float>(h.y)),
              r, r);
          brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
          target_->FillEllipse(dot, brush);
          brush->SetColor(accent);
          target_->DrawEllipse(dot, brush, 1.5f);
        }
      }
    }
    if (drag_ == Drag::kMarquee) {
      const PhiRectF m = ToClient(marquee_);
      const D2D1_RECT_F r = D2D1::RectF(
          static_cast<float>(m.left), static_cast<float>(m.top),
          static_cast<float>(m.right), static_cast<float>(m.bottom));
      brush->SetColor(Ink(palette().accent, 0.12f));
      target_->FillRectangle(r, brush);
      brush->SetColor(accent);
      target_->DrawRectangle(r, brush, 1.0f);
    }
  }

  void PaintCrop(ID2D1SolidColorBrush* brush) {
    if (tool_ != Tool::kCrop || !crop_) {
      return;
    }
    const PhiRectF c = ToClient(*crop_);
    const PhiRectF page = ToClient(PhiRectF{
        0, 0, static_cast<double>(doc_->image().width),
        static_cast<double>(doc_->image().height)});
    auto f = [](double v) { return static_cast<float>(v); };
    brush->SetColor(D2D1::ColorF(0, 0, 0, 0.5f));
    // The dimmed exterior, as four bands.
    target_->FillRectangle(
        D2D1::RectF(f(page.left), f(page.top), f(page.right), f(c.top)), brush);
    target_->FillRectangle(
        D2D1::RectF(f(page.left), f(c.bottom), f(page.right), f(page.bottom)),
        brush);
    target_->FillRectangle(
        D2D1::RectF(f(page.left), f(c.top), f(c.left), f(c.bottom)), brush);
    target_->FillRectangle(
        D2D1::RectF(f(c.right), f(c.top), f(page.right), f(c.bottom)), brush);
    // Thirds.
    brush->SetColor(D2D1::ColorF(1, 1, 1, 0.55f));
    for (int i = 1; i < 3; ++i) {
      const double x = c.left + c.width() * i / 3;
      const double y = c.top + c.height() * i / 3;
      target_->DrawLine(D2D1::Point2F(f(x), f(c.top)),
                        D2D1::Point2F(f(x), f(c.bottom)), brush, 1.0f);
      target_->DrawLine(D2D1::Point2F(f(c.left), f(y)),
                        D2D1::Point2F(f(c.right), f(y)), brush, 1.0f);
    }
    brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
    target_->DrawRectangle(
        D2D1::RectF(f(c.left), f(c.top), f(c.right), f(c.bottom)), brush, 1.5f);
    const double cx = (c.left + c.right) / 2;
    const double cy = (c.top + c.bottom) / 2;
    const PhiPointF handles[] = {{c.left, c.top},  {cx, c.top},
                                 {c.right, c.top}, {c.right, cy},
                                 {c.right, c.bottom}, {cx, c.bottom},
                                 {c.left, c.bottom}, {c.left, cy}};
    const float s = static_cast<float>(Scaled(kHandle) / 2);
    for (const PhiPointF& h : handles) {
      target_->FillRectangle(
          D2D1::RectF(f(h.x) - s, f(h.y) - s, f(h.x) + s, f(h.y) + s), brush);
    }
  }

  ComPtr<IDWriteTextFormat> Format(float size, bool semibold, bool icon) {
    ComPtr<IDWriteTextFormat> format;
    painter_.dwrite()->CreateTextFormat(
        icon ? PhiIconFaceName() : L"Segoe UI", nullptr,
        semibold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"",
        &format);
    if (format) {
      format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return format;
  }

  void Text(ID2D1SolidColorBrush* brush,
            const std::wstring& text,
            const RECT& box,
            float size,
            bool semibold,
            bool icon = false) {
    ComPtr<IDWriteTextFormat> format = Format(size, semibold, icon);
    if (format) {
      target_->DrawText(text.c_str(), static_cast<UINT32>(text.size()),
                        format.Get(), Rf(box), brush);
    }
  }

  float TextWidth(const std::wstring& text, float size, bool semibold) {
    ComPtr<IDWriteTextFormat> format = Format(size, semibold, false);
    ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS m = {};
    if (!format ||
        FAILED(painter_.dwrite()->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), format.Get(),
            10000.0f, 100.0f, &layout)) ||
        FAILED(layout->GetMetrics(&m))) {
      return size * text.size() * 0.6f;
    }
    return m.widthIncludingTrailingWhitespace;
  }

  // The tool glyphs, drawn as vectors in `box` (they have no common font
  // glyphs on both Windows 10 and 11).
  void ToolIcon(ID2D1SolidColorBrush* brush, Tool t, const RECT& box) {
    const float cx = (box.left + box.right) / 2.0f;
    const float cy = (box.top + box.bottom) / 2.0f;
    const float u = static_cast<float>(Scaled(1));
    const float sw = 1.6f * u;
    auto pt = [&](float x, float y) { return D2D1::Point2F(cx + x * u, cy + y * u); };
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    auto open = [&]() {
      path.Reset();
      sink.Reset();
      painter_.factory()->CreatePathGeometry(&path);
      if (path) {
        path->Open(&sink);
      }
      return sink != nullptr;
    };
    switch (t) {
      case Tool::kSelect:
        if (open()) {
          sink->BeginFigure(pt(-4, -7), D2D1_FIGURE_BEGIN_FILLED);
          sink->AddLine(pt(-4, 6));
          sink->AddLine(pt(-0.8f, 3));
          sink->AddLine(pt(1.6f, 8));
          sink->AddLine(pt(3.4f, 7.2f));
          sink->AddLine(pt(1, 2.2f));
          sink->AddLine(pt(5.4f, 2.2f));
          sink->EndFigure(D2D1_FIGURE_END_CLOSED);
          sink->Close();
          target_->DrawGeometry(path.Get(), brush, sw);
        }
        return;
      case Tool::kArrow:
        target_->DrawLine(pt(-6, 6), pt(6, -6), brush, sw);
        target_->DrawLine(pt(6, -6), pt(0, -6), brush, sw);
        target_->DrawLine(pt(6, -6), pt(6, 0), brush, sw);
        return;
      case Tool::kFreehand:
        if (open()) {
          sink->BeginFigure(pt(-7, 4), D2D1_FIGURE_BEGIN_HOLLOW);
          sink->AddBezier(D2D1::BezierSegment(pt(-4, -6), pt(-1, -6), pt(0, 0)));
          sink->AddBezier(D2D1::BezierSegment(pt(1, 6), pt(4, 6), pt(7, -4)));
          sink->EndFigure(D2D1_FIGURE_END_OPEN);
          sink->Close();
          target_->DrawGeometry(path.Get(), brush, sw);
        }
        return;
      case Tool::kText:
        target_->DrawLine(pt(-6, -6), pt(6, -6), brush, sw * 1.2f);
        target_->DrawLine(pt(0, -6), pt(0, 7), brush, sw * 1.2f);
        return;
      case Tool::kRectangle:
        target_->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(cx - 7 * u, cy - 5.5f * u,
                                          cx + 7 * u, cy + 5.5f * u),
                              1.5f * u, 1.5f * u),
            brush, sw);
        return;
      case Tool::kEllipse:
        target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 7 * u, 5.5f * u),
                             brush, sw);
        return;
      case Tool::kBlur:
        for (int y = 0; y < 3; ++y) {
          for (int x = 0; x < 3; ++x) {
            const float l = cx + (-6 + x * 4) * u;
            const float tp = cy + (-6 + y * 4) * u;
            if ((x + y) % 2 == 0) {
              target_->FillRectangle(D2D1::RectF(l, tp, l + 4 * u, tp + 4 * u),
                                     brush);
            } else {
              target_->DrawRectangle(
                  D2D1::RectF(l + 0.5f * u, tp + 0.5f * u, l + 3.5f * u,
                              tp + 3.5f * u),
                  brush, u);
            }
          }
        }
        return;
      case Tool::kCrop:
        target_->DrawLine(pt(-4, -8), pt(-4, 4), brush, sw);
        target_->DrawLine(pt(-4, 4), pt(8, 4), brush, sw);
        target_->DrawLine(pt(-8, -4), pt(4, -4), brush, sw);
        target_->DrawLine(pt(4, -4), pt(4, 8), brush, sw);
        return;
    }
  }

  // One round toolbar button: its wash, then `draw`, then its hit.
  template <typename Draw>
  void Button(ID2D1SolidColorBrush* brush,
              const RECT& box,
              int key,
              const std::wstring& name,
              bool selected,
              bool enabled,
              Draw draw) {
    const PhiPalette& p = palette();
    const int index = static_cast<int>(hits_.size());
    const bool hover = hovered() == index && enabled;
    if (selected) {
      brush->SetColor(Ink(p.accent));
      target_->FillRoundedRectangle(
          D2D1::RoundedRect(Rf(box), Scaled(8), Scaled(8)), brush);
    } else if (hover) {
      brush->SetColor(Ink(p.ink, 0.08f));
      target_->FillRoundedRectangle(
          D2D1::RoundedRect(Rf(box), Scaled(8), Scaled(8)), brush);
    }
    brush->SetColor(selected ? Ink(p.on_accent)
                             : Ink(p.label, enabled ? 1.0f : 0.35f));
    draw();
    AddHit(box, key, name, selected ? PhiHitRole::kToggle : PhiHitRole::kButton,
           selected, enabled);
  }

  void PaintChrome(ID2D1SolidColorBrush* brush) {
    const PhiPalette& p = palette();
    RECT rc = {};
    ::GetClientRect(hwnd(), &rc);
    const int b = Scaled(kButton);
    const int pad = Scaled(kBarPad);
    const int sep = Scaled(kSeparator);
    const bool crop = tool_ == Tool::kCrop;
    std::wstring crop_label;
    int width = 0;
    if (crop) {
      crop_label =
          crop_ ? std::to_wstring(static_cast<int>(std::lround(crop_->width()))) +
                      L" × " +
                      std::to_wstring(static_cast<int>(std::lround(crop_->height())))
                : PhiStr("browser.screenshot.cropHint");
      const float fs = static_cast<float>(Scaled(13));
      crop_w_label_ = static_cast<int>(TextWidth(crop_label, fs, false)) +
                      Scaled(20);
      crop_w_cancel_ = static_cast<int>(TextWidth(
                           PhiStr("browser.screenshot.cancelCrop"), fs, false)) +
                       Scaled(28);
      crop_w_apply_ = static_cast<int>(TextWidth(
                          PhiStr("browser.screenshot.applyCrop"), fs, true)) +
                      Scaled(28);
      width = pad * 2 + crop_w_label_ + crop_w_cancel_ + crop_w_apply_ +
              Scaled(6);
    } else {
      width = pad * 2 + b * 11 + sep * 2;
    }
    const int bar_h = Scaled(kBarHeight);
    const int left = (rc.right - width) / 2;
    const int top = rc.bottom - Scaled(kBarGap) - bar_h;
    chrome_top_ = top;
    const RECT bar = {left, top, left + width, top + bar_h};
    // The capsule: the flyout ground with a hairline.
    const D2D1_ROUNDED_RECT capsule =
        D2D1::RoundedRect(Rf(bar), bar_h / 2.0f, bar_h / 2.0f);
    brush->SetColor(D2D1::ColorF(0, 0, 0, 0.12f));
    target_->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(bar.left, bar.top + Scaled(2),
                                      bar.right, bar.bottom + Scaled(2)),
                          bar_h / 2.0f, bar_h / 2.0f),
        brush);
    brush->SetColor(Ink(p.dark ? PhiRgb(0x2C, 0x2C, 0x2E)
                               : PhiRgb(0xFB, 0xFB, 0xFB)));
    target_->FillRoundedRectangle(capsule, brush);
    brush->SetColor(Ink(p.ink, 0.10f));
    target_->DrawRoundedRectangle(capsule, brush, 1.0f);

    int x = left + pad;
    const int y = top + pad;
    if (crop) {
      const float fs = static_cast<float>(Scaled(13));
      brush->SetColor(Ink(p.label_dim));
      Text(brush, crop_label, RECT{x, y, x + crop_w_label_, y + b}, fs, false);
      x += crop_w_label_;
      const RECT cancel = {x, y, x + crop_w_cancel_, y + b};
      Button(brush, cancel, kHitCropCancel,
             PhiStr("browser.screenshot.cancelCrop"), false, true, [&] {
               Text(brush, PhiStr("browser.screenshot.cancelCrop"), cancel, fs,
                    false);
             });
      x += crop_w_cancel_ + Scaled(6);
      const RECT apply = {x, y, x + crop_w_apply_, y + b};
      const bool can_apply = crop_ && crop_->width() >= 1 &&
                             crop_->height() >= 1 && drag_ == Drag::kNone;
      Button(brush, apply, kHitCropApply, PhiStr("browser.screenshot.applyCrop"),
             can_apply, can_apply, [&] {
               Text(brush, PhiStr("browser.screenshot.applyCrop"), apply, fs,
                    true);
             });
    } else {
      for (size_t i = 0; i < std::size(kTools); ++i) {
        const Tool t = kTools[i];
        const RECT box = {x, y, x + b, y + b};
        std::wstring name = PhiStr(ToolLabelKey(t));
        name += L" (";
        name.push_back(ToolKey(t));
        name += L")";
        Button(brush, box, kHitTool + static_cast<int>(i), name, tool_ == t,
               true, [&] { ToolIcon(brush, t, box); });
        x += b;
      }
      x += sep / 2;
      brush->SetColor(Ink(p.ink, 0.12f));
      target_->DrawLine(D2D1::Point2F(x + 0.5f, y + Scaled(7)),
                        D2D1::Point2F(x + 0.5f, y + b - Scaled(7)), brush, 1.0f);
      x += sep - sep / 2;
      const float glyph = static_cast<float>(Scaled(15));
      const RECT undo = {x, y, x + b, y + b};
      Button(brush, undo, kHitUndo, PhiStr("browser.screenshot.undo"), false,
             doc_->CanUndo(),
             [&] { Text(brush, L"", undo, glyph, false, true); });
      x += b;
      const RECT redo = {x, y, x + b, y + b};
      Button(brush, redo, kHitRedo, PhiStr("browser.screenshot.redo"), false,
             doc_->CanRedo(),
             [&] { Text(brush, L"", redo, glyph, false, true); });
      x += b;
      x += sep / 2;
      brush->SetColor(Ink(p.ink, 0.12f));
      target_->DrawLine(D2D1::Point2F(x + 0.5f, y + Scaled(7)),
                        D2D1::Point2F(x + 0.5f, y + b - Scaled(7)), brush, 1.0f);
      x += sep - sep / 2;
      export_rect_ = {x, y, x + b, y + b};
      Button(brush, export_rect_, kHitExport, PhiStr("browser.screenshot.export"),
             false, true,
             [&] { Text(brush, L"", export_rect_, glyph, false, true); });
      if (doc_->dirty()) {
        // An unsaved-edits dot on Export.
        brush->SetColor(Ink(p.accent));
        target_->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(export_rect_.right - Scaled(7),
                                        export_rect_.top + Scaled(7)),
                          Scaled(3), Scaled(3)),
            brush);
      }
      if (StripShown()) {
        PaintStrip(brush, bar);
      }
    }
    PaintZoom(brush, rc);
  }

  void PaintStrip(ID2D1SolidColorBrush* brush, const RECT& bar) {
    const PhiPalette& p = palette();
    const int sw = Scaled(28);
    const int pad = Scaled(kBarPad);
    const bool text = StyleTargetIsText();
    bool fillable = tool_ == Tool::kRectangle || tool_ == Tool::kEllipse;
    for (uint64_t id : selection_) {
      const PhiAnnotation* a = doc_->Find(id);
      fillable = fillable || (a && (a->kind == PhiAnnotationKind::kRectangle ||
                                    a->kind == PhiAnnotationKind::kEllipse));
    }
    const int width = pad * 2 + sw * 6 + Scaled(kSeparator) + sw * 5 +
                      (fillable ? Scaled(kSeparator) + Scaled(52) : 0);
    const int h = sw + pad * 2;
    const int left = (bar.left + bar.right - width) / 2;
    const int top = bar.top - Scaled(kStripGap) - h;
    chrome_top_ = top;
    const RECT strip = {left, top, left + width, top + h};
    const D2D1_ROUNDED_RECT capsule =
        D2D1::RoundedRect(Rf(strip), h / 2.0f, h / 2.0f);
    brush->SetColor(Ink(p.dark ? PhiRgb(0x2C, 0x2C, 0x2E)
                               : PhiRgb(0xFB, 0xFB, 0xFB)));
    target_->FillRoundedRectangle(capsule, brush);
    brush->SetColor(Ink(p.ink, 0.10f));
    target_->DrawRoundedRectangle(capsule, brush, 1.0f);
    int x = left + pad;
    const int y = top + pad;
    const uint32_t current = CurrentColor();
    for (int i = 0; i < 6; ++i) {
      const RECT box = {x, y, x + sw, y + sw};
      const uint32_t c = kPhiScreenshotSwatches[i];
      Button(brush, box, kHitSwatch + i, PhiStr("browser.screenshot.color"),
             false, true, [&] {
               const D2D1_ELLIPSE dot = D2D1::Ellipse(
                   D2D1::Point2F((box.left + box.right) / 2.0f,
                                 (box.top + box.bottom) / 2.0f),
                   Scaled(8), Scaled(8));
               brush->SetColor(Argb(c));
               target_->FillEllipse(dot, brush);
               brush->SetColor(Ink(p.ink, 0.25f));
               target_->DrawEllipse(dot, brush, 1.0f);
               if (c == current) {
                 brush->SetColor(Ink(p.accent));
                 target_->DrawEllipse(
                     D2D1::Ellipse(dot.point, Scaled(11), Scaled(11)), brush,
                     2.0f);
               }
             });
      x += sw;
    }
    x += Scaled(kSeparator) / 2;
    brush->SetColor(Ink(p.ink, 0.12f));
    target_->DrawLine(D2D1::Point2F(x + 0.5f, y + Scaled(5)),
                      D2D1::Point2F(x + 0.5f, y + sw - Scaled(5)), brush, 1.0f);
    x += Scaled(kSeparator) - Scaled(kSeparator) / 2;
    const double size = CurrentSize(text);
    for (int i = 0; i < 5; ++i) {
      const RECT box = {x, y, x + sw, y + sw};
      const double value =
          text ? kPhiScreenshotTextSizes[i] : kPhiScreenshotWidths[i];
      // The Mac's numbers on the buttons; what they apply is scaled.
      const bool on = std::abs(value * unit_ - size) < 0.01;
      Button(brush, box, kHitSize + i,
             PhiStr(text ? "browser.screenshot.textSize"
                         : "browser.screenshot.width"),
             on, true, [&] {
               if (text) {
                 Text(brush, std::to_wstring(static_cast<int>(value)), box,
                      static_cast<float>(Scaled(11)), true);
               } else {
                 const float r = static_cast<float>(
                     std::min(Scaled(10), Scaled(1) + static_cast<int>(
                                                          value * Scaled(1) / 3)));
                 target_->FillEllipse(
                     D2D1::Ellipse(D2D1::Point2F((box.left + box.right) / 2.0f,
                                                 (box.top + box.bottom) / 2.0f),
                                   r, r),
                     brush);
               }
             });
      x += sw;
    }
    if (fillable) {
      x += Scaled(kSeparator) / 2;
      brush->SetColor(Ink(p.ink, 0.12f));
      target_->DrawLine(D2D1::Point2F(x + 0.5f, y + Scaled(5)),
                        D2D1::Point2F(x + 0.5f, y + sw - Scaled(5)), brush, 1.0f);
      x += Scaled(kSeparator) - Scaled(kSeparator) / 2;
      const RECT box = {x, y, x + Scaled(52), y + sw};
      Button(brush, box, kHitFill, PhiStr("browser.screenshot.fill"),
             CurrentFill(), true, [&] {
               Text(brush, PhiStr("browser.screenshot.fill"), box,
                    static_cast<float>(Scaled(12)), false);
             });
    }
  }

  void PaintZoom(ID2D1SolidColorBrush* brush, const RECT& rc) {
    const PhiPalette& p = palette();
    const int b = Scaled(28);
    const int label_w = Scaled(52);
    const int pad = Scaled(4);
    const int width = pad * 2 + b * 2 + label_w;
    const int h = b + pad * 2;
    const int right = rc.right - Scaled(kBarGap);
    const int bottom = rc.bottom - Scaled(kBarGap) -
                       (Scaled(kBarHeight) - h) / 2;
    zoom_rect_ = {right - width, bottom - h, right, bottom};
    const D2D1_ROUNDED_RECT capsule =
        D2D1::RoundedRect(Rf(zoom_rect_), h / 2.0f, h / 2.0f);
    brush->SetColor(Ink(p.dark ? PhiRgb(0x2C, 0x2C, 0x2E)
                               : PhiRgb(0xFB, 0xFB, 0xFB)));
    target_->FillRoundedRectangle(capsule, brush);
    brush->SetColor(Ink(p.ink, 0.10f));
    target_->DrawRoundedRectangle(capsule, brush, 1.0f);
    int x = zoom_rect_.left + pad;
    const int y = zoom_rect_.top + pad;
    const float glyph = static_cast<float>(Scaled(12));
    const RECT out = {x, y, x + b, y + b};
    Button(brush, out, kHitZoomOut, PhiStr("browser.screenshot.zoomOut"), false,
           zoom_ > kMinZoom, [&] { Text(brush, L"", out, glyph, false, true); });
    x += b;
    const RECT label = {x, y, x + label_w, y + b};
    Button(brush, label, kHitZoomLabel, PhiStr("browser.screenshot.fit"), false,
           true, [&] {
             Text(brush,
                  std::to_wstring(static_cast<int>(std::lround(zoom_ * 100))) +
                      L"%",
                  label, static_cast<float>(Scaled(12)), false);
           });
    x += label_w;
    const RECT in = {x, y, x + b, y + b};
    Button(brush, in, kHitZoomIn, PhiStr("browser.screenshot.zoomIn"), false,
           zoom_ < kMaxZoom, [&] { Text(brush, L"", in, glyph, false, true); });
  }

  RAW_PTR_EXCLUSION PhiShellWindow* shell_;
  std::wstring title_;
  std::unique_ptr<PhiScreenshotDocument> doc_;
  // PhiScreenshotStyleUnit of the capture: the Mac's sizes times this.
  double unit_ = 1;
  PhiScreenshotPainter painter_;
  ComPtr<ID2D1DCRenderTarget> target_;
  ComPtr<ID2D1Bitmap> bitmap_;
  PhiScreenshotImageRef bitmap_image_;

  double zoom_ = 1;
  double origin_x_ = 0;
  double origin_y_ = 0;

  Tool tool_ = Tool::kSelect;
  uint32_t color_ = kPhiScreenshotSwatches[0];
  double stroke_;
  double font_size_;
  bool fill_ = false;

  std::vector<uint64_t> selection_;
  Drag drag_ = Drag::kNone;
  Capture capture_;
  PhiScreenshotDocument::State before_;
  PhiPointF start_;
  PhiPointF last_;
  bool shift_at_start_ = false;
  uint64_t drawing_id_ = 0;
  int handle_ = -1;
  PhiAnnotation resize_original_;
  PhiRectF marquee_;
  std::vector<uint64_t> marquee_base_;
  std::optional<PhiRectF> crop_;
  PhiRectF crop_original_;

  HWND edit_ = nullptr;
  uint64_t editing_id_ = 0;
  PhiScreenshotDocument::State text_before_;
  HFONT edit_font_ = nullptr;
  HBRUSH edit_brush_ = nullptr;
  COLORREF edit_paper_ = 0;

  int pressed_hit_ = -1;
  int chrome_top_ = 0;
  RECT export_rect_ = {};
  RECT zoom_rect_ = {};
  int crop_w_label_ = 0;
  int crop_w_cancel_ = 0;
  int crop_w_apply_ = 0;
};

Editor* Editor::instance = nullptr;

void DestroyEditor() {
  Editor* gone = Editor::instance;
  if (!gone) {
    return;
  }
  Editor::instance = nullptr;
  HWND shell = gone->shell() ? gone->shell()->shell_hwnd() : nullptr;
  if (gone->alive()) {
    gone->Destroy();
  } else {
    delete gone;
  }
  // The page gets the keyboard back.
  if (PhiShellWindow* s = shell ? PhiShellWindow::FromHwnd(shell) : nullptr) {
    s->FocusPageBrowser();
  }
}

}  // namespace

// static
bool PhiScreenshotEditor::Open(PhiShellWindow* shell,
                               PhiScreenshotImageRef image,
                               const std::wstring& title,
                               double device_pixel_ratio) {
  if (!shell || !image) {
    return false;
  }
  if (Editor::instance) {
    if (!Editor::instance->ConfirmDiscard()) {
      return false;
    }
    DestroyEditor();
  }
  auto* editor = new Editor(shell, std::move(image), title, device_pixel_ratio);
  if (!editor->alive()) {
    delete editor;
    return false;
  }
  Editor::instance = editor;
  return true;
}

// static
bool PhiScreenshotEditor::IsOpen() {
  return Editor::instance != nullptr;
}

// static
bool PhiScreenshotEditor::OwnsFocus() {
  return Editor::instance && Editor::instance->alive() &&
         Editor::instance->OwnsFocus();
}

// static
bool PhiScreenshotEditor::RequestClose(bool discard) {
  if (!Editor::instance) {
    return true;
  }
  if (!discard && !Editor::instance->ConfirmDiscard()) {
    return false;
  }
  DestroyEditor();
  return true;
}

// static
void PhiScreenshotEditor::CloseIfBoundTo(PhiShellWindow* shell) {
  if (Editor::instance && Editor::instance->shell() == shell) {
    // The window is going: nothing can ask any more.
    Editor* gone = Editor::instance;
    Editor::instance = nullptr;
    if (gone->alive()) {
      gone->Destroy();
    } else {
      delete gone;
    }
  }
}

// static
void PhiScreenshotEditor::RepositionIfBoundTo(PhiShellWindow* shell) {
  if (Editor::instance && Editor::instance->shell() == shell) {
    Editor::instance->Reposition();
  }
}

// static
void PhiScreenshotEditor::RaiseIfBoundTo(PhiShellWindow* shell) {
  if (Editor::instance && Editor::instance->shell() == shell) {
    Editor::instance->Raise();
  }
}

// static
bool PhiScreenshotEditor::IsBoundTo(PhiShellWindow* shell) {
  return Editor::instance && Editor::instance->shell() == shell &&
         Editor::instance->alive();
}

// static
bool PhiScreenshotEditor::FocusIfBoundTo(PhiShellWindow* shell) {
  if (!IsBoundTo(shell)) {
    return false;
  }
  Editor::instance->TakeFocus();
  return true;
}

// static
base::DictValue PhiScreenshotEditor::StateForTest() {
  if (!Editor::instance) {
    base::DictValue out;
    out.Set("open", false);
    return out;
  }
  return Editor::instance->State();
}

// static
bool PhiScreenshotEditor::ExportForTest(const std::string& format,
                                        const std::wstring& path,
                                        base::DictValue* out,
                                        std::string* error) {
  if (!Editor::instance) {
    *error = "no_editor";
    return false;
  }
  PhiScreenshotFormat f = PhiScreenshotFormat::kPng;
  if (!PhiScreenshotParseFormat(format, &f)) {
    *error = "bad_format";
    return false;
  }
  return Editor::instance->ExportTo(f, path, out, error);
}

}  // namespace phi_client

// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_WINDOW_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"

#if defined(_WIN32)
#include <windows.h>

#include "client/phi_theme.h"
#endif

// THE window substrate (docs/architecture-review.md §3.6, appendix
// lane3-ui-substrate.md §6.1). Every client surface that owns an HWND
// derives from PhiWindow; the three families that sit on top of it
// (PhiPopover here, PhiPanel and PhiSecondaryWindow in later phases) add
// only their placement and dismissal policy. Nothing else in the client
// may call RegisterClass*, bind `this` into GWLP_USERDATA, allocate a
// timer id, take the capture, or write a raw double-buffer block again.
//
// CONTRACT FOR THE FAMILIES BUILT ON THIS (read before adding one):
//
//  1. One registration path. Create() takes a ClassSpec; the atom is
//     cached by class NAME in one process-wide table, so two objects of
//     the same class register once and a class name is spelled once.
//     Never write your own RegisterClassExW.
//  2. `this` is bound at WM_NCCREATE from lpCreateParams by a static
//     thunk and CLEARED at WM_NCDESTROY. Every override therefore runs on
//     a live object, and every message delivered inside CreateWindowExW
//     (WM_CREATE, the first WM_SIZE) reaches the object — the eight
//     POST-bound classes in the tree today drop those. After
//     WM_NCDESTROY the thunk hands messages straight to DefWindowProcW,
//     so a proc can never run against a freed object.
//  3. Painting goes through Paint(dc, dirty) into a CACHED back buffer.
//     WM_ERASEBKGND returns 1 and WM_PRINTCLIENT shares the same path, so
//     a surface paints identically to screen, to a print DC and to a
//     thumbnail. Paint() must be pure drawing: NO model rebuilds, no host
//     round-trips, no timer arming (docs/sidebar-architecture.md §2.7).
//  4. dpi() is written ONLY by Create() and WM_DPICHANGED. Nothing else
//     may assign it — the shell's ComputeLayout() side effect (lane 3
//     §3.2) is exactly the bug this rules out. WM_DPICHANGED rebuilds
//     fonts() and then calls OnDpiChanged(old_dpi) so the subclass can
//     re-measure; the base has already moved the window to the suggested
//     rect by then.
//  5. palette() is owned by the base and refreshed from the theme bus.
//     Override QueryPalette() to say where your colours come from (a
//     controller, a snapshot); override OnThemeChanged() only to do extra
//     work — call the base first.
//  6. hits_ is THE rect list. Hover, click routing, tooltips and (a later
//     agent's) UIA provider all read it and nothing else — see the
//     "ACCESSIBILITY SEAM" note below. Rebuild it from your layout pass,
//     never from Paint().
//  7. Timers come from AllocTimer()/PhiWindow::Timer. No file picks a
//     literal id and no object arms a timer on a window it does not own.
//  8. Capture comes from BeginCapture(); WM_CAPTURECHANGED and
//     WM_CANCELMODE both land in OnCaptureLost(), so a stolen capture can
//     never leave a drag stuck (lane 3 §4.2).
//
// The first half of this header is PURE: plain ints, no windows.h, no GDI.
// It holds the placement, hit-resolution, timer-allocation and easing
// decisions so test/model/ui_substrate_test.cc can pin them on the Linux
// host. The Win32 half is compiled only for the client.

namespace phi_client {

// ---------------------------------------------------------------------------
// Pure geometry.

// A rectangle in device pixels, in RECT's field order and half-open
// convention (right/bottom are one past the last pixel). PhiToRect /
// PhiFromRect convert; the pure code never sees a RECT so the host can
// compile it.
struct PhiRectI {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  constexpr int width() const { return right - left; }
  constexpr int height() const { return bottom - top; }
  constexpr bool empty() const { return right <= left || bottom <= top; }
  constexpr bool Contains(int x, int y) const {
    return x >= left && x < right && y >= top && y < bottom;
  }
};

constexpr bool operator==(const PhiRectI& a, const PhiRectI& b) {
  return a.left == b.left && a.top == b.top && a.right == b.right &&
         a.bottom == b.bottom;
}
constexpr bool operator!=(const PhiRectI& a, const PhiRectI& b) {
  return !(a == b);
}

// The smallest rect covering both; an empty operand is ignored, so
// hover's "old ∪ new" works when either side is "nothing hovered".
PhiRectI PhiRectUnion(const PhiRectI& a, const PhiRectI& b);

// ---------------------------------------------------------------------------
// Hit rects — hover, clicks, tooltips and accessibility over ONE list.

// What a hit IS, for assistive tech and for tooltips. Kept deliberately
// small: it maps onto UIA's control types without the provider having to
// know anything about the surface that produced it.
enum class PhiHitRole {
  kNone,
  kButton,
  kToggle,      // A pin/star/checkbox: `selected` carries the state.
  kListItem,    // A row in a list; `selected` is the selection.
  kTab,
  kText,        // Static label — announced, not invokable.
  kSlider,
  kGroup,       // A container announced around finer hits.
  kImage,       // A favicon, a thumbnail: announced by `name`, not invoked.
};

// One addressable region of a window. `key` is the surface's own id (a row
// index, a command id, a packed row+column); the base never interprets it.
// `name` is the accessible name AND the tooltip text — a hit with an empty
// name is invisible to both.
struct PhiHit {
  PhiRectI bounds;
  int key = 0;
  std::wstring name;
  PhiHitRole role = PhiHitRole::kButton;
  bool selected = false;
  bool enabled = true;
};

// The index of the hit under (x, y), or -1. LAST match wins: hits are
// added in paint order, so a child rect added after its row's rect takes
// the pointer, exactly as the pixels do.
int PhiHitAt(const std::vector<PhiHit>& hits, int x, int y);

// ---------------------------------------------------------------------------
// Popover placement — ONE implementation.
//
// Five hand-rolled copies exist today (lane 3 §2, "Anchor placement with
// monitor clamping"): the downloads popover, the folder picker, the Space
// theme panel, the tab preview's CardOrigin and the search-tabs panel.
// This is their union, and the migration proved each one reproduces
// pixel-for-pixel.

enum class PhiPopoverSide {
  kBelow,     // Under the anchor: y = anchor.bottom + gap.
  kAbove,     // Over it: y = anchor.top - height - gap.
  kRightTop,  // Beside it, tops aligned: x = anchor.right + gap.
  kExplicit,  // `anchor`'s top-left IS the position; only clamping applies.
};

enum class PhiPopoverAlign {
  kLeading,          // x = anchor.left (kBelow/kAbove).
  kCenter,           // Centred on the anchor.
  kCenterIfWider,    // Centred only when the anchor is wider than the popover
                     // — the Mac's nearTrigger / windowTop pair.
};

struct PhiPlacementSpec {
  PhiPopoverSide side = PhiPopoverSide::kBelow;
  PhiPopoverAlign align = PhiPopoverAlign::kLeading;
  // Device pixels, already scaled by the caller.
  int gap = 0;
  // Keep-off-the-screen-edge margin, device pixels.
  int margin = 0;
  // Flip to the opposite side when the preferred one does not fit.
  // kBelow/kAbove flip UNCONDITIONALLY (then clamp) — that is what the
  // folder picker, the Space theme panel and the extensions popover do.
  // kRightTop flips only when the other side actually fits, which is the
  // tab preview's CustomTooltipGeometry rule.
  bool flip = true;
  // Only flip when the OTHER side actually fits. False (the default) flips
  // unconditionally and lets the clamp sort it out, which is what the
  // folder picker, the Space theme panel and the extensions popover do;
  // the tab preview's CustomTooltipGeometry sets it.
  bool flip_only_if_fits = false;
  // Clamp into `work` minus `margin`. False keeps the exact position, for
  // the three surfaces that place themselves off their owner and do not
  // clamp today (toast, tab-switcher HUD, search-tabs palette).
  bool clamp = true;
};

// Where a `width` x `height` popover lands for `anchor`, inside the
// monitor work area `work`. All coordinates are screen device pixels.
PhiRectI PhiPlacePopover(const PhiRectI& anchor,
                         int width,
                         int height,
                         const PhiRectI& work,
                         const PhiPlacementSpec& spec);

// ---------------------------------------------------------------------------
// Timer ids.
//
// Today's ids are an unwritten cross-file 0x515–0x542 registry with five
// files also using literal 1 (lane 3 §4.1). A PhiWindow hands out ids from
// its OWN pool instead, so two objects on one HWND cannot collide and no
// file picks a number. The base is above the legacy space so a surface
// that still carries a hand-picked id while it migrates cannot clash.
inline constexpr unsigned kPhiTimerIdBase = 0x600u;

class PhiTimerPool {
 public:
  PhiTimerPool();
  ~PhiTimerPool();

  // The lowest free id, kPhiTimerIdBase upward. Never returns 0.
  unsigned Alloc();
  void Release(unsigned id);
  bool InUse(unsigned id) const;
  size_t live() const;

 private:
  std::vector<bool> used_;
};

// ---------------------------------------------------------------------------
// Easing — the curve behind PhiAnimator (phi_animator.h) and the seven
// hand-rolled loops it replaces.

enum class PhiEasing {
  kLinear,
  kEaseIn,      // t^2
  kEaseOut,     // 1-(1-t)^2
  kEaseInOut,   // NSAnimationContext's .easeInEaseOut, the shape the tab
                // preview's slide and the toast's fade already use.
};

// `t` is clamped to 0..1 before the curve is applied.
double PhiEase(PhiEasing easing, double t);

// Rounds away from `from`, so a one-pixel move lands on the first tick
// instead of on the last (the tab preview's Lerp, kept exactly).
int PhiLerpInt(int from, int to, double t);

#if defined(_WIN32)

// ---------------------------------------------------------------------------
// Win32.

// THE tooltip (PhiWindow::EnableTooltips) for a surface that is not a
// PhiWindow: the shell, whose hand-rolled window procedure keeps its own
// hover. Shows `text` by the cursor in `palette`, at `owner`'s scale; the
// caller runs the dwell. PhiHideTooltipFor hides it while `owner` is the
// one showing it.
void PhiShowTooltipFor(HWND owner,
                       const std::wstring& text,
                       const PhiPalette& palette);
void PhiHideTooltipFor(HWND owner);

// What the accessibility provider needs from a surface, and nothing more.
// PhiWindow implements it, and so does the shell — which owns a hand-rolled
// window procedure of its own and is therefore not a PhiWindow, yet holds
// the one hit list a screen reader most needs to see (the tab list). The
// provider talks to this, so neither surface has to pretend to be the other.
class PhiHitSource {
 public:
  virtual ~PhiHitSource() = default;
  virtual HWND hwnd() const = 0;
  virtual const std::vector<PhiHit>& hits() const = 0;
  virtual uint32_t hits_version() const = 0;
  // Index of the hit under a CLIENT point, or -1.
  virtual int HitTest(POINT client_pt) const = 0;
  // Acts on hits()[index] exactly as a mouse click on it would.
  virtual bool InvokeHit(int index) = 0;
};


inline RECT PhiToRect(const PhiRectI& r) {
  RECT out = {r.left, r.top, r.right, r.bottom};
  return out;
}
inline PhiRectI PhiFromRect(const RECT& r) {
  PhiRectI out;
  out.left = static_cast<int>(r.left);
  out.top = static_cast<int>(r.top);
  out.right = static_cast<int>(r.right);
  out.bottom = static_cast<int>(r.bottom);
  return out;
}

// One font, in DESIGN units: `size` is the point size at 96 dpi and the
// set scales it. `face` must outlive the set (a string literal).
struct PhiFontDesc {
  int size = 13;
  int weight = FW_NORMAL;
  const wchar_t* face = L"Segoe UI";
  bool strikeout = false;
  bool italic = false;
};

// The fonts one window owns, keyed by (face, size, weight, flags) at ONE
// dpi. WM_DPICHANGED calls Rebuild(), which drops every HFONT; a Get()
// after that re-creates at the new dpi. This is what fixes the stale-font
// drift in the feedback and preview surfaces (lane 3 §3.5).
class PhiFontSet {
 public:
  explicit PhiFontSet(UINT dpi);
  ~PhiFontSet();
  PhiFontSet(const PhiFontSet&) = delete;
  PhiFontSet& operator=(const PhiFontSet&) = delete;

  HFONT Get(const PhiFontDesc& desc);
  // Convenience for the common Segoe UI case.
  HFONT Get(int size, int weight);
  void Rebuild(UINT dpi);
  UINT dpi() const { return dpi_; }

 private:
  struct Entry {
    PhiFontDesc desc;
    HFONT font = nullptr;
  };
  UINT dpi_;
  std::vector<Entry> fonts_;
};

class PhiWindow : public PhiHitSource {
 public:
  // What a window class IS. `name` is spelled ONCE, here.
  struct ClassSpec {
    const wchar_t* name = nullptr;
    UINT style = 0;                       // CS_DBLCLKS, CS_HREDRAW, ...
    const wchar_t* cursor = IDC_ARROW;    // A LoadCursorW system id.
    HICON icon = nullptr;
    HICON icon_small = nullptr;
  };

  // Holds the mouse capture for as long as it lives. Releasing it, the
  // system taking it away (WM_CAPTURECHANGED) and WM_CANCELMODE all end in
  // PhiWindow::OnCaptureLost().
  class Capture {
   public:
    Capture() = default;
    explicit Capture(PhiWindow* owner);
    ~Capture();
    Capture(Capture&& other) noexcept;
    Capture& operator=(Capture&& other) noexcept;
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    bool held() const { return owner_ != nullptr; }
    void Release();

   private:
    // Back-pointer to the window that created us and outlives us.
    RAW_PTR_EXCLUSION PhiWindow* owner_ = nullptr;
  };

  // An id out of the window's pool, killed and released on destruction.
  class Timer {
   public:
    Timer() = default;
    explicit Timer(PhiWindow* owner);
    ~Timer();
    Timer(Timer&& other) noexcept;
    Timer& operator=(Timer&& other) noexcept;
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    UINT_PTR id() const { return id_; }
    bool valid() const { return owner_ != nullptr && id_ != 0; }
    bool running() const { return running_; }
    // `coalescable` uses SetCoalescableTimer so the OS may batch ticks
    // with other timers — right for animation, wrong for a poll.
    void Start(UINT interval_ms, bool coalescable = false);
    void Stop();

   private:
    // Back-pointer to the window that created us and outlives us.
    RAW_PTR_EXCLUSION PhiWindow* owner_ = nullptr;
    UINT_PTR id_ = 0;
    bool running_ = false;
  };

  PhiWindow();
  ~PhiWindow() override;
  PhiWindow(const PhiWindow&) = delete;
  PhiWindow& operator=(const PhiWindow&) = delete;

  // Registers `spec` (once per name, process-wide) and creates the window
  // with `this` bound at WM_NCCREATE. `bounds` is in screen coordinates
  // for a popup / top-level and parent coordinates for a child. Returns
  // the HWND, or null.
  HWND Create(const ClassSpec& spec,
              DWORD ex_style,
              DWORD style,
              const RECT& bounds,
              HWND owner_or_parent,
              const wchar_t* window_name = L"");

  HWND hwnd() const override;
  bool alive() const { return hwnd_ != nullptr; }
  UINT dpi() const { return dpi_; }
  int Scaled(int design) const;
  RECT ScaledRect(const RECT& design) const;
  PhiFontSet& fonts() { return fonts_; }
  const PhiPalette& palette() const { return palette_; }
  // Replaces the palette and repaints when it actually changed.
  void SetPalette(const PhiPalette& palette);

  // THE hit list. Hover, clicks and — once a later agent lands the
  // provider — UIA all read this and nothing else.
  const std::vector<PhiHit>& hits() const override;
  // The index of the hovered hit, or -1.
  int hovered() const { return hover_; }
  int HitTest(POINT client_pt) const override;
  // The first hit carrying `key`, or null.
  const PhiHit* HitByKey(int key) const;
  // Bumped every time hits_ is replaced through ClearHits(); the UIA
  // provider uses it to know when to raise a structure-changed event
  // (phi_uia.cc, PhiUiaHitsChanged).
  uint32_t hits_version() const override;

  // Acts on hits()[index] exactly as a mouse click on it would: the same
  // WM_LBUTTONDOWN / WM_LBUTTONUP pair, through the same wndproc, the same
  // hit test and the same OnMouseDown/OnClick overrides. THE path, not a
  // copy of it — this is what the UIA Invoke pattern calls, so a surface
  // cannot be clickable by mouse and dead to a screen reader.
  //
  // The point sent is one inside the hit's own rect that the hit test
  // resolves back to it, so a row whose middle is covered by a button on
  // top is still invoked correctly. False when the hit does not exist, is
  // disabled, or is completely covered by later hits — cases where a real
  // click cannot reach it either.
  //
  // The window MAY BE DESTROYED by the click (the search-tabs panel
  // commits and closes on the button-down): this returns without touching
  // any member afterwards, and the caller must not touch the object
  // either.
  bool InvokeHit(int index) override;

  // Tooltips, from the same hit list (lane 3 §2 "Tooltips", §7 item 15).
  // OFF in this base; the PhiPopover and PhiPanel families turn them on for
  // every surface they create: hovering a hit whose name is non-empty for
  // the system's tooltip delay shows that name, and nothing else in the
  // client needs a TOOLTIPS_CLASS, a TTM_ message or a second rect list.
  // PhiHit::name is the accessible name AND the tooltip text, so a surface
  // cannot label one and forget the other — name hits for a user, never
  // with an internal verb.
  void EnableTooltips(bool enabled);
  bool tooltips_enabled() const { return tooltips_enabled_; }

  UINT_PTR AllocTimer();
  void ReleaseTimer(UINT_PTR id);
  void StartTimer(UINT_PTR id, UINT interval_ms, bool coalescable = false);
  void StopTimer(UINT_PTR id);
  Timer NewTimer() { return Timer(this); }

  Capture BeginCapture() { return Capture(this); }

  void Invalidate();
  void Invalidate(const RECT& rect);
  void InvalidateHit(int index);
  // Moves what is already painted in `area` (client px) by `dy`, in the back
  // buffer and on screen, and invalidates only the band it uncovers — a
  // scroll repaints a strip instead of the whole window. False (nothing
  // moved; invalidate as usual) without a back buffer or when no row of the
  // area would survive the move.
  bool ScrollPainted(const RECT& area, int dy);
  void Destroy();
  // Self-owned windows: `delete this` once the HWND is fully gone — AFTER
  // WM_NCDESTROY, not inside WM_DESTROY where five popovers do it today
  // (children are destroyed after WM_DESTROY, so their subclass procs then
  // run against a freed owner — lane 3 §5). The base clears
  // GWLP_USERDATA, drops the back buffer and unregisters from the theme
  // bus before the delete, so nothing can reach the object afterwards.
  void DeleteWhenDestroyed() { delete_on_destroy_ = true; }

  // Called by PhiThemeBus when the OS appearance or the app theme moves.
  void NotifyThemeChanged();

 protected:
  // Draw the window. `dirty` is the update rect in client coordinates;
  // `dc` is the cached back buffer (or the caller's DC for
  // WM_PRINTCLIENT), already origin-aligned with the client area.
  virtual void Paint(HDC dc, const RECT& dirty) = 0;
  // Alpha-aware GDI surfaces already prepare premultiplied pixels.
  virtual bool PreservePaintAlpha() const;

  // First refusal on every message. Set *handled and return a result to
  // stop the base from handling it.
  virtual LRESULT OnMessage(UINT message,
                            WPARAM wparam,
                            LPARAM lparam,
                            bool* handled);

  virtual void OnCreated() {}
  virtual void OnSize(int width, int height) {}
  // The base has already written dpi(), moved the window to the suggested
  // rect and rebuilt fonts(). Re-measure content here.
  virtual void OnDpiChanged(UINT old_dpi) {}
  // Where this window's colours come from. The default keeps whatever
  // SetPalette() last stored (a snapshot taken at open, which is what the
  // two framework popovers want); override to read a live source.
  virtual PhiPalette QueryPalette() const;
  // Palette moved. The base re-reads QueryPalette() and repaints; an
  // override must call the base.
  virtual void OnThemeChanged();
  // `hit` and `previous` index hits(); either may be -1. The base has
  // already invalidated the union of the two rects.
  virtual void OnHover(int hit, int previous) {}
  virtual void OnMouseDown(int hit, POINT client_pt) {}
  virtual void OnClick(int hit, POINT client_pt) {}
  virtual void OnWheel(int delta, POINT client_pt) {}
  // True when handled. The default routes VK_ESCAPE to OnEscape() and
  // VK_RETURN to OnEnter().
  virtual bool OnKey(WPARAM virtual_key);
  virtual void OnEscape() {}
  virtual void OnEnter() {}
  virtual void OnTimerFired(UINT_PTR id) {}
  virtual void OnCaptureLost() {}
  // The HWND is going away. Members are still live; hwnd() is still set.
  virtual void OnNcDestroy() {}
  // WM_GETDLGCODE. The default asks for all keys and arrows so a popover
  // sees Escape/Enter/arrows even inside an IsDialogMessage loop.
  virtual UINT DlgCode() const;

  // Rebuild the hit list from a layout pass — never from Paint().
  void ClearHits();
  void AddHit(const RECT& bounds,
              int key,
              std::wstring name,
              PhiHitRole role,
              bool selected = false,
              bool enabled = true);

  // ACCESSIBILITY SEAM (lane 3 §6.1, ranked finding 2) — LANDED, see
  // phi_uia.h. The base answers WM_GETOBJECT / UiaRootObjectId with an
  // IRawElementProviderSimple + IRawElementProviderFragmentRoot over
  // hits(), so EVERY PhiWindow is accessible with no per-surface code:
  // hits(), hits_version() and hovered() are the whole data source. A
  // surface overrides this only to answer a DIFFERENT object id (a hosted
  // child's OBJID, say); call the base for everything else, and do NOT add
  // a second rect list anywhere.
  //
  // UIA calls arrive on the UI thread (the providers declare
  // ProviderOptions_UseComThreading), so an override may touch members
  // freely.
  virtual LRESULT OnGetObject(WPARAM wparam, LPARAM lparam, bool* handled);

  // THE rect list (see the contract above). Protected because the
  // families rebuild it directly; prefer ClearHits()/AddHit().
  std::vector<PhiHit> hits_;

 private:
  static LRESULT CALLBACK Thunk(HWND hwnd,
                                UINT message,
                                WPARAM wparam,
                                LPARAM lparam);
  LRESULT Dispatch(UINT message, WPARAM wparam, LPARAM lparam);
  void PaintNow();
  bool EnsureBackBuffer(int width, int height);
  void DropBackBuffer();
  void UpdateHover(POINT client_pt);
  void ClearHover();
  void CaptureTaken();
  void CaptureLost();
  // Tooltip plumbing; all no-ops while tooltips_enabled_ is false.
  void ScheduleTooltip();
  void HideTooltip();
  bool HandleTooltipTimer(UINT_PTR id);

  HWND hwnd_ = nullptr;
  UINT dpi_ = 96;
  PhiPalette palette_;
  PhiFontSet fonts_;
  PhiTimerPool timers_;
  int hover_ = -1;
  uint32_t hits_version_ = 0;
  bool tracking_leave_ = false;
  bool capture_held_ = false;
  bool theme_registered_ = false;
  bool delete_on_destroy_ = false;
  bool tooltips_enabled_ = false;
  // Out of this window's own pool, allocated on the first hover that could
  // show a tip and released with the window.
  UINT_PTR tooltip_timer_ = 0;
  // The cached back buffer (lane 3 §3.4 retires ten copies of the block).
  HDC buffer_dc_ = nullptr;
  HBITMAP buffer_bitmap_ = nullptr;
  // Borrowed from the GDI bitmap allocation, outside PartitionAlloc.
  RAW_PTR_EXCLUSION void* buffer_bits_ = nullptr;
  HGDIOBJ buffer_previous_ = nullptr;
  int buffer_width_ = 0;
  int buffer_height_ = 0;
};

#endif  // defined(_WIN32)

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_WINDOW_H_

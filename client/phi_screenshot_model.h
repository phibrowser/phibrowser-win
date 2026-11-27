// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SCREENSHOT_MODEL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SCREENSHOT_MODEL_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Page screenshots (docs/page-screenshots.md): the PURE half — capture
// geometry, PNG checks, the viewport map of Capture Area…, and the
// editor's document (annotations, undo, crop, blur blocks). No Win32 here,
// so every rule can be read (and tested) without a window.

namespace phi_client {

// --- Capture limits (the Mac's PageScreenshotService) ----------------------

inline constexpr int kPhiScreenshotMaxAxis = 16384;          // Clip output.
inline constexpr int64_t kPhiScreenshotMaxPixels = 64000000;  // 64 Mpx.
inline constexpr int kPhiScreenshotMaxDecodeAxis = 32768;     // Validation.
inline constexpr size_t kPhiScreenshotMaxBase64 = 30u << 20;  // 30 MiB.
inline constexpr int kPhiScreenshotBudgetMs = 60000;
inline constexpr int kPhiScreenshotCommandMs = 15000;
inline constexpr int kPhiScreenshotOpenMs = 10000;

struct PhiPointF {
  double x = 0;
  double y = 0;
};

struct PhiRectF {
  double left = 0;
  double top = 0;
  double right = 0;
  double bottom = 0;
  double width() const { return right - left; }
  double height() const { return bottom - top; }
  bool empty() const { return right <= left || bottom <= top; }
  bool Contains(PhiPointF p) const {
    return p.x >= left && p.x <= right && p.y >= top && p.y <= bottom;
  }
};

// The rect spanned by two corners, in any order.
PhiRectF PhiRectFromPoints(PhiPointF a, PhiPointF b);
PhiRectF PhiRectIntersect(const PhiRectF& a, const PhiRectF& b);
PhiRectF PhiRectUnionF(const PhiRectF& a, const PhiRectF& b);
bool PhiRectsIntersect(const PhiRectF& a, const PhiRectF& b);
PhiRectF PhiRectInflate(const PhiRectF& r, double by);

// The clip Page.captureScreenshot gets: the CSS rect rounded OUT to whole
// pixels, and the scale that keeps the PHYSICAL output (css × dpr × scale)
// within kPhiScreenshotMaxAxis per axis and then kPhiScreenshotMaxPixels,
// with a one-pixel rounding margin. A clip within both keeps scale 1.
// `ok` is false for an empty rect or a non-positive dpr.
struct PhiScreenshotClip {
  bool ok = false;
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
  double scale = 1;
  // The pixel size Chromium will return (ceil of css × dpr × scale).
  int64_t out_width = 0;
  int64_t out_height = 0;
};
PhiScreenshotClip PhiScreenshotClipFor(double x,
                                       double y,
                                       double width,
                                       double height,
                                       double device_pixel_ratio);

// What the capture's PNG must be before it may reach the clipboard:
// signature, an IHDR within kPhiScreenshotMaxDecodeAxis per axis and
// kPhiScreenshotMaxPixels in total, and an IEND trailer.
struct PhiPngInfo {
  bool ok = false;
  int width = 0;
  int height = 0;
  std::string error;
};
PhiPngInfo PhiScreenshotValidatePng(const std::string& png);

// Base64 (RFC 4648, the CDP `data` field). False on a malformed string.
bool PhiScreenshotDecodeBase64(const std::string& text, std::string* out);

// Capture Area…'s map between the overlay's pixels and the document's CSS
// pixels, measured from what the page reports (innerWidth / innerHeight
// against the viewport's pixel size — page zoom and DPI fold into it).
struct PhiScreenshotViewport {
  // The viewport in the overlay's client pixels.
  double left = 0;
  double top = 0;
  double width = 0;
  double height = 0;
  // From the page.
  double inner_width = 0;
  double inner_height = 0;
  double scroll_x = 0;
  double scroll_y = 0;
  // document.documentElement scrollWidth / scrollHeight.
  double doc_width = 0;
  double doc_height = 0;
  bool valid() const {
    return width > 0 && height > 0 && inner_width > 0 && inner_height > 0;
  }
  // CSS px per overlay px.
  double css_per_px_x() const { return inner_width / width; }
  double css_per_px_y() const { return inner_height / height; }
};
// Overlay px → document CSS px, the point clamped into the viewport first
// (a pointer outside the page maps to its edge).
PhiPointF PhiScreenshotToDocument(const PhiScreenshotViewport& v,
                                  double px,
                                  double py);
// Document CSS px → overlay px (no clamping).
PhiPointF PhiScreenshotToOverlay(const PhiScreenshotViewport& v,
                                 double css_x,
                                 double css_y);
// The edge autoscroll step for a pointer at (px, py): how far (CSS px) to
// scroll this tick, zero away from every edge. `zone` is the edge band in
// overlay px; `max_step` the step at the edge itself.
PhiPointF PhiScreenshotEdgeScroll(const PhiScreenshotViewport& v,
                                  double px,
                                  double py,
                                  double zone,
                                  double max_step);

// A file name for `title` + `ext` ("png"): characters Windows refuses and
// control characters become "-", leading / trailing dots and spaces go,
// reserved device names gain a "_", the stem is cut to 120 characters, and
// an empty result falls back to `fallback`.
std::wstring PhiScreenshotFileName(const std::wstring& title,
                                   const std::wstring& fallback,
                                   const wchar_t* ext);

// --- The editor's document ---------------------------------------------------

// An immutable source image revision: premultiplied BGRA, top-down,
// stride = width * 4. A crop makes a new one.
struct PhiScreenshotImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> bgra;
};
using PhiScreenshotImageRef = std::shared_ptr<const PhiScreenshotImage>;

enum class PhiAnnotationKind {
  kArrow,
  kFreehand,
  kText,
  kRectangle,
  kEllipse,
  kBlur,
};
const char* PhiAnnotationKindName(PhiAnnotationKind kind);

// Source-image pixels throughout. `frame` is the shape's rect (rectangle,
// ellipse, blur, text — its height follows the laid-out text); an arrow
// runs `from` → `to`; a freehand path is `points`.
struct PhiAnnotation {
  uint64_t id = 0;
  PhiAnnotationKind kind = PhiAnnotationKind::kRectangle;
  PhiRectF frame;
  PhiPointF from;
  PhiPointF to;
  std::vector<PhiPointF> points;
  uint32_t color = 0xFFF21F1F;  // ARGB; the Mac's ScreenshotColor.red.
  double stroke = 2.5;
  bool fill = false;
  double font_size = 24;
  std::wstring text;
};

// The Mac's arrowhead: open 45-degree wings min(0.3 x length, 32) long at
// its default 2.5 stroke; a thicker stroke grows the head in proportion
// (PhiArrowHeadLength), so a scaled-up style keeps the Mac's shape.
inline constexpr double kPhiArrowHeadLength = 32;
// The Mac's style choices (ScreenshotAnnotationWidgets): six swatches
// (ARGB), five stroke widths, five text sizes.
inline constexpr uint32_t kPhiScreenshotSwatches[6] = {
    0xFFF21F1F, 0xFFF5B326, 0xFF40A652, 0xFF4073F5, 0xFF26262B, 0xFFFFFFFF};
inline constexpr double kPhiScreenshotWidths[5] = {2.5, 4, 8, 16, 32};
inline constexpr double kPhiScreenshotTextSizes[5] = {15, 24, 44, 60, 80};

// The head length of `a` (an arrow) at shaft length `distance`.
double PhiArrowHeadLength(const PhiAnnotation& a, double distance);

// The Mac's sizes are image pixels of a 2x Retina capture. A capture at a
// higher device scale has proportionally smaller-looking annotations, so
// the editor multiplies its defaults and the strip's choices by this.
double PhiScreenshotStyleUnit(double device_pixel_ratio);

// The box selection handles and hit tests work on (stroke included).
PhiRectF PhiAnnotationBounds(const PhiAnnotation& a);
// True when `p` (source px) is on the object: a stroke within
// `tolerance` of a line or outline, inside a filled shape, a blur or a
// text frame.
bool PhiAnnotationHit(const PhiAnnotation& a, PhiPointF p, double tolerance);
void PhiAnnotationMove(PhiAnnotation* a, double dx, double dy);
// Maps the object from `from` to `to` (both its bounds), points scaled.
// Text keeps its font size and takes only the new left / right.
void PhiAnnotationResize(PhiAnnotation* a,
                         const PhiRectF& from,
                         const PhiRectF& to);
// Stroke width or text size by `delta` (minimum 1; a smaller value is kept
// on a decrement). False for a blur (it has neither).
bool PhiAnnotationAdjustSize(PhiAnnotation* a, double delta);

// The freehand path as quadratic Béziers through the samples' midpoints
// (the Mac's 715c8b67): `out` gets the start point, then (control, end)
// pairs, ending exactly on the last sample.
void PhiFreehandCurve(const std::vector<PhiPointF>& points,
                      std::vector<PhiPointF>* out);

// One opaque block of a blur: its rect (source px) and premultiplied
// colour, the average of the source pixels under it. Blocks are about
// `block` px on a grid anchored at the blur's own origin.
struct PhiBlurBlock {
  PhiRectF rect;
  uint32_t bgra = 0;
};
inline constexpr double kPhiBlurBlock = 20;
void PhiBlurBlocks(const PhiScreenshotImage& image,
                   const PhiRectF& frame,
                   double block,
                   std::vector<PhiBlurBlock>* out);

// A new image holding `rect` (rounded, clamped) of `image`; null when the
// rect has no pixels.
PhiScreenshotImageRef PhiCropImage(const PhiScreenshotImage& image,
                                   const PhiRectF& rect);

class PhiScreenshotDocument {
 public:
  struct State {
    PhiScreenshotImageRef image;
    std::vector<PhiAnnotation> annotations;
    // Which edit this state is; undo / redo bring theirs back, so undoing
    // to what was exported is clean again.
    uint64_t generation = 0;
  };

  static constexpr size_t kMaxUndo = 100;

  explicit PhiScreenshotDocument(PhiScreenshotImageRef image);
  ~PhiScreenshotDocument();
  PhiScreenshotDocument(const PhiScreenshotDocument&) = delete;
  PhiScreenshotDocument& operator=(const PhiScreenshotDocument&) = delete;

  const PhiScreenshotImage& image() const { return *state_.image; }
  const PhiScreenshotImageRef& image_ref() const { return state_.image; }
  const std::vector<PhiAnnotation>& annotations() const {
    return state_.annotations;
  }
  const State& state() const { return state_; }

  // --- Transactions. Each finished gesture or command is ONE: take
  // Snapshot() before it starts changing the document, then Commit(it)
  // when it ends (or Restore(it) to abandon). A commit that changed
  // nothing records nothing.
  State Snapshot() const { return state_; }
  void Commit(State before);
  void Restore(State before) { state_ = std::move(before); }

  // Live edits inside a transaction (no history of their own).
  PhiAnnotation* Find(uint64_t id);
  const PhiAnnotation* Find(uint64_t id) const;
  // Appends `a` (given a fresh id) and returns that id.
  uint64_t Add(PhiAnnotation a);
  void Remove(const std::vector<uint64_t>& ids);
  // Copies offset by `offset`, appended in their order; the new ids.
  std::vector<uint64_t> Duplicate(const std::vector<uint64_t>& ids,
                                  double offset);
  void BringToFront(const std::vector<uint64_t>& ids);
  void SendToBack(const std::vector<uint64_t>& ids);
  // Crops the source to `rect` (source px): every object moves by the
  // crop's origin, objects wholly outside are dropped. False (nothing
  // changed) for a rect with no pixels.
  bool Crop(const PhiRectF& rect);

  // Topmost object at `p`, or 0.
  uint64_t HitTest(PhiPointF p, double tolerance) const;
  // Objects whose bounds meet `rect`.
  std::vector<uint64_t> InRect(const PhiRectF& rect) const;

  bool CanUndo() const { return !undo_.empty(); }
  bool CanRedo() const { return !redo_.empty(); }
  size_t undo_depth() const { return undo_.size(); }
  size_t redo_depth() const { return redo_.size(); }
  bool Undo();
  bool Redo();

  // A commit gives the state a fresh generation; an export records the
  // generation it wrote, and the document is dirty while they differ.
  uint64_t generation() const { return state_.generation; }
  void MarkSaved(uint64_t generation) { saved_generation_ = generation; }
  bool dirty() const { return state_.generation != saved_generation_; }

 private:
  State state_;
  std::vector<State> undo_;
  std::vector<State> redo_;
  uint64_t next_id_ = 1;
  uint64_t next_generation_ = 1;
  uint64_t saved_generation_ = 0;
};

// The object's geometry box: its frame, the arrow's endpoints' rect or the
// freehand samples' bounds — what the resize handles sit on.
PhiRectF PhiAnnotationBox(const PhiAnnotation& a);

// Whether two states hold the same document (the image revision and every
// object field).
bool PhiScreenshotStatesEqual(const PhiScreenshotDocument::State& a,
                              const PhiScreenshotDocument::State& b);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SCREENSHOT_MODEL_H_

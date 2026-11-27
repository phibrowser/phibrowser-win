// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_screenshot_model.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <utility>

namespace phi_client {

namespace {

double Dist2(PhiPointF a, PhiPointF b) {
  const double dx = a.x - b.x;
  const double dy = a.y - b.y;
  return dx * dx + dy * dy;
}

// Distance from `p` to the segment a–b.
double SegmentDistance(PhiPointF p, PhiPointF a, PhiPointF b) {
  const double vx = b.x - a.x;
  const double vy = b.y - a.y;
  const double len2 = vx * vx + vy * vy;
  double t = 0;
  if (len2 > 0) {
    t = ((p.x - a.x) * vx + (p.y - a.y) * vy) / len2;
    t = std::clamp(t, 0.0, 1.0);
  }
  const PhiPointF q = {a.x + vx * t, a.y + vy * t};
  return std::sqrt(Dist2(p, q));
}

uint32_t ReadBe32(const std::string& s, size_t at) {
  return (static_cast<uint32_t>(static_cast<uint8_t>(s[at])) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[at + 1])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[at + 2])) << 8) |
         static_cast<uint32_t>(static_cast<uint8_t>(s[at + 3]));
}

PhiRectF Normalized(const PhiRectF& r) {
  return {std::min(r.left, r.right), std::min(r.top, r.bottom),
          std::max(r.left, r.right), std::max(r.top, r.bottom)};
}

bool SameAnnotation(const PhiAnnotation& a, const PhiAnnotation& b) {
  auto same_point = [](PhiPointF p, PhiPointF q) {
    return p.x == q.x && p.y == q.y;
  };
  auto same_rect = [](const PhiRectF& p, const PhiRectF& q) {
    return p.left == q.left && p.top == q.top && p.right == q.right &&
           p.bottom == q.bottom;
  };
  if (a.id != b.id || a.kind != b.kind || !same_rect(a.frame, b.frame) ||
      !same_point(a.from, b.from) || !same_point(a.to, b.to) ||
      a.color != b.color || a.stroke != b.stroke || a.fill != b.fill ||
      a.font_size != b.font_size || a.text != b.text ||
      a.points.size() != b.points.size()) {
    return false;
  }
  for (size_t i = 0; i < a.points.size(); ++i) {
    if (!same_point(a.points[i], b.points[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace

// --- Rect helpers -------------------------------------------------------------

PhiRectF PhiRectFromPoints(PhiPointF a, PhiPointF b) {
  return {std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x),
          std::max(a.y, b.y)};
}

PhiRectF PhiRectIntersect(const PhiRectF& a, const PhiRectF& b) {
  PhiRectF r = {std::max(a.left, b.left), std::max(a.top, b.top),
                std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
  if (r.right < r.left) {
    r.right = r.left;
  }
  if (r.bottom < r.top) {
    r.bottom = r.top;
  }
  return r;
}

PhiRectF PhiRectUnionF(const PhiRectF& a, const PhiRectF& b) {
  return {std::min(a.left, b.left), std::min(a.top, b.top),
          std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}

bool PhiRectsIntersect(const PhiRectF& a, const PhiRectF& b) {
  return a.left <= b.right && b.left <= a.right && a.top <= b.bottom &&
         b.top <= a.bottom;
}

PhiRectF PhiRectInflate(const PhiRectF& r, double by) {
  return {r.left - by, r.top - by, r.right + by, r.bottom + by};
}

// --- Capture geometry ----------------------------------------------------------

PhiScreenshotClip PhiScreenshotClipFor(double x,
                                       double y,
                                       double width,
                                       double height,
                                       double device_pixel_ratio) {
  PhiScreenshotClip clip;
  if (!(width > 0) || !(height > 0) || !(device_pixel_ratio > 0) ||
      !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) ||
      !std::isfinite(height)) {
    return clip;
  }
  const double x0 = std::floor(x);
  const double y0 = std::floor(y);
  const double x1 = std::ceil(x + width);
  const double y1 = std::ceil(y + height);
  clip.x = x0;
  clip.y = y0;
  clip.width = x1 - x0;
  clip.height = y1 - y0;
  if (clip.width <= 0 || clip.height <= 0) {
    return clip;
  }
  const double phys_w = clip.width * device_pixel_ratio;
  const double phys_h = clip.height * device_pixel_ratio;
  double scale = 1;
  // Per axis first, with a one-pixel margin for Chromium's rounding up.
  if (phys_w > kPhiScreenshotMaxAxis) {
    scale = std::min(scale, (kPhiScreenshotMaxAxis - 1) / phys_w);
  }
  if (phys_h > kPhiScreenshotMaxAxis) {
    scale = std::min(scale, (kPhiScreenshotMaxAxis - 1) / phys_h);
  }
  auto out = [&](double s, int64_t* w, int64_t* h) {
    *w = static_cast<int64_t>(std::ceil(phys_w * s - 1e-9));
    *h = static_cast<int64_t>(std::ceil(phys_h * s - 1e-9));
  };
  int64_t w = 0;
  int64_t h = 0;
  out(scale, &w, &h);
  if (w * h > kPhiScreenshotMaxPixels) {
    // Both axes together, again with a pixel of margin on each.
    const double fit = std::sqrt(static_cast<double>(kPhiScreenshotMaxPixels) /
                                 ((phys_w + 1) * (phys_h + 1)));
    scale = std::min(scale, fit);
    out(scale, &w, &h);
    for (int guard = 0; guard < 64 && w * h > kPhiScreenshotMaxPixels;
         ++guard) {
      scale *= 0.999;
      out(scale, &w, &h);
    }
  }
  clip.scale = scale;
  clip.out_width = std::max<int64_t>(1, w);
  clip.out_height = std::max<int64_t>(1, h);
  clip.ok = true;
  return clip;
}

PhiPngInfo PhiScreenshotValidatePng(const std::string& png) {
  PhiPngInfo info;
  static const char kSignature[8] = {'\x89', 'P',  'N',    'G',
                                     '\r',   '\n', '\x1a', '\n'};
  static const char kIend[12] = {'\0', '\0', '\0',   '\0',   'I',    'E',
                                 'N',  'D',  '\xae', '\x42', '\x60', '\x82'};
  if (png.size() < 8 + 25 + 12 ||
      png.compare(0, 8, std::string(kSignature, 8)) != 0) {
    info.error = "not_png";
    return info;
  }
  if (ReadBe32(png, 8) != 13 || png.compare(12, 4, "IHDR") != 0) {
    info.error = "no_ihdr";
    return info;
  }
  const uint32_t w = ReadBe32(png, 16);
  const uint32_t h = ReadBe32(png, 20);
  if (w == 0 || h == 0 || w > kPhiScreenshotMaxDecodeAxis ||
      h > kPhiScreenshotMaxDecodeAxis ||
      static_cast<int64_t>(w) * static_cast<int64_t>(h) >
          kPhiScreenshotMaxPixels) {
    info.error = "too_large";
    return info;
  }
  if (png.compare(png.size() - 12, 12, std::string(kIend, 12)) != 0) {
    info.error = "no_iend";
    return info;
  }
  info.ok = true;
  info.width = static_cast<int>(w);
  info.height = static_cast<int>(h);
  return info;
}

bool PhiScreenshotDecodeBase64(const std::string& text, std::string* out) {
  out->clear();
  out->reserve(text.size() / 4 * 3);
  uint32_t acc = 0;
  int bits = 0;
  size_t pad = 0;
  for (char c : text) {
    int v = -1;
    if (c >= 'A' && c <= 'Z') {
      v = c - 'A';
    } else if (c >= 'a' && c <= 'z') {
      v = c - 'a' + 26;
    } else if (c >= '0' && c <= '9') {
      v = c - '0' + 52;
    } else if (c == '+') {
      v = 62;
    } else if (c == '/') {
      v = 63;
    } else if (c == '=') {
      ++pad;
      continue;
    } else if (c == '\r' || c == '\n' || c == ' ') {
      continue;
    } else {
      return false;
    }
    if (pad) {
      return false;  // Data after padding.
    }
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  return pad <= 2;
}

PhiPointF PhiScreenshotToDocument(const PhiScreenshotViewport& v,
                                  double px,
                                  double py) {
  px = std::clamp(px, v.left, v.left + v.width);
  py = std::clamp(py, v.top, v.top + v.height);
  return {v.scroll_x + (px - v.left) * v.css_per_px_x(),
          v.scroll_y + (py - v.top) * v.css_per_px_y()};
}

PhiPointF PhiScreenshotToOverlay(const PhiScreenshotViewport& v,
                                 double css_x,
                                 double css_y) {
  return {v.left + (css_x - v.scroll_x) / v.css_per_px_x(),
          v.top + (css_y - v.scroll_y) / v.css_per_px_y()};
}

PhiPointF PhiScreenshotEdgeScroll(const PhiScreenshotViewport& v,
                                  double px,
                                  double py,
                                  double zone,
                                  double max_step) {
  PhiPointF step;
  if (!v.valid() || zone <= 0) {
    return step;
  }
  auto ramp = [&](double distance) {
    // 0 at the zone's inner edge, max_step at (or past) the viewport edge.
    const double t = 1.0 - std::clamp(distance / zone, 0.0, 1.0);
    return max_step * t;
  };
  const double dl = px - v.left;
  const double dr = v.left + v.width - px;
  const double dt = py - v.top;
  const double db = v.top + v.height - py;
  if (dl < zone) {
    step.x = -ramp(dl);
  } else if (dr < zone) {
    step.x = ramp(dr);
  }
  if (dt < zone) {
    step.y = -ramp(dt);
  } else if (db < zone) {
    step.y = ramp(db);
  }
  return step;
}

std::wstring PhiScreenshotFileName(const std::wstring& title,
                                   const std::wstring& fallback,
                                   const wchar_t* ext) {
  std::wstring stem;
  for (wchar_t c : title) {
    if (c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' ||
        c == L'/' || c == L'\\' || c == L'|' || c == L'?' || c == L'*') {
      stem.push_back(L'-');
    } else {
      stem.push_back(c);
    }
  }
  auto trim = [](std::wstring* s) {
    while (!s->empty() && (s->front() == L' ' || s->front() == L'.')) {
      s->erase(s->begin());
    }
    while (!s->empty() && (s->back() == L' ' || s->back() == L'.')) {
      s->pop_back();
    }
  };
  trim(&stem);
  if (stem.size() > 120) {
    stem.resize(120);
    trim(&stem);
  }
  if (stem.empty()) {
    stem = fallback;
  }
  // CON, PRN, AUX, NUL, COM1-9, LPT1-9 — as the whole name or before a dot.
  std::wstring head = stem.substr(0, stem.find(L'.'));
  for (wchar_t& c : head) {
    c = static_cast<wchar_t>(std::towupper(c));
  }
  const bool reserved =
      head == L"CON" || head == L"PRN" || head == L"AUX" || head == L"NUL" ||
      (head.size() == 4 &&
       (head.compare(0, 3, L"COM") == 0 || head.compare(0, 3, L"LPT") == 0) &&
       head[3] >= L'1' && head[3] <= L'9');
  if (reserved) {
    stem += L"_";
  }
  return stem + L"." + ext;
}

// --- Annotations ------------------------------------------------------------------

double PhiArrowHeadLength(const PhiAnnotation& a, double distance) {
  const double scale = std::max(1.0, a.stroke / kPhiScreenshotWidths[0]);
  return std::min(0.3 * distance, kPhiArrowHeadLength * scale);
}

double PhiScreenshotStyleUnit(double device_pixel_ratio) {
  if (!(device_pixel_ratio > 0) || !std::isfinite(device_pixel_ratio)) {
    return 1;
  }
  return std::max(1.0, device_pixel_ratio / 2);
}

const char* PhiAnnotationKindName(PhiAnnotationKind kind) {
  switch (kind) {
    case PhiAnnotationKind::kArrow:
      return "arrow";
    case PhiAnnotationKind::kFreehand:
      return "freehand";
    case PhiAnnotationKind::kText:
      return "text";
    case PhiAnnotationKind::kRectangle:
      return "rectangle";
    case PhiAnnotationKind::kEllipse:
      return "ellipse";
    case PhiAnnotationKind::kBlur:
      return "blur";
  }
  return "unknown";
}

PhiRectF PhiAnnotationBox(const PhiAnnotation& a) {
  switch (a.kind) {
    case PhiAnnotationKind::kArrow:
      return PhiRectFromPoints(a.from, a.to);
    case PhiAnnotationKind::kFreehand: {
      if (a.points.empty()) {
        return {};
      }
      PhiRectF r = {a.points[0].x, a.points[0].y, a.points[0].x,
                    a.points[0].y};
      for (const PhiPointF& p : a.points) {
        r.left = std::min(r.left, p.x);
        r.top = std::min(r.top, p.y);
        r.right = std::max(r.right, p.x);
        r.bottom = std::max(r.bottom, p.y);
      }
      return r;
    }
    default:
      return Normalized(a.frame);
  }
}

PhiRectF PhiAnnotationBounds(const PhiAnnotation& a) {
  const PhiRectF box = PhiAnnotationBox(a);
  switch (a.kind) {
    case PhiAnnotationKind::kArrow:
      // The wings reach back from the tip by up to the head's length.
      return PhiRectInflate(box,
                            PhiArrowHeadLength(a, 1e9) * 0.75 + a.stroke);
    case PhiAnnotationKind::kFreehand:
    case PhiAnnotationKind::kRectangle:
    case PhiAnnotationKind::kEllipse:
      return PhiRectInflate(box, a.stroke / 2);
    default:
      return box;
  }
}

bool PhiAnnotationHit(const PhiAnnotation& a, PhiPointF p, double tolerance) {
  const double reach = a.stroke / 2 + tolerance;
  switch (a.kind) {
    case PhiAnnotationKind::kArrow:
      return SegmentDistance(p, a.from, a.to) <= reach;
    case PhiAnnotationKind::kFreehand: {
      if (a.points.size() == 1) {
        return std::sqrt(Dist2(p, a.points[0])) <= reach;
      }
      for (size_t i = 1; i < a.points.size(); ++i) {
        if (SegmentDistance(p, a.points[i - 1], a.points[i]) <= reach) {
          return true;
        }
      }
      return false;
    }
    case PhiAnnotationKind::kRectangle: {
      const PhiRectF box = Normalized(a.frame);
      if (!PhiRectInflate(box, reach).Contains(p)) {
        return false;
      }
      if (a.fill) {
        return true;
      }
      const PhiRectF inner = PhiRectInflate(box, -reach);
      return inner.empty() || !inner.Contains(p);
    }
    case PhiAnnotationKind::kEllipse: {
      const PhiRectF box = Normalized(a.frame);
      const double rx = box.width() / 2;
      const double ry = box.height() / 2;
      const double cx = box.left + rx;
      const double cy = box.top + ry;
      if (rx <= 0 || ry <= 0) {
        return SegmentDistance(p, {box.left, box.top},
                               {box.right, box.bottom}) <= reach;
      }
      const double nx = (p.x - cx) / rx;
      const double ny = (p.y - cy) / ry;
      const double e = std::sqrt(nx * nx + ny * ny);
      if (a.fill && e <= 1) {
        return true;
      }
      return std::abs(e - 1) * std::min(rx, ry) <= reach;
    }
    case PhiAnnotationKind::kText:
    case PhiAnnotationKind::kBlur:
      return PhiRectInflate(Normalized(a.frame), tolerance).Contains(p);
  }
  return false;
}

void PhiAnnotationMove(PhiAnnotation* a, double dx, double dy) {
  a->frame.left += dx;
  a->frame.right += dx;
  a->frame.top += dy;
  a->frame.bottom += dy;
  a->from.x += dx;
  a->from.y += dy;
  a->to.x += dx;
  a->to.y += dy;
  for (PhiPointF& p : a->points) {
    p.x += dx;
    p.y += dy;
  }
}

void PhiAnnotationResize(PhiAnnotation* a,
                         const PhiRectF& from,
                         const PhiRectF& to) {
  if (a->kind == PhiAnnotationKind::kText) {
    // Width only: the text reflows at its size, its height follows.
    PhiRectF next = Normalized(to);
    a->frame.left = next.left;
    a->frame.right = std::max(next.right, next.left + 1);
    return;
  }
  const double sx = from.width() != 0 ? to.width() / from.width() : 1;
  const double sy = from.height() != 0 ? to.height() / from.height() : 1;
  auto map = [&](PhiPointF p) {
    return PhiPointF{to.left + (p.x - from.left) * sx,
                     to.top + (p.y - from.top) * sy};
  };
  const PhiPointF tl = map({a->frame.left, a->frame.top});
  const PhiPointF br = map({a->frame.right, a->frame.bottom});
  a->frame = PhiRectFromPoints(tl, br);
  a->from = map(a->from);
  a->to = map(a->to);
  for (PhiPointF& p : a->points) {
    p = map(p);
  }
}

bool PhiAnnotationAdjustSize(PhiAnnotation* a, double delta) {
  if (a->kind == PhiAnnotationKind::kBlur) {
    return false;
  }
  double* value =
      a->kind == PhiAnnotationKind::kText ? &a->font_size : &a->stroke;
  const double next = *value + delta;
  if (next < 1) {
    // A value already under the minimum is kept on a decrement.
    if (*value > 1) {
      *value = 1;
      return true;
    }
    return false;
  }
  *value = next;
  return true;
}

void PhiFreehandCurve(const std::vector<PhiPointF>& points,
                      std::vector<PhiPointF>* out) {
  out->clear();
  if (points.empty()) {
    return;
  }
  out->push_back(points[0]);
  if (points.size() == 1) {
    return;
  }
  auto mid = [](PhiPointF a, PhiPointF b) {
    return PhiPointF{(a.x + b.x) / 2, (a.y + b.y) / 2};
  };
  if (points.size() == 2) {
    out->push_back(mid(points[0], points[1]));
    out->push_back(points[1]);
    return;
  }
  for (size_t i = 1; i + 1 < points.size(); ++i) {
    out->push_back(points[i]);
    out->push_back(mid(points[i], points[i + 1]));
  }
  // The last half segment, straight, ending exactly on the last sample.
  const PhiPointF last = points.back();
  out->push_back(mid(out->back(), last));
  out->push_back(last);
}

void PhiBlurBlocks(const PhiScreenshotImage& image,
                   const PhiRectF& frame,
                   double block,
                   std::vector<PhiBlurBlock>* out) {
  out->clear();
  const PhiRectF box = Normalized(frame);
  const PhiRectF bounds = {0, 0, static_cast<double>(image.width),
                           static_cast<double>(image.height)};
  if (box.empty() || block <= 0 || image.width <= 0 || image.height <= 0) {
    return;
  }
  const size_t stride = static_cast<size_t>(image.width) * 4;
  for (double y = box.top; y < box.bottom; y += block) {
    for (double x = box.left; x < box.right; x += block) {
      const PhiRectF cell = {x, y, std::min(x + block, box.right),
                             std::min(y + block, box.bottom)};
      const PhiRectF on_image = PhiRectIntersect(cell, bounds);
      if (on_image.empty()) {
        continue;
      }
      const int x0 = std::clamp(static_cast<int>(std::floor(on_image.left)), 0,
                                image.width - 1);
      const int y0 = std::clamp(static_cast<int>(std::floor(on_image.top)), 0,
                                image.height - 1);
      const int x1 = std::clamp(static_cast<int>(std::ceil(on_image.right)),
                                x0 + 1, image.width);
      const int y1 = std::clamp(static_cast<int>(std::ceil(on_image.bottom)),
                                y0 + 1, image.height);
      // Sample at most ~16x16 points per block: enough for an average.
      const int step_x = std::max(1, (x1 - x0) / 16);
      const int step_y = std::max(1, (y1 - y0) / 16);
      uint64_t sum[4] = {0, 0, 0, 0};
      uint64_t n = 0;
      for (int py = y0; py < y1; py += step_y) {
        const size_t row = static_cast<size_t>(py) * stride;
        for (int px = x0; px < x1; px += step_x) {
          const size_t at = row + static_cast<size_t>(px) * 4;
          for (size_t c = 0; c < 4; ++c) {
            sum[c] += image.bgra[at + c];
          }
          ++n;
        }
      }
      if (!n) {
        continue;
      }
      uint32_t bgra = 0;
      for (size_t c = 0; c < 4; ++c) {
        bgra |= static_cast<uint32_t>((sum[c] + n / 2) / n) << (8 * c);
      }
      // Blocks are opaque: a page area painted transparent reads as white.
      const uint32_t alpha = bgra >> 24;
      if (alpha < 255) {
        uint32_t out_px = 0xFF000000u;
        for (int c = 0; c < 3; ++c) {
          const uint32_t v = (bgra >> (8 * c)) & 0xFF;
          out_px |= std::min<uint32_t>(255, v + (255 - alpha)) << (8 * c);
        }
        bgra = out_px;
      }
      out->push_back({cell, bgra});
    }
  }
}

PhiScreenshotImageRef PhiCropImage(const PhiScreenshotImage& image,
                                   const PhiRectF& rect) {
  const PhiRectF box = Normalized(rect);
  const int x0 = std::clamp(static_cast<int>(std::lround(box.left)), 0,
                            image.width);
  const int y0 = std::clamp(static_cast<int>(std::lround(box.top)), 0,
                            image.height);
  const int x1 = std::clamp(static_cast<int>(std::lround(box.right)), 0,
                            image.width);
  const int y1 = std::clamp(static_cast<int>(std::lround(box.bottom)), 0,
                            image.height);
  if (x1 <= x0 || y1 <= y0) {
    return nullptr;
  }
  auto out = std::make_shared<PhiScreenshotImage>();
  out->width = x1 - x0;
  out->height = y1 - y0;
  out->bgra.resize(static_cast<size_t>(out->width) * out->height * 4);
  const size_t src_stride = static_cast<size_t>(image.width) * 4;
  const size_t dst_stride = static_cast<size_t>(out->width) * 4;
  for (int y = 0; y < out->height; ++y) {
    const size_t src = static_cast<size_t>(y0 + y) * src_stride +
                       static_cast<size_t>(x0) * 4;
    std::copy(image.bgra.begin() + static_cast<ptrdiff_t>(src),
              image.bgra.begin() + static_cast<ptrdiff_t>(src + dst_stride),
              out->bgra.begin() +
                  static_cast<ptrdiff_t>(static_cast<size_t>(y) * dst_stride));
  }
  return out;
}

// --- The document --------------------------------------------------------------------

bool PhiScreenshotStatesEqual(const PhiScreenshotDocument::State& a,
                              const PhiScreenshotDocument::State& b) {
  if (a.image != b.image || a.annotations.size() != b.annotations.size()) {
    return false;
  }
  for (size_t i = 0; i < a.annotations.size(); ++i) {
    if (!SameAnnotation(a.annotations[i], b.annotations[i])) {
      return false;
    }
  }
  return true;
}

PhiScreenshotDocument::PhiScreenshotDocument(PhiScreenshotImageRef image) {
  state_.image = std::move(image);
  state_.generation = 0;
}

PhiScreenshotDocument::~PhiScreenshotDocument() = default;

void PhiScreenshotDocument::Commit(State before) {
  if (PhiScreenshotStatesEqual(before, state_)) {
    state_.generation = before.generation;
    return;
  }
  state_.generation = next_generation_++;
  undo_.push_back(std::move(before));
  if (undo_.size() > kMaxUndo) {
    undo_.erase(undo_.begin());
  }
  redo_.clear();
}

PhiAnnotation* PhiScreenshotDocument::Find(uint64_t id) {
  for (PhiAnnotation& a : state_.annotations) {
    if (a.id == id) {
      return &a;
    }
  }
  return nullptr;
}

const PhiAnnotation* PhiScreenshotDocument::Find(uint64_t id) const {
  for (const PhiAnnotation& a : state_.annotations) {
    if (a.id == id) {
      return &a;
    }
  }
  return nullptr;
}

uint64_t PhiScreenshotDocument::Add(PhiAnnotation a) {
  a.id = next_id_++;
  state_.annotations.push_back(std::move(a));
  return state_.annotations.back().id;
}

void PhiScreenshotDocument::Remove(const std::vector<uint64_t>& ids) {
  std::erase_if(state_.annotations, [&](const PhiAnnotation& a) {
    return std::find(ids.begin(), ids.end(), a.id) != ids.end();
  });
}

std::vector<uint64_t> PhiScreenshotDocument::Duplicate(
    const std::vector<uint64_t>& ids,
    double offset) {
  std::vector<PhiAnnotation> copies;
  for (const PhiAnnotation& a : state_.annotations) {
    if (std::find(ids.begin(), ids.end(), a.id) != ids.end()) {
      copies.push_back(a);
      PhiAnnotationMove(&copies.back(), offset, offset);
    }
  }
  std::vector<uint64_t> added;
  for (PhiAnnotation& a : copies) {
    added.push_back(Add(std::move(a)));
  }
  return added;
}

void PhiScreenshotDocument::BringToFront(const std::vector<uint64_t>& ids) {
  std::stable_partition(
      state_.annotations.begin(), state_.annotations.end(),
      [&](const PhiAnnotation& a) {
        return std::find(ids.begin(), ids.end(), a.id) == ids.end();
      });
}

void PhiScreenshotDocument::SendToBack(const std::vector<uint64_t>& ids) {
  std::stable_partition(
      state_.annotations.begin(), state_.annotations.end(),
      [&](const PhiAnnotation& a) {
        return std::find(ids.begin(), ids.end(), a.id) != ids.end();
      });
}

bool PhiScreenshotDocument::Crop(const PhiRectF& rect) {
  PhiScreenshotImageRef cropped = PhiCropImage(*state_.image, rect);
  if (!cropped) {
    return false;
  }
  // The crop's own pixel origin (what PhiCropImage rounded to).
  const PhiRectF box = Normalized(rect);
  const double dx = -std::clamp(static_cast<double>(std::lround(box.left)),
                                0.0, static_cast<double>(image().width));
  const double dy = -std::clamp(static_cast<double>(std::lround(box.top)),
                                0.0, static_cast<double>(image().height));
  const PhiRectF canvas = {0, 0, static_cast<double>(cropped->width),
                           static_cast<double>(cropped->height)};
  for (PhiAnnotation& a : state_.annotations) {
    PhiAnnotationMove(&a, dx, dy);
  }
  std::erase_if(state_.annotations, [&](const PhiAnnotation& a) {
    return !PhiRectsIntersect(PhiAnnotationBounds(a), canvas);
  });
  state_.image = std::move(cropped);
  return true;
}

uint64_t PhiScreenshotDocument::HitTest(PhiPointF p, double tolerance) const {
  for (auto it = state_.annotations.rbegin(); it != state_.annotations.rend();
       ++it) {
    if (PhiAnnotationHit(*it, p, tolerance)) {
      return it->id;
    }
  }
  return 0;
}

std::vector<uint64_t> PhiScreenshotDocument::InRect(
    const PhiRectF& rect) const {
  std::vector<uint64_t> ids;
  for (const PhiAnnotation& a : state_.annotations) {
    if (PhiRectsIntersect(PhiAnnotationBounds(a), rect)) {
      ids.push_back(a.id);
    }
  }
  return ids;
}

bool PhiScreenshotDocument::Undo() {
  if (undo_.empty()) {
    return false;
  }
  redo_.push_back(std::move(state_));
  state_ = std::move(undo_.back());
  undo_.pop_back();
  return true;
}

bool PhiScreenshotDocument::Redo() {
  if (redo_.empty()) {
    return false;
  }
  undo_.push_back(std::move(state_));
  state_ = std::move(redo_.back());
  redo_.pop_back();
  return true;
}

}  // namespace phi_client

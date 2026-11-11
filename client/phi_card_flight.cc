// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_card_flight.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "client/phi_panel.h"

namespace phi_client {

namespace {

constexpr UINT kFrameMs = 8;
constexpr wchar_t kFlightClass[] = L"PhiCardFlight";

struct Flight {
  int id = 0;
  HWND hwnd = nullptr;
  bool in = true;
  RECT small = {};   // The link-sized end of the flight.
  RECT full = {};    // The card's own place.
  PhiCardFlight::Look look;
  std::function<void()> landed;
  std::function<void(double)> progress;
  ULONGLONG start = 0;
  bool done = false;      // Landed; the window lingers one more frame.
  // FlyOut: what the card looked like, at `full`'s size.
  HDC snapshot_dc = nullptr;
  HBITMAP snapshot = nullptr;
  HGDIOBJ snapshot_previous = nullptr;
};

std::vector<std::unique_ptr<Flight>>& Flights() {
  static std::vector<std::unique_ptr<Flight>> flights;
  return flights;
}

UINT_PTR g_timer = 0;
int g_next_id = 1;

void Release(Flight& flight) {
  if (flight.snapshot_dc) {
    if (flight.snapshot_previous) {
      ::SelectObject(flight.snapshot_dc, flight.snapshot_previous);
    }
    ::DeleteDC(flight.snapshot_dc);
    flight.snapshot_dc = nullptr;
  }
  if (flight.snapshot) {
    ::DeleteObject(flight.snapshot);
    flight.snapshot = nullptr;
  }
  if (flight.hwnd && ::IsWindow(flight.hwnd)) {
    ::DestroyWindow(flight.hwnd);
  }
  flight.hwnd = nullptr;
}

RECT Lerp(const RECT& a, const RECT& b, double t) {
  const auto mix = [t](LONG from, LONG to) {
    return static_cast<LONG>(std::lround(from + (to - from) * t));
  };
  return {mix(a.left, b.left), mix(a.top, b.top), mix(a.right, b.right),
          mix(a.bottom, b.bottom)};
}

// The small end: link-sized at the press, kept inside the card, with the
// card's own proportions; or the card at 97 % about its centre.
RECT SmallRect(const RECT& full, const PhiCardFlight::Anchor& anchor) {
  const double width = full.right - full.left;
  const double height = full.bottom - full.top;
  double start_w = width * 0.97;
  double cx = (full.left + full.right) / 2.0;
  double cy = (full.top + full.bottom) / 2.0;
  if (anchor.has_origin && anchor.start_width_px > 0 &&
      anchor.start_width_px < width) {
    start_w = anchor.start_width_px;
    const double start_h = height * start_w / width;
    cx = std::clamp(static_cast<double>(anchor.origin.x),
                    full.left + start_w / 2.0, full.right - start_w / 2.0);
    cy = std::clamp(static_cast<double>(anchor.origin.y),
                    full.top + start_h / 2.0, full.bottom - start_h / 2.0);
  }
  const double start_h = height * start_w / width;
  return {static_cast<LONG>(std::lround(cx - start_w / 2.0)),
          static_cast<LONG>(std::lround(cy - start_h / 2.0)),
          static_cast<LONG>(std::lround(cx + start_w / 2.0)),
          static_cast<LONG>(std::lround(cy + start_h / 2.0))};
}

// One frame: the card at `card` (screen), with its shadow, at `opacity`.
void Draw(Flight& flight, const RECT& card, double opacity) {
  const PhiCardFlight::Look& look = flight.look;
  const int margin = look.shadow_alpha > 0.0 ? look.shadow_margin_px : 0;
  const int card_w = std::max(1L, card.right - card.left);
  const int card_h = std::max(1L, card.bottom - card.top);
  // The surface: the card and its shadow's room, or the whole dimmed area.
  const bool dimmed = !flight.in && look.scrim_alpha > 0.0 &&
                      look.scrim.right > look.scrim.left &&
                      look.scrim.bottom > look.scrim.top;
  RECT surface = {card.left - margin, card.top - margin, card.right + margin,
                  card.bottom + margin};
  if (dimmed) {
    surface = look.scrim;
  }
  const int width = surface.right - surface.left;
  const int height = surface.bottom - surface.top;
  PhiLayerSurface layer(width, height);
  if (!layer.valid()) {
    return;
  }
  const int ox = card.left - surface.left;  // The card inside the surface.
  const int oy = card.top - surface.top;
  // The corner radius shrinks with the card, so a link-sized card is not a
  // lozenge.
  const double scale =
      static_cast<double>(card_w) /
      std::max(1L, flight.full.right - flight.full.left);
  const double radius =
      std::min({look.radius_px * std::max(scale, 0.35), card_w / 2.0,
                card_h / 2.0});
  const double blur = std::max(1.0, look.shadow_blur_px * std::max(scale, 0.35));
  if (flight.snapshot_dc) {
    // A small card (a menu, a popover) is worth the smoother filter; a
    // page-sized one would cost a frame.
    const bool small = static_cast<int64_t>(card_w) * card_h < 1200 * 1000;
    ::SetStretchBltMode(layer.dc(), small ? HALFTONE : COLORONCOLOR);
    if (small) {
      ::SetBrushOrgEx(layer.dc(), 0, 0, nullptr);
    }
    ::StretchBlt(layer.dc(), ox, oy, card_w, card_h, flight.snapshot_dc, 0, 0,
                 flight.full.right - flight.full.left,
                 flight.full.bottom - flight.full.top, SRCCOPY);
    ::GdiFlush();
  }
  // SAFETY: the layer holds exactly width*height 32-bit pixels.
  const base::span<uint32_t> pixels = UNSAFE_BUFFERS(base::span<uint32_t>(
      static_cast<uint32_t*>(layer.bits()),
      static_cast<size_t>(width) * height));
  const bool snapshot = flight.snapshot_dc != nullptr;
  const double scrim = dimmed ? look.scrim_alpha : 0.0;
  const uint32_t ground =
      static_cast<uint32_t>(scrim * 255.0 + 0.5) << 24;  // Premultiplied black.
  const uint32_t fill = 0xFF000000u | (GetRValue(look.fill) << 16) |
                        (GetGValue(look.fill) << 8) | GetBValue(look.fill);
  // Where the card and its shadow can reach; everything else is ground.
  const int reach = margin;
  const int x0 = std::clamp(ox - reach, 0, width);
  const int x1 = std::clamp(ox + card_w + reach, 0, width);
  const int y0 = std::clamp(oy - reach, 0, height);
  const int y1 = std::clamp(oy + card_h + reach, 0, height);
  for (int y = 0; y < height; ++y) {
    uint32_t* row = &pixels[static_cast<size_t>(y) * width];
    if (y < y0 || y >= y1) {
      std::fill(row, UNSAFE_BUFFERS(row + width), ground);
      continue;
    }
    std::fill(row, UNSAFE_BUFFERS(row + x0), ground);
    std::fill(UNSAFE_BUFFERS(row + x1), UNSAFE_BUFFERS(row + width), ground);
  }
  const double cx = ox + card_w / 2.0;
  const double cy = oy + card_h / 2.0;
  const double half_x = card_w / 2.0 - radius;
  const double half_y = card_h / 2.0 - radius;
  const int inner = static_cast<int>(std::ceil(radius)) + 2;
  for (int y = y0; y < y1; ++y) {
    const bool edge_row = y < oy + inner || y >= oy + card_h - inner;
    for (int x = x0; x < x1; ++x) {
      if (!edge_row && x == ox + inner) {
        // The solid middle: the card's own pixels, opaque.
        const int end = std::clamp(ox + card_w - inner, x, x1);
        for (; x < end; ++x) {
          uint32_t& pixel = pixels[static_cast<size_t>(y) * width + x];
          pixel = snapshot ? (pixel | 0xFF000000u) : fill;
        }
        if (x >= x1) {
          break;
        }
      }
      const double dx = std::abs(x + 0.5 - cx) - half_x;
      const double dy = std::abs(y + 0.5 - cy) - half_y;
      const auto distance_at = [&](double ddy) {
        if (dx <= 0.0 && ddy <= 0.0) {
          return std::max(dx, ddy) - radius;
        }
        const double px = std::max(dx, 0.0);
        const double py = std::max(ddy, 0.0);
        return std::sqrt(px * px + py * py) - radius;
      };
      const double distance = distance_at(dy);
      const double cover = std::clamp(0.5 - distance, 0.0, 1.0);
      double shadow = 0.0;
      if (look.shadow_alpha > 0.0 && cover < 1.0) {
        // The shadow is the same silhouette, a little lower.
        const double sdy =
            std::abs(y + 0.5 - cy - look.shadow_offset_y_px * scale) - half_y;
        const double t = std::clamp(0.5 - distance_at(sdy) / blur, 0.0, 1.0);
        shadow = look.shadow_alpha * t * t * (3.0 - 2.0 * t);
      }
      uint32_t& pixel = pixels[static_cast<size_t>(y) * width + x];
      uint32_t source = snapshot ? pixel : fill;
      if (!snapshot && distance > -1.5) {
        // The hairline stroke: the card's outermost pixel.
        const double rim = std::clamp(distance + 1.5, 0.0, 1.0);
        const auto mix = [rim](int from, int to) {
          return static_cast<uint32_t>(from + (to - from) * rim + 0.5);
        };
        source = 0xFF000000u |
                 (mix(GetRValue(look.fill), GetRValue(look.stroke)) << 16) |
                 (mix(GetGValue(look.fill), GetGValue(look.stroke)) << 8) |
                 mix(GetBValue(look.fill), GetBValue(look.stroke));
      }
      // The card over its shadow over the dim, premultiplied (the two under
      // it are black, so only their alphas count).
      const double under = shadow + (1.0 - shadow) * scrim;
      const double alpha = cover + (1.0 - cover) * under;
      const auto channel = [&](int shift) {
        return static_cast<uint32_t>(((source >> shift) & 0xFF) * cover + 0.5);
      };
      pixel = (static_cast<uint32_t>(alpha * 255.0 + 0.5) << 24) |
              (channel(16) << 16) | (channel(8) << 8) | channel(0);
    }
  }
  if (dimmed && look.scrim_radius_px > 0) {
    // The dimmed area's own rounded corners stay clear.
    const double r = look.scrim_radius_px;
    const int corner = std::min({look.scrim_radius_px, width / 2, height / 2});
    for (int y = 0; y < corner; ++y) {
      for (int x = 0; x < corner; ++x) {
        const double px = r - (x + 0.5);
        const double py = r - (y + 0.5);
        const double keep =
            std::clamp(0.5 - (std::sqrt(px * px + py * py) - r), 0.0, 1.0);
        const uint32_t value =
            static_cast<uint32_t>(scrim * keep * 255.0 + 0.5) << 24;
        pixels[static_cast<size_t>(y) * width + x] = value;
        pixels[static_cast<size_t>(y) * width + (width - 1 - x)] = value;
        pixels[static_cast<size_t>(height - 1 - y) * width + x] = value;
        pixels[static_cast<size_t>(height - 1 - y) * width + (width - 1 - x)] =
            value;
      }
    }
  }
  const POINT origin = {surface.left, surface.top};
  layer.Commit(flight.hwnd, &origin,
               static_cast<BYTE>(std::clamp(opacity, 0.0, 1.0) * 255.0 + 0.5));
  if (!::IsWindowVisible(flight.hwnd)) {
    ::ShowWindow(flight.hwnd, SW_SHOWNOACTIVATE);
  }
}

void CALLBACK OnFrame(HWND, UINT, UINT_PTR, DWORD) {
  std::vector<std::unique_ptr<Flight>>& flights = Flights();
  const ULONGLONG now = ::GetTickCount64();
  // Landed callbacks run after the walk: one may start or cancel a flight.
  std::vector<std::function<void()>> landed;
  for (auto it = flights.begin(); it != flights.end();) {
    Flight& flight = **it;
    if (flight.done || !flight.hwnd || !::IsWindow(flight.hwnd)) {
      Release(flight);
      it = flights.erase(it);
      continue;
    }
    const double t = std::min(
        1.0, static_cast<double>(now - flight.start) /
                 static_cast<double>(std::max(1, flight.look.duration_ms)));
    if (flight.in) {
      const double eased = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
      Draw(flight, Lerp(flight.small, flight.full, eased), eased);
      if (flight.progress) {
        flight.progress(eased);
      }
    } else {
      const double eased = t * t;
      Draw(flight, Lerp(flight.full, flight.small, eased), 1.0 - eased);
      if (flight.progress) {
        flight.progress(eased);
      }
    }
    if (t >= 1.0) {
      // The real card goes up under the last frame, which stays one tick
      // more so there is no gap between the two.
      flight.done = true;
      if (flight.landed) {
        landed.push_back(std::move(flight.landed));
        flight.landed = nullptr;
      }
    }
    ++it;
  }
  if (flights.empty() && g_timer) {
    ::KillTimer(nullptr, g_timer);
    g_timer = 0;
  }
  for (std::function<void()>& callback : landed) {
    callback();
  }
}

Flight* Start(HWND owner,
              const RECT& full,
              const PhiCardFlight::Anchor& anchor,
              const PhiCardFlight::Look& look) {
  if (full.right - full.left < 8 || full.bottom - full.top < 8) {
    return nullptr;
  }
  static bool registered = false;
  if (!registered) {
    registered = true;
    WNDCLASSW cls = {};
    cls.lpfnWndProc = ::DefWindowProcW;
    cls.hInstance = ::GetModuleHandleW(nullptr);
    cls.lpszClassName = kFlightClass;
    ::RegisterClassW(&cls);
  }
  const HWND root = owner ? ::GetAncestor(owner, GA_ROOT) : nullptr;
  const HWND hwnd = ::CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
          (look.topmost ? WS_EX_TOPMOST : 0),
      kFlightClass, L"", WS_POPUP, full.left, full.top, 1, 1, root, nullptr,
      ::GetModuleHandleW(nullptr), nullptr);
  if (!hwnd) {
    return nullptr;
  }
  auto flight = std::make_unique<Flight>();
  flight->id = g_next_id++;
  flight->hwnd = hwnd;
  flight->full = full;
  flight->small = SmallRect(full, anchor);
  flight->look = look;
  flight->start = ::GetTickCount64();
  Flight* raw = flight.get();
  Flights().push_back(std::move(flight));
  if (!g_timer) {
    g_timer = ::SetTimer(nullptr, 0, kFrameMs, &OnFrame);
  }
  if (!g_timer) {
    Release(*raw);
    Flights().pop_back();
    return nullptr;
  }
  return raw;
}

}  // namespace

// static
int PhiCardFlight::FlyIn(HWND owner,
                         const RECT& target,
                         const Anchor& anchor,
                         const Look& look,
                         std::function<void()> landed,
                         std::function<void(double)> progress) {
  Flight* flight = Start(owner, target, anchor, look);
  if (!flight) {
    return 0;
  }
  flight->in = true;
  flight->landed = std::move(landed);
  flight->progress = std::move(progress);
  Draw(*flight, flight->small, 0.0);
  return flight->id;
}

// static
int PhiCardFlight::FlyInSnapshot(HWND owner,
                                 const RECT& target,
                                 const Anchor& anchor,
                                 const Look& look,
                                 HBITMAP snapshot,
                                 std::function<void()> landed) {
  HDC screen = ::GetDC(nullptr);
  HDC memory = snapshot ? ::CreateCompatibleDC(screen) : nullptr;
  ::ReleaseDC(nullptr, screen);
  Flight* flight = memory ? Start(owner, target, anchor, look) : nullptr;
  if (!flight) {
    if (memory) ::DeleteDC(memory);
    if (snapshot) ::DeleteObject(snapshot);
    return 0;
  }
  flight->in = true;
  flight->landed = std::move(landed);
  flight->snapshot_dc = memory;
  flight->snapshot = snapshot;
  flight->snapshot_previous = ::SelectObject(memory, snapshot);
  Draw(*flight, flight->small, 0.0);
  return flight->id;
}

// static
int PhiCardFlight::FlyOut(HWND owner,
                          const RECT& from,
                          const Anchor& anchor,
                          const Look& look,
                          std::function<void(double)> progress) {
  const int width = from.right - from.left;
  const int height = from.bottom - from.top;
  if (width < 8 || height < 8) {
    return 0;
  }
  // What is on screen there now, the hosted page included.
  HDC screen = ::GetDC(nullptr);
  HDC memory = ::CreateCompatibleDC(screen);
  HBITMAP bitmap = ::CreateCompatibleBitmap(screen, width, height);
  if (!memory || !bitmap) {
    if (bitmap) ::DeleteObject(bitmap);
    if (memory) ::DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);
    return 0;
  }
  HGDIOBJ previous = ::SelectObject(memory, bitmap);
  ::BitBlt(memory, 0, 0, width, height, screen, from.left, from.top, SRCCOPY);
  ::ReleaseDC(nullptr, screen);
  Flight* flight = Start(owner, from, anchor, look);
  if (!flight) {
    ::SelectObject(memory, previous);
    ::DeleteObject(bitmap);
    ::DeleteDC(memory);
    return 0;
  }
  flight->in = false;
  flight->snapshot_dc = memory;
  flight->snapshot = bitmap;
  flight->snapshot_previous = previous;
  flight->progress = std::move(progress);
  Draw(*flight, flight->full, 1.0);
  return flight->id;
}

// static
void PhiCardFlight::Detach(int id) {
  if (id == 0) {
    return;
  }
  for (std::unique_ptr<Flight>& flight : Flights()) {
    if (flight->id == id) {
      flight->landed = nullptr;
      flight->progress = nullptr;
    }
  }
}

// static
void PhiCardFlight::Cancel(int id) {
  if (id == 0) {
    return;
  }
  for (std::unique_ptr<Flight>& flight : Flights()) {
    if (flight->id == id) {
      flight->landed = nullptr;
      flight->progress = nullptr;
      flight->done = true;  // Released on the next frame.
    }
  }
}

}  // namespace phi_client

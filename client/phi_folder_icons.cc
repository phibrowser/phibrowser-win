// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_folder_icons.h"

#include <algorithm>
#include <cmath>

#include "base/no_destructor.h"

namespace phi_client {

const std::vector<PhiFolderIcon>& PhiFolderIconsAll() {
  // BookmarkFolderIcon.allCases order — the picker reads left to right in
  // exactly this sequence on both clients.
  static const base::NoDestructor<std::vector<PhiFolderIcon>> catalog({
      {"default", "Default"},
      {"github", "GitHub"},
      {"discord", "Discord"},
      {"x", "X"},
      {"reddit", "Reddit"},
      {"facebook", "Facebook"},
      {"emoji-happy", "Happy"},
      {"emoji-sad", "Sad"},
      {"key", "Key"},
      {"download", "Download"},
      {"thumb-up", "Thumb Up"},
      {"chart-bar", "Chart"},
      {"location-marker", "Location"},
      {"currency-dollar", "Currency"},
      {"shield-check", "Shield"},
      {"heart", "Heart"},
      {"bookmark", "Bookmark"},
      {"refresh", "Refresh"},
      {"chat", "Chat"},
      {"fire", "Fire"},
      {"puzzle", "Puzzle"},
      {"variable", "Variable"},
      {"save", "Save"},
      {"backspace", "Backspace"},
  });
  return *catalog;
}

const char* PhiFolderIconDefaultId() {
  return "default";
}

const PhiMacIcon* PhiPlainFolderMask(bool expanded) {
  return PhiFolderIconMask(expanded ? "folder-open" : "folder-closed");
}

std::string PhiResolveFolderIcon(const std::string& stored) {
  const std::vector<PhiFolderIcon>& all = PhiFolderIconsAll();
  const bool known = std::any_of(
      all.begin(), all.end(),
      [&](const PhiFolderIcon& icon) { return stored == icon.id; });
  return known ? stored : std::string(PhiFolderIconDefaultId());
}

namespace {

struct Hsb {
  double h, s, b;  // 0..1 each, the NSColor calibrated components.
};

Hsb ToHsb(COLORREF c) {
  const double r = GetRValue(c) / 255.0;
  const double g = GetGValue(c) / 255.0;
  const double b = GetBValue(c) / 255.0;
  const double hi = std::max({r, g, b});
  const double lo = std::min({r, g, b});
  Hsb out = {0.0, hi > 0 ? (hi - lo) / hi : 0.0, hi};
  const double d = hi - lo;
  if (d > 0) {
    if (hi == r) {
      out.h = std::fmod((g - b) / d, 6.0);
    } else if (hi == g) {
      out.h = (b - r) / d + 2.0;
    } else {
      out.h = (r - g) / d + 4.0;
    }
    out.h /= 6.0;
    if (out.h < 0) {
      out.h += 1.0;
    }
  }
  return out;
}

COLORREF FromHsb(double h, double s, double v) {
  s = std::clamp(s, 0.0, 1.0);
  v = std::clamp(v, 0.0, 1.0);
  h = h - std::floor(h);
  const double c = v * s;
  const double x = c * (1.0 - std::fabs(std::fmod(h * 6.0, 2.0) - 1.0));
  const double m = v - c;
  double r = 0, g = 0, b = 0;
  const int sector = static_cast<int>(h * 6.0) % 6;
  switch (sector) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
  }
  const auto byte = [&](double f) {
    return static_cast<int>(std::lround(std::clamp(f + m, 0.0, 1.0) * 255));
  };
  return RGB(byte(r), byte(g), byte(b));
}

// Premultiplied "over": `layer` onto `dst`, both px*px BGRA.
void Over(std::vector<unsigned char>* dst,
          const std::vector<unsigned char>& layer) {
  if (layer.size() != dst->size()) {
    return;
  }
  for (size_t i = 0; i + 3 < layer.size(); i += 4) {
    const unsigned a = layer[i + 3];
    for (size_t k = 0; k < 4; ++k) {
      (*dst)[i + k] = static_cast<unsigned char>(
          std::min(255u, layer[i + k] + (*dst)[i + k] * (255 - a) / 255));
    }
  }
}

}  // namespace

std::vector<unsigned char> PhiPlainFolderBgra(bool expanded,
                                              int px,
                                              COLORREF accent,
                                              bool dark,
                                              COLORREF window_bg) {
  if (px <= 0) {
    return {};
  }
  // BookmarkFolderIconPalette(theme:appearance:).
  const Hsb a = ToHsb(accent);
  COLORREF back, front, stroke;
  if (dark) {
    back = window_bg;
    front = FromHsb(a.h, a.s + 0.07, a.b - 0.24);
    stroke = RGB(0xFF, 0xFF, 0xFF);
  } else {
    back = FromHsb(a.h, 0.65, a.b - 0.15);
    front = FromHsb(a.h, 0.20, 1.00);
    stroke = FromHsb(a.h, 0.65, 0.30);
  }
  const std::string state = expanded ? "folder-open" : "folder-closed";
  const struct {
    const char* layer;
    COLORREF color;
  } layers[] = {{"-back", back},
                {"-back-stroke", stroke},
                {"-front", front},
                {"-front-stroke", stroke}};
  std::vector<unsigned char> out(static_cast<size_t>(px) * px * 4, 0);
  for (const auto& layer : layers) {
    const PhiMacIcon* mask = PhiFolderIconMask(state + layer.layer);
    if (!mask) {
      return {};  // Masks ship together; a missing one means a stale table.
    }
    Over(&out, PhiTintedIconBgra(*mask, layer.color, px));
  }
  return out;
}

}  // namespace phi_client

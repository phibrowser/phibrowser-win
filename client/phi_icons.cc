// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_icons.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string_view>

#include "base/compiler_specific.h"
#include "base/no_destructor.h"
#include "client/phi_agent_icons.h"
#include "client/phi_dwrite.h"
#include "base/strings/utf_string_conversions.h"

namespace phi_client {

const std::vector<PhiCatalogIcon>& PhiIconCatalogAll() {
  // Keep the shared catalog's ids, order and display stand-ins stable.
  // Every entry renders from its solid artwork mask, including the original
  // heavy letter, digit and punctuation shapes.
  static const base::NoDestructor<std::vector<PhiCatalogIcon>> icons({
      {"phi-icon-mail", "mail", 0xE715},              // Mail
      {"phi-icon-color-swatch", "color-swatch", 0xE790},  // Color
      {"phi-icon-camera", "camera", 0xE722},          // Camera
      {"phi-icon-chat", "chat", 0xE90A},              // Comment
      {"phi-icon-bell", "bell", 0xEA8F},              // Ringer
      {"phi-icon-user", "user", 0xE77B},              // Contact
      {"phi-icon-globe-alt", "globe-alt", 0xE774},    // Globe
      {"phi-icon-sun", "sun", 0xE706},                // Brightness
      {"phi-icon-sparkles", "sparkles", 0xE734},
      {"phi-icon-credit-card", "credit-card", 0xE8C7},  // PaymentCard
      {"phi-icon-ticket", "ticket", 0xE8EC},
      {"phi-icon-moon", "moon", 0xE708},              // QuietHours
      {"phi-icon-briefcase", "briefcase", 0xE821},    // Work
      {"phi-icon-archive", "archive", 0xE7B8},        // Package
      {"phi-icon-lock-closed", "lock-closed", 0xE72E},  // Lock
      {"phi-icon-office-building", "office-building", 0xEC06},  // CityNext
      {"phi-icon-chart-pie", "chart-pie", 0xEB05},    // PieSingle
      {"phi-icon-shield-exclamation", "shield-exclamation", 0xEA18},  // Shield
      {"phi-icon-photograph", "photograph", 0xE91B},  // Photo
      {"phi-icon-cog", "cog", 0xE713},                // Settings
      {"phi-icon-filter", "filter", 0xE71C},          // Filter
      {"phi-icon-qrcode", "qrcode", 0xED14},          // QRCode
      {"phi-icon-clipboard-list", "clipboard-list", 0xF0E3},  // ClipboardList
      {"phi-icon-lightning-bolt", "lightning-bolt", 0xE945},  // LightningBolt
      {"phi-icon-light-bulb", "light-bulb", 0xEA80},  // Lightbulb
      {"phi-icon-view-grid-add", "view-grid-add", 0xE8A9},  // ViewAll
      {"phi-icon-md-library", "md-library", 0xE8F1},  // Library
      {"phi-icon-play", "play", 0xE768},              // Play
      {"phi-icon-presentation-chart-line", "presentation-chart-line", 0xE9D2},  // AreaChart
      {"phi-icon-chip", "chip", 0xE7F8},
      {"phi-icon-cube-transparent", "cube-transparent", 0xE8B3},
      {"phi-icon-search-circle", "search-circle", 0xE721},  // Search
      {"phi-icon-cloud", "cloud", 0xE753},            // Cloud
      {"phi-icon-academic-cap", "academic-cap", 0xE7BE},  // Education
      {"phi-icon-gift", "gift", 0xF133},              // GiftboxOpen
      {"phi-icon-truck", "truck", 0xE7C0},
      {"phi-icon-cake", "cake", 0xE728},
      {"phi-icon-beaker", "beaker", 0xF196},          // Beaker
      {"phi-icon-table", "table", 0xE80A},
      {"phi-icon-paper-airplane", "paper-airplane", 0xE724},  // Send
      {"phi-icon-device-mobile", "device-mobile", 0xE8EA},  // CellPhone
      {"phi-icon-music-note", "music-note", 0xEC4F},  // MusicNote
      {"phi-icon-film", "film", 0xE714},              // Video
      {"phi-icon-database", "database", 0xE7C3},
      {"phi-icon-server", "server", 0xE83B},
      {"phi-icon-identification", "identification", 0xE779},  // ContactInfo
      {"phi-icon-rss", "rss", 0xE736},
      {"phi-icon-map", "map", 0xE707},                // MapPin
      {"phi-icon-number-1", "1", L'1'},
      {"phi-icon-number-2", "2", L'2'},
      {"phi-icon-number-3", "3", L'3'},
      {"phi-icon-number-4", "4", L'4'},
      {"phi-icon-number-5", "5", L'5'},
      {"phi-icon-number-6", "6", L'6'},
      {"phi-icon-number-7", "7", L'7'},
      {"phi-icon-number-8", "8", L'8'},
      {"phi-icon-number-9", "9", L'9'},
      {"phi-icon-number-0", "0", L'0'},
      {"phi-icon-letter-a", "A", L'A'},
      {"phi-icon-letter-b", "B", L'B'},
      {"phi-icon-letter-c", "C", L'C'},
      {"phi-icon-letter-d", "D", L'D'},
      {"phi-icon-letter-e", "E", L'E'},
      {"phi-icon-letter-f", "F", L'F'},
      {"phi-icon-letter-g", "G", L'G'},
      {"phi-icon-letter-h", "H", L'H'},
      {"phi-icon-letter-i", "I", L'I'},
      {"phi-icon-letter-j", "J", L'J'},
      {"phi-icon-letter-k", "K", L'K'},
      {"phi-icon-letter-l", "L", L'L'},
      {"phi-icon-letter-m", "M", L'M'},
      {"phi-icon-letter-n", "N", L'N'},
      {"phi-icon-letter-o", "O", L'O'},
      {"phi-icon-letter-p", "P", L'P'},
      {"phi-icon-letter-q", "Q", L'Q'},
      {"phi-icon-letter-r", "R", L'R'},
      {"phi-icon-letter-s", "S", L'S'},
      {"phi-icon-letter-t", "T", L'T'},
      {"phi-icon-letter-u", "U", L'U'},
      {"phi-icon-letter-v", "V", L'V'},
      {"phi-icon-letter-w", "W", L'W'},
      {"phi-icon-letter-x", "X", L'X'},
      {"phi-icon-letter-y", "Y", L'Y'},
      {"phi-icon-letter-z", "Z", L'Z'},
      {"phi-icon-question-mark", "?", 0xE897},        // Help
      {"phi-icon-exclamation-mark", "!", 0xE8C9},     // Important
      {"phi-icon-tilde", "~", L'~'},
      {"phi-icon-backtick", "`", L'`'},
  });
  return *icons;
}

std::string PhiIconCatalogGlyph(const std::string& id) {
  for (const PhiCatalogIcon& icon : PhiIconCatalogAll()) {
    if (id == icon.id) {
      const std::wstring wide(1, icon.glyph);
      return base::WideToUTF8(wide);
    }
  }
  return std::string();
}

const PhiIconInkBox& PhiCatalogInkBox(const PhiMacIcon& icon) {
  // Measured once per mask (keyed by its pixel buffer, which the catalog
  // owns for the process lifetime) — a 48x48 scan is nothing next to the
  // paint it saves from re-measuring.
  static base::NoDestructor<std::map<const unsigned char*, PhiIconInkBox>>
      boxes;
  auto it = boxes->find(icon.alpha);
  if (it != boxes->end()) {
    return it->second;
  }
  PhiIconInkBox box = {icon.width, icon.height, -1, -1};
  for (int y = 0; y < icon.height; ++y) {
    for (int x = 0; x < icon.width; ++x) {
      // SAFETY: `alpha` is a width*height array by construction, and x/y
      // walk it in bounds.
      if (UNSAFE_BUFFERS(icon.alpha[y * icon.width + x]) > 32) {
        box.left = std::min(box.left, x);
        box.top = std::min(box.top, y);
        box.right = std::max(box.right, x);
        box.bottom = std::max(box.bottom, y);
      }
    }
  }
  if (box.right < box.left || box.bottom < box.top) {
    box = {0, 0, icon.width - 1, icon.height - 1};  // Blank: fit the canvas.
  }
  return boxes->emplace(icon.alpha, box).first->second;
}

bool PhiIsPrivateGlyph(const std::wstring& text) {
  return !text.empty() && text.front() >= 0xE000 && text.front() <= 0xF8FF;
}

namespace {

// The artwork a stand-in glyph names: the Phi catalog's, or an agent
// brand mark's (phi_agent_icons.h) — the same draw sites serve both.
const PhiMacIcon* MaskForGlyph(const std::wstring& text) {
  if (const PhiMacIcon* mask = PhiCatalogMaskForGlyph(text)) {
    return mask;
  }
  return PhiAgentIconMaskForGlyph(text);
}

// The catalog entry a stand-in glyph names (null for an agent mark, an
// emoji or initials).
const PhiCatalogIcon* CatalogForGlyph(const std::wstring& text) {
  if (text.size() != 1) {
    return nullptr;
  }
  for (const PhiCatalogIcon& icon : PhiIconCatalogAll()) {
    if (icon.glyph == text.front()) {
      return &icon;
    }
  }
  return nullptr;
}

}  // namespace

bool PhiDrawCatalogArtwork(HDC dc,
                           const RECT& rc,
                           const std::wstring& text,
                           int px,
                           COLORREF ink) {
  const PhiMacIcon* mask = MaskForGlyph(text);
  if (!mask || px <= 0) {
    return false;
  }
  const std::vector<unsigned char> bgra = PhiTintedIconBgra(*mask, ink, px);
  const int x = rc.left + ((rc.right - rc.left) - px) / 2;
  const int y = rc.top + ((rc.bottom - rc.top) - px) / 2;
  const RECT dest = {x, y, x + px, y + px};
  return PhiBlendBgra(dc, dest, bgra.data(), px, px);
}

bool PhiDrawCatalogIcon(HDC dc,
                        const RECT& rc,
                        const std::wstring& text,
                        int ink_size,
                        COLORREF ink) {
  const PhiMacIcon* mask = MaskForGlyph(text);
  if (!mask || ink_size <= 0 || text.empty()) {
    return false;
  }
  const PhiIconInkBox& box = PhiCatalogInkBox(*mask);
  const int ink_w = box.right - box.left + 1;
  const int ink_h = box.bottom - box.top + 1;
  const int span = std::max(ink_w, ink_h);
  if (span <= 0) {
    return false;
  }
  int canvas = 0;
  const PhiCatalogIcon* catalog_icon = CatalogForGlyph(text);
  if (catalog_icon && std::string_view(catalog_icon->name).size() == 1) {
    // The catalog's letters, numbers and punctuation are TYPE, not pictures:
    // their imagesets hold a glyph at its own cap height (22 of the 48px
    // mask, the same for every one of them) inside the shared canvas, and
    // fitting THAT ink to an icon's span would set a "7" as tall as a cube.
    // They keep the canvas relation the Mac draws them with — a pictorial
    // icon covers about five eighths of it, and a fifth more again brings
    // the cap to about five sixths of an icon: type BESIDE the icons rather
    // than small print under them. Their stand-in glyph is the character
    // itself for letters and digits; the catalog name also identifies marks.
    canvas = ink_size * 48 / 25;
    // …with a floor. A mark drawn to that shared cap height can still be a
    // speck — the backtick's ink is five mask pixels — and a Space wearing
    // one would read as damage rather than as a character. Nothing in the
    // strip goes below half an icon.
    const double rendered = static_cast<double>(span) * canvas / mask->width;
    const double floor_span = ink_size * 0.55;
    if (rendered < floor_span) {
      canvas = static_cast<int>(
          std::lround(floor_span * mask->width / span));
    }
  } else {
    // The imagesets are 20x20 canvases holding a 16x16 drawing
    // (`translate(2 2) scale(0.8)` in every SVG), and each shape then covers
    // a different part of THAT: the cog fills it, the grid stops short.
    // Fitting the canvas to a slot therefore renders a row of unequal
    // icons. Fit the INK instead and they all read as one size — the Mac
    // reaches the same place from the other end, normalizing its agent
    // imagesets so every glyph's ink spans 42 of 54 and one ratio serves
    // them all (AgentDriverBadge.assetInkRatio).
    canvas = static_cast<int>(std::lround(static_cast<double>(ink_size) *
                                          mask->width / span));
  }
  canvas = std::max(1, canvas);
  const std::vector<unsigned char> bgra =
      PhiTintedIconBgra(*mask, ink, canvas);
  // Placed by the INK's centre, not the canvas's: a glyph is not always
  // centred in its imageset (the "7" sits a pixel to the left of its own),
  // and rounding the canvas into place instead would tilt it further.
  const double scale = static_cast<double>(canvas) / mask->width;
  const int x = (rc.left + rc.right) / 2 -
                static_cast<int>(std::lround((box.left + box.right + 1) / 2.0 *
                                             scale));
  const int y = (rc.top + rc.bottom) / 2 -
                static_cast<int>(std::lround((box.top + box.bottom + 1) / 2.0 *
                                             scale));
  const RECT dest = {x, y, x + canvas, y + canvas};
  return PhiBlendBgra(dc, dest, bgra.data(), canvas, canvas);
}

}  // namespace phi_client

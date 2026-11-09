// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_extension_action.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "client/phi_dwrite.h"

namespace phi_client {

namespace {

// "#AARRGGBB" (the framework's spelling) → an opaque COLORREF. Leaves
// `out` alone for anything else.
void ReadHexColor(const std::string* hex, COLORREF* out) {
  if (!hex || hex->size() != 9 || (*hex)[0] != '#') {
    return;
  }
  uint32_t argb = 0;
  if (!base::HexStringToUInt(std::string_view(*hex).substr(1), &argb)) {
    return;
  }
  *out = RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
}

}  // namespace

PhiExtensionActionState PhiReadExtensionActionState(
    const base::DictValue& row) {
  PhiExtensionActionState state;
  if (const std::string* text = row.FindString("badgeText")) {
    state.badge_text = base::UTF8ToWide(*text);
  }
  ReadHexColor(row.FindString("badgeBackground"), &state.badge_background);
  ReadHexColor(row.FindString("badgeTextColor"), &state.badge_text_color);
  state.visible = row.FindBool("visible").value_or(true);
  state.enabled = row.FindBool("enabled").value_or(true);
  state.grayscale = row.FindBool("grayscale").value_or(false);
  return state;
}

std::vector<unsigned char> PhiGrayedActionIcon(
    base::span<const unsigned char> bgra) {
  std::vector<unsigned char> out(bgra.begin(), bgra.end());
  for (size_t i = 0; i + 3 < out.size(); i += 4) {
    const unsigned b = out[i];
    const unsigned g = out[i + 1];
    const unsigned r = out[i + 2];
    const unsigned a = out[i + 3];
    // Rec. 709 luma; of premultiplied channels it stays premultiplied.
    const unsigned luma = (r * 54 + g * 183 + b * 19) >> 8;
    // 0.8·gray + 0.2·white, the white premultiplied by the pixel's alpha —
    // so the lightening stays inside the icon's own shape, the Mac's
    // sourceAtop fill.
    const unsigned value = std::min(255u, (luma * 4 + a) / 5);
    out[i] = out[i + 1] = out[i + 2] = static_cast<unsigned char>(value);
  }
  return out;
}

void PhiDrawExtensionBadge(HDC dc,
                           const RECT& icon,
                           const PhiExtensionActionState& state,
                           HFONT font,
                           UINT dpi) {
  if (state.badge_text.empty()) {
    return;
  }
  const auto scaled = [dpi](int design) {
    return ::MulDiv(design, static_cast<int>(dpi), 96);
  };
  // At most four characters, as Chrome draws them: an extension that sets a
  // long string would otherwise stretch the pill across the whole tile. A
  // surrogate pair is one character, never split in half.
  std::wstring badge;
  int characters = 0;
  for (size_t i = 0; i < state.badge_text.size() && characters < 4;
       ++characters) {
    const wchar_t unit = state.badge_text[i];
    const size_t length =
        unit >= 0xD800 && unit <= 0xDBFF && i + 1 < state.badge_text.size()
            ? 2
            : 1;
    badge.append(state.badge_text, i, length);
    i += length;
  }
  HGDIOBJ previous_font = font ? ::SelectObject(dc, font) : nullptr;
  RECT measure = {0, 0, 0, 0};
  ::DrawTextW(dc, badge.c_str(), -1, &measure,
              DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
  // The Mac's frame(minWidth: 11, minHeight: 11) around 2pt side padding.
  const int text_w = static_cast<int>(measure.right - measure.left);
  const int text_h = static_cast<int>(measure.bottom - measure.top);
  const int height = std::max(scaled(11), text_h);
  const int width = std::max(scaled(11), text_w + 2 * scaled(2));
  // .offset(x: 4, y: 4) off the icon's bottom-trailing alignment.
  const int right = icon.right + scaled(4);
  const int bottom = icon.bottom + scaled(4);
  const RECT pill = {right - width, bottom - height, right, bottom};
  PhiFillRoundRectAA(dc, pill, height / 2, state.badge_background);
  const int previous_mode = ::SetBkMode(dc, TRANSPARENT);
  const COLORREF previous_color = ::SetTextColor(dc, state.badge_text_color);
  RECT text = pill;
  PhiDrawTextAA(dc, badge.c_str(), -1, &text,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  ::SetTextColor(dc, previous_color);
  ::SetBkMode(dc, previous_mode);
  if (previous_font) {
    ::SelectObject(dc, previous_font);
  }
}

}  // namespace phi_client

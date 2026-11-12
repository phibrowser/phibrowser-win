// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_EXTENSION_ACTION_H_
#define PHIBROWSER_WIN_CLIENT_PHI_EXTENSION_ACTION_H_

#include <windows.h>

#include <string>
#include <vector>

#include "base/containers/span.h"

namespace base {
class DictValue;
}

namespace phi_client {

// One extension action's state on a window's active tab, as a
// ListExtensionsJson row carries it (phi_framework_api.h) — the Mac's
// ExtensionManager.BadgeState, Chrome's ExtensionActionViewModel semantics.
// Default-constructed is the default state: no badge, laid out, clickable,
// in colour. Shared by the address capsule's pinned icons and the
// extensions popover's rows, the two faces the Mac gives an action.
struct PhiExtensionActionState {
  std::wstring badge_text;
  // Resolved by Chromium (IconWithBadgeImageSource): the theme default when
  // the extension left the background transparent, max-contrast text when
  // it left the text colour unset. Opaque here — the Mac draws them so.
  COLORREF badge_background = RGB(0x42, 0x85, 0xF4);
  COLORREF badge_text_color = RGB(0xFF, 0xFF, 0xFF);
  // False only for a page action hidden on this tab: not laid out at all.
  bool visible = true;
  // False: a click falls back to the action's context menu (Chrome's
  // ExecuteUserAction) instead of running it.
  bool enabled = true;
  // The icon renders desaturated: disabled AND no way to interact with
  // the page.
  bool grayscale = false;

  bool operator==(const PhiExtensionActionState&) const = default;
};

// Reads the state keys off a ListExtensionsJson row; a missing or
// malformed key is the default.
PhiExtensionActionState PhiReadExtensionActionState(const base::DictValue& row);

// The grayed variant of a PREMULTIPLIED BGRA icon, for `grayscale`.
// Chrome's IconWithBadgeImageSource HSL shift {-1, 0, 0.6} as the Mac's
// disabledActionVariant reproduces it: full desaturation, then each channel
// blended 20% toward white, alpha untouched.
std::vector<unsigned char> PhiGrayedActionIcon(
    base::span<const unsigned char> bgra);

// The Mac ExtensionBadge pill straddling `icon`'s bottom-right corner
// (device pixels): `state.badge_text` in `font` (8pt semibold), 2pt side
// padding, at least 11×11, a capsule in the badge background, its
// bottom-right 4pt past the icon's. Draws nothing for an empty text.
void PhiDrawExtensionBadge(HDC dc,
                           const RECT& icon,
                           const PhiExtensionActionState& state,
                           HFONT font,
                           UINT dpi);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_EXTENSION_ACTION_H_

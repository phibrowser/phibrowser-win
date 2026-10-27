// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_FONTS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_FONTS_H_

#include <string>

namespace phi_client {

// The brand faces the Mac bundles (Resources/Fonts): IvyPresto Display
// SemiBold Italic (the onboarding titles), IvyPresto Headline SemiBold and
// IvyPresto Headline Light (the membership card's name; GDI family
// "IvyPresto Headline Light"). Shipped beside the exe as resources/fonts and
// registered PRIVATE to this process, so CreateFont finds the families
// without an install. Idempotent; UI thread, before the first window.
void PhiLoadPrivateFonts();

// Family names, for PhiFontDesc::face / LOGFONT. Latin-only faces: a
// caller rendering text that may hold CJK falls back to Segoe UI, as the
// Mac's NSFont.brandDisplay does.
inline constexpr const wchar_t* kPhiBrandDisplayFace = L"IvyPresto Display";
inline constexpr const wchar_t* kPhiBrandHeadlineFace = L"IvyPresto Headline";

// True when every non-space character of `text` is within the brand face's
// Latin coverage (the Mac's `renders:` check, approximated by code point).
bool PhiBrandFaceRenders(const std::wstring& text);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_FONTS_H_

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_TEXT_WRAP_H_
#define PHIBROWSER_WIN_CLIENT_PHI_TEXT_WRAP_H_

#include <windows.h>

#include <string>
#include <string_view>

namespace phi_client {

// DT_WORDBREAK breaks at spaces and, on a Windows whose system code page is
// double-byte, next to any character that page has a two-byte form for. On a
// Chinese Windows that covers é, è, à, ê, ü, ó, á and the curly quotes, so a
// French row wraps as "Dé/sactiver". Neither the font's charset, the thread
// locale nor the process code page changes it.
//
// So the breaks are decided here, from Unicode's line-breaking rules: returns
// `text` with a hard break at the end of each line that fits `width` in the
// font selected into `dc`. DrawText keeps every other job (alignment, clipping,
// the measured rect), and a line it was told fits, it leaves whole. With
// `max_lines` above 0, whatever comes after that many lines is left for
// DrawText to wrap and cut, so DT_END_ELLIPSIS still sees the overflow.
//
// Returns empty when `text` should go to DrawText untouched: text it breaks
// correctly (plain ASCII, East Asian), or anything that fits on one line.
std::wstring PhiWrapTextLines(HDC dc,
                              std::wstring_view text,
                              int width,
                              int max_lines);

// A multi-line EDIT has the same fault, in its own wrapping. This gives `edit`
// the same line-breaking rules through EM_SETWORDBREAKPROC, which also decides
// what Ctrl+arrows and a double-click take for a word.
void PhiUseTextWrapRules(HWND edit);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_TEXT_WRAP_H_

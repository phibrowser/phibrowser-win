// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_UTIL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_UTIL_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace phi_client {

// The client's one copy of the helpers the review found duplicated across the
// tree (docs/architecture-review/lane7-cross-cutting.md §5.5): two
// Widen/Narrow pairs, eight colour blends under three names with three
// argument orders, three Trims, three Lowers, two "now in Unix milliseconds",
// and forty-three hand-rolled clamps.
//
// EVERY helper here keeps the spelling and the ARGUMENT ORDER the majority of
// those copies already used, so converting a call site is a rename and not a
// re-read. Where two copies disagreed the safer one won: base's UTF-8
// conversion over the pair that reimplemented it with an unchecked
// MultiByteToWideChar size.
//
// FILE I/O IS NOT HERE. §5.5's five read loops and four writers were already
// consolidated by phi_file_io.h in the persistence phase, which does more
// than a shared loop would (atomic replace with the write and flush checked,
// corrupt-file quarantine, the JSON load/save pair). Read and write through
// that header; do not add a second one.
//
// The file is in two halves. Everything above the `_WIN32` guard is pure —
// no Win32, no //base — so test/model compiles it on the Linux host; the
// guarded half is UTF-16 conversion, which cannot be. New helpers belong in
// the pure half whenever the answer does not need the OS.

// ---------------------------------------------------------------------------
// Colour
// ---------------------------------------------------------------------------

// COLORREF's underlying type (0x00BBGGRR), spelled without <windows.h> so the
// blend below is host-testable. On Windows this IS COLORREF — windows.h
// declares `typedef unsigned long DWORD; typedef DWORD COLORREF;` — so a
// PhiColor and a COLORREF are the same type and no call site needs a cast.
using PhiColor = unsigned long;

constexpr PhiColor PhiRgb(int r, int g, int b) {
  return static_cast<PhiColor>((r & 0xFF) | ((g & 0xFF) << 8) |
                               ((b & 0xFF) << 16));
}
constexpr int PhiRed(PhiColor c) {
  return static_cast<int>(c & 0xFF);
}
constexpr int PhiGreen(PhiColor c) {
  return static_cast<int>((c >> 8) & 0xFF);
}
constexpr int PhiBlue(PhiColor c) {
  return static_cast<int>((c >> 16) & 0xFF);
}

// `over` composited on `base` at `alpha255`/255 — the Mac's percentage
// overlays, flattened because GDI paints opaque. THE argument order is
// (over, base, alpha): five of the eight copies spelled it that way, and it
// reads the way the sentence does ("white over the ground at 30%").
// phi_downloads_popover's BlendOver(base, over, alpha) is the odd one out and
// its two call sites are converted in this change.
PhiColor PhiBlend(PhiColor over, PhiColor base, int alpha255);

// `from` to `to`, `t` clamped to 0..1 — the strip's colour animation.
PhiColor PhiLerpColor(PhiColor from, PhiColor to, double t);

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------

// `value` held inside [lo, hi]. std::clamp is undefined when hi < lo, which
// is exactly the case the hand-rolled `std::max(a, std::min(b, x))` sites
// survive by accident (an empty rect, a zero-width column); this one yields
// `lo` there instead.
template <typename T>
constexpr T PhiClamp(T value, T lo, T hi) {
  if (hi < lo) {
    return lo;
  }
  return value < lo ? lo : (hi < value ? hi : value);
}
constexpr double PhiClamp01(double value) {
  return PhiClamp(value, 0.0, 1.0);
}
// PhiLerpInt lives in phi_window.h: its rounding is away from `from`, which
// is what makes a one-pixel animation move on the first tick rather than the
// last, and the tab preview depends on that exact curve.

// `design` design units at `dpi`, rounded to nearest, matching ::MulDiv —
// the one scaler for the 266 scattered pixel constants. Pure, so a layout
// that scales can be checked on the host: pass the window's dpi, do not read
// it here.
int PhiDip(int dpi, int design);

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

// Leading and trailing " \t\r\n" removed. Three copies before, all with this
// behaviour and this name.
std::string PhiTrim(std::string_view text);
std::wstring PhiTrim(std::wstring_view text);

// Lowercased. The wide form folds with towlower, which is what two of the
// three copies did (the third used CharLowerBuffW, whose answer differs only
// for characters no locale in the catalog has); the narrow form is ASCII
// only, which is all its caller — a scheme/host compare — needs.
std::string PhiLower(std::string_view text);
std::wstring PhiLower(std::wstring_view text);

// Positional substitution: "{0}", "{1}" … in `pattern` are replaced by the
// matching entry of `args`. A placeholder with no argument is LEFT AS IS
// rather than dropped, so a mistranslated pattern degrades to something
// visible. This is PhiStrF's engine, lifted here so PhiStrN shares it and so
// it can be tested without a catalog.
//
// ONE left-to-right pass over the pattern, so an argument whose own text
// contains a placeholder — a host, a file name, a Space someone called
// "{1}" — is never substituted into a second time. PhiStrF looped once per
// argument and could; no call site reached it, and now none can.
std::wstring PhiSubstitute(std::wstring_view pattern,
                           const std::vector<std::wstring>& args);

// ---------------------------------------------------------------------------
// Plurals and numbers (the pure half of phi_i18n's formatting)
// ---------------------------------------------------------------------------

// CLDR's plural categories, as far as the ten shipping locales need them.
// `few`/`many`/`two`/`zero` will be added with the locales that have them;
// the key shape (`<key>.one`, `<key>.other`) already leaves room, because a
// category name is the suffix.
enum class PhiPlural { kOne, kOther };

// Which category `n` falls in for `locale` (a catalog tag, e.g. "zh-Hans").
//   en, de, es, nl : n == 1
//   fr, pt-BR      : n == 0 or n == 1  (both count zero as singular;
//                    CLDR gives pt-PT the English rule)
//   ja, ko, zh     : never — those languages have one form
// An unknown tag gets the English rule, which is the safest guess for a
// language we have not classified.
PhiPlural PhiPluralFor(std::string_view locale, long long n);

// "one" / "other" — the suffix appended to a plural key.
const char* PhiPluralSuffix(PhiPlural category);

// `value` with `group_sep` inserted every `primary` digits, then every
// `secondary` (the Indic 3;2 grouping; pass secondary == primary for the
// usual case). An empty separator groups nothing. This is what
// PhiFormatNumber falls back to when the OS declines, and the only part of
// number formatting that can be checked on the host.
std::wstring PhiGroupDigits(long long value,
                            std::wstring_view group_sep,
                            int primary,
                            int secondary);

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

// Milliseconds since the Unix epoch. `NowMs` and `NowUnixMs` were the same
// function; the longer name won because the shorter one does not say which
// epoch.
int64_t PhiNowUnixMs();

// Which bucket a "how long ago" reads in. Split out of the downloads
// popover's FormatRelative so the boundaries — the reason "1 days ago" was
// reachable — are pinned by a host test rather than by a screenshot.
enum class PhiRelativeUnit {
  kJustNow,    // < 1 minute
  kMinutes,    // < 1 hour        — counts minutes
  kHours,      // < 12 hours      — counts hours
  kToday,      // < 24 hours
  kYesterday,  // < 48 hours
  kDays,       // < 72 hours      — counts days
  kAbsolute,   // older: the caller formats a date
};
struct PhiRelativeSpan {
  PhiRelativeUnit unit = PhiRelativeUnit::kJustNow;
  // The number the unit counts (minutes, hours or days); 0 for the units
  // that carry no number.
  long long count = 0;
};
// `delta_seconds` is now minus the timestamp; a negative delta (a clock
// skew) reads as "just now" rather than as a future date.
PhiRelativeSpan PhiRelativeSpanFor(long long delta_seconds);

#if defined(_WIN32)
// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-16. Win32 only; the host half above stops here.
// ---------------------------------------------------------------------------

// base's conversions, which is what two of the four copies used. The other
// two reimplemented them with MultiByteToWideChar and did not check the
// returned size, so a malformed byte sequence sized the destination wrong;
// those two (phi_kiosk_window.cc, phi_peek_panel.cc) are the ones to convert
// first.
std::wstring PhiWiden(const std::string& utf8);
std::string PhiNarrow(const std::wstring& wide);
#endif  // defined(_WIN32)

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_UTIL_H_

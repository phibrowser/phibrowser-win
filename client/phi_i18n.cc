// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_i18n.h"

#include <windows.h>

#include <map>
#include <optional>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_util.h"

namespace phi_client {

namespace {

// One locale's strings. Held in a NEVER-ERASED registry so that a pointer
// returned by PhiStr stays valid for the life of the process — a language
// switch points at a different map instead of rewriting this one.
using Catalog = std::map<std::string, std::wstring, std::less<>>;

struct State {
  bool loaded = false;
  std::vector<std::string> locales;         // "en" first.
  std::map<std::string, Catalog> catalogs;  // locale -> strings
  std::string preference;                   // "" = follow the system
  std::string active = "en";
  // Keys asked for that the catalog does not carry. Their wide form is
  // parked here so PhiStr can still hand back a stable pointer.
  std::map<std::string, std::wstring, std::less<>> unknown;
};

State& Get() {
  static base::NoDestructor<State> state;
  return *state;
}

// Reads a whole file with plain Win32 I/O. See the header: the catalog loads
// lazily on the UI thread, where base::ReadFileToString is a blocking call
// the thread disallows once the message loop runs.
bool ReadWholeFile(const std::wstring& path, std::string* out) {
  HANDLE f = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return false;
  }
  char buf[16384];
  DWORD read = 0;
  while (::ReadFile(f, buf, sizeof(buf), &read, nullptr) && read > 0) {
    out->append(buf, read);
  }
  ::CloseHandle(f);
  return true;
}

// The operating system's preferred UI languages, as BCP-47 tags.
std::vector<std::string> SystemUILanguages() {
  std::vector<std::string> out;
  ULONG count = 0;
  ULONG chars = 0;
  if (!::GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr,
                                     &chars) ||
      chars == 0) {
    return out;
  }
  std::vector<wchar_t> buffer(chars);
  if (!::GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(),
                                     &chars)) {
    return out;
  }
  // A double-NUL-terminated list of NUL-separated tags
  // ("zh-Hans-CN\0en-US\0\0"). Split by scanning indices — no pointer walk,
  // so the bounds stay checked all the way to the terminator.
  const base::span<const wchar_t> tags(buffer);
  size_t start = 0;
  for (size_t at = 0; at < tags.size(); ++at) {
    if (tags[at] != L'\0') {
      continue;
    }
    if (at == start) {
      break;  // The empty tag: the list's double NUL.
    }
    out.push_back(base::WideToUTF8(
        std::wstring(tags.subspan(start, at - start).begin(),
                     tags.subspan(start, at - start).end())));
    start = at + 1;
  }
  return out;
}

// Case-insensitive equality for locale tags (Windows reports "zh-hans-cn",
// the catalog carries "zh-Hans").
bool TagEquals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (::tolower(static_cast<unsigned char>(a[i])) !=
        ::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

// Best catalog locale for an OS tag. Exact match wins; then the script-aware
// Chinese rules (a tag naming Hant, or a Hant-only region, is Traditional and
// everything else Chinese is Simplified); then the bare language subtag.
// That last rule is deliberate for Portuguese too: the catalog carries only
// pt-BR, so a Windows "pt-PT" or bare "pt" reads Brazilian Portuguese, which
// is closer to the user than the English fallback. A pt-PT catalog, once one
// ships, wins by exact match and needs no change here.
std::string MatchLocale(const std::string& os_tag,
                        const std::vector<std::string>& available) {
  for (const std::string& tag : available) {
    if (TagEquals(os_tag, tag)) {
      return tag;
    }
  }
  std::string lower;
  for (char c : os_tag) {
    lower.push_back(static_cast<char>(::tolower(static_cast<unsigned char>(c))));
  }
  const std::string language = lower.substr(0, lower.find('-'));
  if (language == "zh") {
    const bool traditional = lower.find("hant") != std::string::npos ||
                             lower.find("-tw") != std::string::npos ||
                             lower.find("-hk") != std::string::npos ||
                             lower.find("-mo") != std::string::npos;
    const std::string want = traditional ? "zh-Hant" : "zh-Hans";
    for (const std::string& tag : available) {
      if (TagEquals(want, tag)) {
        return tag;
      }
    }
    return std::string();
  }
  for (const std::string& tag : available) {
    if (TagEquals(language, tag.substr(0, tag.find('-')))) {
      return tag;
    }
  }
  return std::string();
}

// Resolves `preference` (or the system list when it is empty) to a locale the
// catalog carries. Always yields something: "en" is the floor.
std::string ResolveActive(const State& state) {
  if (!state.preference.empty()) {
    const std::string matched = MatchLocale(state.preference, state.locales);
    if (!matched.empty()) {
      return matched;
    }
  } else {
    for (const std::string& os_tag : SystemUILanguages()) {
      const std::string matched = MatchLocale(os_tag, state.locales);
      if (!matched.empty()) {
        return matched;
      }
    }
  }
  return "en";
}

void EnsureLoaded() {
  State& state = Get();
  if (state.loaded) {
    return;
  }
  state.loaded = true;  // One attempt: a missing catalog must not re-read.
  state.locales.push_back("en");

  base::FilePath dir;
  if (!base::PathService::Get(base::DIR_EXE, &dir)) {
    return;
  }
  const base::FilePath path =
      dir.AppendASCII("resources").AppendASCII("strings.json");
  std::string json;
  if (!ReadWholeFile(path.value(), &json)) {
    LOG(ERROR) << "[PhiClient] string catalog missing: " << path
               << " — the UI stays English";
    return;
  }
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root) {
    LOG(ERROR) << "[PhiClient] string catalog is not valid JSON";
    return;
  }
  if (const base::ListValue* locales = root->FindList("locales")) {
    state.locales.clear();
    for (const base::Value& v : *locales) {
      if (v.is_string()) {
        state.locales.push_back(v.GetString());
      }
    }
  }
  if (state.locales.empty()) {
    state.locales.push_back("en");
  }
  const base::DictValue* strings = root->FindDict("strings");
  if (strings) {
    for (const auto [key, value] : *strings) {
      const base::DictValue* per_locale = value.GetIfDict();
      if (!per_locale) {
        continue;
      }
      for (const auto [locale, text] : *per_locale) {
        if (text.is_string()) {
          state.catalogs[std::string(locale)][std::string(key)] =
              base::UTF8ToWide(text.GetString());
        }
      }
    }
  }
  state.active = ResolveActive(state);
  LOG(INFO) << "[PhiClient] strings: " << (strings ? strings->size() : 0u)
            << " keys, " << state.locales.size() << " locales, active "
            << state.active;
}

// --- Number and date formatting --------------------------------------------
// GetNumberFormatEx / GetDateFormatEx are the answer, not a hand-rolled
// table: they carry the grouping (Indic 3;2 as well as 3;3), the separators,
// the negative-number order and the locale's own digits. What this file
// supplies is the NUMBERFMTW they need, read from the same locale.

// One LOCALE_* string, or `fallback` when the OS has none.
std::wstring LocaleInfo(const std::wstring& locale,
                        LCTYPE type,
                        const wchar_t* fallback) {
  std::wstring out(32, L'\0');
  const int written = ::GetLocaleInfoEx(locale.c_str(), type, out.data(),
                                        static_cast<int>(out.size()));
  if (written <= 1) {
    return fallback;
  }
  out.resize(static_cast<size_t>(written) - 1);
  return out;
}

// LOCALE_SGROUPING is "3;0" (group every three) or "3;2;0" (the Indic
// grouping); NUMBERFMTW wants 3 and 32. The trailing 0 is a terminator.
UINT GroupingFromLocale(const std::wstring& grouping) {
  UINT value = 0;
  for (wchar_t c : grouping) {
    if (c >= L'1' && c <= L'9') {
      value = value * 10 + static_cast<UINT>(c - L'0');
    }
  }
  return value == 0 ? 3u : value;
}

// A C-locale decimal string ("-1234.50") — what GetNumberFormatEx takes as
// input. Built by integer arithmetic rather than swprintf so there is no
// buffer to size wrongly.
std::wstring RawDecimal(double value, int decimals) {
  const int places = PhiClamp(decimals, 0, 9);
  long long scale = 1;
  for (int i = 0; i < places; ++i) {
    scale *= 10;
  }
  const double scaled = value * static_cast<double>(scale);
  const long long rounded = static_cast<long long>(
      scaled < 0 ? scaled - 0.5 : scaled + 0.5);
  const bool negative = rounded < 0;
  const unsigned long long magnitude =
      negative ? 0ULL - static_cast<unsigned long long>(rounded)
               : static_cast<unsigned long long>(rounded);
  const auto divisor = static_cast<unsigned long long>(scale);
  std::wstring out = std::to_wstring(magnitude / divisor);
  if (places > 0) {
    std::wstring fraction = std::to_wstring(magnitude % divisor);
    fraction.insert(0, static_cast<size_t>(places) - fraction.size(), L'0');
    out += L"." + fraction;
  }
  return negative ? L"-" + out : out;
}

std::wstring FormatDecimalString(const std::wstring& raw, int decimals) {
  EnsureLoaded();
  const std::wstring locale = base::UTF8ToWide(Get().active);
  // Non-const: NUMBERFMTW takes LPWSTR, not a pointer to const.
  std::wstring decimal_sep = LocaleInfo(locale, LOCALE_SDECIMAL, L".");
  std::wstring group_sep = LocaleInfo(locale, LOCALE_STHOUSAND, L",");

  NUMBERFMTW format = {};
  format.NumDigits = static_cast<UINT>(PhiClamp(decimals, 0, 9));
  format.LeadingZero =
      LocaleInfo(locale, LOCALE_ILZERO, L"1") == L"0" ? 0u : 1u;
  format.Grouping = GroupingFromLocale(
      LocaleInfo(locale, LOCALE_SGROUPING, L"3;0"));
  format.lpDecimalSep = decimal_sep.data();
  format.lpThousandSep = group_sep.data();
  const std::wstring negative_order =
      LocaleInfo(locale, LOCALE_INEGNUMBER, L"1");
  format.NegativeOrder =
      negative_order.empty() || negative_order[0] < L'0' ||
              negative_order[0] > L'9'
          ? 1u
          : static_cast<UINT>(negative_order[0] - L'0');

  std::wstring out(64, L'\0');
  const int written =
      ::GetNumberFormatEx(locale.c_str(), 0, raw.c_str(), &format, out.data(),
                          static_cast<int>(out.size()));
  if (written > 1) {
    out.resize(static_cast<size_t>(written) - 1);
    return out;
  }
  // The OS declined. Group by hand with the separators it did give us — the
  // one path phi_util's PhiGroupDigits exists for, and the one the host test
  // pins.
  const size_t dot = raw.find(L'.');
  const std::wstring integer_part =
      dot == std::wstring::npos ? raw : raw.substr(0, dot);
  const bool negative = !integer_part.empty() && integer_part[0] == L'-';
  const std::wstring digits =
      negative ? integer_part.substr(1) : integer_part;
  long long magnitude = 0;
  for (wchar_t c : digits) {
    if (c >= L'0' && c <= L'9') {
      magnitude = magnitude * 10 + (c - L'0');
    }
  }
  std::wstring grouped = PhiGroupDigits(magnitude, group_sep, 3, 3);
  if (negative) {
    grouped.insert(0, 1, L'-');
  }
  if (dot != std::wstring::npos && decimals > 0) {
    grouped += decimal_sep + raw.substr(dot + 1);
  }
  return grouped;
}

// `unix_ms` in the user's own time zone.
bool LocalTimeFor(int64_t unix_ms, SYSTEMTIME* out) {
  const int64_t ticks = (unix_ms + 11644473600000LL) * 10000LL;
  if (ticks < 0) {
    return false;
  }
  FILETIME utc = {};
  utc.dwLowDateTime = static_cast<DWORD>(static_cast<uint64_t>(ticks) &
                                         0xFFFFFFFFULL);
  utc.dwHighDateTime = static_cast<DWORD>(static_cast<uint64_t>(ticks) >> 32);
  SYSTEMTIME utc_parts = {};
  if (!::FileTimeToSystemTime(&utc, &utc_parts)) {
    return false;
  }
  return ::SystemTimeToTzSpecificLocalTime(nullptr, &utc_parts, out) != 0;
}

// The last-resort date: unambiguous in every locale, wrong in all of them.
std::wstring IsoDate(const SYSTEMTIME& t, PhiDateStyle style) {
  const auto pad = [](int value, size_t width) {
    std::wstring text = std::to_wstring(value);
    return text.size() >= width
               ? text
               : std::wstring(width - text.size(), L'0') + text;
  };
  if (style == PhiDateStyle::kShortTime) {
    return pad(t.wHour, 2) + L":" + pad(t.wMinute, 2);
  }
  return pad(t.wYear, 4) + L"-" + pad(t.wMonth, 2) + L"-" + pad(t.wDay, 2);
}

}  // namespace

const std::string& PhiActiveLocale() {
  EnsureLoaded();
  return Get().active;
}

const std::vector<std::string>& PhiAvailableLocales() {
  EnsureLoaded();
  return Get().locales;
}

const wchar_t* PhiLocaleDisplayName(const std::string& tag) {
  // Endonyms: a language is named in itself, the way every browser's language
  // picker does it — someone looking for their own language should not have
  // to read the current one to find it.
  struct Entry {
    const char* tag;
    const wchar_t* name;
  };
  static constexpr Entry kNames[] = {
      {"en", L"English"},
      {"de", L"Deutsch"},
      {"es", L"Español"},
      {"fr", L"Français"},
      {"ja", L"日本語"},
      {"ko", L"한국어"},
      {"nl", L"Nederlands"},
      {"pt-BR", L"Português (Brasil)"},
      {"zh-Hans", L"简体中文"},
      {"zh-Hant", L"繁體中文"},
  };
  for (const Entry& entry : kNames) {
    if (tag == entry.tag) {
      return entry.name;
    }
  }
  static base::NoDestructor<std::map<std::string, std::wstring>> fallback;
  auto it = fallback->find(tag);
  if (it == fallback->end()) {
    it = fallback->emplace(tag, base::UTF8ToWide(tag)).first;
  }
  return it->second.c_str();
}

bool PhiSetLanguage(const std::string& tag) {
  EnsureLoaded();
  State& state = Get();
  state.preference = tag;
  const std::string next = ResolveActive(state);
  if (next == state.active) {
    return false;
  }
  state.active = next;
  LOG(INFO) << "[PhiClient] language switched to " << next
            << (tag.empty() ? " (following the system)" : "");
  return true;
}

const std::string& PhiLanguagePreference() {
  EnsureLoaded();
  return Get().preference;
}

const wchar_t* PhiStr(const char* key) {
  EnsureLoaded();
  State& state = Get();
  const std::string_view wanted(key ? key : "");
  // Active locale, then English, then the key itself.
  auto locale = state.catalogs.find(state.active);
  if (locale != state.catalogs.end()) {
    auto found = locale->second.find(wanted);
    if (found != locale->second.end()) {
      return found->second.c_str();
    }
  }
  if (state.active != "en") {
    auto english = state.catalogs.find("en");
    if (english != state.catalogs.end()) {
      auto found = english->second.find(wanted);
      if (found != english->second.end()) {
        return found->second.c_str();
      }
    }
  }
  auto unknown = state.unknown.find(wanted);
  if (unknown == state.unknown.end()) {
    LOG(WARNING) << "[PhiClient] no string for key: " << wanted;
    unknown = state.unknown
                  .emplace(std::string(wanted), base::UTF8ToWide(wanted))
                  .first;
  }
  return unknown->second.c_str();
}

std::wstring PhiStrF(const char* key, const std::vector<std::wstring>& args) {
  return PhiSubstitute(PhiStr(key), args);
}

std::wstring PhiStrN(const char* key,
                     long long count,
                     const std::vector<std::wstring>& args) {
  EnsureLoaded();
  const std::string family(key ? key : "");
  const std::string plural =
      family + "." + PhiPluralSuffix(PhiPluralFor(Get().active, count));
  // The count is {0}, formatted for the locale; the caller's args follow it.
  std::vector<std::wstring> all;
  all.reserve(args.size() + 1);
  all.push_back(PhiFormatNumber(count));
  all.insert(all.end(), args.begin(), args.end());
  return PhiSubstitute(PhiStr(plural.c_str()), all);
}

std::wstring PhiFormatNumber(long long value) {
  return FormatDecimalString(std::to_wstring(value), 0);
}

std::wstring PhiFormatNumber(double value, int decimals) {
  return FormatDecimalString(RawDecimal(value, decimals),
                             decimals < 0 ? 0 : decimals);
}

std::wstring PhiFormatDate(int64_t unix_ms, PhiDateStyle style) {
  EnsureLoaded();
  SYSTEMTIME local = {};
  if (!LocalTimeFor(unix_ms, &local)) {
    return std::wstring();
  }
  const std::wstring locale = base::UTF8ToWide(Get().active);
  std::wstring out(96, L'\0');
  int written = 0;
  if (style == PhiDateStyle::kShortTime) {
    written = ::GetTimeFormatEx(locale.c_str(), TIME_NOSECONDS, &local,
                                nullptr, out.data(),
                                static_cast<int>(out.size()));
  } else {
    const DWORD flags = style == PhiDateStyle::kLongDate ? DATE_LONGDATE
                                                         : DATE_SHORTDATE;
    written = ::GetDateFormatEx(locale.c_str(), flags, &local, nullptr,
                                out.data(), static_cast<int>(out.size()),
                                nullptr);
  }
  if (written <= 1) {
    // The OS declined the locale. ISO-8601 is wrong for every user and
    // ambiguous for none, which is the right way to be wrong.
    LOG(WARNING) << "[PhiClient] no date format for locale " << Get().active;
    return IsoDate(local, style);
  }
  out.resize(static_cast<size_t>(written) - 1);
  return out;
}

}  // namespace phi_client

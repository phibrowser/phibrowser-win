// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_downloads_model.h"

#include <commctrl.h>
#include <commoncontrols.h>  // IImageList, for SHGetImageList.
#include <shellapi.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>

#include "base/compiler_specific.h"
#include "base/json/json_reader.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_util.h"

namespace phi_client {

std::vector<PhiDownloadRow> PhiParseDownloadsJson(const std::string& json) {
  std::vector<PhiDownloadRow> rows;
  std::optional<base::Value> parsed =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    return rows;
  }
  for (const base::Value& value : parsed->GetList()) {
    const base::DictValue* dict = value.GetIfDict();
    if (!dict) {
      continue;
    }
    PhiDownloadRow row;
    row.id = dict->FindInt("id").value_or(0);
    // A row can arrive before the framework has a name for it (an in-flight
    // download whose target is still being chosen); the row shape is the
    // framework's, not ours, so never assume the key.
    if (const std::string* name = dict->FindString("name")) {
      row.name = base::UTF8ToWide(*name);
    }
    if (const std::string* host = dict->FindString("host")) {
      row.host = base::UTF8ToWide(*host);
    }
    if (const std::string* path = dict->FindString("path")) {
      row.path = base::UTF8ToWide(*path);
    }
    if (const std::string* url = dict->FindString("url")) {
      row.url = base::UTF8ToWide(*url);
    }
    if (const std::string* state = dict->FindString("state")) {
      row.state = *state;
    }
    row.paused = dict->FindBool("paused").value_or(false);
    row.exists = dict->FindBool("exists").value_or(true);
    row.received = dict->FindDouble("received").value_or(0);
    row.total = dict->FindDouble("total").value_or(0);
    row.speed = dict->FindDouble("speed").value_or(0);
    row.end_time = dict->FindDouble("endTime").value_or(0);
    if (const std::string* danger = dict->FindString("danger")) {
      row.danger = *danger;
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<PhiDownloadAction> PhiDownloadActionsFor(
    const PhiDownloadRow& row) {
  // Safe Browsing states first (the Mac popover's keep/discard): a warning
  // offers Keep + Discard, a block offers Discard only. "remove" IS the
  // discard — Chromium's bubble does the same (cancel + delete the
  // temporary file). Scanning rows keep the normal actions. Glyphs are
  // escapes: private-use characters do not survive every editor round-trip.
  if (row.danger == "warning") {
    return {{"keep", L'',
             "downloads.item.safetyWarning.keepButtonTooltip"},
            {"remove", L'',
             "downloads.item.safetyWarning.discardButtonTooltip"}};
  }
  if (row.danger == "blocked" || row.danger == "policyBlocked") {
    return {{"remove", L'',
             "downloads.item.blocked.discardButtonTooltip"}};
  }
  if (row.state == "in_progress") {
    return {{"copy", L'',
             "downloads.item.inProgress.copyLinkButtonTooltip"},
            {row.paused ? "resume" : "pause",
             row.paused ? L'' : L'',
             row.paused ? "downloads.item.resumeButtonTooltip"
                        : "downloads.item.pauseButtonTooltip"},
            {"cancel", L'', "downloads.item.cancelButtonTooltip"}};
  }
  if (row.state == "complete") {
    return {{"copy", L'',
             "downloads.item.completed.copyLinkButtonTooltip"},
            {"show", L'', "downloads.item.showInFolderButtonTooltip",
             row.exists},
            {"remove", L'',
             "downloads.item.completed.removeButtonTooltip"}};
  }
  if (row.state == "interrupted") {
    // Retry (DownloadItemRow's, for a failed download): the framework's
    // "resume" restarts an interrupted item, from where it stopped when
    // the server allows it. MDL2 Refresh.
    return {{"copy", L'',
             "downloads.item.interrupted.copyLinkButtonTooltip"},
            {"resume", L'', "downloads.item.retryButtonTooltip"},
            {"remove", L'',
             "downloads.item.interrupted.removeButtonTooltip"}};
  }
  return {{"copy", L'',
           "downloads.item.interrupted.copyLinkButtonTooltip"},
          {"remove", L'',
           "downloads.item.interrupted.removeButtonTooltip"}};
}

bool PhiDownloadCanOpen(const PhiDownloadRow& row) {
  return row.state == "complete" && row.exists && row.danger == "normal";
}

bool PhiDownloadFailed(const PhiDownloadRow& row) {
  return row.state == "cancelled" || row.state == "interrupted" ||
         (row.state == "complete" && !row.exists);
}

const char* PhiDownloadStatusKey(const PhiDownloadRow& row) {
  if (row.danger == "warning") {
    return "downloads.dangerous";
  }
  if (row.danger == "blocked") {
    return "download.safety.short.blocked";
  }
  if (row.danger == "policyBlocked") {
    return "downloads.blockedByPolicy";
  }
  if (row.danger == "scanning") {
    return "download.safety.short.scanning";
  }
  if (row.state == "cancelled") {
    return "downloads.item.status.cancelled";
  }
  if (row.state == "interrupted") {
    return "downloads.item.status.failed";
  }
  if (row.state == "complete" && !row.exists) {
    return "downloads.item.status.deleted";
  }
  return nullptr;
}

std::wstring PhiFormatDownloadBytes(double bytes) {
  // The unit names are catalog keys rather than literals. The FIGURE is
  // still built with std::to_wstring, which is C-locale: a German user reads
  // "9.3 MB" where the system writes "9,3 MB". The fix is
  // PhiFormatNumber(bytes, 1) — deliberately NOT made here, because
  // GetNumberFormatEx also groups, so "1023 B" would become "1,023 B" and
  // this change is required to render identically in English. It is on the
  // list for the pass that converts the rest of the downloads surfaces.
  static constexpr std::array<const char*, 4> kUnitKeys = {
      "downloads.b", "downloads.kb", "downloads.mb", "downloads.gb"};
  size_t unit = 0;
  while (bytes >= 1024.0 && unit + 1 < kUnitKeys.size()) {
    bytes /= 1024.0;
    ++unit;
  }
  const double rounded = unit == 0 ? bytes : std::round(bytes * 10.0) / 10.0;
  std::wstring text = std::to_wstring(rounded);
  const size_t dot = text.find(L'.');
  if (dot != std::wstring::npos) {
    text.resize(unit == 0 ? dot : dot + 2);
  }
  return text + L" " + PhiStr(kUnitKeys[unit]);
}

std::wstring PhiFormatDownloadRelative(double unix_seconds) {
  // The ladder itself is PhiRelativeSpanFor (phi_util.h) so its boundaries
  // are pinned by a host test; this function only names the strings.
  const auto now = static_cast<double>(::time(nullptr));
  const auto delta = static_cast<long long>(now - unix_seconds);
  const PhiRelativeSpan span = PhiRelativeSpanFor(delta);
  switch (span.unit) {
    case PhiRelativeUnit::kJustNow:
      return PhiStr("downloads.item.completedTime.justNow");
    case PhiRelativeUnit::kMinutes:
      return PhiStrN("downloads.item.completedTime.minutesAgo", span.count);
    case PhiRelativeUnit::kHours:
      return PhiStrN("downloads.item.completedTime.hoursAgo", span.count);
    case PhiRelativeUnit::kToday:
      return PhiStr("downloads.item.completedTime.today");
    case PhiRelativeUnit::kYesterday:
      return PhiStr("downloads.item.completedTime.yesterday");
    case PhiRelativeUnit::kDays:
      return PhiStrN("downloads.item.completedTime.daysAgo", span.count);
    case PhiRelativeUnit::kAbsolute:
      break;
  }
  // The locale's own short date: correct for every locale and narrower
  // than an English month name.
  return PhiFormatDate(static_cast<int64_t>(unix_seconds) * 1000,
                       PhiDateStyle::kShortDate);
}

HICON PhiShellIconForPath(const std::wstring& path, int px) {
  SHFILEINFOW info = {};
  if (!::SHGetFileInfoW(path.c_str(), FILE_ATTRIBUTE_NORMAL, &info,
                        sizeof(info),
                        SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES)) {
    return nullptr;
  }
  IImageList* list = nullptr;
  if (FAILED(::SHGetImageList(px <= 32 ? SHIL_LARGE : SHIL_EXTRALARGE,
                              IID_PPV_ARGS(&list))) ||
      !list) {
    return nullptr;
  }
  HICON icon = nullptr;
  if (FAILED(list->GetIcon(info.iIcon, ILD_TRANSPARENT, &icon))) {
    icon = nullptr;
  }
  list->Release();
  return icon;
}

PhiDownloadIconCache::PhiDownloadIconCache() = default;

PhiDownloadIconCache::~PhiDownloadIconCache() {
  Clear();
}

HICON PhiDownloadIconCache::IconFor(const std::wstring& path, int px) {
  const size_t dot = path.find_last_of(L'.');
  const std::wstring ext = dot == std::wstring::npos ? L"" : path.substr(dot);
  auto found = icons_.find(ext);
  if (found != icons_.end()) {
    return found->second;
  }
  HICON icon = PhiShellIconForPath(L"file" + ext, px);
  if (!icon) {
    SHFILEINFOW info = {};
    ::SHGetFileInfoW((L"file" + ext).c_str(), FILE_ATTRIBUTE_NORMAL, &info,
                     sizeof(info),
                     SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES);
    icon = info.hIcon;
  }
  icons_[ext] = icon;
  return icon;
}

void PhiDownloadIconCache::Draw(HDC dc,
                                const RECT& rect,
                                const std::wstring& path,
                                int px) {
  HICON handle = IconFor(path, px);
  if (!handle) {
    return;
  }
  int draw = px;
  ICONINFO info = {};
  if (::GetIconInfo(handle, &info)) {
    BITMAP bitmap = {};
    if (info.hbmColor &&
        ::GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) &&
        std::abs(static_cast<int>(bitmap.bmWidth) - px) <= 3) {
      draw = static_cast<int>(bitmap.bmWidth);
    }
    if (info.hbmColor) {
      ::DeleteObject(info.hbmColor);
    }
    if (info.hbmMask) {
      ::DeleteObject(info.hbmMask);
    }
  }
  const int x = (rect.left + rect.right - draw) / 2;
  const int y = (rect.top + rect.bottom - draw) / 2;
  ::DrawIconEx(dc, x, y, handle, draw, draw, 0, nullptr, DI_NORMAL);
}

void PhiDownloadIconCache::Clear() {
  for (auto& [ext, icon] : icons_) {
    if (icon) {
      ::DestroyIcon(icon);
    }
  }
  icons_.clear();
}

void PhiCopyTextToClipboard(HWND owner, const std::wstring& text) {
  if (!::OpenClipboard(owner)) {
    return;
  }
  ::EmptyClipboard();
  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  if (HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    if (void* target = ::GlobalLock(handle)) {
      // SAFETY: the allocation above is exactly `bytes` long.
      UNSAFE_BUFFERS(::memcpy(target, text.c_str(), bytes));
      ::GlobalUnlock(handle);
      ::SetClipboardData(CF_UNICODETEXT, handle);
    } else {
      ::GlobalFree(handle);
    }
  }
  ::CloseClipboard();
}

}  // namespace phi_client

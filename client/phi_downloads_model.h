// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_DOWNLOADS_MODEL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_DOWNLOADS_MODEL_H_

#include <windows.h>

#include <map>
#include <string>
#include <vector>

namespace phi_client {

// The downloads model the popover (phi_downloads_popover.cc) and the
// Library's All Downloads list (phi_library.cc) share: one row shape over
// the framework's ListDownloadsJson, one parse, one per-state action set,
// one icon cache, one relative-time formatter. Before the Library both
// halves lived file-local in the popover.

// One row of the framework's ListDownloadsJson.
struct PhiDownloadRow {
  int id = 0;
  std::wstring name;
  std::wstring host;
  std::wstring path;
  std::wstring url;
  std::string state;  // "in_progress|complete|cancelled|interrupted".
  bool paused = false;
  bool exists = true;
  double received = 0;
  double total = 0;
  double speed = 0;
  // Unix seconds; 0 until the download ends. The framework's list carries
  // no start time, so this is also what the Library groups by.
  double end_time = 0;
  // "normal|scanning|warning|blocked|policyBlocked" — the framework's
  // DownloadSafetyState (the Mac safety-state mapping).
  std::string danger = "normal";
};

// A row action button.
struct PhiDownloadAction {
  const char* verb;  // Framework DownloadAction verb, or "copy".
  wchar_t glyph;     // MDL2.
  // The catalog key of its tooltip / accessible name (DownloadItemRow's
  // per-state tooltip keys): a raw verb is no name for a user.
  const char* name_key;
  bool enabled = true;
};

// Live refresh cadence while a downloads surface is on screen.
inline constexpr UINT kPhiDownloadsPollMs = 500;

// Parses the framework's JSON array into rows, in the framework's order
// (newest first). A row can arrive before the framework has a name for it,
// so every key is optional.
std::vector<PhiDownloadRow> PhiParseDownloadsJson(const std::string& json);

// The per-state action set (DownloadItemRow's normalActionButtons, the
// Safe Browsing keep/discard pair first).
std::vector<PhiDownloadAction> PhiDownloadActionsFor(const PhiDownloadRow& row);

// A completed download the user can open from the list: the file is there
// and nothing about it is in question (DownloadItemRow.openCompletedFile).
bool PhiDownloadCanOpen(const PhiDownloadRow& row);

// A failed, cancelled or deleted row (struck through, with a status tag).
bool PhiDownloadFailed(const PhiDownloadRow& row);
// The status tag's catalog key ("· Canceled"), or null for a plain row.
const char* PhiDownloadStatusKey(const PhiDownloadRow& row);

// "9.3 MB" — the popover's byte formatter (C-locale figure; see the
// definition for why).
std::wstring PhiFormatDownloadBytes(double bytes);
// DownloadItemRow's friendly completion time ("Just now", "3 min ago",
// "Yesterday", the locale's short date).
std::wstring PhiFormatDownloadRelative(double unix_seconds);

// The icon the shell shows for `path`'s type, from the system image list
// nearest `px` (32, 48), not a 32 px icon stretched. Caller destroys it.
HICON PhiShellIconForPath(const std::wstring& path, int px);

// Per-extension cache of the shell icons at one size; owns the handles.
class PhiDownloadIconCache {
 public:
  PhiDownloadIconCache();
  ~PhiDownloadIconCache();
  PhiDownloadIconCache(const PhiDownloadIconCache&) = delete;
  PhiDownloadIconCache& operator=(const PhiDownloadIconCache&) = delete;

  // The icon for `path`'s extension at `px`; null when the shell has none.
  HICON IconFor(const std::wstring& path, int px);
  // Draws it centred in `rect` at the image list's own size when that is
  // within a pixel or two of `px` (a GDI stretch of an icon is
  // nearest-neighbour, and blurs or jags it).
  void Draw(HDC dc, const RECT& rect, const std::wstring& path, int px);
  // Drops every icon (a dpi change picked them for the old size).
  void Clear();

 private:
  std::map<std::wstring, HICON> icons_;
};

// Puts `text` on the clipboard as CF_UNICODETEXT, owned by `owner`.
void PhiCopyTextToClipboard(HWND owner, const std::wstring& text);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_DOWNLOADS_MODEL_H_

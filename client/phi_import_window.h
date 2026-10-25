// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_IMPORT_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_IMPORT_WINDOW_H_

#include <windows.h>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_browser_import.h"
#include "phibrowser-win/client/phi_secondary_window.h"

namespace phi_client {

// Where an import lands: the Space (and its profile) the window was opened
// from, and the browser window whose Chromium profile receives the data.
struct PhiImportTarget {
  std::string profile_id;
  std::string space_id;
  int64_t window_id = 0;
  std::wstring label;  // "Space (Profile)", the caption under the title.
};

// One run, as the window hands it to the coordinator (the Mac's
// startImportData arguments).
struct PhiImportRequest {
  PhiImportTarget target;
  std::vector<PhiImportSource> sources;  // Configured rows, row order.
  std::map<PhiImportSource, std::vector<PhiImportDataType>> types;
  // The Chromium profile directory per Chrome/Edge/Brave row.
  std::map<PhiImportSource, std::string> profiles;
  std::optional<PhiArcSpace> arc_space;
  std::wstring file_path;
};

// "Import from Another Browser…" — the Mac's ImportFromOtherBrowserViewController
// in its standalone (.normal) mode: one window per app, the installed
// sources as rows, each expanding to its data-type toggles (single-open
// accordion), a profile — or for Arc a Space — picker on rows with more
// than one, and the file row's picker. Import hands the configured rows to
// the coordinator, which runs them one by one; the window shows the
// progress and closes when every source succeeded. Unlike the Mac it stays
// open on a failure, naming the sources that failed, rather than closing
// over a status nobody sees.
class PhiImportWindow : public PhiSecondaryWindow {
 public:
  class Host {
   public:
    virtual ~Host() = default;
    // Starts a run; false when one is already going.
    virtual bool StartBrowserImport(const PhiImportRequest& request) = 0;
    virtual bool BrowserImportRunning() const = 0;
  };

  // Opens (or fronts) the window for `target`. A window already open is
  // retargeted unless its import is still running.
  static void Open(Host* host, const PhiImportTarget& target, HWND anchor);
  static PhiImportWindow* Current();

  // The coordinator's progress: the status line while a run goes, then its
  // end. `failed` names the sources that failed; empty closes the window.
  void SetStatus(const std::wstring& status);
  void Finish(const std::vector<PhiImportSource>& failed);

  // The harness (agentSpace.import.*): the rows and their state, and a
  // control pressed by name — "expand" (value: source), "toggle" (value
  // "<source>:<bookmarks|history|cookies|extensions>"), "profile" (value
  // "<source>:<index>"), "file" (value: a path, standing in for the
  // picker), "import", "cancel". Empty on success, else an error token.
  std::string StateJson() const;
  std::string Drive(const std::string& action, const std::string& value);

 protected:
  void Paint(HDC dc, const RECT& dirty) override;
  void OnHover(int hit, int previous) override;
  void OnClick(int hit, POINT client_pt) override;
  void OnEscape() override;
  void OnDpiChanged(UINT old_dpi) override;
  void OnNcDestroy() override;
  void OnColorsChanged() override;
  LRESULT OnWindowMessage(UINT message,
                          WPARAM wparam,
                          LPARAM lparam,
                          bool* handled) override;

 private:
  enum class Phase { kSelecting, kImporting, kFailed };
  enum class Control {
    kNone,
    kHeader,   // key = row
    kPicker,   // key = row
    kToggle,   // key = row * 10 + type
    kChooseFile,
    kImport,
    kCancel,
    kDone,
  };
  struct Row {
    PhiImportSource source = PhiImportSource::kChrome;
    std::wstring title;  // "From Chrome"
    HICON icon = nullptr;
    // Chrome / Edge / Brave: the Local State profiles; Arc: its Spaces.
    std::vector<PhiChromiumProfile> profiles;
    std::vector<PhiArcSpace> spaces;
    int selected = 0;
    bool enabled = true;
    std::set<PhiImportDataType> types;
    std::vector<PhiImportDataType> offered;  // The toggles the row shows.
    // Layout.
    RECT card{};
    RECT header{};
    RECT picker{};
    std::vector<std::pair<PhiImportDataType, RECT>> toggles;
    RECT hint{};
    RECT file_name{};
    RECT choose{};
  };
  struct Button {
    Control control = Control::kNone;
    std::wstring text;
    int role = 0;  // PhiPopupButtonRole
    bool enabled = true;
    RECT rect{};
  };

  PhiImportWindow(Host* host, const PhiImportTarget& target);
  ~PhiImportWindow() override;
  bool OpenWindow(HWND anchor);
  void LoadRows();
  void Retarget(const PhiImportTarget& target);

  bool Configured(const Row& row) const;
  bool AnyConfigured() const;
  std::wstring PickerText(const Row& row) const;
  void ToggleExpansion(int row);
  void ToggleType(int row, PhiImportDataType type);
  void Pick(int row);
  void SelectPick(int row, int index);
  void ChooseFile();
  void SetFile(const std::wstring& path);
  void StartImport();

  HFONT TitleFont();
  HFONT BodyFont();
  HFONT SmallFont();
  HFONT GlyphFont(int size);
  int MeasureHeight(HFONT font, const std::wstring& text, int width);
  void Layout();
  void AddButtons(std::vector<Button> buttons, int bottom);
  int RowOf(PhiImportSource source) const;

  RAW_PTR_EXCLUSION Host* const host_;
  PhiImportTarget target_;
  Phase phase_ = Phase::kSelecting;
  std::vector<Row> rows_;
  int expanded_ = -1;
  std::wstring file_path_;
  std::wstring status_;
  std::vector<Button> buttons_;
  RECT title_rect_{};
  RECT target_rect_{};
  RECT status_rect_{};
  HBRUSH bg_brush_ = nullptr;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_IMPORT_WINDOW_H_

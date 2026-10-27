// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_URL_RULES_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_URL_RULES_WINDOW_H_

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_secondary_window.h"
#include "phibrowser-win/client/phi_url_router.h"

namespace phi_client {

class PhiSettingsHost;

// The Mac's universal URL Rules editor (Sidebar/Spaces/SpaceURLRulesEditor:
// URLRulesEditor + RuleTableView): every Space's routing rules in ONE
// list, reached from the Spaces menu's "URL Rules…" and from Settings ▸
// Navigation ▸ "Manage URL Rules…". Each row is a match-type picker, the
// value field and a remove button on its first line, and "Open in" with
// the destination picker — the user's Spaces ("Name — Profile"), one
// generic Incognito entry, Kiosk, and "Ask every time" — on its second.
// Rows drag to reorder (the order is the routing tiebreak). Nothing is
// written until Save: the whole list replaces the stored rules at once
// (PhiUrlRouter::ReplaceRules, the Mac's setAllRules), and a save whose
// rows equal what was loaded is a no-op. A row Save cannot store (no
// usable pattern, or an auto-route rule whose Space is gone) is outlined
// and the window stays open with a note instead of dropping it silently.
// Cancel, Escape and the close button discard — after asking, when there
// are unsaved edits.
//
// App-global like Settings and Feedback: one window, re-shown by a second
// request. A PhiSecondaryWindow on the shared popup chrome; the rule list
// is a child PhiPanel that owns the rows' EDIT fields so scrolling clips
// them, and the window registers kPhiDialogTab so Tab walks the fields and
// buttons from the MAIN message loop (phi_secondary_window.h).
//
// Reads through PhiSettingsHost — the rules (mutable, the one service the
// settings host hands over that way), the Space list and the profile
// names — so the coordinator adds no seam of its own.
class PhiUrlRulesWindow : public PhiSecondaryWindow {
 public:
  static void Open(PhiSettingsHost* host, HWND anchor);
  // The open window, or null.
  static HWND Handle();
  // Sign-out closes the window without the discard prompt (the Mac's
  // closeNonBrowserWindows: the sign-out is confirmed already). Posted.
  static void CloseForSignOut();

  ~PhiUrlRulesWindow() override;

  // One editable row: the Mac's URLRulesEditor.Row.
  struct Row {
    int id = 0;  // Session identity; names the row's EDIT (kFieldIdBase + id).
    UrlRuleMatchType type = UrlRuleMatchType::kDomainSuffix;
    std::string value;      // The display value the field shows.
    std::string target_id;  // A Space id, or an action target.
    bool ask = false;       // "Ask every time" (target_id is the default).
    bool invalid = false;   // The last Save could not store it (outlined).
  };

  // What the list panel reads and drives (the window owns the rows).
  std::vector<Row>& rows() { return rows_; }
  const PhiPopupColors& popup_colors() const { return colors(); }
  UINT window_dpi() const { return dpi(); }
  // The pickers, anchored under `anchor` (list-panel client pixels).
  void PickMatchType(int row_index, const RECT& anchor);
  void PickTarget(int row_index, const RECT& anchor);
  void RemoveRow(int row_index);
  // A row's field changed: keep the model in step so a reorder survives.
  void SetRowValue(int row_id, std::string value);
  // Any row the last Save refused (the footer shows its note).
  bool HasInvalidRows() const;
  // The destination picker's label for a row.
  std::wstring TargetLabel(const Row& row) const;
  const wchar_t* MatchTypeLabel(UrlRuleMatchType type) const;
  const wchar_t* MatchValuePlaceholder(UrlRuleMatchType type) const;

 private:
  class RuleList;

  explicit PhiUrlRulesWindow(PhiSettingsHost* host);
  bool OpenWindow(HWND anchor);
  void Build();
  void EnsureFonts();
  void Layout();
  void Load();
  void AddBlankRow();
  void Save();
  // Cancel / Escape / close: true when the window may go — nothing unsaved,
  // or the user chose Discard.
  bool ConfirmDiscard();
  // The edits moved a row: it is no longer the one Save refused.
  void ClearInvalid(Row* row);
  std::string Fingerprint() const;
  // Destinations a rule may target, in picker order: every user Space,
  // then Incognito, then Kiosk (the Mac's ruleTargetSpaces).
  struct Destination {
    std::string id;
    std::wstring label;
  };
  std::vector<Destination> Destinations() const;
  HWND Child(const wchar_t* cls,
             const wchar_t* text,
             DWORD style,
             UINT_PTR id);
  static LRESULT CALLBACK ControlProc(HWND hwnd,
                                      UINT msg,
                                      WPARAM wparam,
                                      LPARAM lparam,
                                      UINT_PTR id,
                                      DWORD_PTR data);

  // PhiSecondaryWindow.
  void Paint(HDC dc, const RECT& dirty) override;
  LRESULT OnWindowMessage(UINT msg,
                          WPARAM wparam,
                          LPARAM lparam,
                          bool* handled) override;
  void OnColorsChanged() override;
  void OnDpiChanged(UINT old_dpi) override;
  void OnEscape() override;
  void OnNcDestroy() override;

  RAW_PTR_EXCLUSION PhiSettingsHost* const host_;
  std::vector<Row> rows_;
  int next_row_id_ = 1;
  std::string initial_fingerprint_;
  std::unique_ptr<RuleList> list_;
  HBRUSH bg_brush_ = nullptr;
  HBRUSH divider_brush_ = nullptr;
  HFONT font_ = nullptr;
  HFONT headline_font_ = nullptr;
  HFONT small_font_ = nullptr;
  HWND add_ = nullptr;
  HWND cancel_ = nullptr;
  HWND save_ = nullptr;
  // Measured at layout: where the list sits between the header and the
  // footer, and the header's wrapped subtitle height.
  RECT header_ = {};
  RECT list_bounds_ = {};
  RECT footer_ = {};
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_URL_RULES_WINDOW_H_

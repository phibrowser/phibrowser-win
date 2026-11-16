// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_CONTENT_BLOCKING_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_CONTENT_BLOCKING_WINDOW_H_

#include <windows.h>

#include <memory>
#include <string>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_content_blocking.h"
#include "client/phi_secondary_window.h"

namespace phi_client {

class PhiSettingsHost;

// The Mac's ContentBlockingListStrings: a catalog list's title and
// description by id (a custom list's title is its name).
std::wstring PhiContentBlockingListTitle(const PhiContentBlockingList& list);
std::wstring PhiContentBlockingListDescription(
    const PhiContentBlockingList& list);
// The line under a switch that is on (the Mac's ContentBlockingRuleSets
// .summary): its checked lists, the custom lists, and their state.
std::wstring PhiContentBlockingSummary(PhiContentBlockingCategory category,
                                       const PhiContentBlockingState& state);

// The Mac's ContentBlockingRuleSetSheet and ContentBlockingCustomFilterSheet
// as one small window over Settings: "Rule sets for blocking ads" (or
// trackers, or cookie banners) lists the switch's catalog lists and the
// profile's custom lists, each with a checkbox (use it), a download button
// or a progress bar (fetch its text), Remove for a custom list, and an info
// button with the list's details. Choices apply as they are made. Done
// downloads every checked list still missing and closes once they are all
// on disk; a failed download keeps the window open with the error under
// the row, so Done can retry. "Update Downloaded" fetches the shown lists
// that are on disk again. "Add Filter…" turns the window into the Custom
// filter form (a URL to download, or pasted rules); Add or Cancel brings
// the list back.
//
// Opened from a switch that was off and has nothing usable, the window
// turns that switch on when it closes with a usable rule set, as the Mac's
// chooserClosed does. One window app-wide: a second request retargets it.
// Everything goes through PhiContentBlocking; the window only observes it.
class PhiContentBlockingWindow : public PhiSecondaryWindow,
                                 public PhiContentBlockingObserver {
 public:
  static void Open(PhiSettingsHost* host,
                   const std::string& profile_id,
                   PhiContentBlockingCategory category,
                   bool enable_on_close,
                   HWND anchor);
  // The open window, or null.
  static HWND Handle();
  // Settings is closing: the chooser goes with it (no switch is turned on).
  static void CloseQuietly();

  ~PhiContentBlockingWindow() override;

  // PhiContentBlockingObserver.
  void OnContentBlockingChanged(const std::string& profile_id) override;

  // What the list panel reads and drives.
  const PhiContentBlockingState* state() const;
  PhiContentBlockingCategory category() const { return category_; }
  const PhiPopupColors& popup_colors() const { return colors(); }
  void ToggleList(const std::string& list_id, bool checked);
  void DownloadList(const std::string& list_id);
  void RemoveCustomList(const std::string& list_id);
  void ShowListInfo(const std::string& list_id);
  void ShowAddFilter();
  // Escape inside the list.
  void CloseFromList();

 private:
  class ListPanel;
  enum class Page { kChooser, kAddFilter };

  PhiContentBlockingWindow(PhiSettingsHost* host,
                           const std::string& profile_id,
                           PhiContentBlockingCategory category,
                           bool enable_on_close);
  bool OpenWindow(HWND anchor);
  void Retarget(const std::string& profile_id,
                PhiContentBlockingCategory category,
                bool enable_on_close);
  void Build();
  void ApplyFonts();
  void Layout();
  void SyncControls();
  void ShowPage(Page page);
  void SetKind(bool rules);
  void Done();
  void UpdateDownloaded();
  void SubmitFilter();
  // Hide now, close on the window's own turn: the request may come from a
  // control's handler still on its stack.
  void CloseSoon();
  const wchar_t* Title() const;
  COLORREF FieldColor() const;
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
  RAW_PTR_EXCLUSION PhiContentBlocking* const blocking_;
  std::string profile_id_;
  PhiContentBlockingCategory category_;
  bool enable_on_close_ = false;
  bool quiet_close_ = false;
  bool closing_ = false;
  // Done started downloads and closes once they have all landed.
  bool waiting_for_downloads_ = false;
  Page page_ = Page::kChooser;
  bool kind_rules_ = false;  // Custom filter: pasted rules, not a URL.
  bool submitting_ = false;
  bool attempted_ = false;
  std::wstring submit_error_;
  std::unique_ptr<ListPanel> list_;
  HBRUSH bg_brush_ = nullptr;
  HBRUSH field_brush_ = nullptr;
  // The chooser's footer.
  HWND update_ = nullptr;
  HWND done_ = nullptr;
  // The Custom filter form.
  HWND kind_url_ = nullptr;
  HWND kind_rules_button_ = nullptr;
  HWND name_ = nullptr;
  HWND url_ = nullptr;
  HWND rules_ = nullptr;
  HWND add_ = nullptr;
  HWND cancel_ = nullptr;
  // Measured at layout.
  RECT header_ = {};
  RECT intro_ = {};
  RECT list_bounds_ = {};
  RECT footer_ = {};
  RECT name_label_ = {};
  RECT source_label_ = {};
  RECT error_ = {};
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CONTENT_BLOCKING_WINDOW_H_

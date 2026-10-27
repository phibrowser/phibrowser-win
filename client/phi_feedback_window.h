// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_FEEDBACK_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_FEEDBACK_WINDOW_H_

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_feedback_outbox.h"
#include "phibrowser-win/client/phi_popup_chrome.h"
#include "phibrowser-win/client/phi_secondary_window.h"
#include "phibrowser-win/client/phi_theme.h"

namespace phi_client {

// What a report is about: the window it was invoked from and its focused
// tab — the Mac FeedbackViewController's host window + focusingTab (or the
// crashed tab's snapshot when opened from a crash page).
struct PhiFeedbackContext {
  int64_t window_id = 0;  // The browser whose profile supplies system logs.
  std::string url;
  std::string title;
  // Off the record (an incognito window, an Incognito Space): the form
  // prefills nothing of the page — the user types a URL if they want one.
  bool incognito = false;
};

// What the form needs from the coordinator.
class PhiFeedbackHost {
 public:
  virtual ~PhiFeedbackHost() = default;
  // True with `email` when an account is signed in.
  virtual bool FeedbackAccount(std::string* email) = 0;
  virtual void RequestFeedbackSignIn() = 0;
  // Opens `url` in a new foreground tab of `window_id` and raises it (the
  // Privacy Policy / Terms of Service links).
  virtual void OpenFeedbackLink(int64_t window_id, const std::string& url) = 0;
  // Starts the framework's system-logs collection; the answer arrives at
  // PhiFeedbackWindow::OnSystemLogs. False when nothing will answer.
  virtual bool FetchFeedbackSystemLogs(int64_t window_id, int request_id) = 0;
  // Phi's own extensions in `window_id`'s profile, name -> version.
  virtual std::map<std::string, std::string> PhiExtensionVersions(
      int64_t window_id) = 0;
};

// The "Send feedback" window (docs/windows-native-ui.md §13, the canvas's
// "Send feedback" boards): app-global — one window, rebound to whichever
// browser window reopens it — with the description, the page it is about,
// attachment tiles (picked, dropped or pasted), the diagnostic-info
// expander, the privacy notice and a Send / Cancel footer. Send collects
// Chromium's system logs through the framework, files the report in the
// outbox and turns the window into a confirmation; the upload happens in the
// background from there.
//
// Windows Settings look on the shared popup chrome: the #F3F3F3 / #202020
// ground, Fluent text boxes (a 2 dip accent bottom edge on focus), cards,
// InfoBars for "sign in to send" and refused files instead of modal
// boxes. The window's height follows its content (an InfoBar, a second tile
// row or the expanded diagnostics grow it) up to the work area's height;
// past that the body scrolls above a pinned Send / Cancel footer. Modeless, like the Mac's — the
// user can go back to the page to check a detail while composing: it is a
// PhiSecondaryWindow that registers kPhiDialogTab, so its control set gets
// tab navigation from the MAIN message loop and no nested loop exists here
// (phi_secondary_window.h).
class PhiFeedbackWindow : public PhiSecondaryWindow {
 public:
  static void Open(PhiFeedbackHost* host,
                   const PhiFeedbackContext& context,
                   HWND anchor);
  // The framework answered a FetchFeedbackSystemLogs (or the request timed
  // out — `text` null): the pending Send proceeds.
  static void OnSystemLogs(int request_id, const char* text);
  // The open window, or null.
  static HWND Handle();
  // Sign-out closes the window without the discard prompt (the Mac's
  // closeNonBrowserWindows: the sign-out is confirmed already). Posted.
  static void CloseForSignOut();
  // The account behind feedback changed (signed in or out): the window
  // swaps its "Sending as" line and the sign-in InfoBar.
  static void OnAccountChanged();
  // The form's state for the harness: {"open","windowId","url","title",
  // "description","attachments":[{filename,size,image}],"submitting",
  // "signedIn","sent","diagnosticsExpanded","attachmentError"}.
  static std::string StateJson();
  // Drives the form from the control socket: sets what is given
  // (description / url), adds `attachFiles` (paths), and presses Send when
  // `send` is true. Empty on success, else why not: not_open,
  // empty_description, no_account (the driven Send never raises the
  // interactive sign-in prompt — a modal box would wedge the caller).
  static std::string Drive(const std::string& description,
                    bool set_description,
                    const std::string& url,
                    bool set_url,
                    const std::vector<std::wstring>& attach_files,
                    bool send);

  ~PhiFeedbackWindow() override;

 private:
  struct Attachment {
    PhiFeedbackDraftAttachment draft;
    std::wstring preview_path;  // A pasted image written out for Open.
    // The tile's thumbnail, premultiplied BGRA at `thumb_size` device px
    // (rebuilt when the DPI moves the tile).
    std::vector<unsigned char> thumb;
    SIZE thumb_size = {0, 0};
    bool thumb_failed = false;
    HWND tile = nullptr;
  };
  class DropTarget;
  // Where everything goes, in device px of the client area; recomputed
  // from the state by Layout().
  struct Geometry {
    RECT sign_in_bar = {};
    RECT title = {};
    RECT from = {};
    RECT description_label = {};
    RECT description_box = {};
    RECT help = {};
    RECT page_label = {};
    RECT page_box = {};
    RECT error_bar = {};
    RECT attach_label = {};
    RECT tiles = {};  // The tile grid, one or two rows.
    RECT diagnostics = {};  // The whole card, expanded body included.
    RECT legal = {};
    RECT footer = {};
    RECT progress = {};
    // The legal notice laid out word by word: plain runs painted, the two
    // links placed as buttons.
    struct Run {
      std::wstring text;
      RECT rect;
    };
    std::vector<Run> legal_runs;
    int content_height = 0;  // Design units below the band.
  };

  PhiFeedbackWindow(PhiFeedbackHost* host, const PhiFeedbackContext& context);
  bool OpenWindow();
  static LRESULT CALLBACK ControlProc(HWND hwnd,
                                      UINT msg,
                                      WPARAM wparam,
                                      LPARAM lparam,
                                      UINT_PTR id,
                                      DWORD_PTR data);

  void Build();
  void EnsureFonts();
  void ApplyFonts();
  // Computes the geometry for the current state, places the controls and
  // sizes the window to the content.
  void Layout();
  void LayoutLegal(HDC dc, int x, int* y, int width);
  // A form taller than the work area: the body scrolls (wheel, and the
  // focused control is kept in view) between the band and the pinned
  // footer.
  void ClipBodyControls();
  void ScrollTo(int px);
  void ScrollIntoView(HWND control);
  void Rebind(const PhiFeedbackContext& context);
  void RefreshAccount();
  void RebuildTiles();
  void RefreshSendState();
  void RefreshCounter();
  // `interactive` shows the refused files in the error InfoBar; a driven
  // call records them in `attachment_error_` as well (StateJson).
  bool AddFiles(const std::vector<std::wstring>& paths, bool interactive);
  void ShowAttachmentError(const std::wstring& text);
  void ChooseFiles();
  // Ctrl+V: files copied in Explorer attach as files, an image as a PNG.
  // False when the clipboard holds neither (the edit pastes its text).
  bool PasteAttachments();
  void PasteImage();
  void RemoveAttachment(size_t index);
  void OpenAttachment(size_t index);
  void RevealAttachment(size_t index);
  void ShowTileMenu(size_t index, POINT screen);
  size_t TileIndex(HWND tile) const;
  // The ✕ a hovered tile shows, in the tile's client px.
  RECT TileRemoveRect(HWND tile) const;
  void EnsureThumbnail(Attachment* a, SIZE px);
  // False when nothing was sent: blank, already sending, or no account.
  bool Send(bool interactive);
  void CancelSend();
  // The second half of Send: the logs arrived (or were given up on).
  void CompleteSend(const char* logs);
  void ShowSent();
  void SetDragging(bool dragging, int count);
  std::string DescriptionText() const;
  std::string UrlText() const;
  // The page's title, but only while the URL field still names the page
  // it came with: cleared or edited, the title no longer describes it.
  std::string PageTitleForReport() const;
  // Escape / the caption's close: a typed report is not thrown away
  // without asking. True when the window may close.
  bool ConfirmDiscard();
  HWND Child(const wchar_t* cls,
             const wchar_t* text,
             DWORD style,
             UINT_PTR id,
             DWORD ex_style = 0);

  // Painting.
  void PaintForm(HDC dc);
  void PaintSent(HDC dc);
  void PaintTextBox(HDC dc, const RECT& box, HWND edit);
  void PaintInfoBar(HDC dc,
                    const RECT& bar,
                    bool error,
                    const std::wstring& title,
                    const std::wstring& message,
                    int trailing_px);
  void PaintFooter(HDC dc);
  bool DrawOwnerItem(const DRAWITEMSTRUCT& item);
  void DrawTile(const DRAWITEMSTRUCT& item, size_t index);
  void DrawAddTile(const DRAWITEMSTRUCT& item);
  void DrawDiagnosticsHeader(const DRAWITEMSTRUCT& item);
  void DrawGlyphButton(const DRAWITEMSTRUCT& item, const wchar_t* glyph);
  void DrawFocusRing(HDC dc, const RECT& r, int radius);
  COLORREF Ink(int alpha) const;  // colors().ink over the ground.
  COLORREF FieldFill() const;

  // PhiSecondaryWindow.
  void Paint(HDC dc, const RECT& dirty) override;
  LRESULT OnWindowMessage(UINT msg,
                          WPARAM wparam,
                          LPARAM lparam,
                          bool* handled) override;
  void OnColorsChanged() override;
  void OnDpiChanged(UINT old_dpi) override;
  void OnNcDestroy() override;
  void OnTimerFired(UINT_PTR id) override;
  void OnWheel(int delta, POINT client_pt) override;

  RAW_PTR_EXCLUSION PhiFeedbackHost* const host_;
  PhiFeedbackContext context_;
  Geometry geo_;
  HBRUSH bg_brush_ = nullptr;
  HBRUSH card_brush_ = nullptr;
  HBRUSH field_brush_ = nullptr;
  HBRUSH disabled_brush_ = nullptr;
  HFONT font_ = nullptr;         // 14, body and fields.
  HFONT title_font_ = nullptr;   // 20 semibold.
  HFONT strong_font_ = nullptr;  // 14 semibold.
  HFONT small_font_ = nullptr;   // 12.
  HFONT tiny_font_ = nullptr;    // 11.
  HWND description_ = nullptr;
  HWND url_ = nullptr;
  HWND url_clear_ = nullptr;
  HWND add_ = nullptr;
  HWND diagnostics_ = nullptr;
  HWND sign_in_ = nullptr;
  HWND error_close_ = nullptr;
  HWND privacy_ = nullptr;
  HWND terms_ = nullptr;
  HWND send_ = nullptr;
  HWND cancel_ = nullptr;
  // The owner-drawn control under the pointer, and whether the pointer is
  // on a hovered tile's ✕.
  HWND hot_ = nullptr;
  bool hot_remove_ = false;
  // The last press on a tile began on its ✕ (BN_CLICKED then removes).
  bool press_on_remove_ = false;
  // How long Send waits for Chromium's system logs before filing without
  // them; an id out of the window's own pool, not the literal 1 the file
  // used to pick (lane 3 §4.1).
  Timer logs_timer_;
  // The indeterminate progress bar's frames while sending.
  Timer progress_timer_;
  int progress_phase_ = 0;
  std::vector<Attachment> attachments_;
  // The last refused-attachment message (the error InfoBar; StateJson).
  std::string attachment_error_;
  std::wstring error_text_;
  std::string email_;
  bool signed_in_ = false;
  bool diagnostics_expanded_ = false;
  // The expander's rows, read when it opens: label -> value.
  std::vector<std::pair<std::wstring, std::wstring>> diagnostic_rows_;
  // The flyout palette the tile menu paints with.
  PhiPalette menu_palette_;
  // Ctrl+V attached something: the edit's Ctrl+V character that follows is
  // swallowed.
  bool swallow_paste_char_ = false;
  bool submitting_ = false;
  bool sent_ = false;
  // Files are being dragged over the window: the tile row becomes the drop
  // zone.
  bool dragging_ = false;
  int drag_count_ = 0;
  // The OLE drop target, or null when RegisterDragDrop failed and
  // WM_DROPFILES carries drops instead.
  RAW_PTR_EXCLUSION DropTarget* drop_target_ = nullptr;
  int pending_request_ = 0;
  // Device px the body is scrolled, and how far it can go (0 = the whole
  // form fits the work area).
  int scroll_ = 0;
  int max_scroll_ = 0;
  // Body controls carry viewport regions (ClipBodyControls).
  bool clipped_children_ = false;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_FEEDBACK_WINDOW_H_

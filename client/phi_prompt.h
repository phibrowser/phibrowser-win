// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_PROMPT_H_
#define PHIBROWSER_WIN_CLIENT_PHI_PROMPT_H_

#include <windows.h>

#include <string>
#include <vector>

namespace phi_client {

// The modal dialogs, all drawn in the Mac's PhiAlert layout (465 wide, the
// Phi mark, an 18pt title, 14pt copy, 130x36 actions with the primary
// rightmost) on the shared popup chrome — a caption band with the Windows
// close button in the right corner, no OS title bar. Each is modal against
// its owner, as NSAlert.runModal is, and returns once dismissed.
//
// WHY THIS ONE MAY BLOCK, AND NOTHING ELSE MAY.
//
// Every other surface in the client is asynchronous: the popovers take a
// completion callback, the secondary windows get their dialog keys from
// the thread's active-dialog list inside the MAIN message loop
// (phi_secondary_window.h), and lane 3 §4.4's three nested GetMessage
// loops are down to the ONE inside PhiPrompt's Alert::Run. It survives
// because the functions below are called from a dozen places — Settings'
// rename / delete rows, the feedback form's sign-in offer, the URL
// router's "ask first" chooser, the shortcut recorder — that need the
// answer on the same stack and cannot be turned inside out from files
// this substrate does not own.
//
// What that loop is required to do, and does (it is the one audited
// implementation; do not grow a second):
//   * PhiModalScope for its whole duration, so a control-pipe verb
//     arriving on the socket cannot destroy the controller whose handler
//     is parked waiting for the answer;
//   * EnableWindow(owner, FALSE) on the way in and TRUE on the way out,
//     so the alert is modal against its owner the way NSAlert.runModal
//     is;
//   * IsDialogMessageW for its own control set, so Tab, Escape and
//     Return behave, plus an optional raw-message `filter` the chord
//     recorder needs BEFORE the dialog manager sees the key;
//   * a WM_NULL kick from Finish(), because `done` is only checked
//     between GetMessage returns.
// The alert therefore registers NO keys with the active-dialog list: its
// own loop already translates them, and doing both would hide the raw
// keys from the recorder.

// One action button. Actions are listed in DISPLAY order, left to right:
// the affirmative first, Cancel last (docs/windows-native-ui.md §6, impl
// log D2). Every button takes an equal share of the footer.
enum class PhiAlertRole { kPrimary, kSecondary, kDestructive };
struct PhiAlertAction {
  std::wstring label;
  PhiAlertRole role = PhiAlertRole::kSecondary;
};

// A PhiAlert: `title` over `message` (may hold newlines; wraps) and the
// actions. Returns the index of the pressed action; -1 when the alert was
// closed or escaped (`cancel_index` names an action Escape maps to instead).
// Return presses `default_index` (-1 = none, the Mac's irreversible-action
// mode). `owner` null centres on the work area.
int PhiAlert(HWND owner,
             const wchar_t* title,
             const wchar_t* message,
             const std::vector<PhiAlertAction>& actions,
             int default_index = -1,
             int cancel_index = -1);

// PhiAlert with a checkbox under the copy — the Mac's NSAlert
// suppressionButton ("Do not ask again"). `checked` carries the box's
// state in and out (read whichever action ended the alert).
int PhiAlertWithCheckbox(HWND owner,
                         const wchar_t* title,
                         const wchar_t* message,
                         const std::vector<PhiAlertAction>& actions,
                         const wchar_t* checkbox_label,
                         bool* checked,
                         int default_index = -1,
                         int cancel_index = -1);

// A one-button notice — the MessageBox(MB_OK) stand-in.
void PhiNotice(HWND owner, const wchar_t* title, const wchar_t* message);

// A modal one-line text prompt — the Mac's NSAlert with an accessory text
// field (rename / create dialogs). `value` carries the initial text in and
// the entered text out. False on Cancel or empty input. `confirm_label`
// null reads the localized "OK" (prompt.ok); the cancelling button is always
// prompt.cancel. A caller that passes its own label MUST have localized it.
bool PhiPromptText(HWND owner,
                   const wchar_t* title,
                   const wchar_t* message,
                   std::wstring* value,
                   const wchar_t* confirm_label = nullptr);

// What a NAME prompt (rename / new Space, profile, account) enforces before
// it returns. `trim` drops leading and trailing white space, so a blank
// entry counts as empty (false, as Cancel). `max_code_points` (0 = none)
// refuses a longer name with the reason and asks again, the text kept.
// Lengths are CODE POINTS, as the onboarding name step counts them — an
// emoji is one, not two UTF-16 units.
struct PhiTextRules {
  bool trim = false;
  size_t max_code_points = 0;
};
// The length every name prompt shares (the Mac's 100-character names).
inline constexpr size_t kPhiMaxNameCodePoints = 100;
// The rules a rename / create prompt uses.
inline constexpr PhiTextRules kPhiNameRules = {true, kPhiMaxNameCodePoints};
bool PhiPromptText(HWND owner,
                   const wchar_t* title,
                   const wchar_t* message,
                   std::wstring* value,
                   const wchar_t* confirm_label,
                   const PhiTextRules& rules);

// Code points in `text` (a surrogate pair counts once).
size_t PhiCodePointCount(const std::wstring& text);
// `text` without leading or trailing white space.
std::wstring PhiTrimWhitespace(const std::wstring& text);

// A modal confirmation. The confirming button (`confirm_label`, null = the
// localized prompt.ok) is destructive red by default — "Delete …?" — or the
// accent when `destructive` is false. True when the user confirms.
bool PhiConfirm(HWND owner,
                const wchar_t* title,
                const wchar_t* message,
                const wchar_t* confirm_label = nullptr,
                bool destructive = true);

// One keyboard chord — a virtual key plus its modifiers. vk 0 means
// "unbound" (a rebindable shortcut whose chord was stolen by another).
struct PhiShortcutChord {
  UINT vk = 0;
  bool ctrl = false;
  bool shift = false;
  bool alt = false;
  bool operator==(const PhiShortcutChord&) const = default;
};

// "Ctrl+Alt+Shift+Key" display form, matching the menu bar's labels;
// an unbound chord renders as an em dash.
std::wstring PhiFormatChord(const PhiShortcutChord& chord);

// A modal chord recorder — the Mac Shortcuts pane's inline capture. Shows
// `message` over a live preview of the held modifiers and waits for one
// complete chord (Ctrl or Alt required, except for function keys). Escape
// or Cancel returns false; a captured chord lands in `out`. A bare Delete
// (or Backspace) returns true with the UNBOUND chord (vk 0): the caller
// removes the shortcut, as the Mac recorder's Delete does.
bool PhiPromptShortcut(HWND owner,
                       const wchar_t* title,
                       const wchar_t* message,
                       PhiShortcutChord* out);

// A modal list chooser — the Win32 stand-in for the Mac's Space chooser
// shown when a navigation matches an "ask first" URL rule. `options` fills
// a list with `default_index` preselected; the confirming button (or a
// double-click) returns the selected index, the cancelling button / close
// returns -1. The labels default to the localized prompt.open /
// prompt.keepHere.
//
// `cancel_is_default`: the security prompts (agent consent, credential
// approval) must not be approvable by a reflex Enter — with it set, the
// cancelling button takes the focus and Return, so Enter returns -1 and
// only a deliberate click on the confirming button (or a double-click on a
// row) approves. The preselected row still says what "Continue" would do.
//
// `require_hardware_input` (the same prompts): the confirming button, the
// row double-click and the row they confirm answer only real keyboard or
// mouse input (GetCurrentInputMessageSource: IMO_HARDWARE / IMO_SYSTEM) —
// another program's BM_CLICK, UI Automation Invoke / Select, LB_SETCURSEL
// or SendInput cannot approve. Cancel, Esc and the close button still
// refuse whatever sent them.
int PhiPromptChoose(HWND owner,
                    const wchar_t* title,
                    const wchar_t* message,
                    const std::vector<std::wstring>& options,
                    int default_index,
                    const wchar_t* confirm_label = nullptr,
                    const wchar_t* cancel_label = nullptr,
                    bool cancel_is_default = false,
                    bool require_hardware_input = false);

// One row of the sectioned chooser below. `starts_section` draws a divider
// above the row (the Mac SpaceChooserView's Kiosk section); `current`
// trails the row with the "Current" status the Mac shows on the Space the
// navigation started in.
struct PhiChoiceRow {
  std::wstring label;
  bool starts_section = false;
  bool current = false;
};

// PhiPromptChoose with owner-drawn rows: the URL router's "ask first"
// chooser, whose list mixes the user's Spaces with the two action
// destinations. Same return contract as PhiPromptChoose.
int PhiPromptChooseRows(HWND owner,
                        const wchar_t* title,
                        const wchar_t* message,
                        const std::vector<PhiChoiceRow>& rows,
                        int default_index,
                        const wchar_t* confirm_label = nullptr,
                        const wchar_t* cancel_label = nullptr,
                        bool cancel_is_default = false);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_PROMPT_H_

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_LIBRARY_H_
#define PHIBROWSER_WIN_CLIENT_PHI_LIBRARY_H_

#include <windows.h>

namespace phi_client {

class PhiShellWindow;

// The Library's two sections (the Mac's LibraryViewModule.Section without
// Folio, which Windows opens as Mirage's page instead).
enum class PhiLibrarySection { kSpaces, kDownloads };

// The Mac Library (LibraryOverlayController + LibraryViewModule): a card
// over the browser window — a 136-wide rail (Spaces, Downloads) beside the
// section's content — under a 28 % scrim, flown in from the control that
// opened it. Spaces is one horizontal row of per-Space cards (icon, name,
// theme, pins grid, bookmark outline, a drag handle to reorder, a "…" menu)
// ending in a "+" that hosts the create-Space form inline; Downloads is the
// All Downloads list (search, day groups, the popover's row actions).
//
// One overlay exists at a time, bound to the shell it covers; it closes on
// Escape, a click on the scrim, the window's active tab or Space changing,
// and the shell going away — never on focus loss, because its popovers and
// menus take the focus. "Open Library in New Window" hosts the same view in
// a standalone PhiSecondaryWindow (one at a time too).
class PhiLibrary {
 public:
  // Opens the overlay over `shell` on `section` (re-sectioning an open
  // one). `source_screen`, when given, is the control the card flies out
  // of (the ⋯ button, the avatar); null flies from the card's centre.
  static void Open(PhiShellWindow* shell, PhiLibrarySection section);
  static void OpenFrom(PhiShellWindow* shell,
                       PhiLibrarySection section,
                       const RECT* source_screen);
  // Opens on the window's last section (Downloads at first), or closes an
  // overlay already up over `shell`.
  static void Toggle(PhiShellWindow* shell);
  // Closes the overlay (animated when it can). Safe when none is open.
  static void Close();
  static bool IsOpen();
  // The statics a shell calls on its own lifecycle, each a no-op unless
  // the open overlay (and the standalone window) belongs to `shell`.
  static void CloseIfBoundTo(PhiShellWindow* shell);
  // The shell's rows rebuilt: the cards re-read their Spaces, pins and
  // bookmarks; a changed active tab or Space closes the overlay unanimated
  // (the Mac's focusingTab subscription).
  static void RefreshIfBoundTo(PhiShellWindow* shell);
  // The shell's client area moved or resized.
  static void RepositionIfBoundTo(PhiShellWindow* shell);
  // Back to the top of the shell's card stack — the shell's kMsgRaiseCaps
  // turn, or the overlay drops under the browser child on the next raise.
  static void RaiseIfBoundTo(PhiShellWindow* shell);
  // The overlay's card window when one is open over `shell`, else null —
  // what another surface checks before treating a click as "outside".
  static HWND HwndIfBoundTo(PhiShellWindow* shell);
  // The shell's Escape while the overlay is up: an in-flight card drag or
  // inline rename takes it, otherwise the overlay closes. False when no
  // overlay is open over `shell` (the key is the shell's to handle).
  static bool HandleEscapeIfBoundTo(PhiShellWindow* shell);
  // The shell's Find (Ctrl+F) while the overlay is up: Spaces swallows it,
  // Downloads focuses its search field. False when none is open.
  static bool HandleFindIfBoundTo(PhiShellWindow* shell);

  // The standalone Library window for `shell`'s session, fronted if it is
  // already up (the rail's "Open Library in New Window").
  static void OpenInNewWindow(PhiShellWindow* shell,
                              PhiLibrarySection section);
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_LIBRARY_H_

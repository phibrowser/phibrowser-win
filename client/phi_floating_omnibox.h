// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_FLOATING_OMNIBOX_H_
#define PHIBROWSER_WIN_CLIENT_PHI_FLOATING_OMNIBOX_H_

#include <windows.h>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_popover.h"

namespace phi_client {

class PhiOmniboxSession;

// The Mac's centred omnibox (OmniBoxContainerViewController, opened by
// toggleOmniBox(fromAddressBar: false)): under "New tab behavior = Omnibox"
// Ctrl+T, the New Tab rows and an empty window open this card over the
// page instead of a new-tab page, and the tab is born when it commits.
//
// A popover — owned, activating, DWM-rounded and shadowed — holding its own
// EDIT, which joins the address field's PhiOmniboxSession (AttachField):
// the query, inline completion, the arrows, Enter, the keyword chip and the
// suggestion list (flush under the card, as under the new-tab page's box)
// are all the session's. The card only lays out and paints the box.
// Self-deleting; the session holds a raw pointer it clears in
// PhiOmniboxSession::OnFloatingDestroyed.
class PhiFloatingOmnibox : public PhiPopover {
 public:
  // Opens the card centred over `content` (screen pixels: the page area
  // beside the sidebar) and focuses its field. Null when the window could
  // not be created.
  static PhiFloatingOmnibox* Open(PhiOmniboxSession* session,
                                  HWND owner,
                                  const RECT& content);

  PhiFloatingOmnibox(const PhiFloatingOmnibox&) = delete;
  PhiFloatingOmnibox& operator=(const PhiFloatingOmnibox&) = delete;

  HWND field() const { return field_; }
  // Gone at once (a commit lands within the popover's opening grace, when
  // losing the activation would not dismiss it yet).
  void Close();
  // The chip or the hint came or went: re-seat the EDIT and repaint.
  void Relayout();
  // Where the session paints the chip and the hint: the text's span
  // without either, in client pixels.
  RECT TextArea() const;

 protected:
  void Paint(HDC dc, const RECT& dirty) override;
  void OnSize(int width, int height) override;
  void OnDpiChanged(UINT old_dpi) override;
  void OnDismiss() override;
  void OnNcDestroy() override;
  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) override;

 private:
  explicit PhiFloatingOmnibox(PhiOmniboxSession* session);
  ~PhiFloatingOmnibox() override;

  bool Create(HWND owner, const RECT& content);
  HFONT FieldFont();

  RAW_PTR_EXCLUSION PhiOmniboxSession* session_;
  HWND field_ = nullptr;
  HBRUSH brush_ = nullptr;
  COLORREF brush_color_ = CLR_INVALID;
  bool closing_ = false;  // Close() ran: hidden, WM_CLOSE on its way.
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_FLOATING_OMNIBOX_H_

// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_opaque_edit.h"

#include <cstdint>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "phibrowser-win/client/phi_panel.h"

namespace phi_client {

bool PhiPaintEditOpaque(HWND field,
                        WNDPROC base_proc,
                        COLORREF ground,
                        std::unique_ptr<PhiLayerSurface>& surface) {
  if (!field || !base_proc) {
    return false;
  }
  RECT rc = {};
  ::GetClientRect(field, &rc);
  if (rc.right <= 0 || rc.bottom <= 0) {
    return false;
  }
  if (!surface || surface->width() != rc.right ||
      surface->height() != rc.bottom) {
    surface = std::make_unique<PhiLayerSurface>(rc.right, rc.bottom);
  }
  if (!surface->valid()) {
    return false;
  }
  PAINTSTRUCT ps = {};
  HDC dc = ::BeginPaint(field, &ps);
  const HDC buffer = surface->dc();
  HBRUSH brush = ::CreateSolidBrush(ground);
  ::FillRect(buffer, &rc, brush);
  ::DeleteObject(brush);
  ::CallWindowProcW(base_proc, field, WM_PRINTCLIENT,
                    reinterpret_cast<WPARAM>(buffer),
                    PRF_CLIENT | PRF_ERASEBKGND);
  ::GdiFlush();
  // SAFETY: PhiLayerSurface owns a tightly packed, top-down 32-bit DIB of
  // byte_size() bytes.
  auto pixels = UNSAFE_BUFFERS(base::span<uint32_t>(
      static_cast<uint32_t*>(surface->bits()), surface->byte_size() / 4));
  for (uint32_t& pixel : pixels) {
    pixel |= 0xFF000000u;
  }
  ::BitBlt(dc, 0, 0, rc.right, rc.bottom, buffer, 0, 0, SRCCOPY);
  ::EndPaint(field, &ps);
  return true;
}

bool PhiEditDrawsDirectly(UINT msg, WPARAM wparam) {
  switch (msg) {
    case WM_CHAR:
    case WM_KEYDOWN:
    case WM_SETTEXT:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case EM_SETSEL:
    case EM_REPLACESEL:
    case EM_UNDO:
    case WM_PASTE:
    case WM_CUT:
    case WM_CLEAR:
    case WM_UNDO:
    case WM_IME_COMPOSITION:
    case WM_IME_ENDCOMPOSITION:
      return true;
    case WM_MOUSEMOVE:
      return (wparam & MK_LBUTTON) != 0;  // A drag-selection.
    default:
      return false;
  }
}

}  // namespace phi_client

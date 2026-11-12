// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_folder_icon_picker.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "client/phi_dwrite.h"
#include "client/phi_folder_icons.h"
#include "client/phi_mac_icons.h"
#include "client/phi_modal_scope.h"
#include "client/phi_popover.h"
#include "client/phi_theme.h"
#include "client/phi_shell_window.h"

namespace phi_client {

namespace {

// BookmarkFolderIconPicker's metrics, in DIP.
constexpr int kColumns = 6;
constexpr int kItemSize = 26;
constexpr int kIconSize = 20;
constexpr int kColumnSpacing = 16;
constexpr int kRowSpacing = 4;
constexpr int kGridWidth = 236;
constexpr int kPickerWidth = 264;
constexpr int kVerticalPad = 12;
constexpr int kItemRadius = 10;

// The picker keeps its catalog grid, its Paint() and its click handling.
// Placement, hover, the back buffer, dismissal and lifetime are
// PhiPopover's; the modal GetMessage loop the file used to run is gone
// (lane 3 §4.4 — one of the two this phase deletes).
class Picker : public PhiPopover {
 public:
  Picker(PhiShellWindow* controller,
         std::string selected,
         PhiFolderIconChosen on_chosen)
      : controller_(controller),
        selected_(std::move(selected)),
        on_chosen_(std::move(on_chosen)) {}

  ~Picker() override = default;

  bool Open(const RECT& anchor_screen) {
    HWND owner = controller_ ? controller_->shell_hwnd() : nullptr;
    if (!owner) {
      return false;
    }
    ClassSpec spec;
    spec.name = L"PhiFolderIconPicker";

    Options options;  // Activating, dismiss on inactive or Escape.

    Placement placement;
    placement.anchor = anchor_screen;
    placement.side = PhiPopoverSide::kBelow;
    placement.align = PhiPopoverAlign::kLeading;
    placement.gap_dip = 4;

    SIZE probe = {1, 1};
    if (!CreatePopover(spec, owner, probe, placement, options)) {
      return false;
    }
    BuildCells();
    SIZE size = {Scaled(kPickerWidth), PickerHeight()};
    Reposition(placement, size);
    Show();
    return true;
  }

  // Hands the callback back when Open() failed: no window means no
  // OnNcDestroy, so the caller answers it.
  PhiFolderIconChosen TakeCallback() {
    PhiFolderIconChosen callback = std::move(on_chosen_);
    on_chosen_ = PhiFolderIconChosen();
    return callback;
  }

 protected:
  PhiPalette QueryPalette() const override {
    return controller_ ? controller_->CurrentPalette()
                       : PhiResolvePalette(PhiThemeById("pure"),
                                           PhiAppearance::kSystem);
  }

  void OnDpiChanged(UINT) override {
    BuildCells();
    ::SetWindowPos(hwnd(), nullptr, 0, 0, Scaled(kPickerWidth), PickerHeight(),
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }

  void OnClick(int hit, POINT) override {
    if (hit >= 0) {
      const std::vector<PhiFolderIcon>& all = PhiFolderIconsAll();
      const size_t index = static_cast<size_t>(hits_[static_cast<size_t>(hit)].key);
      if (index < all.size()) {
        chosen_ = all[index].id;
      }
    }
    Dismiss();
  }

  void OnNcDestroy() override {
    // Exactly once, after the window is gone: the callback may open
    // another popover, or destroy the controller's menu frame.
    if (on_chosen_) {
      PhiFolderIconChosen callback = std::move(on_chosen_);
      on_chosen_ = PhiFolderIconChosen();
      callback(chosen_);
    }
  }

  void Paint(HDC dc, const RECT&) override {
    RECT rc = {};
    ::GetClientRect(hwnd(), &rc);
    const PhiPalette& colors = palette();
    HBRUSH ground = ::CreateSolidBrush(colors.window_bg);
    ::FillRect(dc, &rc, ground);
    ::DeleteObject(ground);

    const std::vector<PhiFolderIcon>& all = PhiFolderIconsAll();
    for (size_t i = 0; i < hits_.size() && i < all.size(); ++i) {
      const RECT cell = PhiToRect(hits_[i].bounds);
      const bool is_selected = selected_ == all[i].id;
      const bool is_hover = hovered() == static_cast<int>(i);
      if (is_selected || is_hover) {
        const int r = Scaled(kItemRadius);
        PhiFillRoundRectAA(dc, cell, r,
                           is_selected ? colors.hover_strong : colors.hover);
        PhiStrokeRoundRectAA(dc, cell, r,
                             is_selected ? colors.accent : colors.border, 1);
      }
      const PhiMacIcon* mask = PhiFolderIconMask(all[i].id);
      if (!mask) {
        continue;
      }
      const int px = Scaled(kIconSize);
      RECT art = {cell.left + ((cell.right - cell.left) - px) / 2,
                  cell.top + ((cell.bottom - cell.top) - px) / 2, 0, 0};
      art.right = art.left + px;
      art.bottom = art.top + px;
      const std::vector<unsigned char> bgra =
          PhiTintedIconBgra(*mask, colors.label, px);
      if (!bgra.empty()) {
        PhiBlendBgra(dc, art, bgra.data(), px, px);
      }
    }
  }

 private:
  int PickerHeight() const {
    const size_t count = PhiFolderIconsAll().size();
    const int rows = static_cast<int>((count + kColumns - 1) / kColumns);
    return 2 * Scaled(kVerticalPad) + rows * Scaled(kItemSize) +
           std::max(0, rows - 1) * Scaled(kRowSpacing);
  }

  // THE layout pass. One cell per catalog icon, into the base's hit list:
  // hover, click routing and (later) the UIA grid all read it.
  void BuildCells() {
    ClearHits();
    const std::vector<PhiFolderIcon>& all = PhiFolderIconsAll();
    const int item = Scaled(kItemSize);
    const int gap = Scaled(kColumnSpacing);
    const int row_gap = Scaled(kRowSpacing);
    const int grid_left = (Scaled(kPickerWidth) - Scaled(kGridWidth)) / 2;
    for (size_t i = 0; i < all.size(); ++i) {
      const int column = static_cast<int>(i) % kColumns;
      const int row = static_cast<int>(i) / kColumns;
      const int x = grid_left + column * (item + gap);
      const int y = Scaled(kVerticalPad) + row * (item + row_gap);
      RECT cell = {x, y, x + item, y + item};
      AddHit(cell, static_cast<int>(i), base::UTF8ToWide(all[i].name),
             PhiHitRole::kListItem, selected_ == all[i].id);
    }
  }

  // The controller outlives the picker (it owns the shell that owns it).
  RAW_PTR_EXCLUSION PhiShellWindow* const controller_;
  std::string selected_;
  std::string chosen_;
  PhiFolderIconChosen on_chosen_;
};

}  // namespace

void PhiOpenFolderIconPicker(PhiShellWindow* controller,
                             const RECT& anchor_screen,
                             const std::string& selected,
                             PhiFolderIconChosen on_chosen) {
  if (!controller || !controller->shell_hwnd()) {
    if (on_chosen) {
      on_chosen(std::string());
    }
    return;
  }
  Picker* picker = new Picker(controller, PhiResolveFolderIcon(selected),
                              std::move(on_chosen));
  picker->DeleteWhenDestroyed();
  if (!picker->Open(anchor_screen)) {
    // Open() never showed a window, so OnNcDestroy will not run: answer
    // here (a cancel) and drop the object.
    PhiFolderIconChosen callback = picker->TakeCallback();
    delete picker;
    if (callback) {
      callback(std::string());
    }
  }
}

std::string PhiPickFolderIcon(PhiShellWindow* controller,
                              const RECT& anchor_screen,
                              const std::string& selected) {
  // On the heap, not this frame: a WM_QUIT can end the loop below while
  // the picker is still up, and its later answer must land somewhere.
  struct Answer {
    std::string chosen;
    bool done = false;
  };
  auto answer = std::make_shared<Answer>();
  PhiOpenFolderIconPicker(controller, anchor_screen, selected,
                          [answer](const std::string& icon_id) {
                            answer->chosen = icon_id;
                            answer->done = true;
                          });
  if (answer->done) {
    return answer->chosen;  // The picker never opened.
  }
  // Control verbs from the pipe are deferred while this loop is on the
  // stack, so nothing can destroy the owning controller under it. Unlike
  // the loop this replaced, no state lives on this frame — the picker
  // owns itself and the answer is shared.
  {
    PhiModalScope modal;
    MSG msg = {};
    BOOL got = 1;  // > 0 until GetMessageW says otherwise.
    while (!answer->done && (got = ::GetMessageW(&msg, nullptr, 0, 0)) > 0) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
    if (got == 0) {
      // WM_QUIT belongs to the outer loop: put it back for it to see.
      ::PostQuitMessage(static_cast<int>(msg.wParam));
    }
  }
  return answer->chosen;
}

}  // namespace phi_client

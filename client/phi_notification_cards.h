// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_NOTIFICATION_CARDS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_NOTIFICATION_CARDS_H_

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "client/phi_theme.h"

namespace phi_client {

class PhiNotificationCardView;

// The Mac's NotificationCardManager for agent cards (notification.show,
// phibrowser-mac 36b43944): an agent asks the user something with a title,
// a message and one button; the user accepts (the button), rejects (the ✕)
// or lets it expire, and the decision goes back to THAT agent session only
// as a notification.response event. Hiding the stack decides nothing.
//
// One shared queue of at most five cards, newest first; a sixth evicts the
// oldest, which answers `timeout` exactly as expiry does. The stack shows
// one card at a time (‹ › pages through the rest) in the bottom-right
// corner of the user's focused Phi window, never taking focus; with no user
// window open the cards wait, and the next window that comes up shows
// them. UI thread only.
class PhiNotificationCards {
 public:
  struct Request {
    std::string title;
    std::string message;
    std::string button_title;  // Empty: "Run", as on the Mac.
    int expires_in_seconds = 300;
  };
  // Where a decision goes: the capability that asked, and the event's
  // payload ({notificationId, decision}).
  using DecisionSink = std::function<void(const std::string& capability_id,
                                          const std::string& payload_json)>;
  // The user window the stack shows over; null when there is none.
  using OwnerProvider = std::function<HWND()>;
  using PaletteProvider = std::function<PhiPalette()>;
  // The user's doings with the cards, for notification_card_action
  // (phi_client_analytics.h): "show", "run", "dismiss", "delete". Never a
  // timeout or an eviction — those are nobody's action.
  using ActionSink = std::function<void(const char* action)>;

  PhiNotificationCards(DecisionSink sink,
                       OwnerProvider owner,
                       PaletteProvider palette,
                       ActionSink action);
  ~PhiNotificationCards();

  PhiNotificationCards(const PhiNotificationCards&) = delete;
  PhiNotificationCards& operator=(const PhiNotificationCards&) = delete;

  // Queues a card for `capability_id` and shows the stack. Returns the
  // notification id ("agent-notification:<uuid>") — the acknowledgement
  // of queue insertion, not of visibility.
  std::string Show(const Request& request, const std::string& capability_id);
  // A user window came up or moved to the front: show waiting cards there.
  void Refresh();
  // The owner window moved or resized: the stack follows it.
  static void NoteOwnerMoved(HWND owner);

  size_t count() const { return cards_.size(); }

 private:
  friend class PhiNotificationCardView;

  struct Card {
    std::string id;
    std::wstring title;
    std::wstring message;
    std::wstring button_title;
    uint64_t expires_at_ms = 0;  // GetTickCount64 clock.
    std::string capability_id;
  };

  // The view's actions on the card it shows.
  void Decide(size_t index, const char* decision);
  void HideStack();
  void Page(int delta);

  // Removes cards_[index] and answers `decision` to its asker.
  void Answer(size_t index, const char* decision);
  void NoteAction(const char* action);
  // Brings the view in line with the queue. `allow_recreate` is false on
  // the view's own click path, where the view must outlive the call.
  void Present(bool allow_recreate);
  void ArmExpiry();
  void ExpireDue();
  static void CALLBACK ExpiryTimerProc(HWND, UINT, UINT_PTR, DWORD);

  DecisionSink sink_;
  OwnerProvider owner_;
  PaletteProvider palette_;
  ActionSink action_;
  std::vector<Card> cards_;  // Newest first.
  // The card the view shows, for "show" once per card that becomes the
  // visible one (the Mac's onChange of the presented card's id); cleared
  // when nothing is on screen, so a re-presented stack counts again.
  std::string shown_id_;
  size_t current_ = 0;
  // The user hid the stack; a NEW card shows it again (the Mac's
  // isExplicitlyHidden).
  bool hidden_ = false;
  UINT_PTR expiry_timer_ = 0;
  std::unique_ptr<PhiNotificationCardView> view_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_NOTIFICATION_CARDS_H_

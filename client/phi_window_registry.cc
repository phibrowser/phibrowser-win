// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_window_registry.h"

#include <utility>

namespace phi_client {

PhiWindowRegistry::PhiWindowRegistry() = default;
PhiWindowRegistry::~PhiWindowRegistry() = default;

PhiShellWindow* PhiWindowRegistry::AddOwned(
    SlotId slot,
    ControllerPtr controller,
    void (*invalidate)(PhiShellWindow*),
    bool agent,
    bool incognito) {
  PhiShellWindow* raw = controller.get();
  // insert_or_assign rather than operator[]: a Slot cannot be default
  // constructed (its deleter is a function pointer), and a repeated Add for
  // one slot id — which should not happen — replaces rather than leaks.
  slots_.insert_or_assign(
      slot, Slot{std::move(controller), invalidate, agent, incognito});
  return raw;
}

bool PhiWindowRegistry::Remove(SlotId slot) {
  auto it = slots_.find(slot);
  if (it == slots_.end()) {
    return false;
  }
  // Bindings first: nothing may resolve a browser to a controller that is
  // being destroyed.
  for (auto bound = slot_of_browser_.begin(); bound != slot_of_browser_.end();) {
    bound = bound->second == slot ? slot_of_browser_.erase(bound) : ++bound;
  }
  slots_.erase(it);
  return true;
}

PhiShellWindow* PhiWindowRegistry::Get(SlotId slot) const {
  const Slot* found = Find(slot);
  return found ? found->controller.get() : nullptr;
}

const PhiWindowRegistry::Slot* PhiWindowRegistry::Find(SlotId slot) const {
  auto it = slots_.find(slot);
  return it == slots_.end() ? nullptr : &it->second;
}

std::vector<PhiShellWindow*> PhiWindowRegistry::Controllers() const {
  std::vector<PhiShellWindow*> out;
  out.reserve(slots_.size());
  for (const auto& [slot, entry] : slots_) {
    out.push_back(entry.controller.get());
  }
  return out;
}

std::vector<PhiShellWindow*> PhiWindowRegistry::UserControllers() const {
  std::vector<PhiShellWindow*> out;
  for (const auto& [slot, entry] : slots_) {
    if (!entry.agent && !entry.incognito) {
      out.push_back(entry.controller.get());
    }
  }
  return out;
}

SlotId PhiWindowRegistry::FirstUserSlot() const {
  for (const auto& [slot, entry] : slots_) {
    if (!entry.agent && !entry.incognito) {
      return slot;
    }
  }
  return SlotId();
}

PhiShellWindow* PhiWindowRegistry::FirstUserController() const {
  return Get(FirstUserSlot());
}

bool PhiWindowRegistry::IsAgentSlot(SlotId slot) const {
  const Slot* found = Find(slot);
  return found && found->agent;
}

bool PhiWindowRegistry::IsIncognitoSlot(SlotId slot) const {
  const Slot* found = Find(slot);
  return found && found->incognito;
}

void PhiWindowRegistry::Invalidate(SlotId slot) const {
  const Slot* found = Find(slot);
  if (found && found->invalidate) {
    found->invalidate(found->controller.get());
  }
}

void PhiWindowRegistry::InvalidateAll() const {
  for (const auto& [slot, entry] : slots_) {
    if (entry.invalidate) {
      entry.invalidate(entry.controller.get());
    }
  }
}

void PhiWindowRegistry::InvalidateUserSlots() const {
  // The agent pips live in the user windows' switchers AND in an agent
  // window's own strip (the watched one selected there), so both repaint.
  for (const auto& [slot, entry] : slots_) {
    if (!entry.incognito && entry.invalidate) {
      entry.invalidate(entry.controller.get());
    }
  }
}

std::optional<SlotId> PhiWindowRegistry::SlotFor(BrowserId browser) const {
  auto it = slot_of_browser_.find(browser);
  if (it == slot_of_browser_.end()) {
    return std::nullopt;
  }
  return it->second;
}

PhiShellWindow* PhiWindowRegistry::Resolve(int64_t window_id) const {
  if (window_id == 0) {
    return FirstUserController();
  }
  if (PhiShellWindow* slot = Get(SlotId{window_id})) {
    return slot;
  }
  return ControllerForBrowser(BrowserId{window_id});
}

PhiShellWindow* PhiWindowRegistry::ControllerForBrowser(
    BrowserId browser) const {
  const std::optional<SlotId> slot = SlotFor(browser);
  return slot ? Get(*slot) : nullptr;
}

void PhiWindowRegistry::Bind(BrowserId browser, SlotId slot) {
  if (browser.valid() && slot.valid()) {
    slot_of_browser_[browser] = slot;
  }
}

void PhiWindowRegistry::Unbind(BrowserId browser) {
  slot_of_browser_.erase(browser);
}

bool PhiWindowRegistry::IsBound(BrowserId browser) const {
  return slot_of_browser_.count(browser) > 0;
}

std::vector<BrowserId> PhiWindowRegistry::Browsers() const {
  std::vector<BrowserId> out;
  out.reserve(slot_of_browser_.size());
  for (const auto& [browser, slot] : slot_of_browser_) {
    out.push_back(browser);
  }
  return out;
}

PhiKioskWindow* PhiWindowRegistry::AddKioskOwned(BrowserId browser,
                                                 KioskPtr kiosk) {
  PhiKioskWindow* raw = kiosk.get();
  kiosks_.insert_or_assign(browser, std::move(kiosk));
  return raw;
}

bool PhiWindowRegistry::RemoveKiosk(BrowserId browser) {
  return kiosks_.erase(browser) > 0;
}

PhiKioskWindow* PhiWindowRegistry::Kiosk(BrowserId browser) const {
  auto it = kiosks_.find(browser);
  return it == kiosks_.end() ? nullptr : it->second.get();
}

std::vector<PhiKioskWindow*> PhiWindowRegistry::Kiosks() const {
  std::vector<PhiKioskWindow*> out;
  out.reserve(kiosks_.size());
  for (const auto& [browser, kiosk] : kiosks_) {
    out.push_back(kiosk.get());
  }
  return out;
}

void PhiWindowRegistry::AddPending(phi_framework::RequestToken token,
                                   PendingCreation creation) {
  if (token.valid()) {
    pending_.insert_or_assign(token, std::move(creation));
  }
}

std::optional<PendingCreation> PhiWindowRegistry::TakePending(
    phi_framework::RequestToken token) {
  auto it = pending_.find(token);
  if (it == pending_.end()) {
    return std::nullopt;
  }
  PendingCreation creation = std::move(it->second);
  pending_.erase(it);
  return creation;
}

PendingCreation* PhiWindowRegistry::FindPendingSpace(
    SlotId slot,
    const std::string& space_id) {
  for (auto& [token, creation] : pending_) {
    if (creation.kind == phi_framework::WindowKind::kSpace &&
        creation.slot == slot && creation.space_id == space_id) {
      return &creation;
    }
  }
  return nullptr;
}

bool PhiWindowRegistry::HasPendingTask(const std::string& task_id) const {
  if (task_id.empty()) {
    return false;
  }
  for (const auto& [token, creation] : pending_) {
    if ((creation.kind == phi_framework::WindowKind::kAgent ||
         creation.kind == phi_framework::WindowKind::kShadow) &&
        creation.task_id == task_id) {
      return true;
    }
  }
  return false;
}

void PhiWindowRegistry::AddPendingGhost(BrowserId restored_from,
                                        PendingCreation creation) {
  if (restored_from.valid()) {
    pending_ghosts_.insert_or_assign(restored_from, std::move(creation));
  }
}

std::optional<PendingCreation> PhiWindowRegistry::TakePendingGhost(
    BrowserId restored_from) {
  auto it = pending_ghosts_.find(restored_from);
  if (it == pending_ghosts_.end()) {
    return std::nullopt;
  }
  PendingCreation creation = std::move(it->second);
  pending_ghosts_.erase(it);
  return creation;
}

std::vector<PendingCreation> PhiWindowRegistry::ExpirePending(
    uint64_t now_tick) {
  std::vector<PendingCreation> expired;
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->second.deadline_tick && it->second.deadline_tick <= now_tick) {
      if (it->second.kind == phi_framework::WindowKind::kSpace &&
          !it->second.open_urls.empty()) {
        expired_spaces_.insert_or_assign(it->first, it->second);
        if (expired_spaces_.size() > kMaxExpiredSpaces) {
          expired_spaces_.erase(expired_spaces_.begin());
        }
      }
      expired.push_back(std::move(it->second));
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = pending_ghosts_.begin(); it != pending_ghosts_.end();) {
    if (it->second.deadline_tick && it->second.deadline_tick <= now_tick) {
      expired.push_back(std::move(it->second));
      it = pending_ghosts_.erase(it);
    } else {
      ++it;
    }
  }
  return expired;
}

std::optional<PendingCreation> PhiWindowRegistry::TakeExpiredSpace(
    phi_framework::RequestToken token) {
  auto it = expired_spaces_.find(token);
  if (it == expired_spaces_.end()) {
    return std::nullopt;
  }
  PendingCreation creation = std::move(it->second);
  expired_spaces_.erase(it);
  return creation;
}

}  // namespace phi_client

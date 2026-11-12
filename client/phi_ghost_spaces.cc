// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_ghost_spaces.h"

#include <algorithm>
#include <optional>
#include <set>
#include <utility>

#include "base/json/json_reader.h"
#include "base/values.h"

namespace phi_client {

PhiGhostSpaces::PhiGhostSpaces(model::PhiRestoreModel* model)
    : model_(model) {}

PhiGhostSpaces::~PhiGhostSpaces() = default;

model::RememberedWindow* PhiGhostSpaces::FindRemembered(
    const std::string& space_id) {
  for (model::RememberedWindow& row : model_->remembered) {
    if (row.space_id == space_id) {
      return &row;
    }
  }
  return nullptr;
}

const model::RememberedWindow* PhiGhostSpaces::FindRemembered(
    const std::string& space_id) const {
  for (const model::RememberedWindow& row : model_->remembered) {
    if (row.space_id == space_id) {
      return &row;
    }
  }
  return nullptr;
}

void PhiGhostSpaces::NoteLiveWindow(const std::string& space_id,
                                    const std::string& profile,
                                    BrowserId window_id) {
  if (space_id.empty() || !window_id.valid()) {
    return;
  }
  bool changed = false;
  if (model::RememberedWindow* row = FindRemembered(space_id)) {
    changed = row->profile != profile || row->window_id != window_id.value;
    row->profile = profile;
    row->window_id = window_id.value;
  } else {
    model_->remembered.push_back({space_id, profile, window_id.value});
    changed = true;
  }
  // A secondary promoted to primary keeps one row, not two.
  if (EraseSecondaryRow(space_id, window_id)) {
    changed = true;
  }
  // A live window is by definition not parked.
  parked_.erase(space_id);
  noted_this_run_.insert(space_id);
  if (changed) {
    MarkDirty();
  }
}

void PhiGhostSpaces::NoteSecondaryWindow(const std::string& space_id,
                                         const std::string& profile,
                                         BrowserId window_id) {
  if (space_id.empty() || !window_id.valid()) {
    return;
  }
  if (!FindRemembered(space_id)) {
    // No primary to stand beside: this window IS the Space's window.
    NoteLiveWindow(space_id, profile, window_id);
    return;
  }
  bool first = true;
  for (model::RememberedWindow& row : model_->remembered) {
    if (row.space_id != space_id) {
      continue;
    }
    if (first) {
      first = false;  // The primary is never rewritten from here.
      continue;
    }
    if (row.window_id == window_id.value) {
      if (row.profile != profile) {
        row.profile = profile;
        MarkDirty();
      }
      return;
    }
  }
  model_->remembered.push_back({space_id, profile, window_id.value});
  MarkDirty();
}

void PhiGhostSpaces::ForgetSecondaryWindow(const std::string& space_id,
                                           BrowserId window_id) {
  if (space_id.empty() || !window_id.valid()) {
    return;
  }
  if (EraseSecondaryRow(space_id, window_id)) {
    MarkDirty();
  }
}

bool PhiGhostSpaces::EraseSecondaryRow(const std::string& space_id,
                                       BrowserId window_id) {
  bool primary_seen = false;
  for (auto it = model_->remembered.begin(); it != model_->remembered.end();
       ++it) {
    if (it->space_id != space_id) {
      continue;
    }
    if (!primary_seen) {
      primary_seen = true;  // The Space's first row is its primary: kept.
      continue;
    }
    if (it->window_id == window_id.value) {
      model_->remembered.erase(it);
      return true;
    }
  }
  return false;
}

bool PhiGhostSpaces::HasSecondaryWindow(const std::string& space_id,
                                        BrowserId window_id) const {
  bool first = true;
  for (const model::RememberedWindow& row : model_->remembered) {
    if (row.space_id != space_id) {
      continue;
    }
    if (!first && row.window_id == window_id.value) {
      return true;
    }
    first = false;
  }
  return false;
}

std::set<std::string> PhiGhostSpaces::Profiles() const {
  std::set<std::string> profiles;
  for (const model::RememberedWindow& row : model_->remembered) {
    profiles.insert(row.profile);
  }
  return profiles;
}

std::optional<PhiGhostSpaces::Remembered> PhiGhostSpaces::PrimaryWindow(
    const std::string& space_id) const {
  const model::RememberedWindow* row = FindRemembered(space_id);
  if (!row) {
    return std::nullopt;
  }
  return Remembered{row->profile, BrowserId{row->window_id}};
}

void PhiGhostSpaces::MarkDirty() const {
  if (dirty_callback_) {
    dirty_callback_();
  }
}

std::vector<int> PhiGhostSpaces::EagerWindowIds(
    const std::string& active_space_id) const {
  std::vector<int> eager;
  const model::RememberedWindow* row = FindRemembered(active_space_id);
  if (row && BrowserId{row->window_id}.valid()) {
    eager.push_back(static_cast<int>(row->window_id));
  }
  // Every secondary window too: a parked one could only come back through
  // the settle pass's re-host, a beat later and one by one.
  std::set<std::string> seen;
  for (const model::RememberedWindow& r : model_->remembered) {
    if (!seen.insert(r.space_id).second && BrowserId{r.window_id}.valid() &&
        std::find(eager.begin(), eager.end(),
                  static_cast<int>(r.window_id)) == eager.end()) {
      eager.push_back(static_cast<int>(r.window_id));
    }
  }
  return eager;
}

void PhiGhostSpaces::ReplaceFromReceipt(const std::string& receipt_json) {
  // REPLACE, never merge: the receipt describes the whole ghost store, so a
  // window missing from it is not parked any more (and one this reopen never
  // touched is still parked and must survive). The model's advisory copy is
  // replaced the same way — it records what the LAST receipt said.
  parked_.clear();
  orphans_.clear();
  strays_.clear();
  model_->parked.clear();
  MarkDirty();
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(receipt_json, base::JSON_PARSE_RFC);
  if (!root) {
    return;
  }
  const base::ListValue* parked = root->FindList("parked");
  if (!parked) {
    return;  // An empty receipt means nothing is parked — a real answer.
  }
  for (const base::Value& v : *parked) {
    const base::DictValue* d = v.GetIfDict();
    const std::string* profile = d ? d->FindString("profile") : nullptr;
    const base::ListValue* ids = d ? d->FindList("windowIds") : nullptr;
    if (!profile || !ids) {
      continue;
    }
    for (const base::Value& id_value : *ids) {
      if (!id_value.is_int()) {
        continue;
      }
      const BrowserId window_id{id_value.GetInt()};
      model_->parked[*profile].push_back(window_id.value);
      // Map back to whichever Space remembers this window: its primary
      // parks behind the Space, a secondary is re-hosted on settle.
      bool claimed = false;
      std::set<std::string> seen;
      for (const model::RememberedWindow& row : model_->remembered) {
        const bool primary = seen.insert(row.space_id).second;
        if (row.window_id == window_id.value && row.profile == *profile) {
          if (primary) {
            parked_[row.space_id] = Remembered{row.profile, window_id};
          } else {
            strays_.push_back(
                Stray{row.space_id, Remembered{row.profile, window_id}});
          }
          claimed = true;
          break;
        }
      }
      if (!claimed) {
        orphans_.push_back(Remembered{*profile, window_id});
      }
    }
  }
}

bool PhiGhostSpaces::IsParked(const std::string& space_id) const {
  return parked_.count(space_id) > 0;
}

const PhiGhostSpaces::Remembered* PhiGhostSpaces::Parked(
    const std::string& space_id) const {
  auto it = parked_.find(space_id);
  return it == parked_.end() ? nullptr : &it->second;
}

const std::string* PhiGhostSpaces::SpaceForWindowId(BrowserId window_id,
                                                    bool* secondary) const {
  if (!window_id.valid()) {
    return nullptr;
  }
  std::set<std::string> seen;
  for (const model::RememberedWindow& row : model_->remembered) {
    const bool primary = seen.insert(row.space_id).second;
    if (row.window_id == window_id.value) {
      if (secondary) {
        *secondary = !primary;
      }
      return &row.space_id;
    }
  }
  return nullptr;
}

void PhiGhostSpaces::ClearParked(const std::string& space_id) {
  parked_.erase(space_id);
}

PhiGhostSpaces::Remembered PhiGhostSpaces::Forget(const std::string& space_id) {
  Remembered gone;
  auto parked = parked_.find(space_id);
  if (parked != parked_.end()) {
    gone = parked->second;  // Still a ghost: the caller drops the window.
    parked_.erase(parked);
  }
  auto row = std::find_if(
      model_->remembered.begin(), model_->remembered.end(),
      [&](const model::RememberedWindow& r) { return r.space_id == space_id; });
  if (row != model_->remembered.end()) {
    if (!gone.window_id.valid()) {
      gone = Remembered{row->profile, BrowserId{row->window_id}};
    }
    // The primary and every secondary row with it.
    model_->remembered.erase(
        std::remove_if(model_->remembered.begin(), model_->remembered.end(),
                       [&](const model::RememberedWindow& r) {
                         return r.space_id == space_id;
                       }),
        model_->remembered.end());
    MarkDirty();
  }
  return gone;
}

std::vector<PhiGhostSpaces::Remembered> PhiGhostSpaces::Orphans() const {
  return orphans_;
}

void PhiGhostSpaces::ClearOrphans() {
  orphans_.clear();
}

std::vector<PhiGhostSpaces::Stray> PhiGhostSpaces::TakeStrays() {
  return std::exchange(strays_, {});
}

std::vector<PhiGhostSpaces::Stray> PhiGhostSpaces::TakeStrays(
    const std::string& space_id) {
  std::vector<Stray> taken;
  std::vector<Stray> kept;
  for (Stray& stray : strays_) {
    (stray.space_id == space_id ? taken : kept).push_back(std::move(stray));
  }
  strays_ = std::move(kept);
  return taken;
}

}  // namespace phi_client

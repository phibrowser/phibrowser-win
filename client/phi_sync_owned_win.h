// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_OWNED_WIN_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_OWNED_WIN_H_

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "client/phi_sync_bookmarks.h"
#include "client/phi_sync_pins.h"
#include "client/phi_sync_rules.h"
#include "client/phi_sync_settings_win.h"

namespace phi_client {

namespace store {
class LocalStore;
}  // namespace store

// One owned kind's cursor table as a JSON file, the Mac's names:
// <account dir>\sync\bookmarks-cursors.json / pins-cursors.json.
class PhiOwnedTableFile : public sync::OwnedTableStore {
 public:
  explicit PhiOwnedTableFile(std::wstring path);
  ~PhiOwnedTableFile() override;

  sync::OwnedTable Load(bool had_records, bool* reported_loss) override;
  bool Save(const sync::OwnedTable& table) override;
  bool Remove() override;

 private:
  const std::wstring path_;
  std::mutex lock_;
  // The last Load found the file unreadable or a newer build's.
  bool save_blocked_ = false;
};

// What the owned kinds need from the UI side (the coordinator).
struct PhiOwnedSyncHost {
  // The account's open store; null when this account's data is not open.
  std::function<store::LocalStore*()> store;
  // Around a landing: the store's change signal is not a local edit then.
  std::function<void(bool applying)> applying;
  // After a landing that wrote rows: repaint the sidebar and Settings.
  std::function<void()> landed;
  // URL rules: the Space model's pending rule edits are saved before sync
  // reads the table, and the model reloads after sync wrote it.
  std::function<void()> flush_rules;
  std::function<void()> rules_landed;
};

// PhiBookmarkLocalAccess over the Windows LocalStore. Called on the key
// worker; each read of the store is one UI-thread hop, and the snapshot is
// cached until the next AllBookmarks() / Apply().
class PhiSyncBookmarkAccess : public sync::BookmarkLocalAccess {
 public:
  PhiSyncBookmarkAccess(PhiRunOnUi run_on_ui, PhiOwnedSyncHost host);
  ~PhiSyncBookmarkAccess() override;

  bool AllBookmarks(std::vector<sync::LocalBookmark>* out) override;
  bool AllSyncIds(std::set<std::string>* out) override;
  bool CachedBookmarks(std::vector<sync::LocalBookmark>* out) override;
  std::vector<sync::LocalBookmark> Siblings(
      const std::optional<std::string>& parent_guid,
      const std::string& space_id) override;
  bool IsKnownLocalBookmark(const std::string& guid) override;
  std::optional<bool> LocalIsFolder(const std::string& guid) override;
  sync::BookmarkApplyResult Apply(
      const std::vector<sync::BookmarkApplyOp>& ops) override;

 private:
  struct Snapshot {
    bool loaded = false;
    std::vector<sync::LocalBookmark> rows;
    std::set<std::string> sync_ids;
    std::map<std::string, bool> is_folder;
  };
  // Runs on the UI thread.
  static Snapshot Read(const store::LocalStore& store);
  bool Rebuild();

  PhiRunOnUi run_on_ui_;
  PhiOwnedSyncHost host_;
  Snapshot cache_;
};

// PhiPinnedTabLocalAccess over the Windows LocalStore.
class PhiSyncPinAccess : public sync::PinLocalAccess {
 public:
  PhiSyncPinAccess(PhiRunOnUi run_on_ui, PhiOwnedSyncHost host);
  ~PhiSyncPinAccess() override;

  std::string CurrentScope() override;
  uint64_t ScopeGeneration() override;
  std::optional<std::string> AccountScope() override;
  bool AllPins(std::vector<sync::LocalPin>* out) override;
  bool AllPinRows(std::vector<sync::LocalPin>* out) override;
  bool CachedPins(std::vector<sync::LocalPin>* out) override;
  bool IsKnownLocalPin(const std::string& lineage,
                       const std::optional<std::string>& owner_key) override;
  sync::PinApplyResult Apply(const std::vector<sync::PinApplyOp>& ops) override;

 private:
  struct Snapshot {
    bool loaded = false;
    std::string scope = "profile";
    uint64_t generation = 0;  // the store's pin_scope_generation when read
    std::vector<sync::LocalPin> active;
    std::vector<sync::LocalPin> all;
    std::set<std::string> identity_pairs;
  };
  static Snapshot Read(const store::LocalStore& store);
  bool Rebuild();

  PhiRunOnUi run_on_ui_;
  PhiOwnedSyncHost host_;
  Snapshot cache_;
};

// PhiURLRuleLocalAccess over the Windows LocalStore: the batch runs as
// phi_sync_rules' pure function over the stored table, then the table is
// replaced in one flush.
class PhiSyncUrlRuleAccess : public sync::UrlRuleLocalAccess {
 public:
  PhiSyncUrlRuleAccess(PhiRunOnUi run_on_ui, PhiOwnedSyncHost host);
  ~PhiSyncUrlRuleAccess() override;

  bool AllRules(std::vector<sync::LocalUrlRule>* out) override;
  bool CachedRules(std::vector<sync::LocalUrlRule>* out) override;
  sync::UrlRuleApplyError Apply(const std::vector<sync::UrlRuleSyncOp>& ops,
                                const sync::UrlRuleMergeTail& merge_tail,
                                bool* failed,
                                sync::UrlRuleBatchOutcome* outcome) override;
  void HardDelete(const std::string& sync_id) override;
  int PurgeSoftDeleted(int64_t cutoff_ms) override;
  void ClearPendingLocalEdits(
      const std::map<std::string, sync::RuleProjection>& entries,
      bool clears_partner) override;

 private:
  // One UI-thread write: `mutate` edits the whole table; the table is
  // replaced when it returns true. False when the write did not run.
  bool Mutate(std::function<bool(std::vector<sync::LocalUrlRule>*)> mutate,
              bool notify);

  PhiRunOnUi run_on_ui_;
  PhiOwnedSyncHost host_;
  bool loaded_ = false;
  std::vector<sync::LocalUrlRule> cache_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_OWNED_WIN_H_

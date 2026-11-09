// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_GHOST_SPACES_H_
#define PHIBROWSER_WIN_CLIENT_PHI_GHOST_SPACES_H_

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "client/phi_app_model.h"
#include "client/phi_ids.h"

namespace phi_client {

// Which Spaces currently exist only as a PARKED window (a "ghost") rather
// than a live browser — the client half of lazy Space restore.
//
// The link between a Space and a parked window is the window's SessionID from
// the PREVIOUS run: the client remembers, per Space, the id its browser had,
// and that same id is what the session file (and so the ghost store) names.
// Materializing hands that id back to the framework, which rebuilds the
// window inside a ScopedRestoredFromWindowId scope so it can be re-attached
// to its Space.
//
// A VIEW, not a store: the remembered Space -> window rows live in the
// coordinator's model::PhiRestoreModel (PhiRestore.json, written by its
// PhiStore alongside the last-active Space), and this class is the query
// and mutation API over them. The parked set and the orphans are session
// state derived from the framework's receipt; only the advisory copy of the
// parked ids is mirrored into the model.
//
// TWO RULES FROM THE FRAMEWORK CONTRACT ARE LOAD-BEARING HERE:
//
//   * The settlement receipt is READ OFF THE WHOLE GHOST STORE, not off what
//     one replay diverted. A profile a reopen never replayed keeps whatever
//     an earlier one parked for it. So ReplaceFromReceipt REPLACES the park
//     set; merging would strand windows that the store no longer reports,
//     and an empty receipt is a meaningful answer ("nothing is parked"), not
//     a missing one.
//   * Materialize has THREE outcomes, not two. kRefusedForNow means "ask
//     again later" and the record MUST be kept, or the Space can never come
//     back; kNoSuchGhost means nothing will ever satisfy it and the record
//     MUST be dropped, or the Space is pinned on a materialization that
//     cannot succeed.
class PhiGhostSpaces {
 public:
  // What the client remembers about a Space's window between runs.
  struct Remembered {
    std::string profile;  // On-disk basename, the ghost store's key.
    // The window's SessionID in the PREVIOUS session — a browser id that
    // names nothing live until the ghost materializes under it.
    BrowserId window_id;
  };

  // `model` is the coordinator's restore model, which outlives this view.
  explicit PhiGhostSpaces(model::PhiRestoreModel* model);
  ~PhiGhostSpaces();

  PhiGhostSpaces(const PhiGhostSpaces&) = delete;
  PhiGhostSpaces& operator=(const PhiGhostSpaces&) = delete;

  // A mutation of the remembered rows does not write anything itself: it
  // calls this, and the owner (the coordinator's ScheduleSave(kRestore))
  // folds the write into its coalesced-save timer.
  using DirtyCallback = std::function<void()>;
  void set_dirty_callback(DirtyCallback callback) {
    dirty_callback_ = std::move(callback);
  }

  // A Space's browser is live: remember the pair so a later run can name it,
  // and clear any ghost record (it is not parked any more). This is the
  // Space's PRIMARY row — the window a switch materializes — and it
  // replaces whatever window the Space remembered before.
  void NoteLiveWindow(const std::string& space_id,
                      const std::string& profile,
                      BrowserId window_id);

  // SECONDARY rows (release review B6): a second live window of a Space
  // whose primary window still stands — live in another shell, parked, or
  // still on its way from the cold start. Remembered BESIDE the primary,
  // never over it, so the next run brings the window back as a shell of
  // its own instead of orphan-dropping it (or, before this existed, the
  // primary it would have overwritten). The primary is the Space's FIRST
  // row in the model; every later row of the same Space is a secondary, so
  // the file's shape is unchanged.
  void NoteSecondaryWindow(const std::string& space_id,
                           const std::string& profile,
                           BrowserId window_id);
  // The secondary window is gone for good (closed while other windows
  // stay up) or is being re-keyed under this run's id. Never touches the
  // primary row.
  void ForgetSecondaryWindow(const std::string& space_id, BrowserId window_id);
  // The Space's primary row, if it has one.
  std::optional<Remembered> PrimaryWindow(const std::string& space_id) const;
  // Whether `window_id` is one of the Space's secondary windows.
  bool HasSecondaryWindow(const std::string& space_id,
                          BrowserId window_id) const;
  // Whether the Space's primary row was written THIS run (else it still
  // names the last run's window, which may yet park or arrive).
  bool PrimaryNotedThisRun(const std::string& space_id) const {
    return noted_this_run_.count(space_id) > 0;
  }
  // Every profile a row names.
  std::set<std::string> Profiles() const;

  // What the client should ARM as the eager set: the window ids it wants
  // rebuilt immediately (everything else parks). Empty means "park all".
  // The active Space's primary window, plus every secondary window: those
  // have no Space switch to come back through, so they come back as the
  // windows they were. Plain ints: OnColdStartRestorePlan feeds them to the
  // framework's ColdStartPlan sink one by one.
  std::vector<int> EagerWindowIds(const std::string& active_space_id) const;

  // Takes the framework's settlement receipt and REPLACES the park set from
  // it. Window ids the client cannot map back to a Space are kept as
  // orphans, so they can still be dropped rather than leaking.
  void ReplaceFromReceipt(const std::string& receipt_json);

  // True while `space_id` exists only as a parked window.
  bool IsParked(const std::string& space_id) const;
  // The parked window for `space_id`, or nullptr.
  const Remembered* Parked(const std::string& space_id) const;

  // The Space stopped being a ghost: it materialized, or it was deleted.
  void ClearParked(const std::string& space_id);

  // The Space is gone (DeleteSpace): drop everything remembered about it,
  // so its old window id can neither match a materialized window nor be
  // named eager by the next cold start. Marks the model dirty. Returns the
  // record it held (window id 0 when none), so the caller can drop the
  // parked window on the framework side too.
  Remembered Forget(const std::string& space_id);

  // True when nothing has been remembered yet — the client has no basis to
  // gate a replay on, so it must not try.
  bool Empty() const { return model_->remembered.empty(); }

  // Which Space remembers `window_id` as its window, or nullptr. This is how
  // a materialized window — announced with the PREVIOUS session's id — finds
  // the Space it belongs to. `secondary` (optional) says whether the row is
  // one of the Space's secondary windows rather than its primary.
  const std::string* SpaceForWindowId(BrowserId window_id,
                                      bool* secondary = nullptr) const;

  // Every parked window that no Space claims — dropped rather than left to
  // be replayed as a loose window by the next unfiltered restore.
  std::vector<Remembered> Orphans() const;
  void ClearOrphans();

  // Parked windows a SECONDARY row claims: they hold a live Space's tabs
  // and have no switch to come back through, so they are re-hosted
  // (materialized as shells of their own), never dropped. Taken by the
  // settle pass; `space_id` narrows to one Space's (a delete).
  struct Stray {
    std::string space_id;
    Remembered window;
  };
  std::vector<Stray> TakeStrays();
  std::vector<Stray> TakeStrays(const std::string& space_id);

 private:
  // The model row for `space_id`, or nullptr.
  model::RememberedWindow* FindRemembered(const std::string& space_id);
  const model::RememberedWindow* FindRemembered(
      const std::string& space_id) const;
  // The remembered set (or the advisory parked copy) changed: schedule the
  // write.
  void MarkDirty() const;
  // Erases the Space's secondary row for `window_id` (never its primary);
  // true when one went. Does not mark dirty.
  bool EraseSecondaryRow(const std::string& space_id, BrowserId window_id);

  // The persisted rows: Space -> the window it had, whether or not it is
  // currently parked. Non-owning; a plain pointer across a client-only
  // boundary, like the controller's host pointers.
  RAW_PTR_EXCLUSION model::PhiRestoreModel* const model_;
  DirtyCallback dirty_callback_;
  // The subset that the last receipt says is parked right now.
  std::map<std::string, Remembered> parked_;
  std::vector<Remembered> orphans_;
  std::vector<Stray> strays_;
  // The Spaces whose primary row NoteLiveWindow wrote this run.
  std::set<std::string> noted_this_run_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_GHOST_SPACES_H_

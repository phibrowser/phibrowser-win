// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_SETUP_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_SETUP_WINDOW_H_

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_secondary_window.h"
#include "phibrowser-win/client/phi_sync_keys.h"

namespace phi_client {

class PhiSyncKeyService;

// Puts a SECRET (the sync recovery code; reusable for any credential) on
// the clipboard as CF_UNICODETEXT with the formats that keep it out of
// Win+V clipboard history, Cloud Clipboard and clipboard monitors
// (ExcludeClipboardContentFromMonitorProcessing, CanIncludeInClipboardHistory
// = 0, CanUploadToCloudClipboard = 0), and clears it after
// `clear_after_ms` unless something else has been copied since
// (GetClipboardSequenceNumber). The clear rides a thread timer, so it fires
// even after `owner` has closed. UI thread. False when the clipboard could
// not be opened or written.
bool PhiCopySecretToClipboard(HWND owner,
                              const std::wstring& text,
                              UINT clear_after_ms = 60 * 1000);

// "Finish setting up sync" (docs/user-data-parity.md S3 UI plan) — the
// Mac's AppModalPairingHost with KeyLayerView and the pairing wizard's
// Profiles step in one window: how this device joins the account (first
// device: a recovery code; later ones: approval from an authorized device
// or the recovery code), then which account profile each local profile is.
// Opened only on request (Settings ▸ Sync); never pops up by itself.
//
// Self-owned (one at a time): the key layer answers asynchronously, and a
// reply for a window that has gone is dropped through a weak token. Closing
// (the band's close, Escape, "Finish later") is refused while an operation
// that must not be abandoned runs and while the recovery code is shown.
class PhiSyncSetupWindow : public PhiSecondaryWindow {
 public:
  class Host {
   public:
    virtual ~Host() = default;
    // The signed-in account's key layer; null once it is gone (the window
    // then closes).
    virtual PhiSyncKeyService* SyncKeys() = 0;
    // "Create on this PC": a Chromium profile named `display_name`; the
    // answer comes back through NoteProfileCreated.
    virtual void SyncSetupCreateProfile(const std::string& display_name) = 0;
    // Whether a local profile already carries `display_name` (the Mac's
    // uniqueDisplayName pre-check).
    virtual bool SyncProfileNameExists(const std::string& display_name) = 0;
    // The window closed; the Sync pane re-reads.
    virtual void SyncSetupClosed() = 0;
  };

  enum class Page {
    kWorking,
    kIntroduction,
    kShowingCode,
    kChooseJoin,
    kWaiting,
    kEnterCode,
    // The same field, for the code this account's setup just showed: sync stays
    // off until it is typed back.
    kConfirmCode,
    kDenied,
    kExpired,
    kError,
    kProfiles,
    kSubmitting,
  };

  // Opens (or fronts) the window and starts the setup.
  static void Open(Host* host, HWND anchor);
  static PhiSyncSetupWindow* Current();
  // The account went away (sign-out, a switch): the window closes, whatever
  // it was doing.
  static void CloseForAccountChange();
  // A kSyncPairing profile creation finished (`profile_id` empty = failed).
  static void NoteProfileCreated(const std::string& display_name,
                                 const std::string& profile_id);

  Page page() const { return page_; }
  static const char* PageName(Page page);

  // The harness (agentSpace.sync.setup.*): the page's state, and a control
  // pressed by name — "continue", "saved", "copy", "requestApproval",
  // "useRecovery", "cancel", "code" (value = text), "submit",
  // "tryAnother", "tryAgain", "local" (value "<row>:<remote row>|new"),
  // "remote" (value "<row>:create|assign:<local row>|none"),
  // "continueProfiles", "finishLater". Empty on success, else an error
  // token.
  std::string StateJson() const;
  std::string Drive(const std::string& action, const std::string& value);

 protected:
  void Paint(HDC dc, const RECT& dirty) override;
  void OnHover(int hit, int previous) override;
  void OnClick(int hit, POINT client_pt) override;
  void OnEscape() override;
  void OnEnter() override;
  void OnTimerFired(UINT_PTR id) override;
  void OnDpiChanged(UINT old_dpi) override;
  void OnNcDestroy() override;
  void OnColorsChanged() override;
  LRESULT OnWindowMessage(UINT message,
                          WPARAM wparam,
                          LPARAM lparam,
                          bool* handled) override;

 private:
  enum class Control {
    kNone,
    kContinue,
    kSaved,
    kBackToCode,
    kCopy,
    kRequestApproval,
    kUseRecovery,
    kCancelJoin,
    kSubmit,
    kTryAnother,
    kTryAgain,
    kFinishLater,
    kProfilesContinue,
    kLocalPick,   // key = local row
    kRemotePick,  // key = remote row
  };
  struct Button {
    Control control = Control::kNone;
    std::wstring text;
    int role = 0;  // PhiPopupButtonRole
    bool enabled = true;
    RECT rect{};
  };
  struct PickerRow {
    Control control = Control::kNone;
    int key = 0;
    std::wstring label;
    std::wstring value;  // The pill's text; empty = read-only line below.
    std::wstring note;   // Read-only rows.
    bool fixed = false;  // The pill shows a value nobody can change.
    RECT label_rect{};
    RECT pill_rect{};
  };
  // The Mac's ProfilePairingModel choices.
  static constexpr int kRegisterNew = -1;
  enum class RemoteChoice { kUndecided, kCreate, kAssign };
  struct RemoteDecision {
    RemoteChoice choice = RemoteChoice::kUndecided;
    int local = -1;  // kAssign
  };

  explicit PhiSyncSetupWindow(Host* host);
  ~PhiSyncSetupWindow() override;
  bool OpenWindow(HWND anchor);

  // --- Flow -----------------------------------------------------------------
  void ShowPage(Page page);
  void BeginSetup();
  void StartBootstrap();
  void StartJoinRequest();
  void Poll();
  void CancelJoin();
  void ShowRecoveryEntry();
  void SubmitRecoveryCode();
  void LoadPairing();
  void ContinueProfiles();
  void CreateNextProfile();
  void ApplyDecisions();
  void ShowError(const std::wstring& message);
  void FinishLater();
  bool CanClose() const;

  // --- The Profiles model ---------------------------------------------------
  // The default-profile rule (the Mac's ProfilePairingModel, docs/sync.md
  // "Profile pairing and the default profile"): this device's Default
  // profile pairs only with the account's default profile. The account
  // default it is fixed to is authoritative; no other local is offered it
  // and it is never created as a new local; every other account default is
  // an ordinary profile in this sheet.
  void SeedSelections();
  bool LocalIsDefault(size_t local) const;
  // The first readable account default, which the local Default's row is
  // fixed to (shown, not chosen); -1 without a local Default in the sheet
  // or a readable account default.
  int FixedDefaultRemote() const;
  // The account default this sheet treats as THE default: the local
  // Default's (fixed or picked) remote when flagged, else a remote row
  // assigned to the local Default; empty when the local Default takes none.
  std::optional<std::string> AuthoritativeDefaultUuid() const;
  bool MayPair(size_t local, int remote) const;
  // The local row's LIVE choice: the fixed account default for the local
  // Default; a stored remote the rule forbids reads back as kRegisterNew.
  int LocalChoice(size_t local) const;
  bool OffersCreateLocal(int remote) const;
  bool RemoteClaimedByLocal(int remote) const;
  bool RemoteAssignmentValid(int remote) const;
  bool AllRowsDecided() const;
  std::vector<keys::PairingDecision> BuildDecisions() const;
  void PickLocal(int row);
  void PickRemote(int row);
  std::wstring RemoteName(int remote) const;
  std::string UniqueProfileName(const std::string& base) const;

  // --- Layout and paint -----------------------------------------------------
  void Layout();
  void AddButtonRow(std::vector<Button> buttons, int bottom);
  HFONT TitleFont();
  HFONT BodyFont();
  HFONT SmallFont();
  HFONT CodeFont();
  int MeasureHeight(HFONT font, const std::wstring& text, int width);
  void EnsureCodeField();
  std::wstring CodeFieldText() const;
  void CopyToClipboard(const std::wstring& text);
  std::wstring TitleText() const;
  std::wstring BodyText() const;

  RAW_PTR_EXCLUSION Host* host_;
  Page page_ = Page::kWorking;
  // Weak token for key-layer replies (the window may be gone), and the
  // generation that makes a superseded reply (a cancelled poll, a retried
  // load) land nowhere.
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
  int generation_ = 0;
  // An operation that must not be abandoned is in flight.
  bool busy_ = false;
  bool poll_in_flight_ = false;
  std::wstring input_error_;
  // A failed apply's message, shown once the reloaded Profiles page is up.
  std::wstring profiles_error_;
  std::wstring error_message_;
  // In memory only, until it is typed back or the window closes: Back on the
  // confirmation page shows it again; a reopened window has nothing to go back
  // to.
  std::wstring recovery_code_;
  bool copied_ = false;
  std::string request_id_;
  std::wstring verification_code_;
  int64_t deadline_ms_ = 0;

  keys::PairingCandidates candidates_;
  std::vector<int> local_choice_;
  std::vector<RemoteDecision> remote_choice_;
  // "Create on this PC": the decisions waiting for their profiles.
  std::vector<keys::PairingDecision> pending_decisions_;
  size_t creating_ = 0;
  std::string creating_name_;

  Timer poll_timer_;
  Timer countdown_;
  Timer copied_timer_;
  HWND code_field_ = nullptr;
  HBRUSH field_brush_ = nullptr;
  HBRUSH bg_brush_ = nullptr;

  // The laid-out page (client pixels).
  RECT title_rect_{};
  RECT body_rect_{};
  RECT back_hint_rect_{};
  RECT code_rect_{};
  RECT note_rect_{};
  RECT error_rect_{};
  RECT section_rect_{};
  std::wstring note_text_;
  std::vector<Button> buttons_;
  std::vector<PickerRow> rows_;
  int remote_header_index_ = -1;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_SETUP_WINDOW_H_

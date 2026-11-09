// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_CREDENTIAL_SERVICE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_CREDENTIAL_SERVICE_H_

#include <windows.h>

#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/values.h"
#include "client/phi_app_model.h"
#include "client/phi_credentials.h"
#include "client/phi_settings_types.h"
#include "client/phi_vault_helper.h"

namespace phi_client {

// The agent's password surface (docs/architecture-review.md §3.4, lane 1
// §1.2 cluster 5): the two credential providers, the approval prompt, the
// session-scoped grants, the denial memory and the audit trail.
//
// Invariant: the grant key is "<item scope>|<page host>" for a fill and the
// item scope for a reveal — grants are all-agents, as on Mac, but never
// wider than one page host; only a LIVE TIMED grant ever covers a request
// (there are no standing credential grants at all — a standing password
// grant plus an agent-chosen page was the exfil path, lane 5 F2); and every
// decision, allowed or not, is written to the audit.
//
// It is the only writer of the consent model (`credential_audit`); agent
// admission is a plain setting (allowAgentControl) and keeps nothing in
// this file. It marks the consent file dirty through its own callback.
class PhiCredentialService {
 public:
  // What the service needs from the app: a window to parent the modal to,
  // and who to tell when the audit trail moved.
  class Host {
   public:
    virtual ~Host() = default;
    // Any shell window to own the approval dialog, or null (the request is
    // then refused: there is no hardened prompt to ask in).
    virtual HWND CredentialPromptOwner() = 0;
    // A grant or audit row changed: the Settings pane re-reads them.
    virtual void NoteCredentialConsentChanged() = 0;
  };

  using DirtyCallback = std::function<void()>;

  PhiCredentialService(model::PhiConsentModel& consent,
                       Host& host,
                       DirtyCallback dirty);
  ~PhiCredentialService();

  PhiCredentialService(const PhiCredentialService&) = delete;
  PhiCredentialService& operator=(const PhiCredentialService&) = delete;

  // Starts the REAL provider: the out-of-process Bitwarden helper, which
  // owns the user's actual vault (and links the GPL SDK on its own side of
  // the process boundary). With no helper installed every credentials verb
  // answers not_installed. A --dev-hooks build (phi_features.h) adds two
  // doors: PHI_VAULT_HELPER replaces the helper binary (a PATH TO AN EXE,
  // never arguments, so the no-arguments spawn contract is intact), and the
  // local DPAPI dev vault stands in when no helper is installed.
  void Init(const std::wstring& user_data_dir);

  // The providers, for the credentials.* verbs' status reports.
  const PhiVaultHelper& vault_helper() const { return vault_helper_; }
  const PhiCredentialVault& credentials() const { return credentials_; }
  // The helper's vault status wire string, or empty when it is not running.
  // Reused for a couple of seconds; asked with a short timeout.
  std::string HelperStatus();
  // True when the helper is running — respawning it first when an earlier
  // call stopped it (PhiVaultHelper::Restart).
  bool HelperUp();

  // Resolves a query through whichever provider is active (helper first, dev
  // vault otherwise) to exactly ONE item. True fills `item` with the vault
  // item; false fills `out` with the wire error body (not_found / ambiguous +
  // non-secret candidates / not_ready), so both credential verbs share one
  // matching and one refusal policy.
  bool ResolveCredentialItem(const std::string& domain,
                             const std::string& id,
                             const std::string& search,
                             const std::string& username,
                             base::DictValue* item,
                             base::DictValue* out);

  // How a request was decided, for the usage metric (the Mac's
  // agent_credential_access_requested / _approved): whether a prompt was
  // shown, and for an approval how — "existing_grant", "once" or
  // "ten_minutes" (null when denied). Never the scope or the purpose.
  struct Decision {
    bool approved = false;
    bool prompted = false;
    const char* approval_type = nullptr;
  };

  // The native approve/deny prompt for a credential request. `detail` names
  // the item and purpose. Returns true when the user approves; `decision`,
  // when given, says how. Consults the timed grants and the denial memory
  // first; prompts only when neither answers. `grant_key` is
  // "<item scope>|<page host>" for a fill and the item scope for a reveal.
  // `once_only` (a cross-origin fill) offers no timed grant and consumes
  // none.
  bool PromptCredentialApproval(const std::wstring& detail,
                                model::CredentialAccessKind kind,
                                const std::string& grant_key,
                                bool once_only,
                                Decision* decision = nullptr);

  // An ambiguous query settled by the USER, never the agent: a chooser
  // over the matching items (`candidates`, the non-secret rows the
  // resolver returned), with "none" preselected and hardware-confirmed
  // like the approval prompt. kChosen sets `credential_id`; kDenied is the
  // user choosing none (counted as a denial) or prompts being paused;
  // kUnavailable means no prompt could be shown (no window, or a candidate
  // without an id to re-query by). The candidates never reach the agent.
  // `scope` names the request in the audit.
  enum class PickResult { kChosen, kDenied, kUnavailable };
  PickResult PromptCredentialPick(const std::wstring& detail,
                            const base::ListValue& candidates,
                            model::CredentialAccessKind kind,
                            const std::string& scope,
                            std::string* credential_id);

  // Appends one line to the credential audit trail (newest first, capped)
  // and persists it — every approval decision is recorded, allowed or not.
  void AppendCredentialAudit(const std::string& scope,
                             model::CredentialAccessKind kind,
                             const char* decision);
  // Ends every live timed grant at once — the Mac's
  // CredentialGrantStore.revokeAll, which Developer mode going OFF runs. The
  // next request for any credential prompts afresh.
  void RevokeAllGrants();


 private:
  // Looks a query up through the helper. Returns the reply's `result` dict,
  // or nullopt with `error` set (`ambiguous` carries a candidates list).
  std::optional<base::DictValue> HelperLookup(const std::string& domain,
                                              const std::string& id,
                                              const std::string& search,
                                              const std::string& username,
                                              std::string* error);
  // The prompt-spam brake. InDenialBackoff answers (and audits) a request
  // while prompts are paused; NoteDenial records one denial and pauses
  // prompts after kDenialBurst of them inside kDenialWindowMs, whatever
  // items they named.
  bool InDenialBackoff(const std::string& scope,
                       model::CredentialAccessKind kind);
  void NoteDenial(const std::string& scope, model::CredentialAccessKind kind);
  // True while a live TIMED grant covers `grant_key`. The kind is not
  // consulted: fill and reveal are one exposure (model::CredentialAccessKind).
  bool GrantCovers(const std::string& grant_key) const;

  // The persisted consent file's credential half. Owned by the coordinator's
  // store; this service is its only writer.
  RAW_PTR_EXCLUSION model::PhiConsentModel& consent_;
  RAW_PTR_EXCLUSION Host& host_;
  DirtyCallback dirty_;

  // The REAL provider: the out-of-process Bitwarden helper (the user's own
  // vault, same engine as macOS). `credentials_` is a local DPAPI dev/test
  // store used ONLY when no helper binary is installed.
  PhiVaultHelper vault_helper_;
  PhiCredentialVault credentials_;

  // The session-scoped grants (the Mac's in-memory 10-minute cache), so a
  // restart is always a clean slate for them.
  struct TimedGrant {
    model::CredentialAccessKind kind = model::CredentialAccessKind::kFill;
    ULONGLONG expires_tick = 0;
  };
  std::map<std::string, TimedGrant> timed_grants_;
  // Denial memory: grant key -> tick until which the request is refused
  // without a prompt (5 minutes), so a denied ask cannot re-prompt at once.
  std::map<std::string, ULONGLONG> denials_;
  // The brake's recent denial ticks and the end of the current pause.
  std::deque<ULONGLONG> recent_denials_;
  ULONGLONG backoff_until_tick_ = 0;
  // HelperStatus's last answer and when it was read.
  std::string cached_status_;
  ULONGLONG cached_status_tick_ = 0;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CREDENTIAL_SERVICE_H_

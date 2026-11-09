// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_credential_service.h"

#include <algorithm>
#include <utility>

#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "client/phi_features.h"
#include "client/phi_i18n.h"
#include "client/phi_prompt.h"

namespace phi_client {

namespace {
using CredentialAccessKind = model::CredentialAccessKind;

// The helper is asked on the UI thread, so its answers are bounded tightly:
// a status read and a lookup are local to the unlocked vault.
constexpr DWORD kHelperStatusTimeoutMs = 3000;
constexpr DWORD kHelperLookupTimeoutMs = 5000;
// How long a status answer is reused (agents poll credentials.status).
constexpr ULONGLONG kHelperStatusCacheMs = 2000;
// The prompt-spam brake: this many denials (any item) inside the window
// refuse every request without a prompt for the back-off.
constexpr size_t kDenialBurst = 3;
constexpr ULONGLONG kDenialWindowMs = 2 * 60 * 1000;
constexpr ULONGLONG kDenialBackoffMs = 5 * 60 * 1000;
}  // namespace

PhiCredentialService::PhiCredentialService(model::PhiConsentModel& consent,
                                           Host& host,
                                           DirtyCallback dirty)
    : consent_(consent), host_(host), dirty_(std::move(dirty)) {}

PhiCredentialService::~PhiCredentialService() = default;

void PhiCredentialService::Init(const std::wstring& user_data_dir) {
  std::wstring helper_override;
  if constexpr (kPhiDevHooks) {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length =
        ::GetEnvironmentVariableW(L"PHI_VAULT_HELPER", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
      helper_override.assign(buffer, length);
    }
  }
  if (!vault_helper_.Start(helper_override)) {
    if constexpr (kPhiDevHooks) {
      LOG(INFO) << "[PhiVault] no helper installed — using the local dev "
                   "vault (dev hooks build)";
      credentials_.Load(user_data_dir);
    } else {
      LOG(INFO) << "[PhiVault] no helper installed — credentials unavailable";
    }
  }
}

namespace {

// The non-secret candidate list for an ambiguous match.
base::ListValue CandidateList(
    const std::vector<const PhiCredentialItem*>& items) {
  base::ListValue out;
  for (const PhiCredentialItem* item : items) {
    base::DictValue d;
    if (!item->credential_id.empty()) {
      d.Set("credentialId", item->credential_id);
    }
    d.Set("type", item->type);
    if (!item->name.empty()) {
      d.Set("name", item->name);
    }
    if (!item->username.empty()) {
      d.Set("username", item->username);
    }
    if (!item->uri.empty()) {
      d.Set("uri", item->uri);
    }
    if (!item->domain.empty()) {
      d.Set("domain", item->domain);
    }
    out.Append(std::move(d));
  }
  return out;
}

}  // namespace

bool PhiCredentialService::HelperUp() {
  return vault_helper_.running() || vault_helper_.Restart();
}

std::string PhiCredentialService::HelperStatus() {
  if (!HelperUp()) {
    cached_status_.clear();
    return std::string();
  }
  const ULONGLONG now = ::GetTickCount64();
  if (!cached_status_.empty() &&
      now - cached_status_tick_ < kHelperStatusCacheMs) {
    return cached_status_;
  }
  std::string error;
  std::optional<base::DictValue> result = vault_helper_.Call(
      "status", base::DictValue(), &error, kHelperStatusTimeoutMs);
  const std::string* status = result ? result->FindString("status") : nullptr;
  cached_status_ = status ? *status : std::string();
  cached_status_tick_ = now;
  return cached_status_;
}

std::optional<base::DictValue> PhiCredentialService::HelperLookup(
    const std::string& domain,
    const std::string& id,
    const std::string& search,
    const std::string& username,
    std::string* error) {
  base::DictValue query;
  if (!domain.empty()) {
    query.Set("domain", domain);
    if (!username.empty()) {
      query.Set("username", username);
    }
  } else if (!id.empty()) {
    query.Set("id", id);
  } else {
    query.Set("search", search);
  }
  base::DictValue params;
  params.Set("query", std::move(query));
  return vault_helper_.Call("lookup", params, error, kHelperLookupTimeoutMs);
}

bool PhiCredentialService::ResolveCredentialItem(const std::string& domain,
                                                 const std::string& id,
                                                 const std::string& search,
                                                 const std::string& username,
                                                 base::DictValue* item,
                                                 base::DictValue* out) {
  if (HelperUp()) {
    std::string error;
    std::optional<base::DictValue> result =
        HelperLookup(domain, id, search, username, &error);
    if (!result) {
      // The helper refused or died. Never silently fall through to the dev
      // vault here: that would answer a real-vault question from a different
      // store.
      out->Set("ok", false);
      out->Set("error",
               error == "helper_unavailable" ? "not_ready" : "provider_error");
      if (error == "helper_unavailable") {
        out->Set("status", "unavailable");
      }
      return false;
    }
    if (result->FindBool("ambiguous").value_or(false)) {
      out->Set("ok", false);
      out->Set("error", "ambiguous");
      out->Set("matches", result->FindInt("matches").value_or(0));
      if (base::ListValue* candidates = result->FindList("candidates")) {
        out->Set("candidates", candidates->Clone());
      }
      return false;
    }
    base::DictValue* hit = result->FindBool("found").value_or(false)
                               ? result->FindDict("item")
                               : nullptr;
    if (!hit) {
      out->Set("ok", false);
      out->Set("error", "not_found");
      return false;
    }
    *item = hit->Clone();
    return true;
  }

  if constexpr (!kPhiDevHooks) {
    // No helper, no fallback: the dev store is a --dev-hooks door only.
    out->Set("ok", false);
    out->Set("error", "not_installed");
    return false;
  }
  // Dev/test fallback: the local DPAPI store.
  if (!credentials_.present()) {
    out->Set("ok", false);
    out->Set("error", "not_ready");
    out->Set("status", "logged_out");
    return false;
  }
  std::vector<const PhiCredentialItem*> matches =
      credentials_.Match(domain, id, search, username);
  if (matches.empty()) {
    out->Set("ok", false);
    out->Set("error", "not_found");
    return false;
  }
  if (matches.size() > 1) {
    out->Set("ok", false);
    out->Set("error", "ambiguous");
    out->Set("matches", static_cast<int>(matches.size()));
    out->Set("candidates", CandidateList(matches));
    return false;
  }
  const PhiCredentialItem* found = matches.front();
  item->Set("type", found->type);
  item->Set("name", found->name);
  if (!found->credential_id.empty()) {
    item->Set("credentialId", found->credential_id);
  }
  if (!found->username.empty()) {
    item->Set("username", found->username);
  }
  if (!found->password.empty()) {
    item->Set("password", found->password);
  }
  if (!found->uri.empty()) {
    item->Set("uri", found->uri);
  }
  if (!found->domain.empty()) {
    item->Set("domain", found->domain);
  }
  if (!found->notes.empty()) {
    item->Set("notes", found->notes);
  }
  return true;
}

void PhiCredentialService::RevokeAllGrants() {
  if (!timed_grants_.empty()) {
    LOG(INFO) << "[PhiCredentials] " << timed_grants_.size()
              << " credential grant(s) revoked";
  }
  timed_grants_.clear();
}

bool PhiCredentialService::GrantCovers(const std::string& grant_key) const {
  // Only a live TIMED grant counts. The persisted "always" rows are display
  // only (LoadCredentialGrants), and the kind is not consulted: a fill is a
  // reveal (CredentialAccessKind), so one exposure, one grant.
  auto timed = timed_grants_.find(grant_key);
  return timed != timed_grants_.end() &&
         ::GetTickCount64() < timed->second.expires_tick;
}

bool PhiCredentialService::PromptCredentialApproval(
    const std::wstring& detail,
    CredentialAccessKind kind,
    const std::string& grant_key,
    bool once_only,
    Decision* decision) {
  Decision local;
  Decision& out = decision ? *decision : local;
  out = Decision();
  const bool reveal = kind == CredentialAccessKind::kReveal;
  const ULONGLONG now = ::GetTickCount64();
  // Denial memory first: a refused ask stays refused for five minutes, so
  // an agent cannot re-raise the same dialog until the user gives in.
  auto denied = denials_.find(grant_key);
  if (denied != denials_.end()) {
    if (now < denied->second) {
      AppendCredentialAudit(grant_key, kind, "denied (remembered)");
      return false;
    }
    denials_.erase(denied);
  }
  if (!once_only && GrantCovers(grant_key)) {
    AppendCredentialAudit(grant_key, kind, "allowed (10-minute grant)");
    out.approved = true;
    out.approval_type = "existing_grant";
    return true;
  }
  if (InDenialBackoff(grant_key, kind)) {
    return false;
  }
  HWND owner = host_.CredentialPromptOwner();
  if (!owner) {
    // No Phi window to ask in. The old MessageBox fallback's Yes answered
    // BM_CLICK from any program, so nothing is released without the
    // hardened prompt below; not remembered as the user's denial.
    AppendCredentialAudit(grant_key, kind, "denied (no window to ask in)");
    return false;
  }
  out.prompted = true;
  // The exposure banner is the load-bearing line, as on Mac — and it says
  // what is TRUE: a filled page is a page the agent holds a CDP session on.
  const std::wstring exposure =
      reveal ? L"The secret will be GIVEN TO THE AGENT."
             : L"Phi types the secret into the page. Any automation attached "
               L"to that page can read it.";
  const std::wstring text = detail + L"\n\n" + exposure;
  // No "Always allow": a standing grant plus an agent-chosen page was the
  // exfil path. A once-only ask (cross-origin) offers no timed grant either.
  std::vector<std::wstring> options = {L"Allow once"};
  if (!once_only) {
    options.push_back(L"Allow for 10 minutes");
  }
  options.push_back(L"Deny");
  const int deny_index = static_cast<int>(options.size()) - 1;
  const int timed_index = once_only ? -1 : 1;
  // Deny preselected AND Cancel holding Return: Enter refuses; only a
  // deliberate click on Continue (or a row double-click) approves — and
  // only a real one: another program's BM_CLICK, UI Automation or
  // SendInput cannot press Continue or move the selection.
  const int chosen = PhiPromptChoose(owner, L"Credential request",
                                     text.c_str(), options, deny_index,
                                     L"Continue", L"Cancel",
                                     /*cancel_is_default=*/true,
                                     /*require_hardware_input=*/true);
  if (chosen == timed_index) {
    TimedGrant grant;
    grant.kind = kind;
    grant.expires_tick = now + 10 * 60 * 1000;
    timed_grants_[grant_key] = grant;
    AppendCredentialAudit(grant_key, kind, "allowed (10 minutes)");
    out.approved = true;
    out.approval_type = "ten_minutes";
    return true;
  }
  if (chosen == 0) {
    AppendCredentialAudit(
        grant_key, kind,
        once_only ? "allowed (once, cross-origin)" : "allowed (once)");
    out.approved = true;
    out.approval_type = "once";
    return true;
  }
  denials_[grant_key] = now + 5 * 60 * 1000;
  AppendCredentialAudit(grant_key, kind, "denied");
  NoteDenial(grant_key, kind);
  return false;
}

bool PhiCredentialService::InDenialBackoff(const std::string& scope,
                                           CredentialAccessKind kind) {
  if (::GetTickCount64() >= backoff_until_tick_) {
    return false;
  }
  AppendCredentialAudit(scope, kind, "denied (prompts paused)");
  return true;
}

void PhiCredentialService::NoteDenial(const std::string& scope,
                                      CredentialAccessKind kind) {
  // Keyed on nothing: an agent that walks through item after item after
  // the user said no is the spam this stops.
  const ULONGLONG now = ::GetTickCount64();
  recent_denials_.push_back(now);
  while (!recent_denials_.empty() &&
         now - recent_denials_.front() > kDenialWindowMs) {
    recent_denials_.pop_front();
  }
  if (recent_denials_.size() >= kDenialBurst) {
    recent_denials_.clear();
    backoff_until_tick_ = now + kDenialBackoffMs;
    AppendCredentialAudit(scope, kind,
                          "prompts paused for 5 minutes (repeated denials)");
  }
}

PhiCredentialService::PickResult PhiCredentialService::PromptCredentialPick(
    const std::wstring& detail,
    const base::ListValue& candidates,
    CredentialAccessKind kind,
    const std::string& scope,
    std::string* credential_id) {
  credential_id->clear();
  // Every row must carry an id to re-query by; otherwise the user's pick
  // could not be honoured exactly, and nothing is offered.
  std::vector<std::string> ids;
  std::vector<std::wstring> options;
  for (const base::Value& entry : candidates) {
    const base::DictValue* d = entry.GetIfDict();
    const std::string* id = d ? d->FindString("credentialId") : nullptr;
    if (!id || id->empty()) {
      return PickResult::kUnavailable;
    }
    const std::string* name = d->FindString("name");
    const std::string* username = d->FindString("username");
    const std::string* uri = d->FindString("uri");
    std::string label = name && !name->empty() ? *name
                        : uri               ? *uri
                                            : *id;
    if (username && !username->empty()) {
      label += "  (" + *username + ")";
    }
    ids.push_back(*id);
    options.push_back(base::UTF8ToWide(label));
  }
  if (ids.empty()) {
    return PickResult::kUnavailable;
  }
  if (InDenialBackoff(scope, kind)) {
    return PickResult::kDenied;
  }
  HWND owner = host_.CredentialPromptOwner();
  if (!owner) {
    return PickResult::kUnavailable;
  }
  options.push_back(PhiStr("agent.credentialPick.noneRow"));
  const int none_index = static_cast<int>(options.size()) - 1;
  const std::wstring text =
      detail + L"\n\n" + PhiStr("agent.credentialPick.message");
  const int chosen = PhiPromptChoose(
      owner, PhiStr("agent.credentialPick.title"), text.c_str(), options,
      none_index, PhiStr("agent.credentialPick.continueButton"),
      PhiStr("prompt.cancel"), /*cancel_is_default=*/true,
      /*require_hardware_input=*/true);
  if (chosen < 0 || chosen >= none_index) {
    AppendCredentialAudit(scope, kind, "denied (no item chosen)");
    NoteDenial(scope, kind);
    return PickResult::kDenied;
  }
  *credential_id = ids[static_cast<size_t>(chosen)];
  return PickResult::kChosen;
}

void PhiCredentialService::AppendCredentialAudit(const std::string& scope,
                                                 CredentialAccessKind kind,
                                                 const char* decision) {
  SYSTEMTIME now = {};
  ::GetLocalTime(&now);
  char stamp[32] = {};
  ::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d", now.wYear,
             now.wMonth, now.wDay, now.wHour, now.wMinute);
  consent_.credential_audit.insert(
      consent_.credential_audit.begin(),
      PhiCredentialAuditWireRow({stamp, kind, scope, decision}));
  constexpr size_t kMaxAudit = 50;
  if (consent_.credential_audit.size() > kMaxAudit) {
    consent_.credential_audit.resize(kMaxAudit);
  }
  dirty_();
  host_.NoteCredentialConsentChanged();
}

}  // namespace phi_client

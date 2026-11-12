// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include <windows.h>

#include <string>
#include <utility>
#include <vector>

#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "client/phi_client_analytics.h"
#include "client/phi_control_dispatcher.h"
#include "client/phi_coordinator.h"
#include "client/phi_credentials.h"
#include "client/phi_features.h"
#include "client/phi_i18n.h"

// The vault verbs. Both prompting ones are marked `prompts` in the registry,
// so the dispatcher serialises them behind its prompt mutex before either
// body runs — two approval dialogs never stack.

namespace phi_client {

namespace {

// Pulls a credential query out of a payload: either a nested `query`
// {domain|id|search|username} or a top-level `domain`. Precedence
// domain -> id -> search matches the Mac parser.
struct CredentialQuery {
  std::string domain;
  std::string id;
  std::string search;
  std::string username;
  bool valid = false;
};

CredentialQuery ParseCredentialQuery(const PhiParams& params) {
  CredentialQuery q;
  const base::DictValue* query = params.FindDict("query");
  auto pick = [&](const char* key) -> std::string {
    if (query) {
      if (const std::string* s = query->FindString(key)) {
        return *s;
      }
    }
    return std::string();
  };
  q.domain = pick("domain");
  q.id = pick("id");
  q.search = pick("search");
  q.username = pick("username");
  if (q.domain.empty() && !query) {
    q.domain = params.String("domain");
  }
  q.valid = !q.domain.empty() || !q.id.empty() || !q.search.empty();
  return q;
}

// The field set a resolved item exposes, read by name.
std::string FieldOf(const base::DictValue& item, const std::string& key) {
  const std::string* value = item.FindString(key);
  return value ? *value : std::string();
}

// Resolves `q` to ONE item for an agent. An ambiguous match is put to the
// user (PromptCredentialPick) and re-resolved by the id they chose; the
// agent only ever learns how many items matched — which accounts exist is
// not its to see before anyone approved anything. False with `*refusal` set.
bool ResolveForAgent(PhiCoordinator* coordinator,
                     const CredentialQuery& q,
                     const std::wstring& detail,
                     model::CredentialAccessKind kind,
                     base::DictValue* item,
                     PhiVerbResult* refusal) {
  base::DictValue body;
  if (coordinator->ResolveCredentialItem(q.domain, q.id, q.search, q.username,
                                         item, &body)) {
    return true;
  }
  const std::string* error = body.FindString("error");
  if (!error || *error != "ambiguous") {
    // `body` already carries the wire error the resolver chose.
    *refusal = PhiVerbResult::FromLegacyDict(std::move(body));
    return false;
  }
  const int matches = body.FindInt("matches").value_or(0);
  const std::string scope = !q.domain.empty()   ? q.domain
                            : !q.search.empty() ? q.search
                                                : q.id;
  std::string chosen;
  const base::ListValue* candidates = body.FindList("candidates");
  const PhiCredentialService::PickResult pick =
      candidates ? coordinator->credential_service().PromptCredentialPick(
                       detail, *candidates, kind, scope, &chosen)
                 : PhiCredentialService::PickResult::kUnavailable;
  if (pick == PhiCredentialService::PickResult::kChosen) {
    base::DictValue again;
    if (coordinator->ResolveCredentialItem(std::string(), chosen,
                                           std::string(), std::string(), item,
                                           &again)) {
      return true;
    }
    *refusal = PhiVerbResult::FromLegacyDict(std::move(again));
    return false;
  }
  if (pick == PhiCredentialService::PickResult::kDenied) {
    *refusal = PhiVerbResult::Fail(PhiErrorCode::kUserDenied);
    return false;
  }
  base::DictValue count_only;
  count_only.Set("ok", false);
  count_only.Set("error", "ambiguous");
  count_only.Set("matches", matches);
  *refusal = PhiVerbResult::FromLegacyDict(std::move(count_only));
  return false;
}

}  // namespace

void PhiControlDispatcher::CaptureCredentialDecision(
    const PhiVerbContext& ctx,
    const char* kind,
    const PhiCredentialService::Decision& decision) {
  const std::string agent_name = AgentNameFor(ctx.principal);
  coordinator_->CaptureAnalytics(
      kAnalyticsModuleAgent, kEventAgentCredentialRequested,
      PhiCredentialRequestedProperties(kind, decision.approved,
                                       decision.prompted, agent_name));
  if (decision.approved && decision.approval_type) {
    coordinator_->CaptureAnalytics(
        kAnalyticsModuleAgent, kEventAgentCredentialApproved,
        PhiCredentialApprovedProperties(kind, decision.approval_type,
                                        agent_name));
  }
}

PhiVerbResult PhiControlDispatcher::VerbCredentialsStatus(
    const PhiVerbContext& ctx) {
  base::DictValue out;
  if constexpr (!kPhiDevHooks) {
    // A release build knows one provider, the vault helper. Without it
    // there is nothing to ask — not the dev store, which only a --dev-hooks
    // build ever loads.
    if (!coordinator_->credential_service().HelperUp()) {
      out.Set("status", "not_installed");
      out.Set("ready", false);
      return PhiVerbResult::Ok(std::move(out));
    }
  }
  // The helper (the user's real vault) answers when it is running; the dev
  // store only speaks for itself.
  const std::string helper_status = coordinator_->HelperStatus();
  if (!helper_status.empty()) {
    // Helper wire strings -> the skill's status vocabulary.
    std::string wire = "unavailable";
    if (helper_status == "unlocked") {
      wire = "ready";
    } else if (helper_status == "locked") {
      wire = "locked";
    } else if (helper_status == "loggedOut" ||
               helper_status == "notConfigured") {
      wire = "logged_out";
    } else if (helper_status == "notInstalled") {
      wire = "not_installed";
    }
    out.Set("status", wire);
    out.Set("ready", wire == "ready");
    return PhiVerbResult::Ok(std::move(out));
  }
  if constexpr (!kPhiDevHooks) {
    // The helper started but is not answering: unavailable, not the dev
    // store's "logged_out".
    out.Set("status", "unavailable");
    out.Set("ready", false);
    return PhiVerbResult::Ok(std::move(out));
  }
  out.Set("status",
          coordinator_->credentials().present() ? "ready" : "logged_out");
  out.Set("ready", coordinator_->credentials().ready());
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbCredentialsGetTotp(
    const PhiVerbContext& ctx) {
  if constexpr (!kPhiDevHooks) {
    // No provider, nothing to refuse about: the same answer every other
    // vault verb gives on a build with no helper.
    if (!coordinator_->credential_service().HelperUp()) {
      return PhiVerbResult::Fail("not_installed");
    }
  }
  // A TOTP code is a SECOND FACTOR handed to the agent, which makes the
  // second factor worth nothing against an agent that already holds the
  // first. Withdrawn from the agent-callable set (docs/architecture-
  // review.md §3.7 item 5); the helper's getTotp stays unused.
  return PhiVerbResult::Fail(PhiErrorCode::kTotpNotSupported);
}

PhiVerbResult PhiControlDispatcher::VerbCredentialsGet(
    const PhiVerbContext& ctx) {
  if constexpr (!kPhiDevHooks) {
    if (!coordinator_->credential_service().HelperUp()) {
      return PhiVerbResult::Fail("not_installed");
    }
  }
  // A fill request is never satisfied by a reveal.
  if (ctx.params.String("mode") == "fill") {
    return PhiVerbResult::Fail("fill_requires_autofill");
  }
  const CredentialQuery q = ParseCredentialQuery(ctx.params);
  if (!q.valid) {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload);
  }
  const std::string purpose = ctx.params.String("purpose");
  std::wstring ask = L"An agent is requesting a credential from your Phi vault.";
  if (!purpose.empty()) {
    ask += L"\n" + base::UTF8ToWide(purpose);
  }
  base::DictValue item;
  PhiVerbResult refusal = PhiVerbResult::Fail(PhiErrorCode::kInternal);
  if (!ResolveForAgent(coordinator_, q, ask, CredentialAccessKind::kReveal,
                       &item, &refusal)) {
    return refusal;
  }
  const std::string item_type =
      FieldOf(item, "type").empty() ? "login" : FieldOf(item, "type");
  const std::string scope = FieldOf(item, "name").empty()
                                ? FieldOf(item, "domain")
                                : FieldOf(item, "name");
  const std::string username = FieldOf(item, "username");
  // The requested (or type-default) field set, decided BEFORE the prompt so
  // the user sees exactly what would leave the vault.
  std::vector<std::string> fields = ctx.params.StringList("fields");
  if (fields.empty()) {
    if (item_type == "note") {
      fields = {"notes"};
    } else {
      fields = {"username", "password", "uri", "domain"};
    }
  }
  std::string field_list;
  for (const std::string& field : fields) {
    if (field != "totp") {  // Never served (below), so never named.
      field_list += (field_list.empty() ? "" : ", ") + field;
    }
  }
  std::wstring detail = base::UTF8ToWide(
      "An agent is requesting a credential from your Phi vault:\n\n" + scope +
      (username.empty() ? "" : "  (" + username + ")"));
  detail += L"\n" + PhiStrF("agent.credentialPrompt.typeLine",
                             {base::UTF8ToWide(item_type)});
  detail += L"\n" + PhiStrF("agent.credentialPrompt.fieldsLine",
                             {base::UTF8ToWide(field_list)});
  if (!purpose.empty()) {
    detail += L"\n" + base::UTF8ToWide(purpose);
  }
  // A read hands the secret TO the agent — the reveal exposure. No page
  // is involved, so the grant key is the item scope alone; it never
  // covers a fill (those are keyed by page host).
  PhiCredentialService::Decision decision;
  const bool allowed = coordinator_->PromptCredentialApproval(
      detail, CredentialAccessKind::kReveal, scope,
      /*once_only=*/false, &decision);
  CaptureCredentialDecision(ctx, "reveal", decision);
  if (!allowed) {
    return PhiVerbResult::Fail(PhiErrorCode::kUserDenied);
  }
  // Build the credential by the field set the prompt named.
  base::DictValue cred;
  cred.Set("type", item_type);
  cred.Set("name", FieldOf(item, "name"));
  for (const std::string& field : fields) {
    // Only the asked-for (or type-default) fields cross to the agent — and
    // never a TOTP seed: the helper does not encode one at all.
    const std::string value = FieldOf(item, field);
    if (!value.empty() && field != "totp") {
      cred.Set(field, value);
    }
  }
  base::DictValue out;
  out.Set("credential", std::move(cred));
  out.Set("matches", 1);
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbCredentialsAutofill(
    const PhiVerbContext& ctx) {
  if constexpr (!kPhiDevHooks) {
    if (!coordinator_->credential_service().HelperUp()) {
      return PhiVerbResult::Fail("not_installed");
    }
  }
  const std::string token = ctx.params.String("token");
  const std::string target_id = ctx.params.String("targetId");
  const std::string field = ctx.params.String("field", "password");
  if (field != "password" && field != "username") {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload);
  }
  const CredentialQuery q = ParseCredentialQuery(ctx.params);
  if (!q.valid) {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload);
  }
  if (!framework_) {
    return PhiVerbResult::Fail("autofill_not_available");
  }
  // Resolve the page host up front for the origin binding.
  const std::string page_url =
      framework_->TargetUrlForCdpTarget(target_id.c_str());
  const std::string page_host = PhiCredentialVault::HostOfUri(page_url);
  if (page_host.empty()) {
    return PhiVerbResult::Fail("page_not_found");
  }
  base::DictValue item;
  PhiVerbResult refusal = PhiVerbResult::Fail(PhiErrorCode::kInternal);
  if (!ResolveForAgent(
          coordinator_, q,
          base::UTF8ToWide("An agent is requesting a credential fill (" +
                           field + ") on " + page_host + "."),
          CredentialAccessKind::kFill, &item, &refusal)) {
    return refusal;
  }
  const std::string item_type =
      FieldOf(item, "type").empty() ? "login" : FieldOf(item, "type");
  if (item_type != "login") {
    base::DictValue details;
    details.Set("type", item_type);
    return PhiVerbResult::Fail(PhiErrorCodeForReason("not_a_login"),
                               "not_a_login", std::move(details));
  }
  const std::string value = field == "username" ? FieldOf(item, "username")
                                                : FieldOf(item, "password");
  if (value.empty()) {
    base::DictValue details;
    details.Set("field", field);
    return PhiVerbResult::Fail(PhiErrorCodeForReason("field_unavailable"),
                               "field_unavailable", std::move(details));
  }
  // Origin binding: the page must belong to the credential's site unless
  // the caller explicitly allowed a cross-origin fill.
  const std::string cred_host =
      FieldOf(item, "domain").empty()
          ? PhiCredentialVault::HostOfUri(FieldOf(item, "uri"))
          : FieldOf(item, "domain");
  const bool cross_origin =
      !PhiCredentialVault::HostMatches(page_host, cred_host);
  if (!ctx.params.Bool("allowCrossOrigin", false) && cross_origin) {
    base::DictValue details;
    details.Set("pageHost", page_host);
    return PhiVerbResult::Fail(PhiErrorCode::kOriginMismatch,
                               "origin_mismatch", std::move(details));
  }
  const std::string scope =
      FieldOf(item, "name").empty() ? cred_host : FieldOf(item, "name");
  // A cross-origin fill (the item's site is not the page's) ALWAYS
  // prompts, says so in capitals, and is approved once only — never
  // through a timed grant, never into one.
  std::string detail_utf8 = "An agent is requesting a credential fill:\n\n" +
                            scope + "  → " + field + " on " + page_host + "\n" +
                            base::WideToUTF8(PhiStrF(
                                "agent.credentialPrompt.typeLine",
                                {base::UTF8ToWide(item_type)})) +
                            "\n" +
                            base::WideToUTF8(PhiStrF(
                                "agent.credentialPrompt.fieldsLine",
                                {base::UTF8ToWide(field)}));
  if (cross_origin) {
    detail_utf8 += "\n\nCROSS-ORIGIN: " + cred_host + " → " + page_host;
  }
  // Grants are per page host: an approval for site A's password on host A
  // never carries to a page the agent navigated to afterwards.
  const std::string grant_key = scope + "|" + page_host;
  PhiCredentialService::Decision decision;
  const bool allowed = coordinator_->PromptCredentialApproval(
      base::UTF8ToWide(detail_utf8), CredentialAccessKind::kFill, grant_key,
      /*once_only=*/cross_origin, &decision);
  CaptureCredentialDecision(ctx, "fill", decision);
  if (!allowed) {
    return PhiVerbResult::Fail(PhiErrorCode::kUserDenied);
  }
  // The dialog sat open for a while; the page may have navigated under
  // it. Fill only the host the user approved.
  const std::string host_now = PhiCredentialVault::HostOfUri(
      framework_->TargetUrlForCdpTarget(target_id.c_str()));
  if (host_now != page_host) {
    coordinator_->AppendCredentialAudit(grant_key, CredentialAccessKind::kFill,
                                        "refused (page moved after approval)");
    base::DictValue details;
    details.Set("pageHost", host_now);
    return PhiVerbResult::Fail(PhiErrorCode::kOriginChanged, "origin_changed",
                               std::move(details));
  }
  // The secret goes client vault -> framework -> page DOM. FillTaggedField
  // returns false only when the tab is gone.
  if (!framework_->FillTaggedField(target_id.c_str(), token.c_str(),
                                   field.c_str(), value.c_str())) {
    return PhiVerbResult::Fail("target_not_found");
  }
  base::DictValue out;
  out.Set("filled", true);
  out.Set("field", field);
  out.Set("matches", 1);
  return PhiVerbResult::Ok(std::move(out));
}

}  // namespace phi_client

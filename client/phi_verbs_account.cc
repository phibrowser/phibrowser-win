// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The agentSpace.account.* verbs (docs/login-port.md): the harness's view
// of the Auth0 session and the three things Settings ▸ You can do to it.
// Test rig only — the session is the user's, never an agent's to drive.

#include <algorithm>
#include <array>
#include <optional>
#include <string>

#include "base/json/json_reader.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_account_feedback.h"
#include "phibrowser-win/client/phi_account_profile.h"
#include "phibrowser-win/client/phi_auth.h"
#include "phibrowser-win/client/phi_chromium_sync.h"
#include "phibrowser-win/client/phi_sync_key_service.h"
#include "phibrowser-win/client/phi_import_window.h"
#include "phibrowser-win/client/phi_sync_setup_window.h"
#include "phibrowser-win/client/phi_control_dispatcher.h"
#include "phibrowser-win/client/phi_coordinator.h"
#include "phibrowser-win/client/phi_onboarding_window.h"
#include "phibrowser-win/client/phi_window_registry.h"

namespace phi_client {

PhiVerbResult PhiControlDispatcher::VerbAccountStatus(
    const PhiVerbContext& ctx) {
  const PhiAccountAndFeedback& account = coordinator_->account();
  const PhiAuth& auth = account.auth();
  base::DictValue out;
  out.Set("signedIn", auth.signed_in());
  out.Set("signingIn", auth.signing_in());
  out.Set("reauthRequired", account.AccountReauthRequired());
  out.Set("busy", account.AccountBusy());
  out.Set("lastError", account.AccountLastError());
  out.Set("tenantOrigin", auth.TenantOrigin());
  if (auth.signed_in()) {
    base::DictValue user;
    user.Set("sub", auth.user().sub);
    user.Set("name", auth.user().name);
    user.Set("email", auth.user().email);
    user.Set("picture", auth.user().picture);
    out.Set("user", std::move(user));
    std::string name;
    std::string email;
    account.AccountInfo(&name, &email);
    out.Set("displayName", name);
    out.Set("displayEmail", email);
  }
  const std::optional<PhiAccountProfile>& profile = account.Profile().profile();
  if (profile) {
    base::DictValue row;
    row.Set("id", profile->id);
    row.Set("auth0Id", profile->auth0_id);
    row.Set("name", profile->name);
    row.Set("email", profile->email);
    row.Set("picture", profile->picture);
    out.Set("profile", std::move(row));
  }
  out.Set("hasAvatar", account.Profile().has_avatar());
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbAccountSignIn(
    const PhiVerbContext& ctx) {
  // The hosted login rides in the onboarding window's off-the-record
  // browser, which needs no user window.
  coordinator_->account().RequestSignIn();
  base::DictValue out;
  out.Set("signingIn", coordinator_->auth().signing_in());
  out.Set("tenantOrigin", coordinator_->auth().TenantOrigin());
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbAccountCancelSignIn(
    const PhiVerbContext& ctx) {
  coordinator_->account().RequestCancelSignIn();
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbAccountSignOut(
    const PhiVerbContext& ctx) {
  coordinator_->RequestSignOut();  // Re-arms the onboarding gate too.
  base::DictValue out;
  out.Set("signedIn", coordinator_->auth().signed_in());
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbOnboardingStatus(
    const PhiVerbContext& ctx) {
  // The gate and the onboarding window, for the box: which page is up
  // and what it holds.
  base::DictValue out;
  const PhiCoordinator::Access access = coordinator_->access();
  out.Set("access", access == PhiCoordinator::Access::kLoginRequired
                        ? "loginRequired"
                    : access == PhiCoordinator::Access::kGuest ? "guest"
                                                               : "signedIn");
  out.Set("open", coordinator_->onboarding() != nullptr);
  if (PhiOnboardingWindow* window = coordinator_->onboarding()) {
    if (std::optional<base::Value> state = base::JSONReader::Read(
            window->StateJson(), base::JSON_PARSE_RFC)) {
      if (base::DictValue* dict = state->GetIfDict()) {
        out.Merge(std::move(*dict));
      }
    }
    out.Set("hwnd", static_cast<double>(
                        reinterpret_cast<uintptr_t>(window->hwnd())));
  }
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbOnboardingDrive(
    const PhiVerbContext& ctx) {
  PhiOnboardingWindow* window = coordinator_->onboarding();
  if (!window) {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload, "not_open");
  }
  const std::string error =
      window->Drive(ctx.params.String("action"), ctx.params.String("value"));
  if (!error.empty()) {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload, error);
  }
  return VerbOnboardingStatus(ctx);
}

// The token itself never leaves: its presence and lifetime are what a rig
// needs to know.
PhiVerbResult PhiControlDispatcher::VerbSyncStatus(const PhiVerbContext& ctx) {
  PhiChromiumSync& sync = coordinator_->chromium_sync();
  base::DictValue out;
  std::string subject;
  std::string email;
  switch (sync.Account(&subject, &email)) {
    case PhiChromiumSync::AccountState::kUnresolved:
      out.Set("account", "unresolved");
      break;
    case PhiChromiumSync::AccountState::kSignedIn:
      out.Set("account", "signedIn");
      out.Set("subject", subject);
      out.Set("email", email);
      break;
    case PhiChromiumSync::AccountState::kSignedOut:
      out.Set("account", "signedOut");
      break;
  }
  out.Set("serverUrl", PhiAuth::SyncServerUrl());
  out.Set("hasToken", !sync.AccessToken().empty());
  base::ListValue keys;
  for (const auto& [profile_id, key] : sync.keys()) {
    base::DictValue row;
    row.Set("profileId", profile_id);
    row.Set("uuid", key.uuid);
    keys.Append(std::move(row));
  }
  out.Set("profileKeys", std::move(keys));
  if (PhiSyncKeyService* layer = coordinator_->sync_keys()) {
    const PhiSyncKeyService::Snapshot& snap = layer->snapshot();
    static constexpr std::array<const char*, 6> kUnlock = {
        "unknown", "working", "unlocked", "needsJoin", "notSignedIn", "failed"};
    base::DictValue key_layer;
    key_layer.Set("unlock", kUnlock[static_cast<int>(snap.unlock)]);
    key_layer.Set("paired", snap.paired);
    key_layer.Set("needsPairing", snap.needs_pairing);
    key_layer.Set("needsPairingActionable", snap.needs_pairing_actionable);
    key_layer.Set("deviceKeyId", snap.device_key_id);
    key_layer.Set("exposedProfiles", static_cast<int>(snap.exposed.size()));
    key_layer.Set("lastError", snap.last_error);
    out.Set("keyLayer", std::move(key_layer));
    base::DictValue native;
    native.Set("active", snap.native_sync_active);
    native.Set("outcome", snap.native_sync_outcome);
    native.Set("pages", snap.native_sync_pages);
    native.Set("needsReconfiguration", snap.native_sync_needs_reconfiguration);
    native.Set("streamHealthy", snap.invalidation_stream_healthy);
    native.Set("phase", sync::SyncPhaseName(snap.native_sync_status.phase));
    if (snap.native_sync_status.last_success_ms) {
      native.Set("lastSuccessMs",
                 static_cast<double>(*snap.native_sync_status.last_success_ms));
    }
    base::DictValue spaces;
    spaces.Set("enabled", snap.native_sync_spaces_enabled);
    spaces.Set("pulled", snap.native_sync_spaces.pulled);
    spaces.Set("applied", snap.native_sync_spaces.applied);
    spaces.Set("refused", snap.native_sync_spaces.refused);
    spaces.Set("parked", snap.native_sync_spaces.parked);
    spaces.Set("pushed", snap.native_sync_spaces.pushed);
    spaces.Set("conflicts", snap.native_sync_spaces.conflicts);
    spaces.Set("tombstones", snap.native_sync_spaces.tombstones);
    native.Set("spaces", std::move(spaces));
    for (const auto& [label, c] : snap.native_sync_owned) {
      base::DictValue owned;
      owned.Set("pulled", c.pulled);
      owned.Set("applied", c.applied);
      owned.Set("tombstones", c.tombstones);
      owned.Set("pushed", c.pushed);
      owned.Set("refused", c.refused);
      owned.Set("adopted", c.adopted);
      owned.Set("unreadable", c.unreadable);
      owned.Set("pendingPublish", c.pending_publish);
      owned.Set("excludedUnmappedOwner", c.excluded_unmapped_owner);
      owned.Set("localReadFailed", c.local_read_failed);
      native.Set(label, std::move(owned));
    }
    out.Set("nativeSync", std::move(native));
  }
  if (key_op_seq_ > 0) {
    out.Set("lastOp", key_op_.Clone());
  }
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSyncSetProfileKey(
    const PhiVerbContext& ctx) {
  PhiChromiumSync::ProfileKey key;
  key.uuid = ctx.params.String("uuid");
  key.passphrase = ctx.params.String("passphrase");
  if (!key.passphrase.empty() &&
      (key.passphrase.size() != 64 ||
       key.passphrase.find_first_not_of("0123456789abcdef") !=
           std::string::npos)) {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload, "passphrase");
  }
  coordinator_->chromium_sync().SetProfileKey(ctx.params.String("profileId"),
                                               key);
  return VerbSyncStatus(ctx);
}

void PhiControlDispatcher::FinishKeyOp(int seq,
                                       keys::KeyStatus status,
                                       base::DictValue result) {
  if (seq != key_op_seq_) {
    return;  // a later operation replaced this one
  }
  key_op_.Set("done", true);
  key_op_.Set("status", keys::KeyStatusName(status));
  if (!status.ok() && !status.detail.empty()) {
    key_op_.Set("detail", status.detail);
  }
  key_op_.Set("result", std::move(result));
}

// The key layer driven step by step (docs/user-data-parity.md S3), standing
// in for the setup window and Settings ▸ Sync. Starts ONE operation and
// answers at once; agentSpace.sync.status's lastOp carries its outcome.
// Secrets never come back: not the ARK, not a passphrase, and a recovery
// code only from the bootstrap that minted it.
PhiVerbResult PhiControlDispatcher::VerbSyncKeyOp(const PhiVerbContext& ctx) {
  PhiSyncKeyService* layer = coordinator_->sync_keys();
  if (!layer) {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload, "no_key_layer");
  }
  const std::string op = ctx.params.String("op");
  const int seq = ++key_op_seq_;
  key_op_ = base::DictValue();
  key_op_.Set("seq", seq);
  key_op_.Set("op", op);
  key_op_.Set("done", false);
  auto finish = [this, seq](keys::KeyStatus status, base::DictValue result) {
    FinishKeyOp(seq, std::move(status), std::move(result));
  };
  if (op == "unlock") {
    layer->SilentUnlockAndResolve();
    finish(keys::KeyStatus::Ok(), base::DictValue());
  } else if (op == "resolve") {
    layer->ResolveMappings();
    finish(keys::KeyStatus::Ok(), base::DictValue());
  } else if (op == "beginSetup") {
    layer->BeginSetup([finish](PhiSyncKeyService::SetupRoute route,
                               keys::KeyStatus status) {
      static constexpr std::array<const char*, 6> kRoutes = {
          "done", "readyToPair", "notSignedIn", "chooseJoin", "introduction",
          "error"};
      base::DictValue result;
      result.Set("route", kRoutes[static_cast<int>(route)]);
      finish(status, std::move(result));
    });
  } else if (op == "bootstrap") {
    // Only with an explicit confirmation: it creates the account's keys.
    if (ctx.params.String("confirm") != "bootstrap") {
      return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload, "confirm");
    }
    layer->Bootstrap([finish](keys::KeyStatus status, std::string code) {
      base::DictValue result;
      result.Set("recoveryCode", code);
      finish(status, std::move(result));
    });
  } else if (op == "requestJoin") {
    layer->RequestJoin([finish](keys::KeyStatus status,
                                keys::JoinTicket ticket) {
      base::DictValue result;
      result.Set("requestId", ticket.request_id);
      result.Set("verificationCode", ticket.verification_code);
      finish(status, std::move(result));
    });
  } else if (op == "pollJoin") {
    layer->PollJoin(ctx.params.String("requestId"),
                    [finish](keys::KeyStatus status, keys::JoinPoll poll) {
                      static constexpr std::array<const char*, 4> kStates = {
                          "pending", "approved", "denied", "expired"};
                      base::DictValue result;
                      result.Set("state", kStates[static_cast<int>(poll.state)]);
                      result.Set("deadlineMs",
                                 static_cast<double>(poll.deadline_ms));
                      finish(status, std::move(result));
                    });
  } else if (op == "cancelJoin") {
    layer->CancelJoin(ctx.params.String("requestId"));
    finish(keys::KeyStatus::Ok(), base::DictValue());
  } else if (op == "recover") {
    layer->JoinWithRecoveryCode(
        ctx.params.String("code"), [finish](keys::KeyStatus status) {
          finish(status, base::DictValue());
        });
  } else if (op == "startPairing") {
    layer->StartPairing([finish](keys::KeyStatus status,
                                 keys::PairingCandidates candidates) {
      base::DictValue result;
      base::ListValue locals;
      for (const keys::LocalProfile& local : candidates.locals) {
        base::DictValue row;
        row.Set("profileId", local.profile_id);
        row.Set("name", local.display_name);
        locals.Append(std::move(row));
      }
      base::ListValue remotes;
      for (const keys::RemoteProfile& remote : candidates.remotes) {
        base::DictValue row;
        row.Set("uuid", remote.uuid);
        if (remote.name) {
          row.Set("name", *remote.name);
        }
        remotes.Append(std::move(row));
      }
      result.Set("locals", std::move(locals));
      result.Set("remotes", std::move(remotes));
      finish(status, std::move(result));
    });
  } else if (op == "applyDecisions") {
    std::vector<keys::PairingDecision> decisions;
    if (const base::ListValue* list = ctx.params.FindList("decisions")) {
      for (const base::Value& item : *list) {
        const base::DictValue* d = item.GetIfDict();
        const std::string* kind = d ? d->FindString("kind") : nullptr;
        if (!kind) {
          return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload,
                                     "decisions");
        }
        keys::PairingDecision decision;
        decision.kind = *kind == "adopt" ? keys::PairingDecision::Kind::kAdopt
                        : *kind == "createLocal"
                            ? keys::PairingDecision::Kind::kCreateLocal
                            : keys::PairingDecision::Kind::kRegisterNew;
        const std::string* local = d->FindString("localProfileId");
        const std::string* remote = d->FindString("remoteUuid");
        const std::string* name = d->FindString("displayName");
        decision.local_profile_id = local ? *local : std::string();
        decision.remote_uuid = remote ? *remote : std::string();
        decision.display_name = name ? *name : std::string();
        decisions.push_back(std::move(decision));
      }
    }
    layer->ApplyDecisions(std::move(decisions),
                          [finish](keys::KeyStatus status) {
                            finish(status, base::DictValue());
                          });
  } else if (op == "refresh") {
    layer->Refresh([this, finish](PhiSyncKeyService::DeviceList list) {
      key_op_pending_ = list.pending;
      base::DictValue result;
      base::ListValue devices;
      for (const keys::AccountDevice& device : list.devices) {
        base::DictValue row;
        row.Set("deviceKeyId", device.device_key_id);
        row.Set("name", device.name);
        row.Set("platform", device.platform);
        row.Set("status", device.status);
        devices.Append(std::move(row));
      }
      base::ListValue pending;
      for (const keys::PendingApproval& approval : list.pending) {
        base::DictValue row;
        row.Set("id", approval.id);
        row.Set("name", approval.name);
        row.Set("platform", approval.platform);
        row.Set("verificationCode", approval.verification_code);
        pending.Append(std::move(row));
      }
      result.Set("devices", std::move(devices));
      result.Set("pending", std::move(pending));
      finish(list.status, std::move(result));
    });
  } else if (op == "approve" || op == "deny") {
    const std::string id = ctx.params.String("requestId");
    const auto it =
        std::find_if(key_op_pending_.begin(), key_op_pending_.end(),
                     [&](const keys::PendingApproval& a) { return a.id == id; });
    if (it == key_op_pending_.end()) {
      return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload, "requestId");
    }
    auto done = [finish](keys::KeyStatus status) {
      finish(status, base::DictValue());
    };
    if (op == "approve") {
      layer->Approve(*it, done);
    } else {
      layer->Deny(id, done);
    }
  } else if (op == "syncNow") {
    layer->SyncNow();
    finish(keys::KeyStatus::Ok(), base::DictValue());
  } else if (op == "peek") {
    layer->PeekPhiData([finish](std::string json) {
      std::optional<base::DictValue> parsed =
          base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
      finish(keys::KeyStatus::Ok(),
             parsed ? std::move(*parsed) : base::DictValue());
    });
  } else if (op == "removeDevice") {
    layer->RemoveThisDevice([finish](keys::KeyStatus status) {
      finish(status, base::DictValue());
    });
  } else {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload, "op");
  }
  base::DictValue out;
  out.Set("seq", seq);
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSyncSetupOpen(
    const PhiVerbContext& ctx) {
  if (!coordinator_->sync_keys()) {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload, "no_key_layer");
  }
  coordinator_->RequestSyncSetup(nullptr);
  return VerbSyncSetupStatus(ctx);
}

PhiVerbResult PhiControlDispatcher::VerbSyncSetupStatus(
    const PhiVerbContext& ctx) {
  PhiSyncSetupWindow* window = PhiSyncSetupWindow::Current();
  if (!window) {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload, "not_open");
  }
  std::optional<base::DictValue> state =
      base::JSONReader::ReadDict(window->StateJson(), base::JSON_PARSE_RFC);
  base::DictValue out = state ? std::move(*state) : base::DictValue();
  out.Set("hwnd", static_cast<double>(
                      reinterpret_cast<uintptr_t>(window->hwnd())));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSyncSetupDrive(
    const PhiVerbContext& ctx) {
  PhiSyncSetupWindow* window = PhiSyncSetupWindow::Current();
  if (!window) {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload, "not_open");
  }
  const std::string error =
      window->Drive(ctx.params.String("action"), ctx.params.String("value"));
  if (!error.empty()) {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload, error);
  }
  if (!PhiSyncSetupWindow::Current()) {
    base::DictValue out;
    out.Set("closed", true);
    return PhiVerbResult::Ok(std::move(out));
  }
  return VerbSyncSetupStatus(ctx);
}

PhiVerbResult PhiControlDispatcher::VerbImportOpen(const PhiVerbContext& ctx) {
  coordinator_->RequestBrowserImport();
  return VerbImportStatus(ctx);
}

PhiVerbResult PhiControlDispatcher::VerbImportStatus(
    const PhiVerbContext& ctx) {
  PhiImportWindow* window = PhiImportWindow::Current();
  base::DictValue out;
  if (window) {
    std::optional<base::DictValue> state =
        base::JSONReader::ReadDict(window->StateJson(), base::JSON_PARSE_RFC);
    if (state) {
      out = std::move(*state);
    }
    out.Set("hwnd", static_cast<double>(
                        reinterpret_cast<uintptr_t>(window->hwnd())));
  }
  out.Set("open", window != nullptr);
  out.Set("running", coordinator_->BrowserImportRunning());
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbImportDrive(const PhiVerbContext& ctx) {
  PhiImportWindow* window = PhiImportWindow::Current();
  if (!window) {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload, "not_open");
  }
  const std::string error =
      window->Drive(ctx.params.String("action"), ctx.params.String("value"));
  if (!error.empty()) {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload, error);
  }
  return VerbImportStatus(ctx);
}

}  // namespace phi_client

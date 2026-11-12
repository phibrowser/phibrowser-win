// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include <cmath>
#include <optional>
#include <string>

#include "base/strings/string_util.h"
#include "base/values.h"
#include "client/phi_control_dispatcher.h"
#include "client/phi_coordinator.h"
#include "client/phi_notification_cards.h"

// Agent notification cards (phibrowser-mac 36b43944,
// docs/agent-notification-cards.md there): an app-level ask with no agent
// Space or page behind it.

namespace phi_client {

PhiVerbResult PhiControlDispatcher::VerbNotificationShow(
    const PhiVerbContext& ctx) {
  // The decision goes back to the asking agent session, so there must be
  // one: the client's own UI is no agent, and a connection without a
  // capability has no session to answer.
  if (ctx.caller == ControlCaller::kInternal ||
      ctx.principal.capability_id.empty()) {
    return PhiVerbResult::Fail("agent_session_required");
  }
  PhiNotificationCards* cards = coordinator_->notification_cards();
  if (!cards) {
    return PhiVerbResult::Fail(PhiErrorCode::kInternal);
  }
  PhiNotificationCards::Request request;
  request.title = ctx.params.String("title");
  request.message = ctx.params.String("message");
  request.button_title = ctx.params.String("buttonTitle");
  if (base::TrimWhitespaceASCII(request.title, base::TRIM_ALL).empty() ||
      base::TrimWhitespaceASCII(request.message, base::TRIM_ALL).empty()) {
    return PhiVerbResult::Fail("invalid_params");
  }
  if (const std::optional<double> expires =
          ctx.params.FindDouble("expiresInSeconds")) {
    // An integer, as the Mac decodes it: 1.5 is not a number of seconds.
    if (*expires != std::floor(*expires) || *expires < 1 ||
        *expires > 86400) {
      return PhiVerbResult::Fail("invalid_params");
    }
    request.expires_in_seconds = static_cast<int>(*expires);
  }
  base::DictValue out;
  out.Set("notificationId",
          cards->Show(request, ctx.principal.capability_id));
  return PhiVerbResult::Ok(std::move(out));
}

}  // namespace phi_client

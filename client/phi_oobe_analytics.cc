// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_oobe_analytics.h"

#include <utility>

namespace phi_client {

PhiOobeAnalyticsSession::PhiOobeAnalyticsSession(Sink sink)
    : sink_(std::move(sink)) {}

PhiOobeAnalyticsSession::~PhiOobeAnalyticsSession() = default;

}  // namespace phi_client

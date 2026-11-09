// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_oobe_analytics.h"

#include <utility>

namespace phi_client {

PhiOobeAnalyticsSession::PhiOobeAnalyticsSession(Sink sink)
    : sink_(std::move(sink)) {}

PhiOobeAnalyticsSession::~PhiOobeAnalyticsSession() = default;

}  // namespace phi_client

// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

// The page screenshot test hooks (docs/page-screenshots.md "Test hooks"):
// kHost, test rig only.

#include <optional>
#include <string>
#include <utility>

#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "client/phi_control_dispatcher.h"
#include "client/phi_coordinator.h"
#include "client/phi_page_screenshot.h"
#include "client/phi_screenshot_editor.h"
#include "client/phi_shell_window.h"
#include "client/phi_window_registry.h"

namespace phi_client {

PhiVerbResult PhiControlDispatcher::VerbScreenshotCapture(
    const PhiVerbContext& ctx) {
  PhiShellWindow* controller = nullptr;
  if (const int64_t window_id = ctx.params.Int64("windowId", 0)) {
    controller = coordinator_->registry().Resolve(window_id);
    if (!controller) {
      return PhiVerbResult::Fail(PhiErrorCode::kWindowNotOpen);
    }
  }
  if (!controller) {
    controller = coordinator_->FocusedUserController();
  }
  if (!controller) {
    return PhiVerbResult::Fail(PhiErrorCode::kWindowNotOpen);
  }
  const std::string mode = ctx.params.String("mode", "full");
  if (mode != "full" && mode != "area") {
    return PhiVerbResult::Fail(PhiErrorCode::kInvalidPayload);
  }
  std::optional<PhiRectF> rect;
  if (const base::DictValue* r = ctx.params.FindDict("rect")) {
    const double x = r->FindDouble("x").value_or(0);
    const double y = r->FindDouble("y").value_or(0);
    rect = PhiRectF{x, y, x + r->FindDouble("width").value_or(0),
                    y + r->FindDouble("height").value_or(0)};
  }
  std::string error;
  if (!PhiPageScreenshot::StartForTest(
          controller,
          mode == "area" ? PhiPageScreenshot::Mode::kArea
                         : PhiPageScreenshot::Mode::kFullPage,
          rect ? &*rect : nullptr, &error)) {
    return PhiVerbResult::Fail(error);
  }
  base::DictValue out;
  out.Set("started", true);
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbScreenshotState(
    const PhiVerbContext& ctx) {
  return PhiVerbResult::Ok(PhiPageScreenshot::StateForTest());
}

PhiVerbResult PhiControlDispatcher::VerbScreenshotExport(
    const PhiVerbContext& ctx) {
  base::DictValue out;
  std::string error;
  if (!PhiScreenshotEditor::ExportForTest(
          ctx.params.String("format"),
          base::UTF8ToWide(ctx.params.String("path")), &out, &error)) {
    return PhiVerbResult::Fail(error);
  }
  out.Set("path", ctx.params.String("path"));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbScreenshotClose(
    const PhiVerbContext& ctx) {
  base::DictValue out;
  out.Set("closed", PhiScreenshotEditor::RequestClose(
                        ctx.params.FindBool("discard").value_or(false)));
  return PhiVerbResult::Ok(std::move(out));
}

}  // namespace phi_client

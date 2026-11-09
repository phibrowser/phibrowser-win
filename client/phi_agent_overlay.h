// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_AGENT_OVERLAY_H_
#define PHIBROWSER_WIN_CLIENT_PHI_AGENT_OVERLAY_H_

#include <windows.h>

#include <string>

#include "client/phi_theme.h"

namespace phi_client {

class PhiShellWindow;

// The watch-mode overlay for an agent Space's content area — the Windows twin
// of the Mac AgentSpaceOverlayView. Two sibling child windows above the
// reparented browser child:
//
//   the FOG   a layered wash over the whole content area that SWALLOWS mouse
//             input while the agent owns the Space, so a watching user cannot
//             disturb the page by accident; ownership changes only through
//             the pill's explicit button.
//   the PILL  agent name, status caption, and the control buttons
//             ("Take control" while the agent owns; "Hand back" + "Finish"
//             once the user has it).
//
// Native windows rather than injected DOM, for the Mac's reasons: they
// survive navigation, work over any page, and can reliably act as the input
// interceptor.
class PhiAgentOverlay {
 public:
  // Mounts (or updates) the overlay over `controller`'s content area.
  // `agent_owns` drives both the barrier and the button set.
  // `mark` is the agent's brand glyph (PhiAgentIconGlyph, empty for the
  // robot), `status` the state word the capsule shows ("Running",
  // "Waiting for you", …) and `waiting` whether the agent is asking the
  // user something — the capsule then expands on its own and stays open
  // (windows-native-ui.md §5.10 cue 4).
  static void Show(PhiShellWindow* controller,
                   const std::string& task_id,
                   const std::wstring& agent_label,
                   const std::wstring& caption,
                   bool agent_owns,
                   const std::wstring& mark = std::wstring(),
                   const std::wstring& status = std::wstring(),
                   bool waiting = false);
  // Tears the overlay down (watch mode ended, or the task is gone).
  static void Hide();
  // Hide() only if the overlay is bound to `controller` — its shell is being
  // destroyed (called from ~PhiShellWindow; a different window's
  // overlay is left alone).
  static void HideIfBoundTo(const PhiShellWindow* controller);
  // The agent's virtual cursor (agentSpace.cursor): `x`/`y` in page CSS px.
  // The overlay converts to device px and GLIDES to the point — the Mac
  // AgentSpaceOverlayView's distance-scaled travel — teleporting only on
  // first appearance. No-op unless `task_id` is the shown overlay's task.
  static void SetAgentCursor(const std::string& task_id, double x, double y);
  // A transient input-mirror animation (agentSpace.effect): `kind` is
  // "click" | "type" | "scroll". `has_point` false anchors on the cursor's
  // current position; `w`/`h` size the typing outline; `dy` signs the
  // scroll hint's direction. All coordinates in page CSS px.
  static void PlayAgentEffect(const std::string& task_id,
                              const std::string& kind,
                              bool has_point,
                              double x,
                              double y,
                              double w,
                              double h,
                              double dy);
  // True while mounted — the shell consults this before handing content
  // clicks to the browser child.
  static bool IsShown();
  // Follows a content-rect change (window resize, layout mode switch).
  static void Reposition();
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_AGENT_OVERLAY_H_

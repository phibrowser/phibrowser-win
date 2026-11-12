// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_AGENT_HANDOFF_PROMPT_H_
#define PHIBROWSER_WIN_CLIENT_PHI_AGENT_HANDOFF_PROMPT_H_

#include <string>

namespace phi_client {

class AgentHost;
class PhiShellWindow;

// The Mac's agent handoff prompt (AgentSpaceManager.presentHandoffPrompt):
// when an agent hands control to the user and asks for them
// (agentSpace.handoff), a small FLOATING, NON-MODAL window — never a
// sheet, the window stack churns at handoff time — says "The agent needs
// you" over the agent's message, with "Switch to Agent Space" (default)
// and "Later". It sits centred on the anchor window, asks for attention
// (the taskbar flash — the Mac's dock bounce), and comes down on its own
// when the hand-off resolves: hand-back, the agent resuming, completion,
// the Space closing. One prompt at a time; a newer one replaces it.
class PhiAgentHandoffPrompt {
 public:
  // `anchor` is the shell the prompt centres on and the slot the switch
  // lands in (null: centred on the work area, switch centres the agent
  // window). `host` serves the switch.
  static void Show(PhiShellWindow* anchor,
                   const std::string& task_id,
                   const std::string& message,
                   AgentHost* host);
  // Takes the prompt down if it is for `task_id` (never another's).
  static void DismissFor(const std::string& task_id);
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_AGENT_HANDOFF_PROMPT_H_

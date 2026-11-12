// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_AGENT_SPACE_ROUTE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_AGENT_SPACE_ROUTE_H_

namespace phi_client {

// Where a switch to an agent Space lands, asked from one user slot — the
// Mac's SpaceManager.agentSpaceSwitchRoute (phibrowser-mac 3a9fc111).
//
// The agent operates ONE window, and only that window shows its Space. On
// Windows the agent's browser lives in its own off-screen agent shell until
// the user watches it; a watch moves the browser into the frame of the user
// slot whose pip was clicked (PhiShellWindow::HostAgentSpace), and leaving
// hands it back only when the swap-back slide lands (FinishUnhost →
// OnAgentBrowserReturned). So at any moment at most one user slot HOSTS the
// browser — for a live watch, or still in flight on the way back — and that
// slot alone presents the Space: its strip and its switcher offer the pip,
// every other window's do not. A switch that still resolves against another
// slot (the handoff prompt, Agent Autoview, a pip in a strip drawn before
// the map changed) surfaces the Space where it lives instead of pulling the
// browser over: pulling it used to end the hosting window's watch under the
// user — ripping the page out of their frame — or, from the same window,
// merely toggled the watch off. Pure and header-only so the table is pinned
// by test/model/agent_space_route_test.cc.
enum class PhiAgentSpaceSwitchRoute {
  // This slot presents the Space: it hosts the browser, or no slot does — an
  // unwatched agent Space is offered everywhere and watched from whichever
  // slot asks (the browser moves out of the agent shell into it).
  kLocal,
  // Another slot hosts the browser for a live watch: bring that window
  // forward. The browser never moves between user slots.
  kSurfaceInHost,
  // Another slot still holds the browser in flight (its swap back has not
  // landed): nothing to surface yet, and hosting it here would fight the
  // hand-back — the switch is dropped.
  kDropWhileInFlight,
};

inline PhiAgentSpaceSwitchRoute PhiAgentSpaceSwitchRouteFor(
    bool hosts_browser_here,
    bool another_slot_hosts_browser,
    bool another_slot_holds_in_flight) {
  if (hosts_browser_here) {
    return PhiAgentSpaceSwitchRoute::kLocal;
  }
  if (another_slot_hosts_browser) {
    return PhiAgentSpaceSwitchRoute::kSurfaceInHost;
  }
  if (another_slot_holds_in_flight) {
    return PhiAgentSpaceSwitchRoute::kDropWhileInFlight;
  }
  return PhiAgentSpaceSwitchRoute::kLocal;
}

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_AGENT_SPACE_ROUTE_H_

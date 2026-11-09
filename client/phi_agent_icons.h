// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_AGENT_ICONS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_AGENT_ICONS_H_

#include <string>

#include "client/phi_mac_icons.h"

namespace phi_client {

// The brand marks of the code agents that drive Phi — the Mac's Assets ▸
// agents imagesets, template images whose ink is normalized to 42 of the
// 54px canvas so every brand reads as one size — as alpha masks tinted at
// draw time (generated phi_agent_icon_masks.cc). Ids: claude, openai,
// cursor, hermes, openclaw, pi, grok, grokbot, antigravity, copilot,
// opencode, qwen, codebuddy, generic (the terminal an unbranded agent
// wears) and phi (the browser's own mark).
const PhiMacIcon* PhiAgentIconMask(const std::string& id);

// A mark as ONE private-use character the icon pipeline resolves back to
// its mask (PhiDrawCatalogIcon / PhiDrawCatalogArtwork), the way the Phi
// catalog's MDL2 stand-ins work — so a pip or menu row that holds display
// text draws the brand without new plumbing. Empty for an unknown id.
// Render-time only: never a stored Space icon.
std::wstring PhiAgentIconGlyph(const std::string& id);
const PhiMacIcon* PhiAgentIconMaskForGlyph(const std::wstring& text);

// "Which code agent drives this Space": the Mac's AgentDriverBadge, from
// the name the driver declared on agentSpace.create (Windows has no peer
// code signature to resolve one from). `icon` is the mark's id when the
// name matches a known brand, else empty — the pip keeps its robot then —
// and `label` the canonical product name, or a readable form of whatever
// name arrived ("Agent" for none).
struct PhiAgentDriverBadge {
  std::string icon;
  std::string label;
};
PhiAgentDriverBadge PhiAgentDriverBadgeFor(const std::string& agent_name);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_AGENT_ICONS_H_

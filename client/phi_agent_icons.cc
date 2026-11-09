// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_agent_icons.h"

#include <string_view>

#include "client/phi_util.h"

namespace phi_client {

namespace {

// Each mark's stand-in: a private-use character above the Segoe MDL2
// block the catalog's stand-ins come from (E700–F8B3), so the two never
// collide and PhiIsPrivateGlyph still routes a stray one to that font
// rather than to the emoji face.
struct AgentGlyph {
  const char* id;
  wchar_t glyph;
};
constexpr AgentGlyph kAgentGlyphs[] = {
    {"claude", L'\uF8E0'},
    {"openai", L'\uF8E1'},
    {"cursor", L'\uF8E2'},
    {"hermes", L'\uF8E3'},
    {"openclaw", L'\uF8E4'},
    {"pi", L'\uF8E5'},
    {"grok", L'\uF8E6'},
    {"grokbot", L'\uF8E7'},
    {"antigravity", L'\uF8E8'},
    {"copilot", L'\uF8E9'},
    {"opencode", L'\uF8EA'},
    {"qwen", L'\uF8EB'},
    {"codebuddy", L'\uF8EC'},
    {"generic", L'\uF8ED'},
    {"phi", L'\uF8EE'},
};

bool Contains(std::string_view text, std::string_view needle) {
  return text.find(needle) != std::string_view::npos;
}

// The Mac's friendlyName: a signing id / bundle id reduced to its last
// dot-separated segment ("com.apple.Terminal" → "Terminal").
std::string FriendlyName(const std::string& raw) {
  const std::string trimmed = PhiTrim(raw);
  if (trimmed.empty() || trimmed == "Unknown process") {
    return std::string();
  }
  const size_t dot = trimmed.rfind('.');
  return dot == std::string::npos ? trimmed : trimmed.substr(dot + 1);
}

// The Mac's namesPhiAgent: the product itself, or the phi-agent component
// as a whole dot-separated segment — never a substring, so "pi-agent"
// keeps its own badge.
bool NamesPhiAgent(std::string_view lower) {
  if (lower == "phi" || lower == "phi browser") {
    return true;
  }
  size_t start = 0;
  while (start <= lower.size()) {
    const size_t dot = lower.find('.', start);
    const size_t end = dot == std::string_view::npos ? lower.size() : dot;
    if (lower.substr(start, end - start) == "phi-agent") {
      return true;
    }
    if (dot == std::string_view::npos) {
      break;
    }
    start = dot + 1;
  }
  return false;
}

}  // namespace

std::wstring PhiAgentIconGlyph(const std::string& id) {
  for (const AgentGlyph& entry : kAgentGlyphs) {
    if (id == entry.id) {
      return std::wstring(1, entry.glyph);
    }
  }
  return std::wstring();
}

const PhiMacIcon* PhiAgentIconMaskForGlyph(const std::wstring& text) {
  if (text.size() != 1) {
    return nullptr;
  }
  for (const AgentGlyph& entry : kAgentGlyphs) {
    if (entry.glyph == text[0]) {
      return PhiAgentIconMask(entry.id);
    }
  }
  return nullptr;
}

PhiAgentDriverBadge PhiAgentDriverBadgeFor(const std::string& agent_name) {
  // The Mac's AgentDriverBadge.make for a CDP-driven task, needle for
  // needle. Order matters twice: "grok bot" contains "grok", and "pi" is
  // too short a token for a substring test at all.
  const std::string lower = PhiLower(agent_name);
  if (NamesPhiAgent(lower)) {
    return {"phi", "Phi"};
  }
  if (Contains(lower, "claude")) {
    return {"claude", "Claude Code"};
  }
  if (Contains(lower, "codex") || Contains(lower, "openai") ||
      Contains(lower, "chatgpt")) {
    return {"openai", "Codex"};
  }
  if (Contains(lower, "openclaw")) {
    return {"openclaw", "OpenClaw"};
  }
  if (Contains(lower, "hermes")) {
    return {"hermes", "Hermes"};
  }
  if (Contains(lower, "cursor")) {
    return {"cursor", "Cursor"};
  }
  struct SkillOnly {
    const char* needle;
    const char* icon;
    const char* label;
  };
  constexpr SkillOnly kSkillOnly[] = {
      {"grok bot", "grokbot", "Grok Bot"},
      {"grokbot", "grokbot", "Grok Bot"},
      {"grok", "grok", "Grok Build"},
      {"antigravity", "antigravity", "Antigravity"},
      {"copilot", "copilot", "GitHub Copilot"},
      {"opencode", "opencode", "OpenCode"},
      {"qwen", "qwen", "Qwen Code"},
      {"codebuddy", "codebuddy", "CodeBuddy"},
  };
  for (const SkillOnly& entry : kSkillOnly) {
    if (Contains(lower, entry.needle)) {
      return {entry.icon, entry.label};
    }
  }
  if (lower == "pi" || lower.rfind("pi.", 0) == 0 ||
      lower.rfind("pi-", 0) == 0 || Contains(lower, "pi.dev") ||
      Contains(lower, ".pi.")) {
    return {"pi", "Pi"};
  }
  const std::string friendly = FriendlyName(agent_name);
  return {std::string(), friendly.empty() ? "Agent" : friendly};
}

}  // namespace phi_client

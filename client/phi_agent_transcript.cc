// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client/phi_agent_transcript.h"

#include <algorithm>
#include <utility>

namespace phi_client {

namespace {

using Kind = PhiTranscriptEntry::Kind;

bool IsProse(Kind kind) {
  return kind == Kind::kNarration || kind == Kind::kUser ||
         kind == Kind::kAssistant || kind == Kind::kThinking;
}

// Control characters go; newlines survive only in prose (a terse action
// line is one line by definition). Then the cap, at a UTF-8 boundary.
std::string Sanitize(const std::string& in, bool keep_newlines, size_t cap) {
  std::string out;
  out.reserve(std::min(in.size(), cap));
  for (const char c : in) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u == '\n' || u == '\r') {
      if (keep_newlines && (out.empty() || out.back() != '\n')) {
        out.push_back('\n');
      } else if (!keep_newlines && !out.empty() && out.back() != ' ') {
        out.push_back(' ');
      }
      continue;
    }
    if (u < 0x20 && u != '\t') {
      continue;
    }
    out.push_back(c);
  }
  // Trim.
  while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) {
    out.pop_back();
  }
  size_t start = 0;
  while (start < out.size() && (out[start] == ' ' || out[start] == '\n')) {
    ++start;
  }
  out.erase(0, start);
  if (out.size() > cap) {
    size_t cut = cap;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
      --cut;
    }
    out.resize(cut);
    out += "…";
  }
  return out;
}

}  // namespace

// static
std::string PhiAgentTranscript::SanitizeText(const std::string& text,
                                             bool keep_newlines,
                                             size_t cap) {
  return Sanitize(text, keep_newlines, cap);
}

PhiAgentTranscript::PhiAgentTranscript() = default;
PhiAgentTranscript::~PhiAgentTranscript() = default;

bool PhiAgentTranscript::Append(const std::string& task_id,
                                PhiTranscriptEntry entry) {
  Buffer& buffer = buffers_[task_id];
  const bool prose = IsProse(entry.kind);
  entry.text = Sanitize(entry.text, prose,
                        prose ? kMaxProseChars : kMaxTerseChars);
  const bool pi_tool = entry.kind == Kind::kTool && entry.agent == "pi";
  entry.detail = Sanitize(entry.detail, /*keep_newlines=*/true,
                          pi_tool ? kMaxPiToolDetailChars : kMaxDetailChars);
  if (entry.kind == Kind::kTool && !entry.source_id.empty()) {
    // A result for a pending tool call settles that card in place.
    for (PhiTranscriptEntry& existing : buffer.entries) {
      if (existing.kind == Kind::kTool &&
          existing.source_id == entry.source_id &&
          existing.tool_state == "pending") {
        existing.tool_state =
            entry.tool_state.empty() ? "success" : entry.tool_state;
        if (!entry.detail.empty()) {
          existing.detail = entry.detail;
        }
        if (!entry.text.empty()) {
          existing.text = entry.text;
        }
        return true;
      }
    }
  }
  if (entry.text.empty() && entry.detail.empty()) {
    return false;
  }
  entry.seq = next_seq_++;
  if (entry.task_number == 0) {
    entry.task_number = buffer.number;
  }
  buffer.entries.push_back(std::move(entry));
  while (buffer.entries.size() > kMaxEntriesPerTask) {
    buffer.entries.pop_front();
  }
  return true;
}

void PhiAgentTranscript::SetActivity(const std::string& task_id,
                                     const std::string& text) {
  auto it = buffers_.find(task_id);
  if (it == buffers_.end()) {
    if (text.empty()) {
      return;
    }
    it = buffers_.emplace(task_id, Buffer()).first;
  }
  it->second.activity = Sanitize(text, false, kMaxTerseChars);
}

const std::string& PhiAgentTranscript::Activity(
    const std::string& task_id) const {
  static const std::string kEmpty;
  auto it = buffers_.find(task_id);
  return it == buffers_.end() ? kEmpty : it->second.activity;
}

std::vector<PhiTranscriptEntry> PhiAgentTranscript::Entries(
    const std::string& task_id) const {
  auto it = buffers_.find(task_id);
  if (it == buffers_.end()) {
    return {};
  }
  return std::vector<PhiTranscriptEntry>(it->second.entries.begin(),
                                         it->second.entries.end());
}

std::vector<PhiTranscriptTaskInfo> PhiAgentTranscript::Tasks() const {
  std::vector<PhiTranscriptTaskInfo> out;
  for (const auto& [task_id, buffer] : buffers_) {
    if (buffer.entries.empty() && !buffer.live) {
      continue;
    }
    PhiTranscriptTaskInfo info;
    info.task_id = task_id;
    info.number = buffer.number;
    info.live = buffer.live;
    out.push_back(std::move(info));
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const PhiTranscriptTaskInfo& a,
                      const PhiTranscriptTaskInfo& b) {
                     if (a.live != b.live) {
                       return a.live;
                     }
                     return a.number < b.number;
                   });
  return out;
}

bool PhiAgentTranscript::Has(const std::string& task_id) const {
  return buffers_.count(task_id) > 0;
}

void PhiAgentTranscript::NoteTaskLive(const std::string& task_id,
                                      int number,
                                      int64_t now_ms) {
  auto it = buffers_.find(task_id);
  if (it != buffers_.end() && !it->second.live && it->second.ended_ms &&
      now_ms - it->second.ended_ms > kContinuityMs) {
    buffers_.erase(it);  // Too old to be the same conversation.
    it = buffers_.end();
  }
  Buffer& buffer = it == buffers_.end() ? buffers_[task_id] : it->second;
  buffer.live = true;
  buffer.number = number;
  buffer.ended_ms = 0;
}

void PhiAgentTranscript::NoteTaskEnded(const std::string& task_id,
                                       int64_t now_ms) {
  auto it = buffers_.find(task_id);
  if (it == buffers_.end()) {
    return;
  }
  it->second.live = false;
  it->second.ended_ms = now_ms;
  it->second.activity.clear();
}

void PhiAgentTranscript::Reap(int64_t now_ms, const std::string& keep) {
  for (auto it = buffers_.begin(); it != buffers_.end();) {
    const Buffer& buffer = it->second;
    if (!buffer.live && it->first != keep &&
        (buffer.ended_ms == 0 || now_ms - buffer.ended_ms > kContinuityMs)) {
      it = buffers_.erase(it);
    } else {
      ++it;
    }
  }
}

void PhiAgentTranscript::Clear(const std::string& task_id) {
  if (task_id.empty()) {
    for (auto& [id, buffer] : buffers_) {
      buffer.entries.clear();
    }
    return;
  }
  auto it = buffers_.find(task_id);
  if (it != buffers_.end()) {
    it->second.entries.clear();
  }
}

std::string PhiRenderTranscriptEntry(const PhiTranscriptEntry& entry) {
  std::string line;
  switch (entry.kind) {
    case Kind::kRound:
      return "── " + entry.text + " ──";
    case Kind::kStatus:
      line = "· " + entry.text;
      break;
    case Kind::kError:
      line = "⚠ " + entry.text;
      break;
    case Kind::kUser:
      line = "❯ " + entry.text;
      break;
    case Kind::kThinking:
      line = "  ✻ " + entry.text;
      break;
    case Kind::kTool: {
      const char* mark = entry.tool_state == "pending"   ? "◌ "
                         : entry.tool_state == "error"   ? "✗ "
                                                         : "⏺ ";
      line = std::string(mark) + entry.text;
      break;
    }
    case Kind::kAssistant:
    case Kind::kNarration:
      line = entry.text;
      break;
    case Kind::kAction:
      line = "⏺ " + entry.text;
      break;
  }
  if (!entry.detail.empty()) {
    std::string detail = entry.detail;
    // Indent every continuation line under the ⎿ hook.
    std::string indented = "  ⎿ ";
    for (const char c : detail) {
      indented.push_back(c);
      if (c == '\n') {
        indented += "    ";
      }
    }
    line += "\n" + indented;
  }
  return line;
}

}  // namespace phi_client

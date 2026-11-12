// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_sync_invalidation.h"

#include <cstdint>
#include <map>

namespace phi_client::sync {

namespace {

// A minimal JSON reader for the hint's object: strings, integers, arrays of
// them, nested values skipped. Enough for the stream's two payloads.
class JsonReader {
 public:
  explicit JsonReader(std::string_view text) : text_(text) {}

  // Reads one object into `strings` / `numbers` (top level only).
  bool ReadObject(std::map<std::string, std::string>* strings,
                  std::map<std::string, std::vector<int64_t>>* numbers) {
    SkipSpace();
    if (!Eat('{')) {
      return false;
    }
    SkipSpace();
    if (Eat('}')) {
      return AtEnd();
    }
    for (;;) {
      std::string key;
      SkipSpace();
      if (!ReadString(&key)) {
        return false;
      }
      SkipSpace();
      if (!Eat(':')) {
        return false;
      }
      SkipSpace();
      if (Peek() == '"') {
        std::string value;
        if (!ReadString(&value)) {
          return false;
        }
        (*strings)[key] = value;
      } else if (Peek() == '[') {
        std::vector<int64_t> values;
        if (!ReadNumberArray(&values)) {
          return false;
        }
        (*numbers)[key] = values;
      } else if (!SkipValue()) {
        return false;
      }
      SkipSpace();
      if (Eat(',')) {
        continue;
      }
      if (Eat('}')) {
        return AtEnd();
      }
      return false;
    }
  }

 private:
  char Peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
  bool Eat(char c) {
    if (Peek() != c) {
      return false;
    }
    ++pos_;
    return true;
  }
  bool AtEnd() {
    SkipSpace();
    return pos_ == text_.size();
  }
  void SkipSpace() {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' ||
            text_[pos_] == '\r')) {
      ++pos_;
    }
  }
  bool ReadString(std::string* out) {
    if (!Eat('"')) {
      return false;
    }
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') {
        return true;
      }
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) {
        return false;
      }
      const char escape = text_[pos_++];
      switch (escape) {
        case '"':
        case '\\':
        case '/':
          out->push_back(escape);
          break;
        case 'n':
          out->push_back('\n');
          break;
        case 't':
          out->push_back('\t');
          break;
        case 'r':
          out->push_back('\r');
          break;
        case 'b':
          out->push_back('\b');
          break;
        case 'f':
          out->push_back('\f');
          break;
        case 'u':
          // Identifiers are ASCII; any \\u escape is kept as a marker.
          if (pos_ + 4 > text_.size()) {
            return false;
          }
          pos_ += 4;
          out->push_back('?');
          break;
        default:
          return false;
      }
    }
    return false;
  }
  bool ReadNumber(int64_t* out) {
    bool negative = Eat('-');
    if (Peek() < '0' || Peek() > '9') {
      return false;
    }
    int64_t value = 0;
    while (Peek() >= '0' && Peek() <= '9') {
      if (value > (INT64_MAX - 9) / 10) {
        return false;
      }
      value = value * 10 + (text_[pos_++] - '0');
    }
    if (Peek() == '.' || Peek() == 'e' || Peek() == 'E') {
      return false;  // integers only
    }
    *out = negative ? -value : value;
    return true;
  }
  bool ReadNumberArray(std::vector<int64_t>* out) {
    if (!Eat('[')) {
      return false;
    }
    SkipSpace();
    if (Eat(']')) {
      return true;
    }
    for (;;) {
      SkipSpace();
      int64_t value = 0;
      if (!ReadNumber(&value)) {
        return false;
      }
      out->push_back(value);
      SkipSpace();
      if (Eat(',')) {
        continue;
      }
      return Eat(']');
    }
  }
  // Nesting past 32 levels is refused: each level is one stack frame.
  bool SkipValue(int depth = 0) {
    if (depth >= 32) {
      return false;
    }
    SkipSpace();
    const char c = Peek();
    if (c == '"') {
      std::string ignored;
      return ReadString(&ignored);
    }
    if (c == '{' || c == '[') {
      const char close = c == '{' ? '}' : ']';
      ++pos_;
      SkipSpace();
      if (Eat(close)) {
        return true;
      }
      for (;;) {
        if (c == '{') {
          std::string key;
          SkipSpace();
          if (!ReadString(&key)) {
            return false;
          }
          SkipSpace();
          if (!Eat(':')) {
            return false;
          }
        }
        if (!SkipValue(depth + 1)) {
          return false;
        }
        SkipSpace();
        if (Eat(',')) {
          continue;
        }
        return Eat(close);
      }
    }
    for (const char* word : {"true", "false", "null"}) {
      const std::string_view w(word);
      if (text_.substr(pos_, w.size()) == w) {
        pos_ += w.size();
        return true;
      }
    }
    int64_t ignored = 0;
    return ReadNumber(&ignored);
  }

  std::string_view text_;
  size_t pos_ = 0;
};

constexpr std::string_view kChromiumPrefix = "chromium:";

}  // namespace

std::optional<std::string> InvalidationHint::profile_uuid() const {
  if (name_space.rfind(kChromiumPrefix, 0) != 0 || name_space == "chromium:phi") {
    return std::nullopt;
  }
  return name_space.substr(kChromiumPrefix.size());
}

bool InvalidationHint::IsValid() const {
  if (name_space.rfind(kChromiumPrefix, 0) != 0) {
    return false;
  }
  const std::string suffix = name_space.substr(kChromiumPrefix.size());
  if (suffix.empty() || suffix.size() > 64) {
    return false;
  }
  for (const char c : suffix) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
    if (!ok) {
      return false;
    }
  }
  if (data_types.empty() || data_types.size() > 64) {
    return false;
  }
  for (const int type : data_types) {
    if (type <= 0) {
      return false;
    }
  }
  return source_client_id.size() <= 256;
}

InvalidationParser::InvalidationParser() = default;
InvalidationParser::~InvalidationParser() = default;

bool InvalidationParser::Feed(std::string_view bytes,
                              std::vector<InvalidationEvent>* events) {
  for (const char byte : bytes) {
    if (previous_was_cr_) {
      previous_was_cr_ = false;
      if (byte == '\n') {
        continue;
      }
    }
    if (byte == '\n' || byte == '\r') {
      std::optional<InvalidationEvent> event;
      if (!FinishLine(&event)) {
        return false;
      }
      if (event) {
        events->push_back(std::move(*event));
      }
      previous_was_cr_ = byte == '\r';
      continue;
    }
    if (line_.size() >= kMaximumEventBytes) {
      return false;
    }
    line_.push_back(byte);
  }
  return true;
}

bool InvalidationParser::FinishLine(std::optional<InvalidationEvent>* event) {
  std::string text = std::move(line_);
  line_.clear();
  if (first_line_) {
    first_line_ = false;
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) {
      text.erase(0, 3);
    }
  }
  if (text.empty()) {
    // A blank line dispatches the event.
    const std::string name = std::move(event_name_);
    std::vector<std::string> lines = std::move(data_lines_);
    event_name_.clear();
    data_lines_.clear();
    event_bytes_ = 0;
    if (lines.empty()) {
      return true;
    }
    std::string payload;
    for (size_t i = 0; i < lines.size(); ++i) {
      if (i) {
        payload.push_back('\n');
      }
      payload += lines[i];
    }
    std::map<std::string, std::string> strings;
    std::map<std::string, std::vector<int64_t>> numbers;
    if (name == "ready") {
      if (!JsonReader(payload).ReadObject(&strings, &numbers)) {
        return false;
      }
      InvalidationEvent out;
      out.kind = InvalidationEvent::Kind::kReady;
      *event = std::move(out);
      return true;
    }
    if (name == "invalidate") {
      if (!JsonReader(payload).ReadObject(&strings, &numbers)) {
        return false;
      }
      InvalidationEvent out;
      out.kind = InvalidationEvent::Kind::kInvalidate;
      out.hint.name_space = strings["namespace"];
      out.hint.source_client_id = strings["source_client_id"];
      for (const int64_t type : numbers["data_types"]) {
        if (type <= 0 || type > INT32_MAX) {
          return false;
        }
        out.hint.data_types.push_back(static_cast<int>(type));
      }
      if (!out.hint.IsValid()) {
        return false;
      }
      *event = std::move(out);
      return true;
    }
    return true;  // an unknown event is ignored
  }
  if (text[0] == ':') {
    InvalidationEvent out;
    out.kind = InvalidationEvent::Kind::kHeartbeat;
    *event = std::move(out);
    return true;
  }
  event_bytes_ += text.size();
  if (event_bytes_ > kMaximumEventBytes) {
    return false;
  }
  const size_t colon = text.find(':');
  const std::string field = text.substr(0, colon);
  std::string value = colon == std::string::npos ? std::string()
                                                 : text.substr(colon + 1);
  if (!value.empty() && value[0] == ' ') {
    value.erase(0, 1);
  }
  if (field == "event") {
    event_name_ = value;
  } else if (field == "data") {
    data_lines_.push_back(value);
  }
  return true;  // id / retry are not a durable cursor
}

}  // namespace phi_client::sync

// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.
//
// JSON reading and writing with the behaviour of Chromium's base: profiles and
// settings files are shared with the framework and with earlier versions.

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "utf8_decode.h"

namespace base {

namespace {

// --- Reader ----------------------------------------------------------------

class Parser {
 public:
  Parser(std::string_view json, int options, size_t max_depth)
      : json_(json), options_(options), max_depth_(max_depth) {}

  std::optional<Value> Parse() {
    if (json_.substr(0, 3) == "\xEF\xBB\xBF") {
      pos_ = 3;
    }
    SkipSpace();
    std::optional<Value> value = ParseValue(1);
    if (!value || failed_) {
      return std::nullopt;
    }
    SkipSpace();
    if (failed_ || pos_ != json_.size()) {
      return std::nullopt;
    }
    return value;
  }

 private:
  bool AtEnd() const { return pos_ >= json_.size(); }
  char Peek() const { return json_[pos_]; }

  void SkipSpace() {
    while (!AtEnd()) {
      const char c = Peek();
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else if (c == '/' && (options_ & JSON_ALLOW_COMMENTS)) {
        if (!SkipComment()) {
          failed_ = true;
          return;
        }
      } else {
        return;
      }
    }
  }

  bool SkipComment() {
    if (pos_ + 1 >= json_.size()) {
      return false;
    }
    if (json_[pos_ + 1] == '/') {
      pos_ += 2;
      while (!AtEnd() && Peek() != '\n') {
        ++pos_;
      }
      return true;
    }
    if (json_[pos_ + 1] == '*') {
      const size_t end = json_.find("*/", pos_ + 2);
      if (end == std::string_view::npos) {
        return false;
      }
      pos_ = end + 2;
      return true;
    }
    return false;
  }

  bool Consume(std::string_view word) {
    if (json_.substr(pos_, word.size()) != word) {
      return false;
    }
    pos_ += word.size();
    return true;
  }

  std::optional<Value> ParseValue(size_t depth) {
    if (AtEnd()) {
      return std::nullopt;
    }
    switch (Peek()) {
      case '{': return ParseDict(depth);
      case '[': return ParseList(depth);
      case '"': {
        std::string text;
        if (!ParseString(&text)) {
          return std::nullopt;
        }
        return Value(std::move(text));
      }
      case 't':
        return Consume("true") ? std::optional<Value>(Value(true)) : std::nullopt;
      case 'f':
        return Consume("false") ? std::optional<Value>(Value(false))
                                : std::nullopt;
      case 'n':
        return Consume("null") ? std::optional<Value>(Value()) : std::nullopt;
      default:
        return ParseNumber();
    }
  }

  std::optional<Value> ParseDict(size_t depth) {
    if (depth >= max_depth_) {
      return std::nullopt;
    }
    ++pos_;
    DictValue dict;
    SkipSpace();
    if (!AtEnd() && Peek() == '}') {
      ++pos_;
      return Value(std::move(dict));
    }
    while (true) {
      SkipSpace();
      if (AtEnd() || Peek() != '"') {
        return std::nullopt;
      }
      std::string key;
      if (!ParseString(&key)) {
        return std::nullopt;
      }
      SkipSpace();
      if (AtEnd() || Peek() != ':') {
        return std::nullopt;
      }
      ++pos_;
      SkipSpace();
      std::optional<Value> value = ParseValue(depth + 1);
      if (!value) {
        return std::nullopt;
      }
      dict.Set(key, std::move(*value));
      SkipSpace();
      if (AtEnd()) {
        return std::nullopt;
      }
      const char c = Peek();
      ++pos_;
      if (c == '}') {
        return Value(std::move(dict));
      }
      if (c != ',') {
        return std::nullopt;
      }
      if (options_ & JSON_ALLOW_TRAILING_COMMAS) {
        SkipSpace();
        if (!AtEnd() && Peek() == '}') {
          ++pos_;
          return Value(std::move(dict));
        }
      }
    }
  }

  std::optional<Value> ParseList(size_t depth) {
    if (depth >= max_depth_) {
      return std::nullopt;
    }
    ++pos_;
    ListValue list;
    SkipSpace();
    if (!AtEnd() && Peek() == ']') {
      ++pos_;
      return Value(std::move(list));
    }
    while (true) {
      SkipSpace();
      std::optional<Value> value = ParseValue(depth + 1);
      if (!value) {
        return std::nullopt;
      }
      list.Append(std::move(*value));
      SkipSpace();
      if (AtEnd()) {
        return std::nullopt;
      }
      const char c = Peek();
      ++pos_;
      if (c == ']') {
        return Value(std::move(list));
      }
      if (c != ',') {
        return std::nullopt;
      }
      if (options_ & JSON_ALLOW_TRAILING_COMMAS) {
        SkipSpace();
        if (!AtEnd() && Peek() == ']') {
          ++pos_;
          return Value(std::move(list));
        }
      }
    }
  }

  static void AppendUtf8(uint32_t code_point, std::string* out) {
    if (code_point < 0x80) {
      out->push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (code_point >> 6)));
      out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (code_point >> 12)));
      out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (code_point >> 18)));
      out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
  }

  bool ReadHex(size_t digits, uint32_t* out) {
    if (pos_ + digits > json_.size()) {
      return false;
    }
    uint32_t value = 0;
    for (size_t i = 0; i < digits; ++i) {
      const char c = json_[pos_ + i];
      uint32_t digit;
      if (c >= '0' && c <= '9') {
        digit = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      } else {
        return false;
      }
      value = value * 16 + digit;
    }
    pos_ += digits;
    *out = value;
    return true;
  }

  // Copies one UTF-8 sequence from the input, validating it.
  bool CopyUtf8Sequence(std::string* out) {
    const size_t start = pos_;
    if (internal::NextUtf8(json_, &pos_) >= 0) {
      out->append(json_.substr(start, pos_ - start));
      return true;
    }
    if (!(options_ & JSON_REPLACE_INVALID_CHARACTERS)) {
      return false;
    }
    AppendUtf8(0xFFFD, out);
    return true;
  }

  bool ParseUnicodeEscape(std::string* out) {
    uint32_t unit;
    if (!ReadHex(4, &unit)) {
      return false;
    }
    if (unit >= 0xD800 && unit <= 0xDBFF) {
      if (json_.substr(pos_, 2) == "\\u") {
        pos_ += 2;
        uint32_t low;
        if (!ReadHex(4, &low)) {
          return false;
        }
        if (low >= 0xDC00 && low <= 0xDFFF) {
          AppendUtf8(0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00), out);
          return true;
        }
      }
      if (!(options_ & JSON_REPLACE_INVALID_CHARACTERS)) {
        return false;
      }
      AppendUtf8(0xFFFD, out);
      return true;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
      if (!(options_ & JSON_REPLACE_INVALID_CHARACTERS)) {
        return false;
      }
      AppendUtf8(0xFFFD, out);
      return true;
    }
    AppendUtf8(unit, out);
    return true;
  }

  bool ParseString(std::string* out) {
    ++pos_;
    while (true) {
      if (AtEnd()) {
        return false;
      }
      const char c = Peek();
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (c == '\\') {
        ++pos_;
        if (AtEnd()) {
          return false;
        }
        const char escape = Peek();
        ++pos_;
        switch (escape) {
          case '"': out->push_back('"'); break;
          case '\\': out->push_back('\\'); break;
          case '/': out->push_back('/'); break;
          case 'b': out->push_back('\b'); break;
          case 'f': out->push_back('\f'); break;
          case 'n': out->push_back('\n'); break;
          case 'r': out->push_back('\r'); break;
          case 't': out->push_back('\t'); break;
          case 'u':
            if (!ParseUnicodeEscape(out)) {
              return false;
            }
            break;
          case 'v':
            if (!(options_ & JSON_ALLOW_VERT_TAB)) {
              return false;
            }
            out->push_back('\v');
            break;
          case 'x': {
            uint32_t value;
            if (!(options_ & JSON_ALLOW_X_ESCAPES) || !ReadHex(2, &value)) {
              return false;
            }
            AppendUtf8(value, out);
            break;
          }
          default:
            return false;
        }
        continue;
      }
      if (static_cast<unsigned char>(c) < 0x20) {
        const bool allowed = (c == '\n' || c == '\r') &&
                             (options_ & JSON_ALLOW_NEWLINES_IN_STRINGS);
        if (!allowed) {
          return false;
        }
      }
      if (!CopyUtf8Sequence(out)) {
        return false;
      }
    }
  }

  std::optional<Value> ParseNumber() {
    const size_t start = pos_;
    const bool negative = Peek() == '-';
    if (negative) {
      ++pos_;
    }
    if (AtEnd() || Peek() < '0' || Peek() > '9') {
      return std::nullopt;
    }
    if (Peek() == '0') {
      ++pos_;
      if (!AtEnd() && Peek() >= '0' && Peek() <= '9') {
        return std::nullopt;
      }
    } else {
      while (!AtEnd() && Peek() >= '0' && Peek() <= '9') {
        ++pos_;
      }
    }
    bool is_integer = true;
    if (!AtEnd() && Peek() == '.') {
      is_integer = false;
      ++pos_;
      if (AtEnd() || Peek() < '0' || Peek() > '9') {
        return std::nullopt;
      }
      while (!AtEnd() && Peek() >= '0' && Peek() <= '9') {
        ++pos_;
      }
    }
    if (!AtEnd() && (Peek() == 'e' || Peek() == 'E')) {
      is_integer = false;
      ++pos_;
      if (!AtEnd() && (Peek() == '+' || Peek() == '-')) {
        ++pos_;
      }
      if (AtEnd() || Peek() < '0' || Peek() > '9') {
        return std::nullopt;
      }
      while (!AtEnd() && Peek() >= '0' && Peek() <= '9') {
        ++pos_;
      }
    }
    const std::string_view text = json_.substr(start, pos_ - start);
    if (is_integer && !(negative && text == "-0")) {
      int64_t whole;
      const auto [end, error] =
          std::from_chars(text.data(), text.data() + text.size(), whole);
      if (error == std::errc() && end == text.data() + text.size() &&
          whole >= INT32_MIN && whole <= INT32_MAX) {
        return Value(static_cast<int>(whole));
      }
    }
    double real;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), real);
    if (error == std::errc::result_out_of_range) {
      // Too small rounds to zero; too large is an error, as in serde.
      const size_t exponent = text.find_first_of("eE");
      const bool tiny = exponent != std::string_view::npos &&
                        text[exponent + 1] == '-';
      if (!tiny) {
        return std::nullopt;
      }
      return Value(negative ? -0.0 : 0.0);
    }
    if (error != std::errc() || end != text.data() + text.size() ||
        !std::isfinite(real)) {
      return std::nullopt;
    }
    return Value(real);
  }

  std::string_view json_;
  int options_;
  size_t max_depth_;
  size_t pos_ = 0;
  bool failed_ = false;
};

// --- Writer ----------------------------------------------------------------

constexpr char kLineEnding[] = "\r\n";

void AppendUEscape(uint32_t code_point, std::string* out) {
  char buffer[8];
  snprintf(buffer, sizeof(buffer), "\\u%04X", code_point);
  out->append(buffer);
}

void AppendEscaped(std::string_view text, std::string* out) {
  out->push_back('"');
  size_t index = 0;
  while (index < text.size()) {
    const size_t start = index;
    const int32_t decoded = internal::NextUtf8(text, &index);
    if (decoded < 0) {
      out->append("\xEF\xBF\xBD");
      continue;
    }
    const uint32_t code_point = static_cast<uint32_t>(decoded);
    switch (code_point) {
      case '\b': out->append("\\b"); continue;
      case '\f': out->append("\\f"); continue;
      case '\n': out->append("\\n"); continue;
      case '\r': out->append("\\r"); continue;
      case '\t': out->append("\\t"); continue;
      case '\\': out->append("\\\\"); continue;
      case '"': out->append("\\\""); continue;
      case '<': out->append("\\u003C"); continue;
      case 0x2028: out->append("\\u2028"); continue;
      case 0x2029: out->append("\\u2029"); continue;
      default: break;
    }
    if (code_point < 0x20) {
      AppendUEscape(code_point, out);
    } else {
      out->append(text.substr(start, index - start));
    }
  }
  out->push_back('"');
}

class Writer {
 public:
  Writer(int options, size_t max_depth, std::string* out)
      : omit_binary_(options & OPTIONS_OMIT_BINARY_VALUES),
        omit_double_type_(options & OPTIONS_OMIT_DOUBLE_TYPE_PRESERVATION),
        pretty_(options & OPTIONS_PRETTY_PRINT),
        max_depth_(max_depth),
        out_(out) {}

  bool Write(const Value& value, size_t indent) {
    switch (value.type()) {
      case Value::Type::NONE: out_->append("null"); return true;
      case Value::Type::BOOLEAN:
        out_->append(value.GetBool() ? "true" : "false");
        return true;
      case Value::Type::INTEGER:
        out_->append(NumberToString(value.GetInt()));
        return true;
      case Value::Type::DOUBLE: return WriteDouble(value.GetDouble());
      case Value::Type::STRING:
        AppendEscaped(value.GetString(), out_);
        return true;
      case Value::Type::BINARY: return omit_binary_;
      case Value::Type::DICT: return Write(value.GetDict(), indent);
      case Value::Type::LIST: return Write(value.GetList(), indent);
    }
    return false;
  }

  bool Write(const DictValue& dict, size_t indent) {
    if (depth_ >= max_depth_) {
      return false;
    }
    ++depth_;
    out_->push_back('{');
    if (pretty_) {
      out_->append(kLineEnding);
    }
    bool first = true;
    bool result = true;
    for (const auto [key, value] : dict) {
      if (omit_binary_ && value.is_blob()) {
        continue;
      }
      if (!first) {
        out_->push_back(',');
        if (pretty_) {
          out_->append(kLineEnding);
        }
      }
      if (pretty_) {
        Indent(indent + 1);
      }
      AppendEscaped(key, out_);
      out_->push_back(':');
      if (pretty_) {
        out_->push_back(' ');
      }
      result &= Write(value, indent + 1);
      first = false;
    }
    if (pretty_) {
      if (!first) {
        out_->append(kLineEnding);
      }
      Indent(indent);
    }
    out_->push_back('}');
    --depth_;
    return result;
  }

  bool Write(const ListValue& list, size_t indent) {
    if (depth_ >= max_depth_) {
      return false;
    }
    ++depth_;
    out_->push_back('[');
    if (pretty_) {
      out_->push_back(' ');
    }
    bool first = true;
    bool result = true;
    for (const Value& value : list) {
      if (omit_binary_ && value.is_blob()) {
        continue;
      }
      if (!first) {
        out_->push_back(',');
        if (pretty_) {
          out_->push_back(' ');
        }
      }
      result &= Write(value, indent);
      first = false;
    }
    if (pretty_) {
      out_->push_back(' ');
    }
    out_->push_back(']');
    --depth_;
    return result;
  }

 private:
  void Indent(size_t depth) { out_->append(depth * 3, ' '); }

  bool WriteDouble(double value) {
    if (omit_double_type_ && std::floor(value) == value &&
        value >= -9223372036854775808.0 && value < 9223372036854775808.0) {
      out_->append(NumberToString(static_cast<int64_t>(value)));
      return true;
    }
    std::string real = NumberToString(value);
    if (real.find_first_of(".eE") == std::string::npos) {
      real.append(".0");
    }
    out_->append(real);
    return true;
  }

  bool omit_binary_;
  bool omit_double_type_;
  bool pretty_;
  size_t max_depth_;
  size_t depth_ = 0;
  std::string* out_;
};

bool WriteView(ValueView node, int options, size_t max_depth, std::string* out) {
  out->clear();
  Writer writer(options, max_depth, out);
  bool ok;
  if (node.value()) {
    ok = writer.Write(*node.value(), 0);
  } else if (node.dict()) {
    ok = writer.Write(*node.dict(), 0);
  } else {
    ok = writer.Write(*node.list(), 0);
  }
  if ((options & OPTIONS_PRETTY_PRINT) && ok) {
    out->append(kLineEnding);
  }
  return ok;
}

}  // namespace

std::optional<Value> JSONReader::Read(std::string_view json,
                                      int options,
                                      size_t max_depth) {
  return Parser(json, options, max_depth).Parse();
}

std::optional<DictValue> JSONReader::ReadDict(std::string_view json,
                                              int options,
                                              size_t max_depth) {
  std::optional<Value> value = Read(json, options, max_depth);
  if (!value || !value->is_dict()) {
    return std::nullopt;
  }
  return std::move(*value).TakeDict();
}

std::optional<ListValue> JSONReader::ReadList(std::string_view json,
                                              int options,
                                              size_t max_depth) {
  std::optional<Value> value = Read(json, options, max_depth);
  if (!value || !value->is_list()) {
    return std::nullopt;
  }
  return std::move(*value).TakeList();
}

bool JSONWriter::Write(ValueView node, std::string* json) {
  return WriteView(node, 0, 200, json);
}

bool JSONWriter::WriteWithOptions(ValueView node,
                                  int options,
                                  std::string* json) {
  return WriteView(node, options, 200, json);
}

std::optional<std::string> WriteJson(ValueView node, size_t max_depth) {
  return WriteJsonWithOptions(node, 0, max_depth);
}

std::optional<std::string> WriteJsonWithOptions(ValueView node,
                                                int options,
                                                size_t max_depth) {
  std::string out;
  if (!WriteView(node, options, max_depth, &out)) {
    return std::nullopt;
  }
  return out;
}

}  // namespace base

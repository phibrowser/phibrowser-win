// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#include "client/phi_sync_proto.h"

#include <utility>

namespace phi_client::sync {

// --- Wire primitives -------------------------------------------------------------

namespace wire {

void PutVarint(std::string* out, uint64_t value) {
  while (value >= 0x80) {
    out->push_back(static_cast<char>((value & 0x7F) | 0x80));
    value >>= 7;
  }
  out->push_back(static_cast<char>(value));
}

void PutTag(std::string* out, int field, WireType type) {
  PutVarint(out, (static_cast<uint64_t>(field) << 3) | type);
}

void PutVarintField(std::string* out, int field, uint64_t value) {
  PutTag(out, field, kVarint);
  PutVarint(out, value);
}

void PutBytesField(std::string* out, int field, std::string_view bytes) {
  PutTag(out, field, kLengthDelimited);
  PutVarint(out, bytes.size());
  out->append(bytes);
}

bool Reader::Advance(size_t count, std::string_view* out) {
  if (count > data_.size() - pos_) {
    return false;
  }
  if (out) {
    *out = data_.substr(pos_, count);
  }
  pos_ += count;
  return true;
}

bool Reader::Varint(uint64_t* value) {
  uint64_t result = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (pos_ >= data_.size()) {
      return false;
    }
    const uint8_t byte = static_cast<uint8_t>(data_[pos_++]);
    result |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if (!(byte & 0x80)) {
      *value = result;
      return true;
    }
  }
  return false;
}

bool Reader::Next(int* field, WireType* type) {
  tag_start_ = pos_;
  uint64_t tag = 0;
  if (!Varint(&tag)) {
    return false;
  }
  const uint64_t number = tag >> 3;
  const uint64_t kind = tag & 7;
  if (number == 0 || number > 0x1FFFFFFF || kind > kFixed32) {
    return false;
  }
  *field = static_cast<int>(number);
  *type = static_cast<WireType>(kind);
  return true;
}

bool Reader::Bytes(std::string_view* value) {
  uint64_t length = 0;
  return Varint(&length) && Advance(static_cast<size_t>(length), value);
}

bool Reader::Group(int field, std::string_view* body, int depth) {
  // Each nesting level is one stack frame: a hostile response must not
  // overflow the stack.
  if (depth >= 64) {
    return false;
  }
  const size_t start = pos_;
  for (;;) {
    const size_t tag_at = pos_;
    int inner = 0;
    WireType inner_type = kVarint;
    if (!Next(&inner, &inner_type)) {
      return false;
    }
    if (inner_type == kEndGroup) {
      if (inner != field) {
        return false;
      }
      *body = data_.substr(start, tag_at - start);
      return true;
    }
    if (inner_type == kStartGroup) {
      std::string_view nested;
      if (!Group(inner, &nested, depth + 1)) {
        return false;
      }
      continue;
    }
    if (!Skip(inner_type, nullptr)) {
      return false;
    }
  }
}

bool Reader::Skip(WireType type, std::string* unknown) {
  const size_t tag_start = tag_start_;
  bool ok = false;
  switch (type) {
    case kStartGroup: {
      // The group's own field number is in the tag just read.
      Reader probe(data_);
      probe.pos_ = tag_start;
      int field = 0;
      WireType ignored = kVarint;
      std::string_view body;
      ok = probe.Next(&field, &ignored) && probe.Group(field, &body);
      if (ok) {
        pos_ = probe.pos_;
      }
      break;
    }
    case kEndGroup:
      ok = false;  // an unmatched end-group
      break;
    case kVarint: {
      uint64_t ignored = 0;
      ok = Varint(&ignored);
      break;
    }
    case kFixed64:
      ok = Advance(8, nullptr);
      break;
    case kFixed32:
      ok = Advance(4, nullptr);
      break;
    case kLengthDelimited: {
      std::string_view ignored;
      ok = Bytes(&ignored);
      break;
    }
  }
  if (ok && unknown) {
    unknown->append(data_.substr(tag_start, pos_ - tag_start));
  }
  tag_start_ = tag_start;
  return ok;
}

}  // namespace wire

namespace {

using wire::Reader;
using wire::WireType;

void PutInt64(std::string* out, int field, int64_t value) {
  wire::PutVarintField(out, field, static_cast<uint64_t>(value));
}

void PutInt32(std::string* out, int field, int32_t value) {
  // Negative int32 goes out sign-extended to ten bytes, as protobuf does.
  wire::PutVarintField(out, field,
                       static_cast<uint64_t>(static_cast<int64_t>(value)));
}

void PutValue(std::string* out, int field, const OptionalValue& value) {
  if (value.has()) {
    wire::PutBytesField(out, field, value.get().Serialize());
  }
}

bool ReadVarint(Reader& reader, WireType type, uint64_t* value) {
  return type == wire::kVarint && reader.Varint(value);
}

bool ReadInt64(Reader& reader, WireType type, int64_t* value) {
  uint64_t raw = 0;
  if (!ReadVarint(reader, type, &raw)) {
    return false;
  }
  *value = static_cast<int64_t>(raw);
  return true;
}

bool ReadInt32(Reader& reader, WireType type, int32_t* value) {
  uint64_t raw = 0;
  if (!ReadVarint(reader, type, &raw)) {
    return false;
  }
  *value = static_cast<int32_t>(static_cast<uint32_t>(raw));
  return true;
}

bool ReadBool(Reader& reader, WireType type, bool* value) {
  uint64_t raw = 0;
  if (!ReadVarint(reader, type, &raw)) {
    return false;
  }
  *value = raw != 0;
  return true;
}

bool ReadString(Reader& reader, WireType type, std::string* value) {
  std::string_view bytes;
  if (type != wire::kLengthDelimited || !reader.Bytes(&bytes)) {
    return false;
  }
  *value = std::string(bytes);
  return true;
}

// A message field: a repeat MERGES into what is there.
bool ReadValue(Reader& reader, WireType type, OptionalValue* value) {
  std::string_view bytes;
  if (type != wire::kLengthDelimited || !reader.Bytes(&bytes)) {
    return false;
  }
  return value->mut().ParseFrom(bytes);
}

const SettingValue& DefaultValue() {
  static const SettingValue kDefault;
  return kDefault;
}

}  // namespace

// --- SettingValue ------------------------------------------------------------------

void SettingValue::SetString(std::string value) {
  kind = Kind::kString;
  string_value = std::move(value);
}

void SettingValue::SetBool(bool value) {
  kind = Kind::kBool;
  bool_value = value;
}

void SettingValue::SetInt(int64_t value) {
  kind = Kind::kInt;
  int_value = value;
}

bool SettingValue::ParseFrom(std::string_view bytes) {
  Reader reader(bytes);
  while (!reader.done()) {
    int field = 0;
    WireType type = wire::kVarint;
    if (!reader.Next(&field, &type)) {
      return false;
    }
    bool ok = true;
    switch (field) {
      case 1:
        ok = ReadInt64(reader, type, &updated_at_ms);
        break;
      case 2: {
        bool value = false;
        ok = ReadBool(reader, type, &value);
        if (ok) {
          SetBool(value);
        }
        break;
      }
      case 3: {
        std::string value;
        ok = ReadString(reader, type, &value);
        if (ok) {
          SetString(std::move(value));
        }
        break;
      }
      case 4: {
        int64_t value = 0;
        ok = ReadInt64(reader, type, &value);
        if (ok) {
          SetInt(value);
        }
        break;
      }
      default:
        ok = reader.Skip(type, &unknown);
        break;
    }
    if (!ok) {
      return false;
    }
  }
  return true;
}

std::string SettingValue::Serialize() const {
  std::string out;
  if (updated_at_ms != 0) {
    PutInt64(&out, 1, updated_at_ms);
  }
  switch (kind) {
    case Kind::kNone:
      break;
    case Kind::kBool:
      wire::PutVarintField(&out, 2, bool_value ? 1 : 0);
      break;
    case Kind::kString:
      wire::PutBytesField(&out, 3, string_value);
      break;
    case Kind::kInt:
      PutInt64(&out, 4, int_value);
      break;
  }
  out += unknown;
  return out;
}

bool SettingValue::operator==(const SettingValue& other) const {
  if (updated_at_ms != other.updated_at_ms || kind != other.kind ||
      unknown != other.unknown) {
    return false;
  }
  switch (kind) {
    case Kind::kNone:
      return true;
    case Kind::kBool:
      return bool_value == other.bool_value;
    case Kind::kString:
      return string_value == other.string_value;
    case Kind::kInt:
      return int_value == other.int_value;
  }
  return false;
}

// --- OptionalValue -------------------------------------------------------------------

OptionalValue::OptionalValue() = default;
OptionalValue::OptionalValue(const OptionalValue& other) = default;
OptionalValue::OptionalValue(OptionalValue&& other) = default;
OptionalValue& OptionalValue::operator=(const OptionalValue& other) = default;
OptionalValue& OptionalValue::operator=(OptionalValue&& other) = default;
OptionalValue::~OptionalValue() = default;

const SettingValue& OptionalValue::get() const {
  return value_ ? *value_ : DefaultValue();
}

SettingValue& OptionalValue::mut() {
  if (!value_) {
    value_.emplace();
  }
  return *value_;
}

// --- SettingEntity -------------------------------------------------------------------

bool SettingEntity::ParseFrom(std::string_view bytes) {
  Reader reader(bytes);
  while (!reader.done()) {
    int field = 0;
    WireType type = wire::kVarint;
    if (!reader.Next(&field, &type)) {
      return false;
    }
    if (field != 1) {
      if (!reader.Skip(type, &unknown)) {
        return false;
      }
      continue;
    }
    std::string_view entry_bytes;
    if (type != wire::kLengthDelimited || !reader.Bytes(&entry_bytes)) {
      return false;
    }
    // One map entry: key 1, value 2; either may be absent (the default).
    std::string key;
    SettingValue value;
    Reader entry(entry_bytes);
    while (!entry.done()) {
      int entry_field = 0;
      WireType entry_type = wire::kVarint;
      if (!entry.Next(&entry_field, &entry_type)) {
        return false;
      }
      bool ok = true;
      if (entry_field == 1) {
        ok = ReadString(entry, entry_type, &key);
      } else if (entry_field == 2) {
        std::string_view value_bytes;
        ok = entry_type == wire::kLengthDelimited &&
             entry.Bytes(&value_bytes) && value.ParseFrom(value_bytes);
      } else {
        ok = entry.Skip(entry_type, nullptr);
      }
      if (!ok) {
        return false;
      }
    }
    values[key] = std::move(value);
  }
  return true;
}

std::string SettingEntity::Serialize() const {
  std::string out;
  for (const auto& [key, value] : values) {  // key order: deterministic
    std::string entry;
    wire::PutBytesField(&entry, 1, key);
    wire::PutBytesField(&entry, 2, value.Serialize());
    wire::PutBytesField(&out, 1, entry);
  }
  out += unknown;
  return out;
}

bool SettingEntity::operator==(const SettingEntity& other) const {
  return values == other.values && unknown == other.unknown;
}

// --- SpaceEntity ---------------------------------------------------------------------

bool SpaceEntity::ParseFrom(std::string_view bytes) {
  Reader reader(bytes);
  while (!reader.done()) {
    int field = 0;
    WireType type = wire::kVarint;
    if (!reader.Next(&field, &type)) {
      return false;
    }
    bool ok = true;
    switch (field) {
      case 1:
        ok = ReadString(reader, type, &space_uuid);
        break;
      case 2:
        ok = ReadValue(reader, type, &name);
        break;
      case 3:
        ok = ReadValue(reader, type, &icon_name);
        break;
      case 4:
        ok = ReadValue(reader, type, &color_hex);
        break;
      case 5:
        ok = ReadValue(reader, type, &rank);
        break;
      case 6:
        ok = ReadValue(reader, type, &profile_uuid);
        break;
      case 7:
        ok = ReadValue(reader, type, &theme_id);
        break;
      case 8:
        ok = ReadValue(reader, type, &overlay_opacity_light);
        break;
      case 9:
        ok = ReadValue(reader, type, &overlay_opacity_dark);
        break;
      case 10:
        ok = ReadInt64(reader, type, &created_at_ms);
        break;
      default:
        ok = reader.Skip(type, &unknown);
        break;
    }
    if (!ok) {
      return false;
    }
  }
  return true;
}

std::string SpaceEntity::Serialize() const {
  std::string out;
  if (!space_uuid.empty()) {
    wire::PutBytesField(&out, 1, space_uuid);
  }
  PutValue(&out, 2, name);
  PutValue(&out, 3, icon_name);
  PutValue(&out, 4, color_hex);
  PutValue(&out, 5, rank);
  PutValue(&out, 6, profile_uuid);
  PutValue(&out, 7, theme_id);
  PutValue(&out, 8, overlay_opacity_light);
  PutValue(&out, 9, overlay_opacity_dark);
  if (created_at_ms != 0) {
    PutInt64(&out, 10, created_at_ms);
  }
  out += unknown;
  return out;
}

bool SpaceEntity::operator==(const SpaceEntity& other) const {
  return space_uuid == other.space_uuid && name == other.name &&
         icon_name == other.icon_name && color_hex == other.color_hex &&
         rank == other.rank && profile_uuid == other.profile_uuid &&
         theme_id == other.theme_id &&
         overlay_opacity_light == other.overlay_opacity_light &&
         overlay_opacity_dark == other.overlay_opacity_dark &&
         created_at_ms == other.created_at_ms && unknown == other.unknown;
}

// --- BookmarkEntity ------------------------------------------------------------------

bool BookmarkEntity::ParseFrom(std::string_view bytes) {
  Reader reader(bytes);
  while (!reader.done()) {
    int field = 0;
    WireType type = wire::kVarint;
    if (!reader.Next(&field, &type)) {
      return false;
    }
    bool ok = true;
    switch (field) {
      case 1:
        ok = ReadString(reader, type, &bookmark_uuid);
        break;
      case 2:
        ok = ReadValue(reader, type, &space_uuid);
        break;
      case 3:
        ok = ReadValue(reader, type, &parent_uuid);
        break;
      case 4:
        ok = ReadValue(reader, type, &rank);
        break;
      case 5:
        ok = ReadBool(reader, type, &is_folder);
        break;
      case 6:
        ok = ReadValue(reader, type, &title);
        break;
      case 7:
        ok = ReadValue(reader, type, &url);
        break;
      case 8:
        ok = ReadValue(reader, type, &secondary_url);
        break;
      case 9:
        ok = ReadValue(reader, type, &secondary_title);
        break;
      case 10:
        ok = ReadInt32(reader, type, &source);
        break;
      case 11:
        ok = ReadInt64(reader, type, &created_at_ms);
        break;
      default:
        ok = reader.Skip(type, &unknown);
        break;
    }
    if (!ok) {
      return false;
    }
  }
  return true;
}

std::string BookmarkEntity::Serialize() const {
  std::string out;
  if (!bookmark_uuid.empty()) {
    wire::PutBytesField(&out, 1, bookmark_uuid);
  }
  PutValue(&out, 2, space_uuid);
  PutValue(&out, 3, parent_uuid);
  PutValue(&out, 4, rank);
  if (is_folder) {
    wire::PutVarintField(&out, 5, 1);
  }
  PutValue(&out, 6, title);
  PutValue(&out, 7, url);
  PutValue(&out, 8, secondary_url);
  PutValue(&out, 9, secondary_title);
  if (source != 0) {
    PutInt32(&out, 10, source);
  }
  if (created_at_ms != 0) {
    PutInt64(&out, 11, created_at_ms);
  }
  out += unknown;
  return out;
}

bool BookmarkEntity::operator==(const BookmarkEntity& other) const {
  return bookmark_uuid == other.bookmark_uuid &&
         space_uuid == other.space_uuid && parent_uuid == other.parent_uuid &&
         rank == other.rank && is_folder == other.is_folder &&
         title == other.title && url == other.url &&
         secondary_url == other.secondary_url &&
         secondary_title == other.secondary_title && source == other.source &&
         created_at_ms == other.created_at_ms && unknown == other.unknown;
}

// --- PinTabEntity --------------------------------------------------------------------

bool PinTabEntity::ParseFrom(std::string_view bytes) {
  Reader reader(bytes);
  while (!reader.done()) {
    int field = 0;
    WireType type = wire::kVarint;
    if (!reader.Next(&field, &type)) {
      return false;
    }
    bool ok = true;
    switch (field) {
      case 1:
        ok = ReadString(reader, type, &pin_uuid);
        break;
      case 2:
      case 3: {
        std::string value;
        ok = ReadString(reader, type, &value);
        if (ok) {
          owner = field == 2 ? Owner::kSpace : Owner::kProfile;
          owner_uuid = std::move(value);
        }
        break;
      }
      case 4:
        ok = ReadValue(reader, type, &rank);
        break;
      case 5:
        ok = ReadValue(reader, type, &title);
        break;
      case 6:
        ok = ReadValue(reader, type, &url);
        break;
      case 7:
        ok = ReadValue(reader, type, &split_partner_uuid);
        break;
      case 8:
        ok = ReadInt32(reader, type, &source);
        break;
      case 9:
        ok = ReadInt64(reader, type, &created_at_ms);
        break;
      default:
        ok = reader.Skip(type, &unknown);
        break;
    }
    if (!ok) {
      return false;
    }
  }
  return true;
}

std::string PinTabEntity::Serialize() const {
  std::string out;
  if (!pin_uuid.empty()) {
    wire::PutBytesField(&out, 1, pin_uuid);
  }
  if (owner == Owner::kSpace) {
    wire::PutBytesField(&out, 2, owner_uuid);
  } else if (owner == Owner::kProfile) {
    wire::PutBytesField(&out, 3, owner_uuid);
  }
  PutValue(&out, 4, rank);
  PutValue(&out, 5, title);
  PutValue(&out, 6, url);
  PutValue(&out, 7, split_partner_uuid);
  if (source != 0) {
    PutInt32(&out, 8, source);
  }
  if (created_at_ms != 0) {
    PutInt64(&out, 9, created_at_ms);
  }
  out += unknown;
  return out;
}

bool PinTabEntity::operator==(const PinTabEntity& other) const {
  return pin_uuid == other.pin_uuid && owner == other.owner &&
         (owner == Owner::kApp || owner_uuid == other.owner_uuid) &&
         rank == other.rank && title == other.title && url == other.url &&
         split_partner_uuid == other.split_partner_uuid &&
         source == other.source && created_at_ms == other.created_at_ms &&
         unknown == other.unknown;
}

// --- UrlRuleEntity -------------------------------------------------------------------

bool UrlRuleEntity::ParseFrom(std::string_view bytes) {
  Reader reader(bytes);
  while (!reader.done()) {
    int field = 0;
    WireType type = wire::kVarint;
    if (!reader.Next(&field, &type)) {
      return false;
    }
    bool ok = true;
    switch (field) {
      case 1:
        ok = ReadString(reader, type, &rule_uuid);
        break;
      case 2:
        ok = ReadValue(reader, type, &host);
        break;
      case 3:
        ok = ReadValue(reader, type, &path_prefix);
        break;
      case 4:
        ok = ReadValue(reader, type, &ask);
        break;
      case 5:
        ok = ReadValue(reader, type, &target_space_uuid);
        break;
      case 6:
        ok = ReadValue(reader, type, &rank);
        break;
      case 7:
        ok = ReadInt64(reader, type, &created_at_ms);
        break;
      case 8:
        ok = ReadInt32(reader, type, &source);
        break;
      default:
        ok = reader.Skip(type, &unknown);
        break;
    }
    if (!ok) {
      return false;
    }
  }
  return true;
}

std::string UrlRuleEntity::Serialize() const {
  std::string out;
  if (!rule_uuid.empty()) {
    wire::PutBytesField(&out, 1, rule_uuid);
  }
  PutValue(&out, 2, host);
  PutValue(&out, 3, path_prefix);
  PutValue(&out, 4, ask);
  PutValue(&out, 5, target_space_uuid);
  PutValue(&out, 6, rank);
  if (created_at_ms != 0) {
    PutInt64(&out, 7, created_at_ms);
  }
  if (source != 0) {
    PutInt32(&out, 8, source);
  }
  out += unknown;
  return out;
}

bool UrlRuleEntity::operator==(const UrlRuleEntity& other) const {
  return rule_uuid == other.rule_uuid && host == other.host &&
         path_prefix == other.path_prefix && ask == other.ask &&
         target_space_uuid == other.target_space_uuid && rank == other.rank &&
         created_at_ms == other.created_at_ms && source == other.source &&
         unknown == other.unknown;
}

// --- PhiEntity -----------------------------------------------------------------------

bool PhiEntity::ParseFrom(std::string_view bytes) {
  Reader reader(bytes);
  while (!reader.done()) {
    int field = 0;
    WireType type = wire::kVarint;
    if (!reader.Next(&field, &type)) {
      return false;
    }
    if (field < 1 || field > 5) {
      if (!reader.Skip(type, &unknown)) {
        return false;
      }
      continue;
    }
    std::string_view body;
    if (type != wire::kLengthDelimited || !reader.Bytes(&body)) {
      return false;
    }
    // A oneof member repeated merges; a different member replaces.
    const Kind next = static_cast<Kind>(field);
    if (next != kind) {
      setting = SettingEntity();
      space = SpaceEntity();
      bookmark = BookmarkEntity();
      pin_tab = PinTabEntity();
      url_rule = UrlRuleEntity();
      kind = next;
    }
    bool ok = false;
    switch (kind) {
      case Kind::kSetting:
        ok = setting.ParseFrom(body);
        break;
      case Kind::kSpace:
        ok = space.ParseFrom(body);
        break;
      case Kind::kBookmark:
        ok = bookmark.ParseFrom(body);
        break;
      case Kind::kPinTab:
        ok = pin_tab.ParseFrom(body);
        break;
      case Kind::kUrlRule:
        ok = url_rule.ParseFrom(body);
        break;
      case Kind::kNone:
        break;
    }
    if (!ok) {
      return false;
    }
  }
  return true;
}

std::string PhiEntity::Serialize() const {
  std::string out;
  switch (kind) {
    case Kind::kNone:
      break;
    case Kind::kSetting:
      wire::PutBytesField(&out, 1, setting.Serialize());
      break;
    case Kind::kSpace:
      wire::PutBytesField(&out, 2, space.Serialize());
      break;
    case Kind::kBookmark:
      wire::PutBytesField(&out, 3, bookmark.Serialize());
      break;
    case Kind::kPinTab:
      wire::PutBytesField(&out, 4, pin_tab.Serialize());
      break;
    case Kind::kUrlRule:
      wire::PutBytesField(&out, 5, url_rule.Serialize());
      break;
  }
  out += unknown;
  return out;
}

}  // namespace phi_client::sync

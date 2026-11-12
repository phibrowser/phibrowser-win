// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SYNC_PROTO_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SYNC_PROTO_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

// phi.PhiEntity (phibrowser-mac Sources/Sync/Phi/Proto/phi_entity.proto) —
// the plaintext of Phi native sync's ciphertext (docs/user-data-parity.md
// §1.2) — as plain structs with a hand-written protobuf codec. Host-clean
// (STL only).
//
// The bytes must be the Mac's bytes, not merely wire-compatible ones: the
// shared last-writer-wins rule breaks timestamp ties on a value's SERIALIZED
// bytes, and the engine decides "publish again?" by comparing bytes. So the
// writer reproduces swift-protobuf exactly:
//   - fields in field-number order, a oneof at its members' position;
//   - proto3 scalars omitted at their default (0, "", false); message fields
//     written when PRESENT (an empty one is `tag 00`); oneof members written
//     whatever their value;
//   - map entries in key order (the Mac serializes with deterministic
//     ordering), each entry carrying both key and value;
//   - unknown fields (a newer peer's) kept verbatim and written LAST, in the
//     order they arrived — merging always starts from the server's bytes so
//     they survive (the Mac's §6.2).
// Parsing follows protobuf: a repeated scalar keeps the last value, a
// repeated message field MERGES into the first, the last oneof member wins.
namespace phi_client::sync {

// PhiSettingValue: one LWW-stamped scalar.
struct SettingValue {
  enum class Kind { kNone, kBool, kString, kInt };

  int64_t updated_at_ms = 0;
  Kind kind = Kind::kNone;
  bool bool_value = false;
  std::string string_value;
  int64_t int_value = 0;
  std::string unknown;

  // swift-protobuf's readouts: the member when the oneof holds it, else the
  // type's default.
  std::string str() const {
    return kind == Kind::kString ? string_value : std::string();
  }
  bool flag() const { return kind == Kind::kBool && bool_value; }
  int64_t integer() const { return kind == Kind::kInt ? int_value : 0; }

  void SetString(std::string value);
  void SetBool(bool value);
  void SetInt(int64_t value);

  bool ParseFrom(std::string_view bytes);  // merges into *this
  std::string Serialize() const;
  bool operator==(const SettingValue& other) const;
  bool operator!=(const SettingValue& other) const { return !(*this == other); }
};

// A message field with presence: absent reads back as the default value
// (swift-protobuf's getter), and assigning makes it present.
class OptionalValue {
 public:
  OptionalValue();
  OptionalValue(const OptionalValue& other);
  OptionalValue(OptionalValue&& other);
  OptionalValue& operator=(const OptionalValue& other);
  OptionalValue& operator=(OptionalValue&& other);
  ~OptionalValue();

  bool has() const { return value_.has_value(); }
  const SettingValue& get() const;
  SettingValue& mut();  // makes it present
  void set(SettingValue value) { value_ = std::move(value); }
  void clear() { value_.reset(); }
  bool operator==(const OptionalValue& other) const {
    return value_ == other.value_;
  }

 private:
  std::optional<SettingValue> value_;
};

struct SettingEntity {
  std::map<std::string, SettingValue> values;
  std::string unknown;

  bool ParseFrom(std::string_view bytes);
  std::string Serialize() const;
  bool operator==(const SettingEntity& other) const;
};

struct SpaceEntity {
  std::string space_uuid;
  OptionalValue name;
  OptionalValue icon_name;
  OptionalValue color_hex;
  OptionalValue rank;
  OptionalValue profile_uuid;
  OptionalValue theme_id;
  OptionalValue overlay_opacity_light;
  OptionalValue overlay_opacity_dark;
  int64_t created_at_ms = 0;
  std::string unknown;

  bool ParseFrom(std::string_view bytes);
  std::string Serialize() const;
  bool operator==(const SpaceEntity& other) const;
};

struct BookmarkEntity {
  std::string bookmark_uuid;
  OptionalValue space_uuid;
  OptionalValue parent_uuid;
  OptionalValue rank;
  bool is_folder = false;
  OptionalValue title;
  OptionalValue url;
  OptionalValue secondary_url;
  OptionalValue secondary_title;
  int32_t source = 0;
  int64_t created_at_ms = 0;
  std::string unknown;

  bool ParseFrom(std::string_view bytes);
  std::string Serialize() const;
  bool operator==(const BookmarkEntity& other) const;
};

struct PinTabEntity {
  enum class Owner { kApp, kSpace, kProfile };  // kApp = the oneof absent

  std::string pin_uuid;
  Owner owner = Owner::kApp;
  std::string owner_uuid;  // kSpace / kProfile
  OptionalValue rank;
  OptionalValue title;
  OptionalValue url;
  OptionalValue split_partner_uuid;
  int32_t source = 0;
  int64_t created_at_ms = 0;
  std::string unknown;

  bool ParseFrom(std::string_view bytes);
  std::string Serialize() const;
  bool operator==(const PinTabEntity& other) const;
};

struct UrlRuleEntity {
  std::string rule_uuid;
  OptionalValue host;
  OptionalValue path_prefix;
  OptionalValue ask;
  OptionalValue target_space_uuid;
  OptionalValue rank;
  int64_t created_at_ms = 0;
  int32_t source = 0;
  std::string unknown;

  bool ParseFrom(std::string_view bytes);
  std::string Serialize() const;
  bool operator==(const UrlRuleEntity& other) const;
};

// The envelope: exactly one kind (or none, for a newer kind this build does
// not know — its bytes then ride in `unknown`).
struct PhiEntity {
  enum class Kind { kNone, kSetting, kSpace, kBookmark, kPinTab, kUrlRule };

  Kind kind = Kind::kNone;
  SettingEntity setting;
  SpaceEntity space;
  BookmarkEntity bookmark;
  PinTabEntity pin_tab;
  UrlRuleEntity url_rule;
  std::string unknown;

  bool ParseFrom(std::string_view bytes);
  std::string Serialize() const;
};

// --- Wire primitives (shared with the sync protocol messages) --------------

namespace wire {

enum WireType {
  kVarint = 0,
  kFixed64 = 1,
  kLengthDelimited = 2,
  kStartGroup = 3,  // proto2 groups (the server's CommitResponse.EntryResponse)
  kEndGroup = 4,
  kFixed32 = 5,
};

void PutVarint(std::string* out, uint64_t value);
void PutTag(std::string* out, int field, WireType type);
void PutVarintField(std::string* out, int field, uint64_t value);
void PutBytesField(std::string* out, int field, std::string_view bytes);

class Reader {
 public:
  explicit Reader(std::string_view data) : data_(data) {}
  bool done() const { return pos_ >= data_.size(); }
  // The next field's number and type; false on malformed input.
  bool Next(int* field, WireType* type);
  bool Varint(uint64_t* value);
  bool Bytes(std::string_view* value);
  // Skips the current field's payload, appending the field's raw bytes
  // (tag included) to `unknown` when non-null. A start-group skips through
  // its matching end-group.
  bool Skip(WireType type, std::string* unknown);
  // The body of the group whose start-group tag (field `field`) was just
  // read, up to (not including) its end-group tag, which is consumed.
  // Groups nested deeper than 64 are malformed (`depth` is this one's).
  bool Group(int field, std::string_view* body, int depth = 0);

 private:
  bool Advance(size_t count, std::string_view* out);

  std::string_view data_;
  size_t pos_ = 0;
  size_t tag_start_ = 0;
};

}  // namespace wire

}  // namespace phi_client::sync

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SYNC_PROTO_H_

// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#include "base/values.h"

#include <algorithm>
#include <cmath>

namespace base {

// --- DictValue -------------------------------------------------------------

DictValue::DictValue() = default;
DictValue::DictValue(DictValue&&) noexcept = default;
DictValue& DictValue::operator=(DictValue&&) noexcept = default;
DictValue::~DictValue() = default;

DictValue DictValue::Clone() const {
  DictValue copy;
  for (const auto& [key, value] : storage_) {
    copy.storage_.emplace(key, std::make_unique<Value>(value->Clone()));
  }
  return copy;
}

void DictValue::Merge(DictValue dict) {
  for (auto& [key, value] : dict.storage_) {
    if (value->is_dict()) {
      if (DictValue* existing = FindDict(key)) {
        existing->Merge(std::move(*value).TakeDict());
        continue;
      }
    }
    Set(key, std::move(*value));
  }
}

const Value* DictValue::Find(std::string_view key) const {
  auto it = storage_.find(key);
  return it == storage_.end() ? nullptr : it->second.get();
}

Value* DictValue::Find(std::string_view key) {
  auto it = storage_.find(key);
  return it == storage_.end() ? nullptr : it->second.get();
}

std::optional<bool> DictValue::FindBool(std::string_view key) const {
  const Value* value = Find(key);
  return value ? value->GetIfBool() : std::nullopt;
}

std::optional<int> DictValue::FindInt(std::string_view key) const {
  const Value* value = Find(key);
  return value ? value->GetIfInt() : std::nullopt;
}

std::optional<double> DictValue::FindDouble(std::string_view key) const {
  const Value* value = Find(key);
  return value ? value->GetIfDouble() : std::nullopt;
}

const std::string* DictValue::FindString(std::string_view key) const {
  const Value* value = Find(key);
  return value ? value->GetIfString() : nullptr;
}

std::string* DictValue::FindString(std::string_view key) {
  Value* value = Find(key);
  return value ? value->GetIfString() : nullptr;
}

const std::vector<uint8_t>* DictValue::FindBlob(std::string_view key) const {
  const Value* value = Find(key);
  return value ? value->GetIfBlob() : nullptr;
}

const DictValue* DictValue::FindDict(std::string_view key) const {
  const Value* value = Find(key);
  return value ? value->GetIfDict() : nullptr;
}

DictValue* DictValue::FindDict(std::string_view key) {
  Value* value = Find(key);
  return value ? value->GetIfDict() : nullptr;
}

const ListValue* DictValue::FindList(std::string_view key) const {
  const Value* value = Find(key);
  return value ? value->GetIfList() : nullptr;
}

ListValue* DictValue::FindList(std::string_view key) {
  Value* value = Find(key);
  return value ? value->GetIfList() : nullptr;
}

DictValue* DictValue::EnsureDict(std::string_view key) {
  if (DictValue* existing = FindDict(key)) {
    return existing;
  }
  return Set(key, DictValue())->GetIfDict();
}

ListValue* DictValue::EnsureList(std::string_view key) {
  if (ListValue* existing = FindList(key)) {
    return existing;
  }
  return Set(key, ListValue())->GetIfList();
}

Value* DictValue::Set(std::string_view key, Value&& value) & {
  auto it = storage_.find(key);
  if (it != storage_.end()) {
    *it->second = std::move(value);
    return it->second.get();
  }
  return storage_
      .emplace(std::string(key), std::make_unique<Value>(std::move(value)))
      .first->second.get();
}
Value* DictValue::Set(std::string_view key, bool value) & {
  return Set(key, Value(value));
}
Value* DictValue::Set(std::string_view key, int value) & {
  return Set(key, Value(value));
}
Value* DictValue::Set(std::string_view key, double value) & {
  return Set(key, Value(value));
}
Value* DictValue::Set(std::string_view key, std::string_view value) & {
  return Set(key, Value(value));
}
Value* DictValue::Set(std::string_view key, const char* value) & {
  return Set(key, Value(value));
}
Value* DictValue::Set(std::string_view key, std::string&& value) & {
  return Set(key, Value(std::move(value)));
}
Value* DictValue::Set(std::string_view key, const std::string& value) & {
  return Set(key, Value(value));
}
Value* DictValue::Set(std::string_view key, DictValue&& value) & {
  return Set(key, Value(std::move(value)));
}
Value* DictValue::Set(std::string_view key, ListValue&& value) & {
  return Set(key, Value(std::move(value)));
}

#define PHI_DICT_CHAIN(Type)                                     \
  DictValue&& DictValue::Set(std::string_view key, Type value)&& { \
    static_cast<DictValue&>(*this).Set(key, std::forward<Type>(value)); \
    return std::move(*this);                                     \
  }
PHI_DICT_CHAIN(Value&&)
PHI_DICT_CHAIN(bool)
PHI_DICT_CHAIN(int)
PHI_DICT_CHAIN(double)
PHI_DICT_CHAIN(std::string_view)
PHI_DICT_CHAIN(const char*)
PHI_DICT_CHAIN(std::string&&)
PHI_DICT_CHAIN(const std::string&)
PHI_DICT_CHAIN(DictValue&&)
PHI_DICT_CHAIN(ListValue&&)
#undef PHI_DICT_CHAIN

bool DictValue::Remove(std::string_view key) {
  auto it = storage_.find(key);
  if (it == storage_.end()) {
    return false;
  }
  storage_.erase(it);
  return true;
}

std::optional<Value> DictValue::Extract(std::string_view key) {
  auto it = storage_.find(key);
  if (it == storage_.end()) {
    return std::nullopt;
  }
  Value value = std::move(*it->second);
  storage_.erase(it);
  return value;
}

const Value* DictValue::FindByDottedPath(std::string_view path) const {
  const DictValue* current = this;
  while (true) {
    const size_t dot = path.find('.');
    if (dot == std::string_view::npos) {
      return current->Find(path);
    }
    current = current->FindDict(path.substr(0, dot));
    if (!current) {
      return nullptr;
    }
    path = path.substr(dot + 1);
  }
}

Value* DictValue::FindByDottedPath(std::string_view path) {
  return const_cast<Value*>(
      static_cast<const DictValue*>(this)->FindByDottedPath(path));
}

const std::string* DictValue::FindStringByDottedPath(
    std::string_view path) const {
  const Value* value = FindByDottedPath(path);
  return value ? value->GetIfString() : nullptr;
}

std::optional<int> DictValue::FindIntByDottedPath(std::string_view path) const {
  const Value* value = FindByDottedPath(path);
  return value ? value->GetIfInt() : std::nullopt;
}

std::optional<bool> DictValue::FindBoolByDottedPath(
    std::string_view path) const {
  const Value* value = FindByDottedPath(path);
  return value ? value->GetIfBool() : std::nullopt;
}

const DictValue* DictValue::FindDictByDottedPath(std::string_view path) const {
  const Value* value = FindByDottedPath(path);
  return value ? value->GetIfDict() : nullptr;
}

const ListValue* DictValue::FindListByDottedPath(std::string_view path) const {
  const Value* value = FindByDottedPath(path);
  return value ? value->GetIfList() : nullptr;
}

bool DictValue::operator==(const DictValue& other) const {
  return std::equal(
      storage_.begin(), storage_.end(), other.storage_.begin(),
      other.storage_.end(), [](const auto& a, const auto& b) {
        return a.first == b.first && *a.second == *b.second;
      });
}

// --- ListValue -------------------------------------------------------------

ListValue::ListValue() = default;
ListValue::ListValue(ListValue&&) noexcept = default;
ListValue& ListValue::operator=(ListValue&&) noexcept = default;
ListValue::~ListValue() = default;

ListValue ListValue::Clone() const {
  ListValue copy;
  copy.storage_.reserve(storage_.size());
  for (const Value& value : storage_) {
    copy.storage_.push_back(value.Clone());
  }
  return copy;
}

bool ListValue::empty() const { return storage_.empty(); }
size_t ListValue::size() const { return storage_.size(); }
ListValue::iterator ListValue::begin() { return storage_.begin(); }
ListValue::iterator ListValue::end() { return storage_.end(); }
ListValue::const_iterator ListValue::begin() const { return storage_.begin(); }
ListValue::const_iterator ListValue::end() const { return storage_.end(); }
Value& ListValue::front() { return storage_.front(); }
const Value& ListValue::front() const { return storage_.front(); }
Value& ListValue::back() { return storage_.back(); }
const Value& ListValue::back() const { return storage_.back(); }
Value& ListValue::operator[](size_t index) { return storage_[index]; }
const Value& ListValue::operator[](size_t index) const {
  return storage_[index];
}
void ListValue::reserve(size_t capacity) { storage_.reserve(capacity); }
void ListValue::clear() { storage_.clear(); }
ListValue::iterator ListValue::erase(const_iterator pos) {
  return storage_.erase(pos);
}
ListValue::iterator ListValue::erase(const_iterator first,
                                     const_iterator last) {
  return storage_.erase(first, last);
}
bool ListValue::contains(const Value& value) const {
  return std::find(storage_.begin(), storage_.end(), value) != storage_.end();
}
bool ListValue::contains(std::string_view value) const {
  return std::any_of(storage_.begin(), storage_.end(), [&](const Value& item) {
    return item.is_string() && item.GetString() == value;
  });
}
bool ListValue::contains(int value) const {
  return std::any_of(storage_.begin(), storage_.end(), [&](const Value& item) {
    return item.is_int() && item.GetInt() == value;
  });
}
ListValue::iterator ListValue::Insert(const_iterator pos, Value&& value) {
  return storage_.insert(pos, std::move(value));
}

void ListValue::Append(Value&& value) & { storage_.push_back(std::move(value)); }
void ListValue::Append(bool value) & { storage_.emplace_back(value); }
void ListValue::Append(int value) & { storage_.emplace_back(value); }
void ListValue::Append(double value) & { storage_.emplace_back(value); }
void ListValue::Append(std::string_view value) & {
  storage_.emplace_back(value);
}
void ListValue::Append(const char* value) & { storage_.emplace_back(value); }
void ListValue::Append(std::string&& value) & {
  storage_.emplace_back(std::move(value));
}
void ListValue::Append(const std::string& value) & {
  storage_.emplace_back(value);
}
void ListValue::Append(DictValue&& value) & {
  storage_.emplace_back(std::move(value));
}
void ListValue::Append(ListValue&& value) & {
  storage_.emplace_back(std::move(value));
}

#define PHI_LIST_CHAIN(Type)                         \
  ListValue&& ListValue::Append(Type value)&& {      \
    static_cast<ListValue&>(*this).Append(std::forward<Type>(value)); \
    return std::move(*this);                         \
  }
PHI_LIST_CHAIN(Value&&)
PHI_LIST_CHAIN(bool)
PHI_LIST_CHAIN(int)
PHI_LIST_CHAIN(double)
PHI_LIST_CHAIN(std::string_view)
PHI_LIST_CHAIN(const char*)
PHI_LIST_CHAIN(std::string&&)
PHI_LIST_CHAIN(const std::string&)
PHI_LIST_CHAIN(DictValue&&)
PHI_LIST_CHAIN(ListValue&&)
#undef PHI_LIST_CHAIN

bool ListValue::operator==(const ListValue& other) const {
  return storage_ == other.storage_;
}

// --- Value -----------------------------------------------------------------

Value::Value() noexcept = default;

Value::Value(Type type) {
  switch (type) {
    case Type::NONE: break;
    case Type::BOOLEAN: data_ = false; break;
    case Type::INTEGER: data_ = 0; break;
    case Type::DOUBLE: data_ = 0.0; break;
    case Type::STRING: data_ = std::string(); break;
    case Type::BINARY: data_ = BlobStorage(); break;
    case Type::DICT: data_ = DictValue(); break;
    case Type::LIST: data_ = ListValue(); break;
  }
}

Value::Value(bool value) : data_(value) {}
Value::Value(int value) : data_(value) {}
// JSON cannot carry these, so base::Value refuses to hold them.
Value::Value(double value) : data_(std::isfinite(value) ? value : 0.0) {}
Value::Value(std::string_view value) : data_(std::string(value)) {}
Value::Value(const char* value) : data_(std::string(value)) {}
Value::Value(std::string&& value) noexcept : data_(std::move(value)) {}
Value::Value(const std::string& value) : data_(value) {}
Value::Value(BlobStorage value) noexcept : data_(std::move(value)) {}
Value::Value(DictValue&& value) noexcept : data_(std::move(value)) {}
Value::Value(ListValue&& value) noexcept : data_(std::move(value)) {}
Value::Value(Value&&) noexcept = default;
Value& Value::operator=(Value&&) noexcept = default;
Value::~Value() = default;

Value Value::Clone() const {
  switch (type()) {
    case Type::NONE: return Value();
    case Type::BOOLEAN: return Value(GetBool());
    case Type::INTEGER: return Value(GetInt());
    case Type::DOUBLE: return Value(std::get<double>(data_));
    case Type::STRING: return Value(GetString());
    case Type::BINARY: return Value(GetBlob());
    case Type::DICT: return Value(GetDict().Clone());
    case Type::LIST: return Value(GetList().Clone());
  }
  return Value();
}

std::optional<bool> Value::GetIfBool() const {
  return is_bool() ? std::optional<bool>(std::get<bool>(data_)) : std::nullopt;
}
std::optional<int> Value::GetIfInt() const {
  return is_int() ? std::optional<int>(std::get<int>(data_)) : std::nullopt;
}
std::optional<double> Value::GetIfDouble() const {
  if (is_double()) {
    return std::get<double>(data_);
  }
  if (is_int()) {
    return static_cast<double>(std::get<int>(data_));
  }
  return std::nullopt;
}
const std::string* Value::GetIfString() const {
  return std::get_if<std::string>(&data_);
}
std::string* Value::GetIfString() { return std::get_if<std::string>(&data_); }
const Value::BlobStorage* Value::GetIfBlob() const {
  return std::get_if<BlobStorage>(&data_);
}
const DictValue* Value::GetIfDict() const {
  return std::get_if<DictValue>(&data_);
}
DictValue* Value::GetIfDict() { return std::get_if<DictValue>(&data_); }
const ListValue* Value::GetIfList() const {
  return std::get_if<ListValue>(&data_);
}
ListValue* Value::GetIfList() { return std::get_if<ListValue>(&data_); }

namespace {

// A wrong-type accessor is a programming error, as it is in base::Value.
template <typename T>
T* Checked(T* value) {
  if (!value) {
    __fastfail(0);
  }
  return value;
}

}  // namespace

bool Value::GetBool() const { return *Checked(std::get_if<bool>(&data_)); }
int Value::GetInt() const { return *Checked(std::get_if<int>(&data_)); }
double Value::GetDouble() const {
  const std::optional<double> value = GetIfDouble();
  if (!value) {
    __fastfail(0);
  }
  return *value;
}
const std::string& Value::GetString() const { return *Checked(GetIfString()); }
std::string& Value::GetString() { return *Checked(GetIfString()); }
const Value::BlobStorage& Value::GetBlob() const {
  return *Checked(GetIfBlob());
}
const DictValue& Value::GetDict() const { return *Checked(GetIfDict()); }
DictValue& Value::GetDict() { return *Checked(GetIfDict()); }
const ListValue& Value::GetList() const { return *Checked(GetIfList()); }
ListValue& Value::GetList() { return *Checked(GetIfList()); }

std::string Value::TakeString() && { return std::move(GetString()); }
DictValue Value::TakeDict() && { return std::move(GetDict()); }
ListValue Value::TakeList() && { return std::move(GetList()); }

bool Value::operator==(const Value& other) const {
  return data_ == other.data_;
}

}  // namespace base

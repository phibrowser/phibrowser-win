// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_VALUES_H_
#define BASE_VALUES_H_

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// The JSON-shaped value tree the client keeps its data in: the same names and
// behaviour as Chromium's base::Value, so files written by either read the
// same.
namespace base {

class Value;
class ListValue;

class DictValue {
 public:
  using Storage = std::map<std::string, std::unique_ptr<Value>, std::less<>>;

  template <bool Const>
  class Iter {
   public:
    using MapIt = std::conditional_t<Const, Storage::const_iterator,
                                     Storage::iterator>;
    using value_type = std::pair<const std::string&,
                                 std::conditional_t<Const, const Value&, Value&>>;
    using iterator_category = std::bidirectional_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = value_type;
    Iter() = default;
    explicit Iter(MapIt it) : it_(it) {}
    template <bool C = Const>
      requires C
    Iter(const Iter<false>& other) : it_(other.it_) {}
    reference operator*() const { return {it_->first, *it_->second}; }
    Iter& operator++() { ++it_; return *this; }
    Iter operator++(int) { Iter old = *this; ++it_; return old; }
    Iter& operator--() { --it_; return *this; }
    bool operator==(const Iter& other) const { return it_ == other.it_; }

    struct Arrow { value_type pair; value_type* operator->() { return &pair; } };
    Arrow operator->() const { return Arrow{**this}; }

   private:
    template <bool>
    friend class Iter;
    friend class DictValue;
    MapIt it_{};
  };
  using iterator = Iter<false>;
  using const_iterator = Iter<true>;

  DictValue();
  DictValue(DictValue&&) noexcept;
  DictValue& operator=(DictValue&&) noexcept;
  DictValue(const DictValue&) = delete;
  DictValue& operator=(const DictValue&) = delete;
  ~DictValue();

  DictValue Clone() const;
  bool empty() const { return storage_.empty(); }
  size_t size() const { return storage_.size(); }
  iterator begin() { return iterator(storage_.begin()); }
  iterator end() { return iterator(storage_.end()); }
  const_iterator begin() const { return const_iterator(storage_.begin()); }
  const_iterator end() const { return const_iterator(storage_.end()); }
  const_iterator cbegin() const { return begin(); }
  const_iterator cend() const { return end(); }
  iterator find(std::string_view key) { return iterator(storage_.find(key)); }
  const_iterator find(std::string_view key) const {
    return const_iterator(storage_.find(key));
  }
  iterator erase(iterator pos) { return iterator(storage_.erase(pos.it_)); }
  void clear() { storage_.clear(); }
  bool contains(std::string_view key) const { return storage_.contains(key); }
  void Merge(DictValue dict);

  const Value* Find(std::string_view key) const;
  Value* Find(std::string_view key);
  std::optional<bool> FindBool(std::string_view key) const;
  std::optional<int> FindInt(std::string_view key) const;
  std::optional<double> FindDouble(std::string_view key) const;
  const std::string* FindString(std::string_view key) const;
  std::string* FindString(std::string_view key);
  const std::vector<uint8_t>* FindBlob(std::string_view key) const;
  const DictValue* FindDict(std::string_view key) const;
  DictValue* FindDict(std::string_view key);
  const ListValue* FindList(std::string_view key) const;
  ListValue* FindList(std::string_view key);
  DictValue* EnsureDict(std::string_view key);
  ListValue* EnsureList(std::string_view key);

  Value* Set(std::string_view key, Value&& value) &;
  Value* Set(std::string_view key, bool value) &;
  template <typename T>
    requires std::is_pointer_v<T>
  Value* Set(std::string_view key, T value) & = delete;
  Value* Set(std::string_view key, int value) &;
  Value* Set(std::string_view key, double value) &;
  Value* Set(std::string_view key, std::string_view value) &;
  Value* Set(std::string_view key, const char* value) &;
  Value* Set(std::string_view key, std::string&& value) &;
  Value* Set(std::string_view key, const std::string& value) &;
  Value* Set(std::string_view key, DictValue&& value) &;
  Value* Set(std::string_view key, ListValue&& value) &;

  DictValue&& Set(std::string_view key, Value&& value) &&;
  DictValue&& Set(std::string_view key, bool value) &&;
  template <typename T>
    requires std::is_pointer_v<T>
  DictValue&& Set(std::string_view key, T value) && = delete;
  DictValue&& Set(std::string_view key, int value) &&;
  DictValue&& Set(std::string_view key, double value) &&;
  DictValue&& Set(std::string_view key, std::string_view value) &&;
  DictValue&& Set(std::string_view key, const char* value) &&;
  DictValue&& Set(std::string_view key, std::string&& value) &&;
  DictValue&& Set(std::string_view key, const std::string& value) &&;
  DictValue&& Set(std::string_view key, DictValue&& value) &&;
  DictValue&& Set(std::string_view key, ListValue&& value) &&;

  bool Remove(std::string_view key);
  std::optional<Value> Extract(std::string_view key);

  const Value* FindByDottedPath(std::string_view path) const;
  Value* FindByDottedPath(std::string_view path);
  const std::string* FindStringByDottedPath(std::string_view path) const;
  std::optional<int> FindIntByDottedPath(std::string_view path) const;
  std::optional<bool> FindBoolByDottedPath(std::string_view path) const;
  const DictValue* FindDictByDottedPath(std::string_view path) const;
  const ListValue* FindListByDottedPath(std::string_view path) const;

  bool operator==(const DictValue& other) const;

 private:
  Storage storage_;
};

class ListValue {
 public:
  using iterator = std::vector<Value>::iterator;
  using const_iterator = std::vector<Value>::const_iterator;

  ListValue();
  ListValue(ListValue&&) noexcept;
  ListValue& operator=(ListValue&&) noexcept;
  ListValue(const ListValue&) = delete;
  ListValue& operator=(const ListValue&) = delete;
  ~ListValue();

  ListValue Clone() const;
  bool empty() const;
  size_t size() const;
  iterator begin();
  iterator end();
  const_iterator begin() const;
  const_iterator end() const;
  const_iterator cbegin() const { return begin(); }
  const_iterator cend() const { return end(); }
  Value& front();
  const Value& front() const;
  Value& back();
  const Value& back() const;
  Value& operator[](size_t index);
  const Value& operator[](size_t index) const;
  void reserve(size_t capacity);
  void clear();
  iterator erase(const_iterator pos);
  iterator erase(const_iterator first, const_iterator last);
  bool contains(const Value& value) const;
  bool contains(std::string_view value) const;
  bool contains(int value) const;
  iterator Insert(const_iterator pos, Value&& value);

  template <typename Predicate>
  size_t EraseIf(Predicate predicate);

  void Append(Value&& value) &;
  void Append(bool value) &;
  template <typename T>
    requires std::is_pointer_v<T>
  void Append(T value) & = delete;
  void Append(int value) &;
  void Append(double value) &;
  void Append(std::string_view value) &;
  void Append(const char* value) &;
  void Append(std::string&& value) &;
  void Append(const std::string& value) &;
  void Append(DictValue&& value) &;
  void Append(ListValue&& value) &;

  ListValue&& Append(Value&& value) &&;
  ListValue&& Append(bool value) &&;
  ListValue&& Append(int value) &&;
  ListValue&& Append(double value) &&;
  ListValue&& Append(std::string_view value) &&;
  ListValue&& Append(const char* value) &&;
  ListValue&& Append(std::string&& value) &&;
  ListValue&& Append(const std::string& value) &&;
  ListValue&& Append(DictValue&& value) &&;
  ListValue&& Append(ListValue&& value) &&;

  bool operator==(const ListValue& other) const;

 private:
  std::vector<Value> storage_;
};

class Value {
 public:
  enum class Type : unsigned char {
    NONE = 0, BOOLEAN, INTEGER, DOUBLE, STRING, BINARY, DICT, LIST,
  };
  using BlobStorage = std::vector<uint8_t>;
  using Dict = DictValue;
  using List = ListValue;

  Value() noexcept;
  explicit Value(Type type);
  explicit Value(bool value);
  template <typename T>
    requires std::is_pointer_v<T>
  explicit Value(T value) = delete;
  explicit Value(int value);
  explicit Value(double value);
  explicit Value(std::string_view value);
  explicit Value(const char* value);
  explicit Value(std::string&& value) noexcept;
  explicit Value(const std::string& value);
  explicit Value(BlobStorage value) noexcept;
  explicit Value(DictValue&& value) noexcept;
  explicit Value(ListValue&& value) noexcept;
  Value(Value&&) noexcept;
  Value& operator=(Value&&) noexcept;
  Value(const Value&) = delete;
  Value& operator=(const Value&) = delete;
  ~Value();

  Value Clone() const;
  Type type() const { return static_cast<Type>(data_.index()); }
  bool is_none() const { return type() == Type::NONE; }
  bool is_bool() const { return type() == Type::BOOLEAN; }
  bool is_int() const { return type() == Type::INTEGER; }
  bool is_double() const { return type() == Type::DOUBLE; }
  bool is_string() const { return type() == Type::STRING; }
  bool is_blob() const { return type() == Type::BINARY; }
  bool is_dict() const { return type() == Type::DICT; }
  bool is_list() const { return type() == Type::LIST; }

  std::optional<bool> GetIfBool() const;
  std::optional<int> GetIfInt() const;
  std::optional<double> GetIfDouble() const;
  const std::string* GetIfString() const;
  std::string* GetIfString();
  const BlobStorage* GetIfBlob() const;
  const DictValue* GetIfDict() const;
  DictValue* GetIfDict();
  const ListValue* GetIfList() const;
  ListValue* GetIfList();

  bool GetBool() const;
  int GetInt() const;
  double GetDouble() const;
  const std::string& GetString() const;
  std::string& GetString();
  const BlobStorage& GetBlob() const;
  const DictValue& GetDict() const;
  DictValue& GetDict();
  const ListValue& GetList() const;
  ListValue& GetList();

  std::string TakeString() &&;
  DictValue TakeDict() &&;
  ListValue TakeList() &&;

  bool operator==(const Value& other) const;

 private:
  std::variant<std::monostate, bool, int, double, std::string, BlobStorage,
               DictValue, ListValue>
      data_;
};

template <typename Predicate>
size_t ListValue::EraseIf(Predicate predicate) {
  return std::erase_if(storage_, predicate);
}

// Lets a writer take any of the three container types.
class ValueView {
 public:
  ValueView(const Value& value) : value_(&value) {}  // NOLINT
  ValueView(const DictValue& value) : dict_(&value) {}  // NOLINT
  ValueView(const ListValue& value) : list_(&value) {}  // NOLINT
  const Value* value() const { return value_; }
  const DictValue* dict() const { return dict_; }
  const ListValue* list() const { return list_; }

 private:
  const Value* value_ = nullptr;
  const DictValue* dict_ = nullptr;
  const ListValue* list_ = nullptr;
};

}  // namespace base

#endif  // BASE_VALUES_H_

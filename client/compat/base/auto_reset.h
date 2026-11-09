// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_AUTO_RESET_H_
#define BASE_AUTO_RESET_H_

#include <utility>

namespace base {

// Sets a variable for the length of a scope and puts the old value back.
template <typename T>
class AutoReset {
 public:
  template <typename U>
  AutoReset(T* scoped_variable, U&& new_value)
      : scoped_variable_(scoped_variable),
        original_value_(
            std::exchange(*scoped_variable, std::forward<U>(new_value))) {}
  explicit AutoReset(T* scoped_variable)
      : scoped_variable_(scoped_variable), original_value_(*scoped_variable) {}
  AutoReset(const AutoReset&) = delete;
  AutoReset& operator=(const AutoReset&) = delete;
  ~AutoReset() { *scoped_variable_ = std::move(original_value_); }

 private:
  T* scoped_variable_;
  T original_value_;
};

}  // namespace base

#endif  // BASE_AUTO_RESET_H_

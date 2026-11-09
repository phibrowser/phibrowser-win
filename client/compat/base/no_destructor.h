// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_NO_DESTRUCTOR_H_
#define BASE_NO_DESTRUCTOR_H_

#include <new>
#include <utility>

namespace base {

// Builds a T once and never destroys it.
template <typename T>
class NoDestructor {
 public:
  template <typename... Args>
  explicit NoDestructor(Args&&... args) {
    new (storage_) T(std::forward<Args>(args)...);
  }
  explicit NoDestructor(const T& x) { new (storage_) T(x); }
  explicit NoDestructor(T&& x) { new (storage_) T(std::move(x)); }
  NoDestructor(const NoDestructor&) = delete;
  NoDestructor& operator=(const NoDestructor&) = delete;
  ~NoDestructor() = default;

  const T& operator*() const { return *get(); }
  T& operator*() { return *get(); }
  const T* operator->() const { return get(); }
  T* operator->() { return get(); }
  const T* get() const { return std::launder(reinterpret_cast<const T*>(storage_)); }
  T* get() { return std::launder(reinterpret_cast<T*>(storage_)); }

 private:
  alignas(T) char storage_[sizeof(T)];
};

}  // namespace base

#endif  // BASE_NO_DESTRUCTOR_H_

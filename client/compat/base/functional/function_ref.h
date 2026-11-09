// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_FUNCTIONAL_FUNCTION_REF_H_
#define BASE_FUNCTIONAL_FUNCTION_REF_H_

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace base {

template <typename Signature>
class FunctionRef;

// A non-owning reference to something callable, valid for the call it is
// passed to.
template <typename R, typename... Args>
class FunctionRef<R(Args...)> {
 public:
  template <typename F>
    requires(!std::is_same_v<std::remove_cvref_t<F>, FunctionRef> &&
             std::is_invocable_r_v<R, F&, Args...>)
  FunctionRef(F&& f)  // NOLINT: implicit by design.
      : object_(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
        call_([](void* object, Args... args) -> R {
          return std::invoke(
              *static_cast<std::add_pointer_t<F>>(object),
              std::forward<Args>(args)...);
        }) {}

  R operator()(Args... args) const {
    return call_(object_, std::forward<Args>(args)...);
  }

 private:
  void* object_;
  R (*call_)(void*, Args...);
};

}  // namespace base

#endif  // BASE_FUNCTIONAL_FUNCTION_REF_H_

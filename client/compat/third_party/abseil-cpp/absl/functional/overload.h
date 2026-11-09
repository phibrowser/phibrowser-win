// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef ABSL_FUNCTIONAL_OVERLOAD_H_
#define ABSL_FUNCTIONAL_OVERLOAD_H_

namespace absl {

template <typename... Ts>
struct Overload : Ts... {
  using Ts::operator()...;
};
template <typename... Ts>
Overload(Ts...) -> Overload<Ts...>;

}  // namespace absl

#endif  // ABSL_FUNCTIONAL_OVERLOAD_H_

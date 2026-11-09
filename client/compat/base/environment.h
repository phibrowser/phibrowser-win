// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_ENVIRONMENT_H_
#define BASE_ENVIRONMENT_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace base {

class Environment {
 public:
  static std::unique_ptr<Environment> Create() {
    return std::make_unique<Environment>();
  }
  std::optional<std::string> GetVar(std::string_view name);
};

}  // namespace base

#endif  // BASE_ENVIRONMENT_H_

// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_UUID_H_
#define BASE_UUID_H_

#include <string>

namespace base {

class Uuid {
 public:
  static Uuid GenerateRandomV4();
  const std::string& AsLowercaseString() const { return text_; }

 private:
  std::string text_;
};

}  // namespace base

#endif  // BASE_UUID_H_

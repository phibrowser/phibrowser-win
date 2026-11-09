// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_PATH_SERVICE_H_
#define BASE_PATH_SERVICE_H_

#include "base/files/file_path.h"

namespace base {

enum BasePathKey { DIR_EXE };

class PathService {
 public:
  static bool Get(int key, FilePath* path);
};

}  // namespace base

#endif  // BASE_PATH_SERVICE_H_

// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_WIN_WINDOWS_VERSION_H_
#define BASE_WIN_WINDOWS_VERSION_H_

namespace base::win {

class OSInfo {
 public:
  struct VersionNumber {
    int major = 0;
    int minor = 0;
    int build = 0;
    int patch = 0;
  };
  static OSInfo* GetInstance();
  const VersionNumber& version_number() const { return version_; }

 private:
  OSInfo();
  VersionNumber version_;
};

}  // namespace base::win

#endif  // BASE_WIN_WINDOWS_VERSION_H_

// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_FILES_FILE_PATH_H_
#define BASE_FILES_FILE_PATH_H_

#include <ostream>
#include <string>
#include <string_view>

#define FILE_PATH_LITERAL(x) L##x

namespace base {

class FilePath {
 public:
  using StringType = std::wstring;
  using StringViewType = std::wstring_view;

  FilePath() = default;
  explicit FilePath(StringViewType path) : path_(path) {}

  const StringType& value() const { return path_; }
  bool empty() const { return path_.empty(); }
  bool IsAbsolute() const;
  FilePath Append(StringViewType component) const;
  FilePath AppendASCII(std::string_view component) const;
  FilePath NormalizePathSeparators() const;
  std::string AsUTF8Unsafe() const;

 private:
  StringType path_;
};

std::ostream& operator<<(std::ostream& out, const FilePath& path);

}  // namespace base

#endif  // BASE_FILES_FILE_PATH_H_

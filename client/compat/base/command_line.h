// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_COMMAND_LINE_H_
#define BASE_COMMAND_LINE_H_

#include <map>
#include <string>
#include <string_view>

namespace base {

// The process's command line, parsed the way Chromium parses it.
class CommandLine {
 public:
  static void Init(int argc, const char* const* argv);
  static CommandLine* ForCurrentProcess();

  bool HasSwitch(std::string_view name) const;
  std::string GetSwitchValueASCII(std::string_view name) const;
  std::wstring GetSwitchValueNative(std::string_view name) const;
  void AppendSwitch(std::string_view name);
  void AppendSwitchASCII(std::string_view name, std::string_view value);

 private:
  std::map<std::string, std::wstring, std::less<>> switches_;
};

}  // namespace base

#endif  // BASE_COMMAND_LINE_H_

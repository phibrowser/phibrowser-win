// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_LOGGING_H_
#define BASE_LOGGING_H_

#include <intrin.h>

#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

// The client's log lines, in the format chrome_debug.log uses, written to the
// same file as the framework's.
namespace logging {

using LogSeverity = int;
inline constexpr LogSeverity LOGGING_INFO = 0;
inline constexpr LogSeverity LOGGING_WARNING = 1;
inline constexpr LogSeverity LOGGING_ERROR = 2;
inline constexpr LogSeverity LOGGING_FATAL = 3;
inline constexpr LogSeverity LOGGING_NUM_SEVERITIES = 4;

bool ShouldLog(LogSeverity severity);
void SetMinLogLevel(int level);

class LogMessage {
 public:
  LogMessage(const char* file, int line, LogSeverity severity);
  LogMessage(const LogMessage&) = delete;
  LogMessage& operator=(const LogMessage&) = delete;
  ~LogMessage();
  std::ostream& stream() { return stream_; }

 private:
  const char* file_;
  int line_;
  LogSeverity severity_;
  std::ostringstream stream_;
};

std::ostream& NullStream();

// Discards a stream expression's value, for the conditional in LOG.
struct Voidify {
  void operator&(std::ostream&) {}
};

[[noreturn]] void CheckFailed(const char* file, int line, const char* what,
                              const std::string& message);

// Collects a failed CHECK's message and stops the process when it ends.
class CheckMessage {
 public:
  CheckMessage(const char* file, int line, const char* what)
      : file_(file), line_(line), what_(what) {}
  [[noreturn]] ~CheckMessage() {
    CheckFailed(file_, line_, what_, stream_.str());
  }
  std::ostream& stream() { return stream_; }

 private:
  const char* file_;
  int line_;
  const char* what_;
  std::ostringstream stream_;
};

}  // namespace logging

namespace std {
ostream& operator<<(ostream& out, wstring_view text);
inline ostream& operator<<(ostream& out, const wstring& text) {
  return out << wstring_view(text);
}
}  // namespace std

#define LOG(severity)                                                \
  !::logging::ShouldLog(::logging::LOGGING_##severity)               \
      ? (void)0                                                      \
      : ::logging::Voidify() &                                       \
            ::logging::LogMessage(__FILE__, __LINE__,                \
                                  ::logging::LOGGING_##severity)     \
                .stream()
#define LOG_IF(severity, condition)                                  \
  !((condition) && ::logging::ShouldLog(::logging::LOGGING_##severity)) \
      ? (void)0                                                      \
      : ::logging::Voidify() &                                       \
            ::logging::LogMessage(__FILE__, __LINE__,                \
                                  ::logging::LOGGING_##severity)     \
                .stream()
#define PHI_LOG_OFF true ? (void)0 : ::logging::Voidify() & ::logging::NullStream()
#define VLOG(level) PHI_LOG_OFF
#define DVLOG(level) PHI_LOG_OFF
#define DLOG(severity) PHI_LOG_OFF
#define PLOG(severity) LOG(severity)
#define VLOG_IS_ON(level) false

#define CHECK(condition)                                             \
  (condition) ? (void)0                                              \
              : ::logging::Voidify() &                               \
                    ::logging::CheckMessage(__FILE__, __LINE__, #condition) \
                        .stream()
#define PHI_CHECK_OP(a, op, b) CHECK((a)op(b))
#define CHECK_EQ(a, b) PHI_CHECK_OP(a, ==, b)
#define CHECK_NE(a, b) PHI_CHECK_OP(a, !=, b)
#define CHECK_LE(a, b) PHI_CHECK_OP(a, <=, b)
#define CHECK_LT(a, b) PHI_CHECK_OP(a, <, b)
#define CHECK_GE(a, b) PHI_CHECK_OP(a, >=, b)
#define CHECK_GT(a, b) PHI_CHECK_OP(a, >, b)
#define DCHECK(condition) PHI_LOG_OFF
#define DCHECK_EQ(a, b) PHI_LOG_OFF
#define DCHECK_NE(a, b) PHI_LOG_OFF
#define DCHECK_LE(a, b) PHI_LOG_OFF
#define DCHECK_LT(a, b) PHI_LOG_OFF
#define DCHECK_GE(a, b) PHI_LOG_OFF
#define DCHECK_GT(a, b) PHI_LOG_OFF
#define DCHECK_IS_ON() 0
#define NOTREACHED() ::logging::CheckMessage(__FILE__, __LINE__, "NOTREACHED").stream()

#endif  // BASE_LOGGING_H_

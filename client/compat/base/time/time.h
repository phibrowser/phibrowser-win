// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_TIME_TIME_H_
#define BASE_TIME_TIME_H_

#include <cstdint>

namespace base {

class Time {
 public:
  struct Exploded {
    int year = 0;
    int month = 0;
    int day_of_week = 0;
    int day_of_month = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int millisecond = 0;
  };
  static Time Now();
  // False when `exploded` is not a real date.
  static bool FromUTCExploded(const Exploded& exploded, Time* time);
  int64_t InMillisecondsSinceUnixEpoch() const;
  double InSecondsFSinceUnixEpoch() const;

 private:
  int64_t us_since_unix_epoch_ = 0;
};

}  // namespace base

#endif  // BASE_TIME_TIME_H_

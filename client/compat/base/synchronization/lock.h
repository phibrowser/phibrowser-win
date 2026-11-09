// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_SYNCHRONIZATION_LOCK_H_
#define BASE_SYNCHRONIZATION_LOCK_H_

#include <mutex>

namespace base {

class Lock {
 public:
  Lock() = default;
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
  void Acquire() { mutex_.lock(); }
  void Release() { mutex_.unlock(); }
  bool Try() { return mutex_.try_lock(); }
  void AssertAcquired() const {}

 private:
  std::mutex mutex_;
};

class AutoLock {
 public:
  explicit AutoLock(Lock& lock) : lock_(lock) { lock_.Acquire(); }
  AutoLock(const AutoLock&) = delete;
  AutoLock& operator=(const AutoLock&) = delete;
  ~AutoLock() { lock_.Release(); }

 private:
  Lock& lock_;
};

class AutoUnlock {
 public:
  explicit AutoUnlock(Lock& lock) : lock_(lock) { lock_.Release(); }
  AutoUnlock(const AutoUnlock&) = delete;
  AutoUnlock& operator=(const AutoUnlock&) = delete;
  ~AutoUnlock() { lock_.Acquire(); }

 private:
  Lock& lock_;
};

}  // namespace base

#endif  // BASE_SYNCHRONIZATION_LOCK_H_

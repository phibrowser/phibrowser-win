// Copyright 2026 Phinomenon Inc.
//
// Use of this source code is governed by an Apache license that can be
// found in the LICENSE file.

#ifndef BASE_COMPILER_SPECIFIC_H_
#define BASE_COMPILER_SPECIFIC_H_

#define UNSAFE_BUFFERS(...) __VA_ARGS__
#define UNSAFE_TODO(...) __VA_ARGS__
#define NOINLINE __declspec(noinline)
#define ALWAYS_INLINE __forceinline
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)

#endif  // BASE_COMPILER_SPECIFIC_H_

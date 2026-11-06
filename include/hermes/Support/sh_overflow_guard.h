/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_SUPPORT_SH_OVERFLOW_GUARD_H
#define HERMES_SUPPORT_SH_OVERFLOW_GUARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Native stack bounds used to detect native stack overflow. A C struct so
/// that native code can check them inline.
typedef struct SHOverflowGuard {
  /// Upper bound on the stack, null if currently unknown.
  const void *nativeStackHigh;
  /// Usable stack size below nativeStackHigh, with the gap already
  /// subtracted.
  size_t nativeStackSize;
} SHOverflowGuard;

/// Clear the bounds in \p guard, forcing recomputation on the next check.
static inline void sh_overflow_guard_clear(SHOverflowGuard *guard) {
  guard->nativeStackHigh = NULL;
  guard->nativeStackSize = 0;
}

/// \return true if \p sp is outside the bounds in \p guard: the stack is
/// overflowing, the bounds are unknown, or execution changed threads.
/// If sp > nativeStackHigh, the unsigned subtraction wraps around to a value
/// larger than nativeStackSize, because the stack must fit below
/// nativeStackHigh.
static inline bool sh_overflow_guard_out_of_bounds(
    const SHOverflowGuard *guard,
    const void *sp) {
  return (uintptr_t)guard->nativeStackHigh - (uintptr_t)sp >
      guard->nativeStackSize;
}

/// Recompute the bounds in \p guard for the current thread, leaving \p gap
/// bytes of headroom. Only available with HERMES_CHECK_NATIVE_STACK.
/// \return true if the caller's stack is out of the new bounds.
bool sh_overflow_guard_update(SHOverflowGuard *guard, unsigned gap);

#ifdef __cplusplus
}
#endif

#endif // HERMES_SUPPORT_SH_OVERFLOW_GUARD_H

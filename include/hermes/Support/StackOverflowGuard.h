/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
///
/// OverflowGuard is designed to catch imminent native stack overflow. It does
/// this using two different heuristics, depending on HERMES_CHECK_NATIVE_STACK.
///
/// If HERMES_CHECK_NATIVE_STACK is defined, it will use real stack checking. It
/// calls into platform-specific APIs to obtain the upper stack bound of the
/// currently executing thread. It will then check the current stack address
/// against the upper limit, along with some user-defined gap. Overflow is
/// reported if the current stack address is close enough to the upper bound,
/// accounting for the supplied gap value.
///
/// If HERMES_CHECK_NATIVE_STACK is not defined, a simple depth counter is used.
/// Every time a recursive call is made, the counter should be bumped. Overflow
/// is reported if the counter reaches some user-defined max.
///
//===----------------------------------------------------------------------===//

#ifndef HERMES_SUPPORT_STACKOVERFLOWGUARD_H
#define HERMES_SUPPORT_STACKOVERFLOWGUARD_H

#ifdef _MSC_VER
#include <intrin.h>
#endif

#include "hermes/Support/sh_overflow_guard.h"

#include "llvh/Support/Compiler.h"

#include <cstddef>
#include <cstdint>

namespace hermes {

#ifdef HERMES_CHECK_NATIVE_STACK

class StackOverflowGuard {
  StackOverflowGuard(unsigned stackGap, SHOverflowGuard bounds)
      : bounds(bounds), nativeStackGap(stackGap) {}

 public:
  /// Current stack bounds.
  SHOverflowGuard bounds;
  /// Native stack remaining before assuming overflow.
  unsigned nativeStackGap;

  /// \return a guard with \p stackGap, starting from \p bounds (unknown by
  ///   default).
  static StackOverflowGuard nativeStackGuard(
      unsigned stackGap,
      SHOverflowGuard bounds = {nullptr, 0}) {
    return StackOverflowGuard(stackGap, bounds);
  }

  /// \return true if the native stack is overflowing the bounds of the
  ///   current thread. Updates the stack bounds if the thread which Runtime
  ///   is executing on changes.
  inline bool isOverflowing() {
    return isOverflowing(&bounds, nativeStackGap);
  }

  /// Same as the member \c isOverflowing, for bounds stored elsewhere.
  /// \p guard is updated when the bounds are recomputed with \p gap.
  static inline bool isOverflowing(SHOverflowGuard *guard, unsigned gap) {
#ifdef _MSC_VER
    void *stackPointer = _AddressOfReturnAddress();
#else
    void *stackPointer = __builtin_frame_address(0);
#endif
    if (LLVM_LIKELY(!sh_overflow_guard_out_of_bounds(guard, stackPointer))) {
      // Fast path: quickly check the stored stack bounds.
      // NOTE: It is possible to have a false negative here (highly unlikely).
      // If the program creates many threads and destroys them, a new
      // thread's stack could overlap the saved stack so we'd be checking
      // against the wrong bounds.
      return false;
    }
    // Slow path: might be overflowing, but update the stack bounds first
    // in case execution has changed threads.
    return sh_overflow_guard_update(guard, gap);
  }

  /// Clear the native stack bounds in \p guard and force recomputation.
  static inline void clearStackBounds(SHOverflowGuard *guard) {
    sh_overflow_guard_clear(guard);
  }
};

#else

class StackOverflowGuard {
  explicit StackOverflowGuard(size_t max) : maxCallDepth(max) {}

 public:
  /// This is how many recursive calls have already been made.
  /// This grows towards maxCallDepth.
  size_t callDepth{0};
  /// If callDepth exceeds this value, it is considered overflow.
  size_t maxCallDepth;

  static StackOverflowGuard depthCounterGuard(unsigned stackGap) {
    return StackOverflowGuard(stackGap);
  }

  /// \return true if \c callDepth has exceeded the budget set by \c
  /// maxCallDepth.
  inline bool isOverflowing() {
    return callDepth > maxCallDepth;
  }

  class CallFrameRAII;
};

/// A simple RAII class to help users increment/decrement frame depth if
/// necessary.
class [[nodiscard]] StackOverflowGuard::CallFrameRAII {
  StackOverflowGuard &guard_;

 public:
  explicit CallFrameRAII(StackOverflowGuard &guard) : guard_(guard) {
    ++guard_.callDepth;
  }

  ~CallFrameRAII() {
    --guard_.callDepth;
  }
};

#endif

} // namespace hermes

#endif // HERMES_SUPPORT_STACKOVERFLOWGUARD_H

/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Support/StackOverflowGuard.h"
#include "hermes/Support/OSCompat.h"

#ifdef HERMES_CHECK_NATIVE_STACK

extern "C" bool sh_overflow_guard_update(SHOverflowGuard *guard, unsigned gap) {
  auto [highPtr, size] = hermes::oscompat::thread_stack_bounds(gap);
  guard->nativeStackHigh = highPtr;
  guard->nativeStackSize = size;
#ifdef _MSC_VER
  void *stackPointer = _AddressOfReturnAddress();
#else
  void *stackPointer = __builtin_frame_address(0);
#endif
  return LLVM_UNLIKELY(sh_overflow_guard_out_of_bounds(guard, stackPointer));
}

#endif

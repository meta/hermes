/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JIT_JITCOUNTERS_H
#define HERMES_VM_JIT_JITCOUNTERS_H

namespace hermes {
namespace vm {

/// List of counters that can be incremented from JIT emitted code. The
/// counters themselves are part of the ABI between the VM and emitted code
/// (an array of uint64_t reachable from Runtime), not specific to any
/// backend.
#define JIT_COUNTERS(X)                                           \
  X(NumCall)                                                      \
  X(NumCallSlow)                                                  \
  /* Counts every threshold crossing, including after budget      \
   * exhaustion and from retired bodies (the gates run inside     \
   * considerRecompile), so it grows where it used to plateau. */ \
  X(NumRecompileChecks)                                           \
  X(NumRecompiles)                                                \
  /* PutByVal sites whose helper slot the demotion pass flipped   \
   * from the recording helper to the plain one. Retirement-time  \
   * flips are bookkeeping, not policy, and are NOT counted. */   \
  X(NumByValDemotions)

/// Enum with an entry for each JIT counter. This is used to index into the
/// list of counters.
enum class JitCounter : unsigned {
#define COUNTER_NAME(name) name,
  JIT_COUNTERS(COUNTER_NAME)
#undef COUNTER_NAME
      _Last,
};

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_JIT_JITCOUNTERS_H

/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_VMEXPERIMENTS_H
#define HERMES_VM_VMEXPERIMENTS_H

#include <cstdint>

namespace hermes {
namespace vm {
namespace experiments {

/// List of active experiments, corresponding to
/// Runtime::getVMExperimentFlags(). If possible, skip the commented values to
/// avoid unexpected conflicts.
enum {
  Default = 0,
  MAdviseSequential = 1 << 2,
  MAdviseRandom = 1 << 3,
  MAdviseStringsSequential = 1 << 4,
  MAdviseStringsRandom = 1 << 5,
  MAdviseStringsWillNeed = 1 << 6,
  VerifyBytecodeChecksum = 1 << 7,
  IgnoreMemoryWarnings = 1 << 9,
  // HadesCompaction = 1 << 10,
  // GenGC = 1 << 11,
  // HadesTimedIncremental = 1 << 12,
  CrashTrace = 1 << 13,
  // JobQueue = 1 << 14,
  IdleTimeGC = 1 << 15,
  HadesYGSizeByCPUTime = 1 << 16,
};

/// Set of flags for active VM experiments.
using VMExperimentFlags = uint32_t;

} // namespace experiments
} // namespace vm
} // namespace hermes

#endif // HERMES_VM_VMEXPERIMENTS_H

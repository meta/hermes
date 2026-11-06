/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_RUNTIMEFLAGS_H
#define HERMES_VM_RUNTIMEFLAGS_H

#include "hermes/VM/sh_config.h"

#include "llvh/ADT/StringRef.h"
#include "llvh/Support/CommandLine.h"

#include "hermes/ConsoleHost/ConsoleHost.h"
#include "hermes/Public/RuntimeConfig.h"
#include "hermes/Support/MemorySizeParser.h"
#include "hermes/Support/RandomSeedParser.h"
#include "hermes/Utils/CompilerRuntimeFlags.h"

#include <string>

namespace hermes::cli {

/// All command line runtime options relevant to the VM, excluding options
/// shared between the compiler and VM. Instantiating this struct registers
/// the options in the global state of llvh::CommandLine. If this library is
/// linked statically, it shares the same llvh::CommandLine global state with
/// its user. If linked dynamically, it has its own global state, since LLVH is
/// not dynamically exported.
///
/// This struct can be used as a header-only dependency and instantiated by
/// any client needing to parse these command line options.
struct VMOnlyRuntimeFlags {
  llvh::cl::OptionCategory GCCategory{
      "Garbage Collector Options",
      "These control various parts of the GC."};

  llvh::cl::OptionCategory RuntimeCategory{
      "Runtime Options",
      "These control aspects of the VM when it is being run."};

  llvh::cl::opt<double> GCSanitizeRate{
      "gc-sanitize-handles",
      llvh::cl::desc(
          "A probability between 0 and 1 inclusive which indicates the chance "
          "that we do handle sanitization at a given allocation. "
          "Sanitization moves the heap to a new location. With ASAN "
          "enabled, this causes accesses via stale pointers into the heap to "
          "be sanitized."),
      llvh::cl::cat(GCCategory),
#if HERMESVM_SANITIZE_HANDLES != 0
      llvh::cl::init(0.01)
#else
      llvh::cl::init(0.0),
      llvh::cl::Hidden
#endif
  };

  llvh::cl::opt<int64_t, false, RandomSeedParser> GCSanitizeRandomSeed{
      "gc-sanitize-handles-random-seed",
      llvh::cl::desc(
          "A number used as a seed to the random engine for handle sanitization."
          "A negative value means to choose the seed at random"),
      llvh::cl::cat(GCCategory),
      llvh::cl::init(-1)
#if HERMESVM_SANITIZE_HANDLES == 0
          ,
      llvh::cl::Hidden
#endif
  };

  llvh::cl::opt<MemorySize, false, MemorySizeParser> InitHeapSize{
      "gc-init-heap",
      llvh::cl::desc("Initial heap size.  Format: <unsigned>{{K,M,G}{iB}"),
      llvh::cl::cat(GCCategory),
      llvh::cl::init(MemorySize{vm::GCConfig::getDefaultInitHeapSize()})};

  llvh::cl::opt<double> OccupancyTarget{
      "occupancy-target",
      llvh::cl::desc(
          "Sizing heuristic: fraction of heap to be occupied by live data."),
      llvh::cl::cat(GCCategory),
      llvh::cl::init(vm::GCConfig::getDefaultOccupancyTarget())};

  llvh::cl::opt<bool> GCPrintStats{
      "gc-print-stats",
      llvh::cl::desc("Output summary garbage collection statistics at exit"),
      llvh::cl::cat(GCCategory),
      llvh::cl::init(false)};

  llvh::cl::opt<ExecuteOptions::SampleProfilingMode> SampleProfiling{
      "sample-profiling",
      llvh::cl::init(ExecuteOptions::SampleProfilingMode::None),
      llvh::cl::desc("Enable sampling profiler"),
      values(
          clEnumValN(
              ExecuteOptions::SampleProfilingMode::Chrome,
              "chrome",
              "Dump profile in Chrome DevTools format"),
          clEnumValN(
              ExecuteOptions::SampleProfilingMode::Tracery,
              "tracery",
              "Dump profile in Tracery format")),
      llvh::cl::cat(RuntimeCategory)};

  llvh::cl::opt<double> SampleProfilingFreq{
      "sample-profiling-freq",
      llvh::cl::init(100),
      llvh::cl::desc("Sampling profiler frequency (in Hz)"),
      llvh::cl::cat(RuntimeCategory)};

  llvh::cl::opt<MemorySize, false, MemorySizeParser> MaxHeapSize{
      "gc-max-heap",
      llvh::cl::desc("Max heap size.  Format: <unsigned>{K,M,G}{iB}"),
      llvh::cl::cat(GCCategory),
      llvh::cl::init(MemorySize{vm::GCConfig::getDefaultMaxHeapSize()})};

  llvh::cl::opt<unsigned> MaxNumRegisters{
      "max-register-stack",
      llvh::cl::desc("Max number of registers in the register stack."),
      llvh::cl::cat(RuntimeCategory),
      llvh::cl::init(vm::RuntimeConfig::getDefaultMaxNumRegisters())};

  llvh::cl::opt<bool> ES6Proxy{
      "Xes6-proxy",
      llvh::cl::desc("Enable support for ES6 Proxy"),
      llvh::cl::init(vm::RuntimeConfig::getDefaultES6Proxy()),
      llvh::cl::cat(RuntimeCategory)};

  llvh::cl::opt<bool> Intl{
      "Xintl",
      llvh::cl::desc("Enable support for ECMA-402 Intl APIs"),
      llvh::cl::init(vm::RuntimeConfig::getDefaultIntl()),
      llvh::cl::cat(RuntimeCategory)};

  llvh::cl::opt<bool> MicrotaskQueue{
      "Xmicrotask-queue",
      llvh::cl::desc("Enable support for using microtasks"),
      llvh::cl::init(vm::RuntimeConfig::getDefaultMicrotaskQueue()),
      llvh::cl::cat(RuntimeCategory)};

  llvh::cl::opt<bool> StopAfterInit{
      "stop-after-module-init",
      llvh::cl::desc(
          "Exit once module loading is finished. Useful "
          "to measure module initialization time"),
      llvh::cl::cat(RuntimeCategory)};

  llvh::cl::opt<bool> TrackBytecodeIO{
      "track-io",
      llvh::cl::desc(
          "Track bytecode I/O when executing bytecode. Only works with bytecode mode"),
      llvh::cl::cat(RuntimeCategory)};

  llvh::cl::opt<uint32_t> VMExperimentFlags{
      "Xvm-experiment-flags",
      llvh::cl::desc("VM experiment flags."),
      llvh::cl::init(vm::RuntimeConfig::getDefaultVMExperimentFlags()),
      llvh::cl::Hidden,
      llvh::cl::cat(RuntimeCategory)};

  llvh::cl::opt<bool> EnableHermesInternal{
      "enable-hermes-internal",
      llvh::cl::desc("Enable the HermesInternal object."),
      llvh::cl::init(vm::RuntimeConfig::getDefaultEnableHermesInternal())};

  llvh::cl::opt<bool> EnableHermesInternalTestMethods{
      "Xhermes-internal-test-methods",
      llvh::cl::desc("Enable the HermesInternal test methods."),
      llvh::cl::init(
          vm::RuntimeConfig::getDefaultEnableHermesInternalTestMethods()),
      llvh::cl::Hidden};

  llvh::cl::opt<bool> HeapTimeline{
      "Xheap-timeline",
      llvh::cl::desc(
          "Track heap allocation stacks and add them to the output of createHeapSnapshot()"),
      llvh::cl::init(false),
      llvh::cl::cat(RuntimeCategory)};

  llvh::cl::opt<bool> RandomizeMemoryLayout{
      "Xrandomize-memory-layout",
      llvh::cl::desc("Randomize stack placement etc."),
      llvh::cl::init(false),
      llvh::cl::Hidden};

  llvh::cl::opt<bool> GCAllocYoung{
      "gc-alloc-young",
      llvh::cl::desc(
          "Determines whether to (initially) allocate in the young generation"),
      llvh::cl::cat(GCCategory),
      llvh::cl::init(true)};

  llvh::cl::opt<bool> GCRevertToYGAtTTI{
      "gc-revert-to-yg-at-tti",
      llvh::cl::desc(
          "Determines whether to revert to young generation, if necessary, at "
          "TTI notification"),
      llvh::cl::cat(GCCategory),
      llvh::cl::init(false)};

  enum class JITMode {
    // JIT is ON with default thresholds.
    On,
    // Every function is JIT compiled immediately, ignoring thresholds.
    Force,
    // JIT is OFF, no JIT compilation is performed.
    Off,
  };

  llvh::cl::opt<JITMode> JIT{
      "Xjit",
      llvh::cl::ValueOptional,
      llvh::cl::Hidden,
      llvh::cl::ZeroOrMore,
      llvh::cl::cat(RuntimeCategory),
      llvh::cl::desc("JIT mode (default off)"),
      llvh::cl::init(JITMode::Off),
      values(
          clEnumValN(JITMode::On, "", "JIT is on"),
          clEnumValN(JITMode::On, "on", "JIT is on"),
          clEnumValN(
              JITMode::Force,
              "force",
              "force JIT compilation of every function"),
          clEnumValN(JITMode::Off, "off", "JIT is disabled"))};

  llvh::cl::opt<uint32_t> JITThreshold{
      "Xjit-threshold",
      llvh::cl::Hidden,
      llvh::cl::cat(RuntimeCategory),
      llvh::cl::desc("default minimum number of invocations to JIT compile"),
      llvh::cl::init(1 << 5)};

  llvh::cl::opt<uint32_t> JITMemoryLimit{
      "Xjit-memory-limit",
      llvh::cl::Hidden,
      llvh::cl::cat(RuntimeCategory),
      llvh::cl::desc("maximum size for JIT code (in bytes)"),
      llvh::cl::init(32u << 20)};

  llvh::cl::opt<uint32_t> JITMaxRecompiles{
      "Xjit-max-recompiles",
      llvh::cl::Hidden,
      llvh::cl::cat(RuntimeCategory),
      llvh::cl::desc(
          "maximum number of recompiles per function "
          "(0 disables recompilation)"),
      llvh::cl::init(2)};

  llvh::cl::opt<uint32_t> JITRecompileThreshold{
      "Xjit-recompile-threshold",
      llvh::cl::Hidden,
      llvh::cl::cat(RuntimeCategory),
      llvh::cl::desc(
          "ById helper declines within one compiled body "
          "before a recompile is considered (0 means 1)"),
      llvh::cl::init(64)};

  llvh::cl::opt<uint32_t> JITHCIdLimit{
      "Xjit-hc-id-limit",
      llvh::cl::Hidden,
      llvh::cl::cat(RuntimeCategory),
      llvh::cl::desc(
          "largest lazy JIT id assignable to a HiddenClass (testing only)"),
      llvh::cl::init(0xFFFF)};

  /// To get the value of this CLI option, use the method below.
  llvh::cl::opt<unsigned> DumpJITCode{
      "Xdump-jitcode",
      llvh::cl::Hidden,
      llvh::cl::cat(RuntimeCategory),
      llvh::cl::desc("dump JIT'ed code"),
      llvh::cl::init(0),
      llvh::cl::ValueRequired};

#ifdef HERMES_ENABLE_PERF_PROF
  llvh::cl::opt<bool> PerfProf{
      "Xperf-prof",
      llvh::cl::Hidden,
      llvh::cl::cat(RuntimeCategory),
      llvh::cl::desc("Enable perf profiling support"),
      llvh::cl::init(false)};

  llvh::cl::opt<std::string> PerfProfDir{
      "Xperf-prof-dir",
      llvh::cl::Hidden,
      llvh::cl::cat(RuntimeCategory),
      llvh::cl::desc(
          "Directory path for generated perf supplementary data, default is /tmp"),
      llvh::cl::init("/tmp"),
      llvh::cl::ValueRequired};
#endif

  llvh::cl::opt<bool> JITCrashOnError{
      "Xjit-crash-on-error",
      llvh::cl::Hidden,
      llvh::cl::cat(RuntimeCategory),
      llvh::cl::desc("crash on any JIT compilation error"),
      llvh::cl::init(false)};

  llvh::cl::opt<bool> JITEmitAsserts{
      "Xjit-emit-asserts",
      llvh::cl::Hidden,
      llvh::cl::cat(RuntimeCategory),
#ifdef NDEBUG
      llvh::cl::desc(
          "(default false) Whether assertions in JIT compiled code are enabled"),
      llvh::cl::init(false),
#else
      llvh::cl::desc(
          "(default true) Whether assertions in JIT compiled code are enabled"),
      llvh::cl::init(true)
#endif
  };

  llvh::cl::opt<bool> JITEmitTypeAsserts{
      "Xjit-emit-type-asserts",
      llvh::cl::Hidden,
      llvh::cl::cat(RuntimeCategory),
      llvh::cl::desc(
          "(default false) Whether to verify FR type assumptions in JIT "
          "compiled code"),
      llvh::cl::init(false)};

  llvh::cl::opt<bool> JITEmitCounters{
      "Xjit-emit-counters",
      llvh::cl::Hidden,
      llvh::cl::cat(RuntimeCategory),
      llvh::cl::desc("Whether to emit counters in JIT compiled code"),
      llvh::cl::init(false)};
};

/// All command line runtime options relevant to the VM, including options
/// shared between the compiler and VM. If this library is linked statically, it
/// shares the same llvh::CommandLine global state with its user. If linked
/// dynamically, it has its own global state, since LLVH is not dynamically
/// exported.
///
/// This struct can be used as a header-only dependency and instantiated by
/// any client needing to parse these command line options.
struct RuntimeFlags : public VMOnlyRuntimeFlags, CompilerRuntimeFlags {};

#ifndef HERMES_IS_MOBILE_BUILD
/// Build runtime config from the parsed command line flags.
vm::RuntimeConfig buildRuntimeConfig(const RuntimeFlags &flags);
#endif // HERMES_IS_MOBILE_BUILD

} // namespace hermes::cli

#endif // HERMES_VM_RUNTIMEFLAGS_H

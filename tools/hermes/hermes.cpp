/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/CompilerDriver/CompilerDriver.h"
#include "hermes/ConsoleHost/ConsoleHost.h"
#include "hermes/Support/OSCompat.h"
#include "hermes/Support/PageAccessTracker.h"
#include "hermes/TypedLib/TypedLib.h"
#include "hermes/VM/RuntimeFlags.h"

#include "llvh/ADT/SmallString.h"
#include "llvh/ADT/SmallVector.h"
#include "llvh/Support/Allocator.h"
#include "llvh/Support/CommandLine.h"
#include "llvh/Support/FileSystem.h"
#include "llvh/Support/InitLLVM.h"
#include "llvh/Support/PrettyStackTrace.h"
#include "llvh/Support/Program.h"
#include "llvh/Support/SHA1.h"
#include "llvh/Support/Signals.h"

#include "repl.h"

#ifdef HERMES_ENABLE_PERF_PROF
#include <fcntl.h>
#endif

using namespace hermes;

namespace {
struct Flags : public cli::VMOnlyRuntimeFlags {
  llvh::cl::opt<unsigned> Repeat{
      "Xrepeat",
      llvh::cl::desc("Repeat execution N number of times"),
      llvh::cl::init(1),
      llvh::cl::Hidden};

  llvh::cl::opt<bool> GCBeforeStats{
      "gc-before-stats",
      llvh::cl::desc(
          "Perform a full GC just before printing statistics at exit"),
      llvh::cl::cat(GCCategory),
      llvh::cl::init(false)};

  llvh::cl::opt<bool> GCPrintCollectionStats{
      "gc-print-collection-stats",
      llvh::cl::desc("Output statistics for each garbage collection at exit"),
      llvh::cl::cat(GCCategory),
      llvh::cl::init(false)};

  llvh::cl::opt<unsigned> ExecutionTimeLimit{
      "time-limit",
      llvh::cl::desc(
          "Number of milliseconds after which to abort JS execution"),
      llvh::cl::init(0)};
};

Flags flags;

/// Extra positional arguments after '--' that are passed to the script
/// via hermescli.getScriptArgs().
std::vector<std::string> scriptArgsFromCL;

} // namespace

/// Execute Hermes bytecode \p bytecode, respecting command line arguments.
/// \return an exit status.
static int executeHBCBytecodeFromCL(
    std::unique_ptr<hbc::BCProvider> bytecode,
    const driver::BytecodeBufferInfo &info) {
#if !HERMESVM_JIT
  if (flags.DumpJITCode || flags.JIT != cli::VMOnlyRuntimeFlags::JITMode::Off) {
    llvh::errs() << "JIT is not enabled in this build\n";
    return EXIT_FAILURE;
  }
#endif

  ExecuteOptions options;

  auto gcConfigBuilder = vm::GCConfig::Builder()
                             .withInitHeapSize(flags.InitHeapSize.bytes)
                             .withMaxHeapSize(flags.MaxHeapSize.bytes)
                             .withOccupancyTarget(flags.OccupancyTarget)
                             .withSanitizeConfig(
                                 vm::GCSanitizeConfig::Builder()
                                     .withSanitizeRate(flags.GCSanitizeRate)
                                     .withRandomSeed(flags.GCSanitizeRandomSeed)
                                     .build())
                             .withShouldReleaseUnused(vm::kReleaseUnusedNone)
                             .withAllocInYoung(flags.GCAllocYoung)
                             .withRevertToYGAtTTI(flags.GCRevertToYGAtTTI);

  std::vector<vm::GCAnalyticsEvent> gcAnalyticsEvents;
  if (flags.GCPrintStats || flags.GCBeforeStats ||
      flags.GCPrintCollectionStats) {
    gcConfigBuilder.withShouldRecordStats(true);
    if (flags.GCPrintCollectionStats) {
      options.gcAnalyticsEvents = &gcAnalyticsEvents;
      gcConfigBuilder.withAnalyticsCallback(
          [&gcAnalyticsEvents](const vm::GCAnalyticsEvent &event) {
            gcAnalyticsEvents.push_back(event);
          });
    }
  }

  options.runtimeConfig =
      vm::RuntimeConfig::Builder()
          .withGCConfig(gcConfigBuilder.build())
          .withMaxNumRegisters(flags.MaxNumRegisters)
          .withEnableJIT(
              flags.DumpJITCode ||
              flags.JIT != cli::VMOnlyRuntimeFlags::JITMode::Off)
          .withForceJIT(flags.JIT == cli::VMOnlyRuntimeFlags::JITMode::Force)
          .withJITThreshold(flags.JITThreshold)
          .withJITMemoryLimit(flags.JITMemoryLimit)
          .withJITMaxRecompiles(flags.JITMaxRecompiles)
          .withJITRecompileThreshold(flags.JITRecompileThreshold)
          .withEnableEval(cl::compilerRuntimeFlags.EnableEval)
          .withEnableAsyncGenerators(
              cl::compilerRuntimeFlags.EnableAsyncGenerators)
          .withVerifyEvalIR(cl::compilerRuntimeFlags.VerifyIR)
          .withOptimizedEval(cl::compilerRuntimeFlags.OptimizedEval)
          .withAsyncBreakCheckInEval(
              cl::compilerRuntimeFlags.EmitAsyncBreakCheck)
          .withVMExperimentFlags(flags.VMExperimentFlags)
          .withES6Proxy(flags.ES6Proxy)
          .withIntl(flags.Intl)
          .withMicrotaskQueue(flags.MicrotaskQueue)
          .withEnableSampleProfiling(
              flags.SampleProfiling !=
              ExecuteOptions::SampleProfilingMode::None)
          .withRandomizeMemoryLayout(flags.RandomizeMemoryLayout)
          .withTrackIO(flags.TrackBytecodeIO)
          .withEnableHermesInternal(flags.EnableHermesInternal)
          .withEnableHermesInternalTestMethods(
              flags.EnableHermesInternalTestMethods)
          .withTest262(cl::compilerRuntimeFlags.Test262)
          .withES6BlockScoping(cl::compilerRuntimeFlags.ES6BlockScoping)
          .build();

  options.basicBlockProfiling = cl::BasicBlockProfiling;
  options.profilingOutFile = cl::ProfilingOutFile;

  options.stopAfterInit = false;
  options.timeLimit = flags.ExecutionTimeLimit;
  options.dumpJITCode = flags.DumpJITCode;
  options.jitCrashOnError = flags.JITCrashOnError;
  options.jitEmitAsserts = flags.JITEmitAsserts;
  options.jitEmitTypeAsserts = flags.JITEmitTypeAsserts;
  options.jitEmitCounters = flags.JITEmitCounters;
  options.jitHCIdLimit = flags.JITHCIdLimit;
  options.stopAfterInit = flags.StopAfterInit;
  options.forceGCBeforeStats = flags.GCBeforeStats;
  options.sampleProfiling = flags.SampleProfiling;
  options.sampleProfilingFreq = flags.SampleProfilingFreq;
  options.heapTimeline = flags.HeapTimeline;

  options.scriptArgs = scriptArgsFromCL;

#ifdef HERMES_ENABLE_PERF_PROF
  std::string jitdumpFile;
  if (flags.PerfProf) {
    llvh::raw_string_ostream sos{jitdumpFile};
    // jitdump uses uint32_t for pid.
    sos << llvh::format(
        "%s/jit-%d.dump",
        flags.PerfProfDir.c_str(),
        (uint32_t)oscompat::process_id());
    options.perfProfJitDumpFd =
        open(sos.str().c_str(), O_CREAT | O_TRUNC | O_RDWR, 0666);
    if (options.perfProfJitDumpFd == -1)
      hermes_fatal("Failed to open jitdump file: " + jitdumpFile);
    llvh::SmallString<256> debugInfoFile;
    if (std::error_code error = llvh::sys::fs::createUniqueFile(
            llvh::Twine(flags.PerfProfDir) + "/debuginfo-%%%%%%%%.txt",
            options.perfProfDebugInfoFd,
            debugInfoFile)) {
      hermes_fatal("Fail to create debuginfo file for perf jitdump");
    }
    options.perfProfDebugInfoFile = debugInfoFile.str();
  }
#endif

  bool success;
  if (flags.Repeat <= 1) {
    success = executeHBCBytecode(std::move(bytecode), options, &info.filename);
  } else {
    // The runtime is supposed to own the bytecode exclusively, but we
    // want to keep it around in this special case, so we can reuse it
    // between iterations.
    std::shared_ptr<hbc::BCProvider> sharedBytecode = std::move(bytecode);

    success = true;
    for (unsigned i = 0; i < flags.Repeat; ++i) {
      success &= executeHBCBytecode(
          std::shared_ptr<hbc::BCProvider>{sharedBytecode},
          options,
          &info.filename);
    }
  }
#ifdef HERMES_ENABLE_PERF_PROF
  if (flags.PerfProf) {
    if (close(options.perfProfJitDumpFd) == -1)
      hermes_fatal("Fail to close jitdump file: " + jitdumpFile);
    if (close(options.perfProfDebugInfoFd) == -1)
      hermes_fatal(
          "Fail to close debuginfo file: " + options.perfProfDebugInfoFile);
  }

#endif
  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}

static vm::RuntimeConfig getReplRuntimeConfig() {
  return vm::RuntimeConfig::Builder()
      .withGCConfig(
          vm::GCConfig::Builder()
              .withInitHeapSize(flags.InitHeapSize.bytes)
              .withMaxHeapSize(flags.MaxHeapSize.bytes)
              .withSanitizeConfig(
                  vm::GCSanitizeConfig::Builder()
                      .withSanitizeRate(flags.GCSanitizeRate)
                      .withRandomSeed(flags.GCSanitizeRandomSeed)
                      .build())
              .build())
      .withVMExperimentFlags(flags.VMExperimentFlags)
      .withES6Proxy(flags.ES6Proxy)
      .withEnableAsyncGenerators(cl::compilerRuntimeFlags.EnableAsyncGenerators)
      .withIntl(flags.Intl)
      .withMicrotaskQueue(flags.MicrotaskQueue)
      .withEnableHermesInternal(flags.EnableHermesInternal)
      .withEnableHermesInternalTestMethods(
          flags.EnableHermesInternalTestMethods)
      .withES6BlockScoping(cl::compilerRuntimeFlags.ES6BlockScoping)
      .build();
}

int main(int argc, char **argv) {
#ifndef HERMES_FBCODE_BUILD
  // Normalize the arg vector.
  llvh::InitLLVM initLLVM(argc, argv);
#else
  // When both HERMES_FBCODE_BUILD and sanitizers are enabled, InitLLVM may have
  // been already created and destroyed before main() is invoked. This presents
  // a problem because InitLLVM can't be instantiated more than once in the same
  // process. The most important functionality InitLLVM provides is shutting
  // down LLVM in its destructor. We can use "llvm_shutdown_obj" to do the same.
  llvh::llvm_shutdown_obj Y;
#endif

  // Enable the microtask queue in the CLI by default.
  flags.MicrotaskQueue.setInitialValue(true);

  // Extract script arguments after '--' before LLVM command line parsing,
  // so they are not interpreted as input filenames. These are made available
  // to JS via hermescli.getScriptArgs().
  int clArgc = argc;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--") == 0) {
      for (int j = i + 1; j < argc; ++j)
        scriptArgsFromCL.emplace_back(argv[j]);
      clArgc = i;
      break;
    }
  }

  driver::VMFeatures vmFeatures;
#ifdef HERMES_ENABLE_DEBUGGER
  vmFeatures.debugger = true;
#endif
#ifdef HERMESVM_CONTIGUOUS_HEAP
  vmFeatures.contiguousHeap = true;
#endif
#ifdef HERMES_ENABLE_UNICODE_REGEXP_PROPERTY_ESCAPES
  vmFeatures.unicodeRegExpPropertyEscapes = true;
#endif
  vmFeatures.jit = HERMESVM_JIT;

  llvh::cl::AddExtraVersionPrinter([vmFeatures](llvh::raw_ostream &s) {
    driver::printHermesCompilerVMVersion(s, &vmFeatures);
  });
  llvh::cl::ParseCommandLineOptions(clArgc, argv, "Hermes driver\n");

  if (cl::HelpTyped) {
    llvh::outs() << getTypedLanguageDoc();
    return 0;
  }

  if (cl::InputFilenames.size() == 0) {
    return repl(getReplRuntimeConfig());
  }

  // Tell compiler to emit async break check if time-limit feature is enabled
  // so that user can turn on this feature with single ExecutionTimeLimit
  // option.
  if (flags.ExecutionTimeLimit > 0) {
    cl::compilerRuntimeFlags.EmitAsyncBreakCheck = true;
  }

  // Make sure any allocated alt signal stack is not considered a leak
  // by ASAN.
  oscompat::SigAltStackLeakSuppressor sigAltLeakSuppressor;
  driver::CompileResult res = driver::compileFromCommandLineOptions();
  if (res.bytecodeProvider) {
    auto ret = executeHBCBytecodeFromCL(
        std::move(res.bytecodeProvider), res.bytecodeBufferInfo);
    return ret;
  } else {
    return res.status;
  }
}

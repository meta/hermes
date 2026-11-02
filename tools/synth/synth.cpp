/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/ConsoleHost/RuntimeFlags.h>
#include <hermes/Support/Algorithms.h>
#include <hermes/Support/MemoryBuffer.h>
#include <hermes/TraceInterpreter.h>
#include <hermes/hermes.h> // @manual=fbsource//xplat/hermes/API:HermesAPI
#include <hermes/hermes_tracing.h>

#include "llvh/ADT/Statistic.h"
#include "llvh/Support/CommandLine.h"
#include "llvh/Support/PrettyStackTrace.h"
#include "llvh/Support/Signals.h"

#include <iostream>
#include <tuple>

using MarkerAction =
    facebook::hermes::tracing::TraceInterpreter::ExecuteOptions::MarkerAction;

namespace cl {

using llvh::cl::desc;
using llvh::cl::init;
using llvh::cl::list;
using llvh::cl::OneOrMore;
using llvh::cl::opt;
using llvh::cl::Positional;
using llvh::cl::Required;

/// @name Synth benchmark specific flags
/// @{

static opt<std::string>
    TraceFile(desc("input trace file"), Positional, Required);

static list<std::string>
    BytecodeFiles(desc("input bytecode files"), Positional, OneOrMore);

static opt<std::string> Marker(
    "marker",
    desc("marker to stop at, \"end\" means end of trace"),
    init("end"));
static llvh::cl::alias
    MarkerA("m", desc("alias for -marker"), llvh::cl::aliasopt(Marker));

static opt<MarkerAction> Action(
    "action-at-marker",
    desc("Take a snapshot at the given marker"),
    init(MarkerAction::NONE),
    llvh::cl::values(
        clEnumValN(MarkerAction::NONE, "stop", "Stop the trace and get stats"),
        clEnumValN(
            MarkerAction::SNAPSHOT,
            "snapshot",
            "Take a heap snapshot at the marker to stop at"),
        clEnumValN(
            MarkerAction::TIMELINE,
            "timeline",
            "Take a heap timeline from the beginning of execution until the "
            "marker to stop at"),
        clEnumValN(
            MarkerAction::SAMPLE_MEMORY,
            "sample-memory",
            "Take a heap sampling profile at the marker to stop at"),
        clEnumValN(
            MarkerAction::SAMPLE_TIME,
            "sample-time",
            "Take a CPU sampling profile at the marker to stop at")));

static opt<bool> UseTraceConfig(
    "use-trace-config",
    desc(
        "Controls what RuntimeConfig as the default that the various config "
        "modify.  True says to use the recorded config of the trace, false "
        "means start from the default config."),
    init(true));

static opt<bool> UseVerification(
    "verification",
    desc("Replay the trace with verification enabled."),
    init(false));

static opt<std::string> Trace(
    "trace",
    desc(
        "Take a trace of the synthetic benchmark running. Can be used to verify that the replay made the same trace again. Outputs to the file given"),
    init(""));

#if !defined(NDEBUG) || defined(LLVM_ENABLE_STATS)
static opt<bool>
    PrintStats("print-stats", desc("Print statistics"), init(false));
#endif

static opt<unsigned> BytecodeWarmupPercent(
    "bytecode-warmup-percent",
    desc(
        "Eagerly read some bytecode into page cache. May yield faster startup "
        "for large bytecode files."),
    init(0));

static opt<int> Reps(
    "reps",
    desc(
        "Number of repetitions of execution. Any GC stats printed are those for the "
        "rep with the median \"totalTime\"."),
    init(1));

static opt<bool> DisableSourceHashCheck(
    "disable-source-hash-check",
    desc(
        "Remove the requirement that the input bytecode was compiled from the "
        "same source used to record the trace. There must only be one input "
        "bytecode file in this case. If its observable behavior deviates "
        "from the trace, the results are undefined."),
    init(false));

/// @}

/// @name Common flags from Hermes VM
/// @{

static opt<bool> GCAllocYoung(
    "gc-alloc-young",
    desc("Determines whether to (initially) allocate in the young generation"),
    cat(GCCategory),
    init(false));

static opt<bool> GCRevertToYGAtTTI(
    "gc-revert-to-yg-at-tti",
    desc(
        "Determines whether to revert to young generation, if necessary, at "
        "TTI notification"),
    cat(GCCategory),
    init(true));

static opt<bool> GCBeforeStats(
    "gc-before-stats",
    desc("Perform a full GC just before printing statistics at exit"),
    cat(GCCategory),
    init(false));

static opt<bool> GCPrintStats(
    "gc-print-stats",
    desc("Output summary garbage collection statistics at exit"),
    cat(GCCategory),
    init(true));

static opt<::hermes::vm::ReleaseUnused> ShouldReleaseUnused(
    "release-unused",
    desc("How aggressively to return unused memory to the OS."),
    init(GCConfig::getDefaultShouldReleaseUnused()),
    llvh::cl::values(
        clEnumValN(
            ::hermes::vm::kReleaseUnusedNone,
            "none",
            "Don't try to release unused memory."),
        clEnumValN(
            ::hermes::vm::kReleaseUnusedOld,
            "old",
            "Only old gen, on full collections."),
        clEnumValN(
            ::hermes::vm::kReleaseUnusedYoungOnFull,
            "young-on-full",
            "Also young gen, but only on full collections."),
        clEnumValN(
            ::hermes::vm::kReleaseUnusedYoungAlways,
            "young-always",
            "Also young gen, also on young gen collections")));

/// @}

} // namespace cl

// Helper functions.
template <typename T>
static llvh::Optional<T> execOption(const cl::opt<T> &clOpt) {
  if (clOpt.getNumOccurrences() > 0) {
    return static_cast<T>(clOpt);
  } else {
    return llvh::None;
  }
}

// Must do this special case explicitly, because of the MemorySizeParser.
static llvh::Optional<::hermes::vm::gcheapsize_t> execOption(
    const cl::opt<cl::MemorySize, false, cl::MemorySizeParser> &clOpt) {
  if (clOpt.getNumOccurrences() > 0) {
    return clOpt.bytes;
  } else {
    return llvh::None;
  }
}

static const char *fileExtensionForAction(MarkerAction action) {
  switch (action) {
    case MarkerAction::SNAPSHOT:
      return "heapsnapshot";
    case MarkerAction::TIMELINE:
      return "heaptimeline";
    case MarkerAction::SAMPLE_MEMORY:
      return "heapprofile";
    case MarkerAction::SAMPLE_TIME:
      return "cpuprofile";
    default:
      llvm_unreachable(
          "Should never call fileExtensionForAction with a none action");
  }
}

int main(int argc, char **argv) {
  // Print a stack trace if we signal out.
  llvh::sys::PrintStackTraceOnErrorSignal("Hermes synth");
  llvh::PrettyStackTraceProgram X(argc, argv);
  // Call llvm_shutdown() on exit to print stats and free memory.
  llvh::llvm_shutdown_obj Y;
  llvh::cl::ParseCommandLineOptions(argc, argv, "Hermes synth trace driver\n");

  using namespace facebook::hermes::tracing;
  try {
    TraceInterpreter::ExecuteOptions options;

    // These are not config parameters: just set them according to the
    // runtime flag.
    options.useTraceConfig = cl::UseTraceConfig;
    options.verificationEnabled = cl::UseVerification;
    options.reps = cl::Reps;
    options.marker = cl::Marker;
    options.action = cl::Action;
    if (options.action != MarkerAction::NONE) {
      llvh::SmallVector<char, 16> tmpfile;
      llvh::sys::fs::createTemporaryFile(
          options.marker, fileExtensionForAction(options.action), tmpfile);
      options.profileFileName = std::string{tmpfile.begin(), tmpfile.end()};
    }
    options.forceGCBeforeStats = cl::GCBeforeStats;
    options.disableSourceHashCheck = cl::DisableSourceHashCheck;

    // These are the config parameters.

    // We want to print the GC stats by default.  We won't print them
    // if -gc-print-stats is specified false explicitly, and
    // -gc-before-stats is also false.
    bool shouldPrintGCStats = true;
    if (cl::GCPrintStats.getNumOccurrences() > 0) {
      shouldPrintGCStats = (cl::GCPrintStats || cl::GCBeforeStats);
    }

    llvh::Optional<::hermes::vm::gcheapsize_t> minHeapSize =
        execOption(cl::MinHeapSize);
    llvh::Optional<::hermes::vm::gcheapsize_t> initHeapSize =
        execOption(cl::InitHeapSize);
    llvh::Optional<::hermes::vm::gcheapsize_t> maxHeapSize =
        execOption(cl::MaxHeapSize);
    llvh::Optional<double> occupancyTarget = execOption(cl::OccupancyTarget);
    llvh::Optional<::hermes::vm::ReleaseUnused> shouldReleaseUnused =
        execOption(cl::ShouldReleaseUnused);
    llvh::Optional<bool> allocInYoung = execOption(cl::GCAllocYoung);
    llvh::Optional<bool> revertToYGAtTTI = execOption(cl::GCRevertToYGAtTTI);
    options.shouldTrackIO = execOption(cl::TrackBytecodeIO);
    options.bytecodeWarmupPercent = execOption(cl::BytecodeWarmupPercent);
    llvh::Optional<double> sanitizeRate = execOption(cl::GCSanitizeRate);
    // The type of this case is complicated, so just do it explicitly.
    llvh::Optional<int64_t> sanitizeRandomSeed;
    if (cl::GCSanitizeRandomSeed) {
      sanitizeRandomSeed = cl::GCSanitizeRandomSeed;
    }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_STATS)
    if (cl::PrintStats)
      llvh::EnableStatistics();
#endif

    options.gcConfigBuilder.withShouldRecordStats(shouldPrintGCStats);
    if (minHeapSize) {
      options.gcConfigBuilder.withMinHeapSize(*minHeapSize);
    }
    if (initHeapSize) {
      options.gcConfigBuilder.withMinHeapSize(*initHeapSize);
    }
    if (maxHeapSize) {
      options.gcConfigBuilder.withMaxHeapSize(*maxHeapSize);
    }
    if (occupancyTarget) {
      options.gcConfigBuilder.withOccupancyTarget(*occupancyTarget);
    }
    if (shouldReleaseUnused) {
      options.gcConfigBuilder.withShouldReleaseUnused(*shouldReleaseUnused);
    }
    if (allocInYoung) {
      options.gcConfigBuilder.withAllocInYoung(*allocInYoung);
    }
    if (revertToYGAtTTI) {
      options.gcConfigBuilder.withRevertToYGAtTTI(*revertToYGAtTTI);
    }
    if (sanitizeRate || sanitizeRandomSeed) {
      auto sanitizeConfigBuilder = ::hermes::vm::GCSanitizeConfig::Builder();
      if (sanitizeRate) {
        sanitizeConfigBuilder.withSanitizeRate(*sanitizeRate);
      }
      if (sanitizeRandomSeed) {
        sanitizeConfigBuilder.withRandomSeed(*sanitizeRandomSeed);
      }
      options.gcConfigBuilder.withSanitizeConfig(sanitizeConfigBuilder.build());
    }

    std::vector<std::string> bytecodeFiles{
        cl::BytecodeFiles.begin(), cl::BytecodeFiles.end()};
    if (cl::DisableSourceHashCheck && bytecodeFiles.size() != 1) {
      throw std::invalid_argument(
          "Must have single bytecode file to disable source hash check");
    }
    if (!cl::Trace.empty()) {
      // If this is tracing mode, get the trace instead of the stats.
      options.gcConfigBuilder.withShouldRecordStats(false);
      options.shouldTrackIO = false;
      std::error_code ec;
      auto os = std::make_unique<llvh::raw_fd_ostream>(
          cl::Trace.c_str(),
          ec,
          llvh::sys::fs::CD_CreateAlways,
          llvh::sys::fs::FA_Write,
          llvh::sys::fs::OF_Text);
      if (ec) {
        throw std::system_error(ec);
      }

      options.traceEnabled = true;
      TraceInterpreter::execWithRuntime(
          cl::TraceFile,
          bytecodeFiles,
          options,
          [stream = std::ref(os)](
              const ::hermes::vm::RuntimeConfig &config) mutable {
            auto *hermesRoot =
                facebook::jsi::castInterface<facebook::hermes::IHermesRootAPI>(
                    facebook::hermes::makeHermesRootAPI());
            auto &st = stream.get();
            return facebook::hermes::makeTracingHermesRuntime(
                hermesRoot->makeHermesRuntime(config),
                config,
                std::move(st),
                /* forReplay */ true);
          });

      llvh::outs() << "\nWrote output trace to: " << cl::Trace << "\n";
    } else {
      llvh::outs() << TraceInterpreter::execAndGetStats(
                          cl::TraceFile, bytecodeFiles, options)
                   << "\n";
    }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_STATS)
    if (cl::PrintStats)
      llvh::PrintStatistics(llvh::outs());
#endif
    if (!options.profileFileName.empty()) {
      llvh::outs() << "Wrote profile for marker \"" << options.marker
                   << "\" to " << options.profileFileName << "\n";
    }
    return 0;
  } catch (const std::invalid_argument &e) {
    std::cerr << "Invalid argument: " << e.what() << std::endl;
  } catch (const std::system_error &e) {
    std::cerr << "System error: " << e.what() << std::endl;
  }
  return 1;
}

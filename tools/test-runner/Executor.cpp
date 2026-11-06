/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Executor.h"

#include "hermes/BCGen/HBC/BCProviderFromSrc.h"
#include "hermes/BCGen/HBC/HBC.h"
#include "hermes/ConsoleHost/ConsoleHost.h"
#include "hermes/Public/RuntimeConfig.h"
#include "hermes/Support/MemoryBuffer.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/Domain.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/RuntimeFlags.h"
#include "hermes/VM/TimeLimitMonitor.h"
#include "hermes/hermes.h"

#include "jsi/jsi.h"

#include "llvh/ADT/ScopeExit.h"
#include "llvh/Support/FileSystem.h"
#include "llvh/Support/MemoryBuffer.h"
#include "llvh/Support/Program.h"
#include "llvh/Support/raw_ostream.h"

#include <cassert>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include <csetjmp>
#include <csignal>
#endif

namespace hermes {
namespace testrunner {

namespace {

using Clock = std::chrono::steady_clock;

#ifndef _WIN32

/// Size of the alternate signal stack per worker thread.
constexpr size_t kAltStackSize = 64 * 1024;

/// Thread-local state for the per-test crash guard.
/// sigsetjmp/siglongjmp recovers from SIGSEGV/SIGABRT/SIGBUS during
/// in-process bytecode execution, converting crashes to test failures.
struct CrashGuardState {
  sigjmp_buf jmpBuf;
  /// Whether the guard is active. The handler only jumps when set.
  volatile sig_atomic_t active = 0;
  /// The signal number that triggered the crash.
  volatile sig_atomic_t caughtSignal = 0;
  /// Alternate signal stack memory (needed to handle stack overflows).
  void *altStackMem = nullptr;
};

thread_local CrashGuardState tCrashGuard;

const char *crashSignalName(int sig) {
  switch (sig) {
    case SIGSEGV:
      return "Segmentation fault";
    case SIGABRT:
      return "Aborted";
    case SIGBUS:
      return "Bus error";
    case SIGFPE:
      return "Floating point exception";
    case SIGILL:
      return "Illegal instruction";
    default:
      return "Unknown signal";
  }
}

void crashSignalHandler(int sig) {
  if (tCrashGuard.active) {
    tCrashGuard.caughtSignal = sig;
    siglongjmp(tCrashGuard.jmpBuf, 1);
  }
  // Not in a guarded region — restore default and re-raise.
  signal(sig, SIG_DFL);
  raise(sig);
}

/// Install process-wide signal handlers for crash recovery.
/// Called exactly once in runAllTests() before spawning worker threads.
void installCrashHandlers() {
  struct sigaction sa{};
  sa.sa_handler = crashSignalHandler;
  sa.sa_flags = SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGABRT, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
  sigaction(SIGFPE, &sa, nullptr);
  sigaction(SIGILL, &sa, nullptr);
}

/// Set up an alternate signal stack for the calling thread, unless one is
/// already in place. Sanitizer runtimes (ASan/TSan/MSan) install a per-thread
/// alt stack via sanitizer_common's SetAlternateSignalStack at thread init —
/// when present, we reuse it (it's typically larger than ours anyway) and
/// avoid touching the kernel's record at all.
///
/// When we DO install our own, the memory must be obtained via mmap, not
/// malloc. Sanitizer runtimes call UnsetAlternateSignalStack on every thread
/// exit, which queries the kernel for the current alt stack and unconditionally
/// calls munmap on the reported ss_sp. macOS retains the kernel's ss_sp record
/// even after sigaltstack(SS_DISABLE), so the sanitizer sees our pointer and
/// tries to munmap it — which succeeds for mmap memory but fails with EINVAL
/// for malloc memory, tripping the sanitizer's internal CHECK and aborting
/// the process via SIGTRAP.
void setupThreadAltStack() {
  stack_t cur{};
  if (sigaltstack(nullptr, &cur) == 0 && !(cur.ss_flags & SS_DISABLE)) {
    return;
  }
  void *mem = mmap(
      nullptr,
      kAltStackSize,
      PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANON,
      -1,
      0);
  if (mem == MAP_FAILED)
    return;
  tCrashGuard.altStackMem = mem;
  stack_t ss{};
  ss.ss_sp = mem;
  ss.ss_size = kAltStackSize;
  ss.ss_flags = 0;
  sigaltstack(&ss, nullptr);
}

/// Tear down the alternate signal stack for the calling thread, but only
/// if we installed it ourselves. If we reused an existing alt stack
/// (altStackMem is null), leave it alone for its owner to clean up.
void teardownThreadAltStack() {
  if (tCrashGuard.altStackMem) {
    stack_t ss{};
    ss.ss_flags = SS_DISABLE;
    sigaltstack(&ss, nullptr);
    munmap(tCrashGuard.altStackMem, kAltStackSize);
    tCrashGuard.altStackMem = nullptr;
  }
}

/// The stack size to give each worker thread, or 0 to use the platform
/// default. Workers run the recursive-descent parser and the interpreter, both
/// of which recurse proportionally to source nesting depth; the default
/// secondary-thread stack (512KB on macOS) overflows on deeply nested tests.
/// Match SerialExecutor and use the main thread's limit.
size_t workerStackSize() {
  struct rlimit limit;
  if (getrlimit(RLIMIT_STACK, &limit) != 0 || limit.rlim_cur == RLIM_INFINITY)
    return 0;
  return (size_t)limit.rlim_cur;
}

#endif // !_WIN32

/// Holds the runtime environment created for a single test execution.
struct TestRuntimeEnv {
  std::unique_ptr<facebook::jsi::Runtime> jsiRuntime;
  vm::Runtime *runtime;
  std::shared_ptr<vm::TimeLimitMonitor> timeLimitMonitor;
};

/// Capture the current exception message from the runtime and clear it.
std::string captureException(vm::Runtime &runtime) {
  auto thrownVal = runtime.makeHandle(runtime.getThrownValue());
  std::string buf;
  llvh::raw_string_ostream sos(buf);
  runtime.printException(sos, thrownVal);
  runtime.clearThrownValue();
  return sos.str();
}

/// Create a configured Hermes runtime for test262 execution.
/// Uses buildRuntimeConfig() from RuntimeFlags to get a base config from
/// parsed CLI flags, then overlays test262-specific settings.
/// When \p disableHandleSan is true, GC handle sanitization is disabled
/// (sanitize rate = 0), matching the Python runner's behavior for
/// handlesan_skip_list tests.
TestRuntimeEnv createTestRuntime(
    const ExecConfig &config,
    bool disableHandleSan) {
  assert(config.runtimeFlags && "runtimeFlags must be set");
  auto baseConfig = cli::buildRuntimeConfig(*config.runtimeFlags);
  using JITMode = cli::VMOnlyRuntimeFlags::JITMode;
  auto builder =
      baseConfig
          .rebuild()
          // Test262-specific overrides (always true for test runner).
          .withES6Proxy(true)
          .withES6BlockScoping(true)
          .withMicrotaskQueue(true)
          .withEnableHermesInternal(true)
          .withEnableHermesInternalTestMethods(true)
          .withEnableAsyncGenerators(true)
          .withTest262(true)
          .withEnableEval(true)
          .withAsyncBreakCheckInEval(true)
          // JIT settings (not set by buildRuntimeConfig).
          .withEnableJIT(config.runtimeFlags->JIT != JITMode::Off)
          .withForceJIT(config.runtimeFlags->JIT == JITMode::Force)
          .withJITThreshold(config.runtimeFlags->JITThreshold)
          .withJITMemoryLimit(config.runtimeFlags->JITMemoryLimit)
          .withJITMaxRecompiles(config.runtimeFlags->JITMaxRecompiles)
          .withJITRecompileThreshold(
              config.runtimeFlags->JITRecompileThreshold);
  if (disableHandleSan) {
    auto gcConfig =
        baseConfig.getGCConfig()
            .rebuild()
            .withSanitizeConfig(
                vm::GCSanitizeConfig::Builder().withSanitizeRate(0.0).build())
            .build();
    builder.withGCConfig(gcConfig);
  }
  auto runtimeConfig = builder.build();

  auto hermesRuntime = facebook::hermes::makeHermesRuntime(runtimeConfig);
  auto *runtime = static_cast<vm::Runtime *>(
      facebook::jsi::castInterface<facebook::hermes::IHermes>(
          hermesRuntime.get())
          ->getVMRuntimeUnsafe());

  std::shared_ptr<vm::TimeLimitMonitor> timeLimitMonitor;
  if (config.timeoutSeconds > 0) {
    timeLimitMonitor = vm::TimeLimitMonitor::getOrCreate();
    runtime->timeLimitMonitor = timeLimitMonitor;
    timeLimitMonitor->watchRuntime(
        *runtime, std::chrono::milliseconds(config.timeoutSeconds * 1000u));
  }

  return {std::move(hermesRuntime), runtime, std::move(timeLimitMonitor)};
}

/// Drain the microtask queue, returning EXCEPTION on the first thrown
/// exception (the thrown value is left on the runtime for the caller to
/// capture). Unlike `microtask::performCheckpoint`, which prints exceptions
/// to stderr and continues draining — useful for a REPL but wrong for a test
/// runner that needs to observe failures originating in Promise.then
/// callbacks and other microtask contexts.
vm::ExecutionStatus drainMicrotasks(vm::Runtime &runtime) {
  runtime.clearKeptObjects();
  if (LLVM_UNLIKELY(
          runtime.cleanUpFinalizationCallbacks() ==
          vm::ExecutionStatus::EXCEPTION)) {
    return vm::ExecutionStatus::EXCEPTION;
  }
  if (!runtime.hasMicrotaskQueue())
    return vm::ExecutionStatus::RETURNED;
  return runtime.drainJobs();
}

/// Drain the setTimeout task queue, executing each callback and its
/// microtasks. Returns true and sets exceptionMsg if an exception was thrown.
bool drainTaskQueue(
    vm::Runtime &runtime,
    ConsoleHostContext &ctx,
    std::string &exceptionMsg) {
  vm::GCScope scope{runtime};
  vm::GCScopeMarkerRAII marker{scope};
  vm::MutableHandle<vm::Callable> task{runtime};
  while (auto optTask = ctx.dequeueTask()) {
    marker.flush();
    task = std::move(*optTask);
    auto callRes = vm::Callable::executeCall0(
        task, runtime, vm::Runtime::getUndefinedValue(), false);
    if (callRes == vm::ExecutionStatus::EXCEPTION) {
      exceptionMsg = captureException(runtime);
      return true;
    }
    if (drainMicrotasks(runtime) == vm::ExecutionStatus::EXCEPTION) {
      exceptionMsg = captureException(runtime);
      return true;
    }
  }
  return false;
}

/// Run compiled bytecode in a fresh runtime and evaluate the result
/// against negative expectations.
TestResult executeCompiledTest(
    llvh::StringRef testName,
    std::unique_ptr<hbc::BCProvider> bytecode,
    llvh::StringRef sourceURL,
    bool isAsync,
    const NegativeExpectation &negative,
    const ExecConfig &config,
    bool disableHandleSan,
    Clock::time_point startTime) {
  // Mirror Python's run() decision logic exactly:
  //   lazy: expect error iff negative.phase is non-empty (any phase)
  //   eager: expect error iff negative.phase == "runtime"
  // Parse/resolution errors in eager mode are resolved against the compile
  // step in executeTestVariant before we get here; in lazy mode they collapse
  // into "any error counts" because lazy compilation defers parsing.
  bool expectError =
      config.lazy ? !negative.phase.empty() : negative.phase == "runtime";

  auto makeResult = [&](ResultCode code, const std::string &msg) {
    auto endTime = Clock::now();
    TestResult r;
    r.testName = testName;
    r.code = code;
    r.message = msg;
    r.duration = std::chrono::duration_cast<std::chrono::microseconds>(
        endTime - startTime);
    return r;
  };

  auto env = createTestRuntime(config, disableHandleSan);

  // Install console bindings (including $262, alert, setTimeout, etc.).
  vm::GCScope scope(*env.runtime);
  ConsoleHostContext ctx{*env.runtime};
  ctx.enableTestMethods_ = true;
  installConsoleBindings(*env.runtime, ctx);

  // Run the test bytecode.
  // Lazy compilation cannot use persistent mode.
  vm::RuntimeModuleFlags rmFlags;
  rmFlags.persistent = !config.lazy;
  auto status = env.runtime->runBytecode(
      std::move(bytecode),
      rmFlags,
      sourceURL,
      vm::Runtime::makeNullHandle<vm::Environment>());

  bool threwException = status == vm::ExecutionStatus::EXCEPTION;
  std::string exceptionMsg;
  if (threwException) {
    exceptionMsg = captureException(*env.runtime);
  }

  // Check for timeout.
  if (threwException &&
      exceptionMsg.find("Javascript execution has timed out") !=
          std::string::npos) {
    if (env.timeLimitMonitor) {
      env.timeLimitMonitor->unwatchRuntime(*env.runtime);
    }
    return makeResult(ResultCode::ExecuteTimeout, "FAIL: Test timed out");
  }

  // Drain microtask queue and task queue while still under timeout protection.
  if (!threwException) {
    if (drainMicrotasks(*env.runtime) == vm::ExecutionStatus::EXCEPTION) {
      exceptionMsg = captureException(*env.runtime);
      threwException = true;
    } else if (drainTaskQueue(*env.runtime, ctx, exceptionMsg)) {
      threwException = true;
    }
  }

  // Unwatch runtime from time limit monitor after all execution is complete.
  if (env.timeLimitMonitor) {
    env.timeLimitMonitor->unwatchRuntime(*env.runtime);
  }

  // Check for timeout during microtask/task draining.
  if (threwException &&
      exceptionMsg.find("Javascript execution has timed out") !=
          std::string::npos) {
    return makeResult(ResultCode::ExecuteTimeout, "FAIL: Test timed out");
  }

  // For async tests, our injected $DONE records its outcome to a global
  // instead of throwing — which lets the failure survive even when
  // called inside a Promise.then callback (whose throw would be wrapped
  // into a rejected promise by Promise machinery). After drain, check:
  //   - asyncTestFailure  → $DONE was called with an error
  //   - asyncTestComplete → $DONE() was called with no error
  //   - neither           → $DONE was never called
  if (isAsync && !threwException) {
    auto global = env.jsiRuntime->global();
    if (global.hasProperty(*env.jsiRuntime, "asyncTestFailure")) {
      exceptionMsg = global.getProperty(*env.jsiRuntime, "asyncTestFailure")
                         .asString(*env.jsiRuntime)
                         .utf8(*env.jsiRuntime);
      threwException = true;
    } else if (!global.hasProperty(*env.jsiRuntime, "asyncTestComplete")) {
      exceptionMsg = "Test262:AsyncTestFailure: $DONE not called";
      threwException = true;
    }
  }

  // Evaluate result based on expectations.
  if (expectError) {
    if (!threwException) {
      return makeResult(
          ResultCode::ExecuteFailed, "FAIL: Expected error but test passed");
    }
    if (!config.lazy && !negative.errorType.empty() &&
        exceptionMsg.find(negative.errorType) == std::string::npos) {
      return makeResult(
          ResultCode::ExecuteFailed,
          "FAIL: Expected " + negative.errorType + " but got: " + exceptionMsg);
    }
    return makeResult(ResultCode::Passed, "PASS (expected error)");
  }

  if (threwException) {
    return makeResult(ResultCode::ExecuteFailed, "FAIL: " + exceptionMsg);
  }

  return makeResult(ResultCode::Passed, "PASS");
}

/// Process a single test entry: read file, parse frontmatter, determine
/// variants, and execute each variant.
void processTestEntry(
    const TestEntry &entry,
    const HarnessCache &harness,
    const Skiplist *skiplist,
    const ExecConfig &config,
    TestResult &slot,
    std::atomic<size_t> &featureSkipped,
    std::atomic<size_t> &permanentFeatureSkipped) {
  // The slot is default-constructed (code = Skipped); each exit path below
  // assigns the appropriate code explicitly so intent is clear at the call
  // site rather than relying on the default.
  slot.testName = entry.fullName;

  // Read test file.
  auto fileBuf = llvh::MemoryBuffer::getFile(entry.path);
  if (!fileBuf) {
    slot.code = ResultCode::Failed;
    slot.message = "FAIL: Cannot read file";
    return;
  }

  llvh::StringRef content = (*fileBuf)->getBuffer();
  TestRecord record = parseFrontmatter(content);

  // Feature-based skipping.
  if (skiplist) {
    for (const auto &feat : record.features) {
      SkipReason reason = skiplist->shouldSkipFeature(feat);
      if (reason != SkipReason::NotSkipped) {
        ++featureSkipped;
        if (reason == SkipReason::PermanentUnsupportedFeature) {
          ++permanentFeatureSkipped;
          slot.code = ResultCode::PermanentlySkipped;
        } else {
          slot.code = ResultCode::Skipped;
        }
        return;
      }
    }
  }

  // Skip module tests — Hermes doesn't support ES module execution.
  if (record.isModule()) {
    ++featureSkipped;
    slot.code = ResultCode::Skipped;
    return;
  }

  // Skip tests that include testIntl.js — Hermes doesn't implement all
  // Intl constructors required by this harness (matching Python runner).
  for (const auto &inc : record.includes) {
    if (inc == "testIntl.js") {
      ++featureSkipped;
      slot.code = ResultCode::Skipped;
      return;
    }
  }

  // Check if handle sanitizer should be disabled for this test.
  bool disableHandleSan =
      skiplist && skiplist->shouldDisableHandleSan(entry.path);

  // Determine variants.
  bool runStrict = !record.isNoStrict() && !record.isRaw();
  bool runNonStrict = !record.isOnlyStrict() && !record.isRaw();
  bool runRaw = record.isRaw();

  // At least one variant must be run.
  assert((runNonStrict || runStrict || runRaw) && "No test variant to run");
  std::vector<std::string> includes = buildTestIncludes(entry, record);

  // Match the Python runner behavior: the compiler's strict mode flag
  // is set based on whether the test CAN run in strict mode (runStrict),
  // not on which variant is currently being run.
  bool compileStrict = runStrict;

  // Run variants in order, short-circuiting on first failure (like the Python
  // runner). Push exactly one result per test file. The variant identity
  // (default/strict/raw) is intentionally not encoded in the test name —
  // matching the Python runner.
  auto runVariant = [&](bool isStrict) -> TestResult {
    std::string source = harness.buildSource(
        includes, record.src, isStrict, record.isAsync(), config.shermes);

    if (config.shermes) {
      return executeTestVariantShermes(
          entry.fullName,
          source,
          compileStrict,
          record.isAsync(),
          record.negative,
          config.timeoutSeconds,
          config.optimize,
          disableHandleSan,
          config.shermesBinary,
          config.shermesExtraFlags);
    }

    return executeTestVariant(
        entry.fullName,
        source,
        entry.path,
        compileStrict,
        record.isAsync(),
        record.negative,
        config,
        disableHandleSan);
  };

  TestResult lastResult;
  if (runRaw) {
    lastResult = runVariant(false);
  } else {
    if (runNonStrict) {
      lastResult = runVariant(false);
      if (lastResult.code != ResultCode::Passed) {
        slot = std::move(lastResult);
        return;
      }
    }
    if (runStrict) {
      lastResult = runVariant(true);
    }
  }

  slot = std::move(lastResult);
}

/// Background reporter that polls a completion counter on a fixed interval and
/// prints the same percentage-based progress that the previous reportProgress()
/// emitted: "Testing: 0 .. 10.. 20.. ... 90.. done.\n". Owns a single thread
/// so workers stay off the I/O path entirely. Construction prints the prefix,
/// destruction signals stop, joins, and emits the trailing newline.
class ProgressReporter {
 public:
  ProgressReporter(
      const std::atomic<size_t> &counter,
      size_t total,
      std::chrono::milliseconds interval = std::chrono::milliseconds(200))
      : counter_(counter), total_(total), interval_(interval) {
    llvh::outs() << "Testing: 0 ..";
    llvh::outs().flush();
    thread_ = std::thread([this] { run(); });
  }

  ~ProgressReporter() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_one();
    thread_.join();
    llvh::outs() << " \n";
  }

  ProgressReporter(const ProgressReporter &) = delete;
  ProgressReporter &operator=(const ProgressReporter &) = delete;

 private:
  void run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
      cv_.wait_for(lock, interval_, [this] { return stop_; });
      // catchUp() reads/writes only this thread's lastPrintedPct_, so no
      // additional synchronization is needed beyond the loop's own mutex.
      catchUp();
    }
  }

  /// Print every 10% milestone between lastPrintedPct_ and the current
  /// percentage. Single-writer to lastPrintedPct_ (the reporter thread).
  void catchUp() {
    if (total_ == 0)
      return;
    size_t done = counter_.load(std::memory_order_relaxed);
    unsigned pct = (unsigned)(done * 100 / total_);
    unsigned milestone = (pct / 10) * 10;
    while (lastPrintedPct_ < milestone) {
      lastPrintedPct_ += 10;
      if (lastPrintedPct_ == 100)
        llvh::outs() << " done.";
      else
        llvh::outs() << " " << lastPrintedPct_ << "..";
    }
    llvh::outs().flush();
  }

  const std::atomic<size_t> &counter_;
  size_t total_;
  std::chrono::milliseconds interval_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  unsigned lastPrintedPct_ = 0;
  std::thread thread_;
};

} // namespace

std::unique_ptr<hbc::BCProvider> compileSource(
    llvh::StringRef source,
    llvh::StringRef sourceURL,
    bool strict,
    bool optimize,
    bool lazy,
    std::string &errorMsg) {
  // Non-owning view over the caller's source string. Safe because the source
  // outlives this BCProvider: the caller's std::string lives until runVariant
  // returns, while the BCProvider is constructed here, handed to runBytecode
  // via executeCompiledTest, and destroyed with the per-test runtime before
  // executeCompiledTest returns. std::string::data() is null-terminated
  // (C++11), satisfying getMemBuffer's RequiresNullTerminator default.
  auto llvmBuf = llvh::MemoryBuffer::getMemBuffer(source, sourceURL);
  auto buf = std::make_unique<OwnedMemoryBuffer>(std::move(llvmBuf));

  hbc::CompileFlags flags;
  flags.strict = strict;
  flags.test262 = true;
  flags.emitAsyncBreakCheck = true;
  flags.enableGenerator = true;
  flags.enableAsyncGenerators = true;
  flags.enableES6BlockScoping = true;
  flags.enableTDZ = true;
  flags.lazy = lazy;

  auto [provider, error] = hbc::BCProviderFromSrc::create(
      std::move(buf),
      sourceURL,
      /*sourceMap=*/"",
      flags,
      /*topLevelFunctionName=*/"global",
      /*diagHandler=*/{},
      /*diagContext=*/nullptr,
      optimize ? hbc::fullOptimizationPipeline
               : std::function<void(Module &)>{});

  if (!provider) {
    errorMsg = error;
    return nullptr;
  }

  return std::move(provider);
}

std::vector<std::string> buildTestIncludes(
    const TestEntry &entry,
    const TestRecord &record) {
  std::vector<std::string> includes;
  if (!record.isRaw()) {
    if (entry.suiteKind == SuiteKind::Test262) {
      includes.push_back("sta.js");
      includes.push_back("assert.js");
    }
    for (const auto &inc : record.includes) {
      includes.push_back(inc);
    }
    if (record.isAsync()) {
      includes.push_back("doneprintHandle.js");
    }
  }
  return includes;
}

TestResult executeTestVariant(
    llvh::StringRef testName,
    llvh::StringRef source,
    llvh::StringRef sourceURL,
    bool isStrict,
    bool isAsync,
    const NegativeExpectation &negative,
    const ExecConfig &config,
    bool disableHandleSan) {
  auto startTime = Clock::now();

  auto makeResult = [&](ResultCode code, const std::string &msg) {
    auto endTime = Clock::now();
    TestResult r;
    r.testName = testName;
    r.code = code;
    r.message = msg;
    r.duration = std::chrono::duration_cast<std::chrono::microseconds>(
        endTime - startTime);
    return r;
  };

  bool expectCompileError = negative.phase == "parse";
  bool expectResolutionError = negative.phase == "resolution";

  // Compile the source.
  // Note: enableJIT/forceJIT are runtime-only settings and don't affect
  // compilation — they are passed through to createTestRuntime instead.
  std::string compileError;
  auto bytecode = compileSource(
      source, sourceURL, isStrict, config.optimize, config.lazy, compileError);

  if (!bytecode) {
    if (expectCompileError || expectResolutionError) {
      return makeResult(ResultCode::Passed, "PASS (expected compile error)");
    }
    return makeResult(
        ResultCode::CompileFailed, "FAIL: Compilation failed: " + compileError);
  }

  // Eager mode: a parse/resolution-expected test that compiled cleanly is a
  // hard failure. Skip this check under --lazy because lazy compilation defers
  // function-body parsing, so a successful create() does not prove the source
  // is well-formed. Mirror Python's lazy path, which has no compile-side check
  // at all — defer the phase decision to executeCompiledTest.
  if (!config.lazy && (expectCompileError || expectResolutionError)) {
    return makeResult(
        ResultCode::ExecuteFailed,
        "FAIL: Expected compile error but compilation succeeded");
  }

  // Check if compilation already exceeded the timeout budget.
  if (config.timeoutSeconds > 0) {
    auto elapsed = Clock::now() - startTime;
    if (elapsed >= std::chrono::seconds(config.timeoutSeconds)) {
      return makeResult(
          ResultCode::CompileTimeout, "FAIL: Compilation timed out");
    }
  }

#ifndef _WIN32
  // Crash guard: catch SIGSEGV/SIGABRT/SIGBUS during bytecode execution
  // and convert to a test failure. On crash, the Hermes runtime and all
  // its state are leaked — destructors cannot run safely after siglongjmp.
  // The TimeLimitMonitor is also leaked — unwatchRuntime() is never called,
  // so its background thread retains a reference to the dead runtime. This
  // is harmless: the runtime memory is still allocated (leaked, not freed),
  // so any write from the monitor is a no-op on valid memory.
  // Note: after a crash, the worker thread continues with subsequent tests.
  // If the crash corrupted global heap metadata, later allocations on this
  // thread may fail, but the second crash will fall through to SIG_DFL
  // (active is 0) and terminate the process.
  tCrashGuard.caughtSignal = 0;
  if (sigsetjmp(tCrashGuard.jmpBuf, 1) != 0) {
    tCrashGuard.active = 0;
    int sig = tCrashGuard.caughtSignal;
    return makeResult(
        ResultCode::ExecuteFailed,
        std::string("FAIL: Execution crashed (") + crashSignalName(sig) + ")");
  }
  tCrashGuard.active = 1;
#endif

  auto result = executeCompiledTest(
      testName,
      std::move(bytecode),
      sourceURL,
      isAsync,
      negative,
      config,
      disableHandleSan,
      startTime);

#ifndef _WIN32
  tCrashGuard.active = 0;
#endif

  return result;
}

namespace {

/// Result of running a subprocess.
struct SubprocessResult {
  int exitCode;
  std::string stdoutStr;
  std::string stderrStr;
  /// True only for genuine timeouts (not signal crashes).
  bool timedOut;
  bool execFailed;
  /// Error message from ExecuteAndWait (signal name on crash, "timed out"
  /// on timeout).
  std::string errMsg;
};

/// Run a program as a subprocess, capturing stdout and stderr.
/// Uses LLVM's ExecuteAndWait with file redirects for output capture.
SubprocessResult runProcess(
    llvh::StringRef program,
    llvh::ArrayRef<llvh::StringRef> args,
    unsigned timeoutSeconds) {
  SubprocessResult result{};
  result.exitCode = -1;
  result.timedOut = false;
  result.execFailed = false;

  // Create temp files for stdout and stderr capture.
  llvh::SmallString<128> stdoutPath, stderrPath;
  if (llvh::sys::fs::createTemporaryFile("test-stdout", "txt", stdoutPath)) {
    result.execFailed = true;
    result.stderrStr = "Failed to create temp files for output capture";
    return result;
  }
  if (llvh::sys::fs::createTemporaryFile("test-stderr", "txt", stderrPath)) {
    llvh::sys::fs::remove(stdoutPath);
    result.execFailed = true;
    result.stderrStr = "Failed to create temp files for output capture";
    return result;
  }
  auto cleanupRedirects = llvh::make_scope_exit([&] {
    llvh::sys::fs::remove(stdoutPath);
    llvh::sys::fs::remove(stderrPath);
  });

  // Set up redirects: stdin=/dev/null, stdout/stderr to temp files.
  llvh::Optional<llvh::StringRef> redirects[] = {
      llvh::StringRef(""), // stdin -> /dev/null
      llvh::StringRef(stdoutPath), // stdout -> file
      llvh::StringRef(stderrPath), // stderr -> file
  };

  std::string errMsg;
  bool execFailed = false;
  int rc = llvh::sys::ExecuteAndWait(
      program,
      args,
      /*Env=*/llvh::None,
      redirects,
      timeoutSeconds,
      /*MemoryLimit=*/0,
      &errMsg,
      &execFailed);

  result.execFailed = execFailed;
  result.exitCode = rc;
  result.errMsg = errMsg;
  // ExecuteAndWait returns -2 for BOTH timeout and signal termination
  // (SIGSEGV, SIGABRT, etc.). Distinguish using ErrMsg: LLVM sets it to
  // "Child timed out" on timeout, and to the signal name (e.g.,
  // "Segmentation fault") on crash.
  result.timedOut = (rc == -2 && errMsg.find("timed out") != std::string::npos);

  // Read captured output.
  if (auto buf = llvh::MemoryBuffer::getFile(stdoutPath))
    result.stdoutStr = (*buf)->getBuffer().str();
  if (auto buf = llvh::MemoryBuffer::getFile(stderrPath))
    result.stderrStr = (*buf)->getBuffer().str();

  return result;
}

} // namespace

TestResult executeTestVariantShermes(
    llvh::StringRef testName,
    llvh::StringRef source,
    bool isStrict,
    bool isAsync,
    const NegativeExpectation &negative,
    unsigned timeoutSeconds,
    bool optimize,
    bool disableHandleSan,
    const std::string &shermesBinary,
    const std::vector<std::string> &shermesExtraFlags) {
  using Clock = std::chrono::steady_clock;
  auto startTime = Clock::now();

  auto makeResult = [&](ResultCode code, const std::string &msg) {
    TestResult r;
    r.testName = testName;
    r.code = code;
    r.message = msg;
    r.duration = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - startTime);
    return r;
  };

  bool hasNegative = !negative.phase.empty();
  bool expectCompileError = hasNegative && negative.phase == "parse";
  bool expectResolutionError = hasNegative && negative.phase == "resolution";
  bool expectRuntimeError = hasNegative && negative.phase == "runtime";

  // Write preprocessed source to temp file.
  llvh::SmallString<128> sourcePath;
  int sourceFD;
  if (llvh::sys::fs::createTemporaryFile("test", "js", sourceFD, sourcePath)) {
    return makeResult(ResultCode::Failed, "FAIL: Cannot create temp file");
  }
  {
    llvh::raw_fd_ostream sourceFile(sourceFD, /*shouldClose=*/true);
    sourceFile << source;
  }

  // Output binary path.
  llvh::SmallString<128> binaryPath(sourcePath);
  binaryPath += ".out";

  // Clean up temp files on all return paths.
  auto cleanup = llvh::make_scope_exit([&] {
    llvh::sys::fs::remove(sourcePath);
    llvh::sys::fs::remove(binaryPath);
  });

  // Step 1: Compile with shermes.
  // Matches Python runner: shermes <source> -o <binary> <COMPILE_ARGS>
  //   [-strict] [-O|-O0]
  std::vector<std::string> compileArgStorage;
  compileArgStorage.push_back(shermesBinary);
  compileArgStorage.push_back(std::string(sourcePath.str()));
  compileArgStorage.push_back("-o");
  compileArgStorage.push_back(std::string(binaryPath.str()));
  // COMPILE_ARGS from Python runner.
  compileArgStorage.push_back("-test262");
  compileArgStorage.push_back("-fno-static-builtins");
  compileArgStorage.push_back("-Xes6-block-scoping");
  compileArgStorage.push_back("-Xenable-tdz");
  compileArgStorage.push_back("-Xasync-generators");
  if (isStrict)
    compileArgStorage.push_back("-strict");
  compileArgStorage.push_back(optimize ? "-O" : "-O0");
  for (const auto &flag : shermesExtraFlags)
    compileArgStorage.push_back(flag);

  std::vector<llvh::StringRef> compileArgs;
  for (const auto &arg : compileArgStorage)
    compileArgs.push_back(arg);

  auto compileResult = runProcess(shermesBinary, compileArgs, timeoutSeconds);

  if (compileResult.timedOut) {
    return makeResult(
        ResultCode::CompileTimeout, "FAIL: Compilation timed out");
  }

  if (compileResult.exitCode != 0 || compileResult.execFailed) {
    if (expectCompileError || expectResolutionError) {
      return makeResult(ResultCode::Passed, "PASS (expected compile error)");
    }
    // Include signal info (errMsg) when the compiler was killed by a signal.
    std::string detail = compileResult.stderrStr;
    if (!compileResult.errMsg.empty())
      detail = compileResult.errMsg + "\n" + detail;
    return makeResult(
        ResultCode::CompileFailed, "FAIL: Compilation failed: " + detail);
  }

  if (expectCompileError || expectResolutionError) {
    return makeResult(
        ResultCode::ExecuteFailed,
        "FAIL: Expected compile error but compilation succeeded");
  }

  // Step 2: Run the compiled binary.
  // Matches Python runner: <binary> [-Xes6-proxy]
  //   [-Xhermes-internal-test-methods] [-Xmicrotask-queue]
  //   [-gc-sanitize-handles=0]
  std::vector<std::string> runArgStorage;
  runArgStorage.push_back(std::string(binaryPath.str()));
  runArgStorage.push_back("-Xes6-proxy");
  runArgStorage.push_back("-Xhermes-internal-test-methods");
  runArgStorage.push_back("-Xmicrotask-queue");
  if (disableHandleSan)
    runArgStorage.push_back("-gc-sanitize-handles=0");

  std::vector<llvh::StringRef> runArgs;
  for (const auto &arg : runArgStorage)
    runArgs.push_back(arg);

  auto runResult = runProcess(binaryPath.str(), runArgs, timeoutSeconds);

  if (runResult.timedOut) {
    return makeResult(ResultCode::ExecuteTimeout, "FAIL: Test timed out");
  }

  // Match Python runner's result evaluation logic for non-lazy mode.
  if (runResult.exitCode != 0) {
    if (runResult.exitCode < 0) {
      // Signal termination or exec failure.
      std::string detail = "FAIL: Execution terminated";
      if (!runResult.errMsg.empty())
        detail += ": " + runResult.errMsg;
      return makeResult(ResultCode::ExecuteFailed, detail);
    }
    if (!expectRuntimeError) {
      return makeResult(
          ResultCode::ExecuteFailed,
          "FAIL: Execution threw unexpected error: " + runResult.stderrStr);
    }
    return makeResult(ResultCode::Passed, "PASS (expected runtime error)");
  }

  // Exit code 0. For async tests, doneprintHandle.js's $DONE writes
  // "Test262:AsyncTestComplete" on success and "Test262:AsyncTestFailure:"
  // on failure — both without throwing, so the subprocess exits cleanly
  // either way. Stdout is the only signal the parent has.
  if (isAsync &&
      (runResult.stdoutStr.find("Test262:AsyncTestFailure") !=
           std::string::npos ||
       runResult.stdoutStr.find("Test262:AsyncTestComplete") ==
           std::string::npos)) {
    return makeResult(
        ResultCode::ExecuteFailed,
        "FAIL: Async test failure: " + runResult.stdoutStr);
  }

  if (expectRuntimeError) {
    return makeResult(
        ResultCode::ExecuteFailed,
        "FAIL: Expected runtime error but test passed");
  }

  return makeResult(ResultCode::Passed, "PASS");
}

namespace {

/// Drain \p p (a WorkQueue) until it is finished. Body of a worker thread.
void *workerMain(void *p) {
  auto *queue = static_cast<WorkQueue *>(p);
#ifndef _WIN32
  setupThreadAltStack();
#endif
  std::function<void()> task;
  while (queue->pop(task)) {
    task();
  }
#ifndef _WIN32
  teardownThreadAltStack();
#endif
  return nullptr;
}

} // namespace

void WorkQueue::push(std::function<void()> task) {
  std::lock_guard<std::mutex> lock(mutex_);
  tasks_.push_back(std::move(task));
  cv_.notify_one();
}

bool WorkQueue::pop(std::function<void()> &task) {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this] { return !tasks_.empty() || done_; });
  if (tasks_.empty())
    return false;
  task = std::move(tasks_.front());
  tasks_.pop_front();
  return true;
}

void WorkQueue::finish() {
  std::lock_guard<std::mutex> lock(mutex_);
  done_ = true;
  cv_.notify_all();
}

void runAllTests(
    const std::vector<TestEntry> &tests,
    const HarnessCache &harness,
    const Skiplist *skiplist,
    const ExecConfig &config,
    std::vector<TestResult> &results,
    std::atomic<size_t> &featureSkipped,
    std::atomic<size_t> &permanentFeatureSkipped) {
  std::atomic<size_t> completedCount{0};

  // Pre-size results so each worker can write to a unique index without
  // locking. Default-constructed slots are ResultCode::Skipped — see
  // processTestEntry, which overwrites the slot on every exit path.
  results.resize(tests.size());

  // Suppress stderr for the duration of test execution. GCBase prints
  // a handle sanitization warning on every runtime init when built with
  // -DHERMESVM_SANITIZE_HANDLES=ON, which is very noisy (one per test).
  // Stdout is left untouched so ProgressReporter continues to work.
  // TODO: consider also redirecting stdout if there are other noise
  // outputs. Use dup() to save stdout before redirecting, and pass the
  // saved fd via raw_fd_ostream to all outs() callers in runAllTests.
#ifndef _WIN32
  int savedStderr = dup(STDERR_FILENO);
  int devNull = open("/dev/null", O_WRONLY);
  if (devNull >= 0) {
    dup2(devNull, STDERR_FILENO);
    close(devNull);
  }
  auto restoreStderr = llvh::make_scope_exit([savedStderr] {
    if (savedStderr >= 0) {
      dup2(savedStderr, STDERR_FILENO);
      close(savedStderr);
    }
  });
#endif

  WorkQueue queue;

  unsigned numWorkers = std::min(config.numThreads, (unsigned)tests.size());
  if (numWorkers == 0)
    numWorkers = 1;

#ifndef _WIN32
  installCrashHandlers();

  // Spawn workers with pthread rather than std::thread so the stack size can
  // be set explicitly; see workerStackSize().
  std::vector<pthread_t> workers;
  size_t stackSize = workerStackSize();
  for (unsigned i = 0; i < numWorkers; ++i) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0)
      continue;
    if (stackSize != 0)
      pthread_attr_setstacksize(&attr, stackSize);
    pthread_t tid;
    if (pthread_create(&tid, &attr, workerMain, &queue) == 0)
      workers.push_back(tid);
    pthread_attr_destroy(&attr);
  }
  if (workers.empty()) {
    llvh::outs() << "Error: failed to create worker threads\n";
    return;
  }
#else
  std::vector<std::thread> workers;
  for (unsigned i = 0; i < numWorkers; ++i) {
    workers.emplace_back([&queue] { workerMain(&queue); });
  }
#endif

  {
    // Reporter lifetime brackets the work-dispatch window. Construction
    // prints "Testing: 0 .."; destruction signals stop, joins the polling
    // thread, and prints the trailing " \n".
    ProgressReporter reporter(completedCount, tests.size());

    for (size_t i = 0; i < tests.size(); ++i) {
      queue.push([i,
                  &tests,
                  &harness,
                  skiplist,
                  &config,
                  &results,
                  &completedCount,
                  &featureSkipped,
                  &permanentFeatureSkipped] {
        processTestEntry(
            tests[i],
            harness,
            skiplist,
            config,
            results[i],
            featureSkipped,
            permanentFeatureSkipped);
        ++completedCount;
      });
    }

    queue.finish();
    for (auto &w : workers) {
#ifndef _WIN32
      pthread_join(w, nullptr);
#else
      w.join();
#endif
    }
  }
}

} // namespace testrunner
} // namespace hermes

/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/hermes.h>

#include "llvh/Support/Compiler.h"

#include "hermes/ADT/ManagedChunkedList.h"
#include "hermes/BCGen/HBC/BCProvider.h"
#include "hermes/BCGen/HBC/BytecodeFileFormat.h"
#include "hermes/BCGen/HBC/BytecodeStream.h"
#include "hermes/BCGen/HBC/HBC.h"
#include "hermes/DebuggerAPI.h"
#include "hermes/Platform/Logging.h"
#include "hermes/Public/JSOutOfMemoryError.h"
#include "hermes/Public/RuntimeConfig.h"
#include "hermes/SourceMap/SourceMapParser.h"
#include "hermes/Support/Buffer.h"
#include "hermes/Support/SerialExecutor.h"
#include "hermes/Support/SimpleDiagHandler.h"
#include "hermes/Support/UTF16Stream.h"
#include "hermes/Support/UTF8.h"
#include "hermes/VM/CallResult.h"
#include "hermes/VM/Debugger/Debugger.h"
#include "hermes/VM/GC.h"
#include "hermes/VM/HostModel.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/JSError.h"
#include "hermes/VM/JSLib.h"
#include "hermes/VM/JSLib/JSLibStorage.h"
#include "hermes/VM/JSLib/RuntimeJSONParse.h"
#include "hermes/VM/JSTypedArray.h"
#include "hermes/VM/NativeState.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/Profiler/CodeCoverageProfiler.h"
#include "hermes/VM/Profiler/SamplingProfiler.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/SerializedValue.h"
#include "hermes/VM/StaticHUtils.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringView.h"
#include "hermes/VM/SymbolID.h"
#include "hermes/VM/TimeLimitMonitor.h"
#include "hermes/VM/VMExperiments.h"
#include "hermes/VM/WeakRoot-inline.h"

#if HERMES_ENABLE_CORE_EXTENSIONS
#include "extensions/Extensions.h"
#include "extensions/ExtensionsBytecode.h"
#endif

#include "llvh/Support/ConvertUTF.h"
#include "llvh/Support/ErrorHandling.h"
#include "llvh/Support/FileSystem.h"
#include "llvh/Support/SHA1.h"
#include "llvh/Support/raw_os_ostream.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <list>
#include <mutex>
#include <system_error>
#include <type_traits>
#include <unordered_map>

#include <jsi/instrumentation.h>
#include <jsi/threadsafe.h>

#ifdef __ANDROID__
#include <fbjni/fbjni.h>
#endif

#ifdef HERMESVM_LLVM_PROFILE_DUMP
extern "C" {
int __llvm_profile_dump(void);
}
#endif

// Android OSS has a bug where exception data can get mangled when going via
// fbjni. This macro can be used to expose the root cause in adb log. It serves
// no purpose other than as a backup.
#ifdef __ANDROID__
#define LOG_EXCEPTION_CAUSE(...) hermesLog("HermesVM", __VA_ARGS__)
#else
#define LOG_EXCEPTION_CAUSE(...) \
  do {                           \
  } while (0)
#endif

namespace vm = hermes::vm;
namespace hbc = hermes::hbc;
using ::hermes::hermesLog;

namespace facebook {
namespace hermes {
namespace detail {

#if !defined(NDEBUG) && !defined(ASSERT_ON_DANGLING_VM_REFS)
#define ASSERT_ON_DANGLING_VM_REFS
#endif

static void (*sApiFatalHandler)(const std::string &) = nullptr;
/// Handler called by HermesVM to report unrecoverable errors.
/// This is a forward declaration to prevent a compiler warning.
void hermesFatalErrorHandler(
    void *user_data,
    const std::string &reason,
    bool gen_crash_diag);

void hermesFatalErrorHandler(
    void * /*user_data*/,
    const std::string &reason,
    bool /*gen_crash_diag*/) {
  // Copied from ErrorHandling.cpp.
  // Blast the result out to stderr.  We don't try hard to make sure this
  // succeeds (e.g. handling EINTR) and we can't use errs() here because
  // raw ostreams can call report_fatal_error.
  llvh::SmallVector<char, 64> buffer;
  llvh::raw_svector_ostream OS(buffer);
  OS << "LLVM ERROR: " << reason << "\n";
  llvh::StringRef messageStr = OS.str();
  ssize_t written = ::write(2, messageStr.data(), messageStr.size());
  (void)written; // If something went wrong, we deliberately just give up.

  if (sApiFatalHandler) {
    try {
      sApiFatalHandler(reason);
    } catch (...) {
      // Catch the exception and do nothing as the fatal handler must not throw
      // exceptions.
    }
  } else {
#ifdef HERMES_IS_MOBILE_BUILD
    // Actually crash and let breakpad handle the reporting.
    *((volatile int *)nullptr) = 42;
#endif
  }
  abort();
}

} // namespace detail

namespace {

void raw_ostream_append(llvh::raw_ostream &os) {}

template <typename Arg0, typename... Args>
void raw_ostream_append(llvh::raw_ostream &os, Arg0 &&arg0, Args &&...args) {
  os << arg0;
  raw_ostream_append(os, args...);
}

/// HermesVM uses the LLVM fatal error handle to report fatal errors. This
/// wrapper helps us install the handler at construction time, before any
/// HermesVM code has been invoked.
class InstallHermesFatalErrorHandler {
 public:
  InstallHermesFatalErrorHandler() {
    // The LLVM fatal error handler can only be installed once. Use a Meyer's
    // singleton to guarantee it - the static "dummy" is guaranteed by the
    // compiler to be initialized no more than once.
    static int dummy = ([]() {
      llvh::install_fatal_error_handler(detail::hermesFatalErrorHandler);
      return 0;
    })();
    (void)dummy;
  }
};

#if !HERMESVM_SAMPLING_PROFILER_AVAILABLE
void throwHermesNotCompiledWithSamplingProfilerSupport() {
  throw std::logic_error(
      "Hermes was not compiled with SamplingProfiler support");
}
#endif // !HERMESVM_SAMPLING_PROFILER_AVAILABLE

struct UUIDInfo : llvh::DenseMapInfo<jsi::UUID> {
  static inline jsi::UUID getEmptyKey() {
    return jsi::UUID{};
  }
  static inline jsi::UUID getTombstoneKey() {
    return jsi::UUID{0xffffffff, 0xffff, 0xffff, 0xffff, 0xfffffffffff};
  }
  static inline bool isEqual(const jsi::UUID &a, const jsi::UUID &b) {
    return a == b;
  }
  static unsigned getHashValue(const jsi::UUID &Val) {
    return jsi::UUID::Hash()(Val);
  }
};

#ifdef JSI_UNSTABLE
/// HermesSerialized represents a jsi::Serialized object produced by the Hermes
/// runtime. HermesSerialized is hidden from users, who should remain unaware of
/// the serialized type. However, it provides the implementation of the
/// `getPrivate` function that allows Hermes runtimes to obtain the
/// vm::SerializedValue object that actually holds the serialized data.
class HermesSerialized final : public jsi::Serialized {
 public:
  vm::SerializedValue serialized_;

  HermesSerialized(vm::SerializedValue &serializedValue)
      : serialized_(std::move(serializedValue)) {}

  static const void *getHermesSerializedSecret() {
    static const uint8_t hidden = 0;
    return &hidden;
  }

  void *getPrivate(const void *secretAddr) override {
    if (secretAddr == getHermesSerializedSecret()) {
      return &serialized_;
    }
    return nullptr;
  }
};
#endif
} // namespace

class HermesRootAPI final : public IHermesRootAPI, public ISetFatalHandler {
 public:
  jsi::ICast *castInterface(const jsi::UUID &interfaceUUID) override;

  std::unique_ptr<HermesRuntime> makeHermesRuntime(
      const ::hermes::vm::RuntimeConfig &runtimeConfig) override;

  bool isHermesBytecode(const uint8_t *data, size_t len) override;
  uint32_t getBytecodeVersion() override;
  void prefetchHermesBytecode(const uint8_t *data, size_t len) override;
  bool hermesBytecodeSanityCheck(
      const uint8_t *data,
      size_t len,
      std::string *errorMessage = nullptr) override;
  std::pair<const uint8_t *, size_t> getBytecodeEpilogue(
      const uint8_t *data,
      size_t len) override;
  void setFatalHandler(void (*handler)(const std::string &)) override;
  void enableSamplingProfiler(double meanHzFreq = 100) override;
  void disableSamplingProfiler() override;
  void dumpSampledTraceToFile(const std::string &fileName) override;
  void dumpSampledTraceToStream(std::ostream &stream) override;
  std::unordered_map<std::string, std::vector<std::string>>
  getExecutedFunctions() override;
  bool isCodeCoverageProfilerEnabled() override;
  void enableCodeCoverageProfiler() override;
  void disableCodeCoverageProfiler() override;
};

namespace {
using EventLoopCallback = const std::function<void()>;
using ScheduleCallbackFunc = std::function<void(EventLoopCallback)>;

/// Return the runner to install on the finalizer worker thread when the
/// embedder did not configure one: a JNI ThreadScope wrapper on Android, and
/// an empty runner, which leaves the thread unwrapped, everywhere else.
vm::ThreadRunner makeDefaultFinalizerThreadRunner() {
#ifdef __ANDROID__
  return [](std::function<void()> run) {
    // Finalizers may release JNI references, which requires the thread to be
    // attached to the JVM. ThreadScope's constructor throws when fbjni has no
    // JavaVM. Nothing may escape into SerialExecutor, which is compiled
    // without exceptions, so report any failure as a Hermes fatal error
    // instead of letting it terminate the process without a diagnostic.
    try {
      const facebook::jni::ThreadScope scope;
      run();
    } catch (const std::exception &e) {
      ::hermes::hermes_fatal(
          std::string("Exception on the JSI finalizer thread: ") + e.what());
    } catch (...) {
      ::hermes::hermes_fatal("Unknown exception on the JSI finalizer thread");
    }
  };
#else
  return {};
#endif
}

/// Return the embedder's finalizer thread runner, falling back to the platform
/// default when none was configured. A configured runner is used as-is, so an
/// explicitly empty one suppresses the platform default.
vm::ThreadRunner finalizerThreadRunner(const vm::RuntimeConfig &runtimeConfig) {
  auto runner = runtimeConfig.getFinalizerThreadRunner();
  return runner ? std::move(*runner) : makeDefaultFinalizerThreadRunner();
}

class HermesRuntimeImpl final : public HermesRuntime,
                                private IHermesTestHelpers,
                                private IStringWriter,
                                private InstallHermesFatalErrorHandler,
                                private jsi::Instrumentation,
                                public ISetEventLoopControl
#ifdef JSI_UNSTABLE
    ,
                                public jsi::ISerialization,
                                private IHermesTracingHelpers
#endif
{
 public:
  HermesRuntimeImpl(const vm::RuntimeConfig &runtimeConfig)
      : hermesValues_(runtimeConfig.getGCConfig().getOccupancyTarget()),
        weakHermesValues_(runtimeConfig.getGCConfig().getOccupancyTarget()),
        rt_(::hermes::vm::Runtime::create(runtimeConfig)),
        runtime_(*rt_),
        vmExperimentFlags_(runtimeConfig.getVMExperimentFlags()),
        finalizerExecutor_(
            0,
            ::hermes::SerialExecutor::kDefaultTimeout,
            finalizerThreadRunner(runtimeConfig)),
        mutatorScope{*this} {
#ifdef HERMES_ENABLE_DEBUGGER
    compileFlags_.debug = true;
#endif

    switch (runtimeConfig.getCompilationMode()) {
      case vm::SmartCompilation:
        compileFlags_.lazy = true;
        // (Leaves thresholds at default values)
        break;
      case vm::ForceEagerCompilation:
        compileFlags_.lazy = false;
        break;
      case vm::ForceLazyCompilation:
        compileFlags_.lazy = true;
        compileFlags_.preemptiveFileCompilationThreshold = 0;
        compileFlags_.preemptiveFunctionCompilationThreshold = 0;
        break;
    }

    compileFlags_.enableGenerator = runtimeConfig.getEnableGenerator();
    compileFlags_.enableES6BlockScoping = runtimeConfig.getES6BlockScoping();
    compileFlags_.enableAsyncGenerators =
        runtimeConfig.getEnableAsyncGenerators();
    compileFlags_.emitAsyncBreakCheck =
        runtimeConfig.getAsyncBreakCheckInEval();
    runtime_.addCustomRootsFunction(
        [this](vm::GC *, vm::RootAcceptor &acceptor) {
          hermesValues_.forEach([&acceptor](HermesPointerValue &element) {
            acceptor.accept(element.value());
          });
        });
    runtime_.addCustomWeakRootsFunction(
        [this](vm::GC *, vm::WeakRootAcceptor &acceptor) {
          weakHermesValues_.forEach([&acceptor](WeakRefPointerValue &element) {
            // We need to use valueUnsafe() because the refernece count may
            // be decremented to 0 after we check that it is not free and hence
            // fires the assertion in value(). It is safe to accept the value
            // here because it is not freed before the iteration begings and
            // the iteration will not destroy the WeakRoot.
            acceptor.acceptWeak(element.valueUnsafe());
          });
        });
#ifdef HERMES_MEMORY_INSTRUMENTATION
    runtime_.addCustomSnapshotFunction(
        [this](vm::HeapSnapshot &snap) {
          snap.beginNode();
          snap.endNode(
              vm::HeapSnapshot::NodeType::Native,
              "ManagedValues",
              vm::GCBase::IDTracker::reserved(
                  vm::GCBase::IDTracker::ReservedObjectID::JSIHermesValueList),
              hermesValues_.capacity() * sizeof(HermesPointerValue),
              0);
          snap.beginNode();
          snap.endNode(
              vm::HeapSnapshot::NodeType::Native,
              "ManagedValues",
              vm::GCBase::IDTracker::reserved(
                  vm::GCBase::IDTracker::ReservedObjectID::
                      JSIWeakHermesValueList),
              weakHermesValues_.capacity() * sizeof(WeakRefPointerValue),
              0);
        },
        [](vm::HeapSnapshot &snap) {
          snap.addNamedEdge(
              vm::HeapSnapshot::EdgeType::Internal,
              "hermesValues",
              vm::GCBase::IDTracker::reserved(
                  vm::GCBase::IDTracker::ReservedObjectID::JSIHermesValueList));
          snap.addNamedEdge(
              vm::HeapSnapshot::EdgeType::Internal,
              "weakHermesValues",
              vm::GCBase::IDTracker::reserved(
                  vm::GCBase::IDTracker::ReservedObjectID::
                      JSIWeakHermesValueList));
        });
#endif // HERMES_MEMORY_INSTRUMENTATION
  }

 public:
  ~HermesRuntimeImpl() override {
#ifdef HERMES_ENABLE_DEBUGGER
    // Deallocate the debugger so it frees any HermesPointerValues it may hold.
    // This must be done before we check hermesValues_ below.
    debugger_.reset();
#endif
    // Free the resources in the custom data map
    for (auto [_, entry] : dataMap_) {
      auto *deleter = entry.second;
      deleter(entry.first);
    }

    // Release the VM runtime. This needs to happen before the
    // finalizerExecutor_ is destroyed so that the VM runtime has a chance to
    // queue all clean-up tasks.
    rt_.reset();
  }

  // This should only be called once by the factory.
  void setDebugger(std::unique_ptr<debugger::Debugger> d) {
    debugger_ = std::move(d);
  }

  template <typename T>
  T add(::hermes::vm::HermesValue hv) {
    static_assert(
        std::is_base_of<jsi::Pointer, T>::value, "this type cannot be added");
    return make<T>(&hermesValues_.add(hv));
  }

  jsi::WeakObject addWeak(::hermes::vm::WeakRoot<vm::JSObject> wr) {
    return make<jsi::WeakObject>(&weakHermesValues_.add(wr));
  }

  // overriden from jsi::Instrumentation
  std::string getRecordedGCStats() override {
    std::string s;
    llvh::raw_string_ostream os(s);
    runtime_.printHeapStats(os);
    return os.str();
  }

  // Overridden from jsi::Instrumentation
  // See include/hermes/VM/GCBase.h for documentation of the fields
  std::unordered_map<std::string, int64_t> getHeapInfo(
      bool includeExpensive) override {
    vm::GCBase::HeapInfo info;
    if (includeExpensive) {
      runtime_.getHeap().getHeapInfoWithMallocSize(info);
    } else {
      runtime_.getHeap().getHeapInfo(info);
    }
#ifndef NDEBUG
    vm::GCBase::DebugHeapInfo debugInfo;
    runtime_.getHeap().getDebugHeapInfo(debugInfo);
#endif

    std::unordered_map<std::string, int64_t> jsInfo;

#define BRIDGE_INFO(TYPE, HOLDER, NAME) \
  jsInfo["hermes_" #NAME] = static_cast<TYPE>(HOLDER.NAME);

    BRIDGE_INFO(int, info, numCollections);
    BRIDGE_INFO(double, info, totalAllocatedBytes);
    BRIDGE_INFO(double, info, allocatedBytes);
    BRIDGE_INFO(double, info, heapSize);
    BRIDGE_INFO(double, info, va);
    BRIDGE_INFO(double, info, externalBytes);
    if (includeExpensive) {
      BRIDGE_INFO(double, info, mallocSizeEstimate);
    }

#ifndef NDEBUG
    BRIDGE_INFO(int, debugInfo, numAllocatedObjects);
    BRIDGE_INFO(int, debugInfo, numReachableObjects);
    BRIDGE_INFO(int, debugInfo, numCollectedObjects);
    BRIDGE_INFO(int, debugInfo, numFinalizedObjects);
    BRIDGE_INFO(int, debugInfo, numMarkedSymbols);
    BRIDGE_INFO(int, debugInfo, numHiddenClasses);
    BRIDGE_INFO(int, debugInfo, numLeafHiddenClasses);
#endif

#undef BRIDGE_INFO

    jsInfo["hermes_peakAllocatedBytes"] =
        runtime_.getHeap().getPeakAllocatedBytes();
    jsInfo["hermes_peakLiveAfterGC"] = runtime_.getHeap().getPeakLiveAfterGC();

#define BRIDGE_GEN_INFO(NAME, STAT_EXPR, FACTOR)                    \
  jsInfo["hermes_full_" #NAME] = info.fullStats.STAT_EXPR * FACTOR; \
  jsInfo["hermes_yg_" #NAME] = info.youngGenStats.STAT_EXPR * FACTOR;

    BRIDGE_GEN_INFO(numCollections, numCollections, 1.0);
    // Times are converted from seconds to milliseconds for the logging pipeline
    // ...
    BRIDGE_GEN_INFO(gcTime, gcWallTime.sum(), 1000);
    BRIDGE_GEN_INFO(maxPause, gcWallTime.max(), 1000);
    BRIDGE_GEN_INFO(gcCPUTime, gcCPUTime.sum(), 1000);
    BRIDGE_GEN_INFO(gcMaxCPUPause, gcCPUTime.max(), 1000);
    // ... and since this is square seconds, we must square the 1000 too.
    BRIDGE_GEN_INFO(gcTimeSquares, gcWallTime.sumOfSquares(), 1000 * 1000);
    BRIDGE_GEN_INFO(gcCPUTimeSquares, gcCPUTime.sumOfSquares(), 1000 * 1000);

#undef BRIDGE_GEN_INFO

    return jsInfo;
  }

  // Overridden from jsi::Instrumentation
  void collectGarbage(std::string cause) override {
    runtime_.collect(std::move(cause));
  }

  // Overridden from jsi::Instrumentation
  void startTrackingHeapObjectStackTraces(
      std::function<void(
          uint64_t,
          std::chrono::microseconds,
          std::vector<HeapStatsUpdate>)> fragmentCallback) override {
#ifdef HERMES_MEMORY_INSTRUMENTATION
    runtime_.enableAllocationLocationTracker(std::move(fragmentCallback));
#else
    throw std::logic_error(
        "Cannot track heap object stack traces if Hermes isn't "
        "built with memory instrumentation.");
#endif
  }

  // Overridden from jsi::Instrumentation
  void stopTrackingHeapObjectStackTraces() override {
#ifdef HERMES_MEMORY_INSTRUMENTATION
    runtime_.disableAllocationLocationTracker();
#else
    throw std::logic_error(
        "Cannot track heap object stack traces if Hermes isn't "
        "built with memory instrumentation.");
#endif
  }

  // Overridden from jsi::Instrumentation
  void startHeapSampling(size_t samplingInterval) override {
#ifdef HERMES_MEMORY_INSTRUMENTATION
    runtime_.enableSamplingHeapProfiler(samplingInterval);
#else
    throw std::logic_error(
        "Cannot perform heap sampling if Hermes isn't built with "
        "memory instrumentation.");
#endif
  }

  // Overridden from jsi::Instrumentation
  void stopHeapSampling(std::ostream &os) override {
#ifdef HERMES_MEMORY_INSTRUMENTATION
    llvh::raw_os_ostream ros(os);
    runtime_.disableSamplingHeapProfiler(ros);
#else
    throw std::logic_error(
        "Cannot perform heap sampling if Hermes isn't built with "
        " memory instrumentation.");
#endif
  }

  // Overridden from jsi::Instrumentation
  void createSnapshotToFile(
      const std::string &path,
      const HeapSnapshotOptions &options) override {
#ifdef HERMES_MEMORY_INSTRUMENTATION
    std::error_code code;
    llvh::raw_fd_ostream os(path, code, llvh::sys::fs::FileAccess::FA_Write);
    if (code) {
      throw std::system_error(code);
    }
    // Taking a snapshot always starts with garbage collection.
    runtime_.getHeap().collect("snapshot");
    runtime_.getHeap().createSnapshot(os, options.captureNumericValue);
#else
    throw std::logic_error(
        "Cannot create heap snapshots if Hermes isn't built with "
        "memory instrumentation.");
#endif
  }

  // Overridden from jsi::Instrumentation
  void createSnapshotToStream(
      std::ostream &os,
      const HeapSnapshotOptions &options) override {
#ifdef HERMES_MEMORY_INSTRUMENTATION
    // Taking a snapshot always starts with garbage collection.
    runtime_.getHeap().collect("snapshot");
    llvh::raw_os_ostream ros(os);
    runtime_.getHeap().createSnapshot(ros, options.captureNumericValue);
#else
    throw std::logic_error(
        "Cannot create heap snapshots if Hermes isn't built with "
        "memory instrumentation.");
#endif
  }

  // Overridden from jsi::Instrumentation
  std::string flushAndDisableBridgeTrafficTrace() override {
    throw std::logic_error(
        "Bridge traffic trace is only supported by TracingRuntime");
  }

  // Overridden from jsi::Instrumentation
  void writeBasicBlockProfileTraceToFile(
      const std::string &fileName) const override {
#ifdef HERMESVM_PROFILER_BB
    std::error_code ec;
    llvh::raw_fd_ostream os(fileName.c_str(), ec, llvh::sys::fs::F_Text);
    if (ec) {
      throw std::system_error(ec);
    }
    runtime_.dumpBasicBlockProfileTrace(os);
#else
    throw std::logic_error(
        "Cannot write the basic block profile trace out if Hermes wasn't built with "
        "hermes.profiler=BB");
#endif
  }

  void dumpOpcodeStats(std::ostream &stream) const override {
#ifdef HERMESVM_PROFILER_OPCODE
    llvh::raw_os_ostream os(stream);
    static_cast<const HermesRuntimeImpl *>(this)->runtime_.dumpOpcodeStats(os);
#else
    throw std::logic_error(
        "Cannot write the basic block profile trace out if Hermes wasn't built with "
        "hermes.profiler=OPCODE");
#endif
  }

  void dumpProfilerSymbolsToFile(const std::string &fileName) const override {
    throw std::logic_error(
        "Cannot dump profiler symbols out if Hermes wasn't built with "
        "hermes.profiler=EXTERN");
  }

  // These are all methods which do pointer type gymnastics and should
  // mostly inline and optimize away.

  static const ::hermes::vm::PinnedHermesValue &phv(
      const jsi::Pointer &pointer) {
    assert(
        dynamic_cast<const HermesPointerValue *>(getPointerValue(pointer)) &&
        "Pointer does not contain a HermesPointerValue");
    return static_cast<const HermesPointerValue *>(getPointerValue(pointer))
        ->value();
  }

  static const ::hermes::vm::PinnedHermesValue &phv(const jsi::Value &value) {
    assert(
        dynamic_cast<const HermesPointerValue *>(getPointerValue(value)) &&
        "Pointer does not contain a HermesPointerValue");
    return static_cast<const HermesPointerValue *>(getPointerValue(value))
        ->value();
  }

  static ::hermes::vm::Handle<vm::StringPrimitive> stringHandle(
      const jsi::String &str) {
    return ::hermes::vm::Handle<vm::StringPrimitive>::vmcast(&phv(str));
  }

  static ::hermes::vm::Handle<::hermes::vm::JSObject> handle(
      const jsi::Object &obj) {
    return ::hermes::vm::Handle<::hermes::vm::JSObject>::vmcast(&phv(obj));
  }

  static ::hermes::vm::Handle<::hermes::vm::JSArrayBuffer> arrayBufferHandle(
      const jsi::ArrayBuffer &arr) {
    return ::hermes::vm::Handle<::hermes::vm::JSArrayBuffer>::vmcast(&phv(arr));
  }

  static ::hermes::vm::Handle<::hermes::vm::JSTypedArrayBase> typedArrayHandle(
      const jsi::TypedArray &arr) {
    return ::hermes::vm::Handle<::hermes::vm::JSTypedArrayBase>::vmcast(
        &phv(arr));
  }

  static const ::hermes::vm::WeakRoot<vm::JSObject> &weakRoot(
      const jsi::Pointer &pointer) {
    assert(
        dynamic_cast<const WeakRefPointerValue *>(getPointerValue(pointer)) &&
        "Pointer does not contain a WeakRefPointerValue");
    return static_cast<const WeakRefPointerValue *>(getPointerValue(pointer))
        ->value();
  }

  // These helpers use public (mostly) interfaces on the runtime and
  // value types to convert between jsi and vm types.

  static vm::HermesValue hvFromValue(const jsi::Value &value) {
    if (value.isUndefined()) {
      return vm::HermesValue::encodeUndefinedValue();
    } else if (value.isNull()) {
      return vm::HermesValue::encodeNullValue();
    } else if (value.isBool()) {
      return vm::HermesValue::encodeBoolValue(value.getBool());
    } else if (value.isNumber()) {
      return vm::HermesValue::encodeUntrustedNumberValue(value.getNumber());
    } else if (
        value.isSymbol() || value.isBigInt() || value.isString() ||
        value.isObject()) {
      return phv(value);
    } else {
      llvm_unreachable("unknown value kind");
    }
  }

  static vm::Handle<> vmHandleFromValue(
      const jsi::Value &value,
      vm::PinnedHermesValue *numberStorage) {
    if (value.isUndefined()) {
      return vm::Runtime::getUndefinedValue();
    } else if (value.isNull()) {
      return vm::Runtime::getNullValue();
    } else if (value.isBool()) {
      return vm::Runtime::getBoolValue(value.getBool());
    } else if (value.isNumber()) {
      *numberStorage =
          vm::HermesValue::encodeUntrustedNumberValue(value.getNumber());
      return vm::Handle<>(numberStorage);
    } else if (
        value.isSymbol() || value.isBigInt() || value.isString() ||
        value.isObject()) {
      return vm::Handle<vm::HermesValue>(&phv(value));
    } else {
      llvm_unreachable("unknown value kind");
    }
  }

  jsi::Value valueFromHermesValue(vm::HermesValue hv) {
    if (hv.isUndefined() || hv.isEmpty()) {
      return jsi::Value::undefined();
    } else if (hv.isNull()) {
      return nullptr;
    } else if (hv.isBool()) {
      return hv.getBool();
    } else if (hv.isDouble()) {
      return hv.getDouble();
    } else if (hv.isSymbol()) {
      return add<jsi::Symbol>(hv);
    } else if (hv.isBigInt()) {
      return add<jsi::BigInt>(hv);
    } else if (hv.isString()) {
      return add<jsi::String>(hv);
    } else if (hv.isObject()) {
      return add<jsi::Object>(hv);
    } else {
      llvm_unreachable("unknown HermesValue type");
    }
  }

  /// Same as \c prepareJavaScript but with a source map.
  std::shared_ptr<const jsi::PreparedJavaScript> prepareJavaScriptWithSourceMap(
      const std::shared_ptr<const jsi::Buffer> &buffer,
      const std::shared_ptr<const jsi::Buffer> &sourceMapBuf,
      std::string sourceURL);

  ICast *castInterface(const jsi::UUID &interfaceUUID) override;

#ifdef JSI_UNSTABLE
  std::shared_ptr<jsi::Serialized> serialize(const jsi::Value &value) override;
  jsi::Value deserialize(
      const std::shared_ptr<jsi::Serialized> &serialized) override;

  std::unique_ptr<jsi::Serialized> serializeWithTransfer(
      const jsi::Value &value,
      const jsi::Array &transferList) override;
  jsi::Array deserializeWithTransfer(
      std::unique_ptr<jsi::Serialized> &serialized) override;

  const vm::SerializedValue *getHermesSerializedValue(
      const jsi::Serialized &serialized) const override;
  const std::shared_ptr<jsi::Serialized> makeSerialized(
      vm::SerializedValue &value) const override;
#endif

  void setEventLoopControl(IEventLoopControl *eventLoopControl) override;
  IEventLoopControl *getEventLoopControl() override;

  // Concrete declarations of jsi::Runtime pure virtual methods
  std::shared_ptr<const jsi::PreparedJavaScript> prepareJavaScript(
      const std::shared_ptr<const jsi::Buffer> &buffer,
      std::string sourceURL) override;
  jsi::Value evaluatePreparedJavaScript(
      const std::shared_ptr<const jsi::PreparedJavaScript> &js) override;
  jsi::Value evaluateJavaScript(
      const std::shared_ptr<const jsi::Buffer> &buffer,
      const std::string &sourceURL) override;
  void queueMicrotask(const jsi::Function &callback) override;
  bool drainMicrotasks(int maxMicrotasksHint = -1) override;
  jsi::Object global() override;

  std::string description() override;
  bool isInspectable() override;
  jsi::Instrumentation &instrumentation() override;

  PointerValue *cloneSymbol(const Runtime::PointerValue *pv) override;
  PointerValue *cloneBigInt(const Runtime::PointerValue *pv) override;
  PointerValue *cloneString(const Runtime::PointerValue *pv) override;
  PointerValue *cloneObject(const Runtime::PointerValue *pv) override;
  PointerValue *clonePropNameID(const Runtime::PointerValue *pv) override;

  jsi::PropNameID createPropNameIDFromAscii(const char *str, size_t length)
      override;
  jsi::PropNameID createPropNameIDFromUtf8(const uint8_t *utf8, size_t length)
      override;
  jsi::PropNameID createPropNameIDFromUtf16(
      const char16_t *utf16,
      size_t length) override;
  jsi::PropNameID createPropNameIDFromString(const jsi::String &str) override;
  jsi::PropNameID createPropNameIDFromSymbol(const jsi::Symbol &sym) override;
  std::string utf8(const jsi::PropNameID &) override;
  bool compare(const jsi::PropNameID &, const jsi::PropNameID &) override;

  std::string symbolToString(const jsi::Symbol &) override;

  jsi::BigInt createBigIntFromInt64(int64_t) override;
  jsi::BigInt createBigIntFromUint64(uint64_t) override;
  bool bigintIsInt64(const jsi::BigInt &) override;
  bool bigintIsUint64(const jsi::BigInt &) override;
  uint64_t truncate(const jsi::BigInt &) override;
  jsi::String bigintToString(const jsi::BigInt &, int) override;

  jsi::String createStringFromAscii(const char *str, size_t length) override;
  jsi::String createStringFromUtf8(const uint8_t *utf8, size_t length) override;
  jsi::String createStringFromUtf16(const char16_t *utf16, size_t length)
      override;
  jsi::String createStringFromAsciiWriter(
      size_t length,
      void *ctx,
      IStringWriter::AsciiWriteFn write) override;
  jsi::String createStringFromUtf16Writer(
      size_t length,
      void *ctx,
      IStringWriter::Utf16WriteFn write) override;
  /// IStringWriter's AsciiWriteFn (char) or Utf16WriteFn (char16_t).
  template <typename CharT>
  using StringWriteFn =
      bool (*)(void *ctx, CharT *dst, size_t offset, size_t count) noexcept;
  /// Shared by the string writers: \p CharT is char (ASCII) or char16_t.
  template <typename CharT>
  jsi::String
  createStringFromWriter(size_t length, void *ctx, StringWriteFn<CharT> write);
  std::string utf8(const jsi::String &) override;
  size_t length(const jsi::String &) override;

  std::u16string utf16(const jsi::String &str) override;
  std::u16string utf16(const jsi::PropNameID &sym) override;

  void getStringData(
      const jsi::String &str,
      void *ctx,
      void (*cb)(void *ctx, bool ascii, const void *data, size_t num)) override;

  void getPropNameIdData(
      const jsi::PropNameID &sym,
      void *ctx,
      void (*cb)(void *ctx, bool ascii, const void *data, size_t num)) override;

  jsi::Value createValueFromJsonUtf8(const uint8_t *json, size_t length)
      override;

  jsi::Object createObject() override;
  jsi::Object createObject(std::shared_ptr<jsi::HostObject> ho) override;
  std::shared_ptr<jsi::HostObject> getHostObject(const jsi::Object &) override;
  jsi::Object createObjectWithPrototype(const jsi::Value &prototype) override;
  jsi::HostFunctionType &getHostFunction(const jsi::Function &) override;
  bool hasNativeState(const jsi::Object &) override;
  std::shared_ptr<jsi::NativeState> getNativeState(
      const jsi::Object &) override;
  void setNativeState(const jsi::Object &, std::shared_ptr<jsi::NativeState>)
      override;
  void setExternalMemoryPressure(const jsi::Object &, size_t) override;
  jsi::Value getProperty(const jsi::Object &, const jsi::PropNameID &name)
      override;
  jsi::Value getProperty(const jsi::Object &, const jsi::String &name) override;
  jsi::Value getProperty(const jsi::Object &, const jsi::Value &name) override;
  bool hasProperty(const jsi::Object &, const jsi::PropNameID &name) override;
  bool hasProperty(const jsi::Object &, const jsi::String &name) override;
  bool hasProperty(const jsi::Object &, const jsi::Value &name) override;
  void setPropertyValue(
      const jsi::Object &,
      const jsi::PropNameID &name,
      const jsi::Value &value) override;
  void setPropertyValue(
      const jsi::Object &,
      const jsi::String &name,
      const jsi::Value &value) override;
  void setPropertyValue(
      const jsi::Object &,
      const jsi::Value &name,
      const jsi::Value &value) override;
  void deleteProperty(const jsi::Object &, const jsi::PropNameID &name)
      override;
  void deleteProperty(const jsi::Object &, const jsi::String &name) override;
  void deleteProperty(const jsi::Object &, const jsi::Value &name) override;

  bool isArray(const jsi::Object &) const override;
  bool isArrayBuffer(const jsi::Object &) const override;
  bool isTypedArray(const jsi::Object &) const override;
  bool isFunction(const jsi::Object &) const override;
  bool isHostObject(const jsi::Object &) const override;
  bool isHostFunction(const jsi::Function &) const override;
  jsi::Array getPropertyNames(const jsi::Object &) override;

  void setPrototypeOf(const jsi::Object &object, const jsi::Value &prototype)
      override;
  jsi::Value getPrototypeOf(const jsi::Object &object) override;

  jsi::WeakObject createWeakObject(const jsi::Object &) override;
  jsi::Value lockWeakObject(const jsi::WeakObject &) override;

  jsi::Array createArray(size_t length) override;
  jsi::ArrayBuffer createArrayBuffer(
      std::shared_ptr<jsi::MutableBuffer> buffer) override;
  size_t size(const jsi::Array &) override;
  size_t size(const jsi::ArrayBuffer &) override;
  uint8_t *data(const jsi::ArrayBuffer &) override;
  bool detached(const jsi::ArrayBuffer &) override;
  jsi::Value getValueAtIndex(const jsi::Array &, size_t i) override;
  void setValueAtIndexImpl(
      const jsi::Array &,
      size_t i,
      const jsi::Value &value) override;
  size_t push(const jsi::Array &arr, const jsi::Value *elements, size_t count)
      override;

  jsi::Function createFunctionFromHostFunction(
      const jsi::PropNameID &name,
      unsigned int paramCount,
      jsi::HostFunctionType func) override;
  jsi::Value call(
      const jsi::Function &,
      const jsi::Value &jsThis,
      const jsi::Value *args,
      size_t count) override;
  jsi::Value callAsConstructor(
      const jsi::Function &,
      const jsi::Value *args,
      size_t count) override;

  void setRuntimeDataImpl(
      const jsi::UUID &uuid,
      const void *data,
      void (*deleter)(const void *data)) override;
  const void *getRuntimeDataImpl(const jsi::UUID &uuid) override;

  std::shared_ptr<jsi::MutableBuffer> tryGetMutableBuffer(
      const jsi::ArrayBuffer &arrayBuffer) override;

  jsi::Uint8Array createUint8Array(size_t length) override;
  jsi::Uint8Array createUint8Array(
      const jsi::ArrayBuffer &buffer,
      size_t offset,
      size_t length) override;

  jsi::ArrayBuffer buffer(const jsi::TypedArray &typedArray) override;
  size_t byteOffset(const jsi::TypedArray &typedArray) override;
  size_t byteLength(const jsi::TypedArray &typedArray) override;
  size_t length(const jsi::TypedArray &typedArray) override;

  bool isUint8Array(const jsi::Object &obj) const override;

  jsi::Value createError(const jsi::String &msg) override;
  jsi::Value createEvalError(const jsi::String &msg) override;
  jsi::Value createRangeError(const jsi::String &msg) override;
  jsi::Value createReferenceError(const jsi::String &msg) override;
  jsi::Value createSyntaxError(const jsi::String &msg) override;
  jsi::Value createTypeError(const jsi::String &msg) override;
  jsi::Value createURIError(const jsi::String &msg) override;

  bool strictEquals(const jsi::Symbol &a, const jsi::Symbol &b) const override;
  bool strictEquals(const jsi::BigInt &a, const jsi::BigInt &b) const override;
  bool strictEquals(const jsi::String &a, const jsi::String &b) const override;
  bool strictEquals(const jsi::Object &a, const jsi::Object &b) const override;

  bool instanceOf(const jsi::Object &o, const jsi::Function &ctor) override;

  ScopeState *pushScope() override;
  void popScope(ScopeState *prv) override;

  void checkStatus(vm::ExecutionStatus);
  vm::HermesValue stringHVFromAscii(const char *ascii, size_t length);
  vm::HermesValue stringHVFromUtf8(const uint8_t *utf8, size_t length);
  vm::HermesValue stringHVFromUtf16(const char16_t *utf16, size_t length);
  std::string utf8FromStringView(vm::StringView view);

  /// Extract a UTF-8 message from the given exception \p ex as UTF-16. Uses
  /// \p buf as storage for the resulting UTF-16 message.
  vm::UTF16Ref utf16FromErrorWhat(
      const std::exception &ex,
      llvh::SmallVectorImpl<llvh::UTF16> &buf);

  struct JsiProxy final : public vm::HostObjectProxy {
    HermesRuntimeImpl &rt_;
    std::shared_ptr<jsi::HostObject> ho_;

    JsiProxy(HermesRuntimeImpl &rt, std::shared_ptr<jsi::HostObject> ho)
        : rt_(rt), ho_(ho) {}

    ~JsiProxy() {
      rt_.finalizerExecutor_.add(
          [ho = std::move(ho_)]() mutable { ho.reset(); });
    }

    vm::CallResult<vm::HermesValue> get(vm::SymbolID id) override {
      jsi::PropNameID sym =
          rt_.add<jsi::PropNameID>(vm::HermesValue::encodeSymbolValue(id));
      jsi::Value ret;
      try {
        ret = ho_->get(rt_, sym);
      }
#ifdef HERMESVM_EXCEPTION_ON_OOM
      catch (const vm::JSOutOfMemoryError &) {
        throw;
      }
#endif
      catch (const jsi::JSError &error) {
        return rt_.runtime_.setThrownValue(hvFromValue(error.value()));
      } catch (const std::exception &ex) {
        llvh::SmallVector<llvh::UTF16, 16> buf;
        return rt_.runtime_.raiseError(
            vm::TwineChar16{"Exception in HostObject::get for prop '"} +
            rt_.runtime_.getIdentifierTable().getStringViewForDev(
                rt_.runtime_, id) +
            "': " + rt_.utf16FromErrorWhat(ex, buf));
      } catch (...) {
        return rt_.runtime_.raiseError(
            vm::TwineChar16{"Exception in HostObject::get: for prop '"} +
            rt_.runtime_.getIdentifierTable().getStringViewForDev(
                rt_.runtime_, id) +
            "': <unknown exception>");
      }

      return hvFromValue(ret);
    }

    vm::CallResult<bool> set(vm::SymbolID id, vm::HermesValue value) override {
      jsi::PropNameID sym =
          rt_.add<jsi::PropNameID>(vm::HermesValue::encodeSymbolValue(id));
      try {
        ho_->set(rt_, sym, rt_.valueFromHermesValue(value));
      }
#ifdef HERMESVM_EXCEPTION_ON_OOM
      catch (const vm::JSOutOfMemoryError &) {
        throw;
      }
#endif
      catch (const jsi::JSError &error) {
        return rt_.runtime_.setThrownValue(hvFromValue(error.value()));
      } catch (const std::exception &ex) {
        llvh::SmallVector<llvh::UTF16, 16> buf;
        return rt_.runtime_.raiseError(
            vm::TwineChar16{"Exception in HostObject::set for prop '"} +
            rt_.runtime_.getIdentifierTable().getStringViewForDev(
                rt_.runtime_, id) +
            "': " + rt_.utf16FromErrorWhat(ex, buf));
      } catch (...) {
        return rt_.runtime_.raiseError(
            vm::TwineChar16{"Exception in HostObject::set: for prop '"} +
            rt_.runtime_.getIdentifierTable().getStringViewForDev(
                rt_.runtime_, id) +
            "': <unknown exception>");
      }
      return true;
    }

    vm::ExecutionStatus getHostPropertyNames(
        vm::MutableHandle<vm::JSArray> result) override {
      struct : public vm::Locals {
        vm::PinnedValue<vm::SymbolID> tmpHandle;
      } lv;
      vm::LocalsRAII lraii(rt_.runtime_, &lv);

      try {
        auto names = ho_->getPropertyNames(rt_);

        auto arrayRes =
            vm::JSArray::create(rt_.runtime_, names.size(), names.size());
        if (arrayRes == vm::ExecutionStatus::EXCEPTION) {
          return vm::ExecutionStatus::EXCEPTION;
        }
        result.castAndSetHermesValue<vm::JSArray>(arrayRes->getHermesValue());

        vm::GCScope gcScope{rt_.runtime_};
        size_t i = 0;
        for (auto &name : names) {
          lv.tmpHandle = phv(name).getSymbol();
          if (LLVM_UNLIKELY(
                  vm::JSArray::setElementAt(
                      result, rt_.runtime_, i++, lv.tmpHandle) ==
                  vm::ExecutionStatus::EXCEPTION))
            return vm::ExecutionStatus::EXCEPTION;
        }

        return vm::ExecutionStatus::RETURNED;
      }
#ifdef HERMESVM_EXCEPTION_ON_OOM
      catch (const vm::JSOutOfMemoryError &) {
        throw;
      }
#endif
      catch (const jsi::JSError &error) {
        return rt_.runtime_.setThrownValue(hvFromValue(error.value()));
      } catch (const std::exception &ex) {
        llvh::SmallVector<llvh::UTF16, 16> buf;
        return rt_.runtime_.raiseError(
            vm::TwineChar16{"Exception in HostObject::getPropertyNames: "} +
            rt_.utf16FromErrorWhat(ex, buf));
      } catch (...) {
        return rt_.runtime_.raiseError(
            vm::TwineChar16{
                "Exception in HostObject::getPropertyNames: <unknown>"});
      }
    };
  };

  struct HFContext final {
    HFContext(jsi::HostFunctionType hf, HermesRuntimeImpl &hri)
        : hostFunction(std::move(hf)), hermesRuntimeImpl(hri) {}

    static vm::CallResult<vm::HermesValue> func(
        void *context,
        vm::Runtime &runtime) {
      vm::NativeArgs hvArgs = runtime.getCurrentFrame().getNativeArgs();
      HFContext *hfc = reinterpret_cast<HFContext *>(context);
      HermesRuntimeImpl &rt = hfc->hermesRuntimeImpl;
      assert(&runtime == &rt.runtime_);

      llvh::SmallVector<jsi::Value, 8> apiArgs;
      for (vm::HermesValue hv : hvArgs) {
        apiArgs.push_back(rt.valueFromHermesValue(hv));
      }

      jsi::Value ret;
      const jsi::Value *args = apiArgs.empty() ? nullptr : &apiArgs.front();

      try {
        ret = (hfc->hostFunction)(
            rt,
            rt.valueFromHermesValue(hvArgs.getThisArg()),
            args,
            apiArgs.size());
      }
#ifdef HERMESVM_EXCEPTION_ON_OOM
      catch (const vm::JSOutOfMemoryError &) {
        throw;
      }
#endif
      catch (const jsi::JSError &error) {
        return runtime.setThrownValue(hvFromValue(error.value()));
      } catch (const std::exception &ex) {
        llvh::SmallVector<llvh::UTF16, 16> buf;
        return runtime.raiseError(
            vm::TwineChar16{"Exception in HostFunction: "} +
            rt.utf16FromErrorWhat(ex, buf));
      } catch (...) {
        return runtime.raiseError("Exception in HostFunction: <unknown>");
      }

      return hvFromValue(ret);
    }

    static void finalize(void *context) {
      auto *hfc = reinterpret_cast<HFContext *>(context);
      hfc->hermesRuntimeImpl.finalizerExecutor_.add([hfc]() { delete hfc; });
    }

    jsi::HostFunctionType hostFunction;
    HermesRuntimeImpl &hermesRuntimeImpl;
  };

  /// Holds the jsi::NativeState shared pointer and the HermesRuntimeImpl.
  /// This is passed in as the NativeState context to store the NativeState
  /// object. The static finalize method will be passed in as function pointer
  /// to the VM.
  struct NativeStateContext {
    std::shared_ptr<jsi::NativeState> state;
    HermesRuntimeImpl &runtime;

    /// Called when the VM tries to
    /// finalize the NativeState object.
    static void finalize(vm::GC &, vm::NativeState *ns) {
      auto *context = reinterpret_cast<NativeStateContext *>(ns->context());
      context->runtime.finalizerExecutor_.add([context]() { delete context; });
    }
  };

  // A ManagedChunkedList element that indicates whether it's occupied based on
  // a refcount.
  template <typename T>
  struct ManagedValue : PointerValue {
    ManagedValue() : refCount_(0) {}

    // Determine whether the element is occupied by inspecting the refcount.
    bool isFree() const {
      // In general, it is safe to use relaxed operations here because there
      // will be a control dependency between this load and any store that needs
      // to be ordered. However, TSAN does not analyse control dependencies,
      // since they are technically not part of C++, so use acquire under TSAN
      // to enforce the ordering of subsequent writes resulting from deleting a
      // freed element. (see the comment on dec() for more information)

#if LLVM_THREAD_SANITIZER_BUILD
      return refCount_.load(std::memory_order_acquire) == 0;
#else
      return refCount_.load(std::memory_order_relaxed) == 0;
#endif
    }

    // Store a value and start the refcount at 1. After invocation, this
    // instance is occupied with a value, and the "nextFree" methods should
    // not be used until the value is released.
    template <typename... Args>
    void emplace(Args &&...args) {
      assert(isFree() && "Emplacing already occupied value");
      refCount_.store(1, std::memory_order_relaxed);
      new (&value_) T(std::forward<Args>(args)...);
    }

    // Get the next free element. Must not be called when this instance is
    // occupied with a value.
    ManagedValue<T> *getNextFree() {
      assert(isFree() && "Free pointer unusuable while occupied");
      return nextFree_;
    }

    // Set the next free element. Must not be called when this instance is
    // occupied with a value.
    void setNextFree(ManagedValue<T> *nextFree) {
      assert(isFree() && "Free pointer unusuable while occupied");
      nextFree_ = nextFree;
    }

    T &value() {
      assert(!isFree() && "Value not present");
      return value_;
    }

    /// Returning the value without asserting if it's free. This is only used
    /// when visiting the weak values by the WeakAcceptor of GC.
    T &valueUnsafe() {
      return value_;
    }

    const T &value() const {
      assert(!isFree() && "Value not present");
      return value_;
    }

    void invalidate() noexcept override {
#ifdef ASSERT_ON_DANGLING_VM_REFS
      assert(
          ((1u << 31) & refCount_) == 0 &&
          "This PointerValue was left dangling after the Runtime was destroyed.");
#endif
      dec();
    }

    void inc() noexcept {
      // It is always safe to use relaxed operations for incrementing the
      // reference count, because the only operation that may occur concurrently
      // with it is decrementing the reference count, and we do not need to
      // enforce any ordering between the two.
      auto oldCount = refCount_.fetch_add(1, std::memory_order_relaxed);
      assert(oldCount && "Cannot resurrect a pointer");
      assert(oldCount + 1 != 0 && "Ref count overflow");
      (void)oldCount;
    }

    void dec() noexcept {
      // It is safe to use relaxed operations here because decrementing the
      // reference count is the only access that may be performed without proper
      // synchronisation. As a result, the only ordering we need to enforce when
      // decrementing is that the vtable pointer used to call \c invalidate is
      // loaded from before the decrement, in case the decrement ends up causing
      // this value to be freed. We get this ordering from the fact that the
      // vtable read and the reference count update form a load-store control
      // dependency, which preserves their ordering on any reasonable hardware.
      // TSAN does not analyse control dependencies, so we use release under
      // TSAN to enforce the ordering of the preceding vtable load.

#if LLVM_THREAD_SANITIZER_BUILD
      auto oldCount = refCount_.fetch_sub(1, std::memory_order_release);
#else
      auto oldCount = refCount_.fetch_sub(1, std::memory_order_relaxed);
#endif
      assert(oldCount > 0 && "Ref count underflow");
      (void)oldCount;
    }

#ifdef ASSERT_ON_DANGLING_VM_REFS
    void markDangling() {
      // Mark this PointerValue as dangling by setting the top bit AND the
      // second-top bit. The top bit is used to determine if the pointer is
      // dangling. Setting the second-top bit ensures that accidental
      // over-calling the dec() function doesn't clear the top bit without
      // complicating the implementation of dec().
      refCount_ |= 0b11u << 30;
    }
#endif

   private:
    std::atomic<uint32_t> refCount_;
    union {
      T value_;
      ManagedValue<T> *nextFree_;
    };
  };

  template <typename T>
  class ManagedValues : public ::hermes::ManagedChunkedList<ManagedValue<T>> {
    static constexpr double kSizingWeight_ = 0.5;

   public:
    explicit ManagedValues(double occupancyRatio)
        : ::hermes::ManagedChunkedList<ManagedValue<T>>(
              occupancyRatio,
              kSizingWeight_) {}

#ifdef ASSERT_ON_DANGLING_VM_REFS
    // If we have active HermesValuePointers when deconstructing, these will
    // now be dangling. We deliberately allocate and immediately leak heap
    // memory to hold the internal list. This keeps alive memory holding the
    // ref-count of the now dangling references, allowing them to detect the
    // dangling case safely and assert when they are eventually released. By
    // deferring the assert it's a bit easier to see what's holding the pointers
    // for too long.
    ~ManagedValues() {
      ::hermes::ManagedChunkedList<ManagedValue<T>>::collect();
      bool empty = true;
      ::hermes::ManagedChunkedList<ManagedValue<T>>::forEach(
          [&empty](ManagedValue<T> &element) {
            element.markDangling();
            empty = false;
          });
      if (!empty) {
        // This is the deliberate memory leak described above.
        new ::hermes::ManagedChunkedList(std::move(*this));
      }
    }
#endif
  };

  using HermesPointerValue = ManagedValue<vm::PinnedHermesValue>;
  using WeakRefPointerValue = ManagedValue<vm::WeakRoot<vm::JSObject>>;

  HermesPointerValue *clone(const Runtime::PointerValue *pv) {
    if (!pv) {
      return nullptr;
    }
    // These are only ever allocated by us, so we can remove their constness
    auto result = static_cast<HermesPointerValue *>(
        const_cast<Runtime::PointerValue *>(pv));
    result->inc();
    return result;
  }

  /// Helper function that is parameterized over the type of context being
  /// created.
  template <typename ContextType>
  jsi::Function createFunctionFromHostFunction(
      ContextType *context,
      const jsi::PropNameID &name,
      unsigned int paramCount);

  /// Throw the exception stored in the Runtime as a jsi::JSError.
  LLVM_ATTRIBUTE_NORETURN void throwPendingError();

  /// Throw a jsi::JSError with a message created by concatenating the string
  /// representations of \p args.
  template <typename... Args>
  LLVM_ATTRIBUTE_NORETURN void throwJSErrorWithMessage(Args &&...args);

  /// Concrete declarations of HermesRuntime methods.
  ICast *getHermesRootAPI() override;
  void sampledTraceToStreamInDevToolsFormat(std::ostream &stream) override;
  void resetTimezoneCache() override;
  sampling_profiler::Profile dumpSampledTraceToProfile() override;
  void loadSegment(
      std::unique_ptr<const jsi::Buffer> buffer,
      const jsi::Value &context) override;
  uint64_t getUniqueID(const jsi::Object &o) const override;
  uint64_t getUniqueID(const jsi::BigInt &s) const override;
  uint64_t getUniqueID(const jsi::String &s) const override;
  uint64_t getUniqueID(const jsi::PropNameID &pni) const override;
  uint64_t getUniqueID(const jsi::Symbol &sym) const override;
  uint64_t getUniqueID(const jsi::Value &val) const override;
  jsi::Value getObjectForID(uint64_t id) override;
  const ::hermes::vm::GCExecTrace &getGCExecTrace() const override;
  std::string getIOTrackingInfoJSON() override;
  debugger::Debugger &getDebugger() override;
  void debugJavaScript(
      const std::string &src,
      const std::string &sourceURL,
      const DebugFlags &debugFlags) override;
  void registerForProfiling() override;
  void unregisterForProfiling() override;
  void asyncTriggerTimeout() override;
  void watchTimeLimit(uint32_t timeoutInMs) override;
  void unwatchTimeLimit() override;
  jsi::Value evaluateJavaScriptWithSourceMap(
      const std::shared_ptr<const jsi::Buffer> &buffer,
      const std::shared_ptr<const jsi::Buffer> &sourceMapBuf,
      const std::string &sourceURL) override;
  SHUnitCreator getSHUnitCreator() const override;
  jsi::Value evaluateSHUnit(SHUnitCreator shUnitCreator) override;
  SHRuntime *getSHRuntime() noexcept override;
  void *getVMRuntimeUnsafe() const override;
  size_t rootsListLengthForTests() const override;

  ManagedValues<vm::PinnedHermesValue> hermesValues_;
  ManagedValues<vm::WeakRoot<vm::JSObject>> weakHermesValues_;
  std::shared_ptr<::hermes::vm::Runtime> rt_;
  ::hermes::vm::Runtime &runtime_;
  friend class debugger::Debugger;
  std::unique_ptr<debugger::Debugger> debugger_;
  ::hermes::vm::experiments::VMExperimentFlags vmExperimentFlags_{0};
  friend struct ::HermesTestHelper;

  /// Compilation flags used by prepareJavaScript().
  ::hermes::hbc::CompileFlags compileFlags_{};
  /// Custom data stored by the user of the runtime.
  llvh::DenseMap<
      jsi::UUID,
      std::pair<const void *, void (*)(const void *data)>,
      UUIDInfo>
      dataMap_;

  /// Executor used to finalize NativeState, HostFunction, and HostObjects on
  /// a background thread so that GC collection is not blocked by potentially
  /// expensive clean-up work. The destructor explicitly calls rt_.reset()
  /// before this member is destroyed to ensure all clean-up tasks are queued
  /// before the executor drains and joins.
  ::hermes::SerialExecutor finalizerExecutor_;

  /// Provided by the integrator for the Runtime to schedule a task. This is
  /// called whenever the Hermes Runtime wants to run a task, but should not
  /// determine when it should be run. This is particularly useful for the
  /// Worker implementation, where the Worker will queue a task for the main
  /// thread to check a posted message.
  IEventLoopControl *eventLoopControl_{nullptr};

  /// Observe a new YG collection or schedule idle GC after the entry budget.
  LLVM_ATTRIBUTE_NOINLINE void maybeScheduleIdleGC();

  /// Tracking status when the current execution enters/exits the mutator from
  /// JSI.
  struct MutatorScope {
    HermesRuntimeImpl &jsiRuntime;
    /// Tracks how many times we have (potentially) entered mutator. This may be
    /// useful in heuristics for running certain tasks when entering mutator
    /// in the future.
    /// This is allowed to overflow (which will wrap around) since we don't need
    /// the exact accumulated counter.
    uint64_t counter{0};
    /// Counter at the last GC observation or scheduling check.
    uint64_t lastGCActivityCounter{0};
    /// Number of collections last observed on the mutator.
    unsigned lastObservedGCCount{0};
  };

  /// RAII for managing MutatorScope when entering/exiting the mutator from JSI.
  class ExecutionScopeRAII {
   private:
    MutatorScope &scope_;

   public:
    explicit ExecutionScopeRAII(MutatorScope &scope) : scope_(scope) {
      scope_.counter++;
      if ((scope_.jsiRuntime.runtime_.getVMExperimentFlags() &
           ::hermes::vm::experiments::IdleTimeGC) &&
          scope_.jsiRuntime.eventLoopControl_) {
        scope_.jsiRuntime.maybeScheduleIdleGC();
      }
    }

    ~ExecutionScopeRAII() = default;

    // Non-copyable, non-movable.
    ExecutionScopeRAII(const ExecutionScopeRAII &) = delete;
    ExecutionScopeRAII(ExecutionScopeRAII &&) = delete;
    ExecutionScopeRAII &operator=(const ExecutionScopeRAII &) = delete;
    ExecutionScopeRAII &operator=(ExecutionScopeRAII &&) = delete;
  };

  /// ExecutionScope for this Runtime.
  MutatorScope mutatorScope;
};

void HermesRuntimeImpl::maybeScheduleIdleGC() {
  const auto numGCs = runtime_.getHeap().getNumYGGCs();
  const bool gcOccurred = mutatorScope.lastObservedGCCount != numGCs;
  if (LLVM_LIKELY(
          !gcOccurred &&
          mutatorScope.counter - mutatorScope.lastGCActivityCounter < 10000)) {
    return;
  }

  mutatorScope.lastObservedGCCount = numGCs;
  // A controller may drop tasks, so scheduling checks also restart the budget.
  mutatorScope.lastGCActivityCounter = mutatorScope.counter;
  if (gcOccurred) {
    return;
  }

  eventLoopControl_->scheduleTask([&runtime = runtime_,
                                   &scope = mutatorScope,
                                   numGCs]() {
    auto &gc = runtime.getHeap();
    if (gc.getNumYGGCs() != numGCs) {
      return;
    }
    gc.collectWithOptions(
        "EnterMutator",
        vm::GCBase::GCOptions{
            vm::GCBase::GCType::Minor, vm::GCBase::GCExecutionPolicy::Sync});
    scope.lastObservedGCCount = gc.getNumYGGCs();
    scope.lastGCActivityCounter = scope.counter;
  });
}
} // namespace

jsi::ICast *HermesRootAPI::castInterface(const jsi::UUID &interfaceUUID) {
  if (interfaceUUID == IHermesRootAPI::uuid) {
    return static_cast<IHermesRootAPI *>(this);
  }
  if (interfaceUUID == ISetFatalHandler::uuid) {
    return static_cast<ISetFatalHandler *>(this);
  }
  return nullptr;
}

#if HERMES_ENABLE_CORE_EXTENSIONS
/// Load and install JSI-based extensions (TextEncoder, etc.).
/// Uses internal VM APIs to set hidesEpilogue=true so the extensions bytecode
/// epilogue is not visible to users via runtime.getEpilogues().
static void loadAndInstallExtensions(HermesRuntimeImpl &runtime) {
  llvh::ArrayRef<uint8_t> bytecode = getExtensionsBytecode();
  auto bcResult = hbc::BCProviderFromBuffer::createBCProviderFromBuffer(
      std::make_unique<::hermes::Buffer>(bytecode.data(), bytecode.size()));
  if (LLVM_UNLIKELY(!bcResult.first)) {
    ::hermes::hermes_fatal(
        ("Error loading extensions bytecode: " + bcResult.second).c_str());
  }
  vm::RuntimeModuleFlags flags;
  flags.persistent = true;
  flags.hidesEpilogue = true;
  flags.funcsAreBuiltins = true;
  auto res = runtime.runtime_.runBytecode(
      std::move(bcResult.first),
      flags,
      /*sourceURL*/ "Extensions.js",
      vm::Runtime::makeNullHandle<vm::Environment>());
  if (LLVM_UNLIKELY(res == vm::ExecutionStatus::EXCEPTION)) {
    ::hermes::hermes_fatal("Extensions bytecode threw exception");
  }
  jsi::Object extensions = runtime.valueFromHermesValue(*res).asObject(runtime);
  installExtensions(runtime, std::move(extensions));
}
#endif // HERMES_ENABLE_CORE_EXTENSIONS

std::unique_ptr<HermesRuntime> HermesRootAPI::makeHermesRuntime(
    const vm::RuntimeConfig &runtimeConfig) {
#ifdef HERMESVM_PLATFORM_LOGGING
  auto ret = std::make_unique<HermesRuntimeImpl>(
      runtimeConfig.rebuild()
          .withGCConfig(runtimeConfig.getGCConfig()
                            .rebuild()
                            .withShouldRecordStats(true)
                            .build())
          .build());
#else
  auto ret = std::make_unique<HermesRuntimeImpl>(runtimeConfig);
#endif

#ifdef HERMES_ENABLE_DEBUGGER
  // Only HermesRuntime can create a debugger instance.  This requires
  // the setter and not using make_unique, so the call to new is here
  // in this function, which is a friend of debugger::Debugger.
  ret->setDebugger(
      std::unique_ptr<debugger::Debugger>(
          new debugger::Debugger(ret.get(), ret->runtime_)));
#else
  ret->setDebugger(std::make_unique<debugger::Debugger>());
#endif

#if HERMES_ENABLE_CORE_EXTENSIONS
  loadAndInstallExtensions(*ret);
#endif

  return ret;
}

bool HermesRootAPI::isHermesBytecode(const uint8_t *data, size_t len) {
  return hbc::BCProviderFromBuffer::isBytecodeStream(
      llvh::ArrayRef<uint8_t>(data, len));
}

uint32_t HermesRootAPI::getBytecodeVersion() {
  return hbc::BYTECODE_VERSION;
}

void HermesRootAPI::prefetchHermesBytecode(const uint8_t *data, size_t len) {
  hbc::BCProviderFromBuffer::prefetch(llvh::ArrayRef<uint8_t>(data, len));
}

bool HermesRootAPI::hermesBytecodeSanityCheck(
    const uint8_t *data,
    size_t len,
    std::string *errorMessage) {
  return hbc::BCProviderFromBuffer::bytecodeStreamSanityCheck(
      llvh::ArrayRef<uint8_t>(data, len), errorMessage);
}

std::pair<const uint8_t *, size_t> HermesRootAPI::getBytecodeEpilogue(
    const uint8_t *data,
    size_t len) {
  auto epi = hbc::BCProviderFromBuffer::getEpilogueFromBytecode(
      llvh::ArrayRef<uint8_t>(data, len));
  return std::make_pair(epi.data(), epi.size());
}

void HermesRootAPI::setFatalHandler(void (*handler)(const std::string &)) {
  detail::sApiFatalHandler = handler;
}

void HermesRootAPI::enableSamplingProfiler(double meanHzFreq) {
#if HERMESVM_SAMPLING_PROFILER_AVAILABLE
  ::hermes::vm::SamplingProfiler::enable(meanHzFreq);
#else
  throwHermesNotCompiledWithSamplingProfilerSupport();
#endif // HERMESVM_SAMPLING_PROFILER_AVAILABLE
}

void HermesRootAPI::disableSamplingProfiler() {
#if HERMESVM_SAMPLING_PROFILER_AVAILABLE
  ::hermes::vm::SamplingProfiler::disable();
#else
  throwHermesNotCompiledWithSamplingProfilerSupport();
#endif // HERMESVM_SAMPLING_PROFILER_AVAILABLE
}

void HermesRootAPI::dumpSampledTraceToFile(const std::string &fileName) {
#if HERMESVM_SAMPLING_PROFILER_AVAILABLE
  std::error_code ec;
  llvh::raw_fd_ostream os(fileName.c_str(), ec, llvh::sys::fs::F_Text);
  if (ec) {
    throw std::system_error(ec);
  }
  ::hermes::vm::SamplingProfiler::dumpTraceryTraceGlobal(os);
#else
  throw std::logic_error(
      "Hermes was not compiled with SamplingProfilerSupport");
#endif // HERMESVM_SAMPLING_PROFILER_AVAILABLE
}

void HermesRootAPI::dumpSampledTraceToStream(std::ostream &stream) {
#if HERMESVM_SAMPLING_PROFILER_AVAILABLE
  llvh::raw_os_ostream os(stream);
  ::hermes::vm::SamplingProfiler::dumpTraceryTraceGlobal(os);
#else
  throwHermesNotCompiledWithSamplingProfilerSupport();
#endif // HERMESVM_SAMPLING_PROFILER_AVAILABLE
}

std::unordered_map<std::string, std::vector<std::string>>
HermesRootAPI::getExecutedFunctions() {
  std::unordered_map<
      std::string,
      std::vector<::hermes::vm::CodeCoverageProfiler::FuncInfo>>
      executedFunctionsByVM =
          ::hermes::vm::CodeCoverageProfiler::getExecutedFunctions();
  std::unordered_map<std::string, std::vector<std::string>> result;

  for (auto const &x : executedFunctionsByVM) {
    std::vector<std::string> res;
    std::transform(
        x.second.begin(),
        x.second.end(),
        std::back_inserter(res),
        [](const ::hermes::vm::CodeCoverageProfiler::FuncInfo &entry) {
          std::stringstream ss;
          ss << entry.moduleId;
          ss << ":";
          ss << entry.funcVirtualOffset;
          ss << ":";
          ss << entry.debugInfo;
          return ss.str();
        });
    result.emplace(x.first, res);
  }
  return result;
}

bool HermesRootAPI::isCodeCoverageProfilerEnabled() {
  return ::hermes::vm::CodeCoverageProfiler::globallyEnabled();
}

void HermesRootAPI::enableCodeCoverageProfiler() {
  ::hermes::vm::CodeCoverageProfiler::enableGlobal();
}

void HermesRootAPI::disableCodeCoverageProfiler() {
  ::hermes::vm::CodeCoverageProfiler::disableGlobal();
}

namespace {

/// A class which adapts a jsi buffer to a Hermes buffer.
/// It also provides the ability to create a partial "view" into the buffer.
class BufferAdapter final : public ::hermes::Buffer {
 public:
  explicit BufferAdapter(
      const std::shared_ptr<const jsi::Buffer> &buf,
      const uint8_t *data,
      size_t size)
      : buf_(buf) {
    data_ = data;
    size_ = size;
  }

  explicit BufferAdapter(const std::shared_ptr<const jsi::Buffer> &buf)
      : BufferAdapter(buf, buf->data(), buf->size()) {}

 private:
  /// The buffer we are "adapting".
  std::shared_ptr<const jsi::Buffer> buf_;
};
} // namespace

jsi::ICast *HermesRuntimeImpl::getHermesRootAPI() {
  return makeHermesRootAPI();
}

jsi::ICast *HermesRuntimeImpl::castInterface(const jsi::UUID &interfaceUUID) {
  if (interfaceUUID == IHermesTestHelpers::uuid) {
    return static_cast<IHermesTestHelpers *>(this);
  } else if (interfaceUUID == IHermes::uuid) {
    return static_cast<IHermes *>(this);
  } else if (interfaceUUID == IHermesSHUnit::uuid) {
    return static_cast<IHermesSHUnit *>(this);
  } else if (interfaceUUID == IStringWriter::uuid) {
    return static_cast<IStringWriter *>(this);
  }
#ifdef JSI_UNSTABLE
  else if (interfaceUUID == ISerialization::uuid) {
    return static_cast<ISerialization *>(this);
  } else if (interfaceUUID == IHermesTracingHelpers::uuid) {
    return static_cast<IHermesTracingHelpers *>(this);
  }
#endif
  else if (interfaceUUID == ISetEventLoopControl::uuid) {
    return static_cast<ISetEventLoopControl *>(this);
  }
  return nullptr;
}

#ifdef JSI_UNSTABLE
std::shared_ptr<jsi::Serialized> HermesRuntimeImpl::serialize(
    const jsi::Value &value) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  vm::PinnedHermesValue numberStorage;
  auto serializedRes =
      vm::serialize_RJS(runtime_, vmHandleFromValue(value, &numberStorage));
  checkStatus(serializedRes.getStatus());
  return std::make_shared<HermesSerialized>(*serializedRes);
}

jsi::Value HermesRuntimeImpl::deserialize(
    const std::shared_ptr<jsi::Serialized> &serialized) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  if (LLVM_UNLIKELY(!serialized)) {
    throw jsi::JSINativeException("serialized cannot be null");
  }

  void *privateSerialized =
      serialized->getPrivate(HermesSerialized::getHermesSerializedSecret());
  if (LLVM_UNLIKELY(!privateSerialized)) {
    throw jsi::JSINativeException(
        "Cannot deserialize non-Hermes serialized objects");
  }

  vm::GCScope gcScope(runtime_);
  auto serializedValue =
      reinterpret_cast<vm::SerializedValue *>(privateSerialized);
  auto deserializedRes = vm::deserialize(runtime_, *serializedValue);
  checkStatus(deserializedRes.getStatus());
  return valueFromHermesValue(*deserializedRes);
}

std::unique_ptr<jsi::Serialized> HermesRuntimeImpl::serializeWithTransfer(
    const jsi::Value &value,
    const jsi::Array &transferList) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  if (LLVM_UNLIKELY(!vm::vmisa<vm::JSArray>(phv(transferList)))) {
    throw jsi::JSINativeException("Transfer array must not be a Proxy.");
  }
  vm::GCScope gcScope(runtime_);
  vm::PinnedHermesValue numberStorage;

  auto arrHandle = vm::Handle<vm::JSArray>::vmcast(&phv(transferList));
  auto serializedRes = vm::serializeWithTransfer_RJS(
      runtime_, vmHandleFromValue(value, &numberStorage), arrHandle);
  checkStatus(serializedRes.getStatus());

  return std::make_unique<HermesSerialized>(*serializedRes);
}

jsi::Array HermesRuntimeImpl::deserializeWithTransfer(
    std::unique_ptr<jsi::Serialized> &serialized) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  if (LLVM_UNLIKELY(!serialized)) {
    throw jsi::JSINativeException("serialized cannot be null");
  }

  void *privateSerialized =
      serialized->getPrivate(HermesSerialized::getHermesSerializedSecret());
  if (LLVM_UNLIKELY(!privateSerialized)) {
    throw jsi::JSINativeException(
        "Cannot deserialize non-Hermes serialized objects");
  }

  vm::GCScope gcScope(runtime_);
  auto *serializedValue =
      reinterpret_cast<vm::SerializedValue *>(privateSerialized);
  auto deserializedRes =
      vm::deserializeWithTransfer(runtime_, *serializedValue);
  serialized.reset();
  checkStatus(deserializedRes.getStatus());

  return add<jsi::Array>(deserializedRes->getHermesValue());
}

const vm::SerializedValue *HermesRuntimeImpl::getHermesSerializedValue(
    const jsi::Serialized &serialized) const {
  const void *secret = HermesSerialized::getHermesSerializedSecret();
  void *privateSerialized =
      const_cast<jsi::Serialized *>(&serialized)->getPrivate(secret);

  const vm::SerializedValue *serializedValue =
      reinterpret_cast<vm::SerializedValue *>(privateSerialized);

  return serializedValue;
}

const std::shared_ptr<jsi::Serialized> HermesRuntimeImpl::makeSerialized(
    ::hermes::vm::SerializedValue &value) const {
  return std::make_unique<HermesSerialized>(value);
}
#endif

void HermesRuntimeImpl::setEventLoopControl(
    IEventLoopControl *eventLoopControl) {
  eventLoopControl_ = eventLoopControl;
}

IEventLoopControl *HermesRuntimeImpl::getEventLoopControl() {
  return eventLoopControl_;
}

sampling_profiler::Profile HermesRuntimeImpl::dumpSampledTraceToProfile() {
#if HERMESVM_SAMPLING_PROFILER_AVAILABLE
  vm::SamplingProfiler *sp = runtime_.samplingProfiler.get();
  if (!sp) {
    throw jsi::JSINativeException("Runtime not registered for profiling");
  }
  return sp->dumpAsProfile();
#else
  throwHermesNotCompiledWithSamplingProfilerSupport();
#endif // HERMESVM_SAMPLING_PROFILER_AVAILABLE
}

void HermesRuntimeImpl::resetTimezoneCache() {
  runtime_.getJSLibStorage()->localTimeOffsetCache.reset();
}

void HermesRuntimeImpl::sampledTraceToStreamInDevToolsFormat(
    std::ostream &stream) {
#if HERMESVM_SAMPLING_PROFILER_AVAILABLE
  vm::SamplingProfiler *sp = runtime_.samplingProfiler.get();
  if (!sp) {
    throw jsi::JSINativeException("Runtime not registered for profiling");
  }
  llvh::raw_os_ostream os(stream);
  sp->dumpChromeTrace(os);
#else
  throwHermesNotCompiledWithSamplingProfilerSupport();
#endif // HERMESVM_SAMPLING_PROFILER_AVAILABLE
}

void HermesRuntimeImpl::loadSegment(
    std::unique_ptr<const jsi::Buffer> buffer,
    const jsi::Value &context) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  auto ret = hbc::BCProviderFromBuffer::createBCProviderFromBuffer(
      std::make_unique<BufferAdapter>(std::move(buffer)));
  if (!ret.first) {
    LOG_EXCEPTION_CAUSE("Error evaluating javascript: %s", ret.second.c_str());
    throw jsi::JSINativeException("Error evaluating javascript: " + ret.second);
  }

  vm::PinnedHermesValue numberStorage;
  auto requireContext = vm::Handle<vm::RequireContext>::dyn_vmcast(
      vmHandleFromValue(context, &numberStorage));
  if (!requireContext) {
    LOG_EXCEPTION_CAUSE("Error loading segment: Invalid context");
    throw jsi::JSINativeException("Error loading segment: Invalid context");
  }

  vm::RuntimeModuleFlags flags;
  flags.persistent = true;
  checkStatus(
      runtime_.loadSegment(std::move(ret.first), requireContext, flags));
}

uint64_t HermesRuntimeImpl::getUniqueID(const jsi::Object &o) const {
  return runtime_.getHeap().getObjectID(
      static_cast<vm::GCCell *>(phv(o).getObject()));
}
uint64_t HermesRuntimeImpl::getUniqueID(const jsi::BigInt &s) const {
  return runtime_.getHeap().getObjectID(
      static_cast<vm::GCCell *>(phv(s).getBigInt()));
}
uint64_t HermesRuntimeImpl::getUniqueID(const jsi::String &s) const {
  return runtime_.getHeap().getObjectID(
      static_cast<vm::GCCell *>(phv(s).getString()));
}

// TODO(T111638575): PropNameID and Symbol can have the same unique ID. We
// should either add a way to distinguish them, or explicitly state that the
// unique ID may not be used to distinguish a PropNameID from a Value.
uint64_t HermesRuntimeImpl::getUniqueID(const jsi::PropNameID &pni) const {
  return runtime_.getHeap().getObjectID(phv(pni).getSymbol());
}
uint64_t HermesRuntimeImpl::getUniqueID(const jsi::Symbol &sym) const {
  return runtime_.getHeap().getObjectID(phv(sym).getSymbol());
}

uint64_t HermesRuntimeImpl::getUniqueID(const jsi::Value &val) const {
  vm::HermesValue hv = HermesRuntimeImpl::hvFromValue(val);
  // 0 is reserved as a non-ID.
  return runtime_.getHeap().getSnapshotID(hv).getValueOr(0);
}

jsi::Value HermesRuntimeImpl::getObjectForID(uint64_t id) {
  vm::GCCell *ptr =
      static_cast<vm::GCCell *>(runtime_.getHeap().getObjectForID(id));
  if (ptr && vm::vmisa<vm::JSObject>(ptr)) {
    return add<jsi::Object>(vm::HermesValue::encodeObjectValue(ptr));
  }
  // If the ID doesn't map to a pointer, or that pointer isn't an object,
  // return null.
  // This is because a jsi::Object can't be used to represent something internal
  // to the VM like a HiddenClass.
  return jsi::Value::null();
}

const ::hermes::vm::GCExecTrace &HermesRuntimeImpl::getGCExecTrace() const {
  return static_cast<const HermesRuntimeImpl *>(this)
      ->runtime_.getGCExecTrace();
}

std::string HermesRuntimeImpl::getIOTrackingInfoJSON() {
  std::string buf;
  llvh::raw_string_ostream strstrm(buf);
  runtime_.getIOTrackingInfoJSON(strstrm);
  strstrm.flush();
  return buf;
}

debugger::Debugger &HermesRuntimeImpl::getDebugger() {
  return *(debugger_);
}

void HermesRuntimeImpl::debugJavaScript(
    const std::string &src,
    const std::string &sourceURL,
    const DebugFlags &debugFlags) {
#ifdef HERMES_ENABLE_DEBUGGER
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::Runtime &runtime = runtime_;
  vm::GCScope gcScope(runtime);
  vm::ExecutionStatus res =
      runtime.run(src, sourceURL, compileFlags_).getStatus();
  checkStatus(res);
#endif
}

void HermesRuntimeImpl::registerForProfiling() {
  vm::NoMutatorScope noMutatorScope{runtime_};
#if HERMESVM_SAMPLING_PROFILER_AVAILABLE
  vm::Runtime &runtime = runtime_;
  if (runtime.samplingProfiler) {
    runtime.samplingProfiler->setRuntimeThread();
  } else {
    runtime.samplingProfiler = ::hermes::vm::SamplingProfiler::create(runtime);
  }
#else
  throwHermesNotCompiledWithSamplingProfilerSupport();
#endif // HERMESVM_SAMPLING_PROFILER_AVAILABLE
}

void HermesRuntimeImpl::unregisterForProfiling() {
  vm::NoMutatorScope noMutatorScope{runtime_};
#if HERMESVM_SAMPLING_PROFILER_AVAILABLE
  if (!runtime_.samplingProfiler) {
    ::hermes::hermes_fatal(
        "unregistering HermesVM not registered for profiling is not allowed");
  }
  runtime_.samplingProfiler.reset();
#else
  throwHermesNotCompiledWithSamplingProfilerSupport();
#endif // HERMESVM_SAMPLING_PROFILER_AVAILABLE
}

void HermesRuntimeImpl::asyncTriggerTimeout() {
  // No NoMutatorScope here: this method can be called from any thread,
  // and NoMutatorScope uses non-atomic counters that would race with the
  // main thread. triggerTimeoutAsyncBreak() is already thread-safe.
  runtime_.triggerTimeoutAsyncBreak();
}

void HermesRuntimeImpl::watchTimeLimit(uint32_t timeoutInMs) {
  vm::NoMutatorScope noMutatorScope{runtime_};
  auto &runtimeTimeLimitMonitor = runtime_.timeLimitMonitor;
  if (!runtimeTimeLimitMonitor) {
    runtimeTimeLimitMonitor = ::hermes::vm::TimeLimitMonitor::getOrCreate();
  }
  runtimeTimeLimitMonitor->watchRuntime(
      runtime_, std::chrono::milliseconds(timeoutInMs));
}

void HermesRuntimeImpl::unwatchTimeLimit() {
  vm::NoMutatorScope noMutatorScope{runtime_};
  vm::Runtime &runtime = runtime_;
  if (auto &runtimeTimeLimitMonitor = runtime.timeLimitMonitor) {
    runtimeTimeLimitMonitor->unwatchRuntime(runtime);
  }
}

namespace {

/// If the buffer contains an embedded terminating zero, shrink it, so it is
/// one past the size, as per the LLVM MemoryBuffer convention. Otherwise, copy
/// it into a new zero-terminated buffer.
std::unique_ptr<BufferAdapter> ensureZeroTerminated(
    const std::shared_ptr<const jsi::Buffer> &buf) {
  size_t size = buf->size();
  const uint8_t *data = buf->data();

  // Check for zero termination
  if (size != 0 && data[size - 1] == 0) {
    return std::make_unique<BufferAdapter>(buf, data, size - 1);
  } else {
    // Copy into a zero-terminated instance.
    return std::make_unique<BufferAdapter>(std::make_shared<jsi::StringBuffer>(
        std::string((const char *)data, size)));
  }
}

} // namespace

jsi::Value HermesRuntimeImpl::evaluateJavaScriptWithSourceMap(
    const std::shared_ptr<const jsi::Buffer> &buffer,
    const std::shared_ptr<const jsi::Buffer> &sourceMapBuf,
    const std::string &sourceURL) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  std::pair<std::unique_ptr<hbc::BCProvider>, std::string> bcErr{};
  vm::RuntimeModuleFlags runtimeFlags{};

  auto *api = jsi::castInterface<IHermesRootAPI>(makeHermesRootAPI());
  bool isBytecode = api->isHermesBytecode(buffer->data(), buffer->size());
#ifdef HERMESVM_PLATFORM_LOGGING
  hermesLog(
      "HermesVM", "Prepare JS on %s.", isBytecode ? "bytecode" : "source");
#endif

  // Construct the BC provider either from buffer or source.
  if (isBytecode) {
    if (sourceMapBuf) {
      throw std::logic_error("Source map cannot be specified with bytecode");
    }
    bcErr = hbc::BCProviderFromBuffer::createBCProviderFromBuffer(
        std::make_unique<BufferAdapter>(buffer));
    runtimeFlags.persistent = true;
  } else {
    llvh::StringRef sourceMap;
    std::unique_ptr<BufferAdapter> buf0;
    if (sourceMapBuf) {
      buf0 = ensureZeroTerminated(sourceMapBuf);
      // The source map StringRef must include the null terminator.
      sourceMap = llvh::StringRef((const char *)buf0->data(), buf0->size() + 1);
    }
    bcErr = hbc::createBCProviderFromSrc(
        ensureZeroTerminated(buffer), sourceURL, sourceMap, compileFlags_);
    if (bcErr.first) {
      runtimeFlags.persistent = bcErr.first->allowPersistent();
    }
  }
  if (!bcErr.first) {
    LOG_EXCEPTION_CAUSE(
        "Compiling JS failed: %s, sourceURL: %s",
        bcErr.second.c_str(),
        sourceURL.c_str());
    throw jsi::JSINativeException(
        "Compiling JS failed: " + std::move(bcErr.second) +
        ", sourceURL: " + sourceURL);
  }

  vm::GCScope gcScope(runtime_);
  auto res = runtime_.runBytecode(
      std::move(bcErr.first),
      runtimeFlags,
      sourceURL,
      vm::Runtime::makeNullHandle<vm::Environment>());
  checkStatus(res.getStatus());
  return valueFromHermesValue(*res);
}

#ifdef HERMES_SH_UNIT_FN
extern "C" SHUnit *HERMES_SH_UNIT_FN(void);
#endif

SHUnitCreator HermesRuntimeImpl::getSHUnitCreator() const {
#ifdef HERMES_SH_UNIT_FN
  return HERMES_SH_UNIT_FN;
#else
  return nullptr;
#endif
}

jsi::Value HermesRuntimeImpl::evaluateSHUnit(SHUnitCreator shUnitCreator) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::Runtime &runtime = runtime_;

  SHLegacyValue resOrExc;
  if (_sh_unit_init_guarded(
          vm::getSHRuntime(runtime), shUnitCreator, &resOrExc)) {
    return valueFromHermesValue(vm::HermesValue::fromRaw(resOrExc.raw));
  } else {
    vm::GCScope gcScope{runtime};
    runtime.setThrownValue(vm::HermesValue::fromRaw(resOrExc.raw));
    checkStatus(vm::ExecutionStatus::EXCEPTION);
    LLVM_BUILTIN_UNREACHABLE;
  }
}

SHRuntime *HermesRuntimeImpl::getSHRuntime() noexcept {
  return vm::getSHRuntime(runtime_);
}

void *HermesRuntimeImpl::getVMRuntimeUnsafe() const {
  return rt_.get();
}

size_t HermesRuntimeImpl::rootsListLengthForTests() const {
  vm::NoMutatorScope noMutatorScope{runtime_};
  return hermesValues_.sizeForTests();
}

namespace {

/// An implementation of PreparedJavaScript that can work across multiple
/// runtime instances. It stores either compiled bytecode (for eager
/// compilation) or source code (for lazy compilation).
class HermesPreparedJavaScript final : public jsi::PreparedJavaScript {
 public:
  /// For bytecode path (eager compilation): bytecodeBuffer is non-null
  std::shared_ptr<const jsi::Buffer> bytecodeBuffer;

  /// For source path (lazy compilation): sourceBuffer is non-null
  std::shared_ptr<const jsi::Buffer> sourceBuffer;
  std::shared_ptr<const jsi::Buffer> sourceMapBuffer; // may be null

  std::string sourceURL;

  explicit HermesPreparedJavaScript(
      std::shared_ptr<const jsi::Buffer> bytecodeBuffer,
      std::shared_ptr<const jsi::Buffer> sourceBuffer,
      std::shared_ptr<const jsi::Buffer> sourceMapBuffer,
      std::string sourceURL)
      : bytecodeBuffer(std::move(bytecodeBuffer)),
        sourceBuffer(std::move(sourceBuffer)),
        sourceMapBuffer(std::move(sourceMapBuffer)),
        sourceURL(std::move(sourceURL)) {}
};

} // namespace

std::shared_ptr<const jsi::PreparedJavaScript>
HermesRuntimeImpl::prepareJavaScriptWithSourceMap(
    const std::shared_ptr<const jsi::Buffer> &jsiBuffer,
    const std::shared_ptr<const jsi::Buffer> &sourceMapBuf,
    std::string sourceURL) {
  auto *api = jsi::castInterface<IHermesRootAPI>(makeHermesRootAPI());
  bool isBytecode = api->isHermesBytecode(jsiBuffer->data(), jsiBuffer->size());

  // If input is already bytecode, store it directly
  if (isBytecode) {
    if (sourceMapBuf) {
      throw std::logic_error("Source map cannot be specified with bytecode");
    }
#ifdef HERMESVM_PLATFORM_LOGGING
    hermesLog("HermesVM", "Prepare JS on bytecode.");
#endif
    return std::make_shared<const HermesPreparedJavaScript>(
        jsiBuffer, // bytecodeBuffer
        nullptr, // sourceBuffer
        nullptr, // sourceMapBuffer
        std::move(sourceURL));
  }

  // Check if we should eagerly compile or defer (lazy compilation)
  bool shouldEagerlyCompile = !compileFlags_.lazy ||
      jsiBuffer->size() < compileFlags_.preemptiveFileCompilationThreshold;

  if (shouldEagerlyCompile) {
    // Eager compilation: compile to bytecode now
#ifdef HERMESVM_PLATFORM_LOGGING
    hermesLog("HermesVM", "Prepare JS with eager compilation.");
#endif

    llvh::StringRef sourceMap;
    std::unique_ptr<BufferAdapter> buf0;
    if (sourceMapBuf) {
      buf0 = ensureZeroTerminated(sourceMapBuf);
      sourceMap = llvh::StringRef((const char *)buf0->data(), buf0->size() + 1);
    }

    // Ensure compilation is eager.
    auto cflags = compileFlags_;
    cflags.lazy = false;
    auto bcErr = hbc::createBCProviderFromSrc(
        ensureZeroTerminated(jsiBuffer), sourceURL, sourceMap, cflags);

    if (!bcErr.first) {
      LOG_EXCEPTION_CAUSE(
          "Compiling JS failed: %s, sourceURL: %s",
          bcErr.second.c_str(),
          sourceURL.c_str());
      throw jsi::JSINativeException(
          "Compiling JS failed: " + std::move(bcErr.second) +
          ", sourceURL: " + sourceURL);
    }

    // Extract bytecode from BCProvider and store it
    std::string bytecodeStr;
    llvh::raw_string_ostream bcstream(bytecodeStr);

    ::hermes::BytecodeGenerationOptions opts(::hermes::EmitBundle);

    hbc::serializeBytecodeModule(
        *bcErr.first->getBytecodeModule(), {}, bcstream, opts);

    bcstream.flush();

    std::shared_ptr<const jsi::Buffer> bytecodeBuffer =
        std::make_shared<jsi::StringBuffer>(std::move(bytecodeStr));

    return std::make_shared<const HermesPreparedJavaScript>(
        bytecodeBuffer, // bytecodeBuffer
        nullptr, // sourceBuffer
        nullptr, // sourceMapBuffer
        std::move(sourceURL));
  }

  // Lazy compilation: store source for compilation at evaluation time
#ifdef HERMESVM_PLATFORM_LOGGING
  hermesLog("HermesVM", "Prepare JS with lazy compilation (store source).");
#endif
  return std::make_shared<const HermesPreparedJavaScript>(
      nullptr, // bytecodeBuffer
      jsiBuffer, // sourceBuffer
      sourceMapBuf, // sourceMapBuffer
      std::move(sourceURL));
}

std::shared_ptr<const jsi::PreparedJavaScript>
HermesRuntimeImpl::prepareJavaScript(
    const std::shared_ptr<const jsi::Buffer> &jsiBuffer,
    std::string sourceURL) {
  return prepareJavaScriptWithSourceMap(jsiBuffer, nullptr, sourceURL);
}

jsi::Value HermesRuntimeImpl::evaluatePreparedJavaScript(
    const std::shared_ptr<const jsi::PreparedJavaScript> &js) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  assert(
      dynamic_cast<const HermesPreparedJavaScript *>(js.get()) &&
      "js must be an instance of HermesPreparedJavaScript");
  const auto *hermesPrep =
      static_cast<const HermesPreparedJavaScript *>(js.get());

  // Check which path: bytecode or source
  if (hermesPrep->bytecodeBuffer) {
    // Bytecode path: create BCProvider and execute
    auto bcErr = hbc::BCProviderFromBuffer::createBCProviderFromBuffer(
        std::make_unique<BufferAdapter>(hermesPrep->bytecodeBuffer));

    if (!bcErr.first) {
      throw jsi::JSINativeException(
          "Failed to create BCProvider from bytecode: " + bcErr.second);
    }

    vm::GCScope gcScope(runtime_);
    vm::RuntimeModuleFlags runtimeFlags{};
    runtimeFlags.persistent = true;
    auto res = runtime_.runBytecode(
        std::move(bcErr.first),
        runtimeFlags,
        hermesPrep->sourceURL,
        vm::Runtime::makeNullHandle<vm::Environment>());
    checkStatus(res.getStatus());
    return valueFromHermesValue(*res);
  } else {
    // Source path: delegate to evaluateJavaScript
    // Each runtime compiles independently, avoiding shared mutable state
    return evaluateJavaScriptWithSourceMap(
        hermesPrep->sourceBuffer,
        hermesPrep->sourceMapBuffer,
        hermesPrep->sourceURL);
  }
}

jsi::Value HermesRuntimeImpl::evaluateJavaScript(
    const std::shared_ptr<const jsi::Buffer> &buffer,
    const std::string &sourceURL) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  return evaluateJavaScriptWithSourceMap(buffer, nullptr, sourceURL);
}

void HermesRuntimeImpl::queueMicrotask(const jsi::Function &callback) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  if (LLVM_UNLIKELY(!runtime_.hasMicrotaskQueue())) {
    throw jsi::JSINativeException(
        "Could not enqueue microtask because they are disabled in this runtime");
  }

  vm::Handle<vm::Callable> handle =
      vm::Handle<vm::Callable>::vmcast(&phv(callback));
  runtime_.enqueueJob(handle.get());
}

bool HermesRuntimeImpl::drainMicrotasks(int maxMicrotasksHint) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  if (runtime_.hasMicrotaskQueue()) {
    checkStatus(runtime_.drainJobs());
  }
  // \c drainJobs is currently an unbounded execution, hence no exceptions
  // implies drained until TODO(T89426441): \c maxMicrotasksHint is supported
  runtime_.clearKeptObjects();
  checkStatus(runtime_.cleanUpFinalizationCallbacks());
  return true;
}

jsi::Object HermesRuntimeImpl::global() {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  return add<jsi::Object>(runtime_.getGlobal().getHermesValue());
}

std::string HermesRuntimeImpl::description() {
  std::string gcName = runtime_.getHeap().getName();
  if (gcName.empty()) {
    return "HermesRuntime";
  } else {
    return "HermesRuntime[" + gcName + "]";
  }
}

bool HermesRuntimeImpl::isInspectable() {
#ifdef HERMES_ENABLE_DEBUGGER
  return true;
#else
  return false;
#endif
}

jsi::Instrumentation &HermesRuntimeImpl::instrumentation() {
  return *this;
}

jsi::Runtime::PointerValue *HermesRuntimeImpl::cloneSymbol(
    const Runtime::PointerValue *pv) {
  return clone(pv);
}

jsi::Runtime::PointerValue *HermesRuntimeImpl::cloneBigInt(
    const Runtime::PointerValue *pv) {
  return clone(pv);
}

jsi::Runtime::PointerValue *HermesRuntimeImpl::cloneString(
    const Runtime::PointerValue *pv) {
  return clone(pv);
}

jsi::Runtime::PointerValue *HermesRuntimeImpl::cloneObject(
    const Runtime::PointerValue *pv) {
  return clone(pv);
}

jsi::Runtime::PointerValue *HermesRuntimeImpl::clonePropNameID(
    const Runtime::PointerValue *pv) {
  return clone(pv);
}

jsi::PropNameID HermesRuntimeImpl::createPropNameIDFromAscii(
    const char *str,
    size_t length) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
#ifndef NDEBUG
  for (size_t i = 0; i < length; ++i) {
    assert(
        static_cast<unsigned char>(str[i]) < 128 &&
        "non-ASCII character in property name");
  }
#endif

  vm::GCScope gcScope(runtime_);
  auto cr = vm::stringToSymbolID(
      runtime_,
      vm::StringPrimitive::createNoThrow(
          runtime_, llvh::StringRef(str, length)));
  checkStatus(cr.getStatus());
  return add<jsi::PropNameID>(cr->getHermesValue());
}

jsi::PropNameID HermesRuntimeImpl::createPropNameIDFromUtf8(
    const uint8_t *utf8,
    size_t length) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  auto cr = vm::stringToSymbolID(
      runtime_,
      vm::createPseudoHandle(stringHVFromUtf8(utf8, length).getString()));
  checkStatus(cr.getStatus());
  return add<jsi::PropNameID>(cr->getHermesValue());
}

jsi::PropNameID HermesRuntimeImpl::createPropNameIDFromUtf16(
    const char16_t *utf16,
    size_t length) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  auto cr = vm::stringToSymbolID(
      runtime_,
      vm::createPseudoHandle(stringHVFromUtf16(utf16, length).getString()));
  checkStatus(cr.getStatus());
  return add<jsi::PropNameID>(cr->getHermesValue());
}

jsi::PropNameID HermesRuntimeImpl::createPropNameIDFromString(
    const jsi::String &str) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  auto cr = vm::stringToSymbolID(
      runtime_, vm::createPseudoHandle(phv(str).getString()));
  checkStatus(cr.getStatus());
  return add<jsi::PropNameID>(cr->getHermesValue());
}

jsi::PropNameID HermesRuntimeImpl::createPropNameIDFromSymbol(
    const jsi::Symbol &sym) {
  return add<jsi::PropNameID>(phv(sym));
}

std::string HermesRuntimeImpl::utf8(const jsi::PropNameID &sym) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  vm::SymbolID id = phv(sym).getSymbol();
  auto view = runtime_.getIdentifierTable().getStringView(runtime_, id);
  return utf8FromStringView(view);
}

bool HermesRuntimeImpl::compare(
    const jsi::PropNameID &a,
    const jsi::PropNameID &b) {
  return phv(a).getSymbol() == phv(b).getSymbol();
}

std::string HermesRuntimeImpl::utf8FromStringView(vm::StringView view) {
  if (view.isASCII())
    return std::string{view.castToCharPtr(), view.length()};

  std::string ret;
  ::hermes::convertUTF16ToUTF8WithReplacements(
      ret, llvh::ArrayRef{view.castToChar16Ptr(), view.length()});
  return ret;
}

vm::UTF16Ref HermesRuntimeImpl::utf16FromErrorWhat(
    const std::exception &ex,
    llvh::SmallVectorImpl<llvh::UTF16> &buf) {
  assert(buf.empty() && "buf must be empty");
  if (!llvh::convertUTF8ToUTF16String(ex.what(), buf))
    return {u"<invalid utf-8 exception message>"};
  static_assert(
      sizeof(char16_t) == sizeof(llvh::UTF16),
      "Cannot reinterpret UTF16 as char16_t");
  return vm::UTF16Ref{(char16_t *)buf.data(), buf.size()};
}

std::string HermesRuntimeImpl::symbolToString(const jsi::Symbol &sym) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  auto res = symbolDescriptiveString(
      runtime_,
      ::hermes::vm::Handle<::hermes::vm::SymbolID>::vmcast(&phv(sym)));
  checkStatus(res.getStatus());

  return utf8FromStringView(
      vm::StringPrimitive::createStringView(runtime_, *res));
}

jsi::BigInt HermesRuntimeImpl::createBigIntFromInt64(int64_t value) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  vm::CallResult<vm::HermesValue> res =
      vm::BigIntPrimitive::fromSigned(runtime_, value);
  checkStatus(res.getStatus());
  return add<jsi::BigInt>(*res);
}

jsi::BigInt HermesRuntimeImpl::createBigIntFromUint64(uint64_t value) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  vm::CallResult<vm::HermesValue> res =
      vm::BigIntPrimitive::fromUnsigned(runtime_, value);
  checkStatus(res.getStatus());
  return add<jsi::BigInt>(*res);
}

bool HermesRuntimeImpl::bigintIsInt64(const jsi::BigInt &bigint) {
  constexpr bool signedTruncation = true;
  return phv(bigint).getBigInt()->isTruncationToSingleDigitLossless(
      signedTruncation);
}

bool HermesRuntimeImpl::bigintIsUint64(const jsi::BigInt &bigint) {
  constexpr bool signedTruncation = false;
  return phv(bigint).getBigInt()->isTruncationToSingleDigitLossless(
      signedTruncation);
}

uint64_t HermesRuntimeImpl::truncate(const jsi::BigInt &bigint) {
  auto digit = phv(bigint).getBigInt()->truncateToSingleDigit();
  static_assert(
      sizeof(digit) == sizeof(uint64_t),
      "BigInt digit is no longer sizeof(uint64_t) bytes.");
  return digit;
}

jsi::String HermesRuntimeImpl::bigintToString(
    const jsi::BigInt &bigint,
    int radix) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  if (radix < 2 || radix > 36) {
    throwJSErrorWithMessage("Invalid radix ", radix, " to BigInt.toString");
  }

  vm::GCScope gcScope(runtime_);
  vm::CallResult<vm::HermesValue> toStringRes = vm::BigIntPrimitive::toString(
      runtime_, vm::createPseudoHandle(phv(bigint).getBigInt()), radix);

  checkStatus(toStringRes.getStatus());
  return add<jsi::String>(*toStringRes);
}

jsi::String HermesRuntimeImpl::createStringFromAscii(
    const char *str,
    size_t length) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
#ifndef NDEBUG
  for (size_t i = 0; i < length; ++i) {
    assert(
        static_cast<unsigned char>(str[i]) < 128 &&
        "non-ASCII character in string");
  }
#endif
  vm::GCScope gcScope(runtime_);
  return add<jsi::String>(stringHVFromAscii(str, length));
}

jsi::String HermesRuntimeImpl::createStringFromUtf8(
    const uint8_t *utf8,
    size_t length) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  return add<jsi::String>(stringHVFromUtf8(utf8, length));
}

jsi::String HermesRuntimeImpl::createStringFromUtf16(
    const char16_t *utf16,
    size_t length) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  return add<jsi::String>(stringHVFromUtf16(utf16, length));
}

jsi::String HermesRuntimeImpl::createStringFromAsciiWriter(
    size_t length,
    void *ctx,
    IStringWriter::AsciiWriteFn write) {
  return createStringFromWriter(length, ctx, write);
}

jsi::String HermesRuntimeImpl::createStringFromUtf16Writer(
    size_t length,
    void *ctx,
    IStringWriter::Utf16WriteFn write) {
  return createStringFromWriter(length, ctx, write);
}

template <typename S, typename = void>
struct HasResizeDefaultInit : std::false_type {};
template <typename S>
struct HasResizeDefaultInit<
    S,
    std::void_t<decltype(std::declval<S &>().__resize_default_init(0))>>
    : std::true_type {};

/// Resizes \p s to \p n without zero-filling it where the standard library
/// allows (libc++); the caller overwrites every element.
template <typename S>
static void resizeUninitialized(S &s, size_t n) {
  if constexpr (HasResizeDefaultInit<S>::value) {
    s.__resize_default_init(n);
  } else {
    s.resize(n);
  }
}

template <typename CharT>
jsi::String HermesRuntimeImpl::createStringFromWriter(
    size_t length,
    void *ctx,
    StringWriteFn<CharT> write) {
  constexpr bool ascii = std::is_same<CharT, char>::value;
  // Every string is contiguous today, so write is invoked once for the whole
  // range.
  if (length < 2) {
    // 0/1-char strings come from the interned table in createEfficient.
    CharT buf[1];
    if (length != 0 && !write(ctx, buf, 0, length)) {
      throw jsi::JSError(*this, "String writer failed");
    }
    if constexpr (ascii) {
      return createStringFromAscii(buf, length);
    } else {
      return createStringFromUtf16(buf, length);
    }
  }
  ExecutionScopeRAII scopeRAII(mutatorScope);
  if (length > vm::StringPrimitive::MAX_STRING_LENGTH) {
    // The same error the other createStringFrom* methods give.
    checkStatus(runtime_.raiseRangeError("String length exceeds limit"));
  }
  if (!ascii && length >= vm::StringPrimitive::EXTERNAL_STRING_THRESHOLD) {
    // External strings adopt a std::basic_string. Filling one we own skips
    // the zero-fill of StringPrimitive::create, which for char16_t is a scalar
    // loop that costs more than the copy this writer saves (for char it is a
    // fast memset, so ASCII keeps the regular path).
    // Reject requests that exceed the limit before allocating native memory.
    if (!runtime_.getHeap().canAllocExternalMemory(length * sizeof(CharT))) {
      checkStatus(runtime_.raiseRangeError(
          "Cannot allocate an external string primitive."));
    }
    std::basic_string<CharT> storage;
    resizeUninitialized(storage, length);
    // Check the actual capacity too: std::basic_string may over-allocate.
    if (!runtime_.getHeap().canAllocExternalMemory(
            storage.capacity() * sizeof(CharT))) {
      checkStatus(runtime_.raiseRangeError(
          "Cannot allocate an external string primitive."));
    }
    if (!write(ctx, &storage[0], 0, length)) {
      throw jsi::JSError(*this, "String writer failed");
    }
    auto res =
        vm::StringPrimitive::createEfficient(runtime_, std::move(storage));
    checkStatus(res.getStatus());
    return add<jsi::String>(*res);
  }
  auto res = vm::StringPrimitive::create(
      runtime_, static_cast<uint32_t>(length), /* asciiNotUTF16 */ ascii);
  checkStatus(res.getStatus());
  CharT *dst;
  if constexpr (ascii) {
    dst = res->getString()->castToASCIIPointerForWrite();
  } else {
    dst = res->getString()->castToUTF16PointerForWrite();
  }
  bool ok;
  {
    // The new cell is unrooted, so write must not allocate on the JS heap.
    vm::NoAllocScope noAlloc(runtime_);
    ok = write(ctx, dst, 0, length);
  }
  if (!ok) {
    // Zero the discarded string: an ASCII string must not hold other bytes.
    std::fill_n(dst, length, CharT(0));
    throw jsi::JSError(*this, "String writer failed");
  }
#ifndef NDEBUG
  if constexpr (ascii) {
    for (size_t i = 0; i < length; ++i) {
      assert(
          static_cast<unsigned char>(dst[i]) < 128 &&
          "non-ASCII character in string");
    }
  }
#endif
  return add<jsi::String>(*res);
}

std::string HermesRuntimeImpl::utf8(const jsi::String &str) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  return utf8FromStringView(
      vm::StringPrimitive::createStringView(runtime_, stringHandle(str)));
}

size_t HermesRuntimeImpl::length(const jsi::String &str) {
  return stringHandle(str)->getStringLength();
}

std::u16string HermesRuntimeImpl::utf16(const jsi::String &str) {
  vm::NoMutatorScope noMutatorScope{runtime_};
  auto *stringPrim = phv(str).getString();
  if (stringPrim->isASCII()) {
    auto arrayRef = stringPrim->getStringRef<char>();
    return std::u16string(arrayRef.begin(), arrayRef.end());
  }
  auto arrayRef = stringPrim->getStringRef<char16_t>();
  return std::u16string(arrayRef.data(), arrayRef.size());
}

std::u16string HermesRuntimeImpl::utf16(const jsi::PropNameID &sym) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::SymbolID id = phv(sym).getSymbol();
  auto *stringPrim = runtime_.getStringPrimFromSymbolID(id);
  if (stringPrim->isASCII()) {
    auto arrayRef = stringPrim->getStringRef<char>();
    return std::u16string(arrayRef.begin(), arrayRef.end());
  }
  auto arrayRef = stringPrim->getStringRef<char16_t>();
  return std::u16string(arrayRef.data(), arrayRef.size());
}

void HermesRuntimeImpl::getStringData(
    const jsi::String &str,
    void *ctx,
    void (*cb)(void *ctx, bool ascii, const void *data, size_t num)) {
  vm::NoMutatorScope noMutatorScope{runtime_};
  auto *stringPrim = phv(str).getString();
  if (stringPrim->isASCII()) {
    auto arrayRef = stringPrim->getStringRef<char>();
    cb(ctx, true, arrayRef.data(), arrayRef.size());
  } else {
    auto arrayRef = stringPrim->getStringRef<char16_t>();
    cb(ctx, false, arrayRef.data(), arrayRef.size());
  }
}

void HermesRuntimeImpl::getPropNameIdData(
    const jsi::PropNameID &sym,
    void *ctx,
    void (*cb)(void *ctx, bool ascii, const void *data, size_t num)) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::SymbolID id = phv(sym).getSymbol();
  auto *stringPrim = runtime_.getStringPrimFromSymbolID(id);
  if (stringPrim->isASCII()) {
    auto arrayRef = stringPrim->getStringRef<char>();
    cb(ctx, true, arrayRef.data(), arrayRef.size());
  } else {
    auto arrayRef = stringPrim->getStringRef<char16_t>();
    cb(ctx, false, arrayRef.data(), arrayRef.size());
  }
}

jsi::Value HermesRuntimeImpl::createValueFromJsonUtf8(
    const uint8_t *json,
    size_t length) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  llvh::ArrayRef<uint8_t> ref(json, length);
  vm::CallResult<vm::HermesValue> res =
      runtimeJSONParseRef(runtime_, ::hermes::UTF16Stream(ref));
  checkStatus(res.getStatus());
  return valueFromHermesValue(*res);
}

jsi::Object HermesRuntimeImpl::createObject() {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  return add<jsi::Object>(vm::JSObject::create(runtime_).getHermesValue());
}

jsi::Object HermesRuntimeImpl::createObject(
    std::shared_ptr<jsi::HostObject> ho) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);

  auto objRes = vm::HostObject::createWithoutPrototype(
      runtime_, std::make_unique<JsiProxy>(*this, ho));
  checkStatus(objRes.getStatus());
  return add<jsi::Object>(*objRes);
}

jsi::Object HermesRuntimeImpl::createObjectWithPrototype(
    const jsi::Value &prototype) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  if (!prototype.isObject() && !prototype.isNull()) {
    throw jsi::JSError(
        *this, "Object prototype argument must be an Object or null");
  }

  vm::PinnedHermesValue numberStorage;
  auto object = vm::JSObject::create(
      runtime_,
      vm::Handle<vm::JSObject>::dyn_vmcast(
          vmHandleFromValue(prototype, &numberStorage)));
  return add<jsi::Object>(object.getHermesValue());
}

std::shared_ptr<jsi::HostObject> HermesRuntimeImpl::getHostObject(
    const jsi::Object &obj) {
  vm::NoMutatorScope noMutatorScope{runtime_};
  const vm::HostObjectProxy *proxy =
      vm::vmcast<vm::HostObject>(phv(obj))->getProxy();
  return static_cast<const JsiProxy *>(proxy)->ho_;
}

bool HermesRuntimeImpl::hasNativeState(const jsi::Object &obj) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  auto h = handle(obj);
  if (h->isProxyObject(runtime_) || h->isHostObject(runtime_)) {
    return false;
  }
  vm::NamedPropertyDescriptor desc;
  return vm::JSObject::getOwnNamedDescriptor(
      h,
      runtime_,
      vm::Predefined::getSymbolID(vm::Predefined::InternalPropertyNativeState),
      desc);
}

void HermesRuntimeImpl::setNativeState(
    const jsi::Object &obj,
    std::shared_ptr<jsi::NativeState> state) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  struct : public vm::Locals {
    vm::PinnedValue<vm::NativeState> ns;
  } lv;
  vm::LocalsRAII lraii(runtime_, &lv);

  vm::GCScope gcScope(runtime_);
  auto h = handle(obj);
  if (h->isProxyObject(runtime_)) {
    throw jsi::JSINativeException("native state unsupported on Proxy");
  } else if (h->isHostObject(runtime_)) {
    throw jsi::JSINativeException("native state unsupported on HostObject");
  }
  // Allocate a NativeStateBox on the C++ heap and use it as context of
  // NativeState. The box also carries a reference to the runtime so the
  // Release callback can queue destruction to the finalizer thread.
  auto *nativeStateCtx = new NativeStateContext{std::move(state), *this};
  lv.ns = vm::NativeState::create(
      runtime_, nativeStateCtx, NativeStateContext::finalize);
  auto res = vm::JSObject::defineOwnProperty(
      h,
      runtime_,
      vm::Predefined::getSymbolID(vm::Predefined::InternalPropertyNativeState),
      vm::DefinePropertyFlags::getDefaultNewPropertyFlags(),
      lv.ns);
  // NB: If setting the property failed, then the NativeState cell will soon
  // be unreachable, and when it's later finalized, the shared_ptr will be
  // deleted.
  checkStatus(res.getStatus());
  if (!*res) {
    throw jsi::JSINativeException(
        "failed to define internal native state property");
  }
}

std::shared_ptr<jsi::NativeState> HermesRuntimeImpl::getNativeState(
    const jsi::Object &obj) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  assert(hasNativeState(obj) && "object lacks native state");
  auto h = handle(obj);
  vm::NamedPropertyDescriptor desc;
  bool exists = vm::JSObject::getOwnNamedDescriptor(
      h,
      runtime_,
      vm::Predefined::getSymbolID(vm::Predefined::InternalPropertyNativeState),
      desc);
  (void)exists;
  assert(exists && "hasNativeState lied");
  // Raw pointers below.
  vm::NoAllocScope scope(runtime_);
  vm::NativeState *ns = vm::vmcast<vm::NativeState>(
      vm::JSObject::getNamedSlotValueUnsafe(*h, runtime_, desc)
          .getObject(runtime_));
  return reinterpret_cast<NativeStateContext *>(ns->context())->state;
}

void HermesRuntimeImpl::setExternalMemoryPressure(
    const jsi::Object &obj,
    size_t amt) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  struct : public vm::Locals {
    vm::PinnedValue<vm::NativeState> nsHnd;
  } lv;
  vm::LocalsRAII lraii(runtime_, &lv);

  vm::GCScope gcScope(runtime_);
  auto h = handle(obj);
  if (h->isProxyObject(runtime_))
    throw jsi::JSINativeException("Cannot set external memory on Proxy");

  // Check if the internal property is already set. If so, we can update the
  // associated external memory in place.
  vm::NamedPropertyDescriptor desc;
  bool exists = vm::JSObject::getOwnNamedDescriptor(
      h,
      runtime_,
      vm::Predefined::getSymbolID(
          vm::Predefined::InternalPropertyExternalMemoryPressure),
      desc);

  vm::NativeState *ns;
  if (exists) {
    ns = vm::vmcast<vm::NativeState>(
        vm::JSObject::getNamedSlotValueUnsafe(*h, runtime_, desc)
            .getObject(runtime_));
  } else {
    // An object without the property implicitly has no external memory, so
    // setting it to 0 is a no-op.
    if (amt == 0)
      return;

    auto debitMem = [](vm::GC &gc, vm::NativeState *ns) {
      auto amt = reinterpret_cast<uintptr_t>(ns->context());
      gc.debitExternalMemory(ns, amt);
    };

    // This is the first time adding external memory to this object. Create a
    // new NativeState. We use the context pointer to store the external memory
    // amount.
    lv.nsHnd = vm::NativeState::create(
        runtime_, reinterpret_cast<void *>(0), debitMem);

    // Use defineNewOwnProperty to create the new property since we know it
    // doesn't exist. Note that this also bypasses the extensibility check on
    // the object.
    auto res = vm::JSObject::defineNewOwnProperty(
        h,
        runtime_,
        vm::Predefined::getSymbolID(
            vm::Predefined::InternalPropertyExternalMemoryPressure),
        vm::PropertyFlags::defaultNewNamedPropertyFlags(),
        lv.nsHnd);
    checkStatus(res);
    ns = *lv.nsHnd;
  }

  auto curAmt = reinterpret_cast<uintptr_t>(ns->context());
  assert(llvh::isUInt<32>(curAmt) && "Amount is too large.");

  // Nothing to do if the amount is unchanged.
  if (amt == curAmt)
    return;

  // The GC does not support adding more than a 32 bit amount.
  if (!llvh::isUInt<32>(amt))
    throw jsi::JSINativeException("Amount is too large.");

  // Try to credit or debit the delta depending on whether the new amount is
  // larger.
  if (amt > curAmt) {
    auto delta = amt - curAmt;
    if (!runtime_.getHeap().canAllocExternalMemory(delta))
      throw jsi::JSINativeException("External memory is too high.");
    runtime_.getHeap().creditExternalMemory(ns, delta);
  } else {
    runtime_.getHeap().debitExternalMemory(ns, curAmt - amt);
  }

  ns->setContext(reinterpret_cast<void *>(amt));
}

void HermesRuntimeImpl::setPrototypeOf(
    const jsi::Object &object,
    const jsi::Value &prototype) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  if (!prototype.isObject() && !prototype.isNull()) {
    throw jsi::JSError(
        *this, "Object prototype argument must be an Object or null");
  }

  auto cr = vm::JSObject::setParent(
      vm::vmcast<vm::JSObject>(phv(object)),
      runtime_,
      vm::dyn_vmcast<vm::JSObject>(hvFromValue(prototype)),
      vm::PropOpFlags().plusThrowOnError());
  checkStatus(cr.getStatus());
}

jsi::Value HermesRuntimeImpl::getPrototypeOf(const jsi::Object &object) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::CallResult<vm::PseudoHandle<vm::JSObject>> cr =
      vm::JSObject::getPrototypeOf(handle(object), runtime_);
  checkStatus(cr.getStatus());
  if (!*cr) {
    return jsi::Value::null();
  }
  return valueFromHermesValue(cr->getHermesValue());
}

jsi::Value HermesRuntimeImpl::getProperty(
    const jsi::Object &obj,
    const jsi::String &name) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  auto h = handle(obj);
  auto res = h->getComputed_RJS(h, runtime_, stringHandle(name));
  checkStatus(res.getStatus());
  return valueFromHermesValue(res->get());
}

jsi::Value HermesRuntimeImpl::getProperty(
    const jsi::Object &obj,
    const jsi::PropNameID &name) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  auto h = handle(obj);
  vm::SymbolID nameID = phv(name).getSymbol();
  auto res = h->getNamedOrIndexed(h, runtime_, nameID);
  checkStatus(res.getStatus());
  return valueFromHermesValue(res->get());
}

jsi::Value HermesRuntimeImpl::getProperty(
    const jsi::Object &obj,
    const jsi::Value &name) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  vm::PinnedHermesValue numStorage;
  auto nameHandle = vmHandleFromValue(name, &numStorage);
  auto objHandle = handle(obj);
  auto res = vm::JSObject::getComputed_RJS(objHandle, runtime_, nameHandle);
  checkStatus(res.getStatus());
  return valueFromHermesValue(res->get());
}

bool HermesRuntimeImpl::hasProperty(
    const jsi::Object &obj,
    const jsi::String &name) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  auto h = handle(obj);
  auto result = h->hasComputed(h, runtime_, stringHandle(name));
  checkStatus(result.getStatus());
  return result.getValue();
}

bool HermesRuntimeImpl::hasProperty(
    const jsi::Object &obj,
    const jsi::PropNameID &name) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  auto h = handle(obj);
  vm::SymbolID nameID = phv(name).getSymbol();
  auto result = h->hasNamedOrIndexed(h, runtime_, nameID);
  checkStatus(result.getStatus());
  return result.getValue();
}

bool HermesRuntimeImpl::hasProperty(
    const jsi::Object &obj,
    const jsi::Value &name) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  vm::PinnedHermesValue numStorage;
  auto nameHandle = vmHandleFromValue(name, &numStorage);
  auto objHandle = handle(obj);
  auto res = vm::JSObject::hasComputed(objHandle, runtime_, nameHandle);
  checkStatus(res.getStatus());
  return res.getValue();
}

void HermesRuntimeImpl::setPropertyValue(
    const jsi::Object &obj,
    const jsi::String &name,
    const jsi::Value &value) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  vm::PinnedHermesValue valueStorage;
  auto h = handle(obj);
  checkStatus(h->putComputed_RJS(
                   h,
                   runtime_,
                   stringHandle(name),
                   vmHandleFromValue(value, &valueStorage),
                   vm::PropOpFlags().plusThrowOnError())
                  .getStatus());
}

void HermesRuntimeImpl::setPropertyValue(
    const jsi::Object &obj,
    const jsi::PropNameID &name,
    const jsi::Value &value) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  vm::PinnedHermesValue valueStorage;
  auto h = handle(obj);
  vm::SymbolID nameID = phv(name).getSymbol();
  checkStatus(h->putNamedOrIndexed(
                   h,
                   runtime_,
                   nameID,
                   vmHandleFromValue(value, &valueStorage),
                   vm::PropOpFlags().plusThrowOnError())
                  .getStatus());
}

void HermesRuntimeImpl::setPropertyValue(
    const jsi::Object &obj,
    const jsi::Value &name,
    const jsi::Value &value) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  vm::PinnedHermesValue nameNumStorage;
  vm::PinnedHermesValue valNumStorage;
  auto nameHandle = vmHandleFromValue(name, &nameNumStorage);
  auto valhandle = vmHandleFromValue(value, &valNumStorage);
  auto objHandle = handle(obj);
  auto res = vm::JSObject::putComputed_RJS(
      objHandle,
      runtime_,
      nameHandle,
      valhandle,
      vm::PropOpFlags().plusThrowOnError());
  checkStatus(res.getStatus());
}

void HermesRuntimeImpl::deleteProperty(
    const jsi::Object &obj,
    const jsi::PropNameID &name) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  auto res = vm::JSObject::deleteNamed(
      handle(obj),
      runtime_,
      phv(name).getSymbol(),
      vm::PropOpFlags().plusThrowOnError());
  checkStatus(res.getStatus());
}

void HermesRuntimeImpl::deleteProperty(
    const jsi::Object &obj,
    const jsi::String &name) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  auto res = vm::JSObject::deleteComputed(
      handle(obj),
      runtime_,
      stringHandle(name),
      vm::PropOpFlags().plusThrowOnError());
  checkStatus(res.getStatus());
}

void HermesRuntimeImpl::deleteProperty(
    const jsi::Object &obj,
    const jsi::Value &name) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  vm::PinnedHermesValue numStorage;
  auto res = vm::JSObject::deleteComputed(
      handle(obj),
      runtime_,
      vmHandleFromValue(name, &numStorage),
      vm::PropOpFlags().plusThrowOnError());
  checkStatus(res.getStatus());
}

bool HermesRuntimeImpl::isArray(const jsi::Object &obj) const {
  ExecutionScopeRAII scopeRAII(
      const_cast<HermesRuntimeImpl *>(this)->mutatorScope);
  if (vm::vmisa<vm::JSArray>(phv(obj))) {
    return true;
  }
  auto cr = vm::isArray(runtime_, vm::vmcast<vm::JSObject>(phv(obj)));
  const_cast<HermesRuntimeImpl *>(this)->checkStatus(cr.getStatus());
  return *cr;
}

bool HermesRuntimeImpl::isArrayBuffer(const jsi::Object &obj) const {
  return vm::vmisa<vm::JSArrayBuffer>(phv(obj));
}

bool HermesRuntimeImpl::isFunction(const jsi::Object &obj) const {
  return vm::vmisa<vm::Callable>(phv(obj));
}

bool HermesRuntimeImpl::isHostObject(const jsi::Object &obj) const {
  return vm::vmisa<vm::HostObject>(phv(obj));
}

bool HermesRuntimeImpl::isHostFunction(const jsi::Function &func) const {
  return vm::vmisa<vm::FinalizableNativeFunction>(phv(func));
}

jsi::Array HermesRuntimeImpl::getPropertyNames(const jsi::Object &obj) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  uint32_t beginIndex;
  uint32_t endIndex;
  vm::CallResult<vm::Handle<vm::ArrayStorageSmall>> cr =
      vm::getForInPropertyNames(runtime_, handle(obj), beginIndex, endIndex);
  checkStatus(cr.getStatus());
  vm::Handle<vm::ArrayStorageSmall> arr = *cr;
  size_t length = endIndex - beginIndex;

  auto ret = createArray(length);
  for (size_t i = 0; i < length; ++i) {
    vm::SmallHermesValue name = arr->at(beginIndex + i);
    if (name.isString()) {
      ret.setValueAtIndex(
          *this,
          i,
          valueFromHermesValue(
              vm::HermesValue::encodeStringValue(name.getString(runtime_))));
    } else if (name.isSymbol()) {
      // May allocate. 'name' must not be used afterwards.
      vm::StringPrimitive *str =
          runtime_.getStringPrimFromSymbolID(name.getSymbol());
      ret.setValueAtIndex(
          *this,
          i,
          valueFromHermesValue(vm::HermesValue::encodeStringValue(str)));
    } else if (name.isNumber()) {
      std::string s;
      llvh::raw_string_ostream os(s);
      os << static_cast<size_t>(name.getNumber(runtime_));
      ret.setValueAtIndex(
          *this, i, jsi::String::createFromAscii(*this, os.str()));
    } else {
      llvm_unreachable("property name is not String or Number");
    }
  }

  return ret;
}

jsi::WeakObject HermesRuntimeImpl::createWeakObject(const jsi::Object &obj) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  return addWeak(
      vm::WeakRoot<vm::JSObject>(
          static_cast<vm::JSObject *>(phv(obj).getObject()), runtime_));
}

jsi::Value HermesRuntimeImpl::lockWeakObject(const jsi::WeakObject &wo) {
  vm::NoMutatorScope noMutatorScope{runtime_};
  const vm::WeakRoot<vm::JSObject> &wr = weakRoot(wo);

  if (const auto ptr = wr.get(runtime_, runtime_.getHeap()))
    return add<jsi::Object>(vm::HermesValue::encodeObjectValue(ptr));

  return jsi::Value();
}

jsi::Array HermesRuntimeImpl::createArray(size_t length) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  auto result = vm::JSArray::create(runtime_, length, length);
  checkStatus(result.getStatus());
  return add<jsi::Array>(result->getHermesValue());
}

jsi::ArrayBuffer HermesRuntimeImpl::createArrayBuffer(
    std::shared_ptr<jsi::MutableBuffer> buffer) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  struct : public vm::Locals {
    vm::PinnedValue<vm::JSArrayBuffer> buf;
  } lv;
  vm::LocalsRAII lraii(runtime_, &lv);

  vm::GCScope gcScope(runtime_);
  lv.buf = vm::JSArrayBuffer::create(
      runtime_,
      vm::Handle<vm::JSObject>::vmcast(&runtime_.arrayBufferPrototype));
  auto size = buffer->size();
  auto *data = buffer->data();
  vm::JSArrayBuffer::setExternalDataBlock(runtime_, lv.buf, data, size, buffer);
  return add<jsi::ArrayBuffer>(lv.buf.getHermesValue());
}

size_t HermesRuntimeImpl::size(const jsi::Array &arr) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  if (LLVM_LIKELY(vm::vmisa<vm::JSArray>(phv(arr)))) {
    return vm::JSArray::getLength(vm::vmcast<vm::JSArray>(phv(arr)), runtime_);
  }

  vm::GCScope gcScope(runtime_);
  struct : vm::Locals {
    vm::PinnedValue<> lenProp;
  } lv;
  vm::LocalsRAII lraii{runtime_, &lv};
  auto cr = vm::JSObject::getNamed_RJS(
      handle(arr),
      runtime_,
      vm::Predefined::getSymbolID(vm::Predefined::length));
  checkStatus(cr.getStatus());

  lv.lenProp = std::move(*cr);
  auto lenRes = toLength(runtime_, lv.lenProp);
  checkStatus(lenRes.getStatus());

  return lenRes->getNumber();
}

size_t HermesRuntimeImpl::size(const jsi::ArrayBuffer &arr) {
  vm::NoMutatorScope noMutatorScope{runtime_};
  auto ab = arrayBufferHandle(arr);
  if (LLVM_UNLIKELY(!ab->attached()))
    throw jsi::JSINativeException("ArrayBuffer is detached.");
  return ab->size();
}

uint8_t *HermesRuntimeImpl::data(const jsi::ArrayBuffer &arr) {
  vm::NoMutatorScope noMutatorScope{runtime_};
  auto ab = arrayBufferHandle(arr);
  if (LLVM_UNLIKELY(!ab->attached()))
    throw jsi::JSINativeException("ArrayBuffer is detached.");
  return ab->getDataBlock();
}

bool HermesRuntimeImpl::detached(const jsi::ArrayBuffer &arr) {
  return !arrayBufferHandle(arr)->attached();
}

jsi::Value HermesRuntimeImpl::getValueAtIndex(const jsi::Array &arr, size_t i) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  if (LLVM_UNLIKELY(i >= size(arr))) {
    throwJSErrorWithMessage(
        "getValueAtIndex: index ", i, " is out of bounds [0, ", size(arr), ")");
  }

  auto res = vm::getIndexed_RJS(runtime_, handle(arr), i);
  checkStatus(res.getStatus());

  return valueFromHermesValue(res->get());
}

void HermesRuntimeImpl::setValueAtIndexImpl(
    const jsi::Array &arr,
    size_t i,
    const jsi::Value &value) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  if (LLVM_UNLIKELY(i >= size(arr))) {
    throwJSErrorWithMessage(
        "setValueAtIndex: index ", i, " is out of bounds [0, ", size(arr), ")");
  }

  vm::PinnedHermesValue indexStorage =
      vm::HermesValue::encodeTrustedNumberValue(i);
  vm::PinnedHermesValue valueStorage;
  auto res = vm::JSObject::putComputed_RJS(
      handle(arr),
      runtime_,
      vm::Handle<>(&indexStorage),
      vmHandleFromValue(value, &valueStorage));
  checkStatus(res.getStatus());
}

size_t HermesRuntimeImpl::push(
    const jsi::Array &arr,
    const jsi::Value *elements,
    size_t count) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  size_t oldLen = arr.length(*this);
  auto newLen = oldLen + count;
  bool isJSArray = vm::vmisa<vm::JSArray>(phv(arr));

  // Fast-path: If it's a regular JSArray with fast index properties, and its
  // parents do not have any index properties, we can increase the JSArray size
  // directly and set the elements.
  if (LLVM_LIKELY(isJSArray)) {
    auto arrHandle = vm::Handle<vm::JSArray>::vmcast(&phv(arr));
    if (LLVM_LIKELY(oldLen < UINT32_MAX - count) &&
        arrayFastPathCheck(
            runtime_, arrHandle, runtime_.classJSArray, oldLen)) {
      auto increaseSizeRes =
          vm::JSArray::increaseStorageEndIndex(arrHandle, runtime_, newLen);
      checkStatus(increaseSizeRes);

      for (size_t i = 0; i < count; ++i) {
        auto shv = vm::SmallHermesValue::encodeHermesValue(
            hvFromValue(elements[i]), runtime_);
        vm::JSArray::unsafeSetExistingElementAt(
            *arrHandle, runtime_, oldLen + i, shv);
      }

      auto newLenShv =
          vm::SmallHermesValue::encodeNumberValue(newLen, runtime_);
      vm::JSArray::putLengthUnsafe(*arrHandle, runtime_, newLenShv);

      return newLen;
    }
  }

  // Slow-path: Set the property at [oldLen, oldLen + count - 1].
  vm::PinnedHermesValue numStorage;
  vm::PinnedHermesValue valStorage;
  for (size_t i = 0; i < count; i++) {
    numStorage = vm::HermesValue::encodeTrustedNumberValue(oldLen + i);

    auto res = vm::JSObject::putComputed_RJS(
        handle(arr),
        runtime_,
        vm::Handle<>(&numStorage),
        vmHandleFromValue(elements[i], &valStorage));
    checkStatus(res.getStatus());
  }

  // If it's not a JS Array, we need to update the 'length' property explicitly
  if (LLVM_UNLIKELY(!isJSArray)) {
    numStorage = vm::HermesValue::encodeTrustedNumberValue(newLen);
    auto res = vm::JSObject::putNamed_RJS(
        handle(arr),
        runtime_,
        vm::Predefined::getSymbolID(vm::Predefined::length),
        vm::Handle<>(&numStorage),
        vm::PropOpFlags().plusThrowOnError());
    checkStatus(res.getStatus());
  }

  return newLen;
}

jsi::Function HermesRuntimeImpl::createFunctionFromHostFunction(
    const jsi::PropNameID &name,
    unsigned int paramCount,
    jsi::HostFunctionType func) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  auto context = std::make_unique<HFContext>(std::move(func), *this);
  auto hostfunc =
      createFunctionFromHostFunction(context.get(), name, paramCount);
  context.release();
  return hostfunc;
}

template <typename ContextType>
jsi::Function HermesRuntimeImpl::createFunctionFromHostFunction(
    ContextType *context,
    const jsi::PropNameID &name,
    unsigned int paramCount) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  vm::SymbolID nameID = phv(name).getSymbol();
  auto funcRes = vm::FinalizableNativeFunction::createWithoutPrototype(
      runtime_,
      context,
      &ContextType::func,
      &ContextType::finalize,
      nameID,
      paramCount);
  checkStatus(funcRes.getStatus());
  jsi::Function ret = add<jsi::Function>(*funcRes);
  return ret;
}

jsi::HostFunctionType &HermesRuntimeImpl::getHostFunction(
    const jsi::Function &func) {
  vm::NoMutatorScope noMutatorScope{runtime_};
  return static_cast<HFContext *>(
             vm::vmcast<vm::FinalizableNativeFunction>(phv(func))->getContext())
      ->hostFunction;
}

jsi::Value HermesRuntimeImpl::call(
    const jsi::Function &func,
    const jsi::Value &jsThis,
    const jsi::Value *args,
    size_t count) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  vm::Handle<vm::Callable> handle =
      vm::Handle<vm::Callable>::vmcast(&phv(func));
  if (count > std::numeric_limits<uint32_t>::max()) {
    LOG_EXCEPTION_CAUSE(
        "HermesRuntimeImpl::call: Unable to call function: stack overflow");
    throw jsi::JSINativeException(
        "HermesRuntimeImpl::call: Unable to call function: stack overflow");
  }

  vm::ScopedNativeCallFrame newFrame{
      runtime_,
      static_cast<uint32_t>(count),
      handle.getHermesValue(),
      vm::HermesValue::encodeUndefinedValue(),
      hvFromValue(jsThis)};
  if (LLVM_UNLIKELY(newFrame.overflowed())) {
    checkStatus(runtime_.raiseStackOverflow(
        ::hermes::vm::Runtime::StackOverflowKind::NativeStack));
  }

  for (uint32_t i = 0; i != count; ++i) {
    newFrame->getArgRef(i) = hvFromValue(args[i]);
  }
  auto callRes = vm::Callable::call(handle, runtime_);
  checkStatus(callRes.getStatus());

  return valueFromHermesValue(callRes->get());
}

jsi::Value HermesRuntimeImpl::callAsConstructor(
    const jsi::Function &func,
    const jsi::Value *args,
    size_t count) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  struct : public vm::Locals {
    vm::PinnedValue<> thisArg;
  } lv;
  vm::LocalsRAII lraii(runtime_, &lv);

  vm::GCScope gcScope(runtime_);
  vm::Handle<vm::Callable> funcHandle =
      vm::Handle<vm::Callable>::vmcast(&phv(func));

  if (count > std::numeric_limits<uint32_t>::max()) {
    LOG_EXCEPTION_CAUSE(
        "HermesRuntimeImpl::call: Unable to call function: stack overflow");
    throw jsi::JSINativeException(
        "HermesRuntimeImpl::call: Unable to call function: stack overflow");
  }

  // We follow es5 13.2.2 [[Construct]] here. Below F == func.
  // 13.2.2.5:
  //    Let proto be the value of calling the [[Get]] internal property of
  //    F with argument "prototype"
  // 13.2.2.6:
  //    If Type(proto) is Object, set the [[Prototype]] internal property
  //    of obj to proto
  // 13.2.2.7:
  //    If Type(proto) is not Object, set the [[Prototype]] internal property
  //    of obj to the standard built-in Object prototype object as described
  //    in 15.2.4
  //
  // Note that 13.2.2.1-4 are also handled by the call to newObject.
  auto thisArgRes = vm::Callable::createThisForConstruct_RJS(
      funcHandle, runtime_, funcHandle);
  checkStatus(thisArgRes.getStatus());
  // We need to capture this in case the ctor doesn't return an object,
  // we need to return this object.
  lv.thisArg = std::move(*thisArgRes);

  // 13.2.2.8:
  //    Let result be the result of calling the [[Call]] internal property of
  //    F, providing obj as the this value and providing the argument list
  //    passed into [[Construct]] as args.
  //
  // For us result == res.

  vm::ScopedNativeCallFrame newFrame{
      runtime_,
      static_cast<uint32_t>(count),
      funcHandle.getHermesValue(),
      funcHandle.getHermesValue(),
      lv.thisArg.getHermesValue()};
  if (newFrame.overflowed()) {
    checkStatus(runtime_.raiseStackOverflow(
        ::hermes::vm::Runtime::StackOverflowKind::NativeStack));
  }
  for (uint32_t i = 0; i != count; ++i) {
    newFrame->getArgRef(i) = hvFromValue(args[i]);
  }
  // The last parameter indicates that this call should construct an object.
  auto callRes = vm::Callable::call(funcHandle, runtime_);
  checkStatus(callRes.getStatus());

  // 13.2.2.9:
  //    If Type(result) is Object then return result
  // 13.2.2.10:
  //    Return obj
  auto resultValue = callRes->get();
  vm::HermesValue resultHValue =
      resultValue.isObject() ? resultValue : lv.thisArg.getHermesValue();
  return valueFromHermesValue(resultHValue);
}

void HermesRuntimeImpl::setRuntimeDataImpl(
    const jsi::UUID &uuid,
    const void *data,
    void (*deleter)(const void *data)) {
  if (auto it = dataMap_.find(uuid); it != dataMap_.end()) {
    // Free the data currently keyed at UUID
    auto oldData = it->second.first;
    auto oldDataDeleter = it->second.second;
    oldDataDeleter(oldData);
  }
  dataMap_[uuid] = {data, deleter};
}

const void *HermesRuntimeImpl::getRuntimeDataImpl(const jsi::UUID &uuid) {
  auto entry = dataMap_.lookup(uuid);
  return entry.first;
}

std::shared_ptr<jsi::MutableBuffer> HermesRuntimeImpl::tryGetMutableBuffer(
    const jsi::ArrayBuffer &arrayBuffer) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  auto abHandle = arrayBufferHandle(arrayBuffer);
  if (LLVM_UNLIKELY(!abHandle->attached())) {
    return nullptr;
  }
  if (LLVM_UNLIKELY(!abHandle->external())) {
    return nullptr;
  }

  // External ArrayBuffers are created using the `createArrayBuffer` API with a
  // shared pointer of a MutableBuffer. We pass in that shared pointer as the
  // context when creating a JSArrayBuffer, thus we can assume the type of the
  // context here.
  return std::static_pointer_cast<jsi::MutableBuffer>(
      vm::JSArrayBuffer::getExternalDataContext(runtime_, abHandle));
}

jsi::Uint8Array HermesRuntimeImpl::createUint8Array(size_t length) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  auto result =
      vm::JSTypedArray<uint8_t, vm::CellKind::Uint8ArrayKind>::allocate(
          runtime_, length);
  checkStatus(result.getStatus());
  return add<jsi::Uint8Array>(result->getHermesValue());
}

jsi::Uint8Array HermesRuntimeImpl::createUint8Array(
    const jsi::ArrayBuffer &buffer,
    size_t offset,
    size_t length) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  auto abHandle = arrayBufferHandle(buffer);

  auto typedArrayRes =
      vm::JSTypedArray<uint8_t, vm::CellKind::Uint8ArrayKind>::allocate(
          runtime_, length);
  checkStatus(typedArrayRes.getStatus());

  vm::JSTypedArrayBase::setBuffer(
      runtime_,
      typedArrayRes->get(),
      *abHandle,
      offset,
      length,
      sizeof(uint8_t));

  return add<jsi::Uint8Array>(typedArrayRes->getHermesValue());
}

jsi::ArrayBuffer HermesRuntimeImpl::buffer(const jsi::TypedArray &typedArray) {
  vm::NoMutatorScope noMutatorScope{runtime_};
  vm::GCScope gcScope(runtime_);
  auto taHandle = typedArrayHandle(typedArray);
  return add<jsi::ArrayBuffer>(
      vm::HermesValue::encodeObjectValue(taHandle->getBuffer(runtime_)));
}

size_t HermesRuntimeImpl::byteOffset(const jsi::TypedArray &typedArray) {
  vm::NoMutatorScope noMutatorScope{runtime_};
  auto taHandle = typedArrayHandle(typedArray);
  if (LLVM_UNLIKELY(!taHandle->attached(runtime_)))
    return 0;
  return taHandle->getByteOffset();
}

size_t HermesRuntimeImpl::byteLength(const jsi::TypedArray &typedArray) {
  vm::NoMutatorScope noMutatorScope{runtime_};
  auto taHandle = typedArrayHandle(typedArray);
  if (LLVM_UNLIKELY(!taHandle->attached(runtime_)))
    return 0;
  return taHandle->getByteLength();
}

size_t HermesRuntimeImpl::length(const jsi::TypedArray &typedArray) {
  vm::NoMutatorScope noMutatorScope{runtime_};
  auto taHandle = typedArrayHandle(typedArray);
  if (LLVM_UNLIKELY(!taHandle->attached(runtime_)))
    return 0;
  return taHandle->getLength();
}

bool HermesRuntimeImpl::isTypedArray(const jsi::Object &obj) const {
  return vm::vmisa<vm::JSTypedArrayBase>(phv(obj));
}

bool HermesRuntimeImpl::isUint8Array(const jsi::Object &obj) const {
  return vm::vmisa<vm::JSTypedArray<uint8_t, vm::CellKind::Uint8ArrayKind>>(
      phv(obj));
}

jsi::Value HermesRuntimeImpl::createError(const jsi::String &msg) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  struct : public vm::Locals {
    vm::PinnedValue<vm::JSError> errorObj;
  } lv;
  vm::LocalsRAII lraii(runtime_, &lv);
  vm::Handle<vm::JSObject> prototype =
      vm::Handle<vm::JSObject>::vmcast(&runtime_.ErrorPrototype);
  lv.errorObj = vm::JSError::create(runtime_, prototype);
  vm::JSError::recordStackTrace(lv.errorObj, runtime_);
  vm::JSError::setMessage(lv.errorObj, runtime_, stringHandle(msg));
  return valueFromHermesValue(lv.errorObj.getHermesValue());
}

jsi::Value HermesRuntimeImpl::createEvalError(const jsi::String &msg) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  struct : public vm::Locals {
    vm::PinnedValue<vm::JSError> errorObj;
  } lv;
  vm::LocalsRAII lraii(runtime_, &lv);
  vm::Handle<vm::JSObject> prototype =
      vm::Handle<vm::JSObject>::vmcast(&runtime_.EvalErrorPrototype);
  lv.errorObj = vm::JSError::create(runtime_, prototype);
  vm::JSError::recordStackTrace(lv.errorObj, runtime_);
  vm::JSError::setMessage(lv.errorObj, runtime_, stringHandle(msg));
  return valueFromHermesValue(lv.errorObj.getHermesValue());
}

jsi::Value HermesRuntimeImpl::createRangeError(const jsi::String &msg) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  struct : public vm::Locals {
    vm::PinnedValue<vm::JSError> errorObj;
  } lv;
  vm::LocalsRAII lraii(runtime_, &lv);
  vm::Handle<vm::JSObject> prototype =
      vm::Handle<vm::JSObject>::vmcast(&runtime_.RangeErrorPrototype);
  lv.errorObj = vm::JSError::create(runtime_, prototype);
  vm::JSError::recordStackTrace(lv.errorObj, runtime_);
  vm::JSError::setMessage(lv.errorObj, runtime_, stringHandle(msg));
  return valueFromHermesValue(lv.errorObj.getHermesValue());
}

jsi::Value HermesRuntimeImpl::createReferenceError(const jsi::String &msg) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  struct : public vm::Locals {
    vm::PinnedValue<vm::JSError> errorObj;
  } lv;
  vm::LocalsRAII lraii(runtime_, &lv);
  vm::Handle<vm::JSObject> prototype =
      vm::Handle<vm::JSObject>::vmcast(&runtime_.ReferenceErrorPrototype);
  lv.errorObj = vm::JSError::create(runtime_, prototype);
  vm::JSError::recordStackTrace(lv.errorObj, runtime_);
  vm::JSError::setMessage(lv.errorObj, runtime_, stringHandle(msg));
  return valueFromHermesValue(lv.errorObj.getHermesValue());
}

jsi::Value HermesRuntimeImpl::createSyntaxError(const jsi::String &msg) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  struct : public vm::Locals {
    vm::PinnedValue<vm::JSError> errorObj;
  } lv;
  vm::LocalsRAII lraii(runtime_, &lv);
  vm::Handle<vm::JSObject> prototype =
      vm::Handle<vm::JSObject>::vmcast(&runtime_.SyntaxErrorPrototype);
  lv.errorObj = vm::JSError::create(runtime_, prototype);
  vm::JSError::recordStackTrace(lv.errorObj, runtime_);
  vm::JSError::setMessage(lv.errorObj, runtime_, stringHandle(msg));
  return valueFromHermesValue(lv.errorObj.getHermesValue());
}

jsi::Value HermesRuntimeImpl::createTypeError(const jsi::String &msg) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  struct : public vm::Locals {
    vm::PinnedValue<vm::JSError> errorObj;
  } lv;
  vm::LocalsRAII lraii(runtime_, &lv);
  vm::Handle<vm::JSObject> prototype =
      vm::Handle<vm::JSObject>::vmcast(&runtime_.TypeErrorPrototype);
  lv.errorObj = vm::JSError::create(runtime_, prototype);
  vm::JSError::recordStackTrace(lv.errorObj, runtime_);
  vm::JSError::setMessage(lv.errorObj, runtime_, stringHandle(msg));
  return valueFromHermesValue(lv.errorObj.getHermesValue());
}

jsi::Value HermesRuntimeImpl::createURIError(const jsi::String &msg) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  struct : public vm::Locals {
    vm::PinnedValue<vm::JSError> errorObj;
  } lv;
  vm::LocalsRAII lraii(runtime_, &lv);
  vm::Handle<vm::JSObject> prototype =
      vm::Handle<vm::JSObject>::vmcast(&runtime_.URIErrorPrototype);
  lv.errorObj = vm::JSError::create(runtime_, prototype);
  vm::JSError::recordStackTrace(lv.errorObj, runtime_);
  vm::JSError::setMessage(lv.errorObj, runtime_, stringHandle(msg));
  return valueFromHermesValue(lv.errorObj.getHermesValue());
}

bool HermesRuntimeImpl::strictEquals(const jsi::Symbol &a, const jsi::Symbol &b)
    const {
  return phv(a).getSymbol() == phv(b).getSymbol();
}

bool HermesRuntimeImpl::strictEquals(const jsi::BigInt &a, const jsi::BigInt &b)
    const {
  return phv(a).getBigInt()->compare(phv(b).getBigInt()) == 0;
}

bool HermesRuntimeImpl::strictEquals(const jsi::String &a, const jsi::String &b)
    const {
  return phv(a).getString()->equals(phv(b).getString());
}

bool HermesRuntimeImpl::strictEquals(const jsi::Object &a, const jsi::Object &b)
    const {
  return phv(a).getRaw() == phv(b).getRaw();
}

bool HermesRuntimeImpl::instanceOf(
    const jsi::Object &o,
    const jsi::Function &f) {
  ExecutionScopeRAII scopeRAII(mutatorScope);
  vm::GCScope gcScope(runtime_);
  auto result = vm::instanceOfOperator_RJS(runtime_, handle(o), handle(f));
  checkStatus(result.getStatus());
  return *result;
}

jsi::Runtime::ScopeState *HermesRuntimeImpl::pushScope() {
  return nullptr;
}

void HermesRuntimeImpl::popScope(ScopeState *prv) {
  assert(!prv && "pushScope only returns nullptrs");
}

void HermesRuntimeImpl::checkStatus(vm::ExecutionStatus status) {
  if (LLVM_LIKELY(status != vm::ExecutionStatus::EXCEPTION)) {
    return;
  }

  throwPendingError();
}

vm::HermesValue HermesRuntimeImpl::stringHVFromAscii(
    const char *str,
    size_t length) {
  auto strRes = vm::StringPrimitive::createEfficient(
      runtime_, llvh::makeArrayRef(str, length));
  checkStatus(strRes.getStatus());
  return *strRes;
}

vm::HermesValue HermesRuntimeImpl::stringHVFromUtf8(
    const uint8_t *utf8,
    size_t length) {
  const bool IgnoreInputErrors = true;
  auto strRes = vm::StringPrimitive::createEfficient(
      runtime_, llvh::makeArrayRef(utf8, length), IgnoreInputErrors);
  checkStatus(strRes.getStatus());
  return *strRes;
}

vm::HermesValue HermesRuntimeImpl::stringHVFromUtf16(
    const char16_t *utf16,
    size_t length) {
  auto strRes = vm::StringPrimitive::createEfficient(
      runtime_, llvh::makeArrayRef(utf16, length));
  checkStatus(strRes.getStatus());
  return *strRes;
}

void HermesRuntimeImpl::throwPendingError() {
  struct : public vm::Locals {
    vm::PinnedValue<vm::StringPrimitive> propHandle;
  } lv;
  vm::LocalsRAII lraii(runtime_, &lv);

  vm::GCScope scope{runtime_};

  // Retrieve the exception value and clear as we will rethrow it as a C++
  // exception.
  auto hv = runtime_.getThrownValue();
  runtime_.clearThrownValue();
  auto jsiVal = valueFromHermesValue(hv);
  vm::PinnedHermesValue valueStorage;
  auto hnd = vmHandleFromValue(jsiVal, &valueStorage);

  std::string msg = "No message";
  std::string stack = "No stack";
  if (auto str = vm::Handle<vm::StringPrimitive>::dyn_vmcast(hnd)) {
    // If the exception is a string, use it as the message.
    msg = utf8FromStringView(
        vm::StringPrimitive::createStringView(runtime_, str));
  } else if (auto obj = vm::Handle<vm::JSObject>::dyn_vmcast(hnd)) {
    // If the exception is an object try to retrieve its message and stack
    // properties.

    /// Attempt to retrieve a string property \p sym from \c obj and store it
    /// in \p out. Ignore any catchable errors and non-string properties.
    auto getStrProp = [this, obj, &lv](vm::SymbolID sym, std::string &out) {
      auto propRes = vm::JSObject::getNamed_RJS(obj, runtime_, sym);
      if (LLVM_UNLIKELY(propRes == vm::ExecutionStatus::EXCEPTION)) {
        // An exception was thrown while retrieving the property, if it is
        // catchable, suppress it. Otherwise, rethrow this exception without
        // trying to invoke any more JavaScript.
        auto propExHv = runtime_.getThrownValue();
        runtime_.clearThrownValue();

        if (!vm::isUncatchableError(propExHv))
          return;

        // An uncatchable error occurred, it is unsafe to do anything that might
        // execute more JavaScript.
        throw jsi::JSError(
            valueFromHermesValue(propExHv),
            "Uncatchable exception thrown while creating error",
            "No stack");
      }

      // If the property is a string, update out. Otherwise ignore it.
      auto prop = propRes->get();
      if (prop.isString()) {
        lv.propHandle = prop.getString();
        auto view =
            vm::StringPrimitive::createStringView(runtime_, lv.propHandle);
        out = utf8FromStringView(view);
      }
    };

    getStrProp(vm::Predefined::getSymbolID(vm::Predefined::message), msg);
    getStrProp(vm::Predefined::getSymbolID(vm::Predefined::stack), stack);
  }

  // Use the constructor of jsi::JSError that cannot run additional
  // JS, since that may then result in additional exceptions and infinite
  // recursion.
  throw jsi::JSError(std::move(jsiVal), msg, stack);
}

template <typename... Args>
void HermesRuntimeImpl::throwJSErrorWithMessage(Args &&...args) {
  // TODO: Add support for size_t in TwineChar16 and directly construct that
  //       instead of using a stream.
  std::string s;
  llvh::raw_string_ostream os(s);
  raw_ostream_append(os, std::forward<Args>(args)...);
  LOG_EXCEPTION_CAUSE("JSError: %s", os.str().c_str());
  // Raise an error with this message in the Runtime and rethrow it with
  // throwPendingError.
  (void)runtime_.raiseError(vm::TwineChar16(s));
  throwPendingError();
}

namespace {

class HermesMutex : public std::recursive_mutex {
 public:
  // ThreadSafeRuntimeImpl expects that the lock ctor takes a
  // reference to the Runtime.  Otherwise, this is a
  // std::recursive_mutex.
  HermesMutex(HermesRuntimeImpl &) {}
};

} // namespace

vm::RuntimeConfig hardenedHermesRuntimeConfig() {
  vm::RuntimeConfig::Builder config;
  // Disable optional JS features.
  config.withEnableEval(false);
  config.withES6Proxy(false);
  config.withEnableHermesInternal(false);
  config.withEnableHermesInternalTestMethods(false);

  // Enabled hardening options.
  config.withRandomizeMemoryLayout(true);
  return config.build();
}

jsi::ICast *makeHermesRootAPI() {
  static HermesRootAPI api;
  return (IHermesRootAPI *)&api;
}

std::unique_ptr<HermesRuntime> makeHermesRuntime(
    const vm::RuntimeConfig &runtimeConfig) {
  auto *api = jsi::castInterface<IHermesRootAPI>(makeHermesRootAPI());
  return api->makeHermesRuntime(runtimeConfig);
}

std::unique_ptr<HermesRuntime> makeHermesRuntimeNoThrow(
    const vm::RuntimeConfig &runtimeConfig) noexcept {
  try {
    return makeHermesRuntime(runtimeConfig);
  } catch (...) {
    return nullptr;
  }
}

std::unique_ptr<jsi::ThreadSafeRuntime> makeThreadSafeHermesRuntime(
    const vm::RuntimeConfig &runtimeConfig) {
#if defined(HERMESVM_PLATFORM_LOGGING)
  const vm::RuntimeConfig &actualRuntimeConfig =
      runtimeConfig.rebuild()
          .withGCConfig(runtimeConfig.getGCConfig()
                            .rebuild()
                            .withShouldRecordStats(true)
                            .build())
          .build();
#else
  const vm::RuntimeConfig &actualRuntimeConfig = runtimeConfig;
#endif

  auto ret = std::make_unique<
      jsi::detail::ThreadSafeRuntimeImpl<HermesRuntimeImpl, HermesMutex>>(
      actualRuntimeConfig);

  auto &hermesRt = ret->getUnsafeRuntime();
#ifdef HERMES_ENABLE_DEBUGGER
  // Only HermesRuntime can create a debugger instance.  This requires
  // the setter and not using make_unique, so the call to new is here
  // in this function, which is a friend of debugger::Debugger.
  hermesRt.setDebugger(
      std::unique_ptr<debugger::Debugger>(
          new debugger::Debugger(&hermesRt, hermesRt.runtime_)));
#else
  hermesRt.setDebugger(std::make_unique<debugger::Debugger>());
#endif

#if HERMES_ENABLE_CORE_EXTENSIONS
  loadAndInstallExtensions(hermesRt);
#endif

  return ret;
}

#ifdef HERMES_ENABLE_DEBUGGER
/// Glue code enabling the Debugger to produce a jsi::Value from a HermesValue.
jsi::Value debugger::Debugger::jsiValueFromHermesValue(vm::HermesValue hv) {
  return static_cast<HermesRuntimeImpl *>(runtime_)->valueFromHermesValue(hv);
}
#endif

} // namespace hermes
} // namespace facebook

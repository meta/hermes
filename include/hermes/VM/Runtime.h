/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_RUNTIME_H
#define HERMES_VM_RUNTIME_H

#include "hermes/ADT/TransparentOwningPtr.h"
#include "hermes/FrontEndDefs/Builtins.h"
#include "hermes/Public/DebuggerTypes.h"
#include "hermes/Public/RuntimeConfig.h"
#include "hermes/Support/Compiler.h"
#include "hermes/Support/ErrorHandling.h"
#include "hermes/Support/StackExecutor.h"
#include "hermes/Support/StackOverflowGuard.h"
#include "hermes/VM/AllocOptions.h"
#include "hermes/VM/AllocResult.h"
#include "hermes/VM/BasicBlockExecutionInfo.h"
#include "hermes/VM/CallResult.h"
#include "hermes/VM/Casting.h"
#include "hermes/VM/Debugger/Debugger.h"
#include "hermes/VM/GC.h"
#include "hermes/VM/GCBase-inline.h"
#include "hermes/VM/Handle-inline.h"
#include "hermes/VM/HandleRootOwner-inline.h"
#include "hermes/VM/IdentifierTable.h"
#include "hermes/VM/InternalProperty.h"
#include "hermes/VM/InterpreterState.h"
#include "hermes/VM/JIT/JIT.h"
#include "hermes/VM/Predefined.h"
#include "hermes/VM/Profiler.h"
#include "hermes/VM/Profiler/SamplingProfilerDefs.h"
#include "hermes/VM/PropertyCache.h"
#include "hermes/VM/PropertyDescriptor.h"
#include "hermes/VM/RegExpMatch.h"
#include "hermes/VM/RuntimeModule.h"
#include "hermes/VM/StackFrame.h"
#include "hermes/VM/StackTracesTree-NoRuntime.h"
#include "hermes/VM/SymbolRegistry.h"
#include "hermes/VM/TimeLimitMonitor.h"
#include "hermes/VM/TwineChar16.h"
#include "hermes/VM/VMExperiments.h"

#include "llvh/ADT/DenseMap.h"
#include "llvh/ADT/SmallVector.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <type_traits>
#include <vector>

typedef struct SHJmpBuf SHJmpBuf;
typedef struct SHLocals SHLocals;
typedef struct SHUnit SHUnit;

#ifdef __EMSCRIPTEN__
/// In Emscripten builds, allow the integrator to provide a callback to
/// synchronously notify the VM that a timeout has occurred. This should be
/// checked whenever we check for timeouts.
extern "C" bool test_wasm_host_timeout();
extern "C" bool test_and_clear_wasm_host_timeout();
#endif

namespace hermes {
// Forward declaration.
class JSONEmitter;

namespace inst {
struct Inst;
enum class OpCode : uint8_t;
} // namespace inst

namespace hbc {
class BytecodeModule;
struct CompileFlags;
} // namespace hbc

namespace regex {
struct CapturedRange;
} // namespace regex

namespace vm {

// External forward declarations.
class CodeBlock;
class Environment;
class Interpreter;
class JSObject;
class PropertyAccessor;
struct JSLibStorage;
struct RuntimeOffsets;
class ScopedNativeDepthReducer;
class ScopedNativeDepthTracker;
class ScopedNativeCallFrame;
class CodeCoverageProfiler;
struct StackTracesTree;
struct Locals;
template <CellKind C>
class JSMapImpl;
struct SerializationManagedValue;
class JSFinalizationRegistry;

#if HERMESVM_SAMPLING_PROFILER_AVAILABLE
class SamplingProfiler;
#endif // HERMESVM_SAMPLING_PROFILER_AVAILABLE

#ifdef HERMESVM_PROFILER_BB
class JSArray;
#endif

/// Wrap \p code in a source Buffer suitable for compiling with \p compileFlags.
/// When the resulting module will retain its source buffer -- i.e. lazy or
/// full-debug-info compilation, matching keepCompilationData in
/// createBCProviderFromSrc -- the bytes are copied so the buffer owns them.
/// Otherwise they are borrowed from \p code, which is safe because the buffer
/// is dropped once compilation finishes. Copying in the retained case avoids a
/// use-after-free when \p code does not outlive the module (e.g. a transient
/// eval or debugger command string).
std::unique_ptr<Buffer> makeCompilationSourceBuffer(
    llvh::StringRef code,
    const hbc::CompileFlags &compileFlags);

/// Number of stack words after the top of frame that we always ensure are
/// available. This is necessary so we can perform native calls with small
/// number of arguments without checking.
static const unsigned STACK_RESERVE = SH_STACK_RESERVE;

/// Type used to assign object unique integer identifiers.
using ObjectID = uint32_t;

#define PROP_CACHE_IDS(V) V(RegExpLastIndex, Predefined::lastIndex)

/// Fixed set of ids used by the property cache in Runtime.
enum class PropCacheID {
#define V(id, predef) id,
  PROP_CACHE_IDS(V)
#undef V
      _COUNT
};

/// Trace of the last few instructions for debugging crashes.
class CrashTraceImpl {
  /// Record of the last executed instruction.
  struct Record {
    /// Offset from start of bytecode file.
    uint32_t ipOffset;
    /// Opcode of last executed instruction.
    inst::OpCode opCode;
  };

 public:
  /// Record the currently executing module, replacing the info of the
  /// previous one.
  inline void recordModule(
      uint32_t segmentID,
      llvh::StringRef sourceURL,
      llvh::StringRef sourceHash);

  /// Add a record to the circular trace buffer.
  void recordInst(uint32_t ipOffset, inst::OpCode opCode) {
    static_assert(
        kTraceLength && (kTraceLength & (kTraceLength - 1)) == 0,
        "kTraceLength must be a power of 2");
    unsigned n = (last_ + 1) & (kTraceLength - 1);
    last_ = n;
    Record *p = trace_ + n;
    p->ipOffset = ipOffset;
    p->opCode = opCode;
  }

 private:
  /// Size of the circular buffer.
  static constexpr unsigned kTraceLength = 8;
  /// The index of the last entry written to the buffer.
  unsigned last_ = kTraceLength - 1;
  /// A circular buffer of Record.
  Record trace_[kTraceLength] = {};

  /// Current segmentID.
  uint32_t segmentID_ = 0;
  /// Source URL, truncated, zero terminated.
  char sourceURL_[16] = {};
  /// SHA1 source hash.
  uint8_t sourceHash_[20] = {};
};

inline void CrashTraceImpl::recordModule(
    uint32_t segmentID,
    llvh::StringRef sourceURL,
    llvh::StringRef sourceHash) {
  segmentID_ = segmentID;
  size_t len = std::min(sizeof(sourceURL_) - 1, sourceURL.size());
  ::memcpy(sourceURL_, sourceURL.data(), len);
  sourceURL_[len] = 0;
  ::memcpy(
      sourceHash_,
      sourceHash.data(),
      std::min(sizeof(sourceHash_), sourceHash.size()));
}

class CrashTraceNoop {
 public:
  void recordModule(
      uint32_t segmentID,
      llvh::StringRef sourceURL,
      llvh::StringRef sourceHash) {}

  /// Add a record to the circular trace buffer.
  void recordInst(uint32_t ipOffset, inst::OpCode opCode) {}
};

#ifndef HERMESVM_CRASH_TRACE
// Make sure it is 0 or 1 so it can be checked in C++.
#define HERMESVM_CRASH_TRACE 0
#endif

#if HERMESVM_CRASH_TRACE
using CrashTrace = CrashTraceImpl;
#else
using CrashTrace = CrashTraceNoop;
#endif

/// Wrap SHRuntime in a C++ class, which we can use to implement C++
/// functionality that should logically live in SHRuntime. For example, this
/// ensures SHRuntime's fields are initialized before anything else in Runtime.
class RuntimeBase : public SHRuntime {
 protected:
  RuntimeBase();

 public:
  /// See documentation on \c GCBase::GCCallbacks.
  void registerHeapSegment(unsigned idx, void *lowLim);
};

/// The Runtime encapsulates the entire context of a VM. Multiple instances can
/// exist and are completely independent from each other.
class Runtime : public RuntimeBase, public HandleRootOwner {
 public:
  static std::shared_ptr<Runtime> create(const RuntimeConfig &runtimeConfig);

  ~Runtime();

  /// Add a custom function that will be executed at the start of every garbage
  /// collection to mark additional GC roots that may not be known to the
  /// Runtime.
  void addCustomRootsFunction(
      std::function<void(GC *, RootAcceptor &)> markRootsFn);

  /// Register a callback invoked once at the start of drainJobs(),
  /// before any jobs are executed. Used by embedders (e.g., NAPI) to
  /// perform housekeeping such as draining deferred finalizers.
  void addDrainJobsCallback(std::function<void()> callback);

  /// Register a callback invoked from ~Runtime() BEFORE
  /// getHeap().finalizeAll() runs. At this point the heap is still
  /// fully functional — callbacks may allocate, call into JS, and
  /// trigger GC. Used by embedders (e.g., NAPI) to drive substantive
  /// teardown of their state (running cleanup hooks, finalizing
  /// persistent references, etc.) while a valid JS execution context
  /// is still available. Callbacks run in registration order.
  void addShutdownCallback(std::function<void()> callback);

  /// Register a deleter invoked from ~Runtime() AFTER
  /// getHeap().finalizeAll() returns. At this point the heap is
  /// quiesced — do NOT allocate, call into JS, or trigger GC. Used
  /// to free embedder objects that needed to outlive finalizeAll
  /// (e.g., a napi_env whose NativeState finalizers were called by
  /// finalizeAll). Deleters run in registration order.
  void addPostShutdownDeleter(std::function<void()> deleter);

  /// Add a custom function that will be executed sometime during garbage
  /// collection to mark additional weak GC roots that may not be known to the
  /// Runtime.
  void addCustomWeakRootsFunction(
      std::function<void(GC *, WeakRootAcceptor &)> markRootsFn);

  /// Add a custom function that will be executed when a heap snapshot is taken,
  /// to add any extra nodes.
  /// \param nodes Use snap.beginNode() and snap.endNode() to create nodes in
  ///   snapshot graph.
  /// \param edges Use snap.addNamedEdge() or snap.addIndexedEdge() to create
  ///   edges to the nodes defined in \p nodes.
  void addCustomSnapshotFunction(
      std::function<void(HeapSnapshot &)> nodes,
      std::function<void(HeapSnapshot &)> edges);

  /// Runs the given UTF-8 \p code in a new RuntimeModule as top-level code.
  /// Note that if compileFlags.lazy is set, the code string will be copied.
  /// \param sourceURL the location of the source that's being run.
  /// \param compileFlags Flags controlling compilation.
  /// \return the status of the execution.
  CallResult<HermesValue> run(
      llvh::StringRef code,
      llvh::StringRef sourceURL,
      const hbc::CompileFlags &compileFlags);

  /// Runs the given UTF-8 \p code in a new RuntimeModule as top-level code.
  /// \param sourceURL the location of the source that's being run.
  /// \param compileFlags Flags controlling compilation.
  /// \return the status of the execution.
  CallResult<HermesValue> run(
      std::unique_ptr<Buffer> code,
      llvh::StringRef sourceURL,
      const hbc::CompileFlags &compileFlags);

  /// Runs the given \p bytecode with the given \p runtimeModuleFlags. The \p
  /// sourceURL, if not empty, is reported as the file name in backtraces. If \p
  /// environment is not null, set it as the environment associated with the
  /// initial JSFunction, which enables local eval. \p thisArg the "this"
  /// argument to use initially. \p newTarget the "new.target" to use initially.
  CallResult<HermesValue> runBytecode(
      std::shared_ptr<hbc::BCProvider> &&bytecode,
      RuntimeModuleFlags runtimeModuleFlags,
      llvh::StringRef sourceURL,
      Handle<Environment> environment,
      Handle<> thisArg,
      Handle<> newTarget = Runtime::getUndefinedValue());

  /// Runs the given \p bytecode. If \p environment is not null, set it as the
  /// environment associated with the initial JSFunction, which enables local
  /// eval.
  /// Uses global_ as the "this" value initially.
  /// \p isPersistent indicates whether the created runtime module should
  /// persist in memory.
  CallResult<HermesValue> runBytecode(
      std::shared_ptr<hbc::BCProvider> &&bytecode,
      RuntimeModuleFlags runtimeModuleFlags,
      llvh::StringRef sourceURL,
      Handle<Environment> environment) {
    getHeap().runtimeWillExecute();
    return runBytecode(
        std::move(bytecode),
        runtimeModuleFlags,
        sourceURL,
        environment,
        Handle<>(toPHV(&global_)));
  }

  ExecutionStatus loadSegment(
      std::shared_ptr<hbc::BCProvider> &&bytecode,
      Handle<RequireContext> requireContext,
      RuntimeModuleFlags flags = {});

  /// Runs the internal JavaScript. This is called once during initialization.
  /// \return the completion value of internal bytecode IIFE.
  Handle<JSObject> runInternalJavaScript();

  /// A convenience function to print an exception to a stream.
  void printException(llvh::raw_ostream &os, Handle<> valueHandle);

  /// @name Heap management
  /// @{

  /// Create a fixed size object of type T.
  /// If necessary perform a GC cycle, which may potentially move
  /// allocated objects.
  /// \return a pointer to the newly created object in the GC heap.
  template <
      typename T,
      HasFinalizer hasFinalizer = HasFinalizer::No,
      LongLived longLived = LongLived::No,
      class... Args>
  T *makeAFixed(Args &&...args);

  /// Create a variable size object of type T and size \p size.
  /// If necessary perform a GC cycle, which may potentially move
  /// allocated objects.
  /// \return a pointer to the newly created object in the GC heap.
  template <
      typename T,
      HasFinalizer hasFinalizer = HasFinalizer::No,
      LongLived longLived = LongLived::No,
      CanBeLarge canBeLarge = CanBeLarge::No,
      MayFail mayFail = MayFail::No,
      class... Args>
  T *makeAVariable(uint32_t size, Args &&...args);

  /// Allocate two young gen objects of the size \p size1 + \p size2.
  /// Calls the constructors of types T1 and T2 with the arguments \p t1Args and
  /// \p t2Args respectively.
  /// \pre the TOTAL size must be able to fit in the young gen
  ///  (can be checked with GC::canAllocateInYoungGen(size)).
  /// \post both result pointers are in the young gen.
  /// \return a pointer to size1 size T1, and a pointer to size2 size T2.
  template <typename T1, typename T2, typename... T1Args, typename... T2Args>
  inline std::pair<T1 *, T2 *> make2YoungGenUnsafe(
      uint32_t size1,
      std::tuple<T1Args...> t1Args,
      uint32_t size2,
      std::tuple<T2Args...> t2Args);

  /// Used as a placeholder for places where we should be checking for OOM
  /// but aren't yet.
  /// TODO: do something when there is an uncaught exception, e.g. print
  /// stack traces.
  template <typename T>
  T ignoreAllocationFailure(CallResult<T> res);

  /// Used as a placeholder for places where we should be checking for OOM
  /// but aren't yet.
  void ignoreAllocationFailure(ExecutionStatus status);

  // Inform the VM that TTI has been reached.  (In case, for example, the
  // runtime should change its behavior at that point.)
  void ttiReached();

  /// Force a garbage collection cycle.
  void collect(std::string cause) {
    getHeap().collect(std::move(cause));
  }

  /// Request garbage collection with specific options.
  void collectWithOptions(
      std::string &&cause,
      const GCBase::GCOptions &options) {
    getHeap().collectWithOptions(std::move(cause), options);
  }

  /// Potentially move the heap if handle sanitization is on.
  void potentiallyMoveHeap();

  using HandleRootOwner::makeHandle;
  using HandleRootOwner::makeMutableHandle;

  /// Convenience function to create a Handle from a GCPointer.
  template <class T>
  inline Handle<T> makeHandle(const GCPointer<T> &p);

  /// Convenience function to create a MutableHandle from a GCPointer.
  template <class T>
  MutableHandle<T> makeMutableHandle(const GCPointer<T> &p);

  /// \return the \c StringPrimitive of a predefined string.
  StringPrimitive *getPredefinedString(Predefined::Str predefined);
  StringPrimitive *getPredefinedString(Predefined::Sym predefined);

  /// \return a \c Handle<StringPrimitive> to a predefined string.
  Handle<StringPrimitive> getPredefinedStringHandle(Predefined::Str predefined);
  Handle<StringPrimitive> getPredefinedStringHandle(Predefined::Sym predefined);

  /// \return the \c StringPrimitive given a symbol ID \p id.
  inline StringPrimitive *getStringPrimFromSymbolID(SymbolID id);

  /// \return true if a symbol specified by \p id has the same string content
  /// as a given string primitive \p strPrim.
  bool symbolEqualsToStringPrim(SymbolID id, StringPrimitive *strPrim);

  /// A wrapper to facilitate printing the name of a SymbolID to a stream.
  struct FormatSymbolID {
    Runtime &runtime;
    SymbolID const symbolID;

    FormatSymbolID(Runtime &runtime, SymbolID symbolID)
        : runtime(runtime), symbolID(symbolID) {}
  };

  /// Create an object that when serialized to a stream will print the contents
  /// of a SymbolID.
  FormatSymbolID formatSymbolID(SymbolID id);

  GC &getHeap() {
    return heap_;
  }

  /// @}

  /// Return a pointer to a callable builtin identified by id.
  /// Unfortunately we can't use the enum here, since we don't want to include
  /// the builtins header header.
  inline Callable *getBuiltinCallable(unsigned builtinMethodID);

  /// Store \p builtin in the builtins array.
  /// \pre builtinIndex must not have already been registered.
  inline void registerBuiltin(
      BuiltinMethod::Enum builtinIndex,
      Callable *builtin);

  /// Overwrite an already registered builtin callable. This is only used by
  /// synth tool to wrap Math builtins to provide stable results across libm
  /// implementations.
  /// \pre builtinIndex must have already been registered.
  /// \pre builtinIndex < BuiltinMethod::_count.
  /// \pre \p builtin is NativeFunction if builtinIndex <
  /// BuiltinMethod::_firstJS.
  inline void overwriteBuiltinUnsafe(
      BuiltinMethod::Enum builtinIndex,
      Callable *builtin);

  /// ES6-ES11 8.4.1 EnqueueJob ( queueName, job, arguments )
  /// See \c jobQueue_ for how the Jobs and Job Queues are set up in Hermes.
  inline void enqueueJob(Callable *job);

  /// ES6-ES11 8.6 RunJobs ( )
  /// Draining the job queue by invoking the queued jobs in FIFO order.
  ///
  /// \return ExecutionStatus::RETURNED if the job queue is emptied and no
  /// exception was thrown from any job. Otherwise, ExecutionStatus::EXCEPTION
  /// if an exception was thrown and the draining is stopped.
  /// It's the caller's responsibility to check the status and re-invoke this
  /// function to resume the draining to fulfill the microtask requirements.
  ///
  /// Note that ECMA-262 Promise Jobs always handle internal abruptCompletion
  /// by returning a rejected Promise. However, the capability of propagating
  /// exception per job is required by `queueMicrotask` to properly "report the
  /// exception" (https://html.spec.whatwg.org/C#microtask-queuing).
  ExecutionStatus drainJobs();

  /// ES2025 9.11 "When the abstract operation AddToKeptObjects is called with a
  /// target object or symbol, it adds the target to a list that will point
  /// strongly at the target until ClearKeptObjects is called."
  /// \pre \p obj must hold an Object or non-registered Symbol.
  ExecutionStatus addToKeptObjects(Handle<> obj);

  /// ES2025 9.10 "ECMAScript implementations are
  /// expected to call ClearKeptObjects when a synchronous sequence of
  /// ECMAScript executions completes." This method clears all kept WeakRefs and
  /// allows their targets to be eligible for garbage collection again.
  void clearKeptObjects();

  /// Add \p registry to the list of finalizationRegistries_.
  void addFinalizationRegistry(JSFinalizationRegistry *registry);

  /// Call the cleanup callbacks of alive JSFinalizationRegistry, on each dead
  /// registered target, w.r.t. ES16 9.12 CleanupFinalizationRegistry.
  LLVM_NODISCARD ExecutionStatus cleanUpFinalizationCallbacks();

  IdentifierTable &getIdentifierTable() {
    return identifierTable_;
  }

  SymbolRegistry &getSymbolRegistry() {
    return symbolRegistry_;
  }

  /// Return a StringPrimitive representation of a single character. The first
  /// 256 characters are pre-allocated. The rest are allocated every time.
  Handle<StringPrimitive> getCharacterString(char16_t ch);

  CodeBlock *getEmptyCodeBlock() const {
    assert(emptyCodeBlock_ && "Invalid empty code block");
    return emptyCodeBlock_;
  }

  CodeBlock *getReturnThisCodeBlock() const {
    assert(returnThisCodeBlock_ && "Invalid return this code block");
    return returnThisCodeBlock_;
  }

  /// Compute a hash value of a given HermesValue that is guaranteed to
  /// be stable with a moving GC. It however does not guarantee to be
  /// a perfect hash for strings.
  uint64_t gcStableHashHermesValue(HermesValue value);

  /// Compute a hash value of a given JSObject that is guaranteed to
  /// be stable with a moving GC.
  uint64_t gcStableHashJSObject(JSObject *object);

  /// @name Public VM State
  /// @{

  /// \return the current stack pointer.
  PinnedHermesValue *getStackPointer() {
    return toPHV(stackPointer);
  }

  /// Pop the register stack down to a previously saved stack pointer.
  inline void popToSavedStackPointer(PinnedHermesValue *stackPointer);

  /// \return the number of elements in the stack.
  inline uint32_t getStackLevel() const;

  /// Return the available stack size (in registers).
  inline uint32_t availableStackSize() const;

  /// \return true if there is space to allocate <tt>count + STACK_RESERVE</tt>
  /// registers.
  inline bool checkAvailableStack(uint32_t count);

  /// Allocate stack space for \p registers, but keep it uninitialized. The
  /// caller should initialize it ASAP.
  /// \return the new stack pointer.
  inline PinnedHermesValue *allocUninitializedStack(uint32_t count);

  /// Allocate stack space for \p registers and initialize them.
  inline void allocStack(uint32_t count);

  /// Check whether <tt>count + STACK_RESERVE</tt> stack registers are available
  /// and allocate \p count registers.
  /// \param count number of registers to allocate.
  /// \return \c true if allocation was successful.
  inline bool checkAndAllocStack(uint32_t count);

  /// Pop the specified number of elements from the stack.
  inline void popStack(uint32_t count);

  /// \return the current frame pointer.
  StackFramePtr getCurrentFrame() const {
    return StackFramePtr(toPHV(currentFrame));
  }

  /// Set the current frame pointer to the specified value.
  inline void setCurrentFrame(StackFramePtr frame);

  /// Set the current frame pointer to the current top of the stack and return
  /// it.
  /// \return the new value of the current frame pointer.
  inline StackFramePtr setCurrentFrameToTopOfStack();

  /// Restore the stack pointer to the base of the current frame and then
  /// set the frame pointer to the previous frame.
  /// \param currentFrame the currentFrame. It must match the value of
  ///   getCurrentFrame(), but is more efficient to pass it in assuming it
  ///   already is in a register. It also provides some additional error
  ///   checking in debug builds, ensuring that the stack hasn't changed
  ///   unexpectedly.
  /// \return the updated value of the current frame pointer.
  inline StackFramePtr restoreStackAndPreviousFrame(StackFramePtr currentFrame);

  /// Restore the stack pointer to the base of the current frame and then
  /// set the frame pointer to the previous frame.
  /// \return the updated value of the current frame pointer.
  inline StackFramePtr restoreStackAndPreviousFrame();

  /// \return an iterator range that provides access to all stack frames
  /// starting from the top-most one.
  inline llvh::iterator_range<StackFrameIterator> getStackFrames();

  /// \return an iterator range that provides access to all stack frames
  /// starting from the top-most one.
  inline llvh::iterator_range<ConstStackFrameIterator> getStackFrames() const;

  /// Dump information about all stack frames to \p OS.
  void dumpCallFrames(llvh::raw_ostream &OS);

  /// Dump information about all stack frames to llvh::errs(). This is a
  /// helper method intended to be called from a debugger.
  void dumpCallFrames();

  /// \return true if the native stack is overflowing the bounds of the
  ///   current thread. Updates the stack bounds if the thread which Runtime
  ///   is executing on changes. Will use simple depth counter if Runtime
  ///   has been compiled without \c HERMES_CHECK_NATIVE_STACK.
  inline bool isStackOverflowing();

  /// \return the overflow guard to be used in the regex engine.
  inline StackOverflowGuard getOverflowGuardForRegex();

  /// \return `thrownValue`.
  HermesValue getThrownValue() const {
    return *thrownValue_;
  }

  /// Set `thrownValue` to the specified value \p value, `returnValue` to
  /// empty and \return ExecutionResult::EXCEPTION.
  ExecutionStatus setThrownValue(HermesValue value);

  /// Set `thrownValue` to empty.
  void clearThrownValue();

  /// Return a hidden class corresponding to the specified prototype object
  /// and CellKind.
  /// \param root the root HiddenClass to use when making a new class,
  ///   must be non-null.
  /// IMPORTANT: This returns a Handle that is stored in a Runtime PHV field,
  /// so DO NOT hold the result of this function call through another
  /// getHiddenClassForPrototype call.
  inline Handle<HiddenClass> getHiddenClassForPrototype(
      JSObject *proto,
      Handle<HiddenClass> root);

  /// Return a hidden class corresponding to the specified prototype object
  /// for a lazy object.
  /// Lazy objects have a different HiddenClass hierarchy in order to avoid
  /// collisions with real objects.
  /// IMPORTANT: This returns a Handle that is stored in a Runtime PHV field,
  /// so DO NOT hold the result of this function call through another
  /// getHiddenClassForPrototype call.
  inline Handle<HiddenClass> getLazyHiddenClassForPrototype(JSObject *proto);

  /// Return the global object.
  Handle<JSObject> getGlobal();

  /// Return the JIT context.
  JITContext &getJITContext() {
    return jitContext_;
  }
  /// Returns trailing data for all runtime modules.
  std::vector<llvh::ArrayRef<uint8_t>> getEpilogues();

  /// \return the low 24 bits of the parent cache epoch: the part an
  /// AddPropertyCacheEntry stores. Never 0.
  uint32_t getParentCacheEpoch() const {
    return parentCacheEpoch_ & AddPropertyCacheEntry::kMaxParentEpoch;
  }

  /// \return the full parent cache epoch. It only increases and, being
  /// 64-bit, never wraps in practice; GetById chain entries compare it.
  uint64_t getFullParentCacheEpoch() const {
    return parentCacheEpoch_;
  }

  /// \return true if the parent cache epoch has not been updated since the
  /// array fast path prototype check was done.
  bool checkArrayFastPathParentEpoch() const {
    assert(
        arrayFastPathParentCacheEpoch_ <= parentCacheEpoch_ &&
        "parent cache epoch went backwards");
    return arrayFastPathParentCacheEpoch_ == parentCacheEpoch_;
  }

  /// Set the epoch to use for array fast path prototype checks.
  void setArrayFastPathParentEpoch() {
    arrayFastPathParentCacheEpoch_ = parentCacheEpoch_;
  }

#ifdef UNIT_TEST
  /// Unsafe function to use for testing the overflow mechanism. Sets the
  /// full counter.
  void testSetParentCacheEpoch(uint64_t epoch) {
    parentCacheEpoch_ = epoch;
  }
#endif

  /// Increment the parent cache epoch. When its low 24 bits reach
  /// kMaxParentEpoch, invalidate ALL PutById cache entries that have stored
  /// an epoch (i.e. represent a HiddenClass transition), and skip ahead so
  /// the low bits restart at 1 (0 is never a valid add cache epoch). The
  /// full counter keeps increasing.
  /// \return the new low 24 bits, as getParentCacheEpoch() returns them.
  uint32_t incParentCacheEpoch() {
    ++parentCacheEpoch_;
    if (LLVM_UNLIKELY(
            (parentCacheEpoch_ & AddPropertyCacheEntry::kMaxParentEpoch) ==
            AddPropertyCacheEntry::kMaxParentEpoch)) {
      invalidateAllAddCacheEntries();
      parentCacheEpoch_ += 2;
    }
    return getParentCacheEpoch();
  }

  /// Iterate all AddPropertyCacheEntry in all RuntimeModules,
  /// and set their startClazz to nullptr, invalidating them.
  void invalidateAllAddCacheEntries();

  void printHeapStats(llvh::raw_ostream &os);

  /// Write IO tracking (aka HBC page access) info to the supplied
  /// stream as JSON. There will only be useful data for RuntimeModules
  /// backed by mmap'ed bytecode, and there will only be any data at all if
  /// RuntimeConfig::withTrackIO() has been set, and IO tracking is available on
  /// the current platform.
  void getIOTrackingInfoJSON(llvh::raw_ostream &os);

#ifndef NDEBUG
  /// Iterate over all arrays in the heap and print their sizes and capacities.
  void printArrayCensus(llvh::raw_ostream &os);
#endif

  /// Returns the storage object for JSLib.
  JSLibStorage *getJSLibStorage() {
    return jsLibStorage_.get();
  }

  const GCExecTrace &getGCExecTrace() {
    return getHeap().getGCExecTrace();
  }

#if defined(HERMES_ENABLE_DEBUGGER)
  /// Request the interpreter loop to take an asynchronous break at a convenient
  /// point due to debugger UI request. This may be called from any thread, or a
  /// signal handler.
  void triggerDebuggerAsyncBreak(
      ::facebook::hermes::debugger::AsyncPauseKind kind) {
    triggerAsyncBreak(
        kind == ::facebook::hermes::debugger::AsyncPauseKind::Explicit
            ? AsyncBreakReasonBits::DebuggerExplicit
            : AsyncBreakReasonBits::DebuggerImplicit);
  }
#endif

  /// Request the interpreter loop to take an asynchronous break at a convenient
  /// point due to previous registered timeout. This may be called from any
  /// thread, or a signal handler.
  void triggerTimeoutAsyncBreak() {
    triggerAsyncBreak(AsyncBreakReasonBits::Timeout);
  }

#ifdef HERMES_ENABLE_DEBUGGER
  /// Encapsulates useful information about a stack frame, needed by the
  /// debugger. It requres extra context and cannot be extracted from a
  /// single frame pointer.
  struct StackFrameInfo {
    ConstStackFramePtr frame;
    bool isGlobal;
  };

  /// Extract \c StackFrameInfo of the specified stack frame.
  /// \param frameIdx a relative frame index where the top-most frame is 0.
  /// \return a populated StackFrameInfo, or llvh::None if the frame is invalid.
  llvh::Optional<StackFrameInfo> stackFrameInfoByIndex(uint32_t frameIdx) const;

  /// Calculate and \return the offset between the location of the specified
  /// frame and the start of the stack. This value increases with every nested
  /// call.
  uint32_t calcFrameOffset(ConstStackFrameIterator it) const;

  /// \return the offset between the location of the current frame and the
  ///   start of the stack. This value increases with every nested call.
  uint32_t getCurrentFrameOffset() const;
#endif

  /// Flag the interpreter that an error with the specified \p msg must be
  /// thrown when execution resumes.
  /// \return ExecutionResult::EXCEPTION
  LLVM_NODISCARD ExecutionStatus raiseError(const TwineChar16 &msg);

  /// Flag the interpreter that a type error with the specified message must be
  /// thrown when execution resumes.
  /// If the message is not a string, it is converted using toString().
  LLVM_NODISCARD ExecutionStatus raiseTypeError(Handle<> message);

  /// Flag the interpreter that a type error must be thrown when execution
  /// resumes.
  /// \return ExecutionResult::EXCEPTION
  LLVM_NODISCARD ExecutionStatus raiseTypeError(const TwineChar16 &msg);

  /// Flag the interpreter that a type error must be thrown when execution
  /// resumes. The string thrown concatenates a description of \p value
  /// with \p msg.
  /// \return ExecutionResult::EXCEPTION
  LLVM_NODISCARD ExecutionStatus
  raiseTypeErrorForValue(Handle<> value, llvh::StringRef msg) {
    return raiseTypeErrorForValue("", value, msg);
  }

  /// Flag the interpreter that a type error must be thrown when execution
  /// resumes. The string thrown concatenates \p msg1, a description of \p
  /// value, and \p msg2. \return ExecutionResult::EXCEPTION
  LLVM_NODISCARD ExecutionStatus raiseTypeErrorForValue(
      llvh::StringRef msg1,
      Handle<> value,
      llvh::StringRef msg2);

  /// Flag the interpreter that a syntax error must be thrown.
  /// \return ExecutionStatus::EXCEPTION
  LLVM_NODISCARD ExecutionStatus raiseSyntaxError(const TwineChar16 &msg);

  /// Raise a special SyntaxError when attempting to eval when disallowed.
  LLVM_NODISCARD ExecutionStatus raiseEvalUnsupported(llvh::StringRef code);

  /// Raise a \c RangeError exception.
  /// \return ExecutionStatus::EXCEPTION
  LLVM_NODISCARD ExecutionStatus raiseRangeError(const TwineChar16 &msg);

  /// Raise a \c ReferenceError exception.
  /// \return ExecutionStatus::EXCEPTION
  LLVM_NODISCARD ExecutionStatus raiseReferenceError(const TwineChar16 &msg);

  /// Overload that accepts any value as the message.
  /// If \p message is not a string, it is converted using toString().
  LLVM_NODISCARD ExecutionStatus raiseReferenceError(Handle<> message);

  /// Raise a \c URIError exception.
  /// \return ExecutionStatus::EXCEPTION
  LLVM_NODISCARD ExecutionStatus raiseURIError(const TwineChar16 &msg);

  enum class StackOverflowKind {
    // The JS register stack was exhausted.
    JSRegisterStack,
    // A limit on the number of native stack frames used in
    // evaluation, intended to conservatively prevent native stack
    // overflow, was exceeded.
    NativeStack,
    // RuntimeJSONParser has a maximum number of "nesting levels", and
    // calls raiseStackOverflow if that is exceeded.
    JSONParser,
    // JSONStringifyer has the same limit as JSONParser.
    JSONStringify,
  };

  /// Raise a stack overflow exception. This is special because constructing
  /// the object must not execute any custom or JavaScript code.  The
  /// argument influences the exception's message, to aid debugging.
  /// \return ExecutionStatus::EXCEPTION
  LLVM_NODISCARD ExecutionStatus raiseStackOverflow(StackOverflowKind kind);

  /// Raise an error for the quit function. This error is not catchable.
  LLVM_NODISCARD ExecutionStatus raiseQuitError();

  /// Raise an error for execution timeout. This error is not catchable.
  LLVM_NODISCARD ExecutionStatus raiseTimeoutError();

  /// Utility function to raise a catchable JS error with \p errMessage.
  LLVM_NODISCARD ExecutionStatus
  raiseUncatchableError(Handle<JSObject> prototype, llvh::StringRef errMessage);

  /// Interpret the current function until it returns or throws and return
  /// CallResult<HermesValue> or the thrown object in 'thrownObject'.
  CallResult<HermesValue> interpretFunction(CodeBlock *newCodeBlock);

#ifdef HERMES_ENABLE_DEBUGGER
  /// Single-step the provided function, update the interpreter state.
  ExecutionStatus stepFunction(InterpreterState &state);
#endif

  /// Inserts an object into the string cycle checking stack if it does not
  /// already exist.
  /// \return true if a cycle was found
  bool insertVisitedObject(JSObject *obj);

  /// Removes the last element (which must be obj) from the cycle check stack.
  /// \param obj the last element, which will be removed. Used for checking
  /// that invariants aren't violated in debug mode.
  void removeVisitedObject(JSObject *obj);

  /// Like calling JSObject::getNamed, but uses this runtime's property cache.
  CallResult<PseudoHandle<>> getNamed(Handle<JSObject> obj, PropCacheID id);

  /// Like calling JSObject::putNamed with the ThrowOnError flag, but uses this
  /// runtime's property cache.
  ExecutionStatus putNamedThrowOnError(
      Handle<JSObject> obj,
      PropCacheID id,
      SmallHermesValue hv);

  /// @}

#define RUNTIME_HV_FIELD(name, type) PinnedValue<type> name{};
#include "hermes/VM/RuntimeHermesValueFields.def"
#define RUNTIME_PHV_FIELD(name) \
  PinnedHermesValue name{HermesValue::encodeRawZeroValueUnsafe()};
#include "hermes/VM/RuntimePHVFields.def"

  /// Head of the locals list used for VM operations.
  Locals *vmLocals{};

  /// Raw pointers to prototypes.
  JSObject *objectPrototypeRawPtr{};

  JSObject *functionPrototypeRawPtr{};

  RegExpMatch regExpLastMatch{};

  /// The capture ranges of the latest regex search, reused by every search
  /// to avoid an allocation per search, so it keeps its peak capacity for
  /// the lifetime of the runtime. It is used only between the executor's
  /// return and the copy into a RegExpMatch (performSearch in
  /// JSRegExp.cpp); the executor runs no JS and allocates nothing on the GC
  /// heap, so it is never in use twice at once. regex::CapturedRange is
  /// incomplete here, so this has no default member initializer: GCC would
  /// instantiate the vector's destructor at it.
  std::vector<regex::CapturedRange> regExpSearchCaptures;

  /// A global counter that increments, used to provide unique object IDs.
  /// Each successive value is hashed by JSObject before being stored as the ID.
  uint32_t objectIDCounter{0};

  /// Whether to allow eval and Function ctor.
  const bool enableEval : 1;
  /// Whether to verify the IR being generated by eval and the Function ctor.
  const bool verifyEvalIR : 1;
  /// Whether to optimize the code in the string passed to eval and the Function
  /// ctor.
  const bool optimizedEval : 1;
  /// Whether to emit async break check instructions in eval().
  const bool asyncBreakCheckInEval : 1;
  /// Whether to increase compliance with test262.
  const bool test262 : 1;

  const SynthTraceMode traceMode;

  /// A ManagedChunkedList that stores values currently being used for
  /// serialization/deserialization
  ManagedChunkedList<SerializationManagedValue> serializationValues_;

#ifdef HERMESVM_PROFILER_OPCODE
  /// Track the frequency of each opcode in the interpreter.
  uint32_t opcodeExecuteFrequency[256] = {0};

  /// Track time spent of each opcode in the interpreter, in CPU cycles.
  uint64_t timeSpent[256] = {0};

  /// Dump opcode stats to a stream.
  void dumpOpcodeStats(llvh::raw_ostream &os) const;
#endif

#if defined(HERMESVM_PROFILER_JSFUNCTION)
  static std::atomic<ProfilerID> nextProfilerId;

  std::vector<ProfilerFunctionInfo> functionInfo{};

  /// Get block's index in functionInfo (creating a new entry if needed).
  ProfilerID getProfilerID(CodeBlock *block);

  /// Get profiler info associated with given id or nullptr if the id
  /// is not associated with any function.
  const ProfilerFunctionInfo *getProfilerInfo(ProfilerID id);

  /// Track the maximum size of the stack.
  uint32_t maxStackLevel = 0;
#endif

#if defined(HERMESVM_PROFILER_JSFUNCTION)
  std::vector<ProfilerFunctionEvent> functionEvents{};

  /// Total number of opcodes executed by this runtime.
  uint64_t opcodeCount = 0;

  /// Dump function profiling stats to stdout.
  enum class ProfileType { TIME, OPCODES, ALL };
  void dumpJSFunctionStats(ProfileType type = ProfileType::ALL);
#endif

#ifdef HERMESVM_PROFILER_BB
  BasicBlockExecutionInfo &getBasicBlockExecutionInfo();

  /// Dump basic block profile trace to \p OS in json format.
  void dumpBasicBlockProfileTrace(llvh::raw_ostream &OS);
#endif

  CodeCoverageProfiler &getCodeCoverageProfiler() {
    return *codeCoverageProfiler_;
  }

#if HERMESVM_SAMPLING_PROFILER_AVAILABLE
  /// Sampling profiler data for this runtime. The ctor/dtor of SamplingProfiler
  /// will automatically register/unregister this runtime from profiling.
  std::unique_ptr<SamplingProfiler> samplingProfiler;
#endif // HERMESVM_SAMPLING_PROFILER_AVAILABLE

  /// Time limit monitor data for this runtime.
  std::shared_ptr<TimeLimitMonitor> timeLimitMonitor;

#ifdef HERMESVM_PROFILER_NATIVECALL
  /// Dump statistics about native calls.
  void dumpNativeCallStats(llvh::raw_ostream &OS);
#endif

#ifdef HERMES_ENABLE_DEBUGGER
  Debugger &getDebugger() {
    return debugger_;
  }
#endif

  /// \return the stored StackExecutor.
  StackExecutor &getStackExecutor() {
    return *stackExecutor_;
  }

  /// \return the newly allocated script ID, incrementing the internal counter.
  facebook::hermes::debugger::ScriptID allocateScriptId() {
    return nextScriptId_++;
  }

  RuntimeModuleList &getRuntimeModules() {
    return runtimeModuleList_;
  }

  bool hasES6Proxy() const {
    return hasES6Proxy_;
  }

  bool hasAsyncGenerators() const {
    return hasAsyncGenerators_;
  }

  bool hasES6BlockScoping() const {
    return hasES6BlockScoping_;
  }

  bool hasIntl() const {
    return hasIntl_;
  }

  bool hasMicrotaskQueue() const {
    return hasMicrotaskQueue_;
  }

  bool builtinsAreFrozen() const {
    return builtinsFrozen_;
  }

  experiments::VMExperimentFlags getVMExperimentFlags() const {
    return vmExperimentFlags_;
  }

  // Return a reference to the runtime's CrashManager.
  inline CrashManager &getCrashManager();

  /// Returns a string representation of the JS stack.  Does no operations
  /// that allocate on the JS heap, so safe to use for an out-of-memory
  /// exception.
  /// \p ip specifies the the IP of the leaf frame.
  std::string getCallStackNoAlloc(const Inst *ip);

  /// \return a string representation of the JS stack without knowing the leaf
  /// frame ip.  Does no operations that allocate on the JS heap, so safe to use
  /// for an out-of-memory exception.
  std::string getCallStackNoAlloc() {
    return getCallStackNoAlloc(nullptr);
  }

  /// \return whether we are currently formatting a stack trace. Used to break
  /// recursion in Error.prepareStackTrace.
  bool formattingStackTrace() const {
    return formattingStackTrace_;
  }

  /// Mark whether we are currently formatting a stack trace. Used to break
  /// recursion in Error.prepareStackTrace.
  void setFormattingStackTrace(bool value) {
    assert(
        value != formattingStackTrace() &&
        "All calls to setFormattingStackTrace must actually change the current state");
    formattingStackTrace_ = value;
  }

  /// A stack overflow exception is thrown when \c nativeCallFrameDepth_ exceeds
  /// this threshold.
  static constexpr unsigned MAX_NATIVE_CALL_FRAME_DEPTH =
#ifdef HERMES_LIMIT_STACK_DEPTH
      // UBSAN builds will hit a native stack overflow much earlier, so make
      // this limit dramatically lower.
      30
#elif defined(_MSC_VER) && defined(__clang__) && defined(HERMES_SLOW_DEBUG)
      30
#elif defined(_MSC_VER) && defined(HERMES_SLOW_DEBUG)
      // On windows in dbg mode builds, stack frames are bigger, and a depth
      // limit of 384 results in a C++ stack overflow in testing.
      128
#elif defined(_MSC_VER) && defined(__clang__) && !defined(NDEBUG)
      128
#elif defined(_MSC_VER) && !NDEBUG
      192
#else
      /// This depth limit was originally 256, and we
      /// increased when an app violated it.  The new depth is 128
      /// larger.  See T46966147 for measurements/calculations indicating
      /// that this limit should still insulate us from native stack overflow.)
      384
#endif
      ;

  /// Called when various GC events(e.g. collection start/end) happen.
  void onGCEvent(GCEventKind kind, const std::string &extraInfo);

  /// Called by the GC at the beginning of a collection. This method informs the
  /// GC of all runtime roots.  The \p markLongLived argument
  /// indicates whether root data structures that contain only
  /// references to long-lived objects (allocated directly as long lived)
  /// are required to be scanned.
  void markRoots(RootAcceptorWithNames &acceptor, bool markLongLived);

  /// Called by the GC during collections that may reset weak references. This
  /// method informs the GC of all runtime weak roots.
  void markWeakRoots(WeakRootAcceptor &weakAcceptor, bool markLongLived);

  /// Iterate over runtimeModuleList_ and mark each RuntimeModule's WeakRoot to
  /// its owning Domain. If the domain is dead, destroy the RuntimeModule and
  /// remove it from runtimeModuleList_.
  void markDomainRefInRuntimeModules(WeakRootAcceptor &weakRootAcceptor);

  /// See documentation on \c GCBase::GCCallbacks.
  void markRootsForCompleteMarking(RootAcceptorWithNames &acceptor);

  /// Visits every entry in the identifier table and calls acceptor with
  /// the entry and its id as arguments. This is intended to be used only for
  /// snapshots, as it is slow. The function passed as acceptor shouldn't
  /// perform any heap operations.
  void visitIdentifiers(
      const std::function<void(SymbolID, const StringPrimitive *)> &acceptor);

  /// Convert the given symbol into its UTF-8 string representation.
  std::string convertSymbolToUTF8(SymbolID id);

  /// Prints any statistics maintained in the Runtime about GC to \p
  /// os.  At present, this means the breakdown of markRoots time by
  /// "phase" within markRoots.
  void printRuntimeGCStats(JSONEmitter &json) const;

  /// \return one higher than the largest symbol in the identifier table. This
  /// enables the GC to size its internal structures for symbol marking.
  /// Optionally invoked at the beginning of a garbage collection.
  unsigned getSymbolsEnd() const;

  /// Called by the GC at the end of a collection to free all symbols not set in
  /// markedSymbols. The function may set additional bits in \p markedSymbols to
  /// reflect the fact that some symbols were not freed.
  void freeSymbols(llvh::BitVector &markedSymbols);

#ifdef HERMES_SLOW_DEBUG
  /// \return true if the given symbol is a live entry in the identifier
  /// table.
  bool isSymbolLive(SymbolID id);

  /// \return An associated heap cell for the symbol if one exists, null
  /// otherwise.
  const void *getStringForSymbol(SymbolID id);
#endif

  /// See \c GCCallbacks for details.
  size_t mallocSize() const;

 private:
  /// Only called internally or by the wrappers used for profiling.
  CallResult<HermesValue> interpretFunctionImpl(CodeBlock *newCodeBlock);

  explicit Runtime(
      std::shared_ptr<StorageProvider> provider,
      const RuntimeConfig &runtimeConfig);

  /// Generate a bytecode buffer that contains a few special functions:
  /// 0) an empty function that returns undefined.
  /// 1) a function that returns the global object.
  static std::unique_ptr<Buffer> generateSpecialRuntimeBytecode();

  /// Insert the predefined strings into the IdentifierTable.
  /// NOTE: this function does not do any allocations in the GC heap, it is safe
  /// to use at any time in initialization.
  void initPredefinedStrings();

  /// Initialize the \c charStrings_ array with a StringPrimitive for each
  /// character.
  void initCharacterStrings();

  /// Initialize the root HiddenClasses for all the GCCells.
  void initRootHiddenClasses();

  /// \param methodIndex is the index of the method in the table that lists
  ///   all the builtin methods, which is what we are iterating over.
  /// \param objectName is the id for the name of the object in the list of the
  ///   predefined strings.
  /// \param object is the object where the builtin method is defined as a
  ///   property.
  /// \param methodID is the SymbolID for the name of the method.
  using ForEachPublicNativeBuiltinCallback = ExecutionStatus(
      unsigned methodIndex,
      bool frozen,
      Predefined::Str objectName,
      Handle<JSObject> &object,
      SymbolID methodID);

  /// Enumerate all public native builtin methods, and invoke the callback on
  /// each method.
  /// If the properties on the global object which are supposed to contain the
  /// public native builtin methods no longer are objects, throws TypeError
  /// instead of calling the callback.
  /// e.g. if globalThis.Object is not an object, it'll throw.
  ExecutionStatus forEachPublicNativeBuiltin(
      const std::function<ForEachPublicNativeBuiltinCallback> &callback);

  /// Populate native builtins into the builtins table.
  /// Public native builtins are added by extracting the values from the global
  /// object. Private native builtins are added by \c createHermesBuiltins().
  void initNativeBuiltins();

  /// Populate JS builtins into the builtins table, after verifying they do
  /// exist from the result of running internal bytecode.
  void initJSBuiltins(Handle<JSObject> jsBuiltins);

  /// Walk all the builtin methods, assert that they are not overridden. If they
  /// are, throw an exception. This will be called at most once, before freezing
  /// the builtins.
  ExecutionStatus assertBuiltinsUnmodified();

  /// Called after asserting all builtin methods are not overridden, to freeze
  /// those builtins. This will be called at most once.
  void freezeBuiltins();

  /// The slow path for \c getCharacterString(). This function allocates a new
  /// string for the passed character \p ch.
  Handle<StringPrimitive> allocateCharacterString(char16_t ch);

  /// Add a \c RuntimeModule \p rm to the runtime module list.
  void addRuntimeModule(RuntimeModule *rm) {
    runtimeModuleList_.push_back(*rm);
  }

  /// Remove a \c RuntimeModule \p rm from the runtime module list.
  void removeRuntimeModule(RuntimeModule *rm);

  /// Called by CrashManager on the event of a crash to produce a stream of data
  /// to crash log. Output should be a JSON object. This is the central point
  /// for adding calls to further functions which dump specific elements of
  /// of crash dump data for a Hermes Runtime instance.
  void crashCallback(int fd);

  /// Write a JS stack trace as part of a \c crashCallback() run.
  void crashWriteCallStack(JSONEmitter &json);

  /// Initialize the given stack space with zeroes.
  /// \param base the starting address of the space to initialize.
  /// \param count the number of HermesValues to initialize.
  inline void initStackWithZeroes(PinnedHermesValue *base, uint32_t count);

 private:
  GCBase::GCCallbacksWrapper<Runtime> gcCallbacksWrapper_;
  GC heap_;

  std::vector<std::function<void(GC *, RootAcceptor &)>> customMarkRootFuncs_;
  std::vector<std::function<void(GC *, WeakRootAcceptor &)>>
      customMarkWeakRootFuncs_;
  std::vector<std::function<void(HeapSnapshot &)>> customSnapshotNodeFuncs_;
  std::vector<std::function<void(HeapSnapshot &)>> customSnapshotEdgeFuncs_;
  std::vector<std::function<void()>> drainJobsCallbacks_;
  std::vector<std::function<void()>> shutdownCallbacks_;
  std::vector<std::function<void()>> postShutdownDeleters_;

  /// All state related to JIT compilation.
  JITContext jitContext_;

  /// Set to true if we should enable ES6 Proxy.
  const bool hasES6Proxy_;

  /// Set to true if we should enable async generators.
  const bool hasAsyncGenerators_;

  /// Set to true if we should enable ES6 block scoping.
  const bool hasES6BlockScoping_;

  /// Set to true if we should enable ECMA-402 Intl APIs.
  const bool hasIntl_;

  /// Set to true if we are using microtasks.
  const bool hasMicrotaskQueue_;

  /// Set to true if we should randomize stack placement etc.
  const bool shouldRandomizeMemoryLayout_;

  // Percentage in [0,100] of bytecode we should eagerly read into page cache.
  const uint8_t bytecodeWarmupPercent_;

  // Signal-based I/O tracking. Slows down execution.
  const bool trackIO_;

  // Whether we are currently formatting a stack trace. Used to break recursion
  // in Error.prepareStackTrace.
  bool formattingStackTrace_{false};

  /// This value can be passed to the runtime as flags to test experimental
  /// features. Each experimental feature decides how to interpret these
  /// values. Generally each experiment is associated with one or more bits of
  /// this value. Interpretation of these bits is up to each experiment.
  /// To add an experiment, populate the VMExperimentFlags enum with additional
  /// bit values, typically 1 as test and 0 as control.
  experiments::VMExperimentFlags vmExperimentFlags_{experiments::Default};

  friend class GCScope;
  friend class HandleBase;
  friend class Interpreter;
  friend class RuntimeModule;
  friend class MarkRootsPhaseTimer;
  friend struct RuntimeOffsets;
  friend class ScopedNativeDepthReducer;
  friend class ScopedNativeDepthTracker;
  friend class ScopedNativeCallFrame;
  FRIEND_JIT;

  class StackRuntime;
  class MarkRootsPhaseTimer;

  /// Whenever we pass through the first phase, we record the current time here,
  /// so we can calculate the total time after we pass through the last phase.
  std::chrono::time_point<std::chrono::steady_clock> startOfMarkRoots_;
  /// The duration of each GC root marking phase is accumulated here.
  double markRootsPhaseTimes_[static_cast<unsigned>(
      RootAcceptor::Section::NumSections)] = {};
  /// The duration of the all root makring is accumulated here.
  double totalMarkRootsTime_ = 0.0;

  /// The identifier table.
  IdentifierTable identifierTable_{};

  /// The global symbol registry.
  SymbolRegistry symbolRegistry_{};

  /// Shared location to place native objects required by JSLib
  std::unique_ptr<JSLibStorage> jsLibStorage_;

  /// Empty code block that returns undefined.
  /// Owned by specialCodeBlockRuntimeModule_.
  CodeBlock *emptyCodeBlock_{};

  /// Code block that returns the global object.
  /// Owned by specialCodeBlockRuntimeModule_.
  CodeBlock *returnThisCodeBlock_{};

  /// The runtime module that owns emptyCodeBlock_ and returnThisCodeBlock_.
  /// We use a raw pointer here because it will be added to runtimeModuleList_,
  /// and will be freed when Runtime is freed.
  RuntimeModule *specialCodeBlockRuntimeModule_{};

  /// A list of all active runtime modules. Each \c RuntimeModule adds itself
  /// on construction and removes itself on destruction. When marking weak
  /// roots, scan each RuntimeModule and destroy it if the owning Domain is
  /// dead (each RuntimeModule has a WeakRoot to its owning Domain).
  RuntimeModuleList runtimeModuleList_{};

  /// Optional record of the last few executed bytecodes in case of a crash.
  CrashTrace crashTrace_{};

  /// A list of created JSFinalizationRegistry. It's weakly held because the
  /// Runtime does not keep them alive. At each microtask checkpoint, all dead
  /// elements are removed, the rest alive JSFinalizationRegistry are iterated
  /// and its `cleanup()` method is called.
  std::vector<WeakRoot<JSFinalizationRegistry>> finalizationRegistries_{};

  /// @name Private VM State
  /// @{

  /// If the register stack is allocated by the runtime, then this stores its
  /// location and size.
  llvh::MutableArrayRef<PinnedHermesValue> registerStackAllocation_;
  /// Manages data to be used in the case of a crash.
  std::shared_ptr<CrashManager> crashMgr_;

#ifdef HERMES_CHECK_NATIVE_STACK
  /// Native stack remaining before assuming overflow. The bounds computed
  /// with it are in SHRuntime::overflowGuard, where native code checks them
  /// inline.
  unsigned nativeStackGap_;
#else
  /// Used to guard against stack overflow by counting call depth.
  StackOverflowGuard overflowGuard_;
#endif

  /// Caches for property lookups in non-JS code.
  WritePropertyCacheEntry fixedWritePropCache_[(size_t)PropCacheID::_COUNT];
  ReadPropertyCacheEntry fixedReadPropCache_[(size_t)PropCacheID::_COUNT];

  /// StringPrimitive representation of the first 256 characters.
  /// These are allocated as "long-lived" objects, so they don't need
  /// to be scanned as roots in young-gen collections.
  std::vector<PinnedHermesValue> charStrings_{};

  /// Keeps track of objects that have already been visited in order to detect
  /// cycles while doing string conversions.
  std::vector<JSObject *> stringCycleCheckVisited_{};

  /// Array of BuiltinMethod::_count length to callable implementations of
  /// builtins.
  ///
  /// Allocated with calloc, will be free() by the destructor.
  TransparentOwningPtr<Callable *, llvh::FreeDeleter> builtins_{};

  /// True if the builtins are all frozen (non-writable, non-configurable).
  bool builtinsFrozen_{false};

  /// ES6-ES11 8.4 Jobs and Job Queues.
  /// A queue of pointers to callables that represent Jobs.
  ///
  /// Job: Since the ScriptJob is removed from ES12, the only type of Job from
  /// ECMA-262 are Promise Jobs (https://tc39.es/ecma262/#sec-promise-jobs).
  /// But it is also possible to implement the HTML defined `queueMicrotask`,
  /// which is polyfill-able via Promise, by directly enqueuing into this job.
  ///
  /// Job are represented as callables with no parameters and would be invoked
  /// via \c executeCall0 in Hermes. It's safe to do so because:
  /// - Promise Jobs enqueued from Promise internal bytecode are thunks (or, in
  /// the ES12 wording, Promise Jobs are Abstract Closure with no parameters).
  /// - `queueMicrotask` take a JSFunction but only invoke it with 0 arguments.
  ///
  /// Although ES12 (9.4 Jobs and Host Operations to Enqueue Jobs) changed the
  /// meta-language to ask hosts to schedule Promise Job to integrate with the
  /// HTML spec, Hermes chose to adapt the ES6-11 suggested internal queue
  /// approach, similar to other engines, e.g. V8/JSC, which is more efficient
  /// (being able to batch the job invocations) and sufficient to express the
  /// HTML spec specified "perform a microtask checkpoint" algorithm.
  std::deque<Callable *> jobQueue_{};

#ifdef HERMESVM_PROFILER_BB
  BasicBlockExecutionInfo basicBlockExecInfo_;
#endif

  /// ScriptIDs to use for new RuntimeModules coming in.
  facebook::hermes::debugger::ScriptID nextScriptId_{1};

  /// An epoch used to invalidate property write cache entries and GetById
  /// chain entries. Incremented whenever a cached object's parent changes
  /// its parent or its HiddenClass, because those operations could
  /// introduce parent chain changes that prevent adding the property the
  /// way that was cached.
  ///
  /// 64-bit and monotonic, so it never wraps in practice. The add cache
  /// stores only its low 24 bits (getParentCacheEpoch()); when those wrap,
  /// ALL property write caches are invalidated. Starts at 1, and the low
  /// bits are never 0, so 0 can be used to ensure an add cache check
  /// fails.
  uint64_t parentCacheEpoch_ = 1;

  /// The full epoch when the array prototype was verified to allow us to
  /// use fast paths in array JSLib functions. If parentCacheEpoch_ is
  /// different, we must reverify the prototype chain.
  uint64_t arrayFastPathParentCacheEpoch_ = 0;

  /// Store a key for the function that is executed if a crash occurs.
  /// This key will be unregistered in the destructor.
  const CrashManager::CallbackKey crashCallbackKey_;

  /// Pointer to the code coverage profiler.
  const std::unique_ptr<CodeCoverageProfiler> codeCoverageProfiler_;

  /// Bit flags for async break request reasons.
  enum class AsyncBreakReasonBits : uint8_t {
    DebuggerExplicit = 0x1,
    DebuggerImplicit = 0x2,
    Timeout = 0x4,
  };

  /// An atomic flag set when an async pause is requested.
  /// It is a bits flag with each bit reserved for different clients
  /// defined by AsyncBreakReasonBits.
  /// This may be manipulated from multiple threads.
  std::atomic<uint8_t> asyncBreakRequestFlag_{0};

#if defined(HERMES_ENABLE_DEBUGGER)
  /// \return zero if no debugger async pause was requested, the old nonzero
  /// async flags if an async pause was requested. If nonzero is returned, the
  /// flag is reset to 0.
  uint8_t testAndClearDebuggerAsyncBreakRequest() {
    return testAndClearAsyncBreakRequest(
        (uint8_t)AsyncBreakReasonBits::DebuggerExplicit |
        (uint8_t)AsyncBreakReasonBits::DebuggerImplicit);
  }

  Debugger debugger_{*this};

  /// A copy of the internal bytecode made so that we can set breakpoints in it
  /// to allow for debugging.
  std::unique_ptr<uint8_t, llvh::FreeDeleter> internalBytecodeCopy_;
#endif

  /// Use an 8MB stack, which is the default size on mac and linux.
  static constexpr size_t kExecutorStackSize = 1 << 23;

  /// Idle for 1 second before letting the executor thread be cleaned up,
  /// after which further tasks will start a new thread.
  static constexpr std::chrono::milliseconds kExecutorTimeout =
      std::chrono::milliseconds(1000);

  /// The executor used to run the compiler.
  std::shared_ptr<StackExecutor> stackExecutor_ =
      newStackExecutor(kExecutorStackSize, kExecutorTimeout);

  /// Holds references to persistent BC providers for the lifetime of the
  /// Runtime. This is needed because the identifier table may contain pointers
  /// into bytecode, and so memory backing these must be preserved.
  std::vector<std::shared_ptr<hbc::BCProvider>> persistentBCProviders_;

  /// Config-provided callback for GC events.
  std::function<void(GCEventKind, const char *)> gcEventCallback_;

  /// @}

  /// \return whether any async break is requested or not.
  bool hasAsyncBreak() const {
    return asyncBreakRequestFlag_.load(std::memory_order_relaxed) != 0
#ifdef __EMSCRIPTEN__
        || test_wasm_host_timeout()
#endif
        ;
  }

  /// \return whether async break was requested or not for \p reasonBits. Clear
  /// \p reasonBit request bit afterward.
  uint8_t testAndClearAsyncBreakRequest(uint8_t reasonBits) {
    /// Note that while the triggerTimeoutAsyncBreak() function may be called
    /// from any thread, this one may only be called from within the Interpreter
    /// loop.
    uint8_t flag = asyncBreakRequestFlag_.load(std::memory_order_relaxed);
    if (LLVM_LIKELY((flag & (uint8_t)reasonBits) == 0)) {
      // Fast path.
      return false;
    }
    // Clear the flag using CAS.
    uint8_t oldFlag = asyncBreakRequestFlag_.fetch_and(
        ~(uint8_t)reasonBits, std::memory_order_relaxed);
    assert(oldFlag != 0 && "Why is oldFlag zero?");
    return oldFlag;
  }

  /// \return whether timeout async break was requested or not. Clear the
  /// timeout request bit afterward.
  bool testAndClearTimeoutAsyncBreakRequest() {
    return testAndClearAsyncBreakRequest((uint8_t)AsyncBreakReasonBits::Timeout)
#ifdef __EMSCRIPTEN__
        // Avoid the short-circuiting logic to make sure we clear both.
        | test_and_clear_wasm_host_timeout()
#endif
        ;
  }

  /// Request the interpreter loop to take an asynchronous break
  /// at a convenient point.
  void triggerAsyncBreak(AsyncBreakReasonBits reason) {
    asyncBreakRequestFlag_.fetch_or((uint8_t)reason, std::memory_order_relaxed);
  }

  /// Notify runtime execution has timeout.
  ExecutionStatus notifyTimeout();

 private:
  /// See \c ::setCurrentIP() and \c ::getCurrentIP() .
  const inst::Inst *currentIP_{nullptr};

  /// Sentinel value for \c currentIP_ to indicate that the currently stored
  /// value is invalid and should not be used.
  /// This is used to invalidate \c currentIP inside the interpreter between
  /// calls to CAPTURE_IP(). The interpreter itself saves and restores it on
  /// entry and exit. The purpose of these games is to catch if the interpreter
  /// ever calls out into a function that may observe the IP without setting it.
  static constexpr uintptr_t kInvalidCurrentIP = 0x1;

#ifndef NDEBUG
  /// The number of alive/active NoRJSScopes. If nonzero, then no JS execution
  /// is allowed
  uint32_t noRJSLevel_{0};

  friend class NoRJSScope;

  /// If the function currently at the top of the stack is a JSFunction, assert
  /// that the given IP falls within it.
  void assertTopCodeBlockContainsIP(const inst::Inst *ip) const;
#endif

 public:
  /// Set the value of the current Instruction Pointer (IP). Generally this is
  /// called by the interpeter before it makes any major calls out of its main
  /// loop. However, the interpeter also reads the value held here back after
  /// major calls return so this can also be called by other code which wants to
  /// return to the interpreter at a different IP. This allows things external
  /// to the interpreter loop to affect the flow of bytecode execution.
  inline void setCurrentIP(const inst::Inst *ip) {
#ifdef HERMES_SLOW_DEBUG
    assertTopCodeBlockContainsIP(ip);
#endif
    currentIP_ = ip;
  }

  /// Get the current Instruction Pointer (IP). This can be used to find out
  /// the last bytecode executed if we're currently in the interpeter loop. If
  /// we are not in the interpeter loop (i.e. we've made it into the VM
  /// internals via a native call), this this will return nullptr.
  inline const inst::Inst *getCurrentIP() const {
    assert(
        (uintptr_t)currentIP_ != kInvalidCurrentIP &&
        "Current IP unknown - this probably means a CAPTURE_IP_* is missing in the interpreter.");
#ifdef HERMES_SLOW_DEBUG
    assertTopCodeBlockContainsIP(currentIP_);
#endif
    return currentIP_;
  }

  /// This is slow compared to \c getCurrentIP() as it's virtual.
  inline const inst::Inst *getCurrentIPSlow() const {
    return getCurrentIP();
  }

#ifdef NDEBUG
  void invalidateCurrentIP() {}
  void validateSavedIPBeforeCall() const {}
#else
  void invalidateCurrentIP() {
    currentIP_ = (const inst::Inst *)kInvalidCurrentIP;
  }
  /// Validate that the saved IP in the outgoing call registers of the current
  /// frame has been correctly populated. This must be called when the frame and
  /// stack pointers still represent to the caller frame.
  void validateSavedIPBeforeCall() const;
#endif

 private:
#ifdef HERMES_MEMORY_INSTRUMENTATION
  /// Given the current last known IP used in the interpreter loop, returns the
  /// last known CodeBlock and IP combination. IP must not be null as this
  /// suggests we're not in the interpter loop, and there will be no CodeBlock
  /// to find.
  std::pair<const CodeBlock *, const inst::Inst *>
  getCurrentInterpreterLocation(const inst::Inst *initialSearchIP);

 public:
  /// Return a StackTraceTreeNode for the last known interpreter bytecode
  /// location. Returns nullptr if we're not in the interpeter loop, or
  /// allocation location tracking is not enabled. For this to function it
  /// needs to be passed the current IP from getCurrentIP(). We do not call
  /// this internally because it should always be called prior even if we
  /// do not really want a stack-traces node. This means we can leverage our
  /// library of tests to assert getCurrentIP() would return the right value
  /// at this point without actually collecting stack-trace data.
  StackTracesTreeNode *getCurrentStackTracesTreeNode(const inst::Inst *ip);

  /// Return the current StackTracesTree or nullptr if it's not available.
  StackTracesTree *getStackTracesTree() {
    return stackTracesTree_.get();
  }

  /// To facilitate allocation location tracking this must be called by the
  /// interpeter:
  /// * Just before we enter a new CodeBlock
  /// * At the entry point of a CodeBlock if this is the first entry into the
  ///   interpter loop.
  inline void pushCallStack(const CodeBlock *codeBlock, const inst::Inst *ip) {
    if (stackTracesTree_) {
      pushCallStackImpl(codeBlock, ip);
    }
  }

  /// Must pair up with every call to \c pushCallStack .
  inline void popCallStack() {
    if (stackTracesTree_) {
      popCallStackImpl();
    }
  }

  void enableAllocationLocationTracker() {
    enableAllocationLocationTracker(nullptr);
  }
  void enableAllocationLocationTracker(
      std::function<void(
          uint64_t,
          std::chrono::microseconds,
          std::vector<GCBase::AllocationLocationTracker::HeapStatsUpdate>)>
          fragmentCallback);

  /// Disable allocation location tracking for new objects. Old objects tagged
  /// with stack traces continue to be tracked until they are freed.
  /// \param clearExistingTree is for use by tests and in general will break
  /// because old objects would end up with dead pointers to stack-trace nodes.
  void disableAllocationLocationTracker(bool clearExistingTree = false);

  /// Enable the heap sampling profiler. About every \p samplingInterval bytes
  /// allocated, the stack will be sampled. This will attribute the highest
  /// allocating functions.
  /// \param samplingInterval A number of bytes that a sample should be taken,
  ///   on average.
  /// \param seed If non-negative, used as the seed for the random engine.
  void enableSamplingHeapProfiler(size_t samplingInterval, int64_t seed = -1);

  /// Disable the heap sampling profiler and flush the results out to \p os.
  void disableSamplingHeapProfiler(llvh::raw_ostream &os);

 private:
  void popCallStackImpl();
  void pushCallStackImpl(const CodeBlock *codeBlock, const inst::Inst *ip);
  std::unique_ptr<StackTracesTree> stackTracesTree_;
#endif
};

/// An encrypted/obfuscated native pointer. The key is held by GCBase.
template <typename T, XorPtrKeyID K>
class XorPtr {
  uintptr_t bits_;

 public:
  XorPtr() = default;
  XorPtr(Runtime &runtime, T *ptr) {
    set(runtime, ptr);
  }
  void set(Runtime &runtime, T *ptr) {
    set(runtime.getHeap(), ptr);
  }
  void set(GC &gc, T *ptr) {
    bits_ = reinterpret_cast<uintptr_t>(ptr) ^ gc.pointerEncryptionKey_[K];
  }
  T *get(Runtime &runtime) const {
    return get(runtime.getHeap());
  }
  T *get(GC &gc) const {
    return reinterpret_cast<T *>(bits_ ^ gc.pointerEncryptionKey_[K]);
  }
};

static_assert(
    std::is_trivial<XorPtr<void, XorPtrKeyID::_NumKeys>>::value,
    "XorPtr must be trivial");

/// Base for a stack allocated container for HermesValues, which are tracked by
/// the GC. This allows us to reference GC managed values without incurring the
/// dynamic allocation overhead of GCScope.
struct Locals {
  /// Number of trailing values that need to be marked.
  uint32_t numLocals;

  /// Pointer to the previous Locals struct.
  Locals *prev;

  /// Return a pointer to the locals array following this structure.
  inline PinnedHermesValue *locals();
  /// Return the offset of the locals array within Locals.
  static inline constexpr size_t localsOffset();

#ifndef NDEBUG
  /// In debug mode, assert to ensure the locals are properly registered.
  Locals() : numLocals(UINT32_MAX) {}
  ~Locals() {
    assert(numLocals != UINT32_MAX && "Locals never pushed");
  }
#endif
};

/// The layout local instances will have when instantiated.
struct LocalsImpl : public Locals {
  /// The trailing locals.
  PinnedHermesValue _locals[1];
};

inline PinnedHermesValue *Locals::locals() {
  return static_cast<LocalsImpl *>(this)->_locals;
}

inline constexpr size_t Locals::localsOffset() {
  return offsetof(LocalsImpl, _locals);
}

/// RAII class to push/pop a Locals struct within a scope.
class [[nodiscard]] LocalsRAII {
  Runtime &runtime_;
  Locals *locals_;

 public:
  template <typename T>
  explicit LocalsRAII(Runtime &runtime, T *locals)
      : runtime_(runtime), locals_(locals) {
    locals->prev = runtime_.vmLocals;
    locals->numLocals =
        (sizeof(T) - Locals::localsOffset()) / sizeof(PinnedHermesValue);
#ifdef HERMES_SLOW_DEBUG
    // All pointer/symbol type PinnedValues in locals must be initialized/reset
    // to default. Otherwise, potential dangling pointers could be accessed by
    // the GC.
    for (size_t i = 0; i < locals_->numLocals; ++i) {
      auto &phv = locals_->locals()[i];
      assert(
          (!phv.isPointer() || !phv.getPointer()) &&
          (!phv.isSymbol() || phv.getSymbol().isInvalid()));
    }
#endif
    runtime_.vmLocals = locals;
  }
  ~LocalsRAII() {
    assert(
        runtime_.vmLocals == locals_ &&
        "LocalsRAII must be destroyed in the reverse order of creation");
    runtime_.vmLocals = locals_->prev;
  }
};

/// An RAII class for automatically tracking the native call frame depth.
class ScopedNativeDepthTracker {
  Runtime &runtime_;
  /// Whether the stack overflowed when the tracker was constructed.
  bool overflowed_;
#ifndef HERMES_CHECK_NATIVE_STACK
  StackOverflowGuard::CallFrameRAII frame_;
#endif

 public:
  explicit ScopedNativeDepthTracker(Runtime &runtime)
      : runtime_(runtime)
#ifndef HERMES_CHECK_NATIVE_STACK
        ,
        frame_(StackOverflowGuard::CallFrameRAII{runtime_.overflowGuard_})
#endif
  {
    (void)runtime_;
    overflowed_ = runtime.isStackOverflowing();
  }

  /// \return whether we overflowed the native call frame depth.
  bool overflowed() const {
    return overflowed_;
  }
};

/// An RAII class which creates a little headroom in the native depth
/// tracking.  This is used when calling into JSError to extract the
/// stack, as the error may represent an overflow.  Without this, a
/// cascade of exceptions could occur, overflowing the C++ stack.
class ScopedNativeDepthReducer {
  Runtime &runtime_;
#ifdef HERMES_CHECK_NATIVE_STACK
  unsigned nativeStackGapOld;
  // This is empirically good enough.
  static constexpr int kReducedNativeStackGap =
#if LLVM_ADDRESS_SANITIZER_BUILD
      128 * 1024;
#else
      32 * 1024;
#endif
#else
  bool undo = false;
  // This is empirically good enough.
  static constexpr int kDepthAdjustment = 3;
#endif

 public:
#ifdef HERMES_CHECK_NATIVE_STACK
  explicit ScopedNativeDepthReducer(Runtime &runtime)
      : runtime_(runtime), nativeStackGapOld(runtime.nativeStackGap_) {
    // Temporarily reduce the gap to use that headroom for gathering the error.
    // If overflow is detected, the recomputation of the stack bounds will
    // result in no gap for the duration of the ScopedNativeDepthReducer's
    // lifetime.
    runtime_.nativeStackGap_ = kReducedNativeStackGap;
  }
  ~ScopedNativeDepthReducer() {
    assert(
        runtime_.nativeStackGap_ == kReducedNativeStackGap &&
        "ScopedNativeDepthReducer gap was overridden");
    runtime_.nativeStackGap_ = nativeStackGapOld;
    // Force the bounds to be recomputed the next time.
    StackOverflowGuard::clearStackBounds(&runtime_.overflowGuard);
  }
#else
  explicit ScopedNativeDepthReducer(Runtime &runtime) : runtime_(runtime) {
    if (runtime.overflowGuard_.callDepth >= kDepthAdjustment) {
      runtime.overflowGuard_.callDepth -= kDepthAdjustment;
      undo = true;
    }
  }
  ~ScopedNativeDepthReducer() {
    if (undo) {
      runtime_.overflowGuard_.callDepth += kDepthAdjustment;
    }
  }
#endif

 private:
  /// Unused function for static asserts that use internal information.
  static void staticAsserts();
};

/// A ScopedNativeCallFrame is an RAII class that manipulates the Runtime
/// stack and depth counter, and holds a stack frame. The stack frame contents
/// may be accessed (as StackFramePtr) via ->. Note that constructing this may
/// fail due to stack overflow, either via the register stack or the depth
/// counter. It is necessary to check the overflowed() flag before access the
/// stack frame contents.
/// Note that the arguments to the call frame are left uninitialized. The caller
/// must clear these before triggering a GC. The fillArgs() function may be used
/// for this purpose.
class ScopedNativeCallFrame {
  /// The runtime for this call frame.
  Runtime &runtime_;

  /// The stack pointer that will be restored in the destructor.
  PinnedHermesValue *const savedSP_;

  /// The contents of the new frame.
  StackFramePtr frame_;

  /// Whether this call frame overflowed.
  bool overflowed_;

#ifndef NDEBUG
  /// Whether the user has called overflowed() with a false result.
  mutable bool overflowHasBeenChecked_{false};
#endif

  /// \return whether the runtime can allocate a new frame with the given number
  /// of registers. This may fail if we've overflowed our register stack, or
  /// exceeded the native call frame depth.
  static bool runtimeCanAllocateFrame(
      Runtime &runtime,
      uint32_t registersNeeded) {
    return runtime.checkAvailableStack(registersNeeded) &&
        !runtime.isStackOverflowing();
  }

 public:
  /// Construct a native call frame for the given \p runtime in preparation of
  /// calling \p callee with \p argCount arguments and the given \p thisArg.
  /// \p callee is either a native pointer to CodeBlock, or an object pointer to
  /// a Callable (the two cases are distinguished by the type tag).
  /// \p newTarget is either \c undefined or the callable of the constructor
  /// currently being invoked by new.
  /// On overflow, the overflowed() flag is set, in which case the stack frame
  /// must not be used.
  /// The arguments are initially uninitialized. The caller should initialize
  /// them by storing into them, or via fillArguments().
  ScopedNativeCallFrame(
      Runtime &runtime,
      uint32_t argCount,
      HermesValue callee,
      HermesValue newTarget,
      HermesValue thisArg)
      : runtime_(runtime), savedSP_(runtime.getStackPointer()) {
#ifndef HERMES_CHECK_NATIVE_STACK
    runtime.overflowGuard_.callDepth++;
#endif
    uint32_t registersNeeded =
        StackFrameLayout::callerOutgoingRegisters(argCount);
    overflowed_ = !runtimeCanAllocateFrame(runtime, registersNeeded);
    if (LLVM_UNLIKELY(overflowed_)) {
      return;
    }

    // We have enough space. Increment the call frame depth and construct the
    // frame. The ScopedNativeCallFrame will restore both.
    auto *stack = runtime.allocUninitializedStack(registersNeeded);
    frame_ = StackFramePtr::initFrame(
        stack,
        runtime.getCurrentFrame(),
        runtime.getCurrentIP(),
        nullptr,
        nullptr,
        argCount,
        callee,
        newTarget);
    frame_.getThisArgRef() = thisArg;
#ifdef HERMES_SLOW_DEBUG
    // Poison the initial arguments to ensure the caller sets all of them before
    // a GC.
    assert(!overflowed_ && "Overflow should return early");
    overflowHasBeenChecked_ = true;
    fillArguments(argCount, HermesValue::encodeInvalidValue());
    // We still want the user to check for overflow.
    overflowHasBeenChecked_ = false;
#endif
  }

  /// Construct a native call frame for the given \p runtime in preparation of
  /// calling \p callee with \p argCount arguments and the given \p thisArg. On
  /// overflow, the overflowed() flag is set, in which case the stack frame must
  /// not be used.
  /// The arguments are initially uninitialized. The caller should initialize
  /// them by storing into them, or via fillArguments().
  ScopedNativeCallFrame(
      Runtime &runtime,
      uint32_t argCount,
      Callable *callee,
      bool construct,
      HermesValue thisArg)
      : ScopedNativeCallFrame(
            runtime,
            argCount,
            HermesValue::encodeObjectValue(callee),
            construct ? HermesValue::encodeObjectValue(callee)
                      : HermesValue::encodeUndefinedValue(),
            thisArg) {}

  ~ScopedNativeCallFrame() {
    // Note that we unconditionally increment the native call frame depth and
    // save the SP to avoid branching in the dtor.
#ifndef HERMES_CHECK_NATIVE_STACK
    runtime_.overflowGuard_.callDepth--;
#endif
    runtime_.popToSavedStackPointer(savedSP_);
#ifndef NDEBUG
    // Clear the frame to detect use-after-free.
    frame_ = StackFramePtr{};
#endif
  }

  /// Fill \p argCount arguments with the given value \p fillValue.
  void fillArguments(uint32_t argCount, HermesValue fillValue) {
    assert(overflowHasBeenChecked_ && "ScopedNativeCallFrame could overflow");
    assert(argCount == frame_.getArgCount() && "Arg count mismatch.");
    std::uninitialized_fill_n(frame_.argsBegin(), argCount, fillValue);
  }

  /// \return whether the stack frame overflowed.
  bool overflowed() const {
#ifndef NDEBUG
    overflowHasBeenChecked_ = !overflowed_;
#endif
    return overflowed_;
  }

  /// Access the stack frame contents via ->.
  StackFramePtr operator->() {
    assert(overflowHasBeenChecked_ && "ScopedNativeCallFrame could overflow");
    return frame_;
  }
};

#ifdef NDEBUG

/// NoAllocScope and NoHandleScope have no impact in release mode (except that
/// if they are embedded into another struct/class, they will still use space!)
class NoAllocScope {
 public:
  explicit NoAllocScope(Runtime &runtime) {}
  explicit NoAllocScope(GC &gc) {}
  NoAllocScope(const NoAllocScope &) = default;
  NoAllocScope(NoAllocScope &&) = default;
  NoAllocScope &operator=(const NoAllocScope &) = default;
  NoAllocScope &operator=(NoAllocScope &&rhs) = default;
  void release() {}

 private:
  NoAllocScope() = delete;
};
using NoHandleScope = NoAllocScope;
using NoRJSScope = NoAllocScope;

class NoLeakHandleScope {
 public:
  explicit NoLeakHandleScope(Runtime &runtime) {}

  NoLeakHandleScope(const NoLeakHandleScope &) = delete;
  NoLeakHandleScope &operator=(const NoLeakHandleScope &) = delete;
  NoLeakHandleScope(NoLeakHandleScope &&) = delete;
  NoLeakHandleScope &operator=(NoLeakHandleScope &&) = delete;
};

struct NoMutatorScope {
  explicit NoMutatorScope([[maybe_unused]] vm::Runtime &runtime) {}
  NoMutatorScope(const NoMutatorScope &) = delete;
  NoMutatorScope &operator=(const NoMutatorScope &) = delete;
  NoMutatorScope(NoMutatorScope &&) = delete;
  NoMutatorScope &operator=(NoMutatorScope &&) = delete;
};
#else

/// RAII class to temporarily disallow allocation of something.
class BaseNoScope {
 public:
  explicit BaseNoScope(uint32_t *level) : level_(level) {
    assert(level_ && "constructing BaseNoScope with moved/release object");
    ++*level_;
  }

  BaseNoScope(const BaseNoScope &other) : BaseNoScope(other.level_) {}
  BaseNoScope(BaseNoScope &&other) : level_(other.level_) {
    // not a release operation as this inherits the counter from other.
    other.level_ = nullptr;
  }

  ~BaseNoScope() {
    if (level_)
      release();
  }

  BaseNoScope &operator=(BaseNoScope &&rhs) {
    if (level_) {
      release();
    }

    // N.B.: to account for cases when this == &rhs, first copy rhs.level_
    // to a temporary, then null it out.
    auto ptr = rhs.level_;
    rhs.level_ = nullptr;
    level_ = ptr;
    return *this;
  }

  BaseNoScope &operator=(const BaseNoScope &other) {
    return *this = BaseNoScope(other.level_);
  }

  /// End this scope early. May only be called once.
  void release() {
    assert(level_ && "already released");
    assert(*level_ > 0 && "unbalanced no alloc");
    --*level_;
    level_ = nullptr;
  }

 protected:
  uint32_t *level_;

 private:
  BaseNoScope() = delete;
};

/// RAII class to temporarily disallow handle creation.
class NoHandleScope : public BaseNoScope {
 public:
  explicit NoHandleScope(Runtime &runtime)
      : BaseNoScope(&runtime.noHandleLevel_) {}
  using BaseNoScope::BaseNoScope;
  using BaseNoScope::operator=;
};

/// RAII class to temporarily disallow allocating cells in the JS heap.
class NoAllocScope : public BaseNoScope {
 public:
  explicit NoAllocScope(Runtime &runtime) : NoAllocScope(runtime.getHeap()) {}
  explicit NoAllocScope(GC &gc) : BaseNoScope(&gc.noAllocLevel_) {}
  using BaseNoScope::BaseNoScope;
  using BaseNoScope::operator=;
};

/// RAII class to temporarily disallow reentering JS execution.
class NoRJSScope : public BaseNoScope {
 public:
  explicit NoRJSScope(Runtime &runtime) : BaseNoScope(&runtime.noRJSLevel_) {}
  using BaseNoScope::BaseNoScope;
  using BaseNoScope::operator=;
};

/// RAII class that ensures all handles allocated during its lifetime are freed
/// before it is destroyed.
class NoLeakHandleScope {
  Runtime &runtime_;
  size_t handleCount_;

 public:
  explicit NoLeakHandleScope(Runtime &runtime)
      : runtime_(runtime),
        handleCount_(runtime_.getTopGCScope()->getHandleCountDbg()) {}
  ~NoLeakHandleScope() {
    assert(
        handleCount_ == runtime_.getTopGCScope()->getHandleCountDbg() &&
        "Handle leak detected");
  }

  NoLeakHandleScope(const NoLeakHandleScope &) = delete;
  NoLeakHandleScope &operator=(const NoLeakHandleScope &) = delete;
  NoLeakHandleScope(NoLeakHandleScope &&) = delete;
  NoLeakHandleScope &operator=(NoLeakHandleScope &&) = delete;
};

/// Asserts no allocation or JS execution when this scope is alive.
class NoMutatorScope {
 private:
  vm::NoAllocScope noAlloc_;
  vm::NoRJSScope noRJS_;

 public:
  explicit NoMutatorScope(vm::Runtime &runtime)
      : noAlloc_(runtime), noRJS_(runtime) {}
  ~NoMutatorScope() = default;

  NoMutatorScope(const NoMutatorScope &) = delete;
  NoMutatorScope &operator=(const NoMutatorScope &) = delete;
  NoMutatorScope(NoMutatorScope &&) = delete;
  NoMutatorScope &operator=(NoMutatorScope &&) = delete;
};
#endif

//===----------------------------------------------------------------------===//
// Runtime inline methods.

inline void Runtime::addCustomRootsFunction(
    std::function<void(GC *, RootAcceptor &)> markRootsFn) {
  customMarkRootFuncs_.emplace_back(std::move(markRootsFn));
}

inline void Runtime::addCustomWeakRootsFunction(
    std::function<void(GC *, WeakRootAcceptor &)> markRootsFn) {
  customMarkWeakRootFuncs_.emplace_back(std::move(markRootsFn));
}

inline void Runtime::addDrainJobsCallback(std::function<void()> callback) {
  drainJobsCallbacks_.emplace_back(std::move(callback));
}

inline void Runtime::addShutdownCallback(std::function<void()> callback) {
  shutdownCallbacks_.emplace_back(std::move(callback));
}

inline void Runtime::addPostShutdownDeleter(std::function<void()> deleter) {
  postShutdownDeleters_.emplace_back(std::move(deleter));
}

inline void Runtime::addCustomSnapshotFunction(
    std::function<void(HeapSnapshot &)> nodes,
    std::function<void(HeapSnapshot &)> edges) {
  customSnapshotNodeFuncs_.emplace_back(std::move(nodes));
  customSnapshotEdgeFuncs_.emplace_back(std::move(edges));
}

template <
    typename T,
    HasFinalizer hasFinalizer,
    LongLived longLived,
    class... Args>
T *Runtime::makeAFixed(Args &&...args) {
#ifndef NDEBUG
  // We always call getCurrentIP() in a debug build as this has the effect of
  // asserting the IP is correctly set (not invalidated) at this point. This
  // allows us to leverage our whole test-suite to find missing cases of
  // CAPTURE_IP* macros in the interpreter loop.
  (void)getCurrentIP();
#endif
  return getHeap().makeAFixed<T, hasFinalizer, longLived>(
      std::forward<Args>(args)...);
}

template <
    typename T,
    HasFinalizer hasFinalizer,
    LongLived longLived,
    CanBeLarge canBeLarge,
    MayFail mayFail,
    class... Args>
T *Runtime::makeAVariable(uint32_t size, Args &&...args) {
#ifndef NDEBUG
  // We always call getCurrentIP() in a debug build as this has the effect of
  // asserting the IP is correctly set (not invalidated) at this point. This
  // allows us to leverage our whole test-suite to find missing cases of
  // CAPTURE_IP* macros in the interpreter loop.
  (void)getCurrentIP();
#endif
  return getHeap()
      .makeAVariable<T, hasFinalizer, longLived, canBeLarge, mayFail>(
          size, std::forward<Args>(args)...);
}

template <typename T1, typename T2, typename... T1Args, typename... T2Args>
std::pair<T1 *, T2 *> Runtime::make2YoungGenUnsafe(
    uint32_t size1,
    std::tuple<T1Args...> t1Args,
    uint32_t size2,
    std::tuple<T2Args...> t2Args) {
#ifndef NDEBUG
  // We always call getCurrentIP() in a debug build as this has the effect of
  // asserting the IP is correctly set (not invalidated) at this point. This
  // allows us to leverage our whole test-suite to find missing cases of
  // CAPTURE_IP* macros in the interpreter loop.
  (void)getCurrentIP();
#endif
  return getHeap().make2YoungGenUnsafe<T1, T2>(size1, t1Args, size2, t2Args);
}

template <typename T>
inline T Runtime::ignoreAllocationFailure(CallResult<T> res) {
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    hermes_fatal("Unhandled out of memory exception");
  // Use std::move here to account for specializations of CallResult,
  // (in particular, CallResult<PseudoHandle<U>>)
  // which wrap a class with a deleted copy constructor.
  return std::move(res.getValue());
}

inline void Runtime::ignoreAllocationFailure(ExecutionStatus status) {
  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION))
    hermes_fatal("Unhandled out of memory exception");
}

inline void Runtime::ttiReached() {
  // Currently, only the heap_ behavior can change at TTI.
  getHeap().ttiReached();
}

template <class T>
inline Handle<T> Runtime::makeHandle(const GCPointer<T> &p) {
  return Handle<T>(*this, p.get(*this));
}

template <class T>
inline MutableHandle<T> Runtime::makeMutableHandle(const GCPointer<T> &p) {
  return MutableHandle<T>(*this, p.get(*this));
}

inline StringPrimitive *Runtime::getPredefinedString(
    Predefined::Str predefined) {
  return getStringPrimFromSymbolID(Predefined::getSymbolID(predefined));
}

inline StringPrimitive *Runtime::getPredefinedString(
    Predefined::Sym predefined) {
  return getStringPrimFromSymbolID(Predefined::getSymbolID(predefined));
}

inline Handle<StringPrimitive> Runtime::getPredefinedStringHandle(
    Predefined::Str predefined) {
  return makeHandle(getPredefinedString(predefined));
}

inline Handle<StringPrimitive> Runtime::getPredefinedStringHandle(
    Predefined::Sym predefined) {
  return makeHandle(getPredefinedString(predefined));
}

inline StringPrimitive *Runtime::getStringPrimFromSymbolID(SymbolID id) {
  return identifierTable_.getStringPrim(*this, id);
}

#ifdef HERMESVM_PROFILER_BB
inline BasicBlockExecutionInfo &Runtime::getBasicBlockExecutionInfo() {
  return basicBlockExecInfo_;
}

inline void Runtime::dumpBasicBlockProfileTrace(llvh::raw_ostream &OS) {
  basicBlockExecInfo_.dump(OS);
}
#endif

inline Runtime::FormatSymbolID Runtime::formatSymbolID(SymbolID id) {
  return FormatSymbolID(*this, id);
}

inline void Runtime::popToSavedStackPointer(PinnedHermesValue *toRestore) {
  assert(
      toRestore <= stackPointer &&
      "attempting to pop the stack to a higher level");
  stackPointer = toRestore;
}

inline uint32_t Runtime::getStackLevel() const {
  return (uint32_t)(toPHV(stackPointer) - toPHV(registerStackStart));
}

inline uint32_t Runtime::availableStackSize() const {
  return _sh_available_stack_size(this);
}

inline bool Runtime::checkAvailableStack(uint32_t count) {
  return _sh_check_available_stack(this, count);
}

inline PinnedHermesValue *Runtime::allocUninitializedStack(uint32_t count) {
  return toPHV(_sh_alloc_uninitialized_stack(this, count));
}

inline bool Runtime::checkAndAllocStack(uint32_t count) {
  return _sh_check_and_alloc_stack(this, count);
}

inline void Runtime::allocStack(uint32_t count) {
  auto *fillPtr = stackPointer;
  allocUninitializedStack(count);
  // Initialize the new registers.
  initStackWithZeroes(toPHV(fillPtr), count);
}

inline void Runtime::initStackWithZeroes(
    PinnedHermesValue *base,
    uint32_t count) {
  auto *lim = base + count;
  // Fill the first count % 3 elements so we can fill 4 at a time in the loop.
  if (count & 1)
    *(base++) = HermesValue::encodeRawZeroValueUnsafe();

  if (count & 2) {
    *(base++) = HermesValue::encodeRawZeroValueUnsafe();
    *(base++) = HermesValue::encodeRawZeroValueUnsafe();
  }

  // Store the remaining elements 4 at a time, allowing the compiler to use
  // vectorized stores for up to 4 elements at a time. Note that the empty asm
  // statement at the end of the loop prevents the compiler from storing more
  // elements together.
  while (base < lim) {
    *(base++) = HermesValue::encodeRawZeroValueUnsafe();
    *(base++) = HermesValue::encodeRawZeroValueUnsafe();
    *(base++) = HermesValue::encodeRawZeroValueUnsafe();
    *(base++) = HermesValue::encodeRawZeroValueUnsafe();

#ifdef __GNUC__
    // This empty asm statement tells the compiler that we want each write to
    // be observable, so it cannot turn the loop into a memset call.
    asm volatile("" : : : "memory");
#endif
  }
  assert(base == lim && "Fill failed");
}

inline void Runtime::popStack(uint32_t count) {
  assert(getStackLevel() >= count && "register stack underflow");
  stackPointer += count;
}

inline void Runtime::setCurrentFrame(StackFramePtr frame) {
  currentFrame = frame.ptr();
}

/// Set the current frame pointer to the current top of the stack and return
/// it.
/// \return the new value of the current frame pointer.
inline StackFramePtr Runtime::setCurrentFrameToTopOfStack() {
  currentFrame = stackPointer;
  return StackFramePtr(toPHV(stackPointer));
}

inline StackFramePtr Runtime::restoreStackAndPreviousFrame(
    StackFramePtr currentFrameArg) {
  assert(
      getCurrentFrame() == currentFrameArg &&
      "currentFrame parameter must match currentFrame_");
  stackPointer = currentFrameArg.ptr();
  currentFrame = currentFrameArg.getPreviousFramePointer();
  return getCurrentFrame();
}

inline StackFramePtr Runtime::restoreStackAndPreviousFrame() {
  return restoreStackAndPreviousFrame(getCurrentFrame());
}

inline llvh::iterator_range<StackFrameIterator> Runtime::getStackFrames() {
  return {
      StackFrameIterator{getCurrentFrame()},
      StackFrameIterator{toPHV(registerStackStart)}};
};

inline llvh::iterator_range<ConstStackFrameIterator> Runtime::getStackFrames()
    const {
  return {
      ConstStackFrameIterator{getCurrentFrame()},
      ConstStackFrameIterator{toPHV(registerStackStart)}};
};

inline StackOverflowGuard Runtime::getOverflowGuardForRegex() {
#ifdef HERMES_CHECK_NATIVE_STACK
  return StackOverflowGuard::nativeStackGuard(nativeStackGap_, overflowGuard);
#else
  StackOverflowGuard copy = overflowGuard_;
  // We should take into account the approximate difference in sizes between the
  // stack size of the interpreter vs the regex executor. The max call depth in
  // use here was calculated using call stack sizes of the interpreter. Since
  // the executor has a smaller stack size, it should be allowed to recurse more
  // than the interpreter could have. So we multiply the max allowed depth by
  // some number larger than 1.
  constexpr uint32_t kRegexMaxDepthMult = 5;
  copy.maxCallDepth *= kRegexMaxDepthMult;
  return copy;
#endif
}

inline bool Runtime::isStackOverflowing() {
#ifdef HERMES_CHECK_NATIVE_STACK
  return StackOverflowGuard::isOverflowing(&overflowGuard, nativeStackGap_);
#else
  return overflowGuard_.isOverflowing();
#endif
}

inline ExecutionStatus Runtime::setThrownValue(HermesValue value) {
  thrownValue_ = value;
  return ExecutionStatus::EXCEPTION;
}

inline void Runtime::clearThrownValue() {
  thrownValue_ = HermesValue::encodeEmptyValue();
}

inline CrashManager &Runtime::getCrashManager() {
  return *crashMgr_;
}

#if HERMESVM_SANITIZE_HANDLES == 0
inline void Runtime::potentiallyMoveHeap() {}
#endif

//===----------------------------------------------------------------------===//

/// Invoke the T constructor with the given args to construct the
/// object in the given memory mem.
template <typename T, typename... CtorArgs>
inline void constructInHeapObj(void *mem, CtorArgs... args) {
  new (mem) T(args...);
}

/// Serialize a SymbolID.
llvh::raw_ostream &operator<<(
    llvh::raw_ostream &OS,
    Runtime::FormatSymbolID format);

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_RUNTIME_H

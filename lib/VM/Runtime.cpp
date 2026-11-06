/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Support/StackOverflowGuard.h"
#define DEBUG_TYPE "vm"
#include "hermes/VM/Runtime.h"

#include "SerializationManagedValue.h"
#include "hermes/BCGen/HBC/BCProvider.h"
#include "hermes/BCGen/HBC/HBC.h"
#include "hermes/BCGen/HBC/SimpleBytecodeBuilder.h"
#include "hermes/FrontEndDefs/Builtins.h"
#include "hermes/Platform/Logging.h"
#include "hermes/Regex/Executor.h"
#include "hermes/Support/MemoryBuffer.h"
#include "hermes/Support/OSCompat.h"
#include "hermes/Support/PerfSection.h"
#include "hermes/VM/AlignedHeapSegment.h"
#include "hermes/VM/BuildMetadata.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/CodeBlock.h"
#include "hermes/VM/Domain.h"
#include "hermes/VM/FillerCell.h"
#include "hermes/VM/HeapRuntime.h"
#include "hermes/VM/HostModel.h"
#include "hermes/VM/IdentifierTable.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSCallableProxy.h"
#include "hermes/VM/JSError.h"
#include "hermes/VM/JSFinalizationRegistry.h"
#include "hermes/VM/JSLib.h"
#include "hermes/VM/JSLib/JSLibStorage.h"
#include "hermes/VM/JSMapImpl.h"
#include "hermes/VM/JSProxy.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/PredefinedStringIDs.h"
#include "hermes/VM/Profiler/CodeCoverageProfiler.h"
#include "hermes/VM/Profiler/SamplingProfiler.h"
#include "hermes/VM/StackFrame-inline.h"
#include "hermes/VM/StackTracesTree.h"
#include "hermes/VM/StaticHUtils.h"
#include "hermes/VM/StringView.h"

#ifdef HERMESVM_INTERNAL_JAVASCRIPT_NATIVE
#include "hermes/InternalJavaScript/internal_unit.h"
#else
#include "hermes/InternalJavaScript/InternalBytecode.h"
#endif

#include "llvh/ADT/Hashing.h"
#include "llvh/ADT/ScopeExit.h"
#include "llvh/Support/Debug.h"
#include "llvh/Support/raw_ostream.h"

#ifdef HERMESVM_PROFILER_BB
#include "hermes/VM/IterationKind.h"
#include "hermes/VM/JSArray.h"
#include "llvh/ADT/DenseMap.h"
#endif

#include <cstring>
#include <future>

#ifdef __EMSCRIPTEN__
/// Provide implementations with weak linkage that can serve as the default in a
/// wasm build, while allowing them to be overridden depending on the target.
/// Since Emscripten will effectively LTO, these should be inlined.
__attribute__((__weak__)) extern "C" bool test_wasm_host_timeout() {
  return false;
}
__attribute__((__weak__)) extern "C" bool test_and_clear_wasm_host_timeout() {
  return false;
}
#endif

namespace hermes {
namespace vm {

namespace {

/// The maximum number of registers that can be requested in a RuntimeConfig.
static constexpr uint32_t kMaxSupportedNumRegisters =
    UINT32_MAX / sizeof(PinnedHermesValue);

#ifdef HERMES_CHECK_NATIVE_STACK
/// The minimum stack gap allowed from RuntimeConfig.
static constexpr uint32_t kMinSupportedNativeStackGap =
#if LLVM_ADDRESS_SANITIZER_BUILD
    256 * 1024;
#else
    64 * 1024;
#endif
#endif

// Only track I/O for buffers > 64 kB (which excludes things like
// Runtime::generateSpecialRuntimeBytecode).
static constexpr size_t MIN_IO_TRACKING_SIZE = 64 * 1024;

static const Predefined::Str fixedPropCacheNames[(size_t)PropCacheID::_COUNT] =
    {
#define V(id, predef) predef,
        PROP_CACHE_IDS(V)
#undef V
};

} // namespace

// Minidumps include stack memory, not heap memory.  If we want to be
// able to inspect the Runtime object in a minidump, we can do that by
// arranging for it to be allocated on a stack.  No existing stack is
// a good candidate, so we achieve this by creating a thread just to
// hold the Runtime.
class Runtime::StackRuntime {
 public:
  StackRuntime(const vm::RuntimeConfig &runtimeConfig)
      : thread_(runtimeMemoryThread, this) {
    startup_.get_future().get();
    new (runtime_) Runtime(StorageProvider::mmapProvider(), runtimeConfig);
  }

  ~StackRuntime() {
    runtime_->~Runtime();
    shutdown_.set_value();
    thread_.join();
  }

  static std::shared_ptr<Runtime> create(const RuntimeConfig &runtimeConfig) {
    auto srt = std::make_shared<StackRuntime>(runtimeConfig);
    return std::shared_ptr<Runtime>(srt, srt->runtime_);
  }

 private:
  static void runtimeMemoryThread(StackRuntime *stack) {
    ::hermes::oscompat::set_thread_name("hermes-runtime-memorythread");
    std::aligned_storage<sizeof(Runtime)>::type rt;
    stack->runtime_ = reinterpret_cast<Runtime *>(&rt);
    stack->startup_.set_value();
    stack->shutdown_.get_future().get();
  }

  // The order here matters.
  // * Set up the promises
  // * Initialize runtime_ to null
  // * Start the thread which uses them
  // * Initialize runtime_ from that thread
  std::promise<void> startup_;
  std::promise<void> shutdown_;
  Runtime *runtime_{nullptr};
  std::thread thread_;
};

/* static */
std::shared_ptr<Runtime> Runtime::create(const RuntimeConfig &runtimeConfig) {
#if defined(HERMESVM_CONTIGUOUS_HEAP) && defined(HERMESVM_RUNTIME_ON_STACK)
#error Contiguous heap is not compatible with Runtime on stack.
#endif

#if defined(HERMESVM_CONTIGUOUS_HEAP)
  uint64_t maxHeapSize = runtimeConfig.getGCConfig().getMaxHeapSize();
  // Allow some extra segments for the runtime, and as a buffer for the GC.
  uint64_t providerSize = std::min<uint64_t>(
      1ULL << 32, maxHeapSize + FixedSizeHeapSegment::storageSize() * 4);
  std::shared_ptr<StorageProvider> sp =
      StorageProvider::contiguousVAProvider(providerSize);
  auto rt = HeapRuntime<Runtime>::create(sp);
  new (rt.get()) Runtime(std::move(sp), runtimeConfig);
  return rt;
#elif defined(HERMESVM_RUNTIME_ON_STACK)
  return StackRuntime::create(runtimeConfig);
#else
  return std::shared_ptr<Runtime>{
      new Runtime(StorageProvider::mmapProvider(), runtimeConfig)};
#endif
}

CallResult<PseudoHandle<>> Runtime::getNamed(
    Handle<JSObject> obj,
    PropCacheID id) {
  CompressedPointer clazzPtr{obj->getClassGCPtr()};
  auto *cacheEntry = &fixedReadPropCache_[static_cast<int>(id)];
  if (LLVM_LIKELY(cacheEntry->clazz == clazzPtr)) {
    // The slot is cached, so it is safe to use the Internal function.
    return createPseudoHandle(
        JSObject::getNamedSlotValueUnsafe(*obj, *this, cacheEntry->getSlot())
            .unboxToHV(*this));
  }
  auto sym = Predefined::getSymbolID(fixedPropCacheNames[static_cast<int>(id)]);
  NamedPropertyDescriptor desc;
  // Check writable and internalSetter flags since the cache slot is shared for
  // get/put.
  OptValue<bool> hasOwnProp =
      JSObject::tryGetOwnNamedDescriptorFast(*obj, *this, sym, desc);
  if (LLVM_LIKELY(hasOwnProp && *hasOwnProp) && !desc.flags.accessor &&
      desc.flags.writable && !desc.flags.internalSetter) {
    HiddenClass *clazz = vmcast<HiddenClass>(clazzPtr.getNonNull(*this));
    if (LLVM_LIKELY(
            !clazz->isDictionaryNoCache() &&
            desc.slot <= ReadPropertyCacheEntry::kMaxSlot)) {
      // Cache the class, id and property slot.
      cacheEntry->clazz = clazzPtr;
      cacheEntry->setSlot(desc.slot);
    }
    return JSObject::getNamedSlotValue(createPseudoHandle(*obj), *this, desc);
  }
  return JSObject::getNamed_RJS(obj, *this, sym);
}

ExecutionStatus Runtime::putNamedThrowOnError(
    Handle<JSObject> obj,
    PropCacheID id,
    SmallHermesValue shv) {
  CompressedPointer clazzPtr{obj->getClassGCPtr()};
  auto *cacheEntry = &fixedWritePropCache_[static_cast<int>(id)];
  if (LLVM_LIKELY(cacheEntry->clazz == clazzPtr)) {
    JSObject::setNamedSlotValueUnsafe(*obj, *this, cacheEntry->getSlot(), shv);
    return ExecutionStatus::RETURNED;
  }
  auto sym = Predefined::getSymbolID(fixedPropCacheNames[static_cast<int>(id)]);
  NamedPropertyDescriptor desc;
  OptValue<bool> hasOwnProp =
      JSObject::tryGetOwnNamedDescriptorFast(*obj, *this, sym, desc);
  if (LLVM_LIKELY(hasOwnProp && *hasOwnProp) && !desc.flags.accessor &&
      desc.flags.writable && !desc.flags.internalSetter) {
    HiddenClass *clazz = vmcast<HiddenClass>(clazzPtr.getNonNull(*this));
    if (LLVM_LIKELY(!clazz->isDictionaryNoCache()) &&
        LLVM_LIKELY(desc.slot <= WritePropertyCacheEntry::kMaxSlot)) {
      // Cache the class and property slot.
      assert(
          cacheEntry->getAddCacheIndex() == 0 &&
          "fixedWritePropCache_ has no corresponding add cache");
      cacheEntry->clazz = clazzPtr;
      cacheEntry->setSlot(desc.slot);
    }
    JSObject::setNamedSlotValueUnsafe(*obj, *this, desc.slot, shv);
    return ExecutionStatus::RETURNED;
  }
  Handle<> value = makeHandle(shv.unboxToHV(*this));
  return JSObject::putNamed_RJS(
             obj, *this, sym, value, PropOpFlags().plusThrowOnError())
      .getStatus();
}

RuntimeBase::RuntimeBase() {
#if defined(HERMESVM_COMPRESSED_POINTERS) && !defined(HERMESVM_CONTIGUOUS_HEAP)
  // Initialize the 0 entry in the segment map to be nullptr.
  segmentMap[0] = nullptr;
#endif

  // Zero-initialize the unit pointers.
  std::fill(std::begin(units), std::end(units), nullptr);

  shCurJmpBuf = nullptr;
  stackPointer = nullptr;
  currentFrame = nullptr;
  shLocals = nullptr;
  sh_overflow_guard_clear(&overflowGuard);
}

void RuntimeBase::registerHeapSegment(unsigned idx, void *lowLim) {
#if defined(HERMESVM_COMPRESSED_POINTERS) && !defined(HERMESVM_CONTIGUOUS_HEAP)
  char *bias =
      reinterpret_cast<char *>(lowLim) - (idx << AlignedHeapSegment::kLogSize);
  segmentMap[idx] = bias;
#endif
  // Ideally we need to assert that lowLim is the start address of the segment,
  // but the approach for computing segment start address does not work for
  // JumboHeapSegment.
  assert(
      (uintptr_t)(lowLim) % AlignedHeapSegment::kSegmentUnitSize == 0 &&
      "Segment start address should be aligned to kSegmentUnitSize");
  AlignedHeapSegment::setSegmentIndexFromStart(lowLim, idx);
}

Runtime::Runtime(
    std::shared_ptr<StorageProvider> provider,
    const RuntimeConfig &runtimeConfig)
    // The initial heap size can't be larger than the max.
    : // thrownValue_ is Empty when there is no value currently being thrown.
      thrownValue_(HermesValue::encodeEmptyValue()),
      enableEval(runtimeConfig.getEnableEval()),
      verifyEvalIR(runtimeConfig.getVerifyEvalIR()),
      optimizedEval(runtimeConfig.getOptimizedEval()),
      asyncBreakCheckInEval(runtimeConfig.getAsyncBreakCheckInEval()),
      test262(runtimeConfig.getTest262()),
      traceMode(runtimeConfig.getSynthTraceMode()),
      serializationValues_(
          runtimeConfig.getGCConfig().getOccupancyTarget(),
          0.5),
      gcCallbacksWrapper_(*this),
      heap_(
          gcCallbacksWrapper_,
          *this,
          runtimeConfig.getGCConfig(),
          runtimeConfig.getCrashMgr(),
          std::move(provider),
          runtimeConfig.getVMExperimentFlags()),
      jitContext_(runtimeConfig.getEnableJIT()),
      hasES6Proxy_(runtimeConfig.getES6Proxy()),
      hasAsyncGenerators_(runtimeConfig.getEnableAsyncGenerators()),
      hasES6BlockScoping_(runtimeConfig.getES6BlockScoping()),
      hasIntl_(runtimeConfig.getIntl()),
      hasMicrotaskQueue_(runtimeConfig.getMicrotaskQueue()),
      shouldRandomizeMemoryLayout_(runtimeConfig.getRandomizeMemoryLayout()),
      bytecodeWarmupPercent_(runtimeConfig.getBytecodeWarmupPercent()),
      trackIO_(runtimeConfig.getTrackIO()),
      vmExperimentFlags_(runtimeConfig.getVMExperimentFlags()),
      jsLibStorage_(createJSLibStorage()),
      crashMgr_(runtimeConfig.getCrashMgr()),
#ifdef HERMES_CHECK_NATIVE_STACK
      nativeStackGap_(
          std::max(
              runtimeConfig.getNativeStackGap(),
              kMinSupportedNativeStackGap)),
#else
      overflowGuard_(
          StackOverflowGuard::depthCounterGuard(
              Runtime::MAX_NATIVE_CALL_FRAME_DEPTH)),
#endif
      builtins_(
          static_cast<Callable **>(
              checkedCalloc(BuiltinMethod::_count, sizeof(Callable *)))),
      crashCallbackKey_(
          crashMgr_->registerCallback([this](int fd) { crashCallback(fd); })),
      codeCoverageProfiler_(std::make_unique<CodeCoverageProfiler>(*this)),
      gcEventCallback_(runtimeConfig.getGCConfig().getCallback()) {
  hermes_assert(
      runtimeConfig.getMicrotaskQueue(),
      "MicrotaskQueue must be enabled. Setting it to false is no longer supported.");
  assert(
      (void *)this == (void *)(PointerBase *)this &&
      "cast to PointerBase should be no-op");
  crashMgr_->registerMemory(this, sizeof(Runtime));
  auto maxNumRegisters = runtimeConfig.getMaxNumRegisters();
  if (LLVM_UNLIKELY(maxNumRegisters > kMaxSupportedNumRegisters)) {
    hermes_fatal("RuntimeConfig maxNumRegisters too big");
  }
  registerStackStart =
      static_cast<SHLegacyValue *>(runtimeConfig.getRegisterStack());
  if (!registerStackStart) {
    // registerStackAllocation_ should not be allocated with new, because then
    // default constructors would run for the whole stack space.
    // Round up to page size as required by vm_allocate.
    const uint32_t numBytesForRegisters = llvh::alignTo(
        sizeof(PinnedHermesValue) * maxNumRegisters, oscompat::page_size());
    auto result = oscompat::vm_allocate(numBytesForRegisters);
    if (!result) {
      hermes_fatal("Failed to allocate register stack", result.getError());
    }
    registerStackStart = static_cast<SHLegacyValue *>(result.get());
    registerStackAllocation_ = {
        toPHV(registerStackStart), numBytesForRegisters};
    crashMgr_->registerMemory(toPHV(registerStackStart), numBytesForRegisters);
  }

  registerStackEnd = registerStackStart + maxNumRegisters;
  if (shouldRandomizeMemoryLayout_) {
    const unsigned bytesOff = std::random_device()() % oscompat::page_size();
    registerStackStart += bytesOff / sizeof(SHLegacyValue);
    assert(
        registerStackEnd >= registerStackStart && "register stack too small");
  }
  stackPointer = registerStackStart;

  static_assert(
      sizeof(SHUnit::script_id) == sizeof(facebook::hermes::debugger::ScriptID),
      "ScriptID size mismatch");

  // Setup the "root" stack frame.
  setCurrentFrameToTopOfStack();
  // Allocate the "reserved" registers in the root frame.
  allocStack(StackFrameLayout::CalleeExtraRegistersAtStart);

  // Zero-initialize all SHRuntime roots before any allocation can trigger a GC
  // that would scan them.
#define SHRUNTIME_HV_FIELD(name) name = _sh_ljs_raw_zero_value_unsafe();
#include "hermes/VM/SHRuntimeHermesValueFields.def"

  // Initialize Predefined Strings.
  // This function does not do any allocations.
  initPredefinedStrings();
  // Initialize special code blocks pointing to their own runtime module.
  // specialCodeBlockRuntimeModule_ will be owned by runtimeModuleList_.
  RuntimeModuleFlags flags;
  flags.hidesEpilogue = true;
  specialCodeBlockDomain_ = Domain::create(*this).get();
  specialCodeBlockRuntimeModule_ = RuntimeModule::createUninitialized(
      *this, Handle<Domain>::vmcast(&specialCodeBlockDomain_), flags);
  assert(
      &runtimeModuleList_.back() == specialCodeBlockRuntimeModule_ &&
      "specialCodeBlockRuntimeModule_ not added to runtimeModuleList_");

  // At this point, allocations can begin, as all the roots are markable.

  // Initialize the pre-allocated character strings.
  initCharacterStrings();

  // Allocate a sentinel StringPrimitive for Symbol.description.  Symbols
  // created without a description, so Symbol(undefined), store this sentinel as
  // their description string.
  strForSymbolNoDescription = vmcast<StringPrimitive>(ignoreAllocationFailure(
      StringPrimitive::createLongLived(*this, ASCIIRef("", (size_t)0))));

  GCScope scope(*this);

  // Explicitly initialize the specialCodeBlockRuntimeModule_ without CJS
  // modules.
  specialCodeBlockRuntimeModule_->initializeWithoutCJSModulesMayAllocate(
      hbc::BCProviderFromBuffer::createBCProviderFromBuffer(
          generateSpecialRuntimeBytecode())
          .first);
  emptyCodeBlock_ = specialCodeBlockRuntimeModule_->getCodeBlockMayAllocate(0);
  returnThisCodeBlock_ =
      specialCodeBlockRuntimeModule_->getCodeBlockMayAllocate(1);

  initRootHiddenClasses();

  // Object.prototype needs a class before classJSObject can exist, since
  // classJSObject's parent is Object.prototype. Start it on a temporary root,
  // then move it to classJSObject's null-parent transition below, so it shares
  // a class with every other null-parent JSObject.
  {
    struct : public Locals {
      PinnedValue<HiddenClass> tmpRoot;
    } lv;
    LocalsRAII lraii{*this, &lv};
    lv.tmpRoot = HiddenClass::createRoot(
        *this, makeNullHandle<JSObject>(), ClassFlags{});
    objectPrototype =
        JSObject::create(*this, makeNullHandle<JSObject>(), lv.tmpRoot);
  }
  objectPrototypeRawPtr = *objectPrototype;

  // We want the prototype of 'global' to be an ordinary object,
  // with Object.prototype as the parent.
  // We don't need to use the full createRootHiddenClass in GlobalObject.cpp
  // because we know that JSObject has no overlap slots, simplifying things.
  static_assert(
      JSObject::numOverlapSlots<JSObject>() == 0,
      "must have no overlap slots for classJSObject");
  classJSObject = HiddenClass::createRoot(*this, objectPrototype, ClassFlags{});
  {
    // getHiddenClassForPrototype may allocate and move objectPrototype, so
    // dereference objectPrototype only after it returns.
    Handle<HiddenClass> nullParentClass =
        getHiddenClassForPrototype(nullptr, classJSObject);
    JSObject::setEmptyClassUnsafe(*objectPrototype, *this, *nullParentClass);
  }

  global_ =
      JSObject::create(*this, objectPrototype, classJSObject).getHermesValue();

  JSLibFlags jsLibFlags{};
  jsLibFlags.enableHermesInternal = runtimeConfig.getEnableHermesInternal();
  jsLibFlags.enableHermesInternalTestMethods =
      runtimeConfig.getEnableHermesInternalTestMethods();
  initGlobalObject(*this, jsLibFlags);

  // Once the global object has been initialized, populate native builtins to
  // the builtins table.
  initNativeBuiltins();

  symbolRegistry_.init(*this);

  codeCoverageProfiler_->disable();
  // FIXME: temporarily disable JIT for internal bytecode
  jitContext_.setEnabled(false);
  // Execute our internal bytecode.
  auto jsBuiltinsObj = runInternalJavaScript();
  jitContext_.setEnabled(runtimeConfig.getEnableJIT());
  jitContext_.setForceJIT(runtimeConfig.getForceJIT());
  jitContext_.setDefaultExecThreshold(runtimeConfig.getJITThreshold());
  jitContext_.setMemoryLimit(runtimeConfig.getJITMemoryLimit());
  jitContext_.setMaxRecompiles(
      std::min<uint32_t>(runtimeConfig.getJITMaxRecompiles(), 255));
  jitContext_.setRecompileDeclineThreshold(
      std::max<uint32_t>(runtimeConfig.getJITRecompileThreshold(), 1));
  codeCoverageProfiler_->restore();

  // Populate JS builtins returned from internal bytecode to the builtins table.
  initJSBuiltins(jsBuiltinsObj);

#if HERMESVM_SAMPLING_PROFILER_AVAILABLE
  if (runtimeConfig.getEnableSampleProfiling())
    samplingProfiler = SamplingProfiler::create(*this);
#endif // HERMESVM_SAMPLING_PROFILER_AVAILABLE

  LLVM_DEBUG(llvh::dbgs() << "Runtime initialized\n");
}

Runtime::~Runtime() {
#if HERMESVM_SAMPLING_PROFILER_AVAILABLE
  samplingProfiler.reset();
#endif // HERMESVM_SAMPLING_PROFILER_AVAILABLE

  // Run embedder shutdown callbacks while the heap is still functional.
  // Callbacks may allocate, call into JS, and trigger GC.
  for (auto &cb : shutdownCallbacks_)
    cb();
  shutdownCallbacks_.clear();

  getHeap().finalizeAll();

  // Heap is now quiesced; run deleters for embedder objects that had to
  // outlive finalizeAll (e.g., napi_env whose NativeState finalizers
  // were invoked above and called env->queuePendingFinalizer).
  for (auto &d : postShutdownDeleters_)
    d();
  postShutdownDeleters_.clear();
  // Remove inter-module dependencies so we can delete them in any order.
  for (auto &module : runtimeModuleList_) {
    module.prepareForDestruction();
  }
  // All RuntimeModules must be destroyed before the next assertion, to untrack
  // all native IDs related to it (e.g., CodeBlock).
  while (!runtimeModuleList_.empty()) {
    // Calling delete will automatically remove it from the list.
    delete &runtimeModuleList_.back();
  }
  // Now that all objects are finalized, there shouldn't be any native memory
  // keys left in the ID tracker for memory profiling. Assert that the only IDs
  // left are JS heap pointers.
  assert(
      !getHeap().getIDTracker().hasNativeIDs() &&
      "A pointer is left in the ID tracker that is from non-JS memory. "
      "Was untrackNative called?");
  crashMgr_->unregisterCallback(crashCallbackKey_);
  if (!registerStackAllocation_.empty()) {
    crashMgr_->unregisterMemory(registerStackAllocation_.data());
    oscompat::vm_free(
        registerStackAllocation_.data(), registerStackAllocation_.size());
  }

  assert(
      !formattingStackTrace_ &&
      "Runtime is being destroyed while exception is being formatted");

  for (auto *unit : units)
    if (unit)
      sh_unit_done(*this, unit);

  // Unwatch the runtime from the time limit monitor in case the latter still
  // has any references to this.
  if (timeLimitMonitor) {
    timeLimitMonitor->unwatchRuntime(*this);
  }

  crashMgr_->unregisterMemory(this);
}

/// A helper class used to measure the duration of GC marking different roots.
/// It accumulates the times in \c Runtime::markRootsPhaseTimes[] and \c
/// Runtime::totalMarkRootsTime.
class Runtime::MarkRootsPhaseTimer {
 public:
  MarkRootsPhaseTimer(Runtime &rt, RootAcceptor::Section section)
      : rt_(rt), section_(section), start_(std::chrono::steady_clock::now()) {
    if (static_cast<unsigned>(section) == 0) {
      // The first phase; record the start as the start of markRoots.
      rt_.startOfMarkRoots_ = start_;
    }
  }
  ~MarkRootsPhaseTimer() {
    auto tp = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = (tp - start_);
    start_ = tp;
    unsigned index = static_cast<unsigned>(section_);
    rt_.markRootsPhaseTimes_[index] += elapsed.count();
    if (index + 1 ==
        static_cast<unsigned>(RootAcceptor::Section::NumSections)) {
      std::chrono::duration<double> totalElapsed = (tp - rt_.startOfMarkRoots_);
      rt_.totalMarkRootsTime_ += totalElapsed.count();
    }
  }

 private:
  Runtime &rt_;
  RootAcceptor::Section section_;
  std::chrono::time_point<std::chrono::steady_clock> start_;
};

void Runtime::markRoots(RootAcceptorWithNames &acceptor, bool markLongLived) {
  // The body of markRoots should be sequence of blocks, each of which starts
  // with the declaration of an appropriate RootSection instance.
  {
    MarkRootsPhaseTimer timer(*this, RootAcceptor::Section::Registers);
    acceptor.beginRootSection(RootAcceptor::Section::Registers);
    for (auto *p = toPHV(registerStackStart), *e = getStackPointer(); p != e;
         ++p)
      acceptor.accept(*p);
    acceptor.endRootSection();
  }

  {
    MarkRootsPhaseTimer timer(*this, RootAcceptor::Section::Locals);
    acceptor.beginRootSection(RootAcceptor::Section::Locals);
    for (Locals *locals = vmLocals; locals; locals = locals->prev) {
      for (size_t i = 0, e = locals->numLocals; i < e; ++i) {
        auto &phv = locals->locals()[i];
#ifdef HERMESVM_BOXED_DOUBLES
        if (LLVM_UNLIKELY(phv.isRawHV32())) {
          PinnedSmallHermesValue sphv{
              SmallHermesValue::decodeFromHermesValue(phv)};
          // The encoded HV32 can't be null pointer.
          acceptor.accept(sphv);
          phv = sphv.encodeAsHermesValue();
        } else
#endif
        {
          acceptor.acceptNullable(phv);
        }
      }
    }
    acceptor.endRootSection();
  }

  {
    MarkRootsPhaseTimer timer(*this, RootAcceptor::Section::SHLocals);
    acceptor.beginRootSection(RootAcceptor::Section::SHLocals);
    for (auto *locals = shLocals; locals; locals = locals->prev) {
      for (auto *p = locals->locals, *e = p + locals->count; p != e; ++p) {
        acceptor.accept(*toPHV(p));
      }
    }
    acceptor.endRootSection();
  }

  {
    MarkRootsPhaseTimer timer(*this, RootAcceptor::Section::RuntimeFields);
    acceptor.beginRootSection(RootAcceptor::Section::RuntimeFields);
#define SHRUNTIME_HV_FIELD(name) acceptor.acceptNullable(*toPHV(&name));
#include "hermes/VM/SHRuntimeHermesValueFields.def"
#define RUNTIME_HV_FIELD(name, type) acceptor.acceptNullablePV(name);
#include "hermes/VM/RuntimeHermesValueFields.def"
#define RUNTIME_PHV_FIELD(name) acceptor.acceptNullable(name);
#include "hermes/VM/RuntimePHVFields.def"
    acceptor.acceptPtr(objectPrototypeRawPtr, "objectPrototype");
    acceptor.acceptPtr(functionPrototypeRawPtr, "functionPrototype");
    for (auto &rm : runtimeModuleList_)
      rm.markRoots(acceptor, markLongLived);
    jitContext_.markRoots(acceptor, markLongLived);
    acceptor.endRootSection();
  }

  {
    MarkRootsPhaseTimer timer(*this, RootAcceptor::Section::SHUnits);
    acceptor.beginRootSection(RootAcceptor::Section::SHUnits);
    for (auto *unit : units)
      if (unit)
        sh_unit_mark_roots(unit, acceptor, markLongLived);
    acceptor.endRootSection();
  }

  {
    MarkRootsPhaseTimer timer(*this, RootAcceptor::Section::CharStrings);
    acceptor.beginRootSection(RootAcceptor::Section::CharStrings);
    if (markLongLived) {
      for (auto &hv : charStrings_)
        acceptor.accept(hv);
    }
    acceptor.endRootSection();
  }

  {
    MarkRootsPhaseTimer timer(
        *this, RootAcceptor::Section::StringCycleCheckVisited);
    acceptor.beginRootSection(RootAcceptor::Section::StringCycleCheckVisited);
    for (auto *&ptr : stringCycleCheckVisited_)
      acceptor.acceptPtr(ptr);
    acceptor.endRootSection();
  }

  {
    MarkRootsPhaseTimer timer(*this, RootAcceptor::Section::Builtins);
    acceptor.beginRootSection(RootAcceptor::Section::Builtins);
    for (size_t i = 0; i < BuiltinMethod::_count; ++i)
      acceptor.acceptPtr(builtins_[i]);
    acceptor.endRootSection();
  }

  {
    MarkRootsPhaseTimer timer(*this, RootAcceptor::Section::Jobs);
    acceptor.beginRootSection(RootAcceptor::Section::Jobs);
    for (Callable *&f : jobQueue_)
      acceptor.acceptPtr(f);
    acceptor.endRootSection();
  }

  {
    MarkRootsPhaseTimer timer(*this, RootAcceptor::Section::IdentifierTable);
    if (markLongLived) {
#ifdef HERMES_MEMORY_INSTRUMENTATION
      // Need to add nodes before the root section, and edges during the root
      // section.
      acceptor.provideSnapshot([this](HeapSnapshot &snap) {
        identifierTable_.snapshotAddNodes(snap);
      });
#endif
      acceptor.beginRootSection(RootAcceptor::Section::IdentifierTable);
      identifierTable_.markIdentifiers(acceptor, getHeap());
#ifdef HERMES_MEMORY_INSTRUMENTATION
      acceptor.provideSnapshot([this](HeapSnapshot &snap) {
        identifierTable_.snapshotAddEdges(snap);
      });
#endif
      acceptor.endRootSection();
    }
  }

  {
    MarkRootsPhaseTimer timer(*this, RootAcceptor::Section::GCScopes);
    acceptor.beginRootSection(RootAcceptor::Section::GCScopes);
    markGCScopes(acceptor);
    acceptor.endRootSection();
  }

  {
    MarkRootsPhaseTimer timer(*this, RootAcceptor::Section::SymbolRegistry);
    acceptor.beginRootSection(RootAcceptor::Section::SymbolRegistry);
    symbolRegistry_.markRoots(acceptor);
    acceptor.endRootSection();
  }
#if HERMESVM_SAMPLING_PROFILER_AVAILABLE
  {
    MarkRootsPhaseTimer timer(*this, RootAcceptor::Section::SamplingProfiler);
    acceptor.beginRootSection(RootAcceptor::Section::SamplingProfiler);
    if (samplingProfiler) {
      samplingProfiler->markRoots(acceptor);
    }
    acceptor.endRootSection();
  }
#endif

  {
    MarkRootsPhaseTimer timer(
        *this, RootAcceptor::Section::CodeCoverageProfiler);
    acceptor.beginRootSection(RootAcceptor::Section::CodeCoverageProfiler);
    if (codeCoverageProfiler_) {
      codeCoverageProfiler_->markRoots(acceptor);
    }
    acceptor.endRootSection();
  }

  {
    MarkRootsPhaseTimer timer(*this, RootAcceptor::Section::Serialization);
    acceptor.beginRootSection(RootAcceptor::Section::Serialization);
    serializationValues_.forEach(
        [&acceptor](SerializationManagedValue &element) {
          acceptor.accept(element.value());
        });
    acceptor.endRootSection();
  }

  {
    MarkRootsPhaseTimer timer(*this, RootAcceptor::Section::Custom);
    // Define nodes before the root section starts.
    for (auto &fn : customSnapshotNodeFuncs_) {
      acceptor.provideSnapshot(fn);
    }
    acceptor.beginRootSection(RootAcceptor::Section::Custom);
    for (auto &fn : customMarkRootFuncs_)
      fn(&getHeap(), acceptor);
    // Define edges while inside the root section.
    for (auto &fn : customSnapshotEdgeFuncs_) {
      acceptor.provideSnapshot(fn);
    }
    acceptor.endRootSection();
  }
}

void Runtime::markWeakRoots(WeakRootAcceptor &acceptor, bool markLongLived) {
  MarkRootsPhaseTimer timer(*this, RootAcceptor::Section::WeakRefs);
  acceptor.beginRootSection(RootAcceptor::Section::WeakRefs);
  // Call this first so that it can remove RuntimeModules whose owning Domain is
  // dead from runtimeModuleList_, before marking long-lived WeakRoots in them.
  markDomainRefInRuntimeModules(acceptor);
  if (markLongLived) {
    for (auto &entry : fixedWritePropCache_) {
      acceptor.acceptWeak(entry.clazz);
    }
    for (auto &entry : fixedReadPropCache_) {
      assert(
          !entry.negMatchClazz &&
          "negMatchClazz is always zero for non-JS code property cache");
      acceptor.acceptWeak(entry.clazz);
    }
  }
  for (auto &registry : finalizationRegistries_) {
    acceptor.acceptWeak(registry);
  }
  for (auto &rm : runtimeModuleList_)
    rm.markWeakRoots(acceptor, markLongLived);
  for (SHUnit *unit : units)
    if (unit)
      sh_unit_mark_weak_roots(unit, acceptor, markLongLived);
  for (auto &fn : customMarkWeakRootFuncs_)
    fn(&getHeap(), acceptor);
  acceptor.endRootSection();
}

void Runtime::markDomainRefInRuntimeModules(WeakRootAcceptor &acceptor) {
  std::vector<RuntimeModule *> modulesToDelete;
  for (auto &rm : runtimeModuleList_) {
    rm.markDomainRef(acceptor);
    // If the owning domain is dead, store the RuntimeModule pointer for
    // destruction later.
    if (LLVM_UNLIKELY(rm.isOwningDomainDead())) {
      // Prepare these RuntimeModules for destruction so that we don't rely on
      // their relative order in runtimeModuleList_.
      rm.prepareForDestruction();
      modulesToDelete.push_back(&rm);
    }
  }

  // We need to destroy these RuntimeModules after we call
  // prepareForDestruction() on all of them, otherwise, it may cause
  // use-after-free when checking the ownership of a CodeBlock in the destructor
  // of a RuntimeModule (which may refer a CodeBlock that is owned and deleted
  // by another RuntimeModule).
  for (auto *rm : modulesToDelete) {
    // Calling delete will automatically remove it from the list.
    delete rm;
  }
}

void Runtime::markRootsForCompleteMarking(RootAcceptorWithNames &acceptor) {
#if HERMESVM_SAMPLING_PROFILER_AVAILABLE
  MarkRootsPhaseTimer timer(*this, RootAcceptor::Section::SamplingProfiler);
  acceptor.beginRootSection(RootAcceptor::Section::SamplingProfiler);
  if (samplingProfiler) {
    samplingProfiler->markRootsForCompleteMarking(acceptor);
  }
#endif // HERMESVM_SAMPLING_PROFILER_AVAILABLE
  acceptor.endRootSection();
}

void Runtime::visitIdentifiers(
    const std::function<void(SymbolID, const StringPrimitive *)> &acceptor) {
  identifierTable_.visitIdentifiers(acceptor);
}

std::string Runtime::convertSymbolToUTF8(SymbolID id) {
  return identifierTable_.convertSymbolToUTF8(id);
}

void Runtime::invalidateAllAddCacheEntries() {
  for (auto &rm : runtimeModuleList_) {
    for (size_t i = 0, e = rm.numAddCacheEntries(); i < e; ++i) {
      AddPropertyCacheEntry &entry = rm.getAddCacheEntry(i);
      entry.startClazz = CompressedPointer(nullptr);
    }
  }
}

void Runtime::printRuntimeGCStats(JSONEmitter &json) const {
  const unsigned kNumPhases =
      static_cast<unsigned>(RootAcceptor::Section::NumSections);
#define ROOT_SECTION(phase) "MarkRoots_" #phase,
  static const char *markRootsPhaseNames[kNumPhases] = {
#include "hermes/VM/RootSections.def"
  };
#undef ROOT_SECTION
  json.emitKey("runtime");
  json.openDict();
  json.emitKeyValue("totalMarkRootsTime", formatSecs(totalMarkRootsTime_).secs);
  for (unsigned phaseNum = 0; phaseNum < kNumPhases; phaseNum++) {
    json.emitKeyValue(
        std::string(markRootsPhaseNames[phaseNum]) + "Time",
        formatSecs(markRootsPhaseTimes_[phaseNum]).secs);
  }
  json.closeDict();
}

void Runtime::printHeapStats(llvh::raw_ostream &os) {
  getHeap().printAllCollectedStats(os);
#ifndef NDEBUG
  printArrayCensus(os);
#endif
}

void Runtime::getIOTrackingInfoJSON(llvh::raw_ostream &os) {
  JSONEmitter json(os);
  json.openArray();
  for (auto &module : getRuntimeModules()) {
    auto tracker = module.getBytecode()->getPageAccessTracker();
    if (tracker) {
      json.openDict();
      json.emitKeyValue("url", module.getSourceURL());
      json.emitKey("tracking_info");
      tracker->getJSONStats(json);
      json.closeDict();
    }
  }
  json.closeArray();
}

void Runtime::removeRuntimeModule(RuntimeModule *rm) {
#ifdef HERMES_ENABLE_DEBUGGER
  debugger_.willUnloadModule(rm);
#endif
  runtimeModuleList_.remove(*rm);
}

#ifndef NDEBUG
void Runtime::assertTopCodeBlockContainsIP(const inst::Inst *ip) const {
  // Check that if the topmost function is currently a JSFunction, then the IP
  // is in that function.
  if (getCurrentFrame() != StackFramePtr(toPHV(registerStackStart)))
    if (auto *codeBlock =
            StackFramePtr(toPHV(currentFrame)).getCalleeCodeBlock())
      assert(codeBlock->contains(ip) && "IP not in CodeBlock");
}

void Runtime::validateSavedIPBeforeCall() const {
  // If this is the first frame, there is nothing to save.
  if (getCurrentFrame() == StackFramePtr(toPHV(registerStackStart)))
    return;

  auto *codeBlock = getCurrentFrame().getCalleeCodeBlock();
  // If the current function is not bytecode, the IP does not need to be saved.
  if (!codeBlock)
    return;

  auto *ip = StackFramePtr(toPHV(stackPointer)).getSavedIP();
  // Validate that the IP is correct.
  assert(ip && "Saved IP is null");
  assert(codeBlock->contains(ip) && "Saved IP not in CodeBlock");
  assert(ip == getCurrentIP() && "IP mismatch");
}

void Runtime::printArrayCensus(llvh::raw_ostream &os) {
  if (!getHeap().shouldRecordGCStats())
    return;
  // Do array capacity histogram.
  // Map from array size to number of arrays that are that size.
  // Arrays includes ArrayStorage.
  std::map<std::pair<size_t, size_t>, std::pair<size_t, size_t>>
      arraySizeToCountAndWastedSlots;
  auto printTable = [&os](
                        const std::map<
                            std::pair<size_t, size_t>,
                            std::pair<size_t, size_t>>
                            &arraySizeToCountAndWastedSlots) {
    os << llvh::format(
        "%8s %8s %8s %10s %15s %15s %15s %20s %25s\n",
        (const char *)"Capacity",
        (const char *)"Sizeof",
        (const char *)"Count",
        (const char *)"Count %",
        (const char *)"Cum Count %",
        (const char *)"Bytes %",
        (const char *)"Cum Bytes %",
        (const char *)"Wasted Slots %",
        (const char *)"Cum Wasted Slots %");
    size_t totalBytes = 0;
    size_t totalCount = 0;
    size_t totalWastedSlots = 0;
    for (const auto &p : arraySizeToCountAndWastedSlots) {
      totalBytes += p.first.second * p.second.first;
      totalCount += p.second.first;
      totalWastedSlots += p.second.second;
    }
    size_t cumulativeBytes = 0;
    size_t cumulativeCount = 0;
    size_t cumulativeWastedSlots = 0;
    for (const auto &p : arraySizeToCountAndWastedSlots) {
      cumulativeBytes += p.first.second * p.second.first;
      cumulativeCount += p.second.first;
      cumulativeWastedSlots += p.second.second;
      os << llvh::format(
          "%8d %8d %8d %9.2f%% %14.2f%% %14.2f%% %14.2f%% %19.2f%% %24.2f%%\n",
          p.first.first,
          p.first.second,
          p.second.first,
          p.second.first * 100.0 / totalCount,
          cumulativeCount * 100.0 / totalCount,
          p.first.second * p.second.first * 100.0 / totalBytes,
          cumulativeBytes * 100.0 / totalBytes,
          totalWastedSlots ? p.second.second * 100.0 / totalWastedSlots : 100.0,
          totalWastedSlots ? cumulativeWastedSlots * 100.0 / totalWastedSlots
                           : 100.0);
    }
    os << "\n";
  };

  os << "Array Census for ArrayStorage:\n";
  getHeap().forAllObjs([&arraySizeToCountAndWastedSlots](GCCell *cell) {
    if (cell->getKind() == CellKind::ArrayStorageKind) {
      ArrayStorage *arr = vmcast<ArrayStorage>(cell);
      const auto key =
          std::make_pair(arr->capacity(), arr->getAllocatedSizeSlow());
      arraySizeToCountAndWastedSlots[key].first++;
      arraySizeToCountAndWastedSlots[key].second +=
          arr->capacity() - arr->size();
    }
  });
  if (arraySizeToCountAndWastedSlots.empty()) {
    os << "\tNo ArrayStorages\n\n";
  } else {
    printTable(arraySizeToCountAndWastedSlots);
  }

  os << "Array Census for JSArray:\n";
  arraySizeToCountAndWastedSlots.clear();
  getHeap().forAllObjs([&arraySizeToCountAndWastedSlots, this](GCCell *cell) {
    if (JSArray *arr = dyn_vmcast<JSArray>(cell)) {
      JSArray::StorageType *storage = arr->getIndexedStorageNullable(*this);
      const auto capacity = storage ? storage->capacity() : 0;
      const auto sz = storage ? storage->size() : 0;
      const auto key = std::make_pair(capacity, arr->getAllocatedSizeSlow());
      arraySizeToCountAndWastedSlots[key].first++;
      arraySizeToCountAndWastedSlots[key].second += capacity - sz;
    }
  });
  if (arraySizeToCountAndWastedSlots.empty()) {
    os << "\tNo JSArrays\n\n";
  } else {
    printTable(arraySizeToCountAndWastedSlots);
  }

  os << "\n";
}
#endif

unsigned Runtime::getSymbolsEnd() const {
  return identifierTable_.getSymbolsEnd();
}

void Runtime::freeSymbols(llvh::BitVector &markedSymbols) {
  identifierTable_.freeUnmarkedSymbols(markedSymbols, getHeap().getIDTracker());
}

#ifdef HERMES_SLOW_DEBUG
bool Runtime::isSymbolLive(SymbolID id) {
  return identifierTable_.isSymbolLive(id);
}

const void *Runtime::getStringForSymbol(SymbolID id) {
  return identifierTable_.getStringForSymbol(id);
}
#endif

size_t Runtime::mallocSize() const {
  size_t shSize = 0;
  for (const SHUnit *unit : units)
    if (unit)
      shSize += sh_unit_additional_memory_size(unit);

  // Register stack uses mmap and RuntimeModules are tracked by their owning
  // Domains. So this only considers IdentifierTable size.
  return shSize + sizeof(IdentifierTable) +
      identifierTable_.additionalMemorySize();
}

#if HERMESVM_SANITIZE_HANDLES != 0
void Runtime::potentiallyMoveHeap() {
  // Do a dummy allocation which could force a heap move if handle sanitization
  // is on.
  FillerCell::create(
      *this,
      std::max<size_t>(
          heapAlignSize(sizeof(FillerCell)), GC::minAllocationSize()));
}
#endif

LLVM_ATTRIBUTE_NOINLINE
static CallResult<HermesValue> interpretFunctionWithRandomStack(
    Runtime &runtime,
    CodeBlock *globalCode) {
  static void *volatile dummy;
  const unsigned amount = std::random_device()() % oscompat::page_size();
  // Prevent compiler from optimizing alloca away by assigning to volatile
  dummy = alloca(amount);
  (void)dummy;
  return runtime.interpretFunction(globalCode);
}

std::unique_ptr<Buffer> makeCompilationSourceBuffer(
    llvh::StringRef code,
    const hbc::CompileFlags &compileFlags) {
  // The compiled module retains its source buffer when compiling lazily or with
  // full debug info (see keepCompilationData in BCProviderFromSrc), so in those
  // cases the buffer must own its bytes: `code` may not outlive the module (for
  // example a transient eval or debugger command string). Otherwise the buffer
  // is dropped after compilation, so borrowing `code` is safe.
  if (compileFlags.lazy || compileFlags.debug) {
    return std::make_unique<OwnedMemoryBuffer>(
        llvh::MemoryBuffer::getMemBufferCopy(code));
  }
  return std::make_unique<OwnedMemoryBuffer>(
      llvh::MemoryBuffer::getMemBuffer(code));
}

CallResult<HermesValue> Runtime::run(
    llvh::StringRef code,
    llvh::StringRef sourceURL,
    const hbc::CompileFlags &compileFlags) {
  std::unique_ptr<hermes::Buffer> buffer =
      makeCompilationSourceBuffer(code, compileFlags);
  return run(std::move(buffer), sourceURL, compileFlags);
}

CallResult<HermesValue> Runtime::run(
    std::unique_ptr<hermes::Buffer> code,
    llvh::StringRef sourceURL,
    const hbc::CompileFlags &compileFlags) {
  std::unique_ptr<hbc::BCProvider> bytecode;
  {
    PerfSection loading("Loading new JavaScript code");
    loading.addArg("url", sourceURL);
    auto bytecode_err = hbc::createBCProviderFromSrc(
        std::move(code), sourceURL, /* sourceMap */ {}, compileFlags);
    if (!bytecode_err.first) {
      return raiseSyntaxError(TwineChar16(bytecode_err.second));
    }
    bytecode = std::move(bytecode_err.first);
  }

  PerfSection loading("Executing global function");
  RuntimeModuleFlags rmflags;
  if (bytecode->allowPersistent())
    rmflags.persistent = true;
  return runBytecode(
      std::move(bytecode), rmflags, sourceURL, makeNullHandle<Environment>());
}

CallResult<HermesValue> Runtime::runBytecode(
    std::shared_ptr<hbc::BCProvider> &&bytecode,
    RuntimeModuleFlags flags,
    llvh::StringRef sourceURL,
    Handle<Environment> environment,
    Handle<> thisArg,
    Handle<> newTarget) {
  clearThrownValue();

  auto globalFunctionIndex = bytecode->getGlobalFunctionIndex();

  if (bytecode->getBytecodeOptions().getStaticBuiltins() && !builtinsFrozen_) {
    if (assertBuiltinsUnmodified() == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    freezeBuiltins();
    assert(builtinsFrozen_ && "Builtins must be frozen by now.");
  }

  if (flags.persistent) {
    // Persistent flag can't be true if the BCProvider doesn't support it.
    if (LLVM_UNLIKELY(!bytecode->allowPersistent())) {
      const char *msg = "Cannot enable persistent mode for lazy compilation";
      hermesLog("Hermes", "%s", msg);
      hermes_fatal(msg);
    }

    persistentBCProviders_.push_back(bytecode);
    if (bytecodeWarmupPercent_ > 0) {
      // Start the warmup thread for this bytecode if it's a buffer.
      bytecode->startWarmup(bytecodeWarmupPercent_);
    }
    if (getVMExperimentFlags() & experiments::MAdviseRandom) {
      bytecode->madvise(oscompat::MAdvice::Random);
    } else if (getVMExperimentFlags() & experiments::MAdviseSequential) {
      bytecode->madvise(oscompat::MAdvice::Sequential);
    }
    if (getVMExperimentFlags() & experiments::VerifyBytecodeChecksum) {
      llvh::ArrayRef<uint8_t> buf = bytecode->getRawBuffer();
      // buf is empty for non-buffer providers
      if (!buf.empty()) {
        if (!hbc::BCProviderFromBuffer::bytecodeHashIsValid(buf)) {
          const char *msg = "Bytecode checksum verification failed";
          hermesLog("Hermes", "%s", msg);
          hermes_fatal(msg);
        }
      }
    }
  }
  // Only track I/O for buffers > 64 kB (which excludes things like
  // Runtime::generateSpecialRuntimeBytecode).
  if (flags.persistent && trackIO_ &&
      bytecode->getRawBuffer().size() > MIN_IO_TRACKING_SIZE) {
    bytecode->startPageAccessTracker();
    if (!bytecode->getPageAccessTracker()) {
      hermesLog(
          "Hermes",
          "Failed to start bytecode I/O instrumentation, "
          "maybe not supported on this platform.");
    }
  }

  GCScope scope(*this);

  Handle<Domain> domain = makeHandle(Domain::create(*this));

  auto runtimeModuleRes = RuntimeModule::create(
      *this, domain, allocateScriptId(), std::move(bytecode), flags, sourceURL);
  if (LLVM_UNLIKELY(runtimeModuleRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto runtimeModule = *runtimeModuleRes;
  auto globalCode = runtimeModule->getCodeBlockMayAllocate(globalFunctionIndex);

#ifdef HERMES_ENABLE_DEBUGGER
  // If the debugger is configured to pause on load, give it a chance to pause.
  getDebugger().willExecuteModule(runtimeModule, globalCode);
#endif

  if (runtimeModule->hasCJSModules()) {
    auto requireContext = RequireContext::create(
        *this, domain, getPredefinedStringHandle(Predefined::dotSlash));
    return runRequireCall(
        *this,
        requireContext,
        domain,
        *domain->getCJSModuleOffset(*this, domain->getCJSEntryModuleID()));
  } else if (runtimeModule->hasCJSModulesStatic()) {
    return runRequireCall(
        *this,
        makeNullHandle<RequireContext>(),
        domain,
        *domain->getCJSModuleOffset(*this, domain->getCJSEntryModuleID()));
  } else {
    // Create a JSFunction which will reference count the runtime module.
    // Note that its handle gets registered in the scope, so we don't need to
    // save it. Also note that environment will often be null here, except if
    // this is local eval.
    auto func = JSFunction::create(
        *this,
        domain,
        Handle<JSObject>::vmcast(&functionPrototype),
        environment,
        globalCode);

    ScopedNativeCallFrame newFrame{
        *this, 0, func.getHermesValue(), *newTarget, *thisArg};
    if (LLVM_UNLIKELY(newFrame.overflowed()))
      return raiseStackOverflow(StackOverflowKind::NativeStack);
    return shouldRandomizeMemoryLayout_
        ? interpretFunctionWithRandomStack(*this, globalCode)
        : interpretFunction(globalCode);
  }
}

ExecutionStatus Runtime::loadSegment(
    std::shared_ptr<hbc::BCProvider> &&bytecode,
    Handle<RequireContext> requireContext,
    RuntimeModuleFlags flags) {
  GCScopeMarkerRAII marker{*this};
  auto domain = makeHandle(RequireContext::getDomain(*this, *requireContext));

  if (LLVM_UNLIKELY(
          RuntimeModule::create(
              *this, domain, nextScriptId_++, std::move(bytecode), flags, "") ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return ExecutionStatus::RETURNED;
}

Handle<JSObject> Runtime::runInternalJavaScript() {
#ifdef HERMESVM_INTERNAL_JAVASCRIPT_NATIVE
  SHLegacyValue resOrExc;
  if (_sh_unit_init_guarded(
          getSHRuntime(*this), &sh_export_internal_unit, &resOrExc))
    return makeHandle<JSObject>(HermesValue::fromRaw(resOrExc.raw));
  hermes_fatal("Error evaluating internal unit.");
#else
  llvh::ArrayRef<uint8_t> module = getInternalBytecode();

#ifdef HERMES_ENABLE_DEBUGGER
  // If the debugger is enabled we need to be able to set breakpoints in
  // internal bytecode. Copy it into the heap to allow mutating it.
  // Copy module into internalBytecodeCopy_ and use that instead.
  internalBytecodeCopy_.reset(
      static_cast<uint8_t *>(checkedMalloc(module.size())));
  std::memcpy(internalBytecodeCopy_.get(), module.data(), module.size());
  module = {internalBytecodeCopy_.get(), module.size()};
#endif

  std::pair<std::unique_ptr<hbc::BCProvider>, std::string> bcResult =
      hbc::BCProviderFromBuffer::createBCProviderFromBuffer(
          std::make_unique<Buffer>(module.data(), module.size()));
  if (LLVM_UNLIKELY(!bcResult.first)) {
    hermes_fatal((llvh::Twine("Error running internal bytecode: ") +
                  bcResult.second.c_str())
                     .str());
  }
  // The bytes backing our buffer are immortal, so we can be persistent.
  RuntimeModuleFlags flags;
  flags.persistent = true;
  flags.hidesEpilogue = true;

  // Why are we setting funcsAreBuiltins conditionally?
  // Pay attention, because this will blow your mind. It turns out that JS
  // libraries routinely check whether certain functions are "built-in" by
  // running `foo.toString().includes("[native code]")` and change behavior in
  // significant ways. Some versions of React Native use the following logic:
  //
  // hasPromiseQueuedToJSVM =
  //    (HermesInternal?.hasPromise?.()
  //      && global?.HermesInternal?.useEngineQueue?.())
  //    ||
  //    (Promise && Promise.toString().includes("[native code]");
  //
  // So, even if the microtask queue is disabled, RN will incorrectly assume it
  // is enabled, because the Promise implementation is a built-in.
  flags.funcsAreBuiltins = hasMicrotaskQueue();

  auto res = runBytecode(
      std::move(bcResult.first),
      flags,
      /*sourceURL*/ "InternalBytecode.js",
      makeNullHandle<Environment>());
  // It is a fatal error for the internal bytecode to throw an exception.
  assert(
      res != ExecutionStatus::EXCEPTION && "Internal bytecode threw exception");
  assert(
      res->isObject() &&
      "Completion value of internal bytecode must be an object");

  return makeHandle<JSObject>(*res);
#endif
}

void Runtime::printException(llvh::raw_ostream &os, Handle<> valueHandle) {
  os << "Uncaught ";
  clearThrownValue();

  // Try to fetch the stack trace.
  CallResult<PseudoHandle<>> propRes{ExecutionStatus::EXCEPTION};
  if (auto objHandle = Handle<JSObject>::dyn_vmcast(valueHandle)) {
    if (LLVM_UNLIKELY(
            (propRes = JSObject::getNamed_RJS(
                 objHandle,
                 *this,
                 Predefined::getSymbolID(Predefined::stack))) ==
            ExecutionStatus::EXCEPTION)) {
      // Suppress prepareStackTrace using the recursion-breaking flag and retry.
      bool wasFormattingStackTrace = formattingStackTrace();
      if (LLVM_LIKELY(!wasFormattingStackTrace)) {
        setFormattingStackTrace(true);
      }
      const auto &guard =
          llvh::make_scope_exit([this, wasFormattingStackTrace]() {
            if (formattingStackTrace() != wasFormattingStackTrace) {
              setFormattingStackTrace(wasFormattingStackTrace);
            }
          });
      (void)guard;

      if (LLVM_UNLIKELY(
              (propRes = JSObject::getNamed_RJS(
                   objHandle,
                   *this,
                   Predefined::getSymbolID(Predefined::stack))) ==
              ExecutionStatus::EXCEPTION)) {
        os << "exception thrown while getting stack trace\n";
        return;
      }
    }
  }
  SmallU16String<32> tmp;
  if (LLVM_UNLIKELY(
          propRes == ExecutionStatus::EXCEPTION || (*propRes)->isUndefined())) {
    // If stack trace is unavailable, we just print error.toString.
    auto strRes = toString_RJS(*this, valueHandle);
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      os << "exception thrown in toString of original exception\n";
      return;
    }

    strRes->get()->appendUTF16String(tmp);
    os << tmp << "\n";
    return;
  }
  // stack trace is available, try to convert it to string.
  auto strRes = toString_RJS(*this, makeHandle(std::move(*propRes)));
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    os << "exception thrown in toString of stack trace\n";
    return;
  }
  PseudoHandle<StringPrimitive> str = std::move(*strRes);
  if (str->getStringLength() == 0) {
    str.invalidate();
    // If the final value is the empty string,
    // fall back to just printing the error.toString directly.
    auto errToStringRes = toString_RJS(*this, valueHandle);
    if (LLVM_UNLIKELY(errToStringRes == ExecutionStatus::EXCEPTION)) {
      os << "exception thrown in toString of original exception\n";
      return;
    }
    str = std::move(*errToStringRes);
  }
  str->appendUTF16String(tmp);
  os << tmp << "\n";
}

Handle<JSObject> Runtime::getGlobal() {
  return Handle<JSObject>::vmcast(toPHV(&global_));
}

std::vector<llvh::ArrayRef<uint8_t>> Runtime::getEpilogues() {
  std::vector<llvh::ArrayRef<uint8_t>> result;
  for (const auto &m : runtimeModuleList_) {
    if (!m.hidesEpilogue()) {
      result.push_back(m.getEpilogue());
    }
  }
  return result;
}

#ifdef HERMES_ENABLE_DEBUGGER

llvh::Optional<Runtime::StackFrameInfo> Runtime::stackFrameInfoByIndex(
    uint32_t frameIdx) const {
  // Locate the frame.
  auto frames = getStackFrames();
  auto it = frames.begin();
  for (; frameIdx && it != frames.end(); ++it, --frameIdx) {
  }
  if (it == frames.end())
    return llvh::None;

  StackFrameInfo info;
  info.frame = *it++;
  info.isGlobal = it == frames.end();
  return info;
}

/// Calculate and \return the offset between the location of the specified
/// frame and the start of the stack. This value increases with every nested
/// call.
uint32_t Runtime::calcFrameOffset(ConstStackFrameIterator it) const {
  assert(it != getStackFrames().end() && "invalid frame");
  return it->ptr() - toPHV(registerStackStart);
}

/// \return the offset between the location of the current frame and the
///   start of the stack. This value increases with every nested call.
uint32_t Runtime::getCurrentFrameOffset() const {
  return calcFrameOffset(getStackFrames().begin());
}

#endif

static ExecutionStatus
raisePlaceholder(Runtime &runtime, Handle<JSError> errorObj, Handle<> message) {
  JSError::recordStackTrace(errorObj, runtime);
  JSError::setMessage(errorObj, runtime, message);
  return runtime.setThrownValue(errorObj.getHermesValue());
}

/// A placeholder used to construct a Error Object that takes in a the specified
/// message.
static ExecutionStatus raisePlaceholder(
    Runtime &runtime,
    Handle<JSObject> prototype,
    Handle<> message) {
  GCScopeMarkerRAII gcScope{runtime};

  // Create the error object, initialize stack property and set message.
  auto errorObj = runtime.makeHandle(JSError::create(runtime, prototype));
  return raisePlaceholder(runtime, errorObj, message);
}

/// A placeholder used to construct a Error Object that takes in a const
/// message. A new StringPrimitive is created each time.
// TODO: Predefine each error message.
static ExecutionStatus raisePlaceholder(
    Runtime &runtime,
    Handle<JSObject> prototype,
    const TwineChar16 &msg) {
  // Since this happens unexpectedly and rarely, don't rely on the parent
  // GCScope.
  GCScope gcScope{runtime};

  SmallU16String<64> buf;
  msg.toVector(buf);

  auto strRes = StringPrimitive::create(runtime, buf);
  if (strRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto str = runtime.makeHandle<StringPrimitive>(*strRes);
  LLVM_DEBUG(llvh::errs() << buf.arrayRef() << "\n");
  return raisePlaceholder(runtime, prototype, str);
}

ExecutionStatus Runtime::raiseError(const TwineChar16 &msg) {
  return raisePlaceholder(
      *this, Handle<JSObject>::vmcast(&ErrorPrototype), msg);
}

ExecutionStatus Runtime::raiseTypeError(Handle<> message) {
  // Since this happens unexpectedly and rarely, don't rely on the parent
  // GCScope.
  GCScope gcScope{*this};
  return raisePlaceholder(
      *this, Handle<JSObject>::vmcast(&TypeErrorPrototype), message);
}

ExecutionStatus Runtime::raiseTypeErrorForValue(
    llvh::StringRef msg1,
    Handle<> value,
    llvh::StringRef msg2) {
  switch (value->getTag()) {
    case HermesValue::Tag::Object:
      return raiseTypeError(msg1 + TwineChar16("Object") + msg2);
    case HermesValue::Tag::Str:
      return raiseTypeError(
          msg1 + TwineChar16("'") + vmcast<StringPrimitive>(*value) + "'" +
          msg2);
    case HermesValue::Tag::BoolSymbol:
      if (value->isBool()) {
        if (value->getBool()) {
          return raiseTypeError(msg1 + TwineChar16("true") + msg2);
        } else {
          return raiseTypeError(msg1 + TwineChar16("false") + msg2);
        }
      }
      return raiseTypeError(
          msg1 + TwineChar16("Symbol(") +
          getStringPrimFromSymbolID(value->getSymbol()) + ")" + msg2);
    case HermesValue::Tag::UndefinedNull:
      if (value->isUndefined())
        return raiseTypeError(msg1 + TwineChar16("undefined") + msg2);
      else
        return raiseTypeError(msg1 + TwineChar16("null") + msg2);
    default:
      if (value->isNumber()) {
        char buf[hermes::NUMBER_TO_STRING_BUF_SIZE];
        llvh::StringRef str = hermes::numberToString(
            value->getNumber(), buf, hermes::NUMBER_TO_STRING_BUF_SIZE);
        return raiseTypeError(msg1 + TwineChar16(str) + msg2);
      }
  }
  return raiseTypeError(msg1 + TwineChar16("Value") + msg2);
}

ExecutionStatus Runtime::raiseTypeError(const TwineChar16 &msg) {
  return raisePlaceholder(
      *this, Handle<JSObject>::vmcast(&TypeErrorPrototype), msg);
}

ExecutionStatus Runtime::raiseSyntaxError(const TwineChar16 &msg) {
  return raisePlaceholder(
      *this, Handle<JSObject>::vmcast(&SyntaxErrorPrototype), msg);
}

ExecutionStatus Runtime::raiseRangeError(const TwineChar16 &msg) {
  return raisePlaceholder(
      *this, Handle<JSObject>::vmcast(&RangeErrorPrototype), msg);
}

ExecutionStatus Runtime::raiseReferenceError(const TwineChar16 &msg) {
  return raisePlaceholder(
      *this, Handle<JSObject>::vmcast(&ReferenceErrorPrototype), msg);
}

ExecutionStatus Runtime::raiseReferenceError(Handle<> message) {
  return raisePlaceholder(
      *this, Handle<JSObject>::vmcast(&ReferenceErrorPrototype), message);
}

ExecutionStatus Runtime::raiseURIError(const TwineChar16 &msg) {
  return raisePlaceholder(
      *this, Handle<JSObject>::vmcast(&URIErrorPrototype), msg);
}

ExecutionStatus Runtime::raiseStackOverflow(StackOverflowKind kind) {
  const char *msg;
  switch (kind) {
    case StackOverflowKind::JSRegisterStack:
      msg = "Maximum call stack size exceeded";
      break;
    case StackOverflowKind::NativeStack:
      msg = "Maximum call stack size exceeded (native stack depth)";
      break;
    case StackOverflowKind::JSONParser:
      msg = "Maximum nesting level in JSON parser exceeded";
      break;
    case StackOverflowKind::JSONStringify:
      msg = "Maximum nesting level in JSON stringifyer exceeded";
      break;
  }
  return raisePlaceholder(
      *this, Handle<JSObject>::vmcast(&RangeErrorPrototype), msg);
}

ExecutionStatus Runtime::raiseQuitError() {
  return raiseUncatchableError(
      Handle<JSObject>::vmcast(&QuitErrorPrototype), "Quit");
}

ExecutionStatus Runtime::raiseTimeoutError() {
  return raiseUncatchableError(
      Handle<JSObject>::vmcast(&TimeoutErrorPrototype),
      "Javascript execution has timed out.");
}

ExecutionStatus Runtime::raiseUncatchableError(
    Handle<JSObject> prototype,
    llvh::StringRef errMessage) {
  Handle<JSError> err =
      makeHandle(JSError::createUncatchable(*this, prototype));
  auto res = StringPrimitive::create(
      *this, llvh::ASCIIRef{errMessage.begin(), errMessage.end()});
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto str = makeHandle(*res);
  return raisePlaceholder(*this, err, str);
}

ExecutionStatus Runtime::raiseEvalUnsupported(llvh::StringRef code) {
  return raiseSyntaxError(
      TwineChar16("Parsing source code unsupported: ") + code.substr(0, 32));
}

bool Runtime::insertVisitedObject(JSObject *obj) {
  if (llvh::find(stringCycleCheckVisited_, obj) !=
      stringCycleCheckVisited_.end())
    return true;
  stringCycleCheckVisited_.push_back(obj);
  return false;
}

void Runtime::removeVisitedObject(JSObject *obj) {
  (void)obj;
  assert(
      stringCycleCheckVisited_.back() == obj && "string cycle stack corrupted");
  stringCycleCheckVisited_.pop_back();
}

std::unique_ptr<Buffer> Runtime::generateSpecialRuntimeBytecode() {
  hbc::SimpleBytecodeBuilder builder;
  {
    hbc::BytecodeInstructionGenerator bcGen;
    bcGen.emitLoadConstUndefined(0);
    bcGen.emitRet(0);
    builder.addFunction(1, 1, bcGen.acquireBytecode());
  }
  {
    hbc::BytecodeInstructionGenerator bcGen;
    bcGen.emitGetGlobalObject(0);
    bcGen.emitRet(0);
    builder.addFunction(1, 1, bcGen.acquireBytecode());
  }
  auto buffer = builder.generateBytecodeBuffer();
  assert(buffer->size() < MIN_IO_TRACKING_SIZE);
  return buffer;
}

void Runtime::initPredefinedStrings() {
  assert(!getTopGCScope() && "There shouldn't be any handles allocated yet");

  auto buffer = predefStringAndSymbolChars;
  auto propLengths = predefPropertyLengths;
  auto strLengths = predefStringLengths;
  auto symLengths = predefSymbolLengths;

  static const uint32_t hashes[] = {
#define STR(name, string) constexprHashString(string),
#include "hermes/VM/PredefinedStrings.def"
  };

  uint32_t offset = 0;
  uint32_t registered = 0;
  (void)registered;
  const uint32_t strCount = Predefined::NumStrings;
  const uint32_t symCount = Predefined::NumSymbols;
  identifierTable_.reserve(Predefined::_IPROP_AFTER_LAST + strCount + symCount);

  for (uint32_t idx = 0; idx < Predefined::_IPROP_AFTER_LAST; ++idx) {
    SymbolID sym = identifierTable_.createNotUniquedLazySymbol(
        ASCIIRef{&buffer[offset], propLengths[idx]});

    assert(sym == Predefined::getSymbolID((Predefined::IProp)registered++));
    (void)sym;

    offset += propLengths[idx];
  }

  assert(
      strCount == sizeof hashes / sizeof hashes[0] &&
      "Arrays should have same length");
  for (uint32_t idx = 0; idx < strCount; idx++) {
    SymbolID sym = identifierTable_.registerLazyIdentifier(
        *this, ASCIIRef{&buffer[offset], strLengths[idx]}, hashes[idx]);

    assert(sym == Predefined::getSymbolID((Predefined::Str)registered++));
    (void)sym;

    offset += strLengths[idx];
  }

  for (uint32_t idx = 0; idx < symCount; ++idx) {
    SymbolID sym = identifierTable_.createNotUniquedLazySymbol(
        ASCIIRef{&buffer[offset], symLengths[idx]});

    assert(sym == Predefined::getSymbolID((Predefined::Sym)registered++));
    (void)sym;

    offset += symLengths[idx];
  }

  assert(
      !getTopGCScope() &&
      "There shouldn't be any handles allocated during initializing the predefined strings");
}

void Runtime::initCharacterStrings() {
  GCScope gc(*this);
  auto marker = gc.createMarker();
  charStrings_.reserve(256);
  for (char16_t ch = 0; ch < 256; ++ch) {
    gc.flushToMarker(marker);
    charStrings_.push_back(allocateCharacterString(ch).getHermesValue());
  }
}

/// Initialize the root HiddenClasses for all the GCCells.
void Runtime::initRootHiddenClasses() {
  struct : public Locals {
    PinnedValue<HiddenClass> clazz;
  } lv;
  LocalsRAII lraii{*this, &lv};

  // For lazy objects, they should just use this empty HiddenClass until they
  // are actually populated.
  ClassFlags lazyFlags{};
  lazyFlags.lazyObject = 1;
  lv.clazz =
      HiddenClass::createRoot(*this, makeNullHandle<JSObject>(), lazyFlags);
  lazyObjectClass_ = lv.clazz;
}

Handle<StringPrimitive> Runtime::allocateCharacterString(char16_t ch) {
  // This can in theory throw when called out of initialization phase.
  // However there is only that many character strings and in practice this
  // is not a problem.  Note that we allocate these as "long-lived" objects,
  // so we don't have to scan the charStrings_ array in
  // young-generation collections.

  PinnedHermesValue strRes;
  if (LLVM_LIKELY(ch < 128)) {
    strRes = ignoreAllocationFailure(
        StringPrimitive::createLongLived(*this, ASCIIRef(ch)));
  } else {
    strRes = ignoreAllocationFailure(
        StringPrimitive::createLongLived(*this, UTF16Ref(ch)));
  }
  return makeHandle<StringPrimitive>(strRes);
}

Handle<StringPrimitive> Runtime::getCharacterString(char16_t ch) {
  if (LLVM_LIKELY(ch < 256))
    return Handle<StringPrimitive>::vmcast(&charStrings_[ch]);

  return makeHandle<StringPrimitive>(
      ignoreAllocationFailure(StringPrimitive::create(*this, UTF16Ref(ch))));
}

static constexpr uint16_t FROZEN_FLAG = 0x8000;
/// Store all object and symbol ids in a static table to conserve code size.
static const struct {
  /// The Predefined:: string id of the object. Or FROZEN_FLAG if it should be
  /// frozen.
  uint16_t object;
  uint16_t method;
#ifndef NDEBUG
  const char *name;
#endif
} publicNativeBuiltins[] = {

#ifndef NDEBUG
#define NORMAL_METHOD(object, method) \
  {(uint16_t)Predefined::object,      \
   (uint16_t)Predefined::method,      \
   #object "::" #method},
#define BUILTIN_METHOD(object, method)         \
  {(uint16_t)Predefined::object | FROZEN_FLAG, \
   (uint16_t)Predefined::method,               \
   #object "::" #method},
#else
#define NORMAL_METHOD(object, method) \
  {(uint16_t)Predefined::object, (uint16_t)Predefined::method},
#define BUILTIN_METHOD(object, method) \
  {(uint16_t)Predefined::object | FROZEN_FLAG, (uint16_t)Predefined::method},
#endif

#define PRIVATE_BUILTIN(name)
#define JS_BUILTIN(name)
#define NORMAL_OBJECT(object)

#include "hermes/FrontEndDefs/Builtins.def"
};

static_assert(
    sizeof(publicNativeBuiltins) / sizeof(publicNativeBuiltins[0]) ==
        BuiltinMethod::_firstPrivate,
    "builtin method table mismatch");

ExecutionStatus Runtime::forEachPublicNativeBuiltin(
    const std::function<ExecutionStatus(
        unsigned methodIndex,
        bool frozen,
        Predefined::Str objectName,
        Handle<JSObject> &object,
        SymbolID methodID)> &callback) {
  MutableHandle<JSObject> lastObject{*this};
  Predefined::Str lastObjectName = Predefined::_STRING_AFTER_LAST;

  for (unsigned methodIndex = 0; methodIndex < BuiltinMethod::_firstPrivate;
       ++methodIndex) {
    GCScopeMarkerRAII marker{*this};
    LLVM_DEBUG(llvh::dbgs() << publicNativeBuiltins[methodIndex].name << "\n");
    // Find the object first, if it changed.
    bool frozen = (publicNativeBuiltins[methodIndex].object & FROZEN_FLAG) != 0;
    auto objectName =
        (Predefined::Str)(publicNativeBuiltins[methodIndex].object &
                          ~FROZEN_FLAG);
    if (objectName != lastObjectName) {
      auto objectID = Predefined::getSymbolID(objectName);
      // Avoid running any JS here to avoid modifying the builtins while
      // iterating them.
      NamedPropertyDescriptor desc;
      // Check if the builtin is overridden.
      if (!JSObject::getOwnNamedDescriptor(
              getGlobal(), *this, objectID, desc)) {
        return raiseTypeError(
            TwineChar16{
                "Cannot execute a bytecode compiled with -fstatic-builtins: "} +
            getPredefinedString(objectName) + " was deleted");
      }
      // Doesn't run accessors.
      auto cr = JSObject::getNamedSlotValue(getGlobal(), *this, desc);
      if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      // This is known to not be PropertyAccessor, so check that casting it to
      // JSObject is allowed.
      if (LLVM_UNLIKELY(!vmisa<JSObject>(cr->get()))) {
        return raiseTypeError(
            TwineChar16{
                "Cannot execute a bytecode compiled with -fstatic-builtins: "} +
            getPredefinedString(objectName) + " is not an object");
      }

      lastObject = vmcast<JSObject>(cr->get());
      lastObjectName = objectName;
    }

    // Find the method.
    auto methodName = (Predefined::Str)publicNativeBuiltins[methodIndex].method;
    auto methodID = Predefined::getSymbolID(methodName);

    ExecutionStatus status =
        callback(methodIndex, frozen, objectName, lastObject, methodID);
    if (status != ExecutionStatus::RETURNED) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  return ExecutionStatus::RETURNED;
}

void Runtime::initNativeBuiltins() {
  GCScopeMarkerRAII gcScope{*this};

  (void)forEachPublicNativeBuiltin([this](
                                       unsigned methodIndex,
                                       bool /*frozen*/,
                                       Predefined::Str /* objectName */,
                                       Handle<JSObject> &currentObject,
                                       SymbolID methodID) {
    auto cr = JSObject::getNamed_RJS(currentObject, *this, methodID);
    assert(
        cr.getStatus() != ExecutionStatus::EXCEPTION &&
        "getNamed() of builtin method failed");
    assert(
        vmisa<NativeFunction>(cr->get()) &&
        "getNamed() of builtin method must be a NativeFunction");
    registerBuiltin(
        (BuiltinMethod::Enum)methodIndex, vmcast<NativeFunction>(cr->get()));
    return ExecutionStatus::RETURNED;
  });

  // Now add the private native builtins.
  createHermesBuiltins(*this);
#ifndef NDEBUG
  // Make sure native builtins are all defined.
  for (unsigned i = 0; i < BuiltinMethod::_firstJS; ++i) {
    assert(builtins_[i] && "native builtin not initialized");
  }
#endif
}

static const struct JSBuiltin {
  uint16_t symID, builtinIndex;
} jsBuiltins[] = {
#define NORMAL_OBJECT(object)
#define NORMAL_METHOD(object, method)
#define BUILTIN_METHOD(object, method)
#define PRIVATE_BUILTIN(name)
#define JS_BUILTIN(name) \
  {(uint16_t)Predefined::name, (uint16_t)BuiltinMethod::HermesBuiltin##_##name},
#include "hermes/FrontEndDefs/Builtins.def"
};

void Runtime::initJSBuiltins(Handle<JSObject> jsBuiltinsObj) {
  for (const JSBuiltin &jsBuiltin : jsBuiltins) {
    auto symID = jsBuiltin.symID;
    auto builtinIndex = jsBuiltin.builtinIndex;

    // Try to get the JS function from jsBuiltinsObj.
    auto getRes = JSObject::getNamed_RJS(
        jsBuiltinsObj, *this, Predefined::getSymbolID((Predefined::Str)symID));
    assert(
        getRes == ExecutionStatus::RETURNED &&
        !getRes.getValue()->isUndefined() && "Failed to get JS builtin.");

    Callable *jsFunc = vmcast<Callable>(getRes->getHermesValue());

    registerBuiltin((BuiltinMethod::Enum)builtinIndex, jsFunc);
  }
}

ExecutionStatus Runtime::assertBuiltinsUnmodified() {
  assert(!builtinsFrozen_ && "Builtins are already frozen.");
  GCScope gcScope(*this);
  NoRJSScope noRJS{*this};

  return forEachPublicNativeBuiltin([this](
                                        unsigned methodIndex,
                                        bool frozen,
                                        Predefined::Str objectName,
                                        Handle<JSObject> &currentObject,
                                        SymbolID methodID) {
    // Skip "normal" builtins, since they are not frozen.
    if (!frozen)
      return ExecutionStatus::RETURNED;

    // Avoid running any JS here to avoid modifying the builtins while iterating
    // them.
    NamedPropertyDescriptor desc;
    // Check if the builtin is overridden.
    // Need to check for flags which could result in JS execution.
    if (!JSObject::getOwnNamedDescriptor(
            currentObject, *this, methodID, desc) ||
        desc.flags.proxyObject || desc.flags.hostObject) {
      return raiseTypeError(
          TwineChar16{
              "Cannot execute a bytecode compiled with -fstatic-builtins: "} +
          getPredefinedString(objectName) + "." +
          getStringPrimFromSymbolID(methodID) + " has been modified");
    }
    auto cr = JSObject::getNamedSlotValue(currentObject, *this, desc);
    if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    auto currentBuiltin = dyn_vmcast<NativeFunction>(cr->get());
    if (!currentBuiltin || currentBuiltin != builtins_[methodIndex]) {
      return raiseTypeError(
          TwineChar16{
              "Cannot execute a bytecode compiled with -fstatic-builtins: "} +
          getPredefinedString(objectName) + "." +
          getStringPrimFromSymbolID(methodID) + " has been modified");
    }
    return ExecutionStatus::RETURNED;
  });
}

void Runtime::freezeBuiltins() {
  assert(!builtinsFrozen_ && "Builtins are already frozen.");
  GCScope gcScope{*this};

  // A list storing all the object ids that we will freeze on the global object
  // in the end.
  std::vector<SymbolID> objectList;
  // A list storing all the method ids on the same object that we will freeze on
  // each object.
  std::vector<SymbolID> methodList;

  // Masks for setting the property a static builtin and read-only.
  PropertyFlags clearFlags;
  clearFlags.configurable = 1;
  clearFlags.writable = 1;
  PropertyFlags setFlags;
  setFlags.staticBuiltin = 1;

  (void)forEachPublicNativeBuiltin(
      [this, &objectList, &methodList, &clearFlags, &setFlags](
          unsigned methodIndex,
          bool frozen,
          Predefined::Str objectName,
          Handle<JSObject> &currentObject,
          SymbolID methodID) {
        // Skip normal builtins.
        if (!frozen)
          return ExecutionStatus::RETURNED;

        methodList.push_back(methodID);
        // This is the last method on current object.
        if (methodIndex + 1 == BuiltinMethod::_publicCount ||
            objectName != publicNativeBuiltins[methodIndex + 1].object) {
          // Store the object id in the object set.
          SymbolID objectID = Predefined::getSymbolID(objectName);
          objectList.push_back(objectID);
          // Freeze all methods and mark them as static builtins on the current
          // object.
          JSObject::updatePropertyFlagsWithoutTransitions(
              currentObject,
              *this,
              clearFlags,
              setFlags,
              llvh::ArrayRef<SymbolID>(methodList));
          methodList.clear();
        }
        return ExecutionStatus::RETURNED;
      });

  // Freeze all builtin objects and mark them as static builtins on the global
  // object.
  JSObject::updatePropertyFlagsWithoutTransitions(
      getGlobal(),
      *this,
      clearFlags,
      setFlags,
      llvh::ArrayRef<SymbolID>(objectList));

  builtinsFrozen_ = true;
}

ExecutionStatus Runtime::drainJobs() {
  // Invoke registered callbacks before draining (e.g., NAPI
  // finalizer draining).
  for (auto &cb : drainJobsCallbacks_)
    cb();

  GCScope gcScope{*this};
  MutableHandle<Callable> job{*this};
  // Note that new jobs can be enqueued during the draining.
  while (!jobQueue_.empty()) {
    GCScopeMarkerRAII marker{gcScope};

    job = jobQueue_.front();
    jobQueue_.pop_front();

    // Jobs are guaranteed to behave as thunks.
    auto callRes =
        Callable::executeCall0(job, *this, Runtime::getUndefinedValue());

    // Early return to signal the caller. Note that the exceptional job has been
    // popped, so re-invocation would pick up from the next available job.
    if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  return ExecutionStatus::RETURNED;
}

ExecutionStatus Runtime::addToKeptObjects(Handle<> obj) {
  assert(
      canBeHeldWeakly(*this, *obj) &&
      "obj must hold an Object or non-registered Symbol");
  // Lazily create the map for keptObjects_
  if (*keptObjects_ == nullptr) {
    keptObjects_ = JSSet::create(*this, mapPrototype);

    if (LLVM_UNLIKELY(
            JSSet::initializeStorage(keptObjects_, *this) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  return JSSet::insert(keptObjects_, *this, obj);
}

void Runtime::clearKeptObjects() {
  keptObjects_ = nullptr;
}

void Runtime::addFinalizationRegistry(JSFinalizationRegistry *registry) {
  finalizationRegistries_.emplace_back(registry, *this);
}

ExecutionStatus Runtime::cleanUpFinalizationCallbacks() {
  struct : Locals {
    PinnedValue<JSFinalizationRegistry> registry;
  } lv;
  LocalsRAII lraii{*this, &lv};

  // Iterate each registry to run the callback (if it has dead registered
  // targets). This is conservative but still compliant with the spec. We don't
  // immediately enqueue a cleanup task when a registered target is dead,
  // instead, we iterate all registries in a single step at microtask
  // checkpoint.
  // Don't cache the size because cleanup() call in the loop may add new
  // entries. Use index-based iteration because adding new entries can
  // invalidate iterators.
  for (size_t i = 0; i < finalizationRegistries_.size();) {
    // Check if the pointer is null. This registry could have been freed by the
    // GC when running last cleanup.
    WeakRoot<JSFinalizationRegistry> &element = finalizationRegistries_[i];
    if (element) {
      auto *frPtr = element.get(*this, getHeap());
      lv.registry = frPtr;
      if (JSFinalizationRegistry::cleanup(lv.registry, *this) ==
          ExecutionStatus::EXCEPTION)
        return ExecutionStatus::EXCEPTION;
      ++i;
    } else {
      // Swap with the last element and pop it out.
      // We don't need a read barrier here because if the read weak root is
      // freed later by the GC, we will just skip it because it's dead and it's
      // spec compliant to do so.
      element = finalizationRegistries_.back().getNoBarrierUnsafe();
      finalizationRegistries_.pop_back();
    }
  }
  return ExecutionStatus::RETURNED;
}

uint64_t Runtime::gcStableHashHermesValue(HermesValue value) {
  switch (value.getTag()) {
    case HermesValue::Tag::Object: {
      return gcStableHashJSObject(vmcast<JSObject>(value));
    }
    case HermesValue::Tag::BigInt: {
      // For bigints, we hash the string content.
      auto bytes = vmcast<BigIntPrimitive>(value)->getRawDataCompact();
      return llvh::hash_combine_range(bytes.begin(), bytes.end());
    }
    case HermesValue::Tag::Str: {
      // For strings, we hash the string content.
      return vmcast<StringPrimitive>(value)->getOrComputeHash();
    }
    default:
      assert(!value.isPointer() && "Unhandled pointer type");
      if (value.isNumber()) {
        // We need to check for NaNs because they may differ in the sign bit,
        // but they should have the same hash value.
        if (LLVM_UNLIKELY(value.isNaN()))
          return llvh::hash_value(HermesValue::encodeNaNValue().getRaw());
        // To normalize -0 to 0.
        if (value.getNumber() == 0)
          return 0;
      }
      // For everything else, we just take advantage of HermesValue.
      return llvh::hash_value(value.getRaw());
  }
}

uint64_t Runtime::gcStableHashJSObject(JSObject *object) {
  // For objects, because pointers can move, we need a unique ID
  // that does not change for each object.
  return JSObject::getObjectID(vmcast<JSObject>(object), *this);
}

bool Runtime::symbolEqualsToStringPrim(SymbolID id, StringPrimitive *strPrim) {
  auto view = identifierTable_.getStringView(*this, id);
  return strPrim->equals(view);
}

void Runtime::dumpCallFrames(llvh::raw_ostream &OS) {
  OS << "== Call Frames ==\n";
  const PinnedHermesValue *next = getStackPointer();
  unsigned i = 0;
  for (StackFramePtr sf : getStackFrames()) {
    OS << i++ << " ";
    if (auto *closure = dyn_vmcast<Callable>(sf.getCalleeClosureOrCBRef())) {
      OS << cellKindStr(closure->getKind()) << " ";
    }
    if (auto *cb = sf.getCalleeCodeBlock()) {
      OS << formatSymbolID(cb->getNameMayAllocate()) << " ";
    }
    dumpStackFrame(sf, OS, next);
    next = sf.ptr();
  }
}

LLVM_ATTRIBUTE_NOINLINE
void Runtime::dumpCallFrames() {
  dumpCallFrames(llvh::errs());
}

/// Serialize a SymbolID.
llvh::raw_ostream &operator<<(
    llvh::raw_ostream &OS,
    Runtime::FormatSymbolID format) {
  if (!format.symbolID.isValid())
    return OS << "SymbolID(INVALID)";

  OS << "SymbolID("
     << (format.symbolID.isNotUniqued() ? "(External)" : "(Internal)")
     << format.symbolID.unsafeGetIndex() << " \"";

  OS << format.runtime.getIdentifierTable().convertSymbolToUTF8(
      format.symbolID);
  return OS << "\")";
}

/****************************************************************************
 * WARNING: This code is run after a crash. Avoid walking data structures,
 *          doing memory allocation, or using libc etc. as much as possible
 ****************************************************************************/
void Runtime::crashCallback(int fd) {
  llvh::raw_fd_ostream jsonStream(fd, false);
  JSONEmitter json(jsonStream);

  // Temporary buffer for pointers converted to strings. 20 bytes is enough,
  // since an 8 byte pointer is 16 characters, plus the "0x" and the null
  // terminator.
  char hexBuf[20];
  auto writeHex = [&hexBuf](void *ptr) {
    unsigned len = snprintf(hexBuf, sizeof(hexBuf), "%p", ptr);
    assert(len < sizeof(hexBuf) && "Need more chars than expected");
    return llvh::StringRef{hexBuf, len};
  };

  json.openDict();
  json.emitKeyValue("type", "runtime");
  json.emitKeyValue("address", writeHex(this));
  json.emitKeyValue(
      "registerStackAllocation", writeHex(registerStackAllocation_.data()));
  json.emitKeyValue("registerStackStart", writeHex(registerStackStart));
  json.emitKeyValue("registerStackPointer", writeHex(stackPointer));
  json.emitKeyValue("registerStackEnd", writeHex(registerStackEnd));
  json.emitKey("callstack");
  crashWriteCallStack(json);
  json.closeDict();
}

/****************************************************************************
 * WARNING: This code is run after a crash. Avoid walking data structures,
 *          doing memory allocation, or using libc etc. as much as possible
 ****************************************************************************/
void Runtime::crashWriteCallStack(JSONEmitter &json) {
  json.openArray();
  for (auto frame : getStackFrames()) {
    json.openDict();
    json.emitKeyValue(
        "StackFrameRegOffs",
        (uint32_t)(frame.ptr() - toPHV(registerStackStart)));
    auto codeBlock = frame.getSavedCodeBlock();
    if (codeBlock) {
      json.emitKeyValue("FunctionID", codeBlock->getFunctionID());
      auto bytecodeOffs = codeBlock->getOffsetOf(frame.getSavedIP());
      json.emitKeyValue("ByteCodeOffset", bytecodeOffs);
      auto blockSourceCode = codeBlock->getDebugSourceLocationsOffset();
      const RuntimeModule *runtimeModule = codeBlock->getRuntimeModule();
      if (blockSourceCode.hasValue()) {
        auto debugInfo = runtimeModule->getBytecode()->getDebugInfo();
        auto sourceLocation = debugInfo->getLocationForAddress(
            blockSourceCode.getValue(), bytecodeOffs);
        if (sourceLocation) {
          auto file =
              debugInfo->getUTF8FilenameByID(sourceLocation->filenameId);
          char buf[256];
          unsigned len = snprintf(
              buf,
              sizeof(buf),
              "%s:%d:%d",
              file.c_str(),
              sourceLocation->line,
              sourceLocation->column);
          // The length is either the return value of snprintf, or the buffer
          // size without the null terminator, whichever is smaller.
          llvh::StringRef str{buf, std::min<size_t>(len, sizeof(buf) - 1)};
          json.emitKeyValue("SourceLocation", str);
        }
      }
      uint32_t segmentID = runtimeModule->getBytecode()->getSegmentID();
      llvh::StringRef sourceURL = runtimeModule->getSourceURL();
      json.emitKeyValue("SegmentID", segmentID);
      json.emitKeyValue("SourceURL", sourceURL);
    } else {
      json.emitKeyValue("NativeCode", true);
    }
    json.closeDict(); // frame
  }
  json.closeArray(); // frames
}

std::string Runtime::getCallStackNoAlloc(const Inst *ip) {
  NoAllocScope noAlloc(*this);
  std::string res;

  /// Try adding the function name and source location into res for the native
  /// frame. Return false if no valid source location or calleeHV is not a
  /// NativeJSFunction.
  auto tryAddNativeFrame = [&res, this](
                               HermesValue calleeHV, const SHLocals *locals) {
    if (!locals || locals->src_location_idx == 0)
      return false;
    const SHUnit *unit = locals->unit;
    // If unit is nonnull then we have a valid location and can use the
    // fields in loc. Otherwise, it is an unknown location.
    if (!unit)
      return false;

    if (!vmisa<NativeJSFunction>(calleeHV))
      return false;
    auto *callee = vmcast<NativeJSFunction>(calleeHV);
    auto nameIndex = callee->getFunctionInfo()->name_index;
    assert(
        nameIndex < unit->num_symbols &&
        "out of bound access on symbols table");
    SymbolID nameSym = SymbolID::unsafeCreate(unit->symbols[nameIndex]);
    auto name = identifierTable_.convertSymbolToUTF8(nameSym);

    uint32_t curLocIdx = locals->src_location_idx;
    assert(
        curLocIdx < unit->source_locations_size &&
        "out of bound access on source locations table");
    SHSrcLoc curLoc = unit->source_locations[curLocIdx];
    assert(
        curLoc.filename_idx < unit->num_symbols &&
        "out of bound access on symbols table");
    auto fnameSym = SymbolID::unsafeCreate(unit->symbols[curLoc.filename_idx]);
    auto fname = identifierTable_.convertSymbolToUTF8(fnameSym);
    res += name + ": " + fname + ":" + std::to_string(curLoc.line) + ":" +
        std::to_string(curLoc.column) + "\n";

    return true;
  };

  // Note that the order of iteration is youngest (leaf) frame to oldest.
  for (auto frame : getStackFrames()) {
    auto codeBlock = frame->getCalleeCodeBlock();
    if (codeBlock) {
      res += codeBlock->getNameString();
      // Default to the function entrypoint, this
      // ensures source location is provided for leaf frame even
      // if ip is not available.
      const uint32_t bytecodeOffs =
          ip != nullptr ? codeBlock->getOffsetOf(ip) : 0;
      auto blockSourceCode = codeBlock->getDebugSourceLocationsOffset();
      if (blockSourceCode.hasValue()) {
        auto debugInfo =
            codeBlock->getRuntimeModule()->getBytecode()->getDebugInfo();
        auto sourceLocation = debugInfo->getLocationForAddress(
            blockSourceCode.getValue(), bytecodeOffs);
        if (sourceLocation) {
          auto file =
              debugInfo->getUTF8FilenameByID(sourceLocation->filenameId);
          res += ": " + file + ":" + std::to_string(sourceLocation->line) +
              ":" + std::to_string(sourceLocation->column);
        }
      }
      res += "\n";
    } else {
      if (!tryAddNativeFrame(
              frame.getCalleeClosureOrCBRef(), frame.getSHLocals())) {
        res += "<Native code>\n";
      }
    }
    // Get the ip of the caller frame -- which will then be correct for the
    // next iteration.
    ip = frame.getSavedIP();
  }
  return res;
}

void Runtime::onGCEvent(GCEventKind kind, const std::string &extraInfo) {
#if HERMESVM_SAMPLING_PROFILER_AVAILABLE
  if (samplingProfiler) {
    switch (kind) {
      case GCEventKind::CollectionStart:
        samplingProfiler->suspend(
            SamplingProfiler::SuspendFrameInfo::Kind::GC, extraInfo);
        break;
      case GCEventKind::CollectionEnd:
        samplingProfiler->resume();
        break;
      default:
        llvm_unreachable("unknown GCEventKind");
    }
  }
#endif // HERMESVM_SAMPLING_PROFILER_AVAILABLE
  if (gcEventCallback_) {
    gcEventCallback_(kind, extraInfo.c_str());
  }
}

ExecutionStatus Runtime::notifyTimeout() {
  // TODO: allow a vector of callbacks.
  return raiseTimeoutError();
}

#ifdef HERMES_MEMORY_INSTRUMENTATION

std::pair<const CodeBlock *, const inst::Inst *>
Runtime::getCurrentInterpreterLocation(const inst::Inst *ip) {
  assert(ip && "IP being null implies we're not currently in the interpreter.");
  auto callFrames = getStackFrames();
  const CodeBlock *codeBlock = nullptr;
  for (auto frameIt = callFrames.begin(); frameIt != callFrames.end();
       ++frameIt) {
    codeBlock = frameIt->getCalleeCodeBlock();
    if (codeBlock) {
      break;
    } else {
      ip = frameIt->getSavedIP();
    }
  }
  assert(codeBlock && "Could not find CodeBlock.");
  return {codeBlock, ip};
}

StackTracesTreeNode *Runtime::getCurrentStackTracesTreeNode(
    const inst::Inst *ip) {
  assert(stackTracesTree_ && "Runtime not configured to track alloc stacks");
  assert(
      (getHeap().getAllocationLocationTracker().isEnabled() ||
       getHeap().getSamplingAllocationTracker().isEnabled()) &&
      "AllocationLocationTracker not enabled");
  if (!ip) {
    return nullptr;
  }
  const CodeBlock *codeBlock;
  std::tie(codeBlock, ip) = getCurrentInterpreterLocation(ip);
  return stackTracesTree_->getStackTrace(*this, codeBlock, ip);
}

void Runtime::enableAllocationLocationTracker(
    std::function<void(
        uint64_t,
        std::chrono::microseconds,
        std::vector<GCBase::AllocationLocationTracker::HeapStatsUpdate>)>
        fragmentCallback) {
  if (!stackTracesTree_) {
    stackTracesTree_ = std::make_unique<StackTracesTree>();
  }
  stackTracesTree_->syncWithRuntimeStack(*this);
  getHeap().enableHeapProfiler(std::move(fragmentCallback));
}

void Runtime::disableAllocationLocationTracker(bool clearExistingTree) {
  getHeap().disableHeapProfiler();
  if (clearExistingTree) {
    stackTracesTree_.reset();
  }
}

void Runtime::enableSamplingHeapProfiler(
    size_t samplingInterval,
    int64_t seed) {
  if (!stackTracesTree_) {
    stackTracesTree_ = std::make_unique<StackTracesTree>();
  }
  stackTracesTree_->syncWithRuntimeStack(*this);
  getHeap().enableSamplingHeapProfiler(samplingInterval, seed);
}

void Runtime::disableSamplingHeapProfiler(llvh::raw_ostream &os) {
  getHeap().disableSamplingHeapProfiler(os);
  stackTracesTree_.reset();
}

void Runtime::popCallStackImpl() {
  assert(stackTracesTree_ && "Runtime not configured to track alloc stacks");
  stackTracesTree_->popCallStack();
}

void Runtime::pushCallStackImpl(
    const CodeBlock *codeBlock,
    const inst::Inst *ip) {
  assert(stackTracesTree_ && "Runtime not configured to track alloc stacks");
  stackTracesTree_->pushCallStack(*this, codeBlock, ip);
}

#endif // HERMES_MEMORY_INSTRUMENTATION

void ScopedNativeDepthReducer::staticAsserts() {
#ifdef HERMES_CHECK_NATIVE_STACK
  static_assert(
      kReducedNativeStackGap < kMinSupportedNativeStackGap,
      "kMinSupportedNativeStackGap too low, must be reduced in the reducer");
#endif
}

} // namespace vm
} // namespace hermes

#undef DEBUG_TYPE

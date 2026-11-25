/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_UNITTESTS_VMRUNTIME_TESTHELPERS_H
#define HERMES_UNITTESTS_VMRUNTIME_TESTHELPERS_H

#include "hermes/BCGen/HBC/BCProviderFromSrc.h"
#include "hermes/BCGen/HBC/SimpleBytecodeBuilder.h"
#include "hermes/Public/GCConfig.h"
#include "hermes/Public/JSOutOfMemoryError.h"
#include "hermes/Public/RuntimeConfig.h"
#include "hermes/VM/BigIntPrimitive.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/CodeBlock.h"
#include "hermes/VM/Domain.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/RuntimeModule-inline.h"
#include "hermes/VM/StorageProvider.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringRefUtils.h"

#include "gtest/gtest.h"

namespace hermes {
namespace vm {

// Initial and max heap size constants
static constexpr uint32_t kInitHeapSmall = 1 << 8;
static constexpr uint32_t kMaxHeapSmall = 1 << 11;
static constexpr uint32_t kInitHeapSize = 1 << 16;
static constexpr uint32_t kMaxHeapSize = 1 << 19;
static constexpr uint32_t kInitHeapLarge = 1 << 20;
static constexpr uint32_t kMaxHeapLarge = 1 << 24;
static constexpr uint32_t kExtremeHeapLarge = 1 << 28;

static const GCConfig::Builder kTestGCConfigBaseBuilder =
    GCConfig::Builder().withSanitizeConfig(
        vm::GCSanitizeConfig::Builder().withSanitizeRate(0.01).build());

static const GCConfig kTestGCConfigSmall =
    GCConfig::Builder(kTestGCConfigBaseBuilder)
        .withInitHeapSize(kInitHeapSmall)
        .withMaxHeapSize(kMaxHeapSmall)
        .build();

static const GCConfig::Builder kTestGCConfigBuilder =
    GCConfig::Builder(kTestGCConfigBaseBuilder)
        .withInitHeapSize(kInitHeapSize)
        .withMaxHeapSize(kMaxHeapSize);

static const GCConfig kTestGCConfig =
    GCConfig::Builder(kTestGCConfigBuilder).build();

static const GCConfig kTestGCConfigLarge =
    GCConfig::Builder(kTestGCConfigBuilder)
        .withInitHeapSize(kInitHeapLarge)
        .withMaxHeapSize(kMaxHeapLarge)
        .build();

static const GCConfig kTestGCConfigExtremeLarge =
    GCConfig::Builder(kTestGCConfigBuilder)
        .withInitHeapSize(kInitHeapLarge)
        .withMaxHeapSize(kExtremeHeapLarge)
        .build();

static const RuntimeConfig kTestRTConfigSmallHeap =
    RuntimeConfig::Builder().withGCConfig(kTestGCConfigSmall).build();

static const RuntimeConfig::Builder kTestRTConfigBuilder =
    RuntimeConfig::Builder().withGCConfig(kTestGCConfig);

static const RuntimeConfig kTestRTConfig =
    RuntimeConfig::Builder(kTestRTConfigBuilder).build();

static const RuntimeConfig kTestRTConfigLargeHeap =
    RuntimeConfig::Builder().withGCConfig(kTestGCConfigLarge).build();

static const RuntimeConfig kTestRTConfigExtremeLargeHeap =
    RuntimeConfig::Builder().withGCConfig(kTestGCConfigExtremeLarge).build();

template <typename T>
::testing::AssertionResult isException(
    Runtime &runtime,
    const CallResult<T> &res) {
  return isException(runtime, res.getStatus());
}

::testing::AssertionResult isException(
    Runtime &runtime,
    ExecutionStatus status);

/// A RuntimeTestFixture should be used by any test that requires a Runtime.
/// For different heap sizes, use the different subclasses.
///
/// Rooting values in a test body: declare one Locals at the top of the body
/// and hold values in PinnedValue fields (the fixture's \c runtime member is
/// a Runtime &):
/// \code
///   struct : public Locals {
///     PinnedValue<JSObject> obj;
///     PinnedValue<StringPrimitive> str;
///   } lv;
///   LocalsRAII lraii(runtime, &lv);
/// \endcode
/// The "...ForTest" helpers below take a MutableHandle<T> slot (e.g. a
/// PinnedValue<T> field of that Locals) to write their result into.
class RuntimeTestFixtureBase : public ::testing::Test {
  std::shared_ptr<Runtime> rt;

 protected:
  // Convenience accessor that points to rt.
  Runtime &runtime;

  RuntimeConfig rtConfig;

  /// Roots for the fixture's own values.
  struct FixtureLocals : public Locals {
    PinnedValue<Domain> domain;
  } fixtureLocals_;
  LocalsRAII fixtureLocalsRAII_;

  /// A fresh Domain, rooted in the fixture.
  Handle<Domain> domain;

  RuntimeTestFixtureBase(const RuntimeConfig &runtimeConfig)
      : rt(Runtime::create(runtimeConfig)),
        runtime(*rt),
        rtConfig(runtimeConfig),
        fixtureLocalsRAII_(runtime, &fixtureLocals_),
        domain(fixtureLocals_.domain) {
    fixtureLocals_.domain = Domain::create(runtime);
  }

  /// Can't copy due to internal pointer.
  RuntimeTestFixtureBase(const RuntimeTestFixtureBase &) = delete;

  template <typename T>
  ::testing::AssertionResult isException(const CallResult<T> &res) {
    return isException(res.getStatus());
  }

  ::testing::AssertionResult isException(ExecutionStatus status) {
    return ::hermes::vm::isException(runtime, status);
  }

  std::shared_ptr<Runtime> newRuntime() {
    return Runtime::create(rtConfig);
  }
};

class RuntimeTestFixture : public RuntimeTestFixtureBase {
 public:
  RuntimeTestFixture() : RuntimeTestFixtureBase(kTestRTConfig) {}
  RuntimeTestFixture(experiments::VMExperimentFlags flags)
      : RuntimeTestFixtureBase(
            RuntimeConfig::Builder()
                .withGCConfig(kTestGCConfig)
                .withVMExperimentFlags(flags)
                .build()) {}
};

class SmallHeapRuntimeTestFixture : public RuntimeTestFixtureBase {
 public:
  SmallHeapRuntimeTestFixture()
      : RuntimeTestFixtureBase(kTestRTConfigSmallHeap) {}
};

class LargeHeapRuntimeTestFixture : public RuntimeTestFixtureBase {
 public:
  LargeHeapRuntimeTestFixture()
      : RuntimeTestFixtureBase(kTestRTConfigLargeHeap) {}
};

class ExtremeLargeHeapRuntimeTestFixture : public RuntimeTestFixtureBase {
 public:
  ExtremeLargeHeapRuntimeTestFixture()
      : RuntimeTestFixtureBase(kTestRTConfigExtremeLargeHeap) {}
};

/// Configuration for the GC which fixes a size -- \p sz -- for the heap, and
/// does not permit any growth.  Intended only for testing purposes where we
/// don't expect or want the heap to grow.
///
/// \p builder is an optional parameter representing an initial builder to set
///     size parameters upon, providing an opportunity to set other parameters.
inline const GCConfig TestGCConfigFixedSize(
    gcheapsize_t sz,
    GCConfig::Builder builder = kTestGCConfigBuilder) {
  return builder.withInitHeapSize(sz).withMaxHeapSize(sz).build();
}

/// Assert that execution of `x' didn't throw.
#define ASSERT_RETURNED(x) ASSERT_EQ(ExecutionStatus::RETURNED, x)

/// Expect that 'x' is a string primitive with value 'str'
#define EXPECT_STRINGPRIM(str, x)                               \
  do {                                                          \
    struct : public Locals {                                    \
      PinnedValue<> expectStrPrimX_;                            \
      PinnedValue<StringPrimitive> expectStrPrimStr_;           \
    } expectStrPrimLv_;                                         \
    LocalsRAII expectStrPrimLraii_(runtime, &expectStrPrimLv_); \
    expectStrPrimLv_.expectStrPrimX_ = (x);                     \
    Handle<StringPrimitive> strHandle = createNoThrowForTest(   \
        runtime, str, expectStrPrimLv_.expectStrPrimStr_);      \
    EXPECT_TRUE(isSameValue(                                    \
        strHandle.getHermesValue(),                             \
        expectStrPrimLv_.expectStrPrimX_.getHermesValue()));    \
  } while (0)

/// Assert that execution of 'x' didn't throw and returned the expected bool.
#define EXPECT_CALLRESULT_BOOL_RAW(B, x) \
  do {                                   \
    auto res = x;                        \
    ASSERT_RETURNED(res.getStatus());    \
    EXPECT_##B(*res);                    \
  } while (0)

/// Assert that execution of 'x' didn't throw and returned the expected bool.
#define EXPECT_CALLRESULT_BOOL(B, x)  \
  do {                                \
    auto res = x;                     \
    ASSERT_RETURNED(res.getStatus()); \
    EXPECT_##B((*res)->getBool());    \
  } while (0)

/// Assert that execution of 'x' didn't throw and returned the expected
/// CallResult.
// Will replace "EXPECT_RETURN_STRING" after the entire refactor.
#define EXPECT_CALLRESULT_STRING(str, x) \
  do {                                   \
    auto res = x;                        \
    ASSERT_RETURNED(res.getStatus());    \
    EXPECT_STRINGPRIM(str, res->get());  \
  } while (0)

/// Assert that execution of 'x' didn't throw and returned undefined.
#define EXPECT_CALLRESULT_UNDEFINED(x)  \
  do {                                  \
    auto res = x;                       \
    ASSERT_RETURNED(res.getStatus());   \
    EXPECT_TRUE((*res)->isUndefined()); \
  } while (0)

/// Assert that execution of 'x' didn't throw and returned the expected double.
#define EXPECT_CALLRESULT_DOUBLE(d, x) \
  do {                                 \
    auto res = x;                      \
    ASSERT_RETURNED(res.getStatus());  \
    EXPECT_EQ(d, (*res)->getDouble()); \
  } while (0)

/// Assert that execution of 'x' didn't throw and returned the expected double.
#define EXPECT_CALLRESULT_VALUE(v, x) \
  do {                                \
    auto res = x;                     \
    ASSERT_RETURNED(res.getStatus()); \
    EXPECT_EQ(v, res->get());         \
  } while (0)

/// Some tests expect out of memory.  This may either be fatal, or throw
/// exception; parameterize tests over this choice.
#ifdef HERMESVM_EXCEPTION_ON_OOM
#define EXPECT_OOM(exp)                    \
  {                                        \
    bool exThrown = false;                 \
    try {                                  \
      exp;                                 \
    } catch (const JSOutOfMemoryError &) { \
      exThrown = true;                     \
    }                                      \
    EXPECT_TRUE(exThrown);                 \
  }
#else
#define EXPECT_OOM(exp) EXPECT_DEATH_IF_SUPPORTED({ exp; }, "OOM")
#endif

/// Get a named value from an object.
#define GET_VALUE(objHandle, predefinedId)                  \
  do {                                                      \
    propRes = JSObject::getNamed_RJS(                       \
        objHandle,                                          \
        runtime,                                            \
        Predefined::getSymbolID(Predefined::predefinedId)); \
    ASSERT_RETURNED(propRes.getStatus());                   \
  } while (0)

/// Get the global object.
#define GET_GLOBAL(predefinedId) GET_VALUE(runtime.getGlobal(), predefinedId)

inline HermesValue operator""_hd(long double d) {
  return HermesValue::encodeTrustedNumberValue(d);
}

/// A minimal Runtime for GC tests.
/// Note that RuntimeBase must be the first base class to ensure that
/// PointerBase is at the start of the segment in contiguous heap mode.
class DummyRuntime final : public RuntimeBase, public HandleRootOwner {
 private:
  GCBase::GCCallbacksWrapper<DummyRuntime> gcCallbacksWrapper_;
  GC heap_;

 public:
  std::vector<WeakRoot<GCCell> *> weakRoots{};
  std::vector<WeakSmallHermesValue *> weakSHVs;
  /// Head of the locals list used for VM operations.
  Locals *vmLocals{};

  /// Create a DummyRuntime with the default parameters.
  static std::shared_ptr<DummyRuntime> create(
      const GCConfig &gcConfig,
      experiments::VMExperimentFlags vmExperimentFlags = experiments::Default);

  /// Use a custom storage provider and/or a custom crash manager.
  /// \param provider A pointer to a StorageProvider. It *must* use
  ///   StorageProvider::defaultProvider eventually or the test will fail.
  /// \param crashMgr
  static std::shared_ptr<DummyRuntime> create(
      const GCConfig &gcConfig,
      std::shared_ptr<StorageProvider> provider,
      std::shared_ptr<CrashManager> crashMgr =
          std::make_shared<NopCrashManager>(),
      experiments::VMExperimentFlags vmExperimentFlags = experiments::Default);

  /// Provide the correct storage provider based on build modes.
  /// All decorator StorageProviders must wrap the one returned from this
  /// function.
  static std::unique_ptr<StorageProvider> defaultProvider(
      uint64_t providerSize = 128 << 20);

  ~DummyRuntime();

  template <
      typename T,
      HasFinalizer hasFinalizer = HasFinalizer::No,
      LongLived longLived = LongLived::No,
      class... Args>
  T *makeAFixed(Args &&...args) {
    return getHeap().makeAFixed<T, hasFinalizer, longLived>(
        std::forward<Args>(args)...);
  }

  template <
      typename T,
      HasFinalizer hasFinalizer = HasFinalizer::No,
      LongLived longLived = LongLived::No,
      CanBeLarge canBeLarge = CanBeLarge::No,
      MayFail mayFail = MayFail::No,
      class... Args>
  T *makeAVariable(uint32_t size, Args &&...args) {
    return getHeap()
        .makeAVariable<T, hasFinalizer, longLived, canBeLarge, mayFail>(
            size, std::forward<Args>(args)...);
  }

  GC &getHeap() {
    return heap_;
  }

  void collect();

  void markRoots(RootAcceptorWithNames &acceptor, bool);

  void markWeakRoots(WeakRootAcceptor &weakAcceptor, bool);

  void markRootsForCompleteMarking(RootAcceptorWithNames &acceptor);

  unsigned int getSymbolsEnd() const {
    return 0;
  }

  void unmarkSymbols() {}

  void freeSymbols(const llvh::BitVector &) {}

#ifdef HERMES_SLOW_DEBUG
  bool isSymbolLive(SymbolID) {
    return true;
  }

  const void *getStringForSymbol(SymbolID) {
    return nullptr;
  }
#endif

  void printRuntimeGCStats(JSONEmitter &) const {}

  void visitIdentifiers(
      const std::function<void(SymbolID, const StringPrimitive *)> &) {}

  std::string convertSymbolToUTF8(SymbolID);

  std::string getCallStackNoAlloc() {
    return "<dummy runtime has no call stack>";
  }

  void onGCEvent(GCEventKind, const std::string &) {}

  /// It's a unit test, it doesn't care about reporting how much memory it uses.
  size_t mallocSize() const {
    return 0;
  }

  const inst::Inst *getCurrentIPSlow() const {
    return nullptr;
  }

#ifdef HERMES_MEMORY_INSTRUMENTATION
  StackTracesTreeNode *getCurrentStackTracesTreeNode(const inst::Inst *ip) {
    return nullptr;
  }

  StackTracesTree *getStackTracesTree() {
    return nullptr;
  }
#endif

 private:
  DummyRuntime(
      const GCConfig &gcConfig,
      std::shared_ptr<StorageProvider> storageProvider,
      std::shared_ptr<CrashManager> crashMgr,
      experiments::VMExperimentFlags vmExperimentFlags);
};

/// RAII class to push/pop a Locals struct within a scope. This is a duplicate
/// of LocalsRAII in Runtime.h.
class [[nodiscard]] DummyLocalsRAII {
  DummyRuntime &runtime_;
  Locals *locals_;

 public:
  template <typename T>
  explicit DummyLocalsRAII(DummyRuntime &runtime, T *locals)
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
  ~DummyLocalsRAII() {
    assert(
        runtime_.vmLocals == locals_ &&
        "LocalsRAII must be destroyed in the reverse order of creation");
    runtime_.vmLocals = locals_->prev;
  }
};

/// A DummyRuntimeTestFixtureBase should be used by any test that requires a
/// DummyRuntime. It takes a GCConfig, which can be
/// used to specify heap size using the constants i.e kInitHeapSize.
class DummyRuntimeTestFixtureBase : public ::testing::Test {
  std::shared_ptr<DummyRuntime> rt;

 protected:
  // Convenience accessor that points to rt.
  DummyRuntime &runtime;

  DummyRuntimeTestFixtureBase(const GCConfig &gcConfig)
      : rt(DummyRuntime::create(gcConfig)), runtime(*rt) {}

  /// Can't copy due to internal pointer.
  DummyRuntimeTestFixtureBase(const DummyRuntimeTestFixtureBase &) = delete;
};

// Provide HermesValue & wrappers comparison operators for convenience.

/// Compare two HermesValue for bit equality.
inline bool operator==(HermesValue a, HermesValue b) {
  return a.getRaw() == b.getRaw();
}

/// Helper function to create a CodeBlock that correspond to a single function
/// generated from \p BFG. The generated code block will be part of the \p
/// runtimeModule.
inline CodeBlock *createSimpleCodeBlock(
    RuntimeModule *runtimeModule,
    Runtime &,
    hbc::SimpleBytecodeBuilder &builder) {
  auto buffer = builder.generateBytecodeBuffer();
  runtimeModule->initializeWithoutCJSModulesMayAllocate(
      hbc::BCProviderFromBuffer::createBCProviderFromBuffer(std::move(buffer))
          .first);
  return runtimeModule->getCodeBlockMayAllocate(0);
}

/// Test-only convenience: uniqued SymbolID of \p str, written to the slot
/// \p out. Returns a Handle viewing \p out.
inline CallResult<Handle<SymbolID>> stringToSymbolIDForTest(
    Runtime &runtime,
    PseudoHandle<StringPrimitive> str,
    MutableHandle<SymbolID> out) {
  if (LLVM_UNLIKELY(
          stringToSymbolID(runtime, std::move(str), out) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  return Handle<SymbolID>(out);
}

/// Test-only convenience: SymbolID of \p value, written to the slot \p out.
/// Returns a Handle viewing \p out.
inline CallResult<Handle<SymbolID>> valueToSymbolIDForTest(
    Runtime &runtime,
    Handle<> value,
    MutableHandle<SymbolID> out) {
  if (LLVM_UNLIKELY(
          valueToSymbolID(runtime, value, out) == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  return Handle<SymbolID>(out);
}

/// Test-only convenience: intern \p str and write the SymbolID to the slot
/// \p out. Returns a Handle viewing \p out.
template <typename Ref>
inline CallResult<Handle<SymbolID>>
getSymbolHandleForTest(Runtime &runtime, Ref str, MutableHandle<SymbolID> out) {
  auto res = runtime.getIdentifierTable().getSymbolID(runtime, str);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  out.set(*res);
  return Handle<SymbolID>(out);
}

/// Test-only convenience: HiddenClass::addProperty writing the new class to
/// the slot \p out. Returns a Handle viewing \p out.
inline CallResult<std::pair<Handle<HiddenClass>, SlotIndex>> addPropertyForTest(
    Handle<HiddenClass> selfHandle,
    Runtime &runtime,
    SymbolID name,
    PropertyFlags propertyFlags,
    MutableHandle<HiddenClass> out) {
  auto res =
      HiddenClass::addProperty(selfHandle, runtime, name, propertyFlags, out);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  return std::make_pair(Handle<HiddenClass>(out), *res);
}

/// Test-only convenience: StringPrimitive::createNoThrow writing the string to
/// the slot \p out. Returns a Handle viewing \p out.
inline Handle<StringPrimitive> createNoThrowForTest(
    Runtime &runtime,
    UTF16Ref str,
    MutableHandle<StringPrimitive> out) {
  out = StringPrimitive::createNoThrow(runtime, str);
  return out;
}
inline Handle<StringPrimitive> createNoThrowForTest(
    Runtime &runtime,
    llvh::StringRef ascii,
    MutableHandle<StringPrimitive> out) {
  out = StringPrimitive::createNoThrow(runtime, ascii);
  return out;
}

/// Test-only convenience: a new BigIntPrimitive for a signed integer, written
/// to the slot \p out. Returns a Handle viewing \p out. Brings down the VM on
/// OOM.
template <typename T>
inline Handle<BigIntPrimitive> fromSignedNoThrowForTest(
    Runtime &runtime,
    T value,
    MutableHandle<BigIntPrimitive> out) {
  out.set(
      vmcast<BigIntPrimitive>(runtime.ignoreAllocationFailure(
          BigIntPrimitive::fromSigned(runtime, value))));
  return out;
}

/// Test-only convenience: a new BigIntPrimitive from \p bytes, written to the
/// slot \p out. Returns a Handle viewing \p out. Brings down the VM on OOM.
inline Handle<BigIntPrimitive> fromBytesNoThrowForTest(
    Runtime &runtime,
    llvh::ArrayRef<uint8_t> bytes,
    MutableHandle<BigIntPrimitive> out) {
  out.set(
      vmcast<BigIntPrimitive>(runtime.ignoreAllocationFailure(
          BigIntPrimitive::fromBytes(runtime, bytes))));
  return out;
}

} // namespace vm
} // namespace hermes

#endif // HERMES_UNITTESTS_VMRUNTIME_TESTHELPERS_H

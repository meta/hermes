/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TestHelpers1.h"
#include "VMRuntimeTestHelpers.h"

#include "../../lib/VM/SHUnitExt.h"
#include "hermes/BCGen/HBC/BCProvider.h"
#include "hermes/BCGen/HBC/HBC.h"
#include "hermes/Regex/RegexSerialization.h"
#include "hermes/Support/MemoryBuffer.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/GCConcurrency.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSRegExp.h"
#include "hermes/VM/RegExpBytecode.h"
#include "hermes/VM/RuntimeModule.h"
#include "hermes/VM/StaticHUtils.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringRefUtils.h"
#include "hermes/VM/static_h.h"

#include "gtest/gtest.h"

#include "llvh/Support/MemoryBuffer.h"

#include <cstddef>
#include <memory>
#include <thread>

using namespace hermes::vm;

namespace hermes {
namespace unittest {
namespace regexpbytecodetest {
namespace {

/// Runs every test with handle sanitization at every allocation. On a
/// HERMESVM_SANITIZE_HANDLES build with MallocGC, each allocation then
/// moves the heap (Hades instead collects the young generation); without
/// HERMESVM_SANITIZE_HANDLES the rate has no effect.
class RegExpBytecodeTest : public RuntimeTestFixtureBase {
 public:
  RegExpBytecodeTest()
      : RuntimeTestFixtureBase(
            RuntimeConfig::Builder()
                .withGCConfig(
                    GCConfig::Builder(kTestGCConfigBuilder)
                        .withSanitizeConfig(
                            vm::GCSanitizeConfig::Builder()
                                .withSanitizeRate(1.0)
                                .build())
                        .build())
                .build()) {}
};

/// \return element \p index of \p arr, which must be a JSRegExp.
JSRegExp *regExpAt(Runtime &runtime, Handle<JSArray> arr, uint32_t index) {
  return vmcast<JSRegExp>(arr->at(runtime, index).unboxToHV(runtime));
}

/// Run \p code in a new module and \return its completion value, which
/// must be an array.
Handle<JSArray> runForArray(Runtime &runtime, const char *code) {
  CallResult<HermesValue> res =
      runtime.run(code, "test.js", hbc::CompileFlags{});
  EXPECT_NE(ExecutionStatus::EXCEPTION, res.getStatus());
  return runtime.makeHandle(vmcast<JSArray>(*res));
}

/// Run \p code in a new module, compiled with \p flags, and \return its
/// completion value, which must be a function.
Handle<JSFunction> runForFunction(
    Runtime &runtime,
    const char *code,
    const hbc::CompileFlags &flags = hbc::CompileFlags{}) {
  CallResult<HermesValue> res = runtime.run(code, "test.js", flags);
  EXPECT_NE(ExecutionStatus::EXCEPTION, res.getStatus());
  return runtime.makeHandle(vmcast<JSFunction>(*res));
}

/// Call \p fn with no arguments and \return its result, which must be a
/// JSRegExp.
Handle<JSRegExp> callForRegExp(Runtime &runtime, Handle<JSFunction> fn) {
  CallResult<PseudoHandle<>> res =
      Callable::executeCall0(fn, runtime, Runtime::getUndefinedValue());
  EXPECT_NE(ExecutionStatus::EXCEPTION, res.getStatus());
  return runtime.makeHandle(vmcast<JSRegExp>(res->get()));
}

/// \return whether \p re matches the ASCII string \p str, searching from 0.
bool matches(Runtime &runtime, Handle<JSRegExp> re, const char *str) {
  CallResult<RegExpMatch> m = JSRegExp::search(
      re,
      runtime,
      runtime.makeHandle(StringPrimitive::createNoThrow(runtime, str)),
      0);
  EXPECT_NE(ExecutionStatus::EXCEPTION, m.getStatus());
  return !m->empty();
}

/// Run \p provider, whose code evaluates to [mk, fill]: mk() returns a
/// regexp literal, fill(n) returns an array of n results of mk().
void runFactory(
    Runtime &runtime,
    std::shared_ptr<hbc::BCProvider> provider,
    MutableHandle<JSFunction> &mk,
    MutableHandle<JSFunction> &fill) {
  RuntimeModuleFlags rmFlags;
  rmFlags.persistent = false;
  CallResult<HermesValue> res = runtime.runBytecode(
      std::move(provider),
      rmFlags,
      "test.js",
      Runtime::makeNullHandle<Environment>());
  ASSERT_NE(ExecutionStatus::EXCEPTION, res.getStatus());
  Handle<JSArray> arr = runtime.makeHandle(vmcast<JSArray>(*res));
  mk = vmcast<JSFunction>(arr->at(runtime, 0).unboxToHV(runtime));
  fill = vmcast<JSFunction>(arr->at(runtime, 1).unboxToHV(runtime));
}

/// The source run by runFactory.
const char *const kFactorySource =
    "(function () {"
    "  function mk() { return /c(o)n/g; }"
    "  function fill(n) {"
    "    var a = new Array(n);"
    "    for (var i = 0; i < n; ++i) a[i] = mk();"
    "    return a;"
    "  }"
    "  return [mk, fill];"
    "})()";

/// \return a provider for kFactorySource that is owned only by the
/// returned pointer, and set \p weak to observe it.
std::shared_ptr<hbc::BCProvider> factoryProvider(
    std::weak_ptr<hbc::BCProvider> &weak) {
  std::vector<uint8_t> bc = bytecodeForSource(kFactorySource);
  std::shared_ptr<hbc::BCProvider> provider =
      hbc::BCProviderFromBuffer::createBCProviderFromBuffer(
          std::make_unique<OwnedMemoryBuffer>(
              llvh::MemoryBuffer::getMemBufferCopy(
                  llvh::StringRef((const char *)bc.data(), bc.size()))))
          .first;
  weak = provider;
  return provider;
}

/// Collect young generations until every element of \p arr is in the old
/// generation, so that its finalizer runs in an old-gen sweep.
::testing::AssertionResult promoteAll(Runtime &runtime, Handle<JSArray> arr) {
  for (int round = 0; round < 4; ++round) {
    bool allOld = true;
    for (uint32_t i = 0, e = JSArray::getLength(arr.get(), runtime); i < e;
         ++i) {
      if (runtime.getHeap().inYoungGen(
              vmcast<GCCell>(arr->at(runtime, i).unboxToHV(runtime))))
        allOld = false;
    }
    if (allOld && !runtime.getHeap().inYoungGen(arr.get()))
      return ::testing::AssertionSuccess();
    runtime.getHeap().collectWithOptions("test", GCBase::GCOptions{});
  }
  return ::testing::AssertionFailure() << "regexps were not promoted";
}

/// Request asynchronous old-generation collections while running \p work
/// for \p iterations, then finish with a synchronous full collection.
template <typename Work>
void driveAsyncMajor(Runtime &runtime, int iterations, Work work) {
  const GCBase::GCOptions async{
      GCBase::GCType::Major, GCBase::GCExecutionPolicy::Async};
  for (int i = 0; i < iterations; ++i) {
    if (i % 64 == 0)
      runtime.getHeap().collectWithOptions("test", async);
    GCScopeMarkerRAII marker{runtime};
    work();
    std::this_thread::yield();
  }
  runtime.collect("test");
}

TEST(RegExpBytecodeObjectTest, CreateCopiesBytes) {
  auto compiled = CompiledRegExp::tryCompile("a(b)c", "gy");
  ASSERT_TRUE(compiled.hasValue());
  llvh::ArrayRef<uint8_t> src = compiled->getBytecode();

  llvh::IntrusiveRefCntPtr<RegExpBytecode> entry =
      RegExpBytecode::create(src, RegExpBytecode::Ownership::Exclusive);
  EXPECT_NE(src.data(), entry->bytecode().data());
  EXPECT_TRUE(src.equals(entry->bytecode()));
  EXPECT_TRUE(regex::SyntaxFlags::fromByte(entry->header().syntaxFlags).sticky);
  EXPECT_EQ(RegExpBytecode::Ownership::Exclusive, entry->ownership());
  // The bytes follow the object, aligned like a malloc result.
  EXPECT_EQ(
      0u,
      reinterpret_cast<uintptr_t>(entry->bytecode().data()) %
          alignof(std::max_align_t));
  EXPECT_GE(entry->mallocSize(), sizeof(RegExpBytecode) + src.size());
  EXPECT_LT(
      entry->mallocSize(),
      sizeof(RegExpBytecode) + alignof(std::max_align_t) + src.size());

  // A second reference keeps the bytes alive after the first is dropped
  // (ASan checks the read).
  llvh::IntrusiveRefCntPtr<RegExpBytecode> second = entry;
  entry.reset();
  EXPECT_TRUE(src.equals(second->bytecode()));
}

TEST_F(RegExpBytecodeTest, DynamicRegExpsAreExclusive) {
  Handle<JSArray> arr =
      runForArray(runtime, "[new RegExp('abc', 'g'), new RegExp('abc', 'g')]");
  JSRegExp *r0 = regExpAt(runtime, arr, 0);
  JSRegExp *r1 = regExpAt(runtime, arr, 1);
  EXPECT_NE(r0->getBytecode().data(), r1->getBytecode().data());
  EXPECT_TRUE(r0->getBytecode().equals(r1->getBytecode()));
  EXPECT_EQ(
      RegExpBytecode::Ownership::Exclusive,
      r0->getBytecodeEntry()->ownership());
  // The sole holder reports the entry.
  EXPECT_EQ(
      r0->getBytecodeEntry()->mallocSize(),
      static_cast<GCCell *>(r0)->getVT()->getMallocSize(r0));
}

TEST_F(RegExpBytecodeTest, SplitDoesNotClearStickyOfItsArgument) {
  // split() makes a sticky-cleared copy of its separator
  // (lib/VM/JSLib/RegExp.cpp:2436-2439); the separator itself must stay
  // sticky.
  CallResult<HermesValue> res = runtime.run(
      "var r = /a/y; 'bab'.split(r); r.lastIndex = 0;"
      "r.exec('ba') === null && r.sticky",
      "test.js",
      hbc::CompileFlags{});
  ASSERT_FALSE(isException(res));
  EXPECT_TRUE(res->getBool());
}

TEST_F(RegExpBytecodeTest, LiteralIsSharedAcrossEvaluations) {
  Handle<JSArray> arr = runForArray(
      runtime,
      "(function () { function f() { return /a(b)c/g; }"
      " return [f(), f()]; })()");
  JSRegExp *r0 = regExpAt(runtime, arr, 0);
  JSRegExp *r1 = regExpAt(runtime, arr, 1);
  EXPECT_NE(r0, r1);
  EXPECT_EQ(r0->getBytecode().data(), r1->getBytecode().data());
  EXPECT_EQ(
      RegExpBytecode::Ownership::Cached, r0->getBytecodeEntry()->ownership());
  // The cache, not the JSRegExp, reports a Cached entry.
  EXPECT_EQ(0u, static_cast<GCCell *>(r0)->getVT()->getMallocSize(r0));
}

TEST_F(RegExpBytecodeTest, LiteralCachesArePerModule) {
  // Each module's only literal has regexp ID 0.
  Handle<JSFunction> one =
      runForFunction(runtime, "(function () { return /one/; })");
  Handle<JSFunction> two =
      runForFunction(runtime, "(function () { return /two/; })");
  Handle<JSRegExp> r1 = callForRegExp(runtime, one);
  Handle<JSRegExp> r2 = callForRegExp(runtime, two);
  EXPECT_NE(r1->getBytecode().data(), r2->getBytecode().data());
  EXPECT_TRUE(matches(runtime, r1, "one"));
  EXPECT_FALSE(matches(runtime, r1, "two"));
  EXPECT_TRUE(matches(runtime, r2, "two"));
  EXPECT_FALSE(matches(runtime, r2, "one"));
}

TEST_F(RegExpBytecodeTest, LazilyCompiledLiteralGrowsTheCache) {
  hbc::CompileFlags flags;
  flags.lazy = true;
  flags.preemptiveFileCompilationThreshold = 0;
  flags.preemptiveFunctionCompilationThreshold = 0;
  Handle<JSFunction> f = runForFunction(
      runtime,
      "(function () { return function () { return /lazy(re)/g; }; })()",
      flags);
  RuntimeModule *rm = f->getRuntimeModule();
  // Precondition: the literal is not in the table until its function is
  // compiled, so its ID is past the table size at module load.
  ASSERT_EQ(0u, rm->getBytecode()->getRegExpTable().size());
  Handle<JSRegExp> r0 = callForRegExp(runtime, f);
  ASSERT_EQ(1u, rm->getBytecode()->getRegExpTable().size());
  Handle<JSRegExp> r1 = callForRegExp(runtime, f);
  EXPECT_EQ(r0->getBytecode().data(), r1->getBytecode().data());
  EXPECT_TRUE(matches(runtime, r1, "xlazyre"));
}

TEST_F(RegExpBytecodeTest, LazyCompilesKeepGrowingTheCache) {
  hbc::CompileFlags flags;
  flags.lazy = true;
  flags.preemptiveFileCompilationThreshold = 0;
  flags.preemptiveFunctionCompilationThreshold = 0;
  CallResult<HermesValue> res = runtime.run(
      "[function () { return /first(re)/g; },"
      " function () { return /second(re)/g; }]",
      "test.js",
      flags);
  ASSERT_NE(ExecutionStatus::EXCEPTION, res.getStatus());
  Handle<JSArray> arr = runtime.makeHandle(vmcast<JSArray>(*res));
  Handle<JSFunction> f1 = runtime.makeHandle(
      vmcast<JSFunction>(arr->at(runtime, 0).unboxToHV(runtime)));
  Handle<JSFunction> f2 = runtime.makeHandle(
      vmcast<JSFunction>(arr->at(runtime, 1).unboxToHV(runtime)));
  RuntimeModule *rm = f1->getRuntimeModule();

  ASSERT_EQ(0u, rm->getBytecode()->getRegExpTable().size());
  Handle<JSRegExp> a0 = callForRegExp(runtime, f1);
  ASSERT_EQ(1u, rm->getBytecode()->getRegExpTable().size());
  // Compiling the second function grows the table, and so the cache, again.
  Handle<JSRegExp> b0 = callForRegExp(runtime, f2);
  ASSERT_EQ(2u, rm->getBytecode()->getRegExpTable().size());
  Handle<JSRegExp> b1 = callForRegExp(runtime, f2);
  EXPECT_EQ(b0->getBytecode().data(), b1->getBytecode().data());
  EXPECT_NE(a0->getBytecode().data(), b0->getBytecode().data());
  EXPECT_TRUE(matches(runtime, b1, "xsecondre"));
  EXPECT_FALSE(matches(runtime, b1, "xfirstre"));
}

TEST_F(RegExpBytecodeTest, FastCopySharesOnlyCachedBytecode) {
  Handle<JSArray> arr =
      runForArray(runtime, "[/a(b)c/g, new RegExp('a(b)c', 'g')]");
  Handle<JSRegExp> lit = runtime.makeHandle(regExpAt(runtime, arr, 0));
  Handle<JSRegExp> dyn = runtime.makeHandle(regExpAt(runtime, arr, 1));
  ASSERT_EQ(
      RegExpBytecode::Ownership::Cached, lit->getBytecodeEntry()->ownership());
  ASSERT_EQ(
      RegExpBytecode::Ownership::Exclusive,
      dyn->getBytecodeEntry()->ownership());

  // The fast copy used by matchAll and split.
  auto copy = [&](Handle<JSRegExp> other, const char *flags) {
    Handle<JSRegExp> self = runtime.makeHandle(JSRegExp::create(runtime));
    EXPECT_EQ(
        ExecutionStatus::RETURNED,
        JSRegExp::initialize(
            self,
            runtime,
            other,
            runtime.makeHandle(
                StringPrimitive::createNoThrow(runtime, flags))));
    return self;
  };

  Handle<JSRegExp> litSame = copy(lit, "g");
  EXPECT_EQ(lit->getBytecode().data(), litSame->getBytecode().data());

  Handle<JSRegExp> litOther = copy(lit, "gy");
  EXPECT_NE(lit->getBytecode().data(), litOther->getBytecode().data());
  EXPECT_EQ(
      RegExpBytecode::Ownership::Exclusive,
      litOther->getBytecodeEntry()->ownership());
  EXPECT_TRUE(JSRegExp::getSyntaxFlags(*litOther).sticky);

  Handle<JSRegExp> dynSame = copy(dyn, "g");
  EXPECT_NE(dyn->getBytecode().data(), dynSame->getBytecode().data());
  EXPECT_TRUE(dyn->getBytecode().equals(dynSame->getBytecode()));
  EXPECT_EQ(
      RegExpBytecode::Ownership::Exclusive,
      dynSame->getBytecodeEntry()->ownership());
}

TEST_F(RegExpBytecodeTest, LiteralOutlivesItsModule) {
  // runtime.run() would load the module as persistent, and the runtime
  // would keep its storage. Instead, run from an owning buffer, held only
  // by the provider, with persistent = false.
  std::weak_ptr<hbc::BCProvider> weakProvider;
  MutableHandle<JSRegExp> re{runtime};
  {
    std::vector<uint8_t> bc =
        bytecodeForSource("(function () { return /a(b)c/g; })()");
    auto buffer = std::make_unique<OwnedMemoryBuffer>(
        llvh::MemoryBuffer::getMemBufferCopy(
            llvh::StringRef((const char *)bc.data(), bc.size())));
    std::shared_ptr<hbc::BCProvider> provider =
        hbc::BCProviderFromBuffer::createBCProviderFromBuffer(std::move(buffer))
            .first;
    ASSERT_TRUE(provider);
    weakProvider = provider;
    RuntimeModuleFlags rmFlags;
    rmFlags.persistent = false;
    CallResult<HermesValue> res = runtime.runBytecode(
        std::move(provider),
        rmFlags,
        "test.js",
        Runtime::makeNullHandle<Environment>());
    ASSERT_FALSE(isException(res));
    re = runtime.makeHandle(vmcast<JSRegExp>(*res));
  }
  for (int i = 0; i < 3 && !weakProvider.expired(); ++i)
    runtime.collect("test");
  // Precondition: the module and its storage are gone.
  ASSERT_TRUE(weakProvider.expired());
  EXPECT_TRUE(matches(runtime, re, "xabcx"));
}

TEST_F(RegExpBytecodeTest, SHUnitSharesPrecompiledBytecode) {
  auto compiled = CompiledRegExp::tryCompile("a(b)c", "g");
  ASSERT_TRUE(compiled.hasValue());
  llvh::ArrayRef<uint8_t> bc = compiled->getBytecode();
  const SHRegExpTableEntry table[] = {{0, (uint32_t)bc.size()}};

  // Only the regexp fields matter to _sh_ljs_create_regexp.
  SHUnit unit{};
  unit.regexp_storage = bc.data();
  unit.regexp_table = table;
  unit.num_regexps = 1;
  unit.runtime_ext = new SHUnitExt{unit.num_regexps};

  auto symbol = [&](const char *str) {
    CallResult<Handle<SymbolID>> sym =
        getSymbolHandleForTest(runtime, createASCIIRef(str));
    EXPECT_NE(ExecutionStatus::EXCEPTION, sym.getStatus());
    return sym->get().unsafeGetRaw();
  };
  const SHSymbolID pattern = symbol("a(b)c");
  const SHSymbolID flags = symbol("g");

  const size_t sizeBefore = sh_unit_additional_memory_size(&unit);
  // Must count the SHUnitExt object itself plus the cache slot sized at
  // construction, not just a pointer to it.
  EXPECT_GE(
      sizeBefore,
      sizeof(SHUnitExt) + sizeof(llvh::IntrusiveRefCntPtr<RegExpBytecode>));
  auto create = [&]() {
    SHLegacyValue v =
        _sh_ljs_create_regexp(getSHRuntime(runtime), &unit, pattern, flags, 0);
    return runtime.makeHandle(vmcast<JSRegExp>(HermesValue::fromRaw(v.raw)));
  };
  Handle<JSRegExp> r0 = create();
  Handle<JSRegExp> r1 = create();
  EXPECT_EQ(r0->getBytecode().data(), r1->getBytecode().data());
  EXPECT_TRUE(bc.equals(r0->getBytecode()));
  EXPECT_EQ(
      RegExpBytecode::Ownership::Cached, r0->getBytecodeEntry()->ownership());
  // The vector was sized when the unit was attached; only the entry is
  // new.
  EXPECT_EQ(
      sizeBefore + r0->getBytecodeEntry()->mallocSize(),
      sh_unit_additional_memory_size(&unit));
  EXPECT_TRUE(matches(runtime, r1, "xabc"));

  // The regexps keep the entry alive after the unit's cache is gone.
  delete unit.runtime_ext;
  EXPECT_TRUE(matches(runtime, r0, "abc"));
}

TEST_F(RegExpBytecodeTest, ConcurrentReleaseWhileAcquiring) {
  // This gtest has no GTEST_SKIP: without the Hades background thread
  // there is nothing to exercise, so return.
  if (!kConcurrentGC)
    return;
  std::weak_ptr<hbc::BCProvider> weak;
  MutableHandle<JSFunction> mk{runtime}, fill{runtime};
  runFactory(runtime, factoryProvider(weak), mk, fill);

  CallResult<PseudoHandle<>> filled = Callable::executeCall1(
      fill,
      runtime,
      Runtime::getUndefinedValue(),
      HermesValue::encodeTrustedNumberValue(256));
  ASSERT_NE(ExecutionStatus::EXCEPTION, filled.getStatus());
  MutableHandle<JSArray> arr{runtime, vmcast<JSArray>(filled->get())};
  ASSERT_TRUE(promoteAll(runtime, arr));

  // Drop the promoted regexps; their old-gen sweep releases the shared
  // entry while the mutator keeps acquiring it.
  arr = nullptr;
  driveAsyncMajor(runtime, 4096, [&]() { callForRegExp(runtime, mk); });
  EXPECT_TRUE(matches(runtime, callForRegExp(runtime, mk), "con"));
}

TEST_F(RegExpBytecodeTest, ConcurrentReleaseAfterModuleDeath) {
  // This gtest has no GTEST_SKIP: without the Hades background thread
  // there is nothing to exercise, so return.
  if (!kConcurrentGC)
    return;
  std::weak_ptr<hbc::BCProvider> weak;
  MutableHandle<JSArray> arr{runtime};
  {
    // Handles live until their GCScope ends, not their C++ scope. This
    // scope frees the factory's handles; arr belongs to the fixture's.
    GCScope factoryScope{runtime};
    MutableHandle<JSFunction> mk{runtime}, fill{runtime};
    runFactory(runtime, factoryProvider(weak), mk, fill);
    CallResult<PseudoHandle<>> filled = Callable::executeCall1(
        fill,
        runtime,
        Runtime::getUndefinedValue(),
        HermesValue::encodeTrustedNumberValue(256));
    ASSERT_NE(ExecutionStatus::EXCEPTION, filled.getStatus());
    arr = vmcast<JSArray>(filled->get());
    // Release the factory: a callable function keeps its Domain alive.
  }
  ASSERT_TRUE(promoteAll(runtime, arr));
  for (int i = 0; i < 3 && !weak.expired(); ++i)
    runtime.collect("test");
  // Precondition: the module is gone; only the regexps hold the entry.
  ASSERT_TRUE(weak.expired());

  arr = nullptr;
  driveAsyncMajor(runtime, 4096, [&]() {
    // Unrelated allocation keeps the mutator busy.
    JSObject::create(runtime);
  });
}

} // namespace
} // namespace regexpbytecodetest
} // namespace unittest
} // namespace hermes

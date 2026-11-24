/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "VMRuntimeTestHelpers.h"

#include "hermes/VM/Callable.h"
#include "hermes/VM/ComputedPropertyCache-inline.h"
#include "hermes/VM/HiddenClass.h"
#include "hermes/VM/HostModel.h"
#include "hermes/VM/JSObject.h"

#include <functional>
#include <initializer_list>
#include <memory>
#include <vector>

using namespace hermes::vm;

namespace {

/// The runtime configuration of these tests: the usual test heap, with the
/// Handle-SAN trees moving the heap on every allocation instead of one in a
/// hundred.
const RuntimeConfig kCPCTestRTConfig =
    RuntimeConfig::Builder()
        .withGCConfig(
            GCConfig::Builder(kTestGCConfigBuilder)
                .withSanitizeConfig(
                    GCSanitizeConfig::Builder().withSanitizeRate(1.0).build())
                .build())
        .build();

/// The fixture of every ComputedPropertyCache test.
class ComputedPropertyCacheTest : public RuntimeTestFixtureBase {
 public:
  /// Create the runtime with kCPCTestRTConfig.
  ComputedPropertyCacheTest() : RuntimeTestFixtureBase(kCPCTestRTConfig) {}
};

/// \return the SymbolID of \p s; its handle stays in the current GCScope.
SymbolID sym(Runtime &runtime, const char *s) {
  auto res =
      runtime.getIdentifierTable().getSymbolHandle(runtime, createASCIIRef(s));
  EXPECT_NE(res.getStatus(), ExecutionStatus::EXCEPTION);
  return res->get();
}

/// \return a handle to the uniqued string of \p id
/// (runtime.getStringPrimFromSymbolID).
Handle<> keyOf(Runtime &runtime, SymbolID id) {
  return runtime.makeHandle(
      HermesValue::encodeStringValue(runtime.getStringPrimFromSymbolID(id)));
}

/// \return a new object whose parent is \p parent, with the named
/// properties added in order by JSObject::putNamed_RJS, each holding
/// \p value.
Handle<JSObject> makeObject(
    Runtime &runtime,
    Handle<JSObject> parent,
    std::initializer_list<SymbolID> names,
    Handle<> value) {
  Handle<JSObject> obj = runtime.makeHandle(JSObject::create(runtime, parent));
  for (SymbolID name : names) {
    auto res = JSObject::putNamed_RJS(obj, runtime, name, value);
    EXPECT_NE(res.getStatus(), ExecutionStatus::EXCEPTION);
  }
  return obj;
}

/// A WeakRootAcceptor whose acceptWeak(WeakRootBase &) applies \p fn to
/// the root; the other two overloads do nothing.
struct TestWeakAcceptor final : public WeakRootAcceptor {
  /// Applied to every WeakRootBase accepted.
  std::function<void(WeakRootBase &)> fn;

  /// Accept weak roots with \p fn.
  explicit TestWeakAcceptor(std::function<void(WeakRootBase &)> fn)
      : fn(std::move(fn)) {}

  /// Apply fn to \p ptr.
  void acceptWeak(WeakRootBase &ptr) override {
    fn(ptr);
  }
  /// Do nothing.
  void acceptWeakSym(WeakRootSymbolID &) override {}
  /// Do nothing.
  void acceptWeak(WeakSmallHermesValue &) override {}
};

/// A HostObjectProxy with no properties.
class TrivialHostObjectProxy final : public HostObjectProxy {
 public:
  /// \return undefined.
  CallResult<HermesValue> get(SymbolID) override {
    return HermesValue::encodeUndefinedValue();
  }
  /// \return false: nothing is stored.
  CallResult<bool> set(SymbolID, HermesValue) override {
    return false;
  }
  /// Report no names.
  ExecutionStatus getHostPropertyNames(MutableHandle<JSArray>) override {
    return ExecutionStatus::RETURNED;
  }
};

/// \return the object prototype of \p runtime as a handle.
Handle<JSObject> objProto(Runtime &runtime) {
  return Handle<JSObject>::vmcast(&runtime.objectPrototype);
}

TEST_F(ComputedPropertyCacheTest, InsertLookup) {
  GCScope scope{runtime, "InsertLookup", 128};
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  SymbolID a = sym(runtime, "a");
  SymbolID b = sym(runtime, "b");
  SymbolID x = sym(runtime, "x");
  SymbolID y = sym(runtime, "y");
  Handle<> undef = Runtime::getUndefinedValue();
  Handle<JSObject> objC = makeObject(runtime, objProto(runtime), {x}, undef);
  Handle<JSObject> objR = makeObject(runtime, objProto(runtime), {x, y}, undef);
  Handle<HiddenClass> C = runtime.makeHandle(objC->getClass(runtime));
  Handle<HiddenClass> R = runtime.makeHandle(objR->getClass(runtime));

  // No allocation from here on.
  cache.clear();
  cache.insertSlot(runtime, *C, a, 3, true);
  CompressedPointer cpC = CompressedPointer::encode(*C, runtime);
  const ComputedPropertyCache::Entry *e = cache.lookup(cpC, a);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->getKind(), ComputedPropertyCache::Kind::Slot);
  EXPECT_EQ(e->slot, 3u);
  EXPECT_TRUE(e->writable);
  EXPECT_EQ(cache.lookup(cpC, b), nullptr);
  EXPECT_EQ(cache.numEntries(), 1u);

  cache.insertAdd(runtime, *C, b, *R, 1, 77);
  e = cache.lookup(cpC, b);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->getKind(), ComputedPropertyCache::Kind::Add);
  EXPECT_EQ(e->slot, 1u);
  EXPECT_EQ(e->parentEpoch, 77u);
  EXPECT_TRUE(e->resultClazz == CompressedPointer::encode(*R, runtime));
  EXPECT_EQ(cache.numEntries(), 2u);

  cache.insertSlot(runtime, *C, a, 4, true);
  EXPECT_EQ(cache.numEntries(), 2u);
  e = cache.lookup(cpC, a);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->slot, 4u);
  EXPECT_NE(cache.lookup(cpC, b), nullptr);
}

TEST_F(ComputedPropertyCacheTest, TwoWays) {
  GCScope scope{runtime, "TwoWays", 128};
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  Handle<JSObject> obj = runtime.makeHandle(JSObject::create(runtime));
  Handle<HiddenClass> C = runtime.makeHandle(obj->getClass(runtime));

  // No allocation from here on, so the class's address, which is part of
  // the hash, stays put. The names are fabricated: the cache only compares
  // them.
  CompressedPointer cpC = CompressedPointer::encode(*C, runtime);
  std::vector<std::vector<SymbolID>> sets(ComputedPropertyCache::kNumSets);
  std::vector<SymbolID> *triple = nullptr;
  for (uint32_t i = 1; i <= 2049 && !triple; ++i) {
    SymbolID name = SymbolID::unsafeCreate(i);
    auto &set = sets[ComputedPropertyCache::setIndex(cpC, name)];
    set.push_back(name);
    if (set.size() == 3)
      triple = &set;
  }
  ASSERT_NE(triple, nullptr);
  SymbolID n0 = (*triple)[0], n1 = (*triple)[1], n2 = (*triple)[2];

  cache.clear();
  cache.insertSlot(runtime, *C, n0, 0, true);
  cache.insertSlot(runtime, *C, n1, 1, true);
  ASSERT_NE(cache.lookup(cpC, n0), nullptr);
  EXPECT_EQ(cache.lookup(cpC, n0)->slot, 0u);
  ASSERT_NE(cache.lookup(cpC, n1), nullptr);
  EXPECT_EQ(cache.lookup(cpC, n1)->slot, 1u);

  cache.insertSlot(runtime, *C, n2, 2, true);
  EXPECT_EQ(cache.lookup(cpC, n0), nullptr);
  ASSERT_NE(cache.lookup(cpC, n1), nullptr);
  EXPECT_EQ(cache.lookup(cpC, n1)->slot, 1u);
  ASSERT_NE(cache.lookup(cpC, n2), nullptr);
  EXPECT_EQ(cache.lookup(cpC, n2)->slot, 2u);
  EXPECT_EQ(cache.numEntries(), 2u);

  // n1 is in way 1. Re-inserting it must not leave a second copy.
  cache.insertSlot(runtime, *C, n1, 5, true);
  EXPECT_EQ(cache.numEntries(), 2u);
  ASSERT_NE(cache.lookup(cpC, n1), nullptr);
  EXPECT_EQ(cache.lookup(cpC, n1)->slot, 5u);
  ASSERT_NE(cache.lookup(cpC, n2), nullptr);
  EXPECT_EQ(cache.lookup(cpC, n2)->slot, 2u);
}

TEST_F(ComputedPropertyCacheTest, EmptyWay0) {
  GCScope scope{runtime, "EmptyWay0", 128};
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  SymbolID x = sym(runtime, "x");
  Handle<> undef = Runtime::getUndefinedValue();
  Handle<HiddenClass> C = runtime.makeHandle(
      makeObject(runtime, objProto(runtime), {}, undef)->getClass(runtime));
  Handle<HiddenClass> D = runtime.makeHandle(
      makeObject(runtime, objProto(runtime), {x}, undef)->getClass(runtime));

  // No allocation from here on; the names are fabricated, as in TwoWays.
  // Find a set that holds two keys of C and one of D.
  const CompressedPointer cpC = CompressedPointer::encode(*C, runtime);
  const CompressedPointer cpD = CompressedPointer::encode(*D, runtime);
  std::vector<std::vector<SymbolID>> namesC(ComputedPropertyCache::kNumSets);
  std::vector<std::vector<SymbolID>> namesD(ComputedPropertyCache::kNumSets);
  int found = -1;
  for (uint32_t i = 1; i <= 8192 && found < 0; ++i) {
    SymbolID name = SymbolID::unsafeCreate(i);
    unsigned setC = ComputedPropertyCache::setIndex(cpC, name);
    unsigned setD = ComputedPropertyCache::setIndex(cpD, name);
    namesC[setC].push_back(name);
    namesD[setD].push_back(name);
    for (unsigned s : {setC, setD}) {
      if (namesC[s].size() >= 2 && namesD[s].size() >= 1) {
        found = s;
        break;
      }
    }
  }
  ASSERT_GE(found, 0);
  SymbolID a = namesD[found][0];
  SymbolID b = namesC[found][0];
  SymbolID c = namesC[found][1];

  /// Leave (D, a) in way 0 and (C, b) in way 1, then empty way 0 the way
  /// the GC does when D dies.
  auto setUp = [&]() {
    cache.clear();
    cache.insertSlot(runtime, *C, b, 1, true);
    cache.insertSlot(runtime, *D, a, 0, true);
    TestWeakAcceptor acceptor{[&](WeakRootBase &root) {
      if (root == cpD)
        root = nullptr;
    }};
    cache.markWeakRoots(acceptor);
    ASSERT_EQ(cache.lookup(cpD, a), nullptr);
    ASSERT_NE(cache.lookup(cpC, b), nullptr);
    ASSERT_EQ(cache.numEntries(), 1u);
  };

  {
    SCOPED_TRACE("a new key does not evict way 1");
    setUp();
    cache.insertSlot(runtime, *C, c, 2, true);
    ASSERT_NE(cache.lookup(cpC, b), nullptr);
    EXPECT_EQ(cache.lookup(cpC, b)->slot, 1u);
    ASSERT_NE(cache.lookup(cpC, c), nullptr);
    EXPECT_EQ(cache.lookup(cpC, c)->slot, 2u);
    EXPECT_EQ(cache.numEntries(), 2u);
  }
  {
    SCOPED_TRACE("the key in way 1 is not duplicated");
    setUp();
    cache.insertSlot(runtime, *C, b, 7, true);
    EXPECT_EQ(cache.numEntries(), 1u);
    ASSERT_NE(cache.lookup(cpC, b), nullptr);
    EXPECT_EQ(cache.lookup(cpC, b)->slot, 7u);
  }
}

TEST_F(ComputedPropertyCacheTest, Predicate) {
  GCScope scope{runtime, "Predicate", 128};
  Handle<JSObject> plain = runtime.makeHandle(JSObject::create(runtime));

  auto hostRes = HostObject::createWithoutPrototype(
      runtime, std::make_unique<TrivialHostObjectProxy>());
  ASSERT_NE(hostRes.getStatus(), ExecutionStatus::EXCEPTION);
  Handle<JSObject> host = runtime.makeHandle(vmcast<JSObject>(*hostRes));

  auto arrRes = JSArray::create(runtime, 0, 0);
  ASSERT_NE(arrRes.getStatus(), ExecutionStatus::EXCEPTION);
  Handle<JSArray> arr = runtime.makeHandle(std::move(*arrRes));

  ClassFlags lazyFlags{};
  lazyFlags.lazyObject = 1;
  Handle<HiddenClass> lazy = runtime.makeHandle(
      HiddenClass::createRoot(runtime, objProto(runtime), lazyFlags));

  EXPECT_TRUE(ComputedPropertyCache::isCacheable(plain->getClass(runtime)));
  EXPECT_FALSE(ComputedPropertyCache::isCacheable(host->getClass(runtime)));
  EXPECT_FALSE(ComputedPropertyCache::isCacheable(arr->getClass(runtime)));
  EXPECT_FALSE(ComputedPropertyCache::isCacheable(*lazy));
}

TEST_F(ComputedPropertyCacheTest, RecordSlot) {
  GCScope scope{runtime, "RecordSlot", 128};
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  SymbolID a = sym(runtime, "a");
  Handle<> keyA = keyOf(runtime, a);
  Handle<JSObject> plain = runtime.makeHandle(JSObject::create(runtime));
  auto arrRes = JSArray::create(runtime, 0, 0);
  ASSERT_NE(arrRes.getStatus(), ExecutionStatus::EXCEPTION);
  Handle<JSArray> arr = runtime.makeHandle(std::move(*arrRes));
  auto strRes = StringPrimitive::create(runtime, createASCIIRef("a"));
  ASSERT_NE(strRes.getStatus(), ExecutionStatus::EXCEPTION);
  Handle<> plainStr = runtime.makeHandle(*strRes);
  ASSERT_FALSE(plainStr->getString()->isUniqued());

  // No allocation from here on.
  CompressedPointer cpC = plain->getClassGCPtr();
  const PropertyFlags dflt = PropertyFlags::defaultNewNamedPropertyFlags();

  cache.clear();
  cache.recordSlot(runtime, plain->getClass(runtime), a, dflt, 2);
  const ComputedPropertyCache::Entry *e = cache.lookup(cpC, a);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->getKind(), ComputedPropertyCache::Kind::Slot);
  EXPECT_EQ(e->slot, 2u);
  EXPECT_TRUE(e->writable);

  cache.clear();
  PropertyFlags internalSetter = dflt;
  internalSetter.internalSetter = 1;
  cache.recordSlot(runtime, plain->getClass(runtime), a, internalSetter, 2);
  e = cache.lookup(cpC, a);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->getKind(), ComputedPropertyCache::Kind::Slot);
  EXPECT_FALSE(e->writable);

  /// Expect that recording \p flags at \p slot on the class of \p obj
  /// records nothing.
  auto expectNothing = [&](Handle<JSObject> obj,
                           PropertyFlags flags,
                           SlotIndex slot,
                           const char *what) {
    SCOPED_TRACE(what);
    cache.clear();
    cache.recordSlot(runtime, obj->getClass(runtime), a, flags, slot);
    EXPECT_EQ(cache.numEntries(), 0u);
    EXPECT_EQ(cache.lookup(obj->getClassGCPtr(), a), nullptr);
  };

  PropertyFlags f = dflt;
  f.accessor = 1;
  expectNothing(plain, f, 2, "accessor");
  f = dflt;
  f.indexed = 1;
  expectNothing(plain, f, 2, "indexed");
  f = dflt;
  f.hostObject = 1;
  expectNothing(plain, f, 2, "hostObject");
  f = dflt;
  f.proxyObject = 1;
  expectNothing(plain, f, 2, "proxyObject");
  expectNothing(plain, dflt, 1u << 30, "slot 2^30");
  expectNothing(arr, dflt, 2, "array class");

  // The name of a key: a uniqued string has one, a number and a string
  // that is not uniqued do not.
  hermes::OptValue<SymbolID> nameA =
      ComputedPropertyCache::nameOfKey(runtime, *keyA);
  ASSERT_TRUE(nameA.hasValue());
  EXPECT_EQ(*nameA, a);
  EXPECT_FALSE(
      ComputedPropertyCache::nameOfKey(
          runtime, HermesValue::encodeTrustedNumberValue(1))
          .hasValue());
  EXPECT_FALSE(ComputedPropertyCache::nameOfKey(runtime, *plainStr).hasValue());

  // fillSlot records with a uniqued key and a cacheable class only.
  cache.clear();
  cache.fillSlot(runtime, plain->getClass(runtime), *keyA, dflt, 2);
  e = cache.lookup(cpC, a);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->slot, 2u);
  cache.clear();
  cache.fillSlot(runtime, plain->getClass(runtime), *plainStr, dflt, 2);
  EXPECT_EQ(cache.numEntries(), 0u);
  cache.fillSlot(
      runtime,
      plain->getClass(runtime),
      HermesValue::encodeTrustedNumberValue(1),
      dflt,
      2);
  EXPECT_EQ(cache.numEntries(), 0u);
  cache.fillSlot(runtime, arr->getClass(runtime), *keyA, dflt, 2);
  EXPECT_EQ(cache.numEntries(), 0u);
}

TEST_F(ComputedPropertyCacheTest, WeakRoots) {
  GCScope scope{runtime, "WeakRoots", 128};
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  SymbolID a = sym(runtime, "a");
  SymbolID x = sym(runtime, "x");
  SymbolID y = sym(runtime, "y");
  SymbolID z = sym(runtime, "z");
  Handle<> undef = Runtime::getUndefinedValue();
  Handle<HiddenClass> C1 = runtime.makeHandle(
      makeObject(runtime, objProto(runtime), {x}, undef)->getClass(runtime));
  Handle<HiddenClass> C2 = runtime.makeHandle(
      makeObject(runtime, objProto(runtime), {y}, undef)->getClass(runtime));
  Handle<HiddenClass> C3 = runtime.makeHandle(
      makeObject(runtime, objProto(runtime), {z}, undef)->getClass(runtime));
  Handle<HiddenClass> R = runtime.makeHandle(
      makeObject(runtime, objProto(runtime), {y, a}, undef)->getClass(runtime));

  // No allocation from here on.
  const CompressedPointer cp1 = CompressedPointer::encode(*C1, runtime);
  const CompressedPointer cp2 = CompressedPointer::encode(*C2, runtime);
  const CompressedPointer cp3 = CompressedPointer::encode(*C3, runtime);
  const CompressedPointer cpR = CompressedPointer::encode(*R, runtime);

  /// Plant the three entries in an empty cache.
  auto plant = [&]() {
    cache.clear();
    cache.insertSlot(runtime, *C1, a, 0, true);
    cache.insertAdd(runtime, *C2, a, *R, 1, 5);
    cache.insertSlot(runtime, *C3, a, 2, true);
    ASSERT_EQ(cache.numEntries(), 3u);
  };

  {
    SCOPED_TRACE("null the result class");
    plant();
    TestWeakAcceptor acceptor{[&](WeakRootBase &root) {
      if (root == cpR)
        root = nullptr;
    }};
    cache.markWeakRoots(acceptor);
    EXPECT_EQ(cache.lookup(cp2, a), nullptr);
    EXPECT_NE(cache.lookup(cp1, a), nullptr);
    EXPECT_NE(cache.lookup(cp3, a), nullptr);
    EXPECT_EQ(cache.numEntries(), 2u);
  }
  {
    SCOPED_TRACE("move C1 to C3's address");
    plant();
    TestWeakAcceptor acceptor{[&](WeakRootBase &root) {
      if (root == cp1)
        root = cp3;
    }};
    cache.markWeakRoots(acceptor);
    EXPECT_EQ(cache.lookup(cp1, a), nullptr);
    EXPECT_EQ(cache.numEntries(), 2u);
  }
  {
    SCOPED_TRACE("null C3");
    plant();
    TestWeakAcceptor acceptor{[&](WeakRootBase &root) {
      if (root == cp3)
        root = nullptr;
    }};
    cache.markWeakRoots(acceptor);
    EXPECT_EQ(cache.lookup(cp3, a), nullptr);
    EXPECT_EQ(cache.numEntries(), 2u);
  }
  {
    SCOPED_TRACE("change nothing");
    plant();
    TestWeakAcceptor acceptor{[](WeakRootBase &) {}};
    cache.markWeakRoots(acceptor);
    EXPECT_NE(cache.lookup(cp1, a), nullptr);
    EXPECT_NE(cache.lookup(cp2, a), nullptr);
    EXPECT_NE(cache.lookup(cp3, a), nullptr);
    EXPECT_EQ(cache.numEntries(), 3u);
  }
}

/// The number of names and objects of collectEmpties.
constexpr unsigned kNumCollectNames = 8;

/// \return the name "cpcCollect<i>" of collectEmpties.
std::string collectName(unsigned i) {
  return "cpcCollect" + std::to_string(i);
}

/// Empty the cache, fill it with entries whose classes and names nothing
/// else keeps, collect, and expect the cache to be empty.
void collectEmpties(Runtime &runtime) {
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  cache.clear();
  {
    GCScope inner{runtime, "collectEmpties", 128};
    Handle<> undef = Runtime::getUndefinedValue();
    std::vector<SymbolID> names;
    std::vector<Handle<JSObject>> objs;
    for (unsigned i = 0; i < kNumCollectNames; ++i) {
      names.push_back(sym(runtime, collectName(i).c_str()));
      objs.push_back(makeObject(runtime, objProto(runtime), {names[i]}, undef));
    }
    // No allocation from here on to the end of the scope.
    for (unsigned i = 0; i < kNumCollectNames; ++i)
      cache.insertSlot(runtime, objs[i]->getClass(runtime), names[i], 0, true);
    EXPECT_EQ(cache.numEntries(), kNumCollectNames);
  }
  runtime.collect("test");
  EXPECT_EQ(cache.numEntries(), 0u);
}

TEST_F(ComputedPropertyCacheTest, CollectEmpties) {
  GCScope scope{runtime, "CollectEmpties", 128};
  collectEmpties(runtime);
}

} // namespace

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

/// Compile \p source, an expression evaluating to a function, and
/// \return the function. Call it before planting anything.
Handle<Callable> compileFn(Runtime &runtime, const char *source) {
  hermes::hbc::CompileFlags flags;
  CallResult<HermesValue> res = runtime.run(source, "", flags);
  if (res == ExecutionStatus::EXCEPTION) {
    ADD_FAILURE() << "compiling failed: " << source;
    return Runtime::makeNullHandle<Callable>();
  }
  if (!vmisa<Callable>(*res)) {
    ADD_FAILURE() << "not a function: " << source;
    return Runtime::makeNullHandle<Callable>();
  }
  return runtime.makeHandle(vmcast<Callable>(*res));
}

/// \return the result of calling \p fn with this undefined and the
/// arguments \p a0 and \p a1, or undefined if it throws.
HermesValue
call2(Runtime &runtime, Handle<Callable> fn, HermesValue a0, HermesValue a1) {
  auto res =
      Callable::executeCall2(fn, runtime, Runtime::getUndefinedValue(), a0, a1);
  EXPECT_NE(res.getStatus(), ExecutionStatus::EXCEPTION);
  if (res == ExecutionStatus::EXCEPTION)
    return HermesValue::encodeUndefinedValue();
  return res->get();
}

/// \return the object returned by calling \p fn with this undefined and
/// no arguments.
Handle<JSObject> call0Obj(Runtime &runtime, Handle<Callable> fn) {
  auto res = Callable::executeCall0(fn, runtime, Runtime::getUndefinedValue());
  EXPECT_NE(res.getStatus(), ExecutionStatus::EXCEPTION);
  if (res == ExecutionStatus::EXCEPTION)
    return Runtime::makeNullHandle<JSObject>();
  if (!vmisa<JSObject>(res->get())) {
    ADD_FAILURE() << "the function did not return an object";
    return Runtime::makeNullHandle<JSObject>();
  }
  return runtime.makeHandle(vmcast<JSObject>(res->get()));
}

/// \return the value of \p obj[\p key] by JSObject::getComputed_RJS, or
/// undefined if it throws.
HermesValue getComputed(Runtime &runtime, Handle<JSObject> obj, Handle<> key) {
  auto res = JSObject::getComputed_RJS(obj, runtime, key);
  EXPECT_NE(res.getStatus(), ExecutionStatus::EXCEPTION);
  if (res == ExecutionStatus::EXCEPTION)
    return HermesValue::encodeUndefinedValue();
  return res->get();
}

/// \return success if \p obj[\p key], read by getComputed, is the value of
/// \p expected. \p expected is read after the get, which may move the heap.
::testing::AssertionResult
getIs(Runtime &runtime, Handle<JSObject> obj, Handle<> key, Handle<> expected) {
  HermesValue got = getComputed(runtime, obj, key);
  if (got.getRaw() == expected->getRaw())
    return ::testing::AssertionSuccess();
  return ::testing::AssertionFailure()
      << "got raw " << got.getRaw() << ", expected raw " << expected->getRaw();
}

/// \return the result of JSObject::putComputed_RJS(\p obj, \p key, \p value),
/// or false if it throws.
bool putComputed(
    Runtime &runtime,
    Handle<JSObject> obj,
    Handle<> key,
    Handle<> value) {
  auto res = JSObject::putComputed_RJS(obj, runtime, key, value);
  EXPECT_NE(res.getStatus(), ExecutionStatus::EXCEPTION);
  if (res == ExecutionStatus::EXCEPTION)
    return false;
  return *res;
}

/// \return the result of JSObject::putComputed_RJS(\p obj, \p key, \p value)
/// with the throwOnError flag. An exception is left in the runtime.
CallResult<bool> putComputedThrow(
    Runtime &runtime,
    Handle<JSObject> obj,
    Handle<> key,
    Handle<> value) {
  return JSObject::putComputed_RJS(
      obj, runtime, key, value, PropOpFlags().plusThrowOnError());
}

/// \return true if \p obj has an own property \p name.
bool hasOwnNamed(Runtime &runtime, Handle<JSObject> obj, SymbolID name) {
  NamedPropertyDescriptor desc;
  return JSObject::getOwnNamedDescriptor(obj, runtime, name, desc);
}

/// Call JSObject::setCachedUsingEpoch on \p proto and on every object
/// above it on its prototype chain.
void premark(Runtime &runtime, Handle<JSObject> proto) {
  struct : public Locals {
    PinnedValue<JSObject> cur;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  for (lv.cur = proto.get(); lv.cur.get() != nullptr;
       lv.cur = lv.cur->getParent(runtime)) {
    JSObject::setCachedUsingEpoch(lv.cur, runtime);
  }
}

/// \return true if \p a and \p b have the same class.
bool sameClass(Handle<JSObject> a, Handle<JSObject> b) {
  return a->getClassGCPtr() == b->getClassGCPtr();
}

/// A typed object and the slots of its two public properties.
struct TypedObject {
  /// The object, in the caller's GCScope.
  Handle<JSObject> obj;
  /// The slot of the first property.
  SlotIndex slotA;
  /// The slot of the second property.
  SlotIndex slotB;
};

/// \return an object with no parent whose class comes from
/// HiddenClass::createForTypedObject, with the public properties \p a holding
/// \p va and \p b holding \p vb. The handle is in the current GCScope.
TypedObject makeTypedObject(
    Runtime &runtime,
    SymbolID a,
    SymbolID b,
    Handle<JSObject> va,
    Handle<JSObject> vb) {
  struct : public Locals {
    PinnedValue<HiddenClass> clazz;
    PinnedValue<JSObject> obj;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  lv.clazz = HiddenClass::createForTypedObject(
      runtime, Runtime::makeNullHandle<JSObject>(), 2);
  SlotIndex slotA =
      HiddenClass::addNewTypedPublicProperty(lv.clazz, runtime, a, true);
  SlotIndex slotB =
      HiddenClass::addNewTypedPublicProperty(lv.clazz, runtime, b, true);
  lv.obj =
      JSObject::create(runtime, Runtime::makeNullHandle<JSObject>(), lv.clazz);
  JSObject::setNamedSlotValueUnsafe(
      *lv.obj,
      runtime,
      slotA,
      SmallHermesValue::encodeObjectValue(*va, runtime));
  JSObject::setNamedSlotValueUnsafe(
      *lv.obj,
      runtime,
      slotB,
      SmallHermesValue::encodeObjectValue(*vb, runtime));
  return TypedObject{runtime.makeHandle(*lv.obj), slotA, slotB};
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

// [2] A put to an existing property fills a writable Slot entry, and a
// planted entry is followed only when writable. Fails without the put
// fill, without the put fast path, and if writable is ignored.
TEST_F(ComputedPropertyCacheTest, PutFillsAndHits) {
  GCScope scope{runtime, "PutFillsAndHits", 128};
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  SymbolID a = sym(runtime, "a");
  SymbolID b = sym(runtime, "b");
  Handle<> keyA = keyOf(runtime, a);
  Handle<> keyB = keyOf(runtime, b);
  Handle<JSObject> v1 = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> v2 = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> v3 = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> v4 = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> o = makeObject(runtime, objProto(runtime), {a, b}, v1);

  cache.clear();
  EXPECT_TRUE(putComputed(runtime, o, keyA, v2));
  // Nothing allocates between the fill and this lookup.
  const ComputedPropertyCache::Entry *e = cache.lookup(o->getClassGCPtr(), a);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->getKind(), ComputedPropertyCache::Kind::Slot);
  EXPECT_EQ(e->slot, 0u);
  EXPECT_TRUE(e->writable);
  // The cache is emptied before each read back, so that the get follows no
  // entry, planted or filled.
  cache.clear();
  EXPECT_TRUE(getIs(runtime, o, keyA, v2));
  EXPECT_TRUE(getIs(runtime, o, keyB, v1));

  {
    SCOPED_TRACE("planted writable: the store goes to b");
    cache.clear();
    cache.insertSlot(runtime, o->getClass(runtime), a, 1, true);
    EXPECT_TRUE(putComputed(runtime, o, keyA, v3));
    cache.clear();
    EXPECT_TRUE(getIs(runtime, o, keyA, v2));
    EXPECT_TRUE(getIs(runtime, o, keyB, v3));
  }
  {
    SCOPED_TRACE("planted not writable: the store goes to a");
    cache.clear();
    cache.insertSlot(runtime, o->getClass(runtime), a, 1, false);
    EXPECT_TRUE(putComputed(runtime, o, keyA, v4));
    cache.clear();
    EXPECT_TRUE(getIs(runtime, o, keyA, v4));
    EXPECT_TRUE(getIs(runtime, o, keyB, v3));
  }
}

// [8] With a planted entry sending a to b, a put with another receiver
// takes the generic path: it stores a on the receiver and leaves the
// object alone. Fails if the receiver test is removed from the put fast
// path.
TEST_F(ComputedPropertyCacheTest, PutReceiver) {
  GCScope scope{runtime, "PutReceiver", 128};
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  SymbolID a = sym(runtime, "a");
  SymbolID b = sym(runtime, "b");
  Handle<> keyA = keyOf(runtime, a);
  Handle<> keyB = keyOf(runtime, b);
  Handle<JSObject> v1 = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> v2 = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> v3 = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> o = makeObject(runtime, objProto(runtime), {a, b}, v1);
  ASSERT_NE(
      JSObject::putNamed_RJS(o, runtime, b, v2).getStatus(),
      ExecutionStatus::EXCEPTION);
  Handle<JSObject> other = makeObject(runtime, objProto(runtime), {}, v1);

  cache.clear();
  cache.insertSlot(runtime, o->getClass(runtime), a, 1, true);
  auto res = JSObject::putComputedWithReceiver_RJS(o, runtime, keyA, v3, other);
  ASSERT_NE(res.getStatus(), ExecutionStatus::EXCEPTION);
  EXPECT_TRUE(*res);
  cache.clear();
  EXPECT_TRUE(getIs(runtime, o, keyA, v1));
  EXPECT_TRUE(getIs(runtime, o, keyB, v2));
  EXPECT_TRUE(getIs(runtime, other, keyA, v3));
}

// [9] A string with the characters of an existing name but without the
// uniqued bit writes the property and does not follow a planted entry.
// Fails if the uniqued test is removed from nameOfKey: getUniqueID
// asserts.
TEST_F(ComputedPropertyCacheTest, PutNonUniquedKey) {
  GCScope scope{runtime, "PutNonUniquedKey", 128};
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  SymbolID a = sym(runtime, "a");
  SymbolID b = sym(runtime, "b");
  Handle<> keyA = keyOf(runtime, a);
  Handle<> keyB = keyOf(runtime, b);
  Handle<JSObject> v1 = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> v2 = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> v3 = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> o = makeObject(runtime, objProto(runtime), {a, b}, v1);
  auto strRes = StringPrimitive::create(runtime, createASCIIRef("a"));
  ASSERT_NE(strRes.getStatus(), ExecutionStatus::EXCEPTION);
  Handle<> plainA = runtime.makeHandle(*strRes);

  cache.clear();
  ASSERT_FALSE(plainA->getString()->isUniqued());
  EXPECT_TRUE(putComputed(runtime, o, plainA, v2));
  EXPECT_EQ(cache.lookup(o->getClassGCPtr(), a), nullptr);
  cache.clear();
  EXPECT_TRUE(getIs(runtime, o, keyA, v2));
  EXPECT_TRUE(getIs(runtime, o, keyB, v1));

  cache.clear();
  ASSERT_FALSE(plainA->getString()->isUniqued());
  cache.insertSlot(runtime, o->getClass(runtime), a, 1, true);
  EXPECT_TRUE(putComputed(runtime, o, plainA, v3));
  cache.clear();
  EXPECT_TRUE(getIs(runtime, o, keyA, v3));
  EXPECT_TRUE(getIs(runtime, o, keyB, v1));
}

// [15, put half] A typed object's public property is read-only: a put
// fails as it does generically, with no entry and with a planted one
// whose writable bit is clear. Fails if the put fast path ignores writable.
TEST_F(ComputedPropertyCacheTest, TypedObjectPut) {
  GCScope scope{runtime, "TypedObjectPut", 128};
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  SymbolID a = sym(runtime, "a");
  SymbolID b = sym(runtime, "b");
  Handle<> keyA = keyOf(runtime, a);
  Handle<JSObject> v1 = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> v2 = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> v3 = runtime.makeHandle(JSObject::create(runtime));
  TypedObject typed = makeTypedObject(runtime, a, b, v1, v2);
  Handle<JSObject> obj = typed.obj;

  cache.clear();
  // Pins the generic result; only the second put can fail on a cache mutation.
  EXPECT_FALSE(putComputed(runtime, obj, keyA, v3));
  EXPECT_EQ(cache.lookup(obj->getClassGCPtr(), a), nullptr);
  EXPECT_TRUE(getIs(runtime, obj, keyA, v1));

  // Nothing allocates between planting the entry and the put.
  cache.insertSlot(runtime, obj->getClass(runtime), a, typed.slotA, false);
  EXPECT_FALSE(putComputed(runtime, obj, keyA, v3));
  cache.clear();
  EXPECT_TRUE(getIs(runtime, obj, keyA, v1));
}

// A property made read-only by Object.defineProperty: with an entry whose
// writable bit is clear planted for it, a put fails and leaves the value.
// Fails if the put fast path ignores writable.
TEST_F(ComputedPropertyCacheTest, ReadOnlyPut) {
  GCScope scope{runtime, "ReadOnlyPut", 128};
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  Handle<Callable> makeReadOnly = compileFn(
      runtime,
      "(function() { var o = {a: {}, b: {}};"
      " Object.defineProperty(o, 'a', {writable: false}); return o; })");
  ASSERT_TRUE(makeReadOnly.get());
  SymbolID a = sym(runtime, "a");
  Handle<> keyA = keyOf(runtime, a);
  Handle<JSObject> v3 = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> o = call0Obj(runtime, makeReadOnly);
  ASSERT_TRUE(o.get());
  ASSERT_TRUE(ComputedPropertyCache::isCacheable(o->getClass(runtime)));
  MutableHandle<> orig{runtime};

  cache.clear();
  orig = getComputed(runtime, o, keyA);
  ASSERT_TRUE(orig->isObject());
  NamedPropertyDescriptor desc;
  ASSERT_TRUE(JSObject::getOwnNamedDescriptor(o, runtime, a, desc));
  ASSERT_FALSE(desc.flags.writable);
  // Nothing allocates between planting the entry and the put.
  cache.insertSlot(runtime, o->getClass(runtime), a, desc.slot, false);
  EXPECT_FALSE(putComputed(runtime, o, keyA, v3));
  cache.clear();
  EXPECT_TRUE(getIs(runtime, o, keyA, orig));
}

// [3] A put that adds fills an Add entry, the next object of the class
// follows it, and a planted entry is followed. Fails without the add fill,
// and, planted, without the Add fast path.
TEST_F(ComputedPropertyCacheTest, AddFillsAndHits) {
  GCScope scope{runtime, "AddFillsAndHits", 128};
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  SymbolID a = sym(runtime, "a");
  SymbolID b = sym(runtime, "b");
  Handle<> keyA = keyOf(runtime, a);
  Handle<JSObject> v = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> proto = runtime.makeHandle(JSObject::create(runtime));
  auto fresh = [&]() {
    return runtime.makeHandle(JSObject::create(runtime, proto));
  };
  Handle<JSObject> o1 = fresh();
  Handle<JSObject> o2 = fresh();
  Handle<JSObject> o3 = fresh();
  Handle<JSObject> ob = fresh();
  ASSERT_NE(
      JSObject::putNamed_RJS(ob, runtime, b, v).getStatus(),
      ExecutionStatus::EXCEPTION);
  premark(runtime, proto);
  ASSERT_TRUE(sameClass(o1, o2) && sameClass(o1, o3));

  cache.clear();
  EXPECT_TRUE(putComputed(runtime, o1, keyA, v));
  // Nothing allocates between the fill and this lookup, or between it and
  // the put on o2.
  const ComputedPropertyCache::Entry *e = cache.lookup(o2->getClassGCPtr(), a);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->getKind(), ComputedPropertyCache::Kind::Add);
  EXPECT_TRUE(e->resultClazz == o1->getClassGCPtr());
  EXPECT_EQ(e->slot, 0u);
  EXPECT_EQ(e->parentEpoch, runtime.getFullParentCacheEpoch());
  EXPECT_TRUE(putComputed(runtime, o2, keyA, v));
  EXPECT_TRUE(sameClass(o2, o1));
  EXPECT_TRUE(getIs(runtime, o2, keyA, v));

  // Send the add of a to the class of ob.
  cache.insertAdd(
      runtime,
      o3->getClass(runtime),
      a,
      ob->getClass(runtime),
      0,
      runtime.getFullParentCacheEpoch());
  EXPECT_TRUE(putComputed(runtime, o3, keyA, v));
  EXPECT_TRUE(sameClass(o3, ob));
}

// [8] A put that adds, with another receiver, adds the property to the
// receiver and records no Add entry, for the object or for the receiver.
// Fails if the receiver test is removed from the add fill's call site:
// the property is then added to the object instead of the receiver.
TEST_F(ComputedPropertyCacheTest, AddReceiver) {
  GCScope scope{runtime, "AddReceiver", 128};
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  SymbolID a = sym(runtime, "a");
  SymbolID b = sym(runtime, "b");
  Handle<> keyA = keyOf(runtime, a);
  Handle<JSObject> v = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> proto = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> o = makeObject(runtime, proto, {}, v);
  Handle<JSObject> other = makeObject(runtime, proto, {b}, v);
  ASSERT_FALSE(sameClass(o, other));
  Handle<HiddenClass> oStart = runtime.makeHandle(o->getClass(runtime));
  Handle<HiddenClass> otherStart = runtime.makeHandle(other->getClass(runtime));

  cache.clear();
  auto res = JSObject::putComputedWithReceiver_RJS(o, runtime, keyA, v, other);
  ASSERT_NE(res.getStatus(), ExecutionStatus::EXCEPTION);
  EXPECT_TRUE(*res);
  // Nothing allocates between the put and these lookups.
  EXPECT_EQ(
      cache.lookup(CompressedPointer::encode(*oStart, runtime), a), nullptr);
  EXPECT_EQ(
      cache.lookup(CompressedPointer::encode(*otherStart, runtime), a),
      nullptr);
  EXPECT_FALSE(hasOwnNamed(runtime, o, a));
  EXPECT_TRUE(hasOwnNamed(runtime, other, a));
  cache.clear();
  EXPECT_TRUE(getIs(runtime, other, keyA, v));
}

// A put that adds with a string key built at run time, which is not
// uniqued, adds the property and records nothing, though the key's name
// exists. Fails if the add site passes cacheComputedAdd without the
// isUniqued() test.
TEST_F(ComputedPropertyCacheTest, ComputedAddNonUniquedKeyRecordsNothing) {
  GCScope scope{runtime, "ComputedAddNonUniquedKeyRecordsNothing", 128};
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  SymbolID a = sym(runtime, "a");
  Handle<> keyA = keyOf(runtime, a);
  Handle<JSObject> v = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> proto = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> o = runtime.makeHandle(JSObject::create(runtime, proto));
  auto strRes = StringPrimitive::create(runtime, createASCIIRef("a"));
  ASSERT_NE(strRes.getStatus(), ExecutionStatus::EXCEPTION);
  Handle<> plainA = runtime.makeHandle(*strRes);
  Handle<HiddenClass> start = runtime.makeHandle(o->getClass(runtime));
  premark(runtime, proto);

  cache.clear();
  unsigned before = cache.numEntries();
  ASSERT_FALSE(plainA->getString()->isUniqued());
  EXPECT_TRUE(putComputed(runtime, o, plainA, v));
  // Nothing allocates between the put and these checks.
  EXPECT_EQ(cache.numEntries(), before);
  EXPECT_EQ(
      cache.lookup(CompressedPointer::encode(*start, runtime), a), nullptr);
  EXPECT_TRUE(hasOwnNamed(runtime, o, a));
  cache.clear();
  EXPECT_TRUE(getIs(runtime, o, keyA, v));
}

// [5] A planted Add entry with a stale epoch is not followed. Nothing
// allocates between the plant and the put, so this fails under every
// collector if the epoch comparison is removed.
TEST_F(ComputedPropertyCacheTest, StaleEpochNotFollowed) {
  GCScope scope{runtime, "StaleEpochNotFollowed", 128};
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  SymbolID a = sym(runtime, "a");
  SymbolID b = sym(runtime, "b");
  Handle<> keyA = keyOf(runtime, a);
  Handle<JSObject> v = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> proto = runtime.makeHandle(JSObject::create(runtime));
  auto fresh = [&]() {
    return runtime.makeHandle(JSObject::create(runtime, proto));
  };
  Handle<JSObject> o4 = fresh();
  Handle<JSObject> ob = fresh();
  ASSERT_NE(
      JSObject::putNamed_RJS(ob, runtime, b, v).getStatus(),
      ExecutionStatus::EXCEPTION);
  Handle<JSObject> oa = fresh();
  ASSERT_NE(
      JSObject::putNamed_RJS(oa, runtime, a, v).getStatus(),
      ExecutionStatus::EXCEPTION);

  cache.clear();
  cache.insertAdd(
      runtime,
      o4->getClass(runtime),
      a,
      ob->getClass(runtime),
      0,
      runtime.getFullParentCacheEpoch() - 1);
  const ComputedPropertyCache::Entry *e = cache.lookup(o4->getClassGCPtr(), a);
  ASSERT_NE(e, nullptr);
  EXPECT_NE(e->parentEpoch, runtime.getFullParentCacheEpoch());
  EXPECT_TRUE(putComputed(runtime, o4, keyA, v));
  EXPECT_TRUE(sameClass(o4, oa));
  EXPECT_FALSE(sameClass(o4, ob));
}

// [4] Two objects get the same 20 names one at a time: the first object's
// put fills, the second follows the entry through the slots in the object,
// the first out-of-line slot and each growth of the storage. Fails if the
// hit stores to the wrong slot or skips the class update.
TEST_F(ComputedPropertyCacheTest, AddAcrossStorage) {
  GCScope scope{runtime, "AddAcrossStorage", 256};
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  constexpr unsigned kNumNames = 20;
  std::vector<SymbolID> names;
  std::vector<Handle<>> keys;
  for (unsigned i = 0; i < kNumNames; ++i) {
    std::string s = "p" + std::to_string(i);
    names.push_back(sym(runtime, s.c_str()));
    keys.push_back(keyOf(runtime, names[i]));
  }
  Handle<JSObject> v = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> proto = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> objA = runtime.makeHandle(JSObject::create(runtime, proto));
  Handle<JSObject> objB = runtime.makeHandle(JSObject::create(runtime, proto));
  premark(runtime, proto);

  cache.clear();
  for (unsigned i = 0; i < kNumNames; ++i) {
    SCOPED_TRACE(i);
    GCScopeMarkerRAII marker{runtime};
    EXPECT_TRUE(putComputed(runtime, objA, keys[i], v));
    // Nothing allocates between the fill, this lookup and the put on B.
    const ComputedPropertyCache::Entry *e =
        cache.lookup(objB->getClassGCPtr(), names[i]);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->getKind(), ComputedPropertyCache::Kind::Add);
    EXPECT_TRUE(e->resultClazz == objA->getClassGCPtr());
    EXPECT_EQ(e->parentEpoch, runtime.getFullParentCacheEpoch());
    EXPECT_TRUE(putComputed(runtime, objB, keys[i], v));
    EXPECT_TRUE(sameClass(objB, objA));
    EXPECT_TRUE(getIs(runtime, objB, keys[i], v));
  }
}

// Like AddAcrossStorage, with the second object created by
// JSObject::create with a property count larger than its class needs, as
// JSON.parse creates one from text with a duplicate key: its storage
// already has the slots the first out-of-line adds give, and the hits must
// store there; the add after them grows the storage. Each name has its own
// value, and after each put every value so far reads back. Fails without
// the in-place store in addNewOwnPropertyInSlot, on its assertion that the
// slot is the storage's size.
TEST_F(ComputedPropertyCacheTest, AddLongStorage) {
  GCScope scope{runtime, "AddLongStorage", 256};
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  // Storage for three out-of-line slots; the fourth out-of-line add grows it.
  constexpr unsigned kNumSlots = JSObject::DIRECT_PROPERTY_SLOTS + 3;
  constexpr unsigned kNumNames = kNumSlots + 1;
  std::vector<SymbolID> names;
  std::vector<Handle<>> keys;
  std::vector<Handle<JSObject>> vals;
  for (unsigned i = 0; i < kNumNames; ++i) {
    std::string s = "p" + std::to_string(i);
    names.push_back(sym(runtime, s.c_str()));
    keys.push_back(keyOf(runtime, names[i]));
    vals.push_back(runtime.makeHandle(JSObject::create(runtime)));
  }
  Handle<JSObject> proto = runtime.makeHandle(JSObject::create(runtime));
  Handle<HiddenClass> root =
      runtime.getHiddenClassForPrototype(*proto, runtime.classJSObject);
  Handle<JSObject> objA = runtime.makeHandle(JSObject::create(runtime, proto));
  Handle<JSObject> objB =
      runtime.makeHandle(JSObject::create(runtime, proto, root, kNumSlots));
  ASSERT_TRUE(sameClass(objA, objB));
  premark(runtime, proto);

  cache.clear();
  for (unsigned i = 0; i < kNumNames; ++i) {
    SCOPED_TRACE(i);
    GCScopeMarkerRAII marker{runtime};
    EXPECT_TRUE(putComputed(runtime, objA, keys[i], vals[i]));
    // Nothing allocates between the fill, this lookup and the put on B.
    const ComputedPropertyCache::Entry *e =
        cache.lookup(objB->getClassGCPtr(), names[i]);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->getKind(), ComputedPropertyCache::Kind::Add);
    EXPECT_EQ(e->slot, i);
    EXPECT_EQ(e->parentEpoch, runtime.getFullParentCacheEpoch());
    EXPECT_TRUE(putComputed(runtime, objB, keys[i], vals[i]));
    EXPECT_TRUE(sameClass(objB, objA));
    for (unsigned j = 0; j <= i; ++j) {
      SCOPED_TRACE(j);
      EXPECT_TRUE(getIs(runtime, objB, keys[j], vals[j]));
    }
  }
}

// [5] After an add is cached, a change on the prototype chain bumps the
// epoch and the next put takes the generic path: a setter on the prototype,
// a read-only property on the prototype, and a replaced prototype of the
// prototype. Asserts results only. Under Hades, where nothing moves, fails
// if the epoch comparison is removed.
TEST_F(ComputedPropertyCacheTest, AddEpoch) {
  GCScope scope{runtime, "AddEpoch", 256};
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  Handle<Callable> defineSetter = compileFn(
      runtime,
      "(function(p, log) { Object.defineProperty(p, 'a',"
      " {set: function(x) { log.v = x; }}); })");
  Handle<Callable> defineReadOnly = compileFn(
      runtime,
      "(function(p) { Object.defineProperty(p, 'a',"
      " {value: 1, writable: false}); })");
  ASSERT_TRUE(defineSetter.get() && defineReadOnly.get());
  SymbolID a = sym(runtime, "a");
  SymbolID vName = sym(runtime, "v");
  Handle<> keyA = keyOf(runtime, a);
  Handle<> keyV = keyOf(runtime, vName);
  Handle<JSObject> v = runtime.makeHandle(JSObject::create(runtime));
  /// \return a new object whose parent is \p parent.
  auto child = [&](Handle<JSObject> parent) {
    return runtime.makeHandle(JSObject::create(runtime, parent));
  };

  {
    SCOPED_TRACE("setter on the prototype");
    Handle<JSObject> proto = runtime.makeHandle(JSObject::create(runtime));
    Handle<JSObject> first = child(proto);
    Handle<JSObject> second = child(proto);
    Handle<JSObject> log = runtime.makeHandle(JSObject::create(runtime));
    cache.clear();
    EXPECT_TRUE(putComputed(runtime, first, keyA, v));
    call2(runtime, defineSetter, proto.getHermesValue(), log.getHermesValue());
    EXPECT_TRUE(putComputed(runtime, second, keyA, v));
    EXPECT_TRUE(getIs(runtime, log, keyV, v));
    EXPECT_FALSE(hasOwnNamed(runtime, second, a));
  }
  {
    SCOPED_TRACE("read-only on the prototype");
    Handle<JSObject> proto = runtime.makeHandle(JSObject::create(runtime));
    Handle<JSObject> first = child(proto);
    Handle<JSObject> second = child(proto);
    Handle<JSObject> third = child(proto);
    cache.clear();
    EXPECT_TRUE(putComputed(runtime, first, keyA, v));
    call2(
        runtime,
        defineReadOnly,
        proto.getHermesValue(),
        HermesValue::encodeUndefinedValue());
    EXPECT_FALSE(putComputed(runtime, second, keyA, v));
    EXPECT_FALSE(hasOwnNamed(runtime, second, a));
    CallResult<bool> res = putComputedThrow(runtime, third, keyA, v);
    EXPECT_EQ(res.getStatus(), ExecutionStatus::EXCEPTION);
    if (res == ExecutionStatus::EXCEPTION) {
      JSObject *thrown = dyn_vmcast<JSObject>(runtime.getThrownValue());
      ASSERT_NE(thrown, nullptr);
      EXPECT_EQ(thrown->getParent(runtime), runtime.TypeErrorPrototype.get());
      runtime.clearThrownValue();
    }
    EXPECT_FALSE(hasOwnNamed(runtime, third, a));
  }
  {
    SCOPED_TRACE("prototype of the prototype replaced");
    Handle<JSObject> proto = runtime.makeHandle(JSObject::create(runtime));
    Handle<JSObject> first = child(proto);
    Handle<JSObject> second = child(proto);
    Handle<JSObject> log = runtime.makeHandle(JSObject::create(runtime));
    Handle<JSObject> withSetter = runtime.makeHandle(JSObject::create(runtime));
    call2(
        runtime,
        defineSetter,
        withSetter.getHermesValue(),
        log.getHermesValue());
    cache.clear();
    EXPECT_TRUE(putComputed(runtime, first, keyA, v));
    auto setRes = JSObject::setParent(*proto, runtime, *withSetter);
    ASSERT_NE(setRes.getStatus(), ExecutionStatus::EXCEPTION);
    ASSERT_TRUE(*setRes);
    EXPECT_TRUE(putComputed(runtime, second, keyA, v));
    EXPECT_TRUE(getIs(runtime, log, keyV, v));
    EXPECT_FALSE(hasOwnNamed(runtime, second, a));
  }
}

// [6] Puts that reach the add fill and must record nothing, and the
// boundary that must. Fails if dictionaryMode is removed from isCacheable
// (65th property), without the add fill (64th property), and if the
// dictionary or the host-object test is removed from
// parentsAllowEpochCaching.
TEST_F(ComputedPropertyCacheTest, AddNotRecorded) {
  GCScope scope{runtime, "AddNotRecorded", 256};
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  constexpr unsigned kThreshold = HiddenClass::kDictionaryThreshold;
  static_assert(kThreshold == 64, "the test names the 64th and 65th");
  std::vector<SymbolID> names;
  for (unsigned i = 0; i <= kThreshold; ++i) {
    std::string s = "q" + std::to_string(i);
    names.push_back(sym(runtime, s.c_str()));
  }
  Handle<> key64 = keyOf(runtime, names[kThreshold]);
  // The 64th name of the 63-property object differs from the 64-property
  // object's, so that the add creates a new class.
  SymbolID r63 = sym(runtime, "r63");
  Handle<> keyR63 = keyOf(runtime, r63);
  SymbolID a = sym(runtime, "a");
  SymbolID t = sym(runtime, "t");
  SymbolID x = sym(runtime, "x");
  Handle<> keyA = keyOf(runtime, a);
  Handle<JSObject> v = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> proto = runtime.makeHandle(JSObject::create(runtime));
  /// \return an object with parent proto and the first \p n names.
  auto withNames = [&](unsigned n) {
    Handle<JSObject> obj = runtime.makeHandle(JSObject::create(runtime, proto));
    for (unsigned i = 0; i < n; ++i) {
      GCScopeMarkerRAII marker{runtime};
      EXPECT_NE(
          JSObject::putNamed_RJS(obj, runtime, names[i], v).getStatus(),
          ExecutionStatus::EXCEPTION);
    }
    return obj;
  };
  MutableHandle<HiddenClass> start{runtime};
  // The cases that must record nothing premark the chain too, and look
  // the entry up right after the put: a wrong fill on an unmarked chain
  // would allocate after writing its entry (marking the chain), and where
  // every allocation moves the heap the entry would be gone before the
  // lookup, hiding the fault.

  {
    SCOPED_TRACE("64 properties adding the 65th: dictionaryMode");
    Handle<JSObject> obj = withNames(kThreshold);
    ASSERT_FALSE(obj->getClass(runtime)->isDictionary());
    start = obj->getClass(runtime);
    premark(runtime, proto);
    cache.clear();
    EXPECT_TRUE(putComputed(runtime, obj, key64, v));
    EXPECT_EQ(
        cache.lookup(
            CompressedPointer::encode(*start, runtime), names[kThreshold]),
        nullptr);
    EXPECT_TRUE(obj->getClass(runtime)->isDictionary());
  }
  {
    SCOPED_TRACE("63 properties adding the 64th: recorded");
    Handle<JSObject> obj = withNames(kThreshold - 1);
    start = obj->getClass(runtime);
    premark(runtime, proto);
    cache.clear();
    EXPECT_TRUE(putComputed(runtime, obj, keyR63, v));
    EXPECT_FALSE(obj->getClass(runtime)->isDictionary());
    // Nothing allocates between the fill and this lookup.
    const ComputedPropertyCache::Entry *e =
        cache.lookup(CompressedPointer::encode(*start, runtime), r63);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->getKind(), ComputedPropertyCache::Kind::Add);
    EXPECT_EQ(e->slot, kThreshold - 1);
    EXPECT_TRUE(e->resultClazz == obj->getClassGCPtr());
  }
  {
    SCOPED_TRACE("dictionary prototype: the chain check");
    Handle<JSObject> dict = makeObject(runtime, objProto(runtime), {x, t}, v);
    auto delRes = JSObject::deleteNamed(dict, runtime, t);
    ASSERT_NE(delRes.getStatus(), ExecutionStatus::EXCEPTION);
    ASSERT_TRUE(*delRes);
    ASSERT_TRUE(dict->getClass(runtime)->isDictionary());
    Handle<JSObject> obj = runtime.makeHandle(JSObject::create(runtime, dict));
    start = obj->getClass(runtime);
    // A dictionary is marked in place and stays a dictionary.
    premark(runtime, dict);
    ASSERT_TRUE(dict->getClass(runtime)->isDictionary());
    ASSERT_TRUE(dict->getClass(runtime)->getIsCachedUsingEpoch());
    cache.clear();
    EXPECT_TRUE(putComputed(runtime, obj, keyA, v));
    EXPECT_EQ(
        cache.lookup(CompressedPointer::encode(*start, runtime), a), nullptr);
    EXPECT_TRUE(hasOwnNamed(runtime, obj, a));
  }
  {
    SCOPED_TRACE("host object prototype: the chain check");
    auto hostRes = HostObject::createWithoutPrototype(
        runtime, std::make_unique<TrivialHostObjectProxy>());
    ASSERT_NE(hostRes.getStatus(), ExecutionStatus::EXCEPTION);
    Handle<JSObject> host = runtime.makeHandle(vmcast<JSObject>(*hostRes));
    Handle<JSObject> obj = runtime.makeHandle(JSObject::create(runtime, host));
    start = obj->getClass(runtime);
    // Marking gives the host a new class that is still a host object's.
    premark(runtime, host);
    ASSERT_TRUE(host->isHostObject(runtime));
    ASSERT_TRUE(host->getClass(runtime)->getIsCachedUsingEpoch());
    cache.clear();
    EXPECT_TRUE(putComputed(runtime, obj, keyA, v));
    EXPECT_EQ(
        cache.lookup(CompressedPointer::encode(*start, runtime), a), nullptr);
    EXPECT_TRUE(hasOwnNamed(runtime, obj, a));
  }
}

// The assumption AddNameCollected rests on: a uniqued string whose SymbolID
// nothing holds loses its uniqued bit in a collection while the string
// itself stays alive.
TEST_F(ComputedPropertyCacheTest, SymbolDiesWhileStringLives) {
  GCScope scope{runtime, "SymbolDiesWhileStringLives", 128};
  MutableHandle<> key{runtime};
  {
    GCScope inner{runtime, "SymbolDiesWhileStringLives inner", 16};
    SymbolID id = sym(runtime, "cpcDyingName");
    key = keyOf(runtime, id).get();
    ASSERT_TRUE(key->getString()->isUniqued());
  }
  runtime.collect("test");
  ASSERT_TRUE(key->isString());
  EXPECT_FALSE(key->getString()->isUniqued());
}

// [10] The put finds an Add entry for its key, and the encode of the value
// collects. The entry's result class is held only by an object that has
// died, and it alone holds the key's name, so the collection empties the
// entry, frees the key's SymbolID and clears the string's uniqued bit. The
// re-probe must test the bit again and miss, and the put add the property
// by the generic path. Only in the Handle-SAN BOXED tree, where the encode
// of 0.5 allocates and every allocation collects, does the re-probe see
// the name freed; there the test fails if the re-probe and its test are
// skipped, and fails on getUniqueID's assertion if the re-probe takes the
// name without testing the uniqued bit. Elsewhere the put follows the
// entry.
TEST_F(ComputedPropertyCacheTest, AddNameCollected) {
  GCScope scope{runtime, "AddNameCollected", 128};
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  Handle<JSObject> proto = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> obj = runtime.makeHandle(JSObject::create(runtime, proto));
  Handle<> num = runtime.makeHandle(HermesValue::encodeTrustedNumberValue(0.5));
  MutableHandle<> key{runtime};
  // Marked, the chain lets the fill below write its entry last.
  premark(runtime, proto);
  {
    GCScope inner{runtime, "AddNameCollected inner", 64};
    SymbolID id = sym(runtime, "cpcCollectedName");
    key = keyOf(runtime, id).get();
    Handle<JSObject> o1 = runtime.makeHandle(JSObject::create(runtime, proto));
    ASSERT_TRUE(sameClass(o1, obj));
    cache.clear();
    EXPECT_TRUE(putComputed(runtime, o1, key, Runtime::getUndefinedValue()));
    // Nothing allocates between the fill and this lookup, or between it and
    // the put on obj.
    const ComputedPropertyCache::Entry *e =
        cache.lookup(obj->getClassGCPtr(), id);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->getKind(), ComputedPropertyCache::Kind::Add);
    EXPECT_TRUE(e->resultClazz == o1->getClassGCPtr());
    EXPECT_EQ(e->parentEpoch, runtime.getFullParentCacheEpoch());
  }
  // Leaving the scope drops o1 and the handle of the name.
  ASSERT_TRUE(key->getString()->isUniqued());
  EXPECT_TRUE(putComputed(runtime, obj, key, num));
  EXPECT_TRUE(getIs(runtime, obj, key, num));
}

// [12, second half] After a collection has emptied the cache, puts of the
// same names on objects created beforehand add and refill. Fails without
// the add fill.
TEST_F(ComputedPropertyCacheTest, CollectRefills) {
  GCScope scope{runtime, "CollectRefills", 128};
  ComputedPropertyCache &cache = runtime.getComputedPropertyCache();
  Handle<JSObject> v = runtime.makeHandle(JSObject::create(runtime));
  Handle<JSObject> proto = runtime.makeHandle(JSObject::create(runtime));
  std::vector<Handle<JSObject>> objs;
  for (unsigned i = 0; i < kNumCollectNames; ++i)
    objs.push_back(runtime.makeHandle(JSObject::create(runtime, proto)));
  Handle<HiddenClass> freshClazz =
      runtime.makeHandle(objs[0]->getClass(runtime));

  collectEmpties(runtime);

  std::vector<SymbolID> names;
  std::vector<Handle<>> keys;
  for (unsigned i = 0; i < kNumCollectNames; ++i) {
    names.push_back(sym(runtime, collectName(i).c_str()));
    keys.push_back(keyOf(runtime, names[i]));
  }
  premark(runtime, proto);
  for (unsigned i = 0; i < kNumCollectNames; ++i) {
    SCOPED_TRACE(i);
    GCScopeMarkerRAII marker{runtime};
    EXPECT_TRUE(putComputed(runtime, objs[i], keys[i], v));
    // Nothing allocates between the fill and this lookup.
    const ComputedPropertyCache::Entry *e =
        cache.lookup(CompressedPointer::encode(*freshClazz, runtime), names[i]);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->getKind(), ComputedPropertyCache::Kind::Add);
    EXPECT_TRUE(e->resultClazz == objs[i]->getClassGCPtr());
  }
}

} // namespace

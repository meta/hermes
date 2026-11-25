/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/HiddenClass.h"

#include "hermes/VM/HiddenClass-inline.h"
#include "hermes/VM/HostModel.h"
#include "hermes/VM/JSCallableProxy.h"
#include "hermes/VM/JSProxy.h"
#include "hermes/VM/Runtime.h"

#include "VMRuntimeTestHelpers.h"

#include "gtest/gtest.h"

using namespace hermes::vm;

namespace {

using HiddenClassTest = LargeHeapRuntimeTestFixture;

TEST_F(HiddenClassTest, SmokeTest) {
  struct : public Locals {
    PinnedValue<SymbolID> aHnd;
    PinnedValue<SymbolID> bHnd;
    PinnedValue<SymbolID> cHnd;
    PinnedValue<SymbolID> dHnd;
    PinnedValue<HiddenClass> addRes;
    PinnedValue<HiddenClass> addRes2;
    PinnedValue<HiddenClass> addRes3;
    PinnedValue<HiddenClass> addRes4;
    PinnedValue<HiddenClass> addRes5;
    PinnedValue<HiddenClass> addRes6;
    PinnedValue<HiddenClass> addRes7;
    PinnedValue<HiddenClass> addRes8;
    PinnedValue<HiddenClass> addRes9;
    PinnedValue<HiddenClass> addRes10;
    PinnedValue<HiddenClass> xSlot;
    PinnedValue<HiddenClass> ySlot;
    PinnedValue<HiddenClass> zSlot;
    PinnedValue<HiddenClass> rootHnd;
    PinnedValue<HiddenClass> newClz;
    PinnedValue<HiddenClass> newClz2;
    PinnedValue<HiddenClass> y1;
    PinnedValue<HiddenClass> y2;
    PinnedValue<HiddenClass> y3;
    PinnedValue<HiddenClass> x1;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  GCScope gcScope{runtime, "HiddenClassTest.SmokeTest", 48};

  runtime.collect("test");

  auto aHnd = *getSymbolHandleForTest(runtime, createUTF16Ref(u"a"), lv.aHnd);
  auto bHnd = *getSymbolHandleForTest(runtime, createUTF16Ref(u"b"), lv.bHnd);
  auto cHnd = *getSymbolHandleForTest(runtime, createUTF16Ref(u"c"), lv.cHnd);
  auto dHnd = *getSymbolHandleForTest(runtime, createUTF16Ref(u"d"), lv.dHnd);

  // We will simulate and verify the following property operations, starting
  // from the same root.
  /// x.a, x.b
  /// y.a, y.b
  /// x.c
  /// y.d
  /// read-only x.b
  /// z.a, z.b, z.c
  /// read-only z.b
  /// all-read-only y twice

  MutableHandle<HiddenClass> x{lv.xSlot};
  MutableHandle<HiddenClass> y{lv.ySlot};
  MutableHandle<HiddenClass> y1{lv.y1};
  MutableHandle<HiddenClass> y2{lv.y2};
  MutableHandle<HiddenClass> y3{lv.y3};
  MutableHandle<HiddenClass> z{lv.zSlot};

  lv.rootHnd = HiddenClass::createRoot(
      runtime, Runtime::makeNullHandle<JSObject>(), ClassFlags{});
  Handle<HiddenClass> rootHnd = lv.rootHnd;

  ASSERT_EQ(0u, rootHnd->getNumProperties());
  ASSERT_FALSE(rootHnd->isDictionary());
  ASSERT_TRUE(rootHnd->isKnownLeaf());
  ASSERT_FALSE(rootHnd->getObjectParentGCPtr());

  // x = {}
  x = *rootHnd;
  {
    // x.a
    auto addRes = addPropertyForTest(
        x,
        runtime,
        *aHnd,
        PropertyFlags::defaultNewNamedPropertyFlags(),
        lv.addRes);
    ASSERT_RETURNED(addRes);
    ASSERT_EQ(0u, addRes->second);
    ASSERT_NE(*rootHnd, *addRes->first);
    x = *addRes->first;
  }
  {
    // x.b
    auto addRes = addPropertyForTest(
        x,
        runtime,
        *bHnd,
        PropertyFlags::defaultNewNamedPropertyFlags(),
        lv.addRes2);
    ASSERT_RETURNED(addRes);
    ASSERT_EQ(1u, addRes->second);
    ASSERT_NE(*x, *addRes->first);
    x = *addRes->first;
  }
  // y = {}
  y = *rootHnd;
  {
    // y.a
    auto addRes = addPropertyForTest(
        y,
        runtime,
        *aHnd,
        PropertyFlags::defaultNewNamedPropertyFlags(),
        lv.addRes3);
    ASSERT_RETURNED(addRes);
    ASSERT_EQ(0u, addRes->second);
    y = *addRes->first;
  }
  {
    // y.b
    auto addRes = addPropertyForTest(
        y,
        runtime,
        *bHnd,
        PropertyFlags::defaultNewNamedPropertyFlags(),
        lv.addRes4);
    ASSERT_RETURNED(addRes);
    ASSERT_EQ(1u, addRes->second);
    y = *addRes->first;
    ASSERT_EQ(*x, *y);
  }
  {
    // x.c
    auto addRes = addPropertyForTest(
        x,
        runtime,
        *cHnd,
        PropertyFlags::defaultNewNamedPropertyFlags(),
        lv.addRes5);
    ASSERT_RETURNED(addRes);
    ASSERT_EQ(2u, addRes->second);
    ASSERT_NE(*x, *addRes->first);
    x = *addRes->first;
    ASSERT_EQ(3u, x->getNumProperties());
    ASSERT_FALSE(x->isDictionary());
    ASSERT_TRUE(x->isKnownLeaf());
  }
  {
    // y.d
    auto addRes = addPropertyForTest(
        y,
        runtime,
        *dHnd,
        PropertyFlags::defaultNewNamedPropertyFlags(),
        lv.addRes6);
    ASSERT_RETURNED(addRes);
    ASSERT_EQ(2u, addRes->second);
    y = *addRes->first;
    ASSERT_NE(*x, *y);
    ASSERT_EQ(3u, y->getNumProperties());
    ASSERT_FALSE(y->isDictionary());
    ASSERT_TRUE(y->isKnownLeaf());
  }

  // Find all properties in x.
  NamedPropertyDescriptor desc;
  {
    auto found = HiddenClass::findProperty(x, runtime, *aHnd, desc);
    ASSERT_TRUE(found);
    ASSERT_EQ(0u, desc.slot);
    found = HiddenClass::findProperty(x, runtime, *bHnd, desc);
    ASSERT_TRUE(found);
    ASSERT_EQ(1u, desc.slot);
    found = HiddenClass::findProperty(x, runtime, *cHnd, desc);
    ASSERT_TRUE(found);
    ASSERT_EQ(2u, desc.slot);
    found = HiddenClass::findProperty(x, runtime, *dHnd, desc);
    ASSERT_FALSE(found);
  }

  {
    // Read-only x.b
    auto found = HiddenClass::findProperty(x, runtime, *bHnd, desc);
    ASSERT_TRUE(found);
    ASSERT_EQ(1u, desc.slot);
    ASSERT_TRUE(desc.flags.writable);

    desc.flags.writable = false;
    lv.newClz = HiddenClass::updateProperty(x, runtime, *found, desc.flags);
    Handle<HiddenClass> newClz = lv.newClz;
    ASSERT_NE(*x, *newClz);
    ASSERT_EQ(x->getNumProperties(), newClz->getNumProperties());
    x = *newClz;

    found = HiddenClass::findProperty(x, runtime, *bHnd, desc);
    ASSERT_TRUE(found);
    ASSERT_EQ(1u, desc.slot);
    ASSERT_FALSE(desc.flags.writable);
  }

  // z = {}
  z = *rootHnd;
  {
    // z.a
    auto addRes = addPropertyForTest(
        z,
        runtime,
        *aHnd,
        PropertyFlags::defaultNewNamedPropertyFlags(),
        lv.addRes7);
    ASSERT_RETURNED(addRes);
    ASSERT_EQ(0u, addRes->second);
    z = *addRes->first;
  }
  {
    // z.b
    auto addRes = addPropertyForTest(
        z,
        runtime,
        *bHnd,
        PropertyFlags::defaultNewNamedPropertyFlags(),
        lv.addRes8);
    ASSERT_RETURNED(addRes);
    ASSERT_EQ(1u, addRes->second);
    z = *addRes->first;
  }
  {
    // z.c
    auto addRes = addPropertyForTest(
        z,
        runtime,
        *cHnd,
        PropertyFlags::defaultNewNamedPropertyFlags(),
        lv.addRes9);
    ASSERT_RETURNED(addRes);
    ASSERT_EQ(2u, addRes->second);
    z = *addRes->first;
  }

  {
    // Read-only z.b
    auto found = HiddenClass::findProperty(z, runtime, *bHnd, desc);
    ASSERT_TRUE(found);
    ASSERT_EQ(1u, desc.slot);
    ASSERT_TRUE(desc.flags.writable);

    desc.flags.writable = false;
    lv.newClz2 = HiddenClass::updateProperty(z, runtime, *found, desc.flags);
    Handle<HiddenClass> newClz = lv.newClz2;
    ASSERT_NE(*z, *newClz);
    ASSERT_EQ(z->getNumProperties(), newClz->getNumProperties());
    z = *newClz;

    found = HiddenClass::findProperty(z, runtime, *bHnd, desc);
    ASSERT_TRUE(found);
    ASSERT_EQ(1u, desc.slot);
    ASSERT_FALSE(desc.flags.writable);

    ASSERT_EQ(*x, *z);
  }

  lv.y1 = HiddenClass::freeze(y, runtime);
  lv.y2 = HiddenClass::freeze(y, runtime);
  ASSERT_EQ(*y1, *y2);
  lv.y3 = HiddenClass::freeze(y1, runtime);
  ASSERT_EQ(*y1, *y3);

  // Turn x into a dictionary by erasing x.a

  {
    auto found = HiddenClass::findProperty(x, runtime, *aHnd, desc);
    ASSERT_TRUE(found);
    ASSERT_EQ(0u, desc.slot);

    lv.x1 = HiddenClass::deleteProperty(x, runtime, *found);
    Handle<HiddenClass> x1 = lv.x1;
    ASSERT_NE(*x, *x1);
    ASSERT_FALSE(x->isDictionary());
    ASSERT_TRUE(x1->isDictionary());
    ASSERT_EQ(2u, x1->getNumProperties());

    found = HiddenClass::findProperty(x1, runtime, *aHnd, desc);
    ASSERT_FALSE(found);

    x = *x1;
  }

  {
    // x.a (again)
    auto addRes = addPropertyForTest(
        x,
        runtime,
        *aHnd,
        PropertyFlags::defaultNewNamedPropertyFlags(),
        lv.addRes10);
    ASSERT_RETURNED(addRes);
    ASSERT_EQ(0u, addRes->second);
    ASSERT_EQ(*x, *addRes->first);
    ASSERT_EQ(3u, x->getNumProperties());
  }
}

TEST_F(HiddenClassTest, AccessorsTest) {
  struct : public Locals {
    PinnedValue<SymbolID> aSym;
    PinnedValue<SymbolID> bSym;
    PinnedValue<SymbolID> cSym;
    PinnedValue<SymbolID> dSym;
    PinnedValue<HiddenClass> addRes;
    PinnedValue<HiddenClass> addRes2;
    PinnedValue<HiddenClass> addRes3;
    PinnedValue<HiddenClass> addRes4;
    PinnedValue<HiddenClass> addRes5;
    PinnedValue<HiddenClass> addRes6;
    PinnedValue<HiddenClass> addRes7;
    PinnedValue<HiddenClass> xSlot;
    PinnedValue<HiddenClass> ySlot;
    PinnedValue<HiddenClass> rootCls;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  GCScope gcScope{runtime, "HiddenClassTest.SmokeTest", 48};
  runtime.collect("test");
  auto aSym = *getSymbolHandleForTest(runtime, createUTF16Ref(u"a"), lv.aSym);
  auto bSym = *getSymbolHandleForTest(runtime, createUTF16Ref(u"b"), lv.bSym);
  auto cSym = *getSymbolHandleForTest(runtime, createUTF16Ref(u"c"), lv.cSym);
  auto dSym = *getSymbolHandleForTest(runtime, createUTF16Ref(u"d"), lv.dSym);
  auto defaultFlags = PropertyFlags::defaultNewNamedPropertyFlags();
  auto accessorFlags = PropertyFlags::defaultNewNamedPropertyFlags();
  accessorFlags.accessor = true;

  // We will simulate and verify the following property operations, starting
  // from the same root.
  /// x.a, x.b, x.c accessor, x.d
  /// y.a accessor, y.b, delete y.a, y.c

  MutableHandle<HiddenClass> x{lv.xSlot};
  MutableHandle<HiddenClass> y{lv.ySlot};

  lv.rootCls = HiddenClass::createRoot(
      runtime, Runtime::makeNullHandle<JSObject>(), ClassFlags{});
  Handle<HiddenClass> rootCls = lv.rootCls;

  ASSERT_FALSE(rootCls->getMayHaveAccessor());

  // x = {}
  x = *rootCls;
  {
    // x.a
    auto addRes =
        addPropertyForTest(x, runtime, *aSym, defaultFlags, lv.addRes);
    x = *addRes->first;
    ASSERT_FALSE(x->getMayHaveAccessor());
  }
  {
    // x.b
    auto addRes =
        addPropertyForTest(x, runtime, *bSym, defaultFlags, lv.addRes2);
    x = *addRes->first;
    ASSERT_FALSE(x->getMayHaveAccessor());
  }
  {
    // x.c accessor
    auto addRes =
        addPropertyForTest(x, runtime, *cSym, accessorFlags, lv.addRes3);
    x = *addRes->first;
    ASSERT_TRUE(x->getMayHaveAccessor());
  }
  {
    // x.d
    auto addRes =
        addPropertyForTest(x, runtime, *dSym, defaultFlags, lv.addRes4);
    x = *addRes->first;
    // Since there may be an accessor in the chain, we must still return
    // true.
    ASSERT_TRUE(x->getMayHaveAccessor());
  }

  // y = {}
  y = *rootCls;
  {
    // y.a
    auto addRes =
        addPropertyForTest(y, runtime, *aSym, accessorFlags, lv.addRes5);
    y = *addRes->first;
    ASSERT_TRUE(y->getMayHaveAccessor());
  }
  {
    // y.b
    auto addRes =
        addPropertyForTest(y, runtime, *bSym, defaultFlags, lv.addRes6);
    y = *addRes->first;
    ASSERT_TRUE(y->getMayHaveAccessor());
  }
  {
    // delete y.a
    NamedPropertyDescriptor desc;
    auto found = HiddenClass::findProperty(y, runtime, *aSym, desc);
    ASSERT_TRUE(found);
    ASSERT_EQ(0u, desc.slot);
    y = HiddenClass::deleteProperty(y, runtime, *found);
    ASSERT_TRUE(y->getMayHaveAccessor());
  }
  {
    // y.d
    auto addRes =
        addPropertyForTest(y, runtime, *cSym, defaultFlags, lv.addRes7);
    y = *addRes->first;
    // Since there may still be an accessor in the chain, we must still return
    // true.
    ASSERT_TRUE(y->getMayHaveAccessor());
  }
}

/// findPropertyNoAlloc() on classes without a property map, below a flags
/// transition of the property looked up: the slot must come from the class
/// that added the property, the flags from the newest flags transition.
TEST_F(HiddenClassTest, FindPropertyNoAllocFlagsTransitionTest) {
  struct : public Locals {
    PinnedValue<SymbolID> aHnd;
    PinnedValue<SymbolID> bHnd;
    PinnedValue<SymbolID> cHnd;
    PinnedValue<SymbolID> dHnd;
    PinnedValue<SymbolID> eHnd;
    PinnedValue<HiddenClass> clazzSlot;
    PinnedValue<HiddenClass> cClass;
    PinnedValue<HiddenClass> flagsClass;
    PinnedValue<HiddenClass> dClass;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);

  auto aHnd = *getSymbolHandleForTest(runtime, createUTF16Ref(u"a"), lv.aHnd);
  auto bHnd = *getSymbolHandleForTest(runtime, createUTF16Ref(u"b"), lv.bHnd);
  auto cHnd = *getSymbolHandleForTest(runtime, createUTF16Ref(u"c"), lv.cHnd);
  auto dHnd = *getSymbolHandleForTest(runtime, createUTF16Ref(u"d"), lv.dHnd);
  auto eHnd = *getSymbolHandleForTest(runtime, createUTF16Ref(u"e"), lv.eHnd);

  MutableHandle<HiddenClass> clazz{lv.clazzSlot};
  clazz = HiddenClass::createRoot(
      runtime, Runtime::makeNullHandle<JSObject>(), ClassFlags{});
  auto add = [this, &clazz](Handle<SymbolID> name) {
    auto addRes = addPropertyForTest(
        clazz,
        runtime,
        *name,
        PropertyFlags::defaultNewNamedPropertyFlags(),
        clazz);
    ASSERT_RETURNED(addRes);
  };
  // Make the existing property \p name non-writable, then non-enumerable:
  // two flags transitions.
  auto update = [this, &clazz](Handle<SymbolID> name) {
    NamedPropertyDescriptor desc;
    auto found = HiddenClass::findProperty(clazz, runtime, *name, desc);
    ASSERT_TRUE(found);
    desc.flags.writable = false;
    clazz =
        HiddenClass::updateProperty(clazz, runtime, *found, desc.flags).get();
    found = HiddenClass::findProperty(clazz, runtime, *name, desc);
    ASSERT_TRUE(found);
    desc.flags.enumerable = false;
    clazz =
        HiddenClass::updateProperty(clazz, runtime, *found, desc.flags).get();
  };

  // {a, b, c}, then b's flags change twice. b keeps slot 1, while the
  // transition classes for b have three properties.
  add(aHnd);
  add(bHnd);
  add(cHnd);
  lv.cClass = *clazz;
  Handle<HiddenClass> cClass = lv.cClass;
  update(bHnd);
  lv.flagsClass = *clazz;
  Handle<HiddenClass> flagsClass = lv.flagsClass;
  // Adding d moves the map from flagsClass to the new class, and adding e
  // moves it on again, so neither flagsClass nor dClass has a map.
  add(dHnd);
  lv.dClass = *clazz;
  Handle<HiddenClass> dClass = lv.dClass;
  add(eHnd);

  for (Handle<HiddenClass> start : {flagsClass, dClass}) {
    auto desc = HiddenClass::findPropertyNoAlloc(*start, runtime, *bHnd);
    ASSERT_TRUE(desc.hasValue());
    EXPECT_EQ(1u, desc->slot);
    EXPECT_FALSE(desc->flags.writable);
    EXPECT_FALSE(desc->flags.enumerable);
    EXPECT_FALSE(desc->flags.flagsTransition);
  }
  // findProperty() rebuilds a map on cClass, which predates the flags
  // transitions. The walk from flagsClass then takes b's slot from that map,
  // and the newest flags must still replace the map's stale ones.
  {
    NamedPropertyDescriptor cDesc;
    ASSERT_TRUE(HiddenClass::findProperty(cClass, runtime, *bHnd, cDesc));
    ASSERT_TRUE(cDesc.flags.writable);
    auto desc = HiddenClass::findPropertyNoAlloc(*flagsClass, runtime, *bHnd);
    ASSERT_TRUE(desc.hasValue());
    EXPECT_EQ(1u, desc->slot);
    EXPECT_FALSE(desc->flags.writable);
    EXPECT_FALSE(desc->flags.enumerable);
    EXPECT_FALSE(desc->flags.flagsTransition);
  }
  // A property added below the flags transitions is unaffected.
  auto dDesc = HiddenClass::findPropertyNoAlloc(*dClass, runtime, *dHnd);
  ASSERT_TRUE(dDesc.hasValue());
  EXPECT_EQ(3u, dDesc->slot);
}

TEST_F(HiddenClassTest, UpdatePropertyFlagsWithoutTransitionsTest) {
  struct : public Locals {
    PinnedValue<SymbolID> aHnd;
    PinnedValue<SymbolID> bHnd;
    PinnedValue<SymbolID> cHnd;
    PinnedValue<SymbolID> dHnd;
    PinnedValue<HiddenClass> addRes;
    PinnedValue<HiddenClass> addRes2;
    PinnedValue<HiddenClass> addRes3;
    PinnedValue<HiddenClass> addRes4;
    PinnedValue<HiddenClass> ySlot;
    PinnedValue<HiddenClass> yClone;
    PinnedValue<HiddenClass> y3;
    PinnedValue<HiddenClass> y4;
    PinnedValue<HiddenClass> partlyFrozenSingleton;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  GCScope gcScope{
      runtime, "HiddenClassTest.UpdatePropertyFlagsWithoutTransitionsTest", 48};

  runtime.collect("test");

  auto aHnd = *getSymbolHandleForTest(runtime, createUTF16Ref(u"a"), lv.aHnd);
  auto bHnd = *getSymbolHandleForTest(runtime, createUTF16Ref(u"b"), lv.bHnd);
  auto cHnd = *getSymbolHandleForTest(runtime, createUTF16Ref(u"c"), lv.cHnd);
  auto dHnd = *getSymbolHandleForTest(runtime, createUTF16Ref(u"d"), lv.dHnd);

  // Add y.a, y.b, y.c
  lv.ySlot = HiddenClass::createRoot(
      runtime, Runtime::makeNullHandle<JSObject>(), ClassFlags{});
  MutableHandle<HiddenClass> y{lv.ySlot};
  {
    // y.a
    auto addRes = addPropertyForTest(
        y,
        runtime,
        *aHnd,
        PropertyFlags::defaultNewNamedPropertyFlags(),
        lv.addRes);
    ASSERT_RETURNED(addRes);
    ASSERT_EQ(0u, addRes->second);
    y = *addRes->first;
  }
  {
    // y.b
    auto addRes = addPropertyForTest(
        y,
        runtime,
        *bHnd,
        PropertyFlags::defaultNewNamedPropertyFlags(),
        lv.addRes2);
    ASSERT_RETURNED(addRes);
    ASSERT_EQ(1u, addRes->second);
    y = *addRes->first;
  }
  {
    // y.c
    auto addRes = addPropertyForTest(
        y,
        runtime,
        *cHnd,
        PropertyFlags::defaultNewNamedPropertyFlags(),
        lv.addRes3);
    ASSERT_RETURNED(addRes);
    ASSERT_EQ(2u, addRes->second);
    y = *addRes->first;
  }

  NamedPropertyDescriptor desc;

  PropertyFlags clearFlags;
  clearFlags.writable = 1;
  clearFlags.configurable = 1;
  PropertyFlags setFlags;

  ASSERT_FALSE(y->isDictionary());
  // y is not a dictionary so we will get a new hidden class.
  lv.yClone = HiddenClass::updatePropertyFlagsWithoutTransitions(
      y, runtime, clearFlags, setFlags, llvh::None);
  Handle<HiddenClass> yClone = lv.yClone;
  ASSERT_NE(*y, *yClone);
  ASSERT_EQ(y->getNumProperties(), yClone->getNumProperties());
  ASSERT_TRUE(yClone->isDictionary());
  // Check each property
  auto found = HiddenClass::findProperty(yClone, runtime, *aHnd, desc);
  ASSERT_TRUE(found);
  ASSERT_EQ(0u, desc.slot);
  ASSERT_FALSE(desc.flags.writable);
  ASSERT_FALSE(desc.flags.configurable);

  found = HiddenClass::findProperty(yClone, runtime, *bHnd, desc);
  ASSERT_TRUE(found);
  ASSERT_EQ(1u, desc.slot);
  ASSERT_FALSE(desc.flags.writable);
  ASSERT_FALSE(desc.flags.configurable);

  found = HiddenClass::findProperty(yClone, runtime, *cHnd, desc);
  ASSERT_TRUE(found);
  ASSERT_EQ(2u, desc.slot);
  ASSERT_FALSE(desc.flags.writable);
  ASSERT_FALSE(desc.flags.configurable);

  // Turn y into a dictionary y3 by deleting y.a
  found = HiddenClass::findProperty(y, runtime, *aHnd, desc);
  ASSERT_TRUE(found);
  ASSERT_EQ(0u, desc.slot);

  lv.y3 = HiddenClass::deleteProperty(y, runtime, *found);
  Handle<HiddenClass> y3 = lv.y3;
  ASSERT_NE(*y, *y3);
  ASSERT_TRUE(y3->isDictionary());
  ASSERT_EQ(2u, y3->getNumProperties());
  // We should not create a new hidden class in this case.
  lv.y4 = HiddenClass::updatePropertyFlagsWithoutTransitions(
      y3, runtime, clearFlags, setFlags, llvh::None);
  Handle<HiddenClass> y4 = lv.y4;
  ASSERT_EQ(*y4, *y3);

  // Only freeze y.a and y.c
  std::vector<SymbolID> propsToFreeze;
  propsToFreeze.push_back(*aHnd);
  propsToFreeze.push_back(*cHnd);
  propsToFreeze.push_back(*dHnd); // This is not in the map yet.
  // Freeze while only create a singleton hidden class.
  lv.partlyFrozenSingleton = HiddenClass::updatePropertyFlagsWithoutTransitions(
      y,
      runtime,
      clearFlags,
      setFlags,
      llvh::ArrayRef<SymbolID>(propsToFreeze));
  Handle<HiddenClass> partlyFrozenSingleton = lv.partlyFrozenSingleton;

  ASSERT_NE(*y, *partlyFrozenSingleton);
  ASSERT_EQ(y->getNumProperties(), partlyFrozenSingleton->getNumProperties());
  ASSERT_TRUE(partlyFrozenSingleton->isDictionary());
  // Check each property
  found =
      HiddenClass::findProperty(partlyFrozenSingleton, runtime, *aHnd, desc);
  ASSERT_TRUE(found);
  ASSERT_EQ(0u, desc.slot);
  ASSERT_FALSE(desc.flags.writable);
  ASSERT_FALSE(desc.flags.configurable);

  found =
      HiddenClass::findProperty(partlyFrozenSingleton, runtime, *bHnd, desc);
  ASSERT_TRUE(found);
  ASSERT_EQ(1u, desc.slot);
  ASSERT_TRUE(desc.flags.writable);
  ASSERT_TRUE(desc.flags.configurable);

  found =
      HiddenClass::findProperty(partlyFrozenSingleton, runtime, *cHnd, desc);
  ASSERT_TRUE(found);
  ASSERT_EQ(2u, desc.slot);
  ASSERT_FALSE(desc.flags.writable);
  ASSERT_FALSE(desc.flags.configurable);

  // We can still add another property to it.
  auto addRes = addPropertyForTest(
      partlyFrozenSingleton,
      runtime,
      *dHnd,
      PropertyFlags::defaultNewNamedPropertyFlags(),
      lv.addRes4);
  ASSERT_RETURNED(addRes);
  ASSERT_EQ(3u, addRes->second);
  ASSERT_EQ(*addRes->first, *partlyFrozenSingleton);
  ASSERT_EQ(addRes->first->getNumProperties(), 4);
}

TEST_F(HiddenClassTest, ForEachProperty) {
  struct : public Locals {
    PinnedValue<SymbolID> aHnd;
    PinnedValue<SymbolID> bHnd;
    PinnedValue<HiddenClass> addRes;
    PinnedValue<HiddenClass> addRes2;
    PinnedValue<HiddenClass> clazzSlot;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  lv.clazzSlot = HiddenClass::createRoot(
      runtime, Runtime::makeNullHandle<JSObject>(), ClassFlags{});
  MutableHandle<HiddenClass> clazz{lv.clazzSlot};

  auto aHnd = *getSymbolHandleForTest(runtime, createUTF16Ref(u"a"), lv.aHnd);
  auto bHnd = *getSymbolHandleForTest(runtime, createUTF16Ref(u"b"), lv.bHnd);

  {
    // clazz.a
    auto addRes = addPropertyForTest(
        clazz,
        runtime,
        *aHnd,
        PropertyFlags::defaultNewNamedPropertyFlags(),
        lv.addRes);
    ASSERT_RETURNED(addRes);
    ASSERT_EQ(0u, addRes->second);
    clazz = *addRes->first;
  }
  {
    // clazz.b
    auto addRes = addPropertyForTest(
        clazz,
        runtime,
        *bHnd,
        PropertyFlags::defaultNewNamedPropertyFlags(),
        lv.addRes2);
    ASSERT_RETURNED(addRes);
    ASSERT_EQ(1u, addRes->second);
    clazz = *addRes->first;
  }

  std::vector<std::pair<SymbolID, NamedPropertyDescriptor>> expectedProperties{
      {aHnd.get(),
       NamedPropertyDescriptor{
           PropertyFlags::defaultNewNamedPropertyFlags(), 0}},
      {bHnd.get(),
       NamedPropertyDescriptor{
           PropertyFlags::defaultNewNamedPropertyFlags(), 1}}};

  std::vector<std::pair<SymbolID, NamedPropertyDescriptor>> properties;
  HiddenClass::forEachProperty(
      clazz, runtime, [&properties](SymbolID id, NamedPropertyDescriptor desc) {
        properties.emplace_back(id, desc);
      });
  EXPECT_EQ(expectedProperties, properties);

  std::vector<std::pair<SymbolID, NamedPropertyDescriptor>> propertiesNoAlloc;
  HiddenClass::forEachPropertyNoAlloc(
      clazz.get(),
      runtime,
      [&propertiesNoAlloc](SymbolID id, NamedPropertyDescriptor desc) {
        propertiesNoAlloc.emplace_back(id, desc);
      });
  EXPECT_EQ(expectedProperties, propertiesNoAlloc);
}

TEST_F(HiddenClassTest, TypedObjectTest) {
  struct : public Locals {
    PinnedValue<SymbolID> aHnd;
    PinnedValue<HiddenClass> hc;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  Handle<SymbolID> aHnd =
      *getSymbolHandleForTest(runtime, createASCIIRef("a"), lv.aHnd);

  lv.hc = vmcast<HiddenClass>(HiddenClass::createForTypedObject(
      runtime, Runtime::makeNullHandle<JSObject>(), 2));
  // Shouldn't do anything, the rest of the test should run.
  lv.hc->clearPropertyMap(runtime.getHeap());

  EXPECT_FALSE(lv.hc->isDictionary());
  EXPECT_EQ(0, lv.hc->getNumProperties());

  HiddenClass::addNewTypedPrivateProperty(lv.hc, runtime);
  EXPECT_EQ(1, lv.hc->getNumProperties());

  {
    // Ensure there's nothing in the property map.
    uint32_t count = 0;
    HiddenClass::forEachProperty(
        lv.hc, runtime, [&count](SymbolID, NamedPropertyDescriptor) {
          ++count;
        });
    EXPECT_EQ(0, count);
  }

  HiddenClass::addNewTypedPublicProperty(
      lv.hc, runtime, *aHnd, /* propertiesEnumerable */ true);
  EXPECT_EQ(2, lv.hc->getNumProperties());
  {
    // Ensure there's just one symbol.
    uint32_t count = 0;
    HiddenClass::forEachProperty(
        lv.hc,
        runtime,
        [&count, &aHnd](SymbolID name, NamedPropertyDescriptor desc) {
          EXPECT_EQ(name, *aHnd);
          EXPECT_TRUE(desc.flags.enumerable);
          EXPECT_FALSE(desc.flags.writable);
          EXPECT_FALSE(desc.flags.configurable);
          ++count;
        });
    EXPECT_EQ(1, count);
  }

  // Make sure we can find the property.
  {
    NamedPropertyDescriptor desc;
    auto pos = HiddenClass::findProperty(
        createPseudoHandle(*lv.hc), runtime, *aHnd, desc);
    EXPECT_TRUE(pos.hasValue());
  }
}

TEST_F(HiddenClassTest, ObjectParentTest) {
  struct : public Locals {
    PinnedValue<HiddenClass> clazz;
    PinnedValue<JSObject> obj1;
    PinnedValue<JSObject> tmp;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.clazz = HiddenClass::createRoot(
      runtime, Runtime::makeNullHandle<JSObject>(), ClassFlags{});
  EXPECT_TRUE(lv.clazz->getObjectParent(runtime) == nullptr);

  lv.obj1 = JSObject::create(runtime);
  lv.clazz = HiddenClass::updateObjectParent(
      lv.clazz, runtime, createPseudoHandle(*lv.obj1));
  EXPECT_TRUE(lv.clazz->getObjectParent(runtime) == *lv.obj1);

  lv.clazz = HiddenClass::updateObjectParent(
      lv.clazz, runtime, createPseudoHandle<JSObject>(nullptr));
  EXPECT_TRUE(lv.clazz->getObjectParent(runtime) == nullptr);

  for (size_t i = 0; i < 10; ++i) {
    lv.tmp = JSObject::create(runtime);
    lv.clazz = HiddenClass::updateObjectParent(
        lv.clazz, runtime, createPseudoHandle(*lv.tmp));
  }

  EXPECT_TRUE(lv.clazz->isDictionary());
}

TEST_F(HiddenClassTest, ObjectParentTransitionReuseTest) {
  struct : public Locals {
    PinnedValue<HiddenClass> root;
    PinnedValue<HiddenClass> first;
    PinnedValue<HiddenClass> firstNull;
    PinnedValue<HiddenClass> second;
    PinnedValue<JSObject> obj1;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.root = HiddenClass::createRoot(
      runtime, Runtime::makeNullHandle<JSObject>(), ClassFlags{});
  lv.obj1 = JSObject::create(runtime);

  // Repeating the same transition must hit parentTransitionMap_ and hand back
  // the same class, otherwise every parent change would invalidate the
  // property caches.
  lv.first = HiddenClass::updateObjectParent(
      lv.root, runtime, createPseudoHandle(*lv.obj1));
  lv.second = HiddenClass::updateObjectParent(
      lv.root, runtime, createPseudoHandle(*lv.obj1));
  EXPECT_EQ(*lv.first, *lv.second);

  // Same for the null parent.
  lv.firstNull = HiddenClass::updateObjectParent(
      lv.first, runtime, createPseudoHandle<JSObject>(nullptr));
  lv.second = HiddenClass::updateObjectParent(
      lv.first, runtime, createPseudoHandle<JSObject>(nullptr));
  EXPECT_EQ(*lv.firstNull, *lv.second);
  EXPECT_TRUE(lv.firstNull->getObjectParent(runtime) == nullptr);
}

TEST_F(HiddenClassTest, ObjectParentTransitionCycleIsCollectedTest) {
  struct : public Locals {
    PinnedValue<HiddenClass> root;
    PinnedValue<HiddenClass> child;
    PinnedValue<JSObject> proto;
    PinnedValue<JSObject> instance;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.root = HiddenClass::createRoot(
      runtime, Runtime::makeNullHandle<JSObject>(), ClassFlags{});
  lv.proto = JSObject::create(runtime);
  lv.child = HiddenClass::updateObjectParent(
      lv.root, runtime, createPseudoHandle(*lv.proto));
  lv.instance = JSObject::create(runtime, lv.proto, lv.child);

  {
    GCScopeMarkerRAII marker{runtime};
    ASSERT_NE(
        JSObject::putNamed_RJS(
            lv.proto,
            runtime,
            Predefined::getSymbolID(Predefined::length),
            lv.instance),
        ExecutionStatus::EXCEPTION);
  }

  WeakRef<HiddenClass> weakChild{runtime, lv.child};
  WeakRef<JSObject> weakProto{runtime, lv.proto};
  lv.child = nullptr;
  lv.proto = nullptr;
  lv.instance = JSObject::create(runtime);
  lv.instance = nullptr;
  runtime.collect("test");

  EXPECT_FALSE(weakChild.isValid());
  EXPECT_FALSE(weakProto.isValid());
  weakChild.releaseSlot();
  weakProto.releaseSlot();
}

} // namespace

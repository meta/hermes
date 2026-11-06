/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JIT/Config.h"

#if HERMESVM_JIT

#include "../../lib/VM/JIT/JitHandlers.h"
#include "VMRuntimeTestHelpers.h"

#include "hermes/VM/HiddenClass.h"
#include "hermes/VM/JSObject.h"

#include "gtest/gtest.h"

using namespace hermes::vm;

namespace {

/// A large heap: the typed holder below has 65537 slots.
using JitChainWalkTest = LargeHeapRuntimeTestFixture;

/// \return the symbol for \p s. Its handle stays in the current GCScope,
/// which keeps the symbol alive.
SymbolID sym(Runtime &runtime, const char *s) {
  return **runtime.getIdentifierTable().getSymbolHandle(
      runtime, createASCIIRef(s));
}

/// Store \p v into \p obj's property \p name, adding it if absent.
void put(Runtime &runtime, Handle<JSObject> obj, SymbolID name, double v) {
  GCScopeMarkerRAII marker{runtime};
  ASSERT_TRUE(*JSObject::putNamed_RJS(
      obj,
      runtime,
      name,
      runtime.makeHandle(HermesValue::encodeTrustedNumberValue(v))));
}

/// Make \p obj a dictionary by adding and deleting a property \p t.
void makeDictionary(Runtime &runtime, Handle<JSObject> obj, SymbolID t) {
  put(runtime, obj, t, 0);
  ASSERT_TRUE(*JSObject::deleteNamed(obj, runtime, t));
  ASSERT_TRUE(obj->getClass(runtime)->isDictionary());
}

// The slot limit is the read cache's (ReadPropertyCacheEntry::kMaxSlot,
// 0xffff). Only typed classes can have a non-dictionary slot that large,
// so a typed holder reached at depth 2 has 65535 private slots, `a` at
// 0xffff and `b` at 0x10000. Fails if the limit is missing (b is Found)
// or off by one (a is Rejected).
TEST_F(JitChainWalkTest, TypedHolderSlotLimit) {
  GCScope scope{runtime, "TypedHolderSlotLimit", 128};
  struct : public Locals {
    PinnedValue<HiddenClass> clazz;
    PinnedValue<JSObject> holder;
    PinnedValue<JSObject> mid;
    PinnedValue<JSObject> recv;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  SymbolID a = sym(runtime, "a");
  SymbolID b = sym(runtime, "b");

  lv.clazz = HiddenClass::createForTypedObject(
      runtime, Runtime::makeNullHandle<JSObject>(), 2);
  for (unsigned i = 0; i < 0xffff; ++i)
    HiddenClass::addNewTypedPrivateProperty(lv.clazz, runtime);
  EXPECT_EQ(
      0xffffu,
      HiddenClass::addNewTypedPublicProperty(lv.clazz, runtime, a, true));
  EXPECT_EQ(
      0x10000u,
      HiddenClass::addNewTypedPublicProperty(lv.clazz, runtime, b, true));
  lv.holder =
      JSObject::create(runtime, Runtime::makeNullHandle<JSObject>(), lv.clazz);
  lv.mid = JSObject::create(runtime, lv.holder);
  lv.recv = JSObject::create(runtime, lv.mid);

  JitChainWalk w = jitWalkChain(runtime, *lv.recv, a);
  ASSERT_EQ(JitChainWalk::Status::Found, w.status);
  EXPECT_EQ(*lv.holder, w.holder);
  EXPECT_EQ(0xffffu, w.slot);
  EXPECT_EQ(2, (int)w.depth);
  // mid is not flagged; the typed holder counts as flagged.
  EXPECT_FALSE(w.allFlagged);

  w = jitWalkChain(runtime, *lv.recv, b);
  EXPECT_EQ(JitChainWalk::Status::Rejected, w.status);

  JSObject::setCachedUsingEpoch(lv.mid, runtime);
  w = jitWalkChain(runtime, *lv.recv, a);
  ASSERT_EQ(JitChainWalk::Status::Found, w.status);
  EXPECT_TRUE(w.allFlagged);
}

// A holder whose class has no property map and whose `p` went through a
// flags transition: holder and sibling are {p: 1, q: 2} with p then made
// non-enumerable, which keeps p at slot 0 in a flags-transition child C
// (HiddenClass.cpp:625-698); both share C. Adding r to sibling moves C's
// map to C's new child (HiddenClass.cpp:565-568), so the second walk
// falls back to findPropertyNoAlloc on C. Fails if the fallback is
// missing (Rejected), and with adee03e65's HiddenClass.cpp hunk reverted
// (slot 1, the last slot).
TEST_F(JitChainWalkTest, FlagsTransitionHolderWithoutMap) {
  GCScope scope{runtime, "FlagsTransitionHolderWithoutMap", 128};
  struct : public Locals {
    PinnedValue<JSObject> holder;
    PinnedValue<JSObject> sibling;
    PinnedValue<JSObject> mid;
    PinnedValue<JSObject> recv;
    PinnedValue<HiddenClass> c;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  SymbolID p = sym(runtime, "p");
  SymbolID q = sym(runtime, "q");
  SymbolID r = sym(runtime, "r");

  DefinePropertyFlags nonEnum{};
  nonEnum.setEnumerable = 1;
  nonEnum.enumerable = 0;
  lv.holder = JSObject::create(runtime, Runtime::makeNullHandle<JSObject>());
  put(runtime, lv.holder, p, 1);
  put(runtime, lv.holder, q, 2);
  ASSERT_TRUE(*JSObject::defineOwnProperty(
      lv.holder, runtime, p, nonEnum, Runtime::getUndefinedValue()));
  lv.sibling = JSObject::create(runtime, Runtime::makeNullHandle<JSObject>());
  put(runtime, lv.sibling, p, 1);
  put(runtime, lv.sibling, q, 2);
  ASSERT_TRUE(*JSObject::defineOwnProperty(
      lv.sibling, runtime, p, nonEnum, Runtime::getUndefinedValue()));
  lv.c = lv.holder->getClass(runtime);
  ASSERT_EQ(*lv.c, lv.sibling->getClass(runtime));
  lv.mid = JSObject::create(runtime, lv.holder);
  lv.recv = JSObject::create(runtime, lv.mid);

  JitChainWalk w = jitWalkChain(runtime, *lv.recv, p);
  ASSERT_EQ(JitChainWalk::Status::Found, w.status);
  EXPECT_EQ(0u, w.slot);

  put(runtime, lv.sibling, r, 3);
  ASSERT_EQ(*lv.c, lv.holder->getClass(runtime));
  w = jitWalkChain(runtime, *lv.recv, p);
  ASSERT_EQ(JitChainWalk::Status::Found, w.status);
  EXPECT_EQ(0u, w.slot);
  EXPECT_EQ(2, (int)w.depth);
  EXPECT_EQ(*lv.holder, w.holder);
}

// The verdicts besides Found, and what allFlagged reports. o[0] holds p
// and q's absence is tested; o[i] has parent o[i - 1], so p is i
// prototypes up from o[i]. Each EXPECT names the rule it pins.
TEST_F(JitChainWalkTest, Verdicts) {
  GCScope scope{runtime, "Verdicts", 128};
  struct : public Locals {
    PinnedValue<JSObject> o[10];
    PinnedValue<JSObject> dictRecv;
    PinnedValue<JSObject> dictMid;
    PinnedValue<JSObject> viaDict;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  SymbolID p = sym(runtime, "p");
  SymbolID q = sym(runtime, "q");
  SymbolID t = sym(runtime, "t");

  lv.o[0] = JSObject::create(runtime, Runtime::makeNullHandle<JSObject>());
  put(runtime, lv.o[0], p, 1);
  for (unsigned i = 1; i < 10; ++i)
    lv.o[i] = JSObject::create(runtime, lv.o[i - 1]);

  // Own property: Rejected, decided by the receiver's class alone.
  JitChainWalk w = jitWalkChain(runtime, *lv.o[0], p);
  EXPECT_EQ(JitChainWalk::Status::Rejected, w.status);
  EXPECT_TRUE(w.allFlagged);
  // Depth 1 is the prototype entry's job; o[0] is not flagged yet.
  w = jitWalkChain(runtime, *lv.o[1], p);
  EXPECT_EQ(JitChainWalk::Status::Rejected, w.status);
  EXPECT_FALSE(w.allFlagged);
  // Depth 2 and depth 8 (kMaxChainDepth) are Found.
  w = jitWalkChain(runtime, *lv.o[2], p);
  ASSERT_EQ(JitChainWalk::Status::Found, w.status);
  EXPECT_EQ(2, (int)w.depth);
  EXPECT_EQ(*lv.o[0], w.holder);
  EXPECT_FALSE(w.allFlagged);
  w = jitWalkChain(runtime, *lv.o[8], p);
  ASSERT_EQ(JitChainWalk::Status::Found, w.status);
  EXPECT_EQ(8, (int)w.depth);
  // Depth 9: not found within kMaxChainDepth.
  w = jitWalkChain(runtime, *lv.o[9], p);
  EXPECT_EQ(JitChainWalk::Status::Rejected, w.status);
  // Not found at all.
  w = jitWalkChain(runtime, *lv.o[3], q);
  EXPECT_EQ(JitChainWalk::Status::Rejected, w.status);

  // Flag o[1] and o[0]: the depth-2 verdict now depends only on flagged
  // objects. Flagging gives o[0] a new, mapless class; p keeps slot 0.
  JSObject::setCachedUsingEpoch(lv.o[1], runtime);
  JSObject::setCachedUsingEpoch(lv.o[0], runtime);
  w = jitWalkChain(runtime, *lv.o[2], p);
  ASSERT_EQ(JitChainWalk::Status::Found, w.status);
  EXPECT_EQ(0u, w.slot);
  EXPECT_TRUE(w.allFlagged);

  // A dictionary receiver can gain the property without a class change.
  lv.dictRecv = JSObject::create(runtime, lv.o[1]);
  makeDictionary(runtime, lv.dictRecv, t);
  w = jitWalkChain(runtime, *lv.dictRecv, p);
  EXPECT_EQ(JitChainWalk::Status::Rejected, w.status);
  EXPECT_FALSE(w.allFlagged);

  // A dictionary on the path changes in place.
  lv.dictMid = JSObject::create(runtime, lv.o[0]);
  makeDictionary(runtime, lv.dictMid, t);
  lv.viaDict = JSObject::create(runtime, lv.dictMid);
  w = jitWalkChain(runtime, *lv.viaDict, p);
  EXPECT_EQ(JitChainWalk::Status::Rejected, w.status);
  EXPECT_FALSE(w.allFlagged);
}

} // namespace

#endif // HERMESVM_JIT

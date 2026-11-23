/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/PropertyCache.h"

#include "VMRuntimeTestHelpers.h"

#include "hermes/BCGen/HBC/SimpleBytecodeBuilder.h"

#include "gtest/gtest.h"

using namespace hermes::hbc;
using namespace hermes::vm;

namespace {

TEST(PropertyCacheTest, WritePropertyCacheTest) {
  WritePropertyCacheEntry entry{};
  entry.setSlot(0xab);
  EXPECT_EQ(0xab, entry.getSlot());
  EXPECT_EQ(0, entry.getAddCacheIndex());
  entry.setSlot(0xab);
  EXPECT_EQ(0xab, entry.getSlot());
  EXPECT_EQ(0x0, entry.getAddCacheIndex());
  entry.setAddCacheIndex(0x5abacc);
  EXPECT_EQ(0xab, entry.getSlot());
  EXPECT_EQ(0x5abacc, entry.getAddCacheIndex());
  entry.setSlot(0x12);
  EXPECT_EQ(0x12, entry.getSlot());
  EXPECT_EQ(0x5abacc, entry.getAddCacheIndex());
}

using PropertyCacheRuntimeTest = RuntimeTestFixture;

/// A full collection must clear the property cache weak roots in a CodeBlock
/// and in the RuntimeModule add cache whose referents died, and must keep the
/// others pointing at their referents, which may have moved.
TEST_F(PropertyCacheRuntimeTest, WeakRootsTest) {
  auto *runtimeModule = RuntimeModule::createUninitialized(runtime, domain);
  // A function with two read cache entries and two write cache entries.
  SimpleBytecodeBuilder builder;
  BytecodeInstructionGenerator instGen;
  instGen.emitLoadConstDoubleDirect(0, 10.0);
  instGen.emitRet(0);
  builder.addFunction(1, 1, instGen.acquireBytecode(), 2, 2);
  CodeBlock *codeBlock = createSimpleCodeBlock(runtimeModule, runtime, builder);
  uint32_t liveIdx = *runtimeModule->allocateAddCacheEntry();
  uint32_t deadIdx = *runtimeModule->allocateAddCacheEntry();
  uint32_t deadOldIdx = *runtimeModule->allocateAddCacheEntry();

  struct : public Locals {
    PinnedValue<JSObject> deadOldObj;
    PinnedValue<HiddenClass> liveClazz;
    PinnedValue<HiddenClass> deadClazz;
    PinnedValue<JSObject> liveObj;
    PinnedValue<JSObject> deadObj;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  lv.deadOldObj = JSObject::create(runtime);
  // With a generational GC, move deadOldObj out of the young generation, so
  // that only the marking of a full collection can clear a weak root to it.
  runtime.collect("test");
  lv.liveClazz = HiddenClass::createRoot(
      runtime, Runtime::makeNullHandle<JSObject>(), ClassFlags{});
  lv.deadClazz = HiddenClass::createRoot(
      runtime, Runtime::makeNullHandle<JSObject>(), ClassFlags{});
  lv.liveObj = JSObject::create(runtime);
  lv.deadObj = JSObject::create(runtime);

  // Point some weak roots at cells that stay alive and some at cells that die,
  // and leave the others empty.
  ReadPropertyCacheEntry *readCache = codeBlock->readPropertyCache();
  readCache[0].clazz.set(runtime, *lv.liveClazz);
  readCache[0].negMatchClazz.set(runtime, *lv.deadClazz);
  readCache[1].clazz.set(runtime, *lv.deadClazz);
  WritePropertyCacheEntry *writeCache = codeBlock->writePropertyCache();
  writeCache[0].clazz.set(runtime, *lv.deadClazz);
  writeCache[1].clazz.set(runtime, *lv.liveClazz);
  AddPropertyCacheEntry &liveEntry = runtimeModule->getAddCacheEntry(liveIdx);
  liveEntry.startClazz.set(runtime, *lv.liveClazz);
  liveEntry.parent.set(runtime, *lv.liveObj);
  AddPropertyCacheEntry &deadEntry = runtimeModule->getAddCacheEntry(deadIdx);
  deadEntry.startClazz.set(runtime, *lv.deadClazz);
  deadEntry.resultClazz.set(runtime, *lv.deadClazz);
  deadEntry.parent.set(runtime, *lv.deadObj);
  AddPropertyCacheEntry &deadOldEntry =
      runtimeModule->getAddCacheEntry(deadOldIdx);
  deadOldEntry.parent.set(runtime, *lv.deadOldObj);
  lv.deadOldObj = nullptr;
  lv.deadClazz = nullptr;
  lv.deadObj = nullptr;

  runtime.collect("test");

  EXPECT_EQ(*lv.liveClazz, readCache[0].clazz.getNoBarrierUnsafe(runtime));
  EXPECT_FALSE(readCache[0].negMatchClazz);
  EXPECT_FALSE(readCache[1].clazz);
  EXPECT_FALSE(readCache[1].negMatchClazz);
  EXPECT_FALSE(writeCache[0].clazz);
  EXPECT_EQ(*lv.liveClazz, writeCache[1].clazz.getNoBarrierUnsafe(runtime));
  EXPECT_EQ(*lv.liveClazz, liveEntry.startClazz.getNoBarrierUnsafe(runtime));
  EXPECT_FALSE(liveEntry.resultClazz);
  EXPECT_EQ(*lv.liveObj, liveEntry.parent.getNoBarrierUnsafe(runtime));
  EXPECT_FALSE(deadEntry.startClazz);
  EXPECT_FALSE(deadEntry.resultClazz);
  EXPECT_FALSE(deadEntry.parent);
  EXPECT_FALSE(deadOldEntry.parent);
}

} // namespace

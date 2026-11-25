/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/WeakValueObjectMap.h"

#include "hermes/VM/JSObject.h"
#include "hermes/VM/Runtime.h"

#include "VMRuntimeTestHelpers.h"

#include "gtest/gtest.h"

using namespace hermes::vm;

namespace hermes::vm {
/// Test values are objects whose prototype is their key, which matches the
/// contract: the key never changes and the value keeps it alive.
template <>
struct WeakValueObjectMapKey<JSObject> {
  static JSObject *get(JSObject *value, Runtime &runtime) {
    return value->getParent(runtime);
  }
};
} // namespace hermes::vm

namespace {

using WeakValueObjectMapTest = LargeHeapRuntimeTestFixture;
using Map = WeakValueObjectMap<JSObject>;

/// \return a new value for \p key.
JSObject *makeValue(Runtime &runtime, Handle<JSObject> key) {
  return JSObject::create(runtime, key).get();
}

/// Fill \p keys and \p values with \p count new entries starting at \p start,
/// inserting each into \p map.
void insertEntries(
    Runtime &runtime,
    Map &map,
    Handle<ArrayStorage> keys,
    Handle<ArrayStorage> values,
    uint32_t start,
    uint32_t count) {
  struct : public Locals {
    PinnedValue<JSObject> key;
    PinnedValue<JSObject> value;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  for (uint32_t i = start; i < start + count; ++i) {
    lv.key = JSObject::create(runtime);
    lv.value = makeValue(runtime, lv.key);
    keys->set(i, HermesValue::encodeObjectValue(*lv.key), runtime.getHeap());
    values->set(
        i, HermesValue::encodeObjectValue(*lv.value), runtime.getHeap());
    map.insert(runtime, *lv.key, *lv.value);
  }
}

/// Expect that the key at \p i in \p keys maps to the value at \p i in
/// \p values.
void expectFound(
    Runtime &runtime,
    const Map &map,
    ArrayStorage *keys,
    ArrayStorage *values,
    uint32_t i) {
  JSObject *key = vmcast<JSObject>(keys->at(i));
  EXPECT_EQ(vmcast<JSObject>(values->at(i)), map.lookup(runtime, key))
      << "i = " << i;
}

TEST_F(WeakValueObjectMapTest, InsertTest) {
  struct : public Locals {
    PinnedValue<JSObject> key;
    PinnedValue<JSObject> value1;
    PinnedValue<JSObject> value2;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  Map map;

  lv.key = JSObject::create(runtime);
  lv.value1 = makeValue(runtime, lv.key);
  lv.value2 = makeValue(runtime, lv.key);

  map.insert(runtime, *lv.key, *lv.value1);
  EXPECT_EQ(*lv.value1, map.lookup(runtime, *lv.key));

  // Overwrite the live entry.
  map.insert(runtime, *lv.key, *lv.value2);
  EXPECT_EQ(*lv.value2, map.lookup(runtime, *lv.key));

  // value1 is no longer in the map, so freeing it doesn't affect the entry.
  lv.value1 = nullptr;
  runtime.collect("test");
  EXPECT_EQ(*lv.value2, map.lookup(runtime, *lv.key));

  // Free value2 and trigger GC to invalidate its WeakRef.
  lv.value1 = makeValue(runtime, lv.key);
  lv.value2 = nullptr;
  runtime.collect("test");
  EXPECT_FALSE(map.containsKey(runtime, *lv.key));

  // The dead entry is reused, so the table doesn't change size.
  const auto capacity = map.capacity();
  map.insert(runtime, *lv.key, *lv.value1);
  EXPECT_EQ(*lv.value1, map.lookup(runtime, *lv.key));
  EXPECT_EQ(capacity, map.capacity());
}

TEST_F(WeakValueObjectMapTest, NullKeyTest) {
  struct : public Locals {
    PinnedValue<JSObject> value;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  Map map;

  lv.value = makeValue(runtime, Runtime::makeNullHandle<JSObject>());
  ASSERT_EQ(nullptr, lv.value->getParent(runtime));

  map.insert(runtime, nullptr, *lv.value);
  EXPECT_EQ(*lv.value, map.lookup(runtime, nullptr));
  EXPECT_TRUE(map.containsKey(runtime, nullptr));
}

TEST_F(WeakValueObjectMapTest, RehashWithDeadEntryTest) {
  struct : public Locals {
    PinnedValue<ArrayStorage> keys;
    PinnedValue<ArrayStorage> values;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  Map map;

  lv.keys.castAndSetHermesValue<ArrayStorage>(
      *ArrayStorage::create(runtime, 32, 32));
  lv.values.castAndSetHermesValue<ArrayStorage>(
      *ArrayStorage::create(runtime, 32, 32));

  insertEntries(runtime, map, lv.keys, lv.values, 0, 2);
  expectFound(runtime, map, *lv.keys, *lv.values, 0);
  expectFound(runtime, map, *lv.keys, *lv.values, 1);

  lv.values->set(1, HermesValue::encodeUndefinedValue(), runtime.getHeap());
  runtime.collect("test");
  EXPECT_FALSE(map.containsKey(runtime, vmcast<JSObject>(lv.keys->at(1))));

  // Force rehashes with a dead entry in the table. Live entries must survive
  // them.
  insertEntries(runtime, map, lv.keys, lv.values, 2, 20);

  EXPECT_FALSE(map.containsKey(runtime, vmcast<JSObject>(lv.keys->at(1))));
  for (uint32_t i = 0; i < 22; ++i) {
    if (i != 1)
      expectFound(runtime, map, *lv.keys, *lv.values, i);
  }
}

TEST_F(WeakValueObjectMapTest, DeadEntriesDoNotGrowTableTest) {
  struct : public Locals {
    PinnedValue<ArrayStorage> keys;
    PinnedValue<ArrayStorage> values;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  Map map;

  lv.keys.castAndSetHermesValue<ArrayStorage>(
      *ArrayStorage::create(runtime, 64, 64));
  lv.values.castAndSetHermesValue<ArrayStorage>(
      *ArrayStorage::create(runtime, 64, 64));

  // Insert an entry, then let its value die, over and over. The table never
  // holds more than one live entry, so it must not keep growing.
  const uint32_t numRounds = 50;
  for (uint32_t i = 0; i < numRounds; ++i) {
    insertEntries(runtime, map, lv.keys, lv.values, i, 1);
    expectFound(runtime, map, *lv.keys, *lv.values, i);
    lv.values->set(i, HermesValue::encodeUndefinedValue(), runtime.getHeap());
    runtime.collect("test");
    EXPECT_FALSE(map.containsKey(runtime, vmcast<JSObject>(lv.keys->at(i))))
        << "i = " << i;
  }

  EXPECT_LE(map.capacity(), 16u);
}

TEST_F(WeakValueObjectMapTest, GrowTest) {
  struct : public Locals {
    PinnedValue<ArrayStorage> keys;
    PinnedValue<ArrayStorage> values;
    PinnedValue<JSObject> key;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  Map map;

  lv.keys.castAndSetHermesValue<ArrayStorage>(
      *ArrayStorage::create(runtime, 32, 32));
  lv.values.castAndSetHermesValue<ArrayStorage>(
      *ArrayStorage::create(runtime, 32, 32));

  EXPECT_TRUE(map.isKnownEmpty());
  EXPECT_EQ(map.capacity(), 0u);

  // The 7th insert overflows the initial 8 entries; growth doubles.
  insertEntries(runtime, map, lv.keys, lv.values, 0, 6);
  EXPECT_EQ(map.capacity(), 8u);
  insertEntries(runtime, map, lv.keys, lv.values, 6, 1);
  EXPECT_EQ(map.capacity(), 16u);

  const uint32_t numItems = 20;
  insertEntries(runtime, map, lv.keys, lv.values, 7, numItems - 7);

  EXPECT_EQ(map.capacity(), 32u);
  EXPECT_FALSE(map.isKnownEmpty());
  for (uint32_t i = 0; i < numItems; ++i)
    expectFound(runtime, map, *lv.keys, *lv.values, i);

  // Keys that were never inserted are not found.
  lv.key = JSObject::create(runtime);
  EXPECT_FALSE(map.containsKey(runtime, *lv.key));
}

TEST_F(WeakValueObjectMapTest, ReuseAfterMassDeathTest) {
  struct : public Locals {
    PinnedValue<ArrayStorage> keys;
    PinnedValue<ArrayStorage> values;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  Map map;

  lv.keys.castAndSetHermesValue<ArrayStorage>(
      *ArrayStorage::create(runtime, 64, 64));
  lv.values.castAndSetHermesValue<ArrayStorage>(
      *ArrayStorage::create(runtime, 64, 64));

  const uint32_t numItems = 40;
  insertEntries(runtime, map, lv.keys, lv.values, 0, numItems);
  const auto grownCapacity = map.capacity();
  EXPECT_GE(grownCapacity, 64u);

  // Kill all but a few values.
  const uint32_t itemsToKeep = 3;
  for (uint32_t i = itemsToKeep; i < numItems; ++i)
    lv.values->set(i, HermesValue::encodeUndefinedValue(), runtime.getHeap());
  runtime.collect("test");

  // Short-lived entries (kept in a scratch slot) reuse the dead entries, so
  // the table doesn't grow.
  const uint32_t scratch = 63;
  for (uint32_t i = 0; i < 200; ++i) {
    insertEntries(runtime, map, lv.keys, lv.values, scratch, 1);
    lv.values->set(
        scratch, HermesValue::encodeUndefinedValue(), runtime.getHeap());
    if (i % 8 == 0)
      runtime.collect("test");
  }
  EXPECT_LE(map.capacity(), grownCapacity);

  for (uint32_t i = 0; i < itemsToKeep; ++i)
    expectFound(runtime, map, *lv.keys, *lv.values, i);
  for (uint32_t i = itemsToKeep; i < numItems; ++i) {
    EXPECT_FALSE(map.containsKey(runtime, vmcast<JSObject>(lv.keys->at(i))))
        << "i = " << i;
  }
}

TEST_F(WeakValueObjectMapTest, WeaknessWithGCTest) {
  struct : public Locals {
    PinnedValue<ArrayStorage> keys;
    PinnedValue<ArrayStorage> values;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  Map map;

  lv.keys.castAndSetHermesValue<ArrayStorage>(
      *ArrayStorage::create(runtime, 4, 4));
  lv.values.castAndSetHermesValue<ArrayStorage>(
      *ArrayStorage::create(runtime, 4, 4));

  insertEntries(runtime, map, lv.keys, lv.values, 0, 4);

  auto contains = [&map, &lv, this](uint32_t i) {
    return map.containsKey(runtime, vmcast<JSObject>(lv.keys->at(i)));
  };

  lv.values->set(3, HermesValue::encodeUndefinedValue(), runtime.getHeap());
  runtime.collect("test");
  EXPECT_TRUE(contains(0));
  EXPECT_TRUE(contains(1));
  EXPECT_TRUE(contains(2));
  EXPECT_FALSE(contains(3));

  lv.values->set(0, HermesValue::encodeUndefinedValue(), runtime.getHeap());
  lv.values->set(1, HermesValue::encodeUndefinedValue(), runtime.getHeap());
  runtime.collect("test");
  EXPECT_FALSE(contains(0));
  EXPECT_FALSE(contains(1));
  EXPECT_TRUE(contains(2));
  EXPECT_FALSE(contains(3));
}

TEST_F(WeakValueObjectMapTest, KeyReachingValueIsCollectedTest) {
  struct : public Locals {
    PinnedValue<JSObject> key;
    PinnedValue<JSObject> value;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  Map map;

  lv.key = JSObject::create(runtime);
  lv.value = makeValue(runtime, lv.key);
  map.insert(runtime, *lv.key, *lv.value);

  // The key reaches its own value, as in proto.inst = Object.create(proto).
  ASSERT_NE(
      JSObject::putNamed_RJS(
          lv.key,
          runtime,
          Predefined::getSymbolID(Predefined::length),
          lv.value),
      ExecutionStatus::EXCEPTION);

  WeakRef<JSObject> weakKey{runtime, lv.key};
  lv.key = nullptr;
  // Creating an object with a given parent may leave that parent reachable
  // from a runtime scratch root. Create one with the default parent to reset
  // it, so only the map could keep the key alive.
  lv.value = makeValue(runtime, runtime.objectPrototype);
  lv.value = nullptr;
  runtime.collect("test");

  // The map must not keep the key (and through it, the value) alive.
  EXPECT_FALSE(weakKey.isValid());
  weakKey.releaseSlot();
}

TEST_F(WeakValueObjectMapTest, EmptyMapOperationsTest) {
  struct : public Locals {
    PinnedValue<JSObject> key;
    PinnedValue<JSObject> value;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  Map map;

  EXPECT_TRUE(map.isKnownEmpty());
  EXPECT_EQ(map.capacity(), 0u);

  lv.key = JSObject::create(runtime);
  EXPECT_FALSE(map.containsKey(runtime, *lv.key));
  EXPECT_EQ(map.lookup(runtime, *lv.key), nullptr);

  lv.value = makeValue(runtime, lv.key);
  map.insert(runtime, *lv.key, *lv.value);
  EXPECT_FALSE(map.isKnownEmpty());
}

} // namespace

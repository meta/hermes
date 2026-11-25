/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/DictPropertyMap.h"

#include "VMRuntimeTestHelpers.h"

#include "gtest/gtest.h"

using namespace hermes::vm;

namespace {

using DictPropertyMapTest = LargeHeapRuntimeTestFixture;

TEST_F(DictPropertyMapTest, SmokeTest) {
  struct : public Locals {
    PinnedValue<DictPropertyMap> mapSlot;
    PinnedValue<DictPropertyMap> saveMapSlot;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  auto id1 = SymbolID::unsafeCreate(1);
  auto id2 = SymbolID::unsafeCreate(2);
  auto id3 = SymbolID::unsafeCreate(3);
  auto id4 = SymbolID::unsafeCreate(4);

  NamedPropertyDescriptor desc1{};

  auto res = DictPropertyMap::create(runtime, 2);
  ASSERT_FALSE(isException(res));
  lv.mapSlot = res->get();
  MutableHandle<DictPropertyMap> map{lv.mapSlot};
  // We need to use another MutableHandle here. If it's a raw pointer, when
  // handlesan is ON, even if the map is reallocated, since collection happens
  // before the creation of new map, it will be reallocated at the same address.
  lv.saveMapSlot = res->get();
  MutableHandle<DictPropertyMap> saveMap{lv.saveMapSlot};

  // Try to find a property in the empty map.
  ASSERT_FALSE(DictPropertyMap::find(map.get(), id1));

  // Add prop1.
  DictPropertyMap::add(map, runtime, id1, desc1);
  ASSERT_EQ(1u, map->size());

  auto found = DictPropertyMap::find(*map, id1);
  ASSERT_TRUE(found);
  ASSERT_EQ(id1, DictPropertyMap::getDescriptorPair(*map, *found)->first);

  // Add prop2.
  DictPropertyMap::add(map, runtime, id2, desc1);
  ASSERT_EQ(2u, map->size());

  // Find prop1, prop2.
  found = DictPropertyMap::find(*map, id1);
  ASSERT_TRUE(found);
  ASSERT_EQ(id1, DictPropertyMap::getDescriptorPair(*map, *found)->first);
  found = DictPropertyMap::find(*map, id2);
  ASSERT_TRUE(found);
  ASSERT_EQ(id2, DictPropertyMap::getDescriptorPair(*map, *found)->first);

  // Make sure we haven't reallocated (which will update map).
  ASSERT_EQ(saveMap.get(), map.get());

  // Add a prop3, causing a reallocation.
  DictPropertyMap::add(map, runtime, id3, desc1);
  // Make sure we reallocated.
  ASSERT_NE(saveMap.get(), map.get());
  saveMap = map.get();
  for (unsigned i = 0; i < 3; ++i) {
    auto sym = SymbolID::unsafeCreate(id1.unsafeGetIndex() + i);
    found = DictPropertyMap::find(*map, sym);
    ASSERT_TRUE(found);
    ASSERT_EQ(sym, DictPropertyMap::getDescriptorPair(*map, *found)->first);
  }

  // Add a prop4.
  DictPropertyMap::add(map, runtime, id4, desc1);
  ASSERT_EQ(saveMap.get(), map.get());
  for (unsigned i = 0; i < 4; ++i) {
    auto sym = SymbolID::unsafeCreate(id1.unsafeGetIndex() + i);
    found = DictPropertyMap::find(*map, sym);
    ASSERT_TRUE(found);
    ASSERT_EQ(sym, DictPropertyMap::getDescriptorPair(*map, *found)->first);
  }

  // Verify the enumeration order.
  {
    unsigned symIndex = 1;
    DictPropertyMap::forEachProperty(
        map, runtime, [&](SymbolID id, NamedPropertyDescriptor desc) {
          ASSERT_EQ(symIndex, id.unsafeGetIndex());
          ++symIndex;
        });
  }

  // Delete prop2.
  found = DictPropertyMap::find(*map, id2);
  DictPropertyMap::erase(*map, runtime, *found);
  ASSERT_EQ(saveMap.get(), map.get());
  ASSERT_EQ(3u, map->size());

  {
    static const unsigned symbols[] = {1, 3, 4};
    for (unsigned i = 0; i < 3; ++i) {
      auto sym = SymbolID::unsafeCreate(symbols[i]);
      found = DictPropertyMap::find(*map, sym);
      ASSERT_TRUE(found);
      ASSERT_EQ(sym, DictPropertyMap::getDescriptorPair(*map, *found)->first);
    }
    unsigned i = 0;
    DictPropertyMap::forEachProperty(
        map, runtime, [&](SymbolID id, NamedPropertyDescriptor desc) {
          ASSERT_EQ(symbols[i], id.unsafeGetIndex());
          ++i;
        });
  }

  // Add prop2 again.
  DictPropertyMap::add(map, runtime, id2, desc1);
  ASSERT_EQ(4u, map->size());

  {
    static const unsigned symbols[] = {1, 3, 4, 2};
    for (unsigned i = 0; i < 4; ++i) {
      auto sym = SymbolID::unsafeCreate(symbols[i]);
      found = DictPropertyMap::find(*map, sym);
      ASSERT_TRUE(found);
      ASSERT_EQ(sym, DictPropertyMap::getDescriptorPair(*map, *found)->first);
    }
    unsigned i = 0;
    DictPropertyMap::forEachProperty(
        map, runtime, [&](SymbolID id, NamedPropertyDescriptor desc) {
          ASSERT_EQ(symbols[i], id.unsafeGetIndex());
          ++i;
        });
  }
}

TEST_F(DictPropertyMapTest, CreateOverCapacityTest) {
  (void)DictPropertyMap::create(runtime);
  ASSERT_EQ(
      ExecutionStatus::EXCEPTION,
      DictPropertyMap::create(runtime, DictPropertyMap::getMaxCapacity() + 1));
}

} // namespace

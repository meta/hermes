/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/ArrayStorage.h"

#include "hermes/VM/FastArray.h"

#include "VMRuntimeTestHelpers.h"

using namespace hermes::vm;

namespace {

using ArrayStorageTest = RuntimeTestFixture;

/// This is a test that exposed a bug in the removed shift() function. It is
/// good to still keep it as it can exercise different code paths in the new
/// resize()/resizeLeft() function.
TEST_F(ArrayStorageTest, ResizeTest) {
  using StorageType = ArrayStorageSmall;
  using HVType = SmallHermesValue;

  struct : Locals {
    PinnedValue<StorageType> arr;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  MutableHandle<StorageType> arrHandle{lv.arr};

  // Create a new ArrayStorage.
  lv.arr.castAndSetHermesValue<StorageType>(*StorageType::create(runtime, 1));
  // Size is 900, capacity is 900.
  StorageType::resize(arrHandle, runtime, 900);
  // arr[10] = 1
  auto num = HVType::encodeNumberValue(1, runtime);
  lv.arr->set(10, num, runtime.getHeap());
  // Insert 90 elements at the left, new size will be 990, capacity will be
  // 1800.
  // Copy elements from [0, 900) in old array to [90, 990) in new array.
  StorageType::resizeLeft(arrHandle, runtime, 990);
  ASSERT_EQ(lv.arr->at(100).getNumber(runtime), 1);
  // Assert some elements at boundary and in the middle are correct Empty.
  ASSERT_TRUE(lv.arr->at(101).isEmpty());
  ASSERT_TRUE(lv.arr->at(950).isEmpty());
  ASSERT_TRUE(lv.arr->at(989).isEmpty());
  // Size becomes 1234, while capacity is still 1800. Elements between 990 and
  // 1234 are initialized to Empty, elements after 1234 are uninitialized.
  StorageType::resize(arrHandle, runtime, 1234);
  ASSERT_EQ(lv.arr->at(100).getNumber(runtime), 1);
  // Assert some elements at boundary and in the middle are correct Empty.
  ASSERT_TRUE(lv.arr->at(101).isEmpty());
  ASSERT_TRUE(lv.arr->at(1000).isEmpty());
  ASSERT_TRUE(lv.arr->at(1233).isEmpty());
  // The capacity is large enough, so it will only move elements towards right
  // with offset of 5.
  StorageType::resizeLeft(arrHandle, runtime, 1239);
  ASSERT_EQ(lv.arr->at(105).getNumber(runtime), 1);
  // Assert some elements at boundary and in the middle are correct Empty.
  ASSERT_TRUE(lv.arr->at(0).isEmpty());
  ASSERT_TRUE(lv.arr->at(106).isEmpty());
  ASSERT_TRUE(lv.arr->at(1230).isEmpty());
  ASSERT_TRUE(lv.arr->at(1238).isEmpty());
}

TEST_F(ArrayStorageTest, RandomResizeTest) {
  using StorageType = ArrayStorageSmall;

  auto randomInt = [] {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> uniformDist(0, 1000);
    return (StorageType::size_type)uniformDist(gen);
  };

  struct : Locals {
    PinnedValue<StorageType> arr;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  MutableHandle<StorageType> arrHandle{lv.arr};

  for (int i = 0; i < 1000; ++i) {
    auto size = randomInt();
    // We need to make sure capacity is always >= size.
    lv.arr.castAndSetHermesValue<StorageType>(
        *StorageType::create(runtime, size + randomInt(), size));

    StorageType::resize(arrHandle, runtime, randomInt());
    StorageType::resizeLeft(arrHandle, runtime, randomInt());
    // The object creation below will move array when handlesan is enabled. Note
    // that in MallocGC, when moving live objects, it does trimming as well,
    // which affects the capacity of the array, and may expose potential bugs.
    // Same applies to collect() call.
    (void)JSObject::create(runtime);
    StorageType::resizeLeft(arrHandle, runtime, randomInt());
    StorageType::resize(arrHandle, runtime, randomInt());
    runtime.collect("test");
    StorageType::resize(arrHandle, runtime, randomInt());
    StorageType::resizeLeft(arrHandle, runtime, randomInt());
  }
}

TEST_F(ArrayStorageTest, PushBackTest) {
  struct : public Locals {
    PinnedValue<ArrayStorage> stSlot;
    PinnedValue<> val;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  MutableHandle<ArrayStorage> st{lv.stSlot};
  st = vmcast<ArrayStorage>(*ArrayStorage::create(runtime, 4));

  lv.val = 1.0_hd;
  (void)ArrayStorage::push_back(st, runtime, lv.val);
  lv.val = 2.0_hd;
  (void)ArrayStorage::push_back(st, runtime, lv.val);
  lv.val = 3.0_hd;
  (void)ArrayStorage::push_back(st, runtime, lv.val);
  lv.val = 4.0_hd;
  (void)ArrayStorage::push_back(st, runtime, lv.val);
  lv.val = 5.0_hd;
  (void)ArrayStorage::push_back(st, runtime, lv.val);

  ASSERT_EQ(5u, st->size());
  ASSERT_EQ(8u, st->capacity());
  ASSERT_EQ(1.0_hd, st->at(0));
  ASSERT_EQ(2.0_hd, st->at(1));
  ASSERT_EQ(3.0_hd, st->at(2));
  ASSERT_EQ(4.0_hd, st->at(3));
  ASSERT_EQ(5.0_hd, st->at(4));
}

TEST_F(ArrayStorageTest, AppendExactCapacityTest) {
  struct : Locals {
    PinnedValue<ArrayStorage> dst;
    PinnedValue<ArrayStorage> src;
    PinnedValue<FastArray> fastDst;
    PinnedValue<FastArray> fastSrc;
    PinnedValue<ArrayStorageSmall> fastDstStorage;
    PinnedValue<ArrayStorageSmall> fastSrcStorage;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // ArrayStorage: dst capacity 4 + size 2, src size 2. After append, the
  // resulting size exactly equals capacity, exercising the boundary of the
  // fast path.
  lv.dst.castAndSetHermesValue<ArrayStorage>(*ArrayStorage::create(runtime, 4));
  lv.src.castAndSetHermesValue<ArrayStorage>(*ArrayStorage::create(runtime, 2));
  lv.dst->pushWithinCapacity(runtime, HermesValue::encodeTrustedNumberValue(1));
  lv.dst->pushWithinCapacity(runtime, HermesValue::encodeTrustedNumberValue(2));
  lv.src->pushWithinCapacity(runtime, HermesValue::encodeTrustedNumberValue(3));
  lv.src->pushWithinCapacity(runtime, HermesValue::encodeTrustedNumberValue(4));

  ASSERT_EQ(4u, lv.dst->capacity());
  ASSERT_EQ(2u, lv.dst->size());
  MutableHandle<ArrayStorage> dst{lv.dst};
  ASSERT_RETURNED(ArrayStorage::append(dst, runtime, lv.src));
  EXPECT_EQ(4u, lv.dst->size());
  EXPECT_EQ(4u, lv.dst->capacity());
  EXPECT_EQ(1, lv.dst->at(0).getNumber());
  EXPECT_EQ(2, lv.dst->at(1).getNumber());
  EXPECT_EQ(3, lv.dst->at(2).getNumber());
  EXPECT_EQ(4, lv.dst->at(3).getNumber());

  // Same boundary case for FastArray::append.
  lv.fastDst.castAndSetHermesValue<FastArray>(*FastArray::create(runtime, 4));
  lv.fastSrc.castAndSetHermesValue<FastArray>(*FastArray::create(runtime, 2));
  lv.fastDstStorage = lv.fastDst->unsafeGetIndexedStorage(runtime);
  lv.fastSrcStorage = lv.fastSrc->unsafeGetIndexedStorage(runtime);
  auto v1 = SmallHermesValue::encodeNumberValue(1, runtime);
  lv.fastDstStorage->pushWithinCapacity(runtime, v1);
  auto v2 = SmallHermesValue::encodeNumberValue(2, runtime);
  lv.fastDstStorage->pushWithinCapacity(runtime, v2);
  auto v3 = SmallHermesValue::encodeNumberValue(3, runtime);
  lv.fastSrcStorage->pushWithinCapacity(runtime, v3);
  auto v4 = SmallHermesValue::encodeNumberValue(4, runtime);
  lv.fastSrcStorage->pushWithinCapacity(runtime, v4);

  ASSERT_EQ(4u, lv.fastDstStorage->capacity());
  ASSERT_RETURNED(
      FastArray::append(
          Handle<FastArray>{lv.fastDst},
          runtime,
          Handle<FastArray>{lv.fastSrc}));
  EXPECT_EQ(4u, lv.fastDst->getLengthAsUint32(runtime));
  lv.fastDstStorage = lv.fastDst->unsafeGetIndexedStorage(runtime);
  EXPECT_EQ(4u, lv.fastDstStorage->size());
  EXPECT_EQ(4u, lv.fastDstStorage->capacity());
  EXPECT_EQ(1, lv.fastDstStorage->at(0).getNumber(runtime));
  EXPECT_EQ(2, lv.fastDstStorage->at(1).getNumber(runtime));
  EXPECT_EQ(3, lv.fastDstStorage->at(2).getNumber(runtime));
  EXPECT_EQ(4, lv.fastDstStorage->at(3).getNumber(runtime));
}

/// Append a sub-range of one storage to another with appendWithinCapacity. The
/// destination is long-lived, so the copied pointer to a young object needs a
/// write barrier to be updated when the GC moves the object.
TEST_F(ArrayStorageTest, AppendRangeWithinCapacityTest) {
  using StorageType = ArrayStorageSmall;
  using HVType = SmallHermesValue;

  struct : Locals {
    PinnedValue<StorageType> src;
    PinnedValue<StorageType> dst;
    PinnedValue<JSObject> obj;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // src = [0, empty, obj, 3, 4].
  lv.src.castAndSetHermesValue<StorageType>(*StorageType::create(runtime, 5));
  lv.obj = JSObject::create(runtime);
  auto v0 = HVType::encodeNumberValue(0, runtime);
  lv.src->pushWithinCapacity(runtime, v0);
  lv.src->pushWithinCapacity(runtime, HVType::encodeEmptyValue());
  auto vObj = HVType::encodeObjectValue(lv.obj.get(), runtime);
  lv.src->pushWithinCapacity(runtime, vObj);
  auto v3 = HVType::encodeNumberValue(3, runtime);
  lv.src->pushWithinCapacity(runtime, v3);
  auto v4 = HVType::encodeNumberValue(4, runtime);
  lv.src->pushWithinCapacity(runtime, v4);

  // dst = [9], with capacity for 4 elements.
  lv.dst.castAndSetHermesValue<StorageType>(
      *StorageType::createLongLived(runtime, 4));
  auto v9 = HVType::encodeNumberValue(9, runtime);
  lv.dst->pushWithinCapacity(runtime, v9);

  // An empty range appends nothing.
  lv.dst->appendWithinCapacity(runtime, lv.src.get(), 2, 2);
  EXPECT_EQ(1u, lv.dst->size());

  // Append [1, 4).
  lv.dst->appendWithinCapacity(runtime, lv.src.get(), 1, 4);
  ASSERT_EQ(4u, lv.dst->size());

  // Move the object.
  runtime.collect("test");

  EXPECT_EQ(9, lv.dst->at(0).getNumber(runtime));
  EXPECT_TRUE(lv.dst->at(1).isEmpty());
  EXPECT_EQ(lv.obj.get(), lv.dst->at(2).getObject(runtime));
  EXPECT_EQ(3, lv.dst->at(3).getNumber(runtime));

  // The source is unchanged.
  ASSERT_EQ(5u, lv.src->size());
  EXPECT_EQ(0, lv.src->at(0).getNumber(runtime));
  EXPECT_TRUE(lv.src->at(1).isEmpty());
  EXPECT_EQ(lv.obj.get(), lv.src->at(2).getObject(runtime));
  EXPECT_EQ(3, lv.src->at(3).getNumber(runtime));
  EXPECT_EQ(4, lv.src->at(4).getNumber(runtime));
}

// ArrayStorage trimming is disabled for now. In the future, we may consider
// re-enabling it only for specific ArrayStorages (e.g. property storage).
TEST_F(ArrayStorageTest, DISABLED_AllowTrimming) {
  struct : public Locals {
    PinnedValue<ArrayStorage> stSlot;
    PinnedValue<> val;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  MutableHandle<ArrayStorage> st{lv.stSlot};
  constexpr ArrayStorage::size_type originalCapacity = 8;
  // Create an array and put in an element so its size is 1 and its capacity
  // is 8.
  st = vmcast<ArrayStorage>(*ArrayStorage::create(runtime, originalCapacity));
  EXPECT_LE(st->capacity(), originalCapacity);
  lv.val = 0.0_hd;
  ASSERT_RETURNED(ArrayStorage::push_back(st, runtime, lv.val));
  EXPECT_EQ(1, st->size());

  // Now force some GCs to happen.
  for (auto i = 0; i < 2; i++) {
    runtime.collect("test");
  }

  // The array should be trimmed.
  EXPECT_EQ(st->size(), st->capacity());
}

using ArrayStorageBigHeapTest = ExtremeLargeHeapRuntimeTestFixture;

TEST_F(ArrayStorageBigHeapTest, AllocLarge) {
  // 1M elements would need > 4MB in HV32, > 8MB in HV64. Both cases trigger
  // large allocation.
  using StorageType = ArrayStorageSmall;
  using HVType = SmallHermesValue;

  constexpr size_t sz = 1024 * 1024;
  struct : Locals {
    PinnedValue<StorageType> st;
    PinnedValue<JSObject> obj;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  lv.st.castAndSetHermesValue<StorageType>(
      *StorageType::create(runtime, 1024 * 1024));
  MutableHandle<StorageType> st{lv.st};
  StorageType::resize(st, runtime, sz);
  lv.obj = JSObject::create(runtime);
  lv.st->set(
      sz - 1,
      HVType::encodeHermesValue(lv.obj.getHermesValue(), runtime),
      runtime.getHeap());
  // Force the created object to move.
  runtime.collect("test");
  auto val = lv.st->at(sz - 1);
  // It should be accessible and returning the same object pointer.
  EXPECT_EQ(lv.obj.get(), val.getPointer(runtime));
}

TEST_F(ArrayStorageBigHeapTest, AllocLargeArrayThrowsRangeError) {
  // Attempt to allocate an array so big that it fails and throws a range error.
  auto res = ArrayStorage::create(runtime, 1024 * 1024 * 1024);
  EXPECT_EQ(res, ExecutionStatus::EXCEPTION)
      << "Allocating an array slightly larger than its max size should throw";
  HermesValue hv = runtime.getThrownValue();
  EXPECT_EQ(
      vmcast<JSObject>(hv)->getParent(runtime), *runtime.RangeErrorPrototype)
      << "Exception thrown was not a RangeError";
}

} // namespace

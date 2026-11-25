/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/ArrayStorage.h"

#include "VMRuntimeTestHelpers.h"

using namespace hermes::vm;

namespace {

using ArrayTest = RuntimeTestFixture;

/// Test only the C++ API. We use the high-level API only when it is necessary
/// in order to access some part of the C++ API.
TEST_F(ArrayTest, CppAPITest) {
  struct : Locals {
    PinnedValue<JSArray> array;
    PinnedValue<> val;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  auto arrayRes = JSArray::create(runtime, 1, 0);
  ASSERT_FALSE(isException(arrayRes));
  lv.array = std::move(*arrayRes);

  // Make sure the beginning is at 0.
  ASSERT_EQ(0u, lv.array->getBeginIndex());
  // Make sure the length is 0.
  ASSERT_EQ(0u, lv.array->getEndIndex());

  // Call haveOwnIndexed() and getOwnIndexed() on a element not in the array.
  ComputedPropertyDescriptor desc;
  lv.val = 100.0_hd;
  ASSERT_FALSE(*lv.array->getOwnComputedPrimitiveDescriptor(
      lv.array, runtime, lv.val, JSObject::IgnoreProxy::No, desc));
  lv.val = 100.0_hd;
  EXPECT_CALLRESULT_UNDEFINED(
      lv.array->getComputed_RJS(lv.array, runtime, lv.val));

// Obtain the value a couple of different ways and check its value.
#define EXPECT_INDEX_VALUE(value, array, index)                           \
  {                                                                       \
    struct : Locals {                                                     \
      PinnedValue<> idx;                                                  \
    } mlv;                                                                \
    LocalsRAII mlraii{runtime, &mlv};                                     \
    mlv.idx = HermesValue::encodeTrustedNumberValue(index);               \
    EXPECT_EQ(value, array->at(runtime, index).unboxToHV(runtime));       \
    ASSERT_TRUE(                                                          \
        *array->getOwnComputedDescriptor(array, runtime, mlv.idx, desc)); \
    EXPECT_CALLRESULT_VALUE(                                              \
        value,                                                            \
        JSArray::getComputedPropertyValue_RJS(                            \
            array, runtime, array, desc, mlv.idx));                       \
    EXPECT_CALLRESULT_VALUE(                                              \
        value, array->getComputed_RJS(array, runtime, mlv.idx));          \
  }

  // array[100] = 50. This will case a reallocation.
  lv.val = 50.0_hd;
  ASSERT_NE(
      JSArray::setElementAt(lv.array, runtime, 100, lv.val),
      ExecutionStatus::EXCEPTION);
  // Length must be 101.
  ASSERT_EQ(101u, lv.array->getEndIndex());
  EXPECT_INDEX_VALUE(50.0_hd, lv.array, 100);

  // array[90] = 40. This will cause a reallocation.
  lv.val = 40.0_hd;
  ASSERT_NE(
      JSArray::setElementAt(lv.array, runtime, 90, lv.val),
      ExecutionStatus::EXCEPTION);
  // Length must still be 101.
  ASSERT_EQ(101u, lv.array->getEndIndex());
  EXPECT_INDEX_VALUE(40.0_hd, lv.array, 90);
  EXPECT_INDEX_VALUE(50.0_hd, lv.array, 100);

  // array[105] = 60. This will case a reallocation.
  lv.val = 60.0_hd;
  ASSERT_NE(
      JSArray::setElementAt(lv.array, runtime, 105, lv.val),
      ExecutionStatus::EXCEPTION);
  ASSERT_EQ(106u, lv.array->getEndIndex());
  EXPECT_INDEX_VALUE(40.0_hd, lv.array, 90);
  EXPECT_INDEX_VALUE(50.0_hd, lv.array, 100);
  EXPECT_INDEX_VALUE(60.0_hd, lv.array, 105);

  // array[106] = 70. This will not case a reallocation.
  lv.val = 70.0_hd;
  ASSERT_NE(
      JSArray::setElementAt(lv.array, runtime, 106, lv.val),
      ExecutionStatus::EXCEPTION);
  ASSERT_EQ(107u, lv.array->getEndIndex());
  EXPECT_INDEX_VALUE(40.0_hd, lv.array, 90);
  EXPECT_INDEX_VALUE(50.0_hd, lv.array, 100);
  EXPECT_INDEX_VALUE(60.0_hd, lv.array, 105);
  EXPECT_INDEX_VALUE(70.0_hd, lv.array, 106);

  // array[100] = 51. We are updating an element in-place.
  lv.val = 51.0_hd;
  ASSERT_NE(
      JSArray::setElementAt(lv.array, runtime, 100, lv.val),
      ExecutionStatus::EXCEPTION);
  ASSERT_EQ(107u, lv.array->getEndIndex());
  EXPECT_INDEX_VALUE(40.0_hd, lv.array, 90);
  EXPECT_INDEX_VALUE(51.0_hd, lv.array, 100);
  EXPECT_INDEX_VALUE(60.0_hd, lv.array, 105);
  EXPECT_INDEX_VALUE(70.0_hd, lv.array, 106);

  // Shrink the array to 106 elements.
  JSArray::setStorageEndIndex(lv.array, runtime, 106);
  ASSERT_EQ(106u, lv.array->getEndIndex());
  EXPECT_INDEX_VALUE(40.0_hd, lv.array, 90);
  EXPECT_INDEX_VALUE(51.0_hd, lv.array, 100);
  EXPECT_INDEX_VALUE(60.0_hd, lv.array, 105);
  lv.val = 106.0_hd;
  ASSERT_FALSE(*lv.array->getOwnComputedPrimitiveDescriptor(
      lv.array, runtime, lv.val, JSObject::IgnoreProxy::No, desc));
  lv.val = 106.0_hd;
  EXPECT_CALLRESULT_UNDEFINED(
      lv.array->getComputed_RJS(lv.array, runtime, lv.val));

  // Increase to 107 again.
  JSArray::setStorageEndIndex(lv.array, runtime, 107);
  ASSERT_EQ(107u, lv.array->getEndIndex());
  EXPECT_INDEX_VALUE(40.0_hd, lv.array, 90);
  EXPECT_INDEX_VALUE(51.0_hd, lv.array, 100);
  EXPECT_INDEX_VALUE(60.0_hd, lv.array, 105);
  lv.val = 106.0_hd;
  ASSERT_FALSE(*lv.array->getOwnComputedPrimitiveDescriptor(
      lv.array, runtime, lv.val, JSObject::IgnoreProxy::No, desc));
  lv.val = 106.0_hd;
  EXPECT_CALLRESULT_UNDEFINED(
      lv.array->getComputed_RJS(lv.array, runtime, lv.val));

  // array[106] = 70 again.
  lv.val = 70.0_hd;
  ASSERT_NE(
      JSArray::setElementAt(lv.array, runtime, 106, lv.val),
      ExecutionStatus::EXCEPTION);
  ASSERT_EQ(107u, lv.array->getEndIndex());
  EXPECT_INDEX_VALUE(40.0_hd, lv.array, 90);
  EXPECT_INDEX_VALUE(51.0_hd, lv.array, 100);
  EXPECT_INDEX_VALUE(60.0_hd, lv.array, 105);
  EXPECT_INDEX_VALUE(70.0_hd, lv.array, 106);

#undef EXPECT_INDEX_VALUE
}

TEST_F(ArrayTest, TestLength) {
  struct : Locals {
    PinnedValue<JSArray> array;
    PinnedValue<> val;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  auto lengthID = Predefined::getSymbolID(Predefined::length);
  auto arrayRes = JSArray::create(runtime, 10, 10);
  ASSERT_FALSE(isException(arrayRes));
  lv.array = std::move(*arrayRes);

  // Make sure the length is 10.
  ASSERT_EQ(10u, JSArray::getLength(lv.array.get(), runtime));
  EXPECT_CALLRESULT_DOUBLE(
      10.0, JSObject::getNamed_RJS(lv.array, runtime, lengthID));

  // Change it to 5.0.
  lv.val = 5.0_hd;
  ASSERT_TRUE(*JSObject::putNamed_RJS(lv.array, runtime, lengthID, lv.val));
  ASSERT_EQ(5u, JSArray::getLength(lv.array.get(), runtime));
  EXPECT_CALLRESULT_DOUBLE(
      5.0, JSObject::getNamed_RJS(lv.array, runtime, lengthID));

  // Try setting it to an invalid value.
  {
    lv.val = 5.1_hd;
    auto res = JSObject::putNamed_RJS(lv.array, runtime, lengthID, lv.val);
    ASSERT_TRUE(isException(res));
    runtime.clearThrownValue();
  }

  // Make sure it didn't change.
  ASSERT_EQ(5u, JSArray::getLength(lv.array.get(), runtime));
  EXPECT_CALLRESULT_DOUBLE(
      5.0, JSObject::getNamed_RJS(lv.array, runtime, lengthID));
}

#if HERMESVM_GCKIND == _HERMESVM_GCVALUE_HADES
using ArrayLargeHeapTest = LargeHeapRuntimeTestFixture;

/// An array with capacity maxYoungGenAllocationCapacity() is allocated
/// together with its storage in a single young gen allocation.
TEST_F(ArrayLargeHeapTest, CreateWithMaxYoungGenAllocationCapacity) {
  struct : Locals {
    PinnedValue<JSArray> array;
    PinnedValue<> value;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  constexpr uint32_t capacity = JSArray::maxYoungGenAllocationCapacity();
  // The array and its storage exactly fill the maximum young gen allocation.
  EXPECT_EQ(
      heapAlignSize(cellSize<JSArray>()) +
          heapAlignSize(JSArray::StorageType::allocationSize(capacity)),
      GC::maxYoungGenAllocationSize());

  auto arrayRes = JSArray::create(runtime, capacity, capacity);
  ASSERT_FALSE(isException(arrayRes));
  lv.array = std::move(*arrayRes);
  EXPECT_EQ(capacity, JSArray::getLength(lv.array.get(), runtime));

  // Write the last element, then make sure it survives a collection.
  lv.value = 1.0_hd;
  ASSERT_NE(
      JSArray::setElementAt(lv.array, runtime, capacity - 1, lv.value),
      ExecutionStatus::EXCEPTION);
  runtime.collect("test");
  EXPECT_EQ(capacity, JSArray::getLength(lv.array.get(), runtime));
  EXPECT_EQ(1.0_hd, lv.array->at(runtime, capacity - 1).unboxToHV(runtime));
}
#endif

} // namespace

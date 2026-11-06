/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "EmptyCell.h"
#include "VMRuntimeTestHelpers.h"
#include "hermes/VM/ArrayStorage.h"
#include "hermes/VM/BuildMetadata.h"
#include "hermes/VM/CardBoundaryTable.h"
#include "hermes/VM/CompressedPointer.h"
#include "hermes/VM/DummyObject.h"
#include "hermes/VM/FillerCell.h"
#include "hermes/VM/GC.h"
#include "hermes/VM/LargeDummyObject.h"
#include "hermes/VM/WeakRef.h"
#include "hermes/VM/WeakRoot-inline.h"

#include <chrono>
#include <functional>
#include <new>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

using namespace hermes::vm;

namespace {

using testhelpers::DummyObject;
using testhelpers::LargeDummyObject;

struct GCBasicsTest : public ::testing::Test {
  std::shared_ptr<DummyRuntime> runtime;
  DummyRuntime &rt;
  GCBasicsTest()
      : runtime(DummyRuntime::create(kTestGCConfigSmall)), rt(*runtime) {}
};

// Hades doesn't report its stats the same way as other GCs.
#if !defined(NDEBUG) && (HERMESVM_GCKIND != _HERMESVM_GCVALUE_HADES) && \
    HERMESVM_SANITIZE_HANDLES == 0
TEST_F(GCBasicsTest, SmokeTest) {
  auto &gc = rt.getHeap();
  GCBase::HeapInfo info;
  GCBase::DebugHeapInfo debugInfo;
  struct : Locals {
    PinnedValue<DummyObject> obj2;
  } lv;
  DummyLocalsRAII lraii{rt, &lv};

  // Verify the initial state.
  gc.getHeapInfo(info);
  gc.getDebugHeapInfo(debugInfo);
  ASSERT_EQ(0u, debugInfo.numAllocatedObjects);
  ASSERT_EQ(0u, debugInfo.numReachableObjects);
  ASSERT_EQ(0u, debugInfo.numCollectedObjects);
  ASSERT_EQ(0u, debugInfo.numFinalizedObjects);
  ASSERT_EQ(0u, info.numCollections);
  ASSERT_EQ(0u, info.allocatedBytes);

  // Collect an empty heap.
  rt.collect();
  gc.getHeapInfo(info);
  gc.getDebugHeapInfo(debugInfo);
  ASSERT_EQ(0u, debugInfo.numAllocatedObjects);
  ASSERT_EQ(0u, debugInfo.numReachableObjects);
  ASSERT_EQ(0u, debugInfo.numCollectedObjects);
  ASSERT_EQ(0u, debugInfo.numFinalizedObjects);
  ASSERT_EQ(1u, info.numCollections);
  ASSERT_EQ(0u, info.allocatedBytes);

  // Allocate a single object without GC.
  DummyObject::create(rt.getHeap(), rt);
  gc.getHeapInfo(info);
  gc.getDebugHeapInfo(debugInfo);
  ASSERT_EQ(1u, debugInfo.numAllocatedObjects);
  ASSERT_EQ(0u, debugInfo.numReachableObjects);
  ASSERT_EQ(0u, debugInfo.numCollectedObjects);
  ASSERT_EQ(0u, debugInfo.numFinalizedObjects);
  ASSERT_EQ(1u, info.numCollections);
  ASSERT_EQ(sizeof(DummyObject), info.allocatedBytes);

  // Now free the unreachable object.
  rt.collect();
  gc.getHeapInfo(info);
  gc.getDebugHeapInfo(debugInfo);
  ASSERT_EQ(0u, debugInfo.numAllocatedObjects);
  ASSERT_EQ(0u, debugInfo.numReachableObjects);
  ASSERT_EQ(1u, debugInfo.numCollectedObjects);
  ASSERT_EQ(1u, debugInfo.numFinalizedObjects);
  ASSERT_EQ(2u, info.numCollections);
  ASSERT_EQ(0u, info.allocatedBytes);

  // Allocate two objects, the second with a GC handle.
  DummyObject::create(rt.getHeap(), rt);
  lv.obj2 = DummyObject::create(rt.getHeap(), rt);
  gc.getHeapInfo(info);
  gc.getDebugHeapInfo(debugInfo);
  ASSERT_EQ(2u, debugInfo.numAllocatedObjects);
  ASSERT_EQ(0u, debugInfo.numReachableObjects);
  ASSERT_EQ(1u, debugInfo.numCollectedObjects);
  ASSERT_EQ(1u, debugInfo.numFinalizedObjects);
  ASSERT_EQ(2u, info.numCollections);
  ASSERT_EQ(2 * sizeof(DummyObject), info.allocatedBytes);

  // Collect the first but not second object.
  rt.collect();
  gc.getHeapInfo(info);
  gc.getDebugHeapInfo(debugInfo);
  ASSERT_EQ(1u, debugInfo.numAllocatedObjects);
  ASSERT_EQ(1u, debugInfo.numReachableObjects);
  ASSERT_EQ(1u, debugInfo.numCollectedObjects);
  ASSERT_EQ(1u, debugInfo.numFinalizedObjects);
  ASSERT_EQ(3u, info.numCollections);
  ASSERT_EQ(sizeof(DummyObject), info.allocatedBytes);
}

TEST_F(GCBasicsTest, MovedObjectTest) {
  auto &gc = rt.getHeap();
  GCBase::HeapInfo info;
  GCBase::DebugHeapInfo debugInfo;
  struct : Locals {
    PinnedValue<ArrayStorage> a2;
  } lv;
  DummyLocalsRAII lraii{rt, &lv};

  // Initialize three arrays, one with a GC handle.
  gcheapsize_t totalAlloc = 0;

  ArrayStorage::createForTest(gc, 0);
  totalAlloc += heapAlignSize(ArrayStorage::allocationSize(0));
  auto *a1 = ArrayStorage::createForTest(gc, 3);
  totalAlloc += heapAlignSize(ArrayStorage::allocationSize(3));
  lv.a2 = ArrayStorage::createForTest(gc, 3);
  totalAlloc += heapAlignSize(ArrayStorage::allocationSize(3));
  // Verify the initial state.
  gc.getHeapInfo(info);
  gc.getDebugHeapInfo(debugInfo);
  EXPECT_EQ(3u, debugInfo.numAllocatedObjects);
  EXPECT_EQ(0u, debugInfo.numReachableObjects);
  EXPECT_EQ(0u, debugInfo.numCollectedObjects);
  EXPECT_EQ(0u, debugInfo.numFinalizedObjects);
  EXPECT_EQ(0u, info.numCollections);
  EXPECT_EQ(totalAlloc, info.allocatedBytes);

  // Initialize a reachable graph.
  lv.a2->set(0, HermesValue::encodeObjectValue(a1), gc);
  lv.a2->set(2, HermesValue::encodeObjectValue(*lv.a2), gc);
  a1->set(0, HermesValue::encodeObjectValue(a1), gc);
  a1->set(1, HermesValue::encodeObjectValue(*lv.a2), gc);

  rt.collect();
  totalAlloc -= heapAlignSize(ArrayStorage::allocationSize(0));
  gc.getHeapInfo(info);
  gc.getDebugHeapInfo(debugInfo);
  EXPECT_EQ(2u, debugInfo.numAllocatedObjects);
  EXPECT_EQ(2u, debugInfo.numReachableObjects);
  EXPECT_EQ(1u, debugInfo.numCollectedObjects);
  EXPECT_EQ(0u, debugInfo.numFinalizedObjects);
  EXPECT_EQ(1u, info.numCollections);
  EXPECT_EQ(totalAlloc, info.allocatedBytes);

  // Extract what we know was a pointer to a1.
  a1 = vmcast<ArrayStorage>(lv.a2->at(0));

  // Ensure the contents of the objects changed.
  EXPECT_EQ(a1, lv.a2->at(0).getPointer());
  EXPECT_EQ(HermesValue::encodeEmptyValue(), lv.a2->at(1));
  EXPECT_EQ(*lv.a2, lv.a2->at(2).getPointer());
  EXPECT_EQ(a1, a1->at(0).getPointer());
  EXPECT_EQ(*lv.a2, a1->at(1).getPointer());
  EXPECT_EQ(HermesValue::encodeEmptyValue(), a1->at(2));
}

TEST_F(GCBasicsTest, WeakRefSlotTest) {
  // WeakRefSlot can hold any 4-byte aligned pointer.
  auto obj = (void *)0x12345670;
  CompressedPointer ptr =
      CompressedPointer::encode(static_cast<GCCell *>(obj), rt);
  auto shv = SmallHermesValue::encodeObjectValue(ptr);

  WeakRefSlot s;
  s.emplace(shv);
  EXPECT_TRUE(s.hasValue());
  EXPECT_EQ(ptr, s.getObjectNoBarrierUnsafe());
  EXPECT_EQ(obj, s.getObjectNoBarrierUnsafe(rt));

  // Update pointer of unmarked slot.
  auto obj2 = (void *)0x76543210;
  s.setPointer(CompressedPointer::encode(static_cast<GCCell *>(obj2), rt));
  EXPECT_FALSE(s.isFree());
  EXPECT_TRUE(s.hasValue());
  EXPECT_EQ(obj2, s.getObjectNoBarrierUnsafe(rt));

  s.clearPointer();
  EXPECT_FALSE(s.isFree());
  EXPECT_FALSE(s.hasValue());

  // Free slot.
  s.free();
  EXPECT_TRUE(s.isFree());
}

TEST_F(GCBasicsTest, WeakRefTest) {
  auto &gc = rt.getHeap();
  GCBase::DebugHeapInfo debugInfo;
  struct : Locals {
    PinnedValue<ArrayStorage> a2;
  } lv;
  DummyLocalsRAII lraii{rt, &lv};

  gc.getDebugHeapInfo(debugInfo);
  EXPECT_EQ(0u, debugInfo.numAllocatedObjects);

  lv.a2 = ArrayStorage::createForTest(gc, 10);
  auto *a1 = ArrayStorage::createForTest(gc, 10);

  gc.getDebugHeapInfo(debugInfo);
  EXPECT_EQ(2u, debugInfo.numAllocatedObjects);

  WeakRef<ArrayStorage> wr1{rt, gc, a1};
  WeakRef<ArrayStorage> wr2{rt, gc, lv.a2};

  ASSERT_TRUE(wr1.isValid());
  ASSERT_TRUE(wr2.isValid());

  ASSERT_EQ(a1, wr1.getNoBarrierUnsafe(rt));
  ASSERT_EQ(*lv.a2, wr2.getNoBarrierUnsafe(rt));

  // a1 is supposed to be freed during the following collection, so clear
  // the pointer to avoid mistakes.
  a1 = nullptr;

  // Test that freeing an object correctly "empties" the weak ref slot but
  // preserves the other slot.
  rt.collect();
  gc.getDebugHeapInfo(debugInfo);
  EXPECT_EQ(1u, debugInfo.numAllocatedObjects);
  ASSERT_FALSE(wr1.isValid());
  // Though the slot is empty, it's still reachable, so must not be freed yet.
  ASSERT_FALSE(wr1.isSlotFree());
  ASSERT_TRUE(wr2.isValid());
  ASSERT_EQ(*lv.a2, wr2.getNoBarrierUnsafe(rt));

  // Make the slot unreachable and test that it is freed.
  wr1.releaseSlot();

  ASSERT_TRUE(wr1.isSlotFree());

  // Create a new weak ref, possibly reusing the just freed slot.
  auto *a3 = ArrayStorage::createForTest(gc, 10);
  WeakRef<ArrayStorage> wr3{rt, gc, a3};

  ASSERT_TRUE(wr3.isValid());
  ASSERT_EQ(a3, wr3.getNoBarrierUnsafe(rt));

  wr2.releaseSlot();
  wr3.releaseSlot();
}
#endif // !NDEBUG && !HERMESVM_GCVALUE_HADES

TEST_F(GCBasicsTest, WeakRootTest) {
  GC &gc = rt.getHeap();

  WeakRoot<GCCell> wr;
  rt.weakRoots.push_back(&wr);
  {
    struct : Locals {
      PinnedValue<DummyObject> obj;
    } lv;
    DummyLocalsRAII lraii{rt, &lv};
    lv.obj = DummyObject::create(rt.getHeap(), rt);
    wr.set(rt, *lv.obj);
    rt.collect();
    ASSERT_EQ(wr.get(rt, gc), *lv.obj);
  }
  rt.collect();
  ASSERT_TRUE(wr.get(rt, gc) == nullptr);
}

TEST_F(GCBasicsTest, WeakSmallHermesValueTest) {
  struct : Locals {
    PinnedValue<DummyObject> o;
  } lv;
  DummyLocalsRAII lraii{rt, &lv};

  WeakSmallHermesValue wshv;
  rt.weakSHVs.push_back(&wshv);

  lv.o = DummyObject::create(rt.getHeap(), rt);
  wshv.setObject(rt, *lv.o);
  rt.collect();
  ASSERT_EQ(wshv.getPointer(rt, rt.getHeap()), *lv.o);

  lv.o = nullptr;
  rt.collect();
  ASSERT_TRUE(wshv.isInvalid());
}

TEST_F(GCBasicsTest, VariableSizeRuntimeCellOffsetTest) {
  auto *cell = ArrayStorage::createForTest(rt.getHeap(), 1);
  EXPECT_EQ(
      cell->getAllocatedSize(), heapAlignSize(ArrayStorage::allocationSize(1)));
}

TEST_F(GCBasicsTest, TestFixedRuntimeCell) {
  auto *cell = DummyObject::create(rt.getHeap(), rt);
  EXPECT_EQ(cell->getAllocatedSize(), heapAlignSize(sizeof(DummyObject)));
}

/// Test that the extra bytes in the heap are reported correctly.
TEST_F(GCBasicsTest, ExtraBytes) {
  struct : Locals {
    PinnedValue<DummyObject> o1;
    PinnedValue<DummyObject> o2;
  } lv;
  DummyLocalsRAII lraii{rt, &lv};
  auto &gc = rt.getHeap();

  {
    GCBase::HeapInfo info;
    lv.o1 = DummyObject::create(rt.getHeap(), rt);
    lv.o1->acquireExtMem(gc, 256);
    lv.o1->extraBytes = 1;
    gc.getHeapInfoWithMallocSize(info);
    EXPECT_EQ(info.externalBytes, 256);
    // Since there is one dummy in the heap, the malloc size is at least the
    // size of its corresponding WeakRefSlot and one extra byte (since
    // ManagedChunkedList may allocate more bytes than actually used).
    EXPECT_GE(info.mallocSizeEstimate, sizeof(WeakRefSlot) + 1);
  }

  {
    GCBase::HeapInfo info;
    lv.o2 = DummyObject::create(rt.getHeap(), rt);
    lv.o2->acquireExtMem(gc, 1024);
    lv.o2->extraBytes = 1;
    gc.getHeapInfoWithMallocSize(info);
    EXPECT_EQ(info.externalBytes, 256 + 1024);
    EXPECT_GE(info.mallocSizeEstimate, sizeof(WeakRefSlot) * 2 + 2);
  }

  lv.o1 = nullptr;
  lv.o2 = nullptr;
  rt.collect();

  {
    GCBase::HeapInfo info;
    gc.getHeapInfoWithMallocSize(info);
    EXPECT_EQ(info.externalBytes, 0);
    EXPECT_GE(info.mallocSizeEstimate, sizeof(WeakRefSlot) * 2);
  }
}

/// Test that the id is set to a unique number for each allocated object.
TEST_F(GCBasicsTest, TestIDIsUnique) {
  auto *cell = DummyObject::create(rt.getHeap(), rt);
  auto id1 = rt.getHeap().getObjectID(cell);
  cell = DummyObject::create(rt.getHeap(), rt);
  auto id2 = rt.getHeap().getObjectID(cell);
  EXPECT_NE(id1, id2);
}

TEST_F(GCBasicsTest, TestIDPersistsAcrossCollections) {
  struct : Locals {
    PinnedValue<DummyObject> handle;
  } lv;
  DummyLocalsRAII lraii{rt, &lv};
  lv.handle = DummyObject::create(rt.getHeap(), rt);
  const auto idBefore = rt.getHeap().getObjectID(*lv.handle);
  rt.collect();
  const auto idAfter = rt.getHeap().getObjectID(*lv.handle);
  EXPECT_EQ(idBefore, idAfter);
}

/// Test that objects that die during (YG) GC are untracked.
TEST_F(GCBasicsTest, TestIDDeathInYoung) {
  rt.getHeap().getObjectID(DummyObject::create(rt.getHeap(), rt));
  rt.collect();
  // ~DummyRuntime will verify all pointers in ID map.
}

// Hades doesn't do any GCEventKind monitoring.
TEST(GCCallbackTest, TestCallbackInvoked) {
  std::vector<GCEventKind> ev;
  auto cb = [&ev](GCEventKind kind, const char *) { ev.push_back(kind); };
  GCConfig config = GCConfig::Builder().withCallback(cb).build();
  auto rt =
      Runtime::create(RuntimeConfig::Builder().withGCConfig(config).build());
  rt->collect("test");
  EXPECT_GT(ev.size(), 0);
  for (size_t i = 0; i < ev.size(); i++) {
    if (i % 2 == 0) {
      EXPECT_EQ(GCEventKind::CollectionStart, ev[i]);
    } else {
      EXPECT_EQ(GCEventKind::CollectionEnd, ev[i]);
    }
  }
}

#if HERMESVM_GCKIND != _HERMESVM_GCVALUE_MALLOC
TEST(GCBasicsTestNCGen, TestIDPersistsAcrossMultipleCollections) {
  using SegmentCell = EmptyCell<FixedSizeHeapSegment::maxSize()>;
  constexpr size_t kSegmentCellStorageSize =
      FixedSizeHeapSegment::storageSize();
  constexpr size_t kHeapSizeHint = FixedSizeHeapSegment::maxSize() * 10;
  constexpr size_t N = kHeapSizeHint / kSegmentCellStorageSize - 1;

  const GCConfig kGCConfig = TestGCConfigFixedSize(kHeapSizeHint);
  auto runtime = DummyRuntime::create(kGCConfig);
  DummyRuntime &rt = *runtime;

  struct : Locals {
    PinnedValue<SegmentCell> handle;
    PinnedValue<SegmentCell> handle2;
    PinnedValue<SegmentCell> handles[N - 1]; // For large allocation loop
  } lv;
  DummyLocalsRAII lraii{rt, &lv};
  lv.handle = SegmentCell::create(rt);
  const auto originalID = rt.getHeap().getObjectID(*lv.handle);
  GC::HeapInfo oldHeapInfo;
  rt.getHeap().getHeapInfo(oldHeapInfo);
  // A second allocation should put the first object into the old gen.
  lv.handle2 = SegmentCell::create(rt);
  auto idAfter = rt.getHeap().getObjectID(*lv.handle);
  GC::HeapInfo newHeapInfo;
  rt.getHeap().getHeapInfo(newHeapInfo);
  EXPECT_EQ(originalID, idAfter);
  // There should have been one young gen collection.
  EXPECT_GT(newHeapInfo.numCollections, oldHeapInfo.numCollections);
  oldHeapInfo = newHeapInfo;
  // Fill the old gen to force a collection.
  for (size_t i = 0; i < N - 1; ++i) {
    lv.handles[i] = SegmentCell::create(rt);
  }
  SegmentCell::create(rt);
  idAfter = rt.getHeap().getObjectID(*lv.handle);
  rt.getHeap().getHeapInfo(newHeapInfo);
  EXPECT_EQ(originalID, idAfter);
  // There should have been one old gen collection.
  EXPECT_GT(newHeapInfo.numCollections, oldHeapInfo.numCollections);
}
#endif // #if HERMESVM_GCKIND != _HERMESVM_GCVALUE_MALLOC

// handlesan allows allocation beyond the max heap size limit, so we need to
// disable it here.
#if HERMESVM_SANITIZE_HANDLES == 0
TEST(LargeAllocationBigHeapTest, LOABasicOperations) {
  // An object with this size requires large allocation support to successfully
  // allocate within HadesGC. This should fit into a JumboHeapSegment with size
  // of 2 * kSegmentUnitSize.
  static constexpr size_t kLargeCellSize =
      JumboHeapSegment::computeActualCellSize(
          AlignedHeapSegment::kSegmentUnitSize);
  // For 32-bit platforms, or 64-bit platforms using MallocGC or HadesGC without
  // compressed pointers, we set the maximum heap size to slightly larger than
  // triple GCCell::maxNormalSize() to test allocating cells larger than this
  // limit. In other cases where GCCell::maxNormalSize() equals UINT32_MAX, we
  // don't support allocations beyond that size, so we use a reasonably large
  // heap size to ensure the remaining tests can execute properly.
  static constexpr size_t kMaxHeapSize =
      GCCell::maxNormalSize() >= (std::numeric_limits<uint32_t>::max() / 2)
      ? 1UL << 25
      : (GCCell::maxNormalSize() * 3 + 8);
  const GCConfig kGCConfig = TestGCConfigFixedSize(kMaxHeapSize);
  auto runtime = DummyRuntime::create(kGCConfig);
  DummyRuntime &rt = *runtime;

  struct : Locals {
    PinnedValue<LargeDummyObject> l1;
    PinnedValue<LargeDummyObject> l2;
    PinnedValue<DummyObject> o1;
    PinnedValue<DummyObject> o2;

    void reset() {
      l1 = nullptr;
      l2 = nullptr;
      o1 = nullptr;
      o2 = nullptr;
    }
  } lv;

  GCBase::HeapInfo heapInfo;
  // Test that YG/OG objects bytes are still correctly tracked.
  {
    DummyLocalsRAII lraii{rt, &lv};
    uint64_t expectedTotalAllocBytes = 0;
    static constexpr uint32_t kHalfYGSize = FixedSizeHeapSegment::maxSize() / 2;
    lv.l1 = LargeDummyObject::create(kLargeCellSize, rt.getHeap());
    expectedTotalAllocBytes += kLargeCellSize;
    lv.l2 = LargeDummyObject::create(kHalfYGSize, rt.getHeap());
    expectedTotalAllocBytes += kHalfYGSize;
    for (int i = 0; i < 10; ++i) {
      LargeDummyObject::create(kHalfYGSize, rt.getHeap());
      expectedTotalAllocBytes += kHalfYGSize;
    }
    rt.getHeap().getHeapInfo(heapInfo);
    ASSERT_EQ(heapInfo.totalAllocatedBytes, expectedTotalAllocBytes);
#if HERMESVM_GCKIND != _HERMESVM_GCVALUE_MALLOC
    // YG + 1 normal OG segment (created when evacuation) + 1 jumbo heap segment
    // (with size equals to 2 * kSegmentUnitSize).
    ASSERT_EQ(heapInfo.heapSize, 4 * FixedSizeHeapSegment::kSegmentUnitSize);
    // The heapSize should always be larger than allocatedBytes.
    // Note that we don't assert for exact allocatedBytes here since the YG
    // collection behavior depends on the updates of ygSizeFactor_, which could
    // be different in different builds and platforms (we are allocating a half
    // segment size object here, which may reset the segment effective end if
    // ygSizeFactor_ is smaller than 0.5).
    ASSERT_GE(heapInfo.heapSize, heapInfo.allocatedBytes);
#endif
  }
  // Clear all objects in heap.
  rt.collect();

  lv.reset();
  uint64_t prevTotalAllocBytes = 0;
  // Test that large allocation in case of insufficient space returns nullptr,
  // and allocation with size larger than GCCell::maxNormalSize() works as
  // expected.
  {
    DummyLocalsRAII lraii{rt, &lv};
    // Add a few extra bytes to ensure that the third allocation below fails due
    // to insufficient heap space.
    lv.l1 = LargeDummyObject::create(kMaxHeapSize / 3 + 8, rt.getHeap());
    lv.l2 = LargeDummyObject::create(kMaxHeapSize / 3, rt.getHeap());
    ASSERT_NE(lv.l2.get(), nullptr);
#if HERMESVM_GCKIND == _HERMESVM_GCVALUE_MALLOC
    auto expectedSize = heapAlignSize(kMaxHeapSize / 3);
#else
    auto expectedSize =
        JumboHeapSegment::computeActualCellSize(kMaxHeapSize / 3);
#endif
    // The above allocation may have size larger than GCCell::maxNormalSize(),
    // assert that we can still get correct cell size.
    ASSERT_EQ(lv.l2->getAllocatedSizeSlow(), expectedSize);
    // No enough space, this allocation should fail and return nullptr.
    ASSERT_EQ(
        LargeDummyObject::create(kMaxHeapSize / 3, rt.getHeap()), nullptr);
    rt.getHeap().getHeapInfo(heapInfo);
    prevTotalAllocBytes = heapInfo.totalAllocatedBytes;
  }
  // Full collection should free all large objects allocated above, otherwise,
  // next large object allocation will fail.
  rt.collect();

  lv.reset();
  {
    DummyLocalsRAII lraii{rt, &lv};
    // Create a LargeCell with CanBeLarge::Yes should now succeed.
    lv.l1 = LargeDummyObject::create(kLargeCellSize, rt.getHeap());
    EXPECT_NE(lv.l1.get(), nullptr);
    EXPECT_EQ(lv.l1->getAllocatedSizeSlow(), kLargeCellSize);
    rt.getHeap().getHeapInfo(heapInfo);
    // allocatedBytes should be equal to size of the only object above.
    ASSERT_EQ(heapInfo.allocatedBytes, kLargeCellSize);
    ASSERT_EQ(
        heapInfo.totalAllocatedBytes, kLargeCellSize + prevTotalAllocBytes);

    lv.o1 = DummyObject::create(rt.getHeap(), rt);
    lv.l1->hv.set(
        SmallHermesValue::encodeObjectValue(*lv.o1, rt), *lv.l1, rt.getHeap());
    lv.o2 = DummyObject::create(rt.getHeap(), rt);
    lv.l1->ptrToNormalObj.setNonNull(rt, rt.getHeap(), *lv.o2, *lv.l1);

    rt.collect();
    // Ensure pointer fields of large object are correctly updated after
    // collection.
    ASSERT_EQ(lv.l1->hv.getPointer(rt), *lv.o1);
    ASSERT_EQ(lv.l1->ptrToNormalObj.get(rt), *lv.o2);

    // Set the two handles to two new objects, and run collection. The pointer
    // fields of large object must dirty corresponding cards (since they point
    // to YG objects allocated below), otherwise, the pointed objects will be
    // freed.
    auto tmpSHV = SmallHermesValue::encodeObjectValue(
        DummyObject::create(rt.getHeap(), rt), rt);
    lv.l1->hv.set(tmpSHV, *lv.l1, rt.getHeap());
    auto tmpObj = DummyObject::create(rt.getHeap(), rt);
    lv.l1->ptrToNormalObj.setNonNull(rt, rt.getHeap(), tmpObj, *lv.l1);
    rt.collect();
    ASSERT_TRUE(lv.l1->hv.getPointer(rt)->isValid());
    ASSERT_TRUE(lv.l1->ptrToNormalObj.get(rt)->isValid());
  }
  // Clear all objects in heap.
  rt.collect();
  rt.getHeap().getHeapInfo(heapInfo);
  // The large object should be correctly freed now.
  ASSERT_EQ(heapInfo.allocatedBytes, 0);
}
#endif

/// Test that a super large allocation (larger than max of int32_t) still works
/// correctly.
TEST(LargeAllocationBigHeapTest, SuperLargeAlloc) {
  constexpr gcheapsize_t kMaxHeapSize = (1ULL << 32) - 1;
  const GCConfig kGCConfig = TestGCConfigFixedSize(kMaxHeapSize);
  auto runtime = DummyRuntime::create(kGCConfig);
  DummyRuntime &rt = *runtime;

  struct : Locals {
    PinnedValue<LargeDummyObject> l1;
    PinnedValue<LargeDummyObject> l2;
  } lv;
  DummyLocalsRAII lraii{rt, &lv};

  GCBase::HeapInfo heapInfo;
  rt.getHeap().getHeapInfo(heapInfo);
  ASSERT_EQ(heapInfo.allocatedBytes, 0);

  constexpr uint32_t size1 =
      JumboHeapSegment::computeActualCellSize(1024 * 1024 * 32);
  lv.l1 = LargeDummyObject::create(size1, rt.getHeap());
  ASSERT_NE(lv.l1.get(), nullptr);

  // This is larger than INT32_MAX. It will fail if we use wrong integer type
  // when tracking allocated bytes (e.g., we used to use int32_t in
  // incrementAllocatedBytes() of HadesGC).
  constexpr uint32_t size2 = JumboHeapSegment::computeActualCellSize(1UL << 31);
  lv.l2 = LargeDummyObject::create(size2, rt.getHeap());
  ASSERT_NE(lv.l2.get(), nullptr);

  // Assert that both large allocation succeeded and the final number of
  // allocated bytes is correct.
  rt.getHeap().getHeapInfo(heapInfo);
  ASSERT_EQ(heapInfo.allocatedBytes, (uint64_t)size1 + size2);
}

/// We do not test with MallocGC since GCOptions does not have any effect there.
/// Handle-SAN collects on allocation, which breaks the exact heap size
/// assertions below.
#if HERMESVM_GCKIND == _HERMESVM_GCVALUE_HADES && HERMESVM_SANITIZE_HANDLES == 0
/// Use a slightly larger initial heap size so that the first YG collection
/// doesn't always trigger an OG collection.
class GCBasicsTestGCOptions : public ::testing::Test {
 protected:
  std::shared_ptr<DummyRuntime> runtime{DummyRuntime::create(
      TestGCConfigFixedSize(4 * FixedSizeHeapSegment::storageSize()))};
  DummyRuntime &rt{*runtime};
};

/// Test that collectWithOptions works correctly for different GCType and
/// GCExecutionPolicy combinations.
TEST_F(GCBasicsTestGCOptions, CollectWithGCOptionsMinorSync) {
  auto &gc = rt.getHeap();
  GCBase::HeapInfo info;

  // Allocate an object without holding a reference to it.
  DummyObject::create(gc, rt);
  // Allocate a long lived object, which can't be collected by Minor GC in
  // HadesGC.
  DummyObject::createLongLived(gc);
  gc.getHeapInfo(info);
  EXPECT_EQ(cellSize<DummyObject>() * 2, info.allocatedBytes);

  constexpr GCBase::GCOptions options;
  EXPECT_EQ(GCBase::GCType::Minor, options.type);
  EXPECT_EQ(GCBase::GCExecutionPolicy::Sync, options.executionPolicy);
  gc.collectWithOptions("test", options);

  gc.getHeapInfo(info);
  // The unreachable object in YG should be collected.
  EXPECT_EQ(cellSize<DummyObject>(), info.allocatedBytes);
}

TEST(GCOptionsPromotionTest, MinorRespectsPromotionPolicy) {
  for (const auto executionPolicy :
       {GCBase::GCExecutionPolicy::Sync, GCBase::GCExecutionPolicy::Async}) {
    SCOPED_TRACE(static_cast<int>(executionPolicy));
    for (const bool revertToYGAtTTI : {false, true}) {
      SCOPED_TRACE(revertToYGAtTTI);
      auto runtime = DummyRuntime::create(TestGCConfigFixedSize(
          8 * FixedSizeHeapSegment::storageSize(),
          GCConfig::Builder(kTestGCConfigBuilder)
              .withAllocInYoung(false)
              .withRevertToYGAtTTI(revertToYGAtTTI)));
      auto &rt = *runtime;
      auto &gc = rt.getHeap();
      const GCBase::GCOptions options{GCBase::GCType::Minor, executionPolicy};
      GCBase::HeapInfo info;

      DummyObject::create(gc, rt);
      gc.collectWithOptions("test", options);
      gc.getHeapInfo(info);
      // Whole-segment promotion retains even unreachable objects.
      EXPECT_EQ(cellSize<DummyObject>(), info.allocatedBytes);

      gc.ttiReached();
      DummyObject::create(gc, rt);
      gc.collectWithOptions("test", options);
      gc.getHeapInfo(info);
      // Only the new YG object can be reclaimed; the first is already in OG.
      EXPECT_EQ(
          cellSize<DummyObject>() * (revertToYGAtTTI ? 1 : 2),
          info.allocatedBytes);
    }
  }
}

TEST_F(GCBasicsTestGCOptions, CollectWithGCOptionsMajorSync) {
  auto &gc = rt.getHeap();
  GCBase::HeapInfo info;

  // Allocate an object without holding a reference to it.
  DummyObject::create(gc, rt);
  // Allocate a long lived object, which can't be collected by Minor GC in
  // HadesGC.
  DummyObject::createLongLived(gc);
  gc.getHeapInfo(info);
  EXPECT_EQ(cellSize<DummyObject>() * 2, info.allocatedBytes);

  // Collect with Major GC type and Sync execution policy.
  GCBase::GCOptions options{
      GCBase::GCType::Major, GCBase::GCExecutionPolicy::Sync};
  gc.collectWithOptions("test", options);

  gc.getHeapInfo(info);
  // The unreachable objects in both YG and OG should be collected.
  EXPECT_EQ(0u, info.allocatedBytes);
}

TEST_F(GCBasicsTestGCOptions, CollectWithGCOptionsMajorAsync) {
  auto &gc = rt.getHeap();
  GCBase::HeapInfo info;

  // Allocate an object without holding a reference to it.
  DummyObject::create(gc, rt);
  // Allocate a long lived object, which can't be collected by Minor GC in
  // HadesGC.
  DummyObject::createLongLived(gc);
  gc.getHeapInfo(info);
  // Two objects allocated: one in YG, one in OG.
  EXPECT_EQ(cellSize<DummyObject>() * 2, info.allocatedBytes);

  // Collect with Major GC type and Async execution policy.
  // This triggers a YG collection synchronously, which collects the
  // unreachable object in young gen. The OG collection runs asynchronously.
  GCBase::GCOptions options{
      GCBase::GCType::Major, GCBase::GCExecutionPolicy::Async};
  gc.collectWithOptions("test", options);
  gc.getHeapInfo(info);
  // The YG object is collected; the long-lived OG object remains until
  // the async OG collection completes.
  EXPECT_EQ(cellSize<DummyObject>(), info.allocatedBytes);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (info.allocatedBytes != 0 &&
         std::chrono::steady_clock::now() < deadline) {
    gc.collectWithOptions("test", options);
    gc.getHeapInfo(info);
    std::this_thread::yield();
  }
  // Sweeping may finish during the last yield, even if the deadline expires.
  gc.getHeapInfo(info);
  EXPECT_EQ(0u, info.allocatedBytes);
}

/// Test that collectWithOptions preserves reachable objects.
TEST_F(GCBasicsTestGCOptions, CollectWithGCOptionsPreservesReachable) {
  auto &gc = rt.getHeap();
  GCBase::HeapInfo info;
  struct : Locals {
    PinnedValue<DummyObject> handle;
  } lv;
  DummyLocalsRAII lraii{rt, &lv};

  // Allocate a reachable object.
  lv.handle = DummyObject::create(gc, rt);
  gc.getHeapInfo(info);
  EXPECT_EQ(cellSize<DummyObject>(), info.allocatedBytes);

  // Collect with Minor GC.
  GCBase::GCOptions minorOptions{
      GCBase::GCType::Minor, GCBase::GCExecutionPolicy::Sync};
  gc.collectWithOptions("test", minorOptions);

  gc.getHeapInfo(info);
  // The reachable object should still be present.
  EXPECT_EQ(cellSize<DummyObject>(), info.allocatedBytes);

  // Collect with Major GC.
  GCBase::GCOptions majorOptions{
      GCBase::GCType::Major, GCBase::GCExecutionPolicy::Sync};
  gc.collectWithOptions("test", majorOptions);

  gc.getHeapInfo(info);
  // The reachable object should still be present.
  EXPECT_EQ(cellSize<DummyObject>(), info.allocatedBytes);
}

TEST_F(GCBasicsTestGCOptions, CollectWithGCOptionsMajorAsyncCoalesces) {
  auto &gc = rt.getHeap();
  GCBase::HeapInfo info;
  struct : Locals {
    PinnedValue<DummyObject> object;
  } lv;
  DummyLocalsRAII lraii{rt, &lv};

  lv.object = DummyObject::createLongLived(gc);
  GCBase::GCOptions options{
      GCBase::GCType::Major, GCBase::GCExecutionPolicy::Async};
  gc.collectWithOptions("test", options);
  lv.object = nullptr;

  // The second request assists marking without finishing the entire first
  // collection and starting another one.
  gc.collectWithOptions("test", options);
  gc.getHeapInfo(info);
  // OG statistics are published by a later YG collection, so this stays zero
  // even if background sweeping has already finished.
  ASSERT_EQ(0u, info.fullStats.numCollections);

  const auto collectUntilCompleted = [&](unsigned numCollections) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
      gc.collectWithOptions("test", options);
      gc.getHeapInfo(info);
      if (info.fullStats.numCollections >= numCollections)
        return true;
      std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  };

  ASSERT_TRUE(collectUntilCompleted(1));
  ASSERT_EQ(1u, info.fullStats.numCollections);
  // The object was reachable when the first collection began, so it survives
  // that collection even though the root has since been cleared.
  EXPECT_EQ(cellSize<DummyObject>(), info.allocatedBytes);

  ASSERT_TRUE(collectUntilCompleted(2));
  EXPECT_EQ(2u, info.fullStats.numCollections);
  EXPECT_EQ(0u, info.allocatedBytes);
}

/// Test scanning a dirty card when an OG collection completes while an object
/// in the card is being marked. Marking the object promotes a YG object, and
/// since the OG is full and the heap is at its maximum size, OldGen::allocSlow
/// completes the OG collection on the mutator. Its sweep must not trim the
/// object being marked: the scan has already computed where the object ends,
/// and the trimmed space would be coalesced with the dead cell after it.
/// ArrayStorage trimming is currently disabled, in which case nothing is
/// trimmed anyway, but the OG collection still completes in the middle of the
/// scan.
TEST(GCDirtyCardScanTest, OGCollectionCompletesWhileMarking) {
  // Only room for the YG segment and a single OG segment, so no OG segment can
  // be added, and the OG collection doesn't choose a compactee.
  auto runtime = DummyRuntime::create(
      TestGCConfigFixedSize(2 * FixedSizeHeapSegment::storageSize()));
  DummyRuntime &rt = *runtime;
  auto &gc = rt.getHeap();
  struct : Locals {
    PinnedValue<ArrayStorage> x;
    PinnedValue<ArrayStorage> a;
  } lv;
  DummyLocalsRAII lraii{rt, &lv};

  auto allocDeadCell = [&rt](uint32_t size) {
    return rt.makeAVariable<FillerCell, HasFinalizer::No, LongLived::Yes>(size);
  };
  auto allocStorage = [&rt, &gc](
                          ArrayStorage::size_type capacity,
                          ArrayStorage::size_type size) {
    auto *storage =
        rt.makeAVariable<ArrayStorage, HasFinalizer::No, LongLived::Yes>(
            ArrayStorage::allocationSize(capacity));
    ArrayStorage::resizeWithinCapacity(storage, gc, size);
    return storage;
  };

  // The first OG allocation is at the start of the new segment, and the rest
  // of the segment becomes a free cell. Later allocations are carved from the
  // end of that free cell, which gives the layout [p][free][d][x][a][b].
  FillerCell *p = allocDeadCell(heapAlignSize(64));
  FillerCell *b = allocDeadCell(heapAlignSize(64));
  // a has spare capacity, so a sweep that trims would trim it.
  ArrayStorage *a = allocStorage(16, 1);
  lv.a = a;
  ArrayStorage *x = allocStorage(4, 4);
  lv.x = x;
  // d leaves only a small free cell, so the OG has no room to promote y below.
  const uint32_t used = p->getAllocatedSize() + b->getAllocatedSize() +
      a->getAllocatedSize() + x->getAllocatedSize();
  FillerCell *d = allocDeadCell(
      (FixedSizeHeapSegment::maxSize() - used - GC::minAllocationSize() - 64) &
      ~(HeapAlign - 1));

  // x, a and b are adjacent and in the same card, so a is marked in the middle
  // of the scan of that card, and the cell after it is dead.
  auto *xStart = reinterpret_cast<char *>(x);
  auto *aStart = reinterpret_cast<char *>(a);
  auto *bStart = reinterpret_cast<char *>(b);
  ASSERT_EQ(reinterpret_cast<char *>(d) + d->getAllocatedSize(), xStart);
  ASSERT_EQ(xStart + x->getAllocatedSize(), aStart);
  ASSERT_EQ(aStart + a->getAllocatedSize(), bStart);
  auto cardOf = [](const char *ptr) {
    return reinterpret_cast<uintptr_t>(ptr) >> CardBoundaryTable::kLogCardSize;
  };
  ASSERT_EQ(cardOf(xStart), cardOf(bStart + b->getAllocatedSize() - 1));
  const ArrayStorage::size_type aCapacity = a->capacity();

  // Start an OG collection.
  gc.collectWithOptions(
      "test", {GCBase::GCType::Major, GCBase::GCExecutionPolicy::Async});
  GCBase::HeapInfo info;
  gc.getHeapInfo(info);
  ASSERT_EQ(0u, info.fullStats.numCollections);

  // y is only reachable from a, so it is promoted when a's card is scanned.
  auto *y = rt.makeAVariable<ArrayStorage>(ArrayStorage::allocationSize(500));
  lv.a->set(0, HermesValue::encodeObjectValue(y), gc);
  gc.collectWithOptions(
      "test", {GCBase::GCType::Minor, GCBase::GCExecutionPolicy::Sync});

  // Promoting y required completing the OG collection, which did not trim a.
  gc.getHeapInfo(info);
  EXPECT_EQ(1u, info.fullStats.numCollections);
  EXPECT_EQ(aCapacity, lv.a->capacity());
  ASSERT_TRUE(lv.a->at(0).isObject());
  EXPECT_FALSE(
      gc.inYoungGen(static_cast<const GCCell *>(lv.a->at(0).getObject())));
}
#endif

} // namespace

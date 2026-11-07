/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/sh_config.h"

#if HERMESVM_GCKIND != _HERMESVM_GCVALUE_MALLOC

#include "gtest/gtest.h"

#include "hermes/VM/AlignedHeapSegment.h"
#include "hermes/VM/GCCell.h"
#include "hermes/VM/HeapAlign.h"
#include "hermes/VM/LimitedStorageProvider.h"

namespace {

using namespace hermes;
using namespace hermes::vm;

/// Calculate the length of the longest prefix of the sequence from \p begin
/// (inclusive) to \p end (exclusive) containing at most one unique value.
static size_t runLength(const size_t *begin, const size_t *end) {
  const size_t *it = begin;
  size_t count = 0;
  while (it < end && *it++ == *begin)
    count++;
  return count;
}

static char *alignPointer(char *p, size_t align) {
  return reinterpret_cast<char *>(
      llvh::alignTo(reinterpret_cast<uintptr_t>(p), align));
}

struct AlignedHeapSegmentTest : public ::testing::Test {
  AlignedHeapSegmentTest()
      : provider_(StorageProvider::mmapProvider()),
        s(std::move(FixedSizeHeapSegment::create(provider_.get()).get())) {}

  ~AlignedHeapSegmentTest() = default;

  std::unique_ptr<StorageProvider> provider_;
  FixedSizeHeapSegment s;
};

/// Testing against a reasonably large segment size.
static constexpr size_t jumboSize = AlignedHeapSegment::kSegmentUnitSize * 8;

struct JumboHeapSegmentTest : public ::testing::Test {
  JumboHeapSegmentTest()
      : provider_(StorageProvider::mmapProvider()),
        s(std::move(
            JumboHeapSegment::create(provider_.get(), "jumbo", jumboSize)
                .get())) {}

  std::unique_ptr<StorageProvider> provider_;
  JumboHeapSegment s;
};

TEST_F(JumboHeapSegmentTest, Alloc) {
  auto *cell = s.alloc();
  EXPECT_EQ(cell, s.start());
  EXPECT_EQ(s.level(), s.hiLim());
}

TEST_F(JumboHeapSegmentTest, AllocSize) {
  uint32_t sz = AlignedHeapSegment::kSegmentUnitSize;
  // Allocate an object with the size of a normal segment would require double
  // size.
  EXPECT_EQ(
      AlignedHeapSegment::kSegmentUnitSize * 2,
      JumboHeapSegment::computeSegmentSize(sz));
  sz = AlignedHeapSegment::kSegmentUnitSize * 2 - 1;
  EXPECT_EQ(
      AlignedHeapSegment::kSegmentUnitSize * 3,
      JumboHeapSegment::computeSegmentSize(sz));
  sz = AlignedHeapSegment::kSegmentUnitSize * 2 -
      AlignedHeapSegment::kOffsetOfAllocRegion;
  EXPECT_EQ(
      AlignedHeapSegment::kSegmentUnitSize * 2,
      JumboHeapSegment::computeSegmentSize(sz));
}

#ifndef NDEBUG
TEST_F(AlignedHeapSegmentTest, FailedAllocation) {
  LimitedStorageProvider limitedProvider{StorageProvider::mmapProvider(), 0};
  auto result = FixedSizeHeapSegment::create(&limitedProvider);
  EXPECT_FALSE(result);
}
#endif // !NDEBUG

TEST_F(AlignedHeapSegmentTest, Start) {
  char *lo = s.lowLim();
  char *hi = s.hiLim();

  EXPECT_EQ(lo, FixedSizeHeapSegment::storageStart(lo));
  EXPECT_EQ(
      lo,
      FixedSizeHeapSegment::storageStart(
          lo + FixedSizeHeapSegment::storageSize() / 2));
  EXPECT_EQ(lo, FixedSizeHeapSegment::storageStart(hi - 1));

  // `hi` is the first address in the storage following \c storage (if
  // such a storage existed).
  EXPECT_EQ(hi, FixedSizeHeapSegment::storageStart(hi));
}

TEST_F(AlignedHeapSegmentTest, End) {
  char *lo = s.lowLim();
  char *hi = s.hiLim();

  EXPECT_EQ(hi, FixedSizeHeapSegment::storageEnd(lo));
  EXPECT_EQ(
      hi,
      FixedSizeHeapSegment::storageEnd(
          lo + FixedSizeHeapSegment::storageSize() / 2));
  EXPECT_EQ(hi, FixedSizeHeapSegment::storageEnd(hi - 1));

  // `hi` is the first address in the storage following \c storage (if
  // such a storage existed).
  EXPECT_EQ(
      hi + FixedSizeHeapSegment::storageSize(),
      FixedSizeHeapSegment::storageEnd(hi));
}

TEST_F(AlignedHeapSegmentTest, Offset) {
  char *lo = s.lowLim();
  char *hi = s.hiLim();
  const size_t size = FixedSizeHeapSegment::storageSize();

  EXPECT_EQ(0, FixedSizeHeapSegment::offset(lo));
  EXPECT_EQ(size / 2, FixedSizeHeapSegment::offset(lo + size / 2));
  EXPECT_EQ(size - 1, FixedSizeHeapSegment::offset(hi - 1));

  // `hi` is the first address in the storage following \c storage (if
  // such a storage existed).
  EXPECT_EQ(0, FixedSizeHeapSegment::offset(hi));
}

TEST_F(AlignedHeapSegmentTest, AdviseUnused) {
// TODO(T40416012) Re-enable this test on Windows when vm_unused is fixed.
// Skip this test in Windows because vm_unused has a no-op implementation. Skip
// it when huge pages are on because we do not return memory to the OS.
#if !defined(_WINDOWS) && !defined(HERMESVM_ALLOW_HUGE_PAGES)
  const size_t PG_SIZE = oscompat::page_size();

  ASSERT_EQ(0, FixedSizeHeapSegment::storageSize() % PG_SIZE);

  const size_t TOTAL_PAGES = FixedSizeHeapSegment::storageSize() / PG_SIZE;
  const size_t FREED_PAGES = TOTAL_PAGES / 2;

  // We can't use the storage of s here since it contains guard pages and also
  // s.start() may not align to actual page boundary.
  void *storage =
      provider_->newStorage(FixedSizeHeapSegment::storageSize()).get();
  char *start = reinterpret_cast<char *>(storage);
  char *end = start + FixedSizeHeapSegment::storageSize();

  // On some platforms, the mapping containing [start, end) can be larger than
  // [start, end) itself, and the extra space may already contribute to the
  // footprint, so we account for this in \c initial.
  auto initial = oscompat::vm_footprint(start, end);
  ASSERT_TRUE(initial);

  for (volatile char *p = start; p < end; p += PG_SIZE)
    *p = 1;

  auto touched = oscompat::vm_footprint(start, end);
  ASSERT_TRUE(touched);

  oscompat::vm_unused(start, FREED_PAGES * PG_SIZE);

  auto marked = oscompat::vm_footprint(start, end);
  ASSERT_TRUE(marked);

#ifndef QEMU_MODE
  EXPECT_EQ(*initial + TOTAL_PAGES, *touched);
  EXPECT_EQ(*touched - FREED_PAGES, *marked);
#endif

  provider_->deleteStorage(storage, FixedSizeHeapSegment::storageSize());
#endif
}

TEST_F(AlignedHeapSegmentTest, Containment) {
  // Boundaries
  EXPECT_FALSE(s.contains(s.lowLim() - 1));
  EXPECT_TRUE(s.contains(s.lowLim()));
  EXPECT_TRUE(s.contains(s.hiLim() - 1));
  EXPECT_FALSE(s.contains(s.hiLim()));

  // Interior
  EXPECT_TRUE(s.contains(s.lowLim() + FixedSizeHeapSegment::storageSize() / 2));
}

TEST_F(AlignedHeapSegmentTest, Alignment) {
  /**
   * This test alternates between allocating an FixedSizeHeapSegment, and an
   * anonymous "spacer" mapping such that the i-th spacer has size:
   *
   *     FixedSizeHeapSegment::storageSize() + i MB
   *
   * In the worst case the anonymous mappings are perfectly interleaved with the
   * aligned storage, and we must be intentional about aligning the storage
   * allocations, like so:
   *
   *     ---+---+---+---+---+----+--+---+----+--+---+-----+-+---+---
   *     ...|AAA|SSS/   |AAA|SSSS|  |AAA|SSSS/  |AAA|SSSSS| |AAA|...
   *     ---+---+---+---+---+----+--+---+----+--+---+-----+-+---+---
   *
   * In the above diagram:
   *
   * - A character width correseponds to 2MB.
   * - A box's width includes its left boundary and excludes its right boundary.
   * - A / boundary indicates 1MB belongs to the previous box and 1MB to the
   *   next.
   * - Boxes labeled with `A` are FixedSizeHeapSegment.
   * - Boxes labeled with `S` are spacers.
   * - Boxes with no label are unmapped.
   *
   * We cannot guarantee that we get this layout, but spacers disturb the
   * allocation pattern we (might) get from allocating in a tight loop.
   */

  std::vector<FixedSizeHeapSegment> segments;
  std::vector<void *> spacers;

  const size_t MB = 1 << 20;
  const size_t SIZE = FixedSizeHeapSegment::storageSize();

  for (size_t space = SIZE + MB; space < 2 * SIZE; space += MB) {
    segments.emplace_back(
        std::move(FixedSizeHeapSegment::create(provider_.get()).get()));
    FixedSizeHeapSegment &seg = segments.back();

    EXPECT_EQ(seg.lowLim(), alignPointer(seg.lowLim(), SIZE));

    spacers.push_back(oscompat::vm_allocate(space).get());
  }

  { // When \c storages goes out of scope, it will correctly destruct the \c
    // FixedSizeHeapSegment instances it holds. \c spacers, on the other hand,
    // holds only raw pointers, so we must clean them up manually:
    size_t space = SIZE + MB;
    for (void *spacer : spacers) {
      oscompat::vm_free(spacer, space);
      space += MB;
    }
  }
}

TEST_F(AlignedHeapSegmentTest, AllocTest) {
  const size_t INIT_BYTES = heapAlignSize(sizeof(GCCell));
  const size_t STEP_BYTES = HeapAlign;

  size_t allocated = 0;
  size_t size = INIT_BYTES;
  const size_t capacity = s.available();
  EXPECT_LE(capacity, s.size());

  AllocResult res = s.alloc(size);
  while (res.success) {
    allocated += size;

    auto raw = reinterpret_cast<size_t *>(res.ptr);
    std::fill(raw, raw + size / sizeof(size_t), size);

    EXPECT_EQ(allocated, s.used());
    EXPECT_EQ(capacity, s.used() + s.available());

    size += STEP_BYTES;
    res = s.alloc(size);
  }

  // This is the only reason why the allocation could have failed.
  EXPECT_LT(s.available(), size);

  const size_t INIT_SIZE = INIT_BYTES / sizeof(size_t);
  const size_t STEP_SIZE = STEP_BYTES / sizeof(size_t);
  const size_t FINAL_SIZE = size / sizeof(size_t);

  // Check the bit pattern we wrote during allocation
  size_t *data = reinterpret_cast<size_t *>(s.start());
  size_t *const end = reinterpret_cast<size_t *>(s.start() + allocated);

  for (size_t run = INIT_SIZE; run < FINAL_SIZE; run += STEP_SIZE) {
    size_t actualRun = runLength(data, end);
    EXPECT_EQ(run, actualRun) << "Run " << run;
    data += run;
  }
}

TEST_F(AlignedHeapSegmentTest, FullSize) {
  EXPECT_EQ(s.size(), FixedSizeHeapSegment::maxSize());
  EXPECT_EQ(s.size(), s.available());
  EXPECT_EQ(s.size(), s.hiLim() - s.start());

  // Try and allocate the entire region.
  AllocResult res = s.alloc(s.size());

  EXPECT_TRUE(res.success);
  EXPECT_TRUE(nullptr != res.ptr);
}

TEST_F(AlignedHeapSegmentTest, ResetLevel) {
  // Make the level different from the start of the region.
  AllocResult res = s.alloc(cellSize<GCCell>());
  ASSERT_TRUE(res.success);
  ASSERT_NE(s.start(), s.level());

  s.resetLevel();
  EXPECT_EQ(s.start(), s.level());
}

/// Charging external memory lowers the effective end by as much of it as the
/// segment has room for.
TEST_F(AlignedHeapSegmentTest, ChargeExternalMemory) {
  const size_t room = s.available();
  s.chargeExternalMemory(room / 4);
  EXPECT_EQ(room - room / 4, s.available());

  s.chargeExternalMemory(room);
  EXPECT_EQ(0u, s.available());
}

/// A charge takes only the room above the level, and capping the charge at zero
/// gives it back.
TEST_F(AlignedHeapSegmentTest, ChargeExternalMemoryStopsAtLevel) {
  AllocResult res = s.alloc(cellSize<GCCell>());
  ASSERT_TRUE(res.success);

  s.chargeExternalMemory(s.size());
  EXPECT_EQ(s.level(), s.effectiveEnd());

  s.capExternalMemoryCharge(0);
  EXPECT_EQ(s.end(), s.effectiveEnd());
}

/// Capping the charge gives back the part of it that the external memory still
/// credited no longer accounts for.
TEST_F(AlignedHeapSegmentTest, CapExternalMemoryCharge) {
  const size_t room = s.available();
  s.chargeExternalMemory(room / 2);

  s.capExternalMemoryCharge(room / 2);
  EXPECT_EQ(room - room / 2, s.available());

  s.capExternalMemoryCharge(room / 8);
  EXPECT_EQ(room - room / 8, s.available());

  s.capExternalMemoryCharge(0);
  EXPECT_EQ(room, s.available());
}

/// A charge that found less room than it asked for is given back only as far
/// as it lowered the effective end.
TEST_F(AlignedHeapSegmentTest, CapExternalMemoryChargeThatStoppedAtLevel) {
  const size_t room = s.available();
  s.chargeExternalMemory(room);
  s.chargeExternalMemory(room);
  EXPECT_EQ(0u, s.available());

  // Both credits outstanding account for more than the charge, which stays.
  s.capExternalMemoryCharge(2 * room);
  EXPECT_EQ(s.level(), s.effectiveEnd());

  // The credit still outstanding accounts for all the room.
  s.capExternalMemoryCharge(room);
  EXPECT_EQ(0u, s.available());

  s.capExternalMemoryCharge(0);
  EXPECT_EQ(room, s.available());
}

/// Setting the effective end forgets the charge taken before it.
TEST_F(AlignedHeapSegmentTest, SetEffectiveEndForgetsExternalMemoryCharge) {
  const size_t room = s.available();
  s.chargeExternalMemory(room / 2);
  s.setEffectiveEnd(s.effectiveEnd());

  s.capExternalMemoryCharge(0);
  EXPECT_EQ(room - room / 2, s.available());
}

#ifndef NDEBUG

using AlignedHeapSegmentDeathTest = AlignedHeapSegmentTest;

// Allocating into a null segment causes an assertion failure on !NDEBUG builds.
TEST_F(AlignedHeapSegmentDeathTest, NullAlloc) {
  FixedSizeHeapSegment s;
  constexpr uint32_t SIZE = heapAlignSize(sizeof(GCCell));
  EXPECT_DEATH_IF_SUPPORTED({ s.alloc(SIZE); }, "null segment");
}
#endif // !NDEBUG

} // namespace

#endif // !HERMESVM_GCVALUE_MALLOC

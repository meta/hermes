/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/ADT/ManagedChunkedList.h"

#include "gtest/gtest.h"

#include <vector>

namespace {

using namespace hermes;

// A ManagedChunkedList element that indicates whether it's occupied based on a
// member boolean.
struct ManagedValue {
  ManagedValue() {}

  bool isFree() {
    return !occupied_;
  }

  void emplace(int value) {
    value_ = value;
    occupied_ = true;
  }

  void free() {
    occupied_ = false;
  }

  ManagedValue *getNextFree() {
    return nextFree_;
  }

  void setNextFree(ManagedValue *nextFree) {
    nextFree_ = nextFree;
  }

  int value() {
    return value_;
  }

 private:
  bool occupied_ = false;
  union {
    int value_;
    ManagedValue *nextFree_;
  };
};

constexpr double kOccupancyRatio = 0.5;
constexpr double kSizingWeight = 0.5;
constexpr size_t kElementsPerChunk = 16;
using IntegerList = ManagedChunkedList<ManagedValue, kElementsPerChunk>;

TEST(ManagedChunkListTest, Empty) {
  IntegerList list(kOccupancyRatio, kSizingWeight);
  EXPECT_EQ(0, list.capacity());
  EXPECT_EQ(0, list.sizeForTests());
}

TEST(ManagedChunkListTest, Add) {
  IntegerList list(kOccupancyRatio, kSizingWeight);
  list.add(1);
  EXPECT_GT(list.capacity(), 0);
  EXPECT_EQ(1, list.sizeForTests());
}

TEST(ManagedChunkListTest, Remove) {
  IntegerList list(kOccupancyRatio, kSizingWeight);

  // Insert some values
  list.add(111);
  list.add(222);
  list.add(333);
  EXPECT_EQ(3, list.sizeForTests());

  // Remove the odd values
  list.forEach([](ManagedValue &element) {
    if ((element.value() % 2) == 1) {
      element.free();
    }
  });

  // Ensure removed values are ignored.
  EXPECT_EQ(1, list.sizeForTests());
  list.forEach([](ManagedValue &element) { EXPECT_EQ(222, element.value()); });
}

TEST(ManagedChunkListTest, OnlyAllocateWhenNecessary) {
  IntegerList list(kOccupancyRatio, kSizingWeight);

  // Populate exactly one chunk
  for (size_t value = 0; value < kElementsPerChunk; value++) {
    list.add(value);
  }
  EXPECT_EQ(kElementsPerChunk, list.sizeForTests());
  EXPECT_EQ(kElementsPerChunk, list.capacity());

  // Exceed the capacity of a single chunk
  list.add(1234);

  // Ensure the capacity grew.
  EXPECT_EQ(kElementsPerChunk + 1, list.sizeForTests());
  EXPECT_GT(list.capacity(), kElementsPerChunk);
}

TEST(ManagedChunkListTest, CollectionReusesFreeItems) {
  constexpr size_t kChunkCount = 3;
  IntegerList list(kOccupancyRatio, kSizingWeight);

  // Add exactly enough values to fill kChunkCount chunks.
  size_t oddSum = 0;
  size_t evenSum = 0;
  for (size_t value = 0; value < kChunkCount * kElementsPerChunk; value++) {
    list.add(value);

    if ((value % 2) == 0) {
      evenSum += value;
    } else {
      oddSum += value;
    }
  }
  EXPECT_EQ(kChunkCount * kElementsPerChunk, list.capacity());

  // Remove odd values
  list.forEach([](ManagedValue &element) {
    if ((element.value() % 2) == 1) {
      element.free();
    }
  });
  list.collect();
  EXPECT_EQ(kChunkCount * kElementsPerChunk / 2, list.sizeForTests());
  EXPECT_EQ(kChunkCount * kElementsPerChunk, list.capacity());

  // Reuse odd slots, inserting a value 3x as large
  for (size_t value = 0; value < kChunkCount * kElementsPerChunk; value++) {
    if ((value % 2) == 1) {
      list.add(value * 3);
    }
  }

  // Ensure all elements are present with no additional capacity.
  EXPECT_EQ(kChunkCount * kElementsPerChunk, list.sizeForTests());
  EXPECT_EQ(kChunkCount * kElementsPerChunk, list.capacity());
  size_t sum = 0;
  list.forEach([&sum](ManagedValue &element) { sum += element.value(); });
  EXPECT_EQ(evenSum + oddSum * 3, sum);
}

TEST(ManagedChunkListTest, CollectionIsTriggered) {
  // Ensure storing many values one at a time doesn't allocate
  // extra chunks; collections should keep the capacity small.
  constexpr size_t kElementCount = kElementsPerChunk * 100;
  IntegerList list(kOccupancyRatio, kSizingWeight);
  for (size_t value = 0; value < kElementCount; value++) {
    list.add(value).free();
  }
  EXPECT_EQ(kElementsPerChunk, list.capacity());
}

TEST(ManagedChunkListTest, ChunksAreFreed) {
  constexpr size_t kInitialChunkCount = 10;
  constexpr size_t kInitialElementCount =
      kElementsPerChunk * kInitialChunkCount;
  IntegerList list(kOccupancyRatio, kSizingWeight);

  // Populate chunks
  for (size_t value = 0; value < kInitialElementCount; value++) {
    list.add(value);
  }
  EXPECT_EQ(kInitialElementCount, list.capacity());

  // Reduce the list to a fraction of its initial size
  constexpr size_t kReductionFactor = 5;
  size_t index = 0;
  list.forEach([=, &index](ManagedValue &element) {
    size_t chunkNumber = index / kElementsPerChunk;
    if ((chunkNumber % kReductionFactor) != 0) {
      element.free();
    }
    index++;
  });

  // Trigger many collections
  for (size_t value = 0; value < kInitialElementCount * 100; value++) {
    list.add(value).free();
  }

  // Ensure capacity was eventually reduced to the expected capacity given
  // the occupancy ratio
  EXPECT_EQ(
      list.capacity(),
      kInitialElementCount / kOccupancyRatio / kReductionFactor);
}

// Return the sum of the values of the elements visited by forEachRecorded.
int sumRecorded(const IntegerList &list) {
  int sum = 0;
  list.forEachRecorded(
      [&sum](ManagedValue &element) { sum += element.value(); });
  return sum;
}

TEST(ManagedChunkListTest, RecordedElements) {
  IntegerList list(kOccupancyRatio, kSizingWeight);
  ManagedValue &a = list.add(1);
  list.add(2);
  ManagedValue &c = list.add(3);
  EXPECT_EQ(0, sumRecorded(list));

  // An element may be recorded more than once.
  list.record(a);
  list.record(c);
  list.record(c);
  EXPECT_EQ(3, list.numRecordedForTests());
  EXPECT_EQ(1 + 3 + 3, sumRecorded(list));

  // Free elements are skipped.
  c.free();
  EXPECT_EQ(1, sumRecorded(list));

  list.clearRecorded();
  EXPECT_EQ(0, list.numRecordedForTests());
  EXPECT_EQ(0, sumRecorded(list));
}

TEST(ManagedChunkListTest, RecordedElementReused) {
  IntegerList list(kOccupancyRatio, kSizingWeight);

  // Fill a chunk, and record one of its elements.
  ManagedValue &a = list.add(1);
  for (size_t value = 2; value <= kElementsPerChunk; value++) {
    list.add(value);
  }
  EXPECT_EQ(kElementsPerChunk, list.capacity());
  list.record(a);
  EXPECT_EQ(1, sumRecorded(list));

  // Freeing the element does not remove its record.
  a.free();
  EXPECT_EQ(1, list.numRecordedForTests());
  EXPECT_EQ(0, sumRecorded(list));

  // Collecting keeps the chunk, and puts the freed element on the free list, so
  // adding a value reuses it. The record then visits the new value.
  list.collect();
  ManagedValue &b = list.add(100);
  EXPECT_EQ(&a, &b);
  EXPECT_EQ(kElementsPerChunk, list.capacity());
  EXPECT_FALSE(list.recordsDropped());
  EXPECT_EQ(100, sumRecorded(list));
}

TEST(ManagedChunkListTest, RecordedElementsOverflow) {
  IntegerList list(kOccupancyRatio, kSizingWeight);
  ManagedValue &a = list.add(1);
  list.add(2);
  EXPECT_EQ(kElementsPerChunk, list.capacity());

  // Records are dropped when there would be more than kMaxRecords of them.
  for (size_t i = 0; i < IntegerList::kMaxRecords; i++) {
    list.record(a);
  }
  EXPECT_FALSE(list.recordsDropped());
  EXPECT_EQ(IntegerList::kMaxRecords, list.numRecordedForTests());
  list.record(a);
  EXPECT_TRUE(list.recordsDropped());
  EXPECT_EQ(0, list.numRecordedForTests());
  list.record(a);
  EXPECT_TRUE(list.recordsDropped());
  EXPECT_EQ(0, list.numRecordedForTests());

  // After an overflow, forEachRecorded visits every element that is not free.
  EXPECT_EQ(1 + 2, sumRecorded(list));

  list.clearRecorded();
  EXPECT_FALSE(list.recordsDropped());
  EXPECT_EQ(0, sumRecorded(list));
  list.record(a);
  EXPECT_EQ(1, list.numRecordedForTests());
  EXPECT_EQ(1, sumRecorded(list));
}

TEST(ManagedChunkListTest, RecordedElementsAreDroppedWithTheirChunk) {
  constexpr size_t kChunkCount = 10;
  IntegerList list(kOccupancyRatio, kSizingWeight);

  // Populate chunks and record one element in each.
  std::vector<ManagedValue *> elements;
  for (size_t value = 0; value < kChunkCount * kElementsPerChunk; value++) {
    elements.push_back(&list.add(value));
  }
  for (size_t i = 0; i < elements.size(); i += kElementsPerChunk) {
    list.record(*elements[i]);
  }
  EXPECT_EQ(kChunkCount, list.numRecordedForTests());

  // Free everything and trigger many collections, which delete chunks.
  for (ManagedValue *element : elements) {
    element->free();
  }
  for (size_t value = 0; value < kChunkCount * kElementsPerChunk * 100;
       value++) {
    list.add(value).free();
  }
  EXPECT_EQ(kElementsPerChunk, list.capacity());

  // Deleting a chunk while there are records drops all of them, so that none
  // of them refers to a deleted chunk.
  EXPECT_EQ(0, list.numRecordedForTests());
  EXPECT_TRUE(list.recordsDropped());
  ManagedValue &a = list.add(1);
  list.record(a);
  EXPECT_EQ(0, list.numRecordedForTests());
  EXPECT_EQ(1, sumRecorded(list));
}

} // namespace

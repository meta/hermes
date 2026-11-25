/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JSMapImpl.h"

#include "VMRuntimeTestHelpers.h"

using namespace hermes::vm;

namespace {

using OrderedHashMapTest = RuntimeTestFixture;

/// Test that iterator adjustment during rehash correctly handles the case
/// where the iterator is pointing at a deleted entry. This exercises the
/// updateIteratorIndicesForRehash code path.
///
/// The bug: When an entry is deleted during iteration (e.g. inside a forEach
/// callback), the iterator's index still points at the now-deleted slot. If
/// that deletion triggers a rehash (shrink or grow), the data table is
/// compacted. The iterator index must be adjusted to account for removed
/// deleted entries. Using std::lower_bound (>=) fails to count the deleted
/// entry at the iterator's own position, causing the adjusted index to be 1 too
/// high and skipping the next valid entry. Using std::upper_bound (>) correctly
/// counts it.

/// Constants derived from OrderedHashMapBase via JSSet (exposed under
/// UNIT_TEST). These are used to set up precise conditions for triggering
/// shrink and grow rehashes.
static constexpr uint32_t kInitialCapacity = JSSet::kInitialCapacity;
static constexpr uint32_t kGrowOrShrinkFactor = JSSet::kGrowOrShrinkFactor;
/// Capacity after one grow rehash.
static constexpr uint32_t kGrownCapacity =
    kInitialCapacity * kGrowOrShrinkFactor;
/// Rehash threshold at initial capacity: rehash triggers when
/// (keyCount + deleteCount) reaches this value.
static constexpr uint32_t kInitialRehashThreshold =
    JSSet::rehashThreshold(kInitialCapacity);
/// Shrink threshold at grown capacity: the largest keyCount for which
/// shouldShrink returns true.
static constexpr uint32_t kGrownShrinkThreshold = kGrownCapacity / 8;

/// Helper to create a JSSet, initialize its storage, and return a handle.
static Handle<JSSet> createSet(Runtime &runtime, MutableHandle<JSSet> out) {
  out = JSSet::create(runtime, runtime.setPrototype);
  auto res = JSSet::initializeStorage(out, runtime);
  (void)res;
  assert(res != ExecutionStatus::EXCEPTION);
  return out;
}

/// Helper to add a number to a JSSet.
static void setAdd(Handle<JSSet> set, Runtime &runtime, double value) {
  struct : public Locals {
    PinnedValue<> val;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.val = HermesValue::encodeTrustedNumberValue(value);
  auto res = JSSet::insert(set, runtime, lv.val);
  (void)res;
  assert(res != ExecutionStatus::EXCEPTION);
}

/// Helper to delete a number from a JSSet.
static bool setDelete(Handle<JSSet> set, Runtime &runtime, double value) {
  struct : public Locals {
    PinnedValue<> val;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.val = HermesValue::encodeTrustedNumberValue(value);
  return JSSet::erase(set, runtime, lv.val);
}

/// Test rehash triggered by erase (shouldShrink path).
///
/// Setup:
///   - Add (kInitialRehashThreshold + 1) entries to grow capacity to
///   kGrownCapacity.
///   - Delete (kInitialRehashThreshold - kGrownShrinkThreshold) entries outside
///     iteration, leaving (kGrownShrinkThreshold + 1) alive entries.
///
/// During iteration:
///   - Iterator advances to the first remaining entry.
///   - Delete that entry: size drops to kGrownShrinkThreshold.
///   - shouldShrink(kGrownCapacity, kGrownShrinkThreshold) = true -> rehash.
///   - Rehash compacts deleted entries out of the data table.
///   - The iterator was pointing at the just-deleted entry.
///
/// With the bug (lower_bound): the deleted entry at the iterator position is
///   not counted, adjusted index is 1 too high, skipping one entry.
/// With the fix (upper_bound): the deleted entry is counted, all remaining
///   entries are visited.
TEST_F(OrderedHashMapTest, IteratorAdjustmentOnShrinkRehash) {
  struct : public Locals {
    PinnedValue<JSSet> setSlot;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  auto set = createSet(runtime, lv.setSlot);

  // Add (kInitialRehashThreshold + 1) entries to trigger a grow rehash, growing
  // capacity from kInitialCapacity to kGrownCapacity.
  const uint32_t numInitialEntries = kInitialRehashThreshold + 1;
  for (uint32_t i = 0; i < numInitialEntries; ++i) {
    setAdd(set, runtime, i);
  }
  ASSERT_EQ(numInitialEntries, set->size());

  // Delete entries to leave (kGrownShrinkThreshold + 1) alive, so that one more
  // delete during iteration will trigger shouldShrink.
  const uint32_t numToDelete = numInitialEntries - (kGrownShrinkThreshold + 1);
  for (uint32_t i = 0; i < numToDelete; ++i) {
    ASSERT_TRUE(setDelete(set, runtime, i));
  }
  ASSERT_EQ(kGrownShrinkThreshold + 1, set->size());

  // Create an iterator and advance to the first remaining entry.
  auto iterCtx = set->newIterator(runtime);
  ASSERT_TRUE(set->advanceIterator(runtime, iterCtx));
  double firstEntry = set->iteratorKey(runtime, iterCtx).getNumber();
  EXPECT_EQ(static_cast<double>(numToDelete), firstEntry);

  // Delete the current entry — this triggers shouldShrink -> rehash.
  ASSERT_TRUE(setDelete(set, runtime, firstEntry));
  ASSERT_EQ(kGrownShrinkThreshold, set->size());

  // Advance the iterator through the remaining entries.
  // With the fix, all kGrownShrinkThreshold entries should be visited.
  std::vector<double> visited;
  while (set->advanceIterator(runtime, iterCtx)) {
    visited.push_back(set->iteratorKey(runtime, iterCtx).getNumber());
  }

  ASSERT_EQ(kGrownShrinkThreshold, (uint32_t)visited.size());
  for (uint32_t i = 0; i < kGrownShrinkThreshold; ++i) {
    EXPECT_EQ(static_cast<double>(numToDelete + 1 + i), visited[i]);
  }
}

/// Test rehash triggered by doInsert (shouldRehash path).
///
/// Setup:
///   - Add kInitialRehashThreshold entries at capacity kInitialCapacity. This
///   fills the table
///     right up to the rehash threshold without triggering a rehash (the check
///     happens before insert in doInsert).
///
/// During iteration:
///   - Iterator advances to entry 0 (at data index 0).
///   - Delete entry 0: size = kInitialRehashThreshold - 1, deletedCount = 1.
///     shouldShrink(kInitialCapacity, kInitialRehashThreshold - 1) = false (too
///     many alive).
///   - Iterator is now at data index 0 (a deleted entry).
///   - Insert a new entry: shouldRehash(kInitialCapacity,
///   kInitialRehashThreshold - 1, 1) =
///     (kInitialRehashThreshold >= kInitialRehashThreshold) = true -> grow
///     rehash.
///   - Rehash compacts to [1..kInitialRehashThreshold-1], then inserts the new
///   entry.
///
/// With the bug (lower_bound): adjusted = 0, next advance starts at 1,
///   skipping entry 1.
/// With the fix (upper_bound): isFirstIteration is set, next advance starts
///   at 0, visiting all entries.
TEST_F(OrderedHashMapTest, IteratorAdjustmentOnGrowRehash) {
  struct : public Locals {
    PinnedValue<JSSet> setSlot;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  auto set = createSet(runtime, lv.setSlot);

  // Add kInitialRehashThreshold entries at capacity kInitialCapacity.
  for (uint32_t i = 0; i < kInitialRehashThreshold; ++i) {
    setAdd(set, runtime, i);
  }
  ASSERT_EQ(kInitialRehashThreshold, set->size());

  // Create an iterator and advance to the first element (should be 0).
  auto iterCtx = set->newIterator(runtime);
  ASSERT_TRUE(set->advanceIterator(runtime, iterCtx));
  EXPECT_EQ(0.0, set->iteratorKey(runtime, iterCtx).getNumber());

  // Delete entry 0, then insert a new entry — triggers shouldRehash -> grow.
  const double newEntry = 100;
  ASSERT_TRUE(setDelete(set, runtime, 0));
  setAdd(set, runtime, newEntry);

  // Advance the iterator through remaining entries.
  // With the fix, should visit entries 1..(kInitialRehashThreshold-1) and
  // newEntry.
  std::vector<double> visited;
  while (set->advanceIterator(runtime, iterCtx)) {
    visited.push_back(set->iteratorKey(runtime, iterCtx).getNumber());
  }

  ASSERT_EQ(kInitialRehashThreshold, (uint32_t)visited.size());
  for (uint32_t i = 0; i < kInitialRehashThreshold - 1; ++i) {
    EXPECT_EQ(static_cast<double>(i + 1), visited[i]);
  }
  EXPECT_EQ(newEntry, visited[kInitialRehashThreshold - 1]);
}

/// Test the normal case: iterator is NOT at a deleted entry during rehash.
/// This verifies that the fix doesn't break the common case where the iterator
/// points to a valid (non-deleted) entry when rehash happens.
TEST_F(OrderedHashMapTest, IteratorAdjustmentNormalCase) {
  struct : public Locals {
    PinnedValue<JSSet> setSlot;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  auto set = createSet(runtime, lv.setSlot);

  // Add kInitialRehashThreshold entries at capacity kInitialCapacity.
  for (uint32_t i = 0; i < kInitialRehashThreshold; ++i) {
    setAdd(set, runtime, i);
  }

  // Create an iterator and advance past entry 0 to entry 1.
  auto iterCtx = set->newIterator(runtime);
  ASSERT_TRUE(set->advanceIterator(runtime, iterCtx));
  EXPECT_EQ(0.0, set->iteratorKey(runtime, iterCtx).getNumber());
  ASSERT_TRUE(set->advanceIterator(runtime, iterCtx));
  EXPECT_EQ(1.0, set->iteratorKey(runtime, iterCtx).getNumber());

  // Delete entry 0 (already visited, not the current entry) and insert a new
  // entry. This triggers shouldRehash -> grow rehash.
  // Iterator is at data index 1 (valid entry for value 1, not deleted).
  const double newEntry = 100;
  ASSERT_TRUE(setDelete(set, runtime, 0));
  setAdd(set, runtime, newEntry);

  // After rehash, should visit entries 2..(kInitialRehashThreshold-1) and
  // newEntry.
  std::vector<double> visited;
  while (set->advanceIterator(runtime, iterCtx)) {
    visited.push_back(set->iteratorKey(runtime, iterCtx).getNumber());
  }

  const uint32_t expectedCount = kInitialRehashThreshold - 1;
  ASSERT_EQ(expectedCount, (uint32_t)visited.size());
  for (uint32_t i = 0; i < expectedCount - 1; ++i) {
    EXPECT_EQ(static_cast<double>(i + 2), visited[i]);
  }
  EXPECT_EQ(newEntry, visited[expectedCount - 1]);
}

} // namespace

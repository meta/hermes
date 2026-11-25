/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/OrderedHashMap.h"
#include "hermes/VM/JSMapImpl.h"

#include "hermes/Support/ErrorHandling.h"
#include "hermes/VM/BuildMetadata.h"
#include "hermes/VM/GCPointer-inline.h"
#include "hermes/VM/Operations.h"

namespace hermes {
namespace vm {
//===----------------------------------------------------------------------===//
// class OrderedHashMapBase

template <typename BucketType, typename Derived>
void OrderedHashMapBase<BucketType, Derived>::IteratorContext::reset() {
  if (index_ != nullptr) {
    index_->free();
  }
  index_ = nullptr;
  storage_ = nullptr;
}

/// A ManagedChunkedList element that stores the iterator index that should be
/// accessed next.
template <typename BucketType, typename Derived>
class OrderedHashMapBase<BucketType, Derived>::IteratorIndex {
 public:
  /// \return the iterator index value
  uint32_t getValue() const {
    assert(!isFree() && "Value not present");
    return value_;
  }

  /// Sets the iterator index value
  void setValue(uint32_t value) {
    assert(!isFree() && "Value not present");
    value_ = value;
  }

  /// \return whether an initial iteration has been done using this
  /// IteratorIndex
  bool getIsFirstIteration() const {
    assert(!isFree() && "Value not present");
    return isFirstIteration_;
  }

  /// Sets the state indicating whether an initial iteration has been done
  /// using this IteratorIndex. This is needed because we want to skip
  /// advancing the index value one time after a clear or if it's newly
  /// created.
  void setIsFirstIteration(bool isFirstIteration) {
    assert(!isFree() && "Value not present");
    isFirstIteration_ = isFirstIteration;
    if (isFirstIteration)
      value_ = 0;
  }

  /// Set the element to free, this can be called from either the mutator
  /// (when destroying an iterator) or the background thread (in finalizer).
  void free() {
    /// There are three operations related to this atomic variable:
    /// 1. Freeing the element on background thread.
    /// 2. Checking if the element is free on the mutator.
    /// 3. Freeing and reusing the element on the mutator.
    /// Since the only operation that background thread can do with the
    /// element is freeing it, we don't need a barrier to order the store.
    free_.store(true, std::memory_order_relaxed);
  }

  // Methods required by ManagedChunkedList.

  IteratorIndex() = default;

  /// \return true if the element has been freed. As noted in free(), since
  /// background thread can only free a element, we don't need a stricter
  /// order.
  bool isFree() const {
    return free_.load(std::memory_order_relaxed);
  }

  /// Get the next free element. Must not be called when this instance is
  /// occupied with a value.
  IteratorIndex *getNextFree() {
    assert(isFree() && "Can only get nextFree on a free element");
    return nextFree_;
  }

  /// Set the next free element. Must not be called when this instance is
  /// occupied with a value.
  void setNextFree(IteratorIndex *nextFree) {
    assert(isFree() && "Can only set nextFree on a free element");
    nextFree_ = nextFree;
  }

  /// Emplace new value to this element.
  void emplace() {
    assert(isFree() && "Emplacing already occupied element");
    free_.store(false, std::memory_order_relaxed);
    setIsFirstIteration(true);
  }

 private:
  union {
    /// Stores the index to the data table.
    uint32_t value_;
    IteratorIndex *nextFree_;
  };
  /// Indicates whether an iteration has been performed using this
  /// IteratorIndex. This is used for newly created IteratorIndex and any
  /// active IteratorIndex when a clear operation is performed on the hash
  /// table. Used by the advanceIterator function to know whether to update
  /// the index value.
  bool isFirstIteration_;
  /// Atomic state represents whether the element is free. It can be written
  /// by background thread (in finalizer) and read/written by mutator.
  std::atomic<bool> free_{true};
};

template <typename BucketType, typename Derived>
void OrderedHashMapBase<BucketType, Derived>::buildMetadata(
    const GCCell *cell,
    Metadata::Builder &mb) {
  const auto *self = static_cast<const Derived *>(cell);
  mb.addField("dataTable", &self->dataTable_);
}

/// Allocate the internal element storage.
template <typename BucketType, typename Derived>
ExecutionStatus OrderedHashMapBase<BucketType, Derived>::initializeStorage(
    Handle<Derived> self,
    Runtime &runtime) {
  self->hashTable_.resize(kInitialCapacity, kHashTableElementUnused);
  runtime.getHeap().creditExternalMemory(
      *self, kInitialCapacity * sizeof(uint32_t));

  // Create data table with corresponding capacity, but start off with 0 size.
  // Entries will be appended as the hash table receives elements.
  auto dataTableRes = StorageType::create(
      runtime, dataTableAllocationSize(kInitialCapacity), 0 /* size */);
  if (LLVM_UNLIKELY(dataTableRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  auto *dataTable = vmcast<StorageType>(*dataTableRes);
  self->dataTable_.setNonNull(runtime, dataTable, runtime.getHeap());

  // Create iterator metadata storage
  self->iteratorIndices_ = std::make_shared<ManagedChunkedList<IteratorIndex>>(
      0.5 /* occupancyRatio */, 0.5 /* sizingWeight */);

  return ExecutionStatus::RETURNED;
}

template <typename BucketType, typename Derived>
std::pair<OptValue<uint32_t>, uint32_t>
OrderedHashMapBase<BucketType, Derived>::lookupInBucket(
    Runtime &runtime,
    uint32_t bucket,
    HermesValue key) const {
  assert(hashTable_.size() == capacity_ && "Inconsistent capacity");
  [[maybe_unused]] const uint32_t firstBucket = bucket;

  NoAllocScope noAlloc{runtime};

  // We can deref the handles because we are in NoAllocScope.
  StorageType *dataTable = dataTable_.getNonNull(runtime);
  const uint32_t mask = capacity_ - 1;

  // Each bucket must be either kHashTableElementUnused,
  // kHashTableElementDeleted, or an index to the data table.
  uint32_t dataTableKeyIndex;
  assert(bucket < capacity_ && "Hash table index >= capacity.");
  while ((dataTableKeyIndex = hashTable_[bucket]) != kHashTableElementUnused) {
    if (dataTableKeyIndex != kHashTableElementDeleted) {
      auto keySHV = dataTable->at(dataTableKeyIndex);
      if (isSameValueZero(keySHV.unboxToHV(runtime), key)) {
        return {dataTableKeyIndex, bucket};
      }
    }

    bucket = (bucket + 1) & mask;
    assert(bucket != firstBucket && "Hash table has wrapped around");
  }

  return {llvh::None, bucket};
}

template <typename BucketType, typename Derived>
ExecutionStatus OrderedHashMapBase<BucketType, Derived>::rehash(
    Handle<Derived> self,
    Runtime &runtime,
    bool beforeAdd) {
  // A rehash rebuilds the table, relocating every entry and invalidating any
  // cached bucket index.
  self->cachedBucketInvalidated_ = true;
  struct : public Locals {
    PinnedValue<StorageType> newDataTable;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  const CallResult<uint32_t> newCapacity = checkedNextCapacity(
      runtime, self->capacity_, self->size_ + (beforeAdd ? 1 : 0));
  if (LLVM_UNLIKELY(newCapacity == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // Create a new hash table.
  std::vector<uint32_t> newHashTable;
  newHashTable.resize(*newCapacity, kHashTableElementUnused);

  // Create a new data table.
  auto dataTableRes = StorageType::create(
      runtime,
      dataTableAllocationSize(*newCapacity),
      self->size_ * BucketType::kElementsPerEntry);
  if (LLVM_UNLIKELY(dataTableRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.newDataTable.template castAndSetHermesValue<StorageType>(*dataTableRes);

  // Publish the new capacity so the re-add loop below hashes into the new
  // table. This happens only after the fallible allocation above succeeds; an
  // early return on OOM must leave capacity_ consistent with the still-current
  // hashTable_.
  self->capacity_ = *newCapacity;

  // Now re-add all entries to the hash table.
  uint32_t totalEntries = self->size_ + self->deletedCount_;
  uint32_t newDataTableKeyIndex = 0;
  uint32_t oldDataTableKeyIndex = 0;

  // For each of the deleted entry, we store the entry index to help iterators
  // adjust their entry index in the next step.
  // Suppose you have the following data table (kElementsPerEntry = 1), and 'x'
  // marks the deleted entries.
  // entry index: 0 1 2 3 4 5 6 7 8
  //            : 0 x 2 x 4 x 6 x 8
  // We'll record the deleted indices as: [1 3 5 7]
  std::vector<uint32_t> deletedEntryIndices;
  deletedEntryIndices.reserve(self->deletedCount_);

  const uint32_t mask = self->capacity_ - 1;
  for (uint32_t i = 0; i < totalEntries; ++i) {
    auto keySHV =
        self->dataTable_.getNonNull(runtime)->at(oldDataTableKeyIndex);
    if (!keySHV.isEmpty()) {
      uint32_t bucket =
          hashToBucket(self->capacity_, runtime, keySHV.unboxToHV(runtime));
      [[maybe_unused]] const uint32_t firstBucket = bucket;
      assert(bucket < *newCapacity && "Hash table index >= new capacity.");
      while (newHashTable[bucket] != kHashTableElementUnused) {
        // Find another bucket if it is already used.
        bucket = (bucket + 1) & mask;
        assert(firstBucket != bucket && "Hash table is full!");
        assert(bucket < *newCapacity && "Hash table index >= new capacity.");
      }
      // Set the new entry to the bucket.
      lv.newDataTable->set(newDataTableKeyIndex, keySHV, runtime.getHeap());
      if constexpr (std::is_same_v<BucketType, HashMapEntry>) {
        auto valueSHV =
            self->dataTable_.getNonNull(runtime)->at(oldDataTableKeyIndex + 1);
        lv.newDataTable->set(
            newDataTableKeyIndex + 1, valueSHV, runtime.getHeap());
      }
      assert(bucket < *newCapacity && "Hash table index >= new capacity.");
      newHashTable[bucket] = newDataTableKeyIndex;

      newDataTableKeyIndex += BucketType::kElementsPerEntry;
    } else {
      // Entry is deleted.
      deletedEntryIndices.push_back(i);
    }
    oldDataTableKeyIndex += BucketType::kElementsPerEntry;
  }

  NoAllocScope noAlloc{runtime};

  OrderedHashMapBase<BucketType, Derived> *rawSelf = *self;

  // Adjust any active iterators to handle the fact that we removed deleted
  // entries in the new data table.
  rawSelf->updateIteratorIndicesForRehash(deletedEntryIndices);

  rawSelf->deletedCount_ = 0;
  // The external memory credit tracks hashTable_.size() * sizeof(uint32_t).
  // The new table may be larger or smaller than the old one, so settle the
  // difference in whichever direction it went.
  const uint32_t oldSizeInBytes = rawSelf->hashTable_.size() * sizeof(uint32_t);
  const uint32_t newSizeInBytes = newHashTable.size() * sizeof(uint32_t);
  rawSelf->hashTable_ = std::move(newHashTable);
  if (newSizeInBytes >= oldSizeInBytes) {
    runtime.getHeap().creditExternalMemory(
        *self, newSizeInBytes - oldSizeInBytes);
  } else {
    runtime.getHeap().debitExternalMemory(
        *self, oldSizeInBytes - newSizeInBytes);
  }
  rawSelf->dataTable_.setNonNull(runtime, *lv.newDataTable, runtime.getHeap());
  return ExecutionStatus::RETURNED;
}

template <typename BucketType, typename Derived>
void OrderedHashMapBase<BucketType, Derived>::updateIteratorIndicesForRehash(
    llvh::ArrayRef<uint32_t> deletedEntryIndices) {
  assert(
      deletedEntryIndices.size() == deletedCount_ &&
      "Should encounter same number of deleted elements as the deletedCount_");

  // Update all iterators to adjust their indices for the rehash. We take the
  // IteratorIndex's value and reduce it by the number of deleted elements that
  // occur before that index.
  //
  // For example, if we have the following data table (kElementsPerEntry = 1):
  // entry index: 0 1 2 3 4 5 6 7 8
  // values     : 0 x 2 x 4 x 6 x 8
  // And the iterator has already navigated and printed the #4 entry, then the
  // IteratorIndex value will be 5. Since there are 2 deleted entries already
  // traversed (entry 1 and entry 3), the IteratorIndex value should become 3
  // after the rehash (representing that we already navigated. #0, #2, and #4
  // entries).
  //
  // In the new data table, the values are as follows:
  // entry index: 0 1 2 3 4
  // values     : 0 2 4 6 8
  // Therefore, IteratorIndex value = 3 will allow the iterator to print the
  // next value, which should be 6.
  iteratorIndices_->forEach([&deletedEntryIndices](IteratorIndex &index) {
    // Perform the std::upper_bound algorithm to do a binary search to find
    // the first deleted entry index > IteratorIndex value. Using the same
    // example as above, the \p deletedEntryIndices would be [1 3 5 7].
    // So for IteratorIndex value = 5, then \p firstGreaterIter will
    // be calculated to be the iterator pointing at index 3 of \p
    // deletedEntryIndices (value 7). We then know the number of deleted
    // entry indices that appeared at or before (entry 1, entry 3, and
    // entry 5).
    //
    // Note that we use upper_bound (>) instead of lower_bound (>=) because the
    // iterator's current element could itself be deleted (e.g., 5 in the
    // example, which may happen when the current entry is deleted during a
    // forEach callback). In that case, the deleted entry at the iterator's
    // position must also be counted so that the adjusted index is correct.
    //
    // If, however, IteratorIndex value = 8 and there aren't any deleted
    // entry index that is > 8, then \p firstGreaterIter will be
    // deletedEntryIndices.end().
    const uint32_t iteratorIndex = index.getValue();
    auto firstGreaterIter = std::upper_bound(
        deletedEntryIndices.begin(), deletedEntryIndices.end(), iteratorIndex);

    uint32_t numDeletedAtOrBeforeIndex =
        std::distance(deletedEntryIndices.begin(), firstGreaterIter);
    // If all entries up to and including the iterator's position were deleted,
    // set the index to 0 and set the isFirstIteration flag to true, so that
    // next iteration will start from 0. This is to avoid the underflow (though
    // it's well defined behavior), so index always remains valid.
    if (numDeletedAtOrBeforeIndex == iteratorIndex + 1) {
      index.setValue(0);
      index.setIsFirstIteration(true);
    } else {
      index.setValue(iteratorIndex - numDeletedAtOrBeforeIndex);
    }
  });
}

template <typename BucketType, typename Derived>
bool OrderedHashMapBase<BucketType, Derived>::has(
    Runtime &runtime,
    HermesValue key) const {
  NoAllocScope noAlloc{runtime};
  assertInitialized();
  auto bucket = hashToBucket(capacity_, runtime, key);
  return lookupInBucket(runtime, bucket, key).first.hasValue();
}

template <typename BucketType, typename Derived>
SmallHermesValue OrderedHashMapBase<BucketType, Derived>::get(
    Runtime &runtime,
    HermesValue key) const {
  NoAllocScope noAlloc{runtime};
  assertInitialized();
  auto bucket = hashToBucket(capacity_, runtime, key);
  OptValue<uint32_t> dataTableKeyIndex =
      lookupInBucket(runtime, bucket, key).first;
  if (!dataTableKeyIndex.hasValue()) {
    return SmallHermesValue::encodeUndefinedValue();
  }
  return dataTable_.getNonNull(runtime)->at(*dataTableKeyIndex + 1);
}

template <typename BucketType, typename Derived>
template <typename>
ExecutionStatus OrderedHashMapBase<BucketType, Derived>::insert(
    Handle<Derived> self,
    Runtime &runtime,
    Handle<> key,
    Handle<> value) {
  self->assertInitialized();
  uint32_t bucket = hashToBucket(self->capacity_, runtime, *key);

  // Find the bucket for this key. If the entry already exists, update the value
  // and return.
  {
    OptValue<uint32_t> dataTableKeyIndex;
    uint32_t newBucket;
    std::tie(dataTableKeyIndex, newBucket) =
        self->lookupInBucket(runtime, bucket, key.getHermesValue());
    bucket = newBucket;
    if (dataTableKeyIndex.hasValue()) {
      // Element for the key already exists, update value and return.
      auto valueSHV =
          SmallHermesValue::encodeHermesValue(value.getHermesValue(), runtime);
      self->dataTable_.getNonNull(runtime)->set(
          *dataTableKeyIndex + 1, valueSHV, runtime.getHeap());
      return ExecutionStatus::RETURNED;
    }
  }

  return doInsert(self, runtime, bucket, key, value);
}

template <typename BucketType, typename Derived>
template <typename>
CallResult<HermesValue> OrderedHashMapBase<BucketType, Derived>::getOrInsert(
    Handle<Derived> self,
    Runtime &runtime,
    Handle<> key,
    Handle<> value) {
  self->assertInitialized();
  uint32_t bucket = hashToBucket(self->capacity_, runtime, *key);
  {
    NoAllocScope noAlloc{runtime};
    OptValue<uint32_t> dataTableKeyIndex;
    std::tie(dataTableKeyIndex, bucket) =
        self->lookupInBucket(runtime, bucket, key.getHermesValue());
    if (dataTableKeyIndex.hasValue()) {
      // Element for the key already exists; return its value without
      // overwriting.
      SmallHermesValue existing =
          self->dataTable_.getNonNull(runtime)->at(*dataTableKeyIndex + 1);
      return existing.unboxToHV(runtime);
    }
  }

  // Key is absent; insert at the bucket we just resolved and return the value.
  if (LLVM_UNLIKELY(
          doInsert(self, runtime, bucket, key, value) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return value.getHermesValue();
}

template <typename BucketType, typename Derived>
template <typename>
CallResult<HermesValue>
OrderedHashMapBase<BucketType, Derived>::getOrInsertComputed(
    Handle<Derived> self,
    Runtime &runtime,
    Handle<> key,
    Handle<Callable> callback) {
  self->assertInitialized();
  uint32_t bucket = hashToBucket(self->capacity_, runtime, *key);
  {
    NoAllocScope noAlloc{runtime};
    OptValue<uint32_t> dataTableKeyIndex;
    std::tie(dataTableKeyIndex, bucket) =
        self->lookupInBucket(runtime, bucket, key.getHermesValue());
    if (dataTableKeyIndex.hasValue()) {
      // Element for the key already exists; return its value without
      // overwriting.
      SmallHermesValue existing =
          self->dataTable_.getNonNull(runtime)->at(*dataTableKeyIndex + 1);
      return existing.unboxToHV(runtime);
    }
  }

  // Clear the flag so we can detect whether the callback structurally modifies
  // the map; save its prior value to restore on the exception path.
  bool savedInvalidated = self->cachedBucketInvalidated_;
  self->cachedBucketInvalidated_ = false;

  struct : public Locals {
    PinnedValue<> value;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  CallResult<PseudoHandle<>> callRes = Callable::executeCall1(
      callback, runtime, Runtime::getUndefinedValue(), key.getHermesValue());
  if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
    // The callback threw, so this call inserts nothing. The flag must reflect
    // whether a cached bucket held by an enclosing call is now stale, which is
    // the case if the map was modified before this call (savedInvalidated) or
    // if the callback modified it before throwing (e.g. a rehash triggered by a
    // nested getOrInsertComputed). Combine both.
    self->cachedBucketInvalidated_ =
        savedInvalidated || self->cachedBucketInvalidated_;
    return ExecutionStatus::EXCEPTION;
  }
  lv.value = std::move(*callRes);

  // Fast path: the bucket above is still valid and can be used for insertion.
  if (LLVM_LIKELY(!self->cachedBucketInvalidated_)) {
    if (LLVM_UNLIKELY(
            doInsert(self, runtime, bucket, key, lv.value) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return lv.value.getHermesValue();
  }
  // The callback modified the map, so do an insert from scratch.
  if (LLVM_UNLIKELY(
          insert(self, runtime, key, lv.value) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return lv.value.getHermesValue();
}

template <typename BucketType, typename Derived>
template <typename>
ExecutionStatus OrderedHashMapBase<BucketType, Derived>::insert(
    Handle<Derived> self,
    Runtime &runtime,
    Handle<> key) {
  self->assertInitialized();
  uint32_t bucket = hashToBucket(self->capacity_, runtime, *key);

  // Find the bucket for this key. If the entry already exists, then return.
  {
    OptValue<uint32_t> dataTableKeyIndex;
    std::tie(dataTableKeyIndex, bucket) =
        self->lookupInBucket(runtime, bucket, key.getHermesValue());
    if (dataTableKeyIndex.hasValue()) {
      return ExecutionStatus::RETURNED;
    }
  }

  return doInsert(
      self, runtime, bucket, key, HandleRootOwner::getUndefinedValue());
}

template <typename BucketType, typename Derived>
ExecutionStatus OrderedHashMapBase<BucketType, Derived>::doInsert(
    Handle<Derived> self,
    Runtime &runtime,
    uint32_t bucket,
    Handle<> key,
    Handle<> value) {
  // Appending an entry can invalidate a cached bucket index.
  self->cachedBucketInvalidated_ = true;
  struct : public Locals {
    PinnedValue<StorageType> dataTable;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Run rehash if necessary before inserting.
  if (shouldRehash(self->capacity_, self->size_, self->deletedCount_)) {
    if (LLVM_UNLIKELY(
            rehash(self, runtime, true) == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // Find a new empty bucket after rehash.
    bucket = hashToBucket(self->capacity_, runtime, *key);
    OptValue<uint32_t> dataTableKeyIndex;
    std::tie(dataTableKeyIndex, bucket) =
        self->lookupInBucket(runtime, bucket, key.getHermesValue());
    assert(
        !dataTableKeyIndex.hasValue() &&
        "After rehash, we must be able to find an empty bucket");
  }

  // Store the data table index in \p hashTable_ so we can go from hash table to
  // the actual data
  assert(bucket < self->capacity_ && "Hash table index >= capacity.");
  self->hashTable_[bucket] = self->dataTable_.getNonNull(runtime)->size();

  lv.dataTable = self->dataTable_.getNonNull(runtime);
  assert(
      lv.dataTable->size() < lv.dataTable->capacity() &&
      "Data table should always have enough capacity");
  auto k = SmallHermesValue::encodeHermesValue(key.getHermesValue(), runtime);
  lv.dataTable->pushWithinCapacity(runtime, k);
  if constexpr (std::is_same_v<BucketType, HashMapEntry>) {
    auto v =
        SmallHermesValue::encodeHermesValue(value.getHermesValue(), runtime);
    lv.dataTable->pushWithinCapacity(runtime, v);
  }

  self->size_++;
  return ExecutionStatus::RETURNED;
}

template <typename BucketType, typename Derived>
bool OrderedHashMapBase<BucketType, Derived>::erase(
    Handle<Derived> self,
    Runtime &runtime,
    Handle<> key) {
  self->assertInitialized();
  uint32_t bucket = hashToBucket(self->capacity_, runtime, *key);
  OptValue<uint32_t> dataTableKeyIndex;
  std::tie(dataTableKeyIndex, bucket) =
      self->lookupInBucket(runtime, bucket, key.getHermesValue());
  if (!dataTableKeyIndex.hasValue()) {
    // Element does not exist.
    return false;
  }

  // Mark the bucket as deleted. We remove this in rehash.
  deleteBucket(self, runtime, bucket, *dataTableKeyIndex);
  self->deletedCount_++;
  self->size_--;
  // A deleted entry (and any shrink-rehash below) can invalidate a cached
  // bucket index.
  self->cachedBucketInvalidated_ = true;

  if (shouldShrink(self->capacity_, self->size_))
    rehash(self, runtime);

  return true;
}

template <typename BucketType, typename Derived>
typename OrderedHashMapBase<BucketType, Derived>::IteratorContext
OrderedHashMapBase<BucketType, Derived>::newIterator(Runtime &runtime) {
  return IteratorContext(iteratorIndices_->add(), iteratorIndices_);
}

template <typename BucketType, typename Derived>
bool OrderedHashMapBase<BucketType, Derived>::advanceIterator(
    Runtime &runtime,
    IteratorContext &iterCtx) {
  uint32_t i = 0;

  assert(iterCtx.initialized() && "Invalid IteratorContext");
  IteratorIndex *index = iterCtx.index_;

  // Check if the iterator was newly created or was reset due to clearing the
  // OrderedHashMap. In both cases, we won't advance the index this time around
  // since the index is already at 0.
  if (!index->getIsFirstIteration()) {
    // Advance to the next entry.
    i = index->getValue() + 1;
  } else {
    index->setIsFirstIteration(false);
  }

  // The iterator index might be pointing to a deleted element, so need to
  // iterate and find the next one that isn't deleted.
  for (; i < size_ + deletedCount_; ++i) {
    if (!dataTable_.getNonNull(runtime)
             ->at(i * BucketType::kElementsPerEntry)
             .isEmpty()) {
      // Found valid element
      index->setValue(i);
      return true;
    }
  }

  return false;
}

template <typename BucketType, typename Derived>
HermesValue OrderedHashMapBase<BucketType, Derived>::iteratorKey(
    Runtime &runtime,
    const IteratorContext &iterCtx) const {
  assert(iterCtx.initialized() && "Invalid IteratorContext");
  auto shv = dataTable_.getNonNull(runtime)->at(
      iterCtx.index_->getValue() * BucketType::kElementsPerEntry);
  assert(!shv.isEmpty() && "Invalid key encountered");
  return shv.unboxToHV(runtime);
}

template <typename BucketType, typename Derived>
HermesValue OrderedHashMapBase<BucketType, Derived>::iteratorValue(
    Runtime &runtime,
    const IteratorContext &iterCtx) const {
  assert(iterCtx.initialized() && "Invalid IteratorContext");
  if constexpr (std::is_same_v<BucketType, HashMapEntry>) {
    auto shv = dataTable_.getNonNull(runtime)->at(
        iterCtx.index_->getValue() * BucketType::kElementsPerEntry + 1);
    assert(!shv.isEmpty() && "Invalid value encountered");
    return shv.unboxToHV(runtime);
  } else {
    auto shv = dataTable_.getNonNull(runtime)->at(
        iterCtx.index_->getValue() * BucketType::kElementsPerEntry);
    assert(!shv.isEmpty() && "Invalid key encountered");
    return shv.unboxToHV(runtime);
  }
}

template <typename BucketType, typename Derived>
ExecutionStatus OrderedHashMapBase<BucketType, Derived>::clear(
    Handle<Derived> self,
    Runtime &runtime) {
  self->assertInitialized();
  if (self->size_ == 0 && self->deletedCount_ == 0) {
    // Empty set: nothing to do, so the table layout is unchanged.
    return ExecutionStatus::RETURNED;
  }

  // Clearing rebuilds the table and invalidates any cached bucket index.
  self->cachedBucketInvalidated_ = true;

  // Clear the hash table. The external memory credit tracks
  // hashTable_.size() * sizeof(uint32_t), so debit the difference between the
  // discarded table and the reinitialized one.
  const uint32_t oldSizeInBytes = self->hashTable_.size() * sizeof(uint32_t);
  self->hashTable_ = std::vector<uint32_t>();
  // Resize the hash table to the initial size.
  self->hashTable_.resize(kInitialCapacity, kHashTableElementUnused);
  self->capacity_ = kInitialCapacity;
  constexpr uint32_t newSizeInBytes = kInitialCapacity * sizeof(uint32_t);
  assert(
      oldSizeInBytes >= newSizeInBytes &&
      "Capacity never shrinks below kInitialCapacity");
  runtime.getHeap().debitExternalMemory(*self, oldSizeInBytes - newSizeInBytes);

  // Resize the data table back to 0.
  self->dataTable_.getNonNull(runtime)->clear(runtime);

  // Update all iterators back to index 0.
  self->iteratorIndices_->forEach(
      [](IteratorIndex &index) { index.setIsFirstIteration(true); });

  self->deletedCount_ = 0;
  self->size_ = 0;
  return ExecutionStatus::RETURNED;
}

template class OrderedHashMapBase<HashMapEntry, JSMapImpl<CellKind::JSMapKind>>;
template class OrderedHashMapBase<HashSetEntry, JSMapImpl<CellKind::JSSetKind>>;

template ExecutionStatus
OrderedHashMapBase<HashMapEntry, JSMapImpl<CellKind::JSMapKind>>::insert(
    Handle<JSMapImpl<CellKind::JSMapKind>> self,
    Runtime &runtime,
    Handle<> key,
    Handle<> value);
template CallResult<HermesValue>
OrderedHashMapBase<HashMapEntry, JSMapImpl<CellKind::JSMapKind>>::getOrInsert(
    Handle<JSMapImpl<CellKind::JSMapKind>> self,
    Runtime &runtime,
    Handle<> key,
    Handle<> value);
template CallResult<HermesValue>
OrderedHashMapBase<HashMapEntry, JSMapImpl<CellKind::JSMapKind>>::
    getOrInsertComputed(
        Handle<JSMapImpl<CellKind::JSMapKind>> self,
        Runtime &runtime,
        Handle<> key,
        Handle<Callable> callback);
template ExecutionStatus
OrderedHashMapBase<HashSetEntry, JSMapImpl<CellKind::JSSetKind>>::insert(
    Handle<JSMapImpl<CellKind::JSSetKind>> self,
    Runtime &runtime,
    Handle<> key);

} // namespace vm
} // namespace hermes

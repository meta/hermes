/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_ARRAYSTORAGE_H
#define HERMES_VM_ARRAYSTORAGE_H

#include "hermes/VM/AlignedHeapSegment.h"
#include "hermes/VM/HermesValue-inline.h"
#include "hermes/VM/Metadata.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/SmallHermesValue-inline.h"

#include "llvh/Support/TrailingObjects.h"

namespace hermes {
namespace vm {

/// A GC-managed resizable vector of values. It is used for storage of property
/// values in objects and also indexed property values in arrays. It supports
/// resizing on both ends which is necessary for the simplest implementation of
/// JavaScript arrays (using a base offset and length).
template <typename HVType>
class ArrayStorageBase final : public VariableSizeRuntimeCell,
                               private llvh::TrailingObjects<
                                   ArrayStorageBase<HVType>,
                                   GCHermesValueInLargeObjImpl<HVType>> {
  using GCHVType = GCHermesValueInLargeObjImpl<HVType>;

 public:
  using size_type = uint32_t;
  using iterator = GCHVType *;

 private:
  AtomicIfConcurrentGC<size_type> size_{0};

  friend llvh::TrailingObjects<ArrayStorageBase<HVType>, GCHVType>;
  friend void ArrayStorageBuildMeta(const GCCell *cell, Metadata::Builder &mb);
  friend void ArrayStorageSmallBuildMeta(
      const GCCell *cell,
      Metadata::Builder &mb);

 public:
  static const VTable vt;

  /// Gets the amount of memory used by this object for a given \p capacity.
  static constexpr size_t allocationSize(size_type capacity) {
    return ArrayStorageBase::template totalSizeToAlloc<GCHVType>(capacity);
  }

  /// \return The the maximum number of elements that will fit in an
  /// ArrayStorage with allocated size \p allocSize.
  static constexpr size_type capacityForAllocationSize(uint32_t allocSize) {
    assert(
        allocSize >= allocationSize(0) &&
        "allocSize must be at least the size of ArrayStorage itself");
    return (allocSize - allocationSize(0)) / sizeof(HVType);
  }

  static bool classof(const GCCell *cell) {
    return cell->getKind() == getCellKind();
  }

  static constexpr CellKind getCellKind() {
    static_assert(
        std::is_same<HVType, HermesValue>::value ||
            std::is_same<HVType, SmallHermesValue>::value,
        "Illegal HVType.");
    return std::is_same<HVType, HermesValue>::value
        ? CellKind::ArrayStorageKind
        : CellKind::ArrayStorageSmallKind;
  }

  /// Return the maximum capacity that ensures no overflow when computing
  /// allocation size from it. In this class, we use `size_type`, which is
  /// uint32_t, to represent capacity and size, since each element occupies more
  /// than one byte, the allocation size for a large capacity may overflow. We
  /// should always check against this value when creating ArrayStorage with a
  /// passed in capacity.
  static constexpr size_type maxCapacityNoOverflow() {
    return capacityForAllocationSize(std::numeric_limits<size_type>::max());
  }

  /// Return allocation size for \p capacity with overflow check.
  static CallResult<size_type> checkedAllocationSize(
      Runtime &runtime,
      size_type capacity) {
    if (capacity > maxCapacityNoOverflow())
      return throwAllocationFailure(runtime, capacity);
    return allocationSize(capacity);
  }

  /// Create a new instance with at least the specified \p capacity.
  static CallResult<HermesValue> create(Runtime &runtime, size_type capacity) {
    auto allocSizeRes = checkedAllocationSize(runtime, capacity);
    if (LLVM_UNLIKELY(allocSizeRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    auto *cell = runtime.makeAVariable<
        ArrayStorageBase<HVType>,
        HasFinalizer::No,
        LongLived::No,
        CanBeLarge::Yes,
        MayFail::Yes>(*allocSizeRes);
    if (LLVM_UNLIKELY(!cell))
      return throwAllocationFailure(runtime, capacity);
    return HermesValue::encodeObjectValue(cell);
  }

#ifdef UNIT_TEST
  /// Make it possible to construct an ArrayStorage with just a GC* that is
  /// immediately resized to its capacity. This is used only in tests, where we
  /// have a GC* but not a Runtime*.
  static ArrayStorageBase *createForTest(GC &gc, size_type capacity) {
    const auto allocSize = allocationSize(capacity);
    auto *cell = gc.makeAVariable<ArrayStorageBase>(allocSize);
    ArrayStorageBase::resizeWithinCapacity(cell, gc, capacity);
    return cell;
  }
#endif

  /// Create a new long-lived instance with at least the specified \p capacity.
  static CallResult<HermesValue> createLongLived(
      Runtime &runtime,
      size_type capacity) {
    auto allocSizeRes = checkedAllocationSize(runtime, capacity);
    if (LLVM_UNLIKELY(allocSizeRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    auto *ptr = runtime.makeAVariable<
        ArrayStorageBase<HVType>,
        HasFinalizer::No,
        LongLived::Yes,
        CanBeLarge::Yes,
        MayFail::Yes>(*allocSizeRes);
    if (LLVM_UNLIKELY(!ptr)) {
      return throwAllocationFailure(runtime, capacity);
    }
    return HermesValue::encodeObjectValue(ptr);
  }

  /// Create a new instance with at least the specified \p capacity and a size
  /// of \p size. Requires that \p size <= \p capacity.
  static CallResult<HermesValue>
  create(Runtime &runtime, size_type capacity, size_type size) {
    auto arrRes = create(runtime, capacity);
    if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    growWithinCapacity(
        vmcast<ArrayStorageBase<HVType>>(*arrRes), runtime, size);
    return arrRes;
  }

  /// \return a pointer to the underlying data storage.
  GCHVType *data() {
    return ArrayStorageBase<HVType>::template getTrailingObjects<GCHVType>();
  }
  const GCHVType *data() const {
    return ArrayStorageBase<HVType>::template getTrailingObjects<GCHVType>();
  }

  /// \return the element at index \p index
  HVType at(size_type index) const {
    assert(index < size() && "index out of range");
    return data()[index];
  }

  /// \return the element at index \p index
  void set(size_type index, HVType val, GC &gc) {
    assert(index < size() && "index out of range");
    data()[index].set(val, this, gc);
  }

  /// \return the element at index \p index
  void setNonPtr(size_type index, HVType val, GC &gc) {
    assert(index < size() && "index out of range");
    data()[index].setNonPtr(val, gc);
  }

  /// \return The capacity of the current storage. It must be smaller than the
  /// value of maxCapacityNoOverflow().
  size_type capacity() const {
    return capacityForAllocationSize(getAllocatedSizeSlow());
  }
  size_type size() const {
    return size_.load(std::memory_order_relaxed);
  }

  iterator begin() {
    return data();
  }
  iterator end() {
    return data() + size();
  }

  /// Append the given element to the end, where we know that there is no need
  /// to allocate additional capacity. This is more efficient than push_back,
  /// since the caller does not need to pin the ArrayStorage,
  /// check for exceptions, or write back the resulting pointer in case it
  /// moves.
  void pushWithinCapacity(Runtime &runtime, HVType value) {
    auto sz = size();
    assert(sz < capacity());
    // Use the constructor of GCHermesValue to use the correct write barrier
    // for uninitialized memory.
    new (&data()[sz]) GCHVType(value, this, runtime.getHeap());
    size_.store(sz + 1, std::memory_order_release);
  }

  /// Append the given element to the end (increasing size by 1).
  ///
  /// \param[in,out] selfHandle The ArrayStorageBase to be modified. Note the
  /// MutableHandle will be updated to point to a new allocated ArrayStorageBase
  /// if allocation is required. Caller needs to take the updated handle value
  /// after the call to update their own pointers.
  /// \param runtime The Runtime.
  /// \param value The value to append to the ArrayStorage. If this is a
  /// ArrayStorageSmall, the value will be encoded into a SmallHermesValue.
  static ExecutionStatus push_back(
      MutableHandle<ArrayStorageBase<HVType>> selfHandle,
      Runtime &runtime,
      Handle<> value) {
    // This must be done before the capacity check, because encodeHermesValue
    // may allocate, which could cause trimming of the ArrayStorage. If the
    // capacity check fails, then this work is wasted, but that's okay because
    // it's a slow path.
    auto hv = HVType::encodeHermesValue(value.get(), runtime);
    auto *self = selfHandle.get();
    if (LLVM_LIKELY(self->size() < self->capacity())) {
      // Use the constructor of GCHermesValue to use the correct write barrier
      // for uninitialized memory.
      self->pushWithinCapacity(runtime, hv);
      return ExecutionStatus::RETURNED;
    }
    return pushBackSlowPath(selfHandle, runtime, value);
  }

  /// Pop the last element off the array and return it.
  HVType pop_back(Runtime &runtime) {
    const size_type sz = size();
    assert(sz > 0 && "Can't pop from empty ArrayStorage");
    HVType val = data()[sz - 1];
    // In Hades, a snapshot write barrier must be executed on the value that is
    // conceptually being changed to null. The write doesn't need to occur, but
    // it is the only correct way to use the write barrier.
    data()[sz - 1].unreachableWriteBarrier(runtime.getHeap());
    // The background thread can't mutate size, so we don't need fetch_sub here.
    // Relaxed is fine, because the GC doesn't care about the order of seeing
    // the length and the individual elements, as long as illegal HermesValues
    // aren't written there (which they won't be).
    size_.store(sz - 1, std::memory_order_relaxed);
    return val;
  }

  /// Append the contents of the given ArrayStorage \p other starting at
  /// index \p fromIndex to this ArrayStorage, where we know that there is no
  /// need to allocate additional capacity. This is more efficient than
  /// append, since the caller does not need to pin the ArrayStorage, check
  /// for exceptions, or write back the resulting pointer in case it moves.
  void appendWithinCapacity(
      Runtime &runtime,
      ArrayStorageBase *other,
      size_type fromIndex = 0) {
    appendWithinCapacity(runtime, other, fromIndex, other->size());
  }

  /// Append the elements of the given ArrayStorage \p other in the range
  /// [\p fromIndex, \p toIndex) to this ArrayStorage, where we know that there
  /// is no need to allocate additional capacity.
  void appendWithinCapacity(
      Runtime &runtime,
      ArrayStorageBase *other,
      size_type fromIndex,
      size_type toIndex) {
    size_t sz = size();
    assert(
        fromIndex <= toIndex && toIndex <= other->size() &&
        "range out of bounds");
    size_t copyLen = toIndex - fromIndex;
    assert(sz + copyLen <= capacity() && "Insufficient capacity");
    auto *fromStart = other->data() + fromIndex;
    auto *fromEnd = other->data() + toIndex;
    GCHVType::uninitialized_copy(
        fromStart, fromEnd, data() + sz, this, runtime.getHeap());
    size_.store(sz + copyLen, std::memory_order_release);
  }

  /// Append the contents of the given ArrayStorage \p other starting at
  /// index \p fromIndex to this ArrayStorage.
  ///
  /// \param[in,out] selfHandle The ArrayStorageBase to be modified. Note the
  /// MutableHandle will be updated to point to a new allocated ArrayStorageBase
  /// if allocation is required. Caller needs to take the updated handle value
  /// after the call to update their own pointers.
  /// \param runtime The Runtime.
  /// \param other The ArrayStorage to append.
  /// \param fromIndex The starting index in \p other from which to copy.
  static ExecutionStatus append(
      MutableHandle<ArrayStorageBase> selfHandle,
      Runtime &runtime,
      Handle<ArrayStorageBase> other,
      size_type fromIndex = 0) {
    auto *self = selfHandle.get();
    assert(fromIndex <= other->size() && "fromIndex out of range");
    size_t copyLen = other->size() - fromIndex;
    if (LLVM_LIKELY(self->size() + copyLen <= self->capacity())) {
      self->appendWithinCapacity(runtime, *other, fromIndex);
      return ExecutionStatus::RETURNED;
    }
    return appendSlowPath(selfHandle, runtime, other, fromIndex);
  }

  /// Ensure that the capacity of the array is at least \p capacity,
  /// reallocating if needed.
  ///
  /// \param[in,out] selfHandle The ArrayStorageBase to be modified. Note the
  /// MutableHandle will be updated to point to a new allocated ArrayStorageBase
  /// if allocation is required. Caller needs to take the updated handle value
  /// after the call to update their own pointers.
  /// \param runtime The Runtime.
  /// \param capacity The capacity to ensure is available.
  static ExecutionStatus ensureCapacity(
      MutableHandle<ArrayStorageBase<HVType>> selfHandle,
      Runtime &runtime,
      size_type capacity);

  /// Decrease the size to zero.
  void clear(Runtime &runtime) {
    resizeWithinCapacity(this, runtime, 0);
  }

  /// Change the size of the storage to \p newSize. If the new size is larger,
  /// the additional elements are initialized to "empty". This may cause a
  /// reallocation when the new size exceeds the capacity. If the new size is
  /// smaller, the "extra" elements are marked as unreachable for the GC, but
  /// there is no reallocation.
  ///
  /// \param[in,out] selfHandle The ArrayStorageBase to be modified. Note the
  /// MutableHandle will be updated to point to a new allocated ArrayStorageBase
  /// if allocation is required. Caller needs to take the updated handle value
  /// after the call to update their own pointers.
  /// \param runtime The Runtime.
  /// \param newSize The new size of the ArrayStorage.
  static ExecutionStatus resize(
      MutableHandle<ArrayStorageBase<HVType>> selfHandle,
      Runtime &runtime,
      size_type newSize) {
    if (selfHandle->size() == newSize)
      return ExecutionStatus::RETURNED;
    // Check if we could expand within capacity.
    if (newSize <= selfHandle->capacity()) {
      ArrayStorageBase::resizeWithinCapacity(
          *selfHandle, runtime.getHeap(), newSize);
      return ExecutionStatus::RETURNED;
    }

    // Reallocate to a capacity of at least newSize.
    return reallocateToLarger(selfHandle, runtime, newSize, 0, 0, newSize);
  }

  /// Change the size of the storage to \p newSize. If the size is decreasing,
  /// shift the data the left, marking the extra elements at the end as
  /// unreachable. If the size is increasing, reallocate if necessary, shift the
  /// data to the right and initialize the new elements at the start as "empty".
  ///
  /// \param[in,out] selfHandle The ArrayStorageBase to be modified. Note the
  /// MutableHandle will be updated to point to a new allocated ArrayStorageBase
  /// if allocation is required. Caller needs to take the updated handle value
  /// after the call to update their own pointers.
  /// \param runtime The Runtime.
  /// \param newSize The new size of the ArrayStorage.
  static ExecutionStatus resizeLeft(
      MutableHandle<ArrayStorageBase<HVType>> selfHandle,
      Runtime &runtime,
      size_type newSize);

  /// Set the size to a value <= the capacity. This is a special
  /// case of resize() but has a simpler interface since we know that it doesn't
  /// need to reallocate.
  static void resizeWithinCapacity(
      ArrayStorageBase<HVType> *self,
      GC &gc,
      size_type newSize);

  static void resizeWithinCapacity(
      ArrayStorageBase<HVType> *self,
      Runtime &runtime,
      size_type newSize) {
    resizeWithinCapacity(self, runtime.getHeap(), newSize);
  }

  /// Set the size to \p newSize, which must be >= \c size_ and <= the capacity.
  static void growWithinCapacity(
      ArrayStorageBase *self,
      Runtime &runtime,
      size_type newSize) {
    const auto sz = self->size();
    // Using GCHVType::uninitialized_fill generates a call to a libc routine to
    // initialize the memory. However, this is an indirect call, which is
    // expensive since we know that the number of additional elements is
    // typically small. Instead, we use our own loop where we block the compiler
    // from emitting a libc call using an empty asm statement.
    for (auto cur = self->data() + sz, end = self->data() + newSize; cur != end;
         ++cur) {
      // Pass the extra nullptr to select the overload that doesn't handle
      // pointers.
      new (&*cur) GCHermesValueInLargeObjImpl<HVType>(
          HVType::encodeEmptyValue(), runtime.getHeap(), nullptr);
#ifdef __GNUC__
      // This empty asm statement tells the compiler that we want each write to
      // be observable, so it cannot turn the loop into a memset call.
      asm volatile("" : : : "memory");
#endif
    }
    self->size_.store(newSize, std::memory_order_release);
  }

  /// Append the given element to the end when the capacity has been exhausted
  /// and a reallocation is needed.
  ///
  /// \param[in,out] selfHandle The ArrayStorageBase to be modified. Note the
  /// MutableHandle will be updated to point to a new allocated ArrayStorageBase
  /// if allocation is required. Caller needs to take the updated handle value
  /// after the call to update their own pointers.
  /// \param runtime The Runtime.
  /// \param value The value to append.
  static ExecutionStatus pushBackSlowPath(
      MutableHandle<ArrayStorageBase<HVType>> selfHandle,
      Runtime &runtime,
      Handle<> value);

 public:
  ArrayStorageBase() = default;
  ArrayStorageBase(const ArrayStorageBase &) = delete;
  void operator=(const ArrayStorageBase &) = delete;
  ~ArrayStorageBase() = delete;

 private:
  /// Throws a RangeError with a descriptive message describing the attempted
  /// capacity allocated.
  /// \returns ExecutionStatus::EXCEPTION always.
  static ExecutionStatus throwAllocationFailure(
      Runtime &runtime,
      size_type capacity);

  /// Append the contents of the given ArrayStorage \p other to this
  /// ArrayStorage when the capacity has been exhausted and a reallocation is
  /// needed.
  ///
  /// \param[in,out] selfHandle The ArrayStorageBase to be modified. Note the
  /// MutableHandle will be updated to point to a new allocated ArrayStorageBase
  /// if allocation is required. Caller needs to take the updated handle value
  /// after the call to update their own pointers.
  /// \param runtime The Runtime.
  /// \param other The ArrayStorage to append.
  /// \param fromIndex The starting index in \p other from which to copy.
  static ExecutionStatus appendSlowPath(
      MutableHandle<ArrayStorageBase<HVType>> selfHandle,
      Runtime &runtime,
      Handle<ArrayStorageBase> other,
      size_type fromIndex = 0);

  /// Shrinks \p self during GC compaction, so that it's capacity is equal to
  /// its size.
  /// \return the size the object will have when compaction is complete.
  static gcheapsize_t _trimSizeCallback(const GCCell *self);

  /// Reallocate to a larger storage capacity of the storage and copy the
  /// specified portion of the data to the new storage. The length of the data
  /// to be copied is
  ///   length = min(size - fromFirst, toLast - toFirst).
  /// "length" number of elements are copied from "fromFirst" to "toFirst".
  /// \p minCapacity must be larger than \p toLast, and it is guaranteed that
  /// the array will have a capacity of at least \p minCapacity after this
  /// operation. \p minCapacity should also be larger than
  /// selfHandle->capacity(), otherwise we do not need reallocation.
  ///
  /// \param[in,out] selfHandle The ArrayStorageBase to be modified. Note the
  /// MutableHandle will be updated to point to a new allocated ArrayStorageBase
  /// if allocation is required. Caller needs to take the updated handle value
  /// after the call to update their own pointers.
  /// \param runtime The Runtime.
  /// \param minCapacity The minimum capacity the ArrayStorage will have.
  /// \param fromFirst First element to be copied from.
  /// \param toFirst Index where to copy the first element to.
  /// \param toLast Index where to copy the last element to.
  static ExecutionStatus reallocateToLarger(
      MutableHandle<ArrayStorageBase<HVType>> selfHandle,
      Runtime &runtime,
      size_type minCapacity,
      size_type fromFirst,
      size_type toFirst,
      size_type toLast);

  /// Dummy function for static asserts that may need private fields. This
  /// function is specialised for the specific \p HVType.
  static void staticAsserts();
};

using ArrayStorage = ArrayStorageBase<HermesValue>;
using ArrayStorageSmall = ArrayStorageBase<SmallHermesValue>;

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_ARRAYSTORAGE_H

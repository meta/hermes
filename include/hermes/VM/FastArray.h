/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_FASTARRAY_H
#define HERMES_VM_FASTARRAY_H

#include "hermes/VM/IterationKind.h"
#include "hermes/VM/JSObject.h"

namespace hermes {
namespace vm {

/// A simple fast array type intended to be used from statically typed code. For
/// performance, it uses contiguous backing storage, and forbids much of the
/// dynamic behaviour of ordinary JS arrays, like writing to the length or
/// configuring elements.
class FastArray : public JSObject {
  /// The indexed storage for this array.
  GCPointer<ArrayStorageSmall> indexedStorage_;

  friend void FastArrayBuildMeta(const GCCell *cell, Metadata::Builder &mb);

  /// \return the index at which we know the length property will be stored.
  static constexpr inline SlotIndex lengthPropIndex() {
    // It will be in the first slot after the overlap slots.
    return numOverlapSlots<FastArray>();
  }

 public:
  static const ObjectVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::FastArrayKind;
  }

  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::FastArrayKind;
  }

  /// \return the length of this FastArray, by reading it from the known length
  /// property slot. We provide two versions so we are explicit about
  /// conversions to/from double. In general, if we are comparing against an
  /// integer, the uint32 version is preferable, so the other integer does not
  /// need to be converted to a double for the comparison. On the other hand, if
  /// we need the value as a double, we should use the double version to avoid a
  /// roundtrip through uint32.
  uint32_t getLengthAsUint32(PointerBase &pb) const {
    return getDirectSlotValue<lengthPropIndex()>(this).getNumber(pb);
  }
  double getLengthAsDouble(PointerBase &pb) const {
    return getDirectSlotValue<lengthPropIndex()>(this).getNumber(pb);
  }

  /// \return the backing storage of this FastArray. This is useful if we want
  /// to make multiple accesses to the storage without having to retrieve it
  /// each time. Note that this must never be used to push or pop elements (or
  /// otherwise resize the array), as that will invalidate the length property.
  ArrayStorageSmall *unsafeGetIndexedStorage(PointerBase &pb) {
    return indexedStorage_.getNonNull(pb);
  }

  /// \return the value at index \p index, or throw if the index is not
  /// within bounds
  CallResult<SmallHermesValue> at(Runtime &runtime, size_t index) const {
    auto *storage = indexedStorage_.getNonNull(runtime);
    if (index >= storage->size())
      return runtime.raiseRangeError("Array index out of bounds");
    return storage->at(index);
  }

  /// \return the value at \p index, where it is known to be within bounds.
  SmallHermesValue unsafeAt(Runtime &runtime, size_t index) const {
    auto *storage = indexedStorage_.getNonNull(runtime);
    assert(index < getLengthAsUint32(runtime));
    return storage->at(index);
  }

  /// Set the value at \p index to \p val, where it is known to be within
  /// bounds.
  ExecutionStatus
  unsafeSet(Runtime &runtime, size_t index, SmallHermesValue val) {
    auto *storage = indexedStorage_.getNonNull(runtime);
    assert(index < getLengthAsUint32(runtime));
    storage->set(index, val, runtime.getHeap());
    return ExecutionStatus::RETURNED;
  }

  /// Set the value at \p index to \p val, or throw if the index is not within
  /// bounds.
  ExecutionStatus set(Runtime &runtime, size_t index, SmallHermesValue val) {
    auto *storage = indexedStorage_.getNonNull(runtime);
    if (index >= storage->size())
      return runtime.raiseRangeError("Array index out of bounds");
    storage->set(index, val, runtime.getHeap());
    return ExecutionStatus::RETURNED;
  }

  /// Push the element \p val onto the end of the storage, and increase the
  /// length by 1.
  static ExecutionStatus
  push(Handle<FastArray> self, Runtime &runtime, Handle<> val) {
    // Speculatively convert the value to a SmallHermesValue. This must be done
    // before we check the capacity, because it can trigger a GC, which could
    // resize the storage.
    auto shv = SmallHermesValue::encodeHermesValue(*val, runtime);
    auto *storage = self->indexedStorage_.getNonNull(runtime);
    auto curSz = storage->size();
    if (LLVM_LIKELY(storage->size() < storage->capacity())) {
      storage->pushWithinCapacity(runtime, shv);
      auto newSz = SmallHermesValue::encodeNumberValue(curSz + 1, runtime);
      self->setLength(runtime, newSz);
      return ExecutionStatus::RETURNED;
    }
    return pushSlow(self, runtime, val);
  }

  /// Pop the last \p n elements from the array and decrease the length by the
  /// number of elements popped. If \p n exceeds the current length, all
  /// elements are popped.
  /// \return the topmost popped element, or undefined if no elements were
  /// popped (either the array was empty or \p n was zero).
  static HermesValue pop(Handle<FastArray> self, Runtime &runtime, uint32_t n) {
    uint32_t sz = self->indexedStorage_.getNonNull(runtime)->size();
    if (sz == 0 || n == 0)
      return HermesValue::encodeUndefinedValue();

    uint32_t toPop = std::min(n, sz);
    uint32_t newSz = sz - toPop;

    // Encode the new length before shrinking, this can allocate.
    auto newLen = SmallHermesValue::encodeNumberValue(newSz, runtime);

    NoAllocScope noAlloc{runtime};
    auto *storage = self->indexedStorage_.getNonNull(runtime);
    // Capture the topmost element about to be popped before resizing.
    SmallHermesValue lastSHV = storage->at(sz - 1);
    ArrayStorageSmall::resizeWithinCapacity(storage, runtime.getHeap(), newSz);
    HermesValue val = lastSHV.unboxToHV(runtime);
    self->setLength(runtime, newLen);
    return val;
  }

  /// Append the elements of \p other starting at index \p fromIndex onto the
  /// end of \p self.
  static ExecutionStatus append(
      Handle<FastArray> self,
      Runtime &runtime,
      Handle<FastArray> other,
      uint32_t fromIndex = 0) {
    auto *storage = self->indexedStorage_.getNonNull(runtime);
    auto *otherStorage = other->indexedStorage_.getNonNull(runtime);
    assert(fromIndex <= otherStorage->size() && "fromIndex out of range");
    size_t curSz = storage->size();
    size_t copyLen = otherStorage->size() - fromIndex;
    size_t newSz = curSz + copyLen;
    if (LLVM_LIKELY(newSz <= storage->capacity())) {
      storage->appendWithinCapacity(runtime, otherStorage, fromIndex);
      auto shv = SmallHermesValue::encodeNumberValue(newSz, runtime);
      self->setLength(runtime, shv);
      return ExecutionStatus::RETURNED;
    }
    return appendSlow(self, runtime, other, fromIndex);
  }

  /// Construct an instance of the hidden class describing the layout of JSArray
  /// instances.
  /// \param rootClazz the starting HiddenClass for the JSArray.
  static PseudoHandle<HiddenClass> createClass(
      Runtime &runtime,
      Handle<JSObject> prototypeHandle,
      Handle<HiddenClass> rootClazz);

  /// Create a new FastArray with the given capacity. Note that storage will be
  /// allocated even if the capacity is zero, to maintain indexedStorage_ as
  /// non-null.
  static CallResult<HermesValue> create(Runtime &runtime, size_t capacity = 4);

  /// Create a new FastArray with the given \p prototype and \p capacity.
  static CallResult<HermesValue>
  create(Runtime &runtime, Handle<JSObject> prototype, size_t capacity);

  template <typename NeedsBarriers>
  FastArray(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz,
      NeedsBarriers needsBarriers)
      : JSObject(runtime, *parent, *clazz, needsBarriers) {
    assert(hasIndexedStorage(runtime) && "must have indexed storage");
    assert(
        hasFastIndexProperties(runtime) && "must have fast index properties");
  }

 private:
  /// Set the length of this FastArray to \p newLength.
  void setLength(Runtime &runtime, SmallHermesValue newLength) {
    // TODO: Potentially optimise this by knowing it's a SMI.
    setDirectSlotValue<lengthPropIndex()>(this, newLength, runtime.getHeap());
  }

  static ExecutionStatus
  pushSlow(Handle<FastArray> self, Runtime &runtime, Handle<> val);

  static ExecutionStatus appendSlow(
      Handle<FastArray> self,
      Runtime &runtime,
      Handle<FastArray> other,
      uint32_t fromIndex = 0);

#ifdef HERMES_MEMORY_INSTRUMENTATION
  /// Adds the special indexed element edges from this array to its backing
  /// storage.
  static void _snapshotAddEdgesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap);
#endif

  /// Check whether property with index \p index exists in indexed storage and
  /// \return true if it does.
  static bool
  _haveOwnIndexedImpl(JSObject *selfObj, Runtime &runtime, uint32_t index);

  /// Check whether property with index \p index exists in indexed storage and
  /// extract its \c PropertyFlags.
  /// \return PropertyFlags if the property exists.
  static OptValue<PropertyFlags> _getOwnIndexedPropertyFlagsImpl(
      JSObject *selfObj,
      Runtime &runtime,
      uint32_t index);

  /// \return the range of indices (end-exclusive) in the array.
  static std::pair<uint32_t, uint32_t> _getOwnIndexedRangeImpl(
      JSObject *selfObj,
      PointerBase &runtime);

  /// Obtain an element from the "indexed storage" of this object. The storage
  /// itself is implementation dependent.
  /// \return the value of the element or "empty" if there is no such element.
  static HermesValue _getOwnIndexedImpl(
      PseudoHandle<JSObject> self,
      Runtime &runtime,
      uint32_t index);

  /// Attempts to set an element in the indexed storage. Currently, this always
  /// throws, as setting elements in a FastArray is only supported using
  /// specialised operations in typed mode.
  static CallResult<bool> _setOwnIndexedImpl(
      Handle<JSObject> selfHandle,
      Runtime &runtime,
      uint32_t index,
      Handle<> value);

  /// Attempts to delete an element in the FastArray. FastArray does not
  /// actually support element deletion so this always returns false.
  static bool _deleteOwnIndexedImpl(
      Handle<JSObject> selfHandle,
      Runtime &runtime,
      uint32_t index);

  /// Check whether all indexed properties satisfy the requirement specified by
  /// \p mode. Either whether they are all non-configurable, or whether they are
  /// all both non-configurable and non-writable.
  static bool _checkAllOwnIndexedImpl(
      JSObject *selfObj,
      Runtime &runtime,
      ObjectVTable::CheckAllOwnIndexedMode mode);

  /// Dummy function for static asserts that may need private fields.
  static void staticAsserts();
};

} // namespace vm
} // namespace hermes
#endif

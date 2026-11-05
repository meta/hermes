/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JSARRAY_H
#define HERMES_VM_JSARRAY_H

#include "hermes/VM/IterationKind.h"
#include "hermes/VM/JSObject.h"

namespace hermes {
namespace vm {

/// A common implementation of "Array-like" objects.
class ArrayImpl : public JSObject {
  using Super = JSObject;

 public:
  using size_type = uint32_t;
  /// StorageType is the underlying storage that JSArray uses to put the values
  /// into.
  using StorageType = ArrayStorageSmall;

 private:
  /// The first index contained in the storage.
  uint32_t beginIndex_{0};
  /// Number of elements in the storage starting from \p beginIndex_.
  uint32_t elemCount_{0};
  /// The indexed storage for this array.
  GCPointer<StorageType> indexedStorage_;

  friend void ArrayImplBuildMeta(const GCCell *cell, Metadata::Builder &mb);
  /// The x86-64 JIT emits an inline fast array element store that reads the
  /// three fields above directly; RuntimeOffsets is where their offsets are
  /// derived with offsetof().
  friend struct RuntimeOffsets;

 public:
  static bool classof(const GCCell *cell) {
    return kindInRange(
        cell->getKind(),
        CellKind::ArrayImplKind_first,
        CellKind::ArrayImplKind_last);
  }

  /// @name API for C++ users.
  /// @{

  /// Resize the internal storage. The ".length" property is not affected. It
  /// does \b NOT check for read-only properties.
  static ExecutionStatus setStorageEndIndex(
      Handle<ArrayImpl> selfHandle,
      Runtime &runtime,
      uint32_t newLength);

  /// Set the size of the storage to be \p newLength which must >= the current
  /// size. The ".length" property is not affected. It does \b NOT check for
  /// read-only properties.
  static ExecutionStatus increaseStorageEndIndex(
      Handle<ArrayImpl> selfHandle,
      Runtime &runtime,
      uint32_t newLength) {
    assert(
        newLength >= selfHandle->beginIndex_ + selfHandle->elemCount_ &&
        "Must be growing");
    // Check if the storage has already been initialized.
    if (LLVM_LIKELY(selfHandle->indexedStorage_)) {
      auto newElemCount = newLength - selfHandle->beginIndex_;
      auto *indexedStorage = selfHandle->getIndexedStorageUnsafe(runtime);
      // If the storage has sufficient capacity, just increase the size inline.
      if (LLVM_LIKELY(newElemCount <= indexedStorage->capacity())) {
        selfHandle->elemCount_ = newElemCount;
        StorageType::growWithinCapacity(indexedStorage, runtime, newElemCount);
        return ExecutionStatus::RETURNED;
      }
    }
    // Fall back to the general case.
    return setStorageEndIndex(selfHandle, runtime, newLength);
  }

  /// Update the element at index \p index. If necessary, the array will be
  /// resized, but if it is an \c JSArray, it's \c .length property will not
  /// be affected.
  [[nodiscard]] static ExecutionStatus setElementAt(
      Handle<ArrayImpl> selfHandle,
      Runtime &runtime,
      size_type index,
      Handle<> value) {
    return _setOwnIndexedImpl(selfHandle, runtime, index, value).getStatus();
  }

  /// Update an array element, which must exist in storage. Elements with value
  /// "empty" are assumed to exist for the purpose of this definition. This is
  /// only safe to do for arrays that were created by the caller, can be
  /// extended, are not sealed or frozen, and were never passed to user JS code.
  static void unsafeSetExistingElementAt(
      ArrayImpl *self,
      Runtime &runtime,
      size_type index,
      SmallHermesValue value) {
    // The array must be extendable (and by implication is not frozen or sealed)
    // because we don't know whether the element being set is empty or not.
    assert(
        self->getClass(runtime)->isExtensible() &&
        "this array cannot be extended");

    assert(
        index >= self->beginIndex_ && index < self->getEndIndex() &&
        "array index out of range");
    self->getIndexedStorageUnsafe(runtime)->set(
        index - self->beginIndex_, value, runtime.getHeap());
  }

  /// Set the element at index \p index to empty. This does not affect the
  /// storage size or array length.
  /// \return true if the operation succeeded (which is always in this class).
  static bool deleteElementAt(
      Handle<ArrayImpl> selfHandle,
      Runtime &runtime,
      size_type index) {
    return _deleteOwnIndexedImpl(selfHandle, runtime, index);
  }

  /// \return the first index of the array.
  size_type getBeginIndex() const {
    return beginIndex_;
  }

  /// \return the number of elements contained in the storage, starting from
  /// \p beginIndex_.
  size_type getElemCount() const {
    return elemCount_;
  }

  /// Set the elemCount_.
  /// Unsafe: caller must ensure that the number of elements in the storage are
  /// the same as the final elemCount.
  void setElemCountUnsafe(uint32_t elemCount) {
    elemCount_ = elemCount;
  }

  /// \return 1 + the index of the last element contained in the storage.
  size_type getEndIndex() const {
    return beginIndex_ + elemCount_;
  }

  /// Return the value at index \p index, or \c empty if the index is not
  /// contained in the storage.
  const SmallHermesValue at(Runtime &runtime, size_type index) const {
    // Optimized range check: index should be
    // [beginIndex_, beginIndex_+elemCount_). If we subtract beginIndex_ from
    // index, values smaller than beginIndex_ will wrap around to the top of the
    // range, so we can use a single comparison.
    index -= beginIndex_;
    return index < elemCount_ ? getIndexedStorageUnsafe(runtime)->at(index)
                              : SmallHermesValue::encodeEmptyValue();
  }

  /// Return the value at index \p index.
  Handle<> handleAt(Runtime &runtime, size_type index) const {
    return runtime.makeHandle(at(runtime, index).unboxToHV(runtime));
  }

  /// Get a pointer to the indexed storage for this array. The returned value
  /// may be null if there is no indexed storage.
  StorageType *getIndexedStorageNullable(PointerBase &base) const {
    return indexedStorage_.get(base);
  }
  /// Get a pointer to the indexed storage for this array. The indexed storage
  /// must not be null.
  StorageType *getIndexedStorageUnsafe(PointerBase &base) const {
    return indexedStorage_.getNonNull(base);
  }

  /// Set the indexed storage of this array to be \p p. The pointer is allowed
  /// to be null.
  void setIndexedStorage(PointerBase &base, StorageType *p, GC &gc) {
    indexedStorage_.set(base, p, gc);
  }

  /// @}

 protected:
  /// Initialize the object with the supplied parameters.
  /// \param indexedStorage allocated storage for indexed properties. It can be
  ///   nullptr, which implies capacity and size of 0. We rely on this when
  ///   constructing array objects using the JavaScript Array() constructor,
  ///   since we don't know the length in advance.
  /// \tparam NeedsBarriers indicates whether write barriers are needed
  ///   for initializing writes in the constructor.  (In debug builds,
  ///   a claim that they are not necessary is checked dynamically,
  ///   which should find any incorrect specifications.)
  template <typename NeedsBarriers>
  ArrayImpl(
      Runtime &runtime,
      JSObject *parent,
      HiddenClass *clazz,
      NeedsBarriers needsBarriers)
      : JSObject(runtime, parent, clazz, needsBarriers) {
    assert(hasIndexedStorage(runtime) && "must have indexed storage");
    assert(
        hasFastIndexProperties(runtime) && "must have fast index properties");
  }

  /// Default needsBarriers to Yes.
  ArrayImpl(Runtime &runtime, JSObject *parent, HiddenClass *clazz)
      : ArrayImpl(runtime, parent, clazz, GCPointerBase::YesBarriers()) {}

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
  /// extract its \c PropertyFlags (if necessary checking whether the object is
  /// frozen or sealed).
  /// \return PropertyFlags if the property exists.
  static OptValue<PropertyFlags> _getOwnIndexedPropertyFlagsImpl(
      JSObject *selfObj,
      Runtime &runtime,
      uint32_t index);

  /// \return the range of indexes (end-exclusive) in the array.
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

  /// Set an element in the "indexed storage" of this object. Depending on the
  /// semantics of the "indexed storage" the storage capacity may need to be
  /// expanded (e.g. affecting Array.length), or the write may simply be ignored
  /// (in the case of typed arrays).
  /// \return true if the write succeeded, or false if it was ignored.
  static CallResult<bool> _setOwnIndexedImpl(
      Handle<JSObject> selfHandle,
      Runtime &runtime,
      uint32_t index,
      Handle<> value);

  /// Delete an element in the "indexed storage".
  /// \return 'true' if the element was successfully deleted, or if it was
  ///     outside of the storage range. 'false' if this storage doesn't support
  ///     "holes"/deletion (e.g. typed arrays).
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

  /// Return the value at index \p index, which must be valid.
  const SmallHermesValue unsafeAt(Runtime &runtime, size_type index) const {
    return getIndexedStorageUnsafe(runtime)->at(index - beginIndex_);
  }
};

class Arguments final : public ArrayImpl {
  using Super = ArrayImpl;

 public:
  static const ObjectVTable vt;

  // We need one more slot for the '.length' property.
  static const PropStorage::size_type NAMED_PROPERTY_SLOTS =
      Super::NAMED_PROPERTY_SLOTS + 1;

  static constexpr CellKind getCellKind() {
    return CellKind::ArgumentsKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::ArgumentsKind;
  }

  /// Create an instance of Arguments, with size and capacity equal to \p length
  /// and a property "length" initialized to that value.
  static CallResult<PseudoHandle<Arguments>> create(
      Runtime &runtime,
      size_type length,
      Handle<Callable> curFunction,
      bool strictMode);

  Arguments(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz)
      : ArrayImpl(runtime, *parent, *clazz) {}
};

class JSArray final : public ArrayImpl {
  using Super = ArrayImpl;

  // Object needs to be able to call setLength.
  friend class JSObject;

  static constexpr SlotIndex jsArrayPropertyCount() {
    return numOverlapSlots<JSArray>() + NAMED_PROPERTY_SLOTS;
  }

  static constexpr inline SlotIndex lengthPropIndex() {
    return jsArrayPropertyCount() - 1;
  }

 public:
  static const ObjectVTable vt;

  // We need one more slot for the '.length' property.
  static const PropStorage::size_type NAMED_PROPERTY_SLOTS =
      Super::NAMED_PROPERTY_SLOTS + 1;

  /// Construct an instance of the hidden class describing the layout of JSArray
  /// instances.
  /// \param rootClazz the starting HiddenClass for the JSArray.
  static HiddenClass *createClass(
      Runtime &runtime,
      Handle<JSObject> prototypeHandle,
      Handle<HiddenClass> rootClazz);

  static constexpr CellKind getCellKind() {
    return CellKind::JSArrayKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::JSArrayKind;
  }

  /// \return the maximum capacity JSArray for which an object and indexed
  /// storage can be allocated entirely at once in the GC young gen.
  /// This is useful because we can allocate both the object and the indexed
  /// storage easily in the JIT.
  static constexpr inline uint32_t maxYoungGenAllocationCapacity() {
    return StorageType::capacityForAllocationSize(
        GC::maxYoungGenAllocationSize() - heapAlignSize(cellSize<JSArray>()));
  }

  static uint32_t getLength(const JSArray *self, PointerBase &pb) {
    return getDirectSlotValue<lengthPropIndex()>(self).getNumber(pb);
  }

  /// Create an instance of Array, with [[Prototype]] initialized with
  /// \p prototypeHandle, with capacity for \p capacity elements and actual size
  /// \p length. Sets the size of the indexed property storage array to \p
  /// length and sets the elemCount_ to \p length.
  ///
  /// Does not allocate the return object's property storage array.
  static CallResult<PseudoHandle<JSArray>> createNoAllocPropStorage(
      Runtime &runtime,
      Handle<JSObject> prototypeHandle,
      Handle<HiddenClass> classHandle,
      size_type capacity = 0,
      size_type length = 0);

  /// Create an instance of Array, with [[Prototype]] initialized with
  /// \p prototypeHandle, with capacity for \p capacity elements and actual size
  /// \p length. It also allocates the return object's property storage array
  /// to hold all properties in \p classHandle.
  static CallResult<PseudoHandle<JSArray>> createAndAllocPropStorage(
      Runtime &runtime,
      Handle<JSObject> prototypeHandle,
      Handle<HiddenClass> classHandle,
      size_type capacity = 0,
      size_type length = 0);

  static CallResult<PseudoHandle<JSArray>> create(
      Runtime &runtime,
      Handle<JSObject> prototypeHandle,
      size_type capacity,
      size_type length);
  static CallResult<PseudoHandle<JSArray>> create(
      Runtime &runtime,
      Handle<JSObject> prototypeHandle) {
    return create(runtime, prototypeHandle, 0, 0);
  }

  /// Create an instance of Array, using the standard array prototype, with
  /// capacity for \p capacity elements and actual size \p length.
  static CallResult<PseudoHandle<JSArray>>
  create(Runtime &runtime, size_type capacity, size_type length);

  /// A convenience method for setting the \c .length property of the array.
  /// It performs the necessary checks and updates the property. It could fail
  /// if the property is not writable or if there are read-only index-like
  /// properties which cannot be deleted (see the spec for details).
  static CallResult<bool> setLengthProperty(
      Handle<JSArray> selfHandle,
      Runtime &runtime,
      uint32_t newValue,
      PropOpFlags opFlags = PropOpFlags{}) {
    // TODO: optimize this now that we know the index of the property slot.
    return putNamed_RJS(
        selfHandle,
        runtime,
        Predefined::getSymbolID(Predefined::length),
        runtime.makeHandle(HermesValue::encodeTrustedNumberValue(newValue)),
        opFlags);
  }

  template <typename NeedsBarrier>
  JSArray(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz,
      NeedsBarrier needsBarrier)
      : ArrayImpl(runtime, *parent, *clazz, needsBarrier) {}

  /// Write \p newLength directly to the length property of the array. The
  /// caller must have already checked that the length is writable, and that the
  /// end index does not need to be updated.
  static void
  putLengthUnsafe(JSArray *self, Runtime &runtime, SmallHermesValue newLength) {
    setDirectSlotValue<lengthPropIndex()>(self, newLength, runtime.getHeap());
  }

 private:
  /// Update the JavaScript '.length' property, which also resizes the array.
  /// The writability of the property \b MUST already have been checked.
  /// If not sure, use \c putNamed().
  static CallResult<bool> setLength(
      Handle<JSArray> selfHandle,
      Runtime &runtime,
      Handle<> newLength,
      PropOpFlags opFlags);

  /// Update the JavaScript '.length' property, which also resizes the array.
  /// The writability of the property \b MUST already have been checked.
  /// If not sure, use \c putNamed().
  static CallResult<bool> setLength(
      Handle<JSArray> selfHandle,
      Runtime &runtime,
      uint32_t newLength,
      PropOpFlags opFlags);
};

/// The ArrayIterator class, which is used to iterate from 0 to the array's
/// length.
/// ES6.0 22.1.5.
class JSArrayIterator : public JSObject {
  using Super = JSObject;

  /// [[IteratedObject]]
  /// This is null if iteration has been completed.
  GCPointer<JSObject> iteratedObject_;

  /// [[ArrayIteratorNextIndex]]
  uint64_t nextIndex_{0};

  /// [[ArrayIterationKind]]
  IterationKind iterationKind_;

  friend void JSArrayIteratorBuildMeta(
      const GCCell *cell,
      Metadata::Builder &mb);

 public:
  static const ObjectVTable vt;

  static constexpr CellKind getCellKind() {
    return CellKind::JSArrayIteratorKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::JSArrayIteratorKind;
  }

  static PseudoHandle<JSArrayIterator>
  create(Runtime &runtime, Handle<JSObject> array, IterationKind iterationKind);

  /// Iterate to the next element and return.
  static CallResult<HermesValue> nextElement(
      Handle<JSArrayIterator> self,
      Runtime &runtime);

 public:
  JSArrayIterator(
      Runtime &runtime,
      Handle<JSObject> parent,
      Handle<HiddenClass> clazz,
      Handle<JSObject> iteratedObject,
      IterationKind iterationKind)
      : JSObject(runtime, *parent, *clazz),
        iteratedObject_(runtime, *iteratedObject, runtime.getHeap()),
        iterationKind_(iterationKind) {}
};

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_JSARRAY_H

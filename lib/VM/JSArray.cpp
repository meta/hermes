/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JSArray.h"

#include "hermes/VM/BuildMetadata.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/Handle.h"
#include "hermes/VM/JSTypedArray.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/PropertyAccessor.h"
#include "hermes/VM/Runtime-inline.h"

namespace hermes {
namespace vm {

//===----------------------------------------------------------------------===//
// class ArrayImpl

void ArrayImplBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<ArrayImpl>());
  JSObjectBuildMeta(cell, mb);
  const auto *self = static_cast<const ArrayImpl *>(cell);
  // This edge has to be called "elements" in order for Chrome to attribute
  // the size of the indexed storage as part of total usage of "JS Arrays".
  mb.addField("elements", &self->indexedStorage_);
}

#ifdef HERMES_MEMORY_INSTRUMENTATION
void ArrayImpl::_snapshotAddEdgesImpl(
    GCCell *cell,
    GC &gc,
    HeapSnapshot &snap) {
  auto *const self = vmcast<ArrayImpl>(cell);
  // Add the super type's edges too.
  JSObject::_snapshotAddEdgesImpl(self, gc, snap);
  if (!self->getIndexedStorageNullable(gc.getPointerBase())) {
    return;
  }

  // This edge has to be called "elements" in order for Chrome to attribute
  // the size of the indexed storage as part of total usage of "JS Arrays".
  snap.addNamedEdge(
      HeapSnapshot::EdgeType::Internal,
      "elements",
      gc.getObjectID(self->getIndexedStorageUnsafe(gc.getPointerBase())));
  auto *const indexedStorage =
      self->getIndexedStorageUnsafe(gc.getPointerBase());
  const auto beginIndex = self->beginIndex_;
  const auto endIndex = self->getEndIndex();
  for (uint32_t i = beginIndex; i < endIndex; i++) {
    const auto &elem = indexedStorage->at(i - beginIndex);
    const llvh::Optional<HeapSnapshot::NodeID> elemID =
        gc.getSnapshotID(elem.toHV(gc.getPointerBase()));
    if (!elemID) {
      continue;
    }
    snap.addIndexedEdge(HeapSnapshot::EdgeType::Element, i, elemID.getValue());
  }
}
#endif

bool ArrayImpl::_haveOwnIndexedImpl(
    JSObject *selfObj,
    Runtime &runtime,
    uint32_t index) {
  auto *self = vmcast<ArrayImpl>(selfObj);

  // Check whether the index is within the storage.
  if (index >= self->beginIndex_ && index < self->getEndIndex())
    return !self->getIndexedStorageUnsafe(runtime)
                ->at(index - self->beginIndex_)
                .isEmpty();

  return false;
}

OptValue<PropertyFlags> ArrayImpl::_getOwnIndexedPropertyFlagsImpl(
    JSObject *selfObj,
    Runtime &runtime,
    uint32_t index) {
  auto *self = vmcast<ArrayImpl>(selfObj);

  // Check whether the index is within the storage.
  index -= self->beginIndex_;
  if (index < self->elemCount_ &&
      !self->getIndexedStorageUnsafe(runtime)->at(index).isEmpty()) {
    PropertyFlags indexedElementFlags{};
    indexedElementFlags.enumerable = 1;
    indexedElementFlags.writable = 1;
    indexedElementFlags.configurable = 1;

    if (LLVM_UNLIKELY(self->getClass(runtime)->isSealed())) {
      indexedElementFlags.configurable = 0;
      if (LLVM_UNLIKELY(self->getClass(runtime)->isFrozen()))
        indexedElementFlags.writable = 0;
    }

    return indexedElementFlags;
  }

  return llvh::None;
}

std::pair<uint32_t, uint32_t> ArrayImpl::_getOwnIndexedRangeImpl(
    JSObject *selfObj,
    PointerBase &) {
  auto *self = vmcast<ArrayImpl>(selfObj);
  return {self->beginIndex_, self->getEndIndex()};
}

HermesValue ArrayImpl::_getOwnIndexedImpl(
    PseudoHandle<JSObject> selfObj,
    Runtime &runtime,
    uint32_t index) {
  NoAllocScope noAllocs{runtime};

  return vmcast<ArrayImpl>(selfObj.get())
      ->at(runtime, index)
      .unboxToHV(runtime);
}

ExecutionStatus ArrayImpl::setStorageEndIndex(
    Handle<ArrayImpl> selfHandle,
    Runtime &runtime,
    uint32_t newLength) {
  auto *self = selfHandle.get();

  struct : Locals {
    PinnedValue<StorageType> storage;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  auto *const indexedStorage = self->getIndexedStorageNullable(runtime);
  // If indexedStorage hasn't even been allocated.
  if (LLVM_UNLIKELY(!indexedStorage)) {
    if (newLength == 0) {
      return ExecutionStatus::RETURNED;
    }
    auto arrRes = StorageType::create(runtime, newLength, newLength);
    if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.storage.castAndSetHermesValue<StorageType>(*arrRes);
    selfHandle->setIndexedStorage(runtime, lv.storage.get(), runtime.getHeap());
    selfHandle->beginIndex_ = 0;
    selfHandle->elemCount_ = newLength;
    return ExecutionStatus::RETURNED;
  }

  assert(indexedStorage && "Already checked for null");
  auto beginIndex = self->beginIndex_;

  {
    NoAllocScope scope{runtime};

    if (newLength <= beginIndex) {
      // the new length is prior to beginIndex, clearing the storage.
      selfHandle->elemCount_ = 0;
      // Remove the storage. If this array grows again it can be re-allocated.
      self->setIndexedStorage(runtime, nullptr, runtime.getHeap());
      return ExecutionStatus::RETURNED;
    } else if (newLength - beginIndex <= indexedStorage->capacity()) {
      selfHandle->elemCount_ = newLength - beginIndex;
      StorageType::resizeWithinCapacity(
          indexedStorage, runtime, newLength - beginIndex);
      return ExecutionStatus::RETURNED;
    }
  }

  lv.storage = indexedStorage;
  MutableHandle<StorageType> indexedStorageHnd{lv.storage};
  if (StorageType::resize(indexedStorageHnd, runtime, newLength - beginIndex) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  selfHandle->elemCount_ = newLength - beginIndex;
  selfHandle->setIndexedStorage(runtime, lv.storage.get(), runtime.getHeap());
  return ExecutionStatus::RETURNED;
}

CallResult<bool> ArrayImpl::_setOwnIndexedImpl(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    uint32_t index,
    Handle<> value) {
  auto *self = vmcast<ArrayImpl>(selfHandle.get());
  auto beginIndex = self->beginIndex_;

  if (LLVM_UNLIKELY(self->getClass(runtime)->isFrozen()))
    return false;

  // Check whether the index is within the storage.
  if (uint32_t rel = index - beginIndex; LLVM_LIKELY(rel < self->elemCount_)) {
    const auto shv = SmallHermesValue::encodeHermesValue(*value, runtime);
    Handle<ArrayImpl>::vmcast(selfHandle)
        ->getIndexedStorageUnsafe(runtime)
        ->set(rel, shv, runtime.getHeap());
    return true;
  }

  struct : Locals {
    PinnedValue<StorageType> storage;
    PinnedValue<> nameVal;
    PinnedValue<SymbolID> sym;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // If indexedStorage hasn't even been allocated.
  if (LLVM_UNLIKELY(!self->getIndexedStorageNullable(runtime))) {
    // Allocate storage with capacity for 4 elements and length 1.
    auto arrRes = StorageType::create(runtime, 4, 1);
    if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.storage.castAndSetHermesValue<StorageType>(*arrRes);
    const auto shv = SmallHermesValue::encodeHermesValue(*value, runtime);
    self = vmcast<ArrayImpl>(selfHandle.get());

    self->setIndexedStorage(runtime, lv.storage.get(), runtime.getHeap());
    self->beginIndex_ = index;
    self->elemCount_ = 1;
    lv.storage->set(0, shv, runtime.getHeap());
    return true;
  }

  auto endIndex = self->getEndIndex();
  {
    const auto shv = SmallHermesValue::encodeHermesValue(*value, runtime);
    NoAllocScope scope{runtime};
    self = vmcast<ArrayImpl>(selfHandle.get());
    auto *const indexedStorage = self->getIndexedStorageUnsafe(runtime);

    // Can we do it without reallocation for sure?
    if (index >= endIndex && index - beginIndex < indexedStorage->capacity()) {
      self->elemCount_ = index - beginIndex + 1;
      StorageType::resizeWithinCapacity(
          indexedStorage, runtime, index - beginIndex + 1);
      // self shouldn't have moved since there haven't been any allocations.
      indexedStorage->set(index - beginIndex, shv, runtime.getHeap());
      return true;
    }
  }

  lv.storage = self->getIndexedStorageUnsafe(runtime);
  MutableHandle<StorageType> indexedStorageHandle{lv.storage};
  // We only shift an array if the shift amount is within the limit.
  constexpr uint32_t shiftLimit = (1 << 20);

  // Is the array empty?
  if (LLVM_UNLIKELY(endIndex == beginIndex)) {
    if (StorageType::resize(indexedStorageHandle, runtime, 1) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    const auto shv = SmallHermesValue::encodeHermesValue(*value, runtime);
    indexedStorageHandle->set(0, shv, runtime.getHeap());
    self = vmcast<ArrayImpl>(selfHandle.get());
    self->beginIndex_ = index;
    self->elemCount_ = 1;
  } else if (LLVM_UNLIKELY(
                 (index > endIndex && index - endIndex > shiftLimit) ||
                 (index < beginIndex && beginIndex - index > shiftLimit))) {
    // The new index is too far away from the current index range.
    // Shifting will lead to a very large allocation.
    // This is likely a misuse of the array (e.g. use array as an object).
    // In this case, we should just treat the index access as
    // a property access.
    lv.nameVal = HermesValue::encodeTrustedNumberValue(index);
    auto vr = valueToSymbolID(runtime, lv.nameVal, lv.sym);
    assert(
        vr != ExecutionStatus::EXCEPTION &&
        "valueToIdentifier() failed for uint32_t value");
    (void)vr;

    if (LLVM_UNLIKELY(
            JSObject::defineNewOwnProperty(
                selfHandle,
                runtime,
                *lv.sym,
                PropertyFlags::defaultNewNamedPropertyFlags(),
                value) == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // self->indexedStorage_ is unmodified, we should return directly.
    return true;
  } else if (index >= endIndex) {
    // Extending to the right.
    if (StorageType::resize(
            indexedStorageHandle, runtime, index - beginIndex + 1) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    const auto shv = SmallHermesValue::encodeHermesValue(*value, runtime);
    self = vmcast<ArrayImpl>(selfHandle.get());
    self->elemCount_ = index - beginIndex + 1;
    indexedStorageHandle->set(index - beginIndex, shv, runtime.getHeap());
  } else {
    // Extending to the left. 'index' will become the new 'beginIndex'.
    assert(index < beginIndex);

    if (StorageType::resizeLeft(
            indexedStorageHandle,
            runtime,
            indexedStorageHandle->size() + beginIndex - index) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    const auto shv = SmallHermesValue::encodeHermesValue(*value, runtime);
    self = vmcast<ArrayImpl>(selfHandle.get());
    self->beginIndex_ = index;
    self->elemCount_ = endIndex - index;
    indexedStorageHandle->set(0, shv, runtime.getHeap());
  }

  // Update the potentially changed pointer.
  self->setIndexedStorage(
      runtime, indexedStorageHandle.get(), runtime.getHeap());
  return true;
}

bool ArrayImpl::_deleteOwnIndexedImpl(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    uint32_t index) {
  auto *self = vmcast<ArrayImpl>(selfHandle.get());
  NoAllocScope noAlloc{runtime};
  index -= self->beginIndex_;
  if (index < self->elemCount_) {
    auto *indexedStorage = self->getIndexedStorageUnsafe(runtime);
    // Cannot delete indexed elements if we are sealed.
    if (LLVM_UNLIKELY(self->getClass(runtime)->isSealed())) {
      SmallHermesValue elem = indexedStorage->at(index);
      if (!elem.isEmpty())
        return false;
    }

    indexedStorage->setNonPtr(
        index, SmallHermesValue::encodeEmptyValue(), runtime.getHeap());
  }

  return true;
}

bool ArrayImpl::_checkAllOwnIndexedImpl(
    JSObject *selfObj,
    Runtime &runtime,
    ObjectVTable::CheckAllOwnIndexedMode /*mode*/
) {
  NoAllocScope noAlloc{runtime};
  auto *self = vmcast<ArrayImpl>(selfObj);

  // If we have any indexed properties at all, they don't satisfy the
  // requirements.
  auto *indexedStorage = self->getIndexedStorageNullable(runtime);
  for (uint32_t i = 0, e = self->elemCount_; i != e; ++i) {
    assert(indexedStorage);
    if (!indexedStorage->at(i).isEmpty())
      return false;
  }
  return true;
}

//===----------------------------------------------------------------------===//
// class Arguments

const ObjectVTable Arguments::vt{
    VTable(
        CellKind::ArgumentsKind,
        cellSize<Arguments>(),
        /* allowLargeAlloc */ false,
        nullptr,
        nullptr,
        nullptr
#ifdef HERMES_MEMORY_INSTRUMENTATION
        ,
        VTable::HeapSnapshotMetadata{
            HeapSnapshot::NodeType::Object,
            nullptr,
            Arguments::_snapshotAddEdgesImpl,
            nullptr,
            nullptr}
#endif
        ),
    Arguments::_getOwnIndexedRangeImpl,
    Arguments::_haveOwnIndexedImpl,
    Arguments::_getOwnIndexedPropertyFlagsImpl,
    Arguments::_getOwnIndexedImpl,
    Arguments::_setOwnIndexedImpl,
    Arguments::_deleteOwnIndexedImpl,
    Arguments::_checkAllOwnIndexedImpl,
};

void ArgumentsBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<Arguments>());
  ArrayImplBuildMeta(cell, mb);
  mb.setVTable(&Arguments::vt);
}

CallResult<PseudoHandle<Arguments>> Arguments::create(
    Runtime &runtime,
    size_type length,
    Handle<Callable> curFunction,
    bool strictMode) {
  struct : Locals {
    PinnedValue<Arguments> self;
    PinnedValue<> lengthValue;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  auto clazz = runtime.getHiddenClassForPrototype(
      runtime.objectPrototypeRawPtr, runtime.classArguments);
  auto obj = runtime.makeAFixed<Arguments>(
      runtime, Handle<JSObject>::vmcast(&runtime.objectPrototype), clazz);
  lv.self = JSObjectInit::initToPointer(runtime, obj);

  {
    auto arrRes = StorageType::create(runtime, length);
    if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    lv.self->setIndexedStorage(
        runtime, vmcast<StorageType>(*arrRes), runtime.getHeap());
  }
  Arguments::setStorageEndIndex(lv.self, runtime, length);

  PropertyFlags pf{};
  namespace P = Predefined;

/// Adds a property to the object in \p OBJ_HANDLE.  \p SYMBOL provides its name
/// as a \c Predefined enum value, and its value is  rooted in \p HANDLE.  If
/// property definition fails, the exceptional execution status will be
/// propagated to the outer function.
#define DEFINE_PROP(OBJ_HANDLE, SYMBOL, HANDLE)                            \
  do {                                                                     \
    auto status = JSObject::defineNewOwnProperty(                          \
        OBJ_HANDLE, runtime, Predefined::getSymbolID(SYMBOL), pf, HANDLE); \
    if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {             \
      return ExecutionStatus::EXCEPTION;                                   \
    }                                                                      \
  } while (false)

  // Define the length property.
  pf.enumerable = 0;
  pf.writable = 1;
  pf.configurable = 1;

  lv.lengthValue = HermesValue::encodeTrustedNumberValue(length);
  DEFINE_PROP(lv.self, P::length, lv.lengthValue);

  DEFINE_PROP(lv.self, P::SymbolIterator, runtime.arrayPrototypeValues);

  if (strictMode) {
    // Define .callee and .caller properties: throw always in strict mode
    // or for GeneratorInnerFunction, because GeneratorInnerFunction isn't
    // visible to users.
    auto accessor =
        Handle<PropertyAccessor>::vmcast(&runtime.throwTypeErrorAccessor);

    pf.clear();
    pf.enumerable = 0;
    pf.writable = 0;
    pf.configurable = 0;
    pf.accessor = 1;

    DEFINE_PROP(lv.self, P::callee, accessor);
    DEFINE_PROP(lv.self, P::caller, accessor);
  } else {
    // Define .callee in non-strict mode to point to the current function.
    // Leave .caller undefined, because it's a non-standard ES extension.
    assert(
        vmisa<Callable>(curFunction.getHermesValue()) &&
        "attempt to reify arguments with a non-callable callee");

    pf.clear();
    pf.enumerable = 0;
    pf.writable = 1;
    pf.configurable = 1;

    DEFINE_PROP(lv.self, P::callee, curFunction);
  }

  return createPseudoHandle(*lv.self);

#undef DEFINE_PROP
}

//===----------------------------------------------------------------------===//
// class JSArray

const ObjectVTable JSArray::vt{
    VTable(
        CellKind::JSArrayKind,
        cellSize<JSArray>(),
        /* allowLargeAlloc */ false,
        nullptr,
        nullptr,
        nullptr
#ifdef HERMES_MEMORY_INSTRUMENTATION
        ,
        VTable::HeapSnapshotMetadata{
            HeapSnapshot::NodeType::Object,
            nullptr,
            JSArray::_snapshotAddEdgesImpl,
            nullptr,
            nullptr}
#endif
        ),
    JSArray::_getOwnIndexedRangeImpl,
    JSArray::_haveOwnIndexedImpl,
    JSArray::_getOwnIndexedPropertyFlagsImpl,
    JSArray::_getOwnIndexedImpl,
    JSArray::_setOwnIndexedImpl,
    JSArray::_deleteOwnIndexedImpl,
    JSArray::_checkAllOwnIndexedImpl,
};

void JSArrayBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<JSArray>());
  ArrayImplBuildMeta(cell, mb);
  mb.setVTable(&JSArray::vt);
}

HiddenClass *JSArray::createClass(
    Runtime &runtime,
    Handle<JSObject> prototypeHandle,
    Handle<HiddenClass> rootClazz) {
  assert(rootClazz && "must have a starting point");

  struct : public Locals {
    PinnedValue<HiddenClass> clazz;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  lv.clazz = runtime.getHiddenClassForPrototype(*prototypeHandle, rootClazz);

  PropertyFlags pf{};
  pf.enumerable = 0;
  pf.writable = 1;
  pf.configurable = 0;
  pf.internalSetter = 1;

  auto added = HiddenClass::addProperty(
      lv.clazz,
      runtime,
      Predefined::getSymbolID(Predefined::length),
      pf,
      MutableHandle<HiddenClass>(lv.clazz));
  assert(
      added != ExecutionStatus::EXCEPTION &&
      "Adding the first properties shouldn't cause overflow");
  assert(*added == lengthPropIndex() && "JSArray.length has invalid index");
  (void)added;

  assert(
      lv.clazz->getNumProperties() == jsArrayPropertyCount() &&
      "JSArray class defined with incorrect number of properties");

  return *lv.clazz;
}

CallResult<PseudoHandle<JSArray>> JSArray::createNoAllocPropStorage(
    Runtime &runtime,
    Handle<JSObject> prototypeHandle,
    Handle<HiddenClass> classHandle,
    size_type capacity,
    size_type length) {
  assert(length <= capacity && "length must be <= capacity");

  assert(
      classHandle &&
      classHandle->getNumProperties() >= jsArrayPropertyCount() &&
      "invalid number of properties in JSArray hidden class");

  struct : Locals {
    PinnedValue<JSArray> self;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  if (LLVM_UNLIKELY(capacity > maxYoungGenAllocationCapacity())) {
    // Make two separate allocations.
    lv.self = JSObjectInit::initToPointer(
        runtime,
        runtime.makeAFixed<JSArray>(
            runtime,
            prototypeHandle,
            classHandle,
            GCPointerBase::NoBarriers()));

    // Only allocate the storage if capacity is not zero.
    auto arrRes = StorageType::create(runtime, capacity, length);
    if (arrRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.self->setIndexedStorage(
        runtime, vmcast<StorageType>(*arrRes), runtime.getHeap());
    lv.self->setElemCountUnsafe(length);
  } else if (capacity > 0) {
    // Allocate both together.
    auto storageSize = heapAlignSize(StorageType::allocationSize(capacity));
    auto [self, storage] = runtime.make2YoungGenUnsafe<JSArray, StorageType>(
        heapAlignSize(cellSize<JSArray>()),
        std::tuple<
            Runtime &,
            Handle<JSObject>,
            Handle<HiddenClass>,
            GCPointerBase::NoBarriers>{
            runtime, prototypeHandle, classHandle, GCPointerBase::NoBarriers()},
        storageSize,
        std::tuple{});
    NoAllocScope noAlloc{runtime};
    lv.self = JSObjectInit::initToPointer(runtime, self);
    lv.self->setIndexedStorage(runtime, storage, runtime.getHeap());

    StorageType::growWithinCapacity(storage, runtime, length);
    lv.self->setElemCountUnsafe(length);
  } else {
    // capacity == 0, no storage needed.
    lv.self = JSObjectInit::initToPointer(
        runtime,
        runtime.makeAFixed<JSArray>(
            runtime,
            prototypeHandle,
            classHandle,
            GCPointerBase::NoBarriers()));
  }

  auto shv = SmallHermesValue::encodeNumberValue(length, runtime);
  putLengthUnsafe(lv.self.get(), runtime, shv);

  return PseudoHandle<JSArray>::create(*lv.self);
}

CallResult<PseudoHandle<JSArray>> JSArray::createAndAllocPropStorage(
    Runtime &runtime,
    Handle<JSObject> prototypeHandle,
    Handle<HiddenClass> classHandle,
    size_type capacity,
    size_type length) {
  struct : Locals {
    PinnedValue<JSArray> arr;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  auto res = createNoAllocPropStorage(
      runtime, prototypeHandle, classHandle, capacity, length);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // Allocate property storage with size corresponding to number of properties
  // in the hidden class.
  lv.arr = std::move(*res);
  runtime.ignoreAllocationFailure(
      JSObject::allocatePropStorage(
          lv.arr, runtime, classHandle->getNumProperties()));

  return PseudoHandle<JSArray>::create(*lv.arr);
}

CallResult<PseudoHandle<JSArray>> JSArray::create(
    Runtime &runtime,
    Handle<JSObject> prototypeHandle,
    size_type capacity,
    size_type length) {
  return createNoAllocPropStorage(
      runtime,
      prototypeHandle,
      runtime.getHiddenClassForPrototype(
          *prototypeHandle, runtime.classJSArray),
      capacity,
      length);
}

CallResult<PseudoHandle<JSArray>>
JSArray::create(Runtime &runtime, size_type capacity, size_type length) {
  return JSArray::createNoAllocPropStorage(
      runtime, runtime.arrayPrototype, runtime.classJSArray, capacity, length);
}

CallResult<bool> JSArray::setLength(
    Handle<JSArray> selfHandle,
    Runtime &runtime,
    Handle<> newLength,
    PropOpFlags opFlags) {
  // About the truncation of double to uint32: in theory this produces UB,
  // however in practice it is well-defined.
  // Regardless of what happens in the conversion, 'ulen' can never compare
  // equal to 'd' unless 'd' is really an uint32 number, in which case the
  // conversion would have succeeded.
  // The only way this could fail is if the conversion throws an exception or
  // aborts the application, which is not the case on any platform we are
  // targeting.

  uint32_t ulen;
  double d;
  if (LLVM_LIKELY(newLength->isNumber())) {
    d = newLength->getNumber();
    ulen = _sh_tryfast_f64_to_u32_cvt(d);
  } else {
    // According to the spec, toNumber() has to be called twice.
    // https://tc39.es/ecma262/multipage/ordinary-and-exotic-objects-behaviours.html#sec-arraysetlength
    // There is a note that says:
    // "In steps 3 and 4, if Desc.[[Value]] is an object then its valueOf method
    // is called twice. This is legacy behaviour that was specified with this
    // effect starting with the 2nd Edition of this specification."
    auto res = toNumber_RJS(runtime, newLength);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    d = res->getNumber();
    ulen = _sh_tryfast_f64_to_u32_cvt(d);
    // If it is a string, no need to convert again, since it is pretty
    // expensive. Other types are not so important, since their conversions are
    // either fast (bool) or slow (object).
    // We could do the inverse check here - for object - but that is more risky,
    // since calling toNumber() twice is always correct, but calling it once
    // might be incorrect.
    if (!newLength->isString()) {
      res = toNumber_RJS(runtime, newLength);
      if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      d = res->getNumber();
    }
  }

  if (ulen != d)
    return runtime.raiseRangeError("Invalid array length");

  return setLength(selfHandle, runtime, ulen, opFlags);
}

CallResult<bool> JSArray::setLength(
    Handle<JSArray> selfHandle,
    Runtime &runtime,
    uint32_t newLength,
    PropOpFlags opFlags) {
  // Fast-path: if we are enlarging, do nothing.
  const auto currentLength = getLength(*selfHandle, runtime);
  if (LLVM_LIKELY(newLength >= currentLength)) {
    auto shv = SmallHermesValue::encodeNumberValue(newLength, runtime);
    // The caller must have already checked that the length property is
    // writable, so we can just write directly to it.
    putLengthUnsafe(*selfHandle, runtime, shv);
    return true;
  }

  struct : public Locals {
    PinnedValue<HiddenClass> clazz;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Length adjusted to the index of the highest non-deletable property + 1.
  // Nothing smaller than it can be deleted.
  uint32_t adjustedLength = newLength;

  // If we are sealed, we can't shrink past non-empty properties.
  if (LLVM_UNLIKELY(selfHandle->getClass(runtime)->isSealed())) {
    // We must scan backwards looking for a non-empty property. We only have
    // to scan in the intersection between the range of present values and
    // the range between the current length and the new length.
    //              newLength         currentLength
    //                 |                    |
    //                 +--------------------+
    //       begin                end
    //         |                   |
    //         +-------------------+
    //                 +-----------+
    //                 |           |
    //          lowestScanLen  highestLen
    //

    auto *self = selfHandle.get();
    auto range = _getOwnIndexedRangeImpl(self, runtime);
    uint32_t lowestScanLen = std::max(range.first, newLength);
    uint32_t highestLen = std::min(range.second, currentLength);

    for (; highestLen > lowestScanLen; --highestLen) {
      if (!self->unsafeAt(runtime, highestLen - 1).isEmpty()) {
        adjustedLength = highestLen;
        break;
      }
    }
  }

  if (LLVM_UNLIKELY(
          selfHandle->getClass(runtime)->getHasIndexLikeProperties())) {
    // Uh-oh. We are making the array smaller and we have index-like named
    // properties, so we may have to delete some of them: the ones greater or
    // equal to 'newLength'.

    // We iterate all named properties to find all index-like ones that need to
    // be deleted. At the same time we keep track of the highest index of a non-
    // deletable property - nothing smaller than that can be deleted because the
    // highest non-deletable would have terminated the deletion process.

    using IndexProp = std::pair<uint32_t, SymbolID>;
    llvh::SmallVector<IndexProp, 8> toBeDeleted;

    lv.clazz = selfHandle->getClass(runtime);
    HiddenClass::forEachProperty(
        lv.clazz,
        runtime,
        [&runtime, &adjustedLength, &toBeDeleted](
            SymbolID id, NamedPropertyDescriptor desc) {
          // If this property is not an integer index, or it doesn't need to be
          // deleted (it is less than 'adjustedLength'), ignore it.
          auto propNameAsIndex =
              runtime.getIdentifierTable().symbolToArrayIndex(id);
          if (!propNameAsIndex || *propNameAsIndex < adjustedLength)
            return;

          if (!desc.flags.configurable) {
            adjustedLength = *propNameAsIndex + 1;
          } else {
            toBeDeleted.push_back({*propNameAsIndex, id});
          }
        });

    // Scan the properties to be deleted in reverse order (to make deletion more
    // efficient) and delete those >= adjustedLength.
    for (auto it = toBeDeleted.rbegin(), e = toBeDeleted.rend(); it != e;
         ++it) {
      if (it->first >= adjustedLength) {
        auto cr = JSObject::deleteNamed(selfHandle, runtime, it->second);
        assert(
            cr != ExecutionStatus::EXCEPTION && *cr &&
            "Failed to delete a configurable property");
        (void)cr;
      }
    }
  }

  if (adjustedLength < selfHandle->getEndIndex()) {
    auto cr = setStorageEndIndex(selfHandle, runtime, adjustedLength);
    if (cr == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  auto shv = SmallHermesValue::encodeNumberValue(adjustedLength, runtime);
  putLengthUnsafe(*selfHandle, runtime, shv);

  if (adjustedLength != newLength) {
    if (opFlags.getThrowOnError()) {
      return runtime.raiseTypeError(
          TwineChar16("Cannot delete property '") + (adjustedLength - 1) + "'");
    }
    return false;
  }

  return true;
}

//===----------------------------------------------------------------------===//
// class JSArrayIterator

const ObjectVTable JSArrayIterator::vt{
    VTable(CellKind::JSArrayIteratorKind, cellSize<JSArrayIterator>()),
    JSArrayIterator::_getOwnIndexedRangeImpl,
    JSArrayIterator::_haveOwnIndexedImpl,
    JSArrayIterator::_getOwnIndexedPropertyFlagsImpl,
    JSArrayIterator::_getOwnIndexedImpl,
    JSArrayIterator::_setOwnIndexedImpl,
    JSArrayIterator::_deleteOwnIndexedImpl,
    JSArrayIterator::_checkAllOwnIndexedImpl,
};

void JSArrayIteratorBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<JSArrayIterator>());
  JSObjectBuildMeta(cell, mb);
  const auto *self = static_cast<const JSArrayIterator *>(cell);
  mb.setVTable(&JSArrayIterator::vt);
  mb.addField("iteratedObject", &self->iteratedObject_);
}

PseudoHandle<JSArrayIterator> JSArrayIterator::create(
    Runtime &runtime,
    Handle<JSObject> array,
    IterationKind iterationKind) {
  auto proto = Handle<JSObject>::vmcast(&runtime.arrayIteratorPrototype);
  auto clazz =
      runtime.getHiddenClassForPrototype(*proto, runtime.classJSArrayIterator);
  auto *obj = runtime.makeAFixed<JSArrayIterator>(
      runtime, proto, clazz, array, iterationKind);
  return JSObjectInit::initToPseudoHandle(runtime, obj);
}

/// Iterate to the next element and return.
CallResult<HermesValue> JSArrayIterator::nextElement(
    Handle<JSArrayIterator> self,
    Runtime &runtime) {
  if (!self->iteratedObject_) {
    // 5. If a is undefined, return CreateIterResultObject(undefined, true).
    return createIterResultObject(runtime, Runtime::getUndefinedValue(), true);
  }

  struct : Locals {
    PinnedValue<JSObject> a;
    PinnedValue<> index;
    PinnedValue<> value;
    PinnedValue<JSArray> arr;
    PinnedValue<> propValue;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // 4. Let a be the value of the [[IteratedObject]] internal slot of O.
  lv.a = self->iteratedObject_.getNonNull(runtime);
  // 6. Let index be the value of the [[ArrayIteratorNextIndex]] internal slot
  // of O.
  uint64_t index = self->nextIndex_;

  uint64_t len;
  if (vmisa<JSTypedArrayBase>(*lv.a)) {
    // 8. If a has a [[TypedArrayName]] internal slot, then
    // a. If IsDetachedBuffer(a.[[ViewedArrayBuffer]]) is true,
    //    throw a TypeError exception.
    // b. Let len be the value of O’s [[ArrayLength]] internal slot.
    auto ta = Handle<JSTypedArrayBase>::vmcast(&lv.a);
    if (LLVM_UNLIKELY(!ta->attached(runtime))) {
      return runtime.raiseTypeError("TypedArray detached during iteration");
    }
    len = ta->getLength();
  } else {
    // 9. Else,
    // a. Let len be ToLength(Get(a, "length")).
    auto propRes = JSObject::getNamed_RJS(
        lv.a, runtime, Predefined::getSymbolID(Predefined::length));
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.propValue = std::move(*propRes);
    auto lenRes = toLength(runtime, lv.propValue);
    if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    len = lenRes->getNumber();
  }

  if (index >= len) {
    // 10. If index ≥ len, then
    // a. Set the value of the [[IteratedObject]] internal slot of O to
    // undefined.
    self->iteratedObject_.setNull(runtime.getHeap());
    // b. Return CreateIterResultObject(undefined, true).
    return createIterResultObject(runtime, Runtime::getUndefinedValue(), true);
  }

  // 11. Set the value of the [[ArrayIteratorNextIndex]] internal slot of O to
  // index+1.
  ++self->nextIndex_;

  lv.index = HermesValue::encodeTrustedNumberValue(index);

  if (self->iterationKind_ == IterationKind::Key) {
    // 12. If itemKind is "key", return CreateIterResultObject(index, false).
    return createIterResultObject(runtime, lv.index, false);
  }

  // 13. Let elementKey be ToString(index).
  // 14. Let elementValue be Get(a, elementKey).
  CallResult<PseudoHandle<>> valueRes = getIndexed_RJS(runtime, lv.a, index);
  if (LLVM_UNLIKELY(valueRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.value = valueRes->getHermesValue();

  switch (self->iterationKind_) {
    case IterationKind::Key:
      llvm_unreachable("Early return already occurred in Key case");
    case IterationKind::Value:
      // 16. If itemKind is "value", let result be elementValue.
      return createIterResultObject(runtime, lv.value, false);
    case IterationKind::Entry: {
      // 17. b. Let result be CreateArrayFromList(«index, elementValue»).
      auto resultRes = JSArray::create(runtime, 2, 2);
      if (LLVM_UNLIKELY(resultRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.arr = std::move(*resultRes);
      if (LLVM_UNLIKELY(
              JSArray::setElementAt(lv.arr, runtime, 0, lv.index) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      if (LLVM_UNLIKELY(
              JSArray::setElementAt(lv.arr, runtime, 1, lv.value) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      // 18. Return CreateIterResultObject(result, false).
      return createIterResultObject(runtime, lv.arr, false);
    }
    case IterationKind::NumKinds:
      llvm_unreachable("Invalid iteration kind");
  }

  llvm_unreachable("Invalid iteration kind");
}

} // namespace vm
} // namespace hermes

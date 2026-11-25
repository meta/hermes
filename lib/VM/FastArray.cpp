/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/FastArray.h"

#include "hermes/VM/Runtime-inline.h"

#include "hermes/VM/BuildMetadata.h"

namespace hermes {
namespace vm {

void FastArrayBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<FastArray>());
  JSObjectBuildMeta(cell, mb);
  const auto *self = static_cast<const FastArray *>(cell);
  // This edge has to be called "elements" in order for Chrome to attribute
  // the size of the indexed storage as part of total usage of "JS Arrays".
  mb.addField("elements", &self->indexedStorage_);
  mb.setVTable(&FastArray::vt);
}

const ObjectVTable FastArray::vt{
    VTable(
        CellKind::FastArrayKind,
        cellSize<FastArray>(),
        /* allowLargeAlloc */ false,
        nullptr,
        nullptr,
        nullptr
#ifdef HERMES_MEMORY_INSTRUMENTATION
        ,
        VTable::HeapSnapshotMetadata{
            HeapSnapshot::NodeType::Object,
            nullptr,
            FastArray::_snapshotAddEdgesImpl,
            nullptr,
            nullptr}
#endif
        ),
    FastArray::_getOwnIndexedRangeImpl,
    FastArray::_haveOwnIndexedImpl,
    FastArray::_getOwnIndexedPropertyFlagsImpl,
    FastArray::_getOwnIndexedImpl,
    FastArray::_setOwnIndexedImpl,
    FastArray::_deleteOwnIndexedImpl,
    FastArray::_checkAllOwnIndexedImpl,
};

void FastArray::staticAsserts() {
  // Add the size of the length property when comparing sizes.
  static_assert(
      llvh::alignTo<sizeof(GCSmallHermesValue)>(sizeof(FastArray)) +
          sizeof(GCSmallHermesValue) ==
      sizeof(SHFastArray));
  static_assert(
      offsetof(FastArray, indexedStorage_) ==
      offsetof(SHFastArray, indexedStorage));
  llvm_unreachable("staticAsserts must never be called.");
}

PseudoHandle<HiddenClass> FastArray::createClass(
    Runtime &runtime,
    Handle<JSObject> prototypeHandle,
    Handle<HiddenClass> rootClazz) {
  struct : public Locals {
    PinnedValue<HiddenClass> clazz;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Add the length property.
  PropertyFlags pf{};
  pf.enumerable = 0;
  pf.writable = 0;
  pf.configurable = 0;

  auto added = HiddenClass::addProperty(
      rootClazz,
      runtime,
      Predefined::getSymbolID(Predefined::length),
      pf,
      MutableHandle<HiddenClass>(lv.clazz));
  assert(
      added != ExecutionStatus::EXCEPTION &&
      "Adding the first properties shouldn't cause overflow");
  assert(*added == lengthPropIndex() && "FastArray.length has invalid index");
  (void)added;

  return createPseudoHandle(*lv.clazz);
}

CallResult<HermesValue> FastArray::create(Runtime &runtime, size_t capacity) {
  struct : Locals {
    PinnedValue<FastArray> self;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.self = JSObjectInit::initToPointer(
      runtime,
      runtime.makeAFixed<FastArray>(
          runtime,
          runtime.fastArrayPrototype,
          runtime.classFastArray,
          GCPointerBase::NoBarriers()));

  auto arrRes = ArrayStorageSmall::create(runtime, capacity);
  if (arrRes == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;

  lv.self->indexedStorage_.setNonNull(
      runtime, vmcast<ArrayStorageSmall>(*arrRes), runtime.getHeap());

  auto shv = SmallHermesValue::encodeNumberValue(0, runtime);
  lv.self->setLength(runtime, shv);

  return lv.self.getHermesValue();
}

CallResult<HermesValue> FastArray::create(
    Runtime &runtime,
    Handle<JSObject> prototype,
    size_t capacity) {
  struct : Locals {
    PinnedValue<FastArray> self;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  auto classHandle =
      runtime.getHiddenClassForPrototype(*prototype, runtime.classFastArray);

  lv.self = JSObjectInit::initToPointer(
      runtime,
      runtime.makeAFixed<FastArray>(
          runtime, prototype, classHandle, GCPointerBase::NoBarriers()));

  auto arrRes = ArrayStorageSmall::create(runtime, capacity);
  if (arrRes == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;

  lv.self->indexedStorage_.setNonNull(
      runtime, vmcast<ArrayStorageSmall>(*arrRes), runtime.getHeap());

  auto shv = SmallHermesValue::encodeNumberValue(0, runtime);
  lv.self->setLength(runtime, shv);

  return lv.self.getHermesValue();
}

ExecutionStatus
FastArray::pushSlow(Handle<FastArray> self, Runtime &runtime, Handle<> val) {
  GCScopeMarkerRAII marker{runtime};
  UsePinnedValueRAII<ArrayStorageSmall> storage{runtime.hvStorageTmp};
  storage = self->indexedStorage_.getNonNull(runtime);
  if (LLVM_UNLIKELY(
          ArrayStorageSmall::push_back(storage, runtime, val) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;

  self->indexedStorage_.setNonNull(runtime, storage.get(), runtime.getHeap());
  auto newSz =
      SmallHermesValue::encodeNumberValue(storage.get()->size(), runtime);
  self->setLength(runtime, newSz);
  return ExecutionStatus::RETURNED;
}

ExecutionStatus FastArray::appendSlow(
    Handle<FastArray> self,
    Runtime &runtime,
    Handle<FastArray> other,
    uint32_t fromIndex) {
  struct : public Locals {
    PinnedValue<ArrayStorageSmall> storage;
    PinnedValue<ArrayStorageSmall> otherStorage;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  GCScopeMarkerRAII marker{runtime};
  lv.storage = self->indexedStorage_.getNonNull(runtime);
  lv.otherStorage = other->indexedStorage_.getNonNull(runtime);
  if (LLVM_UNLIKELY(
          ArrayStorageSmall::append(
              MutableHandle<ArrayStorageSmall>{lv.storage},
              runtime,
              lv.otherStorage,
              fromIndex) == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  self->indexedStorage_.setNonNull(
      runtime, lv.storage.get(), runtime.getHeap());
  auto newSz = SmallHermesValue::encodeNumberValue(lv.storage->size(), runtime);
  self->setLength(runtime, newSz);
  return ExecutionStatus::RETURNED;
}

#ifdef HERMES_MEMORY_INSTRUMENTATION
void FastArray::_snapshotAddEdgesImpl(
    GCCell *cell,
    GC &gc,
    HeapSnapshot &snap) {
  auto *const self = vmcast<FastArray>(cell);
  // Add the super type's edges too.
  JSObject::_snapshotAddEdgesImpl(self, gc, snap);
  auto *indexedStorage = self->indexedStorage_.getNonNull(gc.getPointerBase());
  if (!indexedStorage) {
    return;
  }

  // This edge has to be called "elements" in order for Chrome to attribute
  // the size of the indexed storage as part of total usage of "JS Arrays".
  snap.addNamedEdge(
      HeapSnapshot::EdgeType::Internal,
      "elements",
      gc.getObjectID(indexedStorage));
  for (uint32_t i = 0, e = self->getLengthAsUint32(gc.getPointerBase()); i < e;
       i++) {
    const auto &elem = indexedStorage->at(i);
    const llvh::Optional<HeapSnapshot::NodeID> elemID =
        gc.getSnapshotID(elem.toHV(gc.getPointerBase()));
    if (!elemID)
      continue;
    snap.addIndexedEdge(HeapSnapshot::EdgeType::Element, i, elemID.getValue());
  }
}
#endif

bool FastArray::_haveOwnIndexedImpl(
    JSObject *selfObj,
    Runtime &runtime,
    uint32_t index) {
  // Check whether the index is within the storage.
  return index < vmcast<FastArray>(selfObj)->getLengthAsUint32(runtime);
}

OptValue<PropertyFlags> FastArray::_getOwnIndexedPropertyFlagsImpl(
    JSObject *selfObj,
    Runtime &runtime,
    uint32_t index) {
  // Check whether the index is within the storage.
  if (index < vmcast<FastArray>(selfObj)->getLengthAsUint32(runtime)) {
    PropertyFlags pf{};
    pf.enumerable = 1;
    pf.writable = 1;
    return pf;
  }

  return llvh::None;
}

std::pair<uint32_t, uint32_t> FastArray::_getOwnIndexedRangeImpl(
    JSObject *selfObj,
    PointerBase &runtime) {
  return {0, vmcast<FastArray>(selfObj)->getLengthAsUint32(runtime)};
}

HermesValue FastArray::_getOwnIndexedImpl(
    PseudoHandle<JSObject> selfObj,
    Runtime &runtime,
    uint32_t index) {
  NoHandleScope noHandles{runtime};
  auto *self = vmcast<FastArray>(selfObj.get());
  auto *storage = self->indexedStorage_.getNonNull(runtime);
  if (index >= storage->size())
    return HermesValue::encodeEmptyValue();
  return storage->at(index).unboxToHV(runtime);
}

CallResult<bool> FastArray::_setOwnIndexedImpl(
    Handle<JSObject>,
    Runtime &runtime,
    uint32_t,
    Handle<>) {
  return runtime.raiseRangeError("FastArray is only writable in typed code");
}

bool FastArray::_deleteOwnIndexedImpl(Handle<JSObject>, Runtime &, uint32_t) {
  return false;
}

bool FastArray::_checkAllOwnIndexedImpl(
    JSObject *selfObj,
    Runtime &runtime,
    ObjectVTable::CheckAllOwnIndexedMode mode) {
  // FastArray elements are always non-configurable and non-writable.
  if (mode == ObjectVTable::CheckAllOwnIndexedMode::NonConfigurable)
    return true;
  assert(
      mode == ObjectVTable::CheckAllOwnIndexedMode::ReadOnly && "Unknown mode");
  return false;
}

} // namespace vm
} // namespace hermes

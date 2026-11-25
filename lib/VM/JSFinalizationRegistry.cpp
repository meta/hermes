/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JSFinalizationRegistry.h"
#include "hermes/VM/BuildMetadata.h"
#include "hermes/VM/Casting.h"
#include "hermes/VM/HermesValue.h"
#include "hermes/VM/JSFinalizationRegistry.h"
#include "hermes/VM/Runtime-inline.h"
#include "hermes/VM/Runtime.h"

namespace hermes {
namespace vm {

const VTable FinalizationRecord::vt{VTable(
    CellKind::FinalizationRecordKind,
    cellSize<FinalizationRecord>(),
    false,
    FinalizationRecord::_finalizeImpl,
    nullptr,
    nullptr
#ifdef HERMES_MEMORY_INSTRUMENTATION
    ,
    VTable::HeapSnapshotMetadata{
        HeapSnapshot::NodeType::Object,
        nullptr,
        FinalizationRecord::_snapshotAddEdgesImpl,
        nullptr,
        nullptr}
#endif
    )};

/* static */
void FinalizationRecord::_finalizeImpl(GCCell *cell, GC &) {
  auto *self = static_cast<FinalizationRecord *>(cell);
  self->target.releaseSlot();
  if (!self->unregisterToken.isEmpty())
    self->unregisterToken.releaseSlot();
}

void FinalizationRecordBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  const auto *self = static_cast<const FinalizationRecord *>(cell);
  mb.setVTable(&FinalizationRecord::vt);
  mb.addField("HeldValue", &self->heldValue);
}

#ifdef HERMES_MEMORY_INSTRUMENTATION
/* static */
void FinalizationRecord::_snapshotAddEdgesImpl(
    GCCell *cell,
    GC &gc,
    HeapSnapshot &snap) {
  auto *const self = reinterpret_cast<FinalizationRecord *>(cell);
  if (!self->target.isEmpty() && self->target.isValid()) {
    snap.addNamedEdge(
        HeapSnapshot::EdgeType::Weak, "Target", self->target.getNodeID(gc));
  }
  if (!self->unregisterToken.isEmpty() && self->unregisterToken.isValid()) {
    snap.addNamedEdge(
        HeapSnapshot::EdgeType::Weak,
        "UnregisterToken",
        self->unregisterToken.getNodeID(gc));
  }
}
#endif

/* static */
HermesValue FinalizationRecord::create(
    Runtime &runtime,
    Handle<> target,
    Handle<> heldValue,
    Handle<> unregisterToken) {
  struct : Locals {
    PinnedValue<SmallHermesValue> targetSHV;
    PinnedValue<SmallHermesValue> unregisterTokenSHV;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // This may allocate when Handle Sanitizer is ON and target is a Symbol.
  lv.targetSHV = SmallHermesValue::encodeHermesValue(*target, runtime);
  lv.unregisterTokenSHV =
      SmallHermesValue::encodeHermesValue(*unregisterToken, runtime);
  auto *record = runtime.makeAFixed<FinalizationRecord, HasFinalizer::Yes>(
      runtime, lv.targetSHV, heldValue, lv.unregisterTokenSHV);
  return HermesValue::encodeObjectValue(record);
}

const ObjectVTable JSFinalizationRegistry::vt{
    VTable(
        CellKind::JSFinalizationRegistryKind,
        cellSize<JSFinalizationRegistry>()),
    JSFinalizationRegistry::_getOwnIndexedRangeImpl,
    JSFinalizationRegistry::_haveOwnIndexedImpl,
    JSFinalizationRegistry::_getOwnIndexedPropertyFlagsImpl,
    JSFinalizationRegistry::_getOwnIndexedImpl,
    JSFinalizationRegistry::_setOwnIndexedImpl,
    JSFinalizationRegistry::_deleteOwnIndexedImpl,
    JSFinalizationRegistry::_checkAllOwnIndexedImpl,
};

void JSFinalizationRegistryBuildMeta(
    const GCCell *cell,
    Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(
      JSObject::numOverlapSlots<JSFinalizationRegistry>());
  JSObjectBuildMeta(cell, mb);
  const auto *self = static_cast<const JSFinalizationRegistry *>(cell);
  mb.setVTable(&JSFinalizationRegistry::vt);
  mb.addField("CleanupCallback", &self->cleanupCallback_);
  mb.addField("Cells", &self->cells_);
}

/* static */
PseudoHandle<JSFinalizationRegistry> JSFinalizationRegistry::create(
    Runtime &runtime,
    Handle<JSObject> parentHandle,
    Handle<Callable> cleanupCallback) {
  auto *cell = runtime.makeAFixed<JSFinalizationRegistry>(
      runtime,
      parentHandle,
      runtime.getHiddenClassForPrototype(
          *parentHandle, runtime.classJSFinalizationRegistry),
      cleanupCallback);
  return JSObjectInit::initToPseudoHandle(runtime, cell);
}

/* static */
ExecutionStatus JSFinalizationRegistry::cleanup(
    Handle<JSFinalizationRegistry> self,
    Runtime &runtime) {
  // If no cells to clean up, return.
  if (!self->cells_)
    return ExecutionStatus::RETURNED;

  struct : Locals {
    PinnedValue<Callable> cleanupCallback;
    PinnedValue<StorageType> cells;
    PinnedValue<FinalizationRecord> record;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.cleanupCallback = self->cleanupCallback_.getNonNull(runtime);
  lv.cells = self->cells_.getNonNull(runtime);
  // The size of lv.cells may change during the loop, since the cleanup callback
  // may register/unregister cells.
  for (size_t i = 0; i < lv.cells->size(); ++i) {
    auto element = lv.cells->at(i);
    // This element may be unregistered or cleaned up before.
    // Free slots are not objects (they contain a NativeUInt32 index).
    if (!element.isObject())
      continue;
    lv.record = vmcast<FinalizationRecord>(element);

    // ES16 9.12.a Find out dead targets to iterate on.
    if (lv.record->target.isValid())
      continue;

    // ES16 9.12.b Free this record since the target is dead.
    self->free(runtime, i);
    self->numUsedCells_ -= 1;

    // ES16 9.12.c Run the callback with the held value.
    auto result = Callable::executeCall1(
        lv.cleanupCallback,
        runtime,
        Runtime::getUndefinedValue(),
        lv.record->heldValue);

    if (result == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;

    // Re-read cells_ since the callback may have caused reallocation
    // (e.g., by calling register() which triggers ArrayStorage reallocation).
    lv.cells = self->cells_.getNonNull(runtime);
  }
  tryShrink(self, runtime);
  return ExecutionStatus::RETURNED;
}

/* static */
ExecutionStatus JSFinalizationRegistry::registerCell(
    Handle<JSFinalizationRegistry> self,
    Runtime &runtime,
    Handle<> target,
    Handle<> heldValue,
    Handle<> unregisterToken) {
  assert(
      canBeHeldWeakly(runtime, *target) &&
      "target must be Object or non-registered Symbol");
  assert(
      (unregisterToken->isUndefined() ||
       canBeHeldWeakly(runtime, *unregisterToken)) &&
      "unregisterToken must be Undefined, or Object/non-registered Symbol");
  assert(
      target->getRaw() != heldValue->getRaw() &&
      "heldValue can't be the same as target");
  if (!self->cells_) {
    // This should not fail unless OOM.
    auto cells = StorageType::create(runtime, kInitCapacity);
    if (LLVM_UNLIKELY(cells == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    self->cells_.setNonNull(
        runtime, vmcast<StorageType>(*cells), runtime.getHeap());
  }

  struct : Locals {
    PinnedValue<StorageType> cells;
    PinnedValue<> record;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  lv.cells = self->cells_.getNonNull(runtime);
  lv.record =
      FinalizationRecord::create(runtime, target, heldValue, unregisterToken);
  MutableHandle<StorageType> cellsHandle{lv.cells};
  self->numUsedCells_ += 1;
  if (auto freeIndex = self->useFreeIndex(runtime)) {
    // Use the free index.
    lv.cells->set(*freeIndex, *lv.record, runtime.getHeap());
    return ExecutionStatus::RETURNED;
  }
  // Add a new FinalizationRecord to the vector.
  if (LLVM_UNLIKELY(
          StorageType::push_back(cellsHandle, runtime, lv.record) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  // Write back the array pointer.
  self->cells_.setNonNull(runtime, *lv.cells, runtime.getHeap());
  return ExecutionStatus::RETURNED;
}

bool JSFinalizationRegistry::unregisterCells(
    Runtime &runtime,
    Handle<> unregisterToken) {
  // No target has been registered yet.
  if (!cells_)
    return false;
  assert(
      canBeHeldWeakly(runtime, *unregisterToken) &&
      "unregisterToken must be Object or non-registered Symbol");
  NoAllocScope noAlloc{runtime};
  // ES16 26.2.3.3
  // 4. Let removed be false.
  // 5. For each Record { [[WeakRefTarget]], [[HeldValue]], [[UnregisterToken]]
  //    } cell of finalizationRegistry.[[Cells]], do
  //    a. If cell.[[UnregisterToken]] is not empty and
  //       SameValue(cell.[[UnregisterToken]], unregisterToken) is true, then
  //       i. Remove cell from finalizationRegistry.[[Cells]].
  //       ii. Set removed to true.
  bool removed = false;
  StorageType &cellsRef = *cells_.get(runtime);
  for (size_t i = 0, e = cellsRef.size(); i < e; ++i) {
    HermesValue element = cellsRef.at(i);
    // This element may be unregistered or cleaned up before.
    // Free slots contain NativeUInt32 values.
    if (!element.isObject())
      continue;
    auto *record = vmcast<FinalizationRecord>(element);
    // Check if the unregisterToken on this record is Empty (not specified)
    // or invalid (freed since it's weakly held).
    if (record->unregisterToken.isEmpty() || !record->unregisterToken.isValid())
      continue;
    // Since unregister token can only be Object or non-registered Symbol,
    // we can directly compare raw bits.
    if (unregisterToken->getRaw() ==
        record->unregisterToken.getNoBarrierUnsafe(runtime).getRaw()) {
      free(runtime, i);
      --numUsedCells_;
      removed = true;
    }
  }
  return removed;
}

OptValue<JSFinalizationRegistry::StorageIndex>
JSFinalizationRegistry::useFreeIndex(Runtime &runtime) {
  if (!nextFree_)
    return llvh::None;
  StorageIndex current = *nextFree_;
  StorageType &cellsRef = *cells_.getNonNull(runtime);
  StorageIndex nextIndex =
      static_cast<StorageIndex>(cellsRef.at(current).getNativeUInt32());
  // End of list is marked by self-reference (value == index).
  if (nextIndex == current) {
    nextFree_ = llvh::None;
  } else {
    nextFree_ = nextIndex;
  }
  return current;
}

void JSFinalizationRegistry::free(Runtime &runtime, StorageIndex freedIndex) {
  StorageType &cellsRef = *cells_.getNonNull(runtime);
  // Store the current nextFree_ value at the freed slot.
  // If there's no previous free slot, store freedIndex itself (self-reference
  // marks end of list).
  StorageIndex valueToStore = nextFree_ ? *nextFree_ : freedIndex;
  cellsRef.set(
      freedIndex,
      HermesValue::encodeNativeUInt32(valueToStore),
      runtime.getHeap());
  // Update nextFree_ to point to this freed slot.
  nextFree_ = freedIndex;
}

/* static */
void JSFinalizationRegistry::tryShrink(
    Handle<JSFinalizationRegistry> self,
    Runtime &runtime) {
  StorageType *cells = self->cells_.getNonNull(runtime);
  auto arrSize = cells->size();
  if ((double)self->numUsedCells_ / arrSize >= kOccupancyRatio)
    return;

  StorageType::size_type newSize =
      std::max(self->numUsedCells_ * 2, kInitCapacity);
  // No need to shrink if the size is the same (this happens when size is still
  // kInitCapacity).
  if (newSize == arrSize)
    return;

  struct : Locals {
    PinnedValue<StorageType> oldCells;
    PinnedValue<StorageType> newCells;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.oldCells = cells;
  auto newCellsRes = StorageType::create(runtime, newSize);
  // If allocation fails, just skip shrinking, it's not required.
  if (LLVM_UNLIKELY(newCellsRes == ExecutionStatus::EXCEPTION)) {
    runtime.clearThrownValue();
    return;
  }
  lv.newCells = vmcast<StorageType>(*newCellsRes);

  // Copy alive elements from old cells to new cells.
  NoAllocScope noAlloc{runtime};
  [[maybe_unused]] StorageIndex destIndex = 0;
  for (StorageIndex i = 0, e = lv.oldCells->size(); i < e; ++i) {
    HermesValue element = lv.oldCells->at(i);
    if (element.isObject()) {
      lv.newCells->pushWithinCapacity(runtime, element);
      ++destIndex;
    }
  }

  assert(
      destIndex == self->numUsedCells_ &&
      "Must copy exactly numUsedCells_ elements");
  assert(destIndex < newSize && "Must have extra free entries in new array");

  // Update the registry to use the new cells array.
  self->cells_.setNonNull(runtime, *lv.newCells, runtime.getHeap());
  self->nextFree_ = llvh::None;
}

} // namespace vm
} // namespace hermes
